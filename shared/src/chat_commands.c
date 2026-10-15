// Krunker v3.1.1 chat "/" commands (see shared/include/chat_commands.h). One function per
// cmd_* of the v3.1.1 offline server's packetHandlers.ts, with its reply strings. Native
// differences are commented in place and listed in doc/console.md.
#include <chat_commands.h>
#include <game_config.h> // game_config_is_custom (third person)
#include <flavor_rules.h> // flavor_class_allowed (class)
#include <ruleset.h>     // rules_classes / rules_weapons (class)
#include <ai.h>
#include <bots.h>
#include <bot_path.h>
#include "modes/mode_internal.h"
#include "triggers_internal.h" // trigger_send_change_position ("chgp")
#include <map_catalog.h>
#include <object_manager.h> // give / drop (KrunkNative Object Manager)
#include <target_select.h>
#include <weapons.h>
#include <world.h> // build_fill: game_build_object
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define CHAT_REPLY_MAX 280      // bytes per Server line (NET_CHAT_TEXT_SIZE - NUL, with room)
#define CHAT_ARGS_MAX 24
#define CHAT_TEXT_MAX 1536      // one formatted reply before it is split into lines

typedef struct {
    ChatCommandHost *host;
    int sender;                 // connection id, or CHAT_CMD_CONSOLE
    ChatPeer self;              // the sender's connection (valid when has_self)
    int has_self;
    Player *player;             // the sender's player, NULL in the menu / console
    int argc;
    char *argv[CHAT_ARGS_MAX];  // parts.slice(1) of text.split(" ") (empty parts kept)
    char rest[NET_CHAT_TEXT_SIZE]; // args.join(" ").trim()
    const char *command;        // the command name (denial log)
} ChatCtx;

typedef struct {
    ChatCommandInfo info;
    void (*run)(ChatCtx*);
} ChatCommand;

// ---------------------------------------------------------------- output

static void send_line(ChatCtx *c, const int target, const char *text) {
    if (target == CHAT_CMD_CONSOLE) {
        if (c->host->log) c->host->log(c->host->user, text);
        return;
    }
    NetChatPacket packet;
    net_packet_chat(&packet, -1, NET_CHAT_SERVER, "Server", text);
    game_send(c->host->game, target, NET_PACKET_CHAT, &packet, sizeof(packet));
}

// Native chat lines hold NET_CHAT_TEXT_SIZE bytes: long lists ("Maps: ...") are split
// after a ", " (else at a UTF-8 boundary) into several Server lines.
static void send_split(ChatCtx *c, const int target, const char *text) {
    size_t len = strlen(text);
    while (len > CHAT_REPLY_MAX) {
        size_t cut = CHAT_REPLY_MAX;
        for (size_t i = CHAT_REPLY_MAX; i > CHAT_REPLY_MAX / 2; i--) {
            if (text[i - 1] == ',' && text[i] == ' ') {
                cut = i;
                break;
            }
        }
        while (cut > 0 && ((unsigned char) text[cut] & 0xC0) == 0x80) cut--;
        char line[CHAT_REPLY_MAX + 1];
        memcpy(line, text, cut);
        line[cut] = 0;
        send_line(c, target, line);
        text += cut;
        while (*text == ' ') text++;
        len = strlen(text);
    }
    if (len) send_line(c, target, text);
}

// ctx.send("ch", "Server", text, 2)
static void reply(ChatCtx *c, const char *fmt, ...) {
    char text[CHAT_TEXT_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    send_split(c, c->sender, text);
}

// ctx.broadcast("ch", "Server", text, 2)
static void broadcast(ChatCtx *c, const char *fmt, ...) {
    char text[CHAT_TEXT_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    send_split(c, NET_TARGET_ALL, text);
    if (c->host->log) c->host->log(c->host->user, text);
}

static void send_cheat(ChatCtx *c, const int uid, const NetCheatFlag flag, const int on) {
    const NetCheatPacket packet = {.flag = (uint8_t) flag, .on = (uint8_t) !!on};
    game_send(c->host->game, uid, NET_PACKET_CHEAT, &packet, sizeof(packet));
}

// ---------------------------------------------------------------- helpers

static int peer_by_id(const ChatCommandHost *host, const int id, ChatPeer *out) {
    const int n = host->peer_count ? host->peer_count(host->user) : 0;
    for (int i = 0; i < n; i++) {
        ChatPeer peer;
        if (host->peer_at(host->user, i, &peer) && peer.id == id) {
            if (out) *out = peer;
            return 1;
        }
    }
    return 0;
}

// requireDev / requirePremiumCheatPerm: a denial answers ["notify", "DEV only"].
// KrunkNative (security, doc/v7/command-auth.md): v3.1.1 lets everyone run every command while
// PERMS is off, and server_bin's default config (prod included) has perms off. That made /kick,
// /ban, /banip, /ip (every player's address), /map, /end ... open to any guest on a public
// server. Like v7.1.4 (requireHostOrDev / requireAuth, never a free-for-all), moderation
// (CHAT_PERM_DEV) and the cheats now stay with DEV peers (ChatPeer.dev: a brain-verified DEV
// account, a config op, the server's own machine) and, for the cheats, the custom host, unless
// the owner opened every command (perms off + gameserver.cheats "all" / --open-cheats: v3.1.1's
// perms-off server; the offline sandbox does). The identity is the server's ChatPeer for the
// connection id, never anything in the packet.
static int allowed(ChatCtx *c, const ChatCommandPerm perm) {
    if (perm == CHAT_PERM_ANY || c->sender == CHAT_CMD_CONSOLE) return 1;
    if (perm != CHAT_PERM_HOST_DEV && !c->host->perms && c->host->cheats_open) return 1;
    if (c->has_self && c->self.dev) return 1;
    if ((perm == CHAT_PERM_CHEAT || perm == CHAT_PERM_HOST_DEV) && c->has_self && c->self.premium_host) return 1;
    if (c->host->log) {
        char line[160];
        snprintf(line, sizeof(line), "[gameserver] %sperms denied: %s from %d (%s, account %d)", perm == CHAT_PERM_CHEAT ? "premium cheat " : "",
                 c->command ? c->command : "?", c->sender, c->has_self && c->self.name ? c->self.name : "?",
                 c->has_self ? c->self.account_id : 0);
        c->host->log(c->host->user, line);
    }
    // ctx.send("notify", text): the client's mod shows it as a toast (ui_notify)
    NetNotifyPacket notify;
    memset(&notify, 0, sizeof(notify));
    snprintf(notify.text, sizeof(notify.text), "%s", perm == CHAT_PERM_CHEAT ? "DEV or premium custom host only"
                                                     : perm == CHAT_PERM_HOST_DEV ? "Host or developer required" : "DEV only");
    game_send(c->host->game, c->sender, NET_PACKET_NOTIFY, &notify, sizeof(notify));
    return 0;
}

// isDev(ctx) (the account check itself, independent of PERMS)
static int is_dev(const ChatCtx *c) {
    return c->sender == CHAT_CMD_CONSOLE || (c->has_self && c->self.dev);
}

// normalisePlayerQuery: lowercase, [^a-z0-9] removed
static void normalise(const char *in, char *out, const size_t n) {
    size_t len = 0;
    for (; in && *in && len + 1 < n; in++) {
        const unsigned char ch = (unsigned char) *in;
        if (isalnum(ch)) out[len++] = (char) tolower(ch);
    }
    out[len] = 0;
}

static const char *player_label(const Player *player, char *buf, const size_t n) {
    if (player->name[0]) snprintf(buf, n, "%s", player->name);
    else snprintf(buf, n, "Guest_%d", player->uid);
    return buf;
}

// fields [name, alias, id] normalised
static int player_fields(const Player *player, char fields[2][64]) {
    char id[16];
    normalise(player->name, fields[0], sizeof(fields[0]));
    snprintf(id, sizeof(id), "%d", player->uid);
    normalise(id, fields[1], sizeof(fields[1]));
    return 2;
}

// ---------------------------------------------------------------- target selection (KrunkNative)
// Every player-targeting command resolves its target through shared/src/target_select.c: a name or
// id as v3.1.1's resolveOnlinePlayerByQuery (exact normalised name / id, else a unique prefix), or a
// selector (*, @me / !, @others, @bots, @humans, @team, @enemies, @alive, @dead, a Joel* glob) that
// may match several players. A selector never widens who may run a command: allowed() gates first.

#define CHAT_TARGETS_MAX 128

typedef struct {
    Player *players[CHAT_TARGETS_MAX];
    int count;
    int selector;               // a selector / glob that left several (or no) players: replies count them
} ChatTargets;

static int fill_candidates(const ChatCtx *c, TargetCandidate *cands, Player **map, const int max) {
    const Game *game = c->host->game;
    int n = 0;
    for (size_t i = 0; i < game->player_count && n < max; i++) {
        Player *p = game->players[i];
        if (!p) continue;
        cands[n] = (TargetCandidate) {.name = p->name, .id = p->uid, .team = p->team, .bot = (unsigned char) !!p->is_bot,
                                      .alive = (unsigned char) !!p->active, .self = (unsigned char) (p == c->player)};
        map[n++] = p;
    }
    return n;
}

static void reply_no_target(ChatCtx *c, const char *query, const TargetResult *r) {
    switch (r->status) {
        case TARGET_AMBIGUOUS: reply(c, "Player '%s' is ambiguous.", query); break;
        case TARGET_NO_MATCH: reply(c, "No players match '%s'.", query); break;
        case TARGET_NO_SELF: reply(c, "'%s' needs your own player (none here).", query); break;
        default: reply(c, "Player '%s' not found.", query); break;
    }
}

// The players `query` names (quiet: no reply on failure). `alive_only` drops unspawned players from
// a selector's matches (a single name still resolves, the command decides what "not spawned" means).
static int select_players(ChatCtx *c, const char *query, const int alive_only, ChatTargets *out, TargetResult *r) {
    static TargetCandidate cands[CHAT_TARGETS_MAX];
    static Player *map[CHAT_TARGETS_MAX];
    int idx[CHAT_TARGETS_MAX];
    const int n = fill_candidates(c, cands, map, CHAT_TARGETS_MAX);
    target_select(cands, n, query, 0, idx, CHAT_TARGETS_MAX, r);
    out->count = 0;
    for (int i = 0; i < r->count && i < CHAT_TARGETS_MAX; i++) {
        Player *p = map[idx[i]];
        if (alive_only && r->selector && !p->active) continue;
        out->players[out->count++] = p;
    }
    if (r->status == TARGET_OK && !out->count) r->status = TARGET_NO_MATCH; // matched only unspawned players
    // a selector that leaves one player answers like that player's name ("God mode ON on Sam")
    out->selector = r->selector && out->count != 1;
    return out->count;
}

static int select_or_reply(ChatCtx *c, const char *query, const int alive_only, ChatTargets *out) {
    TargetResult r;
    if (select_players(c, query, alive_only, out, &r)) return out->count;
    if (r.status == TARGET_NO_MATCH && alive_only && r.count) reply(c, "No spawned players match '%s'.", query);
    else reply_no_target(c, query, &r);
    return 0;
}

// One player: a name / id, or a selector that matches exactly one (tp, ip, banip, unban).
static Player *resolve_or_reply(ChatCtx *c, const char *query) {
    ChatTargets t;
    if (!select_or_reply(c, query, 0, &t)) return NULL;
    if (t.count > 1) {
        reply(c, "'%s' matches %d players: name one.", query, t.count);
        return NULL;
    }
    return t.players[0];
}

static const char *players_word(const int n) { return n == 1 ? "player" : "players"; }

static int is_toggle_literal(const char *s) {
    if (!s) return 0;
    char v[8];
    normalise(s, v, sizeof(v));
    return !strcmp(v, "on") || !strcmp(v, "off") || !strcmp(v, "1") || !strcmp(v, "0") || !strcmp(v, "true") || !strcmp(v, "false");
}

int chat_parse_toggle(const char *arg, const int current) {
    if (!arg) return !current;
    char v[8];
    normalise(arg, v, sizeof(v));
    if (!strcmp(v, "on") || !strcmp(v, "1") || !strcmp(v, "true")) return 1;
    if (!strcmp(v, "off") || !strcmp(v, "0") || !strcmp(v, "false")) return 0;
    return !current;
}

typedef struct {
    ChatTargets targets;
    const char *mode_arg;
    int is_self;                // the caller alone, named by no query ("god", "god on")
    int menu_self;              // ... and not spawned in: targets.players[0] may be NULL (the keep only)
} ToggleTarget;

// ---------------------------------------------------------------- the kept player state (KrunkNative)
// A connection's cheats outlive its Player (PlayerKeep, shared.h): run from the menu they land in the
// keep and the next spawn applies them; run in game they go on the Player and into the keep, so a map
// change (a new Player) keeps them too.

static PlayerKeep *keep_of(const ChatCtx *c, const int uid) {
    return c->host->keep && uid != CHAT_CMD_CONSOLE ? c->host->keep(c->host->user, uid) : NULL;
}

unsigned int player_keep_apply(Player *player, const PlayerKeep *keep) {
    if (!player || !keep) return 0;
    unsigned int changed = 0;
#define KEEP_APPLY(bit, field) \
    if ((keep->set & (bit)) && player->field != keep->field) { \
        player->field = keep->field ? 1 : 0; \
        changed |= (bit); \
    }
    KEEP_APPLY(PLAYER_KEEP_GOD, god_mode)
    KEEP_APPLY(PLAYER_KEEP_NOCLIP, noclip)
    KEEP_APPLY(PLAYER_KEEP_NOTARGET, notarget)
    KEEP_APPLY(PLAYER_KEEP_SMITE, smite)
    KEEP_APPLY(PLAYER_KEEP_UNLIMITED_AMMO, unlimited_ammo)
    KEEP_APPLY(PLAYER_KEEP_FORCE_HIDE, force_hide)
#undef KEEP_APPLY
    if (player->unlimited_ammo) player->can_throw = 1;
    return changed;
}

// The query of `parts` words, args.join(" ").trim()
static void join_query(ChatCtx *c, const int parts, char *query, const size_t size) {
    size_t len = 0;
    query[0] = 0;
    for (int i = 0; i < parts; i++) {
        len += (size_t) snprintf(query + len, size - len, "%s%s", i ? " " : "", c->argv[i]);
        if (len >= size) break;
    }
    char *start = query;
    while (*start == ' ') start++;
    memmove(query, start, strlen(start) + 1);
    for (size_t n = strlen(query); n && query[n - 1] == ' '; n--) query[n - 1] = 0;
}

// resolveDevToggleTarget: "" -> self, "on" -> self + mode, "<who...> [on|off]" -> target(s).
// KrunkNative: <who> may be a selector (god *, god @bots off): every spawned match.
static int toggle_target(ChatCtx *c, const char *label, ToggleTarget *out) {
    int parts = c->argc;
    out->mode_arg = NULL;
    out->is_self = 0;
    out->targets.count = 0;
    out->targets.selector = 0;
    if (c->argc == 1 && is_toggle_literal(c->argv[0])) {
        out->mode_arg = c->argv[0];
        parts = 0;
    } else if (c->argc >= 2 && is_toggle_literal(c->argv[c->argc - 1])) {
        out->mode_arg = c->argv[c->argc - 1];
        parts = c->argc - 1;
    }

    if (!parts) {
        if (!c->player || !c->player->active) {
            // KrunkNative: not spawned in (the menu, dead): the connection's kept state, applied at the spawn
            if (c->has_self && keep_of(c, c->sender)) {
                out->targets.players[0] = c->player;
                out->targets.count = 1;
                out->is_self = 1;
                out->menu_self = 1;
                return 1;
            }
            reply(c, "%s unavailable (no active player).", label);
            return 0;
        }
        out->targets.players[0] = c->player;
        out->targets.count = 1;
        out->is_self = 1;
        out->menu_self = 0;
        return 1;
    }
    out->menu_self = 0;

    char query[NET_CHAT_TEXT_SIZE];
    join_query(c, parts, query, sizeof(query));
    if (!select_or_reply(c, query, 1, &out->targets)) return 0;
    // (a named player who is dead keeps it on the Player and in its keep: the next life has it)
    if (!out->targets.selector && !out->targets.players[0]->active && !keep_of(c, out->targets.players[0]->uid)) {
        reply(c, "%s unavailable (target not active).", label);
        return 0;
    }
    out->is_self = !out->targets.selector && out->targets.players[0] == c->player;
    return 1;
}

// get: the Player's flag, else (no Player: the menu) the keep's; set: the Player's (when there is
// one) and the keep's of connection `uid`
typedef int (*ToggleGet)(const Player*, const PlayerKeep*);
typedef void (*ToggleSet)(ChatCtx*, Player*, int uid, int on);

// One on/off cheat over the targets. With no on|off a single target flips; several go ON unless
// every one already is (then OFF), so `god *` never leaves a mixed lobby. `force` >= 0 overrides
// (ungod). Replies "God mode ON" / "God mode ON on X" / "God mode ON: 5 players".
static void run_toggle(ChatCtx *c, const char *label, const ToggleGet get, const ToggleSet set, const int force) {
    ToggleTarget t;
    if (!toggle_target(c, label, &t)) return;
    int next;
    if (t.menu_self) { // not spawned in: the keep (and the dead Player, if any)
        const PlayerKeep *k = keep_of(c, c->sender);
        next = force >= 0 ? force : chat_parse_toggle(t.mode_arg, get(t.targets.players[0], k));
        set(c, t.targets.players[0], c->sender, next);
        reply(c, "%s %s", label, next ? "ON" : "OFF");
        return;
    }
    if (force >= 0) next = force;
    else if (t.mode_arg || t.targets.count == 1) next = chat_parse_toggle(t.mode_arg, get(t.targets.players[0], keep_of(c, t.targets.players[0]->uid)));
    else {
        int all_on = 1;
        for (int i = 0; i < t.targets.count; i++) all_on &= !!get(t.targets.players[i], NULL);
        next = !all_on;
    }
    for (int i = 0; i < t.targets.count; i++) set(c, t.targets.players[i], t.targets.players[i]->uid, next);
    if (t.is_self) reply(c, "%s %s", label, next ? "ON" : "OFF");
    else if (!t.targets.selector) {
        char name[PLAYER_NAME_SIZE + 16];
        reply(c, "%s %s on %s", label, next ? "ON" : "OFF", player_label(t.targets.players[0], name, sizeof(name)));
    } else {
        reply(c, "%s %s: %d %s", label, next ? "ON" : "OFF", t.targets.count, players_word(t.targets.count));
    }
}

static const char *map_name(const Game *game, const int index) {
    if (index < 0 || (size_t) index >= game->map_count || !game->maps[index]) return NULL;
    const cJSON *name = cJSON_GetObjectItem(game->maps[index], "name");
    return cJSON_IsString(name) ? name->valuestring : NULL;
}

static const char *mode_alias(const int id) {
    const GameModeConfig *config = mode_config(id);
    return config && config->alias ? config->alias : NULL;
}

// The players a kill / team command can see (Krunker players.list).
static size_t player_slots(const ChatCtx *c) {
    return c->host->game->player_count;
}

// ---------------------------------------------------------------- commands

static void cmd_c(ChatCtx *c) {
    const int n = c->host->peer_count ? c->host->peer_count(c->host->user) : 0;
    int in_menu = 0;
    for (int i = 0; i < n; i++) {
        ChatPeer peer;
        if (c->host->peer_at(c->host->user, i, &peer) && (peer.spectating || !peer.player)) in_menu++;
    }
    reply(c, "%d connected, %d in menu", n, in_menu);
}

// One kick / ban; 1 = done. `quiet`: a selector run counts instead of one line per player.
static int kick_or_ban_one(ChatCtx *c, Player *target, const int ban, const int quiet) {
    char label[PLAYER_NAME_SIZE + 16];
    player_label(target, label, sizeof(label));
    GameBot *bot = game_bot_of(c->host->game, target);
    if (bot) { // a v7.1.4 bot player leaves like a kicked guest (BotManager.remove)
        game_bots_remove(c->host->game, bot);
        if (quiet) return 1;
        if (ban) reply(c, "Disconnected %s, but no account id was available to persist a ban.", label);
        else reply(c, "Kicked %s.", label);
        return 1;
    }
    if (!peer_by_id(c->host, target->uid, NULL) || !c->host->disconnect) {
        if (!quiet) reply(c, "Target socket not found.");
        return 0;
    }
    ChatPeer peer;
    const int account = ban && peer_by_id(c->host, target->uid, &peer) && peer.account_id > 0 && c->host->ban_account
                            ? peer.account_id : 0;
    if (account) c->host->ban_account(c->host->user, account, label); // pnhBannedAccountIds.add(accountId)
    c->host->disconnect(c->host->user, target->uid, ban ? "Banned" : "Kicked", ban ? "004003" : "004002");
    if (quiet) return 1;
    if (ban && account) reply(c, "Banned %s (account %d).", label, account);
    else if (ban) reply(c, "Disconnected %s, but no account id was available to persist a ban.", label); // a guest
    else reply(c, "Kicked %s.", label);
    return 1;
}

static void cmd_kick_or_ban(ChatCtx *c, const int ban) {
    if (!allowed(c, CHAT_PERM_DEV)) return;
    if (!c->rest[0]) {
        reply(c, ban ? "Usage: /ban <name|id|selector>" : "Usage: /kick <name|id|selector>");
        return;
    }
    ChatTargets t;
    if (!select_or_reply(c, c->rest, 0, &t)) return;
    if (!t.selector) {
        if (t.players[0] == c->player) {
            reply(c, ban ? "Cannot ban yourself." : "Cannot kick yourself.");
            return;
        }
        kick_or_ban_one(c, t.players[0], ban, 0);
        return;
    }
    // a selector (kick *, kick @bots): every match but the caller. The players are copied first:
    // removing a bot frees its Player.
    int uids[CHAT_TARGETS_MAX], n = 0, done = 0;
    for (int i = 0; i < t.count; i++) if (t.players[i] != c->player) uids[n++] = t.players[i]->uid;
    if (!n) {
        reply(c, ban ? "Cannot ban yourself." : "Cannot kick yourself.");
        return;
    }
    for (int i = 0; i < n; i++) {
        Player *p = game_find_player(c->host->game, uids[i]);
        if (p) done += kick_or_ban_one(c, p, ban, 1);
    }
    reply(c, "%s: %d %s", ban ? "Banned" : "Kicked", done, players_word(done));
}

static void cmd_kick(ChatCtx *c) { cmd_kick_or_ban(c, 0); }
static void cmd_ban(ChatCtx *c) { cmd_kick_or_ban(c, 1); }

// NET_EXT_ADMIN_ACTION (the player list's Kick / Ban, v10 ADMIN_ACTIONS): /kick and /ban for one
// player picked by uid. The sender's rights are checked here again (allowed, CHAT_PERM_DEV: the same
// gate as the chat command), so the caller must not have checked them for us. 1 = the player was
// kicked or banned.
int chat_command_admin_action(ChatCommandHost *host, const int sender_id, const int target_uid, const int ban) {
    if (!host || !host->game || sender_id == CHAT_CMD_CONSOLE) return 0;
    ChatCtx c;
    memset(&c, 0, sizeof(c));
    c.host = host;
    c.sender = sender_id;
    c.command = ban ? "ban" : "kick";
    if (peer_by_id(host, sender_id, &c.self)) {
        c.has_self = 1;
        c.player = c.self.player;
    }
    if (!allowed(&c, CHAT_PERM_DEV)) return 0;
    if (target_uid == sender_id) {
        reply(&c, ban ? "Cannot ban yourself." : "Cannot kick yourself.");
        return 0;
    }
    Player *target = game_find_player(host->game, target_uid);
    if (!target) return 0;
    return kick_or_ban_one(&c, target, ban, 0);
}

static int all_digits(const char *s) {
    if (!*s) return 0;
    for (; *s; s++) if (!isdigit((unsigned char) *s)) return 0;
    return 1;
}

static void cmd_unban(ChatCtx *c) {
    if (!allowed(c, CHAT_PERM_DEV)) return;
    if (!c->rest[0]) {
        reply(c, "Usage: /unban <accountId|name|id>");
        return;
    }
    int account = 0;
    if (all_digits(c->rest)) {
        account = atoi(c->rest);
    } else {
        Player *target = resolve_or_reply(c, c->rest);
        if (!target) return;
        ChatPeer peer;
        if (peer_by_id(c->host, target->uid, &peer)) account = peer.account_id;
    }
    if (account <= 0) {
        reply(c, "No valid account id to unban.");
        return;
    }
    const int removed = c->host->unban_account && c->host->unban_account(c->host->user, account);
    if (removed) reply(c, "Unbanned account %d.", account);
    else reply(c, "Account %d was not banned.", account);
}

// KrunkNative: /kpd <name> on|off (the K.P.D officer flag of an account, kn_brain) and /kpd apps (the
// pending applications). DEV only; the answer comes from the host once the coordinator replies.
static void cmd_kpd(ChatCtx *c) {
    if (!allowed(c, CHAT_PERM_DEV)) return;
    if (c->argc >= 1 && !strcasecmp(c->argv[0], "apps")) {
        if (!c->host->kpd_apps) reply(c, "No K.P.D on this server.");
        else c->host->kpd_apps(c->host->user, c->sender);
        return;
    }
    const int on = c->argc >= 2 && (!strcasecmp(c->argv[1], "on") || !strcmp(c->argv[1], "1"));
    const int off = c->argc >= 2 && (!strcasecmp(c->argv[1], "off") || !strcmp(c->argv[1], "0"));
    if (c->argc < 2 || !c->argv[0][0] || (!on && !off)) {
        reply(c, "Usage: /kpd <account> on|off, /kpd apps");
        return;
    }
    if (!c->host->kpd_officer) {
        reply(c, "No K.P.D on this server.");
        return;
    }
    c->host->kpd_officer(c->host->user, c->sender, c->argv[0], on);
}

static int compare_ints(const void *a, const void *b) {
    const int x = *(const int *) a, y = *(const int *) b;
    return (x > y) - (x < y);
}

static void cmd_bans(ChatCtx *c) {
    if (!allowed(c, CHAT_PERM_DEV)) return;
    enum { BANS_MAX = 512 };
    static int ids[BANS_MAX];
    int total = c->host->list_account_bans ? c->host->list_account_bans(c->host->user, ids, BANS_MAX) : 0;
    if (total <= 0) {
        reply(c, "Ban list is empty.");
        return;
    }
    const int have = total < BANS_MAX ? total : BANS_MAX;
    qsort(ids, (size_t) have, sizeof(int), compare_ints); // .sort((a, b) => a - b)
    char list[CHAT_TEXT_MAX] = "";
    size_t len = 0;
    const int shown = have < 30 ? have : 30;
    for (int i = 0; i < shown && len < sizeof(list); i++) {
        len += (size_t) snprintf(list + len, sizeof(list) - len, "%s%d", i ? ", " : "", ids[i]);
    }
    if (total > shown) reply(c, "Banned accounts (%d): %s (+%d more)", total, list, total - shown);
    else reply(c, "Banned accounts (%d): %s", total, list);
}

static const char *peer_address(const ChatCtx *c, const Player *player) {
    ChatPeer peer;
    if (!peer_by_id(c->host, player->uid, &peer) || !peer.address || !peer.address[0]) return NULL;
    return peer.address;
}

static void cmd_ip(ChatCtx *c) {
    if (!allowed(c, CHAT_PERM_DEV)) return;
    if (!c->rest[0]) {
        reply(c, "Usage: /ip <name|id>");
        return;
    }
    Player *target = resolve_or_reply(c, c->rest);
    if (!target) return;
    char label[PLAYER_NAME_SIZE + 16];
    const char *address = peer_address(c, target);
    reply(c, "%s: %s", player_label(target, label, sizeof(label)), address ? address : "(no address)");
}

// isLikelyIpLiteral
static int ip_literal(const char *s) {
    if (strchr(s, ':')) return 1;
    int dots = 0, digits = 0;
    for (; *s; s++) {
        if (isdigit((unsigned char) *s)) {
            if (++digits > 3) return 0;
        } else if (*s == '.' && digits) {
            dots++;
            digits = 0;
        } else {
            return 0;
        }
    }
    return dots == 3 && digits;
}

static int ip_ban_index(const ChatCommandHost *host, const char *address) {
    for (int i = 0; i < host->ip_ban_count; i++) {
        if (!strcmp(host->ip_bans[i], address)) return i;
    }
    return -1;
}

int chat_command_ip_banned(const ChatCommandHost *host, const char *address) {
    return host && address && address[0] && ip_ban_index(host, address) >= 0;
}

static void cmd_banip(ChatCtx *c) {
    if (!allowed(c, CHAT_PERM_DEV)) return;
    if (!c->rest[0]) {
        reply(c, "Usage: /banip <name|id|ip>");
        return;
    }
    char ip[CHAT_CMD_ADDRESS_SIZE] = "";
    int target_id = -1;
    if (ip_literal(c->rest)) {
        snprintf(ip, sizeof(ip), "%s", c->rest);
    } else {
        Player *target = resolve_or_reply(c, c->rest);
        if (!target) return;
        if (target == c->player) {
            reply(c, "Cannot ban your own IP.");
            return;
        }
        const char *address = peer_address(c, target);
        if (address) {
            snprintf(ip, sizeof(ip), "%s", address);
            target_id = target->uid;
        }
    }
    if (!ip[0]) {
        reply(c, "Could not resolve an IP for that target.");
        return;
    }
    if (c->has_self && c->self.address && c->self.address[0] && !strcmp(c->self.address, ip)) {
        reply(c, "Refused: target IP matches your own connection. Use /pnh ban for account-only ban.");
        return;
    }
    const int already = ip_ban_index(c->host, ip) >= 0;
    if (!already && c->host->ip_ban_count < CHAT_CMD_IP_BANS_MAX) {
        snprintf(c->host->ip_bans[c->host->ip_ban_count++], CHAT_CMD_ADDRESS_SIZE, "%s", ip);
    }
    if (target_id >= 0 && c->host->disconnect) c->host->disconnect(c->host->user, target_id, "Banned", "004003");
    reply(c, already ? "IP already banned: %s" : "IP banned: %s", ip);
}

static void cmd_unbanip(ChatCtx *c) {
    if (!allowed(c, CHAT_PERM_DEV)) return;
    if (!c->rest[0]) {
        reply(c, "Usage: /unbanip <ip>");
        return;
    }
    const int index = ip_ban_index(c->host, c->rest);
    if (index >= 0) {
        ChatCommandHost *host = c->host;
        memmove(host->ip_bans[index], host->ip_bans[index + 1], (size_t) (host->ip_ban_count - index - 1) * CHAT_CMD_ADDRESS_SIZE);
        host->ip_ban_count--;
    }
    reply(c, index >= 0 ? "IP unbanned: %s" : "IP was not banned: %s", c->rest);
}

static void cmd_ipbans(ChatCtx *c) {
    if (!allowed(c, CHAT_PERM_DEV)) return;
    const ChatCommandHost *host = c->host;
    if (!host->ip_ban_count) {
        reply(c, "IP ban list is empty.");
        return;
    }
    char list[CHAT_TEXT_MAX] = "";
    size_t len = 0;
    const int shown = host->ip_ban_count < 20 ? host->ip_ban_count : 20;
    for (int i = 0; i < shown && len < sizeof(list); i++) {
        len += (size_t) snprintf(list + len, sizeof(list) - len, "%s%s", i ? ", " : "", host->ip_bans[i]);
    }
    if (host->ip_ban_count > shown) reply(c, "IP bans (%d): %s (+%d more)", host->ip_ban_count, list, host->ip_ban_count - shown);
    else reply(c, "IP bans (%d): %s", host->ip_ban_count, list);
}

static void ping_report(ChatCtx *c, const char *label, const int ping_ms) {
    if (ping_ms < 0) reply(c, "%s: calculating", label);
    else reply(c, "%s: %d ms", label, ping_ms);
}

static void cmd_ping(ChatCtx *c) {
    const int own = c->has_self ? c->self.ping_ms : -1;
    if (!c->rest[0]) {
        ping_report(c, "Your ping", own);
        return;
    }
    if (!is_dev(c)) {
        char query[64], fields[2][64];
        normalise(c->rest, query, sizeof(query));
        int mine = 0;
        if (c->player && query[0]) {
            player_fields(c->player, fields);
            for (int i = 0; i < 2; i++) {
                if (fields[i][0] && !strncmp(fields[i], query, strlen(query))) mine = 1;
            }
        }
        if (mine) ping_report(c, "Your ping", own);
        else reply(c, "You can only view your own ping.");
        return;
    }
    Player *target = resolve_or_reply(c, c->rest);
    if (!target) return;
    ChatPeer peer;
    const int ping = peer_by_id(c->host, target->uid, &peer) ? peer.ping_ms : -1;
    char label[PLAYER_NAME_SIZE + 32], name[PLAYER_NAME_SIZE + 16];
    snprintf(label, sizeof(label), "%s's ping", player_label(target, name, sizeof(name)));
    ping_report(c, label, ping);
}

static void cmd_end(ChatCtx *c) {
    if (!allowed(c, CHAT_PERM_DEV)) return;
    game_set_round_time(c->host->game, 3.0f); // endTimer = 0, gameTimer = 3000
    reply(c, "Match timer set to 3 seconds.");
}

static void cmd_endless(ChatCtx *c) {
    if (!allowed(c, CHAT_PERM_DEV)) return;
    Game *game = c->host->game;
    if (!game->ready) return;
    const int was = game->round_endless;
    game_set_endless(game, !was);
    reply(c, was ? "Match timer resumed." : "Match timer set to endless.");
}

static int map_has_objects(const Game *game) {
    return game->ready && game->map && game->map->object_count > 0;
}

static void cmd_borders(ChatCtx *c) {
    if (!allowed(c, CHAT_PERM_DEV)) return;
    Game *game = c->host->game;
    if (!map_has_objects(game)) {
        reply(c, "Borders unavailable on this map.");
        return;
    }
    const int current = !game->config.disable_borders;
    const int next = chat_parse_toggle(c->argc ? c->argv[0] : NULL, current);
    if (next == current) {
        reply(c, "Borders already %s", current ? "ON" : "OFF");
        return;
    }
    game->config.disable_borders = (unsigned int) !next;
    send_cheat(c, NET_TARGET_ALL, NET_CHEAT_BORDERS, next);
    broadcast(c, "Borders %s", next ? "ON" : "OFF");
}

// Solid map objects only: zones and triggers stay walk-through (v3.1.1 also injects
// hidden colliders for the map JSON's col objects; natively those are the same objects).
static int force_collide_candidate(const Object *object) {
    if (!object->active || object->collision_type != COLLISION_TYPE_NONE) return 0;
    if (object->trigger || object->teleporter || object->checkpoint || object->pickup || object->flag ||
        object->objective || object->bomb_site || object->score_zone || object->team_zone || object->kill) return 0;
    return object->scale.x > 0.0f && object->scale.y > 0.0f && object->scale.z > 0.0f;
}

int chat_map_force_collide(Map *map, const int on) {
    if (!map) return 0;
    int changed = 0;
    for (size_t i = 0; i < map->object_count; i++) {
        Object *object = map->objects[i];
        if (!object) continue;
        if (on && force_collide_candidate(object)) {
            object->collision_type = object->prefab == PREFAB_CYLINDER ? COLLISION_TYPE_CYLINDER : COLLISION_TYPE_BOX;
            object->force_col = 1;
            changed++;
        } else if (!on && object->force_col) {
            object->collision_type = COLLISION_TYPE_NONE;
            object->force_col = 0;
            changed++;
        }
    }
    return changed;
}

static void cmd_forcecol(ChatCtx *c) {
    if (!allowed(c, CHAT_PERM_DEV)) return;
    Game *game = c->host->game;
    if (!map_has_objects(game)) {
        reply(c, "Force collide unavailable on this map.");
        return;
    }
    ChatCommandHost *host = c->host;
    const int enabled = host->force_collide_map && host->force_collide_map == game->map;
    const int next = chat_parse_toggle(c->argc ? c->argv[0] : NULL, enabled);
    if (!next) {
        if (!enabled) {
            reply(c, "Force collide already OFF");
            return;
        }
        const int restored = chat_map_force_collide(game->map, 0);
        host->force_collide_map = NULL;
        send_cheat(c, NET_TARGET_ALL, NET_CHEAT_FORCE_COLLIDE, 0);
        broadcast(c, "Force collide OFF (%d object%s restored)", restored, restored == 1 ? "" : "s");
        return;
    }
    if (enabled) {
        reply(c, "Force collide already ON");
        return;
    }
    const int forced = chat_map_force_collide(game->map, 1);
    host->force_collide_map = game->map;
    send_cheat(c, NET_TARGET_ALL, NET_CHEAT_FORCE_COLLIDE, 1);
    broadcast(c, "Force collide ON (%d object%s)", forced, forced == 1 ? "" : "s");
}

static void cmd_server(ChatCtx *c) {
    const ChatCommandHost *host = c->host;
    const Game *game = host->game;
    const int mode_id = game_mode_id(game);
    const char *name = map_name(game, game->current_map_index);
    const char *alias = mode_alias(mode_id);
    const float hz = host->tick_rate > 0.0f ? host->tick_rate : 30.0f;
    const int peers = host->peer_count ? host->peer_count(host->user) : 0;

    char socket[24];
    if (c->sender == CHAT_CMD_CONSOLE) snprintf(socket, sizeof(socket), "console");
    else snprintf(socket, sizeof(socket), "%d", c->sender);
    reply(c, "Lobby %s | region %s | socket %s", host->lobby && host->lobby[0] ? host->lobby : "(none)",
          host->region && host->region[0] ? host->region : "(none)", socket);

    char map_label[96];
    if (name) snprintf(map_label, sizeof(map_label), "%s (%d)", name, game->current_map_index);
    else snprintf(map_label, sizeof(map_label), "%d", game->current_map_index);
    char mode_label[32];
    if (alias) snprintf(mode_label, sizeof(mode_label), "%s", alias);
    else snprintf(mode_label, sizeof(mode_label), "%d", mode_id < 0 ? 0 : mode_id);
    reply(c, "Map %s | mode %s (%d) | players %d | tick %.0f Hz (%.2f ms)", map_label, mode_label, mode_id < 0 ? 0 : mode_id, peers, hz, 1000.0f / hz);
    int quota = 0;
    const int autofill = game_bots_autofill(game, &quota);
    if (autofill) reply(c, "Bots %d | autofill on (fill to %d players)", game_bots_count(game), quota); // KrunkNative `autofill`
    else reply(c, "Bots %d | autofill off", game_bots_count(game));
}

static void cmd_modes(ChatCtx *c) {
    if (!allowed(c, CHAT_PERM_DEV)) return;
    int count = 0;
    const GameModeConfig *modes = mode_list(&count);
    char list[CHAT_TEXT_MAX] = "";
    size_t len = 0;
    for (int i = 0; i < count && len < sizeof(list); i++) {
        if (!modes[i].alias || !modes[i].alias[0]) continue;
        len += (size_t) snprintf(list + len, sizeof(list) - len, "%s%s", len ? ", " : "", modes[i].alias);
    }
    reply(c, "%s", list);
}

static void cmd_mode(ChatCtx *c) {
    if (!allowed(c, CHAT_PERM_DEV)) return;
    Game *game = c->host->game;
    if (!c->argc) {
        const int id = game_mode_id(game);
        const GameModeConfig *config = mode_config(id < 0 ? 0 : id);
        if (config) reply(c, "Current mode: %s (%s)", config->alias, config->name);
        else reply(c, "Current mode: %d", id);
        return;
    }
    char arg[32];
    snprintf(arg, sizeof(arg), "%s", c->argv[0]);
    for (char *p = arg; *p; p++) *p = (char) tolower((unsigned char) *p);

    int count = 0, found = -1;
    const GameModeConfig *modes = mode_list(&count);
    for (int i = 0; i < count && found < 0; i++) {
        if (modes[i].alias && !strcmp(modes[i].alias, arg)) found = i;
    }
    if (found < 0) {
        reply(c, "Unknown mode '%s'. Try /modes", arg);
        return;
    }
    if (!c->host->change_map) return;
    const int map_index = game->ready ? game->current_map_index : 0;
    const int mode_index = game_mode_index_for(game, found);
    if (mode_index < 0) return;
    c->host->change_map(c->host->user, map_index, mode_index);
    broadcast(c, "Mode set to %s", modes[found].alias);
}

static void cmd_game(ChatCtx *c) {
    if (!allowed(c, CHAT_PERM_DEV)) return;
    char text[NET_CHAT_TEXT_SIZE] = "";
    size_t len = 0;
    for (int i = 0; i < c->argc && len < sizeof(text); i++) {
        len += (size_t) snprintf(text + len, sizeof(text) - len, "%s%s", i ? " " : "", c->argv[i]);
    }
    reply(c, "Game command received: %s", text);
}

static void cmd_map(ChatCtx *c) {
    if (!allowed(c, CHAT_PERM_DEV)) return;
    Game *game = c->host->game;
    if (!game->ready) {
        reply(c, "Map is still loading, try again shortly.");
        return;
    }
    if (!c->argc) {
        const char *name = map_name(game, game->current_map_index);
        if (name) reply(c, "Current map: %s", name);
        else reply(c, "Current map: %d", game->current_map_index);
        return;
    }
    char arg[64];
    snprintf(arg, sizeof(arg), "%s", c->argv[0]);
    for (char *p = arg; *p; p++) *p = (char) tolower((unsigned char) *p);

    // MAPS (first word / prefix), then custom_maps by name: the catalogue (map_catalog.c)
    const int entry = map_catalog_find(arg);
    if (entry >= 0) {
        const int slot = map_catalog_slot(&game->maps, &game->map_count, (size_t) entry);
        if (slot >= 0 && c->host->change_map) c->host->change_map(c->host->user, slot, game->current_mode_index);
        return; // cycleToMap / loadCustomMapByName: silent
    }
    reply(c, "Unknown map '%s'. Try /maps", arg);
}

static void cmd_maps(ChatCtx *c) {
    if (!allowed(c, CHAT_PERM_DEV)) return;
    char maps[CHAT_TEXT_MAX] = "", custom[CHAT_TEXT_MAX] = "";
    size_t ml = 0, cl = 0;
    for (size_t i = 0; i < map_catalog_count(); i++) {
        const MapCatalogEntry *e = map_catalog_at(i);
        if (!e) continue;
        if (e->source == MAP_SOURCE_OFFICIAL && ml < sizeof(maps)) ml += (size_t) snprintf(maps + ml, sizeof(maps) - ml, "%s%s", ml ? ", " : "", e->name);
        else if (e->source == MAP_SOURCE_CUSTOM && cl < sizeof(custom)) cl += (size_t) snprintf(custom + cl, sizeof(custom) - cl, "%s%s", cl ? ", " : "", e->key);
    }
    reply(c, "Maps: %s", maps);
    if (custom[0]) reply(c, "Custom: %s", custom);
}

static void cmd_cycle(ChatCtx *c) {
    if (!allowed(c, CHAT_PERM_DEV)) return;
    Game *game = c->host->game;
    if (!game->ready) {
        reply(c, "Map is still loading, try again shortly.");
        return;
    }
    if (c->host->change_map && game->map_count) c->host->change_map(c->host->user, game_pick_next_map(game), game->current_mode_index);
}

// One kept flag: the Player's when there is one, else the keep's
#define KEEP_GET(p, k, bit, field) ((p) ? (int) (p)->field : (k) && ((k)->set & (bit)) ? (int) (k)->field : 0)
static void keep_flag(ChatCtx *c, const int uid, const unsigned int bit, const int on) {
    PlayerKeep *k = keep_of(c, uid);
    if (!k) return;
    k->set |= (unsigned char) bit;
    unsigned char *field = bit == PLAYER_KEEP_GOD ? &k->god_mode : bit == PLAYER_KEEP_NOCLIP ? &k->noclip : bit == PLAYER_KEEP_NOTARGET ? &k->notarget
                         : bit == PLAYER_KEEP_SMITE ? &k->smite : bit == PLAYER_KEEP_UNLIMITED_AMMO ? &k->unlimited_ammo : &k->force_hide;
    *field = (unsigned char) !!on;
}

static int get_god(const Player *p, const PlayerKeep *k) { return KEEP_GET(p, k, PLAYER_KEEP_GOD, god_mode); }
static void set_god(ChatCtx *c, Player *p, const int uid, const int on) {
    keep_flag(c, uid, PLAYER_KEEP_GOD, on);
    if (!p) return;
    p->god_mode = (unsigned char) on;
    if (on && p->active) {
        p->health = (float) p->max_health;
        mode_send_health(c->host->game, p); // ["h", hp]
    }
}

static void cmd_god(ChatCtx *c) {
    if (!allowed(c, CHAT_PERM_CHEAT)) return;
    run_toggle(c, "God mode", get_god, set_god, -1);
}

// ungod [target] (KrunkNative): god mode OFF, the same gate as /god
static void cmd_ungod(ChatCtx *c) {
    if (!allowed(c, CHAT_PERM_CHEAT)) return;
    run_toggle(c, "God mode", get_god, set_god, 0);
}

static int get_smite(const Player *p, const PlayerKeep *k) { return KEEP_GET(p, k, PLAYER_KEEP_SMITE, smite); }
static void set_smite(ChatCtx *c, Player *p, const int uid, const int on) {
    keep_flag(c, uid, PLAYER_KEEP_SMITE, on);
    if (p) p->smite = (unsigned char) on;
}

static void cmd_smite(ChatCtx *c) {
    if (!allowed(c, CHAT_PERM_CHEAT)) return;
    run_toggle(c, "Smite", get_smite, set_smite, -1);
}

// notarget (KrunkNative, like Source's): bots and AIs ignore the player (bots.c bot_is_enemy,
// ai.c ai_find_target) and drop them if already targeted. Shots still hit both ways.
static int get_notarget(const Player *p, const PlayerKeep *k) { return KEEP_GET(p, k, PLAYER_KEEP_NOTARGET, notarget); }
static void set_notarget(ChatCtx *c, Player *p, const int uid, const int on) {
    keep_flag(c, uid, PLAYER_KEEP_NOTARGET, on);
    if (p) p->notarget = (unsigned char) on;
}

static void cmd_notarget(ChatCtx *c) {
    if (!allowed(c, CHAT_PERM_CHEAT)) return;
    run_toggle(c, "Notarget", get_notarget, set_notarget, -1);
}

static int get_noclip(const Player *p, const PlayerKeep *k) { return KEEP_GET(p, k, PLAYER_KEEP_NOCLIP, noclip); }
static void set_noclip(ChatCtx *c, Player *p, const int uid, const int on) {
    keep_flag(c, uid, PLAYER_KEEP_NOCLIP, on);
    if (p) p->noclip = (unsigned char) on;
    send_cheat(c, uid, NET_CHEAT_NOCLIP, on); // ["noc", next] (from the menu: the line; the spawn sends it again)
}

static void cmd_noclip(ChatCtx *c) {
    if (!allowed(c, CHAT_PERM_CHEAT)) return;
    run_toggle(c, "Noclip", get_noclip, set_noclip, -1);
}

static int get_forcehide(const Player *p, const PlayerKeep *k) { return KEEP_GET(p, k, PLAYER_KEEP_FORCE_HIDE, force_hide); }
static void set_forcehide(ChatCtx *c, Player *p, const int uid, const int on) {
    keep_flag(c, uid, PLAYER_KEEP_FORCE_HIDE, on);
    if (p) p->force_hide = (unsigned char) on;
    send_cheat(c, uid, NET_CHEAT_FORCE_HIDE, on); // ["fh", next]
}

static void cmd_forcehide(ChatCtx *c) {
    if (!allowed(c, CHAT_PERM_DEV_CHEAT)) return;
    run_toggle(c, "Forcehide", get_forcehide, set_forcehide, -1);
}

static int get_unlimitedammo(const Player *p, const PlayerKeep *k) { return KEEP_GET(p, k, PLAYER_KEEP_UNLIMITED_AMMO, unlimited_ammo); }
static void set_unlimitedammo(ChatCtx *c, Player *p, const int uid, const int on) {
    keep_flag(c, uid, PLAYER_KEEP_UNLIMITED_AMMO, on);
    send_cheat(c, uid, NET_CHEAT_UNLIMITED_AMMO, on); // ["unl", next] (+ "uamo")
    if (!p) return;
    p->unlimited_ammo = (unsigned char) on;
    if (on) p->can_throw = 1;
    if (p->active) mode_send_loadout(c->host->game, p);
}

static void cmd_unlimitedammo(ChatCtx *c) {
    if (!allowed(c, CHAT_PERM_DEV_CHEAT)) return;
    run_toggle(c, "Unlimited ammo", get_unlimitedammo, set_unlimitedammo, -1);
}

unsigned int chat_command_revoke_cheats(ChatCommandHost *host, const int uid, Player *p, PlayerKeep *k, const int dev, const int premium_host) {
    if (!host || dev || (!host->perms && host->cheats_open)) return 0; // allowed() lets them all through
    unsigned int lost = PLAYER_KEEP_FORCE_HIDE | PLAYER_KEEP_UNLIMITED_AMMO; // CHAT_PERM_DEV_CHEAT
    if (!premium_host) lost |= PLAYER_KEEP_GOD | PLAYER_KEEP_NOCLIP | PLAYER_KEEP_NOTARGET | PLAYER_KEEP_SMITE; // CHAT_PERM_CHEAT
    unsigned int off = 0;
#define REVOKE(bit, field, cheat) \
    if ((lost & (bit)) && KEEP_GET(p, k, bit, field)) { \
        off |= (bit); \
        if (k) { \
            k->set |= (unsigned char) (bit); \
            k->field = 0; \
        } \
        if (p) p->field = 0; \
        if ((cheat) >= 0) { \
            const NetCheatPacket packet = {.flag = (uint8_t) (cheat), .on = 0}; \
            game_send(host->game, uid, NET_PACKET_CHEAT, &packet, sizeof(packet)); \
        } \
    }
    REVOKE(PLAYER_KEEP_GOD, god_mode, -1)
    REVOKE(PLAYER_KEEP_NOCLIP, noclip, NET_CHEAT_NOCLIP)
    REVOKE(PLAYER_KEEP_NOTARGET, notarget, -1)
    REVOKE(PLAYER_KEEP_SMITE, smite, -1)
    REVOKE(PLAYER_KEEP_UNLIMITED_AMMO, unlimited_ammo, NET_CHEAT_UNLIMITED_AMMO)
    REVOKE(PLAYER_KEEP_FORCE_HIDE, force_hide, NET_CHEAT_FORCE_HIDE)
#undef REVOKE
    if ((off & PLAYER_KEEP_UNLIMITED_AMMO) && p && p->active) mode_send_loadout(host->game, p);
    return off;
}

// killTargetPlayer: players.kill(target, source) with godMode lifted for the kill.
static void kill_player(Player *target, Player *source) {
    const int had_god = target->god_mode;
    target->god_mode = 0;
    PlayerKillInfo info = {0};
    info.weapon_id = -1;
    player_kill(target, source == target ? NULL : source, &info, 0);
    target->god_mode = (unsigned char) had_god;
}

static void cmd_kill(ChatCtx *c) {
    if (!allowed(c, CHAT_PERM_CHEAT)) return;
    Player *player = c->player;
    // KrunkNative: the server console and rcon (no player) kill targets too, as nobody (no credit)
    if ((!player || !player->active) && c->sender != CHAT_CMD_CONSOLE) return;
    if (!c->rest[0]) {
        if (player) kill_player(player, player);
        return;
    }
    Game *game = c->host->game;

    // KrunkNative selectors (kill *, kill @bots, kill Joel*): every spawned matching player
    if (target_is_selector(c->rest)) {
        ChatTargets t;
        if (!select_or_reply(c, c->rest, 1, &t)) return;
        char names[CHAT_TEXT_MAX] = "";
        size_t len = 0;
        for (int i = 0; i < t.count; i++) {
            char label[PLAYER_NAME_SIZE + 16];
            player_label(t.players[i], label, sizeof(label));
            kill_player(t.players[i], player);
            if (len < sizeof(names)) len += (size_t) snprintf(names + len, sizeof(names) - len, "%s%s", i ? ", " : "", label);
        }
        reply(c, "Killed %d %s: %s.", t.count, players_word(t.count), names);
        return;
    }

    // collectKillTargets: every active player (name/id) and active AI (name/sid) equal to the
    // query, else (no exact match in either list) every one containing it
    char query[64];
    normalise(c->rest, query, sizeof(query));
    if (!query[0]) {
        reply(c, "Kill target '%s' not found.", c->rest);
        return;
    }
    Player *exact[64], *partial[64];
    int exact_n = 0, partial_n = 0;
    for (size_t i = 0; i < player_slots(c); i++) {
        Player *p = game->players[i];
        if (!p || !p->active) continue;
        char fields[2][64];
        player_fields(p, fields);
        if (!strcmp(fields[0], query) || !strcmp(fields[1], query)) {
            if (exact_n < 64) exact[exact_n++] = p;
        } else if ((fields[0][0] && strstr(fields[0], query)) || strstr(fields[1], query)) {
            if (partial_n < 64) partial[partial_n++] = p;
        }
    }
    GameAI *ai_exact[GAME_AI_MAX], *ai_partial[GAME_AI_MAX];
    int ai_exact_n = 0, ai_partial_n = 0;
    for (int i = 0; game->ai && i < game->ai->count; i++) {
        GameAI *ai = &game->ai->ais[i];
        if (!ai->active) continue;
        char fields[2][64], sid[16];
        normalise(ai->name, fields[0], sizeof(fields[0]));
        snprintf(sid, sizeof(sid), "%d", ai->sid);
        normalise(sid, fields[1], sizeof(fields[1]));
        if (!strcmp(fields[0], query) || !strcmp(fields[1], query)) ai_exact[ai_exact_n++] = ai;
        else if ((fields[0][0] && strstr(fields[0], query)) || strstr(fields[1], query)) ai_partial[ai_partial_n++] = ai;
    }
    const int has_exact = exact_n || ai_exact_n;
    Player **targets = has_exact ? exact : partial;
    const int n = has_exact ? exact_n : partial_n;
    GameAI **ai_targets = has_exact ? ai_exact : ai_partial;
    const int ai_n = has_exact ? ai_exact_n : ai_partial_n;
    if (!n && !ai_n) {
        reply(c, "Kill target '%s' not found.", c->rest);
        return;
    }
    char names[CHAT_TEXT_MAX] = "";
    size_t len = 0;
    for (int i = 0; i < n; i++) {
        char label[PLAYER_NAME_SIZE + 16];
        player_label(targets[i], label, sizeof(label));
        kill_player(targets[i], player);
        if (len < sizeof(names)) len += (size_t) snprintf(names + len, sizeof(names) - len, "%s%s", i ? ", " : "", label);
    }
    for (int i = 0; i < ai_n; i++) { // killTargetAI: ai.kill(source), then AI.sync()
        game_ai_kill(game, ai_targets[i], player);
        game_ai_sync(game);
        if (len < sizeof(names)) len += (size_t) snprintf(names + len, sizeof(names) - len, "%s%s", n || i ? ", " : "", ai_targets[i]->name);
    }
    char parts[64] = "";
    if (n) snprintf(parts, sizeof(parts), "%d player%s", n, n == 1 ? "" : "s");
    if (ai_n) snprintf(parts + strlen(parts), sizeof(parts) - strlen(parts), "%s%d AI%s", n ? " + " : "", ai_n, ai_n == 1 ? "" : "s");
    reply(c, "Killed %s: %s.", parts, names);
}

static void cmd_team(ChatCtx *c) {
    Player *player = c->player;
    if (!player || !player->active) return;
    Game *game = c->host->game;
    const GameModeConfig *mode = game->mode ? &game->mode->config : NULL;
    if (!mode || !mode->teams || mode->friendly || mode->clan_war || mode->convert_team) {
        reply(c, "/team only works in team modes.");
        return;
    }
    char arg[16] = "";
    if (c->argc) snprintf(arg, sizeof(arg), "%s", c->argv[0]);
    for (char *p = arg; *p; p++) *p = (char) tolower((unsigned char) *p);
    int next;
    if (!strcmp(arg, "red") || !strcmp(arg, "1")) next = 1;
    else if (!strcmp(arg, "blue") || !strcmp(arg, "2")) next = 2;
    else {
        reply(c, "Usage: /team red|blue");
        return;
    }
    if (player->team == next) {
        reply(c, "Already on %s.", arg);
        return;
    }
    mode_update_team(game, player, next); // player.team = next ("tm" to everyone)
    kill_player(player, player);          // suicide-respawn onto the new team's spawns
    reply(c, "Switched to %s team.", arg);
}

static void cmd_teams(ChatCtx *c) {
    const Game *game = c->host->game;
    int any = 0;
    for (size_t i = 0; i < game->player_count; i++) if (game->players[i]) any = 1;
    if (!any) {
        reply(c, "No players.");
        return;
    }
    if (!game->mode || !game->mode->config.teams || game->mode->config.convert_team) {
        reply(c, "Not a team mode.");
        return;
    }
    int shown = 0;
    for (int team = 0; team < MODE_TEAMS; team++) {
        char names[CHAT_TEXT_MAX] = "";
        size_t len = 0;
        int count = 0;
        for (size_t i = 0; i < game->player_count; i++) {
            const Player *p = game->players[i];
            if (!p || p->team != team) continue;
            char label[PLAYER_NAME_SIZE + 16];
            player_label(p, label, sizeof(label));
            if (len < sizeof(names)) len += (size_t) snprintf(names + len, sizeof(names) - len, "%s%s", count ? ", " : "", label);
            count++;
        }
        if (!count) continue;
        if (team == 1) reply(c, "Red (%d): %s", count, names);
        else if (team == 2) reply(c, "Blue (%d): %s", count, names);
        else reply(c, "Team %d (%d): %s", team, count, names);
        shown++;
    }
    if (!shown) reply(c, "No active players.");
}

static void cmd_score(ChatCtx *c) {
    if (!allowed(c, CHAT_PERM_DEV_CHEAT)) return;
    Player *player = c->player;
    if (!player || !player->active) {
        reply(c, "Score command unavailable (no active player).");
        return;
    }
    double amount = 100.0;
    if (c->argc) {
        char *end = NULL;
        amount = strtod(c->argv[0], &end);
        if (!c->argv[0][0] || (end && *end)) amount = 0.0; // Number("x") is NaN
    }
    if (amount == 0.0 || amount != amount) {
        reply(c, "Usage: /score <amount> (default 100).");
        return;
    }
    player_score(player, (int) amount, 0);
    reply(c, "%s%d score (total %d).", amount > 0 ? "+" : "", (int) amount, player->score);
}

// game.ts listSoulSanctumAIs, joined like `names.join(", ")`
static void summon_names(char *out, const size_t size) {
    int count = 0;
    const GameAITemplate *templates = game_ai_templates(&count);
    size_t len = 0;
    out[0] = 0;
    for (int i = 0; i < count && len < size; i++) len += (size_t) snprintf(out + len, size - len, "%s%s", i ? ", " : "", templates[i].name);
}

// cmd_summon: a Soul Sanctum BOT template by name or alias, at the caller (game.ts spawnSoulSanctumAI).
static void cmd_summon(ChatCtx *c) {
    if (!allowed(c, CHAT_PERM_DEV_CHEAT)) return;
    Game *game = c->host->game;
    int count = 0;
    game_ai_templates(&count);
    if (!count || !game_is_authority(game)) {
        reply(c, "Summon unavailable on this game instance.");
        return;
    }
    Player *player = c->player;
    if (!player || !player->active) {
        reply(c, "Summon unavailable (no active player).");
        return;
    }
    char names[CHAT_TEXT_MAX];
    summon_names(names, sizeof(names));
    if (!c->rest[0]) {
        reply(c, "Summonable: %s", names);
        return;
    }
    const GameAITemplate *template = game_ai_resolve_template(c->rest);
    if (!template) {
        reply(c, "Unknown summon '%s'. Try: %s", c->rest, names);
        return;
    }
    const GameAI *ai = game_ai_summon(game, template, player);
    if (!ai) {
        reply(c, "Failed to summon %s.", template->name);
        return;
    }
    reply(c, "Summoned %s (%d).", template->name, ai->sid);
}

static void cmd_players(ChatCtx *c) {
    const Game *game = c->host->game;
    char names[CHAT_TEXT_MAX] = "";
    size_t len = 0;
    int count = 0;
    for (size_t i = 0; i < game->player_count; i++) {
        const Player *p = game->players[i];
        if (!p) continue;
        char label[PLAYER_NAME_SIZE + 16];
        player_label(p, label, sizeof(label));
        if (len < sizeof(names)) len += (size_t) snprintf(names + len, sizeof(names) - len, "%s%s", count ? ", " : "", label);
        count++;
    }
    if (!count) reply(c, "No players.");
    else reply(c, "Players (%d): %s", count, names);
}

// cmd_ais: every listed AI (dead ones too), name or "(unnamed)"
static void cmd_ais(ChatCtx *c) {
    const Game *game = c->host->game;
    const int count = game->ai ? game->ai->count : 0;
    if (!count) {
        reply(c, "No AI.");
        return;
    }
    char names[CHAT_TEXT_MAX] = "";
    size_t len = 0;
    for (int i = 0; i < count && len < sizeof(names); i++) {
        const char *name = game->ai->ais[i].name[0] ? game->ai->ais[i].name : "(unnamed)";
        len += (size_t) snprintf(names + len, sizeof(names) - len, "%s%s", i ? ", " : "", name);
    }
    reply(c, "AI (%d): %s", count, names);
}

// ---------------------------------------------------------------- bots (Krunker v7.1.4)
// server/index.ts handleDevConsoleBotCommand over shared/src/bots.c, with its reply strings;
// gated like v7 (`bot` / `bot_*`: requireHostOrDev). Offline the local player hosts.

static int bot_manager_ready(ChatCtx *c) {
    if (game_is_authority(c->host->game) && game_bots(c->host->game)) return 1;
    reply(c, "Bot manager is not installed for this lobby.");
    return 0;
}

static const char *plural(const int n) { return n == 1 ? "" : "s"; }

// bot / bot_add / bot_place [count] [tfue|autoRespawn|key=value]
static void bot_spawn_command(ChatCtx *c, const int place) {
    if (!allowed(c, CHAT_PERM_HOST_DEV) || !bot_manager_ready(c)) return;
    BotAssignments a;
    bot_parse_assignments(c->argc, c->argv, &a);
    if (a.error[0]) {
        reply(c, "%s", a.error);
        return;
    }
    Game *game = c->host->game;
    // bot_place: at the caller (the offline devConsole's semantics; v7's server ignores it)
    const Player *at = place && c->player && c->player->active ? c->player : NULL;
    if (place && !at && c->sender != CHAT_CMD_CONSOLE) {
        reply(c, "Bot placement unavailable. Spawn first.");
        return;
    }
    const int requested = place ? 1 : a.count ? a.count : 1;
    int spawned = 0;
    for (int i = 0; i < requested; i++) spawned += game_bots_add(game, &a, at ? &at->position : NULL, at ? at->direction.y : 0.0f) != NULL;
    const char *type = bot_type_name(game->bots->type);
    if (!spawned) reply(c, "Bot limit reached (%d).", GAME_BOTS_MAX);
    else if (at) reply(c, "Spawned %d %s bot%s at your position.", spawned, type, plural(spawned));
    else reply(c, "Spawned %d %s bot%s.", spawned, type, plural(spawned));
}

static void cmd_bot(ChatCtx *c) { bot_spawn_command(c, 0); }
static void cmd_bot_place(ChatCtx *c) { bot_spawn_command(c, 1); }

static void cmd_bot_type(ChatCtx *c) {
    if (!allowed(c, CHAT_PERM_HOST_DEV) || !bot_manager_ready(c)) return;
    Game *game = c->host->game;
    BotType next;
    if (!c->argc || !bot_parse_type(c->argv[0], &next)) {
        reply(c, "Bot type: %s (use bot_type nextbot|tfue)", bot_type_name(game->bots->type));
        return;
    }
    const BotType previous = game->bots->type;
    const int removed = game_bots_set_type(game, next);
    reply(c, "Bot type set to %s; removed %d %s bot%s.", bot_type_name(next), removed, bot_type_name(previous), plural(removed));
}

// botMatchesQuery: all / * / "" = every bot, a number = position, bot id or player sid,
// else the normalised name / socket id / sid (exact or prefix).
static int bot_matches(const GameBot *bot, const int position, const char *query) {
    char needle[64];
    normalise(query, needle, sizeof(needle));
    if (!needle[0] || !strcmp(needle, "all") || !strcmp(query, "*")) return 1;
    char *end = NULL;
    const long n = strtol(query, &end, 10);
    if (end && !*end && end != query && (n == position || n == bot->id || n == bot->uid)) return 1;
    char keys[3][64], sid[16];
    normalise(bot->name, keys[0], sizeof(keys[0]));
    normalise(bot->socket_id, keys[1], sizeof(keys[1]));
    snprintf(sid, sizeof(sid), "%d", bot->uid);
    normalise(sid, keys[2], sizeof(keys[2]));
    for (int i = 0; i < 3; i++) {
        if (keys[i][0] && !strncmp(keys[i], needle, strlen(needle))) return 1;
    }
    return 0;
}

// KrunkNative: a bot's bunny hop skill for bot_list / bot_nav (shared/src/bot_move.c: none /
// learning / clean, "off" when slideHop is off)
static const char *bot_hop_label(const GameBot *bot) {
    return bot->config.slide_hop ? bot_move_skill_name(bot_move_skill(bot)) : "off";
}

static void cmd_bot_list(ChatCtx *c) {
    if (!allowed(c, CHAT_PERM_HOST_DEV) || !bot_manager_ready(c)) return;
    const GameBots *bots = c->host->game->bots;
    if (!bots->count) {
        reply(c, "No bots.");
        return;
    }
    char label[CHAT_TEXT_MAX] = "";
    size_t len = 0;
    for (int i = 0; i < bots->count && len < sizeof(label); i++) {
        const GameBot *bot = &bots->list[i];
        char sid[24];
        if (bot->player) snprintf(sid, sizeof(sid), "#%d", bot->uid);
        else snprintf(sid, sizeof(sid), "(unspawned)");
        len += (size_t) snprintf(label + len, sizeof(label) - len, "%s%s:%d:%s%s bhop:%s", i ? ", " : "", bot_type_name(bot->type), bot->id, bot->name, sid,
                                 bot_hop_label(bot));
    }
    reply(c, "Bots (%d): %s", bots->count, label);
}

// KrunkNative: the navigation counters and every bot's route (shared/src/bot_path.c), for the
// nav e2e (server/tests/bot_nav_e2e.py) and for a look at what the bots are doing.
static void cmd_bot_nav(ChatCtx *c) {
    if (!allowed(c, CHAT_PERM_HOST_DEV) || !bot_manager_ready(c)) return;
    Game *game = c->host->game;
    const GameBots *bots = game->bots;
    BotNavStats st;
    bot_path_stats(game, &st);
    const BotNav *nav = bot_nav_for(game);
    reply(c, "Bot nav: stuck %d, replans %d (forced %d), goals %d, unreachable %d, falls %d; grid %d nodes.", st.stuck, st.replans, st.forced_replans,
          st.goals_reached, st.unreachable, bots->nav_falls, bot_nav_node_count(nav));
    char label[CHAT_TEXT_MAX] = "";
    size_t len = 0;
    for (int i = 0; i < bots->count && len < sizeof(label); i++) {
        const GameBot *bot = &bots->list[i];
        if (!bot->player || !bot->player->active) { // dead (respawning) or not spawned yet
            len += (size_t) snprintf(label + len, sizeof(label) - len, "%s%d:dead", len ? ", " : "", bot->id);
            continue;
        }
        vec3 goal;
        const int has_goal = bot_path_goal(bot, &goal);
        len += (size_t) snprintf(label + len, sizeof(label) - len, "%s%d:%s:%d/%d%s bhop:%s", len ? ", " : "", bot->id, bot_state_name(bot->state),
                                 bot->path_index, bot->path.count, has_goal ? "" : ":nogoal", bot_hop_label(bot));
    }
    if (label[0]) reply(c, "Routes (id:state:waypoint/count): %s", label);
}

static void cmd_bot_kick(ChatCtx *c) {
    if (!allowed(c, CHAT_PERM_HOST_DEV) || !bot_manager_ready(c)) return;
    Game *game = c->host->game;
    if (!c->rest[0] || !strcasecmp(c->rest, "all") || !strcmp(c->rest, "*")) {
        const int removed = game_bots_remove_all(game);
        if (removed) reply(c, "Removing %d bot%s.", removed, plural(removed));
        else reply(c, "No bots.");
        return;
    }
    int removed = 0;
    for (int i = game->bots->count - 1; i >= 0; i--) {
        if (!bot_matches(&game->bots->list[i], i, c->rest)) continue;
        game_bots_remove(game, &game->bots->list[i]);
        removed++;
    }
    if (removed) reply(c, "Removing %d bot%s.", removed, plural(removed));
    else reply(c, "No bots matched '%s'.", c->rest);
}

static void cmd_bot_kill(ChatCtx *c) {
    if (!allowed(c, CHAT_PERM_HOST_DEV) || !bot_manager_ready(c)) return;
    Game *game = c->host->game;
    const char *query = c->rest[0] ? c->rest : "all";
    int matched = 0, killed = 0, missing = 0;
    for (int i = 0; i < game->bots->count; i++) {
        GameBot *bot = &game->bots->list[i];
        if (!bot_matches(bot, i, query)) continue;
        matched++;
        if (!bot->player || !bot->player->active) {
            missing++;
            continue;
        }
        // players.kill(target, player || null, undefined, true): skipRewards
        const int had_god = bot->player->god_mode;
        bot->player->god_mode = 0;
        player_kill(bot->player, c->player && c->player != bot->player ? c->player : NULL, NULL, 1);
        bot->player->god_mode = (unsigned char) had_god;
        killed++;
    }
    if (!matched) reply(c, "No bots matched '%s'.", query);
    else if (killed) reply(c, "Killed %d bot%s.", killed, plural(killed));
    else if (missing) reply(c, "Matched bots are not spawned yet.");
    else reply(c, "No bots matched '%s'.", query);
}

static void cmd_bot_config(ChatCtx *c) {
    if (!allowed(c, CHAT_PERM_HOST_DEV) || !bot_manager_ready(c)) return;
    Game *game = c->host->game;
    char text[CHAT_TEXT_MAX];
    int given = 0;
    for (int i = 0; i < c->argc; i++) given += c->argv[i][0] != 0;
    if (!given) {
        BotAssignments current;
        memset(&current, 0, sizeof(current));
        current.config = game->bots->config[game->bots->type];
        bot_describe_config(&current, 1, text, sizeof(text));
        reply(c, "Bot config: %s", text);
        return;
    }
    BotAssignments a;
    bot_parse_assignments(c->argc, c->argv, &a);
    if (a.error[0]) {
        reply(c, "%s", a.error);
        return;
    }
    const int updated = game_bots_configure(game, &a);
    bot_describe_config(&a, 0, text, sizeof(text));
    reply(c, "Bot config updated (%s); %d live bot%s updated.", text, updated, plural(updated));
}

// bot_mimic [on|off]: mimicTarget (the devConsole's toggle) with v7 server's follow config.
static void cmd_bot_mimic(ChatCtx *c) {
    if (!allowed(c, CHAT_PERM_HOST_DEV) || !bot_manager_ready(c)) return;
    const int enabled = chat_parse_toggle(c->argc ? c->argv[0] : NULL, 0);
    char *argv_on[] = {"mimicTarget=on", "attackTarget=off", "followTarget=on", "watchTarget=on", "roam=off"};
    char *argv_off[] = {"mimicTarget=off", "attackTarget=on", "followTarget=on", "watchTarget=off", "roam=on"};
    BotAssignments a;
    bot_parse_assignments(5, enabled ? argv_on : argv_off, &a);
    const int updated = game_bots_configure(c->host->game, &a);
    reply(c, "%s mode set (%d live bot%s updated).", enabled ? "Mimic-style follow" : "Combat follow", updated, plural(updated));
}

// bot_stop [on|off]: BOT_STOP_CONFIG / BOT_RESUME_CONFIG. With no argument it toggles (v7 always
// stopped again, so a bare bot_stop could never resume); bot_start resumes.
static void bots_set_stopped(ChatCtx *c, const int stopping) {
    char *argv_stop[] = {"attackTarget=off", "followTarget=off", "watchTarget=off", "roam=off"};
    char *argv_resume[] = {"attackTarget=on", "followTarget=on", "watchTarget=off", "roam=on"};
    BotAssignments a;
    bot_parse_assignments(4, stopping ? argv_stop : argv_resume, &a);
    const int updated = game_bots_configure(c->host->game, &a);
    c->host->game->bots->stopped = stopping ? 1 : 0;
    reply(c, "%s %d live bot%s; applies to future bot_add.", stopping ? "Stopped" : "Resumed", updated, plural(updated));
}

static void cmd_bot_stop(ChatCtx *c) {
    if (!allowed(c, CHAT_PERM_HOST_DEV) || !bot_manager_ready(c)) return;
    bots_set_stopped(c, chat_parse_toggle(c->argc ? c->argv[0] : NULL, c->host->game->bots->stopped));
}

// bot_start (KrunkNative addition, alias bot_resume): undo bot_stop.
static void cmd_bot_start(ChatCtx *c) {
    if (!allowed(c, CHAT_PERM_HOST_DEV) || !bot_manager_ready(c)) return;
    bots_set_stopped(c, 0);
}

static void js_number(char *out, const size_t size, const double v) {
    if (v == (double) (long long) v) snprintf(out, size, "%lld", (long long) v);
    else snprintf(out, size, "%.6g", v);
}

static void cmd_bot_difficulty(ChatCtx *c) {
    if (!allowed(c, CHAT_PERM_HOST_DEV) || !bot_manager_ready(c)) return;
    Game *game = c->host->game;
    char value[32];
    if (!c->argc || !c->argv[0][0]) {
        js_number(value, sizeof(value), game->bots->config[game->bots->type].difficulty);
        reply(c, "Bot difficulty: %s", value);
        return;
    }
    float difficulty;
    if (!bot_parse_difficulty(c->argv[0], &difficulty)) {
        reply(c, "Usage: bot_difficulty <easy|normal|hard|max|number>");
        return;
    }
    BotAssignments a;
    bot_difficulty_assignments(difficulty, &a);
    const int updated = game_bots_configure(game, &a);
    js_number(value, sizeof(value), difficulty);
    reply(c, "Bot difficulty set to %s (%d live bot%s updated; applies to future bot_add).", value, updated, plural(updated));
}

// ---------------------------------------------------------------- tp / bring (KrunkNative)
// Krunker v7.1.4's admin actions 4 / 5 / 9 (server/index.ts handleAdminAction: go to a player,
// bring a player, bring everyone) as console commands, plus coordinates. Gated like bot_*
// (requireHostOrDev): DEV peers and the custom host, perms on or off; offline the local player
// hosts. Coordinates are feet positions, what `pos` prints. Players match like /kick (normalised
// name or id, exact, else a unique prefix); bots are players too.

#define CHAT_TP_TOKENS 8

// args.join(" ") split on whitespace (empty parts dropped), into `work`
static int tp_tokens(const ChatCtx *c, char *work, const size_t size, char **out) {
    snprintf(work, size, "%s", c->rest);
    int n = 0;
    for (char *p = work; *p && n < CHAT_TP_TOKENS;) {
        while (*p == ' ' || *p == '\t') *p++ = 0;
        if (!*p) break;
        out[n++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
    }
    return n;
}

static int tp_number(const char *s, float *out) {
    char *end = NULL;
    const float v = strtof(s, &end);
    if (!end || end == s || *end || !isfinite(v)) return 0;
    *out = v;
    return 1;
}

// the last three tokens as x y z
static int tp_coords(char **tokens, const int n, vec3 *out) {
    return n >= 3 && tp_number(tokens[n - 3], &out->x) && tp_number(tokens[n - 2], &out->y) && tp_number(tokens[n - 1], &out->z);
}

// movePlayerTo: players.changePosition(player, x, y, z, doRecon, stopMomentum, dir, broadcast), the
// server-authoritative move: the owner snaps to it (POSITION + a full "l"), everyone else gets "chgp".
// v7 passes the destination's xDire (radians) as dir, a quarter-turn index, and spins the player
// (v7 bug, not replicated): the moved player keeps its facing.
static void tp_move(Game *game, Player *player, const vec3 feet) {
    game_change_position(game, player, feet, 1, 1, 0, 0);
    trigger_send_change_position(game, player, 1, 1, 0, 0);
}

static Player *tp_spawned_target(ChatCtx *c, const char *query) {
    Player *target = resolve_or_reply(c, query);
    if (target && !target->active) {
        char label[PLAYER_NAME_SIZE + 16];
        reply(c, "%s is not spawned.", player_label(target, label, sizeof(label)));
        return NULL;
    }
    return target;
}

// tp <x> <y> <z> | tp <player>
static void cmd_tp(ChatCtx *c) {
    if (!allowed(c, CHAT_PERM_HOST_DEV)) return;
    char work[NET_CHAT_TEXT_SIZE], *tokens[CHAT_TP_TOKENS];
    const int n = tp_tokens(c, work, sizeof(work), tokens);
    if (!n) {
        reply(c, "Usage: tp <x> <y> <z> | tp <player>");
        return;
    }
    Player *self = c->player;
    PlayerKeep *keep = c->has_self ? keep_of(c, c->sender) : NULL;
    if ((!self || !self->active) && !keep) {
        reply(c, c->sender == CHAT_CMD_CONSOLE ? "tp moves your own player; use bring <player|all> <x> <y> <z>." : "Spawn first.");
        return;
    }
    vec3 to;
    char label[PLAYER_NAME_SIZE + 64];
    if (!self || !self->active) { // KrunkNative: from the menu, the next spawn's feet
        if (n == 3 && tp_coords(tokens, n, &to)) {
            snprintf(label, sizeof(label), "%.2f %.2f %.2f", (double) to.x, (double) to.y, (double) to.z);
        } else {
            const Player *target = tp_spawned_target(c, c->rest);
            if (!target) return;
            to = target->position;
            player_label(target, label, sizeof(label));
        }
        keep->has_spawn = 1;
        keep->spawn_at = to;
        reply(c, "You will spawn at %s.", label);
        return;
    }
    if (n == 3 && tp_coords(tokens, n, &to)) {
        snprintf(label, sizeof(label), "%.2f %.2f %.2f", (double) to.x, (double) to.y, (double) to.z);
    } else {
        const Player *target = tp_spawned_target(c, c->rest);
        if (!target) return;
        if (target == self) {
            reply(c, "You are already there.");
            return;
        }
        to = target->position;
        player_label(target, label, sizeof(label));
    }
    tp_move(c->host->game, self, to);
    reply(c, "Teleported to %s.", label);
}

// build_fill [count] [0-3] (KrunkNative): count player builds (default 100, game_build_object: the
// Build Tool's own path, "bo" to everyone, the BUILD_MAX cap) of BUILD_OBJS kind 0-3 (default the
// Build Block), stacked in 10 x 10 layers starting 30 units along +x of your feet. A load fixture for
// placement and frame-time tests (doc/console.md); costs no score.
static void cmd_build_fill(ChatCtx *c) {
    if (!allowed(c, CHAT_PERM_HOST_DEV)) return;
    Player *self = c->player;
    if (!self || !self->active) {
        reply(c, "Spawn first.");
        return;
    }
    const int count = c->argc > 0 && c->argv[0][0] ? atoi(c->argv[0]) : 100;
    const int kind = c->argc > 1 && c->argv[1][0] ? atoi(c->argv[1]) : 0;
    if (count < 1 || count > 2000 || kind < 0 || kind >= WORLD_BUILD_COUNT) {
        reply(c, "Usage: build_fill [count 1-2000] [kind 0-%d]", WORLD_BUILD_COUNT - 1);
        return;
    }
    const WorldBuildObject *def = &g_build_objects[kind];
    int placed = 0;
    for (int i = 0; i < count; i++) {
        const int layer = i / 100, row = (i / 10) % 10, col = i % 10;
        const vec3 feet = {self->position.x + 30.0f + def->w * 0.5f + (float) col * def->w, self->position.y + (float) layer * def->h,
                           self->position.z + ((float) row - 4.5f) * def->l};
        if (!game_build_object(c->host->game, kind, feet, (vec3) {0, 0, 0}, 0.0f, 0, self)) break;
        placed++;
    }
    const size_t len = strlen(def->name);
    reply(c, "Built %d %s%s (%d builds).", placed, def->name, placed == 1 || (len && def->name[len - 1] == 's') ? "" : "s", c->host->game->world.build_count);
}

// bring <player|all> [<x> <y> <z>]: all = every spawned player but you
static void cmd_bring(ChatCtx *c) {
    if (!allowed(c, CHAT_PERM_HOST_DEV)) return;
    char work[NET_CHAT_TEXT_SIZE], *tokens[CHAT_TP_TOKENS];
    const int n = tp_tokens(c, work, sizeof(work), tokens);
    vec3 to;
    const int coords = n >= 4 && tp_coords(tokens, n, &to);
    if (!n) {
        reply(c, "Usage: bring <player|all> [<x> <y> <z>]");
        return;
    }
    Player *self = c->player;
    char where[64];
    if (coords) {
        snprintf(where, sizeof(where), "%.2f %.2f %.2f", (double) to.x, (double) to.y, (double) to.z);
    } else {
        if (!self || !self->active) {
            reply(c, c->sender == CHAT_CMD_CONSOLE ? "Usage from the console: bring <player|all> <x> <y> <z>" : "Spawn first.");
            return;
        }
        to = self->position;
        snprintf(where, sizeof(where), "you");
    }
    // the query: every token before the coordinates, as typed
    char query[NET_CHAT_TEXT_SIZE] = "";
    size_t len = 0;
    for (int i = 0; i < (coords ? n - 3 : n) && len < sizeof(query); i++) {
        len += (size_t) snprintf(query + len, sizeof(query) - len, "%s%s", i ? " " : "", tokens[i]);
    }
    Game *game = c->host->game;
    if (!strcasecmp(query, "all") || target_is_selector(query)) { // all = *: every spawned player but you
        ChatTargets t;
        TargetResult r;
        select_players(c, !strcasecmp(query, "all") ? "*" : query, 1, &t, &r);
        if (r.status == TARGET_NO_SELF) {
            reply_no_target(c, query, &r);
            return;
        }
        int moved = 0;
        for (int i = 0; i < t.count; i++) {
            if (t.players[i] == self) continue;
            tp_move(game, t.players[i], to);
            moved++;
        }
        if (moved) reply(c, "Brought %d player%s to %s.", moved, plural(moved), where);
        else if (!strcasecmp(query, "all") || !strcmp(query, "*")) reply(c, "Nobody to bring.");
        else reply(c, "No players match '%s'.", query);
        return;
    }
    Player *target = tp_spawned_target(c, query);
    if (!target) return;
    if (target == self && !coords) {
        reply(c, "You are already there.");
        return;
    }
    tp_move(game, target, to);
    char label[PLAYER_NAME_SIZE + 16];
    reply(c, "Brought %s to %s.", player_label(target, label, sizeof(label)), where);
}

// autofill [on|off|<players>] (KrunkNative): the server.cfg bot_quota fill turned on / off at run
// time (doc/console.md "Bots"), for this server's lifetime. Off: the fill's bots leave at once and
// none come; on: they fill the lobby again (to <players>, else the server's quota, else the lobby
// size; a hosted custom game too, never ranked). The KNINFO query and /server show it.
static void cmd_autofill(ChatCtx *c) {
    if (!allowed(c, CHAT_PERM_HOST_DEV) || !bot_manager_ready(c)) return;
    Game *game = c->host->game;
    int quota = 0;
    const int on = game_bots_autofill(game, &quota);
    const char *arg = c->argc ? c->argv[0] : NULL;
    if (!arg || !arg[0]) {
        reply(c, "Autofill %s (fill to %d players, %d bot%s now). Usage: autofill on|off|<players>", on ? "ON" : "OFF", quota,
              game_bots_count(game), plural(game_bots_count(game)));
        return;
    }
    char *end = NULL;
    const long n = strtol(arg, &end, 10);
    int next, size = -1;
    if (end != arg && end && !*end) {
        next = n > 0;
        if (n > 0) size = n > GAME_BOT_LIMIT ? GAME_BOT_LIMIT : (int) n;
    } else if (is_toggle_literal(arg)) {
        next = chat_parse_toggle(arg, on);
    } else {
        reply(c, "Usage: autofill on|off|<players>");
        return;
    }
    if (next && c->has_self && c->self.ranked) {
        reply(c, "No autofill in ranked.");
        return;
    }
    const int removed = game_bots_set_autofill(game, next, size);
    game_bots_autofill(game, &quota);
    if (next) reply(c, "Autofill ON: bots fill the lobby to %d players.", quota);
    else reply(c, "Autofill OFF: %d bot%s left.", removed, plural(removed));
}

// v3.1.1 `cmds`, in its order; descriptions from the v3.1.1 dev console mod.

// ---------------------------------------------------------------- give / drop (KrunkNative)
// Not v3.1.1 / v7.1.4 commands: the Object Manager's (shared/include/object_manager.h) weapons
// and items from the console. The Object Manager's own rules decide (object_manager_caps), not
// PERMS: extra weapons wherever its Weapons tab works, items and the Tool Gun where it spawns.

static ObjectManagerPerms om_perms_of(const ChatCtx *c) {
    ObjectManagerPerms perms;
    memset(&perms, 0, sizeof(perms));
    if (c->sender == CHAT_CMD_CONSOLE) perms.dev = 1; // the server console / rcon
    else if (c->has_self) {
        perms.dev = (unsigned char) !!c->self.dev;
        perms.host = (unsigned char) !!c->self.premium_host;
        perms.ranked = (unsigned char) !!c->self.ranked;
    }
    return perms;
}

static void om_notify(ChatCtx *c, const char *text) {
    if (c->sender == CHAT_CMD_CONSOLE) return;
    NetNotifyPacket notify;
    memset(&notify, 0, sizeof(notify));
    snprintf(notify.text, sizeof(notify.text), "%s", text);
    game_send(c->host->game, c->sender, NET_PACKET_NOTIFY, &notify, sizeof(notify));
}

// One Object Manager request for `player` with the sender's rights; the extras the server keeps
static NetSpawnResultPacket om_run(ChatCtx *c, Player *player, const int action, const int item, const float pitch) {
    const ObjectManagerPerms perms = om_perms_of(c);
    const NetSpawnReqPacket req = {.action = (uint8_t) action, .item = (int16_t) item, .pitch = pitch, .yaw = player->direction.y};
    NetSpawnResultPacket out;
    object_manager_request(c->host->game, player, &req, &perms, &out);
    if (c->host->om_extras_changed) c->host->om_extras_changed(c->host->user, player->uid);
    return out;
}

static void om_reply_failure(ChatCtx *c, const NetSpawnResultPacket *r, const char *denied) {
    if (r->status == OM_STATUS_DENIED) {
        reply(c, "%s", denied);
        om_notify(c, denied);
    } else if (r->status == OM_STATUS_LIMIT && r->limit) {
        reply(c, "%s (%u).", object_manager_status_text(r->status), (unsigned int) r->limit);
    } else {
        reply(c, "%s.", object_manager_status_text(r->status));
    }
}

// parseArgs: words, "quoted words" as one
static int split_quoted(const char *rest, char *work, const size_t n, char **argv, const int max) {
    snprintf(work, n, "%s", rest ? rest : "");
    int argc = 0;
    char *p = work;
    while (*p && argc < max) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (*p == '"') {
            argv[argc++] = ++p;
            while (*p && *p != '"') p++;
        } else {
            argv[argc++] = p;
            while (*p && *p != ' ') p++;
        }
        if (*p) *p++ = 0;
    }
    return argc;
}

static void join_words(char **argv, const int count, char *out, const size_t n) {
    size_t len = 0;
    out[0] = 0;
    for (int i = 0; i < count && len < n; i++) len += (size_t) snprintf(out + len, n - len, "%s%s", i ? " " : "", argv[i]);
}

// One Object Manager request of the sender without a live player (the menu, dead): the server's
// object_manager_request_view (devs only; others hear "Spawn first")
static NetSpawnResultPacket om_run_view(ChatCtx *c, const int action, const int item, const float pitch) {
    const NetSpawnReqPacket req = {.action = (uint8_t) action, .item = (int16_t) item, .pitch = pitch, .yaw = NAN};
    NetSpawnResultPacket out = {.status = OM_STATUS_DEAD};
    if (c->host->om_request_view) c->host->om_request_view(c->host->user, c->sender, &req, &out);
    return out;
}

// give to yourself while not spawned in: a weapon joins the loadout of the next spawn, an item drops
// at the view (the last life's spot, else the menu camera's floor)
static void give_from_menu(ChatCtx *c, const int kind, const int id, const char *name) {
    const int weapon = kind == OM_GIVE_WEAPON;
    const NetSpawnResultPacket r = om_run_view(c, weapon ? OM_ACTION_ADD_WEAPON : OM_ACTION_SPAWN_ENTITY, id, weapon ? 0.0f : (float) -M_PI / 2.0f);
    if (r.status != OM_STATUS_OK) {
        om_reply_failure(c, &r, "Not allowed: only developers and the host can spawn here.");
        return;
    }
    reply(c, weapon ? "%s added to your loadout." : "%s dropped.", name);
}

// give <weapon|item> [player]: an extra loadout slot (OM_ACTION_ADD_WEAPON, the Weapons tab's
// left click) or an Entities item at the receiver's feet (OM_ACTION_SPAWN_ENTITY). Names match
// like the Object Manager search; another player than you: devs and the host only.
static void cmd_give(ChatCtx *c) {
    char work[NET_CHAT_TEXT_SIZE], item[NET_CHAT_TEXT_SIZE];
    char *argv[CHAT_ARGS_MAX];
    int argc = split_quoted(c->rest, work, sizeof(work), argv, CHAT_ARGS_MAX);
    if (!argc) {
        reply(c, "Usage: /give <weapon|item> [player|selector] (e.g. /give sniper, /give \"tool gun\", /give krtag, /give * sniper)");
        return;
    }
    int id = -1, matches = 0, kind = OM_GIVE_NONE;
    ChatTargets targets = {.count = 0};
    const char *query = NULL;
    // KrunkNative: give <selector> <weapon|item> too (give * sniper): only a selector leads, so
    // "give tool gun" never reads "tool" as a player
    if (argc >= 2 && target_is_selector(argv[0])) {
        join_words(argv + 1, argc - 1, item, sizeof(item));
        kind = object_manager_give_lookup(item, &id, &matches);
        if (kind != OM_GIVE_NONE) query = argv[0];
    }
    if (kind == OM_GIVE_NONE) {
        join_words(argv, argc, item, sizeof(item));
        kind = object_manager_give_lookup(item, &id, &matches);
    }
    if (kind == OM_GIVE_NONE && argc >= 2) { // the last word names the receiver(s)
        TargetResult r;
        ChatTargets found;
        const int any = select_players(c, argv[argc - 1], 0, &found, &r);
        char head[NET_CHAT_TEXT_SIZE];
        join_words(argv, argc - 1, head, sizeof(head));
        int head_id = -1, head_matches = 0;
        const int head_kind = object_manager_give_lookup(head, &head_id, &head_matches);
        if (head_kind != OM_GIVE_NONE && (any || r.selector)) {
            kind = head_kind;
            id = head_id;
            matches = head_matches;
            query = argv[argc - 1];
            snprintf(item, sizeof(item), "%s", head);
        } else if (head_kind != OM_GIVE_NONE && r.status == TARGET_AMBIGUOUS) {
            reply(c, "Player '%s' is ambiguous.", argv[argc - 1]);
            return;
        }
    }
    if (kind == OM_GIVE_NONE) {
        if (matches > 1) reply(c, "'%s' matches %d weapons and items: be more specific.", item, matches);
        else reply(c, "Unknown weapon or item '%s'.", item);
        return;
    }
    const char *name = object_manager_give_name(kind, id);
    if (query) {
        if (!select_or_reply(c, query, 0, &targets)) return;
    } else if (c->player) {
        targets.players[0] = c->player;
        targets.count = 1;
    } else if (c->has_self && c->host->om_request_view) { // KrunkNative: from the menu, for the next spawn
        give_from_menu(c, kind, id, name);
        return;
    } else {
        reply(c, "Name a player: /give %s <player>.", name);
        return;
    }
    const ObjectManagerPerms perms = om_perms_of(c);
    int others = 0;
    for (int i = 0; i < targets.count; i++) others |= targets.players[i] != c->player;
    if (others && !perms.dev && !perms.host) { // a selector never widens the gate
        reply(c, "Only developers and the host can give to other players.");
        om_notify(c, "Host or developer required");
        return;
    }
    const int weapon = kind == OM_GIVE_WEAPON;
    const char *denied = weapon && id != WEAPON_ID_TOOL_GUN ? "Not allowed: extra weapons are off in this game."
                                                            : "Not allowed: only developers and the host can spawn here.";
    if (!targets.selector) {
        Player *target = targets.players[0];
        char who[PLAYER_NAME_SIZE + 16];
        player_label(target, who, sizeof(who));
        if (!target->active && target == c->player && c->host->om_request_view) { // dead: kept for the next life
            give_from_menu(c, kind, id, name);
            return;
        }
        if (!target->active) {
            reply(c, "%s is not spawned.", target == c->player ? "You are" : who);
            return;
        }
        // an item lands at the receiver's feet: the crosshair straight down
        const NetSpawnResultPacket r = om_run(c, target, weapon ? OM_ACTION_ADD_WEAPON : OM_ACTION_SPAWN_ENTITY, id, weapon ? 0.0f : (float) -M_PI / 2.0f);
        if (r.status != OM_STATUS_OK) {
            om_reply_failure(c, &r, denied);
            return;
        }
        if (target == c->player) reply(c, weapon ? "%s added to your loadout." : "%s dropped at your feet.", name);
        else reply(c, "Gave %s to %s.", name, who);
        return;
    }
    int given = 0, spawned = 0;
    NetSpawnResultPacket failure = {0};
    for (int i = 0; i < targets.count; i++) {
        Player *target = targets.players[i];
        if (!target->active) continue;
        spawned++;
        const NetSpawnResultPacket r = om_run(c, target, weapon ? OM_ACTION_ADD_WEAPON : OM_ACTION_SPAWN_ENTITY, id, weapon ? 0.0f : (float) -M_PI / 2.0f);
        if (r.status == OM_STATUS_OK) given++;
        else if (failure.status == OM_STATUS_OK) failure = r;
    }
    if (!spawned) {
        reply(c, "No spawned players match '%s'.", query);
        return;
    }
    if (!given) {
        om_reply_failure(c, &failure, denied);
        return;
    }
    reply(c, "Gave %s: %d %s", name, given, players_word(given));
}

// spawn <object|entity|npc> (KrunkNative): an Objects / Entities / NPCs entry of the Object Manager at
// your crosshair, by name (matched like give). The Object Manager's rules decide (devs, the host,
// the sandbox); from the menu (devs) at your view: the last life's spot, else the menu camera's.
static void cmd_spawn(ChatCtx *c) {
    if (!c->rest[0]) {
        reply(c, "Usage: /spawn <object|entity|npc> (e.g. /spawn crate, /spawn \"jump pad\", /spawn ghost, /spawn bot)");
        return;
    }
    char work[NET_CHAT_TEXT_SIZE], name[NET_CHAT_TEXT_SIZE];
    char *argv[CHAT_ARGS_MAX];
    const int argc = split_quoted(c->rest, work, sizeof(work), argv, CHAT_ARGS_MAX);
    join_words(argv, argc, name, sizeof(name));
    int tab = -1, index = -1, matches = 0;
    if (!object_manager_spawn_lookup(name, &tab, &index, &matches)) {
        if (matches > 1) reply(c, "'%s' matches %d objects, entities and NPCs: be more specific.", name, matches);
        else reply(c, "Unknown object, entity or NPC '%s'.", name);
        return;
    }
    int count = 0;
    const ObjectManagerEntry *e = &object_manager_entries(tab, &count)[index];
    const int action = tab == OM_TAB_OBJECTS ? OM_ACTION_SPAWN_OBJECT : tab == OM_TAB_NPCS ? OM_ACTION_SPAWN_NPC : OM_ACTION_SPAWN_ENTITY;
    NetSpawnResultPacket r;
    if (c->player && c->player->active) r = om_run(c, c->player, action, index, c->player->direction.x);
    else if (c->has_self && c->host->om_request_view) r = om_run_view(c, action, index, NAN);
    else {
        reply(c, "spawn needs a player: run it in game.");
        return;
    }
    if (r.status != OM_STATUS_OK) {
        om_reply_failure(c, &r, "Not allowed: only developers and the host can spawn here.");
        return;
    }
    reply(c, "Spawned %s.", e->name);
}

// drop: the weapon in hand on the ground in front of you. An Object Manager extra slot becomes a
// pickup a step ahead (OM_ACTION_DROP_HELD); a picked-up weapon goes back into its pickup at your
// feet (v7.1.4 players.dropWeapon, the drop key's "drpw"). v7 never drops a melee, a class weapon
// (no lastPicked) or the last weapon.
static void cmd_drop(ChatCtx *c) {
    Player *p = c->player;
    if (!p) {
        reply(c, "drop needs a player: run it in game.");
        return;
    }
    if (!p->active || !p->loadout || p->loadout_index < 0 || p->loadout_index >= p->loadout_size) {
        reply(c, "Spawn first.");
        return;
    }
    const int id = p->loadout[p->loadout_index];
    const Weapon *w = p->weapon;
    const char *name = w && w->name ? w->name : "weapon";
    if (object_manager_holds_extra(p)) {
        const NetSpawnResultPacket r = om_run(c, p, OM_ACTION_DROP_HELD, 0, p->direction.x);
        if (r.status != OM_STATUS_OK) om_reply_failure(c, &r, "Not allowed: weapon pickups are off in this game.");
        else if (id == WEAPON_ID_TOOL_GUN) reply(c, "Tool Gun put away.");
        else reply(c, "Dropped the %s.", name);
        return;
    }
    if (!w || w->melee) {
        reply(c, "You can't drop your melee weapon.");
        return;
    }
    if (id >= 0 && id < PLAYER_PICKED_MAX && p->last_picked[id]) {
        const int before = p->loadout_size;
        game_player_drop_weapon(c->host->game, p);
        if (p->loadout_size < before) reply(c, "Dropped the %s.", name);
        else reply(c, "You can't drop the %s here.", name);
        return;
    }
    reply(c, "You can't drop your class weapons: only picked-up and Object Manager weapons drop.");
}

// ---------------------------------------------------------------- third-person camera (KrunkNative)

// Third person shows around corners, so the server decides who may use it
// (doc/v7/decisions.md "Third person"). The rule, in order:
//   - the offline sandbox (and the server console): always;
//   - a DEV peer: always, so hosts and admins can spectate-style record anywhere;
//   - a ranked / comp match: never (not even the host), it is a competitive advantage;
//   - a custom game: the host itself, and everyone once the host sets knTPCam;
//   - a server that opened its cheats to everyone (perms off, gameserver.cheats "all"): everyone;
//   - anything else (the public rotation): refused.
static int third_person_allowed(ChatCtx *c, const char **reason) {
    const Game *game = c->host->game;
    if (reason) *reason = "Third person is off on this server";
    if (c->sender == CHAT_CMD_CONSOLE || (game && game->is_sandbox)) return 1;
    if (c->has_self && c->self.dev) return 1;
    if ((c->has_self && c->self.ranked) || (game && game->config.comp)) {
        if (reason) *reason = "No third person in ranked";
        return 0;
    }
    if (game && game_config_is_custom(game)) {
        if (c->has_self && c->self.premium_host) return 1;
        if (game->config.tp_cam) return 1;
        if (reason) *reason = "The host has third person off";
        return 0;
    }
    if (!c->host->perms && c->host->cheats_open) return 1;
    return 0;
}

// ---------------------------------------------------------------- class (KrunkNative dev, offline sandbox)

// A name matches when its letters and digits (normalise) equal the query's, else when they start with it.
typedef struct {
    int exact, exact_n, part, part_n;
} NameMatch;

static void name_match_add(NameMatch *m, const char *query, const char *name, const int index) {
    char low[64];
    normalise(name, low, sizeof(low));
    if (!strcmp(low, query)) {
        if (m->exact_n++ == 0) m->exact = index;
    } else if (query[0] && !strncmp(low, query, strlen(query))) {
        if (m->part_n++ == 0) m->part = index;
    }
}

// The one match's index, else -1 with *matches the count of matches (0 none, > 1 ambiguous).
static int name_match_result(const NameMatch *m, int *matches) {
    if (m->exact_n) {
        *matches = m->exact_n;
        return m->exact_n == 1 ? m->exact : -1;
    }
    *matches = m->part_n;
    return m->part_n == 1 ? m->part : -1;
}

// A class by its number, else by name (the same matching as the names the console completes).
static int class_lookup(const char *query, const ClassConfig *classes, const size_t count, int *matches) {
    char *end = NULL;
    const long id = strtol(query, &end, 10);
    if (query[0] && !*end) {
        *matches = 1;
        return id >= 0 && (size_t) id < count ? (int) id : -1;
    }
    char needle[64];
    normalise(query, needle, sizeof(needle));
    NameMatch m = {0};
    for (size_t i = 0; i < count; i++) {
        if (classes[i].name) name_match_add(&m, needle, classes[i].name, (int) i);
    }
    return name_match_result(&m, matches);
}

static int weapon_lookup(const char *query, const Weapon *const *weapons, const size_t count, int *matches) {
    char *end = NULL;
    const long id = strtol(query, &end, 10);
    if (query[0] && !*end) {
        *matches = 1;
        return id >= 0 && (size_t) id < count && weapons[id] ? (int) id : -1;
    }
    char needle[64];
    normalise(query, needle, sizeof(needle));
    NameMatch m = {0};
    for (size_t i = 0; i < count; i++) {
        if (weapons[i] && weapons[i]->name) name_match_add(&m, needle, weapons[i]->name, (int) i);
    }
    return name_match_result(&m, matches);
}

// Why this player may not take class `index` here (the gates player_spawn applies, and the class picker):
// game_class_allowed (the one class gate: KrunkNative's own classes are out of official krunker.io games
// only), the flavor, the game's class list, the mode's fixed class. 1 with the reply in `out` when refused.
static int class_refusal(const Game *game, const Player *player, const ClassConfig *classes, const int index, char *out, const size_t n) {
    const char *name = classes[index].name ? classes[index].name : "That class";
    if (!game_class_allowed(game, index)) {
        snprintf(out, n, "%s is not playable on official krunker.io servers.", name);
        return 1;
    }
    if (!flavor_class_allowed(index)) {
        snprintf(out, n, "%s is not in this flavor.", name);
        return 1;
    }
    if (game->config.class_list_count > 0) {
        int listed = 0;
        for (int i = 0; i < game->config.class_list_count; i++) listed |= game->config.class_list[i] == index;
        if (!listed) {
            snprintf(out, n, "%s is not in this game's class list.", name);
            return 1;
        }
    }
    if (game->mode && game->config.force_classes) {
        const GameModeConfig *mode = &game->mode->config;
        if (mode->no_weapons || (mode->force_class >= 0 && mode->force_class != index)) {
            snprintf(out, n, "This mode fixes the class: %s is not allowed.", name);
            return 1;
        }
        if (player->team >= 0 && player->team < 3 && mode->team_class[player->team] >= 0 && mode->team_class[player->team] != index) {
            snprintf(out, n, "This mode fixes your team's class: %s is not allowed.", name);
            return 1;
        }
    }
    return 0;
}

// class <name|id> [secondary] (KrunkNative dev, offline sandbox; the console's class): the class and its
// secondary weapon take effect now, as a respawn gives them (player_switch_class), without a new spawn point
// or an input reset. A refused class or weapon replies why and changes nothing.
static void cmd_class(ChatCtx *c) {
    if (!allowed(c, CHAT_PERM_DEV)) return;
    char work[NET_CHAT_TEXT_SIZE], *argv[CHAT_ARGS_MAX];
    const int argc = split_quoted(c->rest, work, sizeof(work), argv, CHAT_ARGS_MAX);
    if (!argc) {
        reply(c, "Usage: class <name|id> [secondary]");
        return;
    }
    if (!c->host->class_switched) {
        reply(c, "class works in the offline sandbox only.");
        return;
    }
    Player *player = c->player;
    if (!player || !player->active) {
        reply(c, "Spawn first: class switches a spawned player.");
        return;
    }
    Game *game = c->host->game;
    const size_t class_count = game->class_count ? game->class_count : rules_class_count();
    const ClassConfig *classes = game->classes ? game->classes : rules_classes();
    int matches = 0;
    const int index = class_lookup(argv[0], classes, class_count, &matches);
    if (index < 0) {
        if (matches > 1) reply(c, "Class '%s' is ambiguous (%d matches): be more specific.", argv[0], matches);
        else reply(c, "No class called '%s'.", argv[0]);
        return;
    }
    char refusal[CHAT_TEXT_MAX];
    if (class_refusal(game, player, classes, index, refusal, sizeof(refusal))) {
        reply(c, "%s", refusal);
        return;
    }
    const ClassConfig *cls = &classes[index];
    const Weapon *const *weapons = game->weapons ? (const Weapon *const *) game->weapons : rules_weapons();
    const size_t weapon_count = game->weapon_count ? game->weapon_count : rules_weapon_count();
    int secondary = player->secondary_index;
    if (argc >= 2) {
        int wmatches = 0;
        const int w = weapon_lookup(argv[1], weapons, weapon_count, &wmatches);
        if (w < 0) {
            if (wmatches > 1) reply(c, "Weapon '%s' is ambiguous (%d matches): be more specific.", argv[1], wmatches);
            else reply(c, "No weapon called '%s'.", argv[1]);
            return;
        }
        const Weapon *weapon = weapons[w];
        const char *wname = weapon->name ? weapon->name : argv[1];
        const int ranked = game->config.comp;
        const int custom = game_config_is_custom(game) && !ranked;
        char refused_weapon[CHAT_TEXT_MAX] = "";
        if (!weapon->secondary) snprintf(refused_weapon, sizeof(refused_weapon), "%s is not a secondary weapon.", wname);
        else if (game->config.no_secondaries) snprintf(refused_weapon, sizeof(refused_weapon), "Secondary weapons are off in this game.");
        else if (cls->kn_secondary > 0) snprintf(refused_weapon, sizeof(refused_weapon), "%s always carries its own secondary.", cls->name);
        else if (!cls->secondary) snprintf(refused_weapon, sizeof(refused_weapon), "%s has no secondary weapon slot.", cls->name);
        else if (weapon->custom_only && !custom) snprintf(refused_weapon, sizeof(refused_weapon), "%s is custom games only.", wname);
        else if (ranked && weapon->no_comp) snprintf(refused_weapon, sizeof(refused_weapon), "%s is not in competitive lobbies.", wname);
        if (refused_weapon[0]) {
            reply(c, "%s", refused_weapon);
            return;
        }
        secondary = w;
    }
    if (!player_switch_class(player, index, secondary)) {
        reply(c, "class: out of memory.");
        return;
    }
    c->host->class_switched(c->host->user, c->sender);
    reply(c, "Switched to %s (%d). Health %d.", cls->name, index, player->max_health);
}

// thirdperson [on|off]: ask the server for the third-person camera. The reply is the
// NET_CHEAT_THIRD_PERSON grant (the client only switches once it arrives), a denial the notify.
static void cmd_third_person(ChatCtx *c) {
    const char *reason = NULL;
    if (!third_person_allowed(c, &reason)) {
        om_notify(c, reason ? reason : "Third person is off on this server");
        if (c->host->log) {
            char line[160];
            snprintf(line, sizeof(line), "[gameserver] third person denied: %d (%s)", c->sender, reason ? reason : "?");
            c->host->log(c->host->user, line);
        }
        reply(c, "Third Person - Not allowed here (%s)", reason ? reason : "server rule");
        // the refusal is also an "off": the client stops asking (it would again each round)
        if (c->sender != CHAT_CMD_CONSOLE) send_cheat(c, c->sender, NET_CHEAT_THIRD_PERSON, 0);
        return;
    }
    // The client always sends on / off (its own state); a bare command asks for the camera.
    const int on = chat_parse_toggle(c->argc ? c->argv[0] : NULL, 0);
    if (c->sender != CHAT_CMD_CONSOLE) send_cheat(c, c->sender, NET_CHEAT_THIRD_PERSON, on); // the console has no camera
    reply(c, "Third Person - %s", on ? "Enabled" : "Disabled");
}

static const ChatCommand COMMANDS[] = {
    {{"c", NULL, "", "show connected players and menu count", CHAT_PERM_ANY}, cmd_c},
    {{"count", "c", "", "alias for /c (show connected players and menu count)", CHAT_PERM_ANY}, cmd_c},
    {{"kick", NULL, "<name|id|selector>", "kick a player (kick @bots: every match but you)", CHAT_PERM_DEV}, cmd_kick},
    {{"ban", NULL, "<name|id|selector>", "ban a player account (or every match but you)", CHAT_PERM_DEV}, cmd_ban},
    {{"unban", NULL, "<accountId|name|id>", "unban an account", CHAT_PERM_DEV}, cmd_unban},
    {{"kpd", NULL, "<account> on|off | apps", "grant or revoke K.P.D officer, or list the K.P.D applications", CHAT_PERM_DEV}, cmd_kpd},
    {{"bans", NULL, "", "list banned account ids", CHAT_PERM_DEV}, cmd_bans},
    {{"ip", NULL, "<name|id>", "show a player's IP", CHAT_PERM_DEV}, cmd_ip},
    {{"banip", NULL, "<name|id|ip>", "ban an IP in this lobby", CHAT_PERM_DEV}, cmd_banip},
    {{"unbanip", NULL, "<ip>", "unban an IP", CHAT_PERM_DEV}, cmd_unbanip},
    {{"ipbans", NULL, "", "list banned IPs", CHAT_PERM_DEV}, cmd_ipbans},
    {{"ping", NULL, "[name|id]", "show your ping (devs can target players)", CHAT_PERM_ANY}, cmd_ping},
    {{"end", NULL, "", "set match timer to 3 seconds", CHAT_PERM_DEV}, cmd_end},
    {{"endless", NULL, "", "toggle endless match timer", CHAT_PERM_DEV}, cmd_endless},
    {{"borders", NULL, "[on|off]", "toggle invisible barriers", CHAT_PERM_DEV}, cmd_borders},
    {{"forcecol", NULL, "[on|off]", "toggle force collide", CHAT_PERM_DEV}, cmd_forcecol},
    {{"server", NULL, "", "show current map/mode/player/tick info", CHAT_PERM_ANY}, cmd_server},
    {{"mode", NULL, "[alias]", "switch game mode", CHAT_PERM_DEV}, cmd_mode},
    {{"modes", NULL, "", "list game modes", CHAT_PERM_DEV}, cmd_modes},
    {{"map", NULL, "[name]", "switch map (no arg = show current)", CHAT_PERM_DEV}, cmd_map},
    {{"maps", NULL, "", "list available maps", CHAT_PERM_DEV}, cmd_maps},
    {{"cycle", NULL, "", "cycle to the next map", CHAT_PERM_DEV}, cmd_cycle},
    {{"game", NULL, "<text>", "send a game command", CHAT_PERM_DEV}, cmd_game},
    {{"god", NULL, "[target] [on|off]", "toggle god mode (self, target or selector: * @bots @team Joel*)", CHAT_PERM_CHEAT}, cmd_god},
    {{"ungod", NULL, "[target]", "turn god mode off (self, target or selector)", CHAT_PERM_CHEAT}, cmd_ungod},
    {{"smite", NULL, "[target] [on|off]", "toggle smite-on-hit (self, target or selector)", CHAT_PERM_CHEAT}, cmd_smite},
    {{"noclip", NULL, "[target] [on|off]", "toggle noclip (self, target or selector)", CHAT_PERM_CHEAT}, cmd_noclip},
    {{"notarget", NULL, "[target] [on|off]", "toggle notarget: bots and AIs ignore you (self, target or selector)", CHAT_PERM_CHEAT}, cmd_notarget},
    {{"kill", NULL, "[target]", "kill yourself or every matching player (or selector: * @bots @enemies)", CHAT_PERM_CHEAT}, cmd_kill},
    {{"team", NULL, "<red|blue>", "switch teams", CHAT_PERM_ANY}, cmd_team},
    {{"teams", NULL, "", "list teams and players", CHAT_PERM_ANY}, cmd_teams},
    {{"forcehide", NULL, "[target] [on|off]", "toggle forcehide (self, target or selector)", CHAT_PERM_DEV_CHEAT}, cmd_forcehide},
    {{"unlimitedammo", NULL, "[target] [on|off]", "toggle unlimited ammo (self, target or selector)", CHAT_PERM_DEV_CHEAT}, cmd_unlimitedammo},
    {{"score", NULL, "[amount]", "add score (default +100)", CHAT_PERM_DEV_CHEAT}, cmd_score},
    {{"summon", NULL, "[name]", "spawn a Soul Sanctum AI by name (no arg lists summonable names)", CHAT_PERM_DEV_CHEAT}, cmd_summon},
    {{"players", NULL, "", "list connected players (id, name, team)", CHAT_PERM_ANY}, cmd_players},
    {{"ais", NULL, "", "list active AI entities (sid, name)", CHAT_PERM_ANY}, cmd_ais},
    // Krunker v7.1.4 bots (mod/devConsole.ts registrations, server/index.ts handlers)
    {{"bot", NULL, "[count] [tfue|autoRespawn|key=value]", "spawn player-targeting auto-respawn bots (tfue=off for full-strength bots)", CHAT_PERM_HOST_DEV}, cmd_bot},
    {{"bot_add", "bot", "[count] [tfue|autoRespawn|key=value]", "alias for bot", CHAT_PERM_HOST_DEV}, cmd_bot},
    {{"bot_type", NULL, "[nextbot|tfue]", "show or switch bot manager type (switching removes old bots)", CHAT_PERM_HOST_DEV}, cmd_bot_type},
    {{"bot_place", NULL, "[tfue|autoRespawn|key=value]", "spawn a bot at your current position", CHAT_PERM_HOST_DEV}, cmd_bot_place},
    {{"bot_kick", NULL, "[all|index|name]", "remove bots by name/index or all bots", CHAT_PERM_HOST_DEV}, cmd_bot_kick},
    {{"bot_kill", NULL, "[all|index|name]", "kill bots by name/index or all bots", CHAT_PERM_HOST_DEV}, cmd_bot_kill},
    {{"bot_list", NULL, "", "list spawned bots", CHAT_PERM_HOST_DEV}, cmd_bot_list},
    {{"bot_nav", NULL, "", "bot navigation counters and routes", CHAT_PERM_HOST_DEV}, cmd_bot_nav},
    {{"bot_config", NULL, "[key=value]", "show or update default bot options", CHAT_PERM_HOST_DEV}, cmd_bot_config},
    {{"bot_mimic", NULL, "[on|off]", "toggle bots mimicking their target inputs", CHAT_PERM_HOST_DEV}, cmd_bot_mimic},
    {{"bot_stop", NULL, "[on|off]", "toggle bot movement and attacking", CHAT_PERM_HOST_DEV}, cmd_bot_stop},
    {{"bot_start", NULL, "", "resume bots after bot_stop", CHAT_PERM_HOST_DEV}, cmd_bot_start},
    {{"bot_resume", "bot_start", "", "alias for bot_start", CHAT_PERM_HOST_DEV}, cmd_bot_start},
    {{"bot_difficulty", NULL, "<value>", "set default bot difficulty", CHAT_PERM_HOST_DEV}, cmd_bot_difficulty},
    {{"bot_diff", "bot_difficulty", "<value>", "alias for bot_difficulty", CHAT_PERM_HOST_DEV}, cmd_bot_difficulty},
    {{"autofill", NULL, "[on|off|<players>]", "bots fill the lobby (server.cfg bot_quota) on / off for this server", CHAT_PERM_HOST_DEV}, cmd_autofill},
    // KrunkNative: v7.1.4's admin teleports (handleAdminAction 4 / 5 / 9) with coordinates
    {{"tp", NULL, "<x> <y> <z> | <player>", "teleport yourself to a coordinate or to a player", CHAT_PERM_HOST_DEV}, cmd_tp},
    {{"bring", NULL, "<player|all|selector> [<x> <y> <z>]", "bring players to you or to a coordinate", CHAT_PERM_HOST_DEV}, cmd_bring},
    {{"build_fill", NULL, "[count] [kind 0-3]", "place count player builds in a stack beside you (a placement load test)", CHAT_PERM_HOST_DEV}, cmd_build_fill},
    // KrunkNative Object Manager (its own rules: object_manager_caps)
    {{"give", NULL, "[selector] <weapon|item> [player|selector]", "give a weapon (an extra loadout slot) or an item; others: devs and the host", CHAT_PERM_ANY}, cmd_give},
    {{"drop", NULL, "", "drop the weapon in your hand in front of you", CHAT_PERM_ANY}, cmd_drop},
    {{"spawn", NULL, "<object|entity|npc>", "spawn an Object Manager object, entity or NPC at your crosshair (devs: from the menu too)", CHAT_PERM_ANY}, cmd_spawn},
    // KrunkNative third-person camera: its own rule (third_person_allowed), not the perms gate
    {{"class", NULL, "<name|id> [secondary]", "switch your class now (dev, offline sandbox): its loadout and health, and the secondary weapon", CHAT_PERM_DEV}, cmd_class},
    {{"thirdperson", NULL, "[on|off]", "ask for the third-person camera (offline, devs, custom hosts / knTPCam)", CHAT_PERM_ANY}, cmd_third_person},
};

#define COMMAND_COUNT ((int) (sizeof(COMMANDS) / sizeof(COMMANDS[0])))

static ChatCommandInfo g_infos[COMMAND_COUNT];

const ChatCommandInfo *chat_command_list(int *count) {
    if (!g_infos[0].name) {
        for (int i = 0; i < COMMAND_COUNT; i++) g_infos[i] = COMMANDS[i].info;
    }
    if (count) *count = COMMAND_COUNT;
    return g_infos;
}

static const ChatCommand *find_command(const char *name) {
    for (int i = 0; i < COMMAND_COUNT; i++) {
        if (!strcmp(COMMANDS[i].info.name, name)) return &COMMANDS[i];
    }
    return NULL;
}

const ChatCommandInfo *chat_command_find(const char *name) {
    chat_command_list(NULL);
    for (int i = 0; i < COMMAND_COUNT; i++) {
        if (name && !strcmp(g_infos[i].name, name)) return &g_infos[i];
    }
    return NULL;
}

// io_ct: `if (safe.startsWith("/"))`: parts = safe.slice(1).split(" "), cmds[parts[0].toLowerCase()].
// (v3.1.1 HTML-escapes the text first; native chat is escaped by the client that shows it.)
int chat_command_handle(ChatCommandHost *host, const int sender_id, const char *text) {
    if (!host || !host->game || !text || text[0] != '/') return 0;

    char buffer[NET_CHAT_TEXT_SIZE];
    snprintf(buffer, sizeof(buffer), "%s", text + 1);

    char *space = strchr(buffer, ' ');
    if (space) *space = 0;
    char name[32];
    snprintf(name, sizeof(name), "%s", buffer);
    for (char *p = name; *p; p++) *p = (char) tolower((unsigned char) *p);

    const ChatCommand *command = find_command(name);
    if (!command) return 0; // unknown: broadcast as ordinary chat

    ChatCtx c;
    memset(&c, 0, sizeof(c));
    c.host = host;
    c.sender = sender_id;
    c.command = command->info.name;
    if (sender_id != CHAT_CMD_CONSOLE && peer_by_id(host, sender_id, &c.self)) {
        c.has_self = 1;
        c.player = c.self.player;
    }
    // KrunkNative (security, doc/v7/command-auth.md): the table's permission is checked here,
    // before the arguments are parsed or any target / selector (kick *, god @bots) is resolved,
    // so a command whose own gate were missing still cannot run for the wrong peer. Each cmd_*
    // keeps its own check (an allowed peer passes both; a denial notifies and logs once).
    if (!allowed(&c, command->info.perm)) return 1;

    // split(" ") keeps empty parts ("a  b" -> ["a", "", "b"])
    if (space) {
        char *p = space + 1;
        while (c.argc < CHAT_ARGS_MAX) {
            c.argv[c.argc++] = p;
            char *next = strchr(p, ' ');
            if (!next) break;
            *next = 0;
            p = next + 1;
        }
        // args.join(" ").trim()
        size_t len = 0;
        for (int i = 0; i < c.argc && len < sizeof(c.rest); i++) {
            len += (size_t) snprintf(c.rest + len, sizeof(c.rest) - len, "%s%s", i ? " " : "", c.argv[i]);
        }
        char *start = c.rest;
        while (*start == ' ') start++;
        memmove(c.rest, start, strlen(start) + 1);
        for (size_t n = strlen(c.rest); n && c.rest[n - 1] == ' '; n--) c.rest[n - 1] = 0;
    }

    command->run(&c);
    return 1;
}

// ---------------------------------------------------------------- cheat packets

static void ctx_for_sender(ChatCtx *c, ChatCommandHost *host, const int sender_id, const char *label) {
    memset(c, 0, sizeof(*c));
    c->host = host;
    c->sender = sender_id;
    c->command = label;
    if (sender_id != CHAT_CMD_CONSOLE && peer_by_id(host, sender_id, &c->self)) {
        c->has_self = 1;
        c->player = c->self.player;
    }
}

int chat_command_allowed(ChatCommandHost *host, const int sender_id, const ChatCommandPerm perm, const char *label) {
    if (!host || !host->game) return 0;
    ChatCtx c;
    ctx_for_sender(&c, host, sender_id, label);
    return allowed(&c, perm);
}

// io_prmA (packetHandlers.ts): the four premium keys mirror /noclip, /god, /kill and /smite.
void chat_command_premium_action(ChatCommandHost *host, const int sender_id, const int index) {
    if (!host || !host->game || index < 0 || index > 3) return;
    ChatCtx c;
    ctx_for_sender(&c, host, sender_id, "prmA");
    Player *player = c.player;
    if (!player || !player->active) return;
    if (!allowed(&c, CHAT_PERM_CHEAT)) return;

    switch (index) {
        case 0: { // io_noclip: ctx.send("noc", next)
            const int next = !player->noclip;
            set_noclip(&c, player, player->uid, next); // (kept for the next spawn too)
            if (host->log) {
                char line[64];
                snprintf(line, sizeof(line), "[gameserver] noclip %d -> %s", sender_id, next ? "true" : "false");
                host->log(host->user, line);
            }
            break;
        }
        case 1: { // cmd_god
            const int next = !player->god_mode;
            set_god(&c, player, player->uid, next);
            reply(&c, "God mode %s", next ? "ON" : "OFF");
            break;
        }
        case 2: // /kill with godMode lifted
            kill_player(player, player);
            reply(&c, "You killed yourself.");
            break;
        case 3: {
            const int next = !player->smite;
            set_smite(&c, player, player->uid, next);
            reply(&c, "Smite %s", next ? "ON" : "OFF");
            break;
        }
        default:
            break;
    }
}

int chat_third_person_allowed(ChatCommandHost *host, const int sender_id, const char **reason) {
    if (!host || !host->game) return 0;
    ChatCtx c;
    ctx_for_sender(&c, host, sender_id, "thirdperson");
    return third_person_allowed(&c, reason);
}
