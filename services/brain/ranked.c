/*
 * services/brain/ranked.c - Krunker v7.1.4 ranked matchmaking (doc/backend.md 6.5).
 *
 * v7.1.4's ranked: one competitive queue (the Ranked window, windows[26], in the main
 * menu bar) for signed-in players of level >= minRankedLevel (15), in parties, per
 * region. A formed match is a comp lobby (game.isComp, config.hostTypes[1] "Comp
 * Server Setup": Hardpoint 4v4 on the comp maps, rankedClasses, fixMov / jumpMlt /
 * slidSpd) that only its roster may join. Ratings: eloK 32 against the other team's
 * average, abandon = -rankedDeduction (30) and a rankedCooldown (2 h) queue ban,
 * ranks every eloPer (120) (shared/include/ranked.h). Ratings are per season
 * (ranked_ratings, <dialect>/0204_ranked_seasons.sql; the season is
 * accounts.current_season) and mirrored to players.player_elo, the account field
 * v7's profile data carries. v7's client bundle has no queue code left (the
 * window's body is empty in the reconstruction), so the HTTP shapes are the
 * v3.1.1 ClientManager QueueManager ones KrunkNative already served.
 *
 * Routes: /mm/queue/join (token, region, party, ready, classIndex; queues may be
 * omitted: the only queue is "ranked"), /queue/poll, /queue/update-ready,
 * /queue/update-class, /queue/kick-party-member, /queue/promote-party-member,
 * /queue/leave, and /mm/queue/rating (the Ranked window's rank card).
 *
 * State lives in the shared database (<dialect>/0201_ranked.sql; kn_rq_* are
 * plpgsql on PostgreSQL, C in db_mm.c elsewhere: P_RQ_*) so every brain serves
 * the same queue. Each brain ticks every second; the one that gets the matcher
 * lock (P_RQ_LOCK: pg_try_advisory_xact_lock(kn_rq_lock()) on PostgreSQL) forms
 * matches (the oldest ready party anchors, others within a rating window that
 * widens with its wait fill two teams), hosts each on an idle game server
 * (brain_host_game_ex, "ranked" config) and moves the parties to "Join". The game
 * server reports "ranked-result" over the internal WS; the brain applies the
 * ratings.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "brain_internal.h"
#include <map_catalog.h> /* the comp map pool: catalogue indices (= v7 mapIndex) */
#include <modes.h>       /* MODE_ID_POINT */
#include <ranked.h>      /* the rating rules, config.rankedClasses, team size */
#include "knsvc/config.h"
#include "knsvc/crypto.h"
#include "knsvc/db.h"
#include "db_procs.h"
#include "knsvc/http.h"
#include "knsvc/json.h"
#include "knsvc/log.h"
#include "knsvc/loop.h"

#define MOD "ranked"
#define QUEUE_ID "ranked"            /* the one queue (its rating key is "elo": the season rating) */
#define MAX_MAPS 32

static const char *const VALID_QUEUES = "{" QUEUE_ID "}";
/* config.hostTypes[1] overDat maps / allMaps: burg, sandstorm, undergrowth, shipment,
 * industry, lumber, site, bureau, atomic (catalogue index = v7 mapIndex) */
static const int k_comp_maps[] = {0, 2, 4, 5, 11, 12, 14, 17, 23};

static struct {
    Brain *b;
    int enabled;
    int min_level;
    int team_size;                /* players per team (4: "Krunker Competitive" 4v4) */
    int mode;
    int maps[MAX_MAPS];           /* catalogue indices */
    int map_count;
    int season;                   /* accounts.current_season */
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

/* The season ranked ratings count for. TODO(season-system): acct_current_season() once it lands. */
static int ranked_season(void) {
    return R.season;
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

static void itoa64(char *out, size_t n, int64_t v) {
    snprintf(out, n, "%lld", (long long) v);
}

/* the account's rating row of the current season (absent = a fresh 0) */
static const kn_sql k_rating_sql = {
    .any = "SELECT rating, peak, games, wins, losses, abandons FROM ranked_ratings WHERE account_id = $1 AND season = $2"};
/* ms left of a queue ban (rankedCooldown), no row = none */
static const kn_sql k_ban_sql = {
    .any = "SELECT {epoch_ms:until} - {epoch_ms:{now}} FROM mm_ranked_bans WHERE account_id = $1 AND until > {now}"};

/* ================================================================ HTTP: join */

typedef struct JoinCtx {
    kn_http_req *req;
    char region[64];
    char party[64];
    int ready;
    int class_index;
    char client_id[32];
    char client_key[64];
    int64_t account_id;
    int level;
    char name[64];
    cJSON *clan, *clan_rank;      /* owned copies of the account's clan fields */
} JoinCtx;

static void join_free(JoinCtx *j) {
    cJSON_Delete(j->clan);
    cJSON_Delete(j->clan_rank);
    free(j);
}

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
        join_free(j);
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
    join_free(j);
}

/* the season rating read: queue the member with it (account_data.elo) */
static void on_join_rating(kn_db_result *res, void *ud) {
    JoinCtx *j = ud;
    if (!kn_db_ok(res)) {
        log_err("ranked rating", res);
        reply_error(j->req, 500, "internal");
        join_free(j);
        return;
    }
    const int64_t rating = kn_db_rows(res) > 0 ? kn_db_get_int64(res, 0, 0, 0) : 0;
    cJSON *data = cJSON_CreateObject();
    cJSON_AddItemToObject(data, "c", j->clan ? cJSON_Duplicate(j->clan, 1) : cJSON_CreateNull());
    cJSON_AddItemToObject(data, "cR", j->clan_rank ? cJSON_Duplicate(j->clan_rank, 1) : cJSON_CreateNull());
    cJSON_AddNumberToObject(data, "elo", (double) rating);
    cJSON_AddNumberToObject(data, "level", j->level);
    char *data_s = kn_json_print(data);
    cJSON_Delete(data);
    char acc_s[32], cls[16], new_party[16], max_party[8];
    itoa64(acc_s, sizeof(acc_s), j->account_id);
    snprintf(cls, sizeof(cls), "%d", j->class_index);
    snprintf(max_party, sizeof(max_party), "%d", R.team_size); /* a party fills at most one team */
    {
        static const char cs[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
        unsigned char rnd[6];
        kn_random_bytes(rnd, sizeof(rnd));
        for (int i = 0; i < 6; i++) new_party[i] = cs[rnd[i] % (sizeof(cs) - 1)];
        new_party[6] = 0;
    }
    kn_db_call(R.b->db, &P_RQ_JOIN,
                KN_DB_ARGS(acc_s, j->name, data_s, j->region, VALID_QUEUES, j->party, j->ready ? "true" : "false", cls,
                           j->client_id, j->client_key, new_party, max_party),
                on_joined, j);
    free(data_s);
}

static void on_join_account(void *ud, const char *err, const cJSON *acc) {
    JoinCtx *j = ud;
    int64_t id = acc ? kn_json_int(acc, "id", 0) : 0;
    if (err || id <= 0) {
        reply_join(j->req, NULL, NULL, "LoginRequired", 0); /* windows.ranked.login */
        join_free(j);
        return;
    }
    j->level = (int) kn_json_int(acc, "level", 0);
    if (j->level < R.min_level) {
        reply_join(j->req, NULL, NULL, "LevelTooLow", 0);   /* windows.ranked.level */
        join_free(j);
        return;
    }
    j->account_id = id;
    snprintf(j->name, sizeof(j->name), "%s", kn_json_str(acc, "username", kn_json_str(acc, "name", "?")));
    const cJSON *clan = cJSON_GetObjectItemCaseSensitive(acc, "clan");
    const cJSON *cr = cJSON_GetObjectItemCaseSensitive(acc, "clan_rank");
    j->clan = clan ? cJSON_Duplicate(clan, 1) : NULL;
    j->clan_rank = cr ? cJSON_Duplicate(cr, 1) : NULL;
    char acc_s[32], season[16];
    itoa64(acc_s, sizeof(acc_s), id);
    snprintf(season, sizeof(season), "%d", ranked_season());
    kn_db_query_sql(R.b->db, &k_rating_sql, KN_DB_ARGS(acc_s, season), on_join_rating, j);
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
    /* `queues` is accepted for the v3.1.1 shape but there is one queue: anything else is refused */
    if (kn_http_query_get(req, "queues", tmp, sizeof(tmp)) > 0 && !strstr(tmp, "\"" QUEUE_ID "\"")) {
        reply_join(req, NULL, NULL, "InvalidQueue", 0);
        free(j);
        return;
    }
    tmp[0] = 0;
    kn_http_query_get(req, "ready", tmp, sizeof(tmp));
    j->ready = !strcmp(tmp, "1") || !strcasecmp(tmp, "true");
    tmp[0] = 0;
    kn_http_query_get(req, "classIndex", tmp, sizeof(tmp));
    j->class_index = atoi(tmp);
    if (!ranked_class_allowed(j->class_index)) j->class_index = g_ranked_classes[0]; /* config.rankedClasses */
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

/* ================================================================ HTTP: rating (the rank card) */

typedef struct RatingCtx {
    kn_http_req *req;
    int64_t account_id;
    int level;
    cJSON *out;
} RatingCtx;

static void on_rating_ban(kn_db_result *res, void *ud) {
    RatingCtx *c = ud;
    const int64_t ms = kn_db_ok(res) && kn_db_rows(res) > 0 ? kn_db_get_int64(res, 0, 0, 0) : 0;
    cJSON_AddNumberToObject(c->out, "cooldownMs", (double) (ms > 0 ? ms : 0));
    kn_http_set_header(c->req, "Cache-Control", "no-cache, no-store, must-revalidate");
    brain_reply_json(c->req, 200, c->out);
    cJSON_Delete(c->out);
    free(c);
}

static void on_rating_row(kn_db_result *res, void *ud) {
    RatingCtx *c = ud;
    if (!kn_db_ok(res)) {
        log_err("ranked rating", res);
        reply_error(c->req, 500, "internal");
        cJSON_Delete(c->out);
        free(c);
        return;
    }
    const int row = kn_db_rows(res) > 0;
    const char *keys[] = {"rating", "peak", "games", "wins", "losses", "abandons"};
    int64_t v[6] = {0};
    for (int i = 0; i < 6; i++) {
        v[i] = row ? kn_db_get_int64(res, 0, i, 0) : 0;
        cJSON_AddNumberToObject(c->out, keys[i], (double) v[i]);
    }
    cJSON_AddNumberToObject(c->out, "rank", ranked_rank_number((double) v[0]));
    char acc_s[32];
    itoa64(acc_s, sizeof(acc_s), c->account_id);
    kn_db_query_sql(R.b->db, &k_ban_sql, KN_DB_ARGS(acc_s), on_rating_ban, c);
}

static void on_rating_account(void *ud, const char *err, const cJSON *acc) {
    RatingCtx *c = ud;
    c->account_id = acc ? kn_json_int(acc, "id", 0) : 0;
    if (err || c->account_id <= 0) {
        reply_error(c->req, 401, "LoginRequired");
        cJSON_Delete(c->out);
        free(c);
        return;
    }
    c->level = (int) kn_json_int(acc, "level", 0);
    cJSON_AddNumberToObject(c->out, "level", c->level);
    char acc_s[32], season[16];
    itoa64(acc_s, sizeof(acc_s), c->account_id);
    snprintf(season, sizeof(season), "%d", ranked_season());
    kn_db_query_sql(R.b->db, &k_rating_sql, KN_DB_ARGS(acc_s, season), on_rating_row, c);
}

/* GET /mm/queue/rating?token= -> {season, rating, peak, games, wins, losses, abandons, rank, level,
 * minLevel, teamSize, cooldownMs} */
static void h_rating(kn_http_req *req, void *ud) {
    (void) ud;
    if (!db_ready()) {
        reply_error(req, 503, "RankedUnavailable");
        return;
    }
    char token[512] = "";
    if (kn_http_query_get(req, "token", token, sizeof(token)) <= 0) {
        const char *bearer = kn_http_bearer(req);
        if (bearer) snprintf(token, sizeof(token), "%s", bearer);
    }
    if (!token[0]) {
        reply_error(req, 401, "LoginRequired");
        return;
    }
    RatingCtx *c = calloc(1, sizeof(*c));
    if (!c) {
        reply_error(req, 500, "internal");
        return;
    }
    c->req = req;
    c->out = cJSON_CreateObject();
    cJSON_AddNumberToObject(c->out, "season", ranked_season());
    cJSON_AddNumberToObject(c->out, "minLevel", R.min_level);
    cJSON_AddNumberToObject(c->out, "teamSize", R.team_size);
    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "token", token);
    accounts_internal_call(R.b, "token", args, on_rating_account, c);
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
    kn_db_call(R.b->db, &P_RQ_STATE, KN_DB_ARGS(key), on_state, req);
}

static void action(kn_http_req *req, const char *name, const char *param) {
    char key[96], arg[512] = "";
    if (get_key(req, key, sizeof(key))) return;
    if (param && kn_http_query_get(req, param, arg, sizeof(arg)) < 0) arg[0] = 0;
    kn_db_call(R.b->db, &P_RQ_ACTION, KN_DB_ARGS(key, name, arg, VALID_QUEUES), on_state, req);
}

static void h_ready(kn_http_req *req, void *ud) { (void) ud; action(req, "ready", "isReady"); }
static void h_class(kn_http_req *req, void *ud) { (void) ud; action(req, "class", "classIndex"); }
static void h_kick(kn_http_req *req, void *ud) { (void) ud; action(req, "kick", "kickClientId"); }
static void h_promote(kn_http_req *req, void *ud) { (void) ud; action(req, "promote", "promoteClientId"); }
static void h_leave(kn_http_req *req, void *ud) { (void) ud; action(req, "leave", NULL); }

/* ================================================================ matcher */

typedef struct Party {
    char id[64];
    char region[64];
    int queued;                   /* in the ranked queue */
    int size;
    double rating;                /* member average */
    double wait_s;
    const cJSON *members;         /* [{a, n, c, elo}] (borrowed from cand) */
    int used;
} Party;

typedef struct Match {
    char match_id[40];
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
            if (cJSON_IsString(q) && !strcmp(q->valuestring, QUEUE_ID)) p->queued = 1;
        }
        p->wait_s = kn_json_num(row, "wait_s", 0);
        p->members = cJSON_GetObjectItemCaseSensitive(row, "members");
        p->size = cJSON_GetArraySize(p->members);
        const cJSON *m;
        cJSON_ArrayForEach(m, p->members) p->rating += kn_json_num(m, "elo", 0);
        if (p->size > 0) p->rating /= p->size;
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

/* The oldest ready party anchors a match; other parties of its region within the
 * rating window (widening with the anchor's wait) fill two teams of R.team_size. */
static Match *form_matches(Party *ps, int count) {
    Match *head = NULL;
    Party **order = calloc((size_t) (count > 0 ? count : 1), sizeof(Party *));
    for (int i = 0; i < count; i++) order[i] = &ps[i];
    qsort(order, (size_t) count, sizeof(Party *), by_wait_desc);
    const int n = R.team_size;
    for (int a = 0; a < count; a++) {
        Party *anchor = order[a];
        if (anchor->used || !anchor->queued || anchor->size > n) continue;
        double window = R.window + R.window_growth * anchor->wait_s;
        if (window > R.window_max) window = R.window_max;
        Party *team[2][RANKED_TEAM_SIZE * 2];
        int tn[2] = {0, 0}, free_slots[2] = {n - anchor->size, n};
        team[0][tn[0]++] = anchor;
        for (int b = 0; b < count && (free_slots[0] || free_slots[1]); b++) {
            Party *p = order[b];
            if (p == anchor || p->used || !p->queued || p->size > n) continue;
            if (strcmp(p->region, anchor->region)) continue;
            if (fabs(p->rating - anchor->rating) > window) continue;
            int t = p->size <= free_slots[0] ? 0 : p->size <= free_slots[1] ? 1 : -1;
            if (t < 0 || tn[t] >= RANKED_TEAM_SIZE * 2) continue;
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
    free(order);
    return head;
}

/* ---- hosting a formed match */

typedef struct HostMatch {
    char match_id[40];
    char region[64];
    cJSON *players;               /* teams_players (owned) */
} HostMatch;

static void on_ranked_hosted(void *ud, const char *err, const cJSON *reply) {
    HostMatch *h = ud;
    Brain *b = R.b;
    if (err || !reply || !b->db) {
        KN_WARN(MOD, "match %s: hosting failed (%s); parties back in the queue", h->match_id, err ? err : "?");
        if (b->db)
            kn_db_call(b->db, &P_RQ_MATCH_FAILED,
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
                itoa64(key, sizeof(key), acc);
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
        kn_db_call(b->db, &P_RQ_MATCH_READY,
                    KN_DB_ARGS(h->match_id, gid, cs, ts), on_ignore, "match ready");
        KN_INFO(MOD, "match %s (%s) is live on %s", h->match_id, h->region, gid);
        free(cs);
        free(ts);
        cJSON_Delete(c);
        cJSON_Delete(tickets);
    }
    cJSON_Delete(h->players);
    free(h);
}

/* The game server's "ranked" host config: a comp lobby (config.hostTypes[1]) for the roster,
 * tmSize = team size - 1, Hardpoint (overDat modes [2]) on a random map of the pool. */
static void host_match(const Match *m) {
    HostMatch *h = calloc(1, sizeof(*h));
    if (!h) return;
    snprintf(h->match_id, sizeof(h->match_id), "%s", m->match_id);
    snprintf(h->region, sizeof(h->region), "%s", m->region);
    h->players = cJSON_Duplicate(m->teams_players, 1);
    cJSON *cfg = cJSON_CreateObject();
    cJSON_AddBoolToObject(cfg, "ranked", 1);
    cJSON_AddStringToObject(cfg, "matchId", m->match_id);
    cJSON_AddStringToObject(cfg, "queue", QUEUE_ID);
    cJSON_AddNumberToObject(cfg, "teamSize", R.team_size);
    cJSON *modes = cJSON_AddArrayToObject(cfg, "modes");
    cJSON_AddItemToArray(modes, cJSON_CreateNumber(R.mode));
    cJSON_AddNumberToObject(cfg, "mode", R.mode);
    if (R.map_count > 0) {
        const int pick = R.maps[kn_random_u64() % (uint64_t) R.map_count];
        cJSON *maps = cJSON_AddArrayToObject(cfg, "maps");
        cJSON_AddItemToArray(maps, cJSON_CreateNumber(pick));
    }
    cJSON_AddBoolToObject(cfg, "private", 1);
    cJSON_AddNumberToObject(cfg, "startGraceS", R.start_grace_s);
    cJSON_AddItemToObject(cfg, "teams", cJSON_Duplicate(m->teams_players, 1));
    KN_INFO(MOD, "match %s formed (%dv%d, %s): hosting", m->match_id, R.team_size, R.team_size, m->region);
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
        kn_db_tx_call(run->tx, &P_RQ_MATCH_CREATE,
                       KN_DB_ARGS(m->match_id, QUEUE_ID, m->region, R.b->id, teams), on_match_created,
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
    kn_db_tx_call(run->tx, &P_RQ_CANDIDATES, KN_DB_ARGS(R.b->id), on_candidates, run);
}

static void on_begin(kn_db_tx *tx, kn_db_result *res, void *ud) {
    MatchRun *run = ud;
    if (!tx) {
        log_err("matcher begin", res);
        run_free(run);
        return;
    }
    run->tx = tx;
    kn_db_tx_call(tx, &P_RQ_LOCK, 0, NULL, on_lock, run);
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
        kn_db_call(R.b->db, &P_RQ_EXPIRE,
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
    cJSON *ratings;
} ResultCtx;

static void result_done(ResultCtx *r) {
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddBoolToObject(reply, "ok", 1);
    cJSON_AddItemToObject(reply, "ratings", r->ratings);
    /* store the new ratings with the match */
    if (R.b->db) {
        char *s = kn_json_print(r->ratings);
        kn_db_call(R.b->db, &P_RQ_RATINGS,
                    KN_DB_ARGS(r->match_id, s ? s : "[]"), on_ignore, "match ratings");
        free(s);
    }
    if (r->cb) r->cb(r->ud, NULL, reply);
    cJSON_Delete(reply);
    cJSON_Delete(r->frame);
    free(r);
}

static int abandoned(const cJSON *frame, int64_t acc) {
    const cJSON *a;
    cJSON_ArrayForEach(a, cJSON_GetObjectItemCaseSensitive(frame, "abandoned")) {
        if (cJSON_IsNumber(a) && (int64_t) a->valuedouble == acc) return 1;
    }
    return 0;
}

/* this season's row: rating += delta (floored at 0), peak, games, wins / losses / abandons. One
 * statement (the pool may run two queries on different connections in any order). */
static void store_rating(int64_t acc, int delta, int won, int lost, int quit) {
    static const kn_sql upsert = {
        .pg = "INSERT INTO ranked_ratings (account_id, season, rating, peak, games, wins, losses, abandons)"
              " VALUES ($1::bigint, $2::integer, greatest(0, $3::integer), greatest(0, $3::integer), 1, $4::integer,"
              " $5::integer, $6::integer)"
              " ON CONFLICT (account_id, season) DO UPDATE SET rating = greatest(0, ranked_ratings.rating + $3::integer),"
              " peak = greatest(ranked_ratings.peak, ranked_ratings.rating + $3::integer), games = ranked_ratings.games + 1,"
              " wins = ranked_ratings.wins + $4::integer, losses = ranked_ratings.losses + $5::integer,"
              " abandons = ranked_ratings.abandons + $6::integer, updated_at = now()",
        .sqlite = "INSERT INTO ranked_ratings (account_id, season, rating, peak, games, wins, losses, abandons)"
                  " VALUES ($1, $2, max(0, $3), max(0, $3), 1, $4, $5, $6)"
                  " ON CONFLICT (account_id, season) DO UPDATE SET rating = max(0, rating + $3),"
                  " peak = max(peak, rating + $3), games = games + 1, wins = wins + $4, losses = losses + $5,"
                  " abandons = abandons + $6, updated_at = {now}",
        .mysql = "INSERT INTO ranked_ratings (account_id, season, rating, peak, games, wins, losses, abandons)"
                 " VALUES ($1, $2, GREATEST(0, $3), GREATEST(0, $3), 1, $4, $5, $6)"
                 " ON DUPLICATE KEY UPDATE peak = GREATEST(peak, rating + $3), rating = GREATEST(0, rating + $3),"
                 " games = games + 1, wins = wins + $4, losses = losses + $5, abandons = abandons + $6, updated_at = {now}"};
    char a[32], season[16], d[16], w[4], l[4], q[4];
    itoa64(a, sizeof(a), acc);
    snprintf(season, sizeof(season), "%d", ranked_season());
    snprintf(d, sizeof(d), "%d", delta);
    snprintf(w, sizeof(w), "%d", won);
    snprintf(l, sizeof(l), "%d", lost);
    snprintf(q, sizeof(q), "%d", quit);
    kn_db_query_sql(R.b->db, &upsert, KN_DB_ARGS(a, season, d, w, l, q), on_ignore, "ranked rating");
}

static void on_elo_mirrored(void *ud, const char *err, const cJSON *data) {
    (void) data;
    if (err) KN_WARN(MOD, "player_elo of account %lld: %s", (long long) (intptr_t) ud, err);
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
    /* each player against the other team's average rating (shared/include/ranked.h) */
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
    cJSON_ArrayForEach(p, players) {
        int64_t acc = kn_json_int(p, "accountId", 0);
        int t = (int) kn_json_int(p, "team", 0) ? 1 : 0;
        int before = (int) kn_json_int(p, "before", 0);
        double score = winner < 0 || winner > 1 ? 0.5 : winner == t ? 1.0 : 0.0;
        int quit = abandoned(r->frame, acc);
        const double opponents = cnt[1 - t] ? avg[1 - t] : avg[t];
        /* an abandoner loses rankedDeduction (brain.ranked.abandon_penalty) instead */
        int delta = quit ? -R.abandon_penalty : (int) lround(ranked_delta(before, opponents, score, 0, R.k));
        if (before + delta < 0) delta = -before;
        const int won = !quit && score == 1.0, lost = !quit && score == 0.0;
        cJSON *e = cJSON_CreateObject();
        cJSON_AddNumberToObject(e, "accountId", (double) acc);
        cJSON_AddStringToObject(e, "name", kn_json_str(p, "name", ""));
        cJSON_AddNumberToObject(e, "team", t);
        cJSON_AddStringToObject(e, "key", "elo");
        cJSON_AddNumberToObject(e, "season", ranked_season());
        cJSON_AddNumberToObject(e, "old", before);
        cJSON_AddNumberToObject(e, "new", before + delta);
        cJSON_AddNumberToObject(e, "delta", delta);
        cJSON_AddNumberToObject(e, "rank", ranked_rank_number(before + delta));
        if (quit) cJSON_AddBoolToObject(e, "abandoned", 1);
        cJSON_AddItemToArray(r->ratings, e);
        if (quit && R.b->db) { /* rankedCooldown */
            char a[32], secs[16];
            itoa64(a, sizeof(a), acc);
            snprintf(secs, sizeof(secs), "%d", R.cooldown_s);
            static const kn_sql ban = {
                .pg = "INSERT INTO mm_ranked_bans (account_id, until, reason)"
                      " VALUES ($1::bigint, now() + make_interval(secs => $2::integer), 'abandon')"
                      " ON CONFLICT (account_id) DO UPDATE SET until = EXCLUDED.until, reason = EXCLUDED.reason",
                .sqlite = "INSERT INTO mm_ranked_bans (account_id, until, reason) VALUES ($1, {later:$2}, 'abandon')"
                          " ON CONFLICT (account_id) DO UPDATE SET until = excluded.until, reason = excluded.reason",
                .mysql = "INSERT INTO mm_ranked_bans (account_id, until, reason) VALUES ($1, {later:$2}, 'abandon')"
                         " ON DUPLICATE KEY UPDATE until = VALUES(until), reason = VALUES(reason)"};
            kn_db_query_sql(R.b->db, &ban, KN_DB_ARGS(a, secs), on_ignore, "ranked ban");
        }
        if (acc > 0 && R.b->db) {
            store_rating(acc, delta, won, lost, quit);
            /* the account's player_elo (v7 profile data): this season's rating */
            cJSON *args = cJSON_CreateObject();
            cJSON_AddNumberToObject(args, "id", (double) acc);
            cJSON_AddNumberToObject(args, "mode", 1);
            cJSON_AddNumberToObject(args, "elo", before + delta);
            accounts_internal_call(R.b, "elo", args, on_elo_mirrored, (void *) (intptr_t) acc);
            cJSON_Delete(args);
        }
    }
    cJSON_Delete(players);
    KN_INFO(MOD, "match %s finished (winner team %d, season %d)", r->match_id, winner, ranked_season());
    result_done(r);
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
    kn_db_call(b->db, &P_RQ_MATCH_FINISH, KN_DB_ARGS(r->match_id, s ? s : "{}"),
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
    /* "Krunker Competitive" 4v4: tmSize 1v1..4v4 */
    R.team_size = (int) kn_config_int(cfg, "brain.ranked.team_size", RANKED_TEAM_SIZE);
    if (R.team_size < 1) R.team_size = 1;
    if (R.team_size > RANKED_TEAM_SIZE) R.team_size = RANKED_TEAM_SIZE;
    /* the comp host's mode (overDat modes [2], fixed: the game server's setupGameConfig keeps it):
     * Hardpoint. v7 has no ranked mode; ids 37 / 38 stay reserved for ranked (modes.h) */
    R.mode = MODE_ID_POINT;
    /* the map pool: catalogue indices (= v7 mapIndex) or catalogue names / keys; default the
     * comp host's maps. Unknown entries are dropped. */
    R.map_count = 0;
    const cJSON *maps = kn_config_get(cfg, "brain.ranked.maps");
    const cJSON *list = cJSON_IsArray(maps) ? maps : NULL;
    const cJSON *it;
    cJSON_ArrayForEach(it, list) {
        const int index = cJSON_IsNumber(it) ? it->valueint : cJSON_IsString(it) ? map_catalog_find(it->valuestring) : -1;
        if (index >= 0 && map_catalog_at((size_t) index) && R.map_count < MAX_MAPS) R.maps[R.map_count++] = index;
        else if (cJSON_IsString(it)) KN_WARN(MOD, "brain.ranked.maps: no catalogue map \"%s\"", it->valuestring);
        else KN_WARN(MOD, "brain.ranked.maps: no catalogue map %d", it->valueint);
    }
    if (!R.map_count) {
        for (size_t i = 0; i < sizeof(k_comp_maps) / sizeof(k_comp_maps[0]); i++) {
            if (map_catalog_at((size_t) k_comp_maps[i])) R.maps[R.map_count++] = k_comp_maps[i];
        }
    }
    R.season = (int) kn_config_int(cfg, "accounts.current_season", 7);
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
    route2(b, "update-class", h_class);
    route2(b, "kick-party-member", h_kick);
    route2(b, "promote-party-member", h_promote);
    route2(b, "leave", h_leave);
    route2(b, "rating", h_rating);
    if (!b->db || !R.enabled) {
        KN_INFO(MOD, "ranked queue disabled (%s)", b->db ? "brain.ranked.enabled=false" : "no database");
        return 0;
    }
    R.timer = kn_loop_add_timer(b->loop, 1000, 1000, tick, NULL);
    KN_INFO(MOD, "ranked queue on /mm/queue/* (%dv%d, min level %d, mode %d, %d map(s), K %.0f, season %d)", R.team_size,
            R.team_size, R.min_level, R.mode, R.map_count, R.k, R.season);
    return 0;
}

void ranked_shutdown(Brain *b) {
    if (R.timer) kn_loop_cancel_timer(b->loop, R.timer);
    R.timer = 0;
    R.enabled = 0;
}
