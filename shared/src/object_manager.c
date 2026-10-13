// KrunkNative Object Manager (shared/include/object_manager.h): the rules, the authority's request
// handling, the extra loadout weapons and the spawned map objects (both sides). Not a v7 feature.
#include <object_manager.h>
#include <ai.h>
#include <bots.h>
#include <map_ai.h>
#include <spawnables.h>
#include <zombies.h>
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

#define OM_SUB_AI 0        // OM_KIND_NPC: an AI (sid)
#define OM_SUB_BOT 1       //              a bot (GameBot.id)
#define OM_SUB_SPAWNABLE 0 // OM_KIND_ENTITY: a spawnable (sid)
#define OM_SUB_AIRDROP 1   //                 an airdrop (sid)

typedef struct {
    uint8_t kind; // ObjectManagerKind
    uint8_t sub;  // OM_SUB_* within the kind
    int32_t id;   // map object uid / sid / bot id
    int owner;    // player uid
    unsigned serial; // OM_SUB_AI: GameAI.serial (sids are reused once an AI is gone)
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

#define SCRAP(name, icon, json) {name, icon, OM_ENTRY_RECORD, 0, json}
// The Object Painter's scraps (v7 editor.js `scraps`, client/src/editor/editor_tools.c k_scraps): TRIGGER
// pickups with a user-asset model, here present at once (no "sd" start-destroyed, no "rsc" respawn chance)
#define SCRAP_TAS(sound) "\"tas\":[{\"esa\":29736,\"ev\":0.1,\"er\":1,\"ef\":2,\"ta\":34},{\"ta\":60,\"tct\":\"" sound "\"}]"
static const ObjectManagerEntry k_entities[] = {
    {"KR Tag", "toll", OM_ENTRY_SPAWNABLE, 3, NULL},
    {"Ammo Pack", "inventory_2", OM_ENTRY_SPAWNABLE, 6, NULL},
    {"Melee Pickup", "construction", OM_ENTRY_SPAWNABLE, 8, NULL},
    {"Ammo Cache", "inventory_2", OM_ENTRY_SPAWNABLE, 9, NULL},
    {"Nuke", "flare", OM_ENTRY_SPAWNABLE, 10, NULL},
    {"Minigun Power-up", "bolt", OM_ENTRY_SPAWNABLE, 11, NULL},
    {"Insta-kill", "dangerous", OM_ENTRY_SPAWNABLE, 12, NULL},
    {"Discount", "savings", OM_ENTRY_SPAWNABLE, 13, NULL},
    {"Double Points", "sports_score", OM_ENTRY_SPAWNABLE, 14, NULL},
    {"Grenade Launcher", "bolt", OM_ENTRY_SPAWNABLE, 15, NULL},
    {"Perk Bottle", "local_drink", OM_ENTRY_SPAWNABLE, 19, NULL},
    {"Free Upgrade", "upgrade", OM_ENTRY_SPAWNABLE, 22, NULL},
    {"Krunkanium", "diamond", OM_ENTRY_SPAWNABLE, 18, NULL},
    {"Frog", "pets", OM_ENTRY_SPAWNABLE, 17, NULL},
    {"Slime Pad", "science", OM_ENTRY_SPAWNABLE, 5, NULL},
    {"Mega Slime Pad", "science", OM_ENTRY_SPAWNABLE, 23, NULL},
    {"Airdrop", "redeem", OM_ENTRY_AIRDROP, -1, NULL},
    SCRAP("Junk", "recycling", "{\"s\":[8.29,3.15,8.6],\"i\":29,\"pl\":1,\"cdy\":1,\"tm\":0,\"su\":1,\"te\":16,\"sfd\":1,\"tas\":[{\"esa\":29736,\"ev\":0.1,\"er\":1,\"ef\":2,\"ta\":34},{\"ta\":59,\"tct\":\"0.01\"}],\"ms\":5,\"aid\":\"30082\",\"inm\":\"pickup Junk\"}"),
    SCRAP("Cloth", "texture", "{\"s\":[6.87,1.22,11.62],\"i\":29,\"pl\":1,\"cdy\":1,\"tm\":0,\"su\":1,\"te\":16,\"sfd\":1," SCRAP_TAS("3238") ",\"ms\":5,\"aid\":\"30162\",\"inm\":\"pickup Cloth\"}"),
    SCRAP("Wood", "forest", "{\"s\":[3.38,0.65,16.87],\"i\":29,\"pl\":1,\"cdy\":1,\"tm\":0,\"su\":1,\"te\":16,\"sfd\":1," SCRAP_TAS("3237") ",\"ms\":4,\"aid\":\"30061\",\"inm\":\"pickup Wood\"}"),
    SCRAP("Wires", "cable", "{\"s\":[12.52,4.18,7.76],\"i\":29,\"pl\":1,\"cdy\":1,\"tm\":0,\"su\":1,\"te\":16,\"sfd\":1," SCRAP_TAS("3239") ",\"ms\":4,\"aid\":\"30163\",\"inm\":\"pickup Wires\"}"),
    SCRAP("Tin Can", "delete", "{\"s\":[2.22,4.57,2.22],\"i\":29,\"pl\":1,\"cdy\":1,\"tm\":0,\"su\":1,\"te\":16,\"sfd\":1," SCRAP_TAS("4044") ",\"ms\":3,\"aid\":\"30167\",\"inm\":\"pickup Tin Can\"}"),
    SCRAP("Chocolate", "cake", "{\"s\":[2.22,4.57,2.22],\"i\":29,\"pl\":1,\"cdy\":1,\"tm\":0,\"su\":1,\"te\":16,\"sfd\":1," SCRAP_TAS("4786") ",\"ms\":3,\"aid\":\"39525\",\"inm\":\"pickup Chocolate\"}"),
    SCRAP("Carrot", "eco", "{\"s\":[2.22,4.57,2.22],\"i\":29,\"pl\":1,\"cdy\":1,\"tm\":0,\"su\":1,\"te\":16,\"sfd\":1," SCRAP_TAS("4787") ",\"ms\":3,\"aid\":\"39526\",\"inm\":\"pickup Carrot\"}"),
    SCRAP("Frog Pickup", "pets", "{\"s\":[2.32,2.49,2.73],\"i\":29,\"pl\":1,\"cdy\":1,\"tm\":0,\"su\":1,\"te\":16,\"sfd\":1," SCRAP_TAS("5366") ",\"aid\":\"46480\",\"inm\":\"pickup Frog\"}"),
    // zombies machines (costs as the shipped zombies maps set them)
    {"Mystery Box", "card_giftcard", OM_ENTRY_RECORD, 0, "{\"i\":91,\"sp\":950}"},
    {"Krum Machine", "precision_manufacturing", OM_ENTRY_RECORD, 0, "{\"i\":94,\"sp\":5000}"},
    {"Perk: Double-Up", "local_drink", OM_ENTRY_RECORD, 0, "{\"i\":92,\"zp\":1,\"sp\":2000}"},
    {"Perk: Western Hands", "local_drink", OM_ENTRY_RECORD, 0, "{\"i\":92,\"zp\":2,\"sp\":3000}"},
    {"Perk: Swift", "local_drink", OM_ENTRY_RECORD, 0, "{\"i\":92,\"zp\":3,\"sp\":2000}"},
    {"Perk: Power Slide", "local_drink", OM_ENTRY_RECORD, 0, "{\"i\":92,\"zp\":4,\"sp\":15000}"},
    {"Perk: Drift Fuze", "local_drink", OM_ENTRY_RECORD, 0, "{\"i\":92,\"zp\":5,\"sp\":2500}"},
    {"Perk: Holster", "local_drink", OM_ENTRY_RECORD, 0, "{\"i\":92,\"zp\":6,\"sp\":9000}"},
};
#undef SCRAP
#undef SCRAP_TAS

static const ObjectManagerEntry k_npcs[] = {
    {"Bot", "smart_toy", OM_ENTRY_BOT, 0, NULL},
    {"Ghost", "blur_on", OM_ENTRY_AI, 0, NULL},
    {"Drowned Sailor", "sailing", OM_ENTRY_AI, 1, NULL},
    {"Jack O' Lantern", "light", OM_ENTRY_AI, 2, NULL},
    {"Mirror Image", "person", OM_ENTRY_AI, 3, NULL},
    {"Lost Soul", "local_fire_department", OM_ENTRY_AI, 4, NULL},
    {"Walker", "directions_walk", OM_ENTRY_ZOMBIE, 0, NULL},
    {"Brute", "fitness_center", OM_ENTRY_ZOMBIE, 1, NULL},
    {"Flesh Colossal", "dangerous", OM_ENTRY_ZOMBIE, 2, NULL},
    {"Runner", "directions_run", OM_ENTRY_ZOMBIE, 3, NULL},
    {"Rascal", "child_care", OM_ENTRY_ZOMBIE, 4, NULL},
    // Soul Sanctum's AIs (game.ts SOUL_SANCTUM_AI_TEMPLATES, /summon)
    {"Angered Soul", "whatshot", OM_ENTRY_AI_TEMPLATE, 0, NULL},
    {"Gatekeeper", "whatshot", OM_ENTRY_AI_TEMPLATE, 0, NULL},
    {"Lord of Souls", "whatshot", OM_ENTRY_AI_TEMPLATE, 0, NULL},
    {"Soul Fiend", "whatshot", OM_ENTRY_AI_TEMPLATE, 0, NULL},
    {"Wandering Soul", "whatshot", OM_ENTRY_AI_TEMPLATE, 0, NULL},
    {"Weeping Soul", "whatshot", OM_ENTRY_AI_TEMPLATE, 0, NULL},
};

const ObjectManagerEntry *object_manager_entries(const int tab, int *count) {
    if (tab == OM_TAB_ENTITIES) {
        if (count) *count = (int) (sizeof(k_entities) / sizeof(k_entities[0]));
        return k_entities;
    }
    if (tab == OM_TAB_NPCS) {
        if (count) *count = (int) (sizeof(k_npcs) / sizeof(k_npcs[0]));
        return k_npcs;
    }
    if (count) *count = 0;
    return NULL;
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

static int thing_push(ObjectManager *om, const int kind, const int sub, const int id, const int owner) {
    if (om->thing_count == om->thing_capacity) {
        const size_t cap = om->thing_capacity ? om->thing_capacity * 2 : 32;
        OmThing *grown = realloc(om->things, cap * sizeof(OmThing));
        if (!grown) return 0;
        om->things = grown;
        om->thing_capacity = cap;
    }
    om->things[om->thing_count++] = (OmThing) {(uint8_t) kind, (uint8_t) sub, id, owner, 0};
    return 1;
}

static Object *spawned_object(const Game *game, const int uid) {
    if (!game->map || uid < 0 || (size_t) uid >= game->map->object_count) return NULL;
    Object *o = game->map->objects[uid];
    return o->spawned ? o : NULL;
}

static GameBot *bot_by_id(Game *game, const int id) {
    GameBots *bots = game->bots;
    for (int i = 0; bots && i < bots->count; i++) {
        if (bots->list[i].id == id) return &bots->list[i];
    }
    return NULL;
}

static int thing_alive(Game *game, const OmThing *t) {
    if (t->kind == OM_KIND_OBJECT) {
        const Object *o = spawned_object(game, t->id);
        return o && o->active;
    }
    if (t->kind == OM_KIND_NPC && t->sub == OM_SUB_BOT) return bot_by_id(game, t->id) != NULL;
    if (t->kind == OM_KIND_NPC) {
        const GameAI *ai = game_ai_find(game, t->id);
        return ai && ai->serial == t->serial;
    }
    if (t->kind == OM_KIND_ENTITY) return t->sub == OM_SUB_AIRDROP ? airdrop_alive(game, t->id, t->owner) : spawnable_alive(game, t->id);
    return 0;
}

// Live things of `owner` (-1: everyone) of `kind`, pickups only when `pickups`.
static int things_count(Game *game, const ObjectManager *om, const int owner, const int kind, const int pickups) {
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
    if (object->prefab == PREFAB_MYSTERY_BOX || object->prefab == PREFAB_PERK_MACHINE || object->prefab == PREFAB_KRUM_MACHINE) zombies_machine_added(game, at);
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
    thing_push(om, OM_KIND_OBJECT, 0, uid, player->uid);
    spawned_send(game, NET_TARGET_ALL, OM_SPAWNED_ADD, uid, game->map->objects[uid]);
    game_pickup_send(game, NET_TARGET_ALL, uid);
    return OM_STATUS_OK;
}

static void thing_remove(Game *game, const OmThing *t) {
    if (t->kind == OM_KIND_OBJECT) {
        spawned_remove(game, t->id);
    } else if (t->kind == OM_KIND_NPC && t->sub == OM_SUB_BOT) {
        GameBot *bot = bot_by_id(game, t->id);
        if (bot) game_bots_remove(game, bot);
    } else if (t->kind == OM_KIND_NPC) {
        GameAI *ai = game_ai_find(game, t->id);
        if (ai && ai->serial == t->serial) game_ai_remove(game, ai);
    } else if (t->kind == OM_KIND_ENTITY && t->sub == OM_SUB_AIRDROP) {
        airdrop_remove(game, t->id);
    } else if (t->kind == OM_KIND_ENTITY) {
        spawnable_remove(game, t->id);
    }
}

// The limit `count` hits for `kind` (owner's live things / everyone's), 0 none.
static int om_limit(Game *game, const ObjectManager *om, const Player *player, const int kind) {
    const int per_player = kind == OM_KIND_NPC ? OM_NPCS_PER_PLAYER : kind == OM_KIND_ENTITY ? OM_ENTITIES_PER_PLAYER : OM_OBJECTS_PER_PLAYER;
    const int per_game = kind == OM_KIND_NPC ? OM_NPCS_MAX : kind == OM_KIND_ENTITY ? OM_ENTITIES_MAX : OM_OBJECTS_MAX;
    if (things_count(game, om, player->uid, kind, 0) >= per_player) return per_player;
    if (things_count(game, om, -1, kind, 0) >= per_game) return per_game;
    return 0;
}

// A catalogue record at the floor point, turned to face the player (a quarter turn: the AABB
// collision matches the model). Owned by the round's records from here on.
static int om_spawn_record(Game *game, ObjectManager *om, Player *player, const char *json, const vec3 at) {
    cJSON *record = cJSON_Parse(json);
    if (!record) return -1;
    const int prefab = map_raw_object_prefab(record);
    const PrefabData *pd = prefab_data(prefab);
    double size[3] = {10.0, 10.0, 10.0};
    const cJSON *s = cJSON_GetObjectItem(record, "s");
    for (int k = 0; k < 3; k++) {
        if (cJSON_IsArray(s)) size[k] = cJSON_GetNumberValue(cJSON_GetArrayItem(s, k));
        else if (pd && pd->has_default_size) size[k] = pd->default_size[k];
    }
    // facing the player: yaw snapped to a quarter turn (w / l swap on the odd ones)
    const double quarter = round((player->direction.y + M_PI) / (M_PI / 2.0));
    const int turn = ((int) quarter % 4 + 4) % 4;
    if (turn % 2) {
        const double w = size[0];
        size[0] = size[2];
        size[2] = w;
    }
    const double pos[3] = {round(at.x * 10.0) / 10.0, round(at.y * 10.0) / 10.0, round(at.z * 10.0) / 10.0};
    const double rot[3] = {0.0, round(turn * (M_PI / 2.0) * 100.0) / 100.0, 0.0};
    cJSON_DeleteItemFromObject(record, "p");
    cJSON_DeleteItemFromObject(record, "s");
    cJSON_DeleteItemFromObject(record, "r");
    cJSON_AddItemToObject(record, "p", cJSON_CreateDoubleArray(pos, 3));
    cJSON_AddItemToObject(record, "s", cJSON_CreateDoubleArray(size, 3));
    if (turn) cJSON_AddItemToObject(record, "r", cJSON_CreateDoubleArray(rot, 3));
    const int uid = spawned_add(game, om, record, player->uid, -1);
    if (uid < 0) return -1;
    thing_push(om, OM_KIND_OBJECT, 0, uid, player->uid);
    spawned_send(game, NET_TARGET_ALL, OM_SPAWNED_ADD, uid, game->map->objects[uid]);
    if (game->map->objects[uid]->pickup) game_pickup_send(game, NET_TARGET_ALL, uid);
    return uid;
}

static int om_spawn_entry(Game *game, Player *player, const NetSpawnReqPacket *req, const int tab, NetSpawnResultPacket *out) {
    int count = 0;
    const ObjectManagerEntry *entries = object_manager_entries(tab, &count);
    if (!entries || req->item < 0 || req->item >= count) return OM_STATUS_INVALID;
    const ObjectManagerEntry *e = &entries[req->item];
    ObjectManager *om = om_of(game);
    if (!om || !game->map) return OM_STATUS_INVALID;
    const int kind = e->kind == OM_ENTRY_RECORD ? OM_KIND_OBJECT : tab == OM_TAB_NPCS ? OM_KIND_NPC : OM_KIND_ENTITY;
    const int limit = om_limit(game, om, player, kind);
    if (limit) {
        out->limit = (uint16_t) limit;
        return OM_STATUS_LIMIT;
    }
    ObjectManagerAim aim;
    if (!object_manager_aim(game, player, req->pitch, req->yaw, &aim)) return OM_STATUS_NO_TARGET;
    const vec3 at = om_floor_point(game, &aim, 4.0f);
    const float yaw = player->direction.y + (float) M_PI; // facing the player
    switch (e->kind) {
        case OM_ENTRY_BOT: {
            GameBot *bot = game_bots_add(game, NULL, &at, yaw);
            if (!bot) return OM_STATUS_LIMIT;
            thing_push(om, OM_KIND_NPC, OM_SUB_BOT, bot->id, player->uid);
            return OM_STATUS_OK;
        }
        case OM_ENTRY_AI:
        case OM_ENTRY_AI_TEMPLATE: {
            const GameAITemplate *t = e->kind == OM_ENTRY_AI_TEMPLATE ? game_ai_resolve_template(e->name) : NULL;
            if (e->kind == OM_ENTRY_AI_TEMPLATE && !t) return OM_STATUS_INVALID;
            GameAI *ai = t ? game_ai_spawn(game, 0, 0, t->name, at, &t->data) : game_ai_spawn(game, 0, e->data, e->name, at, NULL);
            if (!ai) return OM_STATUS_LIMIT; // config.aiLimit
            if (thing_push(om, OM_KIND_NPC, OM_SUB_AI, ai->sid, player->uid)) om->things[om->thing_count - 1].serial = ai->serial;
            game_ai_sync(game); // AI.sync() right away, as /summon
            return OM_STATUS_OK;
        }
        case OM_ENTRY_ZOMBIE: {
            const char *json = mode_zombie_template(e->data, NULL);
            cJSON *data = json ? cJSON_Parse(json) : NULL;
            if (!data) return OM_STATUS_INVALID;
            const int sid = map_ai_spawn(game, data, at, -1);
            cJSON_Delete(data);
            const GameAI *ai = sid >= 0 ? game_ai_find(game, sid) : NULL;
            if (!ai) return OM_STATUS_LIMIT;
            if (thing_push(om, OM_KIND_NPC, OM_SUB_AI, sid, player->uid)) om->things[om->thing_count - 1].serial = ai->serial;
            game_ai_sync(game);
            return OM_STATUS_OK;
        }
        case OM_ENTRY_SPAWNABLE: {
            const int sid = spawnable_drop(game, e->data, NULL, 0, at);
            if (sid < 0) return OM_STATUS_INVALID;
            thing_push(om, OM_KIND_ENTITY, OM_SUB_SPAWNABLE, sid, player->uid);
            return OM_STATUS_OK;
        }
        case OM_ENTRY_AIRDROP: {
            const int sid = airdrop_drop(game, player, (vec3) {at.x, at.y + AIRDROP_HEIGHT, at.z}, -1.0f, e->data);
            if (sid < 0) return OM_STATUS_LIMIT; // AIRDROP_MAX
            thing_push(om, OM_KIND_ENTITY, OM_SUB_AIRDROP, sid, player->uid);
            return OM_STATUS_OK;
        }
        case OM_ENTRY_RECORD:
            return om_spawn_record(game, om, player, e->json, at) >= 0 ? OM_STATUS_OK : OM_STATUS_INVALID;
        default:
            return OM_STATUS_INVALID;
    }
}

static int om_undo(Game *game, Player *player, const int any) {
    ObjectManager *om = game->object_manager;
    for (size_t i = om ? om->thing_count : 0; i-- > 0;) {
        const OmThing *t = &om->things[i];
        if ((!any && t->owner != player->uid) || !thing_alive(game, t)) continue;
        thing_remove(game, t);
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
        case OM_ACTION_SPAWN_ENTITY: out->status = (uint8_t) om_spawn_entry(game, player, req, OM_TAB_ENTITIES, out); break;
        case OM_ACTION_SPAWN_NPC: out->status = (uint8_t) om_spawn_entry(game, player, req, OM_TAB_NPCS, out); break;
        default: out->status = OM_STATUS_INVALID; break; // objects: part 3
    }
}
