// Krunker v3.1.1 chat "/" commands (see shared/include/chat_commands.h). One function per
// cmd_* of the v3.1.1 offline server's packetHandlers.ts, with its reply strings. Native
// differences are commented in place and listed in doc/console.md.
#include <chat_commands.h>
#include "modes/mode_internal.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

// requireDev / requirePremiumCheatPerm. v3.1.1 answers a denial with ["notify", ...],
// which the game client has no handler for; natively the text comes back as a Server line.
static int allowed(ChatCtx *c, const ChatCommandPerm perm) {
    if (perm == CHAT_PERM_ANY || !c->host->perms || c->sender == CHAT_CMD_CONSOLE) return 1;
    if (c->has_self && c->self.dev) return 1;
    reply(c, "%s", perm == CHAT_PERM_DEV ? "DEV only" : "DEV or premium custom host only");
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

typedef enum { RESOLVE_OK, RESOLVE_EMPTY, RESOLVE_AMBIGUOUS, RESOLVE_NOT_FOUND } ResolveReason;

// resolveOnlinePlayerByQuery: exact (normalised) name/id match, else a unique prefix.
static Player *resolve_player(const ChatCtx *c, const char *raw, ResolveReason *reason) {
    char query[64];
    normalise(raw, query, sizeof(query));
    *reason = RESOLVE_EMPTY;
    if (!query[0]) return NULL;

    const Game *game = c->host->game;
    Player *exact = NULL, *prefix = NULL;
    int exact_n = 0, prefix_n = 0;
    for (size_t i = 0; i < game->player_count; i++) {
        Player *p = game->players[i];
        if (!p) continue;
        char fields[2][64];
        player_fields(p, fields);
        if (!fields[0][0] && !fields[1][0]) continue;
        if (!strcmp(fields[0], query) || !strcmp(fields[1], query)) {
            exact = p;
            exact_n++;
        } else if ((fields[0][0] && !strncmp(fields[0], query, strlen(query))) || !strncmp(fields[1], query, strlen(query))) {
            prefix = p;
            prefix_n++;
        }
    }
    if (exact_n == 1) { *reason = RESOLVE_OK; return exact; }
    if (exact_n > 1) { *reason = RESOLVE_AMBIGUOUS; return NULL; }
    if (prefix_n == 1) { *reason = RESOLVE_OK; return prefix; }
    *reason = prefix_n > 1 ? RESOLVE_AMBIGUOUS : RESOLVE_NOT_FOUND;
    return NULL;
}

static Player *resolve_or_reply(ChatCtx *c, const char *query) {
    ResolveReason reason;
    Player *player = resolve_player(c, query, &reason);
    if (player) return player;
    if (reason == RESOLVE_AMBIGUOUS) reply(c, "Player '%s' is ambiguous.", query);
    else reply(c, "Player '%s' not found.", query);
    return NULL;
}

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
    Player *player;
    const char *mode_arg;
    int is_self;
} ToggleTarget;

// resolveDevToggleTarget: "" -> self, "on" -> self + mode, "<who...> [on|off]" -> target.
static int toggle_target(ChatCtx *c, const char *label, ToggleTarget *out) {
    int parts = c->argc;
    out->mode_arg = NULL;
    if (c->argc == 1 && is_toggle_literal(c->argv[0])) {
        out->mode_arg = c->argv[0];
        parts = 0;
    } else if (c->argc >= 2 && is_toggle_literal(c->argv[c->argc - 1])) {
        out->mode_arg = c->argv[c->argc - 1];
        parts = c->argc - 1;
    }

    if (!parts) {
        if (!c->player || !c->player->active) {
            reply(c, "%s unavailable (no active player).", label);
            return 0;
        }
        out->player = c->player;
        out->is_self = 1;
        return 1;
    }

    char query[NET_CHAT_TEXT_SIZE] = "";
    size_t len = 0;
    for (int i = 0; i < parts; i++) {
        len += (size_t) snprintf(query + len, sizeof(query) - len, "%s%s", i ? " " : "", c->argv[i]);
        if (len >= sizeof(query)) break;
    }
    char *start = query;
    while (*start == ' ') start++;
    for (size_t n = strlen(start); n && start[n - 1] == ' '; n--) start[n - 1] = 0;

    Player *player = resolve_or_reply(c, start);
    if (!player) return 0;
    if (!player->active) {
        reply(c, "%s unavailable (target not active).", label);
        return 0;
    }
    out->player = player;
    out->is_self = player == c->player;
    return 1;
}

// targetSuffix: "" for self, " on <name>" otherwise
static const char *target_suffix(const ToggleTarget *t, char *buf, const size_t n) {
    char name[PLAYER_NAME_SIZE + 16];
    if (t->is_self) buf[0] = 0;
    else snprintf(buf, n, " on %s", player_label(t->player, name, sizeof(name)));
    return buf;
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

static void cmd_kick_or_ban(ChatCtx *c, const int ban) {
    if (!allowed(c, CHAT_PERM_DEV)) return;
    if (!c->rest[0]) {
        reply(c, ban ? "Usage: /ban <name|id>" : "Usage: /kick <name|id>");
        return;
    }
    Player *target = resolve_or_reply(c, c->rest);
    if (!target) return;
    if (target == c->player) {
        reply(c, ban ? "Cannot ban yourself." : "Cannot kick yourself.");
        return;
    }
    if (!peer_by_id(c->host, target->uid, NULL) || !c->host->disconnect) {
        reply(c, "Target socket not found.");
        return;
    }
    char label[PLAYER_NAME_SIZE + 16];
    player_label(target, label, sizeof(label));
    c->host->disconnect(c->host->user, target->uid, ban ? "Banned" : "Kicked", ban ? "004003" : "004002");
    // KrunkNative has no accounts (every player is a guest): v3.1.1's guest ban reply.
    if (ban) reply(c, "Disconnected %s, but no account id was available to persist a ban.", label);
    else reply(c, "Kicked %s.", label);
}

static void cmd_kick(ChatCtx *c) { cmd_kick_or_ban(c, 0); }
static void cmd_ban(ChatCtx *c) { cmd_kick_or_ban(c, 1); }

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
    if (all_digits(c->rest)) {
        reply(c, "Account %s was not banned.", c->rest); // the account ban list is always empty
        return;
    }
    if (!resolve_or_reply(c, c->rest)) return;
    reply(c, "No valid account id to unban.");
}

static void cmd_bans(ChatCtx *c) {
    if (!allowed(c, CHAT_PERM_DEV)) return;
    reply(c, "Ban list is empty.");
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

    // parts[0] === arg (split on " " / "_") || name.toLowerCase().startsWith(arg), first in order
    for (size_t i = 0; i < game->map_count; i++) {
        const char *name = map_name(game, (int) i);
        if (!name) continue;
        char lower[64];
        snprintf(lower, sizeof(lower), "%s", name);
        for (char *p = lower; *p; p++) *p = (char) tolower((unsigned char) *p);
        const size_t first = strcspn(lower, " _");
        const int match = (strlen(arg) == first && !strncmp(lower, arg, first)) || !strncmp(lower, arg, strlen(arg));
        if (!match) continue;
        if (c->host->change_map) c->host->change_map(c->host->user, (int) i, game->current_mode_index);
        return; // cycleToMap: silent
    }
    reply(c, "Unknown map '%s'. Try /maps", arg);
}

static void cmd_maps(ChatCtx *c) {
    if (!allowed(c, CHAT_PERM_DEV)) return;
    const Game *game = c->host->game;
    char list[CHAT_TEXT_MAX] = "";
    size_t len = 0;
    for (size_t i = 0; i < game->map_count && len < sizeof(list); i++) {
        const char *name = map_name(game, (int) i);
        if (name) len += (size_t) snprintf(list + len, sizeof(list) - len, "%s%s", len ? ", " : "", name);
    }
    reply(c, "Maps: %s", list);
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

static void cmd_god(ChatCtx *c) {
    if (!allowed(c, CHAT_PERM_CHEAT)) return;
    ToggleTarget t;
    if (!toggle_target(c, "God mode", &t)) return;
    const int next = chat_parse_toggle(t.mode_arg, t.player->god_mode);
    t.player->god_mode = (unsigned char) next;
    if (next) {
        t.player->health = (float) t.player->max_health;
        mode_send_health(c->host->game, t.player); // ["h", hp]
    }
    char suffix[PLAYER_NAME_SIZE + 24];
    reply(c, "God mode %s%s", next ? "ON" : "OFF", target_suffix(&t, suffix, sizeof(suffix)));
}

static void cmd_smite(ChatCtx *c) {
    if (!allowed(c, CHAT_PERM_CHEAT)) return;
    ToggleTarget t;
    if (!toggle_target(c, "Smite", &t)) return;
    const int next = chat_parse_toggle(t.mode_arg, t.player->smite);
    t.player->smite = (unsigned char) next;
    char suffix[PLAYER_NAME_SIZE + 24];
    reply(c, "Smite %s%s", next ? "ON" : "OFF", target_suffix(&t, suffix, sizeof(suffix)));
}

static void cmd_noclip(ChatCtx *c) {
    if (!allowed(c, CHAT_PERM_CHEAT)) return;
    ToggleTarget t;
    if (!toggle_target(c, "Noclip", &t)) return;
    const int next = chat_parse_toggle(t.mode_arg, t.player->noclip);
    t.player->noclip = (unsigned char) next;
    send_cheat(c, t.player->uid, NET_CHEAT_NOCLIP, next); // ["noc", next]
    char suffix[PLAYER_NAME_SIZE + 24];
    reply(c, "Noclip %s%s", next ? "ON" : "OFF", target_suffix(&t, suffix, sizeof(suffix)));
}

static void cmd_forcehide(ChatCtx *c) {
    if (!allowed(c, CHAT_PERM_DEV)) return;
    ToggleTarget t;
    if (!toggle_target(c, "Forcehide", &t)) return;
    const int next = chat_parse_toggle(t.mode_arg, t.player->force_hide);
    t.player->force_hide = (unsigned char) next;
    send_cheat(c, t.player->uid, NET_CHEAT_FORCE_HIDE, next); // ["fh", next]
    char suffix[PLAYER_NAME_SIZE + 24];
    reply(c, "Forcehide %s%s", next ? "ON" : "OFF", target_suffix(&t, suffix, sizeof(suffix)));
}

static void cmd_unlimitedammo(ChatCtx *c) {
    if (!allowed(c, CHAT_PERM_DEV)) return;
    ToggleTarget t;
    if (!toggle_target(c, "Unlimited ammo", &t)) return;
    const int next = chat_parse_toggle(t.mode_arg, t.player->unlimited_ammo);
    t.player->unlimited_ammo = (unsigned char) next;
    if (next) t.player->can_throw = 1;
    send_cheat(c, t.player->uid, NET_CHEAT_UNLIMITED_AMMO, next); // ["unl", next] (+ "uamo")
    mode_send_loadout(c->host->game, t.player);
    char suffix[PLAYER_NAME_SIZE + 24];
    reply(c, "Unlimited ammo %s%s", next ? "ON" : "OFF", target_suffix(&t, suffix, sizeof(suffix)));
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
    if (!player || !player->active) return;
    if (!c->rest[0]) {
        kill_player(player, player);
        return;
    }

    // collectKillTargets: every active player whose name/id equals the query, else contains it
    char query[64];
    normalise(c->rest, query, sizeof(query));
    if (!query[0]) {
        reply(c, "Kill target '%s' not found.", c->rest);
        return;
    }
    Game *game = c->host->game;
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
    Player **targets = exact_n ? exact : partial;
    const int n = exact_n ? exact_n : partial_n;
    if (!n) {
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
    reply(c, "Killed %d player%s: %s.", n, n == 1 ? "" : "s", names); // no AI natively
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
    if (!allowed(c, CHAT_PERM_DEV)) return;
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

static void cmd_summon(ChatCtx *c) {
    if (!allowed(c, CHAT_PERM_DEV)) return;
    reply(c, "Summon unavailable on this game instance."); // no Soul Sanctum AI natively
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

static void cmd_ais(ChatCtx *c) {
    reply(c, "No AI."); // no AI entities natively
}

// v3.1.1 `cmds`, in its order; descriptions from the v3.1.1 dev console mod.
static const ChatCommand COMMANDS[] = {
    {{"c", NULL, "", "show connected players and menu count", CHAT_PERM_ANY}, cmd_c},
    {{"count", "c", "", "alias for /c (show connected players and menu count)", CHAT_PERM_ANY}, cmd_c},
    {{"kick", NULL, "<name|id>", "kick a player", CHAT_PERM_DEV}, cmd_kick},
    {{"ban", NULL, "<name|id>", "ban a player account", CHAT_PERM_DEV}, cmd_ban},
    {{"unban", NULL, "<accountId|name|id>", "unban an account", CHAT_PERM_DEV}, cmd_unban},
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
    {{"god", NULL, "[target] [on|off]", "toggle god mode (self or target)", CHAT_PERM_CHEAT}, cmd_god},
    {{"smite", NULL, "[target] [on|off]", "toggle smite-on-hit (self or target)", CHAT_PERM_CHEAT}, cmd_smite},
    {{"noclip", NULL, "[target] [on|off]", "toggle noclip (self or target)", CHAT_PERM_CHEAT}, cmd_noclip},
    {{"kill", NULL, "[target]", "kill yourself or every matching player", CHAT_PERM_CHEAT}, cmd_kill},
    {{"team", NULL, "<red|blue>", "switch teams", CHAT_PERM_ANY}, cmd_team},
    {{"teams", NULL, "", "list teams and players", CHAT_PERM_ANY}, cmd_teams},
    {{"forcehide", NULL, "[target] [on|off]", "toggle forcehide (self or target)", CHAT_PERM_DEV}, cmd_forcehide},
    {{"unlimitedammo", NULL, "[target] [on|off]", "toggle unlimited ammo (self or target)", CHAT_PERM_DEV}, cmd_unlimitedammo},
    {{"score", NULL, "[amount]", "add score (default +100)", CHAT_PERM_DEV}, cmd_score},
    {{"summon", NULL, "[name]", "spawn a Soul Sanctum AI by name (no arg lists summonable names)", CHAT_PERM_DEV}, cmd_summon},
    {{"players", NULL, "", "list connected players (id, name, team)", CHAT_PERM_ANY}, cmd_players},
    {{"ais", NULL, "", "list active AI entities (sid, name)", CHAT_PERM_ANY}, cmd_ais},
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
    if (sender_id != CHAT_CMD_CONSOLE && peer_by_id(host, sender_id, &c.self)) {
        c.has_self = 1;
        c.player = c.self.player;
    }

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
