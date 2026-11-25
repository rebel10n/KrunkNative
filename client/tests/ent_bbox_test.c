// ent_bbox_test: the developer console's `ent_bbox` geometry (client/src/ent_bbox_geom.c; Krunker v7.1.4
// mod/devConsole.ts:1392). A game on a small map with one object of each kind and three players; the boxes
// must be exactly the ones the movement code tests (player_object_movement_bounds / player_movement_bounds,
// shared/src/player.c), each as 12 edges, in the right layer, and nothing for what movement skips.
#include <ent_bbox_geom.h>
#include <cJSON.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;
#define CHECK(cond, ...)                                                                                                                                 \
    do {                                                                                                                                                 \
        if (!(cond)) {                                                                                                                                   \
            g_failures++;                                                                                                                                \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                                                  \
            printf(__VA_ARGS__);                                                                                                                         \
            printf("\n");                                                                                                                                \
        }                                                                                                                                                \
    } while (0)

static int near(const float a, const float b) { return fabsf(a - b) < 1e-3f; }

// the box spanned by a layer's vertices [first, first + 24)
static void layer_box(const EntBboxVerts *v, const size_t box, vec3 *lo, vec3 *hi) {
    *lo = (vec3) {1e30f, 1e30f, 1e30f};
    *hi = (vec3) {-1e30f, -1e30f, -1e30f};
    for (size_t k = box * ENT_BBOX_BOX_VERTS; k < (box + 1) * ENT_BBOX_BOX_VERTS && k * 3 + 2 < v->n; k++) {
        const float *p = v->v + k * 3;
        lo->x = fminf(lo->x, p[0]), lo->y = fminf(lo->y, p[1]), lo->z = fminf(lo->z, p[2]);
        hi->x = fmaxf(hi->x, p[0]), hi->y = fmaxf(hi->y, p[1]), hi->z = fmaxf(hi->z, p[2]);
    }
}

static int same_box(const vec3 a_lo, const vec3 a_hi, const vec3 b_lo, const vec3 b_hi) {
    return near(a_lo.x, b_lo.x) && near(a_lo.y, b_lo.y) && near(a_lo.z, b_lo.z) && near(a_hi.x, b_hi.x) && near(a_hi.y, b_hi.y) &&
           near(a_hi.z, b_hi.z);
}

// every vertex pair is an axis-aligned edge of its box (12 edges: 4 along each axis)
static int edges_ok(const EntBboxVerts *v, const size_t box) {
    int axis[3] = {0, 0, 0};
    for (size_t e = 0; e < 12; e++) {
        const float *a = v->v + (box * ENT_BBOX_BOX_VERTS + e * 2) * 3, *b = a + 3;
        const int dx = !near(a[0], b[0]), dy = !near(a[1], b[1]), dz = !near(a[2], b[2]);
        if (dx + dy + dz != 1) return 0;
        axis[dx ? 0 : dy ? 1 : 2]++;
    }
    return axis[0] == 4 && axis[1] == 4 && axis[2] == 4;
}

static Player *add_player(Game *game, const int uid, const char *name, const vec3 at) {
    Player *player = player_init(game);
    player->uid = uid;
    player->class_index = 0;
    snprintf(player->name, sizeof(player->name), "%s", name);
    game_players_add(game, player);
    player_spawn(player);
    player->position = at;
    return player;
}

int main(void) {
    printf("Running ent_bbox geometry test...\n");
    char json[2048];
    snprintf(json, sizeof(json),
             "{\"name\":\"Box Arena\",\"spawns\":[[0,0,0,0,0,0],[100,0,0,0,0,0]],\"objects\":["
             "{\"p\":[0,-10,0],\"s\":[400,10,400]},"                         // 0 the floor: a solid CUBE
             "{\"i\":%d,\"p\":[50,0,50],\"s\":[20,30,10]},"                  // 1 a CYLINDER: radius min(sx, sz) / 2
             "{\"i\":%d,\"p\":[-50,0,-50],\"s\":[40,80,40],\"hbw\":0.5,\"hbh\":0.25}," // 2 a CUSTOM_ASSET: s * hbw / hbh
             "{\"p\":[80,0,-80],\"s\":[10,10,10],\"l\":1},"                  // 3 noCol ("l"): no collider, not drawn
             "{\"i\":%d,\"p\":[-80,0,80],\"s\":[30,20,30]},"                 // 4 a SCORE_ZONE: walk-through (volume)
             "{\"i\":%d,\"p\":[0,0,120],\"s\":[10,1,10]}"                    // 5 a BOOST_PAD: p.y -+ max(sy, 3)
             "]}",
             PREFAB_CYLINDER, PREFAB_CUSTOM_ASSET, PREFAB_SCORE_ZONE, PREFAB_BOOST_PAD);
    cJSON *arena = cJSON_Parse(json);
    CHECK(arena != NULL, "map JSON parses");
    if (!arena) return 1;
    static const cJSON *maps[1];
    maps[0] = arena;
    static int modes[1];
    modes[0] = MODE_ID_FFA;
    static Game game;
    game_configure(&game, NULL, maps, 1, modes, 1, NULL, 0, NULL, 0);
    game.round_length = 100000.0f;
    game_init(&game, 0, 0, 1);
    CHECK(game.ready && game.map && game.map->object_count == 6, "game ready on the 6-object map (%zu objects)", game.map ? game.map->object_count : 0);
    if (!game.ready || !game.map || game.map->object_count != 6) return 1;
    const Object *const *obj = (const Object *const *) game.map->objects;
    CHECK(obj[3]->collision_type == COLLISION_TYPE_NONE && obj[1]->collision_type == COLLISION_TYPE_CYLINDER && obj[4]->score_zone &&
              obj[5]->jump_pad, "the objects parsed as intended");

    Player *me = add_player(&game, 1, "Me", (vec3) {0, 0, 0});
    Player *other = add_player(&game, 2, "Other", (vec3) {100, 0, 0});
    Player *dead = add_player(&game, 3, "Dead", (vec3) {-100, 0, 0});
    dead->active = 0;

    EntBboxGeom geom;
    memset(&geom, 0, sizeof(geom));
    size_t total = ent_bbox_geom_build(&game, me, 1, &geom);
    CHECK(geom.players == 2 && geom.objects == 5, "2 player boxes (the dead one left out), 5 object boxes (noCol left out): %d, %d", geom.players,
          geom.objects);
    CHECK(total == 7 * ENT_BBOX_BOX_VERTS, "24 vertices a box (%zu)", total);
    CHECK(geom.layers[ENT_BBOX_SELF].n == 3 * ENT_BBOX_BOX_VERTS && geom.layers[ENT_BBOX_PLAYERS].n == 3 * ENT_BBOX_BOX_VERTS,
          "one box each in self and players (%zu, %zu floats)", geom.layers[ENT_BBOX_SELF].n, geom.layers[ENT_BBOX_PLAYERS].n);
    CHECK(geom.layers[ENT_BBOX_SOLID].n == 3 * 3 * ENT_BBOX_BOX_VERTS, "solid: the floor, the cylinder, the asset (%zu floats)",
          geom.layers[ENT_BBOX_SOLID].n);
    CHECK(geom.layers[ENT_BBOX_VOLUME].n == 2 * 3 * ENT_BBOX_BOX_VERTS, "volume: the score zone, the boost pad (%zu floats)",
          geom.layers[ENT_BBOX_VOLUME].n);
    for (int l = 0; l < ENT_BBOX_LAYERS; l++)
        for (size_t b = 0; b < geom.layers[l].n / (3 * ENT_BBOX_BOX_VERTS); b++) CHECK(edges_ok(&geom.layers[l], b), "layer %d box %zu: 12 axis edges", l, b);

    // the boxes are the movement code's own
    vec3 lo, hi, want_lo, want_hi;
    layer_box(&geom.layers[ENT_BBOX_SELF], 0, &lo, &hi);
    player_movement_bounds(me, &want_lo, &want_hi);
    CHECK(same_box(lo, hi, want_lo, want_hi), "self: player_movement_bounds (%.2f %.2f %.2f .. %.2f %.2f %.2f)", lo.x, lo.y, lo.z, hi.x, hi.y, hi.z);
    const float s = me->scale > 0.0f ? me->scale : game_constants.player_scale;
    CHECK(hi.x - lo.x >= 2.0f * s - 1e-3f && near(lo.y, me->position.y) && hi.y - lo.y >= me->height - 1e-3f,
          "self: at least 2 * scale wide and the player's height tall from the feet (%.2f x %.2f)", hi.x - lo.x, hi.y - lo.y);
    layer_box(&geom.layers[ENT_BBOX_PLAYERS], 0, &lo, &hi);
    player_movement_bounds(other, &want_lo, &want_hi);
    CHECK(same_box(lo, hi, want_lo, want_hi) && near((lo.x + hi.x) * 0.5f, 100.0f), "the other player: its box at x 100");

    layer_box(&geom.layers[ENT_BBOX_SOLID], 0, &lo, &hi);
    CHECK(same_box(lo, hi, (vec3) {-200, -10, -200}, (vec3) {200, 0, 200}), "the floor: p bottom centre, full s (%.1f %.1f %.1f .. %.1f %.1f %.1f)", lo.x,
          lo.y, lo.z, hi.x, hi.y, hi.z);
    layer_box(&geom.layers[ENT_BBOX_SOLID], 1, &lo, &hi);
    CHECK(same_box(lo, hi, (vec3) {45, 0, 45}, (vec3) {55, 30, 55}), "the cylinder: radius min(20, 10) / 2 = 5 (%.1f .. %.1f)", lo.x, hi.x);
    layer_box(&geom.layers[ENT_BBOX_SOLID], 2, &lo, &hi);
    CHECK(same_box(lo, hi, (vec3) {-60, 0, -60}, (vec3) {-40, 20, -40}), "the asset: 40 * 0.5 wide, 80 * 0.25 tall (%.1f %.1f .. %.1f %.1f)", lo.x, lo.y,
          hi.x, hi.y);
    layer_box(&geom.layers[ENT_BBOX_VOLUME], 0, &lo, &hi);
    CHECK(same_box(lo, hi, (vec3) {-95, 0, 65}, (vec3) {-65, 20, 95}), "the score zone (%.1f %.1f .. %.1f %.1f)", lo.x, lo.y, hi.x, hi.y);
    layer_box(&geom.layers[ENT_BBOX_VOLUME], 1, &lo, &hi);
    player_object_movement_bounds(obj[5], &want_lo, &want_hi);
    CHECK(same_box(lo, hi, want_lo, want_hi) && near(lo.y, -3.0f) && near(hi.y, 3.0f), "the boost pad: p.y -+ max(1, 3) (%.1f .. %.1f)", lo.y, hi.y);

    // first person: no own box; a respawned player and a destroyed object follow at the next build
    ent_bbox_geom_build(&game, me, 0, &geom);
    CHECK(geom.players == 1 && geom.layers[ENT_BBOX_SELF].n == 0, "draw_self 0: no own box (%d players)", geom.players);
    dead->active = 1;
    ((Object *) obj[2])->active = 0;
    ent_bbox_geom_build(&game, me, 1, &geom);
    CHECK(geom.players == 3 && geom.objects == 4 && geom.layers[ENT_BBOX_SOLID].n == 2 * 3 * ENT_BBOX_BOX_VERTS,
          "the respawned player is in, the inactive asset out (%d players, %d objects)", geom.players, geom.objects);
    // the player moved: the box follows
    other->position = (vec3) {-30, 5, 10};
    ent_bbox_geom_build(&game, me, 1, &geom);
    layer_box(&geom.layers[ENT_BBOX_PLAYERS], 0, &lo, &hi);
    player_movement_bounds(other, &want_lo, &want_hi);
    CHECK(same_box(lo, hi, want_lo, want_hi) && near(lo.y, 5.0f), "a moved player's box follows it");
    CHECK(ent_bbox_geom_build(NULL, NULL, 1, &geom) == 0 && geom.players == 0 && geom.objects == 0, "no game: nothing");

    ent_bbox_geom_free(&geom);
    CHECK(geom.layers[0].v == NULL && geom.layers[0].n == 0, "free empties the layers");
    if (g_failures) {
        printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("Test OK\n");
    return 0;
}
