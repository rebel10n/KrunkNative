// Krunker Strike's map pool (shared/include/strike_maps.h, shared/src/rules/strike/maps_strike.c):
// the seven maps in FRVR's table order, each one Krunker v10.0.2's official map of its name; every
// pool file (assets/maps/official/NN_<name>.json, the same file the base catalogue loads) passes
// v7.1.4's validateMap, builds with map_init (objects, spawns, prefab ids kept) and plays one
// game_init(). The catalogue hooks (shared/src/map_catalog.c) leave FLAVOR_BASE unchanged and, under
// FLAVOR_STRIKE, load the pool maps from their pool path and rotate the pool.
// Run from the repository root: ./<build>/strike_maps_test [-v]
#include <flavor.h>
#include <map_catalog.h>
#include <map_validate.h>
#include <prefabs.h>
#include <shared.h>
#include <strike_maps.h>
#include <ruleset.h>
#include <stdlib.h>
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

// The pool in its map-table order: key, mapName, the v10.0.2 file stem, catalogue index (the v10 mapIndex),
// the base catalogue key of that index, and the v10 file's object / spawn counts (krunker_maps.h)
static const struct {
    const char *key, *name, *file;
    int index;
    const char *base_key;
    int objects, spawns;
} k_want[] = {
    {"burg", "Burg", "00_burg", 0, "burg_new", 1627, 9},
    {"sandstorm", "Sandstorm", "02_sandstorm", 2, "sandstorm_v3", 1362, 9},
    {"subzero", "Subzero", "03_subzero", 3, "subzero_new", 1288, 12},
    {"lostworld", "Lostworld", "07_lostworld", 7, "lostworld_s6", 1336, 14},
    {"oasis", "Oasis", "09_oasis", 9, "oasis_v3", 1338, 8},
    {"skytemple", "SkyTemple", "15_skytemple", 15, "skytemple_v2", 1351, 8},
    {"habitat", "Habitat", "22_habitat", 22, "habitat_v2", 1374, 8},
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
        snprintf(path, sizeof(path), "maps/official/%s.json", k_want[i].file);
        CHECK(!strcmp(m->key, k_want[i].key) && !strcmp(m->name, k_want[i].name) && !strcmp(m->file, k_want[i].file),
              "pool[%zu] = %s/%s/%s", i, m->key, m->name, m->file);
        CHECK(!strcmp(m->path, path), "%s path %s", m->key, m->path);
        CHECK(m->catalog_index == k_want[i].index && m->weight == 1, "%s index %d weight %d", m->key, m->catalog_index, m->weight);
        const MapCatalogEntry *e = map_catalog_at((size_t) m->catalog_index);
        CHECK(e && !strcmp(e->key, k_want[i].base_key) && !strcmp(e->name, m->name) && e->source == MAP_SOURCE_OFFICIAL,
              "%s stands in for %s (%s)", m->key, e ? e->key : "?", e ? e->name : "?");
        // identical to v10: the pool path is the catalogue's own v10 file, so both flavors load the same map
        CHECK(e && !strcmp(e->path, m->path), "%s: pool path %s is the catalogue's %s", m->key, m->path, e ? e->path : "?");
        CHECK(strike_map_for_catalog((size_t) m->catalog_index) == m, "%s: for_catalog(%d)", m->key, m->catalog_index);
        CHECK(strike_map_by_key(m->key) == m && strike_map_by_key(m->name) == m, "%s: by_key", m->key);
        // a file path names its catalogue map ("maps/official/00_burg.json" -> Burg), like the 311_ files
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
            if (verbose) printf("  %-10s %-9s objects %4zu spawns %2zu (v10 %s.json)\n", pool[i].key,
                                pool[i].name, map->object_count, map->spawn_count, pool[i].file);
            map_fini(map);
            free(map);
        }
        cJSON_Delete(json);
    }
}

// FLAVOR_BASE: v10's rotation and files. Loads only Habitat (index 22) through the catalogue, which
// caches it for the process: the Strike checks below use the other six.
static void test_base(void) {
    CHECK(flavor_get() == FLAVOR_BASE, "the test starts on the base flavor");
    int rotation[64];
    const size_t n = map_catalog_rotation(rotation, 64);
    // map_catalog_rotation returns catalogue positions; the maps are v10 mapIndexes (g_rotation_maps)
    CHECK(n == 34 && map_catalog_map_index((size_t) rotation[0]) == 0 && map_catalog_map_index((size_t) rotation[23]) == 28 &&
          map_catalog_map_index((size_t) rotation[24]) == 32 && map_catalog_map_index((size_t) rotation[33]) == 46,
          "base rotation: %zu maps (v10 rotationMaps, deob-10.0.6.js:169385)", n);
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
              "Strike %s loads the v10 file (%d objects)", k_want[i].key, json ? raw_count(json, "objects") : -1);
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

// The rotation a game plays without a map list (game_configure / map_list_with_custom): under Strike
// FRVR's pool, each map listed StrikeMap.weight times (mapPool's mapWeight); base keeps v7's
// rotationMaps. Runs after test_strike (the pool files are cached by then).
// The controls tutorial's sandbox (strike_ftue_map): FRVR's bot_ftue_v2 file, outside the pool and the rotation,
// and the Strike catalogue loads that copy for it (the base catalogue keeps Krunker v10's custom file).
static void test_ftue(void) {
    const StrikeMap *m = strike_ftue_map();
    CHECK(m && !strcmp(m->key, "bot_ftue_v2") && !strcmp(m->path, "maps/strike/bot_ftue_v2.json") && m->catalog_index < 0 && m->weight == 0,
          "the FTUE sandbox entry");
    CHECK(strike_map_by_key("bot_ftue_v2") == NULL && strike_map_for_catalog(0) != m, "the FTUE sandbox is not a pool map");
    char path[96];
    snprintf(path, sizeof(path), "assets/%s", m->path);
    cJSON *json = NULL;
    CHECK(!load_json_file(path, &json) && cJSON_IsObject(json), "%s does not load", path);
    if (json) {
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, "name"));
        CHECK(name && !strcmp(name, "Mini Industry Tutorial"), "%s: file name '%s'", path, name ? name : "?");
        CHECK(raw_count(json, "objects") == 647, "%s: %d objects", path, raw_count(json, "objects"));
        // v7's validateMap refuses FRVR's own name (a sandbox map, never in the public list): documented, not a failure
        const char *error = map_validate_error(json, 0);
        CHECK(error && strstr(error, "name too long"), "%s: v7 validateMap: %s", path, error ? error : "passes");
        cJSON_Delete(json);
    }
    const int index = map_catalog_index_of_key("bot_ftue_v2");
    CHECK(index >= 0, "bot_ftue_v2 is in the catalogue");
    const cJSON *loaded = map_catalog_json((size_t) index); // the catalogue shows its own display name
    CHECK(loaded && raw_count(loaded, "objects") == 647, "Strike loads FRVR's bot_ftue_v2 (%d objects)",
          loaded ? raw_count(loaded, "objects") : -1);
}

static void test_rotation(void) {
    // the weighting rule on a pool of our own: weights 2, 0, 3
    const StrikeMap pool[] = {{"a", "A", "a", "a.json", 4, 2}, {"b", "B", "b", "b.json", 5, 0}, {"c", "C", "c", "c.json", 6, 3}};
    int out[8];
    CHECK(strike_map_pool_expand(pool, 3, out, 8) == 5 && out[0] == 4 && out[1] == 4 && out[2] == 6 && out[4] == 6,
          "weights 2 / 0 / 3 expand to 4 4 6 6 6");
    CHECK(strike_map_pool_expand(pool, 3, NULL, 0) == 5 && strike_map_pool_expand(pool, 3, out, 2) == 2, "expand: count, clamp");

    CHECK(flavor_set(FLAVOR_STRIKE), "flavor_set(FLAVOR_STRIKE)");
    int rotation[64];
    const size_t n = strike_map_rotation(rotation, 64);
    CHECK(n == WANT_COUNT && map_catalog_rotation(NULL, 0) == n, "Strike rotation = the weighted pool (%zu)", n);
    const Ruleset *rules = ruleset_current();
    CHECK(rules->rotation_map_count == n && !memcmp(rules->rotation_maps, rotation, n * sizeof(int)), "strike_ruleset() rotation_maps = the pool");

    static Game game;
    game_configure(&game, NULL, NULL, 0, NULL, 0, NULL, 0, NULL, 0);
    CHECK(game.map_count == n, "Strike game_configure without maps: %zu pool maps (got %zu)", n, game.map_count);
    for (size_t i = 0; i < game.map_count && i < n; i++) {
        CHECK(game.maps[i] == map_catalog_json((size_t) rotation[i]), "Strike game map %zu is pool map %d", i, rotation[i]);
    }
    int in_pool = 1;
    for (size_t i = 0; i < game.map_count; i++) in_pool &= map_catalog_index_of_json(game.maps[i]) >= 0 && strike_map_for_catalog((size_t) map_catalog_index_of_json(game.maps[i])) != NULL;
    CHECK(in_pool, "Strike: never a map outside FRVR's pool (v7 rotationMaps)");
    for (int round = 0; round < 3; round++) {
        game_init(&game, -1, 0, 0);
        CHECK(game.ready && game.map && strike_map_for_catalog((size_t) map_catalog_index_of_json(game.maps[game.current_map_index])) != NULL,
              "Strike round %d plays a pool map", round);
    }

    cJSON *custom = cJSON_Parse("{\"name\":\"custom\"}");
    size_t list_count = 0;
    const cJSON **list = map_list_with_custom(custom, &list_count);
    CHECK(list && list_count == n + 1 && list[0] == custom && list[1] == map_catalog_json((size_t) rotation[0]),
          "Strike map_list_with_custom: the custom map then the pool (%zu)", list_count);
    free(list);

    // base: v7's rotationMaps, as before
    CHECK(flavor_set(FLAVOR_BASE), "flavor_set(FLAVOR_BASE)");
    game_configure(&game, NULL, NULL, 0, NULL, 0, NULL, 0, NULL, 0);
    const size_t base_n = sizeof(g_rotation_maps) / sizeof(g_rotation_maps[0]);
    int same = game.map_count == base_n;
    for (size_t i = 0; same && i < base_n; i++) same = game.maps[i] == g_maps[g_rotation_maps[i]];
    CHECK(same, "base game_configure without maps: v7's %zu rotation maps (got %zu)", base_n, game.map_count);
    CHECK(ruleset_current()->rotation_maps == g_rotation_maps, "base ruleset rotation_maps = g_rotation_maps");
    list = map_list_with_custom(custom, &list_count);
    CHECK(list && list_count == base_n + 1 && list[1] == g_maps[g_rotation_maps[0]], "base map_list_with_custom (%zu)", list_count);
    free(list);
    cJSON_Delete(custom);
}

int main(const int argc, char **argv) {
    const int verbose = argc > 1 && !strcmp(argv[1], "-v");
    test_table();
    test_files(verbose);
    test_base();
    test_strike();
    test_ftue(); // Strike's flavor (test_strike set it; test_rotation ends on the base flavor)
    test_rotation();
    printf("strike_maps_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
