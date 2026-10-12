/*
 * services/brain/db_progress.c - player-viewer WP-E part 2c: Krunker v7.1.4 badges, data-driven
 * challenges and per-account medal counts (doc/v7/player-viewer.md 1.4.2-1.4.4, doc/backend.md
 * 7.4 / 7.6) on the tables of migration 0107_progression. C on every driver (no .pg text): the
 * challenge rules are shared/src/progression.c, the same code the tests drive.
 *
 *   P_CHALLENGES  (id, now_s)            -> [activeChallenges]   the "chg" window list
 *   P_PROGRESS    (payload json, now_s)  -> {results, completed, reward, newBadges, funds}
 *                 v7 game.js checkChallenges (challenges.finalize + procs 175 / 176) and
 *                 checkBadges (proc 181) after a stats commit, plus the medal counts
 *   P_BADGE_SELECT (id, badge)           -> {bdg}   v7 "bdg": stats.bdg (-1 clears)
 *   P_BADGE_CLAIM  (id, badge)           -> {badge} v7 "cbdg": a claimable badge whose rule holds
 *   P_MEDALS       (id)                  -> {name: count}
 */
#include "db_procs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "badges.h"
#include "knsvc/json.h"
#include "progression.h"

#define P(i) (n > (i) ? p[i] : NULL)
#define PI(i) pint(P(i))

#define CHALLENGES_DAILY 3  /* daily challenges an account gets per UTC day */
#define CHALLENGES_WEEKLY 3 /* weekly challenges per week (weeks start on Monday 00:00 UTC) */

static int64_t pint(const char *s) { return s ? strtoll(s, NULL, 10) : 0; }

/* The period a challenge type counts in: the UTC day / week number for daily / weekly ones (and
 * when it ends, unix s), 0 for the others. */
static int64_t period_of(const char *type, const int64_t now_s, int64_t *ends_s) {
    const int64_t day = now_s / 86400;
    if (type && !strcmp(type, "daily")) {
        if (ends_s) *ends_s = (day + 1) * 86400;
        return day;
    }
    if (type && !strcmp(type, "weekly")) {
        const int64_t week = (day + 3) / 7; /* 1970-01-01 was a Thursday */
        if (ends_s) *ends_s = (week * 7 - 3 + 7) * 86400;
        return week;
    }
    if (ends_s) *ends_s = 0;
    return 0;
}

/* The rotation order of a daily / weekly row for an account in a period. */
static uint32_t rotation_key(const int64_t challenge, const int64_t account, const int64_t period) {
    uint32_t h = 2166136261u;
    const int64_t parts[3] = {challenge, account, period};
    for (int i = 0; i < 3; i++)
        for (int b = 0; b < 8; b++) {
            h ^= (uint32_t) ((parts[i] >> (b * 8)) & 0xff);
            h *= 16777619u;
        }
    return h;
}

static double jnum(const cJSON *o, const char *k) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? v->valuedouble : cJSON_IsString(v) && v->valuestring ? strtod(v->valuestring, NULL) : 0.0;
}

static const char *jstr(const cJSON *o, const char *k) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) ? v->valuestring : "";
}

/* Keeps the CHALLENGES_DAILY / _WEEKLY rows of `type` with the lowest rotation keys. */
static void rotate(cJSON *list, const char *type, const int keep, const int64_t account, const int64_t now_s) {
    for (;;) {
        int count = 0;
        cJSON *worst = NULL;
        uint32_t worst_key = 0;
        cJSON *c;
        cJSON_ArrayForEach(c, list) {
            if (strcmp(jstr(c, "T"), type) != 0) continue;
            count++;
            const uint32_t k = rotation_key((int64_t) jnum(c, "CI"), account, period_of(type, now_s, NULL));
            if (!worst || k > worst_key) {
                worst = c;
                worst_key = k;
            }
        }
        if (count <= keep || !worst) return;
        cJSON_Delete(cJSON_DetachItemViaPointer(list, worst));
    }
}

/* The account's activeChallenges: {CI, T, DAT, REW, IC, N, PV, PT, TR (ms left, daily /
 * weekly), PER (period, KrunkNative)}. NULL after a database failure. */
static cJSON *load_active(Dbx *x, const int64_t id, const int64_t now_s) {
    cJSON *defs = dbx_rows(x, SQL(.any = "SELECT challenge_id AS ci__i, ctype, dat, rew, icon AS ic__i, name FROM challenges"
                                         " WHERE active <> 0 ORDER BY sort, challenge_id"), 0, NULL);
    cJSON *prog = dbx_rows(x, SQL(.any = "SELECT challenge_id AS ci__i, period AS per__i, pv AS pv__f, pt AS pt__i"
                                         " FROM player_challenges WHERE account_id = $1"), KN_DB_ARGS(I64S(id)));
    if (dbx_failed(x)) {
        cJSON_Delete(defs);
        cJSON_Delete(prog);
        return NULL;
    }
    cJSON *list = cJSON_CreateArray();
    const cJSON *d;
    cJSON_ArrayForEach(d, defs) {
        const char *type = jstr(d, "ctype");
        const int64_t ci = (int64_t) jnum(d, "ci");
        int64_t ends = 0;
        const int64_t period = period_of(type, now_s, &ends);
        cJSON *dat = cJSON_Parse(jstr(d, "dat"));
        cJSON *rew = cJSON_Parse(jstr(d, "rew"));
        if (!cJSON_IsObject(dat)) {
            cJSON_Delete(dat);
            cJSON_Delete(rew);
            continue;
        }
        double pv = 0.0;
        int pt = 0;
        const cJSON *r;
        cJSON_ArrayForEach(r, prog) {
            if ((int64_t) jnum(r, "ci") == ci && (int64_t) jnum(r, "per") == period) {
                pv = jnum(r, "pv");
                pt = (int) jnum(r, "pt");
            }
        }
        cJSON *c = cJSON_CreateObject();
        cJSON_AddNumberToObject(c, "CI", (double) ci);
        cJSON_AddStringToObject(c, "T", type);
        cJSON_AddItemToObject(c, "DAT", dat);
        cJSON_AddItemToObject(c, "REW", cJSON_IsObject(rew) ? rew : cJSON_CreateObject());
        if (!cJSON_IsObject(rew)) cJSON_Delete(rew);
        cJSON_AddNumberToObject(c, "IC", jnum(d, "ic"));
        cJSON_AddStringToObject(c, "N", jstr(d, "name"));
        cJSON_AddNumberToObject(c, "PV", pv);
        cJSON_AddNumberToObject(c, "PT", pt ? 1 : 0);
        if (ends) cJSON_AddNumberToObject(c, "TR", (double) (ends - now_s) * 1000.0);
        cJSON_AddNumberToObject(c, "PER", (double) period);
        cJSON_AddItemToArray(list, c);
    }
    cJSON_Delete(defs);
    cJSON_Delete(prog);
    rotate(list, "daily", CHALLENGES_DAILY, id, now_s);
    rotate(list, "weekly", CHALLENGES_WEEKLY, id, now_s);
    return list;
}

/* (account id, now unix s) -> activeChallenges */
static kn_db_result *run_challenges(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const int64_t id = PI(0);
    if (id <= 0 || !dbx_exists(&x, SQL(.any = "SELECT 1 FROM players WHERE player_id = $1"), KN_DB_ARGS(I64S(id)))) {
        if (!dbx_failed(&x)) dbx_refuse(&x, "Account not found.");
        return dbx_finish(&x, NULL);
    }
    return dbx_json(&x, "challenges", load_active(&x, id, PI(1)));
}
const kn_proc P_CHALLENGES = {"challenges", NULL, run_challenges};

static cJSON *stats_object(Dbx *x, const int64_t id) {
    char *text = dbx_text(x, SQL(.any = "SELECT player_stats FROM players WHERE player_id = $1"), KN_DB_ARGS(I64S(id)));
    cJSON *stats = text ? cJSON_Parse(text) : NULL;
    free(text);
    if (!cJSON_IsObject(stats)) {
        cJSON_Delete(stats);
        stats = cJSON_CreateObject();
    }
    return stats;
}

static int owns_badge(Dbx *x, const int64_t id, const int badge) {
    return dbx_exists(x, SQL(.any = "SELECT 1 FROM player_badges WHERE account_id = $1 AND badge = $2"), KN_DB_ARGS(I64S(id), I64S(badge)));
}

static void add_badge(Dbx *x, const int64_t id, const int badge) {
    dbx_exec(x, SQL(.any = "{insert_ignore} INTO player_badges (account_id, badge) VALUES ($1, $2) {ignore_conflicts}"),
             KN_DB_ARGS(I64S(id), I64S(badge)));
}

/* checkBadges: every claimable badge whose rule holds and the account lacks; returns them. */
static cJSON *check_badges(Dbx *x, const int64_t id) {
    cJSON *added = cJSON_CreateArray();
    cJSON *stats = stats_object(x, id);
    for (int i = 0; i < BADGE_COUNT && !dbx_failed(x); i++) {
        if (!g_badges[i].claimable || !badge_requirement_met(i, stats) || owns_badge(x, id, i)) continue;
        add_badge(x, id, i);
        cJSON_AddItemToArray(added, cJSON_CreateNumber(i));
    }
    cJSON_Delete(stats);
    return added;
}

/* ({id, final, prog: [[key, value, weapon, streak]...], medals: {name: count}, mapIndex, mapId,
 *   mode, classIndex, ranked, match}, now unix s) -> {results, completed, reward {kr, skins},
 *   newBadges, funds}. Runs after the match's kn_commit_stats (the badge rules read its stats). */
static kn_db_result *run_progress(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    cJSON *in = P(0) ? cJSON_Parse(P(0)) : NULL;
    const int64_t id = (int64_t) jnum(in, "id"), now_s = PI(1);
    if (!in || id <= 0 || !dbx_exists(&x, SQL(.any = "SELECT 1 FROM players WHERE player_id = $1"), KN_DB_ARGS(I64S(id)))) {
        cJSON_Delete(in);
        if (!dbx_failed(&x)) dbx_refuse(&x, "Account not found.");
        return dbx_finish(&x, NULL);
    }
    const char *ref = jstr(in, "match");
    cJSON *out = cJSON_CreateObject();
    cJSON *results = cJSON_AddArrayToObject(out, "results");
    cJSON *completed = cJSON_AddArrayToObject(out, "completed");
    cJSON *reward = cJSON_AddObjectToObject(out, "reward");

    /* challenges.finalize(player, ..., quit) */
    cJSON *active = load_active(&x, id, now_s);
    const cJSON *map_id = cJSON_GetObjectItemCaseSensitive(in, "mapId");
    const ChallengeMatch match = {
        .map_index = cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(in, "mapIndex")) ? (int) jnum(in, "mapIndex") : -1,
        .map_id = cJSON_IsNumber(map_id) ? map_id->valueint : -1,
        .mode = (int) jnum(in, "mode"),
        .class_index = (int) jnum(in, "classIndex"),
        .ranked = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(in, "ranked")),
        .quit = cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(in, "final")),
    };
    challenges_finalize(active, cJSON_GetObjectItemCaseSensitive(in, "prog"), &match, results, completed, reward);
    for (int i = 0; i + 2 < cJSON_GetArraySize(results) && !dbx_failed(&x); i += 3) {
        const int64_t ci = (int64_t) cJSON_GetNumberValue(cJSON_GetArrayItem(results, i + 1));
        const cJSON *ch = NULL, *it;
        cJSON_ArrayForEach(it, active) if ((int64_t) jnum(it, "CI") == ci) ch = it;
        if (!ch) continue;
        char pv[32];
        snprintf(pv, sizeof pv, "%lld", (long long) jnum(ch, "PV"));
        const char *args[5] = {I64S(id), I64S(ci), I64S((int64_t) jnum(ch, "PER")), pv, jnum(ch, "PT") == 1.0 ? "1" : "0"};
        dbx_exec(&x, SQL(.sqlite = "INSERT INTO player_challenges (account_id, challenge_id, period, pv, pt) VALUES ($1, $2, $3, $4, $5)"
                                   " ON CONFLICT (account_id, challenge_id, period) DO UPDATE SET pv = excluded.pv, pt = excluded.pt,"
                                   " updated_at = strftime('%Y-%m-%d %H:%M:%f','now')",
                         .mysql = "INSERT INTO player_challenges (account_id, challenge_id, period, pv, pt) VALUES ($1, $2, $3, $4, $5)"
                                  " ON DUPLICATE KEY UPDATE pv = VALUES(pv), pt = VALUES(pt), updated_at = NOW(3)",
                         .pg = "INSERT INTO player_challenges (account_id, challenge_id, period, pv, pt)"
                               " VALUES ($1::int, $2::int, $3::int, $4::bigint, $5::smallint)"
                               " ON CONFLICT (account_id, challenge_id, period) DO UPDATE SET pv = EXCLUDED.pv, pt = EXCLUDED.pt,"
                               " updated_at = now()"),
                 5, args);
    }
    /* challenge rewards (proc 176 "chall"): KR and skins; junk / Battle Pass points have no
     * KrunkNative economy (doc/v7/decisions.md, store economy out of scope) */
    const int64_t kr = (int64_t) jnum(reward, "kr");
    if (kr > 0) db_add_funds(&x, id, kr, "challenge", ref, NULL);
    const cJSON *sk;
    cJSON_ArrayForEach(sk, cJSON_GetObjectItemCaseSensitive(reward, "skins")) {
        if (cJSON_IsNumber(sk) && sk->valueint >= 0) db_add_skin(&x, id, sk->valueint, 1, "challenge", ref, NULL);
    }
    cJSON_Delete(active);

    /* per-account medal counts (player.medals of the match) */
    const cJSON *m;
    cJSON_ArrayForEach(m, cJSON_GetObjectItemCaseSensitive(in, "medals")) {
        if (!m->string || !cJSON_IsNumber(m) || m->valuedouble <= 0 || progression_medal_index(m->string) < 0) continue;
        dbx_exec(&x, SQL(.sqlite = "INSERT INTO player_medals (account_id, medal, count) VALUES ($1, $2, $3)"
                                   " ON CONFLICT (account_id, medal) DO UPDATE SET count = count + excluded.count",
                         .mysql = "INSERT INTO player_medals (account_id, medal, count) VALUES ($1, $2, $3)"
                                  " ON DUPLICATE KEY UPDATE count = count + VALUES(count)",
                         .pg = "INSERT INTO player_medals (account_id, medal, count) VALUES ($1::int, $2, $3::bigint)"
                               " ON CONFLICT (account_id, medal) DO UPDATE SET count = player_medals.count + EXCLUDED.count"),
                 KN_DB_ARGS(I64S(id), m->string, I64S((int64_t) m->valuedouble)));
    }

    /* checkBadges (public matches, not hack-flagged: the game server only commits those) */
    cJSON_AddItemToObject(out, "newBadges", check_badges(&x, id));
    cJSON_AddNumberToObject(out, "funds",
                            (double) dbx_int(&x, SQL(.any = "SELECT player_funds FROM players WHERE player_id = $1"), KN_DB_ARGS(I64S(id)), 0));
    cJSON_Delete(in);
    return dbx_json(&x, "progress", out);
}
const kn_proc P_PROGRESS = {"progress", NULL, run_progress};

/* (account id, badge) -> {bdg}: stats.bdg = badge when owned (-1 clears) */
static kn_db_result *run_badge_select(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const int64_t id = PI(0);
    int64_t badge = PI(1);
    if (badge < 0) badge = -1;
    if (badge >= 0 && (badge >= BADGE_COUNT || !owns_badge(&x, id, (int) badge))) {
        if (!dbx_failed(&x)) dbx_refuse(&x, "You dont own this badge");
        return dbx_finish(&x, NULL);
    }
    char *text = dbx_text(&x, SQL(.any = "SELECT player_stats FROM players WHERE player_id = $1 {for_update}"), KN_DB_ARGS(I64S(id)));
    cJSON *stats = text ? cJSON_Parse(text) : NULL;
    free(text);
    if (dbx_failed(&x)) {
        cJSON_Delete(stats);
        return dbx_finish(&x, NULL);
    }
    if (!cJSON_IsObject(stats)) {
        cJSON_Delete(stats);
        stats = cJSON_CreateObject();
    }
    cJSON_DeleteItemFromObjectCaseSensitive(stats, "bdg");
    cJSON_AddNumberToObject(stats, "bdg", (double) badge);
    char *merged = cJSON_PrintUnformatted(stats);
    cJSON_Delete(stats);
    const int64_t done = dbx_exec(&x, SQL(.any = "UPDATE players SET player_stats = $2, updated_at = {now} WHERE player_id = $1"),
                                  KN_DB_ARGS(I64S(id), merged));
    free(merged);
    if (done == 0) dbx_refuse(&x, "Account not found.");
    cJSON *out = cJSON_CreateObject();
    cJSON_AddNumberToObject(out, "bdg", (double) badge);
    return dbx_json(&x, "badge", out);
}
const kn_proc P_BADGE_SELECT = {"badge_select", NULL, run_badge_select};

/* (account id, badge) -> {badge}: claimBadge of a claimable badge whose rule holds */
static kn_db_result *run_badge_claim(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const int64_t id = PI(0), badge = PI(1);
    if (badge < 0 || badge >= BADGE_COUNT || !g_badges[badge].claimable) {
        dbx_refuse(&x, "Badge not claimable");
        return dbx_finish(&x, NULL);
    }
    cJSON *stats = stats_object(&x, id);
    const int met = badge_requirement_met((int) badge, stats);
    cJSON_Delete(stats);
    if (!dbx_failed(&x) && !met) dbx_refuse(&x, "Requirements not met");
    else if (!dbx_failed(&x) && owns_badge(&x, id, (int) badge)) dbx_refuse(&x, "Badge already claimed");
    else add_badge(&x, id, (int) badge);
    if (dbx_failed(&x)) return dbx_finish(&x, NULL);
    cJSON *out = cJSON_CreateObject();
    cJSON_AddNumberToObject(out, "badge", (double) badge);
    return dbx_json(&x, "badge", out);
}
const kn_proc P_BADGE_CLAIM = {"badge_claim", NULL, run_badge_claim};

/* (account id) -> {medal name: count} */
static kn_db_result *run_medals(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    cJSON *rows = dbx_rows(&x, SQL(.any = "SELECT medal, count AS c__i FROM player_medals WHERE account_id = $1 ORDER BY medal"),
                           KN_DB_ARGS(I64S(PI(0))));
    cJSON *out = cJSON_CreateObject();
    const cJSON *r;
    cJSON_ArrayForEach(r, rows) cJSON_AddNumberToObject(out, jstr(r, "medal"), jnum(r, "c"));
    cJSON_Delete(rows);
    return dbx_json(&x, "medals", out);
}
const kn_proc P_MEDALS = {"medals", NULL, run_medals};
