#pragma once
// First-person camera feel (client/src/camera_feel.c): the camera half of Krunker v3.1.1's
// input frame (src/index.js gameLoop after player.procInputs: rotateCam / moveCam),
// the ADS zoom (players.js updateAimZoom / toggleAim) and the renderer's screen shake
// (src/libs/render.js shake / updateShake).
#include <client.h>

// RENDER.shake(v): restart the shake at amplitude v (radians). The nuke uses 1.
void camera_feel_shake(float amount);
// index.js explosion handler: shake by (1 - distance(camera, position) / explosionRange) * 0.14.
void camera_feel_explosion(const Camera*, vec3 position);
// Pose client->camera from client->me after its input: eye position, look + recoil
// (recoilAnimY * recoilMlt) + landing bob (landBobY * 0.1) + shake, and the ADS zoom.
void camera_feel_apply(Client*);
// wallLean: CONTROLS.object.rotation.z on a wall slide (the camera block, before the script's update;
// frame delta in ms); its current value; KrunkScript's CAMERA.rotation.z sets it
void camera_feel_wall_lean(Client*, float delta_ms);
float camera_feel_wall_roll(void);
void camera_feel_set_wall_roll(float roll);
// RENDER.updateShake(delta): once per rendered frame; delta in seconds (clamped like Krunker's).
void camera_feel_update(float delta);
// The current shake offsets (RENDER.shakeX / shakeY) camera_feel_apply adds to the look.
void camera_feel_shake_offset(float *pitch, float *yaw);
