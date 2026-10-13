// KrunkNative Object Manager (F1): a Garry's Mod style spawn menu on the HTML UI. Not a Krunker
// v7.1.4 window (doc/v7/decisions.md "Object Manager"). The rules and the authority live in
// shared/src/object_manager.c; this is the window (id OBJECT_MANAGER_WINDOW), its handlers, the
// F1 / Escape toggle and the client side of packets 250-253.
//
// Window: tabs Objects / Entities / NPCs / Weapons over a searchable grid of icons. Weapons:
// left click adds the weapon to the loadout as an extra slot (again: takes it out), right click
// drops it as a pickup at the crosshair. Opening it in game frees the pointer (like the player
// list key); F1 or Escape closes it and gives the pointer back to the game.
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

#define OM_TAB_STORAGE "kn_omTab"
#define OM_STATUS_SECONDS 4.0

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
} g_om = {.tab = OM_TAB_WEAPONS, .caps = -1, .pending_equip = -1};

static Client *om_client(void) { return menu_ui()->client; }

static int om_window_open(void) {
    return menu_ui()->active_window == OBJECT_MANAGER_WINDOW && menu_window_is_open();
}

static void om_load_tab(void) {
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
static void om_status_html(char **buf, size_t *len, size_t *cap) {
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
        for (int id = 0; id < WEAPON_COUNT; id++) {
            if (!object_manager_weapon_listed(id)) continue;
            const Weapon *w = g_weapons[id];
            if (!om_matches(w->name)) continue;
            char name[96];
            om_escape(name, sizeof(name), w->name);
            const int owned = om_has_extra(me, id);
            om_append(buf, len, cap,
                      "<div class='knOmItem%s' onclick='knOmPick(%d,%d)' oncontextmenu='knOmAlt(%d,%d)'>"
                      "<img class='knOmIcon' src='textures/weapons/%s.png'/><div class='knOmName'>%s</div>%s</div>",
                      owned ? " knOmOwned" : "", OM_TAB_WEAPONS, id, OM_TAB_WEAPONS, id, w->icon ? w->icon : "icon_0", name,
                      owned ? "<div class='knOmBadge'>LOADOUT</div>" : "");
            shown++;
        }
    } else {
        int count = 0;
        const ObjectManagerEntry *entries = object_manager_entries(g_om.tab, &count);
        for (int i = 0; entries && i < count; i++) {
            if (!om_matches(entries[i].name)) continue;
            char name[96];
            om_escape(name, sizeof(name), entries[i].name);
            om_append(buf, len, cap,
                      "<div class='knOmItem' onclick='knOmPick(%d,%d)'><div class='knOmGlyph'><span class='material-icons'>%s</span></div>"
                      "<div class='knOmName'>%s</div></div>",
                      g_om.tab, i, entries[i].icon ? entries[i].icon : "help", name);
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
        case OM_TAB_NPCS: return "Left click: spawn at your crosshair | Undo takes back your last spawn";
        case OM_TAB_ENTITIES: return "Left click: spawn at your crosshair (power-ups despawn on their own timer)";
        default: return "Left click: spawn at your crosshair";
    }
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
        om_append(&buf, &len, &cap, "<div class='bigMenTab%s' onclick='knOmTab(%d)'>%s</div>", i == g_om.tab ? " bigMenTabA" : "", i, TAB_NAMES[i]);
    }
    om_append(&buf, &len, &cap,
              "</div><input id='knOmSearch' class='knOmSearch' type='text' maxlength='40' placeholder='Search' value='%s' "
              "oninput='knOmSearch(this)'/></div><div id='knOmBody'>",
              search);
    char *body = om_body_html();
    om_append(&buf, &len, &cap, "%s</div>", body ? body : "");
    free(body);
    om_append(&buf, &len, &cap, "<div class='knOmFoot'><div class='knOmHint'>%s</div><div class='knOmBtns'>", om_hint());
    if (g_om.tab == OM_TAB_WEAPONS) {
        om_append(&buf, &len, &cap, "<div class='button small buttonR knOmBtn' onclick='knOmClear()'>Clear Extras (%d/%d)</div>",
                  me ? me->extra_weapon_count : 0, PLAYER_EXTRA_WEAPONS_MAX);
    }
    om_append(&buf, &len, &cap, "<div class='button small knOmBtn' onclick='knOmUndo()'>Undo</div></div></div></div>");
    return buf;
}

// Rebuilds the status + grid only (the search box keeps its focus and caret).
static void om_refresh_body(void) {
    if (!om_window_open()) return;
    const HtmlElem body = html_dom_by_id("knOmBody");
    if (!body) return;
    char *html = om_body_html();
    html_dom_set_html(body, html ? html : "");
    free(html);
}

static void om_set_status(const char *text, const int error) {
    snprintf(g_om.status, sizeof(g_om.status), "%s", text ? text : "");
    g_om.status_error = error;
    g_om.status_at = glfwGetTime();
    om_refresh_body();
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
    const HtmlElem search = om_window_open() ? html_dom_by_id("knOmSearch") : 0;
    return search && html_dom_active_element() == search;
}

// ---------------------------------------------------------------- handlers

static int arg_num(const HtmlArg *args, const int argc, const int i, const int def) {
    if (i >= argc) return def;
    if (args[i].kind == HTML_ARG_NUM) return (int) args[i].num;
    if (args[i].kind == HTML_ARG_STR && args[i].str) return atoi(args[i].str);
    return def;
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
    if (tab == OM_TAB_NPCS || tab == OM_TAB_ENTITIES) {
        int count = 0;
        object_manager_entries(tab, &count);
        if (item >= 0 && item < count) om_send(tab == OM_TAB_NPCS ? OM_ACTION_SPAWN_NPC : OM_ACTION_SPAWN_ENTITY, item);
        return;
    }
    if (tab == OM_TAB_WEAPONS) {
        if (!object_manager_weapon_listed(item)) return;
        if (alt) {
            om_send(OM_ACTION_DROP_WEAPON, item);
        } else if (om_has_extra(client->me, item)) {
            om_send(OM_ACTION_REMOVE_WEAPON, item);
        } else {
            g_om.pending_equip = item;
            om_send(OM_ACTION_ADD_WEAPON, item);
        }
    }
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
            menu_show_window(OBJECT_MANAGER_WINDOW, 1);
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
        om_refresh_body();
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
    const Weapon *w = r->item >= 0 && r->item < WEAPON_COUNT ? g_weapons[r->item] : NULL;
    char text[160] = "";
    switch (r->action) {
        case OM_ACTION_ADD_WEAPON: snprintf(text, sizeof(text), "%s added to your loadout", w ? w->name : "Weapon"); break;
        case OM_ACTION_REMOVE_WEAPON: snprintf(text, sizeof(text), "%s taken out of your loadout", w ? w->name : "Weapon"); break;
        case OM_ACTION_CLEAR_WEAPONS: snprintf(text, sizeof(text), "Extra weapons cleared"); break;
        case OM_ACTION_DROP_WEAPON: snprintf(text, sizeof(text), "%s dropped at your crosshair", w ? w->name : "Weapon"); break;
        case OM_ACTION_UNDO: snprintf(text, sizeof(text), "Undone"); break;
        case OM_ACTION_SPAWN_NPC:
        case OM_ACTION_SPAWN_ENTITY: {
            int count = 0;
            const ObjectManagerEntry *entries = object_manager_entries(r->action == OM_ACTION_SPAWN_NPC ? OM_TAB_NPCS : OM_TAB_ENTITIES, &count);
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
        if (g_om.pending_equip >= 0) { // the weapon this client just added: in hand with the next input frame
            for (int i = player_loadout_base(player); i < player->loadout_size; i++) {
                if (player->loadout[i] == g_om.pending_equip) ui_controls_select_slot(i);
            }
            g_om.pending_equip = -1;
        }
        if (om_window_open()) menu_show_window(OBJECT_MANAGER_WINDOW, 1); // owned marks, the extras count
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

// A new connection / map: the caps belong to the server we talked to.
void object_manager_ui_reset(void) {
    g_om.caps = -1;
    g_om.pending_equip = -1;
    g_om.status[0] = 0;
}
