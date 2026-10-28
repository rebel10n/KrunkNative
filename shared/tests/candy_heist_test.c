// candy_heist_test: Krunker v10.0.4 Candy Heist (shared/include/candy_heist.h, the mode in shared/src/modes/modes.c).
//   - the candy count is statForm.candy (floor(score / 10), score || 0) (deob-10.0.4.js:105922-105925)
//   - the mode's config matches the bundle (deob-10.0.4.js:105906-105929): key, clock, class, switches, stats, event
//   - the event's bundled defaults match the bundle's event 10 (deob-10.0.4.js:19486-19503)
// The Hollowmen, door knocks, the Cauldron bank and the Hollowman's Staff counter are map 46's KrunkScript
// (assets/maps/official/46_candy_heist.json), so they are not tested here.
#include <candy_heist.h>
#include <modes.h>
#include <ks.h>
#include <cJSON.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;
#define CHECK(c, ...)                                         \
    do {                                                      \
        if (!(c)) {                                           \
            g_fail++;                                         \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                     \
            fprintf(stderr, "\n");                            \
        }                                                     \
    } while (0)

static void test_candy_count(void) {
    CHECK(candy_heist_candy(0.0) == 0, "0 score is 0 candy");
    CHECK(candy_heist_candy(9.0) == 0, "9 score is 0 candy");
    CHECK(candy_heist_candy(10.0) == 1, "10 score is 1 candy");
    CHECK(candy_heist_candy(99.0) == 9, "99 score is 9 candy");
    CHECK(candy_heist_candy(100.0) == 10, "100 score is 10 candy");
    CHECK(candy_heist_candy(-1.0) == -1, "Math.floor(-0.1) is -1 (%d)", candy_heist_candy(-1.0));
    CHECK(candy_heist_candy(NAN) == 0, "a missing score (NaN) counts 0 (score || 0)");
}

static void test_mode(void) {
    const GameModeConfig *c = mode_config(MODE_ID_CANDY);
    CHECK(c != NULL, "mode_config(MODE_ID_CANDY) exists");
    if (!c) return;
    CHECK(c->id == 37 && MODE_ID_CANDY == 37, "candy is id 37 (%d)", c->id);
    CHECK(!strcmp(c->key, "candy") && !strcmp(c->name, "Candy Heist") && !strcmp(c->alias, "candy"), "key / name / alias");
    CHECK(mode_find("candy") == MODE_ID_CANDY, "mode_find(\"candy\") finds candy");
    CHECK(c->event == 10, "event 10 (deob-10.0.4.js:105906-105929 'event': 0xa)");
    CHECK(c->game_time == 360.0f, "gameTime 360 s (0x57e40 ms), got %.1f", c->game_time);
    CHECK(c->force_class == 9, "forceClass 9, got %d", c->force_class);
    CHECK(c->no_pickups && c->no_scr_limit && c->no_score_xp && c->no_profile_stats && c->no_winner && c->prevent_custom,
          "noPickups / noScrLimit / noScoreXP / noProfileStats / noWinner / preventCustomGames");
    CHECK(c->end_stats && !strcmp(c->end_stats[0], "sid") && !strcmp(c->end_stats[1], "name") && !strcmp(c->end_stats[2], "candy") &&
              !strcmp(c->end_stats[3], "reward") && c->end_stats[4] == NULL,
          "endStats [sid, name, candy, reward]");
    CHECK(c->leader_stats && !strcmp(c->leader_stats[0], "candy") && c->leader_stats[1] == NULL, "leaderStats [candy]");
    CHECK(!mode_hostable(MODE_ID_CANDY), "candy is not hostable as a custom game");

    const CandyHeistMode *m = candy_heist_mode();
    CHECK(!strcmp(m->key, c->key) && !strcmp(m->name, c->name) && m->game_time == c->game_time && m->force_class == c->force_class &&
              m->event == c->event && m->no_winner == c->no_winner,
          "candy_heist_mode() agrees with the mode table");
    CHECK(candy_heist_is_mode("candy") && !candy_heist_is_mode("ffa") && !candy_heist_is_mode(NULL), "candy_heist_is_mode");
}

static void test_event(void) {
    LiveEvent e;
    candy_heist_event_defaults(&e);
    CHECK(e.id == 10 && !strcmp(e.name, "Candy Heist"), "event 10 is Candy Heist");
    CHECK(e.start_ms == 1792368000000.0 && e.end_ms == 1794095940000.0, "Oct 19 to Nov 7 2026 (UTC)");
    CHECK(e.limited_time_mode && e.reward_chance == 5, "limitedTimeMode, rewardChance 5%%");
    CHECK(e.has_reward_skins && e.reward_skin_count == 6, "six reward skins");
    CHECK(e.reward_skins[0] == 0x1f85 && e.reward_skins[5] == 0x1f8b, "reward skins 0x1f85..0x1f8b");
    CHECK(e.has_weights && e.reward_weights[2] == 55 && e.reward_weights[3] == 35 && e.reward_weights[4] == 10 && e.reward_weights[0] == 0,
          "rarity weights [0, 0, 55, 35, 10, 0, 0]");
    CHECK(e.has_eligible_maps && e.eligible_map_count == 1 && e.eligible_maps[0] == 46, "eligible on map 46 only");
}

// Map 46's scripts (assets/maps/official/46_candy_heist.json: the Hollowmen, door knocks, the Cauldron bank and the
// Hollowman's Staff counter) compile with the native KrunkScript front end, client and server sides, as the game loads them.
static void test_map_scripts(void) {
    printf("Running Candy Heist map 46 scripts test...\n");
    const char *path = "assets/maps/official/46_candy_heist.json";
    FILE *f = fopen(path, "rb");
    CHECK(f != NULL, "%s (run from the repository root)", path);
    if (!f) return;
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *text = malloc((size_t) size + 1);
    CHECK(text && fread(text, 1, (size_t) size, f) == (size_t) size, "read %s", path);
    fclose(f);
    if (!text) return;
    text[size] = 0;
    cJSON *root = cJSON_Parse(text);
    free(text);
    const cJSON *scripts = root ? cJSON_GetObjectItemCaseSensitive(root, "scripts") : NULL; // the map JSON itself (fetch_maps.py writes "data")
    CHECK(scripts != NULL, "map 46 has scripts");
    if (!scripts) {
        cJSON_Delete(root);
        return;
    }
    static const struct { const char *key; KsSide side; } sides[] = {{"client", KS_SIDE_CLIENT}, {"server", KS_SIDE_SERVER}};
    for (size_t i = 0; i < sizeof(sides) / sizeof(sides[0]); i++) {
        const cJSON *item = cJSON_GetObjectItemCaseSensitive(scripts, sides[i].key);
        CHECK(cJSON_IsString(item) && item->valuestring && item->valuestring[0], "map 46 %s script", sides[i].key);
        if (!cJSON_IsString(item) || !item->valuestring) continue;
        size_t n = 0;
        char *src = ks_decode_script(item->valuestring, strlen(item->valuestring), &n);
        CHECK(src != NULL && n > 0, "map 46 %s script decodes", sides[i].key);
        if (!src) continue;
        KsDiag err, warnings[32];
        memset(&err, 0, sizeof(err));
        int warning_count = 0;
        const int rc = ks_validate(src, n, sides[i].side, &err, warnings, 32, &warning_count);
        char diag[320] = "";
        if (rc != 0) ks_diag_format(&err, diag, sizeof(diag));
        CHECK(rc == 0, "map 46 %s script: %s", sides[i].key, diag);
        free(src);
    }
    cJSON_Delete(root);
}

int main(void) {
    printf("Running Candy Heist candy count test...\n");
    test_candy_count();
    printf("Running Candy Heist mode test...\n");
    test_mode();
    printf("Running Candy Heist event test...\n");
    test_event();
    test_map_scripts();
    if (g_fail) {
        printf("candy_heist_test: %d failures\n", g_fail);
        return 1;
    }
    printf("candy_heist_test: all checks passed\n");
    return 0;
}
