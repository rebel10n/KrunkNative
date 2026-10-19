/*
 * services/brain/db_season.c - Krunker v7.1.4 seasons and the Battle Pass on the tables of
 * migration 0108_seasons (doc/backend.md 7.8). C on every driver (no .pg text): the rules are
 * shared/src/season.c, the same code the client and the tests use.
 *
 *   P_SEASON_ENSURE (season)                 -> {season, created}   kn_brain start: the
 *                    accounts.current_season row (insert only; older open seasons get ends_at)
 *   P_BP_DATA      (id | 0, season)          -> {bp, progress}      "bpdl" (io_dataLoaded +
 *                    io_syncBpProg): the season's pass in v7's qs_battlepass shape and the
 *                    account's progress {id, bpp, tier, items_claimed}
 *   P_BP_CLAIM     (id, item, season, all)   -> {progress, sk, id, tier, col, items, funds}
 *                    "bpclm" / v10 "bpclmAll" (all = 1: every claimable card); the cards' skins and
 *                    v10 krdata KR
 *   P_BP_BUY       (id, tier, season, now_s, gift name | NULL) -> {progress, tier, funds, price,
 *                    newBadges, gift {id, name, progress}}
 *                    "xstbp": the VIP tier, paid in KR (kr_transactions source 'battlepass'); v10
 *                    selectBPGift: for another player (the payer's own pass is unchanged)
 *   P_BP_SKIP      (id, levels, season, now_s) -> {progress, levels, funds, price, newBadges}
 *                    "xstbpl": skipped levels in KR (source 'battlepass-skip')
 *   P_SEASON_STATS (name | id, season)       -> {season, name, kills, deaths, wins, games,
 *                    score, timePlayed, bpp, tier, level}   the profile's season block
 *
 * The match side (BPP of a public match, challenge REW.bpp, season_stats) runs in P_PROGRESS
 * (db_progress.c) through db_season_match().
 */
#include "db_procs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "badges.h"
#include "knsvc/json.h"
#include "progression.h"
#include "junk.h"
#include "season.h"

#define P(i) (n > (i) ? p[i] : NULL)
#define PI(i) pint(P(i))

static int64_t pint(const char *s) { return s ? strtoll(s, NULL, 10) : 0; }

static double jnum(const cJSON *o, const char *k) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? v->valuedouble : cJSON_IsString(v) && v->valuestring ? strtod(v->valuestring, NULL) : 0.0;
}

static const char *jstr(const cJSON *o, const char *k) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : "";
}

/* ------------------------------------------------------------------ the pass */

/* The active pass of `season` (status 1, the highest bp_id), NULL when none / on failure.
 * *row (optional) gets the row as JSON (caller frees). */
BattlePass *db_bp_load(Dbx *x, const int season, cJSON **row) {
    if (row) *row = NULL;
    cJSON *r = dbx_row(x, SQL(.any = "SELECT bp_id AS id__i, season AS season__i, name, descr, bpp_perlevel AS bpp_perlevel__i,"
                                     " itemcount AS itemcount__i, tier1_name, tier1_price_kr AS tier1_price_kr__i,"
                                     " skip_price_kr AS skip_price_kr__i, itemdata, leveldata, krdata, jnkdata,"
                                     " {epoch_ms:startdate} AS start_ms__f, {epoch_ms:enddate} AS end_ms__f"
                                     " FROM battlepasses WHERE season = {int:$1} AND status = 1 ORDER BY bp_id DESC LIMIT 1"),
                       KN_DB_ARGS(I64S(season)));
    if (!r) return NULL;
    BattlePass *bp = calloc(1, sizeof *bp);
    if (!bp) {
        cJSON_Delete(r);
        return NULL;
    }
    bp->id = (int) jnum(r, "id");
    bp->season = (int) jnum(r, "season");
    snprintf(bp->name, sizeof bp->name, "%s", jstr(r, "name"));
    bp->bpp_per_level = (int) jnum(r, "bpp_perlevel");
    bp->item_count = (int) jnum(r, "itemcount");
    snprintf(bp->tier1_name, sizeof bp->tier1_name, "%s", jstr(r, "tier1_name"));
    bp->tier1_price_kr = (int) jnum(r, "tier1_price_kr");
    bp->skip_price_kr = (int) jnum(r, "skip_price_kr");
    bp->start_s = (int64_t) (jnum(r, "start_ms") / 1000.0);
    bp->end_s = (int64_t) (jnum(r, "end_ms") / 1000.0);
    bp_parse_items(bp, jstr(r, "itemdata"));
    bp_parse_levels(bp, jstr(r, "leveldata")); /* v10.0.2 (0116_v10_rewards) */
    bp_parse_rewards(bp, jstr(r, "krdata"), jstr(r, "jnkdata"));
    if (row) *row = r;
    else cJSON_Delete(r);
    return bp;
}

/* ISO-8601 UTC (v7's startdate / enddate are Date strings: new Date(...) parses them). */
static void iso_time(const int64_t s, char *out, const size_t size) {
    const time_t t = (time_t) s;
    struct tm tm;
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    strftime(out, size, "%Y-%m-%dT%H:%M:%S.000Z", &tm);
}

/* io_dataLoaded's payload (DBClient.ts BattlePassPayload) plus KrunkNative's KR prices. */
static cJSON *bp_payload(const BattlePass *bp, const cJSON *row) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "id", bp->id);
    cJSON_AddNumberToObject(o, "season", bp->season);
    cJSON_AddStringToObject(o, "name", bp->name);
    cJSON_AddStringToObject(o, "descr", jstr(row, "descr"));
    cJSON_AddNumberToObject(o, "bpp_perlevel", bp->bpp_per_level);
    cJSON_AddNumberToObject(o, "itemcount", bp->item_count);
    cJSON_AddStringToObject(o, "tier0name", "Free");
    cJSON_AddNumberToObject(o, "tier0price", 0);
    cJSON_AddStringToObject(o, "tier1name", bp->tier1_name);
    cJSON_AddNumberToObject(o, "tier1price", bp->tier1_price_kr); /* KrunkNative: KR, not USD */
    cJSON_AddNumberToObject(o, "skipprice", bp->skip_price_kr);   /* KrunkNative: KR per level */
    char *items = malloc(BP_ITEMS_MAX * 48);
    if (items) {
        bp_format_items(bp, items, BP_ITEMS_MAX * 48);
        cJSON_AddStringToObject(o, "itemdata", items);
        free(items);
    }
    /* v10.0.2 windows[5].gen: per-level costs and the cards' KR / junk */
    cJSON_AddStringToObject(o, "leveldata", jstr(row, "leveldata"));
    cJSON_AddStringToObject(o, "krdata", jstr(row, "krdata"));
    cJSON_AddStringToObject(o, "jnkdata", jstr(row, "jnkdata"));
    char t[40];
    iso_time(bp->end_s, t, sizeof t);
    cJSON_AddStringToObject(o, "enddate", t);
    iso_time(bp->start_s, t, sizeof t);
    cJSON_AddStringToObject(o, "startdate", t);
    return o;
}

/* qs_battlepass_transactions of (account, pass): {id, bpp, tier, items_claimed, tier_at}; the
 * row is created on first use (getBattlePassProgress). NULL after a failure. */
static cJSON *progress_row(Dbx *x, const int64_t id, const int bp_id, const int lock) {
    const kn_sql *q = lock ? SQL(.any = "SELECT bpp AS bpp__i, tier AS tier__i, items_claimed, {epoch_ms:tier_at} AS tier_ms__f"
                                        " FROM battlepass_progress WHERE account_id = $1 AND bp_id = {int:$2} {for_update}")
                           : SQL(.any = "SELECT bpp AS bpp__i, tier AS tier__i, items_claimed, {epoch_ms:tier_at} AS tier_ms__f"
                                        " FROM battlepass_progress WHERE account_id = $1 AND bp_id = {int:$2}");
    cJSON *r = dbx_row(x, q, KN_DB_ARGS(I64S(id), I64S(bp_id)));
    if (!r && !dbx_failed(x)) {
        dbx_exec(x, SQL(.any = "{insert_ignore} INTO battlepass_progress (account_id, bp_id) VALUES ($1, {int:$2}) {ignore_conflicts}"),
                 KN_DB_ARGS(I64S(id), I64S(bp_id)));
        r = dbx_row(x, q, KN_DB_ARGS(I64S(id), I64S(bp_id)));
    }
    if (!r) return NULL;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "id", bp_id);
    cJSON_AddNumberToObject(o, "bpp", jnum(r, "bpp"));
    cJSON_AddNumberToObject(o, "tier", jnum(r, "tier"));
    cJSON_AddStringToObject(o, "items_claimed", jstr(r, "items_claimed"));
    cJSON_AddNumberToObject(o, "tier_at", jnum(r, "tier_ms") / 1000.0);
    cJSON_Delete(r);
    return o;
}

/* account.BP as the client gets it (bppd): no tier_at */
static cJSON *progress_public(const cJSON *row) {
    cJSON *o = cJSON_Duplicate(row, 1);
    if (o) cJSON_DeleteItemFromObjectCaseSensitive(o, "tier_at");
    return o;
}

static int account_exists(Dbx *x, const int64_t id) {
    return id > 0 && dbx_exists(x, SQL(.any = "SELECT 1 FROM players WHERE player_id = $1"), KN_DB_ARGS(I64S(id)));
}

/* ------------------------------------------------------------------ season badges */

typedef struct {
    Dbx *x;
    int64_t id;
} SkinCtx;

static int has_skin(void *ud, const int skin) {
    SkinCtx *c = ud;
    return dbx_exists(c->x, SQL(.any = "SELECT 1 FROM inventory WHERE account_id = $1 AND skin_id = {int:$2} AND count > 0"),
                      KN_DB_ARGS(I64S(c->id), I64S(skin)));
}

/* checkBadges for the season badges (and Nuclear, from stats): every claimable badge whose rule
 * holds and the account lacks is added; returns them (JSON array). */
cJSON *db_season_check_badges(Dbx *x, const int64_t id, const int season, const int64_t now_s, const cJSON *stats) {
    cJSON *added = cJSON_CreateArray();
    int owned[BADGE_COUNT], owned_count = 0;
    cJSON *rows = dbx_rows(x, SQL(.any = "SELECT badge AS b__i FROM player_badges WHERE account_id = $1 ORDER BY badge"), KN_DB_ARGS(I64S(id)));
    const cJSON *r;
    cJSON_ArrayForEach(r, rows) if (owned_count < BADGE_COUNT) owned[owned_count++] = (int) jnum(r, "b");
    cJSON_Delete(rows);
    SkinCtx skins = {x, id};
    SeasonBadgeAccount a = {.season = season, .now_s = now_s, .badges = owned, .badge_count = owned_count, .has_skin = has_skin, .ud = &skins};
    BattlePass *bp = dbx_failed(x) ? NULL : db_bp_load(x, season, NULL);
    if (bp) {
        cJSON *pr = progress_row(x, id, bp->id, 0);
        a.bp_tier = (int) jnum(pr, "tier");
        a.bpp = (int64_t) jnum(pr, "bpp");
        a.bp_tier_at_s = (int64_t) jnum(pr, "tier_at");
        a.bp_start_s = bp->start_s;
        cJSON_Delete(pr);
        free(bp);
    }
    /* two passes: a Badge Master rule reads the badges this check adds */
    for (int pass = 0; pass < 2 && !dbx_failed(x); pass++) {
        for (int i = 0; i < BADGE_COUNT && !dbx_failed(x); i++) {
            if (!g_badges[i].claimable || !badge_rule_met(i, stats, &a)) continue;
            int have = 0;
            for (int k = 0; k < a.badge_count; k++) have |= owned[k] == i;
            if (have) continue;
            dbx_exec(x, SQL(.any = "{insert_ignore} INTO player_badges (account_id, badge) VALUES ($1, {int:$2}) {ignore_conflicts}"),
                     KN_DB_ARGS(I64S(id), I64S(i)));
            if (owned_count < BADGE_COUNT) owned[owned_count++] = i;
            a.badge_count = owned_count;
            cJSON_AddItemToArray(added, cJSON_CreateNumber(i));
        }
    }
    return added;
}

static cJSON *player_stats(Dbx *x, const int64_t id) {
    char *text = dbx_text(x, SQL(.any = "SELECT player_stats FROM players WHERE player_id = $1"), KN_DB_ARGS(I64S(id)));
    cJSON *stats = text ? cJSON_Parse(text) : NULL;
    free(text);
    if (!cJSON_IsObject(stats)) {
        cJSON_Delete(stats);
        stats = cJSON_CreateObject();
    }
    return stats;
}

/* ------------------------------------------------------------------ the match (P_PROGRESS) */

static void set_progress(Dbx *x, const int64_t id, const int bp_id, const int64_t bpp) {
    dbx_exec(x, SQL(.any = "UPDATE battlepass_progress SET bpp = {int:$3}, updated_at = {now} WHERE account_id = $1 AND bp_id = {int:$2}"),
             KN_DB_ARGS(I64S(id), I64S(bp_id), I64S(bpp)));
}

/* tmpBPData.re: {id, sk, tier, col} */
static cJSON *reward_json(const BattlePassItem *it) {
    if (!it) return cJSON_CreateNull();
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "id", it->id);
    cJSON_AddNumberToObject(o, "sk", it->skin);
    cJSON_AddNumberToObject(o, "tier", it->tier);
    cJSON_AddNumberToObject(o, "col", it->level);
    if (it->kr > 0) cJSON_AddNumberToObject(o, "kr", it->kr);   /* v10: the end screen's krunkies icon */
    if (it->jnk > 0) cJSON_AddNumberToObject(o, "jnk", it->jnk);
    return o;
}

cJSON *db_season_match(Dbx *x, const int64_t id, const int season, const int64_t now_s, const cJSON *in, const int64_t challenge_bpp,
                       cJSON **progress_out) {
    if (progress_out) *progress_out = NULL;
    /* season_stats: the counted match's totals (the commit sends only counted matches here) */
    const cJSON *won = cJSON_GetObjectItemCaseSensitive(in, "won");
    const int64_t kills = (int64_t) jnum(in, "kills"), deaths = (int64_t) jnum(in, "deaths"), score = (int64_t) jnum(in, "score");
    const int64_t games = (int64_t) jnum(in, "games"), played = (int64_t) jnum(in, "timePlayed");
    const int64_t wins = cJSON_IsTrue(won) ? 1 : 0;
    if (kills || deaths || score || games || played || wins) {
        const char *args[8] = {I64S(id), I64S(season), I64S(kills), I64S(deaths), I64S(wins), I64S(games), I64S(score), I64S(played)};
        dbx_exec(x, SQL(.sqlite = "INSERT INTO season_stats (account_id, season, kills, deaths, wins, games, score, time_played)"
                                  " VALUES ($1, $2, $3, $4, $5, $6, $7, $8) ON CONFLICT (account_id, season) DO UPDATE SET"
                                  " kills = kills + excluded.kills, deaths = deaths + excluded.deaths, wins = wins + excluded.wins,"
                                  " games = games + excluded.games, score = score + excluded.score,"
                                  " time_played = time_played + excluded.time_played, updated_at = strftime('%Y-%m-%d %H:%M:%f','now')",
                        .mysql = "INSERT INTO season_stats (account_id, season, kills, deaths, wins, games, score, time_played)"
                                 " VALUES ($1, $2, $3, $4, $5, $6, $7, $8) ON DUPLICATE KEY UPDATE"
                                 " kills = kills + VALUES(kills), deaths = deaths + VALUES(deaths), wins = wins + VALUES(wins),"
                                 " games = games + VALUES(games), score = score + VALUES(score),"
                                 " time_played = time_played + VALUES(time_played), updated_at = NOW(3)",
                        .pg = "INSERT INTO season_stats (account_id, season, kills, deaths, wins, games, score, time_played)"
                              " VALUES ($1::int, $2::int, $3::bigint, $4::bigint, $5::bigint, $6::bigint, $7::bigint, $8::bigint)"
                              " ON CONFLICT (account_id, season) DO UPDATE SET kills = season_stats.kills + EXCLUDED.kills,"
                              " deaths = season_stats.deaths + EXCLUDED.deaths, wins = season_stats.wins + EXCLUDED.wins,"
                              " games = season_stats.games + EXCLUDED.games, score = season_stats.score + EXCLUDED.score,"
                              " time_played = season_stats.time_played + EXCLUDED.time_played, updated_at = now()"),
                 8, args);
    }

    /* endGame's tmpBPData (only for a finished public match: "bpp" in the payload) */
    const cJSON *req = cJSON_GetObjectItemCaseSensitive(in, "bpp");
    if (!cJSON_IsNumber(req) && challenge_bpp <= 0) return NULL;
    cJSON *bprow = NULL;
    BattlePass *bp = dbx_failed(x) ? NULL : db_bp_load(x, season, &bprow);
    if (!bp) return NULL;
    cJSON *row = progress_row(x, id, bp->id, 1);
    if (!row) {
        cJSON_Delete(bprow);
        free(bp);
        return NULL;
    }
    const int64_t before = (int64_t) jnum(row, "bpp");
    const int64_t requested = cJSON_IsNumber(req) ? (int64_t) req->valuedouble : 0;
    const int open = now_s >= bp->start_s && (bp->end_s <= 0 || now_s < bp->end_s);
    const BpAward award = open ? bp_validate_progress(bp, before, requested, now_s) : (BpAward) {0, 0};
    /* challenges.finalize: REW.bpp of the completed challenges goes on top (not capped) */
    const int64_t cbp = open && challenge_bpp > 0 ? challenge_bpp : 0;
    int64_t after = before + award.award + cbp;
    const int64_t max = bp_max_bpp(bp);
    if (after > max) after = max > before ? max : before;
    if (after != before) set_progress(x, id, bp->id, after);

    cJSON *t = cJSON_CreateObject(); /* tmpBPData */
    cJSON_AddNumberToObject(t, "bpp", (double) after);
    cJSON_AddNumberToObject(t, "cbp", (double) (after - before - award.award > 0 ? after - before - award.award : 0));
    cJSON_AddNumberToObject(t, "gn", (double) award.award);
    cJSON_AddNumberToObject(t, "mx", bp->bpp_per_level);
    cJSON_AddNumberToObject(t, "id", bp->id);
    cJSON_AddNumberToObject(t, "ml", bp->item_count);
    cJSON_AddNumberToObject(t, "pbpp", (double) before);
    cJSON_AddNumberToObject(t, "bbpp", 0); /* max(award - requested, 0): the award never exceeds the request */
    cJSON_AddNumberToObject(t, "unbpp", (double) (requested + cbp));
    cJSON_AddNumberToObject(t, "rbpp", (double) (award.remaining - cbp > 0 ? award.remaining - cbp : 0));
    cJSON_AddItemToObject(t, "re", reward_json(bp_next_reward(bp, after)));
    cJSON_AddStringToObject(t, "lv", jstr(bprow, "leveldata")); /* v10 endScreen: _52111({mx, ml, lv}, bpp) */
    cJSON_Delete(bprow);
    cJSON_DeleteItemFromObjectCaseSensitive(row, "bpp");
    cJSON_AddNumberToObject(row, "bpp", (double) after);
    if (progress_out) *progress_out = progress_public(row);
    cJSON_Delete(row);
    free(bp);
    return t;
}

/* ------------------------------------------------------------------ procs */

/* (season) -> {season, created} */
static kn_db_result *run_season_ensure(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const int64_t season = PI(0);
    if (season <= 0) {
        dbx_refuse(&x, "Invalid season");
        return dbx_finish(&x, NULL);
    }
    /* insert only: an existing row (and its history) stays as it is */
    char name[32];
    snprintf(name, sizeof name, "Season %lld", (long long) season);
    const int64_t created = dbx_exec(&x, SQL(.any = "{insert_ignore} INTO seasons (season, name) VALUES ({int:$1}, $2) {ignore_conflicts}"),
                                     KN_DB_ARGS(I64S(season), name));
    /* the rollover: seasons before this one that are still open end now */
    if (created > 0)
        dbx_exec(&x, SQL(.any = "UPDATE seasons SET ends_at = {now} WHERE season < {int:$1} AND ends_at IS NULL"), KN_DB_ARGS(I64S(season)));
    cJSON *out = cJSON_CreateObject();
    cJSON_AddNumberToObject(out, "season", (double) season);
    cJSON_AddBoolToObject(out, "created", created > 0);
    return dbx_json(&x, "season", out);
}
const kn_proc P_SEASON_ENSURE = {"season_ensure", NULL, run_season_ensure};

/* (id | 0, season) -> {bp | null, progress | null} */
static kn_db_result *run_bp_data(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const int64_t id = PI(0);
    cJSON *row = NULL;
    BattlePass *bp = db_bp_load(&x, (int) PI(1), &row);
    cJSON *out = cJSON_CreateObject();
    cJSON_AddItemToObject(out, "bp", bp ? bp_payload(bp, row) : cJSON_CreateNull());
    cJSON *pr = bp && account_exists(&x, id) ? progress_row(&x, id, bp->id, 0) : NULL;
    cJSON_AddItemToObject(out, "progress", pr ? progress_public(pr) : cJSON_CreateNull());
    cJSON_Delete(pr);
    cJSON_Delete(row);
    free(bp);
    return dbx_json(&x, "bp", out);
}
const kn_proc P_BP_DATA = {"bp_data", NULL, run_bp_data};

/* Claims `items` (ids; NULL = every claimable card, v10 claimAllItems) of the account's progress
 * row (locked): the skins and the v10 KR of each card, items_claimed. Returns the claimed cards
 * [{id, tier, col, sk, kr, jnk}] (empty when none) or NULL after a refusal / failure. */
static cJSON *claim_cards(Dbx *x, const int64_t id, const BattlePass *bp, cJSON *row, const int *items, const int count) {
    char claimed[4096];
    snprintf(claimed, sizeof claimed, "%s", jstr(row, "items_claimed"));
    const int64_t bpp = (int64_t) jnum(row, "bpp");
    const int tier = (int) jnum(row, "tier");
    cJSON *out = cJSON_CreateArray();
    const int all = items == NULL;
    const int n = all ? bp->count : count;
    for (int i = 0; i < n && !dbx_failed(x); i++) {
        const int item = all ? bp->items[i].id : items[i];
        const BpClaimResult r = bp_claim_check(bp, bpp, tier, claimed, item);
        if (r != BP_CLAIM_OK) {
            if (!all) dbx_refuse(x, bp_claim_error(r));
            continue;
        }
        const BattlePassItem *it = bp_item(bp, item);
        char next[4096], ref[32];
        bp_add_claimed(claimed, item, next, sizeof next);
        snprintf(claimed, sizeof claimed, "%s", next);
        snprintf(ref, sizeof ref, "bp:%d:%d", bp->id, it->id);
        if (it->skin > 0) db_add_skin(x, id, it->skin, 1, "battlepass", ref, NULL);
        if (it->kr > 0) db_add_funds(x, id, it->kr, "battlepass", ref, NULL);
        if (it->jnk > 0) junk_credit(x, id, junk_hundredths(it->jnk), "battlepass", NULL); /* v10 jnkdata card */
        cJSON *c = cJSON_CreateObject();
        cJSON_AddNumberToObject(c, "id", it->id);
        cJSON_AddNumberToObject(c, "tier", it->tier);
        cJSON_AddNumberToObject(c, "col", it->level);
        cJSON_AddNumberToObject(c, "sk", it->skin);
        cJSON_AddNumberToObject(c, "kr", it->kr);
        cJSON_AddNumberToObject(c, "jnk", it->jnk);
        cJSON_AddItemToArray(out, c);
    }
    if (!dbx_failed(x) && cJSON_GetArraySize(out) > 0) {
        dbx_exec(x, SQL(.any = "UPDATE battlepass_progress SET items_claimed = $3, updated_at = {now} WHERE account_id = $1 AND bp_id = {int:$2}"),
                 KN_DB_ARGS(I64S(id), I64S(bp->id), claimed));
        cJSON_DeleteItemFromObjectCaseSensitive(row, "items_claimed");
        cJSON_AddStringToObject(row, "items_claimed", claimed);
    }
    if (dbx_failed(x)) {
        cJSON_Delete(out);
        return NULL;
    }
    return out;
}

/* (id, item id, season, all) -> {progress, sk, id, tier, col, items, funds} (all = 1: every
 * claimable card, v10 "bpclmAll"; the item id is ignored):
 * v7 claimItemRes(.., sk, .., {id, tier, col}) and v10's (.., funds, .., .., junk, [items]) */
static kn_db_result *run_bp_claim(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const int64_t id = PI(0), item = PI(1);
    if (!account_exists(&x, id)) {
        if (!dbx_failed(&x)) dbx_refuse(&x, "Account not found.");
        return dbx_finish(&x, NULL);
    }
    BattlePass *bp = db_bp_load(&x, (int) PI(2), NULL);
    if (!bp) {
        if (!dbx_failed(&x)) dbx_refuse(&x, "No Battle Pass Found");
        return dbx_finish(&x, NULL);
    }
    cJSON *row = progress_row(&x, id, bp->id, 1);
    cJSON *out = NULL;
    const int one = (int) item, all = PI(3) == 1;
    cJSON *cards = row ? claim_cards(&x, id, bp, row, all ? NULL : &one, 1) : NULL;
    if (cards && all && cJSON_GetArraySize(cards) == 0) dbx_refuse(&x, "No items to claim");
    if (cards && !dbx_failed(&x)) {
        const cJSON *first = cJSON_GetArrayItem(cards, 0);
        out = cJSON_CreateObject();
        cJSON_AddItemToObject(out, "progress", progress_public(row));
        cJSON_AddNumberToObject(out, "sk", jnum(first, "sk"));
        cJSON_AddNumberToObject(out, "id", jnum(first, "id"));
        cJSON_AddNumberToObject(out, "tier", jnum(first, "tier"));
        cJSON_AddNumberToObject(out, "col", jnum(first, "col"));
        cJSON_AddItemToObject(out, "items", cards);
        cards = NULL;
        cJSON_AddNumberToObject(out, "funds",
                                (double) dbx_int(&x, SQL(.any = "SELECT player_funds FROM players WHERE player_id = $1"), KN_DB_ARGS(I64S(id)), 0));
    }
    cJSON_Delete(cards);
    cJSON_Delete(row);
    free(bp);
    if (dbx_failed(&x)) {
        cJSON_Delete(out);
        return dbx_finish(&x, NULL);
    }
    return dbx_json(&x, "claim", out);
}
const kn_proc P_BP_CLAIM = {"bp_claim", NULL, run_bp_claim};

/* (id, tier, season, now_s, gift name | NULL) -> {progress, tier, funds, price, newBadges,
 * gift {id, name, progress, newBadges} | absent}. A gift (v10 selectBPGift) gives `name` the
 * tier; the payer pays and keeps their own pass. */
static kn_db_result *run_bp_buy(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const int64_t id = PI(0), tier = PI(1), season = PI(2), now_s = PI(3);
    const char *gift = P(4) && *P(4) ? P(4) : NULL;
    if (!account_exists(&x, id)) {
        if (!dbx_failed(&x)) dbx_refuse(&x, "Account not found.");
        return dbx_finish(&x, NULL);
    }
    int64_t to = id;
    char gift_name[64] = "";
    if (gift) {
        to = db_account_id(&x, gift);
        char *name = to > 0 ? dbx_text(&x, SQL(.any = "SELECT username FROM accounts WHERE id = $1"), KN_DB_ARGS(I64S(to))) : NULL;
        snprintf(gift_name, sizeof gift_name, "%s", name ? name : "");
        free(name);
        if (!dbx_failed(&x) && (to <= 0 || !account_exists(&x, to))) dbx_refuse(&x, "Player not found");
        else if (!dbx_failed(&x) && to == id) dbx_refuse(&x, "You can't gift yourself");
        if (dbx_failed(&x)) return dbx_finish(&x, NULL);
    }
    BattlePass *bp = tier == 1 ? db_bp_load(&x, (int) season, NULL) : NULL;
    if (!bp) {
        if (!dbx_failed(&x)) dbx_refuse(&x, tier == 1 ? "No Battle Pass Found" : "Invalid Tier");
        return dbx_finish(&x, NULL);
    }
    cJSON *row = progress_row(&x, to, bp->id, 1);
    cJSON *out = NULL;
    int64_t funds = 0;
    char ref[48];
    if (gift) snprintf(ref, sizeof ref, "bp:%d:tier%d:gift:%lld", bp->id, (int) tier, (long long) to);
    else snprintf(ref, sizeof ref, "bp:%d:tier%d", bp->id, (int) tier);
    if (row && jnum(row, "tier") >= (double) tier) dbx_refuse(&x, gift ? "This player already owns this Battle Pass" : "You already own this Battle Pass");
    else if (row && bp->end_s > 0 && now_s >= bp->end_s) dbx_refuse(&x, "This Battle Pass has ended");
    else if (row && (gift ? db_add_funds_ex(&x, id, -(int64_t) bp->tier1_price_kr, "battlepass-gift", ref, to, 0, &funds)
                          : db_add_funds(&x, id, -(int64_t) bp->tier1_price_kr, "battlepass", ref, &funds)) == 0) {
        dbx_exec(&x, SQL(.any = "UPDATE battlepass_progress SET tier = {int:$3}, tier_at = {from_ms:$4}, updated_at = {now}"
                                " WHERE account_id = $1 AND bp_id = {int:$2}"),
                 KN_DB_ARGS(I64S(to), I64S(bp->id), I64S(tier), I64S(now_s * 1000)));
        cJSON_DeleteItemFromObjectCaseSensitive(row, "tier");
        cJSON_AddNumberToObject(row, "tier", (double) tier);
        cJSON *stats = player_stats(&x, to);
        cJSON *badges = db_season_check_badges(&x, to, (int) season, now_s, stats);
        cJSON_Delete(stats);
        out = cJSON_CreateObject();
        if (gift) {
            /* "We will let them know you have gifted it to them" (selectBPGift) */
            dbx_exec(&x, SQL(.any = "INSERT INTO mail (account_id, sender, subject, body, kr)"
                                    " SELECT $1, a.username, 'Battle Pass Gift', $3, 0 FROM accounts a WHERE a.id = $2"),
                     KN_DB_ARGS(I64S(to), I64S(id), bp->name));
            cJSON *g = cJSON_AddObjectToObject(out, "gift");
            cJSON_AddNumberToObject(g, "id", (double) to);
            cJSON_AddStringToObject(g, "name", gift_name);
            cJSON_AddItemToObject(g, "progress", progress_public(row));
            cJSON_AddItemToObject(g, "newBadges", badges);
            cJSON *mine = progress_row(&x, id, bp->id, 0);
            cJSON_AddItemToObject(out, "progress", mine ? progress_public(mine) : cJSON_CreateNull());
            cJSON_Delete(mine);
            cJSON_AddItemToObject(out, "newBadges", cJSON_CreateArray());
        } else {
            cJSON_AddItemToObject(out, "progress", progress_public(row));
            cJSON_AddItemToObject(out, "newBadges", badges);
        }
        cJSON_AddNumberToObject(out, "tier", (double) tier);
        cJSON_AddNumberToObject(out, "funds", (double) funds);
        cJSON_AddNumberToObject(out, "price", bp->tier1_price_kr);
    }
    cJSON_Delete(row);
    free(bp);
    if (dbx_failed(&x)) {
        cJSON_Delete(out);
        return dbx_finish(&x, NULL);
    }
    return dbx_json(&x, "buy", out);
}
const kn_proc P_BP_BUY = {"bp_buy", NULL, run_bp_buy};

/* (id, levels, season, now_s) -> {progress, levels, funds, price, newBadges} */
static kn_db_result *run_bp_skip(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const int64_t id = PI(0), levels = PI(1), season = PI(2), now_s = PI(3);
    if (!account_exists(&x, id)) {
        if (!dbx_failed(&x)) dbx_refuse(&x, "Account not found.");
        return dbx_finish(&x, NULL);
    }
    BattlePass *bp = db_bp_load(&x, (int) season, NULL);
    if (!bp) {
        if (!dbx_failed(&x)) dbx_refuse(&x, "No Battle Pass Found");
        return dbx_finish(&x, NULL);
    }
    cJSON *row = progress_row(&x, id, bp->id, 1);
    cJSON *out = NULL;
    if (row) {
        const int64_t bpp = (int64_t) jnum(row, "bpp");
        int complete = 0;
        bp_level(bp, bpp, NULL, &complete);
        const int64_t max_levels = bp_skip_max(bp, bpp); /* increaseSkip: 1 .. max(1, itemcount - level) */
        int64_t funds = 0;
        char ref[32];
        snprintf(ref, sizeof ref, "bp:%d:skip%lld", bp->id, (long long) levels);
        if (complete || bpp >= bp_max_bpp(bp)) dbx_refuse(&x, "Battle Pass already complete");
        else if (levels < 1 || levels > max_levels) dbx_refuse(&x, "Invalid amount of levels");
        else if (bp->end_s > 0 && now_s >= bp->end_s) dbx_refuse(&x, "This Battle Pass has ended");
        else if (db_add_funds(&x, id, -levels * bp->skip_price_kr, "battlepass-skip", ref, &funds) == 0) {
            const int64_t after = bp_skip_bpp(bp, bpp, (int) levels);
            set_progress(&x, id, bp->id, after);
            cJSON_DeleteItemFromObjectCaseSensitive(row, "bpp");
            cJSON_AddNumberToObject(row, "bpp", (double) after);
            out = cJSON_CreateObject();
            cJSON_AddItemToObject(out, "progress", progress_public(row));
            cJSON_AddNumberToObject(out, "levels", (double) levels);
            cJSON_AddNumberToObject(out, "funds", (double) funds);
            cJSON_AddNumberToObject(out, "price", (double) (levels * bp->skip_price_kr));
            cJSON *stats = player_stats(&x, id);
            cJSON_AddItemToObject(out, "newBadges", db_season_check_badges(&x, id, (int) season, now_s, stats));
            cJSON_Delete(stats);
        }
    }
    cJSON_Delete(row);
    free(bp);
    if (dbx_failed(&x)) {
        cJSON_Delete(out);
        return dbx_finish(&x, NULL);
    }
    return dbx_json(&x, "skip", out);
}
const kn_proc P_BP_SKIP = {"bp_skip", NULL, run_bp_skip};

/* (name | id, season) -> the season block of a profile, null for an unknown player */
static kn_db_result *run_season_stats(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const char *who = P(0) ? P(0) : "";
    const int64_t season = PI(1);
    int64_t id = strspn(who, "0123456789") == strlen(who) && who[0] ? pint(who) : db_account_id(&x, who);
    if (id <= 0 || !account_exists(&x, id)) return dbx_json(&x, "season", NULL);
    cJSON *r = dbx_row(&x, SQL(.any = "SELECT kills AS kills__i, deaths AS deaths__i, wins AS wins__i, games AS games__i,"
                                      " score AS score__i, time_played AS time_played__i FROM season_stats"
                                      " WHERE account_id = $1 AND season = {int:$2}"),
                       KN_DB_ARGS(I64S(id), I64S(season)));
    cJSON *out = cJSON_CreateObject();
    cJSON_AddNumberToObject(out, "season", (double) season);
    char *name = dbx_text(&x, SQL(.any = "SELECT username FROM accounts WHERE id = $1"), KN_DB_ARGS(I64S(id)));
    cJSON_AddStringToObject(out, "name", name ? name : "");
    free(name);
    static const char *const keys[][2] = {{"kills", "kills"}, {"deaths", "deaths"}, {"wins", "wins"},
                                          {"games", "games"}, {"score", "score"}, {"timePlayed", "time_played"}};
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) cJSON_AddNumberToObject(out, keys[i][0], jnum(r, keys[i][1]));
    cJSON_Delete(r);
    BattlePass *bp = dbx_failed(&x) ? NULL : db_bp_load(&x, (int) season, NULL);
    if (bp) {
        cJSON *pr = dbx_row(&x, SQL(.any = "SELECT bpp AS bpp__i, tier AS tier__i FROM battlepass_progress WHERE account_id = $1 AND bp_id = {int:$2}"),
                            KN_DB_ARGS(I64S(id), I64S(bp->id)));
        const int64_t bpp = (int64_t) jnum(pr, "bpp");
        cJSON_AddNumberToObject(out, "bpp", (double) bpp);
        cJSON_AddNumberToObject(out, "tier", jnum(pr, "tier"));
        cJSON_AddNumberToObject(out, "level", bp_level(bp, bpp, NULL, NULL));
        cJSON_AddNumberToObject(out, "levels", bp->item_count);
        cJSON_Delete(pr);
        free(bp);
    }
    return dbx_json(&x, "season", out);
}
const kn_proc P_SEASON_STATS = {"season_stats", NULL, run_season_stats};
