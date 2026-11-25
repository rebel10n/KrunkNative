// The developer console's `ent_bbox` geometry (client/include/ent_bbox_geom.h): the movement boxes of the
// players and map objects, as GL_LINES vertex lists per layer. GL-free (client/tests/ent_bbox_test.c).
#include <ent_bbox_geom.h>
#include <bots.h> // bot_object_blocks_player: player_do_map_collisions' solid objects
#include <stdlib.h>
#include <string.h>

// self green, other players yellow, solid objects magenta, walk-through volumes orange
const int ent_bbox_layer_hex[ENT_BBOX_LAYERS] = {0x00ff00, 0xffff00, 0xff00ff, 0xff8000};

static int verts_push(EntBboxVerts *b, const float x, const float y, const float z) {
    if (b->n + 3 > b->cap) {
        const size_t cap = b->cap ? b->cap * 2 : (size_t) ENT_BBOX_BOX_VERTS * 3 * 32;
        float *grown = realloc(b->v, cap * sizeof(float));
        if (!grown) return 0;
        b->v = grown;
        b->cap = cap;
    }
    b->v[b->n++] = x;
    b->v[b->n++] = y;
    b->v[b->n++] = z;
    return 1;
}

// the 12 edges of the box min..max (bottom ring, top ring, the 4 uprights)
static int push_box(EntBboxVerts *b, const vec3 lo, const vec3 hi) {
    const float p[8][3] = {{lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z}, {hi.x, lo.y, hi.z}, {lo.x, lo.y, hi.z},
                           {lo.x, hi.y, lo.z}, {hi.x, hi.y, lo.z}, {hi.x, hi.y, hi.z}, {lo.x, hi.y, hi.z}};
    static const int e[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6}, {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (int i = 0; i < 12; i++) {
        if (!verts_push(b, p[e[i][0]][0], p[e[i][0]][1], p[e[i][0]][2]) || !verts_push(b, p[e[i][1]][0], p[e[i][1]][1], p[e[i][1]][2]))
            return 0;
    }
    return 1;
}

// player_do_map_collisions tests it: active, a collider, not complex (cpx collides through its tris)
static int object_drawn(const Object *o) {
    return o && o->active && !o->destroyed && o->collision_type != COLLISION_TYPE_NONE && !o->complex_coll;
}

size_t ent_bbox_geom_build(const Game *game, const Player *self, const int draw_self, EntBboxGeom *out) {
    for (int i = 0; i < ENT_BBOX_LAYERS; i++) out->layers[i].n = 0;
    out->players = out->objects = 0;
    if (!game) return 0;
    for (size_t i = 0; i < game->player_count; i++) {
        const Player *p = game->players[i];
        if (!p || !p->active) continue;
        const int is_self = p == self;
        if (is_self && !draw_self) continue;
        vec3 lo, hi;
        player_movement_bounds(p, &lo, &hi);
        if (push_box(&out->layers[is_self ? ENT_BBOX_SELF : ENT_BBOX_PLAYERS], lo, hi)) out->players++;
    }
    const Map *map = game->map;
    for (size_t i = 0; map && i < map->object_count; i++) {
        const Object *o = map->objects[i];
        if (!object_drawn(o)) continue;
        vec3 lo, hi;
        player_object_movement_bounds(o, &lo, &hi);
        const int solid = bot_object_blocks_player(o) && !o->no_player_col;
        if (push_box(&out->layers[solid ? ENT_BBOX_SOLID : ENT_BBOX_VOLUME], lo, hi)) out->objects++;
    }
    size_t total = 0;
    for (int i = 0; i < ENT_BBOX_LAYERS; i++) total += out->layers[i].n / 3;
    return total;
}

void ent_bbox_geom_free(EntBboxGeom *g) {
    for (int i = 0; i < ENT_BBOX_LAYERS; i++) {
        free(g->layers[i].v);
        memset(&g->layers[i], 0, sizeof(g->layers[i]));
    }
    g->players = g->objects = 0;
}
