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
// by shared/src/chat_commands.c on server_bin. Offline that server is the in-process
// sandbox (client/src/net/offline.c), so they work there exactly as online; devConsole's
// offline map / mode / cycle versions (offlineServerCommands, below) stay local because
// they are the sandbox host's controls (local_game_host).
#include <console.h>
#include <krunker_net.h> // the official session's game and region (serverinfo, status)
#include <krunker_lobbies.h> // serverinfo of an official game: its list row (region, players, map, mode)
#include <ruleset.h>     // class / secondary completion and the class table (rules_classes, rules_weapons)
#include <ui_mods.h>
#include <ui_logic.h>
#include <backend_ui.h> // servers_ui_connect_direct / _disconnect / _reconnect
#include <html_ui.h>
#include <chat_commands.h>
#include <map_catalog.h>
#include <ai.h>
#include <app_mode.h> // editor: app_route_url_target
#include <nav_overlay.h>
#include <object_manager.h> // give: weapon / item names
#include <weapons.h>
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <update_ui.h>
#include <third_person.h>
#include <win_geom.h>   // the console box stays in the window (win_geom.h)
#include <game_reload.h> // `reload`: F5's full reload
#include <strike_ui.h>   // strike_reward: a sample Krunker Strike reward (strike_reward.c)
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
#define CON_GEOM_KEY "krk_dev_console_geom"
// KrunkNative: the console open or closed, kept across F5 / F4 restarts, page switches and the web
// build's reloads (devConsole.ts starts closed on every load)
#define CON_OPEN_KEY "kn_dev_console_open"
// devconsole.css .modal: padding 7px on a content-box, so offsetWidth = width + 14 (and the
// height likewise). Geometry is kept as the outer box (devConsole.ts saves offsetWidth /
// offsetHeight), so the CSS width / height written back is the outer size minus this.
#define CON_PAD 14.0f
#define CON_MIN_W (280.0f + CON_PAD) // .modal min-width: 280px / min-height: 160px, outer
#define CON_MIN_H (160.0f + CON_PAD)
#define CON_DEF_W 744.0f             // .modal width: min(744px, calc(100vw - 30px)): the outer box is at most vw - 16
#define CON_DEF_H 280.0f             // .modal height: min(280px, calc(100vh - 30px))
#define CON_DEF_MARGIN 8.0f          // .modal right: 8px; top: 8px
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
// data-kn-* glue attributes, onsubmit="return false" (the mod's submit listener
// preventDefault()s; the browser runs the attribute, the native engine has no forms), and the
// overlay's place in the page inline (devconsole.css has the same): over the whole viewport,
// above every z-index, so the console shows even when its sheet could not be added.
static const char OVERLAY_HTML[] =
    "<div id=\"devConsoleOverlay\" style=\"display: none; position: fixed; left: 0; top: 0; right: 0; bottom: 0; "
    "z-index: 2147483647; pointer-events: none;\"><div class=\"modal\" data-kn-pointer=\"1\">"
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
    int dev;                     // a developer command: hidden (help, completion, run) while the dev console is off
} ConCommand;

typedef struct {
    char key[CON_NAME_MAX];
    char value[CON_TEXT_MAX];
} ConPair;

// A '+name' bind holding its control: `key` is the GLFW key that pressed it, or CON_HOLD_WHEEL for a
// mwheel bind (released when the frame ends, after the input frame sampled the press).
#define CON_HOLDS_MAX 16
#define CON_HOLD_WHEEL (-2)
typedef struct {
    int key;
    char name[CON_NAME_MAX];
} ConHold;

#define CON_CANDS_MAX 256
typedef struct {
    char items[CON_CANDS_MAX][CON_NAME_MAX];
    int count;
} ConCands;

typedef struct {
    Client *client;
    int built;                   // the overlay exists in the current page
    int building;                // inside build(): update() must not recurse into build() again
    unsigned serial;             // html_ui_page_serial() of the document it was built / found in
    int sheet;                   // devconsole.css was added to that document
    HtmlElem overlay, modal, header, close_btn, logs, hints, input, submit, grip;
    int visible;
    int restored;                // the saved open state was read (CON_OPEN_KEY)
    int focus_pending;           // update(): setTimeout(() => input.focus(), 0)
    char last_value[CON_LINE_MAX];
    int log_lines;
    int banner;

    // every line still in the log (at most CON_LOG_MAX, as markup): a page switch or a page that
    // replaced the overlay gets the whole log again, and lines before the overlay exists wait here
    char *lines[CON_LOG_MAX];
    int line_head, line_count;

    ConCommand cmds[CON_CMDS_MAX];
    int cmd_count;
    ConPair binds[CON_BINDS_MAX];     // keyBinds (session only)
    int bind_count;
    ConHold holds[CON_HOLDS_MAX];     // '+' binds still held: released by their key's release or the frame's end
    int hold_count;
    ConPair aliases[CON_BINDS_MAX];   // aliases (session only)
    int alias_count;

    char history[CON_HISTORY_MAX][CON_LINE_MAX];
    int history_count;
    int history_idx;
    char draft[CON_LINE_MAX];

    char hint_lines[CON_HINTS_MAX][CON_LINE_MAX]; // the line a mousedown on each hint item sets ("" inert)
    int hint_count;
    int hint_sel;                // arrow-key selection: hint row index, -1 none (KrunkNative)
    char hint_sel_value[CON_LINE_MAX]; // input value the selection belongs to
    ConCands cycle;              // Tab cycle: candidates, the line before the token, the pick
    int cycle_on, cycle_index;
    char cycle_base[CON_LINE_MAX];
    char cycle_value[CON_LINE_MAX];

    int mode_override;           // offlineModeOverrides (game mode id), -1 none

    // drag / resize / clicks
    int dragging, resizing;
    float dx, dy, start_x, start_y, start_w, start_h;
    HtmlElem pressed;
    int save_timer;
    float fit_vw, fit_vh;        // the viewport the box was last fitted to (0: refit)
} ConState;

static ConState g_con = {.history_idx = -1, .mode_override = -1};

static void con_exec(const char *line);
static void reset_geom(void);
static void build(void);

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

// The overlay's handles belong to the document shown now (a page switch makes them stale until
// ui_console_tick finds or builds the overlay of the new one)
static int con_live(void) {
    return g_con.built && g_con.serial == html_ui_page_serial() && html_ui_is_ready();
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
    char *kept = strdup(html);
    if (kept) {
        if (g_con.line_count == CON_LOG_MAX) { // the oldest goes
            free(g_con.lines[g_con.line_head]);
            g_con.lines[g_con.line_head] = kept;
            g_con.line_head = (g_con.line_head + 1) % CON_LOG_MAX;
        } else {
            g_con.lines[(g_con.line_head + g_con.line_count++) % CON_LOG_MAX] = kept;
        }
    }
    if (!con_live()) return;
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

// The whole kept log into .logs (a fresh or another page's overlay)
static void render_lines(void) {
    size_t len = 1;
    for (int i = 0; i < g_con.line_count; i++) len += strlen(g_con.lines[(g_con.line_head + i) % CON_LOG_MAX]);
    char *all = malloc(len);
    if (!all) return;
    size_t at = 0;
    for (int i = 0; i < g_con.line_count; i++) {
        const char *line = g_con.lines[(g_con.line_head + i) % CON_LOG_MAX];
        const size_t n = strlen(line);
        memcpy(all + at, line, n);
        at += n;
    }
    all[at] = 0;
    html_dom_set_html(g_con.logs, all);
    free(all);
    g_con.log_lines = g_con.line_count;
    html_dom_scroll_to_bottom(g_con.logs);
}

static void clear_lines(void) {
    for (int i = 0; i < g_con.line_count; i++) free(g_con.lines[(g_con.line_head + i) % CON_LOG_MAX]);
    g_con.line_head = g_con.line_count = 0;
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
static const ConArg A_SNAP[] = {{"units|off", ARG_STRING, 0}};
static const ConArg A_UPDATE[] = {{"check|now|download", ARG_STRING, 0}};
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
static const ConArg A_MAP_OPT[] = {{"map", ARG_STRING, 1}};
static const ConArg A_MODE[] = {{"alias", ARG_STRING, 1}};
static const ConArg A_TEAM[] = {{"red|blue", ARG_STRING, 0}};
static const ConArg A_UNBAN[] = {{"accountId|name|id", ARG_STRING, 1}};
static const ConArg A_KPD[] = {{"account|apps", ARG_STRING, 0}, {"on|off", ARG_STRING, 1}}; // KrunkNative /kpd
static const ConArg A_BANIP[] = {{"name|id|ip", ARG_STRING, 1}};
static const ConArg A_IP[] = {{"ip", ARG_STRING, 1}};
static const ConArg A_AMOUNT[] = {{"amount", ARG_NUMBER, 1}};
static const ConArg A_GAME[] = {{"text", ARG_STRING, 1}};
// Krunker v7.1.4 bots (mod/devConsole.ts registrations; shared/src/chat_commands.c runs them)
static const ConArg A_BOT_ADD[] = {{"count", ARG_NUMBER, 1}, {"tfue|autoRespawn|key=value", ARG_STRING, 1}};
static const ConArg A_BOT_TYPE[] = {{"nextbot|tfue", ARG_STRING, 1}};
static const ConArg A_BOT_PLACE[] = {{"tfue|autoRespawn|key=value", ARG_STRING, 1}};
static const ConArg A_BOT_QUERY[] = {{"all|index|name", ARG_STRING, 1}};
static const ConArg A_BOT_CONFIG[] = {{"key=value", ARG_STRING, 1}};
static const ConArg A_BOT_DIFF[] = {{"value", ARG_NUMBER, 0}};
static const ConArg A_AUTOFILL[] = {{"on|off|players", ARG_STRING, 1}};
static const ConArg A_NAV_OFF[] = {{"off", ARG_STRING, 1}}; // show_navmeshes: [arg("off", "string", true)]
// KrunkNative teleports (shared/src/chat_commands.c cmd_tp / cmd_bring)
static const ConArg A_TP[] = {{"x|player", ARG_STRING, 0}, {"y", ARG_NUMBER, 1}, {"z", ARG_NUMBER, 1}};
static const ConArg A_BRING[] = {{"player|all", ARG_STRING, 0}, {"x", ARG_NUMBER, 1}, {"y", ARG_NUMBER, 1}, {"z", ARG_NUMBER, 1}};
static const ConArg A_BUILD_FILL[] = {{"count", ARG_NUMBER, 1}, {"kind", ARG_NUMBER, 1}}; // shared/src/chat_commands.c cmd_build_fill
// KrunkNative Object Manager (shared/src/chat_commands.c cmd_give / cmd_drop)
static const ConArg A_GIVE[] = {{"weapon|item", ARG_STRING, 0}, {"player", ARG_STRING, 1}};
// KrunkNative class (dev): the class, and the loadout's secondary weapon (the class's other weapons are its own)
static const ConArg A_CLASS[] = {{"name|id", ARG_STRING, 0}, {"secondary", ARG_STRING, 1}};
static const ConArg A_STRIKE_REWARD[] = {{"kind", ARG_STRING, 0}};
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

// A developer command (the dev console's gate, ui_mods_dev_console_enabled): listed, completed and run only
// while that is on.
static void register_dev(const char *name, const ConArg *args, const int nargs, const char *desc, ConRun run) {
    ConCommand *cmd = put_cmd(name, 0, args, nargs, desc, run);
    if (cmd) cmd->dev = 1;
}

static int cmd_visible(const ConCommand *cmd) {
    return !cmd->dev || ui_mods_dev_console_enabled();
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

// An official krunker.io game (krunker_net.h): the lobby's own row of krunker.io's list gives its
// region, map, mode and players (the server's values, not ours); a lobby the list does not name
// (a link to a custom game) reports what this client plays.
static void print_official_details(const Client *client, const int humans, const int active) {
    const Game *game = &client->game;
    const char *id = krunker_net_session_game();
    const cJSON *t = krunker_lobbies_find(id);
    const char *region = t ? cJSON_GetStringValue(cJSON_GetArrayItem(t, 1)) : NULL;
    if (!region || !region[0]) region = krunker_net_region();
    const cJSON *meta = t ? cJSON_GetArrayItem(t, 4) : NULL;
    appendf(COL_INFO, "Endpoint: krunker.io official game server (secure WebSocket)");
    if (t) {
        const char *map = cJSON_GetStringValue(cJSON_GetObjectItem(meta, "i"));
        const cJSON *mode = cJSON_GetObjectItem(meta, "mode");
        const GameModeConfig *m = cJSON_IsNumber(mode) ? mode_config(mode->valueint) : NULL;
        appendf(COL_INFO, "Lobby: %s | region %s (%s) | map %s | mode %s (%d)", id, krunker_region_name(region), region,
                map ? map : "(unknown)", m && m->alias ? m->alias : "(unknown)", cJSON_IsNumber(mode) ? mode->valueint : -1);
        appendf(COL_INFO, "Players: %d/%d (krunker.io's list)", (int) cJSON_GetNumberValue(cJSON_GetArrayItem(t, 2)),
                (int) cJSON_GetNumberValue(cJSON_GetArrayItem(t, 3)));
    } else {
        char map[96], mode[64];
        map_label(game, map, sizeof(map));
        mode_label(game, mode, sizeof(mode));
        appendf(COL_INFO, "Lobby: %s (not on krunker.io's list) | region %s (%s) | map %s | mode %s", id, krunker_region_name(region), region, map, mode);
        appendf(COL_INFO, "Players: %d/%d (this client)", active, humans);
    }
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
    if (!client->offline && krunker_net_session_game()[0]) {
        print_official_details(client, humans, active);
        return;
    }
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
    nav_overlay_map_switch(); // clearNavOverlayForMapSwitch + scheduleNavOverlayRebuild
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

// A catalogue map by name (map_catalog_resolve: quotes, case, spaces/underscores, a unique
// prefix), joined to the game's list like /map and Host Game (map_catalog_slot): the
// offline list is the public rotation, which leaves out Shipyard and Soul Sanctum.
// Numbers stay indices into the list /maps prints. -1 = not a catalogue map (or the
// candidates of an ambiguous prefix were printed, *reason = RES_AMBIGUOUS).
static int resolve_catalog_map(Game *game, const char *raw, ResReason *reason) {
    double d;
    if (js_number(raw, &d)) return -1;
    int cands[64];
    size_t cand_n = 0;
    const int index = map_catalog_resolve(raw, cands, sizeof(cands) / sizeof(cands[0]), &cand_n);
    if (index == -2) {
        char list[CON_LINE_MAX] = "";
        size_t len = 0;
        for (size_t i = 0; i < cand_n && i < sizeof(cands) / sizeof(cands[0]) && len < sizeof(list); i++) {
            const MapCatalogEntry *e = map_catalog_at((size_t) cands[i]);
            len += (size_t) snprintf(list + len, sizeof(list) - len, "%s%s", i ? ", " : "",
                                     e->source == MAP_SOURCE_OFFICIAL ? e->name : e->key);
        }
        appendf(COL_ERROR, "Map '%s' is ambiguous (%d matches): %s", raw, (int) cand_n, list);
        *reason = RES_AMBIGUOUS;
        return -1;
    }
    if (index < 0) return -1;
    const int slot = map_catalog_slot(&game->maps, &game->map_count, (size_t) index);
    if (slot < 0) {
        appendf(COL_ERROR, "Map '%s' could not be loaded.", map_catalog_at((size_t) index)->name);
        *reason = RES_AMBIGUOUS; // reported
        return -1;
    }
    *reason = RES_OK;
    return slot;
}

static void off_map(const int argc, char **argv) {
    Game *game = &con_client()->game;
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
    ResReason reason = RES_NOT_FOUND;
    int index = resolve_catalog_map(game, raw, &reason);
    if (index < 0 && reason == RES_AMBIGUOUS) return; // printed
    if (index < 0) index = resolve_offline_map(game, raw, &reason); // indices, non-catalogue maps
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

    // catalogue maps outside the list (rotation leaves out Shipyard / Soul Sanctum): `map` loads them
    char more[CON_LINE_MAX * 2] = "";
    size_t more_len = 0;
    for (size_t i = 0; i < map_catalog_count() && more_len < sizeof(more); i++) {
        const MapCatalogEntry *e = map_catalog_at(i);
        int listed = 0; // index_of_json: no map loads just to print the list
        for (size_t j = 0; j < game->map_count && !listed; j++) listed = map_catalog_index_of_json(game->maps[j]) == (int) i;
        if (listed) continue;
        more_len += (size_t) snprintf(more + more_len, sizeof(more) - more_len, "%s%s", more_len ? ", " : "",
                                      e->source == MAP_SOURCE_OFFICIAL ? e->name : e->key);
    }
    if (more[0]) {
        append("Also available (map <name>):", COL_INFO);
        append(more, COL_LOG);
    }
}

static void off_server(const int argc, char **argv) {
    (void) argc; (void) argv;
    print_server_details();
}

// offlineServerCommands
static const struct {
    const char *name;
    void (*run)(int, char **);
} OFFLINE_COMMANDS[] = { // the player / round cheats (god, noclip, kill, score, team, end, endless, borders, ...) go to the sandbox server
    {"server", off_server}, {"cycle", off_cycle}, {"map", off_map}, {"mode", off_mode}, {"modes", off_modes},
    {"maps", off_maps},
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
        if (!c || !cmd_visible(c)) {
            appendf(COL_ERROR, "unknown command: %s", argv[0]);
            return;
        }
        char args[256];
        fmt_args(c, args, sizeof(args));
        appendf(COL_INFO, "%s %s \xE2\x80\x94 %s", argv[0], args, c->desc);
        return;
    }
    int order[CON_CMDS_MAX];
    int n = 0;
    for (int i = 0; i < g_con.cmd_count; i++) {
        if (cmd_visible(&g_con.cmds[i])) order[n++] = i;
    }
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
    if (con_live()) html_dom_set_html(g_con.logs, "");
    g_con.log_lines = 0;
    clear_lines();
}

static void cmd_echo(const int argc, char **argv, const char *rest) {
    (void) rest;
    char text[CON_LINE_MAX];
    join_args(argc, argv, 0, text, sizeof(text));
    append(text, COL_LOG);
}

// tool_snap [units|off]: the Tool Gun's / Physics Gun's grid step (the Object Manager's Snap field;
// client-local, persisted as kn_omSnapStep). No argument: the current step. Not a player command:
// no target selector.
static void cmd_tool_snap(const int argc, char **argv, const char *rest) {
    (void) rest;
    char text[32];
    if (argc >= 1 && argv[0][0]) {
        const int off = !strcasecmp(argv[0], "off") || !strcasecmp(argv[0], "none");
        char *end = NULL;
        const double v = off ? 0.0 : strtod(argv[0], &end);
        if (!off && (!end || *end || !isfinite(v) || v < 0.0)) {
            appendf(COL_ERROR, "tool_snap: a step from %g to %g units, or off", (double) OB_SNAP_MIN, (double) OB_SNAP_MAX);
            return;
        }
        om_build_set_snap((float) v);
        object_manager_ui_snap_changed();
    }
    om_build_snap_text(text, sizeof(text));
    appendf(COL_INFO, "tool_snap %s", text);
}

// editor: the loaded map (its raw JSON, so hub/custom/online maps work too) opens in the map editor
// as an unsaved copy, through the same storage handoff as the editor's "Open in New Tab"
// (editor.c open_start_projects: knEditorHandoff + editor.html?handoff=1).
static void cmd_editor(const int argc, char **argv, const char *rest) {
    (void) argc;
    (void) argv;
    (void) rest;
    Client *client = con_client();
    const Map *map = client && client->game.ready ? client->game.map : NULL;
    if (!map || !map->raw_data) {
        append("No map loaded.", COL_ERROR);
        return;
    }
    char *text = cJSON_PrintUnformatted(map->raw_data);
    if (!text) {
        append("Could not export the map.", COL_ERROR);
        return;
    }
    html_storage_set("knEditorHandoff", text);
    cJSON_free(text);
    if (!app_route_url_target("editor.html?handoff=1", 1)) {
        html_storage_set("knEditorHandoff", NULL);
        append("The map editor is not available in this build.", COL_ERROR);
        return;
    }
    appendf(COL_INFO, "Opening %s in the map editor (a copy: Save asks where).", map->name ? map->name : "the map");
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

// ---------------------------------------------------------------- '+name' holds

// The control name of a '+name' / '-name' command (case kept: "playerList")
static void hold_name(const char *cmd, char *name, const size_t n) {
    size_t len = strcspn(cmd + 1, " \t");
    if (len >= n) len = n - 1;
    memcpy(name, cmd + 1, len);
    name[len] = 0;
}

static int hold_named(const char *name) {
    for (int i = 0; i < g_con.hold_count; i++) {
        if (!strcmp(g_con.holds[i].name, name)) return 1;
    }
    return 0;
}

// A '+name' bind pressed `key` (or a mwheel bind: CON_HOLD_WHEEL): the control stays held until then
static void hold_add(const int key, const char *name) {
    if (!name[0]) return;
    for (int i = 0; i < g_con.hold_count; i++) {
        if (g_con.holds[i].key == key && !strcmp(g_con.holds[i].name, name)) return;
    }
    if (g_con.hold_count == CON_HOLDS_MAX) return;
    g_con.holds[g_con.hold_count].key = key;
    snprintf(g_con.holds[g_con.hold_count].name, sizeof(g_con.holds[0].name), "%s", name);
    g_con.hold_count++;
}

// The holds of `key` end ('-name'); a control another key still holds stays down
static void hold_release(const int key) {
    for (int i = 0; i < g_con.hold_count;) {
        if (g_con.holds[i].key != key) {
            i++;
            continue;
        }
        char name[CON_NAME_MAX];
        snprintf(name, sizeof(name), "%s", g_con.holds[i].name);
        memmove(&g_con.holds[i], &g_con.holds[i + 1], sizeof(ConHold) * (size_t) (g_con.hold_count - i - 1));
        g_con.hold_count--;
        if (!hold_named(name)) ui_controls_bind_hold(con_client(), name, 0);
    }
}

// The keyBinds of the mwheelup / mwheeldown names (ui_controls_scroll_event, in game, console hidden):
// the command runs; a '+name' holds the control until the frame ends. 1 when the direction is bound.
int ui_console_wheel(const int up) {
    if (g_con.visible) return 0;
    const ConPair *bind = pair_find(g_con.binds, g_con.bind_count, up ? "mwheelup" : "mwheeldown");
    if (!bind) return 0;
    char cmd[CON_TEXT_MAX];
    snprintf(cmd, sizeof(cmd), "%s", bind->value);
    trim(cmd);
    if (cmd[0] == '+') { // a tap: no log line per wheel tick
        char name[CON_NAME_MAX];
        hold_name(cmd, name, sizeof(name));
        ui_controls_bind_hold(con_client(), name, 1);
        hold_add(CON_HOLD_WHEEL, name);
    } else {
        con_exec(cmd);
    }
    return 1;
}

// After the input frame (ui_controls_end_frame): a wheel's '+' tap lets go
void ui_console_wheel_end_frame(void) {
    hold_release(CON_HOLD_WHEEL);
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
#endif

// connect [local|hostname]: devConsole points the matchmaker at the host and seeks a game;
// KrunkNative servers are reached directly (host[:port]; UDP, then TCP; the web build goes
// through the bridge's /native?target=). Like a Source console the server is pinned:
// matchmaking never moves the session elsewhere until Play / the server browser
// (servers_ui.c g_pin), and `reconnect` comes back here. A game link or lobby id
// (connect CH2:kz862, an invite link) joins that game through the matchmaker instead.
static void cmd_connect(const int argc, char **argv, const char *rest) {
    char input[CON_TEXT_MAX];
    if (rest && *rest) snprintf(input, sizeof(input), "%s", rest);
    else join_args(argc, argv, 0, input, sizeof(input));
    trim(input);
    char err[200] = "";
    if (input[0] && con_client()) {
        // a game link (krunker.io's official lobby, a KrunkNative link) or a lobby id joins that game
        const int game = servers_ui_connect_game(input, err, sizeof(err));
        if (game > 0) {
            appendf(COL_INFO, "Joining game %s...", input);
            return;
        }
        if (game < 0) {
            printf("[mm] connect refused: %s\n", err);
            append(err, COL_ERROR);
            return;
        }
    }
    char host[160];
    int port = NET_DEFAULT_PORT;
    if (!servers_ui_parse_address(input, host, sizeof(host), &port, err, sizeof(err))) {
        printf("[mm] connect refused: %s\n", err);
        append(err, COL_ERROR); // the same text as the Connect window
        return;
    }
    char low[CON_TEXT_MAX];
    snprintf(low, sizeof(low), "%s", input[0] ? input : "local");
    lower(low);
    if (!strcmp(low, "local") || !strcmp(low, "default")) append("Connecting through local...", COL_INFO);
    else appendf(COL_INFO, "Connecting through %s:%d...", host, port);
    appendf(COL_INFO, "  Server: tcp://%s:%d", host, port);
    if (!con_client()) return;
    servers_ui_connect_direct(host, port); // pinned; switch_server leaves an offline game (its spinner: direct_join)
}

// disconnect: socket closed, back to the menu, and nothing reconnects (no matchmaker seek,
// no transport retry) until Play / a server / `connect` / `reconnect`. (devConsole goes
// to "/", which on KrunkNative's page would seek a new public game.)
static void cmd_disconnect(const int argc, char **argv, const char *rest) {
    (void) argc; (void) argv; (void) rest;
    Client *client = con_client();
    if (!client) return;
    servers_ui_disconnect();
    client_set_menu_open(client, 1);
    append("Disconnected.", COL_INFO);
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

// reconnect: location.reload() in devConsole; KrunkNative reconnects to the last server
// (a `connect` target, a Direct Connect server or the matchmade lobby, sought again for a
// fresh join ticket), also after `disconnect`. Offline (also after a `disconnect` from the
// offline game): the local game starts over on the same map. The Reconnect key (unbound by
// default, ui_controls.c) runs this command, and F5's reload (game_reload.c) ends with it.
int ui_console_reconnect(void) {
    Client *client = con_client();
    if (!client) return 0;
    if (client->offline) {
        local_game_host(client, client->game.ready ? client->game.current_map_index : -1, client->game.ready ? game_mode_id(&client->game) : 0);
        append("Restarted the offline game.", COL_INFO);
        return 1;
    }
    if (servers_ui_reconnect_offline()) {
        // `disconnect` left the offline game: it starts again on the map it played
        const Game *game = &client->game;
        if (game->ready && game->current_map_index >= 0 && (size_t) game->current_map_index < game->map_count) {
            client->offline_start_map = game->maps[game->current_map_index];
        }
        client->net_should_quit = 1; // as menu_request_play: no transport retries behind it
        const int started = local_game_start(client);
        if (started) append("Restarted the offline game.", COL_INFO);
        else append("reconnect failed: the offline game did not start", COL_ERROR);
        client->offline_start_map = NULL;
        return started;
    }
    if (!servers_ui_reconnect()) {
        append("reconnect: no server to reconnect to", COL_ERROR);
        return 0;
    }
    appendf(COL_INFO, "Reconnecting to %s:%u...", client->net_address, (unsigned int) client->net_port);
    menu_show_connecting();
    return 1;
}

static void cmd_reconnect(const int argc, char **argv, const char *rest) {
    (void) argc; (void) argv; (void) rest;
    ui_console_reconnect();
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

// playCustomMap() (the single player game) + clearPingCounter(): leave the server for the
// in-process sandbox server. `catalog_index` (-1 none) is the map it starts on, or the one a
// running offline game reloads on (local_game_pick_map).
static void go_offline(Client *client, const int catalog_index, const char *command) {
    nav_overlay_map_switch(); // runOfflineSandbox: clearNavOverlayForMapSwitch + scheduleNavOverlayRebuild
#ifndef __EMSCRIPTEN__
    if (!client->offline) stop_net(client);
#else
    if (!client->offline && client->net_connected) client_net_disconnect(client, "Offline", "000000");
    client->net_connected = 0;
#endif
    if (catalog_index >= 0 && local_game_pick_map(client, catalog_index) < 0) {
        appendf(COL_ERROR, "%s failed: map %s could not be loaded", command, map_catalog_at((size_t) catalog_index)->key);
        return;
    }
    menu_host_offline();
    client->offline_start_map = NULL; // consumed by local_game_start (or a running game reloaded)
    clear_ping_counter();
    if (!client->offline) appendf(COL_ERROR, "%s failed: the offline game did not start", command);
}

// offline [map]: the offline game (--offline / web ?offline=1): the running one stays, else it
// starts on the startup map or a random rotation map; a map (catalogue key, name or path under
// assets/maps: local_game_map_lookup) picks where it plays (--map / ?map=).
static void cmd_offline(const int argc, char **argv, const char *rest) {
    char name[CON_TEXT_MAX];
    if (rest && *rest) snprintf(name, sizeof(name), "%s", rest);
    else join_args(argc, argv, 0, name, sizeof(name));
    trim(name);
    Client *client = con_client();
    if (!client) return;
    const int index = name[0] ? local_game_map_lookup(name) : -1;
    if (name[0] && index < 0) {
        appendf(COL_ERROR, "offline: map '%s' not found. Use maps to list choices.", name);
        return;
    }
    go_offline(client, index, "offline");
}

// sandbox: the offline game on the sandbox map (--sandbox / web ?sandbox): v7.1.4's
// config.blankMap, what the editor's Test plays with no map of its own (LOCAL_GAME_SANDBOX_MAP).
static void cmd_sandbox(const int argc, char **argv, const char *rest) {
    (void) argc; (void) argv; (void) rest;
    Client *client = con_client();
    if (!client) return;
    const int index = local_game_map_lookup(LOCAL_GAME_SANDBOX_MAP);
    if (index < 0) {
        append("sandbox failed: the map catalogue has no sandbox map (" LOCAL_GAME_SANDBOX_MAP ")", COL_ERROR);
        return;
    }
    go_offline(client, index, "sandbox");
}

// console_reset (KrunkNative): the console back at the stylesheet's place and size (top right,
// 744 x 280 at most), the saved krk_dev_console_geom gone. For a console a drag or an old save
// left somewhere awkward; one outside the window is brought back on its own (con_fit).
static void cmd_console_reset(const int argc, char **argv, const char *rest) {
    (void) argc; (void) argv; (void) rest;
    reset_geom();
    append("Console position and size reset.", COL_INFO);
}

// reload (KrunkNative): F5's full reload (game_reload.h, doc/console.md "F5: full reload")
static void cmd_reload(const int argc, char **argv, const char *rest) {
    (void) argc; (void) argv; (void) rest;
    append("Reloading...", COL_INFO);
    game_reload_request("console");
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

// show_navmeshes [off]: "off", "0" or "clear" clears, anything else builds (registerLocal
// "show_navmeshes", nav_overlay.c)
static void cmd_show_navmeshes(const int argc, char **argv, const char *rest) {
    (void) rest;
    if (argc && (!strcmp(argv[0], "off") || !strcmp(argv[0], "0") || !strcmp(argv[0], "clear"))) {
        nav_overlay_clear(1);
        return;
    }
    nav_overlay_show(con_client());
}

// the game transport (WebRTC / WebTransport / WebSocket, UDP / TCP), its session counters and
// why a transport was skipped (net_notes.c client_net_info)
static void cmd_netinfo(const int argc, char **argv, const char *rest) {
    (void) argc; (void) argv; (void) rest;
    char info[2048];
    client_net_info(con_client(), info, sizeof(info));
    for (char *line = info, *end; line && *line; line = end ? end + 1 : NULL) {
        end = strchr(line, '\n');
        if (end) *end = 0;
        append(line, COL_INFO);
    }
}

// thirdperson / firstperson (Source style, KrunkNative): the over-the-shoulder camera
// (client/src/third_person.c). thirdperson asks the server ("/thirdperson on"), which grants it
// offline, to devs and to custom games whose host allows it, and refuses it elsewhere with a
// notify; firstperson goes back at once.
static void cmd_thirdperson(const int argc, char **argv, const char *rest) {
    (void) argc; (void) argv; (void) rest;
    third_person_request(con_client());
}

static void cmd_firstperson(const int argc, char **argv, const char *rest) {
    (void) argc; (void) argv; (void) rest;
    third_person_first_person(con_client());
}

// strike_reward <kind>: a sample Krunker Strike reward screen (battlepass: its Awesome! opens the Battle Pass)
static void cmd_strike_reward(const int argc, char **argv, const char *rest) {
    (void) rest;
    if (argc < 1 || !strike_reward_sample(argv[0])) {
        append("usage: strike_reward battlepass", COL_LOG);
        return;
    }
}

// class <name|id> [secondary] (KrunkNative dev): the server switches a spawned player's class now (the offline
// sandbox only for now). The line goes out as typed, quotes included, so the server reads a quoted class name
// ("Spray N Pray") as one argument. The server refuses what it may not give and says why.
static void cmd_class(const int argc, char **argv, const char *rest) {
    (void) argc; (void) argv;
    char line[CON_LINE_MAX];
    snprintf(line, sizeof(line), "/class %s", rest ? rest : "");
    send_server(line);
}

// logout (KrunkNative): the menu's Logout (account_ui.c logoutAcc: the saved token dropped and
// revoked, the social socket signed out, the guest header). Client-local: nothing goes to the
// game server, so its command auth is not involved.
static void cmd_logout(const int argc, char **argv, const char *rest) {
    (void) argc; (void) argv; (void) rest;
    if (!account_ui_logout()) {
        append("not logged in", COL_INFO);
        return;
    }
    append("logged out", COL_REPLY);
}

// update [check|now|download]: the client update (update_ui.c, doc/backend.md "Client updates")
static void cmd_update(const int argc, char **argv, const char *rest) {
    (void) rest;
    update_ui_console(argc, argv);
}

// autofill [on|off|<players>] (Krunker Box Office): the server's bot fill (chat's /autofill, shared/src/chat_commands.c).
// Sent like the other bot_* commands; the server answers the host and developers and refuses everyone else with a toast.
// Official krunker.io games have no bot fill, so the console says so here and sends nothing.
static void cmd_autofill(const int argc, char **argv, const char *rest) {
    (void) argc; (void) argv;
    const Client *client = con_client();
    if (client && !client->offline && client->game.is_official) {
        append("Autofill is not available on official krunker.io games.", COL_ERROR);
        return;
    }
    char line[CON_LINE_MAX + 16];
    snprintf(line, sizeof(line), "/autofill%s%s", rest[0] ? " " : "", rest);
    send_server(line);
}

static void register_commands(void) {
    if (g_con.cmd_count) return;
    register_local("help", ARGS(A_CMD), "list commands", cmd_help);
    register_local("clear", NO_ARGS, "clear console output", cmd_clear);
    register_local("cls", NO_ARGS, "alias for clear", cmd_clear);
    register_local("echo", ARGS(A_TEXT), "print text to console", cmd_echo);
    register_local("print", ARGS(A_TEXT), "alias for echo", cmd_echo);
    register_local("editor", NO_ARGS, "open the current map in the map editor (an unsaved copy)", cmd_editor);
    register_local("tool_snap", ARGS(A_SNAP), "the Tool Gun's grid step in units (0.1 to 100), or off", cmd_tool_snap);
    register_local("bind", ARGS(A_BIND), "bind a key to a command (session only)", cmd_bind);
    register_local("unbind", ARGS(A_KEY), "remove a key bind", cmd_unbind);
    register_local("alias", ARGS(A_ALIAS), "define a command alias", cmd_alias);
    register_local("exec", ARGS(A_COMMANDS), "run multiple commands separated by ;", cmd_exec);
    register_local("connect", ARGS(A_HOSTNAME), "seek and connect through a matchmaker hostname", cmd_connect);
    register_local("disconnect", NO_ARGS, "disconnect and reset to default endpoints", cmd_disconnect);
    register_local("quit", NO_ARGS, "close the window", cmd_quit);
    register_local("reconnect", NO_ARGS, "reconnect to the last server", cmd_reconnect);
    register_local("logout", NO_ARGS, "log out of your account (the menu's Logout)", cmd_logout);
    register_local("reload", NO_ARGS, "full reload in place (F5 relaunches): settings, binds, textures, models, sounds, shaders, menu; then rejoin", cmd_reload);
    register_local("console_reset", NO_ARGS, "put the console back at its default place and size", cmd_console_reset);
    register_local("offline", ARGS(A_MAP_OPT), "switch to offline mode", cmd_offline);
    register_local("sandbox", NO_ARGS, "play offline on the sandbox (editor test) map", cmd_sandbox);
    register_local("ahk", NO_ARGS, "show current antihack key", cmd_ahk);
    register_local("version", NO_ARGS, "show current client version", cmd_version);
    register_local("build", NO_ARGS, "alias for version", cmd_version);
    register_local("verison", NO_ARGS, "alias for version", cmd_version);
    register_local("update", ARGS(A_UPDATE), "check for a newer client (update check), install it through the launcher (update now) or download it", cmd_update);
    register_local("toggle", ARGS(A_VAR), "toggle a server boolean command (god, noclip, ...)", cmd_toggle);
    register_local("say", ARGS(A_TEXT), "say something in chat", cmd_say);
    register_local("status", NO_ARGS, "show current server connection details", cmd_status);
    register_local("serverinfo", NO_ARGS, "alias for status", cmd_status);
    register_local("pos", NO_ARGS, "show local player position", cmd_pos);
    register_local("position", NO_ARGS, "alias for pos", cmd_pos);
    register_local("rot", NO_ARGS, "show local player rotation", cmd_rot);
    register_local("rotation", NO_ARGS, "alias for rot", cmd_rot);
    register_local("ping", NO_ARGS, "show current ping", cmd_ping_local);
    register_local("thirdperson", NO_ARGS, "over-the-shoulder camera (offline, devs, custom games that allow it)", cmd_thirdperson);
    register_local("firstperson", NO_ARGS, "back to the first-person camera", cmd_firstperson);
    register_dev("strike_reward", ARGS(A_STRIKE_REWARD), "a sample Krunker Strike reward screen (battlepass: Awesome! opens the Battle Pass)", cmd_strike_reward);
    register_dev("class", ARGS(A_CLASS), "switch your class now (offline sandbox): its loadout and health, and the secondary weapon", cmd_class);
    register_local("netinfo", NO_ARGS, "show the game transport (WebRTC/WebSocket, UDP/TCP) and its state", cmd_netinfo);
    register_local("show_navmeshes", ARGS(A_NAV_OFF), "visualize generated source-style nav meshes in 3D (off to clear)", cmd_show_navmeshes);
    // Server commands (mirror server/packetHandlers.ts cmds table)
    register_server("god", ARGS(A_TOGGLE), "toggle god mode (self or target)");
    register_server("ungod", ARGS(A_TARGET), "turn god mode off (self, target or selector)");
    register_server("noclip", ARGS(A_TOGGLE), "toggle noclip (self or target)");
    register_server("smite", ARGS(A_TOGGLE), "toggle smite-on-hit (self or target)");
    register_server("notarget", ARGS(A_TOGGLE), "toggle notarget: bots and AIs ignore you (self, target or selector)");
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
    register_server("kpd", ARGS(A_KPD), "grant or revoke K.P.D officer (on|off), or list the K.P.D applications (apps)");
    register_server("ip", ARGS(A_NAME_ID), "show a player's IP");
    register_server("banip", ARGS(A_BANIP), "ban an IP in this lobby");
    register_server("unbanip", ARGS(A_IP), "unban an IP");
    register_server("ipbans", NO_ARGS, "list banned IPs");
    register_server("score", ARGS(A_AMOUNT), "add score (default +100)");
    register_server("summon", ARGS(A_NAME), "spawn a Soul Sanctum AI by name (no arg lists summonable names)");
    register_server("game", ARGS(A_GAME), "send a game command");
    register_server("bot", ARGS(A_BOT_ADD), "spawn player-targeting auto-respawn bots (tfue=off for full-strength bots)");
    register_server("bot_add", ARGS(A_BOT_ADD), "alias for bot");
    register_server("bot_type", ARGS(A_BOT_TYPE), "show or switch bot manager type (switching removes old bots)");
    register_server("bot_place", ARGS(A_BOT_PLACE), "spawn a bot at your current position");
    register_server("bot_kick", ARGS(A_BOT_QUERY), "remove bots by name/index or all bots");
    register_server("bot_kill", ARGS(A_BOT_QUERY), "kill bots by name/index or all bots");
    register_server("bot_list", NO_ARGS, "list spawned bots");
    register_server("bot_nav", NO_ARGS, "bot navigation counters and routes");
    register_server("bot_config", ARGS(A_BOT_CONFIG), "show or update default bot options");
    register_server("bot_mimic", ARGS(A_ON_OFF), "bots copy one dev's inputs: bot_mimic on takes control, off releases it");
    register_server("bot_stop", ARGS(A_ON_OFF), "toggle bot movement and attacking");
    register_server("bot_start", NO_ARGS, "resume bots after bot_stop");
    register_server("bot_resume", NO_ARGS, "alias for bot_start");
    register_server("bot_difficulty", ARGS(A_BOT_DIFF), "set default bot difficulty");
    register_local("autofill", ARGS(A_AUTOFILL), "bots fill the lobby on / off, or to <players>, for this server (the host and developers)", cmd_autofill);
    register_server("tp", ARGS(A_TP), "teleport yourself to x y z (feet, as pos prints) or to a player");
    register_server("bring", ARGS(A_BRING), "bring a player (or all) to you, or to x y z");
    register_server("build_fill", ARGS(A_BUILD_FILL), "place count player builds in a stack beside you (a placement load test)");
    register_server("give", ARGS(A_GIVE), "give a weapon (an extra loadout slot) or an item; others: devs and the host");
    register_server("drop", NO_ARGS, "drop the weapon in your hand in front of you");
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
    if (line[0] == '+' || line[0] == '-') { // '+jump' presses the control, '-jump' lets go (a bind's hold)
        char name[CON_NAME_MAX];
        hold_name(line, name, sizeof(name));
        ui_controls_bind_hold(con_client(), name, line[0] == '+');
        return;
    }
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
    if (!c || !cmd_visible(c)) return; // unknown commands are silent (and a dev command with the dev console off)
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
    char msg[192];
    ui_bhop_announce(msg, sizeof(msg)); // the chat line and the bhop toast: the current mode's name (v3.1.1 /bhop)
    append(msg, COL_REPLY);
    return 1;
}

// KrunkNative: "/clear" empties your own chat log (#chatList), locally; nothing is sent.
static int clear_chat_command(const char *line) {
    char low[16];
    snprintf(low, sizeof(low), "%.15s", line);
    lower(low);
    if (strcmp(low, "/clear")) return 0;
    hud_ui_clear_chat();
    return 1;
}

int ui_console_chat_command(Client *client, const char *text) {
    if (client) g_con.client = client;
    if (!text) return 0;
    char line[CON_LINE_MAX];
    snprintf(line, sizeof(line), "%s", text);
    trim(line);
    if (bhop_command(line)) return 1;
    if (clear_chat_command(line)) return 1;
    return run_offline_server_command(line); // `label === "ct" && this.socket === null`
}

// ---------------------------------------------------------------- hints / completion

static void set_input(const char *value) {
    if (!con_live()) return;
    html_dom_set_value(g_con.input, value);
    snprintf(g_con.last_value, sizeof(g_con.last_value), "%s", value);
}

static void clear_hints(void) {
    if (!con_live()) return;
    html_dom_set_html(g_con.hints, "");
    html_dom_set_class(g_con.hints, "isOpen", 0);
    g_con.hint_count = 0;
    g_con.hint_sel = -1;
}

// One hint row; `pick` is the whole line a mousedown on it puts in the input ("" = inert).
static void add_hint_item(const char *label, const char *meta, const int active, const char *pick) {
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
    if (g_con.hint_count < CON_HINTS_MAX) snprintf(g_con.hint_lines[g_con.hint_count++], CON_LINE_MAX, "%s", pick ? pick : "");
}

// ---- argument completion (KrunkNative extension: devConsole.ts completes command names only)

typedef struct {
    int is_command;              // completing the command word itself
    char cmd[CON_NAME_MAX];      // lowercased, without the leading "/"
    int arg;                     // index of the argument being completed
    char prefix[CON_LINE_MAX];   // what is typed of it so far (quotes stripped)
    size_t token_start;          // where that token starts in the line
} LineCtx;

static void parse_line(const char *v, LineCtx *c) {
    memset(c, 0, sizeof(*c));
    const char *space = strchr(v, ' ');
    if (!space) {
        c->is_command = 1;
        c->token_start = v[0] == '/' ? 1 : 0;
        snprintf(c->prefix, sizeof(c->prefix), "%s", v + c->token_start);
        return;
    }
    const size_t head = (size_t) (space - v);
    const size_t skip = v[0] == '/' ? 1 : 0;
    snprintf(c->cmd, sizeof(c->cmd), "%.*s", (int) (head - skip < sizeof(c->cmd) - 1 ? head - skip : sizeof(c->cmd) - 1), v + skip);
    lower(c->cmd);
    const char *p = space;
    for (;;) {
        while (*p == ' ') p++;
        const char *start = p;
        if (*p == '"') {
            const char *close = strchr(p + 1, '"');
            p = close ? close + 1 : p + strlen(p);
        } else {
            while (*p && *p != ' ') p++;
        }
        if (!*p) { // the token under the caret (possibly empty)
            c->token_start = (size_t) (start - v);
            snprintf(c->prefix, sizeof(c->prefix), "%s", start[0] == '"' ? start + 1 : start);
            const size_t n = strlen(c->prefix);
            if (n && c->prefix[n - 1] == '"' && start[0] == '"') c->prefix[n - 1] = 0;
            return;
        }
        c->arg++;
    }
}

static int ci_starts_with(const char *s, const char *prefix) {
    for (; *prefix; s++, prefix++) {
        if (!*s || tolower((unsigned char) *s) != tolower((unsigned char) *prefix)) return 0;
    }
    return 1;
}

static void cand_add(ConCands *c, const char *value, const char *prefix) {
    if (!value || !*value || c->count >= CON_CANDS_MAX || !ci_starts_with(value, prefix)) return;
    for (int i = 0; i < c->count; i++) if (!strcmp(c->items[i], value)) return;
    snprintf(c->items[c->count++], CON_NAME_MAX, "%s", value);
}

// The v3.1.1 map catalogue (shared/src/map_catalog.c): official maps by name, community
// maps by file stem, which is what cmd_map resolves and cmd_maps prints.
static void con_map_candidates(ConCands *c, const char *prefix) {
    for (size_t i = 0; i < map_catalog_count(); i++) {
        const MapCatalogEntry *e = map_catalog_at(i);
        if (e) cand_add(c, e->source == MAP_SOURCE_OFFICIAL ? e->name : e->key, prefix);
    }
}

// The player names, then the target selectors (shared/include/target_select.h) once one is typed
static void con_player_candidates(ConCands *c, const char *prefix) {
    const Client *client = con_client();
    if (prefix[0] == '*' || prefix[0] == '@' || prefix[0] == '!') {
        static const char *const SELECTORS[] = {"*", "@me", "@others", "@bots", "@humans", "@team", "@enemies", "@alive", "@dead"};
        for (size_t i = 0; i < sizeof(SELECTORS) / sizeof(SELECTORS[0]); i++) cand_add(c, SELECTORS[i], prefix);
    }
    if (!client) return;
    for (size_t i = 0; i < client->game.player_count; i++) {
        const Player *p = client->game.players[i];
        if (p && p->name[0]) cand_add(c, p->name, prefix);
    }
}

static void con_mode_candidates(ConCands *c, const char *prefix) {
    int count = 0;
    const GameModeConfig *modes = mode_list(&count);
    for (int i = 0; i < count; i++) cand_add(c, modes[i].alias, prefix);
    if (con_offline()) {
        cand_add(c, "default", prefix); // devConsole's offline mode: clears the cycle override
        cand_add(c, "reset", prefix);
    }
}

static void con_key_candidates(ConCands *c, const char *prefix) {
    for (int i = 0; i < g_con.bind_count; i++) cand_add(c, g_con.binds[i].key, prefix);
    cand_add(c, "mwheelup", prefix); // the mouse wheel's two directions (ui_console_wheel)
    cand_add(c, "mwheeldown", prefix);
    static const int KEYS[] = {GLFW_KEY_F1, GLFW_KEY_F2, GLFW_KEY_F3, GLFW_KEY_F4, GLFW_KEY_F5, GLFW_KEY_F6, GLFW_KEY_F7,
                               GLFW_KEY_F8, GLFW_KEY_F9, GLFW_KEY_F10, GLFW_KEY_F11, GLFW_KEY_F12, GLFW_KEY_SPACE,
                               GLFW_KEY_TAB, GLFW_KEY_LEFT_SHIFT, GLFW_KEY_LEFT_CONTROL, GLFW_KEY_LEFT_ALT};
    for (int k = GLFW_KEY_A; k <= GLFW_KEY_Z; k++) cand_add(c, ui_mods_key_value(k, 0), prefix); // e.key
    for (int k = GLFW_KEY_0; k <= GLFW_KEY_9; k++) cand_add(c, ui_mods_key_value(k, 0), prefix);
    for (size_t i = 0; i < sizeof(KEYS) / sizeof(KEYS[0]); i++) cand_add(c, ui_mods_key_value(KEYS[i], 0), prefix);
    for (int k = GLFW_KEY_A; k <= GLFW_KEY_Z; k++) cand_add(c, ui_mods_key_code(k), prefix);    // e.code
    for (int k = GLFW_KEY_0; k <= GLFW_KEY_9; k++) cand_add(c, ui_mods_key_code(k), prefix);
}

static void con_command_candidates(ConCands *c, const char *prefix) {
    for (int i = 0; i < g_con.cmd_count; i++) {
        if (cmd_visible(&g_con.cmds[i])) cand_add(c, g_con.cmds[i].name, prefix);
    }
}

static void con_words(ConCands *c, const char *prefix, const char *const *words) {
    for (; *words; words++) cand_add(c, *words, prefix);
}

// The class table's names (Game.classes, else the ruleset's): "Spray N Pray" and the like insert quoted (quote_into).
static void con_class_candidates(ConCands *c, const char *prefix) {
    const Client *client = con_client();
    const ClassConfig *classes = client && client->game.classes ? client->game.classes : rules_classes();
    const size_t count = client && client->game.class_count ? client->game.class_count : rules_class_count();
    for (size_t i = 0; i < count; i++) {
        if (classes[i].name) cand_add(c, classes[i].name, prefix);
    }
}

// The weapons a class's secondary slot can take (Weapon.secondary; the server's gates decide the rest).
static void con_secondary_candidates(ConCands *c, const char *prefix) {
    const Client *client = con_client();
    const Weapon *const *weapons = client && client->game.weapons ? (const Weapon *const *) client->game.weapons : rules_weapons();
    const size_t count = client && client->game.weapon_count ? client->game.weapon_count : rules_weapon_count();
    for (size_t i = 0; i < count; i++) {
        if (weapons[i] && weapons[i]->secondary && weapons[i]->name) cand_add(c, weapons[i]->name, prefix);
    }
}

static int cmd_is(const char *cmd, const char *const *names) {
    for (; *names; names++) if (!strcmp(cmd, *names)) return 1;
    return 0;
}

#define WORDS(...) ((const char *const[]) {__VA_ARGS__, NULL})

// What fits argument `arg` of `cmd`, filtered by `prefix`.
static void arg_candidates(const char *cmd, const int arg, const char *prefix, ConCands *c) {
    static const char *const ON_OFF[] = {"on", "off", NULL};
    const ConCommand *dev = find_cmd(cmd);
    if (dev && !cmd_visible(dev)) return; // a dev command's arguments are offered only with the dev console on
    if (cmd_is(cmd, WORDS("help", "exec"))) con_command_candidates(c, prefix);
    else if (cmd_is(cmd, WORDS("alias"))) {
        if (arg == 0) for (int i = 0; i < g_con.alias_count; i++) cand_add(c, g_con.aliases[i].key, prefix);
        else con_command_candidates(c, prefix);
    } else if (!strcmp(cmd, "bind")) {
        if (arg == 0) con_key_candidates(c, prefix);
        else con_command_candidates(c, prefix);
    } else if (!strcmp(cmd, "unbind")) {
        if (arg == 0) for (int i = 0; i < g_con.bind_count; i++) cand_add(c, g_con.binds[i].key, prefix);
    } else if (!strcmp(cmd, "toggle")) {
        if (arg == 0) con_words(c, prefix, WORDS("god", "noclip", "smite", "forcehide", "unlimitedammo", "borders", "forcecol", "endless", "bhop"));
    } else if (cmd_is(cmd, WORDS("god", "ungod", "noclip", "smite", "notarget", "forcehide", "unlimitedammo"))) {
        if (arg == 0) con_player_candidates(c, prefix); // resolveDevToggleTarget: [target] [on|off]
        if (arg <= 1) con_words(c, prefix, ON_OFF);
    } else if (cmd_is(cmd, WORDS("borders", "forcecol"))) {
        if (arg == 0) con_words(c, prefix, ON_OFF);
    } else if (!strcmp(cmd, "bhop")) {
        if (arg == 0) con_words(c, prefix, WORDS("on", "off", "held"));
    } else if (!strcmp(cmd, "kill")) {
        if (arg == 0) {
            con_player_candidates(c, prefix);
            const Client *client = con_client(); // the active AIs (collectKillTargets also matches them)
            for (int i = 0; client && client->game.ai && i < client->game.ai->count; i++) {
                if (client->game.ai->ais[i].active) cand_add(c, client->game.ai->ais[i].name, prefix);
            }
        }
    } else if (cmd_is(cmd, WORDS("bot_kick", "bot_kill"))) {
        const Client *client = con_client(); // the bots of the in-process game (servers: names from the scoreboard)
        if (arg == 0) cand_add(c, "all", prefix);
        for (size_t i = 0; client && arg == 0 && i < client->game.player_count; i++) {
            const Player *p = client->game.players[i];
            if (p && p->is_bot) cand_add(c, p->name, prefix);
        }
    } else if (cmd_is(cmd, WORDS("tp", "bring"))) {
        if (arg == 0 && !strcmp(cmd, "bring")) cand_add(c, "all", prefix);
        if (arg == 0) con_player_candidates(c, prefix); // bots included: they are players
    } else if (!strcmp(cmd, "give")) { // the Object Manager's weapons (the Tool Gun too) and items
        for (int w = 0; arg == 0 && w < WEAPON_TABLE_COUNT; w++) cand_add(c, object_manager_give_name(OM_GIVE_WEAPON, w), prefix);
        int count = 0;
        object_manager_entries(OM_TAB_ENTITIES, &count);
        for (int e = 0; arg == 0 && e < count; e++) cand_add(c, object_manager_give_name(OM_GIVE_ENTITY, e), prefix);
        if (arg == 1) con_player_candidates(c, prefix);
    } else if (!strcmp(cmd, "bot_type")) {
        if (arg == 0) con_words(c, prefix, WORDS("nextbot", "tfue"));
    } else if (!strcmp(cmd, "show_navmeshes")) {
        if (arg == 0) con_words(c, prefix, ON_OFF);
    } else if (cmd_is(cmd, WORDS("bot_mimic", "bot_stop"))) {
        if (arg == 0) con_words(c, prefix, ON_OFF);
    } else if (!strcmp(cmd, "bot_difficulty")) {
        if (arg == 0) con_words(c, prefix, WORDS("easy", "normal", "hard", "max"));
    } else if (cmd_is(cmd, WORDS("bot", "bot_add", "bot_place", "bot_config"))) {
        con_words(c, prefix, WORDS("tfue", "autoRespawn", "difficulty=", "accuracy=", "name=", "class=", "team=", "level=", "clan=",
                                   "dmgMlt=", "targetBots=", "randomCosmetics=", "persistent="));
    } else if (!strcmp(cmd, "summon")) {
        int count = 0;
        const GameAITemplate *templates = game_ai_templates(&count);
        for (int i = 0; arg == 0 && i < count; i++) cand_add(c, templates[i].name, prefix);
    } else if (cmd_is(cmd, WORDS("ping", "kick", "ban", "unban", "ip", "banip"))) {
        if (arg == 0) con_player_candidates(c, prefix);
    } else if (cmd_is(cmd, WORDS("map", "offline"))) {
        if (arg == 0) con_map_candidates(c, prefix);
    } else if (!strcmp(cmd, "mode")) {
        if (arg == 0) con_mode_candidates(c, prefix);
    } else if (!strcmp(cmd, "team")) {
        if (arg == 0) con_words(c, prefix, WORDS("red", "blue"));
    } else if (!strcmp(cmd, "connect")) {
        const Client *client = con_client();
        if (arg == 0) {
            cand_add(c, "local", prefix);
            if (client) {
                char addr[96];
                snprintf(addr, sizeof(addr), "%s:%u", client->net_address, (unsigned int) client->net_port);
                cand_add(c, addr, prefix);
            }
        }
    } else if (!strcmp(cmd, "score")) {
        if (arg == 0) con_words(c, prefix, WORDS("100", "250", "500", "1000", "-100"));
    } else if (!strcmp(cmd, "class")) {
        if (arg == 0) con_class_candidates(c, prefix);
        else if (arg == 1) con_secondary_candidates(c, prefix);
    }
}

// The candidates for the token under the caret, and the line up to it.
static void line_candidates(const char *v, ConCands *c, char *base, const size_t n) {
    LineCtx ctx;
    parse_line(v, &ctx);
    c->count = 0;
    snprintf(base, n, "%.*s", (int) ctx.token_start, v);
    if (ctx.is_command) con_command_candidates(c, ctx.prefix);
    else arg_candidates(ctx.cmd, ctx.arg, ctx.prefix, c);
}

static void quote_into(char *out, const size_t n, const char *base, const char *value, const int space_after) {
    const int quote = strchr(value, ' ') != NULL;
    snprintf(out, n, "%s%s%s%s%s", base, quote ? "\"" : "", value, quote ? "\"" : "", space_after ? " " : "");
}

// updateHints: the command list for the first word (devConsole), else the command's
// signature row followed by the argument candidates; while Tab cycles, its pick is active.
static void update_hints(void) {
    if (!con_live()) return;
    char v[CON_LINE_MAX] = "";
    html_dom_get_value(g_con.input, v, sizeof(v));
    snprintf(g_con.last_value, sizeof(g_con.last_value), "%s", v);
    if (g_con.cycle_on && strcmp(v, g_con.cycle_value)) g_con.cycle_on = 0; // typing resets the cycle
    clear_hints();
    if (!v[0]) return;

    static ConCands cands;
    char base[CON_LINE_MAX];
    int active = -1;
    if (g_con.cycle_on) {
        cands = g_con.cycle;
        snprintf(base, sizeof(base), "%s", g_con.cycle_base);
        active = g_con.cycle_index;
    } else {
        line_candidates(v, &cands, base, sizeof(base));
    }
    LineCtx ctx;
    parse_line(v, &ctx);

    if (ctx.is_command) {
        if (!cands.count) return;
        html_dom_set_class(g_con.hints, "isOpen", 1);
        if (active < 0) {
            active = 0;
            for (int i = 0; i < cands.count; i++) if (!strcmp(cands.items[i], ctx.prefix)) { active = i; break; }
        }
    } else {
        html_dom_set_class(g_con.hints, "isOpen", 1);
        const ConCommand *c = find_cmd(ctx.cmd);
        if (c && !cmd_visible(c)) c = NULL; // a dev command with the dev console off is unknown
        if (c) {
            char args[256], meta[CON_LINE_MAX];
            fmt_args(c, args, sizeof(args));
            snprintf(meta, sizeof(meta), "%s  \xE2\x80\x94  %s", args, c->desc);
            add_hint_item(ctx.cmd, meta, active < 0, NULL);
        } else {
            add_hint_item("(unknown command)", "will not be sent", 1, NULL);
            return;
        }
    }
    // a window of the list that keeps the active candidate visible
    const int room = ctx.is_command ? CON_HINTS_MAX : CON_HINTS_MAX - 1;
    int first = active >= room ? active - room + 1 : 0;
    if (first + room > cands.count) first = cands.count - room > 0 ? cands.count - room : 0;
    for (int i = first; i < cands.count && i < first + room; i++) {
        char pick[CON_LINE_MAX];
        quote_into(pick, sizeof(pick), base, cands.items[i], 1);
        add_hint_item(cands.items[i], NULL, i == active, pick);
    }
}

// Tab / Shift+Tab: one candidate completes (plus a space); several cycle, starting at the
// first (Tab) or last (Shift+Tab). devConsole.ts only completed the command name, to the
// longest common prefix.
static void tab_complete(const int dir) {
    if (!con_live()) return;
    char v[CON_LINE_MAX] = "";
    html_dom_get_value(g_con.input, v, sizeof(v));
    if (g_con.cycle_on && !strcmp(v, g_con.cycle_value) && g_con.cycle.count) {
        g_con.cycle_index = (g_con.cycle_index + dir + g_con.cycle.count) % g_con.cycle.count;
    } else {
        line_candidates(v, &g_con.cycle, g_con.cycle_base, sizeof(g_con.cycle_base));
        g_con.cycle_on = 0;
        if (!g_con.cycle.count) {
            update_hints();
            return;
        }
        if (g_con.cycle.count == 1) {
            char line[CON_LINE_MAX];
            quote_into(line, sizeof(line), g_con.cycle_base, g_con.cycle.items[0], 1);
            set_input(line);
            update_hints();
            return;
        }
        g_con.cycle_on = 1;
        g_con.cycle_index = dir > 0 ? 0 : g_con.cycle.count - 1;
    }
    char line[CON_LINE_MAX];
    quote_into(line, sizeof(line), g_con.cycle_base, g_con.cycle.items[g_con.cycle_index], 0);
    set_input(line);
    snprintf(g_con.cycle_value, sizeof(g_con.cycle_value), "%s", line);
    update_hints();
}

// ---- arrow-key hint selection (KrunkNative extension: devConsole.ts has none; there
// Up/Down only walk history). Up/Down move through the selectable hint rows, wrapping;
// Tab/Enter accept the selected row. With no selectable rows, or while browsing history,
// Up/Down stay history.

static int hint_sel_valid(void) {
    if (g_con.hint_sel < 0 || g_con.hint_sel >= g_con.hint_count || !g_con.hint_lines[g_con.hint_sel][0]) return 0;
    char v[CON_LINE_MAX] = "";
    html_dom_get_value(g_con.input, v, sizeof(v));
    return !strcmp(v, g_con.hint_sel_value);
}

// Repaint .isActive onto the selected row (overrides update_hints' default highlight).
static void hint_sel_paint(void) {
    HtmlElem items[CON_HINTS_MAX];
    const int n = html_dom_query_all(g_con.hints, ".hintItem", items, CON_HINTS_MAX);
    for (int i = 0; i < n; i++) {
        html_dom_set_class(items[i], "isActive", i == g_con.hint_sel);
        html_dom_set_class(items[i], "isSelected", i == g_con.hint_sel);
    }
}

// Returns 0 when there is nothing to select (the caller falls back to history).
static int hint_sel_move(const int dir) {
    if (!con_live() || !html_dom_has_class(g_con.hints, "isOpen")) return 0;
    if (g_con.history_idx != -1) { // still showing a recalled history line: keep walking history
        char v[CON_LINE_MAX] = "";
        html_dom_get_value(g_con.input, v, sizeof(v));
        if (!strcmp(v, g_con.history[g_con.history_idx])) return 0;
    }
    int any = 0;
    for (int i = 0; i < g_con.hint_count; i++) any |= g_con.hint_lines[i][0] != 0;
    if (!any) return 0;
    int i = hint_sel_valid() ? g_con.hint_sel : (dir > 0 ? -1 : g_con.hint_count);
    do {
        i = (i + dir + g_con.hint_count) % g_con.hint_count; // wrap-around
    } while (!g_con.hint_lines[i][0]);
    g_con.hint_sel = i;
    html_dom_get_value(g_con.input, g_con.hint_sel_value, sizeof(g_con.hint_sel_value));
    hint_sel_paint();
    return 1;
}

// Tab / Enter on a selected row: the row's line goes in the input (like a click on it).
static int hint_sel_accept(void) {
    if (!con_live() || !hint_sel_valid()) return 0;
    char line[CON_LINE_MAX];
    snprintf(line, sizeof(line), "%s", g_con.hint_lines[g_con.hint_sel]);
    g_con.hint_sel = -1;
    g_con.cycle_on = 0;
    set_input(line);
    update_hints();
    return 1;
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
    if (!con_live()) return;
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

// The console's own field or log has the focus (a hidden one would keep taking the keys)
static int con_has_focus(void) {
    const HtmlElem active = con_live() ? html_dom_active_element() : 0;
    return active && (active == g_con.input || active == g_con.logs);
}

// The game page (index.html) runs the menu and the game; the other pages (editor, hub, viewer:
// app_mode.h) only show the console.
static int con_game_page(void) {
    return app_mode() == APP_MODE_GAME;
}

// The console is actually on screen (ui_console_shown): visible intent and a live overlay in the
// ready document shown now. update() sets the overlay's display to "block" whenever this holds, so
// it is the one truth both the pointer lock (menu_ui.c) and the probe agree on.
int ui_console_shown(void) {
    return g_con.visible && con_live();
}

// update(goToGame): show or hide the overlay and, on the game page, free or relock the pointer to
// match. The side effects that free the mouse (opening the menu for exitPointerLock) run only once
// the overlay is actually shown, so the mouse is never released for a console that is not on screen:
// if the page is not live yet the toggle is deferred here and ui_console_tick's build() runs update()
// again the moment the overlay is built, freeing the mouse and showing the console in the same frame.
static void update(const int go_to_game) {
    // Toggled visible while the overlay is stale (a page re-render replaced it) but the document is
    // ready: rebuild it now so it shows this frame instead of waiting for the next tick. build() ends
    // with its own update(0); the guard stops that from recursing back here.
    if (g_con.visible && !g_con.building && !con_live() && html_ui_available() && html_ui_is_ready()) {
        build();
        if (con_live()) return; // build()'s update(0) showed it and ran the side effects
    }
    const int live = con_live();
    if (live) html_dom_set_display(g_con.overlay, g_con.visible ? "block" : "none");
    if (g_con.visible) {
        g_con.focus_pending = 1;                            // setTimeout(() => input.focus(), 0)
        // Not on screen yet (the game page's document is still loading): keep the pointer and the
        // menu as they are and let the tick's build() run update() again once the overlay is live.
        if (!con_game_page() || !live) return;
        if (!html_ui_is_menu_open()) menu_request_open(1);  // document.exitPointerLock()
        ui_controls_reset_keys();
        const HtmlElem chat = html_dom_by_id("chatInput");
        if (chat) html_dom_blur(chat);
        ui_controls_chat_focus(0);
    } else {
        // KrunkNative: hidden always lets go of the focus (devConsole.ts blurs only on goToGame; a
        // browser drops the focus of a display:none field by itself). The close button kept it
        // natively: the hidden input went on taking the keys, the toggle key among them, so ` no
        // longer opened the console again.
        g_con.focus_pending = 0;
        if (con_has_focus()) html_dom_blur(html_dom_active_element());
        if (go_to_game && con_game_page() && html_ui_is_menu_open()) menu_play(); // CONTROLS.locklessChange(true) + requestPointerLock()
    }
}

static void set_visible(const int visible, const int go_to_game) {
    g_con.visible = !!visible;
    g_con.restored = 1;
    html_storage_set(CON_OPEN_KEY, g_con.visible ? "1" : NULL);
    if (g_con.visible) g_con.fit_vw = g_con.fit_vh = 0.0f; // shown: fitted again with its laid-out size
    update(go_to_game);
}

void ui_console_toggle(const int go_to_game) {
    set_visible(!g_con.visible, go_to_game);
}

// ---------------------------------------------------------------- keys

static int key_event(Client *client, const int key, const int scancode, const int action, const int mods, const int game) {
    if (client) g_con.client = client;
    if (!html_ui_available()) return 0;
    const int press = action == GLFW_PRESS;
    const int typing = ui_text_input_focused(); // a text field has focus (the console's own input too)
    // a '+name' bind's hold ends with its key's release, also while the console is open
    if (game && action == GLFW_RELEASE) hold_release(key);

    // handleConsoleKeydown (window capture)
    if (g_con.visible && (press || action == GLFW_REPEAT)) {
        if (key == GLFW_KEY_ESCAPE) {
            if (press) ui_console_toggle(1);
            return 1;
        }
        // the console's own input has focus (ui_text_input_focused + activeElement == input);
        // either way these keys are consumed below, so no game / editor bind sees them
        const int in_input = typing && html_dom_active_element() == g_con.input;
        if (key == GLFW_KEY_UP) {
            if (in_input && !hint_sel_move(-1)) history_up();
            return 1;
        }
        if (key == GLFW_KEY_DOWN) {
            if (in_input && !hint_sel_move(1)) history_down();
            return 1;
        }
        if (key == GLFW_KEY_TAB) {
            if (in_input && !hint_sel_accept()) tab_complete((mods & GLFW_MOD_SHIFT) ? -1 : 1); // Shift+Tab cycles backwards
            return 1;
        }
    }
    // mods.ts document keydown: toggle key, F8 cycle, bhop key (another page: the toggle key only)
    if (game ? ui_mods_key_event(client, key, action, mods, typing) : ui_mods_toggle_key_event(key, action, typing)) return 1;

    if (g_con.visible) {
        if (press && (key == GLFW_KEY_ENTER || key == GLFW_KEY_KP_ENTER)) { // form submit
            if (!hint_sel_accept()) submit(); // a selected hint is accepted first, Enter again runs it
            return 1;
        }
        html_ui_key(key, scancode, action, mods); // the input line edits itself
        return 1;                                 // overlay keydown/keyup stopPropagation
    }
    // Key bind dispatch: keyBinds[e.key] || keyBinds[e.code], not while typing
    if (game && press && !typing && g_con.bind_count) {
        const ConPair *bind = pair_find(g_con.binds, g_con.bind_count, ui_mods_key_value(key, mods));
        if (!bind) bind = pair_find(g_con.binds, g_con.bind_count, ui_mods_key_code(key));
        if (bind) {
            char cmd[CON_TEXT_MAX];
            snprintf(cmd, sizeof(cmd), "%s", bind->value);
            trim(cmd);
            con_exec(cmd);
            if (cmd[0] == '+') {
                char name[CON_NAME_MAX];
                hold_name(cmd, name, sizeof(name));
                hold_add(key, name);
            }
        }
    }
    return 0;
}

int ui_console_key_event(Client *client, const int key, const int scancode, const int action, const int mods) {
    return key_event(client, key, scancode, action, mods, 1);
}

int ui_console_app_page_key_event(Client *client, const int key, const int scancode, const int action, const int mods) {
    if (!con_live()) return 0;
    return key_event(client, key, scancode, action, mods, 0);
}

int ui_console_page_key(const HtmlEvent *ev) {
    if (!con_live() || !ev || !ev->target || ev->target != g_con.input || !g_con.visible) return 0;
    if (html_key_is_escape(ev->key)) {
        ui_console_toggle(1);
        return 1;
    }
    if (html_key_is_enter(ev->key)) {
        if (!hint_sel_accept()) submit();
        return 1;
    }
#ifdef __EMSCRIPTEN__
    // DOM keyCodes (kn-shim forwards these for data-kn-keys fields)
    if (ev->key == 38) {
        if (!hint_sel_move(-1)) history_up();
        return 1;
    }
    if (ev->key == 40) {
        if (!hint_sel_move(1)) history_down();
        return 1;
    }
    if (ev->key == 9) {
        if (!hint_sel_accept()) tab_complete(ev->x != 0.0f ? -1 : 1); // kn-shim: x = 1 when Shift is held on key events
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

// KrunkNative: devConsole.ts clampPos, for the size too, through the rule every draggable window
// follows (win_geom.h). Saved geometry from a bigger monitor or window (a resolution change, F11,
// an F4 / F5 restart) used to stay where it was: clampPos measured the box while the overlay was
// display:none (offsetWidth 0), so a saved left of 5000 only came back to the viewport's right
// edge, fully outside, and nothing clamped again on a resize. Now the box is fitted on load, on
// every viewport change and when the console is shown; the whole box stays inside, shrunk to
// the viewport when it is larger (never below the stylesheet's minimum).
static const WinGeomRules CON_RULES = {CON_MIN_W, CON_MIN_H, 0.0f, 0.0f};

// The console's outer box: left / top from the inline style, the size from the layout while the
// overlay is shown, else the inline size, else the stylesheet's default for this viewport.
static void con_geom_read(WinGeom *g, const float vw, const float vh) {
    char buf[32];
    const float ow = html_dom_offset_width(g_con.modal), oh = html_dom_offset_height(g_con.modal);
    g->x = (float) style_px(g_con.modal, "left");
    g->y = (float) style_px(g_con.modal, "top");
    if (ow > 0.0f) g->w = ow;
    else if (html_dom_get_style(g_con.modal, "width", buf, sizeof(buf)) >= 0) g->w = (float) atoi(buf) + CON_PAD;
    else g->w = fminf(CON_DEF_W + CON_PAD, vw - 2.0f * CON_DEF_MARGIN);
    if (oh > 0.0f) g->h = oh;
    else if (html_dom_get_style(g_con.modal, "height", buf, sizeof(buf)) >= 0) g->h = (float) atoi(buf) + CON_PAD;
    else g->h = fminf(CON_DEF_H + CON_PAD, vh - 2.0f * CON_DEF_MARGIN);
}

// Writes the box back; the size only when it differs from `was` (NULL: always).
static void con_geom_write(const WinGeom *g, const WinGeom *was) {
    set_px(g_con.modal, "left", g->x);
    set_px(g_con.modal, "top", g->y);
    if (!was || fabsf(g->w - was->w) >= 0.5f) set_px(g_con.modal, "width", g->w - CON_PAD);
    if (!was || fabsf(g->h - was->h) >= 0.5f) set_px(g_con.modal, "height", g->h - CON_PAD);
}

// Fits a placed box (data-pos-committed) into the viewport; the CSS-anchored default always
// fits. Returns 1 when the box moved or shrank.
static int con_fit(void) {
    if (!con_live() || !pos_committed()) return 0;
    float vw, vh, dpr;
    html_dom_viewport(&vw, &vh, &dpr);
    if (vw <= 0.0f || vh <= 0.0f) return 0;
    g_con.fit_vw = vw;
    g_con.fit_vh = vh;
    WinGeom g;
    con_geom_read(&g, vw, vh);
    const WinGeom was = g;
    if (!win_geom_fit(&g, vw, vh, &CON_RULES)) return 0;
    con_geom_write(&g, &was);
    return 1;
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
    if (!con_live() || !pos_committed()) return;
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

// restoreGeom; the saved box is fitted to this viewport (con_fit) before it is used, and a
// saved size alone is placed where the stylesheet anchors the console (right / top 8px).
static void restore_geom(void) {
    char buf[256];
    if (html_storage_get(CON_GEOM_KEY, buf, sizeof(buf)) < 0) return;
    cJSON *g = cJSON_Parse(buf);
    if (!cJSON_IsObject(g)) {
        cJSON_Delete(g);
        return;
    }
    float vw, vh, dpr;
    html_dom_viewport(&vw, &vh, &dpr);
    const cJSON *w = cJSON_GetObjectItem(g, "width"), *h = cJSON_GetObjectItem(g, "height");
    const cJSON *l = cJSON_GetObjectItem(g, "left"), *t = cJSON_GetObjectItem(g, "top");
    WinGeom box = {0.0f, 0.0f, fminf(CON_DEF_W + CON_PAD, vw - 2.0f * CON_DEF_MARGIN), fminf(CON_DEF_H + CON_PAD, vh - 2.0f * CON_DEF_MARGIN)};
    const int sized_w = cJSON_IsNumber(w) && isfinite(w->valuedouble) && w->valuedouble >= 280;
    const int sized_h = cJSON_IsNumber(h) && isfinite(h->valuedouble) && h->valuedouble >= 160;
    const int placed = cJSON_IsNumber(l) && cJSON_IsNumber(t) && isfinite(l->valuedouble) && isfinite(t->valuedouble);
    if (sized_w) box.w = (float) w->valuedouble; // saved as offsetWidth: the outer box
    if (sized_h) box.h = (float) h->valuedouble;
    cJSON_Delete(g);
    if (!sized_w && !sized_h && !placed) return;
    if (placed) {
        box.x = (float) l->valuedouble;
        box.y = (float) t->valuedouble;
    } else {
        box.x = vw - CON_DEF_MARGIN - box.w;
        box.y = CON_DEF_MARGIN;
    }
    if (vw > 0.0f && vh > 0.0f) {
        win_geom_fit(&box, vw, vh, &CON_RULES);
        g_con.fit_vw = vw;
        g_con.fit_vh = vh;
    }
    html_dom_set_style(g_con.modal, "transform", "none");
    con_geom_write(&box, NULL);
    html_dom_set_attr(g_con.modal, "data-pos-committed", "1");
}

// console_reset (KrunkNative): the stylesheet's place and size again, the saved geometry gone
static void reset_geom(void) {
    if (g_con.save_timer) html_timer_cancel(g_con.save_timer);
    g_con.save_timer = 0;
    g_con.dragging = g_con.resizing = 0;
    html_storage_set(CON_GEOM_KEY, NULL);
    if (!con_live()) return;
    static const char *const PROPS[] = {"left", "top", "width", "height", "transform"};
    for (size_t i = 0; i < sizeof(PROPS) / sizeof(PROPS[0]); i++) html_dom_set_style(g_con.modal, PROPS[i], "");
    html_dom_remove_attr(g_con.modal, "data-pos-committed");
}

static int pick_hint(const float x, const float y) {
    HtmlElem items[CON_HINTS_MAX];
    const int n = html_dom_query_all(g_con.hints, ".hintItem", items, CON_HINTS_MAX);
    for (int i = 0; i < n && i < g_con.hint_count; i++) {
        if (!hit(items[i], x, y)) continue;
        if (g_con.hint_lines[i][0]) { // addHintItem onPick: input.value = name + " " (or the argument)
            g_con.cycle_on = 0;
            set_input(g_con.hint_lines[i]);
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
            con_fit(); // clampPos
            return 1;
        }
        if (g_con.resizing) {
            // outer size (start_w / start_h are offsetWidth / offsetHeight); the grip stops at the
            // viewport's right / bottom edge and at the stylesheet's minimum (280 x 160)
            float vw, vh, dpr;
            html_dom_viewport(&vw, &vh, &dpr);
            WinGeom g = {(float) style_px(g_con.modal, "left"), (float) style_px(g_con.modal, "top"),
                         g_con.start_w + (fx - g_con.start_x), g_con.start_h + (fy - g_con.start_y)};
            win_geom_fit_resize(&g, vw, vh, &CON_RULES);
            con_geom_write(&g, NULL);
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
    if (!con_live() || !g_con.visible) return 0;
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

// ---------------------------------------------------------------- tests

// input_script `conin`: the console's input line and how many hint rows (the tab completion) are showing.
void ui_console_input_probe(char *out, const size_t n) {
    char value[CON_LINE_MAX] = "";
    const int live = con_live();
    if (live) html_dom_get_value(g_con.input, value, sizeof(value));
    snprintf(out, n, "value=[%s] hints=%d", value, live ? g_con.hint_count : 0);
}

void ui_console_probe(char *out, const size_t n) {
    float vw = 0.0f, vh = 0.0f, dpr = 1.0f;
    const int html = html_ui_available() && html_ui_is_ready();
    if (html) html_dom_viewport(&vw, &vh, &dpr);
    const HtmlElem overlay = html ? html_dom_by_id("devConsoleOverlay") : 0;
    const HtmlElem modal = overlay ? html_dom_query(overlay, ".modal") : 0;
    HtmlRect r = {0}, o = {0};
    const int boxed = modal && html_dom_get_rect(modal, &r);
    const int sheet = g_con.sheet && overlay && html_dom_get_rect(overlay, &o) && fabsf(o.x) < 0.5f && fabsf(o.y) < 0.5f &&
                      fabsf(o.w - vw) < 0.5f && fabsf(o.h - vh) < 0.5f;
    const int shown = modal && html_dom_is_shown(modal);
    int top = 0;
    if (boxed && shown) {
        const float px[5] = {0.5f, 0.1f, 0.9f, 0.1f, 0.9f}, py[5] = {0.5f, 0.1f, 0.1f, 0.9f, 0.9f};
        for (int i = 0; i < 5; i++) {
            const HtmlElem at = html_dom_painted_at(r.x + r.w * px[i], r.y + r.h * py[i]); // pointer-events: none too
            top += at && html_dom_closest(at, "#devConsoleOverlay") == overlay;
        }
    }
    int last = 0;
    if (overlay && html_dom_parent(overlay) == html_dom_body()) {
        last = 1;
        for (HtmlElem e = html_dom_next(overlay); e; e = html_dom_next(e)) {
            char tag[16] = "";
            html_dom_tag(e, tag, sizeof(tag));
            if (strcmp(tag, "script") && strcmp(tag, "style") && strcmp(tag, "link")) last = 0; // rendered elements only
        }
    }
    const int inside = boxed && r.x >= -0.5f && r.y >= -0.5f && r.x + r.w <= vw + 0.5f && r.y + r.h <= vh + 0.5f;
    snprintf(out, n, "open=%d built=%d page=%s shown=%d sheet=%d last=%d top=%d inside=%d rect=%.1f,%.1f,%.1f,%.1f vp=%.0fx%.0f",
             g_con.visible, g_con.built, html ? html_ui_current_page() : "-", shown, sheet, last, top, inside, r.x, r.y, r.w,
             r.h, vw, vh);
}

// ---------------------------------------------------------------- per frame

// The overlay of this document: devConsole.ts' DOM appended to <body>, or the one already there
// (the game page's document is kept while the editor / hub page is shown, html_ui_load_page).
static void build(void) {
    const HtmlElem body = html_dom_body();
    if (!body) return;
    g_con.building = 1; // update() (called at the end) must not recurse back into build()
    // the sheet per document: the game page keeps its sheets, a page switch starts without it
    g_con.sheet = html_ui_add_stylesheet("css/devconsole.css");
    if (!g_con.sheet) {
        static unsigned warned;
        if (warned != html_ui_page_serial()) fprintf(stderr, "[console] css/devconsole.css not added (%s): inline styles only\n", html_ui_current_page());
        warned = html_ui_page_serial();
    }
    g_con.overlay = html_dom_by_id("devConsoleOverlay");
    if (!g_con.overlay) {
        html_dom_insert_html(body, HTML_INSERT_BEFORE_END, OVERLAY_HTML);
        g_con.overlay = html_dom_by_id("devConsoleOverlay");
    }
    if (!g_con.overlay) { g_con.building = 0; return; }
    g_con.modal = html_dom_query(g_con.overlay, ".modal");
    g_con.header = html_dom_query(g_con.overlay, ".titlebar");
    g_con.close_btn = html_dom_query(g_con.overlay, ".titlebar .x");
    g_con.logs = html_dom_query(g_con.overlay, ".logs");
    g_con.hints = html_dom_query(g_con.overlay, ".hints");
    g_con.input = html_dom_query(g_con.overlay, ".row input");
    g_con.submit = html_dom_query(g_con.overlay, ".row button");
    g_con.grip = html_dom_query(g_con.overlay, ".grip");
    if (!g_con.modal || !g_con.logs || !g_con.input) { g_con.building = 0; return; }
    g_con.built = 1;
    g_con.serial = html_ui_page_serial();
    g_con.dragging = g_con.resizing = 0;
    g_con.pressed = 0;
    g_con.fit_vw = g_con.fit_vh = 0.0f;
    render_lines();
    restore_geom();
    html_dom_set_top_layer(g_con.overlay); // natively also above the GL drawn over the page
    html_ui_set_listener(NULL, "knpointerdown", l_pointer);
    html_ui_set_listener(NULL, "knpointermove", l_pointer);
    html_ui_set_listener(NULL, "knpointerup", l_pointer);
    if (!g_con.restored) { // the open state of the last run (CON_OPEN_KEY)
        char open[4] = "";
        g_con.restored = 1;
        if (html_storage_get(CON_OPEN_KEY, open, sizeof(open)) >= 0 && !strcmp(open, "1") && ui_mods_dev_console_enabled()) {
            g_con.visible = 1;
        }
    }
    update(0);          // the overlay is live now, so this shows it (and frees the pointer) at once
    g_con.building = 0; // guard held across update(0) so its build-on-demand cannot recurse
}

// KrunkNative: the console stays above the whole page. devconsole.css gives it z-index 999999,
// which v10's page beats (the view stack 2147483646, #popupHolder, #windowHolder.popupWin, the
// loader, a custom.css): the overlay has the highest z-index and is kept <body>'s last element,
// so for the same z-index it paints last too, whatever the page appends later (v10's login view
// stack, notifications, KrunkScript DIVs ...).
static void keep_on_top(void) {
    const HtmlElem body = html_dom_body();
    for (HtmlElem e = html_dom_next(g_con.overlay); e; e = html_dom_next(e)) {
        char tag[16] = "";
        html_dom_tag(e, tag, sizeof(tag));
        if (!strcmp(tag, "script") || !strcmp(tag, "style") || !strcmp(tag, "link")) continue;
        const int focused = con_has_focus();
        html_dom_append(body, g_con.overlay); // moved, not copied: the same elements and state
        if (focused && g_con.visible) html_dom_focus(g_con.input);
        return;
    }
    if (body && html_dom_parent(g_con.overlay) != body) html_dom_append(body, g_con.overlay);
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
    // the inserted text (InputEvent.data): what differs between the old and the new value. Only a
    // single typed toggle character closes the console (devConsole.ts tests e.data); anything
    // else with one in it (a paste, a recalled line) stays text (it used to close the console).
    const size_t len = strlen(value), was = strlen(g_con.last_value);
    size_t pre = 0;
    while (pre < was && pre < len && value[pre] == g_con.last_value[pre]) pre++;
    size_t post = 0;
    while (post < was - pre && post < len - pre && value[len - 1 - post] == g_con.last_value[was - 1 - post]) post++;
    if (len == was + 1 && pre + post == was && strchr(toggles, value[pre])) {
        memmove(value + pre, value + pre + 1, len - pre);
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
    { // game transport notes (net_notes.c): which transport, why one failed, the fallback taken
        char note[320];
        int level = NET_NOTE_INFO;
        while (client_net_note_next(&level, note, sizeof(note))) {
            appendf(level == NET_NOTE_ERROR ? COL_ERROR : level == NET_NOTE_WARN ? COL_WARN : COL_INFO, "[net] %s", note);
        }
    }
    // every page (the game page, and the editor / hub / viewer pages: app_mode_frame), in every state
    if (!html_ui_available() || !html_ui_is_ready()) return; // a document is loading: kept as it is
    if (!g_con.built || g_con.serial != html_ui_page_serial() || html_dom_by_id("devConsoleOverlay") != g_con.overlay ||
        html_dom_query(g_con.overlay, ".row input") != g_con.input) {
        g_con.built = 0; // another document, or the page replaced the overlay
        build();
        if (!g_con.built) return;
    }
    keep_on_top();
    if (con_game_page()) ui_mods_tick(client);
    if (!g_con.visible && con_has_focus()) html_dom_blur(html_dom_active_element()); // a page focused the hidden field
    { // KrunkNative: the window changed size (a resize, F11, another monitor) or the console was shown
        float vw, vh, dpr;
        html_dom_viewport(&vw, &vh, &dpr);
        if (vw > 0.0f && vh > 0.0f && (vw != g_con.fit_vw || vh != g_con.fit_vh) && !g_con.dragging && !g_con.resizing) {
            g_con.fit_vw = vw;
            g_con.fit_vh = vh;
            con_fit();
        }
    }
    if (!g_con.visible) return;
    if (g_con.focus_pending) {
        g_con.focus_pending = 0;
        html_dom_focus(g_con.input);
        snprintf(g_con.last_value, sizeof(g_con.last_value), "%s", "");
        html_dom_get_value(g_con.input, g_con.last_value, sizeof(g_con.last_value));
    }
    poll_input();
}
