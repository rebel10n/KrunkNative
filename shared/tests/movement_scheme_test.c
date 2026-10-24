// Krunker v10.0.3 movement schemes (the class movSch, game.movement in deob-10.0.3.js:18553-19199;
// shared/src/player.c, doc/v10/movement-schemes.md): every scheme driven through player_proc_input with
// the motion v10's code gives, plus the scheme keys (SRM's noThrow / airDecel), the camType 1 camera
// offset of Default.mov, and the sendVals sync (a reconciliation replays from the server's scheme state).
// The fixture is shared/tests/maps/ramp_fix.json: an 800 x 800 floor (top y 0) and a 20 x 6 x 20 ramp
// (direction 0) at the origin. Run from the repository root: ./<build>/movement_scheme_test
#include <world.h>
#include <game_config.h>
#include <ruleset.h> // rules_weapon
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

// KrunkNative Input.move_dir: 0 W, 1 W+D, 2 D, 3 S+D, 4 S, 5 S+A, 6 A, 7 W+A, -1 none
enum { K_NONE = -1, K_W = 0, K_WD = 1, K_D = 2, K_S = 4, K_A = 6 };

static void test_send(Game *game, const int uid, const int type, const void *payload, const uint16_t length) {
    (void) game, (void) uid, (void) type, (void) payload, (void) length;
}

static cJSON *g_map;

// An authority game in mode `mode_id` on the fixture whose class 0 (Triggerman) gets `cls` (a JSON
// object of classConfig keys, e.g. {"movSch":6}).
static void game_on_mode(Game *game, const char *cls, const int mode_id) {
    static const cJSON *maps[1];
    maps[0] = g_map;
    memset(game, 0, sizeof(*game));
    int *modes = malloc(sizeof(int));
    modes[0] = mode_id;
    game_configure(game, NULL, maps, 1, modes, 1, NULL, 0, NULL, 0);
    game->hooks.send = test_send;
    game_init(game, 0, 0, 1);
    char json[512];
    snprintf(json, sizeof(json), "{\"cls\":{\"0\":%s}}", cls);
    cJSON *cfg = cJSON_Parse(json);
    game_config_apply(game, cfg, GAME_CONFIG_CLAMP_HOSTED, GAME_HOST_CUSTOM);
    cJSON_Delete(cfg);
}

static void game_on(Game *game, const char *cls) {
    game_on_mode(game, cls, MODE_ID_FFA);
}

static Player *add_player(Game *game, const int uid, const float x, const float y, const float z) {
    Player *player = player_init(game);
    player->uid = uid;
    player->class_index = 0;
    snprintf(player->name, sizeof(player->name), "P%d", uid);
    game_players_add(game, player);
    player_spawn(player);
    player->position = (vec3) {x, y, z};
    player->last_position = player->position;
    player->velocity = (vec3) {0.0f, 0.0f, 0.0f};
    player->direction = (vec2) {0.0f, 0.0f};
    return player;
}

static Input make_input(const Player *p, const int move_dir, const float yaw, const float pitch, const int jump, const int scope) {
    Input in = {.seq = p->input_seq + 1, .move_dir = move_dir, .delta = 1.0f / 60.0f, .x_dir = pitch, .y_dir = yaw};
    in.jump = (unsigned char) jump;
    in.scope = (unsigned char) scope;
    return in;
}

// One 60 fps input frame (17 ms after procInputs' rounding)
static void tick(Player *p, const int move_dir, const float yaw, const float pitch, const int jump, const int scope) {
    const Input in = make_input(p, move_dir, yaw, pitch, jump, scope);
    player_proc_input(p, &in, 0, 0);
}

static void ticks(Player *p, const int n, const int move_dir, const float yaw, const float pitch) {
    for (int i = 0; i < n; i++) tick(p, move_dir, yaw, pitch, 0, 0);
}

static float flat_speed(const vec3 v) {
    return sqrtf(v.x * v.x + v.z * v.z);
}

static void test_default(void) {
    printf("0 Default: keys relative to the look; camType 1 without camRot uses the camera offset...\n");
    Game game;
    game_on(&game, "{\"movSch\":0}");
    Player *p = add_player(&game, 1, -200.0f, 0.0f, -200.0f);
    CHECK(player_movement_scheme(p) == MOVEMENT_SCHEME_DEFAULT && player_direction_is_look(p) && player_look_follows_camera(p), "Default follows the look");
    ticks(p, 30, K_W, 0.0f, 0.0f);
    CHECK(p->position.z < -203.0f && fabsf(p->position.x + 200.0f) < 0.01f, "W at yaw 0 walks -z (%.3f, %.3f)", p->position.x, p->position.z);
    Player *q = add_player(&game, 2, 200.0f, 0.0f, -200.0f);
    ticks(q, 30, K_W, (float) M_PI / 2.0f, 0.0f);
    CHECK(q->position.x < 197.0f && fabsf(q->position.z + 200.0f) < 0.01f, "W at yaw PI/2 walks -x (%.3f, %.3f)", q->position.x, q->position.z);

    // Default.mov (deob-10.0.3.js:18591-18594): camType 1 && !camRot -> origin = -getDir(camOffX, camOffZ, 0, 0) % PI
    Game cam;
    game_on(&cam, "{\"movSch\":0,\"camType\":1,\"camRot\":false,\"camOffZ\":20}");
    Player *c = add_player(&cam, 3, -200.0f, 0.0f, 200.0f);
    ticks(c, 30, K_W, 1.0f, 0.0f); // the look is ignored: origin -PI/2, so W (movDir -PI/2) is +x
    CHECK(c->position.x > -197.0f && fabsf(c->position.z - 200.0f) < 0.01f, "3rd Person, no mouse rotation: W walks +x whatever the look (%.3f, %.3f)", c->position.x, c->position.z);
}

static void test_follow_mouse(void) {
    printf("1 Follow Mouse: always runs toward the look, the keys unused...\n");
    Game game;
    game_on(&game, "{\"movSch\":1}");
    Player *p = add_player(&game, 1, -200.0f, 0.0f, -200.0f);
    ticks(p, 30, K_NONE, 0.0f, 0.0f);
    CHECK(p->position.z < -203.0f && fabsf(p->position.x + 200.0f) < 0.01f, "no keys at yaw 0: runs -z (%.3f, %.3f)", p->position.x, p->position.z);
    Player *q = add_player(&game, 2, 200.0f, 0.0f, 200.0f);
    ticks(q, 30, K_D, (float) M_PI / 2.0f, 0.0f);
    CHECK(q->position.x < 197.0f && fabsf(q->position.z - 200.0f) < 0.01f, "D at yaw PI/2 still runs along the look, -x (%.3f, %.3f)", q->position.x, q->position.z);
}

static void test_move_right(void) {
    printf("2 Move Right: velocity.x 0.01 every tick...\n");
    Game game;
    game_on(&game, "{\"movSch\":2}");
    Player *p = add_player(&game, 1, -200.0f, 0.0f, -200.0f);
    ticks(p, 30, K_A, 2.0f, 0.0f);
    // 0.01 per ms, decayed 0.99^17 by Default.vel after the step: x += 0.01 * 17 every tick
    CHECK(fabsf(p->position.x - (-200.0f + 30 * 0.17f)) < 0.05f && fabsf(p->position.z + 200.0f) < 0.01f, "+x 0.17 a tick, keys and look unused (%.3f, %.3f)",
          p->position.x, p->position.z);
}

static void test_fly(void) {
    printf("3 Fly: along the look, pitch included...\n");
    Game game;
    game_on(&game, "{\"movSch\":3,\"arA\":0.1}");
    Player *p = add_player(&game, 1, -200.0f, 100.0f, -200.0f);
    Game def;
    game_on(&def, "{\"movSch\":0,\"arA\":0.1}");
    Player *d = add_player(&def, 2, -200.0f, 100.0f, -200.0f);
    for (int i = 0; i < 30; i++) {
        tick(p, K_NONE, 0.0f, 1.2f, 0, 0);
        tick(d, K_NONE, 0.0f, 1.2f, 0, 0);
    }
    CHECK(p->position.y > 100.0f && d->position.y < 99.0f, "pitched up it climbs, Default falls (fly y %.3f, default y %.3f)", p->position.y, d->position.y);
    CHECK(p->position.z < -200.5f && fabsf(p->position.x + 200.0f) < 0.01f, "and moves along the yaw, -z (%.3f, %.3f)", p->position.x, p->position.z);
}

static void test_dir_only(void) {
    printf("4 Dir Only: the keys along the world axes...\n");
    Game game;
    game_on(&game, "{\"movSch\":4}");
    Player *p = add_player(&game, 1, -200.0f, 0.0f, -200.0f);
    ticks(p, 30, K_W, (float) M_PI / 2.0f, 0.0f);
    CHECK(p->position.z < -203.0f && fabsf(p->position.x + 200.0f) < 0.01f, "W looking -x still walks -z (%.3f, %.3f)", p->position.x, p->position.z);
    Player *q = add_player(&game, 2, 200.0f, 0.0f, 200.0f);
    ticks(q, 30, K_D, 2.5f, 0.0f);
    CHECK(q->position.x > 203.0f && fabsf(q->position.z - 200.0f) < 0.01f, "D walks +x (%.3f, %.3f)", q->position.x, q->position.z);
}

static void test_srm(void) {
    printf("5 SRM: ground speed, dash, auto bhop, noThrow, airDecel 1, ramp bounce...\n");
    Game game;
    game_on(&game, "{\"movSch\":5}");
    Game def;
    game_on(&def, "{\"movSch\":0}");

    // GroundMove: Accelerate adds up to (baseSpeed 0.07 + 17 / 1000) - |tmpVel| after ApplyFriction took
    // 0.006 * 17 of the speed, so the ground speed settles at 0.087 / (1 + 0.102) = 0.0789 along the keys
    Player *p = add_player(&game, 1, -200.0f, 0.0f, -200.0f);
    ticks(p, 120, K_W, 0.0f, 0.0f);
    const float speed = flat_speed(p->scheme.tmp_vel);
    CHECK(fabsf(speed - 0.087f / 1.102f) < 2e-4f && p->scheme.tmp_vel.z < 0.0f, "ground speed 0.0789 along -z (%.5f, tmp z %.4f)", speed, p->scheme.tmp_vel.z);
    CHECK(p->scheme.on_ground_ms >= 120 * 17.0f - 1.0f, "onGroundFrames counts the grounded ms (%.1f)", p->scheme.on_ground_ms);

    // Dash: the aim key on the ground: velocity along the look at >= 0.1 + 17 / 1500, y 0.035, 1 s cooldown
    Player *d = add_player(&game, 2, 200.0f, 0.0f, -200.0f);
    ticks(d, 10, K_NONE, (float) M_PI / 2.0f, 0.0f);
    tick(d, K_NONE, (float) M_PI / 2.0f, 0.0f, 0, 1);
    CHECK(d->scheme.dash_timer == 1000.0f && d->scheme.wish_dash == 0.0f, "dash: cooldown 1000 ms, wish spent (%.1f, %.1f)", d->scheme.dash_timer, d->scheme.wish_dash);
    // 0.1 + 17 / 1500 = 0.11133 along the look, then the same tick's ground friction (x 0.898)
    CHECK(fabsf(d->scheme.tmp_vel.x + 0.11133f * 0.898f) < 2e-4f && fabsf(d->scheme.tmp_vel.z) < 1e-4f && d->velocity.y > 0.03f,
          "dash along the look -x at 0.1113 (less a tick of friction), up 0.035 (tmp %.5f %.4f, vy %.4f)", d->scheme.tmp_vel.x, d->scheme.tmp_vel.z, d->velocity.y);
    tick(d, K_NONE, (float) M_PI / 2.0f, 0.0f, 0, 1);
    CHECK(d->scheme.dash_timer > 900.0f && d->scheme.dash_timer < 1000.0f, "no second dash in the cooldown (%.1f)", d->scheme.dash_timer);

    // auto bhop: the jump key held lands into a new hop every time (Default needs a release)
    Player *b = add_player(&game, 3, 200.0f, 0.0f, 200.0f);
    Player *n = add_player(&def, 4, 200.0f, 0.0f, 200.0f);
    int srm_jumps = 0, def_jumps = 0;
    for (int i = 0; i < 240; i++) {
        const float vy_b = b->velocity.y, vy_n = n->velocity.y;
        tick(b, K_NONE, 0.0f, 0.0f, 1, 0);
        tick(n, K_NONE, 0.0f, 0.0f, 1, 0);
        srm_jumps += b->velocity.y > 0.05f && vy_b <= 0.0f;
        def_jumps += n->velocity.y > 0.05f && vy_n <= 0.0f;
    }
    CHECK(srm_jumps >= 3 && def_jumps == 1, "held jump: SRM hops on every landing (%d), Default once (%d)", srm_jumps, def_jumps);

    // noThrow: the aim key never scopes (it dashes)
    Player *a = add_player(&game, 5, -200.0f, 0.0f, 200.0f);
    Player *m = add_player(&def, 6, -200.0f, 0.0f, 200.0f);
    for (int i = 0; i < 20; i++) {
        tick(a, K_NONE, 0.0f, 0.0f, 0, 1);
        tick(m, K_NONE, 0.0f, 0.0f, 0, 1);
    }
    CHECK(a->aim_val == 0.0f && m->aim_val > 0.0f, "SRM noThrow: no aiming (SRM %.3f, Default %.3f)", a->aim_val, m->aim_val);

    // airDecel 1: an airborne SRM player keeps its speed, Default's decays with airDec
    Player *f = add_player(&game, 7, 0.0f, 300.0f, -300.0f);
    Player *g = add_player(&def, 8, 0.0f, 300.0f, -300.0f);
    f->velocity.x = g->velocity.x = 0.05f;
    srm_jumps = 0;
    tick(f, K_NONE, 0.0f, 0.0f, 0, 0); // tmpVel takes the launch
    f->velocity.x = 0.05f, f->scheme.tmp_vel.x = 0.05f;
    for (int i = 0; i < 30; i++) {
        tick(f, K_NONE, 0.0f, 0.0f, 0, 0);
        tick(g, K_NONE, 0.0f, 0.0f, 0, 0);
    }
    CHECK(fabsf(f->velocity.x - 0.05f) < 1e-4f && g->velocity.x < 0.045f, "air: SRM keeps 0.05 (%.4f), Default decays (%.4f)", f->velocity.x, g->velocity.x);

    // overlayInfo (SetAccelerationValues, isYou only): an airborne strafe of our own player sets the HUD's angles
    Player *o = add_player(&game, 10, 300.0f, 200.0f, 300.0f);
    Player *other = add_player(&game, 11, 300.0f, 200.0f, -300.0f);
    o->is_you = 1;
    int overlay_frames = 0, other_frames = 0;
    for (int i = 0; i < 40; i++) {
        tick(o, K_WD, -0.02f * (float) i, 0.0f, 0, 0);
        tick(other, K_WD, -0.02f * (float) i, 0.0f, 0, 0);
        overlay_frames += o->scheme.overlay_valid;
        other_frames += other->scheme.overlay_valid;
    }
    CHECK(overlay_frames > 30 && other_frames == 0, "the strafe overlay: our airborne strafe (%d frames), never another player (%d)", overlay_frames, other_frames);

    // RampBounce: landing slowly on the ramp (dir 0, slope atan(6 / 20)) pushes down the slope
    Player *r = add_player(&game, 9, 0.0f, 8.0f, 0.0f);
    float min_vx = 0.0f;
    for (int i = 0; i < 40; i++) {
        tick(r, K_NONE, 0.0f, 0.0f, 0, 0);
        if (r->velocity.x < min_vx) min_vx = r->velocity.x;
    }
    CHECK(min_vx < -0.005f, "ramp landing bounces along -x (min vx %.4f)", min_vx);
}

static void test_vehicle(void) {
    printf("6 Vehicle: throttle, reverse, steering, the look ignored...\n");
    Game game;
    game_on(&game, "{\"movSch\":6}");
    Player *p = add_player(&game, 1, -300.0f, 0.0f, 300.0f);
    CHECK(!player_direction_is_look(p) && !player_look_follows_camera(p), "a vehicle's body is not the look");
    ticks(p, 60, K_W, 2.0f, 0.5f); // the mouse look does not turn it
    // vhpF 0.0012 per 60 fps frame (17 ms = 1.02 frames) up to vhmP 0.12
    CHECK(fabsf(p->scheme.forward_v - 60 * 0.0012f * 1.02f) < 1e-3f, "forwardV builds by vhpF a frame (%.4f)", p->scheme.forward_v);
    CHECK(p->position.z < 280.0f && fabsf(p->position.x + 300.0f) < 0.01f && p->direction.y == 0.0f, "drives along its heading -z, look ignored (%.2f, %.2f, yaw %.3f)",
          p->position.x, p->position.z, p->direction.y);
    ticks(p, 60, K_W, 0.0f, 0.0f);
    CHECK(p->scheme.forward_v <= 0.12f + 1e-6f, "forwardV capped at vhmP (%.4f)", p->scheme.forward_v);
    const float x0 = p->position.x;
    ticks(p, 30, K_WD, 0.0f, 0.0f);
    CHECK(p->scheme.ang_vel < 0.0f && p->direction.y < -0.1f && p->position.x > x0, "W+D steers right: angVel < 0, yaw down, drifts +x (angVel %.4f, yaw %.3f)",
          p->scheme.ang_vel, p->direction.y);
    // Vehicle.mov's lean (deob-10.0.3.js:19145-19152) reads angVel before mov decays it: the decayed angVel is
    // lean_ang_vel * vhaDr^frames (0.94 per 60 fps frame, 17 ms = 1.02 frames)
    CHECK(p->scheme.lean_ang_vel < 0.0f && fabsf(p->scheme.ang_vel - p->scheme.lean_ang_vel * powf(0.94f, 1.02f)) < 1e-5f,
          "the model's lean takes angVel before mov's decay (%.5f, then %.5f)", p->scheme.lean_ang_vel, p->scheme.ang_vel);

    Player *r = add_player(&game, 2, 300.0f, 0.0f, -300.0f);
    ticks(r, 60, K_S, 0.0f, 0.0f);
    CHECK(r->scheme.reverse_v > 0.03f && r->scheme.forward_v == 0.0f && r->position.z > -300.0f, "S reverses: reverseV builds, backs up +z (%.4f, z %.2f)",
          r->scheme.reverse_v, r->position.z);
    CHECK(r->scheme.reverse_v <= 0.0375f + 1e-6f, "reverseV capped at vhmR (%.4f)", r->scheme.reverse_v);

    // resetMovVals on spawn
    player_spawn(p);
    CHECK(p->scheme.forward_v == 0.0f && p->scheme.ang_vel == 0.0f, "a spawn resets the scheme state");
}

static void test_mmorpg(void) {
    printf("7 MMORPG: keys relative to the camera, the body turns to the move...\n");
    Game game;
    game_on(&game, "{\"movSch\":7}");
    Player *p = add_player(&game, 1, -200.0f, 0.0f, -200.0f);
    CHECK(!player_direction_is_look(p), "an MMORPG body is not the look");
    ticks(p, 30, K_D, 0.0f, 0.6f);
    CHECK(p->position.x > -197.0f && fabsf(p->position.z + 200.0f) < 0.05f, "D with the camera at yaw 0 walks +x (%.3f, %.3f)", p->position.x, p->position.z);
    CHECK(fabsf(p->direction.y - 1.5f * (float) M_PI) < 1e-3f && p->direction.x == 0.0f, "the body turned to face +x (3 PI / 2), the pitch never set (yaw %.4f, pitch %.3f)",
          p->direction.y, p->direction.x);
    Player *q = add_player(&game, 2, 200.0f, 0.0f, 200.0f);
    tick(q, K_W, (float) M_PI / 2.0f, 0.0f, 0, 0);
    CHECK(fabsf(q->direction.y - 0.204f) < 1e-3f, "the body turns 0.012 rad per ms (%.4f after 17 ms)", q->direction.y);
    ticks(q, 30, K_W, (float) M_PI / 2.0f, 0.0f);
    CHECK(q->position.x < 197.0f && fabsf(q->direction.y - (float) M_PI / 2.0f) < 1e-3f, "W with the camera at PI/2 walks -x and faces it (%.3f, yaw %.4f)", q->position.x, q->direction.y);
    Player *s = add_player(&game, 3, 200.0f, 0.0f, -200.0f);
    s->direction.y = 1.0f;
    ticks(s, 10, K_NONE, 2.0f, 0.0f);
    CHECK(s->direction.y == 1.0f, "no keys: the body keeps its heading (%.3f)", s->direction.y);
}

// v10 SYNC_PLAYER sendVals: a replay from the server's snapshot lands where the server is only when the
// scheme's own state comes with it (player_movement_send_vals / _apply_vals, NET_EXT_PLAYER_MOVE_VALS).
static float replay_error(const char *cls, const int with_vals) {
    Game game;
    game_on(&game, cls);
    Player *server = add_player(&game, 1, -200.0f, 0.0f, -200.0f);
    Player *client = add_player(&game, 2, 200.0f, 0.0f, 200.0f);
    client->position = server->position; // same place: no player collision
    client->last_position = server->last_position;
    Input inputs[60];
    for (int i = 0; i < 60; i++) {
        const int keys[4] = {K_W, K_WD, K_W, K_A};
        inputs[i] = make_input(server, keys[(i / 9) % 4], 0.4f * (float) (i / 20), 0.0f, i % 17 == 0, i == 33);
        inputs[i].seq = i + 1;
    }
    NetReconPacket snap;
    float vals[PLAYER_MOVE_VALS_MAX];
    int count = 0;
    for (int i = 0; i < 60; i++) {
        player_proc_input(server, &inputs[i], 0, 0);
        server->input_seq = inputs[i].seq;
        if (i == 39) {
            net_packet_from_recon(&snap, server, inputs[i].seq);
            count = player_movement_send_vals(server, vals);
        }
    }
    for (int i = 0; i < 60; i++) {
        player_proc_input(client, &inputs[i], 0, 0);
        client->input_seq = inputs[i].seq;
    }
    if (with_vals && count) player_movement_apply_vals(client, (int) player_movement_scheme(client), vals, count);
    Input pending[60];
    memcpy(pending, inputs, sizeof(inputs));
    net_reconcile(client, &snap, pending, 60, 0);
    const float dx = client->position.x - server->position.x, dz = client->position.z - server->position.z, dy = client->position.y - server->position.y;
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

static void test_send_vals(void) {
    printf("sendVals: the reconciliation replays from the server's scheme state...\n");
    float vals[PLAYER_MOVE_VALS_MAX];
    Game game;
    game_on(&game, "{\"movSch\":0}");
    Player *p = add_player(&game, 1, 0.0f, 0.0f, -300.0f);
    CHECK(player_movement_send_vals(p, vals) == 0, "Default has no sendVals");

    const float vehicle = replay_error("{\"movSch\":6}", 1), vehicle_stale = replay_error("{\"movSch\":6}", 0);
    CHECK(vehicle < 1e-4f && vehicle_stale > 0.01f, "Vehicle: replay error %.6f with the sendVals, %.3f without", vehicle, vehicle_stale);
    const float srm = replay_error("{\"movSch\":5}", 1), srm_stale = replay_error("{\"movSch\":5}", 0);
    CHECK(srm < 1e-4f && srm_stale > srm, "SRM: replay error %.6f with the sendVals, %.6f without", srm, srm_stale);
}

// The mode's own physics keys (Krunker v10.0.3 modes): groundAccel scales the ground and the air
// acceleration (`mode.groundAccel ?? 1`, deob-10.0.3.js:191624), gravMlt the gravity (`mode.gravMlt ?? 1`,
// :191697), impulseMlt a shot's knockback (`mode.impulseMlt != null ? ... : 1` with config.impulseMlt,
// :196585 the shooter, :196732 a hit). Chaos Snipers: gravMlt 0.92, groundAccel 2 (:120048-120050);
// Impulsed: gravMlt 1.05, impulseMlt 3, nonObjImpulse, groundAccel 2 (:120077-120081).
typedef struct {
    float fall, walk, air_walk;
} ModeSteps;

static ModeSteps mode_steps(const int mode_id) {
    ModeSteps steps;
    Game game;
    game_on_mode(&game, "{\"movSch\":0}", mode_id);
    Player *f = add_player(&game, 1, -300.0f, 300.0f, -300.0f); // airborne, still: a tick of gravity
    tick(f, K_NONE, 0.0f, 0.0f, 0, 0); // a spawn is onGround until its first collision pass
    const float vy = f->velocity.y;
    tick(f, K_NONE, 0.0f, 0.0f, 0, 0);
    steps.fall = f->velocity.y - vy;
    Player *w = add_player(&game, 2, 300.0f, 0.0f, 300.0f);
    ticks(w, 5, K_NONE, 0.0f, 0.0f); // settle on the floor
    w->velocity = (vec3) {0.0f, 0.0f, 0.0f};
    tick(w, K_W, 0.0f, 0.0f, 0, 0);
    steps.walk = flat_speed(w->velocity) / w->weapon->speed_mlt; // the mode's starting weapon's spdMlt taken out
    Player *a = add_player(&game, 3, 300.0f, 300.0f, -300.0f);
    tick(a, K_NONE, 0.0f, 0.0f, 0, 0); // off the spawn's onGround: the next tick is airborne
    tick(a, K_W, 0.0f, 0.0f, 0, 0);
    steps.air_walk = flat_speed(a->velocity) / a->weapon->speed_mlt;
    return steps;
}

// One shot at the sky (no hit) in mode `mode_id` with `weapon_id`: the shooter's speed after the tick
static float impulse_step(const int mode_id, const int weapon_id, const float mode_impulse) {
    Game game;
    game_on_mode(&game, "{\"movSch\":0}", mode_id);
    if (mode_impulse >= 0.0f) game.mode->config.impulse_mlt = mode_impulse;
    Player *p = add_player(&game, 1, 0.0f, 300.0f, 300.0f);
    p->loadout[0] = weapon_id;
    p->ammo[0] = game.weapons[weapon_id]->ammo ? game.weapons[weapon_id]->ammo : 1;
    player_swap_weapon(p, 0, 1, 1, 0);
    p->swap_timer = p->reload_timer = 0.0f;
    p->reloads[0] = 0.0f;
    p->velocity = (vec3) {0.0f, 0.0f, 0.0f};
    tick(p, K_NONE, 0.0f, 1.2f, 0, 0); // look up, settle the look
    p->velocity = (vec3) {0.0f, 0.0f, 0.0f};
    Input in = make_input(p, K_NONE, 0.0f, 1.2f, 0, 0);
    in.shoot = 1;
    player_proc_input(p, &in, 0, 0);
    const float push_y = p->velocity.y + 17.0f * 0.00015f * game.mode->config.grav_mlt; // gravity's tick taken out
    return sqrtf(flat_speed(p->velocity) * flat_speed(p->velocity) + push_y * push_y);
}

static void test_mode_physics(void) {
    printf("mode physics keys: groundAccel, gravMlt, impulseMlt...\n");
    const ModeSteps ffa = mode_steps(MODE_ID_FFA), chs = mode_steps(MODE_ID_CHS), imp = mode_steps(MODE_ID_IMP);
    CHECK(fabsf(chs.fall / ffa.fall - 0.92f) < 1e-3f && fabsf(imp.fall / ffa.fall - 1.05f) < 1e-3f,
          "gravMlt: Chaos Snipers 0.92, Impulsed 1.05 of FFA's tick (%.6f %.6f %.6f)", ffa.fall, chs.fall, imp.fall);
    CHECK(fabsf(chs.walk / ffa.walk - 2.0f) < 1e-3f && fabsf(imp.walk / ffa.walk - 2.0f) < 1e-3f,
          "groundAccel 2: twice FFA's ground step (%.6f %.6f %.6f)", ffa.walk, chs.walk, imp.walk);
    CHECK(fabsf(chs.air_walk / ffa.air_walk - 2.0f) < 1e-3f && ffa.air_walk < ffa.walk * 0.5f,
          "groundAccel scales the air acceleration (arA) too (%.6f %.6f)", ffa.air_walk, chs.air_walk);

    // Impulsed's starting loadout (deob-10.0.3.js:120083): the first of its weapons with a physPow
    int weapon = -1;
    const int starters[2] = {5, 22};
    for (int i = 0; i < 2 && weapon < 0; i++) if (rules_weapon(starters[i])->physical_power > 0.0f) weapon = starters[i];
    CHECK(weapon >= 0, "an Impulsed starter has a physPow");
    if (weapon < 0) return;
    const float power = rules_weapon(weapon)->physical_power;
    const float three = impulse_step(MODE_ID_IMP, weapon, -1.0f), one = impulse_step(MODE_ID_IMP, weapon, 1.0f);
    CHECK(fabsf(three / one - 3.0f) < 0.02f && fabsf(one - power) < 0.1f * power,
          "impulseMlt 3: a miss pushes the shooter 3 x physPow %.3f (push %.4f, at impulseMlt 1 %.4f)", power, three, one);
}

int main(void) {
    if (load_json_file("shared/tests/maps/ramp_fix.json", &g_map)) {
        printf("Failed to load shared/tests/maps/ramp_fix.json (run from the repository root)\n");
        return 1;
    }
    test_default();
    test_follow_mouse();
    test_move_right();
    test_fly();
    test_dir_only();
    test_srm();
    test_vehicle();
    test_mmorpg();
    test_send_vals();
    test_mode_physics();
    printf("movement_scheme_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
