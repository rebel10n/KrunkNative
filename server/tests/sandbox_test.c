// The offline sandbox server (server/include/server_sandbox.h): server_bin's core over the
// in-memory loopback transport, driven by sandbox_step() like the client's main loop does.
// A raw protocol client (framed packets in and out, the same bytes as over TCP) joins, spawns,
// moves, runs chat commands, interacts and leaves; the sandbox restarts cleanly.
// Run from the repository root: ./<build>/sandbox_test
#include <server_sandbox.h>
#include <shared.h>
#include <map_catalog.h>
#include <knet.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_failures;
static int g_checks;

#define CHECK(cond, ...) do { \
    g_checks++; \
    if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_failures++; } \
} while (0)

#define MAX_RECEIVED 8192

typedef struct {
    int fd;
    NetPacketBuffer buffer;
    NetPacket packets[MAX_RECEIVED];
    int count;
    int closed;
    int pongs;
} TestClient;

static void client_send(TestClient *c, const NetPacketType type, const void *payload, const uint16_t length) {
    CHECK(net_send_packet(c->fd, type, payload, length), "send type %d", (int) type);
}

// What client/src/net/offline.c does every frame: step, then drain into packets (PING -> PONG).
static void pump(TestClient *c, const int frames) {
    for (int f = 0; f < frames; f++) {
        sandbox_step(1.0 / 60.0);
        uint8_t bytes[2048];
        int n;
        while ((n = sandbox_receive(c->fd, bytes, sizeof(bytes))) > 0) {
            CHECK(net_buffer_push(&c->buffer, bytes, (size_t) n), "buffer overflow");
            NetPacket packet;
            while (net_buffer_next(&c->buffer, &packet) > 0) {
                if (packet.type == NET_PACKET_PING && packet.length == sizeof(NetPingPacket)) {
                    client_send(c, NET_PACKET_PONG, packet.payload, packet.length);
                    c->pongs++;
                }
                if (c->count < MAX_RECEIVED) c->packets[c->count++] = packet;
            }
        }
        if (n < 0) c->closed = 1;
    }
}

static const NetPacket *find(const TestClient *c, const int type, const int nth) {
    int seen = 0;
    for (int i = 0; i < c->count; i++) {
        if (c->packets[i].type == type && seen++ == nth) return &c->packets[i];
    }
    return NULL;
}

static int count_type(const TestClient *c, const int type) {
    int n = 0;
    for (int i = 0; i < c->count; i++) n += c->packets[i].type == type;
    return n;
}

static const NetPacket *last(const TestClient *c, const int type) {
    for (int i = c->count - 1; i >= 0; i--) {
        if (c->packets[i].type == type) return &c->packets[i];
    }
    return NULL;
}

static int chat_contains(const TestClient *c, const char *needle) {
    for (int i = 0; i < c->count; i++) {
        if (c->packets[i].type != NET_PACKET_CHAT || c->packets[i].length != sizeof(NetChatPacket)) continue;
        NetChatPacket chat;
        memcpy(&chat, c->packets[i].payload, sizeof(chat));
        chat.text[sizeof(chat.text) - 1] = 0;
        if (strstr(chat.text, needle)) return 1;
    }
    return 0;
}

static void join(TestClient *c) {
    const NetHelloPacket hello = {1, 0, 0};
    client_send(c, NET_PACKET_HELLO, &hello, sizeof(hello));
    NetJoinPacket join_packet;
    memset(&join_packet, 0, sizeof(join_packet));
    snprintf(join_packet.name, sizeof(join_packet.name), "Tester");
    client_send(c, NET_PACKET_JOIN, &join_packet, (uint16_t) NET_JOIN_PACKET_V2_SIZE);
    NetEntPacket ent = {.class_index = 0, .secondary_index = -1, .cosmetics = PLAYER_COSMETICS_NONE, .slid_cont = 1, .lag_comp = 1.0f};
    client_send(c, NET_PACKET_ENT, &ent, (uint16_t) NET_ENT_PACKET_V7_SIZE);
}

static void chat(TestClient *c, const char *text) {
    NetChatSendPacket packet;
    memset(&packet, 0, sizeof(packet));
    snprintf(packet.text, sizeof(packet.text), "%s", text);
    client_send(c, NET_PACKET_CHAT_SEND, &packet, (uint16_t) (strlen(packet.text) + 1));
}

static const cJSON **load_maps(size_t *count) {
    // the v3.1.1 Burg and Littletown (catalogue "v3_" entries): /cycle picks its map with the game
    // RNG, which the maps' contents advance, so the test keeps the maps it was written against
    static const cJSON *maps[2];
    maps[0] = map_catalog_json((size_t) map_catalog_index_of_key("v3_burg"));
    maps[1] = map_catalog_json((size_t) map_catalog_index_of_key("v3_littletown"));
    *count = 2;
    return maps;
}

static void test_join_move_leave(void) {
    size_t map_count = 0;
    const cJSON **maps = load_maps(&map_count);
    const SandboxConfig cfg = {.maps = maps, .map_count = map_count, .map_index = 0, .mode_id = -1};
    CHECK(sandbox_start(&cfg), "sandbox_start");
    CHECK(sandbox_running(), "running");

    static TestClient c;
    memset(&c, 0, sizeof(c));
    c.fd = sandbox_connect();
    CHECK(c.fd >= NET_LOOPBACK_FD_BASE, "connect fd %d", c.fd);
    join(&c);
    pump(&c, 30);

    const NetPacket *init_packet = find(&c, NET_PACKET_INIT, 0);
    CHECK(init_packet && init_packet->length == sizeof(NetInitPacket), "INIT");
    NetInitPacket init = {0};
    if (init_packet) memcpy(&init, init_packet->payload, sizeof(init));
    CHECK(init.map_index == 0 && init.mode_index == MODE_ID_FFA && init.tick_rate == 30, "INIT map %d mode %d tick %d", init.map_index,
          init.mode_index, init.tick_rate);
    CHECK(find(&c, NET_PACKET_MAP_INFO, 0) != NULL, "MAP_INFO before INIT");

    NetSpawnPacket spawn = {0};
    int spawned = 0;
    for (int i = 0; i < c.count; i++) {
        if (c.packets[i].type != NET_PACKET_SPAWN) continue;
        memset(&spawn, 0, sizeof(spawn));
        memcpy(&spawn, c.packets[i].payload, c.packets[i].length < sizeof(spawn) ? c.packets[i].length : sizeof(spawn));
        if (spawn.is_you && spawn.active) {
            spawned = 1;
            break;
        }
    }
    CHECK(spawned, "SPAWN is_you");
    CHECK(spawned && !strcmp(spawn.name, "Guest_1"), "standalone guest name: %s", spawn.name); // a standalone server ignores the JOIN name
    CHECK(c.pongs > 0, "PING answered");
    CHECK(find(&c, NET_PACKET_LEADERBOARD, 0) != NULL, "LEADERBOARD");

    // walk forward for a second: RECON acks the inputs, the position moves
    const vec3 start = {spawn.position[0], spawn.position[1], spawn.position[2]};
    for (int seq = 0; seq < 60; seq++) {
        const NetInputPacket input = {.seq = seq, .move_dir = 0, .delta = 1.0f / 60.0f, .x_dir = 0.0f, .y_dir = spawn.direction[1]};
        client_send(&c, NET_PACKET_INPUT, &input, sizeof(input));
        pump(&c, 1);
    }
    pump(&c, 60);
    const NetPacket *recon_packet = last(&c, NET_PACKET_RECON);
    CHECK(recon_packet && recon_packet->length == sizeof(NetReconPacket), "RECON");
    NetReconPacket recon = {0};
    if (recon_packet) memcpy(&recon, recon_packet->payload, sizeof(recon));
    const float moved = hypotf(recon.position[0] - start.x, recon.position[2] - start.z);
    CHECK(recon.ack_seq == 59, "RECON ack %d", recon.ack_seq);
    CHECK(moved > 10.0f, "moved %.2f units", (double) moved);
    CHECK(count_type(&c, NET_PACKET_STATE) > 15, "STATE at NET_STATE_SEND_HZ: %d", count_type(&c, NET_PACKET_STATE));
    CHECK(count_type(&c, NET_PACKET_TIMER) > 0, "TIMER");

    // chat commands run as the host's own machine (DEV): /god answers, plain chat echoes back
    chat(&c, "hello sandbox");
    chat(&c, "/god");
    pump(&c, 61); // io_ct's burst limit runs on the sandbox clock: 3 lines per second
    CHECK(chat_contains(&c, "hello sandbox"), "chat echo");
    CHECK(chat_contains(&c, "God") || chat_contains(&c, "god"), "/god reply");

    // /map switches the map: MAP_INFO + INIT for map 1, the player respawns there
    const int inits = count_type(&c, NET_PACKET_INIT);
    chat(&c, "/cycle");
    pump(&c, 61);
    CHECK(count_type(&c, NET_PACKET_INIT) == inits + 1, "/cycle INIT");
    init_packet = last(&c, NET_PACKET_INIT);
    if (init_packet) memcpy(&init, init_packet->payload, sizeof(init));
    CHECK(init.map_index == 1, "cycled to map %d", init.map_index);

    // host control: Host Game on map 0, Team Deathmatch
    sandbox_host(0, MODE_ID_TDM);
    pump(&c, 5);
    init_packet = last(&c, NET_PACKET_INIT);
    if (init_packet) memcpy(&init, init_packet->payload, sizeof(init));
    CHECK(init.map_index == 0 && init.mode_index == MODE_ID_TDM, "host map %d mode %d", init.map_index, init.mode_index);

    // a second connection joins (Guest_2) and sees the first player; a kick closes it after its ERROR
    static TestClient b;
    memset(&b, 0, sizeof(b));
    b.fd = sandbox_connect();
    CHECK(b.fd >= NET_LOOPBACK_FD_BASE && b.fd != c.fd, "second connection %d", b.fd);
    join(&b);
    for (int f = 0; f < 61; f++) {
        pump(&c, 1);
        pump(&b, 0);
        uint8_t bytes[2048];
        int n;
        while ((n = sandbox_receive(b.fd, bytes, sizeof(bytes))) > 0) {
            net_buffer_push(&b.buffer, bytes, (size_t) n);
            NetPacket packet;
            while (net_buffer_next(&b.buffer, &packet) > 0) {
                if (b.count < MAX_RECEIVED) b.packets[b.count++] = packet;
            }
        }
    }
    CHECK(count_type(&b, NET_PACKET_SPAWN) >= 2, "Guest_2 sees both players: %d SPAWN", count_type(&b, NET_PACKET_SPAWN));
    chat(&c, "/kick Guest_2");
    for (int f = 0; f < 5; f++) {
        pump(&c, 1);
        uint8_t bytes[2048];
        int n;
        while ((n = sandbox_receive(b.fd, bytes, sizeof(bytes))) > 0) {
            net_buffer_push(&b.buffer, bytes, (size_t) n);
            NetPacket packet;
            while (net_buffer_next(&b.buffer, &packet) > 0) {
                if (b.count < MAX_RECEIVED) b.packets[b.count++] = packet;
            }
        }
        if (n < 0) b.closed = 1;
    }
    CHECK(find(&b, NET_PACKET_ERROR, 0) != NULL, "kick ERROR");
    CHECK(b.closed, "kicked: the client end reads -1");
    CHECK(find(&c, NET_PACKET_LEAVE, 0) != NULL, "LEAVE for the kicked player");
    sandbox_disconnect(b.fd);

    sandbox_stop();
    CHECK(!sandbox_running(), "stopped");
    CHECK(sandbox_connect() < 0, "no connections while stopped");
}

static void test_restart_and_disconnect(void) {
    size_t map_count = 0;
    const cJSON **maps = load_maps(&map_count);
    for (int round = 0; round < 3; round++) {
        const SandboxConfig cfg = {.maps = maps, .map_count = map_count, .map_index = 1, .mode_id = MODE_ID_FFA};
        CHECK(sandbox_start(&cfg), "restart %d", round);
        static TestClient c;
        memset(&c, 0, sizeof(c));
        c.fd = sandbox_connect();
        join(&c);
        pump(&c, 20);
        int spawned = 0;
        for (int i = 0; i < c.count; i++) {
            if (c.packets[i].type != NET_PACKET_SPAWN) continue;
            NetSpawnPacket spawn = {0};
            memcpy(&spawn, c.packets[i].payload, c.packets[i].length < sizeof(spawn) ? c.packets[i].length : sizeof(spawn));
            spawned |= spawn.is_you && spawn.active;
        }
        CHECK(spawned, "spawned after restart %d", round);
        CHECK(count_type(&c, NET_PACKET_STATE) > 0, "states after restart %d (the clock never goes back)", round);

        // the client leaves: the server removes the connection, a new one gets a fresh slot
        sandbox_disconnect(c.fd);
        pump(&c, 2);
        const int again = sandbox_connect();
        CHECK(again >= NET_LOOPBACK_FD_BASE, "reconnect %d", round);
        sandbox_disconnect(again);
        sandbox_step(1.0 / 60.0);
        sandbox_stop();
    }
}

int main(void) {
    test_join_move_leave();
    test_restart_and_disconnect();
    if (g_failures) {
        printf("%d of %d checks failed\n", g_failures, g_checks);
        return 1;
    }
    printf("Test OK (%d checks)\n", g_checks);
    return 0;
}
