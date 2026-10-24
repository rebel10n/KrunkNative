// The offline sandbox server (server/include/server_sandbox.h): server_bin's core over the
// in-memory loopback transport, driven by sandbox_step() like the client's main loop does.
// A raw protocol client (framed packets in and out, the same bytes as over TCP) joins, spawns,
// moves, runs chat commands, interacts and leaves; the sandbox restarts cleanly.
// Run from the repository root: ./<build>/sandbox_test
#include <server_sandbox.h>
#include <server.h>
#include <shared.h>
#include <map_catalog.h>
#include <knet.h>
#include <ai.h>   // NET_PACKET_AI_SHOT / NET_PACKET_AI_ANIM
#include <bots.h> // bot players for the dead-filter match
#include <social_v7.h> // NET_PACKET_CHAT_V7
#include <ruleset.h>   // test_class: the class table's health
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
    unsigned long long bytes;  // every framed byte received (packets[] stops at MAX_RECEIVED)
    int type_count[256];       // every packet received, by NetPacketType
    unsigned long long type_bytes[256]; // and their framed bytes
} TestClient;

static void client_send(TestClient *c, const NetPacketType type, const void *payload, const uint16_t length) {
    CHECK(net_send_packet(c->fd, type, payload, length), "send type %d", (int) type);
}

// What client/src/net/offline.c does every frame: step, then drain into packets (PING -> PONG).
static void pump(TestClient *c, const int frames) {
    for (int f = 0; f < (frames ? frames : 1); f++) {
        if (frames) sandbox_step(1.0 / 60.0); // 0: only drain what is waiting
        uint8_t bytes[2048];
        int n;
        while ((n = sandbox_receive(c->fd, bytes, sizeof(bytes))) > 0) {
            CHECK(net_buffer_push(&c->buffer, bytes, (size_t) n), "buffer overflow");
            c->bytes += (unsigned long long) n;
            NetPacket packet;
            while (net_buffer_next(&c->buffer, &packet) > 0) {
                c->type_count[packet.type & 0xff]++;
                c->type_bytes[packet.type & 0xff] += NET_PACKET_HEADER_SIZE + packet.length;
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
    // the v3.1.1 Burg and Littletown (catalogue "311_" entries): /cycle picks its map with the game
    // RNG, which the maps' contents advance, so the test keeps the maps it was written against
    static const cJSON *maps[2];
    maps[0] = map_catalog_json((size_t) map_catalog_index_of_key("311_burg"));
    maps[1] = map_catalog_json((size_t) map_catalog_index_of_key("311_littletown"));
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
    CHECK(recon_packet && recon_packet->length == NET_RECON_PACKET_V1_SIZE, "RECON (a 1.0 peer: V1)");
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

// WP-C protocol 1.3.1 (NET_PROTOCOL_PATCH) on the offline path: a 1.3.0 peer (and a 2-byte HELLO,
// patch 0) keeps the V7 ENT, V2 SPAWN and V1 RECON it reads; a 1.3.1 peer sends ENT V8 and gets
// SPAWN V3 (attachIndex, headMlt) and RECON V2 (wallJumpC, canSlideT, slide ms), also about others.
static const NetPacket *spawn_of(const TestClient *c, const int is_you, NetSpawnPacket *out) {
    for (int i = c->count - 1; i >= 0; i--) {
        const NetPacket *packet = &c->packets[i];
        if (packet->type != NET_PACKET_SPAWN || !net_packet_read_spawn(out, packet) || out->is_you != is_you || !out->active) continue;
        return packet;
    }
    return NULL;
}

static void mix_join(TestClient *c, const NetHelloPacket *hello, const uint16_t hello_size, const NetEntPacket *ent, const uint16_t ent_size) {
    memset(c, 0, sizeof(*c));
    c->fd = sandbox_connect();
    client_send(c, NET_PACKET_HELLO, hello, hello_size);
    NetJoinPacket join_packet;
    memset(&join_packet, 0, sizeof(join_packet));
    client_send(c, NET_PACKET_JOIN, &join_packet, (uint16_t) NET_JOIN_PACKET_V2_SIZE);
    client_send(c, NET_PACKET_ENT, ent, ent_size);
}

static void mix_move(TestClient *c, const NetSpawnPacket *me) {
    for (int seq = 0; seq < 30; seq++) {
        const NetInputPacket input = {.seq = seq, .move_dir = 0, .delta = 1.0f / 60.0f, .y_dir = me->direction[1]};
        client_send(c, NET_PACKET_INPUT, &input, sizeof(input));
    }
}

static void test_wpc_peer_mix(void) {
    size_t map_count = 0;
    const cJSON **maps = load_maps(&map_count);
    const SandboxConfig cfg = {.maps = maps, .map_count = map_count, .map_index = 0, .mode_id = -1};
    CHECK(sandbox_start(&cfg), "sandbox_start (peer mix)");

    static TestClient old, bare, now;
    const NetHelloPacket hello_old = {NET_PROTOCOL_MAJOR, NET_PROTOCOL_MINOR_V7, 0};
    const NetHelloPacket hello_new = {NET_PROTOCOL_MAJOR, NET_PROTOCOL_MINOR, NET_PROTOCOL_PATCH};
    const NetEntPacket ent_old = {.class_index = 0, .secondary_index = -1, .cosmetics = PLAYER_COSMETICS_NONE, .slid_cont = 1, .lag_comp = 1.0f};
    NetEntPacket ent_new = ent_old;
    ent_new.attach_index = 1;
    ent_new.chall_mode = 1;
    ent_new.anon_mode = 1;
    // WP-F: a HEAD item in the v3.1.1 hat slot, the pet and shoes in ENT V9; the custom badge hidden
    ent_new.cosmetics_v7 = (PlayerCosmeticsV7) {.head = -1, .back = -1, .shoe = 2860, .pet = 2964, .wrist = -1, .charms = {-1, -1}, .player_card = -1, .badge = 7};
    ent_new.show_badges = PLAYER_SHOW_BADGES_ALL & ~PLAYER_SHOW_BADGE_CUSTOM;
    mix_join(&old, &hello_old, sizeof(hello_old), &ent_old, (uint16_t) NET_ENT_PACKET_V7_SIZE);
    mix_join(&bare, &hello_old, 2, &ent_old, (uint16_t) NET_ENT_PACKET_V7_SIZE); // major.minor only: patch 0
    mix_join(&now, &hello_new, sizeof(hello_new), &ent_new, (uint16_t) NET_ENT_PACKET_V9_SIZE);
    for (int f = 0; f < 30; f++) {
        pump(&old, 1);
        pump(&bare, 1);
        pump(&now, 1);
    }

    const NetPacket *init = last(&now, NET_PACKET_INIT);
    NetInitPacket init_now = {0};
    CHECK(init && net_packet_read_init(&init_now, init) && init_now.minor == NET_PROTOCOL_MINOR && init_now.patch == NET_PROTOCOL_PATCH, "INIT announces 1.%d.%d", init_now.minor, init_now.patch);
    CHECK(net_ent_size_for(init_now.major, init_now.minor, init_now.patch) == NET_ENT_PACKET_V9_SIZE, "a 1.3.1 server reads ENT V9");

    NetSpawnPacket me_old, me_bare, me_now, seen;
    const NetPacket *packet = spawn_of(&old, 1, &me_old);
    CHECK(packet && packet->length == NET_SPAWN_PACKET_V2_SIZE, "1.3.0 peer: its V7 ENT spawns it, SPAWN V2 (%u)", packet ? packet->length : 0);
    packet = spawn_of(&bare, 1, &me_bare);
    CHECK(packet && packet->length == NET_SPAWN_PACKET_V2_SIZE, "2-byte HELLO: patch 0, SPAWN V2 (%u)", packet ? packet->length : 0);
    packet = spawn_of(&now, 1, &me_now);
    CHECK(packet && packet->length == NET_SPAWN_PACKET_V4_SIZE && me_now.attach_index == 1 && me_now.head_mlt == 1.0f,
          "1.3.1 peer: SPAWN V4 with its ENT V9 attachIndex %d, headMlt %.2f", packet ? me_now.attach_index : -9, (double) me_now.head_mlt);
    CHECK(packet && me_now.cosmetics_v7.pet == 2964 && me_now.cosmetics_v7.shoe == 2860 && me_now.cosmetics_v7.badge == -1,
          "1.3.1 peer: SPAWN V4 carries its pet %d / shoes %d, no badge (sandbox guest, hidden) %d", me_now.cosmetics_v7.pet, me_now.cosmetics_v7.shoe, me_now.cosmetics_v7.badge);
    // the 1.3.0 peer sees the 1.3.1 player's SPAWN as V2; the 1.3.1 peer the 1.3.0 one's as V3 (attachIndex 0)
    int old_sees_new = 0, new_sees_old = 0;
    for (int i = 0; i < old.count; i++) {
        if (old.packets[i].type == NET_PACKET_SPAWN && net_packet_read_spawn(&seen, &old.packets[i]) && seen.uid == me_now.uid && seen.active)
            old_sees_new = old.packets[i].length == NET_SPAWN_PACKET_V2_SIZE;
    }
    for (int i = 0; i < now.count; i++) {
        if (now.packets[i].type == NET_PACKET_SPAWN && net_packet_read_spawn(&seen, &now.packets[i]) && seen.uid == me_old.uid && seen.active)
            new_sees_old = now.packets[i].length == NET_SPAWN_PACKET_V4_SIZE && seen.attach_index == DEFAULT_ATTACHMENT_INDEX && seen.cosmetics_v7.pet == -1;
    }
    CHECK(old_sees_new && new_sees_old, "SPAWN of the other peer by the receiver's protocol (old %d new %d)", old_sees_new, new_sees_old);

    mix_move(&old, &me_old);
    mix_move(&now, &me_now);
    for (int f = 0; f < 90; f++) {
        pump(&old, 1);
        pump(&bare, 1);
        pump(&now, 1);
    }
    const NetPacket *recon_old = last(&old, NET_PACKET_RECON);
    const NetPacket *recon_now = last(&now, NET_PACKET_RECON);
    CHECK(recon_old && recon_old->length == NET_RECON_PACKET_V1_SIZE, "1.3.0 peer: RECON V1 (%u)", recon_old ? recon_old->length : 0);
    CHECK(recon_now && recon_now->length == NET_RECON_PACKET_V3_SIZE, "1.3.1 peer: RECON V3 (the v7 locks + the grapple, %u)", recon_now ? recon_now->length : 0);
    NetReconPacket recon;
    CHECK(recon_now && net_packet_read_recon(&recon, recon_now, NULL) && recon.ack_seq == 29 && recon.wall_jump_count == 0 &&
          recon.can_slide_timer == 0.0f && (float) (recon.slide_ms / 1000.0) == recon.slide_timer, "RECON V2 movement locks");
    CHECK(!old.closed && !bare.closed && !now.closed, "nobody dropped");
    sandbox_stop();
}

// STATE packets in c->packets[from..] about `uid` (-1: any)
static int states_about(const TestClient *c, const int from, const int uid) {
    int n = 0;
    for (int i = from; i < c->count; i++) {
        if (c->packets[i].type != NET_PACKET_STATE || c->packets[i].length != sizeof(NetStatePacket)) continue;
        NetStatePacket state;
        memcpy(&state, c->packets[i].payload, sizeof(state));
        n += uid < 0 || state.uid == uid;
    }
    return n;
}

static int index_of_kill(const TestClient *c, const int from, const int victim_uid, NetKillPacket *out) {
    for (int i = from; i < c->count; i++) {
        if (c->packets[i].type != NET_PACKET_KILL || c->packets[i].length != sizeof(NetKillPacket)) continue;
        memcpy(out, c->packets[i].payload, sizeof(*out));
        if (out->victim_uid == victim_uid) return i;
    }
    return -1;
}

static int index_of_spawn(const TestClient *c, const int from, const int uid) {
    for (int i = from; i < c->count; i++) {
        NetSpawnPacket spawn;
        if (c->packets[i].type == NET_PACKET_SPAWN && net_packet_read_spawn(&spawn, &c->packets[i]) && spawn.uid == uid && spawn.active) return i;
    }
    return -1;
}

static const Player *server_player(const int uid) {
    for (size_t i = 0; i < g_server->game.player_count; i++) {
        if (g_server->game.players[i] && g_server->game.players[i]->uid == uid) return g_server->game.players[i];
    }
    return NULL;
}

// v7.1.4 game.js updateState: the "k" player sync goes only to a connection whose player is
// active or that spectates (and is not locked), and lists only active players. Between its
// death ("3") and its respawn a player gets no player positions at all, and nobody gets the
// dead player's; the server drops the dead player's inputs (net_handle_input_frame).
static void test_dead_player_sync(void) {
    size_t map_count = 0;
    const cJSON **maps = load_maps(&map_count);
    const SandboxConfig cfg = {.maps = maps, .map_count = map_count, .map_index = 0, .mode_id = MODE_ID_FFA};
    CHECK(sandbox_start(&cfg), "sandbox_start (dead sync)");

    static TestClient a, b;
    const NetHelloPacket hello = {NET_PROTOCOL_MAJOR, NET_PROTOCOL_MINOR_V7, 0}; // 1.3.0: ENT V7
    const NetEntPacket ent = {.class_index = 0, .secondary_index = -1, .cosmetics = PLAYER_COSMETICS_NONE, .slid_cont = 1, .lag_comp = 1.0f};
    mix_join(&a, &hello, sizeof(hello), &ent, (uint16_t) NET_ENT_PACKET_V7_SIZE);
    pump(&a, 30);
    mix_join(&b, &hello, sizeof(hello), &ent, (uint16_t) NET_ENT_PACKET_V7_SIZE);
    for (int f = 0; f < 60; f++) {
        pump(&a, 1);
        pump(&b, 0);
    }

    NetSpawnPacket me_a = {0}, me_b = {0};
    CHECK(spawn_of(&a, 1, &me_a) && spawn_of(&b, 1, &me_b) && me_a.uid != me_b.uid, "both spawned (%d, %d)", me_a.uid, me_b.uid);
    CHECK(states_about(&b, 0, me_a.uid) > 5, "alive: B gets A's position (%d)", states_about(&b, 0, me_a.uid));
    CHECK(states_about(&a, 0, me_b.uid) > 5, "alive: A gets B's position (%d)", states_about(&a, 0, me_b.uid));

    chat(&b, "/kill");
    for (int f = 0; f < 10; f++) {
        pump(&a, 1);
        pump(&b, 0);
    }
    NetKillPacket kill = {0};
    const int kill_a = index_of_kill(&a, 0, me_b.uid, &kill);
    const int kill_b = index_of_kill(&b, 0, me_b.uid, &kill);
    CHECK(kill_a >= 0 && kill_b >= 0, "KILL of B reaches both (%d, %d)", kill_a, kill_b);
    const Player *dead = server_player(me_b.uid);
    CHECK(dead && !dead->active, "B is dead on the server");
    const vec3 corpse = dead ? dead->position : (vec3) {0};

    // the death screen until the respawn delay is over; B's client keeps sending movement (an
    // old client, or a stale batch): the server must not move the dead player
    const int dead_frames = (int) fmaxf(120.0f, (kill.respawn_delay + 0.5f) * 60.0f);
    for (int f = 0; f < dead_frames; f++) {
        const NetInputPacket input = {.seq = 1000 + f, .move_dir = 0, .delta = 1.0f / 60.0f, .y_dir = me_b.direction[1]};
        client_send(&b, NET_PACKET_INPUT, &input, sizeof(input));
        pump(&a, 1);
        pump(&b, 0);
    }
    if (kill_a >= 0 && kill_b >= 0) {
        CHECK(states_about(&b, kill_b + 1, -1) == 0, "dead: B gets no player positions (%d STATE)", states_about(&b, kill_b + 1, -1));
        CHECK(states_about(&a, kill_a + 1, me_b.uid) == 0, "dead: nobody gets B's position (%d STATE)", states_about(&a, kill_a + 1, me_b.uid));
        CHECK(states_about(&a, kill_a + 1, me_a.uid) > 5, "alive: A's sync goes on (%d)", states_about(&a, kill_a + 1, me_a.uid));
        int recon = 0;
        for (int i = kill_b + 1; i < b.count; i++) recon += b.packets[i].type == NET_PACKET_RECON;
        CHECK(recon == 0, "dead: no RECON to B (%d)", recon);
    }
    dead = server_player(me_b.uid);
    CHECK(dead && !dead->active && dead->position.x == corpse.x && dead->position.y == corpse.y && dead->position.z == corpse.z,
          "dead: B's inputs are ignored");

    // respawn (ENT after the death delay): the sync resumes both ways
    const int before_a = a.count, before_b = b.count;
    client_send(&b, NET_PACKET_ENT, &ent, (uint16_t) NET_ENT_PACKET_V7_SIZE);
    for (int f = 0; f < 60; f++) {
        pump(&a, 1);
        pump(&b, 0);
    }
    const int spawn_b = index_of_spawn(&b, before_b, me_b.uid);
    CHECK(spawn_b >= 0, "B respawned");
    CHECK(spawn_b >= 0 && states_about(&b, spawn_b, me_a.uid) > 5, "respawned: B gets A's position again");
    CHECK(states_about(&a, before_a, me_b.uid) > 5, "respawned: A gets B's position again (%d)", states_about(&a, before_a, me_b.uid));
    CHECK(!a.closed && !b.closed, "nobody dropped (dead sync)");
    sandbox_stop();
}

static ClientConnection *server_connection_last(void) {
    return g_server->client_count ? &g_server->clients[g_server->client_count - 1] : NULL;
}

static void pump_both(TestClient *a, TestClient *b, const int frames) {
    for (int f = 0; f < frames; f++) {
        pump(a, 1);
        pump(b, 0);
    }
}

static int got_idle_kick(const TestClient *c) {
    for (int i = 0; i < c->count; i++) {
        if (c->packets[i].type != NET_PACKET_ERROR || c->packets[i].length != sizeof(NetErrorPacket)) continue;
        NetErrorPacket error;
        memcpy(&error, c->packets[i].payload, sizeof(error));
        if (!strcmp(error.code, "000005")) return 1;
    }
    return 0;
}

// "i switched a map and it kicked me for inactivity even tho im the host". The server idle kick
// (net_check_idle_clients): a map change restarts every unspawned connection's clock (v3.1.1
// markPlayerUnspawned; it used to keep counting from the connect, so whoever the new map did
// not respawn was kicked at once), and the host (here the sandbox's own player, a DEV peer)
// is never kicked, as v7.1.4 never kicks the game host or developers.
static void test_idle_kick_map_change(void) {
    size_t map_count = 0;
    const cJSON **maps = load_maps(&map_count);
    const SandboxConfig cfg = {.maps = maps, .map_count = map_count, .map_index = 0, .mode_id = MODE_ID_FFA};
    CHECK(sandbox_start(&cfg), "sandbox_start (idle)");
    g_server->config.idle_timeout = 2.0f; // the sandbox has no idle kick: a server_bin's, shortened
    g_server->config.idle_menu_timeout = 2.0f; // the guest never spawns: the dead / menu clock

    static TestClient host, guest;
    memset(&host, 0, sizeof(host));
    host.fd = sandbox_connect();
    join(&host);
    pump(&host, 30);
    NetSpawnPacket me = {0};
    CHECK(spawn_of(&host, 1, &me), "host spawned (idle)");

    // a remote guest (not this machine: no DEV) who stays in the menu (no ENT)
    memset(&guest, 0, sizeof(guest));
    guest.fd = sandbox_connect();
    const NetHelloPacket hello = {1, 0, 0};
    client_send(&guest, NET_PACKET_HELLO, &hello, sizeof(hello));
    pump_both(&host, &guest, 1);
    ClientConnection *remote = server_connection_last();
    CHECK(remote && remote->id != me.uid, "guest connection");
    if (remote) remote->local = 0;

    // the host dies, then switches the map 1.5 s after the guest connected: the new map does
    // not respawn the dead host, who sits in the menu
    chat(&host, "/kill");
    pump_both(&host, &guest, 89);
    sandbox_host(1, -1);
    pump_both(&host, &guest, 90); // 3 s after the connect, 1.5 s after the map change
    CHECK(!guest.closed && !got_idle_kick(&guest), "a map change restarts the idle clock (guest not kicked 3 s after connecting)");
    CHECK(!host.closed && !got_idle_kick(&host), "host not kicked right after the map change");

    pump_both(&host, &guest, 60); // 2.5 s after the map change
    CHECK(guest.closed && got_idle_kick(&guest), "an idle guest is still kicked, 2 s after the map change");
    pump_both(&host, &guest, 240); // the host idles 6.5 s past the map change
    CHECK(!host.closed && !got_idle_kick(&host), "the host is never idle-kicked");

    // the host spawns on the new map and idles there: still connected
    const NetEntPacket ent = {.class_index = 0, .secondary_index = -1, .cosmetics = PLAYER_COSMETICS_NONE, .slid_cont = 1, .lag_comp = 1.0f};
    client_send(&host, NET_PACKET_ENT, &ent, (uint16_t) NET_ENT_PACKET_V7_SIZE);
    pump(&host, 300);
    CHECK(!host.closed && !got_idle_kick(&host) && server_player(me.uid) && server_player(me.uid)->active,
          "the host idles in game past the timeout");
    sandbox_disconnect(guest.fd);
    sandbox_stop();
}

static int changed_position(const TestClient *c, const int from, const int uid, const vec3 to) {
    for (int i = from; i < c->count; i++) {
        if (c->packets[i].type != NET_PACKET_CHANGE_POSITION || c->packets[i].length != sizeof(NetChangePositionPacket)) continue;
        NetChangePositionPacket packet;
        memcpy(&packet, c->packets[i].payload, sizeof(packet));
        if (packet.uid == uid && packet.x == to.x && packet.y == to.y && packet.z == to.z && packet.recon && packet.stop_momentum) return 1;
    }
    return 0;
}

static int at(const Player *p, const vec3 to) {
    return p && p->position.x == to.x && p->position.y == to.y && p->position.z == to.z;
}

static int notified(const TestClient *c, const int from, const char *text) {
    for (int i = from; i < c->count; i++) {
        if (c->packets[i].type != NET_PACKET_NOTIFY) continue;
        NetNotifyPacket notify;
        memset(&notify, 0, sizeof(notify));
        memcpy(&notify, c->packets[i].payload, c->packets[i].length < sizeof(notify) ? c->packets[i].length : sizeof(notify));
        if (strstr(notify.text, text)) return 1;
    }
    return 0;
}

// tp / bring (shared/src/chat_commands.c, doc/console.md "Teleports"): the host's commands move
// players server-side (changePosition with doRecon: POSITION to the owner, CHANGE_POSITION to
// everyone); a guest is refused; unknown names answer.
// One command line, then a frame for both. The chat burst limit (NET_CHAT_BURST_MAX lines per
// wall-clock second) would close a test that types faster than a player: it starts over each time.
static void command(TestClient *from, TestClient *a, TestClient *b, const char *text) {
    for (size_t i = 0; i < g_server->client_count; i++) memset(&g_server->clients[i].chat_limiter, 0, sizeof(g_server->clients[i].chat_limiter));
    chat(from, text);
    pump_both(a, b, 1);
}

static void test_tp_bring(void) {
    size_t map_count = 0;
    const cJSON **maps = load_maps(&map_count);
    const SandboxConfig cfg = {.maps = maps, .map_count = map_count, .map_index = 0, .mode_id = MODE_ID_FFA};
    CHECK(sandbox_start(&cfg), "sandbox_start (tp)");

    static TestClient host, guest;
    memset(&host, 0, sizeof(host));
    host.fd = sandbox_connect();
    join(&host);
    pump(&host, 30);
    memset(&guest, 0, sizeof(guest));
    guest.fd = sandbox_connect();
    join(&guest);
    pump_both(&host, &guest, 1);
    ClientConnection *remote = server_connection_last();
    if (remote) remote->local = 0; // a remote guest: no DEV, not the host
    pump_both(&host, &guest, 60);
    NetSpawnPacket me = {0}, other = {0};
    CHECK(spawn_of(&host, 1, &me) && spawn_of(&guest, 1, &other) && me.uid != other.uid, "both spawned (tp)");
    Player *ph = (Player *) server_player(me.uid), *pg = (Player *) server_player(other.uid);
    CHECK(ph && pg, "server players");
    if (!ph || !pg) {
        sandbox_stop();
        return;
    }

    int mark = guest.count;
    command(&host, &host, &guest, "/tp 10 20.5 -30");
    const vec3 p1 = {10.0f, 20.5f, -30.0f};
    CHECK(at(ph, p1), "tp x y z: host at %.2f %.2f %.2f", (double) ph->position.x, (double) ph->position.y, (double) ph->position.z);
    CHECK(ph->velocity.x == 0.0f && ph->velocity.y == 0.0f && ph->velocity.z == 0.0f, "tp stops momentum");
    CHECK(find(&host, NET_PACKET_POSITION, 0) != NULL, "tp: POSITION to the owner (doRecon)");
    CHECK(changed_position(&guest, mark, me.uid, p1), "tp: CHANGE_POSITION to the others");
    CHECK(chat_contains(&host, "Teleported to 10.00 20.50 -30.00."), "tp x y z reply");

    const vec3 guest_at = pg->position;
    command(&host, &host, &guest, "/tp guest_2");
    CHECK(at(ph, guest_at), "tp player: host at the guest");
    CHECK(chat_contains(&host, "Teleported to Guest_2."), "tp player reply");

    mark = guest.count;
    const vec3 p2 = {-5.0f, 40.0f, 7.25f};
    command(&host, &host, &guest, "/bring Guest_2 -5 40 7.25");
    CHECK(at(pg, p2), "bring player x y z");
    CHECK(changed_position(&guest, mark, other.uid, p2), "bring: the guest's client gets CHANGE_POSITION");
    CHECK(find(&guest, NET_PACKET_POSITION, 0) != NULL, "bring: POSITION to the moved player");
    CHECK(chat_contains(&host, "Brought Guest_2 to -5.00 40.00 7.25."), "bring x y z reply");

    command(&host, &host, &guest, "/bring GUEST_2");
    CHECK(at(pg, ph->position), "bring player: to the host (case-insensitive name)");
    CHECK(chat_contains(&host, "Brought Guest_2 to you."), "bring player reply");

    const vec3 host_before = ph->position, p3 = {1.0f, 2.0f, 3.0f};
    command(&host, &host, &guest, "/bring all 1 2 3");
    CHECK(at(pg, p3) && at(ph, host_before), "bring all x y z: everyone but the host");
    CHECK(chat_contains(&host, "Brought 1 player to 1.00 2.00 3.00."), "bring all reply");

    command(&host, &host, &guest, "/tp nobody");
    command(&host, &host, &guest, "/bring nobody");
    command(&host, &host, &guest, "/tp");
    CHECK(chat_contains(&host, "Player 'nobody' not found."), "unknown name");
    CHECK(chat_contains(&host, "Usage: tp <x> <y> <z> | tp <player>"), "tp usage");

    // a guest may not: notify, nobody moves
    const vec3 guest_before = pg->position, host_now = ph->position;
    mark = guest.count;
    command(&guest, &host, &guest, "/tp 100 100 100");
    command(&guest, &host, &guest, "/bring all");
    command(&guest, &host, &guest, "/bring Guest_1 0 0 0");
    CHECK(at(pg, guest_before) && at(ph, host_now), "guest tp / bring: nobody moved");
    CHECK(notified(&guest, mark, "Host or developer required"), "guest tp / bring: permission denied");

    // bots are players: bring one by its id
    command(&host, &host, &guest, "/bot_add 1");
    command(&host, &host, &guest, "/bot_stop on");
    pump_both(&host, &guest, 90);
    Player *bot = NULL;
    for (size_t i = 0; i < g_server->game.player_count; i++) {
        Player *p = g_server->game.players[i];
        if (p && p->is_bot && p->active) bot = p;
    }
    CHECK(bot != NULL, "bot spawned");
    if (bot) {
        char line[64];
        snprintf(line, sizeof(line), "/bring %d", bot->uid);
        mark = guest.count;
        command(&host, &host, &guest, line);
        CHECK(changed_position(&guest, mark, bot->uid, ph->position), "bring a bot by id");
    }
    CHECK(!host.closed && !guest.closed, "nobody dropped (tp)");
    sandbox_disconnect(guest.fd);
    sandbox_stop();
}

// The packets Krunker v7.1.4 sends only to `player.active || player.spectating` (server/src/net.c
// net_live_only_packet): player sync, tracers, projectile launches, explosions, broadcast
// sounds, object / AI clips.
static const int k_live_only[] = {NET_PACKET_STATE, NET_PACKET_SHOT, NET_PACKET_PROJECTILE, NET_PACKET_AI_SHOT,
                                  NET_PACKET_EXPLOSION, NET_PACKET_SOUND, NET_PACKET_OBJECT_ANIM, NET_PACKET_AI_ANIM};

static int live_only_after(const TestClient *c, const int from, char *first, const size_t size) {
    int n = 0;
    for (int i = from; i < c->count; i++) {
        for (size_t k = 0; k < sizeof(k_live_only) / sizeof(k_live_only[0]); k++) {
            if (c->packets[i].type != k_live_only[k]) continue;
            if (!n++) snprintf(first, size, "type %d", (int) c->packets[i].type);
        }
    }
    return n;
}

static int index_of_projectile(const TestClient *c, const int from, const int id, NetProjectilePacket *out) {
    for (int i = from; i < c->count; i++) {
        if (c->packets[i].type != NET_PACKET_PROJECTILE || c->packets[i].length != sizeof(NetProjectilePacket)) continue;
        memcpy(out, c->packets[i].payload, sizeof(*out));
        if (out->id == id) return i;
    }
    return -1;
}

static int got_projectile_end(const TestClient *c, const int from, const int id) {
    for (int i = from; i < c->count; i++) {
        if (c->packets[i].type != NET_PACKET_PROJECTILE_END || c->packets[i].length != sizeof(NetProjectileEndPacket)) continue;
        NetProjectileEndPacket end;
        memcpy(&end, c->packets[i].payload, sizeof(end));
        if (end.id == id) return 1;
    }
    return 0;
}

static int got_chat_v7(const TestClient *c, const int from, const char *needle) {
    for (int i = from; i < c->count; i++) {
        if (c->packets[i].type != NET_PACKET_CHAT_V7 || c->packets[i].length != sizeof(NetChatV7Packet)) continue;
        NetChatV7Packet chat;
        memcpy(&chat, c->packets[i].payload, sizeof(chat));
        chat.chat.text[sizeof(chat.chat.text) - 1] = 0;
        if (strstr(chat.chat.text, needle)) return 1;
    }
    return 0;
}

// A server projectile with no owner (above the map, flying up), returns its slot
static int launch_projectile(const vec3 at, const float speed, const float range) {
    const ProjectileConfig config = {.speed = speed, .range = range};
    unsigned char before[GAME_MAX_PROJECTILES];
    for (int i = 0; i < GAME_MAX_PROJECTILES; i++) before[i] = g_server->game.projectiles[i].active;
    game_spawn_projectile(&g_server->game, NULL, 0, &config, at, 0.0f, 1.4f, 0);
    for (int i = 0; i < GAME_MAX_PROJECTILES; i++) {
        if (g_server->game.projectiles[i].active && !before[i]) return i;
    }
    return -1;
}

static void pump_three(TestClient *a, TestClient *b, TestClient *c, const int frames) {
    for (int f = 0; f < frames; f++) {
        pump(a, 1);
        pump(b, 0);
        pump(c, 0);
    }
}

// The last SPAWN the client got for its own (is_you) or another player's life: the newest is the state now.
static NetSpawnPacket last_spawn_of(const TestClient *c, const int is_you) {
    NetSpawnPacket out = {0};
    for (int i = c->count - 1; i >= 0; i--) {
        NetSpawnPacket spawn;
        if (c->packets[i].type == NET_PACKET_SPAWN && net_packet_read_spawn(&spawn, &c->packets[i]) && spawn.is_you == is_you && spawn.active) {
            return spawn;
        }
    }
    return out;
}

// class (KrunkNative dev, offline sandbox; chat_commands.c cmd_class): the class and its secondary take effect in
// place (player_switch_class): the new class's health, a SPAWN to the client, the player stays where it stood. A
// refused class or weapon replies why and leaves the player as it was. The chat limit is 3 lines a second, so each
// line gets its own second.
static void test_class(void) {
    size_t map_count = 0;
    const cJSON **maps = load_maps(&map_count);
    const SandboxConfig cfg = {.maps = maps, .map_count = map_count, .map_index = 0, .mode_id = -1};
    CHECK(sandbox_start(&cfg), "sandbox_start (class)");

    static TestClient c;
    memset(&c, 0, sizeof(c));
    c.fd = sandbox_connect();
    join(&c);
    pump(&c, 60);
    NetSpawnPacket me = {0};
    CHECK(spawn_of(&c, 1, &me), "spawned (class)");
    Player *p = (Player *) server_player(me.uid);
    CHECK(p != NULL, "server player (class)");
    if (!p) {
        sandbox_stop();
        return;
    }
    CHECK(p->class_index == 0, "joined as class 0, got %d", p->class_index);

    const vec3 before = p->position;
    chat(&c, "/class \"Spray N Pray\"");
    pump(&c, 61);
    const int spray = 3;
    const float spray_health = (float) rules_classes()[spray].health;
    CHECK(chat_contains(&c, "Switched to Spray N Pray (3)."), "/class reply");
    CHECK(p->class_index == spray, "server class %d after the switch", p->class_index);
    CHECK(p->max_health == (int) spray_health && p->health == spray_health, "health %.0f / max %d after the switch", (double) p->health, p->max_health);
    CHECK(p->position.x == before.x && p->position.y == before.y && p->position.z == before.z, "the switch keeps the position");
    const NetSpawnPacket after = last_spawn_of(&c, 1);
    CHECK(after.class_index == spray && (float) after.health == spray_health, "SPAWN to the client: class %d health %.0f", after.class_index, (double) after.health);

    // refused: an unknown class, an unknown weapon, a primary as the secondary: nothing changes
    const int secondary_before = p->secondary_index;
    const float health_before = p->health;
    chat(&c, "/class 99");
    pump(&c, 61);
    chat(&c, "/class Nope");
    pump(&c, 61);
    chat(&c, "/class Runner \"Assault Rifle\"");
    pump(&c, 61);
    CHECK(chat_contains(&c, "No class called '99'."), "refused: unknown class id");
    CHECK(chat_contains(&c, "No class called 'Nope'."), "refused: unknown class name");
    CHECK(chat_contains(&c, "Assault Rifle is not a secondary weapon."), "refused: a primary as the secondary");
    CHECK(p->class_index == spray && p->secondary_index == secondary_before && p->health == health_before, "refused switches change nothing");

    // a secondary: Triggerman (0) with the Pistol (id 2) in its secondary slot
    chat(&c, "/class 0 Pistol");
    pump(&c, 61);
    CHECK(chat_contains(&c, "Switched to Triggerman (0)."), "/class 0 Pistol reply");
    CHECK(p->class_index == 0 && p->secondary_index == 2, "class 0 with the Pistol: class %d secondary %d", p->class_index, p->secondary_index);
    CHECK(p->max_health == rules_classes()[0].health, "Triggerman's health %d", p->max_health);

    // KrunkNative's Nettspend: playable here (a KrunkNative lobby), with its own Drank and Money Spread
    chat(&c, "/class Nettspend");
    pump(&c, 61);
    char switched[48];
    snprintf(switched, sizeof(switched), "Switched to Nettspend (%d).", CLASS_ID_NETTSPEND);
    CHECK(chat_contains(&c, switched), "/class Nettspend reply");
    CHECK(p->class_index == CLASS_ID_NETTSPEND && p->loadout_size >= 2 && p->loadout[0] == WEAPON_ID_DRANK && p->loadout[1] == WEAPON_ID_MONEY_SPREAD,
          "the Nettspend's loadout: class %d (%d %d)", p->class_index, p->loadout_size > 0 ? p->loadout[0] : -1, p->loadout_size > 1 ? p->loadout[1] : -1);
    // the same switch in an official krunker.io game (game_class_allowed, the one class gate) is refused
    chat(&c, "/class 0");
    pump(&c, 61);
    CHECK(p->class_index == 0, "back to the Triggerman (%d)", p->class_index);
    p->game->is_official = 1;
    chat(&c, "/class Nettspend");
    pump(&c, 61);
    p->game->is_official = 0;
    CHECK(chat_contains(&c, "Nettspend is not playable on official krunker.io servers."), "official: the Nettspend refused");
    CHECK(p->class_index == 0, "official: the refusal changes nothing (class %d)", p->class_index);
    sandbox_stop();
}

// The human `uid` of client `c` alive, for a scripted /kill: a bot of the match may have killed it
// first, so it sends ENT again (every 0.5 s: the server takes it once config.deathDelay has
// passed) until the server has it back, up to 6 s. Returns the server's player.
static const Player *alive_for_kill(TestClient *c, const int uid, const NetEntPacket *ent, TestClient *a, TestClient *b, TestClient *spec) {
    const Player *p = server_player(uid);
    for (int f = 0; p && !p->active && f < 360; f++) {
        if (f % 30 == 0) client_send(c, NET_PACKET_ENT, ent, (uint16_t) NET_ENT_PACKET_V7_SIZE);
        pump_three(a, b, spec, 1);
        p = server_player(uid);
    }
    return p;
}

// "bullets keep getting sent to the client after the player is dead": in a bot match a dead
// player gets no live-world packet (k_live_only) until its respawn, but still the kill feed,
// chat and projectile ends; a spectator gets everything. On the respawn the projectiles still
// in flight that it missed are resent, and the player sync resumes for every active player.
static void test_dead_player_live_filter(void) {
    size_t map_count = 0;
    const cJSON **maps = load_maps(&map_count);
    const SandboxConfig cfg = {.maps = maps, .map_count = map_count, .map_index = 0, .mode_id = MODE_ID_FFA};
    CHECK(sandbox_start(&cfg), "sandbox_start (dead filter)");
    BotAssignments preset;
    bot_difficulty_assignments(BOT_DIFFICULTY_NORMAL, &preset);
    game_bots_configure(&g_server->game, &preset);
    game_bots_set_quota(&g_server->game, 6, 0);

    static TestClient a, b, spec;
    const NetHelloPacket hello = {NET_PROTOCOL_MAJOR, NET_PROTOCOL_MINOR_V7, 0};
    const NetEntPacket ent = {.class_index = 0, .secondary_index = -1, .cosmetics = PLAYER_COSMETICS_NONE, .slid_cont = 1, .lag_comp = 1.0f};
    NetEntPacket ent_spec = ent;
    ent_spec.spectate = 1;
    mix_join(&a, &hello, sizeof(hello), &ent, (uint16_t) NET_ENT_PACKET_V7_SIZE);
    pump(&a, 30);
    mix_join(&b, &hello, sizeof(hello), &ent, (uint16_t) NET_ENT_PACKET_V7_SIZE);
    mix_join(&spec, &hello, sizeof(hello), &ent_spec, (uint16_t) NET_ENT_PACKET_V7_SIZE);
    pump_three(&a, &b, &spec, 60);
    NetSpawnPacket me_a = {0}, me_b = {0};
    CHECK(spawn_of(&a, 1, &me_a) && spawn_of(&b, 1, &me_b), "A and B spawned");

    // alive: 10 s of the bot match
    const unsigned long long alive_bytes0 = b.bytes;
    const int alive_shots0 = b.type_count[NET_PACKET_SHOT];
    pump_three(&a, &b, &spec, 600);
    const unsigned long long alive_bytes = b.bytes - alive_bytes0;
    const int alive_shots = b.type_count[NET_PACKET_SHOT] - alive_shots0;
    CHECK(alive_shots > 0, "alive: B sees the bots' tracers (%d SHOT)", alive_shots);

    // dead: B kills itself and stays on the death screen 10 s; meanwhile a projectile that
    // outlives the death screen, one that ends during it, A's chat and A's death (kill feed).
    // The six bots are all in from the start (the quota fills at once), so one may have got B
    // in the 10 s: back in first, to die on cue.
    const Player *pb = alive_for_kill(&b, me_b.uid, &ent, &a, &b, &spec);
    CHECK(pb && pb->active, "B alive before its /kill");
    b.count = 0;
    spec.count = 0;
    chat(&b, "/kill");
    pump_three(&a, &b, &spec, 2);
    NetKillPacket kill = {0};
    const int kill_b = index_of_kill(&b, 0, me_b.uid, &kill);
    CHECK(kill_b >= 0, "B got its own KILL");
    const unsigned long long dead_bytes0 = b.bytes;
    unsigned long long dead_type0[256];
    memcpy(dead_type0, b.type_bytes, sizeof(dead_type0));
    const int spec_shots0 = spec.type_count[NET_PACKET_SHOT];
    const vec3 sky = {me_b.position[0], me_b.position[1] + 400.0f, me_b.position[2]};
    const int long_slot = launch_projectile(sky, 0.05f, 5000.0f);  // ~100 s of flight
    const int short_slot = launch_projectile(sky, 0.5f, 50.0f);    // ~0.1 s
    CHECK(long_slot >= 0 && short_slot >= 0 && long_slot != short_slot, "projectiles launched (%d, %d)", long_slot, short_slot);
    pump_three(&a, &b, &spec, 60);
    chat(&a, "dead players read chat");
    pump_three(&a, &b, &spec, 60);
    const Player *pa = alive_for_kill(&a, me_a.uid, &ent, &a, &b, &spec); // a bot got A first: back in, to die on cue
    CHECK(pa && pa->active, "A alive before its /kill");
    chat(&a, "/kill");
    pump_three(&a, &b, &spec, 480);
    const unsigned long long dead_bytes = b.bytes - dead_bytes0;
    CHECK(b.count < MAX_RECEIVED, "dead: B's packets all kept (%d)", b.count);
    char first[32] = "";
    const int leaked = kill_b >= 0 ? live_only_after(&b, kill_b + 1, first, sizeof(first)) : -1;
    CHECK(leaked == 0, "dead: B gets no live-world packet (%d, first %s)", leaked, first);
    int feed = 0;
    for (int i = kill_b + 1; kill_b >= 0 && i < b.count; i++) feed += b.packets[i].type == NET_PACKET_KILL;
    CHECK(feed > 0, "dead: B still gets the kill feed (%d KILL)", feed);
    CHECK(got_chat_v7(&b, kill_b + 1, "dead players read chat"), "dead: B still gets chat");
    CHECK(got_projectile_end(&b, kill_b + 1, short_slot), "dead: B still gets projectile ends (v7 \"pre\" to all)");
    CHECK(count_type(&b, NET_PACKET_HEALTH) > 0, "dead: B still gets health (v7 \"h\" to all)");
    CHECK(spec.type_count[NET_PACKET_SHOT] - spec_shots0 > 0, "spectator: still gets tracers (%d SHOT)", spec.type_count[NET_PACKET_SHOT] - spec_shots0);
    NetProjectilePacket seen;
    CHECK(index_of_projectile(&spec, 0, long_slot, &seen) >= 0, "spectator: gets the projectile launch");
    printf("dead filter: B alive %.0f B/s (%d SHOT/10 s), dead %.0f B/s:", alive_bytes / 10.0, alive_shots, dead_bytes / 10.0);
    for (int t = 0; t < 256; t++) {
        if (b.type_bytes[t] > dead_type0[t]) printf(" %d=%llu", t, b.type_bytes[t] - dead_type0[t]);
    }
    printf("\n");

    // respawn: the in-flight projectile is resent from its current state, the ended one is not,
    // and every active player's position follows
    b.count = 0;
    client_send(&b, NET_PACKET_ENT, &ent, (uint16_t) NET_ENT_PACKET_V7_SIZE);
    client_send(&a, NET_PACKET_ENT, &ent, (uint16_t) NET_ENT_PACKET_V7_SIZE);
    const int respawn_shots0 = b.type_count[NET_PACKET_SHOT];
    const int respawn_spec_shots0 = spec.type_count[NET_PACKET_SHOT];
    pump_three(&a, &b, &spec, 30);
    const int spawn_b = index_of_spawn(&b, 0, me_b.uid);
    CHECK(spawn_b >= 0, "B respawned");
    const GameProjectile *flying = long_slot >= 0 ? &g_server->game.projectiles[long_slot] : NULL;
    const int resent = spawn_b >= 0 ? index_of_projectile(&b, spawn_b, long_slot, &seen) : -1;
    CHECK(flying && flying->active && resent > spawn_b, "respawn: the projectile in flight is resent (%d)", resent);
    if (resent > spawn_b && flying) {
        CHECK(fabsf(seen.position[1] - sky.y) > 100.0f && fabsf(seen.range - 5000.0f) > 100.0f && seen.owner_uid == -1,
              "respawn: from its current state (y %.1f, range %.1f)", seen.position[1], seen.range);
    }
    CHECK(index_of_projectile(&b, 0, short_slot, &seen) < 0 || g_server->game.projectiles[short_slot].active,
          "respawn: an ended projectile is not resent");
    pump_three(&a, &b, &spec, 300);
    for (size_t i = 0; spawn_b >= 0 && i < g_server->game.player_count; i++) {
        const Player *p = g_server->game.players[i];
        if (!p || !p->active || p->uid == me_b.uid) continue;
        CHECK(states_about(&b, spawn_b, p->uid) > 0, "respawn: B gets player %d's position again", p->uid);
    }
    // the bots may not fire for a few seconds (their fights are random): wait, up to 20 s more,
    // until the spectator sees a tracer, which B (alive again) must see too
    for (int f = 0; f < 1200 && spec.type_count[NET_PACKET_SHOT] == respawn_spec_shots0; f += 30) pump_three(&a, &b, &spec, 30);
    pump_three(&a, &b, &spec, 5);
    const int spec_shots = spec.type_count[NET_PACKET_SHOT] - respawn_spec_shots0;
    CHECK(spec_shots > 0, "respawn: the bots fire again (spectator %d SHOT)", spec_shots);
    CHECK(b.type_count[NET_PACKET_SHOT] - respawn_shots0 > 0, "respawn: B sees tracers again (%d, spectator %d)", b.type_count[NET_PACKET_SHOT] - respawn_shots0, spec_shots);
    sandbox_stop();
}

// The last round clock ("t", not the end screen's) received from index `from` on, -1 none.
static int last_round_seconds(const TestClient *c, const int from) {
    int seconds = -1;
    for (int i = from; i < c->count; i++) {
        if (c->packets[i].type != NET_PACKET_TIMER || c->packets[i].length != sizeof(NetTimerPacket)) continue;
        NetTimerPacket t;
        memcpy(&t, c->packets[i].payload, sizeof(t));
        if (!t.end) seconds = t.seconds;
    }
    return seconds;
}

// R64: /endless pauses the round clock and a second /endless resumes it from there (no reset to a
// full round, no clock running on behind the frozen display).
static void test_endless_resumes(void) {
    size_t map_count = 0;
    const cJSON **maps = load_maps(&map_count);
    const SandboxConfig cfg = {.maps = maps, .map_count = map_count, .map_index = 0, .mode_id = MODE_ID_FFA};
    CHECK(sandbox_start(&cfg), "sandbox_start (endless)");
    static TestClient c;
    memset(&c, 0, sizeof(c));
    c.fd = sandbox_connect();
    join(&c);
    pump(&c, 60 * 5); // 5 s of the round
    const int before = last_round_seconds(&c, 0);
    CHECK(before > 0, "a running round clock (%d)", before);

    int mark = c.count;
    command(&c, &c, &c, "/endless");
    CHECK(chat_contains(&c, "Match timer set to endless."), "endless on");
    pump(&c, 60 * 6); // 6 s endless
    const int frozen = last_round_seconds(&c, mark);
    CHECK(frozen < 0 || frozen == before, "endless: the clock stays at %d (got %d)", before, frozen);
    CHECK(g_server->game.round_endless && fabsf(g_server->game.round_timer - (float) before) < 1.5f,
          "endless: the server clock is paused at %d (round_timer %.2f)", before, (double) g_server->game.round_timer);

    mark = c.count;
    command(&c, &c, &c, "/endless");
    CHECK(chat_contains(&c, "Match timer resumed."), "endless off");
    pump(&c, 60 * 2); // 2 s on
    const int resumed = last_round_seconds(&c, mark);
    CHECK(resumed >= before - 3 && resumed <= before, "endless off: resumes from %d (got %d)", before, resumed);
    sandbox_disconnect(c.fd);
    sandbox_step(1.0 / 60.0);
    sandbox_stop();
}

// R45: a noclip player flying level off the map's edge stays in the air on the server too (no
// gravity, no ground under it, the client's prediction and the server agree), and like v10.0.2
// players.tickPlayer (y <= deathY && !godMode: noclip is not exempt) diving below deathY kills it.
static void test_noclip_off_edge(void) {
    size_t map_count = 0;
    const cJSON **maps = load_maps(&map_count);
    const SandboxConfig cfg = {.maps = maps, .map_count = map_count, .map_index = 0, .mode_id = MODE_ID_FFA};
    CHECK(sandbox_start(&cfg), "sandbox_start (noclip)");
    static TestClient c;
    memset(&c, 0, sizeof(c));
    c.fd = sandbox_connect();
    join(&c);
    pump(&c, 30);
    NetSpawnPacket me = {0};
    CHECK(spawn_of(&c, 1, &me) != NULL, "spawned (noclip)");
    Player *p = (Player *) server_player(me.uid);
    CHECK(p && g_server->game.map, "server player (noclip)");
    if (!p || !g_server->game.map) {
        sandbox_stop();
        return;
    }
    command(&c, &c, &c, "/noclip");
    pump(&c, 2);
    CHECK(p->noclip, "/noclip on");

    // 12 s level flight (pitch 0) straight on: far past Burg's edge, over nothing
    const float y0 = p->position.y;
    int seq = 0;
    for (; seq < 60 * 12; seq++) {
        const NetInputPacket input = {.seq = seq, .move_dir = 0, .delta = 1.0f / 60.0f, .x_dir = 0.0f, .y_dir = me.direction[1]};
        client_send(&c, NET_PACKET_INPUT, &input, sizeof(input));
        pump(&c, 1);
    }
    pump(&c, 10);
    const float flown = hypotf(p->position.x - me.position[0], p->position.z - me.position[2]);
    CHECK(p->active && fabsf(p->position.y - y0) < 0.01f && flown > 500.0f,
          "noclip off the edge: alive at y %.2f (from %.2f), %.0f units out", (double) p->position.y, (double) y0, (double) flown);
    const NetPacket *recon_packet = last(&c, NET_PACKET_RECON);
    NetReconPacket recon = {0};
    if (recon_packet) memcpy(&recon, recon_packet->payload, recon_packet->length < sizeof(recon) ? recon_packet->length : sizeof(recon));
    CHECK(recon_packet && fabsf(recon.position[1] - y0) < 0.01f, "noclip: the owner's RECON keeps the height (y %.2f)", (double) recon.position[1]);

    // dive: below deathY the server kills it (v10 exempts godMode only)
    const float death_y = g_server->game.map->death_y;
    for (int f = 0; f < 60 * 30 && p->active; f++, seq++) {
        const NetInputPacket input = {.seq = seq, .move_dir = 0, .delta = 1.0f / 60.0f, .x_dir = -1.2f, .y_dir = me.direction[1]};
        client_send(&c, NET_PACKET_INPUT, &input, sizeof(input));
        pump(&c, 1);
    }
    CHECK(!p->active && p->position.y <= death_y + 0.01f, "noclip below deathY %.0f: killed (active %d, y %.2f)", (double) death_y,
          p->active, (double) p->position.y);
    sandbox_disconnect(c.fd);
    sandbox_step(1.0 / 60.0);
    sandbox_stop();
}

int main(void) {
    test_join_move_leave();
    test_wpc_peer_mix();
    test_dead_player_sync();
    test_class();
    test_dead_player_live_filter();
    test_idle_kick_map_change();
    test_tp_bring();
    test_restart_and_disconnect();
    test_endless_resumes();
    test_noclip_off_edge();
    if (g_failures) {
        printf("%d of %d checks failed\n", g_failures, g_checks);
        return 1;
    }
    printf("Test OK (%d checks)\n", g_checks);
    return 0;
}
