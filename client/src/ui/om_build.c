// KrunkNative Object Manager building: the Physics Gun and the btools (the Tool Gun's Move and
// Resize tools). Not Krunker v7.1.4 features (doc/v7/decisions.md "Physics Gun", "Building
// tools"). The authority validates and applies every pose (shared/src/object_manager.c
// object_manager_edit); this file only aims, drags and sends OM_ACTION_EDIT, and draws the beam,
// the selection box and the handles (client/src/om_gizmo.c).
//
// Every object the crosshair is on can be taken (object_manager_pick: jump pads, teleporters,
// ladders, ramps, triggers, pickups, zones, planes as well as solid ones): spawned objects (your
// own, any for devs and the host) and, for devs and the host (OM_CAP_MAP), the map's own objects.
//
// Physics Gun (WEAPON_ID_PHYS_GUN in hand): hold fire on an object to grab it; it follows the crosshair at its grab distance, the wheel changes the
// distance (OM_PHYSGUN_DIST_*), the rotate key (E, cont_omRotateKey) held turns it with the mouse
// instead of the view, a quarter turn per 90 degrees of mouse (Krunker's AABB collision only
// follows quarter turns). Releasing fire leaves it where it is (frozen), or with "Drop" on lets it
// down onto the surface below (OM_EDIT_DROP). Weapon swaps are off while carrying.
//
// btools (the Tool Gun with Move or Resize, the Tools pane): the pointer stays locked and the
// crosshair is the cursor. Fire on a spawned object selects it (a yellow box); Move shows X / Y /
// Z arrows, Resize a handle on each face. Fire held on a handle drags it: the handle follows the
// point of its axis nearest to the crosshair ray, the change snaps to the grid step (any step from
// OB_SNAP_MIN to OB_SNAP_MAX units or off: the Tools pane's field, the snap keys PageDown / PageUp, the
// console's `tool_snap`), Resize keeps the opposite face (crouch held: both faces move) and
// OM_EDIT_SIZE_MIN. Fire on nothing clears the selection.
//
// Edits go out at most every OB_SEND_SECONDS while a grab or drag changes the pose, and once at
// its release; the first of each opens an undo step (OM_EDIT_BEGIN), so U takes a whole grab or
// drag back. Others see the result through SPAWNED_OBJ OM_SPAWNED_UPDATE.
//
// Properties (the Tool Gun's third btool): fire on an object selects it and opens its fields in the
// Object Manager window (object_manager_ui_open_props); the Properties key (I, cont_omPropsKey) does
// the same for the Move / Resize selection, else for the object at the crosshair.
#include <client.h>
#include <html_dom.h>
#include <ui_logic.h>
#include <object_manager.h>
#include <om_gizmo.h>
#include <weapons.h>
#include <GLFW/glfw3.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OB_SEND_SECONDS 0.1   // edits while dragging: 10 / s (the server takes one per OM_EDIT_INTERVAL)
#define OB_SNAP_STORAGE "kn_omSnapStep" // the step in units, "0" off (kn_omSnap held the old 1 / 2 / 5 / 10 index)
#define OB_SNAP_OLD_STORAGE "kn_omSnap"
#define OB_DROP_STORAGE "kn_omPhysDrop"
#define OB_WHEEL_STEP 10.0f   // grab distance per wheel step (units)
#define OB_ARROW 7.0f         // a Move arrow reaches this far past the face
#define OB_HANDLE 2.6f        // a handle's box (units)
#define OB_HANDLES_MAX 6

static const float k_snaps[OB_SNAPS] = {1.0f, 2.0f, 5.0f, 10.0f}; // the old indices
// The snap keys step through these (and the field takes any value between)
static const float k_snap_steps[] = {0.0f, 0.1f, 0.25f, 0.5f, 1.0f, 2.0f, 2.5f, 5.0f, 10.0f, 20.0f, 25.0f, 50.0f, 100.0f};
#define OB_SNAP_STEP_COUNT ((int) (sizeof(k_snap_steps) / sizeof(k_snap_steps[0])))

typedef struct {
    int axis;  // 0 x, 1 y, 2 z
    int sign;  // the face (+1 / -1); Move: +1
    vec3 tip;  // the handle box's centre
} ObHandle;

static struct {
    int loaded;
    float snap;           // the grid step (units), 0 off
    int drop;             // the Physics Gun's release: 0 freeze in place, 1 down onto the surface below
    int was_shoot;
    // the Physics Gun
    int grab;             // uid + 1, 0 none
    float dist;
    vec3 offset;          // the object's feet from the grab point
    float turn;           // rotate-key mouse yaw not yet turned into a quarter turn
    int releasing;        // fire let go: the last edit waits for the server's pace
    // btools
    int sel;              // uid + 1, 0 none
    int drag;             // handle index + 1, 0 none
    ObHandle handle;
    ObjectManagerPose from;
    float t0;
    // the pose being sent (both)
    int uid;
    ObjectManagerPose pose, sent;
    double sent_at;
    int begin;            // the next edit opens an undo step
} g_ob;

static void ob_load(void) {
    if (g_ob.loaded) return;
    g_ob.loaded = 1;
    g_ob.snap = 5.0f;
    char value[24];
    if (html_storage_get(OB_SNAP_STORAGE, value, sizeof(value)) >= 0) {
        const float v = (float) atof(value);
        if (isfinite(v)) g_ob.snap = v <= 0.0f ? 0.0f : fminf(fmaxf(v, OB_SNAP_MIN), OB_SNAP_MAX);
    } else if (html_storage_get(OB_SNAP_OLD_STORAGE, value, sizeof(value)) >= 0 && atoi(value) >= 0 && atoi(value) < OB_SNAPS) {
        g_ob.snap = k_snaps[atoi(value)]; // the old 1 / 2 / 5 / 10 pick carries over
    }
    if (html_storage_get(OB_DROP_STORAGE, value, sizeof(value)) >= 0) g_ob.drop = atoi(value) != 0;
}

float om_build_snap(void) {
    ob_load();
    return g_ob.snap;
}

float om_build_set_snap(const float step) {
    ob_load();
    if (!isfinite(step)) return g_ob.snap;
    g_ob.snap = step <= 0.0f ? 0.0f : fminf(fmaxf(roundf(step * 1000.0f) / 1000.0f, OB_SNAP_MIN), OB_SNAP_MAX);
    char value[24];
    snprintf(value, sizeof(value), "%g", g_ob.snap);
    html_storage_set(OB_SNAP_STORAGE, value);
    return g_ob.snap;
}

// The snap keys: the next common step up (dir 1) or down (-1) from the current one
float om_build_step_snap(const int dir) {
    const float now = om_build_snap();
    if (dir > 0) {
        for (int i = 0; i < OB_SNAP_STEP_COUNT; i++) {
            if (k_snap_steps[i] > now + 1e-4f) return om_build_set_snap(k_snap_steps[i]);
        }
        return now;
    }
    for (int i = OB_SNAP_STEP_COUNT; i-- > 0;) {
        if (k_snap_steps[i] < now - 1e-4f) return om_build_set_snap(k_snap_steps[i]);
    }
    return now;
}

void om_build_snap_text(char *out, const size_t size) {
    const float s = om_build_snap();
    if (s <= 0.0f) snprintf(out, size, "off");
    else snprintf(out, size, "%g", s);
}

int om_build_drop(void) {
    ob_load();
    return g_ob.drop;
}

void om_build_set_drop(const int on) {
    ob_load();
    g_ob.drop = on != 0;
    html_storage_set(OB_DROP_STORAGE, g_ob.drop ? "1" : "0");
}

int om_build_grabbing(void) { return g_ob.grab != 0; }

// ---------------------------------------------------------------- geometry

static vec3 ob_eye(const Player *me) {
    return (vec3) {me->position.x, me->position.y + me->height - game_constants.camera_height, me->position.z};
}

static vec3 ob_dir(const float pitch, const float yaw) {
    return (vec3) {-sinf(yaw) * cosf(pitch), sinf(pitch), -cosf(yaw) * cosf(pitch)};
}

static vec3 v_add(const vec3 a, const vec3 b) { return (vec3) {a.x + b.x, a.y + b.y, a.z + b.z}; }
static vec3 v_sub(const vec3 a, const vec3 b) { return (vec3) {a.x - b.x, a.y - b.y, a.z - b.z}; }
static vec3 v_scale(const vec3 a, const float s) { return (vec3) {a.x * s, a.y * s, a.z * s}; }
static float v_dot(const vec3 a, const vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static vec3 v_axis(const int axis) { return (vec3) {axis == 0 ? 1.0f : 0.0f, axis == 1 ? 1.0f : 0.0f, axis == 2 ? 1.0f : 0.0f}; }
static float v_get(const vec3 v, const int axis) { return axis == 0 ? v.x : axis == 1 ? v.y : v.z; }

static vec3 ob_centre(const ObjectManagerPose *p) { return (vec3) {p->position.x, p->position.y + p->size.y * 0.5f, p->position.z}; }

// The parameter along the line (origin, unit axis) of its point nearest to the ray (eye, unit dir)
static int ob_axis_param(const vec3 eye, const vec3 dir, const vec3 origin, const vec3 axis, float *t) {
    const vec3 w0 = v_sub(eye, origin);
    const float b = v_dot(dir, axis), d = v_dot(dir, w0), e = v_dot(axis, w0);
    const float denom = 1.0f - b * b;
    if (fabsf(denom) < 1e-4f) return 0; // looking along the axis
    *t = (e - b * d) / denom;
    return 1;
}

// The object the crosshair is on that this player may edit, -1 none; `point` where the ray met
// it. The map's own objects only with OM_CAP_MAP (the authority checks again).
static int ob_pick(Client *client, const Player *me, vec3 *point) {
    ObjectManagerAim aim;
    if (!object_manager_pick(&client->game, me, me->direction.x, me->direction.y, &aim)) return -1;
    if (aim.object < 0 || !object_manager_editable(&client->game, aim.object)) return -1;
    if (object_manager_is_map_object(&client->game, aim.object) && !(object_manager_ui_caps() & OM_CAP_MAP)) return -1;
    *point = aim.point;
    return aim.object;
}

static int ob_handles(const int tool, const ObjectManagerPose *p, ObHandle *out) {
    const vec3 c = ob_centre(p);
    int n = 0;
    if (tool == OM_TOOL_PROPERTIES) return 0; // a selection box only
    for (int axis = 0; axis < 3; axis++) {
        const float half = fmaxf(v_get(p->size, axis), 1.0f) * 0.5f; // (flat things: pads, planes)
        if (tool == OM_TOOL_MOVE) {
            out[n++] = (ObHandle) {axis, 1, v_add(c, v_scale(v_axis(axis), half + OB_ARROW))};
        } else {
            for (int sign = 1; sign >= -1; sign -= 2) out[n++] = (ObHandle) {axis, sign, v_add(c, v_scale(v_axis(axis), (half + 1.5f) * (float) sign))};
        }
    }
    return n;
}

// The handle under the crosshair (nearest), -1 none
static int ob_hover(const Player *me, const ObHandle *h, const int count) {
    const vec3 eye = ob_eye(me), dir = ob_dir(me->direction.x, me->direction.y);
    const vec3 segment = v_scale(dir, OM_AIM_RANGE);
    int best = -1;
    float best_t = 2.0f;
    for (int i = 0; i < count; i++) {
        const vec3 feet = {h[i].tip.x, h[i].tip.y - OB_HANDLE * 0.5f, h[i].tip.z};
        const float t = line_in_rect(eye, segment, feet, (vec3) {OB_HANDLE, OB_HANDLE, OB_HANDLE});
        if (t > 0.0f && t <= 1.0f && t < best_t) {
            best_t = t;
            best = i;
        }
    }
    return best;
}

// ---------------------------------------------------------------- edits out

static int ob_server_ok(const Client *client) {
    return client && (client->offline || client->net_server_minor >= NET_PROTOCOL_MINOR_V7);
}

static int ob_pose_differs(const ObjectManagerPose *a, const ObjectManagerPose *b) {
    return fabsf(a->position.x - b->position.x) > 0.02f || fabsf(a->position.y - b->position.y) > 0.02f || fabsf(a->position.z - b->position.z) > 0.02f ||
           fabsf(a->size.x - b->size.x) > 0.005f || fabsf(a->size.y - b->size.y) > 0.005f || fabsf(a->size.z - b->size.z) > 0.005f || a->quarter != b->quarter;
}

static void ob_send(Client *client, const int source, const int flags) {
    const Player *me = client->me;
    if (!me || !ob_server_ok(client)) return;
    NetSpawnEditPacket e;
    memset(&e, 0, sizeof(e));
    e.req = (NetSpawnReqPacket) {.action = OM_ACTION_EDIT, .item = (int16_t) source, .pitch = me->direction.x, .yaw = me->direction.y};
    e.uid = g_ob.uid;
    e.position[0] = g_ob.pose.position.x, e.position[1] = g_ob.pose.position.y, e.position[2] = g_ob.pose.position.z;
    e.size[0] = g_ob.pose.size.x, e.size[1] = g_ob.pose.size.y, e.size[2] = g_ob.pose.size.z;
    e.quarter = (uint8_t) g_ob.pose.quarter;
    e.flags = (uint8_t) (flags | (g_ob.begin ? OM_EDIT_BEGIN : 0));
    client_net_send(client, NET_PACKET_SPAWN_REQ, &e, sizeof(e));
    g_ob.begin = 0;
    g_ob.sent = g_ob.pose;
    g_ob.sent_at = glfwGetTime();
}

// While a grab / drag goes on: the pose when it changed, at the drag pace
static void ob_send_live(Client *client, const int source) {
    if (ob_pose_differs(&g_ob.pose, &g_ob.sent) && glfwGetTime() - g_ob.sent_at >= OB_SEND_SECONDS) ob_send(client, source, 0);
}

// The last edit of a grab / drag once the server's pace allows it (1 sent or nothing to send)
static int ob_send_final(Client *client, const int source, const int flags) {
    if (!ob_pose_differs(&g_ob.pose, &g_ob.sent) && !flags) return 1;
    if (glfwGetTime() - g_ob.sent_at < OM_EDIT_INTERVAL + 0.02) return 0;
    ob_send(client, source, flags);
    return 1;
}

// A refused edit (SPAWN_RESULT): the grab or drag ends, the object stays as the server has it
void om_build_refused(const int status) {
    if (status == OM_STATUS_RATE) return;
    g_ob.grab = 0;
    g_ob.drag = 0;
    g_ob.releasing = 0;
}

void om_build_reset(void) {
    g_ob.grab = g_ob.sel = g_ob.drag = g_ob.releasing = 0;
}

// ---------------------------------------------------------------- the Physics Gun

static void ob_physgun(Client *client, Player *me, Input *input, const float prev_pitch, const float prev_yaw, const int pressed, const int shoot,
                       const int sampled) {
    if (g_ob.releasing) { // fire was let go: the last pose (and Drop) as soon as the pace allows
        if (ob_send_final(client, OM_EDIT_SOURCE_PHYSGUN, g_ob.drop ? OM_EDIT_DROP : 0)) g_ob.releasing = 0;
        return;
    }
    if (!g_ob.grab && pressed) {
        vec3 point;
        const int uid = ob_pick(client, me, &point);
        if (uid < 0 || !object_manager_pose(&client->game, uid, &g_ob.pose)) return;
        const vec3 eye = ob_eye(me);
        const vec3 d = v_sub(point, eye);
        g_ob.dist = fminf(OM_PHYSGUN_DIST_MAX, fmaxf(OM_PHYSGUN_DIST_MIN, sqrtf(v_dot(d, d))));
        g_ob.offset = v_sub(g_ob.pose.position, v_add(eye, v_scale(ob_dir(me->direction.x, me->direction.y), g_ob.dist)));
        g_ob.uid = uid;
        g_ob.grab = uid + 1;
        g_ob.sent = g_ob.pose;
        g_ob.turn = 0.0f;
        g_ob.begin = 1;
    }
    if (!g_ob.grab) return;
    if (!object_manager_editable(&client->game, g_ob.uid)) { // removed meanwhile
        g_ob.grab = 0;
        return;
    }
    input->swap = 0; // no weapon switching while carrying: the wheel and the rotate key are the gun's
    input->select_slot = 0;
    if (sampled && input->scroll) {
        g_ob.dist = fminf(OM_PHYSGUN_DIST_MAX, fmaxf(OM_PHYSGUN_DIST_MIN, g_ob.dist - (float) input->scroll * OB_WHEEL_STEP));
        input->scroll = 0;
    }
    if (ui_controls_down(UI_CONTROL_OM_ROTATE)) { // the mouse turns the object, the view stays
        g_ob.turn += input->y_dir - prev_yaw;
        input->x_dir = prev_pitch;
        input->y_dir = prev_yaw;
        const float quarter = (float) M_PI / 2.0f;
        while (g_ob.turn > quarter * 0.5f || g_ob.turn < -quarter * 0.5f) {
            const int turns = g_ob.turn > 0.0f ? 1 : -1;
            ObjectManagerPose turned;
            object_manager_pose_turn(&g_ob.pose, turns, &turned);
            g_ob.pose = turned;
            g_ob.turn -= (float) turns * quarter;
        }
    }
    const vec3 eye = ob_eye(me);
    const vec3 point = v_add(eye, v_scale(ob_dir(input->x_dir, input->y_dir), g_ob.dist));
    g_ob.pose.position = v_add(point, g_ob.offset);
    if (shoot) {
        ob_send_live(client, OM_EDIT_SOURCE_PHYSGUN);
        return;
    }
    g_ob.grab = 0; // released: frozen where it is, or dropped
    g_ob.releasing = !ob_send_final(client, OM_EDIT_SOURCE_PHYSGUN, g_ob.drop ? OM_EDIT_DROP : 0);
}

// ---------------------------------------------------------------- btools

static void ob_btools(Client *client, const Player *me, const int tool, const int pressed, const int shoot) {
    const Game *game = &client->game;
    if (g_ob.sel && !object_manager_editable(game, g_ob.sel - 1)) g_ob.sel = g_ob.drag = 0;
    if (g_ob.releasing) {
        if (ob_send_final(client, OM_EDIT_SOURCE_BTOOLS, 0)) g_ob.releasing = 0;
        return;
    }
    ObjectManagerPose pose;
    ObHandle handles[OB_HANDLES_MAX];
    const int count = g_ob.sel && object_manager_pose(game, g_ob.sel - 1, &pose) ? ob_handles(tool, &pose, handles) : 0;
    const vec3 eye = ob_eye(me), dir = ob_dir(me->direction.x, me->direction.y);
    if (pressed && !g_ob.drag && tool == OM_TOOL_PROPERTIES) { // the selection's fields in the window
        vec3 point;
        const int uid = ob_pick(client, me, &point);
        g_ob.sel = uid >= 0 ? uid + 1 : 0;
        if (uid >= 0) object_manager_ui_open_props(client, uid);
        return;
    }
    if (pressed && !g_ob.drag) {
        const int hover = ob_hover(me, handles, count);
        float t;
        if (hover >= 0 && ob_axis_param(eye, dir, ob_centre(&pose), v_axis(handles[hover].axis), &t)) {
            g_ob.drag = hover + 1;
            g_ob.handle = handles[hover];
            g_ob.from = pose;
            g_ob.pose = g_ob.sent = pose;
            g_ob.uid = g_ob.sel - 1;
            g_ob.t0 = t;
            g_ob.begin = 1;
        } else {
            vec3 point;
            const int uid = ob_pick(client, me, &point);
            g_ob.sel = uid >= 0 ? uid + 1 : 0;
        }
    }
    if (!g_ob.drag) return;
    float t;
    if (ob_axis_param(eye, dir, ob_centre(&g_ob.from), v_axis(g_ob.handle.axis), &t)) {
        const float delta = t - g_ob.t0;
        if (tool == OM_TOOL_MOVE) object_manager_pose_move(&g_ob.from, g_ob.handle.axis, delta, om_build_snap(), &g_ob.pose);
        else object_manager_pose_resize(&g_ob.from, g_ob.handle.axis, g_ob.handle.sign, delta * (float) g_ob.handle.sign, om_build_snap(),
                                        ui_controls_down(UI_CONTROL_CROUCH), &g_ob.pose);
    }
    if (shoot) {
        ob_send_live(client, OM_EDIT_SOURCE_BTOOLS);
        return;
    }
    g_ob.drag = 0;
    g_ob.releasing = !ob_send_final(client, OM_EDIT_SOURCE_BTOOLS, 0);
}

// The Properties key with the Tool Gun in hand: the btools selection's fields, else the object's at
// the crosshair. 1 the window opened.
int om_build_props_key(Client *client) {
    const Player *me = client ? client->me : NULL;
    if (!me || !me->active || !me->weapon || !me->weapon->kn_tool) return 0;
    int uid = g_ob.sel && object_manager_editable(&client->game, g_ob.sel - 1) ? g_ob.sel - 1 : -1;
    if (uid < 0) {
        vec3 point;
        uid = ob_pick(client, me, &point);
    }
    if (uid < 0) return 0;
    g_ob.sel = uid + 1;
    object_manager_ui_open_props(client, uid);
    return 1;
}

int om_build_selection(void) { return g_ob.sel - 1; }

// Per frame while the game has the pointer (main.c, after the look and the input sample): the
// Physics Gun and the btools take the fire button, the wheel and the rotate key from the input.
void om_build_input(Client *client, Input *input, const float prev_pitch, const float prev_yaw, const int sampled) {
    Player *me = client ? client->me : NULL;
    if (!me || !me->active || !input) {
        g_ob.grab = g_ob.drag = g_ob.releasing = 0;
        return;
    }
    ob_load();
    const int shoot = sampled ? input->shoot != 0 : g_ob.was_shoot; // keys are sampled on input frames only
    const int pressed = shoot && !g_ob.was_shoot;
    g_ob.was_shoot = shoot;
    const Weapon *w = me->weapon;
    const int tool = object_manager_ui_tool();
    const ObjectManagerToolInfo *info = object_manager_tool_info(tool);
    if (w && w->kn_physgun) {
        g_ob.drag = 0;
        ob_physgun(client, me, input, prev_pitch, prev_yaw, pressed, shoot, sampled);
    } else if (w && w->kn_tool && info && info->handles) {
        if (g_ob.grab) g_ob.grab = 0;
        ob_btools(client, me, tool, pressed, shoot);
    } else {
        g_ob.grab = g_ob.drag = g_ob.releasing = 0; // another weapon: the grab / drag ends where it was
    }
}

// ---------------------------------------------------------------- drawing

static void ob_draw_handle(const int color, const vec3 tip) {
    om_gizmo_box(color, (vec3) {tip.x, tip.y - OB_HANDLE * 0.5f, tip.z}, (vec3) {OB_HANDLE, OB_HANDLE, OB_HANDLE});
}

void om_build_render(Client *client) {
    om_gizmo_clear();
    const Player *me = client ? client->me : NULL;
    const Weapon *w = me && me->active ? me->weapon : NULL;
    if (w && w->kn_physgun && g_ob.grab) {
        const vec3 eye = ob_eye(me), dir = ob_dir(me->direction.x, me->direction.y);
        const vec3 right = {cosf(me->direction.y), 0.0f, -sinf(me->direction.y)};
        const vec3 muzzle = v_add(v_add(eye, v_scale(dir, 3.0f)), v_add(v_scale(right, 1.2f), (vec3) {0.0f, -0.8f, 0.0f}));
        const vec3 end = v_add(eye, v_scale(dir, g_ob.dist));
        om_gizmo_line(OM_GIZMO_CYAN, muzzle, end);
        om_gizmo_box(OM_GIZMO_CYAN, g_ob.pose.position, g_ob.pose.size);
    }
    const int tool = object_manager_ui_tool();
    const ObjectManagerToolInfo *info = object_manager_tool_info(tool);
    ObjectManagerPose pose;
    if (w && w->kn_tool && info && info->handles && g_ob.sel && object_manager_pose(&client->game, g_ob.sel - 1, &pose)) {
        if (g_ob.drag) pose = g_ob.pose; // the dragged pose before the server's answer
        om_gizmo_box(OM_GIZMO_YELLOW, pose.position, pose.size);
        ObHandle handles[OB_HANDLES_MAX];
        const int count = ob_handles(tool, &pose, handles);
        const int hover = g_ob.drag ? -1 : ob_hover(me, handles, count);
        const vec3 c = ob_centre(&pose);
        for (int i = 0; i < count; i++) {
            const int axis_color = handles[i].axis == 0 ? OM_GIZMO_RED : handles[i].axis == 1 ? OM_GIZMO_GREEN : OM_GIZMO_BLUE;
            const int dragged = g_ob.drag && handles[i].axis == g_ob.handle.axis && handles[i].sign == g_ob.handle.sign;
            const int color = i == hover || dragged ? OM_GIZMO_WHITE : axis_color;
            if (tool == OM_TOOL_MOVE) om_gizmo_line(color, c, handles[i].tip);
            ob_draw_handle(color, handles[i].tip);
        }
    }
    if (client) om_gizmo_render(&client->camera);
}

void om_build_fini(void) { om_gizmo_fini(); }

// Input scripts (`om aim <i>`): the look (pitch, yaw) from the eye to handle `index` of the
// selection under the current tool. 1 found.
int om_build_handle_look(Client *client, const int index, float *pitch, float *yaw) {
    const Player *me = client ? client->me : NULL;
    ObjectManagerPose pose;
    const int tool = object_manager_ui_tool();
    if (!me || !g_ob.sel || !object_manager_pose(&client->game, g_ob.sel - 1, &pose)) return 0;
    ObHandle handles[OB_HANDLES_MAX];
    const int count = ob_handles(tool, &pose, handles);
    if (index < 0 || index >= count) return 0;
    const vec3 d = v_sub(handles[index].tip, ob_eye(me));
    *yaw = atan2f(-d.x, -d.z);
    *pitch = atan2f(d.y, sqrtf(d.x * d.x + d.z * d.z));
    return 1;
}
