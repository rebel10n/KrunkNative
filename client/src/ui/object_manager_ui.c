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
// Building (client/src/ui/om_build.c): the pane's two gun tiles (the weapons' own icons) put the
// Tool Gun (with the picked tool) or the Physics Gun in hand at once: its slot when carried, else
// added as an extra slot and equipped when it lands, even with the window open. Move and Resize are
// Tool Gun tools driven by handles (no OM_ACTION_TOOL shots); the Snap row takes any grid step
// (OB_SNAP_MIN..OB_SNAP_MAX, Off; the PageDown / PageUp keys and the console's `tool_snap` too), the Release row
// the Physics Gun's let-go (Freeze / Drop). Devs and the host (OM_CAP_MAP) change the map's own
// objects with both guns as well; the pane says so.
//
// Properties (the Tool Gun's Properties tool, or its key on the selection: om_build.c): the window
// opens over the game with the object's map fields in place of the grid (#knOmPropsPane): the
// fields of object_manager_field that apply to its prefab, grouped as the map editor's inspector,
// with the values the game reads from the client's copy of its record. A change (Enter / leaving a
// field, a switch, a select) goes out at once as one field patch (OM_EDIT_FIELDS, one undo step);
// the authority's answer and every SPAWNED_OBJ of that object redraw the values. Back or a tab goes
// back to the grid.
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
#include <om_interp.h> // other players' edits eased (SPAWNED_OBJ)
#include <spectate.h> // client_find_player
#include <weapons.h>
#include <GLFW/glfw3.h> // glfwGetTime: the status line's age
#include <prefabs.h> // g_prefab_textures, prefab_name (the Properties panel)
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> // strncasecmp
#include <ruleset.h>

#define OM_TAB_STORAGE "kn_omTab"
#define OM_TOOL_STORAGE "kn_omTool"
#define OM_EQUIP_SECONDS 3.0 // a pending equip (pick / give) waits this long for its LOADOUT_EXTRA
#define OM_STATUS_SECONDS 4.0
#define OM_BADGE_HTML "<div class='knOmBadge'>LOADOUT</div>" // on a weapon tile in the loadout (knOmOwned)

// the tab labels, through i18n at render time (kn.om.tab.*)
static const char *tab_name(const int tab) {
    switch (tab) {
        case 0: return I18N("kn.om.tab.objects", "Objects");
        case 1: return I18N("kn.om.tab.entities", "Entities");
        case 2: return I18N("kn.om.tab.npcs", "NPCs");
        default: return I18N("kn.om.tab.weapons", "Weapons");
    }
}

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
    int props_uid;        // the object the Properties panel shows, -1 none (the grid)
    char props_sent[48];  // the label of the field patch on its way
} g_om = {.tab = OM_TAB_WEAPONS, .caps = -1, .pending_equip = -1, .props_uid = -1};

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
    // the crosshair: the view the menu was opened over; not spawned in (a dev in the menu), the
    // camera's look (the server places from its view of this connection, object_manager_request_view)
    const int alive = me && me->active;
    req.pitch = alive ? me->direction.x : client->camera.rotation.x;
    req.yaw = alive ? me->direction.y : client->camera.rotation.y;
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
        text = I18N("kn.om.status.join", "Join a game to spawn things");
    } else if (!om_server_ok(client)) {
        text = I18N("kn.om.status.noServer", "This server has no Object Manager");
        error = 1;
    } else if (g_om.caps >= 0 && g_om.tab == OM_TAB_WEAPONS && !(g_om.caps & OM_CAP_WEAPONS)) {
        text = I18N("kn.om.status.noExtras", "Extra weapons are off in this game");
        error = 1;
    } else if (g_om.caps >= 0 && g_om.tab != OM_TAB_WEAPONS && !(g_om.caps & OM_CAP_SPAWN)) {
        text = I18N("kn.om.status.hostOnly", "Only developers and the host can spawn here");
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
            const Weapon *w = rules_weapon(id);
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
        om_append(buf, len, cap, "<div class='knOmEmpty'>%s</div>", g_om.search[0] ? I18N("kn.om.noMatches", "No matches")
                                                                                     : I18N("kn.om.nothing", "Nothing to spawn here yet"));
    }
    om_append(buf, len, cap, "</div>");
}

static const char *om_hint(void) {
    if (g_om.props_uid >= 0) return I18N("kn.om.hint.props", "Enter or leave a field to apply it: each change is one undo step (U) | Back: the grid");
    switch (g_om.tab) {
        case OM_TAB_WEAPONS: return I18N("kn.om.hint.weapons", "Left click: add to your loadout (again: take it out) | Right click: drop it at your crosshair");
        case OM_TAB_OBJECTS: return I18N("kn.om.hint.objects", "Left click: place it on the surface at your crosshair, facing you | U: undo, Delete: remove at the crosshair");
        case OM_TAB_NPCS: return I18N("kn.om.hint.npcs", "Left click: spawn at your crosshair | U: undo, Delete: remove at the crosshair");
        case OM_TAB_ENTITIES: return I18N("kn.om.hint.entities", "Left click: spawn at your crosshair (power-ups despawn on their own timer)");
        default: return I18N("kn.om.hint.spawn", "Left click: spawn at your crosshair");
    }
}

static const char *om_tool_gun_line(void) {
    const Client *client = om_client();
    const Player *me = client ? client->me : NULL;
    if (g_om.caps >= 0 && !(g_om.caps & OM_CAP_SPAWN)) return I18N("kn.om.gun.hostOnly", "Only developers and the host can use the Tool Gun here");
    const int map = g_om.caps >= 0 && (g_om.caps & OM_CAP_MAP);
    if (me && me->weapon && me->weapon->kn_tool)
        return map ? I18N("kn.om.gun.toolMap", "Tool Gun in hand: works on the map too") : I18N("kn.om.gun.toolHand", "Tool Gun in hand: shoot to use");
    if (me && me->weapon && me->weapon->kn_physgun)
        return map ? I18N("kn.om.gun.physMap", "Physics Gun in hand: works on the map too") : I18N("kn.om.gun.physHand", "Physics Gun in hand");
    for (int i = 0; me && me->loadout && i < me->loadout_size; i++) {
        if (me->loadout[i] == WEAPON_ID_TOOL_GUN) return I18N("kn.om.gun.toolLoadout", "Tool Gun in your loadout");
    }
    return I18N("kn.om.gun.pick", "Pick a tool to equip the Tool Gun");
}

int object_manager_ui_caps(void) { return g_om.caps > 0 ? g_om.caps : 0; }

// A gun tile of the Tools pane: the weapon's own icon (textures/weapons), in hand marked
static void om_gun_tile(char **buf, size_t *len, size_t *cap, const char *id, const char *handler, const int weapon_id, const int held) {
    const Weapon *w = rules_weapon(weapon_id);
    om_append(buf, len, cap,
              "<div id='%s' class='knOmGun%s' onclick='%s'><img class='knOmGunIcon' src='textures/weapons/%s.png'/>"
              "<span class='knOmGunName'>%s</span></div>",
              id, held ? " knOmToolA" : "", handler, w && w->icon ? w->icon : "icon_0", w ? w->name : "?");
}

static int om_holds(const int weapon_id) {
    const Client *client = om_client();
    const Player *me = client ? client->me : NULL;
    return me && me->active && me->loadout && me->loadout_index >= 0 && me->loadout_index < me->loadout_size && me->loadout[me->loadout_index] == weapon_id;
}

// The right pane: the tools (the picked one marked), its hint, and whether the Tool Gun is in hand
static void om_tools_html(char **buf, size_t *len, size_t *cap) {
    const int allowed = g_om.caps < 0 || (g_om.caps & OM_CAP_SPAWN);
    om_append(buf, len, cap, "<div id='knOmTools' class='knOmTools%s'><div class='knOmToolsTitle'>%s</div><div class='knOmGuns'>", allowed ? "" : " knOmOff",
              I18N("kn.om.tools", "Tools"));
    // not id='knOmPhys' / 'knOmGun': on the web window.<id> would be the element (named access, as knOmSearchBox)
    om_gun_tile(buf, len, cap, "knOmToolGunTile", "knOmToolGunPick()", WEAPON_ID_TOOL_GUN, om_holds(WEAPON_ID_TOOL_GUN));
    om_gun_tile(buf, len, cap, "knOmPhysRow", "knOmPhys()", WEAPON_ID_PHYS_GUN, om_holds(WEAPON_ID_PHYS_GUN));
    om_append(buf, len, cap, "</div>");
    for (int i = 0; i < OM_TOOL_COUNT; i++) {
        const ObjectManagerToolInfo *t = object_manager_tool_info(i);
        om_append(buf, len, cap,
                  "<div id='knOmTool%d' class='knOmTool%s' onclick='knOmTool(%d)'><span class='material-icons'>%s</span>"
                  "<span class='knOmToolName'>%s</span></div>",
                  i, i == g_om.tool ? " knOmToolA" : "", i, t->icon, t->name);
    }
    char snap[16];
    om_build_snap_text(snap, sizeof(snap));
    // the grid step: any value (Enter / leaving the field applies it), Off, and PageDown / PageUp in game
    om_append(buf, len, cap,
              "<div class='knOmOpt'><span class='knOmOptName'>%s</span><input id='knOmSnapBox' class='knOmSnapBox' type='text' maxlength='6' "
              "value='%s' onchange='knOmSnapSet(this.value)'/><span id='knOmSnapOff' class='knOmOptBtn%s' onclick='knOmSnapSet(0)'>%s</span></div>",
              I18N("kn.om.snap", "Snap"), om_build_snap() > 0.0f ? snap : "", om_build_snap() > 0.0f ? "" : " knOmOptA", I18N("kn.om.off", "Off"));
    om_append(buf, len, cap, "<div class='knOmOpt'><span class='knOmOptName'>%s</span>"
                             "<span id='knOmDrop0' class='knOmOptBtn%s' onclick='knOmPhysDrop(0)'>%s</span>"
                             "<span id='knOmDrop1' class='knOmOptBtn%s' onclick='knOmPhysDrop(1)'>%s</span></div>",
              I18N("kn.om.release", "Release"), om_build_drop() ? "" : " knOmOptA", I18N("kn.om.freeze", "Freeze"),
              om_build_drop() ? " knOmOptA" : "", I18N("kn.om.drop", "Drop"));
    const ObjectManagerToolInfo *t = object_manager_tool_info(g_om.tool);
    om_append(buf, len, cap, "<div id='knOmToolHint' class='knOmToolHint'>%s</div>", t ? t->hint : "");
    om_append(buf, len, cap, "<div id='knOmToolGun' class='knOmToolGun'>%s</div></div>", om_tool_gun_line());
}

static void om_clear_label(char *out, const size_t size, const Player *me) {
    char have[16], max[16];
    snprintf(have, sizeof(have), "%d", me ? me->extra_weapon_count : 0);
    snprintf(max, sizeof(max), "%d", PLAYER_EXTRA_WEAPONS_MAX);
    i18n_trf(out, size, "kn.om.clearExtras", "Clear Extras ({0}/{1})", 2, I18N_ARGV(have, max));
}

// ---------------------------------------------------------------- the Properties panel

// The object the panel shows (the client's copy), NULL when it is gone
static const Object *om_props_object(void) {
    const Client *client = om_client();
    const Game *game = client ? &client->game : NULL;
    if (!game || !game->map || g_om.props_uid < 0 || (size_t) g_om.props_uid >= game->map->object_count) return NULL;
    const Object *o = game->map->objects[g_om.props_uid];
    return o->raw && object_manager_editable(game, g_om.props_uid) ? o : NULL;
}

static void om_title_case(char *out, const size_t size, const char *name) {
    size_t n = 0;
    int start = 1;
    for (const char *c = name ? name : "?"; *c && n + 1 < size; c++) {
        const char ch = *c == '_' ? ' ' : *c;
        out[n++] = (char) (start ? ch : (ch >= 'A' && ch <= 'Z' ? ch + 32 : ch));
        start = ch == ' ';
    }
    out[n] = 0;
}

static void om_number_text(char *out, const size_t size, const double v) {
    snprintf(out, size, "%.10g", round(v * 1000.0) / 1000.0);
}

static void om_prop_input(char **buf, size_t *len, size_t *cap, const int index, const int axis, const char *value) {
    char esc[64];
    om_escape(esc, sizeof(esc), value);
    if (axis < 0)
        om_append(buf, len, cap, "<input id='knOmF%d' class='knOmPropIn' type='text' maxlength='12' value='%s' onchange='knOmField(%d,this.value)'/>",
                  index, esc, index);
    else
        om_append(buf, len, cap, "<input id='knOmF%d_%d' class='knOmPropIn knOmPropVec' type='text' maxlength='10' value='%s' "
                  "onchange='knOmFieldVec(%d,%d,this.value)'/>", index, axis, esc, index, axis);
}

// One field's row: the inspector's label, the control, the range
static void om_prop_row(char **buf, size_t *len, size_t *cap, const int index, const ObjectManagerField *f, const int prefab, const cJSON *value) {
    om_append(buf, len, cap, "<div class='knOmProp'><span class='knOmPropName'>%s</span><span class='knOmPropVal'>", f->label);
    float mn, mx;
    object_manager_field_range(f, prefab, &mn, &mx);
    char text[48];
    switch (f->kind) {
        case OM_FIELD_ON:
        case OM_FIELD_OFF: {
            const int on = cJSON_IsTrue(value);
            om_append(buf, len, cap, "<span id='knOmF%d_1' class='knOmOptBtn%s' onclick='knOmField(%d,1)'>%s</span>"
                      "<span id='knOmF%d_0' class='knOmOptBtn%s' onclick='knOmField(%d,0)'>%s</span>",
                      index, on ? " knOmOptA" : "", index, I18N("kn.om.on", "On"), index, on ? "" : " knOmOptA", index, I18N("kn.om.off", "Off"));
            break;
        }
        case OM_FIELD_SELECT:
        case OM_FIELD_TEXTURE: {
            const int current = (int) cJSON_GetNumberValue(value);
            om_append(buf, len, cap, "<select id='knOmF%d' class='knOmPropSel' onchange='knOmField(%d,this.value)'>", index, index);
            for (int i = (int) mn; i <= (int) mx; i++) {
                const char *label = f->kind == OM_FIELD_TEXTURE ? (g_prefab_textures[i].alias ? g_prefab_textures[i].alias : g_prefab_textures[i].name)
                                                                : object_manager_field_option(f, i);
                char name[64], esc[96];
                if (f->kind == OM_FIELD_TEXTURE) om_title_case(name, sizeof(name), label);
                else snprintf(name, sizeof(name), "%s", label ? label : "?");
                om_escape(esc, sizeof(esc), name);
                om_append(buf, len, cap, "<option value='%d'%s>%s</option>", i, i == current ? " selected" : "", esc);
            }
            om_append(buf, len, cap, "</select>");
            break;
        }
        case OM_FIELD_COLOR: {
            const char *c = cJSON_GetStringValue(value);
            char esc[32];
            om_escape(esc, sizeof(esc), c ? c : "#ffffff");
            om_append(buf, len, cap, "<span class='knOmSwatch' style='background:%s'></span>", esc);
            om_prop_input(buf, len, cap, index, -1, esc);
            break;
        }
        case OM_FIELD_VEC3:
            for (int k = 0; k < 3; k++) {
                om_number_text(text, sizeof(text), cJSON_GetNumberValue(cJSON_GetArrayItem(value, k)));
                om_prop_input(buf, len, cap, index, k, text);
            }
            break;
        default:
            om_number_text(text, sizeof(text), cJSON_GetNumberValue(value));
            om_prop_input(buf, len, cap, index, -1, text);
            break;
    }
    if (f->kind == OM_FIELD_NUM || f->kind == OM_FIELD_NUMN || f->kind == OM_FIELD_INT) {
        char lo[24], hi[24];
        om_number_text(lo, sizeof(lo), mn);
        om_number_text(hi, sizeof(hi), mx);
        char range[96];
        I18N_F(range, "kn.om.range", "{0} to {1}", lo, hi);
        om_append(buf, len, cap, "<span class='knOmPropRange'>%s</span>", range);
    } else if (f->kind == OM_FIELD_VEC3 && !strcmp(f->key, "r")) {
        om_append(buf, len, cap, "<span class='knOmPropRange'>%s</span>", I18N("kn.om.radians", "radians"));
    }
    om_append(buf, len, cap, "</span></div>");
}

static void om_props_html(char **buf, size_t *len, size_t *cap) {
    const Client *client = om_client();
    const Object *o = om_props_object();
    om_append(buf, len, cap, "<div id='knOmPropsPane' class='knOmProps'><div class='knOmPropsHead'>");
    if (!o) {
        om_append(buf, len, cap, "<span class='knOmPropsTitle'>%s</span><span class='knOmPropsSub'>%s</span>"
                                 "<span id='knOmPropsBack' class='knOmOptBtn' onclick='knOmPropsClose()'>%s</span></div></div>",
                  I18N("kn.om.props.gone", "Gone"), I18N("kn.om.props.goneSub", "Nothing to edit: the object went or may not change"),
                  I18N("generic.back", "Back"));
        return;
    }
    char title[64];
    om_title_case(title, sizeof(title), prefab_name(o->prefab));
    const int map = object_manager_is_map_object(&client->game, g_om.props_uid);
    char owner[96];
    snprintf(owner, sizeof(owner), "%s", I18N("kn.om.props.mapObject", "map object"));
    if (!map) {
        const Player *who = client_find_player((Client *) client, o->spawn_owner);
        char esc[64];
        om_escape(esc, sizeof(esc), who ? who->name : "?");
        I18N_F(owner, "kn.om.props.spawnedBy", "spawned by {0}", esc);
    }
    om_append(buf, len, cap, "<span class='knOmPropsTitle'>%s</span><span class='knOmPropsSub'>#%d, %s</span>"
                             "<span id='knOmPropsBack' class='knOmOptBtn' onclick='knOmPropsClose()'>%s</span></div><div id='knOmPropsList' class='knOmPropsList'>",
              title, g_om.props_uid, owner, I18N("generic.back", "Back"));
    const char *section = NULL;
    int shown = 0;
    cJSON *record = object_manager_record(&client->game, g_om.props_uid); // (the map's palette colour resolved)
    for (int i = 0; record && i < object_manager_field_count(); i++) {
        const ObjectManagerField *f = object_manager_field(i);
        cJSON *value = object_manager_field_value(record, o->prefab, f);
        if (!value) continue;
        if (!section || strcmp(section, f->section)) {
            section = f->section;
            om_append(buf, len, cap, "<div class='knOmPropSec'>%s</div>", section);
        }
        om_prop_row(buf, len, cap, i, f, o->prefab, value);
        cJSON_Delete(value);
        shown++;
    }
    cJSON_Delete(record);
    if (!shown) om_append(buf, len, cap, "<div class='knOmEmpty'>%s</div>", I18N("kn.om.props.none", "No fields to edit"));
    om_append(buf, len, cap, "</div></div>");
}

static char *om_body_html(void) {
    char *buf = NULL;
    size_t len = 0, cap = 0;
    om_status_html(&buf, &len, &cap);
    if (g_om.props_uid >= 0) om_props_html(&buf, &len, &cap);
    else om_grid_html(&buf, &len, &cap);
    return buf;
}

char *object_manager_window_gen(const int id, const char **header) {
    if (id != OBJECT_MANAGER_WINDOW) return NULL;
    om_load_tab();
    if (header) *header = I18N("kn.om.title", "Object Manager");
    const Player *me = om_client() ? om_client()->me : NULL;
    char *buf = NULL;
    size_t len = 0, cap = 0;
    char search[sizeof(g_om.search) * 6];
    om_escape(search, sizeof(search), g_om.search);
    // v7 hides #windowHeader: the title is part of the window
    om_append(&buf, &len, &cap, "<div class='knOm'><div class='knOmTitle'>%s</div><div class='knOmTop'><div class='bigMenTabs knOmTabs'>",
              I18N("kn.om.title", "Object Manager"));
    for (int i = 0; i < OM_TAB_COUNT; i++) {
        om_append(&buf, &len, &cap, "<div id='knOmTab%d' class='bigMenTab%s' onclick='knOmTab(%d)'>%s</div>", i, i == g_om.tab ? " bigMenTabA" : "", i,
                  tab_name(i));
    }
    om_append(&buf, &len, &cap,
              // not id='knOmSearch': on the web window.knOmSearch would be the element (named access),
              // so kn-shim.js installs no knOmSearch forwarder and oninput throws
              "</div><input id='knOmSearchBox' class='knOmSearch' type='text' maxlength='40' placeholder='%s' value='%s' "
              "oninput='knOmSearch(this)'/></div><div class='knOmMain'><div id='knOmBody'>",
              I18N("generic.search", "Search"), search);
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
    om_append(&buf, &len, &cap, "<div id='knOmRemoveBtn' class='button small knOmBtn' onclick='knOmRemove()'>%s</div>"
                                "<div id='knOmUndoBtn' class='button small knOmBtn' onclick='knOmUndo()'>%s</div></div></div></div>",
              I18N("kn.om.remove", "Remove"), I18N("kn.om.undo", "Undo"));
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

// The Properties panel's values again (an answer, an edit of that object): its list keeps the scroll
static void om_props_refresh(void) {
    if (!om_window_open() || g_om.props_uid < 0) return;
    const HtmlElem body = html_dom_by_id("knOmBody"), list = html_dom_by_id("knOmPropsList");
    if (!body) return;
    HtmlScroll scroll = {0};
    const int had = list && html_dom_get_scroll(list, &scroll);
    char *html = om_body_html();
    html_dom_set_html(body, html ? html : "");
    free(html);
    const HtmlElem again = html_dom_by_id("knOmPropsList");
    if (had && again) html_dom_set_scroll(again, scroll.left, scroll.top);
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
    for (int i = 0; i < 3; i++) {
        static const char *const ids[3] = {"knOmSnapOff", "knOmDrop0", "knOmDrop1"};
        const HtmlElem b = html_dom_by_id(ids[i]);
        const int on = i == 0 ? om_build_snap() <= 0.0f : (i - 1) == om_build_drop();
        if (b && html_dom_has_class(b, "knOmOptA") != on) html_dom_set_class(b, "knOmOptA", on);
    }
    const HtmlElem tg = html_dom_by_id("knOmToolGunTile"), pg = html_dom_by_id("knOmPhysRow");
    if (tg && html_dom_has_class(tg, "knOmToolA") != om_holds(WEAPON_ID_TOOL_GUN)) html_dom_set_class(tg, "knOmToolA", om_holds(WEAPON_ID_TOOL_GUN));
    if (pg && html_dom_has_class(pg, "knOmToolA") != om_holds(WEAPON_ID_PHYS_GUN)) html_dom_set_class(pg, "knOmToolA", om_holds(WEAPON_ID_PHYS_GUN));
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
        g_om.props_uid = -1; // (F1 again: the grid)
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
    g_om.props_uid = -1;
    if (g_om.from_game && client && client->me) {
        g_om.from_game = 0;
        menu_request_open(0);
        return 1;
    }
    return 0;
}

void object_manager_ui_open_props(Client *client, const int uid) {
    g_om.props_uid = uid;
    g_om.props_sent[0] = 0;
    if (om_window_open()) {
        menu_show_window(OBJECT_MANAGER_WINDOW, 1);
        return;
    }
    g_om.from_game = ui_controls_enabled(client);
    if (g_om.from_game) { // over the game: the pointer freed, as F1
        ui_controls_reset_keys();
        client_set_menu_open(client, 1);
    }
    html_timer_add(0.0, om_show, NULL);
}

int object_manager_ui_props_uid(void) { return g_om.props_uid; }

// A field patch for the panel's object (OM_ACTION_EDIT + OM_EDIT_FIELDS): the Tool Gun's
static void om_send_fields(const ObjectManagerField *f, cJSON *patch) {
    Client *client = om_client();
    const Player *me = client ? client->me : NULL;
    char *json = patch ? cJSON_PrintUnformatted(patch) : NULL;
    cJSON_Delete(patch);
    const size_t n = json ? strlen(json) + 1 : 0;
    if (!me || !om_server_ok(client) || !n || n > OM_FIELDS_MAX + 1) {
        free(json);
        om_set_status(!me ? I18N("kn.om.status.joinEdit", "Join a game to edit objects") : I18N("kn.om.status.noServer", "This server has no Object Manager"), 1);
        return;
    }
    uint8_t buffer[sizeof(NetSpawnEditPacket) + OM_FIELDS_MAX + 1];
    NetSpawnEditPacket e;
    memset(&e, 0, sizeof(e));
    e.req = (NetSpawnReqPacket) {.action = OM_ACTION_EDIT, .item = OM_EDIT_SOURCE_BTOOLS, .pitch = me->direction.x, .yaw = me->direction.y};
    e.uid = g_om.props_uid;
    e.flags = OM_EDIT_FIELDS;
    memcpy(buffer, &e, sizeof(e));
    memcpy(buffer + sizeof(e), json, n);
    free(json);
    client_net_send(client, NET_PACKET_SPAWN_REQ, buffer, (uint16_t) (sizeof(e) + n));
    snprintf(g_om.props_sent, sizeof(g_om.props_sent), "%s", f->label);
}

static const char *arg_text(const HtmlEvent *ev, const HtmlArg *args, const int argc, const int i, char *buf, const size_t size) {
    buf[0] = 0;
    if (i < argc && args[i].kind == HTML_ARG_NUM) snprintf(buf, size, "%.10g", args[i].num);
    else if (i < argc && (args[i].kind == HTML_ARG_STR || args[i].kind == HTML_ARG_THIS_VALUE) && args[i].str) snprintf(buf, size, "%s", args[i].str);
    else if (ev && ev->target) html_dom_get_value(ev->target, buf, size);
    return buf;
}

// knOmField(index, value) / knOmFieldVec(index, axis, value): one field of the panel changed
static void om_field_changed(const int index, const int axis, const char *text) {
    const ObjectManagerField *f = object_manager_field(index);
    const Object *o = om_props_object();
    if (!f || !o || !object_manager_field_applies(f, o->prefab)) return;
    float mn, mx;
    object_manager_field_range(f, o->prefab, &mn, &mx);
    while (*text == ' ') text++;
    cJSON *patch = cJSON_CreateObject();
    char *end = NULL;
    const double v = strtod(text, &end);
    const int number = end != text && isfinite(v);
    switch (f->kind) {
        case OM_FIELD_ON:
        case OM_FIELD_OFF:
            cJSON_AddBoolToObject(patch, f->key, number && v != 0.0);
            break;
        case OM_FIELD_COLOR: {
            char colour[8];
            snprintf(colour, sizeof(colour), "%s%.6s", text[0] == '#' ? "" : "#", text);
            int ok = strlen(colour) == 7;
            for (int i = 1; ok && i < 7; i++) ok = (colour[i] >= '0' && colour[i] <= '9') || ((colour[i] | 32) >= 'a' && (colour[i] | 32) <= 'f');
            if (!ok) {
                cJSON_Delete(patch);
                om_set_status(I18N("kn.om.status.colour", "Colours are #rrggbb"), 1);
                om_props_refresh();
                return;
            }
            cJSON_AddStringToObject(patch, f->key, colour);
            break;
        }
        case OM_FIELD_VEC3: {
            cJSON *record = object_manager_record(&om_client()->game, g_om.props_uid);
            cJSON *now = object_manager_field_value(record, o->prefab, f);
            cJSON_Delete(record);
            if (!number || axis < 0 || axis > 2 || !now) {
                cJSON_Delete(now);
                cJSON_Delete(patch);
                om_props_refresh();
                return;
            }
            cJSON_ReplaceItemInArray(now, axis, cJSON_CreateNumber(!strcmp(f->key, "s") ? fmax(v, OM_EDIT_SIZE_MIN) : v));
            cJSON_AddItemToObject(patch, f->key, now);
            break;
        }
        default: {
            if (!number) {
                cJSON_Delete(patch);
                om_props_refresh();
                return;
            }
            double clamped = fmin(fmax(v, mn), mx); // the editor's slider range
            if (f->kind != OM_FIELD_NUM && f->kind != OM_FIELD_NUMN) clamped = round(clamped);
            cJSON_AddNumberToObject(patch, f->key, clamped);
            break;
        }
    }
    om_send_fields(f, patch);
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

// A KN gun in hand at once: its slot when carried (picked with the next input frame, also while the
// window is open), else added as an extra slot and equipped when its LOADOUT_EXTRA lands
static void om_equip_gun(const Player *me, const int weapon_id, const char *text) {
    for (int i = 0; me->loadout && i < me->loadout_size; i++) {
        if (me->loadout[i] != weapon_id) continue;
        if (i != me->loadout_index) ui_controls_select_slot(i);
        om_set_status(text, 0);
        om_update_tools();
        return;
    }
    om_set_status(text, 0);
    om_expect_equip(weapon_id);
    om_send(OM_ACTION_ADD_WEAPON, weapon_id);
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
        om_set_status(I18N("kn.om.status.joinTool", "Join a game to use the Tool Gun"), 1);
        return;
    }
    if (!om_server_ok(client)) {
        om_set_status(I18N("kn.om.status.noServer", "This server has no Object Manager"), 1);
        return;
    }
    char text[96];
    I18N_F(text, "kn.om.toolPicked", "Tool: {0}", object_manager_tool_info(tool)->name);
    om_equip_gun(me, WEAPON_ID_TOOL_GUN, text);
}

// The Tools pane's Physics Gun row
void object_manager_ui_pick_physgun(void) {
    const Client *client = om_client();
    const Player *me = client ? client->me : NULL;
    if (!me) {
        om_set_status(I18N("kn.om.status.joinPhys", "Join a game to use the Physics Gun"), 1);
        return;
    }
    if (!om_server_ok(client)) {
        om_set_status(I18N("kn.om.status.noServer", "This server has no Object Manager"), 1);
        return;
    }
    om_equip_gun(me, WEAPON_ID_PHYS_GUN, I18N("kn.om.physPicked", "Physics Gun: hold fire on an object to carry it"));
}

int object_manager_ui_tool(void) {
    om_load_tool();
    return g_om.tool;
}

static void om_pick(const int tab, const int item, const int alt) {
    const Client *client = om_client();
    if (!client || !client->me) {
        om_set_status(I18N("kn.om.status.join", "Join a game to spawn things"), 1);
        return;
    }
    if (!om_server_ok(client)) {
        om_set_status(I18N("kn.om.status.noServer", "This server has no Object Manager"), 1);
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

void object_manager_ui_pick_entry(const int tab, const int item) {
    om_pick(tab, item, 0);
}

void object_manager_ui_pick_tool(const int tool) {
    om_load_tool();
    om_pick_tool(tool);
}

int object_manager_ui_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    if (!name || strncmp(name, "knOm", 4) != 0) return -1;
    if (!strcmp(name, "knOmTab")) {
        const int tab = arg_num(args, argc, 0, g_om.tab);
        if (g_om.props_uid >= 0 && tab == g_om.tab) { // the Properties panel: back to the tab's grid
            g_om.props_uid = -1;
            menu_show_window(OBJECT_MANAGER_WINDOW, 1);
        }
        if (tab >= 0 && tab < OM_TAB_COUNT && tab != g_om.tab) {
            g_om.props_uid = -1;
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
    if (!strcmp(name, "knOmPhys")) {
        object_manager_ui_pick_physgun();
        return 0;
    }
    if (!strcmp(name, "knOmSnapSet")) { // the field's value (any step, "off" / 0 / empty: off)
        float step = 0.0f;
        if (argc > 0 && args[0].kind == HTML_ARG_NUM) step = (float) args[0].num;
        else if (argc > 0 && (args[0].kind == HTML_ARG_STR || args[0].kind == HTML_ARG_THIS_VALUE) && args[0].str) step = (float) atof(args[0].str);
        else if (ev && ev->target) {
            char value[24] = "";
            html_dom_get_value(ev->target, value, sizeof(value));
            step = (float) atof(value);
        }
        om_build_set_snap(step);
        object_manager_ui_snap_changed();
        return 0;
    }
    if (!strcmp(name, "knOmToolGunPick")) { // the Tool Gun tile: in hand with the tool picked
        om_pick_tool(g_om.tool);
        return 0;
    }
    if (!strcmp(name, "knOmPhysDrop")) {
        om_build_set_drop(arg_num(args, argc, 0, 0));
        om_update_tools();
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
    if (!strcmp(name, "knOmField") || !strcmp(name, "knOmFieldVec")) {
        const int vec = name[9] == 'V';
        char text[64];
        om_field_changed(arg_num(args, argc, 0, -1), vec ? arg_num(args, argc, 1, -1) : -1, arg_text(ev, args, argc, vec ? 2 : 1, text, sizeof(text)));
        return 0;
    }
    if (!strcmp(name, "knOmPropsOpen")) { // knOmPropsOpen(uid): scenarios, the console
        const int uid = arg_num(args, argc, 0, -1);
        if (uid >= 0) {
            g_om.props_uid = uid;
            menu_show_window(OBJECT_MANAGER_WINDOW, 1);
        }
        return 0;
    }
    if (!strcmp(name, "knOmPropsClose")) {
        g_om.props_uid = -1;
        menu_show_window(OBJECT_MANAGER_WINDOW, 1);
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
    if (r->action == OM_ACTION_EDIT && r->limit == OM_FIELDS_ANSWER) { // the Properties panel's patch
        char text[160];
        if (r->status == OM_STATUS_OK) I18N_F(text, "kn.om.saved", "{0}: saved", g_om.props_sent[0] ? g_om.props_sent : I18N("kn.om.field", "Field"));
        else snprintf(text, sizeof(text), "%s", object_manager_status_text(r->status));
        om_set_status(text, r->status != OM_STATUS_OK);
        om_props_refresh(); // the values the object has now
        return;
    }
    if (r->action == OM_ACTION_EDIT) { // a drag's edits: only refusals speak (a rate answer is the pace)
        if (r->status != OM_STATUS_OK && r->status != OM_STATUS_RATE) {
            om_build_refused(r->status);
            om_set_status(object_manager_status_text(r->status), 1);
        }
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
    const Weapon *w = r->item >= 0 && r->item < WEAPON_TABLE_COUNT && r->action != OM_ACTION_TOOL ? rules_weapon(r->item) : NULL;
    char text[160] = "";
    switch (r->action) {
        case OM_ACTION_ADD_WEAPON: I18N_F(text, "kn.om.added", "{0} added to your loadout", w ? w->name : I18N("kn.om.weapon", "Weapon")); break;
        case OM_ACTION_REMOVE_WEAPON: I18N_F(text, "kn.om.takenOut", "{0} taken out of your loadout", w ? w->name : I18N("kn.om.weapon", "Weapon")); break;
        case OM_ACTION_CLEAR_WEAPONS: snprintf(text, sizeof(text), "%s", I18N("kn.om.cleared", "Extra weapons cleared")); break;
        case OM_ACTION_DROP_WEAPON: I18N_F(text, "kn.om.droppedAim", "{0} dropped at your crosshair", w ? w->name : I18N("kn.om.weapon", "Weapon")); break;
        case OM_ACTION_UNDO: snprintf(text, sizeof(text), "%s", I18N("kn.om.undone", "Undone")); break;
        case OM_ACTION_REMOVE_AIM: snprintf(text, sizeof(text), "%s", I18N("kn.om.removed", "Removed")); break;
        case OM_ACTION_DROP_HELD:
            if (r->item == WEAPON_ID_TOOL_GUN) snprintf(text, sizeof(text), "%s", I18N("kn.om.toolAway", "Tool Gun put away"));
            else I18N_F(text, "kn.om.dropped", "{0} dropped", w ? w->name : I18N("kn.om.weapon", "Weapon"));
            break;
        case OM_ACTION_TOOL:
            if (r->limit == OM_TOOL_SHOWN) return; // Info: the NOTIFY carries the text
            if (r->limit == OM_TOOL_COPIED) snprintf(text, sizeof(text), "%s", I18N("kn.om.copied", "Copied: shoot a surface to place the copy"));
            else if (r->limit == OM_TOOL_PASTED) snprintf(text, sizeof(text), "%s", I18N("kn.om.pasted", "Copy placed"));
            else if (r->item == OM_TOOL_ROTATOR) snprintf(text, sizeof(text), "%s", I18N("kn.om.rotated", "Rotated"));
            else snprintf(text, sizeof(text), "%s", I18N("kn.om.removed", "Removed"));
            break;
        case OM_ACTION_SPAWN_NPC:
        case OM_ACTION_SPAWN_ENTITY:
        case OM_ACTION_SPAWN_OBJECT: {
            int count = 0;
            const int tab = r->action == OM_ACTION_SPAWN_NPC ? OM_TAB_NPCS : r->action == OM_ACTION_SPAWN_ENTITY ? OM_TAB_ENTITIES : OM_TAB_OBJECTS;
            const ObjectManagerEntry *entries = object_manager_entries(tab, &count);
            I18N_F(text, "kn.om.spawned", "{0} spawned at your crosshair",
                   entries && r->item >= 0 && r->item < count ? entries[r->item].name : I18N("kn.om.it", "It"));
            break;
        }
        default: snprintf(text, sizeof(text), "%s", I18N("kn.om.done", "Done")); break;
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
        case NET_PACKET_SPAWNED_OBJ: {
            NetSpawnedObjPacket head;
            vec3 shown = {0.0f, 0.0f, 0.0f};
            float shown_yaw = 0.0f;
            int moves = 0;
            if (length >= sizeof(head)) { // an edit of an object that is there: eased to the new pose (om_interp.c)
                memcpy(&head, payload, sizeof(head));
                moves = head.kind == OM_KIND_OBJECT && (head.op == OM_SPAWNED_UPDATE || head.op == OM_SPAWNED_MAP_EDIT) &&
                        om_interp_shown(client, head.id, &shown, &shown_yaw);
            }
            object_manager_apply_spawned(&client->game, payload, length);
            if (moves) om_interp_moved(client, head.id, shown, shown_yaw);
            if (length < sizeof(head) || g_om.props_uid < 0) return 1;
            memcpy(&head, payload, sizeof(head));
            const HtmlElem focus = html_dom_active_element();
            // someone's edit of the panel's object: its values again (not under a field being typed in)
            if (head.kind == OM_KIND_OBJECT && head.id == g_om.props_uid && !(focus && html_dom_has_class(focus, "knOmPropIn"))) om_props_refresh();
            return 1;
        }
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
        const ObjectManagerToolInfo *info = object_manager_tool_info(g_om.tool);
        if (info && !info->handles) om_send(OM_ACTION_TOOL, g_om.tool); // Move / Resize: om_build.c drags instead
    }
    g_om.shot_me = me;
    g_om.shot_slot = slot;
    g_om.shot_reload = reload;
}

// "TOOL: DELETER" beside the ammo counter while the Tool Gun is in hand (#knToolHud, made on first
// use next to #ammoHolder so the HUD markup stays v7's)
static void om_tool_hud(const Player *me) {
    char text[sizeof(g_om.hud_text)] = "";
    if (me && me->active && me->weapon && me->weapon->kn_physgun) {
        I18N_F(text, "kn.om.hud.phys", "Physics Gun: {0}", om_build_drop() ? I18N("kn.om.drop", "Drop") : I18N("kn.om.freeze", "Freeze"));
    } else if (me && me->active && me->weapon && me->weapon->kn_tool) {
        om_load_tool();
        const ObjectManagerToolInfo *t = object_manager_tool_info(g_om.tool);
        char snap[24];
        snprintf(snap, sizeof(snap), "%g", (double) om_build_snap());
        if (t && t->handles) I18N_F(text, "kn.om.hud.toolSnap", "Tool: {0} (Snap {1})", t->name, snap);
        else I18N_F(text, "kn.om.toolPicked", "Tool: {0}", t ? t->name : "?");
    }
    for (char *c = text; *c; c++) *c = (char) (*c >= 'a' && *c <= 'z' ? *c - 32 : *c); // the HUD's caps (ASCII; other scripts as they are)
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
        // (the KN guns too: the Physics Gun's reach depends on OM_CAP_MAP / OM_CAP_DEV)
        if (!g_om.queried && me->active && me->weapon && (me->weapon->build || me->weapon->kn_tool || me->weapon->kn_physgun) && om_server_ok(client)) {
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

// The grid step changed (the field, the snap keys, `tool_snap`): the field and a status line
void object_manager_ui_snap_changed(void) {
    char snap[16], text[96];
    om_build_snap_text(snap, sizeof(snap));
    if (om_build_snap() > 0.0f) I18N_F(text, "kn.om.snapUnits", "Snap: {0} units", snap);
    else snprintf(text, sizeof(text), "%s", I18N("kn.om.snapOff", "Snap: off"));
    om_set_status(text, 0);
    om_update_tools();
    const HtmlElem box = om_window_open() ? html_dom_by_id("knOmSnapBox") : 0;
    if (box) html_dom_set_value(box, om_build_snap() > 0.0f ? snap : "");
}

// A new connection / map: the caps belong to the server we talked to.
void object_manager_ui_reset(void) {
    om_build_reset();
    g_om.props_uid = -1;
    g_om.caps = -1;
    g_om.queried = 0;
    g_om.pending_equip = -1;
    g_om.status[0] = 0;
}
