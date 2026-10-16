/*
 * services/brain/db_double_xp.c - Krunker v10.0.2's double XP database work (double_xp.h,
 * migration 0341_double_xp). C on every driver (no .pg text).
 *
 *   P_DXP_CLAIM (id, now_ms, provider, ad_ref) -> {playerScore, xp, games} | refusal
 *       v10's "rqdxp": doubles the account's last match when it is claimable (dxp_offer_claimable).
 *       One transaction behind the players row lock: account.score = playerScore (the match's XP
 *       once more; v10's RECIEVE_DOUBLE_XP handler sets nothing else: no clan, no class score),
 *       claimed_games = games (lastDoubleAttempt), and the claim is logged in double_xp_claims.
 *   db_dxp_state(x, id, now)               -> {offer: {games, xp, claimable} | null, doubleClassXP,
 *       doubleRaidDrops}: the account object's "dxp" (db_common.c; kn_dxp on PostgreSQL).
 *   db_dxp_record_match(x, id, offered, xp): the commit's offer for the match just committed
 *       (db_accounts.c run_commit_stats; kn_commit_stats on PostgreSQL).
 */
#include "db_procs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "double_xp.h"
#include "knsvc/json.h"

#define P(i) (n > (i) ? p[i] : NULL)
#define PS(i) (P(i) ? P(i) : "")
#define PI(i) pint(P(i))

static int64_t pint(const char *s) { return s ? strtoll(s, NULL, 10) : 0; }

static int64_t games_of(Dbx *x, const int64_t id) {
    return dbx_int(x, SQL(.any = "SELECT player_games_played FROM players WHERE player_id = $1"), KN_DB_ARGS(I64S(id)), 0);
}

void db_dxp_record_match(Dbx *x, const int64_t id, const int offered, const int64_t xp) {
    dbx_exec(x,
             SQL(.any = "INSERT INTO double_xp_offers (account_id, offered, games, xp)"
                        " VALUES ($1, {int:$2}, (SELECT player_games_played FROM players WHERE player_id = $1), {int:$3})"
                        " ON CONFLICT (account_id) DO UPDATE SET offered = excluded.offered, games = excluded.games,"
                        " xp = excluded.xp"),
             KN_DB_ARGS(I64S(id), I64S(offered ? 1 : 0), I64S(dxp_match_xp(xp))));
}

cJSON *db_dxp_state(Dbx *x, const int64_t id, const int64_t now) {
    cJSON *o = cJSON_CreateObject();
    cJSON *row = dbx_row(x,
        SQL(.any = "SELECT offered AS offered__i, games AS games__i, xp AS xp__i, claimed_games AS claimed__i"
                   " FROM double_xp_offers WHERE account_id = $1"),
        KN_DB_ARGS(I64S(id)));
    const int64_t games = games_of(x, id);
    if (row && kn_json_int(row, "offered", 0)) {
        cJSON *offer = cJSON_CreateObject();
        const int claimable = dxp_offer_claimable(1, games, kn_json_int(row, "games", 0), kn_json_int(row, "claimed", 0));
        cJSON_AddNumberToObject(offer, "games", (double) kn_json_int(row, "games", 0));
        cJSON_AddNumberToObject(offer, "xp", (double) kn_json_int(row, "xp", 0));
        cJSON_AddBoolToObject(offer, "claimable", claimable);
        cJSON_AddItemToObject(o, "offer", offer);
    } else {
        cJSON_AddNullToObject(o, "offer");
    }
    cJSON_Delete(row);
    const DxpSchedule s = dxp_schedule(now);
    cJSON_AddBoolToObject(o, "doubleClassXP", s.double_class_xp);
    cJSON_AddBoolToObject(o, "doubleRaidDrops", s.double_raid_drops);
    return o;
}

/* (id, now_ms, provider, ad_ref) -> {playerScore, xp, games} */
static kn_db_result *run_claim(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const char *id = PS(0), *ref = P(3) && *P(3) ? P(3) : NULL;
    const int64_t acct = PI(0), now = PI(1);
    /* the players row first: a claim and a commit of one account queue behind it (postgres) */
    dbx_exec(&x, SQL(.any = "UPDATE players SET player_funds = player_funds WHERE player_id = $1"), KN_DB_ARGS(id));
    const int64_t games = games_of(&x, acct);
    cJSON *row = dbx_row(&x,
        SQL(.any = "SELECT offered AS offered__i, games AS games__i, xp AS xp__i, claimed_games AS claimed__i"
                   " FROM double_xp_offers WHERE account_id = $1"),
        KN_DB_ARGS(id));
    if (dbx_failed(&x)) {
        cJSON_Delete(row);
        return dbx_finish(&x, NULL);
    }
    if (!row || !dxp_offer_claimable(kn_json_int(row, "offered", 0), games, kn_json_int(row, "games", 0), kn_json_int(row, "claimed", 0))) {
        cJSON_Delete(row);
        dbx_refuse(&x, "No double XP for this match");
        return dbx_finish(&x, NULL);
    }
    const int64_t xp = dxp_match_xp(kn_json_int(row, "xp", 0));
    cJSON_Delete(row);
    const int64_t score = dbx_int(&x, SQL(.any = "SELECT player_score FROM players WHERE player_id = $1"), KN_DB_ARGS(id), 0);
    const int64_t next = score + xp > DXP_XP_MAX ? DXP_XP_MAX : score + xp;
    dbx_exec(&x, SQL(.any = "UPDATE players SET player_score = {int:$2}, updated_at = {now} WHERE player_id = $1"),
             KN_DB_ARGS(id, I64S(next)));
    dbx_exec(&x, SQL(.any = "UPDATE double_xp_offers SET claimed_games = {int:$2} WHERE account_id = $1"),
             KN_DB_ARGS(id, I64S(games)));
    dbx_exec(&x, SQL(.any = "INSERT INTO double_xp_claims (account_id, games, xp, provider, ad_ref, claimed_at)"
                            " VALUES ($1, {int:$2}, {int:$3}, $4, $5, {int:$6})"),
             KN_DB_ARGS(id, I64S(games), I64S(xp), PS(2), ref, I64S(now)));
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "playerScore", (double) next);
    cJSON_AddNumberToObject(o, "xp", (double) xp);
    cJSON_AddNumberToObject(o, "games", (double) games);
    return dbx_json(&x, "dxp_claim", o);
}
const kn_proc P_DXP_CLAIM = {"dxp_claim", NULL, run_claim};
