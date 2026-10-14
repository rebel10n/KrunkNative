// v10_gameplay_test: Krunker v10.0.2's gameplay deltas that only the v10 ruleset may show
// (doc/v10/gameplay.md): the Boulder's gravMlt, the Compressor's self impulse (left pushes the
// shooter back, right pulls it forward, nobody else moves and nothing takes damage), the Stalker
// rework (the leap on the aim input, its cooldown and coyote time, onKill / onMelee / camping /
// pickStalker), and the ranked rules on a kill. Each check also runs under v7 where v7 must stay
// as it was. Run from the repository root.
#include <shared.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <ruleset.h>
#include <modes.h>

static int g_failures, g_checks;
#define CHECK(cond, ...) do { g_checks++; \
    if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_failures++; } \
} while (0)

static int g_scares;
static void test_send(Game *game, const int uid, const int type, const void *payload, const uint16_t length) {
    (void) game; (void) uid; (void) payload; (void) length;
    if (type == NET_PACKET_JUMP_SCARE) g_scares++;
}

static const cJSON *g_test_maps[1];

static void open_game(Game *game, const RulesetId rules) {
    CHECK(ruleset_set_current(rules), "ruleset %d", (int) rules);
    memset(game, 0, sizeof(*game));
    game_configure(game, NULL, g_test_maps, 1, NULL, 0, NULL, 0, NULL, 0);
    game_init(game, 0, -1, 1);
    game->hooks.send = test_send;
    game->lag_comp = 0;
}

static void close_game(Game *game) {
    if (game->mode) mode_fini(game->mode);
    game->mode = NULL;
    free(game->weapons);
    free(game->classes);
    free(game->modes);
}

static Player *add_player(Game *game, const int uid, const int class_index) {
    Player *player = player_init(game);
    player->uid = uid;
    player->class_index = class_index;
    snprintf(player->name, sizeof(player->name), "p%d", uid);
    game_players_add(game, player);
    player_spawn(player);
    player->position = (vec3) {0.0f, 1000.0f, 0.0f}; // far above the map: no geometry
    player->velocity = (vec3) {0.0f, 0.0f, 0.0f};
    player->direction = (vec2) {0.0f, 0.0f};
    player->swap_timer = 0.0f;
    return player;
}

static void hold(Player *player, const int weapon_id) {
    player->loadout[0] = weapon_id;
    player->ammo[0] = (unsigned int) (player->game->weapons[weapon_id]->ammo ? player->game->weapons[weapon_id]->ammo : 1);
    player_swap_weapon(player, 0, 1, 1, 0);
    player->swap_timer = 0.0f;
    player->reloads[0] = 0.0f;
    player->reload_timer = 0.0f;
}

static Input idle_input(const Player *player) {
    Input input = {0};
    input.move_dir = -1;
    input.delta = 0.016f;
    input.x_dir = player->direction.x;
    input.y_dir = player->direction.y;
    return input;
}

// One airborne tick's vertical velocity change for a still player holding `weapon_id`.
static float fall_step(Player *player, const int weapon_id) {
    hold(player, weapon_id);
    player->position = (vec3) {0.0f, 1000.0f, 0.0f};
    player->velocity = (vec3) {0.0f, 0.0f, 0.0f};
    player->on_ground = 0;
    const Input input = idle_input(player);
    player_proc_input(player, &input, 0, 0);
    return player->velocity.y;
}

static void test_boulder(void) {
    printf("Boulder gravMlt...\n");
    Game game;
    open_game(&game, RULESET_V10_0_2);
    Player *p = add_player(&game, 1, 0);
    const float pistol = fall_step(p, WEAPON_ID_PISTOL);
    const float boulder = fall_step(p, WEAPON_ID_BOULDER);
    CHECK(pistol < 0.0f && boulder < 0.0f, "both fall (%g, %g)", (double) pistol, (double) boulder);
    // gravity x weapon gravMlt (v10:169754): 1.7x the Pistol's one tick of gravity (no drag in the air on y)
    CHECK(fabsf(boulder / pistol - 1.7f) < 1e-3f, "the Boulder falls 1.7x (%g / %g = %g)", (double) boulder, (double) pistol,
          (double) (boulder / pistol));
    close_game(&game);

    // v7 has no Boulder (id 29 is a hole until the KN base) and its fall is the plain one
    open_game(&game, RULESET_V7_1_4);
    CHECK(!rules_weapon(WEAPON_ID_BOULDER) && !rules_weapon(WEAPON_ID_COMPRESSOR), "v7 has neither new weapon");
    Player *q = add_player(&game, 1, 0);
    CHECK(fabsf(fall_step(q, WEAPON_ID_PISTOL) - pistol) < 1e-7f, "v7 and v10 fall alike without a gravMlt weapon");
    close_game(&game);
}

static void test_compressor(void) {
    printf("Compressor self impulse...\n");
    Game game;
    open_game(&game, RULESET_V10_0_2);
    Player *shooter = add_player(&game, 1, 0);
    Player *other = add_player(&game, 2, 0);
    other->position = (vec3) {0.0f, 1000.0f, -10.0f}; // straight ahead (yaw 0 looks down -z)
    hold(shooter, WEAPON_ID_COMPRESSOR);

    // left click: the shooter is pushed back along the shot by physPow 0.1 (v10:172807-172818)
    Input input = idle_input(shooter);
    input.shoot = 1;
    shooter->on_ground = 0;
    const float other_health = other->health;
    player_proc_input(shooter, &input, 0, 0);
    CHECK(shooter->ammo[0] == 0, "the shot spent the round");
    CHECK(shooter->velocity.z > 0.09f, "left click pushes the shooter back (+z): %g", (double) shooter->velocity.z);
    CHECK(other->velocity.x == 0.0f && other->velocity.y == 0.0f && other->velocity.z == 0.0f && other->health == other_health,
          "nobody else moves or takes damage (noDmg, nonObjImpulse)");

    // right click (rShoot, the aim input): physPull pulls the shooter forward (x -1, v10:172832)
    hold(shooter, WEAPON_ID_COMPRESSOR);
    shooter->velocity = (vec3) {0.0f, 0.0f, 0.0f};
    shooter->did_shoot = 0;
    input = idle_input(shooter);
    input.scope = 1;
    player_proc_input(shooter, &input, 0, 0);
    CHECK(shooter->ammo[0] == 0, "the aim input fires the Compressor (rShoot)");
    CHECK(shooter->velocity.z < -0.09f, "right click pulls the shooter forward (-z): %g", (double) shooter->velocity.z);
    CHECK(other->velocity.z == 0.0f && other->health == other_health, "still nobody else");

    // a v10 weapon without rShoot does not fire on the aim input
    hold(shooter, WEAPON_ID_PISTOL);
    const unsigned int ammo = shooter->ammo[0];
    shooter->did_shoot = 0;
    input = idle_input(shooter);
    input.scope = 1;
    player_proc_input(shooter, &input, 0, 0);
    CHECK(shooter->ammo[0] == ammo, "the Pistol does not fire on the aim input");
    close_game(&game);
}

static void test_stalker(void) {
    printf("Stalker rework...\n");
    Game game;
    open_game(&game, RULESET_V10_0_2);
    game.mode = mode_init(MODE_ID_STALK);
    CHECK(game.mode && game.mode->config.stalker_v10, "the v10 Stalker mode");
    Player *stalker = add_player(&game, 1, 0);
    Player *living = add_player(&game, 2, 0);
    Player *living2 = add_player(&game, 3, 0);
    stalker->team = 2;
    living->team = 1;
    living2->team = 1;
    hold(stalker, WEAPON_ID_KNIFE);
    hold(living, WEAPON_ID_PISTOL);

    // the leap: the aim input (input[6]) on the ground, 0.15 along the look direction (v10:169664-169685)
    game.now = 100.0f;
    stalker->last_stalker_leap = 0.0f;
    stalker->on_ground = 1;
    stalker->velocity = (vec3) {0.0f, 0.0f, 0.0f};
    Input input = idle_input(stalker);
    input.scope = 1;
    player_proc_input(stalker, &input, 0, 0);
    CHECK(stalker->last_stalker_leap == 100.0f, "the stalker leapt");
    CHECK(stalker->velocity.z < -0.1f, "the leap goes along the look direction (-z): %g", (double) stalker->velocity.z);

    // 3 s cooldown
    stalker->on_ground = 1;
    stalker->velocity = (vec3) {0.0f, 0.0f, 0.0f};
    game.now = 102.0f;
    player_proc_input(stalker, &input, 0, 0);
    CHECK(stalker->last_stalker_leap == 100.0f, "no second leap within 3 s");
    game.now = 103.5f;
    stalker->on_ground = 1;
    player_proc_input(stalker, &input, 0, 0);
    CHECK(stalker->last_stalker_leap == 103.5f, "a leap again after 3 s");

    // coyote time: 150 ms after leaving the ground the leap still works (v10:170078)
    stalker->stalker_coyote_time = 0.1f;
    stalker->on_ground = 0;
    game.now = 110.0f;
    player_proc_input(stalker, &input, 0, 0);
    CHECK(stalker->last_stalker_leap == 110.0f, "a leap within the coyote time");
    stalker->stalker_coyote_time = 0.0f;
    stalker->on_ground = 0;
    game.now = 120.0f;
    player_proc_input(stalker, &input, 0, 0);
    CHECK(stalker->last_stalker_leap == 110.0f, "no leap in the air after the coyote time");

    // the Living do not leap
    living->on_ground = 1;
    living->velocity = (vec3) {0.0f, 0.0f, 0.0f};
    input = idle_input(living);
    input.scope = 1;
    player_proc_input(living, &input, 0, 0);
    CHECK(living->last_stalker_leap == 0.0f, "the Living cannot leap");

    // onKill: the stalker's kill jump-scares the victim and heals the stalker 50 (v10:104388)
    stalker->max_health = 400;
    stalker->health = 100.0f;
    g_scares = 0;
    game.mode->config.on_kill(&game, living2, stalker, WEAPON_ID_KNIFE);
    CHECK(g_scares == 1 && stalker->health == 150.0f, "a stalker kill: scare + 50 HP (%g, %d)", (double) stalker->health, g_scares);
    game.mode->config.on_kill(&game, stalker, living, WEAPON_ID_PISTOL);
    CHECK(g_scares == 1, "a Living kill scares nobody");

    // onMelee as a damage factor (v10:104385, 172543): the stalker x0.6 and +10 HP, anyone else x0.8
    stalker->health = 100.0f;
    CHECK(fabsf(mode_melee_damage(&game, stalker, living, 50.0f) - 30.0f) < 1e-4f && stalker->health == 110.0f, "the stalker's knife x0.6, +10 HP");
    CHECK(fabsf(mode_melee_damage(&game, living, stalker, 50.0f) - 40.0f) < 1e-4f, "a Living knife x0.8");

    // camping (stalk.update, v10:104392): 20 s within 200 of the camp origin turns a Living Dizzy,
    // 2 s outside clears it
    game.mode->wait_timer_count = 0;
    living->active = 1;
    living->position = (vec3) {0.0f, 1000.0f, 0.0f};
    living->stalker_camp.has_origin = 0;
    for (int i = 0; i < 21; i++) {
        game.now += 1.0f;
        game.mode->config.update(&game, 1.0f);
    }
    CHECK(living->effects[EFFECT_DIZZY] > 0 && living->stalker_camp.has_effect, "20 s camping: Dizzy");
    living->position.x = 300.0f;
    for (int i = 0; i < 3; i++) {
        game.now += 1.0f;
        game.mode->config.update(&game, 1.0f);
    }
    CHECK(living->effects[EFFECT_DIZZY] == 0 && !living->stalker_camp.has_effect, "2 s away: Dizzy cleared");

    // pickStalker: floor(200 + 50 x (players - 1)) (v10:188466)
    for (size_t i = 0; i < game.player_count; i++) {
        game.players[i]->team = 1;
        game.players[i]->active = 1;
    }
    game.mode->config.wait_timers[0].trigger(&game, 0.0f);
    Player *picked = NULL;
    for (size_t i = 0; i < game.player_count; i++)
        if (game.players[i]->team == 2) picked = game.players[i];
    CHECK(picked && picked->max_health == 200 + 50 * 2, "3 players: the stalker gets 300 HP (%d)", picked ? picked->max_health : -1);
    close_game(&game);

    // v7: no leap, no camping, 400 HP stalker, the old instant melee
    open_game(&game, RULESET_V7_1_4);
    game.mode = mode_init(MODE_ID_STALK);
    CHECK(game.mode && !game.mode->config.stalker_v10 && !game.mode->config.on_kill && !game.mode->config.update, "the v7 Stalker mode is untouched");
    Player *s7 = add_player(&game, 1, 0);
    s7->team = 2;
    hold(s7, WEAPON_ID_KNIFE);
    s7->on_ground = 1;
    game.now = 100.0f;
    input = idle_input(s7);
    input.scope = 1;
    player_proc_input(s7, &input, 0, 0);
    CHECK(s7->last_stalker_leap == 0.0f, "v7: the stalker has no leap");
    close_game(&game);
}

static void test_ranked_streaks(void) {
    printf("ranked kill streaks...\n");
    for (int pass = 0; pass < 2; pass++) {
        const RulesetId rules = pass ? RULESET_V7_1_4 : RULESET_V10_0_2;
        Game game;
        open_game(&game, rules);
        game.is_ranked = 1;
        Player *killer = add_player(&game, 1, 0);
        Player *victim = add_player(&game, 2, 0);
        killer->has_account = 1;
        const unsigned int before = killer->kill_streak;
        player_kill(victim, killer, NULL, 1);
        if (rules == RULESET_V10_0_2) CHECK(killer->kill_streak == before, "v10 ranked: no kill streak (v10:174076)");
        else CHECK(killer->kill_streak == before + 1, "v7 ranked: streaks count as before");
        game.is_ranked = 0;
        Player *victim2 = add_player(&game, 3, 0);
        player_kill(victim2, killer, NULL, 1);
        CHECK(killer->kill_streak == (rules == RULESET_V10_0_2 ? before + 1 : before + 2), "unranked: streaks count");
        close_game(&game);
    }
}

int main(void) {
    cJSON *sandstorm = NULL;
    if (load_json_file("assets/maps/311_sandstorm.json", &sandstorm) && load_json_file("../assets/maps/311_sandstorm.json", &sandstorm)) {
        printf("Failed to load assets/maps/311_sandstorm.json (run from the repository root)\n");
        return 1;
    }
    g_test_maps[0] = sandstorm;

    test_boulder();
    test_compressor();
    test_stalker();
    test_ranked_streaks();
    ruleset_set_current(RULESET_V7_1_4);

    if (g_failures) {
        printf("v10_gameplay_test: %d of %d checks failed\n", g_failures, g_checks);
        return 1;
    }
    printf("v10_gameplay_test: %d checks OK\n", g_checks);
    return 0;
}
