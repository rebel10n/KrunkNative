// community_maps_test: the data rules of the Games window's community maps and the Host Game map
// pick (client/src/ui/community_maps_core.c): the Search tab's /api/search request (v10 searchMap(val,
// cat) plus KrunkNative's source split), the mode tags, the catalogue match of a row and the map slot.
#include <community_maps.h>
#include <map_catalog.h>
#include <modes.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks, g_fails;

#define CHECK(cond, ...)                                                                 \
    do {                                                                                 \
        g_checks++;                                                                      \
        if (!(cond)) {                                                                   \
            g_fails++;                                                                   \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                         \
            fprintf(stderr, __VA_ARGS__);                                                \
            fputc('\n', stderr);                                                         \
        }                                                                                \
    } while (0)

static void test_search_path(void) {
    char path[256];
    size_t n = community_search_path(path, sizeof(path), "Cm 1&x", 1, COMMUNITY_SOURCE_COMMUNITY);
    CHECK(n && !strcmp(path, "/api/search?type=map&val=Cm%201%26x&cat=1&source=community"), "encoded query, cat, source: %s", path);
    community_search_path(path, sizeof(path), "", 7, COMMUNITY_SOURCE_ALL);
    CHECK(!strcmp(path, "/api/search?type=map&val=&cat=7"), "category only (Popular Categories): %s", path);
    community_search_path(path, sizeof(path), "burg", -1, COMMUNITY_SOURCE_OFFICIAL);
    CHECK(!strcmp(path, "/api/search?type=map&val=burg&source=official"), "All categories, official: %s", path);
    community_search_path(path, sizeof(path), "x", MAP_CATEGORY_COUNT, 99);
    CHECK(!strcmp(path, "/api/search?type=map&val=x"), "an unknown category / source is left out: %s", path);
    char small[24];
    CHECK(community_search_path(small, sizeof(small), "a long query", 1, 0) == 0 && small[0] == 0, "a path that does not fit: 0, empty");
    CHECK(community_source_of("official") == COMMUNITY_SOURCE_OFFICIAL && community_source_of("community") == COMMUNITY_SOURCE_COMMUNITY &&
          community_source_of("") == COMMUNITY_SOURCE_ALL && community_source_of(NULL) == COMMUNITY_SOURCE_ALL, "source select values");
}

static void test_mode_names(void) {
    char out[128], want[128];
    cJSON *modes = cJSON_Parse("[0, 1]");
    snprintf(want, sizeof(want), "%s, %s", mode_name(0), mode_name(1));
    community_mode_names(out, sizeof(out), modes);
    CHECK(!strcmp(out, want), "modes [0, 1]: \"%s\" (want \"%s\")", out, want);
    cJSON_Delete(modes);
    modes = cJSON_Parse("[0, \"x\", 9999]");
    community_mode_names(out, sizeof(out), modes);
    CHECK(!strcmp(out, mode_name(0)), "non-numbers and unknown ids are skipped: \"%s\"", out);
    cJSON_Delete(modes);
    CHECK(community_mode_names(out, sizeof(out), NULL) == 0 && out[0] == 0, "no modes: empty");
    modes = cJSON_Parse("[0, 1, 2]");
    char tiny[8];
    community_mode_names(tiny, sizeof(tiny), modes);
    CHECK(strlen(tiny) < sizeof(tiny), "a short buffer stays terminated: \"%s\"", tiny);
    cJSON_Delete(modes);
}

static void test_catalogue_index(void) {
    const int burg = map_catalog_find("Burg");
    CHECK(burg >= 0, "Burg is in the catalogue");
    CHECK(community_catalogue_index("Burg", "Krunker") == burg, "an official row names the official map");
    CHECK(community_catalogue_index("Burg", NULL) == burg, "a bare name prefers the official map");
    CHECK(community_catalogue_index("Bur", NULL) == burg, "a bare name matches like cmd_map (prefix)");
    CHECK(community_catalogue_index("Bur", "alice") == -1, "a published row's name is exact: Bur is not Burg");
    CHECK(community_catalogue_index("Cm_nope_12345", "alice") == -1 && community_catalogue_index("", NULL) == -1,
          "a published map is not in the catalogue");
    // a bundled community map by its creator
    for (size_t i = 0; i < map_catalog_count(); i++) {
        const MapCatalogEntry *e = map_catalog_at(i);
        if (e->source != MAP_SOURCE_CUSTOM || !e->creator || !e->creator[0] || !strcmp(e->creator, "Krunker")) continue;
        const int found = community_catalogue_index(e->name, e->creator);
        CHECK(found >= 0 && !strcmp(map_catalog_at((size_t) found)->name, e->name), "community row %s by %s", e->name, e->creator);
        break;
    }
}

static void test_slot_of(void) {
    const cJSON **maps = NULL;
    size_t count = 0;
    cJSON *a = cJSON_CreateObject(), *b = cJSON_CreateObject();
    CHECK(community_slot_of(&maps, &count, a) == 0 && count == 1, "first map: slot 0");
    CHECK(community_slot_of(&maps, &count, b) == 1 && count == 2, "second map: slot 1");
    CHECK(community_slot_of(&maps, &count, a) == 0 && count == 2, "a map already in the list keeps its slot");
    CHECK(community_slot_of(&maps, &count, NULL) == -1, "no map: -1");
    free((void *) maps);
    cJSON_Delete(a);
    cJSON_Delete(b);
}

int main(void) {
    test_search_path();
    test_mode_names();
    test_catalogue_index();
    test_slot_of();
    printf("community_maps_test: %d checks, %d failed\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
