// The viewer page's three.js scene (Krunker v7.1.4 src/viewer.js), on KrunkNative's renderer:
//
// - f458(): a preview Player (new Player(-1), init(.., "preview"), every slot, setClass,
//   generateMeshes(player, false, true)) built by player_generate_meshes(): v7 slots, weapon and
//   melee skins, the sight, charms, the wrist, the preview pet and the items' material effects
//   (skin_fx.c) all come from player_mesh.c;
// - f447(C): one store item on its own (cosmetic=<index>, the 512 x 512 render page): a mesh, or a
//   class-9 preview Player wearing it (hat, head, face, dye, shoes: vO203 poses);
// - showcase=<index>: the item view the store embeds (dye player / spray plane / face on a head /
//   hat or head on a head / any other model), auto-rotating;
// - f448(): the "Viewer" (six SpotLights at +-150 on the axes) and "In-game" (the sun) lighting,
//   the ambient light, and the cosmetic / showcase fill light, through basic.c's light list
//   (basic_lights_override) so the map's lights never reach the viewer;
// - the OrbitControls camera (PerspectiveCamera(60, aspect, 0.01, 50));
// - the #container background linear-gradient(#7d8fa3, #1b1c1e), drawn here under the scene
//   because the page is composited over the GL canvas (the page's container stays clear).
//
// KRUNKNATIVE_VIEWER_FREEZE=1 (g_viewer.freeze) stops the clock for reference captures: render
// deltas are 0 (skin animations, clips and pets stay at their first frame) and the showcase does
// not auto-rotate, like tools/kref/viewer-ref.js does to the v7 page.
//
// Fixed v7.1.4 viewer bugs (doc/v7/player-viewer.md 5.1 item 3): "Show Body" toggles the body item
// (v7 reads vO201.bodyMesh), "Show Pet" the pet (v7: no effect), "Show Player" / "Show Arms" hide the
// head, body and arms (v7 toggles lowerBody.children by position: v7 moved the head first, so only
// the head follows Show Player and Show Arms does nothing), and the Wrist pick shows (v7 only builds
// it for isYou).
//
// Every GL object made here is freed here; the private Game / Map / Player never touch the
// client's game.
#include <gl.h>
#include "klog.h"
#include "viewer_internal.h"
#include <cosmetics.h>
#include <gltf.h>
#include <frame_target.h>
#include <pets.h>
#include <render_settings.h>
#include <skin_fx.h>
#include <store_item_model.h> // a store item's model (the showcase, the cosmetic page)
#include <fast_obj.h>
#include <stb_image.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- viewer.js / config.js constants
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
#define SUN_PITCH (-0.3f * (float) M_PI)     // v5734 = PI * -0.3, v5735 = PI * 2 * -0.25, at lightDistance
#define SUN_YAW (2.0f * (float) M_PI * -0.25f)
#define LIGHT_DISTANCE 500.0f                // config.lightDistance
#define FILL_INTENSITY 0.5f                  // cosmetic / showcase: DirectionalLight("#ffffff", 0.5) at (3, 1, 0)
#define SPOT_DISTANCE 150.0f                 // vA351: SpotLights at +-150 on each axis
#define SPOT_ANGLE ((float) M_PI / 3.0f)     // THREE.SpotLight default angle
#define GRADIENT_TOP 0x7d8fa3                // linear-gradient(#7d8fa3, #1b1c1e)
#define GRADIENT_BOTTOM 0x1b1c1e
#define ORBIT_EPS 0.000001f                  // Spherical.makeSafe
#define ORBIT_ZOOM 0.95f                     // Math.pow(0.95, zoomSpeed)
#define ORBIT_KEY_PAN 7.0f                   // keyPanSpeed
// OrbitControls.autoRotate: rotateLeft(2 PI / 60 / 60 * autoRotateSpeed 2) per update, 120 updates a second
#define ORBIT_AUTO_ROTATE_SPEED (2.0f * (float) M_PI / 60.0f / 60.0f * 2.0f * 120.0f / 1000.0f) // rad per ms
#define ANIM_STEP_SPEED (0.015f * 0.25f)     // playerStep(d, delta * 0.015): stepVal += dt * stepMlt
#define HAIR_COLOR 0x6C5042                  // 7098434: the showcase head cube's hair
#define HEAD_SKIN_COLOR 0xA77860             // 10975328: its skin
#define VIEWER_MAX_ITEMS 16
#define VIEWER_MAX_OVERRIDES 64

typedef struct {
    char *path;        // texture / model cache key (full asset path)
    int is_model;      // 0 texture, 1 OBJ geometry, 2 GLTF / GLB model
} ViewerOverride;

static struct {
    int ready;
    Game game;
    cJSON *map_json;
    Scene *scene;
    Camera camera;
    Player *player;
    int step_dir;

    // f447 / showcase items: meshes, GLTF models, colour cubes and their holder transforms
    Mesh *items[VIEWER_MAX_ITEMS];
    int item_count;
    GltfInstance *models[VIEWER_MAX_ITEMS];
    int model_count;
    ColorCube *cubes[VIEWER_MAX_ITEMS];
    int cube_count;
    MeshTransform *holders[VIEWER_MAX_ITEMS];
    int holder_count;
    Player *item_player;           // v5621: the showcase dye / cosmetic-page preview Player

    // orbit controls
    vec3 target;
    int orbit_state;   // -1 none, 0 rotate, 1 dolly, 2 pan
    float last_x, last_y;
    int auto_rotate;   // showcase: OrbitControls.autoRotate

    // lights (f448)
    int light_mode;    // 0 sun ("In-game"), 1 six axis spot lights ("Viewer")
    int fill_light;    // cosmetic page / showcase fill light
    float sun_x;       // the sun's x (a cosmetic-page face pose moves it to 100)

    unsigned int gradient_program;
    unsigned int empty_vao;

    ViewerOverride overrides[VIEWER_MAX_OVERRIDES];
    int override_count;

    float render_rotation;         // renderHat & co: player.objInstances.rotation.y
    vec3 render_offset;            // renderShoes: player.objInstances.position (0, 2, -2)
    int hide_primary_geo;          // player.weaponGeos[0].visible = false
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

static vec3 vec3_normalize(vec3 v) {
    const float length = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
    return length > 0.0f ? (vec3) {v.x / length, v.y / length, v.z / length} : (vec3) {0.0f, 1.0f, 0.0f};
}

// ---------------------------------------------------------------- shaders (the background gradient)

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

// ---------------------------------------------------------------- lights (f448)

void vscene_update_lights(void) {
    // "Viewer" (and not the cosmetic page): six SpotLights; "In-game" or the cosmetic page: the sun.
    // The cosmetic page and the showcase add the fill light.
    vs.light_mode = !strcmp(g_viewer.e.lighting.str, "Viewer") && !g_viewer.cosmetic_set ? 1 : 0;
    vs.fill_light = g_viewer.cosmetic_set || g_viewer.showcase_set;
    vs.sun_x = LIGHT_DISTANCE * cosf(SUN_YAW);
}

// The page's lights for one draw: the ambient light and the sun in g_render_lighting (the
// renderer's three.js defaults: legacy lights, no tone mapping, linear output, no fog), the spot
// and fill lights as basic.c's light list. Restored by lights_restore.
static void lights_apply(RenderLighting *saved) {
    *saved = g_render_lighting;

    g_render_lighting.ambient_color = color_vec(AMBIENT_COLOR, 1.0f);
    g_render_lighting.ambient_intensity = 1.0f;
    g_render_lighting.enabled = 1;
    g_render_lighting.fog_enabled = 0;
    g_render_lighting.correct_lights = 0;
    g_render_lighting.tone_mapping = 0;
    g_render_lighting.tone_mapping_exposure = 1.0f;
    g_render_lighting.output_encoding = 0;

    BasicLight lights[8];
    int count = 0;
    if (vs.light_mode == 1) {
        g_render_lighting.light_intensity = 0.0f; // no sun: the six white SpotLights aimed at the origin
        static const vec3 axes[3] = {{0.0f, SPOT_DISTANCE, 0.0f}, {SPOT_DISTANCE, 0.0f, 0.0f}, {0.0f, 0.0f, SPOT_DISTANCE}};
        for (int i = 0; i < 3; i++) {
            for (int side = 0; side < 2; side++) {
                const vec3 p = side ? (vec3) {-fabsf(axes[i].x), -fabsf(axes[i].y), -fabsf(axes[i].z)} : axes[i];
                lights[count++] = (BasicLight) {.type = BASIC_LIGHT_SPOT, .visible = 1, .color = {1.0f, 1.0f, 1.0f}, .intensity = 1.0f,
                                                .angle = SPOT_ANGLE, .position = p, .target = {0.0f, 0.0f, 0.0f}};
            }
        }
    } else {
        // DirectionalLight at (lightDistance * cos t, * sin t * sin e, * sin t * cos e), target the origin
        const vec3 position = {vs.sun_x, LIGHT_DISTANCE * sinf(SUN_YAW) * sinf(SUN_PITCH), LIGHT_DISTANCE * sinf(SUN_YAW) * cosf(SUN_PITCH)};
        const vec4 sun = color_vec(SUN_COLOR, 1.0f);
        g_render_lighting.light_color = sun;
        g_render_lighting.light_intensity = SUN_INTENSITY;
        g_render_lighting.light_direction = vec3_normalize(position);
    }
    if (vs.fill_light) {
        lights[count++] = (BasicLight) {.type = BASIC_LIGHT_DIRECTIONAL, .visible = 1, .color = {1.0f, 1.0f, 1.0f}, .intensity = FILL_INTENSITY,
                                        .position = {3.0f, 1.0f, 0.0f}, .target = {0.0f, 0.0f, 0.0f}};
    }
    basic_lights_override(lights, count);
}

static void lights_restore(const RenderLighting *saved) {
    g_render_lighting = *saved;
    basic_lights_override(NULL, -1);
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
        skin_fx_forget(vs.items[i]);
        mesh_fini(vs.items[i]);
    }
    vs.item_count = 0;
    for (int i = 0; i < vs.model_count; i++) {
        scene_remove_gltf(vs.scene, vs.models[i]);
        skin_fx_forget_gltf(vs.models[i]);
        gltf_instance_fini(vs.models[i]);
    }
    vs.model_count = 0;
    for (int i = 0; i < vs.cube_count; i++) {
        for (size_t k = 0; k < vs.cubes[i]->mesh_count; k++) scene_remove_mesh(vs.scene, vs.cubes[i]->meshes[k]);
        color_cube_fini(vs.cubes[i]);
    }
    vs.cube_count = 0;
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
    if (!mesh) return;
    if (vs.item_count >= VIEWER_MAX_ITEMS) {
        mesh_fini(mesh);
        return;
    }
    vs.items[vs.item_count++] = mesh;
    scene_add_mesh(vs.scene, mesh);
}

static void cube_add(ColorCube *cube) {
    if (!cube) return;
    if (vs.cube_count >= VIEWER_MAX_ITEMS) {
        color_cube_fini(cube);
        return;
    }
    vs.cubes[vs.cube_count++] = cube;
    for (size_t k = 0; k < cube->mesh_count; k++) scene_add_mesh(vs.scene, cube->meshes[k]);
}

// rotateX, rotateY, rotateZ from identity: Euler XYZ (three.js Object3D order "XYZ")
static void holder_rotate(MeshTransform *holder, const float rx, const float ry, const float rz) {
    holder->rotation = (vec3) {rx, ry, rz};
    holder->rotation_order = ROTATION_ORDER_INTRINSIC;
}

// translateZ: along the holder's own z axis, R(XYZ) * (0, 0, 1) = (sy, -sx cy, cx cy)
static void holder_translate_z(MeshTransform *holder, const float d) {
    const float cx = cosf(holder->rotation.x), sx = sinf(holder->rotation.x);
    const float cy = cosf(holder->rotation.y), sy = sinf(holder->rotation.y);
    holder->position.x += sy * d;
    holder->position.y += -sx * cy * d;
    holder->position.z += cx * cy * d;
}

// A store item's model src and texSrc as the showcase / cosmetic page load them:
//   src    store.types[assetType || type || 0] + (assetType || type ? id : weapon) + (mid ? "_" + mid : "")
//   texSrc mid ? getReModel : melee ? (pat ? tex : "melee/melee_<id>[_<tex>]") : type ? (tex ? "<src>_<tex>" : null)
//          : tex || "weapons/skins/weapon_<weapon>_<id>"
void viewer_item_keys(const ViewerSkin *n, char *model, const size_t model_n, char *texture, const size_t texture_n) {
    const int dir = n->asset_type ? n->asset_type : n->type;
    const char *types = g_viewer_types[dir >= 0 && dir < g_viewer_type_count ? dir : 0];
    if (n->mid >= 0) snprintf(model, model_n, "%s%d_%d", types, dir ? n->id : n->weapon, n->mid);
    else snprintf(model, model_n, "%s%d", types, dir ? n->id : n->weapon);

    texture[0] = 0;
    if (n->mid >= 0) {
        snprintf(texture, texture_n, "weapons/weapon_%d_%d", n->weapon, n->mid_t > 0 ? n->mid_t : n->mid); // getReModel
    } else if (n->type == ITEM_MELEE) {
        const int id = n->id > 0 ? n->id : 0;
        if ((n->flags & VS_PAT) && n->tex) snprintf(texture, texture_n, "%s", n->tex);
        else if (viewer_skin_tex_truthy(n)) snprintf(texture, texture_n, "melee/melee_%d_%s", id, n->tex);
        else snprintf(texture, texture_n, "melee/melee_%d", id);
    } else if (n->type) {
        if (viewer_skin_tex_truthy(n)) snprintf(texture, texture_n, "%s%d_%s", types, n->id, n->tex);
    } else if (viewer_skin_tex_truthy(n)) {
        snprintf(texture, texture_n, "%s", n->tex);
    } else {
        snprintf(texture, texture_n, "weapons/skins/weapon_%d_%d", n->weapon, n->id);
    }
}

// RENDER.loadMesh / loadAnimMesh of a store item into `holder` at `position`, turned rot_y about y,
// scaled `scale`, with its material options and animations (store_item_model.c, skin_fx.c); the OBJ falls
// back to the model's own texture (or none with noTex), and takes the page's paint (paintTargets items only).
static void item_build(const ViewerSkin *n, MeshTransform *holder, const vec3 position, const float rot_y, const float scale,
                       const int glow_text, const int emissive) {
    StoreItemModel built;
    if (!store_item_model_build(n, holder, position, rot_y, scale, glow_text, emissive, g_viewer.no_tex, &built)) return;
    if (built.model) {
        if (vs.model_count >= VIEWER_MAX_ITEMS) {
            store_item_model_fini(&built);
            return;
        }
        if (n->flags2 & VS2_PAINTABLE) skin_fx_apply_paint_gltf(built.model, g_viewer.paint, n); // the page's paint params
        vs.models[vs.model_count++] = built.model;
        scene_add_gltf(vs.scene, built.model);
        return;
    }
    if (n->flags2 & VS2_PAINTABLE) skin_fx_apply_paint(built.mesh, g_viewer.paint, n); // the page's paint params (paintTargets items only)
    item_add(built.mesh);
}

// player_update_meshes() (third person) poses legs[0..1] / crouched_legs[0..1] without checking
// them, but a noLegs waist item builds none: give such a model empty stand-ins (owned here,
// detached again before player_meshes_fini()).
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
    if (player->mesh) {
        player_legs_stand_in(player->mesh, 0);
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
    player_free(vs.item_player);
    vs.item_player = NULL;
    items_clear();
    vs.render_rotation = 0.0f;
    vs.render_offset = (vec3) {0};
    vs.hide_primary_geo = 0;
    vs.auto_rotate = 0;
}

// A preview Player (new Player(-1), init(z, x, y, "preview"), the slots, setClass, generateMeshes(p,
// false, true), swapWeapon(0), swapTime 0, updateMshClient) at `position`, its model added to the scene.
static Player *preview_player(const int class_index, const int secondary, const PlayerCosmetics *cosmetics, const PlayerCosmeticsV7 *v7,
                              const PlayerSkins skins, const int attach, const vec3 position, const float yaw) {
    Player *player = player_init(&vs.game);
    if (!player) return NULL;
    player->uid = -1;
    player->class_index = class_index >= 0 && class_index < (int) vs.game.class_count ? class_index : 0;
    player->secondary_index = secondary;
    player->cosmetics = *cosmetics;
    player->cosmetics_v7 = *v7;
    player->skins = skins;
    player->attach_index = attach;
    player_spawn(player); // loadout (also picks a spawn: the position is set below)
    if (!player->loadout_size || !player->weapon) {
        player_free(player);
        return NULL;
    }
    player->position = position;
    player->last_position = position;
    player->velocity = (vec3) {0};
    player->direction = (vec2) {0.0f, yaw}; // v7 xDire is the yaw (objInstances.rotation.y)
    player_generate_meshes(player, 0);
    if (!player->mesh) {
        player_free(player);
        return NULL;
    }
    scene_add_player_mesh(vs.scene, player->mesh, player->loadout_size);
    player_legs_stand_in(player->mesh, 1);
    player_swap_weapon(player, 0, 1, 1, 1);
    player->swap_timer = 0.0f;
    player_update_meshes(player, 1);
    return player;
}

// f458(): a fresh preview player from the panel's values
void vscene_rebuild_player(void) {
    if (!vs.ready) return;
    scene_clear();

    const ViewerState *e = &g_viewer.e;
    PlayerCosmetics cosmetics = PLAYER_COSMETICS_NONE;
    PlayerCosmeticsV7 v7 = PLAYER_COSMETICS_V7_NONE;
    cosmetics.hat = e->hat.num;
    cosmetics.body = e->body.num;
    cosmetics.waist = e->waist.num;
    cosmetics.face = e->face.num;
    cosmetics.dye = e->dye.num;
    for (int i = 0; i < g_viewer_skin_color_count; i++) {
        if (g_viewer_skin_colors[i].id == e->skin_col.num) cosmetics.skin_color = g_viewer_skin_colors[i].col;
    }
    v7.head = e->head.num;
    v7.back = e->back.num;
    v7.shoe = e->shoe.num;
    v7.pet = e->pet.num;
    v7.wrist = e->wrist.num;
    // player.skins = [skinPrim, skinSec] (a blocked primary skin falls back to 0), meleeIndex
    PlayerSkins skins = {e->skin_prim.num, e->skin_sec.num, e->melee.num};
    const ViewerSkin *prim = viewer_skin(skins.primary);
    if (prim && (prim->flags & VS_BLOCKED)) skins.primary = 0;

    // init(showPlayer ? 0 : 3, showPlayer ? 0 : -3, 3): z, x, y
    const vec3 position = {e->show_player ? 0.0f : -3.0f, 3.0f, e->show_player ? 0.0f : 3.0f};
    Player *player = preview_player(e->class_index.num, e->secondary.num, &cosmetics, &v7, skins, e->attach.num, position, 0.0f);
    if (!player) return;
    vs.player = player;
    if (g_viewer.no_tex) {
        for (size_t i = 0; i < vs.scene->mesh_count; i++) {
            Material *material = vs.scene->meshes[i]->material;
            if (material && material->vtable == &basic_material_vtable) ((BasicMaterial *) material)->texture = 0;
        }
    }

    // swapWeapon(0); v5631 == -1: swapSecondary, == 1: swapMelee
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

int vscene_player_weapon_id(void) {
    const Player *p = vs.player;
    return p && p->loadout && p->loadout_index >= 0 && p->loadout_index < p->loadout_size ? p->loadout[p->loadout_index] : -1;
}

const Weapon *vscene_player_weapon(void) {
    return vs.player ? vs.player->weapon : NULL;
}

int vscene_player_is_melee(void) {
    return vs.player && vs.player->weapon && vs.player->weapon->melee;
}

static void mesh_show(Mesh *mesh, const int visible) {
    if (mesh) mesh->visible = visible;
}

static void cube_show(ColorCube *cube, const int visible) {
    for (size_t k = 0; cube && k < cube->mesh_count; k++) cube->meshes[k]->visible = visible;
}

static void cosmetic_show(PlayerMesh *mesh, const int slot, const int visible) {
    mesh_show(mesh->cosmetics[slot], visible);
    if (mesh->cosmetic_models[slot]) gltf_instance_set_visible(mesh->cosmetic_models[slot], visible);
}

// f456's visibility pass, with the toggles doing what their names say (see the header)
static void player_visibility(Player *player) {
    const ViewerState *e = &g_viewer.e;
    PlayerMesh *mesh = player->mesh;
    const int show = e->show_player;

    for (int i = 0; i < 2; i++) {
        cube_show(mesh->legs[i], show && e->show_legs && !e->crouch);
        PlayerCrouchedLeg *crouched = mesh->crouched_legs[i];
        if (crouched) {
            const int v = show && e->show_legs && e->crouch;
            mesh_show(crouched->upper, v);
            mesh_show(crouched->joint, v);
            cube_show(crouched->lower, v);
        }
        cosmetic_show(mesh, PLAYER_COSMETIC_SHOE_R + i, show && e->show_legs && !e->crouch && !mesh->shoe_hidden[i]);
    }
    cube_show(mesh->body, show);
    cube_show(mesh->head, show);

    cosmetic_show(mesh, PLAYER_COSMETIC_BODY, show && e->show_body);
    cosmetic_show(mesh, PLAYER_COSMETIC_BACK, show && e->show_back);
    cosmetic_show(mesh, PLAYER_COSMETIC_WAIST, show && e->show_waist);
    cosmetic_show(mesh, PLAYER_COSMETIC_HAT, show && e->show_hat);
    cosmetic_show(mesh, PLAYER_COSMETIC_HEAD, show && e->show_head);
    cosmetic_show(mesh, PLAYER_COSMETIC_FACE, show && e->show_face);
    pet_set_visible(mesh->pet, e->show_pet);

    for (int slot = 0; slot < player->loadout_size; slot++) {
        PlayerArms *arms = mesh->arms ? mesh->arms[slot] : NULL;
        if (!arms) continue;
        const int current = slot == player->loadout_index;
        const int group = current && e->show_wep; // weaponMeshes[i].visible
        const int arm_parts = group && show && e->show_arms;

        for (int j = 0; j < 2; j++) {
            PlayerArmMesh *arm = j ? arms->left : arms->right;
            if (!arm) continue;
            mesh_show(arm->extender, arm_parts);
            mesh_show(arm->upper, arm_parts);
            mesh_show(arm->joint, arm_parts);
            cube_show(arm->lower, arm_parts);
        }
        const int weapon_visible = group && !(slot == 0 && vs.hide_primary_geo);
        mesh_show(arms->weapon_right, weapon_visible);
        mesh_show(arms->weapon_left, group);
        for (int k = 0; k < 2; k++) {
            if (arms->weapon_models[k]) gltf_instance_set_visible(arms->weapon_models[k], k ? group : weapon_visible);
        }
        mesh_show(arms->charm_mount, group);
        mesh_show(arms->charm, group);
        mesh_show(arms->attachment, group);
        mesh_show(arms->wrist, group && show && e->show_wrist);
        if (arms->wrist_model) gltf_instance_set_visible(arms->wrist_model, group && show && e->show_wrist);
    }
}

// ---- f447(C): one store item on its own (the cosmetic=<index> render page)

// vO203: the types the page shows on a class-9 preview Player wearing just that item:
// [slot, camera x, xDire (the yaw), x offset, z rotation, sun x]
typedef struct { int type; float camera_x, yaw, x_offset, z_rotation, sun_x; int has_sun_x; } CosmeticPose;
static const CosmeticPose k_cosmetic_poses[] = {
    {ITEM_HAT, 12.0f, (float) (-M_PI / 2 + M_PI / 6.5), 0.0f, 0.0f, 0.0f, 0},
    {ITEM_HEAD, 12.0f, (float) (-M_PI / 2 + M_PI / 6.5), 0.0f, 0.0f, 0.0f, 0},
    {ITEM_DYE, 26.0f, (float) (-M_PI / 4), 0.0f, 0.0f, 0.0f, 0},
    {ITEM_FACE, 9.5f, (float) (-M_PI / 2), 0.15f, 0.0f, 100.0f, 1},
    {ITEM_SHOE, 12.0f, (float) (-M_PI / 4), 6.5f, -0.3f, 0.0f, 0},
};

static const CosmeticPose *cosmetic_pose(const int type) {
    for (size_t i = 0; i < sizeof(k_cosmetic_poses) / sizeof(k_cosmetic_poses[0]); i++) {
        if (k_cosmetic_poses[i].type == type) return &k_cosmetic_poses[i];
    }
    return NULL;
}

int vscene_cosmetic_shown(const int index) {
    const ViewerSkin *n = viewer_skin(index);
    if (!n) return 0;
    if (n->weapon) return 1;
    switch (n->type) {
        case ITEM_TICKET: case ITEM_CHARM: case ITEM_PET: case ITEM_COLLECTIBLE: case ITEM_MELEE:
        case ITEM_HAT: case ITEM_HEAD: case ITEM_FACE: case ITEM_DYE: case ITEM_SHOE:
            return 1;
        default:
            return 0;
    }
}

void vscene_build_cosmetic(const int index) {
    if (!vs.ready) return;
    scene_clear();
    vscene_update_lights();

    const ViewerSkin *n = viewer_skin(index);
    if (!vscene_cosmetic_shown(index)) return;

    // r = game.weapons[(weapon || 1) - 1] (expScale), i = type ? the item : store.previews[weapon]
    const ViewerPreview *preview = viewer_preview(n);
    float y_off = n->type ? n->y_off : preview->y_off;
    float x_off = n->type ? n->x_off : preview->x_off;
    float z_rota = n->type ? n->z_rota : preview->z_rota;
    float x_rot = n->type ? n->x_rot : preview->x_rot;
    const float i_scl = n->type ? n->scl : preview->scl;
    float exp_scale = 0.0f; // r.expScale, 0 = none

    if (n->type == ITEM_MELEE) {
        const int id = n->id;
        exp_scale = id == 8 ? 0.875f : id == 3 ? 0.8461538461538461f : id == 2 ? 0.875f : id == 1 ? 0.9f : 1.2f;
        if (id == 3) {
            y_off = -2.0f;
            x_off = 0.0f;
        }
        if (id == 12) x_off = -0.5f;
        if (id >= 9 && id <= 12) y_off = -2.0f;
        if (id == 33 || id == 34) y_off = -4.0f;
        if (id == 65) y_off = -2.0f;
        if (id == 67 || id == 71 || id == 70 || id == 76 || id == 77 || id == 78 || id == 80 || id == 84 || id == 85 || id == 92) exp_scale = 0.85f;
        if (id == 86) exp_scale = 0.65f;
        if (id == 93 || id == 95) exp_scale = 0.8f;
        if (id == 82) x_off = 4.0f;
        if (id == 59) {
            exp_scale = 0.7f;
            y_off = 1.5f;
            x_off = -1.0f;
            z_rota = 5.0f;
            x_rot = 3.0f;
        }
    }
    if (n->weapon == 2 && n->mid == 2) { x_off += 1.5f; y_off += 0.5f; }
    if (n->weapon == 2 && (n->mid == 5 || n->mid == 7)) { x_off -= 1.5f; y_off -= 0.5f; }
    if (n->weapon == 22 && n->mid == 1) { exp_scale = 1.2f; x_off -= 4.5f; }
    if (n->type == ITEM_COLLECTIBLE || n->type == ITEM_WRIST) { exp_scale = 1.4f; y_off -= 0.5f; }
    if (n->weapon == 1 && n->mid == 15) { x_off -= 1.5f; y_off -= 0.5f; }
    if (n->weapon == 1 && n->mid == 16) y_off -= 0.5f;
    if (n->weapon == 11 && n->mid == 7) y_off = -1.0f;
    if (n->weapon == 4 && n->mid == 11) x_off += 2.0f;

    const CosmeticPose *pose = cosmetic_pose(n->type);
    MeshTransform *holder = holder_new(NULL);
    if (!holder) return;
    if (!pose) holder->position = (vec3) {0.0f, 1.0f + y_off, x_off}; // genObj3D(0, 1 + yOff, xOff)

    if (n->type == ITEM_PET) {
        holder_rotate(holder, 0.0f, (float) M_PI, 0.0f);
    } else if (n->type == ITEM_WRIST) {
        holder_rotate(holder, 0.0f, (float) M_PI + 0.5f + z_rota, 1.25f);
    } else if (n->type == ITEM_CHARM) {
        holder_rotate(holder, 0.0f, (float) M_PI * 1.75f, 0.0f);
    } else if (!pose) {
        holder_rotate(holder, (n->type == ITEM_MELEE ? 2.5f : 1.0f) * -0.25f, (float) M_PI + 0.5f + z_rota, n->weapon == 6 ? 0.0f : -0.25f);
        if (n->type == ITEM_TICKET) {
            // rotateX(-1) rotateY(-0.5) rotateZ(-0.15) on top: a child holder
            holder = holder_new(holder);
            if (!holder) return;
            holder_rotate(holder, -1.0f, -0.5f, -0.15f);
            exp_scale = 2.5f;
        }
    }

    vec3 camera = {30.0f, 0.0f, 0.0f};
    if (pose) {
        // a class-9 preview Player wearing the item: init(0, -9.6 + pose x, 0), turned xDire, weaponGeos[0] hidden
        if (pose->has_sun_x) vs.sun_x = pose->sun_x;
        PlayerCosmetics cosmetics = PLAYER_COSMETICS_NONE;
        PlayerCosmeticsV7 v7 = PLAYER_COSMETICS_V7_NONE;
        if (n->type == ITEM_HAT) cosmetics.hat = index;
        else if (n->type == ITEM_HEAD) v7.head = index;
        else if (n->type == ITEM_DYE) cosmetics.dye = index;
        else if (n->type == ITEM_FACE) cosmetics.face = index;
        else if (n->type == ITEM_SHOE) v7.shoe = index;
        vs.item_player = preview_player(9, 2, &cosmetics, &v7, PLAYER_SKINS_NONE, -1, (vec3) {-9.6f + pose->x_offset, 0.0f, 0.0f}, pose->yaw);
        if (vs.item_player) ((PlayerMesh *) vs.item_player->mesh)->anchor->parent = holder;
        holder->rotation.z = pose->z_rotation;
        camera.x = pose->camera_x;
    } else {
        const float scale = (n->spn_scl ? n->spn_scl : i_scl ? i_scl : 1.0f) * 2.2f * (exp_scale > 0.0f ? exp_scale : 1.0f);
        char model[128], texture[192];
        viewer_item_keys(n, model, sizeof(model), texture, sizeof(texture));
        if (!(n->flags & VS_HAS_ANIM)) holder_translate_z(holder, model_center_x(model) * scale); // centerZ
        const int glow = (n->flags & (VS_GLOW | VS_SAME_GLOW)) != 0;
        item_build(n, holder, (vec3) {0}, (n->type && n->type != ITEM_MELEE ? (float) M_PI : (float) M_PI / 2.0f) + x_rot, scale,
                   (!n->type || n->type >= 3) && glow, glow);
    }

    // the camera was aimed at the scene origin from x 30 at init; f447 only moves it along x
    vs.camera.position = camera;
    vs.camera.rotation = (vec3) {0.0f, (float) M_PI / 2.0f, 0.0f};
    vs.target = (vec3) {0};
}

// ---- showcase=<index>: the store's item view

// genColorCube(1, 1, 1, [[hair, 0.2], [skin, 0.8]], 0) with the geometry scaled headScale and moved
// down headScale / 2, the mesh scaled 2: a 4 x 4 x 4 head whose top is the holder's origin
static MeshTransform *showcase_head(MeshTransform *holder, const int no_hair) {
    MeshTransform *frame = holder_new(holder);
    if (!frame) return NULL;
    frame->scale = (vec3) {2.0f, 2.0f, 2.0f};
    const float hs = game_constants.head_scale;
    const ColorCubeSegment segments[2] = {{HAIR_COLOR, no_hair ? 0.0f : 0.2f, 1.0f}, {HEAD_SKIN_COLOR, 0.8f, 1.0f}};
    ColorCube *cube = generate_color_cube_at(hs, hs, hs, segments, 2, no_hair ? 0.3f : 0.5f);
    if (cube) {
        cube->transform.position.y = -hs / 2.0f;
        cube->transform.parent = frame;
        cube_add(cube);
    }
    return frame;
}

void vscene_build_showcase(int index) {
    if (!vs.ready) return;
    scene_clear();
    vscene_update_lights();

    const ViewerSkin *t = viewer_skin(index > 0 ? index : 0);
    if (!t) return;
    const ViewerPreview *preview = viewer_preview(t);
    const float y_off = t->type ? t->y_off : preview->y_off;
    const float x_off = t->type ? t->x_off : preview->x_off;
    const float z_rota = t->type ? t->z_rota : preview->z_rota;
    const float x_rot = t->type ? t->x_rot : preview->x_rot;
    const float n_scl = t->type ? t->scl : preview->scl;

    // v5797 = genObj3D(0, 1 + (HAT ? 0 : yOff), FACE / HAT ? 0 : xOff), turned PI + 0.5 + zRota (a dye -PI / 2)
    MeshTransform *holder = holder_new(NULL);
    if (!holder) return;
    holder->position = (vec3) {0.0f, 1.0f + (t->type == ITEM_HAT ? 0.0f : y_off), t->type != ITEM_FACE && t->type != ITEM_HAT ? x_off : 0.0f};
    float yaw = t->type == ITEM_DYE ? (float) -M_PI / 2.0f : (float) M_PI + 0.5f + z_rota;
    if (t->type == ITEM_FACE || t->type == ITEM_HAT || t->type == ITEM_HEAD) yaw += (float) M_PI / 2.0f; // rotateY(PI / 2) after
    holder_rotate(holder, 0.0f, yaw, 0.0f);

    vec3 look = {0.0f, 0.0f, 0.0f};
    vs.auto_rotate = 1;
    if (t->type == ITEM_DYE) {
        // a class-9 preview Player wearing the dye, init(0, -7, 0), its melee hidden. v7.1.4 throws
        // there (v5621.weaponGeos[0] is undefined: class 9's first weapon has no melee mesh), so its dye
        // view is empty, and its camera would aim at the player's local position anyway. KrunkNative
        // shows the player: the camera faces its middle from 12 units away and orbits it.
        PlayerCosmetics cosmetics = PLAYER_COSMETICS_NONE;
        const PlayerCosmeticsV7 v7 = PLAYER_COSMETICS_V7_NONE;
        cosmetics.dye = index;
        vs.item_player = preview_player(9, 2, &cosmetics, &v7, PLAYER_SKINS_NONE, -1, (vec3) {-7.0f, 0.0f, 0.0f}, 0.0f);
        if (vs.item_player) {
            ((PlayerMesh *) vs.item_player->mesh)->anchor->parent = holder;
            // the holder turns -PI / 2 about y: local (-7, 0, 0) is (0, 0, -7) from its origin
            const vec3 middle = {holder->position.x, holder->position.y + game_constants.player_height / 2.0f, holder->position.z - 7.0f};
            camera_place((vec3) {middle.x + 12.0f, middle.y, middle.z}, middle);
            return;
        }
    } else if (t->type == ITEM_SPRAY) {
        // a 9 x 9 plane with the spray (depthWrite false, transparent, its effects), on a brick_0_3
        // backing 0.1 deep, turned PI * 1.1
        MeshTransform *plane = holder_new(holder);
        if (!plane) return;
        plane->scale = (vec3) {9.0f, 9.0f, 1.0f};
        plane->rotation.y = (float) M_PI * 1.1f;
        BasicMaterial *material = basic_material_init();
        if (material) {
            char key[64];
            snprintf(key, sizeof(key), "sprays/%d", t->id > 0 ? t->id : 0);
            material->texture = texture_for(key);
            material->base.transparent = 1;
            material->no_depth_write = 1;
            material->double_sided = 0; // FrontSide: the cube's back face (mirrored UVs) is culled
            if (t->opacity > 0.0f) material->color.w = t->opacity;
            Mesh *quad = mesh_init(create_cube_geo(), (Material *) material); // the unit quad: a cube's front face, 0 deep
            quad->transform.scale = (vec3) {1.0f, 1.0f, 0.0001f};
            quad->transform.position.y = -0.5f;
            quad->transform.parent = plane;
            skin_fx_apply(quad, t, key, key, (t->flags & (VS_GLOW | VS_SAME_GLOW)) != 0, (t->flags & (VS_GLOW | VS_SAME_GLOW)) != 0);
            item_add(quad);
        }
        BasicMaterial *brick = basic_material_init();
        if (brick) {
            brick->texture = texture_for("brick_0_3");
            Mesh *backing = mesh_init(create_cube_geo(), (Material *) brick);
            backing->transform.scale = (vec3) {1.0f, 1.0f, 0.01f};
            backing->transform.position = (vec3) {0.0f, -0.5f, -0.1f};
            backing->transform.parent = plane;
            item_add(backing);
        }
    } else if (t->type == ITEM_FACE) {
        // a 4 x 4 x 4 head (genColorCube(headScale * 2, ...)) with the face at (0, 0, -sitOffZ), scale
        // sclMlt * 0.5 inside it; no auto-rotation
        MeshTransform *head = holder_new(holder);
        if (!head) return;
        const float size = game_constants.head_scale * 2.0f;
        head->scale = (vec3) {size, size, size};
        const ColorCubeSegment segments[2] = {{HAIR_COLOR, 0.2f, 1.0f}, {HEAD_SKIN_COLOR, 0.8f, 1.0f}};
        ColorCube *cube = generate_color_cube_at(1.0f, 1.0f, 1.0f, segments, 2, 0.5f);
        if (cube) {
            cube->transform.parent = head;
            cube_add(cube);
        }
        const int glow = (t->flags & (VS_GLOW | VS_SAME_GLOW)) != 0;
        item_build(t, head, (vec3) {0.0f, 0.0f, -t->sit_off_z}, (float) M_PI / 2.0f, (t->scl_mlt ? t->scl_mlt : 1.0f) * 0.5f,
                   (t->flags & VS_GLOW) != 0, glow);
        vs.auto_rotate = 0;
    } else if (t->type == ITEM_HAT || t->type == ITEM_HEAD) {
        // noHead: the item alone, scale (spnScl || scl || 1) * 2; else on the head at (0, -sitOff, -sitOffZ),
        // scale sclMlt * 2.1
        const int no_head = (t->flags & VS_NO_HEAD) != 0;
        MeshTransform *frame = no_head ? holder : showcase_head(holder, (t->flags2 & VS2_NO_HAIR) != 0);
        if (!frame) return;
        const vec3 at = no_head ? (vec3) {0} : (vec3) {0.0f, -t->sit_off, -t->sit_off_z};
        const float scale = no_head ? (t->spn_scl ? t->spn_scl : n_scl ? n_scl : 1.0f) * 2.0f : (t->scl_mlt ? t->scl_mlt : 1.0f) * 2.1f;
        const int glow = (t->flags & (VS_GLOW | VS_SAME_GLOW)) != 0;
        item_build(t, frame, at, (float) M_PI / 2.0f, scale, (t->flags & VS_GLOW) != 0, glow);
    } else {
        // any other model at the holder's origin, turned (type && !melee ? PI : PI / 4) + xRot, scale
        // (spnScl || scl || 1) * 2; glowText = emissive = glow || sameGlow (emissive #fff)
        const int glow = (t->flags & (VS_GLOW | VS_SAME_GLOW)) != 0;
        item_build(t, holder, (vec3) {0}, (t->type && t->type != ITEM_MELEE ? (float) M_PI : (float) M_PI / 4.0f) + x_rot,
                   (t->spn_scl ? t->spn_scl : n_scl ? n_scl : 1.0f) * 2.0f, glow, glow);
    }

    camera_place((vec3) {12.0f, 0.0f, 0.0f}, look);
}

// renderHat / Head / Face / Body / Back / Waist / Shoes / Dye / Pet: camera (0, 10, -15) looking at
// (0, 6, 0) (dye: from y 11; shoes: (0, 6, -11) at the origin with the model at (0, 2, -2)), the
// model turned, the primary weapon's melee mesh hidden
void vscene_camera_render_pose(const int what) {
    const ViewerSkin *skin = NULL;
    float rotation = 0.0f;
    vec3 camera = {0.0f, 10.0f, -15.0f}, target = {0.0f, 6.0f, 0.0f};
    vs.render_offset = (vec3) {0};
    switch (what) {
        case ITEM_HAT: case ITEM_HEAD: case ITEM_PET: rotation = (float) M_PI / 6.5f; break;
        case ITEM_FACE: rotation = 0.0f; break;
        case ITEM_BODY: case ITEM_BACK: // PI / (prevRot == null ? 1 : prevRot) (the table has no prevRot 0 on these)
            skin = viewer_skin(what == ITEM_BODY ? g_viewer.e.body.num : g_viewer.e.back.num);
            rotation = (float) M_PI / (skin && skin->prev_rot != 0.0f ? skin->prev_rot : 1.0f);
            break;
        case ITEM_WAIST: skin = viewer_skin(g_viewer.e.waist.num);
            rotation = (float) M_PI / (skin ? (skin->prev_rot ? skin->prev_rot : 1.0f) : 4.0f); break;
        case ITEM_SHOE:
            rotation = (float) M_PI / 4.0f;
            vs.render_offset = (vec3) {0.0f, 2.0f, -2.0f};
            camera = (vec3) {0.0f, 6.0f, -11.0f};
            target = (vec3) {0};
            break;
        case ITEM_DYE: rotation = (float) M_PI / 4.0f; camera.y = 11.0f; break;
        default: break;
    }
    vs.render_rotation = rotation;
    vs.hide_primary_geo = 1;
    camera_place(camera, target);
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

void vscene_tick(float dt) {
    if (!vs.ready) return;
    if (g_viewer.freeze) dt = 0.0f;
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
        const vec3 base = {g_viewer.e.show_player ? 0.0f : -3.0f, 3.0f, g_viewer.e.show_player ? 0.0f : 3.0f};
        player->position = (vec3) {base.x + vs.render_offset.x, base.y + vs.render_offset.y, base.z + vs.render_offset.z};
        player_update_meshes(player, 1);
        player_visibility(player);
    }
    if (vs.item_player && vs.item_player->mesh) {
        player_update_meshes(vs.item_player, 1);
        const PlayerMesh *item_mesh = vs.item_player->mesh;
        PlayerArms *arms = item_mesh->arms ? item_mesh->arms[0] : NULL;
        if (arms && arms->weapon_right) arms->weapon_right->visible = 0; // weaponGeos[0].visible = false
        if (arms && arms->weapon_models[0]) gltf_instance_set_visible(arms->weapon_models[0], 0);
    }
    const double now = gltf_clock();
    for (int i = 0; i < vs.model_count; i++) gltf_instance_update(vs.models[i], now);
    // Game.update -> RENDER.render(delta): the material animations (textureAnim is on in the viewer)
    skin_fx_tick(dt, 1, client_render_settings()->lighting == 0);
    // OrbitControls.autoRotate (the showcase's 120 Hz update)
    if (vs.auto_rotate && g_viewer.showcase_set && dt > 0.0f && vs.orbit_state < 0) orbit_apply(-ORBIT_AUTO_ROTATE_SPEED * dt, 0.0f, 1.0f, (vec3) {0});
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
    lights_apply(&saved);
    scene_render(vs.scene, &vs.camera);
    lights_restore(&saved);

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

void vscene_render_rect(const int x, const int y, const int w, const int h, const int fb_w, const int fb_h) {
    if (!vs.ready || w <= 0 || h <= 0) return;
    // over the composited page with a transparent background (setClearColor(0, 0)); the
    // framebuffer's alpha stays the page's (as client_class_preview_render keeps it)
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
    scene_draw(x, y, w, h);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
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
    glBindTexture(GL_TEXTURE_2D, 0); // the scene draws into it: not left on unit 0 (feedback loop)
    g_active_texture = 0;
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

    glBindFramebuffer(GL_FRAMEBUFFER, frame_target_framebuffer()); // the window (frame_target.h)
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
    KN_LOG_VERBOSE2("[viewer] updateTexture %s (%dx%d)\n", key, w, h);
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
            geometry_free_wire(old);
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
    KN_LOG_VERBOSE2("[viewer] updateMesh %s %s\n", key, ok ? "ok" : "failed");
    free(path);
    return ok;
}

// updateMesh(key, data, "gltf" | "glb"): the animated model loadAnimMesh("<key>.<ext>") finds
// next (v7 parses into its vA331[key + ext] cache). v7 reads a picked .glb as text, which
// breaks it; here GLB works too.
int vscene_update_mesh_gltf(const char *key, const char *file_path, const char *ext) {
    if (!strncmp(key, "event_0", 7)) return 0;
    char dotted[16];
    snprintf(dotted, sizeof(dotted), ".%s", ext);
    char *path = asset("models", key, dotted);
    if (!path) return 0;
    const int ok = gltf_model_override(path, file_path);
    if (ok) override_note(path, 2);
    KN_LOG_VERBOSE2("[viewer] updateMesh %s.%s %s\n", key, ext, ok ? "ok" : "failed");
    free(path);
    return ok;
}

// RENDER.reset(): drop the caches, so the next build loads the files again
void vscene_reset_overrides(void) {
    for (int i = 0; i < vs.override_count; i++) {
        ViewerOverride *o = &vs.overrides[i];
        if (o->is_model == 2) {
            gltf_model_restore(o->path);
        } else if (o->is_model) {
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
                        geometry_free_wire(old);
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

    vs.gradient_program = link_program(GRADIENT_VERT, GRADIENT_FRAG);
    glGenVertexArrays(1, &vs.empty_vao);
    if (g_viewer.freeze) gltf_set_clock_override(0.0); // clips, pets and ShaderToy time at their start

    vs.camera.zoom = 1.0f;
    vscene_update_lights();
    vscene_camera_player(1);
    if (g_viewer.cosmetic_set) camera_place((vec3) {30.0f, 0.0f, 0.0f}, (vec3) {0.0f, 0.0f, 0.0f}); // lookAt(scene.position)
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
    if (vs.gradient_program) {
        if (g_active_shader == vs.gradient_program) g_active_shader = 0;
        glDeleteProgram(vs.gradient_program);
    }
    if (vs.empty_vao) glDeleteVertexArrays(1, &vs.empty_vao);
    if (g_viewer.freeze) gltf_set_clock_override(-1.0);
    memset(&vs, 0, sizeof(vs));
}
