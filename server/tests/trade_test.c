// The live trade of Trade mode (doc/v10/trade.md; server/src/trade_session.c, server/src/trade.c).
//   1. the state machine on its own: side limits (5 / 20 premium units), ownership of stacks and copies,
//      locked items, remove, KR against funds + tradeCost, accept / both / the countdown, any change
//      clearing READY, nothing editable while settling, the brain's offer JSON
//   2. the server flow over the offline sandbox (raw protocol 1.4 clients, the real EXT dispatch) in
//      Trade mode, the brain faked through trade_set_post_hook: the level gate, "strd", the request
//      cooldown, "trd" [sid, 1], the "trade_inventories" op, OPEN to both, utrd / utrdc relays and
//      refusals, atrd READY, "Loading 3..1", the "live_trade" op (key, sides, cost), the result to
//      both and skins_owned; a failed settle; cancel; decline; a leave
// Run from the repository root: ./<build>/trade_test
#include <server_sandbox.h>
#include <server.h>
#include <server_trade.h>
#include <server_accounts.h>
#include <trade_session.h>
#include <live_trade.h>
#include <shared.h>
#include <net_ext.h>
#include <modes.h>
#include <map_catalog.h>
#include <knet.h>
#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;
static int g_checks;

#define CHECK(cond, ...) do { \
    g_checks++; \
    if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_failures++; } \
    else { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

// ------------------------------------------------------------------ 1. the state machine

static TradeInvItem *inv(const TradeInvItem *rows, const int n) {
    TradeInvItem *out = malloc(sizeof(TradeInvItem) * (size_t) n);
    memcpy(out, rows, sizeof(TradeInvItem) * (size_t) n);
    return out;
}

static void session_tests(void) {
    TradeSession t;
    trade_session_init(&t, 1, 10, 1001, 11, 1002, 0.0f);
    CHECK(t.state == TRADE_OPENING && trade_session_add(&t, 0, 100, 0) == TRADE_ERR_STATE, "opening: nothing can be added yet");
    const TradeInvItem a[] = {{.skin = 100, .count = 2}, {.skin = 101, .count = 9}, {.skin = 200, .count = 1, .copy = 1000005},
                              {.skin = 300, .count = 1, .locked = 1}};
    const TradeInvItem b[] = {{.skin = 400, .count = 30}};
    trade_session_set_inventory(&t, 0, inv(a, 4), 4, 105, 0);
    trade_session_set_inventory(&t, 1, inv(b, 1), 1, 1000, 1);
    trade_session_open(&t, 0.0f);
    CHECK(t.state == TRADE_OPEN, "open");
    CHECK(trade_session_side_limit(&t, 0) == LIVE_TRADE_MAX_OUT && trade_session_side_limit(&t, 1) == LIVE_TRADE_MAX_OUT_PREMIUM,
          "limits: 5 units, 20 for a premium side (tradeMaxOut / tradeMaxOutP)");

    CHECK(trade_session_add(&t, 0, 100, 0) == TRADE_OK && trade_session_add(&t, 0, 100, 0) == TRADE_OK, "two units of a 2-stack");
    CHECK(trade_session_add(&t, 0, 100, 0) == TRADE_ERR_NOT_OWNED, "a third unit of the 2-stack is refused");
    CHECK(trade_session_add(&t, 0, 200, 1000005) == TRADE_OK, "the painted copy");
    CHECK(trade_session_add(&t, 0, 200, 1000005) == TRADE_ERR_NOT_OWNED, "the same copy twice is refused");
    CHECK(trade_session_add(&t, 0, 200, 0) == TRADE_ERR_NOT_OWNED, "skin 200 has no stack units (only the copy)");
    CHECK(trade_session_add(&t, 0, 200, 1000006) == TRADE_ERR_NOT_OWNED, "another player's copy id is refused");
    CHECK(trade_session_add(&t, 0, 300, 0) == TRADE_ERR_LOCKED, "a locked (untradable) item is refused");
    CHECK(trade_session_add(&t, 0, 999, 0) == TRADE_ERR_NOT_OWNED, "an item not in the inventory is refused");
    CHECK(trade_session_add(&t, 0, 101, 0) == TRADE_OK && trade_session_add(&t, 0, 101, 0) == TRADE_OK, "units 4 and 5");
    CHECK(trade_session_side_units(&t, 0) == 5 && trade_session_add(&t, 0, 101, 0) == TRADE_ERR_FULL, "a 6th unit is refused (5 per side)");
    for (int i = 0; i < LIVE_TRADE_MAX_OUT_PREMIUM; i++) trade_session_add(&t, 1, 400, 0);
    CHECK(trade_session_side_units(&t, 1) == 20 && trade_session_add(&t, 1, 400, 0) == TRADE_ERR_FULL, "a premium side holds 20");
    CHECK(trade_session_remove(&t, 1, 400, 0) == TRADE_OK && trade_session_side_units(&t, 1) == 19, "remove takes one unit off");
    CHECK(trade_session_remove(&t, 1, 401, 0) == TRADE_ERR_NOT_OFFERED, "removing what is not on the side is refused");

    CHECK(trade_session_add_kr(&t, 0, 100) == TRADE_ERR_FUNDS, "100 KR + the 10 KR cost exceed 105 funds");
    CHECK(trade_session_add_kr(&t, 0, 0) == TRADE_ERR_AMOUNT && trade_session_add_kr(&t, 0, -5) == TRADE_ERR_AMOUNT, "0 / negative KR refused");
    CHECK(trade_session_add_kr(&t, 0, 95) == TRADE_OK && t.side[0].kr == 95, "95 KR fits (95 + 10 = 105)");
    CHECK(trade_session_add_kr(&t, 0, 1) == TRADE_ERR_FUNDS, "one more KR does not");
    CHECK(trade_session_clear_kr(&t, 0) == TRADE_OK && t.side[0].kr == 0, "KR off the side");
    CHECK(trade_session_add_kr(&t, 1, LIVE_TRADE_KR_MAX + 1) == TRADE_ERR_AMOUNT, "more than 1e7 KR refused");

    int both = 0;
    CHECK(trade_session_accept(&t, 0, &both) == TRADE_OK && !both && t.side[0].accepted, "A accepts");
    CHECK(trade_session_accept(&t, 0, &both) == TRADE_ERR_STATE, "a second accept does nothing");
    CHECK(trade_session_remove(&t, 0, 101, 0) == TRADE_OK && !t.side[0].accepted, "a change clears A's READY");
    CHECK(trade_session_accept(&t, 0, &both) == TRADE_OK && trade_session_accept(&t, 1, &both) == TRADE_OK && both &&
              t.state == TRADE_COUNTDOWN, "both accept: the countdown");
    CHECK(trade_session_tick(&t, 0.016f) == 3, "Loading 3 at once");
    CHECK(trade_session_tick(&t, 0.5f) == -1 && trade_session_tick(&t, 0.5f) == 2, "Loading 2 a second later");
    CHECK(trade_session_add(&t, 1, 400, 0) == TRADE_OK && t.state == TRADE_OPEN && !t.side[0].accepted && !t.side[1].accepted,
          "a change during the countdown stops it and clears both READY marks");
    CHECK(trade_session_tick(&t, 2.0f) == -1, "no countdown while open");
    trade_session_accept(&t, 0, &both);
    trade_session_accept(&t, 1, &both);
    CHECK(trade_session_tick(&t, 0.01f) == 3 && trade_session_tick(&t, 1.0f) == 2 && trade_session_tick(&t, 1.0f) == 1, "3, 2, 1");
    CHECK(trade_session_tick(&t, 1.0f) == 0 && t.state == TRADE_SETTLING, "then the settle");
    CHECK(trade_session_add(&t, 0, 101, 0) == TRADE_ERR_STATE && trade_session_add_kr(&t, 1, 5) == TRADE_ERR_STATE &&
              trade_session_remove(&t, 0, 100, 0) == TRADE_ERR_STATE, "nothing changes while settling");
    char *ja = trade_session_offer_json(&t, 0), *jb = trade_session_offer_json(&t, 1);
    CHECK(ja && strstr(ja, "[100,2]") && strstr(ja, "[200,1,1000005]") && strstr(ja, "[101,1]"), "the brain's offer JSON: %s", ja ? ja : "");
    CHECK(jb && !strcmp(jb, "[[400,20]]"), "the other side: %s", jb ? jb : "");
    free(ja);
    free(jb);
    trade_session_free(&t);

    TradeSession e;
    trade_session_init(&e, 2, 1, 1, 2, 2, 0.0f);
    trade_session_set_inventory(&e, 0, NULL, 0, 100, 0);
    trade_session_set_inventory(&e, 1, NULL, 0, 5, 0);
    trade_session_open(&e, 0.0f);
    CHECK(trade_session_accept(&e, 0, &both) == TRADE_ERR_EMPTY, "an empty trade cannot be accepted");
    trade_session_add_kr(&e, 0, 50);
    CHECK(trade_session_accept(&e, 1, &both) == TRADE_ERR_FUNDS, "a side without the 10 KR cost cannot accept (5 funds)");
    trade_session_free(&e);
}

// ------------------------------------------------------------------ 2. the server flow

#define MAX_RECEIVED 4096
#define CLIENTS 3

typedef struct {
    int fd;
    int closed;
    NetPacketBuffer buffer;
    NetPacket packets[MAX_RECEIVED];
    int count;
} TestClient;

static TestClient g_clients[CLIENTS];
static cJSON *g_frames[64];
static int g_frame_count;
static int g_post_fails;

static int fake_post(cJSON *frame, const int reliable) {
    if (g_post_fails) {
        cJSON_Delete(frame);
        return -1;
    }
    cJSON_AddBoolToObject(frame, "reliable", reliable != 0);
    if (g_frame_count < 64) g_frames[g_frame_count++] = frame;
    else cJSON_Delete(frame);
    return 0;
}

static void frames_clear(void) {
    for (int i = 0; i < g_frame_count; i++) cJSON_Delete(g_frames[i]);
    g_frame_count = 0;
}

static const cJSON *last_frame(const char *op) {
    for (int i = g_frame_count - 1; i >= 0; i--) {
        if (!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(g_frames[i], "op")), op)) return g_frames[i];
    }
    return NULL;
}

static void reply(const cJSON *frame, const char *err, const char *data_json) {
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "type", "acct-r");
    cJSON_AddNumberToObject(r, "rid", cJSON_GetObjectItem(frame, "rid")->valuedouble);
    if (err) cJSON_AddStringToObject(r, "err", err);
    else cJSON_AddItemToObject(r, "data", cJSON_Parse(data_json));
    CHECK(trade_on_brain_frame(r) == 1, "the reply is the trade's (%s)", cJSON_GetStringValue(cJSON_GetObjectItem(frame, "op")));
    cJSON_Delete(r);
}

static void send_to(TestClient *c, const int type, const void *payload, const uint16_t length) {
    if (c->fd < NET_LOOPBACK_FD_BASE) return;
    net_send_packet(c->fd, (NetPacketType) type, payload, length);
}

static void send_ext_to(TestClient *c, const uint16_t subtype, const void *body, const uint16_t length) {
    uint8_t buf[NET_EXT_HEADER_SIZE + NET_EXT_BODY_MAX];
    const uint16_t n = net_ext_pack(buf, sizeof(buf), subtype, body, length);
    send_to(c, NET_PACKET_EXT, buf, n);
}

static void drain(TestClient *c) {
    uint8_t bytes[4096];
    int n;
    while ((n = sandbox_receive(c->fd, bytes, sizeof(bytes))) > 0) {
        if (!net_buffer_push(&c->buffer, bytes, (size_t) n)) break;
        NetPacket packet;
        while (net_buffer_next(&c->buffer, &packet) > 0) {
            if (packet.type == NET_PACKET_PING && packet.length == sizeof(NetPingPacket)) send_to(c, NET_PACKET_PONG, packet.payload, packet.length);
            if (c->count < MAX_RECEIVED) c->packets[c->count++] = packet;
        }
    }
    if (n < 0) c->closed = 1;
}

static void pump(const int frames) {
    for (int f = 0; f < frames; f++) {
        sandbox_step(1.0 / 60.0);
        for (int i = 0; i < CLIENTS; i++) {
            if (g_clients[i].fd >= NET_LOOPBACK_FD_BASE && !g_clients[i].closed) drain(&g_clients[i]);
        }
    }
}

static void reset_received(void) {
    for (int i = 0; i < CLIENTS; i++) g_clients[i].count = 0;
}

static ClientConnection *conn(const TestClient *c) {
    for (size_t i = 0; i < g_server->client_count; i++) {
        if (c->fd >= NET_LOOPBACK_FD_BASE && g_server->clients[i].fd == c->fd - 1) return &g_server->clients[i];
    }
    return NULL;
}

static int uid(const TestClient *c) {
    const ClientConnection *cc = conn(c);
    return cc ? cc->id : -1;
}

// the n-th EXT of `subtype` (body copied into out, its length returned), -1 none
static int ext_nth(const TestClient *c, const uint16_t subtype, const int nth, void *out, const size_t size) {
    int seen = 0;
    for (int i = 0; i < c->count; i++) {
        if (c->packets[i].type != NET_PACKET_EXT) continue;
        uint16_t sub, bl;
        const uint8_t *body;
        if (!net_ext_unpack(c->packets[i].payload, c->packets[i].length, &sub, &body, &bl) || sub != subtype) continue;
        if (seen++ != nth) continue;
        memset(out, 0, size);
        memcpy(out, body, bl < size ? bl : size);
        return bl;
    }
    return -1;
}

static int ext_count(const TestClient *c, const uint16_t subtype) {
    char tmp[NET_EXT_BODY_MAX];
    int n = 0;
    while (ext_nth(c, subtype, n, tmp, sizeof(tmp)) >= 0) n++;
    return n;
}

// a response of `type` (any uid when uid < -1), its msg containing `text` (NULL: any)
static int got_response(const TestClient *c, const int type, const int who, const char *text) {
    NetTradeResponsePacket r;
    for (int i = 0; ext_nth(c, NET_EXT_TRADE_RESPONSE, i, &r, sizeof(r)) >= 0; i++) {
        r.msg[sizeof(r.msg) - 1] = 0;
        if (r.type == type && (who < -1 || r.uid == who) && (!text || strstr(r.msg, text))) return 1;
    }
    return 0;
}

// the JSON of the chunked subtype (the last record), NULL none; *uid_out its header uid
static cJSON *chunked_json(const TestClient *c, const uint16_t subtype, int *uid_out) {
    static char json[65536];
    size_t len = 0;
    uint8_t body[NET_EXT_BODY_MAX];
    int got = 0;
    for (int i = 0;; i++) {
        const int n = ext_nth(c, subtype, i, body, sizeof(body));
        if (n < 0) break;
        NetTradeChunkHeader h;
        memcpy(&h, body, sizeof(h));
        if (h.part == 0) len = 0;
        const size_t data = (size_t) n - sizeof(h);
        if (len + data < sizeof(json)) {
            memcpy(json + len, body + sizeof(h), data);
            len += data;
        }
        if (h.part + 1 == h.parts) {
            got = 1;
            if (uid_out) *uid_out = h.uid;
        }
    }
    if (!got) return NULL;
    json[len] = 0;
    return cJSON_Parse(json);
}

static void join(TestClient *c) {
    memset(c, 0, sizeof(*c));
    c->fd = sandbox_connect();
    const NetHelloPacket hello = {NET_PROTOCOL_MAJOR, NET_PROTOCOL_MINOR_EXT, 0, 0};
    send_to(c, NET_PACKET_HELLO, &hello, sizeof(hello));
    NetJoinPacket j;
    memset(&j, 0, sizeof(j));
    send_to(c, NET_PACKET_JOIN, &j, (uint16_t) NET_JOIN_PACKET_V2_SIZE);
    NetEntPacket ent = {.class_index = 0, .secondary_index = -1, .cosmetics = PLAYER_COSMETICS_NONE, .slid_cont = 1, .lag_comp = 1.0f};
    send_to(c, NET_PACKET_ENT, &ent, (uint16_t) NET_ENT_PACKET_V7_SIZE);
    pump(2);
}

static void give_account(TestClient *c, const int account_id, const int level, const char *name) {
    ClientConnection *cc = conn(c);
    if (!cc) return;
    cc->acct.account_id = account_id;
    cc->acct.level = level;
    cc->acct.join_state = SERVER_JOIN_READY;
    cc->local = 0;
    cc->op = 0;
    snprintf(cc->acct.username, sizeof(cc->acct.username), "%s", name);
    snprintf(cc->name, sizeof(cc->name), "%s", name);
    if (cc->player) snprintf(cc->player->name, sizeof(cc->player->name), "%s", name);
}

static void request(TestClient *from, const TestClient *to) {
    const NetTradeRequestPacket r = {.uid = uid(to)};
    send_ext_to(from, NET_EXT_TRADE_REQUEST, &r, sizeof(r));
}

static void accept_request(TestClient *c, const TestClient *from) {
    const NetTradeInitPacket r = {.uid = uid(from), .accept = 1};
    send_ext_to(c, NET_EXT_TRADE_INIT, &r, sizeof(r));
}

static void update(TestClient *c, const int skin, const int action, const int64_t copy) {
    const NetTradeUpdatePacket u = {.skin = skin, .action = (uint8_t) action, .copy = copy};
    send_ext_to(c, NET_EXT_TRADE_UPDATE, &u, sizeof(u));
}

static void currency(TestClient *c, const int action, const int amount) {
    const NetTradeCurrencyPacket u = {.currency = LIVE_TRADE_CURRENCY_KR, .action = (uint8_t) action, .amount = amount};
    send_ext_to(c, NET_EXT_TRADE_CURRENCY, &u, sizeof(u));
}

static void bare(TestClient *c, const uint16_t subtype) { send_ext_to(c, subtype, NULL, 0); }

static const char *k_inventories =
    "{\"a\":{\"inv\":[{\"i\":100,\"c\":2},{\"i\":200,\"c\":1,\"id\":1000005,\"m\":{\"paint\":\"#ff0000\"}},{\"i\":300,\"c\":1,\"nt\":1}],"
    "\"funds\":500,\"premium\":false,\"level\":30},"
    "\"b\":{\"inv\":[{\"i\":400,\"c\":1}],\"funds\":50,\"premium\":true,\"level\":20}}";

// A requests B, B accepts, the inventories arrive: an open session
static int open_session(TestClient *A, TestClient *B) {
    request(A, B);
    pump(3);
    accept_request(B, A);
    pump(3);
    const cJSON *f = last_frame("trade_inventories");
    if (!f) return 0;
    reply(f, NULL, k_inventories);
    pump(2);
    return conn(A) && trade_is_trading(conn(A));
}

static void flow_tests(void) {
    const cJSON *burg = map_catalog_json((size_t) map_catalog_index_of_key("311_burg"));
    static const cJSON *maps[1];
    maps[0] = burg;
    const SandboxConfig cfg = {.maps = maps, .map_count = 1, .map_index = 0, .mode_id = MODE_ID_TRADE};
    CHECK(burg && sandbox_start(&cfg), "sandbox Trade on Burg");
    CHECK(g_server->game.mode && g_server->game.mode->config.can_trade && g_server->game.mode->config.max_no_mov == 180.0f,
          "Trade mode: canTrade, maxNoMov 180 s");
    trade_set_post_hook(fake_post);
    for (int i = 0; i < CLIENTS; i++) join(&g_clients[i]);
    pump(30);
    TestClient *A = &g_clients[0], *B = &g_clients[1], *C = &g_clients[2];
    give_account(A, 101, 30, "Alice");
    give_account(B, 102, 20, "Bob");
    give_account(C, 103, 5, "Carol");
    reset_received();

    // the level gate
    request(C, A);
    pump(3);
    CHECK(got_response(C, LIVE_TRADE_RES_MESSAGE, -2, "level 10") && !got_response(A, LIVE_TRADE_RES_REQUEST, -2, NULL),
          "level 5 cannot trade (tradeMinLvl 10), nothing reaches the target");

    // strd -> the target's request line, the requester's confirmation
    request(A, B);
    pump(3);
    CHECK(got_response(B, LIVE_TRADE_RES_REQUEST, uid(A), NULL), "B gets trd type 0 from A (NAME wants to trade)");
    CHECK(got_response(A, LIVE_TRADE_RES_MESSAGE, -2, "Trade request sent to Bob"), "A: Trade request sent to Bob");
    reset_received();
    request(A, B);
    pump(3);
    CHECK(got_response(A, LIVE_TRADE_RES_MESSAGE, -2, "Please wait") && !got_response(B, LIVE_TRADE_RES_REQUEST, -2, NULL),
          "a second request within interactTradeTimer (3 s) is refused");
    accept_request(C, A);
    pump(3);
    CHECK(got_response(C, LIVE_TRADE_RES_MESSAGE, -2, "expired") && !trade_is_trading(conn(C)), "accepting a request nobody sent does nothing");

    // B accepts: the inventories op, then OPEN to both
    frames_clear();
    accept_request(B, A);
    pump(3);
    const cJSON *f = last_frame("trade_inventories");
    const cJSON *args = f ? cJSON_GetObjectItem(f, "args") : NULL;
    CHECK(f && cJSON_GetObjectItem(args, "a")->valueint == 101 && cJSON_GetObjectItem(args, "b")->valueint == 102,
          "trade_inventories {a: 101, b: 102}");
    CHECK(trade_is_trading(conn(A)) && trade_is_trading(conn(B)) && ext_count(A, NET_EXT_TRADE_OPEN) == 0, "opening: no window yet");
    reply(f, NULL, k_inventories);
    pump(2);
    int who = -1;
    cJSON *ia = chunked_json(A, NET_EXT_TRADE_OPEN, &who);
    CHECK(ia && who == uid(B) && cJSON_GetArraySize(ia) == 2 && cJSON_GetArraySize(cJSON_GetArrayItem(ia, 0)) == 3 &&
              cJSON_GetArraySize(cJSON_GetArrayItem(ia, 1)) == 1, "A's window: trader Bob, [mine (3), theirs (1)]");
    cJSON_Delete(ia);
    cJSON *ib = chunked_json(B, NET_EXT_TRADE_OPEN, &who);
    CHECK(ib && who == uid(A) && cJSON_GetArrayItem(cJSON_GetArrayItem(ib, 0), 0) &&
              cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetArrayItem(ib, 0), 0), "i")->valueint == 400, "B's window: [mine = 400, theirs]");
    cJSON_Delete(ib);
    reset_received();

    // utrd / utrdc
    update(A, 100, LIVE_TRADE_ITEM_ADD, 0);
    pump(2);
    NetTradeItemPacket item;
    CHECK(ext_nth(B, NET_EXT_TRADE_ITEM, 0, &item, sizeof(item)) >= 0 && item.owner_uid == uid(A) && item.add && item.skin == 100 &&
              ext_count(A, NET_EXT_TRADE_ITEM) == 1, "utrd add: both get [Alice, 1, 100, 0]");
    update(A, 300, LIVE_TRADE_ITEM_ADD, 0);
    update(A, 400, LIVE_TRADE_ITEM_REQUEST, 0);
    pump(2);
    CHECK(got_response(A, LIVE_TRADE_RES_MESSAGE, -2, "cant be traded"), "a locked item is refused");
    CHECK(ext_nth(B, NET_EXT_TRADE_ITEM, 1, &item, sizeof(item)) >= 0 && item.owner_uid == uid(B) && item.skin == 400,
          "utrd request: B's 400 onto B's side");
    update(A, 200, LIVE_TRADE_ITEM_ADD, 1000005);
    pump(2);
    CHECK(ext_nth(A, NET_EXT_TRADE_ITEM, 2, &item, sizeof(item)) >= 0 && item.copy == 1000005, "the painted copy goes by its id");
    currency(A, LIVE_TRADE_CUR_ADD, 100);
    currency(B, LIVE_TRADE_CUR_ADD, 50);
    pump(2);
    NetTradeCurrencyStatePacket cs;
    CHECK(ext_nth(B, NET_EXT_TRADE_CURRENCY_STATE, 0, &cs, sizeof(cs)) >= 0 && cs.owner_uid == uid(A) && cs.action == LIVE_TRADE_CUR_ADD &&
              cs.amount == 100 && ext_count(B, NET_EXT_TRADE_CURRENCY_STATE) == 1, "utrdc: A's 100 KR to both; B's 50 KR refused");
    CHECK(got_response(B, LIVE_TRADE_RES_MESSAGE, -2, "Not enough KR"), "B: 50 KR + 10 KR cost > 50 funds");
    NetTradeChatLinePacket line;
    const char hi[] = "hello there";
    send_ext_to(B, NET_EXT_TRADE_CHAT, hi, sizeof(hi));
    pump(2);
    CHECK(ext_nth(A, NET_EXT_TRADE_CHAT_LINE, 0, &line, sizeof(line)) >= 0 && !strcmp(line.name, "Bob") && !strcmp(line.msg, "hello there"),
          "tct -> tch [Bob, hello there] to both");
    reset_received();

    // atrd: READY, both -> Loading 3..1 -> live_trade
    frames_clear();
    bare(A, NET_EXT_TRADE_ACCEPT);
    pump(2);
    CHECK(got_response(B, LIVE_TRADE_RES_ACCEPTED, uid(A), NULL) && got_response(A, LIVE_TRADE_RES_ACCEPTED, uid(A), NULL), "A READY on both");
    bare(B, NET_EXT_TRADE_ACCEPT);
    pump(2);
    CHECK(got_response(A, LIVE_TRADE_RES_PROCESS, 3, NULL), "both READY: Loading 3");
    pump(150);
    CHECK(got_response(A, LIVE_TRADE_RES_PROCESS, 2, NULL) && got_response(B, LIVE_TRADE_RES_PROCESS, 1, NULL), "Loading 2, Loading 1");
    CHECK(!last_frame("live_trade"), "no settle before the countdown ends");
    pump(40);
    f = last_frame("live_trade");
    args = f ? cJSON_GetObjectItem(f, "args") : NULL;
    const char *key = args ? cJSON_GetStringValue(cJSON_GetObjectItem(args, "key")) : NULL;
    char *items_a = args ? cJSON_PrintUnformatted(cJSON_GetObjectItem(cJSON_GetObjectItem(args, "a"), "items")) : NULL;
    CHECK(f && cJSON_IsTrue(cJSON_GetObjectItem(f, "reliable")) && key && !strncmp(key, "lt:", 3) && strlen(key) >= 15 &&
              cJSON_GetObjectItem(args, "cost")->valueint == LIVE_TRADE_COST, "live_trade: reliable, key lt:<hex> (%s), cost 10", key ? key : "");
    CHECK(items_a && strstr(items_a, "[100,1]") && strstr(items_a, "[200,1,1000005]") &&
              cJSON_GetObjectItem(cJSON_GetObjectItem(args, "a"), "kr")->valueint == 100 &&
              cJSON_GetObjectItem(cJSON_GetObjectItem(args, "b"), "id")->valueint == 102, "the sides: A [100 x1, copy 1000005] + 100 KR, B [400]");
    free(items_a);
    update(A, 100, LIVE_TRADE_ITEM_ADD, 0);
    bare(A, NET_EXT_TRADE_CANCEL);
    pump(2);
    CHECK(trade_is_trading(conn(A)) && ext_count(B, NET_EXT_TRADE_ITEM) == 0 && !got_response(B, LIVE_TRADE_RES_CANCELED, -2, NULL),
          "settling: no change, no cancel");
    reply(f, NULL,
          "{\"a\":{\"funds\":390,\"removed\":[\"100\",\"200:1000005\"],\"added\":[\"400\"],\"meta\":{},\"owned\":[[100,1],[200,0],[400,1]]},"
          "\"b\":{\"funds\":150,\"removed\":[\"400\"],\"added\":[\"100\",\"200:1000077\"],\"meta\":{\"1000077\":{\"paint\":\"#ff0000\"}},"
          "\"owned\":[[400,0],[100,1],[200,1]]}}");
    pump(2);
    cJSON *ra = chunked_json(A, NET_EXT_TRADE_RESULT, NULL), *rb = chunked_json(B, NET_EXT_TRADE_RESULT, NULL);
    CHECK(ra && cJSON_IsNull(cJSON_GetArrayItem(ra, 0)) && cJSON_GetArrayItem(ra, 1)->valueint == 390 &&
              cJSON_GetArraySize(cJSON_GetArrayItem(ra, 2)) == 2, "A: atrd [null, 390, [100, 200:1000005], [400], {}]");
    CHECK(rb && cJSON_GetArrayItem(rb, 1)->valueint == 150 &&
              cJSON_GetObjectItem(cJSON_GetArrayItem(rb, 4), "1000077"), "B: atrd [null, 150, ..., meta of the new copy]");
    cJSON_Delete(ra);
    cJSON_Delete(rb);
    CHECK(!trade_is_trading(conn(A)) && !trade_is_trading(conn(B)), "the session ended");
    CHECK(conn(A)->acct.funds == 390 && accounts_owns_skin(conn(B), 100) && accounts_owns_skin(conn(B), 200) &&
              accounts_owns_skin(conn(A), 400), "funds and skins_owned follow the brain");
    reset_received();

    // a failed settle: both get atrd [error]
    pump(200); // the request cooldown
    CHECK(open_session(A, B), "a second session");
    update(B, 400, LIVE_TRADE_ITEM_ADD, 0);
    pump(2); // A's socket is read before B's: the item must be on the side before A accepts
    bare(A, NET_EXT_TRADE_ACCEPT);
    bare(B, NET_EXT_TRADE_ACCEPT);
    pump(200);
    f = last_frame("live_trade");
    CHECK(f != NULL, "settling");
    if (f) reply(f, "Item not owned", NULL);
    pump(2);
    ra = chunked_json(A, NET_EXT_TRADE_RESULT, NULL);
    rb = chunked_json(B, NET_EXT_TRADE_RESULT, NULL);
    CHECK(ra && rb && !strcmp(cJSON_GetStringValue(cJSON_GetArrayItem(ra, 0)), "Item not owned") && cJSON_GetArraySize(rb) == 1,
          "a refused settle: atrd [\"Item not owned\"] to both, nothing else");
    cJSON_Delete(ra);
    cJSON_Delete(rb);
    CHECK(!trade_is_trading(conn(A)), "and the session ended");
    reset_received();

    // cancel (the window's close): the other window shuts with the reason, not the canceler's
    pump(200);
    CHECK(open_session(A, B), "a third session");
    reset_received();
    bare(A, NET_EXT_TRADE_CANCEL);
    pump(2);
    CHECK(got_response(B, LIVE_TRADE_RES_CANCELED, uid(A), "Alice canceled the trade") && !got_response(A, LIVE_TRADE_RES_CANCELED, -2, NULL),
          "ctrd: B's window shuts (Alice canceled the trade), A gets nothing back");
    CHECK(!trade_is_trading(conn(B)), "no session");

    // decline (the window's box): both windows shut
    pump(200);
    CHECK(open_session(A, B), "a fourth session");
    reset_received();
    bare(B, NET_EXT_TRADE_DECLINE);
    pump(2);
    CHECK(got_response(A, LIVE_TRADE_RES_CANCELED, uid(B), "Bob declined the trade") && got_response(B, LIVE_TRADE_RES_CANCELED, uid(A), NULL),
          "dtrd: both windows shut, A reads Bob declined the trade");

    // the brain cannot be reached: the request is refused at once
    pump(200);
    request(A, B);
    pump(3);
    g_post_fails = 1;
    reset_received();
    accept_request(B, A);
    pump(3);
    g_post_fails = 0;
    CHECK(got_response(A, LIVE_TRADE_RES_MESSAGE, -2, "unavailable") && !trade_is_trading(conn(A)) && ext_count(A, NET_EXT_TRADE_OPEN) == 0,
          "no brain: Trading is unavailable, no window");

    // a leave: the other window shuts
    pump(200);
    CHECK(open_session(A, B), "a fifth session");
    reset_received();
    sandbox_disconnect(B->fd);
    pump(5);
    CHECK(got_response(A, LIVE_TRADE_RES_CANCELED, -2, "Bob left the trade") && !trade_is_trading(conn(A)), "B leaves: A's window shuts");

    frames_clear();
    trade_set_post_hook(NULL);
    sandbox_stop();
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    session_tests();
    flow_tests();
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
