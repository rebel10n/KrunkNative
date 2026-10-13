#pragma once
// KrunkNative Piss (WEAPON_ID_PISS, shared/src/weapons/kn_piss.c): not a Krunker v7.1.4 weapon
// (doc/v7/decisions.md "Piss"). Ported from the v3.1.1 reconstruction's grappling-gun branch
// (0c317bf "Piss", e1624e4 puddles and hose): players.js fireMustard / buildMustardPacket /
// buildMustardPuddle / findPuddleGroundY / updateMustardPuddles, index.js spawnMustardSpray /
// spawnMustardPuddle.
//
// - Firing (player_shoot -> player.c combat_fire_stream, authority): every `rate` (55 ms) the
//   stream leaves the hose (piss_stream_source) along the view and flies the branch's particle arc
//   (mustardSpeed, mustardGravity, at most mustardLife ms), traced in 50 ms segments against
//   players (lag compensated like hitscan), AIs, map objects and the terrain. A player / AI takes
//   the weapon's damage; a map object or the terrain is where it lands, and a puddle forms on the
//   ground below that point. The branch's stream never hit anything (dmg 0, the puddle went a fixed
//   way along the aim): see decisions.md. The client predicts the fire timer, ammo and recoil
//   like any weapon; its spray particles are visual (client/src/vfx.c), the others see them from
//   the NET_PACKET_SHOT of each shot (weapon_index = the Piss, hit = where the stream stopped).
// - Puddles (authority state in Game.puddles, a replicated copy on clients): a new one within
//   half a radius of a live puddle of the same owner refreshes that one instead; each owner keeps
//   puddles_per_player and the game puddles_max (the oldest goes). Every puddle_tick (250 ms) a
//   player standing in a puddle (within its radius on x / z, 2.8 units on y) takes
//   puddle_dps x tick / 1000 from its owner, once per tick however many puddles overlap. They go
//   after puddle_life (5 s), when their owner leaves, and at the round start.
// - Packets: NET_PACKET_SPAWNED_OBJ (object_manager.h) kind OM_KIND_PUDDLE, id = the puddle id,
//   owner = its owner's uid; an ADD carries NetPuddlePayload. Joiners get every live puddle
//   (piss_send_state). No new packet id.
#include <shared.h>

#define PISS_TRACE_STEP_MS 50.0f   // the stream's arc is traced in segments of this many ms
#define PISS_CROUCH_DROP 1.25f     // buildMustardPacket: sourceY - crouchVal * 1.25
#define PISS_PITCH_RISE 0.35f      // ... + sin(pitch) * 0.35
#define PISS_PUDDLE_LIFT 0.03f     // buildMustardPuddle: y = ground + 0.03
#define PISS_PUDDLE_REACH_Y 2.8f   // updateMustardPuddles: |target.y - puddle.y| <= 2.8
#define PISS_GROUND_PROBE 64.0f    // findPuddleGroundY: the terrain raycast's -64
#define PISS_PUDDLE_MERGE 0.5f     // KN: a puddle this many radii from one of the owner's refreshes it
#define PISS_SOUND_INTERVAL 0.22f  // fireMustard: the spray sound at most every 220 ms (client)

typedef enum {
    PISS_HIT_NONE = 0, // flew its whole life
    PISS_HIT_PLAYER,
    PISS_HIT_AI,
    PISS_HIT_OBJECT,
    PISS_HIT_TERRAIN,
} PissHitKind;

typedef struct {
    vec3 source;         // the hose tip (piss_stream_source)
    vec3 velocity;       // units/ms at the source
    float gravity;       // units/ms^2
    float time;          // ms the stream flew before it stopped
    vec3 end;            // where it stopped
    int hit;             // PissHitKind
    Player *player;      // PISS_HIT_PLAYER
    struct GameAI_t *ai; // PISS_HIT_AI
    const Object *object; // PISS_HIT_OBJECT
    int puddle;          // a puddle forms (a landing with ground below)
    vec3 puddle_at;      // its centre (ground + PISS_PUDDLE_LIFT)
} PissStream;

#pragma pack(push, 1)
typedef struct { // follows NetSpawnedObjPacket on an OM_KIND_PUDDLE ADD
    float position[3];
    float radius;
    int32_t color;      // 0xRRGGBB
    int16_t weapon_id;
    uint16_t reserved;
} NetPuddlePayload;
#pragma pack(pop)

// buildMustardPacket's source: pelvis_forward in front of the feet along the view, pelvis_height
// above them, lowered by the crouch and raised by sin(pitch) * 0.35.
vec3 piss_stream_source(const Player *player, const WeaponStream *stream, float pitch, float yaw);
// The stream's position `ms` after it left `source` with `velocity` (units/ms) under `gravity`.
vec3 piss_stream_at(vec3 source, vec3 velocity, float gravity, float ms);
// Traces the stream of `player`'s weapon along (pitch, yaw) (shared/src/player.c, next to the
// hitscan trace). include_players: players are hit (authority; lag compensated). 0: no stream weapon.
int player_stream_trace(Player *player, float pitch, float yaw, int include_players, PissStream *out);

// ---- authority
// A puddle of `owner`'s `weapon_id` at `at` (merged into a near one of the owner's when there is one).
// Returns its id, -1 when none could be made.
int piss_puddle_spawn(Game *game, const Player *owner, int weapon_id, vec3 at);
void piss_tick(Game *game, float delta);            // game tick (s): lifetimes and damage ticks
void piss_send_state(Game *game, int uid);          // a joiner: every live puddle
void piss_player_left(Game *game, const Player *player); // its puddles go
int piss_puddle_count(const Game *game, int owner); // live puddles of `owner` (-1: all)

// ---- both sides
void piss_round_start(Game *game); // finishInit: no puddles (clients get no REMOVE for these)
// Client: an OM_KIND_PUDDLE SPAWNED_OBJ (`payload` is the whole packet). 1 applied.
int piss_apply_spawned(Game *game, const void *payload, uint16_t length);
void piss_client_tick(Game *game, float delta); // client: puddle ages (the fade in)
