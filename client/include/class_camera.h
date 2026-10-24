#pragma once
// Krunker v10.0.3's class camera (client/src/class_camera.c the pure math, class_camera_client.c the
// client glue). A map's or a host's cls[id] gives a class its "Camera" keys (deob-10.0.3.js:20162-20252):
// camType (FPS, 3rd Person, Platformer (X), Platformer (Z), Orthographic), camOffX/Y/Z, orthoF,
// lockCamP (Face Player), lckCmYR (Lock Y Rot), yCmLim (Y Limit), camRot (Mouse Rotation) and fPLock
// (Force Pointer Lock). v10 reads them in:
//   - updateCamMouse (deob-10.0.3.js:257863-257881, on our spawn / class change): CONTROLS.camOffset
//     (any offset), camRot, UsePointerLock = (camRot || !camOffset || fPLock) && !forceFreeCam,
//     freeMouse = !UsePointerLock, the camType flags, RENDER.setOrto(isOrto, orthoF) and
//     RENDER.aimFov = isNormal || shtToCam (RENDER.zoom is 1 without it, deob:231411-231420);
//   - updateMouseInp (deob:235468-235481): the mouse turns the controls only with camRot;
//   - the game loop (deob:260589-260857): RENDER.camera at (0, 0, 0) / thirdPerson's (5, 2, 14) /
//     the Prop's (0, 0, d), plus the class offset, inside CONTROLS.pchObjc (the pitch object under the
//     yaw object at the eye); camera.lookAt(eye) whenever it has an offset; with freeMouse the cursor's
//     ray on the head plane (3rd Person) or the player's vertical plane (Platformer X / Z) aims;
//     rotateCam (the shake / recoil) after it unless freeMouse, so a mouse-rotated offset camera looks
//     parallel to the look; on input frames lockCamP's chase of the body yaw, lckCmYR and yCmLim set
//     the controls' yaw / pitch.
//   - the Orthographic camera (deob:230982-230983, 231404-231410, 231450-231458) is RENDER's second
//     camera: frustum orthoF x orthoF * aspect, near 0.1, far 2000, and never added to pchObjc (only the
//     perspective camera is, deob:235422): its position is the offset in world coordinates.
//   - renderYou = isYou && isNormal (deob:190538-190542); the model is the third-person one with
//     thirdPerson, a camera offset or !isNormal (updateMsh deob:193600, generateMeshes deob:194102) and
//     the scope / aim dot / scope zoom of toggleAim only show for isNormal (deob:195320-195340).
// v10 has no camera collision, smoothing or zoom here (only the Prop's wheel distance): the class
// camera goes through walls as v10's does (KrunkNative's own third person, third_person.h, keeps its
// pull-in).
//
// Deliberate differences:
//   - a detached KrunkScript camera (CAMERA.detach) ignores the class offset: v10 adds camOffX / Y / Z to
//     camera.position every frame while detached (its reset sits under !detachedCam), so it drifts away;
//   - the lookAt aims at this frame's eye: v10's runs before moveCam, on the last frame's controls;
//   - updateCamMouse's result is read every frame from the local player's class, so KrunkScript's
//     INPUTS.freeMouse applies at once (v10: at the next spawn / class change);
//   - v10's renderYou is false for camTypes 1-4; KrunkNative keeps render_you (its prediction feel:
//     the landing bob, the eye height) and only builds the third-person model and draws it with the world;
//   - updateMsh's third-person test reads each player's own class (v10 the local controls' camType);
//   - rotateCam's feel and the wall lean's roll are summed into KrunkNative's Euler angles (the roll in
//     world space, not on the yaw object, so it does not swing an offset camera);
//   - the controls' yaw / pitch without camRot follow the look's resync (spawn, a teleport's facing,
//     KrunkScript CAMERA.lookAt), where v10 only resets them on the spawn and lookAt.
#include <client.h>

// camType (deob-10.0.3.js:20162-20169; controls.camType, deob:257869-257873)
enum {
    CLASS_CAMERA_FPS = 0,         // isNormal
    CLASS_CAMERA_THIRD = 1,       // isTop ("3rd Person": the cursor aims on the head plane)
    CLASS_CAMERA_SIDE_X = 2,      // isSideX ("Platformer (X)": the cursor aims in the plane z = player.z)
    CLASS_CAMERA_SIDE_Z = 3,      // isSideZ ("Platformer (Z)": the plane x = player.x)
    CLASS_CAMERA_ORTHO = 4,       // isOrto ("Orthographic")
};

#define CLASS_CAMERA_ORTHO_NEAR 0.1f   // new OrthographicCamera(..., 0.1, 2000) (deob:230983)
#define CLASS_CAMERA_ORTHO_FAR 2000.0f
#define CLASS_CAMERA_TP_X 5.0f         // thirdPerson: camera.position.set(5, 2, 14) (deob:260593)
#define CLASS_CAMERA_TP_Y 2.0f
#define CLASS_CAMERA_TP_Z 14.0f

// What updateCamMouse makes of the local player's class (plus the game's thirdPerson).
typedef struct {
    int type;           // camType 0..4 (out of range: FPS)
    vec3 offset;        // camOffX / Y / Z (camera.position += them while has_offset)
    int has_offset;     // CONTROLS.camOffset
    int rotation;       // CONTROLS.camRot: the mouse turns the controls
    int free_mouse;     // CONTROLS.freeMouse: no pointer lock, the cursor aims (3rd Person / Platformer)
    float ortho_f;      // orthoF: the Orthographic frustum height
    float face_player;  // lockCamP
    float lock_y_rot;   // lckCmYR
    float y_limit;      // yCmLim
    int aim_fov;        // RENDER.aimFov: the ADS zoom reaches the camera
    int third_person;   // the game's thirdPerson setting
} ClassCamera;

// Pure: the camera of a class (`o` = class_overrides(), NULL = the defaults) with the game's
// thirdPerson and KrunkScript's INPUTS.freeMouse (CONTROLS.forceFreeCam).
ClassCamera class_camera_make(const ClassOverrides *o, int third_person, int force_free);
// The class camera changes anything about the plain first-person camera.
int class_camera_active(const ClassCamera*);
// The local player's model is the third-person one and is drawn with the world, no viewmodel:
// thirdPerson, a camera offset or a camType other than FPS (deob:193600, 194102).
int class_camera_third_person(const ClassCamera*);

// One frame's rig: the controls (object at the eye, yaw object, pitch object) and the camera's local
// offset before the class's (thirdPerson / the Prop's distance).
typedef struct {
    vec3 eye;           // CONTROLS.object.position: x, y + height - cameraHeight, z (moveCam)
    float pitch, yaw;   // pchObjc.rotation.x, object.rotation.y
    vec3 local;         // RENDER.camera.position before the class offset
    int local_offset;   // thirdPerson / the Prop set it (iîÌìïii: the lookAt runs)
    float feel_pitch;   // rotateCam: shakeY + swayY + recoilAnimY * recoilMlt + landBobY * 0.1
    float feel_yaw;     // rotateCam: shakeX + swayX
    float roll;         // the wall lean (object.rotation.z)
} ClassCameraRig;

// Pose `camera` (position, rotation, ortho; near / far for the Orthographic one) for that rig.
// Returns 1 when the camera looks at the eye (lookAt kept: freeMouse with an offset).
int class_camera_pose(const ClassCamera*, const ClassCameraRig*, Camera *camera);

// The camera ray through the cursor (ndc: -1..1, y up) on the aim plane: 3rd Person the head plane
// (y = player.y + height), Platformer X the plane z = player.z, Platformer Z x = player.x
// (deob:260650-260685). `hit` keeps the last hit (three's intersectPlane leaves it when the ray
// misses). Writes the aim (input yaw / pitch) and returns 1 for those three types, else 0.
int class_camera_free_aim(const ClassCamera*, const Camera *camera, float aspect, float ndc_x, float ndc_y,
                          vec3 player, float player_height, vec3 *hit, float *yaw, float *pitch);

// lockCamP (deob:260824-260836): on an input frame the chase yaw (NAN = unset, player.camChase null)
// moves toward the body's yaw by the wrapped difference x lockCamP x frame_ms / 100; returns it (the
// controls' yaw). Unchanged without lockCamP.
float class_camera_face_player(const ClassCamera*, float *chase, float body_yaw, float frame_ms);
// lckCmYR then yCmLim on the controls' pitch (deob:260837-260845).
float class_camera_lock_pitch(const ClassCamera*, float pitch);

// ---- client glue (class_camera_client.c)
// The local player's class camera this frame (the defaults without an active player).
ClassCamera class_camera_current(const Client*);
// The pointer lock owner: the class camera wants a free cursor (freeMouse).
int class_camera_wants_free_cursor(const Client*);
// The local model is the third-person one / drawn in the world pass this frame.
int class_camera_draws_own_model(const Client*);
// player_mesh.c's build: the local player's model is the third-person one (thirdPerson, a class
// camera offset or a camType other than FPS).
int class_camera_player_third_person(const Player*);
// main.c, before the input frame: the free-mouse aim (the input's look) from the cursor; 0 = none.
int class_camera_input_aim(Client*, vec2 look, vec2 *aim);
// main.c, after the tick of a sampled input frame: lockCamP / lckCmYR / yCmLim on the controls' look.
void class_camera_after_input(Client*, vec2 *look, float frame_ms);
// main.c, before the frame's camera is posed: the perspective projection back (class_camera_apply
// puts the Orthographic one on again while it is wanted).
void class_camera_frame_begin(Client*);
// main.c, after camera_feel_apply: pose the camera from the controls' look. Returns 1 when it did
// (the class camera is active), 0 = the first-person camera stands.
int class_camera_apply(Client*, vec2 look);
// Our (re)spawn: player.camChase = null (deob:190600), the aim's plane hit forgotten.
void class_camera_on_spawn(void);
// The look restarts from the player's (spawn, a teleport's facing: main.c's resync): CONTROLS.reset puts
// the controls' yaw / pitch there, which a camera without camRot keeps from then on.
void class_camera_look_reset(void);
