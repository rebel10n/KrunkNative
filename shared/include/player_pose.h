#pragma once
// Krunker v10.0.3 player poses (KrunkScript `player.pose`, the "pz" PLAYER_POSE packet) and
// game.applyFreezeOthers. A pose is a script's seat / skeletal override of a player model
// (sitting in a vehicle, an emote, holding an item): which legs show and their angle, the body
// lowered (bodyY), the torso and the arms turned, the weapon model hidden, items held in both hands
// ("hands" attach point) or in the left one while the weapon stays (holdL, "handL", new in 10.0.3).
//
// Line numbers: deob-10.0.3.js (Krunker_v10.0.3/deob/game.deob.js).
//   game.sanitizePose :220978-221034   the server's clamp of the script's object (PlayerPose below)
//   game.setPlayerPose :221035-221042  poseData + broadcast PLAYER_POSE sid, pose || 0
//   endGame :221901-221903             every posed player's pose cleared (pose 0 broadcast)
//   init syncData.plrPose :220294-220302, client :256989-256993  a joiner gets every pose
//   players.setPose :195354-195382     the client: poseLerp reset, split arms, pendingPoses by sid
//   players.applyPose :193340-193436   the render (client/src/player_pose_render.c)
//   players.updateHeight :193560-193566 the local camera lowered by bodyY (client only)
//   game.applyFreezeOthers :221547-221573, freezeOthers reset at startGame :219811
//
// The pose itself is engine state (Player.pose: the authority's script sets it, the packet carries
// it, the client's model and camera read it); its smoothing (Player.pose_lerp) is the client's.
#include <math.h>
#include <stdint.h>

struct Game_t;
struct Player_t;

// The keys a sanitized pose carries (sanitizePose copies only those given, so absence matters:
// `legs` absent keeps the legs as the movement code left them, `legX` absent keeps their swing).
enum {
    PLAYER_POSE_LEGS = 1 << 0,      // legs: 0 no legs, 1 standing legs, 2 crouched legs (round(parseFloat) clamped 0..2)
    PLAYER_POSE_WEAPON = 1 << 1,    // weapon: 0 hides the weapon model (isWeaponModelHidden)
    PLAYER_POSE_BOB = 1 << 2,       // bob: 0 cancels the per-arm step swing
    PLAYER_POSE_FACE_RIDE = 1 << 3, // faceRide: the model faces the attached entity's yaw
    PLAYER_POSE_HOLD = 1 << 4,      // hold: both hands hold an item ("hands"); weapon model hidden, arms at -0.25
    PLAYER_POSE_HOLD_L = 1 << 5,    // holdL (10.0.3): the left hand holds an item ("handL"), the weapon stays
    PLAYER_POSE_LEG_X = 1 << 6,     // legX: every visible leg's x angle, +-3.2
    PLAYER_POSE_BODY_Y = 1 << 7,    // bodyY: the whole model (and the local camera) moved up / down, +-12
    PLAYER_POSE_TORSO_X = 1 << 8,   // torsoX: added to the upper body's x angle, +-1.6
    PLAYER_POSE_ARM_L = 1 << 9,     // armL: [x, y, z] turn of the left arm about its shoulder, +-3.2 each
    PLAYER_POSE_ARM_R = 1 << 10,    // armR: the same for the right arm
    PLAYER_POSE_KEYS = (1 << 11) - 1,
};

// sanitizePose's limits (deob-10.0.3.js:221010-221028)
#define PLAYER_POSE_LEG_X_MAX 3.2f
#define PLAYER_POSE_BODY_Y_MAX 12.0f
#define PLAYER_POSE_TORSO_X_MAX 1.6f
#define PLAYER_POSE_ARM_MAX 3.2f
#define PLAYER_POSE_LERP 0.25f // applyPose: poseLerp eases 0.25 of the way per rendered frame (:193370)

typedef struct {
    uint16_t has;               // PLAYER_POSE_* keys present; 0: no pose (poseData null)
    uint8_t legs;               // 0..2
    uint8_t weapon, bob, face_ride, hold, hold_l; // 0 / 1
    float leg_x, body_y, torso_x;
    float arm_l[3], arm_r[3];
} PlayerPose;

// players.applyPose's poseLerp (client): created from the pose when it starts (arms from 0), reset
// when a pose starts or ends (setPose :195364-195366)
typedef struct {
    unsigned char active;
    float leg_x, body_y, torso_x;
    float arm_l[3], arm_r[3];
} PlayerPoseLerp;

// sanitizePose's number rule (iîÍiïíi :220986-220993): parseFloat, then Math.max(-max, Math.min(max,
// Math.round(v * 1000) / 1000)); 0 when not finite (the key is dropped). JS Math.round: halves round up.
// Inline: the official link's translation (krunker_link_script.c) uses it without the game code.
static inline int player_pose_clamp(const double value, const float max, float *out) {
    if (!isfinite(value)) return 0;
    double v = floor(value * 1000.0 + 0.5) / 1000.0;
    if (v > max) v = max;
    if (v < -max) v = -max;
    *out = (float) v;
    return 1;
}

// The pose's arm target for side 0 (left) / 1 (right): the given armL / armR, else hold's
// [-0.25, 0, 0], else holdL's left [-1.2, 0, -0.6] (third person; :193375, IIïiîîi / IIìiïïï.tp), else 0
void player_pose_arm_target(const PlayerPose *pose, int side, float out[3]);
// applyPose's IÎíïíîí: the arms move apart (armL, armR or holdL), each about its own shoulder
int player_pose_splits_arms(const PlayerPose *pose);
// players.isWeaponModelHidden (:195350): weapon === 0 or hold
int player_pose_hides_weapon(const PlayerPose *pose);

// game.setPlayerPose (authority): `pose` (NULL or has == 0 clears it) becomes the player's pose and
// every connection gets NET_EXT_PLAYER_POSE. 0 when not the authority or no player.
int game_set_player_pose(struct Game_t *game, struct Player_t *player, const PlayerPose *pose);
// The NET_EXT_PLAYER_POSE envelope of `player`'s pose (pose 0 when it has none) into `out`
// (NET_EXT_HEADER_SIZE + sizeof(NetPlayerPosePacket)); its length, 0 when it does not fit.
uint16_t player_pose_pack(const struct Player_t *player, uint8_t *out, uint16_t size);
// endGame: every posed player's pose cleared (and broadcast)
void game_clear_player_poses(struct Game_t *game);

// game.applyFreezeOthers(freeze, exceptId) (authority): every player but `except_uid` (-1: none) is
// frozen (spectators skipped) or, unfreezing, every frozen one is let go; each gets "frz" and a
// frozen live player is held where it stands (changePosition recon + stop, "chgp"). Returns how many
// players changed. Game.freeze_others keeps the request until the next round's start.
int game_apply_freeze_others(struct Game_t *game, int freeze, int except_uid);
// What applyFreezeOthers (:221560-221564) and the client's "frz" (:258510-258518) write on a player:
// player.freeze (Player.frozen), defaultMovement / defaultVelocity = !freeze and disableShooting /
// disableMelee = freeze, the script's own flags (its KsRef, created when the game has no script yet).
// An unfreeze turns all four on whatever the script had set; a respawn's reset turns the movement on.
void player_apply_freeze(struct Player_t *player, int freeze);
