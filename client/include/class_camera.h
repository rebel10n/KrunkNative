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
// KrunkScript's GAME.CAMERA moves the same chain (CONTROLS.object position / rotation, pchObjc, RENDER.camera;
// deob:210533-210675); the script's update runs after the camera block (deob:222267-222270), so its writes show
// on the frame they are made, attached or detached. While detached the camera block leaves the chain alone
// but still adds camOffX / Y / Z to camera.position every frame (deob:260591-260646), as v10 does.
//
// Deliberate differences:
//   - v10's renderYou is false for camTypes 1-4; KrunkNative keeps render_you, which its shared prediction
//     reads (the landing bob, the eye height of a pose), and only builds the third-person model and draws it
//     with the world as v10's third-person build does;
//   - KrunkNative's death and menu cameras (menu_view.c) are not CONTROLS: GAME.CAMERA writes while no life is
//     on take effect at the next life (object.rotation.x / z, the controls' yaw until the spawn resets it),
//     and a detach without a life starts from the camera shown.
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
    int mouse_look;     // updateMouseInp turns them: camRot && !detachedCam (class_camera_current)
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

// One frame's rig: CONTROLS.object (at the eye, rotation XYZ: x a KrunkScript tilt, y the yaw, z the wall
// lean's roll) holding pchObjc (rotation.x, the pitch) holding RENDER.camera (deob-10.0.3.js:235419-235424);
// the camera's local offset before the class's (thirdPerson / the Prop's distance).
typedef struct {
    vec3 eye;           // CONTROLS.object.position: x, y + height - cameraHeight, z (moveCam)
    float pitch, yaw;   // pchObjc.rotation.x, object.rotation.y
    float tilt, roll;   // object.rotation.x (only KrunkScript's CAMERA.rotation.x), object.rotation.z (wallLean)
    vec3 local;         // RENDER.camera.position before the class offset
    int local_offset;   // thirdPerson / the Prop set it (iîÌìïii: the lookAt runs)
    float feel_pitch;   // rotateCam: shakeY + swayY + recoilAnimY * recoilMlt + landBobY * 0.1
    float feel_yaw;     // rotateCam: shakeX + swayX
} ClassCameraRig;

// The whole chain at render time: the rig and RENDER.camera's own position / rotation (local to pchObjc;
// world coordinates for the Orthographic camera, which has no parent).
typedef struct {
    ClassCameraRig rig;
    vec3 camera_position;
    float camera_rotation[9];   // row-major
    int ortho;
} ClassCameraChain;

// Rotations (row-major 3x3): three's Euler "XYZ" (Rx Ry Rz), products, and the world rotation as
// KrunkNative's camera angles (camera.c renders Rz(roll) Ry(yaw) Rx(pitch); the smaller-roll solution,
// the yaw within a turn of `yaw_hint`).
void class_camera_m3_identity(float m[9]);
void class_camera_m3_mul(const float a[9], const float b[9], float out[9]);
void class_camera_euler_xyz(vec3 euler, float m[9]);
void class_camera_m3_to_camera(const float m[9], float yaw_hint, vec3 *rotation);
// object x pchObjc
void class_camera_rig_parent(const ClassCameraRig*, float m[9]);
// The game loop's writes to RENDER.camera for the rig (deob:260589-260860): its local position (the
// offsets) and rotation (lookAt with freeMouse, else rotateCam's feel; freeMouse without an offset keeps
// `keep_rotation`, NULL = none). Returns 1 when it looks at the eye.
int class_camera_local(const ClassCamera*, const ClassCameraRig*, const float keep_rotation[9], vec3 *position, float rotation[9]);
// The camera's world position and angles for a chain.
void class_camera_chain_pose(const ClassCameraChain*, Camera*);
// class_camera_local + class_camera_chain_pose for one rig, and the Orthographic projection (ortho, near,
// far; none for the perspective types). Returns 1 when it looks at the eye.
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
// The local player's class camera: updateCamMouse's snapshot (our spawn / class change), the game's
// thirdPerson and mouse_look as of now (the defaults without a player).
ClassCamera class_camera_current(const Client*);
// The pointer lock owner: the class camera wants a free cursor (freeMouse).
int class_camera_wants_free_cursor(const Client*);
// The local model is the third-person one / drawn in the world pass this frame.
int class_camera_draws_own_model(const Client*);
// player_mesh.c's build (generateMeshes, deob:194102): the player's class gives it the third-person model
// (thirdPerson, a class camera offset or a camType other than FPS).
int class_camera_player_third_person(const Player*);
// player_mesh.c's update (updateMsh, deob:193600): thirdPerson, the local controls' camType other than
// FPS, or the player's own class camera offset.
int class_camera_mesh_third_person(const Player*);
// main.c's input stage, before the mouse: a KrunkScript turn of the controls reaches the input look
// (updateMouseInp takes xDr / yDr from them), the spawn's look reaches the controls.
void class_camera_controls_take(Client*, vec2 *look);
// main.c, the look after the mouse: the controls follow it (camRot); the free-mouse aim (the input's
// look) from the cursor, 0 = none.
int class_camera_input_aim(Client*, vec2 look, vec2 *aim);
// main.c, after the tick of a sampled input frame: lockCamP / lckCmYR / yCmLim on the controls, the
// listener (Howler); `look` follows the controls with camRot.
void class_camera_after_input(Client*, vec2 *look, float frame_ms);
// CONTROLS.object.rotation (x the KrunkScript tilt, y the yaw, z the wall lean's roll).
vec3 class_camera_object_rotation(void);
// sound_events.c: the listener at the head, facing the controls' look (Howler.pos / orientation on the
// input frames, deob:260850-260851). 0 = none (no life yet, spectating): the camera's.
int class_camera_listener(const Client*, vec3 *position, float *yaw, float *pitch);
// main.c, before the frame's camera is posed: the perspective projection back (class_camera_apply
// puts the Orthographic one on again while it is wanted).
void class_camera_frame_begin(Client*);
// main.c, after camera_feel_apply: the camera block's writes, then the chain posed. Returns 1 when it
// posed the camera, 0 = the first-person camera stands (nothing here differs from it).
int class_camera_apply(Client*, vec2 look);
// Our (re)spawn: updateCamMouse again, player.camChase = null (deob:190600), CONTROLS.reset's camera
// rotation 0, the aim's plane hit forgotten.
void class_camera_on_spawn(void);
// The look restarts from the player's (the spawn, a forceDir teleport: main.c's resync), which sets
// object.rotation.y and xDr alike (deob:197756-197758, 258177-258181).
void class_camera_look_reset(void);

// KrunkScript GAME.CAMERA on the controls (ks_input.c; deob:210533-210675). Its update runs after the
// camera block in v10, so these write over the block's work of this frame.
void class_camera_ks_detach(Client*);                     // detach: the chain stays where it is
vec3 class_camera_ks_position(const Client*);             // position: object.position
void class_camera_ks_set_position(Client*, int axes, vec3 v); // position.x / y / z (axes 1 / 2 / 4), move()
vec3 class_camera_ks_rotation(void);                      // rotation: object.rotation
void class_camera_ks_set_rotation(Client*, int axis, float v); // rotation.x / y / z (0 / 1 / 2)
void class_camera_ks_rotate(Client*, vec3 euler);         // rotate(): camera.rotation (x, y, z)
void class_camera_ks_look_at(Client*, vec3 target);       // lookAt(): object / pchObjc toward it, camera 0
void class_camera_ks_pose(Client*);                       // ks_client_camera: a detached chain without a life
