#pragma once
// The route a bot player is following (shared/src/bot_path.c): A* over the map's walkable
// grid (shared/src/bot_nav.c) with jump / drop links, followed with a lookahead and local
// avoidance against the map geometry. This header is the read-only view other modules use:
//   - the spectate overlay draws the polyline, goal and markers (client, over KN_EXT_BOT_PATH),
//   - bot movement (bot_move.c) hops only where the segment ahead is clear.
// Segment 0 always starts at the bot's feet: the polyline is seg[0].from, seg[0].to,
// seg[1].to, ... seg[n-1].to (== the goal when the route reaches it).
#include <bots.h>

typedef enum {
    BOT_PATH_WALK = 0,   // on the ground: the rise is within the climb height
    BOT_PATH_JUMP = 1,   // `to` is a ledge above the climb height: jump up onto it
    BOT_PATH_DROP = 2,   // `to` is below: drop down (no jump: the edge is the launch point)
    BOT_PATH_LADDER = 3, // reserved (ladders are not linked yet): climb
} BotPathKind;

// BotPathSegment.flags
#define BOT_PATH_FLAG_LEDGE 1       // `to` lies beside a void edge or a drop: never hop past it
#define BOT_PATH_FLAG_LOW_CEILING 2 // headroom under BOT_PATH_HOP_HEADROOM: no hopping here
#define BOT_PATH_FLAG_HAZARD 4      // a jump pad / kill volume touches the segment: walk it exactly
#define BOT_PATH_FLAG_LAST 8        // the final segment: `to` is the goal (or as close as it gets)
#define BOT_PATH_FLAG_PARTIAL 16    // the route does not reach the goal (unreachable from here)

#define BOT_PATH_HOP_HEADROOM 20.0f // units above the feet a bunny hop needs (jump apex ~14 + body)
#define BOT_PATH_HEADROOM_MAX 64.0f // headroom is capped here ("open sky")

typedef struct {
    vec3 from, to;              // feet positions in world units (y = ground under the feet)
    float length;               // horizontal (xz) length
    float rise;                 // to.y - from.y (negative: downhill / a drop)
    float headroom;             // free height above the higher end's feet, <= BOT_PATH_HEADROOM_MAX
    unsigned char kind;         // BotPathKind: how `to` is reached from `from`
    unsigned char flags;        // BOT_PATH_FLAG_*
} BotPathSegment;

#define BOT_PATH_SEGMENTS_MAX BOT_PATH_MAX

// The remaining route of `bot` from its feet to its goal: at most `max` segments into
// `out_segments`, returns how many were written (0: no bot, no player, standing still, or no
// route). Segment 0 is the one being walked now (its `from` is the bot's feet). Pure read.
int bot_path_current(const GameBot *bot, BotPathSegment *out_segments, int max);

// The point the bot is heading for (its roam / hunt / hide goal; the far end of the route):
// 1 and *goal when it has one, else 0.
int bot_path_goal(const GameBot *bot, vec3 *goal);

// Navigation counters of a game's bots since the manager was created (BotNavStats, bots.h; the
// nav e2e reads them through the `bot_nav` console command). Zeroes *out without a manager.
void bot_path_stats(const Game *game, BotNavStats *out);

// ---- the follower (bots.c's walking; bot_move.c may read the route through the view above)
typedef struct {
    int move;                   // Input.move_dir code relative to the look yaw, -1 none
    unsigned char jump;         // hop this frame (a low obstacle, a jump link, a stuck recovery)
    unsigned char give_up;      // the goal is unreachable or we are stuck: the caller picks another
} BotPathMove;

// Keep the route to `goal` fresh and return the point to head for now (a waypoint, the goal
// itself without a route, the feet when stranded). *on_path: a route waypoint; *give_up: the
// goal cannot be reached from here (the caller should choose another).
vec3 bot_path_route(Game*, GameBot*, Player *self, vec3 goal, float dt, int *on_path, int *give_up);
// Walk toward `point` (from bot_path_route) with local avoidance and stuck handling; the move
// code is relative to `look_yaw` (where the bot looks this frame).
void bot_path_move(Game*, GameBot*, Player *self, vec3 point, int on_path, float look_yaw, float dt, BotPathMove *out);
// A (re)spawn: forget the route and the follower state.
void bot_path_reset(GameBot*, const Player *self);
