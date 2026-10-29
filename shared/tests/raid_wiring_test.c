// raid_wiring_test: Krunker v10.0.4's raid drops wired into a real Game (shared/src/ai.c ai_rewards, game_drop_skin,
// the Loot Drop pickup, updateEventFlags, KrunkScript AI.spawn / getMatch()), through the shared game hooks.
//   - AI.kill's reward loop (deob-10.0.4.js:183017-183038): rewardReq 0 every active non-bot player, 1 the players in
//     dmgList (pushed after the death check, :182978: the killing blow of a new damager does not count), 2 lastHit.doer
//     (set after the death check too: the killing blow's dealer is the previous hitter); bots never; a custom round never
//   - rewardChance picks the rarity (a valid seven-number list, else store.lootRars); rewardUnique skips an owner
//   - isDoubleRaidDrops (:195384-195391): a public round on a Saturday or Sunday (UTC) rolls twice
//   - rewardMethod 1: a Loot Drop at the AI for that player only; its pickup gives the skin (:160448-160451)
//   - creditChallengeKill (:182984-182995): "ai_" + challengeKey for the killer (wep 12 after a melee hit, else the held
//     weapon) and, for a boss or a rewarding AI, the other active damagers
//   - KrunkScript: AI.spawn's reward / rewardReq / challengeKey / boss, getMatch().doubleRaidDrops (:187204)
// Run from the repository root: ./<build>/raid_wiring_test
#include <shared.h>
#include <ai.h>
#include <game_config.h>
#include <ks_game.h>
#include <raid_drops.h>
#include <spawnables.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures, g_checks;
#define CHECK(cond, ...) do { \
    g_checks++; \
    if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_failures++; } \
} while (0)

// ---------------------------------------------------------------- hooks: the store and the accounts

#define SKIN_COMMON 101   // rarity 0
#define SKIN_EPIC 102     // rarity 3
#define SKIN_UNOBT 103    // rarity 6

static int skin_rarity(const Game *game, const int skin) {
    (void) game;
    return skin == SKIN_COMMON ? 0 : skin == SKIN_EPIC ? 3 : skin == SKIN_UNOBT ? 6 : -1;
}

typedef struct {
    int uid, skin;
    char reason[24];
} Given;
static Given g_given[64];
static int g_given_count;

static void give_skin(Game *game, const Player *player, const int skin, const char *reason) {
    (void) game;
    if (g_given_count >= 64) return;
    g_given[g_given_count].uid = player->uid;
    g_given[g_given_count].skin = skin;
    snprintf(g_given[g_given_count].reason, sizeof(g_given[0].reason), "%s", reason ? reason : "");
    g_given_count++;
}

static int g_owner_uid = -1, g_owned_skin = -1; // has_skin: this player owns this skin
static int has_skin(const Game *game, const Player *player, const int skin) {
    (void) game;
    return player && player->uid == g_owner_uid && skin == g_owned_skin;
}

typedef struct {
    int uid, value, weapon;
    char key[48];
} Progress;
static Progress g_progress[64];
static int g_progress_count;

static void progress(Game *game, const Player *player, const int stat, const char *key, const int value, const int weapon,
                     const int streak, const int item) {
    (void) game;
    (void) streak;
    (void) item;
    if (stat || !key || strncmp(key, "ai_", 3) || g_progress_count >= 64) return; // only the AI challenges
    g_progress[g_progress_count].uid = player->uid;
    g_progress[g_progress_count].value = value;
    g_progress[g_progress_count].weapon = weapon;
    snprintf(g_progress[g_progress_count].key, sizeof(g_progress[0].key), "%s", key);
    g_progress_count++;
}

static void test_send(Game *game, const int uid, const int type, const void *payload, const uint16_t length) {
    (void) game;
    (void) uid;
    (void) type;
    (void) payload;
    (void) length;
}

static int given_to(const int uid, const int skin) {
    int n = 0;
    for (int i = 0; i < g_given_count; i++) n += g_given[i].uid == uid && (skin < 0 || g_given[i].skin == skin);
    return n;
}

static void reset_log(void) {
    g_given_count = 0;
    g_progress_count = 0;
}

// ---------------------------------------------------------------- the event clock

static int64_t g_now_ms;
static int64_t test_clock(void) { return g_now_ms; }
#define SATURDAY_MS INT64_C(1793448000000)  // 2026-10-31 12:00 UTC
#define WEDNESDAY_MS INT64_C(1793188800000) // 2026-10-28 12:00 UTC

// ---------------------------------------------------------------- the game

static const char *k_flat_map =
    "{\"name\":\"Raid Flat\",\"spawns\":[[0,0,0,0,0,0]],\"objects\":["
    "{\"p\":[0,-10,0],\"s\":[2000,10,2000]}"
    "]}";

static void game_on(Game *game, const cJSON *map) {
    static const cJSON *maps[1];
    maps[0] = map;
    memset(game, 0, sizeof(*game));
    game_configure(game, NULL, maps, 1, NULL, 0, NULL, 0, NULL, 0);
    game->hooks.send = test_send;
    game->hooks.give_skin = give_skin;
    game->hooks.skin_rarity = skin_rarity;
    game->hooks.has_skin = has_skin;
    game->hooks.progress = progress;
    game_init(game, 0, 0, 1);
}

static Player *add_player(Game *game, const int uid, const int is_bot, const vec3 at) {
    Player *player = player_init(game);
    player->uid = uid;
    player->is_bot = (unsigned char) is_bot;
    player->has_account = !is_bot;
    snprintf(player->name, sizeof(player->name), "P%d", uid);
    game_players_add(game, player);
    player_spawn(player);
    player->position = at;
    player->last_position = at;
    return player;
}

// A rewarding AI with one entry per rarity, the rarity chances on `chance_rarity` (-1: none, store.lootRars)
static GameAI *spawn_rewarder(Game *game, const char *name, const int req, const int method, const int chance_rarity) {
    GameAIData d;
    game_ai_data_defaults(&d);
    d.health = 100.0f;
    d.has_reward = 1;
    d.reward_req = req;
    d.reward_method = method;
    d.reward[0] = SKIN_COMMON;
    d.reward[1] = SKIN_EPIC;
    d.reward[2] = SKIN_UNOBT;
    d.reward_count = 3;
    if (chance_rarity >= 0) {
        double in[RAID_RARITIES] = {0};
        in[chance_rarity] = 100.0;
        d.has_reward_chance = (unsigned char) raid_reward_chance_parse(in, RAID_RARITIES, d.reward_chance);
    }
    return game_ai_spawn(game, 0, 0, name, (vec3) {0.0f, 0.0f, 40.0f}, &d);
}

static GameAI *ai_named(Game *game, const char *name) {
    for (int i = 0; game->ai && i < game->ai->count; i++)
        if (!game->ai->ais[i].removed && !strcmp(game->ai->ais[i].name, name)) return &game->ai->ais[i];
    return NULL;
}

// ---------------------------------------------------------------- AI.kill's reward loop

static void test_reward_req(Game *game, Player *a, Player *b, Player *bot) {
    printf("Running the reward loop's rewardReq test...\n");
    CHECK(game_is_pub_type(game), "the test round is public (isPubType)");
    game->double_raid_drops = 0;

    // rewardReq 0: every active non-bot player; b hits first, a lands the killing blow
    reset_log();
    GameAI *ai = spawn_rewarder(game, "Req Zero", RAID_REQ_ACTIVE, RAID_METHOD_GIVE, 6);
    CHECK(ai, "the AI spawned");
    if (!ai) return;
    game_ai_change_health(game, ai, -10.0f, b, 0, 0);
    game_ai_change_health(game, ai, -1000.0f, a, 0, 0);
    CHECK(given_to(a->uid, SKIN_UNOBT) == 1 && given_to(b->uid, SKIN_UNOBT) == 1, "both players get the drop (a %d, b %d)",
          given_to(a->uid, -1), given_to(b->uid, -1));
    CHECK(given_to(bot->uid, -1) == 0, "a bot never gets one");
    CHECK(g_given_count == 2 && !strcmp(g_given[0].reason, "raid"), "giveSkin's reason is raid (%s)", g_given_count ? g_given[0].reason : "");

    // rewardReq 1: dmgList, pushed after the death check: the killer a never hit it before, so only b
    reset_log();
    ai = spawn_rewarder(game, "Req One", RAID_REQ_DAMAGED, RAID_METHOD_GIVE, 6);
    if (!ai) return;
    game_ai_change_health(game, ai, -10.0f, b, 0, 0);
    game_ai_change_health(game, ai, -1000.0f, a, 0, 0);
    CHECK(given_to(b->uid, -1) == 1 && given_to(a->uid, -1) == 0, "rewardReq 1: the earlier damager only (a %d, b %d)",
          given_to(a->uid, -1), given_to(b->uid, -1));

    // rewardReq 1 with both having hit before the kill
    reset_log();
    ai = spawn_rewarder(game, "Req One Both", RAID_REQ_DAMAGED, RAID_METHOD_GIVE, 6);
    if (!ai) return;
    game_ai_change_health(game, ai, -10.0f, a, 0, 0);
    game_ai_change_health(game, ai, -10.0f, b, 0, 0);
    game_ai_change_health(game, ai, -1000.0f, a, 0, 0);
    CHECK(given_to(a->uid, -1) == 1 && given_to(b->uid, -1) == 1, "rewardReq 1: every damager (a %d, b %d)", given_to(a->uid, -1),
          given_to(b->uid, -1));

    // rewardReq 2: lastHit.doer, set after the death check: b hit last before a's killing blow
    reset_log();
    ai = spawn_rewarder(game, "Req Two", RAID_REQ_LAST_HIT, RAID_METHOD_GIVE, 6);
    if (!ai) return;
    game_ai_change_health(game, ai, -10.0f, a, 0, 0);
    game_ai_change_health(game, ai, -10.0f, b, 0, 0);
    game_ai_change_health(game, ai, -1000.0f, a, 0, 0);
    CHECK(given_to(b->uid, -1) == 1 && given_to(a->uid, -1) == 0, "rewardReq 2: lastHit.doer only (a %d, b %d)", given_to(a->uid, -1),
          given_to(b->uid, -1));

    // an inactive player is never eligible
    reset_log();
    ai = spawn_rewarder(game, "Req Zero Dead", RAID_REQ_ACTIVE, RAID_METHOD_GIVE, 6);
    if (!ai) return;
    b->active = 0;
    game_ai_change_health(game, ai, -1000.0f, a, 0, 0);
    b->active = 1;
    CHECK(given_to(a->uid, -1) == 1 && given_to(b->uid, -1) == 0, "an inactive player gets nothing (b %d)", given_to(b->uid, -1));
}

static void test_reward_chance(Game *game, Player *a) {
    printf("Running the rewardChance test...\n");
    game->double_raid_drops = 0;
    for (int r = 0; r < RAID_RARITIES; r++) {
        const int want = r == 0 ? SKIN_COMMON : r == 3 ? SKIN_EPIC : r == 6 ? SKIN_UNOBT : -1;
        reset_log();
        char name[24];
        snprintf(name, sizeof(name), "Chance %d", r);
        GameAI *ai = spawn_rewarder(game, name, RAID_REQ_ACTIVE, RAID_METHOD_GIVE, r);
        if (!ai) return;
        game_ai_change_health(game, ai, -1000.0f, a, 0, 0);
        if (want > 0) CHECK(given_to(a->uid, want) == 1 && given_to(a->uid, -1) == 1, "rarity %d: its one entry drops (%d given)", r, given_to(a->uid, -1));
        else CHECK(given_to(a->uid, -1) == 0, "rarity %d: no entry of that rarity, nothing drops (%d)", r, given_to(a->uid, -1));
    }
}

static void test_reward_unique(Game *game, Player *a, Player *b) {
    printf("Running the rewardUnique test...\n");
    game->double_raid_drops = 0;
    g_owner_uid = a->uid;
    g_owned_skin = SKIN_UNOBT;
    reset_log();
    GameAI *ai = spawn_rewarder(game, "Unique", RAID_REQ_ACTIVE, RAID_METHOD_GIVE, 6);
    if (!ai) return;
    ai->data.reward_unique = 1;
    game_ai_change_health(game, ai, -1000.0f, a, 0, 0);
    CHECK(given_to(a->uid, -1) == 0 && given_to(b->uid, SKIN_UNOBT) == 1, "the owner is skipped, the other player gets it (a %d, b %d)",
          given_to(a->uid, -1), given_to(b->uid, -1));

    reset_log();
    ai = spawn_rewarder(game, "Not Unique", RAID_REQ_ACTIVE, RAID_METHOD_GIVE, 6);
    if (!ai) return;
    game_ai_change_health(game, ai, -1000.0f, a, 0, 0);
    CHECK(given_to(a->uid, SKIN_UNOBT) == 1, "without rewardUnique an owner gets another (%d)", given_to(a->uid, -1));
    g_owner_uid = g_owned_skin = -1;
}

static void test_double_drops(Game *game, Player *a) {
    printf("Running the double drops test...\n");
    // updateEventFlags: a public round on a Saturday rolls twice, a Wednesday once
    game_set_event_clock(test_clock);
    g_now_ms = SATURDAY_MS;
    game_update_event_flags(game);
    CHECK(game->double_raid_drops == 1, "a Saturday (UTC) in a public round is isDoubleRaidDrops");
    reset_log();
    GameAI *ai = spawn_rewarder(game, "Double", RAID_REQ_ACTIVE, RAID_METHOD_GIVE, 6);
    if (ai) game_ai_change_health(game, ai, -1000.0f, a, 0, 0);
    CHECK(given_to(a->uid, SKIN_UNOBT) == 2, "two rolls on a double drop weekend (%d)", given_to(a->uid, -1));

    g_now_ms = WEDNESDAY_MS;
    game_update_event_flags(game);
    CHECK(game->double_raid_drops == 0, "a Wednesday is not");
    reset_log();
    ai = spawn_rewarder(game, "Single", RAID_REQ_ACTIVE, RAID_METHOD_GIVE, 6);
    if (ai) game_ai_change_health(game, ai, -1000.0f, a, 0, 0);
    CHECK(given_to(a->uid, SKIN_UNOBT) == 1, "one roll on a weekday (%d)", given_to(a->uid, -1));
    game_set_event_clock(NULL);
}

static int loot_drop_slot(const Game *game) {
    for (int s = 0; s < MODE_MAX_SPAWNABLES; s++)
        if (game->mode->spawnables[s].active && game->mode->spawnables[s].type == SPAWNABLE_LOOT_DROP) return s;
    return -1;
}

static void test_loot_drop(Game *game, Player *a, Player *b) {
    printf("Running the Loot Drop (rewardMethod 1) test...\n");
    game->double_raid_drops = 0;
    reset_log();
    GameAI *ai = spawn_rewarder(game, "Dropper", RAID_REQ_DAMAGED, RAID_METHOD_DROP_AI, 3);
    if (!ai) return;
    const vec3 at = ai->position;
    game_ai_change_health(game, ai, -10.0f, a, 0, 0);
    game_ai_change_health(game, ai, -1000.0f, b, 0, 0);
    CHECK(g_given_count == 0, "rewardMethod 1 drops, it does not give (%d given)", g_given_count);
    const int slot = loot_drop_slot(game);
    CHECK(slot >= 0, "a Loot Drop spawned");
    if (slot < 0) return;
    ModeSpawnable *drop = &game->mode->spawnables[slot];
    CHECK(drop->owner_uid == a->uid && drop->data_item == SKIN_EPIC, "for the damager, with the rolled skin (owner %d, item %d)",
          drop->owner_uid, drop->data_item);
    const float dx = drop->position.x - at.x, dz = drop->position.z - at.z;
    CHECK(fabsf(dx) >= 3.0f && fabsf(dx) <= 8.0f && fabsf(dz) >= 3.0f && fabsf(dz) <= 8.0f,
          "dropSkin's spread: 3..8 units off the AI on x and z (%.1f, %.1f)", dx, dz);

    // playerSpecific: b standing on it takes nothing, a takes it
    b->position = drop->position;
    b->position.y -= 4.0f;
    a->position = (vec3) {500.0f, 0.0f, 500.0f};
    spawnables_update(game, 0.016f);
    CHECK(drop->active && g_given_count == 0, "another player cannot pick it up");
    a->position = b->position;
    b->position = (vec3) {-500.0f, 0.0f, -500.0f};
    spawnables_update(game, 0.016f);
    CHECK(!drop->active, "its owner picks it up");
    CHECK(given_to(a->uid, SKIN_EPIC) == 1 && !strcmp(g_given[0].reason, "raid"), "the pickup gives the skin (raid) (%d)", given_to(a->uid, -1));
    a->position = b->position = (vec3) {0.0f, 0.0f, 0.0f};

    // the sandbox has no serverIO: dropSkin does nothing there
    reset_log();
    game->is_sandbox = 1;
    CHECK(game_drop_skin(game, a, SKIN_EPIC, NULL, at) < 0 && loot_drop_slot(game) < 0, "no Loot Drop in the sandbox");
    game->is_sandbox = 0;
    CHECK(game_drop_skin(game, a, 99999, NULL, at) < 0, "a skin the store does not have is not dropped");
}

static void test_not_pub(Game *game, Player *a) {
    printf("Running the custom round test...\n");
    reset_log();
    GameAI *ai = spawn_rewarder(game, "Custom", RAID_REQ_ACTIVE, RAID_METHOD_GIVE, 6);
    if (!ai) return;
    game_config_host(game, NULL, 0); // a hosted (custom) game is not isPubType (the last test: it stays hosted)
    CHECK(!game_is_pub_type(game), "a hosted game is not isPubType");
    game_ai_change_health(game, ai, -1000.0f, a, 0, 0);
    CHECK(g_given_count == 0, "no drops outside a public round (%d)", g_given_count);
}

// ---------------------------------------------------------------- creditChallengeKill

static int progress_for(const int uid, const char *key, int *weapon) {
    int n = 0;
    for (int i = 0; i < g_progress_count; i++) {
        if (g_progress[i].uid != uid || strcmp(g_progress[i].key, key)) continue;
        n += g_progress[i].value;
        if (weapon) *weapon = g_progress[i].weapon;
    }
    return n;
}

static void test_challenge_kill(Game *game, Player *a, Player *b) {
    printf("Running the creditChallengeKill test...\n");
    char key[64];
    GameAIData d;
    game_ai_data_defaults(&d);
    d.health = 100.0f;
    GameAI *ai = game_ai_spawn(game, 0, 0, "Swamp Ghoul #2", (vec3) {0.0f, 0.0f, 40.0f}, &d);
    CHECK(ai && ai_challenge_key(ai, key, sizeof(key)) && !strcmp(key, "ai_swampghoul2"), "challengeKey from the name (%s)", ai ? key : "-");
    if (!ai) return;
    snprintf(ai->data.ch_key, sizeof(ai->data.ch_key), "%s", "Night-Boss");
    CHECK(ai_challenge_key(ai, key, sizeof(key)) && !strcmp(key, "ai_nightboss"), "challengeKey from chKey (%s)", key);
    snprintf(ai->data.ch_key, sizeof(ai->data.ch_key), "%s", "!!!");
    snprintf(ai->name, sizeof(ai->name), "%s", "***");
    CHECK(!ai_challenge_key(ai, key, sizeof(key)), "nothing left of it: no challengeKey (null)");
    ai->data.ch_key[0] = 0;
    snprintf(ai->name, sizeof(ai->name), "%s", "Swamp Ghoul");

    // a plain AI: the killer only, with the held weapon
    reset_log();
    game_ai_change_health(game, ai, -10.0f, b, 0, 0);
    game_ai_change_health(game, ai, -1000.0f, a, 0, 0);
    int weapon = -2;
    const int held = a->loadout[a->loadout_index];
    CHECK(progress_for(a->uid, "ai_swampghoul", &weapon) == 1 && weapon == held, "the killer's challenge, wep = the held weapon (%d, %d)",
          weapon, held);
    CHECK(progress_for(b->uid, "ai_swampghoul", NULL) == 0, "a plain AI credits only its killer");

    // a boss: the other active damagers too (no wep); a melee hit makes the killer's wep 12
    reset_log();
    d.is_boss = 1;
    ai = game_ai_spawn(game, 0, 0, "Bog King", (vec3) {0.0f, 0.0f, 40.0f}, &d);
    if (!ai) return;
    game_ai_change_health(game, ai, -10.0f, b, 0, 0);
    ai->melee_hit = 1;
    game_ai_change_health(game, ai, -1000.0f, a, 0, 0);
    weapon = -2;
    CHECK(progress_for(a->uid, "ai_bogking", &weapon) == 1 && weapon == 12, "after a melee hit the killer's wep is 12 (%d)", weapon);
    weapon = 0;
    CHECK(progress_for(b->uid, "ai_bogking", &weapon) == 1 && weapon == -1, "a boss credits the other damagers, no wep (%d)", weapon);
}

// ---------------------------------------------------------------- KrunkScript

static char g_log[16][96];
static int g_log_count;
static void host_log(void *user, const int is_error, const char *line) {
    (void) user;
    (void) is_error;
    if (g_log_count < 16) snprintf(g_log[g_log_count++], sizeof(g_log[0]), "%s", line ? line : "");
}

static KsApiImpl g_api[KS_API_COUNT];
static void run_script(Game *game, const char *src) {
    ks_game_unload(game);
    static KsHost host;
    memset(g_api, 0, sizeof(g_api));
    ks_game_api_fill(g_api, KS_SIDE_SERVER);
    memset(&host, 0, sizeof(host));
    host.side = KS_SIDE_SERVER;
    host.log = host_log;
    host.api = g_api;
    g_log_count = 0;
    const int rc = ks_game_load(game, KS_SIDE_SERVER, src, strlen(src), &host, NULL);
    CHECK(rc == 0, "the script compiles and loads (rc %d)", rc);
    for (int i = 0; rc && i < g_log_count; i++) printf("  script: %s\n", g_log[i]);
    if (rc == 0) ks_game_start(game);
}

static const char *k_match_script =
    "public action start() {\n"
    "    obj m = GAME.CONFIG.getMatch();\n"
    "    if (m.doubleRaidDrops) { GAME.log(\"double\"); } else { GAME.log(\"single\"); }\n"
    "}\n";

static const char *k_spawn_script =
    "public action start() {\n"
    "    GAME.AI.spawn(\"1\", \"Script Boss\", 0, 0, 40, {reward: \"103,x,-2,2000000\", rewardReq: 2, rewardMethod: 0,\n"
    "        rewardChance: num[0, 0, 0, 0, 0, 0, 100], rewardUnique: true, boss: true, challengeKey: \"Hollow Man\"});\n"
    "}\n";

static void test_script(Game *game) {
    printf("Running the KrunkScript test...\n");
    game->double_raid_drops = 1;
    run_script(game, k_match_script);
    CHECK(g_log_count == 1 && !strcmp(g_log[0], "double"), "getMatch().doubleRaidDrops is the round's flag (%s)", g_log_count ? g_log[0] : "none");
    game->double_raid_drops = 0;
    run_script(game, k_match_script);
    CHECK(g_log_count == 1 && !strcmp(g_log[0], "single"), "and false off the weekend (%s)", g_log_count ? g_log[0] : "none");

    run_script(game, k_spawn_script);
    const GameAI *ai = ai_named(game, "Script Boss");
    CHECK(ai != NULL, "AI.spawn spawned it");
    if (!ai) return;
    const GameAIData *d = &ai->data;
    CHECK(d->has_reward && d->reward_count == 2 && d->reward[0] == 103 && d->reward[1] == 1000000,
          "reward keeps the /^\\d+$/ entries, limited to 1e6 (%d: %d %d)", d->reward_count, d->reward[0], d->reward[1]);
    CHECK(d->reward_req == RAID_REQ_LAST_HIT && d->reward_method == RAID_METHOD_GIVE && d->reward_unique && d->is_boss,
          "rewardReq, rewardMethod, rewardUnique, boss");
    CHECK(d->has_reward_chance && d->reward_chance[6] == 100.0, "rewardChance");
    char key[64];
    CHECK(ai_challenge_key(ai, key, sizeof(key)) && !strcmp(key, "ai_hollowman"), "challengeKey (%s)", key);
    ks_game_unload(game);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    cJSON *map = cJSON_Parse(k_flat_map);
    if (!map) {
        printf("bad map\n");
        return 1;
    }
    static Game game;
    game_on(&game, map);
    if (!game.ready || !game.mode) {
        printf("Failed to init the game\n");
        return 1;
    }
    Player *a = add_player(&game, 1, 0, (vec3) {0.0f, 0.0f, 0.0f});
    Player *b = add_player(&game, 2, 0, (vec3) {5.0f, 0.0f, 0.0f});
    Player *bot = add_player(&game, 3, 1, (vec3) {-5.0f, 0.0f, 0.0f});

    test_reward_req(&game, a, b, bot);
    test_reward_chance(&game, a);
    test_reward_unique(&game, a, b);
    test_double_drops(&game, a);
    test_loot_drop(&game, a, b);
    test_challenge_kill(&game, a, b);
    test_script(&game);
    test_not_pub(&game, a);

    printf("raid_wiring_test: %d checks, %d failures\n", g_checks, g_failures);
    cJSON_Delete(map);
    return g_failures ? 1 : 0;
}
