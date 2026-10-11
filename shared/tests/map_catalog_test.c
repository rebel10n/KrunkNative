// The v3.1.1 map catalogue (shared/src/map_catalog.c): every map file loads, its JSON
// "name" is the catalogue name, map_init() builds it (objects, spawns, prefab fallbacks,
// pooled "si" sizes), ids / rotation / Games-window lists match the v3.1.1 capture, and
// cmd_map style lookups resolve like packetHandlers.ts. Also plays one game_init() on
// every map. Run from the repository root: ./<build>/map_catalog_test [-v]
#include <shared.h>
#include <map_catalog.h>
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

// map_id values the v3.1.1 offline server sent (doc/ref/krunker-v3.1.1/windows/win-games.html)
static const struct {
    const char *key;
    uint32_t id;
} k_ids[] = {
    {"soul_sanctum", 2717856513u}, {"subzero", 1389891507u}, {"undergrowth", 2699178862u}, {"lostworld", 2026576977u},
    {"newtown", 4056084657u}, {"oasis", 2101154094u}, {"sandstorm", 88334292u}, {"shipyard", 2613134335u},
    {"freight", 1010451404u}, {"industry", 312130495u}, {"kanji", 1336255044u}, {"littletown", 3836435553u},
    {"burg", 1257833875u}, {"citadel", 1963634499u}, {"evacuation", 4241009034u}, {"zombie_high", 569198284u},
    {"old_sandstorm", 88334292u}, {"slide_moonlight", 3690042710u}, {"SniperRoyale2", 2800345118u},
    {"Test", 2949673445u}, {"Verb_Arena", 682138864u}, {"eterno_jump", 739315237u}, {"Krunk_Plaza", 537274925u},
    {"Slide_Neon_Stage", 197077862u}, {"Bhop_VIP", 3459552096u}, {"ClashRoyale", 2789783172u},
    {"ESP_Parkour", 2793053018u}, {"FortniteTiltedT", 563473844u},
};

static int count_prefab(const Map *map, const int prefab) {
    int n = 0;
    for (size_t i = 0; i < map->object_count; i++) n += map->objects[i]->prefab == prefab;
    return n;
}

static void test_entry(const size_t index, const int verbose) {
    const MapCatalogEntry *e = map_catalog_at(index);
    const cJSON *json = map_catalog_json(index);
    CHECK(json != NULL, "%s: JSON did not load", e->key);
    if (!json) return;

    const cJSON *name = cJSON_GetObjectItemCaseSensitive(json, "name");
    CHECK(cJSON_IsString(name) && !strcmp(name->valuestring, e->name), "%s: JSON name '%s' != catalogue '%s'", e->key,
          cJSON_IsString(name) ? name->valuestring : "?", e->name);
    const cJSON *creator = cJSON_GetObjectItemCaseSensitive(json, "creator");
    const char *want_creator = cJSON_IsString(creator) && creator->valuestring[0] ? creator->valuestring
                               : e->source == MAP_SOURCE_OFFICIAL ? "Krunker" : "Community";
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
        const cJSON *prefab = cJSON_GetObjectItemCaseSensitive(o, "i");
        // "BOT" objects become AIs (objects.js addAI, shared/src/ai.c), never map objects
        const int bot = (cJSON_IsNumber(prefab) && (int) prefab->valuedouble == PREFAB_BOT) ||
                        (cJSON_IsString(prefab) && !strcmp(prefab->valuestring, "BOT"));
        if (!bot) raw_objects++;
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
    for (size_t i = 0; i < map->object_count; i++) out_of_range += map->objects[i]->prefab < 0 || map->objects[i]->prefab > PREFAB_KNIGHT;
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
    int all[64];
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
    CHECK(count == 36, "catalogue has %zu maps, v3.1.1 ships 15 + 21", count);

    // official maps: catalogue index == v3.1.1 mapIndex; keys unique
    for (size_t i = 0; i < count; i++) {
        const MapCatalogEntry *e = map_catalog_at(i);
        if (i < MAP_CATALOG_OFFICIAL_COUNT) CHECK(e->source == MAP_SOURCE_OFFICIAL && e->index == (int) i, "%s index", e->key);
        else CHECK(e->source == MAP_SOURCE_CUSTOM && e->index == -1 && !e->rotation && !e->hostable, "%s custom flags", e->key);
        CHECK(map_catalog_index_of_key(e->key) == (int) i, "%s key not unique", e->key);
        if (i > MAP_CATALOG_OFFICIAL_COUNT) CHECK(strcmp(map_catalog_at(i - 1)->key, e->key) < 0, "%s not in readdir order", e->key);
    }

    if (verbose) printf("catalogue:\n");
    for (size_t i = 0; i < count; i++) test_entry(i, verbose);

    // rotation == src/data/map.js rotationMaps == g_rotation_maps
    int rotation[64];
    const size_t rotation_count = map_catalog_rotation(rotation, 64);
    CHECK(rotation_count == sizeof(g_rotation_maps) / sizeof(g_rotation_maps[0]), "rotation size %zu", rotation_count);
    for (size_t i = 0; i < rotation_count; i++) CHECK(rotation[i] == g_rotation_maps[i], "rotation[%zu] = %d", i, rotation[i]);
    // the Raid end-vote candidate "23,13" needs map 13 = Soul Sanctum
    CHECK(!strcmp(map_catalog_at(13)->name, "Soul Sanctum"), "mapIndex 13 is not Soul Sanctum");
    load_default_maps();
    for (size_t i = 0; i < MAP_CATALOG_OFFICIAL_COUNT; i++) CHECK(g_maps[i] == map_catalog_json(i), "g_maps[%zu]", i);

    // publicRoutes map_id
    for (size_t i = 0; i < sizeof(k_ids) / sizeof(k_ids[0]); i++) {
        const int index = map_catalog_index_of_key(k_ids[i].key);
        CHECK(index >= 0 && map_catalog_map_id(map_catalog_at((size_t) index)) == k_ids[i].id, "map_id of %s", k_ids[i].key);
    }

    // cmd_map lookups
    CHECK(map_catalog_find("burg") == 0, "find burg");
    CHECK(map_catalog_find("Soul") == 13, "find soul");
    CHECK(map_catalog_find("soul sanctum") == 13, "find soul sanctum");
    CHECK(map_catalog_find("soul_sanctum") == 13, "find soul_sanctum");
    CHECK(map_catalog_find("old_burg") == map_catalog_index_of_key("old_burg"), "find old_burg");
    CHECK(map_catalog_find("zombie") == map_catalog_index_of_key("zombie_high"), "find zombie");
    CHECK(map_catalog_find("SLIDE_") == map_catalog_index_of_key("Slide_Neon_Stage"), "find SLIDE_");
    CHECK(map_catalog_find("5") == 5, "find 5");
    CHECK(map_catalog_find("nosuchmap") == -1, "find nosuchmap");

    // console lookups (client `map`, server_bin `map`/`changelevel`): Soul Sanctum is not in
    // the rotation, so these resolve against the catalogue
    const char *const soul[] = {"Soul Sanctum", "soul sanctum", "SOUL SANCTUM", "\"Soul Sanctum\"", "soul_sanctum",
                                "Soul_Sanctum", "  soul  ", "soul", "Sou", "soulsanctum", "'soul sanctum'"};
    for (size_t i = 0; i < sizeof(soul) / sizeof(soul[0]); i++) CHECK(map_catalog_resolve(soul[i], NULL, 0, NULL) == 13, "resolve %s", soul[i]);
    CHECK(map_catalog_resolve("burg", NULL, 0, NULL) == 0, "resolve burg (key beats old_burg's name)");
    CHECK(map_catalog_resolve("Freight", NULL, 0, NULL) == 6, "resolve Freight");
    CHECK(map_catalog_resolve("old burg", NULL, 0, NULL) == map_catalog_index_of_key("old_burg"), "resolve old burg");
    CHECK(map_catalog_resolve("zombie", NULL, 0, NULL) == map_catalog_index_of_key("zombie_high"), "resolve zombie");
    CHECK(map_catalog_resolve("sand", NULL, 0, NULL) == 2, "resolve sand (old_sandstorm's label is its key)");
    CHECK(map_catalog_resolve("ship", NULL, 0, NULL) == 5, "resolve ship");
    int cands[8];
    size_t cand_n = 0;
    // Sandstorm, Subzero, Shipyard, Soul Sanctum, SniperRoyale2, Slide_Neon_Stage, slide_moonlight;
    // only `max` are written, the count is the total
    CHECK(map_catalog_resolve("s", cands, 3, &cand_n) == -2 && cand_n == 7 && cands[0] == 2 && cands[2] == 5,
          "resolve s ambiguous (%zu)", cand_n);
    CHECK(map_catalog_resolve("slide", cands, 8, &cand_n) == -2 && cand_n == 2 &&
          cands[0] == map_catalog_index_of_key("Slide_Neon_Stage") && cands[1] == map_catalog_index_of_key("slide_moonlight"),
          "resolve slide candidates");
    CHECK(map_catalog_resolve("lit", cands, 8, &cand_n) == 1, "resolve lit");
    CHECK(map_catalog_resolve("nosuchmap", cands, 8, &cand_n) == -1 && cand_n == 0, "resolve nosuchmap");
    CHECK(map_catalog_resolve("\"\"", NULL, 0, NULL) == -1, "resolve empty quotes");

    // Games window tabs (capture): Popular / New list all 36, Featured the 15 official ones
    int tab[64];
    CHECK(map_catalog_games_tab(0, tab, 64) == 36 && tab[0] == 13, "popular");
    CHECK(map_catalog_games_tab(1, tab, 64) == 15 && tab[0] == 12, "featured");
    CHECK(map_catalog_games_tab(2, tab, 64) == 36 && tab[0] == 3, "new");
    CHECK(map_catalog_games_tab(3, tab, 64) == 0, "favorites");
    CHECK(map_catalog_games_tab(4, tab, 64) == 36 && tab[0] == 0 && tab[1] == 8 && tab[15] == 15 && tab[35] == 35, "mine");

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
