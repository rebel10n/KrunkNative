/*
 * services/brain/db_strike.c - Krunker Strike's economy on the tables of migration
 * 0320_strike_economy (strike_econ.c is the caller; doc/v10/krunker-strike.md "Economy").
 * C on every driver (no .pg text). Everything here is Strike's own: strike_wallets, strike_items,
 * strike_loadout, strike_progress, ... never Box Office's players / inventory / kr_transactions.
 *
 * Concurrency: every write proc first makes sure the account's strike_progress row exists and
 * locks it (postgres row lock, mysql FOR UPDATE, sqlite BEGIN IMMEDIATE), so two spins / buys /
 * claims of one account run one after the other and a balance is never spent twice.
 *
 *   P_STRIKE_CATALOG ()                             -> {items, pools, pool_items, wheels, slices,
 *                                                       products, missions, track, config} (rows)
 *   P_STRIKE_ME (id)                                -> {wallet, progress, loadout, claims, items, purchases}
 *   P_STRIKE_EQUIP (id, slot, item | "")            -> {loadout}       owned items only; "" unequips
 *   P_STRIKE_SPIN (id, wheel, currency, cost, now_ms, period_ms, max_acc, golden(0|1), heroic_n,
 *                  items "a,b,..", meter)           -> {wallet, goldenSpinCount, goldenFromMeter}
 *   P_STRIKE_PURCHASE (id, product, period, max, currency, cost, rewards json, quantity)
 *                                                   -> {rewards, wallet}
 *   P_STRIKE_MISSIONS (id, day, spec "id:...,..")    -> {missions: [{id, progress, completed, claimed}], xp}
 *   P_STRIKE_MISSION_CLAIM (id, day, mission, rewards json, xp_needed) -> {rewards, wallet, missions}
 *   P_STRIKE_TRACK_CLAIM (id, slot, xp_needed, choice, rewards json) -> {rewards, wallet, claims}
 *   P_STRIKE_COMMIT (id, match_key, player json, day, spec, max_kr) -> {duplicate, xp, xp_before, kr}
 *   P_STRIKE_OVERLAY (id)                           -> {items: [{item, count}], kr, xp, loadout}
 *   P_STRIKE_LEADERBOARD (board, limit, offset, id) -> {rows: [{rank, id, name, value, xp}], me}
 *   P_STRIKE_FEEDBACK (id, message, email | "") -> {id}   one strike_feedback row (created_at now)
 */
#include "db_procs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "knsvc/json.h"
#include "strike_econ.h"

#define P(i) (n > (i) ? p[i] : NULL)
#define PS(i) (P(i) ? P(i) : "")
#define PI(i) pint(P(i))

static int64_t pint(const char *s) { return s ? strtoll(s, NULL, 10) : 0; }

/* ------------------------------------------------------------------ helpers */

/* the account's strike_progress row, created when missing, then locked */
static void lock_account(Dbx *x, const char *id) {
    dbx_exec(x, SQL(.any = "{insert_ignore} INTO strike_progress (account_id) VALUES ($1) {ignore_conflicts}"), KN_DB_ARGS(id));
    if (dbx_is_mysql(x)) dbx_int(x, SQL(.any = "SELECT xp FROM strike_progress WHERE account_id = $1 FOR UPDATE"), KN_DB_ARGS(id), 0);
    else dbx_exec(x, SQL(.any = "UPDATE strike_progress SET updated_at = updated_at WHERE account_id = $1"), KN_DB_ARGS(id));
}

static void ledger(Dbx *x, const char *id, const char *kind, const char *currency, int64_t delta, const char *item, const char *ref) {
    dbx_exec(x, SQL(.any = "INSERT INTO strike_ledger (account_id, kind, currency, delta, item_id, ref) VALUES ($1, $2, $3, {int:$4}, $5, $6)"),
             KN_DB_ARGS(id, kind, currency ? currency : "", I64S(delta), item ? item : "", ref ? ref : ""));
}

/* delta < 0 spends (refused when the wallet holds less); returns the new amount, -1 on refusal */
static int64_t wallet_add(Dbx *x, const char *id, const char *currency, int64_t delta, const char *kind, const char *ref) {
    if (dbx_failed(x) || !currency || !*currency) return -1;
    if (delta == 0) return dbx_int(x, SQL(.any = "SELECT amount FROM strike_wallets WHERE account_id = $1 AND currency = $2"),
                                   KN_DB_ARGS(id, currency), 0);
    if (delta > 0) {
        dbx_exec(x, SQL(.any = "{insert_ignore} INTO strike_wallets (account_id, currency, amount) VALUES ($1, $2, 0) {ignore_conflicts}"),
                 KN_DB_ARGS(id, currency));
        dbx_exec(x, SQL(.any = "UPDATE strike_wallets SET amount = amount + {int:$3} WHERE account_id = $1 AND currency = $2"),
                 KN_DB_ARGS(id, currency, I64S(delta)));
    } else {
        const int64_t done = dbx_exec(x, SQL(.any = "UPDATE strike_wallets SET amount = amount - {int:$3}"
                                                    " WHERE account_id = $1 AND currency = $2 AND amount >= {int:$3}"),
                                      KN_DB_ARGS(id, currency, I64S(-delta)));
        if (dbx_failed(x)) return -1;
        if (done != 1) {
            char msg[64];
            snprintf(msg, sizeof msg, "%s", !strcmp(currency, "kr") ? "Not enough KR" : "Not enough tickets");
            dbx_refuse(x, msg);
            return -1;
        }
    }
    ledger(x, id, kind, currency, delta, NULL, ref);
    return dbx_int(x, SQL(.any = "SELECT amount FROM strike_wallets WHERE account_id = $1 AND currency = $2"), KN_DB_ARGS(id, currency), 0);
}

static void item_add(Dbx *x, const char *id, const char *item, int64_t count, const char *kind, const char *ref) {
    if (dbx_failed(x) || !item || !*item || count <= 0) return;
    dbx_exec(x, SQL(.any = "{insert_ignore} INTO strike_items (account_id, item_id, count) VALUES ($1, $2, 0) {ignore_conflicts}"),
             KN_DB_ARGS(id, item));
    dbx_exec(x, SQL(.any = "UPDATE strike_items SET count = count + {int:$3} WHERE account_id = $1 AND item_id = $2"),
             KN_DB_ARGS(id, item, I64S(count)));
    ledger(x, id, kind, NULL, count, item, ref);
}

/* FRVR's reward union ({type: currency|item|class|feature}) -> the account; returns what was given */
static cJSON *grant(Dbx *x, const char *id, const cJSON *rewards, int64_t times, const char *kind, const char *ref) {
    cJSON *out = cJSON_CreateArray();
    const cJSON *r;
    if (times < 1) times = 1;
    cJSON_ArrayForEach(r, rewards) {
        if (dbx_failed(x)) break;
        const char *type = kn_json_str(r, "type", "");
        if (!strcmp(type, "currency")) {
            const int64_t amount = kn_json_int(r, "amount", 0) * times;
            if (amount > 0) wallet_add(x, id, kn_json_str(r, "currency", ""), amount, kind, ref);
        } else if (!strcmp(type, "item") || !strcmp(type, "class")) {
            item_add(x, id, kn_json_str(r, "item", kn_json_str(r, "itemDefId", "")), times, kind, ref);
        } else if (!strcmp(type, "feature")) {
            char fid[64];
            snprintf(fid, sizeof fid, "feature:%s", kn_json_str(r, "feature", ""));
            item_add(x, id, fid, 1, kind, ref);
        } else if (!strcmp(type, "battlepass")) {
            /* FRVR's Battle Pass ticket: nothing to add here (the pass is the Battle Pass system's); it is
             * echoed so the client shows the ticket and its Awesome! opens the pass */
        } else {
            continue;
        }
        cJSON *g = cJSON_Duplicate(r, 1);
        if (times > 1 && cJSON_GetObjectItemCaseSensitive(g, "amount")) cJSON_ReplaceItemInObject(g, "amount", cJSON_CreateNumber((double) (kn_json_int(r, "amount", 0) * times)));
        cJSON_AddItemToArray(out, g);
    }
    return out;
}

static cJSON *wallet_json(Dbx *x, const char *id) {
    cJSON *w = cJSON_CreateObject();
    cJSON *rows = dbx_rows(x, SQL(.any = "SELECT currency, amount AS amount__i FROM strike_wallets WHERE account_id = $1 ORDER BY currency"),
                           KN_DB_ARGS(id));
    const cJSON *r;
    cJSON_ArrayForEach(r, rows) cJSON_AddNumberToObject(w, kn_json_str(r, "currency", "?"), (double) kn_json_int(r, "amount", 0));
    cJSON_Delete(rows);
    return w;
}

static cJSON *loadout_json(Dbx *x, const char *id) {
    cJSON *l = cJSON_CreateObject();
    cJSON *rows = dbx_rows(x, SQL(.any = "SELECT slot, item_id FROM strike_loadout WHERE account_id = $1 ORDER BY slot"), KN_DB_ARGS(id));
    const cJSON *r;
    cJSON_ArrayForEach(r, rows) cJSON_AddStringToObject(l, kn_json_str(r, "slot", "?"), kn_json_str(r, "item_id", ""));
    cJSON_Delete(rows);
    return l;
}

static cJSON *parse_param(const char *s) {
    cJSON *v = s && *s ? cJSON_Parse(s) : NULL;
    return v ? v : cJSON_CreateArray();
}

/* ------------------------------------------------------------------ catalogue */

static kn_db_result *run_catalog(kn_dbconn *c, int n, const char *const *p) {
    (void) n;
    (void) p;
    Dbx x = {c, NULL};
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "items", dbx_rows(&x, SQL(.any = "SELECT id, item_type AS item_type__i, weapon AS weapon__i, name,"
                                                                " rarity AS rarity__i, skin_index AS skin_index__i, class_id AS class_id__i,"
                                                                " is_default AS is_default__i, spinnable AS spinnable__i, origin"
                                                                " FROM strike_item_defs ORDER BY sort, id"), 0, NULL));
    cJSON_AddItemToObject(o, "pools", dbx_rows(&x, SQL(.any = "SELECT id, name, color, rarity AS rarity__i FROM strike_pools ORDER BY sort, id"), 0, NULL));
    cJSON_AddItemToObject(o, "pool_items", dbx_rows(&x, SQL(.any = "SELECT pi.pool_id, pi.item_id FROM strike_pool_items pi"
                                                                     " JOIN strike_item_defs d ON d.id = pi.item_id ORDER BY pi.pool_id, d.sort"), 0, NULL));
    cJSON_AddItemToObject(o, "wheels", dbx_rows(&x, SQL(.any = "SELECT id, name, name_key, wheel_type, price_currency, price_amount AS price_amount__i,"
                                                                 " time_between_s AS time_between_s__i, max_accumulated AS max_accumulated__i,"
                                                                 " heroic_chance AS heroic_chance__f, hide_in_shop AS hide_in_shop__i,"
                                                                 " counts_golden AS counts_golden__i, button_color, category"
                                                                 " FROM strike_wheels WHERE active = 1 ORDER BY sort, id"), 0, NULL));
    cJSON_AddItemToObject(o, "slices", dbx_rows(&x, SQL(.any = "SELECT wheel_id, pool_id, weight AS weight__f, guaranteed_if AS guaranteed_if__i"
                                                                 " FROM strike_wheel_slices ORDER BY wheel_id, sort"), 0, NULL));
    cJSON_AddItemToObject(o, "products", dbx_rows(&x, SQL(.any = "SELECT id, section, title, price_type, price_currency, price_amount AS price_amount__i,"
                                                                   " rewards, refresh_s AS refresh_s__i, max_purchases AS max_purchases__i"
                                                                   " FROM strike_products WHERE active = 1 ORDER BY sort, id"), 0, NULL));
    cJSON_AddItemToObject(o, "missions", dbx_rows(&x, SQL(.any = "SELECT id, mission_type, mkey, text_key, required AS required__i,"
                                                                   " max_mode AS max_mode__i, weight AS weight__i, rewards"
                                                                   " FROM strike_missions_def WHERE active = 1 ORDER BY sort, id"), 0, NULL));
    cJSON_AddItemToObject(o, "track", dbx_rows(&x, SQL(.any = "SELECT slot_id, level AS level__i, player_choice AS player_choice__i, rewards"
                                                                " FROM strike_track ORDER BY level, slot_id"), 0, NULL));
    cJSON_AddItemToObject(o, "config", dbx_rows(&x, SQL(.any = "SELECT ckey, value FROM strike_config ORDER BY ckey"), 0, NULL));
    if (dbx_failed(&x)) {
        cJSON_Delete(o);
        return dbx_finish(&x, NULL);
    }
    return dbx_json(&x, "catalog", o);
}
const kn_proc P_STRIKE_CATALOG = {"strike_catalog", NULL, run_catalog};

/* ------------------------------------------------------------------ the account */

static kn_db_result *run_me(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const char *id = PS(0);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "wallet", wallet_json(&x, id));
    cJSON *prog = dbx_row(&x, SQL(.any = "SELECT xp AS xp__i, kills AS kills__i, deaths AS deaths__i, wins AS wins__i, games AS games__i,"
                                         " score AS score__i, time_ms AS time_ms__i, golden_spin_count AS golden_spin_count__i"
                                         " FROM strike_progress WHERE account_id = $1"), KN_DB_ARGS(id));
    cJSON_AddItemToObject(o, "progress", prog ? prog : cJSON_CreateObject());
    cJSON_AddItemToObject(o, "loadout", loadout_json(&x, id));
    cJSON *claims = cJSON_CreateArray();
    cJSON *rows = dbx_rows(&x, SQL(.any = "SELECT slot_id FROM strike_track_claims WHERE account_id = $1 ORDER BY claimed_at, slot_id"), KN_DB_ARGS(id));
    const cJSON *r;
    cJSON_ArrayForEach(r, rows) cJSON_AddItemToArray(claims, cJSON_CreateString(kn_json_str(r, "slot_id", "")));
    cJSON_Delete(rows);
    cJSON_AddItemToObject(o, "claims", claims);
    cJSON_AddItemToObject(o, "items", dbx_rows(&x, SQL(.any = "SELECT item_id, count AS count__i, {epoch_ms:first_at} AS first_ms__i"
                                                                " FROM strike_items WHERE account_id = $1 AND count > 0 ORDER BY item_id"),
                                                 KN_DB_ARGS(id)));
    cJSON_AddItemToObject(o, "purchases", dbx_rows(&x, SQL(.any = "SELECT product_id, period AS period__i, count AS count__i"
                                                                    " FROM strike_purchases WHERE account_id = $1 ORDER BY product_id, period"),
                                                     KN_DB_ARGS(id)));
    if (dbx_failed(&x)) {
        cJSON_Delete(o);
        return dbx_finish(&x, NULL);
    }
    return dbx_json(&x, "me", o);
}
const kn_proc P_STRIKE_ME = {"strike_me", NULL, run_me};

/* (id, slot, item | "") */
static kn_db_result *run_equip(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const char *id = PS(0), *slot = PS(1), *item = PS(2);
    lock_account(&x, id);
    if (!*item) {
        dbx_exec(&x, SQL(.any = "DELETE FROM strike_loadout WHERE account_id = $1 AND slot = $2"), KN_DB_ARGS(id, slot));
    } else {
        if (!dbx_exists(&x, SQL(.any = "SELECT 1 FROM strike_items WHERE account_id = $1 AND item_id = $2 AND count > 0"), KN_DB_ARGS(id, item))) {
            dbx_refuse(&x, "You don't own this item");
            return dbx_finish(&x, NULL);
        }
        dbx_exec(&x, SQL(.any = "DELETE FROM strike_loadout WHERE account_id = $1 AND slot = $2"), KN_DB_ARGS(id, slot));
        dbx_exec(&x, SQL(.any = "INSERT INTO strike_loadout (account_id, slot, item_id) VALUES ($1, $2, $3)"), KN_DB_ARGS(id, slot, item));
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "loadout", loadout_json(&x, id));
    return dbx_json(&x, "equip", o);
}
const kn_proc P_STRIKE_EQUIP = {"strike_equip", NULL, run_equip};

/* ------------------------------------------------------------------ spins */

/* (id, wheel, currency, cost, now_ms, period_ms, max_acc, golden, heroic_n, items "a,b", meter) */
static kn_db_result *run_spin(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const char *id = PS(0), *wheel = PS(1), *currency = PS(2), *items = PS(9);
    const int64_t cost = PI(3), now = PI(4), period = PI(5), max_acc = PI(6), golden = PI(7), heroic = PI(8), meter = PI(10);
    lock_account(&x, id);
    char ref[96];
    snprintf(ref, sizeof ref, "%s@%lld", wheel, (long long) now);
    if (period > 0) { /* a free wheel: one spin per period, banking up to max_acc */
        const int64_t last = dbx_int(&x, SQL(.any = "SELECT last_spin_ms FROM strike_wheel_spins WHERE account_id = $1 AND wheel_id = $2"),
                                     KN_DB_ARGS(id, wheel), 0);
        const int64_t avail = last <= 0 ? (max_acc > 0 ? max_acc : 1) : (now - last) / period;
        if (avail < 1) {
            dbx_refuse(&x, "No free spin available yet");
            return dbx_finish(&x, NULL);
        }
    }
    if (cost > 0 && wallet_add(&x, id, currency, -cost, "spin", ref) < 0) return dbx_finish(&x, NULL);
    int count = 0;
    for (const char *s = items; *s && !dbx_failed(&x);) {
        const char *comma = strchr(s, ',');
        char item[64];
        snprintf(item, sizeof item, "%.*s", (int) (comma ? comma - s : (long) strlen(s)), s);
        item_add(&x, id, item, 1, "spin", ref);
        count++;
        s = comma ? comma + 1 : s + strlen(s);
    }
    dbx_exec(&x, SQL(.any = "{insert_ignore} INTO strike_wheel_spins (account_id, wheel_id) VALUES ($1, $2) {ignore_conflicts}"), KN_DB_ARGS(id, wheel));
    dbx_exec(&x, SQL(.any = "UPDATE strike_wheel_spins SET spins = spins + {int:$3}, last_spin_ms = {int:$4} WHERE account_id = $1 AND wheel_id = $2"),
             KN_DB_ARGS(id, wheel, I64S(count), I64S(now)));
    /* the golden spin meter: every `meter` paid spins is a golden ticket */
    int64_t meter_count = dbx_int(&x, SQL(.any = "SELECT golden_spin_count FROM strike_progress WHERE account_id = $1"), KN_DB_ARGS(id), 0);
    int64_t from_meter = 0;
    if (golden && meter > 0) {
        meter_count += count;
        from_meter = meter_count / meter;
        meter_count %= meter;
        dbx_exec(&x, SQL(.any = "UPDATE strike_progress SET golden_spin_count = {int:$2}, updated_at = {now} WHERE account_id = $1"),
                 KN_DB_ARGS(id, I64S(meter_count)));
    }
    if (from_meter + heroic > 0) wallet_add(&x, id, "spin_golden_ticket", from_meter + heroic, "golden", ref);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "wallet", wallet_json(&x, id));
    cJSON_AddNumberToObject(o, "goldenSpinCount", (double) meter_count);
    cJSON_AddNumberToObject(o, "goldenFromMeter", (double) from_meter);
    cJSON_AddNumberToObject(o, "granted", count);
    return dbx_json(&x, "spin", o);
}
const kn_proc P_STRIKE_SPIN = {"strike_spin", NULL, run_spin};

/* ------------------------------------------------------------------ the store */

/* (id, product, period, max, currency, cost, rewards json, quantity) */
static kn_db_result *run_purchase(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const char *id = PS(0), *product = PS(1), *currency = PS(4);
    const int64_t period = PI(2), max = PI(3), cost = PI(5);
    int64_t qty = PI(7) > 0 ? PI(7) : 1;
    lock_account(&x, id);
    if (max > 0) {
        const int64_t bought = dbx_int(&x, SQL(.any = "SELECT count FROM strike_purchases WHERE account_id = $1 AND product_id = $2 AND period = {int:$3}"),
                                       KN_DB_ARGS(id, product, I64S(period)), 0);
        if (bought + qty > max) {
            dbx_refuse(&x, "Already purchased");
            return dbx_finish(&x, NULL);
        }
    }
    if (cost > 0 && wallet_add(&x, id, currency, -cost * qty, "purchase", product) < 0) return dbx_finish(&x, NULL);
    dbx_exec(&x, SQL(.any = "{insert_ignore} INTO strike_purchases (account_id, product_id, period, count) VALUES ($1, $2, {int:$3}, 0) {ignore_conflicts}"),
             KN_DB_ARGS(id, product, I64S(period)));
    dbx_exec(&x, SQL(.any = "UPDATE strike_purchases SET count = count + {int:$4} WHERE account_id = $1 AND product_id = $2 AND period = {int:$3}"),
             KN_DB_ARGS(id, product, I64S(period), I64S(qty)));
    cJSON *rewards = parse_param(P(6));
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "rewards", grant(&x, id, rewards, qty, "purchase", product));
    cJSON_Delete(rewards);
    cJSON_AddItemToObject(o, "wallet", wallet_json(&x, id));
    return dbx_json(&x, "purchase", o);
}
const kn_proc P_STRIKE_PURCHASE = {"strike_purchase", NULL, run_purchase};

/* ------------------------------------------------------------------ missions */

/* spec "id:required:max_mode,..." -> rows of today's missions, created when missing */
static void ensure_missions(Dbx *x, const char *id, const char *day, const char *spec) {
    for (const char *s = spec; s && *s && !dbx_failed(x);) {
        const char *comma = strchr(s, ',');
        char mid[64];
        const char *colon = strchr(s, ':');
        const size_t len = (size_t) ((colon && (!comma || colon < comma) ? colon : comma ? comma : s + strlen(s)) - s);
        snprintf(mid, sizeof mid, "%.*s", (int) len, s);
        dbx_exec(x, SQL(.any = "{insert_ignore} INTO strike_daily_missions (account_id, day, mission_id) VALUES ($1, {int:$2}, $3) {ignore_conflicts}"),
                 KN_DB_ARGS(id, day, mid));
        s = comma ? comma + 1 : s + strlen(s);
    }
}

static cJSON *mission_rows(Dbx *x, const char *id, const char *day) {
    return dbx_rows(x, SQL(.any = "SELECT mission_id AS id, progress AS progress__i, completed AS completed__b, claimed AS claimed__b"
                                  " FROM strike_daily_missions WHERE account_id = $1 AND day = {int:$2} ORDER BY mission_id"),
                    KN_DB_ARGS(id, day));
}

static kn_db_result *run_missions(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    ensure_missions(&x, PS(0), PS(1), PS(2));
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "missions", mission_rows(&x, PS(0), PS(1)));
    cJSON_AddNumberToObject(o, "xp", (double) dbx_int(&x, SQL(.any = "SELECT xp FROM strike_progress WHERE account_id = $1"), KN_DB_ARGS(PS(0)), 0));
    return dbx_json(&x, "missions", o);
}
const kn_proc P_STRIKE_MISSIONS = {"strike_missions", NULL, run_missions};

/* (id, day, mission, rewards json, xp_needed: the missions' unlock level) */
static kn_db_result *run_mission_claim(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const char *id = PS(0), *day = PS(1), *mission = PS(2);
    lock_account(&x, id);
    if (dbx_int(&x, SQL(.any = "SELECT xp FROM strike_progress WHERE account_id = $1"), KN_DB_ARGS(id), 0) < PI(4)) {
        dbx_refuse(&x, "Missions are locked");
        return dbx_finish(&x, NULL);
    }
    cJSON *row = dbx_row(&x, SQL(.any = "SELECT completed AS completed__i, claimed AS claimed__i FROM strike_daily_missions"
                                        " WHERE account_id = $1 AND day = {int:$2} AND mission_id = $3"), KN_DB_ARGS(id, day, mission));
    if (dbx_failed(&x)) return dbx_finish(&x, NULL);
    const char *refusal = !row ? "Mission not found" : kn_json_int(row, "claimed", 0) ? "Mission already claimed"
                        : !kn_json_int(row, "completed", 0) ? "Mission not completed" : NULL;
    cJSON_Delete(row);
    if (refusal) {
        dbx_refuse(&x, refusal);
        return dbx_finish(&x, NULL);
    }
    dbx_exec(&x, SQL(.any = "UPDATE strike_daily_missions SET claimed = 1 WHERE account_id = $1 AND day = {int:$2} AND mission_id = $3"),
             KN_DB_ARGS(id, day, mission));
    cJSON *rewards = parse_param(P(3));
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "rewards", grant(&x, id, rewards, 1, "mission", mission));
    cJSON_Delete(rewards);
    cJSON_AddItemToObject(o, "wallet", wallet_json(&x, id));
    cJSON_AddItemToObject(o, "missions", mission_rows(&x, id, day));
    return dbx_json(&x, "mission_claim", o);
}
const kn_proc P_STRIKE_MISSION_CLAIM = {"strike_mission_claim", NULL, run_mission_claim};

/* ------------------------------------------------------------------ the progression track */

/* (id, slot, xp_needed, choice, rewards json) */
static kn_db_result *run_track_claim(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const char *id = PS(0), *slot = PS(1);
    lock_account(&x, id);
    const int64_t xp = dbx_int(&x, SQL(.any = "SELECT xp FROM strike_progress WHERE account_id = $1"), KN_DB_ARGS(id), 0);
    if (dbx_failed(&x)) return dbx_finish(&x, NULL);
    if (xp < PI(2)) {
        dbx_refuse(&x, "Level not reached");
        return dbx_finish(&x, NULL);
    }
    const int64_t done = dbx_exec(&x, SQL(.any = "{insert_ignore} INTO strike_track_claims (account_id, slot_id, choice) VALUES ($1, $2, {int:$3}) {ignore_conflicts}"),
                                  KN_DB_ARGS(id, slot, I64S(PI(3) > 0 ? PI(3) : 0)));
    if (dbx_failed(&x)) return dbx_finish(&x, NULL);
    if (done != 1) {
        dbx_refuse(&x, "Reward already claimed");
        return dbx_finish(&x, NULL);
    }
    cJSON *rewards = parse_param(P(4));
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "rewards", grant(&x, id, rewards, 1, "track", slot));
    cJSON_Delete(rewards);
    cJSON_AddItemToObject(o, "wallet", wallet_json(&x, id));
    cJSON *claims = cJSON_CreateArray();
    cJSON *rows = dbx_rows(&x, SQL(.any = "SELECT slot_id FROM strike_track_claims WHERE account_id = $1 ORDER BY claimed_at, slot_id"), KN_DB_ARGS(id));
    const cJSON *r;
    cJSON_ArrayForEach(r, rows) cJSON_AddItemToArray(claims, cJSON_CreateString(kn_json_str(r, "slot_id", "")));
    cJSON_Delete(rows);
    cJSON_AddItemToObject(o, "claims", claims);
    return dbx_json(&x, "track_claim", o);
}
const kn_proc P_STRIKE_TRACK_CLAIM = {"strike_track_claim", NULL, run_track_claim};

/* ------------------------------------------------------------------ a Strike match */

/* one mission key's progress from a commit: the "prog" tallies when it has the key ([key, value,
 * weapon, streak, item]; max_mode: the best value), else the commit's own counters */
static int64_t mission_delta(const cJSON *player, const char *key, int max_mode, int *has) {
    int64_t sum = 0, best = 0;
    int found = 0;
    const cJSON *row;
    cJSON_ArrayForEach(row, cJSON_GetObjectItemCaseSensitive(player, "prog")) {
        const cJSON *k = cJSON_GetArrayItem(row, 0), *v = cJSON_GetArrayItem(row, 1);
        if (!cJSON_IsString(k) || strcmp(k->valuestring, key) || !cJSON_IsNumber(v)) continue;
        found = 1;
        sum += (int64_t) v->valuedouble;
        if ((int64_t) v->valuedouble > best) best = (int64_t) v->valuedouble;
    }
    if (!found) {
        static const struct { const char *key, *field; } direct[] = {{"k", "kills"}, {"d", "deaths"}, {"score", "score"}, {"w", "won"}, {"gp", "games"}};
        for (size_t i = 0; i < sizeof direct / sizeof *direct; i++) {
            if (strcmp(direct[i].key, key)) continue;
            found = 1;
            const cJSON *v = cJSON_GetObjectItemCaseSensitive(player, direct[i].field);
            sum = best = cJSON_IsBool(v) ? cJSON_IsTrue(v) : kn_json_int(player, direct[i].field, 0);
        }
    }
    *has = found;
    return max_mode ? best : sum;
}

/* (id, match_key, player json, day, spec "id:key:required:max,...", max_kr) */
static kn_db_result *run_commit(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const char *id = PS(0), *match = PS(1), *day = PS(3), *spec = PS(4);
    cJSON *player = cJSON_Parse(PS(2));
    if (!player) player = cJSON_CreateObject();
    lock_account(&x, id);
    const int64_t fresh = dbx_exec(&x, SQL(.any = "{insert_ignore} INTO strike_match_results (match_key, account_id) VALUES ($1, $2) {ignore_conflicts}"),
                                   KN_DB_ARGS(match, id));
    cJSON *o = cJSON_CreateObject();
    const int64_t xp_before = dbx_int(&x, SQL(.any = "SELECT xp FROM strike_progress WHERE account_id = $1"), KN_DB_ARGS(id), 0);
    cJSON_AddNumberToObject(o, "xp_before", (double) xp_before);
    if (dbx_failed(&x) || fresh != 1) {
        cJSON_AddBoolToObject(o, "duplicate", 1);
        cJSON_AddNumberToObject(o, "xp", (double) xp_before);
        cJSON_Delete(player);
        return dbx_failed(&x) ? (cJSON_Delete(o), dbx_finish(&x, NULL)) : dbx_json(&x, "commit", o);
    }
    const int64_t xp = kn_json_int(player, "xp", 0) > 0 ? kn_json_int(player, "xp", 0) : 0;
    int64_t kr = kn_json_int(player, "kr", 0);
    if (kr < 0) kr = 0;
    if (PI(5) > 0 && kr > PI(5)) kr = PI(5);
    const int64_t time_ms = kn_json_int(player, "timePlayed", kn_json_int(player, "time_ms", 0));
    dbx_exec(&x, SQL(.any = "UPDATE strike_progress SET xp = xp + {int:$2}, kills = kills + {int:$3}, deaths = deaths + {int:$4},"
                            " wins = wins + {int:$5}, games = games + {int:$6}, score = score + {int:$7}, time_ms = time_ms + {int:$8},"
                            " updated_at = {now} WHERE account_id = $1"),
             KN_DB_ARGS(id, I64S(xp), I64S(kn_json_int(player, "kills", 0)), I64S(kn_json_int(player, "deaths", 0)),
                        I64S(kn_json_bool(player, "won", 0) ? 1 : 0), I64S(kn_json_int(player, "games", 0)),
                        I64S(kn_json_int(player, "score", 0)), I64S(time_ms > 0 ? time_ms : 0)));
    if (kr > 0) wallet_add(&x, id, "kr", kr, "match", match);
    /* today's missions */
    ensure_missions(&x, id, day, spec);
    cJSON *progressed = cJSON_CreateArray();
    for (const char *s = spec; *s && !dbx_failed(&x);) {
        const char *comma = strchr(s, ',');
        char part[160], mid[64] = "", key[24] = "";
        int req = 0, max_mode = 0;
        snprintf(part, sizeof part, "%.*s", (int) (comma ? comma - s : (long) strlen(s)), s);
        s = comma ? comma + 1 : s + strlen(s);
        if (sscanf(part, "%63[^:]:%23[^:]:%d:%d", mid, key, &req, &max_mode) < 3) continue;
        int has = 0;
        const int64_t d = mission_delta(player, key, max_mode, &has);
        if (!has || d <= 0) continue;
        if (max_mode)
            dbx_exec(&x, SQL(.any = "UPDATE strike_daily_missions SET progress = CASE WHEN progress < {int:$4} THEN {int:$4} ELSE progress END WHERE account_id = $1 AND day = {int:$2} AND mission_id = $3"),
                     KN_DB_ARGS(id, day, mid, I64S(d)));
        else
            dbx_exec(&x, SQL(.any = "UPDATE strike_daily_missions SET progress = progress + {int:$4} WHERE account_id = $1 AND day = {int:$2} AND mission_id = $3"),
                     KN_DB_ARGS(id, day, mid, I64S(d)));
        dbx_exec(&x, SQL(.any = "UPDATE strike_daily_missions SET completed = 1 WHERE account_id = $1 AND day = {int:$2} AND mission_id = $3 AND progress >= {int:$4}"),
                 KN_DB_ARGS(id, day, mid, I64S(req)));
        cJSON_AddItemToArray(progressed, cJSON_CreateString(mid));
    }
    cJSON_Delete(player);
    cJSON_AddBoolToObject(o, "duplicate", 0);
    cJSON_AddNumberToObject(o, "xp", (double) (xp_before + xp));
    cJSON_AddNumberToObject(o, "kr", (double) kr);
    cJSON_AddItemToObject(o, "missions", progressed);
    return dbx_json(&x, "commit", o);
}
const kn_proc P_STRIKE_COMMIT = {"strike_commit", NULL, run_commit};

/* what a Strike game server needs of an account: its Strike items (ownership), KR, XP, loadout */
static kn_db_result *run_overlay(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const char *id = PS(0);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "items", dbx_rows(&x, SQL(.any = "SELECT i.item_id AS item, i.count AS count__i, d.skin_index AS skin__i"
                                                                " FROM strike_items i JOIN strike_item_defs d ON d.id = i.item_id"
                                                                " WHERE i.account_id = $1 AND i.count > 0 ORDER BY i.item_id"), KN_DB_ARGS(id)));
    cJSON_AddNumberToObject(o, "kr", (double) dbx_int(&x, SQL(.any = "SELECT amount FROM strike_wallets WHERE account_id = $1 AND currency = 'kr'"),
                                                       KN_DB_ARGS(id), 0));
    cJSON_AddNumberToObject(o, "xp", (double) dbx_int(&x, SQL(.any = "SELECT xp FROM strike_progress WHERE account_id = $1"), KN_DB_ARGS(id), 0));
    cJSON_AddItemToObject(o, "loadout", loadout_json(&x, id));
    if (dbx_failed(&x)) {
        cJSON_Delete(o);
        return dbx_finish(&x, NULL);
    }
    return dbx_json(&x, "overlay", o);
}
const kn_proc P_STRIKE_OVERLAY = {"strike_overlay", NULL, run_overlay};

/* ------------------------------------------------------------------ leaderboards */

/* (board, limit, offset, viewer id): the players by board value, highest first (ties: the older
 * account first) */
static kn_db_result *run_leaderboard(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    static const char *const boards[] = {"xp", "kills", "wins", "score", "games"};
    const char *board = NULL, *want = n > 0 && p[0] ? p[0] : "";
    for (size_t i = 0; i < sizeof boards / sizeof *boards; i++)
        if (!strcmp(want, boards[i])) board = boards[i];
    if (!board) {
        dbx_refuse(&x, "Unknown leaderboard");
        return dbx_finish(&x, NULL);
    }
    char q[640];
    snprintf(q, sizeof q, "SELECT s.account_id AS id__i, a.username AS name, s.%s AS value__i, s.xp AS xp__i"
                          " FROM strike_progress s JOIN accounts a ON a.id = s.account_id"
                          " WHERE s.%s > 0 ORDER BY s.%s DESC, s.account_id ASC LIMIT {int:$1} OFFSET {int:$2}", board, board, board);
    const kn_sql sql = {.any = q};
    cJSON *rows = dbx_rows(&x, &sql, KN_DB_ARGS(I64S(PI(1)), I64S(PI(2))));
    int rank = (int) PI(2);
    cJSON *r;
    cJSON_ArrayForEach(r, rows) cJSON_AddNumberToObject(r, "rank", ++rank);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "board", board);
    cJSON_AddItemToObject(o, "rows", rows ? rows : cJSON_CreateArray());
    if (PI(3) > 0) {
        char mq[512];
        snprintf(mq, sizeof mq, "SELECT s.%s AS value__i, (SELECT count(*) FROM strike_progress o WHERE o.%s > s.%s"
                                " OR (o.%s = s.%s AND o.account_id < s.account_id)) + 1 AS rank__i"
                                " FROM strike_progress s WHERE s.account_id = $1", board, board, board, board, board);
        const kn_sql msql = {.any = mq};
        cJSON *me = dbx_row(&x, &msql, KN_DB_ARGS(PS(3)));
        cJSON_AddItemToObject(o, "me", me ? me : cJSON_CreateNull());
    }
    if (dbx_failed(&x)) {
        cJSON_Delete(o);
        return dbx_finish(&x, NULL);
    }
    return dbx_json(&x, "leaderboard", o);
}
const kn_proc P_STRIKE_LEADERBOARD = {"strike_leaderboard", NULL, run_leaderboard};

/* ------------------------------------------------------------------ feedback */

/* (account id, message, email): one row of strike_feedback; the caller checked the text (route in
 * strike_econ.c). The timestamp is the table's created_at default. */
static kn_db_result *run_feedback(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const int64_t fid = dbx_insert_id(
        &x,
        /* RETURNING id on postgres (verbatim text: plain casts) / sqlite, mysql's auto-increment id (dbx_insert_id) */
        SQL(.pg = "INSERT INTO strike_feedback (account_id, message, email) VALUES ($1::int, $2, $3) RETURNING id",
            .sqlite = "INSERT INTO strike_feedback (account_id, message, email) VALUES ({int:$1}, $2, $3) RETURNING id",
            .any = "INSERT INTO strike_feedback (account_id, message, email) VALUES ({int:$1}, $2, $3)"),
        KN_DB_ARGS(PS(0), PS(1), PS(2)));
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "id", (double) fid);
    return dbx_json(&x, "feedback", o);
}
const kn_proc P_STRIKE_FEEDBACK = {"strike_feedback", NULL, run_feedback};
