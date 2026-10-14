// KrunkNative Object Manager motion on this client (client/include/om_interp.h).
#include <om_interp.h>
#include <GLFW/glfw3.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define OM_INTERP_MAX 64
#define OM_INTERP_HOLD 0.5 // a released carry stays shown at its last pose this long for the echo (s)

typedef struct {
    int uid;
    ObjectManagerInterp it;
    const Mesh *mesh;     // the mesh the base below belongs to (an update builds a new one)
    vec3 base_position;   // its transform as built (at the object's own pose)
    vec3 base_rotation;
} OmMotion;

static OmMotion g_motion[OM_INTERP_MAX];
static int g_motion_count;
static Mesh *g_predicted_mesh; // the mesh our own carry moved last frame (put back when it ends)
static int g_predicted_uid;
static vec3 g_predicted_position, g_predicted_rotation; // its transform as built

static double om_now(void) { return glfwGetTime(); }

static Object *om_object(const Client *client, const int uid) {
    const Map *map = client && client->game.map ? client->game.map : NULL;
    return map && uid >= 0 && (size_t) uid < map->object_count ? map->objects[uid] : NULL;
}

static OmMotion *om_motion(const int uid) {
    for (int i = 0; i < g_motion_count; i++) {
        if (g_motion[i].uid == uid) return &g_motion[i];
    }
    return NULL;
}

static void om_motion_drop(OmMotion *m) {
    *m = g_motion[--g_motion_count];
}

// The mesh's built transform, captured when it is new to us
static void om_base(OmMotion *m, Mesh *mesh) {
    if (m->mesh == mesh) return;
    m->mesh = mesh;
    m->base_position = mesh->transform.position;
    m->base_rotation = mesh->transform.rotation;
}

// The mesh of `o` drawn as if the object stood at (feet, yaw) instead of its own pose: moved by the
// difference and turned about the box's centre by the yaw difference
static void om_place(const Object *o, Mesh *mesh, const vec3 base_position, const vec3 base_rotation, const vec3 feet, const float yaw) {
    const float half = o->scale.y * 0.5f;
    const vec3 c_own = {o->position.x, o->position.y + half, o->position.z};
    const vec3 c_shown = {feet.x, feet.y + half, feet.z};
    const float d = yaw - o->rotation.y;
    const float cs = cosf(d), sn = sinf(d);
    const vec3 rel = {base_position.x - c_own.x, base_position.y - c_own.y, base_position.z - c_own.z};
    mesh->transform.position = (vec3) {c_shown.x + rel.x * cs + rel.z * sn, c_shown.y + rel.y, c_shown.z - rel.x * sn + rel.z * cs};
    mesh->transform.rotation = (vec3) {base_rotation.x, base_rotation.y + d, base_rotation.z};
    mesh_update_transform_matrix(mesh);
}

int om_interp_shown(const Client *client, const int uid, vec3 *feet, float *yaw) {
    const Object *o = om_object(client, uid);
    if (!o || !o->active) return 0;
    OmMotion *m = om_motion(uid);
    if (m && m->it.active) {
        vec3 p;
        float y;
        ObjectManagerInterp copy = m->it; // (sampling may end it: the frame does that)
        object_manager_interp_sample(&copy, om_now(), &p, &y);
        *feet = p;
        *yaw = y;
        return 1;
    }
    *feet = o->position;
    *yaw = o->rotation.y;
    return 1;
}

void om_interp_moved(Client *client, const int uid, const vec3 shown, const float shown_yaw) {
    const Object *o = om_object(client, uid);
    if (!o || !o->active || !o->mesh) return;
    int carry_uid;
    ObjectManagerPose pose;
    if (om_build_carry(&carry_uid, &pose) == 1 && carry_uid == uid) return; // ours: the prediction shows it
    OmMotion *m = om_motion(uid);
    if (!m) {
        if (g_motion_count >= OM_INTERP_MAX) return;
        m = &g_motion[g_motion_count++];
        memset(m, 0, sizeof(*m));
        m->uid = uid;
    }
    const float dx = shown.x - o->position.x, dy = shown.y - o->position.y, dz = shown.z - o->position.z;
    if (dx * dx + dy * dy + dz * dz > 200.0f * 200.0f) { // a jump (Undo, a teleport): no glide across the map
        m->it.active = 0;
        m->it.last_update = om_now();
        return;
    }
    object_manager_interp_push(&m->it, shown, shown_yaw, o->position, o->rotation.y, om_now());
}

void om_interp_released(Client *client, const int uid, const ObjectManagerPose *pose) {
    const Object *o = om_object(client, uid);
    if (!o || !o->mesh || !pose) return;
    OmMotion *m = om_motion(uid);
    if (!m) {
        if (g_motion_count >= OM_INTERP_MAX) return;
        m = &g_motion[g_motion_count++];
        memset(m, 0, sizeof(*m));
        m->uid = uid;
    }
    const double now = om_now();
    m->it = (ObjectManagerInterp) {pose->position, pose->position, pose->directed ? o->rotation.y : pose->yaw, pose->directed ? o->rotation.y : pose->yaw,
                                   now, OM_INTERP_HOLD, now, 1};
    if (o->mesh == g_predicted_mesh && g_predicted_uid == uid) { // the predicted mesh: its built transform is the one kept
        m->mesh = g_predicted_mesh;
        m->base_position = g_predicted_position;
        m->base_rotation = g_predicted_rotation;
    }
    g_predicted_mesh = NULL;
}

static void om_predict(Client *client) {
    int uid;
    ObjectManagerPose pose;
    const int kind = om_build_carry(&uid, &pose);
    Mesh *mesh = NULL;
    if (kind == 1) {
        const Object *o = om_object(client, uid);
        mesh = o && o->active ? o->mesh : NULL;
        if (mesh) {
            OmMotion *m = om_motion(uid);
            if (m) om_motion_drop(m); // (an older ease of it)
            if (mesh != g_predicted_mesh) { // a new mesh (the server's echo rebuilt it): its built transform
                g_predicted_mesh = mesh;
                g_predicted_uid = uid;
                g_predicted_position = mesh->transform.position;
                g_predicted_rotation = mesh->transform.rotation;
            }
            om_place(o, mesh, g_predicted_position, g_predicted_rotation, pose.position, pose.directed ? o->rotation.y : pose.yaw);
        }
    } else if (kind == 2) { // a carried player: drawn at the carried feet now (the server holds it there)
        for (size_t i = 0; i < client->game.player_count; i++) {
            Player *p = client->game.players[i];
            if (!p || p->uid != uid || !p->active || p == client->me) continue;
            p->position = pose.position;
            p->last_position = pose.position;
            p->interp_pos_start = p->interp_pos_end = pose.position;
            player_update_meshes(p, 0);
        }
    }
    if (!mesh && g_predicted_mesh) { // the carry ended without a release (refused): the mesh as built again
        const Object *o = om_object(client, g_predicted_uid);
        if (o && o->mesh == g_predicted_mesh && !om_motion(g_predicted_uid)) {
            g_predicted_mesh->transform.position = g_predicted_position;
            g_predicted_mesh->transform.rotation = g_predicted_rotation;
            mesh_update_transform_matrix(g_predicted_mesh);
        }
        g_predicted_mesh = NULL;
    }
}

void om_interp_frame(Client *client) {
    if (!client || !client->game.map) return;
    om_predict(client);
    const double now = om_now();
    for (int i = 0; i < g_motion_count;) {
        OmMotion *m = &g_motion[i];
        Object *o = om_object(client, m->uid);
        Mesh *mesh = o && o->active ? o->mesh : NULL;
        if (!mesh || !m->it.active) {
            om_motion_drop(m);
            continue;
        }
        om_base(m, mesh);
        vec3 feet;
        float yaw;
        const int easing = object_manager_interp_sample(&m->it, now, &feet, &yaw);
        if (!easing) { // there: the mesh as built
            mesh->transform.position = m->base_position;
            mesh->transform.rotation = m->base_rotation;
            mesh_update_transform_matrix(mesh);
            om_motion_drop(m);
            continue;
        }
        om_place(o, mesh, m->base_position, m->base_rotation, feet, yaw);
        i++;
    }
}

void om_interp_reset(void) {
    g_motion_count = 0;
    g_predicted_mesh = NULL;
}
