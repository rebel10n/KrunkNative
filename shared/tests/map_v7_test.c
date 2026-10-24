// map_v7_test: the Krunker v7.1.4 map schema of WP-A (doc/v7/map-logic.md §1, §7 WP-A).
//
//   - every map under assets/maps, assets/maps/custom and shared/tests/maps loads; no prefab id
//     0-96 falls back to CUBE; the 7 custom maps that use ids >= 60 build those prefabs (hand-
//     checked counts); render-only / zone / anchor objects are non-solid
//   - round trip: map -> printed JSON -> map keeps every key (unknown ones too) and every parsed
//     field, for the v3 maps and the v7 fixtures
//   - shims (§1.10): legacy trigger wrap, SPECTATE_CAM obi, TERMINAL, BOT, tr -> tro, string t /
//     i, short spawns / camPos, pe float, bm / d, at video
//   - validation (map_validate.h): v7's messages for the over-limit fixtures and generated maps
//   - the prefab / texture / trigger tables match the v7 dump sizes; map_object_rotation
//   - the v7.1.4 official maps (read-only reference tree) load, when present
//
// Run from the repository root (like the other shared tests): ./cmake-build-debug/map_v7_test
#include <shared.h>
#include <map_catalog.h>
#include <map_validate.h>
#include <prefabs.h>
#include <trigger_v7_data.h>
#include <world.h>

#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks, g_failures;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        g_checks++;                                                            \
        if (!(cond)) {                                                         \
            g_failures++;                                                      \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                        \
            printf(__VA_ARGS__);                                               \
            printf("\n");                                                      \
        }                                                                      \
    } while (0)

static cJSON *load_json(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *text = malloc((size_t) size + 1);
    if (!text) {
        fclose(f);
        return NULL;
    }
    const size_t n = fread(text, 1, (size_t) size, f);
    fclose(f);
    text[n] = 0;
    cJSON *json = cJSON_Parse(text);
    free(text);
    return json;
}

static int count_prefab(const Map *map, const int prefab) {
    int n = 0;
    for (size_t i = 0; i < map->object_count; i++) n += map->objects[i]->prefab == (unsigned) prefab;
    return n;
}

static int raw_prefab_number(const cJSON *o) {
    const cJSON *i = cJSON_GetObjectItem(o, "i");
    if (!cJSON_IsNumber(i) || !cJSON_GetNumberValue(i)) i = cJSON_GetObjectItem(o, "id");
    return cJSON_IsNumber(i) ? (int) cJSON_GetNumberValue(i) : 0;
}

// The parsed fields a round trip must keep (pointers into raw_data excluded).
static int same_object(const Object *a, const Object *b) {
    return a->prefab == b->prefab && a->texture == b->texture && a->active == b->active && a->collision_type == b->collision_type &&
           a->no_shoot == b->no_shoot && a->wall_jumpable == b->wall_jumpable && a->is_border == b->is_border &&
           a->direction == b->direction && a->team == b->team && a->interface_id == b->interface_id && a->health == b->health &&
           a->can_destroy == b->can_destroy && a->penetration == b->penetration && a->tex_variant == b->tex_variant &&
           a->asset_texture == b->asset_texture && a->tex_rotation == b->tex_rotation && a->frames == b->frames &&
           a->render_side == b->render_side && a->hitbox_h == b->hitbox_h && a->complex_coll == b->complex_coll &&
           a->no_vis == b->no_vis && a->bounce == b->bounce && a->spin == b->spin && a->position.x == b->position.x &&
           a->position.y == b->position.y && a->position.z == b->position.z && a->scale.x == b->scale.x &&
           a->scale.y == b->scale.y && a->scale.z == b->scale.z && a->rotation.x == b->rotation.x &&
           (!a->trig) == (!b->trig) && (!a->trig || (a->trig->action_count == b->trig->action_count &&
                                                     a->trig->event_index == b->trig->event_index &&
                                                     a->trig->action == b->trig->action));
}

// Every key of `a` is in `b` with an equal value (cJSON_Compare), recursively for the objects.
static int keys_kept(const cJSON *a, const cJSON *b) {
    return cJSON_Compare(a, b, 1);
}

static void check_round_trip(const char *label, cJSON *json) {
    Map *map = map_init_ex(json, 0);
    if (!map) return;
    char *printed = cJSON_PrintUnformatted(json);
    cJSON *again = printed ? cJSON_Parse(printed) : NULL;
    free(printed);
    CHECK(again && keys_kept(json, again), "%s: printed map differs from the loaded one", label);
    Map *map2 = again ? map_init_ex(again, 0) : NULL;
    CHECK(map2 && map2->object_count == map->object_count && map2->spawn_count == map->spawn_count &&
          map2->tri_count == map->tri_count && map2->env_zone_count == map->env_zone_count, "%s: round-trip counts", label);
    int differ = 0;
    for (size_t i = 0; map2 && i < map->object_count && i < map2->object_count; i++) differ += !same_object(map->objects[i], map2->objects[i]);
    CHECK(differ == 0, "%s: %d objects parse differently after a round trip", label, differ);
    if (map2) {
        map_fini(map2);
        free(map2);
    }
    cJSON_Delete(again);
    map_fini(map);
    free(map);
}

static int g_maps_loaded;

// Loads one map: object counts / prefabs / collision kinds; returns the map (caller frees) or NULL.
static Map *check_map(const char *label, cJSON *json) {
    Map *map = map_init_ex(json, 0);
    CHECK(map != NULL, "%s: map_init failed", label);
    if (!map) return NULL;
    g_maps_loaded++;

    const cJSON *objects = cJSON_GetObjectItem(json, "objects");
    int kept = 0, lost = 0, ai = 0, bots = 0;
    const cJSON *o;
    cJSON_ArrayForEach(o, objects) {
        const int prefab = map_raw_object_prefab(o);
        const int raw = raw_prefab_number(o);
        if (cJSON_IsNumber(cJSON_GetObjectItem(o, "i")) && raw >= 0 && raw < PREFAB_COUNT && prefab != raw) lost++;
        if (map_raw_object_kept(o)) kept++;
        ai += prefab == PREFAB_AI_SPAWNER;
        bots += prefab == PREFAB_BOT_SPAWNER;
    }
    CHECK(lost == 0, "%s: %d objects fall back to CUBE", label, lost);
    CHECK((int) map->object_count == kept, "%s: %zu of %d objects built", label, map->object_count, kept);
    CHECK((int) map->ai_spawner_count == ai && (int) map->bot_spawner_count == bots, "%s: pending AIs %zu/%d, bots %zu/%d",
          label, map->ai_spawner_count, ai, map->bot_spawner_count, bots);

    int solid_wrong = 0;
    for (size_t i = 0; i < map->object_count; i++) {
        const Object *object = map->objects[i];
        const PrefabInfo *info = prefab_info((int) object->prefab);
        CHECK(info != NULL, "%s: object %zu prefab %u has no info", label, i, object->prefab);
        if (info && info->collision == PREFAB_COL_NONE && object->collision_type != COLLISION_TYPE_NONE) solid_wrong++;
        CHECK(object->raw != NULL, "%s: object %zu without raw record", label, i);
    }
    CHECK(solid_wrong == 0, "%s: %d render-only / pass-through objects collide", label, solid_wrong);
    return map;
}

typedef struct {
    const char *key;
    int counts[PREFAB_COUNT]; // expected map objects per prefab id (-1 = not checked)
} ExpectedCounts;

// Hand-checked counts of the prefabs >= 60 the 7 affected custom maps use (map-logic.md §6.1).
static void check_custom_v7_prefabs(void) {
    static const struct {
        const char *key;
        int prefab, count;
    } expected[] = {
        {"Bhop_VIP", PREFAB_LEADERBOARD, 1},
        {"Krunk_Plaza", PREFAB_PATH_NODE, 5}, {"Krunk_Plaza", PREFAB_ENVIRONMENT_ZONE, 3},
        {"Krunk_Plaza", PREFAB_KPD_ZONE, 3}, {"Krunk_Plaza", PREFAB_SATELLITE_DISH, 1},
        {"Krunk_Plaza", PREFAB_PARTNER_ZONE, 1}, {"Krunk_Plaza", PREFAB_LIGHT_BAR, 9},
        {"Slide_Neon_Stage", PREFAB_ENVIRONMENT_ZONE, 11}, {"Slide_Neon_Stage", PREFAB_LEADERBOARD, 1},
        {"SniperRoyale2", PREFAB_NOPVP_ZONE, 2},
        {"eterno_jump", PREFAB_LEADERBOARD, 1},
        {"slide_moonlight", PREFAB_LEADERBOARD, 1},
        {"zombie_high", PREFAB_LEADERBOARD, 2}, {"zombie_high", PREFAB_LIGHT_BAR, 22},
    };
    printf("Running custom maps with v7 prefabs...\n");
    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
        const int index = map_catalog_index_of_key(expected[i].key);
        CHECK(index >= 0, "%s: not in the catalogue", expected[i].key);
        if (index < 0) continue;
        cJSON *json = cJSON_Duplicate(map_catalog_json((size_t) index), 1);
        Map *map = json ? map_init_ex(json, 0) : NULL;
        CHECK(map != NULL, "%s: does not load", expected[i].key);
        if (!map) {
            cJSON_Delete(json);
            continue;
        }
        const int n = count_prefab(map, expected[i].prefab);
        CHECK(n == expected[i].count, "%s: %d %s objects (expected %d)", expected[i].key, n, prefab_name(expected[i].prefab), expected[i].count);
        for (size_t k = 0; k < map->object_count; k++) {
            const Object *o = map->objects[k];
            if (o->prefab == PREFAB_ENVIRONMENT_ZONE || o->prefab == PREFAB_LIGHT_BAR || o->prefab == PREFAB_PATH_NODE ||
                o->prefab == PREFAB_LEADERBOARD || o->prefab == PREFAB_NOPVP_ZONE) {
                CHECK(o->collision_type == COLLISION_TYPE_NONE, "%s: %s object %zu is solid", expected[i].key, prefab_name((int) o->prefab), k);
            }
        }
        if (!strcmp(expected[i].key, "Krunk_Plaza") && expected[i].prefab == PREFAB_ENVIRONMENT_ZONE) {
            CHECK(map->env_zone_count == 3 && map->path_node_count == 5, "Krunk_Plaza: %zu env zones, %zu path nodes",
                  map->env_zone_count, map->path_node_count);
            CHECK(map->game_config != NULL && map->scripts != NULL, "Krunk_Plaza: config / scripts refs");
            CHECK(map->camera_static || map->camera_rotation.x || map->camera_rotation.y || map->camera_rotation.z ||
                      cJSON_GetArraySize(cJSON_GetObjectItem(json, "camPos")) == 7, "Krunk_Plaza: 7-element camPos");
        }
        if (!strcmp(expected[i].key, "SniperRoyale2")) {
            for (size_t k = 0; k < map->object_count; k++) {
                if (map->objects[k]->prefab == PREFAB_NOPVP_ZONE) CHECK(map->objects[k]->no_pvp, "SniperRoyale2: NOPVP flag");
            }
        }
        map_fini(map);
        free(map);
        cJSON_Delete(json);
    }
}

static void for_each_json(const char *dir, void (*fn)(const char *path, const char *name)) {
    DIR *d = opendir(dir);
    if (!d) return;
    char **names = NULL;
    size_t count = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        const size_t len = strlen(e->d_name);
        if (len < 6 || strcmp(e->d_name + len - 5, ".json")) continue;
        names = realloc(names, (count + 1) * sizeof(char *));
        names[count++] = strdup(e->d_name);
    }
    closedir(d);
    for (size_t i = 0; i < count; i++) { // sorted for stable output
        for (size_t j = i + 1; j < count; j++) {
            if (strcmp(names[j], names[i]) < 0) {
                char *t = names[i];
                names[i] = names[j];
                names[j] = t;
            }
        }
    }
    for (size_t i = 0; i < count; i++) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
        fn(path, names[i]);
        free(names[i]);
    }
    free(names);
}

static void load_one(const char *path, const char *name) {
    cJSON *json = load_json(path);
    CHECK(json != NULL, "%s: not JSON", path);
    if (!json) return;
    if (!strncmp(name, "v7_limits_", 10)) { // validation fixtures: they only have to fail validation
        size_t errors = 0;
        map_validate(json, NULL, NULL, 0, &errors);
        CHECK(errors > 0, "%s: validation accepts it", name);
        cJSON_Delete(json);
        return;
    }
    Map *map = check_map(name, json);
    if (map) {
        map_fini(map);
        free(map);
    }
    check_round_trip(name, json);
    if (getenv("MAP_V7_TEST_VERBOSE")) { // what v7's custom-map validation says about the shipped maps
        MapIssue issues[4];
        size_t errors = 0;
        const MapValidateOptions options = {.featured = 1, .no_warnings = 1};
        const size_t n = map_validate(json, &options, issues, 4, &errors);
        printf("  %-28s %s%s\n", name, errors ? "rejected as a custom map: " : "valid", errors ? issues[0].message : "");
        (void) n;
    }
    cJSON_Delete(json);
}

static void check_all_prefabs_fixture(void) {
    printf("Running v7_all_prefabs fixture...\n");
    cJSON *json = load_json("shared/tests/maps/v7_all_prefabs.json");
    CHECK(json != NULL, "v7_all_prefabs.json missing");
    if (!json) return;
    Map *map = map_init_ex(json, 0);
    CHECK(map && map->object_count == PREFAB_V7_COUNT - 4, "93 map objects (no EVENT, TERMINAL, AI_SPAWNER, BOT_SPAWNER): %zu",
          map ? map->object_count : 0);
    if (!map || map->object_count != PREFAB_V7_COUNT - 4) {
        cJSON_Delete(json);
        return;
    }
    // objects in file order, skipping 40, 41, 55, 95
    const Object *by_id[PREFAB_COUNT] = {0};
    for (size_t i = 0; i < map->object_count; i++) by_id[map->objects[i]->prefab] = map->objects[i];
    for (int id = 0; id < PREFAB_V7_COUNT; id++) {
        const int skipped = id == PREFAB_EVENT || id == PREFAB_TERMINAL || id == PREFAB_AI_SPAWNER || id == PREFAB_BOT_SPAWNER;
        CHECK(skipped ? !by_id[id] : by_id[id] != NULL, "prefab %d (%s) %s", id, prefab_name(id), skipped ? "built" : "missing");
    }
    CHECK(map->ai_spawner_count == 1 && map->bot_spawner_count == 1, "pending AI / bot records");

    const Object *cube = by_id[PREFAB_CUBE];
    CHECK(cube->tex_variant == 1 && fabsf(cube->tex_rotation - 90.0f) < 1e-4f && cube->tex_scale_mult == 2.0f && cube->tex_encoding == 1 &&
              cube->frames == 4 && fabsf(cube->frame_time - 150.0f) < 1e-3f && cube->render_side == 2 && cube->darken == 0.5f,
          "CUBE texture keys");
    CHECK(cube->no_shoot && cube->no_ai_col && cube->no_grapple && cube->no_impulse && cube->complex_coll && cube->no_minimap &&
              cube->step_sound == 1 && cube->is_border && cube->interface_id == 12 && cube->penetration == 0.5f && cube->penetrable,
          "CUBE collision keys");
    CHECK(by_id[PREFAB_PLANE]->alpha_test == 0.4f && by_id[PREFAB_PLANE]->sprite, "PLANE alt / msp");
    CHECK(by_id[PREFAB_SCORE_ZONE]->score_points == 5, "SCORE_ZONE sp");
    CHECK(by_id[PREFAB_DEATH_ZONE]->health == 100.0f && by_id[PREFAB_DEATH_ZONE]->can_destroy && by_id[PREFAB_DEATH_ZONE]->interface_id == 4,
          "DEATH_ZONE destructible / interface (v7)");
    CHECK(by_id[PREFAB_OBJECTIVE]->objective_uid == 2 && map->objective_count == 1, "OBJECTIVE obi 3 -> uid 2");
    CHECK(by_id[PREFAB_GATE]->interact_msg && !strcmp(by_id[PREFAB_GATE]->interact_msg, "Open"), "GATE inm");

    const Object *trig_obj = by_id[PREFAB_TRIGGER];
    const TriggerData *t = trig_obj->trig;
    CHECK(t && t->event_index == 16 && t->event == TRIGGER_EVENT_ON_INTERACT_KEY && t->action_cost == 100 && t->action_method == 1 &&
              t->single_use_player && t->action_count == 2, "TRIGGER v7 object keys");
    CHECK(trig_obj->no_player_col && fabsf(trig_obj->hitbox_h - 2.0f) < 1e-6f && fabsf(trig_obj->hitbox_w - 0.5f) < 1e-6f &&
              trig_obj->interact_msg && !strcmp(trig_obj->interact_msg, "Buy") && !trig_obj->legacy_trigger, "TRIGGER pl / hbh / hbw / inm");
    if (t && t->action_count == 2) {
        const TriggerActionData *a = &t->actions[1];
        CHECK(t->actions[0].action == 0 && t->actions[0].constant == 10 && t->action == 0 && t->constant == 10, "tas[0] mirrored");
        CHECK(a->action == 37 && !strcmp(a->text, "Kills") && !strcmp(a->value_key, "kills") && a->value_type == 2 &&
                  fabsf(a->delay - 1.3f) < 1e-4f && a->condition == 1 && a->condition_values[0] && a->condition_values[2],
              "tas[1] fields (ta 37 text -> [a-z0-9_]{0,10}, tad rounded to 0.1)");
        CHECK(a->sound == 16 && a->volume == 0.5f && fabsf(a->rate - 0.1f) < 1e-6f && a->emit_from == 2, "tas[1] sound (v7 rate min 0.1)");
    }

    CHECK(by_id[PREFAB_CAMERA]->obj_link == 2 && by_id[PREFAB_CAMERA]->collision_type == COLLISION_TYPE_NONE, "CAMERA obl");
    const Object *pad = by_id[PREFAB_BOOST_PAD];
    CHECK(pad->jump_pad && pad->bounce == 0.0f && pad->direction == 4 && pad->crouch, "BOOST_PAD bm 0 kept, d 4");
    CHECK(fabsf(by_id[PREFAB_CYLINDER]->spin - 5.0f) < 1e-6f && by_id[PREFAB_CYLINDER]->collision_type == COLLISION_TYPE_CYLINDER &&
              by_id[PREFAB_CYLINDER]->penetration == 1.0f, "CYLINDER spn / pe");
    CHECK(by_id[PREFAB_PATH_NODE]->node_id == 7 && world_path_node(map, 7) == by_id[PREFAB_PATH_NODE] && !world_path_node(map, 8),
          "PATH_NODE nid -> world_path_node");
    CHECK(by_id[PREFAB_KR_ZONE]->kr_zone && by_id[PREFAB_KR_ZONE]->kr_amount == 5000 && by_id[PREFAB_KR_ZONE]->is_border &&
              by_id[PREFAB_KR_ZONE]->collision_type == COLLISION_TYPE_BOX, "KR_ZONE solid with krs");
    CHECK(by_id[PREFAB_NOPVP_ZONE]->no_pvp && by_id[PREFAB_PVP_ZONE]->force_pvp && by_id[PREFAB_NOPVP_ZONE]->collision_type == COLLISION_TYPE_NONE,
          "NOPVP / PVP pass-through");
    CHECK(by_id[PREFAB_SERVER_NODE]->server_node && by_id[PREFAB_SERVER_NODE]->node_tag &&
              !strcmp(by_id[PREFAB_SERVER_NODE]->node_tag, "zombie_spawn") && by_id[PREFAB_SERVER_NODE]->spread == 20.0f &&
              by_id[PREFAB_SERVER_NODE]->health == 50.0f && by_id[PREFAB_SERVER_NODE]->no_ai_col, "SERVER_NODE keys (kept health)");
    CHECK(by_id[PREFAB_ROPE_LADDER]->rope_ladder && by_id[PREFAB_ROPE_LADDER]->collision_type == COLLISION_TYPE_CYLINDER,
          "ROPE_LADDER cylinder");
    CHECK(by_id[PREFAB_MYSTERY_BOX]->start_inactive && !by_id[PREFAB_MYSTERY_BOX]->active && by_id[PREFAB_MYSTERY_BOX]->score_points == 950,
          "MYSTERY_BOX starts inactive");
    CHECK(by_id[PREFAB_PERK_MACHINE]->zombie_perk == 3 && by_id[PREFAB_PERK_MACHINE]->is_border, "PERK_MACHINE zp / bo");
    CHECK(by_id[PREFAB_SPRAY]->spray_id == 2483 && by_id[PREFAB_MAP_ANCHOR]->anchor_type == 1, "SPRAY spt / MAP_ANCHOR ant");
    CHECK(by_id[PREFAB_AI_REDIRECTION]->redirection && by_id[PREFAB_AI_REDIRECTION]->direction == 1, "AI_REDIRECTION d");
    CHECK(by_id[PREFAB_GIFT]->collision_type == COLLISION_TYPE_BOX && by_id[PREFAB_LAMP]->tex_variant == 2, "model props solid, tv");
    for (int id = PREFAB_LIGHT_BAR; id <= PREFAB_SPOT_LIGHT; id++) {
        if (by_id[id]) CHECK(by_id[id]->collision_type == COLLISION_TYPE_NONE, "%s non-solid", prefab_name(id));
    }
    CHECK(map->server_node_count == 1 && map->map_anchor_count == 1 && map->dom_zone_count == 1 && map->env_zone_count == 1,
          "manager lists");
    if (map->env_zone_count == 1) {
        const MapEnvironmentZone *z = &map->env_zones[0];
        CHECK(!strcmp(z->env.ambient, "#112233") && !strcmp(z->env.fog, "#445566") && z->env.fog_d == 800.0f && z->env.tone_mapping == 4 &&
                  z->env.sky_dome && z->env.amb_ind == 6 && z->env.light_i == 1.3f && !strcmp(z->env.light, "#f2f8fc"),
              "ENVIRONMENT_ZONE data (ambientC / fogC, defaults)");
        // v10.0.3 addEnvironmentZone (deob-10.0.3.js:185271-185302): sunAngX / lightD parseInt'd, ambientI parseFloat'd
        CHECK(z->env.sun_ang_x == 45.0f && z->env.light_d == 300.0f && z->env.ambient_i == 0.5f && z->env.sun_ang_y == 54.0f &&
                  z->env.shadow_r == 1024 && z->env.shadow_d == 1200,
              "ENVIRONMENT_ZONE parseInt / parseFloat keys");
        const vec3 inside = {z->position.x, z->position.y + 1.0f, z->position.z};
        const vec3 outside = {z->position.x, z->position.y - 1.0f, z->position.z};
        CHECK(world_environment_zone_at(map, inside) == 0 && world_environment_zone_at(map, outside) == -1, "environment zone lookup");
    }

    // top level
    CHECK(map->spawn_count == 2 && map->spawns[0]->team_only && map->spawns[0]->spread == 20.0f && !map->spawns[1]->team_only &&
              map->spawns[1]->spread == 50.0f && map->spawns[1]->comp, "spawns [6] teamOnly, [7] spread clamp");
    CHECK(map->camera_static && fabsf(map->camera_rotation.x - 0.1f) < 1e-6f && fabsf(map->camera_rotation.z - 0.3f) < 1e-6f,
          "camPos static / rotation");
    CHECK(map->tri_count == 2 && map->tris[3] == 10.0f, "tris");
    CHECK(map->zone.enabled && map->zone.size == 800.0f && map->zone.stages == 2 && map->zone.height == 1000.0f, "zone config");
    CHECK(map->ocean.enabled && !strcmp(map->ocean.col, "#003344") && map->ocean.opacity == 0.9f, "ocean config");
    CHECK(!map->terrain.enabled && map->terrain.width == 3000.0f && map->terrain.ease == 12, "terrain off without a seed");
    CHECK(map->env.has_ambient && map->env.tone_mapping == 2 && map->env.correct_lights && map->env.fog_d == 1500.0f, "map environment");
    CHECK(map->mode_count == 3 && map->modes[1] == 13 && map->game_config && map->gui && map->bot_paths, "modes / config / gui / botPaths");
    CHECK(map->name && !strcmp(map->name, "v7_all_prefabs"), "name");

    // the trigger table and the action data agree
    CHECK(!strcmp(g_trigger_v7_events[16], "onInteractKey") && !strcmp(g_trigger_v7_actions[37].name, "Set Player Value") &&
              g_trigger_v7_conditions[1].val_count == 3, "trigger tables");

    size_t errors = 0;
    MapIssue issues[16];
    map_validate(json, NULL, issues, 16, &errors);
    CHECK(errors == 0, "v7_all_prefabs validates (%zu errors, first: %s)", errors, errors ? issues[0].message : "");
    map_fini(map);
    free(map);
    cJSON_Delete(json);
}

static void check_legacy_fixture(void) {
    printf("Running v7_legacy_v3 fixture...\n");
    cJSON *json = load_json("shared/tests/maps/v7_legacy_v3.json");
    CHECK(json != NULL, "v7_legacy_v3.json missing");
    if (!json) return;
    Map *map = map_init_ex(json, 0);
    // 10 records: TERMINAL builds nothing, BOT is a pending AI
    CHECK(map && map->object_count == 8 && map->ai_spawner_count == 1, "objects %zu (TERMINAL / BOT skipped)", map ? map->object_count : 0);
    if (!map || map->object_count != 8) {
        cJSON_Delete(json);
        return;
    }
    const Object *floor = map->objects[0];
    CHECK(floor->prefab == PREFAB_CUBE && fabsf(floor->tex_rotation - 89.0f) < 1e-4f, "tr 1.5708 rad -> tro 89 deg (parseInt of toDeg)");
    CHECK(prefab_texture_resolve(PREFAB_CUBE, 1, -1) == 0 && prefab_texture_resolve(PREFAB_LADDER, 0, 0) == 2, "texture fallback");
    const Object *cam = map->objects[1];
    CHECK(cam->prefab == PREFAB_CAMERA && cam->obj_link == 0, "SPECTATE_CAM obi is not read (obl): links to 0");
    const Object *trig = map->objects[2];
    CHECK(trig->prefab == PREFAB_TRIGGER && trig->legacy_trigger && trig->trig && trig->trig->action_count == 1, "legacy trigger wrapped");
    if (trig->trig && trig->trig->action_count == 1) {
        const TriggerActionData *a = &trig->trig->actions[0];
        CHECK(a->action == 0 && a->constant == 7 && !strcmp(a->text, "Hi") && a->value_type == 0 && a->sound == 16 &&
                  fabsf(a->rate - 0.2f) < 1e-6f, "legacy tas[0]: tce stays the Event Value (value type 0: the v7 quirk is fixed)");
        CHECK(trig->trig->constant_event == 3 && trig->trig->timer == 3000.0f, "legacy tce is the Event Value / timer");
    }
    CHECK(map->objects[3]->prefab == PREFAB_BILLBOARD && map->objects[3]->rotation.x == 0.3f, "BILLBOARD r kept (sad ignored)");
    const Object *pad = map->objects[4];
    CHECK(pad->jump_pad && pad->bounce == 2.0f && pad->direction == 0 && pad->rotation.x == 0.5f, "BOOST_PAD v3 bm, no d");
    CHECK(map->objects[5]->prefab == PREFAB_LADDER && map->objects[5]->direction == 3, "\"id\" alias, ladder d -1 -> 3");
    const Object *hp = map->objects[6];
    CHECK(hp->health == 40.0f && hp->penetration == 1.0f && hp->asset_texture == 12 && hp->asset_video, "hp alias, pe 1, at video");
    CHECK(map->objects[7]->prefab == PREFAB_CUBE, "string prefab name loads as CUBE");
    CHECK(map->spawn_count == 2 && !map->spawns[1]->team_only && map->spawns[1]->spread == 0.0f && map->spawns[1]->comp, "6-element spawn");
    CHECK(!map->camera_static && map->camera_rotation.x == 0.0f && map->camera_position.y == 20.0f, "3-element camPos");

    MapIssue issues[32];
    size_t errors = 0;
    const size_t total = map_validate(json, NULL, issues, 32, &errors);
    int saw_texture = 0, saw_game = 0, saw_terminal = 0, saw_camera = 0;
    for (size_t i = 0; i < total && i < 32; i++) {
        saw_texture |= !strcmp(issues[i].code, "object_texture");
        saw_game |= !strcmp(issues[i].code, "legacy_game");
        saw_terminal |= !strcmp(issues[i].code, "terminal");
        saw_camera |= !strcmp(issues[i].code, "camera_obi");
    }
    CHECK(errors >= 1 && saw_texture, "a string \"t\" fails prepareGame (Object Texture error.)");
    CHECK(saw_game && saw_terminal && saw_camera, "legacy warnings");
    map_fini(map);
    free(map);
    cJSON_Delete(json);
}

static void expect_error(const char *path, const char *message) {
    cJSON *json = load_json(path);
    CHECK(json != NULL, "%s missing", path);
    if (!json) return;
    const char *error = map_validate_error(json, 0);
    CHECK(error && !strcmp(error, message), "%s: \"%s\" (expected \"%s\")", path, error ? error : "(accepted)", message);
    cJSON_Delete(json);
}

static cJSON *generated(const int count, const char *object_json) {
    cJSON *map = cJSON_Parse("{\"name\":\"gen\",\"spawns\":[[0,0,0]],\"objects\":[]}");
    cJSON *objects = cJSON_GetObjectItem(map, "objects");
    for (int i = 0; i < count; i++) cJSON_AddItemToArray(objects, cJSON_Parse(object_json));
    return map;
}

static void expect_generated(const int count, const char *object_json, const char *message, const int featured) {
    cJSON *map = generated(count, object_json);
    const char *error = map_validate_error(map, featured);
    CHECK(message ? error && !strcmp(error, message) : !error, "%d x %s: \"%s\" (expected \"%s\")", count, object_json,
          error ? error : "(accepted)", message ? message : "(accepted)");
    cJSON_Delete(map);
}

static void check_validation(void) {
    printf("Running validation...\n");
    expect_error("shared/tests/maps/v7_limits_name.json", "Map name too long.");
    expect_error("shared/tests/maps/v7_limits_prefab.json", "Invalid Object.");
    expect_error("shared/tests/maps/v7_limits_texture.json", "Object Texture error.");
    expect_error("shared/tests/maps/v7_limits_actions.json", "Trigger Has Too Many Actions"); // 9 > v10's 8
    expect_error("shared/tests/maps/v7_limits_condition.json", "CONDITION VALUE OPTION DOESNT EXIST (Type)");
    expect_error("shared/tests/maps/v7_limits_spawn.json", "Spawn point error.");
    expect_error("shared/tests/maps/v7_limits_leaderboards.json", "Too many Leaderboards.");
    expect_error("shared/tests/maps/v7_limits_rotation.json", "Object rotation error.");

    expect_generated(MAP_V7_SIGN_LIMIT, "{\"i\":30,\"p\":[0,0,0],\"s\":[1,1,1],\"st\":\"a\"}", NULL, 0);
    expect_generated(MAP_V7_SIGN_LIMIT + 1, "{\"i\":30,\"p\":[0,0,0],\"s\":[1,1,1],\"st\":\"a\"}", "Too many signs.", 0);
    expect_generated(MAP_V7_WHEEL_LIMIT + 1, "{\"i\":68,\"p\":[0,0,0],\"s\":[1,1,1]}", "Too many Prize Wheels.", 0);
    expect_generated(MAP_V7_AI_LIMIT, "{\"i\":55,\"p\":[0,0,0],\"s\":[1,1,1]}", NULL, 0);
    expect_generated(MAP_V7_AI_LIMIT + 1, "{\"i\":55,\"p\":[0,0,0],\"s\":[1,1,1]}", "Too many AI.", 0);
    expect_generated(MAP_V7_INTERFACE_LIMIT + 1, "{\"i\":0,\"p\":[0,0,0],\"s\":[1,1,1],\"in\":5}",
                     "Interface limit Exceeded: Max 30 (Per id)", 0);
    expect_generated(MAP_V7_COLLIDABLE_LIMIT + 1, "{\"i\":0,\"p\":[0,0,0],\"s\":[1,1,1]}", "Map exceeds collidable object limit.", 0);
    expect_generated(MAP_V7_COLLIDABLE_LIMIT + 1, "{\"i\":0,\"p\":[0,0,0],\"s\":[1,1,1]}", NULL, 1);
    expect_generated(MAP_V7_COLLIDABLE_LIMIT + 1, "{\"i\":0,\"p\":[0,0,0],\"s\":[1,1,1],\"l\":1}", NULL, 0);
    expect_generated(MAP_V7_OBJECT_LIMIT + 1, "{\"i\":0,\"p\":[0,0,0],\"s\":[1,1,1],\"col\":1}", "Map exceeds overall object limit.", 0);
    expect_generated(1, "{\"i\":0,\"p\":[0,0],\"s\":[1,1,1]}", "Object position error.", 0);
    expect_generated(1, "{\"i\":0,\"p\":[0,0,0],\"s\":[1,1,1],\"d\":\"x\"}", "Object direction error.", 0);
    expect_generated(1, "{\"i\":29,\"p\":[0,0,0],\"s\":[1,1,1],\"tct\":\"<b>\"}", "Illegal characters in Trigger Message", 0);
    expect_generated(1, "{\"i\":96,\"p\":[0,0,0],\"s\":[1,1,1]}", NULL, 0);
    expect_generated(1, "{\"i\":97,\"p\":[0,0,0],\"s\":[1,1,1]}", NULL, 0); // v10's SCRAPCRATE
    expect_generated(1, "{\"i\":99,\"p\":[0,0,0],\"s\":[1,1,1]}", "Invalid Object.", 0);
    expect_generated(1, "{\"i\":0,\"id\":120,\"p\":[0,0,0],\"s\":[1,1,1]}", "Object ID error.", 0);

    char *text = map_prepare_custom_text("{\"name\":\"a(b)\"}");
    CHECK(text && !strcmp(text, "{\"name\":\"a\xEF\xBC\x88" "b\xEF\xBC\x89\"}"), "prepareGame paren replacement");
    free(text);
    const char *error = NULL;
    cJSON *ok = map_prepare_custom("{\"name\":\"(x)\",\"spawns\":[[0,0,0]],\"objects\":[{\"p\":[0,0,0],\"s\":[1,1,1]}]}", 0, &error);
    CHECK(ok && !error, "map_prepare_custom accepts a small map (%s)", error ? error : "");
    cJSON_Delete(ok);
    CHECK(!map_prepare_custom("{nope", 0, &error) && error && !strcmp(error, "Map error occured."), "unparsable map");
    cJSON *parsed = cJSON_Parse("{\"name\":\"a(b)\",\"objects\":[{\"st\":\"x)\"}]}");
    CHECK(map_prepare_custom_json(parsed) == 2 &&
              !strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(parsed, "name")), "a\xEF\xBC\x88" "b\xEF\xBC\x89"),
          "paren replacement on a parsed map");
    cJSON_Delete(parsed);
}

static void check_tables(void) {
    printf("Running tables...\n");
    CHECK(PREFAB_V7_COUNT == 97 && PREFAB_COUNT == 99 && PREFAB_TEXTURE_COUNT == 20, "97 v7 prefabs (99 with v10's), 20 textures");
    CHECK(!strcmp(prefab_name(PREFAB_CAMERA), "CAMERA") && !strcmp(prefab_name(PREFAB_AI_SPAWNER), "AI_SPAWNER") &&
              !strcmp(prefab_name(PREFAB_MAP_ANCHOR), "MAP_ANCHOR") && !strcmp(prefab_name(PREFAB_KNIGHT), "KNIGHT"),
          "enum order matches v7 prefabs.js");
    for (int i = 0; i < PREFAB_COUNT; i++) CHECK(prefab_id_from_name(prefab_name(i)) == i, "name lookup %d", i);
    CHECK(prefab_id_from_name("SPECTATE_CAM") == PREFAB_CAMERA && prefab_id_from_name("BOT") == PREFAB_AI_SPAWNER, "v3 aliases");
    CHECK(prefab_has_flag(PREFAB_CUBE, PF_EDITFACES) && prefab_has_flag(PREFAB_AI_SPAWNER, PF_AI) && !prefab_has_flag(PREFAB_CUBE, PF_AI) &&
              prefab_has_flag(PREFAB_SPHERE, PF_NOCOL), "generated flags");
    char src[64];
    CHECK(!strcmp(prefab_texture_src(0, 3, src, sizeof(src)), "wall_0_3") && !strcmp(prefab_texture_src(8, 0, src, sizeof(src)), "grass_1") &&
              !strcmp(prefab_texture_src(19, 2, src, sizeof(src)), "snow_0") && prefab_texture_id_from_name("Stone") == 0,
          "texture src / variants / aliases");
    CHECK(g_prefab_data[PREFAB_CRATE].variant_count == 5 && g_prefab_data[PREFAB_CRATE].variants[2].diff_model, "model variants");

    // map_object_rotation: a PLANE with r = 0 faces up (its local +Z normal -> world +Y)
    Object plane = {.prefab = PREFAB_PLANE};
    float m[9];
    map_object_rotation(&plane, m);
    CHECK(fabsf(m[2] - 0.0f) < 1e-6f && fabsf(m[5] - 1.0f) < 1e-6f && fabsf(m[8]) < 1e-6f, "plane normal up");
    // "yR" = r0, but addCube does rotation.set(yR, xR, zR): r0 is the Euler x angle
    Object cube = {.prefab = PREFAB_CUBE, .rotation = {(float) M_PI / 2.0f, 0.0f, 0.0f}};
    map_object_rotation(&cube, m);
    CHECK(fabsf(m[0] - 1.0f) < 1e-6f && fabsf(m[7] - 1.0f) < 1e-6f, "CUBE r0 rotates about X");
    // RECTAREA_LIGHT: rotateY(r2) rotateX(r0) rotateZ(r1) (Euler YXZ)
    Object rect = {.prefab = PREFAB_RECTAREA_LIGHT, .rotation = {0.0f, 0.0f, (float) M_PI / 2.0f}};
    map_object_rotation(&rect, m);
    CHECK(fabsf(m[4] - 1.0f) < 1e-6f && fabsf(m[2] - 1.0f) < 1e-6f, "RECTAREA_LIGHT r2 rotates about Y");
    // a billboard r = (PI/2, 0, PI/2): v7 similateRot faces it -X; a 311_ map's (v3.1.1 addPlane
    // Ry(r2) Rx(r0 - PI/2) Rz(r1)) faces +X, upright, and a flat v3 plane still faces up
    Object bb = {.prefab = PREFAB_BILLBOARD, .rotation = {(float) M_PI / 2.0f, 0.0f, (float) M_PI / 2.0f}};
    map_object_rotation(&bb, m);
    CHECK(fabsf(m[2] + 1.0f) < 1e-5f && fabsf(m[4] - 1.0f) < 1e-5f, "v7 billboard r2 = PI/2 faces -X");
    bb.v3_plane_rotation = 1;
    map_object_rotation(&bb, m);
    CHECK(fabsf(m[2] - 1.0f) < 1e-5f && fabsf(m[4] - 1.0f) < 1e-5f && fabsf(m[8]) < 1e-5f, "v3 billboard r2 = PI/2 faces +X");
    Object v3plane = {.prefab = PREFAB_PLANE, .v3_plane_rotation = 1};
    map_object_rotation(&v3plane, m);
    CHECK(fabsf(m[5] - 1.0f) < 1e-6f && fabsf(m[2]) < 1e-6f && fabsf(m[8]) < 1e-6f, "v3 plane normal up");
    Object pad = {.prefab = PREFAB_BOOST_PAD, .rotation = {(float) M_PI / 2.0f, 0.0f, 0.0f}}; // v7 xR = r0: about X
    map_object_rotation(&pad, m);
    CHECK(fabsf(m[0] - 1.0f) < 1e-6f && fabsf(m[7] - 1.0f) < 1e-6f, "BOOST_PAD r0 rotates about X (v7 order)");
}

static void load_v7_official(const char *path, const char *name) {
    cJSON *json = load_json(path);
    if (!json) return;
    Map *map = check_map(name, json);
    if (map) {
        map_fini(map);
        free(map);
    }
    check_round_trip(name, json);
    cJSON_Delete(json);
}

int main(void) {
    check_tables();
    check_all_prefabs_fixture();
    check_legacy_fixture();
    check_validation();

    printf("Running every shipped map...\n");
    // the v7.1.4 official maps (tools/maps/import_v7_maps.py) and the v3.1.1 311_ / 152_ maps
    for_each_json("assets/maps", load_one);
    for_each_json("assets/maps/custom", load_one);
    for_each_json("assets/maps/official", load_one); // the v10.0.2 official files (krunker_maps.h)
    for_each_json("shared/tests/maps", load_one);
    check_custom_v7_prefabs();
    const int shipped = g_maps_loaded;

    printf("Running the v7.1.4 official maps (reference tree, when present)...\n");
    for_each_json("/home/user/jails/kru/src/Krunker_v7.1.4/public/maps", load_v7_official);

    printf("map_v7_test %s: %d checks, %d failures (%d shipped / fixture maps, %d v7 reference maps)\n",
           g_failures ? "FAILED" : "OK", g_checks, g_failures, shipped, g_maps_loaded - shipped);
    return g_failures ? 1 : 0;
}
