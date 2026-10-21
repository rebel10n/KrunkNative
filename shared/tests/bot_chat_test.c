// Bot chat (shared/src/bot_chat.c): the safety filter, the personality templates, match-end "gg"
// lines (one or two bots, distinct delays, not every match, never a long silence), the AI path
// through GameHooks.bot_banter (answer, rejected answer, timeout, hook refusal), kill / death
// comment rate limits and the off switch. Run from the repository root: ./<build>/bot_chat_test
#include <shared.h>
#include <bot_chat.h>
#include <bots.h>
#include <map_catalog.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static int g_failures;
#define CHECK(cond, ...) do { \
    if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_failures++; } \
} while (0)

typedef struct {
    int uid;
    float at;
    char text[NET_CHAT_TEXT_SIZE];
} Line;
static Line g_lines[256];
static int g_line_count;
static float g_now;
static Game *g_game;

static void test_send(Game *game, const int uid, const int type, const void *payload, const uint16_t length) {
    (void) uid; (void) length;
    if (type != NET_PACKET_CHAT) return;
    const NetChatPacket *chat = payload;
    const Player *p = game_find_player(game, chat->uid);
    if (chat->kind != NET_CHAT_PLAYER || !p || !p->is_bot) return;
    if (g_line_count >= 256) return;
    Line *l = &g_lines[g_line_count++];
    l->uid = chat->uid;
    l->at = g_now;
    snprintf(l->text, sizeof(l->text), "%s", chat->text);
}

static void tick(Game *game, const float seconds, const float hz) {
    const float dt = 1.0f / hz;
    for (float t = 0.0f; t < seconds; t += dt) {
        g_now += dt;
        game_tick(game, g_now, dt);
    }
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

// ---------------------------------------------------------------- filter + templates

static void test_filter(void) {
    printf("Running filter test...\n");
    char out[NET_CHAT_TEXT_SIZE];
    CHECK(bot_chat_filter("gg wp", NULL, out, sizeof(out)) && !strcmp(out, "gg wp"), "plain line passes (%s)", out);
    CHECK(bot_chat_filter("  \"gg that was close\"  ", NULL, out, sizeof(out)) && !strcmp(out, "gg that was close"), "quotes stripped (%s)", out);
    CHECK(bot_chat_filter("Shadow: nice shot", "Shadow", out, sizeof(out)) && !strcmp(out, "nice shot"), "own name prefix stripped (%s)", out);
    CHECK(bot_chat_filter("first line\nsecond line", NULL, out, sizeof(out)) && !strcmp(out, "first line"), "first line only (%s)", out);
    CHECK(bot_chat_filter("\n\n  gg   wp \t again", NULL, out, sizeof(out)) && !strcmp(out, "gg wp again"), "whitespace collapsed (%s)", out);
    CHECK(!bot_chat_filter("", NULL, out, sizeof(out)), "empty rejected");
    CHECK(!bot_chat_filter("   \n", NULL, out, sizeof(out)), "blank rejected");
    CHECK(!bot_chat_filter("!!!", NULL, out, sizeof(out)), "no letters rejected");
    CHECK(!bot_chat_filter("what the fuck", NULL, out, sizeof(out)), "profanity rejected");
    CHECK(!bot_chat_filter("Fuuuuck that", NULL, out, sizeof(out)), "stretched profanity rejected");
    CHECK(!bot_chat_filter("f u c k you", NULL, out, sizeof(out)), "spaced profanity rejected");
    CHECK(!bot_chat_filter("sh1t happens", NULL, out, sizeof(out)), "leetspeak rejected");
    CHECK(!bot_chat_filter("you n.i.g.g.a", NULL, out, sizeof(out)), "dotted slur rejected");
    CHECK(!bot_chat_filter("kys lol", NULL, out, sizeof(out)), "kys rejected");
    CHECK(!bot_chat_filter("kill yourself", NULL, out, sizeof(out)), "kill yourself rejected");
    CHECK(!bot_chat_filter("gg join discord.gg/abc", NULL, out, sizeof(out)), "link rejected");
    CHECK(!bot_chat_filter("see https://example.com", NULL, out, sizeof(out)), "url rejected");
    CHECK(!bot_chat_filter("As an AI language model I cannot", NULL, out, sizeof(out)), "refusal rejected");
    CHECK(!bot_chat_filter("Sure! Here's a chat message: gg", NULL, out, sizeof(out)), "assistant chatter rejected");
    CHECK(!bot_chat_filter("gg <b>wp</b>", NULL, out, sizeof(out)), "markup rejected");
    CHECK(bot_chat_filter("class act, assassin", NULL, out, sizeof(out)), "ass inside words passes");
    CHECK(bot_chat_filter("gg ez", NULL, out, sizeof(out)), "ez passes");
    CHECK(bot_chat_filter("ggs that was wild lol", NULL, out, sizeof(out)), "lol passes");
    char longline[400];
    memset(longline, 'a', sizeof(longline) - 1);
    longline[sizeof(longline) - 1] = 0;
    CHECK(bot_chat_filter(longline, NULL, out, sizeof(out)) && strlen(out) == NET_CHAT_MAX_CHARS, "clipped to %d chars (%zu)", NET_CHAT_MAX_CHARS, strlen(out));
    char utf8[400] = "";
    for (int i = 0; i < 100; i++) strcat(utf8, "\xc3\xa9"); // 100 x e-acute
    CHECK(bot_chat_filter(utf8, NULL, out, sizeof(out)) && strlen(out) == 2 * NET_CHAT_MAX_CHARS, "clipped by code points (%zu bytes)", strlen(out));
    CHECK(!bot_chat_filter("gg \xe2\x80\x9c" "fuck" "\xe2\x80\x9d", NULL, out, sizeof(out)), "profanity inside curly quotes rejected");
}

static void test_templates(void) {
    printf("Running template test...\n");
    char out[NET_CHAT_TEXT_SIZE], clean[NET_CHAT_TEXT_SIZE];
    int n = 0;
    for (int p = 0; p < BOT_CHAT_PERSONALITIES; p++) {
        for (int e = BOT_CHAT_WIN; e <= BOT_CHAT_GG; e++) {
            for (unsigned pick = 0; pick < 16; pick++) {
                if (!bot_chat_template(p, (BotChatEvent) e, pick, "Slayer_99", "Burg", out, sizeof(out))) continue;
                n++;
                CHECK(bot_chat_filter(out, "Bot", clean, sizeof(clean)), "template passes the filter: %s/%d '%s'", bot_chat_personality_name(p), e, out);
                CHECK(strlen(out) <= NET_CHAT_MAX_CHARS, "template fits: '%s'", out);
                CHECK(!strstr(out, "{"), "placeholders filled: '%s'", out);
            }
        }
        CHECK(bot_chat_template(p, BOT_CHAT_GG, 1, NULL, NULL, out, sizeof(out)), "every personality has a gg line");
    }
    CHECK(n > 100, "templates found (%d)", n);
    CHECK(bot_chat_personality_of("Slayer_99", 7) == bot_chat_personality_of("Slayer_99", 7), "personality is stable");
    int seen[BOT_CHAT_PERSONALITIES] = {0};
    for (int i = 0; i < 200; i++) {
        char name[32];
        snprintf(name, sizeof(name), "Bot_%d", i);
        seen[bot_chat_personality_of(name, i)] = 1;
    }
    for (int p = 0; p < BOT_CHAT_PERSONALITIES; p++) CHECK(seen[p], "personality %s shows up", bot_chat_personality_name(p));
}

// ---------------------------------------------------------------- the game

static int g_hook_calls, g_hook_accept, g_hook_last_id;
static char g_hook_last_event[16], g_hook_last_fallback[NET_CHAT_TEXT_SIZE];

static int test_banter(Game *game, const BotChatRequest *req) {
    (void) game;
    if (!strcmp(req->event, "kill") || !strcmp(req->event, "death")) return 0; // the fights before the end: templates
    g_hook_calls++;
    g_hook_last_id = req->id;
    snprintf(g_hook_last_event, sizeof(g_hook_last_event), "%s", req->event);
    snprintf(g_hook_last_fallback, sizeof(g_hook_last_fallback), "%s", req->fallback);
    CHECK(req->bot_name[0] && req->personality && req->mode && req->map, "request filled");
    return g_hook_accept;
}

static int bots_spawned(const Game *game) {
    int n = 0;
    for (size_t i = 0; i < game->player_count; i++) n += game->players[i]->is_bot;
    return n;
}

// One round: the bots (re)join, the round ends, the end screen runs `screen` seconds.
// Returns the bot lines of the end screen in `first` (index into g_lines).
static int round_end(Game *game, const float screen, int *first) {
    free_players(game);
    game_init(game, 0, 0, 1);
    tick(game, 1.0f, 30.0f); // the bots rejoin
    CHECK(bots_spawned(game) >= 4, "bots rejoined (%d)", bots_spawned(game));
    game_set_round_time(game, 0.5f);
    tick(game, 0.6f, 30.0f);
    CHECK(game->end_timer > 0.0f, "round over");
    *first = g_line_count;
    tick(game, screen, 30.0f);
    return g_line_count - *first;
}

static void test_match_end(Game *game) {
    printf("Running match end test...\n");
    game->hooks.bot_banter = NULL;
    g_line_count = 0;
    int chatty = 0, max_streak_c = 0, max_streak_s = 0, streak_c = 0, streak_s = 0;
    const int rounds = 16;
    for (int r = 0; r < rounds; r++) {
        int first;
        const int n = round_end(game, 9.0f, &first);
        CHECK(n <= BOT_CHAT_END_MAX_SPEAKERS, "round %d: at most %d bot lines (%d)", r, BOT_CHAT_END_MAX_SPEAKERS, n);
        if (n == 2) {
            CHECK(g_lines[first].uid != g_lines[first + 1].uid, "two different bots");
            CHECK(fabsf(g_lines[first].at - g_lines[first + 1].at) >= BOT_CHAT_END_DELAY_GAP - 0.05f, "distinct delays (%.2f vs %.2f)",
                  (double) g_lines[first].at, (double) g_lines[first + 1].at);
        }
        for (int i = first; i < first + n; i++) {
            char clean[NET_CHAT_TEXT_SIZE];
            CHECK(bot_chat_filter(g_lines[i].text, NULL, clean, sizeof(clean)), "line is clean: '%s'", g_lines[i].text);
            CHECK(strlen(g_lines[i].text) <= NET_CHAT_MAX_CHARS, "line fits");
        }
        if (n) {
            chatty++;
            streak_c++;
            streak_s = 0;
        } else {
            streak_s++;
            streak_c = 0;
        }
        if (streak_c > max_streak_c) max_streak_c = streak_c;
        if (streak_s > max_streak_s) max_streak_s = streak_s;
    }
    printf("  %d of %d match ends chatty, longest chatty streak %d, longest silent streak %d, %d lines\n", chatty, rounds, max_streak_c, max_streak_s, g_line_count);
    CHECK(chatty < rounds, "not every match end is chatty");
    CHECK(chatty > 0, "some are");
    CHECK(max_streak_c <= BOT_CHAT_END_CHATTY_MAX, "chatty streak bounded (%d)", max_streak_c);
    CHECK(max_streak_s <= BOT_CHAT_END_SILENT_MAX, "silent streak bounded (%d)", max_streak_s);
    const BotChatStats *st = bot_chat_stats(game);
    CHECK(st && st->ai_requests == 0 && st->lines == g_line_count, "no AI without a hook, every line counted");
    int ggs = 0;
    for (int i = 0; i < g_line_count; i++) ggs += strncasecmp(g_lines[i].text, "gg", 2) == 0;
    CHECK(ggs > 0, "plain gg lines (%d of %d)", ggs, g_line_count);
}

static void test_ai(Game *game) {
    printf("Running AI hook test...\n");
    game->hooks.bot_banter = test_banter;
    const BotChatStats *st = bot_chat_stats(game);

    // 1. the hook takes the request, the answer replaces the template
    g_hook_accept = 1;
    int first = 0, got = 0;
    for (int r = 0; r < 12 && !got; r++) {
        g_hook_calls = 0;
        free_players(game);
        game_init(game, 0, 0, 1);
        tick(game, 1.0f, 30.0f);
        game_set_round_time(game, 0.5f);
        tick(game, 0.6f, 30.0f);
        if (!g_hook_calls) continue;
        got = 1;
        CHECK(!strcmp(g_hook_last_event, "win") || !strcmp(g_hook_last_event, "lose") || !strcmp(g_hook_last_event, "draw"), "match end event (%s)", g_hook_last_event);
        CHECK(g_hook_last_fallback[0], "fallback given");
        CHECK(bot_chat_deliver(game, g_hook_last_id, "nice one, that was a close match"), "delivered");
        CHECK(!bot_chat_deliver(game, g_hook_last_id + 1000, "x"), "unknown id refused");
        first = g_line_count;
        tick(game, 8.0f, 30.0f);
        int found = 0;
        for (int i = first; i < g_line_count; i++) found |= !strcmp(g_lines[i].text, "nice one, that was a close match");
        CHECK(found, "the AI line went out (%d lines)", g_line_count - first);
        CHECK(st->ai_lines >= 1, "ai_lines counted (%d)", st->ai_lines);
    }
    CHECK(got, "the hook was asked within 12 rounds");

    // 2. a profane answer: the template goes out instead
    got = 0;
    for (int r = 0; r < 12 && !got; r++) {
        g_hook_calls = 0;
        free_players(game);
        game_init(game, 0, 0, 1);
        tick(game, 1.0f, 30.0f);
        game_set_round_time(game, 0.5f);
        tick(game, 0.6f, 30.0f);
        if (!g_hook_calls) continue;
        got = 1;
        const int rejected = st->ai_rejected;
        CHECK(bot_chat_deliver(game, g_hook_last_id, "what the fuck gg\nmore"), "delivered");
        CHECK(st->ai_rejected == rejected + 1, "rejected by the filter");
        first = g_line_count;
        tick(game, 8.0f, 30.0f);
        int bad = 0, fallback = 0;
        for (int i = first; i < g_line_count; i++) {
            bad |= strstr(g_lines[i].text, "fuck") != NULL;
            fallback |= !strcmp(g_lines[i].text, g_hook_last_fallback);
        }
        CHECK(!bad, "no profanity went out");
        CHECK(fallback, "the template went out instead");
    }
    CHECK(got, "the hook was asked (2)");

    // 3. no answer: the template goes out after the timeout, not before
    got = 0;
    for (int r = 0; r < 12 && !got; r++) {
        g_hook_calls = 0;
        free_players(game);
        game_init(game, 0, 0, 1);
        tick(game, 1.0f, 30.0f);
        game_set_round_time(game, 0.5f);
        tick(game, 0.6f, 30.0f);
        if (!g_hook_calls) continue;
        got = 1;
        const int timeouts = st->ai_timeouts;
        first = g_line_count;
        tick(game, BOT_CHAT_END_DELAY_MAX + BOT_CHAT_AI_TIMEOUT + 1.0f, 30.0f);
        int fallback = 0;
        for (int i = first; i < g_line_count; i++) fallback |= !strcmp(g_lines[i].text, g_hook_last_fallback);
        CHECK(fallback, "the template went out after the timeout");
        CHECK(st->ai_timeouts == timeouts + 1, "timeout counted");
    }
    CHECK(got, "the hook was asked (3)");

    // 4. the hook refuses (no brain / guests): templates, no pending AI
    g_hook_accept = 0;
    got = 0;
    for (int r = 0; r < 12 && !got; r++) {
        g_hook_calls = 0;
        const int requests = st->ai_requests;
        free_players(game);
        game_init(game, 0, 0, 1);
        tick(game, 1.0f, 30.0f);
        game_set_round_time(game, 0.5f);
        tick(game, 0.6f, 30.0f);
        if (!g_hook_calls) continue;
        got = 1;
        CHECK(st->ai_requests == requests, "a refused hook is not an AI request");
        first = g_line_count;
        tick(game, BOT_CHAT_END_DELAY_MAX + 0.5f, 30.0f);
        int fallback = 0;
        for (int i = first; i < g_line_count; i++) fallback |= !strcmp(g_lines[i].text, g_hook_last_fallback);
        CHECK(fallback, "the template went out");
    }
    CHECK(got, "the hook was asked (4)");

    // 5. ai off: the hook is never asked
    bot_chat_set_ai(game, 0);
    g_hook_accept = 1;
    g_hook_calls = 0;
    for (int r = 0; r < 6; r++) {
        int f;
        round_end(game, 8.0f, &f);
    }
    CHECK(g_hook_calls == 0, "bot_chat_ai off: no hook calls (%d)", g_hook_calls);
    bot_chat_set_ai(game, 1);
    game->hooks.bot_banter = NULL;
}

static void test_kills(Game *game) {
    printf("Running kill comment test...\n");
    free_players(game);
    game_init(game, 0, 0, 1);
    tick(game, 1.0f, 30.0f);
    Player *bots[8];
    int n = 0;
    for (size_t i = 0; i < game->player_count && n < 8; i++)
        if (game->players[i]->is_bot) bots[n++] = game->players[i];
    CHECK(n >= 4, "bots present (%d)", n);
    g_line_count = 0;
    const float start = g_now;
    // 600 kill events over 120 s of game time: the comments stay rate limited
    for (int k = 0; k < 600; k++) {
        bot_chat_on_kill(game, bots[k % n], bots[(k + 1) % n]);
        tick(game, 0.2f, 30.0f);
    }
    tick(game, 5.0f, 30.0f);
    const float span = g_now - start;
    printf("  %d kill / death lines over %.0f s\n", g_line_count, (double) span);
    CHECK(g_line_count >= 2, "some comments (%d)", g_line_count);
    CHECK(g_line_count <= (int) (span / BOT_CHAT_GLOBAL_COOLDOWN) + 1, "global cooldown (%d lines in %.0f s)", g_line_count, (double) span);
    for (int i = 1; i < g_line_count; i++)
        CHECK(g_lines[i].at - g_lines[i - 1].at >= BOT_CHAT_GLOBAL_COOLDOWN - BOT_CHAT_KILL_DELAY_MAX, "lines %d/%d spaced (%.1f s)", i - 1, i,
              (double) (g_lines[i].at - g_lines[i - 1].at));
    int per_bot[8] = {0};
    for (int i = 0; i < g_line_count; i++)
        for (int b = 0; b < n; b++) per_bot[b] += g_lines[i].uid == bots[b]->uid;
    for (int b = 0; b < n; b++) CHECK(per_bot[b] <= BOT_CHAT_LINES_PER_ROUND, "bot %d: %d lines this round", b, per_bot[b]);
    for (int i = 0; i < g_line_count; i++) {
        char clean[NET_CHAT_TEXT_SIZE];
        CHECK(bot_chat_filter(g_lines[i].text, NULL, clean, sizeof(clean)), "kill line is clean: '%s'", g_lines[i].text);
    }
    // on the end screen nobody comments kills
    game_set_round_time(game, 0.5f);
    tick(game, 0.6f, 30.0f);
    const int before = g_line_count;
    for (int k = 0; k < 100; k++) bot_chat_on_kill(game, bots[k % n], bots[(k + 1) % n]);
    tick(game, 4.0f, 30.0f);
    CHECK(g_line_count - before <= BOT_CHAT_END_MAX_SPEAKERS, "end screen: only the gg speakers (%d)", g_line_count - before);
}

// The default is off (gameserver.bot_chat false): no lines, no generation, no AI request, no state.
static void test_default_off(Game *game) {
    printf("Running default off test...\n");
    CHECK(!bot_chat_enabled(game), "bot chat is off by default");
    game->hooks.bot_banter = test_banter;
    g_hook_accept = 1;
    g_hook_calls = 0;
    g_line_count = 0;
    for (int r = 0; r < 6; r++) {
        int f;
        round_end(game, 8.0f, &f);
    }
    CHECK(g_line_count == 0, "default: no bot lines (%d)", g_line_count);
    CHECK(g_hook_calls == 0, "default: no AI request (%d)", g_hook_calls);
    const BotChatStats *st = bot_chat_stats(game);
    CHECK(!st || (st->lines == 0 && st->ai_requests == 0 && st->ends_chatty == 0 && st->ends_silent == 0), "default: nothing generated");
    game->hooks.bot_banter = NULL;
}

// The kill comments name the other player by its uid (game_send_chat_names): one "$" in the template, the name in names[0].
static int g_named_calls, g_named_bad;
static void test_chat_names(Game *game, const int speaker, const char *speaker_name, const int count, const ServerName *names, const char *template) {
    const Player *bot = game_find_player(game, speaker);
    int dollars = 0;
    for (const char *s = template; s && *s; s++) dollars += *s == '$';
    g_named_calls++;
    if (!bot || !bot->is_bot || !speaker_name || strcmp(speaker_name, bot->name) || count != 1 || dollars != 1 ||
        names[0].uid < 0 || !game_find_player(game, names[0].uid))
        g_named_bad++;
}

static void test_named_comments(Game *game) {
    printf("Running named comment test...\n");
    free_players(game);
    game_init(game, 0, 0, 1);
    tick(game, 1.0f, 30.0f);
    Player *bots[8];
    int n = 0;
    for (size_t i = 0; i < game->player_count && n < 8; i++)
        if (game->players[i]->is_bot) bots[n++] = game->players[i];
    CHECK(n >= 2, "bots present (%d)", n);
    game->hooks.chat_names = test_chat_names;
    g_named_calls = g_named_bad = 0;
    for (int k = 0; k < 300; k++) {
        bot_chat_on_kill(game, bots[k % n], bots[(k + 1) % n]);
        tick(game, 0.2f, 30.0f);
    }
    tick(game, 5.0f, 30.0f);
    game->hooks.chat_names = NULL;
    printf("  %d named comment(s)\n", g_named_calls);
    CHECK(g_named_calls >= 1, "some comments name the other player by uid (%d)", g_named_calls);
    CHECK(g_named_bad == 0, "every named comment: one name by uid and one \"$\" (%d bad)", g_named_bad);
}

// Hide player names: the AI is never given a player's name. A bot kills a human (Quentin); the request's `other` is the
// human's Player_<n>, the number its scoreboard shows, and the name never reaches the hook.
static int g_names_calls;
static char g_names_other[PLAYER_NAME_SIZE];
static int test_banter_names(Game *game, const BotChatRequest *req) {
    if (strcmp(req->event, "kill") && strcmp(req->event, "death")) return 0;
    for (size_t i = 0; i < game->player_count; i++) { // the bots fight each other in the tick: only the human's kills count
        const Player *p = game->players[i];
        if (p->is_bot && !strcmp(p->name, req->other)) return 0;
    }
    g_names_calls++;
    snprintf(g_names_other, sizeof(g_names_other), "%s", req->other);
    return 1;
}

static void test_ai_names(Game *game) {
    printf("Running AI player names test...\n");
    free_players(game);
    game_init(game, 0, 0, 1);
    tick(game, 1.0f, 30.0f);
    Player *bot = NULL;
    for (size_t i = 0; i < game->player_count && !bot; i++)
        if (game->players[i]->is_bot) bot = game->players[i];
    CHECK(bot != NULL, "a bot present");
    Player *human = player_init(game);
    CHECK(human != NULL, "a human player");
    if (!bot || !human) return;
    human->uid = 9001;
    snprintf(human->name, sizeof(human->name), "%s", "Quentin");
    game_players_add(game, human);
    game->hooks.bot_banter = test_banter_names;
    g_names_calls = 0;
    g_names_other[0] = 0;
    for (int k = 0; k < 400 && !g_names_calls; k++) {
        bot_chat_on_kill(game, bot, human);
        tick(game, 0.2f, 30.0f);
    }
    CHECK(g_names_calls >= 1, "the AI was asked about the kill (%d)", g_names_calls);
    CHECK(!strstr(g_names_other, "Quentin"), "the AI is not given the player's name: '%s'", g_names_other);
    int digits = !strncmp(g_names_other, "Player_", 7) && g_names_other[7] != 0;
    for (const char *d = g_names_other + 7; digits && *d; d++) digits = *d >= '0' && *d <= '9';
    CHECK(digits, "the AI names the player Player_<n>: '%s'", g_names_other);
    tick(game, BOT_CHAT_KILL_DELAY_MAX + BOT_CHAT_AI_TIMEOUT + 1.0f, 30.0f); // the unanswered request goes out as a template
    game->hooks.bot_banter = NULL;
}

static void test_off(Game *game) {
    printf("Running off switch test...\n");
    bot_chat_set_enabled(game, 0);
    game->hooks.bot_banter = test_banter;
    g_hook_accept = 1;
    g_hook_calls = 0;
    g_line_count = 0;
    for (int r = 0; r < 6; r++) {
        int f;
        round_end(game, 8.0f, &f);
    }
    Player *bots[2] = {NULL, NULL};
    int n = 0;
    for (size_t i = 0; i < game->player_count && n < 2; i++)
        if (game->players[i]->is_bot) bots[n++] = game->players[i];
    for (int k = 0; n == 2 && k < 200; k++) {
        bot_chat_on_kill(game, bots[k % 2], bots[(k + 1) % 2]);
        tick(game, 0.2f, 30.0f);
    }
    CHECK(g_line_count == 0, "bot_chat off: no lines (%d)", g_line_count);
    CHECK(g_hook_calls == 0, "bot_chat off: no AI request (%d)", g_hook_calls);
    game->hooks.bot_banter = NULL;
    bot_chat_set_enabled(game, 1);
}

int main(void) {
    test_filter();
    test_templates();

    const int sandstorm_key = map_catalog_index_of_key("311_sandstorm");
    const cJSON *sandstorm = sandstorm_key >= 0 ? map_catalog_json((size_t) sandstorm_key) : NULL;
    if (!sandstorm) {
        printf("Failed to load Sandstorm (run from the repository root)\n");
        return 1;
    }
    static const cJSON *maps[1];
    maps[0] = sandstorm;
    static int modes[1];
    modes[0] = MODE_ID_FFA;
    Game game = {0};
    g_game = &game;
    game_configure(&game, NULL, maps, 1, modes, 1, NULL, 0, NULL, 0);
    game.hooks.send = test_send;
    game.round_length = 100000.0f;
    game_init(&game, 0, 0, 1);
    if (!game.ready) {
        printf("Failed to init the game\n");
        return 1;
    }
    for (int i = 0; i < 5; i++) CHECK(game_bots_add(&game, NULL, NULL, 0.0f) != NULL, "bot %d added", i);
    tick(&game, 1.0f, 30.0f);
    CHECK(bots_spawned(&game) == 5, "5 bots in the game (%d)", bots_spawned(&game));

    test_default_off(&game);
    bot_chat_set_enabled(&game, 1); // on (gameserver.bot_chat true): the lines come back
    CHECK(bot_chat_enabled(&game), "on: enabled");
    test_match_end(&game);
    test_ai(&game);
    test_kills(&game);
    test_named_comments(&game);
    test_ai_names(&game);
    test_off(&game);

    game_bots_remove_all(&game);
    bot_chat_free(&game);
    if (g_failures) {
        printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("Test OK\n");
    return 0;
}
