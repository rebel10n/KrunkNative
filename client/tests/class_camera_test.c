// Krunker v10.0.3's class camera (client/include/class_camera.h, client/src/class_camera.c):
//   - updateCamMouse: camOffset, camRot, fPLock / forceFreeCam -> freeMouse, aimFov, the camTypes
//   - the pose: the class offset inside the rig (yaw, then pitch) looking parallel to the look with
//     rotateCam's feel, or at the eye with freeMouse (three's lookAt, its straight-down nudge);
//     thirdPerson's (5, 2, 14); the Orthographic camera at the offset in world coordinates
//   - the free-mouse aim: the cursor's ray on the head plane (3rd Person) or the player's vertical
//     plane (Platformer X / Z), a missed plane keeping the last hit
//   - lockCamP's chase (the wrapped difference), lckCmYR and yCmLim on the controls' pitch
//   - a map's config.cls reaches ClassOverrides (lckCmYR's value included)
//   - the chain is three's (object XYZ > pchObjc > camera XYZ) exactly: the feel, the wall lean's roll in
//     the yaw object, a KrunkScript tilt, KrunkNative's camera angles (Rz Ry Rx) for any of them
//   - the controls (class_camera_client.c): KrunkScript's CAMERA.rotation / position / rotate / lookAt
//     while attached show on the frame they are written (the script's update runs after the camera
//     block) and the turn reaches the input look through updateMouseInp only with camRot; a detached
//     camera keeps adding the class offset; the listener sits at the head facing the input look
// GL-free: class_camera.c and class_camera_client.c are included (their few client calls are stubbed
// below). Run from the repository root.
#include "../src/class_camera.c"
#include "../src/class_camera_client.c"
#include <game_config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures, g_checks;

#define CHECK(cond, ...) do { g_checks++; \
    if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_failures++; } \
} while (0)

static int near_f(const double a, const double b, const double eps) { return fabs(a - b) <= eps; }
static int near_v(const vec3 a, const vec3 b, const float eps) { return near_f(a.x, b.x, eps) && near_f(a.y, b.y, eps) && near_f(a.z, b.z, eps); }

// ---- stubs for class_camera_client.c's client calls
static int g_detached, g_wall_roll_stub_set;
static float g_wall_roll_stub;
int ks_client_pointer_free(void) { return 0; }
int ks_client_input_unlocked(void) { return 0; }
int ks_client_camera_detached(void) { return g_detached; }
int ui_mobile_active(void) { return 0; }
int prop_hunt_me(const Client *client) { (void) client; return 0; }
float prop_hunt_camera_distance(void) { return 30.0f; }
int third_person_view(const Client *client) { (void) client; return 0; }
int spectate_active(void) { return 0; }
float camera_feel_wall_roll(void) { return g_wall_roll_stub; }
void camera_feel_set_wall_roll(const float roll) { g_wall_roll_stub = roll; g_wall_roll_stub_set = 1; }

// ---- vectors rotated one axis at a time (right-handed, as three's), independent of the matrices tested
static vec3 rx(const vec3 v, const float a) { return (vec3) {v.x, cosf(a) * v.y - sinf(a) * v.z, sinf(a) * v.y + cosf(a) * v.z}; }
static vec3 ry(const vec3 v, const float a) { return (vec3) {cosf(a) * v.x + sinf(a) * v.z, v.y, -sinf(a) * v.x + cosf(a) * v.z}; }
static vec3 rz(const vec3 v, const float a) { return (vec3) {cosf(a) * v.x - sinf(a) * v.y, sinf(a) * v.x + cosf(a) * v.y, v.z}; }

// KrunkNative's camera (camera.c: world = Rz(z) Ry(y) Rx(x)): its forward (-Z) and up (+Y)
static vec3 cam_dir(const Camera *c, const vec3 v) { return rz(ry(rx(v, c->rotation.x), c->rotation.y), c->rotation.z); }
static vec3 forward(const Camera *c) { return cam_dir(c, (vec3) {0.0f, 0.0f, -1.0f}); }
static vec3 up(const Camera *c) { return cam_dir(c, (vec3) {0.0f, 1.0f, 0.0f}); }
// three's chain: object Rx(tilt) Ry(yaw) Rz(roll), pchObjc Rx(pitch), camera Rx(cx) Ry(cy) Rz(cz)
static vec3 chain_dir(const vec3 v, const float tilt, const float yaw, const float roll, const float pitch, const vec3 cr) {
    return rx(ry(rz(rx(rx(ry(rz(v, cr.z), cr.y), cr.x), pitch), roll), yaw), tilt);
}

static ClassOverrides cls(const int type, const float ox, const float oy, const float oz, const int rot) {
    ClassOverrides o = g_default_class_overrides;
    o.set = 1;
    o.cam_type = type;
    o.cam_offset[0] = ox, o.cam_offset[1] = oy, o.cam_offset[2] = oz;
    o.cam_rotation = (unsigned char) rot;
    return o;
}

static Camera camera0(void) {
    Camera c = {0};
    c.near = 0.1f, c.far = 10000.0f, c.fov = 70.0f * (float) M_PI / 180.0f, c.zoom = 1.0f;
    return c;
}

static void test_make(void) {
    const ClassCamera d = class_camera_make(NULL, 0, 0);
    CHECK(d.type == CLASS_CAMERA_FPS && d.rotation && !d.has_offset && !d.free_mouse && d.aim_fov && d.ortho_f == 50.0f,
          "defaults: FPS, camRot, no offset, pointer lock, aimFov, orthoF 50");
    CHECK(!class_camera_active(&d) && !class_camera_third_person(&d), "the default class camera is the plain first person one");

    ClassOverrides o = cls(CLASS_CAMERA_THIRD, 0, 120, 0, 0);
    ClassCamera c = class_camera_make(&o, 0, 0);
    CHECK(c.has_offset && !c.rotation && c.free_mouse && !c.aim_fov && class_camera_third_person(&c),
          "an offset without camRot: freeMouse (UsePointerLock false), no aimFov, the third-person model");
    o.fp_lock = 1;
    c = class_camera_make(&o, 0, 0);
    CHECK(!c.free_mouse, "fPLock keeps the pointer lock");
    o = cls(CLASS_CAMERA_FPS, 0, 0, 0, 0);
    c = class_camera_make(&o, 0, 0);
    CHECK(!c.free_mouse && class_camera_active(&c) && !class_camera_third_person(&c), "no camRot, no offset: locked, the mouse just does not turn");
    c = class_camera_make(NULL, 0, 1);
    CHECK(c.free_mouse, "forceFreeCam (INPUTS.freeMouse) frees the cursor whatever the class");
    o = cls(CLASS_CAMERA_SIDE_X, 0, 0, 0, 1);
    o.shoot_to_cam = 1;
    c = class_camera_make(&o, 0, 0);
    CHECK(c.aim_fov && class_camera_third_person(&c), "shtToCam keeps aimFov; a Platformer camera always has the third-person model");
    c = class_camera_make(NULL, 1, 0);
    CHECK(class_camera_third_person(&c) && class_camera_active(&c) && !c.free_mouse, "thirdPerson: third person, still locked");
}

static void test_pose(void) {
    const vec3 eye = {10.0f, 20.0f, 30.0f};
    // a mouse-rotated offset: the offset turns with the look, the camera looks parallel to it (+ the feel)
    ClassOverrides o = cls(CLASS_CAMERA_THIRD, 0, 25, 45, 1);
    ClassCamera c = class_camera_make(&o, 0, 0);
    ClassCameraRig r = {.eye = eye, .pitch = 0.0f, .yaw = (float) M_PI / 2.0f, .feel_pitch = 0.01f, .feel_yaw = -0.02f};
    Camera cam = camera0();
    CHECK(!class_camera_pose(&c, &r, &cam), "camRot: rotateCam undoes the lookAt");
    // yaw PI/2 faces -X: back (+Z local) is +X
    CHECK(near_v(cam.position, (vec3) {55.0f, 45.0f, 30.0f}, 1e-3f), "offset (0, 25, 45) at yaw PI/2: %.3f %.3f %.3f", cam.position.x, cam.position.y, cam.position.z);
    CHECK(near_v(forward(&cam), chain_dir((vec3) {0, 0, -1}, 0, (float) M_PI / 2.0f, 0, 0, (vec3) {0.01f, -0.02f, 0}), 1e-5f) &&
          near_v(up(&cam), chain_dir((vec3) {0, 1, 0}, 0, (float) M_PI / 2.0f, 0, 0, (vec3) {0.01f, -0.02f, 0}), 1e-5f),
          "the look, then rotateCam's feel inside it (three's chain)");
    r.pitch = -0.5f;
    class_camera_pose(&c, &r, &cam);
    // pitch swings the offset in the yaw's vertical plane: still at z = eye.z
    CHECK(near_f(cam.position.z, 30.0, 1e-3) && near_f(sqrtf((cam.position.x - 10) * (cam.position.x - 10) + (cam.position.y - 20) * (cam.position.y - 20)), sqrtf(25 * 25 + 45 * 45), 1e-3),
          "a pitch keeps the offset's length in the yaw's vertical plane");

    // freeMouse, straight above: three's lookAt nudge -> pitch -PI/2, yaw 0 (screen up = world -Z)
    o = cls(CLASS_CAMERA_THIRD, 0, 120, 0, 0);
    c = class_camera_make(&o, 0, 0);
    r = (ClassCameraRig) {.eye = eye, .feel_pitch = 0.3f};
    CHECK(class_camera_pose(&c, &r, &cam), "freeMouse with an offset looks at the eye");
    CHECK(near_v(cam.position, (vec3) {10.0f, 140.0f, 30.0f}, 1e-4f) && near_f(cam.rotation.x, -M_PI / 2.0, 2e-4) && near_f(cam.rotation.y, 0.0, 1e-5),
          "straight down: pos %.2f %.2f %.2f rot %.5f %.5f", cam.position.x, cam.position.y, cam.position.z, cam.rotation.x, cam.rotation.y);

    // freeMouse lookAt from an oblique offset: the forward vector points at the eye
    o = cls(CLASS_CAMERA_SIDE_X, 30, 10, 80, 0);
    c = class_camera_make(&o, 0, 0);
    r = (ClassCameraRig) {.eye = eye, .yaw = 0.7f};
    class_camera_pose(&c, &r, &cam);
    vec3 to = {eye.x - cam.position.x, eye.y - cam.position.y, eye.z - cam.position.z};
    const float len = sqrtf(to.x * to.x + to.y * to.y + to.z * to.z);
    to = (vec3) {to.x / len, to.y / len, to.z / len};
    CHECK(near_v(forward(&cam), to, 1e-4f) && near_f(cam.rotation.z, 0.0, 1e-5), "lookAt: the camera faces the eye, no roll");

    // thirdPerson: (5, 2, 14) in the look frame, parallel
    c = class_camera_make(NULL, 1, 0);
    r = (ClassCameraRig) {.eye = eye, .local = {CLASS_CAMERA_TP_X, CLASS_CAMERA_TP_Y, CLASS_CAMERA_TP_Z}, .local_offset = 1};
    CHECK(!class_camera_pose(&c, &r, &cam) && near_v(cam.position, (vec3) {15.0f, 22.0f, 44.0f}, 1e-4f) && near_f(cam.rotation.x, 0.0, 1e-6) && near_f(cam.rotation.y, 0.0, 1e-6),
          "thirdPerson at yaw 0: right 5, up 2, back 14, looking along the look");

    // Orthographic: not in the rig, the offset is a world position; lookAt with freeMouse
    o = cls(CLASS_CAMERA_ORTHO, 150, 200, 150, 0);
    o.ortho_f = 120.0f;
    c = class_camera_make(&o, 0, 0);
    r = (ClassCameraRig) {.eye = eye, .yaw = 1.0f, .pitch = 0.4f};
    CHECK(class_camera_pose(&c, &r, &cam) && near_v(cam.position, (vec3) {150.0f, 200.0f, 150.0f}, 1e-4f), "Orthographic: the offset in world coordinates");
    CHECK(cam.ortho == 60.0f && cam.near == CLASS_CAMERA_ORTHO_NEAR && cam.far == CLASS_CAMERA_ORTHO_FAR, "orthoF 120: half height 60, near 0.1, far 2000");
    o.cam_rotation = 1;
    c = class_camera_make(&o, 0, 0);
    r.feel_pitch = 0.05f;
    CHECK(!class_camera_pose(&c, &r, &cam) && near_f(cam.rotation.x, 0.05, 1e-6) && near_f(cam.rotation.y, 0.0, 1e-6),
          "Orthographic with camRot: rotateCam's feel in world space (the controls never turn it)");
    o = cls(CLASS_CAMERA_FPS, 0, 0, 0, 1);
    c = class_camera_make(&o, 0, 0);
    class_camera_pose(&c, &(ClassCameraRig) {.eye = eye}, &cam);
    CHECK(cam.ortho == 0.0f, "a perspective type turns the orthographic projection off");
}

static void test_free_aim(void) {
    const vec3 player = {0.0f, 0.0f, 0.0f};
    const float height = 11.0f, aspect = 16.0f / 9.0f;
    vec3 hit = {0};
    float yaw = 0, pitch = 0;
    // 3rd Person straight above: the cursor right of the centre faces +X (yaw -PI/2), above it -Z (yaw 0)
    ClassOverrides o = cls(CLASS_CAMERA_THIRD, 0, 120, 0, 0);
    ClassCamera c = class_camera_make(&o, 0, 0);
    Camera cam = camera0();
    class_camera_pose(&c, &(ClassCameraRig) {.eye = {0.0f, height - 1.5f, 0.0f}}, &cam);
    CHECK(class_camera_free_aim(&c, &cam, aspect, 0.5f, 0.0f, player, height, &hit, &yaw, &pitch) && near_f(yaw, -M_PI / 2.0, 2e-3) && pitch == 0.0f,
          "3rd Person: cursor right -> yaw %.4f (-PI/2), pitch 0", yaw);
    CHECK(near_f(hit.z, 0.0, 0.05) && hit.x > 0.0f, "the hit lies right of the player on the head plane");
    class_camera_free_aim(&c, &cam, aspect, 0.0f, 0.5f, player, height, &hit, &yaw, &pitch);
    CHECK(near_f(yaw, 0.0, 2e-3) || near_f(fabs(yaw), 2.0 * M_PI, 2e-3), "cursor up (world -Z) -> yaw 0: %.4f", yaw);
    class_camera_free_aim(&c, &cam, aspect, -0.3f, -0.3f, player, height, &hit, &yaw, &pitch);
    const vec3 f = {-sinf(yaw), 0.0f, -cosf(yaw)};
    CHECK(f.x < 0.0f && f.z > 0.0f && near_f(f.x * hit.z - f.z * hit.x, 0.0, 1e-3), "the look runs through the hit (lower left: -X, +Z)");
    // a camera under the head plane: the ray misses (t < 0), the last hit stays
    const vec3 kept = hit;
    Camera low = cam;
    low.position.y = 5.0f;
    class_camera_free_aim(&c, &low, aspect, 0.5f, 0.0f, player, height, &hit, &yaw, &pitch);
    CHECK(near_v(hit, kept, 0.0f), "a missed plane keeps the last hit (intersectPlane null)");

    // Platformer X: camera at +Z looking -Z; the cursor right / up faces +X and aims up; left mirrors the pitch
    o = cls(CLASS_CAMERA_SIDE_X, 0, 0, 90, 0);
    c = class_camera_make(&o, 0, 0);
    class_camera_pose(&c, &(ClassCameraRig) {.eye = {0.0f, height - 1.5f, 0.0f}}, &cam);
    CHECK(class_camera_free_aim(&c, &cam, aspect, 0.5f, 0.5f, player, height, &hit, &yaw, &pitch) && near_f(yaw, -M_PI / 2.0, 1e-6) && pitch > 0.0f,
          "Platformer X: cursor up right -> yaw -PI/2, pitch %.3f > 0", pitch);
    const float right_pitch = pitch;
    class_camera_free_aim(&c, &cam, aspect, -0.5f, 0.5f, player, height, &hit, &yaw, &pitch);
    CHECK(near_f(yaw, M_PI / 2.0, 1e-6) && near_f(pitch, right_pitch, 0.02), "cursor up left -> yaw PI/2, the same pitch mirrored: %.3f", pitch);
    class_camera_free_aim(&c, &cam, aspect, 0.5f, -0.5f, player, height, &hit, &yaw, &pitch);
    CHECK(pitch < 0.0f && pitch > -(float) M_PI / 2.0f, "cursor down -> a downward pitch in (-PI/2, 0)");

    // Platformer Z: camera at -X looking +X (rig yaw -PI/2); the cursor right is +Z: yaw PI
    o = cls(CLASS_CAMERA_SIDE_Z, 0, 0, 90, 0);
    c = class_camera_make(&o, 0, 0);
    class_camera_pose(&c, &(ClassCameraRig) {.eye = {0.0f, height - 1.5f, 0.0f}, .yaw = -(float) M_PI / 2.0f}, &cam);
    CHECK(near_f(cam.position.x, -90.0, 1e-3), "Platformer Z camera at -X: %.2f", cam.position.x);
    CHECK(class_camera_free_aim(&c, &cam, aspect, 0.5f, 0.0f, player, height, &hit, &yaw, &pitch) && near_f(yaw, M_PI, 1e-6),
          "Platformer Z: cursor right (+Z) -> yaw PI (faces +Z)");
    class_camera_free_aim(&c, &cam, aspect, -0.5f, 0.0f, player, height, &hit, &yaw, &pitch);
    CHECK(yaw == 0.0f, "cursor left (-Z) -> yaw 0");

    // FPS / Orthographic: no aim (the controls' look stays)
    c = class_camera_make(NULL, 0, 1);
    CHECK(!class_camera_free_aim(&c, &cam, aspect, 0.5f, 0.0f, player, height, &hit, &yaw, &pitch), "FPS has no cursor aim");
    o = cls(CLASS_CAMERA_ORTHO, 0, 100, 0, 0);
    c = class_camera_make(&o, 0, 0);
    CHECK(!class_camera_free_aim(&c, &cam, aspect, 0.5f, 0.0f, player, height, &hit, &yaw, &pitch), "Orthographic has no cursor aim");
}

static void test_locks(void) {
    ClassOverrides o = g_default_class_overrides;
    o.lock_cam_pitch = 1.0f;
    ClassCamera c = class_camera_make(&o, 0, 0);
    float chase = NAN;
    float yaw = class_camera_face_player(&c, &chase, 1.0f, 16.0f);
    CHECK(yaw == 1.0f && chase == 1.0f, "camChase null starts at the body's yaw");
    yaw = class_camera_face_player(&c, &chase, 1.5f, 50.0f);
    CHECK(near_f(yaw, 1.25, 1e-6), "half way at lockCamP 1 x 50 ms / 100: %.4f", yaw);
    // across the +-PI seam: the shortest way (body 3.1, chase -3.1: 0.083 the other way round)
    chase = -3.1f;
    yaw = class_camera_face_player(&c, &chase, 3.1f, 100.0f);
    CHECK(near_f(yaw, -3.1 - (2.0 * M_PI - 6.2), 1e-4), "wrapped: %.4f", yaw);
    o.lock_cam_pitch = 0.0f;
    c = class_camera_make(&o, 0, 0);
    chase = 0.3f;
    CHECK(class_camera_face_player(&c, &chase, 2.0f, 16.0f) == 0.3f, "no lockCamP: unchanged");

    o = g_default_class_overrides;
    o.lock_y_rot = 0.5f;
    c = class_camera_make(&o, 0, 0);
    CHECK(near_f(class_camera_lock_pitch(&c, 0.7f), -M_PI / 4.0, 1e-6), "lckCmYR 0.5: pitch -PI/4");
    o.lock_y_rot = -1.0f;
    c = class_camera_make(&o, 0, 0);
    CHECK(near_f(class_camera_lock_pitch(&c, 0.0f), M_PI / 2.0, 1e-6), "lckCmYR -1: straight up");
    o = g_default_class_overrides;
    o.y_cam_limit = 0.25f;
    c = class_camera_make(&o, 0, 0);
    CHECK(near_f(class_camera_lock_pitch(&c, 0.3f), -M_PI / 4.0, 1e-6) && class_camera_lock_pitch(&c, -1.0f) == -1.0f,
          "yCmLim 0.25: at most -PI/4, below it untouched");
}

// a map's config.cls reaches the class's overrides, lckCmYR's value with it
static void test_config(void) {
    const char *json = "{\"cls\":{\"0\":{\"camType\":2,\"camOffZ\":80,\"camRot\":false,\"orthoF\":75,\"lockCamP\":0.4,\"lckCmYR\":-0.5,"
                       "\"yCmLim\":0.3,\"fPLock\":true}}}";
    cJSON *cfg = cJSON_Parse(json);
    FILE *file = fopen("assets/maps/custom/Test.json", "rb");
    char *text = NULL;
    long size = 0;
    if (file && !fseek(file, 0, SEEK_END) && (size = ftell(file)) > 0 && !fseek(file, 0, SEEK_SET) && (text = calloc(1, (size_t) size + 1)))
        if (fread(text, 1, (size_t) size, file) != (size_t) size) size = 0;
    if (file) fclose(file);
    cJSON *map = text && size > 0 ? cJSON_Parse(text) : NULL;
    free(text);
    CHECK(map != NULL, "assets/maps/custom/Test.json (run from the repository root)");
    if (!map) return;
    static const cJSON *maps[1];
    maps[0] = map;
    Game game;
    memset(&game, 0, sizeof(game));
    game_configure(&game, NULL, maps, 1, NULL, 0, NULL, 0, NULL, 0);
    CHECK(game_config_apply(&game, cfg, 1, 0), "game_config_apply");
    const ClassOverrides *o = class_overrides(&game.classes[0]);
    CHECK(o->cam_type == 2 && o->cam_offset[2] == 80.0f && !o->cam_rotation && o->ortho_f == 75.0f && near_f(o->lock_cam_pitch, 0.4, 1e-6) &&
          near_f(o->lock_y_rot, -0.5, 1e-6) && near_f(o->y_cam_limit, 0.3, 1e-6) && o->fp_lock,
          "cls[0]: camType %d camOffZ %g camRot %d orthoF %g lockCamP %g lckCmYR %g yCmLim %g fPLock %d", o->cam_type, o->cam_offset[2],
          o->cam_rotation, o->ortho_f, o->lock_cam_pitch, o->lock_y_rot, o->y_cam_limit, o->fp_lock);
    CHECK(game.classes[0].movement.lock_pitch, "lckCmYR still zeroes the input pitch (ClassMovement.lock_pitch)");
    double v = 0.0;
    CHECK(game_config_value(GAME_CONFIG_CLASS, &game.classes[0], "lckCmYR", &v, NULL) && near_f(v, -0.5, 1e-6), "lckCmYR reads back as -0.5: %g", v);
    const ClassCamera c = class_camera_make(o, 0, 0);
    CHECK(c.type == CLASS_CAMERA_SIDE_Z - 1 && !c.free_mouse && c.has_offset, "Platformer X, fPLock: locked");
    game_config_free(&game);
    if (game.map) map_fini(game.map);
    if (game.mode) mode_fini(game.mode);
    free(game.weapons);
    free(game.classes);
    free(game.modes);
    cJSON_Delete(cfg);
    cJSON_Delete(map);
}

// three's chain composed exactly, and KrunkNative's angles for it
static void test_chain(void) {
    // a rotation without roll comes back as exactly its pitch / yaw, the yaw next to the hint
    const float yaws[] = {0.0f, 1.2f, 2.5f, -2.5f, 3.1f, -1.9f};
    for (size_t i = 0; i < sizeof(yaws) / sizeof(yaws[0]); i++) {
        float m[9], p[9];
        class_camera_euler_xyz((vec3) {0.0f, yaws[i], 0.0f}, m);
        class_camera_euler_xyz((vec3) {-0.7f, 0.0f, 0.0f}, p);
        class_camera_m3_mul(m, p, m);
        vec3 rot;
        class_camera_m3_to_camera(m, yaws[i], &rot);
        CHECK(near_f(rot.x, -0.7, 1e-5) && near_f(rot.y, yaws[i], 1e-5) && near_f(rot.z, 0.0, 1e-5),
              "yaw %.2f pitch -0.7: %.5f %.5f %.5f", yaws[i], rot.x, rot.y, rot.z);
    }
    // any chain: KrunkNative's camera shows the same forward and up as three's
    unsigned seed = 7;
    for (int i = 0; i < 200; i++) {
        float v[7];
        for (int k = 0; k < 7; k++) {
            seed = seed * 1103515245u + 12345u;
            v[k] = ((float) ((seed >> 8) & 0xffff) / 65535.0f * 2.0f - 1.0f) * (k == 1 ? 3.1f : 1.4f);
        }
        const ClassCameraRig r = {.pitch = v[0], .yaw = v[1], .tilt = v[2] * 0.3f, .roll = v[3] * 0.3f};
        ClassCameraChain k = {.rig = r};
        class_camera_euler_xyz((vec3) {v[4] * 0.2f, v[5] * 0.2f, v[6] * 0.2f}, k.camera_rotation);
        Camera cam = camera0();
        class_camera_chain_pose(&k, &cam);
        const vec3 cr = {v[4] * 0.2f, v[5] * 0.2f, v[6] * 0.2f};
        const vec3 f = chain_dir((vec3) {0, 0, -1}, r.tilt, r.yaw, r.roll, r.pitch, cr), u = chain_dir((vec3) {0, 1, 0}, r.tilt, r.yaw, r.roll, r.pitch, cr);
        if (!near_v(forward(&cam), f, 2e-5f) || !near_v(up(&cam), u, 2e-5f)) {
            CHECK(0, "chain %d: forward / up differ from three's", i);
            break;
        }
    }
    CHECK(1, "200 random chains match three's forward and up");
    // the wall lean's roll is inside the yaw: looking along -X it rolls the view, it does not pitch it
    ClassCameraChain k = {.rig = {.yaw = (float) M_PI / 2.0f, .roll = 0.15f}};
    class_camera_m3_identity(k.camera_rotation);
    Camera cam = camera0();
    class_camera_chain_pose(&k, &cam);
    CHECK(near_v(forward(&cam), (vec3) {-1.0f, 0.0f, 0.0f}, 1e-5f) && near_v(up(&cam), (vec3) {0.0f, cosf(0.15f), sinf(0.15f)}, 1e-5f),
          "roll 0.15 at yaw PI/2: forward stays -X, up tilts toward +Z");
}

// a local player of `cls` (NULL = the default class) at the origin, facing yaw `yaw`
static Client *client_with(const ClassOverrides *cls, const float yaw) {
    Client *client = calloc(1, sizeof(Client));
    Player *me = calloc(1, sizeof(Player));
    client->game.class_count = 1;
    client->game.classes = calloc(1, sizeof(ClassConfig));
    if (cls) client->game.classes[0].ext = *cls;
    me->game = &client->game;
    me->active = 1;
    me->height = 11.0f;
    me->direction = (vec2) {0.0f, yaw};
    client->me = me;
    client->camera = camera0();
    class_camera_on_spawn();
    return client;
}

static void client_free(Client *client) {
    free(client->game.classes);
    free(client->me);
    free(client);
}

// one frame of main.c's camera stages; `ks` runs where the client script's update does; returns
// class_camera_apply's answer (1: the chain posed the camera)
static int frame(Client *client, vec2 *look, const float mouse_yaw, const int sampled, void (*ks)(Client*)) {
    class_camera_controls_take(client, look);
    const ClassCamera c = class_camera_current(client);
    if (c.mouse_look) look->y += mouse_yaw;
    vec2 aim;
    class_camera_input_aim(client, *look, &aim);
    if (sampled) {
        if (!c.free_mouse) client->me->direction = *look; // procInputs: Default with trnSp 1 takes the look
        class_camera_after_input(client, look, 16.0f);
    }
    if (ks) ks(client);
    class_camera_frame_begin(client);
    // camera_feel_apply: the eye and the look (no feel here)
    client->camera.position = (vec3) {0.0f, client->me->height - game_constants.camera_height, 0.0f};
    client->camera.rotation = (vec3) {client->me->direction.x, client->me->direction.y, 0.0f};
    const int posed = class_camera_apply(client, *look);
    if (ks) class_camera_ks_pose(client);
    return posed;
}

static void ks_tilt(Client *client) { class_camera_ks_set_rotation(client, 0, 0.3f); }
static void ks_yaw(Client *client) { class_camera_ks_set_rotation(client, 1, 1.0f); }
static void ks_rotate(Client *client) { class_camera_ks_rotate(client, (vec3) {0.2f, 0.4f, 0.0f}); }
static void ks_raise(Client *client) {
    const vec3 p = class_camera_ks_position(client);
    CHECK(near_f(p.y, client->me->height - game_constants.camera_height, 1e-5), "position.y reads this frame's eye: %.3f", p.y);
    class_camera_ks_set_position(client, 2, (vec3) {0.0f, 50.0f, 0.0f});
    CHECK(near_f(class_camera_ks_position(client).y, 50.0, 1e-6), "position.y reads back the script's 50");
}
static void ks_look(Client *client) { class_camera_ks_look_at(client, (vec3) {100.0f, 9.5f, 0.0f}); }
static void ks_detach(Client *client) { class_camera_ks_detach(client); g_detached = 1; }

static void test_controls(void) {
    const float eye_y = 11.0f - game_constants.camera_height;
    // FPS, camRot: a plain frame leaves the first-person camera alone
    Client *client = client_with(NULL, 0.5f);
    vec2 look = {0.0f, 0.5f};
    CHECK(!frame(client, &look, 0.0f, 1, NULL), "a plain FPS frame: the first-person camera stands");

    // CAMERA.rotation.x: object.rotation.x tilts the whole rig from this frame on
    CHECK(frame(client, &look, 0.0f, 1, ks_tilt), "a tilt: the chain poses the camera");
    CHECK(near_v(forward(&client->camera), chain_dir((vec3) {0, 0, -1}, 0.3f, 0.5f, 0, 0, (vec3) {0}), 1e-5f), "the tilted rig: Rx(0.3) Ry(0.5)");
    CHECK(near_f(class_camera_ks_rotation().x, 0.3, 1e-6), "rotation.x reads 0.3");
    CHECK(frame(client, &look, 0.0f, 1, NULL) && near_v(forward(&client->camera), chain_dir((vec3) {0, 0, -1}, 0.3f, 0.5f, 0, 0, (vec3) {0}), 1e-5f),
          "the tilt stays (object.rotation.x is never reset)");
    class_camera_ks_set_rotation(client, 0, 0.0f);

    // CAMERA.rotation.y with camRot: the camera turns this frame, the input look follows at the next updateMouseInp
    frame(client, &look, 0.0f, 1, ks_yaw);
    CHECK(near_v(forward(&client->camera), chain_dir((vec3) {0, 0, -1}, 0, 1.0f, 0, 0, (vec3) {0}), 1e-5f) && near_f(look.y, 0.5, 1e-6),
          "rotation.y 1.0: the camera at once, the input look (xDr) still 0.5");
    frame(client, &look, 0.1f, 1, NULL);
    CHECK(near_f(look.y, 1.1, 1e-5), "next frame xDr = object.rotation.y + the mouse: %.4f", look.y);
    CHECK(near_f(class_camera_ks_rotation().y, 1.1, 1e-5), "rotation.y reads the controls' yaw: %.4f", class_camera_ks_rotation().y);
    // a full turn of mouse keeps object.rotation.y unwrapped, as three's
    for (int i = 0; i < 10; i++) frame(client, &look, (float) M_PI * 0.2f, 1, NULL);
    CHECK(near_f(class_camera_ks_rotation().y, 1.1 + 2.0 * M_PI, 1e-4), "after a full turn rotation.y is %.4f (unwrapped)", class_camera_ks_rotation().y);

    // CAMERA.rotate(a, b, c): camera.rotation for this frame only (rotateCam writes it again next frame)
    look = (vec2) {0.0f, 0.5f};
    class_camera_look_reset();
    frame(client, &look, 0.0f, 1, NULL);
    CHECK(frame(client, &look, 0.0f, 1, ks_rotate), "rotate(): the chain poses");
    CHECK(near_v(forward(&client->camera), chain_dir((vec3) {0, 0, -1}, 0, 0.5f, 0, 0, (vec3) {0.2f, 0.4f, 0}), 1e-5f) &&
          near_v(up(&client->camera), chain_dir((vec3) {0, 1, 0}, 0, 0.5f, 0, 0, (vec3) {0.2f, 0.4f, 0}), 1e-5f),
          "rotate(): camera.rotation (x 0.2, y 0.4) inside the rig");
    CHECK(!frame(client, &look, 0.0f, 1, NULL) && near_f(client->camera.rotation.y, 0.5, 1e-6), "the next frame's rotateCam puts the feel back");

    // CAMERA.position.y: object.position for this frame (the next moveCam puts the eye back)
    frame(client, &look, 0.0f, 1, ks_raise);
    CHECK(near_f(client->camera.position.y, 50.0, 1e-5), "position.y = 50 shows on this frame: %.3f", client->camera.position.y);
    frame(client, &look, 0.0f, 1, NULL);
    CHECK(near_f(client->camera.position.y, eye_y, 1e-5), "and the eye again on the next");
    client_free(client);

    // camRot off: rotation.y turns the camera, never the input look
    ClassOverrides o = g_default_class_overrides;
    o.set = 1;
    o.cam_rotation = 0;
    client = client_with(&o, 0.5f);
    look = (vec2) {0.0f, 0.5f};
    frame(client, &look, 0.0f, 1, ks_yaw);
    frame(client, &look, 0.3f, 1, NULL);
    CHECK(near_f(look.y, 0.5, 1e-6) && near_v(forward(&client->camera), chain_dir((vec3) {0, 0, -1}, 0, 1.0f, 0, 0, (vec3) {0}), 1e-5f),
          "no camRot: the camera keeps yaw 1.0, the input look 0.5 (no mouse either)");
    client_free(client);

    // CAMERA.lookAt with a 3rd Person offset: this frame the camera sits at the eye (camera.position 0)
    // looking at the point; the next frame's camera block puts the offset back, the controls keep the yaw
    o = g_default_class_overrides;
    o.set = 1;
    o.cam_type = CLASS_CAMERA_THIRD;
    o.cam_offset[1] = 25.0f;
    o.cam_offset[2] = 45.0f;
    client = client_with(&o, 0.0f);
    look = (vec2) {0.0f, 0.0f};
    frame(client, &look, 0.0f, 1, ks_look);
    CHECK(near_v(client->camera.position, (vec3) {0.0f, eye_y, 0.0f}, 1e-4f) && near_v(forward(&client->camera), (vec3) {1.0f, 0.0f, 0.0f}, 1e-4f),
          "lookAt(100, eye, 0): at the eye facing +X");
    frame(client, &look, 0.0f, 1, NULL);
    const vec3 back = chain_dir((vec3) {0.0f, 25.0f, 45.0f}, 0, -(float) M_PI / 2.0f, 0, 0, (vec3) {0});
    CHECK(near_f(look.y, -M_PI / 2.0, 1e-5) && near_v(client->camera.position, (vec3) {back.x, eye_y + back.y, back.z}, 1e-3f),
          "next frame: xDr took the lookAt's yaw (-PI/2), the offset behind it again");

    // detached: the camera block stops; `+= camOffX / Y / Z` keeps running
    frame(client, &look, 0.0f, 1, ks_detach);
    const vec3 before = client->camera.position;
    frame(client, &look, 0.5f, 1, NULL);
    frame(client, &look, 0.5f, 1, NULL);
    const vec3 d = chain_dir((vec3) {0.0f, 50.0f, 90.0f}, 0, -(float) M_PI / 2.0f, 0, 0, (vec3) {0});
    CHECK(near_v(client->camera.position, (vec3) {before.x + d.x, before.y + d.y, before.z + d.z}, 1e-3f), "two detached frames add the offset twice");
    CHECK(near_f(look.y, -M_PI / 2.0, 1e-5), "detached: the mouse does not turn the controls");
    g_detached = 0;
    client_free(client);

    // Howler: the head, the input look
    client = client_with(NULL, 0.7f);
    look = (vec2) {-0.2f, 0.7f};
    frame(client, &look, 0.0f, 1, NULL);
    vec3 ear;
    float ear_yaw, ear_pitch;
    CHECK(class_camera_listener(client, &ear, &ear_yaw, &ear_pitch) && near_v(ear, (vec3) {0.0f, eye_y, 0.0f}, 1e-5f) && near_f(ear_yaw, 0.7, 1e-6) &&
          near_f(ear_pitch, atan(-0.2), 1e-6), "listener at the eye, yaw xDr, the vector's yDr as atan(yDr)");
    client_free(client);
}

int main(void) {
    test_make();
    test_pose();
    test_free_aim();
    test_locks();
    test_config();
    test_chain();
    test_controls();
    printf("class_camera_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
