// v10 KrunkScript entities, runtime blocks, moved dyn objects and player locks on the client
// (client/include/entities.h). Line numbers: ~/.cache/kn-official/game.deob.js (the entities class
// :184112-186300 is the same in v10.0.2).
//
// v10 keeps entity bodies in the map manager: addBlock per solid archetype part (dyn, noShader,
// noCol unless coll > 0, noPCol for coll 2, hidden when the part is a model "aid"), re-placed by
// placeBody on every pose change (rotated offsets, the AABB swapped by |sin| / |cos| yaw). Ours are
// spawned map objects appended after the map's own: collision reads them like any object, and
// map_post adds their meshes to the scene. Their uids are this module's, checked against the map
// (a later "bo" build may take a uid: then the part is simply no longer ours).
//
// Not done (documented): the shared / client KrunkScript hooks (onEntityInit, onEntityEvent,
// client-side "sim" stepping between baselines: our client runs no shared script, so an entity
// snaps to each baseline with v10's 100 ms blend), model parts ("aid": hidden collision boxes, no
// GLTF), pitch / roll of bodies, the local stepping of an entity we control (ctl attachments, controlledByLocal:
// its pose is the server's, from our "l" tail, NET_EXT_ENTITY_CONTROLLED), soft zones. Player poses ("pz", NET_EXT_PLAYER_POSE) are routed to player_pose_render.c, which reads
// every player's hard attachment from here (a faceRide pose faces the entity it rides).
#include <entities.h>
#include <net_ext.h>
#include <player_pose_render.h> // "pz"

#include <prefabs.h>
#include <cJSON.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ENT_MAX 64           // entities.list().length >= 0x40: no more spawns (:184310)
#define ENT_PARTS_MAX 32     // solid parts kept per entity
#define LIVE_MAX 200         // liveObjects >= 0xc8 (:183953)
#define ENT_BLEND_MS 100.0f  // applyBaselines: blend = {-dx, -dy, -dz, left: 100}

typedef struct {
    size_t uid;              // map->objects index
    const Object *object;    // what we put there (gone when the map's slot holds another)
    vec3 off;                // part offset (p) from the entity origin; y is the part's bottom
    vec3 size;               // s [w, h, l]
    char name[16];           // skeleton name "n" (state keys <n>_x .. <n>_roll), "" none
} EntPart;

typedef struct {
    int active;
    int eid;
    int arch;
    int coll;                // the archetype's coll: 0 none, 1 solid, 2 no player collision
    char tag[33];
    cJSON *state;            // decoded state (wire keys as sent: "x", "y", "z", "yaw", ...)
    vec3 pos;
    float yaw;
    vec3 blend;              // visual offset (v10 blend), decays over blend_left ms
    float blend_left;
    EntPart parts[ENT_PARTS_MAX];
    int part_count;
} Entity;

typedef struct {
    int id;
    size_t uid;
    const Object *object;
} LiveBlock;

static struct {
    const Map *map;          // the map these objects were appended to
    Entity ents[ENT_MAX];
    LiveBlock live[LIVE_MAX];
    int live_count;
    int my_attached_eid;     // ea+ for our sid, -1 none (soft attachments do not carry)
    // every player's hard attachment: v10 player.attachedEnt (applyAttach deob-10.0.3.js:213569-213574), for a
    // pose's faceRide (player_pose_render.c)
    struct { int sid, eid; } riders[ENT_MAX];
    int rider_count;
    cJSON **records;         // the JSON records our objects were built from (Object.raw points into them)
    size_t record_count;
    // the NetExtJsonChunk being assembled
    uint16_t chunk_subtype;
    char *chunk;
    size_t chunk_len;
    int chunk_next, chunk_parts;
    unsigned spawns, removes, states, events, attaches, lives, moves;
} E = {.my_attached_eid = -1};

// ---------------------------------------------------------------- map objects

static void forget_all(void) {
    for (int i = 0; i < ENT_MAX; i++) {
        cJSON_Delete(E.ents[i].state);
        E.ents[i] = (Entity) {0};
    }
    E.live_count = 0;
    E.my_attached_eid = -1;
    E.rider_count = 0;
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
    player_pose_reset(); // the old session's pending "pz" poses go with its players
    E.spawns = E.removes = E.states = E.events = E.attaches = E.lives = E.moves = 0;
}

// The game's map; a different one than our objects were added to: they went with the old one
static Map *current_map(Client *client) {
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

// A CUBE record (bottom centre p, size s) appended as a spawned map object; its uid, or (size_t) -1
static size_t append_cube(Client *client, cJSON *record, const Object **out) {
    Map *map = current_map(client);
    *out = NULL;
    if (!map || !record) {
        cJSON_Delete(record);
        return (size_t) -1;
    }
    Object *object = map_object_from_json(map, record, 1);
    cJSON **records = object ? realloc(E.records, (E.record_count + 1) * sizeof(cJSON *)) : NULL;
    Object **objects = records ? realloc(map->objects, (map->object_count + 1) * sizeof(Object *)) : NULL;
    if (records) E.records = records;
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

// ---------------------------------------------------------------- entities

static double state_num(const cJSON *state, const char *key, const double def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(state, key);
    return cJSON_IsNumber(v) ? v->valuedouble : def;
}

static Entity *entity_find(const int eid) {
    for (int i = 0; i < ENT_MAX; i++) {
        if (E.ents[i].active && E.ents[i].eid == eid) return &E.ents[i];
    }
    return NULL;
}

// commitPose: x, y, z, yaw from the state (pitch / roll not shown)
static void commit_pose(Entity *e) {
    e->pos = (vec3) {(float) state_num(e->state, "x", e->pos.x), (float) state_num(e->state, "y", e->pos.y),
                     (float) state_num(e->state, "z", e->pos.z)};
    e->yaw = (float) state_num(e->state, "yaw", e->yaw);
}

// partPose: the part's offset plus its skeleton keys (<name>_x / _y / _z added, _yaw added)
static vec3 part_offset(const Entity *e, const EntPart *p, float *yaw) {
    vec3 o = p->off;
    *yaw = 0.0f;
    if (!p->name[0]) return o;
    char key[40];
    static const char *const axes[4] = {"x", "y", "z", "yaw"};
    float *slot[4] = {&o.x, &o.y, &o.z, yaw};
    for (int k = 0; k < 4; k++) {
        snprintf(key, sizeof(key), "%s_%s", p->name, axes[k]);
        *slot[k] += (float) state_num(e->state, key, 0.0);
    }
    return o;
}

// placeBody: every part at the entity's pose; collision snaps, the mesh shows pose + blend.
// Our player riding a solid part (or hard-attached) moves by the pose change.
static void place_body(Client *client, Entity *e, const vec3 moved) {
    const float sy = sinf(e->yaw), cy = cosf(e->yaw), as = fabsf(sy), ac = fabsf(cy);
    const float k = e->blend_left > 0.0f ? e->blend_left / ENT_BLEND_MS : 0.0f;
    int carry = e->eid == E.my_attached_eid;
    for (int i = 0; i < e->part_count; i++) {
        EntPart *p = &e->parts[i];
        Object *o = ours(client, p->uid, p->object);
        if (!o) continue;
        if (moved.x != 0.0f || moved.y != 0.0f || moved.z != 0.0f) carry |= e->coll == 1 && me_on_top(client, o);
        float part_yaw = 0.0f;
        const vec3 off = part_offset(e, p, &part_yaw);
        const float dx = off.x * cy + off.z * sy, dz = -off.x * sy + off.z * cy;
        o->position = (vec3) {e->pos.x + dx, e->pos.y + off.y, e->pos.z + dz};
        o->scale = (vec3) {p->size.x * ac + p->size.z * as, p->size.y, p->size.x * as + p->size.z * ac};
        Mesh *mesh = o->mesh;
        if (mesh) {
            mesh->transform.position = (vec3) {o->position.x + e->blend.x * k, o->position.y + e->blend.y * k, o->position.z + e->blend.z * k};
            mesh->transform.rotation = (vec3) {0.0f, e->yaw + part_yaw, 0.0f};
            mesh->transform.scale = p->size;
        }
    }
    if (carry) carry_me(client, moved);
}

// A JSON object of the body: its parts (archetype objects), coll, the spawn's state
static void entity_spawn(Client *client, const cJSON *rec) {
    const int n = cJSON_GetArraySize(rec);
    if (!cJSON_IsArray(rec) || n < 2) return;
    const int eid = (int) cJSON_GetNumberValue(cJSON_GetArrayItem(rec, 0));
    const int arch_id = (int) cJSON_GetNumberValue(cJSON_GetArrayItem(rec, 1));
    Map *map = current_map(client);
    const MapArchetype *arch = map ? map_archetype(map, arch_id) : NULL;
    E.spawns++;
    Entity *old = entity_find(eid);
    if (old) { // applySpawn of a known eid: despawn first
        for (int i = 0; i < old->part_count; i++) retire(client, old->parts[i].uid, old->parts[i].object);
        cJSON_Delete(old->state);
        *old = (Entity) {0};
    }
    if (!arch) return; // archetypes.get(id) missing: nothing (v10 does the same)
    Entity *e = NULL;
    for (int i = 0; i < ENT_MAX && !e; i++) {
        if (!E.ents[i].active) e = &E.ents[i];
    }
    if (!e) return;
    *e = (Entity) {0};
    e->active = 1;
    e->eid = eid;
    e->arch = arch_id;
    e->coll = arch->coll;
    const char *tag = cJSON_GetStringValue(cJSON_GetArrayItem(rec, 4));
    if (tag) snprintf(e->tag, sizeof(e->tag), "%s", tag);
    const cJSON *state = cJSON_GetArrayItem(rec, 3);
    e->state = cJSON_IsObject(state) ? cJSON_Duplicate(state, 1) : cJSON_CreateObject();
    commit_pose(e);
    // buildBody: one cube per solid part (soft parts are zones only)
    const cJSON *part;
    cJSON_ArrayForEach(part, arch->objects) {
        if (e->part_count >= ENT_PARTS_MAX) break;
        const cJSON *soft = cJSON_GetObjectItem(part, "soft");
        if (soft && !cJSON_IsFalse(soft) && !cJSON_IsNull(soft) && !(cJSON_IsNumber(soft) && soft->valuedouble == 0)) continue;
        const cJSON *pp = cJSON_GetObjectItem(part, "p"), *ps = cJSON_GetObjectItem(part, "s");
        EntPart *ep = &e->parts[e->part_count];
        *ep = (EntPart) {0};
        ep->off = (vec3) {cJSON_IsArray(pp) ? (float) cJSON_GetNumberValue(cJSON_GetArrayItem(pp, 0)) : 0.0f,
                          cJSON_IsArray(pp) ? (float) cJSON_GetNumberValue(cJSON_GetArrayItem(pp, 1)) : 0.0f,
                          cJSON_IsArray(pp) ? (float) cJSON_GetNumberValue(cJSON_GetArrayItem(pp, 2)) : 0.0f};
        ep->size = (vec3) {cJSON_IsArray(ps) ? (float) cJSON_GetNumberValue(cJSON_GetArrayItem(ps, 0)) : 2.0f,
                           cJSON_IsArray(ps) ? (float) cJSON_GetNumberValue(cJSON_GetArrayItem(ps, 1)) : 2.0f,
                           cJSON_IsArray(ps) ? (float) cJSON_GetNumberValue(cJSON_GetArrayItem(ps, 2)) : 2.0f};
        if (ep->off.x != ep->off.x) ep->off.x = 0.0f; // NaN from a missing entry
        if (ep->off.y != ep->off.y) ep->off.y = 0.0f;
        if (ep->off.z != ep->off.z) ep->off.z = 0.0f;
        if (!(ep->size.x == ep->size.x) || ep->size.x <= 0.0f) ep->size.x = 2.0f;
        if (!(ep->size.y == ep->size.y) || ep->size.y <= 0.0f) ep->size.y = 2.0f;
        if (!(ep->size.z == ep->size.z) || ep->size.z <= 0.0f) ep->size.z = 2.0f;
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(part, "n"));
        if (name && strlen(name) <= 12) snprintf(ep->name, sizeof(ep->name), "%s", name);
        cJSON *record = cJSON_CreateObject();
        cJSON_AddItemToObject(record, "p", cJSON_CreateFloatArray((const float[]) {e->pos.x + ep->off.x, e->pos.y + ep->off.y, e->pos.z + ep->off.z}, 3));
        cJSON_AddItemToObject(record, "s", cJSON_CreateFloatArray((const float[]) {ep->size.x, ep->size.y, ep->size.z}, 3));
        cJSON_AddNumberToObject(record, "dyn", 1);
        const cJSON *c = cJSON_GetObjectItem(part, "c"), *op = cJSON_GetObjectItem(part, "o");
        if (cJSON_IsString(c)) cJSON_AddStringToObject(record, "c", c->valuestring);
        if (cJSON_IsNumber(op)) cJSON_AddNumberToObject(record, "o", op->valuedouble);
        if (e->coll <= 0) cJSON_AddNumberToObject(record, "col", 1);                         // noCol
        if (cJSON_GetObjectItem(part, "aid") && !cJSON_IsNull(cJSON_GetObjectItem(part, "aid"))) cJSON_AddNumberToObject(record, "v", 1); // a model part
        const Object *object = NULL;
        const size_t uid = append_cube(client, record, &object);
        if (!object) continue;
        if (e->coll == 2) ((Object *) object)->no_player_col = 1; // noPCol
        ep->uid = uid;
        ep->object = object;
        e->part_count++;
    }
    place_body(client, e, (vec3) {0.0f, 0.0f, 0.0f});
}

// applyBaselines: the server's state; the body snaps, the mesh blends from where it was (100 ms)
static void entity_state(Client *client, const cJSON *rec) {
    Entity *e = cJSON_IsArray(rec) ? entity_find((int) cJSON_GetNumberValue(cJSON_GetArrayItem(rec, 0))) : NULL;
    E.states++;
    if (!e) return;
    const cJSON *state = cJSON_GetArrayItem(rec, 2);
    const vec3 was = e->pos;
    cJSON_Delete(e->state);
    e->state = cJSON_IsObject(state) ? cJSON_Duplicate(state, 1) : cJSON_CreateObject();
    commit_pose(e);
    const vec3 d = {e->pos.x - was.x, e->pos.y - was.y, e->pos.z - was.z};
    if (sqrtf(d.x * d.x + d.y * d.y + d.z * d.z) > 0.01f) {
        e->blend = (vec3) {-d.x, -d.y, -d.z};
        e->blend_left = ENT_BLEND_MS;
    }
    place_body(client, e, d);
}

// applyControlledSync (deob-10.0.3.js:213849-213859): the entity we control, from our "l" ([eid, tick, state],
// NET_EXT_ENTITY_CONTROLLED): the server's state as it is, commitPose and placeBody(e, 0, 0, 0). Unlike a baseline
// there is no `active` check and no new blend (one under way goes on): the body is at the server's pose at once.
static void entity_controlled(Client *client, const cJSON *rec) {
    Entity *e = cJSON_IsArray(rec) ? entity_find((int) cJSON_GetNumberValue(cJSON_GetArrayItem(rec, 0))) : NULL;
    if (!e) return;
    const cJSON *state = cJSON_GetArrayItem(rec, 2);
    cJSON_Delete(e->state);
    e->state = cJSON_IsObject(state) ? cJSON_Duplicate(state, 1) : cJSON_CreateObject(); // decodeState(s || {})
    commit_pose(e);
    place_body(client, e, (vec3) {0.0f, 0.0f, 0.0f});
}

static void entity_remove(Client *client, const cJSON *rec) {
    Entity *e = cJSON_IsArray(rec) ? entity_find((int) cJSON_GetNumberValue(cJSON_GetArrayItem(rec, 0))) : NULL;
    E.removes++;
    if (!e) return;
    for (int i = 0; i < e->part_count; i++) retire(client, e->parts[i].uid, e->parts[i].object);
    if (E.my_attached_eid == e->eid) E.my_attached_eid = -1;
    for (int i = E.rider_count - 1; i >= 0; i--) // detachPlayer for its riders
        if (E.riders[i].eid == e->eid) E.riders[i] = E.riders[--E.rider_count];
    cJSON_Delete(e->state);
    *e = (Entity) {0};
}

// player.attachedEnt of `sid`: eid, or -1 to forget it (a detach, a soft attachment)
static void rider_set(const int sid, const int eid) {
    int i = 0;
    while (i < E.rider_count && E.riders[i].sid != sid) i++;
    if (eid < 0) {
        if (i < E.rider_count) E.riders[i] = E.riders[--E.rider_count];
        return;
    }
    if (i == E.rider_count) {
        if (E.rider_count >= ENT_MAX) return;
        E.rider_count++;
    }
    E.riders[i].sid = sid;
    E.riders[i].eid = eid;
}

// applyAttach / applyDetach: our own hard attachment carries our player; every player's is kept for faceRide
static void entity_attach(Client *client, const cJSON *rec, const int attach) {
    if (!cJSON_IsArray(rec)) return;
    const int sid = (int) cJSON_GetNumberValue(cJSON_GetArrayItem(rec, 0));
    const int eid = (int) cJSON_GetNumberValue(cJSON_GetArrayItem(rec, 1));
    const cJSON *opts = cJSON_GetArrayItem(rec, 2), *soft = cJSON_GetObjectItem(opts, "soft");
    const int is_soft = soft && (cJSON_IsTrue(soft) || (cJSON_IsNumber(soft) && soft->valuedouble != 0));
    if (!attach) {
        int i = 0; // detachPlayer: only that entity's
        while (i < E.rider_count && E.riders[i].sid != sid) i++;
        if (i < E.rider_count && E.riders[i].eid == eid) rider_set(sid, -1);
    } else if (entity_find(eid)) { // applyAttach needs the entity
        rider_set(sid, is_soft ? -1 : eid);
    }
    if (!client->me) return;
    E.attaches++;
    if (sid != (int) client->me->uid) return;
    if (!attach) {
        if (E.my_attached_eid == eid) E.my_attached_eid = -1;
        return;
    }
    E.my_attached_eid = is_soft || !entity_find(eid) ? -1 : eid;
}

int entities_player_ride_yaw(const int uid, float *yaw) {
    for (int i = 0; i < E.rider_count; i++) {
        if (E.riders[i].sid != uid) continue;
        const Entity *e = entity_find(E.riders[i].eid);
        if (!e) return 0;
        if (yaw) *yaw = e->yaw;
        return 1;
    }
    return 0;
}

// ---------------------------------------------------------------- live blocks / moved objects

// syncNewObjects: [id, {type: "block", x, y (bottom), z, w, l, h, s, data}] -> manager.addBlock with
// data's props (src texture, colr, opacity, noCol)
static void live_add(Client *client, const cJSON *rec) {
    E.lives++;
    const cJSON *data = cJSON_GetArrayItem(rec, 1);
    const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(data, "type"));
    if (!cJSON_IsArray(rec) || !type || strcmp(type, "block") || E.live_count >= LIVE_MAX) return;
    const int id = (int) cJSON_GetNumberValue(cJSON_GetArrayItem(rec, 0));
    for (int i = 0; i < E.live_count; i++) {
        if (E.live[i].id == id) return; // already there (a joiner's init and a broadcast)
    }
    const float x = (float) state_num(data, "x", 0), y = (float) state_num(data, "y", 0), z = (float) state_num(data, "z", 0);
    const float w = (float) state_num(data, "w", 1), l = (float) state_num(data, "l", 1), h = (float) state_num(data, "h", 1);
    const cJSON *props = cJSON_GetObjectItem(data, "data");
    cJSON *record = cJSON_CreateObject();
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
    const Object *object = NULL;
    const size_t uid = append_cube(client, record, &object);
    if (object) E.live[E.live_count++] = (LiveBlock) {id, uid, object};
}

// syncLiveObjects: [id, "remove"]
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
        case NET_EXT_KN_OFFICIAL_LIVE_OBJECT:
        case NET_EXT_KN_OFFICIAL_LIVE_SYNC:
            break;
        default:
            return 0;
    }
    cJSON *rec = json_record(subtype, body, length);
    if (!rec) return 1;
    if (client->game.ready) {
        current_map(client);
        switch (subtype) {
            case NET_EXT_ENTITY_SPAWN: entity_spawn(client, rec); break;
            case NET_EXT_ENTITY_REMOVE: entity_remove(client, rec); break;
            case NET_EXT_ENTITY_STATE: entity_state(client, rec); break;
            case NET_EXT_ENTITY_CONTROLLED: entity_controlled(client, rec); break; // our "l"'s tail
            case NET_EXT_ENTITY_EVENT: E.events++; break; // v10: clientEventQueue -> the map's client script hooks (not run here)
            case NET_EXT_ENTITY_ATTACH: entity_attach(client, rec, 1); break;
            case NET_EXT_ENTITY_DETACH: entity_attach(client, rec, 0); break;
            case NET_EXT_KN_OFFICIAL_LIVE_OBJECT: live_add(client, rec); break;
            case NET_EXT_KN_OFFICIAL_LIVE_SYNC: live_sync(client, rec); break;
            default: break;
        }
    }
    cJSON_Delete(rec);
    return 1;
}

void entities_frame(Client *client, const float delta) {
    if (!E.map || current_map(client) != E.map) return;
    for (int i = 0; i < ENT_MAX; i++) {
        Entity *e = &E.ents[i];
        if (!e->active || e->blend_left <= 0.0f) continue;
        e->blend_left -= delta * 1000.0f;
        if (e->blend_left < 0.0f) e->blend_left = 0.0f;
        place_body(client, e, (vec3) {0.0f, 0.0f, 0.0f}); // the meshes follow the decaying blend
    }
}

void entities_describe(const Client *client, char *out, const size_t size) {
    (void) client;
    int active = 0, parts = 0, solid = 0;
    const Entity *first = NULL;
    for (int i = 0; i < ENT_MAX; i++) {
        if (!E.ents[i].active) continue;
        active++;
        parts += E.ents[i].part_count;
        solid += E.ents[i].coll > 0 ? E.ents[i].part_count : 0;
        if (!first) first = &E.ents[i];
    }
    snprintf(out, size, "entities=%d parts=%d solid=%d live=%d spawns=%u states=%u removes=%u events=%u attaches=%u lives=%u moves=%u "
                        "attached=%d first=%d@%.2f,%.2f,%.2f/%.2f",
             active, parts, solid, E.live_count, E.spawns, E.states, E.removes, E.events, E.attaches, E.lives, E.moves, E.my_attached_eid,
             first ? first->eid : -1, first ? first->pos.x : 0.0f, first ? first->pos.y : 0.0f, first ? first->pos.z : 0.0f,
             first ? first->yaw : 0.0f);
}
