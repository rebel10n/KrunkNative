// v10.0.7 "<copyId>#<index>" loadout slots on the server (doc/v10/paint-copy.md section 6; NET_EXT_KN_SKIN_COPY_IDS): an
// owner sends the copy each slot names before its ENT, and a joiner must see that copy's paint (SKINS V2), its seed and wear
// (NET_EXT_KN_SKIN_COPIES V2) and a cosmetic copy's style and wear (NET_EXT_KN_SKIN_STYLES V2); a copy that is gone, a copy
// of another index or no packet at all (an older client) fall back to the plain-index rule. Run from the repository root:
// ./<build>/skin_copy_ids_test
#include <server_sandbox.h>
#include <server.h>
#include <server_accounts.h>
#include <shared.h>
#include <net_ext.h>
#include <map_catalog.h>
#include <modes.h>
#include <knet.h>
#include <cJSON.h>
#include <math.h>
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
    int conn_id;
    NetPacketBuffer buffer;
    NetPacket packets[MAX_RECEIVED];
    int count;
} TestClient;

enum { OWNER, JOINER, CLIENTS };
static TestClient g_clients[CLIENTS];

// the store items (assets/skins.json): Topo (a weapon skin), a melee, a hat and a styled body item
enum { TOPO = 7819, MELEE = 7857, HAT = 111, BODY = 7794 };

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
        for (int i = 0; i < CLIENTS; i++)
            if (g_clients[i].fd >= NET_LOOPBACK_FD_BASE) drain(&g_clients[i]);
    }
}

static ClientConnection *conn(const TestClient *c) {
    for (size_t i = 0; i < g_server->client_count; i++)
        if (g_server->clients[i].id == c->conn_id) return &g_server->clients[i];
    return NULL;
}

static NetSkinCopyIdsPacket no_copies(void) {
    NetSkinCopyIdsPacket p;
    for (int k = 0; k < SKIN_COPY_SLOTS; k++) p.skin[k] = -1, p.copy_id[k] = 0;
    return p;
}

// HELLO, JOIN, then (copies != NULL) NET_EXT_KN_SKIN_COPY_IDS and the ENT, as the client sends them
static void join(TestClient *c, const PlayerSkins skins, const PlayerCosmetics cosmetics, const NetSkinCopyIdsPacket *copies) {
    c->fd = sandbox_connect();
    CHECK(c->fd >= NET_LOOPBACK_FD_BASE && g_server->client_count, "connect");
    c->conn_id = g_server->client_count ? g_server->clients[g_server->client_count - 1].id : -1;
    const NetHelloPacket hello = {NET_PROTOCOL_MAJOR, NET_PROTOCOL_MINOR, NET_PROTOCOL_PATCH};
    send_to(c, NET_PACKET_HELLO, &hello, sizeof(hello));
    NetJoinPacket join_packet;
    memset(&join_packet, 0, sizeof(join_packet));
    send_to(c, NET_PACKET_JOIN, &join_packet, (uint16_t) NET_JOIN_PACKET_V2_SIZE);
    if (copies) {
        uint8_t buf[NET_EXT_HEADER_SIZE + sizeof(*copies)];
        const uint16_t n = net_ext_pack(buf, sizeof(buf), NET_EXT_KN_SKIN_COPY_IDS, copies, sizeof(*copies));
        CHECK(n == NET_EXT_HEADER_SIZE + 128, "the copy ids pack (%u bytes)", n);
        send_to(c, NET_PACKET_EXT, buf, n);
    }
    NetEntPacket ent = {.class_index = 0, .secondary_index = -1, .cosmetics = cosmetics, .slid_cont = 1, .lag_comp = 1.0f, .skins = skins};
    send_to(c, NET_PACKET_ENT, &ent, (uint16_t) NET_ENT_PACKET_V7_SIZE);
    pump(2);
}

static const NetPacket *last_of(const TestClient *c, const int type, const int uid) {
    const NetPacket *found = NULL;
    for (int i = 0; i < c->count; i++) {
        if (c->packets[i].type != type || c->packets[i].length < 4) continue;
        int32_t got = 0;
        memcpy(&got, c->packets[i].payload, sizeof(got));
        if (got == uid) found = &c->packets[i];
    }
    return found;
}

// the last EXT `subtype` body for `uid` (its first int32), its length in *len
static const uint8_t *last_ext(const TestClient *c, const uint16_t subtype, const int uid, uint16_t *len) {
    const uint8_t *found = NULL;
    for (int i = 0; i < c->count; i++) {
        if (c->packets[i].type != NET_PACKET_EXT) continue;
        uint16_t st, bl;
        const uint8_t *body;
        if (!net_ext_unpack(c->packets[i].payload, c->packets[i].length, &st, &body, &bl) || st != subtype || bl < 4) continue;
        int32_t got = 0;
        memcpy(&got, body, sizeof(got));
        if (got == uid) {
            found = body;
            *len = bl;
        }
    }
    return found;
}

// Two Topo copies (ids 501 and 502: other paints, wears and seeds), two melee copies (503 painted and worn, 506 neither), a
// hat copy 504 and two copies of the styled body item (505 style 1, 507 style 2).
static void load_owner_account(void) {
    cJSON *account = cJSON_Parse(
        "{\"skins\":["
        "{\"ind\":7819,\"cnt\":1,\"id\":501,\"m\":{\"paint\":\"#112233\",\"wear\":0.05,\"seed\":11}},"
        "{\"ind\":7819,\"cnt\":1,\"id\":502,\"m\":\"{\\\"paint\\\":\\\"#445566\\\",\\\"wear\\\":0.45,\\\"seed\\\":77}\"},"
        "{\"ind\":7857,\"cnt\":1,\"id\":503,\"m\":{\"paint\":\"#778899\",\"wear\":0.7}},"
        "{\"ind\":7857,\"cnt\":1,\"id\":506,\"m\":{\"serial\":3}},"
        "{\"ind\":111,\"cnt\":1,\"id\":504,\"m\":{\"wear\":0.2}},"
        "{\"ind\":7794,\"cnt\":1,\"id\":505,\"m\":{\"style\":1,\"wear\":0.9}},"
        "{\"ind\":7794,\"cnt\":1,\"id\":507,\"m\":{\"style\":2}}"
        "]}");
    CHECK(account != NULL, "the owned list parses");
    if (!account) return;
    accounts_load_skins(conn(&g_clients[OWNER]), account);
    cJSON_Delete(account);
}

static int near(const float a, const double b) { return fabs((double) a - b) < 1e-5; }

static void test_named_copies_reach_a_joiner(void) {
    memset(g_clients, 0, sizeof(g_clients));
    const PlayerSkins worn = {TOPO, -1, MELEE};
    PlayerCosmetics cos = PLAYER_COSMETICS_NONE;
    cos.hat = HAT;
    cos.body = BODY;
    // the second Topo copy, the unpainted melee copy, the second body copy; the hat stays a plain index
    NetSkinCopyIdsPacket copies = no_copies();
    copies.skin[SKIN_COPY_SLOT_PRIMARY] = TOPO, copies.copy_id[SKIN_COPY_SLOT_PRIMARY] = 502;
    copies.skin[SKIN_COPY_SLOT_MELEE] = MELEE, copies.copy_id[SKIN_COPY_SLOT_MELEE] = 506;
    copies.skin[SKIN_COPY_SLOT_HAT] = HAT;
    copies.skin[SKIN_COPY_SLOT_BODY] = BODY, copies.copy_id[SKIN_COPY_SLOT_BODY] = 507;
    join(&g_clients[OWNER], worn, cos, &copies);
    ClientConnection *owner_conn = conn(&g_clients[OWNER]);
    Player *owner = owner_conn ? owner_conn->player : NULL;
    CHECK(owner != NULL, "owner spawned");
    if (!owner) return;
    CHECK(owner_conn->worn_copies.copy_id[SKIN_COPY_SLOT_PRIMARY] == 502, "the spawn keeps the ENT's copies (got %d)",
          owner_conn->worn_copies.copy_id[SKIN_COPY_SLOT_PRIMARY]);
    load_owner_account();
    CHECK(owner_conn->acct.skin_copy_count == 7, "seven copies with an id (got %u)", owner_conn->acct.skin_copy_count);

    join(&g_clients[JOINER], PLAYER_SKINS_NONE, PLAYER_COSMETICS_NONE, NULL);
    pump(60);

    const NetPacket *sk = last_of(&g_clients[JOINER], NET_PACKET_SKINS, (int) owner->uid);
    CHECK(sk != NULL, "the joiner got the owner's SKINS");
    if (sk) {
        NetSkinsPacketV2 skins;
        CHECK(net_packet_read_skins(&skins, sk->payload, sk->length), "SKINS V2 reads");
        CHECK(skins.paints.primary.color == 0x445566, "the named Topo copy's paint, not the first copy's (got %06x)", skins.paints.primary.color);
        CHECK(skins.paints.melee.color == -1 && skins.paints.melee.shader == -1, "the named unpainted melee copy shows no paint");
    }

    uint16_t len = 0;
    const uint8_t *body = last_ext(&g_clients[JOINER], NET_EXT_KN_SKIN_COPIES, (int) owner->uid, &len);
    CHECK(body != NULL, "the joiner got the owner's SKIN_COPIES");
    if (body) {
        CHECK(len == sizeof(NetSkinCopiesPacketV2) && len == 40, "SKIN_COPIES V2 is 40 bytes (got %u)", len);
        NetSkinCopiesPacketV2 p;
        memcpy(&p, body, sizeof(p));
        CHECK(p.v1.seed[0] == skin_copy_seed(502, 1, 77.0), "the named copy's seed (got %d)", p.v1.seed[0]);
        CHECK(near(p.wear[0], 0.45), "the named copy's wear (got %f)", p.wear[0]);
        CHECK(p.wear[1] < 0.0f, "no secondary: no wear");
        CHECK(p.wear[2] < 0.0f, "the named melee copy has no wear, the first copy's is not used (got %f)", p.wear[2]);
        // an older client reads the V1 prefix (28 bytes): the same uid and seeds
        NetSkinCopiesPacket v1;
        memcpy(&v1, body, sizeof(v1));
        CHECK(v1.uid == (int32_t) owner->uid && v1.seed[0] == p.v1.seed[0], "the V1 prefix carries the same seeds");
    }

    body = last_ext(&g_clients[JOINER], NET_EXT_KN_SKIN_STYLES, (int) owner->uid, &len);
    CHECK(body != NULL, "the joiner got the owner's SKIN_STYLES");
    if (body) {
        CHECK(len == sizeof(NetSkinStylesPacketV2) && len == 104, "SKIN_STYLES V2 is 104 bytes (got %u)", len);
        NetSkinStylesPacketV2 p;
        memcpy(&p, body, sizeof(p));
        int hat = -1, bod = -1;
        for (int k = 0; k < p.v1.count && k < NET_SKIN_STYLES_MAX; k++) {
            if (p.v1.skin[k] == HAT) hat = k;
            if (p.v1.skin[k] == BODY) bod = k;
        }
        CHECK(bod >= 0 && p.v1.style[bod] == 2 && p.wear[bod] < 0.0f, "the named body copy's style 2, and no wear (style %d wear %f)",
              bod >= 0 ? p.v1.style[bod] : -9, bod >= 0 ? p.wear[bod] : -9.0f);
        CHECK(hat >= 0 && p.v1.style[hat] == 0 && near(p.wear[hat], 0.2), "the plain hat: no style, the first copy's wear (wear %f)",
              hat >= 0 ? p.wear[hat] : -9.0f);
    }

    // the fallbacks, on the server's own view of the owner
    owner_conn = conn(&g_clients[OWNER]); // (the joiner's connection may have moved the table)
    if (!owner_conn) return;
    owner = owner_conn->player;
    NetSkinCopyIdsPacket saved = owner_conn->worn_copies;
    PlayerSkinPaints paints;
    owner_conn->worn_copies.copy_id[SKIN_COPY_SLOT_PRIMARY] = 999; // gone (traded, sold)
    CHECK(accounts_worn_copy(owner_conn, SKIN_COPY_SLOT_PRIMARY, TOPO) == NULL, "a gone copy is not worn");
    accounts_skin_paints(owner_conn, &owner->skins, &paints);
    CHECK(paints.primary.color == 0x112233, "a gone copy: the first painted copy (got %06x)", paints.primary.color);
    CHECK(near(accounts_worn_wear(owner_conn, SKIN_COPY_SLOT_PRIMARY, TOPO), 0.05), "a gone copy: the first copy's wear");
    owner_conn->worn_copies.copy_id[SKIN_COPY_SLOT_PRIMARY] = 503; // the melee's copy on the primary
    CHECK(accounts_worn_copy(owner_conn, SKIN_COPY_SLOT_PRIMARY, TOPO) == NULL, "a copy of another index is not worn");
    accounts_skin_paints(owner_conn, &owner->skins, &paints);
    CHECK(paints.primary.color == 0x112233, "a copy of another index: the plain-index rule (got %06x)", paints.primary.color);
    owner_conn->worn_copies = no_copies(); // an older client: no NET_EXT_KN_SKIN_COPY_IDS
    accounts_skin_paints(owner_conn, &owner->skins, &paints);
    CHECK(paints.primary.color == 0x112233 && paints.melee.color == 0x778899, "no copy ids: the plain-index rule for each slot");
    CHECK(near(accounts_worn_wear(owner_conn, SKIN_COPY_SLOT_MELEE, MELEE), 0.7), "no copy ids: the first worn melee copy's wear");
    // the copy follows its index to another slot (cosmetics_route): any slot holding the index names it
    owner_conn->worn_copies.skin[SKIN_COPY_SLOT_HEAD] = BODY, owner_conn->worn_copies.copy_id[SKIN_COPY_SLOT_HEAD] = 505;
    const ServerSkinCopy *moved = accounts_worn_copy(owner_conn, SKIN_COPY_SLOT_BODY, BODY);
    CHECK(moved && moved->id == 505 && moved->style == 1 && near(moved->wear, 0.9), "a copy named on another slot of the same index");
    owner_conn->worn_copies = saved;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const cJSON *burg = map_catalog_json((size_t) map_catalog_index_of_key("311_burg"));
    CHECK(burg != NULL, "map");
    static const cJSON *maps[1];
    maps[0] = burg;
    const SandboxConfig cfg = {.maps = maps, .map_count = 1, .map_index = 0, .mode_id = MODE_ID_FFA};
    if (sandbox_start(&cfg)) {
        test_named_copies_reach_a_joiner();
        for (int i = 0; i < CLIENTS; i++)
            if (g_clients[i].fd >= NET_LOOPBACK_FD_BASE) sandbox_disconnect(g_clients[i].fd);
        sandbox_stop();
    } else {
        CHECK(0, "sandbox start");
    }
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
