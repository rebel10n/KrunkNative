/*
 * services/brain/ranked.c - Krunker v10.0.2 ranked matchmaking and profiles (doc/v10/ranked-impl.md,
 * doc/backend.md 6.5). Replaces the v7.1.4 party queue (/mm/queue/..., which now answers
 * {"status": "ClientOutdated"}).
 *
 * v10's ranked: a solo queue for signed-in players of level >= 10 in one or all of the ranked
 * regions (as / eu / na), monthly seasons (season 1 = 2025-08 UTC, from this brain's clock), a
 * 5-map monthly pool (shared/src/ranked.c ranked_season_maps), TrueSkill ratings per season and
 * region shown as 1000 + 100 * (mu - 3 sigma), 14 ranks after 6 placement matches, Ranked Points
 * from matches, milestones and the end-of-month conversion, and an RP shop.
 *
 * v10's client talks to a matchmaker WebSocket (gamefrontend /v1/matchmaking/queue). KrunkNative
 * keeps the queue on the brain's HTTP routes, so it works through the edge relays
 * (play./matchmaker.krunk.io) and the web build with no new transport: a long poll that carries
 * v10's message shapes ({type: QUEUE_STATUS | ERROR, payload}). Documented deviation.
 *
 * Routes (GET unless noted; token = the session token, query or Bearer):
 *   /mm/ranked/queue?token&regions=eu[,as,na]   join -> QUEUE_STATUS QUEUED {clientKey} | ERROR
 *   /mm/ranked/poll?clientKey&wait=<s>          long poll (<= 20 s) -> QUEUE_STATUS QUEUED | MATCHED
 *   /mm/ranked/leave?clientKey                  cancel (v10: close the WebSocket)
 *   /mm/ranked/connection?token                 {serverUrl, assignment} (v10 /ranked/me/connection)
 *   /mm/ranked/info                             {season, maps, regions, minLevel, ranks}
 *   /mm/ranked/profile?token&season&region      {profile, milestones, rankedPoints, season, maps}
 *   /mm/ranked/leaderboard?season&region&limit&offset   {leaderboard, totalPages}
 *   /mm/ranked/history?token&season&region&limit&offset {matchHistory: {matches, totalPages}}
 *   POST|GET /mm/ranked/claim-milestone?token&region    {success, reward, balance}
 *   /mm/ranked/shop                             {skins}
 *   POST|GET /mm/ranked/purchase?token&id       {success, balance}
 *
 * State lives in the shared database (0205_ranked_v10.sql, db_ranked.c procs, C on every driver)
 * so every brain serves the same queue. Each brain ticks every second; the one that gets the
 * matcher lock (P_RQ_LOCK) forms matches per region (the longest waiting player anchors, others
 * within a rating window that widens with its wait), balances the teams (snake draft by mu),
 * hosts each on an idle game server (brain_host_game_ex, "ranked" config, unchanged shape) and
 * moves the players to MATCHED. The game server reports "ranked-result" over the internal WS.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "brain_internal.h"
#include <map_catalog.h>
#include <modes.h>
#include <ranked.h>
#include "knsvc/config.h"
#include "knsvc/crypto.h"
#include "knsvc/db.h"
#include "db_procs.h"
#include "knsvc/http.h"
#include "knsvc/json.h"
#include "knsvc/log.h"
#include "knsvc/loop.h"
#include "social_ws.h"

#define MOD "ranked"
#define MAX_CAND 512
#define POLL_WAIT_MAX_S 20

/* the Krunker version ranked matches play, recorded per match and sent in the host config: v10.0.2,
 * the only version (shared/include/ruleset.h) */
#define RANKED_RULESET "v10"

static struct {
    Brain *b;
    int enabled;
    int min_level;
    int team_size;
    int season_override;          /* brain.ranked.season (-1 = the calendar) */
    char host_region[RANKED_REGION_COUNT][32]; /* game server region per ranked region ("" = any) */
    int poll_timeout_s, creating_timeout_s, match_timeout_s;
    double window, window_growth, window_max; /* display MMR */
    int cooldown_s;
    int start_grace_s;
    kn_timer_id timer;
    int matching, expiring, closing;
    int closed_season;            /* the last season this brain closed (RP conversion) */
    unsigned ticks;
} R;

static int db_ready(void) { return R.b && R.b->db && R.enabled; }

/* The current ranked season from this brain's UTC clock (authoritative, not the client's). */
static int current_season(void) {
    return R.season_override >= 0 ? R.season_override : ranked_season_at(kn_wall_ms() / 1000);
}

static void log_err(const char *what, kn_db_result *res) { KN_WARN(MOD, "%s failed: %s", what, kn_db_error(res)); }

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

/* v10 MatchmakingService ERROR {code, message, payload} */
static void reply_mm_error(kn_http_req *req, const char *code, const char *message, cJSON *payload) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "ERROR");
    cJSON *p = cJSON_AddObjectToObject(o, "payload");
    cJSON_AddStringToObject(p, "code", code);
    cJSON_AddStringToObject(p, "message", message);
    cJSON_AddItemToObject(p, "payload", payload ? payload : cJSON_CreateObject());
    kn_http_set_header(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    brain_reply_json(req, 200, o);
    cJSON_Delete(o);
}

static void get_token(kn_http_req *req, char *token, size_t cap) {
    token[0] = 0;
    if (kn_http_query_get(req, "token", token, cap) <= 0) {
        const char *bearer = kn_http_bearer(req);
        snprintf(token, cap, "%s", bearer ? bearer : "");
    }
}

static int query_int(kn_http_req *req, const char *key, int def) {
    char v[32];
    return kn_http_query_get(req, key, v, sizeof v) > 0 ? atoi(v) : def;
}

/* the region parameter: "eu" / "3" -> its code, else NULL */
static const char *query_region(kn_http_req *req, const char *def) {
    char v[16];
    if (kn_http_query_get(req, "region", v, sizeof v) <= 0) return def;
    const int index = ranked_region_index(v);
    return index >= 0 ? ranked_region_code(index) : NULL;
}

static void add_season_maps(cJSON *o, const int season) {
    int maps[RANKED_SEASON_MAPS];
    ranked_season_maps(season, maps);
    cJSON *a = cJSON_AddArrayToObject(o, "maps");
    for (int i = 0; i < RANKED_SEASON_MAPS; i++) {
        cJSON *m = cJSON_CreateObject();
        cJSON_AddNumberToObject(m, "id", maps[i]);
        const char *name = ranked_pool_map_name(maps[i]);
        cJSON_AddStringToObject(m, "name", name ? name : "");
        /* KrunkNative's catalogue lacks some v10 maps (Eterno Simulator 39, Frontier 42) */
        cJSON_AddBoolToObject(m, "available", maps[i] < MAP_CATALOG_OFFICIAL_COUNT && map_catalog_at((size_t) maps[i]) != NULL);
        cJSON_AddItemToArray(a, m);
    }
}

/* ---- account lookups: every route resolves its token through the accounts bridge */

typedef void (*AccFn)(void *ud, kn_http_req *req, int64_t id, int level, const char *name);
typedef struct AccCtx {
    kn_http_req *req;
    AccFn fn;
    void *ud;
    int required;
} AccCtx;

static void on_acc(void *ud, const char *err, const cJSON *acc) {
    AccCtx *c = ud;
    const int64_t id = acc && !err ? kn_json_int(acc, "id", 0) : 0;
    if (id <= 0 && c->required) {
        reply_mm_error(c->req, "LOGIN_REQUIRED", "You must be logged in to access Ranked Mode.", NULL);
        free(c->ud);
    } else {
        c->fn(c->ud, c->req, id, (int) (acc ? kn_json_int(acc, "level", 0) : 0),
              acc ? kn_json_str(acc, "username", kn_json_str(acc, "name", "?")) : "");
    }
    free(c);
}

/* fn(ud, req, id, level, name); without a token: required -> the LOGIN_REQUIRED error (ud freed), else id 0 */
static void with_account(kn_http_req *req, const int required, AccFn fn, void *ud) {
    char token[512];
    get_token(req, token, sizeof token);
    if (!token[0]) {
        if (required) {
            reply_mm_error(req, "LOGIN_REQUIRED", "You must be logged in to access Ranked Mode.", NULL);
            free(ud);
        } else {
            fn(ud, req, 0, 0, "");
        }
        return;
    }
    AccCtx *c = calloc(1, sizeof(*c));
    if (!c) {
        reply_error(req, 500, "internal");
        free(ud);
        return;
    }
    c->req = req;
    c->fn = fn;
    c->ud = ud;
    c->required = required;
    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "token", token);
    accounts_internal_call(R.b, "token", args, on_acc, c);
    cJSON_Delete(args);
}

/* ================================================================ queue */

typedef struct JoinCtx {
    kn_http_req *req;
    char regions[32];
    char key[64];
    char name[64];
} JoinCtx;

/* v10 server.message.queued "{0} queued for ranked" (the ranked queue start), to every player
 * this brain serves: each connected game node gets a "ranked-notice" (its players' chat, as
 * NET_CHAT_RANKED) and each signed-in JSON social socket an "eb" "ranked:queued" event (the menu's
 * notification). The colour is the ranked UI's green (#5ce05a). Other brains' nodes and sockets
 * get nothing from this queue (federation pushes are per account). */
static void queue_notice(const char *name) {
    char text[96];
    snprintf(text, sizeof text, "%s queued for ranked", name && name[0] ? name : "A player");
    cJSON *node = cJSON_CreateObject();
    cJSON_AddStringToObject(node, "type", "ranked-notice");
    cJSON_AddStringToObject(node, "text", text);
    cJSON_AddStringToObject(node, "color", "#5ce05a");
    internal_ws_broadcast(R.b, node);
    cJSON_Delete(node);
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "type", "ranked:queued");
    cJSON_AddStringToObject(obj, "name", name && name[0] ? name : "");
    cJSON_AddStringToObject(obj, "text", text);
    cJSON_AddStringToObject(obj, "color", "#5ce05a");
    cJSON *frame = cJSON_CreateArray();
    cJSON_AddItemToArray(frame, cJSON_CreateString("eb"));
    cJSON_AddItemToArray(frame, obj);
    social_push_all_json(frame);
}

static void on_joined(kn_db_result *res, void *ud) {
    JoinCtx *j = ud;
    if (!kn_db_ok(res) || kn_db_rows(res) < 1) {
        log_err("ranked join", res);
        reply_mm_error(j->req, "INTERNAL_ERROR", "Failed to connect to matchmaking server", NULL);
        free(j);
        return;
    }
    cJSON *r = kn_json_parse(kn_db_get(res, 0, 0), strlen(kn_db_get(res, 0, 0)));
    const char *code = kn_json_str(r, "code", NULL);
    if (code && !strcmp(code, "COOLDOWN")) {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddNumberToObject(p, "cooldown", kn_json_num(r, "cooldown", 0));
        reply_mm_error(j->req, "COOLDOWN", "You are on cooldown and cannot queue for ranked matches.", p);
    } else if (code) {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "serverUrl", kn_json_str(r, "serverUrl", ""));
        const cJSON *as = cJSON_GetObjectItemCaseSensitive(r, "assignment");
        if (as) cJSON_AddItemToObject(p, "assignment", cJSON_Duplicate(as, 1));
        reply_mm_error(j->req, "ALREADY_IN_MATCH", "You are already in a Ranked game.", p);
    } else {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "type", "QUEUE_STATUS");
        cJSON *p = cJSON_AddObjectToObject(o, "payload");
        cJSON_AddStringToObject(p, "status", "QUEUED");
        cJSON_AddStringToObject(p, "clientKey", j->key);
        cJSON_AddStringToObject(p, "regions", j->regions);
        cJSON_AddNumberToObject(p, "season", current_season());
        cJSON_AddNumberToObject(p, "estimatedWaitTime", 120);
        queue_notice(j->name);
        brain_reply_json(j->req, 200, o);
        cJSON_Delete(o);
    }
    cJSON_Delete(r);
    free(j);
}

static void join_account(void *ud, kn_http_req *req, const int64_t id, const int level, const char *name) {
    JoinCtx *j = ud;
    (void) req;
    snprintf(j->name, sizeof j->name, "%s", name ? name : "");
    if (level < R.min_level) {
        reply_mm_error(j->req, "MIN_LEVEL_REQUIREMENT", "You must be level 10 or higher to access Ranked Mode.", NULL);
        free(j);
        return;
    }
    char acc[24], cd[16];
    snprintf(acc, sizeof acc, "%lld", (long long) id);
    snprintf(cd, sizeof cd, "%d", R.cooldown_s);
    kn_random_token(j->key, sizeof(j->key), 24);
    kn_db_call(R.b->db, &P_RK_JOIN, KN_DB_ARGS(acc, name, j->key, j->regions, cd), on_joined, j);
}

/* regions=eu or as,eu,na (v10 "&regions=" + regions.join(",")); unknown entries are dropped */
static void h_queue(kn_http_req *req, void *ud) {
    (void) ud;
    /* ranked is Krunker Box Office's: its matches are hosted on base nodes only (brain_host_game_ex
     * flavor NULL), so another app's client (?flavor=strike, doc/backend.md "Flavor pools") is never
     * queued into one, whose server would refuse its HELLO */
    char flavor[16] = "";
    kn_http_query_get(req, "flavor", flavor, sizeof flavor);
    if (!registry_flavor_match("", flavor))
        return reply_mm_error(req, "FLAVOR_UNAVAILABLE", "Ranked is not available in this game", NULL);
    if (!db_ready()) return reply_mm_error(req, "INTERNAL_ERROR", "Ranked is unavailable", NULL);
    if (R.b->draining) return reply_mm_error(req, "INTERNAL_ERROR", "updating", NULL);
    JoinCtx *j = calloc(1, sizeof(*j));
    if (!j) return reply_error(req, 500, "internal");
    j->req = req;
    char raw[64] = "";
    kn_http_query_get(req, "regions", raw, sizeof raw);
    if (!raw[0]) kn_http_query_get(req, "region", raw, sizeof raw);
    for (char *tok = strtok(raw, ","); tok; tok = strtok(NULL, ",")) {
        const int index = ranked_region_index(tok);
        const char *code = index >= 0 ? ranked_region_code(index) : NULL;
        if (!code || strstr(j->regions, code)) continue;
        if (j->regions[0]) strcat(j->regions, ",");
        strcat(j->regions, code);
    }
    if (!j->regions[0]) snprintf(j->regions, sizeof j->regions, "eu"); /* v10 default region (index 3) */
    with_account(req, 1, join_account, j);
}

typedef struct PollCtx {
    kn_http_req *req;
    char key[96];
    int64_t deadline_ms;
} PollCtx;

static void poll_fire(PollCtx *c);

static void poll_timer(kn_loop *loop, void *ud) {
    (void) loop;
    poll_fire(ud);
}

static void on_polled(kn_db_result *res, void *ud) {
    PollCtx *c = ud;
    if (!kn_db_ok(res)) {
        log_err("ranked poll", res);
        reply_mm_error(c->req, "INTERNAL_ERROR", "Failed to parse server message", NULL);
        free(c);
        return;
    }
    if (kn_db_rows(res) < 1 || kn_db_isnull(res, 0, 0)) {
        reply_error(c->req, 404, "InvalidClientKey");
        free(c);
        return;
    }
    cJSON *st = kn_json_parse(kn_db_get(res, 0, 0), strlen(kn_db_get(res, 0, 0)));
    const int matched = !strcmp(kn_json_str(st, "status", ""), "MATCHED");
    if (!matched && kn_wall_ms() < c->deadline_ms && kn_http_req_alive(c->req) && !R.b->draining) {
        cJSON_Delete(st);
        kn_loop_add_timer(R.b->loop, 500, 0, poll_timer, c);
        return;
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "QUEUE_STATUS");
    cJSON_AddItemToObject(o, "payload", st);
    kn_http_set_header(c->req, "Cache-Control", "no-cache, no-store, must-revalidate");
    brain_reply_json(c->req, 200, o);
    cJSON_Delete(o);
    free(c);
}

static void poll_fire(PollCtx *c) {
    if (!kn_http_req_alive(c->req)) { /* the client went away: answering only releases the request */
        reply_error(c->req, 410, "gone");
        free(c);
        return;
    }
    kn_db_call(R.b->db, &P_RK_POLL, KN_DB_ARGS(c->key), on_polled, c);
}

static void h_poll(kn_http_req *req, void *ud) {
    (void) ud;
    if (!db_ready()) return reply_error(req, 503, "RankedUnavailable");
    PollCtx *c = calloc(1, sizeof(*c));
    if (!c) return reply_error(req, 500, "internal");
    c->req = req;
    if (kn_http_query_get(req, "clientKey", c->key, sizeof c->key) <= 0) {
        free(c);
        return reply_error(req, 404, "InvalidClientKey");
    }
    int wait = query_int(req, "wait", 0);
    if (wait < 0) wait = 0;
    if (wait > POLL_WAIT_MAX_S) wait = POLL_WAIT_MAX_S;
    c->deadline_ms = kn_wall_ms() + wait * 1000;
    poll_fire(c);
}

static void on_left(kn_db_result *res, void *ud) {
    kn_http_req *req = ud;
    if (!kn_db_ok(res)) log_err("ranked leave", res);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "left", kn_db_ok(res) && kn_db_affected(res) > 0);
    brain_reply_json(req, 200, o);
    cJSON_Delete(o);
}

static void h_leave(kn_http_req *req, void *ud) {
    (void) ud;
    if (!db_ready()) return reply_error(req, 503, "RankedUnavailable");
    char key[96];
    if (kn_http_query_get(req, "clientKey", key, sizeof key) <= 0) return reply_error(req, 404, "InvalidClientKey");
    static const kn_sql q = {.any = "DELETE FROM ranked_queue WHERE client_key = $1 AND status = 'QUEUED' AND match_id IS NULL"};
    kn_db_query_sql(R.b->db, &q, KN_DB_ARGS(key), on_left, req);
}

/* ================================================================ profile routes */

typedef struct ReqCtx {
    kn_http_req *req;
    int64_t id;
    int season, limit, offset, region_index;
    char region[8];
    cJSON *out;
    char shop_id[16];
    char name[64];
    int global;        /* leaderboard over every region (v10 "Global"): each player's best profile */
} ReqCtx;

static ReqCtx *req_ctx(kn_http_req *req) {
    ReqCtx *c = calloc(1, sizeof(*c));
    if (!c) {
        reply_error(req, 500, "internal");
        return NULL;
    }
    c->req = req;
    c->season = query_int(req, "season", current_season());
    if (c->season < 0) c->season = 0;
    c->limit = query_int(req, "limit", 10);
    if (c->limit < 1) c->limit = 1;
    if (c->limit > 100) c->limit = 100;
    c->offset = query_int(req, "offset", 0);
    if (c->offset < 0) c->offset = 0;
    char raw[16] = "";
    if (kn_http_query_get(req, "region", raw, sizeof raw) > 0 && (!strcmp(raw, "global") || !strcmp(raw, "0"))) {
        c->global = 1; /* the leaderboard's Global tab */
        snprintf(c->region, sizeof c->region, "global");
        c->region_index = 0;
        return c;
    }
    const char *region = query_region(req, "eu");
    snprintf(c->region, sizeof c->region, "%s", region ? region : "");
    c->region_index = ranked_region_index(c->region);
    return c;
}

static void done_ctx(ReqCtx *c, const int status) {
    kn_http_set_header(c->req, "Cache-Control", "no-cache, no-store, must-revalidate");
    brain_reply_json(c->req, status, c->out);
    cJSON_Delete(c->out);
    free(c);
}

static void db_fail(ReqCtx *c, const char *what, kn_db_result *res) {
    log_err(what, res);
    reply_error(c->req, 500, "internal");
    cJSON_Delete(c->out);
    free(c);
}

/* v10 profile {rank, conservative_rating, wins, losses, total_*, highest_*} + KrunkNative extras */
static cJSON *profile_json(const cJSON *row) {
    cJSON *p = cJSON_CreateObject();
    const double cr = row ? kn_json_num(row, "cr", 0) : 0;
    const int wins = (int) kn_json_num(row, "wins", 0), losses = (int) kn_json_num(row, "losses", 0), draws = (int) kn_json_num(row, "draws", 0);
    const int matches = wins + losses + draws;
    cJSON_AddNumberToObject(p, "rank", ranked_rank_of(cr, matches));
    cJSON_AddNumberToObject(p, "conservative_rating", row ? cr : 0);
    cJSON_AddNumberToObject(p, "mmr", ranked_display(cr));
    static const char *const keys[] = {"wins", "losses", "draws", "abandons", "total_kills", "total_deaths", "total_assists", "total_score",
                                       "total_objective_score", "total_damage_done", "total_time_played", "highest_kill_count",
                                       "highest_damage_in_match", "peak_mmr", "last_milestone_claimed", "total_accuracy"};
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++)
        cJSON_AddNumberToObject(p, keys[i], row ? kn_json_num(row, keys[i], 0) : (!strcmp(keys[i], "last_milestone_claimed") ? -1 : 0));
    cJSON_AddNumberToObject(p, "total_matches", matches);
    cJSON_AddNumberToObject(p, "placement_left", matches < RANKED_PLACEMENT_MATCHES ? RANKED_PLACEMENT_MATCHES - matches : 0);
    return p;
}

#define PROFILE_COLS                                                                                                  \
    "cr AS cr__f, wins AS wins__i, losses AS losses__i, draws AS draws__i, abandons AS abandons__i,"                  \
    " total_kills AS total_kills__i, total_deaths AS total_deaths__i, total_assists AS total_assists__i,"             \
    " total_score AS total_score__i, total_objective_score AS total_objective_score__i,"                             \
    " total_damage_done AS total_damage_done__i, total_time_played AS total_time_played__i,"                         \
    " highest_kill_count AS highest_kill_count__i, highest_damage_in_match AS highest_damage_in_match__i,"           \
    " peak_mmr AS peak_mmr__i, last_milestone_claimed AS last_milestone_claimed__i, total_accuracy AS total_accuracy__f"

static void on_profile_rp(kn_db_result *res, void *ud) {
    ReqCtx *c = ud;
    cJSON_AddNumberToObject(c->out, "rankedPoints", kn_db_ok(res) && kn_db_rows(res) > 0 ? (double) kn_db_get_int64(res, 0, 0, 0) : 0);
    done_ctx(c, 200);
}

static void on_profile_row(kn_db_result *res, void *ud) {
    ReqCtx *c = ud;
    if (!kn_db_ok(res)) return db_fail(c, "ranked profile", res);
    cJSON *row = kn_db_rows(res) > 0 ? kn_db_row_to_json(res, 0) : NULL;
    cJSON *profile = profile_json(row);
    cJSON_AddStringToObject(profile, "player_name", c->name); /* v10 profile.player_name */
    cJSON_AddItemToObject(c->out, "profile", profile);
    cJSON_Delete(row);
    static const kn_sql q = {.any = "SELECT balance FROM ranked_points WHERE account_id = $1"};
    char acc[24];
    snprintf(acc, sizeof acc, "%lld", (long long) c->id);
    kn_db_query_sql(R.b->db, &q, KN_DB_ARGS(acc), on_profile_rp, c);
}

static void profile_account(void *ud, kn_http_req *req, const int64_t id, const int level, const char *name) {
    ReqCtx *c = ud;
    (void) req;
    c->id = id;
    snprintf(c->name, sizeof c->name, "%s", name ? name : "");
    c->out = cJSON_CreateObject();
    cJSON_AddNumberToObject(c->out, "season", c->season);
    cJSON_AddNumberToObject(c->out, "currentSeason", current_season());
    cJSON_AddStringToObject(c->out, "region", c->region);
    cJSON_AddNumberToObject(c->out, "regionIndex", c->region_index);
    cJSON_AddNumberToObject(c->out, "level", level);
    cJSON_AddNumberToObject(c->out, "minLevel", R.min_level);
    add_season_maps(c->out, c->season);
    cJSON *ms = cJSON_AddArrayToObject(c->out, "milestones");
    for (int i = 0; c->season != 0 && i < RANKED_MILESTONE_COUNT; i++) { /* "No milestones during Beta Season" */
        cJSON *m = cJSON_CreateObject();
        cJSON_AddNumberToObject(m, "matches", g_ranked_milestones[i].matches);
        cJSON_AddNumberToObject(m, "reward", g_ranked_milestones[i].reward);
        cJSON_AddItemToArray(ms, m);
    }
    static const kn_sql q = {.any = "SELECT " PROFILE_COLS " FROM ranked_profiles WHERE account_id = $1 AND season = {int:$2} AND region = $3"};
    char acc[24], season[16];
    snprintf(acc, sizeof acc, "%lld", (long long) id);
    snprintf(season, sizeof season, "%d", c->season);
    kn_db_query_sql(R.b->db, &q, KN_DB_ARGS(acc, season, c->region), on_profile_row, c);
}

static void h_profile(kn_http_req *req, void *ud) {
    (void) ud;
    if (!db_ready()) return reply_error(req, 503, "RankedUnavailable");
    ReqCtx *c = req_ctx(req);
    if (!c) return;
    if (c->region_index < 0) {
        free(c);
        return reply_error(req, 400, "UnknownRegion");
    }
    with_account(req, 1, profile_account, c);
}

static void on_board_count(kn_db_result *res, void *ud) {
    ReqCtx *c = ud;
    const int64_t total = kn_db_ok(res) && kn_db_rows(res) > 0 ? kn_db_get_int64(res, 0, 0, 0) : 0;
    cJSON_AddNumberToObject(c->out, "totalPages", (double) ((total + c->limit - 1) / c->limit));
    cJSON_AddNumberToObject(c->out, "total", (double) total);
    done_ctx(c, 200);
}

static void on_board(kn_db_result *res, void *ud) {
    ReqCtx *c = ud;
    if (!kn_db_ok(res)) return db_fail(c, "ranked leaderboard", res);
    cJSON *a = cJSON_AddArrayToObject(c->out, "leaderboard");
    for (int i = 0; i < kn_db_rows(res); i++) {
        cJSON *row = kn_db_row_to_json(res, i);
        cJSON *e = cJSON_CreateObject();
        cJSON_AddNumberToObject(e, "position", c->offset + i + 1);
        cJSON_AddStringToObject(e, "player_name", kn_json_str(row, "player_name", ""));
        cJSON_AddStringToObject(e, "clan", kn_json_str(row, "clan", ""));
        cJSON *profile = profile_json(row);
        cJSON_AddStringToObject(profile, "player_name", kn_json_str(row, "player_name", "")); /* v10 reads profile.player_name */
        cJSON_AddItemToObject(e, "profile", profile);
        cJSON_AddItemToArray(a, e);
        cJSON_Delete(row);
    }
    static const kn_sql q = {.any = "SELECT count(*) FROM ranked_profiles WHERE season = {int:$1} AND region = $2"
                                    " AND wins + losses + draws >= " "6"};
    static const kn_sql qg = {.any = "SELECT count(DISTINCT account_id) FROM ranked_profiles WHERE season = {int:$1} AND $2 = $2"
                                     " AND wins + losses + draws >= " "6"};
    char season[16];
    snprintf(season, sizeof season, "%d", c->season);
    kn_db_query_sql(R.b->db, c->global ? &qg : &q, KN_DB_ARGS(season, c->region), on_board_count, c);
}

/* placed players only (Unranked has no position), best conservative rating first */
static void h_leaderboard(kn_http_req *req, void *ud) {
    (void) ud;
    if (!db_ready()) return reply_error(req, 503, "RankedUnavailable");
    ReqCtx *c = req_ctx(req);
    if (!c) return;
    if (c->region_index < 0) {
        free(c);
        return reply_error(req, 400, "UnknownRegion");
    }
    c->out = cJSON_CreateObject();
    cJSON_AddNumberToObject(c->out, "season", c->season);
    cJSON_AddStringToObject(c->out, "region", c->region);
    static const kn_sql q = {.any = "SELECT a.username AS player_name, coalesce(c.clan_name, '') AS clan, " PROFILE_COLS
                                    " FROM ranked_profiles rp JOIN accounts a ON a.id = rp.account_id"
                                    " LEFT JOIN clan_members cm ON cm.account_id = rp.account_id LEFT JOIN clans c ON c.clan_id = cm.clan_id"
                                    " WHERE rp.season = {int:$1} AND rp.region = $2 AND rp.wins + rp.losses + rp.draws >= 6"
                                    " ORDER BY rp.cr DESC, rp.account_id LIMIT {int:$3} OFFSET {int:$4}"};
    /* Global: every placed player once, with the profile of the region where the rating is best */
    static const kn_sql qg = {.any = "SELECT a.username AS player_name, coalesce(c.clan_name, '') AS clan, " PROFILE_COLS
                                     " FROM ranked_profiles rp JOIN accounts a ON a.id = rp.account_id"
                                     " LEFT JOIN clan_members cm ON cm.account_id = rp.account_id LEFT JOIN clans c ON c.clan_id = cm.clan_id"
                                     " WHERE rp.season = {int:$1} AND $2 = $2 AND rp.wins + rp.losses + rp.draws >= 6"
                                     " AND NOT EXISTS (SELECT 1 FROM ranked_profiles r2 WHERE r2.account_id = rp.account_id AND r2.season = rp.season"
                                     " AND r2.wins + r2.losses + r2.draws >= 6 AND (r2.cr > rp.cr OR (r2.cr = rp.cr AND r2.region < rp.region)))"
                                     " ORDER BY rp.cr DESC, rp.account_id LIMIT {int:$3} OFFSET {int:$4}"};
    char season[16], limit[16], offset[16];
    snprintf(season, sizeof season, "%d", c->season);
    snprintf(limit, sizeof limit, "%d", c->limit);
    snprintf(offset, sizeof offset, "%d", c->offset * c->limit); /* v10: offset = page - 1 */
    c->offset *= c->limit;
    kn_db_query_sql(R.b->db, c->global ? &qg : &q, KN_DB_ARGS(season, c->region, limit, offset), on_board, c);
}

static void on_history_count(kn_db_result *res, void *ud) {
    ReqCtx *c = ud;
    const int64_t total = kn_db_ok(res) && kn_db_rows(res) > 0 ? kn_db_get_int64(res, 0, 0, 0) : 0;
    cJSON *mh = cJSON_GetObjectItemCaseSensitive(c->out, "matchHistory");
    cJSON_AddNumberToObject(mh, "totalPages", (double) ((total + c->limit - 1) / c->limit));
    done_ctx(c, 200);
}

static void history_count(ReqCtx *c) {
    static const kn_sql q = {.any = "SELECT count(*) FROM mm_ranked_match_players mp JOIN mm_ranked_matches m ON m.match_id = mp.match_id"
                                    " WHERE mp.account_id = $1 AND m.season = {int:$2} AND m.region = $3 AND m.status = 'done'"};
    char acc[24], season[16];
    snprintf(acc, sizeof acc, "%lld", (long long) c->id);
    snprintf(season, sizeof season, "%d", c->season);
    kn_db_query_sql(R.b->db, &q, KN_DB_ARGS(acc, season, c->region), on_history_count, c);
}

/* v10's match entry: historyEntries (every player of the match: player_name, team 1 Alpha / 2
 * Bravo, score, kills, assists, deaths, objective_score, damage_done, mmr_change, rank) */
static void on_history_entries(kn_db_result *res, void *ud) {
    ReqCtx *c = ud;
    if (!kn_db_ok(res)) return db_fail(c, "ranked history players", res);
    cJSON *players = kn_db_result_to_json(res);
    cJSON *matches = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(c->out, "matchHistory"), "matches");
    cJSON *m;
    cJSON_ArrayForEach(m, matches) {
        const char *id = kn_json_str(m, "match_id", "");
        cJSON *result = cJSON_Parse(kn_json_str(m, "result", "{}"));
        cJSON_DeleteItemFromObjectCaseSensitive(m, "result");
        cJSON *entries = cJSON_AddArrayToObject(m, "historyEntries");
        const cJSON *p;
        cJSON_ArrayForEach(p, players) {
            if (strcmp(kn_json_str(p, "match_id", ""), id)) continue;
            const int64_t acc = kn_json_int(p, "account_id", 0);
            cJSON *e = cJSON_CreateObject();
            cJSON_AddNumberToObject(e, "player_id", (double) acc);
            cJSON_AddStringToObject(e, "player_name", kn_json_str(p, "player_name", ""));
            cJSON_AddNumberToObject(e, "team", (double) (kn_json_int(p, "team", 0) + 1));
            cJSON_AddNumberToObject(e, "score", kn_json_num(p, "score", 0));
            cJSON_AddNumberToObject(e, "kills", kn_json_num(p, "kills", 0));
            cJSON_AddNumberToObject(e, "assists", kn_json_num(p, "assists", 0));
            cJSON_AddNumberToObject(e, "deaths", kn_json_num(p, "deaths", 0));
            double objective = 0;
            const cJSON *rp;
            cJSON_ArrayForEach(rp, cJSON_GetObjectItemCaseSensitive(result, "players")) {
                if (kn_json_int(rp, "accountId", -1) == acc) objective = kn_json_num(rp, "objective", 0);
            }
            cJSON_AddNumberToObject(e, "objective_score", objective);
            cJSON_AddNumberToObject(e, "damage_done", kn_json_num(p, "damage_done", 0));
            const double before = kn_json_num(p, "rating_before", 0), after = kn_json_num(p, "rating_after", before);
            cJSON_AddNumberToObject(e, "mmr_change", after - before);
            cJSON_AddNumberToObject(e, "rank", ranked_rank_of(kn_json_num(p, "cr", 0), (int) kn_json_num(p, "matches", 0)));
            if (acc == c->id) cJSON_AddNumberToObject(m, "rank", kn_json_num(e, "rank", 0));
            cJSON_AddItemToArray(entries, e);
        }
        /* the teams' scores (the game server's teamScores; older results: the winner 1 - 0) */
        const cJSON *ts = cJSON_GetObjectItemCaseSensitive(result, "teamScores");
        const int winner = (int) kn_json_int(result, "winner", -1);
        double alpha = winner == 0 ? 1 : 0, bravo = winner == 1 ? 1 : 0;
        if (cJSON_GetArraySize(ts) == 2) {
            alpha = cJSON_GetArrayItem(ts, 0)->valuedouble;
            bravo = cJSON_GetArrayItem(ts, 1)->valuedouble;
            /* a forfeit / abandon decides the match whatever the scores */
            if (winner == 0 && alpha <= bravo) alpha = bravo + 1;
            if (winner == 1 && bravo <= alpha) bravo = alpha + 1;
        }
        cJSON_AddNumberToObject(m, "score_alpha", alpha);
        cJSON_AddNumberToObject(m, "score_bravo", bravo);
        cJSON_Delete(result);
    }
    cJSON_Delete(players);
    history_count(c);
}

static void on_history(kn_db_result *res, void *ud) {
    ReqCtx *c = ud;
    if (!kn_db_ok(res)) return db_fail(c, "ranked history", res);
    cJSON *mh = cJSON_AddObjectToObject(c->out, "matchHistory");
    cJSON *a = cJSON_AddArrayToObject(mh, "matches");
    for (int i = 0; i < kn_db_rows(res); i++) {
        cJSON *row = kn_db_row_to_json(res, i);
        const int map = (int) kn_json_num(row, "map", -1);
        const MapCatalogEntry *m = map >= 0 ? map_catalog_at((size_t) map) : NULL;
        const char *pool = ranked_pool_map_name(map);
        cJSON_AddStringToObject(row, "map_name", pool ? pool : m ? m->name : "");
        const double before = kn_json_num(row, "rating_before", 0), after = kn_json_num(row, "rating_after", before);
        cJSON_AddNumberToObject(row, "mmr_change", after - before);
        cJSON_AddBoolToObject(row, "is_ranked", 1);
        cJSON_AddItemToArray(a, row);
    }
    if (!kn_db_rows(res)) return history_count(c);
    /* every player of this page's matches (a derived table: MySQL takes no LIMIT in IN) */
    static const kn_sql q = {.any = "SELECT mp.match_id, mp.account_id AS account_id__i, mp.username AS player_name, mp.team AS team__i, mp.kills AS kills__i,"
                                    " mp.deaths AS deaths__i, mp.assists AS assists__i, mp.score AS score__i, mp.damage_done AS damage_done__i,"
                                    " mp.rating_before AS rating_before__i, mp.rating_after AS rating_after__i, coalesce(rp.cr, 0) AS cr__f,"
                                    " coalesce(rp.wins + rp.losses + rp.draws, 0) AS matches__i"
                                    " FROM mm_ranked_match_players mp JOIN (SELECT m.match_id FROM mm_ranked_match_players me"
                                    " JOIN mm_ranked_matches m ON m.match_id = me.match_id WHERE me.account_id = $1 AND m.season = {int:$2} AND m.region = $3"
                                    " AND m.status = 'done' ORDER BY m.finished_at DESC LIMIT {int:$4} OFFSET {int:$5}) pg ON pg.match_id = mp.match_id"
                                    " LEFT JOIN ranked_profiles rp ON rp.account_id = mp.account_id AND rp.season = {int:$2} AND rp.region = $3"
                                    " ORDER BY mp.match_id, mp.team, mp.score DESC"};
    char acc[24], season[16], limit[16], offset[16];
    snprintf(acc, sizeof acc, "%lld", (long long) c->id);
    snprintf(season, sizeof season, "%d", c->season);
    snprintf(limit, sizeof limit, "%d", c->limit);
    snprintf(offset, sizeof offset, "%d", c->offset * c->limit);
    kn_db_query_sql(R.b->db, &q, KN_DB_ARGS(acc, season, c->region, limit, offset), on_history_entries, c);
}

static void history_account(void *ud, kn_http_req *req, const int64_t id, const int level, const char *name) {
    ReqCtx *c = ud;
    (void) req;
    (void) level;
    (void) name;
    c->id = id;
    c->out = cJSON_CreateObject();
    static const kn_sql q = {.any = "SELECT m.match_id, {epoch_ms:m.finished_at} AS date__i, m.map_index AS map__i, m.mode_index AS mode__i,"
                                    " m.region, m.season AS season__i, mp.team AS team__i, mp.victory AS victory__i, mp.kills AS kills__i,"
                                    " mp.deaths AS deaths__i, mp.assists AS assists__i, mp.score AS score__i, mp.damage_done AS damage_done__i,"
                                    " mp.rating_before AS rating_before__i, mp.rating_after AS rating_after__i, mp.abandoned AS abandoned__b,"
                                    " mp.rp AS rp__i, coalesce({text:m.result}, '{}') AS result"
                                    " FROM mm_ranked_match_players mp JOIN mm_ranked_matches m ON m.match_id = mp.match_id"
                                    " WHERE mp.account_id = $1 AND m.season = {int:$2} AND m.region = $3 AND m.status = 'done'"
                                    " ORDER BY m.finished_at DESC LIMIT {int:$4} OFFSET {int:$5}"};
    char acc[24], season[16], limit[16], offset[16];
    snprintf(acc, sizeof acc, "%lld", (long long) id);
    snprintf(season, sizeof season, "%d", c->season);
    snprintf(limit, sizeof limit, "%d", c->limit);
    snprintf(offset, sizeof offset, "%d", c->offset * c->limit);
    kn_db_query_sql(R.b->db, &q, KN_DB_ARGS(acc, season, c->region, limit, offset), on_history, c);
}

static void h_history(kn_http_req *req, void *ud) {
    (void) ud;
    if (!db_ready()) return reply_error(req, 503, "RankedUnavailable");
    ReqCtx *c = req_ctx(req);
    if (!c) return;
    if (c->region_index < 0) {
        free(c);
        return reply_error(req, 400, "UnknownRegion");
    }
    with_account(req, 1, history_account, c);
}

static void on_proc_reply(kn_db_result *res, void *ud) {
    ReqCtx *c = ud;
    if (!kn_db_ok(res)) {
        const char *msg = kn_db_user_message(res);
        if (!msg) log_err("ranked call", res);
        c->out = cJSON_CreateObject();
        cJSON_AddBoolToObject(c->out, "success", 0);
        cJSON_AddStringToObject(c->out, "error", msg ? msg : "internal");
        return done_ctx(c, msg ? 400 : 500);
    }
    const char *t = kn_db_rows(res) > 0 ? kn_db_get(res, 0, 0) : "{}";
    c->out = kn_json_parse(t, strlen(t));
    done_ctx(c, 200);
}

static void claim_account(void *ud, kn_http_req *req, const int64_t id, const int level, const char *name) {
    ReqCtx *c = ud;
    (void) req;
    (void) level;
    (void) name;
    char acc[24], season[16], region[8];
    snprintf(acc, sizeof acc, "%lld", (long long) id);
    snprintf(season, sizeof season, "%d", c->season);
    snprintf(region, sizeof region, "%d", c->region_index);
    kn_db_call(R.b->db, &P_RK_CLAIM, KN_DB_ARGS(acc, season, region), on_proc_reply, c);
}

static void h_claim(kn_http_req *req, void *ud) {
    (void) ud;
    if (!db_ready()) return reply_error(req, 503, "RankedUnavailable");
    ReqCtx *c = req_ctx(req);
    if (!c) return;
    c->season = current_season(); /* milestones of the running season only */
    if (c->region_index < 0) {
        free(c);
        return reply_error(req, 400, "UnknownRegion");
    }
    with_account(req, 1, claim_account, c);
}

/* restricted_by "mmr" (v10:111314, 112369): the item's MMR window against the account's peak MMR over
 * its seasons, the rating db_ranked.c run_rk_purchase checks, so the shop's lock and the purchase's
 * refusal always agree (KrunkNative's choice: v10's server rule is not in the bundle). */
static void on_shop(kn_db_result *res, void *ud) {
    ReqCtx *c = ud;
    if (!kn_db_ok(res)) return db_fail(c, "ranked shop", res);
    cJSON *skins = kn_db_result_to_json(res);
    cJSON *s;
    cJSON_ArrayForEach(s, skins) {
        const int peak = (int) kn_json_num(s, "peak", 0);
        const cJSON *lo = cJSON_GetObjectItemCaseSensitive(s, "min_mmr"), *hi = cJSON_GetObjectItemCaseSensitive(s, "max_mmr");
        const int restricted = (cJSON_IsNumber(lo) && peak < lo->valueint) || (cJSON_IsNumber(hi) && peak > hi->valueint);
        cJSON_DeleteItemFromObjectCaseSensitive(s, "peak");
        if (restricted) cJSON_AddStringToObject(s, "restricted_by", "mmr");
        else cJSON_AddNullToObject(s, "restricted_by");
    }
    c->out = cJSON_CreateObject();
    cJSON_AddItemToObject(c->out, "skins", skins);
    done_ctx(c, 200);
}

static void shop_account(void *ud, kn_http_req *req, const int64_t id, const int level, const char *name) {
    ReqCtx *c = ud;
    (void) req;
    (void) level;
    (void) name;
    static const kn_sql q = {.any = "SELECT s.id AS id__i, s.skin_id AS skin_id__i, s.price AS price__i, {epoch_ms:s.expires_at} AS expires_at__i,"
                                    " s.max_purchase AS max_purchase__i, coalesce(p.count, 0) AS purchase_count__i,"
                                    " s.min_mmr AS min_mmr__i, s.max_mmr AS max_mmr__i,"
                                    " (SELECT coalesce(max(r.peak_mmr), 0) FROM ranked_profiles r WHERE r.account_id = $2) AS peak__i"
                                    " FROM ranked_shop s LEFT JOIN ranked_shop_purchases p ON p.shop_id = s.id AND p.account_id = $1"
                                    " WHERE s.expires_at IS NULL OR s.expires_at > {now} ORDER BY s.id"};
    char acc[24];
    snprintf(acc, sizeof acc, "%lld", (long long) id);
    kn_db_query_sql(R.b->db, &q, KN_DB_ARGS(acc, acc), on_shop, c);
}

static void h_shop(kn_http_req *req, void *ud) {
    (void) ud;
    if (!db_ready()) return reply_error(req, 503, "RankedUnavailable");
    ReqCtx *c = req_ctx(req);
    if (c) with_account(req, 0, shop_account, c);
}

static void purchase_account(void *ud, kn_http_req *req, const int64_t id, const int level, const char *name) {
    ReqCtx *c = ud;
    (void) req;
    (void) level;
    (void) name;
    char acc[24], season[16];
    snprintf(acc, sizeof acc, "%lld", (long long) id);
    snprintf(season, sizeof season, "%d", current_season());
    kn_db_call(R.b->db, &P_RK_PURCHASE, KN_DB_ARGS(acc, c->shop_id, season), on_proc_reply, c);
}

static void h_purchase(kn_http_req *req, void *ud) {
    (void) ud;
    if (!db_ready()) return reply_error(req, 503, "RankedUnavailable");
    ReqCtx *c = req_ctx(req);
    if (!c) return;
    if (kn_http_query_get(req, "id", c->shop_id, sizeof c->shop_id) <= 0) {
        cJSON *body = kn_http_body_json(req); /* v10: POST {skin_id: id} */
        snprintf(c->shop_id, sizeof c->shop_id, "%lld", (long long) kn_json_int(body, "skin_id", kn_json_int(body, "id", 0)));
        cJSON_Delete(body);
    }
    with_account(req, 1, purchase_account, c);
}

static void on_connection(kn_db_result *res, void *ud) {
    ReqCtx *c = ud;
    if (!kn_db_ok(res)) return db_fail(c, "ranked connection", res);
    c->out = cJSON_CreateObject();
    const char *t = kn_db_rows(res) > 0 ? kn_db_get(res, 0, 0) : "";
    cJSON *as = t[0] ? kn_json_parse(t, strlen(t)) : NULL;
    if (cJSON_IsObject(as)) {
        cJSON_AddStringToObject(c->out, "serverUrl", kn_json_str(as, "connection", ""));
        cJSON_AddItemToObject(c->out, "assignment", as);
    } else {
        cJSON_Delete(as);
        cJSON_AddNullToObject(c->out, "serverUrl");
    }
    done_ctx(c, 200);
}

static void connection_account(void *ud, kn_http_req *req, const int64_t id, const int level, const char *name) {
    ReqCtx *c = ud;
    (void) req;
    (void) level;
    (void) name;
    static const kn_sql q = {.any = "SELECT mp.assignment FROM mm_ranked_match_players mp JOIN mm_ranked_matches m ON m.match_id = mp.match_id"
                                    " WHERE mp.account_id = $1 AND m.status = 'live' AND NOT mp.abandoned AND mp.assignment IS NOT NULL"
                                    " ORDER BY m.started_at DESC LIMIT 1"};
    char acc[24];
    snprintf(acc, sizeof acc, "%lld", (long long) id);
    kn_db_query_sql(R.b->db, &q, KN_DB_ARGS(acc), on_connection, c);
}

static void h_connection(kn_http_req *req, void *ud) {
    (void) ud;
    if (!db_ready()) return reply_error(req, 503, "RankedUnavailable");
    ReqCtx *c = req_ctx(req);
    if (c) with_account(req, 1, connection_account, c);
}

static void h_info(kn_http_req *req, void *ud) {
    (void) ud;
    const int season = current_season();
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "enabled", db_ready());
    cJSON_AddNumberToObject(o, "season", season);
    cJSON_AddNumberToObject(o, "minLevel", R.min_level);
    cJSON_AddNumberToObject(o, "teamSize", R.team_size);
    cJSON_AddNumberToObject(o, "placementMatches", RANKED_PLACEMENT_MATCHES);
    add_season_maps(o, season);
    cJSON *regions = cJSON_AddArrayToObject(o, "regions");
    for (int i = 2; i <= 4; i++) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "code", ranked_region_code(i));
        cJSON_AddNumberToObject(r, "index", i);
        cJSON_AddStringToObject(r, "name", ranked_region_name(ranked_region_code(i)));
        cJSON_AddItemToArray(regions, r);
    }
    brain_reply_json(req, 200, o);
    cJSON_Delete(o);
}

/* the v7.1.4 party queue is gone: an old client gets a clear refusal */
static void h_v7_queue(kn_http_req *req, void *ud) {
    (void) ud;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "status", "ClientOutdated");
    cJSON_AddStringToObject(o, "error", "ClientOutdated");
    brain_reply_json(req, 410, o);
    cJSON_Delete(o);
}

/* ================================================================ matcher */

typedef struct HostMatch {
    char match_id[40];
    char region[8];
    cJSON *teams;                 /* [[{accountId, name, mmr}], [...]] (owned) */
} HostMatch;

static void on_ranked_hosted(void *ud, const char *err, const cJSON *reply) {
    HostMatch *h = ud;
    Brain *b = R.b;
    if (err || !reply || !b->db) {
        KN_WARN(MOD, "match %s: hosting failed (%s); players back in the queue", h->match_id, err ? err : "?");
        if (b->db) kn_db_call(b->db, &P_RK_MATCH_FAILED, KN_DB_ARGS(h->match_id, err ? err : "host failed"), on_ignore, "match failed");
    } else {
        const char *gid = kn_json_str(reply, "gameId", "");
        char node_id[MM_ID_MAX];
        snprintf(node_id, sizeof(node_id), "%.*s", (int) strcspn(gid, ":"), gid);
        MmNode *n = registry_node(b->registry, node_id);
        const char *host = kn_json_str(reply, "host", "");
        const int port = (int) kn_json_num(reply, "port", 0);
        const char *web = kn_json_str(reply, "webUrl", NULL);
        cJSON *assignments = cJSON_CreateObject();
        const cJSON *team, *pl;
        cJSON_ArrayForEach(team, h->teams) {
            cJSON_ArrayForEach(pl, team) {
                const int64_t acc = (int64_t) kn_json_num(pl, "accountId", 0);
                cJSON *seek = n ? brain_seek_reply_ex(b, n, gid, acc, kn_json_str(pl, "name", ""), 0, 0) : NULL;
                /* v10 assignment {connection: "host:port"} + what a KrunkNative client joins with */
                cJSON *as = cJSON_CreateObject();
                char conn[160];
                snprintf(conn, sizeof conn, "%s:%d", host, port);
                cJSON_AddStringToObject(as, "connection", conn);
                cJSON_AddStringToObject(as, "gameId", gid);
                cJSON_AddStringToObject(as, "host", host);
                cJSON_AddNumberToObject(as, "port", port);
                if (web) cJSON_AddStringToObject(as, "webUrl", web);
                cJSON_AddStringToObject(as, "clientId", seek ? kn_json_str(seek, "clientId", "") : "");
                cJSON_AddStringToObject(as, "matchId", h->match_id);
                cJSON_AddStringToObject(as, "region", h->region);
                char key[24];
                snprintf(key, sizeof key, "%lld", (long long) acc);
                cJSON_AddItemToObject(assignments, key, as);
                cJSON_Delete(seek);
            }
        }
        char *as = kn_json_print(assignments);
        kn_db_call(b->db, &P_RK_MATCH_READY, KN_DB_ARGS(h->match_id, gid, as ? as : "{}"), on_ignore, "match ready");
        KN_INFO(MOD, "match %s (%s) is live on %s", h->match_id, h->region, gid);
        free(as);
        cJSON_Delete(assignments);
    }
    cJSON_Delete(h->teams);
    free(h);
}

/* The game server's "ranked" host config (unchanged shape: server/src/accounts.c acc_ranked_setup):
 * a comp lobby (config.hostTypes[1]) for the roster, Hardpoint on the season's map. */
static void host_match(const cJSON *m) {
    HostMatch *h = calloc(1, sizeof(*h));
    if (!h) return;
    snprintf(h->match_id, sizeof(h->match_id), "%s", kn_json_str(m, "matchId", ""));
    snprintf(h->region, sizeof(h->region), "%s", kn_json_str(m, "region", ""));
    h->teams = cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(m, "teams"), 1);
    const int map = (int) kn_json_num(m, "map", RANKED_ALWAYS_MAP);
    cJSON *cfg = cJSON_CreateObject();
    cJSON_AddBoolToObject(cfg, "ranked", 1);
    cJSON_AddStringToObject(cfg, "matchId", h->match_id);
    cJSON_AddStringToObject(cfg, "queue", "ranked");
    cJSON_AddStringToObject(cfg, "ruleset", RANKED_RULESET);
    cJSON_AddNumberToObject(cfg, "season", kn_json_num(m, "season", 0));
    cJSON_AddStringToObject(cfg, "rankedRegion", h->region);
    cJSON_AddNumberToObject(cfg, "teamSize", R.team_size);
    cJSON *modes = cJSON_AddArrayToObject(cfg, "modes");
    cJSON_AddItemToArray(modes, cJSON_CreateNumber(MODE_ID_POINT));
    cJSON_AddNumberToObject(cfg, "mode", MODE_ID_POINT);
    cJSON *maps = cJSON_AddArrayToObject(cfg, "maps");
    cJSON_AddItemToArray(maps, cJSON_CreateNumber(map));
    cJSON_AddBoolToObject(cfg, "private", 1);
    cJSON_AddNumberToObject(cfg, "startGraceS", R.start_grace_s);
    cJSON_AddItemToObject(cfg, "teams", cJSON_Duplicate(h->teams, 1));
    const int ri = ranked_region_index(h->region);
    const char *host_region = ri >= 2 && ri <= 4 && R.host_region[ri - 2][0] ? R.host_region[ri - 2] : NULL;
    KN_INFO(MOD, "match %s formed (%dv%d, %s, map %d): hosting", h->match_id, R.team_size, R.team_size, h->region, map);
    /* brain.ranked.host_regions maps a ranked region to game-server regions; unmapped = any node
     * (this brain's own first: registry_pick_host_node) */
    /* ranked is Krunker Box Office's: base nodes only (flavor NULL) */
    brain_host_game_ex(R.b, host_region, cfg, 0, NULL, 0, NULL, on_ranked_hosted, h);
    cJSON_Delete(cfg);
}

static void on_matches(kn_db_result *res, void *ud) {
    (void) ud;
    R.matching = 0;
    if (!kn_db_ok(res)) {
        log_err("ranked matcher", res);
        return;
    }
    if (kn_db_rows(res) < 1 || kn_db_isnull(res, 0, 0)) return;
    const char *t = kn_db_get(res, 0, 0);
    cJSON *a = kn_json_parse(t, strlen(t));
    const cJSON *m;
    cJSON_ArrayForEach(m, a) host_match(m);
    cJSON_Delete(a);
}

static void on_expired(kn_db_result *res, void *ud) {
    (void) ud;
    R.expiring = 0;
    if (!kn_db_ok(res)) log_err("ranked expire", res);
}

static void on_closed(kn_db_result *res, void *ud) {
    const int season = (int) (intptr_t) ud;
    R.closing = 0;
    if (!kn_db_ok(res)) return log_err("ranked season close", res);
    if (kn_db_rows(res) < 1 || kn_db_isnull(res, 0, 0)) return; /* another brain holds it: retried */
    R.closed_season = season;
    const char *t = kn_db_get(res, 0, 0);
    cJSON *o = kn_json_parse(t, strlen(t));
    if (kn_json_int(o, "accounts", 0) > 0)
        KN_INFO(MOD, "season %d closed: Ranked Points for %lld account(s)", season, (long long) kn_json_int(o, "accounts", 0));
    cJSON_Delete(o);
}

static void tick(kn_loop *loop, void *ud) {
    (void) loop;
    (void) ud;
    if (!db_ready() || R.b->draining) return;
    R.ticks++;
    char a[16], b[16], c[16];
    if (!R.expiring && R.ticks % 2 == 0) {
        snprintf(a, sizeof a, "%d", R.poll_timeout_s);
        snprintf(b, sizeof b, "%d", R.creating_timeout_s);
        snprintf(c, sizeof c, "%d", R.match_timeout_s);
        R.expiring = 1;
        kn_db_call(R.b->db, &P_RK_EXPIRE, KN_DB_ARGS(a, b, c), on_expired, NULL);
    }
    /* end-of-month conversion of the previous season (once per brain start and season; the
     * payout table keeps it once per account) */
    const int season = current_season();
    if (!R.closing && season > 1 && R.closed_season < season - 1 && R.ticks % 30 == 1) {
        char s[16];
        snprintf(s, sizeof s, "%d", season - 1);
        R.closing = 1;
        kn_db_call(R.b->db, &P_RK_SEASON_CLOSE, KN_DB_ARGS(s), on_closed, (void *) (intptr_t) (season - 1));
    }
    if (R.matching) return;
    int maps[RANKED_SEASON_MAPS];
    ranked_season_maps(season, maps);
    char list[96] = "";
    for (int i = 0; i < RANKED_SEASON_MAPS; i++) {
        /* KrunkNative's catalogue lacks some v10 pool maps: only playable ones are picked */
        if (maps[i] >= MAP_CATALOG_OFFICIAL_COUNT || !map_catalog_at((size_t) maps[i])) continue;
        const size_t len = strlen(list);
        snprintf(list + len, sizeof list - len, "%s%d", len ? "," : "", maps[i]);
    }
    char s[16], size[8], w[24], g[24], mx[24];
    snprintf(s, sizeof s, "%d", season);
    snprintf(size, sizeof size, "%d", R.team_size);
    snprintf(w, sizeof w, "%g", R.window);
    snprintf(g, sizeof g, "%g", R.window_growth);
    snprintf(mx, sizeof mx, "%g", R.window_max);
    R.matching = 1;
    if (kn_db_call(R.b->db, &P_RK_MATCH_RUN, KN_DB_ARGS(R.b->id, s, size, w, g, mx, list, RANKED_RULESET), on_matches, NULL)) R.matching = 0;
}

/* ================================================================ results */

typedef struct ResultCtx {
    RankedResultCb cb;
    void *ud;
    char match_id[64];
} ResultCtx;

static void on_finished(kn_db_result *res, void *ud) {
    ResultCtx *r = ud;
    if (!kn_db_ok(res) || kn_db_rows(res) < 1) {
        log_err("ranked finish", res);
        if (r->cb) r->cb(r->ud, "db error", NULL);
        free(r);
        return;
    }
    const char *t = kn_db_get(res, 0, 0);
    cJSON *ratings = kn_json_parse(t, strlen(t));
    if (!cJSON_IsArray(ratings)) {
        cJSON_Delete(ratings);
        ratings = cJSON_CreateArray();
    }
    if (!cJSON_GetArraySize(ratings)) KN_INFO(MOD, "match %s: result ignored (unknown or already closed)", r->match_id);
    else KN_INFO(MOD, "match %s rated (%d player(s))", r->match_id, cJSON_GetArraySize(ratings));
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddBoolToObject(reply, "ok", 1);
    cJSON_AddItemToObject(reply, "ratings", ratings);
    if (r->cb) r->cb(r->ud, NULL, reply);
    cJSON_Delete(reply);
    free(r);
}

/* internal WS "ranked-result" {matchId, winner, players [{accountId, team, score, kills, deaths,
 * assists, damage, objective, timeMs, reward}], abandoned [ids]} */
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
    cJSON *stored = cJSON_Duplicate(frame, 1);
    cJSON_DeleteItemFromObjectCaseSensitive(stored, "type");
    cJSON_DeleteItemFromObjectCaseSensitive(stored, "rid");
    char *s = kn_json_print(stored);
    cJSON_Delete(stored);
    char cd[16];
    snprintf(cd, sizeof cd, "%d", R.cooldown_s);
    kn_db_call(b->db, &P_RK_MATCH_FINISH, KN_DB_ARGS(r->match_id, s ? s : "{}", cd), on_finished, r);
    free(s);
}

/* ================================================================ module */

static void route(Brain *b, const char *method, const char *name, kn_http_handler h) {
    char path[96];
    snprintf(path, sizeof(path), "/mm/ranked/%s", name);
    kn_http_route(b->http, method, path, h, b);
}

int ranked_init(Brain *b) {
    const struct kn_config *cfg = b->cfg;
    R.b = b;
    R.enabled = kn_config_bool(cfg, "brain.ranked.enabled", 1);
    R.min_level = (int) kn_config_int(cfg, "brain.ranked.min_level", RANKED_MIN_LEVEL);
    R.team_size = (int) kn_config_int(cfg, "brain.ranked.team_size", RANKED_TEAM_SIZE);
    if (R.team_size < 1) R.team_size = 1;
    if (R.team_size > RANKED_TEAM_SIZE) R.team_size = RANKED_TEAM_SIZE;
    R.season_override = (int) kn_config_int(cfg, "brain.ranked.season", -1);
    /* the game-server region each ranked region hosts on: brain.ranked.host_regions {"na": "VA"} */
    const cJSON *hr = kn_config_get(cfg, "brain.ranked.host_regions");
    for (int i = 0; i < RANKED_REGION_COUNT; i++) {
        const char *v = kn_json_str(hr, ranked_region_code(i + 2), "");
        snprintf(R.host_region[i], sizeof R.host_region[i], "%s", v);
    }
    R.poll_timeout_s = (int) kn_config_int(cfg, "brain.ranked.poll_timeout_s", 30);
    R.creating_timeout_s = (int) kn_config_int(cfg, "brain.ranked.creating_timeout_s", 60);
    R.match_timeout_s = (int) kn_config_int(cfg, "brain.ranked.match_timeout_s", 1800);
    R.window = kn_config_num(cfg, "brain.ranked.rating_window", 150);
    R.window_growth = kn_config_num(cfg, "brain.ranked.window_growth", 10);
    R.window_max = kn_config_num(cfg, "brain.ranked.window_max", 1000);
    R.cooldown_s = (int) kn_config_int(cfg, "brain.ranked.cooldown_s", RANKED_COOLDOWN_MS / 1000);
    R.start_grace_s = (int) kn_config_int(cfg, "brain.ranked.start_grace_s", RANKED_TIME_FIRST_CONNECT_MS / 1000);
    route(b, "GET", "queue", h_queue);
    route(b, "GET", "poll", h_poll);
    route(b, "GET", "leave", h_leave);
    route(b, "GET", "connection", h_connection);
    route(b, "GET", "info", h_info);
    route(b, "GET", "profile", h_profile);
    route(b, "GET", "leaderboard", h_leaderboard);
    route(b, "GET", "history", h_history);
    route(b, NULL, "claim-milestone", h_claim);
    route(b, "GET", "shop", h_shop);
    route(b, NULL, "purchase", h_purchase);
    kn_http_route(b->http, NULL, "/mm/queue/*", h_v7_queue, b);
    kn_http_route(b->http, NULL, "/queue/*", h_v7_queue, b);
    if (!b->db || !R.enabled) {
        KN_INFO(MOD, "ranked disabled (%s)", b->db ? "brain.ranked.enabled=false" : "no database");
        return 0;
    }
    R.timer = kn_loop_add_timer(b->loop, 1000, 1000, tick, NULL);
    KN_INFO(MOD, "ranked (v10) on /mm/ranked/* (%dv%d, min level %d, season %d)", R.team_size, R.team_size, R.min_level, current_season());
    return 0;
}

void ranked_shutdown(Brain *b) {
    if (R.timer) kn_loop_cancel_timer(b->loop, R.timer);
    R.timer = 0;
    R.enabled = 0;
}
