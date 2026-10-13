// KrunkNative Object Manager (F1): a Garry's Mod style spawn menu on the HTML UI. Not a Krunker
// v7.1.4 window (doc/v7/decisions.md "Object Manager"). The rules and the authority live in
// shared/src/object_manager.c; this is the window (id OBJECT_MANAGER_WINDOW), its handlers, the
// F1 / Escape toggle and the client side of packets 250-253.
//
// Window: tabs Objects / Entities / NPCs / Weapons over a searchable grid of icons. Weapons:
// left click adds the weapon to the loadout as an extra slot (again: takes it out), right click
// drops it as a pickup at the crosshair; the other tabs spawn at the crosshair. Remove / Undo
// buttons, and in game the U (undo) and Delete (remove at the crosshair) binds. Opening it in game
// frees the pointer (like the player list key); F1 or Escape closes it and gives it back.
//
// Tools (right pane, #knOmTools): the Tool Gun's tools (object_manager_tool_info). Picking one
// sets the tool and equips the Tool Gun (an extra slot, added like a Weapons-tab click when it is
// not carried); each Tool Gun shot (a fresh fire timer on its slot, object_manager_ui_tick) is an
// OM_ACTION_TOOL request with that tool. The HUD shows "TOOL: <NAME>" beside the ammo (#knToolHud)
// while it is in hand. The tool is kept in storage (kn_omTool).
//
// Console: `give` / `drop` are server commands (shared/src/chat_commands.c); a `give` of a weapon
// to yourself equips it like the Weapons tab's left click (object_manager_ui_chat_sent).
//
// Scroll: #knOmGrid is the scroll container (max-height). Picks, server results and LOADOUT_EXTRA
// update the page in place (om_update_*: the status line's text, the tiles' knOmOwned class and
// badge, the Clear Extras count) so the grid keeps its scrollTop. Only new results rebuild it and
// start at the top: a tab switch (v7's own tabbed windows set menuWindow.scrollTop = 0 on
// switchTab, index.js windows[3]) and a search (om_refresh_body).
#include <client.h>
#include <html_dom.h>
#include <html_ui.h>
#include <ui_logic.h>
#include <object_manager.h>
#include <spectate.h> // client_find_player
#include <weapons.h>
#include <GLFW/glfw3.h> // glfwGetTime: the status line's age
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> // strncasecmp

#define OM_TAB_STORAGE "kn_omTab"
#define OM_TOOL_STORAGE "kn_omTool"
#define OM_EQUIP_SECONDS 3.0 // a pending equip (pick / give) waits this long for its LOADOUT_EXTRA
#define OM_STATUS_SECONDS 4.0
#define OM_BADGE_HTML "<div class='knOmBadge'>LOADOUT</div>" // on a weapon tile in the loadout (knOmOwned)

static const char *const TAB_NAMES[OM_TAB_COUNT] = {"Objects", "Entities", "NPCs", "Weapons"};

static struct {
    int tab;              // ObjectManagerTab on show
    int tab_loaded;
    char search[64];
    int caps;             // OM_CAP_* of the last SPAWN_RESULT, -1 none yet
    int extra_max;
    char status[160];     // the last result line
    int status_error;
    double status_at;     // glfwGetTime() of it
    int from_game;        // opened while the game had the pointer: closing gives it back
    int pending_equip;    // weapon id just added by this client: equip it when LOADOUT_EXTRA lands, -1 none
    int queried;          // a QUERY for the caps went out on this connection (object_manager_ui_tick)
    double pending_at;    // glfwGetTime() of pending_equip
    int tool;             // ObjectManagerTool the Tool Gun applies
    int tool_loaded;
    // the Tool Gun's shots: the fire timer of the held slot (as vfx.c detect_local_shot)
    const Player *shot_me;
    int shot_slot;
    float shot_reload;
    char hud_text[48];    // what #knToolHud shows, "" hidden
} g_om = {.tab = OM_TAB_WEAPONS, .caps = -1, .pending_equip = -1};

static Client *om_client(void) { return menu_ui()->client; }

static int om_window_open(void) {
    return menu_ui()->active_window == OBJECT_MANAGER_WINDOW && menu_window_is_open();
}

static void om_load_tool(void) {
    if (g_om.tool_loaded) return;
    g_om.tool_loaded = 1;
    char value[8];
    if (html_storage_get(OM_TOOL_STORAGE, value, sizeof(value)) >= 0) {
        const int tool = atoi(value);
        if (tool >= 0 && tool < OM_TOOL_COUNT) g_om.tool = tool;
    }
}

static void om_load_tab(void) {
    om_load_tool();
    if (g_om.tab_loaded) return;
    g_om.tab_loaded = 1;
    char value[8];
    if (html_storage_get(OM_TAB_STORAGE, value, sizeof(value)) >= 0) {
        const int tab = atoi(value);
        if (tab >= 0 && tab < OM_TAB_COUNT) g_om.tab = tab;
    }
}

// This client may talk to the server about it: the offline sandbox, or a v7 server.
static int om_server_ok(const Client *client) {
    return client && (client->offline || client->net_server_minor >= NET_PROTOCOL_MINOR_V7);
}

static void om_send(const int action, const int item) {
    Client *client = om_client();
    if (!om_server_ok(client)) {
        printf("[object-manager] the server takes no Object Manager requests\n");
        return;
    }
    NetSpawnReqPacket req = {.action = (uint8_t) action, .item = (int16_t) item};
    const Player *me = client->me;
    req.pitch = me ? me->direction.x : 0.0f; // the crosshair: the view the menu was opened over
    req.yaw = me ? me->direction.y : 0.0f;
    client_net_send(client, NET_PACKET_SPAWN_REQ, &req, sizeof(req));
}

static int om_has_extra(const Player *me, const int weapon) {
    for (int i = 0; me && i < me->extra_weapon_count; i++) {
        if (me->extra_weapons[i] == weapon) return 1;
    }
    return 0;
}

static int om_matches(const char *name) {
    if (!g_om.search[0]) return 1;
    // case-insensitive substring
    for (const char *h = name; *h; h++) {
        size_t i = 0;
        while (g_om.search[i] && h[i] && (h[i] | 32) == (g_om.search[i] | 32)) i++;
        if (!g_om.search[i]) return 1;
    }
    return 0;
}

static void om_append(char **buf, size_t *len, size_t *cap, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static void om_append(char **buf, size_t *len, size_t *cap, const char *fmt, ...) {
    for (;;) {
        va_list ap;
        va_start(ap, fmt);
        const int n = vsnprintf(*buf ? *buf + *len : NULL, *buf ? *cap - *len : 0, fmt, ap);
        va_end(ap);
        if (n < 0) return;
        if (*buf && *len + (size_t) n < *cap) {
            *len += (size_t) n;
            return;
        }
        const size_t want = (*cap ? *cap : 4096) + (size_t) n + 4096;
        char *grown = realloc(*buf, want);
        if (!grown) return;
        if (!*buf) grown[0] = 0;
        *buf = grown;
        *cap = want;
    }
}

static void om_escape(char *out, const size_t size, const char *text) {
    size_t n = 0;
    for (const char *c = text ? text : ""; *c && n + 7 < size; c++) {
        if (*c == '<') n += (size_t) snprintf(out + n, size - n, "&lt;");
        else if (*c == '>') n += (size_t) snprintf(out + n, size - n, "&gt;");
        else if (*c == '&') n += (size_t) snprintf(out + n, size - n, "&amp;");
        else if (*c == '\'' || *c == '"') n += (size_t) snprintf(out + n, size - n, "&#39;");
        else out[n++] = *c;
    }
    out[n] = 0;
}

// The line above the grid: the last result for a while, else what this game allows.
static const char *om_status_text(int *error_out) {
    const Client *client = om_client();
    const char *text = "";
    int error = 0;
    if (g_om.status[0] && glfwGetTime() - g_om.status_at < OM_STATUS_SECONDS) {
        text = g_om.status;
        error = g_om.status_error;
    } else if (!client || !client->me) {
        text = "Join a game to spawn things";
    } else if (!om_server_ok(client)) {
        text = "This server has no Object Manager";
        error = 1;
    } else if (g_om.caps >= 0 && g_om.tab == OM_TAB_WEAPONS && !(g_om.caps & OM_CAP_WEAPONS)) {
        text = "Extra weapons are off in this game";
        error = 1;
    } else if (g_om.caps >= 0 && g_om.tab != OM_TAB_WEAPONS && !(g_om.caps & OM_CAP_SPAWN)) {
        text = "Only developers and the host can spawn here";
        error = 1;
    }
    *error_out = error;
    return text;
}

static void om_status_html(char **buf, size_t *len, size_t *cap) {
    int error = 0;
    const char *text = om_status_text(&error);
    char esc[400];
    om_escape(esc, sizeof(esc), text);
    om_append(buf, len, cap, "<div id='knOmStatus' class='knOmStatus%s'>%s</div>", error ? " knOmErr" : "", esc);
}

static void om_grid_html(char **buf, size_t *len, size_t *cap) {
    const Client *client = om_client();
    const Player *me = client ? client->me : NULL;
    int shown = 0;
    om_append(buf, len, cap, "<div id='knOmGrid' class='knOmGrid'>");
    if (g_om.tab == OM_TAB_WEAPONS) {
        for (int id = 0; id < WEAPON_TABLE_COUNT; id++) {
            if (!object_manager_weapon_listed(id)) continue;
            const Weapon *w = g_weapons[id];
            if (!om_matches(w->name)) continue;
            char name[96];
            om_escape(name, sizeof(name), w->name);
            const int owned = om_has_extra(me, id);
            om_append(buf, len, cap,
                      "<div id='knOmW%d' class='knOmItem%s' onclick='knOmPick(%d,%d)' oncontextmenu='knOmAlt(%d,%d)'>"
                      "<img class='knOmIcon' src='textures/weapons/%s.png'/><div class='knOmName'>%s</div>%s</div>",
                      id, owned ? " knOmOwned" : "", OM_TAB_WEAPONS, id, OM_TAB_WEAPONS, id, w->icon ? w->icon : "icon_0", name,
                      owned ? OM_BADGE_HTML : "");
            shown++;
        }
    } else {
        int count = 0;
        const ObjectManagerEntry *entries = object_manager_entries(g_om.tab, &count);
        for (int i = 0; entries && i < count; i++) {
            if (!om_matches(entries[i].name)) continue;
            char name[96];
            om_escape(name, sizeof(name), entries[i].name);
            const char *icon = entries[i].icon ? entries[i].icon : "help";
            if (strstr(icon, ".png")) // a page image (img/props)
                om_append(buf, len, cap, "<div id='knOmI%d' class='knOmItem' onclick='knOmPick(%d,%d)'><img class='knOmIcon knOmProp' src='%s'/><div class='knOmName'>%s</div></div>",
                          i, g_om.tab, i, icon, name);
            else
                om_append(buf, len, cap,
                          "<div id='knOmI%d' class='knOmItem' onclick='knOmPick(%d,%d)'><div class='knOmGlyph'><span class='material-icons'>%s</span></div>"
                          "<div class='knOmName'>%s</div></div>",
                          i, g_om.tab, i, icon, name);
            shown++;
        }
    }
    if (!shown) {
        om_append(buf, len, cap, "<div class='knOmEmpty'>%s</div>", g_om.search[0] ? "No matches" : "Nothing to spawn here yet");
    }
    om_append(buf, len, cap, "</div>");
}

static const char *om_hint(void) {
    switch (g_om.tab) {
        case OM_TAB_WEAPONS: return "Left click: add to your loadout (again: take it out) | Right click: drop it at your crosshair";
        case OM_TAB_OBJECTS: return "Left click: place it on the surface at your crosshair, facing you | U: undo, Delete: remove at the crosshair";
        case OM_TAB_NPCS: return "Left click: spawn at your crosshair | U: undo, Delete: remove at the crosshair";
        case OM_TAB_ENTITIES: return "Left click: spawn at your crosshair (power-ups despawn on their own timer)";
        default: return "Left click: spawn at your crosshair";
    }
}

static const char *om_tool_gun_line(void) {
    const Client *client = om_client();
    const Player *me = client ? client->me : NULL;
    if (g_om.caps >= 0 && !(g_om.caps & OM_CAP_SPAWN)) return "Only developers and the host can use the Tool Gun here";
    if (me && me->weapon && me->weapon->kn_tool) return "Tool Gun in hand: shoot to use";
    for (int i = 0; me && me->loadout && i < me->loadout_size; i++) {
        if (me->loadout[i] == WEAPON_ID_TOOL_GUN) return "Tool Gun in your loadout: pick a tool to hold it";
    }
    return "Pick a tool to equip the Tool Gun";
}

// The right pane: the tools (the picked one marked), its hint, and whether the Tool Gun is in hand
static void om_tools_html(char **buf, size_t *len, size_t *cap) {
    const int allowed = g_om.caps < 0 || (g_om.caps & OM_CAP_SPAWN);
    om_append(buf, len, cap, "<div id='knOmTools' class='knOmTools%s'><div class='knOmToolsTitle'>Tools</div>", allowed ? "" : " knOmOff");
    for (int i = 0; i < OM_TOOL_COUNT; i++) {
        const ObjectManagerToolInfo *t = object_manager_tool_info(i);
        om_append(buf, len, cap,
                  "<div id='knOmTool%d' class='knOmTool%s' onclick='knOmTool(%d)'><span class='material-icons'>%s</span>"
                  "<span class='knOmToolName'>%s</span></div>",
                  i, i == g_om.tool ? " knOmToolA" : "", i, t->icon, t->name);
    }
    const ObjectManagerToolInfo *t = object_manager_tool_info(g_om.tool);
    om_append(buf, len, cap, "<div id='knOmToolHint' class='knOmToolHint'>%s</div>", t ? t->hint : "");
    om_append(buf, len, cap, "<div id='knOmToolGun' class='knOmToolGun'>%s</div></div>", om_tool_gun_line());
}

static void om_clear_label(char *out, const size_t size, const Player *me) {
    snprintf(out, size, "Clear Extras (%d/%d)", me ? me->extra_weapon_count : 0, PLAYER_EXTRA_WEAPONS_MAX);
}

static char *om_body_html(void) {
    char *buf = NULL;
    size_t len = 0, cap = 0;
    om_status_html(&buf, &len, &cap);
    om_grid_html(&buf, &len, &cap);
    return buf;
}

char *object_manager_window_gen(const int id, const char **header) {
    if (id != OBJECT_MANAGER_WINDOW) return NULL;
    om_load_tab();
    if (header) *header = "Object Manager";
    const Player *me = om_client() ? om_client()->me : NULL;
    char *buf = NULL;
    size_t len = 0, cap = 0;
    char search[sizeof(g_om.search) * 6];
    om_escape(search, sizeof(search), g_om.search);
    // v7 hides #windowHeader: the title is part of the window
    om_append(&buf, &len, &cap, "<div class='knOm'><div class='knOmTitle'>Object Manager</div><div class='knOmTop'><div class='bigMenTabs knOmTabs'>");
    for (int i = 0; i < OM_TAB_COUNT; i++) {
        om_append(&buf, &len, &cap, "<div id='knOmTab%d' class='bigMenTab%s' onclick='knOmTab(%d)'>%s</div>", i, i == g_om.tab ? " bigMenTabA" : "", i,
                  TAB_NAMES[i]);
    }
    om_append(&buf, &len, &cap,
              // not id='knOmSearch': on the web window.knOmSearch would be the element (named access),
              // so kn-shim.js installs no knOmSearch forwarder and oninput throws
              "</div><input id='knOmSearchBox' class='knOmSearch' type='text' maxlength='40' placeholder='Search' value='%s' "
              "oninput='knOmSearch(this)'/></div><div class='knOmMain'><div id='knOmBody'>",
              search);
    char *body = om_body_html();
    om_append(&buf, &len, &cap, "%s</div>", body ? body : "");
    free(body);
    om_tools_html(&buf, &len, &cap);
    om_append(&buf, &len, &cap, "</div>"); // .knOmMain
    om_append(&buf, &len, &cap, "<div class='knOmFoot'><div class='knOmHint'>%s</div><div class='knOmBtns'>", om_hint());
    if (g_om.tab == OM_TAB_WEAPONS) {
        char clear[48];
        om_clear_label(clear, sizeof(clear), me);
        om_append(&buf, &len, &cap, "<div id='knOmClearBtn' class='button small buttonR knOmBtn' onclick='knOmClear()'>%s</div>", clear);
    }
    om_append(&buf, &len, &cap, "<div id='knOmRemoveBtn' class='button small knOmBtn' onclick='knOmRemove()'>Remove</div>"
                                "<div id='knOmUndoBtn' class='button small knOmBtn' onclick='knOmUndo()'>Undo</div></div></div></div>");
    return buf;
}

// Rebuilds the status + grid only (the search box keeps its focus and caret): the search
// changed the results, so the new grid starts at the top.
static void om_refresh_body(void) {
    if (!om_window_open()) return;
    const HtmlElem body = html_dom_by_id("knOmBody");
    if (!body) return;
    char *html = om_body_html();
    html_dom_set_html(body, html ? html : "");
    free(html);
    const HtmlElem grid = html_dom_by_id("knOmGrid");
    if (grid) html_dom_set_scroll(grid, 0.0f, 0.0f);
}

// The status line in place (a result, the caps): the grid keeps its scroll position.
static void om_update_status(void) {
    if (!om_window_open()) return;
    const HtmlElem status = html_dom_by_id("knOmStatus");
    if (!status) return;
    int error = 0;
    html_dom_set_text(status, om_status_text(&error));
    html_dom_set_class(status, "knOmErr", error);
}

// The loadout in place (LOADOUT_EXTRA): the weapon tiles' knOmOwned class and LOADOUT badge,
// the Clear Extras count. Tiles the search hides have no element.
static void om_update_loadout(void) {
    if (!om_window_open() || g_om.tab != OM_TAB_WEAPONS) return;
    const Client *client = om_client();
    const Player *me = client ? client->me : NULL;
    for (int id = 0; id < WEAPON_TABLE_COUNT; id++) {
        char tile_id[16];
        snprintf(tile_id, sizeof(tile_id), "knOmW%d", id);
        const HtmlElem tile = html_dom_by_id(tile_id);
        if (!tile) continue;
        const int owned = om_has_extra(me, id);
        if (owned == html_dom_has_class(tile, "knOmOwned")) continue;
        html_dom_set_class(tile, "knOmOwned", owned);
        const HtmlElem badge = html_dom_query(tile, ".knOmBadge");
        if (owned && !badge) html_dom_insert_html(tile, HTML_INSERT_BEFORE_END, OM_BADGE_HTML);
        else if (!owned && badge) html_dom_remove(badge);
    }
    const HtmlElem clear = html_dom_by_id("knOmClearBtn");
    if (clear) {
        char label[48];
        om_clear_label(label, sizeof(label), me);
        html_dom_set_text(clear, label);
    }
}

// The Tools pane in place: the picked tool, its hint, the caps, the Tool Gun line
static void om_update_tools(void) {
    if (!om_window_open()) return;
    const HtmlElem pane = html_dom_by_id("knOmTools");
    if (!pane) return;
    html_dom_set_class(pane, "knOmOff", g_om.caps >= 0 && !(g_om.caps & OM_CAP_SPAWN));
    for (int i = 0; i < OM_TOOL_COUNT; i++) {
        char id[16];
        snprintf(id, sizeof(id), "knOmTool%d", i);
        const HtmlElem row = html_dom_by_id(id);
        if (row && html_dom_has_class(row, "knOmToolA") != (i == g_om.tool)) html_dom_set_class(row, "knOmToolA", i == g_om.tool);
    }
    const ObjectManagerToolInfo *t = object_manager_tool_info(g_om.tool);
    const HtmlElem hint = html_dom_by_id("knOmToolHint");
    if (hint && t) html_dom_set_text(hint, t->hint);
    const HtmlElem gun = html_dom_by_id("knOmToolGun");
    if (gun) html_dom_set_text(gun, om_tool_gun_line());
}

static void om_set_status(const char *text, const int error) {
    snprintf(g_om.status, sizeof(g_om.status), "%s", text ? text : "");
    g_om.status_error = error;
    g_om.status_at = glfwGetTime();
    if (om_window_open()) {
        om_update_status();
    } else if (text && text[0]) { // the Undo / Remove binds in game: a toast instead
        char html[400];
        char esc[200];
        om_escape(esc, sizeof(esc), text);
        snprintf(html, sizeof(html), "<span style='color:%s'>%s</span>", error ? "#eb5656" : "#fff", esc);
        ui_notify(html);
    }
}

// ---------------------------------------------------------------- open / close

static void om_show(void *user) {
    (void) user;
    menu_show_window(OBJECT_MANAGER_WINDOW, 1);
    om_send(OM_ACTION_QUERY, 0); // what may this player do here
}

void object_manager_ui_toggle(Client *client) {
    if (om_window_open()) {
        if (g_om.from_game && client && client->me) menu_request_open(0); // back to the game: pointer locked again
        else menu_show_window(MENU_WINDOW_NONE, 0);
        g_om.from_game = 0;
        return;
    }
    g_om.from_game = ui_controls_enabled(client);
    if (g_om.from_game) { // in game: free the pointer like pListKey, then the window
        ui_controls_reset_keys();
        client_set_menu_open(client, 1);
    }
    html_timer_add(0.0, om_show, NULL);
}

int object_manager_ui_escape(void) {
    if (!om_window_open()) return 0;
#ifdef __EMSCRIPTEN__
    // Web: Escape is no user activation, so a pointer lock asked for on it is refused (see
    // l_window_keydown): it only closes the window, like any other; a click or F1 resumes.
    return 0;
#endif
    Client *client = om_client();
    if (g_om.from_game && client && client->me) {
        g_om.from_game = 0;
        menu_request_open(0);
        return 1;
    }
    return 0;
}

int object_manager_ui_search_focused(void) {
    const HtmlElem search = om_window_open() ? html_dom_by_id("knOmSearchBox") : 0;
    return search && html_dom_active_element() == search;
}

// ---------------------------------------------------------------- handlers

static int arg_num(const HtmlArg *args, const int argc, const int i, const int def) {
    if (i >= argc) return def;
    if (args[i].kind == HTML_ARG_NUM) return (int) args[i].num;
    if (args[i].kind == HTML_ARG_STR && args[i].str) return atoi(args[i].str);
    return def;
}

static void om_expect_equip(const int weapon_id) {
    g_om.pending_equip = weapon_id;
    g_om.pending_at = glfwGetTime();
}

// The tools pane: pick `tool` and put the Tool Gun in hand (added as an extra slot first when it
// is not carried; the server refuses it without the spawn right)
static void om_pick_tool(const int tool) {
    if (tool < 0 || tool >= OM_TOOL_COUNT) return;
    g_om.tool = tool;
    char value[8];
    snprintf(value, sizeof(value), "%d", tool);
    html_storage_set(OM_TOOL_STORAGE, value);
    om_update_tools();
    const Client *client = om_client();
    const Player *me = client ? client->me : NULL;
    if (!me) {
        om_set_status("Join a game to use the Tool Gun", 1);
        return;
    }
    if (!om_server_ok(client)) {
        om_set_status("This server has no Object Manager", 1);
        return;
    }
    char text[96];
    snprintf(text, sizeof(text), "Tool: %s", object_manager_tool_info(tool)->name);
    for (int i = 0; me->loadout && i < me->loadout_size; i++) {
        if (me->loadout[i] != WEAPON_ID_TOOL_GUN) continue;
        if (i != me->loadout_index) ui_controls_select_slot(i);
        om_set_status(text, 0);
        return;
    }
    om_set_status(text, 0);
    om_expect_equip(WEAPON_ID_TOOL_GUN);
    om_send(OM_ACTION_ADD_WEAPON, WEAPON_ID_TOOL_GUN);
}

static void om_pick(const int tab, const int item, const int alt) {
    const Client *client = om_client();
    if (!client || !client->me) {
        om_set_status("Join a game to spawn things", 1);
        return;
    }
    if (!om_server_ok(client)) {
        om_set_status("This server has no Object Manager", 1);
        return;
    }
    if (tab == OM_TAB_NPCS || tab == OM_TAB_ENTITIES || tab == OM_TAB_OBJECTS) {
        int count = 0;
        object_manager_entries(tab, &count);
        const int action = tab == OM_TAB_NPCS ? OM_ACTION_SPAWN_NPC : tab == OM_TAB_ENTITIES ? OM_ACTION_SPAWN_ENTITY : OM_ACTION_SPAWN_OBJECT;
        if (item >= 0 && item < count) om_send(action, item);
        return;
    }
    if (tab == OM_TAB_WEAPONS) {
        if (!object_manager_weapon_listed(item)) return;
        if (alt) {
            om_send(OM_ACTION_DROP_WEAPON, item);
        } else if (om_has_extra(client->me, item)) {
            om_send(OM_ACTION_REMOVE_WEAPON, item);
        } else {
            om_expect_equip(item);
            om_send(OM_ACTION_ADD_WEAPON, item);
        }
    }
}

// A Weapons-tab click without the window (input scripts: `om add <id>` / `om drop <id>`)
void object_manager_ui_pick_weapon(const int weapon_id, const int drop) {
    om_pick(OM_TAB_WEAPONS, weapon_id, drop);
}

void object_manager_ui_pick_tool(const int tool) {
    om_load_tool();
    om_pick_tool(tool);
}

int object_manager_ui_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    if (!name || strncmp(name, "knOm", 4) != 0) return -1;
    if (!strcmp(name, "knOmTab")) {
        const int tab = arg_num(args, argc, 0, g_om.tab);
        if (tab >= 0 && tab < OM_TAB_COUNT && tab != g_om.tab) {
            g_om.tab = tab;
            char value[8];
            snprintf(value, sizeof(value), "%d", tab);
            html_storage_set(OM_TAB_STORAGE, value);
            menu_show_window(OBJECT_MANAGER_WINDOW, 1); // another grid: it starts at the top (v7 switchTab)
        }
        return 0;
    }
    if (!strcmp(name, "knOmSearch")) {
        char value[sizeof(g_om.search)] = "";
        const int text = argc > 0 && args[0].kind == HTML_ARG_STR && args[0].str; // knOmSearch("rifle"): scenarios, the console
        if (text) snprintf(value, sizeof(value), "%s", args[0].str);
        else if (ev && ev->target) html_dom_get_value(ev->target, value, sizeof(value));
        else if (ev && ev->value) snprintf(value, sizeof(value), "%s", ev->value);
        snprintf(g_om.search, sizeof(g_om.search), "%s", value);
        if (text) menu_show_window(OBJECT_MANAGER_WINDOW, 1); // the box shows it too
        else om_refresh_body();                               // typing: keep the box (focus, caret)
        return 1;
    }
    if (!strcmp(name, "knOmPick")) {
        om_pick(arg_num(args, argc, 0, -1), arg_num(args, argc, 1, -1), 0);
        return 0;
    }
    if (!strcmp(name, "knOmAlt")) {
        om_pick(arg_num(args, argc, 0, -1), arg_num(args, argc, 1, -1), 1);
        return 0;
    }
    if (!strcmp(name, "knOmUndo")) {
        om_send(OM_ACTION_UNDO, 0);
        return 0;
    }
    if (!strcmp(name, "knOmRemove")) { // what the crosshair is on (the view the window was opened over)
        om_send(OM_ACTION_REMOVE_AIM, 0);
        return 0;
    }
    if (!strcmp(name, "knOmTool")) {
        om_pick_tool(arg_num(args, argc, 0, -1));
        return 0;
    }
    if (!strcmp(name, "knOmClear")) {
        om_send(OM_ACTION_CLEAR_WEAPONS, 0);
        return 0;
    }
    return -1;
}

// ---------------------------------------------------------------- packets

static void om_on_result(const NetSpawnResultPacket *r) {
    printf("[object-manager] action %u item %d: %s (caps %u)\n", r->action, r->item, r->status ? object_manager_status_text(r->status) : "ok", r->caps);
    g_om.caps = r->caps;
    g_om.extra_max = r->extra_max;
    if (r->action == OM_ACTION_QUERY) {
        om_update_status();
        om_update_tools();
        return;
    }
    if (r->status != OM_STATUS_OK) {
        if (r->action == OM_ACTION_ADD_WEAPON) g_om.pending_equip = -1;
        char text[160];
        if (r->status == OM_STATUS_LIMIT && r->limit) snprintf(text, sizeof(text), "%s (%u)", object_manager_status_text(r->status), r->limit);
        else snprintf(text, sizeof(text), "%s", object_manager_status_text(r->status));
        om_set_status(text, 1);
        return;
    }
    const Weapon *w = r->item >= 0 && r->item < WEAPON_TABLE_COUNT && r->action != OM_ACTION_TOOL ? g_weapons[r->item] : NULL;
    char text[160] = "";
    switch (r->action) {
        case OM_ACTION_ADD_WEAPON: snprintf(text, sizeof(text), "%s added to your loadout", w ? w->name : "Weapon"); break;
        case OM_ACTION_REMOVE_WEAPON: snprintf(text, sizeof(text), "%s taken out of your loadout", w ? w->name : "Weapon"); break;
        case OM_ACTION_CLEAR_WEAPONS: snprintf(text, sizeof(text), "Extra weapons cleared"); break;
        case OM_ACTION_DROP_WEAPON: snprintf(text, sizeof(text), "%s dropped at your crosshair", w ? w->name : "Weapon"); break;
        case OM_ACTION_UNDO: snprintf(text, sizeof(text), "Undone"); break;
        case OM_ACTION_REMOVE_AIM: snprintf(text, sizeof(text), "Removed"); break;
        case OM_ACTION_DROP_HELD:
            if (r->item == WEAPON_ID_TOOL_GUN) snprintf(text, sizeof(text), "Tool Gun put away");
            else snprintf(text, sizeof(text), "%s dropped", w ? w->name : "Weapon");
            break;
        case OM_ACTION_TOOL:
            if (r->limit == OM_TOOL_SHOWN) return; // Info: the NOTIFY carries the text
            if (r->limit == OM_TOOL_COPIED) snprintf(text, sizeof(text), "Copied: shoot a surface to place the copy");
            else if (r->limit == OM_TOOL_PASTED) snprintf(text, sizeof(text), "Copy placed");
            else if (r->item == OM_TOOL_ROTATOR) snprintf(text, sizeof(text), "Rotated");
            else snprintf(text, sizeof(text), "Removed");
            break;
        case OM_ACTION_SPAWN_NPC:
        case OM_ACTION_SPAWN_ENTITY:
        case OM_ACTION_SPAWN_OBJECT: {
            int count = 0;
            const int tab = r->action == OM_ACTION_SPAWN_NPC ? OM_TAB_NPCS : r->action == OM_ACTION_SPAWN_ENTITY ? OM_TAB_ENTITIES : OM_TAB_OBJECTS;
            const ObjectManagerEntry *entries = object_manager_entries(tab, &count);
            snprintf(text, sizeof(text), "%s spawned at your crosshair", entries && r->item >= 0 && r->item < count ? entries[r->item].name : "It");
            break;
        }
        default: snprintf(text, sizeof(text), "Done"); break;
    }
    om_set_status(text, 0);
}

static void om_on_extras(Client *client, const NetLoadoutExtraPacket *packet) {
    Player *player = client_find_player(client, packet->uid);
    if (!player) return;
    int ids[PLAYER_EXTRA_WEAPONS_MAX];
    const int count = packet->count < PLAYER_EXTRA_WEAPONS_MAX ? packet->count : PLAYER_EXTRA_WEAPONS_MAX;
    for (int i = 0; i < count; i++) ids[i] = packet->weapons[i];

    // the arms are built per loadout slot (client_drop_player_mesh), as hud_modes_apply_loadout
    const int is_you = player == client->me;
    const int had_mesh = player->mesh != NULL;
    if (had_mesh) {
        scene_remove_player_mesh(player->render_you ? client->fps_scene : client->scene, player->mesh, player->loadout_size);
        player_meshes_fini(player);
    }
    player_set_extra_weapons(player, ids, count);
    if (had_mesh) client_add_player_mesh(client, player, is_you);

    if (is_you) {
        if (g_om.pending_equip >= 0 && glfwGetTime() - g_om.pending_at > OM_EQUIP_SECONDS) g_om.pending_equip = -1; // a refused give
        if (g_om.pending_equip >= 0) { // the weapon this client just added: in hand with the next input frame
            for (int i = player_loadout_base(player); i < player->loadout_size; i++) {
                if (player->loadout[i] == g_om.pending_equip) ui_controls_select_slot(i);
            }
            g_om.pending_equip = -1;
        }
        om_update_loadout(); // owned marks, the extras count (in place: the grid keeps its scroll)
        om_update_tools();   // the Tools pane's Tool Gun line
    }
}

int object_manager_events_handle(Client *client, const int type, const void *payload, const uint16_t length) {
    switch (type) {
        case NET_PACKET_SPAWN_RESULT: {
            NetSpawnResultPacket r;
            if (length != sizeof(r)) return 1;
            memcpy(&r, payload, sizeof(r));
            om_on_result(&r);
            return 1;
        }
        case NET_PACKET_LOADOUT_EXTRA: {
            NetLoadoutExtraPacket packet;
            if (length != sizeof(packet)) return 1;
            memcpy(&packet, payload, sizeof(packet));
            om_on_extras(client, &packet);
            return 1;
        }
        case NET_PACKET_SPAWNED_OBJ:
            object_manager_apply_spawned(&client->game, payload, length);
            return 1;
        default:
            return 0;
    }
}

// Per frame: the local player's Build Tool cost waiver (Player.build_free, doc/v7/decisions.md
// "Build Tool cost") follows this server's OM_CAP_SPAWN, as the authority's does; with the caps
// unknown, holding a Build Tool asks once (OM_ACTION_QUERY) so the wheel and helper are right.
// A Tool Gun shot of the local player: its slot's fire timer jumped (player_shoot ran in the last
// tick; shots are no shared/ events, vfx.c detect_local_shot watches the same) -> OM_ACTION_TOOL
static void om_tool_shots(Client *client, const Player *me) {
    const int slot = me->active && me->reloads && me->loadout_index >= 0 && me->loadout_index < me->loadout_size ? me->loadout_index : -1;
    const float reload = slot >= 0 ? me->reloads[slot] : 0.0f;
    const int same = g_om.shot_me == me && g_om.shot_slot == slot && slot >= 0;
    if (same && me->weapon && me->weapon->kn_tool && reload > g_om.shot_reload + 1e-4f && om_server_ok(client)) {
        om_load_tool();
        om_send(OM_ACTION_TOOL, g_om.tool);
    }
    g_om.shot_me = me;
    g_om.shot_slot = slot;
    g_om.shot_reload = reload;
}

// "TOOL: DELETER" beside the ammo counter while the Tool Gun is in hand (#knToolHud, made on first
// use next to #ammoHolder so the HUD markup stays v7's)
static void om_tool_hud(const Player *me) {
    char text[sizeof(g_om.hud_text)] = "";
    if (me && me->active && me->weapon && me->weapon->kn_tool) {
        om_load_tool();
        const ObjectManagerToolInfo *t = object_manager_tool_info(g_om.tool);
        snprintf(text, sizeof(text), "TOOL: %s", t ? t->name : "?");
        for (char *c = text; *c; c++) *c = (char) (*c >= 'a' && *c <= 'z' ? *c - 32 : *c);
    }
    HtmlElem hud = html_dom_by_id("knToolHud");
    if (!strcmp(text, g_om.hud_text) && (hud || !text[0])) return;
    if (!hud && text[0]) {
        const HtmlElem ammo = html_dom_by_id("ammoHolder");
        if (!ammo) return;
        html_dom_insert_html(ammo, HTML_INSERT_BEFORE_BEGIN, "<div id='knToolHud' class='knToolHud'></div>");
        hud = html_dom_by_id("knToolHud");
        if (!hud) return;
    }
    if (hud) {
        html_dom_set_text(hud, text);
        html_dom_set_class(hud, "knToolHudOn", text[0] != 0);
    }
    snprintf(g_om.hud_text, sizeof(g_om.hud_text), "%s", text);
    om_update_tools(); // the pane's Tool Gun line follows the hand
}

void object_manager_ui_tick(Client *client) {
    Player *me = client ? client->me : NULL;
    om_tool_hud(me);
    if (!me) return;
    om_tool_shots(client, me);
    if (g_om.caps < 0) {
        me->build_free = 0;
        if (!g_om.queried && me->active && me->weapon && me->weapon->build && om_server_ok(client)) {
            g_om.queried = 1;
            om_send(OM_ACTION_QUERY, 0);
        }
        return;
    }
    me->build_free = (g_om.caps & OM_CAP_SPAWN) != 0;
}

void object_manager_ui_key(Client *client, const int action) {
    if (!client || !client->me || !client->me->active || !om_server_ok(client)) return;
    om_send(action, 0);
}

// A chat / console line went out: `/give <weapon>` to yourself equips the weapon when its
// LOADOUT_EXTRA lands, like the Weapons tab's left click (the server answers in the chat)
void object_manager_ui_chat_sent(const char *text) {
    if (!text || strncasecmp(text, "/give ", 6) != 0) return;
    const Client *client = om_client();
    const Player *me = client ? client->me : NULL;
    int id = -1;
    if (me && object_manager_give_lookup(text + 6, &id, NULL) == OM_GIVE_WEAPON) om_expect_equip(id);
}

// A new connection / map: the caps belong to the server we talked to.
void object_manager_ui_reset(void) {
    g_om.caps = -1;
    g_om.queried = 0;
    g_om.pending_equip = -1;
    g_om.status[0] = 0;
}
