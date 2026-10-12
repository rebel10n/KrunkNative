// Krunker v7.1.4 ocean: libs/Ocean.js (Ocean.fromConfig, render.js init when map.ocean) on the
// three.js examples Water.js mirror (libs/Water.js). See client/include/ocean.h.
//
// Ocean.js: colour ocCol (#001e0f), opacity limitMM(ocOpac, 0, 1), distortion limitMM(ocDist,
// 0, 10), speed limitMM(ocSpd, 0.1, 5), y limitMM(ocYOff, -1000, 1000), size limitMM(ocW / ocL,
// 100, 20000), mirror size [32, 64, 128, 256, 1024, 2048][limitMM(reflectionQuality || ocRefRes,
// 1, 6) - 1]; Water: normal map textures/water_0.png, sun direction / colour from the map's
// skyLight at creation (white and zero without one), fog, FrontSide, transparent unless
// opacity 1.
// Differences (documented): the reflectionQuality setting is not ported (map dependent, its
// default); the mirror pass clips below the water with a discard plane instead of three's
// oblique near plane; the mirror camera keeps the player's roll (the sampled view is the same).
#include <gl.h>
#include <ocean.h>
#include <shadow.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define OCEAN_NORMAL_TEXTURE_UNIT 2 // the BasicMaterial emissive unit (no other texture of this draw)

typedef struct {
    Material base;
    float time;
} OceanMaterial;

typedef struct {
    int active;
    Mesh *mesh;
    Scene *scene;
    OceanMaterial *material;
    unsigned int program;
    unsigned int fbo, color, depth;
    int resolution;
    unsigned int normal_texture;
    vec3 color_water;
    float opacity, distortion, speed, y;
    vec3 sun_direction, sun_color;
    float texture_matrix[16];
    vec3 eye;
} Ocean;

static Ocean g_ocean;

static GlUniform u_mirror = GL_UNIFORM_INIT("mirror_sampler");
static GlUniform u_normal = GL_UNIFORM_INIT("normal_sampler");
static GlUniform u_alpha = GL_UNIFORM_INIT("alpha");
static GlUniform u_time = GL_UNIFORM_INIT("time");
static GlUniform u_size = GL_UNIFORM_INIT("size");
static GlUniform u_distortion = GL_UNIFORM_INIT("distortion_scale");
static GlUniform u_sun_color = GL_UNIFORM_INIT("sun_color");
static GlUniform u_sun_direction = GL_UNIFORM_INIT("sun_direction");
static GlUniform u_eye = GL_UNIFORM_INIT("eye");
static GlUniform u_water_color = GL_UNIFORM_INIT("water_color");
static GlUniform u_texture_matrix = GL_UNIFORM_INIT("texture_matrix");
static GlUniform u_fog_color = GL_UNIFORM_INIT("fog_color");
static GlUniform u_fog_near = GL_UNIFORM_INIT("fog_near");
static GlUniform u_fog_far = GL_UNIFORM_INIT("fog_far");
static GlUniform u_fog_enabled = GL_UNIFORM_INIT("fog_enabled");
static GlUniform u_tone_mapping = GL_UNIFORM_INIT("tone_mapping");
static GlUniform u_tone_mapping_exposure = GL_UNIFORM_INIT("tone_mapping_exposure");

static void ocean_material_update_uniforms(OceanMaterial *material) {
    const unsigned int program = material->base.program;
    // Water: FrontSide (the plane faces up), normal blending when transparent, depth writes
    scene_draw_state(1, 0, 1);

    glActiveTexture(GL_TEXTURE0 + OCEAN_NORMAL_TEXTURE_UNIT);
    glBindTexture(GL_TEXTURE_2D, g_ocean.normal_texture);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_ocean.color);
    g_active_texture = g_ocean.color;

    gl_uniform_1i(&u_mirror, program, 0);
    gl_uniform_1i(&u_normal, program, OCEAN_NORMAL_TEXTURE_UNIT);
    gl_uniform_1f(&u_alpha, program, g_ocean.opacity);
    gl_uniform_1f(&u_time, program, material->time);
    gl_uniform_1f(&u_size, program, 1.0f);
    gl_uniform_1f(&u_distortion, program, g_ocean.distortion);
    gl_uniform_3f(&u_sun_color, program, g_ocean.sun_color.x, g_ocean.sun_color.y, g_ocean.sun_color.z);
    gl_uniform_3f(&u_sun_direction, program, g_ocean.sun_direction.x, g_ocean.sun_direction.y, g_ocean.sun_direction.z);
    gl_uniform_3f(&u_eye, program, g_ocean.eye.x, g_ocean.eye.y, g_ocean.eye.z);
    gl_uniform_3f(&u_water_color, program, g_ocean.color_water.x, g_ocean.color_water.y, g_ocean.color_water.z);
    gl_uniform_mat4(&u_texture_matrix, program, GL_TRUE, g_ocean.texture_matrix);
    const RenderLighting *l = &g_render_lighting;
    gl_uniform_4f(&u_fog_color, program, l->fog_color.x, l->fog_color.y, l->fog_color.z, l->fog_color.w);
    gl_uniform_1f(&u_fog_near, program, l->fog_near);
    gl_uniform_1f(&u_fog_far, program, l->fog_far);
    gl_uniform_1i(&u_fog_enabled, program, l->fog_enabled);
    gl_uniform_1i(&u_tone_mapping, program, l->tone_mapping);
    gl_uniform_1f(&u_tone_mapping_exposure, program, l->tone_mapping_exposure);
}

static const MaterialVTable ocean_material_vtable = {(void *) ocean_material_update_uniforms};

static float ocean_clamp(const float v, const float lo, const float hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

static float ocean_float(const cJSON *raw, const char *key, const float fallback) {
    const cJSON *item = cJSON_GetObjectItem(raw, key);
    if (cJSON_IsNumber(item)) return (float) cJSON_GetNumberValue(item);
    if (cJSON_IsString(item)) return (float) atof(cJSON_GetStringValue(item));
    return fallback;
}

void ocean_load(const Map *map) {
    ocean_unload();
    if (!map || !map->ocean.enabled) return;
    const cJSON *raw = map->raw_data;

    if (!g_ocean.program) {
        char *vert = concat(assets_path(), "shaders/ocean.vert");
        char *frag = concat(assets_path(), "shaders/ocean.frag");
        g_ocean.program = vert && frag ? shader_compile(vert, frag) : 0;
        free(vert);
        free(frag);
        if (!g_ocean.program) return;
    }

    vec4 col = {0.0f, 30.0f / 255.0f, 15.0f / 255.0f, 1.0f}; // #001e0f
    const cJSON *oc_col = cJSON_GetObjectItem(raw, "ocCol");
    if (cJSON_IsString(oc_col)) parse_hex_color(cJSON_GetStringValue(oc_col), &col);
    g_ocean.color_water = (vec3) {col.x, col.y, col.z};
    // limitMM(undefined, ...) is NaN-free in v7 only through the editor defaults: use them
    g_ocean.opacity = ocean_clamp(ocean_float(raw, "ocOpac", 0.9f), 0.0f, 1.0f);
    g_ocean.distortion = ocean_clamp(ocean_float(raw, "ocDist", 3.7f), 0.0f, 10.0f);
    g_ocean.speed = ocean_clamp(ocean_float(raw, "ocSpd", 1.0f), 0.1f, 5.0f);
    g_ocean.y = ocean_float(raw, "ocYOff", -1.0f); // Ocean: baseMesh.position.y = the raw ocYOff
    const float width = ocean_clamp(ocean_float(raw, "ocW", 1000.0f), 100.0f, 20000.0f);
    const float length = ocean_clamp(ocean_float(raw, "ocL", 1000.0f), 100.0f, 20000.0f);
    static const int resolutions[6] = {32, 64, 128, 256, 1024, 2048};
    const int quality = (int) ocean_clamp(ocean_float(raw, "ocRefRes", 3.0f), 1.0f, 6.0f);
    g_ocean.resolution = resolutions[quality - 1];

    // skyLight ? position.normalize() / colour : (0, 0, 0) / white
    const int has_light = g_render_lighting.light_intensity > 0.0f;
    g_ocean.sun_direction = has_light ? g_render_lighting.light_direction : (vec3) {0.0f, 0.0f, 0.0f};
    g_ocean.sun_color = has_light ? (vec3) {g_render_lighting.light_color.x, g_render_lighting.light_color.y, g_render_lighting.light_color.z} : (vec3) {1.0f, 1.0f, 1.0f};

    char *normal_path = concat(assets_path(), "textures/water_0.png");
    g_ocean.normal_texture = normal_path ? load_texture(normal_path) : 0;
    free(normal_path);
    if (g_ocean.normal_texture) {
        glBindTexture(GL_TEXTURE_2D, g_ocean.normal_texture); // three.js TextureLoader defaults: linear, mipmapped
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        g_active_texture = g_ocean.normal_texture;
    }

    // mirror target: colour (linear, no mipmaps) + depth
    glGenTextures(1, &g_ocean.color);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_ocean.color);
    g_active_texture = g_ocean.color;
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, g_ocean.resolution, g_ocean.resolution, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenRenderbuffers(1, &g_ocean.depth);
    glBindRenderbuffer(GL_RENDERBUFFER, g_ocean.depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, g_ocean.resolution, g_ocean.resolution);
    GLint previous = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous);
    glGenFramebuffers(1, &g_ocean.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g_ocean.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_ocean.color, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, g_ocean.depth);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint) previous);

    // PlaneGeometry(width, length) rotated -PI/2 about x at y = ocYOff
    g_ocean.material = calloc(1, sizeof(OceanMaterial));
    if (!g_ocean.material) return;
    g_ocean.material->base.vtable = &ocean_material_vtable;
    g_ocean.material->base.program = g_ocean.program;
    g_ocean.material->base.transparent = g_ocean.opacity != 1.0f;
    static Geometry *plane;
    if (!plane) {
        static const vertex vertices[] = {
            {{-0.5f, -0.5f, 0.0f}, {0.0f, 0.0f}}, {{0.5f, 0.5f, 0.0f}, {1.0f, 1.0f}},
            {{-0.5f, 0.5f, 0.0f}, {0.0f, 1.0f}}, {{0.5f, -0.5f, 0.0f}, {1.0f, 0.0f}},
        };
        static const unsigned int indices[] = {0, 1, 2, 0, 3, 1};
        plane = calloc(1, sizeof(Geometry));
        if (plane) {
            unsigned int vbo;
            glGenVertexArrays(1, &plane->vao);
            glGenBuffers(1, &plane->ebo);
            glGenBuffers(1, &vbo);
            glBindVertexArray(plane->vao);
            glBindBuffer(GL_ARRAY_BUFFER, vbo);
            glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, plane->ebo);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), NULL);
            glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *) (3 * sizeof(float)));
            glEnableVertexAttribArray(0);
            glEnableVertexAttribArray(1);
            glBindVertexArray(0);
            plane->index_count = 6; // bound_radius 0: never frustum culled
        }
    }
    if (!plane) return;
    g_ocean.mesh = mesh_init(plane, (Material *) g_ocean.material);
    g_ocean.mesh->transform.position = (vec3) {0.0f, g_ocean.y, 0.0f};
    g_ocean.mesh->transform.rotation = (vec3) {-(float) M_PI / 2.0f, 0.0f, 0.0f};
    g_ocean.mesh->transform.scale = (vec3) {width, length, 1.0f};
    g_ocean.mesh->no_shadow_receive = 0;
    g_ocean.active = 1;
}

void ocean_unload(void) {
    if (g_ocean.scene && g_ocean.mesh) scene_remove_mesh(g_ocean.scene, g_ocean.mesh);
    free(g_ocean.mesh); // the material is g_ocean.material, the geometry is shared
    free(g_ocean.material);
    if (g_ocean.fbo) glDeleteFramebuffers(1, &g_ocean.fbo);
    if (g_ocean.color) glDeleteTextures(1, &g_ocean.color);
    if (g_ocean.depth) glDeleteRenderbuffers(1, &g_ocean.depth);
    if (g_active_texture == g_ocean.color) g_active_texture = 0;
    const unsigned int program = g_ocean.program;
    memset(&g_ocean, 0, sizeof(g_ocean));
    g_ocean.program = program; // kept for the next map
}

void ocean_update(Scene *scene, Camera *camera, const float delta_ms) {
    if (!g_ocean.active || !g_ocean.mesh || !scene || !camera) return;
    if (g_ocean.scene != scene) {
        if (g_ocean.scene) scene_remove_mesh(g_ocean.scene, g_ocean.mesh);
        scene_add_mesh(scene, g_ocean.mesh);
        g_ocean.scene = scene;
    }
    g_ocean.material->time += delta_ms / 1000.0f * g_ocean.speed;
    g_ocean.eye = camera->position;

    // Water.onBeforeRender: no update with the camera under the plane (normal +y)
    if (camera->position.y < g_ocean.y) return;

    GLint previous = 0, viewport[4];
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous);
    glGetIntegerv(GL_VIEWPORT, viewport);

    // mirror camera: the reflected position looking at the reflected target, the main
    // projection (same fov, near, far and aspect) on the square target
    Camera mirror = *camera;
    mirror.position.y = 2.0f * g_ocean.y - camera->position.y;
    mirror.rotation.x = -camera->rotation.x;
    mirror.aspect_override = viewport[3] > 0 ? (float) viewport[2] / (float) viewport[3] : 1.0f;

    glBindFramebuffer(GL_FRAMEBUFFER, g_ocean.fbo);
    glViewport(0, 0, g_ocean.resolution, g_ocean.resolution);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT); // the renderer's clear colour (sky); no sky dome
    const float clip[4] = {0.0f, 1.0f, 0.0f, -g_ocean.y};
    basic_material_set_clip_plane(clip);
    g_ocean.mesh->visible = 0;
    shadow_begin_world();
    scene_render(scene, &mirror);
    shadow_end_world();
    g_ocean.mesh->visible = 1;
    basic_material_set_clip_plane(NULL);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint) previous);
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);

    // textureMatrix = bias * projection * view of the mirror camera
    static const float bias[16] = {
        0.5f, 0.0f, 0.0f, 0.5f,
        0.0f, 0.5f, 0.0f, 0.5f,
        0.0f, 0.0f, 0.5f, 0.5f,
        0.0f, 0.0f, 0.0f, 1.0f,
    };
    float view_projection[16];
    mat4x4(mirror.projection_matrix, mirror.world_inverse_matrix, view_projection);
    mat4x4(bias, view_projection, g_ocean.texture_matrix);
}
