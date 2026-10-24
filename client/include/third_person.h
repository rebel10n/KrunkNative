#pragma once
// KrunkNative third-person camera (client/src/third_person.c; doc/v7/decisions.md "Third person"):
// the console's `thirdperson` / `firstperson` (Source style) and the "Third Person" bind.
//
// Krunker v10.0.2 only has the host's thirdPerson game setting (v10.js 221455-221479 gameLoop:
// RENDER.camera.position.set(5, 2, 14) = config.thrdPX/PY/PZ (v10.js 19999-20001) inside
// CONTROLS.pchObjc, the pitch object under the yaw object at the eye (202354-202357), then
// camera.lookAt(eye); but rotateCam (202791) sets camera.rotation to the shake / recoil right
// after it in the same frame (221597 / 221613), so the camera looks along the player's look,
// parallel to it at that offset). This is a per-player version of it on top of the normal
// first-person camera:
//   - the server decides (NET_CHEAT_THIRD_PERSON, shared/src/chat_commands.c `thirdperson`):
//     offline, devs, a custom game's host or everyone when the host sets knTPCam; never ranked
//     or public servers (the reason comes back as a notify);
//   - the camera sits at v10's offset in the look frame (right 5, up 2, back 14) and looks along
//     the look (pitch, yaw) plus the camera feel (recoil, shake), as v10's does once rotateCam ran:
//     pitching swings it over / under the eye, never sideways, and never turns the player;
//   - KrunkNative addition: it is pulled in front of the first solid box / map triangle / terrain
//     between the eye and that spot (v10 has no camera collision: it goes through walls);
//   - the local player's model is the third-person one (player_mesh.c), drawn with the world;
//     no viewmodel (v10 syncHands(renderYou && !thirdPerson), 170835);
//   - aim: the input look is the view look, unchanged (v10 has no aim correction): shots leave
//     the eye along it, parallel to the crosshair ray 5 right and 2 up;
//   - aiming down sights stays third person (v10 has no ADS offset): the world camera zooms as in
//     first person (updateAim: 1 + (zoom - 1) * aim * adsFov; toggleAim: a scope's full zoom once
//     aimed, 172050-172063), a scope shows its overlay and hides the held weapon (toggleAim), and
//     the crosshair stays while aiming except with a scope (updateCrosshair, 221608).
// The game's own thirdPerson setting, class camOffX/Y/Z and camTypes other than FPS are v10's class
// camera (client/include/class_camera.h), not this module.
#include <client.h>

#define TP_OFFSET_X 5.0f      // config.thrdPX: right
#define TP_OFFSET_Y 2.0f      // config.thrdPY: up
#define TP_OFFSET_Z 14.0f     // config.thrdPZ: back
#define TP_WALL_GAP 1.0f      // the camera stays this far in front of what it was pulled in by
#define TP_EASE_OUT 0.012f    // per ms: the pulled-in camera eases back out (in: at once)
#define TP_MODEL_MIN_DIST 3.0f // pulled in closer than this (backed into a corner): the own model is not drawn

// The camera spot for an eye and a look (pitch, yaw), pulled in front of solid geometry.
// Returns the distance kept from the eye (0..|offset|); pure, for tests.
float third_person_camera_spot(const Game*, vec3 eye, float pitch, float yaw, vec3 offset, vec3 *out);

// Console / bind: thirdperson asks the server (offline: the sandbox), firstperson is local.
void third_person_request(Client*);
void third_person_first_person(Client*);
void third_person_toggle(Client*);
// NET_PACKET_CHEAT NET_CHEAT_THIRD_PERSON from the server.
void third_person_on_grant(Client*, int on);
// New connection / round (INIT): the grant is the server's per round; a wanted camera asks again.
void third_person_on_init(Client*);

// The local model is built as third person: granted (player_mesh.c reads it at build time; the
// frame hook rebuilds the mesh when it changes).
int third_person_model(void);
// The third-person view is on this frame (model mode, an active player, no other camera).
int third_person_view(const Client*);
// The local model goes into the world pass this frame (third_person_view, camera not pulled into the head).
int third_person_draw_model(const Client*);
// Per frame, before the input is sampled: rebuild the local mesh on a mode change, re-ask after INIT.
void third_person_frame(Client*);
// After camera_feel_apply: pose the camera from the view look (+ the feel's recoil / shake), keeping
// its ADS zoom.
void third_person_apply_camera(Client*, vec2 view, float delta);
// Our (re)spawn: the camera starts at its distance (no ease out from where the last life's was).
void third_person_on_spawn(void);

// What the last rendered frame did (main.c notes it; the input script's `tpstate` logs it):
// the local model in the world pass, the first-person (viewmodel) pass drawn.
void third_person_note_frame(int own_model, int viewmodel);
typedef struct {
    int view, model, own_model, viewmodel;
    float keep;     // the camera's distance from the eye (0 in first person)
} ThirdPersonFrame;
ThirdPersonFrame third_person_last_frame(const Client*);
