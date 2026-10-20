// The map catalogue (shared/src/map_catalog.c): Krunker's 46 official maps (the v10.0.2 files; their
// mapIndex is the entry's index: 0..31 at the v7 positions, 32..45 appended at 69..82) and the community maps (v3.1.1 custom_maps, the v3.1.1 official maps as "311_<key>" and its
// old_* maps as "152_<key>"). Every map file loads, its JSON "name" is the catalogue name (the
// 311_ / 152_ files' own name + " (3.1.1)" / " (1.5.2)"), old keys and file stems resolve, map_init() builds it (objects, spawns,
// v7 prefabs, pooled "si" sizes), ids / rotation / Games-window lists match, and cmd_map style
// lookups resolve like packetHandlers.ts. Also plays one game_init() on every map.
// Run from the repository root: ./<build>/map_catalog_test [-v]
#include <shared.h>
#include <map_catalog.h>
#include <krunker_maps.h>
#include <prefabs.h>
#include <ranked.h>
#include <stdio.h>
#include <string.h>

// the v10.0.2 official list (krunker_maps.h): the catalogue's first 32 entries are these files, index for index
#define OFFICIAL_PATH(index_, name_, file_, objects_) file_,
#define OFFICIAL_NAME(index_, name_, file_, objects_) name_,
static const char *const official_path[] = {KRUNKER_OFFICIAL_MAPS(OFFICIAL_PATH)};
static const char *const official_name[] = {KRUNKER_OFFICIAL_MAPS(OFFICIAL_NAME)};

// A thumbnail file that loads: a PNG (signature and IHDR at their places, the way stb_image reads it).
// Paths are relative to the repository root (the test's working directory), the ui/ tree.
static int png_loads(const char *path) {
    static const unsigned char sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    unsigned char head[16];
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    const size_t n = fread(head, 1, sizeof(head), f);
    fclose(f);
    return n == sizeof(head) && !memcmp(head, sig, sizeof(sig)) && !memcmp(head + 12, "IHDR", 4);
}

// Community maps with no picture (ui/README.md "Map thumbnails"): their Krunker map id has no
// user-assets thumb, or none was found (152_* maps). Any other map must have one.
static int thumb_missing_on_purpose(const MapCatalogEntry *e) {
    static const char *const keys[] = {"ESP_Parkour", "FortniteTiltedT", "Test", "bot_ftue_v2", "eterno_jump"};
    // every official map has a picture (the v10.0.2 additions, mapIndex 32..45, have v10's map_<n>.png each)
    if (e->source == MAP_SOURCE_OFFICIAL) return 0;
    if (!strncmp(e->key, "152_", 4)) return 1;
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (!strcmp(e->key, keys[i])) return 1;
    }
    return 0;
}

static int g_failures;
static int g_checks;

#define CHECK(cond, ...)                                  \
    do {                                                  \
        g_checks++;                                       \
        if (!(cond)) {                                    \
            g_failures++;                                 \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);   \
            printf(__VA_ARGS__);                          \
            printf("\n");                                 \
        }                                                 \
    } while (0)

// map_id values the v3.1.1 offline server sent (doc/ref/krunker-v3.1.1/windows/win-games.html);
// fnv1a of the lower-cased name, so the v7 maps of the v3.1.1 official maps' names share them
static const struct {
    const char *key;
    uint32_t id;
} k_ids[] = {
    {"sanctum", 2717856513u}, {"subzero_new", 1389891507u}, {"undergrowth", 2699178862u},
    {"lostworld_s6", 2026576977u}, {"oasis_v3", 2101154094u}, {"sandstorm_v3", 88334292u},
    {"shipyard", 2613134335u}, {"freight", 1010451404u}, {"industry", 312130495u}, {"kanji", 1336255044u},
    {"littletown", 3836435553u}, {"burg_new", 1257833875u}, {"citadel", 1963634499u}, {"evacuation", 4241009034u},
    {"zombie_high", 569198284u}, {"slide_moonlight", 3690042710u},
    {"SniperRoyale2", 2800345118u}, {"Test", 2949673445u}, {"Verb_Arena", 682138864u}, {"eterno_jump", 739315237u},
    {"Krunk_Plaza", 537274925u}, {"Slide_Neon_Stage", 197077862u}, {"Bhop_VIP", 3459552096u},
    {"ClashRoyale", 2789783172u}, {"ESP_Parkour", 2793053018u}, {"FortniteTiltedT", 563473844u},
    {"krunk_plaza_v2", 537274925u},
};

static int legacy_key(const char *key) { return !strncmp(key, "311_", 4) || !strncmp(key, "152_", 4); }

// the "name" in the 311_ / 152_ file itself: the display name without " (3.1.1)" / " (1.5.2)"
static void check_file_name(const MapCatalogEntry *e) {
    char path[256];
    snprintf(path, sizeof(path), "assets/%s", e->path);
    cJSON *raw = NULL;
    CHECK(!load_json_file(path, &raw), "%s: %s does not load", e->key, path);
    const char *name = raw ? cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(raw, "name")) : NULL;
    char want[64];
    snprintf(want, sizeof(want), "%s (%s)", name ? name : "?", !strncmp(e->key, "311_", 4) ? "3.1.1" : "1.5.2");
    CHECK(name && !strcmp(want, e->name), "%s: file name '%s', display '%s'", e->key, name ? name : "?", e->name);
    cJSON_Delete(raw);
}

static int count_prefab(const Map *map, const int prefab) {
    int n = 0;
    for (size_t i = 0; i < map->object_count; i++) n += map->objects[i]->prefab == prefab;
    return n;
}

static void test_entry(const size_t index, const int verbose) {
    const MapCatalogEntry *e = map_catalog_at(index);
    if (e->source == MAP_SOURCE_OFFICIAL) CHECK(map_catalog_official_index(e->index) == (int) index, "%s: mapIndex %d", e->key, e->index);
    const cJSON *json = map_catalog_json(index);
    CHECK(json != NULL, "%s: JSON did not load", e->key);
    if (!json) return;

    const cJSON *name = cJSON_GetObjectItemCaseSensitive(json, "name");
    CHECK(cJSON_IsString(name) && !strcmp(name->valuestring, e->name), "%s: JSON name '%s' != catalogue '%s'", e->key,
          cJSON_IsString(name) ? name->valuestring : "?", e->name);
    const cJSON *creator = cJSON_GetObjectItemCaseSensitive(json, "creator");
    const char *want_creator = cJSON_IsString(creator) && creator->valuestring[0] ? creator->valuestring
                               : e->source == MAP_SOURCE_OFFICIAL || !strncmp(e->key, "311_", 4) ? "Krunker" : "Community";
    CHECK(!strcmp(e->creator, want_creator), "%s: creator %s != body %s", e->key, e->creator, want_creator);
    const cJSON *mod = cJSON_GetObjectItemCaseSensitive(json, "modURL");
    CHECK(!strcmp(e->mod_url, cJSON_IsString(mod) ? mod->valuestring : ""), "%s: modURL mismatch", e->key);

    // every object keeps a position and a size after sanitising (sanitizeCustomMapJson)
    const cJSON *objects = cJSON_GetObjectItemCaseSensitive(json, "objects");
    const cJSON *colors = cJSON_GetObjectItemCaseSensitive(json, "colors");
    const int color_count = cJSON_IsArray(colors) ? cJSON_GetArraySize(colors) : 0;
    int raw_objects = 0, bad_palette = 0;
    const cJSON *o;
    cJSON_ArrayForEach(o, objects) {
        // AI_SPAWNER / BOT_SPAWNER become pending AIs / bots, TERMINAL and EVENT build nothing (v7
        // map.js): never map objects (prefabs.h PREFAB_R_NO_OBJECT)
        if (!prefab_has_role(map_raw_object_prefab(o), PREFAB_R_NO_OBJECT)) raw_objects++;
        CHECK(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(o, "s")) >= 3, "%s: object without s", e->key);
        const char *idx_keys[] = {"ci", "ei"};
        for (int k = 0; k < 2; k++) {
            const cJSON *ci = cJSON_GetObjectItemCaseSensitive(o, idx_keys[k]);
            if (cJSON_IsNumber(ci) && ((int) ci->valuedouble < 0 || (int) ci->valuedouble >= color_count)) bad_palette++;
        }
    }
    CHECK(bad_palette == 0, "%s: %d ci/ei palette indices out of range (%d colors)", e->key, bad_palette, color_count);

    Map *map = map_init(json);
    CHECK(map != NULL, "%s: map_init failed", e->key);
    if (!map) return;
    CHECK(map->spawn_count > 0, "%s: no spawns", e->key);
    CHECK((int) map->object_count == raw_objects, "%s: %zu of %d objects built", e->key, map->object_count, raw_objects);
    int out_of_range = 0;
    // v7.1.4: ids 0-96 keep their prefab (no id >= 60 falls back to CUBE any more)
    for (size_t i = 0; i < map->object_count; i++) out_of_range += map->objects[i]->prefab >= PREFAB_COUNT;
    int remapped = 0;
    cJSON_ArrayForEach(o, objects) {
        const cJSON *prefab = cJSON_GetObjectItemCaseSensitive(o, "i");
        if (cJSON_IsNumber(prefab) && prefab->valuedouble >= 0 && prefab->valuedouble < PREFAB_COUNT &&
            map_raw_object_prefab(o) != (int) prefab->valuedouble) remapped++;
    }
    CHECK(remapped == 0, "%s: %d objects lose their prefab id", e->key, remapped);
    CHECK(out_of_range == 0, "%s: %d objects keep an unknown prefab id", e->key, out_of_range);
    if (verbose) {
        printf("  %-2zu %-16s %-16s %-9s objects %4zu spawns %2zu cubes %4d dims x[%.0f,%.0f] y[%.0f,%.0f] z[%.0f,%.0f]\n", index,
               e->key, e->name, e->source == MAP_SOURCE_OFFICIAL ? "official" : "custom", map->object_count, map->spawn_count,
               count_prefab(map, PREFAB_CUBE), map->dimensions.min.x, map->dimensions.max.x, map->dimensions.min.y,
               map->dimensions.max.y, map->dimensions.min.z, map->dimensions.max.z);
    }
    map_fini(map);
    free(map);
}

// One round of the shared game on every catalogue map (the list map_catalog_list builds).
static void test_game_init_all(void) {
    const size_t count = map_catalog_count();
    int all[256]; // 83 maps: the v7 catalogue, the 14 v10.0.2 additions (was int[64]: a stack overflow past 64)
    CHECK(count <= sizeof(all) / sizeof(all[0]), "catalogue fits the test's index array: %zu", count);
    if (count > sizeof(all) / sizeof(all[0])) return;
    for (size_t i = 0; i < count; i++) all[i] = (int) i;
    size_t n = 0;
    const cJSON **maps = map_catalog_list(all, count, &n);
    CHECK(maps && n == count, "map_catalog_list: %zu of %zu maps", n, count);
    if (!maps) return;
    static Game game;
    game_configure(&game, NULL, maps, n, NULL, 0, NULL, 0, NULL, 0);
    for (size_t i = 0; i < n; i++) {
        game_init(&game, (int) i, 0, 0);
        CHECK(game.ready && game.map && game.current_map_index == (int) i, "game_init on %s failed", map_catalog_at(i)->key);
        CHECK(game.map && game.map->spawn_count > 0, "game_init on %s: no spawns", map_catalog_at(i)->key);
    }
}

int main(const int argc, char **argv) {
    const int verbose = argc > 1 && !strcmp(argv[1], "-v");
    const size_t count = map_catalog_count();
    CHECK(MAP_CATALOG_OFFICIAL_COUNT == 46 && count == 46 + 13 + 15 + 9,
          "catalogue has %zu maps: 46 official + 13 community + 15 311_ + 9 152_", count);

    // official maps: one entry per mapIndex 0..45 (the v10.0.2 list), at the v7 positions 0..31 and
    // appended at 69..82; keys unique; community maps in readdir order, then the v3.1.1 official maps
    // (311_), then its old_* maps (152_): the community block is 32..68
    int official_seen[MAP_CATALOG_OFFICIAL_COUNT] = {0};
    for (size_t i = 0; i < count; i++) {
        const MapCatalogEntry *e = map_catalog_at(i);
        const int official_pos = i < 32 || (i >= 69 && i < 83);
        CHECK(official_pos == (e->source == MAP_SOURCE_OFFICIAL), "%s: position %zu of the wrong source", e->key, i);
        if (e->source == MAP_SOURCE_OFFICIAL) {
            CHECK(e->index >= 0 && e->index < MAP_CATALOG_OFFICIAL_COUNT && e->hostable, "%s mapIndex %d", e->key, e->index);
            if (e->index >= 0 && e->index < MAP_CATALOG_OFFICIAL_COUNT) official_seen[e->index]++;
            CHECK(e->index >= 0 && e->index < MAP_CATALOG_OFFICIAL_COUNT && e->path && !strcmp(e->path, official_path[e->index]) &&
                  !strcmp(e->name, official_name[e->index]),
                  "%s: %s (%s) is not the v10 mapIndex %d file", e->key, e->path ? e->path : "?", e->name, e->index);
            CHECK(!e->rotation || e->index < MAP_CATALOG_OFFICIAL_COUNT, "%s: rotation outside the official set", e->key);
        } else {
            CHECK(e->index == -1 && !e->rotation && !e->hostable, "%s custom flags", e->key);
        }
        CHECK(map_catalog_index_of_key(e->key) == (int) i, "%s key not unique", e->key);
        const int legacy = legacy_key(e->key);
        if (i > 32 && e->source == MAP_SOURCE_CUSTOM && !legacy && map_catalog_at(i - 1)->source == MAP_SOURCE_CUSTOM && !legacy_key(map_catalog_at(i - 1)->key))
            CHECK(strcmp(map_catalog_at(i - 1)->key, e->key) < 0, "%s not in readdir order", e->key);
        if (e->source == MAP_SOURCE_CUSTOM && i > 32 && map_catalog_at(i - 1)->source == MAP_SOURCE_CUSTOM && legacy_key(map_catalog_at(i - 1)->key))
            CHECK(legacy, "%s after the 311_ / 152_ maps", e->key);
        // the official maps are the v10.0.2 files (krunker_maps.h); the file stem is the key for the
        // 311_ / 152_ maps (maps/<key>.json) and the community maps (maps/custom/<key>.json)
        if (e->source == MAP_SOURCE_OFFICIAL) {
            // checked above (path and name by mapIndex)
        } else {
            char want[96];
            snprintf(want, sizeof(want), legacy ? "maps/%s.json" : "maps/custom/%s.json", e->key);
            CHECK(e->path && !strcmp(e->path, want), "%s path %s", e->key, e->path ? e->path : "?");
        }
        CHECK(map_catalog_index_of_file(e->path) == (int) i, "%s: index_of_file(%s)", e->key, e->path);
        if (legacy) check_file_name(e);
    }
    CHECK(!strcmp(map_catalog_at(0)->key, "burg_new") && !strcmp(map_catalog_at(10)->name, "Kanji") &&
          !strcmp(map_catalog_at(18)->name, "Tortuga") && !strcmp(map_catalog_at(31)->name, "Soul Sanctum"), "v7 data/maps.js order");
    for (int i = 0; i < MAP_CATALOG_OFFICIAL_COUNT; i++) CHECK(official_seen[i] == 1, "mapIndex %d has %d entries", i, official_seen[i]);
    // mapIndex <-> catalogue index: the same for the v7 set, appended for the rest
    for (int i = 0; i < MAP_CATALOG_OFFICIAL_COUNT; i++) {
        const int at = map_catalog_official_index(i);
        CHECK(at >= 0 && map_catalog_map_index((size_t) at) == i && map_catalog_official_json(i) == map_catalog_json((size_t) at), "official %d", i);
    }
    CHECK(map_catalog_official_index(32) == 69 && map_catalog_official_index(45) == 82 && map_catalog_official_index(46) == -1 &&
          map_catalog_official_index(-1) == -1 && map_catalog_map_index(32) == -1 && map_catalog_map_index(69) == 32 &&
          map_catalog_map_index(31) == 31 && map_catalog_map_index(33) == -1, "official mapIndex <-> catalogue index");
    CHECK(map_catalog_map_index_of_json(map_catalog_official_json(38)) == 38 && map_catalog_map_index_of_json(map_catalog_json(43)) == -1,
          "mapIndex of a JSON");
    CHECK(!strcmp(map_catalog_at(69)->name, "Bazaar") && !strcmp(map_catalog_at(82)->name, "Barnyard"), "appended official names");

    if (verbose) printf("catalogue:\n");
    for (size_t i = 0; i < count; i++) test_entry(i, verbose);

    // rotation == v10.0.2 rotationMaps (v10.js:167210) == g_rotation_maps
    static const int k_v10_rotation[] = {0, 1, 2, 3, 4, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 19, 21, 22, 23, 24, 25, 26, 28,
    32, 33, 34, 36, 37, 39, 40, 42, 44, 45};
    int rotation[64];
    const size_t rotation_count = map_catalog_rotation(rotation, 64);
    CHECK(rotation_count == sizeof(k_v10_rotation) / sizeof(k_v10_rotation[0]) &&
          rotation_count == sizeof(g_rotation_maps) / sizeof(g_rotation_maps[0]), "rotation size %zu", rotation_count);
    for (size_t i = 0; i < rotation_count; i++) {
        const int map = map_catalog_map_index((size_t) rotation[i]); // catalogue position -> mapIndex
        CHECK(map == g_rotation_maps[i] && map == k_v10_rotation[i], "rotation[%zu] = map %d", i, map);
    }
    load_default_maps();
    for (int i = 0; i < MAP_CATALOG_OFFICIAL_COUNT; i++) CHECK(g_maps[i] && g_maps[i] == map_catalog_official_json(i), "g_maps[%d]", i);

    // publicRoutes map_id
    for (size_t i = 0; i < sizeof(k_ids) / sizeof(k_ids[0]); i++) {
        const int index = map_catalog_index_of_key(k_ids[i].key);
        CHECK(index >= 0 && map_catalog_map_id(map_catalog_at((size_t) index)) == k_ids[i].id, "map_id of %s", k_ids[i].key);
    }
    // the display names make the 311_ / 152_ ids their own (v3.1.1's old_burg shared Burg's)
    const uint32_t burg_id = map_catalog_map_id(map_catalog_at(0));
    CHECK(map_catalog_map_id(map_catalog_at((size_t) map_catalog_index_of_key("311_burg"))) != burg_id &&
          map_catalog_map_id(map_catalog_at((size_t) map_catalog_index_of_key("152_burg"))) != burg_id &&
          map_catalog_map_id(map_catalog_at((size_t) map_catalog_index_of_key("152_burg"))) !=
              map_catalog_map_id(map_catalog_at((size_t) map_catalog_index_of_key("311_burg"))), "311_ / 152_ map ids");
    CHECK(!strcmp(map_catalog_at((size_t) map_catalog_index_of_key("311_burg"))->name, "Burg (3.1.1)") &&
          !strcmp(map_catalog_at((size_t) map_catalog_index_of_key("152_burg"))->name, "Burg (1.5.2)"), "display names");

    // the old keys (map_catalog.h aliases); the v3.1.1 stems name the v7 maps now, but newtown
    static const struct { const char *old, *key; } k_renamed[] = {
        {"v3_burg", "311_burg"}, {"v3_soul_sanctum", "311_soul_sanctum"}, {"V3_KANJI", "311_kanji"}, {"old_burg", "152_burg"},
        {"old_undergrowth", "152_undergrowth"}, {"newtown", "311_newtown"}, {"kanji", "kanji"}, {"burg_new", "burg_new"},
        {"311_oasis", "311_oasis"},
    };
    for (size_t i = 0; i < sizeof(k_renamed) / sizeof(k_renamed[0]); i++) {
        CHECK(map_catalog_index_of_alias(k_renamed[i].old) == map_catalog_index_of_key(k_renamed[i].key), "alias %s", k_renamed[i].old);
    }
    CHECK(map_catalog_index_of_alias("burg") == -1 && map_catalog_index_of_alias("sandstorm") == -1, "stems are names, not aliases");
    CHECK(map_catalog_index_of_old_key("OLD_BURG") == map_catalog_index_of_key("152_burg") && map_catalog_index_of_old_key("burg") == -1,
          "old keys only");
    CHECK(map_catalog_index_of_alias("nosuchmap") == -1 && map_catalog_index_of_alias("v3_lumber") == -1, "no alias");
    CHECK(map_catalog_index_of_file("assets/maps/burg.json") == 0 && map_catalog_index_of_file("sandstorm") == 2 &&
          map_catalog_index_of_file("maps/soul_sanctum.json") == 31 && map_catalog_index_of_file("newtown") == map_catalog_index_of_key("311_newtown") &&
          map_catalog_index_of_file("v3_kanji.json") == map_catalog_index_of_key("311_kanji") &&
          map_catalog_index_of_file("maps/v7/kanji.json") == 10 &&
          map_catalog_index_of_file("assets/maps/custom/old_kanji.json") == map_catalog_index_of_key("152_kanji") &&
          map_catalog_index_of_file("custom/Test") == map_catalog_index_of_key("Test") &&
          map_catalog_index_of_file("/tmp/nosuchmap.json") == -1, "index_of_file");
    cJSON *moved = NULL;
    CHECK(!map_catalog_load_file("assets/maps/v7/sanctum.json", &moved) && moved &&
          !strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(moved, "name")), "Soul Sanctum"), "load_file of a moved map");
    cJSON_Delete(moved);
    moved = NULL;
    CHECK(!map_catalog_load_file("assets/maps/custom/old_kanji.json", &moved) && moved &&
          !strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(moved, "name")), "Kanji"), "load_file of an old_ map");
    cJSON_Delete(moved);
    moved = NULL;
    CHECK(map_catalog_load_file("assets/maps/nosuchmap.json", &moved) == -1 && !moved, "load_file of a missing map");
    // the v3.1.1 / v1.5.2 maps carry MAP_KEY_V3_PLANES (v3.1.1 plane rotation), loaded by key or by path
    const cJSON *k311 = map_catalog_json((size_t) map_catalog_index_of_key("311_kanji"));
    const cJSON *k152 = map_catalog_json((size_t) map_catalog_index_of_key("152_kanji"));
    const cJSON *k7 = map_catalog_json(10);
    CHECK(k311 && cJSON_IsTrue(cJSON_GetObjectItem(k311, MAP_KEY_V3_PLANES)) && k152 &&
          cJSON_IsTrue(cJSON_GetObjectItem(k152, MAP_KEY_V3_PLANES)) && k7 && !cJSON_GetObjectItem(k7, MAP_KEY_V3_PLANES),
          "v3 plane flag on 311_ / 152_ maps only");
    CHECK(!map_catalog_load_file("assets/maps/311_kanji.json", &moved) && cJSON_IsTrue(cJSON_GetObjectItem(moved, MAP_KEY_V3_PLANES)),
          "v3 plane flag on a 311_ map loaded by path");
    cJSON_Delete(moved);
    moved = NULL;
    CHECK(!map_catalog_load_file("assets/maps/official/10_kanji.json", &moved) && !cJSON_GetObjectItem(moved, MAP_KEY_V3_PLANES),
          "no v3 plane flag on a v7 map loaded by path");
    cJSON_Delete(moved);
    moved = NULL;

    // cmd_map lookups: official names first, then community keys
    CHECK(map_catalog_find("burg") == 0, "find burg");
    CHECK(map_catalog_find("Soul") == 31, "find soul");
    CHECK(map_catalog_find("soul sanctum") == 31, "find soul sanctum");
    CHECK(map_catalog_find("tortuga") == 18, "find tortuga");
    CHECK(map_catalog_find("311_soul_sanctum") == map_catalog_index_of_key("311_soul_sanctum"), "find 311_soul_sanctum");
    CHECK(map_catalog_find("v3_soul_sanctum") == map_catalog_index_of_key("311_soul_sanctum"), "find v3_soul_sanctum");
    CHECK(map_catalog_find("152_burg") == map_catalog_index_of_key("152_burg"), "find 152_burg");
    CHECK(map_catalog_find("old_burg") == map_catalog_index_of_key("152_burg"), "find old_burg (not v7's Old_Burg)");
    CHECK(map_catalog_find("old") == 24, "find old: v7's Old_Burg");
    CHECK(map_catalog_find("sandstorm") == 2 && map_catalog_find("311_s") == map_catalog_index_of_key("311_sandstorm"), "find sandstorm");
    CHECK(map_catalog_find("newtown") == map_catalog_index_of_key("311_newtown"), "find newtown (a v3.1.1 stem)");
    CHECK(map_catalog_find("zombie") == map_catalog_index_of_key("zombie_high"), "find zombie");
    CHECK(map_catalog_find("SLIDE_") == map_catalog_index_of_key("Slide_Neon_Stage"), "find SLIDE_");
    CHECK(map_catalog_find("5") == 5, "find 5");
    CHECK(map_catalog_find("nosuchmap") == -1, "find nosuchmap");

    // console lookups (client `map`, server_bin `map`/`changelevel`)
    const char *const soul[] = {"Soul Sanctum", "soul sanctum", "SOUL SANCTUM", "\"Soul Sanctum\"", "Soul_Sanctum", "soulsanctum",
                                "'soul sanctum'", "sanctum"};
    for (size_t i = 0; i < sizeof(soul) / sizeof(soul[0]); i++) CHECK(map_catalog_resolve(soul[i], NULL, 0, NULL) == 31, "resolve %s", soul[i]);
    CHECK(map_catalog_resolve("311_soul_sanctum", NULL, 0, NULL) == map_catalog_index_of_key("311_soul_sanctum"), "resolve 311 key");
    CHECK(map_catalog_resolve("v3_soul_sanctum", NULL, 0, NULL) == map_catalog_index_of_key("311_soul_sanctum"), "resolve v3 key");
    CHECK(map_catalog_resolve("Kanji (3.1.1)", NULL, 0, NULL) == map_catalog_index_of_key("311_kanji"), "resolve 3.1.1 name");
    CHECK(map_catalog_resolve("burg 1.5.2", NULL, 0, NULL) == map_catalog_index_of_key("152_burg"), "resolve 1.5.2 name");
    CHECK(map_catalog_resolve("newtown", NULL, 0, NULL) == map_catalog_index_of_key("311_newtown"), "resolve newtown");
    CHECK(map_catalog_resolve("burg", NULL, 0, NULL) == 0, "resolve burg (v7)");
    CHECK(map_catalog_resolve("kanji", NULL, 0, NULL) == 10, "resolve kanji (the v7 key)");
    CHECK(map_catalog_resolve("Freight", NULL, 0, NULL) == 6, "resolve Freight");
    CHECK(map_catalog_resolve("old burg", NULL, 0, NULL) == map_catalog_index_of_key("152_burg"), "resolve old burg");
    CHECK(map_catalog_resolve("zombie", NULL, 0, NULL) == map_catalog_index_of_key("zombie_high"), "resolve zombie");
    CHECK(map_catalog_resolve("trop", NULL, 0, NULL) == 19, "resolve trop");
    int cands[8];
    size_t cand_n = 0;
    // "Slide Moonlight" is both the official map (v10 mapIndex 38) and the community "slide_moonlight"
    CHECK(map_catalog_resolve("slide", cands, 8, &cand_n) == -2 && cand_n == 3 &&
          cands[0] == map_catalog_index_of_key("Slide_Neon_Stage") && cands[1] == map_catalog_index_of_key("slide_moonlight") &&
          cands[2] == map_catalog_index_of_key("slide_moonlight_v10"), "resolve slide candidates");
    // the v10.0.2 additions by name and key (console `map`, the Games window's picks)
    CHECK(map_catalog_resolve("Bazaar", NULL, 0, NULL) == map_catalog_index_of_key("bazaar") && map_catalog_find("erupt") == map_catalog_index_of_key("erupt") &&
          map_catalog_resolve("Eterno Simulator", NULL, 0, NULL) == map_catalog_index_of_key("eterno_simulator") &&
          map_catalog_resolve("Stalk Factory", NULL, 0, NULL) == map_catalog_index_of_key("stalk_factory") &&
          map_catalog_resolve("hq", NULL, 0, NULL) == map_catalog_index_of_key("hq") && map_catalog_find("Frontier") == map_catalog_index_of_key("frontier") &&
          map_catalog_resolve("eterno_jump", NULL, 0, NULL) == map_catalog_index_of_key("eterno_jump"), "resolve the v10 additions");
    CHECK(map_catalog_resolve("nosuchmap", cands, 8, &cand_n) == -1 && cand_n == 0, "resolve nosuchmap");
    CHECK(map_catalog_resolve("\"\"", NULL, 0, NULL) == -1, "resolve empty quotes");

    // Games window tabs: Featured = the 46 official maps (mapIndex order); Popular / New = those, then the rest
    int tab[128];
    CHECK(map_catalog_games_tab(0, tab, 128) == count && tab[0] == 0 && tab[46] == map_catalog_index_of_key("311_soul_sanctum"), "popular");
    CHECK(map_catalog_games_tab(1, tab, 128) == 46 && tab[0] == 0 && tab[31] == 31 && tab[45] == map_catalog_index_of_key("barnyard"), "featured");
    CHECK(map_catalog_games_tab(2, tab, 128) == count && tab[46] == map_catalog_index_of_key("311_subzero"), "new");
    CHECK(map_catalog_games_tab(3, tab, 128) == 0, "favorites");
    CHECK(map_catalog_games_tab(4, tab, 128) == count, "mine");
    // every map the menu lists has a thumbnail file that loads (the Games / Host / Ranked / challenge
    // cards and the server list show ui/<thumb>, img/noimg.png otherwise): the official maps, the
    // v3.1.1 maps and the community maps with a picture; the others are listed in thumb_missing_on_purpose
    char thumb[64], path[160];
    CHECK(map_catalog_thumb(map_catalog_at(31), thumb, sizeof(thumb)) && !strcmp(thumb, "img/maps/map_31.png"), "official thumbnail name");
    CHECK(map_catalog_thumb(map_catalog_at((size_t) map_catalog_index_of_key("311_kanji")), thumb, sizeof(thumb)) &&
          !strcmp(thumb, "img/maps/v311/map_10.png"), "v3.1.1 thumbnail name");
    for (size_t i = 0; i < count; i++) {
        const MapCatalogEntry *e = map_catalog_at(i);
        const char *t = map_catalog_thumb(e, thumb, sizeof(thumb));
        if (!t) {
            CHECK(thumb_missing_on_purpose(e), "%s: no thumbnail, and not listed as missing", e->key);
            continue;
        }
        snprintf(path, sizeof(path), "ui/%s", t);
        CHECK(png_loads(path), "%s: thumbnail %s does not load", e->key, path);
    }
    // the ranked season pool's maps (the Ranked carousel and match history show ui/img/maps/map_<id>.png)
    for (int i = 0; i <= RANKED_POOL_COUNT; i++) {
        const int id = i < RANKED_POOL_COUNT ? g_ranked_map_pool[i] : RANKED_ALWAYS_MAP;
        snprintf(path, sizeof(path), "ui/img/maps/map_%d.png", id);
        CHECK(png_loads(path), "ranked map %d: %s does not load", id, path);
    }

    // map_catalog_slot: find-or-append
    const cJSON **list = NULL;
    size_t list_count = 0;
    CHECK(map_catalog_slot(&list, &list_count, 5) == 0 && map_catalog_slot(&list, &list_count, 20) == 1 &&
          map_catalog_slot(&list, &list_count, 5) == 0 && list_count == 2, "map_catalog_slot");
    free((void *) list);

    test_game_init_all();

    printf("%s: %d checks, %d failures\n", g_failures ? "map_catalog_test FAILED" : "Test OK", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
