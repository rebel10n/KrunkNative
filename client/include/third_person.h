#pragma once
// KrunkNative third-person camera (client/src/third_person.c; doc/v7/decisions.md "Third person"):
// the console's `thirdperson` / `firstperson` (Source style) and the "Third Person" bind.
//
// v7.1.4 only has the host's thirdPerson game setting (index.js gameLoop: RENDER.camera at
// config.thrdPX/PY/PZ = 5, 2, 14 inside the look rig, then lookAt the eye). This is a per-player
// version of it on top of the normal first-person camera:
//   - the server decides (NET_CHEAT_THIRD_PERSON, shared/src/chat_commands.c `thirdperson`):
//     offline, devs, a custom game's host or everyone when the host sets knTPCam; never ranked
//     or public servers (the reason comes back as a notify);
//   - the camera sits at v7's offset in the look frame (right 5, up 2, back 14), but keeps the
//     look direction instead of v7's lookAt(eye), so the crosshair aims; it is pulled in front of
//     the first solid box / map triangle / terrain between the eye and that spot;
//   - the local player's model is the third-person one (player_mesh.c), drawn with the world;
//     no viewmodel;
//   - aim: the crosshair ray (from the camera, past the eye) finds the aim point and the input
//     look aims the eye at it, so the server's hitscan from the eye agrees;
//   - aiming down sights (any weapon, scopes too) goes back to first person, viewmodel and all,
//     until the aim is fully released (the local model is rebuilt each way, as a loadout change).
// The game's own thirdPerson setting and class camOffX/Y/Z keep v7's camera (not this module).
#include <client.h>

#define TP_OFFSET_X 5.0f      // config.thrdPX: right
#define TP_OFFSET_Y 2.0f      // config.thrdPY: up
#define TP_OFFSET_Z 14.0f     // config.thrdPZ: back
#define TP_WALL_GAP 1.0f      // the camera stays this far in front of what it was pulled in by
#define TP_AIM_RANGE 1000.0f  // the crosshair ray's length when it hits nothing
#define TP_EASE_OUT 0.012f    // per ms: the pulled-in camera eases back out (in: at once)
#define TP_MODEL_MIN_DIST 3.0f // pulled in closer than this (backed into a corner): the own model is not drawn

// The camera spot for an eye and a look (pitch, yaw), pulled in front of solid geometry.
// Returns the distance kept from the eye (0..|offset|); pure, for tests.
float third_person_camera_spot(const Game*, vec3 eye, float pitch, float yaw, vec3 offset, vec3 *out);
// The look (x = pitch, y = yaw) that aims the eye at what the crosshair ray from `camera` along
// `view` (pitch, yaw) hits first (solid geometry, other players), never behind the eye.
vec2 third_person_aim_look(const Game*, const Player *me, vec3 eye, vec3 camera, vec2 view);

// Console / bind: thirdperson asks the server (offline: the sandbox), firstperson is local.
void third_person_request(Client*);
void third_person_first_person(Client*);
void third_person_toggle(Client*);
// NET_PACKET_CHEAT NET_CHEAT_THIRD_PERSON from the server.
void third_person_on_grant(Client*, int on);
// New connection / round (INIT): the grant is the server's per round; a wanted camera asks again.
void third_person_on_init(Client*);

// The local model is built as third person: granted and not aiming (player_mesh.c reads it at
// build time; the frame hook rebuilds the mesh when it changes).
int third_person_model(void);
// The third-person view is on this frame (model mode, an active player, no other camera).
int third_person_view(const Client*);
// The local model goes into the world pass this frame (third_person_view, camera not pulled into the head).
int third_person_draw_model(const Client*);
// Per frame, before the input is sampled: rebuild the local mesh on a mode change, re-ask after INIT.
void third_person_frame(Client*);
// The input look to send for this view look (third_person_view: the aim correction; else `view`).
vec2 third_person_input_look(Client*, vec2 view);
// After camera_feel_apply: pose the camera from the view look (+ the feel's recoil / shake).
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
