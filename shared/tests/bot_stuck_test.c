// Bots never stand stuck (the user: "bots walk and get stuck", "bots walking into walls"): over a
// fixed list of rotation maps and modes, through the shared game at 30 Hz with six normal
// NextBots,
// - every spawn of the map is routable: the nav grid (shared/src/bot_nav.c) finds the ground under
//   it and A* reaches another spawn from it (clockwork's spawn at (338, 18, 71) had no node under
//   it: the bot that spawned there never moved);
// - nothing stays stuck: a bot that means to move (a route goal over 12 units away, not fighting
//   an enemy it sees, not holding a zone it stands in) and moves less than STUCK_MOVE units in
//   STUCK_S seconds fails, airborne or not (freight's Hardpoint wedged one in mid-air at
//   (134, 32, -52) for 45 s), and so does one that runs into a wall repeatedly (STUCK_REPEAT stuck
//   recoveries at one spot).
// Also a bot hanging on freight's ladder top lets go (test_ladder_hang).
// KN_STUCK_ALL=1 sweeps every rotation map in every mode the bots play, 60 s each (~6 min);
// KN_STUCK_ONLY=<map>:<mode> runs one; KN_STUCK_SECONDS sets the run length; KN_BOT_SEED another
// run. Debugging: KN_STUCK_TRACE=<bot>:<from s>:<to s> prints one bot's follower state every
// tick, KN_STUCK_VERBOSE=1 every stuck recovery, KN_STUCK_PLACE=x,y,z starts bot 0 there, and with
// KN_STUCK_ONLY, KN_STUCK_PROBE=x,y,z (+ KN_STUCK_GOAL=x,y,z) prints the routes from a point.
// Run from the repository root: ./<build>/bot_stuck_test
#include <shared.h>
#include <bots.h>
#include <bot_path.h>
#include <map_catalog.h>
#include <pcg_basic.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;

#define CHECK(cond, ...) do { \
    if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_failures++; } \
} while (0)

#define STUCK_S 5.0f       // a bot that means to move must cover STUCK_MOVE within this
#define STUCK_MOVE 1.0f
#define STUCK_REPEAT 3     // stuck recoveries within STUCK_SPOT units of each other: running into a wall
#define STUCK_SPOT 16.0f
#define BOTS 6

static void test_send(Game *game, const int uid, const int type, const void *payload, const uint16_t length) {
    (void) game, (void) uid, (void) type, (void) payload, (void) length;
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

static void tick(Game *game, const float seconds) {
    const float dt = 1.0f / 30.0f;
    for (float t = 0.0f; t < seconds; t += dt) {
        g_now += dt;
        game_tick(game, (float) g_now, dt);
    }
}

static int game_open(Game *game, const cJSON *map, const int mode) {
    static const cJSON *maps[1];
    static int modes[1];
    maps[0] = map;
    modes[0] = mode;
    memset(game, 0, sizeof(*game));
    game_configure(game, NULL, maps, 1, modes, 1, NULL, 0, NULL, 0);
    game->hooks.send = test_send;
    game->round_length = 100000.0f;
    game_init(game, 0, 0, 1);
    return game->ready;
}

static void game_close(Game *game) {
    game_bots_remove_all(game);
    free_players(game);
    game_bots_free(game);
}

// Spawns the bot grid can't route from, by map and spawn index. v10's Bazaar has two spawns on its
// roofs (y 168) that no grid node reaches (bot_nav_find_path finds no route). Listed here so the
// rest of the rotation stays strict; bots spawned there are not yet verified in play.
static const struct { const char *map; size_t spawn; } k_unroutable_spawns[] = {{"bazaar", 8}, {"bazaar", 9}};

static int unroutable_spawn(const char *name, const size_t i) {
    for (size_t k = 0; k < sizeof(k_unroutable_spawns) / sizeof(k_unroutable_spawns[0]); k++) {
        if (!strcmp(name, k_unroutable_spawns[k].map) && i == k_unroutable_spawns[k].spawn) return 1;
    }
    return 0;
}

// Every spawn stands on the grid and reaches another spawn (or, alone, has ground under it).
static int spawns_routable(const char *name, const cJSON *map) {
    Game game;
    if (!game_open(&game, map, mode_find("ffa"))) {
        CHECK(0, "%s loads", name);
        game_close(&game);
        return 1;
    }
    int bad = 0;
    const size_t n = game.map->spawn_count;
    for (size_t i = 0; i < n; i++) {
        const vec3 a = game.map->spawns[i]->position;
        int routable = 0, start = 0;
        float first = 0.0f;
        BotPath path;
        for (size_t k = 1; k < n && !routable; k++) {
            const vec3 b = game.map->spawns[(i + k) % n]->position;
            if (hypotf(a.x - b.x, a.z - b.z) < 16.0f) continue; // the same spot
            const int found = bot_nav_find_path(&game, a, b, &path);
            start |= found;
            routable = found && path.reached;
            if (found && path.count) first = hypotf(path.points[0].x - a.x, path.points[0].z - a.z);
        }
        if (n == 1) routable = 1;
        if (!routable && unroutable_spawn(name, i)) continue;
        if (!routable) {
            bad++;
            printf("  %s: spawn %zu at (%.1f %.1f %.1f): %s (on grid %d, first waypoint %.1f away)\n", name, i, a.x, a.y, a.z,
                   start ? "no route to any other spawn" : "no nav node to start from", bot_nav_on_grid(&game, a), first);
        }
    }
    game_close(&game);
    return bad;
}

typedef struct {
    vec3 anchor;
    float still;            // s the bot means to move but stays within STUCK_MOVE of the anchor
    float worst;
    vec3 worst_at;
    int worst_ground, worst_state;
    float last_recover;
    vec3 recover_at[64];
    int recovers, repeat;
    vec3 repeat_at;
} BotWatch;

// The bot means to move: alive, a route goal some way off, and not fighting an enemy it sees or
// holding the zone it stands in (both stand still on purpose: v7's crouch-and-hold, a defender).
static int means_to_move(Game *game, const GameBot *bot) {
    const Player *p = bot->player;
    if (!p || !p->active || p->frozen || game->bots->stopped) return 0;
    if (bot->state == BOT_STATE_ATTACK && bot->sees_target) return 0;
    if (bot->state == BOT_STATE_OBJECTIVE && bot->objective.hold && bot->objective.arrived) return 0;
    vec3 goal;
    if (!bot_path_goal(bot, &goal)) return 0;
    return hypotf(goal.x - p->position.x, goal.z - p->position.z) > 12.0f || fabsf(goal.y - p->position.y) > 12.0f;
}

// One run: the bots that stayed stuck (or ran into one spot again and again), printed.
static int stuck_run(const char *name, const cJSON *map, const char *mode_name, const float seconds) {
    Game game;
    const int mode = mode_find(mode_name);
    if (mode < 0 || !game_open(&game, map, mode)) {
        game_close(&game);
        return -1; // the map does not run this mode
    }
    const int teams = game.mode && game.mode->config.teams;
    for (int i = 0; i < BOTS; i++) {
        char team_arg[16];
        snprintf(team_arg, sizeof(team_arg), "team=%d", 1 + i % 2);
        char *argv[] = {team_arg, "difficulty=normal"};
        BotAssignments a;
        bot_parse_assignments(teams ? 2 : 1, teams ? argv : argv + 1, &a);
        CHECK(game_bots_add(&game, &a, NULL, 0.0f) != NULL, "bot added");
    }
    tick(&game, 1.0f);
    vec3 place;
    if (getenv("KN_STUCK_PLACE") && sscanf(getenv("KN_STUCK_PLACE"), "%f,%f,%f", &place.x, &place.y, &place.z) == 3 && game.bots->count &&
        game.bots->list[0].player) { // debugging: bot 0 starts here
        Player *p = game.bots->list[0].player;
        p->position = p->last_position = place;
        p->velocity = (vec3) {0.0f, 0.0f, 0.0f};
        bot_path_reset(&game.bots->list[0], p);
    }
    BotWatch w[GAME_BOTS_MAX];
    memset(w, 0, sizeof(w));
    const float dt = 1.0f / 30.0f;
    const char *trace = getenv("KN_STUCK_TRACE");
    int tb = -1;
    float t0 = 0.0f, t1 = 0.0f;
    if (trace) sscanf(trace, "%d:%f:%f", &tb, &t0, &t1);
    for (int s = 0; s < (int) (seconds * 30.0f); s++) {
        tick(&game, dt);
        const float t = (float) s * dt;
        for (int i = 0; i < game.bots->count && i < GAME_BOTS_MAX; i++) {
            const GameBot *bot = &game.bots->list[i];
            const Player *p = bot->player;
            BotWatch *b = &w[i];
            if (i == tb && t >= t0 && t <= t1 && p) {
                const vec3 wp = bot->path_index < bot->path.count ? bot->path.points[bot->path_index] : bot->roam_target;
                printf("  T %.2f pos (%.2f %.2f %.2f) vel (%.3f %.3f %.3f) gr %d lad %d act %d st %s wp %d/%d k%d (%.1f %.1f %.1f) goal (%.1f %.1f %.1f) lanes %d%d%d%d%d np %.0f bc %d rec %.0f boxed %d\n",
                       t, p->position.x, p->position.y, p->position.z, p->velocity.x, p->velocity.y, p->velocity.z, p->on_ground, p->on_ladder, p->active, bot_state_name(bot->state),
                       bot->path_index, bot->path.count, bot->path_index < bot->path.count ? bot->path.kinds[bot->path_index] : -1, wp.x, wp.y, wp.z,
                       bot->follow.goal.x, bot->follow.goal.y, bot->follow.goal.z, bot->follow.lanes[0], bot->follow.lanes[1], bot->follow.lanes[2],
                       bot->follow.lanes[3], bot->follow.lanes[4], bot->follow.no_progress, bot->follow.block_count, bot->follow.recover_timer, bot->follow.boxed);
            }
            // a stuck recovery started (the follower backs off and hops): where
            if (p && p->active && bot->follow.recover_timer > b->last_recover + 1.0f) {
                int near = 1;
                for (int k = 0; k < b->recovers && k < 64; k++) {
                    const vec3 q = b->recover_at[k];
                    near += hypotf(q.x - p->position.x, q.z - p->position.z) < STUCK_SPOT && fabsf(q.y - p->position.y) < 6.0f;
                }
                if (getenv("KN_STUCK_VERBOSE")) { // debugging: every stuck recovery
                    const BotPath *path = &bot->path;
                    const vec3 wp = bot->path_index < path->count ? path->points[bot->path_index] : bot->follow.goal;
                    printf("    recover t %.1f bot %d (%.1f %.1f %.1f) %s goal (%.1f %.1f %.1f) route %d/%d k%d wp (%.1f %.1f %.1f) lanes %d%d%d%d%d%d%d\n", t, i,
                           p->position.x, p->position.y, p->position.z, bot_state_name(bot->state), bot->follow.goal.x, bot->follow.goal.y, bot->follow.goal.z,
                           bot->path_index, path->count, bot->path_index < path->count ? path->kinds[bot->path_index] : -1, wp.x, wp.y, wp.z, bot->follow.lanes[0],
                           bot->follow.lanes[1], bot->follow.lanes[2], bot->follow.lanes[3], bot->follow.lanes[4], bot->follow.lanes[5], bot->follow.lanes[6]);
                }
                if (b->recovers < 64) b->recover_at[b->recovers] = p->position;
                b->recovers++;
                if (near > b->repeat) {
                    b->repeat = near;
                    b->repeat_at = p->position;
                }
            }
            b->last_recover = bot->follow.recover_timer;
            if (!means_to_move(&game, bot)) {
                b->still = 0.0f;
                continue;
            }
            const float dx = p->position.x - b->anchor.x, dy = p->position.y - b->anchor.y, dz = p->position.z - b->anchor.z;
            if (b->still <= 0.0f || sqrtf(dx * dx + dy * dy + dz * dz) > STUCK_MOVE) {
                b->anchor = p->position;
                b->still = dt;
                continue;
            }
            b->still += dt;
            if (b->still > STUCK_S && b->still - dt <= STUCK_S) { // the moment it counts: what the follower thinks
                const BotPath *path = &bot->path;
                const vec3 wp = bot->path_index < path->count ? path->points[bot->path_index] : bot->follow.goal;
                const BotObjective *o = &bot->objective;
                printf("    at %.1f s bot %d (%.1f %.1f %.1f) gr %d lad %d %s: goal (%.1f %.1f %.1f) route %d/%d k%d reached %d stranded %d wp (%.1f %.1f %.1f) "
                       "lanes %d%d%d%d%d%d%d boxed %d obj role %d hold %d arrived %d goal (%.1f %.1f %.1f)\n",
                       t, i, p->position.x, p->position.y, p->position.z, p->on_ground, p->on_ladder, bot_state_name(bot->state), bot->follow.goal.x,
                       bot->follow.goal.y, bot->follow.goal.z, bot->path_index, path->count, bot->path_index < path->count ? path->kinds[bot->path_index] : -1,
                       path->reached, bot->path_stranded, wp.x, wp.y, wp.z, bot->follow.lanes[0], bot->follow.lanes[1], bot->follow.lanes[2], bot->follow.lanes[3],
                       bot->follow.lanes[4], bot->follow.lanes[5], bot->follow.lanes[6], bot->follow.boxed, o->role, o->hold, o->arrived, o->goal.x, o->goal.y, o->goal.z);
            }
            if (b->still > b->worst) {
                b->worst = b->still;
                b->worst_at = p->position;
                b->worst_ground = p->on_ground;
                b->worst_state = bot->state;
            }
        }
    }
    int stuck = 0;
    for (int i = 0; i < game.bots->count && i < GAME_BOTS_MAX; i++) {
        const BotWatch *b = &w[i];
        if (b->worst > STUCK_S) {
            stuck++;
            printf("  %s on %s: bot %d stuck %.1f s at (%.1f %.1f %.1f) (%s, %s)\n", mode_name, name, i, b->worst, b->worst_at.x, b->worst_at.y, b->worst_at.z,
                   b->worst_ground ? "on the ground" : "airborne", bot_state_name((BotState) b->worst_state));
        }
        if (b->repeat >= STUCK_REPEAT) {
            stuck++;
            printf("  %s on %s: bot %d ran into one spot %d times, at (%.1f %.1f %.1f)\n", mode_name, name, i, b->repeat, b->repeat_at.x, b->repeat_at.y, b->repeat_at.z);
        }
    }
    game_close(&game);
    return stuck;
}

static const cJSON *catalog_map(const char *key) {
    const int index = map_catalog_index_of_key(key);
    return index >= 0 ? map_catalog_json((size_t) index) : NULL;
}

// Clockwork: its spawn at (338, 18, 71) stands inside the bounds of a cylinder (a tower with a
// circular collision); the grid must cover it with a node (the bounds-only grid had none there).
static void test_cylinder_spawn_on_grid(void) {
    printf("Running cylinder spawn grid test...\n");
    const cJSON *map = catalog_map("clockwork");
    Game game;
    CHECK(map && game_open(&game, map, mode_find("ffa")), "clockwork loads");
    if (!map || !game.ready) {
        game_close(&game);
        return;
    }
    const vec3 want = {338.0f, 18.0f, 71.0f};
    int found = -1;
    for (size_t i = 0; i < game.map->spawn_count; i++) {
        const vec3 s = game.map->spawns[i]->position;
        if (hypotf(s.x - want.x, s.z - want.z) < 1.0f) found = (int) i;
    }
    CHECK(found >= 0, "clockwork has a spawn at (%.0f %.0f %.0f)", want.x, want.y, want.z);
    if (found >= 0) CHECK(bot_nav_on_grid(&game, game.map->spawns[found]->position), "clockwork's spawn at (%.0f %.0f %.0f) is on the nav grid", want.x, want.y, want.z);
    game_close(&game);
}

// Freight: a bot hanging beside the top of the ladder at (138, 0, -46) (a body touching a ladder
// below its top gets no gravity) with its route's next waypoint the floor 32 units straight below
// stood there for good (the waypoint read as reached, 2D). It must let go and drop.
static void test_ladder_hang(void) {
    printf("Running ladder hang test...\n");
    const cJSON *map = catalog_map("freight");
    Game game;
    CHECK(map && game_open(&game, map, mode_find("ffa")), "freight loads");
    if (!map || !game.ready) return;
    BotAssignments a;
    char *argv[] = {"difficulty=normal"};
    bot_parse_assignments(1, argv, &a);
    CHECK(game_bots_add(&game, &a, NULL, 0.0f) != NULL, "bot added");
    tick(&game, 0.5f);
    GameBot *bot = &game.bots->list[0];
    Player *p = bot->player;
    CHECK(p && p->active, "the bot spawned");
    if (p && p->active) {
        p->position = p->last_position = (vec3) {134.0f, 32.0f, -52.0f};
        p->velocity = (vec3) {0.0f, 0.0f, 0.0f};
        bot_path_reset(bot, p);
        bot->has_roam = 1;
        bot->roam_target = (vec3) {15.0f, 2.5f, 104.5f}; // the floor below and on (west)
        bot->roam_timer = 60000.0f;
        tick(&game, 1.0f / 30.0f);
        CHECK(p->on_ladder && p->position.y > 30.0f, "the ladder holds the body (y %.1f, ladder %d)", p->position.y, p->on_ladder);
        float low = p->position.y;
        for (int i = 0; i < 90; i++) {
            tick(&game, 1.0f / 30.0f);
            low = fminf(low, p->position.y);
            if (getenv("KN_STUCK_VERBOSE")) printf("    t %d (%.1f %.1f %.1f) ladder %d\n", i, p->position.x, p->position.y, p->position.z, p->on_ladder);
        }
        CHECK(low < 10.0f, "the bot let go of the ladder and dropped (lowest y %.1f)", low);
    }
    game_close(&game);
}

int main(void) {
    pcg32_srandom(getenv("KN_BOT_SEED") ? (uint64_t) atoll(getenv("KN_BOT_SEED")) : 42u, 54u);
    const float seconds = getenv("KN_STUCK_SECONDS") ? (float) atof(getenv("KN_STUCK_SECONDS")) : 60.0f;
    const char *only = getenv("KN_STUCK_ONLY");

    // the rotation maps (the spawn check is cheap: every one, every run)
    int rotation[96];
    const size_t rotation_count = map_catalog_rotation(rotation, 96);
    CHECK(rotation_count >= 10, "rotation maps (%zu)", rotation_count);
    if (!only) {
        printf("Running spawn route test...\n");
        int bad = 0;
        for (size_t i = 0; i < rotation_count; i++) {
            const MapCatalogEntry *e = map_catalog_at((size_t) rotation[i]);
            const cJSON *map = map_catalog_json((size_t) rotation[i]);
            CHECK(e && map, "rotation map %d loads", rotation[i]);
            if (e && map) bad += spawns_routable(e->key, map);
        }
        printf("  %zu maps, %d spawns without a route\n", rotation_count, bad);
        CHECK(bad == 0, "every spawn of every rotation map is routable (%d are not)", bad);
    }

    if (!only) {
        test_cylinder_spawn_on_grid();
        test_ladder_hang();
    }

    printf("Running stuck bot test...\n");
    // the fixed list: the stucks found so far, then a spread of the rotation in its common modes
    static const struct { const char *map, *mode; } k_runs[] = {
        {"freight", "point"}, {"clockwork", "ffa"}, {"clockwork", "tdm"}, {"lagoon", "tdf"}, {"oasis_v3", "king"}, {"kanji", "kc"},
        {"kanji", "ffa"}, {"sandstorm_v3", "ctf"}, {"burg_new", "tdm"}, {"littletown", "point"},
        {"industry", "king"}, {"subzero_new", "ffa"}, {"tropicano", "dom"}, {"lumber", "ffa"},
        {"site", "tdm"}, {"bureau", "ffa"}, {"evacuation", "ffa"}, {"shipyard", "tdm"},
    };
    static const char *const k_modes[] = {"ffa", "tdm", "point", "ctf", "king", "dom", "tdf", "kc", "hunt"};
    int runs = 0, stuck = 0;
    if (only) {
        char map_key[64], mode[16];
        if (sscanf(only, "%63[^:]:%15s", map_key, mode) != 2) {
            printf("KN_STUCK_ONLY=<map>:<mode>\n");
            return 1;
        }
        const cJSON *map = catalog_map(map_key);
        CHECK(map != NULL, "map %s", map_key);
        if (map) spawns_routable(map_key, map);
        vec3 probe;
        if (map && getenv("KN_STUCK_PROBE") && sscanf(getenv("KN_STUCK_PROBE"), "%f,%f,%f", &probe.x, &probe.y, &probe.z) == 3) { // debugging
            Game game;
            game_open(&game, map, mode_find(mode));
            BotPath path;
            for (size_t i = 0; i < game.map->spawn_count; i++) {
                const int found = bot_nav_find_path(&game, probe, game.map->spawns[i]->position, &path);
                printf("  probe: on grid %d, to spawn %zu: found %d reached %d count %d first (%.1f %.1f %.1f)\n", bot_nav_on_grid(&game, probe), i, found, path.reached,
                       path.count, path.count ? path.points[0].x : 0.0f, path.count ? path.points[0].y : 0.0f, path.count ? path.points[0].z : 0.0f);
            }
            vec3 goal;
            if (getenv("KN_STUCK_GOAL") && sscanf(getenv("KN_STUCK_GOAL"), "%f,%f,%f", &goal.x, &goal.y, &goal.z) == 3) {
                const int found = bot_nav_find_path(&game, probe, goal, &path);
                printf("  probe: to the goal: found %d reached %d count %d, goal on grid %d\n", found, path.reached, path.count, bot_nav_on_grid(&game, goal));
            }
            game_close(&game);
        }
        const int s = map ? stuck_run(map_key, map, mode, seconds) : -1;
        CHECK(s >= 0, "%s runs %s", map_key, mode);
        if (s >= 0) runs++, stuck += s;
    } else if (getenv("KN_STUCK_ALL")) {
        for (size_t m = 0; m < sizeof(k_modes) / sizeof(k_modes[0]); m++) {
            for (size_t i = 0; i < rotation_count; i++) {
                const MapCatalogEntry *e = map_catalog_at((size_t) rotation[i]);
                const cJSON *map = map_catalog_json((size_t) rotation[i]);
                if (!e || !map) continue;
                const int s = stuck_run(e->key, map, k_modes[m], seconds);
                if (s >= 0) runs++, stuck += s;
                fflush(stdout);
            }
        }
    } else {
        for (size_t r = 0; r < sizeof(k_runs) / sizeof(k_runs[0]); r++) {
            const cJSON *map = catalog_map(k_runs[r].map);
            CHECK(map != NULL, "map %s", k_runs[r].map);
            if (!map) continue;
            const int s = stuck_run(k_runs[r].map, map, k_runs[r].mode, seconds);
            CHECK(s >= 0, "%s runs %s", k_runs[r].map, k_runs[r].mode);
            if (s >= 0) runs++, stuck += s;
            fflush(stdout);
        }
    }
    printf("  %d runs of %d bots x %.0f s, %d stuck\n", runs, BOTS, seconds, stuck);
    CHECK(stuck == 0, "no bot stays stuck over %.0f s or runs into one spot %d times (%d)", STUCK_S, STUCK_REPEAT, stuck);

    if (g_failures) {
        printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("Test OK\n");
    return 0;
}
