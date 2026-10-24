// The live trade's state machine (server/include/trade_session.h, doc/v10/trade.md section 3).
// v10.0.3's game server is not in the bundle: these rules are the client's contract
// (deob-10.0.3.js:251107-251527, the windows[36] object; :253238-253323, its packet handlers)
// plus the server's own checks, marked KN.
#include <trade_session.h>
#include <live_trade.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void trade_session_init(TradeSession *t, const uint32_t id, const int conn_a, const int account_a, const int conn_b,
                        const int account_b, const float now) {
    memset(t, 0, sizeof(*t));
    t->id = id;
    t->state = TRADE_OPENING;
    t->started_at = now;
    t->side[0].conn_id = conn_a;
    t->side[0].account_id = account_a;
    t->side[1].conn_id = conn_b;
    t->side[1].account_id = account_b;
}

void trade_session_free(TradeSession *t) {
    for (int s = 0; s < 2; s++) {
        free(t->side[s].inv);
        t->side[s].inv = NULL;
        t->side[s].inv_count = 0;
    }
}

int trade_session_side_of(const TradeSession *t, const int conn_id) {
    if (t->side[0].conn_id == conn_id && !t->side[0].gone) return 0;
    if (t->side[1].conn_id == conn_id && !t->side[1].gone) return 1;
    return -1;
}

void trade_session_set_inventory(TradeSession *t, const int side, TradeInvItem *inv, const int count, const int64_t funds,
                                 const int premium) {
    if (side < 0 || side > 1) {
        free(inv);
        return;
    }
    TradeSide *s = &t->side[side];
    free(s->inv);
    s->inv = inv;
    s->inv_count = inv ? count : 0;
    s->funds = funds;
    s->premium = premium != 0;
}

void trade_session_open(TradeSession *t, const float now) {
    t->state = TRADE_OPEN;
    t->started_at = now;
}

int trade_session_side_limit(const TradeSession *t, const int side) {
    // "You n/5" / "NAME n/20": the side's owner's premiumT (:251157, :251200)
    return t->side[side].premium ? LIVE_TRADE_MAX_OUT_PREMIUM : LIVE_TRADE_MAX_OUT;
}

int trade_session_side_units(const TradeSession *t, const int side) {
    int n = 0;
    for (int i = 0; i < t->side[side].offer_count; i++) n += t->side[side].offer[i].count;
    return n;
}

// Any change to an offer clears both READY marks (the client's utrd / utrdc handlers call updateButtons,
// :253314, :253322) and stops a countdown: nobody accepts what they have not seen (the guide's "Quick
// switching" red flag, :148537).
static void changed(TradeSession *t) {
    t->side[0].accepted = t->side[1].accepted = 0;
    if (t->state == TRADE_COUNTDOWN) t->state = TRADE_OPEN;
    t->countdown = 0.0f;
    t->countdown_n = 0;
}

static int editable(const TradeSession *t) {
    return t->state == TRADE_OPEN || t->state == TRADE_COUNTDOWN;
}

static TradeOffer *find_offer(TradeSide *s, const int32_t skin, const int64_t copy) {
    for (int i = 0; i < s->offer_count; i++) {
        if (s->offer[i].skin == skin && s->offer[i].copy == copy) return &s->offer[i];
    }
    return NULL;
}

static const TradeInvItem *find_inv(const TradeSide *s, const int32_t skin, const int64_t copy) {
    for (int i = 0; i < s->inv_count; i++) {
        const TradeInvItem *it = &s->inv[i];
        if (it->skin != skin) continue;
        if (copy ? it->copy == copy : !it->copy) return it;
    }
    return NULL;
}

TradeError trade_session_add(TradeSession *t, const int side, const int32_t skin, const int64_t copy) {
    if (!editable(t) || side < 0 || side > 1) return TRADE_ERR_STATE;
    TradeSide *s = &t->side[side];
    const TradeInvItem *it = find_inv(s, skin, copy);
    if (!it || it->count <= 0) return TRADE_ERR_NOT_OWNED;
    if (it->locked) return TRADE_ERR_LOCKED; // the lock overlay (:251146): add / request ignore it (:251276, :251290)
    TradeOffer *o = find_offer(s, skin, copy);
    if (o && o->count >= it->count) return TRADE_ERR_NOT_OWNED; // every unit is on the side already
    if (trade_session_side_units(t, side) >= trade_session_side_limit(t, side)) return TRADE_ERR_FULL;
    if (!o) {
        if (s->offer_count >= TRADE_OFFER_MAX) return TRADE_ERR_FULL;
        o = &s->offer[s->offer_count++];
        o->skin = skin;
        o->copy = copy;
        o->count = 0;
    }
    o->count++;
    changed(t);
    return TRADE_OK;
}

TradeError trade_session_remove(TradeSession *t, const int side, const int32_t skin, const int64_t copy) {
    if (!editable(t) || side < 0 || side > 1) return TRADE_ERR_STATE;
    TradeSide *s = &t->side[side];
    TradeOffer *o = find_offer(s, skin, copy);
    if (!o) return TRADE_ERR_NOT_OFFERED;
    if (--o->count <= 0) *o = s->offer[--s->offer_count]; // removeItem deletes the entry at 0 (:251361)
    changed(t);
    return TRADE_OK;
}

TradeError trade_session_add_kr(TradeSession *t, const int side, const int64_t amount) {
    if (!editable(t) || side < 0 || side > 1) return TRADE_ERR_STATE;
    TradeSide *s = &t->side[side];
    // addCurrency / requestCurrency send parseInt(value) > 0 (:251441, :251453); the amounts add up on the
    // side (addCurrencyItem, :251478)
    if (amount <= 0 || amount > LIVE_TRADE_KR_MAX || s->kr + amount > LIVE_TRADE_KR_MAX) return TRADE_ERR_AMOUNT;
    if (s->kr + amount + LIVE_TRADE_COST > s->funds) return TRADE_ERR_FUNDS; // KN: the brain refuses it at the settle anyway
    s->kr += amount;
    changed(t);
    return TRADE_OK;
}

TradeError trade_session_clear_kr(TradeSession *t, const int side) {
    if (!editable(t) || side < 0 || side > 1) return TRADE_ERR_STATE;
    if (!t->side[side].kr) return TRADE_ERR_NOT_OFFERED;
    t->side[side].kr = 0;
    changed(t);
    return TRADE_OK;
}

TradeError trade_session_accept(TradeSession *t, const int side, int *both) {
    if (both) *both = 0;
    if (t->state != TRADE_OPEN || side < 0 || side > 1) return TRADE_ERR_STATE;
    TradeSide *s = &t->side[side];
    if (s->accepted) return TRADE_ERR_STATE; // the box is disabled once checked (:251416)
    // KN: errors.exchange.tradeInvalidOffer "Your trade offer is empty or invalid." (:117355)
    if (!t->side[0].offer_count && !t->side[1].offer_count && !t->side[0].kr && !t->side[1].kr) return TRADE_ERR_EMPTY;
    if (s->kr + LIVE_TRADE_COST > s->funds) return TRADE_ERR_FUNDS; // "Accept Offer - 10 KR"
    s->accepted = 1;
    if (t->side[0].accepted && t->side[1].accepted) {
        t->state = TRADE_COUNTDOWN;
        t->countdown_n = LIVE_TRADE_COUNTDOWN + 1;
        t->countdown = 0.0f; // the first "Loading n" goes out at the next tick
        if (both) *both = 1;
    }
    return TRADE_OK;
}

int trade_session_tick(TradeSession *t, const float dt) {
    if (t->state != TRADE_COUNTDOWN) return -1;
    t->countdown -= dt;
    if (t->countdown > 0.0f) return -1;
    t->countdown += 1.0f;
    if (t->countdown < 0.0f) t->countdown = 1.0f; // a long stall does not skip steps
    if (--t->countdown_n <= 0) {
        t->state = TRADE_SETTLING;
        t->countdown_n = 0;
        return 0;
    }
    return t->countdown_n;
}

char *trade_session_offer_json(const TradeSession *t, const int side) {
    const TradeSide *s = &t->side[side];
    const size_t cap = 4 + (size_t) s->offer_count * 48;
    char *out = malloc(cap);
    if (!out) return NULL;
    size_t len = 0;
    out[len++] = '[';
    for (int i = 0; i < s->offer_count; i++) {
        const TradeOffer *o = &s->offer[i];
        int n;
        if (o->copy) n = snprintf(out + len, cap - len, "%s[%d,1,%lld]", i ? "," : "", (int) o->skin, (long long) o->copy);
        else n = snprintf(out + len, cap - len, "%s[%d,%d]", i ? "," : "", (int) o->skin, (int) o->count);
        if (n < 0 || (size_t) n >= cap - len) break;
        len += (size_t) n;
    }
    out[len++] = ']';
    out[len] = 0;
    return out;
}

const char *trade_error_text(const TradeError e, const int limit) {
    static char buf[64];
    switch (e) {
        case TRADE_OK: return "";
        case TRADE_ERR_NOT_OWNED: return "Item not available";
        case TRADE_ERR_LOCKED: return "Item cant be traded";
        case TRADE_ERR_FULL:
            snprintf(buf, sizeof(buf), "%d items per user", limit); // v7 / the offers' "N items per user"
            return buf;
        case TRADE_ERR_NOT_OFFERED: return "Item not in the trade";
        case TRADE_ERR_FUNDS: return "Not enough KR";
        case TRADE_ERR_AMOUNT: return "Invalid amount"; // addCurrency's own message (:251442)
        case TRADE_ERR_CURRENCY: return "Only KR can be traded";
        case TRADE_ERR_EMPTY: return "Your trade offer is empty or invalid."; // errors.exchange.tradeInvalidOffer (:117355)
        case TRADE_ERR_STATE:
        default: return "Not now";
    }
}
