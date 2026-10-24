// v10 KrunkScript entities, runtime blocks / assets, moved dyn objects and player locks on the client
// (client/include/entities.h). Line numbers are deob-10.0.3.js (the entity manager :212465-214818, the live
// objects :212096-212411).
//
// The entities themselves are the shared manager's replicated copy (shared/include/ks_entities.h): the records
// land there (applySpawn / applyBaselines with its replay / applyRemove / applyAttach / applyControlledSync),
// and every frame (entities_update) it steps the "sim" entities with the map's scripts.shared on the entity
// clock, runs the client script's entity hooks and keeps the render poses. This file keeps the entity clock
// (entityClock, :215740-215802) and the bodies.
//
// v10 keeps entity bodies in the map manager: addBlock per solid archetype part (dyn, noShader, noCol unless
// coll > 0, noPCol for coll 2, hidden when the part is a model "aid"), placed by placeBody on every pose
// change (rotated offsets, the AABB swapped by |sin| / |cos| yaw) while the meshes follow the render pose
// (updateVisuals). Ours are spawned map objects appended after the map's own: collision reads them like any
// object, and map_post adds their meshes to the scene. Their uids are this module's, checked against the map
// (a later "bo" build may take a uid: then the part is simply no longer ours).
//
// Deliberate differences (documented): model parts ("aid") are hidden collision boxes (no GLTF part models,
// so playPartClip has nothing to play); a body's pitch / roll turn its part meshes but not their offsets;
// soft zones carry nothing on a client but its own player (the shared manager); the entity clock is reset at
// an INIT and runs on the client's frame clock instead of Date.now(). Player poses ("pz",
// NET_EXT_PLAYER_POSE) are routed to player_pose_render.c, which reads every player's hard attachment from
// here (a faceRide pose faces the entity it rides).
#include <entities.h>
#include <ks_entities.h>
#include <net_ext.h>
#include <player_pose_render.h> // "pz"
#include <user_asset.h>         // a live asset's GLB joins the scene

#include <prefabs.h>
#include <cJSON.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ENT_MAX 256          // bodies kept (the server keeps 64 entities, :212685)
#define ENT_PARTS_MAX 40     // solid parts per body (config.archetypeObjectLimit)
#define LIVE_MAX 200         // liveObjects >= 200 (:212117)
#define CLOCK_SAMPLES 256    // entityClock: the samples of the last 2 s, 256 at most

typedef struct {
    int part;                // the shared manager's part index (ks_entities_part_pose)
    size_t uid;              // map->objects index
    const Object *object;    // what we put there (gone when the map's slot holds another)
    double size[3];
} EntPart;

typedef struct {
    int active;
    int eid;
    int coll;                // the archetype's coll: 0 none, 1 solid, 2 no player collision
    const MapArchetype *arch;
    EntPart parts[ENT_PARTS_MAX];
    int part_count;
    int placed;              // the collision boxes stand at `at` (placeBody)
    KsEntityPose at;
} Body;

typedef struct {
    int id;
    size_t uid;
    const Object *object;
} LiveBlock;

static struct {
    const Map *map;          // the map these objects were appended to
    Game *game;              // the client's game (the shared manager's copy lives on it)
    Body bodies[ENT_MAX];
    LiveBlock live[LIVE_MAX];
    int live_count;
    cJSON **records;         // the JSON records our objects were built from (Object.raw points into them)
    size_t record_count;
    // the NetExtJsonChunk being assembled
    uint16_t chunk_subtype;
    char *chunk;
    size_t chunk_len;
    int chunk_next, chunk_parts;
    // entityClock: (local time, server time - local time) samples, the smoothed offset
    double local_ms;
    double sample_t[CLOCK_SAMPLES], sample_off[CLOCK_SAMPLES];
    int sample_head, sample_count;
    double clock_offset;
    int clock_synced;
    unsigned spawns, removes, states, events, attaches, lives, moves;
} E;

// ---------------------------------------------------------------- map objects

static void forget_all(void) {
    memset(E.bodies, 0, sizeof(E.bodies));
    E.live_count = 0;
    for (size_t i = 0; i < E.record_count; i++) cJSON_Delete(E.records[i]);
    free(E.records);
    E.records = NULL;
    E.record_count = 0;
    free(E.chunk);
    E.chunk = NULL;
    E.chunk_len = 0;
    E.chunk_next = E.chunk_parts = 0;
    E.map = NULL;
}

void entities_reset(void) {
    forget_all();
    if (E.game) ks_entities_client_reset(E.game); // the shared copy (a map without scripts keeps no KsGame state otherwise)
    player_pose_reset(); // the old session's pending "pz" poses go with its players
    E.sample_head = E.sample_count = 0; // KN: a new INIT starts the entity clock over (v10 keeps its samples)
    E.clock_synced = 0;
    E.clock_offset = 0.0;
    E.spawns = E.removes = E.states = E.events = E.attaches = E.lives = E.moves = 0;
}

// The game's map; a different one than our objects were added to: they went with the old one
static Map *current_map(Client *client) {
    E.game = &client->game;
    Map *map = client->game.ready ? client->game.map : NULL;
    if (map != E.map) {
        forget_all();
        E.map = map;
    }
    return map;
}

// Our object still at its uid (the slot may have been replaced by a build or freed)
static Object *ours(Client *client, const size_t uid, const Object *object) {
    Map *map = client->game.map;
    return map && map == E.map && object && uid < map->object_count && map->objects[uid] == object ? map->objects[uid] : NULL;
}

// A texture index from a Krunker src name ("wall_0"), -1 none
static int texture_of(const char *src) {
    char buf[64];
    for (int i = 0; src && i < PREFAB_TEXTURE_COUNT; i++) {
        if (prefab_texture_src(i, 0, buf, sizeof(buf)) && !strcmp(buf, src)) return i;
    }
    return -1;
}

// A map record appended as a spawned map object; its uid, or (size_t) -1
static size_t append_object(Client *client, cJSON *record, const Object **out) {
    Map *map = current_map(client);
    *out = NULL;
    if (!map || !record) {
        cJSON_Delete(record);
        return (size_t) -1;
    }
    Object *object = map_object_from_json(map, record, 1);
    cJSON **records = object ? realloc(E.records, (E.record_count + 1) * sizeof(cJSON *)) : NULL;
    if (records) E.records = records;
    Object **objects = records ? realloc(map->objects, (map->object_count + 1) * sizeof(Object *)) : NULL;
    if (!objects) {
        if (object) map_object_free(object);
        cJSON_Delete(record);
        return (size_t) -1;
    }
    E.records[E.record_count++] = record;
    map->objects = objects;
    object->spawned = 1;  // map_post_frame adds its mesh to the scene
    object->destroyed_by = -1;
    map->objects[map->object_count++] = object;
    *out = object;
    return map->object_count - 1;
}

// A part / block leaves: inactive, no collision, its mesh hidden (the uid stays taken)
static void retire(Client *client, const size_t uid, const Object *object) {
    Object *o = ours(client, uid, object);
    if (!o) return;
    o->active = 0;
    o->destroyed = 1;
    o->collision_type = COLLISION_TYPE_NONE;
    if (o->mesh) ((Mesh *) o->mesh)->visible = 0;
}

// Our player stands on top of `o` (onTopOf: grounded, feet at its top, inside its footprint)
static int me_on_top(const Client *client, const Object *o) {
    const Player *me = client->me;
    if (!me || !me->active || !me->on_ground || !o || o->collision_type == COLLISION_TYPE_NONE) return 0;
    const vec3 s = object_box_size(o);
    const float r = game_constants.player_scale;
    return fabsf(me->position.y - (o->position.y + s.y)) < 0.6f && fabsf(me->position.x - o->position.x) <= s.x * 0.5f + r &&
           fabsf(me->position.z - o->position.z) <= s.z * 0.5f + r;
}

static void carry_me(Client *client, const vec3 d) {
    Player *me = client->me;
    if (!me || (d.x == 0.0f && d.y == 0.0f && d.z == 0.0f)) return;
    me->position.x += d.x;
    me->position.y += d.y;
    me->position.z += d.z;
}

// ---------------------------------------------------------------- the entity clock (entityClock, :215740-215802)

// addSample(serverT, localT): the offsets of the last 2 s (256 at most); the clock takes their largest (the
// least delayed packet), snaps to a jump over 250 ms and otherwise eases a tenth of the way
static void clock_sample(const double server_ms, const double local_ms) {
    if (!isfinite(server_ms) || !isfinite(local_ms)) return;
    const int slot = (E.sample_head + E.sample_count) % CLOCK_SAMPLES;
    if (E.sample_count == CLOCK_SAMPLES) E.sample_head = (E.sample_head + 1) % CLOCK_SAMPLES;
    else E.sample_count++;
    E.sample_t[slot] = local_ms;
    E.sample_off[slot] = server_ms - local_ms;
    while (E.sample_count > 1 && E.sample_t[E.sample_head] < local_ms - 2000.0) {
        E.sample_head = (E.sample_head + 1) % CLOCK_SAMPLES;
        E.sample_count--;
    }
    double best = E.sample_off[E.sample_head];
    for (int i = 1; i < E.sample_count; i++) best = fmax(best, E.sample_off[(E.sample_head + i) % CLOCK_SAMPLES]);
    if (!E.clock_synced) {
        E.clock_offset = best;
        E.clock_synced = 1;
    } else if (best - E.clock_offset > 250.0 || E.clock_offset - best > 250.0) {
        E.clock_offset = best;
    } else {
        E.clock_offset += (best - E.clock_offset) * 0.1;
    }
}

// ---------------------------------------------------------------- bodies

static Body *body_find(const int eid) {
    for (int i = 0; i < ENT_MAX; i++)
        if (E.bodies[i].active && E.bodies[i].eid == eid) return &E.bodies[i];
    return NULL;
}

static void body_retire(Client *client, Body *b) {
    for (int i = 0; i < b->part_count; i++) retire(client, b->parts[i].uid, b->parts[i].object);
    *b = (Body) {0};
}

// A part record counts as soft as the shared manager reads it (build_parts): soft truthy
static int part_soft(const cJSON *part) {
    const cJSON *soft = cJSON_GetObjectItemCaseSensitive(part, "soft");
    return soft && !cJSON_IsFalse(soft) && !cJSON_IsNull(soft) && !(cJSON_IsNumber(soft) && soft->valuedouble == 0) &&
           !(cJSON_IsString(soft) && !soft->valuestring[0]);
}

static void body_place(Client *client, Body *b, const KsEntityView *v);

// buildBody (:212839): one cube per solid part (soft parts are zones only), as the manager lists them
static void body_build(Client *client, const int eid) {
    KsEntityView v;
    if (!ks_entities_find(&client->game, eid, &v) || !current_map(client)) return;
    Body *b = NULL;
    for (int i = 0; i < ENT_MAX && !b; i++)
        if (!E.bodies[i].active) b = &E.bodies[i];
    if (!b) return;
    *b = (Body) {.active = 1, .eid = eid, .coll = v.arch->coll, .arch = v.arch};
    int index = 0;
    const cJSON *part;
    cJSON_ArrayForEach(part, v.arch->objects) {
        if (part_soft(part)) continue;
        const int k = index++;
        if (k >= v.part_count || b->part_count >= ENT_PARTS_MAX) break;
        KsEntityPose pose;
        double size[3];
        int is_model = 0;
        if (!ks_entities_part_pose(&client->game, eid, k, &pose, size, &is_model)) continue;
        for (int a = 0; a < 3; a++)
            if (!(size[a] > 0.0)) size[a] = 0.01; // a degenerate box still places
        cJSON *record = cJSON_CreateObject();
        cJSON_AddItemToObject(record, "p", cJSON_CreateDoubleArray((const double[]) {v.pose.x + pose.x, v.pose.y + pose.y, v.pose.z + pose.z}, 3));
        cJSON_AddItemToObject(record, "s", cJSON_CreateDoubleArray(size, 3));
        cJSON_AddNumberToObject(record, "dyn", 1);
        const cJSON *c = cJSON_GetObjectItem(part, "c"), *op = cJSON_GetObjectItem(part, "o");
        if (cJSON_IsString(c)) cJSON_AddStringToObject(record, "c", c->valuestring);
        if (cJSON_IsNumber(op)) cJSON_AddNumberToObject(record, "o", op->valuedouble);
        if (b->coll <= 0) cJSON_AddNumberToObject(record, "col", 1); // noCol
        if (is_model) cJSON_AddNumberToObject(record, "v", 1);        // noVis: a model part
        const Object *object = NULL;
        const size_t uid = append_object(client, record, &object);
        if (!object) continue;
        if (b->coll == 2) ((Object *) object)->no_player_col = 1; // noPCol
        b->parts[b->part_count++] = (EntPart) {k, uid, object, {size[0], size[1], size[2]}};
    }
    body_place(client, b, &v);
}

// placeBody (:213050) for the collision boxes at the committed pose, when it changed: our player standing on a
// solid part that moved goes with it (carryPlayers, not when it rides this entity); the meshes at the render
// pose (updateVisuals' meshRoot and pivots)
static void body_place(Client *client, Body *b, const KsEntityView *v) {
    const KsEntityPose *p = &v->pose, *r = &v->render;
    const int moved = !b->placed || p->x != b->at.x || p->y != b->at.y || p->z != b->at.z || p->yaw != b->at.yaw || p->pitch != b->at.pitch ||
                      p->roll != b->at.roll;
    int riding = 0, soft = 0;
    if (client->me && ks_entities_attachment(&client->game, (int) client->me->uid, &riding, &soft, NULL, NULL, NULL)) riding = riding == b->eid;
    const vec3 d = b->placed ? (vec3) {(float) (p->x - b->at.x), (float) (p->y - b->at.y), (float) (p->z - b->at.z)} : (vec3) {0.0f, 0.0f, 0.0f};
    int carry = 0;
    const float sy = (float) sin(p->yaw), cy = (float) cos(p->yaw), as = fabsf(sy), ac = fabsf(cy);
    const float rsy = (float) sin(r->yaw), rcy = (float) cos(r->yaw);
    for (int i = 0; i < b->part_count; i++) {
        EntPart *ep = &b->parts[i];
        Object *o = ours(client, ep->uid, ep->object);
        KsEntityPose pp;
        if (!o || !ks_entities_part_pose(&client->game, b->eid, ep->part, &pp, NULL, NULL)) continue;
        if (moved) {
            if (b->placed && b->coll == 1 && !riding && (d.x != 0.0f || d.y != 0.0f || d.z != 0.0f)) carry |= me_on_top(client, o);
            const float dx = (float) pp.x * cy + (float) pp.z * sy, dz = -(float) pp.x * sy + (float) pp.z * cy;
            o->position = (vec3) {(float) p->x + dx, (float) (p->y + pp.y), (float) p->z + dz};
            o->scale = (vec3) {(float) ep->size[0] * ac + (float) ep->size[2] * as, (float) ep->size[1], (float) ep->size[0] * as + (float) ep->size[2] * ac};
        }
        Mesh *mesh = o->mesh;
        if (mesh) {
            const float rx = (float) pp.x * rcy + (float) pp.z * rsy, rz = -(float) pp.x * rsy + (float) pp.z * rcy;
            mesh->transform.position = (vec3) {(float) r->x + rx, (float) (r->y + pp.y), (float) r->z + rz};
            mesh->transform.rotation = (vec3) {(float) (r->pitch + pp.pitch), (float) (r->yaw + pp.yaw), (float) (r->roll + pp.roll)};
            mesh->transform.scale = (vec3) {(float) ep->size[0], (float) ep->size[1], (float) ep->size[2]};
        }
    }
    if (carry) carry_me(client, d);
    b->at = *p;
    b->placed = 1;
}

int entities_player_ride_yaw(const int uid, float *yaw) {
    int eid = -1, soft = 0;
    KsEntityView v;
    if (!E.game || !ks_entities_attachment(E.game, uid, &eid, &soft, NULL, NULL, NULL) || soft || !ks_entities_find(E.game, eid, &v)) return 0;
    if (yaw) *yaw = (float) v.render.yaw;
    return 1;
}

// ---------------------------------------------------------------- live blocks / assets / moved objects

// syncNewObjects (:212277): [id, {type: "block", x, y (bottom), z, w, l, h, s, data}] -> manager.addBlock with
// data's props (src texture, colr, opacity, noCol); [id, {type: "asset", aid, x, y, z, s, ry, data}] ->
// loadAnimMesh of <aid digits>/model.glb at (x, y, z), turned ry, scaled s. KN draws the asset with no collision
// (its cpx triangles are not loaded on either side, ks_entities.c).
static void live_add(Client *client, const cJSON *rec) {
    E.lives++;
    const cJSON *data = cJSON_GetArrayItem(rec, 1);
    const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(data, "type"));
    if (!cJSON_IsArray(rec) || !type || E.live_count >= LIVE_MAX) return;
    const int id = (int) cJSON_GetNumberValue(cJSON_GetArrayItem(rec, 0));
    for (int i = 0; i < E.live_count; i++) {
        if (E.live[i].id == id) return; // already there (a joiner's init and a broadcast)
    }
    const cJSON *jx = cJSON_GetObjectItem(data, "x"), *jy = cJSON_GetObjectItem(data, "y"), *jz = cJSON_GetObjectItem(data, "z");
    const float x = cJSON_IsNumber(jx) ? (float) jx->valuedouble : 0.0f, y = cJSON_IsNumber(jy) ? (float) jy->valuedouble : 0.0f;
    const float z = cJSON_IsNumber(jz) ? (float) jz->valuedouble : 0.0f;
    const cJSON *props = cJSON_GetObjectItem(data, "data");
    cJSON *record = cJSON_CreateObject();
    if (!strcmp(type, "asset")) {
        char aid[48];
        const cJSON *ja = cJSON_GetObjectItem(data, "aid");
        if (cJSON_IsString(ja)) snprintf(aid, sizeof(aid), "%s", ja->valuestring);
        else snprintf(aid, sizeof(aid), "%.0f", cJSON_IsNumber(ja) ? ja->valuedouble : 0.0);
        size_t n = 0;
        for (size_t i = 0; aid[i]; i++)
            if (aid[i] >= '0' && aid[i] <= '9') aid[n++] = aid[i]; // (aid + "").replace(/[^\d]/g, "")
        aid[n] = 0;
        if (!n) {
            cJSON_Delete(record);
            return;
        }
        strcat(aid, "q"); // fileExt "glb"
        const cJSON *js = cJSON_GetObjectItem(data, "s"), *jr = cJSON_GetObjectItem(data, "ry");
        const double s = cJSON_IsNumber(js) && js->valuedouble ? js->valuedouble : 1.0, ry = cJSON_IsNumber(jr) ? jr->valuedouble : 0.0;
        cJSON_AddNumberToObject(record, "i", PREFAB_CUSTOM_ASSET);
        cJSON_AddStringToObject(record, "aid", aid);
        cJSON_AddItemToObject(record, "p", cJSON_CreateFloatArray((const float[]) {x, y, z}, 3));
        cJSON_AddItemToObject(record, "s", cJSON_CreateFloatArray((const float[]) {1, 1, 1}, 3));
        cJSON_AddItemToObject(record, "r", cJSON_CreateDoubleArray((const double[]) {0, ry, 0}, 3));
        cJSON_AddNumberToObject(record, "ms", s);
        cJSON_AddNumberToObject(record, "col", 1);
    } else if (!strcmp(type, "block")) {
        const cJSON *jw = cJSON_GetObjectItem(data, "w"), *jl = cJSON_GetObjectItem(data, "l"), *jh = cJSON_GetObjectItem(data, "h");
        const float w = cJSON_IsNumber(jw) ? (float) jw->valuedouble : 1.0f, l = cJSON_IsNumber(jl) ? (float) jl->valuedouble : 1.0f;
        const float h = cJSON_IsNumber(jh) ? (float) jh->valuedouble : 1.0f;
        cJSON_AddItemToObject(record, "p", cJSON_CreateFloatArray((const float[]) {x, y, z}, 3));
        cJSON_AddItemToObject(record, "s", cJSON_CreateFloatArray((const float[]) {w, h, l}, 3));
        const char *colr = cJSON_GetStringValue(cJSON_GetObjectItem(props, "colr"));
        if (!colr) colr = cJSON_GetStringValue(cJSON_GetObjectItem(props, "color"));
        if (colr) cJSON_AddStringToObject(record, "c", colr);
        const int tex = texture_of(cJSON_GetStringValue(cJSON_GetObjectItem(props, "src")));
        if (tex >= 0) cJSON_AddNumberToObject(record, "t", tex);
        const cJSON *opacity = cJSON_GetObjectItem(props, "opacity");
        if (cJSON_IsNumber(opacity)) cJSON_AddNumberToObject(record, "o", opacity->valuedouble);
        const cJSON *no_col = cJSON_GetObjectItem(props, "noCol");
        if (no_col && !cJSON_IsFalse(no_col) && !cJSON_IsNull(no_col)) cJSON_AddNumberToObject(record, "col", 1);
    } else {
        cJSON_Delete(record);
        return;
    }
    const Object *object = NULL;
    const size_t uid = append_object(client, record, &object);
    if (!object) return;
    if (object->mesh) user_asset_object_added(object->mesh); // a GLB already loaded joins the scene now
    E.live[E.live_count++] = (LiveBlock) {id, uid, object};
}

// syncLiveObjects (:212320): [id, "remove"]
static void live_sync(Client *client, const cJSON *rec) {
    const char *op = cJSON_GetStringValue(cJSON_GetArrayItem(rec, 1));
    if (!cJSON_IsArray(rec) || !op || strcmp(op, "remove")) return;
    const int id = (int) cJSON_GetNumberValue(cJSON_GetArrayItem(rec, 0));
    for (int i = 0; i < E.live_count; i++) {
        if (E.live[i].id != id) continue;
        retire(client, E.live[i].uid, E.live[i].object);
        E.live[i] = E.live[--E.live_count];
        return;
    }
}

// game.moveObject(uid, x, y, z): only a dyn object; y is its centre; a rider on top moves with it
static void object_moved(Client *client, const NetObjectMovedPacket *m) {
    Map *map = client->game.ready ? client->game.map : NULL;
    E.moves++;
    if (!map || m->uid < 0 || (size_t) m->uid >= map->object_count) return;
    Object *o = map->objects[m->uid];
    if (!o || !o->dyn) return;
    const vec3 s = object_box_size(o);
    const vec3 to = {m->x, m->y - s.y * 0.5f, m->z};
    const vec3 d = {to.x - o->position.x, to.y - o->position.y, to.z - o->position.z};
    const int rider = me_on_top(client, o);
    o->position = to;
    if (o->mesh) ((Mesh *) o->mesh)->transform.position = to;
    if (rider) carry_me(client, d);
}

// ---------------------------------------------------------------- dispatch

// NetExtJsonChunk assembly: the whole record when its last slice arrived (NULL meanwhile)
static cJSON *json_record(const uint16_t subtype, const uint8_t *body, const uint16_t length) {
    if (length < sizeof(NetExtJsonChunk)) return NULL;
    NetExtJsonChunk head;
    memcpy(&head, body, sizeof(head));
    const uint8_t *data = body + sizeof(head);
    const size_t n = length - sizeof(head);
    if (!head.parts || head.part >= head.parts) return NULL;
    if (head.parts == 1) return cJSON_ParseWithLength((const char *) data, n);
    if (head.part == 0 || E.chunk_subtype != subtype || head.part != E.chunk_next) { // a new record (or a broken one: restart)
        free(E.chunk);
        E.chunk = NULL;
        E.chunk_len = 0;
        if (head.part != 0) return NULL;
        E.chunk_subtype = subtype;
        E.chunk_parts = head.parts;
    }
    char *grown = realloc(E.chunk, E.chunk_len + n);
    if (!grown) return NULL;
    E.chunk = grown;
    memcpy(E.chunk + E.chunk_len, data, n);
    E.chunk_len += n;
    E.chunk_next = head.part + 1;
    if (E.chunk_next < E.chunk_parts) return NULL;
    cJSON *json = cJSON_ParseWithLength(E.chunk, E.chunk_len);
    free(E.chunk);
    E.chunk = NULL;
    E.chunk_len = 0;
    E.chunk_next = 0;
    return json;
}

int entities_on_ext(Client *client, const uint16_t subtype, const uint8_t *body, const uint16_t length) {
    switch (subtype) {
        case NET_EXT_PLAYER_POSE: // "pz" players.setPose (player_pose_render.c)
            player_pose_on_ext(client, body, length);
            return 1;
        case NET_EXT_PLAYER_LOCKS: {
            NetPlayerLocksPacket p;
            if (length < sizeof(p)) return 1;
            memcpy(&p, body, sizeof(p));
            if (client->me) {
                client->me->lock_shooting = p.disable_shooting ? 1 : 0;
                client->me->lock_melee = p.disable_melee ? 1 : 0;
            }
            return 1;
        }
        case NET_EXT_KN_OFFICIAL_OBJECT_MOVED: {
            NetObjectMovedPacket m;
            if (length < sizeof(m)) return 1;
            memcpy(&m, body, sizeof(m));
            object_moved(client, &m);
            return 1;
        }
        case NET_EXT_ENTITY_SPAWN:
        case NET_EXT_ENTITY_REMOVE:
        case NET_EXT_ENTITY_STATE:
        case NET_EXT_ENTITY_CONTROLLED:
        case NET_EXT_ENTITY_EVENT:
        case NET_EXT_ENTITY_ATTACH:
        case NET_EXT_ENTITY_DETACH:
        case NET_EXT_ENTITY_SEED:
        case NET_EXT_KN_OFFICIAL_LIVE_OBJECT:
        case NET_EXT_KN_OFFICIAL_LIVE_SYNC:
            break;
        default:
            return 0;
    }
    cJSON *rec = json_record(subtype, body, length);
    if (!rec) return 1;
    if (client->game.ready && !game_is_authority(&client->game)) {
        current_map(client);
        Game *game = &client->game;
        switch (subtype) {
            case NET_EXT_ENTITY_SPAWN: { // applySpawn: a known eid is despawned first, its body rebuilt
                E.spawns++;
                const int eid = cJSON_IsArray(rec) ? (int) cJSON_GetNumberValue(cJSON_GetArrayItem(rec, 0)) : -1;
                Body *old = body_find(eid);
                if (old) body_retire(client, old);
                ks_entities_net_spawn(game, rec);
                body_build(client, eid);
                break;
            }
            case NET_EXT_ENTITY_REMOVE: {
                E.removes++;
                Body *b = cJSON_IsArray(rec) ? body_find((int) cJSON_GetNumberValue(cJSON_GetArrayItem(rec, 0))) : NULL;
                if (b) body_retire(client, b);
                ks_entities_net_remove(game, rec);
                break;
            }
            case NET_EXT_ENTITY_STATE: // a baseline: the entity clock's sample is its tick (:261724-261731)
                E.states++;
                if (cJSON_IsArray(rec) && cJSON_IsNumber(cJSON_GetArrayItem(rec, 1)))
                    clock_sample(cJSON_GetArrayItem(rec, 1)->valuedouble * 33.333333333333336, E.local_ms);
                ks_entities_net_baseline(game, rec, E.local_ms);
                break;
            case NET_EXT_ENTITY_CONTROLLED: ks_entities_net_controlled(game, rec); break; // our "l"'s tail
            case NET_EXT_ENTITY_EVENT: E.events++; ks_entities_net_event(game, rec); break;
            case NET_EXT_ENTITY_ATTACH:
                E.attaches++;
                ks_entities_net_attach(game, rec, client->me ? (int) client->me->uid : -1);
                break;
            case NET_EXT_ENTITY_DETACH: E.attaches++; ks_entities_net_detach(game, rec); break;
            case NET_EXT_ENTITY_SEED: ks_entities_net_seed(game, rec); break;
            case NET_EXT_KN_OFFICIAL_LIVE_OBJECT: live_add(client, rec); break;
            case NET_EXT_KN_OFFICIAL_LIVE_SYNC: live_sync(client, rec); break;
            default: break;
        }
    }
    cJSON_Delete(rec);
    return 1;
}

void entities_update(Client *client, const float delta) {
    E.local_ms += (double) delta * 1000.0;
    if (!client->game.ready || game_is_authority(&client->game)) return;
    current_map(client);
    // game.update's client half (:222187-222216) once the entity clock has a sample
    if (E.clock_synced) ks_entities_client_frame(&client->game, E.local_ms + E.clock_offset, E.local_ms, (double) delta * 1000.0);
}

void entities_frame(Client *client, const float delta) {
    (void) delta;
    if (!E.map || current_map(client) != E.map) return;
    for (int i = 0; i < ENT_MAX; i++) {
        Body *b = &E.bodies[i];
        if (!b->active) continue;
        KsEntityView v;
        if (!ks_entities_find(&client->game, b->eid, &v)) { // gone from the manager (a reset, a despawn)
            body_retire(client, b);
            continue;
        }
        body_place(client, b, &v);
    }
}

void entities_describe(const Client *client, char *out, const size_t size) {
    int parts = 0, solid = 0, attached = -1, soft = 0;
    for (int i = 0; i < ENT_MAX; i++) {
        if (!E.bodies[i].active) continue;
        parts += E.bodies[i].part_count;
        solid += E.bodies[i].coll > 0 ? E.bodies[i].part_count : 0;
    }
    if (client->me && ks_entities_attachment(&client->game, (int) client->me->uid, &attached, &soft, NULL, NULL, NULL) && soft) attached = -1;
    KsEntityView first;
    const int count = ks_entities_count(&client->game);
    const int has = count > 0 && ks_entities_view(&client->game, 0, &first);
    snprintf(out, size, "entities=%d parts=%d solid=%d live=%d spawns=%u states=%u removes=%u events=%u attaches=%u lives=%u moves=%u "
                        "attached=%d first=%d@%.2f,%.2f,%.2f/%.2f",
             count, parts, solid, E.live_count, E.spawns, E.states, E.removes, E.events, E.attaches, E.lives, E.moves, attached,
             has ? first.eid : -1, has ? first.pose.x : 0.0, has ? first.pose.y : 0.0, has ? first.pose.z : 0.0, has ? first.pose.yaw : 0.0);
}
