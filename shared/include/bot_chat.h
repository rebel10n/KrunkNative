#pragma once
// Bot chat (KrunkNative addition, doc/console.md "Bots" / "Bot chat"): the bot players of
// shared/src/bots.c post short, in-character chat lines like humans do: one or two of them say
// "gg" when a match ends (each on its own delay, not every match), and a bot that just killed
// or got killed occasionally comments, rate limited. Every line is either a template of the
// bot's personality (always available: offline, standalone servers, guest lobbies) or, when the
// host provides GameHooks.bot_banter (the server linked to kn_brain, the lobby has a signed-in
// player), written by the AI gateway through kn_brain's `bot-banter` request
// (services/brain/ai.c) and passed through bot_chat_filter before it goes out; the template is
// the fallback when the AI is off, slow, errors or writes something the filter rejects.
//
// Runs on the authority only (game_is_authority); clients see ordinary NET_PACKET_CHAT lines
// (kind NET_CHAT_PLAYER, the bot's uid and name) and cannot tell them from a human's.
#include <shared.h>

#define BOT_CHAT_PERSONALITIES 6      // chill / tryhard / salty / hype / goofy / quiet
#define BOT_CHAT_END_MAX_SPEAKERS 2   // "we don't need more than one or two bots saying it"
#define BOT_CHAT_END_CHANCE 0.7f      // a match end is chatty with this chance, within the streak bounds:
#define BOT_CHAT_END_CHATTY_MAX 3     // never more than 3 chatty match ends in a row (so never every match)
#define BOT_CHAT_END_SILENT_MAX 2     // never more than 2 silent ones in a row
#define BOT_CHAT_END_DELAY_MIN 0.8f   // s after the end before a speaker's line
#define BOT_CHAT_END_DELAY_MAX 7.0f
#define BOT_CHAT_END_DELAY_GAP 0.7f   // two speakers are at least this far apart
#define BOT_CHAT_KILL_CHANCE 0.08f    // a bot comments its kill / death with this chance...
#define BOT_CHAT_DEATH_CHANCE 0.06f
#define BOT_CHAT_KILL_DELAY_MIN 0.6f  // ...this long after it (typing)
#define BOT_CHAT_KILL_DELAY_MAX 2.5f
#define BOT_CHAT_BOT_COOLDOWN 40.0f   // s between two kill / death lines of the same bot
#define BOT_CHAT_GLOBAL_COOLDOWN 10.0f // s between any two kill / death lines of all bots
#define BOT_CHAT_LINES_PER_ROUND 6    // one bot's lines per round, all kinds
#define BOT_CHAT_AI_TIMEOUT 6.0f      // s the AI answer may take past the line's delay; then the template goes
#define BOT_CHAT_MAX_PENDING 8

typedef enum { BOT_CHAT_WIN = 0, BOT_CHAT_LOSE, BOT_CHAT_DRAW, BOT_CHAT_KILL, BOT_CHAT_DEATH, BOT_CHAT_GG } BotChatEvent;

// What the AI is asked for (GameHooks.bot_banter): the answer comes back through
// bot_chat_deliver(game, id, text) (NULL text = failed: the template goes out).
typedef struct BotChatRequest_t {
    int id;                          // request id, unique per game (the reply names it)
    int bot_uid;
    char bot_name[PLAYER_NAME_SIZE];
    const char *personality;         // bot_chat_personality_name
    const char *event;               // "win" / "lose" / "draw" / "kill" / "death"
    char other[PLAYER_NAME_SIZE];    // the match winner / the victim / the killer ("" none)
    const char *mode;                // "Free for All"
    const char *map;                 // "Burg" ("" unknown)
    int kills, deaths;               // the bot's round so far
    int place, players;              // leaderboard place (1 = top), 0 = n/a
    const char *fallback;            // the template line that goes out if the AI does not answer
} BotChatRequest;

typedef struct {
    int lines;                       // bot lines sent
    int ai_lines;                    // of them written by the AI
    int ai_requests;                 // hook calls that were accepted
    int ai_timeouts;                 // ...that were not answered in time
    int ai_failed;                   // ...answered with a failure
    int ai_rejected;                 // ...answered with text the filter refused
    int ends_chatty, ends_silent;    // match ends with / without speakers
} BotChatStats;

typedef struct BotChat_t BotChat;

// Settings (persist across rounds; the server's bot_chat / bot_chat_ai keys, main.c).
void bot_chat_set_enabled(Game*, int on);        // off: no bot lines at all (default on)
int  bot_chat_enabled(const Game*);
void bot_chat_set_ai(Game*, int on);             // off: templates only, the hook is never asked (default on)
int  bot_chat_ai(const Game*);

// Authority events. game.c / player.c call these; a non-authority returns at once.
void bot_chat_on_match_end(Game*, const ModeWinner*, Player *const *sorted, size_t count);
void bot_chat_on_kill(Game*, const Player *victim, const Player *killer);
void bot_chat_tick(Game*, float delta_s);        // every game tick, also on the end screen
void bot_chat_on_init(Game*);                    // game.init: pending lines of the old round are dropped
void bot_chat_free(Game*);
// The AI's answer for request `id` (text NULL / "" = failed). 1 = the request was pending.
int  bot_chat_deliver(Game*, int id, const char *text);
const BotChatStats *bot_chat_stats(const Game*); // NULL before the first use

// The safety filter every line passes (templates included): first line only, wrapping quotes /
// a "<self_name>:" prefix / control characters dropped, whitespace collapsed, clipped to
// NET_CHAT_MAX_CHARS code points. 0 = rejected: empty, a link, a model refusal, profanity or a
// slur (plain, leetspeak, spaced-out or stretched). `out` holds the clean line on 1.
int  bot_chat_filter(const char *in, const char *self_name, char *out, size_t size);
// Personality of a bot: stable per name + uid (the seed), 0..BOT_CHAT_PERSONALITIES-1.
int  bot_chat_personality_of(const char *name, int uid);
const char *bot_chat_personality_name(int personality);
// A template line of `personality` for `event` (BOT_CHAT_GG: the shared "gg" pool), "{o}" and
// "{m}" filled with `other` / `map`. Picks with `pick` (any value; the caller's random). Returns 0
// when the personality has no line for the event (quiet bots skip most comments).
int  bot_chat_template(int personality, BotChatEvent event, unsigned pick, const char *other, const char *map, char *out, size_t size);
