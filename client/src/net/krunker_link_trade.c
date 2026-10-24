// Official lobbies: the live trade of Trade mode (doc/v10/trade.md, shared/include/live_trade.h). Krunker's packets
// -> our EXTs (client/src/ui/trade_ui.c draws them) and back. The KrunkNative uid of an official player is its sid.
//   trd   [msg, sid, type, invs]           NET_EXT_TRADE_RESPONSE / type 1 NET_EXT_TRADE_OPEN (iîîíiîî, deob-10.0.3.js:253238)
//   utrd  [ownerId, add, skin, copyId]     NET_EXT_TRADE_ITEM (iïíîiìî :253308; ownerId is a socket id, :251299)
//   utrdc [playerId, action, cur, amount]  NET_EXT_TRADE_CURRENCY_STATE (iîiììíì :253316)
//   atrd  [error, funds, removed, added, meta] NET_EXT_TRADE_RESULT (iîïîiîì :253269)
//   tch   [name, msg, type, color]         NET_EXT_TRADE_CHAT_LINE (iiìîïii :259539)
// Outbound: NET_EXT_TRADE_REQUEST -> ["strd", socketId] (requestTrade :254081), _INIT -> ["trd", sid, 1] (:253614),
// _UPDATE -> ["utrd", skin, action, copyId] (:251280), _CURRENCY -> ["utrdc", cur, action, amount] (:251446),
// _ACCEPT / _DECLINE / _CANCEL -> ["atrd"] / ["dtrd"] / ["ctrd"], _CHAT -> ["tct", text] (:260033). The prompt's
// "int" [5, sid] is krunker_link.c's NET_PACKET_INTERACT.
#include "krunker_link_internal.h"
#include <live_trade.h>
#include <net_ext.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static double num(const KrunkerValue *v, const double def) { return krunker_num(v, def); }
static int is_nil(const KrunkerValue *v) { return !v || v->type == KRUNKER_NIL; }
static int truthy(const KrunkerValue *v) {
    if (is_nil(v)) return 0;
    if (v->type == KRUNKER_BOOL) return v->u.boolean;
    if (v->type == KRUNKER_NUM) return v->u.num != 0;
    return 1;
}

static void emit_ext(KrunkerLink *l, const uint16_t subtype, const void *body, const uint16_t length) {
    uint8_t buffer[NET_EXT_HEADER_SIZE + NET_EXT_BODY_MAX];
    const uint16_t n = net_ext_pack(buffer, sizeof(buffer), subtype, body, length);
    if (n) krunker_link_emit(l, NET_PACKET_EXT, buffer, n);
}

// a player by its socket id ("0"'s [0]); a number is a sid already. -1 unknown.
static int sid_of(const KrunkerLink *l, const KrunkerValue *v) {
    if (krunker_is(v, KRUNKER_NUM)) return (int) v->u.num;
    const char *id = krunker_str(v);
    for (int i = 0; id && id[0] && i < l->player_count; i++) {
        if (!strcmp(l->players[i].id, id)) return l->players[i].sid;
    }
    return -1;
}

// one JSON array over NET_TRADE_CHUNK_DATA-sized EXTs (NetTradeChunkHeader)
static void emit_chunks(KrunkerLink *l, const uint16_t subtype, const int uid, const char *json, const size_t len) {
    const size_t parts = len ? (len + NET_TRADE_CHUNK_DATA - 1) / NET_TRADE_CHUNK_DATA : 1;
    if (parts > 0xffff) return;
    uint8_t body[NET_EXT_BODY_MAX];
    for (size_t p = 0; p < parts; p++) {
        const NetTradeChunkHeader h = {.part = (uint16_t) p, .parts = (uint16_t) parts, .uid = uid};
        const size_t off = p * NET_TRADE_CHUNK_DATA;
        const size_t n = len - off < (size_t) NET_TRADE_CHUNK_DATA ? len - off : (size_t) NET_TRADE_CHUNK_DATA;
        memcpy(body, &h, sizeof(h));
        if (n) memcpy(body + sizeof(h), json + off, n);
        emit_ext(l, subtype, body, (uint16_t) (sizeof(h) + n));
    }
}

static void emit_json(KrunkerLink *l, const uint16_t subtype, const int uid, const KrunkerValue *v) {
    KrunkerBuf buf = {0};
    if (krunker_value_to_json(v, &buf) == KRUNKER_PROTO_OK && buf.data) emit_chunks(l, subtype, uid, (const char *) buf.data, buf.len);
    else emit_chunks(l, subtype, uid, "[]", 2);
    krunker_buf_free(&buf);
}

static void copy_str(char *out, const size_t size, const KrunkerValue *v) {
    const char *s = krunker_str(v);
    snprintf(out, size, "%s", s ? s : "");
}

// the frame's arguments from `from` on as one array value
static void args_array(const KrunkerFrame *f, const uint32_t from, KrunkerValue *out) {
    *out = KRUNKER_V_ARRAY(f->argc > from ? &f->args[from] : NULL, f->argc > from ? f->argc - from : 0);
}

int krunker_link_trade_frame(KrunkerLink *l, const KrunkerS2C id, const KrunkerFrame *f) {
    switch (id) {
        case KRUNKER_S2C_TRADE_RESPONSE: { // "trd" [msg, sid, type, invs]
            const int type = (int) num(krunker_frame_arg(f, 2), -1);
            const int uid = type == LIVE_TRADE_RES_PROCESS ? (int) num(krunker_frame_arg(f, 1), 0) : sid_of(l, krunker_frame_arg(f, 1));
            if (type == LIVE_TRADE_RES_OPEN) {
                emit_json(l, NET_EXT_TRADE_OPEN, uid, krunker_frame_arg(f, 3));
                const char *msg = krunker_str(krunker_frame_arg(f, 0));
                if (!msg || !msg[0]) return 1;
            }
            NetTradeResponsePacket r;
            memset(&r, 0, sizeof(r));
            r.type = (uint8_t) (type == LIVE_TRADE_RES_OPEN || type < 0 || type > 5 ? LIVE_TRADE_RES_MESSAGE : type);
            r.uid = uid;
            copy_str(r.msg, sizeof(r.msg), krunker_frame_arg(f, 0));
            emit_ext(l, NET_EXT_TRADE_RESPONSE, &r, (uint16_t) (offsetof(NetTradeResponsePacket, msg) + strlen(r.msg) + 1));
            return 1;
        }
        case KRUNKER_S2C_UPDATE_TRADE_RESPONSE: { // "utrd" [ownerId, add, skin, copyId]
            const NetTradeItemPacket p = {.owner_uid = sid_of(l, krunker_frame_arg(f, 0)), .add = (uint8_t) truthy(krunker_frame_arg(f, 1)),
                                          .skin = (int32_t) num(krunker_frame_arg(f, 2), -1),
                                          .copy = (int64_t) num(krunker_frame_arg(f, 3), 0)};
            emit_ext(l, NET_EXT_TRADE_ITEM, &p, sizeof(p));
            return 1;
        }
        case KRUNKER_S2C_UPDATE_TRADE_CURRENCY_RESPONSE: { // "utrdc" [playerId, action, currency, amount]
            const NetTradeCurrencyStatePacket p = {.owner_uid = sid_of(l, krunker_frame_arg(f, 0)),
                                                   .action = (uint8_t) num(krunker_frame_arg(f, 1), 0),
                                                   .currency = (uint8_t) num(krunker_frame_arg(f, 2), 0),
                                                   .amount = (int32_t) num(krunker_frame_arg(f, 3), 0)};
            emit_ext(l, NET_EXT_TRADE_CURRENCY_STATE, &p, sizeof(p));
            return 1;
        }
        case KRUNKER_S2C_TRADE_A_RESPONSE: { // "atrd" [error, funds, removed, added, meta]
            KrunkerValue all;
            args_array(f, 0, &all);
            emit_json(l, NET_EXT_TRADE_RESULT, 0, &all);
            return 1;
        }
        case KRUNKER_S2C_ADD_TRADE_CHAT: { // "tch" [name, msg, type, color]
            NetTradeChatLinePacket p;
            memset(&p, 0, sizeof(p));
            copy_str(p.name, sizeof(p.name), krunker_frame_arg(f, 0));
            copy_str(p.msg, sizeof(p.msg), krunker_frame_arg(f, 1));
            p.type = (uint8_t) num(krunker_frame_arg(f, 2), 0);
            copy_str(p.color, sizeof(p.color), krunker_frame_arg(f, 3));
            emit_ext(l, NET_EXT_TRADE_CHAT_LINE, &p, sizeof(p));
            return 1;
        }
        default: return 0;
    }
}

// "ch" [team, name, sid, 3] (addChat type 3, :259629): a trade request as a chat line
void krunker_link_trade_request_line(KrunkerLink *l, const KrunkerValue *sid) {
    NetTradeResponsePacket r;
    memset(&r, 0, sizeof(r));
    r.type = LIVE_TRADE_RES_REQUEST;
    r.uid = sid_of(l, sid);
    if (r.uid < 0) return;
    emit_ext(l, NET_EXT_TRADE_RESPONSE, &r, (uint16_t) (offsetof(NetTradeResponsePacket, msg) + 1));
}

static int send_wire(KrunkerLink *l, const KrunkerC2S id, const KrunkerValue *args, const int argc) {
    return krunker_link_send(l, krunker_c2s_wire(id), args, argc);
}

int krunker_link_trade_send(KrunkerLink *l, const uint16_t subtype, const uint8_t *body, const uint16_t length) {
    switch (subtype) {
        case NET_EXT_TRADE_REQUEST: { // requestTrade(id): ["strd", player.id] (its socket id)
            if (length < sizeof(NetTradeRequestPacket)) return 0;
            NetTradeRequestPacket r;
            memcpy(&r, body, sizeof(r));
            for (int i = 0; i < l->player_count; i++) {
                if (l->players[i].sid != r.uid || !l->players[i].id[0]) continue;
                const KrunkerValue args[] = {KRUNKER_V_STR(l->players[i].id)};
                return send_wire(l, KRUNKER_C2S_SEND_TRADE_REQUEST, args, 1);
            }
            return 0;
        }
        case NET_EXT_TRADE_INIT: { // acceptRequest(sid): ["trd", sid, 1]
            if (length < sizeof(NetTradeInitPacket)) return 0;
            NetTradeInitPacket r;
            memcpy(&r, body, sizeof(r));
            const KrunkerValue args[] = {KRUNKER_V_NUM(r.uid), KRUNKER_V_NUM(r.accept)};
            return send_wire(l, KRUNKER_C2S_INIT_TRADE, args, 2);
        }
        case NET_EXT_TRADE_UPDATE: { // ["utrd", skin, action, copyId || 0]
            if (length < sizeof(NetTradeUpdatePacket)) return 0;
            NetTradeUpdatePacket u;
            memcpy(&u, body, sizeof(u));
            const KrunkerValue args[] = {KRUNKER_V_NUM(u.skin), KRUNKER_V_NUM(u.action), KRUNKER_V_NUM((double) u.copy)};
            return send_wire(l, KRUNKER_C2S_TRADE_UPDATE, args, 3);
        }
        case NET_EXT_TRADE_CURRENCY: { // ["utrdc", currency, action, amount]
            if (length < sizeof(NetTradeCurrencyPacket)) return 0;
            NetTradeCurrencyPacket c;
            memcpy(&c, body, sizeof(c));
            const KrunkerValue args[] = {KRUNKER_V_NUM(c.currency), KRUNKER_V_NUM(c.action), KRUNKER_V_NUM(c.amount)};
            return send_wire(l, KRUNKER_C2S_TRADE_CURRENCY_UPDATE, args, 3);
        }
        case NET_EXT_TRADE_ACCEPT: return send_wire(l, KRUNKER_C2S_TRADE_ACCEPT, NULL, 0);
        case NET_EXT_TRADE_DECLINE: return send_wire(l, KRUNKER_C2S_TRADE_DECLINE, NULL, 0);
        case NET_EXT_TRADE_CANCEL: return send_wire(l, KRUNKER_C2S_TRADE_CANCEL, NULL, 0);
        case NET_EXT_TRADE_CHAT: { // ["tct", tradeChatInput.value]
            char text[LIVE_TRADE_CHAT_MAX * 4 + 1];
            const size_t n = length < sizeof(text) - 1 ? length : sizeof(text) - 1;
            memcpy(text, body, n);
            text[n] = 0;
            if (!text[0]) return 0;
            const KrunkerValue args[] = {KRUNKER_V_STR(text)};
            return send_wire(l, KRUNKER_C2S_TRADE_CHAT, args, 1);
        }
        default: return -1; // not a trade subtype
    }
}
