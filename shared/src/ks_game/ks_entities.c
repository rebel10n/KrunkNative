// KrunkScript entities (v10.0.2 entity manager, L184033-185900; doc/v10/map-format.md section 4).
// The authority (the server, and the offline sandbox's server core) runs them: the map's ENTITY
// instances spawn at the round start (spawnPending), each entity steps once per 33.33 ms of the
// round clock through the scripts.shared simulateEntity hook, and the entity hooks reach the main
// script after each advance (v10 queueServerHook / drainServerHooks). GAME.ENTITIES and the entity
// host objects are the script side. Not built here (see doc/v10 and krunkscript-gaps.md): the
// attachments (attachPlayer / detachPlayer / updateAttachment and riders), the entity bodies (buildBody,
// placeBody, collideWorld: an entity moves by its state alone), replication (e+ e- es ee ea+ ea-) and
// the client renderer, and the ctx.peer / ctx.peers lookups.
#include "ks_game_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ENT_CTX_EVENT_NAME_MAX KS_ENT_NAME_MAX

extern const KsHostClass ks_entity_class;     // the entity host object (below)
extern const KsHostClass ks_entity_ctx_class; // the simulateEntity ctx (below)

void ks_game_log_error(KsGame *kg, const char *line)
{
    if (kg && kg->host && kg->host->log) kg->host->log(kg->host_user, 1, line);
}

// ---------------------------------------------------------------------------
// Lookups and storage

static KsEntity *ent_find(KsGame *kg, const int eid)
{
    for (size_t i = 0; i < kg->ents.count; i++)
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

static int authority(const KsGame *kg) { return ks_game_is_authority_side(kg); }

static void ent_free(KsEntity *e)
{
    cJSON_Delete(e->state);
    cJSON_Delete(e->params);
    free(e);
}

// Removes an entity without any hook (v10 despawn: detaches, removes its body, leaves the map).
static void ent_remove(KsGame *kg, KsEntity *e)
{
    KsEntities *ents = &kg->ents;
    for (size_t i = 0; i < ents->count; i++) {
        if (ents->items[i] != e) continue;
        memmove(&ents->items[i], &ents->items[i + 1], (ents->count - i - 1) * sizeof(*ents->items));
        ents->count--;
        break;
    }
    e->active = 0;
    ent_free(e);
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

// ---------------------------------------------------------------------------
// Random streams (ctx.rand): v10 buildCtx's murmur-style seed of (matchSeed, eid, tick) and the
// generator of the step, word for word (L184670-184690).

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
// State: validation (v10 validateState, L184237) and the JSON / script value conversions

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
// state_check). `lenient` (spawn params) leaves such fields out instead.
static cJSON *json_from_value(KsScript *s, const KsValue v)
{
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

// The pose keys of a state (x y z yaw pitch roll; numbers only) into the entity.
static void commit_pose(KsEntity *e)
{
    const cJSON *v;
    if (!e->state) return;
    if ((v = cJSON_GetObjectItemCaseSensitive(e->state, "x")) && cJSON_IsNumber(v)) e->x = v->valuedouble;
    if ((v = cJSON_GetObjectItemCaseSensitive(e->state, "y")) && cJSON_IsNumber(v)) e->y = v->valuedouble;
    if ((v = cJSON_GetObjectItemCaseSensitive(e->state, "z")) && cJSON_IsNumber(v)) e->z = v->valuedouble;
    if ((v = cJSON_GetObjectItemCaseSensitive(e->state, "yaw")) && cJSON_IsNumber(v)) e->yaw = v->valuedouble;
    if ((v = cJSON_GetObjectItemCaseSensitive(e->state, "pitch")) && cJSON_IsNumber(v)) e->pitch = v->valuedouble;
    if ((v = cJSON_GetObjectItemCaseSensitive(e->state, "roll")) && cJSON_IsNumber(v)) e->roll = v->valuedouble;
}

// v10 applyStateWrite via setVar / setState: the write stands when the state stays valid, else it
// is undone (returns 0). `items` are owned by the call.
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
    if (ok && state_check(cand, err, sizeof(err))) ok = 0;
    if (!ok) {
        cJSON_Delete(cand);
        return 0;
    }
    cJSON_Delete(e->state);
    e->state = cand;
    commit_pose(e);
    (void) kg;
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

// The authority's entity manager runs with a shared script that is loaded and not failed.
static int manager_ready(const KsGame *kg)
{
    return authority(kg) && kg->shared_script && !ks_script_failed(kg->shared_script);
}

// ---------------------------------------------------------------------------
// Spawn, step, damage (v10 spawn L184284, stepEntity / commitStep L184820-184860, damage L185843)

static void bad_result(KsEntity *e)
{
    if (++e->errors >= KS_ENT_ERRORS) e->frozen = 1;
}

// Creates an entity of `arch` at (x, y, z) with the spawn params and tag. `eid` 0 takes the next id.
// NULL when refused: not the authority or no shared script, 64 entities, the id taken, or the
// onEntityInit state invalid (logged). A missing or failing onEntityInit leaves an empty state
// (v10: `{}` plus the spawn position).
static KsEntity *ent_spawn(KsGame *kg, const MapArchetype *arch, int eid, double x, double y, double z, const cJSON *params, const char *tag)
{
    KsEntities *ents = &kg->ents;
    if (!manager_ready(kg) || !arch) return NULL;
    if (ents->count >= MAP_ENTITY_LIMIT) return NULL;
    if (eid <= 0) eid = ents->next_eid++;
    if (ent_find(kg, eid)) return NULL;
    if (!isfinite(x)) x = 0.0;
    if (!isfinite(y)) y = 0.0;
    if (!isfinite(z)) z = 0.0;

    KsEntity *e = calloc(1, sizeof(*e));
    if (!e) return NULL;
    e->eid = eid;
    e->arch = arch;
    e->hp = arch->hp;
    e->active = 1;
    e->tick = (int) ents->tick;
    e->step_tick = (int) ents->tick;
    snprintf(e->tag, sizeof(e->tag), "%s", tag ? tag : "");
    e->params = params ? cJSON_Duplicate(params, 1) : cJSON_CreateObject();
    if (!e->params) {
        free(e);
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
        ks_game_log_error(kg, line);
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
        ks_game_log_error(kg, line);
        ent_free(e);
        return NULL;
    }
    e->state = state;
    commit_pose(e);
    if (!insert_entity(kg, e)) {
        ent_free(e);
        return NULL;
    }
    KsEntHook *h = queue_push(kg);
    if (h) {
        h->kind = KS_ENT_HOOK_SPAWN;
        h->eid = eid;
        snprintf(h->text, sizeof(h->text), "%s", arch->name);
    }
    return e;
}

// One simulateEntity step to `tick` (v10 stepEntity + commitStep): a non-object result, or one
// the state check refuses, counts as an error (logged when the check refused it), and ten in a row
// freeze the entity. The events of ctx.emit are kept for the advance to queue.
static void ent_step(KsGame *kg, const int eid, const long tick)
{
    KsEntity *e = ent_find(kg, eid);
    if (!e) return;
    KsScript *s = kg->shared_script;
    e->event_count = 0;
    e->step_tick = (int) tick;
    e->rng = ent_rand_seed(kg->ents.seed, eid, tick);

    char eid_text[24];
    snprintf(eid_text, sizeof(eid_text), "%d", eid);
    KsRef *ref = ks_ref_get(kg, KS_REF_ENTITY, eid);
    if (!ref) return;
    KsValue args[5] = {ks_str(s, eid_text, strlen(eid_text)), ks_str(s, e->arch->name, strlen(e->arch->name)), state_value(s, e->state),
                       ks_host_object(s, &ks_entity_ctx_class, ref, (uint32_t) eid), ks_num(KS_ENT_STEP_MS / 1000.0)};
    for (int i = 0; i < 5; i++) ks_handle_pin(s, args[i]);
    KsValue result = ks_undef();
    const int rc = ks_script_call_hook_value(s, "simulateEntity", args, 5, &result);
    for (int i = 4; i >= 0; i--) ks_handle_unpin(s, args[i]);

    e = ent_find(kg, eid); // the hook may have despawned it
    if (!e) return;
    e->tick = (int) tick;
    if (rc != 0 || result.tag != KS_OBJ) {
        bad_result(e);
        return;
    }
    char err[160];
    cJSON *next = NULL;
    if (state_from_value(s, result, &next, err, sizeof(err))) {
        char line[320];
        snprintf(line, sizeof(line), "simulateEntity(%s): %s", e->arch->name, err);
        ks_game_log_error(kg, line);
        bad_result(e);
        return;
    }
    e->errors = 0;
    cJSON_Delete(e->state);
    e->state = next;
    commit_pose(e);
}

// The authority's damage (v10 damage L185843): the hp falls, onEntityDamage is queued, and at hp <= 0
// onEntityDestroyed is queued and the entity despawns. Returns 1 when it was destroyed.
static int ent_damage(KsGame *kg, const int eid, const double amount, const char *by)
{
    KsEntity *e = ent_find(kg, eid);
    if (!authority(kg) || !e || !(e->arch->hp > 0.0f)) return 0;
    Player *doer = by && by[0] ? ks_game_find_player_str(kg->game, by) : NULL;
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
    if (e->hp > 0.0) return 0;
    h = queue_push(kg);
    if (h) {
        h->kind = KS_ENT_HOOK_DESTROYED;
        h->eid = eid;
        snprintf(h->doer, sizeof(h->doer), "%s", doer_id);
    }
    ent_remove(kg, e);
    return 1;
}

// ---------------------------------------------------------------------------
// The round's clock, the advance and the hook queue

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
    }
    e->event_count = 0;
}

// v10 advance (L185298): tick = floor(clock / 33.33); each active, unfrozen entity steps up to it
// (a stall of more than 64 ticks restarts at one step). Ids are snapshotted: a step may spawn or
// despawn.
static void ent_advance(KsGame *kg, const double delta_ms)
{
    KsEntities *ents = &kg->ents;
    ents->clock_ms += delta_ms;
    ents->tick = (long) floor(ents->clock_ms / KS_ENT_STEP_MS);
    if (!ents->count) return;
    const size_t n = ents->count;
    int *ids = malloc(n * sizeof(*ids));
    if (!ids) return;
    for (size_t i = 0; i < n; i++) ids[i] = ents->items[i]->eid;
    for (size_t i = 0; i < n; i++) {
        KsEntity *e = ent_find(kg, ids[i]);
        if (!e || !e->active || e->frozen) continue;
        const long behind = ents->tick - e->tick;
        if (behind <= 0) continue;
        if (behind > KS_ENT_CATCHUP) e->tick = (int) (ents->tick - 1);
        for (;;) {
            e = ent_find(kg, ids[i]);
            if (!e || e->frozen || e->tick >= ents->tick) break;
            ent_step(kg, ids[i], (long) e->tick + 1);
        }
        flush_events(kg, ids[i]);
    }
    free(ids);
}

// v10 drainServerHooks: each queued hook reaches the main script when it has that hook.
static void drain_queue(KsGame *kg)
{
    KsEntities *ents = &kg->ents;
    KsScript *main_script = ks_game_script(kg->game);
    while (ents->queue_head < ents->queue_count) {
        const KsEntHook h = ents->queue[ents->queue_head++];
        if (!main_script) continue;
        char eid_text[24], text_buf[KS_ENT_NAME_MAX + 1];
        snprintf(eid_text, sizeof(eid_text), "%d", h.eid);
        KsValue args[4];
        int argc = 0;
        const char *hook = NULL;
        KsScript *s = main_script;
        switch (h.kind) {
            case KS_ENT_HOOK_SPAWN:
                hook = "onEntitySpawn";
                snprintf(text_buf, sizeof(text_buf), "%s", h.text);
                args[0] = ks_str(s, eid_text, strlen(eid_text));
                args[1] = ks_str(s, text_buf, strlen(text_buf));
                argc = 2;
                break;
            case KS_ENT_HOOK_EVENT:
                hook = "onEntityEvent";
                snprintf(text_buf, sizeof(text_buf), "%s", h.text);
                args[0] = ks_str(s, eid_text, strlen(eid_text));
                args[1] = ks_str(s, text_buf, strlen(text_buf));
                args[2] = ks_num(h.a);
                args[3] = ks_num(h.b);
                argc = 4;
                break;
            case KS_ENT_HOOK_DAMAGE:
                hook = "onEntityDamage";
                args[0] = ks_str(s, eid_text, strlen(eid_text));
                args[1] = ks_str(s, h.doer, strlen(h.doer));
                args[2] = ks_num(h.a);
                argc = 3;
                break;
            case KS_ENT_HOOK_DESTROYED:
                hook = "onEntityDestroyed";
                args[0] = ks_str(s, eid_text, strlen(eid_text));
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

void ks_entities_reset(KsGame *kg)
{
    if (!kg) return;
    KsEntities *ents = &kg->ents;
    for (size_t i = 0; i < ents->count; i++) ent_free(ents->items[i]);
    free(ents->items);
    ents->items = NULL;
    ents->count = ents->cap = 0;
    ents->queue_head = ents->queue_count = 0; // hooks queued by the last round are dropped (v10 clear)
    ents->next_eid = KS_ENT_FIRST_EID;
    ents->clock_ms = 0.0;
    ents->tick = 0;
}

void ks_entities_free(KsGame *kg)
{
    if (!kg) return;
    ks_entities_reset(kg);
    free(kg->ents.queue);
    kg->ents.queue = NULL;
    kg->ents.queue_cap = 0;
}

// Round start (ks_game_start): the last round's entities go (no hooks), then the map's ENTITY
// instances with `as` != 0 spawn in map order (v10 spawnPending). The yaw of an instance is applied
// when its rotation y is non-zero (v10 `r[1] &&`).
void ks_entities_round_start(KsGame *kg)
{
    if (!kg) return;
    ks_entities_reset(kg);
    if (!manager_ready(kg) || !kg->game || !kg->game->map) return;
    const Map *map = kg->game->map;
    for (size_t i = 0; i < map->entity_instance_count; i++) {
        const MapEntityInstance *inst = &map->entity_instances[i];
        if (!inst->auto_spawn) continue;
        const MapArchetype *arch = map_archetype(map, inst->archetype);
        if (!arch) continue;
        KsEntity *e = ent_spawn(kg, arch, inst->eid, inst->position.x, inst->position.y, inst->position.z, inst->spawn_params,
                                inst->has_tag ? inst->tag : NULL);
        if (!e || !inst->rotation.y) continue;
        cJSON *yaw = cJSON_CreateNumber(inst->rotation.y);
        if (!yaw) continue;
        cJSON *items[1] = {yaw};
        const char *keys[1] = {"yaw"};
        state_write(kg, e, items, keys, 1);
    }
}

void ks_entities_update(KsGame *kg, const double delta_ms)
{
    if (!kg) return;
    if (manager_ready(kg)) ent_advance(kg, delta_ms);
    drain_queue(kg);
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

static KsValue ent_position(KsScript *s, void *n, uint32_t id);
static KsValue ent_rotation(KsScript *s, void *n, uint32_t id);

// position.x / y / z: a teleport (v10 teleport L185818-185830): the three pose keys are written
// together; a refused state leaves the entity where it was.
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
static KsGame *ks_native_game_kg(void *native) { KsRef *r = native; return r ? r->kg : NULL; }
POS_AXIS(pos_x, 0)
POS_AXIS(pos_y, 1)
POS_AXIS(pos_z, 2)

// rotation.x / y / z = pitch / yaw / roll: setVar (v10 L181798-181810).
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

// Entity methods (v10 entity wrapper L181810-181870). Writes are the authority's.
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
    // v10 Object.assign(state, obj): each key of the object is written, then the whole state is checked
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
        entity_set_var(kg, e, key, cJSON_CreateNumber(speed));
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

static KsValue m_despawn(KsScript *s, void *n, const KsValue *args, const int argc)
{
    (void) s; (void) args; (void) argc;
    KsEntity *e = ent_of(n);
    KsGame *kg = ks_native_game_kg(n);
    if (e && authority(kg)) ent_remove(kg, e);
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
    {"despawn", NULL, NULL, m_despawn, 0},
};
const KsHostClass ks_entity_class = {"entity", g_entity_props, (int) (sizeof(g_entity_props) / sizeof(g_entity_props[0]))};

static KsValue wrap_entity(KsScript *s, KsGame *kg, const KsEntity *e)
{
    KsRef *ref = e ? ks_ref_get(kg, KS_REF_ENTITY, e->eid) : NULL;
    return ref ? ks_host_object(s, &ks_entity_class, ref, (uint32_t) e->eid) : ks_undef();
}

// The simulateEntity ctx (v10 buildCtx / buildCtxBase): tick, rand, inputs (no controller), data,
// emit, raycast, groundY.
static KsValue ctx_tick(KsScript *s, void *n, uint32_t id) { (void) s; (void) id; KsEntity *e = ent_of(n); return e ? ks_num(e->step_tick) : ks_undef(); }
static KsValue ctx_inputs(KsScript *s, void *n, uint32_t id)
{
    (void) id;
    if (!ent_of(n)) return ks_undef();
    KsValue obj = ks_obj_new(s);
    static const char *const keys[] = {"active", "forward", "strafe", "jump", "crouch", "shoot", "interact", "yaw", "pitch"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) ks_obj_set(s, obj, keys[i], ks_num(0));
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
    if (!e || argc < 1 || e->event_count >= KS_ENT_EVENTS) return ks_undef();
    char name[256];
    ks_to_string(s, args[0], name, sizeof(name));
    KsEntEvent *ev = &e->events[e->event_count++];
    snprintf(ev->name, sizeof(ev->name), "%.*s", ENT_CTX_EVENT_NAME_MAX, name);
    const double a = argc > 1 ? ks_to_number(args[1]) : 0.0, b = argc > 2 ? ks_to_number(args[2]) : 0.0;
    ev->a = isnan(a) ? 0.0 : a;
    ev->b = isnan(b) ? 0.0 : b;
    return ks_undef();
}

// ctx.raycast(x, y, z, dx, dy, dz, dist): the static objects (no dyn ones), like v10's collisions raycast.
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

// groundY(x, z, y): the top of the static object below (x, y, z), else y - 1000 (v10 L184700-184710:
// the argument order is x, z, y).
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

static const KsHostProp g_ctx_props[] = {
    {"tick", ctx_tick, NULL, NULL, 0},
    {"inputs", ctx_inputs, NULL, NULL, 0},
    {"data", ctx_data, NULL, NULL, 0},
    {"rand", NULL, NULL, ctx_rand, 0},
    {"emit", NULL, NULL, ctx_emit, 0},
    {"raycast", NULL, NULL, ctx_raycast, 0},
    {"groundY", NULL, NULL, ctx_ground_y, 0},
};
const KsHostClass ks_entity_ctx_class = {"ctx", g_ctx_props, (int) (sizeof(g_ctx_props) / sizeof(g_ctx_props[0]))};

// ---------------------------------------------------------------------------
// GAME.ENTITIES (v10 L183661)

static Game *ent_api_game(void *user) { return user ? ((KsGame *) user)->game : NULL; }

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
    if (!kg || argc < 1 || !manager_ready(kg) || !kg->game->map) return ks_undef();
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
    if (!kg || argc < 1 || !manager_ready(kg)) return ks_undef();
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
    if (e) ent_remove(kg, e);
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
    }
}
