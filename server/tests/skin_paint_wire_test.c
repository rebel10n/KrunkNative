// Per-copy paint on the SKINS packet (Krunker v10.0.3 paint, doc/v10/paint-copy.md; NET_PROTOCOL_PATCH 6): a player's
// painted copies reach a joiner with their paint. A painted owner is joined over the sandbox server (loopback), its
// account's owned list (data slot 10, with painted copies {ind, cnt: 1, id, m}) is loaded, then a joiner that
// announced patch 6 must get the 40-byte SKINS V2 carrying the owner's paint for each worn skin, and a joiner on
// patch 5 the 16-byte V1 (its paint-less form). Run from the repository root: ./<build>/skin_paint_wire_test
#include <server_sandbox.h>
#include <server.h>
#include <server_accounts.h>
#include <shared.h>
#include <map_catalog.h>
#include <modes.h>
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
} while (0)

#define MAX_RECEIVED 2048

typedef struct {
    int fd;
    int conn_id; // the server's ClientConnection.id of this link
    NetPacketBuffer buffer;
    NetPacket packets[MAX_RECEIVED];
    int count;
} TestClient;

// the owner (painted), a patch 6 joiner, a patch 5 joiner
enum { OWNER, JOINER, OLD_JOINER, CLIENTS };
static TestClient g_clients[CLIENTS];

static void send_to(TestClient *c, const int type, const void *payload, const uint16_t length) {
    CHECK(net_send_packet(c->fd, (NetPacketType) type, payload, length), "send type %d", type);
}

static void drain(TestClient *c) {
    uint8_t bytes[2048];
    int n;
    while ((n = sandbox_receive(c->fd, bytes, sizeof(bytes))) > 0) {
        CHECK(net_buffer_push(&c->buffer, bytes, (size_t) n), "buffer overflow");
        NetPacket packet;
        while (net_buffer_next(&c->buffer, &packet) > 0) {
            if (packet.type == NET_PACKET_PING && packet.length == sizeof(NetPingPacket)) send_to(c, NET_PACKET_PONG, packet.payload, packet.length);
            if (c->count < MAX_RECEIVED) c->packets[c->count++] = packet;
        }
    }
}

static void pump(const int frames) {
    for (int f = 0; f < frames; f++) {
        sandbox_step(1.0 / 60.0);
        for (int i = 0; i < CLIENTS; i++) {
            if (g_clients[i].fd >= NET_LOOPBACK_FD_BASE) drain(&g_clients[i]);
        }
    }
}

static ClientConnection *conn(const TestClient *c) {
    for (size_t i = 0; i < g_server->client_count; i++) {
        if (g_server->clients[i].id == c->conn_id) return &g_server->clients[i];
    }
    return NULL;
}

// A v10 client's join: HELLO (its patch), JOIN, ENT wearing `skins`.
static void join(TestClient *c, const int minor, const int patch, const PlayerSkins skins) {
    c->fd = sandbox_connect();
    CHECK(c->fd >= NET_LOOPBACK_FD_BASE && g_server->client_count, "connect");
    c->conn_id = g_server->client_count ? g_server->clients[g_server->client_count - 1].id : -1;
    const NetHelloPacket hello = {NET_PROTOCOL_MAJOR, (uint8_t) minor, (uint8_t) patch};
    send_to(c, NET_PACKET_HELLO, &hello, sizeof(hello));
    NetJoinPacket join_packet;
    memset(&join_packet, 0, sizeof(join_packet));
    send_to(c, NET_PACKET_JOIN, &join_packet, (uint16_t) NET_JOIN_PACKET_V2_SIZE);
    NetEntPacket ent = {.class_index = 0, .secondary_index = -1, .cosmetics = PLAYER_COSMETICS_NONE, .slid_cont = 1, .lag_comp = 1.0f, .skins = skins};
    send_to(c, NET_PACKET_ENT, &ent, (uint16_t) NET_ENT_PACKET_V7_SIZE);
    pump(2);
}

// The SKINS packets a client received for the player `uid`, the last one (the joiner's own spawn sequence).
static const NetPacket *skins_of(const TestClient *c, const int uid) {
    const NetPacket *found = NULL;
    for (int i = 0; i < c->count; i++) {
        if (c->packets[i].type != NET_PACKET_SKINS || c->packets[i].length < 4) continue;
        int32_t got = 0;
        memcpy(&got, c->packets[i].payload, sizeof(got));
        if (got == uid) found = &c->packets[i];
    }
    return found;
}

// The owner's owned list (account data slot 10): a colour paint on its primary skin, a shader paint on its melee, and an
// unpainted stack of the secondary (no paint). m is an object for one copy and a JSON string for the other (the normaliser).
static void load_owner_account(void) {
    cJSON *account = cJSON_Parse(
        "{\"skins\":["
        "{\"ind\":7776,\"cnt\":1,\"id\":1000001,\"m\":{\"paint\":\"#dc3c3c\"}},"
        "{\"ind\":4208,\"cnt\":1,\"id\":1000002,\"m\":\"{\\\"paintShader\\\":3}\"},"
        "{\"ind\":4211,\"cnt\":2,\"id\":0}"
        "]}");
    CHECK(account != NULL, "the owned list parses");
    if (!account) return;
    accounts_load_skins(conn(&g_clients[OWNER]), account);
    cJSON_Delete(account);
}

static void test_painted_skins_reach_joiners(void) {
    memset(g_clients, 0, sizeof(g_clients));
    const PlayerSkins worn = {7776, 4211, 4208}; // primary (colour paint), secondary (no paint), melee (shader paint)
    join(&g_clients[OWNER], NET_PROTOCOL_MINOR, NET_PROTOCOL_PATCH, worn);
    Player *owner = conn(&g_clients[OWNER]) ? conn(&g_clients[OWNER])->player : NULL;
    CHECK(owner != NULL, "owner spawned");
    if (!owner) return;
    load_owner_account();
    CHECK(conn(&g_clients[OWNER])->acct.skin_paint_count == 2, "two painted copies on the owner's account (got %u)", conn(&g_clients[OWNER])->acct.skin_paint_count);

    // a joiner on this build (patch 6) and an older one (patch 5), after the owner's account is known
    join(&g_clients[JOINER], NET_PROTOCOL_MINOR, NET_PROTOCOL_PATCH, PLAYER_SKINS_NONE);
    join(&g_clients[OLD_JOINER], NET_PROTOCOL_MINOR, NET_PROTOCOL_PATCH - 1, PLAYER_SKINS_NONE);
    pump(60);

    const NetPacket *v2 = skins_of(&g_clients[JOINER], (int) owner->uid);
    CHECK(v2 != NULL, "the patch 6 joiner got the owner's SKINS");
    if (v2) {
        CHECK(v2->length == NET_SKINS_PACKET_V2_SIZE && NET_SKINS_PACKET_V2_SIZE == 40, "SKINS V2 is 40 bytes (got %u)", v2->length);
        NetSkinsPacketV2 skins;
        CHECK(net_packet_read_skins(&skins, v2->payload, v2->length), "SKINS V2 reads");
        CHECK(skins.skins.primary == 7776 && skins.skins.secondary == 4211 && skins.skins.melee == 4208, "the owner's skins arrive as worn");
        CHECK(skins.paints.primary.color == 0xdc3c3c && skins.paints.primary.shader == -1, "the colour paint reaches the joiner (color %d shader %d)",
              skins.paints.primary.color, skins.paints.primary.shader);
        CHECK(skins.paints.secondary.color == -1 && skins.paints.secondary.shader == -1, "an unpainted skin carries no paint");
        CHECK(skins.paints.melee.color == -1 && skins.paints.melee.shader == 3, "the shader paint reaches the joiner (shader %d)", skins.paints.melee.shader);
    }

    const NetPacket *v1 = skins_of(&g_clients[OLD_JOINER], (int) owner->uid);
    CHECK(v1 != NULL, "the patch 5 joiner got the owner's SKINS");
    if (v1) {
        CHECK(v1->length == NET_SKINS_PACKET_V1_SIZE && NET_SKINS_PACKET_V1_SIZE == 16, "a patch 5 peer keeps the 16-byte SKINS (got %u)", v1->length);
        NetSkinsPacket old;
        memcpy(&old, v1->payload, sizeof(old));
        CHECK(old.uid == (int32_t) owner->uid && old.skins.primary == 7776 && old.skins.melee == 4208, "the V1 carries the same skins");
    }
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const cJSON *burg = map_catalog_json((size_t) map_catalog_index_of_key("311_burg"));
    CHECK(burg != NULL, "map");
    static const cJSON *maps[1];
    maps[0] = burg;
    const SandboxConfig cfg = {.maps = maps, .map_count = 1, .map_index = 0, .mode_id = MODE_ID_FFA};
    if (sandbox_start(&cfg)) {
        test_painted_skins_reach_joiners();
        for (int i = 0; i < CLIENTS; i++) {
            if (g_clients[i].fd >= NET_LOOPBACK_FD_BASE) sandbox_disconnect(g_clients[i].fd);
        }
        sandbox_stop();
    } else {
        CHECK(0, "sandbox start");
    }
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
