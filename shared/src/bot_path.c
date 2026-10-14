// The route a bot player follows (shared/include/bot_path.h): the follower over the A* grid of
// shared/src/bot_nav.c, used by bots.c's walking (bot_route / bot_move_toward / bot_walk).
//
// Locomotion (the idea of Krunker v7.1.4 bot/nextbot/body.ts + bot/ai/state.ts findPath,
// KrunkNative's own geometry):
// - the route: A* over the grid with jump / drop links, replanned when the goal moves, when
//   the bot drifts off it, when it stops making progress, and every few seconds as a safety;
//   at most GameBots.path_budget searches per tick, the old route serves meanwhile;
// - following: waypoints are consumed when reached or passed; the bot pursues the farthest of
//   the next two waypoints it can walk to in a straight line (corner cutting), never across a
//   jump or a drop;
// - local avoidance: the move code is chosen among the wanted heading and its neighbours by
//   sweeping the body along each (bot_nav_sweep): walls and overhangs are refused, a low
//   obstacle is jumped only where the segment allows it, the ground ahead must carry the body
//   (no walking off an edge the route does not drop from), other players in the way are
//   side-stepped;
// - stuck handling: no progress toward the waypoint for BP_BLOCKED_MS (0.8 s) first forces a
//   replan that routes around the spot (bot_nav_penalize), a second one in a row (at the same
//   spot) is a stuck recovery (back off, hop, give the goal up) and counts in BotNavStats.stuck;
// - hops (a low obstacle, a jump link) only once the run-up heads at the ledge (a hop at speed
//   sideways flies past it, off the far side of a walkway); airborne, the bot air-steers at the
//   point without judging progress;
// - the guard (bot_path_guard, after every state's movement): no move or hop that ends over the
//   void, a drift toward a void edge brakes (FRVR Strike's SkyTemple lost 227 bots off its
//   island in 30 s to the old straight-steer walking).
#include <bot_path.h>
#include <pcg_basic.h>
#include <math.h>
#include <string.h>

#define BP_PI 3.14159265358979323846f
#define BP_STEP 3.5f                  // climb height (+ tolerance), bot_nav.c NAV_STEP
#define BP_HEIGHT 11.0f               // standing body height, NAV_HEIGHT
#define BP_JUMP 8.5f                  // the highest ledge a standing jump reaches, NAV_JUMP
#define BP_DROP 64.0f                 // the deepest drop a link takes, NAV_DROP
#define BP_REACH_MIN 6.0f             // the sweep ahead at a standstill
#define BP_REACH_MS 160.0f            // ... plus this much travel at the current speed
#define BP_WAYPOINT_RADIUS 4.0f       // a waypoint counts as reached within it
#define BP_LOOKAHEAD 18.0f            // corner cutting: pursue the next waypoint from this far
#define BP_OFF_ROUTE 30.0f            // farther than this beside the segment walked: replan
#define BP_REPLAN_MIN_MS 4000.0f      // the periodic safety replan
#define BP_REPLAN_MAX_MS 6000.0f
#define BP_BLOCKED_MS 800.0f          // no progress this long while walking: blocked
#define BP_PENALTY_RADIUS 10.0f       // the grid around a blocked spot is avoided ...
#define BP_PENALTY_MS 6000.0f         // ... for this long
#define BP_RECOVER_MS 650.0f          // stuck recovery: back off + hop this long
#define BP_GOAL_RADIUS 60.0f          // the route's end counts as the goal reached within it (stats; Hunt drops a roam goal at 56)
#define BP_PLAYER_RADIUS 2.6f         // another player's body half width (playerScale)
#define BP_JUMP_AIR_MS 800.0f         // a standing jump's time in the air (lands ~0.8 s later on flat ground)
#define BP_AIR_LOOK_MS 250.0f         // airborne: where the drift carries the body this soon must have ground below
#define BP_SLOW 0.012f                // horizontal speed (units / ms) under which a hop needs no run-up alignment

// KrunkNative move codes (Input.move_dir, ui_controls.c): 0 forward, 2 right, 4 back, 6 left
#define BP_MOVE_NONE (-1)
#define BP_MOVE_BACK 4

static float bp_d2d(const vec3 a, const vec3 b) { return hypotf(a.x - b.x, a.z - b.z); }
static float bp_d3d(const vec3 a, const vec3 b) {
    const float x = a.x - b.x, y = a.y - b.y, z = a.z - b.z;
    return sqrtf(x * x + y * y + z * z);
}
static vec3 bp_forward(const float yaw) { return (vec3) {-sinf(yaw), 0.0f, -cosf(yaw)}; }
static float bp_angle_delta(const float from, const float to) { return atan2f(sinf(to - from), cosf(to - from)); }
// UTILS.getDrs: the yaw from self to target
static float bp_yaw_to(const vec3 target, const vec3 self) { return (float) -atan2(-(double) (self.x - target.x), (double) (self.z - target.z)); }
// The move code that walks toward world yaw `move_yaw` while looking at `look_yaw`.
static int bp_move_code(const float look_yaw, const float move_yaw) {
    const float rel = bp_angle_delta(look_yaw, move_yaw);
    int k = (int) lroundf(-rel / (BP_PI / 4.0f));
    return ((k % 8) + 8) % 8;
}
static float bp_rand(float lo, float hi) { return lo + (hi - lo) * (float) ldexp((double) pcg32_random(), -32); }

// ---------------------------------------------------------------- the read-only view

int bot_path_goal(const GameBot *bot, vec3 *goal) {
    if (!bot || !bot->player || !bot->player->active) return 0;
    if (bot->follow.has_goal) {
        *goal = bot->follow.goal;
        return 1;
    }
    if (bot->has_roam) {
        *goal = bot->roam_target;
        return 1;
    }
    if (bot->has_route_target) {
        *goal = bot->route_target;
        return 1;
    }
    return 0;
}

static unsigned char bp_segment_flags(Game *game, const BotPathSegment *s, const int last, const int reached) {
    unsigned char flags = 0;
    if (last) flags |= BOT_PATH_FLAG_LAST | (reached ? 0 : BOT_PATH_FLAG_PARTIAL);
    if (s->kind == BOT_PATH_DROP || s->rise < -BP_STEP) flags |= BOT_PATH_FLAG_LEDGE;
    if (s->headroom < BOT_PATH_HOP_HEADROOM) flags |= BOT_PATH_FLAG_LOW_CEILING;
    if (game && (bot_hazard_at(game, s->to) || bot_hazard_at(game, (vec3) {(s->from.x + s->to.x) * 0.5f, MAX(s->from.y, s->to.y), (s->from.z + s->to.z) * 0.5f})))
        flags |= BOT_PATH_FLAG_HAZARD;
    return flags;
}

int bot_path_current(const GameBot *bot, BotPathSegment *out, const int max) {
    if (!bot || !out || max <= 0 || !bot->player || !bot->player->active) return 0;
    const BotPath *path = &bot->path;
    if (!path->count || bot->path_index >= path->count) return 0;
    Game *game = bot->player->game;
    int n = 0;
    vec3 from = bot->player->position;
    for (int i = bot->path_index; i < path->count && n < max; i++) {
        BotPathSegment *s = &out[n++];
        memset(s, 0, sizeof(*s));
        s->from = from;
        s->to = path->points[i];
        s->length = bp_d2d(s->from, s->to);
        s->rise = s->to.y - s->from.y;
        s->kind = path->kinds[i];
        const vec3 high = s->rise > 0.0f ? s->to : s->from;
        s->headroom = game ? bot_nav_headroom(game, high, BOT_PATH_HEADROOM_MAX) : BOT_PATH_HEADROOM_MAX;
        // a low ceiling anywhere along a long segment counts too (sampled at the middle)
        if (game && s->length > 12.0f) {
            const vec3 mid = {(s->from.x + s->to.x) * 0.5f, high.y, (s->from.z + s->to.z) * 0.5f};
            s->headroom = MIN(s->headroom, bot_nav_headroom(game, mid, BOT_PATH_HEADROOM_MAX));
        }
        s->flags = bp_segment_flags(game, s, i == path->count - 1, path->reached);
        from = s->to;
    }
    return n;
}

void bot_path_stats(const Game *game, BotNavStats *out) {
    if (game && game->bots) *out = game->bots->nav_stats;
    else memset(out, 0, sizeof(*out));
}

// ---------------------------------------------------------------- the route

void bot_path_reset(GameBot *bot, const Player *self) {
    memset(&bot->follow, 0, sizeof(bot->follow));
    bot->follow.stuck_pos = self ? self->position : (vec3) {0.0f, 0.0f, 0.0f};
    bot->follow.best_dist = 1e9f;
    bot->path.count = 0;
    bot->path_index = 0;
    bot->path_timer = 0.0f;
    bot->path_stranded = 0;
}

// Is the waypoint at `index` done: reached, or passed (the bot is beyond it along the segment
// and close to its line) at a compatible height.
static int bp_waypoint_done(const GameBot *bot, const Player *self, const int index, const vec3 prev) {
    const BotPath *path = &bot->path;
    const vec3 wp = path->points[index];
    const vec3 p = self->position;
    const float d = bp_d2d(wp, p), dy = p.y - wp.y;
    const int kind = path->kinds[index];
    int height_ok;
    if (kind == BOT_PATH_JUMP) height_ok = dy >= -1.5f && dy <= BP_STEP + 2.0f;
    else if (kind == BOT_PATH_DROP) height_ok = dy <= BP_STEP + 2.0f && dy >= -BP_STEP;
    else height_ok = fabsf(dy) <= BP_STEP + 2.5f;
    if (!height_ok) return 0;
    if (d < BP_WAYPOINT_RADIUS) return 1;
    if (kind != BOT_PATH_WALK) return 0;
    // passed it: beyond the waypoint along prev -> wp, within a body of its line
    const float sx = wp.x - prev.x, sz = wp.z - prev.z, len = hypotf(sx, sz);
    if (len < 1.0f) return 0;
    const float ux = sx / len, uz = sz / len;
    const float along = (p.x - wp.x) * ux + (p.z - wp.z) * uz;
    const float across = fabsf(-(p.x - wp.x) * uz + (p.z - wp.z) * ux);
    return along > 0.0f && along < 10.0f && across < 3.0f;
}

// A straight walk from the feet to `to` is possible: the body sweep is clear and the ground
// carries it the whole way (sampled).
static int bp_straight_walkable(Game *game, const Player *self, const vec3 to) {
    const vec3 p = self->position;
    const float d = bp_d2d(to, p);
    if (d < 1.0f) return 1;
    if (fabsf(to.y - p.y) > BP_STEP + 1.0f) return 0;
    const vec3 dir = {(to.x - p.x) / d, 0.0f, (to.z - p.z) / d};
    float low_top, end_y;
    if (bot_nav_sweep(game, p, dir, d, BP_STEP + 1.0f, &low_top, &end_y) != 0) return 0;
    if (fabsf(end_y - to.y) > BP_STEP + 1.0f) return 0;
    const int steps = (int) (d / 6.0f) + 1;
    for (int i = 1; i <= steps; i++) {
        const float t = (float) i / (float) steps;
        const vec3 q = {p.x + (to.x - p.x) * t, p.y + (to.y - p.y) * t, p.z + (to.z - p.z) * t};
        if (bot_hazard_at(game, q)) return 0;
    }
    return 1;
}

static void bp_search(Game *game, GameBot *bot, const Player *self, const vec3 goal, const int forced) {
    GameBots *bots = game->bots;
    BotPathFollow *f = &bot->follow;
    bots->path_budget--;
    bots->nav_stats.replans++;
    if (forced) bots->nav_stats.forced_replans++;
    bot->path_timer = bp_rand(BP_REPLAN_MIN_MS, BP_REPLAN_MAX_MS);
    bot->path_index = 0;
    bot->path_goal = goal;
    f->best_dist = 1e9f;
    f->no_progress = 0.0f;
    if (!bot_nav_find_path(game, self->position, goal, &bot->path)) {
        bot->path.count = 0;
        bot->path_stranded = 0;
        return;
    }
    // unreachable from here, and the route's end is no closer than we are: give the goal up
    const vec3 end = bot->path.count ? bot->path.points[bot->path.count - 1] : self->position;
    const int stranded = !bot->path.reached && (bp_d2d(end, goal) > 40.0f && bp_d2d(end, goal) > bp_d2d(self->position, goal) - 8.0f);
    if (stranded && !bot->path_stranded) bots->nav_stats.unreachable++;
    bot->path_stranded = stranded;
}

vec3 bot_path_route(Game *game, GameBot *bot, Player *self, const vec3 goal, const float dt, int *on_path, int *give_up) {
    GameBots *bots = game->bots;
    BotPathFollow *f = &bot->follow;
    *on_path = 0;
    *give_up = 0;
    // a new goal: the old one counts as reached when its route was walked to the end
    if (f->has_goal && !f->goal_counted && bp_d3d(goal, f->goal) > 1.0f && bot->path.count && bot->path.reached &&
        bp_d2d(self->position, f->goal) < BP_GOAL_RADIUS) {
        bots->nav_stats.goals_reached++;
    }
    if (bp_d3d(goal, f->goal) > 1.0f) f->goal_counted = 0;
    f->goal = goal;
    f->has_goal = 1;
    bot->path_timer -= dt;

    // consume reached / passed waypoints
    vec3 prev = f->prev_waypoint;
    if (!bot->path_index) prev = f->route_start;
    while (bot->path_index < bot->path.count && bp_waypoint_done(bot, self, bot->path_index, prev)) {
        prev = bot->path.points[bot->path_index];
        f->prev_waypoint = prev;
        bot->path_index++;
        f->best_dist = 1e9f;
        f->no_progress = 0.0f;
        f->block_count = 0;
    }
    const int at_end = bot->path.count && bot->path_index >= bot->path.count;
    if (at_end && !f->goal_counted && bp_d2d(self->position, goal) < BP_GOAL_RADIUS && bot->path.reached) {
        f->goal_counted = 1;
        bots->nav_stats.goals_reached++;
    }

    const float goal_moved = bp_d3d(goal, bot->path_goal);
    const int have_route = bot->path.count && !at_end;
    int stale = !bot->path.count || bot->path_timer <= 0.0f || f->force_replan;
    if (at_end && bp_d2d(self->position, goal) > BP_GOAL_RADIUS + 4.0f && goal_moved > 1.0f) stale = 1; // the goal moved on
    if (goal_moved > 40.0f || (goal_moved > 8.0f && bp_d2d(goal, self->position) < 60.0f)) stale = 1;
    if (have_route) { // knocked off the segment being walked (or fell off it): replan
        const vec3 wp = bot->path.points[bot->path_index];
        const float sx = wp.x - prev.x, sz = wp.z - prev.z, len = hypotf(sx, sz);
        float off;
        if (len < 1.0f) off = bp_d2d(wp, self->position);
        else {
            const float t = CLAMP(((self->position.x - prev.x) * sx + (self->position.z - prev.z) * sz) / (len * len), 0.0f, 1.0f);
            off = hypotf(self->position.x - (prev.x + sx * t), self->position.z - (prev.z + sz * t));
        }
        if (off > BP_OFF_ROUTE || fabsf(self->position.y - MAX(prev.y, wp.y)) > BP_DROP) stale = 1;
        // standing below the segment's level (fell off it, a drop overshot): it no longer applies
        if (self->on_ground && self->position.y < MIN(prev.y, wp.y) - (BP_STEP + 2.0f)) stale = 1;
        // or above it by more than a jump (a ramp side snapped us up onto a roof): from up here the
        // waypoint is a drop the route never planned
        if (self->on_ground && self->position.y > MAX(prev.y, wp.y) + BP_JUMP + 2.0f) stale = 1;
    }
    if (stale && bots->path_budget > 0) {
        f->route_start = self->position;
        bp_search(game, bot, self, goal, f->force_replan);
        f->force_replan = 0;
    } else if (stale && !bot->path.count) {
        bot->path_timer = 0.0f; // no budget this tick: try next tick
    }
    if (bot->path_stranded && bp_d3d(goal, bot->path_goal) <= 40.0f) { // nothing closer to it from here
        *give_up = 1;
        return self->position;
    }
    if (bot->path_index >= bot->path.count) return goal; // at the end (or no route): the goal itself
    *on_path = 1;
    const vec3 wp = bot->path.points[bot->path_index];
    // corner cutting: pursue the next waypoint when it can be walked to straight from here
    if (bot->path_index + 1 < bot->path.count && bot->path.kinds[bot->path_index] == BOT_PATH_WALK && bot->path.kinds[bot->path_index + 1] == BOT_PATH_WALK &&
        bp_d2d(wp, self->position) < BP_LOOKAHEAD) {
        const vec3 next = bot->path.points[bot->path_index + 1];
        if (bp_d2d(next, self->position) < BP_LOOKAHEAD * 3.0f && bp_straight_walkable(game, self, next)) {
            f->prev_waypoint = wp;
            bot->path_index++;
            f->best_dist = 1e9f;
            f->no_progress = 0.0f;
            return next;
        }
    }
    return wp;
}

// ---------------------------------------------------------------- the move

// The body moves along `dir` already (or barely moves): a hop now goes where it should.
static int bp_aligned(const Player *self, const vec3 dir) {
    const float vx = self->velocity.x, vz = self->velocity.z, speed = hypotf(vx, vz);
    return speed < BP_SLOW || (vx * dir.x + vz * dir.z) >= 0.8f * speed;
}

// Another active player's body stands in the swept lane ahead.
static int bp_player_ahead(const Game *game, const Player *self, const vec3 dir, const float reach) {
    for (size_t i = 0; i < game->player_count; i++) {
        const Player *o = game->players[i];
        if (o == self || !o->active) continue;
        const float rx = o->position.x - self->position.x, rz = o->position.z - self->position.z;
        if (fabsf(o->position.y - self->position.y) > BP_HEIGHT) continue;
        if (hypotf(rx, rz) < BP_PLAYER_RADIUS * 1.5f) continue; // overlapping (spawned together): bodies pass through, walk apart
        const float along = rx * dir.x + rz * dir.z;
        if (along < 0.0f || along > reach + BP_PLAYER_RADIUS) continue;
        const float across = fabsf(-rx * dir.z + rz * dir.x);
        if (across < BP_PLAYER_RADIUS * 2.0f + 0.5f) return 1;
    }
    return 0;
}

// Can the body walk (or hop) along world yaw `yaw` from here: 0 no, 1 walk, 2 walk + jump,
// 3 clear but another player stands in it (players do not collide: a soft refusal).
static int bp_lane_ok(Game *game, const GameBot *bot, const Player *self, const float yaw, const float reach, const int allow_hop, const int dropping, const int players, const float level_y) {
    const vec3 dir = bp_forward(yaw);
    const vec3 p = self->position;
    float low_top = 0.0f, end_y = p.y;
    const float max_drop = self->on_terrain ? 1e9f : dropping ? BP_DROP : (BP_STEP + 3.0f);
    const int sweep = bot_nav_sweep(game, p, dir, reach, max_drop, &low_top, &end_y);
    if (sweep == 2) return 0;
    // nothing to stand on within reach: an edge the route does not drop from, or (dropping) a fall
    // deeper than any drop link, out of the map on a floating island
    if (sweep == 3) return 0;
    if (dropping && end_y < level_y - (BP_STEP + 2.0f)) return 0; // a deeper pit than the drop lands in
    int hop = 0;
    if (sweep == 1) {
        // a hop only onto the level the route walks (not onto a barrel beside the way), fitting
        // under whatever is above the obstacle, with the run-up heading that way (a hop at speed
        // sideways flies past the ledge, off the far side of a walkway)
        if (!allow_hop || !self->on_ground || fabsf(end_y - level_y) > BP_STEP + 1.0f) return 0;
        if (!bp_aligned(self, dir)) return 1; // walk up to it first: pressing against it lines the body up
        const vec3 over = {p.x + dir.x * reach, low_top, p.z + dir.z * reach};
        if (bot_nav_headroom(game, over, BP_HEIGHT + 4.0f) < BP_HEIGHT + 1.0f) return 0;
        hop = 1;
    }
    const vec3 ahead = {p.x + dir.x * reach, end_y, p.z + dir.z * reach};
    if (bot_hazard_at(game, ahead)) return 0;
    if (players && !hop && bp_player_ahead(game, self, dir, MIN(reach, 9.0f))) return 3;
    (void) bot;
    return hop ? 2 : 1;
}

void bot_path_move(Game *game, GameBot *bot, Player *self, const vec3 point, const int on_path, const float look_yaw, const float dt, BotPathMove *out) {
    GameBots *bots = game->bots;
    BotPathFollow *f = &bot->follow;
    const vec3 p = self->position;
    out->move = BP_MOVE_NONE;
    out->jump = 0;
    out->give_up = 0;
    const float d = bp_d2d(point, p);

    // stuck recovery in progress: back off the way we came and hop once
    if (f->recover_timer > 0.0f) {
        f->recover_timer -= dt;
        out->move = self->on_ladder ? BP_MOVE_BACK : f->recover_code;
        if (self->on_ground && f->recover_timer > BP_RECOVER_MS - 100.0f) out->jump = 1;
        f->moving = 1;
        return;
    }
    if (d < 2.5f) {
        f->moving = 0;
        f->no_progress = 0.0f;
        return;
    }
    // the grid has no ladder links: a ladder touched in passing (one beside a corridor) is walked
    // past (it is no wall; walking along it does not climb), but one climbed above the level we
    // head for is stepped back off (Krunker climbs whatever ladder you walk into)
    if (self->on_ladder && p.y > point.y + BP_STEP + 1.0f) {
        out->move = BP_MOVE_BACK;
        f->moving = 1;
        return;
    }
    if (!self->on_ground && !self->on_ladder) {
        // airborne (a hop, a drop, knocked back): air-steer straight at the point; the ground
        // sweeps mean nothing up here and bot_path_guard brakes a drift over the void. Progress
        // is not judged in the air (a jump link's arc gets no closer for a while).
        out->move = bp_move_code(look_yaw, bp_yaw_to(point, p));
        f->move_yaw = bp_yaw_to(point, p);
        f->moving = 1;
        return;
    }

    // progress toward the point we pursue
    if (d < f->best_dist - 0.25f) {
        f->best_dist = d;
        f->no_progress = 0.0f;
    } else if (f->moving) {
        f->no_progress += dt;
    }
    if (f->no_progress >= BP_BLOCKED_MS) {
        f->no_progress = 0.0f;
        f->best_dist = 1e9f;
        if (f->block_count && bp_d3d(p, f->stuck_pos) > 12.0f) f->block_count = 0; // blocked somewhere else this time: a fresh start
        f->stuck_pos = p;
        f->block_count++;
        bot_nav_penalize(game, p, BP_PENALTY_RADIUS, BP_PENALTY_MS);
        if (f->block_count >= 2) {
            // stuck: back off from the obstacle, hop, and let the goal go
            bots->nav_stats.stuck++;
            f->block_count = 0;
            f->recover_timer = BP_RECOVER_MS;
            // back the way we came, else the first open lane around (a drop is fine: anywhere but here)
            const int back = bp_move_code(look_yaw, f->move_yaw + BP_PI);
            const int side = pcg32_boundedrand(2) ? 1 : -1;
            static const int k_around[] = {0, 1, -1, 2, -2, 3, -3, 4};
            f->recover_code = bp_move_code(look_yaw, f->move_yaw + (float) side * BP_PI * 0.5f);
            for (size_t i = 0; i < sizeof(k_around) / sizeof(k_around[0]); i++) {
                const int code = ((back + k_around[i] * side) % 8 + 8) % 8;
                if (bp_lane_ok(game, bot, self, look_yaw - (float) code * (BP_PI / 4.0f), BP_REACH_MIN, 0, 1, 0, p.y) == 1) {
                    f->recover_code = code;
                    break;
                }
            }
            f->force_replan = 1;
            bot->path_timer = 0.0f;
            out->move = f->recover_code;
            out->give_up = 1;
            f->moving = 1;
            return;
        }
        f->force_replan = 1; // blocked: route around this spot
        bot->path_timer = 0.0f;
    }

    const float want = bp_yaw_to(point, p);
    const int kind = on_path && bot->path_index < bot->path.count ? bot->path.kinds[bot->path_index] : BOT_PATH_WALK;
    const int dropping = kind == BOT_PATH_DROP || (!on_path && point.y < p.y - BP_STEP);
    const int jumping = kind == BOT_PATH_JUMP;
    const float speed = hypotf(self->velocity.x, self->velocity.z);
    const float reach = BP_REACH_MIN + speed * BP_REACH_MS;
    // a jump link: hop at the ledge when close and running at it (the walk brings us there; a
    // hop while still carrying the speed of the last segment sideways flies past the ledge)
    if (jumping && d < 9.0f && self->on_ground && point.y > p.y + 2.0f && bp_aligned(self, bp_forward(want))) out->jump = 1;

    // the wanted heading, then its neighbours (nearest first): the first open lane wins; a low
    // obstacle is hopped on the wanted lane only where the segment allows it
    const int ideal = bp_move_code(look_yaw, want);
    static const int k_offsets[] = {0, 1, -1, 2, -2, 3, -3, 4};
    const int allow_hop = (kind == BOT_PATH_WALK || jumping) && !dropping;
    int chosen = -1, hop = 0, crowded = -1;
    float crowded_yaw = 0.0f;
    for (size_t i = 0; i < sizeof(k_offsets) / sizeof(k_offsets[0]) && chosen < 0; i++) {
        const int code = ((ideal + k_offsets[i]) % 8 + 8) % 8;
        const float yaw = look_yaw - (float) code * (BP_PI / 4.0f);
        // off the wanted lane the sweep is shorter (we only side-step for a moment) and never hops
        const float lane_reach = i == 0 ? reach : MAX(BP_REACH_MIN, reach * 0.6f);
        const int ok = bp_lane_ok(game, bot, self, yaw, lane_reach, allow_hop && i == 0, dropping, i < 3, point.y);
        if (i < 7) f->lanes[i] = (unsigned char) ok;
        if (!ok) continue;
        if (ok == 3) { // a player in the way: side-step if a neighbouring lane is open, else walk through
            if (crowded < 0) {
                crowded = code;
                crowded_yaw = yaw;
            }
            continue;
        }
        chosen = code;
        hop = ok == 2;
        f->move_yaw = yaw;
    }
    if (chosen < 0 && crowded >= 0) {
        chosen = crowded;
        f->move_yaw = crowded_yaw;
    }
    if (chosen < 0) {
        // boxed in: stand (the blocked timer runs on, its replan routes around this spot)
        f->moving = 1;
        f->boxed++;
        return;
    }
    out->move = chosen;
    if (hop) out->jump = 1;
    f->moving = 1;
}

// ---------------------------------------------------------------- the guard

void bot_path_guard(Game *game, GameBot *bot, const Player *self, const float look_yaw, int *move, unsigned char *jump) {
    (void) bot;
    if (!game || !game->bots || !game->bots->nav || !game->map || !self || !self->active) return;
    if (self->noclip || self->on_terrain || self->on_ladder) return;
    const vec3 p = self->position;
    const float depth = p.y - game->map->death_y - 0.5f; // a top above deathY catches the body
    if (depth <= 0.0f) return;
    const float vx = self->velocity.x, vz = self->velocity.z, speed = hypotf(vx, vz);
    const vec3 vdir = speed > 1e-4f ? (vec3) {vx / speed, 0.0f, vz / speed} : (vec3) {0.0f, 0.0f, 0.0f};
    const float brake_yaw = bp_yaw_to((vec3) {p.x - vdir.x, p.y, p.z - vdir.z}, p);
    float g;
    if (!self->on_ground) {
        // airborne: the drift must come down on something; else brake against it
        if (speed < 0.004f) return;
        const vec3 q = {p.x + vx * BP_AIR_LOOK_MS, p.y, p.z + vz * BP_AIR_LOOK_MS};
        if (bot_nav_ground(game, q, depth, &g)) return;
        *move = bp_move_code(look_yaw, brake_yaw);
        *jump = 0;
        return;
    }
    float low_top, end_y;
    if (*move >= 0) {
        const vec3 dir = bp_forward(look_yaw - (float) *move * (BP_PI / 4.0f));
        float reach = BP_REACH_MIN + speed * BP_REACH_MS;
        if (*jump) reach += MAX(speed, 0.03f) * BP_JUMP_AIR_MS; // the hop lands this much farther on
        if (bot_nav_sweep(game, p, dir, reach, depth, &low_top, &end_y) == 3) {
            // the walk (or the hop) ends over the void: not this way. A hop alone may still be fine
            // (jumping straight up a ledge in place) but not with this move.
            *move = BP_MOVE_NONE;
            *jump = 0;
        }
    }
    if (*jump && *move < 0 && speed > BP_SLOW) {
        // a hop in place carries the momentum: it must land on something
        if (bot_nav_sweep(game, p, vdir, speed * BP_JUMP_AIR_MS + BP_REACH_MIN, depth, &low_top, &end_y) == 3) *jump = 0;
    }
    // momentum alone toward a void edge (knocked back, a turn at speed): brake against it
    if (speed > BP_SLOW && bot_nav_sweep(game, p, vdir, BP_REACH_MIN * 0.5f + speed * BP_REACH_MS, depth, &low_top, &end_y) == 3) {
        *move = bp_move_code(look_yaw, brake_yaw);
        *jump = 0;
    }
}
