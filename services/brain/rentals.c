/*
 * services/brain/rentals.c - Krunker v10.0.7's Rent Dedicated Server (hostTypes[2], buyServ) on Box Office:
 * the rental pool, the window's two social frames, the agent API kn_rentd drives the game host with, and
 * the timer that expires rentals and keeps the startup guarantee. Spec: doc/v10/rented-servers.md.
 *
 * Social socket (Krunker's game-socket labels and argument order; KrunkNative's client talks to kn_brain):
 *   ["dsrvl"]                                 -> ["dsrvl", 0, rows]        (deob-10.0.7.js:219866, dediRes :219811)
 *   ["rdsrv", id, {typ, mid, gnm, cr, key}, update]                     (rentDediServer :204642-204668)
 *       -> ["dsrvl", 1, text, keepHidden], then ["dsrvl", 0, rows] after a success
 * A row is {ID, REG, TRM, OID, SVN, SID, CONF} (:219857-219935): TRM ms left (0 = free), CONF the game as
 * JSON text {gid, mn, typ} (+ Box Office's cr / key: the published map's creator, the catalogue key).
 * cr and key are additions to Krunker's request object; a client without them hosts by name.
 *
 * Prices and durations: config.dediPrices [7500, 14000, 20000] KR for config.dediDurat [30, 60, 90] days
 * (:18707-18708, :19647-19648). dediSlots 3 (:19649) caps the servers one account rents at once
 * (brain.rentals.max_per_account), dediTimer 600000 ms (:19650) is the startup guarantee
 * (brain.rentals.start_timeout_s): Box Office's readings of two values Krunker's client never reads.
 *
 * Agent API (Bearer brain.internal_secret, mm_internal_authorized):
 *   GET  /mm/internal/rentals?host=H           -> {now, slots: [{id, port, name, region, public_host,
 *                                                  generation, run, map, creator, key, game_id}]}
 *   POST /mm/internal/rentals/status {host, slots: [{id, generation, state}]}
 * Owner: svc-accounts (needs the database).
 */
#include "acct_internal.h"
#include "brain_internal.h"
#include "db_procs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "knsvc/config.h"
#include "knsvc/http.h"
#include "knsvc/json.h"
#include "knsvc/log.h"
#include "knsvc/loop.h"

#define MOD "rentals"
#define P(i) (n > (i) ? p[i] : NULL)
#define PI(i) pint(P(i))
#define RENT_TYPES 3

/* config.dediDurat / config.dediPrices (deob-10.0.7.js:18707-18708) */
static const int64_t k_days[RENT_TYPES] = {30, 60, 90};
static const int64_t k_price[RENT_TYPES] = {7500, 14000, 20000};

/* what a rent request turned into (the proc's "kind"): the line the window shows */
enum { RENT_RENTED = 1, RENT_EXTENDED = 2, RENT_UPDATED = 3 };

static struct {
    Brain *b;
    int enabled;
    uint64_t timer;
    int64_t max_per_account, start_timeout_s, agent_timeout_s;
} R;

static int64_t pint(const char *s) { return s && *s ? strtoll(s, NULL, 10) : 0; }
static int64_t now_s(void) { return kn_wall_ms() / 1000; }
static int db_ready(void) { return R.b && R.b->db && R.enabled; }

int64_t rentals_price(int typ) { return typ >= 0 && typ < RENT_TYPES ? k_price[typ] : -1; }
int64_t rentals_days(int typ) { return typ >= 0 && typ < RENT_TYPES ? k_days[typ] : -1; }

/* ------------------------------------------------------------------ procs */

/* the row as the window's {ID, REG, TRM, OID, SVN, SID, CONF} */
static cJSON *window_row(const cJSON *r, int64_t now) {
    const int64_t owner = kn_json_int(r, "owner_id", 0), expires = kn_json_int(r, "expires_s", 0);
    const int live = owner > 0 && expires > now;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "ID", (double) kn_json_int(r, "id", 0));
    cJSON_AddNumberToObject(o, "REG", (double) kn_json_int(r, "region", 0));
    cJSON_AddNumberToObject(o, "TRM", live ? (double) (expires - now) * 1000.0 : 0);
    if (live) cJSON_AddNumberToObject(o, "OID", (double) owner);
    else cJSON_AddNullToObject(o, "OID");
    cJSON_AddStringToObject(o, "SVN", kn_json_str(r, "svn", ""));
    cJSON_AddNumberToObject(o, "SID", (double) kn_json_int(r, "sid", 0));
    const char *conf = kn_json_str(r, "conf", NULL);
    if (live && conf && *conf) cJSON_AddStringToObject(o, "CONF", conf);
    else cJSON_AddNullToObject(o, "CONF");
    return o;
}

/* (now) -> the window's rows, by region then number */
static kn_db_result *run_list(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const int64_t now = PI(0);
    cJSON *rows = dbx_rows(&x, SQL(.any = "SELECT id, region, svn, sid, owner_id, expires_s, conf FROM rented_servers ORDER BY region, sid, id"),
                           0, NULL);
    cJSON *out = cJSON_CreateArray();
    const cJSON *r;
    cJSON_ArrayForEach(r, rows) cJSON_AddItemToArray(out, window_row(r, now));
    cJSON_Delete(rows);
    return dbx_json(&x, "rentals_list", out);
}
static const kn_proc P_RENT_LIST = {"rentals_list", NULL, run_list};

/* the game names of two CONF texts differ (a new game restarts the server) */
static int game_changed(const char *a, const char *b) {
    cJSON *ja = a && *a ? kn_json_parse(a, strlen(a)) : NULL, *jb = b && *b ? kn_json_parse(b, strlen(b)) : NULL;
    const int changed = strcmp(kn_json_str(ja, "mn", ""), kn_json_str(jb, "mn", "")) ||
                        strcmp(kn_json_str(ja, "cr", ""), kn_json_str(jb, "cr", "")) ||
                        strcmp(kn_json_str(ja, "key", ""), kn_json_str(jb, "key", ""));
    cJSON_Delete(ja);
    cJSON_Delete(jb);
    return changed;
}

/* (account, server, typ, conf, update, now, max_per_account, agent_timeout_s) -> {kind, funds}. The checks and the
 * charge share one transaction behind the row's lock: two renters never both take a slot. */
static kn_db_result *run_rent(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const int64_t acc = PI(0), id = PI(1), typ = PI(2), now = PI(5);
    const char *conf = P(3) && *P(3) ? P(3) : NULL;
    const int update = PI(4) != 0;
    if (dbx_exec(&x, SQL(.any = "UPDATE rented_servers SET id = id WHERE id = {int:$1}"), KN_DB_ARGS(I64S(id))) == 0)
        dbx_refuse(&x, "That server doesn't exist.");
    cJSON *row = dbx_failed(&x) ? NULL
                                : dbx_row(&x, SQL(.any = "SELECT owner_id, expires_s, conf, agent_seen_s, generation FROM rented_servers WHERE id = {int:$1}"),
                                          KN_DB_ARGS(I64S(id)));
    if (dbx_failed(&x) || !row) {
        cJSON_Delete(row);
        return dbx_finish(&x, NULL);
    }
    const int64_t owner = kn_json_int(row, "owner_id", 0), expires = kn_json_int(row, "expires_s", 0);
    const int live = owner > 0 && expires > now, mine = live && owner == acc;
    char *old_conf = strdup(kn_json_str(row, "conf", ""));
    const int64_t seen = kn_json_int(row, "agent_seen_s", 0), gen = kn_json_int(row, "generation", 0);
    cJSON_Delete(row);
    int kind = 0;
    int64_t funds = -1;
    char ref[48];
    snprintf(ref, sizeof ref, "%lld:%lld", (long long) id, (long long) typ);
    if (live && !mine) {
        dbx_refuse(&x, "Someone else is renting this server.");
    } else if (update) {
        if (!mine) {
            dbx_refuse(&x, "You don't rent this server.");
        } else {
            /* Restart/Update: free; the game may change; the generation restarts the instance either way */
            dbx_exec(&x, SQL(.any = "UPDATE rented_servers SET conf = $2, generation = generation + 1 WHERE id = {int:$1}"),
                     KN_DB_ARGS(I64S(id), conf ? conf : old_conf));
            kind = RENT_UPDATED;
        }
    } else if (mine) {
        /* Extend Rental: the duration's price, its days on top of what is left; a new game restarts it */
        if (db_add_funds(&x, acc, -k_price[typ], "dedi", ref, &funds) == 0) {
            const int restart = conf && game_changed(conf, old_conf);
            dbx_exec(&x, SQL(.any = "UPDATE rented_servers SET expires_s = expires_s + {int:$2}, conf = $3,"
                                    " generation = generation + {int:$4} WHERE id = {int:$1}"),
                     KN_DB_ARGS(I64S(id), I64S(k_days[typ] * 86400), conf ? conf : old_conf, I64S(restart)));
            kind = RENT_EXTENDED;
        }
    } else if (!conf) {
        dbx_refuse(&x, "Pick a game for the server first.");
    } else if (seen + PI(7) < now) {
        dbx_refuse(&x, "This server is offline right now. Try another one.");
    } else if (dbx_int(&x, SQL(.any = "SELECT COUNT(*) FROM rented_servers WHERE owner_id = {int:$1} AND expires_s > {int:$2}"),
                       KN_DB_ARGS(I64S(acc), I64S(now)), 0) >= PI(6)) {
        char msg[96];
        snprintf(msg, sizeof msg, "You already rent %lld servers, the most one account can.", (long long) PI(6));
        dbx_refuse(&x, msg);
    } else if (db_add_funds(&x, acc, -k_price[typ], "dedi", ref, &funds) == 0) {
        /* explicit values: MySQL's SET reads the columns it already assigned, PostgreSQL / SQLite the old ones */
        dbx_exec(&x, SQL(.any = "UPDATE rented_servers SET owner_id = {int:$2}, expires_s = {int:$3}, conf = $4, generation = {int:$7},"
                                " rent_gen = {int:$7}, rented_s = {int:$5}, paid = {int:$6} WHERE id = {int:$1}"),
                 KN_DB_ARGS(I64S(id), I64S(acc), I64S(now + k_days[typ] * 86400), conf, I64S(now), I64S(k_price[typ]), I64S(gen + 1)));
        kind = RENT_RENTED;
    }
    free(old_conf);
    if (dbx_failed(&x)) return dbx_finish(&x, NULL);
    if (funds < 0) funds = dbx_int(&x, SQL(.any = "SELECT player_funds FROM players WHERE player_id = {int:$1}"), KN_DB_ARGS(I64S(acc)), 0);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "kind", kind);
    cJSON_AddNumberToObject(o, "funds", (double) funds);
    return dbx_json(&x, "rentals_rent", o);
}
static const kn_proc P_RENT = {"rentals_rent", NULL, run_rent};

/* (now, start_timeout_s) -> {refunded: [{id, owner, paid}], expired}: the startup guarantee's refunds (a fresh
 * rental that never ran), then the lapsed rentals freed. Both bump the generation: the agent stops the instance. */
static kn_db_result *run_sweep(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const int64_t now = PI(0);
    cJSON *late = dbx_rows(&x, SQL(.any = "SELECT id, owner_id, paid FROM rented_servers WHERE owner_id IS NOT NULL AND paid > 0"
                                          " AND running_gen < rent_gen AND rented_s + {int:$2} <= {int:$1}"),
                           KN_DB_ARGS(I64S(now), I64S(PI(1))));
    cJSON *refunded = cJSON_CreateArray();
    const cJSON *r;
    cJSON_ArrayForEach(r, late) {
        const int64_t id = kn_json_int(r, "id", 0), owner = kn_json_int(r, "owner_id", 0), paid = kn_json_int(r, "paid", 0);
        char ref[48];
        snprintf(ref, sizeof ref, "%lld:refund", (long long) id);
        if (db_add_funds(&x, owner, paid, "dedi_refund", ref, NULL) < 0) break;
        dbx_exec(&x, SQL(.any = "UPDATE rented_servers SET owner_id = NULL, conf = NULL, expires_s = 0, paid = 0, generation = generation + 1"
                                " WHERE id = {int:$1}"),
                 KN_DB_ARGS(I64S(id)));
        cJSON *e = cJSON_CreateObject();
        cJSON_AddNumberToObject(e, "id", (double) id);
        cJSON_AddNumberToObject(e, "owner", (double) owner);
        cJSON_AddNumberToObject(e, "paid", (double) paid);
        cJSON_AddItemToArray(refunded, e);
    }
    cJSON_Delete(late);
    const int64_t expired = dbx_exec(&x, SQL(.any = "UPDATE rented_servers SET owner_id = NULL, conf = NULL, expires_s = 0, paid = 0,"
                                                     " generation = generation + 1 WHERE owner_id IS NOT NULL AND expires_s <= {int:$1}"),
                                     KN_DB_ARGS(I64S(now)));
    if (dbx_failed(&x)) {
        cJSON_Delete(refunded);
        return dbx_finish(&x, NULL);
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "refunded", refunded);
    cJSON_AddNumberToObject(o, "expired", (double) expired);
    return dbx_json(&x, "rentals_sweep", o);
}
static const kn_proc P_RENT_SWEEP = {"rentals_sweep", NULL, run_sweep};

/* (pool JSON) -> {slots}: one row per slot (an upsert by id); free rows that left the pool go, rented ones stay
 * listed until they lapse */
static kn_db_result *run_pool(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    cJSON *pool = P(0) ? kn_json_parse(P(0), strlen(P(0))) : NULL;
    char keep[4096] = "0";
    size_t used = 1;
    int count = 0;
    const cJSON *s;
    cJSON_ArrayForEach(s, pool) {
        const int64_t id = kn_json_int(s, "id", 0);
        if (id <= 0) continue;
        const char *host = kn_json_str(s, "host", ""), *svn = kn_json_str(s, "name", "Server"), *pub = kn_json_str(s, "public_host", "");
        const char *const args[] = {I64S(id), host, I64S(kn_json_int(s, "region", 0)), svn, I64S(kn_json_int(s, "sid", id)),
                                    I64S(kn_json_int(s, "port", 0)), pub};
        if (dbx_exec(&x, SQL(.any = "UPDATE rented_servers SET host = $2, region = {int:$3}, svn = $4, sid = {int:$5}, port = {int:$6},"
                                    " public_host = $7 WHERE id = {int:$1}"),
                     7, args) == 0)
            dbx_exec(&x, SQL(.any = "INSERT INTO rented_servers (id, host, region, svn, sid, port, public_host)"
                                    " VALUES ({int:$1}, $2, {int:$3}, $4, {int:$5}, {int:$6}, $7)"),
                     7, args);
        if (used + 24 < sizeof keep) used += (size_t) snprintf(keep + used, sizeof keep - used, ",%lld", (long long) id);
        count++;
    }
    cJSON_Delete(pool);
    char sql[4200];
    snprintf(sql, sizeof sql, "DELETE FROM rented_servers WHERE owner_id IS NULL AND id NOT IN (%s)", keep); /* integers only */
    const kn_sql del = {.any = sql};
    dbx_exec(&x, &del, 0, NULL);
    if (dbx_failed(&x)) return dbx_finish(&x, NULL);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "slots", count);
    return dbx_json(&x, "rentals_pool", o);
}
static const kn_proc P_RENT_POOL = {"rentals_pool", NULL, run_pool};

/* (host, now) -> {now, slots}: the agent's desired state; the poll is its heartbeat (agent_seen_s) */
static kn_db_result *run_agent_get(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const int64_t now = PI(1);
    dbx_exec(&x, SQL(.any = "UPDATE rented_servers SET agent_seen_s = {int:$2} WHERE host = $1"), KN_DB_ARGS(P(0), I64S(now)));
    cJSON *rows = dbx_rows(&x, SQL(.any = "SELECT id, port, svn, sid, region, public_host, generation, owner_id, expires_s, conf"
                                          " FROM rented_servers WHERE host = $1 ORDER BY id"),
                           KN_DB_ARGS(P(0)));
    cJSON *slots = cJSON_CreateArray();
    const cJSON *r;
    cJSON_ArrayForEach(r, rows) {
        const int run = kn_json_int(r, "owner_id", 0) > 0 && kn_json_int(r, "expires_s", 0) > now;
        cJSON *conf = NULL;
        const char *ct = kn_json_str(r, "conf", NULL);
        if (run && ct && *ct) conf = kn_json_parse(ct, strlen(ct));
        cJSON *o = cJSON_CreateObject();
        char name[96];
        snprintf(name, sizeof name, "%s_%lld", kn_json_str(r, "svn", ""), (long long) kn_json_int(r, "sid", 0));
        cJSON_AddNumberToObject(o, "id", (double) kn_json_int(r, "id", 0));
        cJSON_AddNumberToObject(o, "port", (double) kn_json_int(r, "port", 0));
        cJSON_AddStringToObject(o, "name", name);
        cJSON_AddNumberToObject(o, "region", (double) kn_json_int(r, "region", 0));
        cJSON_AddStringToObject(o, "public_host", kn_json_str(r, "public_host", ""));
        cJSON_AddNumberToObject(o, "generation", (double) kn_json_int(r, "generation", 0));
        cJSON_AddBoolToObject(o, "run", run && conf);
        cJSON_AddStringToObject(o, "map", kn_json_str(conf, "mn", ""));
        cJSON_AddStringToObject(o, "creator", kn_json_str(conf, "cr", ""));
        cJSON_AddStringToObject(o, "key", kn_json_str(conf, "key", ""));
        const cJSON *gid = cJSON_GetObjectItemCaseSensitive(conf, "gid");
        if (cJSON_IsNumber(gid)) cJSON_AddNumberToObject(o, "game_id", gid->valuedouble);
        else cJSON_AddStringToObject(o, "game_id", cJSON_IsString(gid) ? gid->valuestring : "");
        cJSON_Delete(conf);
        cJSON_AddItemToArray(slots, o);
    }
    cJSON_Delete(rows);
    if (dbx_failed(&x)) {
        cJSON_Delete(slots);
        return dbx_finish(&x, NULL);
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "now", (double) now);
    cJSON_AddItemToObject(o, "slots", slots);
    return dbx_json(&x, "rentals_agent", o);
}
static const kn_proc P_RENT_AGENT_GET = {"rentals_agent", NULL, run_agent_get};

/* (host, slots JSON, now) -> {updated}: what the agent runs. A slot running its rental's generation (or a later one)
 * has started: the startup guarantee is kept (paid = 0). */
static kn_db_result *run_agent_status(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    cJSON *slots = P(1) ? kn_json_parse(P(1), strlen(P(1))) : NULL;
    int updated = 0;
    const cJSON *s;
    cJSON_ArrayForEach(s, slots) {
        const char *state = kn_json_str(s, "state", "");
        static const char *const ok[] = {"running", "starting", "stopped", "failed"};
        int known = 0;
        for (size_t i = 0; i < sizeof ok / sizeof *ok; i++) known |= !strcmp(state, ok[i]);
        if (!known) continue;
        const int running = !strcmp(state, "running");
        const int64_t gen = kn_json_int(s, "generation", -1);
        const char *const args[] = {I64S(kn_json_int(s, "id", 0)), P(0), state, I64S(running ? gen : -1), I64S(PI(2))};
        const int64_t k = dbx_exec(&x, SQL(.any = "UPDATE rented_servers SET state = $3, agent_seen_s = {int:$5},"
                                                   " running_gen = CASE WHEN {int:$4} >= 0 THEN {int:$4} ELSE running_gen END,"
                                                   " paid = CASE WHEN {int:$4} >= rent_gen THEN 0 ELSE paid END"
                                                   " WHERE id = {int:$1} AND host = $2"),
                                   5, args);
        if (k > 0) updated += (int) k;
    }
    cJSON_Delete(slots);
    if (dbx_failed(&x)) return dbx_finish(&x, NULL);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "updated", updated);
    return dbx_json(&x, "rentals_status", o);
}
static const kn_proc P_RENT_AGENT_STATUS = {"rentals_status", NULL, run_agent_status};

/* ------------------------------------------------------------------ social frames */

static void list_done(SocialSession *s, kn_db_result *res, void *extra) {
    (void) extra;
    cJSON *rows = kn_db_ok(res) ? acct_db_json(res) : NULL;
    social_send_n(s, "dsrvl", 2, cJSON_CreateNumber(0), rows ? rows : cJSON_CreateArray());
}

static void send_list(SocialSession *s) {
    char now[24];
    acct_itoa(now, now_s());
    social_call(s, &P_RENT_LIST, KN_DB_ARGS(now), list_done, NULL);
}

/* ["dsrvl"]: the window's list (guests see it too: the regions' free servers) */
void sh_dedi_list(SocialSession *s, const cJSON *f) {
    (void) f;
    if (!db_ready()) {
        social_send_n(s, "dsrvl", 2, cJSON_CreateNumber(0), cJSON_CreateArray());
        return;
    }
    send_list(s);
}

static void push_account_cb(void *ud, const char *err, const cJSON *data) {
    (void) ud;
    if (!err && data) social_push_account_update(data);
}

static void rent_done(SocialSession *s, kn_db_result *res, void *extra) {
    (void) extra;
    cJSON *v = kn_db_ok(res) ? acct_db_json(res) : NULL;
    if (!v) {
        social_send_n(s, "dsrvl", 3, cJSON_CreateNumber(1), cJSON_CreateString(acct_db_error(res, "rent")), cJSON_CreateFalse());
        return;
    }
    const int kind = (int) kn_json_int(v, "kind", 0);
    cJSON_Delete(v);
    const char *text = kind == RENT_RENTED     ? "Server rented. It can take a few minutes before it shows up in the server list."
                       : kind == RENT_EXTENDED ? "Rental extended."
                                               : "Your server is restarting with the selected game.";
    social_send_n(s, "dsrvl", 3, cJSON_CreateNumber(1), cJSON_CreateString(text), cJSON_CreateTrue());
    send_list(s);
    if (kind != RENT_UPDATED) acct_get(s->account_id, push_account_cb, NULL);
}

/* the chosen game as CONF {gid, mn, cr, key, typ}, NULL without a name */
static char *conf_of(const cJSON *game, int typ) {
    const char *name = kn_json_str(game, "gnm", "");
    if (!name[0] || strlen(name) > 96) return NULL;
    cJSON *o = cJSON_CreateObject();
    const cJSON *mid = cJSON_GetObjectItemCaseSensitive(game, "mid");
    if (cJSON_IsNumber(mid)) cJSON_AddNumberToObject(o, "gid", mid->valuedouble);
    else if (cJSON_IsString(mid) && strlen(mid->valuestring) < 64) cJSON_AddStringToObject(o, "gid", mid->valuestring);
    else cJSON_AddNullToObject(o, "gid");
    cJSON_AddStringToObject(o, "mn", name);
    const char *cr = kn_json_str(game, "cr", ""), *key = kn_json_str(game, "key", "");
    if (cr[0] && strlen(cr) < 48) cJSON_AddStringToObject(o, "cr", cr);
    if (key[0] && strlen(key) < 96) cJSON_AddStringToObject(o, "key", key);
    cJSON_AddNumberToObject(o, "typ", typ);
    char *t = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return t;
}

/* ["rdsrv", id, {typ, mid, gnm, cr, key}, update]: rent, extend or restart/update (doc/v10/rented-servers.md table) */
void sh_dedi_rent(SocialSession *s, const cJSON *f) {
    const cJSON *game = fa(f, 2);
    const int typ = (int) (cJSON_IsObject(game) ? (cJSON_IsString(cJSON_GetObjectItem(game, "typ"))
                                                       ? strtol(cJSON_GetObjectItem(game, "typ")->valuestring, NULL, 10)
                                                       : kn_json_int(game, "typ", -1))
                                                : -1);
    const cJSON *upd = fa(f, 3);
    const int update = cJSON_IsTrue(upd) || (cJSON_IsNumber(upd) && upd->valuedouble != 0);
    if (s->account_id <= 0) {
        social_send_n(s, "dsrvl", 3, cJSON_CreateNumber(1), cJSON_CreateString("Sign in to rent a server."), cJSON_CreateFalse());
        return;
    }
    if (!db_ready()) {
        social_send_n(s, "dsrvl", 3, cJSON_CreateNumber(1), cJSON_CreateString("Server rentals are unavailable right now."),
                      cJSON_CreateFalse());
        return;
    }
    if (typ < 0 || typ >= RENT_TYPES) {
        social_send_n(s, "dsrvl", 3, cJSON_CreateNumber(1), cJSON_CreateString("Pick a rental duration."), cJSON_CreateFalse());
        return;
    }
    char *conf = conf_of(game, typ);
    char acc[24], id[24], ty[24], up[24], now[24], max[24], timeout[24];
    acct_itoa(acc, s->account_id);
    acct_itoa(id, fa_int(f, 1, 0));
    acct_itoa(ty, typ);
    acct_itoa(up, update);
    acct_itoa(now, now_s());
    acct_itoa(max, R.max_per_account);
    acct_itoa(timeout, R.agent_timeout_s);
    social_call(s, &P_RENT, KN_DB_ARGS(acc, id, ty, conf ? conf : "", up, now, max, timeout), rent_done, NULL);
    free(conf);
}

/* ------------------------------------------------------------------ agent API */

typedef struct {
    kn_http_req *req;
} AgentReq;

static void reply_error(kn_http_req *req, int status, const char *msg) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "error", msg);
    brain_reply_json(req, status, o);
    cJSON_Delete(o);
}

static void agent_done(kn_db_result *res, void *ud) {
    AgentReq *q = ud;
    if (!kn_db_ok(res)) {
        KN_WARN(MOD, "agent request failed: %s", kn_db_error(res));
        reply_error(q->req, 500, "internal");
    } else {
        const char *t = kn_db_rows(res) > 0 ? kn_db_get(res, 0, 0) : NULL;
        cJSON *data = t ? kn_json_parse(t, strlen(t)) : NULL;
        brain_reply_json(q->req, 200, data ? data : cJSON_CreateObject());
        cJSON_Delete(data);
    }
    free(q);
}

static int agent_host(const char *host) {
    if (!host[0] || strlen(host) > 63) return 0;
    for (const char *c = host; *c; c++)
        if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '-' || *c == '_' || *c == '.')) return 0;
    return 1;
}

static void h_agent_get(kn_http_req *req, void *ud) {
    (void) ud;
    if (!mm_internal_authorized(req, "rentals agent")) return;
    if (!db_ready()) {
        reply_error(req, 503, "RentalsUnavailable");
        return;
    }
    char host[64] = "";
    kn_http_query_get(req, "host", host, sizeof host);
    if (!agent_host(host)) {
        reply_error(req, 400, "host");
        return;
    }
    AgentReq *q = calloc(1, sizeof *q);
    if (!q) {
        reply_error(req, 500, "internal");
        return;
    }
    q->req = req;
    char now[24];
    acct_itoa(now, now_s());
    kn_db_call(R.b->db, &P_RENT_AGENT_GET, KN_DB_ARGS(host, now), agent_done, q);
}

static void h_agent_status(kn_http_req *req, void *ud) {
    (void) ud;
    if (!mm_internal_authorized(req, "rentals status")) return;
    if (!db_ready()) {
        reply_error(req, 503, "RentalsUnavailable");
        return;
    }
    cJSON *body = kn_http_body_json(req);
    const char *host = kn_json_str(body, "host", "");
    const cJSON *slots = cJSON_GetObjectItemCaseSensitive(body, "slots");
    if (!agent_host(host) || !cJSON_IsArray(slots)) {
        cJSON_Delete(body);
        reply_error(req, 400, "body");
        return;
    }
    AgentReq *q = calloc(1, sizeof *q);
    char *text = cJSON_PrintUnformatted(slots);
    if (!q || !text) {
        free(q);
        free(text);
        cJSON_Delete(body);
        reply_error(req, 500, "internal");
        return;
    }
    q->req = req;
    char now[24], h[64];
    acct_itoa(now, now_s());
    snprintf(h, sizeof h, "%s", host);
    cJSON_Delete(body);
    kn_db_call(R.b->db, &P_RENT_AGENT_STATUS, KN_DB_ARGS(h, text, now), agent_done, q);
    free(text);
}

/* ------------------------------------------------------------------ timer, module */

static void sweep_done(kn_db_result *res, void *ud) {
    (void) ud;
    if (!kn_db_ok(res)) {
        KN_WARN(MOD, "sweep failed: %s", kn_db_error(res));
        return;
    }
    const char *t = kn_db_rows(res) > 0 ? kn_db_get(res, 0, 0) : NULL;
    cJSON *v = t ? kn_json_parse(t, strlen(t)) : NULL;
    const cJSON *e;
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(v, "refunded")) {
        const int64_t owner = kn_json_int(e, "owner", 0);
        KN_INFO(MOD, "server %lld never started: %lld KR refunded to account %lld", (long long) kn_json_int(e, "id", 0),
                (long long) kn_json_int(e, "paid", 0), (long long) owner);
        if (owner > 0) acct_get(owner, push_account_cb, NULL);
    }
    if (kn_json_int(v, "expired", 0) > 0) KN_INFO(MOD, "%lld rental(s) lapsed", (long long) kn_json_int(v, "expired", 0));
    cJSON_Delete(v);
}

static void tick(kn_loop *loop, void *ud) {
    (void) loop;
    (void) ud;
    if (!db_ready()) return;
    char now[24], timeout[24];
    acct_itoa(now, now_s());
    acct_itoa(timeout, R.start_timeout_s);
    kn_db_call(R.b->db, &P_RENT_SWEEP, KN_DB_ARGS(now, timeout), sweep_done, NULL);
}

static void pool_done(kn_db_result *res, void *ud) {
    (void) ud;
    if (!kn_db_ok(res)) KN_WARN(MOD, "pool upsert failed: %s", kn_db_error(res));
}

int rentals_init(Brain *b) {
    R.b = b;
    R.enabled = kn_config_bool(b->cfg, "brain.rentals.enabled", 1);
    R.max_per_account = kn_config_int(b->cfg, "brain.rentals.max_per_account", 3);   /* config.dediSlots :19649 */
    R.start_timeout_s = kn_config_int(b->cfg, "brain.rentals.start_timeout_s", 600); /* config.dediTimer :19650 */
    R.agent_timeout_s = kn_config_int(b->cfg, "brain.rentals.agent_timeout_s", 120);
    kn_http_route(b->http, "GET", "/mm/internal/rentals", h_agent_get, b);
    kn_http_route(b->http, "POST", "/mm/internal/rentals/status", h_agent_status, b);
    if (!b->db || !R.enabled) return 0;
    const cJSON *pool = kn_config_get(b->cfg, "brain.rentals.pool");
    char *text = cJSON_IsArray(pool) ? cJSON_PrintUnformatted(pool) : NULL;
    kn_db_call(b->db, &P_RENT_POOL, KN_DB_ARGS(text ? text : "[]"), pool_done, NULL);
    free(text);
    R.timer = kn_loop_add_timer(b->loop, 3000, (uint64_t) kn_config_int(b->cfg, "brain.rentals.check_ms", 30000), tick, NULL);
    KN_INFO(MOD, "rented servers: %d slot(s)", cJSON_IsArray(pool) ? cJSON_GetArraySize(pool) : 0);
    return 0;
}

void rentals_shutdown(Brain *b) {
    if (R.timer) kn_loop_cancel_timer(b->loop, R.timer);
    R.timer = 0;
    R.enabled = 0;
}
