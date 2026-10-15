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
#include <piss.h>
#include <world.h>
#include <cJSON.h>
#include <ctype.h>
#ifdef KRUNKNATIVE_CLIENT
#include <map_post.h> // map_post_object_detach / attach: a detached map object's mesh hides while its stand-in shows
#endif
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ruleset.h>

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
    unsigned seq;    // ObjectManager.seq at the spawn (undo takes the newest spawn or edit)
} OmThing;

// An edit's undo step: the object's record keys before a grab / drag / turn / field patch. A spawned
// object's: the keys the step changed ({"p", "s", "r", "d"} always for a pose edit), null for a key
// it did not have; a map object's: the whole overlay it had (om_overlay_of), or `original`.
typedef struct {
    unsigned seq;
    int uid;     // the object
    int editor;  // the player who edited it (their undo)
    cJSON *pose;
    unsigned char map;      // a map object's step (a pose change, a field patch or a delete)
    unsigned char original; // map: it was as the map has it before this step (undo puts the map's Object back)
} OmEdit;

// An edited map object: the map's own Object, detached while `uid` holds a stand-in (Object.map_edit)
typedef struct {
    int uid;
    Object *original;
    cJSON *record;      // the stand-in's record (its raw), NULL for a delete's placeholder
    cJSON *overlay;     // the keys the stand-in has in place of the map record's (om_overlay_of), NULL for a delete
    unsigned char deleted;
} OmMapEdit;

typedef struct ObjectManager_t {
    cJSON *records;    // array owning the spawned objects' records (Object.raw points into it)
    OmThing *things;   // every spawn of this round, oldest first (undo, caps)
    size_t thing_count, thing_capacity;
    OmEdit *edits;     // the edits' undo steps of this round, oldest first (OM_EDITS_MAX)
    size_t edit_count, edit_capacity;
    OmMapEdit *map_edits; // the edited map objects (any order)
    size_t map_edit_count, map_edit_capacity;
    unsigned seq;      // spawns and edits, in order
} ObjectManager;

static ObjectManager *om_of(Game *game) {
    if (!game->object_manager) game->object_manager = calloc(1, sizeof(ObjectManager));
    ObjectManager *om = game->object_manager;
    if (om && !om->records) om->records = cJSON_CreateArray();
    return om && om->records ? om : NULL;
}

static const Weapon *om_weapon(const Game *game, const int id) {
    const size_t count = game && game->weapon_count ? game->weapon_count : WEAPON_TABLE_COUNT;
    Weapon *const *all = game && game->weapons ? game->weapons : (Weapon *const *) rules_weapons();
    return id >= 0 && (size_t) id < count ? all[id] : NULL;
}

// ---------------------------------------------------------------- catalogue

int object_manager_weapon_listed(const int weapon_id) {
    if (weapon_id < 0 || weapon_id >= WEAPON_TABLE_COUNT || !rules_weapon(weapon_id)) return 0;
    // KrunkNative-only weapons: the Piss, Drank and Money Spread are Weapons tiles (the guns are the Tools pane's).
    // v10.0.2's own 29 Boulder and 30 Compressor (WEAPON_V10_COUNT) are tiles like the rest of Krunker's table.
    if (weapon_id >= WEAPON_V10_COUNT) return weapon_id == WEAPON_ID_PISS || weapon_id == WEAPON_ID_DRANK || weapon_id == WEAPON_ID_MONEY_SPREAD;
    return weapon_id != WEAPON_ID_KNIFE && !rules_weapon(weapon_id)->bomb; // every class has the knife; bombs are site-mode only
}

// The Tool Gun is no Weapons-tile (the Tools pane equips it) and never a pickup, but an extra slot
int object_manager_weapon_allowed(const int weapon_id) {
    return object_manager_weapon_listed(weapon_id) || weapon_id == WEAPON_ID_TOOL_GUN || weapon_id == WEAPON_ID_PHYS_GUN;
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

// The Objects tab: map prefabs at their default size (prefab_data), blocks with a texture ("t")
#define BLOCK(name, t) {name, "view_in_ar", OM_ENTRY_RECORD, 0, "{\"i\":0,\"t\":" #t "}"}
#define PROP(name, icon, i) {name, icon, OM_ENTRY_RECORD, 0, "{\"i\":" #i "}"}
static const ObjectManagerEntry k_objects[] = {
    BLOCK("Stone Block", 0),
    BLOCK("Wood Block", 2),
    BLOCK("Brick Block", 11),
    BLOCK("Metal Block", 17),
    BLOCK("Grass Block", 8),
    BLOCK("Grid Block", 3),
    {"Ramp", "signal_cellular_alt", OM_ENTRY_RECORD, 0, "{\"i\":9,\"t\":2}"},
    {"Ladder", "stairs", OM_ENTRY_RECORD, 0, "{\"i\":3}"},
    {"Cylinder", "circle", OM_ENTRY_RECORD, 0, "{\"i\":48,\"t\":0}"},
    {"Jump Pad", "keyboard_double_arrow_up", OM_ENTRY_RECORD, 0, "{\"i\":46,\"bm\":1.5}"},
    PROP("Crate", "img/props/prop_1.png", 1),
    {"Crate (Destructible)", "img/props/prop_1.png", OM_ENTRY_RECORD, 0, "{\"i\":1,\"h\":100,\"cdy\":1}"},
    PROP("Barrel", "img/props/prop_2.png", 2),
    PROP("Explosive Barrel", "img/props/prop_51.png", 51),
    PROP("Acid Barrel", "img/props/prop_20.png", 20),
    PROP("Stack", "img/props/prop_8.png", 8),
    PROP("Cone", "img/props/prop_16.png", 16),
    PROP("Cardboard Box", "img/props/prop_36.png", 36),
    PROP("Pallet", "img/props/prop_37.png", 37),
    PROP("Cage", "img/props/prop_50.png", 50),
    PROP("Teddy", "img/props/prop_28.png", 28),
    PROP("Pumpkin", "img/props/prop_56.png", 56),
    PROP("Skeleton", "img/props/prop_58.png", 58),
    PROP("Knight", "img/props/prop_59.png", 59),
    PROP("Gift", "img/props/prop_60.png", 60),
    PROP("Container", "local_shipping", 17),
    PROP("Open Container", "local_shipping", 19),
    PROP("Vehicle", "directions_car", 7),
    PROP("Police Car", "local_police", 49),
    PROP("Dumpster", "delete", 81),
    PROP("Door", "door_front", 21),
    PROP("Window", "window", 22),
    PROP("Sheet Metal", "crop_portrait", 86),
    PROP("Tree", "park", 15),
    PROP("Grass", "grass", 18),
    PROP("Christmas Tree", "forest", 61),
    PROP("Candy Cane", "cake", 62),
    PROP("Lamp", "light", 83),
    PROP("AC Unit", "ac_unit", 85),
    PROP("Antenna", "settings_input_antenna", 87),
    PROP("Satellite Dish", "satellite_alt", 88),
    PROP("Water Tower", "water", 84),
    PROP("Weapon Box", "inventory_2", 90),
    PROP("Tiles", "grid_view", 73),
};
#undef BLOCK
#undef PROP

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
    if (tab == OM_TAB_OBJECTS) {
        if (count) *count = (int) (sizeof(k_objects) / sizeof(k_objects[0]));
        return k_objects;
    }
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

// `give krtag`: the items a player picks up (spawnables, the editor's scrap pickups), dropped at
// the receiver's feet; not the machines or an airdrop crate on their head
int object_manager_entity_giveable(const int index) {
    const int count = (int) (sizeof(k_entities) / sizeof(k_entities[0]));
    if (index < 0 || index >= count) return 0;
    const ObjectManagerEntry *e = &k_entities[index];
    return e->kind == OM_ENTRY_SPAWNABLE || (e->kind == OM_ENTRY_RECORD && e->json && strstr(e->json, "\"inm\":\"pickup"));
}

// lowercase letters and digits only ("KR Tag" -> "krtag"), like the chat commands' player queries
static void om_normalise(const char *in, char *out, const size_t n) {
    size_t len = 0;
    for (; in && *in && len + 1 < n; in++) {
        if (isalnum((unsigned char) *in)) out[len++] = (char) tolower((unsigned char) *in);
    }
    out[len] = 0;
}

const char *object_manager_give_name(const int kind, const int id) {
    if (kind == OM_GIVE_WEAPON) return object_manager_weapon_allowed(id) ? rules_weapon(id)->name : NULL;
    if (kind == OM_GIVE_ENTITY) return object_manager_entity_giveable(id) ? k_entities[id].name : NULL;
    return NULL;
}

// pass 0 exact, 1 prefix, 2 substring
static int om_name_matches(const char *name, const char *query, const int pass) {
    char key[64];
    om_normalise(name, key, sizeof(key));
    if (pass == 0) return !strcmp(key, query);
    if (pass == 1) return !strncmp(key, query, strlen(query));
    return strstr(key, query) != NULL;
}

int object_manager_give_lookup(const char *query, int *id, int *matches) {
    char q[64];
    om_normalise(query, q, sizeof(q));
    if (matches) *matches = 0;
    if (!q[0]) return OM_GIVE_NONE;
    int digits = 1;
    for (const char *c = q; *c; c++) digits &= isdigit((unsigned char) *c) != 0;
    if (digits) { // a weapon id
        // ids typed or scripted before the ruleset seam: 29-31 were the KN weapons (weapons.h)
        const int w = weapon_id_from_legacy(atoi(q), rules_krunker_weapon_count());
        if (!object_manager_weapon_allowed(w)) return OM_GIVE_NONE;
        if (id) *id = w;
        if (matches) *matches = 1;
        return OM_GIVE_WEAPON;
    }
    static const struct {
        const char *alias;
        int weapon;
    } k_aliases[] = {{"physgun", WEAPON_ID_PHYS_GUN}, {"toolgun", WEAPON_ID_TOOL_GUN}, {"tool", WEAPON_ID_TOOL_GUN}, // Garry's Mod names
                     {"drank", WEAPON_ID_DRANK}, {"lean", WEAPON_ID_DRANK}, {"money", WEAPON_ID_MONEY_SPREAD}, {"cash", WEAPON_ID_MONEY_SPREAD}}; // the Nettspend's
    for (size_t i = 0; i < sizeof(k_aliases) / sizeof(k_aliases[0]); i++) {
        if (strcmp(q, k_aliases[i].alias) != 0 || !object_manager_weapon_allowed(k_aliases[i].weapon)) continue;
        if (id) *id = k_aliases[i].weapon;
        if (matches) *matches = 1;
        return OM_GIVE_WEAPON;
    }
    const int entity_count = (int) (sizeof(k_entities) / sizeof(k_entities[0]));
    for (int pass = 0; pass < 3; pass++) {
        int weapon = -1, weapons = 0, entity = -1, entities = 0;
        for (int w = 0; w < WEAPON_TABLE_COUNT; w++) {
            if (object_manager_weapon_allowed(w) && om_name_matches(rules_weapon(w)->name, q, pass)) {
                weapon = w;
                weapons++;
            }
        }
        for (int e = 0; e < entity_count; e++) {
            if (object_manager_entity_giveable(e) && om_name_matches(k_entities[e].name, q, pass)) {
                entity = e;
                entities++;
            }
        }
        if (matches) *matches = weapons + entities;
        if (weapons == 1) { // a weapon wins over an entity of the same name ("Grenade Launcher")
            if (id) *id = weapon;
            if (matches) *matches = 1;
            return OM_GIVE_WEAPON;
        }
        if (!weapons && entities == 1) {
            if (id) *id = entity;
            return OM_GIVE_ENTITY;
        }
        if (weapons + entities > 1) return OM_GIVE_NONE; // ambiguous
    }
    return OM_GIVE_NONE;
}

int object_manager_spawn_lookup(const char *query, int *tab, int *index, int *matches) {
    char q[64];
    om_normalise(query, q, sizeof(q));
    if (matches) *matches = 0;
    if (!q[0]) return 0;
    static const int k_tabs[3] = {OM_TAB_OBJECTS, OM_TAB_ENTITIES, OM_TAB_NPCS};
    for (int pass = 0; pass < 3; pass++) {
        int found = 0, found_tab = -1, found_index = -1;
        for (int t = 0; t < 3; t++) {
            int count = 0;
            const ObjectManagerEntry *entries = object_manager_entries(k_tabs[t], &count);
            for (int i = 0; entries && i < count; i++) {
                if (!om_name_matches(entries[i].name, q, pass)) continue;
                found++;
                found_tab = k_tabs[t];
                found_index = i;
            }
        }
        if (matches) *matches = found;
        if (found == 1) {
            if (tab) *tab = found_tab;
            if (index) *index = found_index;
            return 1;
        }
        if (found > 1) return 0; // ambiguous
    }
    return 0;
}

static const ObjectManagerToolInfo k_tools[OM_TOOL_COUNT] = {
    [OM_TOOL_DELETER] = {"Deleter", "delete_forever", "Removes what you shoot (map objects too, for devs and the host)", 1},
    [OM_TOOL_DUPLICATOR] = {"Duplicator", "content_copy", "Shoot an object to copy it, shoot again to place the copy", 0},
    [OM_TOOL_ROTATOR] = {"Rotator", "rotate_90_degrees_cw", "Turns the object you shoot a quarter turn", 1},
    [OM_TOOL_INFO] = {"Info", "info", "Shows what you shoot: type, owner and uid", 0},
    [OM_TOOL_MOVE] = {"Move", "open_with", "Shoot an object to select it, drag an arrow to move it", 1, 1},
    [OM_TOOL_RESIZE] = {"Resize", "aspect_ratio", "Shoot an object to select it, drag a face (crouch: both)", 1, 1},
    [OM_TOOL_PROPERTIES] = {"Properties", "tune", "Shoot an object to edit its map fields (I: the selection's)", 1, 1},
};

const ObjectManagerToolInfo *object_manager_tool_info(const int tool) {
    return tool >= 0 && tool < OM_TOOL_COUNT ? &k_tools[tool] : NULL;
}

// ---------------------------------------------------------------- rules

int object_manager_keep_extras(const Game *game, const ObjectManagerPerms *perms) {
    return (object_manager_caps(game, perms) & OM_CAP_WEAPONS) != 0;
}

int object_manager_filter_extras(const Game *game, const ObjectManagerPerms *perms, int *weapon_ids, const int count) {
    const int spawn = (object_manager_caps(game, perms) & OM_CAP_SPAWN) != 0;
    int n = 0;
    for (int i = 0; weapon_ids && i < count; i++) {
        if ((weapon_ids[i] != WEAPON_ID_TOOL_GUN && weapon_ids[i] != WEAPON_ID_PHYS_GUN) || spawn) weapon_ids[n++] = weapon_ids[i];
    }
    return n;
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
    if (dev || (host && !ranked)) caps |= OM_CAP_MAP; // the map's own objects: never a plain guest, not even in the sandbox
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
        case OM_STATUS_KEEP: return "You can't drop that weapon";
        case OM_STATUS_NO_TOOL: return "Equip the Tool Gun first";
        case OM_STATUS_UNSUPPORTED: return "That object can't be changed";
        case OM_STATUS_BLOCKED: return "It would end up inside a player";
        case OM_STATUS_BOUNDS: return "Out of reach or too big";
        case OM_STATUS_GEOMETRY: return "The Physics Gun can't move the map's geometry";
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
        if (!object_manager_weapon_allowed(id) || !om_weapon(player->game, id)) continue;
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
        const vec3 size = object_box_size(o);
        if (size.x <= 0.0f || size.y <= 0.0f || size.z <= 0.0f) continue;
        const float t = line_in_rect(origin, segment, o->position, size);
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
        const vec3 size = object_box_size(o);
        const float d[6] = {
            fabsf(out->point.y - (o->position.y + size.y)), fabsf(out->point.y - o->position.y),
            fabsf(out->point.x - (o->position.x + size.x * 0.5f)), fabsf(out->point.x - (o->position.x - size.x * 0.5f)),
            fabsf(out->point.z - (o->position.z + size.z * 0.5f)), fabsf(out->point.z - (o->position.z - size.z * 0.5f)),
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
    om->things[om->thing_count++] = (OmThing) {(uint8_t) kind, (uint8_t) sub, id, owner, 0, ++om->seq};
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
    if (op == OM_SPAWNED_ADD || op == OM_SPAWNED_UPDATE) {
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

// ---------------------------------------------------------------- edits: poses (both sides)

static int om_directed(const int prefab) { return prefab == PREFAB_RAMP || prefab == PREFAB_LADDER; }

int object_manager_is_map_object(const Game *game, const int uid) {
    if (!game || !game->map || uid < 0 || (size_t) uid >= game->map->object_count) return 0;
    const Object *o = game->map->objects[uid];
    return !o->spawned && !o->dynamic; // (builds and spawned objects come after the map's own)
}

// A user asset that plays as a GLTF instance (aid with g / q): its instance hangs off the map's
// load (user_asset.c), so a rebuilt copy would have no instance in the scene
static int om_gltf_asset(const Object *o) {
    const cJSON *aid = o->raw ? cJSON_GetObjectItem(o->raw, "aid") : NULL;
    const char *text = cJSON_GetStringValue(aid);
    return text && (strchr(text, 'g') || strchr(text, 'q'));
}

// Prefabs whose position other systems take once (the modes' flags, objectives, zones and nodes,
// the zombies machines, the leaderboards' canvases, path / server nodes, anchors, environment
// zones, particles, sound emitters): the tools leave them be
static int om_prefab_fixed(const int prefab) {
    switch (prefab) {
        case PREFAB_FLAG: case PREFAB_OBJECTIVE: case PREFAB_BOMB_SITE: case PREFAB_DOMINATION_ZONE:
        case PREFAB_PATH_NODE: case PREFAB_SERVER_NODE: case PREFAB_MAP_ANCHOR: case PREFAB_ENVIRONMENT_ZONE:
        case PREFAB_SPAWN_POINT: case PREFAB_CAMERA_POSITION: case PREFAB_CAMERA: case PREFAB_PLACEHOLDER:
        case PREFAB_MYSTERY_BOX: case PREFAB_PERK_MACHINE: case PREFAB_KRUM_MACHINE: case PREFAB_LEADERBOARD:
        case PREFAB_PARTICLES: case PREFAB_SOUND_EMITTER:
            return 1;
        default:
            return 0;
    }
}

static int om_world_moved(const Game *game, const int uid) {
    for (size_t i = 0; i < game->world.moved_count; i++) {
        if (game->world.moved[i].object == uid) return 1;
    }
    return 0;
}

int object_manager_map_edited(const Game *game, const int uid) {
    const ObjectManager *om = game ? game->object_manager : NULL;
    for (size_t i = 0; om && i < om->map_edit_count; i++) {
        if (om->map_edits[i].uid == uid) return om->map_edits[i].deleted ? 2 : 1;
    }
    return 0;
}

int object_manager_map_editable(const Game *game, const int uid) {
    if (!object_manager_is_map_object(game, uid) || object_manager_map_edited(game, uid) == 2) return 0;
    const Object *o = game->map->objects[uid];
    // (a node teleport, game.teleportToNode, moved it: its home is the trigger system's until the round ends)
    return o->active && o->raw && !o->complex_coll && !om_prefab_fixed(o->prefab) && !om_gltf_asset(o) && !om_world_moved(game, uid);
}

int object_manager_editable(const Game *game, const int uid) {
    if (object_manager_map_editable(game, uid)) return 1;
    const Object *o = game ? spawned_object(game, uid) : NULL;
    if (!o || !o->active || !o->raw) return 0;
    return o->prefab != PREFAB_MYSTERY_BOX && o->prefab != PREFAB_PERK_MACHINE && o->prefab != PREFAB_KRUM_MACHINE; // zombies machines
}

// The record's "p" / "s" (a ladder's Object sits off its "p" by the ladder offset), else the Object's
static void om_pose_of(const Object *o, ObjectManagerPose *out) {
    out->position = o->position;
    out->size = o->scale;
    const cJSON *p = cJSON_GetObjectItem(o->raw, "p"), *s = cJSON_GetObjectItem(o->raw, "s");
    float *pv = &out->position.x, *sv = &out->size.x;
    for (int k = 0; k < 3; k++) {
        if (cJSON_GetArraySize(p) >= 3 && cJSON_IsNumber(cJSON_GetArrayItem(p, k))) pv[k] = (float) cJSON_GetArrayItem(p, k)->valuedouble;
        if (cJSON_GetArraySize(s) >= 3 && cJSON_IsNumber(cJSON_GetArrayItem(s, k))) sv[k] = (float) cJSON_GetArrayItem(s, k)->valuedouble;
    }
    out->directed = om_directed(o->prefab);
    const cJSON *dj = cJSON_GetObjectItem(o->raw, "d"); // only ramps / ladders have one ((int) NaN is UB)
    const int d = cJSON_IsNumber(dj) ? (int) dj->valuedouble : 0;
    const cJSON *r = cJSON_GetObjectItem(o->raw, "r");
    const float ry = cJSON_GetArraySize(r) >= 3 && cJSON_IsNumber(cJSON_GetArrayItem(r, 1)) ? (float) cJSON_GetArrayItem(r, 1)->valuedouble : o->rotation.y;
    const int q = object_manager_yaw_quarter(ry);
    out->quarter = ((out->directed ? d : q) % 4 + 4) % 4;
    const float two_pi = 2.0f * (float) M_PI;
    out->yaw = out->directed ? (float) out->quarter * ((float) M_PI / 2.0f) : fmodf(fmodf(ry, two_pi) + two_pi, two_pi);
}

uint16_t object_manager_yaw_pack(const float yaw) {
    const double two_pi = 2.0 * M_PI;
    double y = isfinite(yaw) ? fmod((double) yaw, two_pi) : 0.0;
    if (y < 0.0) y += two_pi;
    return (uint16_t) ((long) llround(y / two_pi * 65536.0) & 0xffff);
}

float object_manager_yaw_unpack(const uint16_t packed) { return (float) ((double) packed / 65536.0 * 2.0 * M_PI); }

int object_manager_yaw_quarter(const float yaw) {
    // (exactly between two quarters, as a 45 degree snap is: the lower one)
    const int q = isfinite(yaw) ? (int) floorf(yaw / ((float) M_PI / 2.0f) + 0.5f - 1e-3f) : 0;
    return (q % 4 + 4) % 4;
}

int object_manager_pose(const Game *game, const int uid, ObjectManagerPose *out) {
    if (!object_manager_editable(game, uid) || !out) return 0;
    om_pose_of(game->map->objects[uid], out);
    return 1;
}

float object_manager_snap(const float value, const float step) {
    return step > 0.0f ? roundf(value / step) * step : value;
}

static float *axis_of(vec3 *v, const int axis) { return axis == 0 ? &v->x : axis == 1 ? &v->y : &v->z; }

void object_manager_pose_move(const ObjectManagerPose *from, const int axis, const float delta, const float snap, ObjectManagerPose *out) {
    *out = *from;
    if (axis < 0 || axis > 2) return;
    *axis_of(&out->position, axis) += object_manager_snap(delta, snap);
}

// The box is centred on the feet in x / z and stands on them in y
void object_manager_pose_resize(const ObjectManagerPose *from, const int axis, const int sign, const float delta, const float snap,
                                const int symmetric, ObjectManagerPose *out) {
    *out = *from;
    if (axis < 0 || axis > 2) return;
    const float size = *axis_of(&out->size, axis);
    float grow = object_manager_snap(delta, snap) * (symmetric ? 2.0f : 1.0f);
    const float cap = fmaxf(OM_EDIT_SIZE_MAX, size); // a map object past the limit may keep its size, never grow
    if (size + grow < OM_EDIT_SIZE_MIN) grow = OM_EDIT_SIZE_MIN - size;
    if (size + grow > cap) grow = cap - size;
    *axis_of(&out->size, axis) = size + grow;
    // the centre moves half the growth toward the dragged face (not at all when symmetric)
    const float shift = symmetric ? 0.0f : grow * 0.5f * (sign < 0 ? -1.0f : 1.0f);
    if (axis == 1) out->position.y += shift - grow * 0.5f; // feet = centre - size / 2
    else *axis_of(&out->position, axis) += shift;
}

void object_manager_pose_turn(const ObjectManagerPose *from, const int turns, ObjectManagerPose *out) {
    *out = *from;
    out->quarter = ((from->quarter + turns) % 4 + 4) % 4;
    const float two_pi = 2.0f * (float) M_PI;
    out->yaw = fmodf(fmodf(from->yaw + (float) turns * ((float) M_PI / 2.0f), two_pi) + two_pi, two_pi);
    if (!from->directed && (turns % 2)) {
        out->size.x = from->size.z;
        out->size.z = from->size.x;
    }
}

// The record with the pose's "p", "s" and "r" (or map.js "d"), doubles rounded like map.js
static cJSON *om_pose_record(const cJSON *raw, const ObjectManagerPose *pose) {
    cJSON *record = cJSON_Duplicate(raw, 1);
    if (!record) return NULL;
    // map.js rounds p and s to 2 decimals: so do edits (a map object turned in place stays in place)
    const double p[3] = {round(pose->position.x * 100.0) / 100.0, round(pose->position.y * 100.0) / 100.0, round(pose->position.z * 100.0) / 100.0};
    const double sz[3] = {round(pose->size.x * 100.0) / 100.0, round(pose->size.y * 100.0) / 100.0, round(pose->size.z * 100.0) / 100.0};
    cJSON_DeleteItemFromObject(record, "p");
    cJSON_DeleteItemFromObject(record, "s");
    cJSON_DeleteItemFromObject(record, "r");
    cJSON_AddItemToObject(record, "p", cJSON_CreateDoubleArray(p, 3));
    cJSON_AddItemToObject(record, "s", cJSON_CreateDoubleArray(sz, 3));
    if (pose->directed) {
        cJSON_DeleteItemFromObject(record, "d");
        cJSON_AddNumberToObject(record, "d", pose->quarter);
    } else {
        // the record's tilt (r0, r2) stays; its yaw is the pose's (any angle: a Physics Gun carry turns
        // freely; the AABB collision follows the nearest quarter turn's size, which the pose carries)
        double r[3] = {0.0, 0.0, 0.0};
        const cJSON *rj = cJSON_GetObjectItem(raw, "r");
        for (int k = 0; k < 3 && cJSON_GetArraySize(rj) >= 3; k++) r[k] = cJSON_IsNumber(cJSON_GetArrayItem(rj, k)) ? cJSON_GetArrayItem(rj, k)->valuedouble : 0.0;
        const double was = r[1];
        double yaw = isfinite(pose->yaw) ? fmod((double) pose->yaw, 2.0 * M_PI) : 0.0;
        if (yaw < 0.0) yaw += 2.0 * M_PI;
        // unchanged (within the record's 2-decimal rounding): as the record has it, so an object only
        // moved keeps an off-grid yaw exactly
        const double d = fmod(fabs(yaw - (was < 0.0 ? fmod(was, 2.0 * M_PI) + 2.0 * M_PI : fmod(was, 2.0 * M_PI))), 2.0 * M_PI);
        r[1] = fmin(d, 2.0 * M_PI - d) < 0.006 ? was : round(yaw * 100.0) / 100.0;
        if (r[0] != 0.0 || r[1] != 0.0 || r[2] != 0.0) cJSON_AddItemToObject(record, "r", cJSON_CreateDoubleArray(r, 3));
    }
    return record;
}

// Swaps a fresh object built from `record` (owned by the round's records from here on) in at
// `uid`: collision, the mesh and the record all follow; the old object and its record go.
static int om_replace(Game *game, ObjectManager *om, const int uid, cJSON *record) {
    Object *old = spawned_object(game, uid);
    Object *o = old && record ? map_object_from_json(game->map, record, !game->server) : NULL;
    if (!o) {
        cJSON_Delete(record);
        return 0;
    }
    o->spawned = 1;
    o->spawn_owner = old->spawn_owner;
    o->destroyed_by = -1;
    cJSON_AddItemToArray(om->records, record);
    const cJSON *old_raw = old->raw;
    if (old->pickup) game_pickup_disable(game, uid);
    game->map->objects[uid] = o;
    map_object_free(old);
    for (cJSON *item = om->records->child; old_raw && item; item = item->next) { // its record leaves the round's
        if (item != old_raw) continue;
        cJSON_Delete(cJSON_DetachItemViaPointer(om->records, item));
        break;
    }
    if (o->pickup) { // the pickup runtime takes the new spot
        game_pickup_added(game, uid);
        if (game_is_authority(game)) game_pickup_send(game, NET_TARGET_ALL, uid);
    }
    return 1;
}

static const char *const k_pose_keys[4] = {"p", "s", "r", "d"};

static int om_pose_key(const char *key) {
    for (int k = 0; k < 4; k++) {
        if (!strcmp(key, k_pose_keys[k])) return 1;
    }
    return 0;
}

// The overlay's keys on `record` in place: a value replaces the key, null removes it. `pose_rule`: a
// pose key the overlay lacks goes too (a map edit overlay; patch 1 sent the pose keys a record had)
static void om_record_overlay(cJSON *record, const cJSON *overlay, const int pose_rule) {
    for (int k = 0; pose_rule && k < 4; k++) {
        if (!cJSON_GetObjectItemCaseSensitive(overlay, k_pose_keys[k])) cJSON_DeleteItemFromObjectCaseSensitive(record, k_pose_keys[k]);
    }
    const cJSON *item;
    cJSON_ArrayForEach(item, overlay) {
        cJSON_DeleteItemFromObjectCaseSensitive(record, item->string);
        if (!cJSON_IsNull(item)) cJSON_AddItemToObject(record, item->string, cJSON_Duplicate(item, 1));
    }
}

// The keys of `from` that `to` has otherwise: to's value, null for a key `to` lacks; with `pose`
// every pose key (to's, or null)
static cJSON *om_keys_changed(const cJSON *from, const cJSON *to, const int pose) {
    cJSON *out = cJSON_CreateObject();
    if (!out) return NULL;
    for (int k = 0; pose && k < 4; k++) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(to, k_pose_keys[k]);
        cJSON_AddItemToObject(out, k_pose_keys[k], v ? cJSON_Duplicate(v, 1) : cJSON_CreateNull());
    }
    const cJSON *item;
    cJSON_ArrayForEach(item, to) {
        if (cJSON_GetObjectItemCaseSensitive(out, item->string)) continue;
        const cJSON *was = cJSON_GetObjectItemCaseSensitive(from, item->string);
        if (!was || !cJSON_Compare(was, item, 1)) cJSON_AddItemToObject(out, item->string, cJSON_Duplicate(item, 1));
    }
    cJSON_ArrayForEach(item, from) {
        if (!cJSON_GetObjectItemCaseSensitive(to, item->string) && !cJSON_GetObjectItemCaseSensitive(out, item->string))
            cJSON_AddItemToObject(out, item->string, cJSON_CreateNull());
    }
    return out;
}

// {"p", "s", "r", "d"} of a record (those it has)
static cJSON *om_pose_keys(const cJSON *raw) {
    cJSON *pose = cJSON_CreateObject();
    for (int k = 0; pose && k < 4; k++) {
        const cJSON *v = cJSON_GetObjectItem(raw, k_pose_keys[k]);
        if (v) cJSON_AddItemToObject(pose, k_pose_keys[k], cJSON_Duplicate(v, 1));
    }
    return pose;
}

// ---------------------------------------------------------------- map edits (both sides)
// The map's own objects (object_manager.h "Map edits"): a stand-in at the uid, the original
// detached until Undo / the round's end. The stand-in is the map record with the edited pose keys,
// built like the map built the original, so collision, jump pads, teleporters, ladders, ramps,
// triggers and pickups all follow the new pose.

static OmMapEdit *om_map_entry(ObjectManager *om, const int uid) {
    for (size_t i = 0; om && i < om->map_edit_count; i++) {
        if (om->map_edits[i].uid == uid) return &om->map_edits[i];
    }
    return NULL;
}

// "ci" (the map's colour palette) resolved into "c": stand-ins are built without the palette
static void om_resolve_palette(const Map *map, cJSON *record) {
    const cJSON *ci = cJSON_GetObjectItem(record, "ci");
    if (!cJSON_IsNumber(ci)) return;
    const cJSON *colour = cJSON_GetArrayItem(cJSON_GetObjectItem(map->raw_data, "colors"), (int) ci->valuedouble);
    if (!cJSON_IsString(colour) || !colour->valuestring[0]) return;
    cJSON_DeleteItemFromObject(record, "c");
    cJSON_AddStringToObject(record, "c", colour->valuestring);
    cJSON_DeleteItemFromObject(record, "ci");
}

cJSON *object_manager_record(const Game *game, const int uid) {
    if (!object_manager_editable(game, uid)) return NULL;
    cJSON *record = cJSON_Duplicate(game->map->objects[uid]->raw, 1);
    if (record) om_resolve_palette(game->map, record);
    return record;
}

// The map's record of `original`, its palette colour resolved (what overlays apply to)
static cJSON *om_map_base(const Map *map, const Object *original) {
    cJSON *record = cJSON_Duplicate(original->raw, 1);
    if (record) om_resolve_palette(map, record);
    return record;
}

// The map's record of `original` with `overlay` ({"p", "s", "r", "d"} and edited keys) in place of its own
static cJSON *om_map_record(const Map *map, const Object *original, const cJSON *overlay) {
    cJSON *record = om_map_base(map, original);
    if (record) om_record_overlay(record, overlay, 1);
    return record;
}

// The overlay that makes the map's record of `original` into `next` (its palette colour resolved in
// place first): the pose keys it has and each other key that differs (null: gone). A pose key the
// overlay lacks goes (om_record_overlay's pose rule), so a pose edit's overlay is patch 1's.
static cJSON *om_overlay_of(const Map *map, const Object *original, cJSON *next) {
    om_resolve_palette(map, next);
    cJSON *base = om_map_base(map, original);
    cJSON *overlay = base ? om_keys_changed(base, next, 1) : NULL;
    cJSON_Delete(base);
    for (int k = 0; overlay && k < 4; k++) {
        if (cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(overlay, k_pose_keys[k]))) cJSON_DeleteItemFromObjectCaseSensitive(overlay, k_pose_keys[k]);
    }
    return overlay;
}

static Object *om_placeholder(const Object *original) {
    Object *o = calloc(1, sizeof(Object));
    if (!o) return NULL;
    o->map_edit = 1; // (inactive, not destroyed: no "dest" state for joiners, MAP_DELETE says it)
    o->destroyed_by = -1;
    o->collision_type = COLLISION_TYPE_NONE;
    o->position = original->position;
    o->prefab = PREFAB_CUBE;
    return o;
}

static void om_bots_replan(Game *game) {
    if (game->bots) bot_nav_invalidate(game);
}

// The uid's object becomes `next` (a stand-in, or the original when `next` is it): the one there
// goes (a stand-in is freed, the original detached), the pickup and zombies runtimes follow
static void om_map_swap(Game *game, OmMapEdit *e, Object *next) {
    Object *current = game->map->objects[e->uid];
    if (current == next) return;
    if (current->pickup) game_pickup_disable(game, e->uid); // (a stand-in pickup re-registers below)
    game->map->objects[e->uid] = next;
    if (current == e->original) {
#ifdef KRUNKNATIVE_CLIENT
        map_post_object_detach(current, e->uid);
#endif
    } else {
        map_object_free(current);
    }
    if (next == e->original) {
#ifdef KRUNKNATIVE_CLIENT
        map_post_object_attach(next, e->uid);
#endif
        // a node teleport of the stand-in (game.teleportToNode) went with it: the original never moved
        for (size_t i = game->world.moved_count; i-- > 0;) {
            if (game->world.moved[i].object == e->uid) game->world.moved[i] = game->world.moved[--game->world.moved_count];
        }
    }
    if (next->pickup) {
        game_pickup_added(game, e->uid);
        if (game_is_authority(game)) game_pickup_send(game, NET_TARGET_ALL, e->uid);
    }
    om_bots_replan(game);
}

// The overlay `pose` (NULL: deleted) on map object `uid`. 1 done.
static int om_map_apply(Game *game, ObjectManager *om, const int uid, const cJSON *pose) {
    if (!object_manager_is_map_object(game, uid)) return 0;
    OmMapEdit *e = om_map_entry(om, uid);
    Object *original = e ? e->original : game->map->objects[uid];
    if (!original->raw) return 0;
    cJSON *record = pose ? om_map_record(game->map, original, pose) : NULL;
    cJSON *overlay = pose ? cJSON_Duplicate(pose, 1) : NULL;
    Object *next = pose ? (record && overlay ? map_object_from_json(game->map, record, !game->server) : NULL) : om_placeholder(original);
    if (!next) {
        cJSON_Delete(record);
        cJSON_Delete(overlay);
        return 0;
    }
    next->map_edit = 1;
    next->destroyed_by = -1;
    if (!e) {
        if (om->map_edit_count == om->map_edit_capacity) {
            const size_t cap = om->map_edit_capacity ? om->map_edit_capacity * 2 : 16;
            OmMapEdit *grown = realloc(om->map_edits, cap * sizeof(OmMapEdit));
            if (!grown) {
                map_object_free(next);
                cJSON_Delete(record);
                cJSON_Delete(overlay);
                return 0;
            }
            om->map_edits = grown;
            om->map_edit_capacity = cap;
        }
        e = &om->map_edits[om->map_edit_count++];
        *e = (OmMapEdit) {uid, original, NULL, NULL, 0};
    }
    cJSON *old_record = e->record;
    om_map_swap(game, e, next); // (frees the previous stand-in, which pointed into old_record)
    cJSON_Delete(old_record);
    cJSON_Delete(e->overlay);
    e->record = record;
    e->overlay = overlay;
    e->deleted = !pose;
    return 1;
}

// Map object `uid` as the map has it again. 1 it was edited.
static int om_map_restore(Game *game, ObjectManager *om, const int uid) {
    OmMapEdit *e = om_map_entry(om, uid);
    if (!e) return 0;
    om_map_swap(game, e, e->original);
    cJSON_Delete(e->record);
    cJSON_Delete(e->overlay);
    *e = om->map_edits[--om->map_edit_count];
    return 1;
}

void object_manager_restore_map(Game *game) {
    ObjectManager *om = game ? game->object_manager : NULL;
    while (om && om->map_edit_count && game->map) om_map_restore(game, om, om->map_edits[om->map_edit_count - 1].uid);
    if (om) om->map_edit_count = 0;
}

// SPAWNED_OBJ of a map edit (authority): MAP_EDIT with the stand-in's overlay, MAP_DELETE, MAP_RESTORE
static void om_map_send(Game *game, const int target, const int uid, const int editor) {
    ObjectManager *om = game->object_manager;
    const OmMapEdit *e = om_map_entry(om, uid);
    uint8_t buffer[sizeof(NetSpawnedObjPacket) + OM_RECORD_MAX + 1];
    const int op = !e ? OM_SPAWNED_MAP_RESTORE : e->deleted ? OM_SPAWNED_MAP_DELETE : OM_SPAWNED_MAP_EDIT;
    const NetSpawnedObjPacket head = {.op = (uint8_t) op, .kind = OM_KIND_OBJECT, .id = uid, .owner = editor};
    memcpy(buffer, &head, sizeof(head));
    uint16_t length = sizeof(head);
    if (op == OM_SPAWNED_MAP_EDIT) {
        char *json = e->overlay ? cJSON_PrintUnformatted(e->overlay) : NULL;
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

// The client's side of a map edit SPAWNED_OBJ
static int om_map_receive(Game *game, ObjectManager *om, const NetSpawnedObjPacket *head, const char *json, const size_t n) {
    if (head->op == OM_SPAWNED_MAP_RESTORE) return om_map_restore(game, om, head->id);
    if (head->op == OM_SPAWNED_MAP_DELETE) return om_map_apply(game, om, head->id, NULL);
    if (!json || !n || json[n - 1] != 0) return 0;
    cJSON *pose = cJSON_Parse(json);
    const int ok = pose && om_map_apply(game, om, head->id, pose);
    cJSON_Delete(pose);
    return ok;
}

// ---------------------------------------------------------------- picking (both sides)

// The box the tools pick an object by: its collision box, at least a unit thick (planes, pads)
static int om_pick_box(const Object *o, vec3 *pos, vec3 *size) {
    if (!o->active) return 0;
    const vec3 s = object_box_size(o);
    if (!(s.x > 0.0f || s.y > 0.0f || s.z > 0.0f)) return 0;
    *size = (vec3) {fmaxf(s.x, 1.0f), fmaxf(s.y, 1.0f), fmaxf(s.z, 1.0f)};
    *pos = (vec3) {o->position.x, o->position.y - (s.y < 1.0f ? (1.0f - s.y) * 0.5f : 0.0f), o->position.z};
    return 1;
}

// The nearest object of any kind along the segment (non-solid, unshootable and thin ones too:
// jump pads, zones, triggers, planes), terrain in the way; -1 none. *t: the fraction.
static int om_pick_ray(const Game *game, const vec3 eye, const vec3 segment, float *t_out) {
    float best = world_terrain_fraction(game, eye, segment);
    if (!(best > 0.0f && best <= 1.0f)) best = 2.0f;
    int found = -1;
    for (size_t i = 0; game->map && i < game->map->object_count; i++) {
        vec3 pos, size;
        if (!om_pick_box(game->map->objects[i], &pos, &size)) continue;
        const float t = line_in_rect(eye, segment, pos, size);
        if (t > 0.0f && t <= 1.0f && t < best) {
            best = t;
            found = (int) i;
        }
    }
    *t_out = best;
    return found;
}

int object_manager_pick(const Game *game, const Player *player, const float pitch, const float yaw, ObjectManagerAim *out) {
    if (!game || !game->map || !player || !out) return 0;
    const vec3 eye = {player->position.x, player->position.y + player->height - game_constants.camera_height, player->position.z};
    const vec3 dir = om_look(isfinite(pitch) ? player_clamp_pitch(pitch) : player->direction.x, isfinite(yaw) ? yaw : player->direction.y);
    const vec3 segment = {dir.x * OM_AIM_RANGE, dir.y * OM_AIM_RANGE, dir.z * OM_AIM_RANGE};
    float t;
    const int uid = om_pick_ray(game, eye, segment, &t);
    if (t > 1.0f) return 0;
    out->object = uid;
    out->point = (vec3) {eye.x + segment.x * t, eye.y + segment.y * t, eye.z + segment.z * t};
    out->normal = (vec3) {0.0f, 1.0f, 0.0f};
    return 1;
}

// An undo step for `editor`'s edit of `uid` (its record `raw` becoming `next`): a spawned object's
// keys the edit changes as they are (`pose`: every pose key), a map object's overlay (or that it
// was the map's own)
static void om_edit_push(const Game *game, ObjectManager *om, const int uid, const int editor, const cJSON *raw, const cJSON *next,
                         const int pose) {
    if (om->edit_count == OM_EDITS_MAX) { // the oldest goes
        cJSON_Delete(om->edits[0].pose);
        memmove(om->edits, om->edits + 1, (om->edit_count - 1) * sizeof(OmEdit));
        om->edit_count--;
    }
    if (om->edit_count == om->edit_capacity) {
        const size_t cap = om->edit_capacity ? om->edit_capacity * 2 : 32;
        OmEdit *grown = realloc(om->edits, cap * sizeof(OmEdit));
        if (!grown) return;
        om->edits = grown;
        om->edit_capacity = cap;
    }
    const int map = object_manager_is_map_object(game, uid);
    const OmMapEdit *e = map ? om_map_entry(om, uid) : NULL;
    cJSON *before = map ? (e && e->overlay ? cJSON_Duplicate(e->overlay, 1) : NULL) : next ? om_keys_changed(next, raw, pose) : om_pose_keys(raw);
    om->edits[om->edit_count++] = (OmEdit) {++om->seq, uid, editor, before, (unsigned char) map, (unsigned char) (map && !e)};
}

// `next` (a record; this call owns it) in place of object `uid`'s (authority): a spawned object
// rebuilt and sent as OM_SPAWNED_UPDATE, a map object's stand-in as OM_SPAWNED_MAP_EDIT (the
// overlay). With `step` an undo step of `editor` first (`pose`: a pose edit). OK, BOUNDS (the
// record or overlay past OM_RECORD_MAX: SPAWNED_OBJ could not carry it), INVALID.
static int om_apply_record(Game *game, ObjectManager *om, const int uid, cJSON *next, const int editor, const int step, const int pose) {
    const Object *o = next ? game->map->objects[uid] : NULL;
    if (!o || !o->raw) {
        cJSON_Delete(next);
        return OM_STATUS_INVALID;
    }
    const int map = object_manager_is_map_object(game, uid);
    cJSON *overlay = NULL;
    if (map) {
        const OmMapEdit *e = om_map_entry(om, uid);
        overlay = om_overlay_of(game->map, e ? e->original : o, next);
    }
    char *json = cJSON_PrintUnformatted(map ? overlay : next);
    const int fits = json && strlen(json) <= OM_RECORD_MAX;
    free(json);
    if (!fits) {
        cJSON_Delete(next);
        cJSON_Delete(overlay);
        return OM_STATUS_BOUNDS;
    }
    if (step) om_edit_push(game, om, uid, editor, o->raw, next, pose);
    int ok;
    if (map) {
        cJSON_Delete(next);
        ok = om_map_apply(game, om, uid, overlay);
        cJSON_Delete(overlay);
        if (ok) om_map_send(game, NET_TARGET_ALL, uid, editor);
    } else {
        ok = om_replace(game, om, uid, next);
        if (ok) spawned_send(game, NET_TARGET_ALL, OM_SPAWNED_UPDATE, uid, game->map->objects[uid]);
    }
    if (!ok && step) cJSON_Delete(om->edits[--om->edit_count].pose);
    return ok ? OM_STATUS_OK : OM_STATUS_INVALID;
}

// The object's pose `pose` in place (om_apply_record)
static int om_apply_pose(Game *game, ObjectManager *om, const int uid, const ObjectManagerPose *pose, const int editor, const int step) {
    const Object *o = game->map->objects[uid];
    return om_apply_record(game, om, uid, o->raw ? om_pose_record(o->raw, pose) : NULL, editor, step, 1);
}

// The newest undo step of `editor` is an edit of `uid` (a drag's later edits add none)
static int om_edit_open(const ObjectManager *om, const int uid, const int editor) {
    for (size_t i = om->edit_count; i-- > 0;) {
        if (om->edits[i].editor == editor) return om->edits[i].uid == uid;
    }
    return 0;
}

// The tracked spawn of map object `uid` (its owner), NULL none
static const OmThing *om_object_thing(const ObjectManager *om, const int uid) {
    for (size_t i = 0; om && i < om->thing_count; i++) {
        if (om->things[i].kind == OM_KIND_OBJECT && om->things[i].id == uid) return &om->things[i];
    }
    return NULL;
}

// Undo of an edit: the saved keys back on the object
static int om_edit_undo(Game *game, ObjectManager *om, const size_t index) {
    const OmEdit e = om->edits[index];
    memmove(om->edits + index, om->edits + index + 1, (om->edit_count - index - 1) * sizeof(OmEdit));
    om->edit_count--;
    if (e.map) { // the map's own Object back (nothing to do when someone's undo did already), or the overlay before the step
        const int changed = e.original ? om_map_restore(game, om, e.uid) : e.pose && om_map_apply(game, om, e.uid, e.pose);
        if (changed) om_map_send(game, NET_TARGET_ALL, e.uid, e.editor);
        cJSON_Delete(e.pose);
        return changed || e.original;
    }
    const Object *o = spawned_object(game, e.uid);
    int ok = 0;
    cJSON *record = o && o->raw && e.pose ? cJSON_Duplicate(o->raw, 1) : NULL;
    if (record) {
        om_record_overlay(record, e.pose, 0);
        ok = om_replace(game, om, e.uid, record);
        if (ok) spawned_send(game, NET_TARGET_ALL, OM_SPAWNED_UPDATE, e.uid, game->map->objects[e.uid]);
    }
    cJSON_Delete(e.pose);
    return ok;
}

// Player `p`'s box overlaps the box (feet `pos`, `size`)
static int om_box_hits_player(const Player *p, const vec3 pos, const vec3 size) {
    const float r = p->scale > 0.0f ? p->scale : game_constants.player_scale;
    const float h = p->height > 0.0f ? p->height : 10.0f;
    const float eps = 0.05f;
    return fabsf(p->position.x - pos.x) < r + size.x * 0.5f - eps && fabsf(p->position.z - pos.z) < r + size.z * 0.5f - eps &&
           p->position.y < pos.y + size.y - eps && p->position.y + h > pos.y + eps;
}

void object_manager_send_state(Game *game, const int uid) {
    for (size_t i = 0; game && game->map && i < game->map->object_count; i++) {
        const Object *o = game->map->objects[i];
        if (o->spawned && o->active) spawned_send(game, uid, OM_SPAWNED_ADD, (int) i, o);
    }
    const ObjectManager *om = game ? game->object_manager : NULL;
    for (size_t i = 0; om && i < om->map_edit_count; i++) om_map_send(game, uid, om->map_edits[i].uid, -1); // the edited map objects
}

void object_manager_round_start(Game *game) {
    ObjectManager *om = game ? game->object_manager : NULL;
    if (!om) return;
    object_manager_restore_map(game); // (the map's own Objects are back: the caller restored them before map_reset)
    // world_remove_builds freed the spawned objects (they point into these records) already
    cJSON_Delete(om->records);
    om->records = NULL;
    om->thing_count = 0;
    for (size_t i = 0; i < om->edit_count; i++) cJSON_Delete(om->edits[i].pose);
    om->edit_count = 0;
    for (size_t i = 0; i < game->player_count; i++) { // the Duplicator's copies were of this round's objects
        if (game->players[i]) game->players[i]->om_copy = 0;
    }
}

int object_manager_apply_spawned(Game *game, const void *payload, const uint16_t length) {
    if (!game || !game->ready || !game->map || length < sizeof(NetSpawnedObjPacket) || game_is_authority(game)) return 0;
    NetSpawnedObjPacket head;
    memcpy(&head, payload, sizeof(head));
    if (head.kind == OM_KIND_PUDDLE) return piss_apply_spawned(game, payload, length); // KrunkNative Piss
    if (head.kind != OM_KIND_OBJECT) return 0;
    if (head.op >= OM_SPAWNED_MAP_EDIT && head.op <= OM_SPAWNED_MAP_RESTORE) {
        ObjectManager *om = om_of(game);
        return om && om_map_receive(game, om, &head, (const char *) payload + sizeof(head), length - sizeof(head));
    }
    if (head.op == OM_SPAWNED_REMOVE) {
        spawned_remove(game, head.id);
        return 1;
    }
    if ((head.op != OM_SPAWNED_ADD && head.op != OM_SPAWNED_UPDATE) || length <= sizeof(head)) return 0;
    const char *json = (const char *) payload + sizeof(head);
    const size_t n = length - sizeof(head);
    if (json[n - 1] != 0) return 0;
    ObjectManager *om = om_of(game);
    cJSON *record = om ? cJSON_Parse(json) : NULL;
    if (!record) return 0;
    if (head.op == OM_SPAWNED_UPDATE && spawned_object(game, head.id)) return om_replace(game, om, head.id, record); // an edit
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

static int om_add_weapon(Player *player, const int id, const unsigned int caps, NetSpawnResultPacket *out) {
    if (!object_manager_weapon_allowed(id) || !om_weapon(player->game, id)) return OM_STATUS_INVALID;
    if ((id == WEAPON_ID_TOOL_GUN || id == WEAPON_ID_PHYS_GUN) && !(caps & OM_CAP_SPAWN)) return OM_STATUS_DENIED; // the spawn rights' guns
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

// A WEAPON_PICKUP of weapon `id` spawned by `player` with its floor point at `p` (the caps checked,
// unless `dev`: devs have none)
static int om_place_pickup(Game *game, Player *player, const int id, const vec3 p, const int dev, NetSpawnResultPacket *out) {
    ObjectManager *om = om_of(game);
    if (!om || !game->map) return OM_STATUS_INVALID;
    if (dev) {
        // (no caps)
    } else if (things_count(game, om, player->uid, OM_KIND_OBJECT, 1) >= OM_PICKUPS_PER_PLAYER) {
        out->limit = OM_PICKUPS_PER_PLAYER;
        return OM_STATUS_LIMIT;
    } else if (things_count(game, om, player->uid, OM_KIND_OBJECT, 0) >= OM_OBJECTS_PER_PLAYER) {
        out->limit = OM_OBJECTS_PER_PLAYER;
        return OM_STATUS_LIMIT;
    } else if (things_count(game, om, -1, OM_KIND_OBJECT, 0) >= OM_OBJECTS_MAX) {
        out->limit = OM_OBJECTS_MAX;
        return OM_STATUS_LIMIT;
    }
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

static int om_drop_weapon(Game *game, Player *player, const NetSpawnReqPacket *req, const int dev, NetSpawnResultPacket *out) {
    const int id = req->item;
    if (!object_manager_weapon_listed(id) || !om_weapon(game, id) || !game->map) return OM_STATUS_INVALID;
    ObjectManagerAim aim;
    if (!object_manager_aim(game, player, req->pitch, req->yaw, &aim)) return OM_STATUS_NO_TARGET;
    return om_place_pickup(game, player, id, om_floor_point(game, &aim, 3.0f), dev, out);
}

int object_manager_holds_extra(const Player *player) {
    return player && player->loadout && player->extra_weapon_count > 0 && player->loadout_index >= player_loadout_base(player) &&
           player->loadout_index < player->loadout_size;
}

// OM_DROP_TOSS along the look (flat), short of a wall, then down onto the floor below
static vec3 om_toss_point(const Game *game, const Player *player) {
    const vec3 dir = om_look(0.0f, player->direction.y);
    const vec3 from = {player->position.x, player->position.y + 2.0f, player->position.z};
    const vec3 segment = {dir.x * OM_DROP_TOSS, 0.0f, dir.z * OM_DROP_TOSS};
    float t;
    int object;
    float reach = OM_DROP_TOSS;
    if (om_ray(game, from, segment, &t, &object)) reach = fmaxf(0.0f, OM_DROP_TOSS * t - 2.0f);
    vec3 p = {from.x + dir.x * reach, from.y, from.z + dir.z * reach};
    const vec3 down = {0.0f, -OM_AIM_RANGE, 0.0f};
    if (om_ray(game, p, down, &t, &object)) p.y = p.y + down.y * t;
    else p.y = player->position.y;
    return p;
}

// OM_ACTION_DROP_HELD (`drop`, the drop key on an extra slot): the weapon in hand becomes a
// pickup a step in front of the player and leaves the loadout. Only extra slots: the class
// loadout and the melee stay (v7's dropWeapon takes picked-up weapons back to their pickup, never
// a melee, never the last weapon). The Tool Gun is put away, never a pickup.
static int om_drop_held(Game *game, Player *player, const int dev, NetSpawnResultPacket *out) {
    if (!object_manager_holds_extra(player)) return OM_STATUS_KEEP;
    const int id = player->loadout[player->loadout_index];
    out->item = (int16_t) id;
    const Weapon *w = om_weapon(game, id);
    if (!w || w->melee) return OM_STATUS_KEEP;
    if (!object_manager_weapon_listed(id)) return om_remove_weapon(player, id, 0);
    if (!game->map || (game->mode && game->mode->config.no_pickups)) return OM_STATUS_DENIED; // dropWeapon: !mode.noPickups
    const int status = om_place_pickup(game, player, id, om_toss_point(game, player), dev, out);
    return status == OM_STATUS_OK ? om_remove_weapon(player, id, 0) : status;
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

// The feet (bottom centre) of a `size` box placed on the face the crosshair hit: on a floor at the
// hit, under a ceiling, or against a wall on the floor in front of it (dropped down to it).
static vec3 om_place_box(const Game *game, const ObjectManagerAim *aim, const double size[3]) {
    vec3 p = aim->point;
    if (aim->normal.y > 0.5f) return p;
    if (aim->normal.y < -0.5f) {
        p.y -= (float) size[1];
        return p;
    }
    const float half = fabsf(aim->normal.x) > 0.5f ? (float) size[0] * 0.5f : (float) size[2] * 0.5f;
    p.x += aim->normal.x * (half + 0.05f);
    p.z += aim->normal.z * (half + 0.05f);
    const vec3 down = {0.0f, -OM_AIM_RANGE, 0.0f};
    float t;
    int object;
    if (om_ray(game, (vec3) {p.x, p.y + 0.5f, p.z}, down, &t, &object)) p.y = p.y + 0.5f + down.y * t;
    return p;
}

// map.js "d" quarter (ramps, ladders) along the player's look: 0 +x, 1 +z, 2 -x, 3 -z
static int om_look_quarter(const Player *player) {
    const float lx = sinf(player->direction.y + (float) M_PI), lz = cosf(player->direction.y + (float) M_PI);
    if (fabsf(lx) >= fabsf(lz)) return lx > 0.0f ? 0 : 2;
    return lz > 0.0f ? 1 : 3;
}

// A catalogue record placed on the surface at the crosshair (aim) or at `at` (aim NULL), turned to
// face the player (a quarter turn: the AABB collision matches the model; ramps and ladders take
// map.js's "d" instead). Owned by the round's records from here on.
static int om_spawn_record(Game *game, ObjectManager *om, Player *player, const char *json, vec3 at, const ObjectManagerAim *aim) {
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
    const int directed = prefab == PREFAB_RAMP || prefab == PREFAB_LADDER;
    const double quarter = round((player->direction.y + M_PI) / (M_PI / 2.0));
    const int turn = directed ? 0 : ((int) quarter % 4 + 4) % 4;
    if (turn % 2) {
        const double w = size[0];
        size[0] = size[2];
        size[2] = w;
    }
    if (directed) {
        cJSON_DeleteItemFromObject(record, "d");
        cJSON_AddNumberToObject(record, "d", om_look_quarter(player));
    }
    if (aim) at = om_place_box(game, aim, size);
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

static int om_spawn_entry(Game *game, Player *player, const NetSpawnReqPacket *req, const int tab, const int dev, NetSpawnResultPacket *out) {
    int count = 0;
    const ObjectManagerEntry *entries = object_manager_entries(tab, &count);
    if (!entries || req->item < 0 || req->item >= count) return OM_STATUS_INVALID;
    const ObjectManagerEntry *e = &entries[req->item];
    ObjectManager *om = om_of(game);
    if (!om || !game->map) return OM_STATUS_INVALID;
    const int kind = e->kind == OM_ENTRY_RECORD ? OM_KIND_OBJECT : tab == OM_TAB_NPCS ? OM_KIND_NPC : OM_KIND_ENTITY;
    const int limit = dev ? 0 : om_limit(game, om, player, kind); // devs: no caps
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
            GameAIData ghost;
            const GameAIData *data = t ? &t->data : NULL;
            if (!t && e->data == 0) {
                // the Ghost: the built-in kind 0, drawn as v7 draws a map's ghost (an AI_SPAWNER without
                // aid: MapManager.addAI fct 2, fs 5 -> frames 2, frameT 500, MapManager.js:3027-3042).
                // Kind 0's own conf has no frames (AI.js:3-14): the whole two-frame strip, gutter and all.
                game_ai_builtin_data(0, &ghost);
                ghost.frames = 2;
                ghost.frame_time = 500.0f;
                data = &ghost;
            }
            GameAI *ai = game_ai_spawn(game, 0, t ? 0 : e->data, t ? t->name : e->name, at, data);
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
        case OM_ENTRY_RECORD: // objects sit on the face they hit; scraps / machines stand on the floor point
            return om_spawn_record(game, om, player, e->json, at, tab == OM_TAB_OBJECTS ? &aim : NULL) >= 0 ? OM_STATUS_OK : OM_STATUS_INVALID;
        default:
            return OM_STATUS_INVALID;
    }
}

// The tracked thing's box for the crosshair test: (bottom centre, full size); 0 when it is gone.
static int thing_box(Game *game, const OmThing *t, vec3 *pos, vec3 *size) {
    if (!thing_alive(game, t)) return 0;
    if (t->kind == OM_KIND_OBJECT) {
        const Object *o = spawned_object(game, t->id);
        *pos = o->position;
        *size = (vec3) {fmaxf(o->scale.x, 1.0f), fmaxf(o->scale.y, 1.0f), fmaxf(o->scale.z, 1.0f)};
        return 1;
    }
    if (t->kind == OM_KIND_NPC && t->sub == OM_SUB_BOT) {
        const GameBot *bot = bot_by_id(game, t->id);
        const Player *p = bot ? bot->player : NULL;
        if (!p || !p->active) return 0;
        *pos = p->position;
        *size = (vec3) {4.0f, p->height > 0.0f ? p->height : 10.0f, 4.0f};
        return 1;
    }
    if (t->kind == OM_KIND_NPC) {
        const GameAI *ai = game_ai_find(game, t->id);
        *pos = ai->position; // game_ai_line_hit's box
        *size = (vec3) {ai->scale * 2.0f, ai->height * 2.0f, ai->scale * 2.0f};
        return 1;
    }
    if (t->sub == OM_SUB_AIRDROP) {
        const AirDrop *d = &game->zombies.drops[t->id - 1];
        *pos = (vec3) {d->position.x, d->position.y - 1.0f, d->position.z};
        *size = (vec3) {10.0f, 10.0f, 10.0f};
        return 1;
    }
    for (int i = 0; game->mode && i < MODE_MAX_SPAWNABLES; i++) {
        const ModeSpawnable *s = &game->mode->spawnables[i];
        if (!s->active || s->sid != t->id) continue;
        *pos = (vec3) {s->position.x, s->position.y - 4.0f, s->position.z};
        *size = (vec3) {6.0f, 8.0f, 6.0f};
        return 1;
    }
    return 0;
}

// The nearest tracked thing the crosshair reaches before a wall (index into om->things), -1 none;
// *t_out its fraction of OM_AIM_RANGE (2 none)
static int om_aim_thing_t(Game *game, const Player *player, const NetSpawnReqPacket *req, float *t_out) {
    ObjectManager *om = game->object_manager;
    *t_out = 2.0f;
    if (!om || !game->map) return -1;
    const vec3 eye = {player->position.x, player->position.y + player->height - game_constants.camera_height, player->position.z};
    const vec3 dir = om_look(isfinite(req->pitch) ? player_clamp_pitch(req->pitch) : player->direction.x, isfinite(req->yaw) ? req->yaw : player->direction.y);
    const vec3 segment = {dir.x * OM_AIM_RANGE, dir.y * OM_AIM_RANGE, dir.z * OM_AIM_RANGE};
    float wall = 2.0f;
    int wall_object = -1;
    om_ray(game, eye, segment, &wall, &wall_object);
    int best = -1;
    float best_t = 2.0f;
    for (size_t i = 0; i < om->thing_count; i++) {
        vec3 pos, size;
        if (!thing_box(game, &om->things[i], &pos, &size)) continue;
        // the eye inside it (spawned on top of the player) counts as the nearest: line_in_rect only
        // reports boxes ahead of the origin
        const int inside = fabsf(eye.x - pos.x) <= size.x * 0.5f && fabsf(eye.z - pos.z) <= size.z * 0.5f && eye.y >= pos.y && eye.y <= pos.y + size.y;
        const float t = inside ? 1e-6f : line_in_rect(eye, segment, pos, size);
        if (t > 0.0f && t <= 1.0f && t < best_t && t <= wall + 0.001f) {
            best_t = t;
            best = (int) i;
        }
    }
    *t_out = best_t;
    return best;
}

static int om_aim_thing(Game *game, const Player *player, const NetSpawnReqPacket *req) {
    float t;
    return om_aim_thing_t(game, player, req, &t);
}

// What a Tool Gun shot is on: a tracked thing (*thing, an index into om->things) or, with `map`,
// one of the map's own objects (*map_uid) when that is nearer; -1 each when not
static void om_aim_target(Game *game, const Player *player, const NetSpawnReqPacket *req, const int map, int *thing, int *map_uid) {
    float thing_t;
    *thing = om_aim_thing_t(game, player, req, &thing_t);
    *map_uid = -1;
    if (!map) return;
    ObjectManagerAim aim;
    if (!object_manager_pick(game, player, req->pitch, req->yaw, &aim) || !object_manager_is_map_object(game, aim.object)) return;
    const vec3 eye = {player->position.x, player->position.y + player->height - game_constants.camera_height, player->position.z};
    const vec3 d = {aim.point.x - eye.x, aim.point.y - eye.y, aim.point.z - eye.z};
    const float t = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z) / OM_AIM_RANGE;
    if (*thing >= 0 && thing_t <= t) return;
    *thing = -1;
    *map_uid = aim.object;
}
// Who may take back / change a tracked thing: its owner, devs and the host (Remove / Undo's rule)
typedef struct {
    unsigned int caps;
    int host;
} OmWho;

static int om_may_touch(const OmThing *t, const Player *player, const OmWho *who) {
    return t->owner == player->uid || (who->caps & OM_CAP_DEV) || who->host;
}

// A map object `uid` the tools may change for this player: OK, DENIED (no OM_CAP_MAP), UNSUPPORTED
static int om_map_target(const Game *game, const int uid, const OmWho *who) {
    if (!(who->caps & OM_CAP_MAP)) return OM_STATUS_DENIED;
    return object_manager_map_editable(game, uid) ? OM_STATUS_OK : OM_STATUS_UNSUPPORTED;
}

// Deletes map object `uid` as an undo step of `editor` (authority)
static int om_map_delete(Game *game, ObjectManager *om, const int uid, const int editor) {
    om_edit_push(game, om, uid, editor, game->map->objects[uid]->raw, NULL, 0);
    if (!om_map_apply(game, om, uid, NULL)) {
        cJSON_Delete(om->edits[--om->edit_count].pose);
        return OM_STATUS_INVALID;
    }
    om_map_send(game, NET_TARGET_ALL, uid, editor);
    return OM_STATUS_OK;
}

// OM_ACTION_REMOVE_AIM (the Remove button, the Delete key) and the Tool Gun's Deleter: the nearest
// tracked thing at the crosshair; the Deleter (`map`) also takes a map object in front of it for
// those with OM_CAP_MAP (Remove and Delete never touch the map: a stray key costs no geometry)
static int om_remove_aim(Game *game, Player *player, const NetSpawnReqPacket *req, const OmWho *who, const int map) {
    int best, map_uid;
    om_aim_target(game, player, req, map && (who->caps & OM_CAP_MAP), &best, &map_uid);
    if (map_uid >= 0) {
        const int status = om_map_target(game, map_uid, who);
        return status == OM_STATUS_OK ? om_map_delete(game, om_of(game), map_uid, player->uid) : status;
    }
    if (best < 0) return OM_STATUS_NOTHING;
    const OmThing *t = &((ObjectManager *) game->object_manager)->things[best];
    if (!om_may_touch(t, player, who)) return OM_STATUS_DENIED; // someone else's
    thing_remove(game, t);
    return OM_STATUS_OK;
}

// ---------------------------------------------------------------- the Tool Gun's tools

static int tool_deleter(Game *game, Player *player, const NetSpawnReqPacket *req, const OmWho *who, NetSpawnResultPacket *out) {
    (void) out;
    return om_remove_aim(game, player, req, who, 1);
}

// The aimed tracked thing when it is a spawned map object, else a status (*index -1); *map_uid a
// map object when that is what the shot is on (status OK, *index -1)
static int om_aim_object(Game *game, const Player *player, const NetSpawnReqPacket *req, const OmWho *who, int *index, int *map_uid) {
    om_aim_target(game, player, req, (who->caps & OM_CAP_MAP) != 0, index, map_uid);
    if (*map_uid >= 0) return om_map_target(game, *map_uid, who);
    if (*index < 0) return OM_STATUS_NOTHING;
    const OmThing *t = &((ObjectManager *) game->object_manager)->things[*index];
    if (t->kind != OM_KIND_OBJECT || !spawned_object(game, t->id) || !spawned_object(game, t->id)->raw) {
        *index = -1;
        return OM_STATUS_UNSUPPORTED;
    }
    return OM_STATUS_OK;
}

// Rotator: a quarter turn clockwise seen from above, as om_spawn_record faces things: yaw + PI/2
// with w and l swapped (the AABB follows the model), ramps and ladders one map.js "d" step. The
// turned copy replaces the object under a new uid (uids never move mid-round); the undo list
// keeps its place.
static int tool_rotator(Game *game, Player *player, const NetSpawnReqPacket *req, const OmWho *who, NetSpawnResultPacket *out) {
    (void) out;
    int index, map_uid;
    const int status = om_aim_object(game, player, req, who, &index, &map_uid);
    if (status != OM_STATUS_OK) return status;
    ObjectManager *om = om_of(game);
    if (map_uid < 0 && !om_may_touch(&om->things[index], player, who)) return OM_STATUS_DENIED;
    const int old = map_uid >= 0 ? map_uid : om->things[index].id;
    ObjectManagerPose pose, turned;
    if (object_manager_pose(game, old, &pose)) { // in place, an undo step (zombies machines below: a new uid)
        object_manager_pose_turn(&pose, 1, &turned);
        return om_apply_pose(game, om, old, &turned, player->uid, 1);
    }
    if (map_uid >= 0) return OM_STATUS_UNSUPPORTED;
    cJSON *record = cJSON_Duplicate(spawned_object(game, old)->raw, 1);
    if (!record) return OM_STATUS_INVALID;
    const int prefab = map_raw_object_prefab(record);
    if (prefab == PREFAB_RAMP || prefab == PREFAB_LADDER) {
        const int d = (int) cJSON_GetNumberValue(cJSON_GetObjectItem(record, "d"));
        cJSON_DeleteItemFromObject(record, "d");
        cJSON_AddNumberToObject(record, "d", ((d + 1) % 4 + 4) % 4);
    } else {
        double size[3] = {10.0, 10.0, 10.0}, rot[3] = {0.0, 0.0, 0.0};
        const cJSON *sz = cJSON_GetObjectItem(record, "s"), *r = cJSON_GetObjectItem(record, "r");
        for (int k = 0; k < 3; k++) {
            if (cJSON_IsArray(sz)) size[k] = cJSON_GetNumberValue(cJSON_GetArrayItem(sz, k));
            if (cJSON_IsArray(r)) rot[k] = cJSON_GetNumberValue(cJSON_GetArrayItem(r, k));
        }
        const double w = size[0];
        size[0] = size[2];
        size[2] = w;
        rot[1] = fmod(rot[1] + M_PI / 2.0, 2.0 * M_PI);
        rot[1] = round(rot[1] * 100.0) / 100.0;
        cJSON_DeleteItemFromObject(record, "s");
        cJSON_DeleteItemFromObject(record, "r");
        cJSON_AddItemToObject(record, "s", cJSON_CreateDoubleArray(size, 3));
        cJSON_AddItemToObject(record, "r", cJSON_CreateDoubleArray(rot, 3));
    }
    const int owner = om->things[index].owner;
    const int uid = spawned_add(game, om, record, owner, -1);
    if (uid < 0) return OM_STATUS_INVALID;
    spawned_remove(game, old);
    om->things[index].id = uid;
    spawned_send(game, NET_TARGET_ALL, OM_SPAWNED_ADD, uid, game->map->objects[uid]);
    if (game->map->objects[uid]->pickup) game_pickup_send(game, NET_TARGET_ALL, uid);
    return OM_STATUS_OK;
}

// Duplicator: a shot at a spawned object copies it (anyone's: copying changes nothing); the next
// shot places the copy on the surface at the crosshair, facing as the original did, as the
// shooter's own spawn (the object caps apply).
static int tool_duplicator(Game *game, Player *player, const NetSpawnReqPacket *req, const OmWho *who, NetSpawnResultPacket *out) {
    ObjectManager *om = om_of(game);
    if (!om || !game->map) return OM_STATUS_INVALID;
    // om_copy: a spawned object's uid + 1, a map object's -(uid + 1)
    const Object *copy = player->om_copy > 0 ? spawned_object(game, player->om_copy - 1)
                       : player->om_copy < 0 && object_manager_map_editable(game, -player->om_copy - 1) ? game->map->objects[-player->om_copy - 1] : NULL;
    if (!copy || !copy->raw) {
        player->om_copy = 0;
        int index, map_uid;
        // copying changes nothing: anyone with the spawn right copies a map object (the copy is their spawn)
        const OmWho copier = {who->caps | OM_CAP_MAP, who->host};
        const int status = om_aim_object(game, player, req, &copier, &index, &map_uid);
        if (status != OM_STATUS_OK) return status;
        player->om_copy = map_uid >= 0 ? -(map_uid + 1) : om->things[index].id + 1;
        out->limit = OM_TOOL_COPIED;
        return OM_STATUS_OK;
    }
    const int dev = (who->caps & OM_CAP_DEV) != 0; // devs: no caps
    const int limit = dev ? 0 : om_limit(game, om, player, OM_KIND_OBJECT);
    if (limit) {
        out->limit = (uint16_t) limit;
        return OM_STATUS_LIMIT;
    }
    if (!dev && copy->pickup && things_count(game, om, player->uid, OM_KIND_OBJECT, 1) >= OM_PICKUPS_PER_PLAYER) {
        out->limit = OM_PICKUPS_PER_PLAYER;
        return OM_STATUS_LIMIT;
    }
    ObjectManagerAim aim;
    if (!object_manager_aim(game, player, req->pitch, req->yaw, &aim)) return OM_STATUS_NO_TARGET;
    cJSON *record = cJSON_Duplicate(copy->raw, 1);
    if (!record) return OM_STATUS_INVALID;
    om_resolve_palette(game->map, record); // (a map object's "ci": the copy has no palette)
    char *printed = cJSON_PrintUnformatted(record);
    const int fits = printed && strlen(printed) + 32 <= OM_RECORD_MAX; // SPAWNED_OBJ carries it to everyone (+ its new "p")
    free(printed);
    if (!fits) {
        cJSON_Delete(record);
        return OM_STATUS_UNSUPPORTED;
    }
    double size[3] = {copy->scale.x, copy->scale.y, copy->scale.z};
    const cJSON *sz = cJSON_GetObjectItem(record, "s");
    for (int k = 0; cJSON_IsArray(sz) && k < 3; k++) size[k] = cJSON_GetNumberValue(cJSON_GetArrayItem(sz, k));
    const vec3 at = copy->pickup ? om_floor_point(game, &aim, 3.0f) : om_place_box(game, &aim, size);
    const double pos[3] = {round(at.x * 10.0) / 10.0, round(at.y * 10.0) / 10.0, round(at.z * 10.0) / 10.0};
    cJSON_DeleteItemFromObject(record, "p");
    cJSON_AddItemToObject(record, "p", cJSON_CreateDoubleArray(pos, 3));
    const int uid = spawned_add(game, om, record, player->uid, -1);
    if (uid < 0) return OM_STATUS_INVALID;
    thing_push(om, OM_KIND_OBJECT, 0, uid, player->uid);
    spawned_send(game, NET_TARGET_ALL, OM_SPAWNED_ADD, uid, game->map->objects[uid]);
    if (game->map->objects[uid]->pickup) game_pickup_send(game, NET_TARGET_ALL, uid);
    player->om_copy = 0;
    out->limit = OM_TOOL_PASTED;
    return OM_STATUS_OK;
}

static const char *om_player_name(const Game *game, const int uid, char *buf, const size_t n) {
    for (size_t i = 0; i < game->player_count; i++) {
        const Player *p = game->players[i];
        if (p && p->uid == uid) {
            snprintf(buf, n, "%s", p->name[0] ? p->name : "Guest");
            return buf;
        }
    }
    snprintf(buf, n, "#%d (left)", uid);
    return buf;
}

// "CRATE" -> "Crate", "WEAPON_PICKUP" -> "Weapon Pickup"
static void om_title(const char *in, char *out, const size_t n) {
    size_t len = 0;
    int start = 1;
    for (; in && *in && len + 1 < n; in++) {
        const char c = *in == '_' ? ' ' : *in;
        out[len++] = (char) (start ? toupper((unsigned char) c) : tolower((unsigned char) c));
        start = c == ' ';
    }
    out[len] = 0;
}

// Info: what the crosshair is on, as a NOTIFY to the shooter (the client toasts it)
static int tool_info(Game *game, Player *player, const NetSpawnReqPacket *req, const OmWho *who, NetSpawnResultPacket *out) {
    (void) who;
    NetNotifyPacket notify;
    memset(&notify, 0, sizeof(notify));
    char what[64] = "", owner[PLAYER_NAME_SIZE + 16];
    int index, map_uid;
    om_aim_target(game, player, req, 1, &index, &map_uid); // (Info reads only: the map's objects for everyone)
    if (map_uid >= 0) { // a map object (any object the pick ray takes, solid or not)
        const Object *o = game->map->objects[map_uid];
        om_title(prefab_name(o->prefab), what, sizeof(what));
        const int edited = object_manager_map_edited(game, map_uid);
        snprintf(notify.text, sizeof(notify.text), "%s, object %d (part of the map%s%s)", what[0] ? what : "Object", map_uid,
                 edited ? ", edited" : "", !object_manager_map_editable(game, map_uid) ? ", fixed" : "");
    } else if (index >= 0) {
        const OmThing *t = &((ObjectManager *) game->object_manager)->things[index];
        if (t->kind == OM_KIND_OBJECT) {
            const Object *o = spawned_object(game, t->id);
            om_title(o ? prefab_name(o->prefab) : NULL, what, sizeof(what));
            if (o && o->raw && o->prefab == PREFAB_WEAPON_PICKUP) {
                const Weapon *w = om_weapon(game, (int) cJSON_GetNumberValue(cJSON_GetObjectItem(o->raw, "w")));
                if (w) snprintf(what, sizeof(what), "%s Pickup", w->name);
            }
            snprintf(notify.text, sizeof(notify.text), "%s, object %d, owner %s", what[0] ? what : "Object", t->id, om_player_name(game, t->owner, owner, sizeof(owner)));
        } else if (t->kind == OM_KIND_NPC && t->sub == OM_SUB_BOT) {
            const GameBot *bot = bot_by_id(game, t->id);
            snprintf(notify.text, sizeof(notify.text), "Bot %s, bot %d, owner %s", bot && bot->player ? bot->player->name : "?", t->id,
                     om_player_name(game, t->owner, owner, sizeof(owner)));
        } else if (t->kind == OM_KIND_NPC) {
            const GameAI *ai = game_ai_find(game, t->id);
            snprintf(notify.text, sizeof(notify.text), "NPC %s, sid %d, owner %s", ai ? ai->name : "?", t->id, om_player_name(game, t->owner, owner, sizeof(owner)));
        } else {
            snprintf(notify.text, sizeof(notify.text), "%s, sid %d, owner %s", t->sub == OM_SUB_AIRDROP ? "Airdrop" : "Spawnable", t->id,
                     om_player_name(game, t->owner, owner, sizeof(owner)));
        }
    } else {
        ObjectManagerAim aim;
        if (!object_manager_aim(game, player, req->pitch, req->yaw, &aim)) return OM_STATUS_NO_TARGET;
        if (aim.object < 0) snprintf(notify.text, sizeof(notify.text), "Terrain (part of the map)");
        else {
            om_title(prefab_name(game->map->objects[aim.object]->prefab), what, sizeof(what));
            snprintf(notify.text, sizeof(notify.text), "%s, object %d (part of the map)", what[0] ? what : "Object", aim.object);
        }
    }
    game_send(game, player->uid, NET_PACKET_NOTIFY, &notify, sizeof(notify));
    out->limit = OM_TOOL_SHOWN;
    return OM_STATUS_OK;
}

typedef int (*OmToolFn)(Game *, Player *, const NetSpawnReqPacket *, const OmWho *, NetSpawnResultPacket *);
static const OmToolFn k_tool_fns[OM_TOOL_COUNT] = {
    [OM_TOOL_DELETER] = tool_deleter,
    [OM_TOOL_DUPLICATOR] = tool_duplicator,
    [OM_TOOL_ROTATOR] = tool_rotator,
    [OM_TOOL_INFO] = tool_info,
};

// OM_ACTION_TOOL: the Tool Gun in hand, then the tool's own function
static int om_tool(Game *game, Player *player, const NetSpawnReqPacket *req, const OmWho *who, NetSpawnResultPacket *out) {
    if (req->item < 0 || req->item >= OM_TOOL_COUNT || !k_tool_fns[req->item]) return OM_STATUS_INVALID;
    if (!player->weapon || !player->weapon->kn_tool) return OM_STATUS_NO_TOOL;
    return k_tool_fns[req->item](game, player, req, who, out);
}

// The newest of the player's spawns and edits goes: a spawn is removed, an edit's pose restored
static int om_undo(Game *game, Player *player, const int any) {
    ObjectManager *om = game->object_manager;
    const OmThing *thing = NULL;
    for (size_t i = om ? om->thing_count : 0; i-- > 0;) {
        const OmThing *t = &om->things[i];
        if ((!any && t->owner != player->uid) || !thing_alive(game, t)) continue;
        thing = t;
        break;
    }
    long edit = -1;
    for (size_t i = om ? om->edit_count : 0; i-- > 0;) {
        const OmEdit *e = &om->edits[i];
        if ((!any && e->editor != player->uid) || (!e->map && !object_manager_editable(game, e->uid))) continue;
        edit = (long) i;
        break;
    }
    if (edit >= 0 && (!thing || om->edits[edit].seq > thing->seq)) return om_edit_undo(game, om, (size_t) edit) ? OM_STATUS_OK : OM_STATUS_INVALID;
    if (!thing) return OM_STATUS_NOTHING;
    thing_remove(game, thing);
    return OM_STATUS_OK;
}

// ---------------------------------------------------------------- edits (authority)

// May `player` edit object `uid` (OM_CAP_SPAWN checked): a map object with OM_CAP_MAP (devs and the
// host), a spawned one of theirs (any for devs and the host, Remove's rule). OK or the refusal.
static int om_edit_target(Game *game, const Player *player, const int uid, const OmWho *who) {
    const ObjectManager *om = game->object_manager;
    if (object_manager_is_map_object(game, uid)) {
        const int status = om_map_target(game, uid, who);
        return status != OM_STATUS_OK ? status : object_manager_editable(game, uid) ? OM_STATUS_OK : OM_STATUS_INVALID;
    }
    const OmThing *t = om ? om_object_thing(om, uid) : NULL;
    if (!t || !object_manager_editable(game, uid)) return spawned_object(game, uid) ? OM_STATUS_UNSUPPORTED : OM_STATUS_NOTHING;
    return om_may_touch(t, player, who) ? OM_STATUS_OK : OM_STATUS_DENIED;
}

// The edited box `to` (from `from`) within the editor's reach and out of every player (not for devs:
// they move things into and through players): OK, BOUNDS, BLOCKED
static int om_pose_allowed(const Game *game, const Player *player, const Object *o, const ObjectManagerPose *from, const ObjectManagerPose *to,
                           const int dev) {
    const vec3 eye = {player->position.x, player->position.y + player->height - game_constants.camera_height, player->position.z};
    // the box's nearest point (a big map object's centre may well be far away)
    const vec3 c = {fmaxf(fabsf(to->position.x - eye.x) - to->size.x * 0.5f, 0.0f),
                    eye.y < to->position.y ? to->position.y - eye.y : fmaxf(eye.y - (to->position.y + to->size.y), 0.0f),
                    fmaxf(fabsf(to->position.z - eye.z) - to->size.z * 0.5f, 0.0f)};
    if (sqrtf(c.x * c.x + c.y * c.y + c.z * c.z) > OM_EDIT_RANGE) return OM_STATUS_BOUNDS;
    const int solid = !dev && o->collision_type != COLLISION_TYPE_NONE && bot_object_blocks_player(o); // (pads, zones, triggers: walk-through)
    for (size_t i = 0; solid && i < game->player_count; i++) { // never into a player (one already inside may stay)
        const Player *p = game->players[i];
        if (!p || !p->active) continue;
        if (om_box_hits_player(p, to->position, to->size) && !om_box_hits_player(p, from->position, from->size)) return OM_STATUS_BLOCKED;
    }
    return OM_STATUS_OK;
}

// ---------------------------------------------------------------- the Physics Gun on players (devs)

static Player *om_player_by_uid(Game *game, const int uid) {
    for (size_t i = 0; i < game->player_count; i++) {
        if (game->players[i] && game->players[i]->uid == uid) return game->players[i];
    }
    return NULL;
}

static void om_hold_send(Game *game, const Player *player, const int on) {
    const NetCheatPacket packet = {.flag = NET_CHEAT_HELD, .on = (uint8_t) !!on};
    game_send(game, player->uid, NET_PACKET_CHEAT, &packet, sizeof(packet));
}

void object_manager_hold_release(Game *game, Player *player) {
    if (!game || !player || !player->om_held) return;
    player->om_held = 0;
    player->om_held_by = 0;
    if (game_is_authority(game)) om_hold_send(game, player, 0); // its own client moves it again
}

// The authority's side of a carry: `target` stays at the carried feet, its own input moving nothing
// (mode_input_locked) until the carrier lets go or stops updating
void object_manager_hold_tick(Game *game, Player *player) {
    if (!game || !player || !player->om_held || !game_is_authority(game)) return;
    const Player *carrier = om_player_by_uid(game, player->om_held_by - 1);
    if (!player->active || !carrier || !carrier->active || !carrier->weapon || !carrier->weapon->kn_physgun || game->now > player->om_held_until) {
        object_manager_hold_release(game, player);
        return;
    }
    player->position = player->om_hold_pos;
    player->last_position = player->om_hold_pos;
    player->velocity = (vec3) {0.0f, 0.0f, 0.0f};
}

// OM_EDIT_PLAYER: a dev's Physics Gun on another player (any spawned player or bot): held at the
// edit's feet (server-authoritative: the carried player's own client snaps there, everyone else sees
// its state), let go with OM_EDIT_RELEASE. God mode and the rest of the rules stay the player's.
static int om_edit_player(Game *game, Player *player, const NetSpawnEditPacket *edit, const OmWho *who) {
    if (!(who->caps & OM_CAP_DEV)) return OM_STATUS_DENIED; // devs only: not the host, not a guest
    if (edit->req.item != OM_EDIT_SOURCE_PHYSGUN) return OM_STATUS_INVALID;
    if (!player->weapon || !player->weapon->kn_physgun) return OM_STATUS_NO_TOOL;
    Player *target = om_player_by_uid(game, edit->uid);
    if (!target || target == player) return OM_STATUS_NOTHING;
    if (edit->flags & OM_EDIT_RELEASE) {
        if (target->om_held && target->om_held_by == player->uid + 1) object_manager_hold_release(game, target);
        return OM_STATUS_OK;
    }
    if (!target->active) return OM_STATUS_NOTHING;
    if (target->om_held && target->om_held_by != player->uid + 1 && game->now <= target->om_held_until) return OM_STATUS_DENIED; // another dev has them
    const vec3 feet = {edit->position[0], edit->position[1], edit->position[2]};
    if (!isfinite(feet.x) || !isfinite(feet.y) || !isfinite(feet.z)) return OM_STATUS_BOUNDS;
    const vec3 eye = {player->position.x, player->position.y + player->height - game_constants.camera_height, player->position.z};
    const vec3 d = {feet.x - eye.x, feet.y - eye.y, feet.z - eye.z};
    if (sqrtf(d.x * d.x + d.y * d.y + d.z * d.z) > OM_EDIT_RANGE) return OM_STATUS_BOUNDS;
    const int grab = !target->om_held;
    const vec3 was = target->om_held ? target->om_hold_pos : target->position;
    target->om_held = 1;
    target->om_held_by = player->uid + 1;
    target->om_held_until = game->now + OM_HOLD_TIMEOUT;
    target->om_hold_pos = feet;
    if (grab) om_hold_send(game, target, 1);
    const float moved = fabsf(was.x - feet.x) + fabsf(was.y - feet.y) + fabsf(was.z - feet.z);
    if (grab || moved > 0.01f) game_change_position(game, target, feet, 1, 1, 0, 0); // its own client snaps there
    return OM_STATUS_OK;
}

// ---------------------------------------------------------------- the Physics Gun's reach

static int om_level_geometry(const int prefab) {
    const PrefabInfo *info = prefab_info(prefab);
    return !info || (info->geometry != PREFAB_GEO_MODEL && info->geometry != PREFAB_GEO_SPECIAL);
}

int object_manager_physgun_takes(const Game *game, const int uid, const int dev) {
    if (!object_manager_editable(game, uid)) return 0;
    const Object *o = game->map->objects[uid];
    const int spawned = !object_manager_is_map_object(game, uid);
    // the user (R185): props only, spawned ones too; a dev's gun keeps every spawned object (and players,
    // om_edit_player) as part of the dev privileges (R184)
    if (spawned && dev) return 1;
    // a stand-in keeps the original's prefab; the size cap backstops big models (a whole building)
    if (om_level_geometry(o->prefab)) return 0;
    if (spawned) return 1; // a spawned prop: the spawner's to carry, whatever its size
    const vec3 s = object_box_size(o);
    return s.x <= OM_PHYSGUN_MAP_SIZE_MAX && s.y <= OM_PHYSGUN_MAP_SIZE_MAX && s.z <= OM_PHYSGUN_MAP_SIZE_MAX;
}

void object_manager_edit(Game *game, Player *player, const NetSpawnEditPacket *edit, const ObjectManagerPerms *perms,
                         NetSpawnResultPacket *out) {
    memset(out, 0, sizeof(*out));
    const unsigned int caps = object_manager_caps(game, perms);
    out->action = OM_ACTION_EDIT;
    out->item = edit->req.item;
    out->caps = (uint8_t) caps;
    out->extra_max = PLAYER_EXTRA_WEAPONS_MAX;
    out->status = OM_STATUS_OK;
    if (!player || !player->active || !game_is_authority(game) || !game->map) {
        out->status = OM_STATUS_DEAD;
        return;
    }
    if (!(caps & OM_CAP_SPAWN)) {
        out->status = OM_STATUS_DENIED;
        return;
    }
    if (edit->flags & OM_EDIT_FIELDS) { // a field patch comes with its JSON (object_manager_edit_fields)
        out->status = OM_STATUS_INVALID;
        return;
    }
    const OmWho who = {caps, perms && perms->host};
    const int dev = (caps & OM_CAP_DEV) != 0;
    if (edit->flags & OM_EDIT_PLAYER) { // the Physics Gun on a player (devs): never paced, no undo step
        out->status = (uint8_t) om_edit_player(game, player, edit, &who);
        return;
    }
    // its own clock: a drag sends up to 20 edits a second, the request bucket stays for the rest;
    // devs are never paced (their client sends up to 60 a second)
    const float now = game->now;
    if (!dev && player->om_edit_at > 0.0f && now >= player->om_edit_at && now - player->om_edit_at < OM_EDIT_INTERVAL) {
        out->status = OM_STATUS_RATE;
        return;
    }
    const Weapon *w = player->weapon;
    if (!w || (edit->req.item == OM_EDIT_SOURCE_PHYSGUN ? !w->kn_physgun : !w->kn_tool)) {
        out->status = OM_STATUS_NO_TOOL;
        return;
    }
    ObjectManager *om = om_of(game);
    const int uid = edit->uid;
    ObjectManagerPose from;
    const int target = om ? om_edit_target(game, player, uid, &who) : OM_STATUS_INVALID;
    if (target != OM_STATUS_OK || !object_manager_pose(game, uid, &from)) {
        out->status = (uint8_t) (target != OM_STATUS_OK ? target : OM_STATUS_INVALID);
        return;
    }
    if (edit->req.item == OM_EDIT_SOURCE_PHYSGUN && !object_manager_physgun_takes(game, uid, dev)) { // props, not the level
        out->status = OM_STATUS_GEOMETRY;
        return;
    }
    ObjectManagerPose to = from;
    to.position = (vec3) {edit->position[0], edit->position[1], edit->position[2]};
    to.size = (vec3) {edit->size[0], edit->size[1], edit->size[2]};
    to.quarter = edit->quarter;
    if (from.directed) to.yaw = (float) (edit->quarter % 4) * ((float) M_PI / 2.0f);
    else if (edit->flags & OM_EDIT_YAW) { // any yaw; the quarter (the collision's size) is the nearest one's
        to.yaw = object_manager_yaw_unpack(edit->reserved);
        to.quarter = object_manager_yaw_quarter(to.yaw);
    } else { // quarter turns only (the yaw turned by the quarters it changed by)
        const float two_pi = 2.0f * (float) M_PI;
        to.yaw = fmodf(fmodf(from.yaw + (float) ((edit->quarter - from.quarter) % 4) * ((float) M_PI / 2.0f), two_pi) + two_pi, two_pi);
    }
    const float v[6] = {to.position.x, to.position.y, to.position.z, to.size.x, to.size.y, to.size.z};
    int ok = edit->quarter < 4;
    for (int k = 0; k < 6; k++) ok &= isfinite(v[k]) != 0;
    const float had[3] = {from.size.x, from.size.y, from.size.z}, had_turned[3] = {from.size.z, from.size.y, from.size.x};
    for (int k = 3; k < 6; k++) { // (a map object bigger than the limit keeps its size: it may not grow)
        const float cap = fmaxf(OM_EDIT_SIZE_MAX, fmaxf(had[k - 3], had_turned[k - 3]));
        ok &= v[k] >= fminf(OM_EDIT_SIZE_MIN, fminf(had[k - 3], had_turned[k - 3])) - 0.001f && v[k] <= cap + 0.001f;
    }
    if (!ok) {
        out->status = OM_STATUS_BOUNDS;
        return;
    }
    Object *o = game->map->objects[uid];
    if (edit->flags & OM_EDIT_DROP) { // down onto whatever is below (itself out of the way)
        const int collision = o->collision_type;
        o->collision_type = COLLISION_TYPE_NONE;
        const vec3 down = {0.0f, -OM_AIM_RANGE, 0.0f};
        float hit;
        int object;
        if (om_ray(game, (vec3) {to.position.x, to.position.y + 0.1f, to.position.z}, down, &hit, &object)) to.position.y = to.position.y + 0.1f + down.y * hit;
        o->collision_type = (unsigned char) collision;
    }
    const int where = om_pose_allowed(game, player, o, &from, &to, dev);
    if (where != OM_STATUS_OK) {
        out->status = (uint8_t) where;
        return;
    }
    player->om_edit_at = now > 0.0f ? now : 1e-6f;
    const int step = (edit->flags & OM_EDIT_BEGIN) || !om_edit_open(om, uid, player->uid);
    out->status = (uint8_t) om_apply_pose(game, om, uid, &to, player->uid, step);
}

// The Properties tool's field patch (OM_EDIT_FIELDS)
static int om_edit_fields(Game *game, Player *player, const NetSpawnEditPacket *edit, const char *json, const size_t length, const OmWho *who) {
    if (edit->req.item != OM_EDIT_SOURCE_BTOOLS || !(edit->flags & OM_EDIT_FIELDS)) return OM_STATUS_INVALID;
    if (!json || length < 3 || length > OM_FIELDS_MAX + 1 || json[length - 1] != 0 || strlen(json) != length - 1) return OM_STATUS_INVALID;
    const float now = game->now;
    if (!(who->caps & OM_CAP_DEV) && player->om_edit_at > 0.0f && now >= player->om_edit_at && now - player->om_edit_at < OM_EDIT_INTERVAL)
        return OM_STATUS_RATE;
    if (!player->weapon || !player->weapon->kn_tool) return OM_STATUS_NO_TOOL;
    ObjectManager *om = om_of(game);
    const int uid = edit->uid;
    const int target = om ? om_edit_target(game, player, uid, who) : OM_STATUS_INVALID;
    if (target != OM_STATUS_OK) return target;
    const Object *o = game->map->objects[uid];
    ObjectManagerPose from, to;
    om_pose_of(o, &from);
    cJSON *patch = cJSON_Parse(json);
    cJSON *base = patch ? object_manager_record(game, uid) : NULL; // (a map object's "ci" resolved: a colour edit replaces it)
    cJSON *next = base ? cJSON_Duplicate(base, 1) : NULL;
    if (!next) {
        cJSON_Delete(patch);
        cJSON_Delete(base);
        return OM_STATUS_INVALID;
    }
    int pose = 0;
    int status = object_manager_fields_patch(next, o->prefab, patch, &pose);
    cJSON_Delete(patch);
    // the pose the patch leaves: the reach (the box as it is when the patch moves nothing) and no box into a player
    to = from;
    const cJSON *p = cJSON_GetObjectItem(next, "p"), *s = cJSON_GetObjectItem(next, "s");
    for (int k = 0; pose && k < 3; k++) {
        if (cJSON_GetArraySize(p) >= 3) (&to.position.x)[k] = (float) cJSON_GetNumberValue(cJSON_GetArrayItem(p, k));
        if (cJSON_GetArraySize(s) >= 3) (&to.size.x)[k] = (float) cJSON_GetNumberValue(cJSON_GetArrayItem(s, k));
    }
    if (status == OM_STATUS_OK) status = om_pose_allowed(game, player, o, &from, &to, (who->caps & OM_CAP_DEV) != 0);
    const int same = cJSON_Compare(next, base, 1);
    cJSON_Delete(base);
    if (status != OM_STATUS_OK || same) { // (nothing changed: no undo step, nothing sent)
        cJSON_Delete(next);
        return status;
    }
    player->om_edit_at = now > 0.0f ? now : 1e-6f;
    return om_apply_record(game, om, uid, next, player->uid, 1, pose);
}

void object_manager_edit_fields(Game *game, Player *player, const NetSpawnEditPacket *edit, const char *json, const size_t length,
                                const ObjectManagerPerms *perms, NetSpawnResultPacket *out) {
    memset(out, 0, sizeof(*out));
    const unsigned int caps = object_manager_caps(game, perms);
    out->action = OM_ACTION_EDIT;
    out->item = edit->req.item;
    out->caps = (uint8_t) caps;
    out->extra_max = PLAYER_EXTRA_WEAPONS_MAX;
    out->limit = OM_FIELDS_ANSWER;
    if (!player || !player->active || !game_is_authority(game) || !game->map) out->status = OM_STATUS_DEAD;
    else if (!(caps & OM_CAP_SPAWN)) out->status = OM_STATUS_DENIED;
    else {
        const OmWho who = {caps, perms && perms->host};
        out->status = (uint8_t) om_edit_fields(game, player, edit, json, length, &who);
    }
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
    if (req->action >= OM_ACTION_COUNT || req->action == OM_ACTION_EDIT) { // (an edit is a NetSpawnEditPacket: object_manager_edit)
        out->status = OM_STATUS_INVALID;
        return;
    }
    if (!player || !player->active || !game_is_authority(game)) {
        out->status = OM_STATUS_DEAD;
        return;
    }
    const int weapons = req->action <= OM_ACTION_DROP_WEAPON || req->action == OM_ACTION_DROP_HELD;
    const int takes_back = req->action == OM_ACTION_UNDO || req->action == OM_ACTION_REMOVE_AIM;
    if (!(caps & (weapons ? OM_CAP_WEAPONS : OM_CAP_SPAWN)) &&
        !(takes_back && (caps & OM_CAP_WEAPONS)) && // undo / remove also take back pickups
        !(req->action == OM_ACTION_REMOVE_WEAPON || req->action == OM_ACTION_CLEAR_WEAPONS) && // giving back is always fine
        !(req->action == OM_ACTION_DROP_HELD && player->weapon && (player->weapon->kn_tool || player->weapon->kn_physgun))) { // (the guns put away too)
        out->status = OM_STATUS_DENIED;
        return;
    }
    const OmWho who = {caps, perms && perms->host};
    const int dev = (caps & OM_CAP_DEV) != 0;
    if (!dev && !om_take_token(player)) { // devs: no pace
        out->status = OM_STATUS_RATE;
        return;
    }
    switch (req->action) {
        case OM_ACTION_ADD_WEAPON: out->status = (uint8_t) om_add_weapon(player, req->item, caps, out); break;
        case OM_ACTION_REMOVE_WEAPON: out->status = (uint8_t) om_remove_weapon(player, req->item, 0); break;
        case OM_ACTION_CLEAR_WEAPONS: out->status = (uint8_t) om_remove_weapon(player, -1, 1); break;
        case OM_ACTION_DROP_WEAPON: out->status = (uint8_t) om_drop_weapon(game, player, req, dev, out); break;
        case OM_ACTION_UNDO: out->status = (uint8_t) om_undo(game, player, 0); break;
        case OM_ACTION_SPAWN_ENTITY: out->status = (uint8_t) om_spawn_entry(game, player, req, OM_TAB_ENTITIES, dev, out); break;
        case OM_ACTION_SPAWN_NPC: out->status = (uint8_t) om_spawn_entry(game, player, req, OM_TAB_NPCS, dev, out); break;
        case OM_ACTION_SPAWN_OBJECT: out->status = (uint8_t) om_spawn_entry(game, player, req, OM_TAB_OBJECTS, dev, out); break;
        case OM_ACTION_REMOVE_AIM: out->status = (uint8_t) om_remove_aim(game, player, req, &who, 0); break;
        case OM_ACTION_DROP_HELD: out->status = (uint8_t) om_drop_held(game, player, dev, out); break;
        case OM_ACTION_TOOL: out->status = (uint8_t) om_tool(game, player, req, &who, out); break;
        default: out->status = OM_STATUS_INVALID; break;
    }
}

// ---------------------------------------------------------------- a dev not spawned in (authority)

void object_manager_menu_view(const Game *game, const Player *last, const PlayerKeep *keep, const int uid, ObjectManagerView *out) {
    memset(out, 0, sizeof(*out));
    out->uid = uid;
    if (last) { // dead: where the last life ended
        out->eye = (vec3) {last->position.x, last->position.y + (last->height > 0.0f ? last->height : game_constants.player_height) - game_constants.camera_height,
                           last->position.z};
        out->direction = last->direction;
    } else if (keep && keep->has_view) {
        out->eye = keep->view_eye;
        out->direction = keep->view_direction;
    } else if (game && game->map) { // the menu camera (map camPos; its pose when the map pins one)
        out->eye = game->map->camera_position;
        out->direction = (vec2) {game->map->camera_static ? game->map->camera_rotation.x : -0.35f, game->map->camera_rotation.y};
    }
}

static int om_extras_find(const int *extras, const int count, const int id) {
    for (int i = 0; i < count; i++) {
        if (extras[i] == id) return i;
    }
    return -1;
}

// The connection's extra weapon list (applied at the next spawn): add / remove / clear
static int om_view_weapons(Game *game, const NetSpawnReqPacket *req, const unsigned int caps, int *extras, int *extra_count, NetSpawnResultPacket *out) {
    if (!extras || !extra_count) return OM_STATUS_DEAD;
    const int id = req->item;
    if (req->action == OM_ACTION_CLEAR_WEAPONS) {
        *extra_count = 0;
        return OM_STATUS_OK;
    }
    const int at = om_extras_find(extras, *extra_count, id);
    if (req->action == OM_ACTION_REMOVE_WEAPON) {
        if (at < 0) return OM_STATUS_NOTHING;
        memmove(extras + at, extras + at + 1, (size_t) (*extra_count - at - 1) * sizeof(int));
        (*extra_count)--;
        return OM_STATUS_OK;
    }
    if (!object_manager_weapon_allowed(id) || !om_weapon(game, id)) return OM_STATUS_INVALID;
    if ((id == WEAPON_ID_TOOL_GUN || id == WEAPON_ID_PHYS_GUN) && !(caps & OM_CAP_SPAWN)) return OM_STATUS_DENIED;
    if (at >= 0) return OM_STATUS_OK;
    if (*extra_count >= PLAYER_EXTRA_WEAPONS_MAX) {
        out->limit = PLAYER_EXTRA_WEAPONS_MAX;
        return OM_STATUS_LIMIT;
    }
    extras[(*extra_count)++] = id;
    return OM_STATUS_OK;
}

void object_manager_request_view(Game *game, const ObjectManagerView *view, const NetSpawnReqPacket *req, const ObjectManagerPerms *perms,
                                 int *extras, int *extra_count, NetSpawnResultPacket *out) {
    memset(out, 0, sizeof(*out));
    const unsigned int caps = object_manager_caps(game, perms);
    out->action = req->action;
    out->item = req->item;
    out->caps = (uint8_t) caps;
    out->extra_max = PLAYER_EXTRA_WEAPONS_MAX;
    out->status = OM_STATUS_OK;
    if (req->action == OM_ACTION_QUERY) return;
    if (req->action >= OM_ACTION_COUNT || req->action == OM_ACTION_EDIT) {
        out->status = OM_STATUS_INVALID;
        return;
    }
    // only devs act without a live player (the host and guests: "Spawn first", as before)
    if (!(caps & OM_CAP_DEV) || !view || !game_is_authority(game) || !game->map) {
        out->status = OM_STATUS_DEAD;
        return;
    }
    switch (req->action) {
        case OM_ACTION_ADD_WEAPON:
        case OM_ACTION_REMOVE_WEAPON:
        case OM_ACTION_CLEAR_WEAPONS:
            out->status = (uint8_t) om_view_weapons(game, req, caps, extras, extra_count, out);
            return;
        case OM_ACTION_DROP_WEAPON:
        case OM_ACTION_UNDO:
        case OM_ACTION_SPAWN_ENTITY:
        case OM_ACTION_SPAWN_NPC:
        case OM_ACTION_SPAWN_OBJECT:
        case OM_ACTION_REMOVE_AIM:
            break;
        default: // the guns' actions need them in hand
            out->status = OM_STATUS_DEAD;
            return;
    }
    // a stand-in player at the view (never in game->players): the owner of what it spawns is the
    // connection, so Undo / Remove take it back from the next life too
    Player *p = player_init(game);
    if (!p) {
        out->status = OM_STATUS_INVALID;
        return;
    }
    p->uid = view->uid;
    p->active = 1;
    p->height = game_constants.player_height;
    p->position = (vec3) {view->eye.x, view->eye.y - (p->height - game_constants.camera_height), view->eye.z};
    NetSpawnReqPacket r = *req; // (the client's camera; a non-finite look is the view's)
    if (!isfinite(r.pitch)) r.pitch = view->direction.x;
    if (!isfinite(r.yaw)) r.yaw = view->direction.y;
    p->direction = (vec2) {player_clamp_pitch(r.pitch), r.yaw};
    const OmWho who = {caps, perms && perms->host};
    for (int attempt = 0; attempt < 2; attempt++) {
        switch (req->action) {
            case OM_ACTION_DROP_WEAPON: out->status = (uint8_t) om_drop_weapon(game, p, &r, 1, out); break;
            case OM_ACTION_UNDO: out->status = (uint8_t) om_undo(game, p, 0); break;
            case OM_ACTION_SPAWN_ENTITY: out->status = (uint8_t) om_spawn_entry(game, p, &r, OM_TAB_ENTITIES, 1, out); break;
            case OM_ACTION_SPAWN_NPC: out->status = (uint8_t) om_spawn_entry(game, p, &r, OM_TAB_NPCS, 1, out); break;
            case OM_ACTION_SPAWN_OBJECT: out->status = (uint8_t) om_spawn_entry(game, p, &r, OM_TAB_OBJECTS, 1, out); break;
            case OM_ACTION_REMOVE_AIM: out->status = (uint8_t) om_remove_aim(game, p, &r, &who, 0); break;
            default: break;
        }
        if (out->status != OM_STATUS_NO_TARGET) break;
        r.pitch = -(float) M_PI / 2.0f; // the look meets nothing (a menu camera over the sky): straight down from the view
        p->direction.x = player_clamp_pitch(r.pitch);
    }
    free(p); // (player_init allocates nothing else)
}

// ---------------------------------------------------------------- client interpolation (pure)

void object_manager_interp_push(ObjectManagerInterp *it, const vec3 shown, const float shown_yaw, const vec3 to, const float yaw, const double now) {
    // the gap between updates, smoothed (frames quantise each arrival: a raw gap jitters by a frame)
    double gap = it->last_update > 0.0 ? now - it->last_update : OM_INTERP_SPAN_MAX * 0.4;
    if (gap < OM_INTERP_SPAN_MIN) gap = OM_INTERP_SPAN_MIN;
    if (gap > OM_INTERP_SPAN_MAX) gap = OM_INTERP_SPAN_MAX;
    const double span = it->last_update > 0.0 && it->span > 0.0 && gap < OM_INTERP_SPAN_MAX ? it->span * 0.75 + gap * 0.25 : gap;
    it->from = shown;
    it->yaw_from = shown_yaw;
    it->to = to;
    it->yaw_to = yaw;
    it->start = now;
    it->span = span;
    it->last_update = now;
    it->active = 1;
}

int object_manager_interp_sample(ObjectManagerInterp *it, const double now, vec3 *pos, float *yaw) {
    double t = it->active && it->span > 0.0 ? (now - it->start) / it->span : 1.0;
    if (t < 0.0) t = 0.0;
    if (t >= 1.0) {
        it->active = 0;
        if (pos) *pos = it->to;
        if (yaw) *yaw = it->yaw_to;
        return 0;
    }
    const float f = (float) t;
    if (pos) *pos = (vec3) {it->from.x + (it->to.x - it->from.x) * f, it->from.y + (it->to.y - it->from.y) * f, it->from.z + (it->to.z - it->from.z) * f};
    if (yaw) { // the short way round
        const float two_pi = 2.0f * (float) M_PI;
        float d = fmodf(it->yaw_to - it->yaw_from, two_pi);
        if (d > (float) M_PI) d -= two_pi;
        if (d < -(float) M_PI) d += two_pi;
        *yaw = it->yaw_from + d * f;
    }
    return 1;
}
