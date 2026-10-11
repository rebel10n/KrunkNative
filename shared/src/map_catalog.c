// The Krunker v3.1.1 map catalogue (shared/include/map_catalog.h).
//
// Sources: Krunker_v3.1.1/main/maps.js + server/game.ts (MAPS: name -> mapIndex),
// src/data/map.js (the bundled `maps` array and rotationMaps), matchmaker/publicRoutes.ts
// (loadMaps: maps/ then custom_maps/, map_id = fnv1a(name.toLowerCase()), creatorname,
// map_featured), server/packetHandlers.ts (cmd_map / listCustomMaps), and the Host Game /
// Games windows captured from the running game (doc/ref/krunker-v3.1.1/windows/).

#include <map_catalog.h>
#include <shared.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OFFICIAL(key_, name_, idx, rot, host, by, year, version) \
    {key_, name_, idx, MAP_SOURCE_OFFICIAL, rot, host, "Krunker", "", by, year, version}
#define CUSTOM(key_, name_, creator_, mod) \
    {key_, name_, -1, MAP_SOURCE_CUSTOM, 0, 0, creator_, mod, "", "", ""}

static const MapCatalogEntry k_catalog[] = {
    // maps/ (MAPS order). Host card labels are genHost's (windows[7]) as captured.
    OFFICIAL("burg", "Burg", 0, 1, 1, "", "2018", "v3.2"),
    OFFICIAL("littletown", "Littletown", 1, 1, 1, "", "2018", "v0.9"),
    OFFICIAL("sandstorm", "Sandstorm", 2, 1, 1, "CrispyCrust", "2019", "v2.3"),
    OFFICIAL("subzero", "Subzero", 3, 1, 1, "CrispyCrust", "2019", "v0.9"),
    OFFICIAL("undergrowth", "Undergrowth", 4, 1, 1, "CrispyCrust", "2019", "v1.2"),
    OFFICIAL("shipyard", "Shipyard", 5, 0, 1, "Chwoubaka", "2019", "v1.0"),
    OFFICIAL("freight", "Freight", 6, 1, 1, "", "2019", "v2.1"),
    OFFICIAL("lostworld", "Lostworld", 7, 1, 1, "DasIstWalter", "2020", "v1.2"),
    OFFICIAL("citadel", "Citadel", 8, 1, 1, "", "2020", "v2.4"),
    OFFICIAL("oasis", "Oasis", 9, 1, 1, "DasIstWalter", "2020", "v0.2"),
    OFFICIAL("kanji", "Kanji", 10, 1, 1, "Arbiter", "2020", "v5.2"),
    OFFICIAL("newtown", "Newtown", 11, 1, 1, "Arbiter", "2020", "v0.1"),
    OFFICIAL("industry", "Industry", 12, 1, 1, "CrispyCrust", "2020", "v1.0"),
    // DISABLED_ROTATION_MAPS (game.ts) and not in genHost: Raid's map ("23,13" end votes)
    OFFICIAL("soul_sanctum", "Soul Sanctum", 13, 0, 0, "", "2020", ""),
    OFFICIAL("evacuation", "Evacuation", 14, 1, 1, "Creationfier", "2020", "vTest"),
    // custom_maps/ (ASCII filename order); creator = body.creator, mod = body.modURL
    CUSTOM("Bhop_VIP", "Bhop_VIP", "eskrr", ""),
    CUSTOM("ClashRoyale", "ClashRoyale", "Community", ""),
    CUSTOM("ESP_Parkour", "ESP_Parkour", "ImCriistian", ""),
    CUSTOM("FortniteTiltedT", "FortniteTiltedT", "Community", ""),
    CUSTOM("Krunk_Plaza", "Krunk_Plaza", "Community", ""),
    CUSTOM("Slide_Neon_Stage", "Slide_Neon_Stage", "R75", ""),
    CUSTOM("SniperRoyale2", "SniperRoyale2", "Community", "https://www.dropbox.com/s/0h3mnvii1nua0h9/Logo.zip?dl=0"),
    CUSTOM("Test", "Test", "Community", ""),
    CUSTOM("Verb_Arena", "Verb_Arena", "Community", "https://www.dropbox.com/s/ejrn99ayvddiq45/Verb%20Mod%20Updated%207April2021.zip?dl=0"),
    CUSTOM("eterno_jump", "eterno_jump", "Community", ""),
    CUSTOM("old_burg", "Burg", "Community", ""),
    CUSTOM("old_freight", "Freight", "Community", ""),
    CUSTOM("old_kanji", "Kanji", "Community", ""),
    CUSTOM("old_littletown", "Littletown", "Community", ""),
    CUSTOM("old_lostworld", "Lostworld", "Community", ""),
    CUSTOM("old_sandstorm", "Sandstorm", "Community", ""),
    CUSTOM("old_shipyard", "Shipyard", "Community", ""),
    CUSTOM("old_subzero", "Subzero", "Community", ""),
    CUSTOM("old_undergrowth", "Undergrowth", "Community", ""),
    CUSTOM("slide_moonlight", "slide_moonlight", "Community", ""),
    CUSTOM("zombie_high", "Zombie_High", "IamFiercer", "https://user-assets.krunker.io/md23677/mod.zip"),
};

#define CATALOG_COUNT (sizeof(k_catalog) / sizeof(k_catalog[0]))

static cJSON *g_catalog_json[CATALOG_COUNT];
static unsigned char g_catalog_failed[CATALOG_COUNT];

// The Games window orders the v3.1.1 offline server sent (win-games.html capture). Popular
// sorts by file mtime and New by birthtime, both install-time noise, so they are recorded
// rather than derived; Featured is the official maps in the order the server returned them.
static const char *const k_games_popular[] = {
    "soul_sanctum", "subzero", "undergrowth", "lostworld", "newtown", "oasis", "sandstorm", "shipyard", "freight",
    "industry", "kanji", "littletown", "burg", "citadel", "evacuation", "zombie_high", "old_sandstorm", "old_shipyard",
    "old_subzero", "old_undergrowth", "slide_moonlight", "old_burg", "old_freight", "old_kanji", "old_littletown",
    "old_lostworld", "SniperRoyale2", "Test", "Verb_Arena", "eterno_jump", "Krunk_Plaza", "Slide_Neon_Stage", "Bhop_VIP",
    "ClashRoyale", "ESP_Parkour", "FortniteTiltedT",
};
static const char *const k_games_featured[] = {
    "industry", "subzero", "lostworld", "evacuation", "undergrowth", "kanji", "shipyard", "freight", "sandstorm",
    "oasis", "littletown", "newtown", "soul_sanctum", "citadel", "burg",
};
static const char *const k_games_new[] = {
    "subzero", "undergrowth", "newtown", "oasis", "sandstorm", "shipyard", "soul_sanctum", "industry", "kanji",
    "littletown", "lostworld", "citadel", "evacuation", "freight", "burg", "old_shipyard", "old_subzero",
    "old_undergrowth", "slide_moonlight", "zombie_high", "old_freight", "old_kanji", "old_littletown", "old_lostworld",
    "old_sandstorm", "Test", "Verb_Arena", "eterno_jump", "old_burg", "Slide_Neon_Stage", "SniperRoyale2",
    "ClashRoyale", "ESP_Parkour", "FortniteTiltedT", "Krunk_Plaza", "Bhop_VIP",
};

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
    const MapCatalogEntry *e = &k_catalog[index];
    char relative[128];
    snprintf(relative, sizeof(relative), "maps/%s%s.json", e->source == MAP_SOURCE_CUSTOM ? "custom/" : "", e->key);
    return concat(assets_path(), relative);
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
    switch (tab) {
        case 1: return games_from_keys(k_games_featured, sizeof(k_games_featured) / sizeof(k_games_featured[0]), out, max);
        case 2: return games_from_keys(k_games_new, sizeof(k_games_new) / sizeof(k_games_new[0]), out, max);
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
        default: return games_from_keys(k_games_popular, sizeof(k_games_popular) / sizeof(k_games_popular[0]), out, max);
    }
}
