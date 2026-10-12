// Krunker v7.1.4 bot players (shared/src/bots.c) through the shared game hooks: option parsing
// (parseBotConfigAssignments), names.js, the bot_* console commands and their replies, bots as
// real players (SKINS / SPAWN / PLAYER_INFO / LEAVE, leaderboard, kills), the AI fighting,
// bot_stop / bot_kill / respawn, teams, map changes, the server.cfg quota and the per-tick cost.
// Run from the repository root: ./<build>/bot_test
#include <shared.h>
#include <bots.h>
#include <chat_commands.h>
#include <map_catalog.h>
#include <math.h>
#include <stdio.h>
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
    memcpy(packet->payload, payload, length);
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

    run("/bot_mimic on");
    CHECK(!strcmp(g_reply, "Mimic-style follow mode set (3 live bots updated)."), "bot_mimic (%s)", g_reply);
    CHECK(bots->list[0].config.mimic_target && !bots->list[0].config.attack_target, "mimic config");
    run("/bot_mimic off");
    CHECK(!strcmp(g_reply, "Combat follow mode set (3 live bots updated)."), "bot_mimic off (%s)", g_reply);

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

// The nav grid (bot_nav.c): Sandstorm's walkable surfaces, a route between spawns that only
// steps over walkable cells, and objective volumes (no collider) are not floors.
static void test_nav(Game *game, const int sandstorm) {
    printf("Running bot nav test...\n");
    free_players(game);
    game_init(game, sandstorm, 0, 1);
    BotNav *nav = bot_nav_for(game);
    CHECK(nav && bot_nav_node_count(nav) > 5000, "Sandstorm grid (%d nodes)", bot_nav_node_count(nav));
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

int main(void) {
    test_parse();

    cJSON *arena = cJSON_Parse(k_arena);
    const int sandstorm_key = map_catalog_index_of_key("sandstorm");
    const cJSON *sandstorm = sandstorm_key >= 0 ? map_catalog_json((size_t) sandstorm_key) : NULL;
    if (!arena || !sandstorm) {
        printf("Failed to load the maps (run from the repository root)\n");
        return 1;
    }
    static const cJSON *maps[2];
    maps[0] = arena;
    maps[1] = sandstorm;
    static int modes[2];
    modes[0] = MODE_ID_FFA;
    modes[1] = mode_find("tdm");
    Game game = {0};
    g_game = &game;
    game_configure(&game, NULL, maps, 2, modes, 2, NULL, 0, NULL, 0);
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
    test_nav(&game, 1);
    test_cost(&game, 1);

    if (g_failures) {
        printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("Test OK\n");
    return 0;
}
