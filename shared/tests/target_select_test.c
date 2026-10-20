// Player target selectors (shared/src/target_select.c) and the chat commands that use them
// (shared/src/chat_commands.c): globs, teams, bots, self, no match; god * / ungod * / kill @bots /
// kick * / give * over a game with humans and bots; a selector never widens a command's gate.
// The menu (no Player yet): /score keeps its points for the next spawn (player_keep_apply) and
// /score in game adds them now.
// Run from the repository root: ./<build>/target_select_test
#include <shared.h>
#include <bots.h>
#include <chat_commands.h>
#include <object_manager.h>
#include <target_select.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;
static char g_reply[4096];
static char g_notify[256];

#define CHECK(cond, ...) do { \
    if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_failures++; } \
} while (0)

// ---------------------------------------------------------------- the resolver

static const TargetCandidate k_cands[] = {
    {"Joel", 1, 1, 0, 1, 1},          // 0: the caller, red
    {"joel_two", 2, 2, 0, 1, 0},      // 1: blue
    {"Joelle", 3, 1, 0, 0, 0},        // 2: red, dead
    {"RedBot", 4, 1, 1, 1, 0},        // 3: red bot
    {"Blue Bot", 5, 2, 1, 1, 0},      // 4: blue bot
    {"Sam", 12, 2, 0, 1, 0},          // 5: blue
};
#define NC ((int) (sizeof(k_cands) / sizeof(k_cands[0])))

static int sel(const TargetCandidate *c, const int n, const char *q, int *out, TargetResult *r) {
    return target_select(c, n, q, 0, out, 16, r);
}

static int same(const int *got, const int n, const int *want, const int wn) {
    if (n != wn) return 0;
    for (int i = 0; i < n; i++) if (got[i] != want[i]) return 0;
    return 1;
}

#define EXPECT(q, ...) do { \
    const int want_[] = {__VA_ARGS__}; \
    int out_[16]; TargetResult r_; \
    const int n_ = sel(k_cands, NC, q, out_, &r_); \
    CHECK(r_.status == TARGET_OK && same(out_, n_, want_, (int) (sizeof(want_) / sizeof(want_[0]))), "'%s': %d matches (status %d)", q, n_, r_.status); \
} while (0)

#define EXPECT_STATUS(q, st) do { \
    int out_[16]; TargetResult r_; \
    const int n_ = sel(k_cands, NC, q, out_, &r_); \
    CHECK(r_.status == (st) && !n_, "'%s': status %d (want %d), %d matches", q, r_.status, st, n_); \
} while (0)

static void test_resolver(void) {
    printf("Running target selector test...\n");
    EXPECT("*", 0, 1, 2, 3, 4, 5);
    EXPECT(" @all ", 0, 1, 2, 3, 4, 5);
    EXPECT("@a", 0, 1, 2, 3, 4, 5);
    EXPECT("@me", 0);
    EXPECT("!", 0);
    EXPECT("@ME", 0);
    EXPECT("@others", 1, 2, 3, 4, 5);
    EXPECT("@bots", 3, 4);
    EXPECT("@humans", 0, 1, 2, 5);
    EXPECT("@team", 0, 2, 3);
    EXPECT("@enemies", 1, 4, 5);
    EXPECT("@alive", 0, 1, 3, 4, 5);
    EXPECT("@dead", 2);
    // globs: case-insensitive, as typed or normalised
    EXPECT("joel*", 0, 1, 2);
    EXPECT("JOEL*", 0, 1, 2);
    EXPECT("*bot", 3, 4);
    EXPECT("blue*", 4);
    EXPECT("blue bot*", 4);
    EXPECT("*o?l*", 0, 1, 2);
    EXPECT("joel_*", 1);
    EXPECT("s?m", 5);
    EXPECT_STATUS("zed*", TARGET_NO_MATCH);
    EXPECT_STATUS("@nobody", TARGET_NO_MATCH);
    // names and ids keep resolveOnlinePlayerByQuery
    EXPECT("joel", 0);
    EXPECT("JOEL", 0);
    EXPECT("joelt", 1);       // normalised prefix of joel_two
    EXPECT("12", 5);
    EXPECT("sa", 5);
    EXPECT("bluebot", 4);
    EXPECT("joell", 2);       // a unique prefix (Joelle)
    EXPECT_STATUS("nobody", TARGET_NOT_FOUND);
    EXPECT_STATUS("", TARGET_EMPTY);
    EXPECT_STATUS("--", TARGET_EMPTY);
    EXPECT_STATUS("bot", TARGET_NOT_FOUND); // "bot" prefixes nobody ("redbot" contains it)
    EXPECT("r", 3);
    {
        int out[16];
        TargetResult r;
        CHECK(!sel(k_cands, NC, "joe", out, &r) && r.status == TARGET_AMBIGUOUS, "joe is ambiguous (%d)", r.status);
        CHECK(!target_select(k_cands, NC, "joelt", TARGET_EXACT_ONLY, out, 16, &r) && r.status == TARGET_NOT_FOUND, "exact only (%d)", r.status);
        CHECK(target_select(k_cands, NC, "Joel", TARGET_EXACT_ONLY, out, 16, &r) == 1 && out[0] == 0, "exact only: Joel");
        CHECK(!r.selector, "a name is not a selector");
        sel(k_cands, NC, "*", out, &r);
        CHECK(r.selector && r.count == NC, "* is a selector");
        CHECK(target_select(k_cands, NC, "*", 0, out, 2, &r) == NC && r.count == NC, "count beyond max");
    }
    // the caller's own selectors without a caller (the server console)
    TargetCandidate console[NC];
    memcpy(console, k_cands, sizeof(console));
    console[0].self = 0;
    {
        int out[16];
        TargetResult r;
        CHECK(!sel(console, NC, "@me", out, &r) && r.status == TARGET_NO_SELF, "@me without a caller (%d)", r.status);
        CHECK(!sel(console, NC, "@team", out, &r) && r.status == TARGET_NO_SELF, "@team without a caller");
        CHECK(!sel(console, NC, "@enemies", out, &r) && r.status == TARGET_NO_SELF, "@enemies without a caller");
        CHECK(sel(console, NC, "@others", out, &r) == NC, "@others without a caller: everyone");
        CHECK(!sel(console, 0, "*", out, &r) && r.status == TARGET_NO_MATCH, "* over nobody");
    }
    // FFA (team 0): @team is the caller alone, @enemies everyone else
    TargetCandidate ffa[NC];
    memcpy(ffa, k_cands, sizeof(ffa));
    for (int i = 0; i < NC; i++) ffa[i].team = 0;
    {
        int out[16];
        TargetResult r;
        CHECK(sel(ffa, NC, "@team", out, &r) == 1 && out[0] == 0, "FFA @team = me");
        CHECK(sel(ffa, NC, "@enemies", out, &r) == NC - 1, "FFA @enemies = the others");
    }
    CHECK(target_is_selector("*") && target_is_selector("@bots") && target_is_selector("Joel*") && target_is_selector("!"),
          "target_is_selector");
    CHECK(!target_is_selector("Joel") && !target_is_selector("12") && !target_is_selector("tool gun"), "names are no selectors");
    CHECK(target_glob_match("a*b*c", "aXXbYc") && !target_glob_match("a*b*c", "aXXbY") && target_glob_match("*", "")
          && target_glob_match("A?C", "abc") && !target_glob_match("?", ""), "glob");
}

// ---------------------------------------------------------------- the commands

static Game *g_game;
static ChatCommandHost g_host;
#define PEERS 3
static ChatPeer g_peers[PEERS]; // the humans (uid 1..3); peer 0 is the dev
static int g_peer_count = PEERS;
static PlayerKeep g_keeps[8];   // the connections' keeps, by uid (test_menu_keep)

static void test_send(Game *game, const int uid, const int type, const void *payload, const uint16_t length) {
    (void) game; (void) uid; (void) length;
    if (type == NET_PACKET_CHAT) {
        const NetChatPacket *chat = payload;
        snprintf(g_reply, sizeof(g_reply), "%s", chat->text);
    }
    if (type == NET_PACKET_NOTIFY) snprintf(g_notify, sizeof(g_notify), "%s", ((const NetNotifyPacket *) payload)->text);
}

static void host_log(void *user, const char *text) {
    (void) user;
    snprintf(g_reply, sizeof(g_reply), "%s", text);
}

static int peer_count(void *user) {
    (void) user;
    return g_peer_count;
}

static PlayerKeep *keep_for(void *user, const int id) {
    (void) user;
    return id > 0 && id < (int) (sizeof(g_keeps) / sizeof(g_keeps[0])) ? &g_keeps[id] : NULL;
}

static int peer_at(void *user, const int index, ChatPeer *out) {
    (void) user;
    if (index < 0 || index >= PEERS) return 0;
    *out = g_peers[index];
    out->player = game_find_player(g_game, out->id);
    return 1;
}

static int g_disconnects;
static void host_disconnect(void *user, const int id, const char *message, const char *code) {
    (void) user; (void) id; (void) message; (void) code;
    g_disconnects++;
}

static const char *say(const int sender, const char *line) {
    g_reply[0] = 0;
    g_notify[0] = 0;
    chat_command_handle(&g_host, sender, line);
    return g_reply;
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

static int count_god(const Game *game) {
    int n = 0;
    for (size_t i = 0; i < game->player_count; i++) n += game->players[i] && game->players[i]->god_mode;
    return n;
}

static int count_flag(const Game *game, const int notarget) {
    int n = 0;
    for (size_t i = 0; i < game->player_count; i++) {
        const Player *p = game->players[i];
        if (p) n += notarget ? p->notarget : p->noclip;
    }
    return n;
}

static const char *k_arena =
    "{\"name\":\"Selector Arena\",\"spawns\":[[0,0,0,0,0,0],[300,0,0,0,0,0],[-300,0,0,0,0,0],[0,0,300,0,0,0],[0,0,-300,0,0,0],[250,0,250,0,0,0]],"
    "\"objects\":[{\"p\":[0,-10,0],\"s\":[1200,10,1200]}]}";

static void test_commands(void) {
    printf("Running selector command test...\n");
    cJSON *arena = cJSON_Parse(k_arena);
    static const cJSON *maps[1];
    maps[0] = arena;
    static int modes[1];
    modes[0] = MODE_ID_FFA;
    static Game game;
    g_game = &game;
    game_configure(&game, NULL, maps, 1, modes, 1, NULL, 0, NULL, 0);
    game.hooks.send = test_send;
    game.round_length = 100000.0f;
    game.is_sandbox = 1; // extra weapons for everyone (Object Manager rules)
    game_init(&game, 0, 0, 1);
    CHECK(game.ready, "game ready");
    if (!game.ready) return;

    add_human(&game, 1, "Joel");
    add_human(&game, 2, "JoelTwo");
    add_human(&game, 3, "Sam");
    g_peers[0] = (ChatPeer) {.id = 1, .name = "Joel", .address = "127.0.0.1", .ping_ms = 0, .dev = 1};
    g_peers[1] = (ChatPeer) {.id = 2, .name = "JoelTwo", .address = "203.0.113.2", .ping_ms = 0};
    g_peers[2] = (ChatPeer) {.id = 3, .name = "Sam", .address = "203.0.113.3", .ping_ms = 0};
    g_host.game = &game;
    g_host.user = NULL;
    g_host.peer_count = peer_count;
    g_host.peer_at = peer_at;
    g_host.log = host_log;
    g_host.disconnect = host_disconnect;
    g_host.perms = 1;
    g_host.cheats_open = 0;

    say(CHAT_CMD_CONSOLE, "/bot_add 3");
    CHECK(game_bots_count(&game) == 3, "3 bots (%d)", game_bots_count(&game));
    const int total = (int) game.player_count;
    CHECK(total == 6, "6 players (%d)", total);

    // god * / ungod * from the dev
    char want[64];
    snprintf(want, sizeof(want), "God mode ON: %d players", total);
    CHECK(!strcmp(say(1, "/god *"), want), "god * (%s)", g_reply);
    // /map reads a quoted name as one argument (the console completes "Soul Sanctum" with its quotes): not "Unknown map".
    // /map may add a map to the game's list (map_catalog_slot reallocs it), so the list is heap memory here.
    const cJSON **heap_maps = malloc(sizeof(*heap_maps));
    heap_maps[0] = arena;
    game.maps = heap_maps;
    game.map_count = 1;
    CHECK(!strstr(say(CHAT_CMD_CONSOLE, "/map \"Soul Sanctum\""), "Unknown map"), "/map \"Soul Sanctum\" names a map (%s)", g_reply);
    CHECK(!strstr(say(CHAT_CMD_CONSOLE, "/map Soul"), "Unknown map"), "/map Soul names a map (%s)", g_reply);
    CHECK(strstr(say(CHAT_CMD_CONSOLE, "/map \"No Such Map\""), "Unknown map") != NULL, "/map of an unknown name is refused (%s)", g_reply);
    free((void *) game.maps);
    game.maps = NULL;
    game.map_count = 0;
    CHECK(count_god(&game) == total, "god * covers everyone (%d)", count_god(&game));
    snprintf(want, sizeof(want), "God mode OFF: %d players", total);
    CHECK(!strcmp(say(1, "/ungod *"), want), "ungod * (%s)", g_reply);
    CHECK(count_god(&game) == 0, "ungod * clears everyone (%d)", count_god(&game));
    CHECK(!strcmp(say(1, "/god @bots on"), "God mode ON: 3 players") && count_god(&game) == 3, "god @bots on (%s)", g_reply);
    // a mixed lobby: god * turns the rest on (not a flip per player)
    say(1, "/god *");
    CHECK(count_god(&game) == total, "god * over a mixed lobby: all on (%d)", count_god(&game));
    say(1, "/god *");
    CHECK(count_god(&game) == 0, "god * again: all off (%d)", count_god(&game));
    CHECK(!strcmp(say(1, "/god"), "God mode ON") && game_find_player(&game, 1)->god_mode, "god: self (%s)", g_reply);
    CHECK(!strcmp(say(1, "/ungod"), "God mode OFF") && !game_find_player(&game, 1)->god_mode, "ungod: self (%s)", g_reply);
    CHECK(!strcmp(say(1, "/god sam"), "God mode ON on Sam"), "god <name> keeps its reply (%s)", g_reply);
    CHECK(!strcmp(say(1, "/god joel* off"), "God mode OFF: 2 players"), "god Joel* off (%s)", g_reply);
    CHECK(!strcmp(say(1, "/god @humans"), "God mode ON: 3 players"), "god @humans (%s)", g_reply);
    CHECK(!strcmp(say(1, "/god @me off"), "God mode OFF"), "god @me: like god (%s)", g_reply);
    CHECK(!strcmp(say(1, "/god sa* on"), "God mode ON on Sam"), "a glob matching one: the name (%s)", g_reply);
    say(1, "/ungod *");
    CHECK(!strcmp(say(1, "/god zed*"), "No players match 'zed*'."), "a glob matching nobody (%s)", g_reply);
    CHECK(!strcmp(say(1, "/god @dead"), "No players match '@dead'."), "@dead with everyone alive (%s)", g_reply);
    CHECK(!strcmp(say(1, "/god nobody"), "Player 'nobody' not found."), "a name matching nobody (%s)", g_reply);
    CHECK(!strcmp(say(1, "/god joel"), "God mode ON") && game_find_player(&game, 1)->god_mode && !game_find_player(&game, 2)->god_mode,
          "exact name beats the prefix, yourself: no suffix (%s)", g_reply);
    CHECK(!strcmp(say(CHAT_CMD_CONSOLE, "/god @me"), "'@me' needs your own player (none here)."), "console @me (%s)", g_reply);
    say(CHAT_CMD_CONSOLE, "/ungod *");
    CHECK(count_god(&game) == 0, "console ungod *");

    // other toggles
    CHECK(!strcmp(say(1, "/notarget @humans"), "Notarget ON: 3 players") && count_flag(&game, 1) == 3, "notarget @humans (%s)", g_reply);
    CHECK(!strcmp(say(1, "/notarget * off"), "Notarget OFF: 6 players") && count_flag(&game, 1) == 0, "notarget * off (%s)", g_reply);
    CHECK(!strcmp(say(1, "/noclip @others"), "Noclip ON: 5 players") && count_flag(&game, 0) == 5, "noclip @others (%s)", g_reply);
    say(1, "/noclip * off");

    // permissions: a selector never widens the gate (perms on, cheats auth)
    // (a denial answers the notify; g_reply holds the host's denial log line)
    say(2, "/god *");
    CHECK(!strcmp(g_notify, "DEV or premium custom host only") && strstr(g_reply, "perms denied") && count_god(&game) == 0,
          "non-dev god * denied (%s / %s)", g_reply, g_notify);
    say(1, "/god @bots on");
    say(2, "/ungod *");
    CHECK(g_notify[0] && count_god(&game) == 3, "non-dev ungod * denied (%d)", count_god(&game));
    say(1, "/ungod *");
    g_disconnects = 0;
    const int bots_before = game_bots_count(&game);
    say(2, "/kick *");
    CHECK(!strcmp(g_notify, "DEV only") && !g_disconnects && game_bots_count(&game) == bots_before,
          "non-dev kick * denied (%s / %s)", g_reply, g_notify);
    say(2, "/kill *");
    int alive = 0;
    for (size_t i = 0; i < game.player_count; i++) alive += game.players[i]->active;
    CHECK(g_notify[0] && alive == total, "non-dev kill * denied (%d alive)", alive);
    // give: perms off for the guest still keeps "others: devs and the host"
    g_host.perms = 0;
    CHECK(!strcmp(say(2, "/give * sniper"), "Only developers and the host can give to other players."), "guest give * refused (%s)", g_reply);
    CHECK(!strcmp(say(2, "/give @me sniper"), "Sniper Rifle added to your loadout."), "guest give @me (%s)", g_reply);
    g_host.perms = 1;

    // give * <weapon> / give <weapon> * from the dev
    snprintf(want, sizeof(want), "Gave Crossbow: %d players", total);
    CHECK(!strcmp(say(1, "/give * crossbow"), want), "give * crossbow (%s)", g_reply);
    int with = 0;
    for (size_t i = 0; i < game.player_count; i++) {
        const Player *p = game.players[i];
        for (int k = 0; p && k < p->extra_weapon_count; k++) if (p->extra_weapons[k] == 13) { with++; break; }
    }
    CHECK(with == total, "everyone holds the crossbow (%d)", with);
    CHECK(!strcmp(say(1, "/give rocket launcher @bots"), "Gave Rocket Launcher: 3 players"), "give <weapon> @bots (%s)", g_reply);
    CHECK(!strcmp(say(1, "/give crossbow sam"), "Gave Crossbow to Sam."), "give <weapon> <name> (%s)", g_reply);
    CHECK(!strcmp(say(1, "/give crossbow"), "Crossbow added to your loadout."), "give self (%s)", g_reply);

    // kill selectors, then kick @bots / kick * never kick the caller
    CHECK(!strncmp(say(1, "/kill @bots"), "Killed 3 players: ", 18), "kill @bots (%s)", g_reply);
    CHECK(!strcmp(say(1, "/god @dead"), "No spawned players match '@dead'."), "god @dead: matches are unspawned (%s)", g_reply);
    CHECK(!strcmp(say(1, "/kill @dead"), "No spawned players match '@dead'."), "kill @dead (%s)", g_reply);
    CHECK(!strcmp(say(1, "/kick @bots"), "Kicked: 3 players") && game_bots_count(&game) == 0, "kick @bots (%s)", g_reply);
    g_disconnects = 0;
    CHECK(!strcmp(say(1, "/kick @me"), "Cannot kick yourself.") && !g_disconnects, "kick @me (%s)", g_reply);
    CHECK(!strcmp(say(1, "/kick *"), "Kicked: 2 players") && g_disconnects == 2, "kick *: everyone but me (%s, %d)", g_reply, g_disconnects);
    CHECK(!strcmp(say(1, "/tp *"), "'*' matches 3 players: name one."), "tp * needs one (%s)", g_reply);

    for (size_t i = 0; i < game.player_count; i++) {
        Player *p = game.players[i];
        free(p->loadout);
        free(p->ammo);
        free(p->reloads);
        free(p->input_queue);
        free(p);
    }
    free(game.players);
    game.players = NULL;
    game.player_count = 0;
    cJSON_Delete(arena);
}

// ---------------------------------------------------------------- the menu keep (/score)

static const char *k_menu_arena =
    "{\"name\":\"Menu Arena\",\"spawns\":[[0,0,0,0,0,0],[300,0,0,0,0,0]],"
    "\"objects\":[{\"p\":[0,-10,0],\"s\":[1200,10,1200]}]}";

static void test_menu_keep(void) {
    printf("Running menu keep test...\n");
    cJSON *arena = cJSON_Parse(k_menu_arena);
    static const cJSON *maps[1];
    maps[0] = arena;
    static int modes[1];
    modes[0] = MODE_ID_FFA;
    static Game game;
    g_game = &game;
    game_configure(&game, NULL, maps, 1, modes, 1, NULL, 0, NULL, 0);
    game.hooks.send = test_send;
    game.round_length = 100000.0f;
    game.is_sandbox = 1;
    game_init(&game, 0, 0, 1);
    CHECK(game.ready, "menu game ready");
    if (!game.ready) return;

    memset(g_keeps, 0, sizeof(g_keeps));
    add_human(&game, 1, "Joel"); // in game
    // uid 4: a dev in the menu (no Player); uid 5: a guest in the menu
    g_peer_count = 3;
    g_peers[0] = (ChatPeer) {.id = 1, .name = "Joel", .address = "127.0.0.1", .ping_ms = 0, .dev = 1};
    g_peers[1] = (ChatPeer) {.id = 4, .name = "Menu", .address = "203.0.113.4", .ping_ms = 0, .dev = 1};
    g_peers[2] = (ChatPeer) {.id = 5, .name = "Guest", .address = "203.0.113.5", .ping_ms = 0};
    g_host.game = &game;
    g_host.user = NULL;
    g_host.peer_count = peer_count;
    g_host.peer_at = peer_at;
    g_host.log = host_log;
    g_host.disconnect = host_disconnect;
    g_host.keep = keep_for;
    g_host.perms = 1;
    g_host.cheats_open = 0;

    // from the menu: the amount is kept, the reply says when it applies (sums, signs)
    CHECK(!strcmp(say(4, "/score 250"), "+250 score at your next spawn (+250 waiting)."), "menu /score 250 (%s)", g_reply);
    CHECK((g_keeps[4].set & PLAYER_KEEP_SCORE) && g_keeps[4].score == 250, "the menu keeps 250 (%d)", g_keeps[4].score);
    CHECK(!game_find_player(&game, 4), "no Player in the menu");
    CHECK(!strcmp(say(4, "/score 50"), "+50 score at your next spawn (+300 waiting)."), "a second menu /score adds (%s)", g_reply);
    CHECK(!strcmp(say(4, "/score -20"), "-20 score at your next spawn (+280 waiting)."), "a negative menu /score (%s)", g_reply);
    CHECK(g_keeps[4].score == 280, "the kept sum is 280 (%d)", g_keeps[4].score);
    CHECK(!strcmp(say(4, "/score x"), "Usage: /score <amount> (default 100)."), "a bad amount is refused (%s)", g_reply);
    CHECK(g_keeps[4].score == 280, "a refused amount keeps nothing new (%d)", g_keeps[4].score);
    // the default is 100; guests and the console have no keep
    CHECK(!strcmp(say(4, "/score"), "+100 score at your next spawn (+380 waiting)."), "menu /score: the default 100 (%s)", g_reply);
    say(4, "/score -100");
    g_notify[0] = 0;
    say(5, "/score 10");
    CHECK(g_notify[0] && !(g_keeps[5].set & PLAYER_KEEP_SCORE), "a guest's menu /score is denied and keeps nothing (%s)", g_notify);
    CHECK(!strcmp(say(CHAT_CMD_CONSOLE, "/score 5"), "Score command unavailable (no active player)."), "console /score without a player (%s)", g_reply);
    // the menu's other refusals that need a live world (the audit in the report)
    CHECK(!strcmp(say(4, "/summon fiend"), "Summon unavailable (no active player)."), "menu /summon refuses (%s)", g_reply);
    CHECK(!strcmp(say(4, "/drop"), "drop needs a player: run it in game."), "menu /drop refuses (%s)", g_reply);

    // the next spawn applies the kept score once (the server's order: spawn, then the keep)
    Player *menu = player_init(&game);
    menu->uid = 4;
    snprintf(menu->name, sizeof(menu->name), "%s", "Menu");
    menu->class_index = 0;
    game_players_add(&game, menu);
    player_spawn(menu);
    const int before = menu->score;
    const unsigned int applied = player_keep_apply(menu, &g_keeps[4]);
    CHECK(applied & PLAYER_KEEP_SCORE, "the spawn applies the kept score (%u)", applied);
    CHECK(menu->score == before + 280, "the spawn adds 280 (%d -> %d)", before, menu->score);
    CHECK(!(g_keeps[4].set & PLAYER_KEEP_SCORE) && g_keeps[4].score == 0, "and clears it (set %u, %d)", g_keeps[4].set, g_keeps[4].score);
    player_keep_apply(menu, &g_keeps[4]);
    CHECK(menu->score == before + 280, "the next spawn adds nothing (%d)", menu->score);

    // in game: /score adds now, on the live Player, and keeps nothing
    const int joel = game_find_player(&game, 1)->score;
    CHECK(!strncmp(say(1, "/score 100"), "+100 score (total ", 18), "in game /score: the live reply (%s)", g_reply);
    CHECK(game_find_player(&game, 1)->score == joel + 100, "in game /score 100 adds now (%d -> %d)", joel, game_find_player(&game, 1)->score);
    CHECK(!(g_keeps[1].set & PLAYER_KEEP_SCORE), "in game keeps nothing");

    // a dead Player (not active) keeps it too, like the menu
    Player *dead = game_find_player(&game, 1);
    dead->active = 0;
    CHECK(!strcmp(say(1, "/score 7"), "+7 score at your next spawn (+7 waiting)."), "dead /score keeps (%s)", g_reply);
    dead->active = 1;
    CHECK(g_keeps[1].score == 7 && (g_keeps[1].set & PLAYER_KEEP_SCORE), "the dead Player's keep holds 7 (%d)", g_keeps[1].score);

    for (size_t i = 0; i < game.player_count; i++) {
        Player *p = game.players[i];
        if (!p) continue;
        free(p->loadout);
        free(p->ammo);
        free(p->reloads);
        free(p->input_queue);
        free(p);
    }
    free(game.players);
    game.players = NULL;
    game.player_count = 0;
    cJSON_Delete(arena);
}

int main(void) {
    test_resolver();
    test_commands();
    test_menu_keep();
    if (g_failures) {
        printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("Test OK\n");
    return 0;
}
