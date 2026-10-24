#pragma once
// The live trade of Krunker v10.0.3's Trade mode (doc/v10/trade.md): the rules both ends share and
// the bodies of the NET_EXT_TRADE_* subtypes (shared/include/net_ext.h, block 0x0b00). The server
// runs the session (server/src/trade.c, server/src/trade_session.c), the brain moves the items
// (services/brain: the "live_trade" op), the client draws windows[36] (client/src/ui/trade_ui.c),
// and client/src/net/krunker_link.c translates the official wire (the v10 names in the comments).
// Every body is little-endian and packed.
#include <stdint.h>

// ---- v10.0.3 config / mode values
#define LIVE_TRADE_COST 10              // config.tradeCost (deob-10.0.3.js:22952): "Accept Offer - 10 KR", each side
#define LIVE_TRADE_MAX_OUT 5            // config.tradeMaxOut (:22953): units per side ("0/5")
#define LIVE_TRADE_MAX_OUT_PREMIUM 20   // config.tradeMaxOutP (:22954): the side's owner is premium ("0/20")
#define LIVE_TRADE_MIN_LEVEL 10         // config.tradeMinLvl (:22955): "Both players must be level 10 or higher to trade."
#define LIVE_TRADE_REQUEST_COOLDOWN 3.0f // config.interactTradeTimer 3000 ms (:22627): one request per 3 s (server)
#define LIVE_TRADE_RANGE 13.2f          // the prompt's reach, eye to eye (:192470)
#define LIVE_TRADE_EYE 5.5f             // ... the eye: y + 5.5 on both players (:192470)
#define LIVE_TRADE_PROMPT_PERIOD 0.5f   // the prompt scan runs every 500 ms (:192467)
#define LIVE_TRADE_CHAT_MAX 70          // #tradeChatInput maxlength (:251202)
#define LIVE_TRADE_CHAT_KEEP 35         // #tradeChatList keeps 35 lines (:259552)
// ---- KrunkNative choices (doc/v10/trade.md section 3)
#define LIVE_TRADE_REQUEST_TTL 30.0f    // a request can be accepted for 30 s
#define LIVE_TRADE_OPEN_TIMEOUT 15.0f   // the inventories must arrive within 15 s
#define LIVE_TRADE_COUNTDOWN 3          // both accepted: "Loading 3", 2, 1 (process(n)), then the brain settles
#define LIVE_TRADE_KR_MAX 10000000      // KR per side (config.giftMax, :22606)
#define LIVE_TRADE_RANGE_SLACK 6.0f     // the server's reach on top of LIVE_TRADE_RANGE (its positions lag the client's)

// ---- the windows[36] actions
enum {
    LIVE_TRADE_ITEM_REMOVE = 0,  // utrd action 0: one unit of mine off my side (windows[36].remove, :251386)
    LIVE_TRADE_ITEM_ADD = 1,     // 1: one unit of mine onto my side (windows[36].add, :251284)
    LIVE_TRADE_ITEM_REQUEST = 2, // 2: one unit of theirs onto their side (windows[36].request, :251270)
};
enum {
    LIVE_TRADE_CUR_ADD = 1,      // utrdc action 1: KR onto my side (addCurrency, :251439)
    LIVE_TRADE_CUR_REMOVE = 2,   // 2: a currency off my side (removeCurrency, :251463)
    LIVE_TRADE_CUR_REQUEST = 3,  // 3: KR onto their side (requestCurrency, :251451)
};
enum {
    LIVE_TRADE_CURRENCY_KR = 1,   // "Krunkies" / KR (:251473)
    LIVE_TRADE_CURRENCY_JUNK = 2, // "Junk" / JNK: v10 has the name, the window never sends it (KN refuses it)
    LIVE_TRADE_CURRENCY_RP = 3,   // "Ranked Points" / RP: same
};
// TRADE_RESPONSE "trd" [msg, sid, type, invs] types (iîîíiîî, :253238-253267)
enum {
    LIVE_TRADE_RES_REQUEST = 0,  // sid wants to trade: the chat line "NAME wants to trade [Accept]"
    LIVE_TRADE_RES_OPEN = 1,     // the window opens (NET_EXT_TRADE_OPEN carries it on our wire)
    LIVE_TRADE_RES_CANCELED = 2, // windows[36].canceled(): the window shuts
    LIVE_TRADE_RES_ACCEPTED = 3, // sid accepted: READY
    LIVE_TRADE_RES_PROCESS = 4,  // windows[36].process(n): "Loading n" (n in uid)
    LIVE_TRADE_RES_RESET = 5,    // windows[36].updateButtons(): both READY cleared
    LIVE_TRADE_RES_MESSAGE = 255, // KrunkNative: only msg (a chat line); v10 sends msg with any type
};

#define NET_TRADE_MSG_MAX 160
#define NET_TRADE_NAME_MAX 32
#define NET_TRADE_COLOR_MAX 16

#pragma pack(push, 1)
// NET_EXT_TRADE_REQUEST (C -> S): requestTrade(id) -> "strd" [id] (:254074-254084), the player list's
// Trade Player button. uid = the player (an official player's sid; the link sends its socket id).
typedef struct {
    int32_t uid;
} NetTradeRequestPacket;

// NET_EXT_TRADE_INIT (C -> S): acceptRequest(sid) -> "trd" [sid, 1] (:253613-253615)
typedef struct {
    int32_t uid;
    uint8_t accept; // 1 (v10 never sends anything else)
} NetTradeInitPacket;

// NET_EXT_TRADE_UPDATE (C -> S): "utrd" [skin, action, copyId || 0] (LIVE_TRADE_ITEM_*)
typedef struct {
    int32_t skin;   // store.skins index
    uint8_t action; // LIVE_TRADE_ITEM_*
    int64_t copy;   // the copy id of a painted / instanced copy, 0 a stack's unit
} NetTradeUpdatePacket;

// NET_EXT_TRADE_CURRENCY (C -> S): "utrdc" [currency, action, amount] (LIVE_TRADE_CUR_*)
typedef struct {
    uint8_t currency; // LIVE_TRADE_CURRENCY_*
    uint8_t action;   // LIVE_TRADE_CUR_*
    int32_t amount;   // > 0 for add / request, 0 for remove
} NetTradeCurrencyPacket;

// NET_EXT_TRADE_RESPONSE (S -> C): "trd" [msg, sid, type] for every type but 1. msg (NUL-terminated,
// "" none) is a chat line (iííîiíí(-1, null, msg, 2)); the body may stop after its NUL.
typedef struct {
    uint8_t type;                // LIVE_TRADE_RES_*
    int32_t uid;                 // the other player (type 4: n)
    char msg[NET_TRADE_MSG_MAX]; // NUL-terminated
} NetTradeResponsePacket;

// NET_EXT_TRADE_OPEN / NET_EXT_TRADE_RESULT (S -> C): one JSON array, sliced over consecutive EXTs of
// the subtype (part 0..parts-1 in order). OPEN: uid = the trader and the JSON is v10's invs
// [mine, theirs], items {i, c[, id, st, m]} (:253244-253253, :251121-251146). RESULT: uid 0, the JSON is
// v10's "atrd" [error, funds, removed, added, meta] (:253269-253307): removed / added "skin" or
// "skin:copyId", meta {copyId: {paint, paintShader}}.
typedef struct {
    uint16_t part;
    uint16_t parts;
    int32_t uid;
} NetTradeChunkHeader;

// NET_EXT_TRADE_ITEM (S -> C): "utrd" [ownerId, add, skin, copyId] (iïíîiìî, :253308-253315): one unit
// on the owner's side added / removed; the window's READY marks clear (updateButtons)
typedef struct {
    int32_t owner_uid;
    uint8_t add;
    int32_t skin;
    int64_t copy;
} NetTradeItemPacket;

// NET_EXT_TRADE_CURRENCY_STATE (S -> C): "utrdc" [playerId, action, currency, amount] (iîiììíì,
// :253316-253323): action 1 adds amount to the owner's currency, 2 removes the currency
typedef struct {
    int32_t owner_uid;
    uint8_t action;   // LIVE_TRADE_CUR_ADD / LIVE_TRADE_CUR_REMOVE
    uint8_t currency; // LIVE_TRADE_CURRENCY_*
    int32_t amount;
} NetTradeCurrencyStatePacket;

// NET_EXT_TRADE_CHAT_LINE (S -> C): "tch" [name, msg, type, color] (iiìîïii, :259539-259561): type 2 a
// system line in color (#fc03ec default), else "name: msg"
typedef struct {
    uint8_t type;
    char color[NET_TRADE_COLOR_MAX];
    char name[NET_TRADE_NAME_MAX];
    char msg[NET_TRADE_MSG_MAX];
} NetTradeChatLinePacket;
#pragma pack(pop)

#define NET_TRADE_CHUNK_DATA (510 - (int) sizeof(NetTradeChunkHeader)) // NET_EXT_BODY_MAX - the header
_Static_assert(sizeof(NetTradeUpdatePacket) == 13, "NetTradeUpdatePacket: 13 bytes");
_Static_assert(sizeof(NetTradeItemPacket) == 17, "NetTradeItemPacket: 17 bytes");
_Static_assert(sizeof(NetTradeChatLinePacket) <= 510, "NetTradeChatLinePacket exceeds NET_EXT_BODY_MAX");
