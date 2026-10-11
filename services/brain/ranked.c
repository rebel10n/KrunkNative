/*
 * services/brain/ranked.c - v3.1.1 ranked matchmaking (doc/backend.md 6.5).
 *
 * Client side (v3.1.1 index.js joinQueue/leaveQueue + ClientManager's
 * QueueManager): start a queue entry, then poll the matchmaker every second.
 * v3.1.1 starts it with io.send("queue", ...) on the game socket ("iq" reply);
 * KrunkNative's menu is not always connected to a game server, so the start is
 * the HTTP route /mm/queue/join (same three values back: clientId, clientKey,
 * status). Everything else is the v3.1.1 matchmaker API: /queue/poll,
 * /queue/update-ready, /queue/update-queues, /queue/update-class,
 * /queue/kick-party-member, /queue/promote-party-member (+ /queue/leave).
 *
 * State lives in PostgreSQL (0201_ranked.sql, plpgsql functions kn_rq_*) so
 * every brain serves the same queue. Each brain ticks every second; the one
 * that gets pg_try_advisory_xact_lock(kn_rq_lock()) forms matches (v3.1.1
 * config.queues r1v1/r2v2/r4v4 = 2 teams of 1/2/4, parties of up to
 * maxPartySize, rating window widening with wait time), hosts each on an idle
 * game server (brain_host_game_ex, "ranked" config) and moves the parties to
 * "Join". The game server reports "ranked-result" over the internal WS; the
 * brain applies Elo (eloK 32, team averages; abandon = -rankedDeduction and a
 * rankedCooldown queue ban) through svc-accounts' "elo" op.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "brain_internal.h"
#include "knsvc/config.h"
#include "knsvc/crypto.h"
#include "knsvc/db.h"
#include "knsvc/http.h"
#include "knsvc/json.h"
#include "knsvc/log.h"
#include "knsvc/loop.h"

#define MOD "ranked"
#define MAX_PARTY 4

/* v3.1.1 config.queues */
static const struct {
    const char *id, *key;
    int team_size;
    int elo_mode; /* accounts "elo" op mode */
} k_queues[] = {
    {"r1v1", "elo", 1, 1},
    {"r2v2", "elo2", 2, 2},
    {"r4v4", "elo4", 4, 4},
};
#define QUEUE_COUNT 3
static const char *const VALID_QUEUES = "{r1v1,r2v2,r4v4}";

static struct {
    Brain *b;
    int enabled;
    int min_level;
    int mode;
    const cJSON *maps;            /* brain.ranked.maps (borrowed), may be NULL */
    int poll_timeout_s, creating_timeout_s, match_timeout_s;
    int window, window_growth, window_max;
    double k;
    int abandon_penalty;
    int cooldown_s;
    int start_grace_s;
    kn_timer_id timer;
    int matching;                 /* matcher transaction in flight */
    int expiring;
    unsigned ticks;
} R;

static int queue_index(const char *id) {
    for (int i = 0; i < QUEUE_COUNT; i++) if (id && !strcmp(k_queues[i].id, id)) return i;
    return -1;
}

static int db_ready(void) {
    return R.b && R.b->db && R.enabled;
}

static void log_err(const char *what, kn_db_result *res) {
    KN_WARN(MOD, "%s failed: %s", what, kn_db_error(res));
}

static void on_ignore(kn_db_result *res, void *ud) {
    if (!kn_db_ok(res)) log_err(ud ? (const char *) ud : "query", res);
}

static void reply_error(kn_http_req *req, int status, const char *name) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "success", 0);
    cJSON_AddStringToObject(o, "error", name);
    brain_reply_json(req, status, o);
    cJSON_Delete(o);
}

/* ================================================================ HTTP: join */

typedef struct JoinCtx {
    kn_http_req *req;
    char region[64];
    char party[64];
    char queues[256];             /* postgres array literal */
    int ready;
    int class_index;
    char client_id[32];
    char client_key[64];
} JoinCtx;

static void reply_join(kn_http_req *req, const char *client_id, const char *client_key, const char *status,
                       int64_t cooldown_ms) {
    cJSON *o = cJSON_CreateObject();
    if (client_id) cJSON_AddStringToObject(o, "clientId", client_id);
    else cJSON_AddNullToObject(o, "clientId");
    if (client_key) cJSON_AddStringToObject(o, "clientKey", client_key);
    else cJSON_AddNullToObject(o, "clientKey");
    if (status) cJSON_AddStringToObject(o, "status", status);
    else cJSON_AddNullToObject(o, "status");
    if (cooldown_ms > 0) cJSON_AddNumberToObject(o, "cooldownMs", (double) cooldown_ms);
    brain_reply_json(req, 200, o);
    cJSON_Delete(o);
}

static void on_joined(kn_db_result *res, void *ud) {
    JoinCtx *j = ud;
    if (!kn_db_ok(res) || kn_db_rows(res) < 1) {
        log_err("queue join", res);
        reply_error(j->req, 500, "internal");
        free(j);
        return;
    }
    const char *txt = kn_db_get(res, 0, 0);
    cJSON *r = kn_json_parse(txt, strlen(txt));
    const char *status = kn_json_str(r, "status", NULL);
    if (status) reply_join(j->req, NULL, NULL, status, kn_json_int(r, "cooldownMs", 0));
    else {
        KN_INFO(MOD, "client %s queued in party %s (%s)", j->client_id, kn_json_str(r, "partyId", "?"), j->region);
        reply_join(j->req, j->client_id, j->client_key, NULL, 0);
    }
    cJSON_Delete(r);
    free(j);
}

static void on_join_account(void *ud, const char *err, const cJSON *acc) {
    JoinCtx *j = ud;
    int64_t id = acc ? kn_json_int(acc, "id", 0) : 0;
    if (err || id <= 0) {
        reply_join(j->req, NULL, NULL, "LoginRequired", 0);
        free(j);
        return;
    }
    if (kn_json_int(acc, "level", 0) < R.min_level) {
        reply_join(j->req, NULL, NULL, "LevelTooLow", 0);
        free(j);
        return;
    }
    const char *name = kn_json_str(acc, "username", kn_json_str(acc, "name", "?"));
    cJSON *data = cJSON_CreateObject();
    const cJSON *clan = cJSON_GetObjectItemCaseSensitive(acc, "clan");
    cJSON_AddItemToObject(data, "c", clan ? cJSON_Duplicate(clan, 1) : cJSON_CreateNull());
    const cJSON *cr = cJSON_GetObjectItemCaseSensitive(acc, "clan_rank");
    cJSON_AddItemToObject(data, "cR", cr ? cJSON_Duplicate(cr, 1) : cJSON_CreateNull());
    for (int i = 0; i < QUEUE_COUNT; i++)
        cJSON_AddNumberToObject(data, k_queues[i].key, (double) kn_json_int(acc, k_queues[i].key, 0));
    cJSON_AddNumberToObject(data, "level", (double) kn_json_int(acc, "level", 0));
    char *data_s = kn_json_print(data);
    cJSON_Delete(data);
    char acc_s[32], cls[16], new_party[16], max_party[8];
    snprintf(acc_s, sizeof(acc_s), "%lld", (long long) id);
    snprintf(cls, sizeof(cls), "%d", j->class_index);
    snprintf(max_party, sizeof(max_party), "%d", MAX_PARTY);
    {
        static const char cs[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
        unsigned char rnd[6];
        kn_random_bytes(rnd, sizeof(rnd));
        for (int i = 0; i < 6; i++) new_party[i] = cs[rnd[i] % (sizeof(cs) - 1)];
        new_party[6] = 0;
    }
    kn_db_query(R.b->db,
                "SELECT kn_rq_join($1::bigint, $2::text, $3::jsonb, $4::text, $5::text[], $6::text,"
                " $7::boolean, $8::integer, $9::text, $10::text, $11::text, $12::integer)::text",
                KN_DB_ARGS(acc_s, name, data_s, j->region, j->queues, j->party, j->ready ? "true" : "false", cls,
                           j->client_id, j->client_key, new_party, max_party),
                on_joined, j);
    free(data_s);
}

/* JSON array of queue ids -> postgres array literal of the valid ones */
static void queues_literal(const char *json, char *out, size_t cap) {
    cJSON *arr = json && *json ? kn_json_parse(json, strlen(json)) : NULL;
    size_t n = 0;
    out[n++] = '{';
    int first = 1;
    const cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        if (!cJSON_IsString(it) || queue_index(it->valuestring) < 0) continue;
        n += (size_t) snprintf(out + n, cap - n, "%s%s", first ? "" : ",", it->valuestring);
        first = 0;
        if (n >= cap - 2) break;
    }
    if (n < cap - 1) out[n++] = '}';
    out[n] = 0;
    cJSON_Delete(arr);
}

static void h_join(kn_http_req *req, void *ud) {
    (void) ud;
    if (!db_ready()) {
        reply_error(req, 503, "RankedUnavailable");
        return;
    }
    if (R.b->draining) {
        reply_error(req, 503, "updating");
        return;
    }
    JoinCtx *j = calloc(1, sizeof(*j));
    if (!j) {
        reply_error(req, 500, "internal");
        return;
    }
    j->req = req;
    char token[512] = "", tmp[512] = "";
    if (kn_http_query_get(req, "token", token, sizeof(token)) <= 0) {
        const char *bearer = kn_http_bearer(req);
        if (bearer) snprintf(token, sizeof(token), "%s", bearer);
    }
    if (kn_http_query_get(req, "region", j->region, sizeof(j->region)) <= 0)
        snprintf(j->region, sizeof(j->region), "%s", R.b->region);
    kn_http_query_get(req, "party", j->party, sizeof(j->party));
    tmp[0] = 0;
    kn_http_query_get(req, "queues", tmp, sizeof(tmp));
    queues_literal(tmp, j->queues, sizeof(j->queues));
    tmp[0] = 0;
    kn_http_query_get(req, "ready", tmp, sizeof(tmp));
    j->ready = !strcmp(tmp, "1") || !strcasecmp(tmp, "true");
    tmp[0] = 0;
    kn_http_query_get(req, "classIndex", tmp, sizeof(tmp));
    j->class_index = atoi(tmp);
    kn_random_hex(j->client_id, sizeof(j->client_id), 8);
    kn_random_token(j->client_key, sizeof(j->client_key), 24);
    if (!token[0]) {
        reply_join(req, NULL, NULL, "LoginRequired", 0);
        free(j);
        return;
    }
    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "token", token);
    accounts_internal_call(R.b, "token", args, on_join_account, j);
    cJSON_Delete(args);
}

/* ================================================================ HTTP: poll + actions */

static void on_state(kn_db_result *res, void *ud) {
    kn_http_req *req = ud;
    if (!kn_db_ok(res)) {
        log_err("queue state", res);
        reply_error(req, 500, "internal");
        return;
    }
    if (kn_db_rows(res) < 1 || kn_db_isnull(res, 0, 0)) {
        reply_error(req, 404, "InvalidClientKey");
        return;
    }
    kn_http_set_header(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    kn_http_respond_json_str(req, 200, kn_db_get(res, 0, 0));
}

static int get_key(kn_http_req *req, char *key, size_t cap) {
    if (!db_ready()) {
        reply_error(req, 503, "RankedUnavailable");
        return -1;
    }
    if (kn_http_query_get(req, "clientKey", key, cap) <= 0) {
        reply_error(req, 404, "InvalidClientKey");
        return -1;
    }
    return 0;
}

static void h_poll(kn_http_req *req, void *ud) {
    (void) ud;
    char key[96];
    if (get_key(req, key, sizeof(key))) return;
    kn_db_query(R.b->db, "SELECT kn_rq_state($1::text)::text", KN_DB_ARGS(key), on_state, req);
}

static void action(kn_http_req *req, const char *name, const char *param) {
    char key[96], arg[512] = "";
    if (get_key(req, key, sizeof(key))) return;
    if (param && kn_http_query_get(req, param, arg, sizeof(arg)) < 0) arg[0] = 0;
    kn_db_query(R.b->db, "SELECT kn_rq_action($1::text, $2::text, $3::text, $4::text[])::text",
                KN_DB_ARGS(key, name, arg, VALID_QUEUES), on_state, req);
}

static void h_ready(kn_http_req *req, void *ud) { (void) ud; action(req, "ready", "isReady"); }
static void h_queues(kn_http_req *req, void *ud) { (void) ud; action(req, "queues", "queues"); }
static void h_class(kn_http_req *req, void *ud) { (void) ud; action(req, "class", "classIndex"); }
static void h_kick(kn_http_req *req, void *ud) { (void) ud; action(req, "kick", "kickClientId"); }
static void h_promote(kn_http_req *req, void *ud) { (void) ud; action(req, "promote", "promoteClientId"); }
static void h_leave(kn_http_req *req, void *ud) { (void) ud; action(req, "leave", NULL); }

/* ================================================================ matcher */

typedef struct Party {
    char id[64];
    char region[64];
    unsigned queues;              /* bit per k_queues index */
    int size;
    double rating[QUEUE_COUNT];   /* member average per queue key */
    double wait_s;
    const cJSON *members;         /* [{a, n, c, elo, elo2, elo4}] (borrowed from cand) */
    int used;
} Party;

typedef struct Match {
    char match_id[40];
    int queue;
    char region[64];
    cJSON *teams_parties;         /* [[party ids], [party ids]] */
    cJSON *teams_players;         /* [[{accountId,name}], [...]] */
    int created;
    struct Match *next;
} Match;

typedef struct MatchRun {
    kn_db_tx *tx;
    cJSON *cand;                  /* owns the candidate rows */
    Match *matches;
} MatchRun;

static void match_free(Match *m) {
    cJSON_Delete(m->teams_parties);
    cJSON_Delete(m->teams_players);
    free(m);
}

static void run_free(MatchRun *run) {
    while (run->matches) {
        Match *n = run->matches->next;
        match_free(run->matches);
        run->matches = n;
    }
    cJSON_Delete(run->cand);
    free(run);
    R.matching = 0;
}

static void parse_parties(const cJSON *cand, Party **out, int *count) {
    int n = cJSON_GetArraySize(cand);
    Party *ps = calloc((size_t) (n > 0 ? n : 1), sizeof(Party));
    int c = 0;
    const cJSON *row;
    cJSON_ArrayForEach(row, cand) {
        Party *p = &ps[c];
        snprintf(p->id, sizeof(p->id), "%s", kn_json_str(row, "party_id", ""));
        snprintf(p->region, sizeof(p->region), "%s", kn_json_str(row, "region", ""));
        const cJSON *q;
        cJSON_ArrayForEach(q, cJSON_GetObjectItemCaseSensitive(row, "queues")) {
            int qi = cJSON_IsString(q) ? queue_index(q->valuestring) : -1;
            if (qi >= 0) p->queues |= 1u << qi;
        }
        p->wait_s = kn_json_num(row, "wait_s", 0);
        p->members = cJSON_GetObjectItemCaseSensitive(row, "members");
        p->size = cJSON_GetArraySize(p->members);
        const cJSON *m;
        cJSON_ArrayForEach(m, p->members) {
            for (int i = 0; i < QUEUE_COUNT; i++) p->rating[i] += kn_json_num(m, k_queues[i].key, 0);
        }
        for (int i = 0; i < QUEUE_COUNT && p->size > 0; i++) p->rating[i] /= p->size;
        if (p->id[0] && p->size > 0) c++;
    }
    *out = ps;
    *count = c;
}

static int by_wait_desc(const void *a, const void *b) {
    const Party *x = *(Party *const *) a, *y = *(Party *const *) b;
    return x->wait_s < y->wait_s ? 1 : x->wait_s > y->wait_s ? -1 : 0;
}

static void add_team_party(Match *m, int team, const Party *p) {
    cJSON *tp = cJSON_GetArrayItem(m->teams_parties, team);
    cJSON_AddItemToArray(tp, cJSON_CreateString(p->id));
    cJSON *pl = cJSON_GetArrayItem(m->teams_players, team);
    const cJSON *mem;
    cJSON_ArrayForEach(mem, p->members) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "accountId", kn_json_num(mem, "a", 0));
        cJSON_AddStringToObject(o, "name", kn_json_str(mem, "n", ""));
        cJSON_AddItemToArray(pl, o);
    }
}

/* v3.1.1-style queue: oldest ready party anchors a match; others within the
 * rating window (widening with the anchor's wait) fill two teams of n. */
static Match *form_matches(Party *ps, int count) {
    Match *head = NULL;
    Party **order = calloc((size_t) (count > 0 ? count : 1), sizeof(Party *));
    for (int i = 0; i < count; i++) order[i] = &ps[i];
    qsort(order, (size_t) count, sizeof(Party *), by_wait_desc);
    /* larger queues first so 4v4 parties are not starved by 1v1 pairs */
    for (int q = QUEUE_COUNT - 1; q >= 0; q--) {
        const int n = k_queues[q].team_size;
        for (int a = 0; a < count; a++) {
            Party *anchor = order[a];
            if (anchor->used || !(anchor->queues & (1u << q)) || anchor->size > n) continue;
            double window = R.window + R.window_growth * anchor->wait_s;
            if (window > R.window_max) window = R.window_max;
            Party *team[2][MAX_PARTY * 2];
            int tn[2] = {0, 0}, free_slots[2] = {n - anchor->size, n};
            team[0][tn[0]++] = anchor;
            for (int b = 0; b < count && (free_slots[0] || free_slots[1]); b++) {
                Party *p = order[b];
                if (p == anchor || p->used || !(p->queues & (1u << q)) || p->size > n) continue;
                if (strcmp(p->region, anchor->region)) continue;
                if (fabs(p->rating[q] - anchor->rating[q]) > window) continue;
                int t = p->size <= free_slots[0] ? 0 : p->size <= free_slots[1] ? 1 : -1;
                if (t < 0) continue;
                team[t][tn[t]++] = p;
                free_slots[t] -= p->size;
            }
            if (free_slots[0] || free_slots[1]) continue;
            Match *m = calloc(1, sizeof(*m));
            if (!m) break;
            char hex[33];
            kn_random_hex(hex, sizeof(hex), 16);
            snprintf(m->match_id, sizeof(m->match_id), "%.8s-%.4s-%.4s-%.4s-%.12s", hex, hex + 8, hex + 12, hex + 16,
                     hex + 20);
            m->queue = q;
            snprintf(m->region, sizeof(m->region), "%s", anchor->region);
            m->teams_parties = cJSON_CreateArray();
            m->teams_players = cJSON_CreateArray();
            for (int t = 0; t < 2; t++) {
                cJSON_AddItemToArray(m->teams_parties, cJSON_CreateArray());
                cJSON_AddItemToArray(m->teams_players, cJSON_CreateArray());
                for (int i = 0; i < tn[t]; i++) {
                    team[t][i]->used = 1;
                    add_team_party(m, t, team[t][i]);
                }
            }
            m->next = head;
            head = m;
        }
    }
    free(order);
    return head;
}

/* ---- hosting a formed match */

typedef struct HostMatch {
    char match_id[40];
    int queue;
    char region[64];
    cJSON *players;               /* teams_players (owned) */
} HostMatch;

static void on_ranked_hosted(void *ud, const char *err, const cJSON *reply) {
    HostMatch *h = ud;
    Brain *b = R.b;
    if (err || !reply || !b->db) {
        KN_WARN(MOD, "match %s: hosting failed (%s); parties back in the queue", h->match_id, err ? err : "?");
        if (b->db)
            kn_db_query(b->db, "SELECT kn_rq_match_failed($1::text, $2::text)",
                        KN_DB_ARGS(h->match_id, err ? err : "host failed"), on_ignore, "match failed");
    } else {
        const char *gid = kn_json_str(reply, "gameId", "");
        char node_id[MM_ID_MAX];
        snprintf(node_id, sizeof(node_id), "%.*s", (int) strcspn(gid, ":"), gid);
        MmNode *n = registry_node(b->registry, node_id);
        cJSON *tickets = cJSON_CreateObject();
        const cJSON *team, *pl;
        cJSON_ArrayForEach(team, h->players) {
            cJSON_ArrayForEach(pl, team) {
                int64_t acc = (int64_t) kn_json_num(pl, "accountId", 0);
                cJSON *seek = n ? brain_seek_reply_ex(b, n, gid, acc, kn_json_str(pl, "name", ""), 0, 0) : NULL;
                char key[32];
                snprintf(key, sizeof(key), "%lld", (long long) acc);
                if (seek) cJSON_AddStringToObject(tickets, key, kn_json_str(seek, "clientId", ""));
                cJSON_Delete(seek);
            }
        }
        cJSON *c = cJSON_CreateObject();
        cJSON_AddStringToObject(c, "gameId", gid);
        cJSON_AddStringToObject(c, "host", kn_json_str(reply, "host", ""));
        cJSON_AddNumberToObject(c, "port", kn_json_num(reply, "port", 0));
        const char *web = kn_json_str(reply, "webUrl", NULL);
        if (web) cJSON_AddStringToObject(c, "webUrl", web);
        else cJSON_AddNullToObject(c, "webUrl");
        cJSON_AddStringToObject(c, "matchId", h->match_id);
        char *cs = kn_json_print(c), *ts = kn_json_print(tickets);
        kn_db_query(b->db, "SELECT kn_rq_match_ready($1::text, $2::text, $3::jsonb, $4::jsonb)",
                    KN_DB_ARGS(h->match_id, gid, cs, ts), on_ignore, "match ready");
        KN_INFO(MOD, "match %s (%s, %s) is live on %s", h->match_id, k_queues[h->queue].id, h->region, gid);
        free(cs);
        free(ts);
        cJSON_Delete(c);
        cJSON_Delete(tickets);
    }
    cJSON_Delete(h->players);
    free(h);
}

static void host_match(const Match *m) {
    HostMatch *h = calloc(1, sizeof(*h));
    if (!h) return;
    snprintf(h->match_id, sizeof(h->match_id), "%s", m->match_id);
    snprintf(h->region, sizeof(h->region), "%s", m->region);
    h->queue = m->queue;
    h->players = cJSON_Duplicate(m->teams_players, 1);
    const int n = k_queues[m->queue].team_size;
    cJSON *cfg = cJSON_CreateObject();
    cJSON_AddBoolToObject(cfg, "ranked", 1);
    cJSON_AddStringToObject(cfg, "matchId", m->match_id);
    cJSON_AddStringToObject(cfg, "queue", k_queues[m->queue].id);
    cJSON_AddNumberToObject(cfg, "teamSize", n);
    cJSON_AddNumberToObject(cfg, "maxPlayers", 2 * n);
    cJSON *modes = cJSON_AddArrayToObject(cfg, "modes");
    cJSON_AddItemToArray(modes, cJSON_CreateNumber(R.mode));
    cJSON_AddNumberToObject(cfg, "mode", R.mode);
    int nmaps = cJSON_GetArraySize(R.maps);
    if (nmaps > 0) {
        const cJSON *pick = cJSON_GetArrayItem(R.maps, (int) (kn_random_u64() % (uint64_t) nmaps));
        cJSON *maps = cJSON_AddArrayToObject(cfg, "maps");
        cJSON_AddItemToArray(maps, cJSON_Duplicate(pick, 1));
        if (cJSON_IsString(pick)) cJSON_AddStringToObject(cfg, "map", pick->valuestring);
    }
    cJSON_AddBoolToObject(cfg, "private", 1);
    cJSON_AddNumberToObject(cfg, "startGraceS", R.start_grace_s);
    cJSON_AddItemToObject(cfg, "teams", cJSON_Duplicate(m->teams_players, 1));
    KN_INFO(MOD, "match %s formed (%s, %s): hosting", m->match_id, k_queues[m->queue].id, m->region);
    brain_host_game_ex(R.b, m->region, cfg, 0, NULL, 0, on_ranked_hosted, h);
    cJSON_Delete(cfg);
}

/* ---- matcher transaction */

static void on_match_commit(kn_db_result *res, void *ud) {
    MatchRun *run = ud;
    if (!kn_db_ok(res)) log_err("matcher commit", res);
    else
        for (Match *m = run->matches; m; m = m->next) if (m->created) host_match(m);
    run_free(run);
}

typedef struct CreateCtx {
    MatchRun *run;
    Match *m;
} CreateCtx;

static void on_match_created(kn_db_result *res, void *ud) {
    CreateCtx *c = ud;
    if (!kn_db_ok(res)) log_err("match create", res);
    else c->m->created = kn_db_get_bool(res, 0, 0, 0);
    free(c);
}

static void on_candidates(kn_db_result *res, void *ud) {
    MatchRun *run = ud;
    if (!kn_db_ok(res)) {
        log_err("matcher candidates", res);
        kn_db_tx_rollback(run->tx, NULL, NULL);
        run_free(run);
        return;
    }
    run->cand = kn_db_result_to_json(res);
    Party *ps = NULL;
    int count = 0;
    parse_parties(run->cand, &ps, &count);
    run->matches = form_matches(ps, count);
    free(ps);
    if (!run->matches) {
        kn_db_tx_rollback(run->tx, NULL, NULL);
        run_free(run);
        return;
    }
    for (Match *m = run->matches; m; m = m->next) {
        CreateCtx *c = calloc(1, sizeof(*c));
        if (!c) continue;
        c->run = run;
        c->m = m;
        char *teams = kn_json_print(m->teams_parties);
        kn_db_tx_query(run->tx, "SELECT kn_rq_match_create($1::text, $2::text, $3::text, $4::text, $5::jsonb)",
                       KN_DB_ARGS(m->match_id, k_queues[m->queue].id, m->region, R.b->id, teams), on_match_created,
                       c);
        free(teams);
    }
    kn_db_tx_commit(run->tx, on_match_commit, run);
}

static void on_lock(kn_db_result *res, void *ud) {
    MatchRun *run = ud;
    if (!kn_db_ok(res) || !kn_db_get_bool(res, 0, 0, 0)) {
        if (!kn_db_ok(res)) log_err("matcher lock", res);
        kn_db_tx_rollback(run->tx, NULL, NULL);
        run_free(run);
        return;
    }
    kn_db_tx_query(run->tx,
                   "SELECT party_id, region, to_jsonb(queue_ids) AS queues, wait_s, members"
                   " FROM mm_ranked_candidates WHERE $1::text IS NOT NULL",
                   KN_DB_ARGS(R.b->id), on_candidates, run);
}

static void on_begin(kn_db_tx *tx, kn_db_result *res, void *ud) {
    MatchRun *run = ud;
    if (!tx) {
        log_err("matcher begin", res);
        run_free(run);
        return;
    }
    run->tx = tx;
    kn_db_tx_query(tx, "SELECT pg_try_advisory_xact_lock(kn_rq_lock())", 0, NULL, on_lock, run);
}

static void on_expired(kn_db_result *res, void *ud) {
    (void) ud;
    R.expiring = 0;
    if (!kn_db_ok(res)) log_err("queue expire", res);
}

static void tick(kn_loop *loop, void *ud) {
    (void) loop;
    (void) ud;
    if (!db_ready() || R.b->draining) return;
    R.ticks++;
    if (!R.expiring && R.ticks % 2 == 0) {
        char poll[16], creating[16], match[16];
        snprintf(poll, sizeof(poll), "%d", R.poll_timeout_s);
        snprintf(creating, sizeof(creating), "%d", R.creating_timeout_s);
        snprintf(match, sizeof(match), "%d", R.match_timeout_s);
        R.expiring = 1;
        kn_db_query(R.b->db, "SELECT kn_rq_expire($1::integer, $2::integer, $3::integer)",
                    KN_DB_ARGS(poll, creating, match), on_expired, NULL);
    }
    if (R.matching) return;
    MatchRun *run = calloc(1, sizeof(*run));
    if (!run) return;
    R.matching = 1;
    if (kn_db_begin(R.b->db, on_begin, run)) run_free(run);
}

/* ================================================================ results */

typedef struct ResultCtx {
    RankedResultCb cb;
    void *ud;
    char match_id[64];
    cJSON *frame;
    int pending;
    cJSON *ratings;
} ResultCtx;

typedef struct EloCtx {
    ResultCtx *r;
    cJSON *entry;                 /* borrowed: element of r->ratings */
} EloCtx;

static void result_done(ResultCtx *r) {
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddBoolToObject(reply, "ok", 1);
    cJSON_AddItemToObject(reply, "ratings", r->ratings);
    /* store the new ratings with the match */
    if (R.b->db) {
        char *s = kn_json_print(r->ratings);
        kn_db_query(R.b->db,
                    "UPDATE mm_ranked_match_players mp SET rating_after = (x ->> 'new')::integer"
                    "  FROM jsonb_array_elements($2::jsonb) x"
                    " WHERE mp.match_id = $1::text AND mp.account_id = (x ->> 'accountId')::bigint",
                    KN_DB_ARGS(r->match_id, s ? s : "[]"), on_ignore, "match ratings");
        free(s);
    }
    if (r->cb) r->cb(r->ud, NULL, reply);
    cJSON_Delete(reply);
    cJSON_Delete(r->frame);
    free(r);
}

static void on_elo(void *ud, const char *err, const cJSON *data) {
    EloCtx *e = ud;
    ResultCtx *r = e->r;
    if (err) {
        cJSON_AddStringToObject(e->entry, "err", err);
        KN_WARN(MOD, "rating update for account %.0f failed: %s", kn_json_num(e->entry, "accountId", 0), err);
    } else if (data && cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(data, "elo"))) {
        cJSON_ReplaceItemInObject(e->entry, "new", cJSON_CreateNumber(kn_json_num(data, "elo", 0)));
    }
    free(e);
    if (--r->pending == 0) result_done(r);
}

static int abandoned(const cJSON *frame, int64_t acc) {
    const cJSON *a;
    cJSON_ArrayForEach(a, cJSON_GetObjectItemCaseSensitive(frame, "abandoned")) {
        if (cJSON_IsNumber(a) && (int64_t) a->valuedouble == acc) return 1;
    }
    return 0;
}

static void on_finished(kn_db_result *res, void *ud) {
    ResultCtx *r = ud;
    if (!kn_db_ok(res) || kn_db_rows(res) < 1) {
        log_err("match finish", res);
        if (r->cb) r->cb(r->ud, "db error", NULL);
        cJSON_Delete(r->frame);
        free(r);
        return;
    }
    const char *txt = kn_db_get(res, 0, 0);
    cJSON *players = kn_json_parse(txt, strlen(txt));
    r->ratings = cJSON_CreateArray();
    if (cJSON_GetArraySize(players) == 0) {
        KN_INFO(MOD, "match %s: result ignored (unknown or already closed)", r->match_id);
        cJSON_Delete(players);
        result_done(r);
        return;
    }
    /* Elo on team averages (v3.1.1 config.eloK) */
    double sum[2] = {0, 0};
    int cnt[2] = {0, 0};
    const cJSON *p;
    cJSON_ArrayForEach(p, players) {
        int t = (int) kn_json_int(p, "team", 0) ? 1 : 0;
        sum[t] += kn_json_num(p, "before", 0);
        cnt[t]++;
    }
    double avg[2] = {cnt[0] ? sum[0] / cnt[0] : 0, cnt[1] ? sum[1] / cnt[1] : 0};
    int winner = (int) kn_json_int(r->frame, "winner", -1);
    /* game results per player can override the team (e.g. a player switched) */
    r->pending = 1;
    cJSON_ArrayForEach(p, players) {
        int64_t acc = kn_json_int(p, "accountId", 0);
        int t = (int) kn_json_int(p, "team", 0) ? 1 : 0;
        const char *key = kn_json_str(p, "key", "elo");
        int before = (int) kn_json_int(p, "before", 0);
        double expect = 1.0 / (1.0 + pow(10.0, (avg[1 - t] - avg[t]) / 400.0));
        double score = winner < 0 || winner > 1 ? 0.5 : winner == t ? 1.0 : 0.0;
        int delta = (int) lround(R.k * (score - expect));
        int quit = abandoned(r->frame, acc);
        if (quit) delta = delta < -R.abandon_penalty ? delta : -R.abandon_penalty;
        if (before + delta < 0) delta = -before;
        int mode = 1;
        for (int i = 0; i < QUEUE_COUNT; i++) if (!strcmp(k_queues[i].key, key)) mode = k_queues[i].elo_mode;
        cJSON *e = cJSON_CreateObject();
        cJSON_AddNumberToObject(e, "accountId", (double) acc);
        cJSON_AddStringToObject(e, "name", kn_json_str(p, "name", ""));
        cJSON_AddNumberToObject(e, "team", t);
        cJSON_AddStringToObject(e, "key", key);
        cJSON_AddNumberToObject(e, "old", before);
        cJSON_AddNumberToObject(e, "new", before + delta);
        cJSON_AddNumberToObject(e, "delta", delta);
        if (quit) cJSON_AddBoolToObject(e, "abandoned", 1);
        cJSON_AddItemToArray(r->ratings, e);
        if (quit && R.b->db) {
            char a[32], secs[16];
            snprintf(a, sizeof(a), "%lld", (long long) acc);
            snprintf(secs, sizeof(secs), "%d", R.cooldown_s);
            kn_db_query(R.b->db,
                        "INSERT INTO mm_ranked_bans (account_id, until, reason)"
                        " VALUES ($1::bigint, now() + make_interval(secs => $2::integer), 'abandon')"
                        " ON CONFLICT (account_id) DO UPDATE SET until = EXCLUDED.until, reason = EXCLUDED.reason",
                        KN_DB_ARGS(a, secs), on_ignore, "ranked ban");
        }
        if (delta != 0 && acc > 0) {
            EloCtx *ec = calloc(1, sizeof(*ec));
            if (!ec) continue;
            ec->r = r;
            ec->entry = e;
            r->pending++;
            cJSON *args = cJSON_CreateObject();
            cJSON_AddNumberToObject(args, "id", (double) acc);
            cJSON_AddNumberToObject(args, "mode", mode);
            cJSON_AddNumberToObject(args, "delta", delta);
            accounts_internal_call(R.b, "elo", args, on_elo, ec);
            cJSON_Delete(args);
        }
    }
    cJSON_Delete(players);
    KN_INFO(MOD, "match %s finished (winner team %d)", r->match_id, winner);
    if (--r->pending == 0) result_done(r);
}

void ranked_result(Brain *b, const cJSON *frame, RankedResultCb cb, void *ud) {
    if (!b->db) {
        if (cb) cb(ud, "RankedUnavailable", NULL);
        return;
    }
    ResultCtx *r = calloc(1, sizeof(*r));
    if (!r) {
        if (cb) cb(ud, "oom", NULL);
        return;
    }
    r->cb = cb;
    r->ud = ud;
    snprintf(r->match_id, sizeof(r->match_id), "%s", kn_json_str(frame, "matchId", ""));
    r->frame = cJSON_Duplicate(frame, 1);
    cJSON *stored = cJSON_Duplicate(frame, 1);
    cJSON_DeleteItemFromObjectCaseSensitive(stored, "type");
    cJSON_DeleteItemFromObjectCaseSensitive(stored, "rid");
    char *s = kn_json_print(stored);
    cJSON_Delete(stored);
    kn_db_query(b->db, "SELECT kn_rq_match_finish($1::text, $2::jsonb)::text", KN_DB_ARGS(r->match_id, s ? s : "{}"),
                on_finished, r);
    free(s);
}

/* ================================================================ module */

static void route2(Brain *b, const char *name, kn_http_handler h) {
    char path[96];
    snprintf(path, sizeof(path), "/mm/queue/%s", name);
    kn_http_route(b->http, "GET", path, h, b);
    snprintf(path, sizeof(path), "/queue/%s", name);
    kn_http_route(b->http, "GET", path, h, b);
}

int ranked_init(Brain *b) {
    const struct kn_config *cfg = b->cfg;
    R.b = b;
    R.enabled = kn_config_bool(cfg, "brain.ranked.enabled", 1);
    R.min_level = (int) kn_config_int(cfg, "brain.ranked.min_level", 15);          /* config.minRankedLevel */
    R.mode = (int) kn_config_int(cfg, "brain.ranked.mode", 14);                     /* modes[14] Ranked */
    const cJSON *maps = kn_config_get(cfg, "brain.ranked.maps");
    R.maps = cJSON_IsArray(maps) ? maps : NULL;
    R.poll_timeout_s = (int) kn_config_int(cfg, "brain.ranked.poll_timeout_s", 15);
    R.creating_timeout_s = (int) kn_config_int(cfg, "brain.ranked.creating_timeout_s", 60);
    R.match_timeout_s = (int) kn_config_int(cfg, "brain.ranked.match_timeout_s", 1800);
    R.window = (int) kn_config_int(cfg, "brain.ranked.rating_window", 150);
    R.window_growth = (int) kn_config_int(cfg, "brain.ranked.window_growth", 10);
    R.window_max = (int) kn_config_int(cfg, "brain.ranked.window_max", 1000);
    R.k = kn_config_num(cfg, "brain.ranked.k", 32);                                 /* config.eloK */
    R.abandon_penalty = (int) kn_config_int(cfg, "brain.ranked.abandon_penalty", 30); /* rankedDeduction */
    R.cooldown_s = (int) kn_config_int(cfg, "brain.ranked.cooldown_s", 7200);       /* rankedCooldown */
    R.start_grace_s = (int) kn_config_int(cfg, "brain.ranked.start_grace_s", 60);
    route2(b, "join", h_join);
    route2(b, "poll", h_poll);
    route2(b, "update-ready", h_ready);
    route2(b, "update-queues", h_queues);
    route2(b, "update-class", h_class);
    route2(b, "kick-party-member", h_kick);
    route2(b, "promote-party-member", h_promote);
    route2(b, "leave", h_leave);
    if (!b->db || !R.enabled) {
        KN_INFO(MOD, "ranked queue disabled (%s)", b->db ? "brain.ranked.enabled=false" : "no database");
        return 0;
    }
    R.timer = kn_loop_add_timer(b->loop, 1000, 1000, tick, NULL);
    KN_INFO(MOD, "ranked queue on /mm/queue/* (min level %d, mode %d, K %.0f)", R.min_level, R.mode, R.k);
    return 0;
}

void ranked_shutdown(Brain *b) {
    if (R.timer) kn_loop_cancel_timer(b->loop, R.timer);
    R.timer = 0;
    R.enabled = 0;
}
