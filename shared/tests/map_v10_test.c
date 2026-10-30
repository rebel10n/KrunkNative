// map_v10_test: the Krunker v10.0.2 map format (doc/v10/map-format.md).
//
//   - parsing (shared/tests/maps/v10/v10_format.json, written for this test): prefabs 41
//     BOT_PATH_NODE / 97 SCRAPCRATE / 98 ENTITY, "dyn", "mp", "shadowNB", the widened texture-id
//     charset, "archetypes", ENTITY instances, "scripts.shared", "botPaths"
//   - validation: the archetype / entity rules, prefab ids 97-98, the 8-action trigger limit
//   - triggers: condition 12 "Custom Action" (shouldTrigger), actions 72-74, the table counts
//   - bots: usePathing walks botPaths
//   - the light-unit factor
//
// Run from the repository root: ./<build>/map_v10_test
#include <shared.h>
#include <ruleset.h>
#include <map_validate.h>
#include <prefabs.h>
#include <triggers_v7.h>
#include <trigger_v7_data.h>
#include <bots.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks, g_failures;
#define CHECK(cond, ...) do { g_checks++; if (!(cond)) { g_failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define FIXTURE "shared/tests/maps/v10/v10_format.json"

static cJSON *load(const char *path) {
    cJSON *json = NULL;
    if (load_json_file(path, &json)) return NULL;
    return json;
}

static int count_prefab(const Map *map, const int prefab) {
    int n = 0;
    for (size_t i = 0; i < map->object_count; i++) n += (int) map->objects[i]->prefab == prefab;
    return n;
}

static const Object *first_prefab(const Map *map, const int prefab) {
    for (size_t i = 0; i < map->object_count; i++) if ((int) map->objects[i]->prefab == prefab) return map->objects[i];
    return NULL;
}

static void free_map(Map *map) {
    if (!map) return;
    map_fini(map);
    free(map);
}

static int feq(const float a, const float b) { return fabsf(a - b) < 1e-4f; }

// ---------------------------------------------------------------- parsing

static void test_tables(void) {
    printf("Running v10 tables...\n");
    CHECK(PREFAB_COUNT == 100 && PREFAB_V7_COUNT == 97 && PREFAB_SCRAPCRATE == 97 && PREFAB_ENTITY == 98 && PREFAB_COLLISION_MESH == 99,
          "prefab ids");
    CHECK(PREFAB_TERMINAL == 41 && PREFAB_BOT_PATH_NODE == 41, "41 TERMINAL / BOT_PATH_NODE");
    CHECK(!strcmp(prefab_name(PREFAB_SCRAPCRATE), "SCRAPCRATE") && !strcmp(prefab_name(PREFAB_ENTITY), "ENTITY") &&
              !strcmp(prefab_name(41), "BOT_PATH_NODE"), "names (41 is v10.0.3's BOT_PATH_NODE)");
    CHECK(prefab_id_from_name("BOT_PATH_NODE") == 41 && prefab_id_from_name("TERMINAL") == 41 && prefab_id_from_name("ENTITY") == 98 &&
              prefab_id_from_name("SCRAPCRATE") == 97, "name lookup and the TERMINAL alias");
    const PrefabData *scrap = prefab_data(PREFAB_SCRAPCRATE), *crate = prefab_data(PREFAB_CRATE);
    CHECK(scrap && scrap->scale == 5.0f && !strcmp(scrap->model, "crate_0") && prefab_has_flag(PREFAB_SCRAPCRATE, PF_HASHEALTH) &&
              prefab_has_flag(PREFAB_SCRAPCRATE, PF_CASTSHADOW) && !prefab_has_flag(PREFAB_SCRAPCRATE, PF_ISPROP) && crate->scale == 6.0f,
          "SCRAPCRATE data (crate_0 at scale 5, not a prop)");
    CHECK(prefab_info(PREFAB_SCRAPCRATE)->geometry == PREFAB_GEO_MODEL && prefab_has_role(PREFAB_ENTITY, PREFAB_R_NO_OBJECT) &&
              prefab_has_role(PREFAB_BOT_PATH_NODE, PREFAB_R_NO_OBJECT), "runtime roles");
    CHECK(prefab_has_flag(PREFAB_ENTITY, PF_ISENTITY) && prefab_data(PREFAB_ENTITY)->opacity == 0.3f && prefab_data(PREFAB_ENTITY)->line_col == 65484,
          "ENTITY data");
    // deob-10.0.5.js:158847-158862: COLLISION_MESH (cube_0, defaultSize [1, 1, 1], scalable, noTexture, noExport, tool,
    // transparent, doubleSide, opacity 0.35, lineCol 0xff00ff, noMinimap); no map object (it ships as "tris")
    const PrefabData *cm = prefab_data(PREFAB_COLLISION_MESH);
    CHECK(cm && !strcmp(cm->name, "COLLISION_MESH") && prefab_id_from_name("COLLISION_MESH") == 99 && cm->opacity == 0.35f &&
              cm->line_col == 0xff00ff && cm->default_size[0] == 1.0f && cm->default_size[1] == 1.0f && cm->default_size[2] == 1.0f &&
              prefab_has_flag(99, PF_SCALABLE) && prefab_has_flag(99, PF_NOTEXTURE) && prefab_has_flag(99, PF_NOEXPORT) &&
              prefab_has_flag(99, PF_TOOL) && prefab_has_flag(99, PF_TRANSPARENT) && prefab_has_flag(99, PF_DOUBLESIDE) &&
              prefab_has_flag(99, PF_NOMINIMAP) && prefab_has_flag(99, PF_GENGEO) && prefab_has_role(99, PREFAB_R_NO_OBJECT),
          "COLLISION_MESH data and role");
    // deob-10.0.3.js:174150: BOT_PATH_NODE noCol, noCollision, noTexture, notStyleable, notTechnical, tool, genGeo,
    // defaultSize [3, 5, 3], lineCol 65535 (v7's TERMINAL cube flags are gone)
    const PrefabData *node = prefab_data(PREFAB_BOT_PATH_NODE);
    CHECK(node && prefab_has_flag(41, PF_NOCOL) && prefab_has_flag(41, PF_NOCOLLISION) && prefab_has_flag(41, PF_NOTEXTURE) &&
              prefab_has_flag(41, PF_NOTSTYLEABLE) && prefab_has_flag(41, PF_NOTTECHNICAL) && prefab_has_flag(41, PF_TOOL) &&
              prefab_has_flag(41, PF_GENGEO) && !prefab_has_flag(41, PF_HIDEBOUNDINGBOX) && !prefab_has_flag(41, PF_EDITFACES) &&
              !prefab_has_flag(41, PF_CANINTERFACE) && !prefab_has_flag(41, PF_SCALABLE) && node->line_col == 65535 &&
              feq(node->default_size[0], 3.0f) && feq(node->default_size[1], 5.0f) && feq(node->default_size[2], 3.0f),
          "BOT_PATH_NODE data (10.0.3 flags, 3 x 5 x 3)");
    CHECK(prefab_has_flag(PREFAB_CUBE, PF_CANDYNAMIC) && prefab_has_flag(PREFAB_TRIGGER, PF_CANDYNAMIC) &&
              prefab_has_flag(PREFAB_CUSTOM_ASSET, PF_CANDYNAMIC) && !prefab_has_flag(PREFAB_CRATE, PF_CANDYNAMIC), "canDynamic");
    CHECK(!strcmp(g_trigger_v7_conditions[12].name, "Custom Action") && g_trigger_v7_conditions[12].val_count == 1 &&
              g_trigger_v7_conditions[12].vals[0].kind == TRIGGER_V7_VAL_INPUT && g_trigger_v7_conditions[12].vals[0].len == 30,
          "condition 12");
    CHECK(!strcmp(g_trigger_v7_actions[72].name, "Spawn Entity") && (g_trigger_v7_actions[72].flags & TRIGGER_V7_ACT_TEXT) &&
              !strcmp(g_trigger_v7_actions[73].name, "Despawn Entity") && !strcmp(g_trigger_v7_actions[74].name, "Set Entity Value") &&
              (g_trigger_v7_actions[74].flags & TRIGGER_V7_ACT_BOTH), "actions 72-74");
    CHECK(TRIGGER_V10_CONDITION_COUNT == 13 && TRIGGER_V10_ACTION_COUNT == 75, "table counts");
    CHECK(ruleset_current()->trigger_action_limit == 8, "triggerActionLimit 8");
}

static void test_parse_v10(void) {
    printf("Running the v10 fixture...\n");
    cJSON *json = load(FIXTURE);
    CHECK(json != NULL, FIXTURE " missing");
    if (!json) return;
    Map *map = map_init_ex(json, 0);
    CHECK(map != NULL, "map_init");
    if (!map) return;

    // 41 and 98 build no map object; 97 is a model prop
    CHECK(map->object_count == 6, "6 map objects (no BOT_PATH_NODE, no ENTITY): %zu", map->object_count);
    CHECK(count_prefab(map, 41) == 0 && count_prefab(map, PREFAB_ENTITY) == 0 && count_prefab(map, PREFAB_SCRAPCRATE) == 1, "prefab counts");
    const Object *scrap = first_prefab(map, PREFAB_SCRAPCRATE);
    CHECK(scrap && scrap->collision_type == COLLISION_TYPE_BOX && scrap->health == 25.0f && scrap->can_destroy && scrap->wall_jumpable,
          "SCRAPCRATE: a solid destructible prop");

    // dyn: CUBE / TRIGGER / CUSTOM_ASSET only; a dyn noCol cube keeps health and interface
    const Object *cube = map->objects[1], *trigger = first_prefab(map, PREFAB_TRIGGER), *asset = first_prefab(map, PREFAB_CUSTOM_ASSET);
    const Object *crate = first_prefab(map, PREFAB_CRATE);
    CHECK(cube->dyn && cube->collision_type == COLLISION_TYPE_NONE && cube->health == 40.0f && cube->interface_id == 4,
          "dyn noCol cube keeps its GameObject (health 40, in 4)");
    CHECK(trigger && trigger->dyn && asset && !asset->dyn && crate && !crate->dyn, "dyn on canDynamic prefabs only");
    CHECK(asset->mp && cJSON_IsObject(cJSON_GetObjectItem(asset->mp, "n")) &&
              cJSON_GetNumberValue(cJSON_GetObjectItem(cJSON_GetObjectItem(asset->mp, "l"), "i")) == 2.0, "CUSTOM_ASSET mp");
    CHECK(!map->objects[0]->mp, "no mp on a plain cube");

    // triggers: all 8 actions kept, the v10 actions and condition parse
    CHECK(trigger->trig && trigger->trig->action_count == 8, "8 trigger actions: %d", trigger->trig ? trigger->trig->action_count : -1);
    if (trigger->trig && trigger->trig->action_count == 8) {
        const TriggerActionData *a = trigger->trig->actions;
        CHECK(a[0].action == 72 && !strcmp(a[0].text, " lift | elevator ") && a[1].action == 73 && a[2].action == 74 && a[2].constant == 4,
              "actions 72-74 parsed");
        CHECK(a[3].condition == 12 && a[3].condition_values[0], "condition 12 with its param");
    }

    // map level
    CHECK(feq(map->env.shadow_nb, 5.0f), "shadowNB clamped to 0..5: %g", map->env.shadow_nb);
    CHECK(map->env.sky_dome_tex_asset == 1234 && map->env.sky_dome_emis_tex == 55 && map->minimap_asset == 77,
          "widened texture ids (1234mr, 55v, 77bh): %d %d %d", map->env.sky_dome_tex_asset, map->env.sky_dome_emis_tex, map->minimap_asset);
    CHECK(map->shared_script && !strcmp(cJSON_GetStringValue(map->shared_script), "YWN0aW9uIG9uRW50aXR5SW5pdCgpIHt9"), "scripts.shared");

    // botPaths: the "p" string entry is skipped, coordinates are parseInt'd
    CHECK(map->bot_path_count == 2, "2 bot paths: %zu", map->bot_path_count);
    if (map->bot_path_count == 2) {
        const MapBotPath *a = &map->bot_path_list[0], *b = &map->bot_path_list[1];
        CHECK(a->reversible && a->node_count == 3 && a->nodes[2].x == 100.0f && a->nodes[1].z == -60.0f, "path 0 (reversible, \"100.7\" -> 100)");
        CHECK(!b->reversible && b->node_count == 5 && b->nodes[4].z == 600.0f, "path 1");
    }

    // archetypes and entity instances
    CHECK(map->archetype_count == 2, "2 archetypes: %zu", map->archetype_count);
    const MapArchetype *lift = map_archetype(map, 3), *nine = map_archetype(map, 9);
    CHECK(lift && !strcmp(lift->name, "lift") && lift->coll == 1 && lift->hp == 50.0f && lift->data && lift->object_count == 3, "archetype 3");
    if (lift) {
        // solid parts: x [-10, 10], z [-5, 5], height max(0 + 2, 2 + 6) = 8; the soft part is a zone
        CHECK(feq(lift->foot_cx, 0.0f) && feq(lift->foot_hx, 10.0f) && feq(lift->foot_hz, 5.0f) && feq(lift->foot_h, 8.0f),
              "footprint %g %g %g %g", lift->foot_cx, lift->foot_hx, lift->foot_hz, lift->foot_h);
        CHECK(lift->soft_zone_count == 1 && feq(lift->soft_zones[0].centre.y, 4.0f) && feq(lift->soft_zones[0].half.x, 10.0f), "soft zone");
    }
    CHECK(nine && !strcmp(nine->name, "archetype_9") && nine->coll == 0 && !nine->data, "archetype 9 defaults");
    CHECK(map->entity_instance_count == 2, "2 entity instances: %zu", map->entity_instance_count);
    if (map->entity_instance_count == 2) {
        const MapEntityInstance *e = &map->entity_instances[0], *f = &map->entity_instances[1];
        CHECK(e->eid == 1 && e->archetype == 3 && e->has_tag && !strcmp(e->tag, "elevator") && e->auto_spawn && feq(e->rotation.y, 1.5f) &&
                  feq(e->position.z, 30.0f) && e->spawn_params, "instance 1");
        CHECK(f->eid == 2 && f->archetype == 9 && !f->auto_spawn && !f->has_tag && !f->spawn_params, "instance 2 (as: 0)");
    }
    MapIssue issues[8];
    size_t errors = 0;
    map_validate(json, NULL, issues, 8, &errors);
    CHECK(errors == 0, "the fixture is a valid v10 map (%zu errors: %s)", errors, errors ? issues[0].message : "");
    free_map(map);
    cJSON_Delete(json);
}

// v10.0.5 COLLISION_MESH: the game never builds the object (game.js reads no "cmt"); its triangles come as the map's
// "tris" (the editor's export, editor-10.0.5.js:16454-16460), which collide like any baked triangle
static void test_collision_mesh(void) {
    printf("Running v10.0.5 collision meshes...\n");
    cJSON *json = cJSON_Parse("{\"name\":\"cm\",\"spawns\":[[0,0,0]],\"objects\":[{\"i\":0,\"p\":[0,-1,0],\"s\":[50,1,50]},"
                              "{\"i\":99,\"p\":[30,0,0],\"s\":[20,10,20],\"cmt\":[-10,0,-10,10,10,-10,10,10,10]}],"
                              "\"tris\":[20,0,-10,40,10,-10,40,10,10,20,0,-10,40,10,10,20,0,10]}");
    Map *map = json ? map_init_ex(json, 0) : NULL;
    CHECK(map && map->object_count == 1 && count_prefab(map, PREFAB_COLLISION_MESH) == 0, "a saved COLLISION_MESH builds no map object");
    CHECK(map && map->tri_count == 2, "the map's tris load (%zu)", map ? map->tri_count : 0);
    free_map(map);
    cJSON_Delete(json);
}

// ---------------------------------------------------------------- validation

static size_t errors_of(const char *json_text, char *first, const size_t size) {
    cJSON *json = cJSON_Parse(json_text);
    if (!json) return 99;
    MapIssue issue = {0};
    size_t errors = 0;
    const MapValidateOptions options = {.first_only = 1, .no_warnings = 1};
    map_validate(json, &options, &issue, 1, &errors);
    snprintf(first, size, "%s", errors ? issue.message : "");
    cJSON_Delete(json);
    return errors;
}

#define V_HEAD "{\"name\":\"v\",\"spawns\":[[0,0,0]],"
#define V_CUBE "{\"i\":0,\"p\":[0,0,0],\"s\":[10,10,10]}"

static void expect_error(const char *json, const char *message) {
    char first[MAP_ISSUE_MESSAGE_SIZE];
    const size_t n = errors_of(json, first, sizeof(first));
    CHECK(n > 0 && !strcmp(first, message), "expected \"%s\", got \"%s\"", message, n ? first : "(valid)");
}

static void expect_valid(const char *json) {
    char first[MAP_ISSUE_MESSAGE_SIZE];
    CHECK(errors_of(json, first, sizeof(first)) == 0, "expected valid, got \"%s\"", first);
}

static void test_validation(void) {
    printf("Running v10 validation...\n");
    expect_valid(V_HEAD "\"objects\":[" V_CUBE ",{\"i\":97,\"p\":[0,0,0],\"s\":[5,5,5]}]}");
    // deob-10.0.5.js:191206: prefabs[i] must exist; 99 is v10.0.5's COLLISION_MESH, 100 is past the table
    expect_valid(V_HEAD "\"objects\":[" V_CUBE ",{\"i\":99,\"p\":[0,0,0],\"s\":[5,5,5],\"cmt\":[0,0,0,1,0,0,0,1,0]}]}");
    expect_error(V_HEAD "\"objects\":[{\"i\":100,\"p\":[0,0,0],\"s\":[5,5,5]}]}", "Invalid Object.");
    expect_error(V_HEAD "\"archetypes\":5,\"objects\":[" V_CUBE "]}", "Archetype list error.");
    expect_error(V_HEAD "\"archetypes\":[{\"id\":0,\"objects\":[" V_CUBE "]}],\"objects\":[" V_CUBE "]}", "Archetype id error.");
    expect_error(V_HEAD "\"archetypes\":[{\"id\":4,\"objects\":[" V_CUBE "]},{\"id\":4,\"objects\":[" V_CUBE "]}],\"objects\":[" V_CUBE "]}",
                 "Duplicate archetype id. (ID: 4)");
    expect_error(V_HEAD "\"archetypes\":[{\"id\":4,\"name\":\"a.b\",\"objects\":[" V_CUBE "]}],\"objects\":[" V_CUBE "]}",
                 "Illegal characters in Archetype name. (ID: 4)");
    expect_error(V_HEAD "\"archetypes\":[{\"id\":4,\"coll\":3,\"objects\":[" V_CUBE "]}],\"objects\":[" V_CUBE "]}",
                 "Archetype collision error. (ID: 4)");
    expect_error(V_HEAD "\"archetypes\":[{\"id\":4,\"objects\":[]}],\"objects\":[" V_CUBE "]}", "Archetype missing objects. (ID: 4)");
    expect_error(V_HEAD "\"archetypes\":[{\"id\":4,\"objects\":[{\"i\":98,\"p\":[0,0,0],\"s\":[1,1,1]}]}],\"objects\":[" V_CUBE "]}",
                 "Archetypes cannot contain entities. (ID: 4)");
    expect_error(V_HEAD "\"archetypes\":[{\"id\":4,\"objects\":[{\"i\":0,\"n\":\"bad name\",\"p\":[0,0,0],\"s\":[1,1,1]}]}],\"objects\":[" V_CUBE "]}",
                 "Archetype part name error. (ID: 4)");
    // 41 parts
    char big[8192];
    int n = snprintf(big, sizeof(big), V_HEAD "\"archetypes\":[{\"id\":4,\"objects\":[");
    for (int i = 0; i < 41; i++) n += snprintf(big + n, sizeof(big) - (size_t) n, "%s" V_CUBE, i ? "," : "");
    snprintf(big + n, sizeof(big) - (size_t) n, "]}],\"objects\":[" V_CUBE "]}");
    expect_error(big, "Archetype exceeds object limit. (ID: 4)");

#define ARCH4 "\"archetypes\":[{\"id\":4,\"objects\":[" V_CUBE "]}],"
    expect_error(V_HEAD ARCH4 "\"objects\":[{\"i\":98,\"p\":[0,0,0],\"s\":[1,1,1],\"eid\":0,\"ar\":4}]}", "Entity id error.");
    expect_error(V_HEAD ARCH4 "\"objects\":[{\"i\":98,\"p\":[0,0,0],\"s\":[1,1,1],\"eid\":1,\"ar\":5}]}", "Unknown entity archetype. (ID: 1)");
    expect_error(V_HEAD ARCH4 "\"objects\":[{\"i\":98,\"p\":[0,0,0],\"s\":[1,1,1],\"eid\":1,\"ar\":4},{\"i\":98,\"p\":[0,0,0],\"s\":[1,1,1],\"eid\":1,\"ar\":4}]}",
                 "Duplicate entity id. (ID: 1)");
    expect_error(V_HEAD ARCH4 "\"objects\":[{\"i\":98,\"p\":[0,0,0],\"s\":[1,1,1],\"eid\":1,\"ar\":4,\"tag\":\"a b\"}]}",
                 "Illegal characters in Entity tag. (ID: 1)");
    expect_error(V_HEAD ARCH4 "\"objects\":[{\"i\":98,\"p\":[0,0,0],\"s\":[1,1,1],\"eid\":1,\"ar\":4,\"as\":2}]}", "Entity auto spawn error. (ID: 1)");
    expect_error(V_HEAD ARCH4 "\"objects\":[{\"i\":98,\"p\":[0,0,0],\"s\":[1,1,1],\"eid\":1,\"ar\":4,\"sp\":[1]}]}", "Entity spawn params error. (ID: 1)");
    expect_valid(V_HEAD ARCH4 "\"objects\":[{\"i\":98,\"p\":[0,0,0],\"s\":[1,1,1],\"eid\":1,\"ar\":4,\"tag\":\"lift-1\",\"as\":0,\"sp\":{\"a\":1}}]}");
    n = snprintf(big, sizeof(big), V_HEAD ARCH4 "\"objects\":[");
    for (int i = 1; i <= 65; i++) n += snprintf(big + n, sizeof(big) - (size_t) n, "%s{\"i\":98,\"p\":[0,0,0],\"s\":[1,1,1],\"eid\":%d,\"ar\":4}", i > 1 ? "," : "", i);
    snprintf(big + n, sizeof(big) - (size_t) n, "]}");
    expect_error(big, "Too many entities.");

    // the 8-action limit; condition 12 and actions 72-74 are known ids
#define TAS(n) "\"objects\":[{\"i\":29,\"p\":[0,0,0],\"s\":[10,10,10],\"tas\":[" n "]}]}"
#define A1 "{\"ta\":0}"
    expect_valid(V_HEAD TAS(A1 "," A1 "," A1 "," A1 "," A1 "," A1 "," A1 "," A1));
    expect_error(V_HEAD TAS(A1 "," A1 "," A1 "," A1 "," A1 "," A1 "," A1 "," A1 "," A1), "Trigger Has Too Many Actions");
    expect_valid(V_HEAD TAS("{\"ta\":74,\"cd\":12,\"cv0\":\"abc_123\"}"));
    expect_error(V_HEAD TAS("{\"ta\":0,\"cd\":13}"), "INVALID CONDITION");
}

// ---------------------------------------------------------------- triggers

static int g_fake_server;
static int g_should_calls, g_should_answer, g_should_sid;
static char g_should_param[64], g_should_player[16];
static int g_spawns, g_despawns, g_sets;
static char g_arch[64], g_tag[64], g_key[64];
static double g_value;
static vec3 g_at;

static int hook_should(Game *game, const char *player_id, const int sid, const char *param) {
    (void) game;
    g_should_calls++;
    g_should_sid = sid;
    snprintf(g_should_param, sizeof(g_should_param), "%s", param);
    snprintf(g_should_player, sizeof(g_should_player), "%s", player_id);
    return g_should_answer;
}
static void hook_spawn(Game *game, const char *arch, const char *tag, const vec3 at) {
    (void) game;
    g_spawns++;
    snprintf(g_arch, sizeof(g_arch), "%s", arch);
    snprintf(g_tag, sizeof(g_tag), "%s", tag ? tag : "(none)");
    g_at = at;
}
static void hook_despawn(Game *game, const char *tag) { (void) game; g_despawns++; snprintf(g_tag, sizeof(g_tag), "%s", tag); }
static void hook_set(Game *game, const char *tag, const char *key, const double value) {
    (void) game;
    g_sets++;
    snprintf(g_tag, sizeof(g_tag), "%s", tag);
    snprintf(g_key, sizeof(g_key), "%s", key);
    g_value = value;
}
static const TriggerScriptHooks g_hooks = {.should_trigger = hook_should, .spawn_entity = hook_spawn,
                                           .despawn_entities = hook_despawn, .set_entity_value = hook_set};

static cJSON *g_keep[16];
static int g_keep_count;
static const cJSON *g_keep_maps[16][1];

static Game *make_game(Game *game, const char *json) {
    memset(game, 0, sizeof(*game));
    cJSON *parsed = cJSON_Parse(json);
    if (!parsed || g_keep_count >= 16) exit(1);
    g_keep[g_keep_count] = parsed;
    g_keep_maps[g_keep_count][0] = parsed;
    game->server = &g_fake_server;
    game_configure(game, NULL, g_keep_maps[g_keep_count++], 1, NULL, 0, NULL, 0, NULL, 0);
    game_init(game, 0, -1, 0);
    game->now = 10.0f;
    return game;
}

static Player *add_player(Game *game, const int uid) {
    Player *p = player_init(game);
    p->uid = uid;
    snprintf(p->name, sizeof(p->name), "P%d", uid);
    game_players_add(game, p);
    player_spawn(p);
    return p;
}

#define T_MAP(tas) V_HEAD "\"objects\":[{\"i\":29,\"p\":[40,0,0],\"s\":[10,10,10],\"te\":0,\"tas\":[" tas "]}]}"

static void test_triggers(void) {
    printf("Running v10 triggers...\n");
    Game g;
    make_game(&g, T_MAP("{\"ta\":72,\"tct\":\" lift | elevator \"},{\"ta\":73,\"tct\":\" elevator \"},"
                        "{\"ta\":74,\"tct\":\"elevator| speed\",\"tc\":4},{\"ta\":0,\"tc\":5,\"cd\":12,\"cv0\":\"open_door\"},"
                        "{\"ta\":0,\"tc\":1},{\"ta\":0,\"tc\":1},{\"ta\":0,\"tc\":1},{\"ta\":0,\"tc\":1}"));
    CHECK(g.ready && g.rules == ruleset_current(), "v10 game");
    Player *p = add_player(&g, 1);
    p->score = 0;
    Object *t = g.map->objects[0];
    CHECK(t->trig && t->trig->action_count == 8, "8 actions");

    // no hooks: entity actions do nothing, condition 12 fails (v10 without shouldTrigger), the rest run
    game_on_trigger(&g, p, t, NULL);
    CHECK(p->score == 4, "no hooks: 4 x Give Score 1, the Custom Action condition fails (%d)", p->score);

    triggers_set_script_hooks(&g, &g_hooks);
    g_should_answer = 0;
    p->score = 0;
    game_on_trigger(&g, p, t, NULL);
    CHECK(g_spawns == 1 && !strcmp(g_arch, "lift") && feq(g_at.x, 40.0f), "72 Spawn Entity: \"lift\" at the trigger (%s)", g_arch);
    CHECK(g_despawns == 1, "73 Despawn Entity");
    CHECK(g_sets == 1 && !strcmp(g_tag, "elevator") && !strcmp(g_key, "speed") && g_value == 4.0, "74 Set Entity Value elevator|speed = 4");
    CHECK(g_should_calls == 1 && !strcmp(g_should_param, "open_door") && g_should_sid == 0 && !strcmp(g_should_player, "1"),
          "condition 12 calls shouldTrigger(\"1\", 0, \"open_door\")");
    CHECK(p->score == 4, "shouldTrigger false: the guarded action is skipped (%d)", p->score);
    g_should_answer = 1;
    p->score = 0;
    game_on_trigger(&g, p, t, NULL);
    CHECK(p->score == 9, "shouldTrigger true: Give Score 5 runs too (%d)", p->score);
    // the tag of action 72 (trimmed, the second split part)
    CHECK(g_spawns == 2, "spawn again");
    triggers_set_script_hooks(&g, NULL);
}

// ---------------------------------------------------------------- bots

static void tick(Game *game, const float seconds) {
    static double now;
    const float dt = 1.0f / 30.0f;
    for (float t = 0.0f; t < seconds; t += dt) {
        now += dt;
        game_tick(game, (float) now, dt);
    }
}

// A tfue bot with usePathing walks the one-way bot path (z 200 -> 600) when it has no target.
static void test_bot_paths(void) {
    printf("Running v10 bot paths...\n");
    Game g;
    make_game(&g, "{\"name\":\"paths\",\"spawns\":[[0,0,180]],\"objects\":[{\"i\":0,\"p\":[0,-10,0],\"s\":[1400,10,1400]}],"
                  "\"botPaths\":[{\"p\":[[0,0,200],[0,0,300],[0,0,400],[0,0,500],[0,0,600]]}]}");
    CHECK(g.ready && g.map->bot_path_count == 1, "path map");
    g.round_length = 100000.0f;
    game_bots_set_type(&g, BOT_TYPE_TFUE);
    BotAssignments a;
    char *argv[] = {"usePathing=on", "roam=off"};
    bot_parse_assignments(2, argv, &a);
    CHECK(!a.error[0], "usePathing parses (%s)", a.error);
    GameBot *bot = game_bots_add(&g, &a, NULL, 0.0f);
    CHECK(bot && bot->config.use_pathing, "bot with usePathing");
    if (!bot) return;
    tick(&g, 0.5f);
    CHECK(bot->player && bot->player->active, "spawned");
    if (!bot->player) return;
    float max_z = bot->player->position.z;
    for (int s = 0; s < 40 * 30 && max_z < 590.0f; s++) {
        tick(&g, 1.0f / 30.0f);
        if (bot->player->position.z > max_z) max_z = bot->player->position.z;
    }
    CHECK(max_z > 560.0f, "the bot walked the path to its end (max z %.0f)", max_z);
    CHECK(bot->route_path == 0, "following path 0 (%d)", bot->route_path);

    // without usePathing the same bot stays at its spawn (roam off, no target)
    game_bots_remove_all(&g);
    char *argv2[] = {"roam=off"};
    bot_parse_assignments(1, argv2, &a);
    bot = game_bots_add(&g, &a, NULL, 0.0f);
    tick(&g, 5.0f);
    CHECK(bot && bot->player && fabsf(bot->player->position.z - 180.0f) < 20.0f, "no usePathing: stays put (z %.0f)",
          bot && bot->player ? bot->player->position.z : 0.0f);
    game_bots_remove_all(&g);
}

// ---------------------------------------------------------------- lights

static void test_light_units(void) {
    printf("Running light units...\n");
    CHECK(feq(ruleset_current()->light_unit_factor, (float) M_PI), "v10: x PI");
}

int main(void) {
    test_tables();
    test_parse_v10();
    test_collision_mesh();
    test_validation();
    test_triggers();
    test_bot_paths();
    test_light_units();
    printf("map_v10_test %s: %d checks, %d failures\n", g_failures ? "FAILED" : "OK", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
