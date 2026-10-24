// Krunker v10.0.3 map environment on the client (client/include/map_environment.h):
//   render init() (deob-10.0.3.js:231079-231111): toneMapping (toneTypes), toneMappingExposure
//   (limitMM 0..10, round 1), outputEncoding (read, no effect: ColorManagement off), physical lights,
//   the ocean (Ocean.fromConfig, map only), the sky dome (skipped when the mode sets skyCol);
//   updateEnvironment(idx) (deob:234508-234531): the camera's ENVIRONMENT_ZONE (the first zone
//   whose box holds the camera) re-runs init(zone data, mode, keep scene) with the map's shadow
//   extents and updates the shadow map; leaving every zone re-runs it with the map's data (-1),
//   which keeps the zone's lights where the map defines none (env_restore_map). The map load also
//   builds getEnvMap for the GLB reflections (env_map.h).
#include <gl.h> // glad natively, GLES3 on the web (before GLFW)
#include <map_environment.h>
#include <client.h>
#include <world.h>
#include <sky.h>
#include <shadow.h>
#include <ocean.h>
#include <env_map.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static const Map *g_env_map;
static const GameMode *g_env_mode;
static RenderLighting g_env_map_lighting; // zone -1
static int g_env_zone = -1;
static RenderLighting g_env_current;      // what the environment last applied (map_environment_restore)
static int g_env_sun_zone = -1;           // the zone whose sun g_render_lighting holds (-1: the map's)

static void env_output(RenderLighting *l, const MapEnvironment *env) {
    l->tone_mapping = env->tone_mapping;
    l->tone_mapping_exposure = env->tone_mapping_exposure;
    // outputEncoding is still read (init, deob-10.0.3.js:231105-231109) but has no effect in v10.0.3: Krunker
    // turns ColorManagement off (deob:230030), so the output colour space conversion is a no-op (basic.frag)
    l->output_encoding = env->output_encoding;
    // v10.0.2 (THREE r186) has no legacy lights: "correctLights" is still parsed but every map
    // renders with physical lights (renderer init L199044-199061), their intensities x PI (basic.c)
    l->correct_lights = 1;
}

static int env_truthy(const cJSON *json) { // JS truthiness of a map key
    if (!json || cJSON_IsNull(json) || cJSON_IsFalse(json)) return 0;
    if (cJSON_IsNumber(json)) return cJSON_GetNumberValue(json) != 0;
    if (cJSON_IsString(json)) return cJSON_GetStringValue(json)[0] != 0;
    return 1;
}

// THREE.Color.set(value) changes the colour (a hex number or a colour string), else keeps it (undefined / null)
static int env_color_given(const cJSON *json) {
    vec4 parsed;
    return cJSON_IsNumber(json) || (cJSON_IsString(json) && !parse_hex_color(cJSON_GetStringValue(json), &parsed));
}

// skyDomeInit's skyDomeCols for the map's own dome (deob-10.0.3.js:231159): [col0, col1, col2].filter(Boolean)
static void env_note_map_dome(const cJSON *raw) {
    static const char *const keys[3] = {"skyDomeCol0", "skyDomeCol1", "skyDomeCol2"};
    char hex[3][8];
    const char *colors[3];
    int count = 0;
    for (int i = 0; i < 3; i++) {
        const cJSON *c = cJSON_GetObjectItem(raw, keys[i]);
        if (!env_truthy(c)) continue;
        if (cJSON_IsNumber(c)) {
            snprintf(hex[count], sizeof(hex[count]), "#%06x", (unsigned int) cJSON_GetNumberValue(c) & 0xffffffu);
        } else if (cJSON_IsString(c)) {
            snprintf(hex[count], sizeof(hex[count]), "%s", cJSON_GetStringValue(c));
        } else {
            continue;
        }
        colors[count] = hex[count];
        count++;
    }
    env_map_note_sky_dome(colors, count);
}

// The dome skyDomeInit builds (deob:231090): data.skyDome, with a mode whose skyCol is not set
static int env_dome_built(const cJSON *raw) {
    return raw && env_truthy(cJSON_GetObjectItem(raw, "skyDome")) && !(g_env_mode && g_env_mode->config.has_sky_col);
}

void map_environment_map_loaded(const Map *map, const GameMode *mode) {
    g_env_map = map;
    g_env_mode = mode;
    g_env_zone = -1;
    if (!map) return;
    env_output(&g_render_lighting, &map->env);
    g_env_map_lighting = g_env_current = g_render_lighting;
    g_env_sun_zone = -1;
    ocean_load(map);
    if (env_dome_built(map->raw_data)) env_note_map_dome(map->raw_data);
    env_map_update(&g_render_lighting.fog_color); // getEnvMap for the GLB models of this map (env_map.h)
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
    env_map_update(NULL); // no scene fog: getEnvMap's "#8899aa"
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
    if (!mode_sky && env->sky_dome) { // skyDomeCols of the zone's dome: addEnvironmentZone always fills the three
        const char *const colors[3] = {env->sky_dome_col[0], env->sky_dome_col[1], env->sky_dome_col[2]};
        env_map_note_sky_dome(colors, 3);
    }
    shadow_set_normal_bias(env->shadow_nb); // a zone's data has no shadowNB: 0
    shadow_environment(env->shadow_r, env->shadow_d, env->light_d);
    g_env_sun_zone = index;
}

// Leaving every zone: v10.0.3 updateEnvironment(-1) re-runs init(map data, mode, keepScene = true)
// (deob-10.0.3.js:234508-234531, 231079-231111). Its sceneInit (deob:234561-234634) keeps the scene's lights, so
//   - the ambient light / sun take the map's only when the map (or the mode: ambCol / lightCol) defines one; a map
//     without `ambient` / `light` keeps the zone's (colour, intensity, direction and shadow camera);
//   - the fog colour and the clear colour take the map's only when one is given (Color.set(undefined) is a no-op),
//     the fog distance always;
//   - tone mapping and exposure are the map's again; the sky dome is rebuilt from the map's keys.
// g_env_map_lighting holds what the map's own load computed for each of them (client_apply_map_lighting).
static void env_restore_map(void) {
    const cJSON *raw = g_env_map->raw_data;
    const GameModeConfig *mode = g_env_mode ? &g_env_mode->config : NULL;
    const RenderLighting *m = &g_env_map_lighting;
    RenderLighting *l = &g_render_lighting;
    const int map_light = (mode && mode->has_light_col) || env_truthy(cJSON_GetObjectItem(raw, "light"));
    if ((mode && mode->has_amb_col) || env_truthy(cJSON_GetObjectItem(raw, "ambient"))) {
        l->ambient_color = m->ambient_color;
        l->ambient_intensity = m->ambient_intensity;
    }
    if (map_light) {
        l->light_color = m->light_color;
        l->light_intensity = m->light_intensity;
        l->light_direction = m->light_direction;
    }
    if ((mode && mode->has_fog_col) || env_color_given(cJSON_GetObjectItem(raw, "fog"))) l->fog_color = m->fog_color;
    l->fog_near = m->fog_near;
    l->fog_far = m->fog_far;
    l->fog_enabled = m->fog_enabled;
    const int mode_sky = mode && mode->has_sky_col;
    if (mode_sky || env_color_given(cJSON_GetObjectItem(raw, "sky"))) l->sky_color = m->sky_color;
    env_output(l, &g_env_map->env);

    glClearColor(l->sky_color.x, l->sky_color.y, l->sky_color.z, 1.0f);
    sky_load(mode_sky ? NULL : raw);
    if (env_dome_built(raw)) env_note_map_dome(raw);
    if (map_light) { // the zone's sun keeps its shadow camera, bias and resolution otherwise
        shadow_set_normal_bias(g_env_map->env.shadow_nb);
        shadow_environment(-1, -1, -1.0f);
        g_env_sun_zone = -1;
    }
}

// Switches the global lighting / sky / shadow state to zone `zone` of the loaded map (-1 = the map's own).
static void env_apply_state(const int zone) {
    if (zone < 0) env_restore_map();
    else env_apply_zone(g_env_map, zone);
    g_env_current = g_render_lighting;
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
    if (g_env_zone >= 0) {
        env_apply_state(g_env_zone);
        return;
    }
    // the map's own environment as the game last showed it (a zone's kept lights included, env_restore_map)
    g_render_lighting = g_env_current;
    glClearColor(g_render_lighting.sky_color.x, g_render_lighting.sky_color.y, g_render_lighting.sky_color.z, 1.0f);
    sky_load(g_env_mode && g_env_mode->config.has_sky_col ? NULL : g_env_map->raw_data);
    if (g_env_sun_zone >= 0 && (size_t) g_env_sun_zone < g_env_map->env_zone_count) {
        const MapEnvironment *zone = &g_env_map->env_zones[g_env_sun_zone].env;
        shadow_set_normal_bias(zone->shadow_nb);
        shadow_environment(zone->shadow_r, zone->shadow_d, zone->light_d);
    } else {
        shadow_set_normal_bias(g_env_map->env.shadow_nb);
        shadow_environment(-1, -1, -1.0f);
    }
}
