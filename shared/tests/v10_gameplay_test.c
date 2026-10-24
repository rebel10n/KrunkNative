// v10_gameplay_test: Krunker v10.0.2's gameplay deltas from v7.1.4 (doc/v10/gameplay.md): the
// Boulder's gravMlt, the Compressor's self impulse (left pushes the shooter back, right pulls it
// forward, nobody else moves and nothing takes damage), the Stalker rework (the leap on the aim
// input, its cooldown and coyote time, onKill / onMelee / camping / pickStalker), and the ranked
// rules on a kill. Run from the repository root.
#include <shared.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <ruleset.h>
#include <modes.h>
#include <stalker.h>
#include <net_ext.h>

static int g_failures, g_checks;
#define CHECK(cond, ...) do { g_checks++; \
    if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_failures++; } \
} while (0)

static int g_scares;
static int g_sounds, g_picked, g_picked_target; // PLAYER_SOUND adds, NET_EXT_STALKER_PICKED and its target
static NetPlayerSoundPacket g_sound;
static void test_send(Game *game, const int uid, const int type, const void *payload, const uint16_t length) {
    (void) game;
    if (type == NET_PACKET_JUMP_SCARE) g_scares++;
    if (type == NET_PACKET_PLAYER_SOUND && length == sizeof(g_sound)) {
        memcpy(&g_sound, payload, sizeof(g_sound));
        g_sounds++;
    }
    uint16_t subtype, body_length;
    const uint8_t *body;
    if (type == NET_PACKET_EXT && net_ext_unpack(payload, length, &subtype, &body, &body_length) && subtype == NET_EXT_STALKER_PICKED) {
        g_picked++;
        g_picked_target = uid;
    }
}

static const cJSON *g_test_maps[1];

static void open_game(Game *game) {
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
    open_game(&game);
    Player *p = add_player(&game, 1, 0);
    const float pistol = fall_step(p, WEAPON_ID_PISTOL);
    const float boulder = fall_step(p, WEAPON_ID_BOULDER);
    CHECK(pistol < 0.0f && boulder < 0.0f, "both fall (%g, %g)", (double) pistol, (double) boulder);
    // gravity x weapon gravMlt (v10:169754): 1.7x the Pistol's one tick of gravity (no drag in the air on y)
    CHECK(fabsf(boulder / pistol - 1.7f) < 1e-3f, "the Boulder falls 1.7x (%g / %g = %g)", (double) boulder, (double) pistol,
          (double) (boulder / pistol));
    close_game(&game);
}

static void test_compressor(void) {
    printf("Compressor self impulse...\n");
    Game game;
    open_game(&game);
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
    open_game(&game);
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
}

// The Stalker outside its rules (shared/src/stalker.c): the 300 ms hide, the taunts and STALKER_PICKED.
static void test_stalker_client(void) {
    printf("Stalker hide, taunts, STALKER_PICKED...\n");
    Game game;
    open_game(&game);
    game.mode = mode_init(MODE_ID_STALK);
    Player *stalker = add_player(&game, 1, 0);
    Player *living = add_player(&game, 2, 0);
    stalker->team = 2;
    living->team = 1;
    CHECK(player_is_stalker(&game, stalker) && !player_is_stalker(&game, living), "isStalker: teamOptions[team - 1] == Stalker");

    // isStalker && noMovT >= 300 (deob-10.0.3.js:191767-191772): still time adds up, any velocity resets it
    stalker->velocity = (vec3) {0.0f, 0.0f, 0.0f};
    stalker->no_move_time = 0.0f;
    for (int i = 0; i < 18; i++) stalker_update_hidden(stalker, 0.016f);
    CHECK(!stalker->is_hidden, "still for 288 ms: shown");
    for (int i = 0; i < 2; i++) stalker_update_hidden(stalker, 0.016f);
    CHECK(stalker->is_hidden, "still for 320 ms: hidden");
    stalker->velocity.x = 0.01f;
    stalker_update_hidden(stalker, 0.016f);
    CHECK(!stalker->is_hidden && stalker->no_move_time == 0.0f, "moving again: shown");
    living->velocity = (vec3) {0.0f, 0.0f, 0.0f};
    for (int i = 0; i < 40; i++) stalker_update_hidden(living, 0.016f);
    CHECK(!living->is_hidden, "the Living never hide");
    living->force_hide = 1;
    stalker_update_hidden(living, 0.016f);
    CHECK(living->is_hidden, "forceHide hides anyone");
    living->force_hide = 0;

    // procInputs runs it, not in replays; the stalker stuck to a wall has no velocity (v10:169754-169755)
    stalker->no_move_time = 0.0f;
    stalker->is_hidden = 0;
    stalker->stuck_to_wall = 1;
    stalker->velocity = (vec3) {0.0f, 0.0f, 0.0f};
    Input input = idle_input(stalker);
    input.delta = 0.033f;
    for (int i = 0; i < 12; i++) player_proc_input(stalker, &input, 1, 0);
    CHECK(!stalker->is_hidden && stalker->no_move_time == 0.0f, "replays leave noMovT alone");
    for (int i = 0; i < 12; i++) player_proc_input(stalker, &input, 0, 0);
    CHECK(stalker->is_hidden, "a stalker stuck to a wall for 396 ms is hidden (noMovT %g)", (double) stalker->no_move_time);
    stalker->stuck_to_wall = 0;
    player_spawn(stalker);
    CHECK(!stalker->is_hidden && stalker->no_move_time == 0.0f, "a spawn resets isHidden / noMovT");
    stalker->team = 2;

    // "stlkrt" i: staunt_<i> at the stalker for everyone, 10 s apart (deob-10.0.3.js:198332-198342)
    game.now = 50.0f;
    g_sounds = 0;
    CHECK(!game_player_stalker_taunt(&game, living, 0) && !g_sounds, "the Living cannot taunt");
    CHECK(!game_player_stalker_taunt(&game, stalker, STALKER_TAUNT_COUNT) && !g_sounds, "no taunt past index 5");
    CHECK(game_player_stalker_taunt(&game, stalker, 2) && g_sounds == 1, "the stalker taunts");
    CHECK(g_sound.add && !g_sound.loop && g_sound.uid == stalker->uid && !strcmp(g_sound.name, "staunt_2") && g_sound.volume == 1.0f,
          "asnd staunt_2 vol 1, a one-shot (%s)", g_sound.name);
    game.now = 59.0f;
    CHECK(!game_player_stalker_taunt(&game, stalker, 1) && g_sounds == 1, "not again within 10 s");
    game.now = 60.0f;
    CHECK(game_player_stalker_taunt(&game, stalker, 5) && g_sounds == 2 && !strcmp(g_sound.name, "staunt_5"), "again after 10 s");

    // pickStalker: STALKER_PICKED to everyone but the stalker (broadcastEx, deob-10.0.3.js:217949)
    stalker->team = living->team = 1;
    stalker->active = living->active = 1;
    g_picked = 0;
    game.mode->config.wait_timers[0].trigger(&game, 0.0f);
    Player *picked = stalker->team == 2 ? stalker : living->team == 2 ? living : NULL;
    CHECK(picked && g_picked == 1 && g_picked_target == NET_TARGET_EXCEPT(picked->uid), "STALKER_PICKED to all but the stalker (%d, %d)",
          g_picked, g_picked_target);
    close_game(&game);
}

static void test_ranked_streaks(void) {
    printf("ranked kill streaks...\n");
    Game game;
    open_game(&game);
    game.is_ranked = 1;
    Player *killer = add_player(&game, 1, 0);
    Player *victim = add_player(&game, 2, 0);
    killer->has_account = 1;
    const unsigned int before = killer->kill_streak;
    player_kill(victim, killer, NULL, 1);
    CHECK(killer->kill_streak == before, "ranked: no kill streak (v10:174076)");
    game.is_ranked = 0;
    Player *victim2 = add_player(&game, 3, 0);
    player_kill(victim2, killer, NULL, 1);
    CHECK(killer->kill_streak == before + 1, "unranked: streaks count");
    close_game(&game);
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
    test_stalker_client();
    test_ranked_streaks();

    if (g_failures) {
        printf("v10_gameplay_test: %d of %d checks failed\n", g_failures, g_checks);
        return 1;
    }
    printf("v10_gameplay_test: %d checks OK\n", g_checks);
    return 0;
}
