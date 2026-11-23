/*
 * services/brain/db_krunkcup.c - Krunker v10.0.7's World KrunkCup 2026 on the tables of 0399_krunkcup. C on every
 * driver (no .pg text).
 *
 * A team is a style of the KrunkCup Jerseys (store 7794, config.krunkCupJersey, deob-10.0.7.js:19788): its id is the
 * style index into the item's 48 `styles` (the Leaderboards' KrunkCup names it styles[id] without " Away" and draws
 * its _s<id> preview, 140946-140956). The view's empty text is how a player joins a team: "Equip a KrunkCup jersey
 * with a team style and play!" (140284).
 *
 *   db_krunkcup_credit (P_PROGRESS, after a stats commit): a match the game server saw played in the jerseys
 *        ("kcJersey") adds its kills, score and game to the team of the account's style on them (item_styles of
 *        0393, the stack's or the first styled copy's; none = style 0). The account must own the jerseys.
 *   P_KC_WRISTBAND       (account) -> {claimed, eligible, team, item_id}: krunkcup.getWristbandStatus
 *        (GET /krunkcup/wristband, 27430-27434). eligible: the account played for a team; team: the band's team
 *        once claimed, else the team it played the most for (score), null without one.
 *   P_KC_WRISTBAND_CLAIM (account, team | -1) -> the same object, claimed: krunkcup.claimWristband
 *        (POST /krunkcup/wristband/claim {team}, 27435-27440): the KrunkCup Participation Band (store 7803,
 *        config.krunkCupWristband 19789) in the team's style, once. The team must be one the account played for
 *        (-1: the one it played the most for). v10.0.7 has no caller of either route, so their shapes are
 *        KrunkNative's.
 */
#include "db_procs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "knsvc/json.h"

#define P(i) (n > (i) ? p[i] : NULL)
#define PI(i) pint(P(i))

#define KC_JERSEY 7794    /* config.krunkCupJersey */
#define KC_WRISTBAND 7803 /* config.krunkCupWristband */
#define KC_TEAMS 48       /* the jerseys' (and the band's) styles: assets/skins.json 7794 / 7803 */

static int64_t pint(const char *s) { return s ? strtoll(s, NULL, 10) : 0; }

static double jnum(const cJSON *o, const char *k) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? v->valuedouble : 0.0;
}

void db_krunkcup_credit(Dbx *x, const int64_t id, const cJSON *in) {
    const cJSON *worn = cJSON_GetObjectItemCaseSensitive(in, "kcJersey");
    if (!(cJSON_IsTrue(worn) || (cJSON_IsNumber(worn) && worn->valuedouble != 0)) || dbx_failed(x)) return;
    const int64_t kills = (int64_t) jnum(in, "kills"), score = (int64_t) jnum(in, "score"), games = (int64_t) jnum(in, "games");
    if (kills <= 0 && score <= 0 && games <= 0) return;
    if (!dbx_exists(x, SQL(.any = "SELECT 1 FROM inventory WHERE account_id = $1 AND skin_id = {int:$2} AND count > 0"),
                    KN_DB_ARGS(I64S(id), I64S(KC_JERSEY))))
        return;
    int64_t team = dbx_int(x, SQL(.any = "SELECT style FROM item_styles WHERE account_id = $1 AND skin_id = {int:$2} ORDER BY copy_id LIMIT 1"),
                           KN_DB_ARGS(I64S(id), I64S(KC_JERSEY)), 0);
    if (team < 0 || team >= KC_TEAMS) team = 0;
    dbx_exec(x,
             SQL(.sqlite = "INSERT INTO krunkcup_contributions (account_id, team, kills, score, games) VALUES ($1, $2, $3, $4, $5)"
                           " ON CONFLICT (account_id, team) DO UPDATE SET kills = kills + excluded.kills, score = score + excluded.score,"
                           " games = games + excluded.games, updated_at = strftime('%Y-%m-%d %H:%M:%f','now')",
                 .mysql = "INSERT INTO krunkcup_contributions (account_id, team, kills, score, games) VALUES ($1, $2, $3, $4, $5)"
                          " ON DUPLICATE KEY UPDATE kills = kills + VALUES(kills), score = score + VALUES(score),"
                          " games = games + VALUES(games), updated_at = NOW(3)",
                 .pg = "INSERT INTO krunkcup_contributions (account_id, team, kills, score, games)"
                       " VALUES ($1::int, $2::int, $3::bigint, $4::bigint, $5::bigint)"
                       " ON CONFLICT (account_id, team) DO UPDATE SET kills = krunkcup_contributions.kills + EXCLUDED.kills,"
                       " score = krunkcup_contributions.score + EXCLUDED.score, games = krunkcup_contributions.games + EXCLUDED.games,"
                       " updated_at = now()"),
             KN_DB_ARGS(I64S(id), I64S(team), I64S(kills > 0 ? kills : 0), I64S(score > 0 ? score : 0), I64S(games > 0 ? games : 0)));
}

/* the team the account played the most for (score, then games, then the lower id), -1 none */
static int64_t top_team(Dbx *x, const int64_t id) {
    return dbx_int(x, SQL(.any = "SELECT team FROM krunkcup_contributions WHERE account_id = $1 AND games > 0"
                                 " ORDER BY score DESC, games DESC, team LIMIT 1"),
                   KN_DB_ARGS(I64S(id)), -1);
}

static cJSON *status_of(Dbx *x, const int64_t id) {
    const int64_t claimed = dbx_int(x, SQL(.any = "SELECT team FROM krunkcup_wristbands WHERE account_id = $1"), KN_DB_ARGS(I64S(id)), -1);
    const int64_t top = top_team(x, id);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "claimed", claimed >= 0);
    cJSON_AddBoolToObject(o, "eligible", top >= 0 || claimed >= 0);
    const int64_t team = claimed >= 0 ? claimed : top;
    if (team >= 0) cJSON_AddNumberToObject(o, "team", (double) team);
    else cJSON_AddNullToObject(o, "team");
    cJSON_AddNumberToObject(o, "item_id", KC_WRISTBAND);
    return o;
}

static int account_exists(Dbx *x, const int64_t id) {
    if (id > 0 && dbx_exists(x, SQL(.any = "SELECT 1 FROM players WHERE player_id = $1"), KN_DB_ARGS(I64S(id)))) return 1;
    if (!dbx_failed(x)) dbx_refuse(x, "Account not found.");
    return 0;
}

static kn_db_result *run_wristband(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const int64_t id = PI(0);
    if (!account_exists(&x, id)) return dbx_finish(&x, NULL);
    return dbx_json(&x, "wristband", status_of(&x, id));
}
const kn_proc P_KC_WRISTBAND = {"kc_wristband", NULL, run_wristband};

static kn_db_result *run_wristband_claim(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const int64_t id = PI(0);
    int64_t team = P(1) && P(1)[0] ? PI(1) : -1;
    if (!account_exists(&x, id)) return dbx_finish(&x, NULL);
    if (dbx_exists(&x, SQL(.any = "SELECT 1 FROM krunkcup_wristbands WHERE account_id = $1"), KN_DB_ARGS(I64S(id)))) {
        dbx_refuse(&x, "You already claimed your KrunkCup Participation Band.");
        return dbx_finish(&x, NULL);
    }
    if (team < 0) team = top_team(&x, id);
    else if (team >= KC_TEAMS ||
             !dbx_exists(&x, SQL(.any = "SELECT 1 FROM krunkcup_contributions WHERE account_id = $1 AND team = {int:$2} AND games > 0"),
                         KN_DB_ARGS(I64S(id), I64S(team))))
        team = -2;
    if (team == -2) {
        if (!dbx_failed(&x)) dbx_refuse(&x, "Play a match for that team in its KrunkCup jersey first.");
        return dbx_finish(&x, NULL);
    }
    if (team < 0) {
        if (!dbx_failed(&x)) dbx_refuse(&x, "Equip a KrunkCup jersey with a team style and play!");
        return dbx_finish(&x, NULL);
    }
    /* one row per account (the primary key): a second claim racing this one fails here and rolls back */
    if (dbx_exec(&x, SQL(.any = "INSERT INTO krunkcup_wristbands (account_id, team) VALUES ({int:$1}, {int:$2})"),
                 KN_DB_ARGS(I64S(id), I64S(team))) < 0)
        return dbx_finish(&x, NULL);
    if (db_add_skin(&x, id, KC_WRISTBAND, 1, "krunkcup", "wristband", NULL) < 0) return dbx_finish(&x, NULL);
    /* the band's style is the team (the stack's item_styles row, as items.setStyle; style 0 has none) */
    if (team == 0 && dbx_exec(&x, SQL(.any = "DELETE FROM item_styles WHERE account_id = $1 AND skin_id = {int:$2} AND copy_id = 0"),
                              KN_DB_ARGS(I64S(id), I64S(KC_WRISTBAND))) < 0)
        return dbx_finish(&x, NULL);
    if (team > 0 &&
        dbx_exec(&x,
                 SQL(.any = "INSERT INTO item_styles (account_id, skin_id, copy_id, style) VALUES ({int:$1}, {int:$2}, 0, {int:$3})"
                            " ON CONFLICT (account_id, skin_id, copy_id) DO UPDATE SET style = excluded.style",
                     .mysql = "INSERT INTO item_styles (account_id, skin_id, copy_id, style) VALUES ({int:$1}, {int:$2}, 0, {int:$3})"
                              " ON DUPLICATE KEY UPDATE style = VALUES(style)"),
                 KN_DB_ARGS(I64S(id), I64S(KC_WRISTBAND), I64S(team))) < 0)
        return dbx_finish(&x, NULL);
    return dbx_json(&x, "wristband", status_of(&x, id));
}
const kn_proc P_KC_WRISTBAND_CLAIM = {"kc_wristband_claim", NULL, run_wristband_claim};
