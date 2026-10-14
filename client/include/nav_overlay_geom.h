#pragma once
// The spectate bot overlay's geometry (KrunkNative; client/src/nav_overlay_geom.c): what a
// spectator following a bot sees with show_navmeshes on, built from NET_EXT_KN_BOT_PATH
// (shared/include/net_ext.h) as GL_LINES vertex lists per layer. No GL here: nav_overlay.c
// uploads the layers each frame (the bot and its target move between packets, so their live
// positions come in as arguments) and client/tests/nav_overlay_test.c checks the output.
//
// Layers (draw order; all depthTest false like the nav mesh layers):
//   path:       the route polyline from the bot's feet, lifted off the ground, a tick at each
//               waypoint; drop segments too
//   jump:       jump segments as a small arc (the bot leaves the ground there)
//   goal:       a pole with a diamond at the far end of the route (bot_path_goal)
//   target:     a ring around the target's feet, a cross at its chest and (line of sight) the
//               sight line from the bot's eye
//   last_known: a ring with an X where the target was last seen (while it is out of sight or
//               elsewhere)
#include <net_ext.h>
#include <common.h>
#include <stddef.h>

enum { NAV_BOT_PATH, NAV_BOT_JUMP, NAV_BOT_GOAL, NAV_BOT_TARGET, NAV_BOT_LAST_KNOWN, NAV_BOT_LAYERS };

extern const int nav_bot_layer_hex[NAV_BOT_LAYERS]; // 0xrrggbb per layer

#define NAV_BOT_PATH_LIFT 1.2f        // the polyline floats this far over the feet
#define NAV_BOT_TICK_HEIGHT 3.0f      // waypoint ticks: NAV_BOT_PATH_LIFT .. this
#define NAV_BOT_JUMP_APEX 6.0f        // the arc's rise over the straight segment
#define NAV_BOT_GOAL_POLE 14.0f       // goal pole height, the diamond sits on top
#define NAV_BOT_GOAL_RADIUS 3.0f
#define NAV_BOT_TARGET_RADIUS 3.5f    // the ring around the target's feet
#define NAV_BOT_TARGET_CHEST 5.0f     // the cross at the target's chest
#define NAV_BOT_EYE 8.0f              // the sight line leaves the bot's eye
#define NAV_BOT_LKP_RADIUS 2.5f
#define NAV_BOT_LKP_POLE 6.0f
#define NAV_BOT_LKP_NEAR 2.0f         // a last-known marker this close to a seen target is skipped

typedef struct {
    float *v; // x, y, z per vertex, GL_LINES pairs
    size_t n, cap;
} NavGeomVerts;

typedef struct {
    NavGeomVerts layers[NAV_BOT_LAYERS];
} NavBotGeom;

// Builds every layer from the packet. bot_feet: the bot's live feet (NULL: the packet's point 0,
// or no polyline without one); target_feet: the target's live feet (NULL: the packet's). Returns
// the vertex count over all layers (0: nothing to draw, e.g. a dead bot with no goal). Layers
// are cleared first; buffers are kept between builds.
size_t nav_bot_geom_build(const NetBotPathPacket*, const vec3 *bot_feet, const vec3 *target_feet, NavBotGeom *out);
void nav_bot_geom_clear(NavBotGeom*); // every layer empty (buffers kept)
void nav_bot_geom_free(NavBotGeom*);
