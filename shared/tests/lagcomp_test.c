// Lag compensation (v3.1.1 players.js storeState / fetchState / getStateConst, shared/src/player.c):
// a target strafing at constant speed, a shooter on a simulated 150 ms RTT aiming where its
// client showed the target. Hits with lag compensation, misses without; the stateHistory window,
// the newest-at-or-before / oldest-entry selection, no hit on an empty history or a dead target,
// nothing moved by a compensated shot, melee, shotgun pellets and projectile stateConst.
// Run from the repository root: ./<build>/lagcomp_test
#include <shared.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

void player_shoot(Player*);  // shared/src/player.c
void player_melee(Player*);

#define DT (1.0 / 32.0)      // a 32 Hz tick: exact in binary, so history times compare exactly
#define RTT_MS 150.0f
#define SPEED 50.0f          // target strafe, units/s
#define DISTANCE 50.0f
#define INTERP_S (1.0 / GAME_LAG_COMP_SEND_RATE * 1.1) // (1000 / cSendRate) * config.interpolation

static int g_failures;

#define CHECK(cond, ...) do { \
    if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_failures++; } \
} while (0)

static void test_send(Game *game, const int uid, const int type, const void *payload, const uint16_t length) {
    (void) game; (void) uid; (void) type; (void) payload; (void) length;
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

// High above the map (nothing in the way), standing still.
static void place(Player *player, const float x, const float z) {
    player->position = (vec3) {x, 1000.0f, z};
    player->velocity = (vec3) {0.0f, 0.0f, 0.0f};
    player->direction = (vec2) {0.0f, 0.0f};
    player->spread = 0.0f;
    player->recoil_anim_y = 0.0f;
}

static double g_t0 = 1.0; // the strafe crosses x = 0 at g_t0

static float target_x(const double t) {
    return (float) (SPEED * (t - g_t0));
}

// Server updates up to `until`: the target strafes along +x at SPEED, both store their state.
static void run_until(Game *game, Player *target, Player *shooter, const double until) {
    while (game->state_now + DT <= until + 1e-9) {
        game->state_now += DT;
        target->position.x = target_x(game->state_now);
        player_store_state(target);
        player_store_state(shooter);
    }
}

// Point the shooter's eye at (x, body, z).
static void aim(Player *shooter, const float x, const float z) {
    const float eye = shooter->position.y + shooter->height - game_constants.camera_height;
    const float dx = x - shooter->position.x, dz = z - shooter->position.z;
    shooter->direction.y = atan2f(dx, dz) - (float) M_PI;              // combat_shot_vector: sin/cos(yaw + PI)
    shooter->direction.x = atanf((1000.0f + 7.0f - eye) / hypotf(dx, dz)); // body height (match_test)
}

static void fire(Player *shooter) {
    shooter->ammo[shooter->loadout_index] = shooter->weapon->ammo;
    shooter->spread = 0.0f;
    shooter->recoil_anim_y = 0.0f;
    player_shoot(shooter);
}

// The scenario: the shooter's input fired at server time g reaches the server at g + RTT/2 and
// waits `queue` s for the tick at `now` (1 s on, a full history of one steady strafe); its
// client was showing the target as of g - RTT/2 - interpolation, about 7 units left of x = 0,
// while the target now is about 7 units right of it. Returns the damage the target took.
static float shot(Game *game, Player *shooter, Player *target, const int lag_comp, const int anchor, const float scale,
                  const double queue, const int melee) {
    const double now = game->state_now + 1.0;
    g_t0 = now - 0.13;
    run_until(game, target, shooter, now);
    const double arrival = game->state_now - queue;
    const double seen = arrival - RTT_MS / 1000.0 - INTERP_S; // = g - RTT/2 - interpolation

    game->lag_comp = (unsigned char) lag_comp;
    game->lag_comp_anchor = (unsigned char) anchor;
    shooter->ping = RTT_MS;
    shooter->lag_comp = scale;
    shooter->input_arrival = arrival;
    target->health = (float) target->max_health;
    target->active = 1; // a previous shot may have killed it

    const vec3 before = target->position;
    const int head = target->state_history_head, count = target->state_history_count;

    aim(shooter, target_x(seen), target->position.z);
    if (melee) player_melee(shooter);
    else fire(shooter);
    shooter->input_arrival = 0.0;

    // nothing to restore: the hit test reads the history, the target never moves
    CHECK(before.x == target->position.x && before.y == target->position.y && before.z == target->position.z,
          "a compensated shot leaves the target where it is");
    CHECK(head == target->state_history_head && count == target->state_history_count, "fetchState leaves the history alone");
    return (float) target->max_health - target->health;
}

static void test_history(Game *game, Player *target, Player *shooter) {
    printf("Running stateHistory window / fetchState selection test...\n");
    game->lag_comp = 1;
    run_until(game, target, shooter, game->state_now + 3.0);

    // entries younger than config.stateHistory (500 ms): ages 0 .. 15/32
    CHECK(target->state_history_count == 16, "500 ms of 32 Hz states: %d entries, want 16", target->state_history_count);

    PlayerStoredState state;
    // back = ping + 110 ms = 160 ms (5.12 ticks): the newest entry at or before, 6 ticks old
    CHECK(players_fetch_state(target, 50.0f, &state) && state.time == game->state_now - 6 * DT,
          "fetchState(50 ms): age %.4f s, want %.4f", game->state_now - state.time, 6 * DT);
    CHECK(state.position.x == target_x(state.time), "the stored x of that update");
    // ping 0 still rewinds the interpolation delay (110 ms -> 4 ticks)
    CHECK(players_fetch_state(target, 0.0f, &state) && state.time == game->state_now - 4 * DT, "fetchState(0): the interpolation delay");
    // beyond the window: the oldest entry (the 500 ms clamp)
    CHECK(players_fetch_state(target, 2000.0f, &state) && state.time == game->state_now - 15 * DT,
          "fetchState(2000 ms) clamps to the oldest entry (age %.4f)", game->state_now - state.time);
    // a lower send rate (servSendR off vs on) means a longer interpolation delay
    target->c_send_rate = 20.0f; // 55 ms -> 2 ticks
    CHECK(players_fetch_state(target, 0.0f, &state) && state.time == game->state_now - 2 * DT, "cSendRate 20: 55 ms");
    target->c_send_rate = GAME_LAG_COMP_SEND_RATE;

    // the ring keeps working past its capacity (128) and across many windows
    run_until(game, target, shooter, game->state_now + 10.0);
    CHECK(target->state_history_count == 16, "still 16 entries after 10 s");
    CHECK(players_fetch_state(target, 0.0f, &state) && state.position.x == target_x(game->state_now - 4 * DT), "ring order after wrapping");

    // without lag compensation (offline, lag_comp 0): the current state
    game->lag_comp = 0;
    target->position.x += 3.0f;
    CHECK(players_fetch_state(target, 2000.0f, &state) && state.position.x == target->position.x && state.height == target->height,
          "lag_comp off: where the target is now");
    target->position.x -= 3.0f;
    game->lag_comp = 1;

    // getStateConst: v3.1.1 ping * 2 * lagComp; anchored: queue time + ping * lagComp
    shooter->ping = 150.0f;
    shooter->lag_comp = 1.0f;
    game->lag_comp_anchor = 0;
    CHECK(fabsf(player_get_state_const(shooter) - 300.0f) < 1e-3f, "literal getStateConst = ping * 2");
    shooter->lag_comp = 0.0f;
    CHECK(fabsf(player_get_state_const(shooter) - 300.0f) < 1e-3f, "lagComp <= 0 counts as 1");
    shooter->lag_comp = 0.5f;
    CHECK(fabsf(player_get_state_const(shooter) - 150.0f) < 1e-3f, "lagComp 0.5 halves it");
    game->lag_comp_anchor = 1;
    shooter->lag_comp = 1.0f;
    shooter->input_arrival = game->state_now - 0.025;
    CHECK(fabsf(player_get_state_const(shooter) - 175.0f) < 1e-2f, "anchored: 25 ms queued + 150 ms ping (%.3f)", player_get_state_const(shooter));
    shooter->input_arrival = 0.0;
    CHECK(fabsf(player_get_state_const(shooter) - 150.0f) < 1e-3f, "anchored without an arrival: one ping");
    shooter->ping = 0.0f;
}

static void test_hitscan(Game *game, Player *target, Player *shooter) {
    printf("Running hitscan lag compensation test (150 ms RTT, %g u/s strafe)...\n", SPEED);
    place(shooter, 0.0f, 0.0f);
    target->position.z = -DISTANCE;

    // where the client saw the target is 13 units (0.26 s) behind where it is now
    const float off = shot(game, shooter, target, 0, 1, 1.0f, 0.02, 0);
    CHECK(off == 0.0f, "lag_comp off: the shot at where the client saw the target misses (%.1f dmg)", off);

    const float anchored = shot(game, shooter, target, 1, 1, 1.0f, 0.02, 0);
    CHECK(anchored > 0.0f, "anchored lag compensation: hit");
    // queued for most of a tick: still anchored to the input
    CHECK(shot(game, shooter, target, 1, 1, 1.0f, 0.03, 0) > 0.0f, "anchored, 30 ms in the queue: hit");

    // literal v3.1.1 (ping * 2 * lagComp): two RTTs back, 150 ms too far at this RTT
    CHECK(shot(game, shooter, target, 1, 0, 1.0f, 0.02, 0) == 0.0f, "literal v3.1.1 at lagComp 1 over-rewinds: miss");
    // lagComp 0.5 makes it one RTT, measured from the tick instead of the arrival
    CHECK(shot(game, shooter, target, 1, 0, 0.5f, 0.01, 0) > 0.0f, "literal v3.1.1 at lagComp 0.5: hit");

    // the shooter is never rewound: it strafed too, the ray starts where it is now
    shooter->position.x = 30.0f;
    run_until(game, target, shooter, game->state_now + 1.0); // its history now holds x = 30
    shooter->position.x = 0.0f;
    CHECK(shot(game, shooter, target, 1, 1, 1.0f, 0.02, 0) > 0.0f, "the shooter's own position stays current");
}

static void test_dead_and_spawn(Game *game, Player *target, Player *shooter) {
    printf("Running dead target / fresh spawn test...\n");
    const double now = game->state_now + 1.0;
    g_t0 = now - 0.13;

    // dead now, alive 260 ms ago: v3.1.1 tests only active players, no resurrection
    run_until(game, target, shooter, now);
    target->active = 0;
    const double arrival = game->state_now - 0.02;
    const double seen = arrival - RTT_MS / 1000.0 - INTERP_S;
    game->lag_comp = 1;
    game->lag_comp_anchor = 1;
    shooter->ping = RTT_MS;
    shooter->input_arrival = arrival;
    target->health = 0.0f;
    aim(shooter, target_x(seen), target->position.z);
    fire(shooter);
    CHECK(target->health == 0.0f && !target->active, "a dead target is not hit at its old position");
    target->active = 1;

    // spawn clears stateHistory (players.js init); until the next storeState there is no box
    player_spawn(target);
    place(target, 0.0f, -DISTANCE);
    CHECK(target->state_history_count == 0, "spawn clears the history");
    PlayerStoredState state;
    CHECK(!players_fetch_state(target, 0.0f, &state), "empty history: fetchState finds nothing");
    target->health = (float) target->max_health;
    shooter->input_arrival = 0.0;
    aim(shooter, target->position.x, target->position.z);
    fire(shooter);
    CHECK(target->health == (float) target->max_health, "no hit before the first storeState (v3.1.1 `if (G = fetchState())`)");

    // one update later the spawn position is all there is: the oldest-entry fallback
    game->state_now += DT;
    player_store_state(target);
    player_store_state(shooter);
    fire(shooter);
    CHECK(target->health < (float) target->max_health, "after the first storeState the spawn position is hittable");
    shooter->ping = 0.0f;
}

static void test_shotgun_melee(Game *game, Player *target) {
    printf("Running shotgun pellets / melee test...\n");
    Player *vince = add_player(game, 3, 4); // Vince: Shotgun
    CHECK(vince->weapon && !strcmp(vince->weapon->name, "Shotgun"), "Vince holds the Shotgun (%s)", vince->weapon ? vince->weapon->name : "none");
    place(vince, 0.0f, 0.0f);
    target->position.z = -30.0f;

    CHECK(shot(game, vince, target, 0, 1, 1.0f, 0.02, 0) == 0.0f, "shotgun, lag_comp off: every pellet misses");
    CHECK(shot(game, vince, target, 1, 1, 1.0f, 0.02, 0) > 0.0f, "shotgun, lag compensation: pellets hit");

    // melee (players.js melee: fetchState as well): the knife at 10 units
    int melee_slot = -1;
    for (int i = 0; i < vince->loadout_size; i++) {
        const int index = vince->loadout[i];
        if (index >= 0 && index < WEAPON_COUNT && g_weapons[index]->melee) melee_slot = i;
    }
    CHECK(melee_slot >= 0, "a melee slot");
    if (melee_slot >= 0) {
        player_swap_weapon(vince, melee_slot, 1, 1, 0);
        vince->can_throw = 1;
        vince->aim_val = 0.0f; // a swing, not a throw
        target->position.z = -10.0f;
        CHECK(shot(game, vince, target, 0, 1, 1.0f, 0.02, 1) == 0.0f, "knife, lag_comp off: miss");
        CHECK(shot(game, vince, target, 1, 1, 1.0f, 0.02, 1) > 0.0f, "knife, lag compensation: hit");
    }
    vince->active = 0;
}

static void test_projectile_state_const(Game *game, Player *shooter) {
    printf("Running projectile stateConst test...\n");
    const Weapon *rocket = NULL, *crossbow = NULL;
    for (int i = 0; i < WEAPON_COUNT; i++) {
        if (!strcmp(g_weapons[i]->name, "Rocket Launcher")) rocket = g_weapons[i];
        if (!strcmp(g_weapons[i]->name, "Crossbow")) crossbow = g_weapons[i];
    }
    CHECK(rocket && crossbow, "rocket and crossbow weapons");
    if (!rocket || !crossbow) return;

    game->lag_comp_anchor = 0;
    shooter->ping = 100.0f;
    shooter->lag_comp = 1.0f;
    const vec3 origin = shooter->position;
    game_spawn_projectile(game, shooter, 0, projectile_config_for(rocket, 0), origin, 0.0f, 0.0f, 0);
    game_spawn_projectile(game, shooter, 0, projectile_config_for(crossbow, 0), origin, 0.0f, 0.0f, 0);
    game_spawn_projectile(game, NULL, -1, projectile_config_for(rocket, 0), origin, 0.0f, 0.0f, 0);
    CHECK(game->projectiles[0].active && fabsf(game->projectiles[0].state_const - 200.0f) < 1e-3f, "rocket: stateConst = ping * 2 * lagComp");
    CHECK(game->projectiles[1].active && game->projectiles[1].state_const == 0.0f, "crossbow (dontPred): stateConst 0");
    CHECK(game->projectiles[2].active && game->projectiles[2].state_const == 0.0f, "no owner: stateConst 0");
    for (int i = 0; i < 3; i++) game->projectiles[i].active = 0;
    shooter->ping = 0.0f;
}

int main(void) {
    cJSON *sandstorm = NULL;
    if (load_json_file("assets/maps/sandstorm.json", &sandstorm) && load_json_file("../assets/maps/sandstorm.json", &sandstorm)) {
        printf("Failed to load assets/maps/sandstorm.json (run from the repository root)\n");
        return 1;
    }

    static const cJSON *maps[1];
    maps[0] = sandstorm;

    Game game = {0};
    game_configure(&game, NULL, maps, 1, NULL, 0, NULL, 0, NULL, 0);
    game_init(&game, 0, -1, 1); // is_local: the authority; lag_comp is set per test as the server would
    game.hooks.send = test_send;
    if (!game.ready) {
        printf("Failed to init the game\n");
        return 1;
    }

    Player *shooter = add_player(&game, 1, 0); // Triggerman: Assault Rifle
    Player *target = add_player(&game, 2, 0);
    place(shooter, 0.0f, 0.0f);
    place(target, 0.0f, -DISTANCE);

    test_history(&game, target, shooter);
    test_hitscan(&game, target, shooter);
    test_dead_and_spawn(&game, target, shooter);
    test_shotgun_melee(&game, target);
    test_projectile_state_const(&game, shooter);

    if (g_failures) {
        printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("Test OK\n");
    return 0;
}
