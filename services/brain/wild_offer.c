/*
 * services/brain/wild_offer.c - Krunker v10.0.2's Wild Offers over the social socket.
 *
 * v10 asks its shop API (v10.js g:26553-26573): GET shop/wild-offer -> {data: {offer}},
 * POST shop/wild-offer/purchase {offer_id} -> {data: {success, message, rewards}} and POST
 * shop/wild-offer/refresh {offer_id} -> {data: {success, message, refresh_cost, offer}}. An offer
 * is {id, status, expires_at "YYYY-MM-DD HH:MM:SS" (UTC), price_type, price {virtual {amount,
 * currency}}, original_price {...}, discount_pct, contents [{type: "skin", ref}]}; the client shows
 * it in the menu's notification center (once per offer id), in the shop front and its item view,
 * buys it, and refreshes it for refresh_cost KR. KrunkNative's client talks to kn_brain only, so
 * these are social socket frames:
 *   ["wo"]          -> ["wo", offer | null]
 *   ["wob", id]     -> ["wob", {success, rewards [{type: "skin", skin_id}], funds}] | ["wob", null, err]
 *   ["wor", id]     -> ["wor", {success, refresh_cost, offer, funds}] | ["wor", null, err]
 * KrunkNative's offers (v10's server is not public): one per account and UTC day, expiring at the
 * day's end, virtual (KR) only (KN has no payments). The item is a deterministic pick (account, day,
 * refreshes that day) from the Heroic wheel's Epic / Legendary / Relic / Contraband pools (weights
 * 60 / 30 / 9 / 1), at a rarity price (1500 / 4000 / 10000 / 25000 KR) cut by 25-60 % in 5 % steps.
 * No table of its own: the ledgers keep the state (kr_transactions "wildoffer-refresh" rows count
 * the day's refreshes; skin_transactions "wildoffer" rows with the offer id mark it bought, and a
 * bought offer is gone until the next day or refresh). Owner: svc-accounts.
 */
#include "acct_internal.h"
#include "db_procs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <store_spins.h>
#include "knsvc/config.h"
#include "knsvc/json.h"
#include "knsvc/log.h"
#include "knsvc/loop.h"

#define MOD "wildoffer"
#define DAY_MS 86400000LL
#define P(i) (n > (i) ? p[i] : NULL)
#define PI(i) pint(P(i))

static int64_t pint(const char *s) { return s ? strtoll(s, NULL, 10) : 0; }

static const char *const k_pools[] = {"s10PoolEpic", "s10PoolLegendary", "s10PoolRelic", "s10PoolContraband"};
static const int k_weights[] = {60, 30, 9, 1};
static const int k_rarity[] = {2, 3, 4, 5};
static const int k_original[] = {1500, 4000, 10000, 25000};

static int refresh_cost(void) { return g_acct_brain ? (int) kn_config_int(g_acct_brain->cfg, "accounts.wild_offer_refresh_cost", 300) : 300; }

static uint64_t mix(uint64_t h) {
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return h;
}

typedef struct {
    int64_t id;      // day * 100 + refreshes
    int skin, rarity, original, price, discount;
    int64_t expires_ms;
} Offer;

/* the day's offer number `r` of account `account` (0 when there is no pool data) */
static int offer_make(const int64_t account, const int64_t now_ms, const int64_t r, Offer *o) {
    const int64_t day = now_ms / DAY_MS;
    uint64_t h = mix((uint64_t) account * 0x9e3779b97f4a7c15ULL ^ mix((uint64_t) day * 1000003ULL + (uint64_t) r));
    const StoreSpins *sp = store_spins_builtin();
    int total = 0;
    for (int i = 0; i < 4; i++) total += k_weights[i];
    int roll = (int) (h % (uint64_t) total), k = 0;
    while (k < 3 && roll >= k_weights[k]) roll -= k_weights[k++];
    for (int tries = 0; tries < 4; tries++, k = (k + 3) % 4) { // a pool missing from the data: the next lower one
        const StorePool *pool = store_spins_pool(sp, k_pools[k]);
        if (!pool || pool->count <= 0) continue;
        h = mix(h);
        o->id = day * 100 + r;
        o->skin = pool->items[h % (uint64_t) pool->count];
        o->rarity = k_rarity[k];
        o->original = k_original[k];
        o->discount = 25 + 5 * (int) (mix(h) % 8);
        o->price = (o->original * (100 - o->discount) + 50) / 100;
        o->expires_ms = (day + 1) * DAY_MS;
        return 1;
    }
    return 0;
}

static cJSON *price_json(const int amount) {
    cJSON *p = cJSON_CreateObject(), *v = cJSON_AddObjectToObject(p, "virtual");
    cJSON_AddNumberToObject(v, "amount", amount);
    cJSON_AddStringToObject(v, "currency", "KR");
    return p;
}

static cJSON *offer_json(const Offer *o) {
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "id", (double) o->id);
    cJSON_AddNullToObject(j, "status");
    char at[32];
    const time_t t = (time_t) (o->expires_ms / 1000);
    struct tm tmv;
    gmtime_r(&t, &tmv);
    strftime(at, sizeof(at), "%Y-%m-%d %H:%M:%S", &tmv);
    cJSON_AddStringToObject(j, "expires_at", at);
    cJSON_AddStringToObject(j, "price_type", "virtual");
    cJSON_AddItemToObject(j, "price", price_json(o->price));
    cJSON_AddItemToObject(j, "original_price", price_json(o->original));
    cJSON_AddNumberToObject(j, "discount_pct", o->discount);
    cJSON *contents = cJSON_AddArrayToObject(j, "contents"), *c = cJSON_CreateObject();
    cJSON_AddStringToObject(c, "type", "skin");
    char ref[16];
    snprintf(ref, sizeof(ref), "%d", o->skin);
    cJSON_AddStringToObject(c, "ref", ref);
    cJSON_AddItemToArray(contents, c);
    cJSON_AddNumberToObject(j, "refresh_cost", refresh_cost());
    return j;
}

static int64_t refreshes(Dbx *x, const char *id, const char *since) {
    return dbx_int(x, SQL(.any = "SELECT count(*) FROM kr_transactions WHERE account_id = $1 AND source = 'wildoffer-refresh'"
                                 " AND {epoch_ms:at} >= {int:$2}"),
                   KN_DB_ARGS(id, since), 0);
}

static int bought(Dbx *x, const char *id, const int64_t offer) {
    char ref[24];
    snprintf(ref, sizeof(ref), "%lld", (long long) offer);
    return dbx_exists(x, SQL(.any = "SELECT 1 FROM skin_transactions WHERE account_id = $1 AND source = 'wildoffer' AND ref = $2"),
                      KN_DB_ARGS(id, ref));
}

/* the account's current offer; 0 when it has none (bought, or no pool data) */
static int current(Dbx *x, const char *id, const int64_t now, Offer *o) {
    char since[24];
    acct_itoa(since, now - now % DAY_MS);
    if (!offer_make(pint(id), now, refreshes(x, id, since), o)) return 0;
    return !bought(x, id, o->id);
}

/* (id, now_ms) -> {offer} */
static kn_db_result *run_get(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    Offer o;
    cJSON *v = cJSON_CreateObject();
    if (current(&x, P(0), PI(1), &o)) cJSON_AddItemToObject(v, "offer", offer_json(&o));
    else cJSON_AddNullToObject(v, "offer");
    return dbx_json(&x, "wild_offer", v);
}
const kn_proc P_WILD_OFFER = {"wild_offer", NULL, run_get}; /* v10_catalog.c runs it over HTTP too */

/* (id, now_ms, offer_id) -> {success, rewards, funds}: the current offer only, once */
static kn_db_result *run_buy(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    dbx_exec(&x, SQL(.any = "UPDATE players SET player_funds = player_funds WHERE player_id = $1"), KN_DB_ARGS(P(0)));
    Offer o;
    if (!current(&x, P(0), PI(1), &o) || o.id != PI(2)) {
        dbx_refuse(&x, "This wild offer has expired");
        return dbx_finish(&x, NULL);
    }
    char ref[24];
    snprintf(ref, sizeof(ref), "%lld", (long long) o.id);
    int64_t funds = 0;
    if (db_add_funds(&x, PI(0), -o.price, "wildoffer", ref, &funds) < 0) return dbx_finish(&x, NULL);
    if (db_add_skin(&x, PI(0), o.skin, 1, "wildoffer", ref, NULL) < 0) return dbx_finish(&x, NULL);
    cJSON *v = cJSON_CreateObject();
    cJSON_AddBoolToObject(v, "success", 1);
    cJSON *rewards = cJSON_AddArrayToObject(v, "rewards"), *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "type", "skin");
    cJSON_AddNumberToObject(r, "skin_id", o.skin);
    cJSON_AddItemToArray(rewards, r);
    cJSON_AddNumberToObject(v, "price", o.price);
    cJSON_AddNumberToObject(v, "funds", (double) funds);
    return dbx_json(&x, "wild_offer_buy", v);
}
const kn_proc P_WILD_OFFER_BUY = {"wild_offer_buy", NULL, run_buy};

/* (id, now_ms, offer_id, cost) -> {success, refresh_cost, offer, funds} */
static kn_db_result *run_refresh(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    dbx_exec(&x, SQL(.any = "UPDATE players SET player_funds = player_funds WHERE player_id = $1"), KN_DB_ARGS(P(0)));
    Offer o;
    if (!current(&x, P(0), PI(1), &o) || o.id != PI(2)) {
        dbx_refuse(&x, "This wild offer has expired");
        return dbx_finish(&x, NULL);
    }
    char ref[24];
    snprintf(ref, sizeof(ref), "%lld", (long long) o.id);
    int64_t funds = 0;
    if (db_add_funds(&x, PI(0), -PI(3), "wildoffer-refresh", ref, &funds) < 0) return dbx_finish(&x, NULL);
    Offer next;
    if (!offer_make(PI(0), PI(1), o.id % 100 + 1, &next)) {
        dbx_refuse(&x, "No wild offer available");
        return dbx_finish(&x, NULL);
    }
    cJSON *v = cJSON_CreateObject();
    cJSON_AddBoolToObject(v, "success", 1);
    cJSON_AddNumberToObject(v, "refresh_cost", (double) PI(3));
    cJSON_AddItemToObject(v, "offer", offer_json(&next));
    cJSON_AddNumberToObject(v, "funds", (double) funds);
    return dbx_json(&x, "wild_offer_refresh", v);
}
const kn_proc P_WILD_OFFER_REFRESH = {"wild_offer_refresh", NULL, run_refresh};

static void get_done(SocialSession *s, kn_db_result *res, void *extra) {
    (void) extra;
    cJSON *v = kn_db_ok(res) ? acct_db_json(res) : NULL;
    if (!v) {
        KN_WARN(MOD, "get: %s", acct_db_error(res, "wild offer"));
        return;
    }
    cJSON *offer = cJSON_DetachItemFromObject(v, "offer");
    cJSON_Delete(v);
    social_send_n(s, "wo", 1, offer ? offer : cJSON_CreateNull());
}

/* ["wo"]: shop.getWildOffer */
void sh_wild_offer(SocialSession *s, const cJSON *f) {
    (void) f;
    if (s->account_id <= 0) return;
    char id[24], now[24];
    acct_itoa(id, s->account_id);
    acct_itoa(now, kn_wall_ms());
    social_call(s, &P_WILD_OFFER, KN_DB_ARGS(id, now), get_done, NULL);
}

static void reply_done(SocialSession *s, kn_db_result *res, void *extra) {
    const char *label = extra ? extra : "wob";
    cJSON *v = kn_db_ok(res) ? acct_db_json(res) : NULL;
    if (!v) {
        social_send_n(s, label, 2, cJSON_CreateNull(), cJSON_CreateString(acct_db_error(res, "wild offer")));
        return;
    }
    social_send_n(s, label, 1, v);
}

static void call_with_offer(SocialSession *s, const cJSON *f, const kn_proc *proc, const char *label) {
    if (!social_require_auth(s)) return;
    const cJSON *offer = cJSON_GetArrayItem(f, 1);
    if (!cJSON_IsNumber(offer)) {
        social_send_n(s, label, 2, cJSON_CreateNull(), cJSON_CreateString("Invalid wild offer"));
        return;
    }
    char id[24], now[24], oid[24], cost[24];
    acct_itoa(id, s->account_id);
    acct_itoa(now, kn_wall_ms());
    acct_itoa(oid, (int64_t) offer->valuedouble);
    acct_itoa(cost, refresh_cost());
    social_call(s, proc, KN_DB_ARGS(id, now, oid, cost), reply_done, strdup(label)); /* extra: freed by social_db */
}

/* ["wob", offer_id]: shop.purchaseWildOffer */
void sh_wild_offer_buy(SocialSession *s, const cJSON *f) { call_with_offer(s, f, &P_WILD_OFFER_BUY, "wob"); }

/* ["wor", offer_id]: shop.refreshWildOffer */
void sh_wild_offer_refresh(SocialSession *s, const cJSON *f) { call_with_offer(s, f, &P_WILD_OFFER_REFRESH, "wor"); }

/* the refresh price in KR (v10_catalog.c passes it to P_WILD_OFFER_REFRESH, as the social frame does) */
int wild_offer_refresh_cost(void) { return refresh_cost(); }
