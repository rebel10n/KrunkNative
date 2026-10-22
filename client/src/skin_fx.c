// Krunker v7.1.4 store-item material effects (client/include/skin_fx.h).
#include <gl.h>
#include <skin_fx.h>
#include <gltf.h>
#include "viewer/viewer_store.h"
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
} SkinFxEntry;

static struct {
    SkinFxEntry *entries;
    int count, capacity;
    float puls_val;               // RENDER.pulsVal, shared by every pulsing material
} g_fx;

// ---------------------------------------------------------------- helpers

static unsigned int fx_texture(const char *key) {
    char rel[256];
    snprintf(rel, sizeof(rel), "textures/%s.png", key);
    char *path = concat(assets_path(), rel);
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

// getMat: shaderNum != null && shaders[shaderNum]: a ShaderToyMaterial instead of the item's own
static void fx_use_shadertoy(Mesh *mesh, const ViewerSkin *skin) {
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

// ---------------------------------------------------------------- applying

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
    if (skin->flags2 & VS2_PROJECTION) {
        material->projection_uv = 1;
        material->double_sided = 1; // projection: side = DoubleSide
    }
    fx_env(material, skin, texture_key, model_key);
    fx_register(mesh, NULL, material, &mesh->transform, skin, 1);

    if (skin->flags2 & VS2_SHADER) {
        skin_fx_forget(mesh); // the BasicMaterial goes away (the rAnim spin stays below)
        fx_use_shadertoy(mesh, skin);
        fx_register(mesh, NULL, NULL, &mesh->transform, skin, 0);
    }
}

void skin_fx_apply_gltf(GltfInstance *model, const ViewerSkin *skin, const char *model_key) {
    if (!model || !skin) return;
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
    BasicMaterial *material = (BasicMaterial *) mesh->material;
    // genHead / genBody / genLeg / genArms getMat: texSrc = dye.tex, colour, opacity, emissive
    // (glow || sameGlow: emiss || #fff, emissiveMap of that texture), rgb (vertexColors + emissive hue)
    if (dye->tex && !(dye->flags & VS_TEX_NUM)) {
        const unsigned int texture = fx_texture(dye->tex);
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
    if ((dye->flags & (VS_GLOW | VS_SAME_GLOW)) && dye->tex && !(dye->flags & VS_TEX_NUM)) {
        char key[256];
        snprintf(key, sizeof(key), "%s%s", dye->tex, (dye->flags & VS_SAME_GLOW) ? "" : "_e");
        material->emissive_texture = fx_texture(key); // missing: the colour glows unmapped (as above)
        material->emissive = fx_color(dye->emiss >= 0 ? dye->emiss : 0xFFFFFF, 1.0f);
    }
    if (dye->flags2 & VS2_PROJECTION) material->projection_uv = 1;
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

void skin_fx_apply_paint(Mesh *mesh, const WornSkinPaint paint, const ViewerSkin *skin) {
    if (!mesh || !mesh->material || mesh->material->vtable != &basic_material_vtable) return;
    if (paint.color < 0 && paint.shader < 0) return;
    fx_paint_material((BasicMaterial *) mesh->material, paint, skin);
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
    }
}

void skin_fx_forget(const Mesh *mesh) {
    if (!mesh) return;
    for (int i = g_fx.count - 1; i >= 0; i--) {
        if (g_fx.entries[i].mesh == mesh) g_fx.entries[i] = g_fx.entries[--g_fx.count];
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
