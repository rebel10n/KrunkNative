// The in-process social hub's map search with no brain (shared/src/social.c api_search, /api/search?type=map&val=&cat=).
// A Popular Categories tile searches its category with an empty name, and a name search can carry one too; the
// brain answers both by services/brain/creations.c (h_search, run, catalog_wanted): the catalogue keeps no categories,
// so a category search lists no catalogue map, a name search with All (cat -1 or none) lists the name's catalogue maps,
// and a catalogue row carries category "" and map_category -1.
// Run from the repository root: ./<build>/social_hub_test
#include <shared.h>
#include <map_catalog.h>
#include <social.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;
static int g_checks;

#define CHECK(cond, ...) do { \
    g_checks++; \
    if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_failures++; } \
} while (0)

// One "api" /search frame with `params` (a JSON object); returns the reply's data rows (a copy the caller deletes).
static cJSON *search_rows(const SocialHost *host, const char *params) {
    char request[512];
    snprintf(request, sizeof(request), "[\"api\",\"/search\",%s]", params);
    char *reply = social_service_handle(host, request);
    cJSON *frames = reply ? cJSON_Parse(reply) : NULL;
    free(reply);
    cJSON *rows = NULL;
    const cJSON *body = cJSON_GetArrayItem(cJSON_GetArrayItem(frames, 0), 2); // ["api", "/search", {data, index, error}]
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(body, "data");
    if (cJSON_IsArray(data)) rows = cJSON_Duplicate(data, 1);
    cJSON_Delete(frames);
    return rows ? rows : cJSON_CreateArray();
}

static const char *row_name(const cJSON *rows, const int i) {
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(rows, i), "map_name");
    return cJSON_IsString(name) ? name->valuestring : "";
}

// The server's own maps (Game.maps, what net.c's social host lists): the catalogue's official maps, no categories.
static Game test_game;
static const cJSON *test_maps[2];

static void test_in_process_maps(void) {
    test_maps[0] = cJSON_Parse("{\"name\":\"Sandstorm\"}");
    test_maps[1] = cJSON_Parse("{\"name\":\"Kanji\"}");
    test_game.maps = test_maps;
    test_game.map_count = 2;
    SocialMapInfo maps[8];
    const SocialHost host = {.maps = maps, .map_count = social_maps_from_game(&test_game, maps, 8)};
    CHECK(host.map_count == 2, "the server lists its 2 maps (%zu)", host.map_count);

    // a category tile (empty name, cat=<n>) lists that category's maps: the catalogue has none, for any category
    for (int c = 0; c < MAP_CATEGORY_COUNT; c++) {
        char params[96];
        snprintf(params, sizeof(params), "{\"type\":\"map\",\"val\":\"\",\"cat\":%d}", c);
        cJSON *rows = search_rows(&host, params);
        CHECK(cJSON_GetArraySize(rows) == 0, "tile cat %d lists %d maps (the catalogue keeps no categories)", c, cJSON_GetArraySize(rows));
        cJSON_Delete(rows);
    }

    // a name search with a category keeps that category's maps only: none here
    cJSON *rows = search_rows(&host, "{\"type\":\"map\",\"val\":\"sand\",\"cat\":19}");
    CHECK(cJSON_GetArraySize(rows) == 0, "Action + \"sand\" lists %d maps (Action keeps none of the catalogue's)", cJSON_GetArraySize(rows));
    cJSON_Delete(rows);

    // All (cat -1, or none) searches by name; its catalogue row carries the brain's category fields ("" and -1)
    rows = search_rows(&host, "{\"type\":\"map\",\"val\":\"sand\",\"cat\":-1}");
    CHECK(cJSON_GetArraySize(rows) == 1 && !strcmp(row_name(rows, 0), "Sandstorm"), "All + \"sand\": %d rows", cJSON_GetArraySize(rows));
    const cJSON *row = cJSON_GetArrayItem(rows, 0);
    const cJSON *category = cJSON_GetObjectItemCaseSensitive(row, "category");
    const cJSON *map_category = cJSON_GetObjectItemCaseSensitive(row, "map_category");
    CHECK(cJSON_IsString(category) && !category->valuestring[0], "catalogue row category is \"\"");
    CHECK(cJSON_IsNumber(map_category) && map_category->valueint == -1, "catalogue row map_category is -1");
    cJSON_Delete(rows);

    rows = search_rows(&host, "{\"type\":\"map\",\"val\":\"sand\"}");
    CHECK(cJSON_GetArraySize(rows) == 1 && !strcmp(row_name(rows, 0), "Sandstorm"), "no cat + \"sand\": %d rows", cJSON_GetArraySize(rows));
    cJSON_Delete(rows);

    // no name and All lists nothing (the brain's rule); a cat outside the category list is All; mods have no registry
    rows = search_rows(&host, "{\"type\":\"map\",\"val\":\"\",\"cat\":-1}");
    CHECK(cJSON_GetArraySize(rows) == 0, "All with no name lists %d maps", cJSON_GetArraySize(rows));
    cJSON_Delete(rows);
    rows = search_rows(&host, "{\"type\":\"map\",\"val\":\"sand\",\"cat\":99}");
    CHECK(cJSON_GetArraySize(rows) == 1, "cat 99 (out of range) is All: %d rows", cJSON_GetArraySize(rows));
    cJSON_Delete(rows);
    rows = search_rows(&host, "{\"type\":\"mod\",\"val\":\"sand\",\"cat\":-1}");
    CHECK(cJSON_GetArraySize(rows) == 0, "mod search lists %d rows (no registry)", cJSON_GetArraySize(rows));
    cJSON_Delete(rows);

    for (size_t i = 0; i < 2; i++) cJSON_Delete((cJSON *) test_maps[i]);
}

// A map with a category (what a published map carries, the brain's map_category): a category search lists that category's
// maps only, a name search with cat= keeps them too, and the row carries the category's name and index.
static void test_categorised_maps(void) {
    const SocialMapInfo maps[] = {
        {.name = "Sandstorm", .creator = "Krunker", .description = "", .featured = 1, .category = -1, .category_name = ""},
        {.name = "Arena Blitz", .creator = "Player", .description = "", .featured = 0, .category = 19, .category_name = "Action"},
    };
    const SocialHost host = {.maps = maps, .map_count = 2};

    cJSON *rows = search_rows(&host, "{\"type\":\"map\",\"val\":\"\",\"cat\":19}");
    CHECK(cJSON_GetArraySize(rows) == 1 && !strcmp(row_name(rows, 0), "Arena Blitz"), "Action tile: %d rows", cJSON_GetArraySize(rows));
    const cJSON *row = cJSON_GetArrayItem(rows, 0);
    const cJSON *category = cJSON_GetObjectItemCaseSensitive(row, "category");
    const cJSON *map_category = cJSON_GetObjectItemCaseSensitive(row, "map_category");
    CHECK(cJSON_IsString(category) && !strcmp(category->valuestring, "Action"), "Action row category is \"Action\"");
    CHECK(cJSON_IsNumber(map_category) && map_category->valueint == 19, "Action row map_category is 19");
    cJSON_Delete(rows);

    rows = search_rows(&host, "{\"type\":\"map\",\"val\":\"sand\",\"cat\":19}");
    CHECK(cJSON_GetArraySize(rows) == 0, "Action + \"sand\": %d rows (Sandstorm is in no category)", cJSON_GetArraySize(rows));
    cJSON_Delete(rows);

    rows = search_rows(&host, "{\"type\":\"map\",\"val\":\"arena\",\"cat\":19}");
    CHECK(cJSON_GetArraySize(rows) == 1 && !strcmp(row_name(rows, 0), "Arena Blitz"), "Action + \"arena\": %d rows", cJSON_GetArraySize(rows));
    cJSON_Delete(rows);

    rows = search_rows(&host, "{\"type\":\"map\",\"val\":\"\",\"cat\":0}");
    CHECK(cJSON_GetArraySize(rows) == 0, "Other tile: %d rows", cJSON_GetArraySize(rows));
    cJSON_Delete(rows);

    rows = search_rows(&host, "{\"type\":\"map\",\"val\":\"\",\"cat\":-1}");
    CHECK(cJSON_GetArraySize(rows) == 0, "All with no name: %d rows", cJSON_GetArraySize(rows));
    cJSON_Delete(rows);

    rows = search_rows(&host, "{\"type\":\"map\",\"val\":\"blitz\"}");
    CHECK(cJSON_GetArraySize(rows) == 1 && !strcmp(row_name(rows, 0), "Arena Blitz"), "no cat + \"blitz\": %d rows", cJSON_GetArraySize(rows));
    cJSON_Delete(rows);
}

int main(void) {
    test_in_process_maps();
    test_categorised_maps();
    printf("%s: %d checks, %d failures\n", g_failures ? "social_hub_test FAILED" : "Test OK", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
