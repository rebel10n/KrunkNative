// The Host Game window (client/include/host_flow_ui.h): v10's host_games window (windows[7] of v10.0.2,
// the captured ui/windows/8.html markup): the Select, Game Settings, Advanced and Server tabs, then the
// last step, Start Game, which is the choice of where the game runs. Every Host Game entry point opens
// this one window (MENU_WINDOW_HOST, openHostWindow(false, type)). Its tabs are v10's (windows[7].switchTab,
// on the page only), so every value stays in the page until a step reads it back (sync_page). The state
// (g_flow) keeps them across a trip to the Dedicated Server window and back (knHostFlow 'reopen').
//
// The last step offers, by the window's backend: the Servers tab it was opened from (host_flow_set_chooser_backend,
// the "+ Host Game" of krunk.io or krunker.io, then the chooser), else the active one (krunker_net_active(): the
// official krunker.io connection). The session's backend is never changed here.
//   krunker.io  -> krunker.io's official hosting: v10's CUSTOM_SERVER request (krunker_net_host_custom); a map
//                  chosen on krunker.io's Games window goes by its name (customMap), a Box Office one is refused
//   krunk.io    -> Box Office servers: the brain hosts the game (/mm/host-game, servers_ui_host_game)
//   always      -> host it yourself: the server_bin command and the Dedicated Server window
#include <host_flow_ui.h>
#include <host_command.h>
#include <dedicated_server_ui.h> // "Get the dedicated server": the Dedicated Server window
#include <rent_servers_ui.h>      // host type 2, Rent Dedicated Server (buyServ): its own pages
#include <client.h>
#include <community_host.h>      // the community maps' seam (community_maps.c): published maps, the catalogue
#include <game_config.h>
#include <html_dom.h>
#include <html_ui.h>
#include <i18n.h>
#include <krunker_net.h>         // the krunker.io branch: the official connection and its hosting
#include <host_route.h>         // which backend Start Game hosts on (the Server Type setting, the tab, an official game)
#include <seek_prefs.h>         // the Server Type setting (seek_prefs_servers)
#include <backend_ui.h>       // servers_ui_host_game: Start Game on Box Office servers; account_backend (one backend per session)
#include <map_catalog.h>
#include <shared.h>
#include <ui_host_config.h>
#include <ui_logic.h>
#include <backend.h>             // the brain's getMapPreset (/webhooks/general/maps/config)
#include <krunker_account.h>     // krunker_host_url: krunker.io's getMapPreset
#include <krunker_kio_routes.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FLOW_STEP_COUNT 5       // 0 Select, 1 Game Settings, 2 Advanced, 3 Server, 4 Start Game (the choice)
#define FLOW_STEP_START 4
#define FLOW_STEP_SERVER 3      // the Server tab: the dedicated server command
#define FLOW_DEFAULT_PORT 21015 // NET_DEFAULT_PORT
#define FLOW_MAP_MAX MAP_CATALOG_OFFICIAL_COUNT

// The flow's state. The page holds the values while the window is open; sync_page() reads them back.
static struct {
    int ready;                             // flow_reset ran once (before it the backend reads 0, the Box Office tab's)
    int host_type;                         // config.hostTypes index (GAME_HOST_*)
    int backend;                           // the Start Game's backend: 1 krunker.io, 0 Box Office, -1 the active one
    int step;                              // the tab shown (FLOW_STEP_*)
    unsigned char maps[FLOW_MAP_MAX];      // gameMap<mapIndex> checked (Krunker's own maps)
    char custom_key[256];                  // a community map chosen in the Games window: its key or published spec ("" none)
    char custom_name[96];                  // its name (the Select banner)
    int custom_official;                   // the choice was made on krunker.io's Games window (a krunker.io map, hosted by name)
    cJSON *request;                        // the rows as last read back (ui_host_config_collect); NULL until read
    char name[64];                         // --hostname (the self-host fields)
    int port;                              // --port (0: the default)
    char password[64];                     // --password (Server tab's Server Password)
    char public_host[128];                 // --public-host
    int public_port;                       // --public-port (0: none)
    int headless;                          // --headless
    char message[192];                     // #hostGameMsg's text when the window is generated
    char *raw;                             // the Raw Game Data field's text (rawMapData; Custom with no community map)
    int busy;                              // a hosting request is out: Start Game / Rehost greyed (deob-10.0.7.js:204800-204802)
} g_flow;

// windows[7].presets[name] (deob-10.0.7.js:219690-219700): the chosen community map's preset, asked once per name
// (getMapPreset, /webhooks/general/maps/config?mn=<name> -> {da, fu}). v10 keeps one per name for the session;
// KrunkNative keeps the last one asked.
static struct {
    char name[96];   // the map it is for ("" none asked)
    int loaded;      // presetLoaded ran (0 while the request is out: "Loading preset...")
    cJSON *data;     // JSON.parse(da) as the rows' preset ({modes, classes, ..., settings}); NULL = 'DEFAULT'
    double funds;    // fu: the map's KR funds (KR Rewards is offered while > 0)
    unsigned gen;    // the request's, so a stale answer is dropped
} g_preset;

// The "+ Host Game" of a Servers tab opens the chooser (window 42, deob-10.0.7.js:218502: showWindow(42)); the tab's
// backend waits there for the server type picked next (host_flow_open). -1: none.
static int g_chooser_backend = -1;

// Krunker's own map (the Select tab's checkboxes): official and hostable in the catalogue
static int official_hostable(const MapCatalogEntry *e) {
    return e && e->source == MAP_SOURCE_OFFICIAL && e->hostable && e->index >= 0 && e->index < FLOW_MAP_MAX;
}

// config.hostTypes' window fields (deob-10.0.5.js:16551-16631; the config itself is shared/src/game_config.c):
// mapH (the Select tab's map heading), allMaps (the maps listed, by lowercase name; NULL every map), topHedr
// (the Select banner; NULL the template's Browse Community Games) and disTabs (bit n: tab n greyed out).
typedef struct {
    const char *map_heading;
    const char *const *all_maps;
    const char *banner;
    unsigned int dis_tabs;
} HostWindowType;

static const char *const k_comp_maps[] = {"burg", "sandstorm", "undergrowth", "shipment", "industry", "lumber", "site",
                                          "bureau", "atomic", "lush", "eterno simulator", "frontier", NULL};
static const char *const k_zombies_maps[] = {"facility", NULL};
static const char *const k_raid_maps[] = {"tortuga", "laboratory", "soul sanctum", "khepri", "bastion", NULL};

#define HOST_BANNER(img, text) \
    "<div id=\"hostCMapPickr\" style=\"background-image:linear-gradient(to right, #000000a8, #00000000),url(" img ");\">" \
    "<div id=\"hostCMapPickT\">" text "</div></div>"

static const HostWindowType k_host_window[GAME_HOST_TYPE_COUNT] = {
    // Comp: deob-10.0.5.js:16559-16564
    [GAME_HOST_COMP] = {NULL, k_comp_maps, HOST_BANNER("img/backgrounds/gamecats/c_1.png", "Dedicated comp &amp; tournament server"),
                        1u << 1 | 1u << 3},
    // Zombies: deob-10.0.5.js:16589-16594
    [GAME_HOST_ZOMBIES] = {"Zombies Maps", k_zombies_maps, HOST_BANNER("img/backgrounds/server_zombies.png", "Dedicated Zombies server"),
                           1u << 1 | 1u << 2 | 1u << 3},
    // Raid: deob-10.0.5.js:16611-16616
    [GAME_HOST_RAID] = {"Raid Maps", k_raid_maps, HOST_BANNER("img/backgrounds/server_raids.png", "Dedicated Raid server"),
                        1u << 1 | 1u << 2 | 1u << 3},
};

static const HostWindowType *host_window(void) {
    return g_flow.host_type >= 0 && g_flow.host_type < GAME_HOST_TYPE_COUNT ? &k_host_window[g_flow.host_type] : &k_host_window[0];
}

// A Krunker map the window lists (deob-10.0.5.js:218128-218130: allMaps.indexOf(name.toLowerCase()) < 0 skips it)
static int listed_map(const MapCatalogEntry *e) {
    if (!official_hostable(e)) return 0;
    const char *const *names = host_window()->all_maps;
    if (!names) return 1;
    for (; *names; names++) {
        const char *a = *names, *b = e->name ? e->name : "";
        while (*a && *b && *a == (char) tolower((unsigned char) *b)) a++, b++;
        if (!*a && !*b) return 1;
    }
    return 0;
}

// The last step's backend: the window's (the Servers tab it opened from), else the Server Type setting (krunker.io's
// hosting for krunker.io, Box Office's brain otherwise), or krunker.io's while an official game is the session
// (host_route.h: the rule, tested by host_route_test)
static int krunker_backend(void) {
    return host_route_krunker(g_flow.backend, seek_prefs_servers() == SEEK_SERVERS_OFFICIAL, krunker_net_active());
}

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

#define RAW_DATA_MAX (1 << 20) // a pasted game (the editor's export; the published maps' limit is 500k characters)

// The Select tab's values into the state (the window open only): the map checkboxes and the Raw Game Data field
static void sync_maps(void) {
    if (!page_open()) return;
    for (size_t i = 0; i < map_catalog_count(); i++) {
        const MapCatalogEntry *e = map_catalog_at(i);
        if (!listed_map(e)) continue;
        char id[24];
        snprintf(id, sizeof(id), "gameMap%d", e->index);
        const HtmlElem box = html_dom_by_id(id);
        if (box) g_flow.maps[e->index] = html_dom_get_checked(box) ? 1 : 0;
    }
    char *raw = malloc(RAW_DATA_MAX);
    if (raw && read_field("rawMapData", raw, RAW_DATA_MAX)) {
        free(g_flow.raw);
        g_flow.raw = raw[0] ? strdup(raw) : NULL;
    }
    free(raw);
}

// The page's values into the state (the window open only): the Select tab's (sync_maps), the self-host
// fields, the password and the rows (ui_host_config_collect). Returns 0 with `err` set when the rows are
// not valid (no mode, no class, a long password); the state then keeps the rows it had.
static int sync_page(char *err, const size_t err_size) {
    if (err_size) err[0] = 0;
    if (!page_open()) return 1;
    sync_maps();
    read_field("passCode", g_flow.password, sizeof(g_flow.password));
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
        if (!listed_map(e) || !g_flow.maps[e->index]) continue;
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

// The keys v10's createPrivateRoom object always carries (deob-10.0.3.js:229827-229839), read off the open window:
// rawMapData (the Raw Game Data field's text, "" when empty; null without the field), passCode (the password field's
// text; null without it, and in a comp / restricted lobby, whose branch never looks it up: :229813-229817), and every
// serverConfig row: one without its field is 0 for a bool or an input row (`el ? el.checked : 0`, `el ? value : 0`,
// :229832-229835; ui_host_config_collect reads the fields that are there). A closed window keeps the rows read last.
static void host_config_fields(cJSON *config) {
    if (!page_open()) {
        if (!cJSON_HasObjectItem(config, "rawMapData")) cJSON_AddNullToObject(config, "rawMapData");
        if (!cJSON_HasObjectItem(config, "passCode")) cJSON_AddNullToObject(config, "passCode");
        return;
    }
    const size_t raw_size = RAW_DATA_MAX;
    char *raw = malloc(raw_size);
    cJSON_DeleteItemFromObjectCaseSensitive(config, "rawMapData");
    if (raw && read_field("rawMapData", raw, raw_size)) cJSON_AddStringToObject(config, "rawMapData", raw);
    else cJSON_AddNullToObject(config, "rawMapData");
    free(raw);
    char text[128];
    const int choices = !(game_host_type_flags(g_flow.host_type) & (GAME_HOST_FLAG_COMP | GAME_HOST_FLAG_RESTRICT));
    cJSON_DeleteItemFromObjectCaseSensitive(config, "passCode");
    if (choices && read_field("passCode", text, sizeof(text))) cJSON_AddStringToObject(config, "passCode", text);
    else cJSON_AddNullToObject(config, "passCode");
    size_t count = 0;
    const GameConfigOption *options = game_config_options(GAME_CONFIG_SERVER, &count);
    for (size_t k = 0; k < count; k++) {
        if (options[k].kind != GC_BOOL && options[k].kind != GC_INPUT) continue;
        char id[48];
        snprintf(id, sizeof(id), "customS%s", options[k].var);
        if (html_dom_by_id(id)) {
            // an input row's text as v10 sends it: sanitize (<, >, & and every non-ASCII byte go, :27403-27409)
            const cJSON *item = cJSON_GetObjectItemCaseSensitive(config, options[k].var);
            if (options[k].kind == GC_INPUT && cJSON_IsString(item)) {
                char *w = item->valuestring;
                for (const char *r = item->valuestring; *r; r++) {
                    if (*r != '<' && *r != '>' && *r != '&' && !((unsigned char) *r & 0x80)) *w++ = *r;
                }
                *w = 0;
            }
            continue;
        }
        cJSON_DeleteItemFromObjectCaseSensitive(config, options[k].var);
        cJSON_AddNumberToObject(config, options[k].var, 0);
    }
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
        if (listed_map(e) && g_flow.maps[e->index]) cJSON_AddItemToArray(maps, cJSON_CreateNumber(e->index));
    }
    if (g_flow.custom_key[0]) cJSON_AddStringToObject(config, "customMap", g_flow.custom_key);
    else cJSON_AddNullToObject(config, "customMap"); // customMap: the picked map's name || null (deob-10.0.3.js:229831)
    // krRewards / defaultSetts: the community map's switches, !!el && el.checked (deob-10.0.7.js:204703-204709)
    const HtmlElem kr = html_dom_by_id("krRewards"), def = html_dom_by_id("defaultSetts");
    cJSON_AddBoolToObject(config, "krRewards", kr && html_dom_get_checked(kr));
    cJSON_AddBoolToObject(config, "defaultSetts", def && html_dom_get_checked(def));
    if (!cJSON_HasObjectItem(config, "share")) cJSON_AddBoolToObject(config, "share", 0); // the Share on Hub row's (ui_host_config_collect)
    host_config_fields(config);
    return config;
}

// createPrivateRoom's first check: a checked map, the chosen game or Raw Game Data (deob-10.0.7.js:204675)
static int any_map(void) {
    if (g_flow.custom_key[0] || (g_flow.raw && g_flow.raw[0])) return 1;
    for (size_t i = 0; i < FLOW_MAP_MAX; i++)
        if (g_flow.maps[i]) return 1;
    return 0;
}

// ------------------------------------------------------------------ Start Game (the choice's first option)

// The community map's preset is still being asked for (v10's iiIiiíî, deob-10.0.7.js:219845): "Loading preset...",
// tabs 1-3 greyed out and Start Game greyed with no onclick
static int preset_loading(void) { return g_flow.custom_key[0] && !(g_preset.loaded && !strcmp(g_preset.name, g_flow.custom_name)); }

// Force Default Settings starts on (v10's iìÍiììí: a community map whose preset came, :219841-219843)
static int default_forced(void) { return g_flow.custom_key[0] && !preset_loading(); }

// canRehost (deob-10.0.7.js:115245: isDev || isAdmin) on a game this socket hosts (iíîïîïî): krunker.io's only, the
// one backend with a rehost (CUSTOM_SERVER's 4th argument). Box Office's brain has none.
static int can_rehost(void) {
#ifdef __EMSCRIPTEN__
    return 0;
#else
    KrunkerNetAccount account;
    return krunker_backend() && krunker_net_is_host() && krunker_net_account(&account) && (account.dev || account.admin);
#endif
}

// createPrivateRoom's "Please wait..." (deob-10.0.7.js:204800-204802): Start Game and Rehost greyed, no clicks; the
// answer gives them back (customRoomResponse, :204627-204628). v10 asks getElementById: the first element with the id.
static void set_buttons_busy(const int busy) {
    g_flow.busy = busy;
    static const char *const ids[] = {"startServBtn", "rehostServBtn"};
    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        const HtmlElem el = html_dom_by_id(ids[i]);
        if (!el) continue;
        html_dom_set_style(el, "background", busy ? "#595959" : i ? "var(--red)" : "#4582d1");
        html_dom_set_style(el, "pointer-events", busy ? "none" : "all");
    }
}

void host_flow_room_response(const int created) {
    set_buttons_busy(0);
    if (!created) return;
    // a game was made: the Raw Game Data field is emptied (deob-10.0.7.js:204630)
    const HtmlElem raw = html_dom_by_id("rawMapData");
    if (raw) html_dom_set_value(raw, "");
    free(g_flow.raw);
    g_flow.raw = NULL;
}

static void on_krunker_hosted(void *user, const char *game_id, const char *region, const char *error) {
    (void) user;
    host_flow_room_response(game_id != NULL);
    if (!game_id) {
        set_message(error && *error ? error : I18N("custom.tryagain", "Please try again."), 1);
        return;
    }
    set_message(I18N("kn.host.created", "Game Created"), 0); // customRoomResponse(message, gameId)
    menu_show_window(MENU_WINDOW_NONE, 0);
    krunker_net_join(menu_ui()->client, game_id, region ? region : "");
}

// A refusal before anything was sent: the buttons come back with the reason
static void refused(const char *text) {
    host_flow_room_response(0);
    set_message(text, 1);
}

// createPrivateRoom(rehost) (deob-10.0.7.js:204669-204811) on the window's backend: krunker.io's official hosting or
// Box Office's brain. v10's checks in its order: a map (a checked one, the chosen game or Raw Game Data), then the
// rows (a mode, a class, the password's length); then "Please wait..." with the buttons greyed until the answer.
void host_flow_create(int rehost) {
    if (preset_loading() || g_flow.busy) return; // the greyed button has no onclick / no pointer events (:219942)
    sync_maps();
    if (!any_map()) {
        set_message(I18N("custom.map.missing", "Select at least one Game or Map"), 1);
        return;
    }
    char err[160] = "";
    if (!sync_page(err, sizeof(err)) || !g_flow.request) {
        set_message(err[0] ? err : I18N("kn.host.invalidSettings", "Invalid game settings"), 1);
        return;
    }
    cJSON *config = host_config();
    if (!config) {
        set_message(I18N("kn.host.invalidSettings", "Invalid game settings"), 1);
        return;
    }
    rehost = rehost && can_rehost(); // iíîïîïî && rehost && account.canRehost (:204672)
    set_message(I18N("generic.wait", "Please wait..."), 0);
    set_buttons_busy(1);
    if (krunker_backend()) {
#ifdef __EMSCRIPTEN__
        refused(I18N("kn.hostflow.kio.web", "Hosting on krunker.io is not available in the browser build"));
#else
        if (g_flow.custom_key[0] && !g_flow.custom_official) { // a Box Office (kn_brain) community map is not on krunker.io
            refused(I18N("kn.hostflow.kio.community", "Community maps are hosted on Box Office servers or on your own server."));
        } else {
            // a krunker.io map is hosted by its name, as v10.0.7's createPrivateRoom sends it (customMap: the chosen
            // map's name, deob-10.0.7.js:204711); its data comes with the game's init (krunker_map_source.h)
            if (g_flow.custom_key[0]) cJSON_ReplaceItemInObject(config, "customMap", cJSON_CreateString(g_flow.custom_name));
            // the refusals say why (krunker_net_host_block): the player can act on each one
            switch (krunker_net_host_block()) {
            case KRUNKER_HOST_NOT_CONNECTED:
                refused(I18N("kn.hostflow.kio.notConnected", "Join a krunker.io game first: a game hosted on krunker.io starts from your official connection."));
                break;
            case KRUNKER_HOST_BUSY:
                refused(I18N("kn.hostflow.kio.busy", "krunker.io is still answering the last game. Wait for it, then try again."));
                break;
            case KRUNKER_HOST_READY:
                if (!krunker_net_host_custom_ex(config, rehost, on_krunker_hosted, NULL))
                    refused(I18N("kn.hostflow.kio.unavailable", "krunker.io servers are unavailable right now. Try again, or host it yourself."));
                break;
            }
        }
#endif
    } else if (account_backend() == ACCOUNT_BACKEND_KRUNKER) {
        // one backend per session: the brain sends nothing while krunker.io is the active backend (account_ui.c)
        refused(I18N("kn.hostflow.box.gated", "Box Office servers are not reachable while you play on krunker.io. Leave that game first."));
    } else {
        char *json = cJSON_PrintUnformatted(config);
        const int sent = json ? servers_ui_host_game(json) : 0; // the brain's answer gives the buttons back
        free(json);
        if (!sent) refused(I18N("kn.hostflow.box.unavailable", "Box Office servers are unavailable right now. Host it yourself with the dedicated server."));
    }
    cJSON_Delete(config);
}

void host_flow_start(void) { host_flow_create(0); }

// ------------------------------------------------------------------ Play this map offline (KrunkNative only)

// The offline game's map: the Games window's catalogue map (the banner), else the first checked Krunker map.
// -1 when none is a catalogue map (a published map is practised through community_fetch, below).
static int practice_catalog_index(void) {
    if (g_flow.custom_key[0]) return map_catalog_index_of_key(g_flow.custom_key);
    for (size_t i = 0; i < map_catalog_count(); i++) {
        const MapCatalogEntry *e = map_catalog_at(i);
        if (listed_map(e) && g_flow.maps[e->index]) return (int) i;
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

// The Select tab's map checkboxes: v10's labels (hostMap, gameMap<mapIndex>), the checked state kept
static void append_map_cards(StrBuf *sb) {
    for (size_t i = 0; i < map_catalog_count(); i++) {
        const MapCatalogEntry *e = map_catalog_at(i);
        if (!listed_map(e)) continue;
        char thumb[48] = "";
        sb_append(sb, "<label class=\"hostMap\"><div class=\"hostMapName blackShad\">");
        sb_append_esc(sb, e->name);
        sb_appendf(sb, "</div><input onclick=\"playSelect(0.1)\" id=\"gameMap%d\" type=\"checkbox\"%s><img loading=\"lazy\" onmouseenter=\"playTick();\" draggable=\"false\" class=\"hostMapImg\" src=\"",
                   e->index, g_flow.maps[e->index] ? " checked" : "");
        sb_append_esc(sb, map_catalog_thumb(e, thumb, sizeof(thumb)) ? thumb : "./img/noimg.png");
        sb_append(sb, "\"></label>");
    }
}

// The dedicated-server command, the one addition to v10's host window: the server_bin line the client builds
// (host_command.c) on the Server tab (hostTb3, shown by switchTab with that tab), with a Copy button in v10's own
// small-button style (.hostPresetBtn). No fields, no hint line, no other buttons.
static char *command_html(const int shown) {
    StrBuf sb = {0};
    sb_appendf(&sb, "<div class=\"hostTb3%s\" id=\"hostFlowCommandBlock\">", shown ? "" : " hide");
    sb_append(&sb, "<div class=\"setHed\" style=\"margin-bottom:15px\">");
    sb_append(&sb, I18N("kn.hostflow.command.heading", "Dedicated server command"));
    sb_append(&sb, "</div><div class=\"setBodH\">");
    sb_append(&sb, "<div id=\"hostFlowCommand\" style=\"font-family:monospace;font-size:14px;white-space:pre-wrap;word-break:break-all;"
                   "user-select:text;background:rgba(0,0,0,0.45);color:#eee;padding:12px;border-radius:6px\">");
    char line[HOST_COMMAND_WORD_MAX + 1024];
    const int built = command_line(line, sizeof(line));
    sb_append_esc(&sb, built >= 0 ? line : I18N("kn.hostflow.command.error", "Too many settings for one command: remove some overrides."));
    sb_append(&sb, "</div><div class=\"hostPresetBtn\" style=\"width:auto;margin-top:12px;padding:6px 12px\" onmouseenter=\"playTick()\" "
                   "onclick=\"playSelect(0.1);knHostFlow('copy')\">");
    sb_append_esc(&sb, I18N("kn.hostflow.copy", "Copy command"));
    sb_append(&sb, "</div></div></div>");
    return sb_finish(&sb);
}

// The v10 tab bar: Select, Game Settings, Advanced, Server (windows[7].switchTab) and Start Game, the choice.
// bigMenTabDis (greyed, pointer-events:none; deob-10.0.7.js:219935-219939): the host type's disTabs, Game Settings and
// Advanced while a community map's defaults are forced, and those and Server while its preset loads (:219846-219849).
static void append_tabs(StrBuf *sb, const int active) {
    static const char *const names[] = {"Select", "Game Settings", "Advanced", "Server"};
    const int loading = preset_loading();
    unsigned int dis = host_window()->dis_tabs;
    if (default_forced()) dis |= 1u << 1 | 1u << 2;
    if (loading) dis |= 1u << 1 | 1u << 2 | 1u << 3;
    sb_append(sb, "<div class=\"bigMenTabs\" style=\"margin-top:30px !important;margin-left:0px;\">");
    for (int t = 0; t < 4; t++) {
        sb_appendf(sb, "<div id=\"hstTab%d\" class=\"bigMenTab%s\" onclick=\"playSelect();windows[7].switchTab(%d)\" onmouseenter=\"playTick(0.1);\">%s</div>",
                   t, t == active ? " bigMenTabA" : dis & 1u << t ? " bigMenTabDis" : "", t, names[t]);
    }
    // startServBtn, a button of the tab row (deob-10.0.7.js:219941-219944): greyed with no onclick while the preset
    // loads (and, here, while a request is out: createPrivateRoom greyed it). A dev / admin hosting this game gets
    // Rehost too (createPrivateRoom(true)), which v10 gives Start Game's id.
    const int rehost = can_rehost();
    const int grey = loading || g_flow.busy;
    sb_appendf(sb, "<div class=\"bigMenTab\" id=\"startServBtn\" style=\"background:%s!important;%s%s\" onclick=\"%s\">%s</div>",
               grey ? "#595959" : "#4582d1", rehost ? "" : "border-top-right-radius:6px;border-bottom-right-radius:6px;",
               g_flow.busy ? "pointer-events:none;" : "", loading ? "" : "playSelect(0.1);createPrivateRoom();", I18N("kn.hostflow.start", "Start Game"));
    if (rehost) {
        sb_appendf(sb, "<div class=\"bigMenTab\" id=\"startServBtn\" style=\"background:%s!important;border-top-right-radius:6px;border-bottom-right-radius:6px;%s\" "
                       "onclick=\"%s\">Rehost</div>",
                   grey ? "#595959" : "var(--red)", g_flow.busy ? "pointer-events:none;" : "", loading ? "" : "playSelect(0.1);createPrivateRoom(true);");
    }
    sb_append(sb, "</div>");
}

// ------------------------------------------------------------------ the community map (deob-10.0.7.js:219949-219950)

// iîìíïïí: a number with thousands commas
static void commas(char *out, const size_t size, const double value) {
    char digits[32];
    snprintf(digits, sizeof(digits), "%.0f", value > 0 ? value : 0);
    const size_t n = strlen(digits);
    size_t k = 0;
    for (size_t i = 0; i < n && k + 2 < size; i++) {
        if (i && (n - i) % 3 == 0) out[k++] = ',';
        out[k++] = digits[i];
    }
    out[k] = 0;
}

// The chosen game's card: its thumbnail with Change Game (showGames(1)), the trash (openHostWindow(true)), its name
// (#commMap) and the Players / Minutes / Gamemodes / Likes lines, which v10.0.7 leaves at "..."
static void append_map_card(StrBuf *sb) {
    char thumb[48] = "";
    const int index = map_catalog_index_of_key(g_flow.custom_key);
    const MapCatalogEntry *e = index >= 0 ? map_catalog_at((size_t) index) : NULL;
    const char *image = e && map_catalog_thumb(e, thumb, sizeof(thumb)) ? thumb : "./img/noimg.png";
    sb_append(sb, "<div style='margin-top:30px;'><div style=\"width:360px;display:inline-block;margin-left:10px;margin-right:20px;\">"
                  "<div class=\"setSugBox sugBoxSelCus blackShad\" style=\"background:url(");
    sb_append_esc(sb, image);
    sb_append(sb, ")\" onmouseenter=\"playTick(0.1);\" onclick=\"playSelect();showGames(1)\"><div class=\"segSugIH\"><div class=\"setSugInf\">Change Game</div></div></div>"
                  "<div id=\"hostGameNameHolder\"><span><i style=\"color:#eb5656;font-size:30px;vertical-align:-3px;cursor:pointer\" onclick=\"openHostWindow(true)\" "
                  "class=\"material-icons\">delete</i> </span><span id=\"commMap\" style=\"color:white\">");
    sb_append_esc(sb, g_flow.custom_name);
    sb_append(sb, "</span></div></div><div class=\"setBodH\" style=\"width:745px;display:inline-block;margin-top:0px;vertical-align:top\">"
                  "<div class=\"settNameSmall\" style=\"font-size:19px\">Players<span id=\"customSetPrev0\" style=\"color:white;float:right\">...</span></div>"
                  "<div class=\"settNameSmall\" style=\"font-size:19px\">Minutes<span id=\"customSetPrev2\" style=\"color:white;float:right\">...</span></div>"
                  "<div class=\"settNameSmall\" style=\"font-size:19px\">Gamemodes<span id=\"gameModesC\" style=\"max-width:480px;color:white;float:right;overflow:hidden;"
                  "text-overflow:ellipsis;white-space:nowrap;\" class=\"textOverflow\">...</span></div>"
                  "<div class=\"settNameSmall\" style=\"font-size:19px\">Likes<span id=\"gameLikesC\" style=\"color:white;float:right\">...</span></div></div></div>");
    if (preset_loading()) return;
    // Force Default Settings (checked) and, while the map has funds, KR Rewards (:219950)
    const int funds = g_preset.funds > 0;
    sb_append(sb, "<div class='setBodH' style='padding-top:10px'>");
    if (!(game_host_type_flags(g_flow.host_type) & (GAME_HOST_FLAG_COMP | GAME_HOST_FLAG_RESTRICT))) { // !noCustM: Custom
        sb_appendf(sb, "<div class='settNameSmall' style='margin:0px;margin-top:10px;border-bottom:none;padding-bottom: 0px;'>%s<label class='switch'>"
                       "<input id='defaultSetts' type='checkbox' onclick='windows[7].toggleDefault()' checked><span class='slider'></span></label></div>"
                       "<div id='defaultSettsInfo' style='margin-top:15px;margin-bottom:15px;'><div style='padding:10px;background-color:rgba(0,0,0,0.6);"
                       "border: 3px solid rgba(255, 255, 255, 0.4);margin-bottom:0px;margin-top:10px;padding-left:15px;font-size:18px'>%s</div></div>",
                   I18N("server.config.forceDefault", "Force Default Settings"),
                   I18N("windows.host.default.settings", "Enabling Default Settings allows for storing Player Progress and Highscores"));
    }
    if (funds) {
        char amount[40];
        commas(amount, sizeof(amount), g_preset.funds);
        sb_appendf(sb, "<div class='settNameSmall' style='margin:0px;margin-top:20px;border-bottom:none;padding-top:0px;padding-bottom:0px;'>%s<div class='krIconHost'>KR</div> "
                       "<label class='switch'><input id='krRewards' type='checkbox' onclick='windows[7].toggleKRRew()' checked><span class='slider'></span></label></div>"
                       "<div id='krRewardsInfo' style='margin-top:15px;margin-bottom:5px;'><div style='padding:10px;background-color:rgba(0,0,0,0.4);"
                       "border: 3px solid rgba(255, 255, 255, 0.4);margin-bottom:0px;margin-top:10px;padding-left:15px;font-size:18px'>%s"
                       "<div style='margin-top:20px;'>Available <span style='float:right;color:rgba(255,255,255,0.4)'>%s <span style='color:rgba(255,255,255,0.8)'>KR</span>"
                       "</span></div></div></div><div style='height:10px;'></div>",
                   I18N("server.config.krRewards", "KR Rewards"),
                   I18N("windows.host.rewards", "Enabling KR Rewards forces the Server to use Default Settings & allows players to earn KR from the map funds:"), amount);
    }
    sb_append(sb, "</div>");
}

static void on_preset(void *user, const int status, const char *body, const size_t length) {
    if ((unsigned) (uintptr_t) user != g_preset.gen) return;
    (void) status; // apiRequest reads any answer's JSON (deob-10.0.7.js:133166): no status check
    cJSON *res = body && length ? cJSON_ParseWithLength(body, length) : NULL;
    const char *da = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(res, "da"));
    const cJSON *fu = cJSON_GetObjectItemCaseSensitive(res, "fu");
    cJSON *data = da && da[0] ? cJSON_Parse(da) : NULL; // JSON.parse(da); 'DEFAULT' when there is none or it throws
    if (data && !cJSON_IsObject(data)) {
        cJSON_Delete(data);
        data = NULL;
    }
    // the brain's da is the map's config read flat (game_config_from_map): the rows' preset form has its settings
    // under "settings", as krunker.io's does
    if (data && !cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(data, "settings"))) {
        cJSON *rows = preset_of(data);
        cJSON_Delete(data);
        data = rows;
    }
    cJSON_Delete(g_preset.data);
    g_preset.data = data;
    g_preset.funds = cJSON_IsNumber(fu) ? fu->valuedouble : 0; // fu || 0
    g_preset.loaded = 1;
    cJSON_Delete(res);
    if (!strcmp(g_preset.name, g_flow.custom_name)) { // the rows are the preset's (presetLoaded: updateWindow(8))
        cJSON_Delete(g_flow.request);
        g_flow.request = NULL;
    }
    menu_rerender_if_open(MENU_WINDOW_HOST);
}

// getMapPreset(name, presetLoaded) once per name (deob-10.0.7.js:220155): krunker.io's data client for a map of its
// Games window, else the brain's (services/brain/creations.c: the catalogue map's config, no funds)
static void preset_request(void) {
    if (!g_flow.custom_key[0] || !strcmp(g_preset.name, g_flow.custom_name)) return;
    snprintf(g_preset.name, sizeof(g_preset.name), "%s", g_flow.custom_name);
    cJSON_Delete(g_preset.data);
    g_preset.data = NULL;
    g_preset.funds = 0;
    g_preset.loaded = 0;
    const unsigned gen = ++g_preset.gen;
    char enc[300], path[400];
    backend_url_encode(enc, sizeof(enc), g_flow.custom_name);
    snprintf(path, sizeof(path), "mn=%s", enc);
    int sent = 0;
#ifndef __EMSCRIPTEN__
    if (g_flow.custom_official) {
        char base[256], url[640];
        krunker_host_url(KRUNKER_HOST_API, "", base, sizeof(base));
        sent = krunker_kio_webhook_url(url, sizeof(url), base, "maps/config", path) &&
               backend_http_get_remote(url, KRUNKER_KIO_READ_TIMEOUT_MS, on_preset, (void *) (uintptr_t) gen);
    } else
#endif
    {
        char full[440];
        snprintf(full, sizeof(full), "/webhooks/general/maps/config?%s", path);
        sent = backend_http_get(full, on_preset, (void *) (uintptr_t) gen);
    }
    if (!sent) g_preset.loaded = 1; // no request: presetLoaded(name) with no data, 'DEFAULT'
}

// windows[7].toggleDefault(forced) (deob-10.0.7.js:219764-219779): clicking Force Default Settings turns KR Rewards
// off; the switch shows or hides the rows and the presets and greys Game Settings / Advanced (setTabs' rd)
static void toggle_default(const int forced) {
    const HtmlElem def = html_dom_by_id("defaultSetts");
    if (!def) return;
    if (!forced) {
        const HtmlElem kr = html_dom_by_id("krRewards");
        if (kr) html_dom_set_checked(kr, 0);
    }
    const int on = html_dom_get_checked(def);
    const HtmlElem rows = html_dom_by_id("forcedSettings"), presets = html_dom_by_id("hostGamePreset");
    if (rows) html_dom_set_style(rows, "display", on ? "none" : "block");
    if (presets) html_dom_set_style(presets, "display", on ? "none" : "block");
    for (int t = 1; t <= 2; t++) {
        char id[16];
        snprintf(id, sizeof(id), "hstTab%d", t);
        const HtmlElem tab = html_dom_by_id(id);
        if (tab) html_dom_set_class(tab, "bigMenTabDis", on);
    }
}

// windows[7].toggleKRRew(skip) (:219757-219763): clicking KR Rewards forces the default settings on
static void toggle_kr_rewards(const int skip) {
    if (!html_dom_by_id("krRewards") || skip) return;
    const HtmlElem def = html_dom_by_id("defaultSetts");
    if (def) html_dom_set_checked(def, 1);
    toggle_default(1);
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
    // buyServ (deob-10.0.7.js:219853): Rent Dedicated Server draws only its rental pages (rent_servers_ui.c)
    if (g_flow.host_type == GAME_HOST_RENT) return rent_servers_ui_gen();
    const char *tpl = ui_windows_html("8");
    if (!tpl || !*tpl) return strdup("");
    const char *title = game_host_type_name(g_flow.host_type);
    preset_request(); // a community map: its preset is asked for the first time here (gen's getMapPreset)
    const int loading = preset_loading(), forced = default_forced();
    // the rows' values: the ones read back last, else the community map's preset (presets[name].data, :219964-219965)
    cJSON *preset = g_flow.request ? preset_of(g_flow.request) : g_flow.custom_key[0] && g_preset.data ? cJSON_Duplicate(g_preset.data, 1) : NULL;
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
    // Select: a dedicated host type's banner (topHedr) replaces the community one, which names the chosen
    // community map; the raw game data is not hostable
    if (host_window()->banner && !g_flow.custom_name[0]) {
        const char *pick = strstr(html, "<div id=\"hostCMapPickr\"");
        const char *end = pick ? div_end(pick) : NULL;
        if (end) html = splice(html, (size_t) (pick - html), (size_t) (end - html), host_window()->banner);
    } else if (!g_flow.custom_name[0]) {
        // Custom's topHedr: gamecats/c_<[4, 8, 9, 11, 13][randInt(0, 4)]> (deob-10.0.5.js:16552-16555). The captured
        // template froze c_9; scenario renders keep it.
        static const int cats[] = {4, 8, 9, 11, 13};
        int cat = cats[rand() % 5];
#ifdef UI_SCENARIO_SUPPORTED
        if (ui_scenario_active()) cat = 9;
#endif
        char img[40];
        snprintf(img, sizeof(img), "gamecats/c_%d.png", cat);
        html = replace_once(html, "gamecats/c_9.png", img);
    } else {
        // a chosen game: its card and switches in place of topHedr (deob-10.0.7.js:219949-219950)
        const char *pick = strstr(html, "<div id=\"hostCMapPickr\"");
        const char *end = pick ? div_end(pick) : NULL;
        StrBuf card = {0};
        append_map_card(&card);
        char *text = sb_finish(&card);
        if (end) html = splice(html, (size_t) (pick - html), (size_t) (end - html), text ? text : "");
        free(text);
    }
    {
        // Raw Game Data (windows.host.raw, :219951): Custom's (no noCustM) with no game chosen, its value kept
        const char *raw = strstr(html, "Raw Game Data");
        const char *start = NULL, *at = html;
        while (raw && (at = strstr(at, "<div class=\"setBodH\">")) && at < raw) start = at++;
        const char *end = start ? div_end(start) : NULL;
        const int shown = g_flow.host_type == GAME_HOST_CUSTOM && !g_flow.custom_key[0];
        if (start && end && end > raw && !shown) html = splice(html, (size_t) (start - html), (size_t) (end - html), "");
        else if (shown && g_flow.raw && g_flow.raw[0]) {
            StrBuf value = {0};
            sb_append(&value, "value=\"");
            sb_append_esc(&value, g_flow.raw);
            sb_append(&value, "\"");
            char *text = sb_finish(&value);
            html = replace_once(html, "placeholder=\"Raw Game Data\"", text ? text : "");
            free(text);
        }
    }
    {
        const char *anchor = "<div class=\"mapListCat\">Default Krunker Maps</div>";
        const char *m = strstr(html, anchor);
        const char *n = m ? strstr(m, "</div></div><div id=\"forcedSettings\"") : NULL;
        if (m && n && g_flow.custom_key[0]) {
            html = splice(html, (size_t) (m - html), (size_t) (n - html), ""); // a chosen game: no map list (:219954)
        } else if (m && n) {
            StrBuf cards = {0};
            append_map_cards(&cards);
            char *list = sb_finish(&cards);
            html = splice(html, (size_t) (m + strlen(anchor) - html), (size_t) (n - html), list ? list : "");
            free(list);
        }
        // mapH || "Default Krunker Maps" (deob-10.0.5.js:218125)
        if (host_window()->map_heading) {
            char heading[64];
            snprintf(heading, sizeof(heading), ">%s</div>", host_window()->map_heading);
            html = replace_once(html, ">Default Krunker Maps</div>", heading);
        }
    }
    if (g_flow.step != 0) html = replace_once(html, "<div class=\"hostTb0\">", "<div class=\"hostTb0 hide\">");
    html = config_bodies(html, g_flow.step, preset, g_flow.host_type);
    cJSON_Delete(preset);
    if (loading) {
        // no preset yet: no rows at all (gen's `if (haveData)`, :219964; getMapPreset instead, :220155)
        const char *a = strstr(html, "<div id=\"forcedSettings\"");
        const char *b = a ? strstr(a, "<div style=\"margin-top:40px\"></div><div id=\"stickyHostH\">") : NULL;
        if (b) html = splice(html, (size_t) (a - html), (size_t) (b - html), "");
    } else if (forced) {
        // the defaults forced: the rows and the presets hidden (:219967, the hostGamePreset style)
        html = replace_once(html, "<div id=\"forcedSettings\" style=\"display:block\">", "<div id=\"forcedSettings\" style=\"display:none\">");
        html = replace_once(html, "<div id='hostGamePreset' style='text-align:left;margin-top:20px;margin-bottom:10px;display:block'>",
                            "<div id='hostGamePreset' style='text-align:left;margin-top:20px;margin-bottom:10px;display:none'>");
    }
    // the dedicated-server command (Server tab), before the sticky Server Cost line
    {
        const char *anchor = "<div style=\"margin-top:40px\"></div><div id=\"stickyHostH\">";
        char *choice = command_html(g_flow.step == FLOW_STEP_SERVER);
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
        sb_append_esc(&msg, loading ? "Loading preset..." : g_flow.message); // :219945
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
    free(g_flow.raw);
    memset(&g_flow, 0, sizeof(g_flow));
    g_flow.backend = -1;
    g_flow.host_type = game_host_type_name(host_type) ? host_type : GAME_HOST_CUSTOM;
    g_flow.port = FLOW_DEFAULT_PORT;
    g_flow.ready = 1;
    servers_ui_set_host_type(g_flow.host_type); // the rows' read-back (ui_host_config_collect) reads it
}

// selectHostMap before any Host window was opened (the Games window's Host button): the window's defaults, so
// Start Game hosts on the active backend (-1) and not on the zeroed state's Box Office tab
static void flow_ensure(void) {
    if (!g_flow.ready) flow_reset(GAME_HOST_CUSTOM);
}

// openHostWindow(false, type): the chooser's server-type cards (window 42). Opened from a Servers tab's "+ Host Game",
// the window's backend is that tab's (host_flow_set_chooser_backend).
void host_flow_open(const int host_type) {
    const int backend = g_chooser_backend;
    g_chooser_backend = -1;
    flow_reset(host_type);
    if (backend >= 0) g_flow.backend = backend;
    if (g_flow.host_type == GAME_HOST_RENT) rent_servers_ui_open();
    flow_show();
}

void host_flow_set_chooser_backend(const int krunker) {
    g_chooser_backend = krunker ? 1 : 0;
    menu_show_window(42, 0); // showWindow(42) (deob-10.0.7.js:218502)
}

void host_flow_on_show_window(const int id) {
    if (id != 42 && id != MENU_WINDOW_HOST) g_chooser_backend = -1; // the chooser was left for another window
}

// openHostWindow(true): the trash on the chosen game's card (deob-10.0.7.js:225502-225511): no game, no raw data,
// host type 0 (Custom), the window again. The window's backend stays.
void host_flow_clear_map(void) {
    const int backend = g_flow.ready ? g_flow.backend : -1;
    flow_reset(GAME_HOST_CUSTOM);
    g_flow.backend = backend;
    flow_show();
}

// selectHostMap on a catalogue map (the Games window's Host button): an official map is checked; a community
// map is the Select banner's choice. The rest of the window's values are kept.
void host_flow_open_map(const int catalog_index) {
    flow_ensure();
    const MapCatalogEntry *e = map_catalog_at((size_t) catalog_index);
    if (e && official_hostable(e)) {
        g_flow.maps[e->index] = 1;
    } else if (e) {
        free(g_flow.raw); // selectHostMap: rawMapData = null (deob-10.0.7.js:225345)
        g_flow.raw = NULL;
        cJSON_Delete(g_flow.request); // the rows come from the new game's preset (gen, :219832)
        g_flow.request = NULL;
        snprintf(g_flow.custom_key, sizeof(g_flow.custom_key), "%s", e->key);
        snprintf(g_flow.custom_name, sizeof(g_flow.custom_name), "%s", e->name);
        g_flow.custom_official = account_backend() == ACCOUNT_BACKEND_KRUNKER;
    }
    g_flow.step = 0;
    flow_show();
}

// selectHostMap on a published map: the banner's choice. A Box Office map is downloaded and checked first
// (community_fetch); a krunker.io map is remembered by its name only, as v10.0.7's selectHostMap does
// ({id, name}, then showWindow(8): deob-10.0.7.js:225334-225347), and hosted by that name.
void host_flow_open_published(const char *name, const char *creator) {
    flow_ensure();
    char spec[256] = "";
    if (community_published_spec(spec, sizeof(spec), name, creator)) {
        free(g_flow.raw); // selectHostMap: rawMapData = null (deob-10.0.7.js:225345)
        g_flow.raw = NULL;
        cJSON_Delete(g_flow.request); // the rows come from the new game's preset (gen, :219832)
        g_flow.request = NULL;
        snprintf(g_flow.custom_key, sizeof(g_flow.custom_key), "%s", spec);
        snprintf(g_flow.custom_name, sizeof(g_flow.custom_name), "%s", name);
        g_flow.custom_official = account_backend() == ACCOUNT_BACKEND_KRUNKER;
    }
    g_flow.step = 0;
    flow_show();
}

// The Host Presets' commMap (deob-10.0.3.js:249190-249209): v10's chosen custom map is {id, name} and it hosts it by
// that name (createPrivateRoom customMap = iîîiïii.name, :229835). Save stores the chosen map's name.
const char *host_flow_community_name(void) { return g_flow.custom_key[0] ? g_flow.custom_name : ""; }

// Load: v10 sets the chosen map's name (and #commMap's text) to the preset's, so the preset's game is the one hosted.
// With no map chosen v10 has no #commMap and throws there, leaving the official maps the choice: KrunkNative skips
// the key the same way (0) and still redraws the presets block. Here the name is turned back into what the window
// hosts: the catalogue's community map of that name, else the published map of that name (published:<name>; the
// server finds it by name, server/src/accounts.c). The Select banner shows the name.
int host_flow_set_community_name(const char *name) {
    if (!g_flow.custom_key[0] || !name || !name[0]) return 0;
    if (strcmp(name, g_flow.custom_name)) {
        char key[sizeof(g_flow.custom_key)] = "";
        for (size_t i = 0; i < map_catalog_count() && !key[0]; i++) {
            const MapCatalogEntry *e = map_catalog_at(i);
            if (e && !official_hostable(e) && e->name && !strcmp(e->name, name)) snprintf(key, sizeof(key), "%s", e->key);
        }
        if (!key[0] && !community_published_spec(key, sizeof(key), name, NULL)) return 0;
        snprintf(g_flow.custom_key, sizeof(g_flow.custom_key), "%s", key);
        snprintf(g_flow.custom_name, sizeof(g_flow.custom_name), "%s", name);
        g_flow.custom_official = account_backend() == ACCOUNT_BACKEND_KRUNKER;
    }
    const HtmlElem banner = html_dom_by_id("hostCMapPickT");
    if (banner) html_dom_set_text(banner, g_flow.custom_name);
    return 1;
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
    g_flow.step = FLOW_STEP_SERVER; // the Server tab, where the command is: its Back returns there
    dedicated_server_ui_open(1);
}

int host_flow_ui_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) ev;
    if (name && !strcmp(name, "windows.7.toggleDefault")) {
        toggle_default(arg_number(args, argc, 0, 0));
        return 1;
    }
    if (name && !strcmp(name, "windows.7.toggleKRRew")) {
        toggle_kr_rewards(arg_number(args, argc, 0, 0));
        return 1;
    }
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
