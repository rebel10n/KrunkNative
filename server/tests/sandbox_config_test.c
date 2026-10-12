// WP-E offline: the sandbox (server/include/server_sandbox.h) plays a map's embedded "config"
// (decisions.md "Embedded map config": v7's sandbox discards its settings, KN applies them) and
// its players count as having an account (decisions.md "Offline account"). A protocol-1.3 raw
// client gets the config as GAME_CONFIG + NETWORK_DATA after its HELLO, spawns as the map's only
// class, and a protocol-1.0 client gets none of the lobby packets.
// Run from the repository root: ./<build>/sandbox_config_test
#include <server_sandbox.h>
#include <server.h>
#include <shared.h>
#include <game_config.h>
#include <account.h>
#include <knet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;

#define CHECK(cond, ...) do { \
    if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_failures++; } \
} while (0)

#define MAX_RECEIVED 4096

typedef struct {
    int fd;
    NetPacketBuffer buffer;
    NetPacket packets[MAX_RECEIVED];
    int count;
} TestClient;

static void send_to(TestClient *c, const NetPacketType type, const void *payload, const uint16_t length) {
    CHECK(net_send_packet(c->fd, type, payload, length), "send type %d", (int) type);
}

static void pump(TestClient *clients, const int n, const int frames) {
    for (int f = 0; f < frames; f++) {
        sandbox_step(1.0 / 60.0);
        for (int i = 0; i < n; i++) {
            TestClient *c = &clients[i];
            uint8_t bytes[2048];
            int got;
            while ((got = sandbox_receive(c->fd, bytes, sizeof(bytes))) > 0) {
                net_buffer_push(&c->buffer, bytes, (size_t) got);
                NetPacket packet;
                while (net_buffer_next(&c->buffer, &packet) > 0) {
                    if (packet.type == NET_PACKET_PING && packet.length == sizeof(NetPingPacket)) send_to(c, NET_PACKET_PONG, packet.payload, packet.length);
                    if (c->count < MAX_RECEIVED) c->packets[c->count++] = packet;
                }
            }
        }
    }
}

static void join(TestClient *c, const uint8_t minor, const char *name) {
    const NetHelloPacket hello = {NET_PROTOCOL_MAJOR, minor, 0};
    send_to(c, NET_PACKET_HELLO, &hello, sizeof(hello));
    NetJoinPacket join_packet;
    memset(&join_packet, 0, sizeof(join_packet));
    snprintf(join_packet.name, sizeof(join_packet.name), "%s", name);
    send_to(c, NET_PACKET_JOIN, &join_packet, (uint16_t) NET_JOIN_PACKET_V2_SIZE);
    NetEntPacket ent = {.class_index = 0, .secondary_index = -1, .cosmetics = PLAYER_COSMETICS_NONE, .slid_cont = 1, .lag_comp = 1.0f};
    send_to(c, NET_PACKET_ENT, &ent, (uint16_t) NET_ENT_PACKET_V7_SIZE);
}

// The last whole GAME_CONFIG, parsed (the caller frees).
static cJSON *game_config_of(const TestClient *c) {
    char *text = NULL;
    uint32_t have = 0, total = 0;
    cJSON *done = NULL;
    for (int i = 0; i < c->count; i++) {
        const NetPacket *p = &c->packets[i];
        if (p->type != NET_PACKET_GAME_CONFIG || p->length < NET_GAME_CONFIG_HEADER_SIZE) continue;
        NetGameConfigChunk chunk;
        memcpy(&chunk, p->payload, p->length);
        if (chunk.offset == 0) {
            free(text);
            text = calloc(1, chunk.total + 1);
            total = chunk.total;
            have = 0;
        }
        if (!text || chunk.offset != have) continue;
        memcpy(text + have, chunk.data, p->length - NET_GAME_CONFIG_HEADER_SIZE);
        have += p->length - NET_GAME_CONFIG_HEADER_SIZE;
        if (have == total) {
            cJSON_Delete(done);
            done = cJSON_Parse(text);
        }
    }
    free(text);
    return done;
}

static int my_spawn_class(const TestClient *c) {
    for (int i = c->count - 1; i >= 0; i--) {
        if (c->packets[i].type != NET_PACKET_SPAWN || c->packets[i].length < NET_SPAWN_PACKET_V1_SIZE) continue;
        NetSpawnPacket spawn;
        memcpy(&spawn, c->packets[i].payload, NET_SPAWN_PACKET_V1_SIZE);
        if (spawn.is_you) return spawn.class_index;
    }
    return -1;
}

int main(void) {
    cJSON *map = NULL;
    if (load_json_file("assets/maps/custom/eterno_jump.json", &map) && load_json_file("../assets/maps/custom/eterno_jump.json", &map)) {
        printf("Failed to load assets/maps/custom/eterno_jump.json (run from the repository root)\n");
        return 1;
    }
    const cJSON *maps[1] = {map};
    const SandboxConfig cfg = {.maps = maps, .map_count = 1, .map_index = 0, .mode_id = -1};
    CHECK(sandbox_start(&cfg), "sandbox starts");
    CHECK(g_server->game.ready && g_server->game.config.game_time == 0 && g_server->game.config.max_players == 20 &&
          g_server->game.config.class_list_count == 1 && !g_server->game.classes[9].wall_jump, "the map's config on the sandbox game");

    static TestClient clients[2];
    clients[0].fd = sandbox_connect();
    clients[1].fd = sandbox_connect();
    CHECK(clients[0].fd >= 0 && clients[1].fd >= 0, "connect");
    join(&clients[0], NET_PROTOCOL_MINOR_V7, "V7");
    join(&clients[1], 0, "Old");
    pump(clients, 2, 30);

    cJSON *gc = game_config_of(&clients[0]);
    const cJSON *settings = cJSON_GetObjectItem(gc, "settings");
    CHECK(gc && cJSON_GetObjectItem(settings, "gameTime")->valueint == 0 && cJSON_IsTrue(cJSON_GetObjectItem(settings, "headClipFix")), "GAME_CONFIG: the map's settings");
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(gc, "classes")) == 1 && cJSON_IsTrue(cJSON_GetObjectItem(gc, "custom")), "GAME_CONFIG: classes [9], a custom game");
    cJSON_Delete(gc);
    int net = 0, old_lobby = 0;
    for (int i = 0; i < clients[0].count; i++) net += clients[0].packets[i].type == NET_PACKET_NETWORK_DATA;
    for (int i = 0; i < clients[1].count; i++) old_lobby += clients[1].packets[i].type >= NET_PACKET_BLOCK_LOBBY && clients[1].packets[i].type < NET_PACKET_BLOCK_LOBBY + 10;
    CHECK(net > 0, "NETWORK_DATA");
    CHECK(!old_lobby, "no lobby packets to a 1.0 client (%d)", old_lobby);
    CHECK(my_spawn_class(&clients[0]) == 9 && my_spawn_class(&clients[1]) == 9, "ENT class 0 spawns the only listed class (%d, %d)",
          my_spawn_class(&clients[0]), my_spawn_class(&clients[1]));

    // the offline player counts as having an account
    const Player *me = NULL;
    for (size_t i = 0; i < g_server->game.player_count; i++) if (g_server->game.players[i] && g_server->game.players[i]->active) me = g_server->game.players[i];
    AccountInfo info;
    CHECK(me && game_account_info(&g_server->game, me, &info) && info.has_account && !info.premium && !info.level, "account_info: has_account only");

    // Host Game (createPrivateRoom: ui_host_config_collect -> local_game_host_config): the
    // collected request wins over the map's own config from the next round on
    cJSON *request = cJSON_Parse("{\"modes\":[0],\"classes\":[1],\"private\":false,\"override\":0,\"gameTime\":7,\"maxPlayers\":6}");
    CHECK(request != NULL, "request parses");
    sandbox_host_config(request, GAME_HOST_CUSTOM);
    CHECK(g_server->game.config.game_time == 0, "the running round keeps its config until the next one");
    sandbox_host(0, 0);
    clients[0].count = clients[1].count = 0;
    pump(clients, 2, 30);
    const GameConfig *c = &g_server->game.config;
    CHECK(c->game_time == 7 && c->max_players == 6 && c->class_list_count == 1 && c->class_list[0] == 1,
          "the host request on the next round (gameTime %d, maxPlayers %d, %d class(es), first %d)", c->game_time, c->max_players,
          c->class_list_count, c->class_list_count ? c->class_list[0] : -1);
    gc = game_config_of(&clients[0]);
    settings = cJSON_GetObjectItem(gc, "settings");
    CHECK(gc && cJSON_GetObjectItem(settings, "gameTime") && cJSON_GetObjectItem(settings, "gameTime")->valueint == 7, "GAME_CONFIG: the host's gameTime");
    cJSON_Delete(gc);

    sandbox_disconnect(clients[0].fd);
    sandbox_disconnect(clients[1].fd);
    sandbox_stop();

    // a sandbox started with the request plays it from its first round
    const SandboxConfig hosted = {.maps = maps, .map_count = 1, .map_index = 0, .mode_id = -1, .host_request = request, .host_type = GAME_HOST_CUSTOM};
    CHECK(sandbox_start(&hosted), "hosted sandbox starts");
    CHECK(g_server->game.config.game_time == 7 && g_server->game.config.class_list_count == 1 && g_server->game.config.class_list[0] == 1,
          "the host request on the first round (gameTime %d)", g_server->game.config.game_time);
    sandbox_stop();
    cJSON_Delete(request);
    cJSON_Delete(map);
    if (g_failures) {
        printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("Test OK\n");
    return 0;
}
