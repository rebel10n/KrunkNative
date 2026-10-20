// KrunkNative Object Manager (shared/include/object_manager.h) over the offline sandbox server
// (server_sandbox.h): extra loadout weapons (cap, switching by slot pick and wheel, persistence
// across respawns), weapon pickups dropped at the crosshair (SPAWNED_OBJ + PICKUP, undo, round
// start), the rate limit, the permissions (sandbox, custom game with and without knOmWeps,
// public game: a guest refused, a dev allowed, ranked) and the join sync (a late v7 joiner gets
// the extras and the spawned objects; a v3 peer never sees ids 250-253), the `give` / `drop`
// commands (by name / id, to others, without privilege; the dropped weapon is a pickup in front
// that another player takes), the Tool Gun (its tools, ownership, the spawn right, public) and the
// edits (the Physics Gun's grab / carry / turn / drop, the btools' snapped move and resize, undo,
// ownership, limits, no box into a player, a client mirror and its collision following), and the
// map edits on Kanji (a map block carried, resized, deleted, turned, copied; a jump pad moved and
// launching at its new spot; a guest refused; undo to the map's own Object; joiner replay; round
// start restoring the map).
// Run from the repository root: ./<build>/object_manager_test
#include <server_sandbox.h>
#include <server.h>
#include <shared.h>
#include <object_manager.h>
#include <game_config.h>
#include <weapons.h>
#include <world.h>
#include <ai.h>
#include <bots.h>
#include <spawnables.h>
#include <zombies.h>
#include <map_catalog.h>
#include <modes.h>
#include <knet.h> // NET_LOOPBACK_FD_BASE
#include <social_v7.h> // NetChatV7Packet (give / drop replies)
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ruleset.h>
#include <strings.h> // strcasecmp
#include <editor_model.h> // the editor's property table (the Properties tool's fields)

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

// The sandbox's local players are devs (no caps, no pace); 0 makes `c` a sandbox guest, whose caps,
// pace and "inside a player" refusal apply
static void set_dev(const TestClient *c, const int dev) {
    ClientConnection *cc = conn(c);
    if (cc) cc->local = (unsigned char) !!dev;
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
    CHECK(p->ammo[base] == rules_weapon(5)->ammo, "fresh ammo");
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
    CHECK(p->loadout_index == base && p->weapon == rules_weapon(5), "slot pick -> the Shotgun (index %d)", p->loadout_index);
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
    // a dev is never paced: a burst of 30 all answered OK
    pump(200);
    for (int i = 0; i < 30; i++) {
        const NetSpawnReqPacket req = {.action = OM_ACTION_ADD_WEAPON, .item = 5};
        send_to(a, NET_PACKET_SPAWN_REQ, &req, sizeof(req));
    }
    a->count = 0;
    pump(2);
    int dev_rated = 0, dev_answers = 0;
    for (int i = 0; i < a->count; i++) {
        if (a->packets[i].type != NET_PACKET_SPAWN_RESULT) continue;
        NetSpawnResultPacket r;
        memcpy(&r, a->packets[i].payload, sizeof(r));
        dev_rated += r.status == OM_STATUS_RATE;
        dev_answers++;
    }
    CHECK(dev_answers == 30 && dev_rated == 0, "a dev's burst of 30: no RATE (%d of %d)", dev_rated, dev_answers);
    request(a, OM_ACTION_CLEAR_WEAPONS, 0, 0.0f);
    // the bucket for everyone else (a sandbox guest)
    set_dev(a, 0);
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
    set_dev(a, 1);
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

    // the per-player pickup cap (a sandbox guest's; devs have none)
    set_dev(a, 0);
    int limit = 0;
    for (int i = 0; i < OM_PICKUPS_PER_PLAYER + 2; i++) {
        pump(20);
        r = request(a, OM_ACTION_DROP_WEAPON, 1, -1.4f);
        if (r.status == OM_STATUS_LIMIT) limit = r.limit;
    }
    CHECK(limit == OM_PICKUPS_PER_PLAYER, "pickup cap %d (got %d)", OM_PICKUPS_PER_PLAYER, limit);
    CHECK(spawned_count() == OM_PICKUPS_PER_PLAYER, "%d live spawned pickups", spawned_count());
    set_dev(a, 1);
    r = request(a, OM_ACTION_DROP_WEAPON, 1, -1.4f); // no pace either: no pump between
    NetSpawnResultPacket r2 = request(a, OM_ACTION_DROP_WEAPON, 1, -1.4f);
    CHECK(r.status == OM_STATUS_OK && r2.status == OM_STATUS_OK && spawned_count() == OM_PICKUPS_PER_PLAYER + 2,
          "a dev drops past the cap, back to back (%u %u, %d live)", r.status, r2.status, spawned_count());
    request(a, OM_ACTION_UNDO, 0, 0.0f);
    request(a, OM_ACTION_UNDO, 0, 0.0f);

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

static int ai_count(void) {
    const GameAIs *ais = g_server->game.ai;
    int n = 0;
    for (int i = 0; ais && i < ais->count; i++) n += ais->ais[i].active;
    return n;
}

static int index_of_entry(const int tab, const int kind, const int data) {
    int count = 0;
    const ObjectManagerEntry *e = object_manager_entries(tab, &count);
    for (int i = 0; i < count; i++) {
        if (e[i].kind == kind && (data < 0 || e[i].data == data)) return i;
    }
    return -1;
}

// part 2: the NPCs and Entities tabs
static void test_npcs_entities(TestClient *a, TestClient *b) {
    printf("NPCs and entities...\n");
    Player *p = player_of(a);
    if (!p) return;
    p->god_mode = 1;
    if (player_of(b)) player_of(b)->god_mode = 1;
    int npc_count = 0, entity_count = 0;
    const ObjectManagerEntry *npcs = object_manager_entries(OM_TAB_NPCS, &npc_count);
    const ObjectManagerEntry *entities = object_manager_entries(OM_TAB_ENTITIES, &entity_count);
    CHECK(npcs && npc_count >= 12 && entities && entity_count >= 20, "catalogues (%d NPCs, %d entities)", npc_count, entity_count);
    CHECK(object_manager_entries(OM_TAB_WEAPONS, NULL) == NULL, "the Weapons tab has no entries (rules_weapons())");

    // every NPC kind spawns at the crosshair floor (the cap stops the rest: a sandbox guest's)
    set_dev(a, 0);
    pump(200);
    const int ais_before = ai_count();
    const int bots_before = game_bots_count(&g_server->game);
    int ok = 0, limited = 0;
    for (int i = 0; i < npc_count; i++) {
        pump(20);
        const NetSpawnResultPacket r = request(a, OM_ACTION_SPAWN_NPC, i, -1.0f);
        CHECK(r.status == OM_STATUS_OK || r.status == OM_STATUS_LIMIT, "NPC %s (status %u)", npcs[i].name, r.status);
        ok += r.status == OM_STATUS_OK;
        if (r.status == OM_STATUS_LIMIT) limited = r.limit;
    }
    pump(4);
    // the cap counts live NPCs: one killed in the brawl makes room for another
    CHECK(ok >= OM_NPCS_PER_PLAYER && limited == OM_NPCS_PER_PLAYER, "%d NPCs spawned, then the per-player cap (%d)", ok, limited);
    set_dev(a, 1);
    CHECK(game_bots_count(&g_server->game) == bots_before + 1, "the Bot entry added a bot");
    // the AIs fight each other right away: one may already be down
    CHECK(ai_count() >= ais_before + OM_NPCS_PER_PLAYER - 3 && ai_count() <= ais_before + OM_NPCS_PER_PLAYER - 1, "the AIs (%d -> %d)",
          ais_before, ai_count());
    const GameBots *bots = g_server->game.bots;
    if (bots && bots->count) CHECK(bots->list[bots->count - 1].pinned, "the bot is pinned to the spot");
    // the AIs reach the other v7 player (AI_ADD_V7 through the regular AI sync)
    pump(30);
    CHECK(count_type(b, NET_PACKET_AI_ADD_V7) >= OM_NPCS_PER_PLAYER - 1, "AI_ADD_V7 to the other player (%d)", count_type(b, NET_PACKET_AI_ADD_V7));

    // undo: the newest NPC goes. Counted around the request alone (the authority handles it at once):
    // the AIs keep fighting each other, and one killed by another AI in the ticks before or after
    // the undo is not the undo's doing (it happened in the train once the RNG stream shifted)
    pump(20);
    const int before = ai_count() + game_bots_count(&g_server->game);
    NetSpawnResultPacket r = request(a, OM_ACTION_UNDO, 0, 0.0f);
    const int after = ai_count() + game_bots_count(&g_server->game);
    pump(2);
    CHECK(r.status == OM_STATUS_OK && after == before - 1, "undo removes the last NPC (%d -> %d)", before, after);
    for (int i = 0; i < 20; i++) { // undo everything this player spawned
        pump(20);
        if (request(a, OM_ACTION_UNDO, 0, 0.0f).status != OM_STATUS_OK) break;
    }
    CHECK(ai_count() == ais_before && game_bots_count(&g_server->game) == bots_before, "every spawned NPC undone (AIs %d, bots %d)",
          ai_count(), game_bots_count(&g_server->game));

    // entities: spawnables, an airdrop, scraps and machines (map objects)
    const int machines = g_server->game.zombies.perk_machine_count + g_server->game.zombies.krum_count + g_server->game.zombies.box_count;
    const size_t objects = g_server->game.map->object_count;
    int spawned_ok = 0;
    for (int i = 0; i < entity_count && i < OM_ENTITIES_PER_PLAYER; i++) {
        pump(20);
        r = request(a, OM_ACTION_SPAWN_ENTITY, i, -0.4f); // a few steps away: touch pickups stay up
        CHECK(r.status == OM_STATUS_OK, "entity %s (status %u)", entities[i].name, r.status);
        spawned_ok += r.status == OM_STATUS_OK;
    }
    pump(2);
    const int tag = index_of_entry(OM_TAB_ENTITIES, OM_ENTRY_SPAWNABLE, SPAWNABLE_KR_TAG);
    CHECK(tag >= 0, "a KR Tag entry");
    int alive_spawnables = 0;
    for (int i = 0; i < MODE_MAX_SPAWNABLES; i++) alive_spawnables += g_server->game.mode->spawnables[i].active;
    CHECK(alive_spawnables >= 15, "spawnables up (%d)", alive_spawnables);
    int drops = 0;
    for (int i = 0; i < g_server->game.zombies.drop_count; i++) drops += g_server->game.zombies.drops[i].active;
    CHECK(drops == 1, "one airdrop falling (%d)", drops);
    const size_t records = g_server->game.map->object_count - objects;
    int records_expected = 0;
    for (int i = 0; i < entity_count && i < OM_ENTITIES_PER_PLAYER; i++) records_expected += entities[i].kind == OM_ENTRY_RECORD;
    CHECK((int) records == records_expected, "record entries appended %zu map objects (%d)", records, records_expected);
    for (size_t i = objects; i < g_server->game.map->object_count; i++) {
        const Object *o = g_server->game.map->objects[i];
        CHECK(o->spawned && o->active && o->scale.x > 0.0f, "record %zu spawned with a size", i);
        if (o->prefab == PREFAB_TRIGGER) CHECK(o->trig && !o->destroyed, "a scrap is a live trigger (no start-destroyed)");
    }
    const int machines_after = g_server->game.zombies.perk_machine_count + g_server->game.zombies.krum_count + g_server->game.zombies.box_count;
    int machines_listed = 0;
    for (int i = 0; i < entity_count && i < OM_ENTITIES_PER_PLAYER; i++) machines_listed += entities[i].json && strstr(entities[i].json, "\"i\":9") != NULL;
    CHECK(machines_after - machines == machines_listed, "machines registered with the zombies runtime (%d)", machines_after - machines);

    // undo everything, then the entity cap with KR Tags (12 s each: the cap is reached before any expires)
    for (int i = 0; i < 40; i++) {
        pump(20);
        if (request(a, OM_ACTION_UNDO, 0, 0.0f).status != OM_STATUS_OK) break;
    }
    pump(200);
    set_dev(a, 0); // a sandbox guest's cap
    int tags = 0, cap = 0;
    for (int i = 0; i < OM_ENTITIES_PER_PLAYER + 2; i++) {
        pump(16); // the bucket refills 4 per second
        r = request(a, OM_ACTION_SPAWN_ENTITY, tag, -0.4f);
        tags += r.status == OM_STATUS_OK;
        if (r.status == OM_STATUS_LIMIT) cap = r.limit;
    }
    CHECK(tags == OM_ENTITIES_PER_PLAYER && cap == OM_ENTITIES_PER_PLAYER, "entity cap %d (%d spawned, limit %d)", OM_ENTITIES_PER_PLAYER, tags, cap);
    set_dev(a, 1);
    r = request(a, OM_ACTION_SPAWN_ENTITY, tag, -0.4f);
    CHECK(r.status == OM_STATUS_OK, "a dev spawns past the entity cap (status %u)", r.status);
    for (int i = 0; i < 40; i++) {
        pump(20);
        if (request(a, OM_ACTION_UNDO, 0, 0.0f).status != OM_STATUS_OK) break;
    }
    drops = 0;
    for (int i = 0; i < g_server->game.zombies.drop_count; i++) drops += g_server->game.zombies.drops[i].active;
    CHECK(drops == 0 && spawned_count() == 0, "every entity undone (drops %d, objects %d)", drops, spawned_count());

    // the rules: a custom game's host spawns, a guest of a public game does not
    const ObjectManagerPerms host = {.host = 1}, guest = {0}, ranked_host = {.host = 1, .ranked = 1};
    Game *game = &g_server->game;
    game->is_sandbox = 0;
    CHECK((object_manager_caps(game, &host) & OM_CAP_SPAWN) && !(object_manager_caps(game, &guest) & OM_CAP_SPAWN), "host spawns, guest not");
    CHECK(!(object_manager_caps(game, &ranked_host) & OM_CAP_SPAWN), "not in ranked");
    game->is_sandbox = 1;
    p->god_mode = 0;
}

static void undo_all(TestClient *c) {
    for (int i = 0; i < 80; i++) {
        pump(20);
        if (request(c, OM_ACTION_UNDO, 0, 0.0f).status != OM_STATUS_OK) break;
    }
}

// Where the newest live thing of `entry` stands (the spawnable / airdrop / record spawned since
// `sid0` / `objects`); 0 when nothing is up.
static int entity_spot(const ObjectManagerEntry *entry, const int sid0, const size_t objects, vec3 *at) {
    const Game *game = &g_server->game;
    if (entry->kind == OM_ENTRY_SPAWNABLE) {
        for (int s = 0; game->mode && s < MODE_MAX_SPAWNABLES; s++) {
            const ModeSpawnable *sp = &game->mode->spawnables[s];
            if (!sp->active || sp->sid < sid0 || sp->type != entry->data) continue;
            *at = (vec3) {sp->position.x, sp->position.y - 1.0f, sp->position.z};
            return 1;
        }
        return 0;
    }
    if (entry->kind == OM_ENTRY_AIRDROP) {
        for (int d = 0; d < game->zombies.drop_count; d++) {
            const AirDrop *drop = &game->zombies.drops[d];
            if (!drop->active) continue;
            *at = drop->position;
            return 1;
        }
        return 0;
    }
    if (game->map->object_count <= objects) return 0;
    const Object *o = game->map->objects[game->map->object_count - 1];
    if (!o->spawned || !o->active) return 0;
    *at = o->position;
    return 1;
}

// Every Entities entry spawned at the crosshair, then both players step onto it (pads launch,
// pickups are taken, scraps / machines trigger), several Slime Pads at once and their despawn.
// A crash anywhere on these paths fails the run (ASan builds report it with the stack).
static void test_entity_touch(TestClient *a, TestClient *b) {
    printf("every Entities entry, stepped on...\n");
    int count = 0;
    const ObjectManagerEntry *entries = object_manager_entries(OM_TAB_ENTITIES, &count);
    Player *p = player_of(a), *q = player_of(b);
    if (!entries || !p || !q) return;
    undo_all(a);
    const float round_timer = g_server->game.round_timer; // ~60 s of pumps: the round must not end meanwhile
    g_server->game.round_timer = 600.0f;
    p->god_mode = q->god_mode = 1; // the Nuke, falls after a launch
    const vec3 home = p->position, q_home = q->position;
    const vec2 dir = p->direction;
    const int slime = index_of_entry(OM_TAB_ENTITIES, OM_ENTRY_SPAWNABLE, SPAWNABLE_SLIME_PAD);
    const int mega = index_of_entry(OM_TAB_ENTITIES, OM_ENTRY_SPAWNABLE, SPAWNABLE_MEGA_SLIME_PAD);
    CHECK(slime >= 0 && mega >= 0, "Slime Pad and Mega Slime Pad entries");

    for (int i = 0; i < count; i++) {
        if (!(p = player_of(a)) || !(q = player_of(b))) break;
        p->position = home, p->velocity = (vec3) {0}, p->direction = dir;
        q->position = q_home, q->velocity = (vec3) {0};
        const int sid0 = g_server->game.mode->next_spawnable_sid;
        const size_t objects = g_server->game.map->object_count;
        pump(20);
        NetSpawnResultPacket r = request(a, OM_ACTION_SPAWN_ENTITY, i, -0.6f);
        CHECK(r.status == OM_STATUS_OK, "spawn %s (status %u)", entries[i].name, r.status);
        if (entries[i].kind == OM_ENTRY_AIRDROP) pump(900); // it falls AIRDROP_HEIGHT first
        vec3 at;
        if (!entity_spot(&entries[i], sid0, objects, &at)) {
            CHECK(entries[i].kind == OM_ENTRY_SPAWNABLE && !spawnable_info(entries[i].data)->always_active, "%s is up", entries[i].name);
            continue;
        }
        const int pad = i == slime || i == mega;
        float launch_p = 0.0f, launch_q = 0.0f;
        for (int f = 0; f < 30; f++) { // both stand in it for half a second (the server ticks every other step)
            if (!(p = player_of(a)) || !(q = player_of(b))) break;
            p->position = q->position = at;
            p->velocity = q->velocity = (vec3) {0};
            pump(1);
            launch_p = fmaxf(launch_p, p->velocity.y), launch_q = fmaxf(launch_q, q->velocity.y);
        }
        if (pad) CHECK(launch_p > 0.1f && launch_q > 0.1f, "%s launches both players (%.3f, %.3f)", entries[i].name, launch_p, launch_q);
        CHECK(player_of(a) && player_of(b), "both players still in after %s", entries[i].name);
        pump(60); // the launch / pickup plays out
        undo_all(a);
    }

    // several pads at once, walked across, then left to their 10 s despawn
    if ((p = player_of(a)) && (q = player_of(b))) {
        p->position = home, p->velocity = (vec3) {0}, p->direction = dir;
        int pads = 0;
        for (int i = 0; i < 8; i++) {
            pump(20);
            p->direction.y = dir.y + (float) i * 0.7f;
            pads += request(a, OM_ACTION_SPAWN_ENTITY, i % 2 ? mega : slime, -0.6f).status == OM_STATUS_OK;
        }
        CHECK(pads == 8, "8 pads spawned (%d)", pads);
        for (int s = 0; s < MODE_MAX_SPAWNABLES; s++) {
            const ModeSpawnable *sp = &g_server->game.mode->spawnables[s];
            if (!sp->active || (sp->type != SPAWNABLE_SLIME_PAD && sp->type != SPAWNABLE_MEGA_SLIME_PAD)) continue;
            p->position = q->position = (vec3) {sp->position.x, sp->position.y - 1.0f, sp->position.z};
            pump(3);
        }
        pump(60 * 11);
        int left = 0;
        for (int s = 0; s < MODE_MAX_SPAWNABLES; s++) {
            const ModeSpawnable *sp = &g_server->game.mode->spawnables[s];
            left += sp->active && (sp->type == SPAWNABLE_SLIME_PAD || sp->type == SPAWNABLE_MEGA_SLIME_PAD);
        }
        CHECK(left == 0, "the pads despawn on their 10 s timer (%d left)", left);
    }
    undo_all(a);
    if ((p = player_of(a))) p->position = home, p->velocity = (vec3) {0}, p->direction = dir, p->god_mode = 0;
    if ((q = player_of(b))) q->position = q_home, q->velocity = (vec3) {0}, q->god_mode = 0;
    pump(30);
    g_server->game.round_timer = round_timer;
}

// The (pitch, yaw) from `p`'s eye to `target` (player_proc_input's look: x = -sin(yaw) cos(pitch)).
static void aim_at(const Player *p, const vec3 target, float *pitch, float *yaw) {
    const float eye = p->position.y + p->height - game_constants.camera_height;
    const float dx = target.x - p->position.x, dy = target.y - eye, dz = target.z - p->position.z;
    *yaw = atan2f(-dx, -dz);
    *pitch = atan2f(dy, sqrtf(dx * dx + dz * dz));
}

static NetSpawnResultPacket request_at(TestClient *c, const int action, const int item, const float pitch, const float yaw) {
    const NetSpawnReqPacket req = {.action = (uint8_t) action, .item = (int16_t) item, .pitch = pitch, .yaw = yaw};
    c->count = 0;
    send_to(c, NET_PACKET_SPAWN_REQ, &req, sizeof(req));
    pump(2);
    NetSpawnResultPacket r = {.status = OM_STATUS_COUNT};
    const NetPacket *packet = last(c, NET_PACKET_SPAWN_RESULT);
    if (packet && packet->length == sizeof(r)) memcpy(&r, packet->payload, sizeof(r));
    return r;
}

static const Object *last_spawned(void) {
    const Map *map = g_server->game.map;
    for (size_t i = map->object_count; i-- > 0;) {
        if (map->objects[i]->spawned && map->objects[i]->active) return map->objects[i];
    }
    return NULL;
}

static int entry_index(const int tab, const char *name) {
    int count = 0;
    const ObjectManagerEntry *e = object_manager_entries(tab, &count);
    for (int i = 0; i < count; i++) {
        if (!strcmp(e[i].name, name)) return i;
    }
    return -1;
}

// part 3: the Objects tab, placement, remove at the crosshair
static void test_objects(TestClient *a, TestClient *b) {
    printf("objects...\n");
    Player *p = player_of(a);
    if (!p) return;
    p->god_mode = 1;
    int count = 0;
    const ObjectManagerEntry *objects = object_manager_entries(OM_TAB_OBJECTS, &count);
    CHECK(objects && count >= 40, "objects catalogue (%d)", count);
    for (int i = 0; i < count; i++) CHECK(objects[i].kind == OM_ENTRY_RECORD && objects[i].json, "object %s is a record", objects[i].name);
    pump(200);

    // straight down: the crate stands on the floor under the player, turned to a quarter
    const int crate = entry_index(OM_TAB_OBJECTS, "Crate");
    CHECK(crate >= 0, "a Crate entry");
    const float feet = p->position.y;
    NetSpawnResultPacket r = request_at(a, OM_ACTION_SPAWN_OBJECT, crate, -1.55f, p->direction.y);
    pump(2);
    const Object *o = last_spawned();
    CHECK(r.status == OM_STATUS_OK && o && o->prefab == PREFAB_CRATE, "crate spawned (status %u)", r.status);
    if (o) {
        CHECK(fabsf(o->position.y - feet) < 0.6f, "on the floor (crate y %.2f, feet %.2f)", o->position.y, feet);
        CHECK(o->collision_type == COLLISION_TYPE_BOX && o->scale.x > 5.0f, "a solid box (%.1f)", o->scale.x);
        const float q = o->rotation.y / ((float) M_PI / 2.0f);
        CHECK(fabsf(q - roundf(q)) < 0.01f, "turned to a quarter (r.y %.3f)", o->rotation.y);
        CHECK(count_type(b, NET_PACKET_SPAWNED_OBJ) >= 1, "the other player gets it");
    }
    // standing inside it, the remove tool still finds it (any aim)
    pump(20);
    r = request_at(a, OM_ACTION_REMOVE_AIM, 0, 0.3f, p->direction.y);
    CHECK(r.status == OM_STATUS_OK && o && !o->active, "a crate around the player is removed from inside (status %u)", r.status);
    // a second pallet at the same aim lands on top of the first (the ray meets its top face)
    const int pallet = entry_index(OM_TAB_OBJECTS, "Pallet");
    pump(20);
    r = request_at(a, OM_ACTION_SPAWN_OBJECT, pallet, -0.6f, p->direction.y);
    const Object *bottom = last_spawned();
    pump(20);
    r = request_at(a, OM_ACTION_SPAWN_OBJECT, pallet, -0.6f, p->direction.y);
    const Object *top = last_spawned();
    CHECK(bottom && top && top != bottom && fabsf(top->position.y - (bottom->position.y + bottom->scale.y)) < 0.6f,
          "the second pallet stands on the first (%.2f on %.2f + %.2f)", top ? top->position.y : -1.0f, bottom ? bottom->position.y : -1.0f,
          bottom ? bottom->scale.y : -1.0f);

    // a ramp carries map.js's direction along the look
    pump(20);
    const int ramp = entry_index(OM_TAB_OBJECTS, "Ramp");
    r = request_at(a, OM_ACTION_SPAWN_OBJECT, ramp, -0.4f, 0.0f);
    const Object *ro = last_spawned();
    CHECK(r.status == OM_STATUS_OK && ro && ro->prefab == PREFAB_RAMP && ro->ramp, "ramp spawned (status %u)", r.status);
    if (ro) CHECK(cJSON_IsNumber(cJSON_GetObjectItem(ro->raw, "d")), "with a \"d\"");

    // every entry spawns (the per-player object cap may end the run)
    int ok = 0;
    for (int i = 0; i < count; i++) {
        pump(16);
        r = request_at(a, OM_ACTION_SPAWN_OBJECT, i, -0.5f, (float) i * 0.37f);
        ok += r.status == OM_STATUS_OK;
        CHECK(r.status == OM_STATUS_OK || r.status == OM_STATUS_NO_TARGET || r.status == OM_STATUS_LIMIT, "%s (status %u)", objects[i].name, r.status);
    }
    CHECK(ok >= count - 4, "%d of %d objects placed", ok, count);

    // remove at the crosshair: the thing the ray reaches first
    for (int i = 0; i < 80; i++) {
        pump(16);
        if (request(a, OM_ACTION_UNDO, 0, 0.0f).status != OM_STATUS_OK) break;
    }
    pump(200);
    const float yaw = p->direction.y;
    r = request_at(a, OM_ACTION_SPAWN_OBJECT, crate, -0.6f, yaw);
    pump(2);
    o = last_spawned();
    CHECK(r.status == OM_STATUS_OK && o, "a crate in front");
    if (o) {
        float pitch, aim_yaw;
        aim_at(p, (vec3) {o->position.x, o->position.y + o->scale.y * 0.5f, o->position.z}, &pitch, &aim_yaw);
        const Object *crate_obj = o;
        // someone else (a guest of a custom game, no dev) cannot take it away
        ClientConnection *gc = conn(b);
        Player *gp = player_of(b);
        g_server->game.is_sandbox = 0;
        if (gc && gp) {
            gc->local = 0;
            gp->position = p->position;
            gp->height = p->height;
            pump(20);
            r = request_at(b, OM_ACTION_REMOVE_AIM, 0, pitch, aim_yaw);
            CHECK(r.status == OM_STATUS_DENIED && crate_obj->active, "not the owner: refused (status %u)", r.status);
            gc->local = 1;
        }
        g_server->game.is_sandbox = 1;
        pump(20);
        r = request_at(a, OM_ACTION_REMOVE_AIM, 0, pitch, aim_yaw);
        CHECK(r.status == OM_STATUS_OK && !crate_obj->active, "the owner removes it at the crosshair (status %u)", r.status);
        pump(20);
        r = request_at(a, OM_ACTION_REMOVE_AIM, 0, pitch, aim_yaw);
        CHECK(r.status == OM_STATUS_NOTHING, "nothing left there (status %u)", r.status);
    }

    // an NPC at the crosshair
    pump(20);
    const int ghost = entry_index(OM_TAB_NPCS, "Ghost");
    r = request_at(a, OM_ACTION_SPAWN_NPC, ghost, -0.4f, yaw);
    pump(2);
    const GameAIs *ais = g_server->game.ai;
    const GameAI *g = NULL;
    for (int i = 0; ais && i < ais->count; i++) {
        if (ais->ais[i].active && !strcmp(ais->ais[i].name, "Ghost")) g = &ais->ais[i];
    }
    CHECK(r.status == OM_STATUS_OK && g, "a ghost in front (status %u)", r.status);
    if (g) {
        const int sid = g->sid;
        float pitch, aim_yaw;
        aim_at(p, (vec3) {g->position.x, g->position.y + g->height, g->position.z}, &pitch, &aim_yaw);
        pump(20);
        r = request_at(a, OM_ACTION_REMOVE_AIM, 0, pitch, aim_yaw);
        CHECK(r.status == OM_STATUS_OK && !game_ai_find(&g_server->game, sid), "the ghost removed at the crosshair (status %u)", r.status);
    }
    p->god_mode = 0;
}


// ---------------------------------------------------------------- give / drop / Tool Gun

static void say(TestClient *c, const char *text) {
    NetChatSendPacket packet;
    memset(&packet, 0, sizeof(packet));
    snprintf(packet.text, sizeof(packet.text), "%s", text);
    pump(40); // under the chat burst limit (3 / s)
    c->count = 0;
    send_to(c, NET_PACKET_CHAT_SEND, &packet, (uint16_t) (strlen(packet.text) + 1));
    pump(3);
}

// The last Server line `c` got since its packets were cleared ("" none)
static const char *server_line(const TestClient *c) {
    static char text[NET_CHAT_TEXT_SIZE];
    text[0] = 0;
    for (int i = 0; i < c->count; i++) {
        const NetPacket *p = &c->packets[i];
        NetChatPacket chat;
        if (p->type == NET_PACKET_CHAT && p->length >= sizeof(chat)) memcpy(&chat, p->payload, sizeof(chat));
        else if (p->type == NET_PACKET_CHAT_V7 && p->length >= sizeof(NetChatV7Packet)) {
            NetChatV7Packet v7;
            memcpy(&v7, p->payload, sizeof(v7));
            chat = v7.chat;
        } else continue;
        if (chat.uid == -1) snprintf(text, sizeof(text), "%s", chat.text);
    }
    return text;
}

static const char *notify_text(const TestClient *c) {
    static NetNotifyPacket n;
    const NetPacket *p = last(c, NET_PACKET_NOTIFY);
    if (!p || p->length != sizeof(n)) return "";
    memcpy(&n, p->payload, sizeof(n));
    n.text[sizeof(n.text) - 1] = 0;
    return n.text;
}

static int slot_of(const Player *p, const int id) {
    for (int i = 0; p && i < p->loadout_size; i++) {
        if (p->loadout[i] == id) return i;
    }
    return -1;
}

static void hold(TestClient *c, const int id) {
    const int slot = slot_of(player_of(c), id);
    if (slot >= 0) input(c, slot, 0);
}

static void clean_up(TestClient *c) {
    for (int i = 0; i < 80; i++) {
        pump(16);
        if (request(c, OM_ACTION_UNDO, 0, 0.0f).status != OM_STATUS_OK) break;
    }
    pump(20);
    request(c, OM_ACTION_CLEAR_WEAPONS, 0, 0.0f);
    pump(20);
}

static void test_give_drop(TestClient *a, TestClient *b) {
    printf("give / drop...\n");
    Player *p = player_of(a), *q = player_of(b);
    ClientConnection *bc = conn(b);
    if (!p || !q || !bc) return;
    clean_up(a);
    clean_up(b);
    pump(200);

    // a selector after the item receives it, as one before the item does (give sniper * = give * sniper). The
    // round's clock is kept as test_entity_touch keeps it: the pumps here must not end the round for the rest.
    const float round_timer = g_server->game.round_timer;
    g_server->game.round_timer = 600.0f;
    int sniper = -1;
    CHECK(object_manager_give_lookup("sniper", &sniper, NULL) == OM_GIVE_WEAPON, "sniper resolves (%d)", sniper);
    say(a, "/give sniper *");
    CHECK(strstr(server_line(a), "Gave Sniper Rifle: 2 players") != NULL, "give sniper *: '%s'", server_line(a));
    CHECK(slot_of(p, sniper) >= 0 && slot_of(q, sniper) >= 0, "both players got the Sniper Rifle");
    request(a, OM_ACTION_REMOVE_WEAPON, sniper, 0.0f); // take them back: no clean_up, so the sections after keep their timing
    request(b, OM_ACTION_REMOVE_WEAPON, sniper, 0.0f);
    pump(4);
    CHECK(slot_of(p, sniper) < 0 && slot_of(q, sniper) < 0, "and back out of both loadouts");
    g_server->game.round_timer = round_timer;

    // lookups (shared): names normalised, exact > prefix > substring, ids, entities, ambiguity
    int id = -1, matches = 0;
    CHECK(object_manager_give_lookup("crossbow", &id, NULL) == OM_GIVE_WEAPON && id == 13, "crossbow -> 13 (%d)", id);
    CHECK(object_manager_give_lookup("Alien", &id, NULL) == OM_GIVE_WEAPON && id == 11, "prefix Alien -> 11 (%d)", id);
    CHECK(object_manager_give_lookup("\"tool gun\"", &id, NULL) == OM_GIVE_WEAPON && id == WEAPON_ID_TOOL_GUN, "tool gun (%d)", id);
    CHECK(object_manager_give_lookup("24", &id, NULL) == OM_GIVE_WEAPON && id == 24, "by id");
    CHECK(object_manager_give_lookup("piss", &id, NULL) == OM_GIVE_WEAPON && id == WEAPON_ID_PISS, "piss -> the KrunkNative Piss (%d)", id);
    CHECK(object_manager_weapon_listed(WEAPON_ID_PISS) && object_manager_weapon_allowed(WEAPON_ID_PISS), "the Piss is a Weapons tile");
    CHECK(object_manager_give_lookup("drank", &id, NULL) == OM_GIVE_WEAPON && id == WEAPON_ID_DRANK, "drank -> the KrunkNative Purple Drank (%d)", id);
    CHECK(object_manager_give_lookup("money", &id, NULL) == OM_GIVE_WEAPON && id == WEAPON_ID_MONEY_SPREAD, "money -> the Money Spread (%d)", id);
    CHECK(object_manager_weapon_listed(WEAPON_ID_DRANK) && object_manager_weapon_listed(WEAPON_ID_MONEY_SPREAD), "the Nettspend's are Weapons tiles");
    CHECK(object_manager_give_lookup("12", &id, NULL) == OM_GIVE_NONE, "no knife");
    CHECK(object_manager_give_lookup("krtag", &id, NULL) == OM_GIVE_ENTITY && id == 0, "krtag -> the KR Tag entry (%d)", id);
    CHECK(object_manager_give_lookup("a", &id, &matches) == OM_GIVE_NONE && matches > 1, "'a' is ambiguous (%d)", matches);
    CHECK(object_manager_give_lookup("mystery box", &id, NULL) == OM_GIVE_NONE, "machines are no give");

    // give yourself a weapon: an extra slot, the connection keeps it
    reset_received();
    say(a, "/give crossbow");
    CHECK(slot_of(p, 13) >= player_loadout_base(p) && p->extra_weapon_count == 1, "give crossbow: an extra slot (%d extras)", p->extra_weapon_count);
    CHECK(strstr(server_line(a), "Crossbow added to your loadout") != NULL, "reply '%s'", server_line(a));
    CHECK(conn(a)->om_extra_count == 1 && conn(a)->om_extras[0] == 13, "the connection keeps a given weapon");
    CHECK(count_type(b, NET_PACKET_LOADOUT_EXTRA) >= 1, "the others hear LOADOUT_EXTRA");
    say(a, "/give \"Alien Blaster\"");
    CHECK(slot_of(p, 11) >= 0 && p->extra_weapon_count == 2, "a quoted name");
    say(a, "/give zzzz");
    CHECK(strstr(server_line(a), "Unknown weapon or item") != NULL, "unknown: '%s'", server_line(a));
    say(a, "/give a");
    CHECK(strstr(server_line(a), "be more specific") != NULL, "ambiguous: '%s'", server_line(a));
    say(a, "/give");
    CHECK(strstr(server_line(a), "Usage: /give") != NULL, "usage");

    // give another player: a dev may, a guest may not
    char line[96];
    snprintf(line, sizeof(line), "/give famas %d", q->uid);
    say(a, line);
    CHECK(slot_of(q, 14) >= 0 && q->extra_weapon_count == 1, "the dev gives b a Famas ('%s')", server_line(a));
    CHECK(strstr(server_line(a), "Gave Famas to") != NULL, "reply '%s'", server_line(a));
    CHECK(bc->om_extra_count == 1 && bc->om_extras[0] == 14, "b's connection keeps it");
    bc->local = 0; // a remote guest: neither dev nor host
    snprintf(line, sizeof(line), "/give \"desert eagle\" %d", p->uid);
    say(b, line);
    CHECK(slot_of(p, 10) < 0, "a guest gives nobody else anything");
    CHECK(strstr(server_line(b), "Only developers and the host") != NULL, "refusal '%s'", server_line(b));
    CHECK(strstr(notify_text(b), "Host or developer") != NULL, "and the notify '%s'", notify_text(b));
    // an item: the sandbox guest may spawn, at their own feet
    say(b, "/give krtag");
    CHECK(strstr(server_line(b), "KR Tag dropped at your feet") != NULL, "give krtag: '%s'", server_line(b));

    // drop: the held extra weapon becomes a pickup in front of the player, out of the loadout
    pump(200);
    hold(a, 13);
    CHECK(p->weapon == &g_crossbow, "the Crossbow in hand");
    const size_t objects = g_server->game.map->object_count;
    const vec3 at = p->position;
    const float yaw = p->direction.y;
    say(a, "/drop");
    CHECK(strstr(server_line(a), "Dropped the Crossbow") != NULL, "drop: '%s'", server_line(a));
    CHECK(slot_of(p, 13) < 0 && p->extra_weapon_count == 1 && conn(a)->om_extra_count == 1, "out of the loadout and the connection");
    const Object *o = g_server->game.map->object_count > objects ? g_server->game.map->objects[g_server->game.map->object_count - 1] : NULL;
    GamePickup k;
    CHECK(o && o->spawned && o->prefab == PREFAB_WEAPON_PICKUP && game_pickup_state(&g_server->game, (int) (g_server->game.map->object_count - 1), &k) &&
          k.active && k.weapon == 13, "a Crossbow pickup");
    if (o) {
        const float dx = o->position.x - at.x, dz = o->position.z - at.z;
        const float ahead = dx * -sinf(yaw) + dz * -cosf(yaw);
        CHECK(ahead > 1.0f && ahead <= OM_DROP_TOSS + 0.5f && sqrtf(dx * dx + dz * dz) < OM_DROP_TOSS + 1.0f, "in front of the player (%.1f ahead)", ahead);
        CHECK(fabsf(o->position.y - at.y) < 3.0f, "on the floor (%.1f, feet %.1f)", o->position.y, at.y);
        // another player picks it up (v7 interact type 6 on its uid)
        pump(200);
        q->position = o->position;
        q->velocity = (vec3) {0};
        const int uid = (int) (g_server->game.map->object_count - 1);
        const NetInteractV7Packet interact = {.action = 1, .type = GAME_INTERACT_PICKUP, .uid = uid};
        send_to(b, NET_PACKET_INTERACT, &interact, sizeof(interact));
        pump(4);
        CHECK(slot_of(q, 13) >= 0, "b picked the dropped Crossbow up");
    }
    // the drop key ("drpw") on an extra slot does the same, answered by SPAWN_RESULT
    pump(200);
    hold(a, 11);
    reset_received();
    const NetInteractV7Packet drpw = {.action = 2};
    send_to(a, NET_PACKET_INTERACT, &drpw, sizeof(drpw));
    pump(3);
    const NetPacket *res = last(a, NET_PACKET_SPAWN_RESULT);
    NetSpawnResultPacket r = {.status = OM_STATUS_COUNT};
    if (res && res->length == sizeof(r)) memcpy(&r, res->payload, sizeof(r));
    CHECK(r.action == OM_ACTION_DROP_HELD && r.status == OM_STATUS_OK && r.item == 11 && slot_of(p, 11) < 0, "drop key on an extra (status %u)", r.status);
    // class weapons and the melee stay (v7: only picked-up weapons drop)
    hold(a, p->loadout[0]);
    say(a, "/drop");
    CHECK(strstr(server_line(a), "can't drop your class weapons") != NULL && p->loadout_size >= 2, "a class weapon stays: '%s'", server_line(a));
    hold(a, WEAPON_ID_KNIFE);
    say(a, "/drop");
    CHECK(strstr(server_line(a), "melee") != NULL && slot_of(p, WEAPON_ID_KNIFE) >= 0, "the melee stays: '%s'", server_line(a));

    // without the privilege: a custom game with knOmWeps off
    Game *game = &g_server->game;
    game->is_sandbox = 0;
    game->config.om_weapons = 0;
    pump(200);
    say(b, "/give crossbow");
    CHECK(strstr(server_line(b), "extra weapons are off") != NULL, "no weapons: '%s'", server_line(b));
    CHECK(strstr(notify_text(b), "extra weapons are off") != NULL, "notify '%s'", notify_text(b));
    say(b, "/give krtag");
    CHECK(strstr(server_line(b), "only developers and the host can spawn") != NULL, "no items: '%s'", server_line(b));
    hold(b, 14);
    say(b, "/drop");
    CHECK(strstr(server_line(b), "Not allowed") != NULL && slot_of(q, 14) >= 0, "no drop either: '%s'", server_line(b));
    say(a, "/give eagle");
    CHECK(slot_of(p, 10) >= 0, "a dev still gives himself one");
    game->config.om_weapons = 1;
    game->is_sandbox = 1;
    bc->local = 1;
    clean_up(a);
    clean_up(b);
}

static void test_tool_gun(TestClient *a, TestClient *b) {
    printf("tool gun...\n");
    Player *p = player_of(a), *q = player_of(b);
    ClientConnection *bc = conn(b);
    if (!p || !q || !bc) return;
    Game *game = &g_server->game;
    p->god_mode = q->god_mode = 1;
    pump(200);
    CHECK(object_manager_tool_info(OM_TOOL_DELETER) && !strcmp(object_manager_tool_info(OM_TOOL_DELETER)->name, "Deleter"), "tools catalogue");
    CHECK(rules_weapon(WEAPON_ID_TOOL_GUN) == &g_tool_gun && g_tool_gun.kn_tool && !object_manager_weapon_listed(WEAPON_ID_TOOL_GUN) &&
          object_manager_weapon_allowed(WEAPON_ID_TOOL_GUN), "the Tool Gun is an extra slot, not a Weapons tile");

    // without the Tool Gun in hand: refused
    NetSpawnResultPacket r = request(a, OM_ACTION_TOOL, OM_TOOL_INFO, 0.0f);
    CHECK(r.status == OM_STATUS_NO_TOOL, "no Tool Gun in hand (status %u)", r.status);
    pump(20);
    r = request(a, OM_ACTION_ADD_WEAPON, WEAPON_ID_TOOL_GUN, 0.0f);
    pump(4);
    hold(a, WEAPON_ID_TOOL_GUN);
    CHECK(r.status == OM_STATUS_OK && p->weapon == &g_tool_gun, "the Tool Gun in hand (status %u)", r.status);

    // its shot: the fire timer, no ammo spent, nobody hurt
    for (int i = 0; i < 20; i++) input(a, -1, 0); // past the swap timer
    const float health = q->health;
    q->position = (vec3) {p->position.x - sinf(p->direction.y) * 20.0f, p->position.y, p->position.z - cosf(p->direction.y) * 20.0f};
    const unsigned int ammo = p->ammo[p->loadout_index];
    Input in = {.seq = ++a->seq, .move_dir = -1, .delta = 1.0f / 60.0f, .x_dir = 0.0f, .y_dir = p->direction.y, .shoot = 1};
    NetInputPacket packet;
    net_packet_from_input(&packet, &in);
    send_to(a, NET_PACKET_INPUT, &packet, sizeof(packet));
    pump(4);
    CHECK(p->reloads[p->loadout_index] > 0.0f, "the Tool Gun fired (fire timer %.2f)", p->reloads[p->loadout_index]);
    CHECK(p->ammo[p->loadout_index] == ammo && q->health == health, "a Tool Gun shot spends no ammo and hurts nobody");

    // a crate of a's in front
    const int crate = entry_index(OM_TAB_OBJECTS, "Crate");
    pump(20);
    r = request_at(a, OM_ACTION_SPAWN_OBJECT, crate, -0.6f, p->direction.y);
    const Object *o = last_spawned();
    CHECK(r.status == OM_STATUS_OK && o && o->prefab == PREFAB_CRATE, "a crate (status %u)", r.status);
    if (!o) return;
    float pitch, yaw;
    aim_at(p, (vec3) {o->position.x, o->position.y + o->scale.y * 0.5f, o->position.z}, &pitch, &yaw);
    const int uid = (int) (game->map->object_count - 1);

    // Info: kind, owner, uid as a NOTIFY
    pump(20);
    r = request_at(a, OM_ACTION_TOOL, OM_TOOL_INFO, pitch, yaw);
    char want[32];
    snprintf(want, sizeof(want), "object %d", uid);
    CHECK(r.status == OM_STATUS_OK && r.limit == OM_TOOL_SHOWN && strstr(notify_text(a), "Crate") && strstr(notify_text(a), want),
          "Info: '%s' (status %u)", notify_text(a), r.status);
    pump(20);
    r = request_at(a, OM_ACTION_TOOL, OM_TOOL_INFO, -1.55f, p->direction.y + 3.0f);
    CHECK(r.status == OM_STATUS_OK && strstr(notify_text(a), "part of the map"), "Info on the map: '%s'", notify_text(a));

    // Rotator: a quarter turn (w / l swapped) in place: the same uid, an undo step
    const float ry = o->rotation.y;
    const vec3 sc = o->scale, at = o->position;
    const size_t count_before = game->map->object_count;
    pump(20);
    r = request_at(a, OM_ACTION_TOOL, OM_TOOL_ROTATOR, pitch, yaw);
    const Object *turned = game->map->objects[uid];
    CHECK(r.status == OM_STATUS_OK && turned->active && turned->prefab == PREFAB_CRATE && game->map->object_count == count_before,
          "Rotator in place (status %u)", r.status);
    const float d = fmodf(turned->rotation.y - ry + 4.0f * (float) M_PI, 2.0f * (float) M_PI);
    CHECK(fabsf(d - (float) M_PI / 2.0f) < 0.02f, "a quarter turn (%.3f -> %.3f)", ry, turned->rotation.y);
    CHECK(fabsf(turned->scale.x - sc.z) < 0.01f && fabsf(turned->scale.z - sc.x) < 0.01f, "w and l swapped");
    CHECK(fabsf(turned->position.x - at.x) < 0.01f && fabsf(turned->position.z - at.z) < 0.01f, "in place");
    pump(20);
    r = request(a, OM_ACTION_UNDO, 0, 0.0f);
    CHECK(r.status == OM_STATUS_OK && game->map->objects[uid]->active && fabsf(game->map->objects[uid]->rotation.y - ry) < 0.02f,
          "Undo turns it back (status %u)", r.status);
    pump(20);
    request_at(a, OM_ACTION_TOOL, OM_TOOL_ROTATOR, pitch, yaw);
    o = game->map->objects[uid];

    // Duplicator: copy, then place the copy on the floor elsewhere
    const int before = spawned_count();
    pump(20);
    r = request_at(a, OM_ACTION_TOOL, OM_TOOL_DUPLICATOR, pitch, yaw);
    CHECK(r.status == OM_STATUS_OK && r.limit == OM_TOOL_COPIED, "Duplicator copies (status %u limit %u)", r.status, r.limit);
    pump(20);
    r = request_at(a, OM_ACTION_TOOL, OM_TOOL_DUPLICATOR, -0.5f, p->direction.y + 1.2f);
    const Object *dup = last_spawned();
    CHECK(r.status == OM_STATUS_OK && r.limit == OM_TOOL_PASTED && spawned_count() == before + 1 && dup && dup != o && dup->prefab == PREFAB_CRATE,
          "and places the copy (status %u limit %u)", r.status, r.limit);
    pump(20);
    r = request_at(a, OM_ACTION_TOOL, OM_TOOL_DUPLICATOR, -1.55f, p->direction.y + 3.0f);
    // the floor under a: a map object (copied: devs copy the map's objects too) or the terrain (nothing)
    CHECK(r.status == OM_STATUS_UNSUPPORTED || r.status == OM_STATUS_NOTHING || (r.status == OM_STATUS_OK && r.limit == OM_TOOL_COPIED),
          "the Duplicator on the floor (status %u limit %u)", r.status, r.limit);
    p->om_copy = 0;
    if (dup) { // the copy is a's own: undo takes it back
        pump(20);
        request(a, OM_ACTION_UNDO, 0, 0.0f);
        CHECK(!dup->active, "the copy is undone like a spawn");
    }

    // Deleter: someone else's spawn is refused unless dev or host
    bc->local = 0; // b: the sandbox lets it spawn, but it is no dev
    pump(20);
    r = request(b, OM_ACTION_ADD_WEAPON, WEAPON_ID_TOOL_GUN, 0.0f);
    pump(4);
    hold(b, WEAPON_ID_TOOL_GUN);
    CHECK(r.status == OM_STATUS_OK && q->weapon == &g_tool_gun, "b's Tool Gun (status %u)", r.status);
    q->position = p->position;
    q->height = p->height;
    pump(20);
    r = request_at(b, OM_ACTION_TOOL, OM_TOOL_DELETER, pitch, yaw);
    CHECK(r.status == OM_STATUS_DENIED && o && o->active, "Deleter on a's crate: refused for b (status %u)", r.status);
    pump(20);
    r = request_at(b, OM_ACTION_TOOL, OM_TOOL_ROTATOR, pitch, yaw);
    CHECK(r.status == OM_STATUS_DENIED, "and so is the Rotator (status %u)", r.status);
    pump(20);
    r = request_at(a, OM_ACTION_TOOL, OM_TOOL_DELETER, pitch, yaw);
    CHECK(r.status == OM_STATUS_OK && o && !o->active, "the owner deletes it (status %u)", r.status);
    // b's own crate: b deletes it; a dev deletes b's
    pump(20);
    r = request_at(b, OM_ACTION_SPAWN_OBJECT, crate, -0.6f, p->direction.y);
    const Object *bo = last_spawned();
    pump(20);
    r = request_at(b, OM_ACTION_TOOL, OM_TOOL_DELETER, pitch, yaw);
    CHECK(r.status == OM_STATUS_OK && bo && !bo->active, "b deletes its own (status %u)", r.status);
    pump(20);
    r = request_at(b, OM_ACTION_SPAWN_OBJECT, crate, -0.6f, p->direction.y);
    bo = last_spawned();
    pump(20);
    r = request_at(a, OM_ACTION_TOOL, OM_TOOL_DELETER, pitch, yaw);
    CHECK(r.status == OM_STATUS_OK && bo && !bo->active, "a dev deletes b's (status %u)", r.status);
    pump(20);
    r = request(b, OM_ACTION_TOOL, OM_TOOL_DELETER, 0.0f);
    CHECK(r.status == OM_STATUS_NOTHING || r.status == OM_STATUS_DENIED, "no spawn left for b, the map is not b's (status %u)", r.status);

    // the spawn right: a custom game's guest keeps no Tool Gun and fires no tool
    game->is_sandbox = 0;
    pump(20);
    r = request(b, OM_ACTION_TOOL, OM_TOOL_INFO, 0.0f);
    CHECK(r.status == OM_STATUS_DENIED, "custom guest: no tools (status %u)", r.status);
    pump(20);
    request(b, OM_ACTION_CLEAR_WEAPONS, 0, 0.0f);
    pump(20);
    r = request(b, OM_ACTION_ADD_WEAPON, WEAPON_ID_TOOL_GUN, 0.0f);
    CHECK(r.status == OM_STATUS_DENIED, "custom guest: no Tool Gun (status %u)", r.status);
    int ids[2] = {WEAPON_ID_TOOL_GUN, 5};
    const ObjectManagerPerms guest = {0};
    CHECK(object_manager_filter_extras(game, &guest, ids, 2) == 1 && ids[0] == 5, "a respawn without the spawn right drops the Tool Gun");
    game->is_sandbox = 1;
    bc->local = 1;
    p->god_mode = q->god_mode = 0;
    clean_up(a);
    clean_up(b);
}

// After test_permissions (a public game): give and the tools are refused to a guest
static void test_public_refusals(TestClient *dev, TestClient *guest) {
    printf("public give / tools...\n");
    Game *game = &g_server->game;
    ClientConnection *gc = conn(guest);
    if (!gc) return;
    game->is_sandbox = 0;
    gc->local = 0;
    CHECK(!game_config_is_custom(game), "public");
    Player *gp = player_of(guest);
    if (gp && !gp->active) {
        gp->death_time = game->now - 10.0f;
        NetEntPacket ent = {.class_index = 0, .secondary_index = -1, .cosmetics = PLAYER_COSMETICS_NONE, .slid_cont = 1, .lag_comp = 1.0f};
        send_to(guest, NET_PACKET_ENT, &ent, (uint16_t) NET_ENT_PACKET_V7_SIZE);
        pump(6);
        gp = player_of(guest);
    }
    CHECK(gp && gp->active, "the guest is back");
    say(guest, "/give crossbow");
    CHECK(strstr(server_line(guest), "extra weapons are off") != NULL && gp && slot_of(gp, 13) < 0, "public: give refused '%s'", server_line(guest));
    say(guest, "/give \"tool gun\"");
    CHECK(strstr(server_line(guest), "only developers and the host can spawn") != NULL, "public: no Tool Gun '%s'", server_line(guest));
    pump(20);
    NetSpawnResultPacket r = request(guest, OM_ACTION_TOOL, OM_TOOL_DELETER, -1.0f);
    CHECK(r.status == OM_STATUS_DENIED, "public: no tools (status %u)", r.status);
    say(guest, "/drop");
    CHECK(strstr(server_line(guest), "class weapons") != NULL, "public: class weapons stay '%s'", server_line(guest));
    say(dev, "/give \"tool gun\"");
    CHECK(slot_of(player_of(dev), WEAPON_ID_TOOL_GUN) >= 0, "public: a dev has one ('%s')", server_line(dev));
    game->is_sandbox = 1;
    gc->local = 1;
    clean_up(dev);
}


// ---------------------------------------------------------------- edits: Physics Gun, btools

static NetSpawnResultPacket edit_at(TestClient *c, const int source, const int uid, const ObjectManagerPose *pose, const int flags) {
    const Player *p = player_of(c);
    NetSpawnEditPacket e;
    memset(&e, 0, sizeof(e));
    e.req = (NetSpawnReqPacket) {.action = OM_ACTION_EDIT, .item = (int16_t) source, .pitch = p ? p->direction.x : 0.0f, .yaw = p ? p->direction.y : 0.0f};
    e.uid = uid;
    e.position[0] = pose->position.x, e.position[1] = pose->position.y, e.position[2] = pose->position.z;
    e.size[0] = pose->size.x, e.size[1] = pose->size.y, e.size[2] = pose->size.z;
    e.quarter = (uint8_t) pose->quarter;
    e.flags = (uint8_t) flags;
    c->count = 0;
    send_to(c, NET_PACKET_SPAWN_REQ, &e, sizeof(e));
    pump(2);
    NetSpawnResultPacket r = {.status = OM_STATUS_COUNT};
    const NetPacket *packet = last(c, NET_PACKET_SPAWN_RESULT);
    if (packet && packet->length == sizeof(r)) memcpy(&r, packet->payload, sizeof(r));
    return r;
}

// A Physics Gun edit with any yaw (OM_EDIT_YAW: the packet's reserved carries it)
static NetSpawnResultPacket edit_yaw(TestClient *c, const int uid, const ObjectManagerPose *pose) {
    const Player *p = player_of(c);
    NetSpawnEditPacket e;
    memset(&e, 0, sizeof(e));
    e.req = (NetSpawnReqPacket) {.action = OM_ACTION_EDIT, .item = OM_EDIT_SOURCE_PHYSGUN, .pitch = p ? p->direction.x : 0.0f, .yaw = p ? p->direction.y : 0.0f};
    e.uid = uid;
    e.position[0] = pose->position.x, e.position[1] = pose->position.y, e.position[2] = pose->position.z;
    e.size[0] = pose->size.x, e.size[1] = pose->size.y, e.size[2] = pose->size.z;
    e.quarter = (uint8_t) object_manager_yaw_quarter(pose->yaw);
    e.flags = OM_EDIT_BEGIN | OM_EDIT_YAW;
    e.reserved = object_manager_yaw_pack(pose->yaw);
    c->count = 0;
    send_to(c, NET_PACKET_SPAWN_REQ, &e, sizeof(e));
    pump(2);
    NetSpawnResultPacket r = {.status = OM_STATUS_COUNT};
    const NetPacket *packet = last(c, NET_PACKET_SPAWN_RESULT);
    if (packet && packet->length == sizeof(r)) memcpy(&r, packet->payload, sizeof(r));
    return r;
}

// The Physics Gun on a player (OM_EDIT_PLAYER): carry `target_uid` to `feet` (release: let go)
static NetSpawnResultPacket edit_player(TestClient *c, const int target_uid, const vec3 feet, const int release) {
    const Player *p = player_of(c);
    NetSpawnEditPacket e;
    memset(&e, 0, sizeof(e));
    e.req = (NetSpawnReqPacket) {.action = OM_ACTION_EDIT, .item = OM_EDIT_SOURCE_PHYSGUN, .pitch = p ? p->direction.x : 0.0f, .yaw = p ? p->direction.y : 0.0f};
    e.uid = target_uid;
    e.position[0] = feet.x, e.position[1] = feet.y, e.position[2] = feet.z;
    e.flags = (uint8_t) (OM_EDIT_PLAYER | (release ? OM_EDIT_RELEASE : 0));
    c->count = 0;
    send_to(c, NET_PACKET_SPAWN_REQ, &e, sizeof(e));
    pump(2);
    NetSpawnResultPacket r = {.status = OM_STATUS_COUNT};
    const NetPacket *packet = last(c, NET_PACKET_SPAWN_RESULT);
    if (packet && packet->length == sizeof(r)) memcpy(&r, packet->payload, sizeof(r));
    return r;
}

// The last OM_SPAWNED_UPDATE `c` got for `uid`: its record's "p" (1 found)
static int update_p(const TestClient *c, const int uid, vec3 *p) {
    int found = 0;
    for (int i = 0; i < c->count; i++) {
        const NetPacket *k = &c->packets[i];
        if (k->type != NET_PACKET_SPAWNED_OBJ || k->length <= sizeof(NetSpawnedObjPacket)) continue;
        NetSpawnedObjPacket head;
        memcpy(&head, k->payload, sizeof(head));
        if (head.op != OM_SPAWNED_UPDATE || head.id != uid) continue;
        cJSON *rec = cJSON_Parse((const char *) k->payload + sizeof(head));
        const cJSON *a = cJSON_GetObjectItem(rec, "p");
        if (cJSON_GetArraySize(a) == 3) {
            *p = (vec3) {(float) cJSON_GetArrayItem(a, 0)->valuedouble, (float) cJSON_GetArrayItem(a, 1)->valuedouble, (float) cJSON_GetArrayItem(a, 2)->valuedouble};
            found = 1;
        }
        cJSON_Delete(rec);
    }
    return found;
}

static void test_pose_math(void) {
    printf("edit math...\n");
    CHECK(object_manager_snap(7.4f, 5.0f) == 5.0f && object_manager_snap(7.6f, 5.0f) == 10.0f && object_manager_snap(3.3f, 0.0f) == 3.3f, "snap");
    const ObjectManagerPose box = {{0.0f, 0.0f, 0.0f}, {10.0f, 10.0f, 4.0f}, 0, 0};
    ObjectManagerPose o;
    object_manager_pose_move(&box, 0, 6.6f, 5.0f, &o);
    CHECK(o.position.x == 5.0f && o.position.y == 0.0f && o.size.x == 10.0f, "move x 6.6 snapped to 5 (%.2f)", o.position.x);
    object_manager_pose_move(&box, 1, 2.2f, 1.0f, &o);
    CHECK(o.position.y == 2.0f, "move y");
    object_manager_pose_resize(&box, 0, 1, 4.0f, 2.0f, 0, &o);
    CHECK(o.size.x == 14.0f && o.position.x == 2.0f, "+x face out 4: size 14, centre +2 (%.2f, %.2f)", o.size.x, o.position.x);
    object_manager_pose_resize(&box, 0, -1, 4.0f, 2.0f, 0, &o);
    CHECK(o.size.x == 14.0f && o.position.x == -2.0f, "-x face out 4: centre -2");
    object_manager_pose_resize(&box, 0, 1, 4.0f, 2.0f, 1, &o);
    CHECK(o.size.x == 18.0f && o.position.x == 0.0f, "symmetric: both faces (%.2f)", o.size.x);
    object_manager_pose_resize(&box, 1, 1, 5.0f, 5.0f, 0, &o);
    CHECK(o.size.y == 15.0f && o.position.y == 0.0f, "top face up: the feet stay");
    object_manager_pose_resize(&box, 1, -1, 5.0f, 5.0f, 0, &o);
    CHECK(o.size.y == 15.0f && o.position.y == -5.0f, "bottom face down: the feet go down (%.2f)", o.position.y);
    object_manager_pose_resize(&box, 2, 1, -20.0f, 1.0f, 0, &o);
    CHECK(o.size.z == OM_EDIT_SIZE_MIN && fabsf(o.position.z - (OM_EDIT_SIZE_MIN - 4.0f) * 0.5f) < 0.001f, "the minimum size (%.2f)", o.size.z);
    object_manager_pose_turn(&box, 1, &o);
    CHECK(o.quarter == 1 && o.size.x == 4.0f && o.size.z == 10.0f, "a quarter turn swaps w and l");
    const ObjectManagerPose ramp = {{0, 0, 0}, {10, 5, 20}, 3, 1};
    object_manager_pose_turn(&ramp, 1, &o);
    CHECK(o.quarter == 0 && o.size.x == 10.0f && o.size.z == 20.0f, "a ramp turns its \"d\" only");
}

static void test_edits(TestClient *a, TestClient *b) {
    printf("edits (Physics Gun, Move / Resize)...\n");
    Player *p = player_of(a), *q = player_of(b);
    ClientConnection *bc = conn(b);
    if (!p || !q || !bc) return;
    Game *game = &g_server->game;
    p->god_mode = q->god_mode = 1;
    clean_up(a);
    clean_up(b);
    pump(200);
    CHECK(rules_weapon(WEAPON_ID_PHYS_GUN) == &g_phys_gun && g_phys_gun.kn_physgun && object_manager_weapon_allowed(WEAPON_ID_PHYS_GUN) &&
          !object_manager_weapon_listed(WEAPON_ID_PHYS_GUN), "the Physics Gun: an extra slot, not a Weapons tile");
    CHECK(object_manager_tool_info(OM_TOOL_MOVE)->handles && object_manager_tool_info(OM_TOOL_RESIZE)->handles, "Move / Resize are handle tools");

    // b's client: a mirror game that applies what b hears
    Game mirror;
    memset(&mirror, 0, sizeof(mirror));
    const cJSON *maps[1] = {game->map->raw_data};
    int modes[1] = {MODE_ID_FFA};
    game_configure(&mirror, NULL, maps, 1, modes, 1, NULL, 0, NULL, 0);
    game_init(&mirror, 0, 0, 0);
    const int crate = entry_index(OM_TAB_OBJECTS, "Crate");
    b->count = 0;
    NetSpawnResultPacket r = request_at(a, OM_ACTION_SPAWN_OBJECT, crate, -0.6f, p->direction.y);
    const int uid = (int) game->map->object_count - 1;
    pump(2);
    for (int i = 0; i < b->count; i++) {
        if (b->packets[i].type == NET_PACKET_SPAWNED_OBJ) object_manager_apply_spawned(&mirror, b->packets[i].payload, b->packets[i].length);
    }
    ObjectManagerPose from, to;
    CHECK(r.status == OM_STATUS_OK && object_manager_pose(game, uid, &from), "a crate to edit (status %u)", r.status);
    const ObjectManagerPose spawned_pose = from;
    // an edit needs the Physics Gun in hand
    to = from;
    to.position.x += 10.0f;
    pump(20);
    r = edit_at(a, OM_EDIT_SOURCE_PHYSGUN, uid, &to, OM_EDIT_BEGIN);
    CHECK(r.status == OM_STATUS_NO_TOOL, "no Physics Gun in hand (status %u)", r.status);
    pump(20);
    r = request(a, OM_ACTION_ADD_WEAPON, WEAPON_ID_PHYS_GUN, 0.0f);
    pump(4);
    hold(a, WEAPON_ID_PHYS_GUN);
    CHECK(r.status == OM_STATUS_OK && p->weapon == &g_phys_gun, "the Physics Gun in hand (status %u)", r.status);
    // it never fires
    for (int i = 0; i < 20; i++) input(a, -1, 0);
    Input in = {.seq = ++a->seq, .move_dir = -1, .delta = 1.0f / 60.0f, .x_dir = 0.0f, .y_dir = p->direction.y, .shoot = 1};
    NetInputPacket packet;
    net_packet_from_input(&packet, &in);
    send_to(a, NET_PACKET_INPUT, &packet, sizeof(packet));
    pump(4);
    CHECK(p->reloads[p->loadout_index] == 0.0f, "the Physics Gun fires nothing");

    // grab and carry: the pose in place, everyone gets the update, collision follows
    reset_received();
    r = edit_at(a, OM_EDIT_SOURCE_PHYSGUN, uid, &to, OM_EDIT_BEGIN);
    const Object *o = game->map->objects[uid];
    CHECK(r.status == OM_STATUS_OK && o->active && fabsf(o->position.x - to.position.x) < 0.06f, "grab + move (status %u, x %.2f)", r.status, o->position.x);
    pump(2);
    vec3 seen;
    CHECK(update_p(b, uid, &seen) && fabsf(seen.x - o->position.x) < 0.06f, "the other player gets OM_SPAWNED_UPDATE (x %.2f)", seen.x);
    for (int i = 0; i < b->count; i++) {
        if (b->packets[i].type == NET_PACKET_SPAWNED_OBJ) object_manager_apply_spawned(&mirror, b->packets[i].payload, b->packets[i].length);
    }
    const Object *m = mirror.map && (size_t) uid < mirror.map->object_count ? mirror.map->objects[uid] : NULL;
    CHECK(m && m->spawned && m->active && fabsf(m->position.x - o->position.x) < 0.001f && fabsf(m->position.z - o->position.z) < 0.001f,
          "b's client moves its copy in place (same uid)");
    if (m) {
        const vec3 top = {m->position.x, m->position.y + 50.0f, m->position.z};
        const float hit = line_in_rect(top, (vec3) {0.0f, -100.0f, 0.0f}, m->position, m->scale);
        CHECK(hit > 0.0f && fabsf(top.y - 100.0f * hit - (o->position.y + o->scale.y)) < 0.05f, "and its collision box with it");
    }
    const cJSON *rp = cJSON_GetObjectItem(o->raw, "p");
    CHECK(fabsf((float) cJSON_GetArrayItem(rp, 0)->valuedouble - o->position.x) < 0.06f, "the record a joiner gets follows (OM_SPAWNED_ADD replays it)");
    ObjectManagerAim aim;
    float pitch, yaw;
    aim_at(p, (vec3) {o->position.x, o->position.y + o->scale.y * 0.5f, o->position.z}, &pitch, &yaw);
    CHECK(object_manager_aim(game, p, pitch, yaw, &aim) && aim.object == uid, "the crosshair ray hits it where it went (object %d)", aim.object);
    // the editor's own clock
    to.position.x += 2.0f;
    r = edit_at(a, OM_EDIT_SOURCE_PHYSGUN, uid, &to, 0);
    CHECK(r.status == OM_STATUS_RATE || r.status == OM_STATUS_OK, "edits are paced (status %u)", r.status);
    pump(6);
    r = edit_at(a, OM_EDIT_SOURCE_PHYSGUN, uid, &to, 0);
    CHECK(r.status == OM_STATUS_OK, "the drag goes on (status %u)", r.status);
    // turned (quarter) by the rotate key, then released with Drop: down onto the floor
    object_manager_pose(game, uid, &from);
    object_manager_pose_turn(&from, 1, &to);
    to.position.y += 25.0f;
    pump(6);
    r = edit_at(a, OM_EDIT_SOURCE_PHYSGUN, uid, &to, OM_EDIT_DROP);
    o = game->map->objects[uid];
    CHECK(r.status == OM_STATUS_OK && fabsf(o->position.y - from.position.y) < 0.6f, "Drop: back on the floor (%.2f, was %.2f)", o->position.y, from.position.y);
    CHECK(fabsf(o->scale.x - from.size.z) < 0.01f, "and turned (w / l %.1f %.1f)", o->scale.x, o->scale.z);

    // btools: Resize with the Tool Gun, collision follows the new size
    pump(20);
    request(a, OM_ACTION_ADD_WEAPON, WEAPON_ID_TOOL_GUN, 0.0f);
    pump(4);
    hold(a, WEAPON_ID_TOOL_GUN);
    object_manager_pose(game, uid, &from);
    object_manager_pose_resize(&from, 1, 1, 7.7f, 5.0f, 0, &to);
    pump(20);
    r = edit_at(a, OM_EDIT_SOURCE_BTOOLS, uid, &to, OM_EDIT_BEGIN);
    o = game->map->objects[uid];
    CHECK(r.status == OM_STATUS_OK && fabsf(o->scale.y - (from.size.y + 10.0f)) < 0.01f, "Resize the top face up 10 (snap 5; %.2f)", o->scale.y);
    const vec3 above = {o->position.x, o->position.y + 60.0f, o->position.z};
    const float t = line_in_rect(above, (vec3) {0.0f, -100.0f, 0.0f}, o->position, o->scale);
    CHECK(t > 0.0f && fabsf(above.y - 100.0f * t - (o->position.y + o->scale.y)) < 0.05f, "a ray from above lands on the new top (%.2f)", above.y - 100.0f * t);
    object_manager_pose_move(&from, 2, 5.0f, 5.0f, &to);
    to.size = o->scale;
    pump(6);
    r = edit_at(a, OM_EDIT_SOURCE_BTOOLS, uid, &to, OM_EDIT_BEGIN);
    CHECK(r.status == OM_STATUS_OK && fabsf(game->map->objects[uid]->position.z - to.position.z) < 0.06f, "Move along z (status %u)", r.status);
    // limits
    ObjectManagerPose bad = to;
    bad.size.x = OM_EDIT_SIZE_MAX + 50.0f;
    pump(6);
    r = edit_at(a, OM_EDIT_SOURCE_BTOOLS, uid, &bad, 0);
    CHECK(r.status == OM_STATUS_BOUNDS, "too big (status %u)", r.status);
    bad = to;
    bad.position.x += 1000.0f;
    pump(6);
    r = edit_at(a, OM_EDIT_SOURCE_BTOOLS, uid, &bad, 0);
    CHECK(r.status == OM_STATUS_BOUNDS, "out of reach (status %u)", r.status);
    // never into a player
    bad = to;
    q->position = (vec3) {p->position.x + sinf(p->direction.y) * 25.0f, p->position.y, p->position.z + cosf(p->direction.y) * 25.0f}; // behind a
    q->velocity = (vec3) {0};
    bad.position = q->position;
    bad.position.y = q->position.y - 1.0f;
    pump(6);
    set_dev(a, 0); // a sandbox guest
    r = edit_at(a, OM_EDIT_SOURCE_BTOOLS, uid, &bad, 0);
    CHECK(r.status == OM_STATUS_BLOCKED, "a guest's box into b: refused (status %u)", r.status);
    set_dev(a, 1); // a dev moves it into b (no pace: right away), then back
    r = edit_at(a, OM_EDIT_SOURCE_BTOOLS, uid, &bad, 0);
    CHECK(r.status == OM_STATUS_OK && fabsf(game->map->objects[uid]->position.x - bad.position.x) < 0.06f, "a dev's box into b: allowed (status %u)", r.status);
    r = edit_at(a, OM_EDIT_SOURCE_BTOOLS, uid, &to, 0);
    CHECK(r.status == OM_STATUS_OK, "and back (status %u)", r.status);

    // Undo: the move, the resize, then the drop, the turn and the carry, one step per grab / drag
    const vec3 home = game->map->objects[uid]->position;
    pump(20);
    r = request(a, OM_ACTION_UNDO, 0, 0.0f);
    CHECK(r.status == OM_STATUS_OK && fabsf(game->map->objects[uid]->position.z - (home.z - 5.0f)) < 0.06f, "undo the move (z %.2f)", game->map->objects[uid]->position.z);
    pump(20);
    r = request(a, OM_ACTION_UNDO, 0, 0.0f);
    CHECK(r.status == OM_STATUS_OK && fabsf(game->map->objects[uid]->scale.y - from.size.y) < 0.01f, "undo the resize (%.2f)", game->map->objects[uid]->scale.y);
    pump(20);
    r = request(a, OM_ACTION_UNDO, 0, 0.0f);
    o = game->map->objects[uid];
    CHECK(r.status == OM_STATUS_OK && o->active && fabsf(o->position.x - spawned_pose.position.x) < 0.06f &&
          fabsf(o->position.y - spawned_pose.position.y) < 0.06f && fabsf(o->scale.x - spawned_pose.size.x) < 0.01f,
          "undo the grab: carry, turn and drop in one step (x %.2f, was %.2f)", o->position.x, spawned_pose.position.x);
    pump(20);
    r = request(a, OM_ACTION_UNDO, 0, 0.0f);
    CHECK(r.status == OM_STATUS_OK && !game->map->objects[uid]->active, "then the spawn itself (status %u)", r.status);

    // someone else's object: refused unless dev / host; a custom game's guest edits nothing
    pump(20);
    r = request_at(a, OM_ACTION_SPAWN_OBJECT, crate, -0.6f, p->direction.y);
    const int mine = (int) game->map->object_count - 1;
    bc->local = 0;
    pump(20);
    r = request(b, OM_ACTION_ADD_WEAPON, WEAPON_ID_PHYS_GUN, 0.0f);
    pump(4);
    hold(b, WEAPON_ID_PHYS_GUN);
    q->position = p->position;
    object_manager_pose(game, mine, &from);
    object_manager_pose_move(&from, 1, 10.0f, 1.0f, &to);
    pump(20);
    r = edit_at(b, OM_EDIT_SOURCE_PHYSGUN, mine, &to, OM_EDIT_BEGIN);
    CHECK(r.status == OM_STATUS_DENIED && fabsf(game->map->objects[mine]->position.y - from.position.y) < 0.01f, "b may not move a's object (status %u)", r.status);
    game->is_sandbox = 0;
    pump(20);
    r = request_at(b, OM_ACTION_SPAWN_OBJECT, crate, -0.6f, q->direction.y);
    r = edit_at(b, OM_EDIT_SOURCE_PHYSGUN, mine, &to, OM_EDIT_BEGIN);
    CHECK(r.status == OM_STATUS_DENIED, "custom guest: no edits (status %u)", r.status);
    game->is_sandbox = 1;
    bc->local = 1;
    pump(20);
    r = edit_at(a, OM_EDIT_SOURCE_PHYSGUN, mine, &to, OM_EDIT_BEGIN);
    CHECK(r.status == OM_STATUS_NO_TOOL, "a holds the Tool Gun, not the Physics Gun (status %u)", r.status);
    p->god_mode = q->god_mode = 0;
    clean_up(a);
    clean_up(b);
}

// ---------------------------------------------------------------- map edits (the map's own objects)

// The first map object of `prefab` the tools may change (and, solid_only, a solid one), -1 none
static int map_object_of(const Game *game, const int prefab, const int solid_only) {
    for (size_t i = 0; game->map && i < game->map->object_count; i++) {
        const Object *o = game->map->objects[i];
        if ((prefab < 0 || o->prefab == prefab) && object_manager_map_editable(game, (int) i) &&
            (!solid_only || (o->collision_type != COLLISION_TYPE_NONE && bot_object_blocks_player(o))) && o->scale.x < 60.0f &&
            o->scale.z < 60.0f && o->scale.y < 60.0f)
            return (int) i;
    }
    return -1;
}

static void stand_near(Player *p, const Object *o) {
    p->position = (vec3) {o->position.x + o->scale.x * 0.5f + 12.0f, o->position.y + o->scale.y + 2.0f, o->position.z};
    p->velocity = (vec3) {0};
}

static int map_ops_since(const TestClient *c, const int op, const int uid) {
    int n = 0;
    for (int i = 0; i < c->count; i++) {
        const NetPacket *k = &c->packets[i];
        if (k->type != NET_PACKET_SPAWNED_OBJ || k->length < sizeof(NetSpawnedObjPacket)) continue;
        NetSpawnedObjPacket head;
        memcpy(&head, k->payload, sizeof(head));
        n += head.op == op && head.id == uid;
    }
    return n;
}

static void mirror_apply(Game *mirror, const TestClient *c) {
    for (int i = 0; i < c->count; i++) {
        if (c->packets[i].type == NET_PACKET_SPAWNED_OBJ) object_manager_apply_spawned(mirror, c->packets[i].payload, c->packets[i].length);
    }
}

static int same_box(const Object *a, const Object *b) {
    return a && b && a->active == b->active && fabsf(a->position.x - b->position.x) < 0.01f && fabsf(a->position.y - b->position.y) < 0.01f &&
           fabsf(a->position.z - b->position.z) < 0.01f && fabsf(a->scale.x - b->scale.x) < 0.01f && fabsf(a->scale.y - b->scale.y) < 0.01f &&
           fabsf(a->scale.z - b->scale.z) < 0.01f;
}

// a: a dev (local), b: a sandbox guest (OM_CAP_SPAWN, not OM_CAP_MAP); late: joins mid-way
static void test_map_edits(TestClient *a, TestClient *b, TestClient *late) {
    printf("map edits (the map's own objects)...\n");
    Player *p = player_of(a), *q = player_of(b);
    ClientConnection *bc = conn(b);
    if (!p || !q || !bc) return;
    Game *game = &g_server->game;
    p->god_mode = q->god_mode = 1;
    pump(200);
    const ObjectManagerPerms dev = {.dev = 1}, guest = {0}, host = {.host = 1};
    CHECK((object_manager_caps(game, &dev) & OM_CAP_MAP) && (object_manager_caps(game, &host) & OM_CAP_MAP) &&
          !(object_manager_caps(game, &guest) & OM_CAP_MAP), "OM_CAP_MAP: devs and the host, not a sandbox guest");

    // b's client mirror
    Game mirror;
    memset(&mirror, 0, sizeof(mirror));
    const cJSON *maps[1] = {game->map->raw_data};
    int modes[1] = {MODE_ID_FFA};
    game_configure(&mirror, NULL, maps, 1, modes, 1, NULL, 0, NULL, 0);
    game_init(&mirror, 0, 0, 0);

    const int block = map_object_of(game, PREFAB_CUBE, 1);
    const int pad = map_object_of(game, PREFAB_BOOST_PAD, 0);
    CHECK(block >= 0 && pad >= 0, "a map block and a jump pad (%d, %d)", block, pad);
    if (block < 0 || pad < 0) return;
    Object *const original = game->map->objects[block];
    const vec3 home = original->position, home_size = original->scale;
    CHECK(object_manager_editable(game, block) && object_manager_is_map_object(game, block) && !object_manager_map_edited(game, block),
          "the block is editable, as the map has it");

    // the Physics Gun takes props, not the level: the block is refused, a crate and the pad are its (devs too)
    const int crate = map_object_of(game, PREFAB_CRATE, 0);
    for (int dev = 0; dev < 2; dev++) {
        CHECK(!object_manager_physgun_takes(game, block, dev) && object_manager_physgun_takes(game, pad, dev) &&
              (crate < 0 || object_manager_physgun_takes(game, crate, dev)), "Physics Gun reach (dev %d): no map cube, the pad and a crate (%d)", dev, crate);
    }
    request(a, OM_ACTION_ADD_WEAPON, WEAPON_ID_PHYS_GUN, 0.0f);
    pump(4);
    hold(a, WEAPON_ID_PHYS_GUN);
    stand_near(p, original);
    pump(4);
    ObjectManagerPose from, to;
    object_manager_pose(game, block, &from);
    to = from;
    to.position.y += 7.0f;
    NetSpawnResultPacket r = edit_at(a, OM_EDIT_SOURCE_PHYSGUN, block, &to, OM_EDIT_BEGIN);
    CHECK(r.status == OM_STATUS_GEOMETRY && game->map->objects[block] == original, "the Physics Gun on a map cube: refused (status %u)", r.status);
    if (crate >= 0) {
        const Object *c0 = game->map->objects[crate];
        ObjectManagerPose cf, ct;
        object_manager_pose(game, crate, &cf);
        stand_near(p, c0);
        ct = cf;
        ct.position.y += 3.0f;
        ct.yaw = (float) M_PI / 4.0f; // a 45 degree carry (OM_EDIT_YAW)
        r = edit_yaw(a, crate, &ct);
        const Object *c1 = game->map->objects[crate];
        const cJSON *rr = cJSON_GetObjectItem(c1->raw, "r");
        CHECK(r.status == OM_STATUS_OK && fabsf(c1->position.y - (cf.position.y + 3.0f)) < 0.02f && rr && fabsf((float) cJSON_GetArrayItem(rr, 1)->valuedouble - 0.79f) < 0.006f,
              "the Physics Gun carries a map crate, turned 45 degrees (status %u, r %.2f)", r.status, rr ? cJSON_GetArrayItem(rr, 1)->valuedouble : -1.0);
        request(a, OM_ACTION_UNDO, 0, 0.0f);
        CHECK(game->map->objects[crate] == c0, "undo: the map's crate back");
        stand_near(p, original);
    }
    // the Tool Gun's Move takes the block (the map's geometry stays the Tool Gun's): a stand-in at the
    // same uid, b hears MAP_EDIT, its mirror follows
    request(a, OM_ACTION_ADD_WEAPON, WEAPON_ID_TOOL_GUN, 0.0f);
    pump(4);
    hold(a, WEAPON_ID_TOOL_GUN);
    reset_received();
    r = edit_at(a, OM_EDIT_SOURCE_BTOOLS, block, &to, OM_EDIT_BEGIN);
    pump(2);
    const Object *moved = game->map->objects[block];
    CHECK(r.status == OM_STATUS_OK && moved != original && moved->map_edit && fabsf(moved->position.y - (home.y + 7.0f)) < 0.02f &&
          object_manager_map_edited(game, block) == 1, "carried up 7 (status %u, y %.2f, was %.2f)", r.status, moved->position.y, home.y);
    CHECK(map_ops_since(b, OM_SPAWNED_MAP_EDIT, block) == 1, "b hears OM_SPAWNED_MAP_EDIT");
    mirror_apply(&mirror, b);
    CHECK(same_box(mirror.map->objects[block], moved), "b's client moves the map block (same uid, same box)");
    if (cJSON_IsNumber(cJSON_GetObjectItem(original->raw, "ci"))) // the palette colour goes with it (stand-ins have no palette)
        CHECK(cJSON_IsString(cJSON_GetObjectItem(moved->raw, "c")) && cJSON_IsString(cJSON_GetObjectItem(mirror.map->objects[block]->raw, "c")),
              "the carried block keeps its palette colour (server and client)");
    const vec3 top = {moved->position.x, moved->position.y + 60.0f, moved->position.z};
    const float hit = line_in_rect(top, (vec3) {0.0f, -100.0f, 0.0f}, moved->position, moved->scale);
    CHECK(hit > 0.0f && fabsf(top.y - 100.0f * hit - (moved->position.y + moved->scale.y)) < 0.05f, "its collision box went with it");

    // a guest may not touch the map: edit, Deleter, Rotator
    bc->local = 0;
    request(b, OM_ACTION_ADD_WEAPON, WEAPON_ID_PHYS_GUN, 0.0f);
    pump(4);
    hold(b, WEAPON_ID_PHYS_GUN);
    stand_near(q, moved);
    pump(20);
    r = edit_at(b, OM_EDIT_SOURCE_PHYSGUN, block, &from, OM_EDIT_BEGIN);
    CHECK(r.status == OM_STATUS_DENIED && fabsf(game->map->objects[block]->position.y - (home.y + 7.0f)) < 0.02f, "a guest's edit of the map: refused (status %u)", r.status);

    // Resize (the Tool Gun's btools): grows, collision follows
    request(a, OM_ACTION_ADD_WEAPON, WEAPON_ID_TOOL_GUN, 0.0f);
    pump(4);
    hold(a, WEAPON_ID_TOOL_GUN);
    object_manager_pose(game, block, &from);
    object_manager_pose_resize(&from, 0, 1, 4.0f, 2.0f, 0, &to);
    pump(20);
    r = edit_at(a, OM_EDIT_SOURCE_BTOOLS, block, &to, OM_EDIT_BEGIN);
    CHECK(r.status == OM_STATUS_OK && fabsf(game->map->objects[block]->scale.x - (from.size.x + 4.0f)) < 0.02f,
          "Resize the +x face out 4 (status %u, %.2f)", r.status, game->map->objects[block]->scale.x);

    // a joiner gets the edited map
    join(late, 1);
    pump(10);
    Game joined;
    memset(&joined, 0, sizeof(joined));
    game_configure(&joined, NULL, maps, 1, modes, 1, NULL, 0, NULL, 0);
    game_init(&joined, 0, 0, 0);
    mirror_apply(&joined, late);
    CHECK(map_ops_since(late, OM_SPAWNED_MAP_EDIT, block) == 1 && same_box(joined.map->objects[block], game->map->objects[block]),
          "the joiner gets the edited block (MAP_EDIT replayed)");

    // the jump pad: carried 20 along x; the boost follows (a player on the new spot is launched)
    object_manager_pose(game, pad, &from);
    to = from;
    to.position.x += 20.0f;
    stand_near(p, game->map->objects[pad]);
    hold(a, WEAPON_ID_PHYS_GUN);
    pump(20);
    reset_received();
    r = edit_at(a, OM_EDIT_SOURCE_PHYSGUN, pad, &to, OM_EDIT_BEGIN);
    const Object *pad_now = game->map->objects[pad];
    CHECK(r.status == OM_STATUS_OK && pad_now->jump_pad && fabsf(pad_now->position.x - to.position.x) < 0.02f,
          "the jump pad moved (status %u)", r.status);
    q->position = (vec3) {pad_now->position.x, pad_now->position.y + 0.5f, pad_now->position.z};
    q->velocity = (vec3) {0};
    for (int i = 0; i < 6; i++) input(b, -1, 0);
    CHECK(q->velocity.y > 0.05f || q->position.y > pad_now->position.y + 1.5f, "the pad launches a player at its new spot (vy %.3f, y %.2f)",
          q->velocity.y, q->position.y - pad_now->position.y);

    // Deleter on the block: a placeholder, inactive; b hears MAP_DELETE
    hold(a, WEAPON_ID_TOOL_GUN);
    const Object *cur = game->map->objects[block];
    stand_near(p, cur);
    pump(20);
    float pitch, yaw;
    aim_at(p, (vec3) {cur->position.x, cur->position.y + cur->scale.y * 0.5f, cur->position.z}, &pitch, &yaw);
    reset_received();
    r = request_at(a, OM_ACTION_TOOL, OM_TOOL_DELETER, pitch, yaw);
    pump(2);
    CHECK(r.status == OM_STATUS_OK && !game->map->objects[block]->active && object_manager_map_edited(game, block) == 2, "Deleter: the map block goes (status %u)", r.status);
    CHECK(map_ops_since(b, OM_SPAWNED_MAP_DELETE, block) == 1, "b hears MAP_DELETE");
    mirror_apply(&mirror, b);
    CHECK(!mirror.map->objects[block]->active, "b's client takes it out");
    // Remove (the Delete key) never takes map objects
    r = request_at(a, OM_ACTION_REMOVE_AIM, 0, -1.55f, p->direction.y);
    CHECK(r.status != OM_STATUS_OK || object_manager_map_edited(game, block) == 2, "Remove at the crosshair leaves the map alone (status %u)", r.status);

    // Undo, newest first: the delete, the pad's carry, the resize, the carry -> the map's own Object back
    reset_received();
    pump(20);
    r = request(a, OM_ACTION_UNDO, 0, 0.0f);
    CHECK(r.status == OM_STATUS_OK && game->map->objects[block]->active && object_manager_map_edited(game, block) == 1, "undo the delete");
    pump(20);
    r = request(a, OM_ACTION_UNDO, 0, 0.0f);
    CHECK(r.status == OM_STATUS_OK && fabsf(game->map->objects[pad]->position.x - from.position.x) < 0.02f && !object_manager_map_edited(game, pad),
          "undo the pad's move: the map's pad back (x %.2f, was %.2f)", game->map->objects[pad]->position.x, from.position.x);
    pump(20);
    r = request(a, OM_ACTION_UNDO, 0, 0.0f);
    CHECK(r.status == OM_STATUS_OK && fabsf(game->map->objects[block]->scale.x - home_size.x) < 0.02f, "undo the resize");
    pump(20);
    r = request(a, OM_ACTION_UNDO, 0, 0.0f);
    CHECK(r.status == OM_STATUS_OK && game->map->objects[block] == original && original->position.y == home.y && !object_manager_map_edited(game, block),
          "undo the carry: the map's own object is back (status %u)", r.status);
    pump(2);
    CHECK(map_ops_since(b, OM_SPAWNED_MAP_RESTORE, block) == 1, "b hears MAP_RESTORE");
    mirror_apply(&mirror, b);
    CHECK(same_box(mirror.map->objects[block], original) && !mirror.map->objects[block]->map_edit, "b's client has the map's block back");

    // the Rotator on a map object (in place, an undo step), then the round start puts it back
    hold(a, WEAPON_ID_TOOL_GUN);
    stand_near(p, original);
    pump(20);
    aim_at(p, (vec3) {original->position.x, original->position.y + original->scale.y * 0.5f, original->position.z}, &pitch, &yaw);
    r = request_at(a, OM_ACTION_TOOL, OM_TOOL_ROTATOR, pitch, yaw);
    const Object *turned = game->map->objects[block];
    CHECK(r.status == OM_STATUS_OK && turned != original && fabsf(turned->scale.x - home_size.z) < 0.02f && fabsf(turned->scale.z - home_size.x) < 0.02f,
          "Rotator: a quarter turn of the map block (status %u)", r.status);
    // Duplicator: a map object copied is the dev's own spawn
    pump(20);
    r = request_at(a, OM_ACTION_TOOL, OM_TOOL_DUPLICATOR, pitch, yaw);
    CHECK(r.status == OM_STATUS_OK && r.limit == OM_TOOL_COPIED, "Duplicator copies a map object (status %u)", r.status);
    pump(20);
    r = request_at(a, OM_ACTION_TOOL, OM_TOOL_DUPLICATOR, -0.5f, p->direction.y + 1.2f);
    const Object *copy = last_spawned();
    CHECK(r.status == OM_STATUS_OK && r.limit == OM_TOOL_PASTED && copy && copy->spawned && copy->prefab == PREFAB_CUBE, "and places it as a spawn");
    world_round_start(game);
    CHECK(game->map->objects[block] == original && !object_manager_map_edited(game, block) && !object_manager_map_edited(game, pad),
          "round start: the map as it was");

    bc->local = 1;
    p->god_mode = q->god_mode = 0;
    leave(late);
    clean_up(a);
    clean_up(b);
    object_manager_restore_map(&mirror); // (the mirrors' maps are freed: their own Objects back first)
    object_manager_restore_map(&joined);
}

// ---------------------------------------------------------------- the Properties tool (field patches)

// The editor's own read of a field (editor_model.c ed_model_prop_get) equals the game's
static int same_value(const cJSON *a, const cJSON *b) {
    if (!a || !b) return 0;
    if (cJSON_IsNumber(a) && cJSON_IsNumber(b)) return fabs(a->valuedouble - b->valuedouble) < 1e-6;
    if (cJSON_IsBool(a) && cJSON_IsBool(b)) return cJSON_IsTrue(a) == cJSON_IsTrue(b);
    if (cJSON_IsString(a) && cJSON_IsString(b)) return !strcasecmp(a->valuestring, b->valuestring);
    return cJSON_Compare(a, b, 1);
}

static int first_prefab_of(const ObjectManagerField *f) {
    for (int prefab = 0; prefab < PREFAB_V7_COUNT; prefab++) {
        if (object_manager_field_applies(f, prefab)) return prefab;
    }
    return -1;
}

static void test_fields_table(void) {
    printf("the Properties tool's fields against the editor...\n");
    // pos / rot / size / texture / collidable / health: editor_objects.c's special props (not in K_PROPS)
    static const char *const special[] = {"pos", "rot", "size", "texture", "collidable", "health", NULL};
    const int n = object_manager_field_count();
    CHECK(n >= 30, "fields (%d)", n);
    for (int i = 0; i < n; i++) {
        const ObjectManagerField *f = object_manager_field(i);
        int skip = 0;
        for (int k = 0; special[k]; k++) skip |= !strcmp(special[k], f->prop);
        const int prefab = first_prefab_of(f);
        CHECK(prefab >= 0, "field %s applies to some prefab", f->key);
        CHECK(object_manager_field_find(f->key) == i, "field %s found by key", f->key);
        if (skip || prefab < 0) continue;
        const int action = !strncmp(f->key, "tas.0.", 6);
        const char *key = action ? f->key + 6 : f->key;
        const char *ed_key = ed_model_prop_key(f->prop);
        CHECK(ed_key && !strcmp(ed_key, key), "%s: the editor's key of '%s' is '%s'", f->key, f->prop, ed_key ? ed_key : "(none)");
        // "bounce": the runtime's read (map.c: bm == null ? 1 : parseFloat(bm || 0)), K_PROPS has an OR-default
        const int runtime_read = !strcmp(f->prop, "bounce");
        // the read rule: absent, 0 and a value in range (written by the game, read back by the editor)
        float mn, mx;
        object_manager_field_range(f, prefab, &mn, &mx);
        const double mid = f->kind == OM_FIELD_COLOR ? 0.0 : (f->kind == OM_FIELD_INT || f->kind == OM_FIELD_SELECT) ? floor((mn + mx) * 0.5 + 0.7) : mn + (mx - mn) * 0.37;
        for (int pass = 0; pass < 3; pass++) {
            cJSON *rec = cJSON_CreateObject();
            cJSON_AddNumberToObject(rec, "i", prefab);
            cJSON *holder = rec;
            if (action) {
                cJSON *tas = cJSON_AddArrayToObject(rec, "tas");
                holder = cJSON_CreateObject();
                cJSON_AddItemToArray(tas, holder);
            }
            if (pass == 1) cJSON_AddNumberToObject(holder, key, 0);
            if (pass == 2) { // through the game's write
                cJSON *patch = cJSON_CreateObject();
                if (f->kind == OM_FIELD_ON || f->kind == OM_FIELD_OFF) cJSON_AddBoolToObject(patch, f->key, 0);
                else if (f->kind == OM_FIELD_COLOR) cJSON_AddStringToObject(patch, f->key, "#12ab34");
                else cJSON_AddNumberToObject(patch, f->key, mid);
                const int status = object_manager_fields_patch(rec, prefab, patch, NULL);
                CHECK(status == OM_STATUS_OK, "%s: the patch writes (status %d)", f->key, status);
                cJSON_Delete(patch);
            }
            cJSON *ours = object_manager_field_value(rec, prefab, f);
            cJSON *theirs = ed_model_prop_get(rec, prefab, f->prop);
            if (!(runtime_read && pass == 1))
                CHECK(same_value(ours, theirs), "%s (%s) pass %d: the game reads %s, the editor %s", f->key, f->prop, pass,
                      ours ? cJSON_PrintUnformatted(ours) : "-", theirs ? cJSON_PrintUnformatted(theirs) : "-");
            else CHECK(cJSON_IsNumber(ours) && ours->valuedouble == 0.0, "bm 0 reads as 0 (the runtime's rule)");
            cJSON_Delete(ours);
            cJSON_Delete(theirs);
            cJSON_Delete(rec);
        }
    }
    // the gates follow the prefabs: a jump pad's force, a teleporter's channel, a light's intensity
    CHECK(object_manager_field_applies(object_manager_field(object_manager_field_find("bm")), PREFAB_BOOST_PAD), "BOOST_PAD: Bounce");
    CHECK(!object_manager_field_applies(object_manager_field(object_manager_field_find("bm")), PREFAB_CUBE), "CUBE: no Bounce");
    CHECK(object_manager_field_applies(object_manager_field(object_manager_field_find("ch")), PREFAB_TELEPORTER), "TELEPORTER: Channel");
    CHECK(object_manager_field_applies(object_manager_field(object_manager_field_find("li")), PREFAB_POINT_LIGHT), "POINT_LIGHT: Intensity");
    CHECK(object_manager_field_applies(object_manager_field(object_manager_field_find("c")), PREFAB_POINT_LIGHT), "POINT_LIGHT: Color");
    CHECK(object_manager_field_applies(object_manager_field(object_manager_field_find("t")), PREFAB_CUBE) &&
          object_manager_field_applies(object_manager_field(object_manager_field_find("c")), PREFAB_CUBE), "CUBE: Texture, Color");
    CHECK(!strcmp(object_manager_field_option(object_manager_field(object_manager_field_find("n")), 2), "Both"), "Node options");
    CHECK(!strcmp(object_manager_field_option(object_manager_field(object_manager_field_find("te")), 3), "onEnterObject"), "Event options");
    // patches the authority refuses before anything moves
    cJSON *rec = cJSON_Parse("{\"i\":0,\"p\":[0,0,0],\"s\":[10,10,10]}");
    static const char *const bad[][2] = {
        {"{\"nope\":1}", "an unknown key"}, {"{\"bm\":1}", "a field of another prefab"}, {"{\"c\":\"red\"}", "a colour not #rrggbb"},
        {"{\"o\":3}", "out of range"}, {"{\"s\":[10,500,10]}", "a size past the limit"}, {"{\"t\":1.5}", "a fractional texture"},
        {"{\"p\":[0,0]}", "a short vector"}, {"{\"v\":\"yes\"}", "a switch as a string"}, {"[]", "not an object"},
    };
    static const int want[] = {OM_STATUS_INVALID, OM_STATUS_UNSUPPORTED, OM_STATUS_INVALID, OM_STATUS_BOUNDS, OM_STATUS_BOUNDS,
                               OM_STATUS_INVALID, OM_STATUS_INVALID, OM_STATUS_INVALID, OM_STATUS_INVALID};
    for (size_t i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        cJSON *patch = cJSON_Parse(bad[i][0]);
        const int status = object_manager_fields_patch(rec, PREFAB_CUBE, patch, NULL);
        CHECK(status == want[i], "refused: %s (status %d)", bad[i][1], status);
        cJSON_Delete(patch);
    }
    cJSON *patch = cJSON_Parse("{\"t\":4,\"c\":\"#FF0000\",\"v\":false,\"p\":[1,2,3]}");
    int pose = 0;
    CHECK(object_manager_fields_patch(rec, PREFAB_CUBE, patch, &pose) == OM_STATUS_OK && pose, "a cube's texture, colour, visibility and spot");
    char *printed = cJSON_PrintUnformatted(rec);
    CHECK(printed && strstr(printed, "\"t\":4") && strstr(printed, "\"c\":\"#ff0000\"") && strstr(printed, "\"v\":1") && strstr(printed, "\"p\":[1,2,3]"),
          "written as the editor writes them: %s", printed ? printed : "");
    free(printed);
    cJSON_Delete(patch);
    // a legacy trigger: its action keys move into tas[0] with the first action edit, the Event Value stays
    cJSON *legacy = cJSON_Parse("{\"i\":29,\"ta\":3,\"tc\":5,\"tce\":2}");
    patch = cJSON_Parse("{\"tas.0.tc\":9}");
    CHECK(object_manager_fields_patch(legacy, PREFAB_TRIGGER, patch, NULL) == OM_STATUS_OK, "a legacy trigger's action value");
    printed = cJSON_PrintUnformatted(legacy);
    CHECK(printed && !strcmp(printed, "{\"i\":29,\"tce\":2,\"tas\":[{\"ta\":3,\"tc\":9}]}"), "legacy action keys in tas[0]: %s", printed ? printed : "");
    free(printed);
    cJSON_Delete(patch);
    cJSON_Delete(legacy);
    cJSON_Delete(rec);
}

// One field patch (the Properties tool: OM_ACTION_EDIT + OM_EDIT_FIELDS + the JSON); its SPAWN_RESULT
static NetSpawnResultPacket fields_at(TestClient *c, const int uid, const char *json) {
    const Player *p = player_of(c);
    uint8_t buffer[sizeof(NetSpawnEditPacket) + OM_FIELDS_MAX + 1];
    NetSpawnEditPacket e;
    memset(&e, 0, sizeof(e));
    e.req = (NetSpawnReqPacket) {.action = OM_ACTION_EDIT, .item = OM_EDIT_SOURCE_BTOOLS, .pitch = p ? p->direction.x : 0.0f, .yaw = p ? p->direction.y : 0.0f};
    e.uid = uid;
    e.flags = OM_EDIT_FIELDS;
    const size_t n = strlen(json) + 1;
    memcpy(buffer, &e, sizeof(e));
    memcpy(buffer + sizeof(e), json, n);
    c->count = 0;
    pump(20); // (the edit pace)
    send_to(c, NET_PACKET_SPAWN_REQ, buffer, (uint16_t) (sizeof(e) + n));
    pump(2);
    NetSpawnResultPacket r = {.status = OM_STATUS_COUNT};
    const NetPacket *packet = last(c, NET_PACKET_SPAWN_RESULT);
    if (packet && packet->length == sizeof(r)) memcpy(&r, packet->payload, sizeof(r));
    return r;
}

// The last MAP_EDIT overlay `c` got for `uid` (caller frees), NULL none
static cJSON *map_edit_overlay(const TestClient *c, const int uid) {
    cJSON *found = NULL;
    for (int i = 0; i < c->count; i++) {
        const NetPacket *k = &c->packets[i];
        if (k->type != NET_PACKET_SPAWNED_OBJ || k->length <= sizeof(NetSpawnedObjPacket)) continue;
        NetSpawnedObjPacket head;
        memcpy(&head, k->payload, sizeof(head));
        if (head.op != OM_SPAWNED_MAP_EDIT || head.id != uid) continue;
        cJSON_Delete(found);
        found = cJSON_Parse((const char *) k->payload + sizeof(head));
    }
    return found;
}

static int map_uid_of(const Game *game, const int prefab) {
    for (size_t i = 0; game->map && i < game->map->object_count; i++) {
        if (game->map->objects[i]->prefab == prefab && object_manager_map_editable(game, (int) i)) return (int) i;
    }
    return -1;
}

// a: a dev (local), b: a sandbox guest (OM_CAP_SPAWN, not OM_CAP_MAP); late: joins mid-way. The map
// is Kanji with a trigger appended (start_with_trigger)
static void test_field_edits(TestClient *a, TestClient *b, TestClient *late) {
    printf("field edits (the Properties tool)...\n");
    Player *p = player_of(a), *q = player_of(b);
    ClientConnection *bc = conn(b);
    if (!p || !q || !bc) return;
    Game *game = &g_server->game;
    p->god_mode = q->god_mode = 1;
    pump(200);
    Game mirror;
    memset(&mirror, 0, sizeof(mirror));
    const cJSON *maps[1] = {game->map->raw_data};
    int modes[1] = {MODE_ID_FFA};
    game_configure(&mirror, NULL, maps, 1, modes, 1, NULL, 0, NULL, 0);
    game_init(&mirror, 0, 0, 0);

    const int pad = map_object_of(game, PREFAB_BOOST_PAD, 0), block = map_object_of(game, PREFAB_CUBE, 1), trig = map_uid_of(game, PREFAB_TRIGGER);
    CHECK(pad >= 0 && block >= 0 && trig >= 0, "a jump pad, a block and the trigger (%d, %d, %d)", pad, block, trig);
    if (pad < 0 || block < 0 || trig < 0) return;
    Object *const pad0 = game->map->objects[pad], *const block0 = game->map->objects[block], *const trig0 = game->map->objects[trig];
    request(a, OM_ACTION_ADD_WEAPON, WEAPON_ID_TOOL_GUN, 0.0f);
    pump(4);
    hold(a, WEAPON_ID_TOOL_GUN);

    // the jump pad's force (Bounce, "bm"): applied, heard, mirrored
    stand_near(p, pad0);
    const float bounce0 = pad0->bounce;
    reset_received();
    NetSpawnResultPacket r = fields_at(a, pad, "{\"bm\":2.5}");
    pump(2);
    const Object *pad1 = game->map->objects[pad];
    CHECK(r.status == OM_STATUS_OK && r.action == OM_ACTION_EDIT && r.limit == OM_FIELDS_ANSWER && pad1 != pad0 && pad1->jump_pad &&
          fabsf(pad1->bounce - 2.5f) < 1e-4f && object_manager_map_edited(game, pad) == 1,
          "the pad's Bounce 2.5 (status %u, bounce %.2f, was %.2f)", r.status, pad1->bounce, bounce0);
    cJSON *overlay = map_edit_overlay(b, pad);
    CHECK(overlay && cJSON_GetNumberValue(cJSON_GetObjectItem(overlay, "bm")) == 2.5 && cJSON_GetObjectItem(overlay, "p"),
          "b hears MAP_EDIT with \"bm\" (and the pose keys)");
    CHECK(overlay && !cJSON_GetObjectItem(overlay, "ci") && !cJSON_IsNull(cJSON_GetObjectItem(overlay, "c")) && !cJSON_GetObjectItem(overlay, "r"),
          "the overlay keeps the palette colour and names no absent pose key");
    if (cJSON_IsNumber(cJSON_GetObjectItem(pad0->raw, "ci")))
        CHECK(cJSON_IsString(cJSON_GetObjectItem(pad1->raw, "c")) && !cJSON_GetObjectItem(pad1->raw, "ci"), "the stand-in has the palette's colour in \"c\"");
    cJSON_Delete(overlay);
    mirror_apply(&mirror, b);
    CHECK(mirror.map->objects[pad]->map_edit && fabsf(mirror.map->objects[pad]->bounce - 2.5f) < 1e-4f, "b's client: the pad bounces 2.5");
    q->position = (vec3) {pad1->position.x, pad1->position.y + 0.5f, pad1->position.z};
    q->velocity = (vec3) {0};
    for (int i = 0; i < 6; i++) input(b, -1, 0);
    CHECK(q->velocity.y > 0.05f || q->position.y > pad1->position.y + 1.5f, "the stronger pad still launches (vy %.3f)", q->velocity.y);
    r = fields_at(a, pad, "{\"bm\":9}");
    CHECK(r.status == OM_STATUS_BOUNDS && fabsf(game->map->objects[pad]->bounce - 2.5f) < 1e-4f, "Bounce 9: past the editor's -3..3 (status %u)", r.status);
    r = fields_at(a, pad, "{\"ch\":3}");
    CHECK(r.status == OM_STATUS_UNSUPPORTED, "a teleporter's Channel on a pad: not its field (status %u)", r.status);
    reset_received();
    r = fields_at(a, pad, "{\"bm\":2.5}");
    CHECK(r.status == OM_STATUS_OK && map_ops_since(b, OM_SPAWNED_MAP_EDIT, pad) == 0, "the same value again: nothing sent (status %u)", r.status);

    // a guest: refused (no OM_CAP_MAP), nothing changes
    bc->local = 0;
    request(b, OM_ACTION_ADD_WEAPON, WEAPON_ID_TOOL_GUN, 0.0f);
    pump(4);
    hold(b, WEAPON_ID_TOOL_GUN);
    stand_near(q, pad1);
    r = fields_at(b, pad, "{\"bm\":-1}");
    CHECK(r.status == OM_STATUS_DENIED && fabsf(game->map->objects[pad]->bounce - 2.5f) < 1e-4f, "a guest's Bounce: refused (status %u)", r.status);

    // a block's texture and colour, then a move: the colour stays with it
    stand_near(p, block0);
    reset_received();
    r = fields_at(a, block, "{\"t\":5,\"c\":\"#FF2200\"}");
    const Object *block1 = game->map->objects[block];
    const cJSON *rt = block1->raw ? cJSON_GetObjectItem(block1->raw, "t") : NULL, *rc = block1->raw ? cJSON_GetObjectItem(block1->raw, "c") : NULL;
    CHECK(r.status == OM_STATUS_OK && block1 != block0 && cJSON_GetNumberValue(rt) == 5 && rc && !strcmp(cJSON_GetStringValue(rc), "#ff2200") &&
          !cJSON_GetObjectItem(block1->raw, "ci"), "the block's texture 5 and colour #ff2200 (status %u)", r.status);
    CHECK(same_box(block1, block0), "its box stays");
    r = fields_at(b, block, "{\"c\":\"#00ff00\"}");
    CHECK(r.status == OM_STATUS_DENIED, "a guest's colour: refused (status %u)", r.status);
    ObjectManagerPose from, to;
    object_manager_pose(game, block, &from);
    to = from;
    to.position.y += 3.0f;
    pump(20);
    r = edit_at(a, OM_EDIT_SOURCE_BTOOLS, block, &to, OM_EDIT_BEGIN);
    const Object *block2 = game->map->objects[block];
    CHECK(r.status == OM_STATUS_OK && fabsf(block2->position.y - to.position.y) < 0.02f && cJSON_GetObjectItem(block2->raw, "c") &&
          !strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(block2->raw, "c")), "#ff2200"), "moved up 3, still #ff2200");
    mirror_apply(&mirror, b);
    const Object *mb = mirror.map->objects[block];
    CHECK(same_box(mb, block2) && mb->raw && cJSON_GetNumberValue(cJSON_GetObjectItem(mb->raw, "t")) == 5 &&
          !strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(mb->raw, "c")), "#ff2200"), "b's client: the moved block keeps its texture and colour");

    // a trigger setting: the chance, single use and the first action's value
    stand_near(p, trig0);
    r = fields_at(a, trig, "{\"cha\":40,\"su\":true,\"tas.0.tc\":7}");
    const Object *trig1 = game->map->objects[trig];
    CHECK(r.status == OM_STATUS_OK && trig1->trig && trig1->trig->chance == 40 && trig1->trig->single_use && trig1->trig->action_count == 1 &&
          trig1->trig->actions[0].constant == 7 && trig1->trig->actions[0].action == 1, "the trigger: chance 40, single use, action value 7 (status %u)", r.status);
    r = fields_at(b, trig, "{\"cha\":0}");
    CHECK(r.status == OM_STATUS_DENIED && game->map->objects[trig]->trig->chance == 40, "a guest's trigger edit: refused (status %u)", r.status);
    r = fields_at(a, trig, "{\"tas.0.ta\":200}");
    CHECK(r.status == OM_STATUS_BOUNDS, "an action past the table (status %u)", r.status);

    // a joiner gets every edited object replayed
    join(late, 1);
    pump(10);
    Game joined;
    memset(&joined, 0, sizeof(joined));
    game_configure(&joined, NULL, maps, 1, modes, 1, NULL, 0, NULL, 0);
    game_init(&joined, 0, 0, 0);
    mirror_apply(&joined, late);
    CHECK(fabsf(joined.map->objects[pad]->bounce - 2.5f) < 1e-4f && joined.map->objects[trig]->trig && joined.map->objects[trig]->trig->chance == 40 &&
          !strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(joined.map->objects[block]->raw, "c")), "#ff2200"),
          "the joiner gets the pad's Bounce, the block's colour and the trigger's chance");

    // Undo, newest first: the trigger, the move, the colour, the Bounce -> the map's own objects
    reset_received();
    pump(20);
    r = request(a, OM_ACTION_UNDO, 0, 0.0f);
    CHECK(r.status == OM_STATUS_OK && game->map->objects[trig] == trig0 && trig0->trig->chance == 0, "undo the trigger edit: the map's trigger back");
    pump(20);
    r = request(a, OM_ACTION_UNDO, 0, 0.0f);
    const Object *block3 = game->map->objects[block];
    CHECK(r.status == OM_STATUS_OK && same_box(block3, block0) && cJSON_GetObjectItem(block3->raw, "c") &&
          !strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(block3->raw, "c")), "#ff2200"), "undo the move: back down, still red");
    pump(20);
    r = request(a, OM_ACTION_UNDO, 0, 0.0f);
    CHECK(r.status == OM_STATUS_OK && game->map->objects[block] == block0, "undo the colour: the map's block back");
    pump(20);
    r = request(a, OM_ACTION_UNDO, 0, 0.0f);
    CHECK(r.status == OM_STATUS_OK && game->map->objects[pad] == pad0 && fabsf(pad0->bounce - bounce0) < 1e-4f, "undo the Bounce: the map's pad back");
    pump(2);
    mirror_apply(&mirror, b);
    CHECK(!mirror.map->objects[pad]->map_edit && fabsf(mirror.map->objects[pad]->bounce - bounce0) < 1e-4f && !mirror.map->objects[block]->map_edit &&
          !mirror.map->objects[trig]->map_edit, "b's client has the map's own objects back");

    // a spawned object: its owner's colour edit (OM_SPAWNED_UPDATE), undone
    bc->local = 1;
    stand_near(p, block0);
    pump(20);
    r = request_at(a, OM_ACTION_SPAWN_OBJECT, entry_index(OM_TAB_OBJECTS, "Crate"), -0.5f, p->direction.y + 1.2f);
    const Object *crate = last_spawned();
    const int crate_uid = crate ? (int) (game->map->object_count - 1) : -1;
    CHECK(r.status == OM_STATUS_OK && crate && crate->prefab == PREFAB_CRATE, "a crate spawned (status %u)", r.status);
    if (crate) {
        reset_received();
        r = fields_at(a, crate_uid, "{\"v\":false}");
        CHECK(r.status == OM_STATUS_OK && cJSON_GetObjectItem(game->map->objects[crate_uid]->raw, "v"), "the crate hidden (v: 1) (status %u)", r.status);
        CHECK(update_p(b, crate_uid, &to.position), "b hears OM_SPAWNED_UPDATE");
        pump(20);
        r = request(a, OM_ACTION_UNDO, 0, 0.0f);
        CHECK(r.status == OM_STATUS_OK && !cJSON_GetObjectItem(game->map->objects[crate_uid]->raw, "v"), "undo: visible again");
        request(a, OM_ACTION_UNDO, 0, 0.0f); // the crate
    }
    // the round start restores what Undo did not
    pump(20);
    r = fields_at(a, block, "{\"o\":0.5}");
    CHECK(r.status == OM_STATUS_OK && game->map->objects[block] != block0, "the block's opacity 0.5");
    world_round_start(game);
    CHECK(game->map->objects[block] == block0 && !object_manager_map_edited(game, block), "round start: the map's block back");

    p->god_mode = q->god_mode = 0;
    leave(late);
    clean_up(a);
    clean_up(b);
    object_manager_restore_map(&mirror);
    object_manager_restore_map(&joined);
}

static void test_join_sync(TestClient *a, TestClient *late, TestClient *old) {
    printf("join sync...\n");
    pump(20);
    request(a, OM_ACTION_ADD_WEAPON, 5, 0.0f);
    pump(20);
    request(a, OM_ACTION_DROP_WEAPON, 1, -1.4f); // something to sync (the earlier ones were undone)
    pump(20);
    request(a, OM_ACTION_DROP_WEAPON, 2, -1.4f);
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
    r = request(guest, OM_ACTION_SPAWN_NPC, 1, -1.0f);
    CHECK(r.status == OM_STATUS_DENIED, "public guest: no NPCs");
    r = request(guest, OM_ACTION_SPAWN_ENTITY, 0, -1.0f);
    CHECK(r.status == OM_STATUS_DENIED, "public guest: no entities");
    r = request(guest, OM_ACTION_SPAWN_OBJECT, 0, -1.0f);
    CHECK(r.status == OM_STATUS_DENIED, "public guest: no objects");
    r = request(guest, OM_ACTION_REMOVE_AIM, 0, -1.0f);
    CHECK(r.status == OM_STATUS_DENIED, "public guest: no remove tool");
    r = request(dev, OM_ACTION_SPAWN_OBJECT, 0, -1.0f);
    CHECK(r.status == OM_STATUS_OK, "public: the dev places an object (status %u)", r.status);
    pump(20);
    request(dev, OM_ACTION_UNDO, 0, 0.0f);
    pump(20);
    r = request(dev, OM_ACTION_SPAWN_NPC, 1, -1.0f);
    CHECK(r.status == OM_STATUS_OK, "public: the dev spawns an NPC (status %u)", r.status);
    pump(20);
    request(dev, OM_ACTION_UNDO, 0, 0.0f);
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

// ---------------------------------------------------------------- dev privileges (KrunkNative)

// A dev has no caps: crates past OM_OBJECTS_PER_PLAYER back to back, and edits / carries of an
// object a player stands in
static void test_dev_privileges(TestClient *a, TestClient *b) {
    printf("dev privileges...\n");
    Player *p = player_of(a), *q = player_of(b);
    if (!p || !q) return;
    clean_up(a);
    Game *game = &g_server->game;
    const int crate = 10; // the Crate tile (object_manager_entries(OM_TAB_OBJECTS))
    const int before = spawned_count();
    int ok = 0;
    for (int i = 0; i < OM_OBJECTS_PER_PLAYER + 6; i++) ok += request(a, OM_ACTION_SPAWN_OBJECT, crate, -0.45f).status == OM_STATUS_OK; // no pump: no pace
    CHECK(ok == OM_OBJECTS_PER_PLAYER + 6 && spawned_count() == before + ok, "a dev spawns %d crates back to back, past the cap of %d (%d live)", ok,
          OM_OBJECTS_PER_PLAYER, spawned_count() - before);
    set_dev(a, 0);
    const NetSpawnResultPacket capped = request(a, OM_ACTION_SPAWN_OBJECT, crate, -0.45f);
    CHECK(capped.status == OM_STATUS_LIMIT || capped.status == OM_STATUS_RATE, "the same as a sandbox guest: capped (status %u)", capped.status);
    set_dev(a, 1);
    clean_up(a);
    // carrying a crate b stands in (a dev may; a guest's edit into b is refused: test_edits)
    request(a, OM_ACTION_ADD_WEAPON, WEAPON_ID_PHYS_GUN, 0.0f);
    hold(a, WEAPON_ID_PHYS_GUN);
    NetSpawnResultPacket r = request(a, OM_ACTION_SPAWN_OBJECT, crate, -0.6f);
    const int uid = (int) game->map->object_count - 1;
    ObjectManagerPose from, to;
    CHECK(r.status == OM_STATUS_OK && object_manager_pose(game, uid, &from), "a crate to carry (status %u)", r.status);
    q->position = (vec3) {from.position.x, from.position.y, from.position.z};
    q->velocity = (vec3) {0};
    to = from;
    to.position.y += 1.0f; // b is inside it before and after
    r = edit_at(a, OM_EDIT_SOURCE_PHYSGUN, uid, &to, OM_EDIT_BEGIN);
    CHECK(r.status == OM_STATUS_OK, "carried while b stands in it (status %u)", r.status);
    to.position.x += 3.0f;
    r = edit_at(a, OM_EDIT_SOURCE_PHYSGUN, uid, &to, 0); // right after: devs are not paced
    CHECK(r.status == OM_STATUS_OK && fabsf(game->map->objects[uid]->position.x - to.position.x) < 0.06f, "and on, unpaced (status %u)", r.status);
    clean_up(a);
}

// R185: a non-dev's Physics Gun carries props only, its own spawned blocks included; a dev's carries
// every spawned object (and players: test_physgun_player)
static void test_physgun_props(TestClient *a) {
    printf("Physics Gun: props only for non-devs...\n");
    Player *p = player_of(a);
    if (!p) return;
    Game *game = &g_server->game;
    p->god_mode = 1;
    clean_up(a);
    pump(200);
    request(a, OM_ACTION_ADD_WEAPON, WEAPON_ID_PHYS_GUN, 0.0f);
    pump(4);
    hold(a, WEAPON_ID_PHYS_GUN);
    NetSpawnResultPacket r = request_at(a, OM_ACTION_SPAWN_OBJECT, entry_index(OM_TAB_OBJECTS, "Stone Block"), -0.6f, p->direction.y);
    const int block = (int) game->map->object_count - 1;
    pump(20);
    NetSpawnResultPacket r2 = request_at(a, OM_ACTION_SPAWN_OBJECT, entry_index(OM_TAB_OBJECTS, "Crate"), -0.6f, p->direction.y + 0.8f);
    const int crate = (int) game->map->object_count - 1;
    CHECK(r.status == OM_STATUS_OK && r2.status == OM_STATUS_OK && block != crate, "a spawned block and crate (status %u %u)", r.status, r2.status);
    CHECK(!object_manager_physgun_takes(game, block, 0) && object_manager_physgun_takes(game, crate, 0), "non-dev reach: the crate, not the block");
    CHECK(object_manager_physgun_takes(game, block, 1) && object_manager_physgun_takes(game, crate, 1), "dev reach: both");
    ObjectManagerPose from, to;
    object_manager_pose(game, block, &from);
    to = from;
    to.position.y += 4.0f;
    set_dev(a, 0);
    pump(20);
    r = edit_at(a, OM_EDIT_SOURCE_PHYSGUN, block, &to, OM_EDIT_BEGIN);
    CHECK(r.status == OM_STATUS_GEOMETRY && fabsf(game->map->objects[block]->position.y - from.position.y) < 0.01f,
          "a non-dev's Physics Gun on its own spawned block: refused (status %u)", r.status);
    ObjectManagerPose cfrom, cto;
    object_manager_pose(game, crate, &cfrom);
    cto = cfrom;
    cto.position.y += 4.0f;
    pump(20);
    r = edit_at(a, OM_EDIT_SOURCE_PHYSGUN, crate, &cto, OM_EDIT_BEGIN);
    CHECK(r.status == OM_STATUS_OK && fabsf(game->map->objects[crate]->position.y - cto.position.y) < 0.06f, "its crate: carried (status %u)", r.status);
    set_dev(a, 1);
    pump(20);
    r = edit_at(a, OM_EDIT_SOURCE_PHYSGUN, block, &to, OM_EDIT_BEGIN);
    CHECK(r.status == OM_STATUS_OK && fabsf(game->map->objects[block]->position.y - to.position.y) < 0.06f, "a dev's: the block too (status %u)", r.status);
    p->god_mode = 0;
    clean_up(a);
}

// Whether the box of object `uid` overlaps a solid one (any solid box, the walls and floors too)
static int box_in_solid(const Game *game, const int uid) {
    const Object *a = game->map->objects[uid];
    const vec3 sa = object_box_size(a);
    for (size_t i = 0; i < game->map->object_count; i++) {
        const Object *b = game->map->objects[i];
        if ((int) i == uid || b->complex_coll || !bot_object_blocks_player(b)) continue;
        const vec3 sb = object_box_size(b);
        const float eps = 0.02f;
        if (fabsf(a->position.x - b->position.x) < (sa.x + sb.x) * 0.5f - eps && fabsf(a->position.z - b->position.z) < (sa.z + sb.z) * 0.5f - eps &&
            a->position.y < b->position.y + sb.y - eps && b->position.y < a->position.y + sa.y - eps)
            return 1;
    }
    return 0;
}

// The near face (min x) of the solid box the crate meets first going +x, -1 none
static float wall_face_ahead(const Game *game, const Object *crate) {
    float best = -1.0f;
    for (size_t i = 0; i < game->map->object_count; i++) {
        const Object *o = game->map->objects[i];
        if (o == crate || o->complex_coll || !bot_object_blocks_player(o)) continue;
        const vec3 s = object_box_size(o);
        if (s.x < 10.0f || s.y < 20.0f || s.z < 20.0f) continue; // a wall, not a crate
        const float face = o->position.x - s.x * 0.5f;
        if (face <= crate->position.x + crate->scale.x * 0.5f) continue;
        if (fabsf(o->position.z - crate->position.z) > s.z * 0.5f) continue;
        if (o->position.y > crate->position.y + crate->scale.y || o->position.y + s.y < crate->position.y) continue;
        if (best < 0.0f || face < best) best = face;
    }
    return best;
}

// The Physics Gun cycle, again and again, in a round of its own: the crosshair picks a crate, the grab
// carries it (into the wall, then back), Drop lets it go, and the crate is solid, out of every box, and
// picked again. Pushing it through the wall stops it at the wall's near face.
static void test_physgun_cycles(TestClient *a) {
    printf("Physics Gun: carry into a wall and back, drop, pick again, several cycles...\n");
    Player *p = player_of(a);
    if (!p) return;
    Game *game = &g_server->game;
    p->god_mode = 1;
    clean_up(a);
    pump(200);
    request(a, OM_ACTION_ADD_WEAPON, WEAPON_ID_PHYS_GUN, 0.0f);
    pump(4);
    hold(a, WEAPON_ID_PHYS_GUN);
    set_dev(a, 0);
    NetSpawnResultPacket r = request_at(a, OM_ACTION_SPAWN_OBJECT, entry_index(OM_TAB_OBJECTS, "Crate"), -0.6f, p->direction.y);
    const int uid = (int) game->map->object_count - 1;
    pump(20);
    CHECK(r.status == OM_STATUS_OK && game->map->objects[uid]->active, "a crate to carry (status %u)", r.status);
    if (r.status != OM_STATUS_OK) return;
    const float ahead = wall_face_ahead(game, game->map->objects[uid]);
    CHECK(ahead > game->map->objects[uid]->position.x, "a wall ahead of the crate (near face %.2f)", ahead);
    if (ahead <= 0.0f) return;
    const float stop = ahead - game->map->objects[uid]->scale.x * 0.5f - 0.05f; // against the wall, a gap short
    for (int cycle = 0; cycle < 4; cycle++) {
        const int push = cycle % 2 == 0; // even: into the wall; odd: back toward us
        const Object *o = game->map->objects[uid];
        float pitch, yaw;
        ObjectManagerAim aim;
        aim_at(p, (vec3) {o->position.x, o->position.y + o->scale.y * 0.5f, o->position.z}, &pitch, &yaw);
        const int hit = object_manager_pick(game, p, pitch, yaw, &aim) && aim.object == uid;
        CHECK(hit && object_manager_editable(game, uid) && object_manager_physgun_takes(game, uid, 0),
              "cycle %d: the crosshair picks the crate (object %d)", cycle, aim.object);
        ObjectManagerPose from, to;
        object_manager_pose(game, uid, &from);
        to = from;
        to.position.y += 3.0f;
        pump(10);
        r = edit_at(a, OM_EDIT_SOURCE_PHYSGUN, uid, &to, OM_EDIT_BEGIN);
        CHECK(r.status == OM_STATUS_OK, "cycle %d: the grab (status %u)", cycle, r.status);
        pump(10);
        // one edit, the whole way: through the wall (it stops at the face) or back to open ground
        to.position.x = push ? ahead + 40.0f : 112.0f;
        r = edit_at(a, OM_EDIT_SOURCE_PHYSGUN, uid, &to, 0);
        pump(4);
        o = game->map->objects[uid];
        const float want = push ? stop : 112.0f - o->scale.x * 0.5f + o->scale.x * 0.5f;
        CHECK((r.status == OM_STATUS_OK || r.status == OM_STATUS_RATE) && fabsf(o->position.x - want) < 0.06f && o->position.x < ahead,
              "cycle %d: the carry %s (status %u, x %.2f, want %.2f, face %.2f)", cycle, push ? "stops at the wall" : "comes back", r.status,
              o->position.x, want, ahead);
        CHECK(!box_in_solid(game, uid), "cycle %d: the crate is in no solid box", cycle);
        // Drop where it is: it lands on the floor (the drop is swept down too)
        pump(10);
        to = from;
        to.position.x = o->position.x;
        to.position.z = o->position.z;
        to.position.y = o->position.y;
        r = edit_at(a, OM_EDIT_SOURCE_PHYSGUN, uid, &to, OM_EDIT_DROP);
        pump(10);
        o = game->map->objects[uid];
        CHECK(r.status == OM_STATUS_OK && o->active && o->collision_type != COLLISION_TYPE_NONE && o->position.y < 0.2f && o->position.y >= game->map->death_y,
              "cycle %d: dropped onto the floor (status %u, y %.2f)", cycle, r.status, o->position.y);
        CHECK(!box_in_solid(game, uid), "cycle %d: after the drop, in no solid box", cycle);
        aim_at(p, (vec3) {o->position.x, o->position.y + o->scale.y * 0.5f, o->position.z}, &pitch, &yaw);
        const int again = object_manager_pick(game, p, pitch, yaw, &aim) && aim.object == uid;
        CHECK(again && object_manager_editable(game, uid) && object_manager_physgun_takes(game, uid, 0),
              "cycle %d: picked again after the drop (object %d)", cycle, aim.object);
    }
    // a carry below the play area's floor (the death plane) stops at it, wherever it is
    const Object *o = game->map->objects[uid];
    const ObjectManagerPose here = {.position = {o->position.x, o->position.y, o->position.z}, .size = o->scale};
    const float vx = o->position.x + 2000.0f, vz = o->position.z + 2000.0f; // open void: no solid, no floor
    const vec3 below = object_manager_carry_feet(game, uid, (vec3) {vx, o->position.y + 20.0f, vz}, (vec3) {vx, game->map->death_y - 500.0f, vz}, here.size);
    CHECK(below.y >= game->map->death_y && below.y < game->map->death_y + 1.0f && fabsf(below.x - vx) < 1e-3f, "in the void, a carry below the death plane stops at it (%.2f, dthY %.2f)", below.y, game->map->death_y);
    p->god_mode = 1;
    clean_up(a);
}

// The Physics Gun on a dead player: the grab goes with the death; the next life grabs and drops again
static void test_physgun_death(TestClient *a) {
    printf("Physics Gun: grab, die, respawn, then grab and drop again...\n");
    Player *p = player_of(a);
    if (!p) return;
    Game *game = &g_server->game;
    p->god_mode = 1;
    clean_up(a);
    pump(200);
    request(a, OM_ACTION_ADD_WEAPON, WEAPON_ID_PHYS_GUN, 0.0f);
    pump(4);
    hold(a, WEAPON_ID_PHYS_GUN);
    set_dev(a, 0);
    NetSpawnResultPacket r = request_at(a, OM_ACTION_SPAWN_OBJECT, entry_index(OM_TAB_OBJECTS, "Crate"), -0.6f, p->direction.y);
    const int uid = (int) game->map->object_count - 1;
    pump(20);
    CHECK(r.status == OM_STATUS_OK, "a crate to carry (status %u)", r.status);
    if (r.status != OM_STATUS_OK) return;
    ObjectManagerPose from, to;
    object_manager_pose(game, uid, &from);
    to = from;
    to.position.y += 3.0f;
    pump(10);
    r = edit_at(a, OM_EDIT_SOURCE_PHYSGUN, uid, &to, OM_EDIT_BEGIN);
    CHECK(r.status == OM_STATUS_OK, "grabbed (status %u)", r.status);
    pump(10);
    // dies in the middle of the carry: the next edit of the dead player is refused
    player_kill(p, NULL, NULL, 0);
    pump(4);
    CHECK(!p->active, "dead");
    to.position.y += 2.0f;
    r = edit_at(a, OM_EDIT_SOURCE_PHYSGUN, uid, &to, 0);
    CHECK(r.status == OM_STATUS_DEAD, "a dead player's edit: Spawn in first (status %u)", r.status);
    // the next life: back in reach of the crate, grab again and drop
    p->death_time = game->now - 10.0f;
    NetEntPacket ent = {.class_index = 0, .secondary_index = -1, .cosmetics = PLAYER_COSMETICS_NONE, .slid_cont = 1, .lag_comp = 1.0f};
    send_to(a, NET_PACKET_ENT, &ent, (uint16_t) NET_ENT_PACKET_V7_SIZE);
    pump(6);
    p = player_of(a);
    CHECK(p && p->active, "respawned");
    if (!p) return;
    const Object *o = game->map->objects[uid];
    p->position = (vec3) {o->position.x + 10.0f, o->position.y, o->position.z};
    p->velocity = (vec3) {0};
    pump(10);
    hold(a, WEAPON_ID_PHYS_GUN);
    object_manager_pose(game, uid, &from);
    to = from;
    to.position.y += 3.0f;
    r = edit_at(a, OM_EDIT_SOURCE_PHYSGUN, uid, &to, OM_EDIT_BEGIN);
    CHECK(r.status == OM_STATUS_OK, "the next life grabs again (status %u)", r.status);
    pump(10);
    to.position.x -= 6.0f;
    r = edit_at(a, OM_EDIT_SOURCE_PHYSGUN, uid, &to, OM_EDIT_DROP);
    pump(10);
    o = game->map->objects[uid];
    CHECK(r.status == OM_STATUS_OK && o->position.y < 0.2f && !box_in_solid(game, uid), "and drops it (status %u, y %.2f)", r.status, o->position.y);
    p->god_mode = 1;
    clean_up(a);
}

// A connection in the menu (no Player yet): the cheats, give, spawn and tp land in its kept state and
// the first spawn applies them; Object Manager requests run from its view (devs)
static void join_menu(TestClient *c) {
    memset(c, 0, sizeof(*c));
    c->v7 = 1;
    c->fd = sandbox_connect();
    c->conn_id = g_server->client_count ? g_server->clients[g_server->client_count - 1].id : -1;
    const NetHelloPacket hello = {NET_PROTOCOL_MAJOR, NET_PROTOCOL_MINOR_V7, 0};
    send_to(c, NET_PACKET_HELLO, &hello, sizeof(hello));
    NetJoinPacket join_packet;
    memset(&join_packet, 0, sizeof(join_packet));
    send_to(c, NET_PACKET_JOIN, &join_packet, (uint16_t) NET_JOIN_PACKET_V2_SIZE);
    pump(20);
}

static void spawn_client(TestClient *c) {
    NetEntPacket ent = {.class_index = 0, .secondary_index = -1, .cosmetics = PLAYER_COSMETICS_NONE, .slid_cont = 1, .lag_comp = 1.0f};
    send_to(c, NET_PACKET_ENT, &ent, (uint16_t) NET_ENT_PACKET_V7_SIZE);
    pump(30);
}

static int cheat_on(const TestClient *c, const int flag, const int from) {
    for (int i = from; i < c->count; i++) {
        const NetPacket *k = &c->packets[i];
        if (k->type != NET_PACKET_CHEAT || k->length != sizeof(NetCheatPacket)) continue;
        NetCheatPacket cheat;
        memcpy(&cheat, k->payload, sizeof(cheat));
        if (cheat.flag == flag) return cheat.on ? 1 : 0;
    }
    return -1;
}

static void test_menu_state(TestClient *m, TestClient *b) {
    printf("commands while not spawned in...\n");
    const Player *q = player_of(b);
    if (!q) return;
    if (m->fd >= NET_LOOPBACK_FD_BASE) leave(m);
    join_menu(m);
    ClientConnection *mc = conn(m);
    CHECK(mc && !mc->player, "a connection in the menu (no Player)");
    if (!mc) return;
    say(m, "/god");
    CHECK(strstr(server_line(m), "God mode ON") != NULL, "god from the menu: '%s'", server_line(m));
    say(m, "/noclip");
    CHECK(strstr(server_line(m), "Noclip ON") != NULL && cheat_on(m, NET_CHEAT_NOCLIP, 0) == 1, "noclip from the menu: '%s'", server_line(m));
    say(m, "/give shotgun");
    CHECK(strstr(server_line(m), "Shotgun added") != NULL && mc->om_extra_count == 1 && mc->om_extras[0] == 5, "give from the menu: '%s' (%d)",
          server_line(m), mc->om_extra_count);
    const int objects = spawned_count();
    say(m, "/spawn crate");
    const Object *spawned = last_spawned();
    CHECK(strstr(server_line(m), "Spawned Crate") != NULL && spawned_count() == objects + 1 && spawned && spawned->spawn_owner == mc->id,
          "spawn from the menu: '%s' (owner %d)", server_line(m), spawned ? spawned->spawn_owner : -1);
    // the Object Manager window's request (the menu camera's look), and Undo from the menu
    NetSpawnResultPacket r = request(m, OM_ACTION_SPAWN_OBJECT, 10, -0.8f);
    CHECK(r.status == OM_STATUS_OK && spawned_count() == objects + 2, "an Objects tile from the menu (status %u)", r.status);
    r = request(m, OM_ACTION_UNDO, 0, 0.0f);
    CHECK(r.status == OM_STATUS_OK && spawned_count() == objects + 1, "Undo from the menu (status %u)", r.status);
    char tp[96];
    const vec3 there = {q->position.x + 4.0f, q->position.y + 2.0f, q->position.z};
    snprintf(tp, sizeof(tp), "/tp %.2f %.2f %.2f", (double) there.x, (double) there.y, (double) there.z);
    say(m, tp);
    CHECK(strstr(server_line(m), "You will spawn at") != NULL, "tp from the menu: '%s'", server_line(m));
    // /score from the menu (KrunkNative): the points wait in the keep; the spawn adds them once
    say(m, "/score 40");
    CHECK(strstr(server_line(m), "+40 score at your next spawn (+40 waiting).") != NULL && mc->keep.score == 40,
          "score from the menu: '%s' (kept %d)", server_line(m), mc->keep.score);
    // a guest in the menu: the Object Manager says "Spawn first", as before
    set_dev(m, 0);
    r = request(m, OM_ACTION_SPAWN_OBJECT, 10, -0.8f);
    CHECK(r.status == OM_STATUS_DEAD, "a guest's request from the menu: Spawn first (status %u)", r.status);
    set_dev(m, 1);

    m->count = 0;
    spawn_client(m);
    Player *p = player_of(m);
    const int spawn_at = index_of(m, NET_PACKET_SPAWN, 0);
    CHECK(p && p->active && p->god_mode && p->noclip, "spawned with god and noclip (god %d noclip %d)", p ? p->god_mode : -1, p ? p->noclip : -1);
    CHECK(p && p->score == 40 && !(mc->keep.set & PLAYER_KEEP_SCORE), "the spawn adds the kept 40 once (score %d, kept %u)", p ? p->score : -1, mc->keep.set);
    CHECK(p && loadout_has(p, 5), "and the Shotgun given in the menu");
    CHECK(p && fabsf(p->position.x - there.x) < 1.0f && fabsf(p->position.z - there.z) < 1.0f, "at the menu tp's spot (%.1f %.1f, want %.1f %.1f)",
          p ? p->position.x : 0.0f, p ? p->position.z : 0.0f, there.x, there.z);
    CHECK(spawn_at >= 0 && cheat_on(m, NET_CHEAT_NOCLIP, spawn_at) == 1, "its client hears noclip on after its SPAWN");
    if (!p) return;
    // dead: the kept state goes on (ungod from the death screen holds for the next life)
    p->god_mode = 0;
    player_kill(p, NULL, NULL, 0);
    pump(4);
    CHECK(!p->active, "dead");
    say(m, "/ungod");
    CHECK(strstr(server_line(m), "God mode OFF") != NULL && !mc->keep.god_mode, "ungod while dead: '%s'", server_line(m));
    const int score_dead = p->score;
    say(m, "/score 9"); // dead: kept like the menu's
    CHECK(strstr(server_line(m), "+9 score at your next spawn (+9 waiting).") != NULL && mc->keep.score == 9, "score while dead: '%s'", server_line(m));
    p->death_time = g_server->game.now - 10.0f;
    spawn_client(m);
    p = player_of(m);
    CHECK(p && p->active && !p->god_mode && p->noclip, "the next life: god off, noclip still on");
    CHECK(p && p->score == score_dead + 9 && !(mc->keep.set & PLAYER_KEEP_SCORE), "the next life adds the kept 9 once (score %d, was %d)", p ? p->score : -1, score_dead);
    // in game: /score adds now and keeps nothing
    const int score_live = p->score;
    say(m, "/score 5");
    CHECK(strstr(server_line(m), "+5 score (total ") != NULL && player_of(m)->score == score_live + 5 && !(mc->keep.set & PLAYER_KEEP_SCORE),
          "in game /score: '%s' (%d -> %d)", server_line(m), score_live, player_of(m)->score);
    r = request(m, OM_ACTION_UNDO, 0, 0.0f); // the crate spawned from the menu is this player's
    CHECK(r.status == OM_STATUS_OK && spawned_count() == objects, "the menu's crate is this player's to undo (status %u)", r.status);
    say(m, "/noclip off");
    leave(m);
}

// The Physics Gun on a player (devs): held at the carried feet, its own input moving nothing, let go
// on release or OM_HOLD_TIMEOUT without an update
static void test_physgun_player(TestClient *a, TestClient *b) {
    printf("the Physics Gun on a player...\n");
    Player *p = player_of(a), *q = player_of(b);
    if (!p || !q) return;
    clean_up(a);
    p->god_mode = q->god_mode = 1;
    request(a, OM_ACTION_ADD_WEAPON, WEAPON_ID_PHYS_GUN, 0.0f);
    hold(a, WEAPON_ID_PHYS_GUN);
    q->position = (vec3) {p->position.x + 20.0f, p->position.y, p->position.z};
    q->velocity = (vec3) {0};
    pump(10);
    set_dev(a, 0);
    vec3 feet = {p->position.x + 15.0f, p->position.y + 12.0f, p->position.z};
    NetSpawnResultPacket r = edit_player(a, q->uid, feet, 0);
    CHECK(r.status == OM_STATUS_DENIED && !q->om_held, "a guest may not carry players (status %u)", r.status);
    set_dev(a, 1);
    b->count = 0;
    r = edit_player(a, q->uid, feet, 0);
    CHECK(r.status == OM_STATUS_OK && q->om_held && q->om_held_by == p->uid + 1, "a dev grabs b (status %u)", r.status);
    CHECK(cheat_on(b, NET_CHEAT_HELD, 0) == 1 && count_type(b, NET_PACKET_POSITION) >= 1, "b hears it is held, and its new feet");
    pump(20); // b's own input and gravity move nothing
    for (int i = 0; i < 10; i++) {
        Input in = {.seq = ++b->seq, .move_dir = 0, .delta = 1.0f / 60.0f, .x_dir = 0.0f, .y_dir = q->direction.y, .jump = 1};
        NetInputPacket packet;
        net_packet_from_input(&packet, &in);
        send_to(b, NET_PACKET_INPUT, &packet, sizeof(packet));
        pump(1);
    }
    CHECK(fabsf(q->position.x - feet.x) < 0.01f && fabsf(q->position.y - feet.y) < 0.01f && fabsf(q->position.z - feet.z) < 0.01f,
          "b stays where it is carried (%.2f %.2f %.2f)", q->position.x, q->position.y, q->position.z);
    // carried on, at 60 a second (no pace)
    for (int i = 0; i < 30; i++) {
        feet.z += 0.5f;
        r = edit_player(a, q->uid, feet, 0);
        if (r.status != OM_STATUS_OK) break;
    }
    CHECK(r.status == OM_STATUS_OK && fabsf(q->position.z - feet.z) < 0.01f, "carried 15 along z, update by update (status %u, z %.2f)", r.status, q->position.z);
    feet.y += 40.0f; // up in the air, then let go
    r = edit_player(a, q->uid, feet, 0);
    CHECK(r.status == OM_STATUS_OK && fabsf(q->position.y - feet.y) < 0.01f, "lifted 40 (status %u)", r.status);
    b->count = 0;
    r = edit_player(a, q->uid, feet, 1);
    CHECK(r.status == OM_STATUS_OK && !q->om_held && cheat_on(b, NET_CHEAT_HELD, 0) == 0, "released (status %u)", r.status);
    for (int i = 0; i < 12; i++) input(b, -1, 0); // b's own frames move it again
    CHECK(q->position.y < feet.y - 0.5f, "and b falls again (y %.2f, was %.2f)", q->position.y, feet.y);
    // the carrier stops sending: let go after OM_HOLD_TIMEOUT
    feet = (vec3) {p->position.x + 15.0f, p->position.y + 12.0f, p->position.z};
    r = edit_player(a, q->uid, feet, 0);
    CHECK(r.status == OM_STATUS_OK && q->om_held, "grabbed again");
    pump((int) (OM_HOLD_TIMEOUT * 60.0f) + 10);
    CHECK(!q->om_held, "let go after %.1f s without an update", (double) OM_HOLD_TIMEOUT);
    p->god_mode = q->god_mode = 0;
    clean_up(a);
}

// A bot nobody tracks (game_bots_add, as bot_add's and the server's quota make them): the Delete key
// takes it for a dev or the host and refuses a guest; the nearest thing the shot reaches goes first
static void test_remove_bot(TestClient *a) {
    printf("the Delete key on an untracked bot...\n");
    Player *p = player_of(a);
    if (!p) return;
    clean_up(a);
    const float yaw = p->direction.y;
    const vec3 at = {p->position.x - sinf(yaw) * 30.0f, p->position.y, p->position.z - cosf(yaw) * 30.0f};
    GameBot *bot = game_bots_add(&g_server->game, NULL, &at, yaw);
    CHECK(bot && bot->player, "a bot 30 units ahead");
    if (!bot || !bot->player) return;
    pump(5);
    float pitch, aim_yaw;
    aim_at(p, (vec3) {bot->player->position.x, bot->player->position.y + bot->player->height * 0.5f, bot->player->position.z}, &pitch, &aim_yaw);
    const int bots = game_bots_count(&g_server->game);
    set_dev(a, 0);
    NetSpawnResultPacket r = request_at(a, OM_ACTION_REMOVE_AIM, 0, pitch, aim_yaw);
    CHECK(r.status != OM_STATUS_OK && game_bots_count(&g_server->game) == bots, "a guest may not remove it (status %u)", r.status);
    set_dev(a, 1);
    r = request_at(a, OM_ACTION_REMOVE_AIM, 0, pitch, aim_yaw);
    CHECK(r.status == OM_STATUS_OK && game_bots_count(&g_server->game) == bots - 1, "a dev removes it at the crosshair (status %u, bots %d)", r.status,
          game_bots_count(&g_server->game));
    pump(5);
    r = request_at(a, OM_ACTION_REMOVE_AIM, 0, pitch, aim_yaw);
    CHECK(r.status == OM_STATUS_NOTHING, "nothing left there (status %u)", r.status);
}

// The client's interpolation of other players' edits: 30 updates a second shown at 144 fps move
// smoothly (no step bigger than the speed allows), reach each target, and turn the short way
static void test_interp(void) {
    printf("edit interpolation...\n");
    ObjectManagerInterp it;
    memset(&it, 0, sizeof(it));
    const double update = 1.0 / 30.0, frame = 1.0 / 144.0;
    const float speed = 60.0f; // units / s: 2 per update
    vec3 shown = {0.0f, 5.0f, 0.0f};
    float shown_yaw = 0.0f;
    double now = 0.0, next_update = 0.0;
    int k = 0, monotonic = 1;
    float max_step = 0.0f, last_x = 0.0f;
    for (int f = 0; f < 144; f++, now += frame) {
        if (now + 1e-9 >= next_update) {
            const vec3 target = {speed * (float) (k * update), 5.0f, 0.0f};
            object_manager_interp_push(&it, shown, shown_yaw, target, 0.0f, now);
            k++;
            next_update += update;
        }
        vec3 pos;
        float yaw;
        object_manager_interp_sample(&it, now, &pos, &yaw);
        if (f > 10) {
            max_step = fmaxf(max_step, pos.x - last_x);
            monotonic &= pos.x >= last_x - 1e-4f;
        }
        last_x = pos.x;
        shown = pos;
        shown_yaw = yaw;
    }
    CHECK(monotonic, "it never moves back");
    CHECK(max_step <= speed * (float) frame * 1.6f, "no stepping: the largest step %.3f (an update is %.3f)", max_step, speed * (float) update);
    const vec3 last_target = it.to;
    vec3 end;
    float end_yaw;
    CHECK(!object_manager_interp_sample(&it, now + 1.0, &end, &end_yaw) && fabsf(end.x - last_target.x) < 1e-4f, "it settles on the last update (%.2f)", end.x);
    // a turn across 0: 6.2 -> 0.1 goes up (the short way), halfway at 2 pi + ... wrap
    memset(&it, 0, sizeof(it));
    object_manager_interp_push(&it, (vec3) {0}, 6.2f, (vec3) {0}, 0.1f, 0.0);
    it.span = 0.1;
    vec3 pos;
    float yaw;
    object_manager_interp_sample(&it, 0.05, &pos, &yaw);
    CHECK(fabsf(yaw - (6.2f + (0.1f + 2.0f * (float) M_PI - 6.2f) * 0.5f)) < 1e-3f, "a turn across 0 goes the short way (%.3f)", yaw);
    // the packed yaw round trip and the 45 degree quarter
    CHECK(fabsf(object_manager_yaw_unpack(object_manager_yaw_pack((float) M_PI / 4.0f)) - (float) M_PI / 4.0f) < 1e-4f &&
          object_manager_yaw_quarter((float) M_PI / 4.0f) == 0 && object_manager_yaw_quarter((float) M_PI * 0.75f) == 1 &&
          object_manager_yaw_pack(-0.5f) == object_manager_yaw_pack(2.0f * (float) M_PI - 0.5f), "yaw packing");
}

// KrunkNative client prediction of Objects-tab spawns (object_manager_predict_spawn): a's client (a
// mirror game that applies what a hears) shows the crate at once at the uid the server gives it;
// the server's SPAWNED_OBJ replaces it there; a cancelled one (its SPAWN_RESULT refused it) or one
// never answered leaves a placeholder so the uids stay aligned.
static void test_prediction(TestClient *a) {
    printf("spawn prediction...\n");
    Player *p = player_of(a);
    if (!p) return;
    Game *game = &g_server->game;
    pump(200);
    Game mirror;
    memset(&mirror, 0, sizeof(mirror));
    const cJSON *maps[1] = {game->map->raw_data};
    int modes[1] = {MODE_ID_FFA};
    game_configure(&mirror, NULL, maps, 1, modes, 1, NULL, 0, NULL, 0);
    game_init(&mirror, 0, 0, 0);
    // the earlier tests' spawned objects (gone now, their uids kept) are placeholders on this client
    if (mirror.ready && game->map->object_count > mirror.map->object_count) {
        Object *pad = calloc(1, sizeof(Object));
        pad->dynamic = pad->destroyed = 1;
        pad->collision_type = COLLISION_TYPE_NONE;
        if (!world_put_object(&mirror, game->map->object_count - 1, pad)) free(pad);
    }
    CHECK(mirror.ready && !game_is_authority(&mirror) && mirror.map->object_count == game->map->object_count, "a client mirror of the map");
    if (!mirror.ready || mirror.map->object_count != game->map->object_count) return;
    Player *me = player_init(&mirror);
    me->uid = p->uid;
    me->position = p->position;
    me->direction = p->direction;
    me->height = p->height;
    me->is_you = 1;
    const int crate = entry_index(OM_TAB_OBJECTS, "Crate");
    const float pitch = -0.6f, yaw = p->direction.y;

    // 1. shown at once at the next uid, solid
    int uid = -1;
    const Object *predicted = object_manager_predict_spawn(&mirror, me, crate, pitch, yaw, &uid);
    CHECK(predicted && uid == (int) game->map->object_count && mirror.map->object_count == (size_t) uid + 1 && predicted->spawned &&
          predicted->active && predicted->predicted_ms > 0.0f && predicted->collision_type == COLLISION_TYPE_BOX && predicted->prefab == PREFAB_CRATE,
          "the crate is predicted at uid %d (server objects %zu)", uid, game->map->object_count);
    if (!predicted) return;
    const vec3 at = predicted->position;

    // 2. the server spawns it (same aim): its SPAWNED_OBJ takes the predicted uid
    NetSpawnResultPacket r = request_at(a, OM_ACTION_SPAWN_OBJECT, crate, pitch, yaw);
    CHECK(r.status == OM_STATUS_OK && game->map->object_count == (size_t) uid + 1, "the server spawns it at that uid (status %u)", r.status);
    for (int i = 0; i < a->count; i++) {
        if (a->packets[i].type == NET_PACKET_SPAWNED_OBJ) object_manager_apply_spawned(&mirror, a->packets[i].payload, a->packets[i].length);
    }
    const Object *real = mirror.map->objects[uid];
    const Object *server = game->map->objects[uid];
    world_cancel_prediction(&mirror, uid, predicted); // its SPAWN_RESULT: nothing to cancel any more
    real = mirror.map->objects[uid];
    CHECK(mirror.map->object_count == (size_t) uid + 1 && real->spawned && real->active && real->predicted_ms == 0.0f &&
          fabsf(real->position.x - server->position.x) < 0.001f && fabsf(real->position.z - server->position.z) < 0.001f,
          "replaced by the server's crate: one object at the uid");
    CHECK(fabsf(at.x - server->position.x) < 0.11f && fabsf(at.y - server->position.y) < 0.11f && fabsf(at.z - server->position.z) < 0.11f,
          "the prediction stood where the server put it (%.2f %.2f %.2f / %.2f %.2f %.2f)", at.x, at.y, at.z, server->position.x,
          server->position.y, server->position.z);

    // 3. refused (its SPAWN_RESULT cancels it): a placeholder keeps the uid, the server's next object takes it
    me->direction.y = yaw + (float) M_PI * 0.5f;
    predicted = object_manager_predict_spawn(&mirror, me, crate, pitch, me->direction.y, &uid);
    CHECK(predicted && uid == (int) mirror.map->object_count - 1, "a second prediction");
    if (!predicted) return;
    world_cancel_prediction(&mirror, uid, predicted);
    const Object *gap = mirror.map->objects[uid];
    CHECK(!gap->spawned && !gap->active && gap->collision_type == COLLISION_TYPE_NONE && mirror.map->object_count == (size_t) uid + 1,
          "cancelled: a placeholder at uid %d", uid);
    pump(20);
    r = request_at(a, OM_ACTION_SPAWN_OBJECT, crate, pitch, yaw + (float) M_PI);
    for (int i = 0; i < a->count; i++) {
        if (a->packets[i].type == NET_PACKET_SPAWNED_OBJ) object_manager_apply_spawned(&mirror, a->packets[i].payload, a->packets[i].length);
    }
    CHECK(r.status == OM_STATUS_OK && mirror.map->objects[uid]->spawned && mirror.map->objects[uid]->active, "the server's next object replaces the placeholder");

    // 4. never answered: gone after BUILD_PREDICT_MS
    predicted = object_manager_predict_spawn(&mirror, me, crate, pitch, me->direction.y, &uid);
    CHECK(predicted != NULL, "a third prediction");
    world_builds_tick(&mirror, BUILD_PREDICT_MS / 1000.0f + 0.1f);
    CHECK(predicted && !mirror.map->objects[uid]->active && !mirror.map->objects[uid]->spawned && !mirror.world.predicted_builds,
          "unanswered: a placeholder after the timeout");

    // 5. the authority never predicts
    CHECK(!object_manager_predict_spawn(game, p, crate, pitch, yaw, &uid) && uid == -1, "no prediction on the authority");

    world_remove_builds(&mirror); // (the mirror is left like the other tests' mirrors)
    object_manager_round_start(&mirror);
    clean_up(a);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    test_codec();
    test_interp();
    const cJSON *burg = map_catalog_json((size_t) map_catalog_index_of_key("311_burg"));
    CHECK(burg != NULL, "map");
    if (burg && start(burg, MODE_ID_FFA)) {
        join(&g_clients[0], 1);
        join(&g_clients[1], 1);
        test_loadout(&g_clients[0], &g_clients[1]);
        test_rate(&g_clients[0]);
        test_pickups(&g_clients[0], &g_clients[1]);
        test_npcs_entities(&g_clients[0], &g_clients[1]);
        test_objects(&g_clients[0], &g_clients[1]);
        test_give_drop(&g_clients[0], &g_clients[1]);
        test_tool_gun(&g_clients[0], &g_clients[1]);
        test_remove_bot(&g_clients[0]);
        test_pose_math();
        test_edits(&g_clients[0], &g_clients[1]);
        test_join_sync(&g_clients[0], &g_clients[2], &g_clients[3]);
        test_permissions(&g_clients[0], &g_clients[1]);
        test_public_refusals(&g_clients[0], &g_clients[1]);
        test_dev_privileges(&g_clients[0], &g_clients[1]);
        test_physgun_player(&g_clients[0], &g_clients[1]);
        test_physgun_props(&g_clients[0]);
        test_prediction(&g_clients[0]);
        test_menu_state(&g_clients[3], &g_clients[1]);
        test_entity_touch(&g_clients[0], &g_clients[1]); // last: power-ups change loadouts and scores
        for (int i = 0; i < CLIENTS; i++) leave(&g_clients[i]);
        sandbox_stop();
    } else CHECK(0, "sandbox");
    // the Physics Gun cycles in a round of their own: grab, drop, pick again
    if (burg && start(burg, MODE_ID_FFA)) {
        join(&g_clients[0], 1);
        test_physgun_cycles(&g_clients[0]);
        test_physgun_death(&g_clients[0]);
        for (int i = 0; i < CLIENTS; i++) leave(&g_clients[i]);
        sandbox_stop();
    } else CHECK(0, "sandbox (Physics Gun cycles)");
    // the map's own objects: Kanji has jump pads (Burg has none)
    const cJSON *kanji = map_catalog_json((size_t) map_catalog_index_of_key("311_kanji"));
    CHECK(kanji != NULL, "kanji");
    if (kanji && start(kanji, MODE_ID_FFA)) {
        join(&g_clients[0], 1);
        join(&g_clients[1], 1);
        test_map_edits(&g_clients[0], &g_clients[1], &g_clients[2]);
        for (int i = 0; i < CLIENTS; i++) leave(&g_clients[i]);
        sandbox_stop();
    } else CHECK(0, "sandbox (kanji)");
    // the Properties tool: Kanji with a trigger appended above its first jump pad
    test_fields_table();
    cJSON *with_trigger = kanji ? cJSON_Duplicate(kanji, 1) : NULL;
    cJSON *objects = with_trigger ? cJSON_GetObjectItem(with_trigger, "objects") : NULL;
    const cJSON *first_pad = NULL, *it;
    cJSON_ArrayForEach(it, objects) {
        if (!first_pad && map_raw_object_prefab(it) == PREFAB_BOOST_PAD) first_pad = it;
    }
    const cJSON *fp = first_pad ? cJSON_GetObjectItem(first_pad, "p") : NULL;
    if (fp && cJSON_GetArraySize(fp) >= 3) {
        char json[256];
        snprintf(json, sizeof(json), "{\"i\":%d,\"p\":[%g,%g,%g],\"s\":[10,10,10],\"te\":3,\"tas\":[{\"ta\":1,\"tc\":5}]}", PREFAB_TRIGGER,
                 cJSON_GetNumberValue(cJSON_GetArrayItem(fp, 0)) + 30.0, cJSON_GetNumberValue(cJSON_GetArrayItem(fp, 1)) + 20.0,
                 cJSON_GetNumberValue(cJSON_GetArrayItem(fp, 2)));
        cJSON_AddItemToArray(objects, cJSON_Parse(json));
    }
    CHECK(fp != NULL, "kanji has a jump pad to hang the trigger by");
    if (fp && start(with_trigger, MODE_ID_FFA)) {
        join(&g_clients[0], 1);
        join(&g_clients[1], 1);
        test_field_edits(&g_clients[0], &g_clients[1], &g_clients[2]);
        for (int i = 0; i < CLIENTS; i++) leave(&g_clients[i]);
        sandbox_stop();
    } else CHECK(0, "sandbox (kanji + trigger)");
    cJSON_Delete(with_trigger);
    printf("%d checks, %d failures\n", g_checks, g_failures);
    printf(g_failures ? "Test FAILED\n" : "Test OK\n");
    return g_failures ? 1 : 0;
}
