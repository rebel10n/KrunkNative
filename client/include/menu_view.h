#pragma once
// The world behind the menu (Krunker v3.1.1 index.js gameLoop): the scene keeps
// rendering and updating in every menu state; only the camera rule changes.
//   before the first spawn (`!player && !spectating`): map camPos, yaw menuCameraYaw
//     += deltaMs * 0.0001, pitch 0 (CONTROLS.rotateCam(menuCameraYaw, 0, 0))
//   in game with the menu open (ESC): the player's own view; procInputs keeps running
//     with the keys reset (CONTROLS.resetKeys on pointer-lock loss)
//   dead: CONTROLS.camLookAt(killer) in g1() starts the chase camera that
//     CONTROLS.update(deltaMs) eases every frame until CONTROLS.reset() (next spawn); the mouse turns
//     the rig meanwhile (CONTROLS.updateMouseInp, menu_view_look), with or without a killer
//   round end while playing/spectating: deltaMs *= endAnimationTimeLeft / config.endAnim
//     (5 s slow motion, then the world holds still behind the results)
// Camera/time state only: client/src/menu_view.c. main.c's client_tick_game drives it.
#include <client.h>

// g1() for the local victim: CONTROLS.camLookAt(killer.x, killer.y + killer.height / 1.5, killer.z).
// NULL killer (suicide, fall): no chase, the camera stays at the death pose.
void  menu_view_death_cam(Client*, const Player *killer);
// CONTROLS.reset(): drop the death view and the chase target (spawn, new round, spectating).
void  menu_view_reset(void);
// The death view is up (menu_view_death_cam ran and no spawn / spectate since): the rig takes the look.
int   menu_view_dead(void);
// CONTROLS.updateMouseInp while dead: the mouse turns the death rig (ui_controls_fill_look sensitivity,
// 0.2 of it while the chase runs (a killer known), pitch clamped to +-PI/2). dx / dy are the frame's
// look deltas, 0 when the pointer is not locked.
void  menu_view_look(Client*, float dx, float dy);
// Per frame: the frame delta the world runs at (end-of-round slow motion), in seconds.
float menu_view_world_delta(Client*, float delta);
// The current world time scale (endAnimationTimeLeft / endAnim, 1 outside the slow motion):
// also SOUND.rate (audio_set_rate).
float menu_view_time_scale(void);
// The player who killed us while the death camera runs (g1 sets killer.isSpectating = true:
// its nametag is forced visible), -1 otherwise. For the overlay nametags once they exist.
int   menu_view_killer_uid(void);
// Per frame, after game_tick: pose the camera for the states without an active player
// (pre-spawn menu camera, death chase camera). Spectating keeps spectate.c's camera.
void  menu_view_camera(Client*, float delta);
