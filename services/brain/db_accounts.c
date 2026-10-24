/*
 * services/brain/db_accounts.c - account / session / stats / store operations
 * (db_procs.h). .pg = the call accounts.c and store.c always made into the
 * functions of postgres/0103, 0105, 0106; run() = the same operation in C for
 * the other drivers (sqlite/0100.., mysql/0100..), returning the same shape.
 */
#include "db_procs.h"
#include "double_xp.h"
#include "knsvc/loop.h"

#include "badges.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "knsvc/crypto.h"

#define P(i) (n > (i) ? p[i] : NULL)
#define PI(i) pint(P(i))

static int64_t pint(const char *s) { return s ? strtoll(s, NULL, 10) : 0; }

/* ------------------------------------------------------------------ login / register / sessions */

/* kn_login_success: new session (token NULL = none), audit row, optional rehash. */
static kn_db_result *login_success(Dbx *x, const char *id, const char *ip, const char *via, const char *token,
                                   const char *ttl, const char *origin, const char *new_hash) {
    if (token)
        dbx_exec(x, SQL(.any = "INSERT INTO sessions (token_hash, account_id, expires_at, ip, origin)"
                               " VALUES ({hash:$1}, $2, {later:$3}, $4, $5)"),
                 KN_DB_ARGS(token, id, ttl, ip, origin));
    dbx_exec(x, SQL(.any = "INSERT INTO login_audit (account_id, ip, via) VALUES ($1, $2, $3)"), KN_DB_ARGS(id, ip, via));
    dbx_exec(x, SQL(.any = "UPDATE accounts SET last_login_at = {now}, last_login_ip = coalesce($2, last_login_ip),"
                           " password_hash = coalesce($3, password_hash) WHERE id = $1"),
             KN_DB_ARGS(id, ip, new_hash));
    if (ip)
        dbx_exec(x, SQL(.any = "DELETE FROM login_failures WHERE ip = $2 AND"
                               " lower(username) = lower((SELECT username FROM accounts WHERE id = $1))"),
                 KN_DB_ARGS(id, ip));
    return dbx_json(x, "kn_login_success", db_account_json(x, strtoll(id, NULL, 10)));
}

static kn_db_result *run_login_success(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    return login_success(&x, P(0), P(1), P(2), P(3), P(4), P(5), P(6));
}
const kn_proc P_LOGIN_SUCCESS = {"login_success", "SELECT kn_login_success($1, $2, $3, $4, $5, $6, $7)",
                                 run_login_success};

static kn_db_result *run_login_failed(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    if (P(1)) {
        dbx_exec(&x, SQL(.any = "INSERT INTO login_failures (username, ip) VALUES ($1, $2)"), KN_DB_ARGS(P(0), P(1)));
        dbx_exec(&x,
                 SQL(.sqlite = "INSERT INTO login_ip_blocks (ip, failures) VALUES ($1, 1)"
                               " ON CONFLICT (ip) DO UPDATE SET failures = failures + 1, updated_at = {now}",
                     .mysql = "INSERT INTO login_ip_blocks (ip, failures) VALUES ($1, 1)"
                              " ON DUPLICATE KEY UPDATE failures = failures + 1, updated_at = {now}"),
                 KN_DB_ARGS(P(1)));
    }
    return dbx_finish(&x, kn_db_result_command(0));
}
const kn_proc P_LOGIN_FAILED = {"login_failed", "SELECT kn_login_failed($1, $2)", run_login_failed};

/* (name, hash, ip, ref, starting_kr, premium, token, ttl, origin) */
static kn_db_result *run_register(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    if (dbx_exists(&x, SQL(.any = "SELECT 1 FROM accounts WHERE lower(username) = lower($1)"), KN_DB_ARGS(P(0))))
        return dbx_finish(&x, kn_db_result_user_error("Username already taken."));
    const char *ref = P(3) && *P(3) ? P(3) : NULL;
    int64_t id = dbx_insert_id(&x,
        SQL(.any = "INSERT INTO accounts (username, password_hash, created_ip, referral) VALUES ($1, $2, $3, $4)"),
        KN_DB_ARGS(P(0), P(1), P(2), ref));
    int64_t kr = PI(4) > 0 ? PI(4) : 0;
    dbx_exec(&x, SQL(.any = "INSERT INTO players (player_id, player_funds, player_premium) VALUES ($1, $2, $3)"),
             KN_DB_ARGS(I64S(id), I64S(kr), P(5) ? P(5) : "0"));
    if (kr > 0)
        dbx_exec(&x, SQL(.any = "INSERT INTO kr_transactions (account_id, amount, source) VALUES ($1, $2, 'starting')"),
                 KN_DB_ARGS(I64S(id), I64S(kr)));
    if (dbx_failed(&x)) return dbx_finish(&x, NULL);
    return login_success(&x, I64S(id), P(2), "register", P(6), P(7), P(8), NULL);
}
const kn_proc P_REGISTER = {"register", "SELECT kn_register($1, $2, $3, $4, $5, $6, $7, $8, $9)", run_register};

/* (name, ip, per_ip_hour, email | NULL) -> taken, banned, limited, email_taken (another account
 * has the email: one account per address, the login modal signs in with it; 0122_email_login) */
static kn_db_result *run_register_precheck(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    int taken = dbx_exists(&x, SQL(.any = "SELECT 1 FROM accounts WHERE lower(username) = lower($1)"), KN_DB_ARGS(P(0)));
    int banned = P(1) ? db_active_ban(&x, 0, P(1), NULL, 0, NULL) : 0;
    int limited = 0;
    if (P(1) && PI(2) > 0)
        limited = dbx_int(&x, SQL(.any = "SELECT count(*) FROM accounts WHERE created_ip = $1 AND created_at > {ago:3600}"),
                          KN_DB_ARGS(P(1)), 0) >= PI(2);
    int email_taken = P(3) && *P(3) &&
                      dbx_exists(&x, SQL(.mysql = "SELECT 1 FROM accounts WHERE email = $1",
                                         .any = "SELECT 1 FROM accounts WHERE lower(email) = lower($1)"),
                                 KN_DB_ARGS(P(3)));
    const char *names[4] = {"taken", "banned", "limited", "email_taken"};
    const kn_db_type types[4] = {KN_DB_T_BOOL, KN_DB_T_BOOL, KN_DB_T_BOOL, KN_DB_T_BOOL};
    kn_db_result *r = kn_db_result_new(4, names, types);
    const char *row[4] = {taken ? "t" : "f", banned ? "t" : "f", limited ? "t" : "f", email_taken ? "t" : "f"};
    if (r) kn_db_result_add_row(r, row);
    return dbx_finish(&x, r);
}
const kn_proc P_REGISTER_PRECHECK = {
    "register_precheck",
    "SELECT EXISTS (SELECT 1 FROM accounts WHERE lower(username) = lower($1)) AS taken, "
    "($2::text IS NOT NULL AND EXISTS (SELECT 1 FROM kn_active_ban(NULL, $2) b WHERE b.reason IS NOT NULL OR b.expires_at IS NOT NULL)) AS banned, "
    "($2::text IS NOT NULL AND $3::int > 0 AND (SELECT count(*) FROM accounts WHERE created_ip = $2 "
    "AND created_at > now() - interval '1 hour') >= $3::int) AS limited, "
    "($4::text IS NOT NULL AND EXISTS (SELECT 1 FROM accounts WHERE lower(email) = lower($4))) AS email_taken",
    run_register_precheck};

/* kn_login_lookup(name, ip, throttle_s, block_max, block_days): one row always */
static kn_db_result *run_login_lookup(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const char *name = P(0), *ip = P(1);
    int ip_blocked = ip && dbx_exists(&x,
        SQL(.any = "SELECT 1 FROM login_ip_blocks WHERE ip = $1 AND failures > {int:$2} AND whitelist = {false}"
                   " AND updated_at > {ago:$3}"),
        KN_DB_ARGS(ip, P(3), I64S(PI(4) * 86400)));
    int throttled = ip && PI(2) > 0 && dbx_exists(&x,
        SQL(.any = "SELECT 1 FROM login_failures WHERE lower(username) = lower($1) AND ip = $2 AND at > {ago:$3}"),
        KN_DB_ARGS(name, ip, P(2)));
    kn_db_result *a = dbx_q(&x, SQL(.any = "SELECT id, password_hash, twofa_secret FROM accounts WHERE lower(username) = lower($1)"),
                            KN_DB_ARGS(name));
    int found = a && kn_db_rows(a) > 0;
    int64_t id = found ? kn_db_get_int64(a, 0, 0, 0) : 0;
    char reason[256];
    int64_t expires = 0;
    int banned = db_active_ban(&x, id, ip, reason, sizeof reason, &expires);
    const char *names[8] = {"ip_blocked", "throttled", "account_id", "password_hash", "ban_reason", "ban_expires", "banned", "twofa"};
    const kn_db_type types[8] = {KN_DB_T_BOOL, KN_DB_T_BOOL, KN_DB_T_INT, KN_DB_T_TEXT, KN_DB_T_TEXT, KN_DB_T_INT,
                                 KN_DB_T_BOOL, KN_DB_T_TEXT};
    kn_db_result *r = kn_db_result_new(8, names, types);
    const char *row[8] = {ip_blocked ? "t" : "f", throttled ? "t" : "f", found ? I64S(id) : NULL,
                          found ? kn_db_get(a, 0, 1) : NULL, banned ? reason : NULL, I64S(expires),
                          banned ? "t" : "f", found && !kn_db_isnull(a, 0, 2) ? kn_db_get(a, 0, 2) : NULL};
    if (r) kn_db_result_add_row(r, row);
    kn_db_result_free(a);
    return dbx_finish(&x, r);
}
const kn_proc P_LOGIN_LOOKUP = {"login_lookup", "SELECT * FROM kn_login_lookup($1, $2, $3, $4, $5)", run_login_lookup};

/* kn_account_json(kn_session_account(token, ttl)): sliding expiry, NULL when unknown */
static kn_db_result *run_session_account(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    int64_t hit = dbx_exec(&x,
        SQL(.any = "UPDATE sessions SET last_seen_at = {now}, expires_at = {greatest:expires_at, {later:$2}}"
                   " WHERE token_hash = {hash:$1} AND expires_at > {now}"),
        KN_DB_ARGS(P(0), P(1)));
    int64_t id = hit > 0 ? dbx_int(&x, SQL(.any = "SELECT account_id FROM sessions WHERE token_hash = {hash:$1}"),
                                   KN_DB_ARGS(P(0)), 0)
                         : 0;
    return dbx_json(&x, "kn_account_json", id > 0 ? db_account_json(&x, id) : NULL);
}
const kn_proc P_SESSION_ACCOUNT = {"session_account", "SELECT kn_account_json(kn_session_account($1, $2))",
                                   run_session_account};

/* (id, hash, token, ttl): new hash, every session revoked, one fresh session -> {token}; no row when unknown */
static kn_db_result *run_set_password(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    int64_t hit = dbx_exec(&x, SQL(.any = "UPDATE accounts SET password_hash = $2 WHERE id = $1"), KN_DB_ARGS(P(0), P(1)));
    if (hit <= 0) {
        const char *names[1] = {"jsonb_build_object"};
        const kn_db_type types[1] = {KN_DB_T_JSON};
        return dbx_finish(&x, kn_db_result_new(1, names, types));
    }
    dbx_exec(&x, SQL(.any = "DELETE FROM sessions WHERE account_id = $1"), KN_DB_ARGS(P(0)));
    dbx_exec(&x, SQL(.any = "INSERT INTO sessions (token_hash, account_id, expires_at, origin)"
                            " VALUES ({hash:$1}, $2, {later:$3}, 'password')"),
             KN_DB_ARGS(P(2), P(0), P(3)));
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "token", P(2) ? P(2) : "");
    return dbx_json(&x, "jsonb_build_object", o);
}
const kn_proc P_SET_PASSWORD = {
    "set_password",
    "WITH u AS (UPDATE accounts SET password_hash = $2 WHERE id = $1 RETURNING id), "
    "d AS (DELETE FROM sessions WHERE account_id IN (SELECT id FROM u)), "
    "s AS (INSERT INTO sessions (token_hash, account_id, expires_at, origin) "
    "SELECT sha256(convert_to($3, 'UTF8')), id, now() + make_interval(secs => $4), 'password' FROM u) "
    "SELECT jsonb_build_object('token', $3::text) FROM u",
    run_set_password};

static kn_db_result *run_set_alias(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    kn_db_result *r = dbx_q(&x, SQL(.any = "SELECT player_alias FROM players WHERE player_id = $1 {for_update}"),
                            KN_DB_ARGS(P(0)));
    if (!r) return dbx_finish(&x, NULL);
    if (kn_db_rows(r) < 1) {
        kn_db_result_free(r);
        return dbx_finish(&x, kn_db_result_user_error("Account not found."));
    }
    char *old = kn_db_isnull(r, 0, 0) ? NULL : strdup(kn_db_get(r, 0, 0));
    kn_db_result_free(r);
    if (dbx_exists(&x, SQL(.any = "SELECT 1 FROM accounts WHERE lower(username) = lower($2) AND id <> $1"), KN_DB_ARGS(P(0), P(1))) ||
        dbx_exists(&x, SQL(.any = "SELECT 1 FROM players WHERE lower(player_alias) = lower($2) AND player_id <> $1"),
                   KN_DB_ARGS(P(0), P(1)))) {
        free(old);
        return dbx_finish(&x, kn_db_result_user_error("Name already taken."));
    }
    dbx_exec(&x, SQL(.any = "UPDATE players SET player_alias = $2, updated_at = {now} WHERE player_id = $1"),
             KN_DB_ARGS(P(0), P(1)));
    dbx_exec(&x, SQL(.any = "INSERT INTO name_changes (account_id, kind, old_name, new_name) VALUES ($1, 'alias', $2, $3)"),
             KN_DB_ARGS(P(0), old, P(1)));
    free(old);
    return dbx_json(&x, "kn_set_alias", db_account_json(&x, PI(0)));
}
const kn_proc P_SET_ALIAS = {"set_alias", "SELECT kn_set_alias($1, $2)", run_set_alias};

static kn_db_result *run_account_json(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    return dbx_json(&x, "kn_account_json", db_account_json(&x, PI(0)));
}
const kn_proc P_ACCOUNT_JSON = {"account_json", "SELECT kn_account_json($1)", run_account_json};

/* ------------------------------------------------------------------ stats, rewards, ops */

/* kn_jsonb_add: numeric members of b added to a's, others replace */
static void json_add(cJSON *a, const cJSON *b) {
    const cJSON *v;
    cJSON_ArrayForEach(v, b) {
        cJSON *cur = cJSON_GetObjectItemCaseSensitive(a, v->string);
        if (cJSON_IsNumber(v) && (!cur || cJSON_IsNumber(cur))) {
            double s = (cur ? cur->valuedouble : 0) + v->valuedouble;
            if (cur) cJSON_SetNumberValue(cur, s);
            else cJSON_AddNumberToObject(a, v->string, s);
        } else {
            cJSON *d = cJSON_Duplicate(v, 1);
            if (cur) cJSON_ReplaceItemInObjectCaseSensitive(a, v->string, d);
            else cJSON_AddItemToObject(a, v->string, d);
        }
    }
}

static int64_t jint(const cJSON *o, const char *k, int64_t def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsNumber(v)) return (int64_t)v->valuedouble;
    if (cJSON_IsString(v) && *v->valuestring) return strtoll(v->valuestring, NULL, 10);
    return def;
}

static const char *jstr(const cJSON *o, const char *k, char buf[64]) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsString(v)) return v->valuestring;
    if (cJSON_IsNumber(v)) {
        snprintf(buf, 64, "%.15g", v->valuedouble);
        return buf;
    }
    return NULL;
}

static int64_t clamp0(int64_t v) { return v < 0 ? 0 : v; }

/* kn_commit_stats(p json, max_kr): idempotent per (match, account) */
static kn_db_result *run_commit_stats(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    cJSON *in = P(0) ? cJSON_Parse(P(0)) : NULL;
    int64_t id = jint(in, "id", 0);
    if (!in || id <= 0 || !dbx_exists(&x, SQL(.any = "SELECT 1 FROM players WHERE player_id = $1"), KN_DB_ARGS(I64S(id)))) {
        cJSON_Delete(in);
        return dbx_finish(&x, kn_db_result_user_error("Account not found."));
    }
    int64_t kills = clamp0(jint(in, "kills", 0)), deaths = clamp0(jint(in, "deaths", 0));
    int64_t score = clamp0(jint(in, "score", 0)), xp = clamp0(jint(in, "xp", jint(in, "score", 0)));
    const cJSON *wv = cJSON_GetObjectItemCaseSensitive(in, "won");
    int won = cJSON_IsNumber(wv) ? wv->valuedouble != 0 : cJSON_IsTrue(wv);
    int64_t games = clamp0(jint(in, "games", 0)), time_ms = clamp0(jint(in, "time_ms", 0));
    const int comp = jint(in, "comp", 0) != 0; /* a competitive game: no end-screen 2x XP (isComp) */
    int64_t kr = clamp0(jint(in, "kr", 0));
    if (kr > clamp0(PI(1))) kr = clamp0(PI(1));
    const cJSON *cls = cJSON_GetObjectItemCaseSensitive(in, "class");
    const cJSON *st = cJSON_GetObjectItemCaseSensitive(in, "stats");
    cJSON *stats = cJSON_IsObject(st) ? cJSON_Duplicate(st, 1) : cJSON_CreateObject();
    char match[160], b1[64], b2[64], b3[64], b4[64], b5[64];
    const char *m = jstr(in, "match", b1);
    if (m && *m) {
        snprintf(match, sizeof match, "%s", m);
    } else {
        char rnd[40];
        kn_random_hex(rnd, sizeof rnd, 16);
        snprintf(match, sizeof match, "nomatch:%s", rnd);
    }
    char *stats_text = cJSON_PrintUnformatted(stats);
    int64_t ins = dbx_exec(&x,
        SQL(.any = "{insert_ignore} INTO match_results (match_id, account_id, server, region, map, mode, class_id, kills,"
                   " deaths, score, won, games, time_ms, kr, stats)"
                   " VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13, $14, $15) {ignore_conflicts}"),
        15, (const char *const[]){match, I64S(id), jstr(in, "server", b2), jstr(in, "region", b3), jstr(in, "map", b4),
                                  jstr(in, "mode", b5), cJSON_IsNumber(cls) ? I64S((int64_t)cls->valuedouble) : NULL,
                                  I64S(kills), I64S(deaths), I64S(score), won ? "1" : "0", I64S(games), I64S(time_ms),
                                  I64S(kr), stats_text});
    free(stats_text);
    if (ins == 0) { /* already committed */
        cJSON_Delete(stats);
        cJSON_Delete(in);
        cJSON *a = db_account_json(&x, id);
        if (a) {
            cJSON_AddNumberToObject(a, "kr_awarded", 0);
            cJSON_AddNumberToObject(a, "duplicate", 1);
        }
        return dbx_json(&x, "kn_commit_stats", a);
    }
    if (cJSON_IsNumber(cls) && cls->valuedouble >= 0) {
        char key[32];
        snprintf(key, sizeof key, "c%lld", (long long)cls->valuedouble);
        cJSON_DeleteItemFromObjectCaseSensitive(stats, key);
        /* v10 isDoubleClassXP, fixed when the match started: the game server sends it per round
         * ("classDouble"); a frame without it (an older server) takes the schedule at the commit */
        const int dcl = cJSON_GetObjectItemCaseSensitive(in, "classDouble")
                            ? jint(in, "classDouble", 0) != 0
                            : dxp_schedule(kn_wall_ms()).double_class_xp;
        cJSON_AddNumberToObject(stats, key, (double)dxp_class_xp(xp, dcl)); /* jsonb || : the class xp replaces a stats member */
    }
    char *old = dbx_text(&x, SQL(.any = "SELECT player_stats FROM players WHERE player_id = $1 {for_update}"),
                         KN_DB_ARGS(I64S(id)));
    cJSON *merged = old ? cJSON_Parse(old) : NULL;
    free(old);
    if (!cJSON_IsObject(merged)) {
        cJSON_Delete(merged);
        merged = cJSON_CreateObject();
    }
    json_add(merged, stats);
    char *merged_text = cJSON_PrintUnformatted(merged);
    cJSON_Delete(merged);
    cJSON_Delete(stats);
    const cJSON *ri = cJSON_GetObjectItemCaseSensitive(in, "region_index");
    dbx_exec(&x,
        SQL(.any = "UPDATE players SET player_kills = player_kills + {int:$2}, player_deaths = player_deaths + {int:$3},"
                   " player_wins = player_wins + {int:$4}, player_games_played = player_games_played + {int:$5},"
                   " player_score = player_score + {int:$6}, player_timeplayed = player_timeplayed + {int:$7},"
                   " player_funds = player_funds + {int:$8}, player_stats = $9,"
                   " player_region = coalesce({int:$10}, player_region), updated_at = {now} WHERE player_id = $1"),
        10, (const char *const[]){I64S(id), I64S(kills), I64S(deaths), won ? "1" : "0", I64S(games), I64S(xp),
                                  I64S(time_ms), I64S(kr), merged_text,
                                  cJSON_IsNumber(ri) ? I64S((int64_t)ri->valuedouble) : NULL});
    free(merged_text);
    if (kr > 0)
        dbx_exec(&x, SQL(.any = "INSERT INTO kr_transactions (account_id, amount, source, ref) VALUES ($1, $2, 'reward', $3)"),
                 KN_DB_ARGS(I64S(id), I64S(kr), match));
    if (dbx_exec(&x, SQL(.any = "UPDATE clan_members SET score = score + {int:$2}, kills = kills + {int:$3},"
                                " deaths = deaths + {int:$4} WHERE account_id = $1"),
                 KN_DB_ARGS(I64S(id), I64S(xp), I64S(kills), I64S(deaths))) > 0) {
        int64_t clan = dbx_int(&x, SQL(.any = "SELECT clan_id FROM clan_members WHERE account_id = $1"), KN_DB_ARGS(I64S(id)), 0);
        dbx_exec(&x, SQL(.any = "UPDATE clans SET clan_score = clan_score + {int:$2} WHERE clan_id = $1"),
                 KN_DB_ARGS(I64S(clan), I64S(xp)));
        dbx_exec(&x, SQL(.any = "UPDATE players SET player_clankills = player_clankills + {int:$2},"
                                " player_clandeaths = player_clandeaths + {int:$3} WHERE player_id = $1"),
                 KN_DB_ARGS(I64S(id), I64S(kills), I64S(deaths)));
    }
    /* v10's end-screen 2x XP offer: the match just played, when it counted and was not competitive */
    db_dxp_record_match(&x, id, games > 0 && !comp, xp);
    cJSON_Delete(in);
    cJSON *a = db_account_json(&x, id);
    if (a) {
        cJSON_AddNumberToObject(a, "kr_awarded", (double)kr);
        cJSON_AddNumberToObject(a, "duplicate", 0);
    }
    return dbx_json(&x, "kn_commit_stats", a);
}
const kn_proc P_COMMIT_STATS = {"commit_stats", "SELECT kn_commit_stats($1::jsonb, $2)", run_commit_stats};

/* (ids json, krs json) -> {funds:[...]} for the ids that exist, in order */
static kn_db_result *run_rewards(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    cJSON *ids = P(0) ? cJSON_Parse(P(0)) : NULL, *krs = P(1) ? cJSON_Parse(P(1)) : NULL;
    cJSON *funds = cJSON_CreateArray();
    int count = cJSON_GetArraySize(ids);
    for (int i = 0; i < count && i < cJSON_GetArraySize(krs) && !dbx_failed(&x); i++) {
        int64_t id = (int64_t)cJSON_GetArrayItem(ids, i)->valuedouble;
        int64_t kr = (int64_t)cJSON_GetArrayItem(krs, i)->valuedouble;
        if (!dbx_exists(&x, SQL(.any = "SELECT 1 FROM players WHERE player_id = $1"), KN_DB_ARGS(I64S(id)))) continue;
        int64_t f = 0;
        if (db_add_funds(&x, id, kr, "reward", NULL, &f) == 0) cJSON_AddItemToArray(funds, cJSON_CreateNumber((double)f));
    }
    cJSON_Delete(ids);
    cJSON_Delete(krs);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "funds", funds);
    return dbx_json(&x, "jsonb_build_object", o);
}
const kn_proc P_REWARDS = {
    "rewards",
    "SELECT jsonb_build_object('funds', coalesce(jsonb_agg(f ORDER BY n), '[]'::jsonb)) FROM ("
    "SELECT n, kn_add_funds(i::int, k::bigint, 'reward', NULL) AS f "
    "FROM jsonb_array_elements_text($1::jsonb) WITH ORDINALITY AS a(i, n) "
    "JOIN jsonb_array_elements_text($2::jsonb) WITH ORDINALITY AS b(k, m) ON n = m "
    "WHERE EXISTS (SELECT 1 FROM players WHERE player_id = i::int)) q",
    run_rewards};

static kn_db_result *run_give_skin(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    int64_t count = 0;
    db_add_skin(&x, PI(0), PI(1), 1, P(2), NULL, &count);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "skin", (double)PI(1));
    cJSON_AddNumberToObject(o, "count", (double)count);
    return dbx_json(&x, "jsonb_build_object", o);
}
const kn_proc P_GIVE_SKIN = {"give_skin", "SELECT jsonb_build_object('skin', $2::int, 'count', kn_add_skin($1, $2, 1, $3, NULL))",
                             run_give_skin};

/* give_skin_n (account, skin, quantity, source, ref): `quantity` units at once, the audit row naming `ref`
 * (a commit's match key: the event kill pools' items, Krunker v10.0.3 updateAccounts {skins: [{id, quantity}]}) */
static kn_db_result *run_give_skin_n(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    int64_t count = 0;
    db_add_skin(&x, PI(0), PI(1), PI(2), P(3), P(4), &count);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "skin", (double)PI(1));
    cJSON_AddNumberToObject(o, "count", (double)count);
    return dbx_json(&x, "jsonb_build_object", o);
}
const kn_proc P_GIVE_SKIN_N = {"give_skin_n",
                               "SELECT jsonb_build_object('skin', $2::int, 'count', kn_add_skin($1, $2, $3::int, $4, $5))",
                               run_give_skin_n};

/* atomic ranked update {elo} clamped at 0; no row when the player is unknown */
static kn_db_result *elo_delta(kn_dbconn *c, int n, const char *const *p, const kn_sql *upd, const kn_sql *get) {
    Dbx x = {c, NULL};
    if (dbx_exec(&x, upd, KN_DB_ARGS(P(0), P(1))) <= 0) {
        const char *names[1] = {"jsonb_build_object"};
        const kn_db_type types[1] = {KN_DB_T_JSON};
        return dbx_finish(&x, kn_db_result_new(1, names, types));
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "elo", (double)dbx_int(&x, get, KN_DB_ARGS(P(0)), 0));
    return dbx_json(&x, "jsonb_build_object", o);
}
#define ELO_PROC(sfx, col)                                                                                       \
    static kn_db_result *run_elo_delta_##sfx(kn_dbconn *c, int n, const char *const *p) {                        \
        return elo_delta(c, n, p, SQL(.any = "UPDATE players SET " col " = {greatest:0, " col " + {int:$2}} WHERE player_id = $1"), \
                         SQL(.any = "SELECT " col " FROM players WHERE player_id = $1"));                       \
    }                                                                                                            \
    const kn_proc P_ELO_DELTA_##sfx = {"elo_delta",                                                              \
                                       "UPDATE players SET " col " = greatest(0, " col " + $2::int) WHERE player_id = $1 " \
                                       "RETURNING jsonb_build_object('elo', " col ")",                          \
                                       run_elo_delta_##sfx};
ELO_PROC(1, "player_elo")
ELO_PROC(2, "player_elo2")
ELO_PROC(4, "player_elo4")

/* (id, chal, kr) -> {chal, funds} */
static kn_db_result *run_challenge(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    dbx_exec(&x, SQL(.any = "UPDATE players SET player_chal = {greatest:player_chal, {int:$2}} WHERE player_id = $1"),
             KN_DB_ARGS(P(0), P(1)));
    kn_db_result *r = dbx_q(&x, SQL(.any = "SELECT player_chal FROM players WHERE player_id = $1"), KN_DB_ARGS(P(0)));
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "chal", r && kn_db_rows(r) ? cJSON_CreateNumber((double)kn_db_get_int64(r, 0, 0, 0))
                                                        : cJSON_CreateNull());
    kn_db_result_free(r);
    int64_t funds = 0;
    db_add_funds(&x, PI(0), PI(2), "challenge", P(1), &funds);
    cJSON_AddNumberToObject(o, "funds", (double)funds);
    return dbx_json(&x, "jsonb_build_object", o);
}
const kn_proc P_CHALLENGE = {
    "challenge",
    "WITH u AS (UPDATE players SET player_chal = greatest(player_chal, $2) WHERE player_id = $1 RETURNING player_chal) "
    "SELECT jsonb_build_object('chal', (SELECT player_chal FROM u), "
    "'funds', kn_add_funds($1, $3, 'challenge', $2::text))",
    run_challenge};

/* (id or "0", ip) -> {banned, reason, expires} */
static kn_db_result *run_ban_check(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    char reason[256];
    int64_t expires = 0;
    int banned = db_active_ban(&x, PI(0), P(1), reason, sizeof reason, &expires);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "banned", banned);
    cJSON_AddItemToObject(o, "reason", banned ? cJSON_CreateString(reason) : cJSON_CreateNull());
    cJSON_AddNumberToObject(o, "expires", (double)expires);
    return dbx_json(&x, "jsonb_build_object", o);
}
const kn_proc P_BAN_CHECK = {
    "ban_check",
    "SELECT jsonb_build_object('banned', CASE WHEN b.reason IS NOT NULL OR b.expires_at IS NOT NULL THEN 1 ELSE 0 END, "
    "'reason', b.reason, 'expires', coalesce(extract(epoch FROM b.expires_at)::bigint, 0)) "
    "FROM kn_active_ban(nullif($1, '0')::int, $2) b",
    run_ban_check};

/* accounts.developers / moderators (json array of names) -> player flags */
static kn_db_result *staff(kn_dbconn *c, int n, const char *const *p, const kn_sql *upd) {
    Dbx x = {c, NULL};
    cJSON *names = P(0) ? cJSON_Parse(P(0)) : NULL;
    const cJSON *it;
    cJSON_ArrayForEach(it, names) {
        if (cJSON_IsString(it)) dbx_exec(&x, upd, KN_DB_ARGS(it->valuestring));
    }
    cJSON_Delete(names);
    return dbx_finish(&x, kn_db_result_command(0));
}
/* A developer's startup grant (every brain start, so a rebuild or restore keeps it): developer and
 * moderator, the verified tick (player_featured), lifetime VIP (player_premium, ms: the value the
 * developers already hold; 315360000000 fits BIGINT / INTEGER64, see migration 0110) and the
 * creator badge. Moderators get only the moderator flag (P_STAFF_MOD). */
#define STAFF_DEV_SET "player_developer = 1, player_moderator = 1, player_featured = 1," \
                      " player_premium = 315360000000, creator_badge = 1"
static kn_db_result *run_staff_dev(kn_dbconn *c, int n, const char *const *p) {
    return staff(c, n, p, SQL(.any = "UPDATE players SET " STAFF_DEV_SET
                                     " WHERE player_id IN (SELECT id FROM accounts WHERE lower(username) = lower($1))"));
}
static kn_db_result *run_staff_mod(kn_dbconn *c, int n, const char *const *p) {
    return staff(c, n, p, SQL(.any = "UPDATE players SET player_moderator = 1"
                                     " WHERE player_id IN (SELECT id FROM accounts WHERE lower(username) = lower($1))"));
}
const kn_proc P_STAFF_DEV = {
    "staff_dev",
    "UPDATE players SET " STAFF_DEV_SET " FROM accounts a "
    "WHERE a.id = player_id AND lower(a.username) IN (SELECT lower(x) FROM jsonb_array_elements_text($1::jsonb) x)",
    run_staff_dev};

/* A developer also owns every progression badge (shared/include/badges.h, indices 0..BADGE_COUNT-1),
 * one player_badges row each, kept once (the key is account_id, badge). A separate proc from the
 * flags: PostgreSQL runs one statement per proc. */
#define KN_STR_(x) #x
#define KN_STR(x) KN_STR_(x)
static kn_db_result *run_staff_dev_badges(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    cJSON *names = P(0) ? cJSON_Parse(P(0)) : NULL;
    const cJSON *it;
    cJSON_ArrayForEach(it, names) {
        if (!cJSON_IsString(it)) continue;
        for (int b = 0; b < BADGE_COUNT && !dbx_failed(&x); b++)
            dbx_exec(&x, SQL(.any = "{insert_ignore} INTO player_badges (account_id, badge)"
                                    " SELECT id, {int:$2} FROM accounts WHERE lower(username) = lower($1) {ignore_conflicts}"),
                     KN_DB_ARGS(it->valuestring, I64S(b)));
    }
    cJSON_Delete(names);
    return dbx_finish(&x, kn_db_result_command(0));
}
const kn_proc P_STAFF_DEV_BADGES = {
    "staff_dev_badges",
    "INSERT INTO player_badges (account_id, badge) SELECT a.id, b.badge FROM accounts a"
    " CROSS JOIN generate_series(0, " KN_STR(BADGE_COUNT - 1) ") AS b(badge)"
    " WHERE lower(a.username) IN (SELECT lower(x) FROM jsonb_array_elements_text($1::jsonb) x)"
    " ON CONFLICT DO NOTHING",
    run_staff_dev_badges};
const kn_proc P_STAFF_MOD = {
    "staff_mod",
    "UPDATE players SET player_moderator = 1 FROM accounts a "
    "WHERE a.id = player_id AND lower(a.username) IN (SELECT lower(x) FROM jsonb_array_elements_text($1::jsonb) x)",
    run_staff_mod};

/* Community role: the local row of an account owned by the central account
 * service (id = the central account id, no password). Creates or renames it and
 * its players row; a local row that holds the name under another id is renamed
 * out of the way ("name~<id>"). -> account object. C on every driver. */
static kn_db_result *run_ensure_central(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const char *id = P(0), *name = P(1);
    if (!id || PI(0) <= 0 || !name || !*name) return kn_db_result_user_error("Invalid account.");
    dbx_exec(&x,
             SQL(.pg = "UPDATE accounts SET username = left(username, 20) || '~' || id"
                       " WHERE lower(username) = lower($2) AND id <> $1::int",
                 .any = "UPDATE accounts SET username = substr(username, 1, 20) || '~' || id"
                        " WHERE lower(username) = lower($2) AND id <> $1"),
             KN_DB_ARGS(id, name));
    dbx_exec(&x,
             SQL(.pg = "INSERT INTO accounts (id, username, password_hash) VALUES ($1::int, $2, '')"
                       " ON CONFLICT (id) DO UPDATE SET username = EXCLUDED.username",
                 .sqlite = "INSERT INTO accounts (id, username, password_hash) VALUES ($1, $2, '')"
                           " ON CONFLICT (id) DO UPDATE SET username = excluded.username",
                 .mysql = "INSERT INTO accounts (id, username, password_hash) VALUES ($1, $2, '')"
                          " ON DUPLICATE KEY UPDATE username = VALUES(username)"),
             KN_DB_ARGS(id, name));
    dbx_exec(&x,
             SQL(.pg = "INSERT INTO players (player_id) VALUES ($1::int) ON CONFLICT DO NOTHING",
                 .any = "{insert_ignore} INTO players (player_id) VALUES ($1) {ignore_conflicts}"),
             KN_DB_ARGS(id));
    if (dbx_is_pg(&x)) /* keep locally generated ids clear of central ones */
        dbx_exec(&x, SQL(.pg = "SELECT setval(pg_get_serial_sequence('accounts', 'id'), greatest(max(id), 1)) FROM accounts"),
                 0, NULL);
    return dbx_json(&x, "account", db_account_json(&x, PI(0)));
}
const kn_proc P_ENSURE_CENTRAL = {"ensure_central", NULL, run_ensure_central};

/* ------------------------------------------------------------------ store (postgres/0106 for the KR wheel / daily reward) */

/* v7.1.4 / v10.0.2 prize wheel spin (store.c), C on every driver (one transaction):
 * (id, ref, price_each, skins "a,b,c", ticket) -> {funds}. A ticket >= 0 is consumed first, one
 * per roll (a TICKET item's wheel or a wheel's ticketIndex: no KR; v10 multi-spins take `multi`
 * tickets); else every roll is charged price_each with its own kr_transactions row. Each won skin
 * is granted with a skin_transactions row (source "spin", ref "<wheel>:<price_each>" or
 * "t<ticket>:0": the spin history reads them back). */
static kn_db_result *run_spin_multi(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const int64_t id = PI(0), price = PI(2), ticket = P(4) ? PI(4) : -1;
    const char *ref = P(1), *list = P(3);
    if (!list || !*list) return kn_db_result_user_error("Error generating item");
    int64_t rolls = 1;
    for (const char *q = list; *q; q++) rolls += *q == ',';
    if (ticket >= 0) db_add_skin(&x, id, ticket, -rolls, "ticket", ref, NULL);
    for (const char *q = list; *q && !dbx_failed(&x);) {
        char *end = NULL;
        const long long skin = strtoll(q, &end, 10);
        if (!end || end == q) break;
        if (price > 0) db_add_funds(&x, id, -price, "spin", ref, NULL);
        db_add_skin(&x, id, skin, 1, "spin", ref, NULL);
        q = *end == ',' ? end + 1 : end;
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "funds",
                            (double)dbx_int(&x, SQL(.any = "SELECT player_funds FROM players WHERE player_id = $1"), KN_DB_ARGS(P(0)), 0));
    return dbx_json(&x, "spin", o);
}
const kn_proc P_SPIN_MULTI = {"spin_multi", NULL, run_spin_multi};

static kn_db_result *daily_kr(Dbx *x, const char *id, const char *kr, const char *cd, const char *now,
                              const char *source) {
    if (dbx_exec(x, SQL(.any = "UPDATE players SET player_lastreward = {int:$2} WHERE player_id = $1"
                               " AND player_lastreward <= {int:$2} - {int:$3}"),
                 KN_DB_ARGS(id, now, cd)) == 0)
        dbx_refuse(x, "Reward not ready yet");
    int64_t funds = 0, k = kr ? strtoll(kr, NULL, 10) : 0;
    db_add_funds(x, strtoll(id, NULL, 10), k > 0 ? k : 0, source, NULL, &funds);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "lastReward", (double)(now ? strtoll(now, NULL, 10) : 0));
    cJSON_AddNumberToObject(o, "funds", (double)funds);
    return dbx_json(x, "kn_daily_kr", o);
}

/* (id, kr, cooldown_ms, now_ms) the Free KR wheel */
static kn_db_result *run_daily_kr(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    return daily_kr(&x, P(0), P(1), P(2), P(3), "freekr");
}
const kn_proc P_DAILY_KR = {"daily_kr", "SELECT kn_daily_kr($1, $2, $3, $4, 'freekr')", run_daily_kr};

/* (id, kr, cooldown_ms, now_ms, min_level, rank_var) the daily reward ("clm") */
static kn_db_result *run_claim_daily(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    kn_db_result *r = dbx_q(&x, SQL(.any = "SELECT player_score FROM players WHERE player_id = $1"), KN_DB_ARGS(P(0)));
    if (!r) return dbx_finish(&x, NULL);
    if (kn_db_rows(r) < 1) { /* postgres: no row -> NULL result */
        kn_db_result_free(r);
        const char *names[1] = {"case"};
        const kn_db_type types[1] = {KN_DB_T_JSON};
        return dbx_finish(&x, kn_db_result_new(1, names, types));
    }
    const char *rv = P(5);
    int level = db_level(kn_db_get_int64(r, 0, 0, 0), rv ? strtod(rv, NULL) : 0.03);
    kn_db_result_free(r);
    if (level < PI(4)) {
        char msg[64];
        snprintf(msg, sizeof msg, "Must be level %lld", (long long)PI(4));
        return dbx_finish(&x, kn_db_result_user_error(msg));
    }
    return daily_kr(&x, P(0), P(1), P(2), P(3), "daily");
}
const kn_proc P_CLAIM_DAILY = {
    "claim_daily",
    "SELECT CASE WHEN kn_level(player_score, $6::float8) < $5::int "
    "THEN kn_fail('Must be level ' || $5::text)::text::jsonb "
    "ELSE kn_daily_kr($1, $2, $3, $4, 'daily') END FROM players WHERE player_id = $1",
    run_claim_daily};
