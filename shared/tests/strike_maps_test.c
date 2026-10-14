// Krunker Strike's map pool (shared/include/strike_maps.h, shared/src/rules/strike/maps_strike.c):
// the seven maps in FRVR's table order, each standing in for the catalogue map of its name; every
// assets/maps/strike/<key>.json loads, passes v7.1.4's validateMap, builds with map_init (objects,
// spawns, prefab ids kept) and plays one game_init(). The catalogue hooks (shared/src/map_catalog.c)
// leave FLAVOR_BASE unchanged and, under FLAVOR_STRIKE, load the pool maps from FRVR's files and
// rotate the pool.
// Run from the repository root: ./<build>/strike_maps_test [-v]
#include <flavor.h>
#include <map_catalog.h>
#include <map_validate.h>
#include <prefabs.h>
#include <shared.h>
#include <strike_maps.h>
#include <stdio.h>
#include <string.h>

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

// FRVR 1.37.2's pool in its map-table order: key, mapName, fileName, catalogue (v7 mapIndex), the
// base catalogue key of that index, and the shipped file's object / spawn counts
static const struct {
    const char *key, *name, *frvr_file;
    int index;
    const char *base_key;
    int objects, spawns;
} k_want[] = {
    {"burg", "Burg", "burg_new", 0, "burg_new", 1604, 9},
    {"sandstorm", "Sandstorm", "sandstorm_v3", 2, "sandstorm_v3", 1339, 9},
    {"subzero", "Subzero", "subzero_new", 3, "subzero_new", 1272, 12},
    {"lostworld", "Lostworld", "lostworld_s6", 7, "lostworld_s6", 1328, 14},
    {"oasis", "Oasis", "oasis_v3", 9, "oasis_v3", 1333, 8},
    {"skytemple", "SkyTemple", "skytemple", 15, "skytemple_v2", 1192, 8},
    {"habitat", "Habitat", "habitat", 22, "habitat_v2", 1261, 8},
};
#define WANT_COUNT (sizeof(k_want) / sizeof(k_want[0]))

static int raw_count(const cJSON *json, const char *key) {
    return cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(json, key));
}

static void test_table(void) {
    const StrikeMap *pool = NULL;
    const size_t n = strike_map_pool(&pool);
    CHECK(n == WANT_COUNT && pool, "pool has %zu maps, want %zu", n, WANT_COUNT);
    CHECK(strike_map_pool(NULL) == n, "strike_map_pool(NULL) counts");
    for (size_t i = 0; pool && i < n && i < WANT_COUNT; i++) {
        const StrikeMap *m = &pool[i];
        char path[64];
        snprintf(path, sizeof(path), "maps/strike/%s.json", k_want[i].key);
        CHECK(!strcmp(m->key, k_want[i].key) && !strcmp(m->name, k_want[i].name) && !strcmp(m->frvr_file, k_want[i].frvr_file),
              "pool[%zu] = %s/%s/%s", i, m->key, m->name, m->frvr_file);
        CHECK(!strcmp(m->path, path), "%s path %s", m->key, m->path);
        CHECK(m->catalog_index == k_want[i].index && m->weight == 1, "%s index %d weight %d", m->key, m->catalog_index, m->weight);
        const MapCatalogEntry *e = map_catalog_at((size_t) m->catalog_index);
        CHECK(e && !strcmp(e->key, k_want[i].base_key) && !strcmp(e->name, m->name) && e->source == MAP_SOURCE_OFFICIAL,
              "%s stands in for %s (%s)", m->key, e ? e->key : "?", e ? e->name : "?");
        CHECK(strike_map_for_catalog((size_t) m->catalog_index) == m, "%s: for_catalog(%d)", m->key, m->catalog_index);
        CHECK(strike_map_by_key(m->key) == m && strike_map_by_key(m->name) == m, "%s: by_key", m->key);
        // a file path names its catalogue map ("maps/strike/burg.json" -> Burg), like the 311_ files
        CHECK(map_catalog_index_of_file(m->path) == m->catalog_index, "%s: index_of_file(%s)", m->key, m->path);
    }
    CHECK(strike_map_by_key("SKYTEMPLE") && strike_map_by_key("lostWorld"), "by_key is case-insensitive");
    CHECK(!strike_map_by_key("kanji") && !strike_map_by_key("") && !strike_map_by_key(NULL) && !strike_map_by_key("burg_new"),
          "by_key: only pool keys and names");
    int in_pool = 0;
    for (size_t i = 0; i < map_catalog_count() + 4; i++) in_pool += strike_map_for_catalog(i) != NULL;
    CHECK(in_pool == (int) WANT_COUNT, "%d catalogue maps in the pool", in_pool);
}

// The shipped files themselves: parse, v7 validateMap, map_init
static void test_files(const int verbose) {
    const StrikeMap *pool = NULL;
    const size_t n = strike_map_pool(&pool);
    for (size_t i = 0; i < n && i < WANT_COUNT; i++) {
        char path[96];
        snprintf(path, sizeof(path), "assets/%s", pool[i].path);
        cJSON *json = NULL;
        CHECK(!load_json_file(path, &json) && cJSON_IsObject(json), "%s does not load", path);
        if (!json) continue;
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, "name"));
        CHECK(name && !strcmp(name, pool[i].name), "%s: file name '%s'", path, name ? name : "?");
        CHECK(raw_count(json, "objects") == k_want[i].objects && raw_count(json, "spawns") == k_want[i].spawns,
              "%s: %d objects %d spawns", path, raw_count(json, "objects"), raw_count(json, "spawns"));
        const char *error = map_validate_error(json, 0);
        CHECK(error == NULL, "%s: v7 validateMap: %s", path, error ? error : "");

        map_sanitize_json(json);
        int kept = 0, remapped = 0;
        const cJSON *o;
        cJSON_ArrayForEach(o, cJSON_GetObjectItemCaseSensitive(json, "objects")) {
            if (!prefab_has_role(map_raw_object_prefab(o), PREFAB_R_NO_OBJECT)) kept++;
            const cJSON *prefab = cJSON_GetObjectItemCaseSensitive(o, "i");
            if (cJSON_IsNumber(prefab) && prefab->valuedouble >= 0 && prefab->valuedouble < PREFAB_COUNT &&
                map_raw_object_prefab(o) != (int) prefab->valuedouble) remapped++;
        }
        Map *map = map_init(json);
        CHECK(map != NULL, "%s: map_init failed", path);
        if (map) {
            CHECK((int) map->spawn_count == k_want[i].spawns, "%s: %zu spawns built", path, map->spawn_count);
            CHECK((int) map->object_count == kept, "%s: %zu of %d objects built", path, map->object_count, kept);
            CHECK(remapped == 0, "%s: %d objects lose their prefab id", path, remapped);
            if (verbose) printf("  %-10s %-9s objects %4zu spawns %2zu (FRVR %s.json, stands in for %s)\n", pool[i].key,
                                pool[i].name, map->object_count, map->spawn_count, pool[i].frvr_file, k_want[i].base_key);
            map_fini(map);
            free(map);
        }
        cJSON_Delete(json);
    }
}

// FLAVOR_BASE: v7's rotation and files. Loads only Habitat (index 22) through the catalogue, which
// caches it for the process: the Strike checks below use the other six.
static void test_base(void) {
    CHECK(flavor_get() == FLAVOR_BASE, "the test starts on the base flavor");
    int rotation[64];
    const size_t n = map_catalog_rotation(rotation, 64);
    CHECK(n == 24 && rotation[0] == 0 && rotation[23] == 28, "base rotation: %zu maps (v7 rotationMaps)", n);
    const cJSON *habitat = map_catalog_json(22);
    CHECK(habitat && raw_count(habitat, "objects") == 1374, "base Habitat is habitat_v2 (%d objects)", habitat ? raw_count(habitat, "objects") : -1);
}

static void test_strike(void) {
    CHECK(flavor_set(FLAVOR_STRIKE) && flavor_get() == FLAVOR_STRIKE, "flavor_set(FLAVOR_STRIKE)");
    int rotation[64];
    const size_t n = map_catalog_rotation(rotation, 64);
    CHECK(n == WANT_COUNT, "Strike rotation: %zu maps", n);
    for (size_t i = 0; i < n && i < WANT_COUNT; i++) CHECK(rotation[i] == k_want[i].index, "rotation[%zu] = %d", i, rotation[i]);
    int small[3];
    CHECK(map_catalog_rotation(small, 3) == 3 && small[2] == 3, "Strike rotation clamps to max");
    CHECK(map_catalog_rotation(NULL, 0) == WANT_COUNT, "Strike rotation count");

    for (size_t i = 0; i < WANT_COUNT; i++) {
        if (k_want[i].index == 22) continue; // cached by test_base
        const cJSON *json = map_catalog_json((size_t) k_want[i].index);
        CHECK(json && raw_count(json, "objects") == k_want[i].objects && raw_count(json, "spawns") == k_want[i].spawns,
              "Strike %s loads FRVR's file (%d objects)", k_want[i].key, json ? raw_count(json, "objects") : -1);
        const char *name = json ? cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, "name")) : NULL;
        CHECK(name && !strcmp(name, k_want[i].name), "Strike %s: name %s", k_want[i].key, name ? name : "?");
    }
    // a map outside the pool keeps its catalogue file
    const cJSON *kanji = map_catalog_json(10);
    CHECK(kanji && raw_count(kanji, "objects") > 0, "Kanji still loads under Strike");

    // one round of the shared game on every pool map
    size_t count = 0;
    const cJSON **maps = map_catalog_list(rotation, n, &count);
    CHECK(maps && count == n, "map_catalog_list: %zu of %zu pool maps", count, n);
    if (!maps) return;
    static Game game;
    game_configure(&game, NULL, maps, count, NULL, 0, NULL, 0, NULL, 0);
    for (size_t i = 0; i < count; i++) {
        game_init(&game, (int) i, 0, 0);
        CHECK(game.ready && game.map && game.current_map_index == (int) i && game.map->spawn_count > 0,
              "game_init on Strike %s failed", k_want[i].key);
    }
}

int main(const int argc, char **argv) {
    const int verbose = argc > 1 && !strcmp(argv[1], "-v");
    test_table();
    test_files(verbose);
    test_base();
    test_strike();
    printf("strike_maps_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
