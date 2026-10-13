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

int main(void) {
    test_wall_behind();
    test_hitch_open();
    test_map("assets/maps/kanji.json");
    test_map("assets/maps/sandstorm_v3.json");
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
