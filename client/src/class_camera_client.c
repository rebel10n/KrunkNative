// Krunker v10.0.3's class camera, the client half (client/include/class_camera.h): updateCamMouse's
// snapshot of the local player's class, v10's CONTROLS chain (object, pchObjc, RENDER.camera) kept between
// frames, the free-mouse aim from the cursor, KrunkScript's GAME.CAMERA writes, the Howler listener and
// the Orthographic camera's near / far.
//
// Frame order (v10 gameLoop, deob-10.0.3.js:260378-260880, then game.update -> mainScript.update at
// :222267-222268 before RENDER.render at :222270): updateMouseInp; the camera block (camera.position, the
// lookAt, the free-mouse aim, procInputs, rotateCam, moveCam, lockCamP / lckCmYR / yCmLim, Howler, the
// wall lean); the KrunkScript update; render. KrunkNative runs its script update before it poses the
// camera, so what the script writes to the camera block's fields this frame is noted and the block's own
// write skipped (the script's comes after it in v10).
#include <class_camera.h>
#include <camera_feel.h> // object.rotation.z: the wall lean's roll
#include <game_config.h>
#include <ks_client.h>
#include <prop_hunt_client.h>
#include <third_person.h>
#include <spectate.h>
#include <ui_mobile.h>
#include <math.h>

static struct {
    // updateCamMouse's result (deob:257863-257881): our spawn / class change
    ClassCamera cam;
    int cam_valid;
    const Player *cam_player;
    int cam_class;

    float chase;            // player.camChase (NAN: null, deob:190600)
    vec3 hit;               // the free-mouse ray's last plane hit (iììiiií: intersectPlane keeps it on a miss)
    int aimed;              // this frame's free-mouse aim set yDr
    float aim_pitch;

    // CONTROLS: pchObjc.rotation.x / object.rotation.y (the yaw kept unwrapped like three's), object.rotation.x;
    // object.rotation.z is camera_feel's wall roll
    int rig_set;
    vec2 rig;
    int rig_dirty;          // KrunkScript turned them: the next updateMouseInp takes xDr / yDr from them
    vec2 look_before;       // the input look before this frame's mouse
    float tilt;
    vec3 object_pos;        // CONTROLS.object.position (moveCam; KrunkScript's while detached)
    int object_set;
    vec3 cam_pos;           // RENDER.camera.position / rotation (local to pchObjc; world for the Orthographic one)
    float cam_rot[9];
    int cam_set;

    // this frame's camera block
    int block_detached;     // game.detachedCam as the block saw it (before the script's update)
    ClassCameraRig look_rig; // the rig the lookAt saw: the eye before procInputs, before lockCamP
    int look_rig_set;
    // which of the block's writes this frame's KrunkScript update overrides (it runs after the block in v10)
    int ks_pos_mask;        // object.position x / y / z (1 / 2 / 4)
    int ks_cam_rot;         // camera.rotation (CAMERA.rotate, CAMERA.lookAt)
    int ks_cam_pos;         // camera.position (CAMERA.lookAt resets it)
    int posed;              // the chain posed the camera this frame
    int was_active;         // the last frame had an active local player

    // Howler.pos / orientation (deob:260850-260851)
    int ear_set;
    vec3 ear;
    float ear_yaw, ear_pitch;

    double cursor_x, cursor_y; // CONTROLS.mouseX / mouseY (window coordinates)
    int cursor_set;
    int ortho;              // the camera wears the Orthographic near / far: put the perspective ones back
    float near, far;
} g_cc = {.chase = NAN};

static const ClassOverrides *class_of(const Game *game, const Player *player) {
    if (!game || !player || !game->classes || player->class_index < 0 || (size_t) player->class_index >= game->class_count) return NULL;
    return class_overrides(&game->classes[player->class_index]);
}

static int me_active(const Client *client) {
    return client && client->me && client->me->active;
}

static vec3 eye_of(const Player *me) {
    return (vec3) {me->position.x, me->position.y + me->height - game_constants.camera_height, me->position.z}; // moveCam
}

// updateCamMouse runs on our spawn and class change with our class (deob:190846, 256363); its camOffset,
// camRot, freeMouse (forceFreeCam and the phone controls read then), camType, orthoF and aimFov hold until
// the next one, through death too. The game's thirdPerson is read every frame (the game loop's
// config.thirdPerson); mouse_look adds !detachedCam (updateMouseInp, deob:235469).
ClassCamera class_camera_current(const Client *client) {
    const Player *me = client ? client->me : NULL;
    if (!g_cc.cam_valid || me != g_cc.cam_player || (me && me->class_index != g_cc.cam_class)) {
        const int force_free = ks_client_pointer_free() && !ks_client_input_unlocked(); // INPUTS.freeMouse, not unlockMouse
        g_cc.cam = class_camera_make(class_of(client ? &client->game : NULL, me), 0, force_free);
        if (ui_mobile_active()) g_cc.cam.free_mouse = 0; // isMobileControls: the phone controls aim
        g_cc.cam_valid = 1;
        g_cc.cam_player = me;
        g_cc.cam_class = me ? me->class_index : -1;
    }
    ClassCamera c = g_cc.cam;
    c.third_person = client && client->game.config.third_person;
    c.mouse_look = c.rotation && !ks_client_camera_detached();
    return c;
}

int class_camera_wants_free_cursor(const Client *client) {
    if (!me_active(client)) return 0;
    const ClassCamera c = class_camera_current(client);
    return c.free_mouse;
}

int class_camera_player_third_person(const Player *player) {
    if (!player || !player->game) return 0;
    const ClassCamera c = class_camera_make(class_of(player->game, player), player->game->config.third_person, 0);
    return class_camera_third_person(&c);
}

int class_camera_mesh_third_person(const Player *player) {
    if (!player || !player->game) return 0;
    const ClassOverrides *o = class_of(player->game, player);
    const int offset = o && (o->cam_offset[0] != 0.0f || o->cam_offset[1] != 0.0f || o->cam_offset[2] != 0.0f);
    return player->game->config.third_person || (g_cc.cam_valid && g_cc.cam.type != CLASS_CAMERA_FPS) || offset;
}

int class_camera_draws_own_model(const Client *client) {
    const Player *me = client ? client->me : NULL;
    if (!me || !me->active || !me->mesh || spectate_active() || third_person_view(client)) return 0;
    const ClassCamera c = class_camera_current(client);
    return class_camera_third_person(&c);
}

static ClassCameraRig rig_frame(const Client *client, const ClassCamera *c, const vec3 eye) {
    ClassCameraRig r = {0};
    r.eye = eye;
    r.pitch = g_cc.rig.x;
    r.yaw = g_cc.rig.y;
    r.tilt = g_cc.tilt;
    r.roll = camera_feel_wall_roll();
    if (c->third_person) { // camera.position.set(5, 2, 14) = config.thrdPX / PY / PZ (deob:260592-260594)
        r.local = (vec3) {CLASS_CAMERA_TP_X, CLASS_CAMERA_TP_Y, CLASS_CAMERA_TP_Z};
        r.local_offset = 1;
    } else if (prop_hunt_me(client)) { // the Prop team: (0, 0, the wheel's distance) (deob:260595-260599)
        r.local = (vec3) {0.0f, 0.0f, prop_hunt_camera_distance()};
        r.local_offset = 1;
    }
    return r;
}

static float wrap_pi(const float a) {
    return a - 2.0f * (float) M_PI * roundf(a / (2.0f * (float) M_PI));
}

void class_camera_controls_take(Client *client, vec2 *look) {
    g_cc.block_detached = ks_client_camera_detached();
    g_cc.aimed = 0;
    if (!g_cc.rig_set) { // the spawn's (and a forceDir teleport's) object.rotation.y = xDire, pchObjc = yDire (deob:258177-258181)
        g_cc.rig = *look;
        g_cc.rig_set = 1;
        g_cc.rig_dirty = 0;
    } else if (g_cc.rig_dirty && class_camera_current(client).mouse_look) {
        // updateMouseInp: yDr = pchObjc.rotation.x, xDr = object.rotation.y % PI2 after the script turned them
        *look = g_cc.rig;
        g_cc.rig_dirty = 0;
    }
    g_cc.look_before = *look;
}

int class_camera_input_aim(Client *client, const vec2 look, vec2 *aim) {
    const Player *me = client ? client->me : NULL;
    if (!me || !me->active) return 0;
    const ClassCamera c = class_camera_current(client);
    if (c.mouse_look) { // updateMouseInp turned the controls with the mouse (the yaw unwrapped, as three's)
        g_cc.rig.x = look.x;
        g_cc.rig.y += wrap_pi(look.y - g_cc.look_before.y);
    }
    // the rig the camera block's lookAt sees: the eye before procInputs (object.position from the last
    // moveCam), the controls after this mouse, before lockCamP and the wall lean
    g_cc.look_rig = rig_frame(client, &c, eye_of(me));
    g_cc.look_rig_set = 1;
    if (!c.free_mouse || !client->window) return 0;
    // the camera the cursor's ray leaves (deob:260589-260653): posed at that rig, or where a detached
    // KrunkScript camera is
    Camera camera = client->camera;
    if (!g_cc.block_detached) {
        class_camera_pose(&c, &g_cc.look_rig, &camera);
        if (!c.aim_fov) camera.zoom = 1.0f;
    }
    int ww = 0, wh = 0, fw = 0, fh = 0;
    glfwGetWindowSize(client->window, &ww, &wh);
    glfwGetFramebufferSize(client->window, &fw, &fh);
    if (ww <= 0 || wh <= 0) return 0;
    // CONTROLS.mouseX / mouseY only follow the cursor while the controls are on (mousemove needs
    // `enabled`, deob-10.0.3.js:235482-235495): over the menu the aim keeps the last spot
    if (client->mouse_state.locked) {
        glfwGetCursorPos(client->window, &g_cc.cursor_x, &g_cc.cursor_y);
        g_cc.cursor_set = 1;
    }
    if (!g_cc.cursor_set) return 0;
    // mouseX / innerWidth * 2 - 1, -mouseY / innerHeight * 2 + 1 (deob:260652); camera.aspect = getRes()
    const float ndc_x = (float) (g_cc.cursor_x / ww * 2.0 - 1.0), ndc_y = (float) (-g_cc.cursor_y / wh * 2.0 + 1.0);
    const float aspect = fw > 0 && fh > 0 ? (float) fw / (float) fh : (float) ww / (float) wh;
    float yaw, pitch;
    if (!class_camera_free_aim(&c, &camera, aspect, ndc_x, ndc_y, me->position, me->height, &g_cc.hit, &yaw, &pitch)) return 0;
    aim->x = pitch;
    aim->y = yaw;
    g_cc.aimed = 1;
    g_cc.aim_pitch = pitch;
    return 1;
}

void class_camera_after_input(Client *client, vec2 *look, const float frame_ms) {
    const Player *me = client ? client->me : NULL;
    if (!me || !me->active) return;
    const ClassCamera c = class_camera_current(client);
    // lockCamP: object.rotation.y = camChase (deob:260824-260836)
    float chase_yaw = 0.0f;
    if (c.face_player != 0.0f) {
        g_cc.rig.y = class_camera_face_player(&c, &g_cc.chase, me->direction.y, frame_ms);
        chase_yaw = g_cc.rig.y;
    }
    g_cc.rig.x = class_camera_lock_pitch(&c, g_cc.rig.x); // lckCmYR, yCmLim: pchObjc.rotation.x
    // Howler.pos(eye), Howler.orientation(sin(xDr + chase + PI), yDr, cos(xDr + chase + PI)) on the input frame
    // (deob:260850-260851); xDr / yDr are the input's look (yDr the free-mouse aim's pitch)
    g_cc.ear = eye_of(me);
    g_cc.ear_yaw = look->y + chase_yaw;
    g_cc.ear_pitch = atanf(g_cc.aimed ? g_cc.aim_pitch : look->x); // the vector (-sin, yDr, -cos) as a pitch
    g_cc.ear_set = 1;
    // the next updateMouseInp turns the controls from there (xDr / yDr = their rotation)
    if (c.mouse_look) *look = g_cc.rig;
}

vec3 class_camera_object_rotation(void) {
    return (vec3) {g_cc.tilt, g_cc.rig.y, camera_feel_wall_roll()};
}

int class_camera_listener(const Client *client, vec3 *position, float *yaw, float *pitch) {
    // spectating: Howler follows the spectate rig (deob:260962-260963), KrunkNative's camera there
    if (!g_cc.ear_set || !client || !client->me || spectate_active()) return 0;
    *position = g_cc.ear;
    *yaw = g_cc.ear_yaw;
    *pitch = g_cc.ear_pitch;
    return 1;
}

static void ks_flags_clear(void) {
    g_cc.ks_pos_mask = g_cc.ks_cam_rot = g_cc.ks_cam_pos = 0;
}

void class_camera_frame_begin(Client *client) {
    if (!client) return;
    g_cc.posed = 0;
    const int active = me_active(client);
    if (g_cc.was_active && !active) g_cc.object_set = 0;
    g_cc.was_active = active;
    if (!active) ks_flags_clear(); // no camera block this frame: the script's writes simply stand
    if (!g_cc.ortho) return;
    client->camera.near = g_cc.near;
    client->camera.far = g_cc.far;
    client->camera.ortho = 0.0f;
    g_cc.ortho = 0;
}

static void chain_pose(Client *client, const ClassCamera *c, const ClassCameraRig *rig) {
    ClassCameraChain k = {.rig = *rig, .ortho = c->type == CLASS_CAMERA_ORTHO};
    k.rig.eye = g_cc.object_pos;
    k.camera_position = g_cc.cam_pos;
    for (int i = 0; i < 9; i++) k.camera_rotation[i] = g_cc.cam_rot[i];
    if (k.ortho && !g_cc.ortho) {
        g_cc.near = client->camera.near;
        g_cc.far = client->camera.far;
        g_cc.ortho = 1;
    }
    class_camera_chain_pose(&k, &client->camera);
    if (k.ortho) {
        client->camera.ortho = c->ortho_f * 0.5f; // top = frustumSize / 2 (deob:231454-231457)
        client->camera.near = CLASS_CAMERA_ORTHO_NEAR;
        client->camera.far = CLASS_CAMERA_ORTHO_FAR;
    }
    g_cc.posed = 1;
}

int class_camera_apply(Client *client, const vec2 look) {
    (void) look;
    const Player *me = client->me;
    if (!me || !me->active || third_person_view(client)) {
        ks_flags_clear();
        return 0;
    }
    const ClassCamera c = class_camera_current(client);
    ClassCameraRig r = rig_frame(client, &c, eye_of(me));
    // camera_feel_apply posed rotation = the look + rotateCam's recoil / landing bob / shake
    r.feel_pitch = client->camera.rotation.x - me->direction.x;
    r.feel_yaw = client->camera.rotation.y - me->direction.y;
    if (!g_cc.cam_set) {
        g_cc.cam_pos = (vec3) {0};
        class_camera_m3_identity(g_cc.cam_rot);
        g_cc.cam_set = 1;
    }
    // the script wrote the chain this frame (a turn of the controls shows before xDr takes it)
    const int touched = g_cc.ks_pos_mask || g_cc.ks_cam_rot || g_cc.ks_cam_pos || g_cc.rig_dirty;
    if (!g_cc.block_detached) {
        // the camera block: moveCam, camera.position (the offsets), the lookAt / rotateCam
        if (!(g_cc.ks_pos_mask & 1)) g_cc.object_pos.x = r.eye.x;
        if (!(g_cc.ks_pos_mask & 2)) g_cc.object_pos.y = r.eye.y;
        if (!(g_cc.ks_pos_mask & 4)) g_cc.object_pos.z = r.eye.z;
        g_cc.object_set = 1;
        ClassCameraRig lr = g_cc.look_rig_set ? g_cc.look_rig : r;
        lr.feel_pitch = r.feel_pitch;
        lr.feel_yaw = r.feel_yaw;
        vec3 pos;
        float rot[9];
        class_camera_local(&c, &lr, g_cc.cam_rot, &pos, rot);
        if (!g_cc.ks_cam_pos) g_cc.cam_pos = pos;
        if (!g_cc.ks_cam_rot) for (int i = 0; i < 9; i++) g_cc.cam_rot[i] = rot[i];
    } else if (c.has_offset && !g_cc.ks_cam_pos) {
        // detached, camera.position.set is skipped but `+= camOffX / Y / Z` is not (deob:260591-260646):
        // the offset adds up every frame
        g_cc.cam_pos = (vec3) {g_cc.cam_pos.x + c.offset.x, g_cc.cam_pos.y + c.offset.y, g_cc.cam_pos.z + c.offset.z};
    }
    g_cc.look_rig_set = 0;
    ks_flags_clear();
    // the plain first-person camera stands unless something here differs from it
    if (!g_cc.block_detached && !class_camera_active(&c) && !touched && g_cc.tilt == 0.0f && r.roll == 0.0f) return 0;
    chain_pose(client, &c, &r);
    if (!c.aim_fov) client->camera.zoom = 1.0f; // RENDER.zoom: zoomVal = 1 without aimFov
    return 1;
}

// ---- KrunkScript GAME.CAMERA (deob:210533-210675); ks_input.c calls these

// The model holds no camera yet (no life so far, or the life ended: KrunkNative's death camera is not
// the controls): it takes the camera shown
static void model_from_camera(const Client *client) {
    if (g_cc.object_set) return;
    g_cc.object_pos = client->camera.position;
    g_cc.object_set = 1;
    if (!me_active(client) || !g_cc.rig_set) {
        g_cc.rig = (vec2) {client->camera.rotation.x, client->camera.rotation.y};
        g_cc.rig_set = 1;
    }
    g_cc.cam_pos = (vec3) {0};
    class_camera_m3_identity(g_cc.cam_rot);
    g_cc.cam_set = 1;
}

void class_camera_ks_detach(Client *client) {
    if (client && !ks_client_camera_detached()) model_from_camera(client);
}

// object.position after this frame's camera block: moveCam's eye unless the script set it
vec3 class_camera_ks_position(const Client *client) {
    vec3 p = g_cc.object_set ? g_cc.object_pos : client ? client->camera.position : (vec3) {0};
    if (me_active(client) && !ks_client_camera_detached()) {
        const vec3 eye = eye_of(client->me);
        if (!(g_cc.ks_pos_mask & 1)) p.x = eye.x;
        if (!(g_cc.ks_pos_mask & 2)) p.y = eye.y;
        if (!(g_cc.ks_pos_mask & 4)) p.z = eye.z;
    }
    return p;
}

void class_camera_ks_set_position(Client *client, const int axes, const vec3 v) {
    if (!client) return;
    model_from_camera(client);
    g_cc.object_pos = class_camera_ks_position(client); // the axes it leaves are moveCam's
    if (axes & 1) g_cc.object_pos.x = v.x;
    if (axes & 2) g_cc.object_pos.y = v.y;
    if (axes & 4) g_cc.object_pos.z = v.z;
    g_cc.ks_pos_mask |= axes;
}

vec3 class_camera_ks_rotation(void) {
    return class_camera_object_rotation();
}

void class_camera_ks_set_rotation(Client *client, const int axis, const float v) {
    if (!client) return;
    model_from_camera(client);
    if (axis == 0) g_cc.tilt = v;
    else if (axis == 1) {
        g_cc.rig.y = v;
        g_cc.rig_dirty = 1;
    } else camera_feel_set_wall_roll(v);
}

void class_camera_ks_rotate(Client *client, const vec3 euler) {
    if (!client) return;
    model_from_camera(client);
    class_camera_euler_xyz(euler, g_cc.cam_rot); // rotateCam: camera.rotation (x, y, z)
    g_cc.ks_cam_rot = 1;
}

void class_camera_ks_look_at(Client *client, const vec3 target) {
    if (!client) return;
    model_from_camera(client);
    // from object.position: pitch = -atan2(dy, |dxz|), yaw = -atan2(-dx, dz) || 0; camera.rotation and
    // camera.position back to 0 (deob:210561-210577)
    const vec3 from = class_camera_ks_position(client);
    const double dx = (double) from.x - target.x, dy = (double) from.y - target.y, dz = (double) from.z - target.z;
    const double flat = sqrt(dx * dx + dz * dz);
    double pitch = -atan2(dy, flat), yaw = -atan2(-dx, dz);
    if (pitch != pitch) pitch = 0.0;
    if (yaw != yaw) yaw = 0.0;
    g_cc.rig = (vec2) {(float) pitch, (float) yaw};
    g_cc.rig_dirty = 1;
    g_cc.cam_pos = (vec3) {0};
    class_camera_m3_identity(g_cc.cam_rot);
    g_cc.ks_cam_rot = g_cc.ks_cam_pos = 1;
}

void class_camera_ks_pose(Client *client) {
    // a detached camera with no camera block this frame (no life): the chain as the script left it
    if (!client || g_cc.posed || !ks_client_camera_detached()) return;
    model_from_camera(client);
    const ClassCamera c = class_camera_current(client);
    ClassCameraRig r = {0};
    r.pitch = g_cc.rig.x;
    r.yaw = g_cc.rig.y;
    r.tilt = g_cc.tilt;
    r.roll = camera_feel_wall_roll();
    chain_pose(client, &c, &r);
}

void class_camera_on_spawn(void) {
    g_cc.cam_valid = 0; // updateCamMouse (the new life's class)
    g_cc.chase = NAN;
    g_cc.rig_set = 0;
    g_cc.rig_dirty = 0;
    g_cc.hit = (vec3) {0};
    // CONTROLS.reset(true): camera.rotation 0 (deob:236412-236417); object.rotation.x / z stay
    g_cc.cam_pos = (vec3) {0};
    class_camera_m3_identity(g_cc.cam_rot);
    g_cc.cam_set = 1;
    g_cc.object_set = 0;
}

void class_camera_look_reset(void) {
    g_cc.rig_set = 0;
}
