// KrunkNative Object Manager (shared/include/object_manager.h): the rules, the authority's request
// handling, the extra loadout weapons and the spawned map objects (both sides). Not a v7 feature.
#include <object_manager.h>
#include <game_config.h>
#include <modes.h>
#include <prefabs.h>
#include <weapons.h>
#include <world.h>
#include <cJSON.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t kind; // ObjectManagerKind
    int32_t id;   // map object uid (OM_KIND_OBJECT)
    int owner;    // player uid
} OmThing;

typedef struct ObjectManager_t {
    cJSON *records;    // array owning the spawned objects' records (Object.raw points into it)
    OmThing *things;   // every spawn of this round, oldest first (undo, caps)
    size_t thing_count, thing_capacity;
} ObjectManager;

static ObjectManager *om_of(Game *game) {
    if (!game->object_manager) game->object_manager = calloc(1, sizeof(ObjectManager));
    ObjectManager *om = game->object_manager;
    if (om && !om->records) om->records = cJSON_CreateArray();
    return om && om->records ? om : NULL;
}

static const Weapon *om_weapon(const Game *game, const int id) {
    const size_t count = game && game->weapon_count ? game->weapon_count : WEAPON_COUNT;
    Weapon *const *all = game && game->weapons ? game->weapons : (Weapon *const *) g_weapons;
    return id >= 0 && (size_t) id < count ? all[id] : NULL;
}

// ---------------------------------------------------------------- catalogue

int object_manager_weapon_listed(const int weapon_id) {
    if (weapon_id < 0 || weapon_id >= WEAPON_COUNT || !g_weapons[weapon_id]) return 0;
    return weapon_id != WEAPON_ID_KNIFE && !g_weapons[weapon_id]->bomb; // every class has the knife; bombs are site-mode only
}

// ---------------------------------------------------------------- rules

int object_manager_keep_extras(const Game *game, const ObjectManagerPerms *perms) {
    return (object_manager_caps(game, perms) & OM_CAP_WEAPONS) != 0;
}

// Extra weapons: devs anywhere; everyone else in the offline sandbox, and in custom / hosted games
// while the host leaves knOmWeps on; never in public matchmade or ranked games. Objects, entities
// and NPCs: devs, the custom game's host and the offline sandbox.
unsigned int object_manager_caps(const Game *game, const ObjectManagerPerms *perms) {
    if (!game) return 0;
    const int dev = perms && perms->dev;
    const int host = perms && perms->host;
    const int ranked = (perms && perms->ranked) || game->config.comp;
    unsigned int caps = dev ? OM_CAP_DEV : 0;
    if (dev || (!ranked && (game->is_sandbox || (game_config_is_custom(game) && game->config.om_weapons)))) caps |= OM_CAP_WEAPONS;
    if (dev || (host && !ranked) || game->is_sandbox) caps |= OM_CAP_SPAWN;
    return caps;
}

const char *object_manager_status_text(const int status) {
    switch (status) {
        case OM_STATUS_OK: return "";
        case OM_STATUS_DENIED: return "Not allowed in this game";
        case OM_STATUS_LIMIT: return "Limit reached";
        case OM_STATUS_RATE: return "Slow down";
        case OM_STATUS_INVALID: return "Unknown item";
        case OM_STATUS_NO_TARGET: return "Aim at a surface";
        case OM_STATUS_NOTHING: return "Nothing to remove";
        case OM_STATUS_DEAD: return "Spawn in first";
        default: return "Failed";
    }
}

// ---------------------------------------------------------------- extra loadout weapons

static int loadout_has(const Player *player, const int count, const int id) {
    for (int i = 0; player->loadout && i < count; i++) {
        if (player->loadout[i] == id) return 1;
    }
    return 0;
}

// loadout_base as recorded (player_loadout_base reads the whole loadout while there are no extras)
static int raw_base(const Player *player) {
    return player->loadout_base > 0 && player->loadout_base <= player->loadout_size ? player->loadout_base : player->loadout_size;
}

// The base (slots [0, loadout_base)) + every extra weapon it does not hold already, with fresh
// ammo (mode_ammo_limit); the base's arrays are reused. Called where the base is (re)built.
void player_loadout_append_extras(Player *player) {
    if (!player || !player->loadout) return;
    const int base = raw_base(player);
    if (player->game && player->game->mode && player->game->mode->config.no_weapons) {
        player->loadout_base = base;
        return; // a weaponless mode (Prop Hunt props, ...) stays weaponless
    }
    int add[PLAYER_EXTRA_WEAPONS_MAX];
    int add_count = 0;
    for (int i = 0; i < player->extra_weapon_count && i < PLAYER_EXTRA_WEAPONS_MAX; i++) {
        const int id = player->extra_weapons[i];
        if (!om_weapon(player->game, id) || loadout_has(player, base, id)) continue;
        int dup = 0;
        for (int k = 0; k < add_count; k++) dup |= add[k] == id;
        if (!dup) add[add_count++] = id;
    }
    const int size = base + add_count;
    if (size != player->loadout_size) {
        int *loadout = realloc(player->loadout, (size_t) (size ? size : 1) * sizeof(int));
        if (loadout) player->loadout = loadout;
        unsigned int *ammo = realloc(player->ammo, (size_t) (size ? size : 1) * sizeof(unsigned int));
        if (ammo) player->ammo = ammo;
        float *reloads = realloc(player->reloads, (size_t) (size ? size : 1) * sizeof(float));
        if (reloads) player->reloads = reloads;
        if (!loadout || !ammo || !reloads) { // keep the base only
            player->loadout_size = base;
            player->loadout_base = base;
            return;
        }
    }
    for (int i = 0; i < add_count; i++) {
        const Weapon *w = om_weapon(player->game, add[i]);
        player->loadout[base + i] = add[i];
        player->ammo[base + i] = (unsigned int) mode_ammo_limit(player->game, w ? (int) w->ammo : 0);
        player->reloads[base + i] = 0.0f;
    }
    player->loadout_size = size;
    player->loadout_base = base;
}

void player_set_extra_weapons(Player *player, const int *weapon_ids, const int count) {
    if (!player) return;
    int list[PLAYER_EXTRA_WEAPONS_MAX];
    int n = 0;
    for (int i = 0; weapon_ids && i < count && n < PLAYER_EXTRA_WEAPONS_MAX; i++) {
        const int id = weapon_ids[i];
        if (!object_manager_weapon_listed(id) || !om_weapon(player->game, id)) continue;
        int dup = 0;
        for (int k = 0; k < n; k++) dup |= list[k] == id;
        if (!dup) list[n++] = id;
    }
    const int same = n == player->extra_weapon_count && !memcmp(list, player->extra_weapons, (size_t) n * sizeof(int));
    const int base = player->loadout ? raw_base(player) : 0; // before the list changes
    memcpy(player->extra_weapons, list, (size_t) n * sizeof(int));
    player->extra_weapon_count = n;
    if (same || !player->loadout || !player->active) return;

    // keep each kept extra's ammo; a removed slot in hand falls back to slot 0
    int old_ids[PLAYER_EXTRA_WEAPONS_MAX];
    unsigned int old_ammo[PLAYER_EXTRA_WEAPONS_MAX];
    int old_count = 0;
    for (int i = base; i < player->loadout_size && old_count < PLAYER_EXTRA_WEAPONS_MAX; i++, old_count++) {
        old_ids[old_count] = player->loadout[i];
        old_ammo[old_count] = player->ammo ? player->ammo[i] : 0;
    }
    const int held = player->loadout_index >= 0 && player->loadout_index < player->loadout_size ? player->loadout[player->loadout_index] : -1;

    game_player_changing(player->game, player, 0); // the offline client drops the old loadout's meshes
    player->loadout_size = base;
    player_loadout_append_extras(player);
    for (int i = base; i < player->loadout_size; i++) {
        for (int k = 0; k < old_count; k++) {
            if (old_ids[k] == player->loadout[i]) player->ammo[i] = old_ammo[k];
        }
    }
    int index = 0;
    for (int i = 0; i < player->loadout_size; i++) {
        if (player->loadout[i] == held) index = i;
    }
    player->loadout_index = -1; // re-equip (the slot may hold another weapon now)
    player_swap_weapon(player, index, 1, 1, 0);
    game_player_changing(player->game, player, 1);
}

void object_manager_send_extras(Game *game, const Player *player, const int target) {
    if (!game || !player) return;
    NetLoadoutExtraPacket packet;
    memset(&packet, 0, sizeof(packet));
    packet.uid = player->uid;
    for (int i = 0; i < player->extra_weapon_count && i < PLAYER_EXTRA_WEAPONS_MAX; i++) packet.weapons[packet.count++] = (uint8_t) player->extra_weapons[i];
    game_send(game, target, NET_PACKET_LOADOUT_EXTRA, &packet, sizeof(packet));
}

// ---------------------------------------------------------------- crosshair

static vec3 om_look(const float pitch, const float yaw) {
    return (vec3) {sinf(yaw + (float) M_PI) * cosf(pitch), sinf(pitch), cosf(yaw + (float) M_PI) * cosf(pitch)};
}

static int om_ray(const Game *game, const vec3 origin, const vec3 segment, float *best_t, int *best_object) {
    *best_t = 2.0f;
    *best_object = -1;
    const float terrain_t = world_terrain_fraction(game, origin, segment);
    if (terrain_t > 0.0f && terrain_t <= 1.0f) *best_t = terrain_t;
    for (size_t i = 0; game->map && i < game->map->object_count; i++) {
        const Object *o = game->map->objects[i];
        if (!o->active || o->no_shoot || o->collision_type == COLLISION_TYPE_NONE) continue;
        if (o->scale.x <= 0.0f || o->scale.y <= 0.0f || o->scale.z <= 0.0f) continue;
        const float t = line_in_rect(origin, segment, o->position, o->scale);
        if (t > 0.0f && t <= 1.0f && t < *best_t) {
            *best_t = t;
            *best_object = (int) i;
        }
    }
    return *best_t <= 1.0f;
}

int object_manager_aim(const Game *game, const Player *player, const float pitch, const float yaw, ObjectManagerAim *out) {
    if (!game || !game->map || !player || !out) return 0;
    const vec3 eye = {player->position.x, player->position.y + player->height - game_constants.camera_height, player->position.z};
    const vec3 dir = om_look(isfinite(pitch) ? player_clamp_pitch(pitch) : player->direction.x, isfinite(yaw) ? yaw : player->direction.y);
    const vec3 segment = {dir.x * OM_AIM_RANGE, dir.y * OM_AIM_RANGE, dir.z * OM_AIM_RANGE};
    float t;
    int object;
    if (!om_ray(game, eye, segment, &t, &object)) return 0;
    out->point = (vec3) {eye.x + segment.x * t, eye.y + segment.y * t, eye.z + segment.z * t};
    out->object = object;
    out->normal = (vec3) {0.0f, 1.0f, 0.0f};
    if (object >= 0) { // the face of the box nearest to the hit point
        const Object *o = game->map->objects[object];
        const float d[6] = {
            fabsf(out->point.y - (o->position.y + o->scale.y)), fabsf(out->point.y - o->position.y),
            fabsf(out->point.x - (o->position.x + o->scale.x * 0.5f)), fabsf(out->point.x - (o->position.x - o->scale.x * 0.5f)),
            fabsf(out->point.z - (o->position.z + o->scale.z * 0.5f)), fabsf(out->point.z - (o->position.z - o->scale.z * 0.5f)),
        };
        const vec3 normals[6] = {{0, 1, 0}, {0, -1, 0}, {1, 0, 0}, {-1, 0, 0}, {0, 0, 1}, {0, 0, -1}};
        int face = 0;
        for (int i = 1; i < 6; i++) {
            if (d[i] < d[face]) face = i;
        }
        out->normal = normals[face];
    }
    return 1;
}

// A floor point for things that stand (pickups, NPCs): the hit itself on a top face, else a step
// off the face, dropped onto whatever is below (or left in the air when nothing is).
static vec3 om_floor_point(const Game *game, const ObjectManagerAim *aim, const float off) {
    if (aim->normal.y > 0.5f) return aim->point;
    vec3 p = {aim->point.x + aim->normal.x * off, aim->point.y + aim->normal.y * off, aim->point.z + aim->normal.z * off};
    const vec3 down = {0.0f, -OM_AIM_RANGE, 0.0f};
    float t;
    int object;
    if (om_ray(game, (vec3) {p.x, p.y + 0.5f, p.z}, down, &t, &object)) p.y = p.y + 0.5f + down.y * t;
    return p;
}

// ---------------------------------------------------------------- spawned map objects

static int thing_push(ObjectManager *om, const int kind, const int id, const int owner) {
    if (om->thing_count == om->thing_capacity) {
        const size_t cap = om->thing_capacity ? om->thing_capacity * 2 : 32;
        OmThing *grown = realloc(om->things, cap * sizeof(OmThing));
        if (!grown) return 0;
        om->things = grown;
        om->thing_capacity = cap;
    }
    om->things[om->thing_count++] = (OmThing) {(uint8_t) kind, id, owner};
    return 1;
}

static Object *spawned_object(const Game *game, const int uid) {
    if (!game->map || uid < 0 || (size_t) uid >= game->map->object_count) return NULL;
    Object *o = game->map->objects[uid];
    return o->spawned ? o : NULL;
}

static int thing_alive(const Game *game, const OmThing *t) {
    if (t->kind == OM_KIND_OBJECT) {
        const Object *o = spawned_object(game, t->id);
        return o && o->active;
    }
    return 0;
}

// Live things of `owner` (-1: everyone) of `kind`, pickups only when `pickups`.
static int things_count(const Game *game, const ObjectManager *om, const int owner, const int kind, const int pickups) {
    int n = 0;
    for (size_t i = 0; om && i < om->thing_count; i++) {
        const OmThing *t = &om->things[i];
        if (t->kind != kind || (owner >= 0 && t->owner != owner) || !thing_alive(game, t)) continue;
        if (pickups && !spawned_object(game, t->id)->pickup) continue;
        n++;
    }
    return n;
}

static void spawned_send(Game *game, const int target, const int op, const int uid, const Object *o) {
    uint8_t buffer[sizeof(NetSpawnedObjPacket) + OM_RECORD_MAX + 1];
    NetSpawnedObjPacket head = {.op = (uint8_t) op, .kind = OM_KIND_OBJECT, .id = uid, .owner = o ? o->spawn_owner : -1};
    memcpy(buffer, &head, sizeof(head));
    uint16_t length = sizeof(head);
    if (op == OM_SPAWNED_ADD) {
        char *json = o && o->raw ? cJSON_PrintUnformatted(o->raw) : NULL;
        const size_t n = json ? strlen(json) : 0;
        if (!json || n > OM_RECORD_MAX) {
            free(json);
            return;
        }
        memcpy(buffer + length, json, n + 1);
        length = (uint16_t) (length + n + 1);
        free(json);
    }
    game_send(game, target, NET_PACKET_SPAWNED_OBJ, buffer, length);
}

// Appends the record as a spawned object at uid `uid` (the next one on the authority). The record
// joins the round's records (owned there from now on, even when this fails). -1: failed.
static int spawned_add(Game *game, ObjectManager *om, cJSON *record, const int owner, const int uid) {
    cJSON_AddItemToArray(om->records, record);
    Object *object = map_object_from_json(game->map, record, !game->server);
    if (!object) return -1;
    object->spawned = 1;
    object->spawn_owner = owner;
    object->destroyed_by = -1;
    if (game_is_authority(game)) {
        Object **grown = realloc(game->map->objects, (game->map->object_count + 1) * sizeof(Object *));
        if (!grown) {
            map_object_free(object);
            return -1;
        }
        game->map->objects = grown;
        game->map->objects[game->map->object_count++] = object;
    } else if (uid < 0 || !world_put_object(game, (size_t) uid, object)) {
        map_object_free(object);
        return -1;
    }
    const int at = game_is_authority(game) ? (int) game->map->object_count - 1 : uid;
    if (object->pickup) game_pickup_added(game, at); // grows the trigger runtime to the new uid
    return at;
}

// Deactivates a spawned object (uids never move mid-round: it stays as an inactive slot).
static void spawned_remove(Game *game, const int uid) {
    Object *o = spawned_object(game, uid);
    if (!o || !o->active) return;
    o->active = 0;
    o->destroyed = 1;
    o->collision_type = COLLISION_TYPE_NONE;
    if (o->pickup) game_pickup_disable(game, uid);
    if (game_is_authority(game)) spawned_send(game, NET_TARGET_ALL, OM_SPAWNED_REMOVE, uid, o);
}

void object_manager_send_state(Game *game, const int uid) {
    for (size_t i = 0; game && game->map && i < game->map->object_count; i++) {
        const Object *o = game->map->objects[i];
        if (o->spawned && o->active) spawned_send(game, uid, OM_SPAWNED_ADD, (int) i, o);
    }
}

void object_manager_round_start(Game *game) {
    ObjectManager *om = game ? game->object_manager : NULL;
    if (!om) return;
    // world_remove_builds freed the spawned objects (they point into these records) already
    cJSON_Delete(om->records);
    om->records = NULL;
    om->thing_count = 0;
}

int object_manager_apply_spawned(Game *game, const void *payload, const uint16_t length) {
    if (!game || !game->ready || !game->map || length < sizeof(NetSpawnedObjPacket) || game_is_authority(game)) return 0;
    NetSpawnedObjPacket head;
    memcpy(&head, payload, sizeof(head));
    if (head.kind != OM_KIND_OBJECT) return 0;
    if (head.op == OM_SPAWNED_REMOVE) {
        spawned_remove(game, head.id);
        return 1;
    }
    if (head.op != OM_SPAWNED_ADD || length <= sizeof(head)) return 0;
    const char *json = (const char *) payload + sizeof(head);
    const size_t n = length - sizeof(head);
    if (json[n - 1] != 0) return 0;
    ObjectManager *om = om_of(game);
    cJSON *record = om ? cJSON_Parse(json) : NULL;
    if (!record) return 0;
    if (spawned_object(game, head.id)) spawned_remove(game, head.id); // a stale copy at that uid
    return spawned_add(game, om, record, head.owner, head.id) >= 0;
}

// ---------------------------------------------------------------- requests (authority)

static int om_take_token(Player *player) {
    const float now = player->game->now;
    if (player->om_tokens_at <= 0.0f || now < player->om_tokens_at) {
        player->om_tokens = OM_RATE_BURST;
        player->om_tokens_at = now > 0.0f ? now : 1e-6f;
    }
    player->om_tokens = fminf(OM_RATE_BURST, player->om_tokens + (now - player->om_tokens_at) * OM_RATE_PER_SECOND);
    player->om_tokens_at = now > 0.0f ? now : 1e-6f;
    if (player->om_tokens < 1.0f) return 0;
    player->om_tokens -= 1.0f;
    return 1;
}

static int om_add_weapon(Player *player, const int id, NetSpawnResultPacket *out) {
    if (!object_manager_weapon_listed(id) || !om_weapon(player->game, id)) return OM_STATUS_INVALID;
    for (int i = 0; i < player->extra_weapon_count; i++) {
        if (player->extra_weapons[i] == id) return OM_STATUS_OK; // already carried
    }
    if (player->extra_weapon_count >= PLAYER_EXTRA_WEAPONS_MAX) {
        out->limit = PLAYER_EXTRA_WEAPONS_MAX;
        return OM_STATUS_LIMIT;
    }
    int list[PLAYER_EXTRA_WEAPONS_MAX];
    memcpy(list, player->extra_weapons, (size_t) player->extra_weapon_count * sizeof(int));
    list[player->extra_weapon_count] = id;
    player_set_extra_weapons(player, list, player->extra_weapon_count + 1);
    object_manager_send_extras(player->game, player, NET_TARGET_ALL);
    return OM_STATUS_OK;
}

static int om_remove_weapon(Player *player, const int id, const int all) {
    int list[PLAYER_EXTRA_WEAPONS_MAX];
    int n = 0;
    for (int i = 0; i < player->extra_weapon_count; i++) {
        if (!all && player->extra_weapons[i] != id) list[n++] = player->extra_weapons[i];
    }
    if (n == player->extra_weapon_count) return all ? OM_STATUS_OK : OM_STATUS_NOTHING;
    player_set_extra_weapons(player, list, n);
    object_manager_send_extras(player->game, player, NET_TARGET_ALL);
    return OM_STATUS_OK;
}

static int om_drop_weapon(Game *game, Player *player, const NetSpawnReqPacket *req, NetSpawnResultPacket *out) {
    const int id = req->item;
    if (!object_manager_weapon_listed(id) || !om_weapon(game, id)) return OM_STATUS_INVALID;
    ObjectManager *om = om_of(game);
    if (!om || !game->map) return OM_STATUS_INVALID;
    if (things_count(game, om, player->uid, OM_KIND_OBJECT, 1) >= OM_PICKUPS_PER_PLAYER) {
        out->limit = OM_PICKUPS_PER_PLAYER;
        return OM_STATUS_LIMIT;
    }
    if (things_count(game, om, player->uid, OM_KIND_OBJECT, 0) >= OM_OBJECTS_PER_PLAYER) {
        out->limit = OM_OBJECTS_PER_PLAYER;
        return OM_STATUS_LIMIT;
    }
    if (things_count(game, om, -1, OM_KIND_OBJECT, 0) >= OM_OBJECTS_MAX) {
        out->limit = OM_OBJECTS_MAX;
        return OM_STATUS_LIMIT;
    }
    ObjectManagerAim aim;
    if (!object_manager_aim(game, player, req->pitch, req->yaw, &aim)) return OM_STATUS_NO_TARGET;
    const vec3 p = om_floor_point(game, &aim, 3.0f);
    const PrefabData *pd = prefab_data(PREFAB_WEAPON_PICKUP);
    // a map.js WEAPON_PICKUP record: the pad at the floor point, turned to face the player
    cJSON *record = cJSON_CreateObject();
    // doubles rounded like map.js (p .round(1), r .round(2)): the record prints short and parses back exactly
    const double pos[3] = {round(p.x * 10.0) / 10.0, round(p.y * 10.0) / 10.0, round(p.z * 10.0) / 10.0};
    const double size[3] = {pd ? pd->default_size[0] : 12.0, pd ? pd->default_size[1] : 2.0, pd ? pd->default_size[2] : 4.0};
    const double rot[3] = {0.0, round((player->direction.y + M_PI / 2.0) * 100.0) / 100.0, 0.0};
    cJSON_AddItemToObject(record, "p", cJSON_CreateDoubleArray(pos, 3));
    cJSON_AddItemToObject(record, "s", cJSON_CreateDoubleArray(size, 3));
    cJSON_AddItemToObject(record, "r", cJSON_CreateDoubleArray(rot, 3));
    cJSON_AddNumberToObject(record, "i", PREFAB_WEAPON_PICKUP);
    cJSON_AddNumberToObject(record, "w", id);
    const int uid = spawned_add(game, om, record, player->uid, -1);
    if (uid < 0) return OM_STATUS_INVALID;
    thing_push(om, OM_KIND_OBJECT, uid, player->uid);
    spawned_send(game, NET_TARGET_ALL, OM_SPAWNED_ADD, uid, game->map->objects[uid]);
    game_pickup_send(game, NET_TARGET_ALL, uid);
    return OM_STATUS_OK;
}

static int om_undo(Game *game, Player *player, const int any) {
    ObjectManager *om = game->object_manager;
    for (size_t i = om ? om->thing_count : 0; i-- > 0;) {
        const OmThing *t = &om->things[i];
        if ((!any && t->owner != player->uid) || !thing_alive(game, t)) continue;
        if (t->kind == OM_KIND_OBJECT) spawned_remove(game, t->id);
        return OM_STATUS_OK;
    }
    return OM_STATUS_NOTHING;
}

void object_manager_request(Game *game, Player *player, const NetSpawnReqPacket *req, const ObjectManagerPerms *perms,
                            NetSpawnResultPacket *out) {
    memset(out, 0, sizeof(*out));
    const unsigned int caps = object_manager_caps(game, perms);
    out->action = req->action;
    out->item = req->item;
    out->caps = (uint8_t) caps;
    out->extra_max = PLAYER_EXTRA_WEAPONS_MAX;
    out->status = OM_STATUS_OK;
    if (req->action == OM_ACTION_QUERY) return;
    if (req->action >= OM_ACTION_COUNT) {
        out->status = OM_STATUS_INVALID;
        return;
    }
    if (!player || !player->active || !game_is_authority(game)) {
        out->status = OM_STATUS_DEAD;
        return;
    }
    const int weapons = req->action <= OM_ACTION_DROP_WEAPON;
    if (!(caps & (weapons ? OM_CAP_WEAPONS : OM_CAP_SPAWN)) &&
        !(req->action == OM_ACTION_UNDO && (caps & OM_CAP_WEAPONS)) && // undo also takes back pickups
        !(req->action == OM_ACTION_REMOVE_WEAPON || req->action == OM_ACTION_CLEAR_WEAPONS)) { // giving back is always fine
        out->status = OM_STATUS_DENIED;
        return;
    }
    if (!om_take_token(player)) {
        out->status = OM_STATUS_RATE;
        return;
    }
    switch (req->action) {
        case OM_ACTION_ADD_WEAPON: out->status = (uint8_t) om_add_weapon(player, req->item, out); break;
        case OM_ACTION_REMOVE_WEAPON: out->status = (uint8_t) om_remove_weapon(player, req->item, 0); break;
        case OM_ACTION_CLEAR_WEAPONS: out->status = (uint8_t) om_remove_weapon(player, -1, 1); break;
        case OM_ACTION_DROP_WEAPON: out->status = (uint8_t) om_drop_weapon(game, player, req, out); break;
        case OM_ACTION_UNDO: out->status = (uint8_t) om_undo(game, player, 0); break;
        default: out->status = OM_STATUS_INVALID; break; // entities, NPCs, objects: parts 2 and 3
    }
}
