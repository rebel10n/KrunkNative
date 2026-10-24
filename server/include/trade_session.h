#pragma once
// One live trade's state (doc/v10/trade.md section 3): the two sides, their inventory snapshots and
// offers, the accept marks and the countdown. Pure: no sockets, no brain, no globals, so the rules
// are unit-tested on their own (server/tests/trade_test.c); server/src/trade.c drives it and sends
// what the calls report.
#include <stdint.h>

typedef enum {
    TRADE_OPENING = 0,   // accepted request, waiting for the brain's inventories
    TRADE_OPEN = 1,      // the window is open on both ends
    TRADE_COUNTDOWN = 2, // both accepted: "Loading n" until the settle starts
    TRADE_SETTLING = 3,  // the brain moves the items; nothing may change
} TradeState;

typedef enum {
    TRADE_OK = 0,
    TRADE_ERR_STATE,     // not now (opening / settling, or nothing to do)
    TRADE_ERR_NOT_OWNED, // not in the side's inventory (or all its units are offered already)
    TRADE_ERR_LOCKED,    // the item cannot be traded
    TRADE_ERR_FULL,      // the side holds tradeMaxOut / tradeMaxOutP units
    TRADE_ERR_NOT_OFFERED, // remove: that unit is not on the side
    TRADE_ERR_FUNDS,     // the side's KR + tradeCost exceed its funds
    TRADE_ERR_AMOUNT,    // a KR amount out of range
    TRADE_ERR_CURRENCY,  // not KR
    TRADE_ERR_EMPTY,     // accept with nothing on either side
} TradeError;

typedef struct {
    int32_t skin;
    int32_t count;   // a stack's units (1 for a copy)
    int64_t copy;    // a painted / instanced copy's id, 0 a stack
    uint8_t locked;  // cannot be traded (the brain's nt mark)
} TradeInvItem;

typedef struct {
    int32_t skin;
    int32_t count;   // units offered (1 for a copy)
    int64_t copy;
} TradeOffer;

#define TRADE_OFFER_MAX 32 // distinct entries per side (tradeMaxOutP 20 units fit)

typedef struct {
    int conn_id;        // ClientConnection id = Player.uid
    int account_id;
    int premium;
    int64_t funds;      // the brain's balance at open
    TradeInvItem *inv;  // the snapshot (owned by the side)
    int inv_count;
    TradeOffer offer[TRADE_OFFER_MAX];
    int offer_count;
    int64_t kr;         // KR on this side
    unsigned char accepted:1;
    unsigned char gone:1; // left while the settle ran
} TradeSide;

typedef struct {
    uint32_t id;
    TradeState state;
    TradeSide side[2];  // [0] requested, [1] accepted the request
    float started_at;   // when it entered its state (opening timeout)
    float countdown;    // s to the next "Loading n" step
    int countdown_n;    // the n shown last
    uint32_t rid;       // the pending brain request
} TradeSession;

void trade_session_init(TradeSession *t, uint32_t id, int conn_a, int account_a, int conn_b, int account_b, float now);
void trade_session_free(TradeSession *t); // the snapshots
// The side of a connection, -1 none.
int trade_session_side_of(const TradeSession *t, int conn_id);
// The snapshot (takes ownership of inv; NULL / 0 = empty) and the side's facts; the session opens once both are set.
void trade_session_set_inventory(TradeSession *t, int side, TradeInvItem *inv, int count, int64_t funds, int premium);
void trade_session_open(TradeSession *t, float now);
// units a side may hold: tradeMaxOutP when its owner is premium, else tradeMaxOut
int trade_session_side_limit(const TradeSession *t, int side);
int trade_session_side_units(const TradeSession *t, int side);
// One unit of the side owner's item onto / off the side (utrd 1 / 2 add or request it, 0 removes it).
// TRADE_OK clears both accept marks and drops a countdown back to TRADE_OPEN.
TradeError trade_session_add(TradeSession *t, int side, int32_t skin, int64_t copy);
TradeError trade_session_remove(TradeSession *t, int side, int32_t skin, int64_t copy);
// KR: add `amount` to the side (its KR + tradeCost must fit its funds), or take it all off.
TradeError trade_session_add_kr(TradeSession *t, int side, int64_t amount);
TradeError trade_session_clear_kr(TradeSession *t, int side);
// atrd: the side accepts. *both = 1 when that started the countdown (LIVE_TRADE_COUNTDOWN).
TradeError trade_session_accept(TradeSession *t, int side, int *both);
// The countdown. Returns the n to show ("Loading n") when a step is due, 0 when the settle is due
// (the state is TRADE_SETTLING then), -1 nothing to do.
int trade_session_tick(TradeSession *t, float dt);
// The offer of a side as the brain's JSON [[skin, count], [skin, 1, copyId], ...] (malloc'd).
char *trade_session_offer_json(const TradeSession *t, int side);
const char *trade_error_text(TradeError e, int limit);
