// The Host Game window (client/include/host_flow_ui.h): v10's host_games window (windows[7] of v10.0.2,
// the captured ui/windows/8.html markup): the Select, Game Settings, Advanced and Server tabs, then the
// last step, Start Game, which is the choice of where the game runs. Every Host Game entry point opens
// this one window (MENU_WINDOW_HOST, openHostWindow(false, type)). Its tabs are v10's (windows[7].switchTab,
// on the page only), so every value stays in the page until a step reads it back (sync_page). The state
// (g_flow) keeps them across a trip to the Dedicated Server window and back (knHostFlow 'reopen').
//
// The last step offers, by the active backend (krunker_net_active(): the official krunker.io connection):
//   krunker.io  -> krunker.io's official hosting: v10's CUSTOM_SERVER request (krunker_net_host_custom)
//   otherwise   -> Box Office servers: the brain hosts the game (/mm/host-game, servers_ui_host_game)
//   always      -> host it yourself: the server_bin command and the Dedicated Server window
#include <host_flow_ui.h>
#include <host_command.h>
#include <dedicated_server_ui.h> // "Get the dedicated server": the Dedicated Server window
#include <client.h>
#include <community_host.h>      // the community maps' seam (community_maps.c): published maps, the catalogue
#include <game_config.h>
#include <html_dom.h>
#include <html_ui.h>
#include <i18n.h>
#include <krunker_net.h>         // the krunker.io branch: the official connection and its hosting
#include <backend_ui.h>       // servers_ui_host_game: Start Game on Box Office servers
#include <map_catalog.h>
#include <shared.h>
#include <ui_host_config.h>
#include <ui_logic.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FLOW_STEP_COUNT 5       // 0 Select, 1 Game Settings, 2 Advanced, 3 Server, 4 Start Game (the choice)
#define FLOW_STEP_START 4
#define FLOW_DEFAULT_PORT 21015 // NET_DEFAULT_PORT
#define FLOW_MAP_MAX MAP_CATALOG_OFFICIAL_COUNT

// The flow's state. The page holds the values while the window is open; sync_page() reads them back.
static struct {
    int host_type;                         // config.hostTypes index (GAME_HOST_*)
    int step;                              // the tab shown (FLOW_STEP_*)
    unsigned char maps[FLOW_MAP_MAX];      // gameMap<mapIndex> checked (Krunker's own maps)
    char custom_key[256];                  // a community map chosen in the Games window: its key or published spec ("" none)
    char custom_name[96];                  // its name (the Select banner)
    cJSON *request;                        // the rows as last read back (ui_host_config_collect); NULL until read
    char name[64];                         // --hostname (the self-host fields)
    int port;                              // --port (0: the default)
    char password[64];                     // --password (Server tab's Server Password)
    char public_host[128];                 // --public-host
    int public_port;                       // --public-port (0: none)
    int headless;                          // --headless
    char message[192];                     // #hostGameMsg's text when the window is generated
} g_flow;

// Krunker's own map (the Select tab's checkboxes): official and hostable in the catalogue
static int official_hostable(const MapCatalogEntry *e) {
    return e && e->source == MAP_SOURCE_OFFICIAL && e->hostable && e->index >= 0 && e->index < FLOW_MAP_MAX;
}

// The last step's backend: krunker.io's hosting while the official connection is active (krunker_net.h)
static int krunker_backend(void) { return krunker_net_active(); }

// ------------------------------------------------------------------ the page's values

static int page_open(void) { return html_dom_by_id("hstTab0") != 0; }

// A page field's value into `out`; the field may be absent (then `out` is kept): 1 when read
static int read_field(const char *id, char *out, const size_t size) {
    const HtmlElem el = html_dom_by_id(id);
    if (!el) return 0;
    out[0] = 0;
    html_dom_get_value(el, out, size);
    return 1;
}

static int read_int(const char *id, const int fallback) {
    char text[32];
    if (!read_field(id, text, sizeof(text)) || !text[0]) return fallback;
    char *end = NULL;
    const long v = strtol(text, &end, 10);
    return end && !*end && v >= 0 && v <= 65535 ? (int) v : fallback;
}

// The page's values into the state (the window open only): the Select tab's checkboxes, the self-host
// fields, the password and the rows (ui_host_config_collect). Returns 0 with `err` set when the rows are
// not valid (no mode, no class, a long password); the state then keeps the rows it had.
static int sync_page(char *err, const size_t err_size) {
    if (err_size) err[0] = 0;
    if (!page_open()) return 1;
    for (size_t i = 0; i < map_catalog_count(); i++) {
        const MapCatalogEntry *e = map_catalog_at(i);
        if (!official_hostable(e)) continue;
        char id[24];
        snprintf(id, sizeof(id), "gameMap%d", e->index);
        const HtmlElem box = html_dom_by_id(id);
        if (box) g_flow.maps[e->index] = html_dom_get_checked(box) ? 1 : 0;
    }
    read_field("hostFlowName", g_flow.name, sizeof(g_flow.name));
    read_field("passCode", g_flow.password, sizeof(g_flow.password));
    g_flow.port = read_int("hostFlowPort", FLOW_DEFAULT_PORT);
    read_field("hostFlowPublicHost", g_flow.public_host, sizeof(g_flow.public_host));
    g_flow.public_port = read_int("hostFlowPublicPort", 0);
    const HtmlElem headless = html_dom_by_id("hostFlowHeadless");
    if (headless) g_flow.headless = html_dom_get_checked(headless) ? 1 : 0;
    cJSON *request = ui_host_config_collect(g_flow.host_type, err, err_size);
    if (!request) return 0;
    cJSON_Delete(g_flow.request);
    g_flow.request = request;
    return 1;
}

// The rows' preset (ui_host_config_tab_html): the request's lists, and its server settings as "settings".
static cJSON *preset_of(const cJSON *request) {
    if (!request) return NULL;
    cJSON *preset = cJSON_CreateObject();
    const char *lists[] = {"modes", "classes", "weps", "cls", "prks"};
    for (size_t i = 0; i < sizeof(lists) / sizeof(lists[0]); i++) {
        const cJSON *item = cJSON_GetObjectItemCaseSensitive(request, lists[i]);
        if (item) cJSON_AddItemToObject(preset, lists[i], cJSON_Duplicate(item, 1));
    }
    cJSON *settings = cJSON_AddObjectToObject(preset, "settings");
    size_t count = 0;
    const GameConfigOption *options = game_config_options(GAME_CONFIG_SERVER, &count);
    for (size_t k = 0; k < count; k++) {
        const cJSON *item = cJSON_GetObjectItemCaseSensitive(request, options[k].var);
        if (item) cJSON_AddItemToObject(settings, options[k].var, cJSON_Duplicate(item, 1));
    }
    return preset;
}

// ------------------------------------------------------------------ the messages and the command

// #hostGameMsg (v10's own message line): an error in red, or plain text
static void set_message(const char *text, const int error) {
    const HtmlElem msg = html_dom_by_id("hostGameMsg");
    if (!msg) {
        snprintf(g_flow.message, sizeof(g_flow.message), "%s", text ? text : "");
        return;
    }
    StrBuf sb = {0};
    if (error) sb_append(&sb, "<span class='error'>");
    sb_append_esc(&sb, text ? text : "");
    if (error) sb_append(&sb, "</span>");
    char *html = sb_finish(&sb);
    html_dom_set_html(msg, html ? html : "");
    free(html);
}

// The checked official maps' keys, comma separated (gameserver.maps). A community map chosen in the
// Games window replaces them (v10's Change Game card: only the chosen game is hosted).
static void maps_text(char *out, const size_t size) {
    out[0] = 0;
    if (g_flow.custom_key[0]) {
        snprintf(out, size, "%s", g_flow.custom_key);
        return;
    }
    size_t n = 0;
    for (size_t i = 0; i < map_catalog_count(); i++) {
        const MapCatalogEntry *e = map_catalog_at(i);
        if (!official_hostable(e) || !g_flow.maps[e->index]) continue;
        n += (size_t) snprintf(out + n, size > n ? size - n : 0, "%s%s", n ? "," : "", e->key);
    }
}

// The server_bin line for the state (shared/include/host_command.h). Returns its length or < 0.
static int command_line(char *out, const size_t size) {
    char maps[1024];
    maps_text(maps, sizeof(maps));
    HostCommand spec = {0};
    spec.program = "server_bin";
    spec.map_key = maps[0] ? maps : NULL;
    spec.host_type = g_flow.host_type;
    spec.port = g_flow.port;
    spec.hostname = g_flow.name[0] ? g_flow.name : NULL;
    spec.password = g_flow.password[0] ? g_flow.password : NULL;
    spec.public_host = g_flow.public_host[0] ? g_flow.public_host : NULL;
    spec.public_port = g_flow.public_port;
    spec.headless = g_flow.headless;
    return host_command_build(&spec, g_flow.request, out, size);
}

// The Server tab's command box (#hostFlowCommand) shows the current values
static void refresh_command(void) {
    char err[160] = "";
    sync_page(err, sizeof(err));
    const HtmlElem box = html_dom_by_id("hostFlowCommand");
    if (!box) return;
    char line[HOST_COMMAND_WORD_MAX + 1024];
    if (command_line(line, sizeof(line)) >= 0) html_dom_set_text(box, line);
    else html_dom_set_text(box, I18N("kn.hostflow.command.error", "Too many settings for one command: remove some overrides."));
}

// The config of a hosting request: v10's createPrivateRoom object, built from the rows (the request), the
// checked maps (catalogue indices, as v10's maps; none with a community map) and the community map
// (customMap, v10's name field)
static cJSON *host_config(void) {
    cJSON *config = cJSON_Duplicate(g_flow.request, 1);
    if (!config) return NULL;
    cJSON *maps = cJSON_AddArrayToObject(config, "maps");
    for (size_t i = 0; i < map_catalog_count() && !g_flow.custom_key[0]; i++) {
        const MapCatalogEntry *e = map_catalog_at(i);
        if (official_hostable(e) && g_flow.maps[e->index]) cJSON_AddItemToArray(maps, cJSON_CreateNumber(e->index));
    }
    if (g_flow.custom_key[0]) cJSON_AddStringToObject(config, "customMap", g_flow.custom_key);
    cJSON_AddBoolToObject(config, "krRewards", 0);
    cJSON_AddBoolToObject(config, "defaultSetts", 0);
    cJSON_AddBoolToObject(config, "share", 0);
    return config;
}

static int any_map(void) {
    if (g_flow.custom_key[0]) return 1;
    for (size_t i = 0; i < FLOW_MAP_MAX; i++)
        if (g_flow.maps[i]) return 1;
    return 0;
}

// ------------------------------------------------------------------ Start Game (the choice's first option)

static void on_krunker_hosted(void *user, const char *game_id, const char *region, const char *error) {
    (void) user;
    if (!game_id) {
        set_message(error && *error ? error : I18N("custom.tryagain", "Please try again."), 1);
        return;
    }
    set_message(I18N("kn.host.created", "Game Created"), 0); // customRoomResponse(message, gameId)
    menu_show_window(MENU_WINDOW_NONE, 0);
    krunker_net_join(menu_ui()->client, game_id, region ? region : "");
}

// Start Game on the active backend: krunker.io's official hosting or Box Office's brain. The rows are
// checked first (v10's messages); a refused hosting says so in #hostGameMsg.
void host_flow_start(void) {
    char err[160] = "";
    if (!sync_page(err, sizeof(err)) || !g_flow.request) {
        set_message(err[0] ? err : I18N("kn.host.invalidSettings", "Invalid game settings"), 1);
        return;
    }
    if (!any_map()) {
        set_message(I18N("custom.map.missing", "Select at least one Game or Map"), 1);
        return;
    }
    cJSON *config = host_config();
    if (!config) {
        set_message(I18N("kn.host.invalidSettings", "Invalid game settings"), 1);
        return;
    }
    int sent = 0;
    if (krunker_backend()) {
        if (g_flow.custom_key[0]) { // a community map is Box Office's (kn_brain) and is not on krunker.io
            set_message(I18N("kn.hostflow.kio.community", "Community maps are hosted on Box Office servers or on your own server."), 1);
        } else {
            set_message(I18N("generic.wait", "Please wait..."), 0);
            sent = krunker_net_host_custom(config, on_krunker_hosted, NULL);
            if (!sent) set_message(I18N("kn.hostflow.kio.unavailable", "krunker.io servers are unavailable right now. Try again, or host it yourself."), 1);
        }
    } else {
        char *json = cJSON_PrintUnformatted(config);
        sent = json ? servers_ui_host_game(json) : 0; // "Creating Game..." until the brain answers
        free(json);
        if (!sent) set_message(I18N("kn.hostflow.box.unavailable", "Box Office servers are unavailable right now. Host it yourself with the dedicated server."), 1);
    }
    cJSON_Delete(config);
}

// ------------------------------------------------------------------ Play this map offline (KrunkNative only)

// The offline game's map: the Games window's catalogue map (the banner), else the first checked Krunker map.
// -1 when none is a catalogue map (a published map is practised through community_fetch, below).
static int practice_catalog_index(void) {
    if (g_flow.custom_key[0]) return map_catalog_index_of_key(g_flow.custom_key);
    for (size_t i = 0; i < map_catalog_count(); i++) {
        const MapCatalogEntry *e = map_catalog_at(i);
        if (official_hostable(e) && g_flow.maps[e->index]) return (int) i;
    }
    return -1;
}

// A published map's name from its spec ("published:<creator>/<name>"); 0 for other keys
static int published_name(const char *key, char *name, const size_t size) {
    if (!key || strncmp(key, "published:", 10)) return 0;
    const char *slash = strchr(key + 10, '/');
    snprintf(name, size, "%s", slash ? slash + 1 : key + 10);
    return name[0] != 0;
}

static int g_practice_mode = -1; // the Krunker mode of the first checked mode (-1 keeps the offline game's)

static void on_practice_slot(void *user, const int slot) {
    (void) user;
    Client *client = menu_ui()->client;
    if (slot < 0) return; // community_fetch's answer already said why
#ifdef LOCAL_GAME_HAVE_HOST_CONFIG
    if (g_flow.request) local_game_host_config(client, g_flow.request, g_flow.host_type);
#endif
    community_host_slot(client, slot, g_practice_mode); // the offline game starts on it (closes this window)
}

static void on_practice_fetched(void *user, const cJSON *json, const char *error) {
    char *name = user;
    if (!json) {
        set_message(error ? error : I18N("kn.games.loadFailed", "Failed to load the game."), 1);
    } else {
        const CommunityMapRef ref = {name, ""};
        community_pick(menu_ui()->client, &ref, on_practice_slot, NULL); // cached: at once
    }
    free(name);
}

// Play this map offline: the chosen map on the embedded offline game, with the first checked mode. A
// published map is downloaded and checked first (community_fetch, v10's checks).
static void practice(void) {
    const cJSON *modes = g_flow.request ? cJSON_GetObjectItemCaseSensitive(g_flow.request, "modes") : NULL;
    const cJSON *first = cJSON_GetArrayItem(modes, 0);
    const int mode = cJSON_IsNumber(first) ? first->valueint : -1;
    const int index = practice_catalog_index();
    char name[96];
    if (index < 0 && published_name(g_flow.custom_key, name, sizeof(name))) {
        g_practice_mode = mode;
        community_fetch(name, on_practice_fetched, strdup(name));
        return;
    }
    if (index < 0) {
        set_message(I18N("kn.hostflow.practice.none", "Pick a map of the catalogue to play it offline."), 1);
        return;
    }
#ifdef LOCAL_GAME_HAVE_HOST_CONFIG
    if (g_flow.request) local_game_host_config(menu_ui()->client, g_flow.request, g_flow.host_type); // the next round's game config
#endif
    host_catalog_map(index, mode); // the offline game starts on it (closes this window)
}

// ------------------------------------------------------------------ the markup

static void append_button(StrBuf *sb, const char *id, const char *label, const char *onclick, const int primary) {
    sb_appendf(sb, "<div %s%s%s class='%s' style='display:inline-block;margin:6px 8px 0 0;cursor:pointer;padding:8px 14px;border-radius:6px;background:%s;color:#fff' onmouseenter='playTick()' onclick=\"playSelect(0.1);%s\">",
               id ? "id='" : "", id ? id : "", id ? "'" : "", primary ? "joinBtn" : "button small", primary ? "#2a7ae2" : "rgba(255,255,255,0.12)", onclick);
    sb_append_esc(sb, label);
    sb_append(sb, "</div>");
}

static void append_text_field(StrBuf *sb, const char *label, const char *id, const char *value, const char *placeholder, const char *type) {
    sb_append(sb, "<div class='settNameSmall'>");
    sb_append_esc(sb, label);
    sb_appendf(sb, "<input id='%s' type='%s' value='", id, type);
    sb_append_esc(sb, value ? value : "");
    sb_append(sb, "' placeholder='");
    sb_append_esc(sb, placeholder ? placeholder : "");
    sb_append(sb, "' autocomplete='off' spellcheck='false' "
                  "style='border-radius:6px;border:none;background:#eee;padding:6px;float:right;margin-top:5px;margin-bottom:5px;width:220px'></div>");
}

// The Select tab's map checkboxes: v10's labels (hostMap, gameMap<mapIndex>), the checked state kept
static void append_map_cards(StrBuf *sb) {
    for (size_t i = 0; i < map_catalog_count(); i++) {
        const MapCatalogEntry *e = map_catalog_at(i);
        if (!official_hostable(e)) continue;
        char thumb[48] = "";
        sb_append(sb, "<label class=\"hostMap\"><div class=\"hostMapName blackShad\">");
        sb_append_esc(sb, e->name);
        sb_appendf(sb, "</div><input onclick=\"playSelect(0.1)\" id=\"gameMap%d\" type=\"checkbox\"%s><img loading=\"lazy\" onmouseenter=\"playTick();\" draggable=\"false\" class=\"hostMapImg\" src=\"",
                   e->index, g_flow.maps[e->index] ? " checked" : "");
        sb_append_esc(sb, map_catalog_thumb(e, thumb, sizeof(thumb)) ? thumb : "./img/noimg.png");
        sb_append(sb, "\"></label>");
    }
}

// The Start Game step: the choice. By the active backend, the first option is krunker.io's hosting or Box
// Office servers (one Start Game button, hostFlowStart); the second is hosting it yourself (the command and
// the Dedicated Server window). The step is hidden unless it is the active tab.
static char *choice_html(const int shown) {
    StrBuf sb = {0};
    const int krunker = krunker_backend();
    sb_appendf(&sb, "<div class='hostTb4%s' id='hostFlowChoice' style='margin-top:-10px;'>", shown ? "" : " hide");
    sb_append(&sb, "<div class='setHed' style='margin-bottom:15px'>");
    sb_append(&sb, krunker ? I18N("kn.hostflow.kio.heading", "krunker.io servers") : I18N("kn.hostflow.box.heading", "Box Office servers"));
    sb_append(&sb, "</div><div class='setBodH' style='padding-top:10px;padding-bottom:12px'><div style='font-size:16px;margin-bottom:6px'>");
    sb_append(&sb, krunker ? I18N("kn.hostflow.kio.body", "krunker.io runs the game on its official servers, with your krunker.io account. Start Game creates it there and you join it.")
                           : I18N("kn.hostflow.box.body", "Box Office runs the game on our servers. Start Game creates it there and you join it."));
    sb_append(&sb, "</div><div id='hostFlowStart' class='bigMenTab' style='display:inline-block;background:#4582d1!important;border-radius:6px;cursor:pointer' "
                   "onmouseenter='playTick(0.1);' onclick='playSelect(0.1);createPrivateRoom();'>");
    sb_append(&sb, I18N("kn.hostflow.start", "Start Game"));
    sb_append(&sb, "</div></div>");

    sb_append(&sb, "<div class='setHed' style='margin:25px 0 15px'>");
    sb_append(&sb, I18N("kn.hostflow.self.heading", "Host it yourself"));
    sb_append(&sb, "</div><div class='setBodH' style='padding-top:10px;padding-bottom:12px'><div style='font-size:16px;margin-bottom:6px'>");
    sb_append(&sb, I18N("kn.hostflow.self.body", "Download the dedicated server and run the command below in its folder. The game runs on your own computer or server."));
    sb_append(&sb, "</div>");
    char port[16], public_port[16];
    snprintf(port, sizeof(port), "%d", g_flow.port);
    snprintf(public_port, sizeof(public_port), "%d", g_flow.public_port);
    append_text_field(&sb, I18N("kn.hostflow.server.name", "Server name"), "hostFlowName", g_flow.name, I18N("kn.hostflow.server.name.hint", "Krunker Box Office server"), "text");
    append_text_field(&sb, I18N("kn.hostflow.server.port", "Port"), "hostFlowPort", port, "21015", "number");
    append_text_field(&sb, I18N("kn.hostflow.server.publicHost", "Public address"), "hostFlowPublicHost", g_flow.public_host, I18N("kn.hostflow.server.publicHost.hint", "for players outside your network"), "text");
    append_text_field(&sb, I18N("kn.hostflow.server.publicPort", "Public port"), "hostFlowPublicPort", public_port, "", "number");
    sb_appendf(&sb, "<div class='settNameSmall'>%s <label class='switch'><input id='hostFlowHeadless' type='checkbox' %s><span class='slider'></span></label></div>",
               I18N("kn.hostflow.server.headless", "No console (headless)"), g_flow.headless ? "checked" : "");
    sb_append(&sb, "<div class='setHed' style='margin:18px 0 10px'>");
    sb_append(&sb, I18N("kn.hostflow.command.heading", "Command for the dedicated server"));
    sb_append(&sb, "</div>");
    sb_append(&sb, "<div id='hostFlowCommand' style='font-family:monospace;font-size:14px;white-space:pre-wrap;word-break:break-all;"
                   "user-select:text;background:rgba(0,0,0,0.45);color:#eee;padding:12px;border-radius:6px'>");
    char line[HOST_COMMAND_WORD_MAX + 1024];
    const int built = command_line(line, sizeof(line));
    sb_append_esc(&sb, built >= 0 ? line : I18N("kn.hostflow.command.error", "Too many settings for one command: remove some overrides."));
    sb_append(&sb, "</div><div style='margin-top:10px;font-size:14px;opacity:0.8'>");
    sb_append(&sb, I18N("kn.hostflow.command.hint", "Run it in the folder with server_bin. The map, mode and settings are in the command."));
    sb_append(&sb, "</div><div style='margin-top:6px'>");
    append_button(&sb, NULL, I18N("kn.hostflow.copy", "Copy command"), "knHostFlow('copy')", 1);
    append_button(&sb, NULL, I18N("kn.hostflow.practice", "Play this map offline"), "knHostFlow('practice')", 0); // its message says when no map is chosen
    // the Dedicated Server window (dedicated_server_ui.c)
    append_button(&sb, "hostFlowGetServer", I18N("kn.hostflow.download", "Get the dedicated server"), "knHostFlow('download')", 0);
    sb_append(&sb, "</div></div></div>");
    return sb_finish(&sb);
}

// The v10 tab bar: Select, Game Settings, Advanced, Server (windows[7].switchTab) and Start Game, the choice
static void append_tabs(StrBuf *sb, const int active) {
    static const char *const names[] = {"Select", "Game Settings", "Advanced", "Server"};
    sb_append(sb, "<div class=\"bigMenTabs\" style=\"margin-top:30px !important;margin-left:0px;\">");
    for (int t = 0; t < 4; t++) {
        sb_appendf(sb, "<div id=\"hstTab%d\" class=\"bigMenTab%s\" onclick=\"playSelect();windows[7].switchTab(%d)\" onmouseenter=\"playTick(0.1);\">%s</div>",
                   t, t == active ? " bigMenTabA" : "", t, names[t]);
    }
    sb_appendf(sb, "<div class=\"bigMenTab%s\" id=\"startServBtn\" style=\"background:#4582d1!important;border-top-right-radius:6px;border-bottom-right-radius:6px;\" "
                   "onclick=\"playSelect(0.1);windows[7].switchTab(%d)\" onmouseenter=\"playTick(0.1);\">%s</div></div>",
               active == FLOW_STEP_START ? " bigMenTabA" : "", FLOW_STEP_START, I18N("kn.hostflow.start", "Start Game"));
}

// ------------------------------------------------------------------ the template splices (ui/windows/8.html)

// s[from, to) replaced with `with` (s is freed; the result is malloc'd)
static char *splice(char *s, const size_t from, const size_t to, const char *with) {
    StrBuf sb = {0};
    sb_append_n(&sb, s, from);
    sb_append(&sb, with ? with : "");
    sb_append(&sb, s + to);
    free(s);
    return sb_finish(&sb);
}

// The first `find` in s replaced with `with`
static char *replace_once(char *s, const char *find, const char *with) {
    const char *at = s ? strstr(s, find) : NULL;
    if (!at) return s;
    return splice(s, (size_t) (at - s), (size_t) (at - s) + strlen(find), with);
}

// Just past the </div> that closes the <div ...> starting at `open`; NULL when unbalanced.
static const char *div_end(const char *open) {
    int depth = 0;
    const char *p = open;
    for (;;) {
        const char *o = strstr(p, "<div"), *c = strstr(p, "</div>");
        if (!c) return NULL;
        if (o && o < c) {
            depth++;
            p = o + 4;
            continue;
        }
        p = c + 6;
        if (--depth <= 0) return p;
    }
}

// The captured hostTb1 block, the run of hostTb2 bodies after it and hostTb3's inside become the
// windows[7] bodies for the step shown (the rows of ui_host_config.c; step_html keeps them visible).
static char *config_bodies(char *html, const int step, const cJSON *preset, const int host_type) {
    char *tab;
    const char *a, *b;
    const char *tb1 = "<div class=\"hostTb1 hide\">";
    if ((a = strstr(html, tb1)) && (b = div_end(a))) {
        tab = step == 1 ? ui_host_config_step_html(HOST_CONFIG_TAB_MODES, host_type, preset) : ui_host_config_tab_html(HOST_CONFIG_TAB_MODES, host_type, preset);
        html = splice(html, (size_t) (a - html), (size_t) (b - html), tab);
        free(tab);
    }
    const char *tb2 = "<div class=\"hostTb2 hide\">"; // the Advanced bodies (not the empty one with a style)
    if ((a = strstr(html, tb2))) {
        b = a;
        const char *next;
        while (!strncmp(b, tb2, strlen(tb2)) && (next = div_end(b))) b = next;
        tab = step == 2 ? ui_host_config_step_html(HOST_CONFIG_TAB_ADVANCED, host_type, preset) : ui_host_config_tab_html(HOST_CONFIG_TAB_ADVANCED, host_type, preset);
        html = splice(html, (size_t) (a - html), (size_t) (b - html), tab);
        free(tab);
    }
    if (step == 3) html = replace_once(html, "<div class=\"hostTb3 hide\"", "<div class=\"hostTb3\"");
    const char *tb3 = "<div class=\"hostTb3\"";
    if ((a = strstr(html, tb3)) || (a = strstr(html, "<div class=\"hostTb3 hide\""))) {
        b = div_end(a);
        const char *inner = strchr(a, '>');
        if (b && inner && inner < b) {
            tab = ui_host_config_tab_html(HOST_CONFIG_TAB_SERVER, host_type, preset);
            html = splice(html, (size_t) (inner + 1 - html), (size_t) (b - 6 - html), tab);
            free(tab);
        }
    }
    return html;
}

// The window's markup for the state (MENU_WINDOW_HOST's generator, servers_ui.c)
char *host_flow_window_gen(void) {
    const char *tpl = ui_windows_html("8");
    if (!tpl || !*tpl) return strdup("");
    const char *title = game_host_type_name(g_flow.host_type);
    cJSON *preset = preset_of(g_flow.request);
    char *html = strdup(tpl);
    if (!html) {
        cJSON_Delete(preset);
        return strdup("");
    }
    if (title) html = replace_once(html, "Custom Game Setup", title); // hostTypes[i].title
    html = replace_once(html, "<div class=\"hostHeader\"", "<div id=\"hostHeader\" class=\"hostHeader\"");
    // the tab bar: v10's four tabs and Start Game, the active one marked
    char *a = html ? strstr(html, "<div class=\"bigMenTabs\"") : NULL;
    const char *end_marker = "Start Game</div></div>";
    const char *b = a ? strstr(a, end_marker) : NULL;
    if (a && b) {
        StrBuf tabs = {0};
        append_tabs(&tabs, g_flow.step < FLOW_STEP_START ? g_flow.step : FLOW_STEP_START);
        char *bar = sb_finish(&tabs);
        html = splice(html, (size_t) (a - html), (size_t) (b + strlen(end_marker) - html), bar);
        free(bar);
    }
    // Select: the community banner names the chosen community map; the raw game data is not hostable
    if (g_flow.custom_name[0]) {
        StrBuf name = {0};
        sb_append_esc(&name, g_flow.custom_name);
        char *esc = sb_finish(&name);
        html = replace_once(html, "Browse Community Games", esc ? esc : "");
        free(esc);
    }
    {
        const char *raw = strstr(html, "Raw Game Data");
        const char *start = NULL, *at = html;
        while (raw && (at = strstr(at, "<div class=\"setBodH\">")) && at < raw) start = at++;
        const char *end = start ? div_end(start) : NULL;
        if (start && end && end > raw) html = splice(html, (size_t) (start - html), (size_t) (end - html), "");
    }
    {
        const char *anchor = "<div class=\"mapListCat\">Default Krunker Maps</div>";
        const char *m = strstr(html, anchor);
        const char *n = m ? strstr(m, "</div></div><div id=\"forcedSettings\"") : NULL;
        if (m && n) {
            StrBuf cards = {0};
            append_map_cards(&cards);
            char *list = sb_finish(&cards);
            html = splice(html, (size_t) (m + strlen(anchor) - html), (size_t) (n - html), list ? list : "");
            free(list);
        }
    }
    if (g_flow.step != 0) html = replace_once(html, "<div class=\"hostTb0\">", "<div class=\"hostTb0 hide\">");
    html = config_bodies(html, g_flow.step, preset, g_flow.host_type);
    cJSON_Delete(preset);
    // the choice: the last step, before the sticky Server Cost line
    {
        const char *anchor = "<div style=\"margin-top:40px\"></div><div id=\"stickyHostH\">";
        char *choice = choice_html(g_flow.step == FLOW_STEP_START);
        StrBuf with = {0};
        sb_append(&with, choice ? choice : "");
        sb_append(&with, anchor);
        char *text = sb_finish(&with);
        html = replace_once(html, anchor, text ? text : "");
        free(text);
        free(choice);
    }
    // the message line (set_message's text when the window is generated)
    {
        StrBuf msg = {0};
        sb_append(&msg, "id=\"hostGameMsg\">");
        sb_append_esc(&msg, g_flow.message);
        sb_append(&msg, "</span>");
        char *text = sb_finish(&msg);
        html = replace_once(html, "id=\"hostGameMsg\"></span>", text ? text : "");
        free(text);
    }
    return html ? html : strdup("");
}

// ------------------------------------------------------------------ the window's state changes

static void flow_show(void) { menu_show_window(MENU_WINDOW_HOST, 1); }

static void flow_reset(const int host_type) {
    cJSON_Delete(g_flow.request);
    memset(&g_flow, 0, sizeof(g_flow));
    g_flow.host_type = game_host_type_name(host_type) ? host_type : GAME_HOST_CUSTOM;
    g_flow.port = FLOW_DEFAULT_PORT;
    servers_ui_set_host_type(g_flow.host_type); // the rows' read-back (ui_host_config_collect) reads it
}

// openHostWindow(false, type): the chooser's server-type cards (window 42)
void host_flow_open(const int host_type) {
    flow_reset(host_type);
    flow_show();
}

// selectHostMap on a catalogue map (the Games window's Host button): an official map is checked; a community
// map is the Select banner's choice. The rest of the window's values are kept.
void host_flow_open_map(const int catalog_index) {
    const MapCatalogEntry *e = map_catalog_at((size_t) catalog_index);
    if (e && official_hostable(e)) {
        g_flow.maps[e->index] = 1;
    } else if (e) {
        snprintf(g_flow.custom_key, sizeof(g_flow.custom_key), "%s", e->key);
        snprintf(g_flow.custom_name, sizeof(g_flow.custom_name), "%s", e->name);
    }
    g_flow.step = 0;
    flow_show();
}

// selectHostMap on a published map (downloaded and checked first, community_fetch): the banner's choice
void host_flow_open_published(const char *name, const char *creator) {
    char spec[256] = "";
    if (community_published_spec(spec, sizeof(spec), name, creator)) {
        snprintf(g_flow.custom_key, sizeof(g_flow.custom_key), "%s", spec);
        snprintf(g_flow.custom_name, sizeof(g_flow.custom_name), "%s", name);
    }
    g_flow.step = 0;
    flow_show();
}

// showGames(true), the banner and the chooser's Community Games card: the Games window browses the
// community maps (its Host button picks one). The window's values are kept for the way back.
void host_flow_browse_community(void) {
    char err[160] = "";
    sync_page(err, sizeof(err));
    menu_show_window(10, 0);
}

// windows[7].switchTab(n) (v10's, page only): the step's tab and body; the choice reads the values back
void host_flow_switch_tab(const int tab) {
    if (tab < 0 || tab >= FLOW_STEP_COUNT) return;
    g_flow.step = tab;
    for (int i = 0; i < 4; i++) {
        char id[16];
        snprintf(id, sizeof(id), "hstTab%d", i);
        HtmlElem t = html_dom_by_id(id);
        if (t) html_dom_set_class(t, "bigMenTabA", i == tab);
        char sel[32];
        snprintf(sel, sizeof(sel), "#menuWindow .hostTb%d", i);
        HtmlElem bodies[16];
        const int n = html_dom_query_all(0, sel, bodies, 16);
        for (int k = 0; k < n; k++) html_dom_set_class(bodies[k], "hide", i != tab);
    }
    HtmlElem start = html_dom_by_id("startServBtn");
    if (start) html_dom_set_class(start, "bigMenTabA", tab == FLOW_STEP_START);
    HtmlElem choice = html_dom_by_id("hostFlowChoice");
    if (choice) html_dom_set_class(choice, "hide", tab != FLOW_STEP_START);
    if (tab == FLOW_STEP_START) refresh_command();
}

// ------------------------------------------------------------------ knHostFlow(action, ...)

static int arg_text(const HtmlArg *args, const int argc, const int i, char *buf, const size_t size) {
    buf[0] = 0;
    if (i >= argc) return 0;
    if (args[i].kind == HTML_ARG_STR && args[i].str) snprintf(buf, size, "%s", args[i].str);
    else if (args[i].kind == HTML_ARG_NUM) snprintf(buf, size, "%.0f", args[i].num);
    else return 0;
    return 1;
}

static int arg_number(const HtmlArg *args, const int argc, const int i, const int fallback) {
    if (i >= argc) return fallback;
    if (args[i].kind == HTML_ARG_NUM || args[i].kind == HTML_ARG_BOOL) return (int) args[i].num;
    if (args[i].kind == HTML_ARG_STR && args[i].str) return atoi(args[i].str);
    return fallback;
}

// A map's checkbox by its key (the tests and the console use keys): checked and kept
static void check_map(const char *key) {
    for (size_t i = 0; i < map_catalog_count(); i++) {
        const MapCatalogEntry *e = map_catalog_at(i);
        if (!official_hostable(e) || strcmp(e->key, key)) continue;
        g_flow.maps[e->index] = 1;
        char id[24];
        snprintf(id, sizeof(id), "gameMap%d", e->index);
        HtmlElem box = html_dom_by_id(id);
        if (box) html_dom_set_checked(box, 1);
        return;
    }
}

// The dedicated server's Get button: the values are kept, the Dedicated Server window opens
static void open_dedicated(void) {
    char err[160] = "";
    sync_page(err, sizeof(err));
    g_flow.step = FLOW_STEP_START; // its Back returns to this step
    dedicated_server_ui_open(1);
}

int host_flow_ui_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) ev;
    if (!name || strcmp(name, "knHostFlow")) return -1;
    char action[24] = "";
    arg_text(args, argc, 0, action, sizeof(action));
    if (!strcmp(action, "reopen")) { // the Dedicated Server window's Back: this window as it was left
        flow_show();
        return 1;
    }
    if (!strcmp(action, "go")) {
        host_flow_switch_tab(arg_number(args, argc, 1, g_flow.step));
    } else if (!strcmp(action, "map")) {
        char key[128] = "";
        arg_text(args, argc, 1, key, sizeof(key));
        check_map(key);
    } else if (!strcmp(action, "copy")) {
        char err[160] = "";
        sync_page(err, sizeof(err));
        char line[HOST_COMMAND_WORD_MAX + 1024];
        if (command_line(line, sizeof(line)) >= 0) {
            html_clipboard_write(line);
            set_message(I18N("kn.hostflow.copied", "Command copied."), 0);
        } else {
            set_message(I18N("kn.hostflow.command.error", "Too many settings for one command: remove some overrides."), 1);
        }
    } else if (!strcmp(action, "practice")) {
        char err[160] = "";
        if (!sync_page(err, sizeof(err))) {
            set_message(err, 1);
            return 1;
        }
        practice(); // the offline game took the window (or its message says why)
        return 1;
    } else if (!strcmp(action, "download")) {
        open_dedicated(); // in the client, not a web page
    }
    // "apply" (a server field changed): the command follows the values when its step is shown
    return 1;
}
