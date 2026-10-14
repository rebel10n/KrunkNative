#pragma once
// KrunkNative Object Manager motion on this client (client/src/om_interp.c): other players' edits
// (SPAWNED_OBJ OM_SPAWNED_UPDATE / MAP_EDIT, 20-60 a second while they carry or drag) ease between
// updates (object_manager_interp_*, one update of delay) instead of stepping, and our own carry / drag
// shows where it is this frame (client-side prediction: the Physics Gun and the Move tool's pose from
// om_build.c, a carried player's feet), not where the server's last echo put it. Only the drawn mesh
// (and a carried player's drawn position) moves; collision, picking and the game state stay the
// server's.
#include <client.h>
#include <object_manager.h>

// SPAWNED_OBJ handling (object_manager_ui.c): before the packet is applied, where object `uid` shows;
// after it, the move to ease (a fresh mesh at the new pose)
int om_interp_shown(const Client *client, int uid, vec3 *feet, float *yaw); // 1 an object is there
void om_interp_moved(Client *client, int uid, vec3 shown, float shown_yaw);
// Per frame after map_post_frame, before the world pass: the meshes at their eased / predicted poses
void om_interp_frame(Client *client);
void om_interp_reset(void); // a new map / connection

// om_build.c: our own carry / drag this frame. 1 an object `*uid` at *pose (Physics Gun, Move), 2 a
// player `*uid` with its feet at pose->position (the Physics Gun, devs), 0 none
int om_build_carry(int *uid, ObjectManagerPose *pose);
// om_build.c: a carry / drag just ended at `pose` (object `uid`): it stays shown there until the
// server's last echo lands (no jump back to an older echo)
void om_interp_released(Client *client, int uid, const ObjectManagerPose *pose);
