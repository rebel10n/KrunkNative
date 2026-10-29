// The Scene glue of the warm (client/include/scene_warm.h) and the map's AI models into the loaders' caches before
// the warm frames.
// v10.0.4's warmScene first waits for every model still loading (its model cache's state 0 / pending,
// deob-10.0.4.js:205001-205009), then compiles. KN's AI meshes are built when the AI first syncs (ai_render.c
// air_build): an OBJ or glTF parse + GPU upload, a few ms each, all in one frame when a map's AIs spawn together
// (13 on Burg, ~36 ms, ~220 ms after the load). Loading them here puts that work in the load frame, behind the
// loading card; air_build then hits the caches (load_obj_model / load_texture / gltf_model_load are cached by path).
// The paths are air_load_asset's and air_load_stock's (ai_render.c).
#include <scene_warm.h>
#include <ai.h>
#include <gltf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void scene_warm_begin(const Scene *scene, const double now_ms) {
    scene_warm_begin_count(scene ? scene->mesh_count : 0, now_ms);
}

int scene_warm_frame(const Scene *scene, const double now_ms) {
    return scene_warm_frame_count(scene ? scene->mesh_count : 0, now_ms);
}

static void preload_obj(const char *rel) {
    char *full = concat(assets_path(), rel);
    if (full) load_obj_model(full, 0);
    free(full);
}

static void preload_texture(const char *rel) {
    char *full = concat(assets_path(), rel);
    if (full) load_texture(full);
    free(full);
}

static void preload_gltf(const char *rel) {
    char *full = concat(assets_path(), rel);
    if (full) gltf_model_load(full);
    free(full);
}

// One AI_SPAWNER record's model (buildAIData's aid: a number or a string with its flag letters).
static void preload_ai(const cJSON *record) {
    const cJSON *aid = cJSON_GetObjectItem(record, "aid");
    char text[GAME_AI_ASSET_SIZE] = "";
    if (cJSON_IsNumber(aid) && aid->valuedouble > 0) snprintf(text, sizeof(text), "%d", aid->valueint);
    else if (cJSON_IsString(aid) && aid->valuestring) snprintf(text, sizeof(text), "%s", aid->valuestring);
    char rel[96];
    if (!text[0]) { // no aid: the ghost (addAI's src ghost_0)
        preload_obj("models/ghost_0.obj");
        preload_texture("textures/ghost_0.png");
        return;
    }
    GameAIData d;
    game_ai_data_defaults(&d);
    game_ai_data_set_asset(&d, text);
    if (!d.asset) return;
    if (strchr(d.asset_text, 'g') || strchr(d.asset_text, 'q')) {
        snprintf(rel, sizeof(rel), "user-assets/%d/model.%s", d.asset, strchr(d.asset_text, 'q') ? "glb" : "gltf");
        preload_gltf(rel);
        return;
    }
    if (strchr(d.asset_text, 'f')) return; // FBX: ai_render.c builds none
    snprintf(rel, sizeof(rel), "user-assets/%d/model.obj", d.asset);
    preload_obj(rel);
    snprintf(rel, sizeof(rel), "user-assets/%d/model.png", d.asset);
    preload_texture(rel);
    if (strchr(d.asset_text, 'e')) {
        snprintf(rel, sizeof(rel), "user-assets/%d/model_e.png", d.asset);
        preload_texture(rel);
    }
}

void scene_warm_preload(const Map *map) {
    if (!map || !scene_warm_enabled()) return;
    for (size_t i = 0; i < map->ai_spawner_count; i++) preload_ai(map->ai_spawners[i]);
}
