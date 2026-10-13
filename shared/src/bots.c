// Krunker v7.1.4 bot players (shared/include/bots.h): BotManager / Bot (src/data/bots.js), the
// NextBot manager and AI (server/NextBotManager.ts, bot/nextbot/*), the dev console's bot_*
// option parsing (server/index.ts) and names.js. Runs on the authority only.
//
// Intentional differences from v7.1.4 (also in doc/console.md):
// - Identity: v7 bots added by bot_add get level 0 and no clan; here every bot rolls a
//   setupLocal-style identity (level, clan tag over level 25, featured / premium chance,
//   cosmeticRarity by level) unless level= / clan= / ... are given, and both managers default
//   to randomCosmetics (v7's tfue default "Gnomies" with store items 3168 / 2428, which do not
//   exist in the v3.1.1 store, would be a nameless naked bot).
// - Cosmetics: v7 fills every slot with a uniform pick under the rarity cap; here each slot is
//   worn with a per-slot chance and lower rarities are likelier, so lobbies look like players.
// - Class: v7 re-rolls an unforced class on every respawn; here a bot keeps its class and
//   switches with a 10% chance per respawn. class= implies forceClass.
// - Bots survive round / map changes (v7 game.init runs bots.reset(), which removes every
//   non-persistent bot); bot_kick removes them.
// - Navigation: KrunkNative has no nav mesh. The NextBot state machine (Idle / Hunt / Attack /
//   Hide, profile reaction / aim focus / dodge / chase timeout / retreat) steers straight at its
//   goal with obstacle probes (jump low walls, side-step high ones, no walking off ledges) and
//   stuck recovery; Hunt roams between spawn points and the last known enemy positions.
// - Parsed but without effect natively: voiceChat (no clip), noRecoil, spreadMlt, rangeMlt
//   (only the AI's engage range), scoreOnDeath, head / pet / shoes / wrist (no such v3.1.1 slots).
#include <bots.h>
#include <ai.h>
#include <map_dialect.h> // v10 setupLocal usePathing
#include <pcg_basic.h>
#include <ctype.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define BOT_PI 3.14159265358979323846f
#define BOT_TWO_PI (BOT_PI * 2.0f)
#define BOT_HALF_PI (BOT_PI * 0.5f)

// bot/nextbot/krunkerConfig.ts / states.ts / body.ts
#define NB_TARGET_REACHED_DISTANCE 56.0f
#define NB_MAX_AWARENESS_DISTANCE 5000.0f
#define NB_VISION_FOV_HALF (BOT_PI / 4.0f)  // VISION_DEFAULT_FOV_DEGREES 90
#define NB_VISION_SCAN_MS 100.0f           // VISION_SCAN_INTERVAL_MS 60 (LOS rays: 100 here)
#define NB_ATTACK_YAW_TOLERANCE 0.45f
#define NB_RETREAT_HEALTH_FRACTION 0.3f
#define NB_RETREAT_DURATION_MS 2500.0f
#define NB_COMBAT_RANGE 350.0f
#define NB_HYSTERESIS_RANGE 100.0f
#define NB_DODGE_RANGE 600.0f
#define NB_ENGAGED_FACE_TOLERANCE 0.6f
#define NB_DODGE_HOLD_MIN_MS 600
#define NB_DODGE_HOLD_MAX_MS 1300
#define NB_JUMP_CHANCE 33
#define NB_ATTACK_CHASE_BASE_MS 2000.0f
#define NB_ATTACK_CHASE_AGGRO_MS 2000.0f
#define NB_ATTACK_CHASE_CROUCH_MS 1000.0f
#define NB_ATTACK_DROP_IGNORE_MS 1000.0f
#define NB_AIM_OFFSET_INTERVAL_MS 350.0f
#define NB_AIM_OFFSET_MAX_RAD 0.12f
#define NB_CLOSE_STOP_DISTANCE 45.0f
#define NB_TURN_RATE 0.012f
#define NB_PATH_TURN_RATE 0.004f

// Movement probes (KrunkNative: no nav mesh)
#define BOT_PROBE_MS 100.0f
#define BOT_PROBE_LENGTH 16.0f
#define BOT_KNEE 2.5f
#define BOT_JUMP_CLEAR 9.5f
#define BOT_GROUND_DROP 45.0f
#define BOT_STUCK_CHECK_MS 500.0f
#define BOT_STUCK_MS 1200.0f
#define BOT_RECOVER_MS 700.0f

// KrunkNative move codes (Input.move_dir, ui_controls.c): 0 forward, 2 right, 4 back, 6 left
#define MOVE_NONE (-1)
#define MOVE_FORWARD 0
#define MOVE_RIGHT 2
#define MOVE_BACK 4
#define MOVE_LEFT 6

// setupLocal clans (NextBotManager's ASCII "THIC" for the full-width one)
static const char *const k_bot_clans[] = {"StaZ", "X", "EMPY", "Sour", "KRM1", "SHOT", "HIZ", "RGB", "HALO", "GLXY", "SWEL", "Oras", "PNTM", "THIC"};
static const int k_setup_local_classes[] = {0, 2, 6, 13}; // setupLocal vA747

// ---------------------------------------------------------------- random

static double bot_rand(void) { return ldexp((double) pcg32_random(), -32); }

static int bot_rand_int(const int lo, const int hi) { // UTILS.randInt (inclusive)
    if (hi <= lo) return lo;
    return lo + (int) pcg32_boundedrand((uint32_t) (hi - lo + 1));
}

static float bot_rand_float(const float lo, const float hi) { return lo + (hi - lo) * (float) bot_rand(); }

static int bot_chance(const double p) { return bot_rand() < p; }

// ---------------------------------------------------------------- math

static float bot_angle_delta(const float from, const float to) { return atan2f(sinf(to - from), cosf(to - from)); }

static float bot_d3d(const vec3 a, const vec3 b) {
    const float x = a.x - b.x, y = a.y - b.y, z = a.z - b.z;
    return sqrtf(x * x + y * y + z * z);
}

static float bot_d2d(const vec3 a, const vec3 b) { return hypotf(a.x - b.x, a.z - b.z); }

// UTILS.getDrs(target, self, yOff): [yaw, pitch] from self to target
static void bot_drs(const vec3 target, const vec3 self, const float y_off, float *yaw, float *pitch) {
    const double p = self.x - target.x, q = self.y + y_off - target.y, u = self.z - target.z;
    *pitch = (float) -atan2(q, sqrt(p * p + u * u));
    *yaw = (float) -atan2(-p, u);
}

static vec3 bot_eye(const Player *p) {
    return (vec3) {p->position.x, p->position.y + p->height - game_constants.camera_height, p->position.z};
}

static vec3 bot_forward(const float yaw) { return (vec3) {-sinf(yaw), 0.0f, -cosf(yaw)}; }

// The move code that walks toward world yaw `move_yaw` while looking at `look_yaw`.
static int bot_move_code(const float look_yaw, const float move_yaw) {
    const float rel = bot_angle_delta(look_yaw, move_yaw);
    int k = (int) lroundf(-rel / (BOT_PI / 4.0f));
    return ((k % 8) + 8) % 8;
}

static float bot_wrap(const float a) {
    float r = fmodf(a, BOT_TWO_PI);
    if (r < 0.0f) r += BOT_TWO_PI;
    return r;
}

// ---------------------------------------------------------------- names

static void bot_name_candidate(char *out, const size_t size) {
    // names.js generateName: f2117(1, 100) >= 20 -> a handle, else colour + separator + noun
    if (bot_rand_int(1, 100) >= 20) {
        snprintf(out, size, "%s", g_bot_name_handles[bot_rand_int(0, g_bot_name_handles_COUNT - 1)]);
        return;
    }
    snprintf(out, size, "%s%s%s", g_bot_name_colors[bot_rand_int(0, g_bot_name_colors_COUNT - 1)],
             g_bot_name_separators[bot_rand_int(0, g_bot_name_separators_COUNT - 1)], g_bot_name_nouns[bot_rand_int(0, g_bot_name_nouns_COUNT - 1)]);
}

void bot_generate_name(char *out, const size_t size) { bot_name_candidate(out, size); }

static int bot_name_taken(const Game *game, const char *name) {
    for (size_t i = 0; i < game->player_count; i++) {
        if (game->players[i] && !strcasecmp(game->players[i]->name, name)) return 1;
    }
    for (int i = 0; game->bots && i < game->bots->count; i++) {
        if (!strcasecmp(game->bots->list[i].name, name)) return 1;
    }
    return 0;
}

// A name nobody in the lobby has (two "Gobsmacked" would give the bots away).
static void bot_unique_name(const Game *game, char *out, const size_t size) {
    for (int tries = 0; tries < 12; tries++) {
        bot_name_candidate(out, size);
        if (!bot_name_taken(game, out)) return;
    }
    char base[PLAYER_NAME_SIZE];
    snprintf(base, sizeof(base), "%s", out);
    for (int n = 2; n < 100; n++) {
        snprintf(out, size, "%.*s%d", (int) (size - 4), base, n);
        if (!bot_name_taken(game, out)) return;
    }
}

// ---------------------------------------------------------------- store (assets/skins.json)

typedef struct {
    int index, type, weapon, rarity;
} BotItem;

static BotItem *g_items;
static int g_item_count;
static int g_items_loaded;

static void bot_items_load(void) {
    if (g_items_loaded) return;
    g_items_loaded = 1;
    char *path = concat(assets_path(), "skins.json");
    unsigned char *buffer = NULL;
    size_t length = 0;
    if (!path || read_file(path, &buffer, &length)) {
        fprintf(stderr, "bots: cannot read %s (bots wear no store items)\n", path ? path : "skins.json");
        free(path);
        return;
    }
    free(path);
    cJSON *json = cJSON_ParseWithLength((const char *) buffer, length);
    free(buffer);
    if (!cJSON_IsArray(json)) {
        cJSON_Delete(json);
        return;
    }
    const int n = cJSON_GetArraySize(json);
    g_items = calloc((size_t) (n > 0 ? n : 1), sizeof(BotItem));
    if (!g_items) {
        cJSON_Delete(json);
        return;
    }
    int index = 0;
    const cJSON *item;
    cJSON_ArrayForEach(item, json) {
        const cJSON *type = cJSON_GetObjectItemCaseSensitive(item, "type");
        const cJSON *weapon = cJSON_GetObjectItemCaseSensitive(item, "weapon");
        const cJSON *rarity = cJSON_GetObjectItemCaseSensitive(item, "rarity");
        g_items[g_item_count++] = (BotItem) {
            .index = index++,
            .type = cJSON_IsNumber(type) ? type->valueint : 0,
            .weapon = cJSON_IsNumber(weapon) ? weapon->valueint : 0,
            .rarity = cJSON_IsNumber(rarity) ? rarity->valueint : 0,
        };
    }
    cJSON_Delete(json);
}

// f2112(weapon, type, classIndex, rarity): a store index of `type` (0 = a weapon skin for
// `weapon` = weapon id + 1) with rarity <= max_rarity (-1 = any), -1 when none. Lower rarities
// are likelier (KrunkNative weighting; v7 picks uniformly).
static int bot_pick_item(const int type, const int weapon, const int max_rarity) {
    static const double k_weight[7] = {10.0, 8.0, 6.0, 4.0, 2.5, 1.2, 0.6};
    bot_items_load();
    double total = 0.0;
    for (int i = 0; i < g_item_count; i++) {
        const BotItem *it = &g_items[i];
        if (it->type != type || (type == 0 && it->weapon != weapon) || (max_rarity >= 0 && it->rarity > max_rarity)) continue;
        total += k_weight[it->rarity < 0 ? 0 : it->rarity > 6 ? 6 : it->rarity];
    }
    if (total <= 0.0) return -1;
    double pick = bot_rand() * total;
    for (int i = 0; i < g_item_count; i++) {
        const BotItem *it = &g_items[i];
        if (it->type != type || (type == 0 && it->weapon != weapon) || (max_rarity >= 0 && it->rarity > max_rarity)) continue;
        pick -= k_weight[it->rarity < 0 ? 0 : it->rarity > 6 ? 6 : it->rarity];
        if (pick <= 0.0) return it->index;
    }
    return -1;
}

// ---------------------------------------------------------------- option keys

typedef enum { KT_BOOL, KT_NUM, KT_STR, KT_DIFF } BotKeyType;
typedef enum { KW_CFG, KW_SPAWN, KW_AUTORESPAWN, KW_TFUE, KW_IGNORE } BotKeyWhere;
typedef enum { KC_U8, KC_F32, KC_I32, KC_STR } BotKeyC;

typedef struct {
    const char *name;
    BotKeyType type;
    BotKeyWhere where;
    BotKeyC c;
    size_t offset;
    size_t size;
} BotKey;

#define K_CB(n, f) {n, KT_BOOL, KW_CFG, KC_U8, offsetof(BotConfig, f), 0}
#define K_CF(n, f) {n, KT_NUM, KW_CFG, KC_F32, offsetof(BotConfig, f), 0}
#define K_CI(n, f) {n, KT_NUM, KW_CFG, KC_I32, offsetof(BotConfig, f), 0}
#define K_SI(n, f) {n, KT_NUM, KW_SPAWN, KC_I32, offsetof(BotSpawnOptions, f), 0}
#define K_SB(n, f) {n, KT_BOOL, KW_SPAWN, KC_I32, offsetof(BotSpawnOptions, f), 0}
#define K_IGN(n) {n, KT_NUM, KW_IGNORE, KC_I32, 0, 0}

// server/index.ts BOT_CONFIG_TYPES (+ NextBot's retreat). body is store type 2, back type 14,
// head type 15 (v7.1.4 slots, WP-F).
static const BotKey k_keys[] = {
    K_CF("accuracy", accuracy),
    K_CB("attackTarget", attack_target),
    {"autoRespawn", KT_BOOL, KW_AUTORESPAWN, KC_U8, 0, 0},
    K_SI("back", back),
    K_SI("body", body),
    K_CB("canRespawn", can_respawn),
    K_SI("class", class_index),
    {"clan", KT_STR, KW_CFG, KC_STR, offsetof(BotConfig, clan), sizeof(((BotConfig *) 0)->clan)},
    K_CI("cosmeticRarity", cosmetic_rarity),
    {"difficulty", KT_DIFF, KW_CFG, KC_F32, offsetof(BotConfig, difficulty), 0},
    K_CF("dmgMlt", dmg_mlt),
    K_SI("dye", dye),
    K_SI("face", face),
    K_CB("featured", featured),
    K_CB("followTarget", follow_target),
    K_SB("forceClass", force_class),
    K_CB("forceCrouch", force_crouch),
    K_CB("godMode", god_mode),
    K_SI("hairColor", hair_color),
    K_SI("hat", hat),
    K_SI("head", head),
    K_CF("healthMlt", health_mlt),
    K_CB("joinMessage", join_message),
    K_CB("leaveMessage", leave_message),
    K_CI("level", level),
    K_SI("lives", lives),
    K_SI("melee", melee),
    K_CB("mimicTarget", mimic_target),
    {"name", KT_STR, KW_SPAWN, KC_STR, offsetof(BotSpawnOptions, name), sizeof(((BotSpawnOptions *) 0)->name)},
    K_CB("noRecoil", no_recoil),
    K_CB("nodNo", nod_no),
    K_CB("nodYes", nod_yes),
    K_SI("pet", pet),
    K_CI("premium", premium),
    K_SB("persistent", persistent),
    K_SI("primSkin", prim_skin),
    K_CB("randomCosmetics", random_cosmetics),
    K_CB("randomJump", random_jump),
    K_CF("rangeMlt", range_mlt),
    K_CB("removeOnDeath", remove_on_death),
    K_CB("retreat", retreat),
    K_CB("roam", roam),
    K_CB("scoreOnDeath", score_on_death),
    K_SI("secondary", secondary),
    K_SI("shoes", shoes),
    K_SI("skinColor", skin_color),
    K_CB("slideHop", slide_hop),
    K_CF("speedMlt", speed_mlt),
    K_CF("spreadMlt", spread_mlt),
    K_CB("targetBots", target_bots),
    K_CF("targetDistance", target_distance),
    K_CB("targetTeam", target_team),
    K_SI("team", team),
    {"tfue", KT_BOOL, KW_TFUE, KC_U8, 0, 0},
    K_CB("usePathing", use_pathing), // v10.0.2 (KrunkNative: also a bot_config key)
    K_CB("useSpawnDelay", use_spawn_delay),
    K_CB("voiceChat", voice_chat),
    K_SI("waist", waist),
    K_CB("watchTarget", watch_target),
    K_SI("wrist", wrist),
};
#define BOT_KEY_COUNT ((int) (sizeof(k_keys) / sizeof(k_keys[0])))
_Static_assert(sizeof(k_keys) / sizeof(k_keys[0]) <= 64, "BotAssignments.set has one bit per key");

// BOT_CONFIG_ALIASES (keys: lowercase without - and _)
static const char *const k_aliases[][2] = {
    {"bhop", "slideHop"},       {"cosmetic", "randomCosmetics"}, {"cosmetics", "randomCosmetics"}, {"crouch", "forceCrouch"},
    {"damage", "dmgMlt"},       {"diff", "difficulty"},          {"skill", "difficulty"},          {"follow", "followTarget"},
    {"god", "godMode"},         {"hp", "healthMlt"},             {"jump", "randomJump"},           {"mimic", "mimicTarget"},
    {"recoil", "noRecoil"},     {"respawn", "canRespawn"},       {"skin", "primSkin"},             {"stick", "persistent"},
    {"targetplayers", "targetTeam"}, {"target", "targetTeam"},   {"targetbots", "targetBots"},     {"watch", "watchTarget"},
};

static int bot_key_index(const char *name) {
    for (int i = 0; i < BOT_KEY_COUNT; i++) {
        if (!strcmp(k_keys[i].name, name)) return i;
    }
    return -1;
}

// canonicalBotConfigKey
static int bot_canonical_key(const char *raw) {
    char compact[64];
    size_t n = 0;
    for (const char *p = raw; *p && n + 1 < sizeof(compact); p++) {
        if (*p != '-' && *p != '_') compact[n++] = (char) tolower((unsigned char) *p);
    }
    compact[n] = 0;
    for (int i = 0; i < BOT_KEY_COUNT; i++) {
        if (!strcasecmp(k_keys[i].name, raw) || !strcasecmp(k_keys[i].name, compact)) return i;
    }
    for (size_t i = 0; i < sizeof(k_aliases) / sizeof(k_aliases[0]); i++) {
        if (!strcmp(k_aliases[i][0], compact)) return bot_key_index(k_aliases[i][1]);
    }
    return -1;
}

static int bot_parse_bool(const char *raw, int *out) {
    static const char *const yes[] = {"on", "1", "true", "yes", "y", "enable", "enabled"};
    static const char *const no[] = {"off", "0", "false", "no", "n", "disable", "disabled"};
    for (size_t i = 0; i < sizeof(yes) / sizeof(yes[0]); i++) {
        if (!strcasecmp(raw, yes[i])) return *out = 1, 1;
        if (!strcasecmp(raw, no[i])) return *out = 0, 1;
    }
    return 0;
}

// Number(raw) finite ("" is 0 in JS)
static int bot_parse_number(const char *raw, double *out) {
    while (isspace((unsigned char) *raw)) raw++;
    if (!*raw) return *out = 0.0, 1;
    char *end = NULL;
    const double v = strtod(raw, &end);
    while (end && isspace((unsigned char) *end)) end++;
    if (!end || *end || !isfinite(v)) return 0;
    *out = v;
    return 1;
}

int bot_parse_difficulty(const char *raw, float *out) {
    if (!raw) return 0;
    static const struct { const char *name; float value; } k_presets[] = {
        {"easy", BOT_DIFFICULTY_EASY}, {"normal", BOT_DIFFICULTY_NORMAL}, {"hard", BOT_DIFFICULTY_HARD}, {"max", BOT_DIFFICULTY_MAX},
    };
    for (size_t i = 0; i < sizeof(k_presets) / sizeof(k_presets[0]); i++) {
        if (!strcasecmp(raw, k_presets[i].name)) return *out = k_presets[i].value, 1;
    }
    double v;
    if (!bot_parse_number(raw, &v) || !(v > 0.0)) return 0;
    *out = (float) v;
    return 1;
}

static void bot_spawn_options_none(BotSpawnOptions *o) {
    memset(o, 0, sizeof(*o));
    o->class_index = o->secondary = o->team = o->lives = o->persistent = o->force_class = -1;
    o->prim_skin = o->hat = o->body = o->waist = o->face = o->dye = o->melee = o->skin_color = o->hair_color = -1;
    o->head = o->back = o->shoes = o->pet = o->wrist = -1;
}

static void bot_assignments_init(BotAssignments *a) {
    memset(a, 0, sizeof(*a));
    bot_spawn_options_none(&a->spawn);
}

static void bot_key_store(BotAssignments *a, const int key, const double number, const char *text) {
    const BotKey *k = &k_keys[key];
    a->set |= 1ull << key;
    if (k->where == KW_IGNORE || k->where == KW_AUTORESPAWN || k->where == KW_TFUE) {
        if (k->where == KW_AUTORESPAWN) a->config.can_respawn = (unsigned char) (number != 0.0);
        if (k->where == KW_TFUE && number != 0.0) {
            a->config.dmg_mlt = 0.01f; // sandbox spawnBot: {name: "Bot", dmgMlt: 0.01}
            a->set |= 1ull << bot_key_index("dmgMlt");
        }
        return;
    }
    unsigned char *base = k->where == KW_CFG ? (unsigned char *) &a->config : (unsigned char *) &a->spawn;
    switch (k->c) {
        case KC_U8: base[k->offset] = (unsigned char) (number != 0.0); break;
        case KC_F32: *(float *) (base + k->offset) = (float) number; break;
        case KC_I32: *(int *) (base + k->offset) = (int) number; break;
        case KC_STR: snprintf((char *) (base + k->offset), k->size, "%s", text ? text : ""); break;
    }
}

// parseBotConfigAssignments (+ the devConsole keywords tfue / autoRespawn / noAutoRespawn /
// useSpawnDelay). The first error is kept in out->error.
void bot_parse_assignments(const int argc, char **argv, BotAssignments *out) {
    bot_assignments_init(out);
    for (int i = 0; i < argc; i++) {
        const char *token = argv[i];
        if (!token || !*token) continue;
        const char *eq = strchr(token, '=');
        if (!eq) {
            double n;
            if (!out->count && bot_parse_number(token, &n) && n == floor(n) && n > 0.0) {
                out->count = n > GAME_BOTS_MAX ? GAME_BOTS_MAX : (int) n; // Math.min(32, count)
                continue;
            }
            if (!strcasecmp(token, "tfue")) { bot_key_store(out, bot_key_index("tfue"), 1.0, NULL); continue; }
            if (!strcasecmp(token, "autorespawn")) { bot_key_store(out, bot_key_index("autoRespawn"), 1.0, NULL); continue; }
            if (!strcasecmp(token, "noautorespawn")) { bot_key_store(out, bot_key_index("autoRespawn"), 0.0, NULL); continue; }
            if (!strcasecmp(token, "usespawndelay")) { bot_key_store(out, bot_key_index("useSpawnDelay"), 1.0, NULL); continue; }
            if (!out->error[0]) snprintf(out->error, sizeof(out->error), "Expected key=value, got '%s'", token);
            continue;
        }
        char key[64];
        snprintf(key, sizeof(key), "%.*s", (int) (eq - token), token);
        const char *value = eq + 1;
        // rawKey / rawValue .trim()
        char *k = key;
        while (isspace((unsigned char) *k)) k++;
        for (size_t n = strlen(k); n && isspace((unsigned char) k[n - 1]); n--) k[n - 1] = 0;
        while (isspace((unsigned char) *value)) value++;
        const int index = bot_canonical_key(k);
        if (index < 0) {
            if (!out->error[0]) snprintf(out->error, sizeof(out->error), "Unknown bot option '%s'", k);
            continue;
        }
        const BotKey *def = &k_keys[index];
        double number = 0.0;
        int ok = 1;
        if (def->type == KT_BOOL) {
            int b = 0;
            ok = bot_parse_bool(value, &b);
            number = b;
        } else if (def->type == KT_DIFF) {
            float d = 0.0f;
            ok = bot_parse_difficulty(value, &d);
            number = d;
        } else if (def->type == KT_NUM) {
            ok = bot_parse_number(value, &number);
        }
        if (!ok) {
            static const char *const type_names[] = {"boolean", "number", "string", "difficulty"};
            if (!out->error[0]) snprintf(out->error, sizeof(out->error), "Invalid %s for '%s'", type_names[def->type], def->name);
            continue;
        }
        bot_key_store(out, index, number, value);
    }
}

// String(value) of a JS number
static void bot_js_number(char *out, const size_t size, const double v) {
    if (v == floor(v) && fabs(v) < 1e15) snprintf(out, size, "%lld", (long long) v);
    else snprintf(out, size, "%.6g", v);
}

static void bot_key_value(const BotAssignments *a, const int key, char *out, const size_t size) {
    const BotKey *k = &k_keys[key];
    if (k->where == KW_AUTORESPAWN) { snprintf(out, size, "%s", a->config.can_respawn ? "true" : "false"); return; }
    if (k->where == KW_TFUE) { snprintf(out, size, "%s", a->config.dmg_mlt > 0.0f && a->config.dmg_mlt <= 0.011f ? "true" : "false"); return; }
    if (k->where == KW_IGNORE) { snprintf(out, size, "-1"); return; }
    const unsigned char *base = k->where == KW_CFG ? (const unsigned char *) &a->config : (const unsigned char *) &a->spawn;
    switch (k->c) {
        case KC_U8: snprintf(out, size, "%s", base[k->offset] ? "true" : "false"); break;
        case KC_F32: bot_js_number(out, size, *(const float *) (base + k->offset)); break;
        case KC_I32:
            if (k->type == KT_BOOL) snprintf(out, size, "%s", *(const int *) (base + k->offset) > 0 ? "true" : "false");
            else if (k->offset == offsetof(BotConfig, cosmetic_rarity) && k->where == KW_CFG && *(const int *) (base + k->offset) < 0) snprintf(out, size, "undefined");
            else bot_js_number(out, size, *(const int *) (base + k->offset));
            break;
        case KC_STR: {
            const char *s = (const char *) (base + k->offset);
            snprintf(out, size, "%s", s[0] ? s : (k->offset == offsetof(BotConfig, clan) && k->where == KW_CFG ? "0" : ""));
            break;
        }
    }
}

// describeBotConfig: "key=value ..." sorted by key, "defaults" when empty. all = every config
// key (the manager's config object), else only the keys the assignments set.
void bot_describe_config(const BotAssignments *a, const int all, char *out, const size_t size) {
    size_t len = 0;
    out[0] = 0;
    for (int i = 0; i < BOT_KEY_COUNT && len < size; i++) {
        const BotKey *k = &k_keys[i];
        if (all ? k->where != KW_CFG : !(a->set & (1ull << i))) continue;
        char value[64];
        bot_key_value(a, i, value, sizeof(value));
        len += (size_t) snprintf(out + len, size - len, "%s%s=%s", len ? " " : "", k->name, value);
    }
    if (!out[0]) snprintf(out, size, "defaults");
}

// botDifficultyConfig(value)
void bot_difficulty_assignments(const float value, BotAssignments *out) {
    bot_assignments_init(out);
    float accuracy = 25.0f, distance = 1200.0f;
    float difficulty = value;
    if (value >= BOT_DIFFICULTY_MAX) {
        difficulty = BOT_DIFFICULTY_MAX;
        accuracy = 100.0f;
        distance = 5000.0f;
        out->config.follow_target = 1;
        out->config.watch_target = 0;
        out->set |= (1ull << bot_key_index("followTarget")) | (1ull << bot_key_index("watchTarget"));
    } else if (value >= BOT_DIFFICULTY_HARD) {
        accuracy = 85.0f;
        distance = 1800.0f;
    } else if (value >= BOT_DIFFICULTY_NORMAL) {
        accuracy = 50.0f;
    }
    out->config.difficulty = difficulty;
    out->config.accuracy = accuracy;
    out->config.attack_target = 1;
    out->config.target_distance = distance;
    out->set |= (1ull << bot_key_index("difficulty")) | (1ull << bot_key_index("accuracy")) | (1ull << bot_key_index("attackTarget")) |
                (1ull << bot_key_index("targetDistance"));
}

// The config keys of `a` over `config`.
static void bot_apply_config(BotConfig *config, const BotAssignments *a) {
    for (int i = 0; i < BOT_KEY_COUNT; i++) {
        if (!(a->set & (1ull << i))) continue;
        const BotKey *k = &k_keys[i];
        if (k->where == KW_AUTORESPAWN) { config->can_respawn = a->config.can_respawn; continue; }
        if (k->where != KW_CFG) continue;
        memcpy((unsigned char *) config + k->offset, (const unsigned char *) &a->config + k->offset,
               k->c == KC_U8 ? 1 : k->c == KC_STR ? k->size : 4);
    }
}

void bot_config_defaults(const BotType type, BotConfig *c) {
    memset(c, 0, sizeof(*c));
    c->attack_target = c->follow_target = c->roam = 1;
    c->can_respawn = c->use_spawn_delay = c->remove_on_death = 1;
    c->voice_chat = 1;
    c->dmg_mlt = c->speed_mlt = c->range_mlt = c->spread_mlt = c->health_mlt = 1.0f;
    c->random_cosmetics = 1; // KrunkNative: both managers (v7 tfue: false)
    c->cosmetic_rarity = -1;
    if (type == BOT_TYPE_TFUE) { // BotManager.config
        c->watch_target = 1;
        c->difficulty = 1.0f;
        c->accuracy = 50.0f;
        c->target_distance = 1200.0f;
    } else { // NextBotManager.config (DEFAULT_AI_CONFIG + manager keys)
        c->watch_target = 0;
        c->retreat = 1;
        c->difficulty = BOT_DIFFICULTY_MAX;
        c->accuracy = 100.0f;
        c->target_distance = NB_MAX_AWARENESS_DISTANCE;
    }
}

const char *bot_type_name(const BotType type) { return type == BOT_TYPE_TFUE ? "tfue" : "nextbot"; }

int bot_parse_type(const char *raw, BotType *out) {
    if (!raw) return 0;
    char v[16];
    size_t n = 0;
    while (isspace((unsigned char) *raw)) raw++;
    for (; *raw && n + 1 < sizeof(v); raw++) v[n++] = (char) tolower((unsigned char) *raw);
    while (n && isspace((unsigned char) v[n - 1])) n--;
    v[n] = 0;
    if (!strcmp(v, "next") || !strcmp(v, "nextbot") || !strcmp(v, "nextbots")) return *out = BOT_TYPE_NEXTBOT, 1;
    if (!strcmp(v, "tfue") || !strcmp(v, "legacy") || !strcmp(v, "old")) return *out = BOT_TYPE_TFUE, 1;
    return 0;
}

const char *bot_state_name(const BotState state) {
    switch (state) {
        case BOT_STATE_HUNT: return "Hunt";
        case BOT_STATE_ATTACK: return "Attack";
        case BOT_STATE_HIDE: return "Hide";
        default: return "Idle";
    }
}

// ---------------------------------------------------------------- manager

GameBots *game_bots(Game *game) {
    if (!game) return NULL;
    if (!game->bots) {
        game->bots = calloc(1, sizeof(GameBots));
        if (!game->bots) return NULL;
        game->bots->next_id = 1;
        game->bots->type = BOT_TYPE_NEXTBOT;
        bot_config_defaults(BOT_TYPE_NEXTBOT, &game->bots->config[BOT_TYPE_NEXTBOT]);
        bot_config_defaults(BOT_TYPE_TFUE, &game->bots->config[BOT_TYPE_TFUE]);
        game->bots->quota_grace = BOT_QUOTA_GRACE_MS;
    }
    return game->bots;
}

int game_bots_count(const Game *game) { return game && game->bots ? game->bots->count : 0; }

GameBot *game_bot_of(Game *game, const Player *player) {
    if (!game || !game->bots || !player) return NULL;
    for (int i = 0; i < game->bots->count; i++) {
        if (game->bots->list[i].player == player) return &game->bots->list[i];
    }
    return NULL;
}

int game_player_is_bot(const Game *game, const Player *player) { return player && player->is_bot && game; }

static int bot_alloc_uid(Game *game) {
    if (game->hooks.alloc_uid) return game->hooks.alloc_uid(game);
    GameBots *bots = game->bots;
    int uid = bots->next_uid > GAME_BOT_UID_BASE ? bots->next_uid : GAME_BOT_UID_BASE;
    for (size_t i = 0; i < game->player_count; i++) {
        if (game->players[i] && game->players[i]->uid >= uid) uid = game->players[i]->uid + 1;
    }
    for (int i = 0; i < bots->count; i++) {
        if (bots->list[i].uid >= uid) uid = bots->list[i].uid + 1;
    }
    bots->next_uid = uid + 1;
    return uid;
}

static int bot_class_allowed(const Game *game, const int index) {
    // NextBotManager.isBotClassAllowed: no Rocketeer (7). KrunkNative: no Runner (9, knife only) either,
    // nor v7.1.4's hidden Survivor (14: its Build Tool never fires outside build modes, WP-B).
    return index >= 0 && (size_t) index < game->class_count && index != 7 && index != 9 && index != CLASS_ID_SURVIVOR &&
           game->classes[index].loadout;
}

static int bot_random_class(const Game *game) {
    int allowed[32], n = 0;
    for (size_t i = 0; i < game->class_count && n < 32; i++) {
        if (bot_class_allowed(game, (int) i)) allowed[n++] = (int) i;
    }
    return n ? allowed[bot_rand_int(0, n - 1)] : 0;
}

// Select Secondary: the Pistol most of the time, else another secondary weapon.
static int bot_random_secondary(const Game *game) {
    int list[32], n = 0;
    for (size_t i = 0; i < game->weapon_count && n < 32; i++) {
        const Weapon *w = game->weapons[i];
        if (w && w->secondary && !w->melee && (int) i != WEAPON_ID_PISTOL) list[n++] = (int) i;
    }
    if (!n || bot_chance(0.55)) return WEAPON_ID_PISTOL;
    return list[bot_rand_int(0, n - 1)];
}

static void bot_roll_weapon_skins(const Game *game, GameBot *bot) {
    const int cap = bot->config.cosmetic_rarity;
    const ClassConfig *class_config = bot->class_index >= 0 && (size_t) bot->class_index < game->class_count ? &game->classes[bot->class_index] : NULL;
    const int primary = class_config && class_config->loadout ? class_config->loadout[0] : -1;
    bot->skins = PLAYER_SKINS_NONE;
    if (!bot->config.random_cosmetics) {
        if (bot->options.prim_skin >= 0) bot->skins.primary = bot->options.prim_skin;
        if (bot->options.melee >= 0) bot->skins.melee = bot->options.melee;
        return;
    }
    if (primary >= 0 && bot_chance(0.7)) bot->skins.primary = bot_pick_item(0, primary + 1, cap);
    if (class_config && class_config->secondary && bot->secondary_index >= 0 && bot_chance(0.4)) bot->skins.secondary = bot_pick_item(0, bot->secondary_index + 1, cap);
    if (bot_chance(0.35)) bot->skins.melee = bot_pick_item(3, 0, cap);
    if (bot->options.prim_skin >= 0) bot->skins.primary = bot->options.prim_skin;
    if (bot->options.melee >= 0) bot->skins.melee = bot->options.melee;
}

static void bot_roll_cosmetics(GameBot *bot) {
    const int cap = bot->config.cosmetic_rarity;
    PlayerCosmetics c = PLAYER_COSMETICS_NONE;
    PlayerCosmeticsV7 v7 = PLAYER_COSMETICS_V7_NONE;
    if (bot->config.random_cosmetics) {
        // v7.1.4 NextBot.randomizeCosmetics rolls every slot; KrunkNative keeps its per-slot chances
        // (a bot wearing all 11 slots looks nothing like a public lobby) and only one of hat / head
        // and body / back, the non-premium rule.
        if (bot_chance(0.35)) c.hat = bot_pick_item(1, 0, cap);
        else if (bot_chance(0.35)) v7.head = bot_pick_item(15, 0, cap);
        if (bot_chance(0.2)) c.body = bot_pick_item(2, 0, cap);
        else if (bot_chance(0.25)) v7.back = bot_pick_item(14, 0, cap);
        if (bot_chance(0.15)) c.waist = bot_pick_item(6, 0, cap);
        if (bot_chance(0.12)) c.face = bot_pick_item(7, 0, cap);
        if (bot_chance(0.3)) c.dye = bot_pick_item(5, 0, cap);
        if (bot_chance(0.3)) v7.shoe = bot_pick_item(8, 0, cap);
        if (bot_chance(0.12)) v7.pet = bot_pick_item(9, 0, cap);
        if (bot_chance(0.3)) v7.player_card = bot_pick_item(16, 0, cap);
    }
    const BotSpawnOptions *o = &bot->options;
    if (o->hat >= 0) c.hat = o->hat;
    if (o->body >= 0) c.body = o->body;
    if (o->head >= 0) v7.head = o->head;
    if (o->back >= 0) v7.back = o->back;
    if (o->shoes >= 0) v7.shoe = o->shoes;
    if (o->pet >= 0) v7.pet = o->pet;
    if (o->wrist >= 0) v7.wrist = o->wrist;
    bot->cosmetics_v7 = v7;
    if (o->waist >= 0) c.waist = o->waist;
    if (o->face >= 0) c.face = o->face;
    if (o->dye >= 0) c.dye = o->dye;
    if (o->skin_color >= 0) c.skin_color = o->skin_color;
    if (o->hair_color >= 0) c.hair_color = o->hair_color;
    bot->cosmetics = c;
}

// The account the scoreboard / nametag shows (NextBot createAccount + setupLocal's roll).
static void bot_roll_identity(GameBot *bot, const BotAssignments *a) {
    const int level_set = a && (a->set & (1ull << bot_key_index("level")));
    int level = bot->config.level;
    if (!level_set || level <= 0) level = bot_chance(0.25) ? bot_rand_int(26, 60) : bot_rand_int(1, 25);
    bot->level = level;
    if (!(a && (a->set & (1ull << bot_key_index("cosmeticRarity")))) && bot->config.cosmetic_rarity < 0) bot->config.cosmetic_rarity = level > 25 ? 6 : 2;
    bot->featured = a && (a->set & (1ull << bot_key_index("featured"))) ? bot->config.featured : level >= 20 && bot_rand_float(0.0f, 100.0f) >= 80.0f;
    bot->premium = a && (a->set & (1ull << bot_key_index("premium"))) ? bot->config.premium : bot_rand_float(0.0f, 100.0f) >= 90.0f;
    if (a && (a->set & (1ull << bot_key_index("clan")))) snprintf(bot->clan, sizeof(bot->clan), "%s", strcmp(bot->config.clan, "0") ? bot->config.clan : "");
    else if (bot->config.clan[0]) memcpy(bot->clan, bot->config.clan, sizeof(bot->clan));
    else snprintf(bot->clan, sizeof(bot->clan), "%s", level > 25 ? k_bot_clans[bot_rand_int(0, (int) (sizeof(k_bot_clans) / sizeof(k_bot_clans[0])) - 1)] : "");
}

static void bot_random_socket_id(char *out) { // UTILS.randomString(12)
    static const char chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    for (int i = 0; i < 12; i++) out[i] = chars[bot_rand_int(0, (int) sizeof(chars) - 2)];
    out[12] = 0;
}

// ---------------------------------------------------------------- packets

static void bot_send_info(Game *game, const GameBot *bot, const int target) {
    NetPlayerInfoPacket info;
    memset(&info, 0, sizeof(info));
    info.uid = bot->uid;
    info.account_id = GAME_BOT_ACCOUNT_BASE + bot->id;
    info.level = bot->level > 0 ? bot->level : 1;
    info.flags = (uint8_t) ((bot->premium ? NET_PLAYER_INFO_PREMIUM : 0) | (bot->featured ? NET_PLAYER_INFO_VERIFIED : 0));
    snprintf(info.clan, sizeof(info.clan), "%s", bot->clan);
    game_send(game, target, NET_PACKET_PLAYER_INFO, &info, sizeof(info));
}

void game_bots_send_infos(Game *game, const int uid) {
    if (!game || !game->bots) return;
    for (int i = 0; i < game->bots->count; i++) {
        if (game->bots->list[i].player) bot_send_info(game, &game->bots->list[i], uid);
    }
}

// What server_bin's net_send_spawn sends a connection: SKINS, then SPAWN (never is_you).
static void bot_broadcast_spawn(Game *game, const Player *p) {
    const NetSkinsPacket skins = {.uid = p->uid, .skins = p->skins};
    game_send(game, NET_TARGET_ALL, NET_PACKET_SKINS, &skins, sizeof(skins));
    NetSpawnPacket packet;
    memset(&packet, 0, sizeof(packet));
    packet.uid = p->uid;
    packet.active = p->active;
    packet.class_index = p->class_index;
    packet.secondary_index = p->secondary_index;
    packet.cosmetics = p->cosmetics;
    packet.attach_index = p->attach_index;
    packet.head_mlt = p->head_mlt > 0.0f ? p->head_mlt : 1.0f;
    packet.cosmetics_v7 = p->cosmetics_v7; // WP-F (bots have no account badge)
    packet.team = p->team;
    packet.health = p->health;
    packet.max_health = p->max_health;
    packet.position[0] = p->position.x;
    packet.position[1] = p->position.y;
    packet.position[2] = p->position.z;
    packet.direction[0] = p->direction.x;
    packet.direction[1] = p->direction.y;
    snprintf(packet.name, sizeof(packet.name), "%s", p->name);
    game_send(game, NET_TARGET_ALL, NET_PACKET_SPAWN, &packet, sizeof(packet));
}

// config.serverMessages[0 / 1] ("$ joined the game" / "$ left the game")
static void bot_server_message(Game *game, const char *name, const int join) {
    char text[NET_CHAT_TEXT_SIZE];
    snprintf(text, sizeof(text), "%s %s the game", name, join ? "joined" : "left");
    NetChatPacket chat;
    net_packet_chat(&chat, -1, NET_CHAT_SERVER, NULL, text);
    game_send(game, NET_TARGET_ALL, NET_PACKET_CHAT, &chat, sizeof(chat));
}

// ---------------------------------------------------------------- spawn / remove

static void bot_reset_ai(GameBot *bot, const Player *p) {
    bot->tick = 0;
    bot->action_freq = 0.0f;
    bot->target_uid = -1;
    bot->yaw = p->direction.y;
    bot->pitch = 0.0f;
    bot->fire_prev = 0;
    bot->has_roam = 0;
    bot->roam_timer = 0.0f;
    bot->sees_target = 0;
    bot->state = BOT_STATE_IDLE;
    bot->state_time = 0.0f;
    bot->reaction = 0.0f;
    bot->not_seen = 0.0f;
    bot->aim_offset_timer = 0.0f;
    bot->aim_offset_yaw = bot->aim_offset_pitch = 0.0f;
    bot->hide_timer = 0.0f;
    bot->ignore_timer = 0.0f;
    bot->ignore_uid = -1;
    bot->stuck_pos = p->position;
    bot->stuck_timer = bot->stuck_check = bot->recover_timer = 0.0f;
    bot->probe_timer = 0.0f;
    bot->probe_move = -99;
    bot->probe_jump = 0;
    bot->start = p->position;
    bot->last_health = p->health;
    bot->path.count = 0;
    bot->path_index = 0;
    bot->path_timer = 0.0f;
    bot->path_stranded = 0;
    bot->route_path = -1; // resetTargeting -> resetPathInfo
    bot->route_node = 0;
    bot->route_dir = 1;
    bot->has_route_target = 0;
    memset(&bot->mimic, 0, sizeof(bot->mimic));
    bot->mimic.move_dir = MOVE_NONE;
}

// Bot.spawn(skipSync): players.add on the first spawn, the loadout / outfit, the multipliers,
// then the SKINS / SPAWN / PLAYER_INFO every client gets for a connection's spawn.
static int bot_spawn(Game *game, GameBot *bot) {
    Player *p = bot->player;
    // spawn(): canRespawn && config.canRespawn gate the respawns (KrunkNative: never the first
    // spawn of a round, so `bot_add noAutoRespawn` still joins)
    if (p && (!bot->can_respawn || !bot->config.can_respawn)) return 0;
    const int first = !p;
    if (!p) {
        p = player_init(game);
        if (!p) return 0;
        p->uid = bot->uid;
        p->is_bot = 1;
        snprintf(p->name, sizeof(p->name), "%s", bot->name);
        const GameModeConfig *mode = game->mode ? &game->mode->config : NULL;
        p->team = mode && mode->teams && bot->team >= 1 && bot->team <= 2 ? bot->team : 0; // validTeam
        bot->player = p;
        game_players_add(game, p); // players.add: setTeam (mode) + lives
    }

    // class: forced, kept, or (first spawn / 10% of respawns) a new random one
    const int class_changed = !first && !(bot->options.force_class > 0) && bot_chance(0.1);
    if (bot->options.force_class > 0 && bot_class_allowed(game, bot->options.class_index)) bot->class_index = bot->options.class_index;
    else if (bot->class_index < 0 || class_changed || !bot_class_allowed(game, bot->class_index)) bot->class_index = bot_random_class(game);
    const ClassConfig *class_config = &game->classes[bot->class_index];
    if (class_config->secondary) {
        const int s = bot->options.secondary;
        if (s >= 0 && (size_t) s < game->weapon_count && game->weapons[s] && game->weapons[s]->secondary) bot->secondary_index = s;
        else if (bot->secondary_index < 0) bot->secondary_index = bot_random_secondary(game);
    }
    if (!bot->cosmetics_rolled) {
        bot->cosmetics_rolled = 1;
        bot_roll_cosmetics(bot);
        bot_roll_weapon_skins(game, bot);
    } else if (class_changed) {
        bot_roll_weapon_skins(game, bot);
    }

    if (game->hooks.bot_spawning) game->hooks.bot_spawning(game, p, 0);
    p->class_index = bot->class_index;
    p->secondary_index = bot->secondary_index;
    p->cosmetics = bot->cosmetics;
    p->cosmetics_v7 = bot->cosmetics_v7;
    p->skins = bot->skins;
    p->spray_index = 0;
    p->slid_cont = 1;
    p->lag_comp = 1.0f;
    player_spawn(p);
    if (!p->active) return 0;
    if (bot->pinned) { // bot_place / add(spawnPoint)
        p->position = bot->pin_position;
        p->last_position = p->position;
        p->direction.y = bot->pin_yaw;
    }
    p->dmg_mlt = bot->config.dmg_mlt > 0.0f ? bot->config.dmg_mlt : 1.0f;
    if (bot->config.speed_mlt > 0.0f) p->speed *= bot->config.speed_mlt;
    if (bot->config.health_mlt > 0.0f && bot->config.health_mlt != 1.0f) {
        p->max_health = (int) ((float) p->max_health * bot->config.health_mlt);
        if (p->max_health < 1) p->max_health = 1;
        p->health = (float) p->max_health;
    }
    p->god_mode = bot->config.god_mode;
    bot_reset_ai(bot, p);
    bot->was_active = 1;
    bot->spawn_delay = 0.0f;
    bot->pending = 0;

    bot_broadcast_spawn(game, p);
    bot_send_info(game, bot, NET_TARGET_ALL);
    if (game->hooks.bot_spawning) game->hooks.bot_spawning(game, p, 1);
    if (first && !bot->joined && bot->config.join_message) bot_server_message(game, bot->name, 1);
    bot->joined = 1;
    game->leaders_dirty = 1; // syncLeaders(true)
    return 1;
}

static void bot_free_player(Game *game, Player *p) {
    game_player_left(game, p);
    const NetLeavePacket leave = {.uid = p->uid};
    game_send(game, NET_TARGET_ALL, NET_PACKET_LEAVE, &leave, sizeof(leave));
    if (game->hooks.bot_removing) game->hooks.bot_removing(game, p);
    for (size_t i = 0; i < game->player_count; i++) {
        if (game->players[i] != p) continue;
        memmove(&game->players[i], &game->players[i + 1], (game->player_count - i - 1) * sizeof(Player *));
        game->player_count--;
        break;
    }
    free(p->loadout);
    free(p->ammo);
    free(p->reloads);
    free(p->input_queue);
    free(p);
    game->leaders_dirty = 1;
}

void game_bots_remove(Game *game, GameBot *bot) {
    GameBots *bots = game ? game->bots : NULL;
    if (!bots || bot < bots->list || bot >= bots->list + bots->count) return;
    const int leave_message = bot->config.leave_message && bot->player;
    char name[PLAYER_NAME_SIZE];
    snprintf(name, sizeof(name), "%s", bot->name);
    if (bot->player) bot_free_player(game, bot->player);
    const int index = (int) (bot - bots->list);
    memmove(&bots->list[index], &bots->list[index + 1], (size_t) (bots->count - index - 1) * sizeof(GameBot));
    bots->count--;
    if (leave_message) bot_server_message(game, name, 0);
}

int game_bots_remove_all(Game *game) {
    const int n = game_bots_count(game);
    while (game_bots_count(game)) game_bots_remove(game, &game->bots->list[game->bots->count - 1]);
    return n;
}

void game_bots_reset(Game *game) {
    for (int i = game_bots_count(game) - 1; i >= 0; i--) {
        if (!game->bots->list[i].persistent) game_bots_remove(game, &game->bots->list[i]);
    }
}

void game_bots_free(Game *game) {
    if (!game || !game->bots) return;
    bot_nav_free(game->bots->nav);
    free(game->bots);
    game->bots = NULL;
}

static int bot_can_spawn_now(const Game *game) {
    return game->ready && game_is_authority(game) && game->end_timer <= 0.0f && game->map;
}

GameBot *game_bots_add(Game *game, const BotAssignments *a, const vec3 *at, const float yaw) {
    GameBots *bots = game_bots(game);
    if (!bots || bots->count >= GAME_BOTS_MAX) return NULL;
    GameBot *bot = &bots->list[bots->count++];
    memset(bot, 0, sizeof(*bot));
    bot->id = bots->next_id++;
    bot->type = bots->type;
    bot->config = bots->config[bots->type]; // { ...config }
    bot_spawn_options_none(&bot->options);
    if (a) {
        bot_apply_config(&bot->config, a); // overrides[key] != null
        bot->options = a->spawn;
    }
    bot_random_socket_id(bot->socket_id);
    char name[PLAYER_NAME_SIZE] = "";
    if (bot->options.name[0]) snprintf(name, sizeof(name), "%s", bot->options.name);
    else bot_unique_name(game, name, sizeof(name)); // overrides.name || names() (bot->name is still "")
    snprintf(bot->name, sizeof(bot->name), "%s", name);
    bot->class_index = bot->options.class_index >= 0 ? bot->options.class_index : -1;
    if (bot->options.class_index >= 0 && bot->options.force_class < 0) bot->options.force_class = 1; // class= implies forceClass
    bot->secondary_index = -1;
    bot->team = bot->options.team > 0 ? bot->options.team : 0;
    bot->lives = bot->options.lives > 0 ? bot->options.lives : 0;
    bot->persistent = bot->options.persistent > 0;
    bot->can_respawn = 1;
    bot->target_uid = -1;
    bot->ignore_uid = -1;
    bot->uid = bot_alloc_uid(game);
    bot_roll_identity(bot, a);
    if (at) {
        bot->pinned = 1;
        bot->pin_position = *at;
        bot->pin_yaw = yaw;
    }
    bot->pending = 1;
    if (bot_can_spawn_now(game)) bot_spawn(game, bot);
    return bot;
}

// bots.js setupLocal: five persistent bots that look like a populated public lobby.
void game_bots_setup_local(Game *game) {
    for (int i = 0; i < 5; i++) {
        BotAssignments a;
        bot_assignments_init(&a);
        const int level = bot_rand_int(i == 0 ? 26 : 1, i == 0 ? 50 : 25);
        char *argv[20];
        char buf[20][48];
        int argc = 0;
        snprintf(buf[argc], sizeof(buf[argc]), "targetBots=on"); argv[argc] = buf[argc]; argc++;
        snprintf(buf[argc], sizeof(buf[argc]), "randomCosmetics=on"); argv[argc] = buf[argc]; argc++;
        snprintf(buf[argc], sizeof(buf[argc]), "cosmeticRarity=%d", level > 25 ? 6 : 2); argv[argc] = buf[argc]; argc++;
        snprintf(buf[argc], sizeof(buf[argc]), "level=%d", level); argv[argc] = buf[argc]; argc++;
        snprintf(buf[argc], sizeof(buf[argc]), "featured=%s", level >= 20 && bot_rand_float(0.0f, 100.0f) >= 80.0f ? "on" : "off"); argv[argc] = buf[argc]; argc++;
        snprintf(buf[argc], sizeof(buf[argc]), "premium=%d", bot_rand_float(0.0f, 100.0f) >= 90.0f ? 1 : 0); argv[argc] = buf[argc]; argc++;
        snprintf(buf[argc], sizeof(buf[argc]), "clan=%s", level > 25 ? k_bot_clans[bot_rand_int(0, 13)] : "0"); argv[argc] = buf[argc]; argc++;
        snprintf(buf[argc], sizeof(buf[argc]), "difficulty=%g", level > 25 ? 0.75 : (double) bot_rand_float(0.1f, 0.5f)); argv[argc] = buf[argc]; argc++;
        snprintf(buf[argc], sizeof(buf[argc]), "randomJump=on"); argv[argc] = buf[argc]; argc++;
        snprintf(buf[argc], sizeof(buf[argc]), "dmgMlt=0.3"); argv[argc] = buf[argc]; argc++;
        snprintf(buf[argc], sizeof(buf[argc]), "class=%d", k_setup_local_classes[bot_rand_int(0, 3)]); argv[argc] = buf[argc]; argc++;
        snprintf(buf[argc], sizeof(buf[argc]), "accuracy=%d", bot_rand_int(1, 5)); argv[argc] = buf[argc]; argc++;
        snprintf(buf[argc], sizeof(buf[argc]), "roam=on"); argv[argc] = buf[argc]; argc++;
        snprintf(buf[argc], sizeof(buf[argc]), "targetDistance=500"); argv[argc] = buf[argc]; argc++;
        snprintf(buf[argc], sizeof(buf[argc]), "persistent=on"); argv[argc] = buf[argc]; argc++;
        // v10 setupLocal adds usePathing: true (L174979); v7's had no such key
        if (map_dialect_of(game) >= MAP_DIALECT_V10) { snprintf(buf[argc], sizeof(buf[argc]), "usePathing=on"); argv[argc] = buf[argc]; argc++; }
        bot_parse_assignments(argc, argv, &a);
        game_bots_add(game, &a, NULL, 0.0f);
    }
}

int game_bots_configure(Game *game, const BotAssignments *a) {
    GameBots *bots = game_bots(game);
    if (!bots || !a) return 0;
    bot_apply_config(&bots->config[BOT_TYPE_NEXTBOT], a);
    bot_apply_config(&bots->config[BOT_TYPE_TFUE], a);
    for (int i = 0; i < bots->count; i++) {
        GameBot *bot = &bots->list[i];
        bot_apply_config(&bot->config, a);
        Player *p = bot->player;
        if (!p) continue;
        // live multipliers (the next spawn applies the rest)
        p->dmg_mlt = bot->config.dmg_mlt > 0.0f ? bot->config.dmg_mlt : 1.0f;
        p->god_mode = bot->config.god_mode;
    }
    return bots->count;
}

int game_bots_set_type(Game *game, const BotType type) {
    GameBots *bots = game_bots(game);
    if (!bots) return 0;
    const int removed = game_bots_remove_all(game);
    bots->type = type;
    return removed;
}

void game_bots_set_quota(Game *game, const int quota, const int fill) {
    GameBots *bots = game_bots(game);
    if (!bots) return;
    bots->quota = quota < 0 ? 0 : quota > GAME_BOT_LIMIT ? GAME_BOT_LIMIT : quota;
    bots->quota_fill = (unsigned char) !!fill;
    bots->quota_timer = 0.0f;
    bots->quota_empty = 0.0f;
}

void game_bots_set_quota_grace(Game *game, const float seconds) {
    GameBots *bots = game_bots(game);
    if (bots) bots->quota_grace = seconds > 0.0f ? seconds * 1000.0f : 0.0f;
}

void game_bots_on_init(Game *game) {
    GameBots *bots = game ? game->bots : NULL;
    if (!bots) return;
    for (int i = 0; i < bots->count; i++) {
        GameBot *bot = &bots->list[i];
        bot->player = NULL; // freed with the old round's players by the host
        bot->pending = 1;
        bot->was_active = 0;
        bot->spawn_delay = 0.0f;
        bot->can_respawn = 1;
        bot->lives = bot->options.lives > 0 ? bot->options.lives : 0;
        bot->target_uid = -1;
        bot->path.count = 0;
    }
    bots->quota_timer = 1000.0f; // let the round's humans spawn first
}

// ---------------------------------------------------------------- world queries

#define bot_object_solid bot_object_blocks_player // shared/src/bot_nav.c

// Segment a -> a + d against every solid box (lineInRect slabs). ramps: include ramps.
static int bot_segment_hits(const Game *game, const vec3 a, const vec3 d, const int ramps) {
    const Map *map = game->map;
    if (!map) return 0;
    const float min_x = MIN(a.x, a.x + d.x), max_x = MAX(a.x, a.x + d.x);
    const float min_y = MIN(a.y, a.y + d.y), max_y = MAX(a.y, a.y + d.y);
    const float min_z = MIN(a.z, a.z + d.z), max_z = MAX(a.z, a.z + d.z);
    for (size_t i = 0; i < map->object_count; i++) {
        const Object *o = map->objects[i];
        if (!bot_object_solid(o) || (!ramps && o->ramp)) continue;
        const vec3 size = object_box_size(o);
        const float hx = size.x * 0.5f, hz = size.z * 0.5f;
        const float bx0 = o->position.x - hx, bx1 = o->position.x + hx;
        const float by0 = o->position.y, by1 = o->position.y + size.y;
        const float bz0 = o->position.z - hz, bz1 = o->position.z + hz;
        if (max_x < bx0 || min_x > bx1 || max_y < by0 || min_y > by1 || max_z < bz0 || min_z > bz1) continue;
        float t0 = 0.0f, t1 = 1.0f;
        const float org[3] = {a.x, a.y, a.z}, dir[3] = {d.x, d.y, d.z};
        const float lo[3] = {bx0, by0, bz0}, hi[3] = {bx1, by1, bz1};
        int hit = 1;
        for (int k = 0; k < 3 && hit; k++) {
            if (fabsf(dir[k]) < 1e-6f) {
                if (org[k] < lo[k] || org[k] > hi[k]) hit = 0;
                continue;
            }
            float n = (lo[k] - org[k]) / dir[k], f = (hi[k] - org[k]) / dir[k];
            if (n > f) { const float t = n; n = f; f = t; }
            if (n > t0) t0 = n;
            if (f < t1) t1 = f;
            if (t0 > t1) hit = 0;
        }
        if (hit) return 1;
    }
    return 0;
}

static int bot_has_ground(const Game *game, const vec3 p) {
    return bot_segment_hits(game, (vec3) {p.x, p.y + 3.0f, p.z}, (vec3) {0.0f, -(3.0f + BOT_GROUND_DROP), 0.0f}, 1);
}

// 0 clear, 1 low obstacle (jump), 2 blocked (wall or no ground)
static int bot_probe(const Game *game, const Player *self, const float yaw) {
    const vec3 f = bot_forward(yaw);
    const vec3 p = self->position;
    const vec3 d = {f.x * BOT_PROBE_LENGTH, 0.0f, f.z * BOT_PROBE_LENGTH};
    if (bot_segment_hits(game, (vec3) {p.x, p.y + BOT_KNEE, p.z}, d, 0)) {
        return bot_segment_hits(game, (vec3) {p.x, p.y + BOT_JUMP_CLEAR, p.z}, d, 0) ? 2 : 1;
    }
    const vec3 ahead = {p.x + d.x, p.y, p.z + d.z};
    if (!self->on_terrain && !bot_has_ground(game, ahead)) return 2;
    if (bot_hazard_at(game, ahead)) return 2; // a jump pad / kill volume
    return 0;
}

// Enemy for this bot: alive, not itself, other team (team 0 = everyone), noBots for tfue.
static int bot_is_enemy(const GameBot *bot, const Player *self, const Player *other) {
    if (!other || other == self || !other->active || other->notarget) return 0; // /notarget
    if (bot->type == BOT_TYPE_TFUE && !bot->config.target_bots && other->is_bot) return 0; // findNearestTo noBots
    if (!bot->config.target_team && self->team && self->team == other->team) return 0;
    return 1;
}

static vec3 bot_aim_point(const Player *t, const float frac) {
    return (vec3) {t->position.x, t->position.y + t->height * frac, t->position.z};
}

static int bot_line_of_sight(const Game *game, const Player *self, const Player *other) {
    return game_ai_player_can_see(game, self, bot_aim_point(other, 0.8f)); // game.canSee
}

// ---------------------------------------------------------------- AI

typedef struct {
    int move;
    unsigned char jump, crouch, shoot, scope, reload;
    int swap;
    float yaw, pitch;
} BotFrame;

typedef struct {
    float skill, aggression, reaction, attack_delay;
} BotProfile;

// BOT_PROFILE_PRESETS by profileForDifficulty
static BotProfile bot_profile(const float difficulty) {
    if (difficulty >= BOT_DIFFICULTY_MAX) return (BotProfile) {1.0f, 0.9f, 0.05f, 0.0f};
    if (difficulty >= BOT_DIFFICULTY_HARD) return (BotProfile) {0.75f, 0.7f, 0.2f, 0.1f};
    if (difficulty >= BOT_DIFFICULTY_NORMAL) return (BotProfile) {0.5f, 0.5f, 0.35f, 0.2f};
    return (BotProfile) {0.2f, 0.3f, 0.5f, 0.4f};
}

static Player *bot_target(Game *game, const GameBot *bot) {
    if (bot->target_uid < 0) return NULL;
    Player *t = game_find_player(game, bot->target_uid);
    return t && t->active && !t->notarget ? t : NULL; // /notarget drops a current target too
}

// Turn `value` toward `want` by at most rate * dt along the shortest arc.
static float bot_turn_yaw(const float value, const float want, const float step) {
    const float err = bot_angle_delta(value, want);
    const float s = fabsf(err) < step ? fabsf(err) : step;
    return bot_wrap(value + (err < 0.0f ? -s : s));
}

static float bot_turn_pitch(const float value, const float want, const float step) {
    const float err = want - value;
    const float s = fabsf(err) < step ? fabsf(err) : step;
    const float v = value + (err < 0.0f ? -s : s);
    return CLAMP(v, -BOT_HALF_PI, BOT_HALF_PI);
}

static float bot_engage_range(const GameBot *bot, const Player *self) {
    const Weapon *w = self->weapon;
    const float range = w && w->range > 0.0f ? w->range : 700.0f;
    return range * (bot->config.range_mlt > 0.0f ? bot->config.range_mlt : 1.0f);
}

// Steering with the obstacle probes: the world yaw to walk along, *jump on a low obstacle;
// -1000 when every direction is blocked. Probes are cached for BOT_PROBE_MS.
static float bot_steer(Game *game, GameBot *bot, const Player *self, const float want_yaw, const float dt, unsigned char *jump) {
    bot->probe_timer -= dt;
    if (bot->probe_timer <= 0.0f) {
        bot->probe_timer = BOT_PROBE_MS;
        bot->probe_move = -99;
        bot->probe_jump = 0;
        static const int k_offsets[] = {0, 1, -1, 2, -2, 3, -3};
        for (size_t i = 0; i < sizeof(k_offsets) / sizeof(k_offsets[0]); i++) {
            const int probe = bot_probe(game, self, want_yaw + (float) k_offsets[i] * (BOT_PI / 4.0f));
            if (probe == 2) continue;
            bot->probe_move = k_offsets[i];
            bot->probe_jump = probe == 1;
            break;
        }
    }
    if (bot->probe_move == -99) return -1000.0f;
    if (bot->probe_jump && self->on_ground) *jump = 1;
    return want_yaw + (float) bot->probe_move * (BOT_PI / 4.0f);
}

// Stuck monitor (locomotion updateStuckRecovery): no progress while walking -> side-step + jump
// for a moment and a new roam goal.
static int bot_stuck_update(GameBot *bot, const Player *self, const float dt, const int moving) {
    if (bot->recover_timer > 0.0f) {
        bot->recover_timer -= dt;
        return 1;
    }
    bot->stuck_check += dt;
    if (bot->stuck_check < BOT_STUCK_CHECK_MS) return 0;
    const float moved = bot_d2d(self->position, bot->stuck_pos);
    bot->stuck_pos = self->position;
    if (moving && moved < 3.0f * bot->stuck_check / BOT_STUCK_CHECK_MS) bot->stuck_timer += bot->stuck_check;
    else bot->stuck_timer = 0.0f;
    bot->stuck_check = 0.0f;
    if (bot->stuck_timer < BOT_STUCK_MS) return 0;
    bot->stuck_timer = 0.0f;
    bot->recover_timer = BOT_RECOVER_MS;
    bot->recover_dir = bot_chance(0.5) ? MOVE_LEFT : MOVE_RIGHT;
    bot->has_roam = 0;
    return 1;
}

// Hunt's roam goal: a spawn point away from us, or where an enemy is now (KrunkNative: no
// nav areas; CS bots head for the enemy too). NextBot ROAM_* radii are too small to find
// anyone on a Krunker map without a nav mesh.
static void bot_pick_roam(Game *game, GameBot *bot, const Player *self) {
    const Map *map = game->map;
    bot->has_roam = 0;
    if (bot_chance(0.45)) {
        Player *candidates[64];
        int n = 0;
        for (size_t i = 0; i < game->player_count && n < 64; i++) {
            Player *o = game->players[i];
            if (bot_is_enemy(bot, self, o) && bot_d3d(o->position, self->position) <= bot->config.target_distance * 2.0f) candidates[n++] = o;
        }
        if (n) {
            bot->roam_target = candidates[bot_rand_int(0, n - 1)]->position;
            bot->has_roam = 1;
        }
    }
    if (!bot->has_roam && bot_chance(0.6) && bot_nav_random_point_from(game, &self->position, &bot->roam_target)) bot->has_roam = 1;
    if (!bot->has_roam && map && map->spawn_count) {
        for (int tries = 0; tries < 6; tries++) {
            const Spawn *s = map->spawns[bot_rand_int(0, (int) map->spawn_count - 1)];
            if (bot_d2d(s->position, self->position) < 120.0f && tries < 5) continue;
            bot->roam_target = s->position;
            bot->has_roam = 1;
            break;
        }
    }
    if (!bot->has_roam) { // tfue roam: a point on the roamRad circle around startP
        const float a = bot_rand_float(-BOT_PI, BOT_PI), r = bot->config.difficulty * 500.0f;
        bot->roam_target = (vec3) {bot->start.x + r * cosf(a), bot->start.y, bot->start.z + r * sinf(a)};
        bot->has_roam = 1;
    }
    bot->roam_timer = bot_rand_float(7000.0f, 14000.0f);
}

// The point to head for on the way to `goal`: the next waypoint of an A* route over the nav
// grid (bot_nav.c; recomputed every ~1.5 s, when the goal moves or after being stuck, at most
// a few searches per tick), else the goal itself. *on_path: a route is being followed.
static vec3 bot_route(Game *game, GameBot *bot, const Player *self, const vec3 goal, const float dt, int *on_path, unsigned char *jump) {
    GameBots *bots = game->bots;
    *on_path = 0;
    bot->path_timer -= dt;
    const int stale = !bot->path.count || bot->path_index >= bot->path.count || bot_d3d(goal, bot->path_goal) > 40.0f || bot->path_timer <= 0.0f;
    if (stale && bots->path_budget > 0) {
        bots->path_budget--;
        bot->path_timer = bot_rand_float(1300.0f, 1900.0f);
        bot->path_index = 0;
        bot->path_goal = goal;
        if (!bot_nav_find_path(game, self->position, goal, &bot->path)) bot->path.count = 0;
        else if (!bot->path.count && !bot->path.reached) bot->path_stranded = 1; // nothing closer to it from here
        else bot->path_stranded = 0;
    }
    if (bot->path_stranded && bot_d3d(goal, bot->path_goal) <= 40.0f) { // stay put, try another goal
        bot->has_roam = 0;
        return self->position;
    }
    while (bot->path_index < bot->path.count) {
        const vec3 wp = bot->path.points[bot->path_index];
        if (bot_d2d(wp, self->position) < 4.5f && fabsf(wp.y - self->position.y) < 6.0f) bot->path_index++;
        else break;
    }
    if (bot->path_index >= bot->path.count) return goal;
    const vec3 wp = bot->path.points[bot->path_index];
    const float d = bot_d2d(wp, self->position);
    if (d > 90.0f) { // drifted off the route (knocked back, fell): recompute
        bot->path_timer = 0.0f;
        return goal;
    }
    if (bot->path.kinds[bot->path_index] == 1 && d < 14.0f && self->on_ground && wp.y > self->position.y + 2.0f) *jump = 1;
    *on_path = 1;
    return wp;
}

// Walk toward `point` (look stays where the caller put it): move code, obstacle probes off
// the route, stuck recovery.
static void bot_move_toward(Game *game, GameBot *bot, const Player *self, const vec3 point, const int on_path, const float dt, BotFrame *f) {
    if (bot_d2d(point, self->position) < 3.0f) {
        bot_stuck_update(bot, self, dt, 0);
        return;
    }
    if (bot_stuck_update(bot, self, dt, 1)) {
        f->move = self->on_ladder ? MOVE_BACK : bot->recover_dir; // a ladder: let go of it
        bot->path_timer = 0.0f;
        if (self->on_ground && bot->recover_timer > BOT_RECOVER_MS - 120.0f) f->jump = 1;
        return;
    }
    float want, pitch;
    bot_drs(point, self->position, 0.0f, &want, &pitch);
    float yaw = want;
    if (self->on_ladder) { // the nav grid has no ladder links: step back off (Krunker climbs whatever you walk into)
        f->move = MOVE_BACK;
        return;
    }
    if (on_path) {
        // the route avoids walls and ledges; still hop a low obstacle in the way
        bot->probe_timer -= dt;
        if (bot->probe_timer <= 0.0f) {
            bot->probe_timer = BOT_PROBE_MS;
            bot->probe_jump = bot_probe(game, self, want) == 1;
        }
        if (bot->probe_jump && self->on_ground) f->jump = 1;
    } else {
        yaw = bot_steer(game, bot, self, want, dt, &f->jump);
        if (yaw <= -999.0f) {
            bot->has_roam = 0; // boxed in: pick another goal
            return;
        }
    }
    int code = bot_move_code(f->yaw, yaw);
    // An 8-way move code walks up to 22.5 degrees off the wanted heading while the bot looks
    // elsewhere (aiming): on a walkway that drifts off the edge. Keep feet over ground.
    if (!self->on_terrain && self->on_ground) {
        static const int k_try[] = {0, 1, -1};
        int chosen = -1;
        for (int i = 0; i < 3 && chosen < 0; i++) {
            const int c = ((code + k_try[i]) % 8 + 8) % 8;
            const vec3 dir = bot_forward(f->yaw - (float) c * (BOT_PI / 4.0f));
            const float reach = 7.0f + hypotf(self->velocity.x, self->velocity.z) * 120.0f; // + the stopping distance at this speed
            const vec3 ahead = {self->position.x + dir.x * reach, self->position.y, self->position.z + dir.z * reach};
            if (bot_has_ground(game, ahead) && !bot_hazard_at(game, ahead)) chosen = c;
        }
        if (chosen < 0) {
            bot->path_timer = 0.0f;
            return; // nothing safe under that step: stop, repath
        }
        code = chosen;
    }
    f->move = code;
}

static void bot_walk(Game *game, GameBot *bot, const Player *self, const vec3 goal, const float dt, BotFrame *f) {
    int on_path;
    const vec3 point = bot_route(game, bot, self, goal, dt, &on_path, &f->jump);
    bot_move_toward(game, bot, self, point, on_path, dt, f);
}

static void bot_weapon_upkeep(const GameBot *bot, const Player *self, BotFrame *f, const int in_combat, const float range) {
    const Weapon *w = self->weapon;
    if (!w || !self->ammo || !self->loadout) return;
    const int slot = self->loadout_index;
    const unsigned int ammo = self->ammo[slot];
    if (!w->melee && w->ammo && ammo == 0 && self->reload_timer <= 0.0f) {
        // maybeSwapEmptyWeapon: an empty primary in a close fight -> the secondary if it has
        // ammo; an empty secondary -> back to the primary (it reloads there)
        if (in_combat && slot == 0 && range < 200.0f && self->loadout_size > 2 && self->ammo[1] > 0) f->swap = 1;
        else if (slot != 0 && self->ammo[0] > 0) f->swap = 3;
        else f->reload = 1;
    } else if (!in_combat && !w->melee && w->ammo && ammo < w->ammo && self->reload_timer <= 0.0f && bot->state_time > 1500.0f) {
        f->reload = 1; // tactical reload once things calm down
    } else if (!in_combat && slot != 0 && self->reload_timer <= 0.0f && self->swap_timer <= 0.0f) {
        f->swap = 3; // back to the primary
    }
}

// HideState's spot: a walkable point within reach that puts distance between us and the threat
// (a raw point "away" can be past the map edge).
static void bot_pick_hide(Game *game, GameBot *bot, const Player *self) {
    vec3 best = self->position;
    float best_score = -1e9f;
    for (int i = 0; i < 12; i++) {
        vec3 p;
        if (!bot_nav_random_point_from(game, &self->position, &p)) break;
        const float from_self = bot_d2d(p, self->position);
        if (from_self > 320.0f || from_self < 40.0f) continue;
        const float score = bot_d2d(p, bot->last_known) - from_self * 0.5f;
        if (score > best_score) {
            best_score = score;
            best = p;
        }
    }
    if (best_score <= -1e8f) { // no grid: straight away from the threat, not far
        float away, pitch;
        bot_drs(self->position, bot->last_known, 0.0f, &away, &pitch);
        best = (vec3) {self->position.x + bot_forward(away).x * 80.0f, self->position.y, self->position.z + bot_forward(away).z * 80.0f};
    }
    bot->roam_target = best;
}

static void bot_set_state(GameBot *bot, const BotState state) {
    if (bot->state == state) return;
    bot->state = state;
    bot->state_time = 0.0f;
}

// AttackState::OnEnter: dodge / crouch-and-hold / coward rolls from the profile.
static void bot_enter_attack(GameBot *bot, const BotProfile *profile, Player *target, const int seen) {
    bot_set_state(bot, BOT_STATE_ATTACK);
    bot->target_uid = target->uid;
    bot->should_dodge = bot_rand_int(0, 100) <= (int) (80.0f * profile->skill);
    bot->crouch_hold = bot_rand_int(0, 100) < (int) (20.0f * (1.0f - profile->aggression));
    bot->coward = bot_rand_int(0, 100) > (int) (100.0f * profile->aggression);
    bot->coward_at = 0.0f;
    bot->first_dodge = 1;
    bot->dodge = 0;
    bot->dodge_until = 0;
    bot->not_seen = 0.0f;
    bot->have_seen = (unsigned char) seen;
    bot->reaction = (profile->reaction + profile->attack_delay) * 1000.0f + bot_rand_float(0.0f, 80.0f);
    bot->ads = bot_chance(0.45 + 0.55 * profile->skill); // KrunkNative: hip fire spread is huge, players ADS
    bot->last_known = target->position;
}

// Vision: the nearest enemy in sight (FOV 90, line of sight, targetDistance), or whoever just
// hurt us (heard/hit stimuli skip the FOV).
static Player *bot_acquire(Game *game, GameBot *bot, Player *self, const float distance) {
    // someone hit us: the latest damage_received entry
    if (self->health < bot->last_health) {
        const PlayerDamage *latest = NULL;
        for (int k = 0; k < PLAYER_DAMAGE_SLOTS; k++) {
            const PlayerDamage *d = &self->damage_received[k];
            if (d->uid >= 0 && (!latest || d->time > latest->time)) latest = d;
        }
        Player *attacker = latest ? game_find_player(game, latest->uid) : NULL;
        if (attacker && bot_is_enemy(bot, self, attacker)) return attacker;
    }
    Player *best = NULL;
    float best_d = 0.0f;
    for (size_t i = 0; i < game->player_count; i++) {
        Player *o = game->players[i];
        if (!bot_is_enemy(bot, self, o) || (bot->ignore_timer > 0.0f && o->uid == bot->ignore_uid)) continue;
        const float d = bot_d3d(o->position, self->position);
        if (d > distance || (best && d >= best_d)) continue;
        float yaw, pitch;
        bot_drs(o->position, self->position, 0.0f, &yaw, &pitch);
        if (d > 60.0f && fabsf(bot_angle_delta(bot->yaw, yaw)) > NB_VISION_FOV_HALF) continue;
        if (!bot_line_of_sight(game, self, o)) continue;
        best = o;
        best_d = d;
    }
    return best;
}

// The NextBot behaviour (Idle / Hunt / Attack / Hide) for one tick of dt ms.
// ---- v10.0.2 botPaths (L174340-174600). The pathing helpers are v10's functions _61139 / _61143
// and the usePathing branch of findTarget.

// _61139: the walk direction from node `index`: a reversible path heads away from its nearer end
// (in the middle a coin flip), a one-way path forwards.
static int bot_path_direction(const int index, const MapBotPath *path) {
    const float n = (float) path->node_count;
    if (!path->reversible) return 1;
    return (float) index >= 0.8f * n ? -1 : (float) index <= 0.2f * n ? 1 : bot_chance(0.5) ? -1 : 1;
}

// _61143: the nearest node over every path (one-way paths only their first 80 % of nodes), `skip`
// excluded (-1 none). Fills route_path / route_node / route_dir as it improves; a node within 20
// ends the search. Returns the path index, -1 none.
static int bot_nearest_path(GameBot *bot, const Map *map, const vec3 from, const int skip) {
    int best = -1;
    float best_d = INFINITY;
    for (size_t i = 0; i < map->bot_path_count; i++) {
        if ((int) i == skip) continue;
        const MapBotPath *path = &map->bot_path_list[i];
        const int limit = path->reversible ? (int) path->node_count : (int) floorf(0.8f * (float) path->node_count);
        for (int k = 0; k < limit; k++) {
            const float d = bot_d3d(from, path->nodes[k]);
            if (d >= best_d) continue;
            best = (int) i;
            best_d = d;
            bot->route_path = best;
            bot->route_node = k;
            bot->route_dir = bot_path_direction(k, path);
            if (d <= 20.0f) return best;
        }
    }
    return best;
}

// findTarget's usePathing function: the next node to walk to, 0 none (no paths, nothing found).
static int bot_next_path_node(GameBot *bot, const Map *map, const vec3 from, vec3 *out) {
    if (!map || !map->bot_path_count) return 0;
    if (bot->route_path >= 0 && (size_t) bot->route_path < map->bot_path_count) {
        const MapBotPath *path = &map->bot_path_list[bot->route_path];
        bot->route_node += bot->route_dir;
        if (bot->route_node < 0 || bot->route_node >= (int) path->node_count) {
            if (path->reversible) { // turn around at the end
                bot->route_dir = -bot->route_dir;
                bot->route_node += 2 * bot->route_dir;
                if (bot->route_node < 0 || bot->route_node >= (int) path->node_count) return 0; // a 1-node path (v10: undefined)
                *out = path->nodes[bot->route_node];
                return 1;
            }
            const int next = bot_nearest_path(bot, map, from, bot->route_path); // another path, ignoring this one
            if (next < 0) return 0;
            *out = map->bot_path_list[next].nodes[bot->route_node];
            return 1;
        }
        *out = path->nodes[bot->route_node];
        return 1;
    }
    const int first = bot_nearest_path(bot, map, from, -1);
    if (first < 0) return 0;
    *out = map->bot_path_list[first].nodes[bot->route_node];
    return 1;
}

// resetPathInfo
static void bot_reset_route(GameBot *bot) {
    bot->route_path = -1;
    bot->route_node = 0;
    bot->route_dir = 1;
    bot->has_route_target = 0;
}

static void bot_update_nextbot(Game *game, GameBot *bot, Player *self, const float dt, BotFrame *f) {
    const BotConfig *cfg = &bot->config;
    const BotProfile profile = bot_profile(cfg->difficulty);
    const int max_mode = cfg->difficulty >= BOT_DIFFICULTY_MAX;
    const float accuracy = max_mode ? 100.0f : CLAMP(MAX(cfg->accuracy, profile.skill * 100.0f), 0.0f, 100.0f);
    const float aim_rate = max_mode ? 0.035f : MAX(0.006f, NB_TURN_RATE * (0.55f + accuracy / 100.0f));
    const float path_rate = MAX(0.0025f, NB_PATH_TURN_RATE * (0.75f + accuracy / 200.0f));
    const float yaw_tolerance = max_mode ? 0.08f : NB_ATTACK_YAW_TOLERANCE;
    const float distance = max_mode ? MAX(cfg->target_distance, NB_MAX_AWARENESS_DISTANCE) : cfg->target_distance;
    const int aims = cfg->watch_target || cfg->attack_target || cfg->follow_target;

    bot->state_time += dt;
    if (bot->ignore_timer > 0.0f) bot->ignore_timer -= dt;
    if (bot->reaction > 0.0f) bot->reaction -= dt;

    Player *target = bot_target(game, bot);
    if (target && !bot_is_enemy(bot, self, target)) target = NULL;

    // vision scan
    bot->action_freq -= dt;
    const int hurt = self->health < bot->last_health;
    if (bot->action_freq <= 0.0f || hurt) {
        bot->action_freq = NB_VISION_SCAN_MS;
        if (target) {
            bot->sees_target = (unsigned char) bot_line_of_sight(game, self, target);
            // a closer visible enemy wins (TARGET_SWITCH_SCORE_MARGIN)
            Player *other = bot_acquire(game, bot, self, distance);
            if (other && other != target && bot_d3d(other->position, self->position) + 350.0f < bot_d3d(target->position, self->position)) {
                target = other;
                bot_enter_attack(bot, &profile, target, 1);
                bot->sees_target = 1;
            }
        } else if (cfg->attack_target || cfg->watch_target || cfg->follow_target) {
            Player *seen = bot_acquire(game, bot, self, distance);
            if (seen) {
                target = seen;
                bot_enter_attack(bot, &profile, target, 1);
                bot->sees_target = 1;
            }
        }
    }
    bot->last_health = self->health;

    // retreat (shouldRetreat): low on health with a threat around
    if (cfg->retreat && target && bot->state != BOT_STATE_HIDE && self->health < (float) self->max_health * NB_RETREAT_HEALTH_FRACTION) {
        bot_set_state(bot, BOT_STATE_HIDE);
        bot->hide_timer = NB_RETREAT_DURATION_MS;
        bot->last_known = target->position;
        bot_pick_hide(game, bot, self);
    }

    if (bot->state == BOT_STATE_ATTACK && !target) bot_set_state(bot, BOT_STATE_HUNT); // lost the enemy
    if (bot->state == BOT_STATE_IDLE) bot_set_state(bot, target ? BOT_STATE_ATTACK : BOT_STATE_HUNT);

    switch (bot->state) {
        case BOT_STATE_HIDE: {
            bot->hide_timer -= dt;
            if (bot->hide_timer <= 0.0f) {
                bot_set_state(bot, target ? BOT_STATE_ATTACK : BOT_STATE_HUNT);
                break;
            }
            // run to the hide spot, looking where we go
            int on_path;
            const vec3 point = bot_route(game, bot, self, bot->roam_target, dt, &on_path, &f->jump);
            float away, pitch;
            bot_drs(point, self->position, 0.0f, &away, &pitch);
            if (aims) f->yaw = bot->yaw = bot_turn_yaw(bot->yaw, away, path_rate * 1.5f * dt);
            f->pitch = bot->pitch = bot_turn_pitch(bot->pitch, 0.0f, path_rate * dt);
            if (cfg->follow_target || cfg->roam) bot_move_toward(game, bot, self, point, on_path, dt, f);
            if (bot->hide_timer < 900.0f && self->on_ground && bot_chance(0.02)) f->crouch = 1;
            bot_weapon_upkeep(bot, self, f, 0, 0.0f);
            return;
        }
        case BOT_STATE_ATTACK: {
            if (!target) break;
            if (bot->sees_target) {
                bot->have_seen = 1;
                bot->not_seen = 0.0f;
                bot->last_known = target->position;
            } else {
                bot->not_seen += dt;
                float chase = NB_ATTACK_CHASE_BASE_MS + NB_ATTACK_CHASE_AGGRO_MS * (1.0f - profile.aggression);
                if (self->crouch_val > 0.5f) chase += NB_ATTACK_CHASE_CROUCH_MS;
                if (bot->not_seen > chase) { // give up; IgnoreEnemies(1 s)
                    bot->ignore_uid = target->uid;
                    bot->ignore_timer = NB_ATTACK_DROP_IGNORE_MS;
                    bot->target_uid = -1;
                    bot->has_roam = 0;
                    bot_set_state(bot, BOT_STATE_HUNT);
                    break;
                }
            }
            // coward: break off after a short hesitation
            if (bot->coward && cfg->retreat) {
                if (bot->coward_at == 0.0f) bot->coward_at = bot->state_time + (float) bot_rand_int(800, 1600);
                if (bot->state_time >= bot->coward_at && self->health < (float) self->max_health) {
                    bot_set_state(bot, BOT_STATE_HIDE);
                    bot->hide_timer = NB_RETREAT_DURATION_MS;
                    bot->coward = 0;
                    bot_pick_hide(game, bot, self);
                    return;
                }
            }

            // aim (body.ts AimAt with the held aim-focus offset)
            const vec3 eye = bot_eye(self);
            const vec3 aim_at = bot->sees_target ? bot_aim_point(target, max_mode ? 0.88f : 0.72f) : (vec3) {bot->last_known.x, bot->last_known.y + target->height * 0.72f, bot->last_known.z};
            float want_yaw, want_pitch;
            bot_drs(aim_at, eye, 0.0f, &want_yaw, &want_pitch);
            bot->aim_offset_timer -= dt;
            if (bot->aim_offset_timer <= 0.0f) {
                bot->aim_offset_timer = NB_AIM_OFFSET_INTERVAL_MS;
                const float spread = NB_AIM_OFFSET_MAX_RAD * (1.0f - (max_mode ? 1.0f : profile.skill));
                bot->aim_offset_yaw = bot_rand_float(-1.0f, 1.0f) * spread;
                bot->aim_offset_pitch = bot_rand_float(-1.0f, 1.0f) * spread;
            }
            want_yaw += bot->aim_offset_yaw;
            want_pitch += bot->aim_offset_pitch;
            // pull down against the recoil climb (shots leave at pitch + recoilAnimY * recoilMlt),
            // as well as the bot's skill allows
            const float climb = self->recoil_anim_y * game_constants.recoil_mlt;
            const float pull = max_mode ? 1.0f : 0.25f + 0.65f * profile.skill;
            want_pitch -= climb * pull;
            if (aims) {
                f->yaw = bot->yaw = bot_turn_yaw(bot->yaw, want_yaw, aim_rate * dt);
                f->pitch = bot->pitch = bot_turn_pitch(bot->pitch, want_pitch, aim_rate * dt);
            }
            const float yaw_err = fabsf(bot_angle_delta(bot->yaw, want_yaw));
            const float range = bot_d3d(target->position, self->position);
            const Weapon *w = self->weapon;
            const int melee = w && w->melee;

            // movement: close in to combat range, then hold / dodge (AttackState::ApplyDodge)
            if (cfg->follow_target) {
                const float want_range = melee ? 4.0f : MIN(NB_COMBAT_RANGE, bot_engage_range(bot, self) * 0.6f);
                if (!bot->sees_target || range > want_range + NB_HYSTERESIS_RANGE * 0.5f) {
                    bot_walk(game, bot, self, bot->sees_target ? target->position : bot->last_known, dt, f);
                } else if (bot->crouch_hold && !melee) {
                    f->move = MOVE_NONE;
                    f->crouch = 1;
                } else if (bot->should_dodge && range <= NB_DODGE_RANGE && yaw_err <= NB_ENGAGED_FACE_TOLERANCE) {
                    if ((float) bot->dodge_until <= bot->state_time) {
                        if (profile.skill > 0.5f) bot->dodge = bot->first_dodge ? (bot_chance(0.5) ? 2 : 1) : (bot->dodge == 1 ? 2 : 1);
                        else {
                            int next;
                            do {
                                next = bot->first_dodge && bot_rand_int(0, 100) < NB_JUMP_CHANCE ? bot_rand_int(0, 3) : bot_rand_int(0, 2);
                            } while (!bot->first_dodge && next == bot->dodge);
                            bot->dodge = next;
                        }
                        bot->first_dodge = 0;
                        bot->dodge_until = (int) bot->state_time + bot_rand_int(NB_DODGE_HOLD_MIN_MS, NB_DODGE_HOLD_MAX_MS);
                    }
                    switch (bot->dodge) {
                        case 1: if (bot_probe(game, self, bot->yaw + BOT_HALF_PI) != 2) f->move = MOVE_LEFT; break;
                        case 2: if (bot_probe(game, self, bot->yaw - BOT_HALF_PI) != 2) f->move = MOVE_RIGHT; break;
                        case 3: if (self->on_ground) f->jump = 1; break;
                        default: if (range < NB_COMBAT_RANGE - NB_HYSTERESIS_RANGE && bot_probe(game, self, bot->yaw + BOT_PI) != 2) f->move = MOVE_BACK; break;
                    }
                } else if (range < NB_CLOSE_STOP_DISTANCE && !melee && bot_probe(game, self, bot->yaw + BOT_PI) != 2) {
                    f->move = MOVE_BACK;
                }
            }

            // fire: aim down sights first (snipers always, others per engagement), shotguns hip fire up close
            if (cfg->attack_target && bot->sees_target && w) {
                const int sniper = w->scope && !melee;
                const int ads = !melee && !w->no_aim && (sniper || bot->ads) && range > (w->shots > 1 ? 45.0f : 15.0f);
                if (ads) f->scope = 1;
                const float tolerance = melee ? 0.6f : MAX(yaw_tolerance * (range > 300.0f ? 0.5f : 1.0f), 0.05f);
                const int aimed = yaw_err <= tolerance && fabsf(want_pitch - bot->pitch) <= tolerance * 1.5f;
                // burst control: let an uncompensated climb settle before the next shot
                const int settled = climb * (1.0f - pull) < 0.035f + 0.02f * (1.0f - profile.skill);
                const int ready = bot->reaction <= 0.0f && settled && (!ads || self->aim_val >= (sniper ? 0.95f : 0.7f));
                if (aimed && ready && range <= bot_engage_range(bot, self) * (melee ? 1.2f : 1.0f)) f->shoot = 1;
            }
            bot_weapon_upkeep(bot, self, f, 1, range);
            if (cfg->random_jump && self->on_ground && bot->tick % 100 == 0 && bot_rand_int(0, 100) >= 25) f->jump = 1;
            return;
        }
        default:
            break;
    }

    // Hunt: roam toward the goal, looking where we walk
    bot_set_state(bot, BOT_STATE_HUNT);
    if (cfg->roam) {
        bot->roam_timer -= dt;
        // KrunkNative: usePathing on a NextBot walks the v10 botPaths node by node as its roam goal
        // (v10 has only the tfue-style Bot; this keeps the key meaningful for both managers)
        const float reached = bot->has_route_target ? BOT_PATH_RADIUS : NB_TARGET_REACHED_DISTANCE;
        if (!bot->has_roam || bot->roam_timer <= 0.0f || bot_d2d(bot->roam_target, self->position) < reached) {
            bot->has_route_target = 0;
            if (cfg->use_pathing && bot_next_path_node(bot, game->map, self->position, &bot->roam_target)) {
                bot->has_roam = bot->has_route_target = 1;
                bot->roam_timer = bot_rand_float(7000.0f, 14000.0f);
            } else {
                bot_pick_roam(game, bot, self);
            }
        }
        int on_path;
        const vec3 point = bot_route(game, bot, self, bot->roam_target, dt, &on_path, &f->jump);
        float want_yaw, want_pitch;
        bot_drs(point, self->position, 0.0f, &want_yaw, &want_pitch);
        if (aims || cfg->roam) {
            f->yaw = bot->yaw = bot_turn_yaw(bot->yaw, want_yaw, path_rate * dt);
            f->pitch = bot->pitch = bot_turn_pitch(bot->pitch, 0.0f, path_rate * dt);
        }
        // walk once roughly facing the next point (NAV_WALK_YAW_TOLERANCE ~100 degrees)
        if (fabsf(bot_angle_delta(bot->yaw, want_yaw)) < 1.75f) bot_move_toward(game, bot, self, point, on_path, dt, f);
    }
    if (cfg->random_jump && self->on_ground && bot->tick % 100 == 0 && bot_rand_int(0, 100) >= 25) f->jump = 1;
    bot_weapon_upkeep(bot, self, f, 0, 0.0f);
}

// bots.js Bot.update (the legacy "tfue" AI) for one tick of dt ms.
static void bot_update_tfue(Game *game, GameBot *bot, Player *self, const float dt, BotFrame *f) {
    const BotConfig *cfg = &bot->config;
    const float difficulty = cfg->difficulty > 0.0f ? cfg->difficulty : 1.0f;
    const float accuracy = cfg->accuracy > 0.0f ? cfg->accuracy : 50.0f;
    const float turn_speed = accuracy * difficulty * 0.002f; // turnSpd per ms
    const float roam_rad = difficulty * 500.0f, roam_time = 3500.0f / difficulty;

    bot->action_freq -= dt;
    if (bot->action_freq <= 0.0f) {
        // game.findNearestTo(player, x, y + height, z, targetTeam ? 0 : team, targetDistance, xray, AIs, players, noBots)
        Player *best = NULL;
        float best_d = 0.0f;
        for (size_t i = 0; i < game->player_count; i++) {
            Player *o = game->players[i];
            if (!bot_is_enemy(bot, self, o)) continue;
            const float d = bot_d3d(o->position, (vec3) {self->position.x, self->position.y + self->height, self->position.z});
            if (d > cfg->target_distance || (best && d >= best_d) || !bot_line_of_sight(game, self, o)) continue;
            best = o;
            best_d = d;
        }
        if (best) {
            bot->target_uid = best->uid;
            bot->action_freq = 2000.0f;
            bot_reset_route(bot); // v10: a new target drops the path (resetPathInfo)
        } else {
            bot->target_uid = -1;
        }
    }
    Player *target = bot_target(game, bot);
    if (!target) bot->target_uid = -1;

    // v10 usePathing: no target and no current path target -> the next botPaths node (it wins over
    // roaming); goToTarget drops the path target once within pathRadius, so the next tick picks
    // the following node.
    if (!target && cfg->use_pathing && !bot->has_route_target && bot_next_path_node(bot, game->map, self->position, &bot->route_target)) {
        bot->has_route_target = 1;
        bot->has_roam = 0;
        bot->roam_timer = 0.0f; // resetRoam
    }
    if (!target && !bot->has_route_target && roam_rad > 0.0f && cfg->roam) {
        bot->roam_timer -= dt;
        if (bot->roam_timer <= 0.0f) {
            bot->roam_timer = roam_time;
            const float a = bot_rand_float(-BOT_PI, BOT_PI);
            bot->roam_target = (vec3) {bot->start.x + roam_rad * cosf(a), bot->start.y, bot->start.z + roam_rad * sinf(a)};
            bot->has_roam = 1;
        }
    }
    // target || roamTarget || pathTarget || startP
    const vec3 goal = target ? target->position : bot->has_roam ? bot->roam_target : bot->has_route_target ? bot->route_target : bot->start;
    const int on_route = !target && !bot->has_roam && bot->has_route_target;
    float yaw_t, pitch_t;
    bot_drs(goal, self->position, target ? game_constants.crouch_distance * target->crouch_val : 0.0f, &yaw_t, &pitch_t);
    if (bot->tick % 10 == 0) { // aim jitter: 0.2 - difficulty * 0.2
        const float off = 0.2f - difficulty * 0.2f;
        yaw_t += off;
        pitch_t += off;
    }
    if (cfg->watch_target || cfg->attack_target || cfg->follow_target) {
        f->yaw = bot->yaw = bot_turn_yaw(bot->yaw, yaw_t, turn_speed * dt);
        f->pitch = bot->pitch = bot_turn_pitch(bot->pitch, pitch_t, turn_speed * dt);
    }
    const float dist = bot_d3d(self->position, goal);
    // goToTarget(d, isPath): a path target is walked to until within pathRadius, then dropped
    if (on_route && dist <= BOT_PATH_RADIUS) bot->has_route_target = 0;
    if (cfg->follow_target && (on_route ? dist > BOT_PATH_RADIUS : dist >= 10.0f)) { // followDistance 10
        bot_walk(game, bot, self, goal, dt, f);
        if (cfg->slide_hop) {
            f->jump = self->on_ground && self->slide_timer <= 0.05f;
            if (self->velocity.y < 0.0f && self->can_slide) f->crouch = 1;
        }
    }
    if (cfg->random_jump && bot->tick % 100 == 0 && bot_rand_int(0, 100) >= 25) f->jump = 1;
    const float range = bot_engage_range(bot, self);
    f->shoot = cfg->attack_target && target && dist < range * 0.5f;
    bot->last_health = self->health;
    bot->state = target ? BOT_STATE_ATTACK : BOT_STATE_HUNT;
    bot_weapon_upkeep(bot, self, f, target != NULL, target ? dist : 0.0f);
}

// Queue this tick as input frames of at most 1/60 s (a client's frame rate), like the
// player.inputs push of bots.js. Semi-automatic weapons release the trigger every other frame.
static void bot_queue_frames(GameBot *bot, Player *self, const BotFrame *f, const float delta_s) {
    int frames = (int) ceilf(delta_s * 60.0f - 1e-3f);
    if (frames < 1) frames = 1;
    if (frames > 8) frames = 8;
    const float each = delta_s / (float) frames;
    const int semi = self->weapon && self->weapon->no_auto;
    for (int i = 0; i < frames; i++) {
        Input in;
        memset(&in, 0, sizeof(in));
        in.seq = ++bot->seq;
        in.move_dir = f->move;
        in.delta = each;
        in.x_dir = CLAMP(f->pitch, -BOT_HALF_PI, BOT_HALF_PI);
        in.y_dir = player_wrap_yaw(f->yaw);
        in.jump = (unsigned char) (i == 0 && f->jump);
        in.crouch = f->crouch;
        in.reload = (unsigned char) (i == 0 && f->reload);
        in.swap = (unsigned char) (i == 0 ? f->swap : 0);
        in.scope = f->scope;
        int shoot = f->shoot;
        if (semi && shoot && bot->fire_prev) shoot = 0; // bots.js: if (prev && nAuto) input[5] = 0
        bot->fire_prev = (unsigned char) shoot;
        in.shoot = (unsigned char) shoot;
        player_queue_input(self, &in);
    }
}

static void bot_on_death(Game *game, GameBot *bot) {
    (void) game;
    bot->target_uid = -1;
    if (bot->lives) {
        bot->lives--;
        if (bot->lives <= 0) {
            if (bot->config.remove_on_death) {
                bot->remove = 1;
                return;
            }
            bot->can_respawn = 0;
        }
    }
    if (bot->can_respawn && bot->config.can_respawn) bot->spawn_delay = bot->config.use_spawn_delay ? GAME_DEATH_DELAY * 1000.0f : 1.0f;
}

static void bot_update(Game *game, GameBot *bot, const float dt, const float delta_s) {
    Player *self = bot->player;
    if (!self) return;
    if (!self->active) {
        if (bot->was_active) {
            bot->was_active = 0;
            bot_on_death(game, bot);
        }
        if (bot->spawn_delay > 0.0f) {
            bot->spawn_delay -= dt;
            if (bot->spawn_delay <= 0.0f) bot->spawn_delay = game_can_spawn(game, self) ? 0.0f : 1.0f;
            if (bot->spawn_delay <= 0.0f) bot_spawn(game, bot);
        }
        return;
    }
    bot->was_active = 1;
    bot->tick++;

    BotFrame f;
    memset(&f, 0, sizeof(f));
    f.move = MOVE_NONE;
    f.yaw = bot->yaw;
    f.pitch = bot->pitch;
    if (bot->type == BOT_TYPE_TFUE) bot_update_tfue(game, bot, self, dt, &f);
    else bot_update_nextbot(game, bot, self, dt, &f);

    if (bot->config.nod_yes) f.pitch = (bot->tick & 1) ? BOT_HALF_PI / 4.0f : -(BOT_HALF_PI / 4.0f);
    if (bot->config.force_crouch) f.crouch = 1;

    // mimicTarget: the target's own inputs instead of ours
    Player *target = bot_target(game, bot);
    if (bot->config.mimic_target && target) {
        if (target->input_queue_size) bot->mimic = target->input_queue[target->input_queue_size - 1];
        f.move = bot->mimic.move_dir;
        f.jump = bot->mimic.jump;
        f.crouch = bot->mimic.crouch;
        f.shoot = bot->mimic.shoot;
        f.scope = bot->mimic.scope;
        f.yaw = bot->yaw = target->direction.y;
        f.pitch = bot->pitch = target->direction.x;
    }
    bot_queue_frames(bot, self, &f, delta_s);
}

// ---------------------------------------------------------------- server.cfg quota
// KrunkNative addition (v7.1.4 has no auto-fill; doc/v7/decisions.md "Bot fill"). Humans are the
// non-bot players in the game: spectators and clients still in the menu have no Player, so they
// do not count (v7 checkPlayerLimit also counts players.list without isBot).

static int bot_quota_slots(const Game *game, const GameBots *bots) {
    return bots->slots < 0 ? 0 : bots->slots > 0 ? bots->slots : game->config.max_players > 0 ? game->config.max_players : 8;
}

static int bot_count_humans(const Game *game) {
    int humans = 0;
    for (size_t i = 0; i < game->player_count; i++) humans += game->players[i] && !game->players[i]->is_bot;
    return humans;
}

static int bot_count_quota(const GameBots *bots) {
    int n = 0;
    for (int i = 0; i < bots->count; i++) n += bots->list[i].quota;
    return n;
}

// The quota bots the lobby should have: fixed `quota`, or (fill) `quota` minus the humans, none
// without a human; never more than the slots the humans and the other bots leave.
static int bot_quota_want(const Game *game, const GameBots *bots, const int humans, const int quota_bots) {
    const int slots = bot_quota_slots(game, bots);
    int want = bots->quota_fill ? (humans > 0 ? bots->quota - humans : 0) : bots->quota;
    const int room = slots - humans - (bots->count - quota_bots); // humans need slots
    if (want > room) want = room;
    return want < 0 ? 0 : want;
}

// The quota bot to remove: one on `team` when it has one, else one on the team with the most
// players (teams stay balanced), else the newest.
static GameBot *bot_quota_victim(Game *game, GameBots *bots, int team) {
    if (team <= 0) {
        int counts[MODE_TEAMS] = {0};
        for (size_t i = 0; i < game->player_count; i++) {
            const Player *p = game->players[i];
            if (p && p->team > 0 && p->team < MODE_TEAMS) counts[p->team]++;
        }
        for (int t = 1; t < MODE_TEAMS; t++) {
            if (!counts[t] || (team > 0 && counts[t] <= counts[team])) continue;
            int has_bot = 0;
            for (int i = 0; i < bots->count && !has_bot; i++) has_bot = bots->list[i].quota && bots->list[i].player && bots->list[i].player->team == t;
            if (has_bot) team = t;
        }
    }
    for (int i = bots->count - 1; team > 0 && i >= 0; i--) {
        if (bots->list[i].quota && bots->list[i].player && bots->list[i].player->team == team) return &bots->list[i];
    }
    for (int i = bots->count - 1; i >= 0; i--) {
        if (bots->list[i].quota) return &bots->list[i];
    }
    return NULL;
}

void game_bots_human_joined(Game *game, const Player *player) {
    GameBots *bots = game ? game->bots : NULL;
    if (!bots || !bots->quota || !player || player->is_bot || !game_is_authority(game)) return;
    bots->quota_empty = 0.0f;
    const int humans = bot_count_humans(game);
    int quota_bots = bot_count_quota(bots);
    // one bot leaves per human at once (from the human's team), and any over the lobby size
    while (quota_bots > bot_quota_want(game, bots, humans, quota_bots)) {
        GameBot *bot = bot_quota_victim(game, bots, player->team);
        if (!bot) break;
        game_bots_remove(game, bot);
        quota_bots--;
    }
}

// Add / remove one bot every 300 ms toward the wanted count. Fill: after the last human left the
// bots wait `quota_grace` ms (a quick rejoin keeps the match), then all of them leave at once.
static void bot_quota_tick(Game *game, GameBots *bots, const float dt) {
    if (!bots->quota) return;
    const int humans = bot_count_humans(game);
    int quota_bots = bot_count_quota(bots);
    if (bots->quota_fill && !humans && quota_bots) {
        bots->quota_empty += dt;
        if (bots->quota_empty < bots->quota_grace) return; // hold the match for a rejoin
        for (GameBot *bot; (bot = bot_quota_victim(game, bots, 0));) game_bots_remove(game, bot);
        return;
    }
    if (humans) bots->quota_empty = 0.0f;
    bots->quota_timer -= dt;
    if (bots->quota_timer > 0.0f) return;
    bots->quota_timer = 300.0f;
    const int want = bot_quota_want(game, bots, humans, quota_bots);
    if (quota_bots < want) {
        GameBot *bot = game_bots_add(game, NULL, NULL, 0.0f); // team 0: players.add puts it on the smaller team
        if (bot) bot->quota = 1;
    } else if (quota_bots > want) {
        GameBot *bot = bot_quota_victim(game, bots, 0);
        if (bot) game_bots_remove(game, bot);
    }
}

void game_bots_tick(Game *game, const float delta) {
    GameBots *bots = game ? game->bots : NULL;
    if (!bots || !game_is_authority(game) || !game->ready || game->end_timer > 0.0f) return;
    const float delta_s = delta * (game->config.delta_mlt > 0.0f ? game->config.delta_mlt : 1.0f);
    const float dt = delta_s * 1000.0f; // bots.update(delta * deltaMlt), ms

    bot_quota_tick(game, bots, dt);
    bots->path_budget = 3; // A* searches per tick (v7 nb_update budget)
    for (int i = 0; i < bots->count; i++) {
        GameBot *bot = &bots->list[i];
        if (!bot->player) {
            if (bot->pending && bot_can_spawn_now(game)) bot_spawn(game, bot);
            continue;
        }
        bot_update(game, bot, dt, delta_s);
    }
    for (int i = bots->count - 1; i >= 0; i--) {
        if (bots->list[i].remove) game_bots_remove(game, &bots->list[i]);
    }
}
