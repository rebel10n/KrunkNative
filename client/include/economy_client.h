#pragma once
// The economy's client half (doc/v7/economy.md section 4): the action frames KrunkNative clients
// send kn_brain over the social socket, their idempotency keys, the server's rules ("ecfg") and the
// value tables the fee / quick-sell displays use. Shared by the v7 hub (client/src/ui/social_ui.c)
// and the v10 Market & Trading popup (client/src/ui/v10_market.c), so the protocol is written once.
// The builders return a new frame (a cJSON array, the caller sends and frees it).
#include <cJSON.h>
#include <stddef.h>

// The rules (v7.1.4 src/config.js defaults until kn_brain's "ecfg" reply replaces them).
typedef struct {
    double gift_fee, gift_min, gift_max, gift_min_lvl;          // giftFee (%), giftMin, giftMax, giftMinLVl
    double market_fee, market_fee_bypass, market_min_lvl;       // marketFee (%), marketFeeBypass, marketMinLVl
    double sale_max, sale_grace, max_listings;                  // saleMax, saleGrace (ms), the "MAX" listings
    double trade_min_lvl, trade_max_out, trade_max_out_p;       // tradeMinLvl, tradeMaxOut, tradeMaxOutP
    double trade_kr_max;                                        // KrunkNative: KR per side of an offer (0 = none)
} EconRules;
extern EconRules g_econ_rules;

void econ_rules_apply(const cJSON *ecfg);   // the "ecfg" reply's object
cJSON *econ_rules_request(void);            // ["ecfg"]

// store.rarities[r].sell / .fee (the quick-sell KR and calcTradeFee per unit), 0..7
int econ_rarity_sell(int rarity);
int econ_rarity_trade_fee(int rarity);
// What the server charges: the list fee floor(price * marketFee%) (0 at or below marketFeeBypass),
// the gift fee ceil(amount * giftFee%) on top
double econ_list_fee(double price);
double econ_gift_fee(double amount);

// An action's idempotency key: kn_brain runs an action once per key, a resent frame replays it.
void econ_new_op(char *out, size_t n, const char *tag);
// [label, v[0..n-1], {op: key}]: li, uli, pi, qsi, pbm, trA, trC, trCA
cJSON *econ_frame(const char *label, int n, const double *v, const char *key);
// One unit of an offer: a stack {i: skin, c: count} (sid 0), or a painted copy, sid its copy id, sent
// as {i, c: 1, iids: [sid]} (Krunker v10.0.3's offer serializer keeps {i, c, iids}: deob-10.0.3.js:125109-125113;
// the copy row's offer entry is built at deob:125069-125072; doc/v10/paint-copy.md section 7)
typedef struct { int i, c, sid; } EconTradeItem;
// ["ctr", receiver, mine, theirs, message, {op, skr?, rkr?}]
cJSON *econ_offer_frame(int receiver_id, const EconTradeItem *mine, int n_mine, const EconTradeItem *theirs, int n_theirs,
                        const char *message, double my_kr, double their_kr, const char *key);

// The Black Market (store.skins entries with `illicit` / blackM): funds, the requirement text and
// rule. The viewer skin table does not carry them. req: stat key + minimum (acc.stats), or age_ms > 0
// = account age, or neither = never met ("No Greenskins!"). kn_brain checks the same (store.checkReq).
typedef struct { int skin, funds; const char *req_t, *stat; double min, age_ms; } EconBlackMarketItem;
const EconBlackMarketItem *econ_black_market(int *count);
const EconBlackMarketItem *econ_black_market_find(int skin);
