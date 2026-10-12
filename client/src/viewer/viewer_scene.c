// The viewer page's three.js scene (Krunker v3.1.1 src/viewer.js), on KrunkNative's renderer:
//
// - Z(): a preview Player (new Player(...), init, setClass, generateMeshes(d, false, true))
//   built with player_generate_meshes(), then the parts players.js generateMeshes gives a
//   preview player and player_mesh.c does not (weapon skins incl. remodels and glow, melee
//   skins, the picked sight attachment, cosmetic glow / spin / frame animation) applied to
//   its meshes here;
// - F(C): one weapon / melee item on its own (cosmetic=<index>), showcase=<index>;
// - U(): the "Viewer" (six spot lights on the axes) and "In-game" (sun) lighting plus the
//   ambient light, through a variant of basic.frag patched at load time (see
//   viewer_program_create) so the basic material can take those lights;
// - the OrbitControls camera (PerspectiveCamera(60, aspect, 0.01, 50));
// - the #container background linear-gradient(#7d8fa3, #1b1c1e), drawn here under the scene
//   because the page is composited over the GL canvas (the page's container stays clear).
//
// Every GL object made here is freed here; the private Game / Map / Player never touch the
// client's game.
#include <gl.h>
#include "viewer_internal.h"
#include <gltf.h>
#include <fast_obj.h>
#include <stb_image.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- viewer.js constants
#define VIEWER_FOV_DEG 60.0f                 // new PerspectiveCamera(60, aspect, 0.01, 50)
#define VIEWER_NEAR 0.01f
#define VIEWER_FAR 50.0f
#define COSMETIC_FOV_DEG 45.0f               // cosmetic page: PerspectiveCamera(45, 1, 0.1, 100) at x 30
#define COSMETIC_NEAR 0.1f
#define COSMETIC_FAR 100.0f
#define COSMETIC_CANVAS 512.0f               // renderer.setSize(512, 512)
#define AMBIENT_COLOR 0x97A0A8               // new AmbientLight(9937064)
#define SUN_COLOR 0xF2F7FC                   // new DirectionalLight(15923452, 1.2)
#define SUN_INTENSITY 1.2f
#define SUN_PITCH (-0.3f * (float) M_PI)     // e = PI * -0.3, t = PI * 2 * -0.25, at lightDistance
#define SUN_YAW (2.0f * (float) M_PI * -0.25f)
#define FILL_INTENSITY 0.5f                  // cosmetic page: DirectionalLight(0xffffff, 0.5) at (3, 1, 0)
#define GRADIENT_TOP 0x7d8fa3                // linear-gradient(#7d8fa3, #1b1c1e)
#define GRADIENT_BOTTOM 0x1b1c1e
#define ORBIT_EPS 0.000001f                  // Spherical.makeSafe
#define ORBIT_ZOOM 0.95f                     // Math.pow(0.95, zoomSpeed)
#define ORBIT_KEY_PAN 7.0f                   // keyPanSpeed
#define PULSE_SPEED 0.003f                   // render.js: pulsVal += delta * 0.003
#define ANIM_STEP_SPEED (0.015f * 0.25f)     // playerStep(d, delta * 0.015): stepVal += dt * stepMlt
#define VIEWER_MAX_ANIMS 64
#define VIEWER_MAX_ITEMS 16
#define VIEWER_MAX_OVERRIDES 64

typedef struct {
    Mesh *mesh;
    BasicMaterial *material;
    int frames;
    float frame_t, frame_left;
    int frame;
    float mov_t;
    int mov_d;
    float puls_t;
    float r_anim;
    char r_axis;
} ViewerAnim;

typedef struct {
    char *path;        // texture / model cache key (full asset path)
    int is_model;
} ViewerOverride;

static struct {
    int ready;
    Game game;
    cJSON *map_json;
    Scene *scene;
    Camera camera;
    Player *player;
    int step_dir;

    // F() / showcase items: meshes and their holder transforms
    Mesh *items[VIEWER_MAX_ITEMS];
    int item_count;
    MeshTransform *holders[VIEWER_MAX_ITEMS];
    int holder_count;
    Player *showcase_player;

    ViewerAnim anims[VIEWER_MAX_ANIMS];
    int anim_count;
    float puls_val;

    // orbit controls
    vec3 target;
    int orbit_state;   // -1 none, 0 rotate, 1 dolly, 2 pan
    float last_x, last_y;

    // lights (U)
    int light_mode;    // 0 sun, 1 six axis spot lights
    int fill_light;    // cosmetic page fill light

    unsigned int program;          // patched basic program, 0 = use the stock one
    unsigned int gradient_program;
    unsigned int empty_vao;

    ViewerOverride overrides[VIEWER_MAX_OVERRIDES];
    int override_count;

    float render_rotation;         // renderHat & co: d.objInstances.rotation.y
    int hide_primary_geo;          // d.weaponGeos[0].visible = false
} vs;

// ---------------------------------------------------------------- small helpers

static char *asset(const char *dir, const char *key, const char *ext) {
    const size_t n = strlen(assets_path()) + strlen(dir) + strlen(key) + strlen(ext) + 2;
    char *path = malloc(n);
    if (path) snprintf(path, n, "%s%s/%s%s", assets_path(), dir, key, ext);
    return path;
}

static unsigned int texture_for(const char *key) {
    char *path = asset("textures", key, ".png");
    const unsigned int texture = path ? load_texture(path) : 0;
    free(path);
    return texture;
}

static Geometry *model_for(const char *key) {
    char *path = asset("models", key, ".obj");
    Geometry *geometry = path ? load_obj_model(path, 0) : NULL;
    free(path);
    return geometry;
}

static int file_exists(const char *dir, const char *key, const char *ext) {
    char *path = asset(dir, key, ext);
    if (!path) return 0;
    ensure_asset_file(path);
    FILE *file = fopen(path, "rb");
    if (file) fclose(file);
    free(path);
    return file != NULL;
}

// x of the model's bounding-box centre (loadMesh centerZ: holder.translateZ(center.x * scale))
static float model_center_x(const char *key) {
    char *path = asset("models", key, ".obj");
    if (!path) return 0.0f;
    ensure_asset_file(path);
    fastObjMesh *mesh = fast_obj_read(path);
    free(path);
    if (!mesh) return 0.0f;

    float lo = INFINITY, hi = -INFINITY;
    for (unsigned int i = 1; i < mesh->position_count; i++) {
        const float x = mesh->positions[i * 3];
        if (x < lo) lo = x;
        if (x > hi) hi = x;
    }
    fast_obj_destroy(mesh);
    return lo <= hi ? (lo + hi) * 0.5f : 0.0f;
}

static vec4 color_vec(const int hex, const float alpha) {
    return (vec4) {(float) ((hex >> 16) & 255) / 255.0f, (float) ((hex >> 8) & 255) / 255.0f, (float) (hex & 255) / 255.0f, alpha};
}

static int skin_has_glow(const ViewerSkin *skin) {
    return skin && (skin->flags & (VS_GLOW | VS_SAME_GLOW));
}

static void anim_add(Mesh *mesh, const ViewerSkin *skin) {
    if (!skin || vs.anim_count >= VIEWER_MAX_ANIMS) return;
    if (!skin->frames && !skin->mov_t && !skin->puls_t && !skin->r_anim) return;

    ViewerAnim *anim = &vs.anims[vs.anim_count++];
    memset(anim, 0, sizeof(*anim));
    anim->mesh = mesh;
    anim->material = (BasicMaterial *) mesh->material;
    anim->frames = skin->frames;
    anim->frame_t = anim->frame_left = (float) skin->frame_t;
    anim->mov_t = skin->mov_t;
    anim->puls_t = skin->puls_t;
    anim->r_anim = skin->r_anim;
    anim->r_axis = skin->r_anim_axis ? skin->r_anim_axis : 'y';

    if (anim->frames > 0) anim->material->texture_repeat.x = 1.0f / (float) anim->frames; // repeat.x = 1 / frames
    if (anim->r_anim) mesh->transform.rotation_order = ROTATION_ORDER_YXZ; // rotateX/Z after the y turn
}

// getMat(): the material options a store item carries (glow, opacity, side, colour, wireframe)
static void material_apply_skin(BasicMaterial *material, const ViewerSkin *skin, const char *texture_key, const char *model_key, const int glow_text, const int emissive) {
    if (!skin) return;

    if (skin->color >= 0) material->color = color_vec(skin->color, material->color.w);
    if (skin->opac > 0.0f) {
        material->color.w = skin->opac;
        material->base.transparent = 1;
    }
    if (skin->flags & VS_TRANS) material->base.transparent = 1;
    if (skin->side == 2) material->double_sided = 1;
    if (skin->alpha > 0.0f) material->alpha_test = skin->alpha;
    if (skin->flags & VS_WIRE) material->base.wireframe = 1;
    if (skin->tile_s > 0.0f) material->texture_repeat = (vec2) {skin->tile_s, skin->tile_s};

    if (emissive) {
        // emissive: emiss || 0xffffff, emissiveMap = ((glowText && texSrc) || src) + (sameGlow ? "" : "_e")
        material->emissive = color_vec(skin->emiss >= 0 ? skin->emiss : 0xFFFFFF, 1.0f);
        char key[256];
        snprintf(key, sizeof(key), "%s%s", glow_text && texture_key ? texture_key : model_key, (skin->flags & VS_SAME_GLOW) ? "" : "_e");
        material->emissive_texture = file_exists("textures", key, ".png") ? texture_for(key) : 0;
        if (!material->emissive_texture) material->emissive = (vec4) {0};
    }
}

static Mesh *mesh_new(Geometry *geometry, const unsigned int texture) {
    BasicMaterial *material = basic_material_init();
    if (!material) return NULL;
    material->texture = texture;
    if (vs.program) material->base.program = vs.program;
    return mesh_init(geometry, (Material *) material);
}

// replace a player mesh's model + texture in place (its transform and parent stay)
static void mesh_reskin(Mesh *mesh, Geometry *geometry, const unsigned int texture) {
    if (!mesh || !geometry) return;
    mesh->geometry = geometry;
    ((BasicMaterial *) mesh->material)->texture = texture;
}

// ---------------------------------------------------------------- shaders

static unsigned int compile_stage(const unsigned int type, const char *source) {
#if defined(KN_GLES) || defined(__APPLE__) // same version rewrite as shader.c
#ifdef KN_GLES
    static const char header[] = "#version 300 es\nprecision highp float; precision highp int; precision highp sampler2D;\n";
#else
    static const char header[] = "#version 410 core\n";
#endif
    const char *rest = !strncmp(source, "#version", 8) && strchr(source, '\n') ? strchr(source, '\n') + 1 : source;
    const char *parts[2] = {header, rest};
    const unsigned int shader = glCreateShader(type);
    glShaderSource(shader, 2, parts, NULL);
#else
    const unsigned int shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
#endif
    glCompileShader(shader);

    int ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        fprintf(stderr, "[viewer] shader compile error: %s\n", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static unsigned int link_program(const char *vertex, const char *fragment) {
    const unsigned int vs_id = compile_stage(GL_VERTEX_SHADER, vertex);
    const unsigned int fs_id = vs_id ? compile_stage(GL_FRAGMENT_SHADER, fragment) : 0;
    if (!vs_id || !fs_id) {
        if (vs_id) glDeleteShader(vs_id);
        return 0;
    }

    const unsigned int program = glCreateProgram();
    glAttachShader(program, vs_id);
    glAttachShader(program, fs_id);
    glLinkProgram(program);
    glDeleteShader(vs_id);
    glDeleteShader(fs_id);

    int ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(program, sizeof(log), NULL, log);
        fprintf(stderr, "[viewer] program link error: %s\n", log);
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

static char *read_asset_text(const char *rel) {
    char *path = concat(assets_path(), rel);
    if (!path) return NULL;
    ensure_asset_file(path);
    unsigned char *data = NULL;
    size_t length = 0;
    const int failed = read_file(path, &data, &length);
    free(path);
    if (failed || !data) return NULL;
    char *text = malloc(length + 1);
    if (text) {
        memcpy(text, data, length);
        text[length] = 0;
    }
    free(data);
    return text;
}

// Replace the first `from` in *text with `to` (realloc'd). 0 when `from` is missing.
static int text_replace(char **text, const char *from, const char *to) {
    char *at = strstr(*text, from);
    if (!at) return 0;
    const size_t head = (size_t) (at - *text), from_n = strlen(from), to_n = strlen(to), total = strlen(*text);
    char *out = malloc(total - from_n + to_n + 1);
    if (!out) return 0;
    memcpy(out, *text, head);
    memcpy(out + head, to, to_n);
    memcpy(out + head + to_n, at + from_n, total - head - from_n + 1);
    free(*text);
    *text = out;
    return 1;
}

static int text_replace_all(char **text, const char *from, const char *to) {
    int count = 0;
    char marker[] = "\x01";
    while (text_replace(text, from, marker)) count++;
    while (text_replace(text, marker, to)) {}
    return count;
}

// basic.frag with the viewer's lights: viewer_light_mode 1 = the six "Viewer" spot lights on
// the axes (cone 60 degrees, no falloff: each lights the faces turned to it, so the direct
// term is |n.x| + |n.y| + |n.z|), 2 = the sun plus the cosmetic page's fill light; and the
// material model (viewer_lm: 1 Lambert, 2 Phong) chosen per draw instead of by the lighting
// setting. The map's point lights never reach the viewer.
static unsigned int viewer_program_create(void) {
    char *vert = read_asset_text("shaders/basic.vert");
    char *frag = read_asset_text("shaders/basic.frag");
    unsigned int program = 0;

    if (vert && frag) {
        int ok = 1;
        ok &= text_replace_all(&frag, "lighting_mode ==", "viewer_lm ==") > 0;
        ok &= text_replace(&frag, "uniform float light_intensity;",
                           "uniform float light_intensity;\nuniform int viewer_lm;\nuniform int viewer_light_mode;\n"
                           "uniform vec3 viewer_light2_direction;\nuniform vec3 viewer_light2_color;");
        ok &= text_replace(&frag, "vec3 direct = light * dot_nl;",
                           "vec3 direct = viewer_light_mode == 1 ? light * (abs(normal.x) + abs(normal.y) + abs(normal.z)) : light * dot_nl;\n"
                           "    vec3 viewer_l2 = normalize(viewer_light2_direction);\n"
                           "    float viewer_nl2 = viewer_light_mode == 2 ? clamp(dot(normal, viewer_l2), 0.0, 1.0) : 0.0;\n"
                           "    direct += viewer_light2_color * viewer_nl2;");
        ok &= text_replace(&frag, "outgoing += light * dot_nl * krunker_blinn_phong(light_dir, normal, view_dir);",
                           "outgoing += light * dot_nl * krunker_blinn_phong(light_dir, normal, view_dir);\n"
                           "        if (viewer_light_mode == 2) outgoing += viewer_light2_color * viewer_nl2 * krunker_blinn_phong(viewer_l2, normal, view_dir);");
        text_replace(&frag, "i < point_light_count", "i < 0 * point_light_count");
        if (ok) program = link_program(vert, frag);
        else fprintf(stderr, "[viewer] basic.frag changed: viewer lights unavailable (stock basic program)\n");
    }

    free(vert);
    free(frag);
    return program;
}

static const char *const GRADIENT_VERT =
    "#version 450 core\n"
    "void main() {\n"
    "    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
    "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

// linear-gradient(top, bottom) between fb rows y0 (top colour) and y1 (bottom colour), from the top
static const char *const GRADIENT_FRAG =
    "#version 450 core\n"
    "uniform vec4 top_color;\n"
    "uniform vec4 bottom_color;\n"
    "uniform vec3 span;\n" // y0, y1, framebuffer height
    "out vec4 frag_color;\n"
    "void main() {\n"
    "    float y = span.z - gl_FragCoord.y;\n"
    "    float t = clamp((y - span.x) / max(span.y - span.x, 1.0), 0.0, 1.0);\n"
    "    frag_color = mix(top_color, bottom_color, t);\n"
    "}\n";

static void draw_gradient(const int fb_h, const float y0, const float y1) {
    if (!vs.gradient_program) return;
    const vec4 top = color_vec(GRADIENT_TOP, 1.0f), bottom = color_vec(GRADIENT_BOTTOM, 1.0f);

    glUseProgram(vs.gradient_program);
    g_active_shader = vs.gradient_program;
    glUniform4f(glGetUniformLocation(vs.gradient_program, "top_color"), top.x, top.y, top.z, top.w);
    glUniform4f(glGetUniformLocation(vs.gradient_program, "bottom_color"), bottom.x, bottom.y, bottom.z, bottom.w);
    glUniform3f(glGetUniformLocation(vs.gradient_program, "span"), y0, y1, (float) fb_h);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glBindVertexArray(vs.empty_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
}

// ---------------------------------------------------------------- lights (U)

void vscene_update_lights(void) {
    // "Viewer" (and not the cosmetic page): six SpotLights; "In-game" or the cosmetic page: the sun
    vs.light_mode = !strcmp(g_viewer.e.lighting.str, "Viewer") && !g_viewer.cosmetic_set ? 1 : 0;
    vs.fill_light = g_viewer.cosmetic_set;
}

static void lights_apply(RenderLighting *saved, const int phong) {
    *saved = g_render_lighting;

    g_render_lighting.ambient_color = color_vec(AMBIENT_COLOR, 1.0f);
    g_render_lighting.ambient_intensity = 1.0f;
    g_render_lighting.enabled = 1;
    g_render_lighting.fog_enabled = 0;

    if (vs.light_mode == 1) {
        g_render_lighting.light_color = color_vec(0xFFFFFF, 1.0f);
        g_render_lighting.light_intensity = 1.0f;
        g_render_lighting.light_direction = (vec3) {0.0f, 1.0f, 0.0f};
    } else {
        // DirectionalLight position (lightDistance * cos t, * sin t * sin e, * sin t * cos e), target the origin
        vec3 dir = {cosf(SUN_YAW), sinf(SUN_YAW) * sinf(SUN_PITCH), sinf(SUN_YAW) * cosf(SUN_PITCH)};
        const float length = sqrtf(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
        dir.x /= length;
        dir.y /= length;
        dir.z /= length;
        g_render_lighting.light_color = color_vec(SUN_COLOR, 1.0f);
        g_render_lighting.light_intensity = SUN_INTENSITY;
        g_render_lighting.light_direction = dir;
    }

    if (vs.program) {
        glUseProgram(vs.program);
        g_active_shader = vs.program;
        glUniform1i(glGetUniformLocation(vs.program, "viewer_lm"), phong ? 2 : 1);
        glUniform1i(glGetUniformLocation(vs.program, "viewer_light_mode"), vs.light_mode == 1 ? 1 : vs.fill_light ? 2 : 0);
        glUniform3f(glGetUniformLocation(vs.program, "viewer_light2_direction"), 3.0f, 1.0f, 0.0f);
        glUniform3f(glGetUniformLocation(vs.program, "viewer_light2_color"), FILL_INTENSITY, FILL_INTENSITY, FILL_INTENSITY);
    }
}

// ---------------------------------------------------------------- camera / orbit controls

static void camera_look_at(const vec3 target) {
    Camera *camera = &vs.camera;
    vec3 dir = {target.x - camera->position.x, target.y - camera->position.y, target.z - camera->position.z};
    const float length = sqrtf(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (length <= 0.0f) return;
    dir.x /= length;
    dir.y /= length;
    dir.z /= length;
    camera->rotation.x = asinf(dir.y);
    camera->rotation.y = atan2f(-dir.x, -dir.z);
    camera->rotation.z = 0.0f;
}

static void camera_place(const vec3 position, const vec3 target) {
    vs.camera.position = position;
    vs.target = target;
    camera_look_at(target);
    if (getenv("KRUNKNATIVE_VIEWER_DEBUG")) {
        fprintf(stderr, "[viewer] camera (%.3f %.3f %.3f) -> (%.3f %.3f %.3f)\n", position.x, position.y, position.z, target.x, target.y, target.z);
    }
}

// d.weaponMeshes[0].position + (3, 6, 3): the primary weapon group's local position (updateMsh
// moves it to the weapon's third-person offset), used as a world point by viewer.js
static vec3 weapon_target(void) {
    vec3 p = {0.0f, 0.0f, 0.0f};
    const PlayerMesh *mesh = vs.player ? vs.player->mesh : NULL;
    if (mesh && mesh->arms && mesh->arms[0]) p = mesh->arms[0]->anchor.position;
    return (vec3) {p.x + 3.0f, p.y + 6.0f, p.z + 3.0f};
}

void vscene_camera_player(const int show_player) {
    camera_place((vec3) {15.0f, 5.0f, 0.0f}, show_player ? (vec3) {0.0f, 5.0f, 0.0f} : weapon_target());
}

void vscene_camera_weapon(void) {
    camera_place((vec3) {12.0f, 5.0f, 0.0f}, weapon_target());
}

// OrbitControls.update(): offset from the target in spherical coordinates
static void orbit_apply(const float d_theta, const float d_phi, const float scale, const vec3 pan) {
    vec3 offset = {vs.camera.position.x - vs.target.x, vs.camera.position.y - vs.target.y, vs.camera.position.z - vs.target.z};
    float radius = sqrtf(offset.x * offset.x + offset.y * offset.y + offset.z * offset.z);
    float theta = 0.0f, phi = 0.0f;
    if (radius > 0.0f) {
        theta = atan2f(offset.x, offset.z);
        phi = acosf(fmaxf(-1.0f, fminf(1.0f, offset.y / radius)));
    }

    theta += d_theta;
    phi += d_phi;
    phi = fmaxf(ORBIT_EPS, fminf((float) M_PI - ORBIT_EPS, phi));
    radius *= scale;

    vs.target.x += pan.x;
    vs.target.y += pan.y;
    vs.target.z += pan.z;

    const float sin_phi = sinf(phi);
    vs.camera.position = (vec3) {
        vs.target.x + radius * sin_phi * sinf(theta),
        vs.target.y + radius * cosf(phi),
        vs.target.z + radius * sin_phi * cosf(theta),
    };
    camera_look_at(vs.target);
}

static float viewport_height(void) {
    float w = 0.0f, h = 0.0f, dpr = 1.0f;
    html_dom_viewport(&w, &h, &dpr);
    return h > 0.0f ? h : 720.0f;
}

// pan(deltaX, deltaY) in px: perspective target distance * tan(fov / 2) * 2 * delta / clientHeight
static void orbit_pan(const float dx, const float dy) {
    const vec3 offset = {vs.camera.position.x - vs.target.x, vs.camera.position.y - vs.target.y, vs.camera.position.z - vs.target.z};
    float distance = sqrtf(offset.x * offset.x + offset.y * offset.y + offset.z * offset.z);
    distance *= tanf(vs.camera.fov / 2.0f);
    const float h = viewport_height();
    const float left = 2.0f * dx * distance / h, up = 2.0f * dy * distance / h;

    const float yaw = vs.camera.rotation.y;
    const vec3 right = {cosf(yaw), 0.0f, -sinf(yaw)};       // camera matrix column 0 (no roll)
    const vec3 forward = {-sinf(yaw), 0.0f, -cosf(yaw)};    // up x right (screenSpacePanning false)
    // panLeft(-left along right), panUp(up along forward)
    const float rx = cosf(vs.camera.rotation.x);
    (void) rx;
    orbit_apply(0.0f, 0.0f, 1.0f, (vec3) {
        -right.x * left + forward.x * up,
        0.0f,
        -right.z * left + forward.z * up,
    });
}

void vscene_orbit_begin(const int button, const int mods, const float x, const float y) {
    vs.last_x = x;
    vs.last_y = y;
    // mouseButtons LEFT rotate (ctrl / meta / shift: pan), MIDDLE dolly, RIGHT pan (GLFW 0 left, 1 right, 2 middle)
    if (button == 0) vs.orbit_state = mods & (GLFW_MOD_CONTROL | GLFW_MOD_SUPER | GLFW_MOD_SHIFT) ? 2 : 0;
    else if (button == 2) vs.orbit_state = 1;
    else if (button == 1) vs.orbit_state = 2;
    else vs.orbit_state = -1;
}

void vscene_orbit_move(const float x, const float y) {
    if (vs.orbit_state < 0) return;
    const float dx = x - vs.last_x, dy = y - vs.last_y;
    vs.last_x = x;
    vs.last_y = y;
    const float h = viewport_height();

    if (vs.orbit_state == 0) {
        orbit_apply(-2.0f * (float) M_PI * dx / h, -2.0f * (float) M_PI * dy / h, 1.0f, (vec3) {0});
    } else if (vs.orbit_state == 1) {
        if (dy > 0.0f) orbit_apply(0.0f, 0.0f, 1.0f / ORBIT_ZOOM, (vec3) {0});
        else if (dy < 0.0f) orbit_apply(0.0f, 0.0f, ORBIT_ZOOM, (vec3) {0});
    } else {
        orbit_pan(dx, dy);
    }
}

void vscene_orbit_end(void) {
    vs.orbit_state = -1;
}

int vscene_orbit_active(void) {
    return vs.orbit_state >= 0;
}

void vscene_orbit_wheel(const float dy) {
    if (vs.orbit_state != -1 && vs.orbit_state != 0) return;
    if (dy < 0.0f) orbit_apply(0.0f, 0.0f, ORBIT_ZOOM, (vec3) {0});        // deltaY < 0: dollyOut
    else if (dy > 0.0f) orbit_apply(0.0f, 0.0f, 1.0f / ORBIT_ZOOM, (vec3) {0});
}

void vscene_orbit_key(const int key) {
    switch (key) {
        case 38: orbit_pan(0.0f, ORBIT_KEY_PAN); break;   // UP
        case 40: orbit_pan(0.0f, -ORBIT_KEY_PAN); break;  // BOTTOM
        case 37: orbit_pan(ORBIT_KEY_PAN, 0.0f); break;   // LEFT
        case 39: orbit_pan(-ORBIT_KEY_PAN, 0.0f); break;  // RIGHT
        default: break;
    }
}

// ---------------------------------------------------------------- scene contents

static void items_clear(void) {
    for (int i = 0; i < vs.item_count; i++) {
        scene_remove_mesh(vs.scene, vs.items[i]);
        mesh_fini(vs.items[i]);
    }
    vs.item_count = 0;
    for (int i = 0; i < vs.holder_count; i++) free(vs.holders[i]);
    vs.holder_count = 0;
}

static MeshTransform *holder_new(MeshTransform *parent) {
    if (vs.holder_count >= VIEWER_MAX_ITEMS) return NULL;
    MeshTransform *holder = calloc(1, sizeof(MeshTransform));
    if (!holder) return NULL;
    holder->scale = (vec3) {1.0f, 1.0f, 1.0f};
    holder->parent = parent;
    vs.holders[vs.holder_count++] = holder;
    return holder;
}

static void item_add(Mesh *mesh) {
    if (!mesh || vs.item_count >= VIEWER_MAX_ITEMS) return;
    vs.items[vs.item_count++] = mesh;
    scene_add_mesh(vs.scene, mesh);
}

// player_update_meshes() (third person) poses legs[0..1] / crouched_legs[0..1] without checking
// them, but a noLegs waist item (AP Rally, ...) builds none: give such a model empty stand-ins
// (owned here, detached again before player_meshes_fini()).
static ColorCube g_no_legs[2];
static ColorCube g_no_lower_legs[2];
static Mesh g_no_leg_parts[4];
static PlayerCrouchedLeg g_no_crouched_legs[2];

static void player_legs_stand_in(PlayerMesh *mesh, const int attach) {
    for (int i = 0; i < 2; i++) {
        g_no_crouched_legs[i].upper = &g_no_leg_parts[i * 2];
        g_no_crouched_legs[i].joint = &g_no_leg_parts[i * 2 + 1];
        g_no_crouched_legs[i].lower = &g_no_lower_legs[i];
        if (attach && !mesh->legs[i]) mesh->legs[i] = &g_no_legs[i];
        if (attach && !mesh->crouched_legs[i]) mesh->crouched_legs[i] = &g_no_crouched_legs[i];
        if (!attach && mesh->legs[i] == &g_no_legs[i]) mesh->legs[i] = NULL;
        if (!attach && mesh->crouched_legs[i] == &g_no_crouched_legs[i]) mesh->crouched_legs[i] = NULL;
    }
}

static void player_free(Player *player) {
    if (!player) return;
    if (player->mesh) player_legs_stand_in(player->mesh, 0);
    if (player->mesh) {
        scene_remove_player_mesh(vs.scene, player->mesh, player->loadout_size);
        player_meshes_fini(player);
    }
    free(player->loadout);
    free(player->ammo);
    free(player->reloads);
    free(player->input_queue);
    free(player);
}

static void scene_clear(void) {
    player_free(vs.player);
    vs.player = NULL;
    player_free(vs.showcase_player);
    vs.showcase_player = NULL;
    items_clear();
    vs.anim_count = 0;
    vs.render_rotation = 0.0f;
    vs.hide_primary_geo = 0;
}

// Every basic material in the scene draws with the viewer program (player_mesh.c makes them
// with the stock one).
static void scene_use_viewer_program(void) {
    if (!vs.program) return;
    for (size_t i = 0; i < vs.scene->mesh_count; i++) {
        Material *material = vs.scene->meshes[i]->material;
        if (material && material->vtable == &basic_material_vtable) material->program = vs.program;
    }
}

// the store item a loadout slot shows (generateMeshes: ae), NULL for {}
static const ViewerSkin *slot_skin(const Player *player, const int slot, const int skins[2], const int melee_index) {
    const Weapon *weapon = player->game->weapons[player->loadout[slot]];
    const ViewerSkin *skin = viewer_skin(weapon->melee ? melee_index : slot < 2 ? skins[slot] : -1);
    if (skin && !weapon->melee && skin->weapon - 1 != player->loadout[slot]) skin = NULL;
    return skin;
}

// generateMeshes weapon loadMesh: src "weapons/<src>[_mid]", texSrc tex | weapons/skins/<src>_<id> |
// getReModel, at (akimbo xOff * -2, yOff, zOff), scale mScl || weapon.scale, emissive for glow skins
static void player_weapon_skin(Player *player, PlayerArms *arms, const Weapon *weapon, const ViewerSkin *skin, const int skin_index) {
    char model[128], texture[192];
    if (skin->mid >= 0) snprintf(model, sizeof(model), "weapons/%s_%d", weapon->src, skin->mid);
    else snprintf(model, sizeof(model), "weapons/%s", weapon->src);

    if (skin->mid >= 0) snprintf(texture, sizeof(texture), "weapons/weapon_%d_%d", skin->weapon, skin->mid_t > 0 ? skin->mid_t : skin->mid);
    else if (viewer_skin_tex_truthy(skin)) snprintf(texture, sizeof(texture), "%s", skin->tex);
    else if (skin->id >= 0) snprintf(texture, sizeof(texture), "weapons/skins/%s_%d", weapon->src, skin->id);
    else snprintf(texture, sizeof(texture), "%s", model);

    Geometry *geometry = model_for(model);
    const unsigned int tex = g_viewer.no_tex ? 0 : texture_for(texture);
    const int glow = skin_index >= 0 && skin_has_glow(skin);
    const int emissive = glow && !(skin->flags & VS_NO_EMIS);
    const float scale = skin->m_scl ? skin->m_scl : weapon->scale ? weapon->scale : 1.0f;

    Mesh *meshes[2] = {arms->weapon_right, arms->weapon_left};
    for (int j = 0; j < 2; j++) {
        Mesh *mesh = meshes[j];
        if (!mesh || !geometry) continue;
        mesh_reskin(mesh, geometry, tex);
        mesh->transform.position.y = skin->y_off;
        mesh->transform.position.z = skin->z_off;
        mesh->transform.scale = (vec3) {scale, scale, scale};
        material_apply_skin((BasicMaterial *) mesh->material, skin, texture, model, glow, emissive);
        anim_add(mesh, skin);
    }
    (void) player;
}

// the melee loadMesh: src melee/melee_<id>, texSrc pat ? tex : tex ? melee_<id>_<tex> : "", scale sclMlt || 0.9
static void player_melee_skin(PlayerArms *arms, const ViewerSkin *skin) {
    Mesh *mesh = arms->weapon_right;
    if (!mesh) return;

    char model[64], texture[192];
    const int id = skin->id > 0 ? skin->id : 0;
    snprintf(model, sizeof(model), "melee/melee_%d", id);
    if ((skin->flags & VS_PAT) && skin->tex) snprintf(texture, sizeof(texture), "%s", skin->tex);
    else if (viewer_skin_tex_truthy(skin)) snprintf(texture, sizeof(texture), "melee/melee_%d_%s", id, skin->tex);
    else snprintf(texture, sizeof(texture), "%s", model);

    Geometry *geometry = model_for(model);
    if (!geometry) return;
    mesh_reskin(mesh, geometry, g_viewer.no_tex ? 0 : texture_for(texture));
    const float scale = skin->scl_mlt ? skin->scl_mlt : 0.9f;
    mesh->transform.scale = (vec3) {scale, scale, scale};
    const int glow = skin_has_glow(skin);
    material_apply_skin((BasicMaterial *) mesh->material, skin, texture, model, glow, glow);
    anim_add(mesh, skin);
}

// back / waist / hat / face: what player_mesh.c leaves out of the store item (glow, spin, frames)
static void player_cosmetic_extras(Player *player) {
    static const char *const prefix[PLAYER_COSMETIC_SLOTS] = {"hats/hat_", "body/body_", "waist/waist_", "faces/face_"};
    const int index[PLAYER_COSMETIC_SLOTS] = {player->cosmetics.hat, player->cosmetics.back, player->cosmetics.waist, player->cosmetics.face};

    for (int slot = 0; slot < PLAYER_COSMETIC_SLOTS; slot++) {
        Mesh *mesh = ((PlayerMesh *) player->mesh)->cosmetics[slot];
        const ViewerSkin *skin = viewer_skin(index[slot]);
        if (!mesh || !skin) continue;

        char model[96], texture[128];
        snprintf(model, sizeof(model), "%s%d", prefix[slot], skin->id);
        if (viewer_skin_tex_truthy(skin)) snprintf(texture, sizeof(texture), "%s_%s", model, skin->tex);
        else snprintf(texture, sizeof(texture), "%s", model);

        BasicMaterial *material = (BasicMaterial *) mesh->material;
        const int glow = (skin->flags & VS_GLOW) != 0;
        if (skin_has_glow(skin)) material_apply_skin(material, skin, texture, model, glow, 1);
        if (g_viewer.no_tex) material->texture = 0;
        anim_add(mesh, skin);
    }
}

// Z(): a fresh preview player from the panel's values
void vscene_rebuild_player(void) {
    if (!vs.ready) return;
    scene_clear();

    const ViewerState *e = &g_viewer.e;
    Player *player = player_init(&vs.game);
    if (!player) return;

    player->uid = -1;
    player->class_index = e->class_index.num;
    if (player->class_index < 0 || player->class_index >= (int) vs.game.class_count) player->class_index = 0;
    player->secondary_index = e->secondary.num;

    PlayerCosmetics cosmetics = PLAYER_COSMETICS_NONE;
    cosmetics.hat = e->hat.num;
    cosmetics.back = e->back.num;
    cosmetics.waist = e->waist.num;
    cosmetics.face = e->face.num;
    cosmetics.dye = e->dye.num;
    for (int i = 0; i < g_viewer_skin_color_count; i++) {
        if (g_viewer_skin_colors[i].id == e->skin_col.num) cosmetics.skin_color = g_viewer_skin_colors[i].col;
    }
    player->cosmetics = cosmetics;

    player_spawn(player);
    if (!player->loadout_size || !player->weapon) {
        player_free(player);
        return;
    }

    // d.init(showPlayer ? 0 : 3, showPlayer ? 0 : -3, 3): init(z, x, y)
    player->position = (vec3) {e->show_player ? 0.0f : -3.0f, 3.0f, e->show_player ? 0.0f : 3.0f};
    player->last_position = player->position;
    player->direction = (vec2) {0.0f, 0.0f};
    player_generate_meshes(player, 0);
    if (!player->mesh) {
        player_free(player);
        return;
    }

    // d.skins = [skinPrim, skinSec] (a blocked primary skin falls back to 0)
    int skins[2] = {e->skin_prim.num, e->skin_sec.num};
    const ViewerSkin *prim = viewer_skin(skins[0]);
    if (prim && (prim->flags & VS_BLOCKED)) skins[0] = 0;

    for (int slot = 0; slot < player->loadout_size; slot++) {
        PlayerArms *arms = ((PlayerMesh *) player->mesh)->arms ? ((PlayerMesh *) player->mesh)->arms[slot] : NULL;
        if (!arms) continue;
        const Weapon *weapon = vs.game.weapons[player->loadout[slot]];
        const ViewerSkin *skin = slot_skin(player, slot, skins, e->melee.num);
        if (skin && weapon->melee) player_melee_skin(arms, skin);
        else if (skin && weapon->src) player_weapon_skin(player, arms, weapon, skin, slot < 2 ? skins[slot] : -1);
    }
    player_cosmetic_extras(player);

    vs.player = player;
    scene_add_player_mesh(vs.scene, player->mesh, player->loadout_size);
    player_legs_stand_in(player->mesh, 1);
    scene_use_viewer_program();

    // swapWeapon(d, 0); swapTime = 0; T == -1: swapSecondary, T == 1: swapMelee
    int slot = 0;
    if (g_viewer.swap == -1 && player->loadout_size > 2) slot = 1;
    if (g_viewer.swap == 1) slot = player->loadout_size - 1;
    player_swap_weapon(player, slot, 1, 1, 1);
    player->swap_timer = 0.0f;
    player->crouch_val = e->crouch ? 1.0f : 0.0f;
    player->height = game_constants.player_height - player->crouch_val * game_constants.crouch_distance; // updateHeight
    vs.step_dir = 1;
    player_update_meshes(player, 1);
}

int vscene_player_weapon_slot(void) {
    return vs.player ? vs.player->loadout_index : 0;
}

const Weapon *vscene_player_weapon(void) {
    return vs.player ? vs.player->weapon : NULL;
}

int vscene_player_is_melee(void) {
    return vs.player && vs.player->weapon && vs.player->weapon->melee;
}

// Krunker's lowerBody children: [0] body, [1] upperBody, then whichever of back / waist / hat /
// face exist, in that order; the loop toggles children[2..5] with showBack / showWaist / showHat /
// showFace by position, not by item (a missing back hands its flag to the next item).
static void player_visibility(Player *player) {
    const ViewerState *e = &g_viewer.e;
    PlayerMesh *mesh = player->mesh;
    const int show = e->show_player;

    for (int i = 0; i < 2; i++) {
        ColorCube *leg = mesh->legs[i];
        for (size_t k = 0; leg && k < leg->mesh_count; k++) leg->meshes[k]->visible = show && e->show_legs && !e->crouch;
        PlayerCrouchedLeg *crouched = mesh->crouched_legs[i];
        if (crouched) {
            const int v = show && e->show_legs && e->crouch;
            if (crouched->upper) crouched->upper->visible = v;
            if (crouched->joint) crouched->joint->visible = v;
            for (size_t k = 0; crouched->lower && k < crouched->lower->mesh_count; k++) crouched->lower->meshes[k]->visible = v;
        }
    }

    ColorCube *parts[2] = {mesh->body, mesh->head};
    for (int p = 0; p < 2; p++) {
        for (size_t k = 0; parts[p] && k < parts[p]->mesh_count; k++) parts[p]->meshes[k]->visible = show;
    }

    static const int order[PLAYER_COSMETIC_SLOTS] = {PLAYER_COSMETIC_BACK, PLAYER_COSMETIC_WAIST, PLAYER_COSMETIC_HAT, PLAYER_COSMETIC_FACE};
    const int flags[PLAYER_COSMETIC_SLOTS] = {e->show_back, e->show_waist, e->show_hat, e->show_face};
    int child = 0;
    for (int i = 0; i < PLAYER_COSMETIC_SLOTS; i++) {
        Mesh *item = mesh->cosmetics[order[i]];
        GltfInstance *model = mesh->cosmetic_models[order[i]]; // v7 animated (GLTF) item
        if (!item && !model) continue;
        const int visible = show && flags[child++];
        if (item) item->visible = visible;
        gltf_instance_set_visible(model, visible);
    }

    for (int slot = 0; slot < player->loadout_size; slot++) {
        PlayerArms *arms = mesh->arms ? mesh->arms[slot] : NULL;
        if (!arms) continue;
        const int current = slot == player->loadout_index;
        const int group = current && e->show_wep; // weaponMeshes[i].visible
        const int arm_parts = group && show && e->show_arms;

        for (int j = 0; j < 2; j++) {
            PlayerArmMesh *arm = j ? arms->left : arms->right;
            if (!arm) continue;
            if (arm->extender) arm->extender->visible = arm_parts;
            if (arm->upper) arm->upper->visible = arm_parts;
            if (arm->joint) arm->joint->visible = arm_parts;
            for (size_t k = 0; arm->lower && k < arm->lower->mesh_count; k++) arm->lower->meshes[k]->visible = arm_parts;
        }
        if (arms->weapon_right) arms->weapon_right->visible = group && !(slot == 0 && vs.hide_primary_geo);
        if (arms->weapon_left) arms->weapon_left->visible = group;
        if (arms->attachment) {
            const Weapon *weapon = vs.game.weapons[player->loadout[slot]];
            arms->attachment->visible = group && weapon->has_attach && g_viewer.e.attach.num == 0;
        }
    }
}

// ---- F(C): a weapon skin or melee item on its own (the cosmetic=<index> render page)

static void item_holder_rotate(MeshTransform *holder, const float rx, const float ry, const float rz) {
    holder->rotation = (vec3) {rx, ry, rz}; // rotateX, rotateY, rotateZ from identity = Euler XYZ
    holder->rotation_order = ROTATION_ORDER_INTRINSIC;
}

static void holder_translate_z(MeshTransform *holder, const float d) {
    // translateZ: along the holder's own z axis, R(XYZ) * (0, 0, 1)
    const float cx = cosf(holder->rotation.x), sx = sinf(holder->rotation.x);
    const float cy = cosf(holder->rotation.y), sy = sinf(holder->rotation.y);
    const float cz = cosf(holder->rotation.z), sz = sinf(holder->rotation.z);
    (void) cz;
    (void) sz;
    // Rx * Ry * Rz * (0,0,1): Rz leaves z, Ry -> (sy, 0, cy), Rx -> (sy, -sx*cy, cx*cy)
    holder->position.x += sy * d;
    holder->position.y += -sx * cy * d;
    holder->position.z += cx * cy * d;
}

static void item_texture_key(const ViewerSkin *n, char *texture, const size_t size, const int full_store_types) {
    const char *types = g_viewer_types[n->type >= 0 && n->type < g_viewer_type_count ? n->type : 0];
    texture[0] = 0;
    if (n->mid >= 0) {
        snprintf(texture, size, "weapons/weapon_%d_%d", n->weapon, n->mid_t > 0 ? n->mid_t : n->mid); // getReModel
    } else if (n->type == 3) {
        if ((n->flags & VS_PAT) && n->tex) snprintf(texture, size, "%s", n->tex);
        else if (viewer_skin_tex_truthy(n)) snprintf(texture, size, "melee/melee_%d_%s", n->id > 0 ? n->id : 0, n->tex);
        else snprintf(texture, size, "melee/melee_%d", n->id > 0 ? n->id : 0);
    } else if (n->type) {
        if (viewer_skin_tex_truthy(n)) {
            if (full_store_types) snprintf(texture, size, "%s%d_%s", types, n->id, n->tex);
            else snprintf(texture, size, "%s%d_%s", n->type == 1 ? "hats/hat_" : "body/body_", n->id, n->tex);
        }
    } else if (viewer_skin_tex_truthy(n)) {
        snprintf(texture, size, "%s", n->tex);
    } else {
        snprintf(texture, size, "weapons/skins/weapon_%d_%d", n->weapon, n->id);
    }
}

static Mesh *item_mesh(const ViewerSkin *n, const char *model, const char *texture, MeshTransform *holder, const float rot_y, const float scale, const int phong) {
    Geometry *geometry = model_for(model);
    if (!geometry) return NULL;
    const unsigned int tex = texture[0] && !g_viewer.no_tex ? texture_for(texture) : g_viewer.no_tex ? 0 : texture_for(model);
    Mesh *mesh = mesh_new(geometry, tex);
    if (!mesh) return NULL;

    mesh->transform.rotation.y = rot_y;
    mesh->transform.scale = (vec3) {scale, scale, scale};
    mesh->transform.parent = holder;
    const int glow = skin_has_glow(n);
    const int glow_text = (!n->type || !(n->type < 3)) && glow;
    material_apply_skin((BasicMaterial *) mesh->material, n, texture[0] ? texture : NULL, model, glow_text, glow);
    anim_add(mesh, n);
    (void) phong;
    item_add(mesh);
    return mesh;
}

static int item_shown(const ViewerSkin *n) {
    return n && (n->weapon || n->type == 3 || (n->type == 4 && (n->flags & VS_IS_3D)));
}

void vscene_build_cosmetic(int index) {
    if (!vs.ready) return;
    scene_clear();

    const ViewerSkin *n = viewer_skin(index);
    if (!item_shown(n)) return;

    const int weapon_index = (n->weapon ? n->weapon : 1) - 1; // game.weapons[(n.weapon || 1) - 1]
    const ViewerPreview *preview = viewer_preview(n->weapon);
    float y_off = n->type ? n->y_off : preview->y_off;
    float x_off = n->type ? n->x_off : preview->x_off;
    const float z_rota = n->type ? n->z_rota : preview->z_rota;
    const float x_rot = n->type ? n->x_rot : preview->x_rot;
    const float i_scl = n->type ? n->scl : preview->scl;
    float exp_scale = 1.0f;

    if (n->type == 3) { // r.expScale per melee model and the item offsets F() adjusts
        exp_scale = n->id == 8 ? 0.875f : n->id == 3 ? 0.8461538461538461f : n->id == 2 ? 0.875f : n->id == 1 ? 0.9f : 1.2f;
        if (n->id == 3) {
            y_off = -2.0f;
            x_off = 0.0f;
        }
        if (n->id == 12) x_off = -0.5f;
        if (n->id >= 9 && n->id <= 12) y_off = -2.0f;
        if (n->id == 0) exp_scale = 1.0f;
    }
    (void) weapon_index;

    MeshTransform *holder = holder_new(NULL);
    if (!holder) return;
    holder->position = (vec3) {0.0f, 1.0f + y_off, x_off}; // genObj3D(0, 1 + yOff, xOff)
    const int is3d = (n->flags & VS_IS_3D) != 0;
    item_holder_rotate(holder,
                       is3d ? 0.0f : (n->type == 3 ? 2.5f : 1.0f) * -0.25f,
                       is3d ? 0.0f : (float) M_PI + 0.5f + z_rota,
                       n->weapon == 6 || is3d ? 0.0f : -0.25f);

    char model[96], texture[192];
    const char *types = g_viewer_types[n->type >= 0 && n->type < g_viewer_type_count ? n->type : 0];
    if (n->mid >= 0) snprintf(model, sizeof(model), "%s%d_%d", types, n->type ? n->id : n->weapon, n->mid);
    else snprintf(model, sizeof(model), "%s%d", types, n->type ? n->id : n->weapon);
    item_texture_key(n, texture, sizeof(texture), 0);

    const float scale = (n->spn_scl ? n->spn_scl : i_scl ? i_scl : 1.0f) * 2.2f * exp_scale;
    holder_translate_z(holder, model_center_x(model) * scale); // centerZ
    item_mesh(n, model, texture, holder, (n->type && n->type != 3 ? (float) M_PI : (float) M_PI / 2.0f) + x_rot, scale, 1);
    scene_use_viewer_program();
}

// ---- showcase=<index>

void vscene_build_showcase(int index) {
    if (!vs.ready) return;
    scene_clear();

    const ViewerSkin *t = viewer_skin(index > 0 ? index : 0);
    if (!t) return;
    const ViewerPreview *preview = viewer_preview(t->weapon);
    const float y_off = t->type ? t->y_off : preview->y_off;
    const float x_off = t->type ? t->x_off : preview->x_off;
    const float z_rota = t->type ? t->z_rota : preview->z_rota;
    const float x_rot = t->type ? t->x_rot : preview->x_rot;
    const float n_scl = t->type ? t->scl : preview->scl;

    MeshTransform *holder = holder_new(NULL);
    if (!holder) return;
    holder->position = (vec3) {0.0f, 1.0f + y_off, x_off};
    if (t->type != 5) item_holder_rotate(holder, (float) -M_PI / 9.0f, (float) M_PI + 0.5f + z_rota, 0.0f);

    vec3 look = {0.0f, 0.0f, 0.0f};
    if (t->type == 5) {
        // a Runner (class 9) wearing the dye, init(0, -7, 0), its melee hidden
        Player *player = player_init(&vs.game);
        if (player) {
            player->uid = -1;
            player->class_index = 9;
            player->secondary_index = 2;
            player->cosmetics = PLAYER_COSMETICS_NONE;
            player->cosmetics.dye = index;
            player_spawn(player);
            player->position = (vec3) {-7.0f, 0.0f, 0.0f}; // init(z 0, x -7, y 0)
            player->direction = (vec2) {0.0f, 0.0f};
            if (player->loadout_size) player_generate_meshes(player, 0);
            if (player->mesh) {
                ((PlayerMesh *) player->mesh)->anchor->parent = holder;
                scene_add_player_mesh(vs.scene, player->mesh, player->loadout_size);
                player_swap_weapon(player, 0, 1, 1, 1);
                player->swap_timer = 0.0f;
                player_update_meshes(player, 1);
                PlayerArms *arms = ((PlayerMesh *) player->mesh)->arms ? ((PlayerMesh *) player->mesh)->arms[0] : NULL;
                if (arms && arms->weapon_right) arms->weapon_right->visible = 0;
                vs.showcase_player = player;
                look = (vec3) {player->position.x, 0.0f, player->position.z};
            } else {
                player_free(player);
            }
        }
    } else if (t->type == 4) {
        // a 9 x 7.2 plane with the spray, turned PI * 1.1
        char key[64];
        snprintf(key, sizeof(key), "sprays/%d", t->id > 0 ? t->id : 0);
        Mesh *mesh = mesh_new(create_plane_geo(), texture_for(key));
        if (mesh) {
            BasicMaterial *material = (BasicMaterial *) mesh->material;
            material->base.transparent = 1;
            material->no_depth_write = 1;
            material->uv_mode = BASIC_UV_MESH;
            mesh->transform.scale = (vec3) {9.0f, 7.2f, 1.0f};
            mesh->transform.rotation.y = (float) M_PI * 1.1f;
            mesh->transform.parent = holder;
            anim_add(mesh, t);
            item_add(mesh);
        }
    } else {
        char model[96], texture[192];
        const char *types = g_viewer_types[t->type >= 0 && t->type < g_viewer_type_count ? t->type : 0];
        if (t->mid >= 0) snprintf(model, sizeof(model), "%s%d_%d", types, t->type ? t->id : t->weapon, t->mid);
        else snprintf(model, sizeof(model), "%s%d", types, t->type ? t->id : t->weapon);
        item_texture_key(t, texture, sizeof(texture), 1);
        item_mesh(t, model, texture, holder, (t->type && t->type != 3 ? (float) M_PI : (float) M_PI / 4.0f) + x_rot,
                  (t->spn_scl ? t->spn_scl : n_scl ? n_scl : 1.0f) * 2.0f, 1);
    }

    scene_use_viewer_program();
    camera_place((vec3) {12.0f, 0.0f, 0.0f}, look);
}

// renderHat / renderFace / renderBack / renderWaist / renderDye: camera (0, 10|11, -15) looking
// at (0, 6, 0), the model turned, the primary weapon mesh hidden
void vscene_camera_render_pose(const int what) {
    const ViewerSkin *skin = NULL;
    float rotation = 0.0f;
    float camera_y = 10.0f;
    switch (what) {
        case 1: rotation = (float) M_PI / 6.5f; break;                                       // hat
        case 7: rotation = 0.0f; break;                                                      // face
        case 2: skin = viewer_skin(g_viewer.e.back.num);                                     // back
            rotation = (float) M_PI / (skin && skin->prev_rot ? skin->prev_rot : 1.0f); break;
        case 6: skin = viewer_skin(g_viewer.e.waist.num);                                    // waist
            rotation = (float) M_PI / (skin ? (skin->prev_rot ? skin->prev_rot : 1.0f) : 4.0f); break;
        case 5: rotation = (float) M_PI / 4.0f; camera_y = 11.0f; break;                     // dye
        default: break;
    }
    vs.render_rotation = rotation;
    vs.hide_primary_geo = 1;
    camera_place((vec3) {0.0f, camera_y, -15.0f}, (vec3) {0.0f, 6.0f, 0.0f});
}

void vscene_canvas_rect(float *x, float *y, float *w, float *h) {
    float vw = 0.0f, vh = 0.0f, dpr = 1.0f;
    html_dom_viewport(&vw, &vh, &dpr);
    *x = 0.0f;
    *y = 0.0f;
    *w = g_viewer.cosmetic_set ? COSMETIC_CANVAS : vw;
    *h = g_viewer.cosmetic_set ? COSMETIC_CANVAS : vh;
}

// ---------------------------------------------------------------- per frame

static void anims_tick(const float dt) {
    vs.puls_val += dt * PULSE_SPEED;
    for (int i = 0; i < vs.anim_count; i++) {
        ViewerAnim *anim = &vs.anims[i];
        if (anim->frames > 0 && anim->frame_t > 0.0f) {
            anim->frame_left -= dt;
            if (anim->frame_left <= 0.0f) {
                anim->frame_left = anim->frame_t;
                anim->frame = (anim->frame + 1) % anim->frames;
                anim->material->texture_offset.x = (float) anim->frame; // offset.x = cur / num (repeat units)
            }
        }
        if (anim->mov_t) {
            const float repeat = anim->material->texture_repeat.x ? anim->material->texture_repeat.x : 1.0f;
            anim->material->texture_offset.x += anim->mov_t * dt / repeat;
        }
        if (anim->puls_t) {
            anim->material->emissive.w = (sinf(vs.puls_val) / 2.0f + 0.5f) * 1.5f;
        }
        if (anim->r_anim) {
            vec3 *rotation = &anim->mesh->transform.rotation;
            if (anim->r_axis == 'x') rotation->x += anim->r_anim * dt;
            else if (anim->r_axis == 'z') rotation->z += anim->r_anim * dt;
            else rotation->y += anim->r_anim * dt;
        }
    }
}

void vscene_tick(const float dt) {
    if (!vs.ready) return;
    Player *player = vs.player;

    if (player && player->mesh) {
        if (g_viewer.e.animate) {
            // playerStep(d, delta * 0.015, true): the walk cycle in place; idleAnim += idleAnimS * delta
            player->step_val += dt * ANIM_STEP_SPEED * (float) vs.step_dir;
            if ((player->step_val >= (float) M_PI / 2.0f && vs.step_dir == 1) || (player->step_val <= (float) -M_PI / 2.0f && vs.step_dir == -1)) {
                vs.step_dir = -vs.step_dir;
            }
            player->step_chase += (player->step_val - player->step_chase) * 0.15f;
            player->idle_anim += game_constants.idle_anim_speed * dt / 1000.0f;
        }
        player->crouch_val = g_viewer.e.crouch ? 1.0f : 0.0f;
        player->direction.y = vs.render_rotation;
        player_update_meshes(player, 1);
        player_visibility(player);
    }
    if (vs.showcase_player && vs.showcase_player->mesh) {
        player_update_meshes(vs.showcase_player, 1);
        const PlayerMesh *showcase_mesh = vs.showcase_player->mesh;
        PlayerArms *arms = showcase_mesh->arms ? showcase_mesh->arms[0] : NULL;
        if (arms && arms->weapon_right) arms->weapon_right->visible = 0;
    }
    anims_tick(dt);
}

static void scene_draw(const int x, const int y, const int w, const int h) {
    const float fov = g_viewer.cosmetic_set ? COSMETIC_FOV_DEG : VIEWER_FOV_DEG;
    vs.camera.fov = fov * (float) M_PI / 180.0f;
    vs.camera.zoom = 1.0f;
    vs.camera.near = g_viewer.cosmetic_set ? COSMETIC_NEAR : VIEWER_NEAR;
    vs.camera.far = g_viewer.cosmetic_set ? COSMETIC_FAR : VIEWER_FAR;

    glViewport(x, y, w, h);
    glEnable(GL_SCISSOR_TEST);
    glScissor(x, y, w, h);
    glClear(GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);

    RenderLighting saved;
    lights_apply(&saved, g_viewer.cosmetic_set || g_viewer.showcase_set);
    scene_render(vs.scene, &vs.camera);
    g_render_lighting = saved;

    glDisable(GL_SCISSOR_TEST);
}

void vscene_render(const int fb_w, const int fb_h) {
    if (!vs.ready || fb_w <= 0 || fb_h <= 0) return;

    float vw = 0.0f, vh = 0.0f, dpr = 1.0f;
    html_dom_viewport(&vw, &vh, &dpr);
    const float scale = vh > 0.0f ? (float) fb_h / vh : 1.0f;

    glViewport(0, 0, fb_w, fb_h);
    glDisable(GL_SCISSOR_TEST);
    if (g_viewer.no_bg) {
        glClearColor(1.0f, 1.0f, 1.0f, 1.0f); // no container background: the white page shows through
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    } else {
        glClear(GL_DEPTH_BUFFER_BIT);
        draw_gradient(fb_h, 0.0f, (float) fb_h);
    }

    float cx, cy, cw, ch;
    vscene_canvas_rect(&cx, &cy, &cw, &ch);
    const int x = (int) lroundf(cx * scale), w = (int) lroundf(cw * scale), h = (int) lroundf(ch * scale);
    const int y = fb_h - (int) lroundf(cy * scale) - h;
    scene_draw(x, y, w, h);

    glViewport(0, 0, fb_w, fb_h);
    glEnable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
}

unsigned char *vscene_capture(const int w, const int h, const int gradient) {
    if (!vs.ready || w <= 0 || h <= 0) return NULL;

    unsigned int fbo = 0, color = 0, depth = 0;
    glGenFramebuffers(1, &fbo);
    glGenTextures(1, &color);
    glGenRenderbuffers(1, &depth);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    g_active_texture = color;
    glBindRenderbuffer(GL_RENDERBUFFER, depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);

    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);

    unsigned char *pixels = NULL;
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
        glViewport(0, 0, w, h);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f); // setClearColor(0, 0): the canvas image is transparent
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        // createLinearGradient(0, 0, 0, canvas.width): spans the width, measured down from the top
        if (gradient) draw_gradient(h, 0.0f, (float) w);
        scene_draw(0, 0, w, h);

        pixels = malloc((size_t) w * h * 4);
        unsigned char *rows = malloc((size_t) w * h * 4);
        if (pixels && rows) {
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rows);
            for (int r = 0; r < h; r++) memcpy(pixels + (size_t) r * w * 4, rows + (size_t) (h - 1 - r) * w * 4, (size_t) w * 4);
        }
        free(rows);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    glDeleteRenderbuffers(1, &depth);
    glDeleteTextures(1, &color);
    g_active_texture = 0;
    glEnable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    return pixels;
}

// ---------------------------------------------------------------- imports (RENDER.updateTexture / updateMesh)

static void override_note(const char *path, const int is_model) {
    for (int i = 0; i < vs.override_count; i++) {
        if (!strcmp(vs.overrides[i].path, path)) return;
    }
    if (vs.override_count >= VIEWER_MAX_OVERRIDES) return;
    vs.overrides[vs.override_count].path = strdup(path);
    vs.overrides[vs.override_count].is_model = is_model;
    vs.override_count++;
}

static void texture_upload(const unsigned int texture, const unsigned char *rgba_top_first, const int w, const int h) {
    // GL wants the bottom row first (load_texture flips on load)
    unsigned char *flipped = malloc((size_t) w * h * 4);
    if (!flipped) return;
    for (int r = 0; r < h; r++) memcpy(flipped + (size_t) r * w * 4, rgba_top_first + (size_t) (h - 1 - r) * w * 4, (size_t) w * 4);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, flipped);
    glGenerateMipmap(GL_TEXTURE_2D);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    g_active_texture = texture;
    free(flipped);
}

int vscene_update_texture_rgba(const char *key, const unsigned char *rgba, const int w, const int h) {
    char *path = asset("textures", key, ".png");
    if (!path) return 0;

    texture_cache_map_itr cached = vt_get(&g_texture_cache, path);
    unsigned int texture = 0;
    if (!vt_is_end(cached)) {
        texture = cached.data->val;
    } else {
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        char *key_copy = strdup(path);
        if (key_copy) vt_insert(&g_texture_cache, key_copy, texture);
    }
    texture_upload(texture, rgba, w, h);
    override_note(path, 0);
    printf("[viewer] updateTexture %s (%dx%d)\n", key, w, h);
    free(path);
    return 1;
}

// the geometry at `path` (cache key) takes the model parsed from obj_path; meshes sharing it follow
static int geometry_replace(const char *path, const char *obj_path) {
    Geometry *fresh = load_obj_model((char *) obj_path, 0);
    if (!fresh) return 0;

    geometry_cache_map_itr cached = vt_get(&g_geometry_cache, (char *) path);
    if (!vt_is_end(cached)) {
        Geometry *old = cached.data->val;
        if (old != fresh) {
            glDeleteVertexArrays(1, &old->vao);
            glDeleteBuffers(1, &old->ebo);
            *old = *fresh; // every mesh holding `old` now draws the new buffers
        }
    } else {
        char *key_copy = strdup(path);
        Geometry *copy = malloc(sizeof(Geometry));
        if (key_copy && copy) {
            *copy = *fresh;
            vt_insert(&g_geometry_cache, key_copy, copy);
        } else {
            free(key_copy);
            free(copy);
        }
    }
    // the temporary file's own cache entry keeps `fresh` (its GL names now shared): drop it
    geometry_cache_map_itr temp = vt_get(&g_geometry_cache, (char *) obj_path);
    if (!vt_is_end(temp) && strcmp(obj_path, path)) {
        char *temp_key = temp.data->key;
        Geometry *temp_geometry = temp.data->val;
        vt_erase_itr(&g_geometry_cache, temp);
        free(temp_key);
        free(temp_geometry);
    }
    return 1;
}

int vscene_update_mesh_obj(const char *key, const char *obj_path) {
    if (!strncmp(key, "event_0", 7)) return 0; // updateMesh ignores event_0*
    char *path = asset("models", key, ".obj");
    if (!path) return 0;
    const int ok = geometry_replace(path, obj_path);
    if (ok) override_note(path, 1);
    printf("[viewer] updateMesh %s %s\n", key, ok ? "ok" : "failed");
    free(path);
    return ok;
}

// RENDER.reset(): drop the caches, so the next build loads the files again
void vscene_reset_overrides(void) {
    for (int i = 0; i < vs.override_count; i++) {
        ViewerOverride *o = &vs.overrides[i];
        if (o->is_model) {
            ensure_asset_file(o->path);
            FILE *file = fopen(o->path, "rb");
            if (file) {
                fclose(file);
                char temp[512];
                snprintf(temp, sizeof(temp), "%s", o->path);
                // reparse the original file into the same Geometry
                geometry_cache_map_itr cached = vt_get(&g_geometry_cache, o->path);
                if (!vt_is_end(cached)) {
                    char *key = cached.data->key;
                    Geometry *old = cached.data->val;
                    vt_erase_itr(&g_geometry_cache, cached);
                    Geometry *fresh = load_obj_model(temp, 0); // re-inserts under the same path
                    if (fresh) {
                        glDeleteVertexArrays(1, &old->vao);
                        glDeleteBuffers(1, &old->ebo);
                        *old = *fresh;
                        geometry_cache_map_itr again = vt_get(&g_geometry_cache, temp);
                        if (!vt_is_end(again)) {
                            free(again.data->val);
                            again.data->val = old;
                        }
                    } else {
                        vt_insert(&g_geometry_cache, key, old);
                        key = NULL;
                    }
                    free(key);
                }
            }
        } else {
            texture_cache_map_itr cached = vt_get(&g_texture_cache, o->path);
            if (!vt_is_end(cached)) {
                ensure_asset_file(o->path);
                int w, h, channels;
                unsigned char *rgba = stbi_load(o->path, &w, &h, &channels, 4);
                if (rgba) {
                    texture_upload(cached.data->val, rgba, w, h);
                    stbi_image_free(rgba);
                }
            }
        }
        free(o->path);
    }
    vs.override_count = 0;
}

// ---------------------------------------------------------------- lifetime

int vscene_init(void) {
    memset(&vs, 0, sizeof(vs));
    vs.orbit_state = -1;

    // a private game: weapons and classes from shared/, an empty map (default map config)
    vs.map_json = cJSON_Parse("{\"name\":\"Viewer\",\"spawns\":[],\"objects\":[]}");
    game_configure(&vs.game, NULL, NULL, 0, NULL, 0, NULL, 0, NULL, 0);
    vs.game.map = vs.map_json ? map_init(vs.map_json) : NULL;
    vs.scene = scene_init();
    if (!vs.game.map || !vs.scene) {
        fprintf(stderr, "[viewer] scene init failed\n");
        return 0;
    }

    vs.program = viewer_program_create();
    vs.gradient_program = link_program(GRADIENT_VERT, GRADIENT_FRAG);
    glGenVertexArrays(1, &vs.empty_vao);

    vs.camera.zoom = 1.0f;
    vscene_camera_player(1);
    if (g_viewer.cosmetic_set) camera_place((vec3) {30.0f, 0.0f, 0.0f}, (vec3) {0.0f, 0.0f, 0.0f}); // g.lookAt(v.position)
    vs.ready = 1;
    return 1;
}

void vscene_fini(void) {
    if (!vs.ready && !vs.scene) return;
    scene_clear();
    for (int i = 0; i < vs.override_count; i++) free(vs.overrides[i].path);
    vs.override_count = 0;
    if (vs.scene) scene_fini(vs.scene);
    if (vs.game.map) map_fini(vs.game.map);
    free(vs.game.maps);
    free(vs.game.modes);
    free(vs.game.weapons);
    free(vs.game.classes);
    cJSON_Delete(vs.map_json);
    if (vs.program) {
        if (g_active_shader == vs.program) g_active_shader = 0;
        gl_uniform_forget_program(vs.program);
        glDeleteProgram(vs.program);
    }
    if (vs.gradient_program) {
        if (g_active_shader == vs.gradient_program) g_active_shader = 0;
        glDeleteProgram(vs.gradient_program);
    }
    if (vs.empty_vao) glDeleteVertexArrays(1, &vs.empty_vao);
    memset(&vs, 0, sizeof(vs));
}
