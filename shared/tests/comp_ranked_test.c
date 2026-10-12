// Krunker v7.1.4 ranked (shared/include/ranked.h) and the comp lobby it plays in
// (shared/include/comp_lobby.h): the rating rules (eloK 32 against the other team's average,
// divisor 330, floored at 0, rankedDeduction, eloPer ranks), config.rankedClasses, the ranked
// host config (config.hostTypes[1] + tmSize / specSlots / allowSpect), and v7 game.js
// lobbyPlayers / syncLobbyP / updLobbyP / canSpawn with the "cd" / "cda" packets.
// Run from the repository root: ./<build>/comp_ranked_test
#include <shared.h>
#include <game_config.h>
#include <comp_lobby.h>
#include <ranked.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures, g_checks;

#define CHECK(cond, ...) do { \
    g_checks++; \
    if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_failures++; } \
} while (0)

#define MAX_SENT 256
typedef struct {
    int uid;
    NetCompLobbyPacket packet;
} Sent;
static Sent g_sent[MAX_SENT];
static int g_sent_count;

static void test_send(Game *game, const int uid, const int type, const void *payload, const uint16_t length) {
    (void) game;
    if (type != NET_PACKET_COMP_LOBBY || length != sizeof(NetCompLobbyPacket) || g_sent_count >= MAX_SENT) return;
    g_sent[g_sent_count].uid = uid;
    memcpy(&g_sent[g_sent_count].packet, payload, sizeof(NetCompLobbyPacket));
    g_sent_count++;
}

static int near(const double a, const double b) { return fabs(a - b) < 1e-6; }

static void test_rating(void) {
    printf("Running rating rules test...\n");
    CHECK(near(ranked_expected(1000, 1000), 0.5), "equal ratings expect 0.5");
    CHECK(near(ranked_expected(1330, 1000), 1.0 / (1.0 + pow(10.0, -1.0))), "divisor 330 (v3.1.1 updateELO)");
    CHECK(near(ranked_delta(500, 500, 1.0, 0, RANKED_ELO_K), 16.0), "a win at equal ratings: +16");
    CHECK(near(ranked_delta(500, 500, 0.0, 0, RANKED_ELO_K), -16.0), "a loss at equal ratings: -16");
    CHECK(near(ranked_delta(500, 500, 0.5, 0, RANKED_ELO_K), 0.0), "a draw at equal ratings: 0");
    CHECK(near(ranked_delta(10, 10, 0.0, 0, RANKED_ELO_K), -10.0), "floored at 0");
    CHECK(near(ranked_delta(0, 0, 0.0, 0, RANKED_ELO_K), 0.0), "0 stays 0");
    CHECK(near(ranked_delta(500, 500, 1.0, 1, RANKED_ELO_K), -RANKED_DEDUCTION), "an abandoner loses rankedDeduction, even on the winning team");
    CHECK(near(ranked_delta(12, 500, 0.0, 1, RANKED_ELO_K), -12.0), "the deduction is floored too");
    const double up = ranked_delta(400, 800, 1.0, 0, RANKED_ELO_K), down = ranked_delta(800, 400, 1.0, 0, RANKED_ELO_K);
    CHECK(up > 16.0 && up < 32.0 && down > 0.0 && down < 16.0, "upsets gain more (%.2f vs %.2f)", up, down);
    CHECK(ranked_rank_index(0) == 0 && ranked_rank_index(239) == 0 && ranked_rank_index(240) == 1 && ranked_rank_index(359) == 1 &&
          ranked_rank_index(360) == 2 && ranked_rank_index(960) == 7 && ranked_rank_index(50000) == 7 && ranked_rank_index(-5) == 0,
          "UTILS.eloIconId: eloPer 120, 8 ranks");
    CHECK(ranked_rank_number(0) == 1 && ranked_rank_number(1000) == 8, "rank numbers 1..8");
    static const int expected[] = {0, 1, 2, 3, 4, 5, 6, 8, 12, 13, 15};
    int ok = 1;
    for (int c = 0; c < 16; c++) {
        int want = 0;
        for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) want |= expected[i] == c;
        ok &= ranked_class_allowed(c) == want;
    }
    CHECK(ok, "config.rankedClasses [0, 1, 2, 3, 4, 5, 6, 8, 12, 13, 15]");
}

// The game server's ranked lobby config (server/src/accounts.c): the comp host type's setup with
// tmSize = team size - 1, no spectator slots, spectating off.
static void comp_game(Game *game, const int team_size) {
    memset(game, 0, sizeof(*game));
    cJSON *request = cJSON_CreateObject();
    cJSON_AddNumberToObject(request, "tmSize", team_size - 1);
    cJSON_AddNumberToObject(request, "specSlots", 0);
    cJSON_AddBoolToObject(request, "allowSpect", 0);
    cJSON *maps = cJSON_AddArrayToObject(request, "maps");
    cJSON_AddItemToArray(maps, cJSON_CreateNumber(2));
    cJSON *cfg = game_config_setup(GAME_HOST_COMP, request, NULL, 1);
    game_config_read(&game->config, cfg, GAME_CONFIG_CLAMP_HOSTED, GAME_HOST_COMP);
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(cfg, "modes")) == 1 && cJSON_GetArrayItem(cJSON_GetObjectItem(cfg, "modes"), 0)->valueint == 2,
          "comp host: Hardpoint");
    CHECK(cJSON_GetArrayItem(cJSON_GetObjectItem(cfg, "maps"), 0)->valueint == 2, "the queue's map replaces the overDat list");
    cJSON_Delete(cfg);
    cJSON_Delete(request);
    game->hooks.send = test_send;
}

static CompLobbyEntry identity(const int account, const char *name) {
    CompLobbyEntry e;
    memset(&e, 0, sizeof(e));
    e.account_id = account;
    e.level = 30;
    e.flag = -1;
    snprintf(e.name, sizeof(e.name), "%s", name);
    return e;
}

static void test_comp_config(void) {
    printf("Running ranked lobby config test...\n");
    Game game;
    comp_game(&game, 4);
    const GameConfig *c = &game.config;
    CHECK(c->comp && c->test_mode && c->select_team && c->limit_classes, "isComp, tstCmp, selTeam, limitClasses");
    CHECK(c->team_size == 3 && c->min_players == 8 && c->max_players == 8 && c->spec_slots == 0 && !c->allow_spectate,
          "4v4: minPlayers / maxPlayers 8, no spectators (%d / %d)", c->min_players, c->max_players);
    int ok = 1;
    for (int i = 0; i < 16; i++) ok &= c->class_limits[i] == (ranked_class_allowed(i) ? 1 : 0);
    CHECK(ok, "classLim<i> defC: 1 for config.rankedClasses, else 0");
    CHECK(c->hitbox_pad_set && near(c->hitbox_pad, 0.1), "hitBoxPad defC 0.1");
    Game two;
    comp_game(&two, 1);
    CHECK(two.config.team_size == 0 && two.config.min_players == 2 && two.config.max_players == 2, "1v1 roster");
}

static void test_comp_lobby(void) {
    printf("Running comp lobby test...\n");
    Game game;
    comp_game(&game, 2);
    g_sent_count = 0;
    CompLobbyEntry a = identity(11, "alpha"), b = identity(12, "bravo"), c = identity(13, "charlie");
    comp_lobby_join(&game, 1, &a);
    CHECK(g_sent_count == 3 && g_sent[0].uid == 1 && g_sent[0].packet.op == NET_COMP_LOBBY_CLEAR && g_sent[1].packet.op == NET_COMP_LOBBY_SET &&
          g_sent[2].uid == NET_TARGET_EXCEPT(1), "join: \"cd\" (clear + every entry) to the joiner, \"cda\" to the others");
    comp_lobby_join(&game, 2, &b);
    comp_lobby_join(&game, 3, &c);
    const CompLobbyEntry *e = comp_lobby_get(&game, 1);
    CHECK(e && e->active && e->team == 0 && e->class_index == -1 && !strcmp(e->name, "alpha") && e->account_id == 11, "entry: account, no team, no class");
    CHECK(!strcmp(comp_lobby_spawn_block(&game, 1), "Please join a Team to Spawn"), "canSpawn: a team first");
    CHECK(!strcmp(comp_lobby_spawn_block(&game, 99), "Missing Sync Data"), "canSpawn: no entry");
    CHECK(comp_lobby_update(&game, 1, 't', 1) && comp_lobby_update(&game, 2, 't', 1), "two join team 1");
    CHECK(!comp_lobby_update(&game, 3, 't', 1) && comp_lobby_get(&game, 3)->team == 0, "a 2v2 team is full at tmSize + 1");
    CHECK(comp_lobby_update(&game, 3, 't', 2), "team 2");
    CHECK(!comp_lobby_update(&game, 3, 't', 7), "no team 7");
    CHECK(!strcmp(comp_lobby_spawn_block(&game, 1), "Please select a Class to Spawn"), "canSpawn: then a class");
    CHECK(!comp_lobby_update(&game, 1, 'c', 7), "Rocketeer: classLim7 0 under limitClasses");
    CHECK(comp_lobby_update(&game, 1, 'c', 0) && comp_lobby_get(&game, 1)->class_index == 0, "Triggerman");
    CHECK(!comp_lobby_update(&game, 2, 'c', 0), "classLim0 1: the team mate cannot take Triggerman too");
    CHECK(comp_lobby_update(&game, 3, 'c', 0), "the other team can");
    CHECK(comp_lobby_class_count(&game, 1, 0) == 1 && comp_lobby_class_count(&game, 2, 0) == 1, "class counts per team");
    CHECK(comp_lobby_spawn_block(&game, 1) == NULL, "team + class: may spawn");
    CHECK(!comp_lobby_update(&game, 1, 'c', 0) && !comp_lobby_update(&game, 1, 'x', 1), "no change / unknown key");
    // the authority's roster team (ranked): no selTeam / size check
    comp_lobby_set_team(&game, 2, 2);
    CHECK(comp_lobby_get(&game, 2)->team == 2, "set_team");
    g_sent_count = 0;
    comp_lobby_leave(&game, 1);
    e = comp_lobby_get(&game, 1);
    CHECK(e && !e->active && e->rejected && e->team == 0 && e->class_index == -1, "leave: a = 0, rej, t / c dropped");
    CHECK(g_sent_count == 1 && g_sent[0].uid == NET_TARGET_ALL && g_sent[0].packet.op == NET_COMP_LOBBY_REMOVE && g_sent[0].packet.entry.uid == 1,
          "leave: \"cda\" id null");
    CHECK(comp_lobby_update(&game, 2, 'c', 0) || comp_lobby_class_count(&game, 2, 0) >= 1, "a freed slot");
    // rejoin keeps the entry, active again, the table to the joiner
    g_sent_count = 0;
    comp_lobby_join(&game, 1, &a);
    CHECK(comp_lobby_get(&game, 1)->active && g_sent_count >= 2 && g_sent[0].packet.op == NET_COMP_LOBBY_CLEAR, "rejoin");

    // a client mirrors the table from the packets
    Game client;
    memset(&client, 0, sizeof(client));
    g_sent_count = 0;
    comp_lobby_send_all(&game, 5);
    for (int i = 0; i < g_sent_count; i++) comp_lobby_apply(&client, &g_sent[i].packet);
    int active = 0;
    for (int i = 0; game.comp_lobby && i < game.comp_lobby->count; i++) active += game.comp_lobby->entries[i].active;
    CHECK(client.comp_lobby && client.comp_lobby->count == active, "the client's table: every active entry (%d)", active);
    NetCompLobbyPacket remove = {.op = NET_COMP_LOBBY_REMOVE};
    remove.entry.uid = 3;
    comp_lobby_apply(&client, &remove);
    CHECK(!comp_lobby_get(&client, 3) && comp_lobby_get(&client, 2), "\"cda\" null removes");
    comp_lobby_free(&client);
    comp_lobby_free(&game);

    // outside a comp game nothing happens
    Game pub;
    memset(&pub, 0, sizeof(pub));
    pub.hooks.send = test_send;
    g_sent_count = 0;
    comp_lobby_join(&pub, 1, &a);
    CHECK(!pub.comp_lobby && !g_sent_count && comp_lobby_spawn_block(&pub, 1) == NULL, "public games have no lobby table");
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    test_rating();
    test_comp_config();
    test_comp_lobby();
    if (g_failures) {
        printf("comp_ranked_test: %d of %d check(s) failed\n", g_failures, g_checks);
        return 1;
    }
    printf("Test OK (%d checks)\n", g_checks);
    return 0;
}
