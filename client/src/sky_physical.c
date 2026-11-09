// Box Office physical sky (rtSky; client/include/sky.h, assets/shaders/rt_sky.glsl, doc/raytracing.md "Physical
// sky"). With rtMode > 0, the tracer supported and the rtSky switch on, the sky pass (rt_sky.frag) replaces
// Krunker's dome / clear colour, the ray misses of the reflection and GI passes see the same sky, the PMREM
// table behind the sky ambient and the GLB env reflections is painted from it (env_map.c), and krunker_fog gets
// the aerial-perspective tint. A textured sky dome (skyDomeTex: an authored picture) keeps Krunker's dome.
//
// This file owns the model's per-frame constants and mirrors rt_sky.glsl's psky_authored / psky_scatter /
// psky_base in C: the day factor (psky_avg) and the PMREM rows need them on the CPU.
#include <gl.h>
#include <client.h>
#include <sky.h>
#include <env_map.h>
#include <rt.h>
#include <render_settings.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PSKY_CLOUDS 0.6f        // the cloud layer's opacity (rt_sky.glsl psky_clouds)
#define PSKY_MIN_DISK_DEG 0.4f  // the disk's smallest angular radius; it follows rtSunRadius above that

typedef struct {
    int active;
    float sun[3], sun_color[3];
    float disk[3];              // towards the visible sun (rt_sky.glsl psky_disk): sun, or the authored sun seen from the camera
    int authored;               // the map draws its own sun (sky_physical_set_map)
    float authored_pos[3];      // ... at this point (the centre of its light cones)
    float stops[3][3];          // top, mid, bottom
    float avg, day, disk_cos, time;
    char key[128];              // the env table's cache key (env_map.c)
} PhysicalSky;

static PhysicalSky g_psky;
static int g_psky_logs; // the log line is printed for the first few distinct skies (a day cycle would spam it)
static unsigned int g_psky_program, g_psky_vao;
static int g_psky_failed;

static float psky_clamp(const float v, const float lo, const float hi) { return v < lo ? lo : v > hi ? hi : v; }
static float psky_mix(const float a, const float b, const float t) { return a + (b - a) * t; }
static float psky_smoothstep(const float e0, const float e1, const float x) {
    const float t = psky_clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
static float psky_lum(const float c[3]) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; }
static float psky_airmass(const float elevation) { return 1.0f / (fmaxf(elevation, 0.0f) + 0.06f); }

static void psky_authored(const float d[3], float out[3]) {
    const float v = 0.5f + asinf(psky_clamp(d[1], -1.0f, 1.0f)) * 0.3183098862f;
    for (int i = 0; i < 3; i++) {
        out[i] = v < 0.5f ? psky_mix(g_psky.stops[2][i], g_psky.stops[1][i], v / 0.5f)
                          : psky_mix(g_psky.stops[1][i], g_psky.stops[0][i], (v - 0.5f) / 0.5f);
    }
}

static void psky_scatter(const float d[3], float out[3]) {
    const float rayleigh[3] = {0.17f * 0.35f, 0.40f * 0.35f, 1.0f * 0.35f};
    const float mie = 0.008f, g = 0.76f;
    const float mu = fmaxf(d[1], 0.0f);
    const float c = d[0] * g_psky.sun[0] + d[1] * g_psky.sun[1] + d[2] * g_psky.sun[2];
    const float pr = 0.0596831f * (1.0f + c * c);
    const float pm = 0.0795775f * (1.0f - g * g) / powf(1.0f + g * g - 2.0f * g * c, 1.5f);
    for (int i = 0; i < 3; i++) {
        const float beta = rayleigh[i] + mie;
        const float sun_t = expf(-beta * psky_airmass(g_psky.sun[1]));
        out[i] = sun_t * (rayleigh[i] * pr + mie * pm) / beta * (1.0f - expf(-beta * psky_airmass(mu)));
        if (d[1] < 0.0f) out[i] *= 1.0f - 0.7f * psky_smoothstep(0.0f, 0.4f, -d[1]);
    }
}

static float psky_glow(const float d[3], const float k) { // rt_sky.glsl psky_glow
    const int cine = rt_cinematic();
    const float g = 0.95f;
    const float c = d[0] * g_psky.sun[0] + d[1] * g_psky.sun[1] + d[2] * g_psky.sun[2];
    const float hg = 0.0795775f * (1.0f - g * g) / powf(fmaxf(1.0f + g * g - 2.0f * g * c, 1e-6f), 1.5f) * 12.566371f;
    const float x = k * (cine ? 2.5f : 1.0f) * fmaxf(hg - 1.0f, 0.0f);
    return 1.0f + (cine ? 0.6f : 0.35f) * x / (1.0f + x);
}

// How much of the authored hue a day sky keeps (rt_sky.glsl psky_keep): the base share, raised towards 0.85 where
// the authored colour is saturated. A map that authored a coloured sky (Sandstorm's peach horizon under a blue
// zenith) keeps its mood; a near-grey authored sky (Burg's clear colour) takes the atmosphere's hue.
static float psky_keep(const float a[3], const int cine) {
    const float mx = fmaxf(a[0], fmaxf(a[1], a[2])), mn = fminf(a[0], fminf(a[1], a[2]));
    const float sat = mx > 1e-4f ? (mx - mn) / mx : 0.0f;
    const float base = cine ? 0.15f : 0.4f;
    return psky_mix(base, 0.85f, psky_smoothstep(0.08f, 0.25f, sat));
}

void sky_physical_base(const float d[3], float out[3]) {
    float a[3], s[3];
    psky_authored(d, a);
    psky_scatter(d, s);
    const float la = psky_lum(a), ls = psky_lum(s);
    const float glow = psky_glow(d, psky_mix(0.001f, 0.004f, g_psky.day));
    const int cine = rt_cinematic();
    const float keep = psky_keep(a, cine);
    for (int i = 0; i < 3; i++) {
        const float chroma = psky_mix(ls > 1e-4f ? s[i] / ls : 1.0f, 1.0f, 0.75f * expf(-fmaxf(d[1], 0.0f) * 6.0f)); // horizon whitening
        out[i] = psky_mix(chroma * la, a[i], keep); // the day colour, finished below
    }
    if (cine) { // Cinematic: a more saturated atmosphere (rt_sky.glsl psky_base)
        const float l = psky_lum(out);
        for (int i = 0; i < 3; i++) out[i] = l + (out[i] - l) * 1.2f;
    }
    for (int i = 0; i < 3; i++) out[i] = psky_mix(a[i], fmaxf(out[i], 0.0f), g_psky.day) * glow;
}

void sky_physical_authored(const float d[3], float out[3]) {
    psky_authored(d, out);
}

int sky_physical_active(void) {
    return g_psky.active;
}

const char *sky_physical_key(void) {
    return g_psky.active ? g_psky.key : "";
}

static int psky_compare_float(const void *a, const void *b) {
    const float x = *(const float *) a, y = *(const float *) b;
    return x < y ? -1 : x > y;
}

// Krunker maps draw their own sun: LIGHT_CONE prefabs parked far above and outside the playable map (Burg: three
// stacked at y 1215..1257, 1 400 units out, the glow players see in its sky; Oasis, Evacuation, Lumber, Habitat,
// Facility, Lagoon, Tortuga and Citadel likewise). Krunker's frame shows that glow and no other sun, so the physical
// sky's disk and aureole go there (psky_disk, found per frame from the camera: the cones sit at a finite distance),
// else the disk hangs next to the authored sun, 15 degrees off on Burg, and the player sees two suns. Candidates are
// the light cones 200 units above the 99th percentile of the other objects' heights; the one nearest the map's light
// direction as seen from the map's centre wins when it is within 40 degrees of it (Kanji's lanterns at 56 degrees
// and more and Erupt's volcano glow at 55 are not suns), and the cones within 5 degrees of it (a stack) are
// averaged. The lighting stays on light_direction: the shadows, the shafts' march, the day factor, the sun colour
// and this file's C mirror of the sky (the PMREM table), whose few-degree aureole hardly registers in those rows.
void sky_physical_set_map(const Map *map) {
    g_psky.authored = 0;
    if (!map || !map->objects || map->object_count == 0) return;
    float *heights = malloc(map->object_count * sizeof(float));
    if (!heights) return;
    size_t others = 0;
    float lo[3] = {INFINITY, INFINITY, INFINITY}, hi[3] = {-INFINITY, -INFINITY, -INFINITY};
    for (size_t i = 0; i < map->object_count; i++) {
        const Object *o = map->objects[i];
        if (!o || o->prefab == PREFAB_LIGHT_CONE) continue;
        heights[others++] = o->position.y;
        const float p[3] = {o->position.x, o->position.y, o->position.z};
        for (int k = 0; k < 3; k++) { lo[k] = fminf(lo[k], p[k]); hi[k] = fmaxf(hi[k], p[k]); }
    }
    if (others == 0) { free(heights); return; }
    qsort(heights, others, sizeof(float), psky_compare_float);
    const float top = heights[(size_t) ((float) (others - 1) * 0.99f)] + 200.0f;
    free(heights);
    const float centre[3] = {(lo[0] + hi[0]) * 0.5f, (lo[1] + hi[1]) * 0.5f, (lo[2] + hi[2]) * 0.5f};
    const vec3 l = g_render_lighting.light_direction;
    const float light_length = sqrtf(l.x * l.x + l.y * l.y + l.z * l.z);
    if (light_length <= 0.0f) return;
    const float light[3] = {l.x / light_length, l.y / light_length, l.z / light_length};
    float best[3] = {0.0f, 0.0f, 0.0f}, best_dot = cosf(40.0f * (float) M_PI / 180.0f);
    float sum[3] = {0.0f, 0.0f, 0.0f};
    int found = 0, count = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (size_t i = 0; i < map->object_count; i++) {
            const Object *o = map->objects[i];
            if (!o || o->prefab != PREFAB_LIGHT_CONE || o->position.y <= top) continue;
            const float p[3] = {o->position.x, o->position.y + o->scale.y * 0.5f, o->position.z}; // the cone's centre
            float d[3] = {p[0] - centre[0], p[1] - centre[1], p[2] - centre[2]};
            const float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (len <= 0.0f) continue;
            for (int k = 0; k < 3; k++) d[k] /= len;
            if (pass == 0) {
                const float dot = d[0] * light[0] + d[1] * light[1] + d[2] * light[2];
                if (dot > best_dot) { best_dot = dot; memcpy(best, d, sizeof(best)); found = 1; }
            } else if (d[0] * best[0] + d[1] * best[1] + d[2] * best[2] >= cosf(5.0f * (float) M_PI / 180.0f)) {
                for (int k = 0; k < 3; k++) sum[k] += p[k];
                count++;
            }
        }
        if (!found) return;
    }
    if (count == 0) return;
    for (int k = 0; k < 3; k++) g_psky.authored_pos[k] = sum[k] / (float) count;
    g_psky.authored = 1;
    fprintf(stderr, "[rt] physical sky: the map draws its own sun (%d light cone%s at %.0f, %.0f, %.0f; %.0f degrees from the light): the disk follows it\n",
            count, count == 1 ? "" : "s", g_psky.authored_pos[0], g_psky.authored_pos[1], g_psky.authored_pos[2],
            acosf(fminf(best_dot, 1.0f)) * 180.0f / (float) M_PI);
}

// psky_disk for this frame: the authored sun seen from `eye`, else the light direction
static void psky_update_disk(const vec3 eye) {
    memcpy(g_psky.disk, g_psky.sun, sizeof(g_psky.disk));
    if (!g_psky.authored) return;
    const float d[3] = {g_psky.authored_pos[0] - eye.x, g_psky.authored_pos[1] - eye.y, g_psky.authored_pos[2] - eye.z};
    const float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (len < 1.0f) return;
    for (int k = 0; k < 3; k++) g_psky.disk[k] = d[k] / len;
}

void sky_physical_frame(const float delta_ms) {
    const ClientRenderSettings *s = client_render_settings();
    vec3 stops[3];
    const int dome = sky_dome_gradient(stops); // 1 untextured dome, 0 none (the clear colour), -1 textured
    const int was = g_psky.active;
    g_psky.active = s->rt_mode > 0 && s->rt_sky && dome >= 0 && rt_supported();
    if (!g_psky.active) {
        if (was) env_map_refresh(); // back to Krunker's getEnvMap gradient
        return;
    }

    if (s->texture_anim) g_psky.time += delta_ms * 0.001f; // drifts like Krunker's sky scroll: textureAnim
    const vec3 *src = stops;
    vec3 flat[3];
    if (!dome) {
        const vec4 c = g_render_lighting.sky_color;
        flat[0] = flat[1] = flat[2] = (vec3) {c.x, c.y, c.z};
        src = flat;
    }
    for (int i = 0; i < 3; i++) { g_psky.stops[i][0] = src[i].x; g_psky.stops[i][1] = src[i].y; g_psky.stops[i][2] = src[i].z; }

    vec3 sun = g_render_lighting.light_direction; // towards the sun (basic.frag's light_direction, rt_pass.c)
    float length = sqrtf(sun.x * sun.x + sun.y * sun.y + sun.z * sun.z);
    if (length <= 0.0f) { sun = (vec3) {0.3f, 0.8f, 0.5f}; length = sqrtf(0.98f); }
    g_psky.sun[0] = sun.x / length; g_psky.sun[1] = sun.y / length; g_psky.sun[2] = sun.z / length;
    memcpy(g_psky.disk, g_psky.sun, sizeof(g_psky.disk)); // until sky_physical_render sees the camera

    // the authored gradient's mean over the upper hemisphere (uniform in solid angle: y uniform in [0, 1]) sets
    // the day factor; the stops / sun decide it, so it is recomputed only when the key changes
    char key[128];
    snprintf(key, sizeof(key), "P%d|%.3f,%.3f,%.3f|%.3f,%.3f,%.3f|%.3f,%.3f,%.3f|%.3f,%.3f,%.3f", rt_cinematic(), // the Cinematic look changes the PMREM rows too
             g_psky.sun[0], g_psky.sun[1], g_psky.sun[2], g_psky.stops[0][0], g_psky.stops[0][1], g_psky.stops[0][2],
             g_psky.stops[1][0], g_psky.stops[1][1], g_psky.stops[1][2], g_psky.stops[2][0], g_psky.stops[2][1], g_psky.stops[2][2]);
    if (strcmp(key, g_psky.key)) {
        snprintf(g_psky.key, sizeof(g_psky.key), "%s", key);
        double authored = 0.0;
        int n = 0;
        for (int yi = 0; yi < 16; yi++) {
            const float y = ((float) yi + 0.5f) / 16.0f, r = sqrtf(1.0f - y * y);
            for (int ai = 0; ai < 32; ai++) {
                const float a = (float) ai * (float) (2.0 * M_PI / 32.0);
                const float d[3] = {r * cosf(a), y, r * sinf(a)};
                float c[3];
                psky_authored(d, c);
                authored += psky_lum(c);
                n++;
            }
        }
        g_psky.avg = (float) (authored / n);
        g_psky.day = psky_smoothstep(0.12f, 0.35f, g_psky.avg) * psky_smoothstep(-0.05f, 0.2f, g_psky.sun[1]);

        // the sun's colour after the atmosphere (rt_sky.glsl psky_scatter's sun_t), times the map's light, luminance 1
        const float rayleigh[3] = {0.17f * 0.35f, 0.40f * 0.35f, 1.0f * 0.35f};
        const vec4 light = g_render_lighting.light_color;
        const float lc[3] = {light.x, light.y, light.z};
        float sc[3];
        for (int i = 0; i < 3; i++) sc[i] = expf(-(rayleigh[i] + 0.008f) * psky_airmass(g_psky.sun[1])) * (lc[i] > 0.0f ? lc[i] : 1.0f);
        const float l = psky_lum(sc);
        for (int i = 0; i < 3; i++) g_psky.sun_color[i] = l > 1e-4f ? sc[i] / l : 1.0f;
        if (g_psky_logs < 8) {
            fprintf(stderr, "[rt] physical sky: %s, sun elevation %.2f, day %.2f, authored mean %.3f\n",
                    dome ? "sky dome" : "clear colour", g_psky.sun[1], g_psky.day, g_psky.avg);
            g_psky_logs++;
        }
    }
    const float radius = fmaxf(s->rt_sun_radius, PSKY_MIN_DISK_DEG) * (float) M_PI / 180.0f;
    g_psky.disk_cos = cosf(radius);
    env_map_refresh(); // the PMREM table follows this sky (a string compare when nothing changed)
}

// How warm the authored horizon is, 0..1 (its red over blue): Sandstorm's peach 1, Burg's grey ~0. The Cinematic
// grade (rt_grade.c) drops its cool shadow tint on warm maps so their shade does not go blue.
float sky_physical_warmth(void) {
    if (!g_psky.active) return 0.0f;
    return psky_smoothstep(0.05f, 0.2f, g_psky.stops[1][0] - g_psky.stops[1][2]);
}

float sky_physical_day(void) {
    if (g_psky.active) return g_psky.day;
    const vec4 c = g_render_lighting.sky_color;
    const float sky[3] = {c.x, c.y, c.z};
    vec3 sun = g_render_lighting.light_direction;
    const float length = sqrtf(sun.x * sun.x + sun.y * sun.y + sun.z * sun.z);
    const float elevation = length > 0.0f ? sun.y / length : 0.5f;
    return psky_smoothstep(0.12f, 0.35f, psky_lum(sky)) * psky_smoothstep(-0.05f, 0.2f, elevation);
}

int sky_physical_sun_color(float out[3]) {
    if (!g_psky.active) return 0;
    for (int i = 0; i < 3; i++) out[i] = g_psky.sun_color[i];
    return 1;
}

static GlUniform u_on = GL_UNIFORM_INIT("psky_on");
static GlUniform u_sun = GL_UNIFORM_INIT("psky_sun");
static GlUniform u_sun_color = GL_UNIFORM_INIT("psky_sun_color");
static GlUniform u_disk = GL_UNIFORM_INIT("psky_disk");
static GlUniform u_top = GL_UNIFORM_INIT("psky_stop_top");
static GlUniform u_mid = GL_UNIFORM_INIT("psky_stop_mid");
static GlUniform u_bottom = GL_UNIFORM_INIT("psky_stop_bottom");
static GlUniform u_avg = GL_UNIFORM_INIT("psky_avg");
static GlUniform u_day = GL_UNIFORM_INIT("psky_day");
static GlUniform u_disk_cos = GL_UNIFORM_INIT("psky_disk_cos");
static GlUniform u_time = GL_UNIFORM_INIT("psky_time");
static GlUniform u_clouds = GL_UNIFORM_INIT("psky_clouds");
static GlUniform u_cine = GL_UNIFORM_INIT("psky_cine"); // Cinematic sky look (rt_sky.glsl psky_cine)

void sky_physical_uniforms(const unsigned int program, const int on) {
    const int enabled = on && g_psky.active;
    gl_uniform_1i(&u_on, program, enabled);
    if (!enabled) return;
    gl_uniform_3f(&u_sun, program, g_psky.sun[0], g_psky.sun[1], g_psky.sun[2]);
    gl_uniform_3f(&u_sun_color, program, g_psky.sun_color[0], g_psky.sun_color[1], g_psky.sun_color[2]);
    gl_uniform_3f(&u_disk, program, g_psky.disk[0], g_psky.disk[1], g_psky.disk[2]);
    gl_uniform_3f(&u_top, program, g_psky.stops[0][0], g_psky.stops[0][1], g_psky.stops[0][2]);
    gl_uniform_3f(&u_mid, program, g_psky.stops[1][0], g_psky.stops[1][1], g_psky.stops[1][2]);
    gl_uniform_3f(&u_bottom, program, g_psky.stops[2][0], g_psky.stops[2][1], g_psky.stops[2][2]);
    gl_uniform_1f(&u_avg, program, g_psky.avg);
    gl_uniform_1f(&u_day, program, g_psky.day);
    gl_uniform_1f(&u_disk_cos, program, g_psky.disk_cos);
    gl_uniform_1f(&u_time, program, g_psky.time);
    gl_uniform_1f(&u_clouds, program, PSKY_CLOUDS);
    gl_uniform_1i(&u_cine, program, rt_cinematic());
}

static GlUniform u_ivp = GL_UNIFORM_INIT("inverse_view_projection");
static GlUniform u_camera = GL_UNIFORM_INIT("camera_position");
static GlUniform u_tone = GL_UNIFORM_INIT("tone_mapping");
static GlUniform u_exposure = GL_UNIFORM_INIT("tone_mapping_exposure");

static int psky_invert(const float m[16], float out[16]) { // row-major 4x4 inverse (Gauss-Jordan)
    float a[4][8];
    for (int r = 0; r < 4; r++) for (int c = 0; c < 8; c++) a[r][c] = c < 4 ? m[r * 4 + c] : (c - 4 == r ? 1.0f : 0.0f);
    for (int c = 0; c < 4; c++) {
        int pivot = c;
        for (int r = c + 1; r < 4; r++) if (fabsf(a[r][c]) > fabsf(a[pivot][c])) pivot = r;
        if (fabsf(a[pivot][c]) < 1e-20f) return 0;
        if (pivot != c) for (int k = 0; k < 8; k++) { const float t = a[c][k]; a[c][k] = a[pivot][k]; a[pivot][k] = t; }
        const float inv = 1.0f / a[c][c];
        for (int k = 0; k < 8; k++) a[c][k] *= inv;
        for (int r = 0; r < 4; r++) {
            if (r == c) continue;
            const float f = a[r][c];
            for (int k = 0; k < 8; k++) a[r][k] -= f * a[c][k];
        }
    }
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) out[r * 4 + c] = a[r][c + 4];
    return 1;
}

int sky_physical_render(Camera *camera) {
    if (!g_psky.active || g_psky_failed) return 0;
    if (!g_psky_program) {
        char *vert = concat(assets_path(), "shaders/scene_composite.vert");
        char *frag = concat(assets_path(), "shaders/rt_sky.frag");
        g_psky_program = shader_compile(vert, frag);
        free(vert);
        free(frag);
        if (!g_psky_program) {
            g_psky_failed = 1;
            fprintf(stderr, "[rt] physical sky shader failed to compile: rtSky off for this run\n");
            return 0;
        }
        glGenVertexArrays(1, &g_psky_vao);
    }

    int viewport[4];
    glGetIntegerv(GL_VIEWPORT, viewport);
    camera_update_projection_matrix(camera, (float) viewport[2] / (float) viewport[3]);
    camera_update_world_inverse_matrix(camera);
    float view_projection[16], inverse[16];
    mat4x4(camera->projection_matrix, camera->world_inverse_matrix, view_projection);
    if (!psky_invert(view_projection, inverse)) return 0;

    glUseProgram(g_psky_program);
    g_active_shader = g_psky_program;
    gl_uniform_mat4(&u_ivp, g_psky_program, GL_TRUE, inverse);
    gl_uniform_3f(&u_camera, g_psky_program, camera->position.x, camera->position.y, camera->position.z);
    gl_uniform_1i(&u_tone, g_psky_program, g_render_lighting.tone_mapping);
    gl_uniform_1f(&u_exposure, g_psky_program, g_render_lighting.tone_mapping_exposure);
    glUniform1f(glGetUniformLocation(g_psky_program, "cine_exposure"), rt_grade_exposure()); // Cinematic curve, 0 off
    psky_update_disk(camera->position); // the sky pass runs first in the frame: the RT passes and krunker_fog reuse it
    sky_physical_uniforms(g_psky_program, 1);

    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glBindVertexArray(g_psky_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    return 1;
}

void sky_physical_fini(void) {
    if (g_psky_vao) glDeleteVertexArrays(1, &g_psky_vao);
    if (g_psky_program) glDeleteProgram(g_psky_program);
    if (g_active_shader == g_psky_program && g_psky_program) g_active_shader = 0;
    g_psky_vao = g_psky_program = 0;
    g_psky_failed = 0;
    g_psky.key[0] = 0;
    g_psky.authored = 0;
}
