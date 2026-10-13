// Every v7.1.4 weapon through the three ways a player gets it, over the offline sandbox server
// (server_sandbox.h, the authority server_bin and the offline client run): (a) the class / mode
// loadout (a class that holds it, else players.giveWeapon as a streak / mode hands it out),
// (b) a KrunkNative Object Manager extra loadout slot (OM_ACTION_ADD_WEAPON + slot pick) and
// (c) an Object Manager weapon pickup dropped at the feet and taken with the v7 "int" (type 6).
// Each case is driven by real input frames (NET_PACKET_INPUT): equip, ADS, fire held at a target
// player 40 units ahead (melee: 8), tactical and empty reloads, and the weapon's special
// mechanic (charge, grapple, build, deploy, burst, pellets, projectiles / splash, throw).
// Prints the weapon x path x behaviour table; a FAIL line names each broken cell.
// Run from the repository root: ./<build>/weapons_audit_test [-v]
#include <server_sandbox.h>
#include <server.h>
#include <shared.h>
#include <object_manager.h>
#include <weapons.h>
#include <world.h>
#include <spawnables.h>
#include <modes.h>
#include <knet.h> // NET_LOOPBACK_FD_BASE
#include <cJSON.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures, g_checks, g_verbose;

#define CHECK(cond, ...) do { \
    g_checks++; \
    if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_failures++; } \
} while (0)

#define FRAME (1.0f / 60.0f)

typedef struct {
    int fd;
    int conn_id;
    int seq;
    NetPacketBuffer buffer;
    int count;
} TestClient;

static TestClient g_a, g_b;

static void send_to(TestClient *c, const int type, const void *payload, const uint16_t length) {
    net_send_packet(c->fd, (NetPacketType) type, payload, length);
}

static void drain(TestClient *c) {
    uint8_t bytes[4096];
    int n;
    while ((n = sandbox_receive(c->fd, bytes, sizeof(bytes))) > 0) {
        if (!net_buffer_push(&c->buffer, bytes, (size_t) n)) c->buffer.length = 0;
        NetPacket packet;
        while (net_buffer_next(&c->buffer, &packet) > 0) {
            if (packet.type == NET_PACKET_PING && packet.length == sizeof(NetPingPacket)) send_to(c, NET_PACKET_PONG, packet.payload, packet.length);
            c->count++;
        }
    }
}

static void pump(const int frames) {
    for (int f = 0; f < frames; f++) {
        sandbox_step(FRAME);
        if (g_a.fd >= NET_LOOPBACK_FD_BASE) drain(&g_a);
        if (g_b.fd >= NET_LOOPBACK_FD_BASE) drain(&g_b);
    }
}

static Player *player_of(const TestClient *c) {
    for (size_t i = 0; i < g_server->client_count; i++) {
        if (g_server->clients[i].id == c->conn_id) return g_server->clients[i].player;
    }
    return NULL;
}

static void send_ent(TestClient *c, const int class_index, const int secondary) {
    NetEntPacket ent = {.class_index = class_index, .secondary_index = secondary, .cosmetics = PLAYER_COSMETICS_NONE, .slid_cont = 1, .lag_comp = 1.0f};
    send_to(c, NET_PACKET_ENT, &ent, (uint16_t) NET_ENT_PACKET_V7_SIZE);
}

static void join(TestClient *c) {
    memset(c, 0, sizeof(*c));
    c->fd = sandbox_connect();
    c->conn_id = g_server->client_count ? g_server->clients[g_server->client_count - 1].id : -1;
    const NetHelloPacket hello = {NET_PROTOCOL_MAJOR, NET_PROTOCOL_MINOR, NET_PROTOCOL_PATCH};
    send_to(c, NET_PACKET_HELLO, &hello, sizeof(hello));
    NetJoinPacket join_packet;
    memset(&join_packet, 0, sizeof(join_packet));
    send_to(c, NET_PACKET_JOIN, &join_packet, (uint16_t) NET_JOIN_PACKET_V2_SIZE);
    send_ent(c, 0, -1);
    pump(30);
}

// One Object Manager request (its effect is read back from the server's player state).
static void om_request(TestClient *c, const int action, const int item, const float pitch) {
    pump(20); // the Object Manager rate limit refills
    const Player *p = player_of(c);
    const NetSpawnReqPacket req = {.action = (uint8_t) action, .item = (int16_t) item, .pitch = pitch, .yaw = p ? p->direction.y : 0.0f};
    send_to(c, NET_PACKET_SPAWN_REQ, &req, sizeof(req));
    pump(3);
}

// ---------------------------------------------------------------- the arena

static float g_pitch, g_yaw;

typedef struct {
    int shoot, scope, reload, jump, slot;
} Keys;

static void frame(TestClient *c, const Keys k) {
    Input in = {.seq = ++c->seq, .move_dir = -1, .delta = FRAME, .x_dir = g_pitch, .y_dir = g_yaw};
    in.shoot = (unsigned char) !!k.shoot;
    in.scope = (unsigned char) !!k.scope;
    in.reload = (unsigned char) !!k.reload;
    in.jump = (unsigned char) !!k.jump;
    in.select_slot = (unsigned char) (k.slot > 0 ? k.slot : 0);
    NetInputPacket packet;
    net_packet_from_input(&packet, &in);
    send_to(c, NET_PACKET_INPUT, &packet, sizeof(packet));
    // B idles (its frames keep it in the game)
    Input idle = {.seq = ++g_b.seq, .move_dir = -1, .delta = FRAME};
    net_packet_from_input(&packet, &idle);
    send_to(&g_b, NET_PACKET_INPUT, &packet, sizeof(packet));
    pump(1);
}

static void frames(TestClient *c, const int n, const Keys k) {
    for (int i = 0; i < n; i++) frame(c, k);
}

#define TARGET_Z -40.0f
#define WALL_Z -300.0f // the wall's front face (z = -295)

static void place(Player *p, const float x, const float y, const float z) {
    p->position = (vec3) {x, y, z};
    p->last_position = p->position;
    p->velocity = (vec3) {0.0f, 0.0f, 0.0f};
}

// A at the origin looking down -z at B's chest; B `dist` ahead, healthy.
static void arena(Player *a, Player *b, const float dist) {
    place(a, 0.0f, 0.0f, 0.0f);
    if (b) {
        if (!b->active) {
            b->active = 1;
        }
        place(b, 0.0f, 0.0f, -dist);
        b->max_health = 100000;
        b->health = 100000.0f;
        b->god_mode = 0;
    }
    const float eye = a->height - game_constants.camera_height;
    g_yaw = 0.0f;
    g_pitch = atanf((6.0f - eye) / dist);
}

// ---------------------------------------------------------------- the table

enum { PATH_CLASS, PATH_EXTRA, PATH_PICKUP, PATH_COUNT };
static const char *k_path_names[PATH_COUNT] = {"class", "extra", "pickup"};

enum { B_EQUIP, B_ADS, B_FIRE, B_AMMO, B_RELOAD_TAC, B_RELOAD_EMPTY, B_SPECIAL, B_COUNT };
static const char *k_beh_names[B_COUNT] = {"equip", "ads", "fire", "ammo", "reload-tac", "reload-empty", "special"};

typedef struct {
    char text[B_COUNT][48];
    int bad[B_COUNT];
    int na;
    char how[32];
} Cell;

static Cell g_cells[WEAPON_COUNT][PATH_COUNT];

static void cell(Cell *c, const int b, const int ok, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char text[48];
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    snprintf(c->text[b], sizeof(c->text[b]), "%s%s", ok ? "" : "BROKEN ", text);
    c->bad[b] = !ok;
}

static int slot_of(const Player *p, const int id) {
    for (int i = p->loadout_size - 1; i >= 0; i--) {
        if (p->loadout[i] == id) return i;
    }
    return -1;
}

static int class_with(const int id) {
    const ClassConfig *classes = g_server->game.classes;
    for (size_t i = 0; i < g_server->game.class_count; i++) {
        if (classes[i].loadout && classes[i].loadout[0] == id) return (int) i;
    }
    return -1;
}

static int class_with_secondary(void) {
    const ClassConfig *classes = g_server->game.classes;
    for (size_t i = 0; i < g_server->game.class_count; i++) {
        if (classes[i].secondary) return (int) i;
    }
    return 0;
}

static void respawn(TestClient *c, const int class_index, const int secondary) {
    Player *p = player_of(c);
    if (p) {
        p->health = 0.0f;
        p->active = 0;
        p->death_time = g_server->game.now - 10.0f;
    }
    const float life = p ? p->life_start : -1.0f;
    send_ent(c, class_index, secondary);
    for (int i = 0; i < 240; i++) { // until the new life starts
        pump(1);
        p = player_of(c);
        if (p && p->active && p->life_start != life) break;
    }
    pump(2);
}

static int count_projectiles(const int weapon) {
    int n = 0;
    for (int i = 0; i < GAME_MAX_PROJECTILES; i++) n += g_server->game.projectiles[i].active && g_server->game.projectiles[i].weapon_index == weapon;
    return n;
}

static int count_spawnables(void) {
    int n = 0;
    for (int i = 0; g_server->game.mode && i < MODE_MAX_SPAWNABLES; i++) n += g_server->game.mode->spawnables[i].active;
    return n;
}

static int count_builds(void) {
    int n = 0;
    const Map *map = g_server->game.map;
    for (size_t i = 0; map && i < map->object_count; i++) n += map->objects[i]->dynamic && map->objects[i]->active && map->objects[i]->build_index >= 0;
    return n;
}

// (a) / (b) / (c): the weapon in the loadout; its slot, -1 n/a
static int give(const int id, const int path, Cell *c) {
    const Weapon *w = g_server->game.weapons[id];
    om_request(&g_a, OM_ACTION_CLEAR_WEAPONS, 0, 0.0f);
    if (path == PATH_CLASS) {
        const int cls = class_with(id);
        if (cls >= 0) {
            respawn(&g_a, cls, -1);
            snprintf(c->how, sizeof(c->how), "class %s", g_server->game.classes[cls].name);
        } else if (w->secondary) {
            respawn(&g_a, class_with_secondary(), id);
            snprintf(c->how, sizeof(c->how), "secondary");
        } else {
            respawn(&g_a, 0, -1);
            player_give_weapon(player_of(&g_a), id); // a streak / mode / mystery box (players.giveWeapon)
            pump(4);
            snprintf(c->how, sizeof(c->how), "giveWeapon");
        }
        return slot_of(player_of(&g_a), id);
    }
    respawn(&g_a, 0, -1);
    if (path == PATH_EXTRA) {
        om_request(&g_a, OM_ACTION_ADD_WEAPON, id, 0.0f);
        snprintf(c->how, sizeof(c->how), "OM extra");
        return slot_of(player_of(&g_a), id);
    }
    // pickup: dropped at the feet, A stands on it and interacts
    Player *a = player_of(&g_a);
    place(a, 0.0f, 0.0f, 0.0f);
    g_pitch = -1.5f;
    g_yaw = 0.0f;
    frame(&g_a, (Keys) {0});
    const size_t before = g_server->game.map->object_count;
    om_request(&g_a, OM_ACTION_DROP_WEAPON, id, -1.5f);
    if (g_server->game.map->object_count <= before) {
        snprintf(c->how, sizeof(c->how), "drop refused");
        return -1;
    }
    const int uid = (int) g_server->game.map->object_count - 1;
    GamePickup k;
    if (!game_pickup_state(&g_server->game, uid, &k)) return -1;
    place(a, k.position.x, k.position.y, k.position.z);
    frame(&g_a, (Keys) {0});
    pump(12); // past config.interactTimer
    const NetInteractV7Packet interact = {.action = 1, .secondary = 0, .type = GAME_INTERACT_PICKUP, .uid = uid};
    send_to(&g_a, NET_PACKET_INTERACT, &interact, sizeof(interact));
    pump(4);
    snprintf(c->how, sizeof(c->how), "OM pickup");
    om_request(&g_a, OM_ACTION_UNDO, 0, 0.0f); // the (now empty / swapped) pickup goes
    return slot_of(player_of(&g_a), id);
}

static int full_ammo(const Player *p, const int slot) {
    const Weapon *w = player_slot_weapon(p, slot);
    return w ? mode_ammo_limit(p->game, (int) w->ammo) : 0;
}

static void wait_s(const float seconds) {
    frames(&g_a, (int) ceilf(seconds / FRAME), (Keys) {0});
}

typedef struct {
    int min_ammo, ammo_spent_events;
    float damage, max_charge;
    int projectiles, max_projectiles;
    int builds, spawnables;
    int grappled;
    float pulled; // distance moved toward the wall
    int removed;  // the weapon left the loadout
} FireResult;

static FireResult hold_fire(const int id, int slot, const int hold, const int scope_first) {
    const float pitch = g_pitch;
    frames(&g_a, 20, (Keys) {0}); // B's lag compensation history catches up with the teleport
    g_pitch = pitch;
    Player *a = player_of(&g_a), *b = player_of(&g_b);
    FireResult r = {.min_ammo = a->ammo[slot]};
    const float health = b->health;
    float low = health; // the lowest health seen: B regenerates
    const int builds = count_builds(), spawnables = count_spawnables();
    const float z0 = a->position.z;
    int last_ammo = (int) a->ammo[slot];
    for (int i = 0; i < scope_first; i++) {
        frame(&g_a, (Keys) {.scope = 1});
        if (player_of(&g_a)->charge_time > r.max_charge) r.max_charge = player_of(&g_a)->charge_time;
    }
    for (int i = 0; i < hold; i++) {
        frame(&g_a, (Keys) {.shoot = 1, .scope = !!scope_first});
        a = player_of(&g_a);
        slot = slot_of(a, id);
        if (slot < 0) {
            r.removed = 1;
            break;
        }
        const int ammo = (int) a->ammo[slot];
        if (ammo < last_ammo) r.ammo_spent_events++;
        last_ammo = ammo;
        if (ammo < r.min_ammo) r.min_ammo = ammo;
        if (a->charge_time > r.max_charge) r.max_charge = a->charge_time;
        const int pj = count_projectiles(id);
        if (pj > r.max_projectiles) r.max_projectiles = pj;
        if (a->grapple.active) r.grappled = 1;
        if (player_of(&g_b)->health < low) low = player_of(&g_b)->health;
        if (g_verbose > 1) printf("    hold %d ammo %d aim %.2f spread %.1f burst %d reloads %.2f did_shoot %d Bhp %.0f\n", i, ammo, a->aim_val, a->spread, a->burst_count, a->reloads[slot], a->did_shoot, player_of(&g_b)->health);
    }
    r.pulled = z0 - a->position.z;
    for (int i = 0; i < 60; i++) { // a burst finishes, projectiles land
        frame(&g_a, (Keys) {0});
        a = player_of(&g_a);
        slot = slot_of(a, id);
        if (slot < 0) {
            r.removed = 1;
            break;
        }
        if ((int) a->ammo[slot] < r.min_ammo) r.min_ammo = (int) a->ammo[slot];
        if (player_of(&g_b)->health < low) low = player_of(&g_b)->health;
    }
    b = player_of(&g_b);
    r.damage = health - fminf(low, b->health);
    if (g_verbose && r.damage <= 0.0f && !player_of(&g_a)->weapon->melee) {
        a = player_of(&g_a);
        printf("  [no damage %s] B active %d hp %.0f pos %.1f %.1f %.1f | A pos %.1f %.1f %.1f pitch %.3f spread %.3f hist %d\n", a->weapon->name, b->active, b->health,
               b->position.x, b->position.y, b->position.z, a->position.x, a->position.y, a->position.z, g_pitch, a->spread, b->state_history_count);
    }
    r.builds = count_builds() - builds;
    r.spawnables = count_spawnables() - spawnables;
    return r;
}

static void run_case(const int id, const int path) {
    Cell *c = &g_cells[id][path];
    const Weapon *w = g_server->game.weapons[id];
    if (w->bomb || (path != PATH_CLASS && !object_manager_weapon_listed(id))) {
        c->na = 1;
        snprintf(c->how, sizeof(c->how), "n/a");
        return;
    }
    int slot = give(id, path, c);
    Player *a = player_of(&g_a), *b = player_of(&g_b);
    if (!a || !b) return;
    if (slot < 0) {
        cell(c, B_EQUIP, path == PATH_CLASS && w->custom_only, "not in loadout (%s)", c->how);
        if (path == PATH_CLASS && w->custom_only) c->na = 1;
        return;
    }
    a->score = 0;
    a->god_mode = 1;
    arena(a, b, w->melee ? 8.0f : 40.0f);
    frame(&g_a, (Keys) {.slot = slot + 1});
    wait_s(1.0f);
    a = player_of(&g_a);
    slot = slot_of(a, id);
    const int equipped = slot >= 0 && a->loadout_index == slot && a->weapon == player_slot_weapon(a, slot);
    cell(c, B_EQUIP, equipped, "slot %d/%d idx %d", slot, a->loadout_size, a->loadout_index);
    if (g_verbose && !equipped) {
        printf("  [%s %s] active %d hp %.0f swap %.2f reload %.2f weapon %s queue %zu seq %d noswap %d\n", g_server->game.weapons[id]->name, k_path_names[path],
               a->active, a->health, a->swap_timer, a->reload_timer, a->weapon ? a->weapon->name : "-", a->input_queue_size, a->input_seq, a->weapon ? a->weapon->no_swap : 0);
    }
    if (!equipped) return;

    // ADS
    frames(&g_a, 45, (Keys) {.scope = 1});
    a = player_of(&g_a);
    const float aim = a->aim_val;
    if (w->no_aim || w->melee) cell(c, B_ADS, aim < 0.01f || w->melee, "aim %.2f (noAim)", aim);
    else cell(c, B_ADS, aim > 0.99f, "aim %.2f", aim);
    wait_s(0.8f);

    // fire
    a = player_of(&g_a);
    arena(a, b, w->melee ? 8.0f : 40.0f);
    const int full = full_ammo(a, slot);
    a->ammo[slot] = (unsigned int) full;
    a->reload_timer = 0.0f;
    FireResult fr;
    char special[48] = "-";
    int special_ok = 1;
    if (w->build) {
        // v7: score >= cost (Block 10); KrunkNative: free with the Object Manager's spawn right
        const int sandbox_free = a->build_free;
        a->build_free = 0;
        g_pitch = -0.6f; // the floor ahead
        fr = hold_fire(id, slot, 4, 0);
        const int at_zero = fr.builds;
        world_remove_builds(&g_server->game);
        wait_s(0.5f);
        a = player_of(&g_a);
        a->score = 1000;
        g_pitch = -0.6f;
        fr = hold_fire(id, slot, 4, 0);
        const int scored = fr.builds;
        world_remove_builds(&g_server->game);
        wait_s(0.5f);
        a = player_of(&g_a);
        a->score = 0;
        a->build_free = (unsigned char) sandbox_free;
        g_pitch = -0.6f;
        fr = hold_fire(id, slot, 4, 0);
        snprintf(special, sizeof(special), "builds: %d at 0, %d at 1000, %d free(%d)", at_zero, scored, fr.builds, sandbox_free);
        special_ok = at_zero == 0 && scored > 0 && sandbox_free && fr.builds > 0 && a->score == 0;
        cell(c, B_FIRE, scored > 0, "%d builds", scored);
        world_remove_builds(&g_server->game); // they would stand between the next shooter and B
    } else if (w->grapple_range > 0.0f) {
        g_pitch = 0.15f; // the wall ahead, over B
        fr = hold_fire(id, slot, 60, 0);
        snprintf(special, sizeof(special), "grapple %d pulled %.1f", fr.grappled, fr.pulled);
        special_ok = fr.grappled && fr.pulled > 25.0f; // v7 reels 0.04 units / ms
        cell(c, B_FIRE, fr.grappled, "grapple %d", fr.grappled);
    } else if (w->deploy_range > 0.0f) {
        g_pitch = -0.8f;
        fr = hold_fire(id, slot, 4, 0);
        snprintf(special, sizeof(special), "pads %d", fr.spawnables);
        special_ok = fr.spawnables > 0;
        cell(c, B_FIRE, fr.spawnables > 0, "pads %d", fr.spawnables);
    } else if (w->charge_time > 0.0f && !w->shoot_charge) { // Crossbow, Zapper: aim to charge
        fr = hold_fire(id, slot, 2, (int) ceilf((w->charge_time + 0.3f) / FRAME));
        snprintf(special, sizeof(special), "aim-charge %.2f/%.2f dmg %.0f", fr.max_charge, w->charge_time, fr.damage);
        special_ok = fr.max_charge >= w->charge_time - 0.02f && fr.damage > 0.0f;
        cell(c, B_FIRE, fr.damage > 0.0f, "dmg %.0f", fr.damage);
    } else if (w->shoot_charge) { // Charge Rifle: hold to charge, the full charge fires the blast
        fr = hold_fire(id, slot, 60, 0);
        snprintf(special, sizeof(special), "charge %.2f dmg %.0f spent %d", fr.max_charge, fr.damage, full - fr.min_ammo);
        special_ok = fr.max_charge >= w->charge_time - 0.02f && fr.damage >= 700.0f;
        cell(c, B_FIRE, fr.damage > 0.0f, "dmg %.0f", fr.damage);
    } else if (w->melee) {
        fr = hold_fire(id, slot, 2, 0);
        cell(c, B_FIRE, fr.damage > 0.0f, "dmg %.0f", fr.damage);
        if (w->can_throw) { // aimed: the throw
            wait_s(0.6f);
            a = player_of(&g_a);
            arena(a, b, 40.0f);
            FireResult t = hold_fire(id, slot, 2, 40);
            snprintf(special, sizeof(special), "throw dmg %.0f", t.damage);
            special_ok = t.damage > 0.0f;
        }
    } else {
        // one click first (aimed: less spread): burst / pellets
        const int aim_frames = w->no_aim ? 0 : 30;
        fr = hold_fire(id, slot, 1, aim_frames);
        const int one_click = full - fr.min_ammo;
        if (w->burst) {
            wait_s(0.6f);
            snprintf(special, sizeof(special), "burst: %d rounds per click", one_click);
            special_ok = one_click == (int) w->burst_count;
        } else if (w->shots > 1) {
            snprintf(special, sizeof(special), "pellets: one click dmg %.0f", fr.damage);
            special_ok = fr.damage > w->damage; // more than one pellet landed
        } else if (w->projectile_config.explode > 0.0f && (!w->projectile_disable)) {
            snprintf(special, sizeof(special), "explosive pj dmg %.0f", fr.damage);
            special_ok = fr.damage > 0.0f;
        }
        wait_s(0.6f);
        a = player_of(&g_a);
        slot = slot_of(a, id);
        if (slot >= 0) {
            arena(a, b, 40.0f);
            a->ammo[slot] = (unsigned int) full;
            FireResult held = hold_fire(id, slot, 30, aim_frames);
            fr.damage += held.damage;
            fr.min_ammo = held.min_ammo;
            fr.removed |= held.removed;
        }
        cell(c, B_FIRE, fr.damage > 0.0f, "dmg %.0f", fr.damage);
    }
    cell(c, B_SPECIAL, special_ok, "%s", special);

    // ammo accounting
    a = player_of(&g_a);
    slot = slot_of(a, id);
    if (w->melee || w->build || w->grapple_range > 0.0f) cell(c, B_AMMO, 1, "no ammo use");
    else if (fr.removed) cell(c, B_AMMO, w->remove_on_empty || w->deploy_range > 0.0f, "removed when empty");
    else cell(c, B_AMMO, fr.min_ammo < full, "%d -> %d", full, fr.min_ammo);

    if (slot < 0 || w->melee || w->build || w->grapple_range > 0.0f) {
        cell(c, B_RELOAD_TAC, 1, "n/a");
        cell(c, B_RELOAD_EMPTY, 1, "n/a");
        return;
    }
    // tactical reload
    wait_s(0.6f);
    a = player_of(&g_a);
    a->ammo[slot] = full > 1 ? (unsigned int) full - 1 : 0;
    a->reload_timer = 0.0f;
    frame(&g_a, (Keys) {.reload = 1});
    const float started = a->reload_timer;
    wait_s(w->reload_time * 1.2f + 0.3f);
    a = player_of(&g_a);
    slot = slot_of(a, id);
    const int extra = path == PATH_EXTRA;
    if (slot < 0) cell(c, B_RELOAD_TAC, 0, "weapon gone");
    else if (w->no_reload && !extra) cell(c, B_RELOAD_TAC, (int) a->ammo[slot] == (full > 1 ? full - 1 : 0), "v7 noReload: %u", a->ammo[slot]);
    else cell(c, B_RELOAD_TAC, (int) a->ammo[slot] == full, "timer %.2f -> %u/%d", started, slot >= 0 ? a->ammo[slot] : 0, full);

    // empty reload: fire on an empty magazine
    if (slot < 0) return;
    a->ammo[slot] = 0;
    a->reload_timer = 0.0f;
    frames(&g_a, 3, (Keys) {.shoot = 1});
    wait_s(w->reload_time * 1.2f + 0.3f);
    a = player_of(&g_a);
    slot = slot_of(a, id);
    if (w->no_reload && !extra) cell(c, B_RELOAD_EMPTY, slot < 0 || a->ammo[slot] == 0, "v7 noReload: %s", slot < 0 ? "removed" : "stays empty");
    else cell(c, B_RELOAD_EMPTY, slot >= 0 && (int) a->ammo[slot] == full, "%u/%d", slot >= 0 ? a->ammo[slot] : 0, full);
}

static cJSON *arena_map(void) {
    return cJSON_Parse("{\"name\":\"weapons_audit\",\"spawns\":[[0,0,0],[0,0,-40]],\"objects\":["
                       "{\"p\":[0,-10,0],\"s\":[1200,10,1200]},"
                       "{\"p\":[0,0,-300],\"s\":[600,300,10]}]}");
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    g_verbose = argc > 1 && !strcmp(argv[1], "-v") ? 1 : argc > 1 && !strcmp(argv[1], "-vv") ? 2 : 0;
    int only = -1, last = -1;
    if (argc > 2) only = atoi(argv[2]);
    if (argc > 3) last = atoi(argv[3]);
    cJSON *map = arena_map();
    static const cJSON *maps[1];
    maps[0] = map;
    const SandboxConfig cfg = {.maps = maps, .map_count = 1, .map_index = 0, .mode_id = MODE_ID_FFA};
    g_a.fd = g_b.fd = -1;
    if (!map || !sandbox_start(&cfg)) {
        printf("sandbox failed\n");
        return 1;
    }
    g_server->game.round_endless = 1; // the audit outlasts a match: no end screen
    join(&g_a);
    join(&g_b);
    for (int id = 0; id < WEAPON_COUNT; id++) {
        if (only >= 0 && (last >= 0 ? (id < only || id > last) : id != only)) continue;
        for (int path = 0; path < PATH_COUNT; path++) run_case(id, path);
    }
    // the table
    int broken = 0;
    for (int id = 0; id < WEAPON_COUNT; id++) {
        if (only >= 0 && (last >= 0 ? (id < only || id > last) : id != only)) continue;
        for (int path = 0; path < PATH_COUNT; path++) {
            const Cell *c = &g_cells[id][path];
            printf("%-2d %-15s %-6s %-22s", id, g_server->game.weapons[id]->name, k_path_names[path], c->how);
            if (c->na) {
                printf(" n/a\n");
                continue;
            }
            for (int b = 0; b < B_COUNT; b++) {
                if (!c->text[b][0]) continue;
                printf(" | %s: %s", k_beh_names[b], c->text[b]);
                broken += c->bad[b];
                g_checks++;
                if (c->bad[b]) g_failures++;
            }
            printf("\n");
        }
    }
    sandbox_stop();
    cJSON_Delete(map);
    printf("%d checks, %d broken cells\n", g_checks, broken);
    printf(g_failures ? "Test FAILED\n" : "Test OK\n");
    return g_failures ? 1 : 0;
}
