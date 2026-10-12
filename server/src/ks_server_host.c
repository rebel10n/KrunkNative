// The server's KrunkScript host (server/include/ks_server.h, doc/v7/krunkscript.md 2.1-2.8, 3.x,
// 4.1, 6.5): the map's scripts.server runs in g_server->game (dedicated server and offline
// sandbox alike). Shared code (shared/src/ks_game) owns the script lifecycle, the hook points
// inside the game and the both-side API; this file adds what needs the connections: the
// console (GAME.log -> server log + NET_PACKET_SCRIPT_LOG), GAME.NETWORK, GAME.CHAT,
// GAME.ADMIN, GAME.PAYMENTS, GAME.CONFIG.getHost, inbound script messages and the custom
// sync values. KrunkScript packets go only to peers that announced NET_PROTOCOL_MINOR_V7.
#include <ks_server.h>
#include <ks_game.h>
#include <ks_net.h>
#include <server_log.h>
#include <server_accounts.h>
#include <server_sandbox.h>
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
    if (!client || !packet || packet->length < sizeof(NetHelloPacket)) return;
    NetHelloPacket hello;
    memcpy(&hello, packet->payload, sizeof(hello));
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

// PAYMENTS.giveKR: needs the host option game.krRewards, which KN's hosted games do not have
// (no KR economy offline; kn_brain has no script reward endpoint) -> always a no-op.
static KsValue pay_give_kr(KsScript *s, void *user, const KsValue *args, int argc)
{
    (void) s; (void) user; (void) args; (void) argc;
    return ks_undef();
}

// CONFIG.getHost: game.host. KN has no hosted custom games on the dedicated server (null);
// the offline sandbox is hosted by the local player.
static KsValue cfg_get_host(KsScript *s, void *user, const KsValue *args, int argc)
{
    (void) user; (void) args; (void) argc;
    if (!sandbox_running()) return ks_null();
    for (size_t i = 0; i < g_server->client_count; i++) {
        ClientConnection *client = &g_server->clients[i];
        if (client->local && client->player) return ks_game_wrap_player(s, game_of(s), client->player);
    }
    return ks_null();
}

static int account_of(void *host_user, const Player *player, char *id, const size_t id_n, char *name, const size_t name_n)
{
    (void) host_user;
    const ClientConnection *client = client_of_player(player);
    if (!client || client->acct.account_id <= 0) return 0;
    snprintf(id, id_n, "%d", client->acct.account_id);
    snprintf(name, name_n, "%s", client->acct.username);
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
    g_api[KS_API_ADMIN_KICK].fn = admin_kick;
    g_api[KS_API_ADMIN_BAN].fn = admin_ban;
    g_api[KS_API_PAYMENTS_GIVEKR].fn = pay_give_kr;
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
    if (hash != ks_game_source_hash(game)) {
        ks_game_unload(game); // onServerClosed of the previous map's script
        if (src) {
            const cJSON *name = game->maps && game->current_map_index >= 0 ? cJSON_GetObjectItem(game->maps[game->current_map_index], "name") : NULL;
            server_log("Loading the server script of %s (%zu bytes)", cJSON_IsString(name) ? name->valuestring : "the map", len);
            if (ks_game_load(game, KS_SIDE_SERVER, src, len, server_host(), NULL) == 0) {
                // UTILS.rand* follows the server's RNG stream (pcg32, seeded like the rest of the game)
                ks_game_seed(game, (uint64_t) pcg32_random() << 32 | pcg32_random());
                ks_game_set_account_fn(game, account_of);
            }
        }
    }
    free(src);
    ks_game_start(game);
}

static double g_sync_timer;

void ks_server_tick(const float delta)
{
    Game *game = &g_server->game;
    for (size_t i = 0; i < g_server->client_count; i++) { // who is in the match (onPlayerLeave on disconnect)
        if (!g_server->clients[i].player) continue;
        KsConn *c = conn_get(g_server->clients[i].id);
        if (c) c->in_match = 1;
    }
    if (!ks_game_script(game) || !game->ready) return;
    // update(delta * deltaMlt); TIME.fixedDelta = the raw tick delta (ms)
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
