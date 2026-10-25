// KrunkScript entities: the v10.0.3 entity manager (deob-10.0.3.js:212412-214818; v10.0.2 L184033-185900;
// doc/v10/map-format.md section 4, doc/v10/krunkscript-1003.md section 5) and LIVE_OBJECTS (:212096-212411).
//
// The authority (the server, and the offline sandbox's server core) runs the simulation: the map's ENTITY
// instances spawn at the round start (spawnPending), each entity steps once per 33.33 ms of the round clock
// through the scripts.shared simulateEntity hook, and the entity hooks reach the main script after each
// advance (queueServerHook / drainServerHooks). It replicates them (e+ e- es ee ea+ ea-, entSeed, the "l"
// tail; shared/include/ks_entities.h) and runs the player attachments (attachPlayer / updateAttachment /
// detachPlayer, pinPlayer, pinAllServer, the soft zones, the controlled entities).
//
// A client keeps the replicated copy in the same manager: applySpawn, applyBaselines (replaying the ticks it
// already ran), applyRemove, applyAttach / applyDetach, applyControlledSync. Between baselines it steps its
// "sim" entities with its own scripts.shared on the entity clock (advance), the entity its player controls
// with that player's inputs (stepControlled from procInputs), and keeps the render poses (getRenderPose:
// simBuf, poseBuf, the blend). client/src/entities.c keeps the bodies.
//
// Deliberate differences (each also noted where it is made):
// - the server builds no bodies (buildBody / placeBody / carryPlayers): its entities collide with the map
//   (collideWorld) and with each other (separateAll), but players do not stand on them on the server;
// - a soft rider's movX / movY / movZ move the player at once (v10 applies them in pinAllServer right after);
// - the soft zones' onTopOf test is "a static map object right under the feet" (KN keeps no onTopOf);
// - the match seed is the shared script's hash with the map (ks_game_load_shared), sent as entSeed;
// - without scripts.shared a client still places its entities from the baselines (v10 runs none of
//   game.update's entity half without hooks, so its meshes would stay where buildBody put them).
#include "ks_game_internal.h"
#include <ks_entities.h>
#include <net_ext.h>
#include <ruleset.h> // game_constants_of: dltMx
#include <ai_parts.h> // a live asset's model (loadTris)
#include <prefabs.h> // a live block's texture

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ENT_CTX_EVENT_NAME_MAX KS_ENT_NAME_MAX
#define LIVE_OBJECTS_MAX 200 // liveObjects.length >= 200: no more (:212117, :212146)
// spawn_owner of the server's body / live block objects: not `spawned` (world_remove_builds and the Object Manager leave
// them alone, so no joiner gets them as Object Manager objects); ks_entities_reset takes them out of the map
#define KS_ENT_OBJECT_OWNER (-77)

extern const KsHostClass ks_entity_class;     // the entity host object (below)
extern const KsHostClass ks_entity_ctx_class; // the simulateEntity ctx (below)

void ks_game_log_error(KsGame *kg, const char *line)
{
    if (kg && kg->host && kg->host->log) kg->host->log(kg->host_user, 1, line);
}

// logScriptError (:214811): "[ENTITIES] " + the line, as an error
static void ent_log(KsGame *kg, const char *line)
{
    char buf[400];
    snprintf(buf, sizeof(buf), "[ENTITIES] %s", line);
    ks_game_log_error(kg, buf);
}

// ---------------------------------------------------------------------------
// Lookups and storage

static KsEntity *ent_find(KsGame *kg, const int eid)
{
    for (size_t i = 0; kg && i < kg->ents.count; i++)
        if (kg->ents.items[i]->eid == eid) return kg->ents.items[i];
    return NULL;
}

// The entity a host wrapper stands for (its KsRef's id), NULL once despawned.
static KsEntity *ent_of(void *native)
{
    KsRef *r = native;
    if (!r || r->kind != KS_REF_ENTITY || !r->kg) return NULL;
    return ent_find(r->kg, r->id);
}

// The script's server side (the wrapper's iïIïíìi): writes and spawns
static int authority(const KsGame *kg) { return ks_game_is_authority_side(kg); }
// The manager's isServer: the Game that runs the match (KN's server and its offline sandbox)
static int server_side(const KsGame *kg) { return kg && kg->game && game_is_authority(kg->game); }
// this.hooks: scripts.shared, loaded and not failed
static int has_hooks(const KsGame *kg) { return kg && kg->shared_script && !ks_script_failed(kg->shared_script); }

static void ent_free(KsEntity *e)
{
    cJSON_Delete(e->state);
    cJSON_Delete(e->params);
    cJSON_Delete(e->prev_wire);
    free(e);
}

static KsEntHook *queue_push(KsGame *kg)
{
    KsEntities *ents = &kg->ents;
    if (ents->queue_count == ents->queue_cap) {
        const size_t cap = ents->queue_cap ? ents->queue_cap * 2 : 16;
        KsEntHook *grown = realloc(ents->queue, cap * sizeof(*grown));
        if (!grown) return NULL;
        ents->queue = grown;
        ents->queue_cap = cap;
    }
    KsEntHook *h = &ents->queue[ents->queue_count++];
    memset(h, 0, sizeof(*h));
    return h;
}

static int insert_entity(KsGame *kg, KsEntity *e)
{
    KsEntities *ents = &kg->ents;
    if (ents->count == ents->cap) {
        const size_t cap = ents->cap ? ents->cap * 2 : 16;
        KsEntity **grown = realloc(ents->items, cap * sizeof(*grown));
        if (!grown) return 0;
        ents->items = grown;
        ents->cap = cap;
    }
    ents->items[ents->count++] = e;
    return 1;
}

static void unlink_entity(KsGame *kg, const KsEntity *e)
{
    KsEntities *ents = &kg->ents;
    for (size_t i = 0; i < ents->count; i++) {
        if (ents->items[i] != e) continue;
        memmove(&ents->items[i], &ents->items[i + 1], (ents->count - i - 1) * sizeof(*ents->items));
        ents->count--;
        return;
    }
}

static Player *player_by_sid(KsGame *kg, const int sid) { return kg && kg->game ? ks_game_player_by_uid(kg->game, (uint32_t) sid) : NULL; }

static void player_id_text(const Player *p, const int sid, char *out, const size_t n)
{
    if (p) ks_player_id(p, out, n);
    else snprintf(out, n, "%d", sid);
}

static double clamp_d(const double v, const double lo, const double hi) { return v < lo ? lo : v > hi ? hi : v; }

// ---------------------------------------------------------------------------
// The wire: one JSON record as NetExtJsonChunk slices of `subtype` (net_ext.h), the authority only

static void net_json(KsGame *kg, const int target, const uint16_t subtype, cJSON *rec)
{
    if (!rec) return;
    char *json = server_side(kg) ? cJSON_PrintUnformatted(rec) : NULL;
    cJSON_Delete(rec);
    if (!json) return;
    const size_t len = strlen(json), slice = (size_t) NET_EXT_JSON_CHUNK_DATA;
    const size_t parts = len ? (len + slice - 1) / slice : 1;
    for (size_t i = 0; parts <= 255 && i < parts; i++) { // a record never needs more (512-byte states)
        uint8_t body[NET_EXT_BODY_MAX];
        const size_t n = i * slice + slice <= len ? slice : len - i * slice;
        const NetExtJsonChunk head = {(uint8_t) i, (uint8_t) parts};
        memcpy(body, &head, sizeof(head));
        memcpy(body + sizeof(head), json + i * slice, n);
        uint8_t buf[NET_EXT_HEADER_SIZE + NET_EXT_BODY_MAX];
        const uint16_t out = net_ext_pack(buf, sizeof(buf), subtype, body, (uint16_t) (sizeof(head) + n));
        if (out) game_send(kg->game, target, NET_PACKET_EXT, buf, out);
    }
    free(json);
}

static cJSON *state_copy(const cJSON *state) { return state ? cJSON_Duplicate(state, 1) : cJSON_CreateObject(); }

// spawnPayload (:212787): [eid, arch.id, tick, encodeState(state), tag, mode, hp]
static cJSON *spawn_payload(const KsEntity *e)
{
    cJSON *rec = cJSON_CreateArray();
    if (!rec) return NULL;
    cJSON_AddItemToArray(rec, cJSON_CreateNumber(e->eid));
    cJSON_AddItemToArray(rec, cJSON_CreateNumber(e->arch->id));
    cJSON_AddItemToArray(rec, cJSON_CreateNumber(e->tick));
    cJSON_AddItemToArray(rec, state_copy(e->state));
    cJSON_AddItemToArray(rec, cJSON_CreateString(e->tag));
    cJSON_AddItemToArray(rec, cJSON_CreateString(e->mode));
    cJSON_AddItemToArray(rec, cJSON_CreateNumber(e->hp));
    return rec;
}

// [eid, tick, encodeState(state)]: a baseline, the controlled sync
static cJSON *state_record(const KsEntity *e)
{
    cJSON *rec = cJSON_CreateArray();
    if (!rec) return NULL;
    cJSON_AddItemToArray(rec, cJSON_CreateNumber(e->eid));
    cJSON_AddItemToArray(rec, cJSON_CreateNumber(e->tick));
    cJSON_AddItemToArray(rec, state_copy(e->state));
    return rec;
}

// broadcastBaseline (:214382)
static void broadcast_baseline(KsGame *kg, const KsEntity *e)
{
    if (server_side(kg)) net_json(kg, NET_TARGET_ALL, NET_EXT_ENTITY_STATE, state_record(e));
}

static cJSON *event_record(const int eid, const char *name, const double a, const double b)
{
    cJSON *rec = cJSON_CreateArray();
    if (!rec) return NULL;
    cJSON_AddItemToArray(rec, cJSON_CreateNumber(eid));
    cJSON_AddItemToArray(rec, cJSON_CreateString(name));
    cJSON_AddItemToArray(rec, cJSON_CreateNumber(a));
    cJSON_AddItemToArray(rec, cJSON_CreateNumber(b));
    return rec;
}

static cJSON *attach_record(const KsEntAttach *a)
{
    cJSON *rec = cJSON_CreateArray(), *opts = cJSON_CreateObject();
    if (!rec || !opts) {
        cJSON_Delete(rec);
        cJSON_Delete(opts);
        return NULL;
    }
    cJSON_AddItemToObject(opts, "o", cJSON_CreateDoubleArray(a->o, 3));
    cJSON_AddNumberToObject(opts, "lk", a->lk);
    cJSON_AddNumberToObject(opts, "ctl", a->ctl);
    cJSON_AddNumberToObject(opts, "soft", a->soft);
    cJSON_AddNumberToObject(opts, "nd", a->nd);
    cJSON_AddItemToArray(rec, cJSON_CreateNumber(a->sid));
    cJSON_AddItemToArray(rec, cJSON_CreateNumber(a->eid));
    cJSON_AddItemToArray(rec, opts);
    return rec;
}

static cJSON *pair_record(const double a, const double b)
{
    cJSON *rec = cJSON_CreateArray();
    if (!rec) return NULL;
    cJSON_AddItemToArray(rec, cJSON_CreateNumber(a));
    cJSON_AddItemToArray(rec, cJSON_CreateNumber(b));
    return rec;
}

// ---------------------------------------------------------------------------
// Random streams (ctx.rand): buildCtx's murmur-style seed of (matchSeed, eid, tick) and the generator of
// the step, word for word (:213131-213155).

static uint32_t rotl32(const uint32_t x, const int r) { return (x << r) | (x >> (32 - r)); }

static uint32_t ent_rand_seed(const uint32_t seed, const int eid, const long tick)
{
    const uint32_t words[3] = {seed, (uint32_t) eid, (uint32_t) tick};
    uint32_t h = 0x9E3779B9u;
    for (int i = 0; i < 3; i++) {
        uint32_t k = words[i] * 0xCC9E2D51u;
        k = rotl32(k, 15) * 0x1B873593u;
        h ^= k;
        h = rotl32(h, 13) * 5u + 0xE6546B64u;
    }
    h ^= 3u;
    h ^= h >> 16;
    h *= 0x85EBCA6Bu;
    h ^= h >> 13;
    h *= 0xC2B2AE35u;
    h ^= h >> 16;
    return h;
}

static double ent_rand_next(uint32_t *state)
{
    *state += 0x9E3779B9u;
    uint32_t s = *state;
    s = (s ^ (s >> 16)) * 569420461u;
    s = (s ^ (s >> 15)) * 1935289751u;
    s ^= s >> 15;
    return 2.3283064365386963e-10 * (double) s;
}

// ---------------------------------------------------------------------------
// State: validation (validateState, :212623) and the JSON / script value conversions

// The checks of a state in v10's order: key length, value kind, string length, finite numbers,
// then the key count and the byte budget. NULL-valued fields (a script's object or array value)
// fail the kind check.
static int state_check(const cJSON *state, char *err, const size_t n)
{
    size_t keys = 0, bytes = 2;
    for (const cJSON *it = state ? state->child : NULL; it; it = it->next) {
        const char *key = it->string ? it->string : "";
        const size_t klen = strlen(key);
        if (klen > 20) {
            snprintf(err, n, "state key too long: %s", key);
            return -1;
        }
        if (cJSON_IsNumber(it)) {
            if (!isfinite(it->valuedouble)) {
                snprintf(err, n, "state number not finite: %s", key);
                return -1;
            }
            bytes += klen + 4 + 8;
        } else if (cJSON_IsBool(it)) {
            bytes += klen + 4 + 8;
        } else if (cJSON_IsString(it)) {
            const size_t slen = strlen(it->valuestring);
            if (slen > 64) {
                snprintf(err, n, "state string too long: %s", key);
                return -1;
            }
            bytes += klen + 4 + slen;
        } else {
            snprintf(err, n, "state value must be num/bool/str: %s", key);
            return -1;
        }
        keys++;
    }
    if (keys > 32) {
        snprintf(err, n, "too many state keys");
        return -1;
    }
    if (bytes > 512) {
        snprintf(err, n, "state too large");
        return -1;
    }
    return 0;
}

// A script value as JSON: num / bool / str keep their kind, anything else is a null (refused by
// state_check).
static cJSON *json_from_value(KsScript *s, const KsValue v)
{
    (void) s;
    switch (v.tag) {
        case KS_NUM: return cJSON_CreateNumber(v.as.num);
        case KS_BOOL: return cJSON_CreateBool(v.as.b);
        case KS_STR: {
            size_t len = 0;
            const char *text = ks_str_utf8(v, &len);
            char *copy = malloc(len + 1);
            if (!copy) return cJSON_CreateNull();
            memcpy(copy, text ? text : "", len);
            copy[len] = 0;
            cJSON *out = cJSON_CreateString(copy);
            free(copy);
            return out ? out : cJSON_CreateNull();
        }
        default: return cJSON_CreateNull();
    }
}

// `lenient` (spawn params) leaves non-scalar fields out instead.
static cJSON *json_object_from_value(KsScript *s, const KsValue obj, const int lenient)
{
    cJSON *out = cJSON_CreateObject();
    if (!out) return NULL;
    for (size_t i = 0; i < ks_obj_count(obj); i++) {
        KsValue k, v;
        if (!ks_obj_entry(s, obj, i, &k, &v)) continue;
        size_t klen = 0;
        const char *key = ks_str_utf8(k, &klen);
        if (!key) continue;
        if (lenient && v.tag != KS_NUM && v.tag != KS_BOOL && v.tag != KS_STR) continue;
        char *name = malloc(klen + 1);
        if (!name) continue;
        memcpy(name, key, klen);
        name[klen] = 0;
        cJSON_AddItemToObject(out, name, json_from_value(s, v));
        free(name);
    }
    return out;
}

// A JSON value as a script value (archetype data, spawn params): objects and arrays nest.
static KsValue value_from_json(KsScript *s, const cJSON *j)
{
    if (cJSON_IsNumber(j)) return ks_num(j->valuedouble);
    if (cJSON_IsBool(j)) return ks_bool(cJSON_IsTrue(j));
    if (cJSON_IsString(j)) return ks_str(s, j->valuestring, strlen(j->valuestring));
    if (cJSON_IsArray(j)) {
        KsValue arr = ks_arr_new(s);
        for (const cJSON *c = j->child; c; c = c->next) ks_arr_push(s, arr, value_from_json(s, c));
        return arr;
    }
    if (cJSON_IsObject(j)) {
        KsValue obj = ks_obj_new(s);
        for (const cJSON *c = j->child; c; c = c->next)
            if (c->string) ks_obj_set(s, obj, c->string, value_from_json(s, c));
        return obj;
    }
    return ks_null();
}

// A state object as a script value (the hooks' state argument).
static KsValue state_value(KsScript *s, const cJSON *state)
{
    KsValue obj = ks_obj_new(s);
    for (const cJSON *c = state ? state->child : NULL; c; c = c->next)
        if (c->string) ks_obj_set(s, obj, c->string, value_from_json(s, c));
    return obj;
}

// A script object as a checked state. 0 with *out set, or -1 with the reason in err.
static int state_from_value(KsScript *s, const KsValue v, cJSON **out, char *err, const size_t n)
{
    *out = NULL;
    if (v.tag != KS_OBJ) {
        snprintf(err, n, "state must be an object");
        return -1;
    }
    cJSON *json = json_object_from_value(s, v, 0);
    if (!json) {
        snprintf(err, n, "out of memory");
        return -1;
    }
    if (state_check(json, err, n)) {
        cJSON_Delete(json);
        return -1;
    }
    *out = json;
    return 0;
}

// A finite number of the state (iííííîî): 1 with *out set
static int state_finite(const cJSON *state, const char *key, double *out)
{
    const cJSON *v = state ? cJSON_GetObjectItemCaseSensitive(state, key) : NULL;
    if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble)) return 0;
    *out = v->valuedouble;
    return 1;
}

static void state_set_number(cJSON *state, const char *key, const double value)
{
    cJSON *item = cJSON_CreateNumber(value);
    if (!item) return;
    if (!cJSON_ReplaceItemInObjectCaseSensitive(state, key, item)) cJSON_AddItemToObject(state, key, item);
}

// commitPose (:213281): prevPose = the pose, then the state's finite x y z yaw pitch roll
static void commit_pose(KsEntity *e)
{
    e->prev = (KsEntPose) {0.0, e->x, e->y, e->z, e->yaw, e->pitch, e->roll};
    e->has_prev = 1;
    if (!e->state) return;
    double v;
    if (state_finite(e->state, "x", &v)) e->x = v;
    if (state_finite(e->state, "y", &v)) e->y = v;
    if (state_finite(e->state, "z", &v)) e->z = v;
    if (state_finite(e->state, "yaw", &v)) e->yaw = v;
    if (state_finite(e->state, "pitch", &v)) e->pitch = v;
    if (state_finite(e->state, "roll", &v)) e->roll = v;
}

// buildBody's colObjs (:212849-212901): the solid parts, p || [0, 0, 0], s || [2, 2, 2], the skeleton name
// when rec.n matches /^\w{1,12}$/
static void build_parts(KsEntity *e)
{
    e->part_count = 0;
    const cJSON *part;
    cJSON_ArrayForEach(part, e->arch->objects) {
        if (e->part_count >= KS_ENT_PARTS) break;
        const cJSON *soft = cJSON_GetObjectItemCaseSensitive(part, "soft");
        if (soft && !cJSON_IsFalse(soft) && !cJSON_IsNull(soft) && !(cJSON_IsNumber(soft) && soft->valuedouble == 0) &&
            !(cJSON_IsString(soft) && !soft->valuestring[0]))
            continue;
        KsEntPart *ep = &e->parts[e->part_count++];
        memset(ep, 0, sizeof(*ep));
        ep->rec = part;
        const cJSON *p = cJSON_GetObjectItemCaseSensitive(part, "p"), *sz = cJSON_GetObjectItemCaseSensitive(part, "s");
        for (int k = 0; k < 3; k++) {
            const cJSON *pv = cJSON_IsArray(p) ? cJSON_GetArrayItem(p, k) : NULL, *sv = cJSON_IsArray(sz) ? cJSON_GetArrayItem(sz, k) : NULL;
            ep->off[k] = cJSON_IsNumber(pv) ? pv->valuedouble : 0.0;
            ep->size[k] = cJSON_IsArray(sz) ? (cJSON_IsNumber(sv) ? sv->valuedouble : 0.0) : 2.0;
        }
        const cJSON *aid = cJSON_GetObjectItemCaseSensitive(part, "aid");
        ep->is_model = aid && !cJSON_IsNull(aid);
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(part, "n"));
        const size_t len = name ? strlen(name) : 0;
        int word = len >= 1 && len <= 12;
        for (size_t i = 0; word && i < len; i++) word = (name[i] >= '0' && name[i] <= '9') || (name[i] >= 'a' && name[i] <= 'z') || (name[i] >= 'A' && name[i] <= 'Z') || name[i] == '_';
        if (word) snprintf(ep->name, sizeof(ep->name), "%s", name);
    }
}

// partPose (:212975): the part's offset plus its skeleton keys (<n>_x / _y / _z added, _yaw / _pitch / _roll set)
static KsEntPose part_pose(const KsEntity *e, const KsEntPart *p)
{
    KsEntPose o = {0.0, p->off[0], p->off[1], p->off[2], 0.0, 0.0, 0.0};
    if (!p->name[0] || !e->state) return o;
    static const char *const axes[6] = {"x", "y", "z", "yaw", "pitch", "roll"};
    double *slot[6] = {&o.x, &o.y, &o.z, &o.yaw, &o.pitch, &o.roll};
    for (int k = 0; k < 6; k++) {
        char key[40];
        snprintf(key, sizeof(key), "%s_%s", p->name, axes[k]);
        double v;
        if (!state_finite(e->state, key, &v)) continue;
        if (k < 3) *slot[k] += v;
        else *slot[k] = v;
    }
    return o;
}


// ---------------------------------------------------------------------------
// Bodies (buildBody :212839, placeBody :213050, carryPlayers :213091, removeBody :213020) and the map objects

static KsEntAttach *att_find(KsGame *kg, const int sid);

// Our object still at its uid (the slot may have been replaced, the map changed or freed)
static Object *our_object(const KsGame *kg, const size_t uid, const Object *object)
{
    const Map *map = kg && kg->game ? kg->game->map : NULL;
    return map && map == kg->ents.obj_map && object && uid < map->object_count && map->objects[uid] == object ? map->objects[uid] : NULL;
}

// manager.addBlock's object: the record as a map object appended after the map's own (a client builds its mesh, which
// its map_post shows: `spawned`); NULL on failure. The record is owned by the manager from here on.
static Object *append_object(KsGame *kg, cJSON *record, size_t *uid)
{
    Map *map = kg && kg->game ? kg->game->map : NULL;
    if (!map || !record) {
        cJSON_Delete(record);
        return NULL;
    }
    if (kg->ents.obj_map != map) { // the old map's objects went with it: so did the need for their records
        cJSON_Delete(kg->ents.records);
        kg->ents.records = NULL;
        kg->ents.obj_map = map;
    }
    if (!kg->ents.records) kg->ents.records = cJSON_CreateArray();
    if (!kg->ents.records) {
        cJSON_Delete(record);
        return NULL;
    }
    cJSON_AddItemToArray(kg->ents.records, record);
    const int client = !server_side(kg);
    Object *o = map_object_from_json(map, record, client);
    if (!o) return NULL;
    Object **grown = realloc(map->objects, (map->object_count + 1) * sizeof(Object *));
    if (!grown) {
        map_object_free(o);
        return NULL;
    }
    map->objects = grown;
    o->spawned = (unsigned char) client;
    o->spawn_owner = client ? -1 : KS_ENT_OBJECT_OWNER;
    o->destroyed_by = -1;
    map->objects[map->object_count] = o;
    *uid = map->object_count++;
    return o;
}

// removeObject: the object stays as an inactive slot (uids never move mid-round), no collision, its mesh hidden
// (map_post shows a spawned object while it is active)
static void retire_object(KsGame *kg, const size_t uid, const Object *object)
{
    Object *o = our_object(kg, uid, object);
    if (!o) return;
    o->active = 0;
    o->destroyed = 1;
    o->collision_type = COLLISION_TYPE_NONE;
}

// The server's objects leave the map at a reset (a new round: world_remove_builds kept them); a client's are freed
// with its map (spawned objects), so a client only forgets them.
static void drop_objects(KsGame *kg)
{
    Map *map = kg && kg->game ? kg->game->map : NULL;
    if (server_side(kg) && map && map == kg->ents.obj_map) {
        size_t kept = 0;
        for (size_t i = 0; i < map->object_count; i++) {
            Object *o = map->objects[i];
            if (o && !o->spawned && o->spawn_owner == KS_ENT_OBJECT_OWNER) map_object_free(o);
            else map->objects[kept++] = o;
        }
        map->object_count = kept;
    }
    cJSON_Delete(kg->ents.records);
    kg->ents.records = NULL;
    kg->ents.obj_map = NULL;
}

// buildBody (:212839): a body only when solid (coll > 0) on the server; a client builds every entity's (noCol unless
// coll > 0). One addBlock per solid part: dyn, noCol, noPCol for coll 2, noVis for a model part ("aid"), the part's
// colour and opacity.
static void place_body(KsGame *kg, KsEntity *e, double dx, double dy, double dz);

static void build_body(KsGame *kg, KsEntity *e)
{
    const int solid = e->arch->coll > 0;
    if (!kg->game || !kg->game->map || (!solid && server_side(kg))) return;
    for (int k = 0; k < e->part_count; k++) {
        KsEntPart *p = &e->parts[k];
        double size[3];
        for (int a = 0; a < 3; a++) size[a] = p->size[a] > 0.0 ? p->size[a] : 0.01; // (a degenerate box still places)
        cJSON *record = cJSON_CreateObject();
        if (!record) continue;
        cJSON_AddItemToObject(record, "p", cJSON_CreateDoubleArray((const double[]) {e->x + p->off[0], e->y + p->off[1], e->z + p->off[2]}, 3));
        cJSON_AddItemToObject(record, "s", cJSON_CreateDoubleArray(size, 3));
        cJSON_AddNumberToObject(record, "dyn", 1);
        const cJSON *c = cJSON_GetObjectItemCaseSensitive(p->rec, "c"), *op = cJSON_GetObjectItemCaseSensitive(p->rec, "o");
        if (cJSON_IsString(c)) cJSON_AddStringToObject(record, "c", c->valuestring);
        if (cJSON_IsNumber(op)) cJSON_AddNumberToObject(record, "o", op->valuedouble);
        if (!solid) cJSON_AddNumberToObject(record, "col", 1);       // noCol
        if (p->is_model) cJSON_AddNumberToObject(record, "v", 1);  // noVis: a model part
        Object *o = append_object(kg, record, &p->uid);
        if (!o) continue;
        if (e->arch->coll == 2) o->no_player_col = 1; // noPCol
        p->object = o;
    }
    place_body(kg, e, 0.0, 0.0, 0.0);
}

static void remove_body(KsGame *kg, KsEntity *e)
{
    for (int k = 0; k < e->part_count; k++) {
        retire_object(kg, e->parts[k].uid, e->parts[k].object);
        e->parts[k].object = NULL;
    }
}

// onTopOf for carryPlayers: grounded, the feet at the part's top (before it moved), inside its footprint. KN keeps no
// onTopOf, so the stand is measured.
static int stood_on(const Player *p, const vec3 bottom, const vec3 size)
{
    const float r = game_constants.player_scale;
    return p->on_ground && fabsf(p->position.y - (bottom.y + size.y)) < 0.6f && fabsf(p->position.x - bottom.x) <= size.x * 0.5f + r &&
           fabsf(p->position.z - bottom.z) <= size.z * 0.5f + r;
}

// iíïîiìi (:214853): the overlap of two centred spans
static double span_overlap(const double a, const double ah, const double b, const double bh)
{
    return fmax(0.0, fmin(a + ah, b + bh) - fmax(a - ah, b - bh));
}

// carryPlayers (:213091): a moved solid part takes the players standing on it along, and pushes the ones it now
// overlaps by their overlap along its motion; not its own riders. A client moves only its own player (the others'
// movX is never applied there). KN moves them at once (v10 adds movX / movY / movZ, which procInputs applies) and has
// no class hit box pads (hbS / hbH 0).
static void carry_players(KsGame *kg, const KsEntity *e, const Object *o, const vec3 was, const vec3 size, const double dx, const double dy,
                          const double dz)
{
    Game *game = kg->game;
    const double len = sqrt(dx * dx + dy * dy + dz * dz);
    if (!game || !len) return;
    for (size_t i = 0; i < game->player_count; i++) {
        Player *p = game->players[i];
        if (!p || !p->active || (!server_side(kg) && !p->is_you)) continue;
        const KsEntAttach *a = att_find(kg, p->uid);
        if (a && a->eid == e->eid) continue;
        if (stood_on(p, was, size)) {
            p->position.x += (float) dx;
            p->position.y += (float) dy;
            p->position.z += (float) dz;
            continue;
        }
        const double s = p->scale, cy = o->position.y + size.y * 0.5;
        const int touch = p->position.x - s <= o->position.x + size.x * 0.5 - 0.001 && p->position.x + s >= o->position.x - size.x * 0.5 + 0.001 &&
                          p->position.z - s <= o->position.z + size.z * 0.5 - 0.001 && p->position.z + s >= o->position.z - size.z * 0.5 + 0.001 &&
                          p->position.y <= o->position.y + size.y && p->position.y + p->height >= o->position.y;
        if (!touch) continue;
        p->position.x += (float) (span_overlap(o->position.x, size.x * 0.5, p->position.x, s) * (dx / len));
        p->position.y += (float) (span_overlap(cy, size.y * 0.5, p->position.y, p->height) * (dy / len));
        p->position.z += (float) (span_overlap(o->position.z, size.z * 0.5, p->position.z, s) * (dz / len));
    }
}

// placeBody (:213050): every part at the entity's pose (the offset turned by the yaw, the box swapped by |sin| / |cos|);
// a moved solid part carries players (not for noPCol)
static void place_body(KsGame *kg, KsEntity *e, const double dx, const double dy, const double dz)
{
    const double s = sin(e->yaw), c = cos(e->yaw), as = fabs(s), ac = fabs(c);
    const int moved = dx || dy || dz;
    for (int k = 0; k < e->part_count; k++) {
        KsEntPart *p = &e->parts[k];
        Object *o = our_object(kg, p->uid, p->object);
        if (!o) continue;
        const KsEntPose pp = part_pose(e, p);
        const double rx = pp.x * c + pp.z * s, rz = -pp.x * s + pp.z * c;
        const vec3 was = o->position, size_was = object_box_size(o);
        o->position = (vec3) {(float) (e->x + rx), (float) (e->y + pp.y), (float) (e->z + rz)};
        o->scale = (vec3) {(float) (p->size[0] * ac + p->size[2] * as), (float) p->size[1], (float) (p->size[0] * as + p->size[2] * ac)};
        if (moved && e->arch->coll > 0 && !o->no_player_col) carry_players(kg, e, o, was, size_was, dx, dy, dz);
    }
}

// ---------------------------------------------------------------------------
// Attachments (attachPlayer / updateAttachment / detachPlayer / pinPlayer, :213435-213625)

static KsEntAttach *att_find(KsGame *kg, const int sid)
{
    for (size_t i = 0; kg && i < kg->ents.att_count; i++)
        if (kg->ents.att[i].sid == sid) return &kg->ents.att[i];
    return NULL;
}

// attachments.set(sid, rec): an existing key keeps its place (Map order)
static KsEntAttach *att_set(KsGame *kg, const KsEntAttach *rec)
{
    KsEntAttach *a = att_find(kg, rec->sid);
    if (!a) {
        KsEntities *ents = &kg->ents;
        if (ents->att_count == ents->att_cap) {
            const size_t cap = ents->att_cap ? ents->att_cap * 2 : 8;
            KsEntAttach *grown = realloc(ents->att, cap * sizeof(*grown));
            if (!grown) return NULL;
            ents->att = grown;
            ents->att_cap = cap;
        }
        a = &ents->att[ents->att_count++];
    }
    *a = *rec;
    return a;
}

static void att_delete(KsGame *kg, const KsEntAttach *a)
{
    KsEntities *ents = &kg->ents;
    const size_t i = (size_t) (a - ents->att);
    if (i >= ents->att_count) return;
    memmove(&ents->att[i], &ents->att[i + 1], (ents->att_count - i - 1) * sizeof(*ents->att));
    ents->att_count--;
}

// pinPlayer (:213605): the player at the entity's pose (or `pose`, a client's renderPose) plus the rotated
// offset o, no velocity, on the ground, the grapple let go
static void pin_player(KsGame *kg, Player *p, const KsEntity *e, const KsEntPose *pose)
{
    const KsEntAttach *a = att_find(kg, p->uid);
    const double o0 = a ? a->o[0] : 0.0, o1 = a ? a->o[1] : 0.0, o2 = a ? a->o[2] : 0.0;
    const double x = pose ? pose->x : e->x, y = pose ? pose->y : e->y, z = pose ? pose->z : e->z, yaw = pose ? pose->yaw : e->yaw;
    const double s = sin(yaw), c = cos(yaw);
    p->position.x = (float) (x + o0 * c + o2 * s);
    p->position.y = (float) (y + o1);
    p->position.z = (float) (z - o0 * s + o2 * c);
    p->velocity = (vec3) {0.0f, 0.0f, 0.0f};
    p->on_ground = 1;
    p->grapple.active = 0;
}

// detachPlayer (:213516): only the attachment to `eid`; a controlled entity is released at the manager's
// tick. The server broadcasts "ea-" and queues onEntityDetach.
static int att_detach(KsGame *kg, const int eid, const int sid)
{
    KsEntAttach *a = att_find(kg, sid);
    if (!a || a->eid != eid) return 0;
    att_delete(kg, a);
    KsEntity *e = ent_find(kg, eid);
    if (e && e->controller == sid) {
        e->controller = -1;
        e->tick = (int) kg->ents.tick;
    }
    if (server_side(kg)) {
        net_json(kg, NET_TARGET_ALL, NET_EXT_ENTITY_DETACH, pair_record(sid, eid));
        KsEntHook *h = queue_push(kg);
        if (h) {
            h->kind = KS_ENT_HOOK_DETACH;
            h->eid = eid;
            player_id_text(player_by_sid(kg, sid), sid, h->doer, sizeof(h->doer));
        }
    }
    return 1;
}

typedef struct {               // attachPlayer's opts after sanitize
    int has_o;
    double o[3];               // opts.o (an array)
    double ox, oy, oz, by;
    int lk, ctl, soft, nd;
} AttachOpts;

// attachPlayer (:213435), the server's
static int att_attach(KsGame *kg, KsEntity *e, Player *p, const AttachOpts *opts)
{
    if (!server_side(kg) || !e || !p) return 0;
    const int sid = p->uid;
    const int soft = !!opts->soft, ctl = !soft && !!opts->ctl;
    if (ctl && e->controller >= 0 && e->controller != sid) return 0;
    const KsEntAttach *old = att_find(kg, sid);
    if (old && old->eid != e->eid) att_detach(kg, old->eid, sid);
    KsEntAttach rec = {.sid = sid, .eid = e->eid, .by = opts->by, .lk = opts->lk ? 1 : 0, .ctl = ctl, .soft = soft, .nd = opts->nd ? 1 : 0};
    if (opts->has_o) memcpy(rec.o, opts->o, sizeof(rec.o));
    else {
        rec.o[0] = opts->ox;
        rec.o[1] = opts->oy;
        rec.o[2] = opts->oz;
    }
    const KsEntAttach *a = att_set(kg, &rec);
    if (!a) return 0;
    if (soft) {
        if (e->controller == sid) {
            e->controller = -1;
            e->tick = (int) kg->ents.tick;
        }
    } else {
        if (ctl) e->controller = sid;
        pin_player(kg, p, e, NULL);
    }
    net_json(kg, NET_TARGET_ALL, NET_EXT_ENTITY_ATTACH, attach_record(a));
    KsEntHook *h = queue_push(kg);
    if (h) {
        h->kind = KS_ENT_HOOK_ATTACH;
        h->eid = e->eid;
        player_id_text(p, sid, h->doer, sizeof(h->doer));
    }
    return 1;
}

// ---------------------------------------------------------------------------
// Despawn, the body's motion on the authority (carrySoft, collideWorld, the separation)

// despawn (:212767): its riders are detached, the entity leaves; the server broadcasts "e-" [eid, reason]
static void ent_despawn(KsGame *kg, KsEntity *e, const char *reason)
{
    const int eid = e->eid;
    for (;;) {
        int sid = -1;
        for (size_t i = 0; i < kg->ents.att_count && sid < 0; i++)
            if (kg->ents.att[i].eid == eid) sid = kg->ents.att[i].sid;
        if (sid < 0 || !att_detach(kg, eid, sid)) break;
    }
    e = ent_find(kg, eid);
    if (!e) return;
    remove_body(kg, e);
    unlink_entity(kg, e);
    e->active = 0;
    ent_free(e);
    if (server_side(kg)) {
        cJSON *rec = cJSON_CreateArray();
        if (rec) {
            cJSON_AddItemToArray(rec, cJSON_CreateNumber(eid));
            cJSON_AddItemToArray(rec, cJSON_CreateString(reason ? reason : ""));
        }
        net_json(kg, NET_TARGET_ALL, NET_EXT_ENTITY_REMOVE, rec);
    }
}

// carrySoft (:213342), the server's: this step's motion is kept (carryStep) and the last one carries the soft
// riders, turned round the entity's old origin by its yaw change. KN moves the rider at once (v10 adds it to
// movX / movY / movZ, which pinAllServer applies right after the advance).
static void carry_soft(KsGame *kg, KsEntity *e, const double ox, const double oy, const double oz, const double oyaw)
{
    if (!server_side(kg)) return;
    double dyaw = fmod(e->yaw - oyaw, M_PI * 2.0);
    if (dyaw > M_PI) dyaw -= M_PI * 2.0;
    else if (dyaw < -M_PI) dyaw += M_PI * 2.0;
    const int had = e->has_carry_step;
    const double pox = e->carry_ox, poz = e->carry_oz, pdx = e->carry_dx, pdy = e->carry_dy, pdz = e->carry_dz, pdyaw = e->carry_dyaw;
    e->has_carry_step = 1;
    e->carry_ox = ox;
    e->carry_oz = oz;
    e->carry_dx = e->x - ox;
    e->carry_dy = e->y - oy;
    e->carry_dz = e->z - oz;
    e->carry_dyaw = dyaw;
    if (!had || !(pdx || pdy || pdz || pdyaw) || !kg->ents.att_count) return;
    const double s = sin(pdyaw), c = cos(pdyaw);
    for (size_t i = 0; i < kg->ents.att_count; i++) {
        const KsEntAttach *a = &kg->ents.att[i];
        if (!a->soft || a->eid != e->eid) continue;
        Player *p = player_by_sid(kg, a->sid);
        if (!p || !p->active) continue;
        const double rx = p->position.x - pox, rz = p->position.z - poz;
        p->position.x += (float) (pdx + (rx * c + rz * s - rx));
        p->position.y += (float) pdy;
        p->position.z += (float) (pdz + (-rx * s + rz * c - rz));
    }
}

// collideWorld (:214292): a solid (coll 1) entity is pushed out of the static map objects it overlaps, its
// parts at their part poses, up to three passes; objects it can step on (top within 3.5 of its base) do not push
static void displace_entity(KsGame *kg, KsEntity *e, double dx, double dz, int broadcast);

static void collide_world(KsGame *kg, KsEntity *e)
{
    const MapArchetype *ar = e->arch;
    const Map *map = kg->game ? kg->game->map : NULL;
    if (ar->coll != 1 || !map || !map->objects) return;
    const double s = sin(e->yaw), c = cos(e->yaw), as = fabs(s), ac = fabs(c);
    const double fhx = ar->foot_hx * ac + ar->foot_hz * as, fhz = ar->foot_hx * as + ar->foot_hz * ac;
    const double fcx = e->x + ar->foot_cx * c + ar->foot_cz * s, fcz = e->z - ar->foot_cx * s + ar->foot_cz * c;
    double px = 0.0, pz = 0.0;
    const double top_e = e->y + ar->foot_h;
    for (int pass = 0; pass < 3; pass++) {
        int pushed = 0;
        for (size_t i = 0; i < map->object_count; i++) {
            const Object *o = map->objects[i];
            if (!o || !o->active || o->dyn || o->no_player_col || o->trigger || o->collision_type == COLLISION_TYPE_NONE) continue;
            const vec3 sz = object_box_size(o);
            const double ow = sz.x * 0.5, ol = sz.z * 0.5, ob = o->position.y, ot = o->position.y + sz.y;
            if (ow + fhx - fabs(fcx + px - o->position.x) <= 0.0 || ol + fhz - fabs(fcz + pz - o->position.z) <= 0.0) continue;
            if (e->y >= ot || ob >= top_e || ot - e->y <= 3.5) continue;
            for (int k = 0; k < e->part_count; k++) {
                const KsEntPart *part = &e->parts[k];
                const KsEntPose pp = part_pose(e, part);
                const double pb = e->y + pp.y, pt = pb + part->size[1];
                if (pb >= ot || ob >= pt || ot - pb <= 3.5) continue;
                const double hw = (part->size[0] * ac + part->size[2] * as) / 2.0, hl = (part->size[0] * as + part->size[2] * ac) / 2.0;
                const double cx = e->x + px + pp.x * c + pp.z * s, cz = e->z + pz - pp.x * s + pp.z * c;
                const double ox = ow + hw - fabs(cx - o->position.x);
                if (ox <= 0.0) continue;
                const double oz = ol + hl - fabs(cz - o->position.z);
                if (oz <= 0.0) continue;
                if (ox < oz) px += cx >= o->position.x ? ox : -ox;
                else pz += cz >= o->position.z ? oz : -oz;
                pushed = 1;
            }
        }
        if (!pushed) break;
    }
    if (px || pz) displace_entity(kg, e, px, pz, 0);
}

// displaceEntity (:214363): state.x / state.z moved (both finite), the pose committed, soft riders carried; the
// server broadcasts a baseline for a push over 0.05 of an uncontrolled entity when asked
static void displace_entity(KsGame *kg, KsEntity *e, const double dx, const double dz, const int broadcast)
{
    double sx, sz;
    if ((!dx && !dz) || !state_finite(e->state, "x", &sx) || !state_finite(e->state, "z", &sz)) return;
    state_set_number(e->state, "x", sx + dx);
    state_set_number(e->state, "z", sz + dz);
    const double ox = e->x, oz = e->z;
    commit_pose(e);
    place_body(kg, e, e->x - ox, 0.0, e->z - oz);
    carry_soft(kg, e, ox, e->y, oz, e->yaw);
    if (e->controller >= 0) e->ctl_dirty = 1;
    if (broadcast && server_side(kg) && e->controller < 0 && fabs(dx) + fabs(dz) > 0.05) broadcast_baseline(kg, e);
}

// pairMTV (:214231): the separating axis of two entities' footprints (their own and the other's x / z axes),
// null when apart or one is above the other
static int pair_mtv(const KsEntity *a, const KsEntity *b, double *mx, double *mz, double *depth)
{
    const MapArchetype *aa = a->arch, *ba = b->arch;
    const double m[8] = {cos(a->yaw), -sin(a->yaw), sin(a->yaw), cos(a->yaw), cos(b->yaw), -sin(b->yaw), sin(b->yaw), cos(b->yaw)};
    const double acx = a->x + aa->foot_cx * m[0] + aa->foot_cz * m[2], acz = a->z + aa->foot_cx * m[1] + aa->foot_cz * m[3];
    const double dx = b->x + ba->foot_cx * m[4] + ba->foot_cz * m[6] - acx, dz = b->z + ba->foot_cx * m[5] + ba->foot_cz * m[7] - acz;
    double best = INFINITY, bx = 0.0, bz = 0.0;
    for (int k = 0; k < 4; k++) {
        const double nx = m[k * 2], nz = m[k * 2 + 1];
        const double over = aa->foot_hx * fabs(nx * m[0] + nz * m[1]) + aa->foot_hz * fabs(nx * m[2] + nz * m[3]) +
                            (ba->foot_hx * fabs(nx * m[4] + nz * m[5]) + ba->foot_hz * fabs(nx * m[6] + nz * m[7])) - fabs(dx * nx + dz * nz);
        if (over <= 0.0) return 0;
        if (over < best) {
            best = over;
            const double sign = dx * nx + dz * nz >= 0.0 ? 1.0 : -1.0;
            bx = nx * sign;
            bz = nz * sign;
        }
    }
    if (a->y >= b->y + ba->foot_h || b->y >= a->y + aa->foot_h) return 0;
    *mx = bx;
    *mz = bz;
    *depth = best;
    return 1;
}

static int solid_free(const KsEntity *e) { return e && e->active && !e->frozen && e->arch->coll == 1; }

static int eid_cmp(const void *a, const void *b)
{
    const KsEntity *x = *(KsEntity *const *) a, *y = *(KsEntity *const *) b;
    return (x->eid > y->eid) - (x->eid < y->eid);
}

// separateAll (:214162): every pair of solid entities, by eid; two free ones share the push, a controlled one
// pushes the other (the server only), two controlled ones pass
static void separate_all(KsGame *kg)
{
    size_t n = 0;
    for (size_t i = 0; i < kg->ents.count; i++) n += solid_free(kg->ents.items[i]);
    if (n < 2) return;
    int *ids = malloc(n * sizeof(*ids));
    KsEntity **list = malloc(n * sizeof(*list));
    if (!ids || !list) {
        free(ids);
        free(list);
        return;
    }
    n = 0;
    for (size_t i = 0; i < kg->ents.count; i++)
        if (solid_free(kg->ents.items[i])) list[n++] = kg->ents.items[i];
    qsort(list, n, sizeof(*list), eid_cmp);
    for (size_t i = 0; i < n; i++) ids[i] = list[i]->eid;
    for (size_t i = 0; i < n; i++) {
        for (size_t j = i + 1; j < n; j++) {
            KsEntity *a = ent_find(kg, ids[i]), *b = ent_find(kg, ids[j]);
            if (!a || !b) continue;
            const int ac = a->controller >= 0, bc = b->controller >= 0;
            if (ac && bc) continue;
            double mx, mz, depth;
            if (ac || bc) {
                if (!server_side(kg) || !pair_mtv(a, b, &mx, &mz, &depth)) continue;
                KsEntity *pushed = ac ? b : a;
                const double sign = pushed == b ? 1.0 : -1.0;
                displace_entity(kg, pushed, sign * mx * (depth + 0.001), sign * mz * (depth + 0.001), 1);
            } else if (pair_mtv(a, b, &mx, &mz, &depth)) { // resolvePair (:214282)
                const double half = depth / 2.0 + 0.001;
                displace_entity(kg, a, -mx * half, -mz * half, 0);
                displace_entity(kg, b, mx * half, mz * half, 0);
            }
        }
    }
    free(ids);
    free(list);
}

// separateEntity (:214208): a controlled step pushes its own entity out of the other solid ones
static void separate_entity(KsGame *kg, KsEntity *e)
{
    if (!solid_free(e)) return;
    for (size_t i = 0; i < kg->ents.count; i++) {
        KsEntity *o = kg->ents.items[i];
        double mx, mz, depth;
        if (o == e || !solid_free(o) || !pair_mtv(e, o, &mx, &mz, &depth)) continue;
        displace_entity(kg, e, -mx * (depth + 0.001), -mz * (depth + 0.001), 0);
    }
}

// pushSimPose (:213392), a client's: the pose of this tick into simBuf (14 kept); a later tick only
static void push_sim_pose(KsGame *kg, KsEntity *e)
{
    if (server_side(kg) || e->controller >= 0 || !e->active) return;
    const double t = e->tick * KS_ENT_STEP_MS;
    KsEntPose *last = e->sim_count ? &e->sim_buf[e->sim_count - 1] : NULL;
    if (last && last->t > t) return;
    KsEntPose *slot = last && last->t == t ? last : NULL;
    if (!slot) {
        if (e->sim_count == KS_ENT_SIM_BUF) {
            memmove(&e->sim_buf[0], &e->sim_buf[1], (KS_ENT_SIM_BUF - 1) * sizeof(e->sim_buf[0]));
            e->sim_count--;
        }
        slot = &e->sim_buf[e->sim_count++];
    }
    *slot = (KsEntPose) {t, e->x, e->y, e->z, e->yaw, e->pitch, e->roll};
}

// ---------------------------------------------------------------------------
// Spawn, step, damage (spawn :212676, stepEntity :213425, commitStep :213305, damage :214530)

static void bad_result(KsEntity *e)
{
    if (++e->errors >= KS_ENT_ERRORS) e->frozen = 1;
}

static KsEntity *ent_new(const MapArchetype *arch, const int eid, const char *tag)
{
    KsEntity *e = calloc(1, sizeof(*e));
    if (!e) return NULL;
    e->eid = eid;
    e->arch = arch;
    e->hp = arch->hp;
    e->active = 1;
    e->controller = -1;
    snprintf(e->mode, sizeof(e->mode), "sim");
    snprintf(e->tag, sizeof(e->tag), "%s", tag ? tag : "");
    build_parts(e);
    return e;
}

// Creates an entity of `arch` at (x, y, z) with the spawn params and tag. `eid` 0 takes the next id.
// NULL when refused: not the authority or no shared script, 64 entities, the id taken, or the
// onEntityInit state invalid (logged). A missing or failing onEntityInit leaves an empty state
// (v10: `{}` plus the spawn position).
static KsEntity *ent_spawn(KsGame *kg, const MapArchetype *arch, int eid, double x, double y, double z, const cJSON *params, const char *tag)
{
    KsEntities *ents = &kg->ents;
    if (!server_side(kg) || !has_hooks(kg) || !arch) return NULL;
    if (ents->count >= MAP_ENTITY_LIMIT) return NULL;
    if (eid <= 0) eid = ents->next_eid++;
    if (ent_find(kg, eid)) return NULL;
    if (!isfinite(x)) x = 0.0;
    if (!isfinite(y)) y = 0.0;
    if (!isfinite(z)) z = 0.0;

    KsEntity *e = ent_new(arch, eid, tag);
    if (!e) return NULL;
    e->tick = (int) ents->tick;
    e->step_tick = (int) ents->tick;
    e->params = params ? cJSON_Duplicate(params, 1) : cJSON_CreateObject();
    if (!e->params) {
        ent_free(e);
        return NULL;
    }

    KsScript *s = kg->shared_script;
    char eid_text[24];
    snprintf(eid_text, sizeof(eid_text), "%d", eid);
    KsValue spawn = value_from_json(s, e->params);
    KsValue args[3] = {ks_str(s, eid_text, strlen(eid_text)), ks_str(s, arch->name, strlen(arch->name)), spawn};
    for (int i = 0; i < 3; i++) ks_handle_pin(s, args[i]);
    KsValue result = ks_undef();
    const int rc = ks_script_has_hook(s, "onEntityInit") ? ks_script_call_hook_value(s, "onEntityInit", args, 3, &result) : -1;
    for (int i = 2; i >= 0; i--) ks_handle_unpin(s, args[i]);

    char err[160];
    if (rc != 0 || (result.tag != KS_OBJ && result.tag != KS_ARR)) result = ks_obj_new(s); // v10: a non-object is {}
    if (result.tag == KS_ARR) { // an array is an object to typeof, and fails the state check
        char line[256];
        snprintf(line, sizeof(line), "onEntityInit(%s): state must be an object", arch->name);
        ent_log(kg, line);
        ent_free(e);
        return NULL;
    }
    // v10 fills the spawn position where the state has none (`null == state.x`), before the check
    const char *pos_keys[3] = {"x", "y", "z"};
    const double pos[3] = {x, y, z};
    for (int i = 0; i < 3; i++) {
        const KsValue cur = ks_obj_get(s, result, pos_keys[i]);
        if (cur.tag == KS_UNDEF || cur.tag == KS_NULL) ks_obj_set(s, result, pos_keys[i], ks_num(pos[i]));
    }
    cJSON *state = NULL;
    if (state_from_value(s, result, &state, err, sizeof(err))) {
        char line[320];
        snprintf(line, sizeof(line), "onEntityInit(%s): %s", arch->name, err);
        ent_log(kg, line);
        ent_free(e);
        return NULL;
    }
    e->state = state;
    commit_pose(e);
    if (ent_find(kg, eid) || !insert_entity(kg, e)) { // onEntityInit spawned this id itself
        ent_free(e);
        return NULL;
    }
    build_body(kg, e);
    net_json(kg, NET_TARGET_ALL, NET_EXT_ENTITY_SPAWN, spawn_payload(e));
    KsEntHook *h = queue_push(kg);
    if (h) {
        h->kind = KS_ENT_HOOK_SPAWN;
        h->eid = eid;
        snprintf(h->text, sizeof(h->text), "%s", arch->name);
    }
    return e;
}

// commitStep (:213305): a non-object result, or one the state check refuses (logged), is an error and ten
// in a row freeze the entity; a good one is the new state, its pose committed, the world and the soft riders
// moved with it.
static void commit_step(KsGame *kg, KsEntity *e, KsScript *s, const int rc, const KsValue result, const int tick)
{
    if (rc != 0 || result.tag != KS_OBJ) {
        bad_result(e);
        e->tick = tick;
        return;
    }
    char err[160];
    cJSON *next = NULL;
    if (state_from_value(s, result, &next, err, sizeof(err))) {
        char line[320];
        snprintf(line, sizeof(line), "simulateEntity(%s): %s", e->arch->name, err);
        ent_log(kg, line);
        bad_result(e);
        e->tick = tick;
        return;
    }
    e->errors = 0;
    cJSON_Delete(e->state);
    e->state = next;
    e->tick = tick;
    const double ox = e->x, oy = e->y, oz = e->z, oyaw = e->yaw;
    commit_pose(e);
    place_body(kg, e, e->x - ox, e->y - oy, e->z - oz);
    collide_world(kg, e);
    carry_soft(kg, e, ox, oy, oz, oyaw);
    if (e->controller >= 0) e->ctl_dirty = 1;
}

// The ctx inputs of the step in progress (a controlled step's, else none)
typedef struct {
    int set;
    double v[9]; // active forward strafe jump crouch shoot interact yaw pitch
} StepInputs;
static StepInputs g_step_inputs;

// One simulateEntity call to `tick` (stepEntity, :213425; stepControlled's call, :213816): prevWire, the ctx
// (tick, rand, inputs), the hook, commitStep. `ctx_tick` is ctx.tick (a controlled step's input number).
static void ent_step_with(KsGame *kg, const int eid, const long ctx_tick, const int commit_tick, const double delta_s)
{
    KsEntity *e = ent_find(kg, eid);
    if (!e) return;
    KsScript *s = kg->shared_script;
    // e->events accumulate over the steps of one advance (8 at most) and flush after them (:213914-213950)
    e->step_tick = (int) ctx_tick;
    e->rng = ent_rand_seed(kg->ents.seed, eid, ctx_tick);
    cJSON_Delete(e->prev_wire);
    e->prev_wire = state_copy(e->state);

    char eid_text[24];
    snprintf(eid_text, sizeof(eid_text), "%d", eid);
    KsRef *ref = ks_ref_get(kg, KS_REF_ENTITY, eid);
    if (!ref) return;
    KsValue args[5] = {ks_str(s, eid_text, strlen(eid_text)), ks_str(s, e->arch->name, strlen(e->arch->name)), state_value(s, e->state),
                       ks_host_object(s, &ks_entity_ctx_class, ref, (uint32_t) eid), ks_num(delta_s)};
    for (int i = 0; i < 5; i++) ks_handle_pin(s, args[i]);
    KsValue result = ks_undef();
    const int rc = ks_script_call_hook_value(s, "simulateEntity", args, 5, &result);
    for (int i = 4; i >= 0; i--) ks_handle_unpin(s, args[i]);

    e = ent_find(kg, eid); // the hook may have despawned it
    if (!e) return;
    commit_step(kg, e, s, rc, result, commit_tick);
}

static void ent_step(KsGame *kg, const int eid, const long tick)
{
    g_step_inputs.set = 0;
    ent_step_with(kg, eid, tick, (int) tick, KS_ENT_STEP_MS / 1000.0);
}

// The authority's damage (:214530): refused for a rider with nd; the hp falls, onEntityDamage is queued and
// "_damage" broadcast; at hp <= 0 onEntityDestroyed is queued and the entity despawns ("destroyed"), else a
// baseline goes out. Returns 1 when it was destroyed.
static int ent_damage(KsGame *kg, const int eid, const double amount, const char *by)
{
    KsEntity *e = ent_find(kg, eid);
    if (!server_side(kg) || !e || !(e->arch->hp != 0.0f)) return 0;
    Player *doer = by && by[0] ? ks_game_find_player_str(kg->game, by) : NULL;
    if (doer) {
        const KsEntAttach *a = att_find(kg, doer->uid);
        if (a && a->eid == eid && a->nd) return 0;
    }
    char doer_id[24];
    if (doer) ks_player_id(doer, doer_id, sizeof(doer_id));
    else snprintf(doer_id, sizeof(doer_id), "%s", by ? by : "");
    e->hp -= amount;
    KsEntHook *h = queue_push(kg);
    if (h) {
        h->kind = KS_ENT_HOOK_DAMAGE;
        h->eid = eid;
        snprintf(h->doer, sizeof(h->doer), "%s", doer_id);
        h->a = amount;
    }
    net_json(kg, NET_TARGET_ALL, NET_EXT_ENTITY_EVENT, event_record(eid, "_damage", amount, e->hp));
    if (e->hp > 0.0) {
        broadcast_baseline(kg, e);
        return 0;
    }
    h = queue_push(kg);
    if (h) {
        h->kind = KS_ENT_HOOK_DESTROYED;
        h->eid = eid;
        snprintf(h->doer, sizeof(h->doer), "%s", doer_id);
    }
    ent_despawn(kg, e, "destroyed");
    return 1;
}

// applyStateWrite (:214473): a candidate state that stays valid is committed (the soft riders carried, a
// baseline broadcast); a refused one is logged and undone. `items` are owned by the call.
static int state_write(KsGame *kg, KsEntity *e, cJSON **items, const char **keys, const int count)
{
    cJSON *cand = cJSON_Duplicate(e->state, 1);
    char err[160];
    int ok = cand != NULL;
    for (int i = 0; ok && i < count; i++) {
        if (!items[i]) continue;
        if (!cJSON_ReplaceItemInObjectCaseSensitive(cand, keys[i], items[i])) cJSON_AddItemToObject(cand, keys[i], items[i]);
        items[i] = NULL;
    }
    for (int i = 0; i < count; i++) cJSON_Delete(items[i]); // (the ones not taken)
    if (ok && state_check(cand, err, sizeof(err))) {
        char line[320];
        snprintf(line, sizeof(line), "setState(%s): %s", e->arch->name, err);
        ent_log(kg, line);
        ok = 0;
    }
    if (!ok) {
        cJSON_Delete(cand);
        return 0;
    }
    cJSON_Delete(e->state);
    e->state = cand;
    const double ox = e->x, oy = e->y, oz = e->z, oyaw = e->yaw;
    commit_pose(e);
    place_body(kg, e, e->x - ox, e->y - oy, e->z - oz);
    carry_soft(kg, e, ox, oy, oz, oyaw);
    broadcast_baseline(kg, e);
    return 1;
}

// setVar(key, num | bool | str) on the authority.
static int entity_set_var(KsGame *kg, KsEntity *e, const char *key, cJSON *value)
{
    if (!authority(kg) || !value) {
        cJSON_Delete(value);
        return 0;
    }
    cJSON *items[1] = {value};
    const char *keys[1] = {key};
    return state_write(kg, e, items, keys, 1);
}

// ---------------------------------------------------------------------------
// The round's clock, the advance and the hook queue

static double wall_ms(KsGame *kg) { return ks_game_now_ms(kg); }

// The events of ctx.emit after an entity's steps: the server queues onEntityEvent and broadcasts "ee", a
// client queues them for its own script (clientEventQueue)
static void flush_events(KsGame *kg, const int eid)
{
    KsEntity *e = ent_find(kg, eid);
    if (!e) return;
    for (int i = 0; i < e->event_count; i++) {
        KsEntHook *h = queue_push(kg);
        if (!h) break;
        h->kind = KS_ENT_HOOK_EVENT;
        h->eid = eid;
        snprintf(h->text, sizeof(h->text), "%s", e->events[i].name);
        h->a = e->events[i].a;
        h->b = e->events[i].b;
        if (server_side(kg)) net_json(kg, NET_TARGET_ALL, NET_EXT_ENTITY_EVENT, event_record(eid, e->events[i].name, e->events[i].a, e->events[i].b));
    }
    e->event_count = 0;
}

// freezeSlowest (:213990): the active "sim" entity with the longest last advance freezes, logged, its baseline sent
static void freeze_slowest(KsGame *kg)
{
    KsEntity *slow = NULL;
    for (size_t i = 0; i < kg->ents.count; i++) {
        KsEntity *e = kg->ents.items[i];
        if (e->active && !e->frozen && !strcmp(e->mode, "sim") && (!slow || e->last_sim_ms >= slow->last_sim_ms)) slow = e;
    }
    if (!slow) return;
    slow->frozen = 1;
    char line[160];
    snprintf(line, sizeof(line), "entity %d (%s) frozen: over sim budget", slow->eid, slow->arch->name);
    ent_log(kg, line);
    broadcast_baseline(kg, slow);
}

// advance (:213891): tick = floor(clock / 33.33); each active, unfrozen, uncontrolled "sim" entity steps up
// to it (a stall of more than 64 ticks restarts at one step); then the separation, a client's sim poses and
// the 2 ms budget (30 slow advances in a row freeze the slowest entity, on the server). Ids are snapshotted:
// a step may spawn or despawn.
static void ent_advance_to(KsGame *kg, const double now_ms)
{
    KsEntities *ents = &kg->ents;
    const long tick = (long) floor(now_ms / KS_ENT_STEP_MS);
    ents->tick = tick;
    if (!has_hooks(kg) || !ents->count) return;
    const double t_start = wall_ms(kg);
    const size_t n = ents->count;
    int *ids = malloc(n * sizeof(*ids));
    if (!ids) return;
    for (size_t i = 0; i < n; i++) ids[i] = ents->items[i]->eid;
    for (size_t i = 0; i < n; i++) {
        KsEntity *e = ent_find(kg, ids[i]);
        if (!e || !e->active || e->frozen || e->controller >= 0 || strcmp(e->mode, "sim")) continue;
        const long behind = tick - e->tick;
        if (behind <= 0) continue;
        if (behind > KS_ENT_CATCHUP) e->tick = (int) (tick - 1);
        const double t0 = wall_ms(kg);
        for (;;) {
            e = ent_find(kg, ids[i]);
            if (!e || e->tick >= tick) break;
            ent_step(kg, ids[i], (long) e->tick + 1);
        }
        e = ent_find(kg, ids[i]);
        if (e) e->last_sim_ms = wall_ms(kg) - t0;
        flush_events(kg, ids[i]);
    }
    free(ids);
    separate_all(kg);
    if (!server_side(kg)) {
        for (size_t i = 0; i < ents->count; i++) {
            KsEntity *e = ents->items[i];
            if (e->active && !e->frozen && e->controller < 0 && !strcmp(e->mode, "sim")) push_sim_pose(kg, e);
        }
    }
    if (wall_ms(kg) - t_start > 2.0) {
        if (++ents->budget_streak >= 30 && server_side(kg)) {
            freeze_slowest(kg);
            ents->budget_streak = 0;
        }
    } else {
        ents->budget_streak = 0;
    }
}

// The player standing on a static map object (v10 player.onTopOf with no entityEid): grounded, its feet at
// the top of a collidable, non-dyn object it stands inside the footprint of. KN keeps no onTopOf.
static int on_static_top(const Game *game, const Player *p)
{
    if (!p->on_ground || !game->map) return 0;
    const float r = game_constants.player_scale;
    for (size_t i = 0; i < game->map->object_count; i++) {
        const Object *o = game->map->objects[i];
        if (!o || !o->active || o->dyn || o->collision_type == COLLISION_TYPE_NONE) continue;
        const vec3 s = object_box_size(o);
        if (fabsf(p->position.y - (o->position.y + s.y)) < 0.05f && fabsf(p->position.x - o->position.x) <= s.x * 0.5f + r &&
            fabsf(p->position.z - o->position.z) <= s.z * 0.5f + r)
            return 1;
    }
    return 0;
}

// updateSoftZones (:213718): a free player inside a soft zone of an entity (in its yawed frame) rides it soft;
// a soft rider leaves when outside the zone grown by 1.5, or on a static top
static void update_soft_zones(KsGame *kg)
{
    Game *game = kg->game;
    for (size_t i = 0; i < kg->ents.count; i++) {
        const int eid = kg->ents.items[i]->eid;
        KsEntity *e = kg->ents.items[i];
        if (!e->active || e->frozen || !e->arch->soft_zone_count) continue;
        const double s = sin(e->yaw), c = cos(e->yaw);
        for (size_t k = 0; k < game->player_count; k++) {
            Player *p = game->players[k];
            e = ent_find(kg, eid);
            if (!e) break;
            if (!p || !p->active) continue;
            const KsEntAttach *a = att_find(kg, p->uid);
            const int soft_here = a && a->eid == eid && a->soft;
            if (a && !soft_here) continue;
            const double dx = p->position.x - e->x, dz = p->position.z - e->z;
            const double lx = dx * c - dz * s, lz = dx * s + dz * c, ly = p->position.y - e->y;
            const double grow = soft_here ? 1.5 : 1.0;
            int inside = 0;
            for (int z = 0; z < e->arch->soft_zone_count && !inside; z++) {
                const MapArchetypeSoftZone *sz = &e->arch->soft_zones[z];
                inside = fabs(lx - sz->centre.x) <= sz->half.x * grow && fabs(ly - sz->centre.y) <= sz->half.y * grow &&
                         fabs(lz - sz->centre.z) <= sz->half.z * grow;
            }
            const int static_top = inside && on_static_top(game, p);
            if (!soft_here || (inside && !static_top)) {
                if (!a && inside && !static_top) {
                    const AttachOpts opts = {.soft = 1};
                    att_attach(kg, e, p, &opts);
                }
            } else {
                att_detach(kg, eid, p->uid);
            }
        }
    }
}

// pinAllServer (:213665): the soft zones, then every hard rider pinned to its entity; a rider that died is
// detached (soft riders' carry already moved them, carry_soft)
static void pin_all_server(KsGame *kg)
{
    if (!server_side(kg) || !kg->game) return;
    update_soft_zones(kg);
    for (size_t i = 0; i < kg->ents.att_count;) {
        const KsEntAttach a = kg->ents.att[i];
        KsEntity *e = ent_find(kg, a.eid);
        Player *p = player_by_sid(kg, a.sid);
        if (e && p && p->active) {
            if (!a.soft) pin_player(kg, p, e, NULL);
        } else if (e && p && !p->active && att_detach(kg, a.eid, a.sid)) {
            continue; // the entry left the list
        }
        i++;
    }
}

// broadcastBaselines (:214389): two entities' baselines per update, round robin
static void broadcast_baselines(KsGame *kg)
{
    KsEntities *ents = &kg->ents;
    if (!server_side(kg) || !ents->count) return;
    for (size_t i = 0; i < 2 && i < ents->count; i++) {
        const KsEntity *e = ents->items[ents->baseline_cursor++ % ents->count];
        net_json(kg, NET_TARGET_ALL, NET_EXT_ENTITY_STATE, state_record(e));
    }
    ents->baseline_cursor %= ents->count;
}

// broadcastControlled (:213824): the controlled entities a step moved since the last one, as baselines
static void broadcast_controlled(KsGame *kg)
{
    if (!server_side(kg)) return;
    for (size_t i = 0; i < kg->ents.count; i++) {
        KsEntity *e = kg->ents.items[i];
        if (!e->active || e->controller < 0 || !e->ctl_dirty) continue;
        e->ctl_dirty = 0;
        net_json(kg, NET_TARGET_ALL, NET_EXT_ENTITY_STATE, state_record(e));
    }
}

// drainServerHooks (:214797): each queued hook reaches the main script when it has that hook; a client's
// drainClientEvents (:222201-222214): "_spawn" -> onEntitySpawn, "_removed" -> onEntityRemoved, an event whose
// name does not start with "_" -> onEntityEvent(id, name, a, b)
static void drain_queue(KsGame *kg)
{
    KsEntities *ents = &kg->ents;
    KsScript *main_script = ks_game_script(kg->game);
    while (ents->queue_head < ents->queue_count) {
        const KsEntHook h = ents->queue[ents->queue_head++];
        if (!main_script) continue;
        char eid_text[24], text_buf[KS_ENT_NAME_MAX + 1];
        snprintf(eid_text, sizeof(eid_text), "%d", h.eid);
        snprintf(text_buf, sizeof(text_buf), "%s", h.text);
        KsValue args[4];
        int argc = 0;
        const char *hook = NULL;
        KsScript *s = main_script;
        args[0] = ks_str(s, eid_text, strlen(eid_text));
        switch (h.kind) {
            case KS_ENT_HOOK_SPAWN:
                hook = "onEntitySpawn";
                args[1] = ks_str(s, text_buf, strlen(text_buf));
                argc = 2;
                break;
            case KS_ENT_HOOK_REMOVED:
                hook = "onEntityRemoved";
                argc = 1;
                break;
            case KS_ENT_HOOK_EVENT:
                if (!server_side(kg) && text_buf[0] == '_') continue; // "_damage" and the like stay internal
                hook = "onEntityEvent";
                args[1] = ks_str(s, text_buf, strlen(text_buf));
                args[2] = ks_num(isnan(h.a) ? 0.0 : h.a);
                args[3] = ks_num(isnan(h.b) ? 0.0 : h.b);
                argc = 4;
                break;
            case KS_ENT_HOOK_DAMAGE:
                hook = "onEntityDamage";
                args[1] = ks_str(s, h.doer, strlen(h.doer));
                args[2] = ks_num(h.a);
                argc = 3;
                break;
            case KS_ENT_HOOK_DESTROYED:
            case KS_ENT_HOOK_ATTACH:
            case KS_ENT_HOOK_DETACH:
                hook = h.kind == KS_ENT_HOOK_DESTROYED ? "onEntityDestroyed" : h.kind == KS_ENT_HOOK_ATTACH ? "onEntityAttach" : "onEntityDetach";
                args[1] = ks_str(s, h.doer, strlen(h.doer));
                argc = 2;
                break;
            default: continue;
        }
        for (int i = 0; i < argc; i++) ks_handle_pin(s, args[i]);
        ks_game_call(kg->game, hook, args, argc);
        for (int i = argc - 1; i >= 0; i--) ks_handle_unpin(s, args[i]);
    }
    ents->queue_head = ents->queue_count = 0;
}

static void live_reset(KsGame *kg);
static void live_poll(KsGame *kg);

void ks_entities_reset(KsGame *kg)
{
    if (!kg) return;
    KsEntities *ents = &kg->ents;
    for (size_t i = 0; i < ents->count; i++) ent_free(ents->items[i]);
    free(ents->items);
    ents->items = NULL;
    ents->count = ents->cap = 0;
    ents->queue_head = ents->queue_count = 0; // hooks queued by the last round are dropped (clear, :214764)
    ents->att_count = 0;
    ents->baseline_cursor = 0;
    ents->budget_streak = 0;
    ents->replaying = 0;
    ents->next_eid = KS_ENT_FIRST_EID;
    ents->clock_ms = 0.0;
    ents->tick = 0;
    live_reset(kg); // liveObjects.resetAll (:219911)
    drop_objects(kg);
}

void ks_entities_free(KsGame *kg)
{
    if (!kg) return;
    ks_entities_reset(kg);
    free(kg->ents.queue);
    kg->ents.queue = NULL;
    kg->ents.queue_cap = 0;
    free(kg->ents.att);
    kg->ents.att = NULL;
    kg->ents.att_cap = 0;
    free(kg->ents.live);
    kg->ents.live = NULL;
    kg->ents.live_cap = 0;
}

// Round start (ks_game_start): the last round's entities go (no hooks), then the map's ENTITY
// instances with `as` != 0 spawn in map order (spawnPending, :212727). The yaw of an instance is written
// when its rotation y is non-zero (`r[1] &&`), straight into the state: no baseline (v10 sets it after the
// spawn's broadcast).
void ks_entities_round_start(KsGame *kg)
{
    if (!kg) return;
    ks_entities_reset(kg);
    if (!server_side(kg) || !has_hooks(kg) || !kg->game || !kg->game->map) return;
    const Map *map = kg->game->map;
    for (size_t i = 0; i < map->entity_instance_count; i++) {
        const MapEntityInstance *inst = &map->entity_instances[i];
        if (!inst->auto_spawn) continue;
        const MapArchetype *arch = map_archetype(map, inst->archetype);
        if (!arch) continue;
        KsEntity *e = ent_spawn(kg, arch, inst->eid, inst->position.x, inst->position.y, inst->position.z, inst->spawn_params,
                                inst->has_tag ? inst->tag : NULL);
        if (!e || !inst->rotation.y) continue;
        e->yaw = inst->rotation.y;
        state_set_number(e->state, "yaw", inst->rotation.y);
        commit_pose(e);
    }
}

// game.update's server half (:222187-222199): advance, pinAllServer, the queued hooks, two baselines; the
// controlled entities' moves at the sync (:222366)
void ks_entities_update(KsGame *kg, const double delta_ms)
{
    if (!kg) return;
    if (server_side(kg)) {
        kg->ents.clock_ms += delta_ms;
        live_poll(kg); // a cpx asset whose model was still loading
        ent_advance_to(kg, kg->ents.clock_ms);
        pin_all_server(kg);
        drain_queue(kg);
        broadcast_baselines(kg);
        broadcast_controlled(kg);
        return;
    }
}

// ---------------------------------------------------------------------------
// stepControlled (:213790): the controlled entity steps with its rider's input; on the server a window of
// 1.5 s of steps per second of wall clock; emit is ignored on a client and in a replay

static void step_controlled(KsGame *kg, KsEntity *e, const Player *p, const Input *in, const int recon)
{
    if (!has_hooks(kg) || !e->active || e->frozen) return;
    const int v10_dir = in->move_dir >= 0 && in->move_dir <= 7 ? (in->move_dir + 1) % 8 : -1; // config.movDirs index
    const int moving = v10_dir != -1;
    const double angle = moving ? -M_PI + (v10_dir + 1) * M_PI / 4.0 + M_PI / 2.0 : 0.0;     // movDirs[i] + PI / 2
    StepInputs inputs = {.set = 1};
    inputs.v[0] = 1.0;
    inputs.v[1] = moving ? cos(angle) : 0.0;
    inputs.v[2] = moving ? sin(angle) : 0.0;
    inputs.v[3] = in->jump ? 1.0 : 0.0;
    inputs.v[4] = in->crouch ? 1.0 : 0.0;
    inputs.v[5] = in->shoot ? 1.0 : 0.0;
    inputs.v[6] = in->interact_held ? 1.0 : 0.0;
    inputs.v[7] = round(in->y_dir * 1000.0) / 1000.0; // input[2] / 1000: the yaw
    inputs.v[8] = round(in->x_dir * 1000.0) / 1000.0; // input[3] / 1000: the pitch
    const Game *game = kg->game;
    const GameConstants *k = game_constants_of(game);
    const double delta_mlt = game && game->config.delta_mlt > 0.0f ? game->config.delta_mlt : 1.0;
    const double dt = fmax(1.0, fmin(round(in->delta * 1000.0), round(k->max_delta * 1000.0))) / 1000.0 / delta_mlt; // dltMx 33
    if (server_side(kg) && !recon) {
        const double now = wall_ms(kg);
        if (!e->has_ctl_window || now - e->ctl_t0 > 1000.0) {
            e->has_ctl_window = 1;
            e->ctl_t0 = now;
            e->ctl_acc = 0.0;
        }
        if (e->ctl_acc + dt > 1.5) return;
        e->ctl_acc += dt;
    }
    (void) p;
    const int was = kg->ents.replaying;
    kg->ents.replaying = recon || !server_side(kg);
    g_step_inputs = inputs;
    ent_step_with(kg, e->eid, in->seq, (int) kg->ents.tick, dt);
    g_step_inputs.set = 0;
    kg->ents.replaying = was;
    e = ent_find(kg, e->eid);
    if (e) separate_entity(kg, e);
}

// ---------------------------------------------------------------------------
// A client's copy (applySpawn, applyRemove, applyBaselines, applyAttach, applyDetach, applyControlledSync)

static KsGame *client_kg(Game *game)
{
    KsGame *kg = game ? ks_game_state(game, 1) : NULL;
    return kg && !server_side(kg) ? kg : NULL;
}

static double rec_num(const cJSON *rec, const int i, const double def)
{
    const cJSON *v = cJSON_GetArrayItem(rec, i);
    return cJSON_IsNumber(v) ? v->valuedouble : def;
}

static void client_event(KsGame *kg, const int kind, const int eid, const char *text, const double a, const double b)
{
    KsEntHook *h = queue_push(kg);
    if (!h) return;
    h->kind = kind;
    h->eid = eid;
    snprintf(h->text, sizeof(h->text), "%s", text ? text : "");
    h->a = a;
    h->b = b;
}

void ks_entities_net_seed(Game *game, const cJSON *rec)
{
    KsGame *kg = client_kg(game);
    if (!kg) return;
    const cJSON *v = cJSON_IsArray(rec) ? cJSON_GetArrayItem(rec, 0) : rec;
    if (cJSON_IsNumber(v)) kg->ents.seed = (uint32_t) (int32_t) (int64_t) v->valuedouble; // matchSeed | 0
}

void ks_entities_net_spawn(Game *game, const cJSON *rec)
{
    KsGame *kg = client_kg(game);
    if (!kg || !cJSON_IsArray(rec) || !game->map) return;
    const int eid = (int) rec_num(rec, 0, -1);
    KsEntity *old = ent_find(kg, eid);
    if (old) ent_despawn(kg, old, NULL);
    const MapArchetype *arch = map_archetype(game->map, (int) rec_num(rec, 1, -1));
    if (!arch || kg->ents.count >= MAP_ENTITY_LIMIT * 4) return;
    KsEntity *e = ent_new(arch, eid, cJSON_GetStringValue(cJSON_GetArrayItem(rec, 4)));
    if (!e) return;
    e->tick = (int) rec_num(rec, 2, 0);
    const char *mode = cJSON_GetStringValue(cJSON_GetArrayItem(rec, 5));
    snprintf(e->mode, sizeof(e->mode), "%s", mode && mode[0] ? mode : "sim");
    e->hp = rec_num(rec, 6, 0);
    const cJSON *state = cJSON_GetArrayItem(rec, 3);
    e->state = cJSON_IsObject(state) ? cJSON_Duplicate(state, 1) : cJSON_CreateObject();
    e->params = cJSON_CreateObject();
    commit_pose(e);
    if (!e->state || !insert_entity(kg, e)) {
        ent_free(e);
        return;
    }
    build_body(kg, e);
    client_event(kg, KS_ENT_HOOK_SPAWN, eid, arch->name, 0.0, 0.0);
}

void ks_entities_net_remove(Game *game, const cJSON *rec)
{
    KsGame *kg = client_kg(game);
    if (!kg || !cJSON_IsArray(rec)) return;
    const int eid = (int) rec_num(rec, 0, -1);
    KsEntity *e = ent_find(kg, eid);
    if (e) ent_despawn(kg, e, NULL);
    client_event(kg, KS_ENT_HOOK_REMOVED, eid, cJSON_GetStringValue(cJSON_GetArrayItem(rec, 1)), 0.0, 0.0);
}

void ks_entities_net_baseline(Game *game, const cJSON *rec, const double local_ms)
{
    KsGame *kg = client_kg(game);
    if (!kg || !cJSON_IsArray(rec)) return;
    const int eid = (int) rec_num(rec, 0, -1);
    KsEntity *e = ent_find(kg, eid);
    if (!e || !e->active || e->controlled_by_local) return;
    const int old_tick = e->tick, tick = (int) rec_num(rec, 1, 0);
    const double ox = e->x, oy = e->y, oz = e->z;
    const cJSON *state = cJSON_GetArrayItem(rec, 2);
    cJSON *next = cJSON_IsObject(state) ? cJSON_Duplicate(state, 1) : cJSON_CreateObject();
    if (!next) return;
    cJSON_Delete(e->state);
    e->state = next;
    e->tick = tick;
    commit_pose(e);
    // the ticks this client already ran are run again on the server's state (no events)
    if (!strcmp(e->mode, "sim") && e->controller < 0 && has_hooks(kg) && tick < old_tick && old_tick - tick <= KS_ENT_CATCHUP) {
        kg->ents.replaying = 1;
        for (;;) {
            e = ent_find(kg, eid);
            if (!e || e->tick >= old_tick) break;
            ent_step(kg, eid, (long) e->tick + 1);
        }
        kg->ents.replaying = 0;
        e = ent_find(kg, eid);
        if (!e) return;
    }
    if (e->controller < 0) {
        const double dx = e->x - ox, dy = e->y - oy, dz = e->z - oz;
        if (sqrt(dx * dx + dy * dy + dz * dz) > 0.01) {
            e->blend[0] = -dx;
            e->blend[1] = -dy;
            e->blend[2] = -dz;
            e->blend_left = KS_ENT_BLEND_MS;
        }
        place_body(kg, e, 0.0, 0.0, 0.0);
        push_sim_pose(kg, e);
    } else { // another player's controlled entity: its poses are interpolated (getRenderPose)
        if (e->pose_count == KS_ENT_POSE_BUF) {
            memmove(&e->pose_buf[0], &e->pose_buf[1], (KS_ENT_POSE_BUF - 1) * sizeof(e->pose_buf[0]));
            e->pose_count--;
        }
        e->pose_buf[e->pose_count++] = (KsEntPose) {local_ms, e->x, e->y, e->z, e->yaw, e->pitch, e->roll};
        place_body(kg, e, 0.0, 0.0, 0.0);
    }
}

void ks_entities_net_controlled(Game *game, const cJSON *rec)
{
    KsGame *kg = client_kg(game);
    if (!kg || !cJSON_IsArray(rec)) return;
    KsEntity *e = ent_find(kg, (int) rec_num(rec, 0, -1));
    if (!e) return;
    const cJSON *state = cJSON_GetArrayItem(rec, 2);
    cJSON *next = cJSON_IsObject(state) ? cJSON_Duplicate(state, 1) : cJSON_CreateObject();
    if (!next) return;
    cJSON_Delete(e->state);
    e->state = next;
    e->tick = (int) rec_num(rec, 1, e->tick);
    commit_pose(e);
    place_body(kg, e, 0.0, 0.0, 0.0);
}

void ks_entities_net_event(Game *game, const cJSON *rec)
{
    KsGame *kg = client_kg(game);
    if (!kg || !cJSON_IsArray(rec)) return;
    const char *name = cJSON_GetStringValue(cJSON_GetArrayItem(rec, 1));
    client_event(kg, KS_ENT_HOOK_EVENT, (int) rec_num(rec, 0, -1), name ? name : "", rec_num(rec, 2, NAN), rec_num(rec, 3, NAN));
}

static int json_truthy(const cJSON *v)
{
    if (!v || cJSON_IsNull(v) || cJSON_IsFalse(v)) return 0;
    if (cJSON_IsNumber(v)) return v->valuedouble != 0 && !isnan(v->valuedouble);
    if (cJSON_IsString(v)) return v->valuestring[0] != 0;
    return 1;
}

void ks_entities_net_attach(Game *game, const cJSON *rec, const int local_sid)
{
    KsGame *kg = client_kg(game);
    if (!kg || !cJSON_IsArray(rec)) return;
    const int sid = (int) rec_num(rec, 0, -1), eid = (int) rec_num(rec, 1, -1);
    KsEntity *e = ent_find(kg, eid);
    if (!e) return;
    const cJSON *opts = cJSON_GetArrayItem(rec, 2), *o = cJSON_GetObjectItemCaseSensitive(opts, "o");
    KsEntAttach a = {.sid = sid, .eid = eid};
    for (int k = 0; k < 3; k++) { // iíììiií: parseFloat || 0 of an array, else [0, 0, 0]
        const cJSON *v = cJSON_IsArray(o) ? cJSON_GetArrayItem(o, k) : NULL;
        a.o[k] = cJSON_IsNumber(v) && isfinite(v->valuedouble) ? v->valuedouble : cJSON_IsString(v) ? strtod(v->valuestring, NULL) : 0.0;
        if (!isfinite(a.o[k])) a.o[k] = 0.0;
    }
    a.lk = json_truthy(cJSON_GetObjectItemCaseSensitive(opts, "lk"));
    a.ctl = json_truthy(cJSON_GetObjectItemCaseSensitive(opts, "ctl"));
    a.soft = json_truthy(cJSON_GetObjectItemCaseSensitive(opts, "soft"));
    a.nd = json_truthy(cJSON_GetObjectItemCaseSensitive(opts, "nd"));
    if (!att_set(kg, &a)) return;
    if (a.ctl) {
        e->controller = sid;
        e->controlled_by_local = sid == local_sid;
        e->tick = (int) kg->ents.tick;
        e->pose_count = 0;
        e->sim_count = 0;
    }
}

void ks_entities_net_detach(Game *game, const cJSON *rec)
{
    KsGame *kg = client_kg(game);
    if (!kg || !cJSON_IsArray(rec)) return;
    const int sid = (int) rec_num(rec, 0, -1), eid = (int) rec_num(rec, 1, -1);
    att_detach(kg, eid, sid);
    KsEntity *e = ent_find(kg, eid);
    if (e && e->controller < 0) {
        e->controlled_by_local = 0;
        e->pose_count = 0;
        e->sim_count = 0;
    }
}

// ---------------------------------------------------------------------------
// A client's render poses (getRenderPose :214562, updateVisuals :214618, glueSoftRider :213627)

static double lerp_d(const double a, const double b, const double t) { return a + (b - a) * t; }

static double lerp_angle(const double a, const double b, const double t)
{
    double d = fmod(b - a, M_PI * 2.0);
    if (d > M_PI) d -= M_PI * 2.0;
    if (d < -M_PI) d += M_PI * 2.0;
    return a + d * t;
}

static KsEntPose pose_lerp(const KsEntPose *a, const KsEntPose *b, const double t)
{
    return (KsEntPose) {0.0, lerp_d(a->x, b->x, t), lerp_d(a->y, b->y, t), lerp_d(a->z, b->z, t), lerp_angle(a->yaw, b->yaw, t),
                        lerp_angle(a->pitch, b->pitch, t), lerp_angle(a->roll, b->roll, t)};
}

// iïiììíî (:214871): the poses of a time-ordered buffer at time t (clamped to its ends)
static KsEntPose buffer_at(const KsEntPose *buf, const int n, const double t)
{
    if (t <= buf[0].t) return buf[0];
    if (t >= buf[n - 1].t) return buf[n - 1];
    for (int i = n - 2; i >= 0; i--)
        if (buf[i].t <= t) return pose_lerp(&buf[i], &buf[i + 1], (t - buf[i].t) / (buf[i + 1].t - buf[i].t));
    return buf[n - 1];
}

static KsEntPose render_pose(KsEntity *e, const double now_ms, const double local_ms)
{
    if (e->controller >= 0 && !e->controlled_by_local && e->pose_count) {
        const double rt = local_ms - 1000.0 / KS_ENT_SEND_HZ;
        while (e->pose_count >= 2 && e->pose_buf[1].t <= rt) {
            memmove(&e->pose_buf[0], &e->pose_buf[1], (size_t) (e->pose_count - 1) * sizeof(e->pose_buf[0]));
            e->pose_count--;
        }
        if (e->pose_count >= 2 && e->pose_buf[0].t <= rt)
            return pose_lerp(&e->pose_buf[0], &e->pose_buf[1], (rt - e->pose_buf[0].t) / (e->pose_buf[1].t - e->pose_buf[0].t));
        return e->pose_buf[0].t > rt ? e->pose_buf[0] : e->pose_buf[e->pose_count - 1];
    }
    if (e->controller < 0 && e->sim_count >= 2) {
        const double rt = now_ms - KS_ENT_STEP_MS;
        while (e->sim_count > 2 && e->sim_buf[1].t <= rt - 150.0) {
            memmove(&e->sim_buf[0], &e->sim_buf[1], (size_t) (e->sim_count - 1) * sizeof(e->sim_buf[0]));
            e->sim_count--;
        }
        e->sim_render_t = rt;
        e->has_sim_render_t = 1;
        return buffer_at(e->sim_buf, e->sim_count, rt);
    }
    const double k = e->controlled_by_local || e->controller >= 0 ? 1.0 : clamp_d(now_ms / KS_ENT_STEP_MS - (e->tick - 1), 0.0, 1.0);
    const KsEntPose cur = {0.0, e->x, e->y, e->z, e->yaw, e->pitch, e->roll};
    const KsEntPose prev = e->has_prev ? e->prev : cur;
    KsEntPose r = pose_lerp(&prev, &cur, k);
    if (e->blend_left > 0.0) {
        const double f = e->blend_left / KS_ENT_BLEND_MS;
        r.x += e->blend[0] * f;
        r.y += e->blend[1] * f;
        r.z += e->blend[2] * f;
    }
    return r;
}

// glueSoftRider (:213627): another player riding soft follows its entity's interpolation (the render pose
// against the pose one send interval earlier), turned round that pose
static void glue_soft_rider(KsGame *kg, Player *p, const int eid)
{
    KsEntity *e = ent_find(kg, eid);
    if (!e || e->controller >= 0 || !e->has_render || !e->has_sim_render_t || !e->sim_count) return;
    const double lag = 1000.0 / KS_ENT_SEND_HZ - KS_ENT_STEP_MS;
    if (lag <= 0.0) return;
    const KsEntPose then = buffer_at(e->sim_buf, e->sim_count, e->sim_render_t - lag);
    const KsEntPose *now = &e->render;
    const double dx = now->x - then.x, dy = now->y - then.y, dz = now->z - then.z;
    double dyaw = fmod(now->yaw - then.yaw, M_PI * 2.0);
    if (dyaw > M_PI) dyaw -= M_PI * 2.0;
    else if (dyaw < -M_PI) dyaw += M_PI * 2.0;
    if (!dx && !dy && !dz && !dyaw) return;
    const double s = sin(dyaw), c = cos(dyaw), rx = p->position.x - then.x, rz = p->position.z - then.z;
    const vec3 d = {(float) (dx + (rx * c + rz * s - rx)), (float) dy, (float) (dz + (-rx * s + rz * c - rz))};
    p->position.x += d.x;
    p->position.y += d.y;
    p->position.z += d.z;
    p->last_position.x += d.x;
    p->last_position.y += d.y;
    p->last_position.z += d.z;
}

static void update_visuals(KsGame *kg, const double now_ms, const double local_ms, const double delta_ms)
{
    for (size_t i = 0; i < kg->ents.count; i++) {
        KsEntity *e = kg->ents.items[i];
        if (!e->active) continue;
        if (e->blend_left > 0.0) e->blend_left -= delta_ms;
        const KsEntPose r = render_pose(e, now_ms, local_ms);
        // softCarryPose: our own soft ride moves us by the render pose's change (KN at once; v10 through movX)
        if (e->has_soft_carry) {
            const KsEntPose *was = &e->soft_carry;
            const double dx = r.x - was->x, dy = r.y - was->y, dz = r.z - was->z;
            double dyaw = fmod(r.yaw - was->yaw, M_PI * 2.0);
            if (dyaw > M_PI) dyaw -= M_PI * 2.0;
            else if (dyaw < -M_PI) dyaw += M_PI * 2.0;
            if (dx || dy || dz || dyaw) {
                const double s = sin(dyaw), c = cos(dyaw);
                for (size_t k = 0; k < kg->ents.att_count; k++) {
                    const KsEntAttach *a = &kg->ents.att[k];
                    if (!a->soft || a->eid != e->eid) continue;
                    Player *p = player_by_sid(kg, a->sid);
                    if (!p || !p->is_you || !p->active) continue;
                    const double rx = p->position.x - was->x, rz = p->position.z - was->z;
                    p->position.x += (float) (dx + (rx * c + rz * s - rx));
                    p->position.y += (float) dy;
                    p->position.z += (float) (dz + (-rx * s + rz * c - rz));
                }
            }
        }
        e->soft_carry = r;
        e->has_soft_carry = 1;
        e->render = r;
        e->has_render = 1;
    }
}

void ks_entities_client_frame(Game *game, const double clock_ms, const double local_ms, const double delta_ms)
{
    KsGame *kg = game ? game->ks : NULL;
    if (!kg || server_side(kg)) return;
    live_poll(kg);
    if (has_hooks(kg)) {
        ent_advance_to(kg, clock_ms);
        drain_queue(kg);
    } else {
        kg->ents.tick = (long) floor(clock_ms / KS_ENT_STEP_MS);
        kg->ents.queue_head = kg->ents.queue_count = 0; // nothing to run them (v10 never drains them)
    }
    update_visuals(kg, clock_ms, local_ms, delta_ms);
    // players.update's riders (:191173-191183): the others (interpolated) pinned to the render pose / glued
    for (size_t i = 0; i < kg->ents.att_count; i++) {
        const KsEntAttach a = kg->ents.att[i];
        Player *p = player_by_sid(kg, a.sid);
        if (!p || p->is_you) continue; // this.interpolate: the others
        KsEntity *e = ent_find(kg, a.eid);
        if (!e) continue;
        if (!a.soft) {
            pin_player(kg, p, e, e->has_render ? &e->render : NULL);
            p->last_position = p->position;
        } else {
            glue_soft_rider(kg, p, a.eid);
        }
    }
}

void ks_entities_client_reset(Game *game)
{
    KsGame *kg = game ? game->ks : NULL;
    if (kg && !server_side(kg)) ks_entities_reset(kg);
}

// ---------------------------------------------------------------------------
// The authority's sends to one connection

static void live_send_all(KsGame *kg, int uid);

void ks_entities_send_seed(Game *game, const int uid)
{
    KsGame *kg = game ? game->ks : NULL;
    if (!kg || !server_side(kg)) return;
    cJSON *seed = cJSON_CreateArray();
    if (seed) cJSON_AddItemToArray(seed, cJSON_CreateNumber((double) (int32_t) kg->ents.seed)); // matchSeed: an int32
    net_json(kg, uid, NET_EXT_ENTITY_SEED, seed);
}

void ks_entities_send_state(Game *game, const int uid)
{
    KsGame *kg = game ? game->ks : NULL;
    if (!kg || !server_side(kg)) return;
    live_send_all(kg, uid);
    for (size_t i = 0; i < kg->ents.count; i++)
        if (kg->ents.items[i]->active) net_json(kg, uid, NET_EXT_ENTITY_SPAWN, spawn_payload(kg->ents.items[i]));
    for (size_t i = 0; i < kg->ents.att_count; i++) net_json(kg, uid, NET_EXT_ENTITY_ATTACH, attach_record(&kg->ents.att[i]));
}

void ks_entities_send_controlled(Game *game, const Player *player, const int uid)
{
    KsGame *kg = game ? game->ks : NULL;
    if (!kg || !player || !server_side(kg)) return;
    const KsEntAttach *a = att_find(kg, player->uid);
    const KsEntity *e = a && a->ctl && !a->soft ? ent_find(kg, a->eid) : NULL;
    if (e) net_json(kg, uid, NET_EXT_ENTITY_CONTROLLED, state_record(e));
}

// ---------------------------------------------------------------------------
// Queries (both sides)

static void view_of(const KsEntity *e, KsEntityView *out)
{
    out->eid = e->eid;
    out->arch = e->arch;
    out->state = e->state;
    out->pose = (KsEntityPose) {e->x, e->y, e->z, e->yaw, e->pitch, e->roll};
    out->render = e->has_render ? (KsEntityPose) {e->render.x, e->render.y, e->render.z, e->render.yaw, e->render.pitch, e->render.roll} : out->pose;
    out->frozen = e->frozen;
    out->hp = e->hp;
    out->controller = e->controller;
    out->part_count = e->part_count;
}

int ks_entities_count(const Game *game)
{
    return game && game->ks ? (int) game->ks->ents.count : 0;
}

int ks_entities_view(const Game *game, const int index, KsEntityView *out)
{
    const KsGame *kg = game ? game->ks : NULL;
    if (!kg || index < 0 || (size_t) index >= kg->ents.count || !out) return 0;
    view_of(kg->ents.items[index], out);
    return 1;
}

int ks_entities_find(const Game *game, const int eid, KsEntityView *out)
{
    const KsEntity *e = game ? ent_find(game->ks, eid) : NULL;
    if (!e) return 0;
    if (out) view_of(e, out);
    return 1;
}

int ks_entities_part_pose(const Game *game, const int eid, const int part, KsEntityPose *out, double size[3], int *is_model)
{
    const KsEntity *e = game ? ent_find(game->ks, eid) : NULL;
    if (!e || part < 0 || part >= e->part_count) return 0;
    const KsEntPose p = part_pose(e, &e->parts[part]);
    if (out) *out = (KsEntityPose) {p.x, p.y, p.z, p.yaw, p.pitch, p.roll};
    if (size) memcpy(size, e->parts[part].size, sizeof(e->parts[part].size));
    if (is_model) *is_model = e->parts[part].is_model;
    return 1;
}

Object *ks_entities_part_object(const Game *game, const int eid, const int part)
{
    KsGame *kg = game ? game->ks : NULL;
    KsEntity *e = ent_find(kg, eid);
    if (!e || part < 0 || part >= e->part_count) return NULL;
    return our_object(kg, e->parts[part].uid, e->parts[part].object);
}

int ks_entities_attachment(const Game *game, const int sid, int *eid, int *soft, int *ctl, int *lk, int *nd)
{
    const KsEntAttach *a = game ? att_find(game->ks, sid) : NULL;
    if (!a) return 0;
    if (eid) *eid = a->eid;
    if (soft) *soft = a->soft;
    if (ctl) *ctl = a->ctl;
    if (lk) *lk = a->lk;
    if (nd) *nd = a->nd;
    return 1;
}

int ks_entities_player_ride(Game *game, Player *player, const Input *input, const int recon)
{
    KsGame *kg = game ? game->ks : NULL;
    const KsEntAttach *a = player ? att_find(kg, player->uid) : NULL;
    if (!a || a->soft) return 0; // player.attachedEnt
    KsEntity *e = ent_find(kg, a->eid);
    if (!e) return 0;
    const int eid = e->eid;
    if (a->ctl && input) step_controlled(kg, e, player, input, recon); // controlledEid == attachedEnt
    e = ent_find(kg, eid);
    if (!e) return 0;
    pin_player(kg, player, e, NULL);
    player->last_position = player->position;
    return 1;
}

int ks_entities_shot_passes(const Game *game, const Player *shooter, const int eid)
{
    const KsEntAttach *a = game && shooter ? att_find(game->ks, shooter->uid) : NULL;
    return a && a->eid == eid && a->nd;
}


// ---------------------------------------------------------------------------
// Shots and splash (rayVsPart :214014, shotPassesThrough :214153, the hits' entities.damage :196082 / :196830 /
// :206933, the explosion's splashDist :214135 / :217057-217067)

static double json_num(const cJSON *v, double def);

static KsEntity *part_owner(KsGame *kg, const Object *object, int *part)
{
    for (size_t i = 0; kg && object && i < kg->ents.count; i++) {
        KsEntity *e = kg->ents.items[i];
        for (int k = 0; k < e->part_count; k++) {
            if (e->parts[k].object != object || !our_object(kg, e->parts[k].uid, object)) continue;
            if (part) *part = k;
            return e;
        }
    }
    return NULL;
}

int ks_entities_object_ray(Game *game, const Object *object, const Player *shooter, const vec3 origin, const vec3 seg, float *t)
{
    KsGame *kg = game ? game->ks : NULL;
    int k = -1;
    const KsEntity *e = object && object->dyn ? part_owner(kg, object, &k) : NULL;
    if (!e) return 0;
    *t = -1.0f;
    if (!e->active || (shooter && ks_entities_shot_passes(game, shooter, e->eid))) return 1;
    const KsEntPart *p = &e->parts[k];
    const double s = sin(e->yaw), c = cos(e->yaw), dx = origin.x - e->x, dz = origin.z - e->z;
    const vec3 lo = {(float) (dx * c - dz * s), origin.y, (float) (dx * s + dz * c)};
    const vec3 ls = {(float) (seg.x * c - seg.z * s), seg.y, (float) (seg.x * s + seg.z * c)};
    const KsEntPose pp = part_pose(e, p);
    *t = line_in_rect(lo, ls, (vec3) {(float) pp.x, (float) (e->y + pp.y), (float) pp.z}, (vec3) {(float) p->size[0], (float) p->size[1], (float) p->size[2]});
    return 1;
}

int ks_entities_object_damage(Game *game, const Object *object, const Player *dealer, const float damage)
{
    KsGame *kg = game ? game->ks : NULL;
    const KsEntity *e = object && object->dyn ? part_owner(kg, object, NULL) : NULL;
    if (!e) return 0;
    if (!server_side(kg) || !(damage > 0.0f)) return 1;
    char by[24] = "";
    if (dealer) ks_player_id(dealer, by, sizeof(by));
    ent_damage(kg, e->eid, damage, by); // (refused for an archetype without hp and for the dealer's own nd ride)
    if (dealer) { // DO_DAMAGE: the dealer's hit marker, sent after every entities.damage call as v10 does
        const NetHitPacket hit = {.target_uid = -1};
        game_send(game, dealer->uid, NET_PACKET_HIT, &hit, sizeof(hit));
    }
    return 1;
}

// A live asset's rayProxy (:212244-212272): its triangles' box, then the model's hit mesh at the rest pose in the
// asset's frame (moved, turned ry, scaled s)
int ks_entities_live_ray(Game *game, const vec3 origin, const vec3 seg, float *t_out)
{
    KsGame *kg = game ? game->ks : NULL;
    float best = 2.0f;
    for (size_t i = 0; kg && i < kg->ents.live_count; i++) {
        const KsLiveObject *lo = &kg->ents.live[i];
        if (!lo->tris || !lo->tri_count) continue;
        float lo_b[3] = {INFINITY, INFINITY, INFINITY}, hi_b[3] = {-INFINITY, -INFINITY, -INFINITY};
        for (size_t v = 0; v < lo->tri_count * 3; v++)
            for (int a = 0; a < 3; a++) {
                const float f = lo->tris[v * 3 + a];
                if (f < lo_b[a]) lo_b[a] = f;
                if (f > hi_b[a]) hi_b[a] = f;
            }
        const vec3 bottom = {(lo_b[0] + hi_b[0]) * 0.5f, lo_b[1], (lo_b[2] + hi_b[2]) * 0.5f};
        const float box_t = line_in_rect(origin, seg, bottom, (vec3) {hi_b[0] - lo_b[0], hi_b[1] - lo_b[1], hi_b[2] - lo_b[2]});
        const int inside = origin.x >= lo_b[0] && origin.x <= hi_b[0] && origin.y >= lo_b[1] && origin.y <= hi_b[1] && origin.z >= lo_b[2] &&
                           origin.z <= hi_b[2];
        if (!(box_t > 0.0f && box_t <= 1.0f) && !inside) continue;
        const char *aid = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(lo->data, "aid"));
        int state = 0;
        const AiPartsModel *m = aid ? ai_parts_model_get(game, atoi(aid), 0, &state) : NULL;
        if (!m) continue;
        const double x = json_num(cJSON_GetObjectItemCaseSensitive(lo->data, "x"), 0), y = json_num(cJSON_GetObjectItemCaseSensitive(lo->data, "y"), 0);
        const double z = json_num(cJSON_GetObjectItemCaseSensitive(lo->data, "z"), 0), ry = json_num(cJSON_GetObjectItemCaseSensitive(lo->data, "ry"), 0);
        const double sc = json_num(cJSON_GetObjectItemCaseSensitive(lo->data, "s"), 0) ? json_num(cJSON_GetObjectItemCaseSensitive(lo->data, "s"), 1) : 1.0;
        const double c = cos(ry), sn = sin(ry), ox = origin.x - x, oz = origin.z - z;
        const double o[3] = {(ox * c - oz * sn) / sc, (origin.y - y) / sc, (ox * sn + oz * c) / sc};
        const double d[3] = {(seg.x * c - seg.z * sn) / sc, seg.y / sc, (seg.x * sn + seg.z * c) / sc};
        double t = 0;
        if (ai_parts_model_ray(m, o, d, &t) && t > 0.0 && t < best) best = (float) t;
    }
    if (best > 1.0f) return 0;
    if (t_out) *t_out = best;
    return 1;
}

// splashDist (:214135): from the point to the entity's footprint box (its yawed frame, footCX / CZ / HX / HZ, 0..footH)
static double splash_dist(const KsEntity *e, const double x, const double y, const double z)
{
    const MapArchetype *a = e->arch;
    const double s = sin(e->yaw), c = cos(e->yaw), dx = x - e->x, dz = z - e->z;
    const double lx = dx * c - dz * s, lz = dx * s + dz * c;
    const double cx = fmax(a->foot_cx - a->foot_hx, fmin(lx, a->foot_cx + a->foot_hx));
    const double cz = fmax(a->foot_cz - a->foot_hz, fmin(lz, a->foot_cz + a->foot_hz));
    const double cy = fmax(0.0, fmin(y - e->y, a->foot_h));
    const double ex = lx - cx, ey = y - e->y - cy, ez = lz - cz;
    return sqrt(ex * ex + ey * ey + ez * ez);
}

void ks_entities_splash(Game *game, const vec3 at, const float radius, const float damage, const Player *owner)
{
    KsGame *kg = game ? game->ks : NULL;
    if (!kg || !server_side(kg) || !owner || !(radius > 0.0f) || !kg->ents.count) return;
    char by[24];
    ks_player_id(owner, by, sizeof(by));
    const size_t n = kg->ents.count;
    int *ids = malloc(n * sizeof(*ids));
    if (!ids) return;
    for (size_t i = 0; i < n; i++) ids[i] = kg->ents.items[i]->eid; // (a damage may despawn)
    for (size_t i = 0; i < n; i++) {
        const KsEntity *e = ent_find(kg, ids[i]);
        if (!e || !e->active || !(e->arch->hp != 0.0f)) continue;
        const double f = 1.0 - splash_dist(e, at.x, at.y, at.z) / radius;
        if (f > 0.0) ent_damage(kg, ids[i], round(damage * f), by);
    }
    free(ids);
}

// ---------------------------------------------------------------------------
// Host objects: the entity (ENTITIES.* results), its position / rotation parts and the ctx the
// simulateEntity hook receives.

static KsValue str_value(KsScript *s, const char *text) { return ks_str(s, text ? text : "", text ? strlen(text) : 0); }

static KsValue ent_id(KsScript *s, void *n, uint32_t id) { (void) id; KsEntity *e = ent_of(n); if (!e) return ks_undef(); char b[24]; snprintf(b, sizeof(b), "%d", e->eid); return str_value(s, b); }
static KsValue ent_archetype(KsScript *s, void *n, uint32_t id) { (void) id; KsEntity *e = ent_of(n); return e ? str_value(s, e->arch->name) : ks_undef(); }
static KsValue ent_tag(KsScript *s, void *n, uint32_t id) { (void) id; KsEntity *e = ent_of(n); return e ? str_value(s, e->tag) : ks_undef(); }
static KsValue ent_hp(KsScript *s, void *n, uint32_t id) { (void) s; (void) id; KsEntity *e = ent_of(n); return e ? ks_num(e->hp) : ks_undef(); }
static KsValue ent_active(KsScript *s, void *n, uint32_t id) { (void) s; (void) id; KsEntity *e = ent_of(n); return e ? ks_bool(e->active) : ks_undef(); }

static KsGame *ks_native_game_kg(void *native) { KsRef *r = native; return r ? r->kg : NULL; }

// position.x / y / z: teleport (:214521): the three pose keys written together; a refused state leaves the
// entity where it was.
static void entity_teleport(KsGame *kg, KsEntity *e, const int axis, const double value)
{
    if (!authority(kg) || !isfinite(value)) return;
    const double pos[3] = {e->x, e->y, e->z};
    const char *keys[3] = {"x", "y", "z"};
    cJSON *items[3];
    for (int i = 0; i < 3; i++) items[i] = cJSON_CreateNumber(i == axis ? value : pos[i]);
    state_write(kg, e, items, keys, 3);
}

static KsValue pos_get(KsScript *s, void *n, uint32_t id, const int axis)
{
    (void) s; (void) id;
    KsEntity *e = ent_of(n);
    if (!e) return ks_undef();
    return ks_num(axis == 0 ? e->x : axis == 1 ? e->y : e->z);
}
static void pos_set(KsGame *kg, void *n, const int axis, KsValue v)
{
    KsEntity *e = ent_of(n);
    if (e) entity_teleport(kg, e, axis, ks_to_float(v));
}
#define POS_AXIS(name, axis)                                                                                                  \
    static KsValue name##_get(KsScript *s, void *n, uint32_t id) { return pos_get(s, n, id, axis); }                           \
    static void name##_set(KsScript *s, void *n, uint32_t id, KsValue v) { (void) s; (void) id; pos_set(ks_native_game_kg(n), n, axis, v); }
POS_AXIS(pos_x, 0)
POS_AXIS(pos_y, 1)
POS_AXIS(pos_z, 2)

// rotation.x / y / z = pitch / yaw / roll: setVar (:209104-209128).
static KsValue rot_get(KsScript *s, void *n, uint32_t id, const int axis)
{
    (void) s; (void) id;
    KsEntity *e = ent_of(n);
    if (!e) return ks_undef();
    return ks_num(axis == 0 ? e->pitch : axis == 1 ? e->yaw : e->roll);
}
static void rot_set(KsScript *s, void *n, uint32_t id, const int axis, KsValue v)
{
    (void) s; (void) id;
    KsEntity *e = ent_of(n);
    if (!e) return;
    const char *key = axis == 0 ? "pitch" : axis == 1 ? "yaw" : "roll";
    const double value = ks_to_float(v);
    entity_set_var(ks_native_game_kg(n), e, key, cJSON_CreateNumber(value));
}
#define ROT_AXIS(name, axis)                                                                                                  \
    static KsValue name##_get(KsScript *s, void *n, uint32_t id) { return rot_get(s, n, id, axis); }                          \
    static void name##_set(KsScript *s, void *n, uint32_t id, KsValue v) { rot_set(s, n, id, axis, v); }
ROT_AXIS(rot_x, 0)
ROT_AXIS(rot_y, 1)
ROT_AXIS(rot_z, 2)

static const KsHostProp g_ent_position_props[] = {{"x", pos_x_get, pos_x_set, NULL, 0}, {"y", pos_y_get, pos_y_set, NULL, 0}, {"z", pos_z_get, pos_z_set, NULL, 0}};
static const KsHostProp g_ent_rotation_props[] = {{"x", rot_x_get, rot_x_set, NULL, 0}, {"y", rot_y_get, rot_y_set, NULL, 0}, {"z", rot_z_get, rot_z_set, NULL, 0}};
static const KsHostClass g_ent_position_class = {"position", g_ent_position_props, 3};
static const KsHostClass g_ent_rotation_class = {"rotation", g_ent_rotation_props, 3};

static KsValue ent_position(KsScript *s, void *n, uint32_t id) { return ent_of(n) ? ks_host_object(s, &g_ent_position_class, n, id) : ks_undef(); }
static KsValue ent_rotation(KsScript *s, void *n, uint32_t id) { return ent_of(n) ? ks_host_object(s, &g_ent_rotation_class, n, id) : ks_undef(); }

// Entity methods (the entity wrapper, :209061-209195). Writes are the authority's.
static void key_text(KsScript *s, const KsValue v, char *out, const size_t n)
{
    ks_to_string(s, v, out, n);
}

static KsValue m_get_var(KsScript *s, void *n, const KsValue *args, const int argc)
{
    KsEntity *e = ent_of(n);
    char key[96] = "";
    if (!e || argc < 1) return ks_undef();
    key_text(s, args[0], key, sizeof(key));
    const cJSON *item = e->state ? cJSON_GetObjectItemCaseSensitive(e->state, key) : NULL;
    if (!item) return ks_undef();
    if (cJSON_IsNumber(item)) return ks_num(item->valuedouble);
    if (cJSON_IsBool(item)) return ks_bool(cJSON_IsTrue(item));
    if (cJSON_IsString(item)) return ks_str(s, item->valuestring, strlen(item->valuestring));
    return ks_undef();
}

static cJSON *scalar_json(const KsValue v)
{
    if (v.tag == KS_NUM) return cJSON_CreateNumber(v.as.num);
    if (v.tag == KS_BOOL) return cJSON_CreateBool(v.as.b);
    return NULL; // strings are copied through json_from_value (they need the script's text)
}

static KsValue m_set_var(KsScript *s, void *n, const KsValue *args, const int argc)
{
    KsEntity *e = ent_of(n);
    if (!e || argc < 2 || !authority(ks_native_game_kg(n))) return ks_bool(0);
    if (args[1].tag == KS_OBJ || args[1].tag == KS_ARR || args[1].tag == KS_UNDEF || args[1].tag == KS_NULL || args[1].tag == KS_FN || args[1].tag == KS_HOST)
        return ks_bool(0);
    char key[96] = "";
    key_text(s, args[0], key, sizeof(key));
    cJSON *value = args[1].tag == KS_STR ? json_from_value(s, args[1]) : scalar_json(args[1]);
    return ks_bool(entity_set_var(ks_native_game_kg(n), e, key, value));
}

static KsValue m_set_state(KsScript *s, void *n, const KsValue *args, const int argc)
{
    KsEntity *e = ent_of(n);
    KsGame *kg = ks_native_game_kg(n);
    if (!e || argc < 1 || !authority(kg) || args[0].tag != KS_OBJ) return ks_bool(0);
    cJSON *patch = json_object_from_value(s, args[0], 0);
    if (!patch) return ks_bool(0);
    // Object.assign(state, obj): each key of the object is written, then the whole state is checked
    const int count = cJSON_GetArraySize(patch);
    cJSON **items = calloc((size_t) (count ? count : 1), sizeof(*items));
    const char **keys = calloc((size_t) (count ? count : 1), sizeof(*keys));
    int taken = 0;
    if (items && keys) {
        for (const cJSON *c = patch->child; c && taken < count; c = c->next) {
            keys[taken] = c->string;
            items[taken] = cJSON_Duplicate(c, 0);
            if (items[taken]) taken++;
        }
        const int ok = state_write(kg, e, items, keys, taken);
        free(items);
        free(keys);
        cJSON_Delete(patch);
        return ks_bool(ok);
    }
    free(items);
    free(keys);
    cJSON_Delete(patch);
    return ks_bool(0);
}

// playAnim / stopAnim / setAnimSpeed: the state's <name>_clip / _clipS on the server. A client's playPartClip
// plays a model part's clip; KN's entity model parts are hidden boxes (client/src/entities.c), so it is false.
static KsValue m_play_anim(KsScript *s, void *n, const KsValue *args, const int argc)
{
    KsEntity *e = ent_of(n);
    KsGame *kg = ks_native_game_kg(n);
    if (!e || !authority(kg)) return ks_bool(0);
    char name[96] = "", clip[256] = "";
    if (argc > 0 && args[0].tag != KS_UNDEF && args[0].tag != KS_NULL) key_text(s, args[0], name, sizeof(name));
    if (argc > 1 && args[1].tag != KS_UNDEF && args[1].tag != KS_NULL) key_text(s, args[1], clip, sizeof(clip));
    char key[160];
    snprintf(key, sizeof(key), "%s_clip", name);
    cJSON *clip_v = cJSON_CreateString(clip);
    const int ok = entity_set_var(kg, e, key, clip_v);
    if (ok && argc > 2 && args[2].tag != KS_UNDEF && args[2].tag != KS_NULL) {
        const double speed = ks_to_float(args[2]);
        snprintf(key, sizeof(key), "%s_clipS", name);
        entity_set_var(kg, e, key, cJSON_CreateNumber(speed ? speed : 1.0)); // parseFloat(speed) || 1
    }
    return ks_bool(ok);
}

static KsValue m_stop_anim(KsScript *s, void *n, const KsValue *args, const int argc)
{
    KsEntity *e = ent_of(n);
    KsGame *kg = ks_native_game_kg(n);
    if (!e || !authority(kg)) return ks_bool(0);
    char name[96] = "", key[160];
    if (argc > 0 && args[0].tag != KS_UNDEF && args[0].tag != KS_NULL) key_text(s, args[0], name, sizeof(name));
    snprintf(key, sizeof(key), "%s_clip", name);
    return ks_bool(entity_set_var(kg, e, key, cJSON_CreateString("")));
}

static KsValue m_set_anim_speed(KsScript *s, void *n, const KsValue *args, const int argc)
{
    KsEntity *e = ent_of(n);
    KsGame *kg = ks_native_game_kg(n);
    if (!e || !authority(kg) || argc < 2) return ks_bool(0);
    char name[96] = "", key[160];
    key_text(s, args[0], name, sizeof(name));
    const double speed = ks_to_float(args[1]);
    if (!isfinite(speed)) return ks_bool(0);
    snprintf(key, sizeof(key), "%s_clipS", name);
    return ks_bool(entity_set_var(kg, e, key, cJSON_CreateNumber(speed)));
}

static KsValue m_damage(KsScript *s, void *n, const KsValue *args, const int argc)
{
    KsEntity *e = ent_of(n);
    KsGame *kg = ks_native_game_kg(n);
    if (!e || !authority(kg)) return ks_bool(0);
    const double amount = argc > 0 ? ks_to_float(args[0]) : 0.0;
    char by[64] = "";
    if (argc > 1 && args[1].tag != KS_UNDEF && args[1].tag != KS_NULL) key_text(s, args[1], by, sizeof(by));
    return ks_bool(ent_damage(kg, e->eid, amount, by));
}

static int value_truthy(const KsValue v)
{
    switch (v.tag) {
        case KS_BOOL: return v.as.b;
        case KS_NUM: return v.as.num != 0 && !isnan(v.as.num);
        case KS_STR: {
            size_t len = 0;
            ks_str_utf8(v, &len);
            return len > 0;
        }
        case KS_UNDEF:
        case KS_NULL: return 0;
        default: return 1;
    }
}

// sanitize(opts) of attachPlayer / updateAttachment: o (an array), ox / oy / oz, by, lk, ctl, soft, nd; `has`
// marks the keys present (updateAttachment keeps the others, `opts.x != null`)
typedef struct {
    int o, lk, ctl, soft, nd;
} AttachHas;

static void attach_opts(KsScript *s, const KsValue v, AttachOpts *out, AttachHas *has)
{
    memset(out, 0, sizeof(*out));
    if (has) memset(has, 0, sizeof(*has));
    if (v.tag != KS_OBJ) return;
    const KsValue o = ks_obj_get(s, v, "o");
    if (o.tag == KS_ARR) {
        out->has_o = 1;
        for (size_t k = 0; k < 3; k++) out->o[k] = k < ks_arr_len(o) ? ks_to_float(ks_arr_get(s, o, k)) : 0.0;
    }
    out->ox = ks_to_float(ks_obj_get(s, v, "ox"));
    out->oy = ks_to_float(ks_obj_get(s, v, "oy"));
    out->oz = ks_to_float(ks_obj_get(s, v, "oz"));
    out->by = ks_to_float(ks_obj_get(s, v, "by"));
    static const char *const flags[4] = {"lk", "ctl", "soft", "nd"};
    int *slot[4] = {&out->lk, &out->ctl, &out->soft, &out->nd};
    int *seen[4] = {has ? &has->lk : NULL, has ? &has->ctl : NULL, has ? &has->soft : NULL, has ? &has->nd : NULL};
    for (int k = 0; k < 4; k++) {
        const KsValue f = ks_obj_get(s, v, flags[k]);
        *slot[k] = value_truthy(f);
        if (seen[k]) *seen[k] = f.tag != KS_UNDEF && f.tag != KS_NULL;
    }
    if (has) has->o = o.tag != KS_UNDEF && o.tag != KS_NULL;
}

// attachPlayer(playerId, opts) (:209172): the server's, the player by sid or id
static KsValue m_attach_player(KsScript *s, void *n, const KsValue *args, const int argc)
{
    KsEntity *e = ent_of(n);
    KsGame *kg = ks_native_game_kg(n);
    if (!e || !authority(kg)) return ks_bool(0);
    char id[64] = "";
    if (argc > 0 && value_truthy(args[0])) key_text(s, args[0], id, sizeof(id));
    AttachOpts opts;
    attach_opts(s, argc > 1 ? args[1] : ks_undef(), &opts, NULL);
    return ks_bool(att_attach(kg, e, ks_game_find_player_str(kg->game, id), &opts));
}

// updateAttachment(playerId, opts) (:213495): the keys given replace the player's attachment to this entity
static KsValue m_update_attachment(KsScript *s, void *n, const KsValue *args, const int argc)
{
    KsEntity *e = ent_of(n);
    KsGame *kg = ks_native_game_kg(n);
    if (!e || !authority(kg)) return ks_bool(0);
    char id[64] = "";
    if (argc > 0 && value_truthy(args[0])) key_text(s, args[0], id, sizeof(id));
    Player *p = ks_game_find_player_str(kg->game, id);
    const KsEntAttach *cur = att_find(kg, p ? p->uid : (int) strtol(id, NULL, 10));
    if (!p || !cur || cur->eid != e->eid) return ks_bool(0);
    AttachOpts opts;
    AttachHas has;
    attach_opts(s, argc > 1 ? args[1] : ks_undef(), &opts, &has);
    AttachOpts merged = {.has_o = 1, .by = 0.0};
    if (has.o && opts.has_o) memcpy(merged.o, opts.o, sizeof(merged.o));
    else if (has.o) merged.has_o = 0; // a non-array o: [ox, oy, oz] of the merged opts, which carry none (0)
    else memcpy(merged.o, cur->o, sizeof(merged.o));
    merged.lk = has.lk ? opts.lk : cur->lk;
    merged.ctl = has.ctl ? opts.ctl : cur->ctl;
    merged.soft = has.soft ? opts.soft : cur->soft;
    merged.nd = has.nd ? opts.nd : cur->nd;
    return ks_bool(att_attach(kg, e, p, &merged));
}

// detachPlayer(playerId) (:209186)
static KsValue m_detach_player(KsScript *s, void *n, const KsValue *args, const int argc)
{
    KsEntity *e = ent_of(n);
    KsGame *kg = ks_native_game_kg(n);
    if (!e || !authority(kg)) return ks_bool(0);
    char id[64] = "";
    if (argc > 0 && value_truthy(args[0])) key_text(s, args[0], id, sizeof(id));
    const Player *p = ks_game_find_player_str(kg->game, id);
    return ks_bool(att_detach(kg, e->eid, p ? p->uid : (int) strtol(id, NULL, 10)));
}

static KsValue m_despawn(KsScript *s, void *n, const KsValue *args, const int argc)
{
    (void) s; (void) args; (void) argc;
    KsEntity *e = ent_of(n);
    KsGame *kg = ks_native_game_kg(n);
    if (e && authority(kg)) ent_despawn(kg, e, "script");
    return ks_undef();
}

static const KsHostProp g_entity_props[] = {
    {"id", ent_id, NULL, NULL, 0},
    {"archetype", ent_archetype, NULL, NULL, 0},
    {"tag", ent_tag, NULL, NULL, 0},
    {"hp", ent_hp, NULL, NULL, 0},
    {"active", ent_active, NULL, NULL, 0},
    {"position", ent_position, NULL, NULL, 1},
    {"rotation", ent_rotation, NULL, NULL, 1},
    {"getVar", NULL, NULL, m_get_var, 0},
    {"setVar", NULL, NULL, m_set_var, 0},
    {"setState", NULL, NULL, m_set_state, 0},
    {"playAnim", NULL, NULL, m_play_anim, 0},
    {"stopAnim", NULL, NULL, m_stop_anim, 0},
    {"setAnimSpeed", NULL, NULL, m_set_anim_speed, 0},
    {"damage", NULL, NULL, m_damage, 0},
    {"attachPlayer", NULL, NULL, m_attach_player, 0},
    {"updateAttachment", NULL, NULL, m_update_attachment, 0},
    {"detachPlayer", NULL, NULL, m_detach_player, 0},
    {"despawn", NULL, NULL, m_despawn, 0},
};
const KsHostClass ks_entity_class = {"entity", g_entity_props, (int) (sizeof(g_entity_props) / sizeof(g_entity_props[0]))};

static KsValue wrap_entity(KsScript *s, KsGame *kg, const KsEntity *e)
{
    KsRef *ref = e ? ks_ref_get(kg, KS_REF_ENTITY, e->eid) : NULL;
    return ref ? ks_host_object(s, &ks_entity_class, ref, (uint32_t) e->eid) : ks_undef();
}

// The simulateEntity ctx (buildCtx / buildCtxBase, :213125-213270): tick, rand, inputs (a controlled step's,
// else zeros), data, emit, raycast, groundY, peer, peers.
static KsValue ctx_tick(KsScript *s, void *n, uint32_t id) { (void) s; (void) id; KsEntity *e = ent_of(n); return e ? ks_num(e->step_tick) : ks_undef(); }
static KsValue ctx_inputs(KsScript *s, void *n, uint32_t id)
{
    (void) id;
    if (!ent_of(n)) return ks_undef();
    KsValue obj = ks_obj_new(s);
    static const char *const keys[] = {"active", "forward", "strafe", "jump", "crouch", "shoot", "interact", "yaw", "pitch"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) ks_obj_set(s, obj, keys[i], ks_num(g_step_inputs.set ? g_step_inputs.v[i] : 0.0));
    return obj;
}
static KsValue ctx_data(KsScript *s, void *n, uint32_t id)
{
    (void) id;
    KsEntity *e = ent_of(n);
    if (!e) return ks_undef();
    return e->arch->data ? value_from_json(s, e->arch->data) : ks_obj_new(s);
}

static KsValue ctx_rand(KsScript *s, void *n, const KsValue *args, const int argc)
{
    (void) s; (void) args; (void) argc;
    KsEntity *e = ent_of(n);
    return e ? ks_num(ent_rand_next(&e->rng)) : ks_undef();
}

static KsValue ctx_emit(KsScript *s, void *n, const KsValue *args, const int argc)
{
    KsEntity *e = ent_of(n);
    KsRef *r = n;
    if (!e || argc < 1 || (r && r->kg && r->kg->ents.replaying) || e->event_count >= KS_ENT_EVENTS) return ks_undef();
    char name[256];
    ks_to_string(s, args[0], name, sizeof(name));
    KsEntEvent *ev = &e->events[e->event_count++];
    snprintf(ev->name, sizeof(ev->name), "%.*s", ENT_CTX_EVENT_NAME_MAX, name);
    const double a = argc > 1 ? ks_to_number(args[1]) : 0.0, b = argc > 2 ? ks_to_number(args[2]) : 0.0;
    ev->a = isnan(a) ? 0.0 : a;
    ev->b = isnan(b) ? 0.0 : b;
    return ks_undef();
}

// ctx.raycast(x, y, z, dx, dy, dz, dist): the static objects (no dyn ones, no entity parts), like v10's
// collisions raycast with its filter.
static KsValue ctx_raycast_value(KsScript *s, KsGame *kg, const double origin[3], const double dir[3], const double dist)
{
    KsValue out = ks_obj_new(s);
    GameRayHit hit;
    vec3 o = {(float) origin[0], (float) origin[1], (float) origin[2]};
    vec3 d = {(float) dir[0], (float) dir[1], (float) dir[2]};
    const int found = game_raycast(kg->game, o, d, (float) dist, NULL, 0, &hit);
    ks_obj_set(s, out, "hit", ks_bool(found));
    ks_obj_set(s, out, "dist", ks_num(found ? hit.dist : dist));
    ks_obj_set(s, out, "nx", ks_num(found ? hit.normal.x : 0));
    ks_obj_set(s, out, "ny", ks_num(found ? hit.normal.y : 0));
    ks_obj_set(s, out, "nz", ks_num(found ? hit.normal.z : 0));
    ks_obj_set(s, out, "px", ks_num(found ? hit.point.x : 0));
    ks_obj_set(s, out, "py", ks_num(found ? hit.point.y : 0));
    ks_obj_set(s, out, "pz", ks_num(found ? hit.point.z : 0));
    return out;
}

static KsValue ctx_raycast(KsScript *s, void *n, const KsValue *args, const int argc)
{
    KsEntity *e = ent_of(n);
    if (!e || argc < 7) return ks_undef();
    KsGame *kg = ks_native_game_kg(n);
    const double o[3] = {ks_to_float(args[0]), ks_to_float(args[1]), ks_to_float(args[2])};
    const double d[3] = {ks_to_float(args[3]), ks_to_float(args[4]), ks_to_float(args[5])};
    return ctx_raycast_value(s, kg, o, d, ks_to_float(args[6]));
}

// groundY(x, z, y): the top of the static object below (x, y, z), else y - 1000 (the argument order is x, z, y).
static KsValue ctx_ground_y(KsScript *s, void *n, const KsValue *args, const int argc)
{
    KsEntity *e = ent_of(n);
    if (!e || argc < 3) return ks_undef();
    KsGame *kg = ks_native_game_kg(n);
    const double x = ks_to_float(args[0]), z = ks_to_float(args[1]), y = ks_to_float(args[2]);
    const double o[3] = {x, y, z}, down[3] = {0.0, -1.0, 0.0};
    KsValue hit = ctx_raycast_value(s, kg, o, down, 1000.0);
    if (ks_obj_get(s, hit, "hit").as.b) return ks_obj_get(s, hit, "py");
    return ks_num(y - 1000.0);
}

// peer(id) (:213228): the other entity's state before its last step (prevWire), null when it has none
static KsValue ctx_peer(KsScript *s, void *n, const KsValue *args, const int argc)
{
    KsEntity *e = ent_of(n);
    KsGame *kg = ks_native_game_kg(n);
    if (!e || argc < 1) return ks_null();
    int eid;
    if (args[0].tag == KS_STR) {
        char text[32];
        ks_to_string(s, args[0], text, sizeof(text));
        char *end = NULL;
        const long v = strtol(text, &end, 10);
        if (end == text) return ks_null();
        eid = (int) v;
    } else {
        eid = (int) ks_to_number(args[0]);
    }
    const KsEntity *o = ent_find(kg, eid);
    return o && o->prev_wire ? state_value(s, o->prev_wire) : ks_null();
}

// peers(radius) (:213236): up to 8 other active entities within radius (|| 100) on x / z, by eid, at their pose
// before this tick's step when they already stepped it (peerPose)
static KsValue ctx_peers(KsScript *s, void *n, const KsValue *args, const int argc)
{
    KsEntity *e = ent_of(n);
    KsGame *kg = ks_native_game_kg(n);
    KsValue list = ks_arr_new(s);
    if (!e) return list;
    double r = argc > 0 ? ks_to_number(args[0]) : 0.0;
    if (!r || isnan(r)) r = 100.0;
    const size_t total = kg->ents.count;
    KsEntity **others = malloc((total ? total : 1) * sizeof(*others));
    if (!others) return list;
    size_t count = 0;
    for (size_t i = 0; i < total; i++)
        if (kg->ents.items[i]->active && kg->ents.items[i]->eid != e->eid) others[count++] = kg->ents.items[i];
    qsort(others, count, sizeof(*others), eid_cmp);
    int pushed = 0;
    for (size_t i = 0; i < count && pushed < 8; i++) {
        const KsEntity *o = others[i];
        const int use_prev = o->tick >= kg->ents.tick && o->has_prev;
        const double px = use_prev ? o->prev.x : o->x, py = use_prev ? o->prev.y : o->y, pz = use_prev ? o->prev.z : o->z;
        const double pyaw = use_prev ? o->prev.yaw : o->yaw;
        const double dx = px - e->x, dz = pz - e->z, dist = sqrt(dx * dx + dz * dz);
        if (dist > r) continue;
        KsValue item = ks_obj_new(s);
        char id[24];
        snprintf(id, sizeof(id), "%d", o->eid);
        ks_obj_set(s, item, "id", ks_str(s, id, strlen(id)));
        ks_obj_set(s, item, "x", ks_num(px));
        ks_obj_set(s, item, "y", ks_num(py));
        ks_obj_set(s, item, "z", ks_num(pz));
        ks_obj_set(s, item, "yaw", ks_num(pyaw));
        ks_obj_set(s, item, "dist", ks_num(dist));
        ks_arr_push(s, list, item);
        pushed++;
    }
    free(others);
    return list;
}

static const KsHostProp g_ctx_props[] = {
    {"tick", ctx_tick, NULL, NULL, 0},
    {"inputs", ctx_inputs, NULL, NULL, 0},
    {"data", ctx_data, NULL, NULL, 0},
    {"rand", NULL, NULL, ctx_rand, 0},
    {"emit", NULL, NULL, ctx_emit, 0},
    {"raycast", NULL, NULL, ctx_raycast, 0},
    {"groundY", NULL, NULL, ctx_ground_y, 0},
    {"peer", NULL, NULL, ctx_peer, 0},
    {"peers", NULL, NULL, ctx_peers, 0},
};
const KsHostClass ks_entity_ctx_class = {"ctx", g_ctx_props, (int) (sizeof(g_ctx_props) / sizeof(g_ctx_props[0]))};

// ---------------------------------------------------------------------------
// LIVE_OBJECTS (liveObjects, :212096-212411; the API :211251-211304), on the authority and as a client's copy of "lo"

static double json_num(const cJSON *v, const double def) { return cJSON_IsNumber(v) ? v->valuedouble : def; }

static void live_free_one(KsGame *kg, KsLiveObject *lo)
{
    retire_object(kg, lo->uid, lo->object);
    cJSON_Delete(lo->data);
    free(lo->tris);
}

static void live_reset(KsGame *kg)
{
    for (size_t i = 0; i < kg->ents.live_count; i++) live_free_one(kg, &kg->ents.live[i]);
    kg->ents.live_count = 0; // the ids keep counting (liveObjects.id is never reset)
}

static cJSON *live_record(const KsLiveObject *lo)
{
    cJSON *rec = cJSON_CreateArray();
    if (!rec) return NULL;
    cJSON_AddItemToArray(rec, cJSON_CreateNumber(lo->id));
    cJSON_AddItemToArray(rec, cJSON_Duplicate(lo->data, 1));
    return rec;
}

static void live_send_all(KsGame *kg, const int uid)
{
    for (size_t i = 0; i < kg->ents.live_count; i++) net_json(kg, uid, NET_EXT_KN_OFFICIAL_LIVE_OBJECT, live_record(&kg->ents.live[i]));
}

static KsLiveObject *live_find(KsGame *kg, const int id)
{
    for (size_t i = 0; kg && i < kg->ents.live_count; i++)
        if (kg->ents.live[i].id == id) return &kg->ents.live[i];
    return NULL;
}

// A texture index from a Krunker src name ("wall_0"), -1 none
static int texture_of(const char *src)
{
    char buf[64];
    for (int i = 0; src && i < PREFAB_TEXTURE_COUNT; i++)
        if (prefab_texture_src(i, 0, buf, sizeof(buf)) && !strcmp(buf, src)) return i;
    return -1;
}

// loadTris (:212187-212216): the model's rest triangles (6000 at most, ai_parts_model_rest_tris) scaled by s, turned ry and
// moved to (x, y, z), as the asset's collision triangles (applyToQuad: world_player_tris). A model still loading is asked
// again each update (the promise's then); a failed one gives none. Its rayProxy (shots): ks_entities_live_ray.
static void live_load_tris(KsGame *kg, KsLiveObject *lo)
{
    lo->tris_wait = 0;
    const char *aid = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(lo->data, "aid"));
    const int asset = aid ? atoi(aid) : 0;
    if (!kg->game || asset <= 0) return;
    int state = 0;
    AiPartsModel *m = ai_parts_model_get(kg->game, asset, 1, &state);
    if (!m) {
        lo->tris_wait = state == AI_PARTS_MODEL_LOADING;
        return;
    }
    size_t n = 0;
    const float *rest = ai_parts_model_rest_tris(m, &n);
    if (!rest || !n) return;
    float *out = malloc(n * 9 * sizeof(float));
    if (!out) return;
    const double x = json_num(cJSON_GetObjectItemCaseSensitive(lo->data, "x"), 0), y = json_num(cJSON_GetObjectItemCaseSensitive(lo->data, "y"), 0);
    const double z = json_num(cJSON_GetObjectItemCaseSensitive(lo->data, "z"), 0), ry = json_num(cJSON_GetObjectItemCaseSensitive(lo->data, "ry"), 0);
    const double sc = json_num(cJSON_GetObjectItemCaseSensitive(lo->data, "s"), 0) ? json_num(cJSON_GetObjectItemCaseSensitive(lo->data, "s"), 1) : 1.0;
    const double c = cos(ry), sn = sin(ry);
    for (size_t v = 0; v < n * 3; v++) {
        const double a = rest[v * 3] * sc, b = rest[v * 3 + 2] * sc;
        out[v * 3] = (float) (x + a * c + b * sn);
        out[v * 3 + 1] = (float) (y + rest[v * 3 + 1] * sc);
        out[v * 3 + 2] = (float) (z - a * sn + b * c);
    }
    lo->tris = out;
    lo->tri_count = n;
}

static void live_poll(KsGame *kg)
{
    for (size_t i = 0; i < kg->ents.live_count; i++)
        if (kg->ents.live[i].tris_wait) live_load_tris(kg, &kg->ents.live[i]);
}

// One live object on this side (liveObjects.addBlock / addAsset on the authority, syncNewObjects :212277 on a client):
// a block that collides (isMapObject: !data.noCol) is a map object on the server; a client draws every block
// (noCol: no collision) and loads an asset's GLB as a CUSTOM_ASSET map object (<aid digits>/model.glb, turned ry,
// scaled s); a cpx asset loads its collision triangles on both sides.
static KsLiveObject *live_add(KsGame *kg, const int id, cJSON *data)
{
    if (!data) return NULL;
    if (kg->ents.live_count == kg->ents.live_cap) {
        const size_t cap = kg->ents.live_cap ? kg->ents.live_cap * 2 : 16;
        KsLiveObject *grown = realloc(kg->ents.live, cap * sizeof(*grown));
        if (!grown) {
            cJSON_Delete(data);
            return NULL;
        }
        kg->ents.live = grown;
        kg->ents.live_cap = cap;
    }
    KsLiveObject *lo = &kg->ents.live[kg->ents.live_count++];
    *lo = (KsLiveObject) {.id = id, .data = data};
    const char *type = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(data, "type"));
    const cJSON *props = cJSON_GetObjectItemCaseSensitive(data, "data");
    const double x = json_num(cJSON_GetObjectItemCaseSensitive(data, "x"), 0), y = json_num(cJSON_GetObjectItemCaseSensitive(data, "y"), 0);
    const double z = json_num(cJSON_GetObjectItemCaseSensitive(data, "z"), 0);
    const cJSON *no_col = cJSON_GetObjectItemCaseSensitive(props, "noCol");
    const int solid = !(no_col && !cJSON_IsFalse(no_col) && !cJSON_IsNull(no_col));
    cJSON *record = NULL;
    if (type && !strcmp(type, "block") && (solid || !server_side(kg))) {
        record = cJSON_CreateObject();
        const double w = json_num(cJSON_GetObjectItemCaseSensitive(data, "w"), 1), l = json_num(cJSON_GetObjectItemCaseSensitive(data, "l"), 1);
        const double h = json_num(cJSON_GetObjectItemCaseSensitive(data, "h"), 1);
        cJSON_AddItemToObject(record, "p", cJSON_CreateDoubleArray((const double[]) {x, y, z}, 3));
        cJSON_AddItemToObject(record, "s", cJSON_CreateDoubleArray((const double[]) {w, h, l}, 3));
        const char *colr = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(props, "colr"));
        if (!colr) colr = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(props, "color"));
        if (colr) cJSON_AddStringToObject(record, "c", colr);
        const int tex = texture_of(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(props, "src")));
        if (tex >= 0) cJSON_AddNumberToObject(record, "t", tex);
        const cJSON *opacity = cJSON_GetObjectItemCaseSensitive(props, "opacity");
        if (cJSON_IsNumber(opacity)) cJSON_AddNumberToObject(record, "o", opacity->valuedouble);
        if (!solid) cJSON_AddNumberToObject(record, "col", 1);
    } else if (type && !strcmp(type, "asset") && !server_side(kg)) {
        const cJSON *ja = cJSON_GetObjectItemCaseSensitive(data, "aid");
        char aid[48];
        if (cJSON_IsString(ja)) snprintf(aid, sizeof(aid), "%s", ja->valuestring);
        else snprintf(aid, sizeof(aid), "%.0f", json_num(ja, 0));
        size_t n = 0;
        for (size_t i = 0; aid[i]; i++)
            if (aid[i] >= '0' && aid[i] <= '9') aid[n++] = aid[i]; // (aid + "").replace(/[^\d]/g, "")
        aid[n] = 0;
        if (n) {
            strcat(aid, "q"); // loadAnimMesh fileExt "glb"
            const double sc = json_num(cJSON_GetObjectItemCaseSensitive(data, "s"), 0) ? json_num(cJSON_GetObjectItemCaseSensitive(data, "s"), 1) : 1.0;
            record = cJSON_CreateObject();
            cJSON_AddNumberToObject(record, "i", PREFAB_CUSTOM_ASSET);
            cJSON_AddStringToObject(record, "aid", aid);
            cJSON_AddItemToObject(record, "p", cJSON_CreateDoubleArray((const double[]) {x, y, z}, 3));
            cJSON_AddItemToObject(record, "s", cJSON_CreateDoubleArray((const double[]) {1, 1, 1}, 3));
            cJSON_AddItemToObject(record, "r", cJSON_CreateDoubleArray((const double[]) {0, json_num(cJSON_GetObjectItemCaseSensitive(data, "ry"), 0), 0}, 3));
            cJSON_AddNumberToObject(record, "ms", sc);
            cJSON_AddNumberToObject(record, "col", 1);
        }
    }
    if (record) lo->object = append_object(kg, record, &lo->uid);
    const cJSON *cpx = cJSON_GetObjectItemCaseSensitive(props, "cpx");
    if (type && !strcmp(type, "asset") && cpx && !cJSON_IsFalse(cpx) && !cJSON_IsNull(cpx)) live_load_tris(kg, lo);
    return lo;
}

// liveObjects.remove(id) (:212361) / syncLiveObjects (:212320): the object, its triangles and its map object go; the
// server broadcasts "slo" [[id, "remove"]]
static void live_remove(KsGame *kg, const int id)
{
    KsLiveObject *lo = live_find(kg, id);
    if (!lo) return;
    live_free_one(kg, lo);
    const size_t i = (size_t) (lo - kg->ents.live);
    memmove(&kg->ents.live[i], &kg->ents.live[i + 1], (kg->ents.live_count - i - 1) * sizeof(*kg->ents.live));
    kg->ents.live_count--;
    if (!server_side(kg)) return;
    cJSON *rec = cJSON_CreateArray();
    if (rec) {
        cJSON_AddItemToArray(rec, cJSON_CreateNumber(id));
        cJSON_AddItemToArray(rec, cJSON_CreateString("remove"));
    }
    net_json(kg, NET_TARGET_ALL, NET_EXT_KN_OFFICIAL_LIVE_SYNC, rec);
}

void ks_entities_net_live(Game *game, const cJSON *rec)
{
    KsGame *kg = client_kg(game);
    const cJSON *data = cJSON_GetArrayItem(rec, 1);
    if (!kg || !cJSON_IsArray(rec) || !cJSON_IsObject(data) || !game->map) return;
    const int id = (int) rec_num(rec, 0, -1);
    if (live_find(kg, id) || kg->ents.live_count >= LIVE_OBJECTS_MAX) return; // a joiner's init and a broadcast: one
    live_add(kg, id, cJSON_Duplicate(data, 1));
}

void ks_entities_net_live_sync(Game *game, const cJSON *rec)
{
    KsGame *kg = client_kg(game);
    const char *op = cJSON_GetStringValue(cJSON_GetArrayItem(rec, 1));
    if (kg && cJSON_IsArray(rec) && op && !strcmp(op, "remove")) live_remove(kg, (int) rec_num(rec, 0, -1));
}

const Object *ks_entities_live_object(const Game *game, const int id)
{
    KsGame *kg = game ? game->ks : NULL;
    const KsLiveObject *lo = live_find(kg, id);
    return lo ? our_object(kg, lo->uid, lo->object) : NULL;
}

int ks_entities_live_count(const Game *game)
{
    return game && game->ks ? (int) game->ks->ents.live_count : 0;
}

const float *ks_entities_live_tris(const Game *game, const int i, size_t *tri_count)
{
    static const float none[1] = {0};
    const KsGame *kg = game ? game->ks : NULL;
    if (tri_count) *tri_count = 0;
    if (!kg || i < 0 || (size_t) i >= kg->ents.live_count) return NULL;
    const KsLiveObject *lo = &kg->ents.live[i];
    if (!lo->tris) return none;
    if (tri_count) *tri_count = lo->tri_count;
    return lo->tris;
}

// The live object wrapper (:209050): id, delete()
static KsValue live_id(KsScript *s, void *n, uint32_t id)
{
    (void) s; (void) id;
    KsRef *r = n;
    return r && r->kind == KS_REF_LIVE ? ks_num(r->id) : ks_undef();
}

static KsValue live_delete(KsScript *s, void *n, const KsValue *args, const int argc)
{
    (void) s; (void) args; (void) argc;
    KsRef *r = n;
    if (r && r->kind == KS_REF_LIVE && r->kg && server_side(r->kg)) live_remove(r->kg, r->id);
    return ks_undef();
}

static const KsHostProp g_live_props[] = {{"id", live_id, NULL, NULL, 0}, {"delete", NULL, NULL, live_delete, 0}};
static const KsHostClass g_live_class = {"liveObject", g_live_props, 2};

static int value_is_number(const KsValue v) { return v.tag == KS_NUM && isfinite(v.as.num); } // iiîiìîî

// The authority keeps `data` as live object liveObjects.id++ (200 at most) and broadcasts it in "lo"; the wrapper,
// 0 when refused (v10's liveObject(undefined) throws: the API's catch returns 0)
static KsValue live_publish(KsScript *s, KsGame *kg, cJSON *data)
{
    if (!data || kg->ents.live_count >= LIVE_OBJECTS_MAX) {
        cJSON_Delete(data);
        return ks_num(0);
    }
    KsLiveObject *lo = live_add(kg, kg->ents.live_next_id++, data);
    if (!lo) return ks_num(0);
    net_json(kg, NET_TARGET_ALL, NET_EXT_KN_OFFICIAL_LIVE_OBJECT, live_record(lo));
    KsRef *ref = ks_ref_get(kg, KS_REF_LIVE, lo->id);
    return ref ? ks_host_object(s, &g_live_class, ref, (uint32_t) lo->id) : ks_num(0);
}

// addAsset(assetId, x, y, z, scale, rotY, opts) (:211292): the server's; the id's digits, numbers only (else 0 /
// scale 1), the scale clamped to 0.01..100, opts.complexCollision as data.cpx. liveObjects.addAsset (:212144)
// keeps {type: "asset", aid, x, y, z, s, ry, data}.
static KsValue api_live_add_asset(KsScript *s, void *u, const KsValue *args, int argc)
{
    KsGame *kg = u;
    if (!kg || !authority(kg)) return ks_undef();
    char raw[64] = "", aid[64] = "";
    if (argc > 0) ks_to_string(s, args[0], raw, sizeof(raw));
    size_t n = 0;
    for (size_t i = 0; raw[i] && n + 1 < sizeof(aid); i++)
        if (raw[i] >= '0' && raw[i] <= '9') aid[n++] = raw[i];
    aid[n] = 0;
    if (!aid[0]) return ks_undef();
    const KsValue opts = argc > 6 && args[6].tag == KS_OBJ ? args[6] : ks_undef();
    const int cpx = opts.tag == KS_OBJ && value_truthy(ks_obj_get(s, opts, "complexCollision"));
    cJSON *data = cJSON_CreateObject(), *inner = cJSON_CreateObject();
    if (!data || !inner) {
        cJSON_Delete(data);
        cJSON_Delete(inner);
        return ks_num(0);
    }
    const double x = argc > 1 && value_is_number(args[1]) ? args[1].as.num : 0.0;
    const double y = argc > 2 && value_is_number(args[2]) ? args[2].as.num : 0.0;
    const double z = argc > 3 && value_is_number(args[3]) ? args[3].as.num : 0.0;
    const double sc = argc > 4 && value_is_number(args[4]) ? clamp_d(args[4].as.num, 0.01, 100.0) : 1.0;
    const double ry = argc > 5 && value_is_number(args[5]) ? args[5].as.num : 0.0;
    cJSON_AddStringToObject(data, "type", "asset");
    cJSON_AddStringToObject(data, "aid", aid);
    cJSON_AddNumberToObject(data, "x", x);
    cJSON_AddNumberToObject(data, "y", y);
    cJSON_AddNumberToObject(data, "z", z);
    cJSON_AddNumberToObject(data, "s", sc);
    cJSON_AddNumberToObject(data, "ry", ry);
    cJSON_AddBoolToObject(inner, "cpx", cpx);
    cJSON_AddItemToObject(data, "data", inner);
    return live_publish(s, kg, data);
}

// /^#([0-9A-F]{3}){1,2}$/i (iîiïiíì :27469)
static int hex_colour(const char *c)
{
    const size_t n = c ? strlen(c) : 0;
    if ((n != 4 && n != 7) || c[0] != '#') return 0;
    for (size_t i = 1; i < n; i++)
        if (!((c[i] >= '0' && c[i] <= '9') || (c[i] >= 'a' && c[i] <= 'f') || (c[i] >= 'A' && c[i] <= 'F'))) return 0;
    return 1;
}

// addCube(x, y, z, w, h, l, _, opts) (:211252-211290): the server's; numbers only (else 0, the sizes 1); the block's
// props: src wall_0, scale 1, noGroup, opts.opacity (a number, else 1), fTrans = !!forceTransparency, opts.aid's
// texture or video (src / video / asset), colr = opts.color when a hex colour, else #FFF. liveObjects.addBlock
// (:212115) keeps {type: "block", x, y, z, w, l, h, s: [1, 1, 1, 1, 1, 1], data: props}; it collides (a server map
// object) as its props carry no noCol.
static KsValue api_live_add_cube(KsScript *s, void *u, const KsValue *args, int argc)
{
    KsGame *kg = u;
    if (!kg || !authority(kg)) return ks_undef();
    const double x = argc > 0 && value_is_number(args[0]) ? args[0].as.num : 0.0;
    const double y = argc > 1 && value_is_number(args[1]) ? args[1].as.num : 0.0;
    const double z = argc > 2 && value_is_number(args[2]) ? args[2].as.num : 0.0;
    const double w = argc > 3 && value_is_number(args[3]) ? args[3].as.num : 1.0;
    const double h = argc > 4 && value_is_number(args[4]) ? args[4].as.num : 1.0;
    const double l = argc > 5 && value_is_number(args[5]) ? args[5].as.num : 1.0;
    const KsValue opts = argc > 7 && args[7].tag == KS_OBJ ? args[7] : ks_undef();
    cJSON *data = cJSON_CreateObject(), *props = cJSON_CreateObject();
    if (!data || !props) {
        cJSON_Delete(data);
        cJSON_Delete(props);
        return ks_num(0);
    }
    cJSON_AddStringToObject(props, "src", "wall_0");
    cJSON_AddNumberToObject(props, "scale", 1);
    cJSON_AddBoolToObject(props, "noGroup", 1);
    const KsValue opacity = opts.tag == KS_OBJ ? ks_obj_get(s, opts, "opacity") : ks_undef();
    cJSON_AddNumberToObject(props, "opacity", value_is_number(opacity) ? opacity.as.num : 1.0);
    cJSON_AddBoolToObject(props, "fTrans", opts.tag == KS_OBJ && value_truthy(ks_obj_get(s, opts, "forceTransparency")));
    const KsValue aid = opts.tag == KS_OBJ ? ks_obj_get(s, opts, "aid") : ks_undef();
    if (aid.tag != KS_UNDEF && aid.tag != KS_NULL) {
        char text[64];
        ks_to_string(s, aid, text, sizeof(text));
        char *end = NULL;
        const long id = strtol(text, &end, 10); // parseInt
        const int parsed = end != text;
        // aid != parseInt(aid): a string with more than the number (a numeric string or a number compares equal)
        const int differs = aid.tag == KS_STR ? !parsed || *end != 0 : aid.tag == KS_NUM && (!parsed || (double) id != aid.as.num);
        if (differs) {
            const int video = strchr(text, 'v') != NULL && parsed;
            char src[48];
            snprintf(src, sizeof(src), "%ld%s", parsed ? id : 0L, video ? "/video" : "/texture");
            cJSON_ReplaceItemInObjectCaseSensitive(props, "src", cJSON_CreateString(src));
            if (video) cJSON_AddNumberToObject(props, "video", (double) id);
            if (parsed) cJSON_AddNumberToObject(props, "asset", (double) id);
            else cJSON_AddNullToObject(props, "asset"); // NaN
        } else if (parsed) {
            char src[48];
            snprintf(src, sizeof(src), "%ld/texture", id);
            cJSON_ReplaceItemInObjectCaseSensitive(props, "src", cJSON_CreateString(src));
            cJSON_AddNumberToObject(props, "asset", (double) id);
        }
    }
    const KsValue color = opts.tag == KS_OBJ ? ks_obj_get(s, opts, "color") : ks_undef();
    char colr[16] = "#FFF";
    if (color.tag == KS_STR) {
        char text[16];
        ks_to_string(s, color, text, sizeof(text));
        if (hex_colour(text)) snprintf(colr, sizeof(colr), "%s", text);
    }
    cJSON_AddStringToObject(props, "colr", colr);
    cJSON_AddStringToObject(data, "type", "block");
    cJSON_AddNumberToObject(data, "x", x);
    cJSON_AddNumberToObject(data, "y", y);
    cJSON_AddNumberToObject(data, "z", z);
    cJSON_AddNumberToObject(data, "w", w);
    cJSON_AddNumberToObject(data, "l", l);
    cJSON_AddNumberToObject(data, "h", h);
    cJSON_AddItemToObject(data, "s", cJSON_CreateIntArray((const int[]) {1, 1, 1, 1, 1, 1}, 6));
    cJSON_AddItemToObject(data, "data", props);
    return live_publish(s, kg, data);
}

// ---------------------------------------------------------------------------
// GAME.ENTITIES (:211768-211824)

static KsGame *ent_api_kg(void *user) { return user; }

static KsValue wrap_list(KsScript *s, KsGame *kg)
{
    KsValue list = ks_arr_new(s);
    for (size_t i = 0; i < kg->ents.count; i++) ks_arr_push(s, list, wrap_entity(s, kg, kg->ents.items[i]));
    return list;
}

// parseInt(ToString(v)): the id, or -1 when it is not a number (no entity has a negative id)
static int id_arg(KsScript *s, const KsValue v)
{
    char text[64];
    ks_to_string(s, v, text, sizeof(text));
    char *end = NULL;
    const long id = strtol(text, &end, 10);
    return end == text || id < 0 || id > 2147483647L ? -1 : (int) id;
}

static KsValue api_ent_list(KsScript *s, void *u, const KsValue *args, int argc)
{
    (void) args; (void) argc;
    KsGame *kg = ent_api_kg(u);
    return kg ? wrap_list(s, kg) : ks_arr_new(s);
}

static KsValue api_ent_find_by_id(KsScript *s, void *u, const KsValue *args, int argc)
{
    KsGame *kg = ent_api_kg(u);
    if (!kg || argc < 1) return ks_undef();
    const int eid = id_arg(s, args[0]);
    KsEntity *e = eid >= 0 ? ent_find(kg, eid) : NULL;
    return e ? wrap_entity(s, kg, e) : ks_undef();
}

static KsValue api_ent_find_by_tag(KsScript *s, void *u, const KsValue *args, int argc)
{
    KsGame *kg = ent_api_kg(u);
    KsValue list = ks_arr_new(s);
    if (!kg || argc < 1) return list;
    char tag[64];
    ks_to_string(s, args[0], tag, sizeof(tag));
    for (size_t i = 0; i < kg->ents.count; i++)
        if (!strcmp(kg->ents.items[i]->tag, tag)) ks_arr_push(s, list, wrap_entity(s, kg, kg->ents.items[i]));
    return list;
}

// spawn(archNameOrId, x, y, z, params): params.tag names the entity; the rest are its spawn params.
static KsValue api_ent_spawn(KsScript *s, void *u, const KsValue *args, int argc)
{
    KsGame *kg = ent_api_kg(u);
    if (!kg || argc < 1 || !authority(kg) || !has_hooks(kg) || !kg->game->map) return ks_undef();
    char name[64];
    ks_to_string(s, args[0], name, sizeof(name));
    const MapArchetype *arch = NULL;
    for (size_t i = 0; i < kg->game->map->archetype_count && !arch; i++)
        if (!strcmp(kg->game->map->archetypes[i].name, name)) arch = &kg->game->map->archetypes[i];
    if (!arch) {
        const int id = id_arg(s, args[0]);
        if (id >= 0) arch = map_archetype(kg->game->map, id);
    }
    if (!arch) return ks_undef();
    cJSON *params = argc > 4 && args[4].tag == KS_OBJ ? json_object_from_value(s, args[4], 1) : cJSON_CreateObject();
    if (!params) return ks_undef();
    char tag[MAP_ENTITY_TAG_MAX + 1] = "";
    const cJSON *tag_item = cJSON_GetObjectItemCaseSensitive(params, "tag");
    const int has_tag = tag_item != NULL;
    if (has_tag) {
        if (cJSON_IsString(tag_item)) snprintf(tag, sizeof(tag), "%.32s", tag_item->valuestring);
        cJSON_DeleteItemFromObjectCaseSensitive(params, "tag");
    }
    KsEntity *e = ent_spawn(kg, arch, 0, argc > 1 ? ks_to_float(args[1]) : 0.0, argc > 2 ? ks_to_float(args[2]) : 0.0,
                            argc > 3 ? ks_to_float(args[3]) : 0.0, params, has_tag ? tag : NULL);
    cJSON_Delete(params);
    return e ? wrap_entity(s, kg, e) : ks_undef();
}

// clone(entity | id): the same archetype, tag and spawn params, at the entity's position.
static KsValue api_ent_clone(KsScript *s, void *u, const KsValue *args, int argc)
{
    KsGame *kg = ent_api_kg(u);
    if (!kg || argc < 1 || !authority(kg) || !has_hooks(kg)) return ks_undef();
    KsValue id_v = args[0];
    if (args[0].tag == KS_OBJ || args[0].tag == KS_HOST) id_v = ks_obj_get(s, args[0], "id");
    const int eid = id_arg(s, id_v);
    KsEntity *src = eid >= 0 ? ent_find(kg, eid) : NULL;
    if (!src) return ks_undef();
    KsEntity *e = ent_spawn(kg, src->arch, 0, src->x, src->y, src->z, src->params, src->tag);
    return e ? wrap_entity(s, kg, e) : ks_undef();
}

static KsValue api_ent_despawn(KsScript *s, void *u, const KsValue *args, int argc)
{
    KsGame *kg = ent_api_kg(u);
    if (!kg || argc < 1 || !authority(kg)) return ks_undef();
    const int eid = id_arg(s, args[0]);
    KsEntity *e = eid >= 0 ? ent_find(kg, eid) : NULL;
    if (e) ent_despawn(kg, e, "script");
    return ks_undef();
}

void ks_entities_api_fill(KsApiImpl *api, const KsSide side)
{
    api[KS_API_ENTITIES_LIST].fn = api_ent_list;
    api[KS_API_ENTITIES_FINDBYID].fn = api_ent_find_by_id;
    api[KS_API_ENTITIES_FINDBYTAG].fn = api_ent_find_by_tag;
    if (side == KS_SIDE_SERVER) {
        api[KS_API_ENTITIES_SPAWN].fn = api_ent_spawn;
        api[KS_API_ENTITIES_CLONE].fn = api_ent_clone;
        api[KS_API_ENTITIES_DESPAWN].fn = api_ent_despawn;
        api[KS_API_LIVE_OBJECTS_ADDASSET].fn = api_live_add_asset;
        api[KS_API_LIVE_OBJECTS_ADDCUBE].fn = api_live_add_cube;
    }
}
