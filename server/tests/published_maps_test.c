// Published maps as server maps (server/src/published_maps.c): the published:<creator>/<name> spec,
// v10's checks, the disk cache and the fallback to it, and gameserver.maps with published entries
// (gs_build_map_list), the noted map ids. Linked with the standalone brain stub, so every fetch "fails" like a brain
// that cannot be reached: what plays is the disk cache's copy. Run from the repository root:
//   ./<build>/published_maps_test
#include <published_maps.h>
#include <server_config.h>
#include <shared.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// setenv for MinGW, whose CRT has only _putenv_s (the same guard as client/src/headless.c).
static int test_setenv(const char *name, const char *value, const int overwrite) {
#ifdef _WIN32
    if (!overwrite && getenv(name)) return 0;
    return _putenv_s(name, value);
#else
    return setenv(name, value, overwrite);
#endif
}

static int g_checks, g_failures;

#define CHECK(cond, ...) do { \
    g_checks++; \
    if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_failures++; } \
} while (0)

static cJSON *make_map(const char *name, const int spawns) {
    char text[512];
    snprintf(text, sizeof(text), "{\"name\":\"%s\",\"objects\":[{\"p\":[0,0,0],\"s\":[100,2,100]}],\"spawns\":[%s]}", name,
             spawns ? "[0,2,0,0,0,0],[10,2,10,0,0,0]" : "");
    return cJSON_Parse(text);
}

static void test_parse(void) {
    char name[96], creator[64];
    CHECK(published_map_parse("published:alice/Arena", name, sizeof(name), creator, sizeof(creator)) && !strcmp(name, "Arena") &&
          !strcmp(creator, "alice"), "creator/name: %s / %s", creator, name);
    CHECK(published_map_parse("published:Arena", name, sizeof(name), creator, sizeof(creator)) && !strcmp(name, "Arena") && !creator[0],
          "name only");
    CHECK(published_map_parse("PUBLISHED:bob/a/b c", name, sizeof(name), creator, sizeof(creator)) && !strcmp(name, "a/b c") &&
          !strcmp(creator, "bob"), "the first '/' ends the creator: %s / %s", creator, name);
    CHECK(!published_map_parse("Burg", name, sizeof(name), creator, sizeof(creator)), "a catalogue name is not published");
    CHECK(!published_map_parse("published:", name, sizeof(name), creator, sizeof(creator)) &&
          !published_map_parse("published:/Arena", name, sizeof(name), creator, sizeof(creator)) &&
          !published_map_parse("published:alice/", name, sizeof(name), creator, sizeof(creator)), "empty parts are refused");
    char spec[128];
    published_map_format(spec, sizeof(spec), "Arena", "alice");
    CHECK(!strcmp(spec, "published:alice/Arena"), "format: %s", spec);
    published_map_format(spec, sizeof(spec), "Arena", "");
    CHECK(!strcmp(spec, "published:Arena"), "format without creator: %s", spec);
}

static void test_verify(void) {
    cJSON *good = make_map("Arena (2)", 1), *bad = make_map("NoSpawn", 0);
    CHECK(published_map_verify(good) == NULL, "a valid map passes");
    CHECK(strstr(cJSON_GetStringValue(cJSON_GetObjectItem(good, "name")), "\xEF\xBC\x88") != NULL, "prepareGame: '(' became full-width");
    const char *err = published_map_verify(bad);
    CHECK(err && !strcmp(err, "Missing spawn points."), "spawnless: v10's message (%s)", err ? err : "none");
    CHECK(published_map_verify(NULL) != NULL, "no map: refused");
    cJSON_Delete(good);
    cJSON_Delete(bad);
}

static void test_cache_and_load(void) {
    GsConfig cfg;
    gs_config_defaults(&cfg);
    char err[160];
    // nothing cached, no brain: unavailable, with the reason
    CHECK(published_map_load(&cfg, "Arena", "alice", 1000, err, sizeof(err)) == NULL && err[0], "no copy anywhere: NULL (%s)", err);

    cJSON *map = make_map("Arena", 1);
    published_map_verify(map);
    published_map_cache_store("Arena", "alice", map);
    cJSON *back = published_map_cache_read("arena", "ALICE");
    CHECK(back && cJSON_IsArray(cJSON_GetObjectItem(back, "spawns")), "the cache reads back (names case-insensitive)");
    cJSON_Delete(back);
    CHECK(published_map_cache_read("Arena", "bob") == NULL, "another creator's cache entry is a different file");

    const cJSON *loaded = published_map_load(&cfg, "Arena", "alice", 1000, err, sizeof(err));
    CHECK(loaded && !strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(loaded, "name")), "Arena"), "the brain is unreachable: the cached copy plays");
    CHECK(published_map_load(&cfg, "Arena", "alice", 1000, err, sizeof(err)) == loaded && published_map_cached("Arena", "alice") == loaded,
          "one copy per process (memory)");

    // a cached copy that no longer passes the checks is not played
    cJSON *bad = make_map("Broken", 0);
    published_map_cache_store("Broken", "alice", bad);
    CHECK(published_map_load(&cfg, "Broken", "alice", 1000, err, sizeof(err)) == NULL, "a cached spawnless map is not played");
    cJSON_Delete(bad);
    cJSON_Delete(map);

    // gameserver.maps: catalogue + published in order, unavailable ones skipped
    snprintf(cfg.maps[0], sizeof(cfg.maps[0]), "Burg");
    snprintf(cfg.maps[1], sizeof(cfg.maps[1]), "published:alice/Arena");
    snprintf(cfg.maps[2], sizeof(cfg.maps[2]), "published:alice/Missing");
    snprintf(cfg.maps[3], sizeof(cfg.maps[3]), "Kanji");
    cfg.map_count = 4;
    size_t count = 0;
    const cJSON **list = gs_build_map_list(&cfg, &count);
    const char *names[4] = {0};
    for (size_t i = 0; i < count && i < 4; i++) names[i] = cJSON_GetStringValue(cJSON_GetObjectItem(list[i], "name"));
    CHECK(count == 3 && names[1] && !strcmp(names[1], "Arena") && names[0] && names[2] && strcmp(names[0], "Arena") && strcmp(names[2], "Arena"),
          "gameserver.maps: Burg, Arena (published), Kanji; Missing skipped (%zu maps)", count);
    free((void *) list);
}

// The brain's map_id (v10.0.3 customMapData.id, KrunkScript giveSkin's maps): a hosted game's fetch notes it
static void test_map_ids(void) {
    CHECK(published_map_id("Arena") == 0, "the disk cache's copy has no id");
    CHECK(published_map_id("Nowhere") == 0 && published_map_id(NULL) == 0, "an unknown map has none");
    published_map_note_id("Hosted Map", 158102);
    CHECK(published_map_id("hosted map") == 158102, "a noted id (names case-insensitive)");
    published_map_note_id("Arena", 157528);
    CHECK(published_map_id("Arena") == 157528, "a noted id over the loaded copy's");
    published_map_note_id("Hosted Map", 0);
    CHECK(published_map_id("Hosted Map") == 0, "a fetch without an id forgets it");
}

int main(void) {
    char dir[256];
    snprintf(dir, sizeof(dir), "%s/kn-published-maps-test-%d", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (int) getpid());
    test_setenv("XDG_CACHE_HOME", dir, 1);
    test_parse();
    test_verify();
    test_cache_and_load();
    test_map_ids();
    char cmd[300];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
    if (system(cmd)) printf("note: could not remove %s\n", dir);
    printf("published_maps_test: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
