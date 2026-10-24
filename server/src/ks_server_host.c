// The server's KrunkScript host (server/include/ks_server.h, doc/v7/krunkscript.md 2.1-2.8, 3.x,
// 4.1, 6.5): the map's scripts.server runs in g_server->game (dedicated server and offline
// sandbox alike). Shared code (shared/src/ks_game) owns the script lifecycle, the hook points
// inside the game and the both-side API; this file adds what needs the connections: the
// console (GAME.log -> server log + NET_PACKET_SCRIPT_LOG), GAME.NETWORK, GAME.CHAT,
// GAME.STORAGE (WP6, backends in server/src/ks_storage.c), GAME.ADMIN, GAME.PAYMENTS,
// GAME.CONFIG.getHost, inbound script messages and the custom sync values. KrunkScript
// packets go only to peers that announced NET_PROTOCOL_MINOR_V7.
#include <ks_server.h>
#include <ks_game.h>
#include <ks_net.h>
#include <server_log.h>
#include <server_accounts.h>
#include <server_sandbox.h>
#include <ks_storage.h>
#include <game_config.h> // CONFIG.getHost: game.host (game_host_player)
#include <pcg_basic.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ARG(i) ((i) < argc ? args[(i)] : ks_undef())

#define KS_LOG_LINES_PER_SECOND 20   // 2.8 [KN]: per game; the rest of the second is dropped
#define KS_INBOUND_PER_SECOND 20     // 2.5 [KN]: client "sd" messages per player per second
#define KS_CHAT_DEFAULT_COLOR 0xfc03ec

// ------------------------------------------------------------------ per connection

typedef struct {
    int id;                 // ClientConnection.id
    uint8_t minor;          // HELLO minor (0 before the HELLO)
    uint8_t in_match;       // had a player since the last onPlayerLeave
    KsNetReassembler in;    // inbound SCRIPT_MSG
    long long in_window;    // 1-s window of the inbound limit
    int in_count;
} KsConn;

static KsConn *g_conns;
static size_t g_conn_count, g_conn_cap;
static uint16_t g_msg_seq;
static double (*g_clock)(void);

static double now_ms(void)
{
    return g_clock ? g_clock() : ks_wall_ms(); // portable (MinGW has no timespec_get)
}

void ks_server_set_clock(double (*clock)(void))
{
    g_clock = clock;
}

static KsConn *conn_find(const int id)
{
    for (size_t i = 0; i < g_conn_count; i++)
        if (g_conns[i].id == id) return &g_conns[i];
    return NULL;
}

static KsConn *conn_get(const int id)
{
    KsConn *c = conn_find(id);
    if (c) return c;
    if (g_conn_count == g_conn_cap) {
        const size_t cap = g_conn_cap ? g_conn_cap * 2 : 16;
        KsConn *list = realloc(g_conns, cap * sizeof(*list));
        if (!list) return NULL;
        g_conns = list;
        g_conn_cap = cap;
    }
    c = &g_conns[g_conn_count++];
    memset(c, 0, sizeof(*c));
    c->id = id;
    return c;
}

void ks_server_on_hello(ClientConnection *client, const NetPacket *packet)
{
    NetHelloPacket hello;
    if (!client || !packet || !net_packet_read_hello(&hello, packet)) return; // WP-C: a 2-byte HELLO is patch 0
    KsConn *c = conn_get(client->id);
    if (c) c->minor = hello.major == NET_PROTOCOL_MAJOR ? hello.minor : 0;
}

void ks_server_on_disconnect(ClientConnection *client)
{
    if (!client) return;
    for (size_t i = 0; i < g_conn_count; i++) {
        if (g_conns[i].id != client->id) continue;
        ks_net_reassembler_free(&g_conns[i].in);
        g_conns[i] = g_conns[--g_conn_count];
        return;
    }
}

int ks_server_peer_v7(const ClientConnection *client)
{
    const KsConn *c = client ? conn_find(client->id) : NULL;
    return c && c->minor >= NET_PROTOCOL_MINOR_V7;
}

static ClientConnection *client_by_id(const int id)
{
    for (size_t i = 0; i < g_server->client_count; i++)
        if (g_server->clients[i].id == id) return &g_server->clients[i];
    return NULL;
}

static ClientConnection *client_of_player(const Player *player)
{
    for (size_t i = 0; player && i < g_server->client_count; i++)
        if (g_server->clients[i].player == player) return &g_server->clients[i];
    return NULL;
}

// ------------------------------------------------------------------ console (2.8)

static long long g_log_window = -1;
static int g_log_count;

static void broadcast_log(const int is_error, const char *line)
{
    uint8_t payload[1 + KS_NET_LOG_TEXT_MAX];
    const uint16_t n = ks_net_encode_log(is_error, line, payload);
    for (size_t i = 0; i < g_server->client_count; i++) {
        ClientConnection *client = &g_server->clients[i];
        if (ks_server_peer_v7(client)) net_send_packet(client->fd, NET_PACKET_SCRIPT_LOG, payload, n);
    }
}

// GAME.log / runtime errors: the server log, and "ksL" to every client (GJS:142-152)
static void host_log(void *user, const int is_error, const char *line)
{
    (void) user;
    if (!line) return;
    if (is_error) server_log_error("[KRUNKSCRIPT] %s", line);
    else server_log("[KRUNKSCRIPT] %s", line);

    const long long window = (long long) floor(now_ms() / 1000.0);
    if (window != g_log_window) {
        g_log_window = window;
        g_log_count = 0;
    }
    if (++g_log_count > KS_LOG_LINES_PER_SECOND) {
        if (g_log_count == KS_LOG_LINES_PER_SECOND + 1) broadcast_log(1, "... (log rate limited)");
        return;
    }
    broadcast_log(is_error, line);
}

static double host_now(void *user)
{
    (void) user;
    return now_ms();
}

// ------------------------------------------------------------------ GAME.NETWORK (2.5, 3.1.3)

static Game *game_of(KsScript *s) { return ks_game_of(s); }

// One message to one connection (when it speaks the KrunkScript packets).
static void send_body(ClientConnection *client, const uint8_t *body, const size_t len, const uint16_t seq)
{
    if (!client || !ks_server_peer_v7(client)) return;
    uint8_t chunk[NET_MAX_PAYLOAD];
    const int count = ks_net_chunk_count(len);
    for (int i = 0; i < count; i++) {
        const uint16_t n = ks_net_make_chunk(body, len, seq, i, chunk);
        if (!n || !net_send_packet(client->fd, NET_PACKET_SCRIPT_MSG, chunk, n)) return;
    }
}

// The message body, or -1 when rejected (id over 64 bytes, data over 2000 JSON chars, cycles)
static int encode(KsScript *s, const KsValue id, const KsValue data, uint8_t *body)
{
    char id_text[KS_NET_ID_MAX + 2];
    const size_t n = ks_to_string(s, id, id_text, sizeof(id_text));
    if (n > KS_NET_ID_MAX) return -1;
    return ks_net_encode_msg(s, id_text, data, body, KS_NET_MAX_BODY);
}

// NETWORK.send(id, data, playerID) on the server (API:1366-1392): n_send<playerID> 20 per
// second (the 20th call in a window fails), then the size rule, then the target.
static KsValue net_send(KsScript *s, void *user, const KsValue *args, int argc)
{
    (void) user;
    char target[64] = "";
    const KsValue to = ARG(2);
    // (playerID || "") + ""
    const int falsy = to.tag == KS_UNDEF || to.tag == KS_NULL || (to.tag == KS_BOOL && !to.as.b) ||
                      (to.tag == KS_NUM && (to.as.num == 0 || isnan(to.as.num))) || (to.tag == KS_STR && !ks_str_utf8(to, NULL)[0]);
    if (!falsy) ks_to_string(s, to, target, sizeof(target));
    char key[80];
    snprintf(key, sizeof(key), "n_send%s", target);
    Game *game = game_of(s);
    if (!ks_game_rate(game, key, 20, 0)) return ks_bool(0);
    static uint8_t body[KS_NET_MAX_BODY];
    const int len = encode(s, ARG(0), ARG(1), body);
    if (len < 0) return ks_bool(0);
    if (!target[0]) return ks_bool(0);
    Player *player = ks_game_find_player_str(game, target);
    ClientConnection *client = player ? client_of_player(player) : client_by_id(atoi(target));
    send_body(client, body, (size_t) len, g_msg_seq++);
    return ks_bool(1);
}

// NETWORK.broadcast(id, data) (API:1350-1365): c_broad 10 per second (9 pass), size rule.
static KsValue net_broadcast(KsScript *s, void *user, const KsValue *args, int argc)
{
    (void) user;
    if (!ks_game_rate(game_of(s), "c_broad", 10, 0)) return ks_bool(0);
    static uint8_t body[KS_NET_MAX_BODY];
    const int len = encode(s, ARG(0), ARG(1), body);
    if (len < 0) return ks_bool(0);
    const uint16_t seq = g_msg_seq++;
    for (size_t i = 0; i < g_server->client_count; i++) send_body(&g_server->clients[i], body, (size_t) len, seq);
    return ks_bool(1);
}

void ks_server_on_script_msg(ClientConnection *client, const NetPacket *packet)
{
    Game *game = &g_server->game;
    KsScript *s = ks_game_script(game);
    KsConn *c = client ? conn_get(client->id) : NULL;
    if (!c || !packet) return;
    const uint8_t *body = NULL;
    size_t len = 0;
    if (ks_net_reassemble(&c->in, packet->payload, packet->length, &body, &len) != 1 || !s) return;

    // 2.5 [KN]: at most 20 messages per player per second, at most 2000 prefixed JSON chars
    const long long window = (long long) floor(now_ms() / 1000.0);
    if (window != c->in_window) {
        c->in_window = window;
        c->in_count = 0;
    }
    if (++c->in_count > KS_INBOUND_PER_SECOND) return;

    char id[KS_NET_ID_MAX + 1];
    KsValue data;
    if (!ks_net_decode_msg(s, body, len, id, sizeof(id), &data)) return;
    const size_t json = ks_json_length_prefixed(s, data);
    if (json == (size_t) -1 || json > KS_NET_JSON_MAX) return;

    char sender[24];
    snprintf(sender, sizeof(sender), "%d", client->player ? client->player->uid : client->id);
    KsValue args[3] = {ks_str(s, id, strlen(id)), data, ks_str(s, sender, strlen(sender))};
    ks_game_call(game, "onNetworkMessage", args, 3);
}

// ------------------------------------------------------------------ GAME.CHAT (3.1.4)

// UTILS.isHexColor: /^#([0-9A-F]{3}){1,2}$/i -> 0xRRGGBB, -1 otherwise
static long ks_hex_color(KsScript *s, const KsValue v)
{
    size_t n = 0;
    const char *text = ks_str_utf8(v, &n);
    (void) s;
    if (!text || (n != 4 && n != 7) || text[0] != '#') return -1;
    unsigned long rgb = 0;
    for (size_t i = 1; i < n; i++) {
        const char c = text[i];
        const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) return -1;
        rgb = rgb << 4 | (unsigned long) d;
        if (n == 4) rgb = rgb << 4 | (unsigned long) d; // #abc -> #aabbcc
    }
    return (long) rgb;
}

// The v7 message checks: a string of at most chatMaxLength (70) UTF-16 units with a non-space
// character; then [^\w:()/? -] removed and every "/" removed.
static int chat_text(KsScript *s, const KsValue v, char *out, const size_t cap)
{
    size_t n = 0;
    const char *text = ks_str_utf8(v, &n);
    (void) s;
    if (!text || !n) return 0;
    size_t units = 0;
    int blank = 1;
    for (size_t i = 0; i < n; i++) {
        const unsigned char c = (unsigned char) text[i];
        if ((c & 0xC0) != 0x80) units += c >= 0xF0 ? 2 : 1;
        // \s: ASCII whitespace plus the non-ASCII JS spaces (treated as non-blank here unless ASCII)
        if (!(c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f')) blank = 0;
    }
    if (units > NET_CHAT_MAX_CHARS || blank) return 0;
    size_t o = 0;
    for (size_t i = 0; i < n && o + 1 < cap; i++) {
        const char c = text[i];
        const int keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == ':' ||
                         c == '(' || c == ')' || c == '?' || c == ' ' || c == '-';
        if (keep) out[o++] = c;
    }
    out[o] = 0;
    return 1;
}

static void send_chat(ClientConnection *client, const char *text, const unsigned long color)
{
    NetScriptChatPacket packet;
    net_packet_chat(&packet.chat, -1, NET_CHAT_SERVER, NULL, text);
    packet.color = (uint32_t) color;
    if (ks_server_peer_v7(client)) net_send_packet(client->fd, NET_PACKET_SCRIPT_CHAT, &packet, sizeof(packet));
    else net_send_packet(client->fd, NET_PACKET_CHAT, &packet.chat, sizeof(packet.chat));
}

static void chat_out(KsScript *s, const KsValue target, const KsValue message, const KsValue color_arg)
{
    const long parsed = ks_hex_color(s, color_arg);
    const unsigned long color = parsed >= 0 ? (unsigned long) parsed : KS_CHAT_DEFAULT_COLOR;
    char text[NET_CHAT_TEXT_SIZE];
    if (!chat_text(s, message, text, sizeof(text))) return;
    const int to_all = target.tag == KS_UNDEF || target.tag == KS_NULL || (target.tag == KS_BOOL && !target.as.b) ||
                       (target.tag == KS_NUM && (target.as.num == 0 || isnan(target.as.num))) ||
                       (target.tag == KS_STR && !ks_str_utf8(target, NULL)[0]);
    if (to_all) {
        server_log("[KRUNKSCRIPT chat] %s", text);
        for (size_t i = 0; i < g_server->client_count; i++) send_chat(&g_server->clients[i], text, color);
        return;
    }
    char id[64];
    ks_to_string(s, target, id, sizeof(id));
    Player *player = ks_game_find_player_str(game_of(s), id);
    ClientConnection *client = player ? client_of_player(player) : client_by_id(atoi(id));
    if (client) send_chat(client, text, color);
}

static KsValue chat_send(KsScript *s, void *user, const KsValue *args, int argc)
{
    (void) user;
    if (!ks_game_rate(game_of(s), "c_send", 3, 0)) return ks_undef();
    chat_out(s, ARG(0), ARG(1), ARG(2));
    return ks_undef();
}

static KsValue chat_broadcast(KsScript *s, void *user, const KsValue *args, int argc)
{
    (void) user;
    if (!ks_game_rate(game_of(s), "c_broadcast", 3, 0)) return ks_undef();
    chat_out(s, ks_undef(), ARG(0), ARG(1));
    return ks_undef();
}

// ------------------------------------------------------------------ GAME.STORAGE (3.1.12, WP6)

// The v7 API context's in-flight locks (f2121 / f2122, API:7-15) of this script instance:
// "str_set_<name>", "str_upd_<name>", "str_lod_<name>". g_script_gen changes once an instance is
// unloaded, so callbacks of a script that was replaced or freed are dropped (even when the next
// instance reuses its address).
#define KS_STORAGE_LOCKS 64
static char g_locks[KS_STORAGE_LOCKS][96];
static int g_lock_count;
static uint32_t g_script_gen = 1;

static int lock_take(const char *key) // f2121: 0 when already held (the lock is taken otherwise)
{
    for (int i = 0; i < g_lock_count; i++)
        if (!strcmp(g_locks[i], key)) return 0;
    if (g_lock_count < KS_STORAGE_LOCKS) snprintf(g_locks[g_lock_count++], sizeof(g_locks[0]), "%s", key);
    return 1;
}

static void lock_drop(const char *key) // f2122
{
    for (int i = 0; i < g_lock_count; i++) {
        if (strcmp(g_locks[i], key)) continue;
        memmove(g_locks[i], g_locks[g_lock_count - 1], sizeof(g_locks[0]));
        g_lock_count--;
        return;
    }
}

static void storage_reset(void)
{
    g_script_gen++;
    g_lock_count = 0;
}

static void log_error_text(const char *text)
{
    host_log(NULL, 1, text); // f2125: console.error of the script console
}

typedef struct {
    uint32_t gen;
    KsScript *script;
    KsValue callback;   // KS_FN or undefined (pinned while pending)
    int load;
    char lock[96];
    char name[64];
} StorageCall;

static StorageCall *call_new(KsScript *s, const KsValue callback, const int load, const char *lock, const char *name)
{
    StorageCall *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->gen = g_script_gen;
    c->script = s;
    c->callback = callback.tag == KS_FN ? callback : ks_undef();
    c->load = load;
    snprintf(c->lock, sizeof(c->lock), "%s", lock);
    snprintf(c->name, sizeof(c->name), "%s", name);
    if (c->callback.tag == KS_FN) ks_handle_pin(s, c->callback);
    return c;
}

// callback(data, success, accountName): data = {key: "value"} with v7's addPrefix keys
static void call_back(KsScript *s, const StorageCall *c, const KsStoragePair *rows, const int count, const int success)
{
    if (c->callback.tag != KS_FN) return;
    KsValue data = ks_obj_new(s);
    ks_handle_pin(s, data);
    for (int i = 0; success && i < count; i++)
        ks_obj_set(s, data, ks_storage_script_key(rows[i].key), ks_str(s, rows[i].value, strlen(rows[i].value)));
    const KsValue args[3] = {data, ks_bool(success), ks_str(s, c->name, strlen(c->name))};
    ks_call_value(s, c->callback, args, 3, NULL);
    ks_handle_unpin(s, data);
}

static void call_free(StorageCall *c, const int live)
{
    if (live && c->callback.tag == KS_FN) ks_handle_unpin(c->script, c->callback);
    free(c);
}

static void storage_done(void *user, const KsStoragePair *rows, const int count, const char *error)
{
    StorageCall *c = user;
    KsScript *s = g_server ? ks_game_script(&g_server->game) : NULL;
    const int live = s && s == c->script && c->gen == g_script_gen;
    if (live) {
        lock_drop(c->lock);
        if (c->load) {
            // load: an error is logged, no rows log "No data"; the callback only runs with data
            if (error) log_error_text(error);
            else if (!count) log_error_text("No data");
            else call_back(s, c, rows, count, 1);
        } else {
            if (error) log_error_text(error);
            call_back(s, c, rows, count, !error && count > 0);
        }
    }
    call_free(c, live);
}

// data (an object: its keys with v7's V_ prefix; an array: its indices) -> storageUpdt's pairs.
// NULL for anything else ("Missing storage data"). [KN] a host object (player, AI...) has no
// enumerable script keys: it saves nothing.
static KsStorageData *storage_data(KsScript *s, const KsValue v, KsStorageData *d)
{
    if (v.tag != KS_OBJ && v.tag != KS_ARR && v.tag != KS_HOST) return NULL;
    ks_storage_data_init(d);
    const size_t n = v.tag == KS_OBJ ? ks_obj_count(v) : v.tag == KS_ARR ? ks_arr_len(v) : 0;
    for (size_t i = 0; i < n; i++) {
        char key[160];
        KsValue value;
        if (v.tag == KS_OBJ) {
            KsValue k;
            if (!ks_obj_entry(s, v, i, &k, &value)) break;
            snprintf(key, sizeof(key), "%s%s", KS_STORAGE_SCRIPT_PREFIX, ks_str_utf8(k, NULL));
        } else {
            snprintf(key, sizeof(key), "%zu", i);
            value = ks_arr_get(s, v, i);
        }
        char small[512];
        const size_t len = ks_to_string(s, value, small, sizeof(small));
        char *text = small;
        if (len >= sizeof(small) && (text = malloc(len + 1)) != NULL) ks_to_string(s, value, text, len + 1);
        ks_storage_data_add(d, key, text ? text : small);
        if (text != small) free(text);
    }
    return d;
}

// STORAGE.set / update (API:2222-2285): lock, the 10-s limit (1 call), then storageUpdt.
static KsValue storage_write(KsScript *s, const KsValue *args, const int argc, const int type, const char *what, const char *prefix)
{
    char name[64], lock[96], text[160];
    ks_to_string(s, ARG(0), name, sizeof(name));
    snprintf(lock, sizeof(lock), "%s%s", prefix, name);
    size_t access_len = 0;
    const char *access = ks_str_utf8(ARG(2), &access_len);
    const int scope = access && !strcmp(access, "global") ? KS_STORAGE_GLOBAL : KS_STORAGE_PRIVATE;
    if (!lock_take(lock)) {
        snprintf(text, sizeof(text), "%s already in progress for %s", what, name);
        log_error_text(text);
        return ks_undef();
    }
    if (!ks_game_rate(game_of(s), lock, 1, 1)) {
        snprintf(text, sizeof(text), "Calling %s too much", what);
        log_error_text(text);
        lock_drop(lock);
        return ks_undef();
    }
    StorageCall *c = call_new(s, ARG(3), 0, lock, name);
    if (!c) {
        lock_drop(lock);
        return ks_undef();
    }
    KsStorageData data;
    const char *err = ks_storage_update(name, storage_data(s, ARG(1), &data), type, scope, storage_done, c);
    if (err) { // storageUpdt answered at once: cb(null, err)
        lock_drop(lock);
        log_error_text(err);
        call_back(s, c, NULL, 0, 0);
        call_free(c, 1);
    }
    return ks_undef();
}

static KsValue storage_set(KsScript *s, void *user, const KsValue *args, int argc)
{
    (void) user;
    return storage_write(s, args, argc, KS_STORAGE_SET, "storage.set", "str_set_");
}

static KsValue storage_update(KsScript *s, void *user, const KsValue *args, int argc)
{
    (void) user;
    return storage_write(s, args, argc, KS_STORAGE_INCREASE, "storage.update", "str_upd_");
}

static KsValue storage_transact(KsScript *s, void *user, const KsValue *args, int argc)
{
    (void) s; (void) user; (void) args; (void) argc;
    log_error_text("Transactions disabled for now");
    return ks_undef();
}

// STORAGE.load(accountName, mapName, callback) (API:2290-2323): the callback is required;
// "" = this map. [KN] v7 never answered a load without a database (the lock stayed taken);
// KN logs "Error" and releases it.
static KsValue storage_load(KsScript *s, void *user, const KsValue *args, int argc)
{
    (void) user;
    if (ARG(2).tag != KS_FN) {
        log_error_text("Missing callback");
        return ks_undef();
    }
    char name[64], map[160] = "", lock[96], text[160];
    ks_to_string(s, ARG(0), name, sizeof(name));
    size_t map_len = 0;
    const char *map_text = ks_str_utf8(ARG(1), &map_len);
    if (!(map_text && !map_len)) ks_to_string(s, ARG(1), map, sizeof(map)); // mapName == "" -> null, else mapName + ""
    snprintf(lock, sizeof(lock), "str_lod_%s", name);
    if (!lock_take(lock)) {
        snprintf(text, sizeof(text), "storage.load already in progress for %s", name);
        log_error_text(text);
        return ks_undef();
    }
    StorageCall *c = call_new(s, ARG(2), 1, lock, name);
    if (!c) {
        lock_drop(lock);
        return ks_undef();
    }
    const char *err = ks_storage_load(name, map[0] ? map : NULL, storage_done, c);
    if (err) {
        lock_drop(lock);
        log_error_text(err);
        call_free(c, 1);
    }
    return ks_undef();
}

// ------------------------------------------------------------------ GAME.ADMIN (3.1.23)

static ClientConnection *admin_target(KsScript *s, const KsValue id)
{
    Player *player = ks_game_player_by_id(game_of(s), id);
    ClientConnection *client = client_of_player(player);
    if (!client) return NULL;
    if (client->acct.flags & NET_PLAYER_INFO_DEV) return NULL; // account.isDev: never kicked by a script
    return client;
}

static KsValue admin_kick(KsScript *s, void *user, const KsValue *args, int argc)
{
    (void) user;
    ClientConnection *client = admin_target(s, ARG(0));
    if (client) net_kick_client(client->id, "Kicked by host", "004002");
    return ks_undef();
}

static KsValue admin_ban(KsScript *s, void *user, const KsValue *args, int argc)
{
    (void) user;
    ClientConnection *client = admin_target(s, ARG(0));
    if (!client) return ks_undef();
    ChatCommandHost *host = net_chat_host(); // game.banList: this server's IP bans
    if (client->address[0] && !chat_command_ip_banned(host, client->address) && host->ip_ban_count < CHAT_CMD_IP_BANS_MAX)
        snprintf(host->ip_bans[host->ip_ban_count++], sizeof(host->ip_bans[0]), "%s", client->address);
    net_kick_client(client->id, "Banned", "004003");
    return ks_undef();
}

// ------------------------------------------------------------------ PAYMENTS / CONFIG

// PAYMENTS.giveKR(player, amount) (API:3089-3097): game.krRewards, a number > 0, p_givKR 4 per
// second (3 pass), then game.rewardKR. v7 expects the engine player; KN takes a player object or
// a player id. The KR come out of the published map's funds through kn_brain (ks_storage.c);
// offline and on standalone servers there is no KR economy: nothing happens.
static KsValue pay_give_kr(KsScript *s, void *user, const KsValue *args, int argc)
{
    (void) user;
    // game.krRewards (the sandbox has no KR economy; ks_storage_reward_kr ignores it there)
    if (!sandbox_running() && !accounts_kr_rewards()) return ks_undef();
    const KsValue amount = ARG(1);
    const double kr = amount.tag == KS_NUM && isfinite(amount.as.num) ? amount.as.num : 0.0; // UTILS.isNumber
    if (!(kr > 0) || !ks_game_rate(game_of(s), "p_givKR", 4, 0)) return ks_undef();
    Player *player = ks_game_player_by_id(game_of(s), ARG(0));
    if (!player && ARG(0).tag == KS_HOST) player = ks_game_player_by_id(game_of(s), ks_obj_get(s, ARG(0), "id"));
    if (player) ks_storage_reward_kr(player, kr);
    return ks_undef();
}

// ------------------------------------------------------------------ GAME.PAYMENTS (v10 L183384-183500)
// This server has no studio economy: isMonetized() is false, currency and skin grants reach no
// account service, and game.transact logs "Transactions disabled for now". The calls validate and
// rate-limit exactly as v10 does (the same messages, the same windows), then answer their callback
// with success false.
#define KS_CUSTOM_SKIN_BASE 10000000   // v10 customSkinBase (L17249)

// The player an argument names: a player id, or a player host object.
static Player *pay_player(KsScript *s, const KsValue v)
{
    Game *game = game_of(s);
    Player *player = ks_game_player_by_id(game, v);
    if (!player && v.tag == KS_HOST) player = ks_game_player_by_id(game, ks_obj_get(s, v, "id"));
    return player;
}

// callback(playerId, success, value): the first argument is the player's id string ("" if unknown)
static void pay_answer(KsScript *s, const KsValue callback, const KsValue player_arg, Player *player, const int nargs, const KsValue value)
{
    if (callback.tag != KS_FN) return;
    char id[32] = "";
    if (player) {
        ks_player_id(player, id, sizeof id);
    } else if (player_arg.tag == KS_HOST || player_arg.tag == KS_OBJ) { // v10: the argument's own id, if any
        const KsValue raw = ks_obj_get(s, player_arg, "id");
        if (raw.tag != KS_UNDEF && raw.tag != KS_NULL) ks_to_string(s, raw, id, sizeof id);
    }
    const KsValue args[3] = {ks_str(s, id, strlen(id)), ks_bool(0), value};
    ks_call_value(s, callback, args, nargs, NULL);
}

// the callback argument of an answer that needs one (v10: `callb && typeof callb == "function"`)
static KsValue pay_callback(const KsValue v) { return v.tag == KS_FN ? v : ks_undef(); }

static KsValue pay_zero(void) { return ks_num(0); }

// Rate limit key of v10 (`cc_<kind>_<account key>`): the player's id stands for the account
static int pay_rate(KsScript *s, Player *player, const char *kind, int limit)
{
    if (!player) return 1;
    char key[64], id[32];
    ks_player_id(player, id, sizeof id);
    snprintf(key, sizeof key, "cc_%s_%s", kind, id);
    return ks_game_rate(game_of(s), key, limit, 1);
}

static KsValue pay_give_currency(KsScript *s, void *user, const KsValue *args, int argc)
{
    (void) user;
    Player *player = pay_player(s, ARG(0));
    const KsValue cb = pay_callback(ARG(3));
    if (!player) {
        log_error_text("Invalid player");
        pay_answer(s, cb, ARG(0), NULL, 3, ks_bool(0));
        return ks_undef();
    }
    if (ARG(1).tag != KS_STR) {
        log_error_text("Invalid currency name");
        pay_answer(s, cb, ARG(0), player, 3, pay_zero());
        return ks_undef();
    }
    const double amount = ARG(2).tag == KS_NUM && isfinite(ARG(2).as.num) ? ARG(2).as.num : 0.0;
    if (amount > 0) {
        if (!pay_rate(s, player, "giv", 5)) {
            log_error_text("Calling giveCurrency too much");
            pay_answer(s, cb, ARG(0), player, 3, pay_zero());
            return ks_undef();
        }
    }
    pay_answer(s, cb, ARG(0), player, 3, pay_zero());
    return ks_undef();
}

static KsValue pay_take_currency(KsScript *s, void *user, const KsValue *args, int argc)
{
    (void) user;
    Player *player = pay_player(s, ARG(0));
    const KsValue cb = pay_callback(ARG(3));
    if (!player) {
        log_error_text("Invalid player");
        pay_answer(s, cb, ARG(0), NULL, 3, ks_bool(0));
        return ks_undef();
    }
    if (ARG(1).tag != KS_STR) {
        log_error_text("Invalid currency name");
        pay_answer(s, cb, ARG(0), player, 3, pay_zero());
        return ks_undef();
    }
    const double amount = ARG(2).tag == KS_NUM && isfinite(ARG(2).as.num) ? ARG(2).as.num : 0.0;
    if (amount > 0 && !pay_rate(s, player, "tak", 5)) {
        log_error_text("Calling takeCurrency too much");
        pay_answer(s, cb, ARG(0), player, 3, pay_zero());
        return ks_undef();
    }
    pay_answer(s, cb, ARG(0), player, 3, pay_zero());
    return ks_undef();
}

static KsValue pay_currency_balance(KsScript *s, void *user, const KsValue *args, int argc)
{
    (void) user;
    const KsValue cb = pay_callback(ARG(2));
    if (cb.tag != KS_FN) {
        log_error_text("Missing callback");
        return ks_undef();
    }
    Player *player = pay_player(s, ARG(0));
    if (!player) {
        log_error_text("Invalid player");
        pay_answer(s, cb, ARG(0), NULL, 3, pay_zero());
        return ks_undef();
    }
    if (ARG(1).tag != KS_STR) {
        log_error_text("Invalid currency name");
        pay_answer(s, cb, ARG(0), player, 3, pay_zero());
        return ks_undef();
    }
    if (!pay_rate(s, player, "bal", 10)) {
        log_error_text("Calling getCurrencyBalance too much");
        pay_answer(s, cb, ARG(0), player, 3, pay_zero());
        return ks_undef();
    }
    pay_answer(s, cb, ARG(0), player, 3, pay_zero());
    return ks_undef();
}

static KsValue pay_give_skin(KsScript *s, void *user, const KsValue *args, int argc)
{
    (void) user;
    const KsValue cb = pay_callback(ARG(2));
    Player *player = pay_player(s, ARG(0));
    if (!player) {
        log_error_text("Invalid player");
        pay_answer(s, cb, ARG(0), NULL, 2, ks_bool(0));
        return ks_undef();
    }
    const double skin = ARG(1).tag == KS_NUM && isfinite(ARG(1).as.num) ? floor(ARG(1).as.num) : -1;
    if (skin < KS_CUSTOM_SKIN_BASE) {
        log_error_text("Invalid skin id");
        pay_answer(s, cb, ARG(0), player, 2, ks_bool(0));
        return ks_undef();
    }
    if (!pay_rate(s, player, "skn", 5)) {
        log_error_text("Calling giveSkin too much");
        pay_answer(s, cb, ARG(0), player, 2, ks_bool(0));
        return ks_undef();
    }
    pay_answer(s, cb, ARG(0), player, 2, ks_bool(0));
    return ks_undef();
}

static KsValue pay_authorize_spend(KsScript *s, void *user, const KsValue *args, int argc)
{
    (void) user;
    const KsValue cb = pay_callback(ARG(2));
    if (cb.tag != KS_FN) {
        log_error_text("Missing callback");
        return ks_undef();
    }
    Player *player = pay_player(s, ARG(0));
    if (!player) {
        log_error_text("Invalid player");
        pay_answer(s, cb, ARG(0), NULL, 3, ks_str(s, "", 0));
        return ks_undef();
    }
    const double amount = ARG(1).tag == KS_NUM && isfinite(ARG(1).as.num) ? ARG(1).as.num : 0.0;
    if (amount > 0) {
        if (!pay_rate(s, player, "auth", 3)) {
            log_error_text("Calling authorizeSpend too much");
        } else {
            log_error_text("Monetization is not enabled for this studio");
        }
    }
    pay_answer(s, cb, ARG(0), player, 3, ks_str(s, "", 0));
    return ks_undef();
}

static KsValue pay_charge(KsScript *s, void *user, const KsValue *args, int argc)
{
    (void) user;
    const KsValue cb = pay_callback(ARG(2));
    Player *player = pay_player(s, ARG(0));
    if (!player) {
        log_error_text("Invalid player");
        pay_answer(s, cb, ARG(0), NULL, 3, pay_zero());
        return ks_undef();
    }
    const double amount = ARG(1).tag == KS_NUM && isfinite(ARG(1).as.num) ? ARG(1).as.num : 0.0;
    if (amount > 0) {
        if (!pay_rate(s, player, "chg", 3)) {
            log_error_text("Calling charge too much");
        } else {
            log_error_text("Monetization is not enabled for this studio");
        }
    }
    pay_answer(s, cb, ARG(0), player, 3, pay_zero());
    return ks_undef();
}

static KsValue pay_transact(KsScript *s, void *user, const KsValue *args, int argc)
{
    (void) user;
    const KsValue cb = pay_callback(ARG(3));
    Player *player = pay_player(s, ARG(0));
    if (!player) {
        log_error_text("Invalid player");
        pay_answer(s, cb, ARG(0), NULL, 3, pay_zero());
        return ks_undef();
    }
    if (!pay_rate(s, player, "tx", 5)) {
        log_error_text("Calling transact too much");
        pay_answer(s, cb, ARG(0), player, 3, pay_zero());
        return ks_undef();
    }
    log_error_text("Transactions disabled for now"); // game.transact: the callback never runs
    return ks_undef();
}

// CONFIG.getHost (deob-10.0.3.js:211983-212002): players.findById(game.host) on the server, null without a host
// or when the host is not in the game. A custom game's host is game_host_player (server/src/accounts.c keeps
// game.host). KrunkNative difference: the offline sandbox (no game.host, as 10.0.3's local server) answers with the
// local player, whose machine hosts it.
static KsValue cfg_get_host(KsScript *s, void *user, const KsValue *args, int argc)
{
    (void) user; (void) args; (void) argc;
    Player *host = game_host_player(game_of(s));
    if (host) return ks_game_wrap_player(s, game_of(s), host);
    if (!sandbox_running()) return ks_null();
    for (size_t i = 0; i < g_server->client_count; i++) {
        ClientConnection *client = &g_server->clients[i];
        if (client->local && client->player) return ks_game_wrap_player(s, game_of(s), client->player);
    }
    return ks_null();
}

// player.accountID / accountName. The offline sandbox player counts as having an account
// (decisions.md "Offline account"): id "0", name "local", the name its storage is kept under.
static int account_of(void *host_user, const Player *player, char *id, const size_t id_n, char *name, const size_t name_n)
{
    (void) host_user;
    const ClientConnection *client = client_of_player(player);
    if (!ks_storage_account_of(client, name, name_n)) return 0;
    snprintf(id, id_n, "%d", client->acct.account_id > 0 ? client->acct.account_id : 0);
    return 1;
}

// ------------------------------------------------------------------ host

static KsApiImpl g_api[KS_API_COUNT];
static KsHost g_host;

static const KsHost *server_host(void)
{
    if (g_host.api) return &g_host;
    memset(g_api, 0, sizeof(g_api));
    ks_game_api_fill(g_api, KS_SIDE_SERVER);
    g_api[KS_API_NETWORK_SEND].fn = net_send;
    g_api[KS_API_NETWORK_BROADCAST].fn = net_broadcast;
    g_api[KS_API_CHAT_SEND].fn = chat_send;
    g_api[KS_API_CHAT_BROADCAST].fn = chat_broadcast;
    g_api[KS_API_STORAGE_SET].fn = storage_set;
    g_api[KS_API_STORAGE_UPDATE].fn = storage_update;
    g_api[KS_API_STORAGE_TRANSACT].fn = storage_transact;
    g_api[KS_API_STORAGE_LOAD].fn = storage_load;
    g_api[KS_API_ADMIN_KICK].fn = admin_kick;
    g_api[KS_API_ADMIN_BAN].fn = admin_ban;
    g_api[KS_API_PAYMENTS_GIVEKR].fn = pay_give_kr;
    g_api[KS_API_PAYMENTS_GIVECURRENCY].fn = pay_give_currency;
    g_api[KS_API_PAYMENTS_TAKECURRENCY].fn = pay_take_currency;
    g_api[KS_API_PAYMENTS_GETCURRENCYBALANCE].fn = pay_currency_balance;
    g_api[KS_API_PAYMENTS_GIVESKIN].fn = pay_give_skin;
    g_api[KS_API_PAYMENTS_AUTHORIZESPEND].fn = pay_authorize_spend;
    g_api[KS_API_PAYMENTS_CHARGE].fn = pay_charge;
    g_api[KS_API_PAYMENTS_TRANSACT].fn = pay_transact;
    g_api[KS_API_CONFIG_GETHOST].fn = cfg_get_host;
    g_host = (KsHost) {.side = KS_SIDE_SERVER, .log = host_log, .now_ms = host_now, .api = g_api};
    return &g_host;
}

void ks_server_round_init(void)
{
    Game *game = &g_server->game;
    if (!game->ready) return;
    size_t len = 0;
    char *src = ks_game_map_source(game, KS_SIDE_SERVER, &len);
    const uint32_t hash = src ? ks_source_hash(src, len) : 0;
    // scripts.shared (v10 entity simulation) reloads on its own text: the server script keeps its globals
    size_t shared_len = 0;
    char *shared = ks_game_map_source(game, KS_SIDE_SHARED, &shared_len);
    const uint32_t shared_hash = shared ? ks_source_hash(shared, shared_len) : 0;
    if (hash != ks_game_source_hash(game)) {
        ks_game_unload(game); // onServerClosed of the previous map's script (and the shared script)
        storage_reset(); // pending STORAGE callbacks (onServerClosed's too) belong to the old instance
        if (src) {
            const cJSON *name = game->maps && game->current_map_index >= 0 ? cJSON_GetObjectItem(game->maps[game->current_map_index], "name") : NULL;
            server_log("Loading the server script of %s (%zu bytes)", cJSON_IsString(name) ? name->valuestring : "the map", len);
            if (ks_game_load(game, KS_SIDE_SERVER, src, len, server_host(), NULL) == 0) {
                // UTILS.rand* follows the server's RNG stream (pcg32, seeded like the rest of the game)
                ks_game_seed(game, (uint64_t) pcg32_random() << 32 | pcg32_random());
                ks_game_set_account_fn(game, account_of);
            }
        }
        if (shared) ks_game_load_shared(game, KS_SIDE_SERVER, shared, shared_len, server_host(), NULL);
    } else if (shared_hash != ks_game_shared_source_hash(game)) {
        if (shared) server_log("Loading the shared script of %s (%zu bytes)", game->map && game->map->name ? game->map->name : "the map", shared_len);
        ks_game_load_shared(game, KS_SIDE_SERVER, shared, shared_len, server_host(), NULL);
    }
    free(shared);
    free(src);
    ks_game_start(game);
}

static double g_sync_timer;

void ks_server_tick(const float delta)
{
    Game *game = &g_server->game;
    ks_storage_poll(); // STORAGE / Save / Load Data completions, the offline store's writes
    for (size_t i = 0; i < g_server->client_count; i++) { // who is in the match (onPlayerLeave on disconnect)
        if (!g_server->clients[i].player) continue;
        KsConn *c = conn_get(g_server->clients[i].id);
        if (c) c->in_match = 1;
    }
    if (!game->ready) return;
    // the entity clock (no main script needed), then update(delta * deltaMlt); TIME.fixedDelta = the raw tick delta (ms)
    ks_game_update(game, (double) delta * 1000.0, game->config.delta_mlt > 0.0f ? game->config.delta_mlt : 1.0);

    // custom sync values at the state rate (2.6), to their own player only
    g_sync_timer += delta;
    if (g_sync_timer < 1.0 / NET_STATE_SEND_HZ) return;
    g_sync_timer = 0.0;
    if (!ks_game_script(game)) return;
    uint8_t payload[NET_MAX_PAYLOAD];
    for (size_t i = 0; i < g_server->client_count; i++) {
        ClientConnection *client = &g_server->clients[i];
        if (!client->player || !ks_server_peer_v7(client)) continue;
        const size_t n = ks_game_sync_payload(game, client->player, payload, sizeof(payload));
        if (n) net_send_packet(client->fd, NET_PACKET_SCRIPT_SYNC, payload, (uint16_t) n);
    }
}

void ks_server_shutdown(void)
{
    if (!g_server) return;
    ks_game_free(&g_server->game); // onServerClosed, then the instance
    storage_reset();               // its pending STORAGE callbacks are dropped
    ks_storage_shutdown();         // flush the offline store; pending requests end ("Error")
    for (size_t i = 0; i < g_conn_count; i++) ks_net_reassembler_free(&g_conns[i].in);
    free(g_conns);
    g_conns = NULL;
    g_conn_count = g_conn_cap = 0;
    g_log_window = -1;
    g_sync_timer = 0.0;
}

// onPlayerLeave: once per stay in the match. A connection that went spectating already left;
// one without a player right now (dead between rounds) that played still leaves.
void ks_server_on_leave(ClientConnection *client)
{
    if (!client) return;
    KsConn *c = conn_get(client->id);
    if (!client->player && !(c && c->in_match)) return; // never played since its last leave
    if (c) c->in_match = 0;
    ks_game_on_player_leave_uid(&g_server->game, client->player ? client->player->uid : client->id);
}

void ks_server_on_chat(ClientConnection *client, const char *text)
{
    if (client) ks_game_on_chat_message(&g_server->game, client->player, text);
}
