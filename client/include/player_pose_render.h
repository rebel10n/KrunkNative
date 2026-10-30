#pragma once
// Krunker v10.0.3 player poses on the client (client/src/player_pose_render.c; the pose itself and the
// authority's side: shared/include/player_pose.h). Line numbers: deob-10.0.3.js.
//   NET_EXT_PLAYER_POSE ("pz", init plrPose) -> players.setPose (:195354-195382): the player's pose, kept
//     pending by uid until that player exists (pendingPoses, applied on players.add :195135-195139)
//   per frame, from player_update_meshes -> players.applyPose (:193340-193436), refreshWeaponVisibility
//     (:195386-195425), syncHands (:193452-193476), syncHandL (:193527-193549)
#include <client.h>
#include <stdint.h>

// NET_EXT_PLAYER_POSE body (NetPlayerPosePacket)
void player_pose_on_ext(Client *client, const uint8_t *body, uint16_t length);
// players.setPose for a player the client has: `pose` NULL / has 0 clears it (the model's arms, body
// height and hidden weapon meshes back)
void player_pose_client_set(Player *player, const PlayerPose *pose);
// A new INIT: the pending poses are forgotten with the old session's players
void player_pose_reset(void);
// players.remove(sid): a pose still pending for that player goes with it (deob-10.0.5.js:174098, new in
// 10.0.5: a later player with the same sid took it over, holding the Candy Heist arm pose)
void player_pose_forget(int uid);

// The pose on `player`'s model this frame (player_update_meshes, after the movement's own pose of the
// legs, arms and upper body; before the charm / GLTF visibility pass). first_person: renderYou && not a
// third-person camera (v10 IIìîìíï); step_anim: the leg swing sin(stepVal) * stepAnim after the crouch
// scale (v10 IIîìîiì), which pose.bob 0 takes back off the per-arm meshes.
void player_pose_render(Player *player, int first_person, float step_anim);
// The yaw a faceRide pose turns the model to (the hard-attached entity's, entities.c); 0 = none
int player_pose_face_ride_yaw(const Player *player, float *yaw);

// players.getHands / getHandL: the "hands" / "handL" attach points of the model (NULL without meshes)
MeshTransform *player_pose_hands_anchor(Player *player);
MeshTransform *player_pose_hand_l_anchor(Player *player);
// syncHandL copies the arm's layers.mask onto the "handL" anchor and everything under it (:193540-193546):
// 1 when that arm is a first-person one (the local first-person model, drawn by the viewmodel pass, fps_scene),
// so what hangs there is drawn in that pass too. syncHands copies no layer: "hands" stays in the world pass.
int player_pose_hand_l_first_person(Player *player);
// One probe record (the input script's `poses`): "has=0x.. hold=y l=x,y,z r=x,y,z merged=x,y,z" (the arm pivots'
// rotations of the slot in hand, the merged arms' kept rotation); "-" without a model
void player_pose_describe(const Player *player, char *out, size_t n);
