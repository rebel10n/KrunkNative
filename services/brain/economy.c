/*
 * services/brain/economy.c - the economy frames of the social socket: KR gifts ("kr"), the
 * market ("r" market pages, "li", "uli", "pi", "qsi", "fil", "srt", "st", "uml"), trade offers
 * ("ri", "ctr", "trA", "trC", "trCA") and the KrunkNative additions "ecfg" (the economy rules the
 * server applies) and "krh" (the account's KR ledger). Krunker v7.1.4 src/social.js is the
 * client side; the rules and the protocol are in doc/v7/economy.md.
 *
 * The handlers validate what needs the store data (skins.json: tradable items, rarity values and
 * fees) and the session, then run one kn_proc of db_economy.c, which does every check that
 * involves balances or inventories inside one database transaction with row locks.
 *
 * Idempotency: a KrunkNative client appends {"op": "<key>"} to every economy action frame (a
 * retry of the same action reuses the key); frames without one (v7.1.4 clients) get a fresh
 * server key, so they are still rate limited and recorded but not deduplicated.
 */
#include "acct_internal.h"
#include "v10_paint.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "knsvc/config.h"
#include "knsvc/crypto.h"
#include "knsvc/json.h"
#include "knsvc/log.h"

#define MOD "economy"

/* store.rarities[] of v7.1.4 src/data/store.js: quick sell KR and trade fee per item */
static const int k_rarity_sell[] = {1, 3, 10, 100, 500, 2500, 10000, 10};
static const int k_rarity_trade_fee[] = {5, 5, 5, 10, 10, 25, 200, 500};
#define RARITIES ((int) (sizeof k_rarity_sell / sizeof k_rarity_sell[0]))

static kn_timer_id s_timer;

/* ------------------------------------------------------------------ helpers */

typedef struct Ctx {
    char label[16];
    int64_t a, b;
    char text[64];
} Ctx;

static Ctx *ctx_new(const char *label, int64_t a, int64_t b) {
    Ctx *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    snprintf(c->label, sizeof c->label, "%s", label ? label : "");
    c->a = a;
    c->b = b;
    return c;
}

static const char *me_str(const SocialSession *s, char buf[24]) { return social_viewer(s, buf); }

/* Tradable / listable: v10's item table carries the server's `tradable` bit for every item (the
 * v7.1.4 rule, not free / noSale / blackM / opensea / itemShop, except 20 opensea items such as
 * Gold Plated that v10 lets players trade). A skins file without the field (v7.1.4's) keeps the rule. */
static int skins_have_tradable(void) {
    static int known = -1;
    if (known < 0) {
        known = 0;
        for (int i = 0; i < acct_skin_count() && !known; i++) {
            const SkinInfo *k = acct_skin(i);
            known = k && k->tradable;
        }
    }
    return known;
}

static int skin_tradable(const SkinInfo *k) {
    if (!k) return 0;
    if (skins_have_tradable()) return k->tradable != 0;
    return !k->free_item && !k->no_sale && !k->black_m && !k->opensea && !k->item_shop;
}

/* Why an item cannot be listed (confListPopup strings), NULL when it can. */
static const char *skin_unlistable(const SkinInfo *k) {
    if (!k) return "Item not found";
    if (k->opensea && !skin_tradable(k)) return "Item cant be sold on krunker.";
    if (!skin_tradable(k)) return "Item cant be sold.";
    return NULL;
}

/* The KrunkNative extension object: the last element of the frame when it is an object. */
static const cJSON *frame_ext(const cJSON *f) {
    const int n = cJSON_GetArraySize(f);
    const cJSON *last = n > 1 ? cJSON_GetArrayItem(f, n - 1) : NULL;
    return cJSON_IsObject(last) ? last : NULL;
}

/* Number of frame arguments without the extension object. */
static int frame_args(const cJSON *f) { return cJSON_GetArraySize(f) - 1 - (frame_ext(f) ? 1 : 0); }

/* The action's idempotency key: the client's {"op"} (1-64 chars of [A-Za-z0-9_.:-]) or a fresh one. */
static const char *econ_key(const cJSON *f, char out[72]) {
    const char *op = kn_json_str(frame_ext(f), "op", NULL);
    if (op && *op && strlen(op) <= 64 && strspn(op, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.:-") == strlen(op)) {
        snprintf(out, 72, "c:%.62s", op);
        return out;
    }
    char hex[40];
    if (kn_random_hex(hex, sizeof hex, 12) != 0) snprintf(hex, sizeof hex, "%016llx", (unsigned long long) kn_random_u64());
    snprintf(out, 72, "s:%s", hex);
    return out;
}

static int rate_of(const char *kind) {
    const AcctConfig *c = &g_acct;
    if (!strcmp(kind, "gift")) return c->rate_gift;
    if (!strcmp(kind, "list")) return c->rate_list;
    if (!strcmp(kind, "unlist")) return c->rate_unlist;
    if (!strcmp(kind, "buy")) return c->rate_buy;
    if (!strcmp(kind, "qsell")) return c->rate_qsell;
    if (!strcmp(kind, "trade")) return c->rate_trade;
    if (!strcmp(kind, "accept")) return c->rate_accept;
    return c->rate_close;
}

/* The common leading parameters of an economy proc ($0 actor, $1 key, $2 rate, $3 feature). */
typedef struct EconHead {
    char me[24], key[72], rate[24];
    const char *feature;
} EconHead;

static void econ_head(EconHead *h, const SocialSession *s, const cJSON *f, const char *kind, const char *feature) {
    me_str(s, h->me);
    econ_key(f, h->key);
    acct_itoa(h->rate, rate_of(kind));
    h->feature = feature;
}

/* ["ua"] to every JSON socket of the account (header KR after a gift / sale / trade). */
static void push_account_cb(void *ud, const char *err, const cJSON *data) {
    (void) ud;
    if (!err && data) social_push_account_update(data);
}

static void push_funds(int64_t account_id) {
    if (account_id > 0) acct_get(account_id, push_account_cb, NULL);
}

/* ["econN", kind, text] to the account's open sockets (KrunkNative): the hub shows it and refreshes
 * its trade list, the menu / game a toast. Not "pRes", which the menu answers with a modal popup. */
static void push_note(int64_t account_id, const char *kind, const char *msg) {
    if (account_id <= 0) return;
    cJSON *push = social_frame("econN");
    cJSON_AddItemToArray(push, cJSON_CreateString(kind));
    cJSON_AddItemToArray(push, cJSON_CreateString(msg));
    social_push_account_ex(account_id, push, 1);
}

static void ignore_done(kn_db_result *res, void *ud) {
    (void) ud;
    if (!kn_db_ok(res)) KN_WARN(MOD, "notification: %s", kn_db_error(res));
}

/* ------------------------------------------------------------------ rules ("ecfg") */

/* ["ecfg"] -> ["ecfg", {...}]: the rules this server applies, so the client's fee and limit
 * displays (calcFee, calcGiftFee, the trade fee, level gates) match what it is charged. */
void sh_economy_config(SocialSession *s, const cJSON *f) {
    (void) f;
    const AcctConfig *c = &g_acct;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "giftFee", c->gift_fee_percent);
    cJSON_AddNumberToObject(o, "giftMin", (double) c->gift_min);
    cJSON_AddNumberToObject(o, "giftMax", (double) c->gift_max);
    cJSON_AddNumberToObject(o, "giftMinLvl", c->gift_min_level);
    cJSON_AddNumberToObject(o, "marketFee", c->market_fee_percent);
    cJSON_AddNumberToObject(o, "marketFeeBypass", (double) c->market_fee_bypass);
    cJSON_AddNumberToObject(o, "marketMinLvl", c->market_min_level);
    cJSON_AddNumberToObject(o, "saleMax", (double) c->sale_max);
    cJSON_AddNumberToObject(o, "saleGrace", (double) c->sale_grace_s * 1000.0);
    cJSON_AddNumberToObject(o, "maxListings", c->max_listings);
    cJSON_AddNumberToObject(o, "tradeMinLvl", c->trade_min_level);
    cJSON_AddNumberToObject(o, "tradeMaxOut", c->trade_max_items);
    cJSON_AddNumberToObject(o, "tradeMaxOutP", c->trade_max_items_premium);
    cJSON_AddNumberToObject(o, "tradeKRMax", (double) c->trade_kr_max);
    cJSON_AddNumberToObject(o, "tradeTTL", (double) c->trade_ttl_s * 1000.0);
    social_send_n(s, "ecfg", 1, o);
}

/* ------------------------------------------------------------------ KR gift */

static void gift_done(SocialSession *s, kn_db_result *res, void *extra) {
    (void) extra;
    cJSON *v = kn_db_ok(res) ? acct_db_json(res) : NULL;
    if (!v || !cJSON_IsObject(v)) {
        cJSON_Delete(v);
        social_error(s, kn_db_ok(res) ? "Failed. Try Again" : acct_db_error(res, "kr"));
        return;
    }
    /* io_gir({funds}); the receiver gets the "KR Gift" mail ("gmc") and its header KR ("ua") */
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "funds", (double) kn_json_int(v, "funds", 0));
    cJSON_AddNumberToObject(r, "amount", (double) kn_json_int(v, "amount", 0));
    cJSON_AddNumberToObject(r, "fee", (double) kn_json_int(v, "fee", 0));
    social_send_n(s, "gir", 1, r);
    if (kn_json_int(v, "replay", 0)) { /* a retried frame: the receiver was told the first time */
        cJSON_Delete(v);
        return;
    }
    const int64_t to = kn_json_int(v, "to", 0);
    char note[96];
    snprintf(note, sizeof note, "%s sent you %lld KR", s->username, (long long) kn_json_int(v, "amount", 0));
    push_note(to, "gift", note);
    social_push_mail_count(to);
    push_funds(to);
    push_funds(s->account_id);
    cJSON_Delete(v);
}

/* ["kr", name, amount, message, captcha?, {op}] -> ["gir", {funds, amount, fee}] | po error */
void sh_gift(SocialSession *s, const cJSON *f) {
    if (!social_require_auth(s)) return;
    const char *name = fa_str(f, 1);
    const cJSON *amt_v = fa(f, 2);
    const double amt_d = cJSON_IsNumber(amt_v) ? amt_v->valuedouble : cJSON_IsString(amt_v) ? strtod(amt_v->valuestring, NULL) : 0;
    /* range-checked as a double first: (int64_t) of a huge or NaN double is undefined */
    const int64_t amount = amt_d >= 1 && amt_d <= (double) g_acct.gift_max ? (int64_t) amt_d : 0;
    if (!name || !*name || strlen(name) > 64 || amount <= 0 || (double) amount != amt_d) {
        social_error(s, "Invalid amount");
        return;
    }
    EconHead h;
    econ_head(&h, s, f, "gift", "gift");
    char amt[24], fee[24], lvl[24], mn[24], mx[24];
    const char *msg = fa_str(f, 3);
    social_call(s, &P_GIFT,
                KN_DB_ARGS(h.me, h.key, h.rate, h.feature, name, acct_itoa(amt, amount), acct_itoa(fee, g_acct.gift_fee_percent),
                           acct_itoa(lvl, g_acct.gift_min_level), msg ? msg : "", acct_itoa(mn, g_acct.gift_min),
                           acct_itoa(mx, g_acct.gift_max)),
                gift_done, NULL);
}

/* ------------------------------------------------------------------ market pages */

typedef struct MarketCtx {
    char page[16];
    char sub[24];
    int64_t item;     /* item filter (-1 none) */
    int64_t token;    /* fil/srt request counter echo */
    int kind;         /* 0 page, 1 fil, 2 srt */
    unsigned rar_mask, wep_mask, type_mask; /* rarities / weapons (index + 1) / types to keep, 0 = all */
    int season;       /* 0 = all */
    int desc;         /* fil order "0": highest price first */
    char text[64];
} MarketCtx;

static int item_matches(const MarketCtx *c, int64_t skin_id) {
    if (c->item >= 0 && skin_id != c->item) return 0;
    if (!c->rar_mask && !c->wep_mask && !c->type_mask && !c->season && !c->text[0]) return 1;
    const SkinInfo *k = acct_skin(skin_id);
    if (!k) return 0;
    if (c->rar_mask && !(c->rar_mask & (1u << (k->rarity & 31)))) return 0;
    if (c->wep_mask && !(c->wep_mask & (1u << (k->weapon & 31)))) return 0;
    if (c->type_mask && !(c->type_mask & (1u << (k->type & 31)))) return 0;
    if (c->season && k->seas != c->season) return 0;
    if (c->text[0]) {
        char a[64], b[64];
        size_t i;
        for (i = 0; k->name[i] && i < sizeof a - 1; i++) a[i] = (char) (k->name[i] | 0x20);
        a[i] = 0;
        for (i = 0; c->text[i] && i < sizeof b - 1; i++) b[i] = (char) (c->text[i] | 0x20);
        b[i] = 0;
        if (!strstr(a, b)) return 0;
    }
    return 1;
}

static int64_t quick_sell_each(const SkinInfo *k);

/* KrunkNative (the v10 inventory cards, v10_market.c): what the client's store table does not say
 * about an owned item, from the server's own skins file: nt = cannot be listed / traded, qs = KR
 * per quick-sold unit (0: none), nqs = noQuickSell (destroy only). */
static void decorate_inventory(cJSON *rows) {
    cJSON *it;
    cJSON_ArrayForEach(it, rows) {
        const SkinInfo *k = acct_skin(kn_json_int(it, "i", -1));
        if (!k) continue;
        if (!skin_tradable(k)) cJSON_AddNumberToObject(it, "nt", 1);
        cJSON_AddNumberToObject(it, "qs", (double) quick_sell_each(k));
        if (k->no_quick_sell) cJSON_AddNumberToObject(it, "nqs", 1);
    }
}

static void market_done(SocialSession *s, kn_db_result *res, void *extra) {
    MarketCtx *c = extra;
    cJSON *data = NULL;
    if (!kn_db_ok(res)) {
        acct_db_error(res, "market");
    } else if (!strcmp(c->sub, "trades") || !strcmp(c->page, "itemsales") || !strcmp(c->page, "listing")) {
        data = acct_db_json(res);
    } else {
        cJSON *rows = kn_db_result_to_json(res);
        data = cJSON_CreateArray();
        const int n = cJSON_GetArraySize(rows);
        for (int k = 0; k < n; k++) {
            const cJSON *it = cJSON_GetArrayItem(rows, c->desc ? n - 1 - k : k);
            if (item_matches(c, kn_json_int(it, "i", kn_json_int(it, "ski", -1))))
                cJSON_AddItemToArray(data, cJSON_Duplicate(it, 1));
        }
        cJSON_Delete(rows);
        if (!strcmp(c->sub, "inventory")) decorate_inventory(data);
    }
    if (c->kind == 1) { /* io_fr(Ne, list|null) */
        if (data && !cJSON_GetArraySize(data)) {
            cJSON_Delete(data);
            data = NULL;
        }
        social_send_n(s, "fr", 2, cJSON_CreateNumber((double) c->token), data ? data : cJSON_CreateNull());
        return;
    }
    if (c->kind == 2) { /* io_str(We, list) */
        social_send_n(s, "str", 2, cJSON_CreateNumber((double) c->token), data ? data : cJSON_CreateNull());
        return;
    }
    cJSON *fr = social_frame("0");
    cJSON_AddItemToArray(fr, cJSON_CreateString(c->page));
    cJSON_AddItemToArray(fr, cJSON_CreateString(c->sub));
    if (!strcmp(c->page, "itemsales")) {
        /* io_0 "itemsales": t = {ind, onS, inC, high, low}, r = owners [{nm, ct, su, ft}] */
        cJSON *owners = data ? cJSON_DetachItemFromObject(data, "owners") : NULL;
        cJSON_AddItemToArray(fr, data ? data : cJSON_CreateNull());
        cJSON_AddItemToArray(fr, owners ? owners : cJSON_CreateArray());
    } else {
        cJSON_AddItemToArray(fr, data ? data : cJSON_CreateNull());
    }
    social_send(s, fr);
}

#define LISTING_ROWS \
    "SELECT l.skin_id AS i, l.skin_id AS s, l.price AS f, l.id AS m, {epoch_ms:l.created_at} AS ss, {epoch_ms:l.created_at} AS ms," \
    " a.username AS p, 1 AS c, l.paint_color AS paint_color, l.paint_shader AS paint_shader" \
    " FROM market_listings l JOIN accounts a ON a.id = l.seller_id "

/* A refused query (the db will not take it) gets its reply anyway: the page's own reply with no data, so the
 * client's wait ends (social_db frees the query's context on a refusal, so the caller passes a copy) */
static void market_refused(SocialSession *s, const MarketCtx *c) {
    if (c->kind == 1) social_send_n(s, "fr", 2, cJSON_CreateNumber((double) c->token), cJSON_CreateNull());
    else if (c->kind == 2) social_send_n(s, "str", 2, cJSON_CreateNumber((double) c->token), cJSON_CreateNull());
    else social_send_n(s, "0", 3, cJSON_CreateString(c->page), cJSON_CreateString(c->sub), cJSON_CreateNull());
}

static void market_query(SocialSession *s, MarketCtx *c) {
    char me[24];
    me_str(s, me);
    const MarketCtx copy = *c; /* social_db frees c when the submit is refused */
    int rc = 0;
    if (!strcmp(c->page, "itemsales")) {
        char item[24];
        rc = social_call(s, &P_MARKET_ITEM, KN_DB_ARGS(acct_itoa(item, c->item >= 0 ? c->item : 0)), market_done, c);
    } else if (!strcmp(c->page, "listing")) {
        char id[24];
        rc = social_call(s, &P_LISTING, KN_DB_ARGS(acct_itoa(id, c->item)), market_done, c);
    } else if (!strcmp(c->sub, "inventory")) {
        /* the stacks (their unpainted units; sid = the store index) and the painted copies (one unit each, sid =
         * the copy id, paint_color / paint_shader set: doc/v10/paint-copy.md). A stack row has no paint. */
        rc = social_query(s, SQL(.any = "SELECT i.skin_id AS i, i.skin_id AS s, i.count - (SELECT count(*) FROM skin_copies c"
                                   " WHERE c.account_id = i.account_id AND c.skin_id = i.skin_id) AS c, i.skin_id AS sid,"
                                   " NULL AS paint_color, NULL AS paint_shader FROM inventory i"
                                   " WHERE i.account_id = $1 AND i.count > (SELECT count(*) FROM skin_copies c"
                                   " WHERE c.account_id = i.account_id AND c.skin_id = i.skin_id)"
                                   " UNION ALL SELECT k.skin_id AS i, k.skin_id AS s, 1 AS c, k.copy_id AS sid,"
                                   " k.paint_color, k.paint_shader FROM skin_copies k WHERE k.account_id = $1"
                                   " ORDER BY i, sid"),
                     KN_DB_ARGS(me), market_done, c);
    } else if (!strcmp(c->sub, "sales")) {
        rc = social_query(s, SQL(.any = LISTING_ROWS "WHERE l.seller_id = $1 AND l.sold_at IS NULL AND l.cancelled_at IS NULL ORDER BY l.id DESC"),
                          KN_DB_ARGS(me), market_done, c);
    } else if (!strcmp(c->sub, "stats")) {
        /* + the v10 Market Stats cards' On Sale / Low / High (onS, low, high: open listings) */
        rc = social_query(s, SQL(.pg = "SELECT skin_id AS ski, sum(count)::bigint AS cnt, sum(count)::bigint AS c, sum(count)::bigint AS \"inC\","
                                  " (SELECT count(*) FROM market_listings l WHERE l.skin_id = i.skin_id AND l.sold_at IS NULL AND l.cancelled_at IS NULL) AS \"onS\","
                                  " (SELECT min(price) FROM market_listings l WHERE l.skin_id = i.skin_id AND l.sold_at IS NULL AND l.cancelled_at IS NULL) AS low,"
                                  " (SELECT max(price) FROM market_listings l WHERE l.skin_id = i.skin_id AND l.sold_at IS NULL AND l.cancelled_at IS NULL) AS high"
                                  " FROM inventory i WHERE count > 0 GROUP BY skin_id ORDER BY 2 ASC, skin_id LIMIT 50",
                            .any = "SELECT skin_id AS ski, sum(count) AS cnt__i, sum(count) AS c__i, sum(count) AS inC__i,"
                                   " (SELECT count(*) FROM market_listings l WHERE l.skin_id = i.skin_id AND l.sold_at IS NULL AND l.cancelled_at IS NULL) AS onS__i,"
                                   " (SELECT min(price) FROM market_listings l WHERE l.skin_id = i.skin_id AND l.sold_at IS NULL AND l.cancelled_at IS NULL) AS low__i,"
                                   " (SELECT max(price) FROM market_listings l WHERE l.skin_id = i.skin_id AND l.sold_at IS NULL AND l.cancelled_at IS NULL) AS high__i"
                                   " FROM inventory i WHERE count > 0 GROUP BY skin_id ORDER BY 2 ASC, skin_id LIMIT 50"),
                     0, NULL, market_done, c);
    } else if (!strcmp(c->sub, "prices")) {
        /* KrunkNative (v10's getPrices / items/prices, the cards' "~ N KR" estimate badge): each item's
         * estimate from this economy's own completed market sales, the rounded mean price (v10's comes
         * from Krunker's market; an item never sold here has none: the client falls back to its bundled
         * snapshot, then v10's "Unknown") */
        rc = social_query(s, SQL(.pg = "SELECT skin_id AS ski, round(avg(price))::bigint AS est FROM market_listings"
                                  " WHERE sold_at IS NOT NULL GROUP BY skin_id ORDER BY skin_id",
                            .sqlite = "SELECT skin_id AS ski, CAST(round(avg(price)) AS INTEGER) AS est FROM market_listings"
                                      " WHERE sold_at IS NOT NULL GROUP BY skin_id ORDER BY skin_id",
                            .mysql = "SELECT skin_id AS ski, CAST(ROUND(AVG(price)) AS SIGNED) AS est FROM market_listings"
                                     " WHERE sold_at IS NOT NULL GROUP BY skin_id ORDER BY skin_id"),
                     0, NULL, market_done, c);
    } else if (!strcmp(c->sub, "history")) {
        /* KrunkNative (the v10 Market History tab): the account's closed market trades, newest first:
         * {type buy|sell, date, skinindex, amount, who, status completed|cancelled} */
        rc = social_query(s, SQL(.any = "SELECT CASE WHEN l.buyer_id = $1 THEN 'buy' ELSE 'sell' END AS type,"
                                   " {epoch_ms:coalesce(l.sold_at, l.cancelled_at)} AS date, l.skin_id AS skinindex, l.price AS amount,"
                                   " (SELECT username FROM accounts WHERE id = CASE WHEN l.buyer_id = $1 THEN l.seller_id ELSE l.buyer_id END) AS who,"
                                   " CASE WHEN l.sold_at IS NOT NULL THEN 'completed' ELSE 'cancelled' END AS status"
                                   " FROM market_listings l WHERE l.buyer_id = $1 OR (l.seller_id = $1 AND (l.sold_at IS NOT NULL OR l.cancelled_at IS NOT NULL))"
                                   " ORDER BY coalesce(l.sold_at, l.cancelled_at) DESC, l.id DESC LIMIT 500"),
                     KN_DB_ARGS(me), market_done, c);
    } else if (!strcmp(c->sub, "trades")) {
        rc = social_call(s, &P_TRADE_PAGE, KN_DB_ARGS(me), market_done, c);
    } else { /* market: cheapest open listing first */
        rc = social_query(s, SQL(.any = LISTING_ROWS "WHERE l.sold_at IS NULL AND l.cancelled_at IS NULL ORDER BY l.price ASC, l.id LIMIT 1000"),
                          0, NULL, market_done, c);
    }
    if (rc < 0) market_refused(s, &copy);
}



/* ["r", "market" | "itemsales" | "listing", sub, id, name, s, region, item, captcha] */
void market_page(SocialSession *s, const char *page, const cJSON *f) {
    MarketCtx *c = calloc(1, sizeof *c);
    if (!c) return;
    snprintf(c->page, sizeof c->page, "%s", page);
    const char *sub = fa_str(f, 2);
    snprintf(c->sub, sizeof c->sub, "%s", sub ? sub : "market");
    c->item = fa_int(f, 7, -1);
    if (!strcmp(page, "listing")) {
        /* ?p=listing&i=<marketId>: the id comes as q (sub) or i */
        if (c->item < 0 && sub) c->item = strtoll(sub, NULL, 10);
        if (c->item <= 0) {
            social_send_n(s, "0", 3, cJSON_CreateString(page), cJSON_CreateString(c->sub), cJSON_CreateNull());
            free(c);
            return;
        }
    } else if (!strcmp(c->sub, "blackm")) {
        /* rendered from the client's own store data: any truthy t */
        social_send_n(s, "0", 3, cJSON_CreateString(page), cJSON_CreateString(c->sub), cJSON_CreateArray());
        free(c);
        return;
    }
    if (s->account_id <= 0 && (!strcmp(c->sub, "inventory") || !strcmp(c->sub, "sales") || !strcmp(c->sub, "trades") || !strcmp(c->sub, "history"))) {
        social_send_n(s, "0", 3, cJSON_CreateString(page), cJSON_CreateString(c->sub), cJSON_CreateNull());
        free(c);
        return;
    }
    market_query(s, c);
}

/* {"3": 1, "5": true} or [0, 1, 0, 1] -> bit mask of the truthy keys / positions */
static unsigned key_mask(const cJSON *obj) {
    unsigned mask = 0;
    const cJSON *it;
    if (cJSON_IsObject(obj)) {
        cJSON_ArrayForEach(it, obj) {
            const int r = it->string ? atoi(it->string) : -1;
            if (r >= 0 && r < 32 && (cJSON_IsTrue(it) || (cJSON_IsNumber(it) && it->valuedouble != 0))) mask |= 1u << r;
        }
    } else if (cJSON_IsArray(obj)) {
        int i = 0;
        cJSON_ArrayForEach(it, obj) {
            if (i < 32 && (cJSON_IsTrue(it) || (cJSON_IsNumber(it) && it->valuedouble != 0))) mask |= 1u << i;
            i++;
        }
    }
    return mask;
}

static void filter_args(MarketCtx *c, const cJSON *f) {
    c->token = fa_int(f, 1, 0);
    snprintf(c->text, sizeof c->text, "%s", fa_str(f, 2) ? fa_str(f, 2) : "");
    c->rar_mask = key_mask(fa(f, 3));
    c->wep_mask = key_mask(fa(f, 4));
    c->type_mask = key_mask(fa(f, 5));
    const cJSON *seas = fa(f, 6);
    c->season = cJSON_IsString(seas) ? atoi(seas->valuestring) : cJSON_IsNumber(seas) ? (int) seas->valuedouble : 0;
}

/* ["fil", Ne, text, rarities, weapons, types, season, order] -> ["fr", Ne, listings|null]
 * (order "0" = highest price first, "1" = lowest, the default) */
void sh_market_filter(SocialSession *s, const cJSON *f) {
    MarketCtx *c = calloc(1, sizeof *c);
    if (!c) return;
    snprintf(c->page, sizeof c->page, "market");
    snprintf(c->sub, sizeof c->sub, "market");
    c->kind = 1;
    c->item = -1;
    filter_args(c, f);
    const cJSON *order = fa(f, 7);
    c->desc = (cJSON_IsString(order) && !strcmp(order->valuestring, "0")) || (cJSON_IsNumber(order) && order->valuedouble == 0);
    market_query(s, c);
}

/* ["srt", We, text, rarities, weapons, types, season, onlyMissing] -> ["str", We, inventory|null] */
void sh_inventory_sort(SocialSession *s, const cJSON *f) {
    if (!social_require_auth(s)) return;
    MarketCtx *c = calloc(1, sizeof *c);
    if (!c) return;
    snprintf(c->page, sizeof c->page, "market");
    snprintf(c->sub, sizeof c->sub, "inventory");
    c->kind = 2;
    c->item = -1;
    filter_args(c, f);
    market_query(s, c);
}

/* v10 REST GET /market/stats (v10_market.c): ["stats", token, text, rarities, weapons, types, season] ->
 * ["0", "market", "stats", rows] with the same filters as the listings (the rows are per skin: ski). */
void sh_market_stats_http(SocialSession *s, const cJSON *f) {
    MarketCtx *c = calloc(1, sizeof *c);
    if (!c) return;
    snprintf(c->page, sizeof c->page, "market");
    snprintf(c->sub, sizeof c->sub, "stats");
    c->item = -1;
    filter_args(c, f);
    market_query(s, c);
}

static void rows_done(SocialSession *s, kn_db_result *res, void *extra) {
    Ctx *c = extra;
    cJSON *rows = kn_db_ok(res) ? kn_db_result_to_json(res) : NULL;
    if (!kn_db_ok(res)) acct_db_error(res, c->label);
    social_send_n(s, c->label, 1, rows ? rows : cJSON_CreateArray());
}

/* ["st", skin, range, captcha] -> ["gd", [{d, f: KR total, t: units}]] (item sales graph; v7.1.4
 * sends an index into config.dataDateRanges, older KrunkNative clients a number of days) */
void sh_item_stats(SocialSession *s, const cJSON *f) {
    static const int ranges[] = {7, 14, 21, 30, 60, 90, 120};
    char sk[24], days[24];
    int64_t d = fa_int(f, 2, 3);
    if (d >= 0 && d < (int64_t) (sizeof ranges / sizeof ranges[0])) d = ranges[d];
    if (d < 1 || d > 366) d = 30;
    social_query(s,
                 SQL(.pg = "SELECT to_char(date_trunc('day', sold_at), 'YYYY-MM-DD') AS d, sum(price)::bigint AS f, count(*) AS t "
                           "FROM market_listings WHERE skin_id = $1 AND sold_at > now() - make_interval(days => $2::int) "
                           "GROUP BY 1 ORDER BY 1",
                     .sqlite = "SELECT strftime('%Y-%m-%d', sold_at) AS d, sum(price) AS f, count(*) AS t "
                               "FROM market_listings WHERE skin_id = $1 AND sold_at > {ago:$2 * 86400} GROUP BY 1 ORDER BY 1",
                     .mysql = "SELECT DATE_FORMAT(sold_at, '%Y-%m-%d') AS d, CAST(SUM(price) AS SIGNED) AS f, count(*) AS t "
                              "FROM market_listings WHERE skin_id = $1 AND sold_at > {ago:$2 * 86400} GROUP BY 1 ORDER BY 1"),
                 KN_DB_ARGS(acct_itoa(sk, fa_int(f, 1, -1)), acct_itoa(days, d)), rows_done, ctx_new("gd", 0, 0));
}

static void listings_done(SocialSession *s, kn_db_result *res, void *extra) {
    (void) extra;
    cJSON *v = kn_db_ok(res) ? acct_db_json(res) : NULL;
    if (!kn_db_ok(res)) acct_db_error(res, "uml");
    social_send_n(s, "ul", 1, v ? v : cJSON_CreateArray());
}

/* ["uml", playerId] -> ["ul", [{sk, skid, kr, c, ms, id, ...}]] (profile "Listings" tab) */
void sh_listings_of(SocialSession *s, const cJSON *f) {
    char id[24];
    social_call(s, &P_LISTINGS_OF, KN_DB_ARGS(acct_itoa(id, fa_int(f, 1, 0))), listings_done, NULL);
}

/* ------------------------------------------------------------------ market actions */

static void list_done(SocialSession *s, kn_db_result *res, void *extra) {
    Ctx *c = extra;
    if (!kn_db_ok(res)) {
        social_error(s, acct_db_error(res, "li"));
        return;
    }
    cJSON *v = acct_db_json(res);
    cJSON_DeleteItemFromObjectCaseSensitive(v, "replay");
    social_send_n(s, "lir", 2, v, cJSON_CreateNumber((double) c->a)); /* io_lir(result, uid) */
    push_funds(s->account_id);
}

/* ["li", skin, skinid, uid, price, {op}] -> ["lir", {cnt, funds, listingId, fee} | {response:"MAX"}, uid] */
void sh_market_list(SocialSession *s, const cJSON *f) {
    if (!social_require_auth(s)) return;
    const int64_t skin = fa_int(f, 1, -1);
    const char *why = skin_unlistable(acct_skin(skin));
    if (why && acct_skin_count() > 0) {
        social_error(s, why);
        return;
    }
    const cJSON *price_v = fa(f, 4);
    if (!cJSON_IsNumber(price_v) || !(price_v->valuedouble >= 1 && price_v->valuedouble <= (double) g_acct.sale_max) ||
        price_v->valuedouble != (double) (int64_t) price_v->valuedouble) {
        social_error(s, "Invalid price");
        return;
    }
    EconHead h;
    econ_head(&h, s, f, "list", "market");
    /* the frame's third value is the unit listed: a painted copy's id, or 0 / the store index for a stack */
    const int64_t copy = fa_int(f, 2, 0) >= V10_COPY_ID_BASE ? fa_int(f, 2, 0) : 0;
    char sk[24], pr[24], fee[24], byp[24], mx[24], ml[24], lvl[24], cp[24];
    social_call(s, &P_MARKET_LIST,
                KN_DB_ARGS(h.me, h.key, h.rate, h.feature, acct_itoa(sk, skin), acct_itoa(pr, (int64_t) price_v->valuedouble),
                           acct_itoa(fee, g_acct.market_fee_percent), acct_itoa(byp, g_acct.market_fee_bypass),
                           acct_itoa(mx, g_acct.sale_max), acct_itoa(ml, g_acct.max_listings),
                           acct_itoa(lvl, g_acct.market_min_level), acct_itoa(cp, copy)),
                list_done, ctx_new("li", fa_int(f, 3, 0), 0));
}

/* ["ip", copy, item, paint, colour, shader, uid] -> ["ipr", {copy_id, item_id, paint_item_id, m, count} | null, uid]
 * (v10.0.3's setPaint; run_item_paint in db_economy.c). The owned list changed: the account is pushed (ua). */
static void paint_done(SocialSession *s, kn_db_result *res, void *extra) {
    Ctx *c = extra;
    if (!kn_db_ok(res)) {
        social_error(s, acct_db_error(res, "ip"));
        return;
    }
    cJSON *v = acct_db_json(res);
    social_send_n(s, "ipr", 2, v ? v : cJSON_CreateNull(), cJSON_CreateNumber((double) c->a));
    push_funds(s->account_id);
}

void sh_item_paint(SocialSession *s, const cJSON *f) {
    if (!social_require_auth(s)) return;
    char me[24], copy[24], item[24], paint[24], shader[24];
    me_str(s, me);
    const char *colour = fa_str(f, 4);
    social_call(s, &P_ITEM_PAINT,
                KN_DB_ARGS(me, acct_itoa(copy, fa_int(f, 1, -1)), acct_itoa(item, fa_int(f, 2, -1)),
                           acct_itoa(paint, fa_int(f, 3, -1)), colour ? colour : "", acct_itoa(shader, fa_int(f, 5, -1))),
                paint_done, ctx_new("ipr", fa_int(f, 6, 0), 0));
}

static void unlist_done(SocialSession *s, kn_db_result *res, void *extra) {
    Ctx *c = extra;
    cJSON *v = kn_db_ok(res) ? acct_db_json(res) : NULL;
    if (!v) {
        social_send_n(s, "ulir", 3, cJSON_CreateNull(), cJSON_CreateNumber((double) c->a),
                      cJSON_CreateString(kn_db_ok(res) ? "Failed. Try Again" : acct_db_error(res, "uli")));
        return;
    }
    social_send_n(s, "ulir", 2, cJSON_CreateString(kn_json_str(v, "r", "SUCCESS")), cJSON_CreateNumber((double) c->a));
    cJSON_Delete(v);
}

/* ["uli", skin, listingId, uid, {op}] -> ["ulir", "SUCCESS"|"SOLD", uid] | ["ulir", null, uid, err] */
void sh_market_unlist(SocialSession *s, const cJSON *f) {
    if (!social_require_auth(s)) return;
    EconHead h;
    econ_head(&h, s, f, "unlist", ""); /* always allowed: it gives the player's item back */
    char id[24];
    social_call(s, &P_MARKET_UNLIST, KN_DB_ARGS(h.me, h.key, h.rate, h.feature, acct_itoa(id, fa_int(f, 2, 0))),
                unlist_done, ctx_new("uli", fa_int(f, 3, 0), 0));
}

static void buy_done(SocialSession *s, kn_db_result *res, void *extra) {
    Ctx *c = extra;
    cJSON *v = kn_db_ok(res) ? acct_db_json(res) : NULL;
    if (!v || !cJSON_IsObject(v)) {
        cJSON_Delete(v);
        const char *err = kn_db_ok(res) ? "Item Unavailable" : acct_db_error(res, "pi");
        if (!strcmp(err, "Item Unavailable")) social_send_n(s, "br", 2, cJSON_CreateNull(), cJSON_CreateNumber((double) c->a));
        else social_error(s, err);
        return;
    }
    const int replay = kn_json_int(v, "replay", 0) != 0;
    const int64_t seller = replay ? 0 : kn_json_int(v, "seller", 0), skin = kn_json_int(v, "i", -1), price = kn_json_int(v, "price", 0);
    cJSON_DeleteItemFromObjectCaseSensitive(v, "seller");
    cJSON_DeleteItemFromObjectCaseSensitive(v, "replay");
    social_send_n(s, "br", 2, v, cJSON_CreateNumber((double) c->a)); /* io_br({f, sid}, uid) */
    push_funds(s->account_id);
    /* v7.1.4 notifies the seller with an "Item sold" mail (index.js: notification type 4) */
    if (seller > 0) {
        const SkinInfo *k = acct_skin(skin);
        char body[256], sid[24];
        snprintf(body, sizeof body, "%s bought your %s for %lld KR", s->username, k && k->name[0] ? k->name : "item",
                 (long long) price);
        kn_db_query_sql(g_acct_brain->db, SQL(.any = "INSERT INTO mail (account_id, sender, subject, body, kr) VALUES ($1, 'Market', 'Item sold', $2, 0)"),
                        KN_DB_ARGS(acct_itoa(sid, seller), body), ignore_done, NULL);
        social_push_mail_count(seller);
        push_funds(seller);
        push_note(seller, "sale", body);
    }
}

/* ["pi", listingId, uid, {op}] -> ["br", {f, i, sid, m, price} | null, uid].
 * (The server -> client "pi" is the ping.) */
void sh_market_buy(SocialSession *s, const cJSON *f) {
    if (!social_require_auth(s)) return;
    EconHead h;
    econ_head(&h, s, f, "buy", "market");
    char id[24], lvl[24], grace[24];
    social_call(s, &P_MARKET_BUY,
                KN_DB_ARGS(h.me, h.key, h.rate, h.feature, acct_itoa(id, fa_int(f, 1, 0)), acct_itoa(lvl, g_acct.market_min_level),
                           acct_itoa(grace, g_acct.sale_grace_s)),
                buy_done, ctx_new("pi", fa_int(f, 2, 0), 0));
}

static void quick_sell_done(SocialSession *s, kn_db_result *res, void *extra) {
    Ctx *c = extra;
    cJSON *v = kn_db_ok(res) ? acct_db_json(res) : NULL;
    if (!v || !cJSON_IsObject(v)) {
        cJSON_Delete(v);
        if (!kn_db_ok(res)) acct_db_error(res, "qsi");
        social_send_n(s, "qsr", 1, cJSON_CreateNull());
        return;
    }
    cJSON_DeleteItemFromObjectCaseSensitive(v, "replay");
    cJSON_AddNumberToObject(v, "i", (double) c->a); /* io_qsr: e.i = card uid */
    social_send_n(s, "qsr", 1, v);
    push_funds(s->account_id);
}

/* KR per quick-sold unit: the item's qsKR, else store.rarities[r].sell (5 for minQuickSell items).
 * KrunkNative: an item the game sells for KR (the Black Market) never quick-sells for KR (its
 * rarity value can be 25x its price: 100 KR capes worth 2500 KR); it can still be destroyed */
static int64_t quick_sell_each(const SkinInfo *k) {
    int64_t each = 0;
    if (k->rarity >= 0 && k->rarity < RARITIES) {
        each = k->qs_kr > 0 ? k->qs_kr : k_rarity_sell[k->rarity];
        if (k->min_quick_sell && each > 0) each = 5; /* config.minQSKR */
    }
    if (k->black_m || k->funds > 0) each = 0;
    return each;
}

/* v7.1.4 ["qsi", mode (0 KR, 1 junk, 2 destroy), skin, uid, amount (0 one, 1 all but one, 2 all), {op}]
 * or the older KrunkNative ["qsi", skin, uid, amount] -> ["qsr", {i: uid, c, f, kr, j, jnk} | null].
 * KR per item: the item's qsKR, else store.rarities[r].sell (5 for minQuickSell items). Junk
 * does not exist here: mode 1 is refused. */
void sh_quick_sell(SocialSession *s, const cJSON *f) {
    if (!social_require_auth(s)) return;
    const int v7 = frame_args(f) >= 4;
    const int64_t mode = v7 ? fa_int(f, 1, 0) : 0, id = fa_int(f, v7 ? 2 : 1, -1), uid = fa_int(f, v7 ? 3 : 2, 0);
    const int64_t sel = fa_int(f, v7 ? 4 : 3, 0);
    const SkinInfo *k = acct_skin(id);
    const int64_t each = k ? quick_sell_each(k) : 0;
    if (!k || (mode != 0 && mode != 2) || (mode == 0 && (k->no_quick_sell || each <= 0)) || sel < 0 || sel > 2) {
        social_send_n(s, "qsr", 1, cJSON_CreateNull());
        return;
    }
    EconHead h;
    econ_head(&h, s, f, "qsell", "market");
    /* the frame's ext carries "copy" when the sold unit is a painted copy (the official skin_id of a single sale) */
    const cJSON *cv = cJSON_GetObjectItemCaseSensitive(frame_ext(f), "copy");
    const int64_t copy = cJSON_IsNumber(cv) && cv->valuedouble >= V10_COPY_ID_BASE ? (int64_t) cv->valuedouble : 0;
    char sk[24], md[24], am[24], kr[24], cp[24];
    social_call(s, &P_QUICK_SELL,
                KN_DB_ARGS(h.me, h.key, h.rate, h.feature, acct_itoa(sk, id), acct_itoa(md, mode), acct_itoa(am, sel),
                           acct_itoa(kr, mode == 0 ? each : 0), acct_itoa(cp, copy)),
                quick_sell_done, ctx_new("qsi", uid, 0));
}

static void blackm_done(SocialSession *s, kn_db_result *res, void *extra) {
    (void) extra;
    cJSON *v = kn_db_ok(res) ? acct_db_json(res) : NULL;
    if (!v || !cJSON_IsObject(v)) {
        cJSON_Delete(v);
        const char *err = kn_db_ok(res) ? "Item Unavailable" : acct_db_error(res, "pbm");
        if (!strcmp(err, "Item Unavailable")) social_send_n(s, "bmr", 1, cJSON_CreateNull()); /* io_bmr(null) */
        else social_error(s, err);
        return;
    }
    cJSON_DeleteItemFromObjectCaseSensitive(v, "replay");
    social_send_n(s, "bmr", 1, v); /* io_bmr({item, kr, f}) */
    push_funds(s->account_id);
}

/* ["pbm", skin, {op}] -> ["bmr", {item, kr, f} | null]: a blackM item for its store `funds` KR */
void sh_blackm_buy(SocialSession *s, const cJSON *f) {
    if (!social_require_auth(s)) return;
    const int64_t id = fa_int(f, 1, -1);
    const SkinInfo *k = acct_skin(id);
    if (!k || !k->black_m || k->funds <= 0 || !strcmp(k->req_kind, "disabled") || k->blocked) {
        social_send_n(s, "bmr", 1, cJSON_CreateNull());
        return;
    }
    EconHead h;
    econ_head(&h, s, f, "buy", "market");
    char sk[24], pr[24], amt[48];
    snprintf(amt, sizeof amt, "%.17g", k->req_amount);
    social_call(s, &P_BLACKM_BUY,
                KN_DB_ARGS(h.me, h.key, h.rate, h.feature, acct_itoa(sk, id), acct_itoa(pr, k->funds), k->req_kind, k->req_var, amt),
                blackm_done, NULL);
}

/* ------------------------------------------------------------------ trades */

static void trade_inv_done(SocialSession *s, kn_db_result *res, void *extra) {
    Ctx *c = extra;
    cJSON *v = kn_db_ok(res) ? acct_db_json(res) : NULL;
    if (!v) {
        social_error(s, kn_db_ok(res) ? "Player not found" : acct_db_error(res, "ri"));
        return;
    }
    /* io_trD(name, id, [myInventory, theirInventory]) with items {i, c}; KrunkNative marks the
     * items that cannot be traded (v7.1.4 checks store.skins itself) with nt: 1 */
    for (int side = 0; side < 2; side++) {
        cJSON *it;
        cJSON_ArrayForEach(it, cJSON_GetArrayItem(v, side)) {
            const SkinInfo *k = acct_skin(kn_json_int(it, "i", -1));
            if (k && !skin_tradable(k)) cJSON_AddNumberToObject(it, "nt", 1);
        }
    }
    social_send_n(s, "trD", 3, cJSON_CreateString(c->text), cJSON_CreateNumber((double) c->a), v);
}

/* ["ri", name, id, captcha] -> ["trD", name, id, [mine, theirs]] */
void sh_trade_inv(SocialSession *s, const cJSON *f) {
    if (!social_require_auth(s)) return;
    const char *name = fa_str(f, 1);
    Ctx *c = ctx_new("ri", fa_int(f, 2, 0), 0);
    if (!c) return;
    snprintf(c->text, sizeof c->text, "%s", name ? name : "");
    char me[24], them[24];
    social_call(s, &P_TRADE_INVENTORIES, KN_DB_ARGS(me_str(s, me), acct_itoa(them, c->a)), trade_inv_done, c);
}

static void trade_done(SocialSession *s, kn_db_result *res, void *extra) {
    Ctx *c = extra;
    cJSON *v = kn_db_ok(res) ? acct_db_json(res) : NULL;
    if (!v || !cJSON_IsObject(v)) {
        cJSON_Delete(v);
        /* io_po(null, msg): the popup shows the reason (io_trR(null) would only say "Failed") */
        social_error(s, kn_db_ok(res) ? "Failed. Try Again" : acct_db_error(res, c->label));
        return;
    }
    const int create = !strcmp(c->label, "ctr"), accept = !strcmp(c->label, "trA"), clear = !strcmp(c->label, "trCA");
    const int replay = kn_json_int(v, "replay", 0) != 0;
    cJSON_DeleteItemFromObjectCaseSensitive(v, "replay");
    if (clear) {
        const cJSON *it;
        cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(v, "closed")) {
            if (replay) break;
            push_note(kn_json_int(it, "sender", 0), "trade", "Trade Offer Declined");
            push_funds(kn_json_int(it, "sender", 0));
        }
        cJSON_Delete(v);
        social_send_n(s, "pRes", 3, cJSON_CreateString("SUCCESS"), cJSON_CreateNull(), cJSON_CreateString("Declined All"));
        return;
    }
    const int removed = !create && !accept && !kn_json_int(v, "declined", 1);
    const char *msg = create ? "Offer Sent" : accept ? "Trade Accepted" : removed ? "Offer Removed" : "Offer Declined";
    const int64_t peer = kn_json_int(v, "receiver", kn_json_int(v, "sender", kn_json_int(v, "peer", 0)));
    /* io_trR(result, tradeIdToRemove, msg) */
    social_send_n(s, "trR", 3, v, create ? cJSON_CreateNull() : cJSON_CreateNumber((double) c->a), cJSON_CreateString(msg));
    push_funds(s->account_id);
    if (peer > 0 && !replay) {
        push_note(peer, "trade", create ? "New Trade Offer" : accept ? "Trade Offer Accepted" : removed ? "Trade Offer Removed" : "Trade Offer Declined");
        push_funds(peer);
    }
}

/* One side of an offer from the client: [{i, c}, ...] (v7.1.4), [id, id, ...] or "id,id," ->
 * [[skin, count], ...] with distinct skins, or NULL with *err set. */
static cJSON *offer_side(const cJSON *v, int64_t *fee, const char **err) {
    int64_t skins[128], counts[128], copies[128], copy_skins[128];
    int n = 0, nc = 0;
    const cJSON *it;
    char *str = cJSON_IsString(v) ? strdup(v->valuestring) : NULL;
    cJSON *parsed = NULL;
    if (str) { /* "1,1,5," */
        parsed = cJSON_CreateArray();
        for (char *t = strtok(str, ","); t; t = strtok(NULL, ",")) cJSON_AddItemToArray(parsed, cJSON_CreateNumber((double) atoll(t)));
        free(str);
        v = parsed;
    }
    if (v && !cJSON_IsArray(v)) {
        *err = "Invalid offer";
        cJSON_Delete(parsed);
        return NULL;
    }
    cJSON_ArrayForEach(it, v) {
        /* a painted copy, as the official client offers it (game.js 125069: {i, c: 1, iids: [copy id], sid, m}): one
         * unit per copy id, its skin from i (doc/v10/paint-copy.md) */
        const cJSON *iids = cJSON_IsObject(it) ? cJSON_GetObjectItemCaseSensitive(it, "iids") : NULL;
        if (cJSON_IsArray(iids)) {
            const cJSON *iv = cJSON_GetObjectItemCaseSensitive(it, "i");
            const double skd = cJSON_IsNumber(iv) ? iv->valuedouble : -1;
            const int64_t id = skd >= 0 && skd < 1e7 ? (int64_t) skd : -1;
            const SkinInfo *k = acct_skin(id);
            if (id < 0 || (acct_skin_count() > 0 && !k)) {
                *err = "Invalid offer";
                cJSON_Delete(parsed);
                return NULL;
            }
            if (k && !skin_tradable(k)) {
                *err = "Item cant be traded";
                cJSON_Delete(parsed);
                return NULL;
            }
            const cJSON *cid;
            cJSON_ArrayForEach(cid, iids) {
                const double cd = cJSON_IsNumber(cid) ? cid->valuedouble : -1;
                if (cd < V10_COPY_ID_BASE || cd >= 9e15 || nc >= 128) {
                    *err = "Invalid offer";
                    cJSON_Delete(parsed);
                    return NULL;
                }
                copies[nc] = (int64_t) cd;
                copy_skins[nc++] = id;
                if (k && k->rarity >= 0 && k->rarity < RARITIES) *fee += k_rarity_trade_fee[k->rarity];
            }
            continue;
        }
        /* range-checked as doubles: (int64_t) of a huge / NaN double is undefined */
        double idd = -1, cntd = 1;
        if (cJSON_IsObject(it)) {
            const cJSON *iv = cJSON_GetObjectItemCaseSensitive(it, "i"), *cv = cJSON_GetObjectItemCaseSensitive(it, "c");
            idd = cJSON_IsNumber(iv) ? iv->valuedouble : -1;
            cntd = cv ? (cJSON_IsNumber(cv) ? cv->valuedouble : -1) : 1;
        } else if (cJSON_IsNumber(it)) {
            idd = it->valuedouble;
        } else if (cJSON_IsString(it)) {
            idd = (double) atoll(it->valuestring);
        }
        const int64_t id = idd >= 0 && idd < 1e7 ? (int64_t) idd : -1, cnt = cntd >= 1 && cntd <= 1000 ? (int64_t) cntd : 0;
        const SkinInfo *k = acct_skin(id);
        if (id < 0 || cnt < 1 || cnt > 1000 || (acct_skin_count() > 0 && !k)) {
            *err = "Invalid offer";
            cJSON_Delete(parsed);
            return NULL;
        }
        if (k && !skin_tradable(k)) {
            *err = "Item cant be traded";
            cJSON_Delete(parsed);
            return NULL;
        }
        if (k && k->rarity >= 0 && k->rarity < RARITIES) *fee += k_rarity_trade_fee[k->rarity] * cnt;
        int j = 0;
        while (j < n && skins[j] != id) j++;
        if (j == n) {
            if (n >= 128) {
                *err = "Invalid offer";
                cJSON_Delete(parsed);
                return NULL;
            }
            skins[n] = id;
            counts[n++] = 0;
        }
        counts[j] += cnt;
    }
    cJSON_Delete(parsed);
    /* the stacks ([skin, count]) first, then the painted copies ([skin, 1, copy id]) */
    cJSON *out = cJSON_CreateArray();
    for (int j = 0; j < n; j++) {
        cJSON *pair = cJSON_CreateArray();
        cJSON_AddItemToArray(pair, cJSON_CreateNumber((double) skins[j]));
        cJSON_AddItemToArray(pair, cJSON_CreateNumber((double) counts[j]));
        cJSON_AddItemToArray(out, pair);
    }
    for (int j = 0; j < nc; j++) {
        cJSON *triple = cJSON_CreateArray();
        cJSON_AddItemToArray(triple, cJSON_CreateNumber((double) copy_skins[j]));
        cJSON_AddItemToArray(triple, cJSON_CreateNumber(1));
        cJSON_AddItemToArray(triple, cJSON_CreateNumber((double) copies[j]));
        cJSON_AddItemToArray(out, triple);
    }
    return out;
}

/* ["ctr", receiverId, give, take, message, captcha?, {op, skr, rkr}] -> ["trR", {trid, funds, fee}, null, "Offer Sent"].
 * give / take: [{i, c}] (v7.1.4). skr / rkr: KR the sender gives / asks for (KrunkNative, under
 * the gift rules: db_economy.c). The fee is v7.1.4 store.calcTradeFee over both sides
 * (rarities[].fee per item; premium pays 0) plus the gift fee on skr. */
void sh_trade_create(SocialSession *s, const cJSON *f) {
    if (!social_require_auth(s)) return;
    const char *err = NULL;
    int64_t fee = 0;
    cJSON *give = offer_side(fa(f, 2), &fee, &err), *take = give ? offer_side(fa(f, 3), &fee, &err) : NULL;
    const cJSON *ext = frame_ext(f);
    const cJSON *skr_v = cJSON_GetObjectItemCaseSensitive(ext, "skr"), *rkr_v = cJSON_GetObjectItemCaseSensitive(ext, "rkr");
    const double skr_d = cJSON_IsNumber(skr_v) ? skr_v->valuedouble : 0, rkr_d = cJSON_IsNumber(rkr_v) ? rkr_v->valuedouble : 0;
    const double kr_max = (double) g_acct.trade_kr_max;
    const int kr_ok = skr_d >= 0 && rkr_d >= 0 && skr_d <= kr_max && rkr_d <= kr_max && skr_d == (double) (int64_t) skr_d &&
                      rkr_d == (double) (int64_t) rkr_d;
    const int64_t skr = kr_ok ? (int64_t) skr_d : -1, rkr = kr_ok ? (int64_t) rkr_d : -1;
    if (!give || !take || !kr_ok) {
        cJSON_Delete(give);
        cJSON_Delete(take);
        social_error(s, err ? err : "Invalid amount");
        return;
    }
    char *g = kn_json_print(give), *t = kn_json_print(take);
    cJSON_Delete(give);
    cJSON_Delete(take);
    EconHead h;
    econ_head(&h, s, f, "trade", "trade");
    char to[24], sk[24], rk[24], fe[24], lvl[24], mx[24], mxp[24], ttl[24], mo[24], km[24], gp[24], gl[24], gm[24];
    const char *msg = fa_str(f, 4);
    social_call(s, &P_TRADE_CREATE,
                KN_DB_ARGS(h.me, h.key, h.rate, h.feature, acct_itoa(to, fa_int(f, 1, 0)), g, t, msg ? msg : "",
                           acct_itoa(sk, skr), acct_itoa(rk, rkr), acct_itoa(fe, fee), acct_itoa(lvl, g_acct.trade_min_level),
                           acct_itoa(mx, g_acct.trade_max_items), acct_itoa(mxp, g_acct.trade_max_items_premium),
                           acct_itoa(ttl, g_acct.trade_ttl_s), acct_itoa(mo, g_acct.trade_max_open), acct_itoa(km, g_acct.trade_kr_max),
                           acct_itoa(gp, g_acct.gift_fee_percent), acct_itoa(gl, g_acct.gift_min_level), acct_itoa(gm, g_acct.gift_min)),
                trade_done, ctx_new("ctr", 0, 0));
    free(g);
    free(t);
}

/* ["trA", tradeId, captcha?, {op}] -> ["trR", {trid, funds}, tradeId, "Trade Accepted"] */
void sh_trade_accept(SocialSession *s, const cJSON *f) {
    if (!social_require_auth(s)) return;
    const int64_t trade = fa_int(f, 1, 0);
    EconHead h;
    econ_head(&h, s, f, "accept", "trade");
    char id[24], lvl[24], gp[24], gl[24];
    social_call(s, &P_TRADE_ACCEPT,
                KN_DB_ARGS(h.me, h.key, h.rate, h.feature, acct_itoa(id, trade), acct_itoa(lvl, g_acct.trade_min_level),
                           acct_itoa(gp, g_acct.gift_fee_percent), acct_itoa(gl, g_acct.gift_min_level)),
                trade_done, ctx_new("trA", trade, 0));
}

/* ["trC", tradeId, fast?, {op}] decline (receiver) / remove (sender) -> ["trR", {trid, funds}, tradeId, msg] */
void sh_trade_cancel(SocialSession *s, const cJSON *f) {
    if (!social_require_auth(s)) return;
    const int64_t trade = fa_int(f, 1, 0);
    EconHead h;
    econ_head(&h, s, f, "close", ""); /* always allowed: it gives the escrow back */
    char id[24];
    social_call(s, &P_TRADE_CLOSE, KN_DB_ARGS(h.me, h.key, h.rate, h.feature, acct_itoa(id, trade)), trade_done,
                ctx_new("trC", trade, 0));
}

/* ["trCA", {op}] decline every open incoming offer -> ["pRes", "SUCCESS", null, "Declined All"] */
void sh_trade_clear(SocialSession *s, const cJSON *f) {
    if (!social_require_auth(s)) return;
    EconHead h;
    econ_head(&h, s, f, "close", "");
    social_call(s, &P_TRADE_CLEAR, KN_DB_ARGS(h.me, h.key, h.rate, h.feature), trade_done, ctx_new("trCA", 0, 0));
}

/* ------------------------------------------------------------------ KR history */

static void history_done(SocialSession *s, kn_db_result *res, void *extra) {
    (void) extra;
    cJSON *v = kn_db_ok(res) ? acct_db_json(res) : NULL;
    if (!kn_db_ok(res)) acct_db_error(res, "krh");
    social_send_n(s, "krh", 1, v ? v : cJSON_CreateArray());
}

/* ["krh", days?] -> ["krh", [{id, amount, source, ref, cp, fee, tim}]] (KrunkNative: own ledger) */
void sh_kr_history(SocialSession *s, const cJSON *f) {
    if (!social_require_auth(s)) return;
    int64_t days = fa_int(f, 1, 30);
    if (days < 1 || days > 366) days = 30;
    char me[24], d[24];
    social_call(s, &P_KR_HISTORY, KN_DB_ARGS(me_str(s, me), acct_itoa(d, days), "200"), history_done, NULL);
}

/* ------------------------------------------------------------------ expiry timer */

static void expire_done(kn_db_result *res, void *ud) {
    (void) ud;
    cJSON *v = kn_db_ok(res) ? acct_db_json(res) : NULL;
    if (!kn_db_ok(res)) {
        KN_WARN(MOD, "trade expiry: %s", kn_db_error(res));
        return;
    }
    const cJSON *it;
    int n = 0;
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(v, "closed")) {
        push_note(kn_json_int(it, "sender", 0), "trade", "Trade Offer Expired");
        push_funds(kn_json_int(it, "sender", 0));
        n++;
    }
    if (n) KN_INFO(MOD, "%d trade offer(s) expired", n);
    cJSON_Delete(v);
}

static void economy_tick(kn_loop *loop, void *ud) {
    (void) loop;
    (void) ud;
    if (g_acct_brain && g_acct_brain->db) kn_db_call(g_acct_brain->db, &P_TRADE_EXPIRE, 0, NULL, expire_done, NULL);
}

void economy_init(Brain *b) {
    const int64_t ms = kn_config_int(b->cfg, "accounts.economy_tick_ms", 60000);
    s_timer = kn_loop_add_timer(b->loop, 5000, (uint64_t) (ms > 100 ? ms : 60000), economy_tick, NULL);
}

void economy_shutdown(Brain *b) {
    if (s_timer) kn_loop_cancel_timer(b->loop, s_timer);
    s_timer = 0;
}
