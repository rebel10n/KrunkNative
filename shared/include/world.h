#pragma once
// map-logic WP-B (doc/v7/map-logic.md §7 WP-B): the v7.1.4 world runtime, run identically by the
// native server, the offline sandbox and (for prediction) the client:
//   shared/src/collision_tri.c  triangle (cpx / top-level "tris") capsule collision, collisions.js
//   shared/src/terrain.c        the procedural terrain (shared/include/terrain.h)
//   shared/src/zone.c           the battle-royale zone, libs/Zone.js
//   shared/src/world.c          check points, teleporters, parkour reset, score zones / logTime,
//                               qualifying and PvP zones, node moves, object animations, join sync
//   shared/src/build.c          player builds (data/buildObjs.js, game.buildObj / players.build)
// Packets: NET_PACKET_BLOCK_WORLD (130-139) in shared.h; the client side is client/src/world_events.c.
#include <shared.h>
#include <terrain.h>

// ---- zone (libs/Zone.js; Map.zone): server tick, "zn" sync, 10 damage/s outside, reset per round
void zone_reset(Game*);                          // resetZone (round start)
void zone_update(Game*, float delta);            // game.update #24: shrink, stages, damage (authority, s)
int zone_contains(const Game*, vec3 position);   // inside the current circle (1 when there is no zone)
// The circle to draw: centre, radius and height. 0 when the map has no zone or it is hidden (size
// 0). The authority gives its live circle; a client the 900 ms tween of the last NET_PACKET_ZONE.
int zone_circle(const Game*, float *x, float *z, float *radius, float *height);
void zone_animate(Game*, float delta);           // client: advance the tween (s)
void zone_apply_packet(Game*, const NetZonePacket*); // client: Zone.update(0, value)
void zone_send_state(Game*, int uid);            // init syncData.zone for a joiner (authority)

// ---- terrain (libs/Terrain.js; Map.terrain)
// Any-direction ray against the map's terrain, world space: 1 and *distance (world units from
// origin) on a hit within max_distance. The v7 callers (hitscan, projectiles, canSee, builds) pass
// their own range as max_distance.
int terrain_raycast(const Game*, vec3 origin, vec3 direction, float max_distance, float *distance);
// The fraction (0, 1] of `segment` (from origin) where it first enters the terrain, -1 none: the
// terrain stop of hitscan rays, projectile steps and canSee (lineInRect fractions).
#define WORLD_HAVE_TERRAIN_FRACTION 1
float world_terrain_fraction(const Game*, vec3 origin, vec3 segment);
// KrunkNative camera collision (the death chase cam, the third-person camera): the fraction (0, 1]
// of `segment` from origin where it first enters a solid object box (bot_object_blocks_player's
// set; "cpx" objects count by their baked triangles, not their box), a map triangle ("tris") or
// the terrain. 1 when the segment is clear. Only a segment that starts in open space means much.
float world_solid_fraction(const Game*, vec3 origin, vec3 segment);
// The terrain height straight under (x, z): raycast(x, -z, 10000, 0, 0, -20000), 0 = no terrain / miss.
int world_terrain_height(const Game*, float x, float z, float *height);

// ---- triangle collision (data/collisions.js: capsuleColl)
// The capsule from feet (x, y, z) to (x, y + height, z) with `radius` against one world-space
// triangle (9 floats): 1 with the push-out normal and depth when they intersect.
int tri_capsule_collide(const float tri[9], vec3 feet, float radius, float height, vec3 *normal, float *depth);

// ---- object moves / animation (trigger actions 40-42, 39 / 63; "mo" / "mf" / "ana")
int world_teleport_object_to_node(Game*, Object*, int node_id); // 0 = no such node
void world_reset_object_position(Game*, Object*);
void world_play_anim(Game*, Object*, const char *clip, int raw);

// ---- implemented by WP-A (shared/src/world_lookup.c): pure map lookups
// manager.pathNodes[nid]: the first PATH_NODE with that "nid", NULL none.
const Object *world_path_node(const Map*, int node_id);
// The ENVIRONMENT_ZONE containing `position` (client camera; pointInBox3D over envZones, the
// first match wins), -1 = the map's own environment.
int world_environment_zone_at(const Map*, vec3 position);

// ---- builds (data/buildObjs.js, shared/src/build.c)
enum { WORLD_BUILD_PLACES_ALL, WORLD_BUILD_PLACES_OBJS, WORLD_BUILD_PLACES_FLOOR };
#define WORLD_BUILD_COUNT 4
typedef struct {
    const char *name;
    float w, h, l;           // full size
    int cost, health;
    int places;              // WORLD_BUILD_PLACES_*: "all" / "objs" (snap onto objects only) / "floor"
    unsigned char rot_facing:1, rot_dir:1;
    unsigned char ramp:1;    // custMod: addRamp (Stairs)
    int color;               // 0xRRGGBB
} WorldBuildObject;
extern const WorldBuildObject g_build_objects[WORLD_BUILD_COUNT];
// game.buildObj(buildInd, x, y, z, w, h, l, dir, -1, grounded, player): a dynamic block / ramp
// with its feet at `feet` (grounded: the centre); a zero size component takes the build object's.
// The authority broadcasts "bo"; config.buildMax 1200 per round. 1 = built.
int game_build_object(Game*, int build_index, vec3 feet, vec3 size, float direction, int grounded, Player *player);
void world_apply_build_packet(Game*, const NetBuildPacket*); // client: "bo" / syncData.dyno
void world_remove_builds(Game*);                             // game.removeBuilds (finishInit)
int world_put_object(Game*, size_t uid, Object *object);     // KrunkNative Object Manager (client): object at the server uid
void world_send_builds(Game*, int uid);                      // syncData.dyno for a joiner
// KrunkNative client prediction of the local player's builds: player_build on a client puts the
// build it placed into the map at once (Object.predicted_ms), at the uid the server is expected to
// give it; the server's "bo" replaces it there (world_apply_build_packet, which also drops a
// prediction of the same build at another uid). One the server never confirms (it refused: score,
// the cap, a different hit) turns into a destroyed placeholder after BUILD_PREDICT_MS, so the uids
// stay aligned. world_builds_tick (game_tick_world, clients) runs that clock (delta in seconds).
#define BUILD_PREDICT_MS 1500.0f
void world_builds_tick(Game*, float delta);
// The Build Tool (players.build / procInputs selectBuild), for the weapon code: holding aim picks
// Player.build_index from the look delta (radians this input); firing calls player_build(place):
// the ray to the nearest object / terrain, the face, buildSnap of a build target, the overlap test,
// then (authority) game.buildObj and the cost. 1 = built.
void player_select_build(Player*, float yaw_delta, float pitch_delta);
int player_build(Player*, int place);

// ---- round / join (game.c calls these)
void world_round_start(Game*);                   // finishInit: zone reset, moved objects home, builds removed, saveColls
void world_send_state(Game*, int uid);           // init syncData: zone, mov, dyno for a joiner (authority)
void world_tick(Game*, float delta);             // per game tick (s): ownTime clocks, reset timers, the client zone tween

// ---- check points (game.setCheckPoint / resetToStart / trigger action 33)
// game_set_checkpoint / game_check_teleport / game_change_position / game_spawn_at_checkpoint keep
// their shared.h declarations (bodies in world.c).
#define WORLD_HAVE_CLEAR_CHECKPOINTS 1
// checkPoint = null, checkPointList and checkPointData cleared; the authority sends "chp" true.
void world_clear_checkpoints(Game*, Player*);

// ---- the player collision chain (player.js procInputs) and its helpers
void world_player_spawned(Game*, Player*);       // spawn: setInitPos, noPVP / forcePVP off
// The tris pass before the object loop (player.js:1337-1350): push out along each hit's normal,
// drop the velocity along it, normal.y > 0.7 = on ground. Skipped in noclip.
void world_player_tris(Player*);
// The qualifying zones a player passes through (player.js:1459-1470): premium (premiumT),
// verified (featured), partner (partnerApp >= 2), KPD (canFlag), team (teamCheck), KR (enoughKR).
// 0 = the object is solid for this player (or is no such zone).
int world_zone_passable(const Player*, const Object*);
// NOPVP_ZONE / PVP_ZONE (COLLISION_TYPE_NONE volumes, collidesE): player.noPVP / forcePVP for this
// tick. Called once per procInputs after the object loop (both start false every tick).
void world_player_pvp_zones(Player*);
// The v7 PvP filter of hitscan, projectiles and explosions: neither side noPVP and both or neither
// forcePVP. forcePVP players have no team (player.team getter), so their friendly fire is on.
int world_pvp_allows(const Player *attacker, const Player *victim);
int world_pvp_ignores_team(const Player *attacker, const Player *victim); // both forcePVP
// A score zone touched (scoreObjs once per round: score(scoreP) + logTime).
void world_player_score_zone(Player*, const Object*);
// players.logTime: timeLog++ and "gt" (authority, when game.doLogTime).
void world_log_time(Game*, Player*);
// The terrain ground snap after the object loop (player.js:1779-1797). Returns 1 on terrain.
int world_player_terrain(Player*, int recon);
// input[13] (player.js:2003-2009): 1 resetToStart, 2 resetToLast (mode.allowReset), after the
// resetTimer countdown of this input (delta_ms, non-recon inputs only).
void world_player_reset_input(Player*, int reset, int recon, float delta_ms);
// collidingObjs bookkeeping of the onEnterObject / onExitObject triggers (player.js:1398-1408,
// 1650-1664): begin before the loop, touch every active padded hit, enter returns 1 the first tick
// an enter/exit object is inside, end fires onExitObject for the ones left.
void world_player_colls_begin(Player*);
void world_player_colls_touch(Player*, int object_index);
int world_player_colls_enter(Player*, int object_index);
void world_player_colls_end(Player*);
