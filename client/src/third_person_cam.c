// KrunkNative third-person camera, the pure half (client/include/third_person.h): the camera spot
// and the aim correction. Only shared code (world_solid_fraction, line_in_rect), so
// client/tests/third_person_test.c links it without GL.
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
    // v7: RENDER.camera.position = (thrdPX, thrdPY, thrdPZ) inside the pitch object: x right,
    // y up, z back (= -forward)
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

vec2 third_person_aim_look(const Game *game, const Player *me, const vec3 eye, const vec3 camera, const vec2 view) {
    vec3 f;
    look_frame(view.x, view.y, &f, NULL, NULL);
    const vec3 seg = {f.x * TP_AIM_RANGE, f.y * TP_AIM_RANGE, f.z * TP_AIM_RANGE};

    // the crosshair ray: the first solid box / triangle / terrain, then any other living player's box
    float t = game ? world_solid_fraction(game, camera, seg) : 1.0f;
    vec3 seg_nz = seg; // line_in_rect divides by each component
    if (seg_nz.x == 0.0f) seg_nz.x = 1e-6f;
    if (seg_nz.y == 0.0f) seg_nz.y = 1e-6f;
    if (seg_nz.z == 0.0f) seg_nz.z = 1e-6f;
    for (size_t i = 0; game && i < game->player_count; i++) {
        const Player *p = game->players[i];
        if (!p || p == me || !p->active) continue;
        const float half = p->scale > 0.0f ? p->scale : game_constants.player_scale;
        const float height = p->height > 0.0f ? p->height : game_constants.player_height;
        const float hit = line_in_rect(camera, seg_nz, p->position, (vec3) {half * 2.0f, height, half * 2.0f});
        if (hit > 0.0f && hit < t) t = hit;
    }
    const vec3 aim = {camera.x + seg.x * t, camera.y + seg.y * t, camera.z + seg.z * t};

    // Aim the eye at it, unless the point is (nearly) beside or behind the eye: then the
    // crosshair is on something next to the player and the plain look is the better guess.
    const vec3 d = {aim.x - eye.x, aim.y - eye.y, aim.z - eye.z};
    const float dist = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
    if (dist < 2.0f || d.x * f.x + d.y * f.y + d.z * f.z < 1.0f) return view;
    vec2 look;
    look.x = asinf(fmaxf(-1.0f, fminf(1.0f, d.y / dist)));
    look.y = atan2f(-d.x, -d.z);
    // keep the yaw on the same turn as the view's (the input yaw is wrapped by the caller)
    look.y = view.y + atan2f(sinf(look.y - view.y), cosf(look.y - view.y));
    return look;
}
