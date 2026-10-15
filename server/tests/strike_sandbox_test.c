// Krunker Strike in the offline sandbox (server/include/server_sandbox.h): the real server core over
// the loopback transport, the same scenario played under the base flavor (Krunker v10.0.2) and under
// FLAVOR_STRIKE (FRVR Krunker Strike 1.37.2's rules, shared/include/strike_rules.h), checked against
// Strike's numbers (tools/strike/strike_data.json, doc/strike/parity.md):
//   class pick   Hunter (1) plays Strike's Hunter (110 HP, the Sniper only); a class Strike does not
//                have (Spray N Pray, 3) plays the starter Triggerman (180 HP, the Assault Rifle)
//   movement     one second walking forward: Hunter / Triggerman distance follows the class speeds
//                (Strike 0.85 / 1.0 with every weapon spdMlt 1; v10 1.0 x 0.95 / 1.05 x 0.95), and Strike's
//                classConfig ground acceleration (grA 0.048, v10 0.042)
//   damage       an Assault Rifle body shot at 10 units: Strike's 20 (v10 23), a headshot x1.45 (v10 x1.5),
//                through the padded AABB hit test (no posed v10 hit boxes under Strike)
//   recoil       the shot's recoil kick: Strike 0.002 / ms (v10 0.003)
// Run from the repository root: ./<build>/strike_sandbox_test
#include <server_sandbox.h>
#include <server.h>
#include <shared.h>
#include <flavor.h>
#include <hitboxes.h>
#include <ai.h> // game_can_see_point
#include <modes.h> // mode_melee_damage
#include <knet.h>
#include <ruleset.h>
#include <strike_rules.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_failures, g_checks;
#define CHECK(cond, ...) do { \
    g_checks++; \
    if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_failures++; } \
    else printf("ok    " __VA_ARGS__), printf("\n"); \
} while (0)

typedef struct {
    int fd;
    NetPacketBuffer buffer;
    int uid;
    int seq;
} Peer;

// A flat 1000 x 1000 floor (top at y 2), spawns far apart.
static const char *k_map =
    "{\"name\":\"Strike Flat\",\"spawns\":[[-300,2,-300,0,0,0],[300,2,300,0,0,0]],"
    "\"objects\":[{\"p\":[0,0,0],\"s\":[1000,2,1000],\"t\":1},{\"p\":[0,2,-400],\"s\":[20,20,2],\"t\":1}]}";

static void drain(Peer *p) {
    uint8_t bytes[2048];
    int n;
    while ((n = sandbox_receive(p->fd, bytes, sizeof(bytes))) > 0) {
        if (!net_buffer_push(&p->buffer, bytes, (size_t) n)) return;
        NetPacket packet;
        while (net_buffer_next(&p->buffer, &packet) > 0) {
            if (packet.type == NET_PACKET_PING && packet.length == sizeof(NetPingPacket)) net_send_packet(p->fd, NET_PACKET_PONG, packet.payload, packet.length);
            NetSpawnPacket spawn;
            if (packet.type == NET_PACKET_SPAWN && net_packet_read_spawn(&spawn, &packet) && spawn.is_you && spawn.active) p->uid = spawn.uid;
        }
    }
}

static void step(Peer *a, Peer *b, const int frames) {
    for (int f = 0; f < frames; f++) {
        sandbox_step(1.0 / 60.0);
        drain(a);
        drain(b);
    }
}

static void join(Peer *p, const int class_index) {
    memset(p, 0, sizeof(*p));
    p->uid = -1;
    p->fd = sandbox_connect();
    const NetHelloPacket hello = {NET_PROTOCOL_MAJOR, NET_PROTOCOL_MINOR, NET_PROTOCOL_PATCH};
    net_send_packet(p->fd, NET_PACKET_HELLO, &hello, sizeof(hello));
    NetJoinPacket join_packet;
    memset(&join_packet, 0, sizeof(join_packet));
    net_send_packet(p->fd, NET_PACKET_JOIN, &join_packet, (uint16_t) NET_JOIN_PACKET_V2_SIZE);
    const NetEntPacket ent = {.class_index = class_index, .secondary_index = -1, .cosmetics = PLAYER_COSMETICS_NONE, .slid_cont = 1,
                              .lag_comp = 1.0f};
    net_send_packet(p->fd, NET_PACKET_ENT, &ent, (uint16_t) NET_ENT_PACKET_V7_SIZE);
}

static Player *player_of(const Peer *p) {
    for (size_t i = 0; i < g_server->game.player_count; i++) {
        if (g_server->game.players[i] && g_server->game.players[i]->uid == p->uid) return g_server->game.players[i];
    }
    return NULL;
}

static void input(Peer *p, const int move_dir, const float pitch, const float yaw, const int shoot) {
    const NetInputPacket in = {.seq = p->seq++, .move_dir = move_dir, .delta = 1.0f / 60.0f, .x_dir = pitch, .y_dir = yaw,
                               .flags = (uint8_t) (shoot ? NET_INPUT_SHOOT : 0)};
    net_send_packet(p->fd, NET_PACKET_INPUT, &in, sizeof(in));
}

static void place(Player *pl, const float x, const float z) {
    pl->position = (vec3) {x, 2.0f, z};
    pl->velocity = (vec3) {0, 0, 0};
}

typedef struct {
    int hunter_class, hunter_health, hunter_weapon, hunter_loadout;
    int fallback_class, fallback_health, fallback_weapon;
    float hunter_walk, trigger_walk;
    float body_damage, head_damage;
    float recoil, recoil_force, spread;
    int posed;
    int stalker_health, stalker_knife;
    int see_blocked, see_padded, see_low, see_high;
    float stalk_melee, stalk_heal, living_health;
} Result;

// Shoot `target` from `shooter` (both standing, 10 units apart along -Z) at height `aim_y` above the
// floor; the damage of the first hit.
static float shot(Peer *ps, Peer *pt, Player *shooter, Player *target, const float aim_y, float *recoil_force) {
    place(shooter, 0.0f, 20.0f);
    place(target, 0.0f, 10.0f);
    target->health = (float) target->max_health;
    for (int f = 0; f < 30; f++) { // settle, the lag-comp history catches up, the gun is ready
        input(ps, -1, 0.0f, 0.0f, 0);
        input(pt, -1, 0.0f, 0.0f, 0);
        step(ps, pt, 1);
    }
    const float eye = shooter->position.y + shooter->height - game_constants.camera_height;
    const float pitch = atan2f((2.0f + aim_y) - eye, 10.0f);
    const float before = target->health;
    for (int f = 0; f < 20 && target->health == before; f++) {
        input(ps, -1, pitch, 0.0f, 1);
        input(pt, -1, 0.0f, 0.0f, 0);
        step(ps, pt, 1);
        if (target->health != before && recoil_force) *recoil_force = shooter->recoil_force;
    }
    input(ps, -1, pitch, 0.0f, 0);
    step(ps, pt, 1);
    return before - target->health;
}

static Result run(const GameFlavor flavor) {
    Result r = {0};
    CHECK(flavor_set(flavor), "flavor %s", flavor_key(flavor));
    cJSON *map = cJSON_Parse(k_map);
    const cJSON *maps[1] = {map};
    const SandboxConfig cfg = {.maps = maps, .map_count = 1, .map_index = 0, .mode_id = MODE_ID_FFA};
    CHECK(sandbox_start(&cfg), "sandbox_start (%s)", flavor_key(flavor));
    r.posed = hitboxes_active(&g_server->game);
    // canSee from a bare point (Strike's aim assist from the camera): the 20 x 20 x 2 wall at z -400
    // blocks; a pad over its half width (10) shrinks it to nothing (canSee's pad, vPad false); the
    // ray starts cameraHeight below the point, so a point 1 above the wall's top still looks into it
    r.see_blocked = !game_can_see_point(&g_server->game, (vec3) {0, 10, -390}, (vec3) {0, 10, -410}, 0.4f);
    r.see_padded = game_can_see_point(&g_server->game, (vec3) {0, 10, -390}, (vec3) {0, 10, -410}, 10.5f);
    r.see_low = !game_can_see_point(&g_server->game, (vec3) {0, 23, -390}, (vec3) {0, 23, -410}, 0.4f);
    r.see_high = game_can_see_point(&g_server->game, (vec3) {0, 25, -390}, (vec3) {0, 25, -410}, 0.4f);

    static Peer a, b;
    join(&a, 1);                                  // Hunter
    join(&b, flavor == FLAVOR_STRIKE ? 3 : 0);    // Strike: Spray N Pray -> Triggerman; base: Triggerman
    step(&a, &b, 120);
    Player *hunter = player_of(&a), *trigger = player_of(&b);
    CHECK(hunter && trigger && hunter->active && trigger->active, "both spawned (%d, %d)", a.uid, b.uid);
    if (!hunter || !trigger) {
        sandbox_stop();
        cJSON_Delete(map);
        return r;
    }
    r.hunter_class = hunter->class_index;
    r.hunter_health = hunter->max_health;
    r.hunter_loadout = hunter->loadout_size;
    r.hunter_weapon = hunter->loadout ? hunter->loadout[0] : -1;
    r.fallback_class = trigger->class_index;
    r.fallback_health = trigger->max_health;
    r.fallback_weapon = trigger->loadout ? trigger->loadout[0] : -1;

    // walk forward (-Z) one second from a standstill, far apart
    place(hunter, -200.0f, 200.0f);
    place(trigger, 200.0f, 200.0f);
    for (int f = 0; f < 10; f++) {
        input(&a, -1, 0.0f, 0.0f, 0);
        input(&b, -1, 0.0f, 0.0f, 0);
        step(&a, &b, 1);
    }
    const vec3 ha = hunter->position, ta = trigger->position;
    for (int f = 0; f < 60; f++) {
        input(&a, 0, 0.0f, 0.0f, 0);
        input(&b, 0, 0.0f, 0.0f, 0);
        step(&a, &b, 1);
    }
    step(&a, &b, 10);
    r.hunter_walk = hypotf(hunter->position.x - ha.x, hunter->position.z - ha.z);
    r.trigger_walk = hypotf(trigger->position.x - ta.x, trigger->position.z - ta.z);

    // the Assault Rifle: a body shot (feet + 6: above the legs at 4.2, below the head band at 9), a
    // headshot (feet + 10)
    r.body_damage = shot(&b, &a, trigger, hunter, 6.0f, &r.recoil_force);
    r.head_damage = shot(&b, &a, trigger, hunter, 10.0f, NULL);
    r.recoil = trigger->weapon ? trigger->weapon->recoil : -1.0f;
    r.spread = trigger->weapon ? trigger->weapon->spread : -1.0f;

    // Stalker: pickStalker after the 15 s wait timer, then the stalker's knife swing (onMelee)
    sandbox_host(0, MODE_ID_STALK);
    step(&a, &b, 60);
    for (int f = 0; f < 17 * 60; f++) {
        input(&a, -1, 0.0f, 0.0f, 0);
        input(&b, -1, 0.0f, 0.0f, 0);
        step(&a, &b, 1);
    }
    Player *pa = player_of(&a), *pb = player_of(&b);
    Player *stalker = pa && pa->team == 2 ? pa : pb && pb->team == 2 ? pb : NULL;
    Player *living = stalker == pa ? pb : pa;
    if (stalker && living) {
        r.stalker_health = stalker->max_health;
        r.stalker_knife = stalker->loadout && stalker->loadout_size > 0 && stalker->loadout[0] == WEAPON_ID_KNIFE;
        stalker->health = 100.0f;
        living->health = (float) living->max_health;
        r.living_health = living->health;
        r.stalk_melee = mode_melee_damage(&g_server->game, stalker, living, 50.0f);
        r.stalk_heal = stalker->health - 100.0f;
    }

    sandbox_disconnect(a.fd);
    sandbox_disconnect(b.fd);
    sandbox_step(1.0 / 60.0);
    sandbox_stop();
    cJSON_Delete(map);
    return r;
}

static int near(const float a, const float b, const float tol) { return fabsf(a - b) <= tol; }

int main(void) {
    printf("-- base (Krunker v10.0.2)\n");
    const Result base = run(FLAVOR_BASE);
    printf("-- strike (FRVR Krunker Strike 1.37.2)\n");
    const Result st = run(FLAVOR_STRIKE);
    printf("   base:   hunter %d HP, walk %.2f / %.2f, AR body %.2f head %.2f, recoil %.3f (force %.3f), spread %.0f\n", base.hunter_health,
           (double) base.hunter_walk, (double) base.trigger_walk, (double) base.body_damage, (double) base.head_damage, (double) base.recoil,
           (double) base.recoil_force, (double) base.spread);
    printf("   strike: hunter %d HP, walk %.2f / %.2f, AR body %.2f head %.2f, recoil %.3f (force %.3f), spread %.0f\n", st.hunter_health,
           (double) st.hunter_walk, (double) st.trigger_walk, (double) st.body_damage, (double) st.head_damage, (double) st.recoil,
           (double) st.recoil_force, (double) st.spread);

    // base: Krunker v10.0.2 untouched
    CHECK(base.hunter_class == 1 && base.hunter_health == 60, "base Hunter: 60 HP (v10)");
    CHECK(base.posed, "base: posed hit boxes");
    CHECK(base.body_damage > 22.0f && base.body_damage <= 23.0f, "base AR body shot ~23 (%.2f)", (double) base.body_damage);
    CHECK(base.head_damage > 1.5f * 22.0f && base.head_damage <= 1.5f * 23.0f + 0.01f, "base AR headshot ~23 x 1.5 (%.2f)", (double) base.head_damage);
    CHECK(near(base.recoil, 3.0f, 1e-4f), "base AR recoil 0.003/ms");

    // Strike: class pick
    CHECK(st.hunter_class == 1 && st.hunter_health == 110 && st.hunter_weapon == 0, "Strike Hunter: 110 HP, Sniper Rifle (%d HP, weapon %d)",
          st.hunter_health, st.hunter_weapon);
    CHECK(st.fallback_class == 0 && st.fallback_health == 180 && st.fallback_weapon == 1,
          "class 3 (not in Strike) plays Triggerman: 180 HP, Assault Rifle (class %d, %d HP, weapon %d)", st.fallback_class, st.fallback_health,
          st.fallback_weapon);
    // movement: the walk ratio follows the class speed x weapon spdMlt
    const float ratio = st.trigger_walk > 0.0f ? st.hunter_walk / st.trigger_walk : 0.0f;
    const float base_ratio = base.trigger_walk > 0.0f ? base.hunter_walk / base.trigger_walk : 0.0f;
    CHECK(near(ratio, 0.85f, 0.02f), "Strike walk Hunter / Triggerman %.3f (0.85 / 1.0)", (double) ratio);
    CHECK(near(base_ratio, (1.0f * 0.95f) / (1.05f * 0.95f), 0.02f), "base walk ratio %.3f (1.0 / 1.05)", (double) base_ratio);
    // Strike's classConfig ground acceleration default grA 0.048 (v10 0.042), speed 1 x spdMlt 1 (v10 1.05 x .95)
    const float tw = base.trigger_walk > 0.0f ? st.trigger_walk / base.trigger_walk : 0.0f;
    CHECK(near(tw, (0.048f / 0.042f) * 1.0f / (1.05f * 0.95f), 0.02f), "Strike Triggerman walks %.3f x base (grA .048/.042, speed 1 / 1.05 x .95)",
          (double) tw);
    // damage through the v7 AABB
    CHECK(!st.posed, "Strike: padded AABB hit test (no posed boxes)");
    CHECK(st.body_damage > 19.5f && st.body_damage <= 20.0f, "Strike AR body shot ~20 (%.2f)", (double) st.body_damage);
    CHECK(st.head_damage > 1.45f * 19.5f && st.head_damage <= 1.45f * 20.0f + 0.01f, "Strike AR headshot ~20 x 1.45 (%.2f)", (double) st.head_damage);
    // recoil and spread
    CHECK(near(st.recoil, 2.0f, 1e-4f) && st.recoil_force > 0.0f && st.recoil_force <= 2.0f + 1e-3f,
          "Strike AR recoil 0.002/ms (kick %.3f)", (double) st.recoil_force);
    CHECK(near(st.spread, 40.0f, 1e-3f) && near(base.spread, 100.0f, 1e-3f), "AR spread: Strike 40, v10 100");

    // Stalker: v10 floor(200 + 50 x (2 - 1)) and the x0.6 swing with a 10 heal; Strike (v7) 400 HP, the
    // swing takes the target's whole health and heals the stalker 40
    CHECK(base.stalker_health == 250 && base.stalker_knife && near(base.stalk_melee, 30.0f, 1e-3f) && near(base.stalk_heal, 10.0f, 1e-3f),
          "base Stalker: 250 HP, knife, swing x0.6 (%.1f) heals 10 (%.1f)", (double) base.stalk_melee, (double) base.stalk_heal);
    CHECK(st.stalker_health == 400 && st.stalker_knife && near(st.stalk_melee, st.living_health, 1e-3f) && near(st.stalk_heal, 40.0f, 1e-3f),
          "Strike Stalker: 400 HP, knife, swing takes the whole health (%.1f) heals 40 (%.1f)", (double) st.stalk_melee, (double) st.stalk_heal);
    CHECK(st.see_blocked && st.see_padded && st.see_low && st.see_high, "canSee from a point: wall blocks, pad shrinks, ray cameraHeight below (%d %d %d %d)",
          st.see_blocked, st.see_padded, st.see_low, st.see_high);

    if (g_failures) {
        printf("strike_sandbox_test: %d of %d checks failed\n", g_failures, g_checks);
        return 1;
    }
    printf("strike_sandbox_test: all %d checks passed\n", g_checks);
    return 0;
}
