// Krunker v10.0.3's class camera, the pure half (client/include/class_camera.h): what updateCamMouse
// makes of a class, the camera pose of the game loop, the free-mouse aim and the controls' locks.
// Only math (no GL, no GLFW): client/tests/class_camera_test.c links it with shared code.
#include <class_camera.h>
#include <game_config.h>
#include <math.h>

ClassCamera class_camera_make(const ClassOverrides *o, const int third_person, const int force_free) {
    if (!o) o = &g_default_class_overrides;
    ClassCamera c = {0};
    // camType: isNormal / isTop / isSideX / isSideZ / isOrto = camType == 0..4 (deob-10.0.3.js:257869-257873);
    // the select keeps it in range (gc_select), anything else reads as FPS
    c.type = o->cam_type >= CLASS_CAMERA_FPS && o->cam_type <= CLASS_CAMERA_ORTHO ? o->cam_type : CLASS_CAMERA_FPS;
    c.offset = (vec3) {o->cam_offset[0], o->cam_offset[1], o->cam_offset[2]};
    // camOffset = camOffX || camOffY || camOffZ (deob:257865)
    c.has_offset = o->cam_offset[0] != 0.0f || o->cam_offset[1] != 0.0f || o->cam_offset[2] != 0.0f;
    c.rotation = o->cam_rotation != 0; // camRot (deob:257866)
    // UsePointerLock = (camRot || !camOffset || fPLock) && !isMobileControls && !forceFreeCam; freeMouse =
    // !UsePointerLock && !isMobileControls (deob:257867-257868; the phone controls: class_camera_current)
    const int pointer_lock = (c.rotation || !c.has_offset || o->fp_lock) && !force_free;
    c.free_mouse = !pointer_lock;
    c.ortho_f = o->ortho_f > 0.0f ? o->ortho_f : 50.0f; // orthoF 1..1000, def 50 (deob:20200-20209)
    c.face_player = o->lock_cam_pitch;
    c.lock_y_rot = o->lock_y_rot;
    c.y_limit = o->y_cam_limit;
    c.aim_fov = c.type == CLASS_CAMERA_FPS || o->shoot_to_cam; // RENDER.aimFov (deob:257875)
    c.third_person = third_person != 0;
    return c;
}

int class_camera_active(const ClassCamera *c) {
    return c && (c->type != CLASS_CAMERA_FPS || c->has_offset || c->third_person || c->free_mouse || !c->rotation ||
                 c->face_player != 0.0f || c->lock_y_rot != 0.0f || c->y_limit != 0.0f || !c->aim_fov);
}

int class_camera_third_person(const ClassCamera *c) {
    return c && (c->third_person || c->has_offset || c->type != CLASS_CAMERA_FPS);
}

// The rig's frame of (pitch, yaw), three.js conventions (object.rotation.y = yaw holds pchObjc with
// rotation.x = pitch; -Z forward): forward f = (-sin y cos p, sin p, -cos y cos p), right r = (cos y, 0,
// -sin y), up u = (sin y sin p, cos p, cos y sin p). A local point (x, y, z) is x r + y u - z f.
static vec3 rig_point(const float pitch, const float yaw, const vec3 local) {
    const float sp = sinf(pitch), cp = cosf(pitch), sy = sinf(yaw), cy = cosf(yaw);
    const vec3 f = {-sy * cp, sp, -cy * cp}, r = {cy, 0.0f, -sy}, u = {sy * sp, cp, cy * sp};
    return (vec3) {r.x * local.x + u.x * local.y - f.x * local.z, r.y * local.x + u.y * local.y - f.y * local.z,
                   r.z * local.x + u.z * local.y - f.z * local.z};
}

// Object3D.lookAt for a camera (three's Matrix4.lookAt with up (0, 1, 0)): the world rotation that
// looks from `from` at `to`, as KrunkNative's pitch / yaw (no roll with that up). Straight above or
// below the target three nudges its z axis by 0.0001 (`_z.z += 0.0001`), so the camera looks down
// (or up) with world -Z at the top of the screen.
static void look_at(const vec3 from, const vec3 to, float *pitch, float *yaw) {
    double dx = (double) to.x - from.x, dy = (double) to.y - from.y, dz = (double) to.z - from.z;
    double len = sqrt(dx * dx + dy * dy + dz * dz);
    if (len <= 0.0) { // _z = (0, 0, 1): along -Z
        *pitch = *yaw = 0.0f;
        return;
    }
    dx /= len, dy /= len, dz /= len;
    if (dx == 0.0 && dz == 0.0) { // up x _z is 0: _z.z += 0.0001 (forward = -_z)
        dz -= 0.0001;
        len = sqrt(dx * dx + dy * dy + dz * dz);
        dx /= len, dy /= len, dz /= len;
    }
    *pitch = (float) asin(fmax(-1.0, fmin(1.0, dy)));
    *yaw = (float) atan2(-dx, -dz);
}

int class_camera_pose(const ClassCamera *c, const ClassCameraRig *r, Camera *camera) {
    // camera.position.set(0, 0, 0) / thirdPerson's (5, 2, 14) / the Prop's (0, 0, d), then += camOffX / Y / Z
    // while camOffset (deob-10.0.3.js:260589-260645); iîÌìïii = camOffset || thirdPerson || the Prop
    vec3 local = r->local;
    if (c->has_offset) local = (vec3) {local.x + c->offset.x, local.y + c->offset.y, local.z + c->offset.z};
    const int offset = c->has_offset || r->local_offset;
    // camera.lookAt(eye) when iîÌìïii (deob:260647-260649), but rotateCam after it (deob:260819-260821 on
    // input frames, 260858-260860 otherwise) sets camera.rotation back to the shake / recoil unless
    // freeMouse: a mouse-rotated offset camera looks parallel to the look, a free-mouse one at the eye
    const int look = offset && c->free_mouse;
    if (c->type == CLASS_CAMERA_ORTHO) {
        // the OrthographicCamera is never added to pchObjc (only the perspective one is, deob:235422):
        // camera.position is world coordinates, its rotation the world rotation (deob:230983, 231404-231410)
        camera->position = local;
        camera->ortho = c->ortho_f * 0.5f; // top = frustumSize / 2, left = -frustumSize * aspect / 2 (deob:231454-231457)
        camera->near = CLASS_CAMERA_ORTHO_NEAR;
        camera->far = CLASS_CAMERA_ORTHO_FAR;
    } else {
        const vec3 d = rig_point(r->pitch, r->yaw, local);
        camera->position = (vec3) {r->eye.x + d.x, r->eye.y + d.y, r->eye.z + d.z};
        camera->ortho = 0.0f;
    }
    if (look) {
        look_at(camera->position, r->eye, &camera->rotation.x, &camera->rotation.y);
        camera->rotation.z = 0.0f; // three's lookAt undoes the parents' rotation, the wall lean's roll too
    } else if (c->type == CLASS_CAMERA_ORTHO) {
        // rotateCam's camera.rotation in world space (none with freeMouse: the camera keeps its rotation, 0)
        camera->rotation.x = c->free_mouse ? 0.0f : r->feel_pitch;
        camera->rotation.y = c->free_mouse ? 0.0f : r->feel_yaw;
        camera->rotation.z = 0.0f;
    } else {
        // the rig's yaw / pitch, then rotateCam's camera.rotation inside it (KrunkNative sums the Euler
        // angles as camera_feel_apply does); freeMouse skips rotateCam
        camera->rotation.x = r->pitch + (c->free_mouse ? 0.0f : r->feel_pitch);
        camera->rotation.y = r->yaw + (c->free_mouse ? 0.0f : r->feel_yaw);
        camera->rotation.z = r->roll;
    }
    return look;
}

// The camera's world point at NDC (x, y, 0.5): Vector3.unproject (deob:260652-260653), with KrunkNative's
// projection (camera.c) and world rotation (Rz Ry Rx, camera_update_world_inverse_matrix).
static vec3 unproject_half(const Camera *camera, const float aspect, const float ndc_x, const float ndc_y) {
    double vx, vy, vz;
    const double n = camera->near, f = camera->far;
    if (camera->ortho > 0.0f) {
        const double hh = camera->ortho, hw = hh * aspect;
        vz = -(0.5 * (f - n) + (f + n)) / 2.0; // -2 z / (f - n) - (f + n) / (f - n) = 0.5
        vx = ndc_x * hw;
        vy = ndc_y * hh;
    } else {
        const double zoom = camera->zoom > 0.0f ? camera->zoom : 1.0f;
        const double hh = tan(camera->fov / zoom / 2.0) * n, hw = hh * aspect;
        const double a = -(f + n) / (f - n), b = n * (a - 1.0);
        vz = -b / (a + 0.5); // (a z + b) / -z = 0.5
        vx = ndc_x * hw * -vz / n;
        vy = ndc_y * hh * -vz / n;
    }
    // Rx(pitch), then Ry(yaw), then Rz(roll)
    const double sp = sin(camera->rotation.x), cp = cos(camera->rotation.x);
    const double sy = sin(camera->rotation.y), cy = cos(camera->rotation.y);
    const double sr = sin(camera->rotation.z), cr = cos(camera->rotation.z);
    const double x1 = vx, y1 = cp * vy - sp * vz, z1 = sp * vy + cp * vz;
    const double x2 = cy * x1 + sy * z1, y2 = y1, z2 = -sy * x1 + cy * z1;
    const double x3 = cr * x2 - sr * y2, y3 = sr * x2 + cr * y2, z3 = z2;
    return (vec3) {(float) (camera->position.x + x3), (float) (camera->position.y + y3), (float) (camera->position.z + z3)};
}

// Ray.intersectPlane with a plane through the origin (Plane's constant stays 0): null (target kept)
// when the ray runs parallel off the plane or the plane lies behind it.
static int ray_plane(const double o[3], const double d[3], const int axis, double hit[3]) {
    const double denom = d[axis];
    double t;
    if (denom == 0.0) {
        if (o[axis] != 0.0) return 0;
        t = 0.0;
    } else {
        t = -o[axis] / denom;
        if (t < 0.0) return 0;
    }
    for (int i = 0; i < 3; i++) hit[i] = o[i] + d[i] * t;
    return 1;
}

int class_camera_free_aim(const ClassCamera *c, const Camera *camera, const float aspect, const float ndc_x, const float ndc_y,
                          const vec3 player, const float player_height, vec3 *hit, float *yaw, float *pitch) {
    if (!c || (c->type != CLASS_CAMERA_THIRD && c->type != CLASS_CAMERA_SIDE_X && c->type != CLASS_CAMERA_SIDE_Z)) return 0;
    // origin = the camera's world position, direction = unproject(mouse, 0.5) - origin, normalized
    const vec3 p = unproject_half(camera, aspect, ndc_x, ndc_y);
    double o[3] = {camera->position.x, camera->position.y, camera->position.z};
    double d[3] = {(double) p.x - o[0], (double) p.y - o[1], (double) p.z - o[2]};
    const double len = sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (len > 0.0) d[0] /= len, d[1] /= len, d[2] /= len;
    const double head = (double) player.y + player_height; // y + height (not the eye: no cameraHeight)
    int axis;
    if (c->type == CLASS_CAMERA_THIRD) { // normal (0, 1, 0); origin.y -= y + height; direction.y <= -0.0001
        axis = 1;
        o[1] -= head;
        d[1] = fmin(d[1], -0.0001);
    } else if (c->type == CLASS_CAMERA_SIDE_X) { // normal (0, 0, 1); origin.z -= z; direction.z <= -0.0001
        axis = 2;
        o[2] -= player.z;
        d[2] = fmin(d[2], -0.0001);
    } else { // Platformer Z: normal (1, 0, 0); origin.x -= x; direction.x >= 0.0001
        axis = 0;
        o[0] -= player.x;
        d[0] = fmax(d[0], 0.0001);
    }
    double h[3];
    if (ray_plane(o, d, axis, h)) *hit = (vec3) {(float) h[0], (float) h[1], (float) h[2]};
    if (c->type == CLASS_CAMERA_THIRD) {
        // yaw = -atan2(hit.z - z, hit.x - x) - PI / 2, yDr = 0 (deob:260670-260672)
        *yaw = (float) (-atan2((double) hit->z - player.z, (double) hit->x - player.x) - M_PI / 2.0);
        *pitch = 0.0f;
        return 1;
    }
    // atan2(hit.y - (y + height), along the plane); the cursor in front (|angle| < PI / 2) faces -X / +Z
    // (Platformer X yaw -PI / 2, Z PI), behind +X / -Z, the pitch mirrored onto the front (deob:260673-260685)
    const int side_x = c->type == CLASS_CAMERA_SIDE_X;
    double angle = atan2((double) hit->y - head, side_x ? (double) hit->x - player.x : (double) hit->z - player.z);
    const int front = fabs(angle) < M_PI / 2.0;
    *yaw = side_x ? (float) ((front ? -1.0 : 1.0) * M_PI / 2.0) : (float) ((front ? 1.0 : 0.0) * M_PI);
    if (!front) angle = -angle + M_PI * (angle > 0.0 ? 1.0 : angle < 0.0 ? -1.0 : 0.0);
    *pitch = (float) angle;
    return 1;
}

float class_camera_face_player(const ClassCamera *c, float *chase, const float body_yaw, const float frame_ms) {
    if (!c || c->face_player == 0.0f) return *chase;
    if (isnan(*chase)) *chase = body_yaw; // camChase == null: xDire
    // d = (xDire - camChase) % 2PI; camChase += (d * 2 % 2PI - d) * lockCamP * (delta / 100): the wrapped
    // difference (JS % keeps the dividend's sign, as fmod does)
    const double full = M_PI * 2.0;
    const double d = fmod((double) body_yaw - *chase, full);
    const double step = (fmod(d * 2.0, full) - d) * ((double) c->face_player * (frame_ms / 100.0));
    *chase = (float) (*chase + step);
    return *chase;
}

float class_camera_lock_pitch(const ClassCamera *c, float pitch) {
    if (!c) return pitch;
    if (c->lock_y_rot != 0.0f) pitch = -(float) M_PI / 2.0f * c->lock_y_rot; // lckCmYR
    if (c->y_limit != 0.0f) { // yCmLim: at most -PI / 2 + yCmLim * PI
        const float limit = c->y_limit * (float) M_PI;
        if (pitch + (float) M_PI / 2.0f >= limit) pitch = -(float) M_PI / 2.0f + limit;
    }
    return pitch;
}
