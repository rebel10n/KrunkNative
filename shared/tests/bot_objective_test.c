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
#include <pcg_basic.h>
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
    // 240 s: a capture usually comes in 25-50 s (KN_BOT_SEED 1-6), but a round where both sides keep
    // killing the carriers takes longer (the default stream: 39 kills, no capture by 150 s since the
    // bots see through the objective volumes as v7 does; main itself captured at 131 s)
    for (int s = 0; s < 240 * 30 && !first_capture; s++) {
        tick(game, 1.0f / 30.0f, 30.0f);
        for (int i = 0; i < game->bots->count; i++) {
            const GameBot *bot = &game->bots->list[i];
            if (!bot->player || !bot->player->active) continue;
            if (bot->state >= 0 && bot->state < 8) states[bot->state]++;
            if (bot->player->mode.flag) {
                pickups++;
                // a carrier's goal is home; or, in a both-flags-taken stalemate, the enemy carrier
                // (bot_objective.c OBJ_STALEMATE_MS), or our own dropped flag on the way (a return)
                const BotObjective *o = &bot->objective;
                home_runs += o->kind == BOT_GOAL_HOME || (o->kind == BOT_GOAL_CARRIER && o->stalemate_ms > 0.0f) || o->kind == BOT_GOAL_FLAG;
            }
        }
        if (s <= 45 * 30 && pickups) picked_in_time = 1; // a bot carries a flag
        if (g_captures) first_capture = (float) (s + 1) / 30.0f;
    }
    printf("  objective %d hunt %d attack %d hide %d ticks; carried %d ticks (%d toward home), %d kills, capture after %.0f s\n",
           states[BOT_STATE_OBJECTIVE], states[BOT_STATE_HUNT], states[BOT_STATE_ATTACK], states[BOT_STATE_HIDE], pickups, home_runs, g_kills, first_capture);
    CHECK(states[BOT_STATE_OBJECTIVE] > states[BOT_STATE_HUNT], "bots play the objective rather than roam");
    CHECK(picked_in_time, "a flag is picked up within 45 s");
    CHECK(pickups > 0 && home_runs >= pickups * 9 / 10, "carriers head home, or for the enemy carrier in a stalemate (%d of %d ticks)", home_runs, pickups);
    CHECK(g_captures >= 1, "a flag is captured within 240 s (team scores %d / %d)", game->mode->team_scores[1], game->mode->team_scores[2]);
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

// An objective with a fight: a bot on its way to a zone (ZONE, not arrived) that a player hit recently.
// The fight comes first while the attacker is in fight range, even when the bot has stalled; out of
// range, or with no hit on it, the objective stands (the old rule, unchanged).
static void test_under_fire(Game *game, const int mode_index) {
    printf("Running objective under fire test...\n");
    start(game, mode_index, 1, 1);
    GameBot *bot = NULL;
    Player *enemy = NULL;
    for (int i = 0; i < game->bots->count; i++) {
        if (!game->bots->list[i].player) continue;
        if (!bot && game->bots->list[i].player->team == 1) bot = &game->bots->list[i];
        else if (!enemy && game->bots->list[i].player->team == 2) enemy = game->bots->list[i].player;
    }
    CHECK(bot && enemy, "a bot and an enemy in the lobby");
    if (!bot || !enemy) return;
    Player *self = bot->player;
    BotObjective *o = &bot->objective;
    o->kind = BOT_GOAL_ZONE;
    o->goal = (vec3) {self->position.x + 600.0f, self->position.y, self->position.z};
    o->last_flag = self->mode.flag;
    o->last_tags = self->mode.dog_tags;
    o->think_timer = 1e6f; // no re-think: the goal stays
    o->fail_timer = 0.0f;
    o->arrived = 0;
    const float dt = 1.0f / 30.0f;
    const int stalled_ms = 6000 + 1; // OBJ_STALL_MS (bot_objective.c)
    enemy->position = (vec3) {self->position.x + 60.0f, self->position.y, self->position.z};
    vec3 goal;

    bot_sense_reset(&bot->sense);
    bot_sense_seed(&bot->sense, enemy, enemy->position, BOT_STIMULUS_HIT);
    bot->state_time = (float) stalled_ms;
    CHECK(bot_objective_over_combat(game, bot, self, enemy, dt, &goal) == 0, "under fire in range, stalled: the fight, not the objective");
    bot_sense_reset(&bot->sense);
    CHECK(bot_objective_over_combat(game, bot, self, enemy, dt, &goal) == 1, "no hit, stalled: the objective (the old rule)");
    bot_sense_reset(&bot->sense);
    bot_sense_seed(&bot->sense, enemy, enemy->position, BOT_STIMULUS_HIT);
    bot->state_time = 0.0f;
    CHECK(bot_objective_over_combat(game, bot, self, enemy, dt, &goal) == 0, "under fire in range: the fight");
    enemy->position = (vec3) {self->position.x + 400.0f, self->position.y, self->position.z};
    CHECK(bot_objective_over_combat(game, bot, self, enemy, dt, &goal) == 1, "under fire, attacker out of range: the objective");
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


// "these bots just walk and get stuck at the zone" (the user). v10's bots contest a zone: they move
// inside it, strafe and fight. Here a bot holding counted as arrived the moment its body touched the
// volume and stood there, at the edge, for the rest of the hold. In Hardpoint, King of the Hill and
// Domination on several rotation maps: no bot heading for or holding a zone (state Objective) may
// stand within 1 unit (feet, 2D) of one spot for over ZONE_FREEZE_S while at the zone (inside its
// volume or within ZONE_NEAR of it). Fights (Attack: v7's crouch-and-hold) and retreats are not
// judged here. KN_ZONE_ALL=1: every rotation map with zones, 60 s each (the full sweep).
#define ZONE_FREEZE_S 4.0f
#define ZONE_NEAR 24.0f
static float zone_outside(const vec3 p, const vec3 center, const vec3 half) { // 2D distance outside the box
    const float dx = fmaxf(0.0f, fabsf(p.x - center.x) - half.x), dz = fmaxf(0.0f, fabsf(p.z - center.z) - half.z);
    return hypotf(dx, dz);
}

static int zone_freeze_run(const cJSON *map, const char *map_name, const int mode, const char *mode_name, const float seconds) {
    static const cJSON *maps[1];
    static int modes[1];
    maps[0] = map;
    modes[0] = mode;
    Game game = {0};
    g_game = &game;
    game_configure(&game, NULL, maps, 1, modes, 1, NULL, 0, NULL, 0);
    game.hooks.send = test_send;
    game.round_length = 100000.0f;
    game_init(&game, 0, 0, 1);
    if (!game.ready || !game.mode || !(game.mode->config.dom ? game.mode->dom_zone_count : game.mode->objective_count)) {
        game_bots_remove_all(&game);
        free_players(&game);
        return -1; // no zones on this map
    }
    for (int team = 1; team <= 2; team++) {
        for (int i = 0; i < 3; i++) {
            char team_arg[16];
            snprintf(team_arg, sizeof(team_arg), "team=%d", team);
            char *argv[] = {team_arg, "difficulty=normal"};
            BotAssignments a;
            bot_parse_assignments(2, argv, &a);
            CHECK(game_bots_add(&game, &a, NULL, 0.0f) != NULL, "bot added");
        }
    }
    vec3 anchor[GAME_BOTS_MAX];
    float still[GAME_BOTS_MAX] = {0}, worst[GAME_BOTS_MAX] = {0};
    vec3 worst_at[GAME_BOTS_MAX];
    int worst_inside[GAME_BOTS_MAX] = {0}, at_zone_ticks = 0;
    const float dt = 1.0f / 30.0f;
    tick(&game, 1.0f, 30.0f);
    for (int s = 0; s < (int) (seconds * 30.0f); s++) {
        tick(&game, dt, 30.0f);
        for (int i = 0; i < game.bots->count && i < GAME_BOTS_MAX; i++) {
            const GameBot *bot = &game.bots->list[i];
            const BotObjective *o = &bot->objective;
            const Player *p = bot->player;
            const int at_zone = p && p->active && bot->state == BOT_STATE_OBJECTIVE && o->has_zone && zone_outside(p->position, o->zone_center, o->zone_half) <= ZONE_NEAR;
            at_zone_ticks += at_zone;
            if (!at_zone || hypotf(p->position.x - anchor[i].x, p->position.z - anchor[i].z) > 1.0f || still[i] <= 0.0f) {
                if (p) anchor[i] = p->position;
                still[i] = at_zone ? dt : 0.0f;
                continue;
            }
            still[i] += dt;
            if (still[i] > worst[i]) {
                worst[i] = still[i];
                worst_at[i] = p->position;
                worst_inside[i] = o->arrived;
            }
        }
    }
    int frozen = 0;
    for (int i = 0; i < game.bots->count && i < GAME_BOTS_MAX; i++) {
        if (worst[i] <= ZONE_FREEZE_S) continue;
        frozen++;
        printf("  %s on %s: bot %d stood %.1f s at (%.0f %.0f %.0f), %s the zone\n", mode_name, map_name, i, worst[i], worst_at[i].x, worst_at[i].y, worst_at[i].z,
               worst_inside[i] ? "inside" : "at the edge of");
    }
    CHECK(at_zone_ticks > 0, "%s on %s: the bots reach the zone", mode_name, map_name);
    game_bots_remove_all(&game);
    free_players(&game);
    return frozen;
}

static void test_zone_freeze(void) {
    printf("Running zone freeze test...\n");
    static const struct { const char *mode, *map; } k_runs[] = {
        {"point", "sandstorm_v3"}, {"point", "kanji"}, {"point", "burg_new"}, {"point", "littletown"},
        {"king", "industry"}, {"king", "lumber"}, {"king", "subzero_new"},
        {"dom", "sandstorm_v3"}, {"dom", "industry"}, {"dom", "tropicano"},
    };
    int runs = 0, frozen = 0;
    if (getenv("KN_ZONE_ALL")) {
        int rotation[64];
        const size_t n = map_catalog_rotation(rotation, 64);
        static const char *const k_modes[] = {"point", "king", "dom"};
        for (size_t m = 0; m < 3; m++) {
            for (size_t i = 0; i < n; i++) {
                const MapCatalogEntry *e = map_catalog_at((size_t) rotation[i]);
                const cJSON *map = map_catalog_json((size_t) rotation[i]);
                if (!e || !map) continue;
                const int f = zone_freeze_run(map, e->key, mode_find(k_modes[m]), k_modes[m], 60.0f);
                if (f >= 0) runs++, frozen += f;
            }
        }
    } else {
        for (size_t r = 0; r < sizeof(k_runs) / sizeof(k_runs[0]); r++) {
            const int key = map_catalog_index_of_key(k_runs[r].map);
            const cJSON *map = key >= 0 ? map_catalog_json((size_t) key) : NULL;
            CHECK(map != NULL, "map %s", k_runs[r].map);
            if (!map) continue;
            const int f = zone_freeze_run(map, k_runs[r].map, mode_find(k_runs[r].mode), k_runs[r].mode, 30.0f);
            CHECK(f >= 0, "%s has %s zones", k_runs[r].map, k_runs[r].mode);
            if (f >= 0) runs++, frozen += f;
        }
    }
    printf("  %d runs, %d bots stood still at a zone over %.0f s\n", runs, frozen, ZONE_FREEZE_S);
    CHECK(frozen == 0, "no bot stands still at the zone over %.0f s (%d)", ZONE_FREEZE_S, frozen);
}


// "bots just seem to all aggregate around zone points and stare at each other ... moving but don't
// actually fire at each other. it's like they just have a party in the zones" (the user). Hardpoint,
// King of the Hill and Domination on rotation maps, four bots a side for 60 s: enemies that meet at
// a zone (both within ZONE_NEAR of a zone volume, at most ZONE_MEET_RANGE apart) fight. Checked: the
// bots fire (shots per minute), land damage and kill in every run, and the party (such a pair where
// neither has fired for ZONE_STANDOFF_S) takes at most a quarter of the time they spend there. The
// cause on main: game.canSee counted the Hardpoint / KOTH objective volume as a wall (ai.c), so the
// bots on a point could not see each other. Line of sight is not required here for that reason: it
// is the thing under test.
#define ZONE_MEET_RANGE 150.0f
#define ZONE_STANDOFF_S 3.0f
// floors and caps per run-minute, from 8 runs: main (the objective hid the point) 97 shots, 491
// damage, 6.8 kills, 287 s of party; fixed 170-206 shots, 1676-1888 damage, 20-24 kills, 5 s of party
#define ZONE_FIGHT_SHOTS_MIN 140.0f
#define ZONE_FIGHT_DAMAGE_MIN 1000.0f
#define ZONE_FIGHT_KILLS_MIN 12.0f
#define ZONE_PARTY_MAX_S 10.0f       // party seconds per run, averaged
#define ZONE_PARTY_RUN_MAX_S 30.0f   // ... and in any one run
typedef struct { float shots, damage, meet, standoff; int kills; } ZoneFight;

static int zone_near_any(const Game *game, const vec3 p) {
    const GameMode *mode = game->mode;
    if (mode->config.dom) {
        for (int z = 0; z < mode->dom_zone_count; z++) {
            if (zone_outside(p, mode->dom_zones[z].position, mode->dom_zones[z].half) <= ZONE_NEAR) return 1;
        }
        return 0;
    }
    const ModeObjective *obj = mode_objective_by_uid(game, mode->active_objective);
    return obj && zone_outside(p, obj->position, obj->half) <= ZONE_NEAR;
}

static int zone_fight_run(const cJSON *map, const char *map_name, const int mode, const char *mode_name, const float seconds, ZoneFight *out) {
    static const cJSON *maps[1];
    static int modes[1];
    maps[0] = map;
    modes[0] = mode;
    Game game = {0};
    g_game = &game;
    game_configure(&game, NULL, maps, 1, modes, 1, NULL, 0, NULL, 0);
    game.hooks.send = test_send;
    game.round_length = 100000.0f;
    game_init(&game, 0, 0, 1);
    memset(out, 0, sizeof(*out));
    if (!game.ready || !game.mode || !(game.mode->config.dom ? game.mode->dom_zone_count : game.mode->objective_count)) {
        game_bots_remove_all(&game);
        free_players(&game);
        return 0;
    }
    for (int team = 1; team <= 2; team++) {
        for (int i = 0; i < 4; i++) {
            char team_arg[16];
            snprintf(team_arg, sizeof(team_arg), "team=%d", team);
            char *argv[] = {team_arg, "difficulty=normal"};
            BotAssignments a;
            bot_parse_assignments(2, argv, &a);
            CHECK(game_bots_add(&game, &a, NULL, 0.0f) != NULL, "bot added");
        }
    }
    tick(&game, 1.0f, 30.0f);
    reset_counts();
    const float dt = 1.0f / 30.0f;
    float last_shot[GAME_BOTS_MAX], health[GAME_BOTS_MAX];
    long serial[GAME_BOTS_MAX];
    for (int i = 0; i < GAME_BOTS_MAX; i++) last_shot[i] = -100.0f, health[i] = -1.0f, serial[i] = -1;
    for (int s = 0; s < (int) (seconds * 30.0f); s++) {
        tick(&game, dt, 30.0f);
        const float now = (float) s * dt;
        const int n = game.bots->count < GAME_BOTS_MAX ? game.bots->count : GAME_BOTS_MAX;
        for (int i = 0; i < n; i++) {
            const Player *p = game.bots->list[i].player;
            const int alive = p && p->active && p->health > 0.0f;
            const unsigned int fired = p ? (unsigned int) p->life_shots + p->shot_count : 0u; // shots this life + projectiles
            if (alive && serial[i] >= 0 && fired > (unsigned int) serial[i]) {
                out->shots += (float) (fired - (unsigned int) serial[i]);
                last_shot[i] = now;
            }
            serial[i] = p ? (long) fired : -1;
            if (alive && health[i] >= 0.0f && p->health < health[i]) out->damage += health[i] - p->health;
            health[i] = alive ? p->health : -1.0f;
        }
        for (int i = 0; i < n; i++) {
            const Player *a = game.bots->list[i].player;
            if (!a || !a->active || !zone_near_any(&game, a->position)) continue;
            for (int j = i + 1; j < n; j++) {
                const Player *b = game.bots->list[j].player;
                if (!b || !b->active || b->team == a->team || !zone_near_any(&game, b->position)) continue;
                const float dx = a->position.x - b->position.x, dz = a->position.z - b->position.z;
                if (dx * dx + dz * dz > ZONE_MEET_RANGE * ZONE_MEET_RANGE) continue;
                out->meet += dt;
                if (now - last_shot[i] > ZONE_STANDOFF_S && now - last_shot[j] > ZONE_STANDOFF_S) out->standoff += dt;
            }
        }
    }
    out->kills = g_kills;
    printf("  %s on %s: %.0f shots/min, %.0f damage, %d kills, enemies together at a zone %.1f s, without a shot %.1f s\n", mode_name, map_name,
           out->shots * 60.0f / seconds, out->damage, out->kills, out->meet, out->standoff);
    game_bots_remove_all(&game);
    free_players(&game);
    return 1;
}

static void test_zone_fight(void) {
    printf("Running zone fight test...\n");
    static const struct { const char *mode, *map; } k_runs[] = {
        {"point", "sandstorm_v3"}, {"point", "kanji"}, {"point", "burg_new"},
        {"king", "industry"}, {"king", "littletown"},
        {"dom", "sandstorm_v3"}, {"dom", "industry"}, {"dom", "tropicano"},
    };
    ZoneFight total = {0};
    int runs = 0, quiet_runs = 0;
    float worst_party = 0.0f;
    for (size_t r = 0; r < sizeof(k_runs) / sizeof(k_runs[0]); r++) {
        const int key = map_catalog_index_of_key(k_runs[r].map);
        const cJSON *map = key >= 0 ? map_catalog_json((size_t) key) : NULL;
        CHECK(map != NULL, "map %s", k_runs[r].map);
        ZoneFight f;
        if (!map || !zone_fight_run(map, k_runs[r].map, mode_find(k_runs[r].mode), k_runs[r].mode, 60.0f, &f)) continue;
        runs++;
        quiet_runs += f.kills == 0;
        if (f.standoff > worst_party) worst_party = f.standoff;
        total.shots += f.shots, total.damage += f.damage, total.meet += f.meet, total.standoff += f.standoff, total.kills += f.kills;
    }
    const float minutes = (float) runs;
    printf("  %d runs: %.0f shots/min, %.0f damage/min, %.1f kills/min, together %.0f s, without a shot %.0f s (%.0f%%), %d runs without a kill\n", runs,
           total.shots / minutes, total.damage / minutes, (float) total.kills / minutes, total.meet, total.standoff,
           total.meet > 0.0f ? 100.0f * total.standoff / total.meet : 0.0f, quiet_runs);
    CHECK(runs == (int) (sizeof(k_runs) / sizeof(k_runs[0])), "every zone run played (%d)", runs);
    CHECK(total.meet > 0.0f, "enemies meet at the zones (%.0f s)", total.meet);
    CHECK(total.shots / minutes >= ZONE_FIGHT_SHOTS_MIN, "the bots fire (%.0f shots/min, at least %.0f)", total.shots / minutes, ZONE_FIGHT_SHOTS_MIN);
    CHECK(total.damage / minutes >= ZONE_FIGHT_DAMAGE_MIN, "damage lands (%.0f/min, at least %.0f)", total.damage / minutes, ZONE_FIGHT_DAMAGE_MIN);
    CHECK((float) total.kills / minutes >= ZONE_FIGHT_KILLS_MIN && quiet_runs == 0, "and kills (%.1f/min, at least %.1f; %d runs without one)",
          (float) total.kills / minutes, ZONE_FIGHT_KILLS_MIN, quiet_runs);
    CHECK(total.standoff / minutes <= ZONE_PARTY_MAX_S && worst_party <= ZONE_PARTY_RUN_MAX_S,
          "no party at the zones: enemies together without a shot %.1f s per run (at most %.0f), %.1f s in the worst run (at most %.0f)",
          total.standoff / minutes, ZONE_PARTY_MAX_S, worst_party, ZONE_PARTY_RUN_MAX_S);
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

    if (getenv("KN_BOT_SEED")) pcg32_srandom((uint64_t) atoll(getenv("KN_BOT_SEED")), 54u); // another run of the same tests (as bot_test)
    test_ctf(&game, 0);
    test_ctf_small(&game);
    test_point(&game, 1);
    test_under_fire(&game, 1);
    test_dom(&game, 2);
    test_king(&game, 3);
    test_tdf(&game, 4);
    test_lms(&game, 5);
    test_zone_freeze();
    test_zone_fight();

    if (g_failures) {
        printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("Test OK\n");
    return 0;
}
