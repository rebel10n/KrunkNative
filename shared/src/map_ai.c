// WP-F map bots: v7.1.4 MapManager.buildBotData / addBot / addPendingBots (MapManager.js:3285-3370)
// and trigger action 69 "Spawn Bot at Node" (triggers.js:1540-1590), on top of the bot players of
// shared/src/bots.c (BotManager.add(spawnPoint, true, true, data) -> game_bots_add with the map's
// key=value overrides). The map AI half of shared/include/map_ai.h is in shared/src/ai.c.
//
// Intentional differences:
// - v7 adds map bots when the game is a sandbox, a dev build or a public game (game.js:3494); KN adds
//   them on every authority game (server and offline), like the other map content.
// - bots.js turns the spawn "dir" into xDire = -((dir || 0) + 1) * PI / 2, but addBot already passes
//   d * PI / 2 (radians) as dir: fixed, the bot faces -(d + 1) * PI / 2 like a spawn point with d.
// - "btrs" (randomSpawn) has no v7 consumer; KN honours it: the bot (re)spawns at a map spawn point
//   instead of the BOT_SPAWNER.
// - Cosmetics -1 mean "none" in v7 (hat / dye defaults are only for absent keys); KN's bot options
//   use -1 for "not given", so such a bot rolls the manager's default identity instead.
// - The bots use the manager's bot type (NextBot unless bot_type tfue), with the map's overrides.
#include <map_ai.h>
#include <ai.h>
#include <bots.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define MAP_BOT_ARGS 48
#define MAP_BOT_ARG_SIZE 48

typedef struct {
    char buf[MAP_BOT_ARGS][MAP_BOT_ARG_SIZE];
    char *argv[MAP_BOT_ARGS];
    int argc;
} MapBotArgs;

static void arg_add(MapBotArgs *a, const char *key, const char *value) {
    if (a->argc >= MAP_BOT_ARGS) return;
    snprintf(a->buf[a->argc], MAP_BOT_ARG_SIZE, "%s=%s", key, value);
    a->argv[a->argc] = a->buf[a->argc];
    a->argc++;
}

static void arg_bool(MapBotArgs *a, const char *key, const int on) { arg_add(a, key, on ? "on" : "off"); }

static void arg_num(MapBotArgs *a, const char *key, const double value) {
    char text[32];
    snprintf(text, sizeof(text), "%g", value);
    arg_add(a, key, text);
}

static double bot_json_number(const cJSON *object, const char *key, const int integer) { // parseInt / parseFloat || 0
    const cJSON *item = cJSON_GetObjectItem(object, key);
    double v = NAN;
    if (cJSON_IsNumber(item)) v = cJSON_GetNumberValue(item);
    else if (cJSON_IsString(item)) v = strtod(cJSON_GetStringValue(item), NULL);
    if (v != v || isinf(v)) return 0.0;
    return integer ? trunc(v) : v;
}

static int bot_has(const cJSON *object, const char *key) { // key != null
    const cJSON *item = cJSON_GetObjectItem(object, key);
    return item && !cJSON_IsNull(item);
}

// addBot's name check: <= 16 characters of the aiName charset, else "".
static int bot_name_ok(const char *name) {
    if (!name || !name[0] || strlen(name) > 16) return 0;
    for (const unsigned char *c = (const unsigned char *) name; *c; c++) {
        if (!((*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c && strchr("_:;()?,.|[] -+*$@=^!{}", *c)))) return 0;
    }
    return 1;
}

// MapManager.buildBotData(obj) as bot_* key=value overrides (shared/src/bots.c BOT_CONFIG_TYPES).
static void bot_data_args(const cJSON *o, MapBotArgs *a) {
    memset(a, 0, sizeof(*a));
    const cJSON *btn = cJSON_GetObjectItem(o, "btn");
    if (cJSON_IsString(btn) && bot_name_ok(cJSON_GetStringValue(btn))) arg_add(a, "name", cJSON_GetStringValue(btn));
    const int class_index = (int) bot_json_number(o, "btc", 1);
    if (bot_has(o, "btfc")) arg_num(a, "class", class_index); // class: v7 only keeps it with forceClass
    arg_bool(a, "forceClass", bot_has(o, "btfc"));
    arg_num(a, "lives", bot_json_number(o, "btl", 1));
    arg_num(a, "team", bot_json_number(o, "btt", 1));
    const double hm = bot_json_number(o, "bthm", 0), dm = bot_json_number(o, "btdm", 0), spm = bot_json_number(o, "btspm", 0);
    const double rm = bot_json_number(o, "btrm", 0), sm = bot_json_number(o, "btsm", 0);
    arg_num(a, "healthMlt", hm ? hm : 1.0);
    arg_num(a, "dmgMlt", dm ? dm : 1.0);
    arg_num(a, "speedMlt", spm ? spm : 1.0);
    arg_num(a, "rangeMlt", rm ? rm : 1.0);
    arg_num(a, "spreadMlt", sm ? sm : 1.0);
    arg_bool(a, "targetBots", bot_has(o, "bttc"));
    arg_bool(a, "targetTeam", bot_has(o, "bttt"));
    arg_bool(a, "forceCrouch", bot_has(o, "btfcr"));
    arg_bool(a, "attackTarget", !bot_has(o, "btat"));
    arg_bool(a, "followTarget", !bot_has(o, "btft"));
    arg_bool(a, "watchTarget", !bot_has(o, "btwt"));
    arg_bool(a, "mimicTarget", bot_has(o, "btmt"));
    arg_bool(a, "slideHop", bot_has(o, "btsh"));
    arg_bool(a, "canRespawn", !bot_has(o, "btcr"));
    arg_bool(a, "useSpawnDelay", !bot_has(o, "btsd"));
    arg_bool(a, "removeOnDeath", !bot_has(o, "btrd"));
    arg_bool(a, "nodYes", bot_has(o, "btny"));
    arg_bool(a, "nodNo", bot_has(o, "btno"));
    arg_bool(a, "joinMessage", bot_has(o, "btjm"));
    arg_bool(a, "leaveMessage", bot_has(o, "btlm"));
    arg_bool(a, "voiceChat", !bot_has(o, "btvc"));
    arg_bool(a, "godMode", bot_has(o, "btgm"));
    arg_bool(a, "roam", !bot_has(o, "btr"));
    arg_bool(a, "noRecoil", bot_has(o, "btrc")); // recoil: btrc == null
    static const struct { const char *json, *key; } k_cosmetics[] = {
        {"btcpr", "primSkin"}, {"btcd", "dye"}, {"btcf", "face"}, {"btcht", "hat"}, {"btcbk", "back"},
        {"btcb", "body"}, {"btcw", "waist"}, {"btcm", "melee"},
        // head (btch), wrist (btcwr), shoes (btcs), pet (btcp): no KN slot (bots.c ignores them)
    };
    for (size_t i = 0; i < sizeof(k_cosmetics) / sizeof(k_cosmetics[0]); i++) {
        const int v = (int) bot_json_number(o, k_cosmetics[i].json, 1);
        if (v > 0) arg_num(a, k_cosmetics[i].key, v); // parseInt || -1: -1 = none (see the header)
    }
}

static vec3 bot_position(const cJSON *o) { // map.js p, limit(positionRange).round(2)
    const cJSON *p = cJSON_GetObjectItem(o, "p");
    vec3 v = {0};
    float *f = &v.x;
    for (int k = 0; k < 3; k++) {
        const cJSON *item = cJSON_GetArrayItem(p, k);
        double n = cJSON_IsNumber(item) ? cJSON_GetNumberValue(item) : cJSON_IsString(item) ? strtod(cJSON_GetStringValue(item), NULL) : 0.0;
        if (n != n) n = 0.0;
        n = n < -500000.0 ? -500000.0 : n > 500000.0 ? 500000.0 : n;
        f[k] = (float) (floor(n * 100.0 + 0.5) / 100.0);
    }
    return v;
}

static void map_bots_forget(Game *game) {
    GameAIs *list = game->ai;
    if (!list || !list->map_bot_count) return;
    for (int k = 0; k < list->map_bot_count; k++) {
        for (int i = game_bots_count(game) - 1; i >= 0; i--) {
            if (game->bots->list[i].id == list->map_bot_ids[k]) game_bots_remove(game, &game->bots->list[i]);
        }
    }
    list->map_bot_count = 0;
}

void map_bots_spawn_pending(Game *game) {
    if (!game_is_authority(game)) return;
    map_bots_forget(game); // bots.reset(): last round's (non-persistent) map bots
    if (!game->map || !game->map->bot_spawner_count) return;
    GameAIs *list = game_ais(game);
    if (!list) return;

    for (size_t i = 0; i < game->map->bot_spawner_count && i < GAME_AI_MAP_BOT_MAX; i++) { // config.botLimit
        const cJSON *o = game->map->bot_spawners[i];
        MapBotArgs args;
        bot_data_args(o, &args);
        BotAssignments a;
        bot_parse_assignments(args.argc, args.argv, &a);
        const vec3 at = bot_position(o);
        const float yaw = -((float) bot_json_number(o, "d", 1) + 1.0f) * (float) M_PI / 2.0f;
        GameBot *bot = game_bots_add(game, &a, bot_has(o, "btrs") ? NULL : &at, yaw);
        if (bot && list->map_bot_count < GAME_AI_MAP_BOT_MAX) list->map_bot_ids[list->map_bot_count++] = bot->id;
    }
}

int map_bot_spawn_at_node(Game *game, const int node_id, int count) {
    if (!game_is_authority(game) || !game->map) return 0;
    const Object *node = NULL;
    for (size_t i = 0; i < game->map->path_node_count; i++) { // manager.pathNodes[id]: the last one wins
        if (game->map->path_nodes[i].nid == node_id && game->map->path_nodes[i].object < game->map->object_count) {
            node = game->map->objects[game->map->path_nodes[i].object];
        }
    }
    if (!node) return 0;
    if (count < 1) count = 1;
    int added = 0;
    for (int n = 0; n < count; n++) {
        static const char *const k_uncle_joe[] = {
            "name=Uncle Joe", "forceClass=on", "class=4", "primSkin=5076", "dye=1075", "lives=1", "face=5078", "hat=5077", "back=5079",
            "waist=5080", "targetBots=off", "targetTeam=off", "forceCrouch=off", "attackTarget=on", "followTarget=on",
            "watchTarget=on", "mimicTarget=off", "slideHop=off", "canRespawn=on", "useSpawnDelay=on", "removeOnDeath=on",
            "nodYes=off", "nodNo=off", "joinMessage=off", "leaveMessage=off", "voiceChat=off", "godMode=off", "roam=on",
        };
        char *argv[sizeof(k_uncle_joe) / sizeof(k_uncle_joe[0])];
        for (size_t i = 0; i < sizeof(argv) / sizeof(argv[0]); i++) argv[i] = (char *) k_uncle_joe[i];
        BotAssignments a;
        bot_parse_assignments((int) (sizeof(argv) / sizeof(argv[0])), argv, &a);
        GameBot *bot = game_bots_add(game, &a, &node->position, -(float) M_PI / 2.0f);
        if (!bot) break;
        added++;
        Player *p = bot->player;
        if (p) { // scoreOnDeath; maxHealth = health = 1000, "h" to everyone; giveWeapon(5)
            p->max_health = 1000;
            p->health = 1000.0f;
            const NetHealthPacket packet = {.uid = p->uid, .health = p->health};
            game_send(game, NET_TARGET_ALL, NET_PACKET_HEALTH, &packet, sizeof(packet));
#ifdef EFFECT_STACK_MAX // player_give_weapon arrives with the v7 effects / loadout code (player-viewer WP-B)
            player_give_weapon(p, 5);
#endif
        }
    }
    return added;
}
