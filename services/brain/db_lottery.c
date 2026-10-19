/*
 * services/brain/db_lottery.c - Krunker v10.0.2's lottery (the v10 raffle window's API: GET
 * /lottery/<id>, POST /lottery/<id>/participate; migration 0348_lottery; routes in lottery.c).
 *
 * A lottery is a sequence of draw windows. Window k ends at k * frequency_h hours + offset_s (UTC),
 * so the open window is the first one that ends after now. A bundle of a cost (cost_quantity of
 * it: skins, KR, Junk hundredths or RP) gives num_entries entries in the open window; the window's
 * draw row is created by its first entry. Once a window's end has passed, the settlement claims the
 * draw (drawn_s, once) and each pool of the lottery pays its winners: one prize per winner, the
 * winning entry drawn at random (so an account with more entries can win more, and can win twice),
 * the prize skin drawn at random from the pool. Prizes are skin grants (db_add_skin, source
 * 'lottery'); the draw, its prizes and the grants are one transaction.
 *
 * Payments: KR through db_add_funds, Junk through junk_spend (reason 'lottery', doc/v10/junk.md),
 * RP through the ranked_points ledger, skins through db_add_skin (-n). A refused payment refuses the
 * entry and writes nothing.
 *
 * Times are epoch seconds in the tables and ISO UTC text in the JSON (v10's "2026-05-25T01:00:00Z").
 */
#include "db_procs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "junk.h"
#include "knsvc/crypto.h"
#include "knsvc/json.h"

#define P(i) (n > (i) ? p[i] : NULL)
#define PI(i) pint(P(i))
#define LOTTERY_MAX_BUNDLES 1000   /* a request's bundles (v10's quantity stepper stops at the balance) */
#define LOTTERY_PREVIOUS 10        /* previous_draws in the view (v10 shows the last ten) */

static int64_t pint(const char *s) { return s ? strtoll(s, NULL, 10) : 0; }

static int64_t floor_div(const int64_t a, const int64_t b) {
    int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) q--;
    return q;
}

/* the open window's end: the first k * freq + off after now */
static int64_t window_end(const int64_t now, const int64_t freq_s, const int64_t off_s) {
    return (floor_div(now - off_s, freq_s) + 1) * freq_s + off_s;
}

static void iso_at(const int64_t s, char out[24]) {
    const time_t t = (time_t) s;
    struct tm tm;
#ifdef _WIN32
    if (gmtime_s(&tm, &t) != 0) memset(&tm, 0, sizeof tm);
#else
    if (!gmtime_r(&t, &tm)) memset(&tm, 0, sizeof tm);
#endif
    strftime(out, 24, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

/* an integer field of a dbx_rows / dbx_row object (the "x__i" aliases read as numbers) */
static int64_t jint(const cJSON *o, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(v) ? (int64_t) v->valuedouble : 0;
}

static const char *jtext(const cJSON *o, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(v) ? v->valuestring : "";
}

typedef struct {
    int64_t id, freq_h, freq_s, off_s;
    int active;
    char title[128];
    char theme[32];
} Lottery;

/* 1 found, 0 unknown, -1 failed */
static int lottery_load(Dbx *x, const int64_t id, Lottery *L) {
    cJSON *row = dbx_row(x, SQL(.any = "SELECT lottery_id AS id__i, title, theme, frequency_h AS freq__i, offset_s AS off__i,"
                                       " active AS active__i FROM lotteries WHERE lottery_id = $1"),
                         KN_DB_ARGS(I64S(id)));
    if (dbx_failed(x)) {
        cJSON_Delete(row);
        return -1;
    }
    if (!row) return 0;
    memset(L, 0, sizeof *L);
    L->id = jint(row, "id");
    L->freq_h = jint(row, "freq");
    L->freq_s = L->freq_h * 3600;
    L->off_s = jint(row, "off");
    L->active = jint(row, "active") != 0;
    snprintf(L->title, sizeof L->title, "%s", jtext(row, "title"));
    snprintf(L->theme, sizeof L->theme, "%s", jtext(row, "theme"));
    cJSON_Delete(row);
    return L->freq_s > 0 ? 1 : 0;
}

/* the lottery row lock: every change of one lottery's draws and entries runs under it */
static int lottery_lock(Dbx *x, const int64_t id) {
    return dbx_exec(x, SQL(.any = "UPDATE lotteries SET active = active WHERE lottery_id = $1"), KN_DB_ARGS(I64S(id))) < 0 ? -1 : 0;
}

/* the window's draw id (0 when no one has entered it yet) */
static int64_t draw_of(Dbx *x, const int64_t lottery, const int64_t ends) {
    return dbx_int(x, SQL(.any = "SELECT id FROM lottery_draws WHERE lottery_id = $1 AND ends_s = $2"),
                   KN_DB_ARGS(I64S(lottery), I64S(ends)), 0);
}

/* ------------------------------------------------------------------ view */

/* the draw's prizes as v10's previous_draws[].prizes (a deleted winner's name reads as "") */
static cJSON *prizes_of(Dbx *x, const int64_t draw) {
    cJSON *prizes = dbx_rows(x, SQL(.any = "SELECT id AS id__i, pool_id AS prize_pool_id__i, prize_type, prize_ref,"
                                           " prize_quantity AS prize_quantity__i, won_by_name FROM lottery_prizes"
                                           " WHERE draw_id = $1 ORDER BY id"),
                             KN_DB_ARGS(I64S(draw)));
    for (int i = 0; prizes && i < cJSON_GetArraySize(prizes); i++) {
        cJSON *pz = cJSON_GetArrayItem(prizes, i);
        if (!cJSON_IsString(cJSON_GetObjectItemCaseSensitive(pz, "won_by_name")))
            cJSON_ReplaceItemInObjectCaseSensitive(pz, "won_by_name", cJSON_CreateString(""));
    }
    return prizes;
}

static kn_db_result *run_view(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const int64_t id = PI(0), account = PI(1), now = PI(2);
    Lottery L;
    const int found = lottery_load(&x, id, &L);
    if (found < 0) return dbx_finish(&x, NULL);
    if (found == 0) {
        dbx_refuse(&x, "lottery not found");
        return dbx_finish(&x, NULL);
    }
    cJSON *o = cJSON_CreateObject();
    cJSON *info = cJSON_CreateObject();
    cJSON_AddNumberToObject(info, "id", (double) L.id);
    cJSON_AddStringToObject(info, "title", L.title);
    cJSON_AddNumberToObject(info, "frequency", (double) L.freq_h);
    cJSON_AddStringToObject(info, "theme", L.theme);
    cJSON_AddItemToObject(o, "lottery", info);

    cJSON *costs = dbx_rows(&x, SQL(.any = "SELECT cost_type, cost_ref, cost_quantity AS cost_quantity__i,"
                                           " num_entries AS num_entries__i FROM lottery_costs WHERE lottery_id = $1 ORDER BY ord"),
                            KN_DB_ARGS(I64S(id)));
    if (dbx_failed(&x)) {
        cJSON_Delete(costs);
        cJSON_Delete(o);
        return dbx_finish(&x, NULL);
    }
    cJSON_AddItemToObject(o, "costs", costs ? costs : cJSON_CreateArray());

    int64_t player = 0, total = 0;
    if (L.active) {
        const int64_t end = window_end(now, L.freq_s, L.off_s);
        const int64_t draw = draw_of(&x, id, end);
        if (draw) {
            total = dbx_int(&x, SQL(.any = "SELECT total_entries FROM lottery_draws WHERE id = $1"), KN_DB_ARGS(I64S(draw)), 0);
            player = dbx_int(&x, SQL(.any = "SELECT entries FROM lottery_entries WHERE draw_id = $1 AND account_id = $2"),
                             KN_DB_ARGS(I64S(draw), I64S(account)), 0);
        }
        cJSON *cur = cJSON_CreateObject();
        cJSON_AddNumberToObject(cur, "id", (double) draw);
        cJSON_AddNumberToObject(cur, "lottery_id", (double) id);
        char starts[24], ends[24];
        iso_at(end - L.freq_s, starts);
        iso_at(end, ends);
        cJSON_AddStringToObject(cur, "starts_at", starts);
        cJSON_AddStringToObject(cur, "ends_at", ends);
        cJSON_AddNullToObject(cur, "drawn_at");
        cJSON_AddNumberToObject(cur, "total_entries", (double) total);
        cJSON_AddNumberToObject(cur, "player_entries", (double) player);
        cJSON_AddItemToObject(o, "current_draw", cur);
    }
    cJSON_AddNumberToObject(o, "player_entries", (double) player);
    cJSON_AddNumberToObject(o, "total_entries", (double) total);

    cJSON *draws = dbx_rows(&x, SQL(.any = "SELECT id AS id__i, starts_s AS starts__i, ends_s AS ends__i, drawn_s AS drawn__i,"
                                           " total_entries AS total__i FROM lottery_draws WHERE lottery_id = $1 AND drawn_s IS NOT NULL"
                                           " ORDER BY ends_s DESC LIMIT 10"),
                            KN_DB_ARGS(I64S(id)));
    if (dbx_failed(&x)) {
        cJSON_Delete(draws);
        cJSON_Delete(o);
        return dbx_finish(&x, NULL);
    }
    cJSON *previous = cJSON_CreateArray();
    for (int i = 0; draws && i < cJSON_GetArraySize(draws); i++) {
        const cJSON *row = cJSON_GetArrayItem(draws, i);
        const int64_t did = jint(row, "id");
        cJSON *d = cJSON_CreateObject();
        char a[24], b[24], c2[24];
        iso_at(jint(row, "starts"), a);
        iso_at(jint(row, "ends"), b);
        iso_at(jint(row, "drawn"), c2);
        cJSON_AddNumberToObject(d, "id", (double) did);
        cJSON_AddNumberToObject(d, "lottery_id", (double) id);
        cJSON_AddStringToObject(d, "starts_at", a);
        cJSON_AddStringToObject(d, "ends_at", b);
        cJSON_AddStringToObject(d, "drawn_at", c2);
        cJSON_AddNumberToObject(d, "total_entries", (double) jint(row, "total"));
        cJSON *pz = prizes_of(&x, did);
        if (dbx_failed(&x)) {
            cJSON_Delete(pz);
            cJSON_Delete(d);
            cJSON_Delete(previous);
            cJSON_Delete(draws);
            cJSON_Delete(o);
            return dbx_finish(&x, NULL);
        }
        cJSON_AddItemToObject(d, "prizes", pz ? pz : cJSON_CreateArray());
        cJSON_AddItemToArray(previous, d);
    }
    cJSON_Delete(draws);
    cJSON_AddItemToObject(o, "previous_draws", previous);
    cJSON_AddBoolToObject(o, "event_ended", !L.active);
    return dbx_json(&x, "lottery_view", o);
}
const kn_proc P_LOTTERY_VIEW = {"lottery_view", NULL, run_view};

/* ------------------------------------------------------------------ participate */

/* KR / Junk / RP are spent here; a skin is removed by the caller. Returns -1 (refused) or 0. */
static int spend_currency(Dbx *x, const int64_t account, const int64_t currency, const int64_t amount, const char *ref) {
    if (currency == 1) {
        int64_t funds = 0;
        return db_add_funds(x, account, -amount, "lottery", ref, &funds) < 0 ? -1 : 0;
    }
    if (currency == 2) {
        const int r = junk_spend(x, account, amount, "lottery", NULL);
        if (r == JUNK_INSUFFICIENT) {
            dbx_refuse(x, "Not enough Junk");
            return -1;
        }
        return r < 0 ? -1 : 0;
    }
    if (currency == 3) {
        if (dbx_exec(x, SQL(.any = "UPDATE ranked_points SET balance = balance - {int:$2}, updated_at = {now}"
                                   " WHERE account_id = $1 AND balance >= {int:$2}"),
                     KN_DB_ARGS(I64S(account), I64S(amount))) <= 0) {
            if (!dbx_failed(x)) dbx_refuse(x, "Not enough Ranked Points");
            return -1;
        }
        return dbx_exec(x, SQL(.any = "INSERT INTO ranked_points_tx (account_id, amount, source, ref) VALUES ($1, $2, 'lottery', $3)"),
                        KN_DB_ARGS(I64S(account), I64S(-amount), ref)) < 0 ? -1 : 0;
    }
    dbx_refuse(x, "Invalid cost");
    return -1;
}

/* {player_entries, total_entries} | refused: the lottery's message (v10's text where it has one) */
static kn_db_result *run_participate(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const int64_t account = PI(0), id = PI(1), bundles = PI(4), now = PI(5);
    const char *type = P(2) ? P(2) : "", *ref = P(3) ? P(3) : "";
    Lottery L;
    const int found = lottery_load(&x, id, &L);
    if (found < 0) return dbx_finish(&x, NULL);
    if (found == 0) {
        dbx_refuse(&x, "lottery not found");
        return dbx_finish(&x, NULL);
    }
    if (bundles < 1 || bundles > LOTTERY_MAX_BUNDLES) {
        dbx_refuse(&x, "Please select at least one item to participate");
        return dbx_finish(&x, NULL);
    }
    if (!L.active) {
        dbx_refuse(&x, "The raffle event has ended. Thank you for participating!");
        return dbx_finish(&x, NULL);
    }
    if (lottery_lock(&x, id) < 0) return dbx_finish(&x, NULL);

    const char *kind = !strcmp(type, "skin") ? "skin" : !strcmp(type, "currency") ? "currency" : NULL;
    cJSON *cost = kind ? dbx_row(&x, SQL(.any = "SELECT cost_quantity AS cq__i, num_entries AS ne__i FROM lottery_costs"
                                                " WHERE lottery_id = $1 AND cost_type = $2 AND cost_ref = $3"),
                                 KN_DB_ARGS(I64S(id), type, ref))
                       : NULL;
    if (dbx_failed(&x)) {
        cJSON_Delete(cost);
        return dbx_finish(&x, NULL);
    }
    if (!cost) {
        dbx_refuse(&x, "Invalid cost");
        cJSON_Delete(cost);
        return dbx_finish(&x, NULL);
    }
    const int64_t units = bundles * jint(cost, "cq"), entries = bundles * jint(cost, "ne");
    cJSON_Delete(cost);
    if (units < 1 || entries < 1) {
        dbx_refuse(&x, "Invalid cost");
        return dbx_finish(&x, NULL);
    }

    // the open window's draw, created by its first entry (the lock above makes it one row)
    const int64_t end = window_end(now, L.freq_s, L.off_s);
    int64_t draw = draw_of(&x, id, end);
    if (dbx_failed(&x)) return dbx_finish(&x, NULL);
    if (!draw) {
        draw = dbx_insert_id(&x, SQL(.pg = "INSERT INTO lottery_draws (lottery_id, starts_s, ends_s, total_entries)"
                                           " VALUES ($1, $2, $3, 0) RETURNING id",
                                     .sqlite = "INSERT INTO lottery_draws (lottery_id, starts_s, ends_s, total_entries)"
                                               " VALUES ($1, $2, $3, 0) RETURNING id",
                                     .mysql = "INSERT INTO lottery_draws (lottery_id, starts_s, ends_s, total_entries)"
                                              " VALUES ($1, $2, $3, 0)"),
                             KN_DB_ARGS(I64S(id), I64S(end - L.freq_s), I64S(end)));
        if (dbx_failed(&x) || draw <= 0) return dbx_finish(&x, NULL);
    }

    // the payment: the same ledgers as the other purchases, refused as they refuse
    if (!strcmp(kind ? kind : "", "skin")) {
        const int64_t skin = pint(ref);
        if (skin <= 0 || db_add_skin(&x, account, skin, -units, "lottery", ref, NULL) < 0) {
            if (!dbx_failed(&x)) dbx_refuse(&x, "Item not owned");
            return dbx_finish(&x, NULL);
        }
    } else if (!kind) {
        dbx_refuse(&x, "Invalid cost");
        return dbx_finish(&x, NULL);
    } else {
        const int64_t currency = pint(ref);
        char why[48];
        snprintf(why, sizeof why, "lottery:%lld", (long long) draw);
        if (spend_currency(&x, account, currency, units, why) < 0) return dbx_finish(&x, NULL);
    }

    // one row per account and window: the entries add up
    if (dbx_exec(&x, SQL(.any = "UPDATE lottery_entries SET entries = entries + {int:$3} WHERE draw_id = $1 AND account_id = $2"),
                 KN_DB_ARGS(I64S(draw), I64S(account), I64S(entries))) < 0)
        return dbx_finish(&x, NULL);
    if (dbx_exists(&x, SQL(.any = "SELECT 1 FROM lottery_entries WHERE draw_id = $1 AND account_id = $2"),
                   KN_DB_ARGS(I64S(draw), I64S(account))) == 0 &&
        !dbx_failed(&x) &&
        dbx_exec(&x, SQL(.any = "INSERT INTO lottery_entries (draw_id, account_id, entries) VALUES ($1, $2, $3)"),
                 KN_DB_ARGS(I64S(draw), I64S(account), I64S(entries))) < 0)
        return dbx_finish(&x, NULL);
    if (dbx_exec(&x, SQL(.any = "UPDATE lottery_draws SET total_entries = total_entries + {int:$2} WHERE id = $1"),
                 KN_DB_ARGS(I64S(draw), I64S(entries))) < 0)
        return dbx_finish(&x, NULL);

    cJSON *o = cJSON_CreateObject();
    const int64_t mine = dbx_int(&x, SQL(.any = "SELECT entries FROM lottery_entries WHERE draw_id = $1 AND account_id = $2"),
                                 KN_DB_ARGS(I64S(draw), I64S(account)), 0);
    const int64_t total = dbx_int(&x, SQL(.any = "SELECT total_entries FROM lottery_draws WHERE id = $1"), KN_DB_ARGS(I64S(draw)), 0);
    cJSON_AddNumberToObject(o, "player_entries", (double) mine);
    cJSON_AddNumberToObject(o, "total_entries", (double) total);
    return dbx_json(&x, "lottery_participate", o);
}
const kn_proc P_LOTTERY_PARTICIPATE = {"lottery_participate", NULL, run_participate};

/* ------------------------------------------------------------------ settlement */

/* the winning entry: the account whose entries cover the drawn ticket */
static int64_t ticket_owner(const cJSON *entries, const uint64_t ticket) {
    uint64_t at = 0;
    for (int i = 0; i < cJSON_GetArraySize(entries); i++) {
        const cJSON *e = cJSON_GetArrayItem(entries, i);
        at += (uint64_t) jint(e, "entries");
        if (ticket < at) return jint(e, "account");
    }
    return 0;
}

/* Draws one window: 1 when it paid, 0 when it was already drawn or had no entries, -1 on failure. */
static int draw_one(Dbx *x, const int64_t draw, const int64_t id, const int64_t now) {
    if (lottery_lock(x, id) < 0) return -1;
    const int64_t claimed = dbx_exec(x, SQL(.any = "UPDATE lottery_draws SET drawn_s = {int:$2} WHERE id = $1 AND drawn_s IS NULL"),
                                     KN_DB_ARGS(I64S(draw), I64S(now)));
    if (claimed <= 0) return claimed < 0 ? -1 : 0;
    cJSON *entries = dbx_rows(x, SQL(.any = "SELECT account_id AS account__i, entries AS entries__i FROM lottery_entries"
                                            " WHERE draw_id = $1 ORDER BY account_id"),
                              KN_DB_ARGS(I64S(draw)));
    if (dbx_failed(x)) {
        cJSON_Delete(entries);
        return -1;
    }
    uint64_t total = 0;
    for (int i = 0; entries && i < cJSON_GetArraySize(entries); i++) total += (uint64_t) jint(cJSON_GetArrayItem(entries, i), "entries");
    if (total == 0) {
        cJSON_Delete(entries);
        return 0;
    }
    cJSON *pools = dbx_rows(x, SQL(.any = "SELECT pool_id AS pool__i, winners AS winners__i FROM lottery_pools"
                                          " WHERE lottery_id = $1 ORDER BY pool_id"),
                            KN_DB_ARGS(I64S(id)));
    int paid = 0;
    char ref[48];
    snprintf(ref, sizeof ref, "lottery:%lld", (long long) draw);
    for (int pi = 0; !dbx_failed(x) && pools && pi < cJSON_GetArraySize(pools); pi++) {
        const int64_t pool = jint(cJSON_GetArrayItem(pools, pi), "pool");
        const int64_t winners = jint(cJSON_GetArrayItem(pools, pi), "winners");
        cJSON *skins = dbx_rows(x, SQL(.any = "SELECT skin_id AS skin__i FROM lottery_pool_skins"
                                              " WHERE lottery_id = $1 AND pool_id = $2 ORDER BY skin_id"),
                                KN_DB_ARGS(I64S(id), I64S(pool)));
        const int nskins = cJSON_GetArraySize(skins);
        for (int64_t w = 0; nskins > 0 && w < winners && !dbx_failed(x); w++) {
            const int64_t winner = ticket_owner(entries, kn_random_u64() % total);
            const int64_t skin = jint(cJSON_GetArrayItem(skins, (int) (kn_random_u64() % (uint64_t) nskins)), "skin");
            char *name = dbx_text(x, SQL(.any = "SELECT COALESCE(NULLIF(p.player_alias, ''), a.username) FROM accounts a" " LEFT JOIN players p ON p.player_id = a.id WHERE a.id = $1"), KN_DB_ARGS(I64S(winner)));
            char skin_text[24];
            snprintf(skin_text, sizeof skin_text, "%lld", (long long) skin);
            if (dbx_exec(x, SQL(.any = "INSERT INTO lottery_prizes (draw_id, pool_id, prize_type, prize_ref, prize_quantity, winner_id, won_by_name)"
                                       " VALUES ($1, $2, 'skin', $3, 1, $4, $5)"),
                         KN_DB_ARGS(I64S(draw), I64S(pool), skin_text, I64S(winner), name ? name : "")) < 0) {
                free(name);
                cJSON_Delete(skins);
                cJSON_Delete(pools);
                cJSON_Delete(entries);
                return -1;
            }
            free(name);
            if (db_add_skin(x, winner, skin, 1, "lottery", ref, NULL) < 0) {
                cJSON_Delete(skins);
                cJSON_Delete(pools);
                cJSON_Delete(entries);
                return -1;
            }
            paid++;
        }
        cJSON_Delete(skins);
    }
    cJSON_Delete(pools);
    cJSON_Delete(entries);
    return dbx_failed(x) ? -1 : paid > 0 ? 1 : 0;
}

/* $0 now (epoch seconds) -> {drawn}: settles every window that has ended and has entries */
static kn_db_result *run_settle(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const int64_t now = PI(0);
    cJSON *due = dbx_rows(&x, SQL(.any = "SELECT id AS id__i, lottery_id AS lottery__i FROM lottery_draws"
                                         " WHERE drawn_s IS NULL AND ends_s <= {int:$1} ORDER BY ends_s, id"),
                          KN_DB_ARGS(I64S(now)));
    if (dbx_failed(&x)) {
        cJSON_Delete(due);
        return dbx_finish(&x, NULL);
    }
    int64_t drawn = 0;
    for (int i = 0; due && i < cJSON_GetArraySize(due); i++) {
        const cJSON *row = cJSON_GetArrayItem(due, i);
        const int r = draw_one(&x, jint(row, "id"), jint(row, "lottery"), now);
        if (r < 0) break;
        drawn += r;
    }
    cJSON_Delete(due);
    if (dbx_failed(&x)) return dbx_finish(&x, NULL);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "drawn", (double) drawn);
    return dbx_json(&x, "lottery_settle", o);
}
const kn_proc P_LOTTERY_SETTLE = {"lottery_settle", NULL, run_settle};
