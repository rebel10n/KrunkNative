// Krunker v7.1.4 bot players (shared/src/bots.c) through the shared game hooks: option parsing
// (parseBotConfigAssignments), names.js, the bot_* console commands and their replies, bots as
// real players (SKINS / SPAWN / PLAYER_INFO / LEAVE, leaderboard, kills), the AI fighting,
// bot_stop / bot_kill / respawn, teams, map changes, the server.cfg quota and the per-tick cost.
// Map bots (BOT_SPAWNER, shared/src/map_ai.c): the bt* keys, the 16 limit, re-init, Spawn Bot at Node.
// Run from the repository root: ./<build>/bot_test
#include <shared.h>
#include <bots.h>
#include <bot_path.h>
#include <pcg_basic.h>
#include <bot_sense.h>
#include <ruleset.h>
#include <map_ai.h>
#include <ai.h>
#include <chat_commands.h>
#include <map_catalog.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_SENT 8192

typedef struct {
    int uid, type;
    uint16_t length;
    uint8_t payload[NET_MAX_PAYLOAD];
} SentPacket;

static SentPacket g_sent[MAX_SENT];
static int g_sent_count;
static int g_type_count[256];
static int g_failures;
static char g_reply[2048];
static int g_team_kills, g_kills, g_bot_kills;
static Game *g_game;

#define CHECK(cond, ...) do { \
    if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_failures++; } \
} while (0)

static void test_send(Game *game, const int uid, const int type, const void *payload, const uint16_t length) {
    (void) game;
    if (type >= 0 && type < 256) g_type_count[type]++;
    if (type == NET_PACKET_CHAT) {
        const NetChatPacket *chat = payload;
        snprintf(g_reply, sizeof(g_reply), "%s", chat->text);
    }
    if (type == NET_PACKET_KILL) {
        const NetKillPacket *kill = payload;
        const Player *victim = game_find_player(g_game, kill->victim_uid);
        const Player *killer = game_find_player(g_game, kill->killer_uid);
        if (killer && victim && killer != victim) {
            g_kills++;
            if (killer->is_bot) g_bot_kills++;
            if (killer->team && killer->team == victim->team) g_team_kills++;
        }
    }
    if (g_sent_count >= MAX_SENT) return;
    SentPacket *packet = &g_sent[g_sent_count++];
    packet->uid = uid;
    packet->type = type;
    packet->length = length;
    if (length) memcpy(packet->payload, payload, length); // payload may be NULL when empty
}

static void reset_sent(void) {
    g_sent_count = 0;
    memset(g_type_count, 0, sizeof(g_type_count));
}

// Console replies: a long one arrives as several Server lines (send_split), joined with " ".
static void host_log(void *user, const char *text) {
    (void) user;
    const size_t len = strlen(g_reply);
    snprintf(g_reply + len, sizeof(g_reply) - len, "%s%s", len ? " " : "", text);
}

static ChatCommandHost g_host;
static ChatPeer g_peer;

static int peer_count(void *user) {
    (void) user;
    return 1;
}

static int peer_at(void *user, const int index, ChatPeer *out) {
    (void) user;
    if (index) return 0;
    *out = g_peer;
    return 1;
}

static const char *run(const char *line) {
    g_reply[0] = 0;
    chat_command_handle(&g_host, CHAT_CMD_CONSOLE, line);
    return g_reply;
}

static void free_players(Game *game) {
    for (size_t i = 0; i < game->player_count; i++) {
        Player *p = game->players[i];
        free(p->loadout);
        free(p->ammo);
        free(p->reloads);
        free(p->input_queue);
        free(p);
    }
    free(game->players);
    game->players = NULL;
    game->player_count = 0;
}

static Player *add_human(Game *game, const int uid, const char *name) {
    Player *player = player_init(game);
    player->uid = uid;
    player->class_index = 0;
    snprintf(player->name, sizeof(player->name), "%s", name);
    game_players_add(game, player);
    player_spawn(player);
    return player;
}

static double g_now;

static void tick(Game *game, const float seconds, const float hz) {
    const float dt = 1.0f / hz;
    for (float t = 0.0f; t < seconds; t += dt) {
        g_now += dt;
        game_tick(game, (float) g_now, dt);
    }
}

static int count_bots(const Game *game, const int active_only) {
    int n = 0;
    for (size_t i = 0; i < game->player_count; i++) n += game->players[i]->is_bot && (!active_only || game->players[i]->active);
    return n;
}

// ---------------------------------------------------------------- parsing / names

static void test_parse(void) {
    printf("Running bot option parsing test...\n");
    char a1[] = "5", a2[] = "difficulty=hard", a3[] = "tfue", a4[] = "name=Foo Bar", a5[] = "god=on", a6[] = "targetbots=yes", a7[] = "class=6";
    char *argv[] = {a1, a2, a3, a4, a5, a6, a7};
    BotAssignments a;
    bot_parse_assignments(7, argv, &a);
    CHECK(!a.error[0], "no error (%s)", a.error);
    CHECK(a.count == 5, "count 5 (%d)", a.count);
    CHECK(fabsf(a.config.difficulty - BOT_DIFFICULTY_HARD) < 1e-6f, "hard = 1.8 (%f)", a.config.difficulty);
    CHECK(fabsf(a.config.dmg_mlt - 0.01f) < 1e-6f, "tfue -> dmgMlt 0.01 (%f)", a.config.dmg_mlt);
    CHECK(!strcmp(a.spawn.name, "Foo Bar"), "name (%s)", a.spawn.name);
    CHECK(a.config.god_mode && a.config.target_bots && a.spawn.class_index == 6, "aliases god / targetbots, class");

    char b1[] = "99";
    char *argv_b[] = {b1};
    bot_parse_assignments(1, argv_b, &a);
    CHECK(a.count == GAME_BOTS_MAX, "Math.min(32, count) (%d)", a.count);

    const struct { const char *token, *error; } errors[] = {
        {"x", "Expected key=value, got 'x'"},
        {"foo=1", "Unknown bot option 'foo'"},
        {"attackTarget=maybe", "Invalid boolean for 'attackTarget'"},
        {"difficulty=-1", "Invalid difficulty for 'difficulty'"},
        {"accuracy=abc", "Invalid number for 'accuracy'"},
    };
    for (size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); i++) {
        char token[64];
        snprintf(token, sizeof(token), "%s", errors[i].token);
        char *argv_e[] = {token};
        bot_parse_assignments(1, argv_e, &a);
        CHECK(!strcmp(a.error, errors[i].error), "'%s' -> '%s' (got '%s')", errors[i].token, errors[i].error, a.error);
    }

    float d = 0.0f;
    CHECK(bot_parse_difficulty("easy", &d) && fabsf(d - 0.65f) < 1e-6f, "easy 0.65");
    CHECK(bot_parse_difficulty("MAX", &d) && d == 3.0f, "max 3");
    CHECK(bot_parse_difficulty("2.5", &d) && d == 2.5f, "number");
    CHECK(!bot_parse_difficulty("0", &d) && !bot_parse_difficulty("nope", &d), "0 / words rejected");

    BotType type;
    CHECK(bot_parse_type("legacy", &type) && type == BOT_TYPE_TFUE, "legacy -> tfue");
    CHECK(bot_parse_type("NextBots", &type) && type == BOT_TYPE_NEXTBOT, "nextbots -> nextbot");

    BotAssignments preset;
    bot_difficulty_assignments(3.0f, &preset);
    CHECK(preset.config.accuracy == 100.0f && preset.config.target_distance == 5000.0f && preset.config.follow_target && !preset.config.watch_target, "max preset");
    bot_difficulty_assignments(1.8f, &preset);
    CHECK(preset.config.accuracy == 85.0f && preset.config.target_distance == 1800.0f, "hard preset");
    bot_difficulty_assignments(0.65f, &preset);
    CHECK(preset.config.accuracy == 25.0f && preset.config.target_distance == 1200.0f, "easy preset");
    char text[256];
    bot_describe_config(&preset, 0, text, sizeof(text));
    CHECK(!strcmp(text, "accuracy=25 attackTarget=true difficulty=0.65 targetDistance=1200"), "describe (%s)", text);

    int handles = 0, generated = 0;
    for (int i = 0; i < 400; i++) {
        char name[PLAYER_NAME_SIZE];
        bot_generate_name(name, sizeof(name));
        CHECK(name[0], "empty name");
        int handle = 0;
        for (int k = 0; k < g_bot_name_handles_COUNT; k++) handle |= !strcmp(name, g_bot_name_handles[k]);
        handles += handle;
        generated += !handle;
    }
    CHECK(handles > 260 && handles < 360 && generated > 40, "names.js: ~80%% handles (%d handles, %d generated)", handles, generated);
}

// ---------------------------------------------------------------- commands + AI on a flat map

static const char *k_arena =
    "{\"name\":\"Bot Arena\",\"spawns\":[[0,0,0,0,0,0],[300,0,0,0,0,0],[-300,0,0,0,0,0],[0,0,300,0,0,0],[0,0,-300,0,0,0],[250,0,250,0,0,0]],"
    "\"objects\":["
    "{\"p\":[0,-10,0],\"s\":[1200,10,1200]},"
    "{\"p\":[120,0,0],\"s\":[20,30,120]},"
    "{\"p\":[-120,0,60],\"s\":[60,6,60]}"
    "]}";

static void test_commands(Game *game) {
    printf("Running bot command test...\n");
    reset_sent();
    CHECK(!strcmp(run("/bot_list"), "No bots."), "empty list (%s)", g_reply);
    CHECK(!strcmp(run("/bot_type"), "Bot type: nextbot (use bot_type nextbot|tfue)"), "bot_type (%s)", g_reply);
    CHECK(!strcmp(run("/bot_difficulty"), "Bot difficulty: 3"), "default difficulty (%s)", g_reply);

    run("/bot_add 3 difficulty=normal");
    CHECK(!strcmp(g_reply, "Spawned 3 nextbot bots."), "bot_add reply (%s)", g_reply);
    CHECK(count_bots(game, 1) == 3 && game_bots_count(game) == 3, "3 bot players (%d)", count_bots(game, 1));
    CHECK(g_type_count[NET_PACKET_SPAWN] == 3 && g_type_count[NET_PACKET_SKINS] == 3 && g_type_count[NET_PACKET_PLAYER_INFO] == 3, "SKINS + SPAWN + PLAYER_INFO per bot (%d %d %d)",
          g_type_count[NET_PACKET_SKINS], g_type_count[NET_PACKET_SPAWN], g_type_count[NET_PACKET_PLAYER_INFO]);
    for (int i = 0; i < g_sent_count; i++) {
        if (g_sent[i].type == NET_PACKET_SPAWN) {
            const NetSpawnPacket *spawn = (const NetSpawnPacket *) g_sent[i].payload;
            CHECK(g_sent[i].uid == NET_TARGET_ALL && !spawn->is_you && spawn->active && spawn->name[0], "SPAWN to all, named");
            CHECK(spawn->class_index != 7 && spawn->class_index != 9, "no Rocketeer / Runner (%d)", spawn->class_index);
        }
        if (g_sent[i].type == NET_PACKET_PLAYER_INFO) {
            const NetPlayerInfoPacket *info = (const NetPlayerInfoPacket *) g_sent[i].payload;
            CHECK(info->account_id > GAME_BOT_ACCOUNT_BASE && info->level >= 1 && info->level <= 60, "PLAYER_INFO account %d level %d", info->account_id, info->level);
            CHECK(strlen(info->clan) < sizeof(info->clan), "clan fits");
        }
    }
    const GameBots *bots = game->bots;
    for (int i = 0; i < bots->count; i++) {
        const GameBot *bot = &bots->list[i];
        CHECK(bot->player && bot->player->is_bot && bot->player->uid == bot->uid && !strcmp(bot->player->name, bot->name), "bot %d is a named player", i);
        CHECK(fabsf(bot->config.difficulty - 1.0f) < 1e-6f, "difficulty=normal override");
        for (int k = 0; k < i; k++) {
            CHECK(bots->list[k].uid != bot->uid && strcmp(bots->list[k].name, bot->name), "unique uid / name");
        }
    }

    run("/bot_list");
    CHECK(!strncmp(g_reply, "Bots (3): nextbot:1:", 20), "bot_list (%s)", g_reply);

    run("/bot_config");
    CHECK(!strncmp(g_reply, "Bot config: accuracy=100 attackTarget=true", 42), "bot_config (%s)", g_reply);
    run("/bot_config dmgMlt=0.5 roam=off");
    CHECK(!strcmp(g_reply, "Bot config updated (dmgMlt=0.5 roam=false); 3 live bots updated."), "bot_config update (%s)", g_reply);
    CHECK(bots->list[0].player->dmg_mlt == 0.5f && !bots->list[0].config.roam, "applied live");
    run("/bot_config dmgMlt=1 roam=on");
    run("/bot_config nope=1");
    CHECK(!strcmp(g_reply, "Unknown bot option 'nope'"), "bot_config error (%s)", g_reply);

    run("/bot_difficulty hard");
    CHECK(!strcmp(g_reply, "Bot difficulty set to 1.8 (3 live bots updated; applies to future bot_add)."), "bot_difficulty (%s)", g_reply);
    run("/bot_difficulty whatever");
    CHECK(!strcmp(g_reply, "Usage: bot_difficulty <easy|normal|hard|max|number>"), "bot_difficulty usage (%s)", g_reply);
    run("/bot_diff normal");
    CHECK(!strcmp(g_reply, "Bot difficulty set to 1 (3 live bots updated; applies to future bot_add)."), "bot_diff alias (%s)", g_reply);

    run("/bot_stop");
    CHECK(!strcmp(g_reply, "Stopped 3 live bots; applies to future bot_add."), "bot_stop (%s)", g_reply);
    tick(game, 0.5f, 30.0f);
    vec3 before[3];
    for (int i = 0; i < 3; i++) before[i] = bots->list[i].player->position;
    tick(game, 2.0f, 30.0f);
    for (int i = 0; i < 3; i++) {
        const vec3 p = bots->list[i].player->position;
        CHECK(hypotf(p.x - before[i].x, p.z - before[i].z) < 0.5f, "stopped bot %d stays (moved %.2f)", i, hypotf(p.x - before[i].x, p.z - before[i].z));
    }
    run("/bot_stop off");
    CHECK(!strcmp(g_reply, "Resumed 3 live bots; applies to future bot_add."), "bot_stop off (%s)", g_reply);
    // a bare bot_stop toggles, and bot_start / bot_resume undo it
    run("/bot_stop");
    run("/bot_stop");
    CHECK(!strcmp(g_reply, "Resumed 3 live bots; applies to future bot_add."), "bot_stop toggles back (%s)", g_reply);
    run("/bot_stop");
    run("/bot_start");
    CHECK(!strcmp(g_reply, "Resumed 3 live bots; applies to future bot_add."), "bot_start (%s)", g_reply);
    CHECK(bots->list[0].config.attack_target && !bots->stopped, "bot_start resumes attacking");
    tick(game, 0.5f, 30.0f);
    for (int i = 0; i < 3; i++) before[i] = bots->list[i].player->position;
    tick(game, 3.0f, 30.0f);
    float moved = 0.0f;
    for (int i = 0; i < 3; i++) moved += hypotf(bots->list[i].player->position.x - before[i].x, bots->list[i].player->position.z - before[i].z);
    CHECK(moved > 0.5f, "resumed bots move again (%.2f)", moved);
    run("/bot_stop");
    run("/bot_resume");
    CHECK(!strcmp(g_reply, "Resumed 3 live bots; applies to future bot_add."), "bot_resume alias (%s)", g_reply);

    run("/bot_mimic on");
    CHECK(!strcmp(g_reply, "Mimic-style follow mode set (3 live bots updated)."), "bot_mimic (%s)", g_reply);
    CHECK(bots->list[0].config.mimic_target && !bots->list[0].config.attack_target, "mimic config");
    run("/bot_mimic off");
    CHECK(!strcmp(g_reply, "Combat follow mode set (3 live bots updated)."), "bot_mimic off (%s)", g_reply);

    run("/bot_stop"); // the fight may have killed one: everyone respawns first (config.deathDelay), nobody fights
    tick(game, 3.5f, 30.0f);
    CHECK(count_bots(game, 1) == 3, "3 bots alive before bot_kill (%d)", count_bots(game, 1));
    run("/bot_stop off");
    reset_sent();
    char kill[64];
    snprintf(kill, sizeof(kill), "/bot_kill %s", bots->list[0].name);
    run(kill);
    CHECK(!strcmp(g_reply, "Killed 1 bot."), "bot_kill by name (%s)", g_reply);
    CHECK(!bots->list[0].player->active && g_type_count[NET_PACKET_KILL] == 1, "the bot died (KILL sent)");
    run("/bot_kill all");
    CHECK(!strcmp(g_reply, "Killed 2 bots."), "bot_kill all (%s)", g_reply);
    run("/bot_kill");
    CHECK(!strcmp(g_reply, "Matched bots are not spawned yet."), "nothing alive (%s)", g_reply);
    run("/bot_kill zzzzqq");
    CHECK(!strcmp(g_reply, "No bots matched 'zzzzqq'."), "no match (%s)", g_reply);
    tick(game, 3.5f, 30.0f); // config.deathDelay
    CHECK(count_bots(game, 1) == 3, "bots respawn after the death delay (%d)", count_bots(game, 1));

    // permissions: requireHostOrDev, with perms on or off (v3.1.1 commands open up with perms off)
    g_reply[0] = 0;
    reset_sent();
    Player *guest = add_human(game, 500, "Guest_9");
    g_peer = (ChatPeer) {.id = 500, .name = "Guest_9", .address = "", .player = guest};
    g_host.peer_count = peer_count;
    g_host.peer_at = peer_at;
    for (int perms = 0; perms < 2; perms++) {
        g_host.perms = perms;
        chat_command_handle(&g_host, 500, "/bot_add 2");
        chat_command_handle(&g_host, 500, "/bot_kick all");
    }
    CHECK(game_bots_count(game) == 3 && g_type_count[NET_PACKET_NOTIFY] == 4, "a guest may not add / kick bots (%d bots, %d notify)", game_bots_count(game), g_type_count[NET_PACKET_NOTIFY]);
    g_peer.dev = 1; // an op / DEV account / the host's own machine may
    chat_command_handle(&g_host, 500, "/bot_add 1");
    CHECK(game_bots_count(game) == 4, "a DEV peer may (%d)", game_bots_count(game));
    run("/bot_kick 4");
    g_host.perms = 0;
    g_host.peer_count = NULL;
    g_host.peer_at = NULL;
    guest->active = 0;
    game_player_left(game, guest);
    for (size_t i = 0; i < game->player_count; i++) {
        if (game->players[i] == guest) {
            memmove(&game->players[i], &game->players[i + 1], (game->player_count - i - 1) * sizeof(Player *));
            game->player_count--;
            break;
        }
    }
    free(guest->loadout); free(guest->ammo); free(guest->reloads); free(guest->input_queue); free(guest);

    reset_sent();
    run("/bot_type tfue");
    CHECK(!strcmp(g_reply, "Bot type set to tfue; removed 3 nextbot bots."), "bot_type switch (%s)", g_reply);
    CHECK(g_type_count[NET_PACKET_LEAVE] == 3 && count_bots(game, 0) == 0, "LEAVE per removed bot");
    run("/bot_add 2");
    CHECK(!strcmp(g_reply, "Spawned 2 tfue bots."), "tfue add (%s)", g_reply);
    run("/bot_kick 1");
    CHECK(!strcmp(g_reply, "Removing 1 bot."), "bot_kick index (%s)", g_reply);
    run("/bot_kick all");
    CHECK(!strcmp(g_reply, "Removing 1 bot."), "bot_kick all (%s)", g_reply);
    run("/bot_kick");
    CHECK(!strcmp(g_reply, "No bots."), "bot_kick nothing (%s)", g_reply);
    run("/bot_type nextbot");
    run("/bot_place");
    CHECK(!strcmp(g_reply, "Spawned 1 nextbot bot."), "bot_place from the console (%s)", g_reply);
    run("/bot_kick all");
}

// The AI: bots find, aim at and kill a human and each other on the arena.
static void test_fight(Game *game) {
    printf("Running bot fight test...\n");
    Player *human = add_human(game, 900, "Human");
    human->position = (vec3) {0.0f, 0.0f, 0.0f};
    run("/bot_add 4 difficulty=hard");
    g_kills = g_bot_kills = g_team_kills = 0;
    reset_sent();
    int human_hits = 0;
    float travelled = 0.0f;
    vec3 last = game->bots->list[0].player->position;
    for (int s = 0; s < 90 * 30; s++) {
        tick(game, 1.0f / 30.0f, 30.0f);
        const Player *p = game->bots->list[0].player;
        if (p && p->active) travelled += hypotf(p->position.x - last.x, p->position.z - last.z);
        if (p) last = p->position;
        if (!human->active && game_can_spawn(game, human)) player_spawn(human); // the human respawns too
        if (human->health < human->max_health) human_hits++;
    }
    printf("  90 s: %d kills (%d by bots), shots %d, hits %d, bot 0 walked %.0f\n", g_kills, g_bot_kills, g_type_count[NET_PACKET_SHOT], g_type_count[NET_PACKET_HIT], travelled);
    CHECK(g_type_count[NET_PACKET_SHOT] > 20, "bots shoot (%d SHOT)", g_type_count[NET_PACKET_SHOT]);
    CHECK(g_bot_kills >= 3, "bots kill (%d)", g_bot_kills);
    CHECK(human_hits > 0, "the human gets shot");
    CHECK(travelled > 100.0f, "bots move (%.0f)", travelled);
    CHECK(g_type_count[NET_PACKET_LEADERBOARD] > 0, "leaderboard rows");
    int scored = 0;
    for (int i = 0; i < game->bots->count; i++) scored += game->bots->list[i].player && game->bots->list[i].player->kills > 0;
    CHECK(scored >= 1, "bots on the scoreboard with kills (%d)", scored);
    run("/bot_kick all");
    human->active = 0;
}

// Team modes: bots join the smaller team and never shoot their own.
static void test_teams(Game *game, const int tdm_index) {
    printf("Running bot team test...\n");
    free_players(game);
    game_init(game, 0, tdm_index, 1);
    CHECK(game->ready && game->mode && game->mode->config.teams, "TDM loaded");
    run("/bot_add 6 difficulty=max");
    tick(game, 0.1f, 30.0f);
    int teams[3] = {0};
    for (size_t i = 0; i < game->player_count; i++) if (game->players[i]->team >= 0 && game->players[i]->team < 3) teams[game->players[i]->team]++;
    CHECK(teams[1] == 3 && teams[2] == 3, "balanced teams (%d / %d)", teams[1], teams[2]);
    g_team_kills = g_kills = 0;
    tick(game, 60.0f, 30.0f);
    printf("  60 s TDM: %d kills, %d team kills\n", g_kills, g_team_kills);
    CHECK(g_kills > 0 && g_team_kills == 0, "kills across teams only (%d / %d)", g_kills, g_team_kills);

    // a map change: the host frees every player, the bots come back with the same identity
    const int uid = game->bots->list[0].uid;
    char name[PLAYER_NAME_SIZE];
    snprintf(name, sizeof(name), "%s", game->bots->list[0].name);
    free_players(game);
    game_init(game, 0, tdm_index, 1);
    CHECK(game->bots->list[0].player == NULL, "players forgotten on init");
    tick(game, 0.1f, 30.0f);
    CHECK(count_bots(game, 1) == 6 && game->bots->list[0].uid == uid && !strcmp(game->bots->list[0].player->name, name), "bots rejoin the next round (%d)", count_bots(game, 1));
    run("/bot_kick all");
}

static void test_quota(Game *game) {
    printf("Running bot quota test...\n");
    free_players(game);
    game_init(game, 0, 0, 1);
    game->config.max_players = 8;
    game_bots_set_quota(game, 4, 1);
    add_human(game, 700, "A");
    tick(game, 3.0f, 30.0f);
    CHECK(count_bots(game, 0) == 3, "fill: 4 players = 1 human + 3 bots (%d)", count_bots(game, 0));
    add_human(game, 701, "B");
    add_human(game, 702, "C");
    tick(game, 3.0f, 30.0f);
    CHECK(count_bots(game, 0) == 1, "humans take the slots (%d)", count_bots(game, 0));
    game_bots_set_quota(game, 5, 0);
    tick(game, 3.0f, 30.0f);
    CHECK(count_bots(game, 0) == 5, "fixed quota 5 (%d)", count_bots(game, 0));
    game->config.max_players = 6;
    tick(game, 3.0f, 30.0f);
    CHECK(count_bots(game, 0) == 3, "slots: 6 - 3 humans (%d)", count_bots(game, 0));
    game_bots_set_quota(game, 0, 0);
    run("/bot_kick all");
}

static void remove_human(Game *game, const int uid) {
    for (size_t i = 0; i < game->player_count; i++) {
        Player *p = game->players[i];
        if (p->is_bot || p->uid != uid) continue;
        game_player_left(game, p);
        memmove(&game->players[i], &game->players[i + 1], (game->player_count - i - 1) * sizeof(Player *));
        game->player_count--;
        free(p->loadout);
        free(p->ammo);
        free(p->reloads);
        free(p->input_queue);
        free(p);
        return;
    }
}

static int team_count(const Game *game, const int team) {
    int n = 0;
    for (size_t i = 0; i < game->player_count; i++) n += game->players[i]->team == team;
    return n;
}

// server.cfg bot_quota (fill): an empty lobby stays empty, one human gets quota - 1 bots, a join
// removes a bot at once (no 300 ms wait), a leave brings one back, the last leave empties the
// lobby after the grace, a rejoin inside the grace keeps the bots; TDM teams stay balanced.
static void test_quota_fill(Game *game, const int tdm_index) {
    printf("Running bot quota fill test...\n");
    free_players(game);
    game_init(game, 0, 0, 1);
    game->config.max_players = 8;
    game_bots_set_quota(game, 8, 1);
    game_bots_set_quota_grace(game, 10.0f);
    tick(game, 5.0f, 30.0f);
    CHECK(count_bots(game, 0) == 0, "empty lobby: no bots (%d)", count_bots(game, 0));
    add_human(game, 800, "A");
    tick(game, 4.0f, 30.0f);
    CHECK(count_bots(game, 0) == 7, "1 human: quota - 1 = 7 bots (%d)", count_bots(game, 0));
    add_human(game, 801, "B");
    CHECK(count_bots(game, 0) == 6, "a join removes a bot at once (%d)", count_bots(game, 0));
    for (int i = 0; i < 6; i++) {
        char name[8];
        snprintf(name, sizeof(name), "H%d", i);
        add_human(game, 810 + i, name);
    }
    CHECK(count_bots(game, 0) == 0 && game->player_count == 8, "8 humans: no bots, never over the lobby (%d bots, %zu players)",
          count_bots(game, 0), game->player_count);
    tick(game, 2.0f, 30.0f);
    CHECK(count_bots(game, 0) == 0, "a full lobby of humans stays bot-free (%d)", count_bots(game, 0));
    for (int i = 0; i < 6; i++) remove_human(game, 810 + i);
    tick(game, 3.0f, 30.0f);
    CHECK(count_bots(game, 0) == 6, "leaves bring the bots back (%d)", count_bots(game, 0));
    remove_human(game, 801);
    remove_human(game, 800);
    tick(game, 5.0f, 30.0f);
    CHECK(count_bots(game, 0) == 6, "the last human left: the bots wait out the grace (%d)", count_bots(game, 0));
    add_human(game, 802, "Back");
    tick(game, 6.0f, 30.0f);
    CHECK(count_bots(game, 0) == 7, "a rejoin inside the grace keeps the match (%d)", count_bots(game, 0));
    remove_human(game, 802);
    tick(game, 9.0f, 30.0f);
    CHECK(count_bots(game, 0) == 7, "9 s into the grace: still 7 (%d)", count_bots(game, 0));
    tick(game, 1.5f, 30.0f);
    CHECK(count_bots(game, 0) == 0 && game_bots_count(game) == 0, "after the grace the empty lobby is empty (%d)", game_bots_count(game));
    tick(game, 3.0f, 30.0f);
    CHECK(count_bots(game, 0) == 0, "and stays empty (%d)", count_bots(game, 0));

    // a round change keeps the fill state (the bots rejoin the next round)
    add_human(game, 803, "R");
    tick(game, 4.0f, 30.0f);
    free_players(game);
    game_init(game, 0, tdm_index, 1);
    add_human(game, 803, "R");
    tick(game, 4.0f, 30.0f);
    // bot players, not only live ones: 4 s into a round with real pathing a bot may already be
    // dead and waiting out its respawn delay (it has rejoined all the same)
    CHECK(count_bots(game, 0) == 7, "TDM next round: 7 bots back (%d)", count_bots(game, 0));
    int diff = team_count(game, 1) - team_count(game, 2);
    CHECK(diff >= -1 && diff <= 1, "TDM teams balanced with bots (%d / %d)", team_count(game, 1), team_count(game, 2));
    for (int i = 0; i < 3; i++) {
        char name[8];
        snprintf(name, sizeof(name), "T%d", i);
        add_human(game, 820 + i, name);
        diff = team_count(game, 1) - team_count(game, 2);
        CHECK(game->player_count == 8 && diff >= -1 && diff <= 1, "TDM join %d: a bot of the human's team left (%d / %d, %zu players)", i,
              team_count(game, 1), team_count(game, 2), game->player_count);
    }
    tick(game, 2.0f, 30.0f);
    diff = team_count(game, 1) - team_count(game, 2);
    CHECK(count_bots(game, 0) == 4 && diff >= -1 && diff <= 1, "TDM 4 humans + 4 bots (%d bots, %d / %d)", count_bots(game, 0),
          team_count(game, 1), team_count(game, 2));

    // ranked / custom (slots -1): no quota bots
    game->bots->slots = -1;
    tick(game, 2.0f, 30.0f);
    CHECK(count_bots(game, 0) == 0, "slots -1 (ranked): the quota bots leave (%d)", count_bots(game, 0));
    game->bots->slots = 0;
    game_bots_set_quota(game, 0, 0);
    run("/bot_kick all");
    free_players(game);
    game_init(game, 0, 0, 1);
}

// Two active players standing on one spot (game_get_spawn_point's spawn_occupied radius).
static int count_stacked(const Game *game) {
    const float radius = 2.0f * game_constants.player_scale;
    int n = 0;
    for (size_t i = 0; i < game->player_count; i++) {
        const Player *a = game->players[i];
        for (size_t j = i + 1; a->active && j < game->player_count; j++) {
            const Player *b = game->players[j];
            const float dx = a->position.x - b->position.x, dz = a->position.z - b->position.z;
            n += b->active && dx * dx + dz * dz < radius * radius && fabsf(a->position.y - b->position.y) < game_constants.player_height;
        }
    }
    return n;
}

// server.cfg bot_quota fill pace (bot_quota_tick): one human in a 16-player lobby gets its 15 bots
// within 2 s (the quota used to add one per 300 ms: 4.5 s), the first tick puts a bot on every free
// spawn (at most 8 per tick) and never two bodies on one spot, a leave's bots come back as fast,
// a lowered quota takes its bots at once, and a fill tick stays under a quarter of a tick.
static void test_quota_pace(Game *game, const int map_index, const int mode_index, const char *label) {
    printf("Running bot quota pace test (%s)...\n", label);
    free_players(game);
    game_init(game, map_index, mode_index, 1);
    game->config.max_players = 16;
    game_bots_set_quota(game, 16, 1);
    game_bots_set_quota_grace(game, 10.0f);
    tick(game, 1.5f, 30.0f); // past the round-start hold: an idle, empty lobby
    CHECK(count_bots(game, 0) == 0, "pace %s: an empty lobby has no bots (%d)", label, count_bots(game, 0));

    bot_nav_for(game); // the grid is built on a map's first bot update (~100 ms on Sandstorm): not the fill's cost
    add_human(game, 900, "P");
    const int spawns = (int) game->map->spawn_count;
    int per_tick[90] = {0}, ticks = 0, stacked_first = 0, free_first = 0;
    double worst_ms = 0.0;
    for (; ticks < 90 && count_bots(game, 0) < 15; ticks++) {
        const clock_t start = clock();
        tick(game, 1.0f / 30.0f, 30.0f);
        const double ms = (double) (clock() - start) * 1000.0 / CLOCKS_PER_SEC;
        if (ms > worst_ms) worst_ms = ms;
        per_tick[ticks] = count_bots(game, 0);
        if (!ticks) {
            stacked_first = count_stacked(game);
            free_first = game_spawn_point_free(game);
        }
    }
    printf("  %d spawns, 16 quota, 1 human: bots after each 30 Hz tick:", spawns);
    for (int i = 0; i < ticks; i++) printf(" %d", per_tick[i]);
    printf("\n  full after %d ticks (%.2f s), worst tick %.3f ms\n", ticks, ticks / 30.0, worst_ms);
    CHECK(count_bots(game, 0) == 15, "pace %s: 1 human fills to 16 players (%d bots)", label, count_bots(game, 0));
    CHECK(ticks <= 60, "pace %s: full within 2 s of the join (%d ticks)", label, ticks);
    CHECK(per_tick[0] >= 8 || per_tick[0] == 15 || !free_first, "pace %s: the first tick fills every free spawn (%d bots, a spawn still free: %d)",
          label, per_tick[0], free_first);
    CHECK(stacked_first == 0, "pace %s: no two bodies on one spawn after the burst (%d pairs)", label, stacked_first);
    CHECK(worst_ms < 8.0, "pace %s: the fill tick costs %.3f ms (8 ms: a quarter of a 30 Hz tick, debug build)", label, worst_ms);

    add_human(game, 901, "Q");
    add_human(game, 902, "R");
    add_human(game, 903, "S");
    CHECK(count_bots(game, 0) == 12, "pace %s: 3 joins take 3 bots at once (%d)", label, count_bots(game, 0));
    tick(game, 1.0f, 30.0f);
    remove_human(game, 903);
    remove_human(game, 902);
    remove_human(game, 901);
    ticks = 0;
    for (; ticks < 90 && count_bots(game, 0) < 15; ticks++) tick(game, 1.0f / 30.0f, 30.0f);
    CHECK(count_bots(game, 0) == 15 && ticks <= 60, "pace %s: 3 leaves bring 3 bots back within 2 s (%d bots, %d ticks)", label,
          count_bots(game, 0), ticks);

    game_bots_set_autofill(game, 1, 4); // chat `autofill 4`: 1 human + 3 bots
    tick(game, 1.0f / 30.0f, 30.0f);
    CHECK(count_bots(game, 0) == 3, "pace %s: a lowered quota takes its bots in one tick (%d)", label, count_bots(game, 0));

    game_bots_set_quota(game, 0, 0);
    run("/bot_kick all");
    game->bots->autofill_off = 0;
    game->bots->autofill_forced = 0;
    free_players(game);
    game_init(game, 0, 0, 1);
}

// ---------------------------------------------------------------- perception / combat (bot_sense.c)

// Floor, a tall wall across the middle (x -10..10, z -100..100) and a half-height wall further
// along +Z (z 300..500) whose height the test sets from the eye and aim-part heights.
static const char *k_sense_arena =
    "{\"name\":\"Sense Arena\",\"spawns\":[[200,0,0,0,0,0],[-200,0,0,0,0,0]],"
    "\"objects\":["
    "{\"p\":[0,-10,0],\"s\":[2000,10,2000]},"
    "{\"p\":[0,0,0],\"s\":[20,40,200]},"
    "{\"p\":[0,0,400],\"s\":[20,8,200]}"
    "]}";

static GameBot *place_bot(Game *game, const char *options, const vec3 at, const float yaw) {
    char text[256];
    snprintf(text, sizeof(text), "%s", options);
    char *argv[16];
    int argc = 0;
    for (char *tok = strtok(text, " "); tok && argc < 16; tok = strtok(NULL, " ")) argv[argc++] = tok;
    BotAssignments a;
    bot_parse_assignments(argc, argv, &a);
    CHECK(!a.error[0], "bot options (%s)", a.error);
    GameBot *bot = game_bots_add(game, &a, &at, yaw);
    CHECK(bot && bot->player && bot->player->active, "bot placed");
    return bot;
}

static float yaw_to(const vec3 from, const vec3 to) { // bot_drs yaw
    return -atan2f(-(from.x - to.x), from.z - to.z);
}

static void test_sense(Game *game, const int sense_map) {
    printf("Running bot perception / combat test...\n");
    free_players(game);
    game_init(game, sense_map, 0, 1);
    CHECK(game->ready && game->map->object_count == 3, "Sense Arena loaded");
    Player *human = add_human(game, 950, "Target");
    const float yaw_west = yaw_to((vec3) {200.0f, 0.0f, 0.0f}, (vec3) {-200.0f, 0.0f, 0.0f}); // facing -X

    // ---- line of sight through / around / over geometry
    GameBot *bot = place_bot(game, "watchTarget=on attackTarget=off followTarget=off roam=off difficulty=hard", (vec3) {200.0f, 0.0f, 0.0f}, yaw_west);
    Player *self = bot->player;
    human->position = (vec3) {-100.0f, 0.0f, 0.0f}; // straight through the tall wall
    CHECK(bot_sense_los(game, self, human) < 0, "the wall blocks every aim part");
    human->position = (vec3) {-100.0f, 0.0f, 250.0f}; // around the wall's end
    CHECK(bot_sense_los(game, self, human) >= 0, "around the wall: in sight");
    // the half wall: set so the gut ray clips it and the head ray clears it
    Object *half = game->map->objects[2];
    self->position = (vec3) {200.0f, 0.0f, 400.0f};
    human->position = (vec3) {-200.0f, 0.0f, 400.0f};
    // game.canSee (ai.c) starts at the eye with the feet -> target slope: at the midpoint the
    // ray is at eye + part / 2
    const float eye_y = self->position.y + self->height - game_constants.camera_height;
    const float gut_y = bot_sense_aim_part(human, BOT_AIM_GUT).y, head_y = bot_sense_aim_part(human, BOT_AIM_HEAD).y;
    const float chest_y = bot_sense_aim_part(human, BOT_AIM_CHEST).y;
    half->scale.y = eye_y + chest_y * 0.5f + 0.3f;
    CHECK(head_y - chest_y > 1.0f && eye_y + head_y * 0.5f > half->scale.y, "aim parts spread (gut %.1f head %.1f eye %.1f)", gut_y, head_y, eye_y);
    CHECK(!bot_sense_los_point(game, self, bot_sense_aim_part(human, BOT_AIM_GUT)), "the half wall hides the gut");
    CHECK(!bot_sense_los_point(game, self, bot_sense_aim_part(human, BOT_AIM_CHEST)), "and the chest");
    CHECK(bot_sense_los(game, self, human) == BOT_AIM_HEAD, "the head shows over it (%d)", bot_sense_los(game, self, human));
    half->scale.y = 40.0f;
    CHECK(bot_sense_los(game, self, human) < 0, "raised: nothing shows");
    half->scale.y = 8.0f;
    bot_nav_invalidate(game);

    // ---- memory: the target is kept after it breaks line of sight, dropped once forgotten
    self->position = (vec3) {200.0f, 0.0f, 0.0f};
    human->position = (vec3) {-100.0f, 0.0f, 250.0f}; // in view (about 40 degrees off the look)
    tick(game, 1.5f, 60.0f);
    CHECK(bot->target_uid == human->uid && bot->state == BOT_STATE_ATTACK && bot->sees_target, "sees and targets the human (uid %d state %d sees %d)",
          bot->target_uid, bot->state, bot->sees_target);
    const BotThreat *known = bot_sense_threat(&bot->sense, human->uid);
    CHECK(known && known->visible && known->stimulus == BOT_STIMULUS_SEEN && known->aware_at >= 0.0f, "a known, recognized, visible threat");
    const vec3 seen_at = human->position;
    human->position = (vec3) {-100.0f, 0.0f, 0.0f}; // steps behind the tall wall
    tick(game, 0.5f, 60.0f);
    known = bot_sense_threat(&bot->sense, human->uid);
    CHECK(bot->target_uid == human->uid && bot->state == BOT_STATE_ATTACK && !bot->sees_target, "out of sight: still the target (state %d sees %d)", bot->state, bot->sees_target);
    CHECK(known && !known->visible && hypotf(known->known.x - seen_at.x, known->known.z - seen_at.z) < 30.0f, "last known where it was seen (%.0f %.0f)",
          known ? known->known.x : 0.0f, known ? known->known.z : 0.0f);
    CHECK(hypotf(bot->last_known.x - seen_at.x, bot->last_known.z - seen_at.z) < 30.0f, "bot->last_known follows the memory");
    const BotSenseProfile hard = bot_sense_profile(BOT_DIFFICULTY_HARD, 85.0f);
    tick(game, hard.memory_ms / 1000.0f * 0.6f, 60.0f);
    CHECK(bot->target_uid == human->uid && bot->state == BOT_STATE_ATTACK, "60%% into the memory: still hunting it (state %d)", bot->state);
    tick(game, hard.memory_ms / 1000.0f * 0.6f, 60.0f);
    CHECK(bot->target_uid < 0 && bot->state == BOT_STATE_HUNT && !bot_sense_threat(&bot->sense, human->uid), "forgotten after memory_ms (uid %d state %d)", bot->target_uid, bot->state);
    // v7 chase timeout no more: a 4 s gap did not drop it (checked above at 60%: 5.4 s)
    game_bots_remove_all(game);

    // ---- search: a follower walks to the last known position, then fans out from it
    bot = place_bot(game, "followTarget=on attackTarget=off watchTarget=on roam=off difficulty=hard", (vec3) {200.0f, 0.0f, 0.0f}, yaw_west);
    self = bot->player;
    human->position = (vec3) {-100.0f, 0.0f, 250.0f};
    tick(game, 1.5f, 60.0f);
    CHECK(bot->target_uid == human->uid && bot->sees_target, "follower targets the human");
    human->position = (vec3) {-100.0f, 0.0f, -3000.0f}; // gone far out of range (still alive)
    const float start_d = hypotf(self->position.x - seen_at.x, self->position.z - seen_at.z);
    tick(game, 0.3f, 60.0f);
    CHECK(bot->target_uid == human->uid && !bot->sees_target, "lost but remembered");
    float closest = start_d;
    int searched = 0;
    for (int i = 0; i < 60 * 12; i++) { // the whole memory window (it ends the loop): a fan spot may come first
        tick(game, 1.0f / 60.0f, 60.0f);
        const float d = hypotf(self->position.x - seen_at.x, self->position.z - seen_at.z);
        if (d < closest) closest = d;
        searched |= bot->sense.search_active;
        if (bot->target_uid < 0) break;
    }
    CHECK(closest < start_d * 0.35f, "walked to the last known position (%.0f -> %.0f)", start_d, closest);
    CHECK(searched, "searched around it");
    CHECK(bot->sense.search_step >= 2, "several search spots (%d)", bot->sense.search_step);
    game_bots_remove_all(game);

    // ---- hearing: a shot behind the wall seeds a threat near the shooter
    bot = place_bot(game, "watchTarget=on attackTarget=off followTarget=off roam=off difficulty=hard", (vec3) {200.0f, 0.0f, 0.0f}, yaw_west);
    self = bot->player;
    human->position = (vec3) {-100.0f, 0.0f, 0.0f};
    tick(game, 0.5f, 60.0f);
    CHECK(bot->target_uid < 0 && !bot_sense_threat(&bot->sense, human->uid), "silent behind the wall: unknown");
    human->life_shots += 1; // a shot
    human->did_shoot = 1;
    tick(game, 0.6f, 60.0f);
    human->did_shoot = 0;
    known = bot_sense_threat(&bot->sense, human->uid);
    CHECK(known && known->stimulus == BOT_STIMULUS_HEARD && !known->visible, "heard the shot");
    CHECK(known && hypotf(known->known.x - human->position.x, known->known.z - human->position.z) < 60.0f, "roughly where it came from (%.0f %.0f)",
          known ? known->known.x : 0.0f, known ? known->known.z : 0.0f);
    CHECK(bot->target_uid == human->uid && !bot->sees_target, "engages the unseen shooter (uid %d)", bot->target_uid);
    // out of earshot: nothing
    bot_sense_reset(&bot->sense);
    bot->target_uid = -1;
    human->position = (vec3) {-100.0f, 0.0f, 1900.0f}; // past hear_shots (1800)
    human->life_shots += 1;
    human->did_shoot = 1;
    tick(game, 0.6f, 60.0f);
    human->did_shoot = 0;
    CHECK(!bot_sense_threat(&bot->sense, human->uid), "a shot out of earshot is not heard");
    // footsteps: a sprinting player close behind the wall
    human->position = (vec3) {-20.0f, 0.0f, 0.0f};
    self->position = (vec3) {60.0f, 0.0f, 0.0f}; // 80 apart, the wall between
    human->velocity = (vec3) {0.0f, 0.0f, 1.0f};
    human->on_ground = 1;
    tick(game, 0.3f, 60.0f);
    known = bot_sense_threat(&bot->sense, human->uid);
    CHECK(known && known->stimulus == BOT_STIMULUS_HEARD, "heard the footsteps");
    human->velocity = (vec3) {0.0f, 0.0f, 0.0f};
    game_bots_remove_all(game);

    // ---- being hit: whoever hurt us is known at once
    bot = place_bot(game, "watchTarget=on attackTarget=off followTarget=off roam=off difficulty=easy", (vec3) {200.0f, 0.0f, 0.0f}, yaw_west);
    self = bot->player;
    human->position = (vec3) {-100.0f, 0.0f, 0.0f};
    tick(game, 0.5f, 60.0f);
    CHECK(bot->target_uid < 0, "easy bot: nothing known");
    self->health -= 10.0f;
    self->damage_received[0] = (PlayerDamage) {human->uid, 10.0f, game->now};
    tick(game, 0.1f, 60.0f);
    known = bot_sense_threat(&bot->sense, human->uid);
    CHECK(known && known->stimulus == BOT_STIMULUS_HIT && known->aware_at >= 0.0f, "hit: the attacker is known");
    CHECK(bot->target_uid == human->uid, "and engaged (uid %d)", bot->target_uid);
    game_bots_remove_all(game);

    // ---- difficulty profiles and the aim error curve
    const BotSenseProfile easy = bot_sense_profile(BOT_DIFFICULTY_EASY, 25.0f), normal = bot_sense_profile(BOT_DIFFICULTY_NORMAL, 50.0f),
                          max = bot_sense_profile(BOT_DIFFICULTY_MAX, 100.0f);
    CHECK(easy.reaction_ms > normal.reaction_ms && normal.reaction_ms > hard.reaction_ms && hard.reaction_ms > max.reaction_ms, "reaction by tier");
    CHECK(easy.memory_ms < normal.memory_ms && normal.memory_ms < hard.memory_ms && hard.memory_ms <= max.memory_ms, "memory by tier");
    CHECK(easy.hear_shots < hard.hear_shots && easy.fov_half < max.fov_half, "senses by tier");
    const BotSenseProfile *tiers[] = {&easy, &normal, &hard, &max};
    for (int i = 0; i < 4; i++) {
        const float e0 = bot_sense_aim_error(tiers[i], 0.0f), e1 = bot_sense_aim_error(tiers[i], 1000.0f), e3 = bot_sense_aim_error(tiers[i], 3000.0f);
        CHECK(e0 > e1 && e1 > e3 && e3 >= tiers[i]->aim_error_locked - 1e-6f, "tier %d: the error shrinks while locked (%.4f %.4f %.4f)", i, e0, e1, e3);
        if (i) CHECK(e0 < bot_sense_aim_error(tiers[i - 1], 0.0f) && e3 <= bot_sense_aim_error(tiers[i - 1], 3000.0f), "tier %d aims better than %d", i, i - 1);
    }
    CHECK(bot_sense_aim_error(&max, 5000.0f) < 0.003f, "max locks on (%.4f)", bot_sense_aim_error(&max, 5000.0f));
    CHECK(bot_sense_profile(1.0f, 100.0f).aim_error < bot_sense_profile(1.0f, 50.0f).aim_error, "accuracy scales the error");
    // leading: a projectile is led by its flight time, hitscan by the slew lag
    Weapon rocket;
    memset(&rocket, 0, sizeof(rocket));
    rocket.projectile_config.speed = 0.5f; // units/ms
    const vec3 eye = {0.0f, 10.0f, 0.0f}, point = {0.0f, 5.0f, -500.0f}, moving = {100.0f, 0.0f, 0.0f};
    const vec3 led = bot_sense_lead(&hard, eye, point, moving, &rocket);
    CHECK(fabsf((led.x - point.x) - 100.0f * (500.0f / 0.5f / 1000.0f) * hard.lead) < 2.0f, "projectile lead (%.1f)", led.x - point.x);
    const vec3 led_hit = bot_sense_lead(&hard, eye, point, moving, NULL);
    CHECK(led_hit.x > 2.0f && led_hit.x < 20.0f, "hitscan lead is the slew lag (%.1f)", led_hit.x);
    CHECK(bot_sense_lead(&easy, eye, point, moving, &rocket).x < led.x, "easy leads less");

    // the live aim on a still target: the error averaged over the first 300 ms of the lock
    // exceeds the last 500 ms of 3 s (the aim settles), and never leaves the profile's band.
    // Averaged over several locks per tier: one 3 s lock is a handful of random error goals
    for (int i = 0; i < 4; i++) {
        bot = place_bot(game, "watchTarget=on attackTarget=off followTarget=off roam=off", (vec3) {300.0f, 0.0f, 0.0f}, 0.0f);
        self = bot->player;
        human->position = (vec3) {300.0f, 0.0f, -300.0f};
        const BotSenseProfile *p = tiers[i];
        const BotSenseRules rules = {1, 1, 5000.0f};
        float early = 0.0f, late = 0.0f, worst = 0.0f;
        int n_early = 0, n_late = 0;
        const vec3 chest = bot_sense_aim_part(human, BOT_AIM_CHEST);
        const vec3 head = bot_sense_aim_part(human, BOT_AIM_HEAD);
        for (int lock = 0; lock < 8; lock++) {
            bot_sense_reset(&bot->sense);
            bot_sense_update(game, &bot->sense, self, 0.0f, p, &rules, 0, 16.0f);
            bot_sense_update(game, &bot->sense, self, 0.0f, p, &rules, 0, BOT_SENSE_SCAN_MS);
            if (!lock) CHECK(bot_sense_visible(&bot->sense, human->uid), "tier %d: target seen", i);
            for (int ms = 0; ms < 3000; ms += 16) {
                float want_yaw, want_pitch;
                bot_sense_aim(&bot->sense, p, self, human, 1, human->position, 16.0f, &want_yaw, &want_pitch);
                // the true angles to the chest, or the head once a marksman walks up to it
                const vec3 eye_s = {self->position.x, self->position.y + self->height - game_constants.camera_height, self->position.z};
                const vec3 part = p->aim_error_locked < 0.015f && bot->sense.lock_ms > 400.0f ? head : chest;
                const double px = eye_s.x - part.x, py = eye_s.y - part.y, pz = eye_s.z - part.z;
                const float true_pitch = (float) -atan2(py, sqrt(px * px + pz * pz));
                const float true_yaw = (float) -atan2(-px, pz);
                const float err = hypotf(atan2f(sinf(want_yaw - true_yaw), cosf(want_yaw - true_yaw)), want_pitch - true_pitch);
                if (err > worst) worst = err;
                if (ms < 300) { early += err; n_early++; }
                if (ms >= 2500) { late += err; n_late++; }
            }
        }
        early /= (float) n_early;
        late /= (float) n_late;
        CHECK(late < early || late < 1e-3f, "tier %d: the aim settles (%.4f -> %.4f)", i, early, late);
        CHECK(worst <= p->aim_error * 1.05f + 1e-4f, "tier %d: never past the initial error (%.4f <= %.4f)", i, worst, p->aim_error);
        CHECK(late <= p->aim_error_locked * 1.2f + 0.004f, "tier %d: settles near the locked floor (%.4f vs %.4f)", i, late, p->aim_error_locked);
        game_bots_remove_all(game);
    }

    // ---- fire control: reaction before the first shot, bursts of burst_shots with rests
    bot = place_bot(game, "watchTarget=on attackTarget=off followTarget=off roam=off difficulty=hard", (vec3) {300.0f, 0.0f, 0.0f}, 0.0f);
    self = bot->player;
    for (size_t id = 0; id < rules_weapon_count(); id++) { // the first automatic gun
        const Weapon *w = rules_weapon((int) id);
        if (w && !w->no_auto && !w->melee && !w->burst && !w->projectile && !w->equipment) {
            self->weapon = w;
            break;
        }
    }
    CHECK(self->weapon && !self->weapon->no_auto && !self->weapon->melee, "an automatic weapon (%s)", self->weapon ? self->weapon->name : "none");
    bot_sense_reset(&bot->sense);
    bot_sense_engage(&bot->sense, &hard, self, human->uid, 0.0f);
    int first_shot_ms = -1, longest = 0, run_len = 0, rests = 0, prev = 0;
    float cooldown = 0.0f;
    for (int ms = 0; ms < 6000; ms += 16) {
        if (bot->sense.reaction_left > 0.0f) bot->sense.reaction_left -= 16.0f;
        if (bot->sense.burst_rest > 0.0f) bot->sense.burst_rest -= 16.0f;
        cooldown -= 16.0f;
        const int fire = bot_sense_fire(&bot->sense, &hard, self, 1, 1, 1, 16.0f);
        if (fire && cooldown <= 0.0f) { // the weapon's rate: a shot every 100 ms with the trigger held
            self->life_shots++;
            cooldown = 100.0f;
            run_len++;
            if (first_shot_ms < 0) first_shot_ms = ms;
        }
        if (!fire && prev) {
            if (run_len > longest) longest = run_len;
            run_len = 0;
            rests++;
        }
        prev = fire;
    }
    CHECK(first_shot_ms >= hard.reaction_ms - 16.0f, "the first shot waits the reaction (%d ms)", first_shot_ms);
    CHECK(longest <= hard.burst_shots && longest >= hard.burst_shots - 1 && rests >= 3, "bursts of %d with rests (longest %d, %d rests)", hard.burst_shots, longest, rests);
    // max holds the trigger
    bot_sense_reset(&bot->sense);
    bot_sense_engage(&bot->sense, &max, self, human->uid, 0.0f);
    bot->sense.reaction_left = 0.0f;
    int held = 1;
    for (int ms = 0; ms < 2000; ms += 16) {
        self->life_shots++;
        held &= bot_sense_fire(&bot->sense, &max, self, 1, 1, 1, 16.0f);
    }
    CHECK(held, "max holds the trigger");
    // the recoil climb pauses a bot that does not pull it all down
    bot_sense_reset(&bot->sense);
    bot_sense_engage(&bot->sense, &normal, self, human->uid, 0.0f);
    bot->sense.reaction_left = 0.0f;
    self->recoil_anim_y = 1.0f;
    CHECK(!bot_sense_fire(&bot->sense, &normal, self, 1, 1, 1, 16.0f), "a big climb pauses the trigger");
    self->recoil_anim_y = 0.0f;
    CHECK(bot_sense_fire(&bot->sense, &normal, self, 1, 1, 1, 16.0f), "settled: fires");
    CHECK(!bot_sense_fire(&bot->sense, &normal, self, 1, 0, 1, 16.0f), "never at a target out of sight");
    game_bots_remove_all(game);
    human->active = 0;
}

// The nav grid (bot_nav.c): Sandstorm's walkable surfaces, a route between spawns that only
// steps over walkable cells, and objective volumes (no collider) are not floors.
static void test_nav(Game *game, const int sandstorm) {
    printf("Running bot nav test...\n");
    free_players(game);
    game_init(game, sandstorm, 0, 1);
    BotNav *nav = bot_nav_for(game);
    // ~4700: the roofs are borders (500 units of wall over them, nobody stands up there), no nodes
    CHECK(nav && bot_nav_node_count(nav) > 4000, "Sandstorm grid (%d nodes)", bot_nav_node_count(nav));
    BotPath path;
    const vec3 a = game->map->spawns[0]->position, b = game->map->spawns[2]->position;
    CHECK(bot_nav_find_path(game, a, b, &path) && path.reached && path.count > 1, "route between spawns (%d points)", path.count);
    if (path.count) {
        const vec3 end = path.points[path.count - 1];
        CHECK(hypotf(end.x - b.x, end.z - b.z) < 12.0f, "the route ends at the goal (%.1f %.1f)", end.x, end.z);
    }
    // the objective volume over (202, 33, 120) has "l" and no collider: the ground is walkable
    const vec3 under = {202.4f, 33.0f, 120.0f};
    CHECK(bot_nav_find_path(game, under, b, &path) && path.count && path.points[0].y < 50.0f, "no roof on a noCol objective (%.1f)", path.count ? path.points[0].y : -1.0f);
    vec3 roam;
    CHECK(bot_nav_random_point(game, &roam), "a roam point");
}

// Navigation (shared/src/bot_path.c): 8 roaming bots on a real map for a while must keep
// reaching their goals without getting stuck against walls: counts stuck recoveries, A*
// searches, goals reached and "blocked" ticks (a bot meant to walk but did not move).
#define BOT_RECOVER_MS_DEBUG 600.0f
// fight: the bots attack each other too (Attack's strafes / dodges / retreats must keep to the
// ground as well); max_falls: bots that may fall out of the map (y <= deathY) over the run.
static void test_nav_follow(Game *game, const int map_index, const char *name, const float seconds, const int fight, const int max_falls) {
    printf("Running bot nav follow test (%s%s)...\n", name, fight ? ", fighting" : "");
    free_players(game);
    game_init(game, map_index, 0, 1);
    CHECK(game->ready, "%s loaded", name);
    if (!game->ready) return;
    const clock_t built = clock();
    const BotNav *nav = bot_nav_for(game); // built on first use: time it
    const double build_ms = (double) (clock() - built) * 1000.0 / CLOCKS_PER_SEC;
    printf("  %s: grid of %d nodes in %.1f ms\n", name, bot_nav_node_count(nav), build_ms);
    CHECK(nav && build_ms < 1000.0, "%s: the grid builds (%.1f ms)", name, build_ms);
    game->bots->nav_stats = (BotNavStats) {0};
    run(fight ? "/bot_add 8 difficulty=normal" : "/bot_add 8 difficulty=normal attackTarget=false watchTarget=false followTarget=false");
    tick(game, 1.0f, 30.0f);
    GameBots *bots = game->bots;
    int blocked = 0, walking = 0, segments = 0, with_path = 0, checks = 0, falls = 0, alive_ticks = 0, moved_ticks = 0;
    int alive[GAME_BOTS_MAX];
    for (int i = 0; i < GAME_BOTS_MAX; i++) alive[i] = i < bots->count && bots->list[i].player && bots->list[i].player->active;
    vec3 before[GAME_BOTS_MAX];
    const int ticks = (int) (seconds * 30.0f);
    for (int t = 0; t < ticks; t++) {
        int intend[GAME_BOTS_MAX], alive_before[GAME_BOTS_MAX];
        for (int i = 0; i < bots->count; i++) {
            const GameBot *bot = &bots->list[i];
            const Player *p = bot->player;
            before[i] = p ? p->position : (vec3) {0, 0, 0};
            vec3 goal;
            alive_before[i] = p && p->active;
            intend[i] = p && p->active && p->on_ground && bot->state != BOT_STATE_ATTACK && bot->follow.moving && bot->follow.recover_timer <= 0.0f && bot_path_goal(bot, &goal) && hypotf(goal.x - p->position.x, goal.z - p->position.z) > 12.0f;
        }
        const int stuck_before = bots->nav_stats.stuck;
        tick(game, 1.0f / 30.0f, 30.0f);
        { // KN_BOT_TRACE=<map>:<bot index>:<from s>:<to s>: one bot's follower state every tick (debugging)
            const char *tr = getenv("KN_BOT_TRACE");
            char tm[64]; int tb; float t0, t1;
            if (tr && sscanf(tr, "%63[^:]:%d:%f:%f", tm, &tb, &t0, &t1) == 4 && !strcmp(tm, name) && t >= t0 * 30 && t <= t1 * 30 && tb < bots->count) {
                const GameBot *bot = &bots->list[tb];
                const Player *p = bot->player;
                const vec3 wp = bot->path_index < bot->path.count ? bot->path.points[bot->path_index] : bot->roam_target;
                printf("  T %.2f pos (%.2f %.2f %.2f) vel (%.3f %.3f %.3f) gr %d st %d wp %d/%d k%d (%.1f %.1f %.1f) myaw %.2f lanes %d%d%d%d%d np %.0f bc %d rec %.0f\n", t / 30.0f,
                       p->position.x, p->position.y, p->position.z, p->velocity.x, p->velocity.y, p->velocity.z, p->on_ground, bot->state, bot->path_index, bot->path.count,
                       bot->path_index < bot->path.count ? bot->path.kinds[bot->path_index] : -1, wp.x, wp.y, wp.z, bot->follow.move_yaw,
                       bot->follow.lanes[0], bot->follow.lanes[1], bot->follow.lanes[2], bot->follow.lanes[3], bot->follow.lanes[4], bot->follow.no_progress, bot->follow.block_count, bot->follow.recover_timer);
            }
        }
        if (getenv("KN_BOT_NAV_DEBUG") && bots->nav_stats.stuck != stuck_before) {
            for (int i = 0; i < bots->count; i++) {
                const GameBot *bot = &bots->list[i];
                const Player *p = bot->player;
                if (!p || bot->follow.recover_timer < BOT_RECOVER_MS_DEBUG) continue;
                const int k = bot->path_index < bot->path.count ? bot->path.kinds[bot->path_index] : -1;
                const vec3 wp = bot->path_index < bot->path.count ? bot->path.points[bot->path_index] : bot->roam_target;
                printf("  STUCK t=%.1f bot %d at (%.1f %.1f %.1f) ground %d ladder %d vel (%.3f %.3f %.3f) wp %d/%d kind %d (%.1f %.1f %.1f) goal (%.1f %.1f %.1f) stranded %d lanes %d%d%d%d%d%d%d boxed %d\n",
                       (float) t / 30.0f, i, p->position.x, p->position.y, p->position.z, p->on_ground, p->on_ladder, p->velocity.x, p->velocity.y, p->velocity.z,
                       bot->path_index, bot->path.count, k, wp.x, wp.y, wp.z, bot->roam_target.x, bot->roam_target.y, bot->roam_target.z, bot->path_stranded,
                       bot->follow.lanes[0], bot->follow.lanes[1], bot->follow.lanes[2], bot->follow.lanes[3], bot->follow.lanes[4], bot->follow.lanes[5], bot->follow.lanes[6], bot->follow.boxed);
            }
        }
        for (int i = 0; i < bots->count; i++) { // a fall: alive before the tick, dead at or under deathY after it
            const Player *p = bots->list[i].player;
            const int now_alive = p && p->active;
            if (alive[i] && !now_alive && p && p->position.y <= game->map->death_y + 0.5f) {
                falls++;
                if (getenv("KN_BOT_NAV_DEBUG")) printf("  FALL t=%.1f bot %d from (%.1f %.1f %.1f)\n", (float) t / 30.0f, i, before[i].x, before[i].y, before[i].z);
            }
            alive[i] = now_alive;
        }
        for (int i = 0; i < bots->count; i++) { // any state: alive and moving (a frozen bot never is)
            const Player *p = bots->list[i].player;
            if (!p || !p->active || !alive_before[i]) continue;
            alive_ticks++;
            moved_ticks += hypotf(p->position.x - before[i].x, p->position.z - before[i].z) >= 0.15f;
        }
        for (int i = 0; i < bots->count; i++) {
            const GameBot *bot = &bots->list[i];
            const Player *p = bot->player;
            if (!intend[i] || !p || !p->active) continue;
            walking++;
            if (hypotf(p->position.x - before[i].x, p->position.z - before[i].z) < 0.15f) blocked++;
            if (t % 30 == 0) {
                BotPathSegment segs[BOT_PATH_SEGMENTS_MAX];
                const int n = bot_path_current(bot, segs, BOT_PATH_SEGMENTS_MAX);
                checks++;
                if (n) {
                    with_path++;
                    segments += n;
                    CHECK(fabsf(segs[0].from.x - p->position.x) < 0.01f && fabsf(segs[0].from.z - p->position.z) < 0.01f, "segment 0 starts at the feet");
                    CHECK(segs[n - 1].flags & BOT_PATH_FLAG_LAST, "the last segment is flagged");
                }
            }
        }
    }
    BotNavStats st;
    bot_path_stats(game, &st);
    const double blocked_pct = walking ? 100.0 * blocked / walking : 0.0;
    printf("  %s: %d bots x %.0f s: stuck %d, replans %d (forced %d), goals reached %d, unreachable %d, falls %d, blocked %.1f%% of %d walking ticks, path %d/%d samples (%.1f segs)\n",
           name, bots->count, seconds, st.stuck, st.replans, st.forced_replans, st.goals_reached, st.unreachable, falls, blocked_pct, walking, with_path, checks,
           with_path ? (double) segments / with_path : 0.0);
    printf("  %s: moving %d of %d alive bot-ticks (%.0f%%)\n", name, moved_ticks, alive_ticks, alive_ticks ? 100.0 * moved_ticks / alive_ticks : 0.0);
    CHECK(falls <= max_falls, "%s: %d bots fell out of the map (at most %d)", name, falls, max_falls);
    // Fights: walking counts only the ticks out of Attack, which a fight is mostly made of (a bot holds a
    // visible enemy, keeps fighting whoever hits it, and goes back to the fight after a Hide instead of
    // roaming off in Hunt), so it is no measure there. The direct one: alive bots move (in any state: walking,
    // closing in, dodging) at least 70% of their ticks in a fight (85-95% measured), 90% out of one
    // (99%). A frozen bot fails it; the nav checks below (stuck, falls, blocked, path) catch a lost one.
    if (!fight) CHECK(walking > ticks * 2.0f, "%s: the bots walked (%d walking ticks)", name, walking);
    CHECK(moved_ticks >= alive_ticks * (fight ? 0.7f : 0.9f), "%s: the bots move (%d of %d alive bot-ticks)", name, moved_ticks, alive_ticks);
    CHECK(blocked_pct < 6.0, "%s: blocked %.1f%% of walking ticks (walls)", name, blocked_pct);
    CHECK(st.stuck <= 2, "%s: %d stuck recoveries", name, st.stuck);
    run("/bot_kick all");
}

// 10 bots on Sandstorm at a 30 Hz tick: CPU per tick.
static void test_cost(Game *game, const int sandstorm) {
    printf("Running bot cost test...\n");
    free_players(game);
    game_init(game, sandstorm, 0, 1);
    CHECK(game->ready, "Sandstorm loaded");
    run("/bot_add 10 difficulty=normal");
    tick(game, 1.0f, 30.0f);
    const clock_t start = clock();
    const int ticks = 30 * 30;
    tick(game, 30.0f, 30.0f);
    const double ms = (double) (clock() - start) * 1000.0 / CLOCKS_PER_SEC / ticks;
    printf("  10 bots, Sandstorm (%zu objects): %.3f ms per 30 Hz tick (%.1f%% of a 33 ms tick), %d kills\n",
           game->map->object_count, ms, ms / 33.3 * 100.0, g_kills);
    CHECK(ms < 5.0, "bots cost %.3f ms per tick", ms);
    run("/bot_kick all");
}

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

static Player *bot_named(Game *game, const char *name) {
    for (size_t i = 0; i < game->player_count; i++) {
        if (game->players[i]->is_bot && !strcmp(game->players[i]->name, name)) return game->players[i];
    }
    return NULL;
}

// BOT_SPAWNER (v7 MapManager.addBot / addPendingBots): shared/tests/maps/v7_ai_paths.json has two.
static void test_map_bots(void) {
    printf("Running map bot (BOT_SPAWNER) test...\n");
    cJSON *map = load_json("shared/tests/maps/v7_ai_paths.json");
    CHECK(map != NULL, "v7_ai_paths.json");
    if (!map) return;
    static const cJSON *maps[1];
    maps[0] = map;
    static int modes[1] = {MODE_ID_FFA};
    Game game = {0};
    game_configure(&game, NULL, maps, 1, modes, 1, NULL, 0, NULL, 0);
    game.hooks.send = test_send;
    game_init(&game, 0, 0, 1);

    CHECK(game_bots_count(&game) == 2 && count_bots(&game, 1) == 2, "2 map bots (%d)", game_bots_count(&game));
    Player *a = bot_named(&game, "MapBotA"), *b = bot_named(&game, "MapBotB");
    CHECK(a && b, "btn names");
    if (a) {
        CHECK(a->class_index == 1 && game_bot_of(&game, a) && game_bot_of(&game, a)->team == 1, "MapBotA: btfc + btc 1, btt 1 (class %d)", a->class_index);
        CHECK(fabsf(a->position.x) < 0.01f && fabsf(a->position.z + 1500.0f) < 0.01f, "at its BOT_SPAWNER (%.1f %.1f)", a->position.x, a->position.z);
        CHECK(fabsf(a->direction.y + 3.0f * (float) M_PI / 2.0f) < 1e-4f, "d 2 faces -(d + 1) PI / 2 (%f)", a->direction.y);
    }
    // btt: the bot's team (players only take it in team modes: validTeam; this round is FFA)
    if (b) CHECK(game_bot_of(&game, b) && game_bot_of(&game, b)->team == 2 && b->god_mode, "MapBotB: btt 2, btgm");

    // a new round: last round's map bots leave, the map's join again (still 2)
    free_players(&game);
    game_init(&game, 0, 0, 1);
    tick(&game, 1.0f, 30.0f);
    CHECK(game_bots_count(&game) == 2, "re-init keeps 2 map bots (%d)", game_bots_count(&game));

    // trigger action 69: Uncle Joe at PATH_NODE 1 (-1000, 0, 0), 1000 HP
    CHECK(map_bot_spawn_at_node(&game, 1, 1) == 1, "Spawn Bot at Node");
    Player *joe = bot_named(&game, "Uncle Joe");
    CHECK(joe && joe->max_health == 1000 && fabsf(joe->position.x + 1000.0f) < 0.01f, "Uncle Joe at node 1 with 1000 HP");
    CHECK(map_bot_spawn_at_node(&game, 99, 1) == 0, "no such node");
    game_bots_remove_all(&game);
    free_players(&game);
    game_ai_free(&game);
    game_bots_free(&game);

    // config.botLimit: 20 BOT_SPAWNERs -> 16 bots
    char *text = malloc(16 * 1024);
    int n = snprintf(text, 16 * 1024, "{\"name\":\"bots\",\"spawns\":[[0,0,0,0,0,0]],\"objects\":[{\"p\":[0,-10,0],\"s\":[4000,10,4000]}");
    for (int i = 0; i < 20; i++) n += snprintf(text + n, (size_t) (16 * 1024 - n), ",{\"i\":95,\"p\":[%d,0,300],\"s\":[5,10,5]}", i * 20);
    snprintf(text + n, (size_t) (16 * 1024 - n), "]}");
    cJSON *many = cJSON_Parse(text);
    free(text);
    maps[0] = many;
    Game game2 = {0};
    game_configure(&game2, NULL, maps, 1, modes, 1, NULL, 0, NULL, 0);
    game2.hooks.send = test_send;
    game_init(&game2, 0, 0, 1);
    CHECK(game_bots_count(&game2) == 16, "botLimit 16 (%d)", game_bots_count(&game2));
    game_bots_remove_all(&game2);
    free_players(&game2);
    game_ai_free(&game2);
    game_bots_free(&game2);
    cJSON_Delete(many);
    cJSON_Delete(map);
}

// The line of fire (the user's report: bots get stuck shooting at something and dealing no damage,
// at an angle the bullets never land). Only the head of the target shows over the half wall: no shot
// may leave through the wall, checked on the look each shot was fired with (Player.shot_look, set as
// the ray leaves: aim + recoil climb + spread), and a bot locked on it with no damage landing must let
// go within BOT_HOLD_CAP_MS (plus a re-aim beat).
static void test_line_of_fire(Game *game, const int sense_map) {
    printf("Running bot line of fire test...\n");
    free_players(game);
    game_init(game, sense_map, 0, 1);
    CHECK(game->ready && game->map->object_count == 3, "Sense Arena loaded (line of fire)");
    Player *human = add_human(game, 951, "Target");
    const vec3 self_at = {200.0f, 0.0f, 400.0f}, human_at = {-200.0f, 0.0f, 400.0f};
    Object *half = game->map->objects[2];
    GameBot *bot = place_bot(game, "watchTarget=on attackTarget=on followTarget=off roam=off difficulty=hard class=0", self_at, yaw_to(self_at, human_at));
    Player *self = bot->player;
    human->position = human_at;
    const float eye_y = self->position.y + self->height - game_constants.camera_height;
    const float chest_y = bot_sense_aim_part(human, BOT_AIM_CHEST).y;
    half->scale.y = eye_y + chest_y * 0.5f + 0.3f; // the head shows over the wall, the chest and gut do not
    bot_nav_invalidate(game);
    CHECK(bot_sense_los(game, self, human) == BOT_AIM_HEAD, "only the head shows over the wall (%d)", bot_sense_los(game, self, human));
    float health = human->health, quiet = 0.0f, quiet_max = 0.0f;
    int shots = 0, through_wall = 0, in_sight = 0;
    for (int t = 0; t < 20 * 30; t++) {
        tick(game, 1.0f / 30.0f, 30.0f);
        in_sight += bot->sees_target;
        if (self->did_shoot) {
            const float dist = hypotf(hypotf(self->position.x - human->position.x, self->position.z - human->position.z), self->position.y - human->position.y);
            shots++;
            // the ray the shot left on (its centre only: the spread is already in it)
            if (!bot_sense_clear_shot(game, self, self->shot_look.y, self->shot_look.x, dist, 0.0f, human->position.y + 1.0f)) {
                through_wall++;
            }
        }
        if (human->health < health) { // a hit landed: the hold starts again
            health = human->health;
            quiet = 0.0f;
        } else if (bot->target_uid != human->uid) {
            quiet = 0.0f; // let go (moved on): nothing is being held
        } else {
            quiet += 1000.0f / 30.0f;
        }
        if (quiet > quiet_max) quiet_max = quiet;
    }
    printf("  %d shots, %d through the wall, longest hold without damage %.1f s\n", shots, through_wall, quiet_max / 1000.0f);
    CHECK(in_sight > 0, "the head is in sight for a while (%d ticks)", in_sight);
    CHECK(through_wall == 0, "no shot leaves through the wall (%d of %d)", through_wall, shots);
    CHECK(quiet_max <= BOT_HOLD_CAP_MS + 1500.0f, "the hold without damage ends within the cap (%.1f s)", quiet_max / 1000.0f);
    // the gate is no excuse not to shoot: with the wall at knee height the chest is in the open, and
    // the same bot fires at it and lands hits at this range (the ground under the target is no cover)
    game_bots_remove_all(game);
    half->scale.y = 4.0f;
    bot_nav_invalidate(game);
    human->health = (float) human->max_health;
    human->position = human_at;
    bot = place_bot(game, "watchTarget=on attackTarget=on followTarget=off roam=off difficulty=hard class=0", self_at, yaw_to(self_at, human_at));
    self = bot->player;
    int open_shots = 0, open_wall = 0;
    for (int t = 0; t < 8 * 30 && human->health > 0.0f; t++) {
        tick(game, 1.0f / 30.0f, 30.0f);
        human->position = human_at;
        if (self->did_shoot) {
            const float dist = hypotf(hypotf(self->position.x - human->position.x, self->position.z - human->position.z), self->position.y - human->position.y);
            open_shots++;
            open_wall += !bot_sense_clear_shot(game, self, self->shot_look.y, self->shot_look.x, dist, 0.0f, human->position.y + 1.0f);
        }
    }
    printf("  knee wall: %d shots, %d through the wall, target health %.0f\n", open_shots, open_wall, human->health);
    CHECK(open_shots > 0 && human->health < (float) human->max_health, "knee wall: the bot fires and hits (%d shots, health %.0f)", open_shots, human->health);
    CHECK(open_wall == 0, "knee wall: no shot leaves through the wall (%d of %d)", open_wall, open_shots);
    half->scale.y = 8.0f;
    bot_nav_invalidate(game);
    game_bots_remove_all(game);
}

// The attacker is kept (the user's report: "the bots just forgor they're shooting me and go find
// better things to do"). A player in sight keeps hitting the bot while the bot cannot hurt it (god
// mode: no damage ever lands, so the damage-less hold would end the fight after BOT_HOLD_CAP_MS):
// the bot keeps that player as its target every tick, fights it (fires), and when the player ducks
// behind the wall and goes quiet it still holds it until the memory window has passed, then lets go.
static void test_attacker_kept(Game *game, const int sense_map) {
    printf("Running bot attacker kept test...\n");
    free_players(game);
    game_init(game, sense_map, 0, 1);
    Player *human = add_human(game, 953, "Attacker");
    human->god_mode = 1;
    const vec3 self_at = {200.0f, 0.0f, 0.0f}, open_at = {-150.0f, 0.0f, -250.0f}, hidden_at = {-100.0f, 0.0f, 0.0f};
    human->position = open_at; // past the tall wall's end: in sight
    GameBot *bot = place_bot(game, "watchTarget=on attackTarget=on followTarget=off roam=on difficulty=hard class=0", self_at, yaw_to(self_at, open_at));
    Player *self = bot->player;
    CHECK(bot_sense_los(game, self, human) >= 0, "the attacker is in sight");
    const BotSenseProfile hard = bot_sense_profile(BOT_DIFFICULTY_HARD, 85.0f);
    int dropped = 0, shots = 0, hits = 0;
    const int fight_ticks = (int) ((BOT_HOLD_CAP_MS * 2.5f) / 1000.0f * 60.0f); // well past the hold cap
    for (int t = 0; t < fight_ticks; t++) {
        if (t % 30 == 0) { // a hit every 0.5 s
            self->health -= 10.0f;
            self->damage_received[0] = (PlayerDamage) {human->uid, 10.0f, game->now};
            human->life_shots += 1;
            human->did_shoot = 1;
            hits++;
        }
        tick(game, 1.0f / 60.0f, 60.0f);
        human->did_shoot = 0;
        human->position = open_at;
        self->health = (float) self->max_health; // healthy: no retreat (the next hit still reads as one)
        shots += self->did_shoot;
        if (t >= 30 && bot->target_uid != human->uid) dropped++;
    }
    CHECK(human->health >= (float) human->max_health, "no damage ever lands on the attacker (%.0f)", human->health);
    CHECK(shots > 0, "the bot fights back (%d shots)", shots);
    CHECK(dropped == 0, "%.1f s under fire with nothing landing: the attacker stays the target (%d ticks dropped of %d, %d hits)",
          (float) fight_ticks / 60.0f, dropped, fight_ticks, hits);
    // the attacker ducks behind the tall wall and goes quiet: held for the memory window, then let go
    human->position = hidden_at;
    self->position = self_at; // the dodge may have drifted it: back where the wall hides the attacker
    tick(game, 1.0f / 60.0f, 60.0f);
    CHECK(bot_sense_los(game, self, human) < 0, "behind the wall: out of sight");
    const int hold_ticks = (int) (hard.memory_ms * 0.85f / 1000.0f * 60.0f);
    int lost = 0;
    for (int t = 0; t < hold_ticks; t++) {
        tick(game, 1.0f / 60.0f, 60.0f);
        human->position = hidden_at;
        self->position = self_at; // held where the wall hides the attacker (its search would walk round it)
        if (bot->target_uid != human->uid) lost++;
    }
    CHECK(lost == 0, "out of sight and quiet: still the target through 85%% of the memory window (%d of %d ticks lost)", lost, hold_ticks);
    for (int t = 0; t < (int) (hard.memory_ms * 0.4f / 1000.0f * 60.0f); t++) {
        tick(game, 1.0f / 60.0f, 60.0f);
        human->position = hidden_at;
        self->position = self_at;
    }
    CHECK(bot->target_uid != human->uid, "let go once the memory window has passed (uid %d state %d)", bot->target_uid, bot->state);
    // low on health it breaks off (Hide, shouldRetreat) and, the retreat over with the attacker still
    // known, goes back to fighting it: never a Hunt that roams off with the attacker still its target
    human->position = open_at;
    self->health = (float) self->max_health * 0.25f;
    self->damage_received[0] = (PlayerDamage) {human->uid, 10.0f, game->now};
    tick(game, 1.0f / 60.0f, 60.0f);
    CHECK(bot->state == BOT_STATE_HIDE && bot->target_uid == human->uid, "hit when low: retreats from the attacker (state %d uid %d)", bot->state, bot->target_uid);
    self->health = (float) self->max_health; // healed while away
    int hunting_with_target = 0, back = -1;
    for (int t = 0; t < 4 * 60; t++) { // the retreat is 2.5 s (NB_RETREAT_DURATION_MS)
        if (t % 30 == 0) { // still shooting at the bot from in sight
            self->health -= 5.0f;
            self->damage_received[0] = (PlayerDamage) {human->uid, 5.0f, game->now};
        }
        tick(game, 1.0f / 60.0f, 60.0f);
        human->position = open_at;
        if (self->health < (float) self->max_health * 0.5f) self->health = (float) self->max_health;
        hunting_with_target += bot->state != BOT_STATE_HIDE && bot->state != BOT_STATE_ATTACK && bot->target_uid == human->uid;
        if (back < 0 && bot->state == BOT_STATE_ATTACK && bot->target_uid == human->uid) back = t;
    }
    CHECK(back >= 0 && hunting_with_target == 0, "the retreat over: back in the fight with the attacker (after %d ticks, %d ticks roaming with it as the target)", back, hunting_with_target);
    // still engaged with it: fighting, or a coward's short break-off (bot_attack: a hurt coward Hides)
    CHECK((bot->state == BOT_STATE_ATTACK || bot->state == BOT_STATE_HIDE) && bot->target_uid == human->uid, "and still engaged with it (state %d uid %d)", bot->state, bot->target_uid);
    game_bots_remove_all(game);
}

// A shooter that hits the bot from cover stays its threat: it keeps firing from behind the wall (every
// shot heard) and then stops, and the bot holds it for the memory window from the last shot, not
// the chase (the bot never saw it, so the lost-sight chase would otherwise end the attack after 2-4 s).
static void test_threat_memory(Game *game, const int sense_map) {
    printf("Running bot threat memory test...\n");
    free_players(game);
    game_init(game, sense_map, 0, 1);
    Player *human = add_human(game, 952, "Shooter");
    const float yaw_west = yaw_to((vec3) {200.0f, 0.0f, 0.0f}, (vec3) {-200.0f, 0.0f, 0.0f});
    GameBot *bot = place_bot(game, "watchTarget=on attackTarget=on followTarget=off roam=off difficulty=hard", (vec3) {200.0f, 0.0f, 0.0f}, yaw_west);
    Player *self = bot->player;
    human->position = (vec3) {-100.0f, 0.0f, 0.0f}; // behind the tall wall: never in sight
    tick(game, 0.5f, 60.0f);
    CHECK(bot->target_uid < 0, "behind cover: nothing known");
    self->health -= 10.0f;
    self->damage_received[0] = (PlayerDamage) {human->uid, 10.0f, game->now};
    tick(game, 0.1f, 60.0f);
    CHECK(bot->target_uid == human->uid && bot->state == BOT_STATE_ATTACK, "hit from cover: the shooter is the threat (uid %d)", bot->target_uid);
    for (int i = 0; i < 8; i++) { // 4 s of fire from cover, every shot heard
        human->life_shots += 1;
        human->did_shoot = 1;
        tick(game, 0.5f, 60.0f);
        human->did_shoot = 0;
    }
    CHECK(bot->target_uid == human->uid && bot->state == BOT_STATE_ATTACK, "4 s of fire from cover: still the shooter (uid %d state %d)", bot->target_uid, bot->state);
    const BotSenseProfile hard = bot_sense_profile(BOT_DIFFICULTY_HARD, 85.0f);
    tick(game, hard.memory_ms / 1000.0f * 0.6f, 60.0f);
    CHECK(bot->target_uid == human->uid, "60%% of the memory window after the last shot: still held (uid %d)", bot->target_uid);
    tick(game, hard.memory_ms / 1000.0f * 0.6f, 60.0f);
    CHECK(bot->target_uid < 0 && bot->state == BOT_STATE_HUNT, "dropped once the memory window has passed (uid %d state %d)", bot->target_uid, bot->state);
    game_bots_remove_all(game);
}

int main(void) {
    if (getenv("KN_BOT_SEED")) pcg32_srandom((uint64_t) atoll(getenv("KN_BOT_SEED")), 54u); // another run of the same tests
    test_parse();

    cJSON *arena = cJSON_Parse(k_arena);
    const int sandstorm_key = map_catalog_index_of_key("311_sandstorm"); // the v3.1.1 Sandstorm
    const cJSON *sandstorm = sandstorm_key >= 0 ? map_catalog_json((size_t) sandstorm_key) : NULL;
    if (!arena || !sandstorm) {
        printf("Failed to load the maps (run from the repository root)\n");
        return 1;
    }
    // the v10 official ones, then v7's SkyTemple and FRVR Strike's (a floating island: falls)
    static const char *const k_nav_maps[] = {"kanji", "sandstorm_v3", "industry", "skytemple_v2", "strike/skytemple"};
    enum { NAV_MAPS = 5 };
    static const cJSON *maps[3 + NAV_MAPS];
    maps[0] = arena;
    maps[1] = sandstorm;
    cJSON *strike_skytemple = load_json("assets/maps/strike/skytemple.json"); // not in the base catalogue
    if (strike_skytemple) map_sanitize_json(strike_skytemple);
    maps[2 + 4] = strike_skytemple;
    for (int i = 0; i < 4; i++) {
        const int key = map_catalog_index_of_key(k_nav_maps[i]);
        maps[2 + i] = key >= 0 ? map_catalog_json((size_t) key) : NULL;
        if (!maps[2 + i]) {
            printf("Failed to load %s\n", k_nav_maps[i]);
            return 1;
        }
    }
    if (!strike_skytemple) {
        printf("Failed to load assets/maps/strike/skytemple.json\n");
        return 1;
    }
    static int modes[3 + NAV_MAPS];
    cJSON *sense_arena = cJSON_Parse(k_sense_arena); // the last map: test_sense
    maps[2 + NAV_MAPS] = sense_arena;
    modes[2 + NAV_MAPS] = MODE_ID_FFA;
    modes[0] = MODE_ID_FFA;
    modes[1] = mode_find("tdm");
    for (int i = 0; i < NAV_MAPS; i++) modes[2 + i] = MODE_ID_FFA;
    Game game = {0};
    g_game = &game;
    game_configure(&game, NULL, maps, 3 + NAV_MAPS, modes, 3 + NAV_MAPS, NULL, 0, NULL, 0);
    game.hooks.send = test_send;
    game.round_length = 100000.0f;
    game_init(&game, 0, 0, 1);
    if (!game.ready) {
        printf("Failed to init the game\n");
        return 1;
    }
    g_host.game = &game;
    g_host.log = host_log;

    test_commands(&game);
    test_fight(&game);
    test_teams(&game, 1);
    test_quota(&game);
    test_quota_fill(&game, 1);
    test_quota_pace(&game, 0, 0, "arena, 6 spawns");
    test_quota_pace(&game, 3, 0, "sandstorm_v3 ffa");
    test_quota_pace(&game, 1, 1, "sandstorm tdm");
    test_sense(&game, 2 + NAV_MAPS);
    test_line_of_fire(&game, 2 + NAV_MAPS);
    test_threat_memory(&game, 2 + NAV_MAPS);
    test_attacker_kept(&game, 2 + NAV_MAPS);
    test_nav(&game, 1);
    const float nav_seconds = getenv("KN_BOT_NAV_SECONDS") ? (float) atof(getenv("KN_BOT_NAV_SECONDS")) : 45.0f;
    for (int i = 0; i < NAV_MAPS; i++) test_nav_follow(&game, 2 + i, k_nav_maps[i], nav_seconds, 0, 0);
    // fighting on the islands: Attack's movement keeps to the ground too (main had 227 falls in
    // 30 s with 6 bots on FRVR's SkyTemple, 17 on skytemple_v2)
    for (int i = 3; i < NAV_MAPS; i++) test_nav_follow(&game, 2 + i, k_nav_maps[i], nav_seconds, 1, 1);
    test_cost(&game, 1);
    test_map_bots();
    cJSON_Delete(strike_skytemple);

    if (g_failures) {
        printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("Test OK\n");
    return 0;
}
