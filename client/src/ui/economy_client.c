// The economy's client half (client/include/economy_client.h, doc/v7/economy.md section 4).
#include <economy_client.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
    /* v10.0.7's list modal (deob-10.0.7.js:130807): Math.ceil(0.1 * parseInt(price)). The percent
     * comes from the server's marketFee. At or below marketFeeBypass there is no fee. */
    const long long p = (long long) price, pct = (long long) g_econ_rules.market_fee;
    if (p < 1 || p <= (long long) g_econ_rules.market_fee_bypass || pct <= 0) return 0;
    return (double) ((p * pct + 99) / 100);
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

// the table: economy_black_market_table.c (tools/ui-import/gen_black_market.py)
const EconBlackMarketItem *econ_black_market(int *count) {
    if (count) *count = g_econ_black_market_count;
    return g_econ_black_market;
}

const EconBlackMarketItem *econ_black_market_find(const int skin) {
    for (int i = 0; i < g_econ_black_market_count; i++)
        if (g_econ_black_market[i].skin == skin) return &g_econ_black_market[i];
    return NULL;
}

// account[reqVar] for the accVar keys v10's table uses (deob-10.0.7.js:115088-115126: kills, funds, timePlayed,
// followers, jobRatingPos, trades)
static int acc_var(const EconReqAccount *a, const char *var, double *out) {
    static const struct { const char *key; size_t off; } k[] = {
        {"funds", offsetof(EconReqAccount, funds)},           {"trades", offsetof(EconReqAccount, trades)},
        {"jobRatingPos", offsetof(EconReqAccount, job_rating_pos)}, {"kills", offsetof(EconReqAccount, kills)},
        {"timePlayed", offsetof(EconReqAccount, time_played)}, {"followers", offsetof(EconReqAccount, followers)},
    };
    for (size_t i = 0; var && i < sizeof(k) / sizeof(k[0]); i++) {
        if (strcmp(var, k[i].key) != 0) continue;
        *out = *(const double *) ((const char *) a + k[i].off);
        return 1;
    }
    return 0;
}

// store.checkReq(item, account) (deob-10.0.7.js:103566-103580); the tab passes {peakMMR} for a rank item
// (:137682-137685), here a->peak_mmr. An item with no req is met without an account (:137682 `!req ||`).
int econ_black_market_req_met(const EconBlackMarketItem *it, const EconReqAccount *a) {
    if (!it) return 0;
    if (it->req == ECON_REQ_NONE) return 1;
    if (!a) return 0;
    double v = 0;
    switch (it->req) {
        case ECON_REQ_ACC_AGE: return a->create_ms > 0 && epoch_ms() - a->create_ms >= it->amount;
        case ECON_REQ_STATS: {
            const cJSON *s = a->stats && it->var ? cJSON_GetObjectItemCaseSensitive(a->stats, it->var) : NULL;
            return (cJSON_IsNumber(s) ? s->valuedouble : 0) >= it->amount; // (stats[reqVar] || 0) >= reqAmount
        }
        case ECON_REQ_ACC_VAR: return acc_var(a, it->var, &v) && v >= it->amount;
        case ECON_REQ_RANK: return a->peak_mmr >= it->amount;
        case ECON_REQ_OWN_SKINS:
            if (!a->owns || it->skin_count <= 0) return 0;
            for (int i = 0; i < it->skin_count; i++)
                if (!a->owns(it->skins[i], a->owns_user)) return 0;
            return 1;
        default: return 0;
    }
}
