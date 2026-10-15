// The Host Game flow (client/include/host_flow_ui.h). Every Host Game entry point opens this one
// window: the chooser's server-type cards (openHostWindow), its Community Games card (showGames(true)),
// the Games window's Host button (selectHostMap). The steps keep one state (g_flow): the Host Game
// rows (ui_host_config.c) are rendered in every step and read back as a whole, so moving between
// steps never loses a value. The command is host_command_build's line for that state.
#include <host_flow_ui.h>
#include <host_command.h>
#include <client.h>
#include <game_config.h>
#include <html_dom.h>
#include <html_ui.h>
#include <i18n.h>
#include <map_catalog.h>
#include <community_host.h> // published maps: the list, the download for practice (community_maps.c)
#include <shared.h>
#include <ui_host_config.h>
#include <ui_logic.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FLOW_STEPS 4
#define FLOW_MAPS_MAX 512
#define FLOW_DEFAULT_PORT 21015 // NET_DEFAULT_PORT
#define FLOW_FILTER_ALL 0
#define FLOW_FILTER_OFFICIAL 1
#define FLOW_FILTER_COMMUNITY 2

typedef struct {
    const char *label;          // the card's name
    const char *key;            // the server's catalogue key (--set gameserver.maps=<key>)
    const char *map_file;       // a map file the server loads (a positional argument), or NULL
    const MapCatalogEntry *entry; // the catalogue entry (thumbnail, practice), NULL for a map_file map
    int official;               // Krunker's own map (the Official filter)
    const char *image;          // a published map's thumbnail (a data: URL), NULL none
    const char *creator;        // a published map's creator, NULL for the catalogue's
} HostFlowMap;

// THE SEAM for the community map system: the map step lists exactly these maps. A community map the
// server's catalogue knows (map_catalog.h, a custom_maps/ key) is an entry with `entry` set; a map
// outside the catalogue sets `key` NULL and `map_file` to its JSON path (the server loads that file
// as its map; that map has no practice play). Filters and the selection work on `key` / `map_file`.
// Published maps (kn_brain, community_published_maps) follow the catalogue: their key is the spec
// server_bin takes, "published:<creator>/<name>" (server/include/published_maps.h: the server
// fetches, checks and caches the map), so the command's gameserver.maps names them.
static size_t host_flow_map_entries(HostFlowMap *out, const size_t max) {
    size_t n = 0;
    const CommunityPublished *published = NULL;
    const size_t published_count = community_published_maps(&published);
    for (size_t i = 0; i < map_catalog_count() && n < max; i++) {
        const MapCatalogEntry *e = map_catalog_at(i);
        if (!e) continue;
        const int official = e->source == MAP_SOURCE_OFFICIAL;
        if (official && !e->hostable) continue; // the Host Game window's official maps only
        out[n].label = e->name;
        out[n].key = e->key;
        out[n].map_file = NULL;
        out[n].entry = e;
        out[n].official = official;
        out[n].image = NULL;
        out[n].creator = NULL;
        n++;
    }
    for (size_t i = 0; i < published_count && n < max; i++) {
        out[n] = (HostFlowMap) {published[i].name, published[i].spec, NULL, NULL, 0, published[i].image, published[i].creator};
        n++;
    }
    return n;
}

// A published map's spec: name and creator ("published:<creator>/<name>"), 0 for other keys
static int published_key(const char *key, char *name, const size_t name_size, char *creator, const size_t creator_size) {
    if (!key || strncmp(key, "published:", 10)) return 0;
    const char *rest = key + 10, *slash = strchr(rest, '/');
    snprintf(creator, creator_size, "%.*s", slash ? (int) (slash - rest) : 0, rest);
    snprintf(name, name_size, "%s", slash ? slash + 1 : rest);
    return name[0] != 0;
}

// The flow's state. Strings are the page's and the command's values; `request` is the last
// ui_host_config_collect() object (NULL until the Rules step was read back: the rows' defaults).
static struct {
    int host_type;            // config.hostTypes index (GAME_HOST_*)
    int step;                 // 0 Map, 1 Mode & classes, 2 Rules, 3 Server & command
    int filter;               // FLOW_FILTER_*
    char map_key[128];        // the chosen map's key ("" none)
    char map_file[512];       // the chosen map's file for a map outside the catalogue ("" none)
    cJSON *request;           // the Host Game config (ui_host_config_collect)
    char name[64];            // --hostname
    int port;                 // --port (0: the default)
    char password[64];        // --password (the Server step's field: every host type has it)
    char public_host[128];    // --public-host
    int public_port;          // --public-port (0: none)
    int headless;             // --headless
    char message[192];        // the line under the steps (an error, "Copied")
} g_flow;

static const char *step_title(const int step) {
    switch (step) {
        case 0: return I18N("kn.hostflow.step.map", "Map");
        case 1: return I18N("kn.hostflow.step.mode", "Mode & classes");
        case 2: return I18N("kn.hostflow.step.rules", "Rules");
        default: return I18N("kn.hostflow.step.server", "Server & command");
    }
}

static void flow_reset(const int host_type) {
    cJSON_Delete(g_flow.request);
    memset(&g_flow, 0, sizeof(g_flow));
    g_flow.host_type = game_host_type_name(host_type) ? host_type : GAME_HOST_CUSTOM;
    g_flow.port = FLOW_DEFAULT_PORT;
    servers_ui_set_host_type(g_flow.host_type); // the windows[7] rows read it too (the collect)
}

static void flow_show(void) { menu_show_window(HOST_FLOW_WINDOW, 1); }

static void flow_render(void);

void host_flow_open(const int host_type) {
    flow_reset(host_type);
    flow_show();
    community_published_refresh(flow_render); // the published maps (the map step re-renders with them)
}

void host_flow_open_map(const int catalog_index) {
    flow_reset(GAME_HOST_CUSTOM);
    const MapCatalogEntry *e = map_catalog_at((size_t) catalog_index);
    if (e) {
        snprintf(g_flow.map_key, sizeof(g_flow.map_key), "%s", e->key);
        g_flow.step = 1; // the map is chosen: the Mode & classes step
    }
    flow_show();
}

void host_flow_open_community(void) {
    flow_reset(GAME_HOST_CUSTOM);
    g_flow.filter = FLOW_FILTER_COMMUNITY;
    flow_show();
    community_published_refresh(flow_render);
}

void host_flow_open_published(const char *name, const char *creator) {
    flow_reset(GAME_HOST_CUSTOM);
    if (community_published_spec(g_flow.map_key, sizeof(g_flow.map_key), name, creator)) g_flow.step = 1; // chosen: the Mode step
    flow_show();
    community_published_refresh(flow_render);
}

// ------------------------------------------------------------------ the state from the page

static void field_text(const char *id, char *out, const size_t size) {
    out[0] = 0;
    const HtmlElem el = html_dom_by_id(id);
    if (el) html_dom_get_value(el, out, size);
}

static int field_checked(const char *id) {
    const HtmlElem el = html_dom_by_id(id);
    return el && html_dom_get_checked(el);
}

static int field_int(const char *id, const int fallback) {
    char text[32];
    field_text(id, text, sizeof(text));
    if (!text[0]) return fallback;
    char *end = NULL;
    const long v = strtol(text, &end, 10);
    return end && !*end && v >= 0 && v <= 65535 ? (int) v : fallback;
}

// The page's values into the state. The server fields always; the Host Game rows only when they
// read back (ui_host_config_collect's check fails for a mode-less or class-less or long-password
// game). Returns 0 with `err` set when the rows are not valid.
static int sync_page(char *err, const size_t err_size) {
    field_text("hostFlowName", g_flow.name, sizeof(g_flow.name));
    field_text("passCode", g_flow.password, sizeof(g_flow.password));
    g_flow.port = field_int("hostFlowPort", FLOW_DEFAULT_PORT);
    field_text("hostFlowPublicHost", g_flow.public_host, sizeof(g_flow.public_host));
    g_flow.public_port = field_int("hostFlowPublicPort", 0);
    g_flow.headless = field_checked("hostFlowHeadless");
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

// ------------------------------------------------------------------ the command

static int command_line(char *out, const size_t size) {
    HostCommand spec = {0};
    spec.program = "server_bin";
    spec.map_key = g_flow.map_key[0] ? g_flow.map_key : NULL;
    spec.map_file = g_flow.map_file[0] ? g_flow.map_file : NULL;
    spec.host_type = g_flow.host_type;
    spec.port = g_flow.port;
    spec.hostname = g_flow.name[0] ? g_flow.name : NULL;
    spec.password = g_flow.password[0] ? g_flow.password : NULL;
    spec.public_host = g_flow.public_host[0] ? g_flow.public_host : NULL;
    spec.public_port = g_flow.public_port;
    spec.headless = g_flow.headless;
    return host_command_build(&spec, g_flow.request, out, size);
}

// The practice game's catalogue index (the embedded server plays catalogue maps only)
static int practice_index(void) {
    return g_flow.map_key[0] ? map_catalog_index_of_key(g_flow.map_key) : -1;
}

// ------------------------------------------------------------------ the markup

static void append_button(StrBuf *sb, const char *label, const char *onclick, const int primary) {
    sb_appendf(sb, "<div class='%s' style='display:inline-block;margin:6px 8px 0 0;cursor:pointer;padding:8px 14px;border-radius:6px;background:%s;color:#fff' onmouseenter='playTick()' onclick=\"playSelect(0.1);%s\">",
               primary ? "joinBtn" : "button small", primary ? "#2a7ae2" : "rgba(255,255,255,0.12)", onclick);
    sb_append_esc(sb, label);
    sb_append(sb, "</div>");
}

static void append_map_cards(StrBuf *sb) {
    static HostFlowMap maps[FLOW_MAPS_MAX];
    const size_t count = host_flow_map_entries(maps, FLOW_MAPS_MAX);
    int shown = 0;
    for (size_t i = 0; i < count; i++) {
        const HostFlowMap *m = &maps[i];
        if (g_flow.filter == FLOW_FILTER_OFFICIAL && !m->official) continue;
        if (g_flow.filter == FLOW_FILTER_COMMUNITY && m->official) continue;
        const int picked = !strcmp(m->key, g_flow.map_key);
        char thumb[48] = "";
        const char *src = m->entry && map_catalog_thumb(m->entry, thumb, sizeof(thumb)) ? thumb : m->image ? m->image : "./img/noimg.png";
        // the key as a JS string ('\\' and '\'' escaped: a published map's name may hold them), then
        // HTML-escaped for the attribute
        char js[sizeof(g_flow.map_key) * 2];
        size_t j = 0;
        for (const char *c = m->key; *c && j + 3 < sizeof(js); c++) {
            if (*c == '\\' || *c == '\'') js[j++] = '\\';
            js[j++] = *c;
        }
        js[j] = 0;
        sb_appendf(sb, "<div class='hostMap' style='display:inline-block;vertical-align:top;margin:0 10px 10px 0;width:160px;cursor:pointer;%s' onmouseenter='playTick()' "
                       "onclick=\"playSelect(0.1);knHostFlow('map','", picked ? "outline:3px solid #2a7ae2;" : "");
        sb_append_esc(sb, js);
        sb_append(sb, "')\"");
        if (m->creator) {
            sb_append(sb, " title='");
            sb_append_esc(sb, m->creator);
            sb_append(sb, "'");
        }
        sb_append(sb, "><div class='hostMapName blackShad'>");
        sb_append_esc(sb, m->label);
        sb_append(sb, "</div><img loading='lazy' draggable='false' class='hostMapImg' src='");
        sb_append_esc(sb, src);
        sb_append(sb, "'></div>");
        shown++;
    }
    if (!shown) sb_append(sb, I18N("kn.hostflow.maps.none", "No maps for this filter."));
}

static void append_map_step(StrBuf *sb) {
    sb_append(sb, "<div class='setHed' style='margin-bottom:10px'>");
    sb_append(sb, I18N("kn.hostflow.map.heading", "Choose a map"));
    sb_append(sb, "</div><div style='margin-bottom:10px'>");
    static const char *const labels[] = {"kn.hostflow.filter.all", "kn.hostflow.filter.official", "kn.hostflow.filter.community"};
    static const char *const names[] = {"All maps", "Krunker maps", "Community maps"};
    for (int f = 0; f < 3; f++) {
        char onclick[64];
        snprintf(onclick, sizeof(onclick), "knHostFlow('filter',%d)", f);
        sb_appendf(sb, "<span style='display:inline-block;margin-right:8px;padding:6px 10px;border-radius:6px;cursor:pointer;background:%s;color:#fff' onclick=\"playSelect(0.1);%s\">",
                   g_flow.filter == f ? "#2a7ae2" : "rgba(255,255,255,0.12)", onclick);
        sb_append_esc(sb, I18N(labels[f], names[f]));
        sb_append(sb, "</span>");
    }
    sb_append(sb, "</div><div id='hostFlowMaps' style='max-height:430px;overflow-y:auto;padding-right:6px'>");
    append_map_cards(sb);
    sb_append(sb, "</div>");
}

// The Mode step: the modes and classes rows (ui_host_config.c). Every step's rows are in the page,
// the steps not shown only hidden, so one collect reads the whole Host Game config.
static void append_mode_step(StrBuf *sb, const cJSON *preset) {
    const unsigned int flags = game_host_type_flags(g_flow.host_type);
    if (flags & (GAME_HOST_FLAG_COMP | GAME_HOST_FLAG_RESTRICT)) {
        sb_append(sb, "<div class='setBodH' style='margin-top:10px'>");
        sb_append(sb, I18N("kn.hostflow.mode.fixed", "This server type sets its own modes and classes. Rules and the server settings come next."));
        sb_append(sb, "</div>");
        return;
    }
    char *tab = ui_host_config_step_html(HOST_CONFIG_TAB_MODES, g_flow.host_type, preset);
    sb_append(sb, tab ? tab : "");
    free(tab);
}

static void append_rules_step(StrBuf *sb, const cJSON *preset) {
    char *tab = ui_host_config_step_html(HOST_CONFIG_TAB_ADVANCED, g_flow.host_type, preset);
    sb_append(sb, tab ? tab : "");
    free(tab);
}

static void append_text_field(StrBuf *sb, const char *label, const char *id, const char *value, const char *placeholder, const char *type) {
    sb_append(sb, "<div class='settNameSmall'>");
    sb_append_esc(sb, label);
    sb_appendf(sb, "<input id='%s' type='%s' value='", id, type);
    sb_append_esc(sb, value ? value : "");
    sb_appendf(sb, "' placeholder='");
    sb_append_esc(sb, placeholder ? placeholder : "");
    sb_append(sb, "' autocomplete='off' spellcheck='false' onchange=\"knHostFlow('apply')\" "
                  "style='border-radius:6px;border:none;background:#eee;padding:6px;float:right;margin-top:5px;margin-bottom:5px;width:220px'></div>");
}

static void append_server_step(StrBuf *sb) {
    char line[HOST_COMMAND_WORD_MAX + 1024];
    const int built = command_line(line, sizeof(line));
    char port[16];
    char public_port[16];
    snprintf(port, sizeof(port), "%d", g_flow.port);
    snprintf(public_port, sizeof(public_port), "%d", g_flow.public_port);

    sb_append(sb, "<div class='setHed' style='margin-bottom:10px'>");
    sb_append(sb, I18N("kn.hostflow.server.heading", "Server"));
    sb_append(sb, "</div><div class='setBodH'>");
    append_text_field(sb, I18N("kn.hostflow.server.name", "Server name"), "hostFlowName", g_flow.name, I18N("kn.hostflow.server.name.hint", "Krunker Box Office server"), "text");
    append_text_field(sb, I18N("kn.hostflow.server.port", "Port"), "hostFlowPort", port, "21015", "number");
    append_text_field(sb, I18N("kn.hostflow.server.password", "Password"), "passCode", g_flow.password, I18N("kn.hostflow.server.password.hint", "none"), "text");
    append_text_field(sb, I18N("kn.hostflow.server.publicHost", "Public address"), "hostFlowPublicHost", g_flow.public_host, I18N("kn.hostflow.server.publicHost.hint", "for players outside your network"), "text");
    append_text_field(sb, I18N("kn.hostflow.server.publicPort", "Public port"), "hostFlowPublicPort", public_port, "", "number");
    sb_appendf(sb, "<div class='settNameSmall'>%s <label class='switch'><input id='hostFlowHeadless' type='checkbox' onchange=\"knHostFlow('apply')\" %s><span class='slider'></span></label></div>",
               I18N("kn.hostflow.server.headless", "No console (headless)"), g_flow.headless ? "checked" : "");
    sb_append(sb, "</div>");

    sb_append(sb, "<div class='setHed' style='margin:18px 0 10px'>");
    sb_append(sb, I18N("kn.hostflow.command.heading", "Command for the dedicated server"));
    sb_append(sb, "</div>");
    sb_append(sb, "<div id='hostFlowCommand' style='font-family:monospace;font-size:14px;white-space:pre-wrap;word-break:break-all;"
                  "user-select:text;background:rgba(0,0,0,0.45);color:#eee;padding:12px;border-radius:6px'>");
    sb_append_esc(sb, built >= 0 ? line : I18N("kn.hostflow.command.error", "Too many settings for one command: remove some overrides."));
    sb_append(sb, "</div>");
    sb_append(sb, "<div style='margin-top:10px;font-size:14px;opacity:0.8'>");
    sb_append(sb, I18N("kn.hostflow.command.hint", "Run it in the folder with server_bin. The map, mode and settings are in the command."));
    sb_append(sb, "</div><div style='margin-top:6px'>");
    append_button(sb, I18N("kn.hostflow.copy", "Copy command"), "knHostFlow('copy')", 1);
    char pub_name[96], pub_creator[64];
    if (practice_index() >= 0 || published_key(g_flow.map_key, pub_name, sizeof(pub_name), pub_creator, sizeof(pub_creator)))
        append_button(sb, I18N("kn.hostflow.practice", "Play this map offline"), "knHostFlow('practice')", 0);
    append_button(sb, I18N("kn.hostflow.download", "Get the dedicated server"), "knHostFlow('download')", 0);
    sb_append(sb, "</div>");
}

// The step tabs (windows[7]'s bigMenTab look): a tab goes to its step (its page's values are kept)
static void append_steps(StrBuf *sb) {
    sb_append(sb, "<div style='margin-bottom:12px'>");
    for (int s = 0; s < FLOW_STEPS; s++) {
        char onclick[48];
        snprintf(onclick, sizeof(onclick), "knHostFlow('go',%d)", s);
        sb_appendf(sb, "<span class='%s' style='display:inline-block;margin-right:6px;padding:6px 12px;border-radius:6px;cursor:pointer' onmouseenter='playTick()' onclick=\"playSelect(0.1);%s\">%d. ",
                   s == g_flow.step ? "bigMenTabA" : "bigMenTab", onclick, s + 1);
        sb_append_esc(sb, step_title(s));
        sb_append(sb, "</span>");
    }
    sb_append(sb, "</div>");
}

char *host_flow_window_gen(void) {
    StrBuf sb = {0};
    cJSON *preset = preset_of(g_flow.request);
    sb_append(&sb, "<div id='hostFlow' style='max-width:900px;padding:4px 6px'>");
    sb_append(&sb, "<div class='setHed' style='margin-bottom:6px'>");
    sb_append(&sb, I18N("kn.hostflow.heading", "Host Game"));
    sb_append(&sb, " <span style='opacity:0.7;font-size:16px'>");
    sb_append_esc(&sb, game_host_type_name(g_flow.host_type) ? game_host_type_name(g_flow.host_type) : "");
    sb_append(&sb, "</span></div>");
    append_steps(&sb);
    sb_appendf(&sb, "<div id='hostFlowMsg' style='min-height:20px;margin-bottom:8px;color:#ff9a9a'>");
    sb_append_esc(&sb, g_flow.message);
    sb_append(&sb, "</div>");

    static const char *const step_ids[FLOW_STEPS] = {"hostFlowStep0", "hostFlowStep1", "hostFlowStep2", "hostFlowStep3"};
    for (int s = 0; s < FLOW_STEPS; s++) {
        sb_appendf(&sb, "<div id='%s' style='display:%s'>", step_ids[s], s == g_flow.step ? "block" : "none");
        if (s == 0) append_map_step(&sb);
        else if (s == 1) append_mode_step(&sb, preset);
        else if (s == 2) append_rules_step(&sb, preset);
        else append_server_step(&sb);
        // the step's own navigation
        sb_append(&sb, "<div style='margin-top:12px'>");
        if (s > 0) append_button(&sb, I18N("kn.hostflow.back", "Back"), "knHostFlow('back')", 0);
        if (s < FLOW_STEPS - 1) append_button(&sb, I18N("kn.hostflow.next", "Next"), "knHostFlow('next')", 1);
        sb_append(&sb, "</div></div>");
    }
    sb_append(&sb, "</div>");
    cJSON_Delete(preset);
    return sb_finish(&sb);
}

// ------------------------------------------------------------------ the actions

static void flow_render(void) { menu_rerender_if_open(HOST_FLOW_WINDOW); }

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

// The chosen map by key: its entry (the map step's cards) sets the key and the file
static void choose_map(const char *key) {
    HostFlowMap maps[FLOW_MAPS_MAX];
    const size_t count = host_flow_map_entries(maps, FLOW_MAPS_MAX);
    for (size_t i = 0; i < count; i++) {
        if (strcmp(maps[i].key, key)) continue;
        snprintf(g_flow.map_key, sizeof(g_flow.map_key), "%s", maps[i].key);
        g_flow.map_file[0] = 0;
        if (maps[i].map_file) snprintf(g_flow.map_file, sizeof(g_flow.map_file), "%s", maps[i].map_file);
        return;
    }
}

// A published map's practice: downloaded and checked (community_fetch), then given a slot
static int g_practice_mode = -1;

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
        snprintf(g_flow.message, sizeof(g_flow.message), "%s", error ? error : I18N("kn.games.loadFailed", "Failed to load the game."));
        flow_render();
    } else {
        const CommunityMapRef ref = {name, ""};
        community_pick(menu_ui()->client, &ref, on_practice_slot, NULL); // cached: at once
    }
    free(name);
}

// The Practice (embedded offline game): the request's game config, then the chosen catalogue map
// (or a published one: kn_brain's copy, checked like v10).
static void practice(void) {
    const int index = practice_index();
    char pub_name[96], pub_creator[64];
    if (index < 0 && published_key(g_flow.map_key, pub_name, sizeof(pub_name), pub_creator, sizeof(pub_creator))) {
        g_practice_mode = -1;
        const cJSON *modes = g_flow.request ? cJSON_GetObjectItemCaseSensitive(g_flow.request, "modes") : NULL;
        const cJSON *first = cJSON_GetArrayItem(modes, 0);
        if (cJSON_IsNumber(first)) g_practice_mode = first->valueint;
        community_fetch(pub_name, on_practice_fetched, strdup(pub_name));
        return;
    }
    if (index < 0) {
        snprintf(g_flow.message, sizeof(g_flow.message), "%s", I18N("kn.hostflow.practice.none", "Pick a map of the catalogue to play it offline."));
        flow_render();
        return;
    }
    Client *client = menu_ui()->client;
    int mode = -1;
    const cJSON *modes = g_flow.request ? cJSON_GetObjectItemCaseSensitive(g_flow.request, "modes") : NULL;
    const cJSON *first = cJSON_GetArrayItem(modes, 0);
    if (cJSON_IsNumber(first)) mode = first->valueint;
#ifdef LOCAL_GAME_HAVE_HOST_CONFIG
    if (g_flow.request) local_game_host_config(client, g_flow.request, g_flow.host_type); // the next round's game config
#endif
    host_catalog_map(index, mode); // the offline game starts on it (closes this window)
}

int host_flow_ui_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) ev;
    if (!name || strcmp(name, "knHostFlow")) return -1;
    char action[24] = "";
    arg_text(args, argc, 0, action, sizeof(action));
    char err[160] = "";
    const int page_ok = sync_page(err, sizeof(err)); // the page's values first: every action keeps them
    g_flow.message[0] = 0;

    if (!strcmp(action, "go") || !strcmp(action, "next") || !strcmp(action, "back")) {
        int target = g_flow.step;
        if (!strcmp(action, "go")) target = arg_number(args, argc, 1, g_flow.step);
        else if (!strcmp(action, "next")) target = g_flow.step + 1;
        else target = g_flow.step - 1;
        if (target < 0) target = 0;
        if (target >= FLOW_STEPS) target = FLOW_STEPS - 1;
        if (target > g_flow.step) { // going forward needs a map, and the Host Game rows must read back
            if (g_flow.step == 0 && !g_flow.map_key[0] && !g_flow.map_file[0]) {
                snprintf(g_flow.message, sizeof(g_flow.message), "%s", I18N("kn.hostflow.map.required", "Choose a map first."));
                flow_render();
                return 1;
            }
            if (!page_ok) {
                snprintf(g_flow.message, sizeof(g_flow.message), "%s", err);
                flow_render();
                return 1;
            }
        }
        g_flow.step = target;
    } else if (!strcmp(action, "map")) {
        char key[128] = "";
        arg_text(args, argc, 1, key, sizeof(key));
        choose_map(key);
    } else if (!strcmp(action, "filter")) {
        g_flow.filter = arg_number(args, argc, 1, FLOW_FILTER_ALL);
    } else if (!strcmp(action, "copy")) {
        if (!page_ok) snprintf(g_flow.message, sizeof(g_flow.message), "%s", err);
        else {
            char line[HOST_COMMAND_WORD_MAX + 1024];
            if (command_line(line, sizeof(line)) >= 0) {
                html_clipboard_write(line);
                snprintf(g_flow.message, sizeof(g_flow.message), "%s", I18N("kn.hostflow.copied", "Command copied."));
            } else {
                snprintf(g_flow.message, sizeof(g_flow.message), "%s", I18N("kn.hostflow.command.error", "Too many settings for one command: remove some overrides."));
            }
        }
    } else if (!strcmp(action, "practice")) {
        if (!page_ok) {
            snprintf(g_flow.message, sizeof(g_flow.message), "%s", err);
            flow_render();
            return 1;
        }
        practice();
        return 1; // the offline game took the window
    } else if (!strcmp(action, "download")) {
        ui_open_external(HOST_FLOW_SERVER_DOWNLOAD_URL);
        return 1;
    }
    // "apply" (a server field changed) only re-renders: the command follows the values
    flow_render();
    return 1;
}
