// The economy's client half (client/include/economy_client.h, doc/v7/economy.md section 4).
#include <economy_client.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#ifndef _WIN32
#include <sys/time.h>
#endif

// v10.0.2 levels: giftMinLVl / marketMinLVl / tradeMinLvl 10 (v7.1.4: 30 / 15 / 20; doc/v10/overview.md)
EconRules g_econ_rules = {10, 10, 10000000, 10, 10, 0, 10, 10000000, 300000, 40, 10, 5, 20, 10000000};

static const int k_rarity_sell[] = {1, 3, 10, 100, 500, 2500, 10000, 10};   // store.rarities[r].sell
static const int k_rarity_trade_fee[] = {5, 5, 5, 10, 10, 25, 200, 500};   // store.rarities[r].fee

int econ_rarity_sell(const int rarity) { return rarity >= 0 && rarity < 8 ? k_rarity_sell[rarity] : 1; }
int econ_rarity_trade_fee(const int rarity) { return rarity >= 0 && rarity < 8 ? k_rarity_trade_fee[rarity] : 0; }

void econ_rules_apply(const cJSON *o) {
    if (!cJSON_IsObject(o)) return;
    EconRules *e = &g_econ_rules;
#define ECFG(field, key) do { const cJSON *v_ = cJSON_GetObjectItemCaseSensitive(o, key); if (cJSON_IsNumber(v_)) e->field = v_->valuedouble; } while (0)
    ECFG(gift_fee, "giftFee"); ECFG(gift_min, "giftMin"); ECFG(gift_max, "giftMax"); ECFG(gift_min_lvl, "giftMinLvl");
    ECFG(market_fee, "marketFee"); ECFG(market_fee_bypass, "marketFeeBypass"); ECFG(market_min_lvl, "marketMinLvl");
    ECFG(sale_max, "saleMax"); ECFG(sale_grace, "saleGrace"); ECFG(max_listings, "maxListings");
    ECFG(trade_min_lvl, "tradeMinLvl"); ECFG(trade_max_out, "tradeMaxOut"); ECFG(trade_max_out_p, "tradeMaxOutP");
    ECFG(trade_kr_max, "tradeKRMax");
#undef ECFG
}

cJSON *econ_rules_request(void) {
    cJSON *f = cJSON_CreateArray();
    cJSON_AddItemToArray(f, cJSON_CreateString("ecfg"));
    return f;
}

double econ_list_fee(const double price) {
    if (price <= g_econ_rules.market_fee_bypass) return 0;
    return floor(price * g_econ_rules.market_fee / 100.0);
}

double econ_gift_fee(const double amount) {
    const long long a = (long long) amount, pct = (long long) g_econ_rules.gift_fee;
    return (double) ((a * pct + 99) / 100); // v7's Math.ceil, in integers (what the server charges)
}

static double epoch_ms(void) {
#ifdef _WIN32
    return (double) time(NULL) * 1000.0;
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double) tv.tv_sec * 1000.0 + (double) tv.tv_usec / 1000.0;
#endif
}

void econ_new_op(char *out, const size_t n, const char *tag) {
    static unsigned counter;
    snprintf(out, n, "%s-%.0f-%u-%d", tag, epoch_ms(), ++counter, rand() % 100000);
}

cJSON *econ_frame(const char *label, const int n, const double *v, const char *key) {
    cJSON *f = cJSON_CreateArray();
    cJSON_AddItemToArray(f, cJSON_CreateString(label));
    for (int i = 0; i < n; i++) cJSON_AddItemToArray(f, cJSON_CreateNumber(v[i]));
    cJSON *ext = cJSON_CreateObject();
    cJSON_AddStringToObject(ext, "op", key ? key : "");
    cJSON_AddItemToArray(f, ext);
    return f;
}

static cJSON *side_json(const EconTradeItem *items, const int n) {
    cJSON *a = cJSON_CreateArray();
    for (int k = 0; k < n; k++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "i", items[k].i);
        if (items[k].sid > 0) { // a painted copy: one unit, its copy id in iids (deob:125069-125072, 125109-125113)
            cJSON_AddNumberToObject(o, "c", 1);
            cJSON *iids = cJSON_AddArrayToObject(o, "iids");
            cJSON_AddItemToArray(iids, cJSON_CreateNumber(items[k].sid));
        } else {
            cJSON_AddNumberToObject(o, "c", items[k].c);
        }
        cJSON_AddItemToArray(a, o);
    }
    return a;
}

cJSON *econ_offer_frame(const int receiver_id, const EconTradeItem *mine, const int n_mine, const EconTradeItem *theirs, const int n_theirs,
                        const char *message, const double my_kr, const double their_kr, const char *key) {
    cJSON *f = cJSON_CreateArray();
    cJSON_AddItemToArray(f, cJSON_CreateString("ctr"));
    cJSON_AddItemToArray(f, cJSON_CreateNumber(receiver_id));
    cJSON_AddItemToArray(f, side_json(mine, n_mine));
    cJSON_AddItemToArray(f, side_json(theirs, n_theirs));
    cJSON_AddItemToArray(f, cJSON_CreateString(message ? message : ""));
    cJSON *ext = cJSON_CreateObject();
    cJSON_AddStringToObject(ext, "op", key ? key : "");
    if (my_kr > 0) cJSON_AddNumberToObject(ext, "skr", my_kr);
    if (their_kr > 0) cJSON_AddNumberToObject(ext, "rkr", their_kr);
    cJSON_AddItemToArray(f, ext);
    return f;
}

static const EconBlackMarketItem k_black_market[] = {
    {1483, 100, "1 Year Account Age", NULL, 0, 31536000000.0},
    {1484, 100, "2 Year Account Age", NULL, 0, 63072000000.0},
    {1641, 2000, "No Greenskins!", NULL, 0, 0},
    {1642, 3000, "Call in 1000 Nukes", "n", 1000, 0},
    {1659, 10000, "???????", "ggr", 59961, 0},
    {1672, 10000, "Level 50 Runner", "c9", 2667778, 0},
    {1734, 10000, "Level 100 Hunter", "c1", 10890000, 0},
    {1866, 10000, "10000 Thrown Melee Kills", "tmk", 10000, 0},
};

const EconBlackMarketItem *econ_black_market(int *count) {
    if (count) *count = (int) (sizeof(k_black_market) / sizeof(k_black_market[0]));
    return k_black_market;
}

const EconBlackMarketItem *econ_black_market_find(const int skin) {
    for (size_t i = 0; i < sizeof(k_black_market) / sizeof(k_black_market[0]); i++)
        if (k_black_market[i].skin == skin) return &k_black_market[i];
    return NULL;
}
