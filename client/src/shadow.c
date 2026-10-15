// Krunker v3.1.1 sun shadow port.
//
// JS reference (render.js, three.js r121):
//   renderer.shadowMap = {enabled: true, autoUpdate: false, type: BasicShadowMap}  (settings
//   "shadows" on, "softShad"/"highResShad"/"shadowsDynamic" off by default)
//   skyLight.position = lightD * (cos v, sin v sin u, sin v cos u), target at the origin
//   skyLight.shadow.mapSize = shadowR || 1024; shadow.camera.far = shadowD || 1200 (near 0.5)
//   skyLight.shadow.bias = -0.0004
//   updateLightMap(): camera left/right = -/+shadWidth, top/bottom = +/-shadLength, from
//   map.js generate(): over CUBE objects without l/col, starting from 0,
//     a7 = min(x - sx), a8 = max(x + sx), a9 = min(z - sz), aa = max(z + sz)
//     shadLength = round(|a8 - a7| / 2.5), shadWidth = round(|aa - a9| / 2.5)
//   The map is only re-rendered on needsUpdate (map load / mesh adds), so players never
//   cast. Receivers: getShadowMask() scales the direct light only, single nearest-texel
//   compare (shadowCoord.z + bias <= depth), lit outside the camera frustum.
// Casters render their back faces (three.js shadowSide[FrontSide] = BackSide).
// Settings (render_settings.h): "shadows" off clears the map to fully lit (toggleShadowMap),
// "highResShad" uses a 4096 map; "softShad" (PCFSoftShadowMap) and "shadowsDynamic"
// (players casting, per-frame updates) are not ported and render as their defaults (off).
#include <gl.h>
#include <client.h>
#include <shadow.h>
#include <render_settings.h>
#include <prefabs.h>
#include <terrain.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

// --- tunables (Krunker config.js / render.js defaults) -------------------------------
#define SHADOW_DEFAULT_RES 1024       // config.shadowRes (map shadowR overrides)
#define SHADOW_HIGH_RES 4096          // highResShad (restart) size, also the upper clamp
#define SHADOW_DEFAULT_FAR 1200.0f    // config.shadowDst (map shadowD overrides)
#define SHADOW_NEAR 0.5f              // three.js DirectionalLightShadow camera near
#define SHADOW_BIAS -0.0004f          // skyLight.shadow.bias
static float g_shadow_normal_bias;    // v10 skyLight.shadow.normalBias (map shadowNB), 0 in v7
#define SHADOW_LIGHT_DISTANCE 500.0f  // config.lightDistance (map lightD overrides)
#define SHADOW_EXTENT_DIVISOR 2.5f    // map.js shadLength/shadWidth = extent / 2.5

typedef struct {
    int enabled;      // a DirectionalLight exists (map.light or mode lightCol) and lighting is on
    int dirty;
    int resolution;
    float light_view_projection[16];
    float shadow_matrix[16]; // bias * projection * view: world -> [0,1] shadow coords

    size_t caster_count;
    size_t caster_capacity;
    Mesh **casters;
    unsigned char *caster_double_sided;

    int world_active;

    unsigned int fbo;
    unsigned int depth_texture;
    unsigned int texture_resolution;
    unsigned int program;
} ShadowState;

static ShadowState shadow;

static float json_number_or(const cJSON *json, const float fallback) {
    return cJSON_IsNumber(json) ? (float) cJSON_GetNumberValue(json) : fallback;
}

static int json_truthy(const cJSON *json) {
    if (!json || cJSON_IsNull(json) || cJSON_IsFalse(json)) return 0;
    if (cJSON_IsNumber(json)) return cJSON_GetNumberValue(json) != 0;
    if (cJSON_IsString(json)) return cJSON_GetStringValue(json)[0] != 0;
    return 1;
}

// three.js Matrix4.lookAt(eye, target, up) composed with the eye translation, inverted:
// the shadow camera's matrixWorldInverse (row-major).
static void shadow_look_at_inverse(const vec3 eye, float out[16]) {
    vec3 z = {eye.x, eye.y, eye.z}; // eye - target (target = origin)
    float len = sqrtf(z.x * z.x + z.y * z.y + z.z * z.z);
    if (len == 0.0f) z.z = 1.0f, len = 1.0f;
    z.x /= len; z.y /= len; z.z /= len;

    // x = up(0,1,0) x z
    vec3 x = {z.z, 0.0f, -z.x};
    if (x.x * x.x + x.z * x.z == 0.0f) {
        z.z += 0.0001f;
        len = sqrtf(z.x * z.x + z.y * z.y + z.z * z.z);
        z.x /= len; z.y /= len; z.z /= len;
        x = (vec3) {z.z, 0.0f, -z.x};
    }
    len = sqrtf(x.x * x.x + x.y * x.y + x.z * x.z);
    x.x /= len; x.y /= len; x.z /= len;

    const vec3 y = {z.y * x.z - z.z * x.y, z.z * x.x - z.x * x.z, z.x * x.y - z.y * x.x};

    const float view[16] = {
        x.x, x.y, x.z, -(x.x * eye.x + x.y * eye.y + x.z * eye.z),
        y.x, y.y, y.z, -(y.x * eye.x + y.y * eye.y + y.z * eye.z),
        z.x, z.y, z.z, -(z.x * eye.x + z.y * eye.y + z.z * eye.z),
        0.0f, 0.0f, 0.0f, 1.0f,
    };
    memcpy(out, view, sizeof(view));
}

// three.js Matrix4.makeOrthographic(left, right, top, bottom, near, far) (row-major).
static void shadow_orthographic(const float left, const float right, const float top, const float bottom,
                                const float near, const float far, float out[16]) {
    const float w = 1.0f / (right - left);
    const float h = 1.0f / (top - bottom);
    const float p = 1.0f / (far - near);
    const float projection[16] = {
        2.0f * w, 0.0f, 0.0f, -(right + left) * w,
        0.0f, 2.0f * h, 0.0f, -(top + bottom) * h,
        0.0f, 0.0f, -2.0f * p, -(far + near) * p,
        0.0f, 0.0f, 0.0f, 1.0f,
    };
    memcpy(out, projection, sizeof(projection));
}

// map.js generate(): shadLength (x extent) and shadWidth (z extent) of the solid cubes.
static void shadow_map_extents(const cJSON *raw, float *shad_length, float *shad_width) {
    // v7.1.4 map.js: a terrain map takes terrain.sizeX / sizeY * 1.2 (map-G)
    if (terrain_enabled_json(raw)) {
        const cJSON *w = cJSON_GetObjectItem(raw, "terrainWidth"), *h = cJSON_GetObjectItem(raw, "terrainHeight");
        const float size_x = fminf(fmaxf(cJSON_IsNumber(w) ? (float) cJSON_GetNumberValue(w) : 0.0f, 0.0f), 8000.0f);
        const float size_y = fminf(fmaxf(cJSON_IsNumber(h) ? (float) cJSON_GetNumberValue(h) : 0.0f, 0.0f), 8000.0f);
        *shad_length = size_x * 1.2f;
        *shad_width = size_y * 1.2f;
        return;
    }
    float min_x = 0.0f, max_x = 0.0f, min_z = 0.0f, max_z = 0.0f;
    const cJSON *objects = cJSON_GetObjectItem(raw, "objects");
    const cJSON *object;

    cJSON_ArrayForEach(object, objects) {
        const cJSON *id = cJSON_GetObjectItem(object, "i");
        if (cJSON_IsNumber(id) && cJSON_GetNumberValue(id) != PREFAB_CUBE) continue;
        if (json_truthy(cJSON_GetObjectItem(object, "l")) || json_truthy(cJSON_GetObjectItem(object, "col"))) continue;

        const cJSON *p = cJSON_GetObjectItem(object, "p");
        const cJSON *s = cJSON_GetObjectItem(object, "s");
        if (cJSON_GetArraySize(p) < 3 || cJSON_GetArraySize(s) < 3) continue;

        const float x = json_number_or(cJSON_GetArrayItem(p, 0), 0.0f);
        const float z = json_number_or(cJSON_GetArrayItem(p, 2), 0.0f);
        const float sx = json_number_or(cJSON_GetArrayItem(s, 0), 0.0f);
        const float sz = json_number_or(cJSON_GetArrayItem(s, 2), 0.0f);

        if (x - sx < min_x) min_x = x - sx;
        if (x + sx > max_x) max_x = x + sx;
        if (z - sz < min_z) min_z = z - sz;
        if (z + sz > max_z) max_z = z + sz;
    }

    *shad_length = roundf(fabsf(max_x - min_x) / SHADOW_EXTENT_DIVISOR);
    *shad_width = roundf(fabsf(max_z - min_z) / SHADOW_EXTENT_DIVISOR);
}

// castShadow / receiveShadow of a map object's mesh, from the v3.1.1 builders that
// map.js generate() calls (src/data/objects.js; RENDER.add sets castShadow = shadows,
// receiveShadow = shadows || shadowsR):
//   addBlock / addCylinder / addGate / addBank / addTrigger / addTerminal:
//       shadows = !shadowsR && opacity == 1                    -> both when opaque
//   addLadder: shadows = true; addPad (boost pad), addAsset: shadows + shadowsR -> both
//   addMesh (props incl. ghost/pumpkin/skeleton/knight): prefab castShadow/receiveShadow
//   addRamp, addPlane (PLANE, BILLBOARD, RUNE), addLiquid, addGrass, addWeapon,
//   addShowcase, addBombSite: shadowsR only                   -> receive only
//   addCanvas (SIGN), addLightCone, addSphere, addObjective, flags (noShadow): neither
//   zones, check points, teleporters, spawns, cameras, particles, sound emitters,
//   point lights: no mesh in v3.1.1                            -> neither
//   addMesh (the OBJ props): prefab castShadow / receiveShadow (both true for all of them)
// BOT (addAI) receives but does not cast. Caster sides follow three.js r121
// getDepthMaterial: shadowSide[material.side] (FrontSide -> BackSide, DoubleSide -> both);
// only cylinders (addCylinder: DoubleSide) and boost pads are double-sided casters.
static void shadow_prefab_flags(const Object *object, const Mesh *mesh, int *cast, int *receive, int *double_sided) {
    const int opaque = !mesh->material->transparent;
    *double_sided = 0;

    switch (object->prefab) {
        case PREFAB_CUBE:
        case PREFAB_GATE:
        case PREFAB_DEPOSIT_BOX:
        case PREFAB_TRIGGER:
        case PREFAB_TERMINAL:
            *cast = *receive = opaque;
            return;
        case PREFAB_CYLINDER:
            *cast = *receive = opaque;
            *double_sided = 1;
            return;
        case PREFAB_LADDER:
        case PREFAB_CUSTOM_ASSET:
        case PREFAB_CRATE: case PREFAB_BARREL: case PREFAB_VEHICLE: case PREFAB_STACK:
        case PREFAB_TREE: case PREFAB_CONE: case PREFAB_CONTAINER: case PREFAB_CONTAINER_R:
        case PREFAB_ACID_BARREL: case PREFAB_DOOR: case PREFAB_WINDOW: case PREFAB_TEDDY:
        case PREFAB_CARD_B: case PREFAB_PALLET: case PREFAB_EVENT: case PREFAB_POLICE:
        case PREFAB_CAGE: case PREFAB_E_BARREL:
        case PREFAB_GHOST: case PREFAB_PUMPKIN: case PREFAB_SKELETON: case PREFAB_KNIGHT:
            // FrontSide materials (addMesh: side = prefab doubleSide ? DoubleSide : FrontSide,
            // and no prop prefab is doubleSide): three.js r121 renders them into the shadow
            // map with shadowSide[FrontSide] = BackSide, i.e. back faces only. Drawing the
            // front faces too self-shadows the lit side (stripes on the vehicle).
            *cast = *receive = 1;
            return;
        case PREFAB_BOOST_PAD: // addPad: loadMesh side 2 (DoubleSide)
            *cast = *receive = 1;
            *double_sided = 1;
            return;
        case PREFAB_RAMP:
        case PREFAB_PLANE:
        case PREFAB_BILLBOARD:
        case PREFAB_RUNE:
        case PREFAB_LIQUID:
        case PREFAB_GRASS:
        case PREFAB_WEAPON_PICKUP:
        case PREFAB_SHOWCASE:
        case PREFAB_BOMB_SITE:
        case PREFAB_BOT: // addAI
            *cast = 0;
            *receive = 1;
            return;
        // v7.1.4 (map-G): ROPE_LADDER addCube shadows = opacity == 1; SPRAY / SPRITE / LIGHT_BAR
        // shadowsR (a sprite or an additive bar never shows it); the machines shadows false
        case PREFAB_ROPE_LADDER:
            *cast = *receive = opaque;
            return;
        case PREFAB_SPRAY:
            *cast = 0;
            *receive = 1;
            return;
        default: { // SIGN, LIGHT_CONE, SPHERE, OBJECTIVE, FLAG, zones, helpers, machines
            const PrefabInfo *info = prefab_info(object->prefab);
            if (info && info->geometry == PREFAB_GEO_MODEL) {
                // MapManager.addMesh: shadows = prefab castShadow, shadowsR = receiveShadow
                *cast = prefab_has_flag(object->prefab, PF_CASTSHADOW);
                *receive = *cast || prefab_has_flag(object->prefab, PF_RECEIVESHADOW);
                return;
            }
            *cast = *receive = 0;
            return;
        }
    }
}

static void shadow_add_caster(Mesh *mesh, const int double_sided) {
    if (shadow.caster_count == shadow.caster_capacity) {
        const size_t capacity = shadow.caster_capacity ? shadow.caster_capacity * 2 : 256;
        Mesh **casters = realloc(shadow.casters, capacity * sizeof(Mesh *));
        if (!casters) return;
        shadow.casters = casters;
        unsigned char *sides = realloc(shadow.caster_double_sided, capacity);
        if (!sides) return;
        shadow.caster_double_sided = sides;
        shadow.caster_capacity = capacity;
    }

    shadow.casters[shadow.caster_count] = mesh;
    shadow.caster_double_sided[shadow.caster_count] = (unsigned char) double_sided;
    shadow.caster_count++;
}

static const cJSON *g_shadow_raw; // the loaded map's JSON (shadow_environment)

// The light camera of the sun (map-G: shared by the map load and the environment zones).
static void shadow_setup_light(const int shadow_r, const int shadow_d, const float light_d) {
    const cJSON *raw = g_shadow_raw;
    shadow.enabled = g_render_lighting.enabled && g_render_lighting.light_intensity > 0.0f;
    if (!shadow.enabled || !raw) return;

    // highResShad ? 4096 : p.shadowR || shadowRes
    int resolution = shadow_r >= 0 ? shadow_r : (int) json_number_or(cJSON_GetObjectItem(raw, "shadowR"), 0.0f);
    if (resolution <= 0) resolution = SHADOW_DEFAULT_RES;
    if (client_render_settings()->high_res_shadows) resolution = SHADOW_HIGH_RES;
    if (resolution > SHADOW_HIGH_RES) resolution = SHADOW_HIGH_RES;
    shadow.resolution = resolution;

    float far = shadow_d >= 0 ? (float) shadow_d : json_number_or(cJSON_GetObjectItem(raw, "shadowD"), 0.0f); // p.shadowD || shadowDst
    if (far <= 0.0f) far = SHADOW_DEFAULT_FAR;

    const float distance = light_d >= 0.0f ? light_d : json_number_or(cJSON_GetObjectItem(raw, "lightD"), SHADOW_LIGHT_DISTANCE);
    const vec3 eye = {
        g_render_lighting.light_direction.x * distance,
        g_render_lighting.light_direction.y * distance,
        g_render_lighting.light_direction.z * distance,
    };

    float shad_length, shad_width;
    shadow_map_extents(raw, &shad_length, &shad_width);

    float view[16], projection[16];
    shadow_look_at_inverse(eye, view);
    shadow_orthographic(-shad_width, shad_width, shad_length, -shad_length, SHADOW_NEAR, far, projection);
    mat4x4(projection, view, shadow.light_view_projection);

    static const float bias[16] = {
        0.5f, 0.0f, 0.0f, 0.5f,
        0.0f, 0.5f, 0.0f, 0.5f,
        0.0f, 0.0f, 0.5f, 0.5f,
        0.0f, 0.0f, 0.0f, 1.0f,
    };
    mat4x4(bias, shadow.light_view_projection, shadow.shadow_matrix);
    shadow.dirty = 1;
}


void shadow_load_map(const Map *map) {
    shadow_unload_map();
    if (!map || !map->raw_data) return;

    const cJSON *raw = map->raw_data;
    // A DirectionalLight exists when map.light or the mode's lightCol is set
    // (client_apply_map_lighting gives it a non-zero intensity then); Low lighting
    // (MeshBasicMaterial) receives no light and so no shadow.
    shadow.enabled = g_render_lighting.enabled && g_render_lighting.light_intensity > 0.0f;

    for (size_t i = 0; i < map->object_count; i++) {
        const Object *object = map->objects[i];
        Mesh *mesh = object->mesh;
        if (!mesh || !mesh->material) continue;

        int cast, receive, double_sided;
        shadow_prefab_flags(object, mesh, &cast, &receive, &double_sided);
        mesh->no_shadow_receive = !receive;
        if (cast && mesh->visible) shadow_add_caster(mesh, double_sided);
    }

    g_shadow_raw = raw;
    g_shadow_normal_bias = map->env.shadow_nb;
    shadow_setup_light(-1, -1, -1.0f);
}

void shadow_set_normal_bias(const float normal_bias) {
    g_shadow_normal_bias = normal_bias;
}

// v7 render.js updateEnvironment: an ENVIRONMENT_ZONE re-creates the sun with its own
// shadowR / shadowD / lightD (and direction, already in g_render_lighting) over the map's
// shadow extents; -1 = the map's raw values.
void shadow_environment(const int shadow_r, const int shadow_d, const float light_d) {
    if (!g_shadow_raw) return;
    shadow_setup_light(shadow_r, shadow_d, light_d);
}

void shadow_unload_map(void) {
    g_shadow_raw = NULL;
    shadow.caster_count = 0;
    shadow.enabled = 0;
    shadow.dirty = 0;
}

static int shadow_ensure_target(void) {
    if (!shadow.program) {
        char *vert = concat(assets_path(), "shaders/shadow.vert");
        char *frag = concat(assets_path(), "shaders/shadow.frag");
        shadow.program = shader_compile(vert, frag);
        free(vert);
        free(frag);
        if (!shadow.program) return 0;
    }

    if (shadow.depth_texture && shadow.texture_resolution == (unsigned int) shadow.resolution) return 1;

    if (!shadow.fbo) glGenFramebuffers(1, &shadow.fbo);
    if (!shadow.depth_texture) glGenTextures(1, &shadow.depth_texture);

    glActiveTexture(GL_TEXTURE0 + SHADOW_TEXTURE_UNIT);
    glBindTexture(GL_TEXTURE_2D, shadow.depth_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, shadow.resolution, shadow.resolution, 0,
                 GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, NULL);
    // BasicShadowMap: nearest texel, no hardware compare (basic.frag compares manually).
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glActiveTexture(GL_TEXTURE0);

    int previous_fbo;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, shadow.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, shadow.depth_texture, 0);
    const GLenum none = GL_NONE;
    glDrawBuffers(1, &none);
    glReadBuffer(GL_NONE);
    const int complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, (unsigned int) previous_fbo);

    shadow.texture_resolution = (unsigned int) shadow.resolution;
    return complete;
}

static float shadow_determinant3(const float m[16]) {
    return m[0] * (m[5] * m[10] - m[6] * m[9])
         - m[1] * (m[4] * m[10] - m[6] * m[8])
         + m[2] * (m[4] * m[9] - m[5] * m[8]);
}

void shadow_update(void) {
    if (!shadow.enabled || !shadow.dirty) return;
    shadow.dirty = 0;
    if (!shadow_ensure_target()) {
        shadow.enabled = 0;
        return;
    }

    // The world may be drawn into the scaled scene target (render_target.c): restore
    // whatever framebuffer and viewport were bound.
    int viewport[4], previous_fbo;
    glGetIntegerv(GL_VIEWPORT, viewport);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous_fbo);

    // the depth map stays on SHADOW_TEXTURE_UNIT from the last lit pass (shadow_begin_world);
    // drawing into it while bound is a feedback loop on WebGL / ANGLE
    gl_release_texture(shadow.depth_texture);
    glBindFramebuffer(GL_FRAMEBUFFER, shadow.fbo);
    glViewport(0, 0, shadow.resolution, shadow.resolution);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glClearDepthf(1.0f); // GLES3 has only the float variant
    glClear(GL_DEPTH_BUFFER_BIT);
#ifndef KN_GLES
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
#endif

    glUseProgram(shadow.program);
    g_active_shader = shadow.program;
    glUniformMatrix4fv(glGetUniformLocation(shadow.program, "light_view_projection"), 1, GL_TRUE, shadow.light_view_projection);
    const int transform = glGetUniformLocation(shadow.program, "transform");

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    for (size_t i = 0; i < shadow.caster_count; i++) {
        Mesh *mesh = shadow.casters[i];
        if (!mesh->visible || !mesh->geometry) continue;

        mesh_update_transform_matrix(mesh);

        if (shadow.caster_double_sided[i]) {
            glDisable(GL_CULL_FACE);
        } else {
            // Native geometry is counter-clockwise outward; draw only back faces, and keep
            // that true for mirrored (negative-determinant) transforms.
            glEnable(GL_CULL_FACE);
            glFrontFace(shadow_determinant3(mesh->transform_matrix) < 0.0f ? GL_CW : GL_CCW);
            glCullFace(GL_FRONT);
        }

        glUniformMatrix4fv(transform, 1, GL_TRUE, mesh->transform_matrix);
        glBindVertexArray(mesh->geometry->vao);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh->geometry->ebo);
        glDrawElements(GL_TRIANGLES, mesh->geometry->index_count, GL_UNSIGNED_INT, NULL);
    }

    glDisable(GL_CULL_FACE);
    glFrontFace(GL_CCW);
    glCullFace(GL_BACK);
    glBindVertexArray(0);
    glBindFramebuffer(GL_FRAMEBUFFER, (unsigned int) previous_fbo);
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
}

void shadow_begin_world(void) {
    // settings "shadows" off: toggleShadowMap(false) clears the map, so everything is lit.
    shadow.world_active = shadow.enabled && shadow.texture_resolution > 0 && client_render_settings()->shadows;
    if (!shadow.world_active) return;

    glActiveTexture(GL_TEXTURE0 + SHADOW_TEXTURE_UNIT);
    glBindTexture(GL_TEXTURE_2D, shadow.depth_texture);
    glActiveTexture(GL_TEXTURE0); // g_active_texture tracks unit 0 only
}

void shadow_end_world(void) {
    shadow.world_active = 0;
}

static GlUniform u_shadow_enabled = GL_UNIFORM_INIT("shadow_enabled");
static GlUniform u_shadow_matrix = GL_UNIFORM_INIT("shadow_matrix");
static GlUniform u_shadow_bias = GL_UNIFORM_INIT("shadow_bias");
static GlUniform u_shadow_normal_bias = GL_UNIFORM_INIT("shadow_normal_bias");
static GlUniform u_shadow_map = GL_UNIFORM_INIT("shadow_map");

void shadow_apply_mesh_uniforms(const Mesh *mesh) {
    const unsigned int program = mesh->material->program;
    if (gl_uniform_location(&u_shadow_enabled, program) < 0) return;

    const int receive = shadow.world_active && !mesh->no_shadow_receive;
    gl_uniform_1i(&u_shadow_enabled, program, receive);
    if (!receive) return;

    gl_uniform_mat4(&u_shadow_matrix, program, GL_TRUE, shadow.shadow_matrix);
    gl_uniform_1f(&u_shadow_bias, program, SHADOW_BIAS);
    gl_uniform_1f(&u_shadow_normal_bias, program, g_shadow_normal_bias);
    gl_uniform_1i(&u_shadow_map, program, SHADOW_TEXTURE_UNIT);
}

void shadow_fini(void) {
    if (shadow.fbo) glDeleteFramebuffers(1, &shadow.fbo);
    if (shadow.depth_texture) glDeleteTextures(1, &shadow.depth_texture);
    if (shadow.program) glDeleteProgram(shadow.program);
    if (g_active_shader == shadow.program) g_active_shader = 0;
    free(shadow.casters);
    free(shadow.caster_double_sided);
    memset(&shadow, 0, sizeof(shadow));
}
