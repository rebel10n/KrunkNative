// Krunker v7.1.4 map environment on the client (client/include/map_environment.h):
//   render.js init(): toneMapping (config.toneTypes), toneMappingExposure (limitMM 0..10,
//   round 1), outputEncoding (config.encodingTypes: 1 = sRGB), useLegacyLights = !correctLights,
//   the ocean (Ocean.fromConfig, map only), the sky dome (skipped when the mode sets skyCol);
//   render.js updateEnvironment(idx): the camera's ENVIRONMENT_ZONE (index.js: the first zone
//   whose box holds the camera, pointInBox3D) re-runs init(zone data, mode, keep scene) with the
//   map's shadow extents and updates the shadow map; leaving every zone restores the map (-1).
#include <gl.h> // glad natively, GLES3 on the web (before GLFW)
#include <map_environment.h>
#include <client.h>
#include <world.h>
#include <sky.h>
#include <shadow.h>
#include <ocean.h>
#include <math.h>
#include <string.h>

static const Map *g_env_map;
static const GameMode *g_env_mode;
static RenderLighting g_env_map_lighting; // zone -1
static int g_env_zone = -1;

static void env_output(RenderLighting *l, const MapEnvironment *env) {
    l->tone_mapping = env->tone_mapping;
    l->tone_mapping_exposure = env->tone_mapping_exposure;
    l->output_encoding = env->output_encoding;
    // v10.0.2 (THREE r186) has no legacy lights: "correctLights" is still parsed but every map
    // renders with physical lights (renderer init L199044-199061), their intensities x PI (basic.c)
    l->correct_lights = 1;
}

void map_environment_map_loaded(const Map *map, const GameMode *mode) {
    g_env_map = map;
    g_env_mode = mode;
    g_env_zone = -1;
    if (!map) return;
    env_output(&g_render_lighting, &map->env);
    g_env_map_lighting = g_render_lighting;
    ocean_load(map);
}

void map_environment_unload(void) {
    ocean_unload();
    g_env_map = NULL;
    g_env_mode = NULL;
    g_env_zone = -1;
    // the menu / other pages render with v3.1.1 output (no tone mapping)
    g_render_lighting.tone_mapping = 0;
    g_render_lighting.tone_mapping_exposure = 1.0f;
    g_render_lighting.output_encoding = 0;
    g_render_lighting.correct_lights = 0;
}

static vec4 env_hex(const char *hex) {
    vec4 color = {1.0f, 1.0f, 1.0f, 1.0f};
    if (hex && hex[0]) parse_hex_color(hex, &color);
    return color;
}

// the raw ENVIRONMENT_ZONE record of zone `index` (its sky dome keys), in file order
static const cJSON *env_zone_raw(const Map *map, const int index) {
    const cJSON *objects = cJSON_GetObjectItem(map->raw_data, "objects");
    int found = 0;
    const cJSON *raw;
    cJSON_ArrayForEach(raw, objects) {
        if (map_raw_object_prefab(raw) != PREFAB_ENVIRONMENT_ZONE || !map_raw_object_kept(raw)) continue;
        if (found++ == index) return raw;
    }
    return NULL;
}

// render.js sceneInit / init for a zone's data (MapManager.addEnvironmentZone: every key has a
// value, "ambientC" / "fogC" are its ambient / fog); the mode's ambCol / lightCol / fogC / fogD
// / skyCol still win, as for the map.
static void env_apply_zone(const Map *map, const int index) {
    const MapEnvironment *env = &map->env_zones[index].env;
    const GameModeConfig *mode = g_env_mode ? &g_env_mode->config : NULL;
    RenderLighting *l = &g_render_lighting;

    l->ambient_color = mode && mode->has_amb_col ? hex_to_vec(mode->amb_col) : env_hex(env->ambient);
    l->ambient_intensity = env->ambient_i;
    l->light_color = mode && mode->has_light_col ? hex_to_vec(mode->light_col) : env_hex(env->light);
    l->light_intensity = env->light_i;
    const float u = (float) M_PI * (env->sun_ang_y / -180.0f);
    const float v = (float) M_PI * (env->sun_ang_x / -180.0f);
    vec3 dir = {cosf(v), sinf(v) * sinf(u), sinf(v) * cosf(u)};
    const float len = sqrtf(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (len > 0.0f) dir = (vec3) {dir.x / len, dir.y / len, dir.z / len};
    l->light_direction = dir;
    l->fog_color = mode && mode->has_fog_col ? hex_to_vec(mode->fog_col) : env_hex(env->fog);
    l->fog_near = 1.0f;
    l->fog_far = mode && mode->has_fog_dist ? mode->fog_dist : env->fog_d;
    l->fog_enabled = l->fog_far > l->fog_near;
    const int mode_sky = mode && mode->has_sky_col;
    l->sky_color = mode_sky ? hex_to_vec(mode->sky_col) : env_hex(env->sky);
    env_output(l, env);

    glClearColor(l->sky_color.x, l->sky_color.y, l->sky_color.z, 1.0f);
    sky_load(mode_sky ? NULL : env_zone_raw(map, index));
    shadow_set_normal_bias(env->shadow_nb); // a zone's data has no shadowNB: 0
    shadow_environment(env->shadow_r, env->shadow_d, env->light_d);
}

// Switches the global lighting / sky / shadow state to zone `zone` of the loaded map (-1 = the map's own).
static void env_apply_state(const int zone) {
    if (zone < 0) {
        g_render_lighting = g_env_map_lighting;
        glClearColor(g_render_lighting.sky_color.x, g_render_lighting.sky_color.y, g_render_lighting.sky_color.z, 1.0f);
        const int mode_sky = g_env_mode && g_env_mode->config.has_sky_col;
        sky_load(mode_sky ? NULL : g_env_map->raw_data);
        shadow_set_normal_bias(g_env_map->env.shadow_nb);
        shadow_environment(-1, -1, -1.0f);
    } else {
        env_apply_zone(g_env_map, zone);
    }
}

int map_environment_update(const Map *map, const vec3 camera_position) {
    if (!map || map != g_env_map) return -1;
    const int zone = world_environment_zone_at(map, camera_position);
    if (zone == g_env_zone) return zone;

    env_apply_state(zone);
    g_env_zone = zone;
    return zone;
}

void map_environment_restore(void) {
    if (!g_env_map) { // no map: the v3.1.1 output the menu and other pages render with (map_environment_unload)
        g_render_lighting.tone_mapping = 0;
        g_render_lighting.tone_mapping_exposure = 1.0f;
        g_render_lighting.output_encoding = 0;
        g_render_lighting.correct_lights = 0;
        return;
    }
    env_apply_state(g_env_zone);
}
