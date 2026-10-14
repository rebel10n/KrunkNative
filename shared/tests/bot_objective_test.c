// Bots play the mode (shared/src/bot_objective.c) on the real Sandstorm (v7 sandstorm_v3: team +
// neutral flags, four objectives, three Domination zones) through the shared game, ticked at
// 30 Hz with normal-difficulty NextBots: CTF roles and captures, Hardpoint holds, Domination
// captures, King of the Hill occupancy, Team Defender carries, Last Man Standing rounds, and the
// e2e's small lobby (three easy bots and an idle human) capturing a flag.
// Run from the repository root: ./<build>/bot_objective_test
#include <shared.h>
#include <bots.h>
#include <bot_objective.h>
#include <map_catalog.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;
static int g_type_count[256];
static int g_captures, g_dom_captures, g_ends, g_kills;
static Game *g_game;

#define CHECK(cond, ...) do { \
    if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_failures++; } \
} while (0)

int mode_player_collides(const Player*, vec3 center, vec3 half); // shared/src/modes/mode_objectives.c

static void test_send(Game *game, const int uid, const int type, const void *payload, const uint16_t length) {
    (void) game;
    (void) uid;
    (void) length;
    if (type >= 0 && type < 256) g_type_count[type]++;
    if (type == NET_PACKET_ACTION) {
        const NetActionPacket *action = payload;
        if (!strcmp(action->text, "captured the Flag")) g_captures++;
    }
    if (type == NET_PACKET_DOM_CAPTURE) g_dom_captures++;
    if (type == NET_PACKET_MODE_END) g_ends++;
    if (type == NET_PACKET_KILL) {
        const NetKillPacket *kill = payload;
        if (kill->killer_uid >= 0 && kill->killer_uid != kill->victim_uid) g_kills++;
    }
}

static void reset_counts(void) {
    memset(g_type_count, 0, sizeof(g_type_count));
    g_captures = g_dom_captures = g_ends = g_kills = 0;
}

static void free_players(Game *game) {
    for (size_t i = 0; i < game->player_count; i++) {
        Player *p = game->players[i];
        free(p->loadout);
        free(p->ammo);
        free(p->reloads);
        free(p->input_queue);
        free(p);
    }
    free(game->players);
    game->players = NULL;
    game->player_count = 0;
}

static double g_now;

static void tick(Game *game, const float seconds, const float hz) {
    const float dt = 1.0f / hz;
    for (float t = 0.0f; t < seconds; t += dt) {
        g_now += dt;
        game_tick(game, (float) g_now, dt);
    }
}

// A fresh round of `mode_index` with `per_team` normal bots on each team (team 0: `per_team` bots).
static void start(Game *game, const int mode_index, const int per_team, const int teams) {
    game_bots_remove_all(game);
    free_players(game);
    game_init(game, 0, mode_index, 1);
    CHECK(game->ready, "game ready");
    for (int team = teams ? 1 : 0; team <= (teams ? 2 : 0); team++) {
        for (int i = 0; i < per_team; i++) {
            char team_arg[16];
            snprintf(team_arg, sizeof(team_arg), "team=%d", team);
            char *argv[] = {team_arg, "difficulty=normal"};
            BotAssignments a;
            bot_parse_assignments(teams ? 2 : 1, teams ? argv : argv + 1, &a);
            CHECK(!a.error[0], "bot options: %s", a.error);
            CHECK(game_bots_add(game, &a, NULL, 0.0f) != NULL, "bot added");
        }
    }
    reset_counts();
    tick(game, 1.0f, 30.0f); // everyone spawned
}

static int bots_with_role(const Game *game, const int team, const BotRole role) {
    int n = 0;
    for (int i = 0; i < game->bots->count; i++) {
        const GameBot *bot = &game->bots->list[i];
        n += bot->player && bot->player->team == team && bot->objective.role == role;
    }
    return n;
}

static void test_ctf(Game *game, const int mode_index) {
    printf("Running CTF test...\n");
    start(game, mode_index, 3, 1);
    CHECK(game->mode->flag_count >= 2, "Sandstorm flags (%d)", game->mode->flag_count);
    tick(game, 3.0f, 30.0f);
    // roles: three a side -> one defender and two attackers per team
    for (int team = 1; team <= 2; team++) {
        CHECK(bots_with_role(game, team, BOT_ROLE_DEFEND) == 1, "team %d: one defender (%d)", team, bots_with_role(game, team, BOT_ROLE_DEFEND));
        CHECK(bots_with_role(game, team, BOT_ROLE_ATTACK) + bots_with_role(game, team, BOT_ROLE_SUPPORT) == 2, "team %d: two attackers", team);
    }
    int states[8] = {0}, pickups = 0, home_runs = 0, picked_in_time = 0;
    float first_capture = 0.0f;
    for (int s = 0; s < 150 * 30 && !first_capture; s++) {
        tick(game, 1.0f / 30.0f, 30.0f);
        for (int i = 0; i < game->bots->count; i++) {
            const GameBot *bot = &game->bots->list[i];
            if (!bot->player || !bot->player->active) continue;
            if (bot->state >= 0 && bot->state < 8) states[bot->state]++;
            if (bot->player->mode.flag) {
                pickups++;
                home_runs += bot->objective.kind == BOT_GOAL_HOME; // a carrier's goal is home
            }
        }
        if (s <= 45 * 30 && pickups) picked_in_time = 1; // a bot carries a flag
        if (g_captures) first_capture = (float) (s + 1) / 30.0f;
    }
    printf("  objective %d hunt %d attack %d hide %d ticks; carried %d ticks (%d toward home), %d kills, capture after %.0f s\n",
           states[BOT_STATE_OBJECTIVE], states[BOT_STATE_HUNT], states[BOT_STATE_ATTACK], states[BOT_STATE_HIDE], pickups, home_runs, g_kills, first_capture);
    CHECK(states[BOT_STATE_OBJECTIVE] > states[BOT_STATE_HUNT], "bots play the objective rather than roam");
    CHECK(picked_in_time, "a flag is picked up within 45 s");
    CHECK(pickups > 0 && home_runs >= pickups * 9 / 10, "carriers head home (%d of %d ticks)", home_runs, pickups);
    CHECK(g_captures >= 1, "a flag is captured within 150 s (team scores %d / %d)", game->mode->team_scores[1], game->mode->team_scores[2]);
}

// Hardpoint: bots stand on the active objective and score; a bot holds it for 5 s straight.
static void test_point(Game *game, const int mode_index) {
    printf("Running Hardpoint test...\n");
    start(game, mode_index, 2, 1);
    CHECK(game->mode->objective_count >= 2, "Sandstorm objectives (%d)", game->mode->objective_count);
    float longest = 0.0f, hold[GAME_BOTS_MAX] = {0};
    int on_point_ticks = 0;
    for (int s = 0; s < 90 * 30; s++) {
        tick(game, 1.0f / 30.0f, 30.0f);
        const ModeObjective *obj = mode_objective_by_uid(game, game->mode->active_objective);
        for (int i = 0; obj && i < game->bots->count; i++) {
            const GameBot *bot = &game->bots->list[i];
            if (bot->player && bot->player->active && mode_player_collides(bot->player, obj->position, obj->half)) {
                hold[i] += 1.0f / 30.0f;
                on_point_ticks++;
            } else {
                hold[i] = 0.0f;
            }
            if (hold[i] > longest) longest = hold[i];
        }
    }
    printf("  team scores %d / %d, bot-ticks on the point %d, longest hold %.1f s\n", game->mode->team_scores[1], game->mode->team_scores[2], on_point_ticks, longest);
    CHECK(game->mode->team_scores[1] + game->mode->team_scores[2] >= 50, "the objective is scored (%d / %d)", game->mode->team_scores[1], game->mode->team_scores[2]);
    CHECK(longest >= 5.0f, "a bot holds the point for 5 s (%.1f)", longest);
}

static void test_dom(Game *game, const int mode_index) {
    printf("Running Domination test...\n");
    start(game, mode_index, 2, 1);
    CHECK(game->mode->dom_zone_count == 3, "Sandstorm zones (%d)", game->mode->dom_zone_count);
    tick(game, 3.0f, 30.0f);
    // the two bots of a team split over different zones
    int zone_of[GAME_BOTS_MAX];
    for (int i = 0; i < game->bots->count; i++) zone_of[i] = game->bots->list[i].objective.kind == BOT_GOAL_ZONE ? game->bots->list[i].objective.index : -1;
    for (int team = 1; team <= 2; team++) {
        int a = -1, b = -1;
        for (int i = 0; i < game->bots->count; i++) {
            if (!game->bots->list[i].player || game->bots->list[i].player->team != team) continue;
            if (a < 0) a = zone_of[i];
            else b = zone_of[i];
        }
        CHECK(a >= 0 && b >= 0 && a != b, "team %d goes for two zones (%d, %d)", team, a, b);
    }
    // until two zones are owned at once (a capture can be a recapture: both teams play the zones)
    int owned = 0;
    for (int s = 0; s < 90 * 30 && owned < 2; s++) {
        tick(game, 1.0f / 30.0f, 30.0f);
        owned = 0;
        for (int z = 0; z < game->mode->dom_zone_count; z++) owned += game->mode->dom_zones[z].captured_by != 0;
    }
    printf("  %d captures, %d zones owned, team scores %d / %d, %d kills\n", g_dom_captures, owned, game->mode->team_scores[1], game->mode->team_scores[2], g_kills);
    CHECK(g_dom_captures >= 2, "zones get captured (%d)", g_dom_captures);
    CHECK(owned >= 2, "zones stay owned (%d)", owned);
}

static void test_king(Game *game, const int mode_index) {
    printf("Running King of the Hill test...\n");
    start(game, mode_index, 3, 0); // FFA
    int occupied = 0, lone = 0;
    for (int s = 0; s < 60 * 30; s++) {
        tick(game, 1.0f / 30.0f, 30.0f);
        const ModeObjective *obj = mode_objective_by_uid(game, game->mode->active_objective);
        if (obj && obj->on_zone >= 1) occupied++;
        if (obj && obj->on_zone == 1) lone++;
    }
    int best = 0;
    for (size_t i = 0; i < game->player_count; i++) best = game->players[i]->mode.objective_score > best ? game->players[i]->mode.objective_score : best;
    printf("  hill occupied %d ticks (%d by one bot), best objective score %d\n", occupied, lone, best);
    CHECK(occupied > 20 * 30, "the hill is held most of the time (%d ticks)", occupied);
    CHECK(best >= 10, "a bot scores the hill (%d)", best);
}

static void test_tdf(Game *game, const int mode_index) {
    printf("Running Team Defender test...\n");
    start(game, mode_index, 2, 1);
    int carried = 0, kite = 0, escort = 0;
    for (int s = 0; s < 60 * 30; s++) {
        tick(game, 1.0f / 30.0f, 30.0f);
        for (int i = 0; i < game->bots->count; i++) {
            const GameBot *bot = &game->bots->list[i];
            if (!bot->player || !bot->player->active) continue;
            carried += bot->player->mode.flag != 0;
            kite += bot->player->mode.flag && bot->objective.kind == BOT_GOAL_KITE;
            escort += bot->objective.kind == BOT_GOAL_ESCORT;
        }
    }
    printf("  flag carried %d bot-ticks (%d kiting), %d escort ticks, team scores %d / %d\n", carried, kite, escort, game->mode->team_scores[1], game->mode->team_scores[2]);
    CHECK(carried > 10 * 30, "the neutral flag gets carried (%d ticks)", carried);
    CHECK(kite >= carried / 2, "a carrier keeps away (%d of %d)", kite, carried);
    CHECK(escort > 0, "teammates escort the carrier");
}

static void test_lms(Game *game, const int mode_index) {
    printf("Running Last Man Standing test...\n");
    start(game, mode_index, 2, 1);
    int hunt = 0;
    for (int s = 0; s < 120 * 30 && !g_ends; s++) {
        tick(game, 1.0f / 30.0f, 30.0f);
        for (int i = 0; i < game->bots->count; i++) hunt += game->bots->list[i].objective.kind == BOT_GOAL_HUNT || game->bots->list[i].objective.kind == BOT_GOAL_REGROUP;
    }
    printf("  %d kills, round ended %d, hunt ticks %d\n", g_kills, g_ends, hunt);
    CHECK(hunt > 0, "bots hunt the remaining enemies");
    CHECK(g_kills >= 1, "elimination kills (%d)", g_kills);
    CHECK(g_ends >= 1, "the round is decided within 120 s");
}

// The e2e's lobby (modes_bots_e2e.py): three easy bots and an idle human, so one team is a lone
// bot beside a player who never moves. No defenders on such small teams, a carrier waiting at
// home for its stolen flag hunts the enemy carrier itself (the both-flags-taken stalemate).
static void test_ctf_small(Game *game) {
    printf("Running CTF easy, 3 bots + an idle human test...\n");
    game_bots_remove_all(game);
    free_players(game);
    game_init(game, 0, 0, 1);
    Player *human = player_init(game);
    human->uid = 900;
    human->class_index = 0;
    human->team = 0;
    snprintf(human->name, sizeof(human->name), "Watcher");
    game_players_add(game, human);
    player_spawn(human);
    char *argv[] = {"difficulty=easy"};
    for (int i = 0; i < 3; i++) {
        BotAssignments a;
        bot_parse_assignments(1, argv, &a);
        CHECK(game_bots_add(game, &a, NULL, 0.0f) != NULL, "bot added");
    }
    reset_counts();
    const double t0 = g_now;
    int defenders = 0;
    for (int s = 0; s < 120 * 30 && !g_captures; s++) {
        tick(game, 1.0f / 30.0f, 30.0f);
        if (!human->active && game_can_spawn(game, human)) player_spawn(human);
        for (int i = 0; i < game->bots->count; i++) defenders += game->bots->list[i].objective.role == BOT_ROLE_DEFEND;
    }
    printf("  captures %d after %.0f s, %d kills, defender ticks %d\n", g_captures, g_now - t0, g_kills, defenders);
    CHECK(defenders == 0, "no defenders on teams of two (%d ticks)", defenders);
    CHECK(g_captures >= 1, "a flag is captured within 120 s");
}

int main(void) {
    const int key = map_catalog_index_of_key("sandstorm_v3");
    const cJSON *sandstorm = key >= 0 ? map_catalog_json((size_t) key) : NULL;
    if (!sandstorm) {
        printf("Failed to load Sandstorm (run from the repository root)\n");
        return 1;
    }
    static const cJSON *maps[1];
    maps[0] = sandstorm;
    static int modes[6];
    modes[0] = mode_find("ctf");
    modes[1] = mode_find("point");
    modes[2] = mode_find("dom");
    modes[3] = mode_find("king");
    modes[4] = mode_find("tdf");
    modes[5] = mode_find("lms");
    Game game = {0};
    g_game = &game;
    game_configure(&game, NULL, maps, 1, modes, 6, NULL, 0, NULL, 0);
    game.hooks.send = test_send;
    game.round_length = 100000.0f;
    game_init(&game, 0, 0, 1);
    if (!game.ready) {
        printf("Failed to init the game\n");
        return 1;
    }
    CHECK(!strcmp(bot_state_name(BOT_STATE_OBJECTIVE), "Objective"), "state name");
    CHECK(!strcmp(bot_goal_name(BOT_GOAL_HOME), "home") && !strcmp(bot_role_name(BOT_ROLE_DEFEND), "defend"), "goal / role names");

    test_ctf(&game, 0);
    test_ctf_small(&game);
    test_point(&game, 1);
    test_dom(&game, 2);
    test_king(&game, 3);
    test_tdf(&game, 4);
    test_lms(&game, 5);

    if (g_failures) {
        printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("Test OK\n");
    return 0;
}
