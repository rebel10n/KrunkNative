/*
 * services/brain/db_procs.h - the coordinator's multi-statement database
 * operations as kn_procs (knsvc/db.h): on PostgreSQL each runs today's
 * server-side SQL function (services/db/migrations/postgres, .pg text); on
 * every other driver the C implementation in db_accounts.c / db_social.c /
 * db_mm.c runs on a pool connection inside a transaction. Both return the same
 * shape (usually one JSON column) so the callers do not care which ran.
 *
 * C implementations run on database worker threads: they only use their
 * parameters (text, like the SQL function's) and the Dbx helpers below,
 * never brain state.
 */
#ifndef KN_BRAIN_DB_PROCS_H
#define KN_BRAIN_DB_PROCS_H

#include <stdint.h>

#include "cJSON.h"
#include "knsvc/store.h"

/* ---- synchronous helpers for C procs: the first failure is kept in err */
typedef struct Dbx {
    kn_dbconn *c;
    kn_db_result *err;
} Dbx;

/* Inline kn_sql: SQL(.any = "...") or SQL(.sqlite = "...", .mysql = "..."). */
#define SQL(...) (&(const kn_sql){__VA_ARGS__})
/* int64 -> parameter text (compound literal: valid in the enclosing block) */
#define I64S(v) dbx_i64s((char[24]){0}, (int64_t)(v))
const char *dbx_i64s(char buf[24], int64_t v);

int dbx_failed(const Dbx *x);
int dbx_is_pg(const Dbx *x);
int dbx_is_mysql(const Dbx *x);
/* Ok result (caller frees) or NULL after a failure (kept in x->err). Once
 * failed, every later call is skipped and returns NULL / -1 / def. */
kn_db_result *dbx_q(Dbx *x, const kn_sql *q, int n, const char *const *p);
/* Affected rows, -1 on failure. */
int64_t dbx_exec(Dbx *x, const kn_sql *q, int n, const char *const *p);
/* First cell of the first row (def without rows / NULL). */
int64_t dbx_int(Dbx *x, const kn_sql *q, int n, const char *const *p, int64_t def);
/* First cell as malloc'd text, NULL without rows / NULL / failure. */
char *dbx_text(Dbx *x, const kn_sql *q, int n, const char *const *p);
/* 1 when the query returns at least one row. */
int dbx_exists(Dbx *x, const kn_sql *q, int n, const char *const *p);
/* Last auto-increment id of an INSERT (sqlite / mysql). */
int64_t dbx_insert_id(Dbx *x, const kn_sql *q, int n, const char *const *p);
/* Record a user-facing refusal (SQLSTATE P0001); returns -1. */
int dbx_refuse(Dbx *x, const char *msg);
/* Final result of a proc: the kept failure (res freed) or res. */
kn_db_result *dbx_finish(Dbx *x, kn_db_result *res);
/* Same, wrapping v (consumed) as one JSON column `name` (SQL NULL when v is NULL). */
kn_db_result *dbx_json(Dbx *x, const char *name, cJSON *v);
/* Query rows as a JSON array (kn_db_result_to_json); NULL on failure. */
cJSON *dbx_rows(Dbx *x, const kn_sql *q, int n, const char *const *p);
/* First row as a JSON object, NULL without rows / on failure. */
cJSON *dbx_row(Dbx *x, const kn_sql *q, int n, const char *const *p);

/* ---- shared domain helpers (C versions of postgres/0103-0104 functions) */
/* kn_html */
char *db_html(const char *s);
/* kn_level(score, rank_var) */
int db_level(int64_t score, double rank_var);
/* kn_active_ban: found 1 when an active ban matches (reason may be "" / expires 0 = permanent) */
int db_active_ban(Dbx *x, int64_t account_id, const char *ip, char *reason, size_t reason_len, int64_t *expires_s);
/* kn_add_funds / kn_add_skin; -1 (refusal recorded) when not enough / not owned / no account */
int db_add_funds(Dbx *x, int64_t id, int64_t amount, const char *source, const char *ref, int64_t *funds);
int db_add_skin(Dbx *x, int64_t id, int64_t skin, int64_t delta, const char *source, const char *ref, int64_t *count);
/* kn_account_json (NULL when the account does not exist) */
cJSON *db_account_json(Dbx *x, int64_t id);
/* kn_account_id(name): 0 when unknown */
int64_t db_account_id(Dbx *x, const char *name);
/* Player row of player_rows as JSON (NULL when unknown) */
cJSON *db_player_row(Dbx *x, int64_t id);

/* ---- accounts (db_accounts.c) */
extern const kn_proc P_LOGIN_SUCCESS, P_LOGIN_FAILED, P_REGISTER, P_REGISTER_PRECHECK, P_LOGIN_LOOKUP,
    P_SESSION_ACCOUNT, P_SET_PASSWORD, P_SET_ALIAS, P_ACCOUNT_JSON, P_COMMIT_STATS, P_REWARDS, P_GIVE_SKIN,
    P_ELO_DELTA_1, P_ELO_DELTA_2, P_ELO_DELTA_4, P_CHALLENGE, P_BAN_CHECK, P_STAFF_DEV, P_STAFF_MOD,
    P_ENSURE_CENTRAL, P_SPIN_MULTI, P_DAILY_KR, P_CLAIM_DAILY;

/* ---- social (db_social.c) */
extern const kn_proc P_POST_FULL, P_FEED, P_GIFT, P_CLAN_CREATE, P_CLAN_REQUEST, P_CLAN_DATA_OF, P_CLAN_LEAVE,
    P_CLAN_KICK, P_CLAN_ROLE, P_CLAN_FUNDS, P_CLAN_ANSWER, P_CLAN_PAGE, P_MAIL_READ, P_MARKET_ITEM,
    P_TRADE_PAGE, P_MARKET_LIST, P_MARKET_UNLIST, P_MARKET_BUY, P_QUICK_SELL, P_TRADE_INVENTORIES, P_TRADE_CREATE,
    P_TRADE_ACCEPT, P_TRADE_CLOSE, P_CHAT_THREAD, P_PROFILE, P_MAP_PAGE, P_MOD_PAGE, P_PUBLISH_MAP, P_SET_HACK,
    P_SET_FEATURED;

/* ---- KrunkScript storage and map KR rewards for game servers (db_storage.c) */
extern const kn_proc P_STORAGE_UPDATE, P_STORAGE_LOAD, P_MAP_REWARD;

/* ---- player-viewer WP-E part 2c: badges, v7 challenges, medal counts (db_progress.c) */
extern const kn_proc P_CHALLENGES, P_PROGRESS, P_BADGE_SELECT, P_BADGE_CLAIM, P_MEDALS;

/* ---- Krunker v10.0.2 ranked (db_ranked.c, 0205_ranked_v10) */
extern const kn_proc P_RK_JOIN, P_RK_POLL, P_RK_EXPIRE, P_RK_MATCH_RUN, P_RK_MATCH_READY,
    P_RK_MATCH_FAILED, P_RK_MATCH_FINISH, P_RK_CLAIM, P_RK_SEASON_CLOSE, P_RK_PURCHASE;

/* ---- Krunker v10.0.2 Turf Wars (db_turf.c, 0206_turf_wars) */
extern const kn_proc P_TURF_RECORD, P_TURF_SETTLE, P_TURF_LOOT_SET;

/* ---- Krunker v7.1.4 seasons and the Battle Pass (db_season.c, 0108_seasons) */
extern const kn_proc P_SEASON_ENSURE, P_BP_DATA, P_BP_CLAIM, P_BP_BUY, P_BP_SKIP, P_SEASON_STATS;
/* The season's active pass (malloc'd; NULL when none / on failure); *row: its battlepasses row. */
struct BattlePass *db_bp_load(Dbx *x, int season, cJSON **row);
/* checkBadges with the season badges: adds every claimable badge whose rule holds (stats =
 * players.player_stats); returns the added indices. */
cJSON *db_season_check_badges(Dbx *x, int64_t id, int season, int64_t now_s, const cJSON *stats);
/* P_PROGRESS's season part for one committed match: season_stats, the match BPP (payload "bpp",
 * capped) and the challenges' REW.bpp. Returns tmpBPData (NULL without a pass / BPP) and the new
 * account.BP in *progress (bppd). */
cJSON *db_season_match(Dbx *x, int64_t id, int season, int64_t now_s, const cJSON *payload, int64_t challenge_bpp,
                       cJSON **progress);

/* ---- account security: v7.1.4 2FA (TOTP), email verification (db_security.c, 0109) */
extern const kn_proc P_EMAIL_STATE, P_EMAIL_REQUEST, P_EMAIL_VERIFY, P_TWOFA_CONFIRM, P_TWOFA_DISABLE, P_TWOFA_LOGIN;

/* ---- matchmaking (db_mm.c) */
extern const kn_proc P_RQ_JOIN, P_RQ_STATE, P_RQ_ACTION, P_RQ_EXPIRE, P_RQ_LOCK, P_RQ_CANDIDATES, P_RQ_MATCH_CREATE,
    P_RQ_MATCH_READY, P_RQ_MATCH_FAILED, P_RQ_MATCH_FINISH, P_RQ_RATINGS, P_MM_NODE_TAKEOVER, P_MM_NODE_UPDATE,
    P_MM_HOST_CLAIM, P_MM_HOST_REQUEST, P_MM_NODE_HEARTBEAT, P_MM_REFRESH_NODES;

#endif
