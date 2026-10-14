// KrunkNative spectate overlay (module bots-spectate-overlay): the NET_EXT_KN_BOT_PATH body
// (shared/src/bot_path_net.c: fill from a GameBot, decode on the client, limits) and the GL-free
// overlay geometry (client/src/nav_overlay_geom.c: route polyline, jump arcs, goal / target /
// last-known markers). No map, no GL: a hand-made Game with two players.
#include <bot_path_net.h>
#include <bot_path.h>
#include <nav_overlay_geom.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;
#define CHECK(cond, ...) do { \
    if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_failures++; } \
} while (0)

static Player g_bot_player, g_target_player;
static Player *g_players[2] = {&g_bot_player, &g_target_player};
static Game g_game;

static GameBot *make_bot(const int points, const int reached) {
    GameBot *bot = calloc(1, sizeof(GameBot));
    bot->uid = 7;
    bot->player = &g_bot_player;
    bot->state = BOT_STATE_HUNT;
    bot->target_uid = -1;
    for (int i = 0; i < points && i < BOT_PATH_MAX; i++) {
        bot->path.points[i] = (vec3) {10.0f * (float) (i + 1), i == 2 ? 8.0f : 0.0f, 5.0f * (float) i};
        bot->path.kinds[i] = i == 2 ? BOT_PATH_JUMP : BOT_PATH_WALK;
    }
    bot->path.count = points;
    bot->path.reached = reached;
    bot->path_goal = points ? bot->path.points[points - 1] : (vec3) {0};
    bot->follow.has_goal = points > 0; // bot_path_goal() (bots-nav) reads the follower's goal
    bot->follow.goal = bot->path_goal;
    return bot;
}

static void test_fill_decode(void) {
    printf("Running bot path packet test...\n");
    GameBot *bot = make_bot(4, 1);
    NetBotPathPacket sent, got;
    const uint16_t body = bot_path_packet_fill(&g_game, bot, &sent);
    CHECK(body == NET_BOT_PATH_SIZE(5), "body length %u for 4 segments (%zu)", body, NET_BOT_PATH_SIZE(5));
    CHECK(sent.bot_sid == 7 && sent.n == 5 && sent.target_sid == -1, "sid %d, %d points, target %d", sent.bot_sid, sent.n, sent.target_sid);
    CHECK(!strcmp(sent.state_name, "Hunt") && sent.state == BOT_STATE_HUNT, "state %s", sent.state_name);
    CHECK((sent.flags & NET_BOT_PATH_ACTIVE) && (sent.flags & NET_BOT_PATH_HAS_GOAL), "alive with a goal (flags %u)", sent.flags);
    CHECK(!(sent.flags & (NET_BOT_PATH_HAS_TARGET | NET_BOT_PATH_HAS_LAST_KNOWN | NET_BOT_PATH_PARTIAL | NET_BOT_PATH_TRUNCATED)), "no target, complete (flags %u)", sent.flags);
    CHECK(sent.points[0].x == g_bot_player.position.x && sent.points[0].z == g_bot_player.position.z, "point 0 is the bot's feet");
    CHECK(sent.points[3].kind == BOT_PATH_JUMP && sent.points[3].y == 8.0f, "the jump segment's kind rides on its end point");
    CHECK(sent.goal[0] == 40.0f && sent.goal[2] == 15.0f, "goal is the route's end (%.0f %.0f)", sent.goal[0], sent.goal[2]);
    CHECK((sent.points[4].flags & BOT_PATH_FLAG_LAST), "the last point carries BOT_PATH_FLAG_LAST");

    CHECK(bot_path_packet_decode(&sent, body, &got), "decodes");
    CHECK(!memcmp(&got, &sent, NET_BOT_PATH_SIZE(5)), "round trip identical");
    CHECK(got.points[5].x == 0.0f && got.points[NET_BOT_PATH_POINTS_MAX - 1].x == 0.0f, "points past n are zero");
    CHECK(!bot_path_packet_decode(&sent, (uint16_t) (body - 1), &got), "a body cut inside a point is refused");
    CHECK(!bot_path_packet_decode(&sent, (uint16_t) (NET_BOT_PATH_SIZE(0) - 1), &got), "a body shorter than the head is refused");
    NetBotPathPacket bad = sent;
    bad.n = NET_BOT_PATH_POINTS_MAX + 1;
    CHECK(!bot_path_packet_decode(&bad, (uint16_t) sizeof(bad), &got), "n over the maximum is refused");
    CHECK(bot_path_packet_decode(&sent, (uint16_t) sizeof(sent), &got) && got.n == 5, "a full-size body decodes (n %d)", got.n);

    // a target in sight, seen before: target + last known
    bot->target_uid = 9;
    bot->sees_target = 1;
    bot->have_seen = 1;
    bot->last_known = (vec3) {1.0f, 2.0f, 3.0f};
    bot->state = BOT_STATE_ATTACK;
    bot_path_packet_fill(&g_game, bot, &sent);
    CHECK(sent.target_sid == 9 && (sent.flags & NET_BOT_PATH_HAS_TARGET) && (sent.flags & NET_BOT_PATH_SEES_TARGET), "target 9 seen (flags %u)", sent.flags);
    CHECK(sent.target[0] == g_target_player.position.x && sent.target[1] == g_target_player.position.y, "target feet from the player");
    CHECK((sent.flags & NET_BOT_PATH_HAS_LAST_KNOWN) && sent.last_known[2] == 3.0f, "last known position");
    CHECK(!strcmp(sent.state_name, "Attack"), "state %s", sent.state_name);
    bot->target_uid = 99; // left: no player, memory kept
    bot_path_packet_fill(&g_game, bot, &sent);
    CHECK(!(sent.flags & NET_BOT_PATH_HAS_TARGET) && (sent.flags & NET_BOT_PATH_HAS_LAST_KNOWN), "a gone target: memory only (flags %u)", sent.flags);
    bot_path_packet_fill(NULL, bot, &sent); // no game: no lookup
    CHECK(!(sent.flags & NET_BOT_PATH_HAS_TARGET), "no game, no target lookup");

    // a long partial route is truncated to what fits, flagged
    free(bot);
    bot = make_bot(40, 0);
    const uint16_t long_body = bot_path_packet_fill(&g_game, bot, &sent);
    CHECK(sent.n == NET_BOT_PATH_POINTS_MAX && (sent.flags & NET_BOT_PATH_TRUNCATED), "40 segments: %d points, truncated (flags %u)", sent.n, sent.flags);
    CHECK((sent.flags & NET_BOT_PATH_PARTIAL), "an unreached route is partial (flags %u)", sent.flags);
    CHECK(long_body == NET_BOT_PATH_SIZE(NET_BOT_PATH_POINTS_MAX) && long_body <= NET_EXT_BODY_MAX, "body %u fits an EXT body (%d)", long_body, NET_EXT_BODY_MAX);
    CHECK(bot_path_packet_decode(&sent, long_body, &got) && got.n == NET_BOT_PATH_POINTS_MAX, "the full body decodes");

    // a route already walked to its end: no points, the goal stays
    bot->path_index = bot->path.count;
    bot_path_packet_fill(&g_game, bot, &sent);
    CHECK(sent.n == 0 && (sent.flags & NET_BOT_PATH_HAS_GOAL), "walked route: no points, goal kept (n %d flags %u)", sent.n, sent.flags);
    CHECK(bot_path_packet_fill(&g_game, bot, &sent) == NET_BOT_PATH_SIZE(0), "head-only body");

    // dead: state only
    g_bot_player.active = 0;
    bot_path_packet_fill(&g_game, bot, &sent);
    CHECK(!(sent.flags & NET_BOT_PATH_ACTIVE) && sent.n == 0, "a dead bot sends its state, no route");
    g_bot_player.active = 1;
    bot->player = NULL;
    CHECK(bot_path_packet_fill(&g_game, bot, &sent) == 0, "no player: nothing to send");
    free(bot);
}

static int lines_ok(const NavGeomVerts *v) { return v->n % 6 == 0; } // GL_LINES: 2 vertices x 3 floats

static int has_vertex(const NavGeomVerts *v, const float x, const float y, const float z) {
    for (size_t i = 0; i + 2 < v->n; i += 3) {
        if (fabsf(v->v[i] - x) < 1e-4f && fabsf(v->v[i + 1] - y) < 1e-4f && fabsf(v->v[i + 2] - z) < 1e-4f) return 1;
    }
    return 0;
}

static void test_geometry(void) {
    printf("Running bot overlay geometry test...\n");
    NavBotGeom geom;
    memset(&geom, 0, sizeof(geom));
    CHECK(nav_bot_geom_build(NULL, NULL, NULL, &geom) == 0, "no packet: nothing");

    GameBot *bot = make_bot(4, 1);
    bot->target_uid = 9;
    bot->sees_target = 1;
    bot->have_seen = 1;
    bot->last_known = (vec3) {200.0f, 0.0f, 200.0f};
    NetBotPathPacket p;
    bot_path_packet_fill(&g_game, bot, &p);

    const vec3 live_feet = {3.0f, 1.0f, 4.0f}, live_target = {50.0f, 2.0f, 60.0f};
    const size_t total = nav_bot_geom_build(&p, &live_feet, &live_target, &geom);
    CHECK(total > 0, "vertices %zu", total);
    for (int i = 0; i < NAV_BOT_LAYERS; i++) CHECK(lines_ok(&geom.layers[i]), "layer %d is GL_LINES pairs (%zu floats)", i, geom.layers[i].n);
    const NavGeomVerts *path = &geom.layers[NAV_BOT_PATH], *jump = &geom.layers[NAV_BOT_JUMP];
    const NavGeomVerts *goal = &geom.layers[NAV_BOT_GOAL], *target = &geom.layers[NAV_BOT_TARGET], *lkp = &geom.layers[NAV_BOT_LAST_KNOWN];
    CHECK(path->n && jump->n && goal->n && target->n && lkp->n, "every layer drew (%zu %zu %zu %zu %zu)", path->n, jump->n, goal->n, target->n, lkp->n);
    CHECK(has_vertex(path, 3.0f, 1.0f + NAV_BOT_PATH_LIFT, 4.0f), "the polyline starts at the live feet, lifted");
    CHECK(has_vertex(path, 10.0f, NAV_BOT_PATH_LIFT, 0.0f) && has_vertex(path, 10.0f, NAV_BOT_TICK_HEIGHT, 0.0f), "waypoint 1 and its tick");
    CHECK(has_vertex(path, 40.0f, NAV_BOT_PATH_LIFT, 15.0f), "the polyline reaches the route's end");
    // the jump segment (point 2 -> point 3) is an arc in the jump layer, not a straight path line
    CHECK(has_vertex(jump, 30.0f, 8.0f + NAV_BOT_PATH_LIFT, 10.0f) && has_vertex(jump, 20.0f, NAV_BOT_PATH_LIFT, 5.0f), "the arc joins the jump's ends");
    CHECK(jump->n == 4 * 6, "4 arc pieces (%zu floats)", jump->n);
    int apex = 0;
    for (size_t i = 1; i < jump->n; i += 3) apex |= jump->v[i] > 8.0f + NAV_BOT_PATH_LIFT + 1.0f;
    CHECK(apex, "the arc rises over the segment");
    CHECK(has_vertex(goal, 40.0f, 15.0f + 0.0f + NAV_BOT_GOAL_POLE + NAV_BOT_GOAL_RADIUS - NAV_BOT_GOAL_POLE - NAV_BOT_GOAL_RADIUS + NAV_BOT_GOAL_POLE, 15.0f) ||
          has_vertex(goal, 40.0f, NAV_BOT_GOAL_POLE, 15.0f), "the goal pole top");
    CHECK(has_vertex(goal, 40.0f, 0.0f, 15.0f), "the goal pole foot");
    CHECK(has_vertex(target, 50.0f + NAV_BOT_TARGET_RADIUS, 2.5f, 60.0f + NAV_BOT_TARGET_RADIUS), "the target ring around the live target");
    CHECK(has_vertex(target, 3.0f, 1.0f + NAV_BOT_EYE, 4.0f) && has_vertex(target, 50.0f, 2.0f + NAV_BOT_TARGET_CHEST, 60.0f), "the sight line eye -> chest");
    CHECK(has_vertex(lkp, 200.0f, 0.0f, 200.0f) && has_vertex(lkp, 200.0f, NAV_BOT_LKP_POLE, 200.0f), "the last-known pole");

    // the target steps out of sight: no sight line, the memory marker stays
    p.flags &= (uint8_t) ~NET_BOT_PATH_SEES_TARGET;
    const size_t sight_floats = target->n;
    nav_bot_geom_build(&p, &live_feet, &live_target, &geom);
    CHECK(target->n == sight_floats - 6 && lkp->n, "no sight line out of sight (%zu -> %zu), memory kept", sight_floats, target->n);

    // seen right where it was last seen: the memory marker is redundant
    p.flags |= NET_BOT_PATH_SEES_TARGET;
    p.last_known[0] = 50.0f;
    p.last_known[1] = 2.0f;
    p.last_known[2] = 60.0f;
    nav_bot_geom_build(&p, &live_feet, &live_target, &geom);
    CHECK(lkp->n == 0, "no last-known marker on a seen target (%zu)", lkp->n);

    // no live positions: the packet's own
    nav_bot_geom_build(&p, NULL, NULL, &geom);
    CHECK(has_vertex(path, g_bot_player.position.x, g_bot_player.position.y + NAV_BOT_PATH_LIFT, g_bot_player.position.z), "polyline from the packet's point 0");
    CHECK(has_vertex(target, p.target[0] + NAV_BOT_TARGET_RADIUS, p.target[1] + 0.5f, p.target[2] + NAV_BOT_TARGET_RADIUS), "ring from the packet's target");

    // dead: no route, no sight line; goal and memory still drawn
    p.flags &= (uint8_t) ~NET_BOT_PATH_ACTIVE;
    p.last_known[0] = 200.0f;
    nav_bot_geom_build(&p, &live_feet, &live_target, &geom);
    CHECK(path->n == 0 && jump->n == 0 && goal->n && lkp->n, "dead: no polyline (%zu %zu), markers (%zu %zu)", path->n, jump->n, goal->n, lkp->n);

    // a single point (no segment) draws no polyline
    p.flags |= NET_BOT_PATH_ACTIVE;
    p.n = 1;
    nav_bot_geom_build(&p, &live_feet, &live_target, &geom);
    CHECK(path->n == 0, "one point: no polyline");

    nav_bot_geom_free(&geom);
    CHECK(geom.layers[0].v == NULL && geom.layers[0].cap == 0, "freed");
    free(bot);
}

int main(void) {
    memset(&g_game, 0, sizeof(g_game));
    memset(&g_bot_player, 0, sizeof(g_bot_player));
    memset(&g_target_player, 0, sizeof(g_target_player));
    g_bot_player.uid = 7;
    g_bot_player.active = 1;
    g_bot_player.position = (vec3) {1.0f, 0.0f, 2.0f};
    g_target_player.uid = 9;
    g_target_player.active = 1;
    g_target_player.position = (vec3) {100.0f, 4.0f, 120.0f};
    g_game.players = g_players;
    g_game.player_count = 2;

    test_fill_decode();
    test_geometry();
    if (g_failures) {
        printf("%d failure(s)\n", g_failures);
        return 1;
    }
    printf("All nav overlay tests passed\n");
    return 0;
}
