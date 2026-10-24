// Krunker v10.0.3 KrunkScript items that KN has the API for (doc/v10/changelog-10.0.3.md):
//   - GAME.CONFIG.getMatch().started is !game.waitTimers: false while any wait timer runs (contTime ones too);
//   - preciseHits AI parts (shared/include/ai_parts.h): the model rig and hit mesh of user asset 46802 (a skinned GLB with
//     Melee / Idle / Move), hits on the posed model with part multipliers, armour, onAIDamage / onAIPartDamage /
//     onAIDeath, glowPart / setNodeVisible / getPartPosition, the "aip" record, ai.solid tris and the playAnim hold;
//   - the 10.0.3 AI wrapper members and AI.spawn options, onObjectDamage / onObjectDestroyed;
//   - AI.spawn's alwaysSync option: the AI is synced to every player whatever its distance, where AI.sync
//     only sends the AIs within GAME_AI_SYNC_RANGE (800) of a player.
// The script runs through ks_game_load on a real Game with the server's API table (ks_game_api_fill),
// so the checks cover the host objects the server runs. Run from the repository root:
// ./<build>/ks_game_v1003_test
#include <ai.h>
#include <ai_parts.h>
#include <ks_game.h>
#include <net_ext.h>
#include <math.h>
#include <world.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;
static int g_checks;

#define CHECK(cond, ...) do { \
    g_checks++; \
    if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_failures++; } \
} while (0)

// ---------------------------------------------------------------- host: script log lines, sent packets

static char g_log[32][160];
static int g_log_count;

static void host_log(void *user, const int is_error, const char *line) {
    (void) user;
    (void) is_error;
    if (g_log_count < 32) snprintf(g_log[g_log_count++], sizeof(g_log[0]), "%s", line ? line : "");
}

static int logged(const char *line) {
    for (int i = 0; i < g_log_count; i++)
        if (!strcmp(g_log[i], line)) return 1;
    return 0;
}

typedef struct {
    int type;
    int uid;
    uint8_t payload[NET_MAX_PAYLOAD];
    uint16_t length;
} SentPacket;

static SentPacket g_sent[512];
static int g_sent_count;

static void test_send(Game *game, const int uid, const int type, const void *payload, const uint16_t length) {
    (void) game;
    if (g_sent_count < 512) {
        g_sent[g_sent_count].type = type;
        g_sent[g_sent_count].uid = uid;
        g_sent[g_sent_count].length = length <= NET_MAX_PAYLOAD ? length : 0;
        if (payload && g_sent[g_sent_count].length) memcpy(g_sent[g_sent_count].payload, payload, g_sent[g_sent_count].length);
        g_sent_count++;
    }
}

static int count_sent(const int type, const int uid) {
    int n = 0;
    for (int i = 0; i < g_sent_count; i++) n += g_sent[i].type == type && g_sent[i].uid == uid;
    return n;
}

// ---------------------------------------------------------------- game setup (as ramp_fix_test.c)

static cJSON *load_map(const char *path) {
    cJSON *map = NULL;
    if (load_json_file(path, &map)) {
        printf("Failed to load %s (run from the repository root)\n", path);
        exit(1);
    }
    return map;
}

static void game_on(Game *game, const cJSON *map, const int mode_id) {
    static const cJSON *maps[1];
    maps[0] = map;
    memset(game, 0, sizeof(*game));
    int *modes = malloc(sizeof(int));
    modes[0] = mode_id;
    game_configure(game, NULL, maps, 1, modes, 1, NULL, 0, NULL, 0);
    game->hooks.send = test_send;
    game_init(game, 0, 0, 1);
}

static Player *add_player(Game *game, const int uid) {
    Player *player = player_init(game);
    player->uid = uid;
    snprintf(player->name, sizeof(player->name), "P%d", uid);
    game_players_add(game, player);
    player_spawn(player);
    player->position = (vec3) {0.0f, 0.0f, 0.0f};
    player->last_position = player->position;
    return player;
}

// The server's KrunkScript host: the shared GAME.* entries (ks_game_api_fill) and the script log.
static KsApiImpl g_api[KS_API_COUNT];

static KsHost server_host(void) {
    memset(g_api, 0, sizeof(g_api));
    ks_game_api_fill(g_api, KS_SIDE_SERVER);
    KsHost host = {0};
    host.side = KS_SIDE_SERVER;
    host.log = host_log;
    host.api = g_api;
    return host;
}

// Loads `src` as the server script of `game` and runs its start().
static void run_script(Game *game, const char *src) {
    ks_game_unload(game);
    static KsHost host; // the KsGame keeps the host (hooks after start)
    host = server_host();
    const int rc = ks_game_load(game, KS_SIDE_SERVER, src, strlen(src), &host, NULL);
    CHECK(rc == 0, "the script compiles and loads (rc %d)", rc);
    if (rc == 0) ks_game_start(game);
}

// ---------------------------------------------------------------- getMatch().started

static const char *k_match_script =
    "public action start() {\n"
    "    obj m = GAME.CONFIG.getMatch();\n"
    "    if (m.started) { GAME.log(\"started\"); } else { GAME.log(\"countdown\"); }\n"
    "}\n";

static void test_match_started(Game *game) {
    printf("Running getMatch().started test...\n");
    // the round countdown: a wait timer that is not contTime counts down before the match
    game->mode->wait_timers[0] = (ModeWaitTimer) {.time = 5.0f};
    game->mode->wait_timer_count = 1;
    g_log_count = 0;
    run_script(game, k_match_script);
    CHECK(g_log_count == 1 && !strcmp(g_log[0], "countdown"), "started is false during the countdown (log: %s)",
          g_log_count ? g_log[0] : "none");

    // v10.0.3 `started: !game.waitTimers`: a contTime timer ("Zone closing in", "race ends in") is a wait timer too
    game->mode->wait_timers[0] = (ModeWaitTimer) {.time = 5.0f, .cont_time = 1};
    game->mode->wait_timer_count = 1;
    g_log_count = 0;
    run_script(game, k_match_script);
    CHECK(g_log_count == 1 && !strcmp(g_log[0], "countdown"), "started is false while a contTime timer runs (log: %s)",
          g_log_count ? g_log[0] : "none");

    // the countdown is over: the match has started
    game->mode->wait_timer_count = 0;
    g_log_count = 0;
    run_script(game, k_match_script);
    CHECK(g_log_count == 1 && !strcmp(g_log[0], "started"), "started is true once the countdown is over (log: %s)",
          g_log_count ? g_log[0] : "none");
}

// ---------------------------------------------------------------- AI.spawn alwaysSync

static const char *k_spawn_script =
    "public action start() {\n"
    "    GAME.AI.spawn(\"1\", \"Synced\", 2000, 0, 0, {alwaysSync: true});\n"
    "    GAME.AI.spawn(\"1\", \"Plain\", 2000, 0, 0, {});\n"
    "}\n";

static GameAI *ai_named(Game *game, const char *name) {
    for (int i = 0; game->ai && i < game->ai->count; i++)
        if (!strcmp(game->ai->ais[i].name, name)) return &game->ai->ais[i];
    return NULL;
}

static void test_always_sync(Game *game) {
    printf("Running AI.spawn alwaysSync test...\n");
    // authority spawn: an AI 2000 units away from the only player (AI.sync's range is 800)
    add_player(game, 2);
    run_script(game, k_spawn_script);
    const GameAI *synced = ai_named(game, "Synced");
    const GameAI *plain = ai_named(game, "Plain");
    CHECK(synced && plain, "both AIs spawned (%d)", game->ai ? game->ai->count : -1);
    CHECK(synced && synced->data.always_sync, "AI.spawn {alwaysSync: true} sets the spawn option");
    CHECK(plain && !plain->data.always_sync, "AI.spawn without the option leaves it off");

    // the sync: as a server (not the in-process client), the far AI goes to the player and the plain one does not
    game->is_local = 0;
    static int server;
    game->server = &server;
    g_sent_count = 0;
    game_ai_sync(game);
    CHECK(count_sent(NET_PACKET_AI_ADD_V7, 2) == 1, "the alwaysSync AI is sent to the far player (%d aai)",
          count_sent(NET_PACKET_AI_ADD_V7, 2));
    CHECK(count_sent(NET_PACKET_AI_SYNC_V7, 2) == 1, "one ai list to the far player (%d)", count_sent(NET_PACKET_AI_SYNC_V7, 2));
    game->is_local = 1;
    game->server = NULL;
}

// ---------------------------------------------------------------- ai.playAnim reps (v10.0.2)

// v10.0.2: AI.playAnim(ai, clip, reps, raw) gives reps to the local copy (mesh.playClip(clip, reps)); the
// script's ai.playAnim(clip, reps) passes it, and reps defaults to 1 when it is not a number.
static const char *k_anim_script =
    "public action start() {\n"
    "    obj[] list = GAME.AI.list();\n"
    "    list[0].playAnim(\"Walk\", 3);\n"
    "    list[1].playAnim(\"Walk\");\n"
    "}\n";

static void test_play_anim_reps(void) {
    printf("Running ai.playAnim reps test...\n");
    cJSON *map = load_map("shared/tests/maps/ramp_fix.json");
    static Game game;
    game_on(&game, map, MODE_ID_FFA);
    game.is_local = 1; // the in-process client reads the list: the local copy carries the clip
    GameAIData d;
    game_ai_data_defaults(&d);
    d.has_anim = 1;
    const vec3 three_at = {0.0f, 0.0f, 0.0f}, one_at = {10.0f, 0.0f, 0.0f};
    game_ai_spawn(&game, 0, 0, "Reps3", three_at, &d);
    game_ai_spawn(&game, 0, 0, "Reps1", one_at, &d);
    run_script(&game, k_anim_script);
    const GameAI *three = ai_named(&game, "Reps3");
    const GameAI *one = ai_named(&game, "Reps1");
    CHECK(three && !strcmp(three->clip, "Walk") && three->clip_reps == 3, "playAnim(\"Walk\", 3): the local clip plays 3 times (%d)",
          three ? three->clip_reps : -1);
    CHECK(one && !strcmp(one->clip, "Walk") && one->clip_reps == 1, "playAnim(\"Walk\"): no reps plays once (%d)", one ? one->clip_reps : -1);
    ks_game_unload(&game);
    cJSON_Delete(map);
}

// ---------------------------------------------------------------- v10.0.3 AI parts (preciseHits)

#define PARTS_ASSET 46802 // assets/user-assets/46802/model.glb: a skinned GLB (Head, Chest, Arm.R ... ) with Melee / Idle / Move

static const char *k_parts_script =
    "public action start() {\n"
    "    obj ai = GAME.AI.spawn(\"46802q\", \"Precise\", 0, 0, 0, {preciseHits: true, health: 1000});\n"
    "    GAME.AI.spawn(\"46802q\", \"Boxed\", 300, 0, 0, {health: 1000});\n"
    "}\n"
    "public action onAIDamage(num sid, str doer, num dmg) { GAME.log(\"dmg \" + toStr(dmg)); }\n"
    "public action onAIPartDamage(num sid, str part, str doer, num dmg) { GAME.log(\"part \" + part + \" \" + doer + \" \" + toStr(dmg)); }\n"
    "public action onAIDeath(num sid, str name, str killer) { GAME.log(\"death \" + name + \" \" + killer); }\n"
    "public action onObjectDamage(num sid, num iface, str doer, num dmg) { GAME.log(\"obj \" + toStr(iface) + \" \" + doer + \" \" + toStr(dmg)); }\n"
    "public action onObjectDestroyed(num sid, num iface, str doer) { GAME.log(\"objgone \" + toStr(iface) + \" \" + doer); }\n"
    "public action update(num delta) {}\n";

// The script calls on the precise AI (AI.list()[0]) of the parts game
static void parts_call(Game *game, const char *body) {
    char src[2048];
    snprintf(src, sizeof(src),
             "public action start() {\n    obj[] list = GAME.AI.list();\n    obj ai = list[0];\n%s\n}\n"
             "public action onAIPartDamage(num sid, str part, str doer, num dmg) { GAME.log(\"part \" + part + \" \" + doer + \" \" + toStr(dmg)); }\n"
             "public action onAIDamage(num sid, str doer, num dmg) { GAME.log(\"dmg \" + toStr(dmg)); }\n"
             "public action onAIDeath(num sid, str name, str killer) { GAME.log(\"death \" + name + \" \" + killer); }\n",
             body);
    g_log_count = 0;
    ks_game_unload(game);
    static KsHost host; // the KsGame keeps the host: the hooks run after this returns
    host = server_host();
    const int rc = ks_game_load(game, KS_SIDE_SERVER, src, strlen(src), &host, NULL);
    CHECK(rc == 0, "the parts script compiles (rc %d): %s", rc, g_log_count ? g_log[0] : "");
    if (rc == 0) ks_game_start(game);
}

// A ray from 60 units in front of `target` straight through it (+x to -x): the hit's fraction and part
static float parts_ray(Game *game, GameAI *ai, const vec3 target, GameAIHitPart *part) {
    const vec3 origin = {target.x + 60.0f, target.y, target.z};
    const vec3 delta = {-120.0f, 0.0f, 0.0f};
    return game_ai_ray_hit(game, ai, origin, delta, part);
}

static void test_ai_parts(void) {
    printf("Running v10.0.3 AI parts test...\n");
    // the model reader (iîìïiìî): the rig, the clips, the hit mesh
    unsigned char *glb = NULL;
    size_t len = 0;
    CHECK(!read_file("assets/user-assets/46802/model.glb", &glb, &len), "the test model is there (run from the repository root)");
    AiPartsModel *model = glb ? ai_parts_model_parse(glb, len) : NULL;
    free(glb);
    CHECK(model && ai_parts_model_node_count(model) == 15, "the rig has the file's 15 nodes (%d)", ai_parts_model_node_count(model));
    CHECK(ai_parts_model_node_find(model, "Head") == 1 && ai_parts_model_node_parent(model, 1) == 6, "byName: Head is node 1, under Chest (6)");
    CHECK(ai_parts_model_has_clip(model, "Melee") && ai_parts_model_has_clip(model, "Idle") && ai_parts_model_has_clip(model, "Move"),
          "the clips Melee / Idle / Move");
    CHECK(ai_parts_model_hit_tris(model) == 196, "the hit mesh has the 196 triangles (%d)", ai_parts_model_hit_tris(model));
    ai_parts_model_free(model);
    const unsigned char not_glb[16] = {'n', 'o', 'p', 'e'};
    CHECK(!ai_parts_model_parse(not_glb, sizeof(not_glb)), "a file that is no GLB gives no model");

    cJSON *map = load_map("shared/tests/maps/ramp_fix.json");
    static Game game;
    game_on(&game, map, MODE_ID_FFA);
    game.is_local = 1;
    Player *shooter = add_player(&game, 2);
    run_script(&game, k_parts_script);
    GameAI *ai = ai_named(&game, "Precise");
    GameAI *boxed = ai_named(&game, "Boxed");
    CHECK(ai && ai->data.precise && ai->data.has_anim && ai->data.asset == PARTS_ASSET, "AI.spawn preciseHits: a precise animated AI of asset 46802");
    CHECK(boxed && !boxed->data.precise, "without preciseHits the AI is not precise");
    if (!ai || !boxed) {
        ks_game_unload(&game);
        cJSON_Delete(map);
        return;
    }
    // getPartPosition: the Head's mesh centre in the world (the model scale 4 * the file's units: well above the AI's box)
    vec3 head;
    CHECK(ai_parts_part_position(&game, ai, "Head", &head) && fabsf(head.x) < 0.01f && fabsf(head.y - 35.98f) < 0.05f,
          "partPosition(Head) is the posed head (%.2f %.2f %.2f)", head.x, head.y, head.z);
    vec3 none;
    CHECK(!ai_parts_part_position(&game, ai, "NoSuchPart", &none), "partPosition of no such node: none");
    // usesHitTest: the model is hit where its box is not (the head is above the box top), and missed where only the box is
    GameAIHitPart part;
    const vec3 origin_head = {head.x + 60.0f, head.y, head.z};
    const vec3 across = {-120.0f, 0.0f, 0.0f};
    CHECK(!(game_ai_line_hit(ai, origin_head, across) > 0.0f), "the box does not reach the head");
    float t = parts_ray(&game, ai, head, &part);
    CHECK(t > 0.0f && t < 1.0f && part.has_part && part.mult == 1.0f, "a ray through the head hits the model (t %.3f, %s x%.2f)", t,
          part.name, part.mult);
    char head_part[AI_PART_NAME_SIZE];
    snprintf(head_part, sizeof(head_part), "%s", part.name);
    const vec3 beside = {ai->position.x, ai->position.y + 1.0f, ai->position.z + 7.0f};
    const vec3 origin_beside = {beside.x + 60.0f, beside.y, beside.z};
    CHECK(game_ai_line_hit(ai, origin_beside, across) > 0.0f, "the box reaches 7 units beside the feet");
    t = parts_ray(&game, ai, beside, &part);
    CHECK(!(t > 0.0f && t <= 1.0f), "the precise AI is not hit where only its box is (t %.3f)", t);
    // the box AI: a box hit, no part
    const vec3 box_mid = {boxed->position.x, boxed->position.y + boxed->height, boxed->position.z};
    t = parts_ray(&game, boxed, box_mid, &part);
    CHECK(t > 0.0f && t < 1.0f && !part.has_part, "the box AI is hit on its box (t %.3f)", t);

    // setPartDamage / bodyDamage (clamped 0..20): partMult walks up from the part; onAIDamage then onAIPartDamage
    char call[512];
    snprintf(call, sizeof(call), "    ai.setPartDamage(\"%s\", 2.5);\n    ai.bodyDamage = 30;\n    GAME.log(\"body \" + toStr(ai.bodyDamage));", head_part);
    parts_call(&game, call);
    CHECK(logged("body 20"), "bodyDamage is clamped to 20 (log %s)", g_log_count ? g_log[0] : "none");
    t = parts_ray(&game, ai, head, &part);
    CHECK(part.has_part && part.mult == 2.5f, "the head part's multiplier is its setPartDamage (%.2f)", part.mult);
    const float before = ai->health;
    g_log_count = 0;
    CHECK(!game_ai_damage_part(&game, ai, shooter, 10.0f, 0, NAN, -1, 0, &part), "a part hit is no armour hit");
    CHECK(fabsf(before - ai->health - 25.0f) < 0.01f, "the damage is dmg * part.m (%.1f)", before - ai->health);
    char expect[96];
    snprintf(expect, sizeof(expect), "part %s 2 25", head_part);
    CHECK(g_log_count == 2 && !strcmp(g_log[0], "dmg 25") && !strcmp(g_log[1], expect), "onAIDamage then onAIPartDamage (%s / %s)",
          g_log_count > 0 ? g_log[0] : "", g_log_count > 1 ? g_log[1] : "");
    // armour: part.m 0 -> onAIPartDamage(sid, part, id, 0), no health change, the bullet stops
    snprintf(call, sizeof(call), "    ai.setPartDamage(\"%s\", 0);\n    ai.bodyDamage = 0.5;", head_part);
    parts_call(&game, call);
    t = parts_ray(&game, ai, head, &part);
    const float armoured = ai->health;
    g_log_count = 0;
    CHECK(part.mult == 0.0f && game_ai_damage_part(&game, ai, shooter, 10.0f, 0, NAN, -1, 0, &part), "an armour part is an armour hit");
    snprintf(expect, sizeof(expect), "part %s 2 0", head_part);
    CHECK(ai->health == armoured && g_log_count == 1 && !strcmp(g_log[0], expect), "armour: no damage, onAIPartDamage with 0 (%s)",
          g_log_count ? g_log[0] : "none");
    // a part without its own multiplier gets bodyDamage (a ray through the legs)
    const vec3 legs = {ai->position.x, ai->position.y + 3.0f, ai->position.z};
    t = parts_ray(&game, ai, legs, &part);
    CHECK(t > 0.0f && part.has_part && part.mult == 0.5f, "a part without setPartDamage gets bodyDamage (%s x%.2f)", part.name, part.mult);

    // setNodeVisible: a hidden node (and what is under it) is no longer hit; glowPart; the "aip" record
    snprintf(call, sizeof(call),
             "    ai.setNodeVisible(\"%s\", false);\n    ai.glowPart(\"Head\", \"#ff0000\", 0.456);\n    ai.glowPart(\"Chest\", \"bad\", 3);\n"
             "    ai.glowPart(\"bad name!\", \"#fff\", 1);\n    obj p = ai.getPartPosition(\"Head\");\n    GAME.log(\"pos \" + toStr(p.y));\n"
             "    obj q = ai.getPartPosition(\"nope\");\n    GAME.log(\"none \" + toStr(q.y));",
             head_part);
    parts_call(&game, call);
    snprintf(expect, sizeof(expect), "pos %.2f", roundf(head.y * 100.0f) / 100.0f);
    CHECK(logged("none undefined"), "getPartPosition of no part: {}");
    t = parts_ray(&game, ai, head, &part);
    CHECK(!(part.has_part && !strcmp(part.name, head_part)), "the hidden part is not hit (hit %s)", part.has_part ? part.name : "none");
    AiParts *parts = ai_parts_of(&game, ai, 0);
    CHECK(parts && parts->glow_count == 2 && !strcmp(parts->glow[0].color, "#ff0000") && parts->glow[0].intensity == 0.46 &&
          !parts->glow[1].color[0] && parts->glow[1].intensity == 1.0, "glowPart: color / intensity validated, toFixed(2), clamped");
    CHECK(parts && parts->hidden_count == 1 && parts->dirty, "setNodeVisible(false) hides the node; partsDirty");
    char *json = ai_parts_record_json(&game, ai);
    char want[256];
    snprintf(want, sizeof(want), "[%d,{\"Head\":[\"#ff0000\",0.46],\"Chest\":[\"\",1]},[\"%s\"],null,null]", ai->sid, head_part);
    CHECK(json && !strcmp(json, want), "the aip record: %s", json ? json : "none");
    free(json);
    // the sync sends it to every viewer the AI was sent to
    game.is_local = 0;
    static int server;
    game.server = &server;
    ai->sent_count = 0;
    ai->sent_to[ai->sent_count++] = shooter->uid;
    g_sent_count = 0;
    game_ai_sync(&game);
    int aip = 0;
    for (int i = 0; i < g_sent_count; i++) {
        uint16_t subtype, blen;
        const uint8_t *body;
        if (g_sent[i].type == NET_PACKET_EXT && net_ext_unpack(g_sent[i].payload, g_sent[i].length, &subtype, &body, &blen) &&
            subtype == AI_PARTS_EXT_SUBTYPE && g_sent[i].uid == shooter->uid) aip++;
    }
    CHECK(aip == 1 && !parts->dirty, "a partsDirty AI sends one aip to its viewer (%d)", aip);
    game.is_local = 1;
    game.server = NULL;
    // the mirror (AI.syncParts): a client game with the same AI applies the record
    static Game mirror;
    game_on(&mirror, map, MODE_ID_FFA);
    mirror.is_local = 0;
    mirror.server = NULL; // a client's mirror (game_is_authority: is_local || server)
    CHECK(!game_is_authority(&mirror), "the mirror is no authority");
    GameAIData d;
    game_ai_data_defaults(&d);
    game_ai_data_set_asset(&d, "46802q");
    GameAI *copy = game_ai_spawn(&mirror, ai->sid, 0, "Precise", ai->position, &d);
    json = ai_parts_record_json(&game, ai);
    if (copy && json) ai_parts_apply_json(&mirror, json, strlen(json));
    free(json);
    AiParts *mp = copy ? ai_parts_of(&mirror, copy, 0) : NULL;
    CHECK(mp && mp->glow_count == 2 && mp->hidden_count == 1 && !strcmp(mp->hidden[0], head_part), "syncParts: the mirror has the glows and hidden nodes");

    // playAnim hold: the clip stays on its last frame (clipState.hold) and is the heldClip
    parts_call(&game, "    ai.setNodeVisible(\"Chest\", true);\n    ai.playAnim(\"Melee\", 1, true);");
    CHECK(parts->held_clip[0] && !strcmp(parts->held_clip, "Melee") && parts->dirty, "playAnim(clip, reps, true): heldClip, partsDirty");
    char clip[GAME_AI_ANIM_SIZE];
    double clip_t = 0;
    const float melee = 0.0f;
    (void) melee;
    game.now += 30.0f; // long past the clip's end
    CHECK(ai_parts_clip_at(&game, ai, game.now * 1000.0, clip, sizeof(clip), &clip_t) && !strcmp(clip, "Melee") && clip_t > 0.1,
          "a held clip poses its last frame after it ends (%s %.3f)", clip, clip_t);
    parts_call(&game, "    ai.playAnim(\"Melee\", 1);");
    CHECK(!parts->held_clip[0], "playAnim without hold clears heldClip");
    game.now += 30.0f;
    CHECK(ai_parts_clip_at(&game, ai, game.now * 1000.0, clip, sizeof(clip), &clip_t) && !strcmp(clip, "Idle"), "an ended clip goes back to Idle (%s)", clip);

    // ai.solid (precise only): the pose's collision tris, gone again with solid = false; never on a box AI
    parts_call(&game, "    ai.solid = true;\n    GAME.log(\"solid \" + toStr(ai.solid));\n    obj[] l = GAME.AI.list();\n    l[1].solid = true;\n"
                      "    GAME.log(\"boxsolid \" + toStr(l[1].solid));");
    size_t tri_count = 0;
    int solid_entries = 0;
    for (int i = 0; ai_parts_solid_tris(&game, i, &tri_count); i++) solid_entries += tri_count > 0;
    CHECK(logged("solid true") && logged("boxsolid false") && solid_entries == 1, "ai.solid: one solid AI with tris (%d)", solid_entries);
    tri_count = 0;
    for (int i = 0; ai_parts_solid_tris(&game, i, &tri_count) && !tri_count; i++) {}
    CHECK(tri_count > 0 && tri_count <= 6000, "the solid AI's tris (%zu)", tri_count);
    // a player walking into the solid AI is pushed out of it (world_player_tris)
    Player *walker = add_player(&game, 3);
    walker->position = (vec3) {ai->position.x + 1.0f, ai->position.y + 10.0f, ai->position.z};
    const vec3 inside = walker->position;
    world_player_tris(walker);
    CHECK(walker->position.x != inside.x || walker->position.y != inside.y || walker->position.z != inside.z, "the solid AI pushes a player out");
    parts_call(&game, "    ai.solid = false;");
    tri_count = 0;
    int any = 0;
    for (int i = 0; ai_parts_solid_tris(&game, i, &tri_count); i++) any += tri_count > 0;
    CHECK(!any, "ai.solid = false drops the tris");

    // onAIDeath(sid, name, killerId) after the kill
    g_log_count = 0;
    parts_call(&game, "    GAME.log(\"armed\");");
    g_log_count = 0;
    game_ai_kill(&game, ai, shooter);
    CHECK(logged("death Precise 2"), "onAIDeath(sid, name, killer) (%s)", g_log_count ? g_log[0] : "none");

    ks_game_unload(&mirror);
    game_ai_free(&mirror);
    ks_game_unload(&game);
    cJSON_Delete(map);
    (void) shooter;
}

// ---------------------------------------------------------------- v10.0.3 AI wrapper, AI.spawn options, object hooks

static const char *k_wrapper_script =
    "public action start() {\n"
    "    obj a = GAME.AI.spawn(\"1\", \"Opts\", 0, 0, 0, {team: 3, speed: 5, turnSpeed: \"2\", hitBoxH: 2, modelOffsetY: 500,\n"
    "        modelRotation: 90, animStart: \"Run\", shotEffect: 12, chaseDis: 9999, targPlr: true});\n"
    "    obj b = GAME.AI.spawn(\"1\", \"Bare\", 50, 0, 0, {});\n"
    "    GAME.log(\"max \" + toStr(a.maxHealth) + \" \" + toStr(b.maxHealth));\n"
    "    GAME.log(\"speed \" + toStr(a.speed) + \" \" + toStr(a.turnSpeed) + \" team \" + toStr(a.team));\n"
    "    a.speed = 50;\n    a.team = 7;\n"
    "    GAME.log(\"set \" + toStr(a.speed) + \" \" + toStr(a.team) + \" auto \" + toStr(a.autoTarget));\n"
    "    a.target = \"2\";\n"
    "    GAME.log(\"target \" + a.target + \" auto \" + toStr(a.autoTarget) + \" active \" + toStr(a.active));\n"
    "    a.moveTo(10, 0, 20);\n"
    "    GAME.log(\"behav \" + toStr(a.behaviour));\n"
    "    a.behaviour = 0;\n"
    "    b.moveTo(\"x\", 0, 0);\n"
    "}\n"
    "public action onObjectDamage(num sid, num iface, str doer, num dmg) { GAME.log(\"obj \" + toStr(iface) + \" \" + doer + \" \" + toStr(dmg)); }\n"
    "public action onObjectDestroyed(num sid, num iface, str doer) { GAME.log(\"objgone \" + toStr(iface) + \" \" + doer); }\n";

static void test_ai_wrapper(void) {
    printf("Running v10.0.3 AI wrapper / AI.spawn options / object hooks test...\n");
    cJSON *map = load_map("shared/tests/maps/ramp_fix.json");
    static Game game;
    game_on(&game, map, MODE_ID_FFA);
    game.is_local = 1;
    Player *p = add_player(&game, 2);
    run_script(&game, k_wrapper_script);
    GameAI *a = ai_named(&game, "Opts"), *b = ai_named(&game, "Bare");
    CHECK(a && b, "both AIs spawned");
    if (!a || !b) return;
    // AI.spawn: health 0 when absent (maxHealth = health || 0), numbers only, the clamps
    CHECK(logged("max 0 0"), "no health option: maxHealth 0 (%s)", g_log_count ? g_log[0] : "none");
    CHECK(logged("speed 5 0 team 3"), "speed 5, a string turnSpeed is 0, team 3 (%s)", g_log_count > 1 ? g_log[1] : "none");
    CHECK(a->data.hbh == 2.0f && a->data.m_y_off == 100.0f && a->data.m_r_off == 90.0f && !strcmp(a->data.start_anim, "Run") &&
          a->data.effect_on_shot == 9 && a->data.chase_distance == 2000.0f && a->data.targets_players,
          "the option clamps (hitBoxH 2, modelOffsetY 100, modelRotation 90, animStart, shotEffect 9, chaseDis 2000, targPlr)");
    CHECK(b->data.m_size == 0.0f && b->data.hbh == 1.0f && b->data.effect_on_shot == -1 && b->data.fire_rate == 200 && b->data.vision == 1,
          "without options: mSize || 4, hitBoxH 1, no shot effect, fireRate 200 and visionDis 1 (the clamps of 0)");
    CHECK(logged("set 10 7 auto true"), "speed is clamped to 10, team set, autoTarget by default (%s)", g_log_count > 2 ? g_log[2] : "none");
    CHECK(logged("target 2 auto false active true"), "target = id: scriptTarget, the player (%s)", g_log_count > 3 ? g_log[3] : "none");
    CHECK(a->script_target && a->target.kind == AI_TARGET_PLAYER && a->target.id == p->uid, "the target is the player");
    CHECK(a->has_goal && a->goal.x == 10.0f && a->goal.z == 20.0f && !b->has_goal, "moveTo needs three numbers");
    CHECK(logged("behav undefined") && a->has_script_behavior && a->script_behavior == 0.0f, "behaviour: undefined until set, then ai.behavior");
    // behaviour 0 freezes the server update: the AI does not move towards its goal
    const vec3 at = a->position;
    game_ai_tick(&game, 100.0f);
    CHECK(a->position.x == at.x && a->position.z == at.z, "behaviour 0: no update");

    // onObjectDamage(sid, interface || 0, playerId, dmg) for a destructible, onObjectDestroyed when it breaks
    Object *o = game.map && game.map->object_count ? game.map->objects[0] : NULL;
    CHECK(o != NULL, "the map has an object");
    if (o) {
        o->health = 30.0f;
        o->interface_id = 4;
        g_log_count = 0;
        game_object_hit(&game, o, p, 10.0f, 0, GAME_HIT_SHOT);
        CHECK(logged("obj 4 2 10"), "onObjectDamage (%s)", g_log_count ? g_log[0] : "none");
        g_log_count = 0;
        game_object_hit(&game, o, p, 25.0f, 0, GAME_HIT_SHOT);
        CHECK(logged("obj 4 2 25") && logged("objgone 4 2"), "onObjectDestroyed after the last damage (%d lines)", g_log_count);
        // obj3DServ: interface, health (startHealth objects, 1..1e6), destroyed, toggleDestructible (deob-10.0.3.js:208959-209045)
        o->start_health = 30.0f;
        run_script(&game, "public action start() {\n    obj[] l = GAME.OBJECTS.getByInterface(4);\n    obj o = l[0];\n"
                          "    GAME.log(\"o \" + toStr(o.interface) + \" \" + toStr(o.destroyed));\n    o.toggleDestructible();\n"
                          "    GAME.log(\"t \" + toStr(o.destroyed) + \" \" + toStr(o.health));\n    o.health = 5000000;\n"
                          "    GAME.log(\"h \" + toStr(o.health));\n    o.toggleDestructible();\n    GAME.log(\"d \" + toStr(o.destroyed));\n}\n");
        CHECK(logged("o 4 true") && logged("t false 30") && logged("h 1000000") && logged("d true"),
              "interface / destroyed / toggleDestructible / health clamp (%s | %s | %s | %s)", g_log_count > 0 ? g_log[0] : "",
              g_log_count > 1 ? g_log[1] : "", g_log_count > 2 ? g_log[2] : "", g_log_count > 3 ? g_log[3] : "");
    }
    // player.godMode / speed / aiKills (deob-10.0.3.js:207846-207866, :208133)
    p->mode.ai_kills = 3;
    g_sent_count = 0;
    run_script(&game, "public action start() {\n    obj[] l = GAME.PLAYERS.list();\n    obj me = l[0];\n"
                      "    GAME.log(\"p \" + toStr(me.godMode) + \" \" + toStr(me.speed) + \" \" + toStr(me.aiKills));\n"
                      "    me.godMode = true;\n    me.speed = 50;\n    GAME.log(\"q \" + toStr(me.godMode) + \" \" + toStr(me.speed));\n}\n");
    CHECK(logged("p false 1 3") && logged("q true 10") && p->god_mode && p->speed_boost == 10.0f, "godMode, speed (speedBoost || 1, 0.1..10), aiKills");
    CHECK(count_sent(NET_PACKET_EXT, p->uid) == 1, "speed sends the player its speedBoost (spb)");
    ks_game_unload(&game);
    cJSON_Delete(map);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    cJSON *map = load_map("shared/tests/maps/ramp_fix.json");
    static Game game;
    game_on(&game, map, MODE_ID_FFA);
    CHECK(game.ready && game.mode, "the test game is ready with a mode");

    test_match_started(&game);
    test_always_sync(&game);
    test_play_anim_reps();
    test_ai_parts();
    test_ai_wrapper();

    ks_game_unload(&game);
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
