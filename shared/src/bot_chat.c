// Bot chat (shared/include/bot_chat.h): bots say gg at the end of a match and comment the odd
// kill / death, templated per personality or written by the AI through GameHooks.bot_banter.
//
// Match end (user, 2026-10-14: "some bots should be able to say gg, we don't need more than one
// or two bots saying it and it should be at different intervals"): a match end is chatty with
// BOT_CHAT_END_CHANCE, bounded so a lobby is never silent for long and never chatty every match
// (BOT_CHAT_END_CHATTY_MAX / BOT_CHAT_END_SILENT_MAX in a row). A chatty end picks one or two
// bots (personality-weighted), each with its own delay at least BOT_CHAT_END_DELAY_GAP apart:
// the first says a plain "gg" (always a template), the second (or the lone speaker half the
// time) a win / lose / draw line, through the AI when the host offers it.
//
// Kills / deaths: the killer bot (or the victim bot) comments with a small chance, at most one
// line per BOT_CHAT_GLOBAL_COOLDOWN across all bots and one per BOT_CHAT_BOT_COOLDOWN per bot,
// BOT_CHAT_LINES_PER_ROUND lines per bot per round.
//
// Lines go out through game_send(NET_PACKET_CHAT) as the bot: the same packet a human's chat is
// relayed with. Every line (AI or template) passes bot_chat_filter first.
#include <bot_chat.h>
#include <modes.h>
#include <pcg_basic.h>

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define BOT_CHAT_SPEAKERS_MAX 64

typedef struct {
    int id;
    int bot_uid;
    int round;                       // the round it was queued in
    float due;                       // game->now the line may go out
    float deadline;                  // AI: the template goes out at max(due, deadline) without an answer
    unsigned char ai:1, answered:1, from_ai:1;
    char text[NET_CHAT_TEXT_SIZE];   // the line (the template until the AI answers)
} BotChatPending;

typedef struct {
    int uid;
    int personality;
    float last_line;                 // game->now of the last kill / death line (-1 none)
    int lines_round;
} BotChatSpeaker;

struct BotChat_t {
    pcg32_random_t rng;
    int next_id;
    int round;
    unsigned char enabled:1, ai:1, init_done:1;
    BotChatPending pending[BOT_CHAT_MAX_PENDING];
    int pending_count;
    BotChatSpeaker speakers[BOT_CHAT_SPEAKERS_MAX];
    int speaker_count;
    float last_global_line;          // kill / death lines of all bots (-1 none)
    int chatty_streak, silent_streak;
    BotChatStats stats;
};

// ---------------------------------------------------------------- personalities + templates

static const char *const k_personality_names[BOT_CHAT_PERSONALITIES] = {"chill", "tryhard", "salty", "hype", "goofy", "quiet"};
static const float k_personality_weight[BOT_CHAT_PERSONALITIES] = {1.0f, 1.2f, 1.0f, 1.5f, 1.2f, 0.4f};

#define T(...) ((const char *const[]) {__VA_ARGS__, NULL})

// [personality][event] -> NULL-terminated lines; "{o}" the other player, "{m}" the map
static const char *const *const k_lines[BOT_CHAT_PERSONALITIES][5] = {
    { // chill
        T("gg that was fun", "nice games everyone", "gg, close one", "good games all", "gg, {m} is a good one"),
        T("gg, you guys were cracked", "gg wp, next one", "gg that one got away", "ggs, fun match"),
        T("gg, a draw lol", "even match, gg", "gg, nobody wins on {m}"),
        T("sorry {o} lol", "oops my bad {o}", "nice fight {o}", "gg {o}"),
        T("nice shot {o}", "ok that was clean {o}", "lol got me {o}", "wp {o}"),
    },
    { // tryhard
        T("gg ez", "too easy", "gg, never in doubt", "carried", "gg, free"),
        T("gg, lag", "gg my aim was off", "rematch?", "ggs, not my day", "gg, my ping says hi"),
        T("draw?? rematch", "gg, we go again", "a draw, really"),
        T("sit {o}", "get good {o}", "ez {o}", "1 tap {o}", "{o} too slow"),
        T("lucky {o}", "{o} camping lol", "wait what {o}", "{o} nice spray i guess"),
    },
    { // salty
        T("gg finally", "about time", "gg i guess", "gg, took long enough"),
        T("gg this map is cursed", "gg spawns were trash", "ggs, lagged all game", "gg, whatever", "gg, {m} hates me"),
        T("gg a draw, great", "ugh draw", "a draw. on {m}. of course"),
        T("finally {o}", "stay down {o}", "took long enough {o}"),
        T("really {o}?", "{o} again?!", "ok {o} that's enough", "this game {o}", "{o} where did you come from"),
    },
    { // hype
        T("GG LETS GOOO", "WE WON GG", "gg that was insane", "GGS!! what a game", "GG {m} IS THE BEST MAP"),
        T("GG great match!", "ggs that was close!", "gg you guys are good!", "GG we go next!"),
        T("GG what a tie!", "draw?! gg that was wild", "GG both teams!!"),
        T("LETS GO {o}", "boom {o}!", "got em {o}!", "{o} down!"),
        T("nooo {o}", "ok nice {o}!!", "{o} what was that shot", "ouch {o}"),
    },
    { // goofy
        T("gg i was lagging and still won", "gg my cat was helping", "gg, i pressed buttons", "winner winner", "gg i have no idea what happened"),
        T("gg i blame the gravity", "gg my mouse ran out of mouse", "ggs i was playing with my feet", "gg, {m} moved"),
        T("gg a draw, everyone wins", "tie game, we all lose", "gg nobody won, classic"),
        T("oops {o}", "that one was for the cat {o}", "sorry {o} lol", "{o} i am so sorry"),
        T("{o} why", "{o} rude", "i tripped {o}", "{o} i was afk", "{o} my screen went dark"),
    },
    { // quiet
        T("gg", "gg wp"),
        T("gg", "ggs"),
        T("gg"),
        T("sorry {o}"),
        T("nice {o}"),
    },
};

static const char *const k_gg[] = {"gg", "GG", "gg wp", "ggwp", "ggs", "gg all", "gg guys", "gg", "gg wp all", "GGs", NULL};
static const char *const k_gg_cocky[] = {"gg ez", "ez gg", "gg", "ggs", NULL}; // tryhard / salty

const char *bot_chat_personality_name(const int personality) {
    return k_personality_names[personality >= 0 && personality < BOT_CHAT_PERSONALITIES ? personality : 0];
}

int bot_chat_personality_of(const char *name, const int uid) {
    uint32_t h = 2166136261u; // FNV-1a over the name, then the uid
    for (const unsigned char *p = (const unsigned char *) (name ? name : ""); *p; p++) h = (h ^ *p) * 16777619u;
    h = (h ^ (uint32_t) uid) * 16777619u;
    return (int) ((h >> 8) % BOT_CHAT_PERSONALITIES);
}

static size_t lines_count(const char *const *lines) {
    size_t n = 0;
    while (lines && lines[n]) n++;
    return n;
}

static void fill_placeholders(const char *in, const char *other, const char *map, char *out, const size_t size) {
    size_t o = 0;
    for (const char *p = in; *p && o + 1 < size; ) {
        const char *sub = NULL;
        if (!strncmp(p, "{o}", 3)) sub = other && other[0] ? other : "you";
        else if (!strncmp(p, "{m}", 3)) sub = map && map[0] ? map : "this map";
        if (sub) {
            const size_t n = strlen(sub);
            const size_t room = size - 1 - o;
            memcpy(out + o, sub, n < room ? n : room);
            o += n < room ? n : room;
            p += 3;
        } else out[o++] = *p++;
    }
    out[o] = 0;
}

int bot_chat_template(const int personality, const BotChatEvent event, const unsigned pick, const char *other, const char *map, char *out, size_t size) {
    const int p = personality >= 0 && personality < BOT_CHAT_PERSONALITIES ? personality : 0;
    const char *const *lines;
    if (event == BOT_CHAT_GG) lines = (p == 1 || p == 2) && (pick & 4) ? k_gg_cocky : k_gg;
    else if (event >= BOT_CHAT_WIN && event <= BOT_CHAT_DEATH) lines = k_lines[p][event];
    else return 0;
    const size_t n = lines_count(lines);
    if (!n || !size) return 0;
    fill_placeholders(lines[pick % n], other, map, out, size);
    return out[0] != 0;
}

// ---------------------------------------------------------------- filter

// Whole words after normalisation (lowercase, leetspeak undone, stretched letters squeezed). A
// bot line the filter refuses only costs a template, so the list errs on the side of refusing.
static const char *const k_bad_words[] = {
    "fuck", "fucking", "fucker", "fuckers", "fucked", "fuk", "fck", "fk", "motherfucker", "mf", "wtf", "stfu", "af",
    "shit", "shitty", "shite", "bullshit", "bs", "bitch", "bitches", "biatch", "ass", "asshole", "arse", "cunt", "cunts",
    "dick", "dicks", "dickhead", "cock", "cocks", "pussy", "pussies", "whore", "whores", "slut", "sluts", "bastard",
    "twat", "wanker", "prick", "douche", "douchebag", "jackass", "damn", "goddamn", "crap", "piss", "pissed",
    "sex", "sexy", "porn", "penis", "vagina", "boobs", "tits", "nude", "nudes", "cum", "jizz", "horny", "rape", "raped", "rapist",
    "nazi", "nazis", "hitler", "terrorist", "jihad", "isis", "kys", "suicide", "cancer", "retard", "retarded", "retards",
    "fag", "fags", "faggot", "faggots", "nigga", "niggas", "nigger", "niggers", "kike", "kikes", "spic", "spics", "chink", "chinks",
    "tranny", "trannies", "dyke", "dykes", "homo", "homos", "gay", "gays", "lesbian", "queer", "jew", "jews", "muslim", "muslims",
    "christian", "christians", "black", "blacks", "white", "whites", "race", "racist", "racism", "sexist", "drugs", "weed", "cocaine",
    "meth", "heroin", "alcohol", "drunk", "vodka", "beer", "casino", "bet", "bets", "gamble", "gambling", "discord", "youtube", "twitch",
    "tiktok", "instagram", "snapchat", "onlyfans", "subscribe", "follow", "giveaway", "hack", "hacks", "hacker", "hacking",
    "cheat", "cheats", "cheater", "cheating", "aimbot", "aimbots", "wallhack", "wallhacks", "ban", "banned", "report", "reported", "ip", "dox",
    "doxx", "doxxed", "address", "password", "login", "account", "trump", "biden", "putin", "god", "jesus", "allah", "bible", "quran", "satan",
    "devil", "kill yourself", "kill urself", "kill ur self",
};
// Anywhere in the letters-only form (so "n.i.g.g.a" and "f a g g o t" are caught too).
static const char *const k_bad_anywhere[] = {
    "nigg", "n1gg", "nigga", "fagg", "faggot", "kike", "chink", "tranny", "retard", "rapist", "hitler", "nazi", "killyourself",
    "killurself", "cunt", "motherfuck", "fuckyou", "fucku", "fuckoff", "fuckthis", "fuckthat", "wankr", "whore", "slut",
};
// The model talking about itself instead of playing the part.
static const char *const k_refusals[] = {
    "as an ai", "language model", "i can't", "i cannot", "i can not", "i'm sorry", "i am sorry", "i'm not able", "i am not able",
    "i won't", "i will not", "assistant", "openai", "anthropic", "as a bot", "chat message", "here's", "here is", "sure!", "sure,",
    "certainly", "of course!", "note:", "system prompt", "instruction", "<", ">", "[", "]", "{", "}", "http", "www.", ".com", ".io",
    ".gg/", ".net", ".org", ".tv", "://", "@", "#",
};

static char leet(const unsigned char c) {
    switch (c) {
    case '0': return 'o';
    case '1': return 'i';
    case '3': return 'e';
    case '4': return 'a';
    case '5': return 's';
    case '7': return 't';
    case '8': return 'b';
    case '@': return 'a';
    case '$': return 's';
    case '|': return 'l';
    case '+': return 't';
    default: return (char) tolower(c);
    }
}

static int word_bad(const char *word) {
    if (!word[0]) return 0;
    for (size_t i = 0; i < sizeof(k_bad_words) / sizeof(k_bad_words[0]); i++)
        if (!strcmp(word, k_bad_words[i])) return 1;
    return 0;
}

static int anywhere_bad(const char *letters) {
    for (size_t i = 0; i < sizeof(k_bad_anywhere) / sizeof(k_bad_anywhere[0]); i++)
        if (strstr(letters, k_bad_anywhere[i])) return 1;
    return 0;
}

// Tokens of the normalised text, plain and with stretched letters squeezed ("fuuuck" -> "fuck";
// runs of three or more only, so "good" stays "good").
static int words_bad(const char *text) {
    char word[80], squeezed[80];
    size_t n = 0, s = 0;
    for (const unsigned char *p = (const unsigned char *) text;; p++) {
        if (*p && !isspace(*p)) {
            if (n + 1 < sizeof(word)) {
                size_t run = 1;
                while (run <= n && word[n - run] == (char) *p) run++;
                if (run <= 2) squeezed[s++] = (char) *p; // "oo" stays
                else if (run == 3) s--;                   // "ooo"...: one letter
                word[n++] = (char) *p;
            }
            continue;
        }
        word[n] = 0;
        squeezed[s] = 0;
        if (word_bad(word) || word_bad(squeezed)) return 1;
        n = s = 0;
        if (!*p) break;
    }
    return 0;
}

int bot_chat_filter(const char *in, const char *self_name, char *out, const size_t size) {
    if (!out || size < 2) return 0;
    out[0] = 0;
    if (!in) return 0;

    // the first non-empty line
    const char *p = in;
    while (*p && (isspace((unsigned char) *p) || *p == '\r' || *p == '\n')) p++;
    const char *end = p;
    while (*end && *end != '\n' && *end != '\r') end++;
    while (end > p && isspace((unsigned char) end[-1])) end--;

    // a "Name:" / "Name -" prefix the model may add, wrapping quotes, a list dash
    if (self_name && self_name[0]) {
        const size_t sl = strlen(self_name);
        if ((size_t) (end - p) > sl + 1 && !strncasecmp(p, self_name, sl) && (p[sl] == ':' || (p[sl] == ' ' && p[sl + 1] == '-'))) {
            p += sl + (p[sl] == ':' ? 1 : 2);
            while (p < end && isspace((unsigned char) *p)) p++;
        }
    }
    for (int again = 1; again;) {
        again = 0;
        if (end - p >= 2 && (*p == '"' || *p == '\'' || *p == '`') && end[-1] == *p) {
            p++;
            end--;
            again = 1;
        }
        if (end - p >= 6 && !strncmp(p, "\xe2\x80\x9c", 3) && !strncmp(end - 3, "\xe2\x80\x9d", 3)) { // “ ”
            p += 3;
            end -= 3;
            again = 1;
        }
        if (end - p >= 2 && (*p == '-' || *p == '*' || *p == '>') && p[1] == ' ') {
            p += 2;
            again = 1;
        }
        while (p < end && isspace((unsigned char) *p)) p++;
        while (end > p && isspace((unsigned char) end[-1])) end--;
    }
    if (p >= end) return 0;

    // copy: control characters out, whitespace collapsed, clipped to NET_CHAT_MAX_CHARS code points
    size_t o = 0, chars = 0;
    int space = 1;
    for (const char *c = p; c < end && o + 4 < size; c++) {
        const unsigned char b = (unsigned char) *c;
        if (b < 0x20 || b == 0x7f) continue;
        if (b == ' ' || b == '\t') {
            if (space) continue;
            space = 1;
        } else space = 0;
        if ((b & 0xc0) != 0x80) { // a code point starts
            if (chars >= NET_CHAT_MAX_CHARS) break;
            chars++;
        }
        out[o++] = (char) b;
    }
    while (o && out[o - 1] == ' ') o--;
    out[o] = 0;
    if (!o) return 0;

    // refusals, links, markup
    char lower[NET_CHAT_TEXT_SIZE];
    for (size_t i = 0; i <= o && i < sizeof(lower) - 1; i++) lower[i] = (char) tolower((unsigned char) out[i]);
    lower[o < sizeof(lower) - 1 ? o : sizeof(lower) - 1] = 0;
    for (size_t i = 0; i < sizeof(k_refusals) / sizeof(k_refusals[0]); i++)
        if (strstr(lower, k_refusals[i])) return out[0] = 0, 0;

    // profanity: normalised words and the letters-only form
    char norm[NET_CHAT_TEXT_SIZE], letters[NET_CHAT_TEXT_SIZE];
    size_t n = 0, l = 0, text_chars = 0;
    for (size_t i = 0; i < o; i++) {
        const unsigned char b = (unsigned char) out[i];
        if (b >= 0x80) { // non-ASCII: text, but ignored by the word checks (accents cannot hide a slur from the letters-only form)
            text_chars++;
            continue;
        }
        const char c = leet(b);
        if (c >= 'a' && c <= 'z') {
            norm[n++] = c;
            letters[l++] = c;
        } else if (n && norm[n - 1] != ' ') norm[n++] = ' ';
    }
    norm[n] = 0;
    letters[l] = 0;
    if (words_bad(norm) || anywhere_bad(letters)) return out[0] = 0, 0;
    if (!l && !text_chars) return out[0] = 0, 0; // nothing to say ("!!!", "...")
    return 1;
}

// ---------------------------------------------------------------- state

static BotChat *bot_chat_get(Game *game, const int create) {
    if (!game) return NULL;
    if (!game->bot_chat && create) {
        BotChat *bc = calloc(1, sizeof(*bc));
        if (!bc) return NULL;
        bc->enabled = 1;
        bc->ai = 1;
        bc->next_id = 1;
        bc->last_global_line = -1000.0f;
        pcg32_srandom_r(&bc->rng, (uint64_t) time(NULL) ^ ((uint64_t) (uintptr_t) game << 16), (uint64_t) (uintptr_t) game | 1u);
        game->bot_chat = bc;
    }
    return game->bot_chat;
}

static float frand(BotChat *bc) { return (float) ldexp((double) pcg32_random_r(&bc->rng), -32); }
static float frange(BotChat *bc, const float lo, const float hi) { return lo + (hi - lo) * frand(bc); }

void bot_chat_set_enabled(Game *game, const int on) {
    BotChat *bc = bot_chat_get(game, 1);
    if (bc) bc->enabled = (unsigned char) !!on;
}
int bot_chat_enabled(const Game *game) { return !game || !game->bot_chat || game->bot_chat->enabled; }
void bot_chat_set_ai(Game *game, const int on) {
    BotChat *bc = bot_chat_get(game, 1);
    if (bc) bc->ai = (unsigned char) !!on;
}
int bot_chat_ai(const Game *game) { return !game || !game->bot_chat || game->bot_chat->ai; }
const BotChatStats *bot_chat_stats(const Game *game) { return game && game->bot_chat ? &game->bot_chat->stats : NULL; }

void bot_chat_free(Game *game) {
    if (!game) return;
    free(game->bot_chat);
    game->bot_chat = NULL;
}

void bot_chat_on_init(Game *game) {
    BotChat *bc = game ? game->bot_chat : NULL;
    if (!bc) return;
    bc->round++;
    bc->pending_count = 0; // the old round's lines (and AI answers still in flight) are dropped
    for (int i = 0; i < bc->speaker_count; i++) bc->speakers[i].lines_round = 0;
    bc->last_global_line = -1000.0f;
}

static BotChatSpeaker *speaker_of(BotChat *bc, const Player *p) {
    for (int i = 0; i < bc->speaker_count; i++)
        if (bc->speakers[i].uid == p->uid) return &bc->speakers[i];
    if (bc->speaker_count >= BOT_CHAT_SPEAKERS_MAX) { // a long-lived lobby: recycle the oldest slot
        memmove(bc->speakers, bc->speakers + 1, sizeof(bc->speakers[0]) * (BOT_CHAT_SPEAKERS_MAX - 1));
        bc->speaker_count--;
    }
    BotChatSpeaker *s = &bc->speakers[bc->speaker_count++];
    s->uid = p->uid;
    s->personality = bot_chat_personality_of(p->name, p->uid);
    s->last_line = -1000.0f;
    s->lines_round = 0;
    return s;
}

static const char *game_mode_label(const Game *game) {
    if (game->mode && game->mode->config.name) return game->mode->config.name;
    if (game->modes && game->mode_count) return mode_name(game->modes[game->current_mode_index % (int) game->mode_count]);
    return "Free for All";
}

static const char *game_map_label(const Game *game) {
    return game->map && game->map->name ? game->map->name : "";
}

static void send_line(Game *game, const int uid, const char *text) {
    const Player *p = game_find_player(game, uid);
    if (!p || !p->is_bot) return; // it left meanwhile
    char clean[NET_CHAT_TEXT_SIZE];
    if (!bot_chat_filter(text, p->name, clean, sizeof(clean))) return;
    NetChatPacket chat;
    net_packet_chat(&chat, uid, NET_CHAT_PLAYER, p->name, clean);
    game_send(game, NET_TARGET_ALL, NET_PACKET_CHAT, &chat, sizeof(chat));
}

// Queue a line of `bot`: the template now, the AI's answer later when the hook takes the request.
static void queue_line(Game *game, BotChat *bc, const Player *bot, BotChatSpeaker *speaker, const BotChatEvent event, const char *other,
                       const float delay, const int try_ai, Player *const *sorted, const size_t count) {
    if (bc->pending_count >= BOT_CHAT_MAX_PENDING) return;
    char text[NET_CHAT_TEXT_SIZE];
    const char *map = game_map_label(game);
    if (!bot_chat_template(speaker->personality, event, pcg32_random_r(&bc->rng), other, map, text, sizeof(text))) return;
    char clean[NET_CHAT_TEXT_SIZE];
    if (!bot_chat_filter(text, bot->name, clean, sizeof(clean))) return;

    BotChatPending *pd = &bc->pending[bc->pending_count++];
    memset(pd, 0, sizeof(*pd));
    pd->id = bc->next_id++;
    pd->bot_uid = bot->uid;
    pd->round = bc->round;
    pd->due = game->now + delay;
    snprintf(pd->text, sizeof(pd->text), "%s", clean);
    speaker->lines_round++;

    static const char *const k_events[] = {"win", "lose", "draw", "kill", "death", "gg"};
    if (try_ai && event != BOT_CHAT_GG && bc->ai && game->hooks.bot_banter) {
        BotChatRequest req;
        memset(&req, 0, sizeof(req));
        req.id = pd->id;
        req.bot_uid = bot->uid;
        snprintf(req.bot_name, sizeof(req.bot_name), "%s", bot->name);
        req.personality = bot_chat_personality_name(speaker->personality);
        req.event = k_events[event];
        snprintf(req.other, sizeof(req.other), "%s", other ? other : "");
        req.mode = game_mode_label(game);
        req.map = map;
        req.kills = (int) bot->kills + (event == BOT_CHAT_KILL); // on_kill runs before killer->kills++
        req.deaths = (int) bot->deaths;
        req.players = (int) count;
        for (size_t i = 0; i < count; i++)
            if (sorted[i] == bot) req.place = (int) i + 1;
        req.fallback = pd->text;
        if (game->hooks.bot_banter(game, &req)) {
            pd->ai = 1;
            pd->deadline = game->now + BOT_CHAT_AI_TIMEOUT;
            bc->stats.ai_requests++;
        }
    }
}

// ---------------------------------------------------------------- events

void bot_chat_on_match_end(Game *game, const ModeWinner *winner, Player *const *sorted, const size_t count) {
    if (!game || !game_is_authority(game)) return;
    BotChat *bc = bot_chat_get(game, 1);
    if (!bc || !bc->enabled) return;
    bc->pending_count = 0; // kill / death comments not out yet are dropped: the end screen is the gg moment

    const Player *bots[BOT_CHAT_SPEAKERS_MAX];
    int nbots = 0;
    for (size_t i = 0; i < game->player_count && nbots < BOT_CHAT_SPEAKERS_MAX; i++) {
        const Player *p = game->players[i];
        if (p && p->is_bot) bots[nbots++] = p;
    }
    if (!nbots) return;

    int chatty;
    if (bc->chatty_streak >= BOT_CHAT_END_CHATTY_MAX) chatty = 0;
    else if (bc->silent_streak >= BOT_CHAT_END_SILENT_MAX) chatty = 1;
    else chatty = frand(bc) < BOT_CHAT_END_CHANCE;
    if (!chatty) {
        bc->silent_streak++;
        bc->chatty_streak = 0;
        bc->stats.ends_silent++;
        return;
    }
    bc->chatty_streak++;
    bc->silent_streak = 0;
    bc->stats.ends_chatty++;

    // one or two speakers, personality-weighted, distinct
    const int want = nbots >= 2 && frand(bc) < 0.6f ? 2 : 1;
    const Player *speakers[BOT_CHAT_END_MAX_SPEAKERS] = {NULL, NULL};
    for (int s = 0; s < want; s++) {
        float total = 0.0f;
        for (int i = 0; i < nbots; i++) {
            if (bots[i] == speakers[0]) continue;
            total += k_personality_weight[bot_chat_personality_of(bots[i]->name, bots[i]->uid)];
        }
        float r = frand(bc) * total;
        for (int i = 0; i < nbots; i++) {
            if (bots[i] == speakers[0]) continue;
            r -= k_personality_weight[bot_chat_personality_of(bots[i]->name, bots[i]->uid)];
            if (r <= 0.0f || i == nbots - 1) {
                speakers[s] = bots[i];
                break;
            }
        }
    }

    // delays: distinct, at least BOT_CHAT_END_DELAY_GAP apart
    float delay[BOT_CHAT_END_MAX_SPEAKERS];
    delay[0] = frange(bc, BOT_CHAT_END_DELAY_MIN, BOT_CHAT_END_DELAY_MAX);
    delay[1] = delay[0];
    for (int tries = 0; want > 1 && tries < 16 && fabsf(delay[1] - delay[0]) < BOT_CHAT_END_DELAY_GAP; tries++)
        delay[1] = frange(bc, BOT_CHAT_END_DELAY_MIN, BOT_CHAT_END_DELAY_MAX);
    if (want > 1 && fabsf(delay[1] - delay[0]) < BOT_CHAT_END_DELAY_GAP)
        delay[1] = delay[0] - BOT_CHAT_END_DELAY_GAP >= BOT_CHAT_END_DELAY_MIN ? delay[0] - BOT_CHAT_END_DELAY_GAP : delay[0] + BOT_CHAT_END_DELAY_GAP;

    const char *winner_name = winner && winner->player ? winner->player->name : "";
    for (int s = 0; s < want; s++) {
        const Player *bot = speakers[s];
        if (!bot) continue;
        BotChatSpeaker *sp = speaker_of(bc, bot);
        // the first speaker says gg; the second (or a lone speaker half the time) comments the result
        const int banter = s == 1 || (want == 1 && frand(bc) < 0.5f);
        BotChatEvent event = BOT_CHAT_GG;
        if (banter) {
            const int has_winner = winner && (winner->team || winner->player);
            const int did_win = winner && ((bot->team && bot->team == winner->team) || bot == winner->player);
            event = !has_winner ? BOT_CHAT_DRAW : did_win ? BOT_CHAT_WIN : BOT_CHAT_LOSE;
        }
        queue_line(game, bc, bot, sp, event, winner_name, delay[s], banter, sorted, count);
    }
}

void bot_chat_on_kill(Game *game, const Player *victim, const Player *killer) {
    if (!game || !game_is_authority(game) || game->end_timer > 0.0f) return;
    if (!(killer && killer->is_bot) && !(victim && victim->is_bot)) return; // humans only: nothing to allocate
    BotChat *bc = bot_chat_get(game, 1);
    if (!bc || !bc->enabled) return;
    const Player *bot = NULL;
    const Player *other = NULL;
    float chance = 0.0f;
    BotChatEvent event = BOT_CHAT_KILL;
    if (killer && killer != victim && killer->is_bot) {
        bot = killer;
        other = victim;
        chance = BOT_CHAT_KILL_CHANCE;
    } else if (victim && victim->is_bot && killer && killer != victim) {
        bot = victim;
        other = killer;
        chance = BOT_CHAT_DEATH_CHANCE;
        event = BOT_CHAT_DEATH;
    }
    if (!bot || !other) return;
    if (game->now - bc->last_global_line < BOT_CHAT_GLOBAL_COOLDOWN) return;
    BotChatSpeaker *sp = speaker_of(bc, bot);
    if (game->now - sp->last_line < BOT_CHAT_BOT_COOLDOWN || sp->lines_round >= BOT_CHAT_LINES_PER_ROUND) return;
    if (frand(bc) >= chance) return;

    // place by score among the players (orderByScore, ties by order)
    Player *sorted[GAME_LEADERBOARD_MAX];
    size_t count = 0;
    for (size_t i = 0; i < game->player_count && count < GAME_LEADERBOARD_MAX; i++)
        if (game->players[i]) sorted[count++] = game->players[i];
    for (size_t i = 1; i < count; i++) {
        Player *p = sorted[i];
        size_t j = i;
        while (j && sorted[j - 1]->score < p->score) {
            sorted[j] = sorted[j - 1];
            j--;
        }
        sorted[j] = p;
    }
    const int before = bc->pending_count;
    queue_line(game, bc, bot, sp, event, other->name, frange(bc, BOT_CHAT_KILL_DELAY_MIN, BOT_CHAT_KILL_DELAY_MAX), 1, sorted, count);
    if (bc->pending_count > before) { // the cooldowns start when the line is decided, not sent
        bc->last_global_line = game->now;
        sp->last_line = game->now;
    }
}

int bot_chat_deliver(Game *game, const int id, const char *text) {
    BotChat *bc = game ? game->bot_chat : NULL;
    if (!bc) return 0;
    for (int i = 0; i < bc->pending_count; i++) {
        BotChatPending *pd = &bc->pending[i];
        if (pd->id != id || !pd->ai) continue;
        pd->answered = 1;
        if (!text || !text[0]) {
            bc->stats.ai_failed++;
            return 1;
        }
        const Player *p = game_find_player(game, pd->bot_uid);
        char clean[NET_CHAT_TEXT_SIZE];
        if (bot_chat_filter(text, p ? p->name : NULL, clean, sizeof(clean))) {
            snprintf(pd->text, sizeof(pd->text), "%s", clean);
            pd->from_ai = 1;
        } else bc->stats.ai_rejected++;
        return 1;
    }
    return 0;
}

void bot_chat_tick(Game *game, const float delta_s) {
    (void) delta_s;
    BotChat *bc = game ? game->bot_chat : NULL;
    if (!bc || !bc->pending_count || !game_is_authority(game)) return;
    for (int i = 0; i < bc->pending_count;) {
        BotChatPending *pd = &bc->pending[i];
        int ready = 0, drop = pd->round != bc->round;
        if (!drop && game->now >= pd->due) {
            if (!pd->ai || pd->answered) ready = 1;
            else if (game->now >= pd->deadline) {
                ready = 1;
                bc->stats.ai_timeouts++;
            }
        }
        if (ready) {
            send_line(game, pd->bot_uid, pd->text);
            bc->stats.lines++;
            if (pd->from_ai) bc->stats.ai_lines++;
        }
        if (ready || drop) {
            memmove(pd, pd + 1, sizeof(*pd) * (size_t) (bc->pending_count - i - 1));
            bc->pending_count--;
        } else i++;
    }
}
