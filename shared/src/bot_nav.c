// Bot navigation grid: a C port of the idea of Krunker v7.1.4 shared/navmesh.ts + the bot
// worker's A* (bot/ai/state.ts findPath): walkable surfaces sampled on a grid from the map's
// solid boxes (support tops and ramps, a standing player's headroom), links between
// neighbouring cells (walk within climbHeight, jump up to a jumpable ledge, drop down), A* with
// a binary heap and a closest-reachable fallback, and line-of-ground path smoothing.
//
// Built lazily for the authority's current map the first time a bot needs a path (tens of
// milliseconds on the largest v3.1.1 maps), dropped on a map change. Dynamic objects
// (destructibles, gates) are static for the grid, like v7's first pass.
//
// v7.1.4's bots path over its generated Source-style nav areas instead (navgen.ts, ported as
// shared/src/navgen.c for the show_navmeshes overlay). KrunkNative keeps this grid on purpose:
// v7's area links climb up to 21 units (jumpHeight 18 + climbHeight 3) while a standing jump
// peaks near 14, and they ignore jump pads and kill volumes. With bots pathing on the generated
// mesh, bot_test and bots_e2e still passed, but in 120 s runs of 8 bots, stuck recoveries went
// from 2 to 62 on Shipyard and from 17 to 47 on Little Town (doc/console.md "show_navmeshes").
#include <bots.h>
#include <pcg_basic.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NAV_CELL 8.0f            // world units per cell (v7 cellSize 12: v3.1.1 doorways need finer)
#define NAV_RADIUS 3.6f          // playerScale 2.45 + collPad 1.1
#define NAV_HEIGHT 11.0f         // config.playerHeight
#define NAV_STEP 3.5f            // config.climbHeight 3 (+ tolerance)
#define NAV_JUMP 8.5f            // the highest ledge a standing jump reaches
#define NAV_DROP 64.0f           // the deepest drop a link takes
#define NAV_LAYERS 6             // surfaces per cell
#define NAV_BUCKET 32.0f         // spatial hash bucket of the solid boxes
#define NAV_MAX_CELLS 400000
#define NAV_JUMP_COST 12.0f
#define NAV_DROP_COST 6.0f
#define NAV_VOID_COST 40.0f      // per step on a node beside a fall out of the map
#define NAV_EDGE_MARGIN 2.0f     // a node needs ground this far around it (no standing on the lip)

typedef struct {
    float x0, x1, y0, y1, z0, z1;
    unsigned char ramp;
    float rsx, rsz, rex, rez;    // ramp start / end (x, z)
} NavBox;

typedef struct {
    float y;
    int32_t link[8];             // node per direction, -1 none
    uint8_t kind[8];             // 0 walk, 1 jump, 2 drop
    int32_t component;
    uint8_t void_edge;           // an orthogonal neighbour cell has nothing to land on
} NavNode;

struct BotNav_t {
    const void *key;             // Map.raw_data the grid was built for
    size_t key_objects;
    float x0, z0;
    int w, h;                    // cells
    int32_t *cell_first;         // w * h + 1 offsets into nodes
    NavNode *nodes;
    int node_count;
    NavBox *boxes;
    int box_count;
    int32_t *bucket_first;       // spatial hash: bw * bh + 1 offsets into bucket_items
    int32_t *bucket_items;
    int bw, bh;
    float bx0, bz0;
    int largest_component;
    // A* scratch
    float *g;
    int32_t *parent;
    uint32_t *stamp;
    uint32_t generation;
    int32_t *heap;
    float *heap_f;
};

static const int k_dx[8] = {1, -1, 0, 0, 1, 1, -1, -1};
static const int k_dz[8] = {0, 0, 1, -1, 1, -1, 1, -1};

// What blocks / carries a player: player_do_map_collisions' else-chain (jump pads, zones,
// objectives, teleporters, check points, pickups, flags, kill volumes, ladders and onEnter /
// onInteract triggers are walk-through), plus the collider-less helper prefabs.
int bot_object_blocks_player(const Object *o) {
    if (!o->active) return 0;
    if (o->collision_type == COLLISION_TYPE_NONE && !o->force_col) return 0;
    if (o->jump_pad || o->score_zone || o->objective || o->bomb_site || o->premium || o->verified || o->team_zone || o->teleporter ||
        o->checkpoint || o->pickup || o->flag || o->kill || o->ladder) return 0;
    const int event = game_object_event(o);
    if (event == TRIGGER_EVENT_ON_ENTER || event == TRIGGER_EVENT_ON_INTERACT) return 0;
    switch (o->prefab) {
        case PREFAB_SPAWN_POINT:
        case PREFAB_CAMERA_POSITION:
        case PREFAB_SPECTATE_CAM:
            return 0;
        default:
            return 1;
    }
}

#define nav_box_solid bot_object_blocks_player

static float nav_ramp_y(const NavBox *b, const float x, const float z) {
    const float dx = b->rex - b->rsx, dz = b->rez - b->rsz;
    const float len = dx * dx + dz * dz;
    float t = len > 0.0f ? ((x - b->rsx) * dx + (z - b->rsz) * dz) / len : 0.0f;
    t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
    return b->y0 + (b->y1 - b->y0) * t;
}

// The top a standing player meets inside the box at (x, z): the ramp surface or the top.
static float nav_box_top(const NavBox *b, const float x, const float z) { return b->ramp ? nav_ramp_y(b, x, z) : b->y1; }

static int nav_bucket_index(const BotNav *nav, const float x, const float z) {
    int bx = (int) floorf((x - nav->bx0) / NAV_BUCKET), bz = (int) floorf((z - nav->bz0) / NAV_BUCKET);
    if (bx < 0) bx = 0;
    if (bz < 0) bz = 0;
    if (bx >= nav->bw) bx = nav->bw - 1;
    if (bz >= nav->bh) bz = nav->bh - 1;
    return bz * nav->bw + bx;
}

// Visit every box whose bucket overlaps [x0, x1] x [z0, z1] (a box may be visited twice).
#define NAV_FOR_BOXES(nav, X0, X1, Z0, Z1, ...) do { \
    const int _a = nav_bucket_index(nav, X0, Z0), _b = nav_bucket_index(nav, X1, Z1); \
    const int _ax = _a % (nav)->bw, _az = _a / (nav)->bw, _bx = _b % (nav)->bw, _bz = _b / (nav)->bw; \
    for (int _z = _az; _z <= _bz; _z++) for (int _x = _ax; _x <= _bx; _x++) { \
        const int _k = _z * (nav)->bw + _x; \
        for (int _i = (nav)->bucket_first[_k]; _i < (nav)->bucket_first[_k + 1]; _i++) { \
            const NavBox *box = &(nav)->boxes[(nav)->bucket_items[_i]]; \
            __VA_ARGS__ \
        } \
    } \
} while (0)

// A standing body at (x, y, z): nothing solid between y + NAV_STEP and y + NAV_HEIGHT.
static int nav_clear(const BotNav *nav, const float x, const float y, const float z) {
    const float r = NAV_RADIUS;
    NAV_FOR_BOXES(nav, x - r, x + r, z - r, z + r, {
        if (box->x1 < x - r || box->x0 > x + r || box->z1 < z - r || box->z0 > z + r) continue;
        const float top = box->ramp ? nav_box_top(box, x, z) : box->y1;
        if (box->y0 < y + NAV_HEIGHT && top > y + NAV_STEP) return 0;
    });
    return 1;
}

// Ground near height y under the point (x, z): a top (or ramp surface) within a step of it
// (stairs are thin boxes a step apart; a lip over a fall has nothing).
static int nav_supported_point(const BotNav *nav, const float x, const float y, const float z) {
    NAV_FOR_BOXES(nav, x, x, z, z, {
        if (x < box->x0 || x > box->x1 || z < box->z0 || z > box->z1) continue;
        const float top = nav_box_top(box, x, z);
        if (top >= y - NAV_STEP && top <= y + NAV_STEP) return 1;
    });
    return 0;
}

// The whole footprint stands on ground (any boxes, so tiled floors count).
static int nav_supported(const BotNav *nav, const float x, const float y, const float z) {
    const float m = NAV_EDGE_MARGIN;
    return nav_supported_point(nav, x - m, y, z - m) && nav_supported_point(nav, x + m, y, z - m) &&
           nav_supported_point(nav, x - m, y, z + m) && nav_supported_point(nav, x + m, y, z + m);
}

// The segment a -> b (feet points) at waist height crosses no wall.
static int nav_segment_clear(const BotNav *nav, const vec3 a, const vec3 b, const float lift) {
    const float ax = a.x, ay = a.y + lift, az = a.z, dx = b.x - a.x, dy = (b.y + lift) - ay, dz = b.z - a.z;
    const float minx = MIN(a.x, b.x), maxx = MAX(a.x, b.x), minz = MIN(a.z, b.z), maxz = MAX(a.z, b.z);
    NAV_FOR_BOXES(nav, minx, maxx, minz, maxz, {
        if (box->ramp) continue;
        float t0 = 0.0f, t1 = 1.0f;
        const float o[3] = {ax, ay, az}, d[3] = {dx, dy, dz}, lo[3] = {box->x0, box->y0, box->z0}, hi[3] = {box->x1, box->y1, box->z1};
        int hit = 1;
        for (int k = 0; k < 3 && hit; k++) {
            if (fabsf(d[k]) < 1e-6f) {
                if (o[k] < lo[k] || o[k] > hi[k]) hit = 0;
                continue;
            }
            float n = (lo[k] - o[k]) / d[k], f = (hi[k] - o[k]) / d[k];
            if (n > f) { const float t = n; n = f; f = t; }
            if (n > t0) t0 = n;
            if (f < t1) t1 = f;
            if (t0 > t1) hit = 0;
        }
        if (hit && t1 > 0.0f && t0 < 1.0f) return 0;
    });
    return 1;
}

static vec3 nav_node_pos(const BotNav *nav, const int index) {
    // node index -> cell by binary search of cell_first
    int lo = 0, hi = nav->w * nav->h;
    while (lo + 1 < hi) {
        const int mid = (lo + hi) / 2;
        if (nav->cell_first[mid] <= index) lo = mid;
        else hi = mid;
    }
    const int cx = lo % nav->w, cz = lo / nav->w;
    return (vec3) {nav->x0 + ((float) cx + 0.5f) * NAV_CELL, nav->nodes[index].y, nav->z0 + ((float) cz + 0.5f) * NAV_CELL};
}

void bot_nav_free(BotNav *nav) {
    if (!nav) return;
    free(nav->cell_first);
    free(nav->nodes);
    free(nav->boxes);
    free(nav->bucket_first);
    free(nav->bucket_items);
    free(nav->g);
    free(nav->parent);
    free(nav->stamp);
    free(nav->heap);
    free(nav->heap_f);
    free(nav);
}

typedef struct {
    int32_t cell;
    float y;
} NavCandidate;

static int nav_candidate_cmp(const void *a, const void *b) {
    const NavCandidate *x = a, *y = b;
    if (x->cell != y->cell) return x->cell < y->cell ? -1 : 1;
    return x->y < y->y ? -1 : x->y > y->y ? 1 : 0;
}

// Jump pads and kill volumes: no node may stand inside one (expanded by the body radius).
static int nav_hazard(const Map *map, const float x, const float y, const float z) {
    for (size_t i = 0; i < map->object_count; i++) {
        const Object *o = map->objects[i];
        if (!o->active || !(o->jump_pad || o->kill || (o->ramp && o->ramp->boost))) continue;
        const float hx = o->scale.x * 0.5f + NAV_RADIUS, hz = o->scale.z * 0.5f + NAV_RADIUS;
        if (x < o->position.x - hx || x > o->position.x + hx || z < o->position.z - hz || z > o->position.z + hz) continue;
        if (y + NAV_HEIGHT < o->position.y || y > o->position.y + o->scale.y + 1.0f) continue;
        return 1;
    }
    return 0;
}

int bot_hazard_at(const Game *game, const vec3 p) { return game->map && nav_hazard(game->map, p.x, p.y, p.z); }

static BotNav *nav_build(const Map *map) {
    BotNav *nav = calloc(1, sizeof(BotNav));
    if (!nav) return NULL;
    nav->key = map->raw_data;
    nav->key_objects = map->object_count;

    // solid boxes
    nav->boxes = calloc(map->object_count ? map->object_count : 1, sizeof(NavBox));
    if (!nav->boxes) goto fail;
    float bx0 = 1e9f, bx1 = -1e9f, bz0 = 1e9f, bz1 = -1e9f;
    for (size_t i = 0; i < map->object_count; i++) {
        const Object *o = map->objects[i];
        if (!nav_box_solid(o) || o->scale.x <= 0.0f || o->scale.z <= 0.0f) continue;
        NavBox *b = &nav->boxes[nav->box_count++];
        b->x0 = o->position.x - o->scale.x * 0.5f;
        b->x1 = o->position.x + o->scale.x * 0.5f;
        b->z0 = o->position.z - o->scale.z * 0.5f;
        b->z1 = o->position.z + o->scale.z * 0.5f;
        b->y0 = o->position.y;
        b->y1 = o->position.y + (o->scale.y > 0.01f ? o->scale.y : 0.01f);
        if (o->ramp) {
            b->ramp = 1;
            b->rsx = o->ramp->start.x;
            b->rsz = o->ramp->start.y;
            b->rex = o->ramp->end.x;
            b->rez = o->ramp->end.y;
        }
        if (b->x1 - b->x0 > 6000.0f || b->z1 - b->z0 > 6000.0f) continue; // skybox-sized: no bounds
        bx0 = MIN(bx0, b->x0);
        bx1 = MAX(bx1, b->x1);
        bz0 = MIN(bz0, b->z0);
        bz1 = MAX(bz1, b->z1);
    }
    if (!nav->box_count || bx1 <= bx0 || bz1 <= bz0) goto fail;

    // spatial hash
    nav->bx0 = bx0 - NAV_BUCKET;
    nav->bz0 = bz0 - NAV_BUCKET;
    nav->bw = (int) ceilf((bx1 - bx0) / NAV_BUCKET) + 3;
    nav->bh = (int) ceilf((bz1 - bz0) / NAV_BUCKET) + 3;
    nav->bucket_first = calloc((size_t) nav->bw * nav->bh + 1, sizeof(int32_t));
    if (!nav->bucket_first) goto fail;
    size_t items = 0;
    for (int pass = 0; pass < 2; pass++) {
        int32_t *fill = NULL;
        if (pass == 1) {
            for (int k = 0; k < nav->bw * nav->bh; k++) nav->bucket_first[k + 1] += nav->bucket_first[k];
            items = (size_t) nav->bucket_first[nav->bw * nav->bh];
            nav->bucket_items = calloc(items ? items : 1, sizeof(int32_t));
            fill = calloc((size_t) nav->bw * nav->bh, sizeof(int32_t));
            if (!nav->bucket_items || !fill) {
                free(fill);
                goto fail;
            }
        }
        for (int i = 0; i < nav->box_count; i++) {
            const NavBox *b = &nav->boxes[i];
            const int a = nav_bucket_index(nav, b->x0, b->z0), c = nav_bucket_index(nav, b->x1, b->z1);
            for (int z = a / nav->bw; z <= c / nav->bw; z++) {
                for (int x = a % nav->bw; x <= c % nav->bw; x++) {
                    const int k = z * nav->bw + x;
                    if (pass == 0) nav->bucket_first[k + 1]++;
                    else nav->bucket_items[nav->bucket_first[k] + fill[k]++] = i;
                }
            }
        }
        free(fill);
    }

    // grid
    nav->x0 = bx0;
    nav->z0 = bz0;
    nav->w = (int) ceilf((bx1 - bx0) / NAV_CELL);
    nav->h = (int) ceilf((bz1 - bz0) / NAV_CELL);
    if (nav->w <= 0 || nav->h <= 0 || (long) nav->w * nav->h > NAV_MAX_CELLS) goto fail;

    // support candidates: every solid top over a cell centre
    size_t cap = 4096, count = 0;
    NavCandidate *cands = malloc(cap * sizeof(NavCandidate));
    if (!cands) goto fail;
    for (int i = 0; i < nav->box_count; i++) {
        const NavBox *b = &nav->boxes[i];
        const int cx0 = MAX(0, (int) ceilf((b->x0 - nav->x0) / NAV_CELL - 0.5f)), cx1 = MIN(nav->w - 1, (int) floorf((b->x1 - nav->x0) / NAV_CELL - 0.5f));
        const int cz0 = MAX(0, (int) ceilf((b->z0 - nav->z0) / NAV_CELL - 0.5f)), cz1 = MIN(nav->h - 1, (int) floorf((b->z1 - nav->z0) / NAV_CELL - 0.5f));
        for (int cz = cz0; cz <= cz1; cz++) {
            for (int cx = cx0; cx <= cx1; cx++) {
                if (count == cap) {
                    NavCandidate *grown = realloc(cands, cap * 2 * sizeof(NavCandidate));
                    if (!grown) {
                        free(cands);
                        goto fail;
                    }
                    cands = grown;
                    cap *= 2;
                }
                const float x = nav->x0 + ((float) cx + 0.5f) * NAV_CELL, z = nav->z0 + ((float) cz + 0.5f) * NAV_CELL;
                cands[count++] = (NavCandidate) {cz * nav->w + cx, nav_box_top(b, x, z)};
            }
        }
    }
    qsort(cands, count, sizeof(NavCandidate), nav_candidate_cmp);

    nav->cell_first = calloc((size_t) nav->w * nav->h + 1, sizeof(int32_t));
    nav->nodes = calloc(count ? count : 1, sizeof(NavNode));
    if (!nav->cell_first || !nav->nodes) {
        free(cands);
        goto fail;
    }
    size_t c = 0;
    for (int cell = 0; cell < nav->w * nav->h; cell++) {
        nav->cell_first[cell] = nav->node_count;
        const float x = nav->x0 + ((float) (cell % nav->w) + 0.5f) * NAV_CELL, z = nav->z0 + ((float) (cell / nav->w) + 0.5f) * NAV_CELL;
        float last = -1e9f;
        int layers = 0;
        for (; c < count && cands[c].cell == cell; c++) {
            const float y = cands[c].y;
            if (y - last < 1.0f || layers >= NAV_LAYERS || y < map->death_y + 2.0f) continue;
            if (!nav_clear(nav, x, y, z) || nav_hazard(map, x, y, z) || !nav_supported(nav, x, y, z)) continue;
            NavNode *node = &nav->nodes[nav->node_count++];
            node->y = y;
            node->component = -1;
            for (int d = 0; d < 8; d++) node->link[d] = -1;
            last = y;
            layers++;
        }
    }
    nav->cell_first[nav->w * nav->h] = nav->node_count;
    free(cands);
    if (!nav->node_count) goto fail;

    // links
    for (int cell = 0; cell < nav->w * nav->h; cell++) {
        const int cx = cell % nav->w, cz = cell / nav->w;
        for (int a = nav->cell_first[cell]; a < nav->cell_first[cell + 1]; a++) {
            NavNode *na = &nav->nodes[a];
            const vec3 pa = {nav->x0 + ((float) cx + 0.5f) * NAV_CELL, na->y, nav->z0 + ((float) cz + 0.5f) * NAV_CELL};
            for (int d = 0; d < 8; d++) {
                const int nx = cx + k_dx[d], nz = cz + k_dz[d];
                if (nx < 0 || nz < 0 || nx >= nav->w || nz >= nav->h) continue;
                if (d >= 4) { // diagonals: both orthogonal neighbours walkable at about this height (no corner cutting)
                    int ok = 0;
                    for (int s = 0; s < 2; s++) {
                        const int oc = s ? cz * nav->w + nx : nz * nav->w + cx;
                        for (int o = nav->cell_first[oc]; o < nav->cell_first[oc + 1]; o++) ok += fabsf(nav->nodes[o].y - na->y) <= NAV_STEP;
                    }
                    if (ok < 2) continue;
                }
                const int ncell = nz * nav->w + nx;
                int best = -1, kind = 3;
                float best_dy = 0.0f;
                for (int b = nav->cell_first[ncell]; b < nav->cell_first[ncell + 1]; b++) {
                    const float dy = nav->nodes[b].y - na->y;
                    int k;
                    if (fabsf(dy) <= NAV_STEP) k = 0;
                    else if (dy > 0.0f && dy <= NAV_JUMP) k = 1;
                    else if (dy < 0.0f && dy >= -NAV_DROP) k = 2;
                    else continue;
                    // walk beats jump beats drop; among drops the shallowest
                    if (k < kind || (k == kind && fabsf(dy) < fabsf(best_dy))) {
                        const vec3 pb = {nav->x0 + ((float) nx + 0.5f) * NAV_CELL, nav->nodes[b].y, nav->z0 + ((float) nz + 0.5f) * NAV_CELL};
                        const float lift = k == 1 ? dy + NAV_STEP + 0.5f : NAV_STEP + 0.5f; // a jump clears the ledge
                        if (!nav_segment_clear(nav, pa, pb, k == 2 ? NAV_HEIGHT - 2.0f : lift)) continue;
                        best = b;
                        kind = k;
                        best_dy = dy;
                    }
                }
                if (best >= 0) {
                    na->link[d] = best;
                    na->kind[d] = (uint8_t) kind;
                }
            }
        }
    }

    // void edges: an orthogonal neighbour cell with no surface at or below this one
    for (int cell = 0; cell < nav->w * nav->h; cell++) {
        const int cx = cell % nav->w, cz = cell / nav->w;
        for (int a = nav->cell_first[cell]; a < nav->cell_first[cell + 1]; a++) {
            for (int d = 0; d < 4 && !nav->nodes[a].void_edge; d++) {
                const int nx = cx + k_dx[d], nz = cz + k_dz[d];
                int below = 0;
                if (nx >= 0 && nz >= 0 && nx < nav->w && nz < nav->h) {
                    const int ncell = nz * nav->w + nx;
                    for (int b = nav->cell_first[ncell]; b < nav->cell_first[ncell + 1] && !below; b++) below = nav->nodes[b].y <= nav->nodes[a].y + NAV_STEP;
                }
                if (!below) nav->nodes[a].void_edge = 1;
            }
        }
    }

    // components over walk / jump links both ways (drops are one-way: not merged)
    int32_t *stack = malloc((size_t) nav->node_count * sizeof(int32_t));
    if (!stack) goto fail;
    int components = 0, largest = 0, largest_size = 0;
    for (int s = 0; s < nav->node_count; s++) {
        if (nav->nodes[s].component >= 0) continue;
        int top = 0, size = 0;
        stack[top++] = s;
        nav->nodes[s].component = components;
        while (top) {
            const int n = stack[--top];
            size++;
            for (int d = 0; d < 8; d++) {
                const int m = nav->nodes[n].link[d];
                if (m < 0 || nav->nodes[n].kind[d] == 2 || nav->nodes[m].component >= 0) continue;
                nav->nodes[m].component = components;
                stack[top++] = m;
            }
        }
        if (size > largest_size) {
            largest_size = size;
            largest = components;
        }
        components++;
    }
    free(stack);
    nav->largest_component = largest;

    nav->g = malloc((size_t) nav->node_count * sizeof(float));
    nav->parent = malloc((size_t) nav->node_count * sizeof(int32_t));
    nav->stamp = calloc((size_t) nav->node_count, sizeof(uint32_t));
    nav->heap = malloc((size_t) nav->node_count * 8 * sizeof(int32_t));
    nav->heap_f = malloc((size_t) nav->node_count * 8 * sizeof(float));
    if (!nav->g || !nav->parent || !nav->stamp || !nav->heap || !nav->heap_f) goto fail;
    return nav;

fail:
    bot_nav_free(nav);
    return NULL;
}

BotNav *bot_nav_for(Game *game) {
    GameBots *bots = game_bots(game);
    if (!bots || !game->map) return NULL;
    BotNav *nav = bots->nav;
    if (nav && nav->key == game->map->raw_data && nav->key_objects == game->map->object_count) return nav;
    bot_nav_free(nav);
    bots->nav = NULL;
    if (bots->nav_failed_key == game->map->raw_data) return NULL;
    bots->nav = nav_build(game->map);
    if (!bots->nav) bots->nav_failed_key = game->map->raw_data;
    return bots->nav;
}

int bot_nav_node_count(const BotNav *nav) { return nav ? nav->node_count : 0; }

// The node a player at `p` stands on: its cell (else the nearest cells around it), the
// highest surface at or below the feet (+ a step).
static int nav_nearest(const BotNav *nav, const vec3 p, const int search) {
    const int cx = (int) floorf((p.x - nav->x0) / NAV_CELL), cz = (int) floorf((p.z - nav->z0) / NAV_CELL);
    int best = -1;
    float best_score = 1e9f;
    for (int r = 0; r <= search && best < 0; r++) {
        for (int dz = -r; dz <= r; dz++) {
            for (int dx = -r; dx <= r; dx++) {
                if (MAX(abs(dx), abs(dz)) != r) continue;
                const int x = cx + dx, z = cz + dz;
                if (x < 0 || z < 0 || x >= nav->w || z >= nav->h) continue;
                const int cell = z * nav->w + x;
                for (int n = nav->cell_first[cell]; n < nav->cell_first[cell + 1]; n++) {
                    const float dy = p.y - nav->nodes[n].y;
                    // a surface under the feet (mid-jump, on a ladder) beats one overhead by far
                    const float score = (dy >= -NAV_STEP ? dy : 40.0f - dy * 20.0f) + (float) (abs(dx) + abs(dz)) * NAV_CELL;
                    if (score < best_score) {
                        best_score = score;
                        best = n;
                    }
                }
            }
        }
    }
    return best;
}

static void heap_push(BotNav *nav, int *size, const int node, const float f) {
    int i = (*size)++;
    while (i > 0) {
        const int parent = (i - 1) / 2;
        if (nav->heap_f[parent] <= f) break;
        nav->heap[i] = nav->heap[parent];
        nav->heap_f[i] = nav->heap_f[parent];
        i = parent;
    }
    nav->heap[i] = node;
    nav->heap_f[i] = f;
}

static int heap_pop(BotNav *nav, int *size) {
    const int top = nav->heap[0];
    const int last = --(*size);
    const int node = nav->heap[last];
    const float f = nav->heap_f[last];
    int i = 0;
    for (;;) {
        int child = i * 2 + 1;
        if (child >= last) break;
        if (child + 1 < last && nav->heap_f[child + 1] < nav->heap_f[child]) child++;
        if (nav->heap_f[child] >= f) break;
        nav->heap[i] = nav->heap[child];
        nav->heap_f[i] = nav->heap_f[child];
        i = child;
    }
    if (last > 0) {
        nav->heap[i] = node;
        nav->heap_f[i] = f;
    }
    return top;
}

// Walkable straight from a to b at about one height: every sample over a node within a step.
static int nav_ground_line(const BotNav *nav, const vec3 a, const vec3 b) {
    const float len = hypotf(b.x - a.x, b.z - a.z);
    const int steps = (int) (len / (NAV_CELL * 0.5f)) + 1;
    for (int i = 1; i < steps; i++) {
        const float t = (float) i / (float) steps;
        const vec3 p = {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
        const int cx = (int) floorf((p.x - nav->x0) / NAV_CELL), cz = (int) floorf((p.z - nav->z0) / NAV_CELL);
        if (cx < 0 || cz < 0 || cx >= nav->w || cz >= nav->h) return 0;
        int ok = 0;
        const int cell = cz * nav->w + cx;
        for (int n = nav->cell_first[cell]; n < nav->cell_first[cell + 1] && !ok; n++) ok = fabsf(nav->nodes[n].y - p.y) <= 1.5f;
        if (!ok) return 0;
    }
    return nav_segment_clear(nav, a, b, NAV_STEP + 0.5f) && nav_segment_clear(nav, a, b, NAV_HEIGHT - 2.0f);
}

int bot_nav_find_path(Game *game, const vec3 from, const vec3 to, BotPath *out) {
    out->count = 0;
    BotNav *nav = bot_nav_for(game);
    if (!nav) return 0;
    const int start = nav_nearest(nav, from, 3), goal = nav_nearest(nav, to, 4);
    if (start < 0 || goal < 0) return 0;
    const vec3 goal_pos = nav_node_pos(nav, goal);

    if (++nav->generation == 0) {
        memset(nav->stamp, 0, (size_t) nav->node_count * sizeof(uint32_t));
        nav->generation = 1;
    }
    const uint32_t gen = nav->generation;
    int size = 0, expanded = 0;
    int closest = start;
    float closest_h = 1e9f;
    nav->stamp[start] = gen;
    nav->g[start] = 0.0f;
    nav->parent[start] = -1;
    heap_push(nav, &size, start, 0.0f);
    int found = 0;
    // positions: recomputing cells per node is a binary search; cache via the expansion cell
    while (size && expanded < 60000) {
        const int n = heap_pop(nav, &size);
        expanded++;
        const vec3 pn = nav_node_pos(nav, n);
        const float h = hypotf(goal_pos.x - pn.x, goal_pos.z - pn.z) + fabsf(goal_pos.y - pn.y);
        if (h < closest_h) {
            closest_h = h;
            closest = n;
        }
        if (n == goal) {
            found = 1;
            break;
        }
        for (int d = 0; d < 8; d++) {
            const int m = nav->nodes[n].link[d];
            if (m < 0) continue;
            const float step = d < 4 ? NAV_CELL : NAV_CELL * 1.41421356f;
            const float cost = nav->g[n] + step + (nav->nodes[n].kind[d] == 1 ? NAV_JUMP_COST : nav->nodes[n].kind[d] == 2 ? NAV_DROP_COST : 0.0f) +
                               (nav->nodes[m].void_edge ? NAV_VOID_COST : 0.0f);
            if (nav->stamp[m] == gen && nav->g[m] <= cost) continue;
            nav->stamp[m] = gen;
            nav->g[m] = cost;
            nav->parent[m] = n;
            const vec3 pm = {pn.x + (float) k_dx[d] * NAV_CELL, nav->nodes[m].y, pn.z + (float) k_dz[d] * NAV_CELL};
            heap_push(nav, &size, m, cost + hypotf(goal_pos.x - pm.x, goal_pos.z - pm.z));
            if (size >= nav->node_count * 8 - 8) break;
        }
    }
    const int end = found ? goal : closest; // the closest reachable area when the goal is not
    // walk back (reversed into raw)
    static vec3 raw[4096];
    static uint8_t raw_kind[4096];
    int n = 0;
    for (int at = end; at >= 0 && n < 4096; at = nav->parent[at]) {
        raw[n] = nav_node_pos(nav, at);
        const int p = nav->parent[at];
        raw_kind[n] = 0;
        if (p >= 0) {
            for (int d = 0; d < 8; d++) {
                if (nav->nodes[p].link[d] == at) raw_kind[n] = nav->nodes[p].kind[d];
            }
        }
        n++;
        if (at == start) break;
    }
    // reverse
    for (int i = 0; i < n / 2; i++) {
        const vec3 t = raw[i];
        raw[i] = raw[n - 1 - i];
        raw[n - 1 - i] = t;
        const uint8_t k = raw_kind[i];
        raw_kind[i] = raw_kind[n - 1 - i];
        raw_kind[n - 1 - i] = k;
    }
    // smoothing: from each kept point, the farthest later point reachable in a straight
    // walkable line (never across a jump or a drop)
    int i = 0;
    while (i < n && out->count < BOT_PATH_MAX) {
        int j = i + 1;
        while (j + 1 < n && !raw_kind[j] && !raw_kind[j + 1] && j - i < 24 && nav_ground_line(nav, raw[i], raw[j + 1])) j++;
        if (j >= n) j = n - 1;
        if (j <= i) break;
        out->points[out->count] = raw[j];
        out->kinds[out->count] = raw_kind[j];
        out->count++;
        i = j;
    }
    out->reached = found;
    return out->count > 0 || start == end;
}

// A random walkable point of the connected area under `from` (Hunt's roam goals; an island
// map strands nobody), else of the largest one.
int bot_nav_random_point_from(Game *game, const vec3 *from, vec3 *out) {
    BotNav *nav = bot_nav_for(game);
    if (!nav || !nav->node_count) return 0;
    int component = nav->largest_component;
    if (from) {
        const int at = nav_nearest(nav, *from, 3);
        if (at >= 0) component = nav->nodes[at].component;
    }
    for (int tries = 0; tries < 48; tries++) {
        const int n = (int) (pcg32_boundedrand((uint32_t) nav->node_count));
        if (nav->nodes[n].component != component || nav->nodes[n].void_edge) continue;
        *out = nav_node_pos(nav, n);
        return 1;
    }
    return 0;
}

int bot_nav_random_point(Game *game, vec3 *out) { return bot_nav_random_point_from(game, NULL, out); }
