// The Charge Rifle and the Grappler against Krunker v7.1.4, through procInputs frames
// (player_proc_input) on an authority game, the way server_bin and the offline sandbox run them:
//   Charge Rifle (weapons.js 28: chrgTime 700, shootChrg, bypassChrg, chrgDec 0.8, rateChrg,
//   fullChrgSht, fullChrgMlt 50; player.js:1067-1107 the charge, players.js:2656-2700 shoot):
//   holding fire charges 1 ms per ms to 0.7 s (fullCharge), the 15-damage shots fire meanwhile
//   at rate 150 ms x max(0.7, (1 - charge) * 1.5) without spending the round, the first shot
//   with fullCharge is the blast (15 x 50, "_blast" sound, the round spent), released the
//   charge decays 0.8 ms per ms.
//   Grappler (weapons.js 20: grapRang 500; players.js:2514-2585 tryGrapple, player.js:1271-1290
//   the rope, 1756-1762 the release, 1946-1952 the replay): the hook on the wall the look ray
//   hits, the rope (hit distance + height - cameraHeight + 0.001) reeling in 0.04 per ms down
//   to 35, the player held on its sphere (so pulled at 40 units / s once taut), jump lets go,
//   a weapon swap lets go, a miss spends the shot, "gp" objects take no hook; RECON V3 and
//   input.grapObj carry it to the predicting client.
// Run from the repository root: ./<build>/charge_grapple_test
#include <shared.h>
#include <cJSON.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_failures, g_checks;

#define CHECK(cond, ...) do { g_checks++; \
    if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_failures++; } \
} while (0)

#define DT 0.01f   // 10 ms frames: whole ms, procInputs' rounding keeps them exact
#define WALL_Z -200.0f

static int g_seq;
static int g_blast_shots; // NetShotPacket.first == 2 to the others

static void test_send(Game *game, const int uid, const int type, const void *payload, const uint16_t length) {
    (void) game; (void) uid;
    if (type == NET_PACKET_SHOT && length == sizeof(NetShotPacket) && ((const NetShotPacket *) payload)->first == 2) g_blast_shots++;
}

static Player *add_player(Game *game, const int uid, const int class_index) {
    Player *player = player_init(game);
    player->uid = uid;
    player->class_index = class_index;
    snprintf(player->name, sizeof(player->name), "p%d", uid);
    game_players_add(game, player);
    player_spawn(player);
    return player;
}

static void equip(Player *player, const int weapon_id) {
    for (int i = 0; i < player->loadout_size; i++) {
        if (player->loadout[i] != weapon_id) continue;
        player_swap_weapon(player, i, 1, 1, 0);
        player->swap_timer = 0.0f;
        player->reloads[i] = 0.0f;
        return;
    }
}

static Input frame_input(const Player *player, const int shoot, const int jump) {
    return (Input) {.seq = ++g_seq, .delta = DT, .move_dir = -1, .shoot = (unsigned char) shoot, .jump = (unsigned char) jump,
                    .x_dir = player->direction.x, .y_dir = player->direction.y};
}

static void frame(Player *player, const int shoot, const int jump) {
    const Input input = frame_input(player, shoot, jump);
    player_proc_input(player, &input, 0, 0);
}

static void stand(Player *player, const float x, const float z) {
    player->position = (vec3) {x, 0.0f, z};
    player->last_position = player->position;
    player->velocity = (vec3) {0.0f, 0.0f, 0.0f};
    player->direction = (vec2) {0.0f, 0.0f};
    player->on_ground = 1;
}

static float dist3(const vec3 a, const vec3 b) {
    return sqrtf((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
}

// ---------------------------------------------------------------- Charge Rifle

static void test_charge_rifle(Game *game) {
    printf("Charge Rifle: hold to charge, the full charge blasts...\n");
    Player *p = add_player(game, 1, CLASS_ID_INFILTRATOR);
    Player *target = add_player(game, 2, 3);
    CHECK(p->weapon == g_weapons[WEAPON_ID_CHARGE_RIFLE], "Infiltrator holds the Charge Rifle");
    equip(p, WEAPON_ID_CHARGE_RIFLE);
    stand(p, 0.0f, 0.0f);
    stand(target, 0.0f, -40.0f);
    target->max_health = 100000;
    target->health = 100000.0f;
    for (int i = 0; i < 20; i++) { // the target's lag compensation history, settled
        player_store_state(target);
        frame(p, 0, 0);
    }
    p->direction.x = atanf((6.0f - (p->height - game_constants.camera_height)) / 40.0f); // B's chest
    stand(p, 0.0f, 0.0f);
    p->direction.x = atanf((6.0f - (p->height - game_constants.camera_height)) / 40.0f);
    p->charge_time = 0.0f;
    p->full_charge = 0;
    const int slot = p->loadout_index;
    p->ammo[slot] = 1;

    // hold fire: the charge grows by the frame time; shots fire meanwhile, free
    int shots = 0, blast_frame = -1, charge_exact = 1, full_at = -1;
    float last_reload = 0.0f, first_interval = 0.0f, late_interval = 0.0f;
    float blast_damage = 0.0f;
    g_blast_shots = 0;
    for (int f = 1; f <= 90; f++) {
        const float health = target->health;
        const unsigned int ammo = p->ammo[slot];
        frame(p, 1, 0);
        player_store_state(target);
        const float reload = p->reloads[slot];
        if (reload > last_reload + 1e-6f) { // a shot this frame
            shots++;
            if (shots == 1) first_interval = reload;
            if (p->ammo[slot] < ammo) {
                blast_frame = f;
                blast_damage = health - target->health;
            } else if (f > 55 && !late_interval) {
                late_interval = reload;
            }
        }
        last_reload = reload;
        if (blast_frame < 0 && full_at < 0 && fabsf(p->charge_time - fminf(0.7f, f * DT)) > 1e-4f) charge_exact = 0;
        if (full_at < 0 && p->full_charge) full_at = f;
    }
    printf("  charge: +%.0f ms per 10 ms frame, fullCharge at frame %d (%.2f s); %d shots; first interval %.4f s, late %.4f s; "
           "blast at frame %d: %.1f damage, %d blast SHOT packets\n",
           DT * 1000.0f, full_at, full_at * DT, shots, first_interval, late_interval, blast_frame, blast_damage, g_blast_shots);
    CHECK(charge_exact, "the charge is the held time (chargeTime += delta) up to chrgTime");
    CHECK(full_at == 70, "fullCharge at 0.70 s (frame %d)", full_at);
    // rateChrg: 150 ms x max(0.7, (1 - 0.01 / 0.7) * 1.5) at the first frame's charge
    CHECK(fabsf(first_interval - 0.15f * fmaxf(0.7f, (1.0f - 0.01f / 0.7f) * 1.5f)) < 1e-4f, "first interval %.4f", first_interval);
    CHECK(fabsf(late_interval - 0.15f * 0.7f) < 1e-4f, "a charge past 0.53 s fires every 105 ms (%.4f)", late_interval);
    CHECK(blast_frame >= 70 && blast_frame <= 70 + 11, "the blast fires once the charge is full (frame %d)", blast_frame);
    CHECK(blast_damage > 700.0f && blast_damage <= 750.0f, "the blast does 15 x 50 = 750 less falloff (%.1f)", blast_damage);
    CHECK(g_blast_shots == 1, "the others hear one blast (NetShotPacket.first 2): %d", g_blast_shots);
    CHECK(p->ammo[slot] == 0 && p->charge_time < 0.25f, "the blast spends the round and restarts the charge (%u, %.3f)", p->ammo[slot],
          p->charge_time);
    CHECK(p->reload_timer > 0.0f, "fire held on the empty rifle reloads (%.2f)", p->reload_timer);

    // released: chrgDec 0.8 ms per ms
    for (int i = 0; i < 160; i++) frame(p, 0, 0); // the 1.4 s reload
    CHECK(p->ammo[slot] == 1, "reloaded");
    p->charge_time = 0.0f;
    for (int i = 0; i < 30; i++) frame(p, 1, 0);
    const float held = p->charge_time;
    for (int i = 0; i < 10; i++) frame(p, 0, 0);
    printf("  released after %.2f s: %.3f after 0.1 s (chrgDec 0.8)\n", held, p->charge_time);
    CHECK(fabsf(held - 0.30f) < 1e-4f && fabsf(p->charge_time - (held - 0.08f)) < 1e-4f, "decay 0.8 per s (%.3f -> %.3f)", held, p->charge_time);

    // the bar the HUD draws: interMProg = chargeTime / chrgTime
    CHECK(fabsf(p->charge_time / p->weapon->charge_time - 0.22f / 0.7f) < 1e-3f, "interMProg");
    target->active = 0; // out of the next test
    p->active = 0; // out of the next test
}

// ---------------------------------------------------------------- Grappler

static void test_grapple(Game *game) {
    printf("Grappler: hook, reel, pull, release...\n");
    Player *p = add_player(game, 3, 0);
    // the Object Manager / a custom game's secondary; here straight into the loadout
    p->loadout[1] = WEAPON_ID_GRAPPLER;
    p->ammo[1] = g_weapons[WEAPON_ID_GRAPPLER]->ammo;
    equip(p, WEAPON_ID_GRAPPLER);
    CHECK(p->weapon == g_weapons[WEAPON_ID_GRAPPLER], "holding the Grappler");
    stand(p, 0.0f, 0.0f);
    for (int i = 0; i < 10; i++) frame(p, 0, 0); // settle on the floor
    const float y0 = p->position.y;
    const float eye_off = p->height - game_constants.camera_height;

    // fire at the wall 200 ahead (yaw 0 looks down -z)
    frame(p, 1, 0);
    CHECK(p->grapple.active, "hooked");
    const float range0 = 200.0f + eye_off + 0.001f;
    CHECK(fabsf(p->grapple.point.z - WALL_Z) < 0.05f && fabsf(p->grapple.point.y - (y0 + eye_off)) < 0.11f,
          "the hook on the wall face at the eye height (%.2f %.2f %.2f)", p->grapple.point.x, p->grapple.point.y, p->grapple.point.z);
    // the frame that hooks runs the rope after the weapon code: no reel yet
    CHECK(fabsf(p->grapple.range - range0) < 0.01f, "rope = 200 + height - cameraHeight + 0.001 = %.3f (%.3f)", range0, p->grapple.range);
    CHECK(fabsf(p->reloads[p->loadout_index] - 0.3f) < 1e-4f && p->ammo[p->loadout_index] == g_weapons[WEAPON_ID_GRAPPLER]->ammo,
          "tryGrapple spends the 300 ms fire timer, no ammo");
    CHECK(p->charge_time == 0.0f, "tryGrapple: chargeTime = 0");

    // reel: 0.4 per 10 ms frame; once taut the player follows at 40 units / s
    float z_prev = p->position.z, taut_speed = 0.0f;
    int taut = -1, reeled_exact = 1;
    for (int f = 1; f <= 500 && p->grapple.active; f++) {
        const float before = p->grapple.range;
        frame(p, 0, 0);
        const float expect = before > 35.0f ? before - 0.4f : before;
        if (fabsf(p->grapple.range - expect) > 1e-3f) reeled_exact = 0;
        const float d = dist3(p->position, p->grapple.point);
        if (taut < 0 && d >= p->grapple.range - 0.01f) taut = f;
        if (f == 200) taut_speed = (z_prev - p->position.z) / DT; // units per s, f 199 -> 200
        z_prev = p->position.z;
        if (f == 1 || f == 50 || f == 100 || f == 200 || f == 300 || f == 400 || f == 500)
            printf("  t=%.2f s: rope %.2f, player z %.2f y %.2f, %.2f from the hook\n", f * DT, p->grapple.range, p->position.z, p->position.y, d);
    }
    CHECK(reeled_exact, "the rope reels in 0.04 per ms while above 35");
    CHECK(taut > 0 && taut < 40, "the slack (%.2f) is gone after %d frames", range0 - sqrtf(200.0f * 200.0f + eye_off * eye_off), taut);
    printf("  taut after %d frames, pulled at %.2f units / s\n", taut, taut_speed);
    CHECK(fabsf(taut_speed - 40.0f) < 1.0f, "pulled at 40 units / s (%.2f)", taut_speed);
    CHECK(p->grapple.active && p->grapple.range <= 35.0f && p->grapple.range > 34.5f, "the rope stops at 35 (%.2f)", p->grapple.range);
    CHECK(dist3(p->position, p->grapple.point) <= p->grapple.range + 0.05f, "never further from the hook than the rope (a rope, not a rod)");
    CHECK(p->position.z < -150.0f, "pulled to the wall (z %.1f)", p->position.z);

    // RECON V3 carries it; a V2 one keeps the predicted grapple; grapObj replays it
    NetReconPacket recon;
    net_packet_from_recon(&recon, p, 7);
    NetPacket packet = {.type = NET_PACKET_RECON, .length = (uint16_t) NET_RECON_PACKET_V3_SIZE};
    memcpy(packet.payload, &recon, sizeof(recon));
    NetReconPacket in;
    Player client = {0};
    client.game = game;
    CHECK(net_packet_read_recon(&in, &packet, NULL), "RECON V3 read");
    net_packet_apply_recon(&client, &in);
    CHECK(client.grapple.active && client.grapple.point.z == p->grapple.point.z && client.grapple.range == p->grapple.range, "RECON V3: the grapple");
    packet.length = (uint16_t) NET_RECON_PACKET_V2_SIZE;
    client.grapple.range = 99.0f;
    CHECK(net_packet_read_recon(&in, &packet, NULL) && in.grapple_active == NET_RECON_GRAPPLE_KEEP, "RECON V2: no grapple");
    net_packet_apply_recon(&client, &in);
    CHECK(client.grapple.active && client.grapple.range == 99.0f, "RECON V2 keeps the predicted grapple");

    // jump lets go
    frame(p, 0, 1);
    CHECK(!p->grapple.active, "jump: endGrapple");
    for (int i = 0; i < 100; i++) frame(p, 0, 0);

    // a replayed input with grapObj hooks again (procInputs recon branch)
    Input replay = frame_input(p, 0, 0);
    replay.has_grap_obj = 1;
    replay.grap_obj = (PlayerGrapple) {.active = 1, .point = {0.0f, 9.5f, WALL_Z}, .range = 120.0f};
    player_proc_input(p, &replay, 1, 0);
    CHECK(p->grapple.active && p->grapple.range == 120.0f, "input.grapObj on a replay");
    // a weapon swap lets go
    player_swap_weapon(p, 0, 0, 0, 0);
    CHECK(!p->grapple.active, "swapWeapon: endGrapple");
    equip(p, WEAPON_ID_GRAPPLER);

    // a miss (the sky) spends the shot, no hook
    stand(p, 0.0f, 0.0f);
    for (int i = 0; i < 40; i++) frame(p, 0, 0);
    p->direction.x = 1.2f;
    frame(p, 1, 0);
    CHECK(!p->grapple.active && p->reloads[p->loadout_index] > 0.29f, "a miss: the fire timer, no hook");
    // no auto: holding does not fire again
    for (int i = 0; i < 40; i++) frame(p, 1, 0);
    p->direction.x = 0.0f;
    frame(p, 1, 0);
    CHECK(!p->grapple.active, "nAuto: a held trigger fires once");
    frame(p, 0, 0);

    // "gp" (noGrapple): the hook passes through
    for (size_t i = 0; i < game->map->object_count; i++) {
        if (game->map->objects[i]->position.z < -100.0f) game->map->objects[i]->no_grapple = 1;
    }
    for (int i = 0; i < 40; i++) frame(p, 0, 0);
    frame(p, 1, 0);
    CHECK(!p->grapple.active, "a noGrapple wall takes no hook");
    p->active = 0; // out of the next test
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    // a floor (top at y 0) and a wall whose face is at z = -200
    cJSON *map = cJSON_Parse("{\"name\":\"charge_grapple\",\"spawns\":[[0,0,0],[0,0,-40]],\"objects\":["
                             "{\"p\":[0,-10,0],\"s\":[1200,10,1200]},"
                             "{\"p\":[0,0,-205],\"s\":[600,300,10]}]}");
    static const cJSON *maps[1];
    maps[0] = map;
    Game game = {0};
    game_configure(&game, NULL, maps, 1, NULL, 0, NULL, 0, NULL, 0);
    game_init(&game, 0, -1, 1);
    game.hooks.send = test_send;
    game.lag_comp = 0;

    test_charge_rifle(&game);
    test_grapple(&game);

    cJSON_Delete(map);
    if (g_failures) {
        printf("charge_grapple_test: %d of %d checks failed\n", g_failures, g_checks);
        return 1;
    }
    printf("charge_grapple_test: %d checks OK\n", g_checks);
    return 0;
}
