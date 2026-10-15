// The Host Game flow's server_bin command line (shared/src/host_command.c): exact lines for the
// common settings, the shell quoting (a tokenizer reads every line back to the same words), the
// size limits, and the catalogue keys it writes resolving to their maps.
// Run from the repository root: ./<build>/host_command_test            (checks)
//                               ./<build>/host_command_test --emit    (the lines, one per line:
//                               server/tests/host_command_parse_test.py runs each through server_bin)
#include <host_command.h>
#include <game_config.h>
#include <map_catalog.h>
#include <modes.h>
#include <shared.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;

#define CHECK(cond, ...) do { \
    if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_failures++; } \
} while (0)

// The words of a line as a POSIX shell reads them (the subset the builder writes: bare words,
// 'single quoted' runs, and \' outside the quotes for the quote itself, as in 'it'\''s').
static int split_words(const char *line, char words[][512], const int max) {
    int n = 0;
    const char *p = line;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (n >= max) return -1;
        char *w = words[n];
        size_t len = 0;
        int in_word = 0;
        while (*p && *p != ' ') {
            in_word = 1;
            if (*p == '\'') {
                for (p++; *p && *p != '\''; p++) if (len + 1 < 512) w[len++] = *p;
                if (*p == '\'') p++;
            } else if (*p == '\\' && p[1]) {
                if (len + 1 < 512) w[len++] = p[1];
                p += 2;
            } else {
                if (len + 1 < 512) w[len++] = *p;
                p++;
            }
        }
        w[len] = 0;
        if (in_word) n++;
    }
    return n;
}

static int has_word(char words[][512], const int n, const char *word) {
    for (int i = 0; i < n; i++) if (!strcmp(words[i], word)) return 1;
    return 0;
}

// The request as ui_host_config_collect writes it (every serverConfig row flat, plus the lists)
static cJSON *request_custom(void) {
    cJSON *r = cJSON_CreateObject();
    cJSON *modes = cJSON_AddArrayToObject(r, "modes");
    cJSON_AddItemToArray(modes, cJSON_CreateNumber(mode_find("tdm")));
    cJSON *classes = cJSON_AddArrayToObject(r, "classes");
    cJSON_AddItemToArray(classes, cJSON_CreateNumber(0));
    cJSON_AddItemToArray(classes, cJSON_CreateNumber(1));
    cJSON_AddItemToArray(classes, cJSON_CreateNumber(2));
    cJSON_AddBoolToObject(r, "private", 0);
    cJSON_AddNumberToObject(r, "override", 0);
    cJSON_AddNumberToObject(r, "gameTime", 7);
    cJSON_AddNumberToObject(r, "maxPlayers", 12);
    cJSON_AddBoolToObject(r, "canSlide", 1);      // the default: not written
    cJSON *weps = cJSON_AddObjectToObject(r, "weps");
    cJSON *w3 = cJSON_AddObjectToObject(weps, "3");
    cJSON_AddNumberToObject(w3, "dmg", 50);
    return r;
}

static int build(const HostCommand *spec, const cJSON *request, char *out, const size_t size) {
    return host_command_build(spec, request, out, size);
}

static HostCommand base_spec(void) {
    HostCommand s = {0};
    s.program = NULL;
    s.map_key = "kanji";
    s.host_type = GAME_HOST_CUSTOM;
    s.port = 0;
    return s;
}

static void check_public_default(void) {
    char line[HOST_COMMAND_WORD_MAX];
    const HostCommand s = base_spec();
    const int n = build(&s, NULL, line, sizeof(line));
    CHECK(n > 0, "default line: %d", n);
    CHECK(!strcmp(line, "server_bin --set gameserver.maps=kanji --set gameserver.host_type=custom"), "default line: %s", line);
}

static void check_custom_settings(void) {
    char line[HOST_COMMAND_WORD_MAX];
    HostCommand s = base_spec();
    s.port = 21977;
    s.hostname = "My Krunker server";
    s.password = "it's";
    s.public_host = "203.0.113.7";
    s.public_port = 21977;
    s.headless = 1;
    cJSON *r = request_custom();
    const int n = build(&s, r, line, sizeof(line));
    cJSON_Delete(r);
    CHECK(n > 0, "custom line: %d", n);
    char words[64][512];
    const int count = split_words(line, words, 64);
    CHECK(count > 0, "tokenize: %d", count);
    static const char *const want[] = {
        "server_bin", "--port", "21977", "--hostname", "My Krunker server", "--password", "it's",
        "--public-host", "203.0.113.7", "--public-port", "21977",
        "--set", "gameserver.maps=kanji", "--set", "gameserver.host_type=custom",
        "--mode", "tdm", "--max-players", "12",
        "--set", "gameserver.game.maxPlayers=12", "--set", "gameserver.game.gameTime=7",
        "--set", "gameserver.game.classes=[0,1,2]", "--set", "gameserver.game.weps={\"3\":{\"dmg\":50}}",
        "--headless",
    };
    CHECK(count == (int) (sizeof(want) / sizeof(want[0])), "word count %d: %s", count, line);
    for (size_t i = 0; i < sizeof(want) / sizeof(want[0]) && (int) i < count; i++) {
        CHECK(!strcmp(words[i], want[i]), "word %zu is '%s', want '%s' (line: %s)", i, words[i], want[i], line);
    }
    CHECK(!strstr(line, "canSlide"), "a default row is not written: %s", line);
    CHECK(!strstr(line, "passCode") && !strstr(line, "private"), "the request's own keys stay out: %s", line);
}

static void check_modes_and_restricted(void) {
    char line[HOST_COMMAND_WORD_MAX];
    cJSON *r = cJSON_CreateObject();
    cJSON *modes = cJSON_AddArrayToObject(r, "modes");
    cJSON_AddItemToArray(modes, cJSON_CreateNumber(mode_find("ffa")));
    cJSON_AddItemToArray(modes, cJSON_CreateNumber(mode_find("tdm")));
    HostCommand s = base_spec();
    build(&s, r, line, sizeof(line));
    cJSON_Delete(r);
    CHECK(strstr(line, "--set 'gameserver.modes=[\"ffa\",\"tdm\"]'") != NULL, "two modes: %s", line);
    CHECK(!strstr(line, "--mode "), "two modes are not --mode: %s", line);

    // zombies: no mode / class choice in the Host Game rows, so the line carries the host type only
    cJSON *z = request_custom();
    s.host_type = GAME_HOST_ZOMBIES;
    build(&s, z, line, sizeof(line));
    cJSON_Delete(z);
    CHECK(strstr(line, "gameserver.host_type=zombies") != NULL, "zombies host type: %s", line);
    CHECK(!strstr(line, "--mode") && !strstr(line, "gameserver.game.classes") && !strstr(line, "gameserver.game.weps"),
          "a restricted host type writes no mode / class / weapon choice: %s", line);
    CHECK(strstr(line, "gameserver.game.gameTime=7") != NULL, "a restricted host type keeps the settings rows: %s", line);
}

static void check_positional_map(void) {
    char line[HOST_COMMAND_WORD_MAX];
    HostCommand s = base_spec();
    s.map_key = "kanji";
    s.map_file = "/srv/maps/community one.json";
    build(&s, NULL, line, sizeof(line));
    CHECK(!strncmp(line, "server_bin '/srv/maps/community one.json'", 40), "positional map first, quoted: %s", line);
    CHECK(!strstr(line, "gameserver.maps="), "a positional map is not also a catalogue key: %s", line);
}

static void check_quoting(void) {
    char out[128];
    CHECK(host_command_quote("plain-word_1.2", out, sizeof(out)) == 14 && !strcmp(out, "plain-word_1.2"), "bare word: %s", out);
    CHECK(host_command_quote("two words", out, sizeof(out)) > 0 && !strcmp(out, "'two words'"), "spaces: %s", out);
    CHECK(host_command_quote("it's", out, sizeof(out)) > 0 && !strcmp(out, "'it'\\''s'"), "quote: %s", out);
    CHECK(host_command_quote("", out, sizeof(out)) > 0 && !strcmp(out, "''"), "empty: %s", out);
    CHECK(host_command_quote("a b", out, 4) == HOST_COMMAND_ERR_SIZE, "a small buffer is refused");
}

static void check_limits(void) {
    char line[HOST_COMMAND_WORD_MAX];
    HostCommand s = base_spec();
    CHECK(build(&s, NULL, line, 10) == HOST_COMMAND_ERR_SIZE, "a small output buffer is refused");

    // a weapon override bigger than the server keeps (4096 bytes of game settings)
    cJSON *r = cJSON_CreateObject();
    cJSON *weps = cJSON_AddObjectToObject(r, "weps");
    for (int id = 0; id < 40; id++) {
        char key[8];
        snprintf(key, sizeof(key), "%d", id);
        cJSON *w = cJSON_AddObjectToObject(weps, key);
        for (int k = 0; k < 6; k++) {
            char name[16];
            snprintf(name, sizeof(name), "stat%d", k);
            cJSON_AddNumberToObject(w, name, 123456.789);
        }
    }
    CHECK(build(&s, r, line, sizeof(line)) == HOST_COMMAND_ERR_GAME, "oversized overrides are refused: %s", line);
    cJSON_Delete(r);
}

static void check_catalogue_keys(void) {
    int checked = 0, bad = 0;
    for (size_t i = 0; i < map_catalog_count(); i++) {
        const MapCatalogEntry *e = map_catalog_at(i);
        if (!e || !e->hostable) continue;
        checked++;
        if (map_catalog_find(e->key) != (int) i) {
            bad++;
            printf("  key %s resolves to %d, not %zu\n", e->key, map_catalog_find(e->key), i);
        }
    }
    CHECK(checked > 0, "the catalogue has hostable maps");
    CHECK(bad == 0, "%d hostable catalogue keys do not resolve to their own map", bad);
}

// --emit: the lines the parse test runs through server_bin (no --port: the test adds its own)
static void emit(void) {
    char line[HOST_COMMAND_WORD_MAX];
    const HostCommand plain = base_spec();
    build(&plain, NULL, line, sizeof(line));
    printf("%s\n", line);
    HostCommand custom = base_spec();
    custom.hostname = "Parse test server";
    custom.password = "p ass";
    custom.headless = 1;
    cJSON *r = request_custom();
    build(&custom, r, line, sizeof(line));
    cJSON_Delete(r);
    printf("%s\n", line);
    HostCommand zombies = base_spec();
    zombies.host_type = GAME_HOST_ZOMBIES;
    zombies.map_key = "kanji";
    cJSON *z = request_custom();
    build(&zombies, z, line, sizeof(line));
    cJSON_Delete(z);
    printf("%s\n", line);
}

int main(const int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--emit")) {
        emit();
        return 0;
    }
    check_public_default();
    check_custom_settings();
    check_modes_and_restricted();
    check_positional_map();
    check_quoting();
    check_limits();
    check_catalogue_keys();
    if (g_failures) {
        printf("host_command_test: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("host_command_test: all checks passed\n");
    return 0;
}
