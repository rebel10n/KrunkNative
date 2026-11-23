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

// The Black Market: every store.skins entry with blackM (client/src/ui/economy_black_market_table.c, generated
// from assets/skins.json by tools/ui-import/gen_black_market.py; the viewer skin table carries none of this):
// its price, requirement text and store.checkReq rule (deob-10.0.7.js:103566-103580). kn_brain checks the same
// rule when it sells one (services/brain/db_economy.c run_blackm_buy).
typedef enum {
    ECON_REQ_NONE,      // no req: always met
    ECON_REQ_ACC_AGE,   // now - account.createDate >= amount (ms)
    ECON_REQ_STATS,     // account.stats[var] >= amount
    ECON_REQ_ACC_VAR,   // account[var] >= amount (funds, trades, jobRatingPos, kills, timePlayed, followers)
    ECON_REQ_RANK,      // peakMMR (GET /market/black-market/rank's peak_mmr) >= amount
    ECON_REQ_OWN_SKINS, // the account owns every one of skins[]
    ECON_REQ_NEVER,     // "disabled" or a kind checkReq does not know: never met
} EconReqKind;
typedef struct {
    int skin, funds;
    const char *req_t; // the card's requirement line (reqT)
    EconReqKind req;
    const char *var;   // ECON_REQ_STATS / ECON_REQ_ACC_VAR: the key
    double amount;
    const int *skins;  // ECON_REQ_OWN_SKINS
    int skin_count;
    unsigned char hide, free_; // v10's tab leaves these out (deob-10.0.7.js:137667); the Catalog still names them
} EconBlackMarketItem;
extern const EconBlackMarketItem g_econ_black_market[];
extern const int g_econ_black_market_count;
const EconBlackMarketItem *econ_black_market(int *count);
const EconBlackMarketItem *econ_black_market_find(int skin);
// The account values store.checkReq reads. Each field is what v10's account object holds under that name
// (deob-10.0.7.js:115085-115130); create_ms <= 0 (unknown) or an unknown var is unmet.
typedef struct {
    double create_ms, funds, trades, job_rating_pos, kills, time_played, followers, peak_mmr;
    const cJSON *stats; // acc.stats
    int (*owns)(int skin, void *user); // account.skins has the item
    void *owns_user;
} EconReqAccount;
int econ_black_market_req_met(const EconBlackMarketItem *it, const EconReqAccount *a);
