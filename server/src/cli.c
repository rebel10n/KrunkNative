#include <server_cli.h>
#include <server_accounts.h>
#include <server_config.h>
#include <brain.h>
#include <map_catalog.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef WIN32
#include <direct.h>
#define CLI_PATH_SEPARATOR '\\'
#else
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#define CLI_PATH_SEPARATOR '/'
#endif

#define CLI_PROMPT "\x1b[1;32mkrunk\x1b[0m> "
#define CLI_PROMPT_PLAIN "krunk> "
#define CLI_HISTORY_MAX 1000
#define CLI_PATH_MAX 1024

// Private key code emulated by cli_stop() to unblock replxx_input() from the
// main thread. F24 is effectively never sent by real terminals; a stray press
// while the server is running is ignored.
#define CLI_WAKE_KEY REPLXX_KEY_F24

static int g_cli_interactive;
static int g_cli_thread_running;
static pthread_t g_cli_thread;
static char g_cli_history_path[CLI_PATH_MAX];

static pthread_mutex_t g_stop_lock = PTHREAD_MUTEX_INITIALIZER;
static ServerStopReason g_stop_reason = SERVER_STOP_NONE;
static char g_stop_detail[96];

// ------------------------------------------------------------------ stop reason

void server_request_stop(const ServerStopReason reason, const char *detail) {
    pthread_mutex_lock(&g_stop_lock);

    if (g_stop_reason == SERVER_STOP_NONE) {
        g_stop_reason = reason;
        snprintf(g_stop_detail, sizeof(g_stop_detail), "%s", detail ? detail : "");
    }

    pthread_mutex_unlock(&g_stop_lock);
    g_server->should_quit = 1;
}

ServerStopReason server_stop_reason(const char **detail) {
    pthread_mutex_lock(&g_stop_lock);
    const ServerStopReason reason = g_stop_reason;
    pthread_mutex_unlock(&g_stop_lock);

    if (detail) *detail = g_stop_detail;
    return reason;
}

// ------------------------------------------------------------------ helpers

static const char *cli_skip_spaces(const char *s) {
    while (*s && isspace((unsigned char) *s)) s++;
    return s;
}

static int cli_utf8_length(const char *s) {
    int count = 0;
    for (; *s; s++) {
        if (((unsigned char) *s & 0xC0) != 0x80) count++;
    }
    return count;
}

// Lowercase alphanumerics only, so "Soul Sanctum", "soul_sanctum" and
// "soulsanctum" all compare equal.
static void cli_normalize(const char *in, char *out, const size_t size) {
    size_t n = 0;

    for (; *in && n + 1 < size; in++) {
        const unsigned char c = (unsigned char) *in;
        if (isalnum(c)) out[n++] = (char) tolower(c);
    }

    out[n] = 0;
}

// Completion key for a display name: lowercase, spaces become underscores.
static void cli_completion_key(const char *in, char *out, const size_t size) {
    size_t n = 0;

    for (; *in && n + 1 < size; in++) {
        const unsigned char c = (unsigned char) *in;
        if (isalnum(c) || c == '_' || c == '-') out[n++] = (char) tolower(c);
        else if (c == ' ' && n && out[n - 1] != '_') out[n++] = '_';
    }

    out[n] = 0;
}

static int cli_starts_with_ci(const char *s, const char *prefix, const size_t prefix_len) {
    for (size_t i = 0; i < prefix_len; i++) {
        if (!s[i] || tolower((unsigned char) s[i]) != tolower((unsigned char) prefix[i])) return 0;
    }
    return 1;
}

static int cli_parse_int(const char *s, long *out) {
    if (!s || !*s) return 0;

    char *end = NULL;
    errno = 0;
    const long value = strtol(s, &end, 10);
    if (errno || !end || *cli_skip_spaces(end)) return 0;

    *out = value;
    return 1;
}

static void cli_format_duration(const double seconds, char *out, const size_t size) {
    long total = seconds > 0 ? (long) seconds : 0;
    const long days = total / 86400;
    const long hours = total / 3600 % 24;
    const long minutes = total / 60 % 60;
    const long secs = total % 60;

    if (days) snprintf(out, size, "%ldd %02ldh %02ldm %02lds", days, hours, minutes, secs);
    else if (hours) snprintf(out, size, "%ldh %02ldm %02lds", hours, minutes, secs);
    else if (minutes) snprintf(out, size, "%ldm %02lds", minutes, secs);
    else snprintf(out, size, "%lds", secs);
}

// ------------------------------------------------------------------ game state (caller holds g_server->lock)

static const char *cli_map_name(const size_t index) {
    if (index >= g_server->game.map_count || !g_server->game.maps[index]) return "?";

    const cJSON *name = cJSON_GetObjectItem(g_server->game.maps[index], "name");
    return cJSON_IsString(name) && name->valuestring ? name->valuestring : "?";
}

static const char *cli_mode_name(const int mode_id) {
    return mode_name(mode_id); // Krunker v3.1.1 mode ids (shared/src/modes)
}

static int cli_current_mode_id(void) {
    const Game *game = &g_server->game;
    const int index = game->current_mode_index;
    return index >= 0 && (size_t) index < game->mode_count && game->modes ? game->modes[index] : -1;
}

static const char *cli_class_name(const int class_index) {
    const Game *game = &g_server->game;
    if (class_index < 0 || (size_t) class_index >= game->class_count || !game->classes) return "?";
    return game->classes[class_index].name ? game->classes[class_index].name : "?";
}

// A rotation index, else a catalogue map by name (map_catalog_resolve: quotes, case,
// spaces/underscores, unique prefix) joined to the game's list like the chat /map command
// (map_catalog_slot; the rotation leaves out Shipyard and Soul Sanctum), else a list map by
// name (custom_map_dirs). -1 = none, -2 = ambiguous (the candidates were logged).
static int cli_find_map(const char *query) {
    long index;
    if (cli_parse_int(query, &index)) {
        return index >= 0 && (size_t) index < g_server->game.map_count ? (int) index : -1;
    }

    int cands[64];
    size_t cand_n = 0;
    const int entry = map_catalog_resolve(query, cands, sizeof(cands) / sizeof(cands[0]), &cand_n);
    if (entry == -2) {
        server_log("Map '%s' is ambiguous (%lu matches):", query, (unsigned long) cand_n);
        for (size_t i = 0; i < cand_n && i < sizeof(cands) / sizeof(cands[0]); i++) {
            const MapCatalogEntry *e = map_catalog_at((size_t) cands[i]);
            server_log("  %s", e->source == MAP_SOURCE_OFFICIAL ? e->name : e->key);
        }
        return -2;
    }
    if (entry >= 0) {
        const int slot = map_catalog_slot(&g_server->game.maps, &g_server->game.map_count, (size_t) entry);
        if (slot >= 0) return slot;
    }

    char wanted[64];
    cli_normalize(query, wanted, sizeof(wanted));
    if (!wanted[0]) return -1;

    int prefix_match = -1;
    int prefix_matches = 0;

    for (size_t i = 0; i < g_server->game.map_count; i++) {
        char name[64];
        cli_normalize(cli_map_name(i), name, sizeof(name));

        if (!strcmp(name, wanted)) return (int) i;

        if (!strncmp(name, wanted, strlen(wanted))) {
            prefix_match = (int) i;
            prefix_matches++;
        }
    }

    return prefix_matches == 1 ? prefix_match : -1;
}

static ClientConnection *cli_find_client(const char *query) {
    long id;
    const int numeric = cli_parse_int(query, &id);

    for (size_t i = 0; i < g_server->client_count; i++) {
        ClientConnection *client = &g_server->clients[i];
        if (numeric && client->id == id) return client;
    }

    for (size_t i = 0; i < g_server->client_count; i++) {
        ClientConnection *client = &g_server->clients[i];
        char a[SERVER_PLAYER_NAME_MAX];
        char b[SERVER_PLAYER_NAME_MAX];

        cli_normalize(client->name, a, sizeof(a));
        cli_normalize(query, b, sizeof(b));
        if (b[0] && !strcmp(a, b)) return client;
    }

    return NULL;
}

// ------------------------------------------------------------------ commands

typedef enum {
    CLI_ARG_NONE,
    CLI_ARG_COMMAND,
    CLI_ARG_PLAYER,
    CLI_ARG_MAP,
    CLI_ARG_TEXT,
    CLI_ARG_MODE,
} CliArgKind;

typedef struct {
    const char *const *names; // NULL-terminated, first is canonical
    const char *usage;
    const char *summary;
    CliArgKind arg_kind;
    void (*run)(const char *args);
} CliCommand;

static void cli_cmd_help(const char *args);
static void cli_cmd_status(const char *args);
static void cli_cmd_players(const char *args);
static void cli_cmd_kick(const char *args);
static void cli_cmd_map(const char *args);
static void cli_cmd_mode(const char *args);
#ifdef SERVER_HAVE_NET_CHAT
static void cli_cmd_say(const char *args);
#endif
static void cli_cmd_round(const char *args);
static void cli_cmd_op(const char *args);
static void cli_cmd_deop(const char *args);
static void cli_cmd_clear(const char *args);
static void cli_cmd_quit(const char *args);
static void cli_cmd_ban(const char *args);
static void cli_cmd_drain(const char *args);
static void cli_cmd_send(const char *args);
static void cli_execute(const char *line);

#define CLI_NAMES(...) ((const char *const[]) {__VA_ARGS__, NULL})

static const CliCommand g_cli_commands[] = {
    {CLI_NAMES("help", "?"), "help [command]", "List commands, or show help for one command", CLI_ARG_COMMAND, cli_cmd_help},
    {CLI_NAMES("status"), "status", "Show version, port, map, mode, players, uptime and tick rate", CLI_ARG_NONE, cli_cmd_status},
    {CLI_NAMES("players", "list"), "players", "List connected players (id, slot, name, class, state, address)", CLI_ARG_NONE, cli_cmd_players},
    {CLI_NAMES("kick"), "kick <id|name>", "Disconnect a player (use the id shown by 'players')", CLI_ARG_PLAYER, cli_cmd_kick},
    {CLI_NAMES("ban"), "ban <id|name>", "Kick a player and ban their account (guests: their IP); saved to bans_file", CLI_ARG_PLAYER, cli_cmd_ban},
    {CLI_NAMES("map", "maps", "changelevel"), "map [name|index]", "List the map rotation, or switch every client to another map", CLI_ARG_MAP, cli_cmd_map},
    {CLI_NAMES("mode", "modes"), "mode [alias|index]", "List the game modes, or restart the map in another mode (kept for the next rounds)", CLI_ARG_MODE, cli_cmd_mode},
#ifdef SERVER_HAVE_NET_CHAT
    {CLI_NAMES("say"), "say <message>", "Send a chat message to every player", CLI_ARG_TEXT, cli_cmd_say},
#endif
    {CLI_NAMES("round"), "round [end | time <s> | length <s>]", "Show the round clock, end the round now, set the seconds left, or the length of later rounds (0 = mode default)", CLI_ARG_TEXT, cli_cmd_round},
    {CLI_NAMES("op"), "op <id|name>", "Let a player run DEV chat commands and cheats", CLI_ARG_PLAYER, cli_cmd_op},
    {CLI_NAMES("deop"), "deop <id|name>", "Take op away again", CLI_ARG_PLAYER, cli_cmd_deop},
    {CLI_NAMES("clear", "cls"), "clear", "Clear the screen", CLI_ARG_NONE, cli_cmd_clear},
    {CLI_NAMES("send", "move"), "send <id|name|*> <gameId|host:port> [reason]", "Move player(s) to another server of the network (lobby id from the coordinator, or a direct address)", CLI_ARG_PLAYER, cli_cmd_send},
    {CLI_NAMES("drain"), "drain", "Refuse new players, finish the round, then stop (like SIGTERM)", CLI_ARG_NONE, cli_cmd_drain},
    {CLI_NAMES("quit", "exit", "stop"), "quit", "Disconnect everyone and stop the server now (also Ctrl+C / Ctrl+D)", CLI_ARG_NONE, cli_cmd_quit},
};

#define CLI_COMMAND_COUNT (sizeof(g_cli_commands) / sizeof(g_cli_commands[0]))

static const CliCommand *cli_find_command(const char *name, const size_t length) {
    for (size_t i = 0; i < CLI_COMMAND_COUNT; i++) {
        for (const char *const *alias = g_cli_commands[i].names; *alias; alias++) {
            if (strlen(*alias) == length && cli_starts_with_ci(*alias, name, length)) return &g_cli_commands[i];
        }
    }
    return NULL;
}

static void cli_print_aliases(const CliCommand *command) {
    if (!command->names[1]) return;

    char buffer[128] = "";
    size_t used = 0;

    for (const char *const *alias = command->names + 1; *alias && used < sizeof(buffer); alias++) {
        used += (size_t) snprintf(buffer + used, sizeof(buffer) - used, "%s%s", used ? ", " : "", *alias);
    }

    server_log("  aliases: %s", buffer);
}

static void cli_cmd_help(const char *args) {
    if (*args) {
        const char *end = args;
        while (*end && !isspace((unsigned char) *end)) end++;

        const CliCommand *command = cli_find_command(args, (size_t) (end - args));
        if (!command) {
            char name[32];
            snprintf(name, sizeof(name), "%.*s", (int) (end - args), args);
            const ChatCommandInfo *chat = chat_command_find(name);
            if (chat) server_log("%s %s\n  %s (chat command /%s)", chat->name, chat->usage, chat->desc, chat->name);
            else server_log("Unknown command '%.*s'. Type 'help' for a list.", (int) (end - args), args);
            return;
        }

        server_log("%s\n  %s", command->usage, command->summary);
        cli_print_aliases(command);
        return;
    }

    server_log("Commands:");
    for (size_t i = 0; i < CLI_COMMAND_COUNT; i++) {
        server_log("  %-20s %s", g_cli_commands[i].usage, g_cli_commands[i].summary);
    }
    int chat_count = 0;
    const ChatCommandInfo *chat = chat_command_list(&chat_count);
    char line[256] = "";
    size_t used = 0;
    server_log("Krunker v3.1.1 chat commands (players type them as /cmd; here without the slash, as the server):");
    for (int i = 0; i < chat_count; i++) {
        if (cli_find_command(chat[i].name, strlen(chat[i].name))) continue; // the CLI's own version wins
        used += (size_t) snprintf(line + used, sizeof(line) - used, "%s%s", used ? " " : "  ", chat[i].name);
        if (used > 90 || i + 1 == chat_count) {
            server_log("%s", line);
            used = 0;
            line[0] = 0;
        }
    }
    if (used) server_log("%s", line);
    server_log("Tab completes commands, map names and player names; history is kept in %s",
               g_cli_history_path[0] ? g_cli_history_path : "(disabled)");
}

static void cli_cmd_status(const char *args) {
    (void) args;

    pthread_mutex_lock(&g_server->lock);

    const Game *game = &g_server->game;
    size_t in_game = 0;

    for (size_t i = 0; i < g_server->client_count; i++) {
        const Player *player = g_server->clients[i].player;
        if (player && player->active) in_game++;
    }

    char uptime[48];
    cli_format_duration(g_server->now, uptime, sizeof(uptime));

    const int mode_id = cli_current_mode_id();

    server_log("KrunkNative server v%d.%d.%d, listening on port %u",
               g_server_version.major, g_server_version.minor, g_server_version.patch,
               (unsigned int) g_server->config.listen_port);
    server_log("  uptime     %s", uptime);
    if (game->ready) {
        server_log("  map        %s (%d/%lu)", cli_map_name((size_t) game->current_map_index), game->current_map_index, (unsigned long) game->map_count);
        server_log("  mode       %s (id %d)", cli_mode_name(mode_id), mode_id);
    } else {
        server_log("  map        (no map loaded)");
    }
    server_log("  players    %lu/%u connected, %lu in game", (unsigned long) g_server->client_count, (unsigned int) g_server->config.max_players, (unsigned long) in_game);
    server_log("  tick rate  %u Hz target, %.1f Hz measured, slowest tick %.2f ms",
               (unsigned int) g_server->config.tick_rate, g_server_tick_stats.measured_tick_rate, g_server_tick_stats.max_tick_ms);
    server_log("  idle kick  %.0f s", g_server->config.idle_timeout);
    server_log("  hostname   %s%s", g_gs_config.name, g_gs_config.password[0] ? " (password)" : "");
    server_log("  auth       %s", g_gs_config.online_mode ? "online mode (accounts verified)" : "offline mode (guests)");
    server_log("  network    %s%s%s", g_gs_config.brain_url[0] ? "coordinator " : "standalone",
               g_gs_config.brain_url[0] ? (brain_linked() ? "linked" : "not linked") : "",
               accounts_community_name()[0] ? ", community set" : "");
    server_log("  listing    %s", g_gs_config.finder_url[0] ? g_gs_config.finder_url : "unlisted (direct connect only)");

    pthread_mutex_unlock(&g_server->lock);
}

static void cli_cmd_players(const char *args) {
    (void) args;

    pthread_mutex_lock(&g_server->lock);

    if (!g_server->client_count) {
        server_log("No players connected.");
        pthread_mutex_unlock(&g_server->lock);
        return;
    }

    server_log("%4s %3s  %-20s %-14s %-6s %4s %6s  %s", "ID", "#", "NAME", "CLASS", "STATE", "HP", "IDLE", "ADDRESS");

    for (size_t i = 0; i < g_server->client_count; i++) {
        const ClientConnection *client = &g_server->clients[i];
        const Player *player = client->player;
        const char *state = !player ? "lobby" : player->active ? "alive" : "dead";
        const float idle_since = client->spawned_at > 0.0f ? client->last_significant_move_at : client->connected_at;

        char idle[24];
        char health[16] = "-";
        cli_format_duration(g_server->now - idle_since, idle, sizeof(idle));
        if (player && player->active) snprintf(health, sizeof(health), "%d", (int) player->health);

        server_log("%4d %3d  %-20s %-14s %-6s %4s %6s  %s:%u",
                   client->id, client->player_number, client->name, cli_class_name(client->class_index), state, health, idle,
                   inet_ntoa(client->addr.sin_addr), (unsigned int) ntohs(client->addr.sin_port));
    }

    server_log("%lu/%u players", (unsigned long) g_server->client_count, (unsigned int) g_server->config.max_players);
    pthread_mutex_unlock(&g_server->lock);
}

static void cli_cmd_kick(const char *args) {
    if (!*args) {
        server_log("Usage: kick <id|name>   (see 'players' for ids)");
        return;
    }

    pthread_mutex_lock(&g_server->lock);

    ClientConnection *client = cli_find_client(args);

    if (!client) {
        server_log("No player matches '%s'. Use 'players' to list ids.", args);
    } else {
        char name[SERVER_PLAYER_NAME_MAX];
        const int id = client->id;

        snprintf(name, sizeof(name), "%s", client->name);
        net_remove_client(client);
        server_log("Kicked %s (id %d)", name, id);
    }

    pthread_mutex_unlock(&g_server->lock);
}

// mode [alias|index]: the list, or Krunker host config.modes = [id] and the current map
// restarted in that mode (every client gets INIT).
static void cli_cmd_mode(const char *args) {
    pthread_mutex_lock(&g_server->lock);

    Game *game = &g_server->game;
    const int current = cli_current_mode_id();

    if (!*args) {
        int count = 0;
        const GameModeConfig *modes = mode_list(&count);
        server_log("Game modes (%d):", count);
        for (int i = 0; i < count; i++) {
            server_log("  %c %2d  %-7s %s%s", i == current ? '*' : ' ', i, modes[i].alias, modes[i].name,
                       mode_hostable(i) ? "" : " (not hostable)");
        }
        server_log("Use 'mode <alias|index>' to switch.");
        pthread_mutex_unlock(&g_server->lock);
        return;
    }

    const int mode_id = mode_find(args);
    if (mode_id < 0) {
        server_log("No mode matches '%s'. Type 'mode' to list them.", args);
        pthread_mutex_unlock(&g_server->lock);
        return;
    }

    int *modes = calloc(1, sizeof(int));
    if (!modes) {
        pthread_mutex_unlock(&g_server->lock);
        return;
    }

    modes[0] = mode_id;
    free(game->modes);
    game->modes = modes;
    game->mode_count = 1;

#ifdef SERVER_HAVE_NET_CHANGE_MAP
    server_log("Changing mode to %s...", mode_name(mode_id));
    net_change_map(game->ready ? game->current_map_index : 0, 0);
#else
    server_log("Mode set to %s for the next round.", mode_name(mode_id));
#endif

    pthread_mutex_unlock(&g_server->lock);
}

static void cli_cmd_map(const char *args) {
    pthread_mutex_lock(&g_server->lock);

    const Game *game = &g_server->game;

    if (!*args) {
        server_log("Map rotation (%lu):", (unsigned long) game->map_count);
        for (size_t i = 0; i < game->map_count; i++) {
            const int current = game->ready && (int) i == game->current_map_index;
            server_log("  %c %2lu  %s", current ? '*' : ' ', (unsigned long) i, cli_map_name(i));
        }
        server_log("Use 'map <name|index>' to switch.");
        pthread_mutex_unlock(&g_server->lock);
        return;
    }

    const int index = cli_find_map(args);

    if (index == -2) {
        pthread_mutex_unlock(&g_server->lock);
        return;
    }
    if (index < 0) {
        server_log("No unique map matches '%s'. Type 'map' to list them.", args);
        pthread_mutex_unlock(&g_server->lock);
        return;
    }

#ifdef SERVER_HAVE_NET_CHANGE_MAP
    server_log("Changing map to %s...", cli_map_name((size_t) index));
    net_change_map(index, g_server->game.current_mode_index);
#else
    server_log("Cannot switch to %s: this build has no net_change_map() (server/src/net.c).", cli_map_name((size_t) index));
#endif

    pthread_mutex_unlock(&g_server->lock);
}

#ifdef SERVER_HAVE_NET_CHAT
static void cli_cmd_say(const char *args) {
    if (!*args) {
        server_log("Usage: say <message>");
        return;
    }

    pthread_mutex_lock(&g_server->lock);
    net_broadcast_chat("Server", args);
    pthread_mutex_unlock(&g_server->lock);
    server_log("[Server] %s", args);
}
#endif

// round: debug controls for the match clock (shared/src/game.c).
static void cli_cmd_round(const char *args) {
    pthread_mutex_lock(&g_server->lock);
    Game *game = &g_server->game;
    char word[16] = "";
    float seconds = 0.0f;
    const int parsed = sscanf(args, "%15s %f", word, &seconds);

    if (parsed <= 0) {
        if (game->end_timer > 0.0f) server_log("End screen: next round in %.0f s", game->end_timer);
        else server_log("Round: %.0f s left (length %s)", game->round_timer, game->round_length > 0.0f ? "override" : "mode default");
    } else if (!strcmp(word, "end")) {
        game_set_round_time(game, 0.0f);
        server_log("Ending the round");
    } else if (!strcmp(word, "time") && parsed == 2) {
        game_set_round_time(game, seconds);
        server_log("Round clock set to %.0f s", seconds);
    } else if (!strcmp(word, "length") && parsed == 2 && seconds >= 0.0f) {
        game->round_length = seconds;
        server_log("Later rounds last %s", seconds > 0.0f ? "the given seconds" : "the mode's game time");
    } else {
        server_log("Usage: round [end | time <s> | length <s>]");
    }

    pthread_mutex_unlock(&g_server->lock);
}

static void cli_set_op(const char *args, const int op) {
    if (!*args) {
        server_log("Usage: %s <id|name>", op ? "op" : "deop");
        return;
    }
    pthread_mutex_lock(&g_server->lock);
    ClientConnection *client = cli_find_client(args);
    if (!client) {
        server_log("No player matches '%s'. Use 'players' to list ids.", args);
    } else {
        client->op = (unsigned char) op;
        server_log("%s %s (id %d)%s", op ? "Opped" : "De-opped", client->name, client->id,
                   g_server->config.perms ? "" : g_server->config.cheats_open ? " (perms are off: everyone may run every command)"
                                                                                  : " (perms are off: ops only add the cheats)");
    }
    pthread_mutex_unlock(&g_server->lock);
}

static void cli_cmd_op(const char *args) { cli_set_op(args, 1); }
static void cli_cmd_deop(const char *args) { cli_set_op(args, 0); }

static void cli_cmd_clear(const char *args) {
    (void) args;
    replxx_clear_screen(g_replxx);
}

static void cli_cmd_quit(const char *args) {
    (void) args;
    server_request_stop(SERVER_STOP_COMMAND, NULL);
}

static void cli_cmd_drain(const char *args) {
    (void) args;
    server_request_drain("console drain");
}

// ban: the account (bans_file "accounts", /ban) and, for guests, the address (/banip).
static void cli_cmd_ban(const char *args) {
    if (!*args) {
        server_log("Usage: ban <id|name>   (see 'players' for ids)");
        return;
    }
    pthread_mutex_lock(&g_server->lock);
    ClientConnection *client = cli_find_client(args);
    if (!client) {
        server_log("No player matches '%s'. Use 'players' to list ids.", args);
        pthread_mutex_unlock(&g_server->lock);
        return;
    }
    if (client->acct.account_id > 0) {
        accounts_ban_account(client->acct.account_id, client->acct.username);
        server_log("Banned %s (account %d)", client->name, client->acct.account_id);
    } else { // a guest: its address (bans_file "ips", like /banip)
        char chat[NET_CHAT_TEXT_SIZE];
        snprintf(chat, sizeof(chat), "/banip %s", client->address);
        chat_command_handle(net_chat_host(), CHAT_CMD_CONSOLE, chat);
        server_log("Banned guest %s (ip %s)", client->name, client->address);
    }
    net_kick_client(client->id, "Banned", "004003");
    pthread_mutex_unlock(&g_server->lock);
}

// send: NET_PACKET_REDIRECT. "NODE:lobby" = a coordinator lobby (the client seeks it and
// gets a fresh ticket); anything with a port after the last ':' and no letters in the
// lobby part is host:port (direct connect).
static void cli_cmd_send(const char *args) {
    char who[64] = "", target[160] = "", reason[64] = "";
    if (sscanf(args, "%63s %159s %63[^\n]", who, target, reason) < 2) {
        server_log("Usage: send <id|name|*> <gameId|host:port> [reason]");
        return;
    }
    NetRedirectPacket packet;
    memset(&packet, 0, sizeof(packet));
    const char *colon = strrchr(target, ':');
    char *end = NULL;
    const long port = colon ? strtol(colon + 1, &end, 10) : 0;
    if (colon && end && !*end && port > 0 && port < 65536) {
        snprintf(packet.host, sizeof(packet.host), "%.*s", (int) (colon - target), target);
        packet.port = (uint16_t) port;
    } else if (colon) {
        snprintf(packet.game_id, sizeof(packet.game_id), "%s", target);
    } else {
        server_log("'%s' is neither a lobby id (NODE:lobby) nor host:port", target);
        return;
    }
    snprintf(packet.reason, sizeof(packet.reason), "%s", reason[0] ? reason : "Moving to another server...");
    pthread_mutex_lock(&g_server->lock);
    int sent = 0;
    for (size_t i = 0; i < g_server->client_count; i++) {
        ClientConnection *client = &g_server->clients[i];
        if (strcmp(who, "*") && client != cli_find_client(who)) continue;
        net_send_packet(client->fd, NET_PACKET_REDIRECT, &packet, sizeof(packet));
        sent++;
    }
    pthread_mutex_unlock(&g_server->lock);
    if (!sent) server_log("No player matches '%s'.", who);
    else server_log("Sent %d player(s) to %s", sent, target);
}

int cli_run_command(const char *line, char *out, const size_t size) {
    server_log_capture_begin(out, size);
    cli_execute(line);
    server_log_capture_end();
    return 0;
}

static void cli_execute(const char *line) {
    const char *start = cli_skip_spaces(line);
    if (!*start) return;

    const char *end = start;
    while (*end && !isspace((unsigned char) *end)) end++;

    const CliCommand *command = cli_find_command(start, (size_t) (end - start));

    if (!command) {
        // Krunker's chat commands, run as the server console (every permission).
        char chat[NET_CHAT_TEXT_SIZE];
        snprintf(chat, sizeof(chat), "/%s", start);
        for (size_t n = strlen(chat); n && isspace((unsigned char) chat[n - 1]); n--) chat[n - 1] = 0;
        pthread_mutex_lock(&g_server->lock);
        const int handled = chat_command_handle(net_chat_host(), CHAT_CMD_CONSOLE, chat);
        pthread_mutex_unlock(&g_server->lock);
        if (!handled) server_log("Unknown command '%.*s'. Type 'help' for a list.", (int) (end - start), start);
        return;
    }

    // Trim trailing whitespace from the argument string.
    char args[512];
    snprintf(args, sizeof(args), "%s", cli_skip_spaces(end));
    for (size_t n = strlen(args); n && isspace((unsigned char) args[n - 1]); n--) args[n - 1] = 0;

    if (command->arg_kind == CLI_ARG_NONE && args[0]) {
        server_log("'%s' takes no arguments (usage: %s)", command->names[0], command->usage);
        return;
    }

    command->run(args);
}

// ------------------------------------------------------------------ completion

static void cli_add_completion_if(replxx_completions *completions, const char *candidate, const char *prefix, const size_t prefix_len) {
    if (cli_starts_with_ci(candidate, prefix, prefix_len)) replxx_add_completion(completions, candidate);
}

static void cli_complete(const char *input, replxx_completions *completions, int *context_len, void *user_data) {
    (void) user_data;

    const char *start = cli_skip_spaces(input);
    const char *word_end = start;
    while (*word_end && !isspace((unsigned char) *word_end)) word_end++;

    // Still typing the command word.
    if (!*word_end) {
        const size_t len = (size_t) (word_end - start);
        *context_len = cli_utf8_length(start);

        for (size_t i = 0; i < CLI_COMMAND_COUNT; i++) {
            for (const char *const *alias = g_cli_commands[i].names; *alias; alias++) {
                if (!strcmp(*alias, "?")) continue;
                cli_add_completion_if(completions, *alias, start, len);
            }
        }
        int chat_count = 0;
        const ChatCommandInfo *chat = chat_command_list(&chat_count);
        for (int i = 0; i < chat_count; i++) {
            if (!cli_find_command(chat[i].name, strlen(chat[i].name))) cli_add_completion_if(completions, chat[i].name, start, len);
        }
        return;
    }

    const CliCommand *command = cli_find_command(start, (size_t) (word_end - start));
    if (!command) return;

    // Argument being completed: from the last space to the cursor.
    const char *arg = input + strlen(input);
    while (arg > word_end && !isspace((unsigned char) arg[-1])) arg--;

    // Only the first argument is completed.
    if (cli_skip_spaces(word_end) != arg) return;

    const size_t len = strlen(arg);
    *context_len = cli_utf8_length(arg);

    switch (command->arg_kind) {
        case CLI_ARG_COMMAND:
            for (size_t i = 0; i < CLI_COMMAND_COUNT; i++) {
                cli_add_completion_if(completions, g_cli_commands[i].names[0], arg, len);
            }
            break;
        case CLI_ARG_MAP:
            pthread_mutex_lock(&g_server->lock);
            for (size_t i = 0; i < g_server->game.map_count; i++) {
                char key[64];
                cli_completion_key(cli_map_name(i), key, sizeof(key));
                cli_add_completion_if(completions, key, arg, len);
            }
            pthread_mutex_unlock(&g_server->lock);
            break;
        case CLI_ARG_MODE: {
            int count = 0;
            const GameModeConfig *modes = mode_list(&count);
            for (int i = 0; i < count; i++) cli_add_completion_if(completions, modes[i].alias, arg, len);
            break;
        }
        case CLI_ARG_PLAYER:
            pthread_mutex_lock(&g_server->lock);
            for (size_t i = 0; i < g_server->client_count; i++) {
                const ClientConnection *client = &g_server->clients[i];
                char id[16];
                snprintf(id, sizeof(id), "%d", client->id);
                cli_add_completion_if(completions, client->name, arg, len);
                if (len) cli_add_completion_if(completions, id, arg, len);
            }
            pthread_mutex_unlock(&g_server->lock);
            break;
        default:
            break;
    }
}

// ------------------------------------------------------------------ key bindings

// Ctrl+C at the prompt. The terminal is in raw mode (ISIG off) while replxx
// reads a line, so Ctrl+C arrives as a key, not SIGINT. Declared in server.h.
ReplxxActionResult cli_sigint(int code, void *user_data) {
    (void) user_data;
    server_request_stop(SERVER_STOP_INTERRUPT, NULL);
    // Prints "^C" + newline, drops the pending history entry, makes replxx_input return NULL.
    return replxx_invoke(g_replxx, REPLXX_ACTION_ABORT_LINE, (unsigned int) code);
}

// Emulated by cli_stop() from the main thread: erase the prompt line so
// shutdown logging starts on a clean line, then make replxx_input return NULL.
static ReplxxActionResult cli_on_wake(int code, void *user_data) {
    (void) user_data;
    if (!g_server->should_quit) return REPLXX_ACTION_RESULT_CONTINUE;

    replxx_invoke(g_replxx, REPLXX_ACTION_ABORT_LINE, (unsigned int) code);
    replxx_invoke(g_replxx, REPLXX_ACTION_CLEAR_SELF, 0);
    return REPLXX_ACTION_RESULT_BAIL;
}

// ------------------------------------------------------------------ history

static int cli_mkdirs(char *path) {
    for (char *p = path + 1; *p; p++) {
        if (*p != '/' && *p != '\\') continue;
#ifdef WIN32
        if (p[-1] == ':') continue; // drive root ("C:\\")
#endif

        const char saved = *p;
        *p = 0;
#ifdef WIN32
        const int failed = _mkdir(path) && errno != EEXIST;
#else
        const int failed = mkdir(path, 0700) && errno != EEXIST;
#endif
        *p = saved;
        if (failed) return 0;
    }

    return 1;
}

static int cli_resolve_history_path(char *out, const size_t size) {
#ifdef WIN32
    const char *base = getenv("LOCALAPPDATA");
    if (!base || !*base) base = getenv("APPDATA");
    if (!base || !*base) return 0;

    const int written = snprintf(out, size, "%s\\krunknative\\server_history", base);
#else
    const char *state = getenv("XDG_STATE_HOME");
    const char *home = getenv("HOME");
    int written;

    // XDG: relative paths in XDG_STATE_HOME are invalid and must be ignored.
    if (state && state[0] == '/') written = snprintf(out, size, "%s/krunknative/server_history", state);
    else if (home && *home) written = snprintf(out, size, "%s/.local/state/krunknative/server_history", home);
    else return 0;
#endif

    if (written < 0 || (size_t) written >= size) return 0;
    return cli_mkdirs(out);
}

// ------------------------------------------------------------------ thread / lifecycle

static void *cli_thread_main(void *arg) {
    (void) arg;
    const char *prompt = getenv("NO_COLOR") ? CLI_PROMPT_PLAIN : CLI_PROMPT;

    while (!g_server->should_quit) {
        errno = 0;
        const char *line = replxx_input(g_replxx, prompt);

        if (!line) {
            if (g_server->should_quit) break; // Ctrl+C, wake from cli_stop()
            if (errno == EAGAIN) continue;    // line aborted by some other binding

            // Ctrl+D on an empty line (or the terminal went away): finish the prompt line.
            replxx_write(g_replxx, "\n", 1);
            server_request_stop(SERVER_STOP_EOF, NULL);
            break;
        }

        if (*cli_skip_spaces(line)) replxx_history_add(g_replxx, line);
        cli_execute(line);
    }

    return NULL;
}

// Compatibility entry point (declared in server.h): run the prompt loop on the
// calling thread.
void cli_main() {
    cli_thread_main(NULL);
}

// Headless (stdin not a terminal: a pipe, a FIFO, docker attach, systemd): read plain
// command lines like srcds. EOF only ends the reader (systemd's /dev/null is EOF at once).
#ifndef WIN32
// poll(stdin, wake pipe) + read(2), never stdio: a thread blocked in fgets() holds the
// stdin FILE lock, and glibc's exit() (_IO_cleanup -> _IO_flush_all) waits for every
// stream lock, so the process hung after "server stopped" while stdin was an idle pipe
// or tty. cli_stop() writes the wake pipe and joins the reader.
static int g_stdin_wake[2] = {-1, -1};
static pthread_t g_stdin_thread;
static int g_stdin_thread_running;

static void cli_stdin_line(char *line, const int discard) {
    line[strcspn(line, "\r")] = 0;
    if (!discard && !g_server->should_quit) cli_execute(line);
}

static void *cli_stdin_main(void *arg) {
    (void) arg;
    char buf[512];
    size_t len = 0;
    int discard = 0; // the rest of an over-long line
    while (!g_server->should_quit) {
        struct pollfd fds[2] = {{.fd = STDIN_FILENO, .events = POLLIN}, {.fd = g_stdin_wake[0], .events = POLLIN}};
        if (poll(fds, 2, -1) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (fds[1].revents) break; // cli_stop()
        if (fds[0].revents & POLLNVAL) break;
        if (!(fds[0].revents & (POLLIN | POLLHUP | POLLERR))) continue;
        const ssize_t got = read(STDIN_FILENO, buf + len, sizeof(buf) - 1 - len);
        if (got < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (got <= 0) break; // EOF (or the terminal went away)
        len += (size_t) got;
        size_t start = 0;
        for (size_t i = 0; i < len; i++) {
            if (buf[i] != '\n') continue;
            buf[i] = 0;
            cli_stdin_line(buf + start, discard);
            discard = 0;
            start = i + 1;
        }
        memmove(buf, buf + start, len - start);
        len -= start;
        if (len == sizeof(buf) - 1) { // no newline in 511 bytes: drop the line
            discard = 1;
            len = 0;
        }
    }
    if (len && !g_server->should_quit) { // last line without a newline before EOF
        buf[len] = 0;
        cli_stdin_line(buf, discard);
    }
    return NULL;
}

static void cli_stdin_start(void) {
    if (pipe(g_stdin_wake)) return;
    for (int i = 0; i < 2; i++) fcntl(g_stdin_wake[i], F_SETFD, FD_CLOEXEC);
    if (pthread_create(&g_stdin_thread, NULL, cli_stdin_main, NULL)) {
        close(g_stdin_wake[0]);
        close(g_stdin_wake[1]);
        g_stdin_wake[0] = g_stdin_wake[1] = -1;
        return;
    }
    g_stdin_thread_running = 1;
}

static void cli_stdin_stop(void) {
    if (!g_stdin_thread_running) return;
    const char wake = 1;
    while (write(g_stdin_wake[1], &wake, 1) < 0 && errno == EINTR) {}
    pthread_join(g_stdin_thread, NULL);
    g_stdin_thread_running = 0;
    close(g_stdin_wake[0]);
    close(g_stdin_wake[1]);
    g_stdin_wake[0] = g_stdin_wake[1] = -1;
}
#else
// Windows: no poll() on console handles; the detached reader stays (its fgets may
// outlive shutdown, the CRT does not wait for it).
static void *cli_stdin_main(void *arg) {
    (void) arg;
    char line[512];
    while (!g_server->should_quit && fgets(line, sizeof(line), stdin)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!g_server->should_quit) cli_execute(line);
    }
    return NULL;
}

static void cli_stdin_start(void) {
    pthread_t reader;
    if (!pthread_create(&reader, NULL, cli_stdin_main, NULL)) pthread_detach(reader);
}

static void cli_stdin_stop(void) {}
#endif

int cli_start(const int interactive, const int stdin_console) {
    g_cli_interactive = interactive && g_replxx;
    if (!g_cli_interactive) {
        // --no-cli (and KN_NO_STDIN_CONSOLE): no console at all, stdin is never read
        if (stdin_console && !getenv("KN_NO_STDIN_CONSOLE")) cli_stdin_start();
        return 0;
    }

    replxx_set_completion_callback(g_replxx, cli_complete, NULL);
    replxx_set_ignore_case(g_replxx, 1);
    replxx_set_unique_history(g_replxx, 1);
    replxx_set_max_history_size(g_replxx, CLI_HISTORY_MAX);
    replxx_set_no_color(g_replxx, getenv("NO_COLOR") != NULL);
    replxx_bind_key(g_replxx, REPLXX_KEY_CONTROL('C'), cli_sigint, NULL);
    replxx_bind_key(g_replxx, CLI_WAKE_KEY, cli_on_wake, NULL);
#ifndef WIN32
    replxx_install_window_change_handler(g_replxx);
#endif

    if (cli_resolve_history_path(g_cli_history_path, sizeof(g_cli_history_path))) {
        replxx_history_load(g_replxx, g_cli_history_path); // missing file on first run is fine
    } else {
        g_cli_history_path[0] = 0;
    }

    server_log_set_replxx(g_replxx);

    if (pthread_create(&g_cli_thread, NULL, cli_thread_main, NULL)) {
        server_log_set_replxx(NULL);
        g_cli_interactive = 0;
        return -1;
    }

    g_cli_thread_running = 1;
    return 0;
}

int cli_is_interactive(void) {
    return g_cli_interactive;
}

void cli_stop(void) {
    cli_stdin_stop();
    if (!g_cli_thread_running) return;

    g_server->should_quit = 1;
    // Queued for replxx: picked up immediately if the thread is blocked in
    // replxx_input, or on its next read if it is between lines (it then exits
    // the loop on should_quit before reading anyway).
    replxx_emulate_key_press(g_replxx, CLI_WAKE_KEY);
    pthread_join(g_cli_thread, NULL);
    g_cli_thread_running = 0;
}

void cli_shutdown(void) {
    cli_stop();

    server_log_set_replxx(NULL);

    if (!g_replxx) return;

    if (g_cli_interactive && g_cli_history_path[0]) {
        replxx_history_save(g_replxx, g_cli_history_path);
    }

    replxx_end(g_replxx); // restores the terminal if it is still in raw mode
    g_replxx = NULL;
}
