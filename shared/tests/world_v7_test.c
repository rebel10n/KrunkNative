// map-logic WP-B (doc/v7/map-logic.md §7 WP-B tests): the v7.1.4 world runtime of
// shared/include/world.h against the fixtures in shared/tests/maps/ (v7_zones, v7_movement,
// v7_br_zone, v7_terrain): tris capsule collision, the terrain generator / raycast / ground snap
// (reference values from v7 libs/Terrain.js run under node), the multi-stage BR zone and its
// damage, score zones with logTime, check points, teleporter tcp, the Parkour resets, the
// account / team / KR zones, NoPVP / PVP and the damage filter, collidingObjs, node moves,
// object animations, builds, the deathY godMode rule and the input[13] / [14] wire bits.
// Run from the repository root: ./<build>/world_v7_test
#include <world.h>
#include <account.h>
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

typedef struct {
    int uid, type;
    uint16_t length;
    unsigned char payload[NET_MAX_PAYLOAD];
} Sent;

static Sent g_sent[4096];
static int g_sent_count;

static void test_send(Game *game, const int uid, const int type, const void *payload, const uint16_t length) {
    (void) game;
    if (g_sent_count >= (int) (sizeof(g_sent) / sizeof(g_sent[0]))) return;
    Sent *s = &g_sent[g_sent_count++];
    s->uid = uid;
    s->type = type;
    s->length = length;
    if (payload && length <= sizeof(s->payload)) memcpy(s->payload, payload, length);
}

static const Sent *find_sent(const int type, const int nth) {
    int n = 0;
    for (int i = 0; i < g_sent_count; i++) {
        if (g_sent[i].type == type && n++ == nth) return &g_sent[i];
    }
    return NULL;
}

static int count_sent(const int type) {
    int n = 0;
    for (int i = 0; i < g_sent_count; i++) n += g_sent[i].type == type;
    return n;
}

static AccountInfo g_account;
static int test_account(const Game *game, const Player *player, AccountInfo *out) {
    (void) game;
    (void) player;
    *out = g_account;
    return 1;
}

static int near(const float a, const float b, const float eps) { return fabsf(a - b) <= eps; }

static cJSON *load_map(const char *path) {
    cJSON *map = NULL;
    if (load_json_file(path, &map)) {
        printf("Failed to load %s (run from the repository root)\n", path);
        exit(1);
    }
    return map;
}

// An authority game (is_local) on `map` in mode `mode_id`, with the recording send hook.
static void game_on(Game *game, const cJSON *map, const int mode_id) {
    static const cJSON *maps[1];
    maps[0] = map;
    memset(game, 0, sizeof(*game));
    int *modes = malloc(sizeof(int));
    modes[0] = mode_id;
    game_configure(game, NULL, maps, 1, modes, 1, NULL, 0, NULL, 0);
    game->hooks.send = test_send;
    game_init(game, 0, 0, 1);
}

static Player *add_player(Game *game, const int uid, const int team) {
    Player *player = player_init(game);
    player->uid = uid;
    snprintf(player->name, sizeof(player->name), "P%d", uid);
    game_players_add(game, player);
    player_spawn(player);
    player->team = team;
    return player;
}

static void place(Player *player, const float x, const float y, const float z) {
    player->position = (vec3) {x, y, z};
    player->last_position = player->position;
    player->velocity = (vec3) {0.0f, 0.0f, 0.0f};
}

// One movement frame (procInputs with no keys) at 60 fps.
static void frame(Player *player, const int reset) {
    const Input input = {.seq = player->input_seq + 1, .move_dir = -1, .delta = 1.0f / 60.0f, .reset = (unsigned char) reset};
    player_proc_input(player, &input, 0, 0);
}

static Object *object_at(const Game *game, const int prefab, const float x, const float z) {
    for (size_t i = 0; i < game->map->object_count; i++) {
        Object *o = game->map->objects[i];
        if ((int) o->prefab == prefab && near(o->position.x, x, 0.5f) && near(o->position.z, z, 0.5f)) return o;
    }
    return NULL;
}

// ---------------------------------------------------------------- tris

static void test_tris(void) {
    printf("Running tris capsule collision test...\n");
    cJSON *json = load_map("shared/tests/maps/v7_movement.json");
    Game game;
    game_on(&game, json, MODE_ID_FFA);
    CHECK(game.ready && game.map->tri_count == 4, "4 triangles parsed (%zu)", game.ready ? game.map->tri_count : 0);
    Player *p = add_player(&game, 1, 0);

    // the ramp at z 120 is 10 high: feet below its surface are pushed up along its normal
    place(p, 0.0f, 8.0f, 120.0f);
    p->velocity = (vec3) {0.0f, -0.1f, 0.0f};
    p->on_ground = 0;
    world_player_tris(p);
    CHECK(p->position.y > 8.0f && p->position.z < 120.0f, "pushed out of the ramp (%.3f, %.3f)", p->position.y, p->position.z);
    CHECK(p->on_ground, "normal.y > 0.7: on ground");
    const float nz = -1.0f / sqrtf(5.0f), ny = 2.0f / sqrtf(5.0f);
    CHECK(near(p->velocity.y * ny + p->velocity.z * nz, 0.0f, 1e-5f), "velocity along the normal removed (%f, %f)", p->velocity.y, p->velocity.z);

    // walking the ramp: gravity frames keep the player on the slope
    place(p, 0.0f, 12.0f, 120.0f);
    for (int i = 0; i < 90; i++) frame(p, 0);
    CHECK(p->position.y > 9.0f && p->position.y < 14.0f, "rests on the ramp: y %.3f", p->position.y);

    // the wall facing -x at x 50: a capsule overlapping it goes back to x <= 50 - radius
    place(p, 50.0f - p->scale * 0.5f, 5.0f, 20.0f);
    p->velocity = (vec3) {0.2f, 0.0f, 0.0f};
    p->on_ground = 0;
    world_player_tris(p);
    CHECK(near(p->position.x, 50.0f - p->scale, 1e-3f), "wall push-out: x %.4f (radius %.3f)", p->position.x, p->scale);
    CHECK(near(p->velocity.x, 0.0f, 1e-6f) && !p->on_ground, "wall: x velocity removed, not ground");

    // the edge case: a capsule beside the triangle edge only
    vec3 normal;
    float depth;
    const float tri[9] = {0, 0, 0, 0, 0, 10, 10, 0, 0}; // wound with its normal up
    CHECK(tri_capsule_collide(tri, (vec3) {5.0f, -0.5f, 5.0f}, 1.0f, 11.0f, &normal, &depth) && normal.y > 0.99f, "floor triangle hit from below the surface");
    CHECK(!tri_capsule_collide(tri, (vec3) {30.0f, 0.0f, 30.0f}, 1.0f, 11.0f, &normal, &depth), "far capsule: collBounds rejects");
    CHECK(tri_capsule_collide(tri, (vec3) {-1.2f, 0.0f, 5.0f}, 1.6f, 11.0f, &normal, &depth) && normal.x < -0.9f && depth > 0.0f,
          "edge contact pushes away from the edge (n %.2f %.2f %.2f d %.3f)", normal.x, normal.y, normal.z, depth);

    // cpx objects leave the AABB set (applyToQuad skips complexColl): only their tris collide
    place(p, -30.0f, 0.2f, 100.0f);
    p->velocity = (vec3) {0.0f, 0.0f, 0.0f};
    frame(p, 0);
    CHECK(p->position.y < 0.2f + 1e-3f && near(p->position.x, -30.0f, 1e-3f), "the cpx cube is not solid (y %.3f)", p->position.y);
    game_init(&game, 0, 0, 1);
    cJSON_Delete(json);
}

// ---------------------------------------------------------------- terrain

static void test_terrain(void) {
    printf("Running terrain test...\n");
    cJSON *json = load_map("shared/tests/maps/v7_terrain.json");
    Terrain *t = terrain_generate_json(json);
    CHECK(t && t->seg_x == 40 && t->seg_y == 40, "segments 40 x 40");
    if (t) {
        // v7.1.4 libs/Terrain.js (node, the same seed): terrainHeights[i]
        static const struct { int i; float h; } ref[] = {
            {0, -1000.0f}, {1, -1000.0f}, {7, -1000.0f}, {100, -1129.2664794921875f}, {500, -90.24507141113281f},
            {820, -1000.0f}, {1000, 154.05484008789062f}, {1500, -313.14788818359375f}, {1600, -1837.188720703125f},
        };
        for (size_t k = 0; k < sizeof(ref) / sizeof(ref[0]); k++)
            CHECK(near(t->heights[ref[k].i], ref[k].h, fabsf(ref[k].h) * 1e-6f + 1e-4f), "height[%d] %.6f, v7 %.6f", ref[k].i, t->heights[ref[k].i], ref[k].h);
        double hit[3];
        CHECK(terrain_raycast_local(t, 0, 0, 10000, 0, 0, -20000, hit, NULL) && near((float) hit[2], 257.72918701171875f, 1e-3f),
              "ground under (0, 0): %.5f, v7 257.72919", hit[2]);
        CHECK(terrain_raycast_local(t, 123.4, 56.7, 10000, 0, 0, -20000, hit, NULL) && near((float) hit[2], 260.27205f, 1e-3f),
              "ground under (123.4, -56.7): %.5f, v7 260.27205", hit[2]);
        CHECK(terrain_raycast_local(t, -300, 200, 300, 600, -100, -400, hit, NULL) && near((float) hit[0], -112.909775f, 1e-3f) &&
              near((float) hit[1], 168.818292f, 1e-3f) && near((float) hit[2], 175.273183f, 1e-3f),
              "slanted ray: (%.4f %.4f %.4f), v7 (-112.9098 168.8183 175.2732)", hit[0], hit[1], hit[2]);
        // terrainTSeed objects (v7 counts with a render: TREE 80, ROCK 25, BUSH 13)
        CHECK(t->obj_count[TERRAIN_OBJ_TREE] == 80 && t->obj_count[TERRAIN_OBJ_ROCK] == 25 && t->obj_count[TERRAIN_OBJ_BUSH] == 13 &&
              !t->obj_count[TERRAIN_OBJ_STUMP] && !t->obj_count[TERRAIN_OBJ_LOOT],
              "objects TREE %d ROCK %d BUSH %d", t->obj_count[TERRAIN_OBJ_TREE], t->obj_count[TERRAIN_OBJ_ROCK], t->obj_count[TERRAIN_OBJ_BUSH]);
        if (t->obj_count[TERRAIN_OBJ_TREE])
            CHECK(near(t->objs[TERRAIN_OBJ_TREE][0].x, -1941.0589f, 1e-2f) && near(t->objs[TERRAIN_OBJ_TREE][0].y, -1.21534f, 1e-3f) &&
                  near(t->objs[TERRAIN_OBJ_TREE][0].z, -225.36657f, 1e-2f) && t->objs[TERRAIN_OBJ_TREE][0].biome == 2,
                  "first tree (%.4f %.4f %.4f)", t->objs[TERRAIN_OBJ_TREE][0].x, t->objs[TERRAIN_OBJ_TREE][0].y, t->objs[TERRAIN_OBJ_TREE][0].z);
        terrain_free(t);
    }

    // the runtime: map_terrain, terrain_raycast, the ground snap of procInputs
    Game game;
    game_on(&game, json, MODE_ID_FFA);
    CHECK(map_terrain(game.map) != NULL, "map.terrain built (seed, width, height set)");
    float height = 0.0f;
    CHECK(world_terrain_height(&game, 0.0f, 0.0f, &height) && near(height, 257.729f, 1e-2f), "world height at the origin %.3f", height);
    float distance = 0.0f;
    CHECK(terrain_raycast(&game, (vec3) {0.0f, 400.0f, 0.0f}, (vec3) {0.0f, -1.0f, 0.0f}, 1000.0f, &distance) && near(distance, 400.0f - 257.729f, 1e-2f),
          "a world ray down hits after %.3f", distance);
    CHECK(!terrain_raycast(&game, (vec3) {0.0f, 400.0f, 0.0f}, (vec3) {0.0f, -1.0f, 0.0f}, 100.0f, &distance), "out of range: no hit");
    Player *p = add_player(&game, 1, 0);
    place(p, 0.0f, height + 3.0f, 0.0f);
    for (int i = 0; i < 60; i++) frame(p, 0);
    CHECK(near(p->position.y, height, 1e-3f) && p->on_ground && p->on_terrain, "ground snap: y %.4f on the terrain %.4f", p->position.y, height);
    CHECK(near(world_terrain_fraction(&game, (vec3) {0.0f, height + 10.0f, 0.0f}, (vec3) {0.0f, -20.0f, 0.0f}), 0.5f, 1e-3f), "terrain fraction 0.5");
    game_init(&game, 0, 0, 1);

    // ClashRoyale: the corpus' terrain map has an empty seed, so v7 builds no terrain
    cJSON *clash = load_map("assets/maps/custom/ClashRoyale.json");
    Map *map = map_init_ex(clash, 0);
    CHECK(map && !map_terrain(map), "ClashRoyale: terrainSeed \"\" -> no terrain");
    if (map) map_fini(map);
    free(map);
    cJSON_Delete(clash);
    cJSON_Delete(json);
}

// ---------------------------------------------------------------- BR zone

static void test_br_zone(void) {
    printf("Running battle-royale zone test...\n");
    cJSON *json = load_map("shared/tests/maps/v7_br_zone.json");
    Game game;
    game_on(&game, json, MODE_ID_FFA);
    WorldZone *zone = &game.world.zone;
    CHECK(zone->active && zone->stages == 2 && near(zone->scale.x, 200.0f, 0) && near(zone->target_scale.x, 100.0f, 0),
          "reset: stages 2, size 200 -> target 100");
    CHECK(near(zone->shrink_speed, 125.0f, 1e-4f) && near(zone->wait_time, 5000.0f, 0), "shrink 2.5 * 50, wait 5000 ms");
    Player *in = add_player(&game, 1, 0), *out = add_player(&game, 2, 0);
    place(in, 0.0f, 0.0f, 0.0f);
    place(out, 150.0f, 0.0f, 0.0f);

    g_sent_count = 0;
    float t = 0.0f;
    const float dt = 1.0f / 30.0f;
    game_tick(&game, game.now + dt, dt);
    t += dt;
    const Sent *zn = find_sent(NET_PACKET_ZONE, 0);
    CHECK(zn && zn->uid == NET_TARGET_ALL && zn->length == sizeof(NetZonePacket), "first tick broadcasts \"zn\"");
    // the height dilutes the radial rate: d(size)/dt = 125 * 100 / |(-100, -1000, -100)|
    const float rate = 125.0f * 100.0f / sqrtf(100.0f * 100.0f * 2.0f + 1000.0f * 1000.0f);
    int waiting_seen = 0, stage1_seen = 0;
    float out_health_at_first_damage = -1.0f;
    while (t < 30.0f) {
        place(in, 0.0f, 0.0f, 0.0f);
        place(out, 150.0f, 0.0f, 0.0f);
        const float before = out->health;
        game_tick(&game, game.now + dt, dt);
        t += dt;
        if (out->health < before && out_health_at_first_damage < 0.0f) out_health_at_first_damage = before - out->health;
        if (zone->waiting && !waiting_seen) {
            waiting_seen = 1;
            const float expected = (200.0f - 100.0f) / rate;
            CHECK(fabsf(t - expected) < 0.2f, "stage 1 reached at %.2f s (expected %.2f)", t, expected);
            CHECK(game.mode->wait_timer_count && game.mode->wait_timers[game.mode->wait_timer_count - 1].cont_time &&
                  !strcmp(game.mode->wait_timers[game.mode->wait_timer_count - 1].msg, "Zone closing in "), "\"Zone closing in \" waitTimer pushed");
        }
        if (waiting_seen && !stage1_seen && zone->stages == 1) {
            stage1_seen = 1;
            CHECK(near(zone->target_scale.x, zone->scale.x / 2.0f, 1e-3f), "next circle: half the size (%.2f -> %.2f)", zone->scale.x, zone->target_scale.x);
        }
        if (!out->active) break;
    }
    CHECK(waiting_seen && stage1_seen, "wait and next stage seen");
    CHECK(near(out_health_at_first_damage, 10.0f, 1e-4f), "outside: 10 damage per sync (%.1f)", out_health_at_first_damage);
    CHECK(in->health == (float) in->max_health, "inside: no damage");
    CHECK(count_sent(NET_PACKET_ZONE) >= 8, "\"zn\" every second (%d)", count_sent(NET_PACKET_ZONE));
    CHECK(zone_contains(&game, (vec3) {0.0f, 0.0f, 0.0f}) && !zone_contains(&game, (vec3) {150.0f, 0.0f, 0.0f}), "zone_contains");

    // join sync, round reset
    g_sent_count = 0;
    world_send_state(&game, 7);
    CHECK(find_sent(NET_PACKET_ZONE, 0) && find_sent(NET_PACKET_ZONE, 0)->uid == 7, "syncData.zone to a joiner");
    zone_reset(&game);
    CHECK(zone->stages == 2 && near(zone->scale.x, 200.0f, 0), "resetZone");

    // a client mirror tweens to each packet over 900 ms and hides at size 0
    Game client = {0};
    client.map = game.map;
    client.ready = 1;
    zone_reset(&client);
    float cx, cz, r, h;
    const NetZonePacket packet = {10.0f, 0.0f, 100.0f, 100.0f};
    zone_apply_packet(&client, &packet);
    zone_animate(&client, 0.45f);
    CHECK(zone_circle(&client, &cx, &cz, &r, &h) && near(r, 150.0f, 1e-3f) && near(cx, 5.0f, 1e-3f) && near(h, 1000.0f, 0), "tween half way: r %.2f", r);
    zone_animate(&client, 1.0f);
    CHECK(zone_circle(&client, &cx, &cz, &r, &h) && near(r, 100.0f, 1e-3f), "tween done: r %.2f", r);
    const NetZonePacket gone = {0.0f, 0.0f, 0.0f, 0.0f};
    zone_apply_packet(&client, &gone);
    CHECK(!zone_circle(&client, &cx, &cz, &r, &h), "size 0 hides the zone");
    client.map = NULL;
    game_init(&game, 0, 0, 1);
    cJSON_Delete(json);
}

// ---------------------------------------------------------------- zones, check points, resets

static void test_zones(void) {
    printf("Running zones / check points / resets test...\n");
    cJSON *json = load_map("shared/tests/maps/v7_zones.json");
    Game game;
    game_on(&game, json, MODE_ID_BHOP);
    CHECK(game.ready, "v7_zones ready");
    if (!game.ready) return;
    Player *p = add_player(&game, 1, 1);
    CHECK(p->has_init_pos, "setInitPos at the first spawn");

    // account / team / KR zones
    const Object *premium = object_at(&game, PREFAB_PREMIUM_ZONE, -100, 0), *verified = object_at(&game, PREFAB_VERIFIED_ZONE, -80, 0);
    const Object *team = object_at(&game, PREFAB_TEAM_ZONE, -60, 0), *kr = object_at(&game, PREFAB_KR_ZONE, -40, 0);
    const Object *partner = object_at(&game, PREFAB_PARTNER_ZONE, -20, 0), *kpd = object_at(&game, PREFAB_KPD_ZONE, 20, 0);
    CHECK(premium && verified && team && kr && partner && kpd, "zones parsed");
    if (premium && verified && team && kr && partner && kpd) {
        CHECK(!world_zone_passable(p, premium) && !world_zone_passable(p, verified) && !world_zone_passable(p, kr) &&
              !world_zone_passable(p, partner) && !world_zone_passable(p, kpd), "a guest: every account zone is solid");
        CHECK(world_zone_passable(p, team), "team zone, non-team mode: teamCheck passes");
        game.hooks.account_info = test_account;
        g_account = (AccountInfo) {.has_account = 1, .premium = 1, .partner = 1, .kr = 499};
        CHECK(world_zone_passable(p, premium) && world_zone_passable(p, partner) && !world_zone_passable(p, verified) &&
              !world_zone_passable(p, kpd) && !world_zone_passable(p, kr), "premium / partner pass, KR 499 < 500 is solid");
        g_account.kr = 500;
        g_account.verified = g_account.can_flag = 1;
        CHECK(world_zone_passable(p, kr) && world_zone_passable(p, verified) && world_zone_passable(p, kpd), "KR 500, verified, canFlag pass");
        game.hooks.account_info = NULL;
        // a guest walking into the premium zone is pushed out (solid)
        place(p, -100.0f + 5.0f + p->scale + 0.5f, 0.0f, 0.0f);
        const Input step = {.seq = p->input_seq + 1, .move_dir = -1, .delta = 1.0f / 60.0f};
        p->velocity.x = -0.05f;
        player_proc_input(p, &step, 0, 0);
        CHECK(near(p->position.x, -100.0f + 5.0f + p->scale, 0.01f), "premium zone solid for a guest (x %.3f)", p->position.x);
        // with premium the same step walks into it
        game.hooks.account_info = test_account;
        g_account = (AccountInfo) {.has_account = 1, .premium = 1};
        place(p, -100.0f + 5.0f + p->scale + 0.5f, 0.0f, 0.0f);
        const Input step2 = {.seq = p->input_seq + 1, .move_dir = -1, .delta = 1.0f / 60.0f};
        p->velocity.x = -0.05f;
        player_proc_input(p, &step2, 0, 0);
        CHECK(p->position.x < -100.0f + 5.0f + p->scale - 0.1f, "premium: passes into the zone (x %.3f)", p->position.x);
        game.hooks.account_info = NULL;
    }

    // NoPVP / PVP flags and the damage filter
    Player *q = add_player(&game, 2, 2);
    place(p, 40.0f, 0.0f, 0.0f);
    place(q, 0.0f, 0.0f, -30.0f);
    world_player_pvp_zones(p);
    world_player_pvp_zones(q);
    CHECK(p->no_pvp && !p->force_pvp && !q->no_pvp, "inside NOPVP_ZONE: noPVP");
    const float qh = q->health;
    CHECK(!player_change_health(q, p, 10.0f, 0) && q->health == qh, "noPVP attacker deals nothing");
    CHECK(!player_change_health(p, q, 10.0f, 0) && p->health == (float) p->max_health, "noPVP victim takes nothing");
    place(p, 70.0f, 0.0f, 0.0f);
    world_player_pvp_zones(p);
    CHECK(p->force_pvp && !p->no_pvp, "inside PVP_ZONE: forcePVP");
    CHECK(!world_pvp_allows(p, q) && !world_pvp_allows(q, p), "forcePVP fights only forcePVP");
    place(q, 72.0f, 0.0f, 2.0f);
    world_player_pvp_zones(q);
    CHECK(world_pvp_allows(p, q) && world_pvp_ignores_team(p, q), "both forcePVP: allowed, teams ignored");
    CHECK(!world_pvp_allows(NULL, q), "a world explosion spares forcePVP players");
    const float ph = p->health;
    game_explosion(&game, q->position, 20.0f, 50.0f, NULL, 8);
    CHECK(p->health == ph && q->health == qh, "explosion without an owner: forcePVP players untouched");
    place(p, 0.0f, 0.0f, 0.0f);
    place(q, 0.0f, 0.0f, -30.0f);
    world_player_pvp_zones(p);
    world_player_pvp_zones(q);
    CHECK(world_pvp_allows(p, q) && !p->force_pvp && !p->no_pvp, "outside both: normal rules");

    // score zones and logTime ("gt"), the final time
    p->score = 0;
    g_sent_count = 0;
    place(p, 0.0f, 0.0f, 60.0f);
    frame(p, 0);
    CHECK(p->score == 5 && p->time_log == 1, "score zone 1: +5, timeLog 1 (score %d, log %d)", p->score, p->time_log);
    const Sent *gt = find_sent(NET_PACKET_LOG_TIME, 0);
    CHECK(gt && gt->uid == p->uid && ((const NetLogTimePacket *) gt->payload)->kind == NET_LOG_TIME_ZONE &&
          ((const NetLogTimePacket *) gt->payload)->time_log == 1, "\"gt\" sent");
    frame(p, 0);
    CHECK(p->score == 5 && p->time_log == 1, "scoreObjs: once");
    place(p, 0.0f, 0.0f, 90.0f);
    frame(p, 0);
    CHECK(p->score == 10 && p->time_log == 2, "score zone 2");
    const Sent *fin = find_sent(NET_PACKET_LOG_TIME, 2);
    CHECK(fin && ((const NetLogTimePacket *) fin->payload)->kind == NET_LOG_TIME_FINISH, "last zone: the final time");
    const Sent *chat = find_sent(NET_PACKET_CHAT, 0);
    CHECK(chat && strstr(((const NetChatPacket *) chat->payload)->text, "P1 finished in "), "\"P1 finished in\" chat");

    // check points: checkPointData, single use, "chp"
    g_sent_count = 0;
    place(p, 40.0f, 0.0f, -60.0f);
    frame(p, 0);
    const Object *cp = object_at(&game, PREFAB_CHECK_POINT, 40, -60);
    CHECK(cp && p->checkpoint > 0 && game.map->objects[p->checkpoint - 1] == cp, "check point set");
    CHECK(p->has_checkpoint_data && p->checkpoint_time_log == 2, "checkPointData = [timeLog %d, gameTime]", p->checkpoint_time_log);
    const Sent *chp = find_sent(NET_PACKET_CHECKPOINT, 0);
    CHECK(chp && chp->length == sizeof(NetCheckpointPacket) && !((const NetCheckpointPacket *) chp->payload)->reset, "\"chp\"");

    // teleporter tcp: to the check point (its bottom + 0.01)
    place(p, 100.0f, 0.0f, -60.0f);
    frame(p, 0);
    CHECK(near(p->position.x, 40.0f, 0.5f) && near(p->position.z, -60.0f, 0.5f), "tcp teleporter: at the check point (%.2f, %.2f)", p->position.x, p->position.z);
    // a channel teleporter: sender -> receiver
    place(p, 130.0f, 0.0f, -60.0f);
    frame(p, 0);
    CHECK(near(p->position.x, 200.0f, 0.5f) && near(p->position.z, 200.0f, 0.5f), "channel 3: at the receiver (%.2f, %.2f)", p->position.x, p->position.z);

    // Parkour resets (input [13]): 2 resetToLast, 1 resetToStart
    place(p, 300.0f, 0.0f, 300.0f);
    frame(p, 2);
    CHECK(near(p->position.x, 40.0f, 1e-3f) && near(p->position.y, 0.01f, 1e-4f) && near(p->position.z, -60.0f, 1e-3f) && p->reset_timer > 0.0f,
          "resetToLast: (%.2f %.3f %.2f)", p->position.x, p->position.y, p->position.z);
    CHECK(near(p->direction.y, -2.0f * (float) M_PI / 2.0f, 1e-5f) || !cp->force_dir, "no forceDir: yaw kept");
    place(p, 300.0f, 0.0f, 300.0f);
    frame(p, 1);
    CHECK(near(p->position.x, 300.0f, 1e-3f), "resetTimer blocks a reset for 250 ms");
    for (int i = 0; i < 16; i++) frame(p, 0);
    g_sent_count = 0;
    frame(p, 1);
    CHECK(near(p->position.x, p->init_pos.x, 1e-3f) && near(p->position.z, p->init_pos.z, 1e-3f), "resetToStart: at initPos");
    CHECK(!p->checkpoint && !p->checkpoint_list_count && !p->has_checkpoint_data && !p->time_log && !p->score && !p->score_obj_count,
          "resetToStart clears check points, time, score, scoreObjs");
    const Sent *reset = find_sent(NET_PACKET_CHECKPOINT, 0);
    CHECK(reset && ((const NetCheckpointPacket *) reset->payload)->reset, "\"chp\" true");
    world_clear_checkpoints(&game, p);

    // collidingObjs: the onExitObject trigger is tracked while inside, dropped when left
    place(p, -100.0f, 0.0f, -60.0f);
    frame(p, 0);
    const Object *exit_obj = object_at(&game, PREFAB_TRIGGER, -100, -60);
    CHECK(exit_obj && game.world.save_colls && p->colliding_obj_count == 1, "inside the onExitObject trigger: collidingObjs 1 (saveColls %d)", game.world.save_colls);
    place(p, 0.0f, 0.0f, 0.0f);
    frame(p, 0);
    CHECK(p->colliding_obj_count == 0, "left it: collidingObjs empty");

    // node moves ("mo" / "mf"), the join list, round reset
    Object *cube = object_at(&game, PREFAB_CUBE, -150, 100);
    CHECK(cube != NULL, "the cube to move");
    if (cube) {
        g_sent_count = 0;
        CHECK(world_teleport_object_to_node(&game, cube, 3) && near(cube->position.x, -150.0f, 0) && near(cube->position.z, 150.0f, 0) &&
              near(cube->position.y, 0.0f, 0), "teleportToNode: feet on node 3");
        const Sent *mo = find_sent(NET_PACKET_OBJECT_STATE, 0);
        CHECK(mo && ((const NetObjectStatePacket *) mo->payload)->kind == NET_OBJECT_MOVE_TO_NODE && ((const NetObjectStatePacket *) mo->payload)->value == 3, "\"mo\"");
        CHECK(!world_teleport_object_to_node(&game, cube, 99), "no node 99");
        g_sent_count = 0;
        world_send_state(&game, 5);
        CHECK(find_sent(NET_PACKET_OBJECT_STATE, 0) && find_sent(NET_PACKET_OBJECT_STATE, 0)->uid == 5, "syncData.mov");
        world_reset_object_position(&game, cube);
        CHECK(near(cube->position.z, 100.0f, 0) && !game.world.moved_count, "teleportFromNode: home");
        world_teleport_object_to_node(&game, cube, 3);
        world_round_start(&game);
        CHECK(near(cube->position.z, 100.0f, 0) && !game.world.moved_count, "round start: moved objects go home");

        // object animations: a config.clipNames clip, a raw clip, a raw-less custom name
        g_sent_count = 0;
        world_play_anim(&game, cube, "Jump", 0);
        world_play_anim(&game, cube, "Wave", 1);
        world_play_anim(&game, cube, "Wave", 0);
        CHECK(count_sent(NET_PACKET_OBJECT_ANIM) == 2, "\"ana\": a clip name or a raw clip (%d)", count_sent(NET_PACKET_OBJECT_ANIM));
        const Sent *ana = find_sent(NET_PACKET_OBJECT_ANIM, 1);
        CHECK(ana && ((const NetObjectAnimPacket *) ana->payload)->raw && !strcmp(((const NetObjectAnimPacket *) ana->payload)->clip, "Wave"), "raw clip");
    }

    // builds: "bo", a client mirror with a destroyed-build gap, removeBuilds
    const size_t base = game.map->object_count;
    g_sent_count = 0;
    CHECK(game_build_object(&game, 0, (vec3) {0.0f, 0.0f, 30.0f}, (vec3) {0, 0, 0}, 0.0f, 0, p) && game.map->object_count == base + 1, "Build Block");
    CHECK(game_build_object(&game, 3, (vec3) {30.0f, 0.0f, 30.0f}, (vec3) {0, 0, 0}, (float) M_PI / 2.0f, 0, p), "Stairs");
    const Object *stairs = game.map->objects[base + 1];
    CHECK(stairs->ramp && stairs->direction == 1 && stairs->dynamic && stairs->health == 150.0f, "Stairs: a ramp facing PI / 2, 150 HP");
    const Sent *bo = find_sent(NET_PACKET_BUILD, 1);
    CHECK(bo && ((const NetBuildPacket *) bo->payload)->uid == (int32_t) (base + 1), "\"bo\" with the uid");
    game_destroy_object(&game, game.map->objects[base], p);
    g_sent_count = 0;
    world_send_state(&game, 9);
    CHECK(count_sent(NET_PACKET_BUILD) == 1, "syncData.dyno: the active builds only");
    Game mirror = {0};
    mirror.map = map_init_ex(json, 0);
    mirror.ready = 1;
    const size_t mirror_base = mirror.map->object_count;
    world_apply_build_packet(&mirror, (const NetBuildPacket *) find_sent(NET_PACKET_BUILD, 0)->payload);
    CHECK(mirror.map->object_count == base + 2 && mirror_base == base && !mirror.map->objects[base]->active &&
          mirror.map->objects[base + 1]->ramp, "mirror: the gap padded, the Stairs at its uid");
    world_remove_builds(&mirror);
    CHECK(mirror.map->object_count == mirror_base, "mirror removeBuilds");
    map_fini(mirror.map);
    free(mirror.map);
    world_round_start(&game);
    CHECK(game.map->object_count == base && !game.world.build_count, "round start: builds removed");

    // the Build Tool: aim at the floor, place a Build Block, then stack one on its top face
    place(p, 300.0f, 0.0f, 300.0f);
    p->score = 100;
    p->build_index = 0;
    p->select_build = 0;
    p->direction = (vec2) {-1.2f, 0.0f};
    CHECK(player_build(p, 1) && game.map->object_count == base + 1 && p->score == 90, "player_build: a block on the floor (score %d)", p->score);
    const Object *block = game.map->objects[game.map->object_count - 1];
    CHECK(block->dynamic && near(block->position.y, 0.0f, 0.51f) && near(block->scale.x, 10.0f, 0), "the block stands on the floor (y %.2f)", block->position.y);
    place(p, block->position.x, 0.0f, block->position.z + 3.0f);
    p->position.y = 12.0f; // above it, looking down onto its top
    p->direction = (vec2) {-1.5f, 0.0f};
    CHECK(player_build(p, 1) && game.map->object_count == base + 2, "a second block");
    const Object *top = game.map->objects[game.map->object_count - 1];
    CHECK(near(top->position.y, block->position.y + 10.0f, 1e-3f) && near(top->position.x, block->position.x, 1e-3f), "buildSnap Top: stacked (y %.2f)", top->position.y);
    p->score = 5;
    CHECK(!player_build(p, 1), "not enough score: nothing built");
    world_round_start(&game);

    // deathY: godMode survives, noclip does not
    p->god_mode = 1;
    place(p, 0.0f, game.map->death_y - 5.0f, 0.0f);
    game_tick(&game, game.now + 0.02f, 0.02f);
    CHECK(p->active, "godMode: no deathY kill");
    p->god_mode = 0;
    p->noclip = 1;
    place(p, 0.0f, game.map->death_y - 5.0f, 0.0f);
    game_tick(&game, game.now + 0.02f, 0.02f);
    CHECK(!p->active, "noclip below deathY dies (v7)");

    game_init(&game, 0, 0, 1);
    cJSON_Delete(json);
}

// ---------------------------------------------------------------- the ownTime clock

static void test_own_time(void) {
    printf("Running ownTime clock test...\n");
    cJSON *json = load_map("shared/tests/maps/v7_zones.json");
    Game game;
    game_on(&game, json, MODE_ID_BHOP);
    game.mode->config.game_time = 0.0f; // canOwnTime: a round without a clock
    Player *p = add_player(&game, 1, 0);
    p->covered_distance = 2.0f;
    g_sent_count = 0;
    for (int i = 0; i < 30; i++) game_tick(&game, game.now + 1.0f / 30.0f, 1.0f / 30.0f);
    CHECK(near(p->game_time, 1000.0f, 1.0f), "gameTime ran 1 s (%.1f ms)", p->game_time);
    const Sent *clock = find_sent(NET_PACKET_LOG_TIME, 0);
    CHECK(clock && ((const NetLogTimePacket *) clock->payload)->kind == NET_LOG_TIME_CLOCK, "speed-run clock \"t\"");
    CHECK(count_sent(NET_PACKET_LOG_TIME) >= 8 && count_sent(NET_PACKET_LOG_TIME) <= 11, "one per tenth (%d)", count_sent(NET_PACKET_LOG_TIME));
    game_init(&game, 0, 0, 1);
    cJSON_Delete(json);
}

// ---------------------------------------------------------------- wire bits

static void test_input_bits(void) {
    printf("Running input [13] / [14] wire test...\n");
    for (int reset = 0; reset <= 2; reset++) {
        const Input in = {.seq = 7, .move_dir = 2, .delta = 0.016f, .reset = (unsigned char) reset, .interact_held = (unsigned char) (reset & 1), .jump = 1};
        NetInputPacket packet;
        net_packet_from_input(&packet, &in);
        Input out;
        net_packet_to_input(&out, &packet);
        CHECK(out.reset == reset && out.interact_held == (reset & 1) && out.jump && out.seq == 7, "reset %d round trip", reset);
    }
}

int main(void) {
    test_tris();
    test_terrain();
    test_br_zone();
    test_zones();
    test_own_time();
    test_input_bits();
    if (g_failures) {
        printf("world_v7_test FAILED: %d of %d checks\n", g_failures, g_checks);
        return 1;
    }
    printf("world_v7_test OK (%d checks)\n", g_checks);
    return 0;
}
