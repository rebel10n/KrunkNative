// Krunker v7.1.4 store-item material effects (client/include/skin_fx.h).
#include <gl.h>
#include <skin_fx.h>
#include <skin_video.h>
#include <gltf.h>
#include "viewer/viewer_store.h"
#include <cosmetics.h> // cosmetic_asset_path: a custom skin's user-asset keys
#include <cJSON.h>     // customShaderInfo (ViewerSkin.shader_info)
#include <client.h>    // g_render_lighting: shader 18's Lambert
#include <ruleset.h>   // light_unit_factor (as basic.c)
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SKIN_FX_PULSE_SPEED 0.003f // render.js: pulsVal += delta * 0.003
#define SKIN_FX_RGB_SPEED 0.3f     // rgbS || 0.3
#define SKIN_FX_BOB 0.55f          // rAnimM || 0.55
#define SKIN_FX_FRAMES_PER_ROW 10  // getMat: numX = min(10, frames), numY = ceil(frames / 10)
#define SKIN_FX_SHADER_COUNT 8     // src/data/shaders.js ids 0-7
#define SKIN_FX_SHADER_WIDTH 1500.0f // C240: width 1500, height 1500 / (aspectRatio || 2)
#define SKIN_FX_SHADER_ASPECT 2.0f

// One registered material (or mesh, for rAnim) and its animation state.
typedef struct {
    const Mesh *mesh;             // the owner (skin_fx_forget)
    const GltfInstance *model;    // or the GLTF model it belongs to (skin_fx_forget_gltf)
    BasicMaterial *material;      // NULL for a pure rAnim entry
    MeshTransform *spin;          // rAnim target: the mesh's / model root's transform
    // frames
    int frames, num_x, num_y, cur;
    float frame_t, frame_left;
    // movT / rotT
    float mov_t, rot_t;
    int mov_d;
    // pulse
    unsigned char pulse;
    float min_glo;
    // rgb
    unsigned char rgb;
    int rgb_type;                 // rgbT: 1 = the colour, 0 = the emissive
    float rgb_s, rgb_dlt;
    // rAnim
    float r_anim, r_mlt, r_org, r_bob;
    char r_axis;
    unsigned char r_bob_type;     // rAnimT: bob instead of a spin
    // video: the id + 1 of the video texture the mesh's material shows (skin_video_release at forget), 0 none
    int video;
} SkinFxEntry;

static struct {
    SkinFxEntry *entries;
    int count, capacity;
    float puls_val;               // RENDER.pulsVal, shared by every pulsing material
} g_fx;

// ---------------------------------------------------------------- helpers

static unsigned int fx_texture(const char *key) {
    char *path = cosmetic_asset_path("textures", key, "png"); // a custom skin's "_e" glow map is under user-assets/
    if (!path) return 0;
    ensure_asset_file(path);
    FILE *file = fopen(path, "rb");
    unsigned int texture = 0;
    if (file) {
        fclose(file);
        texture = load_texture(path);
    }
    free(path);
    return texture;
}

static vec4 fx_color(const int hex, const float alpha) {
    return (vec4) {(float) ((hex >> 16) & 255) / 255.0f, (float) ((hex >> 8) & 255) / 255.0f, (float) (hex & 255) / 255.0f, alpha};
}

int skin_fx_hex_from_hue(const int hue) {
    const float h = (float) hue / 60.0f;
    const int x = (int) ((1.0f - fabsf(fmodf(h, 2.0f) - 1.0f)) * 255.0f);
    const int f = (int) floorf(h);
    const int r = f < 1 || f > 4 ? 255 : f == 1 || f == 4 ? x : 0;
    const int g = f == 0 || f == 3 ? x : f == 1 || f == 2 ? 255 : 0;
    const int b = f == 0 || f == 1 ? 0 : f == 3 || f == 4 ? 255 : x;
    return ((r & 255) << 16) | ((g & 255) << 8) | (b & 255);
}

static SkinFxEntry *fx_add(void) {
    if (g_fx.count == g_fx.capacity) {
        const int capacity = g_fx.capacity ? g_fx.capacity * 2 : 64;
        SkinFxEntry *entries = realloc(g_fx.entries, (size_t) capacity * sizeof(*entries));
        if (!entries) return NULL;
        g_fx.entries = entries;
        g_fx.capacity = capacity;
    }
    SkinFxEntry *entry = &g_fx.entries[g_fx.count++];
    memset(entry, 0, sizeof(*entry));
    return entry;
}

// The animations of `skin` on `material` (frames / movT / rotT / pulse / rgb) and `spin` (rAnim).
static void fx_register(const Mesh *mesh, const GltfInstance *model, BasicMaterial *material, MeshTransform *spin, const ViewerSkin *skin, const int with_frames_pulse) {
    const int frames = with_frames_pulse && material && skin->frames > 0 && skin->frame_t > 0;
    const int pulse = with_frames_pulse && material && skin->puls_t != 0.0f;
    const int mov = material && (skin->mov_t != 0.0f || skin->rot_t != 0.0f);
    const int rgb = material && (skin->flags & VS_RGB);
    const int r_anim = spin && skin->r_anim != 0.0f;
    if (!frames && !pulse && !mov && !rgb && !r_anim) return;

    SkinFxEntry *entry = fx_add();
    if (!entry) return;
    entry->mesh = mesh;
    entry->model = model;
    entry->material = material;
    if (frames) {
        entry->frames = skin->frames;
        entry->num_x = skin->frames < SKIN_FX_FRAMES_PER_ROW ? skin->frames : SKIN_FX_FRAMES_PER_ROW;
        entry->num_y = skin->frames > SKIN_FX_FRAMES_PER_ROW ? (skin->frames + SKIN_FX_FRAMES_PER_ROW - 1) / SKIN_FX_FRAMES_PER_ROW : 0;
        entry->frame_t = entry->frame_left = (float) skin->frame_t;
    }
    entry->mov_t = mov ? skin->mov_t : 0.0f;
    entry->mov_d = (int) skin->mov_d;
    entry->rot_t = mov ? skin->rot_t : 0.0f;
    entry->pulse = (unsigned char) pulse;
    entry->min_glo = skin->min_glo;
    entry->rgb = (unsigned char) rgb;
    entry->rgb_type = (int) skin->rgb_t;
    entry->rgb_s = skin->rgb_s != 0.0f ? skin->rgb_s : SKIN_FX_RGB_SPEED;
    if (r_anim) {
        // f1115: axis (rAnimA || "y"); a bob moves position[axis] (lower case), a spin rotateX/Y/Z
        entry->spin = spin;
        entry->r_anim = skin->r_anim;
        entry->r_axis = skin->r_anim_axis ? skin->r_anim_axis : 'y';
        entry->r_bob_type = skin->r_anim_t != 0.0f;
        entry->r_mlt = skin->r_anim_m != 0.0f ? skin->r_anim_m : SKIN_FX_BOB;
        entry->r_org = entry->r_axis == 'x' ? spin->position.x : entry->r_axis == 'z' ? spin->position.z : spin->position.y;
        // rotateX / rotateZ after the loadMesh y turn: three.js Object3D.rotateX = local (intrinsic) axis
        if (!entry->r_bob_type) spin->rotation_order = ROTATION_ORDER_YXZ;
    }
}

// ---------------------------------------------------------------- ShaderToy materials

typedef struct {
    Material base;
    int shader;
    int double_sided;
} ShaderToyMaterial;

static unsigned int g_shader_programs[SKIN_FX_SHADER_COUNT];
static unsigned char g_shader_failed[SKIN_FX_SHADER_COUNT];
static unsigned int g_black_texture;
static double g_shader_clock_start = -1.0;
static int g_shader_frame;

// C240's fragment prelude (WebGL1 `texture()` wrapper dropped: GLSL ES 3.00 has it), around
// mainImage(outfrag, iResolution * vUv)
static const char k_shadertoy_prelude[] =
    "out vec4 kn_frag_color;\n"
    "in vec2 vUv;\n"
    "uniform vec2 iResolution;\n"
    "uniform float iTime;\n"
    "uniform float iTimeDelta;\n"
    "uniform int iFrame;\n"
    "uniform vec4 iDate;\n"
    "uniform vec4 iMouse;\n"
    "uniform vec3 iChannelResolution[4];\n"
    "uniform sampler2D iChannel0;\n"
    "uniform sampler2D iChannel1;\n"
    "uniform sampler2D iChannel2;\n"
    "uniform sampler2D iChannel3;\n"
    "void mainImage(out vec4 fragColor, in vec2 fragCoord);\n"
    "void main() {\n"
    "    vec4 outfrag;\n"
    "    mainImage(outfrag, iResolution * vUv);\n"
    "    kn_frag_color = outfrag;\n"
    "}\n";

#ifdef KN_GLES
static const char k_glsl_header[] = "#version 300 es\nprecision highp float; precision highp int; precision highp sampler2D;\n";
#elif defined(__APPLE__)
static const char k_glsl_header[] = "#version 410 core\n";
#else
static const char k_glsl_header[] = "#version 450 core\n";
#endif

static unsigned int fx_compile_stage(const unsigned int type, const char *const *parts, const int count) {
    const unsigned int shader = glCreateShader(type);
    glShaderSource(shader, count, parts, NULL);
    glCompileShader(shader);
    int status;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (!status) {
        char error[512];
        glGetShaderInfoLog(shader, sizeof(error), NULL, error);
        printf("[skin_fx] shadertoy shader compile error:\n%s\n", error);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static char *fx_read_asset(const char *rel) {
    char *path = concat(assets_path(), rel);
    if (!path) return NULL;
    ensure_asset_file(path);
    unsigned char *data = NULL;
    size_t length = 0;
    const int failed = read_file(path, &data, &length);
    free(path);
    return failed ? NULL : (char *) data;
}

// assets/shaders/shadertoy.vert + the prelude + assets/shaders/shadertoy/<n>.glsl, linked once
static unsigned int fx_shader_program(const int n) {
    if (n < 0 || n >= SKIN_FX_SHADER_COUNT || g_shader_failed[n]) return 0;
    if (g_shader_programs[n]) return g_shader_programs[n];

    char rel[64];
    snprintf(rel, sizeof(rel), "shaders/shadertoy/%d.glsl", n);
    char *code = fx_read_asset(rel);
    char *vert_source = fx_read_asset("shaders/shadertoy.vert");
    unsigned int program = 0;
    if (code && vert_source) {
        const char *vert_body = strchr(vert_source, '\n') ? strchr(vert_source, '\n') + 1 : vert_source; // the "#version" line is ours
        const char *vert_parts[2] = {k_glsl_header, vert_body};
        const char *frag_parts[3] = {k_glsl_header, k_shadertoy_prelude, code};
        const unsigned int vert = fx_compile_stage(GL_VERTEX_SHADER, vert_parts, 2);
        const unsigned int frag = vert ? fx_compile_stage(GL_FRAGMENT_SHADER, frag_parts, 3) : 0;
        if (vert && frag) {
            program = glCreateProgram();
            glAttachShader(program, vert);
            glAttachShader(program, frag);
            glLinkProgram(program);
            int status;
            glGetProgramiv(program, GL_LINK_STATUS, &status);
            if (!status) {
                char error[512];
                glGetProgramInfoLog(program, sizeof(error), NULL, error);
                printf("[skin_fx] shadertoy %d link error:\n%s\n", n, error);
                glDeleteProgram(program);
                program = 0;
            }
        }
        if (vert) glDeleteShader(vert);
        if (frag) glDeleteShader(frag);
    }
    free(code);
    free(vert_source);
    if (!program) g_shader_failed[n] = 1;
    g_shader_programs[n] = program;
    return program;
}

#define U(var, name) static GlUniform var = GL_UNIFORM_INIT(name)
U(u_resolution, "iResolution");
U(u_time, "iTime");
U(u_time_delta, "iTimeDelta");
U(u_frame, "iFrame");
U(u_date, "iDate");
U(u_mouse, "iMouse");
U(u_channel0, "iChannel0");
U(u_channel1, "iChannel1");
U(u_channel2, "iChannel2");
U(u_channel3, "iChannel3");
#undef U

static void shadertoy_update_uniforms(Material *base) {
    ShaderToyMaterial *material = (ShaderToyMaterial *) base;
    const unsigned int program = material->base.program;
    scene_draw_state(!material->double_sided, 0, 1);

    // iTime: the material clock (C240: THREE.Clock started with the material); one clock for all
    // here, on gltf_clock() so --render-scenario captures pin it
    const double now = gltf_clock();
    if (g_shader_clock_start < 0.0) g_shader_clock_start = now;
    const float h = SKIN_FX_SHADER_WIDTH / SKIN_FX_SHADER_ASPECT;
    gl_uniform_2f(&u_resolution, program, SKIN_FX_SHADER_WIDTH, h);
    gl_uniform_1f(&u_time, program, (float) (now - g_shader_clock_start));
    gl_uniform_1f(&u_time_delta, program, 1.0f / 60.0f);
    gl_uniform_1i(&u_frame, program, g_shader_frame);
    gl_uniform_4f(&u_date, program, 0.0f, 0.0f, 0.0f, (float) fmod(now, 86400.0));
    gl_uniform_4f(&u_mouse, program, SKIN_FX_SHADER_WIDTH / 2.0f, h / 2.0f, SKIN_FX_SHADER_WIDTH / 2.0f, h / 2.0f);

    // iChannel0-3: the material map, which getMat never loads for a ShaderToy material: empty (black)
    if (!g_black_texture) {
        glGenTextures(1, &g_black_texture);
        glBindTexture(GL_TEXTURE_2D, g_black_texture);
        const unsigned char black[4] = {0, 0, 0, 255};
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, black);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    glBindTexture(GL_TEXTURE_2D, g_black_texture);
    g_active_texture = g_black_texture;
    gl_uniform_1i(&u_channel0, program, 0);
    gl_uniform_1i(&u_channel1, program, 0);
    gl_uniform_1i(&u_channel2, program, 0);
    gl_uniform_1i(&u_channel3, program, 0);
}

static const MaterialVTable shadertoy_material_vtable = {shadertoy_update_uniforms};

// ---------------------------------------------------------------- v10 "custom" skin shaders
// v10's custom entries (type "custom", deob-10.0.4.js:195962-196150; their TSL twins :92213-93011) are ported as GLSL files:
// assets/shaders/skin_custom.vert + assets/shaders/skin_custom/<n>.frag (8-18). A skin draws one only through its
// customShaderInfo (getMat, :203447-203475: a custom shader with no customShaderInfo keeps the item's own material): "*" or
// the entry whose shaderNum is the skin's on an OBJ item, per part (the glTF node name, "*" / applyToAll = every mesh) on a
// model (:204930-204986). Each material is the bundle's custom-shader class (:156038-156076: transparent, DoubleSide, normal
// blending, no fog) with its build's overrides, its uniforms (the shader's defaults or the named preset of
// shaderArgs.uniforms, then an object shaderArgs.uniforms over them, :156052) and the part's matArgs (side / transparent /
// depthWrite, :204969-204983).
// v10.0.5 adds the "graph" entries (deob-10.0.5.js:196197-197438, the first is 18 "Ametwista"): a node graph the bundle
// compiles to a lit / unlit node material (iiïîiîì, :95805-95835), ported here as 18.frag. getMat takes a graph shader
// from the skin's shaderNum alone (customShaderInfo optional: its entry's shaderArgs, :204743-204760), a model's parts
// from customShaderInfo as the custom ones (:206222-206243); shaderArgs.params sets the graph's params by name (:94816).
#define SKIN_FX_CUSTOM_FIRST 8
#define SKIN_FX_CUSTOM_LAST 18
#define SKIN_FX_CUSTOM_MAX 32
#define SKIN_FX_CUSTOM_UNIFORMS 24 // 18 has 18 params
#define SKIN_FX_SKYLINE_MAX 48 // 17.frag KN_SKYLINE_MAX
#define SKIN_FX_SHOCK_SECONDS 0.75 // shader 14's pulse, sin(min(t / 0.75, 1) * PI) (deob-10.0.4.js:92705-92711)
#define SKIN_FX_SHOCK_ARCS 4        // shader 14 keeps four arcs, one per shot (deob-10.0.4.js:92686-92688)

typedef struct {
    char name[24];
    int count; // 1-4 floats
    float v[4];
    GlUniform handle;
} CustomUniform;

typedef struct {
    ShaderToyMaterial st; // base / shader / double_sided (the ShaderToy fields, so skin_fx_tick's casts stay valid)
    int cull_back;        // side 0 (FrontSide)
    int depth_write;
    int displace;                     // skin_custom.vert uDisplace: 1 shader 9 with uWobble, 2 shader 10 with uWarp
    int seeded;                       // shader 15 (seeded, deob-10.0.4.js:92858)
    float seed;                       // args.seed: skin_fx_seed_mix of the copy's seed, 0 none
    int shot_pulse;                   // shader 14 (shotPulse, deob-10.0.4.js:92676)
    int pulse_pending;                // a shot since the last draw (the bundle's uShock >= 1, :92695)
    int arcs_active;                  // an arc played at the last draw (the bundle's iIîîiîî, :92692)
    double arc_clock;                 // gltf_clock() of the last draw
    float arc_age[SKIN_FX_SHOCK_ARCS];   // seconds since the arc's shot, < 0 free (:92688)
    float arc_time[SKIN_FX_SHOCK_ARCS];  // the arc's own animation time, random(0..1) at the shot + age (arcs.time, :92686)
    float arc_shock[SKIN_FX_SHOCK_ARCS]; // sin(min(age / 0.75, 1) * PI) (arcs.shock, :92705-92711)
    int uniform_count;
    CustomUniform uniforms[SKIN_FX_CUSTOM_UNIFORMS];
    int skyline_count; // shader 17's args.skyline (0: its default)
    float skyline[SKIN_FX_SKYLINE_MAX * 2];
    float moon[3], scale;
} CustomMaterial;

static unsigned int g_custom_programs[SKIN_FX_CUSTOM_MAX];
static unsigned char g_custom_failed[SKIN_FX_CUSTOM_MAX];

static unsigned int fx_custom_program(const int n) {
    if (n < SKIN_FX_CUSTOM_FIRST || n > SKIN_FX_CUSTOM_LAST || g_custom_failed[n]) return 0;
    if (g_custom_programs[n]) return g_custom_programs[n];
    char rel[64];
    snprintf(rel, sizeof(rel), "shaders/skin_custom/%d.frag", n);
    char *frag_source = fx_read_asset(rel);
    char *vert_source = fx_read_asset("shaders/skin_custom.vert");
    unsigned int program = 0;
    if (frag_source && vert_source) {
        const char *vert_body = strchr(vert_source, '\n') ? strchr(vert_source, '\n') + 1 : vert_source; // "#version" is ours
        const char *vert_parts[2] = {k_glsl_header, vert_body};
        const char *frag_parts[2] = {k_glsl_header, frag_source};
        const unsigned int vert = fx_compile_stage(GL_VERTEX_SHADER, vert_parts, 2);
        const unsigned int frag = vert ? fx_compile_stage(GL_FRAGMENT_SHADER, frag_parts, 2) : 0;
        if (vert && frag) {
            program = glCreateProgram();
            glAttachShader(program, vert);
            glAttachShader(program, frag);
            glLinkProgram(program);
            int status;
            glGetProgramiv(program, GL_LINK_STATUS, &status);
            if (!status) {
                char error[512];
                glGetProgramInfoLog(program, sizeof(error), NULL, error);
                printf("[skin_fx] custom shader %d link error:\n%s\n", n, error);
                glDeleteProgram(program);
                program = 0;
            }
        }
        if (vert) glDeleteShader(vert);
        if (frag) glDeleteShader(frag);
    }
    free(frag_source);
    free(vert_source);
    if (!program) g_custom_failed[n] = 1;
    g_custom_programs[n] = program;
    return program;
}

static CustomUniform *custom_uniform(CustomMaterial *m, const char *name) {
    for (int i = 0; i < m->uniform_count; i++)
        if (!strcmp(m->uniforms[i].name, name)) return &m->uniforms[i];
    if (m->uniform_count >= SKIN_FX_CUSTOM_UNIFORMS || strlen(name) >= sizeof(m->uniforms[0].name)) return NULL;
    CustomUniform *u = &m->uniforms[m->uniform_count++];
    memset(u, 0, sizeof(*u));
    snprintf(u->name, sizeof(u->name), "%s", name);
    return u;
}

static void custom_set(CustomMaterial *m, const char *name, const int count, const float a, const float b, const float c) {
    CustomUniform *u = custom_uniform(m, name);
    if (!u) return;
    u->count = count;
    u->v[0] = a;
    u->v[1] = b;
    u->v[2] = c;
}

static void custom_set_hex(CustomMaterial *m, const char *name, const int hex) {
    custom_set(m, name, 3, (float) ((hex >> 16) & 255) / 255.0f, (float) ((hex >> 8) & 255) / 255.0f, (float) (hex & 255) / 255.0f);
}

// The shader's uniforms (the table's `uniforms`, a preset name for 9 / 10, deob-10.0.4.js:195971-196100)
static void custom_defaults(CustomMaterial *m, const int n, const char *preset) {
    const int is = preset != NULL;
    switch (n) {
    case 9: // GasFresnal: "main" / "blackhole" (deob-10.0.4.js:92257-92316; no preset: no uniforms, the shader's zeros)
        if (is && !strcmp(preset, "main")) {
            custom_set_hex(m, "uBase", 0x1d0033);
            custom_set_hex(m, "uAccent", 0x5b5cc2);
            custom_set(m, "uScale", 1, 500.0f, 0, 0);
            custom_set(m, "uPower", 1, 1.0f, 0, 0);
            custom_set(m, "uWobble", 1, 1.0f, 0, 0);
            custom_set(m, "uStars", 1, 1.0f, 0, 0);
        } else if (is && !strcmp(preset, "blackhole")) {
            custom_set_hex(m, "uBase", 0x2b0025);
            custom_set_hex(m, "uAccent", 0xd926d9);
            custom_set(m, "uScale", 1, 200.0f, 0, 0);
            custom_set(m, "uPower", 1, 0.8f, 0, 0);
            custom_set(m, "uWobble", 1, 0.0f, 0, 0);
            custom_set(m, "uStars", 1, 0.0f, 0, 0);
        }
        break;
    case 10: // AngelSheel: main / accent / melee_main / melee_accent = (uDisplacement, uWarp), uScale 1000 (:92375-92394)
        if (is) {
            const int main = !strcmp(preset, "main"), accent = !strcmp(preset, "accent");
            const int melee_main = !strcmp(preset, "melee_main"), melee_accent = !strcmp(preset, "melee_accent");
            if (main || accent || melee_main || melee_accent) {
                custom_set(m, "uScale", 1, 1000.0f, 0, 0);
                custom_set(m, "uDisplacement", 1, main || accent ? 0.15f : 0.02f, 0, 0);
                custom_set(m, "uWarp", 1, main || melee_main ? 1.0f : 0.0f, 0, 0);
            }
        }
        break;
    case 11: // the halftone's defaults (uColor only comes from shaderArgs)
        custom_set_hex(m, "uLightColor", 0xfef8c8);
        custom_set(m, "uResolution", 2, 1000.0f, 500.0f, 0);
        custom_set_hex(m, "uShadowColor", 0x9217a4);
        custom_set(m, "uLightIntensity", 1, 1.0f, 0, 0);
        custom_set(m, "uLightRepetitions", 1, 100.0f, 0, 0);
        custom_set(m, "uShadowRepetitions", 1, 100.0f, 0, 0);
        break;
    case 18: // Ametwista's params (deob-10.0.5.js:196207-196411)
        custom_set(m, "stockX", 1, -7.4f, 0, 0);
        custom_set(m, "muzzleX", 1, 12.2f, 0, 0);
        custom_set_hex(m, "deepColor", 0x160826);
        custom_set_hex(m, "midColor", 0x6e35d9);
        custom_set_hex(m, "lightColor", 0xf1e6ff);
        custom_set(m, "crystalScale", 1, 1.8f, 0, 0);
        custom_set(m, "facetDepth", 1, 0.5f, 0, 0);
        custom_set(m, "glintAmt", 1, 0.9f, 0, 0);
        custom_set(m, "sparkleRate", 1, 0.05f, 0, 0);
        custom_set(m, "glowAmt", 1, 0.7f, 0, 0);
        custom_set(m, "blinkSpeed", 1, 0.6f, 0, 0);
        custom_set_hex(m, "sparkleColor", 0xa970ff);
        custom_set_hex(m, "outlineColor", 0xff5ce1);
        custom_set(m, "outlineAmt", 1, 1.2f, 0, 0);
        custom_set(m, "outlineSpeed", 1, 0.4f, 0, 0);
        custom_set(m, "outlineCount", 1, 1.0f, 0, 0);
        custom_set(m, "bottomY", 1, -3.8f, 0, 0);
        custom_set(m, "topY", 1, 1.4f, 0, 0);
        break;
    default:
        break;
    }
}

// Shader 18's float params' ranges (deob-10.0.5.js:196207-196411): an override is clamped to them (iîïïìîî, :94780-94783)
typedef struct {
    const char *name;
    float min, max;
} GraphRange;

static const GraphRange k_ametwista_ranges[] = {
    {"stockX", -30.0f, 0.0f}, {"muzzleX", 0.0f, 30.0f}, {"crystalScale", 0.5f, 6.0f}, {"facetDepth", 0.0f, 1.0f},
    {"glintAmt", 0.0f, 4.0f}, {"sparkleRate", 0.0f, 0.5f}, {"glowAmt", 0.0f, 3.0f}, {"blinkSpeed", 0.1f, 3.0f},
    {"outlineAmt", 0.0f, 4.0f}, {"outlineSpeed", 0.0f, 2.0f}, {"outlineCount", 0.5f, 5.0f}, {"bottomY", -15.0f, 0.0f},
    {"topY", 0.0f, 15.0f},
};

// shaderArgs.params over a graph's defaults (the param node, deob-10.0.5.js:95064-95100): only the graph's own params,
// a colour as "#rrggbb" or [r, g, b], a float as a finite number clamped to the param's range
static void custom_graph_params(CustomMaterial *m, const cJSON *params) {
    const cJSON *p;
    cJSON_ArrayForEach(p, params) {
        if (!p->string) continue;
        CustomUniform *u = NULL;
        for (int i = 0; i < m->uniform_count; i++)
            if (!strcmp(m->uniforms[i].name, p->string)) u = &m->uniforms[i];
        if (!u) continue;
        if (u->count == 3) {
            if (cJSON_IsString(p) && strlen(p->valuestring) == 7 && p->valuestring[0] == '#' &&
                strspn(p->valuestring + 1, "0123456789abcdefABCDEF") == 6) {
                custom_set_hex(m, p->string, (int) strtol(p->valuestring + 1, NULL, 16));
            } else if (cJSON_IsArray(p) && cJSON_GetArraySize(p) == 3) {
                float f[3];
                int ok = 1;
                for (int i = 0; i < 3; i++) {
                    const cJSON *c = cJSON_GetArrayItem(p, i);
                    ok = ok && cJSON_IsNumber(c) && isfinite(c->valuedouble);
                    f[i] = ok ? (float) c->valuedouble : 0.0f;
                }
                if (ok) custom_set(m, p->string, 3, f[0], f[1], f[2]);
            }
        } else if (u->count == 1 && cJSON_IsNumber(p) && isfinite(p->valuedouble)) {
            float v = (float) p->valuedouble;
            for (size_t i = 0; i < sizeof(k_ametwista_ranges) / sizeof(k_ametwista_ranges[0]); i++) {
                if (strcmp(k_ametwista_ranges[i].name, p->string)) continue;
                if (v < k_ametwista_ranges[i].min) v = k_ametwista_ranges[i].min;
                if (v > k_ametwista_ranges[i].max) v = k_ametwista_ranges[i].max;
            }
            u->v[0] = v;
        }
    }
}

static int custom_hex(const cJSON *v) {
    if (cJSON_IsNumber(v)) return (int) v->valuedouble;
    if (cJSON_IsString(v) && v->valuestring[0] == '#') return (int) strtol(v->valuestring + 1, NULL, 16);
    return -1;
}

// An object shaderArgs.uniforms over the defaults (iìîïïîî, deob-10.0.4.js:95760-95775: color, vec2-4, bool, number)
static void custom_uniforms_object(CustomMaterial *m, const cJSON *uniforms) {
    const cJSON *u;
    cJSON_ArrayForEach(u, uniforms) {
        const cJSON *value = cJSON_IsObject(u) && cJSON_GetObjectItem(u, "value") ? cJSON_GetObjectItem(u, "value") : u;
        const cJSON *type = cJSON_IsObject(u) ? cJSON_GetObjectItem(u, "type") : NULL;
        if (cJSON_IsString(type) && !strcmp(type->valuestring, "color")) {
            const int hex = custom_hex(value);
            if (hex >= 0) custom_set_hex(m, u->string, hex);
        } else if (cJSON_IsArray(value)) {
            float f[3] = {0, 0, 0};
            const int count = cJSON_GetArraySize(value) < 3 ? cJSON_GetArraySize(value) : 3;
            for (int i = 0; i < count; i++) f[i] = (float) cJSON_GetArrayItem(value, i)->valuedouble;
            if (count >= 2) custom_set(m, u->string, count, f[0], f[1], f[2]);
        } else if (cJSON_IsBool(value)) {
            custom_set(m, u->string, 1, cJSON_IsTrue(value) ? 1.0f : 0.0f, 0, 0);
        } else if (cJSON_IsNumber(value)) {
            custom_set(m, u->string, 1, (float) value->valuedouble, 0, 0);
        }
    }
}

// shader 17's args (deob-10.0.4.js:92963-92979): skyline points, moon (x, y, radius), scale; else the build's defaults
static void custom_hollowmere_args(CustomMaterial *m, const cJSON *args) {
    static const float k_skyline[] = {-4.95f, -1.0f, -2.35f, -0.38f, -2.35f, -0.4f, 0.75f, -0.4f, 0.75f, -0.36f, 2.5f, -0.36f, 2.5f, -0.16f};
    const cJSON *skyline = cJSON_GetObjectItem(args, "skyline"), *moon = cJSON_GetObjectItem(args, "moon"), *scale = cJSON_GetObjectItem(args, "scale");
    m->skyline_count = 0;
    if (cJSON_IsArray(skyline) && cJSON_GetArraySize(skyline) >= 2) {
        const cJSON *pt;
        cJSON_ArrayForEach(pt, skyline) {
            if (m->skyline_count >= SKIN_FX_SKYLINE_MAX || !cJSON_IsArray(pt) || cJSON_GetArraySize(pt) < 2) continue;
            m->skyline[m->skyline_count * 2] = (float) cJSON_GetArrayItem(pt, 0)->valuedouble;
            m->skyline[m->skyline_count * 2 + 1] = (float) cJSON_GetArrayItem(pt, 1)->valuedouble;
            m->skyline_count++;
        }
    }
    if (m->skyline_count < 2) {
        m->skyline_count = (int) (sizeof(k_skyline) / sizeof(k_skyline[0]) / 2);
        memcpy(m->skyline, k_skyline, sizeof(k_skyline));
    }
    m->moon[0] = -3.75f;
    m->moon[1] = -0.12f;
    m->moon[2] = 0.24f;
    for (int i = 0; cJSON_IsArray(moon) && i < 3 && i < cJSON_GetArraySize(moon); i++) m->moon[i] = (float) cJSON_GetArrayItem(moon, i)->valuedouble;
    m->scale = cJSON_IsNumber(scale) && scale->valuedouble > 0.0 ? (float) scale->valuedouble : 0.001f;
}

static GlUniform u_custom_skyline = GL_UNIFORM_INIT("uSkyline");
static GlUniform u_custom_skyline_count = GL_UNIFORM_INIT("uSkylineCount");
static GlUniform u_custom_moon = GL_UNIFORM_INIT("uMoon");
static GlUniform u_custom_scale = GL_UNIFORM_INIT("uScale");
static GlUniform u_custom_arc_time = GL_UNIFORM_INIT("uArcTime");
static GlUniform u_custom_arc_shock = GL_UNIFORM_INIT("uArcShock");
static GlUniform u_custom_time = GL_UNIFORM_INIT("time");
static GlUniform u_custom_displace = GL_UNIFORM_INIT("uDisplace");
static GlUniform u_custom_seed = GL_UNIFORM_INIT("uSeed");
static GlUniform u_custom_utime = GL_UNIFORM_INIT("uTime");
static GlUniform u_custom_ambient_color = GL_UNIFORM_INIT("ambient_color");
static GlUniform u_custom_light_color = GL_UNIFORM_INIT("light_color");
static GlUniform u_custom_light_direction = GL_UNIFORM_INIT("light_direction");
static GlUniform u_custom_ambient_intensity = GL_UNIFORM_INIT("ambient_intensity");
static GlUniform u_custom_light_intensity = GL_UNIFORM_INIT("light_intensity");
static GlUniform u_custom_correct_lights = GL_UNIFORM_INIT("correct_lights");

// A graph shader (v10.0.5 "type": "graph", iíiîïìî, deob-10.0.5.js:95865-95867)
static int custom_is_graph(const int n) { return n == 18; }

// shader 14's arcs (onObjectUpdate, deob-10.0.4.js:92689-92717): a shot takes a free arc (else the oldest) with a random
// start time; each draw advances every playing arc by the time since the last draw (0 when none played then), its shock
// following sin(min(age / 0.75, 1) * PI) until 0.75 s.
static void custom_arcs_step(CustomMaterial *m, const double now) {
    const float dt = m->arcs_active ? (float) (now - m->arc_clock) : 0.0f;
    m->arc_clock = now;
    if (m->pulse_pending) {
        m->pulse_pending = 0;
        int slot = -1;
        for (int i = 0; i < SKIN_FX_SHOCK_ARCS && slot < 0; i++)
            if (m->arc_age[i] < 0.0f) slot = i;
        if (slot < 0) {
            slot = 0;
            for (int i = 1; i < SKIN_FX_SHOCK_ARCS; i++)
                if (m->arc_age[i] > m->arc_age[slot]) slot = i;
        }
        m->arc_age[slot] = 0.0f;
        m->arc_time[slot] = (float) rand() / (float) RAND_MAX;
    }
    int active = 0;
    for (int i = 0; i < SKIN_FX_SHOCK_ARCS; i++) {
        if (m->arc_age[i] < 0.0f) continue;
        m->arc_age[i] += dt;
        m->arc_time[i] += dt;
        const float k = m->arc_age[i] / (float) SKIN_FX_SHOCK_SECONDS < 1.0f ? m->arc_age[i] / (float) SKIN_FX_SHOCK_SECONDS : 1.0f;
        m->arc_shock[i] = (float) sin(k * 3.14159265358979);
        if (k >= 1.0f) {
            m->arc_age[i] = -1.0f;
            m->arc_shock[i] = 0.0f;
        } else {
            active = 1;
        }
    }
    m->arcs_active = active;
}

static void custom_update_uniforms(Material *base) {
    CustomMaterial *m = (CustomMaterial *) base;
    const unsigned int program = m->st.base.program;
    scene_draw_state(m->cull_back, 0, m->depth_write);
    const double now = gltf_clock(); // the material clock, as the ShaderToy ones (the TSL `time`)
    if (g_shader_clock_start < 0.0) g_shader_clock_start = now;
    const float t = (float) (now - g_shader_clock_start);
    gl_uniform_1f(&u_custom_time, program, t);
    gl_uniform_1f(&u_custom_utime, program, t);
    gl_uniform_1i(&u_custom_displace, program, m->displace);
    if (m->seeded) gl_uniform_1f(&u_custom_seed, program, m->seed);
    for (int i = 0; i < m->uniform_count; i++) {
        CustomUniform *u = &m->uniforms[i];
        u->handle.name = u->name;
        if (u->count == 1) gl_uniform_1f(&u->handle, program, u->v[0]);
        else if (u->count == 2) gl_uniform_2f(&u->handle, program, u->v[0], u->v[1]);
        else gl_uniform_3f(&u->handle, program, u->v[0], u->v[1], u->v[2]);
    }
    if (m->st.shader == 17) {
        const int at = gl_uniform_location(&u_custom_skyline, program);
        if (at >= 0) glUniform2fv(at, m->skyline_count, m->skyline); // per material (8074 / 8075 differ): no value cache
        gl_uniform_1i(&u_custom_skyline_count, program, m->skyline_count);
        gl_uniform_3f(&u_custom_moon, program, m->moon[0], m->moon[1], m->moon[2]);
        gl_uniform_1f(&u_custom_scale, program, m->scale);
    }
    if (custom_is_graph(m->st.shader)) { // a lit graph: basic.c's scene lighting (ambient and the sun)
        const float units = ruleset_current()->light_unit_factor;
        gl_uniform_3f(&u_custom_ambient_color, program, g_render_lighting.ambient_color.x, g_render_lighting.ambient_color.y, g_render_lighting.ambient_color.z);
        gl_uniform_3f(&u_custom_light_color, program, g_render_lighting.light_color.x, g_render_lighting.light_color.y, g_render_lighting.light_color.z);
        gl_uniform_3f(&u_custom_light_direction, program, g_render_lighting.light_direction.x, g_render_lighting.light_direction.y, g_render_lighting.light_direction.z);
        gl_uniform_1f(&u_custom_ambient_intensity, program, g_render_lighting.ambient_intensity * units);
        gl_uniform_1f(&u_custom_light_intensity, program, g_render_lighting.light_intensity * units);
        gl_uniform_1i(&u_custom_correct_lights, program, g_render_lighting.correct_lights);
    }
    if (m->shot_pulse) {
        custom_arcs_step(m, now);
        int at = gl_uniform_location(&u_custom_arc_time, program);
        if (at >= 0) glUniform1fv(at, SKIN_FX_SHOCK_ARCS, m->arc_time); // per material: no value cache
        at = gl_uniform_location(&u_custom_arc_shock, program);
        if (at >= 0) glUniform1fv(at, SKIN_FX_SHOCK_ARCS, m->arc_shock);
    }
}

static const MaterialVTable custom_material_vtable = {custom_update_uniforms};

// A custom-shader material for shader n with this customShaderInfo entry (its shaderArgs / matArgs), or NULL
static CustomMaterial *custom_material(const int n, const cJSON *entry) {
    const unsigned int program = fx_custom_program(n);
    if (!program) return NULL;
    CustomMaterial *m = calloc(1, sizeof(CustomMaterial));
    if (!m) return NULL;
    m->st.base.vtable = &custom_material_vtable;
    m->st.base.program = program;
    m->st.shader = n;
    // the class (deob-10.0.4.js:156044-156049): transparent, DoubleSide; the builds' overrides (:92846-92848, :92920-92921,
    // :92956, :93006-93007): 14 side 0, 15 side 0 opaque, 16 opaque, 17 side 0 opaque
    // 18 (a graph): its output node's settings, lit / side front / normal blending / depthWrite / opaque (:197240, :95831-95834)
    m->st.base.transparent = !(n == 15 || n == 16 || n == 17 || n == 18);
    m->cull_back = n == 14 || n == 15 || n == 17 || n == 18;
    m->depth_write = 1;
    m->shot_pulse = n == 14;
    m->seeded = n == 15;
    for (int i = 0; i < SKIN_FX_SHOCK_ARCS; i++) m->arc_age[i] = -1.0f;
    const cJSON *args = cJSON_GetObjectItem(entry, "shaderArgs");
    const cJSON *uniforms = cJSON_GetObjectItem(args, "uniforms");
    custom_defaults(m, n, cJSON_IsString(uniforms) ? uniforms->valuestring : NULL);
    if (custom_is_graph(n)) custom_graph_params(m, cJSON_GetObjectItem(args, "params"));
    else if (cJSON_IsObject(uniforms)) custom_uniforms_object(m, uniforms);
    if (n == 17) custom_hollowmere_args(m, args);
    // the positionNode of 9 / 10 (deob-10.0.4.js:92332-92337, :92402-92407): select(uWobble / uWarp, displaced, positionLocal)
    const CustomUniform *wobble = NULL;
    for (int i = 0; i < m->uniform_count; i++)
        if (!strcmp(m->uniforms[i].name, n == 9 ? "uWobble" : "uWarp")) wobble = &m->uniforms[i];
    m->displace = (n == 9 || n == 10) && wobble && wobble->v[0] > 0.5f ? (n == 9 ? 1 : 2) : 0;
    const cJSON *mat = cJSON_GetObjectItem(entry, "matArgs");
    const cJSON *side = cJSON_GetObjectItem(mat, "side"), *transparent = cJSON_GetObjectItem(mat, "transparent"), *depth = cJSON_GetObjectItem(mat, "depthWrite");
    if (cJSON_IsNumber(side)) m->cull_back = side->valueint == 0;
    if (cJSON_IsBool(transparent)) m->st.base.transparent = cJSON_IsTrue(transparent);
    if (cJSON_IsBool(depth)) m->depth_write = cJSON_IsTrue(depth);
    return m;
}

// applyToAll (the table's, deob-10.0.4.js:196115 / :196135): 13 and 15 go on every mesh of the model
static int custom_apply_to_all(const int n) { return n == 13 || n == 15; }

// The customShaderInfo entry for an OBJ item: "*", else the one whose shaderNum is the skin's (deob-10.0.4.js:203459-203461)
static const cJSON *custom_entry(const cJSON *info, const int shader_num) {
    const cJSON *star = cJSON_GetObjectItem(info, "*");
    if (cJSON_IsObject(star)) return star;
    const cJSON *e;
    cJSON_ArrayForEach(e, info) {
        const cJSON *num = cJSON_GetObjectItem(e, "shaderNum");
        if (cJSON_IsNumber(num) && num->valueint == shader_num) return e;
    }
    return NULL;
}

static void custom_replace(Mesh *mesh, CustomMaterial *m) {
    skin_fx_forget(mesh);
    material_fini(mesh->material);
    mesh->material = (Material *) m;
}

// getMat: shaderNum != null && shaders[shaderNum]: a ShaderToyMaterial instead of the item's own (0-7); a custom shader
// (8-17) only with the skin's customShaderInfo; a graph shader (18) with or without it (deob-10.0.5.js:204743-204760)
static void fx_use_shadertoy(Mesh *mesh, const ViewerSkin *skin) {
    if (skin->shader_num >= SKIN_FX_CUSTOM_FIRST) {
        cJSON *info = skin->shader_info ? cJSON_Parse(skin->shader_info) : NULL;
        const cJSON *entry = info ? custom_entry(info, skin->shader_num) : NULL;
        const int graph = custom_is_graph(skin->shader_num);
        CustomMaterial *m = entry || graph ? custom_material(skin->shader_num, entry) : NULL;
        cJSON_Delete(info);
        if (!m) return;
        custom_replace(mesh, m);
        return;
    }
    const unsigned int program = fx_shader_program(skin->shader_num);
    if (!program) return;
    ShaderToyMaterial *material = calloc(1, sizeof(ShaderToyMaterial));
    if (!material) return;
    material->base.vtable = &shadertoy_material_vtable;
    material->base.program = program;
    material->base.transparent = mesh->material ? mesh->material->transparent : 0;
    material->shader = skin->shader_num;
    material->double_sided = skin->side == 2;
    material_fini(mesh->material);
    mesh->material = (Material *) material;
}

// A model's customShaderInfo (deob-10.0.4.js:204930-204986): each part's custom shader on the mesh of that glTF node name
// ("*" / applyToAll: every mesh), then its matArgs on that mesh
static void fx_custom_gltf(GltfInstance *model, const ViewerSkin *skin) {
    cJSON *info = skin->shader_info ? cJSON_Parse(skin->shader_info) : NULL;
    const cJSON *entry;
    cJSON_ArrayForEach(entry, info) {
        const char *part = entry->string;
        const cJSON *num = cJSON_GetObjectItem(entry, "shaderNum");
        const int n = cJSON_IsNumber(num) ? num->valueint : -1;
        const int all = !strcmp(part, "*") || custom_apply_to_all(n);
        for (size_t i = 0; i < model->mesh_count; i++) {
            Mesh *mesh = model->meshes[i];
            if (!mesh || !mesh->material) continue;
            const int named = !strcmp(gltf_instance_mesh_name(model, i), part);
            if (n >= SKIN_FX_CUSTOM_FIRST && (all || named)) {
                CustomMaterial *m = custom_material(n, entry);
                if (!m) continue;
                custom_replace(mesh, m);
            }
            // matArgs on the named part's material (its own or the custom one above); refreshTextures re-reads the map,
            // which KN's per-instance BasicMaterial already holds
            const cJSON *mat = named ? cJSON_GetObjectItem(entry, "matArgs") : NULL;
            if (!mat || mesh->material->vtable != &basic_material_vtable) continue;
            BasicMaterial *basic = (BasicMaterial *) mesh->material;
            const cJSON *side = cJSON_GetObjectItem(mat, "side"), *transparent = cJSON_GetObjectItem(mat, "transparent"), *depth = cJSON_GetObjectItem(mat, "depthWrite");
            if (cJSON_IsNumber(side)) basic->double_sided = side->valueint == 2;
            if (cJSON_IsBool(transparent)) basic->base.transparent = cJSON_IsTrue(transparent);
            if (cJSON_IsBool(depth)) basic->no_depth_write = !cJSON_IsTrue(depth);
        }
    }
    cJSON_Delete(info);
}

// pulseShotShaders (deob-10.0.4.js:174911-174919): a shot starts the pulse of every shotPulse material of the weapon
static void fx_pulse_mesh(const Mesh *mesh, const double now) {
    if (!mesh || !mesh->material || mesh->material->vtable != &custom_material_vtable) return;
    CustomMaterial *m = (CustomMaterial *) mesh->material;
    (void) now;
    if (m->shot_pulse) m->pulse_pending = 1;
}

// lowbias32 (iîììììî, deob-10.0.4.js:24336-24343)
static uint32_t fx_seed_hash(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

int skin_fx_copy_seed(const int copy_id, const int has_meta_seed, const double meta_seed) {
    if (has_meta_seed) { // (floor(seed) % 1e6 + 1e6) % 1e6
        const double f = fmod(floor(meta_seed), 1000000.0);
        return (int) fmod(f + 1000000.0, 1000000.0);
    }
    return copy_id ? (int) (fx_seed_hash((uint32_t) copy_id) % 1000000u) : 0;
}

float skin_fx_seed_mix(const int seed) {
    return seed ? (float) ((double) fx_seed_hash((uint32_t) seed) / 4294967296.0) : 0.0f;
}

static void fx_seed_mesh(Mesh *mesh, const float seed) {
    if (!mesh || !mesh->material || mesh->material->vtable != &custom_material_vtable) return;
    CustomMaterial *m = (CustomMaterial *) mesh->material;
    if (m->seeded) m->seed = seed;
}

void skin_fx_set_seed(struct GltfInstance *model, Mesh *mesh, const int seed) {
    const float mixed = skin_fx_seed_mix(seed);
    for (size_t i = 0; model && i < model->mesh_count; i++) fx_seed_mesh(model->meshes[i], mixed);
    fx_seed_mesh(mesh, mixed);
}

void skin_fx_shot_pulse(const struct GltfInstance *model, const Mesh *mesh) {
    const double now = gltf_clock();
    for (size_t i = 0; model && i < model->mesh_count; i++) fx_pulse_mesh(model->meshes[i], now);
    fx_pulse_mesh(mesh, now);
}

// ---------------------------------------------------------------- applying

// v10.0.3 loadTexture's VideoTexture branch (skin_video.h, deob-10.0.3.js:231859-231889): the video replaces the map
// (repeat repeatX || 1 is the material's tileS repeat, kept). 0 when noVideoSkins is on or KrunkNative cannot play the
// video: the caller then does what v10's noVideoSkins strip does (deob:231704-231710): no video, no projection, no
// texSrc. The mesh is registered so skin_fx_forget lets the video stop decoding.
static int fx_video(const Mesh *mesh, BasicMaterial *material, const int video, const int linear) {
    if (video < 0 || skin_video_disabled()) return 0;
    const unsigned int texture = skin_video_texture(video, linear);
    if (!texture) return 0;
    SkinFxEntry *entry = fx_add();
    if (!entry) {
        skin_video_release(video);
        return 0;
    }
    entry->mesh = mesh;
    entry->video = video + 1;
    material->texture = texture;
    return 1;
}

static int fx_skin_video(const ViewerSkin *skin) {
    return (skin->flags2 & VS2_VIDEO) && skin->video >= 0 ? skin->video : -1;
}

int skin_fx_apply_video(Mesh *mesh, const int video, const int projection, const int linear) {
    if (!mesh || !mesh->material || mesh->material->vtable != &basic_material_vtable) return 0;
    BasicMaterial *material = (BasicMaterial *) mesh->material;
    if (!fx_video(mesh, material, video, linear || projection)) return 0;
    if (projection) {
        material->projection_uv = 1;
        material->double_sided = 1; // getMat's projection branch: side = DoubleSide (deob-10.0.3.js:232264)
    }
    return 1;
}

static void fx_env(BasicMaterial *material, const ViewerSkin *skin, const char *texture_key, const char *model_key) {
    if (skin->env <= 0.0f && !(skin->flags2 & VS2_SAME_ENV)) return;
    // f1117 / getMat: sameEnv: the map itself, else (texSrc || src) + "_env"; envMapIntensity env || 3
    char key[256];
    snprintf(key, sizeof(key), "%s%s", texture_key ? texture_key : model_key, (skin->flags2 & VS2_SAME_ENV) ? "" : "_env");
    material->env_map = (skin->flags2 & VS2_SAME_ENV) ? material->texture : fx_texture(key);
    material->env_map_intensity = skin->env > 0.0f ? skin->env : 3.0f;
}

void skin_fx_apply(Mesh *mesh, const ViewerSkin *skin, const char *texture_key, const char *model_key, const int glow_text, const int emissive) {
    if (!mesh || !skin || !mesh->material || mesh->material->vtable != &basic_material_vtable) return;
    BasicMaterial *material = (BasicMaterial *) mesh->material;

    // video (skin_video.h): getMat / loadTexture play videos/video_<n>.mp4 in the map; LinearFilter (the weapon and melee
    // loadMesh tFilter, deob-10.0.3.js:194628 / :194706, and the VideoTexture's own one with projection: the only video
    // items, the Mysterious Orb melee and the upgraded weapons, are both). noVideoSkins (or no playable video) strips
    // video, projection and texSrc (deob:231704-231710): getMat(src) shows the model's own texture, src + ".png".
    int projection = (skin->flags2 & VS2_PROJECTION) != 0;
    const int video = fx_skin_video(skin);
    if (video >= 0 && !fx_video(mesh, material, video, 1)) {
        projection = 0;
        texture_key = NULL;
        const unsigned int regular = model_key ? fx_texture(model_key) : 0;
        if (regular) material->texture = regular;
    }

    if (skin->color >= 0) material->color = fx_color(skin->color, material->color.w);
    if (skin->opac > 0.0f) {
        material->color.w = skin->opac;
        material->base.transparent = 1;
    }
    if (skin->flags & VS_TRANS) material->base.transparent = 1;
    if (skin->side == 2) material->double_sided = 1;
    if (skin->alpha > 0.0f) material->alpha_test = skin->alpha;
    if (skin->flags & VS_WIRE) material->base.wireframe = 1;
    if (skin->tile_s > 0.0f) material->texture_repeat = (vec2) {skin->tile_s, skin->tile_s};
    if (skin->frames > 0) {
        // loadTexture: repeat (1 / numX, numY ? 1 / numY : repeatY)
        const int num_x = skin->frames < SKIN_FX_FRAMES_PER_ROW ? skin->frames : SKIN_FX_FRAMES_PER_ROW;
        const int num_y = skin->frames > SKIN_FX_FRAMES_PER_ROW ? (skin->frames + SKIN_FX_FRAMES_PER_ROW - 1) / SKIN_FX_FRAMES_PER_ROW : 0;
        material->texture_repeat.x = 1.0f / (float) num_x;
        if (num_y) material->texture_repeat.y = 1.0f / (float) num_y; // offset.y stays 0 until the first frame step, as in v7
    }

    if (emissive) {
        // emissive: emiss || #fff, emissiveMap ((glowText && texSrc) || src) + (sameGlow ? "" : "_e")
        char key[256];
        snprintf(key, sizeof(key), "%s%s", glow_text && texture_key ? texture_key : model_key, (skin->flags & VS_SAME_GLOW) ? "" : "_e");
        // render.js loadTexture never sets emissiveMap when that file is missing: the colour then
        // glows over the whole item (one AK skin in the v7 data, and any glow the dev config adds)
        material->emissive_texture = fx_texture(key);
        material->emissive = fx_color(skin->emiss >= 0 ? skin->emiss : 0xFFFFFF, 1.0f);
        if (skin->glow_int > 0.0f) material->emissive.w = skin->glow_int; // emissInt
    }
    if (projection) {
        material->projection_uv = 1;
        material->double_sided = 1; // projection: side = DoubleSide
    }
    fx_env(material, skin, texture_key, model_key);
    fx_register(mesh, NULL, material, &mesh->transform, skin, 1);

    if (skin->flags2 & VS2_SHADER) {
        skin_fx_forget(mesh); // the BasicMaterial goes away (the rAnim spin stays below; a video's user with it)
        fx_use_shadertoy(mesh, skin);
        fx_register(mesh, NULL, NULL, &mesh->transform, skin, 0);
    }
}

void skin_fx_apply_gltf(GltfInstance *model, const ViewerSkin *skin, const char *model_key) {
    if (!model || !skin) return;
    if (skin->shader_info) fx_custom_gltf(model, skin); // first: the effect entries below hold only the BasicMaterials left
    for (size_t i = 0; i < model->mesh_count; i++) {
        Mesh *mesh = model->meshes[i];
        if (!mesh || !mesh->material || mesh->material->vtable != &basic_material_vtable) continue;
        BasicMaterial *material = (BasicMaterial *) mesh->material;
        // f1117: color (not #ffffff), opacity != 1 (transparent), pulsT, rgb, env
        if (skin->color >= 0 && skin->color != 0xFFFFFF) material->color = fx_color(skin->color, material->color.w);
        if (skin->opac > 0.0f && skin->opac != 1.0f) {
            material->color.w = skin->opac;
            material->base.transparent = 1;
        }
        fx_env(material, skin, NULL, model_key);
        if (skin->puls_t != 0.0f || (skin->flags & VS_RGB)) {
            SkinFxEntry *entry = fx_add();
            if (!entry) continue;
            entry->model = model;
            entry->material = material;
            entry->pulse = skin->puls_t != 0.0f;
            entry->min_glo = skin->min_glo;
            entry->rgb = (skin->flags & VS_RGB) != 0;
            entry->rgb_type = (int) skin->rgb_t;
            entry->rgb_s = skin->rgb_s != 0.0f ? skin->rgb_s : SKIN_FX_RGB_SPEED;
        }
    }
    if (skin->r_anim != 0.0f) fx_register(NULL, model, NULL, &model->root, skin, 0);
}

void skin_fx_apply_dye(Mesh *mesh, const ViewerSkin *dye) {
    if (!mesh || !dye || !mesh->material || mesh->material->vtable != &basic_material_vtable) return;
    // the body parts' getMat takes the dye's shaderNum and customShaderInfo (deob-10.0.4.js:173555-173556): a custom
    // shader (the Ghillie Suit's 16) replaces the part's material
    if ((dye->flags2 & VS2_SHADER) && dye->shader_num >= SKIN_FX_CUSTOM_FIRST && (dye->shader_info || custom_is_graph(dye->shader_num))) {
        fx_use_shadertoy(mesh, dye);
        if (mesh->material->vtable != &basic_material_vtable) return;
    }
    BasicMaterial *material = (BasicMaterial *) mesh->material;
    // video (skin_video.h): the body part's map is the video (getMat "/videos/video_<n>", deob-10.0.3.js:232046-232048);
    // no tFilter there, so NearestFilter unless projection keeps the VideoTexture's LinearFilter (deob:231863-231866).
    // noVideoSkins (or no playable video) strips video, projection and texSrc (deob:231704-231710): getMat("default"),
    // the dye's colours only.
    const int video = fx_skin_video(dye);
    const int playing = video >= 0 && fx_video(mesh, material, video, (dye->flags2 & VS2_PROJECTION) != 0);
    const int projection = (video < 0 || playing) && (dye->flags2 & VS2_PROJECTION);
    const char *tex = video >= 0 ? NULL : dye->tex; // with a video, texSrc is only loadTexture's cache key
    if (playing && dye->tile_s > 0.0f) material->texture_repeat = (vec2) {dye->tile_s, dye->tile_s}; // repeatX/Y: tileS
    // genHead / genBody / genLeg / genArms getMat: texSrc = dye.tex, colour, opacity, emissive
    // (glow || sameGlow: emiss || #fff, emissiveMap of that texture), rgb (vertexColors + emissive hue)
    if (tex && !(dye->flags & VS_TEX_NUM)) {
        const unsigned int texture = fx_texture(tex);
        if (texture) material->texture = texture;
        if (dye->tile_s > 0.0f) material->texture_repeat = (vec2) {dye->tile_s, dye->tile_s};
    }
    if (dye->color >= 0) {
        const vec4 tint = fx_color(dye->color, 1.0f);
        material->color.x *= tint.x;
        material->color.y *= tint.y;
        material->color.z *= tint.z;
    }
    if (dye->opac > 0.0f) {
        material->color.w = dye->opac;
        material->base.transparent = 1;
    }
    if ((dye->flags & (VS_GLOW | VS_SAME_GLOW)) && tex && !(dye->flags & VS_TEX_NUM)) {
        char key[256];
        snprintf(key, sizeof(key), "%s%s", tex, (dye->flags & VS_SAME_GLOW) ? "" : "_e");
        material->emissive_texture = fx_texture(key); // missing: the colour glows unmapped (as above)
        material->emissive = fx_color(dye->emiss >= 0 ? dye->emiss : 0xFFFFFF, 1.0f);
    }
    if (projection) {
        material->projection_uv = 1;
        material->double_sided = 1; // getMat's projection branch: side = DoubleSide (deob-10.0.3.js:232264)
    }
    if (dye->flags & VS_RGB) {
        material->emissive = (vec4) {1.0f, 0.0f, 0.0f, 1.0f}; // hue 0 until the first tick
        SkinFxEntry *entry = fx_add();
        if (!entry) return;
        entry->mesh = mesh;
        entry->material = material;
        entry->rgb = 1;
        entry->rgb_type = (int) dye->rgb_t;
        entry->rgb_s = dye->rgb_s != 0.0f ? dye->rgb_s : SKIN_FX_RGB_SPEED;
    }
}

// The per-copy paint of a worn store skin: v10.0.3 loadMesh / loadAnimMesh's paint branch (deob-10.0.3.js:232196-232210,
// 233822-233850): the material gets paintColor (the colour, #ffffff for a shader), paintIntensity 1 (0 when there is
// neither a colour nor a known shader), the paintMaskMap of the skin's paintMask, and the shader graph (basic.frag
// paint_apply). The metal shaders sample melee_190_env.png (deob:105842), loaded once.
static unsigned int g_paint_env_texture;
static int g_paint_env_tried;

static void fx_paint_material(BasicMaterial *material, const WornSkinPaint paint, const ViewerSkin *skin) {
    const int known_shader = paint.shader >= 0 && paint.shader <= 5;
    const vec4 rgb = fx_color(paint.color >= 0 ? paint.color : 0xFFFFFF, 1.0f);
    material->paint_enabled = 1;
    material->paint_color = (vec3) {rgb.x, rgb.y, rgb.z};
    material->paint_intensity = paint.color >= 0 || known_shader ? 1.0f : 0.0f;
    material->paint_shader = paint.color >= 0 ? -1 : known_shader ? paint.shader : 99; // 99: an unknown shader, unpainted
    material->paint_mask_texture = 0;
    material->paint_env_texture = 0;
    if (skin && skin->paint_mask) {
        char key[256];
        snprintf(key, sizeof(key), "%s", skin->paint_mask);
        if (skin->paint_mask_ext && strcmp(skin->paint_mask_ext, "png") != 0) {
            // fx_texture appends ".png": another extension goes through its own path
            char rel[300];
            snprintf(rel, sizeof(rel), "textures/%s.%s", skin->paint_mask, skin->paint_mask_ext);
            char *path = concat(assets_path(), rel);
            if (path) {
                ensure_asset_file(path);
                FILE *file = fopen(path, "rb");
                if (file) {
                    fclose(file);
                    material->paint_mask_texture = load_texture(path);
                }
                free(path);
            }
        } else {
            material->paint_mask_texture = fx_texture(key);
        }
    }
    if (material->paint_shader >= 2 && material->paint_shader <= 4) {
        if (!g_paint_env_tried) {
            g_paint_env_tried = 1;
            g_paint_env_texture = fx_texture("melee/melee_190_env");
        }
        material->paint_env_texture = g_paint_env_texture;
    }
}

// Probe (two-client paint e2e, client/tests/paint_shader_two_client_e2e.py): one line per painted mesh with the paint state
// the material now carries. Only a painted mesh logs, so any other player's line is a paint their client drew.
static void fx_paint_probe(const BasicMaterial *material) {
    fprintf(stderr, "[paint] mesh enabled=%d shader=%d\n", material->paint_enabled, material->paint_shader);
}

void skin_fx_apply_paint(Mesh *mesh, const WornSkinPaint paint, const ViewerSkin *skin) {
    if (!mesh || !mesh->material || mesh->material->vtable != &basic_material_vtable) return;
    if (paint.color < 0 && paint.shader < 0) return;
    fx_paint_material((BasicMaterial *) mesh->material, paint, skin);
    fx_paint_probe((const BasicMaterial *) mesh->material);
}

// v10.0.3 deob-10.0.3.js:233833-233837: paintTargets true paints every mesh; a list paints the meshes whose name is in it.
// paint_targets is that list joined by "," (NULL = every mesh).
static int fx_paint_target(const char *targets, const char *name) {
    if (!targets || !*targets) return 1;
    if (!name || !*name) return 0;
    const size_t n = strlen(name);
    for (const char *p = targets; *p;) {
        const char *end = strchr(p, ',');
        const size_t len = end ? (size_t) (end - p) : strlen(p);
        if (len == n && !strncmp(p, name, n)) return 1;
        if (!end) break;
        p = end + 1;
    }
    return 0;
}

void skin_fx_apply_paint_gltf(GltfInstance *model, const WornSkinPaint paint, const ViewerSkin *skin) {
    if (!model || (paint.color < 0 && paint.shader < 0)) return;
    for (size_t i = 0; i < model->mesh_count; i++) {
        Mesh *mesh = model->meshes[i];
        if (!mesh || !mesh->material || mesh->material->vtable != &basic_material_vtable) continue;
        if (!fx_paint_target(skin ? skin->paint_targets : NULL, gltf_instance_mesh_name(model, i))) continue;
        fx_paint_material((BasicMaterial *) mesh->material, paint, skin);
        fx_paint_probe((const BasicMaterial *) mesh->material);
    }
}

// ---------------------------------------------------------------- wear (v10.0.6)

int skin_fx_wear_key(const ViewerSkin *skin, char *out, const size_t n) {
    if (!out || !n) return 0;
    out[0] = 0;
    // deob-10.0.6.js:222889: a typed item (type > 0); a weapon skin's is weapons/<src>[_mid]_wear (:222885), not drawn
    if (!skin || skin->type <= 0 || skin->id < 0) return 0;
    const int t = skin->asset_type > 0 ? skin->asset_type : skin->type;
    if (t < 0 || t >= g_viewer_type_count) return 0;
    const int w = snprintf(out, n, "%s%d_wear", g_viewer_types[t], skin->id);
    return w > 0 && (size_t) w < n;
}

static void fx_wear_material(BasicMaterial *material, const unsigned int texture, const double wear) {
    material->wear_texture = texture;
    material->wear_level = (float) wear;
}

// one map per key, loaded once (a worn item's meshes and every player wearing it share it)
static unsigned int fx_wear_texture(const char *key) {
    static struct {
        char key[96];
        unsigned int texture;
    } cache[64];
    static int count;
    for (int i = 0; i < count; i++)
        if (!strcmp(cache[i].key, key)) return cache[i].texture;
    const unsigned int texture = fx_texture(key); // 0 when the host has no such map: no wear drawn
    if (count < (int) (sizeof(cache) / sizeof(cache[0]))) {
        snprintf(cache[count].key, sizeof(cache[count].key), "%s", key);
        cache[count++].texture = texture;
    }
    return texture;
}

void skin_fx_apply_wear(Mesh *mesh, const ViewerSkin *skin, const double wear) {
    if (!mesh || !mesh->material || mesh->material->vtable != &basic_material_vtable || wear < 0.0) return;
    char key[96];
    if (!skin_fx_wear_key(skin, key, sizeof(key))) return;
    const unsigned int texture = fx_wear_texture(key);
    if (texture) fx_wear_material((BasicMaterial *) mesh->material, texture, wear);
}

void skin_fx_apply_wear_gltf(GltfInstance *model, const ViewerSkin *skin, const double wear) {
    if (!model || wear < 0.0) return;
    char key[96];
    if (!skin_fx_wear_key(skin, key, sizeof(key))) return;
    const unsigned int texture = fx_wear_texture(key);
    if (!texture) return;
    for (size_t i = 0; i < model->mesh_count; i++) {
        Mesh *mesh = model->meshes[i];
        if (mesh && mesh->material && mesh->material->vtable == &basic_material_vtable) fx_wear_material((BasicMaterial *) mesh->material, texture, wear);
    }
}

void skin_fx_forget(const Mesh *mesh) {
    if (!mesh) return;
    for (int i = g_fx.count - 1; i >= 0; i--) {
        if (g_fx.entries[i].mesh != mesh) continue;
        if (g_fx.entries[i].video) skin_video_release(g_fx.entries[i].video - 1);
        g_fx.entries[i] = g_fx.entries[--g_fx.count];
    }
}

void skin_fx_forget_gltf(const GltfInstance *model) {
    if (!model) return;
    for (int i = g_fx.count - 1; i >= 0; i--) {
        if (g_fx.entries[i].model == model) g_fx.entries[i] = g_fx.entries[--g_fx.count];
    }
}

// ---------------------------------------------------------------- per frame

void skin_fx_tick(const float dt_ms, const int texture_anim, const int basic_lighting) {
    g_shader_frame++;
    // video textures follow their playback whatever textureAnim says (v10's VideoTexture is not one of the
    // movTextures / frameMats the setting freezes); gltf_clock: --render-scenario pins it
    skin_video_frame(gltf_clock());
    if (!texture_anim || dt_ms <= 0.0f) return;

    g_fx.puls_val += dt_ms * SKIN_FX_PULSE_SPEED;
    const float pulse = (sinf(g_fx.puls_val) / 2.0f + 0.5f) * 1.5f;

    for (int i = 0; i < g_fx.count; i++) {
        SkinFxEntry *e = &g_fx.entries[i];
        BasicMaterial *m = e->material;

        if (m && e->frames > 0) {
            e->frame_left -= dt_ms;
            if (e->frame_left <= 0.0f) {
                e->frame_left = e->frame_t;
                e->cur = (e->cur + 1) % e->frames;
                // offset.x = cur % numX / numX, offset.y = (numY - floor(cur / numX) - 1) / numY (repeat units here)
                m->texture_offset.x = (float) (e->cur % e->num_x);
                if (e->num_y) m->texture_offset.y = (float) (e->num_y - e->cur / e->num_x - 1);
            }
        }
        if (m && e->mov_t != 0.0f) {
            // tex.offset[movD == 0 ? "x" : "y"] += movT * delta (texture units; ours are repeat units)
            if (e->mov_d == 0) m->texture_offset.x += e->mov_t * dt_ms / (m->texture_repeat.x != 0.0f ? m->texture_repeat.x : 1.0f);
            else m->texture_offset.y += e->mov_t * dt_ms / (m->texture_repeat.y != 0.0f ? m->texture_repeat.y : 1.0f);
        }
        if (m && e->rot_t != 0.0f) m->texture_rotation += e->rot_t * dt_ms;
        if (m && e->pulse) m->emissive.w = pulse + e->min_glo; // emissiveIntensity
        if (m && e->rgb) {
            e->rgb_dlt += dt_ms * e->rgb_s;
            if (e->rgb_dlt >= 360.0f) e->rgb_dlt = 0.0f;
            const vec4 hue = fx_color(skin_fx_hex_from_hue((int) roundf(e->rgb_dlt)), 1.0f);
            if (e->rgb_type || basic_lighting) {
                m->color = (vec4) {hue.x, hue.y, hue.z, m->color.w};
            } else {
                m->emissive = (vec4) {hue.x, hue.y, hue.z, m->emissive.w > 0.0f ? m->emissive.w : 1.0f};
            }
        }
        if (e->spin && e->r_anim != 0.0f) {
            if (e->r_bob_type) {
                e->r_bob += e->r_anim * dt_ms;
                const float v = e->r_org + sinf(e->r_bob) * e->r_mlt;
                if (e->r_axis == 'x') e->spin->position.x = v;
                else if (e->r_axis == 'z') e->spin->position.z = v;
                else e->spin->position.y = v;
            } else if (e->r_axis == 'x') {
                e->spin->rotation.x += e->r_anim * dt_ms;
            } else if (e->r_axis == 'z') {
                e->spin->rotation.z += e->r_anim * dt_ms;
            } else {
                e->spin->rotation.y += e->r_anim * dt_ms;
            }
        }
    }
}
