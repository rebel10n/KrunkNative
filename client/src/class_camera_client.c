// Krunker v10.0.3's class camera, the client half (client/include/class_camera.h): the local player's
// class, the controls' own yaw / pitch (the rig), the free-mouse aim from the cursor, the pose and the
// Orthographic camera's near / far.
#include <class_camera.h>
#include <camera_feel.h> // the wall lean's roll
#include <game_config.h>
#include <ks_client.h>
#include <prop_hunt_client.h>
#include <third_person.h>
#include <spectate.h>
#include <ui_mobile.h>
#include <math.h>

static struct {
    float chase;    // player.camChase (NAN: null, deob-10.0.3.js:190600)
    vec3 hit;       // the free-mouse ray's last plane hit (iììiiií: intersectPlane keeps it on a miss)
    int rig_set;    // `rig` holds the controls' yaw / pitch
    vec2 rig;       // (pchObjc.rotation.x, object.rotation.y): with camRot it is the input look (updateMouseInp
                    // sets xDr / yDr from it every frame); without it only the locks and lockCamP move it
    double cursor_x, cursor_y; // CONTROLS.mouseX / mouseY (window coordinates)
    int cursor_set;
    int ortho;      // the camera wears the Orthographic near / far: put the perspective ones back
    float near, far;
} g_cc = {.chase = NAN};

static const ClassOverrides *class_of(const Game *game, const Player *player) {
    if (!game || !player || !game->classes || player->class_index < 0 || (size_t) player->class_index >= game->class_count) return NULL;
    return class_overrides(&game->classes[player->class_index]);
}

// updateCamMouse runs on our spawn and class change with our class (deob:190846); v10 keeps its result
// through death too (the death cam turns only with camRot). KrunkNative reads it every frame: the class
// is only set by a spawn, but KrunkScript's INPUTS.freeMouse (forceFreeCam) reaches it at once instead
// of at the next updateCamMouse.
ClassCamera class_camera_current(const Client *client) {
    const Player *me = client ? client->me : NULL;
    const int force_free = ks_client_pointer_free() && !ks_client_input_unlocked(); // INPUTS.freeMouse, not unlockMouse
    ClassCamera c = class_camera_make(class_of(client ? &client->game : NULL, me), client && client->game.config.third_person, force_free);
    if (ui_mobile_active()) c.free_mouse = 0; // isMobileControls: the phone controls aim
    return c;
}

int class_camera_wants_free_cursor(const Client *client) {
    if (!client || !client->me || !client->me->active) return 0;
    const ClassCamera c = class_camera_current(client);
    return c.free_mouse;
}

int class_camera_player_third_person(const Player *player) {
    if (!player || !player->game) return 0;
    const ClassCamera c = class_camera_make(class_of(player->game, player), player->game->config.third_person, 0);
    return class_camera_third_person(&c);
}

int class_camera_draws_own_model(const Client *client) {
    const Player *me = client ? client->me : NULL;
    if (!me || !me->active || !me->mesh || spectate_active() || third_person_view(client)) return 0;
    const ClassCamera c = class_camera_current(client);
    return class_camera_third_person(&c);
}

// The controls' yaw / pitch: the input look while camRot (updateMouseInp: object.rotation.y -= mouse,
// xDr = object.rotation.y), else their own since the spawn.
static vec2 rig_of(const ClassCamera *c, const vec2 look) {
    if (c->rotation || !g_cc.rig_set) {
        g_cc.rig = look;
        g_cc.rig_set = 1;
    }
    return g_cc.rig;
}

static ClassCameraRig rig_frame(const Client *client, const ClassCamera *c, const vec2 rig) {
    const Player *me = client->me;
    ClassCameraRig r = {0};
    r.eye = (vec3) {me->position.x, me->position.y + me->height - game_constants.camera_height, me->position.z}; // moveCam
    r.pitch = rig.x;
    r.yaw = rig.y;
    if (c->third_person) { // camera.position.set(5, 2, 14) = config.thrdPX / PY / PZ (deob:260592-260594)
        r.local = (vec3) {CLASS_CAMERA_TP_X, CLASS_CAMERA_TP_Y, CLASS_CAMERA_TP_Z};
        r.local_offset = 1;
    } else if (prop_hunt_me(client)) { // the Prop team: (0, 0, the wheel's distance) (deob:260595-260599)
        r.local = (vec3) {0.0f, 0.0f, prop_hunt_camera_distance()};
        r.local_offset = 1;
    }
    return r;
}

int class_camera_input_aim(Client *client, const vec2 look, vec2 *aim) {
    const Player *me = client ? client->me : NULL;
    if (!me || !me->active || !client->window) return 0;
    const ClassCamera c = class_camera_current(client);
    if (!c.free_mouse) return 0;
    // the camera the cursor's ray leaves: posed at this frame's (pre-input) eye as v10's is when its aim
    // runs (deob:260589-260653); a detached KrunkScript camera is where the script left it
    Camera camera = client->camera;
    if (!ks_client_camera_detached()) {
        ClassCameraRig r = rig_frame(client, &c, rig_of(&c, look));
        class_camera_pose(&c, &r, &camera);
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
    const double cx = g_cc.cursor_x, cy = g_cc.cursor_y;
    // mouseX / innerWidth * 2 - 1, -mouseY / innerHeight * 2 + 1 (deob:260652); camera.aspect = getRes()
    const float ndc_x = (float) (cx / ww * 2.0 - 1.0), ndc_y = (float) (-cy / wh * 2.0 + 1.0);
    const float aspect = fw > 0 && fh > 0 ? (float) fw / (float) fh : (float) ww / (float) wh;
    float yaw, pitch;
    if (!class_camera_free_aim(&c, &camera, aspect, ndc_x, ndc_y, me->position, me->height, &g_cc.hit, &yaw, &pitch)) return 0;
    aim->x = pitch;
    aim->y = yaw;
    return 1;
}

void class_camera_after_input(Client *client, vec2 *look, const float frame_ms) {
    const Player *me = client ? client->me : NULL;
    if (!me || !me->active) return;
    const ClassCamera c = class_camera_current(client);
    vec2 rig = rig_of(&c, *look);
    if (c.face_player != 0.0f) rig.y = class_camera_face_player(&c, &g_cc.chase, me->direction.y, frame_ms); // object.rotation.y = camChase
    rig.x = class_camera_lock_pitch(&c, rig.x); // pchObjc.rotation.x
    g_cc.rig = rig;
    // the next updateMouseInp turns the controls from there (xDr / yDr = their rotation) with camRot;
    // without it xDr / yDr stay where they were
    if (c.rotation) *look = rig;
}

void class_camera_frame_begin(Client *client) {
    if (!client || !g_cc.ortho) return;
    client->camera.near = g_cc.near;
    client->camera.far = g_cc.far;
    client->camera.ortho = 0.0f;
    g_cc.ortho = 0;
}

int class_camera_apply(Client *client, const vec2 look) {
    const Player *me = client->me;
    if (!me || !me->active || third_person_view(client)) return 0;
    const ClassCamera c = class_camera_current(client);
    if (!class_camera_active(&c)) return 0;
    ClassCameraRig r = rig_frame(client, &c, rig_of(&c, look));
    // camera_feel_apply posed rotation = the look + rotateCam's recoil / landing bob / shake; the wall
    // lean's roll (object.rotation.z, camera_feel_wall_lean)
    r.feel_pitch = client->camera.rotation.x - me->direction.x;
    r.feel_yaw = client->camera.rotation.y - me->direction.y;
    r.roll = camera_feel_wall_roll();
    if (c.type == CLASS_CAMERA_ORTHO) {
        g_cc.near = client->camera.near;
        g_cc.far = client->camera.far;
        g_cc.ortho = 1;
    }
    class_camera_pose(&c, &r, &client->camera);
    if (!c.aim_fov) client->camera.zoom = 1.0f; // RENDER.zoom: zoomVal = 1 without aimFov
    return 1;
}

void class_camera_on_spawn(void) {
    g_cc.chase = NAN;
    g_cc.rig_set = 0;
    g_cc.hit = (vec3) {0};
}

void class_camera_look_reset(void) {
    g_cc.rig_set = 0;
}
