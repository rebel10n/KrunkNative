// The death chase camera (client/src/menu_view.c, v7.1.4 CONTROLS.camLookAt / update): after a
// kill the camera must stay near the death position and out of solid geometry until the respawn.
//   - a killer closer than camChaseDst puts camLookAt's goal behind the victim: with a wall there
//     the camera used to glide into / through it (the player saw the void)
//   - one long frame (a hitch, alt-tab, a throttled tab) used to move it by delta * camChaseSpd
//     times its distance: past ~1.7 s it overshot and flew off the map (v7 caps at dltMx)
// GL-free: the file is included; spectate.c is stubbed. Run from the repository root.
#include "../src/menu_view.c"
#include <stdio.h>
#include <stdlib.h>

static int g_failures, g_checks;

#define CHECK(cond, ...) do { g_checks++; \
    if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_failures++; } \
} while (0)

int spectate_active(void) { return 0; }
// The alive mouse sensitivity (ui_controls.c) and the input script's look: a fixed 0.002 rad per px here.
void ui_controls_fill_look(Client *client, Input *input, const float dx, const float dy) {
    (void) client;
    input->y_dir -= dx * 0.002f;
    input->x_dir -= dy * 0.002f;
}
void input_script_adjust_input(Client *client, Input *input) { (void) client; (void) input; }

static Client g_client;
static Player g_me, g_killer;

static void test_send(Game *game, const int uid, const int type, const void *payload, const uint16_t length) {
    (void) game; (void) uid; (void) type; (void) payload; (void) length;
}

static void game_on(Game *game, const cJSON *map) {
    static const cJSON *maps[1];
    maps[0] = map;
    memset(game, 0, sizeof(*game));
    int *modes = malloc(sizeof(int));
    modes[0] = 0;
    game_configure(game, NULL, maps, 1, modes, 1, NULL, 0, NULL, 0);
    game->hooks.send = test_send;
    game_init(game, 0, 0, 1);
}

static float dist3(const vec3 a, const vec3 b) {
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

// The solid object box (player collision's set, box collision) containing p, NULL none.
static const Object *solid_at(const Map *map, const vec3 p) {
    for (size_t i = 0; i < map->object_count; i++) {
        const Object *o = map->objects[i];
        if (!bot_object_blocks_player(o) || o->complex_coll) continue;
        const vec3 s = object_box_size(o);
        if (p.x > o->position.x - s.x * 0.5f && p.x < o->position.x + s.x * 0.5f && p.y > o->position.y &&
            p.y < o->position.y + s.y && p.z > o->position.z - s.z * 0.5f && p.z < o->position.z + s.z * 0.5f) return o;
    }
    return NULL;
}

// Death at eye `eye` (looking yaw / pitch) by a killer with feet `killer`; then `seconds` of
// frames at 60 fps with one `hitch` s frame after 0.5 s. Returns the largest distance from the
// death eye; *inside counts frames whose camera sat in a solid box.
static float run_death(const vec3 eye, const vec3 killer, const float seconds, const float hitch, int *inside, vec3 *last) {
    g_client.me = &g_me;
    g_me.active = 0;
    g_me.direction = (vec2) {0.0f, 0.0f};
    g_client.camera.position = eye;
    g_killer.position = killer;
    g_killer.height = game_constants.player_height;
    g_killer.uid = 2;
    menu_view_death_cam(&g_client, &g_killer);

    float worst = 0.0f;
    *inside = 0;
    float t = 0.0f;
    int hitched = hitch <= 0.0f;
    while (t < seconds) {
        float delta = 1.0f / 60.0f;
        if (!hitched && t >= 0.5f) { delta = hitch; hitched = 1; }
        menu_view_camera(&g_client, delta);
        t += delta;
        const vec3 c = g_client.camera.position;
        if (!isfinite(c.x) || !isfinite(c.y) || !isfinite(c.z)) { worst = INFINITY; break; }
        worst = fmaxf(worst, dist3(c, eye));
        if (solid_at(g_client.game.map, c)) (*inside)++;
    }
    *last = g_client.camera.position;
    return worst;
}

// A floor, a wall right behind the victim (x -10..-6) and an open side.
static void test_wall_behind(void) {
    printf("killer in front, wall behind the victim...\n");
    cJSON *map = cJSON_Parse("{\"name\":\"dc\",\"spawns\":[[0,0,0]],\"objects\":["
                             "{\"p\":[0,-10,0],\"s\":[600,10,600]},"
                             "{\"p\":[-8,0,0],\"s\":[4,60,200]}]}");
    game_on(&g_client.game, map);
    CHECK(g_client.game.ready, "dc map loaded");
    if (!g_client.game.ready) return;
    g_client.net_end_seconds = -1;

    const vec3 eye = {0.0f, 9.5f, 0.0f};
    const vec3 killer = {10.0f, 0.0f, 0.0f}; // 10 units away: camLookAt's goal is 14 units behind the victim
    int inside;
    vec3 last;
    float worst = run_death(eye, killer, 6.0f, 0.0f, &inside, &last);
    CHECK(inside == 0, "camera in the wall for %d frames (ends at %.2f %.2f %.2f)", inside, last.x, last.y, last.z);
    CHECK(last.x > -6.0f, "camera passed the wall face x -6: x %.2f", last.x);
    CHECK(worst <= 8.0f, "camera %.2f from the death eye (the wall is 6 away)", worst);

    // the same with a 5 s hitch frame (the death card loading, an alt-tab)
    worst = run_death(eye, killer, 8.0f, 5.0f, &inside, &last);
    CHECK(inside == 0 && last.x > -6.0f, "hitch: camera in the wall for %d frames, x %.2f", inside, last.x);
    CHECK(worst <= 8.0f, "hitch: camera %.2f from the death eye", worst);

    // open space (killer to the side, nothing between): v7's chase is unchanged, the camera ends
    // camChaseDst from the killer's chest
    const vec3 side = {0.0f, 0.0f, 40.0f};
    worst = run_death(eye, side, 10.0f, 0.0f, &inside, &last);
    const vec3 chest = {side.x, side.y + g_killer.height / 1.5f, side.z};
    CHECK(fabsf(dist3(last, chest) - CAM_CHASE_DST) < 0.5f, "open: %.2f from the killer (camChaseDst %.0f)", dist3(last, chest), CAM_CHASE_DST);
    CHECK(inside == 0, "open: camera in a box for %d frames", inside);
    cJSON_Delete(map);
}

// Long frames in the open: the chase must converge, never overshoot past its goal.
static void test_hitch_open(void) {
    printf("long frames in the open...\n");
    cJSON *map = cJSON_Parse("{\"name\":\"dc2\",\"spawns\":[[0,0,0]],\"objects\":[{\"p\":[0,-10,0],\"s\":[2000,10,2000]}]}");
    game_on(&g_client.game, map);
    CHECK(g_client.game.ready, "dc2 map loaded");
    if (!g_client.game.ready) return;
    g_client.net_end_seconds = -1;
    const vec3 eye = {0.0f, 9.5f, 0.0f};
    const vec3 killer = {0.0f, 0.0f, 150.0f};
    const vec3 chest = {killer.x, killer.y + game_constants.player_height / 1.5f, killer.z};
    const float limit = dist3(eye, chest); // the camera only travels from the eye toward the killer
    for (int h = 1; h <= 4; h++) {
        int inside;
        vec3 last;
        const float worst = run_death(eye, killer, 10.0f, 1.5f * (float) h, &inside, &last);
        CHECK(worst <= limit, "hitch %.1f s: camera %.1f from the death eye (killer %.1f away)", 1.5f * (float) h, worst, limit);
    }
    cJSON_Delete(map);
}

// Kanji and Sandstorm: deaths at every spawn by a killer 8 units away in 8 directions.
static void test_map(const char *path) {
    printf("%s: deaths at every spawn...\n", path);
    cJSON *map = NULL;
    if (load_json_file(path, &map)) {
        printf("skip %s (gitignored map assets, as on CI)\n", path);
        return;
    }
    game_on(&g_client.game, map);
    CHECK(g_client.game.ready, "%s loaded", path);
    if (!g_client.game.ready) { cJSON_Delete(map); return; }
    g_client.net_end_seconds = -1;
    const Map *m = g_client.game.map;
    int cases = 0, bad = 0;
    for (size_t s = 0; s < m->spawn_count; s++) {
        const vec3 feet = m->spawns[s]->position;
        const vec3 eye = {feet.x, feet.y + game_constants.player_height - game_constants.camera_height, feet.z};
        if (solid_at(m, eye)) continue;
        for (int k = 0; k < 8; k++) {
            const float a = (float) k * (float) M_PI / 4.0f;
            const vec3 killer = {feet.x + 8.0f * sinf(a), feet.y, feet.z + 8.0f * cosf(a)};
            int inside;
            vec3 last;
            const float worst = run_death(eye, killer, 6.0f, k & 1 ? 3.0f : 0.0f, &inside, &last);
            cases++;
            if (inside || !(worst <= CAM_CHASE_DST)) {
                if (bad++ < 5) printf("  spawn %zu dir %d: inside %d frames, %.1f from the eye\n", s, k, inside, worst);
            }
        }
    }
    CHECK(bad == 0, "%s: %d of %d deaths left the camera in geometry or far off", path, bad, cases);
    cJSON_Delete(map);
}

// The signed shortest angle from `from` to `to` (UTILS.angleDistance).
static float angle_between(const float from, const float to) {
    return atan2f(sinf(to - from), cosf(from - to));
}

// A dead player at eye (yaw 0, pitch 0) on an open floor; the killer `killer` (feet), no wall.
static void dead_on_open_floor(const vec3 killer, const int with_killer) {
    cJSON *map = cJSON_Parse("{\"name\":\"dc3\",\"spawns\":[[0,0,0]],\"objects\":[{\"p\":[0,-10,0],\"s\":[2000,10,2000]}]}");
    game_on(&g_client.game, map);
    cJSON_Delete(map);
    g_client.net_end_seconds = -1;
    const vec3 eye = {0.0f, 9.5f, 0.0f};
    g_client.me = &g_me;
    g_me.active = 0;
    g_me.direction = (vec2) {0.0f, 0.0f};
    g_client.camera.position = eye;
    g_client.camera.rotation = (vec3) {0.0f, 0.0f, 0.0f};
    g_client.camera.zoom = 1.0f;
    g_killer.position = killer;
    g_killer.height = game_constants.player_height;
    g_killer.uid = 2;
    menu_view_death_cam(&g_client, with_killer ? &g_killer : NULL);
}

// v10 CONTROLS.update (camChaseTrn 0.0022 per ms): the yaw eases toward the killer's bearing, each
// frame closing 0.0022 of the angle left per ms (never overshooting). A 120 degree turn is then
// within 1 degree after ~2.1 s (exp(-0.0022 t), time constant 455 ms), 0.1 degree after ~3.2 s.
static void test_turn_to_killer(void) {
    printf("the dead camera turns toward the killer...\n");
    // the killer 40 away at 120 degrees to the left of the death yaw 0 (camera yaw = atan2(-dx, -dz))
    const float want = 2.0944f;
    const vec3 killer = {-40.0f * sinf(want), 0.0f, -40.0f * cosf(want)};
    dead_on_open_floor(killer, 1);
    CHECK(g_client.game.ready, "dc3 map loaded");
    if (!g_client.game.ready) return;
    CHECK(menu_view_dead() && menu_view_killer_uid() == 2, "dead with the killer (uid %d)", menu_view_killer_uid());

    float prev = angle_between(0.0f, want), t = 0.0f, t1 = -1.0f, t01 = -1.0f;
    int monotonic = 1, overshoot = 0;
    for (int frame = 1; frame <= 240; frame++) { // 4 s at 60 fps
        menu_view_camera(&g_client, 1.0f / 60.0f);
        t += 1.0f / 60.0f;
        const float err = angle_between(g_client.camera.rotation.y, want);
        if (fabsf(err) > fabsf(prev) + 1e-5f) monotonic = 0;
        if (err * prev < 0.0f) overshoot = 1; // the sign flipped: it passed the killer
        if (t1 < 0.0f && fabsf(err) <= 0.0174533f) t1 = t;
        if (t01 < 0.0f && fabsf(err) <= 0.00174533f) t01 = t;
        prev = err;
    }
    CHECK(monotonic, "the yaw error grows at some frame (the turn is not monotonic)");
    CHECK(!overshoot, "the yaw overshot the killer's bearing");
    CHECK(t1 > 0.0f && t1 <= 2.5f, "within 1 degree of the killer after %.2f s (v10 ~2.1 s)", t1);
    CHECK(t01 > 0.0f && t01 <= 3.6f, "within 0.1 degree after %.2f s (v10 ~3.2 s)", t01);
    printf("  120 degree turn: 1 degree at %.2f s, 0.1 degree at %.2f s\n", t1, t01);
}

// Mouse look while dead: it turns the rig with or without a killer, the pitch stays within +-PI/2,
// and while the mouse moves the chase does not hold it back (the look can stay away from the killer).
static void test_mouse_while_dead(void) {
    printf("mouse look while dead...\n");
    const vec3 killer = {-34.64f, 0.0f, 20.0f}; // the chase turns toward yaw 2.0944
    dead_on_open_floor(killer, 1);
    if (!g_client.game.ready) return;
    menu_view_camera(&g_client, 1.0f / 60.0f);

    // pitch: a huge push up clamps to +PI/2 (the chase then only pulls it down toward the killer)
    menu_view_look(&g_client, 0.0f, -1.0e6f);
    menu_view_camera(&g_client, 1.0f / 60.0f);
    CHECK(g_client.camera.rotation.x <= (float) M_PI / 2.0f + 1e-6f && g_client.camera.rotation.x >= -(float) M_PI / 2.0f - 1e-6f,
          "pitch %.4f inside +-PI/2", g_client.camera.rotation.x);
    CHECK(g_client.camera.rotation.x > 1.4f, "pitch clamped at +PI/2 and the chase only pulled it %.4f", g_client.camera.rotation.x);

    // Let the chase settle for 5 s first (the 2.09 rad turn is gone), then look away for 2 s: 4 px a
    // frame to the left (dx > 0 turns the yaw down, away from the killer at yaw 2.0944). With the chase
    // the look runs at 0.2 speed: 4 * 0.002 * 0.2 = 0.0016 rad a frame. The steady error is
    // (1 - c) * 0.0016 / c = 0.042 rad (2.4 degrees), c = 0.0022 * 16.67 the chase's pull per frame.
    dead_on_open_floor(killer, 1);
    menu_view_camera(&g_client, 1.0f / 60.0f);
    for (int f = 0; f < 300; f++) menu_view_camera(&g_client, 1.0f / 60.0f);
    float err = 0.0f;
    for (int f = 0; f < 120; f++) {
        menu_view_look(&g_client, 4.0f, 0.0f);
        menu_view_camera(&g_client, 1.0f / 60.0f);
        err = angle_between(g_client.camera.rotation.y, 2.0944f);
    }
    CHECK(fabsf(err) > 0.035f && fabsf(err) < 0.05f, "held look: %.1f degrees off the killer (steady ~2.4)", fabsf(err) * 57.2958f);
    // release: the chase brings the camera back to within 1 degree in ~2.5 s
    float t = 0.0f, t1 = -1.0f;
    for (int f = 0; f < 240; f++) {
        menu_view_camera(&g_client, 1.0f / 60.0f);
        t += 1.0f / 60.0f;
        if (t1 < 0.0f && fabsf(angle_between(g_client.camera.rotation.y, 2.0944f)) <= 0.0174533f) t1 = t;
    }
    CHECK(t1 > 0.0f && t1 <= 3.0f, "released: back within 1 degree after %.2f s", t1);

    // no killer (suicide / kCams off): the camera stays where it died, the mouse still turns it
    dead_on_open_floor(killer, 0);
    menu_view_camera(&g_client, 1.0f / 60.0f);
    const vec3 p0 = g_client.camera.position;
    menu_view_look(&g_client, 100.0f, 0.0f);
    menu_view_camera(&g_client, 1.0f / 60.0f);
    CHECK(menu_view_dead() && menu_view_killer_uid() < 0, "no killer: dead without a chase");
    CHECK(fabsf(g_client.camera.rotation.y + 0.2f) < 1e-4f, "no killer: 100 px turns the camera -0.2 rad (%.4f)", g_client.camera.rotation.y);
    CHECK(dist3(g_client.camera.position, p0) < 1e-6f, "no killer: the camera does not move");

    // a spawn ends the death view: the look no longer reaches the rig
    menu_view_reset();
    CHECK(!menu_view_dead(), "spawn: no death view");
    menu_view_look(&g_client, 100.0f, 0.0f);
    CHECK(!menu_view_dead(), "spawn: the look is ignored");
}

// CONTROLS.updateMouseInp: the look runs at 0.2 of the alive speed while CONTROLS.target is set (a
// killer is known, the chase) and at full speed with no killer. The stub above turns 100 px into
// 0.2 rad, so the rig turns 0.04 rad with a killer and 0.2 rad without, on both axes.
static void test_look_speed_by_chase(void) {
    printf("mouse look speed: 0.2 with a killer, full speed without...\n");
    const vec3 killer = {-34.64f, 0.0f, 20.0f};
    dead_on_open_floor(killer, 1);
    if (!g_client.game.ready) return;
    CHECK(g_view.chase == 1 && menu_view_killer_uid() == 2, "killer: the chase is running");
    menu_view_look(&g_client, 100.0f, 0.0f);
    CHECK(fabsf(g_view.yaw + 0.04f) < 1e-5f, "killer: 100 px turns the rig -0.04 rad (%.5f)", g_view.yaw);
    menu_view_look(&g_client, 0.0f, 100.0f);
    CHECK(fabsf(g_view.pitch + 0.04f) < 1e-5f, "killer: 100 px pitches the rig -0.04 rad (%.5f)", g_view.pitch);

    dead_on_open_floor(killer, 0);
    CHECK(g_view.chase == 0, "no killer: no chase");
    menu_view_look(&g_client, 100.0f, 0.0f);
    CHECK(fabsf(g_view.yaw + 0.2f) < 1e-5f, "no killer: 100 px turns the rig -0.2 rad (%.5f)", g_view.yaw);
    menu_view_look(&g_client, 0.0f, 100.0f);
    CHECK(fabsf(g_view.pitch + 0.2f) < 1e-5f, "no killer: 100 px pitches the rig -0.2 rad (%.5f)", g_view.pitch);
}

int main(void) {
    test_wall_behind();
    test_hitch_open();
    test_turn_to_killer();
    test_mouse_while_dead();
    test_look_speed_by_chase();
    test_map("assets/maps/official/10_kanji.json");
    test_map("assets/maps/official/02_sandstorm.json");
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
