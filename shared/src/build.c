// map-logic WP-B: v7.1.4 player builds (data/buildObjs.js; game.buildObj / removeBuilds, the
// "bo" packet and syncData.dyno; doc/v7/map-logic.md §1.7). A build is a dynamic map object
// appended after the map's own objects: a destructible block (addBlock) or, for Stairs, a ramp
// (custMod -> addRamp). Its uid is its index in map->objects, the same on the server and every
// client (a joiner pads the uids of builds destroyed before it joined).
#include <world.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef KRUNKNATIVE_CLIENT
#include <client.h>
#endif

#define BUILD_MAX 1200 // config.buildMax

int game_object_shootable(const Object*); // shared/src/player.c (the hitscan object filter)

const WorldBuildObject g_build_objects[WORLD_BUILD_COUNT] = {
    // name, w, h, l, cost, health, places, rot_facing, rot_dir, ramp, colour
    {"Build Block", 10.0f, 10.0f, 10.0f, 10, 300, WORLD_BUILD_PLACES_ALL, 0, 0, 0, 0x5e432b},
    {"Platform", 30.0f, 2.0f, 30.0f, 50, 500, WORLD_BUILD_PLACES_ALL, 0, 0, 0, 0x5e432b},
    {"Wall", 30.0f, 30.0f, 2.0f, 50, 500, WORLD_BUILD_PLACES_OBJS, 1, 1, 0, 0x5e432b},
    {"Stairs", 30.0f, 30.0f, 30.0f, 25, 150, WORLD_BUILD_PLACES_OBJS, 0, 1, 1, 0x473220},
};

static size_t build_static_count(const Game *game) {
    const Map *map = game->map;
    if (!map) return 0;
    // the map's own objects come first; builds (and Object Manager objects) are appended after them
    size_t count = map->object_count;
    while (count > 0 && (map->objects[count - 1]->dynamic || map->objects[count - 1]->spawned)) count--;
    return count;
}

static int build_push(Map *map, Object *object) {
    Object **objects = realloc(map->objects, (map->object_count + 1) * sizeof(Object *));
    if (!objects) return 0;
    map->objects = objects;
    map->objects[map->object_count++] = object;
    return 1;
}

static void build_free(Object *object) {
#ifdef KRUNKNATIVE_CLIENT
    if (object->mesh) mesh_fini(object->mesh);
#endif
    free(object->ramp);
    free(object);
}

// direction (radians: 0, PI / 2, PI, -PI / 2 from rotDir) -> the ramp's 0..3 quarter turns
static int build_quarter(const float radians) {
    const int q = (int) lroundf(radians / ((float) M_PI / 2.0f));
    return ((q % 4) + 4) % 4;
}

// addBlock(x, y, z, w, l, h, null, {health, dynamic, buildInd}) / addRamp(x, y, z, w, h, l, dir)
static Object *build_make(const int build_index, const vec3 feet, const vec3 size, const float direction) {
    const WorldBuildObject *def = &g_build_objects[build_index];
    Object *object = calloc(1, sizeof(Object));
    if (!object) return NULL;
    object->prefab = def->ramp ? PREFAB_RAMP : PREFAB_CUBE;
    object->texture = 14; // src "grain_0" (GRAIN)
    object->position = feet;
    object->scale = size;
    object->collision_type = COLLISION_TYPE_BOX;
    object->active = 1;
    object->dynamic = 1;
    object->build_index = build_index;
    object->health = object->start_health = (float) def->health;
    object->can_destroy = 1; // health -> canDest
    object->destroyed_by = -1;
    object->objective_uid = -1;
    object->node_id = -1;
    object->render_side = -1;
    object->hitbox_h = object->hitbox_w = 1.0f;
    object->ambient_enabled = 1;
    object->fog_enabled = 1;
    if (def->ramp) {
        // addRamp: noShoot, ramp line along dir over (rot ? l : w) / 2 (MapManager.addRamp:1405-1493)
        Ramp *ramp = calloc(1, sizeof(Ramp));
        if (!ramp) {
            free(object);
            return NULL;
        }
        const int quarter = build_quarter(direction);
        const float half = (quarter % 2 ? size.z : size.x) * 0.5f;
        ramp->start = (vec2) {feet.x - half * cosf(direction), feet.z - half * sinf(direction)};
        ramp->end = (vec2) {feet.x + half * cosf(direction), feet.z + half * sinf(direction)};
        object->ramp = ramp;
        object->direction = (unsigned char) quarter;
        object->no_shoot = 1;
    } else {
        object->wall_jumpable = 1; // a solid block (notWallJ unset)
    }
    return object;
}

// A destroyed build that only keeps a uid taken (a joiner's gap, a prediction the server refused)
static void build_make_placeholder(Object *object) {
    object->active = 0;
    object->destroyed = 1;
    object->collision_type = COLLISION_TYPE_NONE;
    object->predicted_ms = 0.0f;
    object->position = (vec3) {0.0f, -100000.0f, 0.0f};
    object->scale = (vec3) {0.0f, 0.0f, 0.0f};
}

// Client: the local player's build, shown now at the uid the server's "bo" will take (world.h)
static void build_predict(Game *game, const int build_index, const vec3 feet, const vec3 size, const float direction) {
    Map *map = game->map;
    int pending = 0;
    for (size_t i = 0; i < map->object_count; i++) pending += map->objects[i]->predicted_ms > 0.0f;
    if (game->world.build_count + pending + 1 >= BUILD_MAX) return; // the server would answer "Build limit reached"
    Object *object = build_make(build_index, feet, size, direction);
    if (!object) return;
    object->predicted_ms = BUILD_PREDICT_MS;
    if (!build_push(map, object)) {
        build_free(object);
        return;
    }
    game->world.predicted_builds = 1;
}

// A prediction gives way: a predicted build becomes a placeholder, a predicted Object Manager object
// is freed for one (its mesh leaves the scene), so uid `i` stays taken
static void build_drop_prediction(Map *map, const size_t i) {
    Object *object = map->objects[i];
    if (!object->spawned) {
        build_make_placeholder(object);
        return;
    }
    Object *gap = build_make(0, (vec3) {0.0f, -100000.0f, 0.0f}, (vec3) {0.0f, 0.0f, 0.0f}, 0.0f);
    if (!gap) { // out of memory: an inert spawned slot
        object->active = 0;
        object->destroyed = 1;
        object->collision_type = COLLISION_TYPE_NONE;
        object->predicted_ms = 0.0f;
        return;
    }
    build_make_placeholder(gap);
    map_object_free(object);
    map->objects[i] = gap;
}

void world_cancel_prediction(Game *game, const int uid, const Object *object) {
    Map *map = game->map;
    if (!map || uid < 0 || (size_t) uid >= map->object_count || map->objects[uid] != object || object->predicted_ms <= 0.0f) return;
    build_drop_prediction(map, (size_t) uid);
}

void world_builds_tick(Game *game, const float delta) {
    Map *map = game->map;
    if (!game->world.predicted_builds || !map) return;
    int pending = 0;
    for (size_t i = 0; i < map->object_count; i++) { // (Object Manager stand-ins may sit between builds)
        Object *object = map->objects[i];
        if (object->predicted_ms <= 0.0f) continue;
        object->predicted_ms -= delta * 1000.0f;
        if (object->predicted_ms <= 0.0f) build_drop_prediction(map, i); // never confirmed: the server refused it
        else pending = 1;
    }
    game->world.predicted_builds = (unsigned char) pending;
}

int game_build_object(Game *game, const int build_index, const vec3 feet, vec3 size, const float direction, const int grounded, Player *player) {
    if (!game->map || build_index < 0 || build_index >= WORLD_BUILD_COUNT) return 0;
    const WorldBuildObject *def = &g_build_objects[build_index];
    game->world.build_count++;
    if (player && game->world.build_count >= BUILD_MAX) {
        if (game_is_authority(game)) {
            NetChatPacket packet = {.uid = -1, .kind = NET_CHAT_SERVER}; // "ch" -1, null, "Build limit reached", 2
            snprintf(packet.text, sizeof(packet.text), "Build limit reached");
            game_send(game, player->uid, NET_PACKET_CHAT, &packet, sizeof(packet));
        }
        return 0;
    }
    if (!size.x) size.x = def->w;
    if (!size.y) size.y = def->h;
    if (!size.z) size.z = def->l;
    vec3 at = feet;
    if (grounded) at.y -= size.y / 2.0f;
    Object *object = build_make(build_index, at, size, direction);
    if (!object || !build_push(game->map, object)) {
        if (object) build_free(object);
        return 0;
    }
    if (game_is_authority(game)) {
        const NetBuildPacket packet = {
            .uid = (int32_t) (game->map->object_count - 1),
            .build_index = (uint8_t) build_index,
            .direction = (int8_t) build_quarter(direction),
            .position = {at.x, at.y, at.z},
            .size = {size.x, size.y, size.z},
        };
        game_send(game, NET_TARGET_ALL, NET_PACKET_BUILD, &packet, sizeof(packet)); // "bo"
    }
    return 1;
}

// Client: Game.buildObj(..., uido). The object takes exactly the server's uid; uids of builds
// destroyed before this client joined (syncData.dyno lists the active ones) are padded with
// destroyed placeholders so map object indices stay aligned.
void world_apply_build_packet(Game *game, const NetBuildPacket *packet) {
    Map *map = game->map;
    if (!map || packet->build_index >= WORLD_BUILD_COUNT || packet->uid < 0 || packet->uid > 1000000) return;
    const size_t uid = (size_t) packet->uid;
    if (uid < build_static_count(game)) return; // never over a map object
    while (map->object_count < uid) {
        Object *gap = build_make(0, (vec3) {0.0f, -100000.0f, 0.0f}, (vec3) {0.0f, 0.0f, 0.0f}, 0.0f);
        if (!gap) return;
        build_make_placeholder(gap);
        if (!build_push(map, gap)) {
            build_free(gap);
            return;
        }
    }
    const vec3 feet = {packet->position[0], packet->position[1], packet->position[2]};
    const vec3 size = {packet->size[0], packet->size[1], packet->size[2]};
    Object *object = build_make(packet->build_index, feet, size, (float) packet->direction * (float) M_PI / 2.0f);
    if (!object) return;
    if (uid < map->object_count) { // a placeholder, our prediction (or a stale build) at that uid
        if (map->objects[uid]->spawned) map_object_free(map->objects[uid]); // a stale Object Manager object
        else build_free(map->objects[uid]);
        map->objects[uid] = object;
    } else if (!build_push(map, object)) {
        build_free(object);
        return;
    }
    game->world.build_count++;
    // KrunkNative prediction: this is a build we predicted at another uid (one of ours the server
    // refused took the uid it expected): that copy gives way now
    for (size_t i = 0; game->world.predicted_builds && i < map->object_count; i++) {
        Object *other = map->objects[i];
        if (i == uid || !other->dynamic || other->predicted_ms <= 0.0f || other->build_index != object->build_index) continue;
        if (fabsf(other->position.x - feet.x) < 0.01f && fabsf(other->position.y - feet.y) < 0.01f && fabsf(other->position.z - feet.z) < 0.01f) {
            build_make_placeholder(other);
            break;
        }
    }
}

// game.removeBuilds: every dynamic object goes, buildCount = 0. KrunkNative: the Object Manager's
// spawned objects go with them (object_manager_round_start drops their records afterwards).
void world_remove_builds(Game *game) {
    Map *map = game->map;
    game->world.build_count = 0;
    if (!map) return;
    size_t kept = 0;
    for (size_t i = 0; i < map->object_count; i++) {
        Object *object = map->objects[i];
        if (object->spawned) map_object_free(object);
        else if (object->dynamic) build_free(object);
        else map->objects[kept++] = object;
    }
    map->object_count = kept;
}

// KrunkNative Object Manager (client): put `object` at exactly the server's `uid`, after the map's
// own objects. Missing uids before it (objects removed before this client joined) get destroyed
// build placeholders, like world_apply_build_packet's; a placeholder already at `uid` is replaced.
// 0: refused (a map object's uid, a live object there) or out of memory; `object` is not freed.
int world_put_object(Game *game, const size_t uid, Object *object) {
    Map *map = game->map;
    if (!map || uid > 1000000 || uid < build_static_count(game)) return 0;
    while (map->object_count < uid) {
        Object *gap = build_make(0, (vec3) {0.0f, -100000.0f, 0.0f}, (vec3) {0.0f, 0.0f, 0.0f}, 0.0f);
        if (!gap) return 0;
        build_make_placeholder(gap);
        if (!build_push(map, gap)) {
            build_free(gap);
            return 0;
        }
    }
    if (uid < map->object_count) {
        Object *old = map->objects[uid];
        // only a placeholder gives way, or what we predicted there (the server gave the uid to this)
        if (old->predicted_ms <= 0.0f && (!old->dynamic || !old->destroyed)) return 0;
        if (old->spawned) map_object_free(old); // our predicted Object Manager object
        else build_free(old);
        map->objects[uid] = object;
        return 1;
    }
    return build_push(map, object);
}

// syncData.dyno: [typeInd, x, y, z, w, h, l, 0, uid] of every active build. KrunkNative sends the
// build's direction instead of v7's 0 (a v7 joiner rebuilt every Stairs facing direction 0).
void world_send_builds(Game *game, const int uid) {
    const Map *map = game->map;
    for (size_t i = 0; map && i < map->object_count; i++) {
        const Object *object = map->objects[i];
        if (!object->dynamic || !object->active) continue;
        const NetBuildPacket packet = {
            .uid = (int32_t) i,
            .build_index = (uint8_t) object->build_index,
            .direction = (int8_t) object->direction,
            .position = {object->position.x, object->position.y, object->position.z},
            .size = {object->scale.x, object->scale.y, object->scale.z},
        };
        game_send(game, uid, NET_PACKET_BUILD, &packet, sizeof(packet));
    }
}

// ---------------------------------------------------------------- placement (players.build)
// v7 boxes are centre + half extents (GameObject x, y, z, width, height, length); KrunkNative
// objects are bottom centre + full size.
typedef struct {
    double x, y, z, w, h, l;
} BuildBox;

static BuildBox build_box(const Object *o) {
    const vec3 s = object_box_size(o);
    return (BuildBox) {o->position.x, o->position.y + s.y * 0.5, o->position.z, s.x * 0.5, s.y * 0.5, s.z * 0.5};
}

static double js_min(const double a, const double b) { return a != a || b != b ? NAN : a < b ? a : b; }
static double js_max(const double a, const double b) { return a != a || b != b ? NAN : a > b ? a : b; }

// UTILS.lineInRect(x, z, y, 1/dx, 1/dz, 1/dy, minX, minZ, minY, maxX, maxZ, maxY): the entry
// fraction, or NAN for `false` (a miss). 0 / NaN are falsy for the callers, as in JS.
static double line_in_rect_js(const double x, const double z, const double y, const double ix, const double iz, const double iy,
                              const double min_x, const double min_z, const double min_y, const double max_x, const double max_z, const double max_y) {
    const double a = (min_x - x) * ix, b = (max_x - x) * ix, c = (min_y - y) * iy, d = (max_y - y) * iy;
    const double e = (min_z - z) * iz, f = (max_z - z) * iz;
    const double near = js_max(js_max(js_min(a, b), js_min(c, d)), js_min(e, f));
    const double far = js_min(js_min(js_max(a, b), js_max(c, d)), js_max(e, f));
    return !(far < 0) && !(near > far) ? near : NAN;
}

static int js_truthy(const double v) { return v == v && v != 0; }

typedef struct {
    double x, z, y, ix, iz, iy;
} BuildRay;

static double ray_box(const BuildRay *r, const double min_x, const double min_z, const double min_y, const double max_x, const double max_z, const double max_y) {
    return line_in_rect_js(r->x, r->z, r->y, r->ix, r->iz, r->iy, min_x, min_z, min_y, max_x, max_z, max_y);
}

// the smallest truthy value of a list (snapFunc: `if (v && (best == null || v < best))`), NAN none
static double build_min_truthy(const double *v, const int n) {
    double best = NAN;
    for (int i = 0; i < n; i++)
        if (js_truthy(v[i]) && (best != best || v[i] < best)) best = v[i];
    return best;
}

// BUILD_OBJS[target.buildInd].snapFunc(x, z, y, ix, iz, iy, buildIndex, target); has = 0: null
static double build_snap_func(const int target_build, const BuildRay *r, const int build_index, const BuildBox *o, int *has) {
    *has = 1;
    double v;
    switch (target_build) {
        case 0: // Build Block
            if (build_index != 0) break;
            v = ray_box(r, o->x - o->w, o->z - o->l, o->y - o->h, o->x + o->w, o->z + o->l, o->y + o->h);
            if (js_truthy(v)) return v;
            v = ray_box(r, o->x - o->w * 2, o->z - o->l * 0.98, o->y - o->h + 0.0001, o->x + o->w * 2, o->z + o->l * 0.98, o->y + o->h - 0.0001);
            if (js_truthy(v)) return v;
            v = ray_box(r, o->x - o->w * 0.98, o->z - o->l * 2, o->y - o->h + 0.0001, o->x + o->w * 0.98, o->z + o->l * 2, o->y + o->h - 0.0001);
            if (js_truthy(v)) return v;
            return ray_box(r, o->x - o->w * 0.98, o->z - o->l * 0.98, o->y - o->h * 1.5, o->x + o->w * 0.98, o->z + o->l * 0.98, o->y + o->h * 1.5);
        case 1: // Platform
            if (build_index == 1) {
                v = ray_box(r, o->x - o->w, o->z - o->l, o->y - o->h, o->x + o->w, o->z + o->l, o->y + o->h);
                if (js_truthy(v)) return v;
                v = ray_box(r, o->x - o->w * 2, o->z - o->l * 0.98, o->y - o->h + 0.0001, o->x + o->w * 2, o->z + o->l * 0.98, o->y + o->h - 0.0001);
                if (js_truthy(v)) return v;
                return ray_box(r, o->x - o->w * 0.98, o->z - o->l * 2, o->y - o->h + 0.0001, o->x + o->w * 0.98, o->z + o->l * 2, o->y + o->h - 0.0001);
            }
            if (build_index == 2) {
                const double list[5] = {
                    ray_box(r, o->x - o->w, o->z - o->l, o->y - o->h, o->x - o->w, o->z + o->l, o->y + 30),
                    ray_box(r, o->x + o->w, o->z - o->l, o->y - o->h, o->x + o->w, o->z + o->l, o->y + 30),
                    ray_box(r, o->x - o->w, o->z - o->l, o->y - o->h, o->x + o->w, o->z - o->l, o->y + 30),
                    ray_box(r, o->x - o->w, o->z + o->l, o->y - o->h, o->x + o->w, o->z + o->l, o->y + 30),
                    ray_box(r, o->x - o->w, o->z - o->l, o->y - o->h, o->x + o->w, o->z + o->l, o->y + o->h),
                };
                const double best = build_min_truthy(list, 5);
                if (best != best) *has = 0; // `return v13472` = null
                return best;
            }
            break;
        case 2: // Wall
            if (build_index == 2) {
                double list[4];
                if (o->w > o->l) {
                    list[0] = ray_box(r, o->x - o->w * 2, o->z - o->l * 0.98, o->y - o->h * 0.98, o->x + o->w * 2, o->z + o->l * 0.98, o->y + o->h * 0.98);
                    list[1] = ray_box(r, o->x + o->w, o->z - o->w, o->y - o->h * 0.98, o->x + o->w, o->z + o->w, o->y + o->h * 0.98);
                    list[2] = ray_box(r, o->x - o->w, o->z - o->w, o->y - o->h * 0.98, o->x - o->w, o->z + o->w, o->y + o->h * 0.98);
                } else {
                    list[0] = ray_box(r, o->x - o->w * 0.98, o->z - o->l * 2, o->y - o->h * 0.98, o->x + o->w * 0.98, o->z + o->l * 2, o->y + o->h * 0.98);
                    list[1] = ray_box(r, o->x - o->l, o->z - o->l, o->y - o->h * 0.98, o->x + o->l, o->z - o->l, o->y + o->h * 0.98);
                    list[2] = ray_box(r, o->x - o->l, o->z + o->l, o->y - o->h * 0.98, o->x + o->l, o->z + o->l, o->y + o->h * 0.98);
                }
                list[3] = ray_box(r, o->x - o->w, o->z - o->l, o->y - o->h, o->x + o->w, o->z + o->l, o->y + o->h);
                const double best = build_min_truthy(list, 4);
                if (best != best) *has = 0;
                return best;
            }
            break;
        default: // Stairs: no snapFunc
            break;
    }
    *has = 0;
    return NAN;
}

enum { FACE_NONE, FACE_LEFT, FACE_RIGHT, FACE_FRONT, FACE_BACK, FACE_TOP, FACE_BOTTOM };

// UTILS.findNearest(x, z, [x0, z0, x1, z1, ...]): the nearest point and its pair index * 2
static void find_nearest(const double x, const double z, const double *points, const int n, double *ox, double *oz, int *index) {
    double best = -1;
    *ox = x;
    *oz = z;
    *index = 0;
    for (int i = 0; i < n; i += 2) {
        const double d = hypot(x - points[i], z - points[i + 1]);
        if (best < 0 || d < best) {
            best = d;
            *ox = points[i];
            *oz = points[i + 1];
            *index = i;
        }
    }
}

static double round_to_nearest(const double v, const double step) {
    return v > 0 ? ceil(v / step) * step : v < 0 ? floor(v / step) * step : v;
}

// BUILD_OBJS[target.buildInd].buildSnap(x, y, z, canPlace, face, target, buildIndex) ->
// [x, y, z, canPlace, rot]; 0 = the target has no buildSnap (Stairs return it unchanged, false)
static int build_snap(const int target_build, double *x, double *y, double *z, int *can, int *rot, const int face, const BuildBox *o, const int bi) {
    const double ix = *x, iy = *y, iz = *z;
    *rot = 0;
    switch (target_build) {
        case 0: // Build Block
            if (bi == 0) {
                if (face == FACE_TOP) { *x = o->x; *y = o->y + o->h; *z = o->z; return 1; }
                if (face == FACE_BOTTOM) { *x = o->x; *y = o->y - o->h * 3; *z = o->z; return 1; }
                if (face == FACE_LEFT) { *x = o->x - o->w * 2; *y = o->y - o->h; *z = o->z; return 1; }
                if (face == FACE_RIGHT) { *x = o->x + o->w * 2; *y = o->y - o->h; *z = o->z; return 1; }
                if (face == FACE_FRONT) { *x = o->x; *y = o->y - o->h; *z = o->z - o->l * 2; return 1; }
                if (face == FACE_BACK) { *x = o->x; *y = o->y - o->h; *z = o->z + o->l * 2; return 1; }
            }
            if (bi == 1) {
                if (face == FACE_TOP) { *x = o->x; *y = o->y + o->h; *z = o->z; }
                else if (face == FACE_BOTTOM) { *x = o->x; *y = o->y - o->h; *z = o->z; }
                else { *x = o->x; *z = o->z; }
                return 1;
            }
            *can = 0;
            return 1;
        case 1: // Platform
            if (bi == 0) {
                if (face == FACE_TOP || face == FACE_BOTTOM) {
                    const double qx = round_to_nearest(fabs(o->x - o->w - ix) / (o->w * 2), 1.0 / 3.0);
                    const double qz = round_to_nearest(fabs(o->z - o->l - iz) / (o->l * 2), 1.0 / 3.0);
                    *x = o->x - o->w - 5 + o->w * 2 * qx;
                    *y = iy - (face == FACE_BOTTOM ? 10 : 0);
                    *z = o->z - o->l - 5 + o->l * 2 * qz;
                    return 1;
                }
                *can = 0;
                return 1;
            }
            if (bi == 1) {
                if (face == FACE_TOP) { *x = o->x; *y = o->y + o->h; *z = o->z; return 1; }
                if (face == FACE_BOTTOM) { *x = o->x; *y = o->y - o->h * 3; *z = o->z; return 1; }
                if (face == FACE_LEFT) { *x = o->x - o->w * 2; *y = o->y - o->h; *z = o->z; return 1; }
                if (face == FACE_RIGHT) { *x = o->x + o->w * 2; *y = o->y - o->h; *z = o->z; return 1; }
                if (face == FACE_FRONT) { *x = o->x; *y = o->y - o->h; *z = o->z - o->l * 2; return 1; }
                if (face == FACE_BACK) { *x = o->x; *y = o->y - o->h; *z = o->z + o->l * 2; return 1; }
                return 1; // undefined -> [x, y, z, canPlace] keeps the values (v7 falls through)
            }
            if (bi == 2) {
                const double pts[8] = {o->x - o->w, o->z, o->x + o->w, o->z, o->x, o->z - o->l, o->x, o->z + o->l};
                double nx, nz;
                int index;
                find_nearest(ix, iz, pts, 8, &nx, &nz, &index);
                *x = nx;
                *y = o->y;
                *z = nz;
                *rot = index == 0 || index == 2; // v13471[2] == 0 || == 2 (pair index * 2)
                return 1;
            }
            if (bi == 3) {
                if (iy >= o->y) { *x = o->x; *y = o->y + o->h; *z = o->z; return 1; }
                *can = 0;
                return 1;
            }
            return 1;
        case 2: // Wall
            if (bi == 2) {
                double px = ix, pz = iz;
                int r = 0;
                if (o->w > o->l) {
                    if (iz > o->z + o->l) { px = ix > o->x ? o->x + o->w : o->x - o->w; pz = o->z + o->w; r = 1; }
                    else if (iz < o->z - o->l) { px = ix > o->x ? o->x + o->w : o->x - o->w; pz = o->z - o->w; r = 1; }
                    else if (ix > o->x + o->w) { px = o->x + o->w * 2; pz = o->z; }
                    else if (ix < o->x - o->w) { px = o->x - o->w * 2; pz = o->z; }
                    else { *can = 0; return 1; }
                } else if (ix > o->x + o->w) { pz = iz > o->z ? o->z + o->l : o->z - o->l; px = o->x + o->l; }
                else if (ix < o->x - o->w) { pz = iz > o->z ? o->z + o->l : o->z - o->l; px = o->x - o->l; }
                else if (iz > o->z + o->l) { pz = o->z + o->l * 2; px = o->x; r = 1; }
                else if (iz < o->z - o->l) { pz = o->z - o->l * 2; px = o->x; r = 1; }
                else { *can = 0; return 1; }
                *x = px;
                *y = o->y - o->h;
                *z = pz;
                *rot = r;
                return 1;
            }
            if (bi == 3) {
                double px, pz;
                if (face == FACE_RIGHT && o->l > o->w) { px = o->x + 15; pz = o->z; }
                else if (face == FACE_LEFT && o->l > o->w) { px = o->x - 15; pz = o->z; }
                else if (face == FACE_FRONT && o->w > o->l) { px = o->x; pz = o->z - 15; }
                else if (face == FACE_BACK && o->w > o->l) { px = o->x; pz = o->z + 15; }
                else { *can = 0; return 1; }
                *x = px;
                *y = o->y + 1 - o->h;
                *z = pz;
                return 1;
            }
            *can = 0;
            return 1;
        case 3: // Stairs
            *can = 0;
            return 1;
        default:
            return 0;
    }
}

void player_select_build(Player *player, const float yaw_delta, const float pitch_delta) {
    // procInputs: holding aim with the Build Tool turns the look delta into a build pick
    player->select_build = 1;
    if (fabsf(yaw_delta) <= 0.008f && fabsf(pitch_delta) <= 0.008f) return;
    double dir = atan2(-pitch_delta, -yaw_delta); // UTILS.getDir(0, 0, a, b) = atan2(0 - b, 0 - a)
    dir += M_PI;
    const double slice = M_PI * 2 / WORLD_BUILD_COUNT;
    int index = (int) floor(dir / slice);
    if (index >= WORLD_BUILD_COUNT) index = 0;
    player->build_index = index;
}

int player_build(Player *player, const int place) {
    Game *game = player->game;
    const int authority = game_is_authority(game);
    if ((authority && !place) || player->select_build || !game->map || !player->weapon) return 0;
    const int bi = player->build_index >= 0 && player->build_index < WORLD_BUILD_COUNT ? player->build_index : 0;
    const WorldBuildObject *build = &g_build_objects[bi];
    const double yaw = player->direction.y, pitch = player->direction.x;
    const double range = player->weapon->range;
    const double eye = player->position.y + player->height - game_constants.camera_height;
    const double dx = range * sin(yaw + M_PI) * cos(pitch), dz = range * cos(yaw + M_PI) * cos(pitch), dy = range * sin(pitch);
    const BuildRay ray = {player->position.x, player->position.z, eye, 1 / dx, 1 / dz, 1 / dy};

    double best = NAN;
    const Object *target = NULL;
    // terrain first (vA714 pushes it before the objects; a tie keeps the terrain: stable sort)
    const float terrain_t = world_terrain_fraction(game, (vec3) {player->position.x, (float) eye, player->position.z},
                                                   (vec3) {(float) dx, (float) dy, (float) dz});
    if (terrain_t > 0.0f) best = terrain_t;
    for (size_t i = 0; i < game->map->object_count; i++) {
        const Object *o = game->map->objects[i];
        // manager.objects, active && !noShoot: KN's map list also holds what v7 keeps out of
        // manager.objects or flags noShoot (zones, bomb sites, no-collision props); those go the
        // way a shot (and players.spray) skips them, else a zone the player stands in takes the
        // ray at a negative fraction and the build lands behind the player. Builds always count.
        if (!o->active || o->no_shoot || (!o->dynamic && !game_object_shootable(o))) continue;
        const BuildBox box = build_box(o);
        int has = 0;
        double v = o->dynamic ? build_snap_func(o->build_index, &ray, bi, &box, &has) : NAN;
        if (!has) v = ray_box(&ray, box.x - box.w, box.z - box.l, box.y - box.h, box.x + box.w, box.z + box.l, box.y + box.h);
        if (js_truthy(v) && v <= 1 && (best != best || v < best)) {
            best = v;
            target = o;
        }
    }
    player->build_preview.active = 0;
    if (best != best) return 0; // else if (!serverIO): both helpers hidden

    // !config.isProd || score >= cost. KrunkNative deviation: player->build_free (OM_CAP_SPAWN) waives
    // the cost (and the score payment below) in the sandbox and for devs / a custom game's host.
    int can = player->build_free || player->score >= build->cost;
    const double dist = range * best - 0.0001;
    double hx = player->position.x + dist * sin(yaw + M_PI) * cos(pitch);
    double hy = eye + dist * sin(pitch);
    double hz = player->position.z + dist * cos(yaw + M_PI) * cos(pitch);
    int face = FACE_NONE, rot = 0;
    BuildBox tb = {0};
    if (target) {
        tb = build_box(target);
        if (hx <= tb.x - tb.w) face = FACE_LEFT;
        else if (hx >= tb.x + tb.w) face = FACE_RIGHT;
        else if (hz <= tb.z - tb.l) face = FACE_FRONT;
        else if (hz >= tb.z + tb.l) face = FACE_BACK;
        else if (hy >= tb.y + tb.h) face = FACE_TOP;
        else if (hy <= tb.y - tb.h) face = FACE_BOTTOM;
    }
    hx = round(hx);
    hy = round(hy);
    hz = round(hz);
    const int target_snaps = target && target->dynamic;
    if (target) {
        if (face && target_snaps) build_snap(target->build_index, &hx, &hy, &hz, &can, &rot, face, &tb, bi);
        else if (build->places == WORLD_BUILD_PLACES_FLOOR && face != FACE_TOP && face != FACE_BOTTOM) can = 0;
        else if (build->places == WORLD_BUILD_PLACES_OBJS) can = 0;
    }
    if (build->rot_facing && !target_snaps) {
        const double q = M_PI / 4;
        const double r = fabs(round_to_nearest(yaw, q)) / q;
        rot = r != 1 && r != 4 && r != 5 && r != 8;
    }
    double dir = 0;
    if (build->rot_dir) {
        if (face == FACE_LEFT) dir = 0;
        else if (face == FACE_FRONT) dir = M_PI / 2;
        else if (face == FACE_RIGHT) dir = M_PI;
        else if (face == FACE_BACK) dir = -M_PI / 2;
    }
    const double w = rot ? build->l : build->w, h = build->h, l = rot ? build->w : build->l;
    for (size_t i = 0; can && i < game->map->object_count; i++) {
        const Object *o = game->map->objects[i];
        if (!o->dynamic || !o->active) continue;
        const BuildBox ob = build_box(o);
        if (target_snaps) {
            if (o->build_index == bi && ob.x == hx && ob.z == hz && ob.y == hy + build->h / 2) can = 0;
        } else if (hx - w / 2 + 0.01 <= ob.x + ob.w && hx + w / 2 - 0.01 >= ob.x - ob.w && hz - l / 2 + 0.01 <= ob.z + ob.l &&
                   hz + l / 2 - 0.01 >= ob.z - ob.l && hy - h / 2 + 0.01 <= ob.y + ob.h && hy + h / 2 - 0.01 >= ob.y - ob.h) {
            can = 0; // manager.collisionC(x, y, z, w / 2, h / 2, l / 2, obj, -0.01)
        }
    }
    // the client's buildHelper (can) / buildHelperN (can't): w, h, l + 0.1, moved to (x, y - 0.05, z)
    player->build_preview.active = 1;
    player->build_preview.can = (unsigned char) !!can;
    player->build_preview.feet = (vec3) {(float) hx, (float) hy, (float) hz};
    player->build_preview.size = (vec3) {(float) w, (float) h, (float) l};
    if (place && can) {
        if (player->reloads && player->loadout_index >= 0) player->reloads[player->loadout_index] = player->weapon->rate;
        player->did_shoot = 1;
        player->did_act = 1;
    }
    if (!authority && place && can && player->is_you) {
        // KrunkNative: the local player's build shows at once, not a round trip and a server tick later
        build_predict(game, bi, (vec3) {(float) hx, (float) hy, (float) hz}, (vec3) {(float) w, (float) h, (float) l}, (float) dir);
        return 0;
    }
    if (!authority || !can) return 0;
    if (!game_build_object(game, bi, (vec3) {(float) hx, (float) hy, (float) hz}, (vec3) {(float) w, (float) h, (float) l}, (float) dir, 0, player)) return 0;
    if (!player->build_free) player_score(player, -build->cost, 0);
    return 1;
}
