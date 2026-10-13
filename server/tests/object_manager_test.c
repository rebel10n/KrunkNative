// KrunkNative Object Manager (shared/include/object_manager.h) over the offline sandbox server
// (server_sandbox.h): extra loadout weapons (cap, switching by slot pick and wheel, persistence
// across respawns), weapon pickups dropped at the crosshair (SPAWNED_OBJ + PICKUP, undo, round
// start), the rate limit, the permissions (sandbox, custom game with and without knOmWeps,
// public game: a guest refused, a dev allowed, ranked) and the join sync (a late v7 joiner gets
// the extras and the spawned objects; a v3 peer never sees ids 250-253).
// Run from the repository root: ./<build>/object_manager_test
#include <server_sandbox.h>
#include <server.h>
#include <shared.h>
#include <object_manager.h>
#include <game_config.h>
#include <weapons.h>
#include <world.h>
#include <map_catalog.h>
#include <modes.h>
#include <knet.h> // NET_LOOPBACK_FD_BASE
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

#define MAX_RECEIVED 4096
#define CLIENTS 4

typedef struct {
    int fd;
    int conn_id;
    int v7;
    int seq;
    NetPacketBuffer buffer;
    NetPacket packets[MAX_RECEIVED];
    int count;
} TestClient;

static TestClient g_clients[CLIENTS];

static void send_to(TestClient *c, const int type, const void *payload, const uint16_t length) {
    CHECK(net_send_packet(c->fd, (NetPacketType) type, payload, length), "send type %d", type);
}

static void drain(TestClient *c) {
    uint8_t bytes[4096];
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

static void reset_received(void) {
    for (int i = 0; i < CLIENTS; i++) g_clients[i].count = 0;
}

static int count_type(const TestClient *c, const int type) {
    int n = 0;
    for (int i = 0; i < c->count; i++) n += c->packets[i].type == type;
    return n;
}

static int count_sandbox_block(const TestClient *c) {
    int n = 0;
    for (int i = 0; i < c->count; i++) n += c->packets[i].type >= NET_PACKET_BLOCK_SANDBOX && c->packets[i].type < NET_PACKET_BLOCK_END;
    return n;
}

static const NetPacket *last(const TestClient *c, const int type) {
    for (int i = c->count - 1; i >= 0; i--) {
        if (c->packets[i].type == type) return &c->packets[i];
    }
    return NULL;
}

static int index_of(const TestClient *c, const int type, const int from) {
    for (int i = from; i < c->count; i++) {
        if (c->packets[i].type == type) return i;
    }
    return -1;
}

static ClientConnection *conn(const TestClient *c) {
    for (size_t i = 0; i < g_server->client_count; i++) {
        if (g_server->clients[i].id == c->conn_id) return &g_server->clients[i];
    }
    return NULL;
}

static Player *player_of(const TestClient *c) {
    ClientConnection *cc = conn(c);
    return cc ? cc->player : NULL;
}

static void join(TestClient *c, const int v7) {
    memset(c, 0, sizeof(*c));
    c->v7 = v7;
    c->fd = sandbox_connect();
    CHECK(c->fd >= NET_LOOPBACK_FD_BASE && g_server->client_count, "connect");
    c->conn_id = g_server->client_count ? g_server->clients[g_server->client_count - 1].id : -1;
    const NetHelloPacket hello = {NET_PROTOCOL_MAJOR, (uint8_t) (v7 ? NET_PROTOCOL_MINOR_V7 : 0), 0};
    send_to(c, NET_PACKET_HELLO, &hello, sizeof(hello));
    NetJoinPacket join_packet;
    memset(&join_packet, 0, sizeof(join_packet));
    send_to(c, NET_PACKET_JOIN, &join_packet, (uint16_t) NET_JOIN_PACKET_V2_SIZE);
    NetEntPacket ent = {.class_index = 0, .secondary_index = -1, .cosmetics = PLAYER_COSMETICS_NONE, .slid_cont = 1, .lag_comp = 1.0f};
    send_to(c, NET_PACKET_ENT, &ent, (uint16_t) NET_ENT_PACKET_V7_SIZE);
    pump(30);
    CHECK(player_of(c) != NULL, "joined and spawned");
}

static void leave(TestClient *c) {
    if (c->fd >= NET_LOOPBACK_FD_BASE) sandbox_disconnect(c->fd);
    c->fd = -1;
    pump(4);
}

static int start(const cJSON *map, const int mode) {
    static const cJSON *maps[1];
    maps[0] = map;
    const SandboxConfig cfg = {.maps = maps, .map_count = 1, .map_index = 0, .mode_id = mode};
    for (int i = 0; i < CLIENTS; i++) g_clients[i].fd = -1;
    return sandbox_start(&cfg);
}

// One Object Manager request; the SPAWN_RESULT it got (status OM_STATUS_COUNT: none).
static NetSpawnResultPacket request(TestClient *c, const int action, const int item, const float pitch) {
    const Player *p = player_of(c);
    const NetSpawnReqPacket req = {.action = (uint8_t) action, .item = (int16_t) item, .pitch = pitch, .yaw = p ? p->direction.y : 0.0f};
    c->count = 0;
    send_to(c, NET_PACKET_SPAWN_REQ, &req, sizeof(req));
    pump(2);
    NetSpawnResultPacket r = {.status = OM_STATUS_COUNT};
    const NetPacket *packet = last(c, NET_PACKET_SPAWN_RESULT);
    if (packet && packet->length == sizeof(r)) memcpy(&r, packet->payload, sizeof(r));
    return r;
}

// One input frame: a slot pick (slot >= 0, sent as scroll slot + 2) or a wheel step.
static void input(TestClient *c, const int slot, const int scroll) {
    const Player *p = player_of(c);
    Input in = {.seq = ++c->seq, .move_dir = -1, .delta = 1.0f / 60.0f, .x_dir = p ? p->direction.x : 0.0f, .y_dir = p ? p->direction.y : 0.0f};
    in.select_slot = (unsigned char) (slot >= 0 ? slot + 1 : 0);
    in.scroll = (signed char) scroll;
    NetInputPacket packet;
    net_packet_from_input(&packet, &in);
    send_to(c, NET_PACKET_INPUT, &packet, sizeof(packet));
    pump(4);
}

static int loadout_has(const Player *p, const int id) {
    for (int i = 0; p && i < p->loadout_size; i++) {
        if (p->loadout[i] == id) return 1;
    }
    return 0;
}

static int listed_weapons(int *out, const int max) {
    int n = 0;
    for (int id = 0; id < WEAPON_COUNT && n < max; id++) {
        if (object_manager_weapon_listed(id)) out[n++] = id;
    }
    return n;
}

static int spawned_count(void) {
    int n = 0;
    const Map *map = g_server->game.map;
    for (size_t i = 0; map && i < map->object_count; i++) n += map->objects[i]->spawned && map->objects[i]->active;
    return n;
}

// ---------------------------------------------------------------- tests

static void test_codec(void) {
    printf("input codec...\n");
    Input in = {.seq = 5, .move_dir = -1, .delta = 0.016f, .select_slot = 7};
    NetInputPacket packet;
    net_packet_from_input(&packet, &in);
    CHECK(packet.scroll == 8, "slot 6 rides as scroll 8 (got %d)", packet.scroll);
    Input out;
    net_packet_to_input(&out, &packet);
    CHECK(out.select_slot == 7 && out.scroll == 0, "slot pick round trip (%d %d)", out.select_slot, out.scroll);
    in.select_slot = 0;
    in.scroll = -1;
    net_packet_from_input(&packet, &in);
    net_packet_to_input(&out, &packet);
    CHECK(out.select_slot == 0 && out.scroll == -1, "a wheel step stays a wheel step");
    CHECK(sizeof(NetSpawnReqPacket) == 12 && sizeof(NetSpawnResultPacket) == 8, "packet sizes");
    CHECK(sizeof(NetLoadoutExtraPacket) + 0 <= NET_MAX_PAYLOAD && sizeof(NetSpawnedObjPacket) + OM_RECORD_MAX + 1 <= NET_MAX_PAYLOAD, "frames fit");
    CHECK(NET_PACKET_SPAWN_REQ == 250 && NET_PACKET_SPAWN_RESULT == 251 && NET_PACKET_LOADOUT_EXTRA == 252 && NET_PACKET_SPAWNED_OBJ == 253, "ids 250-253");
}

static void test_loadout(TestClient *a, TestClient *b) {
    printf("extra loadout weapons...\n");
    Player *p = player_of(a);
    if (!p) return;
    const int base = p->loadout_size;
    NetSpawnResultPacket r = request(a, OM_ACTION_QUERY, 0, 0.0f);
    CHECK(r.status == OM_STATUS_OK && (r.caps & OM_CAP_WEAPONS) && (r.caps & OM_CAP_SPAWN) && (r.caps & OM_CAP_DEV),
          "sandbox + local player: weapons, spawn, dev (caps %u)", r.caps);
    CHECK(r.extra_max == PLAYER_EXTRA_WEAPONS_MAX, "extra_max");

    reset_received();
    r = request(a, OM_ACTION_ADD_WEAPON, 5 /* Shotgun */, 0.0f);
    pump(2);
    CHECK(r.status == OM_STATUS_OK, "add Shotgun (status %u)", r.status);
    CHECK(p->loadout_size == base + 1 && p->loadout[base] == 5 && p->loadout_base == base, "Shotgun in slot %d (size %d)", base, p->loadout_size);
    CHECK(p->ammo[base] == g_weapons[5]->ammo, "fresh ammo");
    CHECK(count_type(b, NET_PACKET_LOADOUT_EXTRA) == 1, "the other v7 player hears it");
    CHECK(conn(a)->om_extra_count == 1 && conn(a)->om_extras[0] == 5, "the connection keeps it");

    r = request(a, OM_ACTION_ADD_WEAPON, 5, 0.0f);
    CHECK(r.status == OM_STATUS_OK && p->loadout_size == base + 1, "adding it again changes nothing");
    r = request(a, OM_ACTION_ADD_WEAPON, WEAPON_ID_KNIFE, 0.0f);
    CHECK(r.status == OM_STATUS_INVALID, "the knife is not listed");
    r = request(a, OM_ACTION_ADD_WEAPON, 17, 0.0f);
    CHECK(r.status == OM_STATUS_INVALID, "the bomb is not listed");
    r = request(a, OM_ACTION_ADD_WEAPON, 99, 0.0f);
    CHECK(r.status == OM_STATUS_INVALID, "an unknown id");

    // switching: slot pick, wheel through every slot (wraps both ways), a noSwap extra in hand
    input(a, base, 0);
    CHECK(p->loadout_index == base && p->weapon == g_weapons[5], "slot pick -> the Shotgun (index %d)", p->loadout_index);
    r = request(a, OM_ACTION_ADD_WEAPON, WEAPON_ID_MINIGUN, 0.0f);
    pump(2);
    CHECK(p->loadout_size == base + 2 && p->loadout[base + 1] == WEAPON_ID_MINIGUN, "Minigun in slot %d", base + 1);
    input(a, -1, 1);
    CHECK(p->loadout_index == base + 1, "wheel: shotgun -> minigun (index %d)", p->loadout_index);
    input(a, -1, 1);
    CHECK(p->loadout_index == 0, "wheel wraps past the last slot to 0 (index %d)", p->loadout_index);
    input(a, -1, -1);
    CHECK(p->loadout_index == base + 1, "wheel back from 0 to the last slot (index %d)", p->loadout_index);
    input(a, 0, 0);
    CHECK(p->loadout_index == 0, "a Minigun in an extra slot does not lock the loadout (index %d)", p->loadout_index);
    input(a, 40, 0);
    CHECK(p->loadout_index == 0, "a slot past the loadout is ignored");

    // the cap
    int ids[WEAPON_COUNT];
    const int n = listed_weapons(ids, WEAPON_COUNT);
    CHECK(n > PLAYER_EXTRA_WEAPONS_MAX, "more listed weapons (%d) than the cap", n);
    int limited = 0, added = 0;
    for (int i = 0; i < n; i++) {
        pump(20); // the rate limit refills (4 per second)
        r = request(a, OM_ACTION_ADD_WEAPON, ids[i], 0.0f);
        added += r.status == OM_STATUS_OK;
        if (r.status == OM_STATUS_LIMIT) {
            limited = 1;
            CHECK(r.limit == PLAYER_EXTRA_WEAPONS_MAX, "limit %u", r.limit);
        }
    }
    CHECK(limited && p->extra_weapon_count == PLAYER_EXTRA_WEAPONS_MAX, "capped at %d (%d)", PLAYER_EXTRA_WEAPONS_MAX, p->extra_weapon_count);
    CHECK(p->loadout_size <= base + PLAYER_EXTRA_WEAPONS_MAX, "loadout %d", p->loadout_size);

    // respawn: the class loadout, then the extras again (SPAWN, then LOADOUT_EXTRA to everyone)
    const int full = p->loadout_size;
    p->health = 0.0f;
    p->active = 0;
    p->death_time = g_server->game.now - 10.0f;
    reset_received();
    NetEntPacket ent = {.class_index = 0, .secondary_index = -1, .cosmetics = PLAYER_COSMETICS_NONE, .slid_cont = 1, .lag_comp = 1.0f};
    send_to(a, NET_PACKET_ENT, &ent, (uint16_t) NET_ENT_PACKET_V7_SIZE);
    pump(6);
    p = player_of(a);
    CHECK(p && p->active && p->loadout_size == full, "respawned with the extras (%d of %d)", p ? p->loadout_size : -1, full);
    const int spawn_at = index_of(b, NET_PACKET_SPAWN, 0);
    CHECK(spawn_at >= 0 && index_of(b, NET_PACKET_LOADOUT_EXTRA, spawn_at) > spawn_at, "LOADOUT_EXTRA follows the SPAWN");

    // remove one, then all
    r = request(a, OM_ACTION_REMOVE_WEAPON, 5, 0.0f);
    CHECK(r.status == OM_STATUS_OK && !loadout_has(p, 5), "Shotgun taken out");
    pump(10);
    r = request(a, OM_ACTION_CLEAR_WEAPONS, 0, 0.0f);
    CHECK(r.status == OM_STATUS_OK && p->extra_weapon_count == 0 && p->loadout_size == base, "cleared (size %d)", p->loadout_size);
    pump(10);
}

static void test_rate(TestClient *a) {
    printf("rate limit...\n");
    pump(200);
    int rated = 0;
    for (int i = 0; i < 30; i++) {
        const NetSpawnReqPacket req = {.action = OM_ACTION_ADD_WEAPON, .item = 5};
        send_to(a, NET_PACKET_SPAWN_REQ, &req, sizeof(req));
    }
    a->count = 0;
    pump(2);
    for (int i = 0; i < a->count; i++) {
        if (a->packets[i].type != NET_PACKET_SPAWN_RESULT) continue;
        NetSpawnResultPacket r;
        memcpy(&r, a->packets[i].payload, sizeof(r));
        rated += r.status == OM_STATUS_RATE;
    }
    CHECK(rated >= 30 - (int) OM_RATE_BURST - 1, "a burst of 30 requests: %d refused", rated);
    CHECK(count_type(a, NET_PACKET_SPAWN_RESULT) == 30, "every request answered");
    pump(200);
    request(a, OM_ACTION_CLEAR_WEAPONS, 0, 0.0f);
    pump(10);
}

static void test_pickups(TestClient *a, TestClient *b) {
    printf("weapon pickups at the crosshair...\n");
    Player *p = player_of(a);
    if (!p) return;
    const size_t objects = g_server->game.map->object_count;
    reset_received();
    NetSpawnResultPacket r = request(a, OM_ACTION_DROP_WEAPON, 13 /* Crossbow */, -1.4f);
    pump(2);
    CHECK(r.status == OM_STATUS_OK, "drop a Crossbow at the floor (status %u)", r.status);
    CHECK(g_server->game.map->object_count == objects + 1, "a map object appended");
    const Object *o = g_server->game.map->objects[objects];
    CHECK(o->spawned && o->pickup && o->prefab == PREFAB_WEAPON_PICKUP && o->spawn_owner == p->uid, "a spawned WEAPON_PICKUP");
    GamePickup k;
    CHECK(game_pickup_state(&g_server->game, (int) objects, &k) && k.active && k.weapon == 13, "its pickup state (weapon %d)", k.weapon);
    CHECK(fabsf(o->position.x - p->position.x) < 40.0f && fabsf(o->position.z - p->position.z) < 40.0f, "near the player's feet");
    CHECK(count_type(b, NET_PACKET_SPAWNED_OBJ) == 1 && count_type(b, NET_PACKET_PICKUP) >= 1, "SPAWNED_OBJ + PICKUP to the others");
    const NetPacket *sp = last(b, NET_PACKET_SPAWNED_OBJ);
    if (sp) {
        NetSpawnedObjPacket head;
        memcpy(&head, sp->payload, sizeof(head));
        CHECK(head.op == OM_SPAWNED_ADD && head.id == (int) objects && head.owner == p->uid, "ADD uid %d", head.id);
        CHECK(strstr((const char *) sp->payload + sizeof(head), "\"i\":26") != NULL, "the record JSON");
    }

    r = request(a, OM_ACTION_DROP_WEAPON, 13, 1.5f);
    CHECK(r.status == OM_STATUS_NO_TARGET || r.status == OM_STATUS_OK, "aiming at the sky: no target (or a ceiling) (status %u)", r.status);
    if (r.status == OM_STATUS_OK) request(a, OM_ACTION_UNDO, 0, 0.0f);

    // the per-player pickup cap
    int limit = 0;
    for (int i = 0; i < OM_PICKUPS_PER_PLAYER + 2; i++) {
        pump(20);
        r = request(a, OM_ACTION_DROP_WEAPON, 1, -1.4f);
        if (r.status == OM_STATUS_LIMIT) limit = r.limit;
    }
    CHECK(limit == OM_PICKUPS_PER_PLAYER, "pickup cap %d (got %d)", OM_PICKUPS_PER_PLAYER, limit);
    CHECK(spawned_count() == OM_PICKUPS_PER_PLAYER, "%d live spawned pickups", spawned_count());

    // undo: the newest goes, REMOVE to everyone, its pickup off
    reset_received();
    const int before = spawned_count();
    pump(20);
    r = request(a, OM_ACTION_UNDO, 0, 0.0f);
    pump(2);
    CHECK(r.status == OM_STATUS_OK && spawned_count() == before - 1, "undo (%d -> %d)", before, spawned_count());
    const NetPacket *rm = last(b, NET_PACKET_SPAWNED_OBJ);
    if (rm) {
        NetSpawnedObjPacket head;
        memcpy(&head, rm->payload, sizeof(head));
        CHECK(head.op == OM_SPAWNED_REMOVE && rm->length == sizeof(head), "REMOVE");
        CHECK(game_pickup_state(&g_server->game, head.id, &k) && !k.active, "its pickup is off");
    } else CHECK(0, "REMOVE sent");
}

static void test_join_sync(TestClient *a, TestClient *late, TestClient *old) {
    printf("join sync...\n");
    pump(20);
    request(a, OM_ACTION_ADD_WEAPON, 5, 0.0f);
    pump(4);
    const int live = spawned_count();
    join(late, 1);
    pump(10);
    int adds = 0;
    for (int i = 0; i < late->count; i++) {
        if (late->packets[i].type != NET_PACKET_SPAWNED_OBJ) continue;
        NetSpawnedObjPacket head;
        memcpy(&head, late->packets[i].payload, sizeof(head));
        adds += head.op == OM_SPAWNED_ADD;
    }
    CHECK(adds == live, "the joiner got every live spawned object (%d of %d)", adds, live);
    int extras_for_a = 0;
    for (int i = 0; i < late->count; i++) {
        if (late->packets[i].type != NET_PACKET_LOADOUT_EXTRA) continue;
        NetLoadoutExtraPacket e;
        memcpy(&e, late->packets[i].payload, sizeof(e));
        if (e.uid == player_of(a)->uid) extras_for_a = e.count;
    }
    CHECK(extras_for_a == 1, "and the extras of a player already in game (%d)", extras_for_a);

    // the client mirror: the joiner's packets rebuild the same objects at the same uids
    Game mirror;
    memset(&mirror, 0, sizeof(mirror));
    const cJSON *maps[1] = {g_server->game.map->raw_data};
    int modes[1] = {MODE_ID_FFA};
    game_configure(&mirror, NULL, maps, 1, modes, 1, NULL, 0, NULL, 0);
    game_init(&mirror, 0, 0, 0);
    CHECK(mirror.ready && !game_is_authority(&mirror), "a client mirror");
    int applied = 0;
    for (int i = 0; i < late->count; i++) {
        if (late->packets[i].type == NET_PACKET_SPAWNED_OBJ) applied += object_manager_apply_spawned(&mirror, late->packets[i].payload, late->packets[i].length);
    }
    // removed ones are not replayed: the mirror ends at the last live one (a later ADD pads the gap)
    size_t last_live = 0;
    for (size_t i = 0; i < g_server->game.map->object_count; i++) {
        if (g_server->game.map->objects[i]->spawned && g_server->game.map->objects[i]->active) last_live = i;
    }
    CHECK(applied == live && mirror.map->object_count == last_live + 1, "mirror objects %zu, server's last live uid %zu",
          mirror.map->object_count, last_live);
    for (size_t i = 0; i < mirror.map->object_count && i < g_server->game.map->object_count; i++) {
        const Object *s = g_server->game.map->objects[i], *m = mirror.map->objects[i];
        if (!s->spawned || !s->active) continue;
        CHECK(m->spawned && m->pickup && m->position.x == s->position.x && m->position.y == s->position.y && m->position.z == s->position.z,
              "uid %zu matches", i);
    }

    // a v3 peer never sees the Object Manager's ids
    join(old, 0);
    pump(10);
    request(a, OM_ACTION_ADD_WEAPON, 6, 0.0f);
    pump(20);
    request(a, OM_ACTION_DROP_WEAPON, 6, -1.4f);
    pump(4);
    CHECK(count_sandbox_block(old) == 0, "no packet 250-254 reached the v3 peer (%d)", count_sandbox_block(old));
    const NetSpawnReqPacket req = {.action = OM_ACTION_ADD_WEAPON, .item = 5};
    old->count = 0;
    send_to(old, NET_PACKET_SPAWN_REQ, &req, sizeof(req));
    pump(2);
    CHECK(count_type(old, NET_PACKET_SPAWN_RESULT) == 0 && player_of(old)->extra_weapon_count == 0, "a v3 peer's request is ignored");

    // round start: every spawned object goes with the builds
    world_round_start(&g_server->game);
    CHECK(spawned_count() == 0, "round start removes the spawned objects");
    request(a, OM_ACTION_UNDO, 0, 0.0f);
    pump(2);
    CHECK(last(a, NET_PACKET_SPAWN_RESULT) && ((const NetSpawnResultPacket *) last(a, NET_PACKET_SPAWN_RESULT)->payload)->status == OM_STATUS_NOTHING,
          "nothing left to undo");
}

static void test_permissions(TestClient *dev, TestClient *guest) {
    printf("permissions...\n");
    ClientConnection *gc = conn(guest);
    if (!gc) return;
    gc->local = 0; // a remote guest: not DEV, not the host
    Game *game = &g_server->game;
    pump(200);

    // offline sandbox: everything for everyone
    NetSpawnResultPacket r = request(guest, OM_ACTION_QUERY, 0, 0.0f);
    CHECK((r.caps & OM_CAP_WEAPONS) && (r.caps & OM_CAP_SPAWN) && !(r.caps & OM_CAP_DEV), "sandbox guest: weapons + spawn (caps %u)", r.caps);

    // a custom game (hosted), not the sandbox: weapons for everyone while knOmWeps is on, no spawning
    game->is_sandbox = 0;
    CHECK(game_config_is_custom(game), "the sandbox is hosted");
    r = request(guest, OM_ACTION_ADD_WEAPON, 7, 0.0f);
    CHECK(r.status == OM_STATUS_OK && (r.caps & OM_CAP_WEAPONS) && !(r.caps & OM_CAP_SPAWN), "custom guest: extra weapons (caps %u)", r.caps);
    pump(20);
    game->config.om_weapons = 0;
    r = request(guest, OM_ACTION_ADD_WEAPON, 8, 0.0f);
    CHECK(r.status == OM_STATUS_DENIED, "knOmWeps off: refused (status %u)", r.status);
    r = request(guest, OM_ACTION_DROP_WEAPON, 8, -1.4f);
    CHECK(r.status == OM_STATUS_DENIED, "knOmWeps off: no pickups either");
    r = request(guest, OM_ACTION_REMOVE_WEAPON, 7, 0.0f);
    CHECK(r.status == OM_STATUS_OK, "giving one back is always fine");
    r = request(dev, OM_ACTION_ADD_WEAPON, 8, 0.0f);
    CHECK(r.status == OM_STATUS_OK, "knOmWeps off: a dev still may");
    pump(20);
    game->config.om_weapons = 1;

    // a public game (not custom): a guest is refused, a dev (local machine / DEV account) is not
    game_config_free(game);
    CHECK(!game_config_is_custom(game), "public now");
    r = request(guest, OM_ACTION_ADD_WEAPON, 9, 0.0f);
    CHECK(r.status == OM_STATUS_DENIED && r.caps == 0, "public guest refused (status %u caps %u)", r.status, r.caps);
    r = request(guest, OM_ACTION_DROP_WEAPON, 9, -1.4f);
    CHECK(r.status == OM_STATUS_DENIED, "public guest: no pickups");
    gc->acct.flags |= NET_PLAYER_INFO_DEV;
    pump(20);
    r = request(guest, OM_ACTION_ADD_WEAPON, 9, 0.0f);
    CHECK(r.status == OM_STATUS_OK && (r.caps & OM_CAP_DEV) && (r.caps & OM_CAP_SPAWN), "a DEV account is allowed (status %u caps %u)", r.status, r.caps);
    gc->acct.flags &= ~NET_PLAYER_INFO_DEV;

    // a respawn in a game that no longer allows them drops the guest's extras
    Player *p = player_of(guest);
    CHECK(p && p->extra_weapon_count > 0, "the guest has extras before the respawn");
    if (p) {
        p->active = 0;
        p->death_time = game->now - 10.0f;
        guest->count = 0;
        NetEntPacket ent = {.class_index = 0, .secondary_index = -1, .cosmetics = PLAYER_COSMETICS_NONE, .slid_cont = 1, .lag_comp = 1.0f};
        send_to(guest, NET_PACKET_ENT, &ent, (uint16_t) NET_ENT_PACKET_V7_SIZE);
        pump(6);
        p = player_of(guest);
        CHECK(p && p->active && p->extra_weapon_count == 0 && p->loadout_size == player_loadout_base(p) && gc->om_extra_count == 0,
              "a public respawn drops a guest's extras");
    }

    // ranked / comp: refused even in a custom game, devs excepted
    game->config.comp = 1;
    game->is_sandbox = 1;
    r = request(guest, OM_ACTION_ADD_WEAPON, 9, 0.0f);
    CHECK(r.status == OM_STATUS_DENIED && !(r.caps & OM_CAP_WEAPONS), "comp: refused (caps %u)", r.caps);
    r = request(dev, OM_ACTION_QUERY, 0, 0.0f);
    CHECK((r.caps & OM_CAP_WEAPONS) && (r.caps & OM_CAP_DEV), "comp: a dev still may");
    game->config.comp = 0;

    // not spawned
    p = player_of(guest);
    if (p) p->active = 0;
    r = request(guest, OM_ACTION_ADD_WEAPON, 9, 0.0f);
    CHECK(r.status == OM_STATUS_DEAD, "dead: refused (status %u)", r.status);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    test_codec();
    const cJSON *burg = map_catalog_json((size_t) map_catalog_index_of_key("311_burg"));
    CHECK(burg != NULL, "map");
    if (burg && start(burg, MODE_ID_FFA)) {
        join(&g_clients[0], 1);
        join(&g_clients[1], 1);
        test_loadout(&g_clients[0], &g_clients[1]);
        test_rate(&g_clients[0]);
        test_pickups(&g_clients[0], &g_clients[1]);
        test_join_sync(&g_clients[0], &g_clients[2], &g_clients[3]);
        test_permissions(&g_clients[0], &g_clients[1]);
        for (int i = 0; i < CLIENTS; i++) leave(&g_clients[i]);
        sandbox_stop();
    } else CHECK(0, "sandbox");
    printf("%d checks, %d failures\n", g_checks, g_failures);
    printf(g_failures ? "Test FAILED\n" : "Test OK\n");
    return g_failures ? 1 : 0;
}
