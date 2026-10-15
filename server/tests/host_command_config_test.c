// The Host Game flow's settings reach the server's game (server/src/config.c): `--set
// gameserver.game.<var>=<value>` (gs_config_set_cli) lands in the game object, merges with the
// server.cfg file's own game keys, and the class / weapon / effect overrides stay in the game
// request the sandbox and hosted games apply. Run from the repository root:
//   ./<build>/host_command_config_test
#include <server_config.h>
#include <game_config.h>
#include <shared.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;

#define CHECK(cond, ...) do { \
    if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_failures++; } \
} while (0)

#define CFG_FILE "host_command_config_test.json"

static void write_file(const char *text) {
    FILE *f = fopen(CFG_FILE, "w");
    if (!f) return;
    fputs(text, f);
    fclose(f);
}

int main(void) {
    // 1. --set gameserver.game.<var> with no file: the game object, its typed keys and the overrides
    write_file("{}");
    GsConfig cfg;
    gs_config_defaults(&cfg);
    char err[256] = "";
    CHECK(gs_config_set_cli("game.gameTime", "7") == 0, "set cli gameTime");
    CHECK(gs_config_set_cli("game.maxPlayers", "12") == 0, "set cli maxPlayers");
    CHECK(gs_config_set_cli("game.classes", "[0,1,2]") == 0, "set cli classes");
    CHECK(gs_config_set_cli("game.weps", "{\"3\":{\"dmg\":50}}") == 0, "set cli weps");
    CHECK(gs_config_set_cli("maps", "kanji") == 0, "set cli maps");
    CHECK(gs_config_load(&cfg, CFG_FILE, 1, err, sizeof(err)) == 0, "load: %s", err);
    CHECK(cfg.has_game, "the game object is applied");
    CHECK(cfg.game.game_time == 7, "gameTime 7 reaches the game (got %g)", (double) cfg.game.game_time);
    CHECK(cfg.game.max_players == 12, "maxPlayers 12 reaches the game (got %d)", cfg.game.max_players);
    CHECK(strstr(cfg.game_request, "\"gameTime\":7") != NULL, "gameTime in the game request: %s", cfg.game_request);
    CHECK(strstr(cfg.game_request, "\"classes\":[0,1,2]") != NULL, "classes in the game request: %s", cfg.game_request);
    CHECK(strstr(cfg.game_request, "\"weps\":{\"3\":{\"dmg\":50}}") != NULL, "weps in the game request: %s", cfg.game_request);
    CHECK(cfg.map_count == 1 && !strcmp(cfg.maps[0], "kanji"), "--set gameserver.maps still a plain key");

    // 2. the file's game keys stay when a --set game.<var> is given
    write_file("{\"gameserver\":{\"game\":{\"canSlide\":false,\"gameTime\":3}}}");
    GsConfig merged;
    gs_config_defaults(&merged);
    memset(err, 0, sizeof(err));
    // the same process: the CLI section already holds gameTime 7, maxPlayers 12, classes, weps (case 1)
    CHECK(gs_config_load(&merged, CFG_FILE, 1, err, sizeof(err)) == 0, "load with file: %s", err);
    CHECK(strstr(merged.game_request, "\"canSlide\":false") != NULL, "the file's canSlide survives the CLI game keys: %s", merged.game_request);
    CHECK(merged.game.game_time == 7, "CLI gameTime wins over the file's 3 (got %g)", (double) merged.game.game_time);

    remove(CFG_FILE);
    if (g_failures) {
        printf("host_command_config_test: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("host_command_config_test: all checks passed\n");
    return 0;
}
