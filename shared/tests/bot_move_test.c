// Bot locomotion (shared/src/bot_move.c): a movement simulation through the shared game at a
// client's 60 Hz. A scripted walker driven by bot_move_apply on open ground (hop cadence, speed
// against walking, no head bumps), under a low ceiling (no hops, no bumps), into a wall and
// off a ledge (the last hop lands short of both), then the full AI on a corridor map with a
// tunnel and a doorway (no head bumps, no wall contacts in a hop or its slide) and on a
// plateau over the void (no hop off the edge). Hop skills: the none / learning / clean mix per
// difficulty over seeded bots, a non-hopper never hops, a learning hopper is slower than a clean
// one and faster than a walker, and no skill bumps its head or hops into a wall or off a ledge.
// Run from the repository root: ./<build>/bot_move_test
#include <shared.h>
#include <bots.h>
#include <bot_move.h>
#include <chat_commands.h>
#include <pcg_basic.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;
static double g_now;
static char g_reply[1024];

#define CHECK(cond, ...) do { \
    if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_failures++; } \
} while (0)

#define HZ 60.0f
#define FRAME (1.0f / HZ)
#define YAW_PLUS_X (-1.5707963f) // bot_forward(yaw) = (-sin, 0, -cos): +X

static void test_send(Game *game, const int uid, const int type, const void *payload, const uint16_t length) {
    (void) game; (void) uid; (void) type; (void) payload; (void) length;
}

static void host_log(void *user, const char *text) {
    (void) user;
    snprintf(g_reply, sizeof(g_reply), "%s", text);
}

static ChatCommandHost g_host;

static const char *run(const char *line) {
    g_reply[0] = 0;
    chat_command_handle(&g_host, CHAT_CMD_CONSOLE, line);
    return g_reply;
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

static Player *add_human(Game *game, const int uid, const char *name) {
    Player *player = player_init(game);
    player->uid = uid;
    player->class_index = 0;
    snprintf(player->name, sizeof(player->name), "%s", name);
    game_players_add(game, player);
    player_spawn(player);
    return player;
}

static void tick(Game *game) {
    g_now += FRAME;
    game_tick(game, (float) g_now, FRAME);
}

// ---------------------------------------------------------------- maps
// A runway along X: a 2000 x 200 floor (top at 0) with spawns at both ends; `extra` objects.
static cJSON *runway(const char *extra) {
    char text[4096];
    snprintf(text, sizeof(text),
             "{\"name\":\"Runway\",\"spawns\":[[-900,0,0,0,0,0],[900,0,0,0,0,0]],\"objects\":[{\"p\":[0,-10,0],\"s\":[2000,10,2000]}%s%s]}",
             extra && extra[0] ? "," : "", extra ? extra : "");
    return cJSON_Parse(text);
}

// The AI corridor: a 1400 x 160 floor walled along both long sides (30 high), a tunnel (ceiling
// 14 over the feet) over x -100..100, a cross wall at x 300 with a 40-wide doorway at z 0, four
// spawns along it.
static const char *k_corridor =
    "{\"name\":\"Corridor\",\"spawns\":[[-620,0,0,0,0,0],[620,0,0,0,0,0],[-300,0,50,0,0,0],[450,0,-50,0,0,0]],"
    "\"objects\":["
    "{\"p\":[0,-10,0],\"s\":[1400,10,160]},"
    "{\"p\":[0,0,-85],\"s\":[1400,30,10]},"
    "{\"p\":[0,0,85],\"s\":[1400,30,10]},"
    "{\"p\":[-705,0,0],\"s\":[10,30,180]},"
    "{\"p\":[705,0,0],\"s\":[10,30,180]},"
    "{\"p\":[0,14,0],\"s\":[200,6,160]},"
    "{\"p\":[300,0,-50],\"s\":[10,30,60]},"
    "{\"p\":[300,0,50],\"s\":[10,30,60]}"
    "]}";

// The plateau: a 400 x 400 block (top at 30) over nothing, spawns inset from the corners.
static const char *k_plateau =
    "{\"name\":\"Plateau\",\"spawns\":[[-150,30,-150,0,0,0],[150,30,150,0,0,0],[-150,30,150,0,0,0],[150,30,-150,0,0,0]],"
    "\"objects\":[{\"p\":[0,0,0],\"s\":[400,30,400]}]}";

// ---------------------------------------------------------------- the scripted walker

typedef struct {
    int hops, bumps, frames_grounded_before_hop_max, hop_wait_frames_total;
    float distance, max_y, speed_peak;
    float first_hop_x, last_land_x;
    int landings;
    // the frame something happened: wall contact, leaving the ground past the edge
    int wall_contact_frame, wall_contact_hopping, wall_contact_sliding;
    int fell_frame, fell_hopping;
} WalkStats;

// A wall stopped `p` on an axis: the velocity component (> 0.02 last frame) is zero now AND a
// solid touches the body on that side (a slide turning the velocity to the look also zeroes a
// component, against nothing).
static int touching(const Game *game, const Player *p, const int axis_z, const float sign) {
    const float hw = p->scale + player_hitbox_pad_w(p) + 0.5f;
    const float lo[3] = {p->position.x - hw + (axis_z ? 0.0f : sign * 0.5f), p->position.y + 3.2f, p->position.z - hw + (axis_z ? sign * 0.5f : 0.0f)};
    const float hi[3] = {p->position.x + hw + (axis_z ? 0.0f : sign * 0.5f), p->position.y + game_constants.player_height - 0.5f, p->position.z + hw + (axis_z ? sign * 0.5f : 0.0f)};
    for (size_t i = 0; game->map && i < game->map->object_count; i++) {
        const Object *o = game->map->objects[i];
        if (!bot_object_blocks_player(o) || o->ramp) continue;
        const vec3 sz = object_box_size(o);
        if (hi[0] < o->position.x - sz.x * 0.5f || lo[0] > o->position.x + sz.x * 0.5f || hi[1] < o->position.y || lo[1] > o->position.y + sz.y ||
            hi[2] < o->position.z - sz.z * 0.5f || lo[2] > o->position.z + sz.z * 0.5f) continue;
        return 1;
    }
    return 0;
}

static int wall_stop(const Game *game, const Player *p, const vec3 before) {
    return (fabsf(before.x) > 0.02f && p->velocity.x == 0.0f && touching(game, p, 0, before.x > 0.0f ? 1.0f : -1.0f)) ||
           (fabsf(before.z) > 0.02f && p->velocity.z == 0.0f && touching(game, p, 1, before.z > 0.0f ? 1.0f : -1.0f));
}

// Walk `player` along +X for `seconds` with (hop) or without bot_move_apply; the frame-level
// observations in *st.
// The walker's hop skill: -1 = the default stub (max difficulty: clean), else a BotHopSkill at
// normal difficulty through a uid whose personality seed rolls it.
static int g_walk_skill = -1;
#define PLAIN_WALKER (-2) // average_walk: no bot_move_apply at all

static int uid_for_skill(const int skill, const float difficulty) {
    for (int uid = 0; uid < 100000; uid++) {
        if (bot_move_skill_roll(bot_move_seed("", uid), difficulty) == skill) return uid;
    }
    return -1;
}

static void walk_at(Game *game, Player *player, const float start_x, const float seconds, const int hop, const float edge_x, const float hz, WalkStats *st) {
    memset(st, 0, sizeof(*st));
    st->wall_contact_frame = st->fell_frame = -1;
    GameBot stub;
    memset(&stub, 0, sizeof(stub));
    stub.type = BOT_TYPE_NEXTBOT;
    bot_config_defaults(BOT_TYPE_NEXTBOT, &stub.config);
    stub.player = player;
    stub.target_uid = -1;
    if (g_walk_skill >= 0) {
        stub.config.difficulty = BOT_DIFFICULTY_NORMAL;
        stub.uid = uid_for_skill(g_walk_skill, BOT_DIFFICULTY_NORMAL);
    }
    player->position = (vec3) {start_x, 0.0f, 0.0f};
    player->velocity = (vec3) {0.0f, 0.0f, 0.0f};
    player->direction.y = YAW_PLUS_X;
    int seq = 0, grounded_frames = 0, was_hopping = 0;
    const float dt = 1.0f / hz;
    const int ticks = (int) (seconds * hz);
    const int per_tick = (int) ceilf(dt * 60.0f - 1e-3f); // bot_queue_frames: frames of <= 1/60 s
    for (int i = 0; i < ticks; i++) {
        int move = 0;
        unsigned char jump = 0, crouch = 0, late = 0;
        if (hop) bot_move_apply(game, &stub, player, dt * 1000.0f, YAW_PLUS_X, &move, &jump, &crouch, &late);
        const vec3 before = player->position, vbefore = player->velocity;
        const int on_ground_before = player->on_ground;
        if (on_ground_before) grounded_frames++;
        else grounded_frames = 0;
        if (jump && st->landings) { // after a landing (the first hop follows the walk-up)
            if (grounded_frames > st->frames_grounded_before_hop_max) st->frames_grounded_before_hop_max = grounded_frames;
            st->hop_wait_frames_total += grounded_frames;
        }
        if (jump && !st->hops) st->first_hop_x = player->position.x;
        for (int k = 0; k < per_tick; k++) { // like bot_queue_frames
            Input in;
            memset(&in, 0, sizeof(in));
            in.seq = ++seq;
            in.delta = dt / (float) per_tick;
            in.move_dir = (signed char) move;
            in.y_dir = YAW_PLUS_X;
            in.jump = (unsigned char) (jump && k == (late && per_tick > 1 ? 1 : 0));
            in.crouch = crouch;
            player_queue_input(player, &in);
        }
        g_now += dt;
        game_tick(game, (float) g_now, dt);
        if (!player->active) break;
        st->distance += player->position.x - before.x;
        const float speed = hypotf(player->velocity.x, player->velocity.z);
        if (speed > st->speed_peak) st->speed_peak = speed;
        if (player->position.y > st->max_y) st->max_y = player->position.y;
        if (player->stop_crouch) st->bumps++;
        if (!on_ground_before && player->on_ground) {
            st->landings++;
            st->last_land_x = player->position.x;
        }
        const int contact = wall_stop(game, player, vbefore);
        if (contact && st->wall_contact_frame < 0) {
            st->wall_contact_frame = i;
            st->wall_contact_hopping = was_hopping;
            st->wall_contact_sliding = player->slide_timer > 0.0f;
        }
        if (edge_x > -1e8f && st->fell_frame < 0 && player->position.x > edge_x && !player->on_ground) {
            st->fell_frame = i;
            st->fell_hopping = was_hopping;
        }
        was_hopping = stub.move.hopping;
        if (st->wall_contact_frame >= 0 || st->fell_frame >= 0) break;
    }
    st->hops = stub.move.hops;
}

static void walk(Game *game, Player *player, const float start_x, const float seconds, const int hop, const float edge_x, WalkStats *st) {
    walk_at(game, player, start_x, seconds, hop, edge_x, HZ, st);
}

static void test_open_ground(Game *game) {
    printf("Running open ground hop test...\n");
    Player *p = add_human(game, 900, "Walker");
    WalkStats hop, plain;
    walk(game, p, -900.0f, 8.0f, 1, -1e9f, &hop);
    walk(game, p, -900.0f, 8.0f, 0, -1e9f, &plain);
    printf("  8 s: walking %.0f (peak %.3f u/ms), hopping %.0f (peak %.3f u/ms, %d hops, %d landings, head bumps %d, max rise %.1f, "
           "longest wait on the ground %d frames)\n",
           plain.distance, plain.speed_peak, hop.distance, hop.speed_peak, hop.hops, hop.landings, hop.bumps, hop.max_y, hop.frames_grounded_before_hop_max);
    CHECK(plain.distance > 100.0f, "the walker walks (%.0f)", plain.distance);
    CHECK(hop.distance > plain.distance * 1.25f, "hopping outruns walking (%.0f vs %.0f)", hop.distance, plain.distance);
    CHECK(hop.hops >= 10, "hop cadence: %d hops in 8 s", hop.hops);
    CHECK(hop.frames_grounded_before_hop_max <= 3, "a hop leaves the ground within 3 frames of landing (%d)", hop.frames_grounded_before_hop_max);
    CHECK(hop.bumps == 0, "no head bumps on open ground (%d)", hop.bumps);
    CHECK(hop.max_y < 16.0f, "a slide hop stays low (%.1f)", hop.max_y);
    // the server's 30 Hz tick: two frames per tick, the key released on the first and pressed on the second
    WalkStats slow;
    walk_at(game, p, -900.0f, 8.0f, 1, -1e9f, 30.0f, &slow);
    printf("  8 s at 30 Hz: hopping %.0f (%d hops, head bumps %d, longest wait on the ground %d ticks)\n", slow.distance, slow.hops, slow.bumps, slow.frames_grounded_before_hop_max);
    CHECK(slow.distance > plain.distance * 1.25f, "hopping outruns walking at 30 Hz too (%.0f vs %.0f)", slow.distance, plain.distance);
    CHECK(slow.hops >= 10, "hop cadence at 30 Hz: %d hops in 8 s", slow.hops);
    CHECK(slow.frames_grounded_before_hop_max <= 2, "a hop leaves the ground within 2 ticks of landing at 30 Hz (%d)", slow.frames_grounded_before_hop_max);
    CHECK(slow.bumps == 0, "no head bumps at 30 Hz (%d)", slow.bumps);
    p->active = 0;
}

static void test_ceiling(Game *game, const int full_tunnel, const int part_tunnel) {
    printf("Running ceiling test...\n");
    free_players(game);
    game_init(game, full_tunnel, 0, 1);
    Player *p = add_human(game, 900, "Walker");
    WalkStats st;
    walk(game, p, -900.0f, 6.0f, 1, -1e9f, &st);
    printf("  tunnel (14 over the feet): %d hops, %d head bumps, walked %.0f\n", st.hops, st.bumps, st.distance);
    CHECK(st.hops == 0, "no hop under a 14 ceiling (%d)", st.hops);
    CHECK(st.bumps == 0, "no head bump in the tunnel (%d)", st.bumps);
    CHECK(st.distance > 200.0f, "still walks the tunnel (%.0f)", st.distance);

    free_players(game);
    game_init(game, part_tunnel, 0, 1);
    p = add_human(game, 900, "Walker");
    walk(game, p, -900.0f, 10.0f, 1, -1e9f, &st);
    printf("  tunnel over -500..-300: %d hops, %d head bumps, walked %.0f, first hop at %.0f\n", st.hops, st.bumps, st.distance, st.first_hop_x);
    CHECK(st.hops >= 4, "hops before and after the tunnel (%d)", st.hops);
    CHECK(st.bumps == 0, "no head bump entering the tunnel (%d)", st.bumps);
    CHECK(st.distance > 550.0f, "through the tunnel and on (%.0f)", st.distance);
}

static void test_wall_and_ledge(Game *game, const int wall, const int ledge) {
    printf("Running wall / ledge test...\n");
    free_players(game);
    game_init(game, wall, 0, 1);
    Player *p = add_human(game, 900, "Walker");
    WalkStats st;
    walk(game, p, -900.0f, 10.0f, 1, -1e9f, &st);
    printf("  wall at -600: %d hops, contact at frame %d (hopping %d, sliding %d), last landing at %.0f\n",
           st.hops, st.wall_contact_frame, st.wall_contact_hopping, st.wall_contact_sliding, st.last_land_x);
    CHECK(st.hops >= 2, "hops on the way to the wall (%d)", st.hops);
    CHECK(st.wall_contact_frame >= 0, "the scripted walker reaches the wall");
    CHECK(!st.wall_contact_hopping, "no hop into the wall");
    CHECK(!st.wall_contact_sliding, "no slide into the wall");
    CHECK(st.last_land_x < -605.0f - 4.0f, "the last hop lands short of the wall (%.0f)", st.last_land_x);
    CHECK(st.bumps == 0, "no head bumps (%d)", st.bumps);

    free_players(game);
    game_init(game, ledge, 0, 1);
    p = add_human(game, 900, "Walker");
    walk(game, p, -900.0f, 10.0f, 1, -600.0f, &st);
    printf("  ledge at -600: %d hops, off the edge at frame %d (hopping %d), last landing at %.0f\n", st.hops, st.fell_frame, st.fell_hopping, st.last_land_x);
    CHECK(st.hops >= 2, "hops on the way to the ledge (%d)", st.hops);
    CHECK(st.fell_frame >= 0, "the scripted walker walks off the ledge");
    CHECK(!st.fell_hopping, "no hop off the ledge");
    CHECK(st.last_land_x < -600.0f - 4.0f, "the last hop lands on the floor (%.0f)", st.last_land_x);
}

// ---------------------------------------------------------------- the AI

typedef struct {
    int bots, hops, bumps, hop_contacts, hop_falls;
    int mistakes; // learning hoppers' hesitations + forgotten slides
    float travelled;
} SkillStats;

typedef struct {
    int hops, bumps, contacts, hop_contacts, falls, hop_falls, ai_refused, vetoes;
    int refused[BOT_MOVE_NO_CHANCE + 1];
    float travelled;
    SkillStats skill[BOT_HOP_SKILLS];
} AiStats;

// AI runs per test (3; BOT_MOVE_TEST_SEEDS=N for a longer soak)
static int ai_seeds(void) {
    const char *env = getenv("BOT_MOVE_TEST_SEEDS");
    return env && atoi(env) > 0 ? atoi(env) : 3;
}

static void print_refusals(const AiStats *st) {
    printf("  hops refused:");
    for (int r = 1; r <= BOT_MOVE_NO_CHANCE; r++) if (st->refused[r]) printf(" %s %d", bot_move_reason_name(r), st->refused[r]);
    printf("\n");
}

static void ai_run(Game *game, const float seconds, const float floor_y, AiStats *st) {
    memset(st, 0, sizeof(*st));
    vec3 prev_pos[GAME_BOTS_MAX], prev_vel[GAME_BOTS_MAX];
    int prev_active[GAME_BOTS_MAX], prev_fell[GAME_BOTS_MAX], prev_hop[GAME_BOTS_MAX];
    memset(prev_active, 0, sizeof(prev_active));
    memset(prev_fell, 0, sizeof(prev_fell));
    memset(prev_hop, 0, sizeof(prev_hop));
    memset(prev_pos, 0, sizeof(prev_pos));
    memset(prev_vel, 0, sizeof(prev_vel));
    const int frames = (int) (seconds * HZ);
    for (int i = 0; i < frames; i++) {
        tick(game);
        for (int b = 0; b < game->bots->count; b++) {
            const GameBot *bot = &game->bots->list[b];
            const Player *p = bot->player;
            if (!p || !p->active) {
                prev_active[b] = 0;
                continue;
            }
            if (prev_active[b] && hypotf(p->position.x - prev_pos[b].x, p->position.z - prev_pos[b].z) < 50.0f) {
                SkillStats *k = &st->skill[bot->move.skill % BOT_HOP_SKILLS];
                const float step = hypotf(p->position.x - prev_pos[b].x, p->position.z - prev_pos[b].z);
                st->travelled += step;
                k->travelled += step;
                const int contact = wall_stop(game, p, prev_vel[b]);
                if (contact) {
                    st->contacts++;
                    if (prev_hop[b] || p->slide_timer > 0.0f) {
                        st->hop_contacts++;
                        k->hop_contacts++;
                    }
                }
                if (p->stop_crouch) {
                    st->bumps++;
                    k->bumps++;
                }
                const int fell = p->position.y < floor_y - 2.0f;
                if (fell && !prev_fell[b]) {
                    st->falls++;
                    if (prev_hop[b]) {
                        st->hop_falls++;
                        k->hop_falls++;
                    }
                }
                prev_fell[b] = fell;
            }
            prev_active[b] = 1;
            prev_pos[b] = p->position;
            prev_vel[b] = p->velocity;
            prev_hop[b] = bot->move.hopping;
        }
    }
    for (int b = 0; b < game->bots->count; b++) {
        const GameBot *bot = &game->bots->list[b];
        CHECK(bot->move.skill == bot_move_skill(bot), "%s keeps its rolled skill (%d, %d now; active %d)", bot->name, bot->move.skill, bot_move_skill(bot), bot->player && bot->player->active);
        st->skill[bot->move.skill % BOT_HOP_SKILLS].bots++;
        st->skill[bot->move.skill % BOT_HOP_SKILLS].hops += bot->move.hops;
        st->skill[bot->move.skill % BOT_HOP_SKILLS].mistakes += bot->move.hesitations + bot->move.slides_forgotten;
        st->hops += game->bots->list[b].move.hops;
        st->ai_refused += game->bots->list[b].move.ai_jumps_refused;
        st->vetoes += game->bots->list[b].move.vetoes;
        for (int r = 0; r <= BOT_MOVE_NO_CHANCE; r++) st->refused[r] += game->bots->list[b].move.refused[r];
    }
}

static void test_ai_corridor(Game *game, const int corridor) {
    printf("Running AI corridor test...\n");
    AiStats sum;
    memset(&sum, 0, sizeof(sum));
    for (int seed = 1; seed <= ai_seeds(); seed++) {
        free_players(game);
        game_init(game, corridor, 0, 1);
        CHECK(game->ready, "corridor loaded");
        pcg32_srandom((uint64_t) seed, 54u);
        // no fights: a NextBot targets bots whatever targetBots says (bots.c bot_perceive), and the
        // hops of two bots that meet and fight in the corridor measured the fight, not the walk
        run("/bot_add 2 difficulty=hard targetBots=false attackTarget=false watchTarget=false followTarget=false");
        AiStats st;
        ai_run(game, 40.0f, 0.0f, &st);
        printf("  seed %d, 40 s, 2 bots: %d hops, %d head bumps, %d wall contacts (%d in a hop / slide), %d jumps refused, %d hops refused, travelled %.0f\n",
               seed, st.hops, st.bumps, st.contacts, st.hop_contacts, st.ai_refused, st.vetoes, st.travelled);
        print_refusals(&st);
        CHECK(st.bumps == 0, "no head bumps (%d)", st.bumps);
        CHECK(st.hop_contacts == 0, "no wall contact in a hop or its slide (%d)", st.hop_contacts);
        sum.hops += st.hops;
        sum.travelled += st.travelled;
        sum.contacts += st.contacts;
        run("/bot_kick all");
    }
    printf("  3 runs: %d hops, travelled %.0f, %d wall contacts while walking\n", sum.hops, sum.travelled, sum.contacts);
    CHECK(sum.hops >= 40, "the bots hop along the corridor (%d in 3 x 40 s)", sum.hops);
    CHECK(sum.travelled > 3000.0f, "the bots travel (%.0f)", sum.travelled);
}

// ---------------------------------------------------------------- hop skills

static void test_skill_mix(void) {
    printf("Running hop skill mix test...\n");
    static const float k_diff[4] = {BOT_DIFFICULTY_EASY, BOT_DIFFICULTY_NORMAL, BOT_DIFFICULTY_HARD, BOT_DIFFICULTY_MAX};
    static const char *const k_name[4] = {"easy", "normal", "hard", "max"};
    static const float k_want[4][BOT_HOP_SKILLS] = {{0.75f, 0.25f, 0.0f}, {0.30f, 0.35f, 0.35f}, {0.0f, 0.15f, 0.85f}, {0.0f, 0.0f, 1.0f}};
    const int n = 4000;
    int monotonic = 1;
    for (int d = 0; d < 4; d++) {
        int count[BOT_HOP_SKILLS] = {0};
        for (int i = 0; i < n; i++) {
            char name[32];
            snprintf(name, sizeof(name), "Bot%d", i * 7919);
            const uint32_t seed = bot_move_seed(name, 1 + i % GAME_BOTS_MAX);
            const int skill = bot_move_skill_roll(seed, k_diff[d]);
            count[skill]++;
            if (d && skill < bot_move_skill_roll(seed, k_diff[d - 1])) monotonic = 0; // a bot never gets worse with difficulty
        }
        printf("  %-6s: none %4.1f%%, learning %4.1f%%, clean %4.1f%% (%d seeds)\n", k_name[d], 100.0f * (float) count[0] / (float) n,
               100.0f * (float) count[1] / (float) n, 100.0f * (float) count[2] / (float) n, n);
        for (int k = 0; k < BOT_HOP_SKILLS; k++) {
            const float got = (float) count[k] / (float) n;
            CHECK(fabsf(got - k_want[d][k]) <= 0.03f, "%s: %s share %.3f (want %.2f)", k_name[d], bot_move_skill_name(k), got, k_want[d][k]);
        }
    }
    CHECK(k_want[0][2] == 0.0f && k_want[3][0] == 0.0f, "easy has no clean hoppers, max no walkers");
    CHECK(monotonic, "a bot's skill never drops as the difficulty rises");
}

// bot_list shows every bot's skill; a bot keeps it across a respawn (the seed is its name + uid)
static void test_skill_console(Game *game, const int runway_index) {
    printf("Running hop skill console test...\n");
    free_players(game);
    game_init(game, runway_index, 0, 1);
    pcg32_srandom(7u, 54u);
    run("/bot_add 4 difficulty=normal targetBots=false"); // one console line holds 4
    tick(game);
    char list[1024];
    run("/bot_list");
    snprintf(list, sizeof(list), "%s", g_reply);
    int shown = 0, have[BOT_HOP_SKILLS] = {0};
    for (const char *p = list; (p = strstr(p, " bhop:")); p++) shown++;
    for (int b = 0; b < game->bots->count; b++) {
        const GameBot *bot = &game->bots->list[b];
        char want[64];
        snprintf(want, sizeof(want), "%s#%d bhop:%s", bot->name, bot->uid, bot_move_skill_name(bot->move.skill));
        CHECK(strstr(list, want) != NULL, "bot_list shows %s", want);
        have[bot->move.skill]++;
    }
    printf("  4 normal bots: %d none, %d learning, %d clean (%s)\n", have[0], have[1], have[2], list);
    CHECK(shown == game->bots->count, "a bhop field per bot (%d of %d)", shown, game->bots->count);
    const GameBot *first = &game->bots->list[0];
    const int skill = first->move.skill;
    run("/bot_kill all");
    for (int i = 0; i < 6 * 60 && !(first->player && first->player->active); i++) tick(game);
    tick(game);
    CHECK(first->player && first->player->active && first->move.skill == skill, "the skill survives a respawn (%d -> %d)", skill, first->move.skill);
    run("/bot_config slideHop=false");
    run("/bot_list");
    CHECK(strstr(g_reply, "bhop:off") != NULL, "slideHop off shows as off (%s)", g_reply);
    run("/bot_config slideHop=true"); // the manager default again (later tests)
    run("/bot_kick all");
}

static float average_walk(Game *game, const int skill, const int runs, WalkStats *last) {
    float total = 0.0f;
    g_walk_skill = skill;
    for (int r = 0; r < runs; r++) {
        free_players(game);
        game_init(game, 0, 0, 1);
        Player *p = add_human(game, 900, "Walker");
        pcg32_srandom((uint64_t) (100 + r), 54u);
        walk(game, p, -900.0f, 8.0f, skill >= 0, -1e9f, last);
        total += last->distance;
        CHECK(last->bumps == 0, "%s: no head bumps on open ground (%d)", skill >= 0 ? bot_move_skill_name(skill) : "walker", last->bumps);
    }
    g_walk_skill = -1;
    return total / (float) runs;
}

static void test_skill_speed(Game *game) {
    printf("Running hop skill speed test...\n");
    WalkStats st;
    const float walker = average_walk(game, PLAIN_WALKER, 4, &st);
    const float none = average_walk(game, BOT_HOP_NONE, 4, &st);
    const int none_hops = st.hops;
    const float learning = average_walk(game, BOT_HOP_LEARNING, 4, &st);
    const int learning_hops = st.hops, learning_wait = st.frames_grounded_before_hop_max;
    const float clean = average_walk(game, BOT_HOP_CLEAN, 4, &st);
    const int clean_wait = st.frames_grounded_before_hop_max;
    printf("  8 s on open ground (4 runs each): walker %.0f, none %.0f (%d hops), learning %.0f (%d hops, longest wait %d frames), clean %.0f (longest wait %d frames)\n",
           walker, none, none_hops, learning, learning_hops, learning_wait, clean, clean_wait);
    CHECK(none_hops == 0, "a non-hopper never hops (%d)", none_hops);
    CHECK(fabsf(none - walker) < walker * 0.05f, "a non-hopper walks like a walker (%.0f vs %.0f)", none, walker);
    CHECK(learning > walker * 1.1f, "a learning hopper outruns a walker (%.0f vs %.0f)", learning, walker);
    CHECK(learning < clean * 0.9f, "a learning hopper is slower than a clean one (%.0f vs %.0f)", learning, clean);
    CHECK(learning_wait > clean_wait, "a learning hopper waits on the ground longer (%d vs %d frames)", learning_wait, clean_wait);
}

// The ceiling, wall and ledge runs for every skill: nobody bumps, hops into the wall or off the edge.
static void test_skill_safety(Game *game, const int tunnel, const int wall, const int ledge) {
    printf("Running hop skill safety test...\n");
    for (int skill = 0; skill < BOT_HOP_SKILLS; skill++) {
        g_walk_skill = skill;
        const char *name = bot_move_skill_name(skill);
        for (int r = 0; r < 3; r++) {
            WalkStats st;
            pcg32_srandom((uint64_t) (200 + r), 54u);
            free_players(game);
            game_init(game, tunnel, 0, 1);
            Player *p = add_human(game, 900, "Walker");
            walk(game, p, -900.0f, 10.0f, 1, -1e9f, &st);
            CHECK(st.bumps == 0, "%s: no head bump at the tunnel (%d)", name, st.bumps);

            free_players(game);
            game_init(game, wall, 0, 1);
            p = add_human(game, 900, "Walker");
            walk(game, p, -900.0f, 10.0f, 1, -1e9f, &st);
            CHECK(st.wall_contact_frame >= 0 && !st.wall_contact_hopping && !st.wall_contact_sliding, "%s: no hop / slide into the wall (frame %d, hopping %d, sliding %d)",
                  name, st.wall_contact_frame, st.wall_contact_hopping, st.wall_contact_sliding);
            CHECK(st.bumps == 0, "%s: no head bumps at the wall (%d)", name, st.bumps);

            free_players(game);
            game_init(game, ledge, 0, 1);
            p = add_human(game, 900, "Walker");
            walk(game, p, -900.0f, 10.0f, 1, -600.0f, &st);
            CHECK(st.fell_frame >= 0 && !st.fell_hopping, "%s: no hop off the ledge (frame %d, hopping %d)", name, st.fell_frame, st.fell_hopping);
            if (skill == BOT_HOP_NONE) CHECK(st.hops == 0, "none: no hops (%d)", st.hops);
        }
    }
    g_walk_skill = -1;
}

// The AI at normal difficulty: all three skills in one lobby
static void test_ai_skills(Game *game, const int corridor) {
    printf("Running AI hop skill test...\n");
    AiStats sum;
    memset(&sum, 0, sizeof(sum));
    for (int seed = 1; seed <= ai_seeds(); seed++) {
        free_players(game);
        game_init(game, corridor, 0, 1);
        pcg32_srandom((uint64_t) (40 + seed), 54u);
        run("/bot_add 6 difficulty=normal targetBots=false");
        AiStats st;
        ai_run(game, 30.0f, 0.0f, &st);
        for (int k = 0; k < BOT_HOP_SKILLS; k++) {
            sum.skill[k].bots += st.skill[k].bots;
            sum.skill[k].hops += st.skill[k].hops;
            sum.skill[k].bumps += st.skill[k].bumps;
            sum.skill[k].hop_contacts += st.skill[k].hop_contacts;
            sum.skill[k].travelled += st.skill[k].travelled;
            sum.skill[k].mistakes += st.skill[k].mistakes;
        }
        run("/bot_kick all");
    }
    for (int k = 0; k < BOT_HOP_SKILLS; k++) {
        const SkillStats *s = &sum.skill[k];
        printf("  %-8s: %2d bots, %3d hops, %d mistakes, %d head bumps, %d wall contacts in a hop / slide, travelled %.0f\n", bot_move_skill_name(k), s->bots, s->hops,
               s->mistakes, s->bumps, s->hop_contacts, s->travelled);
        CHECK(s->bots > 0, "3 x 6 normal bots include %s hoppers", bot_move_skill_name(k));
        CHECK(s->bumps == 0, "%s: no head bumps (%d)", bot_move_skill_name(k), s->bumps);
        CHECK(s->hop_contacts == 0, "%s: no wall contact in a hop or its slide (%d)", bot_move_skill_name(k), s->hop_contacts);
    }
    CHECK(sum.skill[BOT_HOP_NONE].hops == 0, "non-hoppers never hop (%d)", sum.skill[BOT_HOP_NONE].hops);
    CHECK(sum.skill[BOT_HOP_LEARNING].mistakes > 0, "learning hoppers hesitate / forget the slide (%d)", sum.skill[BOT_HOP_LEARNING].mistakes);
    CHECK(sum.skill[BOT_HOP_CLEAN].mistakes == 0, "clean hoppers make no such mistakes (%d)", sum.skill[BOT_HOP_CLEAN].mistakes);
    CHECK(sum.skill[BOT_HOP_LEARNING].hops > 0 && sum.skill[BOT_HOP_CLEAN].hops > 0, "learning and clean bots hop (%d, %d)", sum.skill[BOT_HOP_LEARNING].hops,
          sum.skill[BOT_HOP_CLEAN].hops);
}

static void test_ai_plateau(Game *game, const int plateau) {
    printf("Running AI plateau test...\n");
    int hops = 0;
    for (int seed = 1; seed <= ai_seeds(); seed++) {
        free_players(game);
        game_init(game, plateau, 0, 1);
        CHECK(game->ready, "plateau loaded");
        pcg32_srandom((uint64_t) seed, 54u);
        run("/bot_add 3 difficulty=max targetBots=false");
        AiStats st;
        ai_run(game, 30.0f, 30.0f, &st);
        printf("  seed %d, 30 s, 3 bots: %d hops, %d falls (%d from a hop), %d head bumps, %d hops refused, travelled %.0f\n",
               seed, st.hops, st.falls, st.hop_falls, st.bumps, st.vetoes, st.travelled);
        print_refusals(&st);
        CHECK(st.hop_falls == 0, "no hop off the plateau (%d)", st.hop_falls);
        CHECK(st.bumps == 0, "no head bumps (%d)", st.bumps);
        hops += st.hops;
        run("/bot_kick all");
    }
    CHECK(hops >= 15, "the bots hop on the plateau (%d in 3 x 30 s)", hops);
}

int main(void) {
    static const cJSON *maps[7];
    maps[0] = runway("");
    maps[1] = runway("{\"p\":[0,14,0],\"s\":[2000,6,2000]}");                        // a ceiling over the whole runway
    maps[2] = runway("{\"p\":[-400,14,0],\"s\":[200,6,2000]}");                      // over x -500..-300
    maps[3] = runway("{\"p\":[-600,0,0],\"s\":[10,40,2000]}");                       // a wall across at -600
    maps[4] = cJSON_Parse("{\"name\":\"Ledge\",\"spawns\":[[-900,0,0,0,0,0],[-700,0,0,0,0,0]],\"objects\":[{\"p\":[-750,-10,0],\"s\":[300,10,2000]}]}");
    maps[5] = cJSON_Parse(k_corridor);
    maps[6] = cJSON_Parse(k_plateau);
    for (int i = 0; i < 7; i++) {
        if (!maps[i]) {
            printf("map %d failed to parse\n", i);
            return 1;
        }
    }
    static int modes[7];
    for (int i = 0; i < 7; i++) modes[i] = MODE_ID_FFA;
    Game game = {0};
    game_configure(&game, NULL, maps, 7, modes, 7, NULL, 0, NULL, 0);
    game.hooks.send = test_send;
    game.round_length = 100000.0f;
    game_init(&game, 0, 0, 1);
    if (!game.ready) {
        printf("Failed to init the game\n");
        return 1;
    }
    g_host.game = &game;
    g_host.log = host_log;

    test_skill_mix();
    test_open_ground(&game);
    test_skill_speed(&game);
    test_skill_console(&game, 0);
    test_skill_safety(&game, 2, 3, 4);
    test_ceiling(&game, 1, 2);
    test_wall_and_ledge(&game, 3, 4);
    test_ai_corridor(&game, 5);
    test_ai_skills(&game, 5);
    test_ai_plateau(&game, 6);

    if (g_failures) {
        printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("Test OK\n");
    return 0;
}
