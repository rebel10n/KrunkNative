// Krunker v10.0.4 counter skins on the server (server/src/skin_counters_server.c): the table from assets/skins.json, an
// account's counter bases from its owned list (m[stat]), countSkinStat on the worn counter of the counted weapon (the
// weapon check, the stat check, trackStat's every-counter loop), takeSkinStats (the rows, the base folded, nothing kept
// when not saved) and the SKINS' counter bases. Run from the repository root (assets/skins.json).
#include <server.h>
#include <server_skin_counters.h>
#include <ruleset.h>
#include <shared.h>
#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures, g_checks;
#define CHECK(cond, ...) do { \
    g_checks++; \
    if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_failures++; } \
} while (0)

// assets/skins.json (v10.0.4): the counter skins
#define VCORE_01 7937  // weapon 19 (weapon id 18), counts kills (no attach.stat)
#define VCORE_03 7939  // a melee skin, counts jumps
#define STAFF 8071     // the Hollowman's Staff, a melee skin, counts candy

static int64_t row_count(const cJSON *rows, const int skin, const char *stat) {
    const cJSON *r;
    cJSON_ArrayForEach(r, rows) {
        const cJSON *s = cJSON_GetObjectItemCaseSensitive(r, "skin"), *st = cJSON_GetObjectItemCaseSensitive(r, "stat");
        if (cJSON_IsNumber(s) && s->valueint == skin && cJSON_IsString(st) && !strcmp(st->valuestring, stat))
            return (int64_t) cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(r, "count"));
    }
    return -1;
}

int main(void) {
    printf("Running the counter skin table...\n");
    CHECK(skin_counters_stat_of(VCORE_01) == skin_stat_index("kills"), "V-CORE 01 counts kills (the default stat)");
    CHECK(skin_counters_stat_of(VCORE_03) == skin_stat_index("jumps"), "V-CORE 03 counts jumps");
    CHECK(skin_counters_stat_of(STAFF) == skin_stat_index("candy"), "the Hollowman's Staff counts candy");
    CHECK(skin_counters_stat_of(0) == -1 && skin_counters_stat_of(-1) == -1, "a plain skin is no counter");

    // a game and one connection with a player
    static Server server;
    g_server = &server;
    static ClientConnection client;
    memset(&client, 0, sizeof(client));
    client.fd = -1;
    client.acct.account_id = 77;
    server.clients = &client;
    server.client_count = 1;
    static const char *const map_text = "{\"name\":\"Flat\",\"spawns\":[[0,0,0,0,0,0]],\"objects\":[{\"p\":[0,-10,0],\"s\":[2000,10,2000]}]}";
    cJSON *map = cJSON_Parse(map_text);
    const cJSON *maps[1] = {map};
    game_configure(&server.game, NULL, maps, 1, NULL, 0, NULL, 0, NULL, 0);
    game_init(&server.game, 0, 0, 1);
    skin_counters_install_game_hooks(&server.game);
    Player *player = player_init(&server.game);
    player->uid = 5;
    client.player = player;
    int melee = -1;
    for (size_t i = 0; i < rules_weapon_count() && melee < 0; i++)
        if (rules_weapons()[i] && rules_weapons()[i]->melee) melee = (int) i;
    CHECK(melee >= 0, "a melee weapon in the table");
    static int loadout[3];
    loadout[0] = 18; // VCORE_01's weapon
    loadout[1] = 1;
    loadout[2] = melee;
    player->loadout = loadout;
    player->loadout_size = 3;
    player->loadout_index = 0;

    printf("Running an account's counter bases...\n");
    // the owned list: V-CORE 01 a stack with no count, then a copy with one; V-CORE 03 a stack whose m is a JSON string; the Staff
    cJSON *skins = cJSON_Parse("[{\"ind\":7937,\"cnt\":2,\"st\":1},{\"ind\":7937,\"cnt\":1,\"st\":0,\"id\":1000002,\"m\":{\"kills\":120}},"
                               "{\"ind\":7939,\"cnt\":1,\"st\":1,\"m\":\"{\\\"jumps\\\":9}\"},{\"ind\":8071,\"cnt\":1,\"st\":1,\"m\":{\"candy\":3}},"
                               "{\"ind\":12,\"cnt\":1}]");
    skin_counters_load(&client, skins);
    CHECK(client.acct.skin_counter_count == 3, "three counter skins owned (%u)", client.acct.skin_counter_count);
    player->skins = (PlayerSkins) {VCORE_01, -1, VCORE_03};
    int32_t base[3];
    skin_counters_bases(&client, &player->skins, base, NULL);
    CHECK(base[0] == 120 && base[1] == -1 && base[2] == 9, "the worn bases: the copy's m.kills, none, the string m's jumps (%d %d %d)",
          base[0], base[1], base[2]);
    int32_t seed[3];
    skin_counters_bases(&client, &player->skins, base, seed);
    CHECK(seed[0] == skin_copy_seed(1000002, 0, 0.0) && seed[0] != 0 && seed[1] == 0 && seed[2] == 0,
          "the worn seeds: V-CORE 01's copy id mixed, none for a stack (%d %d %d)", seed[0], seed[1], seed[2]);
    cJSON *seeded = cJSON_Parse("[{\"ind\":12,\"cnt\":1,\"st\":0,\"id\":5,\"m\":{\"seed\":-1234567.8}}]");
    skin_counters_load(&client, seeded);
    PlayerSkins plain = {12, -1, -1};
    skin_counters_bases(&client, &plain, base, seed);
    CHECK(seed[0] == 765432, "a meta seed wins over the id: (floor(-1234567.8) %% 1e6 + 1e6) %% 1e6 = 765432 (%d)", seed[0]);
    cJSON_Delete(seeded);
    skin_counters_load(&client, skins); // back to the owned list above

    printf("Running countSkinStat on the server...\n");
    game_count_skin_stat(&server.game, player, 18, "kills", 1);
    game_count_skin_stat(&server.game, player, 18, "kills", 1);
    game_count_skin_stat(&server.game, player, 18, "hits", 4);     // not V-CORE 01's stat
    game_count_skin_stat(&server.game, player, 1, "kills", 1);     // another weapon: no counter there
    game_count_skin_stat(&server.game, player, melee, "jumps", 2); // the melee slot's V-CORE 03
    game_count_skin_stat(&server.game, player, 18, "jumps", 5);    // jumps on the gun: not the melee's counter
    cJSON *rows = skin_counters_take(&client, 1);
    CHECK(row_count(rows, VCORE_01, "kills") == 2, "V-CORE 01: two kills (%lld)", (long long) row_count(rows, VCORE_01, "kills"));
    CHECK(row_count(rows, VCORE_03, "jumps") == 2, "V-CORE 03: the melee's two jumps (%lld)", (long long) row_count(rows, VCORE_03, "jumps"));
    CHECK(cJSON_GetArraySize(rows) == 2, "nothing else counted (%d rows)", cJSON_GetArraySize(rows));
    cJSON_Delete(rows);
    skin_counters_bases(&client, &player->skins, base, NULL);
    CHECK(base[0] == 122 && base[2] == 11, "the take folded the counts into the bases (%d %d)", base[0], base[2]);
    rows = skin_counters_take(&client, 1);
    CHECK(rows == NULL, "a second take has nothing");
    cJSON_Delete(rows);

    printf("Running trackStat's candy loop...\n");
    player->skins = (PlayerSkins) {VCORE_01, -1, STAFF};
    game_count_skin_stat(&server.game, player, -1, "candy", 7); // every worn counter of the stat
    game_count_skin_stat(&server.game, player, -1, "kills", 3); // -1 with kills: V-CORE 01 too
    rows = skin_counters_take(&client, 1);
    CHECK(row_count(rows, STAFF, "candy") == 7, "the Staff banks 7 candy (%lld)", (long long) row_count(rows, STAFF, "candy"));
    CHECK(row_count(rows, VCORE_01, "kills") == 3, "weapon -1 reaches every worn counter of the stat");
    cJSON_Delete(rows);

    printf("Running a take that is not saved...\n");
    game_count_skin_stat(&server.game, player, 18, "kills", 4);
    rows = skin_counters_take(&client, 0);
    CHECK(rows == NULL, "not saved: no rows");
    skin_counters_bases(&client, &player->skins, base, NULL);
    CHECK(base[0] == 125, "not saved: the base keeps its value (%d)", base[0]);
    rows = skin_counters_take(&client, 1);
    CHECK(rows == NULL, "not saved: the counts are dropped (skinStats = {})");
    cJSON_Delete(rows);

    printf("Running a guest...\n");
    client.acct.account_id = 0;
    game_count_skin_stat(&server.game, player, 18, "kills", 1);
    client.acct.account_id = 77;
    rows = skin_counters_take(&client, 1);
    CHECK(rows == NULL, "a guest counts nothing");
    cJSON_Delete(rows);

    cJSON_Delete(skins);
    cJSON_Delete(map);
    printf("skin_counters_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
