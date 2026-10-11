// Krunker v3.1.1 developer console: a 1:1 port of the offline build's mod/devConsole.ts
// (client/include/console.h; command table and native notes in doc/console.md).
//
// The DOM is the one devConsole.ts builds (#devConsoleOverlay > .modal > .titlebar /
// .body (.logs, .inputShell (.hints, form.row)) / .grip), styled by its own stylesheet
// (ui/css/devconsole.css, added with html_ui_add_stylesheet). Its DOM listeners map to:
// keys -> ui_console_key_event (GLFW, every build) and ui_console_page_key (page
// keydown); the input's `input` event -> polling the field each frame; mousedown/move/up
// on the titlebar, grip, close button, Submit and hint items -> ui_console_mouse (main.c
// natively, kn-shim's data-kn-pointer glue on the web). The toggle key and the enable
// switch are mods.ts (ui_mods.c).
//
// Commands: the local ones run here; server ones go out as "/cmd" chat ("ct"), answered
// by shared/src/chat_commands.c on server_bin. In singleplayer devConsole runs its own
// offline versions of some of them (offlineServerCommands, ported below against the
// in-process game, client/src/net/local_game.c); the rest reach that game's chat handler.
#include <console.h>
#include <ui_mods.h>
#include <ui_logic.h>
#include <html_ui.h>
#include <chat_commands.h>
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#define CON_LOG_MAX 1000
#define CON_HISTORY_MAX 200
#define CON_HINTS_MAX 12
#define CON_CMDS_MAX 160
#define CON_BINDS_MAX 64
#define CON_LINE_MAX 1024
#define CON_ARGS_MAX 32
#define CON_NAME_MAX 64
#define CON_TEXT_MAX 512
#define CON_PENDING_MAX 64
#define CON_GEOM_KEY "krk_dev_console_geom"
#define CON_AHK_BASE 29819          // antihackkey.json
#define CON_VERSION "jhiLQ"         // version.json

// const COLORS (Source/HL2 console palette)
#define COL_LOG "#e0e0e0"
#define COL_INFO "#9ac8ff"
#define COL_WARN "#ffb84d"
#define COL_ERROR "#ff6a6a"
#define COL_CMD "#ffffff"
#define COL_REPLY "#9affb0"
#define COL_HINT "#b0b0b0"

void client_send_cycle_packet(Client *client); // main.c

// The overlay devConsole.ts builds and appends to <body>. KrunkNative additions: the
// data-kn-* glue attributes, and onsubmit="return false" (the mod's submit listener
// preventDefault()s; the browser runs the attribute, the native engine has no forms).
static const char OVERLAY_HTML[] =
    "<div id=\"devConsoleOverlay\"><div class=\"modal\" data-kn-pointer=\"1\">"
    "<div class=\"titlebar\"><span>Console</span><span class=\"x\">\xE2\x9C\x95</span></div>"
    "<div class=\"body\"><div class=\"logs\" tabindex=\"-1\" style=\"outline: none;\"></div>"
    "<div class=\"inputShell\"><div class=\"hints\"></div>"
    "<form class=\"row\" onsubmit=\"return false\"><input type=\"text\" autocomplete=\"off\" spellcheck=\"false\" "
    "data-kn-keys=\"1\"><button type=\"submit\">Submit</button></form></div></div>"
    "<div class=\"grip\"></div></div></div>";

// ---------------------------------------------------------------- command registry

typedef enum { ARG_STRING, ARG_NUMBER, ARG_BOOLEAN } ArgType;

typedef struct {
    const char *name;
    ArgType type;
    int optional;
} ConArg;

typedef void (*ConRun)(int argc, char **argv, const char *rest);

typedef struct {
    char name[CON_NAME_MAX];
    int server;                  // kind: "server" (sent as "/name ...")
    const ConArg *args;
    int nargs;
    char desc[CON_TEXT_MAX];
    ConRun run;
    int alias;                   // index into g_con.aliases (alias commands), -1 none
} ConCommand;

typedef struct {
    char key[CON_NAME_MAX];
    char value[CON_TEXT_MAX];
} ConPair;

typedef struct {
    Client *client;
    int built;                   // the overlay exists in the current page
    HtmlElem overlay, modal, header, close_btn, logs, hints, input, submit, grip;
    int visible;
    int focus_pending;           // update(): setTimeout(() => input.focus(), 0)
    char last_value[CON_LINE_MAX];
    int log_lines;
    int banner;

    char *pending[CON_PENDING_MAX]; // lines appended before the overlay exists (the banner)
    int pending_count;

    ConCommand cmds[CON_CMDS_MAX];
    int cmd_count;
    ConPair binds[CON_BINDS_MAX];     // keyBinds (session only)
    int bind_count;
    ConPair aliases[CON_BINDS_MAX];   // aliases (session only)
    int alias_count;

    char history[CON_HISTORY_MAX][CON_LINE_MAX];
    int history_count;
    int history_idx;
    char draft[CON_LINE_MAX];

    char hint_names[CON_HINTS_MAX][CON_NAME_MAX]; // hint items that complete on mousedown
    int hint_count;

    int mode_override;           // offlineModeOverrides (game mode id), -1 none

    // drag / resize / clicks
    int dragging, resizing;
    float dx, dy, start_x, start_y, start_w, start_h;
    HtmlElem pressed;
    int save_timer;
    int user_size;               // width/height come from the saved geometry or the grip
    float css_vw, css_vh;        // viewport the min() size was computed for
} ConState;

static ConState g_con = {.history_idx = -1, .mode_override = -1};

static void con_exec(const char *line);

// ---------------------------------------------------------------- helpers

static Client *con_client(void) {
    if (!g_con.client) g_con.client = menu_ui()->client;
    return g_con.client;
}

static Player *con_player(void) {
    const Client *client = con_client();
    return client ? client->me : NULL;
}

static int con_offline(void) {           // Game.singlePlayer
    const Client *client = con_client();
    return client && client->offline;
}

static void lower(char *s) {
    for (; *s; s++) *s = (char) tolower((unsigned char) *s);
}

static void trim(char *s) {
    char *start = s;
    while (*start && isspace((unsigned char) *start)) start++;
    if (start != s) memmove(s, start, strlen(start) + 1);
    for (size_t n = strlen(s); n && isspace((unsigned char) s[n - 1]); n--) s[n - 1] = 0;
}

static int starts_with(const char *s, const char *prefix) {
    return !strncmp(s, prefix, strlen(prefix));
}

// a.join(" ")
static void join_args(const int argc, char **argv, const int from, char *out, const size_t n) {
    size_t len = 0;
    out[0] = 0;
    for (int i = from; i < argc && len < n; i++) len += (size_t) snprintf(out + len, n - len, "%s%s", i > from ? " " : "", argv[i]);
}

// Number(value) for a finite number (an empty string is 0)
static int js_number(const char *s, double *out) {
    char *end = NULL;
    while (*s && isspace((unsigned char) *s)) s++;
    if (!*s) {
        *out = 0.0;
        return 1;
    }
    const double d = strtod(s, &end);
    while (end && *end && isspace((unsigned char) *end)) end++;
    if (!end || *end || d != d || isinf(d)) return 0;
    *out = d;
    return 1;
}

static void escape_html(char *out, const size_t n, const char *text) {
    size_t len = 0;
    for (const char *p = text; *p && len + 7 < n; p++) {
        const char *rep = NULL;
        switch (*p) {
            case '&': rep = "&amp;"; break;
            case '<': rep = "&lt;"; break;
            case '>': rep = "&gt;"; break;
            case '"': rep = "&quot;"; break;
            default: break;
        }
        if (rep) {
            const size_t r = strlen(rep);
            memcpy(out + len, rep, r);
            len += r;
        } else {
            out[len++] = *p;
        }
    }
    out[len] = 0;
}

// ---------------------------------------------------------------- output sink

// append(text, kind): one <div> in the kind's colour (textContent); at most 1000 lines.
static void append(const char *text, const char *color) {
    static char escaped[4 * CON_LINE_MAX];
    static char html[4 * CON_LINE_MAX + 64];
    escape_html(escaped, sizeof(escaped), text ? text : "");
    snprintf(html, sizeof(html), "<div style=\"color: %s;\">%s</div>", color, escaped);
#ifndef __EMSCRIPTEN__
    printf("[console] %s\n", text ? text : ""); // headless runs and logs see the console too
#endif
    if (!g_con.built) {
        if (g_con.pending_count == CON_PENDING_MAX) {
            free(g_con.pending[0]);
            memmove(g_con.pending, g_con.pending + 1, sizeof(g_con.pending[0]) * (CON_PENDING_MAX - 1));
            g_con.pending_count--;
        }
        g_con.pending[g_con.pending_count++] = strdup(html);
        return;
    }
    html_dom_insert_html(g_con.logs, HTML_INSERT_BEFORE_END, html);
    if (++g_con.log_lines > CON_LOG_MAX) {
        const HtmlElem first = html_dom_first_child(g_con.logs);
        if (first) html_dom_remove(first);
        g_con.log_lines--;
    }
    html_dom_scroll_to_bottom(g_con.logs);
}

static void appendf(const char *color, const char *fmt, ...) {
    char text[CON_LINE_MAX * 2];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    append(text, color);
}

void ui_console_printf(const char *color, const char *fmt, ...) {
    char text[CON_LINE_MAX * 2];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    append(text, color ? color : COL_LOG);
}

static void flush_pending(void) {
    for (int i = 0; i < g_con.pending_count; i++) {
        html_dom_insert_html(g_con.logs, HTML_INSERT_BEFORE_END, g_con.pending[i]);
        g_con.log_lines++;
        free(g_con.pending[i]);
    }
    g_con.pending_count = 0;
}

// hookChat: `${name || "Server"}: ${text without tags}` for every "ch".
void ui_console_on_chat(const char *name, const char *text, const int kind) {
    (void) kind;
    char cleaned[CON_LINE_MAX];
    size_t len = 0;
    int tag = 0;
    for (const char *p = text ? text : ""; *p && len + 1 < sizeof(cleaned); p++) {
        if (*p == '<') tag = 1;
        else if (*p == '>' && tag) tag = 0;
        else if (!tag) cleaned[len++] = *p;
    }
    cleaned[len] = 0;
    appendf(COL_REPLY, "%s: %s", name && *name ? name : "Server", cleaned);
}

// hookChat: "noc" / "unl" / "fh"
void ui_console_on_cheat(const int flag, const int on) {
    const char *label = flag == NET_CHEAT_NOCLIP ? "Noclip" : flag == NET_CHEAT_UNLIMITED_AMMO ? "Unlimited Ammo"
                      : flag == NET_CHEAT_FORCE_HIDE ? "Force Hide" : NULL;
    if (label) appendf(COL_REPLY, "%s - %s", label, on ? "Enabled" : "Disabled");
}

// ---------------------------------------------------------------- registry

static const ConArg A_CMD[] = {{"cmd", ARG_STRING, 1}};
static const ConArg A_TEXT[] = {{"text", ARG_STRING, 0}};
static const ConArg A_BIND[] = {{"key", ARG_STRING, 1}, {"command", ARG_STRING, 1}};
static const ConArg A_KEY[] = {{"key", ARG_STRING, 0}};
static const ConArg A_ALIAS[] = {{"name", ARG_STRING, 1}, {"command", ARG_STRING, 1}};
static const ConArg A_COMMANDS[] = {{"commands", ARG_STRING, 0}};
static const ConArg A_HOSTNAME[] = {{"hostname", ARG_STRING, 1}};
static const ConArg A_VAR[] = {{"var", ARG_STRING, 0}};
static const ConArg A_TOGGLE[] = {{"target", ARG_STRING, 1}, {"on|off", ARG_BOOLEAN, 1}};
static const ConArg A_NAME_ID[] = {{"name|id", ARG_STRING, 1}};
static const ConArg A_ON_OFF[] = {{"on|off", ARG_BOOLEAN, 1}};
static const ConArg A_TARGET[] = {{"target", ARG_STRING, 1}};
static const ConArg A_NAME[] = {{"name", ARG_STRING, 1}};
static const ConArg A_MODE[] = {{"alias", ARG_STRING, 1}};
static const ConArg A_TEAM[] = {{"red|blue", ARG_STRING, 0}};
static const ConArg A_UNBAN[] = {{"accountId|name|id", ARG_STRING, 1}};
static const ConArg A_BANIP[] = {{"name|id|ip", ARG_STRING, 1}};
static const ConArg A_IP[] = {{"ip", ARG_STRING, 1}};
static const ConArg A_AMOUNT[] = {{"amount", ARG_NUMBER, 1}};
static const ConArg A_GAME[] = {{"text", ARG_STRING, 1}};
#define ARGS(a) a, (int) (sizeof(a) / sizeof(a[0]))
#define NO_ARGS NULL, 0

static ConCommand *find_cmd(const char *name) {
    for (int i = 0; i < g_con.cmd_count; i++) {
        if (!strcmp(g_con.cmds[i].name, name)) return &g_con.cmds[i];
    }
    return NULL;
}

// cmds[name] = ...: a new name is appended (Object.keys order), an existing one replaced in place.
static ConCommand *put_cmd(const char *name, const int server, const ConArg *args, const int nargs, const char *desc, ConRun run) {
    ConCommand *cmd = find_cmd(name);
    if (!cmd) {
        if (g_con.cmd_count == CON_CMDS_MAX) return NULL;
        cmd = &g_con.cmds[g_con.cmd_count++];
    }
    memset(cmd, 0, sizeof(*cmd));
    snprintf(cmd->name, sizeof(cmd->name), "%s", name);
    cmd->server = server;
    cmd->args = args;
    cmd->nargs = nargs;
    snprintf(cmd->desc, sizeof(cmd->desc), "%s", desc);
    cmd->run = run;
    cmd->alias = -1;
    return cmd;
}

static void register_local(const char *name, const ConArg *args, const int nargs, const char *desc, ConRun run) {
    put_cmd(name, 0, args, nargs, desc, run);
}

static void register_server(const char *name, const ConArg *args, const int nargs, const char *desc) {
    put_cmd(name, 1, args, nargs, desc, NULL);
}

// fmtArgs: "name: type?  name: type"
static void fmt_args(const ConCommand *cmd, char *out, const size_t n) {
    static const char *const TYPES[] = {"string", "number", "boolean"};
    size_t len = 0;
    out[0] = 0;
    for (int i = 0; i < cmd->nargs && len < n; i++) {
        len += (size_t) snprintf(out + len, n - len, "%s%s: %s%s", i ? "  " : "", cmd->args[i].name, TYPES[cmd->args[i].type],
                                 cmd->args[i].optional ? "?" : "");
    }
}

// ---------------------------------------------------------------- server commands

static int con_connected(void) {
    const Client *client = con_client();
    return client && (client->net_connected || client->offline);
}

static int run_offline_server_command(const char *line);

static int bhop_command(const char *line);

// sendServer(line), then io.send's wrapper (/bhop on the client)
static void send_server(const char *line) {
    if (run_offline_server_command(line)) return;
    if (bhop_command(line)) return;
    Client *client = con_client();
    if (!con_connected()) {
        append("not connected to server", COL_ERROR);
        return;
    }
    client_chat_send(client, line); // io.send("ct", line)
}

// ---------------------------------------------------------------- server details

static const char *map_name(const Game *game, const int index) {
    if (index < 0 || (size_t) index >= game->map_count || !game->maps[index]) return NULL;
    const cJSON *name = cJSON_GetObjectItem(game->maps[index], "name");
    return cJSON_IsString(name) ? name->valuestring : NULL;
}

// getCurrentMapLabel / getCurrentModeLabel
static void map_label(const Game *game, char *out, const size_t n) {
    const char *name = game->ready ? map_name(game, game->current_map_index) : NULL;
    if (name) snprintf(out, n, "%s (%d)", name, game->current_map_index);
    else if (game->ready) snprintf(out, n, "%d", game->current_map_index);
    else snprintf(out, n, "(unknown)");
}

static void mode_label(const Game *game, char *out, const size_t n) {
    const int id = game->ready ? game_mode_id(game) : -1;
    const GameModeConfig *mode = id >= 0 ? mode_config(id) : NULL;
    if (mode && mode->alias) snprintf(out, n, "%s (%d)", mode->alias, id);
    else if (id >= 0) snprintf(out, n, "%d", id);
    else snprintf(out, n, "(unknown)");
}

// printServerDetails
static void print_server_details(void) {
    const Client *client = con_client();
    if (!client) return;
    const Game *game = &client->game;
    int humans = 0, active = 0;
    for (size_t i = 0; i < game->player_count; i++) {
        const Player *p = game->players[i];
        if (!p) continue;
        humans++;
        if (p->active) active++;
    }
#ifdef __EMSCRIPTEN__
    const int has_socket = !client->offline;
#else
    const int has_socket = !client->offline && client->net_thread_running;
#endif
    const char *status = client->offline ? "offline" : client->net_connected ? "connected" : has_socket ? "disconnected" : "no-socket";
    const char *state = client->offline || !has_socket ? "NO_SOCKET" : client->net_connected ? "OPEN" : "CONNECTING";
    char socket_id[24] = "-1"; // io.socketId (-1 until the server assigns one; singleplayer too)
    if (client->net_connected && !client->offline && client->me) snprintf(socket_id, sizeof(socket_id), "%d", client->me->uid);
    appendf(COL_INFO, "Connection: %s | socket %s | state %s", status, socket_id, state);
#ifdef __EMSCRIPTEN__
    appendf(COL_INFO, "Endpoint: %s", emscripten_run_script_string("location.origin")); // socket?.url || location.origin
#else
    appendf(COL_INFO, "Endpoint: tcp://%s:%u", client->net_address, (unsigned int) client->net_port);
#endif
    char map[96], mode[64];
    map_label(game, map, sizeof(map));
    mode_label(game, mode, sizeof(mode));
    appendf(COL_INFO, "Lobby: %s | map %s | mode %s", "(none)", map, mode);
    appendf(COL_INFO, "Players: %d/%d | host %s:%u", active, humans, client->net_address, (unsigned int) client->net_port);
}

// ---------------------------------------------------------------- offline server commands

// parseToggleArg
static int parse_toggle_arg(const char *value, const int current) {
    if (!value || !*value) return !current;
    char v[16];
    snprintf(v, sizeof(v), "%s", value);
    lower(v);
    if (!strcmp(v, "on") || !strcmp(v, "1") || !strcmp(v, "true")) return 1;
    if (!strcmp(v, "off") || !strcmp(v, "0") || !strcmp(v, "false")) return 0;
    return !current;
}

static void normalise(const char *in, char *out, const size_t n) {
    size_t len = 0;
    for (; in && *in && len + 1 < n; in++) {
        if (isalnum((unsigned char) *in)) out[len++] = (char) tolower((unsigned char) *in);
    }
    out[len] = 0;
}

// killOfflinePlayer: players.kill(target, source, {}) with godMode lifted
static void kill_offline_player(Player *source, Player *target) {
    if (!target) return;
    const int had_god = target->god_mode;
    target->god_mode = 0;
    PlayerKillInfo info = {0};
    info.weapon_id = -1;
    player_kill(target, source == target ? NULL : source, &info, 0);
    target->god_mode = (unsigned char) had_god;
}

// offlineBoolCommand
typedef enum { FLAG_GOD, FLAG_NOCLIP, FLAG_SMITE, FLAG_FORCE_HIDE, FLAG_UNLIMITED } OfflineFlag;

static void offline_bool(const OfflineFlag flag, const char *label, const int argc, char **argv) {
    Player *p = con_player();
    if (!p) return;
    int current = 0;
    switch (flag) {
        case FLAG_GOD: current = p->god_mode; break;
        case FLAG_NOCLIP: current = p->noclip; break;
        case FLAG_SMITE: current = p->smite; break;
        case FLAG_FORCE_HIDE: current = p->force_hide; break;
        case FLAG_UNLIMITED: current = p->unlimited_ammo; break;
    }
    const int next = parse_toggle_arg(argc ? argv[0] : NULL, current);
    switch (flag) {
        case FLAG_GOD:
            p->god_mode = (unsigned char) next;
            if (next) p->health = (float) p->max_health;
            break;
        case FLAG_NOCLIP: p->noclip = (unsigned char) next; break;
        case FLAG_SMITE: p->smite = (unsigned char) next; break;
        case FLAG_FORCE_HIDE: p->force_hide = (unsigned char) next; break;
        case FLAG_UNLIMITED:
            p->unlimited_ammo = (unsigned char) next;
            if (next) p->can_throw = 1;
            break;
    }
    appendf(COL_INFO, "%s %s", label, next ? "ON" : "OFF");
}

static int offline_mode_for_change(const Game *game) {
    const int current = game_mode_id(game);
    return g_con.mode_override >= 0 ? g_con.mode_override : current >= 0 ? current : 0;
}

static const char *mode_display_name(const int id) {
    const GameModeConfig *mode = mode_config(id);
    return mode && mode->name ? mode->name : mode && mode->key ? mode->key : NULL;
}

// offlineInitAt(mapIndex, modeIndex)
static int offline_init_at(const int map_index, const int mode_id) {
    Client *client = con_client();
    if (!client || !client->offline || !client->game.map_count) return 0;
    local_game_host(client, map_index, mode_id);
    return 1;
}

static void off_cycle(const int argc, char **argv) {
    (void) argc; (void) argv;
    const Game *game = &con_client()->game;
    if (!game->ready) {
        append("Offline cycle unavailable: game is not ready.", COL_ERROR);
        return;
    }
    if (!game->map_count) {
        append("Offline cycle unavailable: no map list.", COL_ERROR);
        return;
    }
    const int next = (game->current_map_index + 1 + (int) game->map_count) % (int) game->map_count;
    const int mode = offline_mode_for_change(game);
    if (!offline_init_at(next, mode)) {
        append("Cycle failed: game is not ready", COL_ERROR);
        return;
    }
    const char *name = map_name(game, next);
    const char *mode_name_s = mode_display_name(mode);
    char fallback[24], mode_fallback[24];
    snprintf(fallback, sizeof(fallback), "Map %d", next);
    snprintf(mode_fallback, sizeof(mode_fallback), "mode %d", mode);
    appendf(COL_INFO, "Cycled to %s (%d) [%s].", name ? name : fallback, next, mode_name_s ? mode_name_s : mode_fallback);
}

typedef enum { RES_OK, RES_EMPTY, RES_AMBIGUOUS, RES_NOT_FOUND, RES_NONE } ResReason;

// resolveOfflineMapIndex: an index, else one exact name, else one prefix
static int resolve_offline_map(const Game *game, const char *raw, ResReason *reason) {
    if (!game->map_count) { *reason = RES_NONE; return -1; }
    if (!*raw) { *reason = RES_EMPTY; return -1; }
    double d;
    if (js_number(raw, &d) && d == floor(d) && d >= 0 && d < (double) game->map_count) { *reason = RES_OK; return (int) d; }
    char needle[96];
    snprintf(needle, sizeof(needle), "%s", raw);
    lower(needle);
    int exact = -1, exact_n = 0, part = -1, part_n = 0;
    for (size_t i = 0; i < game->map_count; i++) {
        const char *name = map_name(game, (int) i);
        if (!name) continue;
        char low[96];
        snprintf(low, sizeof(low), "%s", name);
        lower(low);
        if (!strcmp(low, needle)) { exact = (int) i; exact_n++; }
        else if (starts_with(low, needle)) { part = (int) i; part_n++; }
    }
    if (exact_n == 1) { *reason = RES_OK; return exact; }
    if (exact_n > 1) { *reason = RES_AMBIGUOUS; return -1; }
    if (part_n == 1) { *reason = RES_OK; return part; }
    *reason = part_n > 1 ? RES_AMBIGUOUS : RES_NOT_FOUND;
    return -1;
}

static void off_map(const int argc, char **argv) {
    const Game *game = &con_client()->game;
    if (!game->map_count) {
        append("No built-in map list available.", COL_ERROR);
        return;
    }
    char raw[CON_TEXT_MAX];
    join_args(argc, argv, 0, raw, sizeof(raw));
    trim(raw);
    if (!raw[0]) {
        char label[96];
        map_label(game, label, sizeof(label));
        appendf(COL_INFO, "Current map: %s", label);
        return;
    }
    ResReason reason;
    const int index = resolve_offline_map(game, raw, &reason);
    if (index < 0) {
        if (reason == RES_AMBIGUOUS) appendf(COL_ERROR, "Map '%s' is ambiguous. Use /maps to list choices.", raw);
        else appendf(COL_ERROR, "Map '%s' not found. Use /maps to list choices.", raw);
        return;
    }
    if (!offline_init_at(index, offline_mode_for_change(game))) {
        append("Map switch failed: game is not ready", COL_ERROR);
        return;
    }
    const char *name = map_name(game, index);
    if (name) appendf(COL_INFO, "Switched map to %s (%d).", name, index);
    else appendf(COL_INFO, "Switched map to Map %d (%d).", index, index);
}

// resolveOfflineModeIndex over Game.map.modes (index = Krunker mode id)
static int resolve_offline_mode(const char *raw, ResReason *reason) {
    int count = 0;
    const GameModeConfig *modes = mode_list(&count);
    if (!count) { *reason = RES_NONE; return -1; }
    if (!*raw) { *reason = RES_EMPTY; return -1; }
    double d;
    if (js_number(raw, &d) && d == floor(d) && d >= 0 && d < count) { *reason = RES_OK; return (int) d; }
    char needle[64];
    snprintf(needle, sizeof(needle), "%s", raw);
    lower(needle);
    int exact = -1, exact_n = 0, part = -1, part_n = 0;
    for (int i = 0; i < count; i++) {
        char alias[64], key[64], name[64];
        snprintf(alias, sizeof(alias), "%s", modes[i].alias ? modes[i].alias : modes[i].key ? modes[i].key : "");
        snprintf(key, sizeof(key), "%s", modes[i].key ? modes[i].key : "");
        snprintf(name, sizeof(name), "%s", modes[i].name ? modes[i].name : "");
        lower(alias);
        lower(key);
        lower(name);
        if (!strcmp(alias, needle) || !strcmp(key, needle) || !strcmp(name, needle)) { exact = i; exact_n++; }
        else if (starts_with(alias, needle) || starts_with(key, needle) || starts_with(name, needle)) { part = i; part_n++; }
    }
    if (exact_n == 1) { *reason = RES_OK; return exact; }
    if (exact_n > 1) { *reason = RES_AMBIGUOUS; return -1; }
    if (part_n == 1) { *reason = RES_OK; return part; }
    *reason = part_n > 1 ? RES_AMBIGUOUS : RES_NOT_FOUND;
    return -1;
}

static void off_mode(const int argc, char **argv) {
    const Game *game = &con_client()->game;
    if (!game->ready) {
        append("Offline mode unavailable: game is not ready.", COL_ERROR);
        return;
    }
    char raw[CON_TEXT_MAX];
    join_args(argc, argv, 0, raw, sizeof(raw));
    trim(raw);
    if (!raw[0]) {
        char label[64];
        mode_label(game, label, sizeof(label));
        appendf(COL_INFO, "Current mode: %s", label);
        if (g_con.mode_override >= 0) appendf(COL_HINT, "Cycle mode override: %d", g_con.mode_override);
        return;
    }
    char low[CON_TEXT_MAX];
    snprintf(low, sizeof(low), "%s", raw);
    lower(low);
    if (!strcmp(low, "default") || !strcmp(low, "reset")) {
        g_con.mode_override = -1;
        append("Cycle mode override cleared.", COL_INFO);
        return;
    }
    ResReason reason;
    const int mode = resolve_offline_mode(raw, &reason);
    if (mode < 0) {
        if (reason == RES_AMBIGUOUS) appendf(COL_ERROR, "Mode '%s' is ambiguous. Use /modes to list choices.", raw);
        else if (reason == RES_NONE) append("No mode list available for this map.", COL_ERROR);
        else appendf(COL_ERROR, "Mode '%s' not found. Use /modes to list choices.", raw);
        return;
    }
    g_con.mode_override = mode;
    if (!offline_init_at(game->current_map_index, mode)) {
        append("Mode switch failed: game is not ready", COL_ERROR);
        return;
    }
    const char *name = mode_display_name(mode);
    if (name) appendf(COL_INFO, "Mode set to %s (%d).", name, mode);
    else appendf(COL_INFO, "Mode set to mode %d (%d).", mode, mode);
}

static void off_modes(const int argc, char **argv) {
    (void) argc; (void) argv;
    const Game *game = &con_client()->game;
    if (!game->ready) {
        append("No active game.", COL_ERROR);
        return;
    }
    int count = 0;
    const GameModeConfig *modes = mode_list(&count);
    if (!count) {
        append("No modes available.", COL_INFO);
        return;
    }
    appendf(COL_INFO, "Modes (%d):", count);
    const int current = game_mode_id(game);
    for (int i = 0; i < count; i++) {
        char id[48] = "";
        if (modes[i].key) snprintf(id, sizeof(id), " id:%s", modes[i].key);
        appendf(COL_HINT, "  %d: %s%s%s", i, modes[i].name ? modes[i].name : modes[i].alias, id, i == current ? " *" : "");
    }
    if (g_con.mode_override >= 0) appendf(COL_INFO, "Cycle mode override: %d", g_con.mode_override);
}

static void off_maps(const int argc, char **argv) {
    (void) argc; (void) argv;
    const Game *game = &con_client()->game;
    if (!game->map_count) {
        append("No built-in map list available.", COL_ERROR);
        return;
    }
    appendf(COL_INFO, "Built-in maps (%d):", (int) game->map_count);
    char list[CON_LINE_MAX * 2] = "";
    size_t len = 0;
    for (size_t i = 0; i < game->map_count && len < sizeof(list); i++) {
        const char *name = map_name(game, (int) i);
        len += (size_t) snprintf(list + len, sizeof(list) - len, "%s%s", i ? ", " : "", name ? name : "");
    }
    append(list, COL_LOG);
}

static void off_borders(const int argc, char **argv) {
    Game *game = &con_client()->game;
    if (!game->ready) {
        append("Offline borders unavailable: game is not ready.", COL_ERROR);
        return;
    }
    const int current = !game->config.disable_borders;
    const int next = parse_toggle_arg(argc ? argv[0] : NULL, current);
    if (next == current) {
        appendf(COL_INFO, "Borders already %s", current ? "ON" : "OFF");
        return;
    }
    game->config.disable_borders = (unsigned int) !next; // applyBordersFromServer
    appendf(COL_INFO, "Borders %s", next ? "ON" : "OFF");
}

static void off_god(const int argc, char **argv) { offline_bool(FLAG_GOD, "God mode", argc, argv); }
static void off_noclip(const int argc, char **argv) { offline_bool(FLAG_NOCLIP, "Noclip", argc, argv); }
static void off_smite(const int argc, char **argv) { offline_bool(FLAG_SMITE, "Smite", argc, argv); }
static void off_forcehide(const int argc, char **argv) { offline_bool(FLAG_FORCE_HIDE, "Forcehide", argc, argv); }
static void off_unlimitedammo(const int argc, char **argv) { offline_bool(FLAG_UNLIMITED, "Unlimited ammo", argc, argv); }

// resolveOfflineKillTarget (players; KrunkNative has no AI)
static Player *resolve_kill_target(const char *query, ResReason *reason) {
    char needle[96];
    normalise(query, needle, sizeof(needle));
    if (!needle[0]) { *reason = RES_EMPTY; return NULL; }
    const Game *game = &con_client()->game;
    Player *exact = NULL, *part = NULL;
    int exact_n = 0, part_n = 0;
    for (size_t i = 0; i < game->player_count; i++) {
        Player *p = game->players[i];
        if (!p || !p->active) continue;
        char name[96], id[16], nid[16];
        normalise(p->name, name, sizeof(name));
        snprintf(id, sizeof(id), "%d", p->uid);
        normalise(id, nid, sizeof(nid));
        if ((name[0] && !strcmp(name, needle)) || !strcmp(nid, needle)) { exact = p; exact_n++; }
        else if ((name[0] && starts_with(name, needle)) || starts_with(nid, needle)) { part = p; part_n++; }
    }
    if (exact_n == 1) { *reason = RES_OK; return exact; }
    if (exact_n > 1) { *reason = RES_AMBIGUOUS; return NULL; }
    if (part_n == 1) { *reason = RES_OK; return part; }
    *reason = part_n > 1 ? RES_AMBIGUOUS : RES_NOT_FOUND;
    return NULL;
}

static void off_kill(const int argc, char **argv) {
    Player *p = con_player();
    if (!p) return;
    char query[CON_TEXT_MAX];
    join_args(argc, argv, 0, query, sizeof(query));
    trim(query);
    if (!query[0]) {
        kill_offline_player(p, p);
        append("You killed yourself.", COL_INFO);
        return;
    }
    ResReason reason;
    Player *target = resolve_kill_target(query, &reason);
    if (!target) {
        if (reason == RES_AMBIGUOUS) appendf(COL_ERROR, "Kill target '%s' is ambiguous.", query);
        else appendf(COL_ERROR, "Kill target '%s' not found.", query);
        return;
    }
    char label[PLAYER_NAME_SIZE + 16];
    if (target->name[0]) snprintf(label, sizeof(label), "%s", target->name);
    else snprintf(label, sizeof(label), "%d", target->uid);
    kill_offline_player(p, target);
    appendf(COL_INFO, "Killed player %s.", label);
}

static void off_score(const int argc, char **argv) {
    Player *p = con_player();
    if (!p) return;
    double amount = 100.0;
    if (argc && !js_number(argv[0], &amount)) amount = 0.0;
    if (amount == 0.0) {
        append("Usage: score <amount> (default 100).", COL_ERROR);
        return;
    }
    player_score(p, (int) amount, 0);
    char num[32];
    if (amount == floor(amount)) snprintf(num, sizeof(num), "%.0f", amount);
    else snprintf(num, sizeof(num), "%g", amount);
    appendf(COL_INFO, "%s%s score (total %d).", amount > 0 ? "+" : "", num, p->score);
}

static void off_team(const int argc, char **argv) {
    Player *p = con_player();
    const Game *game = &con_client()->game;
    if (!p || !game->ready) return;
    const GameModeConfig *mode = game->mode ? &game->mode->config : NULL;
    if (!mode || !mode->teams || mode->friendly || mode->clan_war || mode->convert_team) {
        append("/team only works in team modes.", COL_ERROR);
        return;
    }
    char arg[32] = "";
    if (argc) snprintf(arg, sizeof(arg), "%s", argv[0]);
    lower(arg);
    const int next = !strcmp(arg, "red") || !strcmp(arg, "1") ? 1 : !strcmp(arg, "blue") || !strcmp(arg, "2") ? 2 : 0;
    if (!next) {
        append("Usage: /team red|blue", COL_ERROR);
        return;
    }
    if (p->team == next) {
        appendf(COL_INFO, "Already on %s.", arg);
        return;
    }
    p->team = next;
    kill_offline_player(p, p);
    appendf(COL_INFO, "Switched to %s team.", arg);
}

static void off_teams(const int argc, char **argv) {
    (void) argc; (void) argv;
    const Game *game = &con_client()->game;
    int any = 0;
    for (size_t i = 0; i < game->player_count; i++) if (game->players[i]) any = 1;
    if (!any) {
        append("No players.", COL_INFO);
        return;
    }
    if (!game->mode || !game->mode->config.teams || game->mode->config.convert_team) {
        append("Not a team mode.", COL_INFO);
        return;
    }
    for (int team = 0; team < MODE_TEAMS; team++) {
        char names[CON_LINE_MAX] = "";
        size_t len = 0;
        int count = 0;
        for (size_t i = 0; i < game->player_count; i++) {
            const Player *p = game->players[i];
            if (!p || p->team != team) continue;
            char label[PLAYER_NAME_SIZE + 16];
            if (p->name[0]) snprintf(label, sizeof(label), "%s", p->name);
            else snprintf(label, sizeof(label), "Guest_%d", p->uid);
            if (len < sizeof(names)) len += (size_t) snprintf(names + len, sizeof(names) - len, "%s%s", count ? ", " : "", label);
            count++;
        }
        if (!count) continue;
        if (team == 1) appendf(COL_INFO, "Red (%d): %s", count, names);
        else if (team == 2) appendf(COL_INFO, "Blue (%d): %s", count, names);
        else appendf(COL_INFO, "Team %d (%d): %s", team, count, names);
    }
}

static void off_end(const int argc, char **argv) {
    (void) argc; (void) argv;
    Game *game = &con_client()->game;
    if (!game->ready) return;
    game_set_round_time(game, 3.0f); // endTimer = 0, gameTimer = 3e3
    append("Match timer set to 3 seconds.", COL_INFO);
}

static void off_endless(const int argc, char **argv) {
    (void) argc; (void) argv;
    Game *game = &con_client()->game;
    if (!game->ready) return;
    if (game->round_endless) {
        game_set_endless(game, 0); // the mode's full game time again
        append("Match timer resumed.", COL_INFO);
        return;
    }
    game->round_endless = 1;       // endTimer = "inf": the clock stops where it is
    game->round_seconds_sent = -1;
    append("Match timer paused.", COL_INFO);
}

static void off_server(const int argc, char **argv) {
    (void) argc; (void) argv;
    print_server_details();
}

// offlineServerCommands
static const struct {
    const char *name;
    void (*run)(int, char **);
} OFFLINE_COMMANDS[] = {
    {"server", off_server}, {"cycle", off_cycle}, {"map", off_map}, {"mode", off_mode}, {"modes", off_modes},
    {"maps", off_maps}, {"borders", off_borders}, {"god", off_god}, {"noclip", off_noclip}, {"smite", off_smite},
    {"forcehide", off_forcehide}, {"unlimitedammo", off_unlimitedammo}, {"kill", off_kill}, {"score", off_score},
    {"team", off_team}, {"teams", off_teams}, {"end", off_end}, {"endless", off_endless},
};

static int parse_args(const char *rest, char *work, size_t n, char **argv);

// maybeRunOfflineServerCommand(line): singleplayer only; the leading "/" is optional.
static int run_offline_server_command(const char *line) {
    if (!con_offline()) return 0;
    const char *stripped = line[0] == '/' ? line + 1 : line;
    const char *space = strchr(stripped, ' ');
    char name[CON_NAME_MAX];
    const size_t len = space ? (size_t) (space - stripped) : strlen(stripped);
    snprintf(name, sizeof(name), "%.*s", (int) (len < sizeof(name) - 1 ? len : sizeof(name) - 1), stripped);
    lower(name);
    for (size_t i = 0; i < sizeof(OFFLINE_COMMANDS) / sizeof(OFFLINE_COMMANDS[0]); i++) {
        if (strcmp(OFFLINE_COMMANDS[i].name, name)) continue;
        char work[CON_LINE_MAX];
        char *argv[CON_ARGS_MAX];
        const int argc = parse_args(space ? space + 1 : "", work, sizeof(work), argv);
        OFFLINE_COMMANDS[i].run(argc, argv);
        return 1;
    }
    return 0;
}

// ---------------------------------------------------------------- local commands

static void cmd_help(const int argc, char **argv, const char *rest) {
    (void) rest;
    if (argc) {
        const ConCommand *c = find_cmd(argv[0]);
        if (!c) {
            appendf(COL_ERROR, "unknown command: %s", argv[0]);
            return;
        }
        char args[256];
        fmt_args(c, args, sizeof(args));
        appendf(COL_INFO, "%s %s \xE2\x80\x94 %s", argv[0], args, c->desc);
        return;
    }
    int order[CON_CMDS_MAX];
    const int n = g_con.cmd_count;
    for (int i = 0; i < n; i++) order[i] = i;
    for (int i = 1; i < n; i++) { // Object.keys(cmds).sort()
        const int v = order[i];
        int j = i - 1;
        while (j >= 0 && strcmp(g_con.cmds[order[j]].name, g_con.cmds[v].name) > 0) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = v;
    }
    appendf(COL_INFO, "%d commands:", n);
    for (int i = 0; i < n; i++) appendf(COL_HINT, "  %-18s %s", g_con.cmds[order[i]].name, g_con.cmds[order[i]].desc);
}

static void cmd_clear(const int argc, char **argv, const char *rest) {
    (void) argc; (void) argv; (void) rest;
    if (g_con.built) html_dom_set_html(g_con.logs, "");
    g_con.log_lines = 0;
}

static void cmd_echo(const int argc, char **argv, const char *rest) {
    (void) rest;
    char text[CON_LINE_MAX];
    join_args(argc, argv, 0, text, sizeof(text));
    append(text, COL_LOG);
}

static ConPair *pair_find(ConPair *pairs, const int count, const char *key) {
    for (int i = 0; i < count; i++) if (!strcmp(pairs[i].key, key)) return &pairs[i];
    return NULL;
}

static void cmd_bind(const int argc, char **argv, const char *rest) {
    (void) rest;
    if (!argc) {
        if (!g_con.bind_count) {
            append("no binds", COL_HINT);
            return;
        }
        for (int i = 0; i < g_con.bind_count; i++) appendf(COL_HINT, "  %s = %s", g_con.binds[i].key, g_con.binds[i].value);
        return;
    }
    ConPair *bind = pair_find(g_con.binds, g_con.bind_count, argv[0]);
    if (argc == 1) {
        appendf(COL_INFO, "%s = %s", argv[0], bind ? bind->value : "(unbound)");
        return;
    }
    if (!bind) {
        if (g_con.bind_count == CON_BINDS_MAX) return;
        bind = &g_con.binds[g_con.bind_count++];
        snprintf(bind->key, sizeof(bind->key), "%s", argv[0]);
    }
    join_args(argc, argv, 1, bind->value, sizeof(bind->value));
    appendf(COL_INFO, "bound %s -> %s", argv[0], bind->value);
}

static void cmd_unbind(const int argc, char **argv, const char *rest) {
    (void) rest;
    if (!argc) return;
    for (int i = 0; i < g_con.bind_count; i++) {
        if (strcmp(g_con.binds[i].key, argv[0])) continue;
        memmove(&g_con.binds[i], &g_con.binds[i + 1], sizeof(ConPair) * (size_t) (g_con.bind_count - i - 1));
        g_con.bind_count--;
        break;
    }
    appendf(COL_INFO, "unbound %s", argv[0]);
}

static const ConCommand *g_running; // the command being run (alias commands find their body)

static void cmd_alias_run(const int argc, char **argv, const char *rest) {
    (void) argc; (void) argv; (void) rest;
    if (!g_running || g_running->alias < 0 || g_running->alias >= g_con.alias_count) return;
    char body[CON_TEXT_MAX];
    snprintf(body, sizeof(body), "%s", g_con.aliases[g_running->alias].value);
    con_exec(body);
}

static void cmd_alias(const int argc, char **argv, const char *rest) {
    (void) rest;
    if (!argc) {
        if (!g_con.alias_count) {
            append("no aliases", COL_HINT);
            return;
        }
        for (int i = 0; i < g_con.alias_count; i++) appendf(COL_HINT, "  %s = %s", g_con.aliases[i].key, g_con.aliases[i].value);
        return;
    }
    ConPair *alias = pair_find(g_con.aliases, g_con.alias_count, argv[0]);
    if (argc == 1) {
        appendf(COL_INFO, "%s = %s", argv[0], alias ? alias->value : "(undefined)");
        return;
    }
    if (!alias) {
        if (g_con.alias_count == CON_BINDS_MAX) return;
        alias = &g_con.aliases[g_con.alias_count++];
        snprintf(alias->key, sizeof(alias->key), "%s", argv[0]);
    }
    join_args(argc, argv, 1, alias->value, sizeof(alias->value));
    char desc[CON_TEXT_MAX];
    snprintf(desc, sizeof(desc), "alias for: %s", alias->value);
    ConCommand *cmd = put_cmd(argv[0], 0, NULL, 0, desc, cmd_alias_run);
    if (cmd) cmd->alias = (int) (alias - g_con.aliases);
    appendf(COL_INFO, "alias %s -> %s", argv[0], alias->value);
}

static void cmd_exec(const int argc, char **argv, const char *rest) {
    (void) rest;
    char text[CON_LINE_MAX];
    join_args(argc, argv, 0, text, sizeof(text));
    char *part = text;
    while (part) {
        char *next = strchr(part, ';');
        if (next) *next++ = 0;
        char t[CON_LINE_MAX];
        snprintf(t, sizeof(t), "%s", part);
        trim(t);
        if (t[0]) con_exec(t);
        part = next;
    }
}

#ifndef __EMSCRIPTEN__
static NetMainArgs g_net_args;

static void stop_net(Client *client) {
    if (!client->net_thread_running) return;
    client->net_should_quit = 1;
    client->net_retry_now = 1;
    pthread_mutex_lock(&client->net_lock);
    const int socket = client->net_socket;
    pthread_mutex_unlock(&client->net_lock);
    if (socket >= 0) net_socket_shutdown(socket);
    pthread_join(client->net_thread, NULL);
    client->net_thread_running = 0;
    client->net_should_quit = 0;
    client->net_connected = 0;
}

static int start_net(Client *client) {
    client->net_should_quit = 0;
    client->net_hold_retry = 0;
    g_net_args = (NetMainArgs) {.client = client, .address = NULL};
    if (pthread_create(&client->net_thread, NULL, net_main, &g_net_args)) return 0;
    client->net_thread_running = 1;
    return 1;
}
#endif

// connect [local|hostname]: devConsole points the matchmaker at the host and seeks a game;
// KrunkNative servers are reached directly (host[:port], TCP).
static void cmd_connect(const int argc, char **argv, const char *rest) {
    char input[CON_TEXT_MAX];
    if (rest && *rest) snprintf(input, sizeof(input), "%s", rest);
    else join_args(argc, argv, 0, input, sizeof(input));
    trim(input);
    char target[CON_TEXT_MAX];
    snprintf(target, sizeof(target), "%s", input[0] ? input : "local");
    char low[CON_TEXT_MAX];
    snprintf(low, sizeof(low), "%s", target);
    lower(low);
    const int local = !strcmp(low, "local") || !strcmp(low, "default");

    char host[128];
    unsigned int port = NET_DEFAULT_PORT;
    if (local) {
        snprintf(host, sizeof(host), "127.0.0.1");
    } else {
        const char *start = strstr(target, "://") ? strstr(target, "://") + 3 : target;
        snprintf(host, sizeof(host), "%s", start);
        char *slash = strchr(host, '/');
        if (slash) *slash = 0;
        char *colon = strrchr(host, ':');
        if (colon) {
            *colon = 0;
            port = (unsigned int) strtoul(colon + 1, NULL, 10);
        }
        if (!host[0] || !port || port > 65535) {
            append("connect <local|hostname>", COL_ERROR);
            return;
        }
        if (!strcmp(host, "localhost") || !strcmp(host, "0.0.0.0")) snprintf(host, sizeof(host), "127.0.0.1");
    }
    if (local) append("Connecting through local...", COL_INFO);
    else appendf(COL_INFO, "Connecting through %s:%u...", host, port);
    appendf(COL_INFO, "  Server: tcp://%s:%u", host, port);
#ifdef __EMSCRIPTEN__
    append("endpoint updated; live seek is unavailable on this page", COL_WARN);
#else
    unsigned int b[4];
    char tail;
    if (sscanf(host, "%u.%u.%u.%u%c", &b[0], &b[1], &b[2], &b[3], &tail) != 4 || b[0] > 255 || b[1] > 255 || b[2] > 255 || b[3] > 255) {
        appendf(COL_ERROR, "connect failed: cannot resolve %s (IPv4 addresses only)", host);
        return;
    }
    Client *client = con_client();
    if (!client) return;
    if (client->offline) local_game_stop(client);
    stop_net(client);
    snprintf(client->net_address, sizeof(client->net_address), "%s", host);
    client->net_port = (unsigned short) port;
    if (!start_net(client)) {
        append("connect failed: could not start the network thread", COL_ERROR);
        return;
    }
    menu_retry_connect(); // CONNECTING... until the server answers
#endif
}

// disconnect: endpoints back to the defaults, socket closed, back to the menu ("/").
static void cmd_disconnect(const int argc, char **argv, const char *rest) {
    (void) argc; (void) argv; (void) rest;
#ifdef __EMSCRIPTEN__
    emscripten_run_script("location.href = '/';");
#else
    Client *client = con_client();
    if (!client) return;
    if (client->offline) local_game_stop(client);
    stop_net(client);
    snprintf(client->net_address, sizeof(client->net_address), "127.0.0.1");
    client->net_port = NET_DEFAULT_PORT;
    start_net(client);
    client_set_menu_open(client, 1);
    menu_retry_connect();
#endif
}

static void cmd_quit(const int argc, char **argv, const char *rest) {
    (void) argc; (void) argv; (void) rest;
#ifdef __EMSCRIPTEN__
    emscripten_run_script("window.close();");
#else
    const Client *client = con_client();
    if (client && client->window) glfwSetWindowShouldClose(client->window, GLFW_TRUE); // window.close()
#endif
}

// reconnect: location.reload() (natively: the session starts over)
static void cmd_reconnect(const int argc, char **argv, const char *rest) {
    (void) argc; (void) argv; (void) rest;
#ifdef __EMSCRIPTEN__
    emscripten_run_script("location.reload();");
#else
    Client *client = con_client();
    if (!client) return;
    if (client->offline) {
        local_game_host(client, client->game.ready ? client->game.current_map_index : -1, client->game.ready ? game_mode_id(&client->game) : 0);
        return;
    }
    if (!client->net_thread_running) start_net(client);
    client_net_request_reconnect(client);
    menu_retry_connect();
#endif
}

// clearPingCounter
static void clear_ping_counter(void) {
    Client *client = con_client();
    if (client) client->net_ping_ms = 0;
    const HtmlElem text = html_dom_by_id("menuPingText");
    if (text) html_dom_set_text(text, "0");
    const HtmlElem icon = html_dom_by_id("menuPingIcon");
    if (icon) html_dom_set_style(icon, "color", "gray");
}

// offline: playCustomMap() (the single player game) + clearPingCounter()
static void cmd_offline(const int argc, char **argv, const char *rest) {
    (void) argc; (void) argv; (void) rest;
    Client *client = con_client();
    if (!client) return;
#ifndef __EMSCRIPTEN__
    if (!client->offline) stop_net(client);
#else
    if (!client->offline && client->net_connected) client_net_disconnect(client, "Offline", "000000");
    client->net_connected = 0;
#endif
    menu_host_offline();
    clear_ping_counter();
    if (!client->offline) append("offline failed: the offline game did not start", COL_ERROR);
}

static void cmd_ahk(const int argc, char **argv, const char *rest) {
    (void) argc; (void) argv; (void) rest;
    appendf(COL_INFO, "IO AHK: %d (base: %d)", 0, CON_AHK_BASE); // the native protocol never rotates ahNum
}

static void cmd_version(const int argc, char **argv, const char *rest) {
    (void) argc; (void) argv; (void) rest;
    appendf(COL_INFO, "Version: %s", CON_VERSION);
}

static void cmd_toggle(const int argc, char **argv, const char *rest) {
    (void) rest;
    if (!argc || !argv[0][0]) {
        append("toggle <var>", COL_ERROR);
        return;
    }
    char line[CON_TEXT_MAX];
    snprintf(line, sizeof(line), "/%s", argv[0]);
    send_server(line);
}

static void cmd_say(const int argc, char **argv, const char *rest) {
    (void) rest;
    char text[CON_LINE_MAX];
    join_args(argc, argv, 0, text, sizeof(text));
    trim(text);
    if (!text[0]) return;
    send_server(text);
}

static void cmd_status(const int argc, char **argv, const char *rest) {
    (void) argc; (void) argv; (void) rest;
    print_server_details();
}

static void cmd_pos(const int argc, char **argv, const char *rest) {
    (void) argc; (void) argv; (void) rest;
    const Player *p = con_player();
    if (!p) return;
    appendf(COL_INFO, "position: %.6f %.6f %.6f", p->position.x, p->position.y, p->position.z);
}

static void cmd_rot(const int argc, char **argv, const char *rest) {
    (void) argc; (void) argv; (void) rest;
    const Player *p = con_player();
    if (!p) {
        append("no local player", COL_ERROR);
        return;
    }
    // Player.direction = (xDire, yDire): pitch and yaw
    appendf(COL_INFO, "rotation: yaw=%.6f pitch=%.6f xDire=%.6f", p->direction.y, p->direction.x, p->direction.x);
}

// the local "ping" (replaced by the server command right after; Object.keys keeps its slot)
static void cmd_ping_local(const int argc, char **argv, const char *rest) {
    (void) argc; (void) argv; (void) rest;
    const Client *client = con_client();
    appendf(COL_INFO, "Ping: %dms", client && client->net_ping_ms > 0 ? client->net_ping_ms : 0);
}

static void register_commands(void) {
    if (g_con.cmd_count) return;
    register_local("help", ARGS(A_CMD), "list commands", cmd_help);
    register_local("clear", NO_ARGS, "clear console output", cmd_clear);
    register_local("echo", ARGS(A_TEXT), "print text to console", cmd_echo);
    register_local("print", ARGS(A_TEXT), "alias for echo", cmd_echo);
    register_local("bind", ARGS(A_BIND), "bind a key to a command (session only)", cmd_bind);
    register_local("unbind", ARGS(A_KEY), "remove a key bind", cmd_unbind);
    register_local("alias", ARGS(A_ALIAS), "define a command alias", cmd_alias);
    register_local("exec", ARGS(A_COMMANDS), "run multiple commands separated by ;", cmd_exec);
    register_local("connect", ARGS(A_HOSTNAME), "seek and connect through a matchmaker hostname", cmd_connect);
    register_local("disconnect", NO_ARGS, "disconnect and reset to default endpoints", cmd_disconnect);
    register_local("quit", NO_ARGS, "close the window", cmd_quit);
    register_local("reconnect", NO_ARGS, "reload the current page", cmd_reconnect);
    register_local("offline", NO_ARGS, "switch to offline mode", cmd_offline);
    register_local("ahk", NO_ARGS, "show current antihack key", cmd_ahk);
    register_local("version", NO_ARGS, "show current client version", cmd_version);
    register_local("build", NO_ARGS, "alias for version", cmd_version);
    register_local("verison", NO_ARGS, "alias for version", cmd_version);
    register_local("toggle", ARGS(A_VAR), "toggle a server boolean command (god, noclip, ...)", cmd_toggle);
    register_local("say", ARGS(A_TEXT), "say something in chat", cmd_say);
    register_local("status", NO_ARGS, "show current server connection details", cmd_status);
    register_local("serverinfo", NO_ARGS, "alias for status", cmd_status);
    register_local("pos", NO_ARGS, "show local player position", cmd_pos);
    register_local("position", NO_ARGS, "alias for pos", cmd_pos);
    register_local("rot", NO_ARGS, "show local player rotation", cmd_rot);
    register_local("rotation", NO_ARGS, "alias for rot", cmd_rot);
    register_local("ping", NO_ARGS, "show current ping", cmd_ping_local);
    // Server commands (mirror server/packetHandlers.ts cmds table)
    register_server("god", ARGS(A_TOGGLE), "toggle god mode (self or target)");
    register_server("noclip", ARGS(A_TOGGLE), "toggle noclip (self or target)");
    register_server("smite", ARGS(A_TOGGLE), "toggle smite-on-hit (self or target)");
    register_server("forcehide", ARGS(A_TOGGLE), "toggle forcehide (self or target)");
    register_server("unlimitedammo", ARGS(A_TOGGLE), "toggle unlimited ammo (self or target)");
    register_server("c", NO_ARGS, "show connected players and menu count");
    register_server("count", NO_ARGS, "alias for /c (show connected players and menu count)");
    register_server("ping", ARGS(A_NAME_ID), "show your ping (devs can target players)");
    register_server("bhop", ARGS(A_ON_OFF), "toggle bhop");
    register_server("kill", ARGS(A_TARGET),
                    "kill yourself or a target (examples: /kill, /kill Guest_1, /kill \"Wandering Ghost\", /kill wanderingghost)");
    register_server("end", NO_ARGS, "set match timer to 3 seconds");
    register_server("endless", NO_ARGS, "toggle endless match timer");
    register_server("borders", ARGS(A_ON_OFF), "toggle invisible barriers");
    register_server("forcecol", ARGS(A_ON_OFF), "toggle force collide");
    register_server("server", NO_ARGS, "show current map/mode/player/tick info");
    register_server("cycle", NO_ARGS, "cycle to the next map");
    register_server("map", ARGS(A_NAME), "switch map (no arg = show current)");
    register_server("maps", NO_ARGS, "list available maps");
    register_server("mode", ARGS(A_MODE), "switch game mode");
    register_server("modes", NO_ARGS, "list game modes");
    register_server("team", ARGS(A_TEAM), "switch teams");
    register_server("teams", NO_ARGS, "list teams and players");
    register_server("players", NO_ARGS, "list connected players (id, name, team)");
    register_server("ais", NO_ARGS, "list active AI entities (sid, name)");
    register_server("kick", ARGS(A_NAME_ID), "kick a player");
    register_server("ban", ARGS(A_NAME_ID), "ban a player account");
    register_server("unban", ARGS(A_UNBAN), "unban an account");
    register_server("bans", NO_ARGS, "list banned account ids");
    register_server("ip", ARGS(A_NAME_ID), "show a player's IP");
    register_server("banip", ARGS(A_BANIP), "ban an IP in this lobby");
    register_server("unbanip", ARGS(A_IP), "unban an IP");
    register_server("ipbans", NO_ARGS, "list banned IPs");
    register_server("score", ARGS(A_AMOUNT), "add score (default +100)");
    register_server("summon", ARGS(A_NAME), "spawn a Soul Sanctum AI by name (no arg lists summonable names)");
    register_server("game", ARGS(A_GAME), "send a game command");
}

// ---------------------------------------------------------------- execution

// parseArgs: /"([^"]*)"|(\S+)/g
static int parse_args(const char *rest, char *work, const size_t n, char **argv) {
    snprintf(work, n, "%s", rest ? rest : "");
    int argc = 0;
    char *s = work;
    while (*s && argc < CON_ARGS_MAX) {
        while (*s && isspace((unsigned char) *s)) s++;
        if (!*s) break;
        if (*s == '"') {
            char *close = strchr(s + 1, '"');
            if (close) {
                argv[argc++] = s + 1;
                *close = 0;
                s = close + 1;
                continue;
            }
        }
        argv[argc++] = s;
        while (*s && !isspace((unsigned char) *s)) s++;
        if (*s) *s++ = 0;
    }
    return argc;
}

// exec(line)
static void con_exec(const char *raw) {
    register_commands();
    char line[CON_LINE_MAX];
    snprintf(line, sizeof(line), "%s", raw ? raw : "");
    trim(line);
    if (!line[0]) return;
    appendf(COL_CMD, "] %s", line);
    const char *stripped = line[0] == '/' ? line + 1 : line;
    const char *space = strchr(stripped, ' ');
    char name[CON_NAME_MAX];
    const size_t len = space ? (size_t) (space - stripped) : strlen(stripped);
    snprintf(name, sizeof(name), "%.*s", (int) (len < sizeof(name) - 1 ? len : sizeof(name) - 1), stripped);
    lower(name);
    const char *rest = space ? space + 1 : "";
    char work[CON_LINE_MAX];
    char *argv[CON_ARGS_MAX];
    const int argc = parse_args(rest, work, sizeof(work), argv);

    const ConCommand *c = find_cmd(name);
    if (!c) return; // unknown commands are silent
    if (!c->server) {
        const ConCommand *previous = g_running;
        g_running = c;
        c->run(argc, argv, rest);
        g_running = previous;
        return;
    }
    char out[CON_LINE_MAX + 2];
    snprintf(out, sizeof(out), "/%s", stripped);
    send_server(out);
}

void ui_console_exec(const char *line) {
    con_exec(line);
}

// ---------------------------------------------------------------- client-side chat (io.send wrapper)

// maybeHandleClientBhopCommand / resolveBhopMode: "/bhop [on|off|held]" in any "ct".
static int bhop_command(const char *line) {
    char low[16];
    snprintf(low, sizeof(low), "%.5s", line);
    lower(low);
    if (strcmp(low, "/bhop")) return 0;
    char arg[64];
    snprintf(arg, sizeof(arg), "%s", line + 5);
    trim(arg);
    lower(arg);
    const int current = ui_bhop_mode();
    int next = -1;
    if (!arg[0]) {
        next = current > 0 ? 0 : 1;
    } else if (!strcmp(arg, "off") || !strcmp(arg, "0") || !strcmp(arg, "disable") || !strcmp(arg, "disabled")) {
        next = 0;
    } else {
        static const char *const ON[] = {"on", "1", "enable", "enabled", "hold", "manual", "held", "manualslide",
                                         "holdslide", "jumpcrouch", "both", "2", "3", "4"};
        for (size_t i = 0; i < sizeof(ON) / sizeof(ON[0]); i++) if (!strcmp(arg, ON[i])) next = 1;
    }
    if (next < 0) {
        append("Usage: /bhop [on|off|held]", COL_ERROR);
        return 1;
    }
    ui_bhop_set_mode(next);
    char msg[64];
    snprintf(msg, sizeof(msg), "Bhop - %s", next ? "Hold Jump/Crouch" : "Disabled"); // devConsole's BHOP_MODE_NAMES
    append(msg, COL_REPLY);
    ui_notify(msg);
    return 1;
}

int ui_console_chat_command(Client *client, const char *text) {
    if (client) g_con.client = client;
    if (!text) return 0;
    char line[CON_LINE_MAX];
    snprintf(line, sizeof(line), "%s", text);
    trim(line);
    if (bhop_command(line)) return 1;
    return run_offline_server_command(line); // `label === "ct" && this.socket === null`
}

// ---------------------------------------------------------------- hints / completion

static void set_input(const char *value) {
    if (!g_con.built) return;
    html_dom_set_value(g_con.input, value);
    snprintf(g_con.last_value, sizeof(g_con.last_value), "%s", value);
}

static void clear_hints(void) {
    if (!g_con.built) return;
    html_dom_set_html(g_con.hints, "");
    html_dom_set_class(g_con.hints, "isOpen", 0);
    g_con.hint_count = 0;
}

static void add_hint_item(const char *label, const char *meta, const int active, const int pickable) {
    char a[CON_TEXT_MAX], b[CON_LINE_MAX], html[CON_LINE_MAX * 2];
    escape_html(a, sizeof(a), label);
    if (meta) {
        char spaced[CON_LINE_MAX];
        snprintf(spaced, sizeof(spaced), "  %s", meta);
        escape_html(b, sizeof(b), spaced);
        snprintf(html, sizeof(html), "<div class=\"hintItem%s\">%s<span class=\"hintMeta\">%s</span></div>", active ? " isActive" : "", a, b);
    } else {
        snprintf(html, sizeof(html), "<div class=\"hintItem%s\">%s</div>", active ? " isActive" : "", a);
    }
    html_dom_insert_html(g_con.hints, HTML_INSERT_BEFORE_END, html);
    if (g_con.hint_count < CON_HINTS_MAX) snprintf(g_con.hint_names[g_con.hint_count++], CON_NAME_MAX, "%s", pickable ? label : "");
}

// updateHints
static void update_hints(void) {
    if (!g_con.built) return;
    char v[CON_LINE_MAX] = "";
    html_dom_get_value(g_con.input, v, sizeof(v));
    snprintf(g_con.last_value, sizeof(g_con.last_value), "%s", v);
    clear_hints();
    if (!v[0]) return;
    const char *space = strchr(v, ' ');
    char head[CON_NAME_MAX];
    const size_t len = space ? (size_t) (space - v) : strlen(v);
    snprintf(head, sizeof(head), "%.*s", (int) (len < sizeof(head) - 1 ? len : sizeof(head) - 1), v);
    lower(head);
    const char *stripped = head[0] == '/' ? head + 1 : head;

    if (!space) {
        int matches[CON_HINTS_MAX];
        int n = 0, active = 0;
        for (int i = 0; i < g_con.cmd_count && n < CON_HINTS_MAX; i++) {
            if (starts_with(g_con.cmds[i].name, stripped)) matches[n++] = i;
        }
        if (!n) return;
        html_dom_set_class(g_con.hints, "isOpen", 1);
        for (int i = 0; i < n; i++) {
            if (!strcmp(g_con.cmds[matches[i]].name, stripped)) {
                active = i;
                break;
            }
        }
        for (int i = 0; i < n; i++) add_hint_item(g_con.cmds[matches[i]].name, NULL, i == active, 1);
        return;
    }
    const ConCommand *c = find_cmd(stripped);
    html_dom_set_class(g_con.hints, "isOpen", 1);
    if (c) {
        char args[256], meta[CON_LINE_MAX];
        fmt_args(c, args, sizeof(args));
        snprintf(meta, sizeof(meta), "%s  \xE2\x80\x94  %s", args, c->desc);
        add_hint_item(stripped, meta, 1, 0);
    } else {
        add_hint_item("(unknown command)", "will not be sent", 1, 0);
    }
}

// tabComplete: only the command name
static void tab_complete(void) {
    if (!g_con.built) return;
    char v[CON_LINE_MAX] = "";
    html_dom_get_value(g_con.input, v, sizeof(v));
    if (strchr(v, ' ')) return;
    const char *head = v[0] == '/' ? v + 1 : v;
    int first = -1, count = 0;
    char prefix[CON_NAME_MAX] = "";
    for (int i = 0; i < g_con.cmd_count; i++) {
        if (!starts_with(g_con.cmds[i].name, head)) continue;
        if (first < 0) {
            first = i;
            snprintf(prefix, sizeof(prefix), "%s", g_con.cmds[i].name);
        }
        while (prefix[0] && !starts_with(g_con.cmds[i].name, prefix)) prefix[strlen(prefix) - 1] = 0;
        count++;
    }
    if (count == 1) {
        char line[CON_NAME_MAX + 2];
        snprintf(line, sizeof(line), "%s ", g_con.cmds[first].name);
        set_input(line);
    } else if (count > 1 && strlen(prefix) > strlen(head)) {
        set_input(prefix);
    }
    update_hints();
}

static void history_up(void) {
    if (!g_con.history_count) return;
    if (g_con.history_idx == -1) {
        html_dom_get_value(g_con.input, g_con.draft, sizeof(g_con.draft));
        g_con.history_idx = g_con.history_count - 1;
    } else if (g_con.history_idx > 0) {
        g_con.history_idx--;
    }
    set_input(g_con.history[g_con.history_idx]);
    update_hints();
}

static void history_down(void) {
    if (g_con.history_idx == -1) return;
    if (g_con.history_idx < g_con.history_count - 1) {
        g_con.history_idx++;
        set_input(g_con.history[g_con.history_idx]);
    } else {
        g_con.history_idx = -1;
        set_input(g_con.draft);
    }
    update_hints();
}

// inputRow "submit"
static void submit(void) {
    if (!g_con.built) return;
    char line[CON_LINE_MAX] = "";
    html_dom_get_value(g_con.input, line, sizeof(line));
    set_input("");
    clear_hints();
    char check[CON_LINE_MAX];
    snprintf(check, sizeof(check), "%s", line);
    trim(check);
    if (check[0]) {
        if (g_con.history_count == CON_HISTORY_MAX) {
            memmove(g_con.history[0], g_con.history[1], sizeof(g_con.history[0]) * (CON_HISTORY_MAX - 1));
            g_con.history_count--;
        }
        snprintf(g_con.history[g_con.history_count++], CON_LINE_MAX, "%s", line);
    }
    g_con.history_idx = -1;
    g_con.draft[0] = 0;
    con_exec(line);
}

// ---------------------------------------------------------------- visibility

int ui_console_is_open(void) {
    return g_con.visible;
}

// update(goToGame)
static void update(const int go_to_game) {
    if (g_con.built) html_dom_set_display(g_con.overlay, g_con.visible ? "block" : "none");
    if (g_con.visible) {
        g_con.focus_pending = 1;                            // setTimeout(() => input.focus(), 0)
        if (!html_ui_is_menu_open()) menu_request_open(1);  // document.exitPointerLock()
        ui_controls_reset_keys();
        const HtmlElem chat = html_dom_by_id("chatInput");
        if (chat) html_dom_blur(chat);
        ui_controls_chat_focus(0);
    } else if (go_to_game) {
        if (g_con.built) html_dom_blur(g_con.input);
        if (html_ui_is_menu_open()) menu_play();            // CONTROLS.locklessChange(true) + requestPointerLock()
    }
}

void ui_console_toggle(const int go_to_game) {
    g_con.visible = !g_con.visible;
    update(go_to_game);
}

// ---------------------------------------------------------------- keys

int ui_console_key_event(Client *client, const int key, const int scancode, const int action, const int mods) {
    if (client) g_con.client = client;
    if (!html_ui_available()) return 0;
    const int press = action == GLFW_PRESS;
    const int typing = !g_con.visible && ui_controls_chat_focused(); // focus in another field

    // handleConsoleKeydown (window capture)
    if (g_con.visible && (press || action == GLFW_REPEAT)) {
        if (key == GLFW_KEY_ESCAPE) {
            if (press) ui_console_toggle(1);
            return 1;
        }
        if (key == GLFW_KEY_UP) {
            history_up();
            return 1;
        }
        if (key == GLFW_KEY_DOWN) {
            history_down();
            return 1;
        }
        if (key == GLFW_KEY_TAB) {
            tab_complete();
            return 1;
        }
    }
    // mods.ts document keydown: toggle key, F8 cycle, bhop key
    if (ui_mods_key_event(client, key, action, mods, typing)) return 1;

    if (g_con.visible) {
        if (press && (key == GLFW_KEY_ENTER || key == GLFW_KEY_KP_ENTER)) { // form submit
            submit();
            return 1;
        }
        html_ui_key(key, scancode, action, mods); // the input line edits itself
        return 1;                                 // overlay keydown/keyup stopPropagation
    }
    // Key bind dispatch: keyBinds[e.key] || keyBinds[e.code], not while typing
    if (press && !typing && g_con.bind_count) {
        const ConPair *bind = pair_find(g_con.binds, g_con.bind_count, ui_mods_key_value(key, mods));
        if (!bind) bind = pair_find(g_con.binds, g_con.bind_count, ui_mods_key_code(key));
        if (bind) {
            char cmd[CON_TEXT_MAX];
            snprintf(cmd, sizeof(cmd), "%s", bind->value);
            con_exec(cmd);
        }
    }
    return 0;
}

int ui_console_page_key(const HtmlEvent *ev) {
    if (!g_con.built || !ev || !ev->target || ev->target != g_con.input || !g_con.visible) return 0;
    if (html_key_is_escape(ev->key)) {
        ui_console_toggle(1);
        return 1;
    }
    if (html_key_is_enter(ev->key)) {
        submit();
        return 1;
    }
#ifdef __EMSCRIPTEN__
    // DOM keyCodes (kn-shim forwards these for data-kn-keys fields)
    if (ev->key == 38) {
        history_up();
        return 1;
    }
    if (ev->key == 40) {
        history_down();
        return 1;
    }
    if (ev->key == 9) {
        tab_complete();
        return 1;
    }
    if (ev->key == 192 && ui_mods_dev_console_enabled() && !strcmp(ui_mods_dev_console_key(), "Backquote")) {
        menu_play();          // toggleDeveloperConsole: instructionHolder.click()
        ui_console_toggle(1);
        return 1;
    }
#endif
    return 0;
}

// ---------------------------------------------------------------- mouse (drag, resize, clicks)

static int hit(const HtmlElem el, const float x, const float y) {
    HtmlRect r;
    return el && html_dom_get_rect(el, &r) && x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

static int style_px(const HtmlElem el, const char *prop) {
    char buf[32];
    if (html_dom_get_style(el, prop, buf, sizeof(buf)) < 0) return 0;
    return atoi(buf); // parseInt(modal.style.left || "0", 10)
}

static void set_px(const HtmlElem el, const char *prop, const float v) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%dpx", (int) lroundf(v));
    html_dom_set_style(el, prop, buf);
}

static int pos_committed(void) {
    char buf[8];
    return html_dom_get_attr(g_con.modal, "data-pos-committed", buf, sizeof(buf)) >= 0 && !strcmp(buf, "1");
}

static void clamp_pos(void) {
    float vw, vh, dpr;
    html_dom_viewport(&vw, &vh, &dpr);
    const float w = html_dom_offset_width(g_con.modal);
    const float h = html_dom_offset_height(g_con.modal);
    const float max_x = vw - w > 0 ? vw - w : 0;
    const float max_y = vh - h > 0 ? vh - h : 0;
    float left = (float) style_px(g_con.modal, "left");
    float top = (float) style_px(g_con.modal, "top");
    left = left < 0 ? 0 : left > max_x ? max_x : left;
    top = top < 0 ? 0 : top > max_y ? max_y : top;
    set_px(g_con.modal, "left", left);
    set_px(g_con.modal, "top", top);
}

// commitInitialPos: from the CSS anchor (right/top) to absolute pixels
static void commit_initial_pos(void) {
    if (pos_committed()) return;
    HtmlRect r = {0};
    html_dom_get_rect(g_con.modal, &r);
    html_dom_set_style(g_con.modal, "transform", "none");
    set_px(g_con.modal, "left", r.x);
    set_px(g_con.modal, "top", r.y);
    html_dom_set_attr(g_con.modal, "data-pos-committed", "1");
}

static void save_geom_now(void *user) {
    (void) user;
    g_con.save_timer = 0;
    if (!g_con.built || !pos_committed()) return;
    char json[160];
    snprintf(json, sizeof(json), "{\"left\":%d,\"top\":%d,\"width\":%d,\"height\":%d}", style_px(g_con.modal, "left"),
             style_px(g_con.modal, "top"), (int) lroundf(html_dom_offset_width(g_con.modal)),
             (int) lroundf(html_dom_offset_height(g_con.modal)));
    html_storage_set(CON_GEOM_KEY, json);
}

static void save_geom(void) {
    if (g_con.save_timer) html_timer_cancel(g_con.save_timer);
    g_con.save_timer = html_timer_add(150, save_geom_now, NULL);
}

static void restore_geom(void) {
    char buf[256];
    if (html_storage_get(CON_GEOM_KEY, buf, sizeof(buf)) < 0) return;
    cJSON *g = cJSON_Parse(buf);
    if (!cJSON_IsObject(g)) {
        cJSON_Delete(g);
        return;
    }
    const cJSON *w = cJSON_GetObjectItem(g, "width"), *h = cJSON_GetObjectItem(g, "height");
    const cJSON *l = cJSON_GetObjectItem(g, "left"), *t = cJSON_GetObjectItem(g, "top");
    if (cJSON_IsNumber(w) && w->valuedouble >= 280) {
        set_px(g_con.modal, "width", (float) w->valuedouble);
        g_con.user_size = 1;
    }
    if (cJSON_IsNumber(h) && h->valuedouble >= 160) {
        set_px(g_con.modal, "height", (float) h->valuedouble);
        g_con.user_size = 1;
    }
    if (cJSON_IsNumber(l) && cJSON_IsNumber(t)) {
        html_dom_set_style(g_con.modal, "transform", "none");
        set_px(g_con.modal, "left", (float) l->valuedouble);
        set_px(g_con.modal, "top", (float) t->valuedouble);
        html_dom_set_attr(g_con.modal, "data-pos-committed", "1");
        clamp_pos();
    }
    cJSON_Delete(g);
}

static int pick_hint(const float x, const float y) {
    HtmlElem items[CON_HINTS_MAX];
    const int n = html_dom_query_all(g_con.hints, ".hintItem", items, CON_HINTS_MAX);
    for (int i = 0; i < n && i < g_con.hint_count; i++) {
        if (!hit(items[i], x, y)) continue;
        if (g_con.hint_names[i][0]) { // addHintItem onPick: input.value = name + " "
            char line[CON_NAME_MAX + 2];
            snprintf(line, sizeof(line), "%s ", g_con.hint_names[i]);
            set_input(line);
            update_hints();
            html_dom_focus(g_con.input);
        }
        return 1;
    }
    return 0;
}

int ui_console_mouse(const int action, double x, double y, const int css) {
    if (!css) { // framebuffer px -> CSS px
        float vw, vh, dpr;
        html_dom_viewport(&vw, &vh, &dpr);
        if (dpr > 0.0f) {
            x /= dpr;
            y /= dpr;
        }
    }
    const float fx = (float) x, fy = (float) y;
    if (action == -1) {
        if (g_con.dragging) {
            set_px(g_con.modal, "left", fx - g_con.dx);
            set_px(g_con.modal, "top", fy - g_con.dy);
            clamp_pos();
            return 1;
        }
        if (g_con.resizing) {
            const float w = g_con.start_w + (fx - g_con.start_x);
            const float h = g_con.start_h + (fy - g_con.start_y);
            set_px(g_con.modal, "width", w > 280 ? w : 280);
            set_px(g_con.modal, "height", h > 160 ? h : 160);
            return 1;
        }
        return 0;
    }
    if (action == GLFW_RELEASE) {
        if (g_con.dragging || g_con.resizing) {
            g_con.dragging = g_con.resizing = 0;
            save_geom();
            return 1;
        }
        if (ui_mods_pointer(0, fx, fy)) return 1;
        const HtmlElem pressed = g_con.pressed;
        g_con.pressed = 0;
        if (!pressed) return 0;
        if (hit(pressed, fx, fy)) {
            if (pressed == g_con.close_btn) ui_console_toggle(0); // closeBtn click: toggleConsole(false)
            else if (pressed == g_con.submit) submit();
        }
        return 1;
    }
    // GLFW_PRESS
    if (ui_mods_pointer(1, fx, fy)) return 1;
    if (!g_con.built || !g_con.visible) return 0;
    if (hit(g_con.close_btn, fx, fy)) {
        g_con.pressed = g_con.close_btn;
        return 1;
    }
    if (hit(g_con.submit, fx, fy)) {
        g_con.pressed = g_con.submit;
        return 1;
    }
    if (html_dom_has_class(g_con.hints, "isOpen") && pick_hint(fx, fy)) return 1;
    if (hit(g_con.header, fx, fy)) {
        commit_initial_pos();
        HtmlRect r = {0};
        html_dom_get_rect(g_con.modal, &r);
        g_con.dragging = 1;
        g_con.dx = fx - r.x;
        g_con.dy = fy - r.y;
        return 1;
    }
    if (hit(g_con.grip, fx, fy)) {
        commit_initial_pos();
        g_con.resizing = 1;
        g_con.user_size = 1;
        g_con.start_x = fx;
        g_con.start_y = fy;
        g_con.start_w = html_dom_offset_width(g_con.modal);
        g_con.start_h = html_dom_offset_height(g_con.modal);
        return 1;
    }
    return 0;
}

// web glue (kn-shim data-kn-pointer): window listeners knpointerdown / move / up in CSS px
static int l_pointer(void *user, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) user; (void) args; (void) argc;
    if (!ev || !ev->type) return 1;
    const int action = !strcmp(ev->type, "knpointerdown") ? GLFW_PRESS : !strcmp(ev->type, "knpointerup") ? GLFW_RELEASE : -1;
    return ui_console_mouse(action, ev->x, ev->y, 1) ? 0 : 1;
}

// ---------------------------------------------------------------- per frame

static void build(void) {
    const HtmlElem body = html_dom_body();
    if (!body) return;
    html_ui_add_stylesheet("css/devconsole.css");
    html_dom_insert_html(body, HTML_INSERT_BEFORE_END, OVERLAY_HTML);
    g_con.overlay = html_dom_by_id("devConsoleOverlay");
    if (!g_con.overlay) return;
    g_con.modal = html_dom_query(g_con.overlay, ".modal");
    g_con.header = html_dom_query(g_con.overlay, ".titlebar");
    g_con.close_btn = html_dom_query(g_con.overlay, ".titlebar .x");
    g_con.logs = html_dom_query(g_con.overlay, ".logs");
    g_con.hints = html_dom_query(g_con.overlay, ".hints");
    g_con.input = html_dom_query(g_con.overlay, ".row input");
    g_con.submit = html_dom_query(g_con.overlay, ".row button");
    g_con.grip = html_dom_query(g_con.overlay, ".grip");
    if (!g_con.modal || !g_con.logs || !g_con.input) return;
    g_con.built = 1;
    g_con.log_lines = 0;
    g_con.css_vw = g_con.css_vh = 0.0f;
    flush_pending();
    restore_geom();
    html_ui_set_listener(NULL, "knpointerdown", l_pointer);
    html_ui_set_listener(NULL, "knpointermove", l_pointer);
    html_ui_set_listener(NULL, "knpointerup", l_pointer);
    if (g_con.visible) html_dom_set_display(g_con.overlay, "block");
}

// .modal { width: min(744px, calc(100vw - 16px)); height: min(280px, calc(100vh - 16px)) }:
// the native engine has no min(), so the used size is written inline (the web gets the
// same numbers) until the grip or a saved geometry sets one.
static void apply_css_min_size(void) {
    if (g_con.user_size) return;
    float vw, vh, dpr;
    html_dom_viewport(&vw, &vh, &dpr);
    if (vw <= 0.0f || vh <= 0.0f || (vw == g_con.css_vw && vh == g_con.css_vh)) return;
    g_con.css_vw = vw;
    g_con.css_vh = vh;
    set_px(g_con.modal, "width", vw - 16.0f < 744.0f ? vw - 16.0f : 744.0f);
    set_px(g_con.modal, "height", vh - 16.0f < 280.0f ? vh - 16.0f : 280.0f);
}

// The input's `input` listener: a typed toggle character ("`", "~" or the bind's) is
// removed and closes the console; anything else refreshes the hints.
static void poll_input(void) {
    char value[CON_LINE_MAX] = "";
    html_dom_get_value(g_con.input, value, sizeof(value));
    if (!strcmp(value, g_con.last_value)) return;
    char toggles[4] = "`~";
    const char *bind = ui_mods_dev_console_key();
    if (!strcmp(bind, "Minus")) toggles[2] = '-';
    else if (!strcmp(bind, "Equal")) toggles[2] = '=';
    else if (!strncmp(bind, "Key", 3) && bind[3] && !bind[4]) toggles[2] = (char) tolower((unsigned char) bind[3]);
    char *found = NULL;
    for (char *p = value; *p && !found; p++) if (strchr(toggles, *p)) found = p;
    if (found && strlen(value) > strlen(g_con.last_value)) {
        memmove(found, found + 1, strlen(found));
        set_input(value);
        ui_console_toggle(1);
        return;
    }
    update_hints();
}

void ui_console_tick(Client *client) {
    if (client) g_con.client = client;
    register_commands();
    if (!g_con.banner) { // Bootstrap banner
        g_con.banner = 1;
        append("Krunker developer console - type `help` for commands.", COL_INFO);
        append("Press ~ to toggle, Esc to close, Tab to autocomplete.", COL_HINT);
    }
    if (!html_ui_available() || !html_ui_is_ready() || strcmp(html_ui_current_page(), "index.html")) {
        g_con.built = 0;
        return;
    }
    if (!g_con.built || !html_dom_by_id("devConsoleOverlay")) {
        g_con.built = 0;
        build();
        if (!g_con.built) return;
    }
    ui_mods_tick(client);
    apply_css_min_size();
    if (!g_con.visible) return;
    if (g_con.focus_pending) {
        g_con.focus_pending = 0;
        html_dom_focus(g_con.input);
        snprintf(g_con.last_value, sizeof(g_con.last_value), "%s", "");
        html_dom_get_value(g_con.input, g_con.last_value, sizeof(g_con.last_value));
    }
    poll_input();
}
