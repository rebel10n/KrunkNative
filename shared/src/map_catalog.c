// The KrunkNative map catalogue (shared/include/map_catalog.h): Krunker v7.1.4's 32 official maps
// plus the community maps, among them the v3.1.1 official maps as "311_<key>" and v3.1.1's
// old_* maps (the v1.5.2 era) as "152_<key>".
//
// Sources: Krunker_v7.1.4/src/data/maps.js (the official list: name, fileName; the index is the
// v7 mapIndex) and data/map.js rotationMaps; Krunker_v3.1.1 matchmaker/publicRoutes.ts (loadMaps,
// map_id = fnv1a(name.toLowerCase()), creatorname, map_featured), server/packetHandlers.ts
// (cmd_map / listCustomMaps), and the v3.1.1 Games window capture (doc/ref/krunker-v3.1.1/windows/).
// tools/maps/import_v7_maps.py imports the v7 map files (assets/maps/<fileName>.json) and their
// thumbnails.

#include <map_catalog.h>
#include <flavor.h>
#include <shared.h>
#include <strike_maps.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OFFICIAL(key_, name_, idx, rot) \
    {key_, name_, idx, MAP_SOURCE_OFFICIAL, rot, 1, "Krunker", "", "", "", "", "maps/" key_ ".json"}
#define CUSTOM(key_, name_, creator_, mod) \
    {key_, name_, -1, MAP_SOURCE_CUSTOM, 0, 0, creator_, mod, "", "", "", "maps/custom/" key_ ".json"}
// a v3.1.1 official map, kept playable as a community map (v3.1.1 Host Game card labels kept:
// "by", year, version). KrunkNative only: the display name gains " (3.1.1)" (the file keeps
// its own "name"; map_catalog_json sets the display name on the loaded JSON)
#define V311(key_, name_, by, year, version) \
    {"311_" key_, name_ " (3.1.1)", -1, MAP_SOURCE_CUSTOM, 0, 0, "Krunker", "", by, year, version, "maps/311_" key_ ".json"}
// a v3.1.1 custom_maps/old_<key>.json map (the v1.5.2 era official maps): "152_<key>", " (1.5.2)"
#define V152(key_, name_) \
    {"152_" key_, name_ " (1.5.2)", -1, MAP_SOURCE_CUSTOM, 0, 0, "Community", "", "", "", "", "maps/152_" key_ ".json"}

static const MapCatalogEntry k_catalog[] = {
    // v7.1.4 data/maps.js, in mapIndex order; rotation = map.js rotationMaps
    // [0,1,2,3,4,6,7,8,9,10,11,12,13,14,15,16,19,21,22,23,24,25,26,28]
    OFFICIAL("burg_new", "Burg", 0, 1),
    OFFICIAL("littletown", "Littletown", 1, 1),
    OFFICIAL("sandstorm_v3", "Sandstorm", 2, 1),
    OFFICIAL("subzero_new", "Subzero", 3, 1),
    OFFICIAL("undergrowth", "Undergrowth", 4, 1),
    OFFICIAL("shipment", "Shipment", 5, 0),
    OFFICIAL("freight", "Freight", 6, 1),
    OFFICIAL("lostworld_s6", "Lostworld", 7, 1),
    OFFICIAL("citadel", "Citadel", 8, 1),
    OFFICIAL("oasis_v3", "Oasis", 9, 1),
    OFFICIAL("kanji", "Kanji", 10, 1),
    OFFICIAL("industry", "Industry", 11, 1),
    OFFICIAL("lumber", "Lumber", 12, 1),
    OFFICIAL("evacuation", "Evacuation", 13, 1),
    OFFICIAL("site", "Site", 14, 1),
    OFFICIAL("skytemple_v2", "SkyTemple", 15, 1),
    OFFICIAL("lagoon", "Lagoon", 16, 1),
    OFFICIAL("bureau", "Bureau", 17, 0),
    OFFICIAL("shipwrecked", "Tortuga", 18, 0),
    OFFICIAL("tropicano", "Tropicano", 19, 1),
    OFFICIAL("krunk_plaza_v2", "Krunk_Plaza", 20, 0),
    OFFICIAL("arena", "Arena", 21, 1),
    OFFICIAL("habitat_v2", "Habitat", 22, 1),
    OFFICIAL("atomic", "Atomic", 23, 1),
    OFFICIAL("burg_2018", "Old_Burg", 24, 1),
    OFFICIAL("throwback_2018", "Throwback", 25, 1),
    OFFICIAL("stockade_2018", "Stockade", 26, 1),
    OFFICIAL("facility", "Facility", 27, 0),
    OFFICIAL("clockwork", "Clockwork", 28, 1),
    OFFICIAL("laboratory", "Laboratory", 29, 0),
    OFFICIAL("shipyard", "Shipyard", 30, 0),
    OFFICIAL("sanctum", "Soul Sanctum", 31, 0),
    // assets/maps/custom/ (ASCII filename order); creator = body.creator, mod = body.modURL
    CUSTOM("Bhop_VIP", "Bhop_VIP", "eskrr", ""),
    CUSTOM("ClashRoyale", "ClashRoyale", "Community", ""),
    CUSTOM("ESP_Parkour", "ESP_Parkour", "ImCriistian", ""),
    CUSTOM("FortniteTiltedT", "FortniteTiltedT", "Community", ""),
    CUSTOM("Krunk_Plaza", "Krunk_Plaza", "Community", ""),
    CUSTOM("Slide_Neon_Stage", "Slide_Neon_Stage", "R75", ""),
    CUSTOM("SniperRoyale2", "SniperRoyale2", "Community", "https://www.dropbox.com/s/0h3mnvii1nua0h9/Logo.zip?dl=0"),
    CUSTOM("Test", "Test", "Community", ""),
    CUSTOM("Verb_Arena", "Verb_Arena", "Community", "https://www.dropbox.com/s/ejrn99ayvddiq45/Verb%20Mod%20Updated%207April2021.zip?dl=0"),
    // v10's FTUE (first-time user experience) bot tutorial map (game.deob.js config.maps bot_ftue_v2,
    // "Mini Industry"); fetched from krunker.io. Keyed in ASCII order: "Verb_Arena" < "bot_ftue_v2" < "eterno_jump".
    CUSTOM("bot_ftue_v2", "Mini Industry", "Community", ""),
    CUSTOM("eterno_jump", "eterno_jump", "Community", ""),
    CUSTOM("slide_moonlight", "slide_moonlight", "Community", ""),
    CUSTOM("zombie_high", "Zombie_High", "IamFiercer", "https://user-assets.krunker.io/md23677/mod.zip"),
    // the v3.1.1 official maps (v3.1.1 MAPS order) as "311_<key>"; Host Game card labels as v3.1.1 showed them
    V311("burg", "Burg", "", "2018", "v3.2"),
    V311("littletown", "Littletown", "", "2018", "v0.9"),
    V311("sandstorm", "Sandstorm", "CrispyCrust", "2019", "v2.3"),
    V311("subzero", "Subzero", "CrispyCrust", "2019", "v0.9"),
    V311("undergrowth", "Undergrowth", "CrispyCrust", "2019", "v1.2"),
    V311("shipyard", "Shipyard", "Chwoubaka", "2019", "v1.0"),
    V311("freight", "Freight", "", "2019", "v2.1"),
    V311("lostworld", "Lostworld", "DasIstWalter", "2020", "v1.2"),
    V311("citadel", "Citadel", "", "2020", "v2.4"),
    V311("oasis", "Oasis", "DasIstWalter", "2020", "v0.2"),
    V311("kanji", "Kanji", "Arbiter", "2020", "v5.2"),
    V311("newtown", "Newtown", "Arbiter", "2020", "v0.1"),
    V311("industry", "Industry", "CrispyCrust", "2020", "v1.0"),
    V311("soul_sanctum", "Soul Sanctum", "", "2020", ""),
    V311("evacuation", "Evacuation", "Creationfier", "2020", "vTest"),
    // v3.1.1's custom_maps/old_*.json (its ASCII filename order) as "152_<key>"
    V152("burg", "Burg"),
    V152("freight", "Freight"),
    V152("kanji", "Kanji"),
    V152("littletown", "Littletown"),
    V152("lostworld", "Lostworld"),
    V152("sandstorm", "Sandstorm"),
    V152("shipyard", "Shipyard"),
    V152("subzero", "Subzero"),
    V152("undergrowth", "Undergrowth"),
};

#define CATALOG_COUNT (sizeof(k_catalog) / sizeof(k_catalog[0]))

static cJSON *g_catalog_json[CATALOG_COUNT];
static unsigned char g_catalog_failed[CATALOG_COUNT];

// The Games window orders. v3.1.1's were recorded from its offline server (win-games.html
// capture: Popular by file mtime, New by birthtime); v7.1.4 has no recorded order, so the v7
// official maps lead (data/maps.js order) and the community maps keep the v3.1.1 order, with
// the v3.1.1 official maps under their "311_" keys and the old_* maps as "152_".
static const char *const k_games_popular[] = {
    "311_soul_sanctum", "311_subzero", "311_undergrowth", "311_lostworld", "311_newtown", "311_oasis", "311_sandstorm",
    "311_shipyard", "311_freight", "311_industry", "311_kanji", "311_littletown", "311_burg", "311_citadel", "311_evacuation",
    "zombie_high", "152_sandstorm", "152_shipyard", "152_subzero", "152_undergrowth", "slide_moonlight", "152_burg",
    "152_freight", "152_kanji", "152_littletown", "152_lostworld", "SniperRoyale2", "Test", "Verb_Arena", "eterno_jump",
    "Krunk_Plaza", "Slide_Neon_Stage", "Bhop_VIP", "ClashRoyale", "ESP_Parkour", "FortniteTiltedT", "bot_ftue_v2",
};
static const char *const k_games_new[] = {
    "311_subzero", "311_undergrowth", "311_newtown", "311_oasis", "311_sandstorm", "311_shipyard", "311_soul_sanctum",
    "311_industry", "311_kanji", "311_littletown", "311_lostworld", "311_citadel", "311_evacuation", "311_freight", "311_burg",
    "152_shipyard", "152_subzero", "152_undergrowth", "slide_moonlight", "zombie_high", "152_freight", "152_kanji",
    "152_littletown", "152_lostworld", "152_sandstorm", "Test", "Verb_Arena", "eterno_jump", "152_burg",
    "Slide_Neon_Stage", "SniperRoyale2", "ClashRoyale", "ESP_Parkour", "FortniteTiltedT", "Krunk_Plaza", "Bhop_VIP", "bot_ftue_v2",
};

// Old names that still resolve (KrunkNative only): the catalogue keys before the v7 maps took
// the plain names ("v3_<key>" -> "311_<key>", "old_<key>" -> "152_<key>"), right after an exact
// key, and "newtown", the one v3.1.1 file stem no v7 map is named after (`stem`: only when
// nothing else matches). The other v3.1.1 stems ("burg", "sandstorm", "soul_sanctum") name a v7
// map ("Burg" is v7's Burg): they resolve to it by name (map_catalog_index_of_file).
static const struct {
    const char *old, *key;
    unsigned char stem;
} k_aliases[] = {
    {"v3_burg", "311_burg", 0}, {"v3_littletown", "311_littletown", 0}, {"v3_sandstorm", "311_sandstorm", 0},
    {"v3_subzero", "311_subzero", 0}, {"v3_undergrowth", "311_undergrowth", 0}, {"v3_shipyard", "311_shipyard", 0},
    {"v3_freight", "311_freight", 0}, {"v3_lostworld", "311_lostworld", 0}, {"v3_citadel", "311_citadel", 0},
    {"v3_oasis", "311_oasis", 0}, {"v3_kanji", "311_kanji", 0}, {"v3_newtown", "311_newtown", 0},
    {"v3_industry", "311_industry", 0}, {"v3_soul_sanctum", "311_soul_sanctum", 0}, {"v3_evacuation", "311_evacuation", 0},
    {"old_burg", "152_burg", 0}, {"old_freight", "152_freight", 0}, {"old_kanji", "152_kanji", 0},
    {"old_littletown", "152_littletown", 0}, {"old_lostworld", "152_lostworld", 0}, {"old_sandstorm", "152_sandstorm", 0},
    {"old_shipyard", "152_shipyard", 0}, {"old_subzero", "152_subzero", 0}, {"old_undergrowth", "152_undergrowth", 0},
    {"newtown", "311_newtown", 1},
};

#define ALIAS_COUNT (sizeof(k_aliases) / sizeof(k_aliases[0]))

static char *catalog_path(size_t index);
static void resolve_normalize(const char *in, char *out, size_t size);

static int strcasecmp_ascii(const char *a, const char *b) {
    for (; *a && tolower((unsigned char) *a) == tolower((unsigned char) *b); a++, b++) {}
    return tolower((unsigned char) *a) - tolower((unsigned char) *b);
}

size_t map_catalog_count(void) { return CATALOG_COUNT; }

const MapCatalogEntry *map_catalog_at(const size_t index) {
    return index < CATALOG_COUNT ? &k_catalog[index] : NULL;
}

int map_catalog_index_of_key(const char *key) {
    if (!key) return -1;
    for (size_t i = 0; i < CATALOG_COUNT; i++) {
        if (!strcmp(k_catalog[i].key, key)) return (int) i;
    }
    return -1;
}

// alias lookup, case-insensitive; stems: 0 old keys only, 1 also the plain stems
static int alias_index(const char *name, const int stems) {
    if (!name) return -1;
    for (size_t i = 0; i < ALIAS_COUNT; i++) {
        if ((stems || !k_aliases[i].stem) && !strcasecmp_ascii(k_aliases[i].old, name)) return map_catalog_index_of_key(k_aliases[i].key);
    }
    return -1;
}

int map_catalog_index_of_old_key(const char *key) { return alias_index(key, 0); }

int map_catalog_index_of_alias(const char *name) {
    const int exact = map_catalog_index_of_key(name);
    return exact >= 0 ? exact : alias_index(name, 1);
}

int map_catalog_index_of_file(const char *path) {
    if (!path) return -1;
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    char stem[64];
    size_t n = strlen(base);
    if (n > 5 && !strcmp(base + n - 5, ".json")) n -= 5;
    if (!n || n >= sizeof(stem)) return -1;
    memcpy(stem, base, n);
    stem[n] = 0;
    const int alias = map_catalog_index_of_alias(stem);
    if (alias >= 0) return alias;
    // a v3.1.1 stem: the official map of that name ("burg", "soul_sanctum" -> Burg, Soul Sanctum)
    char wanted[64], norm[64];
    resolve_normalize(stem, wanted, sizeof(wanted));
    for (size_t i = 0; wanted[0] && i < MAP_CATALOG_OFFICIAL_COUNT; i++) {
        resolve_normalize(k_catalog[i].name, norm, sizeof(norm));
        if (!strcmp(norm, wanted)) return (int) i;
    }
    return -1;
}

// load_json_file over read_file (the brain links this file with read_file only)
static int load_json_path(const char *path, cJSON **out) {
    unsigned char *buffer = NULL;
    size_t length = 0;
    *out = NULL;
    if (read_file(path, &buffer, &length)) return -1;
    *out = cJSON_ParseWithLength((const char *) buffer, length);
    free(buffer);
    return *out ? 0 : -1;
}

int map_catalog_load_file(const char *path, cJSON **out) {
    if (!path || !out) return -1;
    if (!load_json_path(path, out)) return 0;
    const int index = map_catalog_index_of_file(path);
    if (index < 0) return -1;
    char *moved = catalog_path((size_t) index);
    const int failed = !moved || load_json_path(moved, out);
    if (!failed) fprintf(stderr, "map %s: not found, loading the catalogue's %s (%s)\n", path, k_catalog[index].key, moved);
    free(moved);
    return failed ? -1 : 0;
}

static void lower_copy(const char *in, char *out, const size_t size) {
    size_t n = 0;
    for (; in[n] && n + 1 < size; n++) out[n] = (char) tolower((unsigned char) in[n]);
    out[n] = 0;
}

int map_catalog_find(const char *query) {
    if (!query) return -1;
    while (*query == ' ') query++;
    if (!*query) return -1;

    char *end = NULL;
    const long number = strtol(query, &end, 10);
    if (end && end != query && !*end) return number >= 0 && (size_t) number < CATALOG_COUNT ? (int) number : -1;

    const int exact = map_catalog_index_of_key(query);
    if (exact >= 0) return exact;
    const int renamed = alias_index(query, 0); // "v3_burg", "old_burg": their keys before the rename
    if (renamed >= 0) return renamed;

    char arg[64], lower[64];
    lower_copy(query, arg, sizeof(arg));
    const size_t arg_len = strlen(arg);

    // MAPS keys: parts[0] === arg (split on / [ _]/) || name.startsWith(arg)
    for (size_t i = 0; i < CATALOG_COUNT; i++) {
        if (k_catalog[i].source != MAP_SOURCE_OFFICIAL) continue;
        lower_copy(k_catalog[i].name, lower, sizeof(lower));
        const size_t first = strcspn(lower, " _");
        if ((first == arg_len && !strncmp(lower, arg, first)) || !strncmp(lower, arg, arg_len)) return (int) i;
    }
    // listCustomMaps: n.toLowerCase() === arg, then startsWith(arg)
    for (int pass = 0; pass < 2; pass++) {
        for (size_t i = 0; i < CATALOG_COUNT; i++) {
            if (k_catalog[i].source != MAP_SOURCE_CUSTOM) continue;
            lower_copy(k_catalog[i].key, lower, sizeof(lower));
            if (pass == 0 ? !strcmp(lower, arg) : !strncmp(lower, arg, arg_len)) return (int) i;
        }
    }
    return alias_index(query, 1); // "newtown", the v3.1.1 stem no v7 map is named after
}

// lowercase letters and digits only: "Soul Sanctum" == "soul_sanctum" == "\"soul sanctum\""
static void resolve_normalize(const char *in, char *out, const size_t size) {
    size_t n = 0;
    for (; *in && n + 1 < size; in++) {
        const unsigned char c = (unsigned char) *in;
        if (isalnum(c)) out[n++] = (char) tolower(c);
    }
    out[n] = 0;
}

int map_catalog_resolve(const char *query, int *candidates, const size_t max, size_t *candidate_count) {
    if (candidate_count) *candidate_count = 0;
    char wanted[64];
    resolve_normalize(query ? query : "", wanted, sizeof(wanted));
    if (!wanted[0]) return -1;
    const size_t wanted_len = strlen(wanted);

    // 0: key (or an old key: "old burg" is 152_burg, not v7's Old_Burg), 1: official name,
    // 2: community name ("Burg (3.1.1)")
    char norm[64];
    for (int pass = 0; pass < 3; pass++) {
        int found = -1, found_n = 0;
        for (size_t i = 0; i < CATALOG_COUNT; i++) {
            const MapCatalogEntry *e = &k_catalog[i];
            if (pass == 1 && e->source != MAP_SOURCE_OFFICIAL) continue;
            if (pass == 2 && e->source != MAP_SOURCE_CUSTOM) continue;
            resolve_normalize(pass == 0 ? e->key : e->name, norm, sizeof(norm));
            int hit = !strcmp(norm, wanted);
            for (size_t a = 0; pass == 0 && !hit && a < ALIAS_COUNT; a++) {
                if (k_aliases[a].stem || strcmp(k_aliases[a].key, e->key)) continue;
                resolve_normalize(k_aliases[a].old, norm, sizeof(norm));
                hit = !strcmp(norm, wanted);
            }
            if (hit) {
                if (candidates && (size_t) found_n < max) candidates[found_n] = (int) i;
                found = (int) i;
                found_n++;
            }
        }
        if (found_n == 1) return found;
        if (found_n > 1) {
            if (candidate_count) *candidate_count = (size_t) found_n;
            return -2;
        }
    }

    // a unique prefix of the /maps label (official name, community key)
    int found = -1;
    size_t found_n = 0;
    for (size_t i = 0; i < CATALOG_COUNT; i++) {
        const MapCatalogEntry *e = &k_catalog[i];
        resolve_normalize(e->source == MAP_SOURCE_OFFICIAL ? e->name : e->key, norm, sizeof(norm));
        if (strncmp(norm, wanted, wanted_len)) continue;
        if (candidates && found_n < max) candidates[found_n] = (int) i;
        found = (int) i;
        found_n++;
    }
    if (found_n == 1) return found;
    if (found_n > 1) {
        if (candidate_count) *candidate_count = found_n;
        return -2;
    }
    // "newtown", the v3.1.1 stem no v7 map is named after
    for (size_t a = 0; a < ALIAS_COUNT; a++) {
        resolve_normalize(k_aliases[a].old, norm, sizeof(norm));
        if (k_aliases[a].stem && !strcmp(norm, wanted)) return map_catalog_index_of_key(k_aliases[a].key);
    }
    return -1;
}

uint32_t map_catalog_map_id(const MapCatalogEntry *entry) {
    uint32_t h = 0x811c9dc5u;
    if (!entry) return h;
    for (const unsigned char *p = (const unsigned char *) entry->name; *p; p++) {
        h ^= (uint32_t) tolower(*p);
        h += (h << 1) + (h << 4) + (h << 7) + (h << 8) + (h << 24);
    }
    return h;
}

const char *map_catalog_thumb(const MapCatalogEntry *entry, char *out, const size_t size) {
    if (!entry || entry->source != MAP_SOURCE_OFFICIAL || !out || !size) return NULL;
    snprintf(out, size, "img/maps/map_%d.png", entry->index);
    return out;
}

// ------------------------------------------------------------------ JSON

static int array_of_three(const cJSON *item) {
    return cJSON_IsArray(item) && cJSON_GetArraySize(item) >= 3;
}

// s = [xyz[si*3], xyz[si*3+1], xyz[si*3+2]] when the pool has three numbers there
static cJSON *size_from_pool(const cJSON *object, const cJSON *xyz) {
    const cJSON *si = cJSON_GetObjectItemCaseSensitive(object, "si");
    if (!cJSON_IsNumber(si) || !cJSON_IsArray(xyz)) return NULL;
    const int base = (int) cJSON_GetNumberValue(si) * 3;
    if (base < 0 || base + 2 >= cJSON_GetArraySize(xyz)) return NULL;
    double v[3];
    for (int i = 0; i < 3; i++) {
        const cJSON *n = cJSON_GetArrayItem(xyz, base + i);
        if (!cJSON_IsNumber(n)) return NULL;
        v[i] = cJSON_GetNumberValue(n);
    }
    return cJSON_CreateDoubleArray(v, 3);
}

int map_sanitize_json(cJSON *map) {
    cJSON *objects = map ? cJSON_GetObjectItemCaseSensitive(map, "objects") : NULL;
    if (!cJSON_IsArray(objects)) return 0;
    const cJSON *xyz = cJSON_GetObjectItemCaseSensitive(map, "xyz");

    int changed = 0;
    for (cJSON *object = objects->child, *next; object; object = next) {
        next = object->next;
        int keep = cJSON_IsObject(object) && array_of_three(cJSON_GetObjectItemCaseSensitive(object, "p"));
        if (keep && !array_of_three(cJSON_GetObjectItemCaseSensitive(object, "s"))) {
            cJSON *size = size_from_pool(object, xyz);
            if (size) {
                cJSON_DeleteItemFromObjectCaseSensitive(object, "s");
                cJSON_AddItemToObject(object, "s", size);
            } else {
                keep = 0; // "dropping object with missing size"
            }
            changed++;
        }
        if (!keep) {
            if (cJSON_IsObject(object) && array_of_three(cJSON_GetObjectItemCaseSensitive(object, "p"))) changed--; // counted above
            cJSON_Delete(cJSON_DetachItemViaPointer(objects, object));
            changed++;
        }
    }
    return changed;
}

static char *catalog_path(const size_t index) {
    // Krunker Strike plays FRVR's own copies of its pool maps (strike_maps.h); base is unchanged
    const StrikeMap *strike = flavor_get() == FLAVOR_STRIKE ? strike_map_for_catalog(index) : NULL;
    return concat(assets_path(), strike ? strike->path : k_catalog[index].path);
}

const cJSON *map_catalog_json(const size_t index) {
    if (index >= CATALOG_COUNT) return NULL;
    if (g_catalog_json[index] || g_catalog_failed[index]) return g_catalog_json[index];

    char *path = catalog_path(index);
    unsigned char *buffer = NULL;
    size_t length = 0;
    if (!path || read_file(path, &buffer, &length)) {
        fprintf(stderr, "map catalogue: cannot read %s\n", path ? path : k_catalog[index].key);
        g_catalog_failed[index] = 1;
        free(path);
        return NULL;
    }
    cJSON *json = cJSON_ParseWithLength((const char *) buffer, length);
    free(buffer);
    if (!cJSON_IsObject(json)) {
        fprintf(stderr, "map catalogue: %s is not a map JSON\n", path);
        cJSON_Delete(json);
        g_catalog_failed[index] = 1;
        free(path);
        return NULL;
    }
    free(path);
    map_sanitize_json(json);
    // the 311_ / 152_ maps keep their file's "name" ("Burg"): the game, the vote cards and
    // the server list show the catalogue's display name ("Burg (3.1.1)") instead
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(json, "name");
    if (!cJSON_IsString(name)) cJSON_AddStringToObject(json, "name", k_catalog[index].name);
    else if (strcmp(name->valuestring, k_catalog[index].name)) cJSON_ReplaceItemInObjectCaseSensitive(json, "name", cJSON_CreateString(k_catalog[index].name));
    g_catalog_json[index] = json;
    return json;
}

static void catalog_prefetch(const int *indices, const size_t count) {
    char **paths = calloc(count ? count : 1, sizeof(char *));
    if (!paths) return;
    size_t n = 0;
    for (size_t i = 0; i < count; i++) {
        const int index = indices[i];
        if (index < 0 || (size_t) index >= CATALOG_COUNT || g_catalog_json[index] || g_catalog_failed[index]) continue;
        paths[n] = catalog_path((size_t) index);
        if (paths[n]) n++;
    }
    asset_prefetch_wait((const char *const *) paths, n);
    for (size_t i = 0; i < n; i++) free(paths[i]);
    free(paths);
}

size_t map_catalog_load_official(void) {
    int indices[MAP_CATALOG_OFFICIAL_COUNT];
    for (int i = 0; i < MAP_CATALOG_OFFICIAL_COUNT; i++) indices[i] = i;
    catalog_prefetch(indices, MAP_CATALOG_OFFICIAL_COUNT);

    size_t loaded = 0;
    for (size_t i = 0; i < MAP_CATALOG_OFFICIAL_COUNT; i++) loaded += map_catalog_json(i) != NULL;
    return loaded;
}

int map_catalog_index_of_json(const cJSON *json) {
    if (!json) return -1;
    for (size_t i = 0; i < CATALOG_COUNT; i++) {
        if (g_catalog_json[i] == json) return (int) i;
    }
    return -1;
}

size_t map_catalog_rotation(int *out, const size_t max) {
    size_t n = 0;
    if (flavor_get() == FLAVOR_STRIKE) { // Krunker Strike rotates its own pool (strike_maps.h)
        const StrikeMap *pool = NULL;
        const size_t count = strike_map_pool(&pool);
        for (; n < count; n++) {
            if (out && n < max) out[n] = pool[n].catalog_index;
        }
        return out && n > max ? max : n;
    }
    for (size_t i = 0; i < CATALOG_COUNT; i++) {
        if (!k_catalog[i].rotation) continue;
        if (out && n < max) out[n] = (int) i;
        n++;
    }
    return out && n > max ? max : n;
}

const cJSON **map_catalog_list(const int *indices, const size_t count, size_t *out_count) {
    if (out_count) *out_count = 0;
    if (!indices || !count) return NULL;
    catalog_prefetch(indices, count);

    const cJSON **list = calloc(count, sizeof(cJSON *));
    if (!list) return NULL;
    size_t n = 0;
    for (size_t i = 0; i < count; i++) {
        if (indices[i] < 0) continue;
        const cJSON *json = map_catalog_json((size_t) indices[i]);
        if (json) list[n++] = json;
    }
    if (out_count) *out_count = n;
    return list;
}

int map_catalog_slot(const cJSON ***maps, size_t *map_count, const size_t index) {
    if (!maps || !map_count) return -1;
    const cJSON *json = map_catalog_json(index);
    if (!json) return -1;
    for (size_t i = 0; i < *map_count; i++) {
        if ((*maps)[i] == json) return (int) i;
    }
    const cJSON **grown = realloc((void *) *maps, (*map_count + 1) * sizeof(cJSON *));
    if (!grown) return -1;
    grown[*map_count] = json;
    *maps = grown;
    return (int) (*map_count)++;
}

static size_t games_from_keys(const char *const *keys, const size_t count, int *out, const size_t max) {
    size_t n = 0;
    for (size_t i = 0; i < count && n < max; i++) {
        const int index = map_catalog_index_of_key(keys[i]);
        if (index >= 0) out[n++] = index;
    }
    return n;
}

static int key_ascii_cmp(const void *a, const void *b) {
    const MapCatalogEntry *ea = &k_catalog[*(const int *) a];
    const MapCatalogEntry *eb = &k_catalog[*(const int *) b];
    if (ea->source != eb->source) return ea->source < eb->source ? -1 : 1;
    return strcmp(ea->key, eb->key);
}

size_t map_catalog_games_tab(const int tab, int *out, const size_t max) {
    if (!out) return 0;
    size_t n = 0;
    switch (tab) {
        case 1: // Featured: the official maps (data/maps.js order)
            for (size_t i = 0; i < MAP_CATALOG_OFFICIAL_COUNT && n < max; i++) out[n++] = (int) i;
            return n;
        case 0: case 2: // Popular / New: the official maps, then the community maps' recorded order
            for (size_t i = 0; i < MAP_CATALOG_OFFICIAL_COUNT && n < max; i++) out[n++] = (int) i;
            if (tab == 2) return n + games_from_keys(k_games_new, sizeof(k_games_new) / sizeof(k_games_new[0]), out + n, max - n);
            return n + games_from_keys(k_games_popular, sizeof(k_games_popular) / sizeof(k_games_popular[0]), out + n, max - n);
        case 3: return 0; // Favorites: nothing favourited
        case 4: {
            // My Games: the rows unsorted, i.e. readdir order (maps/ then custom_maps/, ASCII)
            const size_t n = CATALOG_COUNT < max ? CATALOG_COUNT : max;
            int all[CATALOG_COUNT];
            for (size_t i = 0; i < CATALOG_COUNT; i++) all[i] = (int) i;
            qsort(all, CATALOG_COUNT, sizeof(all[0]), key_ascii_cmp);
            memcpy(out, all, n * sizeof(int));
            return n;
        }
        default: return 0;
    }
}
