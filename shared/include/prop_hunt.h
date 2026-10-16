#pragma once
// Prop Hunt (Krunker v10.0.2 mode "prop", shared/src/modes/modes.c MODE_ID_PROP): the Prop team
// disguises as a map prop, the Seekers hunt them. Ported from the live bundle (game.deob.js
// 10.0.3, same as v10.0.2 here): players.isValidProp / randomProp / updateProp (:171831-171847),
// player.init's propID / propSwitches / propRot / propRotSaved (:169177, :169266-169268), the
// Seeker penalty for shooting / hitting a prop object (:172554, :173031), the taunt timer
// (game.nextTaunt :189480) and the keys (:221435-221446, :221490). The server's "prp" / "prpR"
// handlers are not in the client bundle: v7.1.4's server (handlePropPick / handlePropRotation)
// stands in.
//
// The prop list is the map editor's prefab table filtered by isProp (PF_ISPROP, :156778): 14 prefabs.
// updateProp sets player.scale (the collision and hit box half width) and height to the prefab's
// scale + CP; v10's updateHeight puts the height back every tick, so only scale lasts.
#include <shared.h>
#include <net_ext.h>
#include <stddef.h>
#include <stdint.h>

#define PROP_DEFAULT_ID 1          // player.init: propID = 1 (CRATE)
#define PROP_SEEKER_PENALTY 10.0f  // changeHealth(seeker, null, 10) for hitting a prop object
#define PROP_TAUNT_INTERVAL 20.0f  // nextTaunt: every 20 s ("next taunt in ")
#define PROP_CAM_DISTANCE 30.0f    // the Prop team's camera: z 30 (iîiïìïi = 30, reset by startGame)
#define PROP_CAM_MIN 10.0f         // scroll: += 5 x scrollDelta, clamped to [10, 100]
#define PROP_CAM_MAX 100.0f
#define PROP_CAM_STEP 5.0f
#define PROP_WHEEL_DELAY 0.2f      // propWheelReq 200 ms: the wheel shows once the key is held this long

// The props in prefab order (overlay.propList): count, the prefab id at an index, the index of an id (-1).
int prop_count(void);
int prop_at(int index);
int prop_index_of(int prop_id);
// players.isValidProp: a PF_ISPROP prefab.
int prop_is_valid(int prop_id);
// The prefab's name ("CRATE"), its model ("crate_0": name lowercased + "_0"), its model scale and the
// model's y offset (scale + CPY), and the player size updateProp gives (scale + CP).
const char *prop_name(int prop_id);
void prop_model_src(int prop_id, char *out, size_t size);
float prop_model_scale(int prop_id);
float prop_model_y(int prop_id);
float prop_size(int prop_id);
// players.randomProp: a uniformly random prop.
int prop_random(void);

// teamOptions[team - 1] == "Seekers" (the Prop team: game_player_is_prop, social_v7.h).
int game_player_is_seeker(const Game*, const Player*);
// The yaw the prop model shows: propRot ? the look : propRotSaved, else 0 (players.js :170946).
float prop_model_yaw(const Player*);

// player.init: propID 1, no switches, rotation off and no saved yaw; scale back to the player's.
void prop_reset(Player*);
// updateProp's fields of the player (both sides): propID, scale and height.
void prop_apply(Player*, int prop_id);

// ---- the authority (server, offline sandbox)
// players.add: a Prop-team spawn wears a random prop (enterGame's randomProp(), :208236 / :171957).
void game_prop_spawned(Game*, Player*);
// "prp" propID: a valid prop other than the one worn, while switches are left (else the limit line).
// 1 = the prop changed.
int game_prop_pick(Game*, Player*, int prop_id);
// "prpR" [reset]: toggle the rotation (turning it off keeps the current look), or reset it. 1 = done.
int game_prop_rotate(Game*, Player*, int reset);
// A Seeker's shot or swing hit a prop object of the map: 10 damage to the Seeker, killing it at 0.
void game_prop_seeker_hit(Game*, Player *dealer, const Object *object);
// The player's prop (and rotation) to `target` (a uid or NET_TARGET_ALL): after its SPAWN.
void game_prop_send(Game*, const Player*, int target);
// The packets themselves (an EXT envelope in out): 0 when out is too small.
uint16_t prop_pack_update(const Player*, int limit, uint8_t *out, size_t size);
uint16_t prop_pack_rotation(const Player*, int reset, uint8_t *out, size_t size);

// "next taunt in ": every Prop-team player taunts (forced), then the next timer (game.nextTaunt).
void mode_trigger_next_taunt(Game*, float overflow);
// prop.js's second wait timer: releaseSeeker(), then nextTaunt(null, null, true) (the timer only).
void mode_trigger_release_seekers_prop(Game*, float overflow);
