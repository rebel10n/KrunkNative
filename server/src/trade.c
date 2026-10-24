// The live trade of Trade mode (server/include/server_trade.h, doc/v10/trade.md section 3).
//
// v10.0.3's client (deob-10.0.3.js) is the contract: a request ("strd" [id] from the player list,
// :254081, or "int" [5, sid] from the in-world prompt, :260125) shows the target "NAME wants to
// trade [Accept]" (:259629); [Accept] sends "trd" [sid, 1] (:253614); the server answers both with
// "trd" type 1 and the two inventories (:253244), relays every "utrd" / "utrdc" change to both
// (:253308, :253316), marks "atrd" accepts (type 3), shows "Loading n" (type 4) and ends with "atrd"
// [error, funds, removed, added, meta] (:253269). The game server itself is not in the bundle: what
// it checks and when is KrunkNative's (marked KN), with v10's config values (shared/include/live_trade.h).
//
// The brain is the authority on what anyone owns. "trade_inventories" loads both inventories when a
// session opens; "live_trade" moves both sides in one database transaction (services/brain/
// db_economy.c run_live_trade): it re-checks ownership, tradability, levels, bans and funds, and an
// idempotency key per session makes the reliable (resent) frame settle once.
#include <server_trade.h>
#include <server_accounts.h>
#include <server_log.h>
#include <trade_session.h>
#include <live_trade.h>
#include <net_ext.h>
#include <brain.h>
#include <modes.h>
#include <shared.h>

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TRADE_SESSIONS_MAX 64
#define TRADE_REQUESTS_MAX 128
#define TRADE_PEERS_MAX 256
#define TRADE_CHAT_INTERVAL 0.4f // KN: one trade chat line per 0.4 s

typedef struct {
    TradeSession s;
    char key[40];        // the brain's idempotency key: "lt:" + 32 hex
} LiveTrade;

typedef struct {
    int from, to;        // connection ids
    float at;
} TradeRequest;

typedef struct {
    int conn_id;
    float last_request_at;
    float last_chat_at;
} TradePeer;

static struct {
    LiveTrade *trades[TRADE_SESSIONS_MAX];
    int trade_count;
    TradeRequest requests[TRADE_REQUESTS_MAX];
    int request_count;
    TradePeer peers[TRADE_PEERS_MAX];
    int peer_count;
    uint32_t next_id;
    int (*post)(cJSON *frame, int reliable);
} g_trade;

void trade_set_post_hook(int (*post)(cJSON *frame, int reliable)) { g_trade.post = post; }

// brain_next_rid, or (the standalone build's stub answers 0) one of our own: no brain shares that build's ids
static uint32_t next_rid(void) {
    static uint32_t own = 0x70000000u;
    const uint32_t rid = brain_next_rid();
    return rid ? rid : ++own;
}

static int post_frame(cJSON *frame, const int reliable) {
    return g_trade.post ? g_trade.post(frame, reliable) : brain_post(frame, reliable);
}

// ------------------------------------------------------------------ lookups

static ClientConnection *conn_by_id(const int id) {
    for (size_t i = 0; g_server && i < g_server->client_count; i++) {
        if (g_server->clients[i].id == id) return &g_server->clients[i];
    }
    return NULL;
}

static int trade_mode(void) {
    const GameMode *mode = g_server ? g_server->game.mode : NULL;
    return mode && mode->config.can_trade;
}

static TradePeer *peer_of(const int conn_id, const int create) {
    for (int i = 0; i < g_trade.peer_count; i++) {
        if (g_trade.peers[i].conn_id == conn_id) return &g_trade.peers[i];
    }
    if (!create || g_trade.peer_count >= TRADE_PEERS_MAX) return NULL;
    TradePeer *p = &g_trade.peers[g_trade.peer_count++];
    memset(p, 0, sizeof(*p));
    p->conn_id = conn_id;
    p->last_request_at = p->last_chat_at = -1000.0f;
    return p;
}

static LiveTrade *trade_of(const int conn_id, int *side) {
    for (int i = 0; i < g_trade.trade_count; i++) {
        const int s = trade_session_side_of(&g_trade.trades[i]->s, conn_id);
        if (s >= 0) {
            if (side) *side = s;
            return g_trade.trades[i];
        }
    }
    return NULL;
}

int trade_is_trading(const ClientConnection *client) { return client && trade_of(client->id, NULL) != NULL; }

static LiveTrade *trade_by_rid(const uint32_t rid) {
    for (int i = 0; rid && i < g_trade.trade_count; i++) {
        if (g_trade.trades[i]->s.rid == rid) return g_trade.trades[i];
    }
    return NULL;
}

static void trade_free(LiveTrade *t) {
    for (int i = 0; i < g_trade.trade_count; i++) {
        if (g_trade.trades[i] != t) continue;
        g_trade.trades[i] = g_trade.trades[--g_trade.trade_count];
        break;
    }
    trade_session_free(&t->s);
    free(t);
}

// ------------------------------------------------------------------ packets

static void send_ext(ClientConnection *to, const uint16_t subtype, const void *body, const uint16_t length) {
    if (!to || to->peer_minor < NET_PROTOCOL_MINOR_EXT) return;
    uint8_t buf[NET_EXT_HEADER_SIZE + NET_EXT_BODY_MAX];
    const uint16_t n = net_ext_pack(buf, sizeof(buf), subtype, body, length);
    if (n) net_send_packet(to->fd, NET_PACKET_EXT, buf, n);
}

// "trd" [msg, sid, type]; msg "" / NULL none
static void send_response(ClientConnection *to, const int type, const int uid, const char *msg) {
    NetTradeResponsePacket r;
    memset(&r, 0, sizeof(r));
    r.type = (uint8_t) type;
    r.uid = uid;
    snprintf(r.msg, sizeof(r.msg), "%s", msg ? msg : "");
    send_ext(to, NET_EXT_TRADE_RESPONSE, &r, (uint16_t) (offsetof(NetTradeResponsePacket, msg) + strlen(r.msg) + 1));
}

static void send_message(ClientConnection *to, const char *msg) { send_response(to, LIVE_TRADE_RES_MESSAGE, -1, msg); }

// one JSON array over consecutive EXTs (NetTradeChunkHeader)
static void send_chunks(ClientConnection *to, const uint16_t subtype, const int uid, const char *json) {
    const size_t len = json ? strlen(json) : 0;
    const size_t parts = len ? (len + NET_TRADE_CHUNK_DATA - 1) / NET_TRADE_CHUNK_DATA : 1;
    if (parts > 0xffff) return;
    uint8_t body[NET_EXT_BODY_MAX];
    for (size_t p = 0; p < parts; p++) {
        NetTradeChunkHeader h = {.part = (uint16_t) p, .parts = (uint16_t) parts, .uid = uid};
        const size_t off = p * NET_TRADE_CHUNK_DATA;
        const size_t n = len - off < (size_t) NET_TRADE_CHUNK_DATA ? len - off : (size_t) NET_TRADE_CHUNK_DATA;
        memcpy(body, &h, sizeof(h));
        if (n) memcpy(body + sizeof(h), json + off, n);
        send_ext(to, subtype, body, (uint16_t) (sizeof(h) + n));
    }
}

static ClientConnection *side_conn(const LiveTrade *t, const int side) {
    return t->s.side[side].gone ? NULL : conn_by_id(t->s.side[side].conn_id);
}

static void send_both_response(const LiveTrade *t, const int type, const int uid) {
    for (int s = 0; s < 2; s++) send_response(side_conn(t, s), type, uid, NULL);
}

// "atrd" [error] to both: windows[36].failed(error) shuts the window with a red chat line (:251425)
static void send_failed(const LiveTrade *t, const char *error) {
    cJSON *a = cJSON_CreateArray();
    cJSON_AddItemToArray(a, cJSON_CreateString(error && *error ? error : "Trade failed"));
    char *json = cJSON_PrintUnformatted(a);
    cJSON_Delete(a);
    for (int s = 0; s < 2; s++) send_chunks(side_conn(t, s), NET_EXT_TRADE_RESULT, 0, json);
    free(json);
}

// ------------------------------------------------------------------ ending

// End a session that is not settling. `actor` (or NULL) ended it with `verb` ("canceled", "declined",
// "left"): the other side's window shuts ("trd" type 2) with "NAME <verb> the trade"; the actor's own
// window is already closing when it canceled (ctrd comes from windows[36].close / another window,
// :251524, :253813), so only a decline (the window's own box) shuts it from here.
static void trade_end(LiveTrade *t, const ClientConnection *actor, const char *verb, const int shut_actor) {
    const int opened = t->s.state != TRADE_OPENING;
    for (int s = 0; s < 2; s++) {
        ClientConnection *c = side_conn(t, s);
        if (!c) continue;
        if (c == actor) {
            if (shut_actor && opened) send_response(c, LIVE_TRADE_RES_CANCELED, t->s.side[1 - s].conn_id, NULL);
            continue;
        }
        char msg[96];
        if (actor && verb) snprintf(msg, sizeof(msg), "%s %s the trade", actor->name, verb);
        else snprintf(msg, sizeof(msg), "%s", verb ? verb : "Trade canceled");
        // a window that never opened has nothing to shut: only the line (type 2 would close another window)
        send_response(c, opened ? LIVE_TRADE_RES_CANCELED : LIVE_TRADE_RES_MESSAGE, actor ? actor->id : -1, msg);
    }
    trade_free(t);
}

static void requests_drop(const int conn_id) {
    for (int i = 0; i < g_trade.request_count;) {
        if (g_trade.requests[i].from == conn_id || g_trade.requests[i].to == conn_id) {
            g_trade.requests[i] = g_trade.requests[--g_trade.request_count];
        } else {
            i++;
        }
    }
}

// ------------------------------------------------------------------ requests

static int signed_in(const ClientConnection *c) {
    return c && c->acct.join_state == SERVER_JOIN_READY && c->acct.account_id > 0;
}

// why `from` cannot trade with `to` now, NULL when it can
static const char *refusal(const ClientConnection *from, const ClientConnection *to) {
    if (!signed_in(from)) return "Login to trade";
    if (!signed_in(to)) return "That player can not trade";
    if (from->acct.account_id == to->acct.account_id) return "Cant trade with yourself";
    if (from->acct.level < LIVE_TRADE_MIN_LEVEL || to->acct.level < LIVE_TRADE_MIN_LEVEL)
        return "Both players must be level 10 or higher to trade."; // errors.exchange.tradeLevelTooLow (:117358)
    if (trade_of(from->id, NULL)) return "You are already trading";
    if (trade_of(to->id, NULL)) return "That player is already trading";
    return NULL;
}

static void trade_request(ClientConnection *from, const int target_uid, const int interact) {
    if (!trade_mode() || !from || g_server->game.end_timer > 0.0f) return; // canTrade modes only (the button / prompt, :250341, :192464)
    ClientConnection *to = conn_by_id(target_uid);
    if (!to || to == from) return;
    TradePeer *peer = peer_of(from->id, 1);
    if (!peer) return;
    const float now = g_server->now;
    if (now - peer->last_request_at < LIVE_TRADE_REQUEST_COOLDOWN) { // config.interactTradeTimer
        send_message(from, "Please wait before sending another trade request");
        return;
    }
    if (interact) {
        // the prompt's reach (13.2 eye to eye, :192470) with slack for our older position of the requester
        const Player *a = from->player, *b = to->player;
        if (!a || !b || !a->active || !b->active) return;
        const float dx = a->position.x - b->position.x, dy = a->position.y - b->position.y, dz = a->position.z - b->position.z;
        if (sqrtf(dx * dx + dy * dy + dz * dz) > LIVE_TRADE_RANGE + LIVE_TRADE_RANGE_SLACK) return;
    }
    const char *why = refusal(from, to);
    if (why) {
        send_message(from, why);
        return;
    }
    peer->last_request_at = now;
    int found = 0;
    for (int i = 0; i < g_trade.request_count; i++) {
        if (g_trade.requests[i].from == from->id && g_trade.requests[i].to == to->id) {
            g_trade.requests[i].at = now;
            found = 1;
        }
    }
    if (!found) {
        if (g_trade.request_count >= TRADE_REQUESTS_MAX) {
            send_message(from, "Trading is busy, try again");
            return;
        }
        g_trade.requests[g_trade.request_count++] = (TradeRequest) {.from = from->id, .to = to->id, .at = now};
    }
    send_response(to, LIVE_TRADE_RES_REQUEST, from->id, NULL); // "NAME wants to trade [Accept]"
    char msg[96];
    snprintf(msg, sizeof(msg), "Trade request sent to %s", to->name);
    send_message(from, msg);
    server_log("[trade] %s -> %s: request", from->name, to->name);
}

void trade_handle_interact(ClientConnection *client, const int target_uid) { trade_request(client, target_uid, 1); }

// The brain's inventories for a session: {"type":"acct","op":"trade_inventories","args":{a, b}}
static int request_inventories(LiveTrade *t) {
    cJSON *frame = cJSON_CreateObject();
    const uint32_t rid = next_rid();
    cJSON_AddStringToObject(frame, "type", "acct");
    cJSON_AddNumberToObject(frame, "rid", rid);
    cJSON_AddStringToObject(frame, "op", "trade_inventories");
    cJSON *args = cJSON_AddObjectToObject(frame, "args");
    cJSON_AddNumberToObject(args, "a", t->s.side[0].account_id);
    cJSON_AddNumberToObject(args, "b", t->s.side[1].account_id);
    t->s.rid = rid;
    return post_frame(frame, 0);
}

// "trd" [sid, 1]: `client` accepts the request of sid
static void trade_init(ClientConnection *client, const int from_uid) {
    if (!trade_mode()) return;
    const float now = g_server->now;
    int index = -1;
    for (int i = 0; i < g_trade.request_count; i++) {
        if (g_trade.requests[i].from == from_uid && g_trade.requests[i].to == client->id) index = i;
    }
    if (index < 0 || now - g_trade.requests[index].at > LIVE_TRADE_REQUEST_TTL) {
        send_message(client, "Trade request expired");
        return;
    }
    ClientConnection *from = conn_by_id(from_uid);
    const char *why = from ? refusal(client, from) : "Trade request expired";
    if (why) {
        send_message(client, why);
        return;
    }
    requests_drop(from->id);
    requests_drop(client->id);
    if (g_trade.trade_count >= TRADE_SESSIONS_MAX) {
        send_message(client, "Trading is busy, try again");
        return;
    }
    LiveTrade *t = calloc(1, sizeof(*t));
    if (!t) return;
    trade_session_init(&t->s, ++g_trade.next_id, from->id, from->acct.account_id, client->id, client->acct.account_id, now);
    char hex[40];
    brain_random_hex(hex, sizeof(hex), 16);
    snprintf(t->key, sizeof(t->key), "lt:%s", hex);
    g_trade.trades[g_trade.trade_count++] = t;
    if (request_inventories(t) < 0) {
        trade_end(t, NULL, "Trading is unavailable", 0);
        return;
    }
    server_log("[trade] %s <-> %s: opening (#%u)", from->name, client->name, t->s.id);
}

// ------------------------------------------------------------------ brain replies

// The inventory rows of one side ({i, c[, id, m, nt]}, the brain's v10 shape) into the snapshot.
static int load_inventory(const cJSON *rows, TradeInvItem **out) {
    const int n = cJSON_IsArray(rows) ? cJSON_GetArraySize(rows) : 0;
    *out = n ? calloc((size_t) n, sizeof(TradeInvItem)) : NULL;
    if (n && !*out) return 0;
    int k = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, rows) {
        const cJSON *i = cJSON_GetObjectItemCaseSensitive(it, "i"), *c = cJSON_GetObjectItemCaseSensitive(it, "c");
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(it, "id"), *nt = cJSON_GetObjectItemCaseSensitive(it, "nt");
        if (!cJSON_IsNumber(i) || !cJSON_IsNumber(c) || i->valuedouble < 0 || c->valuedouble < 1) continue;
        TradeInvItem *row = &(*out)[k++];
        row->skin = (int32_t) i->valuedouble;
        row->count = c->valuedouble > 1e6 ? 1000000 : (int32_t) c->valuedouble;
        row->copy = cJSON_IsNumber(id) ? (int64_t) id->valuedouble : 0;
        if (row->copy) row->count = 1;
        row->locked = cJSON_IsNumber(nt) && nt->valuedouble != 0;
    }
    return k;
}

static void on_inventories(LiveTrade *t, const char *err, const cJSON *data) {
    ClientConnection *a = side_conn(t, 0), *b = side_conn(t, 1);
    t->s.rid = 0;
    if (!a || !b) {
        trade_end(t, NULL, "Trade canceled", 0);
        return;
    }
    const cJSON *sa = cJSON_GetObjectItemCaseSensitive(data, "a"), *sb = cJSON_GetObjectItemCaseSensitive(data, "b");
    if (err || !cJSON_IsObject(sa) || !cJSON_IsObject(sb)) {
        trade_end(t, NULL, err && *err ? err : "Trading is unavailable", 0);
        return;
    }
    const cJSON *sides[2] = {sa, sb};
    for (int s = 0; s < 2; s++) {
        const cJSON *v = sides[s];
        const cJSON *level = cJSON_GetObjectItemCaseSensitive(v, "level");
        if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(v, "notrade")) ||
            (cJSON_IsNumber(level) && level->valuedouble < LIVE_TRADE_MIN_LEVEL)) {
            trade_end(t, NULL, cJSON_IsNumber(level) && level->valuedouble < LIVE_TRADE_MIN_LEVEL
                                   ? "Both players must be level 10 or higher to trade." : "That player can not trade", 0);
            return;
        }
        TradeInvItem *inv = NULL;
        const int n = load_inventory(cJSON_GetObjectItemCaseSensitive(v, "inv"), &inv);
        const cJSON *funds = cJSON_GetObjectItemCaseSensitive(v, "funds");
        trade_session_set_inventory(&t->s, s, inv, n, cJSON_IsNumber(funds) ? (int64_t) funds->valuedouble : 0,
                                    cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(v, "premium")));
    }
    trade_session_open(&t->s, g_server->now);
    // "trd" [null, otherSid, 1, [mine, theirs]] to each (:253244-253253)
    for (int s = 0; s < 2; s++) {
        cJSON *invs = cJSON_CreateArray();
        cJSON_AddItemToArray(invs, cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(sides[s], "inv"), 1));
        cJSON_AddItemToArray(invs, cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(sides[1 - s], "inv"), 1));
        char *json = cJSON_PrintUnformatted(invs);
        cJSON_Delete(invs);
        send_chunks(side_conn(t, s), NET_EXT_TRADE_OPEN, t->s.side[1 - s].conn_id, json);
        free(json);
    }
    server_log("[trade] #%u open: %s <-> %s", t->s.id, a->name, b->name);
}

// skins_owned (the ENT ownership check) after the trade: [[skin, count after], ...] of the touched skins
static void apply_owned(ClientConnection *c, const cJSON *owned) {
    const cJSON *it;
    cJSON_ArrayForEach(it, owned) {
        const cJSON *s = cJSON_GetArrayItem(it, 0), *n = cJSON_GetArrayItem(it, 1);
        if (!cJSON_IsNumber(s) || !cJSON_IsNumber(n)) continue;
        const int skin = (int) s->valuedouble;
        if (skin < 0 || skin >= (int) sizeof(c->acct.skins_owned) * 8) continue;
        if (n->valuedouble > 0) c->acct.skins_owned[skin >> 3] |= (uint8_t) (1u << (skin & 7));
        else c->acct.skins_owned[skin >> 3] &= (uint8_t) ~(1u << (skin & 7));
    }
}

static void on_settled(LiveTrade *t, const char *err, const cJSON *data) {
    const cJSON *res[2] = {cJSON_GetObjectItemCaseSensitive(data, "a"), cJSON_GetObjectItemCaseSensitive(data, "b")};
    if (err || !cJSON_IsObject(res[0]) || !cJSON_IsObject(res[1])) {
        server_log("[trade] #%u failed: %s", t->s.id, err ? err : "bad reply");
        send_failed(t, err && *err ? err : "Trade failed");
        trade_free(t);
        return;
    }
    for (int s = 0; s < 2; s++) {
        ClientConnection *c = side_conn(t, s);
        if (!c || c->acct.account_id != t->s.side[s].account_id) continue;
        const cJSON *v = res[s];
        const cJSON *funds = cJSON_GetObjectItemCaseSensitive(v, "funds");
        if (cJSON_IsNumber(funds)) c->acct.funds = (int) funds->valuedouble;
        apply_owned(c, cJSON_GetObjectItemCaseSensitive(v, "owned"));
        // "atrd" [null, funds, removed, added, meta] (:253269-253307)
        cJSON *a = cJSON_CreateArray();
        cJSON_AddItemToArray(a, cJSON_CreateNull());
        cJSON_AddItemToArray(a, cJSON_CreateNumber(cJSON_IsNumber(funds) ? funds->valuedouble : c->acct.funds));
        const char *keys[] = {"removed", "added", "meta"};
        for (int k = 0; k < 3; k++) {
            const cJSON *x = cJSON_GetObjectItemCaseSensitive(v, keys[k]);
            cJSON_AddItemToArray(a, x ? cJSON_Duplicate(x, 1) : (k < 2 ? cJSON_CreateArray() : cJSON_CreateObject()));
        }
        char *json = cJSON_PrintUnformatted(a);
        cJSON_Delete(a);
        send_chunks(c, NET_EXT_TRADE_RESULT, 0, json);
        free(json);
    }
    server_log("[trade] #%u done (%s)", t->s.id, t->key);
    trade_free(t);
}

int trade_on_brain_frame(const cJSON *frame) {
    const cJSON *rid = cJSON_GetObjectItemCaseSensitive(frame, "rid");
    if (!cJSON_IsNumber(rid)) return 0;
    LiveTrade *t = trade_by_rid((uint32_t) rid->valuedouble);
    if (!t) return 0;
    const char *err = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(frame, "err"));
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(frame, "data");
    if (t->s.state == TRADE_OPENING) on_inventories(t, err, data);
    else if (t->s.state == TRADE_SETTLING) on_settled(t, err, data);
    return 1;
}

void trade_link_down(void) {
    for (int i = g_trade.trade_count - 1; i >= 0; i--) {
        LiveTrade *t = g_trade.trades[i];
        // a settle is a reliable frame: brain.c resends it after the reconnect, the key replays it
        if (t->s.state == TRADE_OPENING) trade_end(t, NULL, "Trading is unavailable", 0);
    }
}

// ------------------------------------------------------------------ the settle

static cJSON *side_args(const LiveTrade *t, const int side) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "id", t->s.side[side].account_id);
    char *items = trade_session_offer_json(&t->s, side);
    cJSON *parsed = items ? cJSON_Parse(items) : NULL;
    free(items);
    cJSON_AddItemToObject(o, "items", parsed ? parsed : cJSON_CreateArray());
    cJSON_AddNumberToObject(o, "kr", (double) t->s.side[side].kr);
    return o;
}

static void trade_settle(LiveTrade *t) {
    cJSON *frame = cJSON_CreateObject();
    const uint32_t rid = next_rid();
    cJSON_AddStringToObject(frame, "type", "acct");
    cJSON_AddNumberToObject(frame, "rid", rid);
    cJSON_AddStringToObject(frame, "op", "live_trade");
    cJSON *args = cJSON_AddObjectToObject(frame, "args");
    cJSON_AddStringToObject(args, "key", t->key);
    cJSON_AddItemToObject(args, "a", side_args(t, 0));
    cJSON_AddItemToObject(args, "b", side_args(t, 1));
    cJSON_AddNumberToObject(args, "cost", LIVE_TRADE_COST);
    t->s.rid = rid;
    server_log("[trade] #%u settling (%s)", t->s.id, t->key);
    if (post_frame(frame, 1) < 0) { // reliable: resent until the brain answers (the key makes it settle once)
        send_failed(t, "Trading is unavailable");
        trade_free(t);
    }
}

// ------------------------------------------------------------------ in-session packets

static void on_update(ClientConnection *client, LiveTrade *t, const int side, const NetTradeUpdatePacket *u) {
    const int owner = u->action == LIVE_TRADE_ITEM_REQUEST ? 1 - side : side;
    TradeError e;
    if (u->action == LIVE_TRADE_ITEM_ADD || u->action == LIVE_TRADE_ITEM_REQUEST) e = trade_session_add(&t->s, owner, u->skin, u->copy);
    else if (u->action == LIVE_TRADE_ITEM_REMOVE) e = trade_session_remove(&t->s, owner, u->skin, u->copy);
    else return;
    if (e != TRADE_OK) {
        send_message(client, trade_error_text(e, trade_session_side_limit(&t->s, owner)));
        return;
    }
    NetTradeItemPacket item = {.owner_uid = t->s.side[owner].conn_id, .add = u->action != LIVE_TRADE_ITEM_REMOVE,
                               .skin = u->skin, .copy = u->copy};
    for (int s = 0; s < 2; s++) send_ext(side_conn(t, s), NET_EXT_TRADE_ITEM, &item, sizeof(item));
}

static void on_currency(ClientConnection *client, LiveTrade *t, const int side, const NetTradeCurrencyPacket *c) {
    if (c->currency != LIVE_TRADE_CURRENCY_KR) { // the window only sends KR; Junk / RP have names, no rules
        send_message(client, trade_error_text(TRADE_ERR_CURRENCY, 0));
        return;
    }
    const int owner = c->action == LIVE_TRADE_CUR_REQUEST ? 1 - side : side;
    TradeError e;
    if (c->action == LIVE_TRADE_CUR_ADD || c->action == LIVE_TRADE_CUR_REQUEST) e = trade_session_add_kr(&t->s, owner, c->amount);
    else if (c->action == LIVE_TRADE_CUR_REMOVE) e = trade_session_clear_kr(&t->s, owner);
    else return;
    if (e != TRADE_OK) {
        send_message(client, trade_error_text(e, 0));
        return;
    }
    NetTradeCurrencyStatePacket st = {.owner_uid = t->s.side[owner].conn_id,
                                      .action = (uint8_t) (c->action == LIVE_TRADE_CUR_REMOVE ? LIVE_TRADE_CUR_REMOVE : LIVE_TRADE_CUR_ADD),
                                      .currency = LIVE_TRADE_CURRENCY_KR,
                                      .amount = c->action == LIVE_TRADE_CUR_REMOVE ? 0 : c->amount};
    for (int s = 0; s < 2; s++) send_ext(side_conn(t, s), NET_EXT_TRADE_CURRENCY_STATE, &st, sizeof(st));
}

static void on_accept(ClientConnection *client, LiveTrade *t, const int side) {
    int both = 0;
    const TradeError e = trade_session_accept(&t->s, side, &both);
    if (e != TRADE_OK) {
        if (e != TRADE_ERR_STATE) send_message(client, trade_error_text(e, 0));
        return;
    }
    send_both_response(t, LIVE_TRADE_RES_ACCEPTED, client->id); // READY (windows[36].accepted, :251414)
}

static void on_chat(ClientConnection *client, LiveTrade *t, const uint8_t *body, const uint16_t length) {
    TradePeer *peer = peer_of(client->id, 1);
    const float now = g_server->now;
    if (!peer || now - peer->last_chat_at < TRADE_CHAT_INTERVAL) return;
    char text[NET_CHAT_TEXT_SIZE];
    size_t n = 0;
    while (n < length && body[n]) n++;
    if (!net_chat_sanitize(text, sizeof(text), (const char *) body, n)) return;
    // #tradeChatInput's maxlength 70 (:251202): 70 code points
    size_t cut = 0;
    for (int k = 0; text[cut] && k < LIVE_TRADE_CHAT_MAX; k++) {
        cut++;
        while ((text[cut] & 0xC0) == 0x80) cut++;
    }
    text[cut] = 0;
    peer->last_chat_at = now;
    NetTradeChatLinePacket line;
    memset(&line, 0, sizeof(line));
    snprintf(line.name, sizeof(line.name), "%s", client->name);
    snprintf(line.msg, sizeof(line.msg), "%s", text);
    for (int s = 0; s < 2; s++) send_ext(side_conn(t, s), NET_EXT_TRADE_CHAT_LINE, &line, sizeof(line));
}

int trade_handle_ext(ClientConnection *client, const uint16_t subtype, const uint8_t *body, const uint16_t length) {
    if (subtype < NET_EXT_TRADE_REQUEST || subtype > NET_EXT_TRADE_CHAT) return 0;
    if (subtype == NET_EXT_TRADE_REQUEST) {
        if (length >= sizeof(NetTradeRequestPacket)) {
            NetTradeRequestPacket r;
            memcpy(&r, body, sizeof(r));
            trade_request(client, r.uid, 0);
        }
        return 1;
    }
    if (subtype == NET_EXT_TRADE_INIT) {
        if (length >= sizeof(NetTradeInitPacket)) {
            NetTradeInitPacket r;
            memcpy(&r, body, sizeof(r));
            if (r.accept == 1) trade_init(client, r.uid);
        }
        return 1;
    }
    int side = -1;
    LiveTrade *t = trade_of(client->id, &side);
    if (!t) return 1;
    switch (subtype) {
        case NET_EXT_TRADE_UPDATE:
            if (length >= sizeof(NetTradeUpdatePacket)) {
                NetTradeUpdatePacket u;
                memcpy(&u, body, sizeof(u));
                on_update(client, t, side, &u);
            }
            break;
        case NET_EXT_TRADE_CURRENCY:
            if (length >= sizeof(NetTradeCurrencyPacket)) {
                NetTradeCurrencyPacket c;
                memcpy(&c, body, sizeof(c));
                on_currency(client, t, side, &c);
            }
            break;
        case NET_EXT_TRADE_ACCEPT:
            on_accept(client, t, side);
            break;
        case NET_EXT_TRADE_DECLINE:
        case NET_EXT_TRADE_CANCEL:
            // the guide: "you can cancel any time before both sides accept" (:148537); once the brain
            // moves the items it is too late (KN: the countdown still cancels)
            if (t->s.state == TRADE_SETTLING) break;
            server_log("[trade] #%u %s by %s", t->s.id, subtype == NET_EXT_TRADE_DECLINE ? "declined" : "canceled", client->name);
            trade_end(t, client, subtype == NET_EXT_TRADE_DECLINE ? "declined" : "canceled", subtype == NET_EXT_TRADE_DECLINE);
            break;
        case NET_EXT_TRADE_CHAT:
            if (t->s.state != TRADE_OPENING) on_chat(client, t, body, length);
            break;
        default:
            break;
    }
    return 1;
}

// ------------------------------------------------------------------ lifecycle

void trade_on_leave(ClientConnection *client) {
    if (!client) return;
    requests_drop(client->id);
    int side = -1;
    LiveTrade *t = trade_of(client->id, &side);
    if (t) {
        if (t->s.state == TRADE_SETTLING) t->s.side[side].gone = 1; // the brain's answer still reaches the other side
        else trade_end(t, client, "left", 0);
    }
    for (int i = 0; i < g_trade.peer_count; i++) {
        if (g_trade.peers[i].conn_id != client->id) continue;
        g_trade.peers[i] = g_trade.peers[--g_trade.peer_count];
        break;
    }
}

void trade_tick(const float now, const float dt) {
    for (int i = 0; i < g_trade.request_count;) {
        if (now - g_trade.requests[i].at > LIVE_TRADE_REQUEST_TTL) g_trade.requests[i] = g_trade.requests[--g_trade.request_count];
        else i++;
    }
    const int allowed = trade_mode() && g_server->game.end_timer <= 0.0f;
    if (!allowed) g_trade.request_count = 0;
    for (int i = g_trade.trade_count - 1; i >= 0; i--) {
        LiveTrade *t = g_trade.trades[i];
        if (t->s.state == TRADE_SETTLING) continue;
        // another mode, the end screen (the round switch closes the window, :256686): canceled
        if (!allowed) {
            trade_end(t, NULL, "Trade canceled", 0);
            continue;
        }
        if (t->s.state == TRADE_OPENING && now - t->s.started_at > LIVE_TRADE_OPEN_TIMEOUT) {
            trade_end(t, NULL, "Trading is unavailable", 0);
            continue;
        }
        const int step = trade_session_tick(&t->s, dt);
        if (step > 0) send_both_response(t, LIVE_TRADE_RES_PROCESS, step); // "Loading n" (windows[36].process, :251429)
        else if (step == 0) trade_settle(t);
    }
}
