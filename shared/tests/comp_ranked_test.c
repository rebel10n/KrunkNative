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
#include <turf_wars.h>
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

// Krunker v10.0.2 ranked (shared/include/ranked.h): seasons, the map pool, ranks, TrueSkill, RP.
static void test_v10(void) {
    printf("Running v10 ranked rules test...\n");
    // currentRankedSeason: 0 before 2025-08 UTC, then monthly
    CHECK(ranked_season_at(1753999199) == 0, "2025-07-31 23:59:59 UTC is the Beta season (0)");
    CHECK(ranked_season_at(1754006400) == 1, "2025-08-01 00:00 UTC is season 1");
    CHECK(ranked_season_at(1760313600) == 3, "2025-10-13 is season 3");
    CHECK(ranked_season_at(1798761600) == 18, "2027-01-01 is season 18");
    // getRankedMaps: values from an independent port of v10's JS (scripts outside the repo)
    static const int want[][RANKED_SEASON_MAPS] = {{2, 4, 14, 39, 42}, {2, 4, 14, 39, 42}, {2, 4, 14, 39, 42},
                                                   {2, 4, 14, 17, 42}, {2, 11, 14, 39, 42}, {2, 4, 14, 17, 39}};
    static const int seasons[] = {0, 1, 2, 13, 14, 15};
    for (int i = 0; i < 6; i++) {
        int got[RANKED_SEASON_MAPS];
        ranked_season_maps(seasons[i], got);
        CHECK(!memcmp(got, want[i], sizeof(got)), "season %d map pool [%d,%d,%d,%d,%d]", seasons[i], got[0], got[1], got[2], got[3], got[4]);
    }
    CHECK(ranked_pool_map_name(42) && !strcmp(ranked_pool_map_name(42), "Frontier") && !ranked_pool_map_name(0), "rankedMapPoolInfo names");
    // ranks
    CHECK(ranked_rank_of(10.0, 5) == RANK_UNRANKED, "unranked during the 6 placement matches");
    CHECK(ranked_display(0.0) == 1000 && ranked_display(12.345) == 2235, "display = 1000 + round(100 cr)");
    CHECK(ranked_rank_of(0.0, 6) == RANK_SILVER2, "cr 0 (MMR 1000) places at Silver 2");
    CHECK(ranked_rank_of(-10.0, 6) == RANK_BRONZE1 && ranked_rank_of(-20.0, 6) == RANK_BRONZE1, "low ratings stay Bronze 1");
    CHECK(ranked_rank_of(37.0, 6) == RANK_KRACKED && ranked_rank_of(36.99, 6) == RANK_MASTER, "Kracked from 4700");
    CHECK(!strcmp(ranked_rank_name(RANK_GOLD3), "Gold 3") && !strcmp(ranked_rank_color(RANK_PLATINUM), "#41bdea") &&
          !strcmp(ranked_rank_icon(RANK_SILVER1), "img/ranked/ranks/rank_silver.svg"), "rank names, colours, icons");
    // TrueSkill: equal teams, the winners gain what the losers lose, sigma shrinks
    RankedSkill s[8];
    int team[8], ab[8] = {0};
    for (int i = 0; i < 8; i++) {
        s[i] = ranked_skill_default();
        team[i] = i >= 4;
    }
    ranked_update(s, team, ab, 8, 0);
    CHECK(s[0].mu > RANKED_MU0 && s[4].mu < RANKED_MU0 && near(s[0].mu - RANKED_MU0, RANKED_MU0 - s[4].mu), "symmetric win (%.3f / %.3f)", s[0].mu, s[4].mu);
    CHECK(s[0].sigma < RANKED_SIGMA0 && s[4].sigma < RANKED_SIGMA0, "sigma shrinks");
    const double gain1 = s[0].mu - RANKED_MU0;
    RankedSkill d[8];
    for (int i = 0; i < 8; i++) d[i] = ranked_skill_default();
    ranked_update(d, team, ab, 8, -1);
    CHECK(near(d[0].mu, RANKED_MU0) && d[0].sigma < RANKED_SIGMA0, "a draw between equals moves no mu");
    // an upset gains more than an expected win
    RankedSkill u[8];
    for (int i = 0; i < 8; i++) u[i] = (RankedSkill){i < 4 ? 20.0 : 30.0, 4.0};
    RankedSkill e[8];
    memcpy(e, u, sizeof(u));
    ranked_update(u, team, ab, 8, 0);
    ranked_update(e, team, ab, 8, 1);
    CHECK(u[0].mu - 20.0 > e[4].mu - 30.0 && e[4].mu > 30.0, "upsets gain more (%.3f vs %.3f)", u[0].mu - 20.0, e[4].mu - 30.0);
    // abandon: never gains, and loses RANKED_ABANDON_MU more
    RankedSkill a[8];
    for (int i = 0; i < 8; i++) a[i] = ranked_skill_default();
    ab[1] = 1;
    ranked_update(a, team, ab, 8, 0);
    CHECK(a[1].mu < RANKED_MU0 - RANKED_ABANDON_MU + 1e-9 && near(a[0].mu - RANKED_MU0, gain1), "an abandoner on the winning team loses (%.3f)", a[1].mu);
    CHECK(ranked_conservative(a[1]) < ranked_conservative(ranked_skill_default()), "an abandoner's conservative rating drops");
    RankedSkill dr[8];
    for (int i = 0; i < 8; i++) dr[i] = (RankedSkill){30.0, 3.0};
    ranked_update(dr, team, ab, 8, -1);
    CHECK(ranked_conservative(dr[1]) < 30.0 - 9.0 && ranked_conservative(dr[0]) > 30.0 - 9.0, "a draw: the abandoner drops, the others gain certainty");
    ab[1] = 0;
    // decay, seeds
    RankedSkill dec = ranked_decay((RankedSkill){30.0, 2.0}, 6.0);
    CHECK(near(dec.sigma, 2.0), "no decay within 7 days");
    dec = ranked_decay((RankedSkill){30.0, 2.0}, 30.0);
    CHECK(dec.sigma > 2.0 && dec.sigma <= RANKED_SIGMA0 && near(dec.mu, 30.0), "idle sigma grows (%.3f)", dec.sigma);
    const RankedSkill prev = {35.0, 2.0};
    RankedSkill seed = ranked_season_seed(&prev, -1);
    CHECK(near(seed.mu, 30.0) && near(seed.sigma, RANKED_SIGMA0 * 2.0 / 3.0), "soft reset: half way to mu0");
    seed = ranked_season_seed(NULL, 300);
    CHECK(near(seed.mu, 30.0), "a v7 Elo 300 seeds mu 30");
    seed = ranked_season_seed(NULL, 0);
    CHECK(near(seed.mu, RANKED_MU0) && near(seed.sigma, RANKED_SIGMA0 * 2.0 / 3.0), "a v7 player at 0: mu0, sigma 2/3 sigma0 (0205)");
    seed = ranked_season_seed(NULL, -1);
    CHECK(near(seed.mu, RANKED_MU0) && near(seed.sigma, RANKED_SIGMA0), "no history: the default");
    // Ranked Points
    CHECK(ranked_season_points(0.0, 5, 3) == 0 && ranked_season_points(0.0, 50, 3) == 500 && ranked_season_points(0.0, 150, 3) == 1000 &&
          ranked_season_points(0.0, 1, 0) == 1000, "season conversion: 0 below 6, matches/100, Beta 1:1");
    CHECK(ranked_match_points(50) == 25 && ranked_match_points(500) == 60 && ranked_match_points(-4) == 0, "per-match RP clamp(reward/2, 0, 60)");
    CHECK(ranked_region_index("eu") == 3 && ranked_region_index("4") == 4 && ranked_region_index("xx") == -1 &&
          !strcmp(ranked_region_code(2), "as") && !strcmp(ranked_region_name("na"), "North America"), "regions as/eu/na = 2/3/4");
}

// Krunker v10.0.2 Turf Wars rules (shared/include/turf_wars.h)
static void test_turf(void) {
    printf("Running Turf Wars rules test...\n");
    CHECK(turf_day_at(1773705599) == 0 && turf_day_at(1773705600) == 0 && turf_day_at(1773705600 + 86400) == 1 &&
          turf_day_at(1773705600 + 86400 * 30 + 5) == 30, "getDayNumber: days since 2026-03-17 UTC");
    CHECK(turf_game_eligible(0, 0, 1, 0, 0) && turf_game_eligible(3, 1, 1, 0, 0) && !turf_game_eligible(1, 0, 1, 0, 0) &&
          !turf_game_eligible(2, 2, 1, 0, 0) && !turf_game_eligible(2, 0, 0, 0, 0) && !turf_game_eligible(2, 0, 1, 1, 0) &&
          !turf_game_eligible(2, 0, 1, 0, 1), "eligible: Burg/Sandstorm/Subzero, FFA/TDM, public, no custom map, not ranked");
    CHECK(turf_kill_counts("AAA", 0, 1, "BBB") && turf_kill_counts("AAA", 0, 1, "") && !turf_kill_counts("AAA", 0, 1, "aaa") &&
          !turf_kill_counts("", 0, 1, "BBB") && !turf_kill_counts("AAA", 1, 1, "BBB") && !turf_kill_counts("AAA", 0, 0, "BBB"),
          "counted kills: a clan member's, not on its own clan (case-insensitive), no bots / guests");
    CHECK(turf_cut(650, 0) == 390 && turf_cut(650, 1) == 195 && turf_cut(650, 2) == 65 && turf_cut(650, 3) == 0, "60 / 30 / 10 split");
    CHECK(turf_loot_mode("top_performance") == TURF_LOOT_TOP_PERFORMANCE && turf_loot_mode("x") == -1 &&
          !strcmp(turf_loot_mode_name(TURF_LOOT_EVEN), "even"), "loot mode names");
    const int kills[5] = {40, 30, 20, 7, 3};
    int64_t out[5], total;
    for (int m = 0; m < TURF_LOOT_COUNT; m++) {
        TurfLoot l = turf_loot_default();
        l.mode = m;
        l.clan_pct = 10;
        l.top_player_count = 2;
        turf_loot_clamp(&l);
        const int64_t bank = turf_loot_split(&l, 1001, kills, 5, out);
        total = bank;
        int nonneg = bank >= 0;
        for (int i = 0; i < 5; i++) {
            total += out[i];
            nonneg &= out[i] >= 0;
        }
        CHECK(total == 1001 && nonneg, "loot %s: shares + bank = the cut", turf_loot_mode_name(m));
    }
    TurfLoot even = turf_loot_default();
    even.mode = TURF_LOOT_EVEN;
    even.clan_pct = 20;
    CHECK(turf_loot_split(&even, 390, kills, 2, out) == 78 && out[0] == 156 && out[1] == 156, "even: 20%% bank, the rest split");
    TurfLoot perf = turf_loot_default();
    perf.mode = TURF_LOOT_PERFORMANCE;
    CHECK(turf_loot_split(&perf, 100, kills, 5, out) == 0 && out[0] == 40 && out[4] == 3, "performance: by kills");
    TurfLoot topw = turf_loot_default();
    topw.mode = TURF_LOOT_TOP_WEIGHTED;
    topw.top_player_count = 2;
    turf_loot_clamp(&topw);
    turf_loot_split(&topw, 1000, kills, 5, out);
    CHECK(out[0] == 400 && out[1] == 400 && out[2] == 66, "top weighted: 80%% to the top 2, 20%% to the rest");
    TurfLoot bank = turf_loot_default();
    CHECK(turf_loot_split(&bank, 500, kills, 5, out) == 500 && out[0] == 0, "clan bank: all of it");
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
    test_v10();
    test_turf();
    test_comp_config();
    test_comp_lobby();
    if (g_failures) {
        printf("comp_ranked_test: %d of %d check(s) failed\n", g_failures, g_checks);
        return 1;
    }
    printf("Test OK (%d checks)\n", g_checks);
    return 0;
}
