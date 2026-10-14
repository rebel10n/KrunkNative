// KrunkNative third-person camera, the pure half (client/include/third_person.h): the camera spot.
// Only shared code (world_solid_fraction), so client/tests/third_person_test.c links it without GL.
#include <third_person.h>
#include <world.h>
#include <math.h>

// The look frame of (pitch, yaw), three.js conventions (camera.rotation.x = pitch, .y = yaw,
// -Z forward): forward f = (-sin y cos p, sin p, -cos y cos p), right r = (cos y, 0, -sin y),
// up u = r x f = (sin y sin p, cos p, cos y sin p).
static void look_frame(const float pitch, const float yaw, vec3 *f, vec3 *r, vec3 *u) {
    const float sp = sinf(pitch), cp = cosf(pitch), sy = sinf(yaw), cy = cosf(yaw);
    if (f) *f = (vec3) {-sy * cp, sp, -cy * cp};
    if (r) *r = (vec3) {cy, 0.0f, -sy};
    if (u) *u = (vec3) {sy * sp, cp, cy * sp};
}

float third_person_camera_spot(const Game *game, const vec3 eye, const float pitch, const float yaw, const vec3 offset, vec3 *out) {
    vec3 f, r, u;
    look_frame(pitch, yaw, &f, &r, &u);
    // v10 gameLoop: RENDER.camera.position.set(5, 2, 14) inside CONTROLS.pchObjc (the pitch
    // object, a child of the yaw object at the eye): x right, y up, z back (= -forward). The
    // offset turns with the look, so a pitch swings the camera over / under the eye in the
    // vertical plane of the yaw; it never moves it sideways (v10.js 202354-202357, 221457).
    const vec3 seg = {
        r.x * offset.x + u.x * offset.y - f.x * offset.z,
        r.y * offset.x + u.y * offset.y - f.y * offset.z,
        r.z * offset.x + u.z * offset.y - f.z * offset.z,
    };
    const float len = sqrtf(seg.x * seg.x + seg.y * seg.y + seg.z * seg.z);
    float keep = len;
    if (len > 0.0f && game) {
        const float t = world_solid_fraction(game, eye, seg);
        if (t < 1.0f) keep = fmaxf(0.0f, t * len - TP_WALL_GAP);
    }
    const float k = len > 0.0f ? keep / len : 0.0f;
    if (out) *out = (vec3) {eye.x + seg.x * k, eye.y + seg.y * k, eye.z + seg.z * k};
    return keep;
}
