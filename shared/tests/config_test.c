// WP-E custom game config (shared/src/game_config.c) against Krunker v7.1.4's own code:
// shared/tests/data/v7/config_cases.json holds what v7's setupGameConfig + applyConfig produce
// for each request (tools/kref/v7-config-dump.js); every serverConfig value, config.classes and
// the overridden weapon / class / effect entries must match. Then the KN-only parts: validation,
// server.cfg merging, the v7 bugs fixed, class overrides at spawn, map "config" on hosted /
// offline games, GAME_CONFIG sync, match votes and network rates.
// Run from the repository root: ./<build>/config_test
#include <shared.h>
#include <game_config.h>
#include <account.h>
#include <modes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ruleset.h>

static int g_failures;

#define CHECK(cond, ...) do { \
    if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_failures++; } \
} while (0)

#define MAX_SENT 2048
typedef struct {
    int uid, type;
    uint16_t length;
    uint8_t payload[NET_MAX_PAYLOAD];
} SentPacket;
static SentPacket g_sent[MAX_SENT];
static int g_sent_count;

static void test_send(Game *game, const int uid, const int type, const void *payload, const uint16_t length) {
    (void) game;
    if (g_sent_count >= MAX_SENT) return;
    SentPacket *p = &g_sent[g_sent_count++];
    p->uid = uid;
    p->type = type;
    p->length = length;
    memcpy(p->payload, payload, length);
}

static const SentPacket *last_sent(const int type, const int uid) {
    for (int i = g_sent_count - 1; i >= 0; i--) {
        if (g_sent[i].type == type && (uid == -100 || g_sent[i].uid == uid)) return &g_sent[i];
    }
    return NULL;
}

static cJSON *load(const char *path) {
    cJSON *json = NULL;
    char alt[512];
    snprintf(alt, sizeof(alt), "../%s", path);
    if (load_json_file(path, &json) && load_json_file(alt, &json)) return NULL;
    return json;
}

static int near(const double a, const double b) {
    return fabs(a - b) <= 1e-4 * fmax(1.0, fabs(b));
}

// A fresh authority game on `map` (default rotation weapons / classes).
static void game_on(Game *game, const cJSON *map) {
    memset(game, 0, sizeof(*game));
    static const cJSON *maps[1];
    maps[0] = map;
    game_configure(game, NULL, maps, 1, NULL, 0, NULL, 0, NULL, 0);
    game->hooks.send = test_send;
}

static void game_off(Game *game) {
    game_config_free(game);
    if (game->map) map_fini(game->map);
    if (game->mode) mode_fini(game->mode);
    free(game->weapons);
    free(game->classes);
    free(game->modes);
    memset(game, 0, sizeof(*game));
}

// One expected value: number / bool vs the KN value read back in v7 units.
static void check_value(const char *what, const GameConfigTable table, const void *entry, const cJSON *expect) {
    double num = 0.0;
    const char *str = NULL;
    if (!game_config_value(table, entry, expect->string, &num, &str)) return; // not kept by KN
    if (cJSON_IsString(expect)) {
        CHECK(str && !strcmp(str, expect->valuestring), "%s %s = \"%s\", v7 \"%s\"", what, expect->string, str ? str : "(null)", expect->valuestring);
    } else if (cJSON_IsBool(expect)) {
        CHECK((num != 0.0) == cJSON_IsTrue(expect), "%s %s = %g, v7 %s", what, expect->string, num, cJSON_IsTrue(expect) ? "true" : "false");
    } else if (cJSON_IsNumber(expect)) {
        const double want = expect->valuedouble;
        if (!strcmp(expect->string, "lckCmYR")) { // KN keeps it as a flag (ClassMovement.lock_pitch): v7 only tests it
            CHECK((num != 0.0) == (want != 0.0), "%s lckCmYR = %g, v7 %g", what, num, want);
            return;
        }
        CHECK(near(num, want), "%s %s = %g, v7 %g", what, expect->string, num, want);
    }
}

// The config's own value of weps / cls entry `id`'s `key` (the request, after setupGameConfig)
static int requested(const cJSON *cfg, const char *list, const int id, const char *key) {
    char index[16];
    snprintf(index, sizeof(index), "%d", id);
    const cJSON *entry = cJSON_GetObjectItem(cJSON_GetObjectItem(cfg, list), index);
    return entry && cJSON_GetObjectItemCaseSensitive(entry, key);
}

// A weapon / class value the config leaves alone is the base table's: v7.1.4's setupGameConfig
// produced the expectations, and Krunker v10.0.2 (the ruleset) rebalanced some defaults
// (doc/v10/gameplay.md sections 4-5), so those are compared with the v10 base entry instead.
static void check_entry(const char *what, const GameConfigTable table, const void *entry, const void *base, const cJSON *cfg,
                        const char *list, const int id, const cJSON *expect) {
    double base_num = 0.0;
    const char *str = NULL;
    if (!requested(cfg, list, id, expect->string) && cJSON_IsNumber(expect) &&
        game_config_value(table, base, expect->string, &base_num, &str) && !near(base_num, expect->valuedouble)) {
        cJSON *v10 = cJSON_CreateNumber(base_num);
        v10->string = expect->string;
        check_value(what, table, entry, v10);
        v10->string = NULL;
        cJSON_Delete(v10);
        return;
    }
    check_value(what, table, entry, expect);
}

static void check_case(const cJSON *c, const cJSON *map) {
    const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(c, "name"));
    const int host = cJSON_GetObjectItem(c, "host")->valueint;
    const int clamp = cJSON_GetObjectItem(c, "clamp")->valueint;
    const cJSON *request = cJSON_GetObjectItem(c, "request");
    const int setup = cJSON_IsTrue(cJSON_GetObjectItem(c, "setup"));
    cJSON *cfg = setup ? game_config_setup(host, request, NULL, 0) : cJSON_Duplicate(request, 1);

    if (setup) {
        static const char *const lists[2][2] = {{"setupMaps", "maps"}, {"setupModes", "modes"}};
        for (int l = 0; l < 2; l++) {
            const cJSON *want = cJSON_GetObjectItem(c, lists[l][0]);
            const cJSON *have = cJSON_GetObjectItem(cfg, lists[l][1]);
            char *a = cJSON_PrintUnformatted(want), *b = have ? cJSON_PrintUnformatted(have) : NULL;
            const int same = cJSON_IsNull(want) ? !have : (b && !strcmp(a, b));
            CHECK(same, "%s: setup %s %s, v7 %s", name, lists[l][1], b ? b : "(none)", a);
            cJSON_free(a);
            cJSON_free(b);
        }
    }

    Game game;
    game_on(&game, map);
    game_config_apply(&game, cfg, clamp, host);

    const cJSON *expect;
    char what[160];
    snprintf(what, sizeof(what), "%s: settings", name);
    cJSON_ArrayForEach(expect, cJSON_GetObjectItem(c, "settings")) {
        if (!strcmp(expect->string, "hitBoxPad") && !game.config.hitbox_pad_set) {
            CHECK(near(0.6, expect->valuedouble), "%s hitBoxPad: KN mode default 0.6, v7 %g", what, expect->valuedouble);
            continue;
        }
        check_value(what, GAME_CONFIG_SERVER, &game.config, expect);
    }
    CHECK(game.config.comp == cJSON_IsTrue(cJSON_GetObjectItem(c, "isComp")) || host == GAME_HOST_COMP, "%s: isComp", name);
    CHECK(game.config.mimic_pubs == cJSON_IsTrue(cJSON_GetObjectItem(c, "mimicPubs")), "%s: mimicPubs", name);
    CHECK(game.config.is_private == cJSON_IsTrue(cJSON_GetObjectItem(c, "private")), "%s: private", name);

    // config.classes (KN: 0 entries = every class, what resetClasses gives a custom game)
    const cJSON *classes = cJSON_GetObjectItem(c, "classes");
    if (!game.config.class_list_count) {
        // (v7's 16; the game's table also holds the KrunkNative Nettspend, CLASS_TABLE_COUNT)
        CHECK(cJSON_GetArraySize(classes) == CLASS_COUNT && game.class_count == CLASS_TABLE_COUNT, "%s: classes reset, v7 has %d", name, cJSON_GetArraySize(classes));
    } else {
        int ok = cJSON_GetArraySize(classes) == game.config.class_list_count;
        for (int i = 0; ok && i < game.config.class_list_count; i++) ok = cJSON_GetArrayItem(classes, i)->valueint == game.config.class_list[i];
        CHECK(ok, "%s: classes list", name);
    }

    const cJSON *entry;
    cJSON_ArrayForEach(entry, cJSON_GetObjectItem(c, "weps")) {
        const int id = atoi(entry->string);
        snprintf(what, sizeof(what), "%s: weapon %d", name, id);
        cJSON_ArrayForEach(expect, entry)
            check_entry(what, GAME_CONFIG_WEAPON, game.weapons[id], rules_weapon(id), cfg, "weps", id, expect);
    }
    cJSON_ArrayForEach(entry, cJSON_GetObjectItem(c, "cls")) {
        const int id = atoi(entry->string);
        snprintf(what, sizeof(what), "%s: class %d", name, id);
        cJSON_ArrayForEach(expect, entry)
            check_entry(what, GAME_CONFIG_CLASS, &game.classes[id], &rules_classes()[id], cfg, "cls", id, expect);
    }
    cJSON_ArrayForEach(entry, cJSON_GetObjectItem(c, "prks")) {
        const int id = atoi(entry->string);
        snprintf(what, sizeof(what), "%s: effect %d", name, id);
        cJSON_ArrayForEach(expect, entry) check_value(what, GAME_CONFIG_EFFECT, game.effects ? &game.effects[id] : &g_effects[id], expect);
    }
    cJSON_Delete(cfg);
    game_off(&game);
}

static void test_v7_cases(const cJSON *map) {
    printf("v7 reference cases (setupGameConfig + applyConfig)...\n");
    cJSON *cases = load("shared/tests/data/v7/config_cases.json");
    CHECK(cases && cJSON_GetArraySize(cases) > 10, "shared/tests/data/v7/config_cases.json");
    const cJSON *c;
    cJSON_ArrayForEach(c, cases) check_case(c, map);
    cJSON_Delete(cases);
}

static void test_tables(void) {
    printf("option tables...\n");
    size_t n = 0;
    game_config_options(GAME_CONFIG_SERVER, &n);
    CHECK(n == 95, "serverConfig has %zu entries, v7 85 + v10 hurtboxScale / noPvp / transferHostOnLeave / webhook + KrunkNative knOmWeps (Object Manager) + knTPCam (third person) + knClanWar / knClanA / knClanB + v10.0.3 rampFix (Solid Ramp Sides)", n);
    game_config_options(GAME_CONFIG_CLASS, &n);
    CHECK(n == 80, "classConfig has %zu entries, v7 79 + v10 hurtboxScale", n);
    game_config_options(GAME_CONFIG_WEAPON, &n);
    CHECK(n == 48, "weaponConfig has %zu entries, v7 48", n);
    game_config_options(GAME_CONFIG_EFFECT, &n);
    CHECK(n == 5, "effectConfig has %zu entries, v7 5", n);
    const GameConfigOption *o = game_config_find_option(GAME_CONFIG_SERVER, "maxPlayers");
    CHECK(o && o->max == 10 && o->max_p == 16 && o->max_f == 20 && o->max_d == 50 && o->max_g == 6, "maxPlayers columns");
    o = game_config_find_option(GAME_CONFIG_SERVER, "gravMlt");
    CHECK(o && o->min == 0.0f, "gravMlt min 0 (v3 0.1)");
    CHECK(!game_config_find_option(GAME_CONFIG_SERVER, "requireCaptcha") && !game_config_find_option(GAME_CONFIG_SERVER, "maxPS"), "removed keys");
    CHECK(!strcmp(game_host_type_name(GAME_HOST_RAID), "Raid Server Setup") && !game_host_type_name(5), "host types");
}

static void test_validate(void) {
    printf("validateConfigs...\n");
    struct { const char *json, *error; } cases[] = {
        {"{}", NULL},
        {"{\"modes\":[0,4]}", NULL},
        {"{\"modes\":[99]}", "Invalid Mode in Config."},
        {"{\"settings\":{\"foo\":1}}", "Invalid Setting in Config."},
        {"{\"settings\":{\"gravMlt\":\"x\"}}", "Invalid Config Value. (Set: Gravity)"},
        {"{\"settings\":{\"maxPlayers\":51}}", "Config Value out of Range. (Set: Players)"},
        {"{\"settings\":{\"maxPlayers\":50,\"tmSize\":2}}", NULL},
        {"{\"settings\":{\"tmSize\":4}}", "Config Value out of Range. (Set: Team Size)"},
        {"{\"weps\":{\"0\":{\"bogus\":1}}}", "Invalid Setting in weaponConfig. (ID: 0)"},
        {"{\"weps\":{\"0\":{\"dmg\":201}}}", "weaponConfig Value out of Range. (ID: 0, Set: Damage)"},
        {"{\"cls\":{\"1\":{\"aID\":\"a<b\"}}}", "Invalid classConfig Value. (ID: 1, Set: Custom Asset)"},
        {"{\"cls\":{\"1\":{\"camType\":7}}}", "Config Value out of Range. (ID: 1, Set: Type)"},
        {"{\"prks\":{\"2\":{\"max\":11}}}", "effectConfig Value out of Range. (ID: 2, Set: Max Limit)"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        cJSON *json = cJSON_Parse(cases[i].json);
        const char *e = game_config_validate(json);
        CHECK(cases[i].error ? (e && !strcmp(e, cases[i].error)) : !e, "validate %s: \"%s\", want \"%s\"", cases[i].json, e ? e : "(ok)", cases[i].error ? cases[i].error : "(ok)");
        cJSON_Delete(json);
    }
    // config.classes: the table's last id is KrunkNative's Nettspend (CLASS_ID_NETTSPEND, after v10's CLASS_COUNT);
    // one past it is not a class
    for (int id = CLASS_ID_NETTSPEND; id <= CLASS_TABLE_COUNT; id++) {
        char text[48];
        snprintf(text, sizeof(text), "{\"classes\":[%d]}", id);
        cJSON *json = cJSON_Parse(text);
        const char *e = game_config_validate(json);
        const int want_ok = id < CLASS_TABLE_COUNT;
        CHECK(want_ok ? !e : (e && !strcmp(e, "Invalid Class in Config.")), "validate %s: \"%s\"", text, e ? e : "(ok)");
        cJSON_Delete(json);
    }
    // v7 bug fixed: "Too much data" measures the JSON
    cJSON *big = cJSON_CreateObject(), *weps = cJSON_AddObjectToObject(big, "weps");
    for (int i = 0; i < 2000; i++) {
        char id[16];
        snprintf(id, sizeof(id), "%d", i);
        cJSON *w = cJSON_AddObjectToObject(weps, id);
        cJSON_AddNumberToObject(w, "dmg", 1);
    }
    const char *e = game_config_validate(big);
    CHECK(e && !strcmp(e, "weaponConfig - Too much data."), "a 15000+ character weps is rejected (%s)", e ? e : "ok");
    cJSON_Delete(big);
}

static void test_inputs_and_merge(void) {
    printf("input names (v7 bug fixed), server.cfg merge...\n");
    GameConfig g = g_default_game_config;
    cJSON *cfg = cJSON_Parse("{\"nameTeam1\":\"<b>Hi</b>\",\"nameTeam2\":\"A very long team name here\",\"nameTeam3\":\"ok-name|x\"}");
    game_config_read(&g, cfg, GAME_CONFIG_CLAMP_HOSTED, GAME_HOST_CUSTOM);
    CHECK(!strcmp(g.team_names[1], "Alpha"), "a name with '/' after sanitizeStr falls back (%s)", g.team_names[1]);
    CHECK(!strcmp(g.team_names[2], "A very long team"), "cut to 16 characters (%s)", g.team_names[2]);
    CHECK(!strcmp(g.team_names[3], "ok-name|x") && g.team1_name == g.team_names[1] && !strcmp(g.team2_name, g.team_names[2]), "team names (%s)", g.team_names[3]);
    cJSON_Delete(cfg);

    g = g_default_game_config;
    game_config_sync_teams(&g);
    cfg = cJSON_Parse("{\"gameTime\":7,\"canSlide\":false,\"gravMlt\":5,\"t4Dmg\":0.5,\"hitBoxPad\":-1,\"bogus\":1,\"jumpMlt\":\"x\"}");
    const int applied = game_config_merge(&g, cfg, GAME_CONFIG_CLAMP_DEDICATED);
    CHECK(applied == 5, "merge applies the known keys of the right type (%d)", applied);
    CHECK(g.game_time == 7 && !g.can_slide && g.gravity_mlt == 2.0f && g.team_damage[4] == 0.5f, "merged values");
    CHECK(g.hitbox_pad == -1.0f && g.hitbox_pad_set, "hitBoxPad set");
    CHECK(g.max_players == g_default_game_config.max_players && g.jump_mlt == 1.0f && g.health_regen, "absent keys keep the server's values");
    cJSON_Delete(cfg);

    cJSON *round = game_config_write(&g);
    GameConfig back = g_default_game_config;
    game_config_read(&back, round, GAME_CONFIG_CLAMP_DEDICATED, GAME_HOST_CUSTOM);
    cJSON *again = game_config_write(&back);
    char *a = cJSON_PrintUnformatted(round), *b = cJSON_PrintUnformatted(again);
    CHECK(!strcmp(a, b), "write -> read -> write is stable");
    cJSON_free(a);
    cJSON_free(b);
    cJSON_Delete(round);
    cJSON_Delete(again);
}

static Player *spawn(Game *game, const int uid, const int class_index) {
    Player *p = player_init(game);
    p->uid = uid;
    p->class_index = class_index;
    game_players_add(game, p);
    player_spawn(p);
    return p;
}

static void test_class_overrides(const cJSON *map) {
    printf("class / weapon / effect overrides in play...\n");
    Game game;
    game_on(&game, map);
    game_init(&game, 0, 0, 1);
    cJSON *cfg = cJSON_Parse("{\"cls\":{\"0\":{\"health\":250,\"speed\":1.5,\"name\":\"Tank\",\"canShoot\":false,\"secondary\":false},"
                             "\"1\":{\"camType\":\"2\",\"movSch\":\"6\",\"arA\":0.03}},"
                             "\"weps\":{\"1\":{\"ammo\":99,\"rate\":200,\"name\":\"Hose\"}},\"prks\":{\"0\":{\"max\":3}},\"healthMlt\":2}");
    game_config_apply(&game, cfg, GAME_CONFIG_CLAMP_HOSTED, GAME_HOST_CUSTOM);
    Player *p = spawn(&game, 1, 0);
    CHECK(p->max_health == 500, "cls health 250 x healthMlt 2 = %d", p->max_health);
    CHECK(fabsf(p->speed - 1.5f) < 1e-6f, "cls speed");
    CHECK(!strcmp(game.classes[0].name, "Tank") && !class_overrides(&game.classes[0])->can_shoot, "cls name / canShoot");
    CHECK(p->loadout_size == 2, "secondary off: primary + knife (%d)", p->loadout_size);
    CHECK(p->weapon && p->weapon->ammo == 99 && fabsf(p->weapon->rate - 0.2f) < 1e-6f && !strcmp(p->weapon->name, "Hose"), "weps on the spawned AR");
    CHECK(rules_weapon(1)->ammo != 99 && strcmp(rules_weapon(1)->name, "Hose"), "the base table is untouched");
    CHECK(class_overrides(&game.classes[1])->cam_type == 2 && game.classes[1].movement.scheme == 6 && near(game.classes[1].movement.air_accel, 0.03), "camera / movement scheme");
    CHECK(!game.classes[2].ext.set && class_overrides(&game.classes[2])->can_shoot && class_overrides(&game.classes[2])->ortho_f == 50.0f, "untouched classes read the v7 defaults");
    CHECK(game.effects && game.effects[0].max == 3 && g_effects[0].max != 3, "prks on the game's effect table");

    // a new apply without overrides is back on the base tables
    cJSON *plain = cJSON_Parse("{}");
    game_config_apply(&game, plain, GAME_CONFIG_CLAMP_HOSTED, GAME_HOST_CUSTOM);
    CHECK(game.weapons[1] == rules_weapon(1) && game.classes[0].health == rules_classes()[0].health && !game.classes[0].ext.set && !game.effects, "overrides reset");
    cJSON_Delete(plain);

    // config.classes: classes[0] when the pick is not listed
    cJSON *list = cJSON_Parse("{\"classes\":[9,1]}");
    game_config_apply(&game, list, GAME_CONFIG_CLAMP_HOSTED, GAME_HOST_CUSTOM);
    Player *q = spawn(&game, 2, 0);
    CHECK(q->class_index == 9, "class 0 not in [9,1] -> 9 (%d)", q->class_index);
    Player *r = spawn(&game, 3, 1);
    CHECK(r->class_index == 1, "a listed class stays (%d)", r->class_index);
    cJSON_Delete(list);
    cJSON_Delete(cfg);
    game_off(&game);
}

static void test_map_config(cJSON *sandstorm) {
    printf("embedded map config (hosted / offline sandbox, public)...\n");
    cJSON *eterno = load("assets/maps/custom/eterno_jump.json");
    cJSON *plaza = load("assets/maps/custom/Krunk_Plaza.json");
    CHECK(eterno && plaza, "custom maps load");
    if (!eterno || !plaza) return;

    // public game: the map's config is a host preset only
    Game pub;
    game_on(&pub, eterno);
    game_init(&pub, 0, 0, 1);
    CHECK(pub.ready && pub.config.game_time == g_default_game_config.game_time && !pub.config.class_list_count && pub.classes[9].wall_jump == rules_classes()[9].wall_jump,
          "a public game ignores the map config");
    game_off(&pub);

    // the offline sandbox / a hosted game: setupGameConfig(0, map config) -> applyConfig(cfg, 3, 0)
    const cJSON *maps[3] = {eterno, plaza, sandstorm};
    const cJSON **list = malloc(sizeof(maps));
    memcpy(list, maps, sizeof(maps));
    Game game;
    memset(&game, 0, sizeof(game));
    game_configure(&game, NULL, list, 3, NULL, 0, NULL, 0, NULL, 0);
    game.hooks.send = test_send;
    game_config_host(&game, NULL, GAME_HOST_CUSTOM);
    game_init(&game, 0, 0, 1);
    CHECK(game.ready && game.config.game_time == 0 && game.config.max_players == 20 && game.config.head_clip_fix, "eterno_jump settings: gameTime 0, maxPlayers 20, headClipFix");
    CHECK(game.config.class_list_count == 1 && game.config.class_list[0] == 9 && !game.classes[9].wall_jump, "eterno_jump classes [9], cls 9 wallJ false");
    CHECK(game_config_is_custom(&game), "the sandbox is a custom game (isCustom)");

    game_init(&game, 1, 0, 1);
    CHECK(game.ready && game.classes[0].health == 120 && class_overrides(&game.classes[0])->cam_type == 3 && game.classes[0].movement.scheme == 7,
          "Krunk_Plaza cls 0: health 120, camType 3, movSch 7");
    CHECK(game.classes[3].movement.scheme == 6 && !strcmp(game.classes[3].name, "Farreri") && !strcmp(class_overrides(&game.classes[3])->asset_id, "48931cq"),
          "Krunk_Plaza cls 3: the vehicle class (%s)", game.classes[3].name);
    CHECK(game.config.class_list_count == 13 && game.config.max_players == 20 && game.config.game_time == 0 && game.config.wall_jump == 0.0f &&
          game.config.warmup_time == 0.0f && game.config.jump_mlt == 1.0f, "Krunk_Plaza: 13 classes, its settings, v7 defaults for the rest");
    CHECK(game.weapons[0]->ammo == 1 && fabsf(game.weapons[0]->reload_time - 5.0f) < 1e-6f && game.effects && game.effects[5].max == 10, "Krunk_Plaza weps / prks");

    game_init(&game, 2, 0, 1);
    CHECK(game.ready && game.config.game_time == g_default_game_config.game_time && !game.config.class_list_count && game.classes[0].health == rules_classes()[0].health,
          "a map without config: back to the sandbox's own settings");

    // the host's request wins over the map preset
    cJSON *request = cJSON_Parse("{\"gameTime\":9,\"cls\":{\"9\":{\"health\":33}}}");
    game_config_host(&game, request, GAME_HOST_CUSTOM);
    cJSON_Delete(request);
    game_init(&game, 0, 0, 1);
    CHECK(game.config.game_time == 9 && game.config.max_players == 20 && game.classes[9].health == 33, "host settings over eterno_jump's");
    game_off(&game);
    cJSON_Delete(eterno);
    cJSON_Delete(plaza);
}

static void test_mimic_pubs(cJSON *sandstorm) {
    printf("mimicPubs (game.isPubType): a host's request (operator preset, Host Game) and forced hosts carry it, a map's settings do not...\n");
    // setupGameConfig: forceDat (Zombies) sets it, overDat (Comp) has no key, a custom host keeps the request's
    cJSON *request = cJSON_Parse("{\"mimicPubs\":true}");
    cJSON *zombies = game_config_setup(GAME_HOST_ZOMBIES, NULL, NULL, 1);
    cJSON *comp = game_config_setup(GAME_HOST_COMP, request, NULL, 1);
    cJSON *custom = game_config_setup(GAME_HOST_CUSTOM, request, NULL, 1);
    CHECK(zombies && cJSON_IsTrue(cJSON_GetObjectItem(zombies, "mimicPubs")), "Zombies (forceDat) mimics pubs");
    CHECK(comp && !cJSON_IsTrue(cJSON_GetObjectItem(comp, "mimicPubs")), "Comp (overDat) never mimics pubs");
    CHECK(custom && cJSON_IsTrue(cJSON_GetObjectItem(custom, "mimicPubs")), "a custom host keeps the mimicPubs of its request");
    cJSON_Delete(zombies);
    cJSON_Delete(comp);
    cJSON_Delete(custom);
    cJSON_Delete(request);

    // a published map's embedded settings cannot set it
    cJSON *map_config = cJSON_Parse("{\"settings\":{\"mimicPubs\":true,\"gameTime\":5}}");
    cJSON *from_map = game_config_from_map(map_config);
    CHECK(from_map && !cJSON_GetObjectItem(from_map, "mimicPubs") && cJSON_GetObjectItem(from_map, "gameTime"), "a map's settings never set mimicPubs");
    cJSON_Delete(from_map);
    cJSON_Delete(map_config);

    // game.isPubType() = !hasHost() || mimicPubs, read on every round of a hosted game
    const cJSON *maps[1] = {sandstorm};
    Game pub;
    memset(&pub, 0, sizeof(pub));
    game_configure(&pub, NULL, memcpy(malloc(sizeof(maps)), maps, sizeof(maps)), 1, NULL, 0, NULL, 0, NULL, 0);
    pub.hooks.send = test_send;
    game_init(&pub, 0, 0, 1);
    CHECK(pub.ready && game_is_pub_type(&pub) && !game_config_is_custom(&pub), "a public game is a pub-type game");
    game_off(&pub);

    Game hosted;
    memset(&hosted, 0, sizeof(hosted));
    game_configure(&hosted, NULL, memcpy(malloc(sizeof(maps)), maps, sizeof(maps)), 1, NULL, 0, NULL, 0, NULL, 0);
    hosted.hooks.send = test_send;
    request = cJSON_Parse("{\"mimicPubs\":true}");
    game_config_host(&hosted, request, GAME_HOST_CUSTOM);
    cJSON_Delete(request);
    for (int round = 0; round < 2; round++) {
        game_init(&hosted, 0, 0, 1);
        CHECK(hosted.ready && game_config_is_custom(&hosted) && game_is_pub_type(&hosted), "a hosted game with mimicPubs is a pub-type game (round %d)", round + 1);
    }
    game_off(&hosted);

    Game plain;
    memset(&plain, 0, sizeof(plain));
    game_configure(&plain, NULL, memcpy(malloc(sizeof(maps)), maps, sizeof(maps)), 1, NULL, 0, NULL, 0, NULL, 0);
    plain.hooks.send = test_send;
    game_config_host(&plain, NULL, GAME_HOST_CUSTOM);
    game_init(&plain, 0, 0, 1);
    CHECK(plain.ready && game_config_is_custom(&plain) && !game_is_pub_type(&plain), "a hosted game without mimicPubs is not a pub-type game");
    game_off(&plain);
}

static void test_sync(const cJSON *map) {
    printf("GAME_CONFIG / NETWORK_DATA sync...\n");
    Game server, client;
    game_on(&server, map);
    game_on(&client, map);
    cJSON *cfg = cJSON_Parse("{\"gameTime\":13,\"nameTeam1\":\"Reds\",\"classes\":[2,3],\"weps\":{\"1\":{\"ammo\":77}},"
                             "\"cls\":{\"2\":{\"camType\":1,\"speedX\":0}},\"prks\":{\"1\":{\"timer\":99}},\"padding\":\""
                             "0123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789"
                             "0123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789"
                             "0123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789"
                             "0123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789"
                             "0123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789"
                             "\"}");
    game_config_apply(&server, cfg, GAME_CONFIG_CLAMP_HOSTED, GAME_HOST_CUSTOM);
    cJSON_Delete(cfg);
    g_sent_count = 0;
    game_config_send(&server, 5);
    int chunks = 0, applied = 0;
    for (int i = 0; i < g_sent_count; i++) {
        if (g_sent[i].type == NET_PACKET_GAME_CONFIG) {
            chunks++;
            applied += game_config_receive(&client, g_sent[i].payload, g_sent[i].length);
        }
    }
    CHECK(chunks >= 1 && applied == 1, "GAME_CONFIG chunks %d, applied %d", chunks, applied);
    CHECK(client.config.game_time == 13 && !strcmp(client.config.team1_name, "Reds") && client.config.class_list_count == 2, "client settings");
    CHECK(client.weapons[1]->ammo == 77 && class_overrides(&client.classes[2])->cam_type == 1 && client.effects && client.effects[1].timer == 99.0f, "client tables");
    CHECK(client.classes[2].movement.speed_x > 0.0f && client.classes[2].movement.speed_x < 1e-5f, "speedX 0 kept as an explicit 0");
    CHECK(last_sent(NET_PACKET_NETWORK_DATA, 5) != NULL, "NETWORK_DATA follows");

    // an INIT without GAME_CONFIG first (older server): the client plays the defaults
    game_config_on_init(&client); // this INIT had one
    CHECK(client.config.game_time == 13, "kept for the INIT it came with");
    game_config_on_init(&client); // the next INIT had none
    CHECK(client.config.game_time == g_default_game_config.game_time && client.weapons[1] == rules_weapon(1) && !client.effects, "reset without GAME_CONFIG");
    game_off(&server);
    game_off(&client);
}

static const NetMatchVoteStatePacket *votes_to(const int uid) {
    const SentPacket *p = last_sent(NET_PACKET_MATCH_VOTE_STATE, uid);
    return p && p->length == sizeof(NetMatchVoteStatePacket) ? (const NetMatchVoteStatePacket *) p->payload : NULL;
}

static void test_votes(void) {
    printf("endData.mts / maVote / init tally...\n");
    cJSON *maps_json[4];
    const char *names[4] = {"assets/maps/official/10_kanji.json", "assets/maps/official/00_burg.json", "assets/maps/official/12_lumber.json", "assets/maps/official/21_arena.json"};
    for (int i = 0; i < 4; i++) {
        maps_json[i] = load(names[i]);
        CHECK(maps_json[i] != NULL, "%s", names[i]);
        if (!maps_json[i]) return;
    }
    const cJSON **list = malloc(4 * sizeof(cJSON *));
    memcpy(list, maps_json, 4 * sizeof(cJSON *));
    int *modes = malloc(3 * sizeof(int));
    modes[0] = MODE_ID_FFA;
    modes[1] = 2;  // tdm
    modes[2] = 33; // only arena offers it (config.modes [33])
    Game game;
    memset(&game, 0, sizeof(game));
    game_configure(&game, NULL, list, 4, modes, 3, NULL, 0, NULL, 0);
    game.hooks.send = test_send;
    game_init(&game, 0, 0, 1);

    // a public game: genMatchList over each map's config.modes ∩ the rotation, FFA first
    for (int round = 0; round < 20; round++) {
        g_sent_count = 0;
        game.end_timer = 0.0f;
        game_end_round(&game);
        const NetMatchVoteStatePacket *v = votes_to(NET_TARGET_ALL);
        CHECK(v && v->count >= 2 && v->count <= NET_MATCH_PICKS && v->voted == -1, "MATCH_VOTE_STATE to everyone (%d picks)", v ? v->count : -1);
        if (!v) break;
        CHECK(v->entries[0].mode_id == MODE_ID_FFA, "FFA first");
        for (int i = 0; i < v->count; i++) {
            const NetMatchVoteEntry *e = &v->entries[i];
            const int ok_mode = e->mode_id == MODE_ID_FFA || e->mode_id == 2 || (e->mode_id == 33 && e->map_index == 3);
            CHECK(ok_mode && e->map_index >= 0 && e->map_index < 4 && e->map_name[0], "pick %d: mode %d map %d (%s)", i, e->mode_id, e->map_index, e->map_name);
            for (int j = 0; j < i; j++) CHECK(e->mode_id != v->entries[j].mode_id || e->map_index != v->entries[j].map_index, "no duplicate picks");
        }
    }

    // votes: toggle, change, tally (ties go to the first), spectators vote too
    const NetMatchVoteStatePacket *v = votes_to(NET_TARGET_ALL);
    NetMatchVoteStatePacket list_now = *v;
    CHECK(!game_vote_match(&game, 1, 99, 0), "a pick that is not offered is rejected");
    CHECK(game_vote_match(&game, 1, list_now.entries[1].mode_id, list_now.entries[1].map_index), "vote 1 -> pick 1");
    const NetMatchVoteStatePacket *mine = votes_to(1), *others = votes_to(NET_TARGET_EXCEPT(1));
    CHECK(mine && mine->voted == 1 && mine->entries[1].votes == 1, "the voter sees its vote");
    CHECK(others && others->voted == -2 && others->entries[1].votes == 1, "the others keep theirs");
    CHECK(game_vote_match(&game, 1, list_now.entries[1].mode_id, list_now.entries[1].map_index), "the same pick again");
    CHECK(votes_to(1)->voted == -1 && votes_to(1)->entries[1].votes == 0, "withdrawn");
    game_vote_match(&game, 1, list_now.entries[1].mode_id, list_now.entries[1].map_index);
    game_vote_match(&game, 77, list_now.entries[0].mode_id, list_now.entries[0].map_index); // a spectator connection
    int map_index = -1, mode_index = -1;
    CHECK(game_next_match(&game, &map_index, &mode_index), "next match");
    CHECK(map_index == list_now.entries[0].map_index && game.modes[mode_index] == list_now.entries[0].mode_id, "a 1-1 tie goes to the first pick");
    CHECK(!game_vote_match(&game, 1, list_now.entries[0].mode_id, list_now.entries[0].map_index), "no votes after the tally");

    // no votes: random among the picks
    g_sent_count = 0;
    game.end_timer = 0.0f;
    game_end_round(&game);
    list_now = *votes_to(NET_TARGET_ALL);
    game_next_match(&game, &map_index, &mode_index);
    int found = 0;
    for (int i = 0; i < list_now.count; i++) found |= list_now.entries[i].map_index == map_index && list_now.entries[i].mode_id == game.modes[mode_index];
    CHECK(found, "no votes: one of the picks");

    // forceMap / forceMode
    game_force_next_match(&game, 2, 3);
    g_sent_count = 0;
    game.end_timer = 0.0f;
    game_end_round(&game);
    v = votes_to(NET_TARGET_ALL);
    CHECK(v && v->count == 1 && v->entries[0].mode_id == 2 && v->entries[0].map_index == 3, "forced pick");
    game_next_match(&game, &map_index, &mode_index);
    CHECK(map_index == 3 && game.modes[mode_index] == 2, "forced next match");

    // a custom game: random config.modes x config.maps pairs, mode 0 first
    game_config_host(&game, NULL, GAME_HOST_CUSTOM);
    g_sent_count = 0;
    game.end_timer = 0.0f;
    game_end_round(&game);
    v = votes_to(NET_TARGET_ALL);
    CHECK(v && v->count >= 2 && v->entries[0].mode_id == MODE_ID_FFA, "custom picks");
    game.end_timer = 0.0f;

    game_off(&game);
    free((void *) list);
    for (int i = 0; i < 4; i++) cJSON_Delete(maps_json[i]);
}

// The public rotation over Krunker's 24 rotation maps (decisions.md "Public mode rotation").
static int pair_ok(const Game *game, const int map_index, const int mode_id) {
    int listed = 0;
    for (size_t i = 0; i < game->mode_count; i++) listed |= game->modes[i] == mode_id;
    return listed && map_index >= 0 && (size_t) map_index < game->map_count && mode_config(mode_id) &&
           !mode_config(mode_id)->prevent_custom && game_map_offers_mode(game->maps[map_index], mode_id);
}

static const char *map_name_of(const Game *game, const int map_index) {
    const cJSON *name = cJSON_GetObjectItem(game->maps[map_index], "name");
    return cJSON_IsString(name) ? name->valuestring : "?";
}

static void test_rotation(void) {
    printf("public rotation: startup pair, no-vote picks, votes...\n");
    // the objects check: Arena has no flags / objectives, Kanji has them all
    load_default_maps();
    const cJSON *arena = g_maps[21], *kanji = g_maps[10], *lostworld = g_maps[7];
    CHECK(arena && kanji && lostworld, "official maps load");
    if (!arena || !kanji || !lostworld) return;
    CHECK(game_map_offers_mode(kanji, MODE_ID_CTF) && game_map_offers_mode(kanji, MODE_ID_DOM) && game_map_offers_mode(kanji, MODE_ID_POINT),
          "Kanji: ctf / dom / hardpoint");
    CHECK(!game_map_offers_mode(arena, MODE_ID_FFA) && game_map_offers_mode(arena, MODE_ID_CHS), "Arena: config.modes [33] only");
    CHECK(!game_map_offers_mode(lostworld, MODE_ID_KC), "Lostworld does not list Kill Confirmed");
    cJSON *bare = cJSON_Parse("{\"name\":\"bare\",\"objects\":[],\"config\":{\"modes\":[0,2,3,25]}}");
    CHECK(game_map_offers_mode(bare, MODE_ID_FFA) && !game_map_offers_mode(bare, MODE_ID_POINT) && !game_map_offers_mode(bare, MODE_ID_CTF) &&
          !game_map_offers_mode(bare, MODE_ID_DOM), "listed modes without their objects are left out");
    cJSON_Delete(bare);

    Game game;
    memset(&game, 0, sizeof(game));
    const size_t rotation_count = sizeof(g_rotation_modes) / sizeof(g_rotation_modes[0]);
    int *modes = malloc(rotation_count * sizeof(int));
    memcpy(modes, g_rotation_modes, rotation_count * sizeof(int));
    game_configure(&game, NULL, NULL, 0, modes, rotation_count, NULL, 0, NULL, 0); // Krunker's rotation maps
    game.hooks.send = test_send;
    CHECK(game.map_count == 34, "34 rotation maps (%zu)", game.map_count);

    // the first round: a random playable pair, every mode of the rotation comes up
    int seen_mode[MODE_COUNT] = {0}, seen_map[64] = {0}, bad = 0;
    for (int i = 0; i < 2000; i++) {
        int map_index = -1, mode_index = -1;
        if (!game_pick_rotation_pair(&game, &map_index, &mode_index)) {
            bad++;
            continue;
        }
        bad += !pair_ok(&game, map_index, game.modes[mode_index]);
        seen_mode[game.modes[mode_index]] = 1;
        seen_map[map_index] = 1;
    }
    int modes_seen = 0, maps_seen = 0;
    for (int i = 0; i < MODE_COUNT; i++) modes_seen += seen_mode[i];
    for (int i = 0; i < 64; i++) maps_seen += seen_map[i];
    CHECK(!bad, "startup pairs are playable (%d bad)", bad);
    CHECK(modes_seen == (int) rotation_count && maps_seen >= 18, "startup: %d modes, %d maps", modes_seen, maps_seen);
    for (int i = 0; i < 20; i++) {
        game_init(&game, -1, -1, 1);
        CHECK(game.ready && pair_ok(&game, game.current_map_index, game_mode_id(&game)), "game_init(-1, -1): %s on %s",
              mode_name(game_mode_id(&game)), map_name_of(&game, game.current_map_index));
    }

    // round ends nobody votes on: genMatchList's cards, one of them at random
    int count_mode[MODE_COUNT] = {0};
    memset(seen_map, 0, sizeof(seen_map));
    bad = 0;
    const int rounds = 600;
    printf("  20 consecutive no-vote rounds:");
    for (int round = 0; round < rounds; round++) {
        g_sent_count = 0;
        game.end_timer = 0.0f;
        game_end_round(&game);
        const NetMatchVoteStatePacket *v = votes_to(NET_TARGET_ALL);
        CHECK(v && v->count >= 2, "picks offered (%d)", v ? v->count : -1);
        for (int i = 0; v && i < v->count; i++) bad += !pair_ok(&game, v->entries[i].map_index, v->entries[i].mode_id);
        int map_index = -1, mode_index = -1;
        if (!game_next_match(&game, &map_index, &mode_index)) {
            bad++;
            continue;
        }
        const int mode_id = game.modes[mode_index];
        bad += !pair_ok(&game, map_index, mode_id);
        count_mode[mode_id]++;
        seen_map[map_index] = 1;
        if (round < 20) printf("%s(%s, %s)", round ? ", " : " ", mode_config(mode_id)->alias, map_name_of(&game, map_index));
    }
    printf("\n  modes over %d rounds:", rounds);
    modes_seen = maps_seen = 0;
    for (int i = 0; i < MODE_COUNT; i++) {
        if (count_mode[i]) printf(" %s %d", mode_config(i)->alias, count_mode[i]);
        modes_seen += count_mode[i] > 0;
    }
    for (int i = 0; i < 64; i++) maps_seen += seen_map[i];
    printf(", %d maps\n", maps_seen);
    CHECK(!bad, "every offered and played pair is playable (%d bad)", bad);
    CHECK(modes_seen == (int) rotation_count && maps_seen >= 18, "no votes: %d modes, %d maps", modes_seen, maps_seen);
    CHECK(count_mode[MODE_ID_FFA] < rounds * 3 / 4 && count_mode[MODE_ID_FFA] > rounds / 4, "FFA is one card of three (%d / %d)",
          count_mode[MODE_ID_FFA], rounds);

    // votes still win: two for the last card beat one for the first
    for (int round = 0; round < 50; round++) {
        g_sent_count = 0;
        game.end_timer = 0.0f;
        game_end_round(&game);
        const NetMatchVoteStatePacket *v = votes_to(NET_TARGET_ALL);
        if (!v || v->count < 2) {
            CHECK(0, "picks offered");
            break;
        }
        const NetMatchVoteEntry first = v->entries[0], last = v->entries[v->count - 1];
        game_vote_match(&game, 1, first.mode_id, first.map_index);
        game_vote_match(&game, 2, last.mode_id, last.map_index);
        game_vote_match(&game, 3, last.mode_id, last.map_index);
        int map_index = -1, mode_index = -1;
        CHECK(game_next_match(&game, &map_index, &mode_index) && map_index == last.map_index && game.modes[mode_index] == last.mode_id,
              "the most votes win (%d on %d)", last.mode_id, last.map_index);
    }
    game.end_timer = 0.0f;
    const cJSON **maps = game.maps;
    game_off(&game);
    free((void *) maps);
}

static void test_network(const cJSON *map) {
    printf("networkData rates, viewDis...\n");
    Game game;
    game_on(&game, map);
    game_init(&game, 0, 0, 1);
    game_network_apply(&game, 60);
    const NetNetworkDataPacket *net = game_network_data(&game);
    CHECK(net->send_hz == 10 && net->tick_hz == 60 && net->sync_hz == 1 && net->view_distance == 0.0f, "FFA: 10 Hz states");
    const int parkour = game_mode_index_for(&game, 4);
    game_init(&game, 0, parkour, 1);
    game_network_apply(&game, 30);
    CHECK(game_network_data(&game)->send_hz == 5, "Parkour: mode.sendR 5 (%u)", game_network_data(&game)->send_hz);
    const float dis = game.mode->config.view_dis; // Parkour: viewDis 1000 (modes.c)
    CHECK(dis > 0.0f && game_network_data(&game)->view_distance == dis, "view distance from the mode (%g)", dis);
    Player *a = spawn(&game, 1, 0), *b = spawn(&game, 2, 0);
    a->position = (vec3) {0, 0, 0};
    b->position = (vec3) {0, 0, dis * 0.5f};
    CHECK(game_network_relevant(&game, a, b), "inside viewDis");
    b->position.z = dis + 1.0f;
    CHECK(!game_network_relevant(&game, a, b) && game_network_relevant(&game, NULL, b), "past viewDis: only spectators");
    game_off(&game);
}

static void test_account_hook(void) {
    printf("account_info without a hook...\n");
    Game game = {0};
    Player player = {0};
    AccountInfo info;
    CHECK(!game_account_info(&game, &player, &info) && !info.has_account, "no hook: a guest");
}

int main(void) {
    cJSON *sandstorm = load("assets/maps/311_sandstorm.json");
    if (!sandstorm) {
        printf("Failed to load assets/maps/311_sandstorm.json (run from the repository root)\n");
        return 1;
    }
    test_tables();
    test_v7_cases(sandstorm);
    test_validate();
    test_inputs_and_merge();
    test_class_overrides(sandstorm);
    test_map_config(sandstorm);
    test_mimic_pubs(sandstorm);
    test_sync(sandstorm);
    test_votes();
    test_rotation();
    test_network(sandstorm);
    test_account_hook();
    cJSON_Delete(sandstorm);
    if (g_failures) {
        printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("Test OK\n");
    return 0;
}
