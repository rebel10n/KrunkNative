// Menu state machine over the DOM-ops API: the C port of the menu JavaScript
// (krunknative.js) plus the net/state/connection glue. No JS runs natively; on the web
// ui/js/kn-shim.js only forwards page events here. The page is Krunker's own markup + main.css; this mirrors what the real
// client does in its loaded, logged-out, menu-open state:
//   - scaleBase()                   fit the 1700x900 * uiCustomScale design space
//   - UI.toggleMenu/toggleMenuHider menu open/close inline styles
//   - updatePlayInstructions()      #instructions = "CLICK TO PLAY" (4 states)
//   - showWindow/closeWindow        #windowHolder / #windowHeader / #menuWindow
//   - the class box (#menuClassContainer) population
// Window contents come from ui/windows (ui_windows.c); state, classes, maps, connection
// and menu open/close are direct DOM writes.
#include <ui_logic.h>
#include <html_ui.h>
#include <weapons.h>
#include <ui_mobile.h> // isMobile: lockless play (client/src/ui/ui_mobile.c)
#include <gamepad.h> // Gamepad.active: "PRESS START" (client/src/gamepad.c)
#include <console.h> // developer console overlay (client/src/ui/ui_console.c)
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

// After closing the menu the pointer lock request is asynchronous in browsers; do not
// treat "not locked yet" as "lock lost" for this long.
#define MENU_LOCK_GRACE_SECONDS 1.0
// How long "CONNECTING..." may stay up with no connection event from the transport
// before the page shows the connection error screen on its own (the transport reports
// every failed attempt within ~2 s, so this only covers a hung transport).
#define CONNECT_TIMEOUT_MS 6000.0
// Failures repeat every retry; identical text is re-applied at most this often.
#define MENU_CONNECT_ERROR_REPEAT_SECONDS 5.0
#define SPEECH_BUBBLE_MS 2100.0
#define CONNECT_ERROR_DEFAULT "Could not connect to the game server"

static MenuUi g_ui;
static char g_connect_code[8]; // Krunker's error code on the connection error screen ("000000")

// Krunker v3.1.1 idle kick (index.js gameLoop + config.kickTimer): while the menu is up
// (menuHolder shown), not spectating, on a production page, not the lobby host, not from
// a ranked queue and the mode has no `noKick`, CONTROLS.idleTimer += delta; at 90 s
// disconnect(i18n "player.inactive", "000005"). The timer resets on any key, opening a
// window (showWindow), showMenu() and entering the game.
#define MENU_KICK_TIMER_MS 90000.0
static double g_idle_ms;
static double g_idle_last;

MenuUi *menu_ui(void) { return &g_ui; }

void menu_idle_reset(void) { g_idle_ms = 0.0; }

// config.isProd is "the page is not served from 127.0.0.1 / localhost / 192.168.*";
// natively the closest equivalent is the server we play on. KRUNKNATIVE_IDLE_KICK=<s>
// forces the kick on with that timeout (testing local servers).
static int idle_kick_prod(const Client *client, double *limit_ms) {
    const char *force = getenv("KRUNKNATIVE_IDLE_KICK");
    if (force && *force) {
        const double s = atof(force);
        if (s > 0.0) {
            *limit_ms = s * 1000.0;
            return 1;
        }
    }
    *limit_ms = MENU_KICK_TIMER_MS;
    const char *host = client->net_address;
    return host[0] && strcmp(host, "127.0.0.1") && strcmp(host, "localhost") && strncmp(host, "192.168.", 8);
}

static void menu_idle_tick(Client *client) {
    const double now = glfwGetTime();
    const double delta_ms = g_idle_last > 0.0 ? (now - g_idle_last) * 1000.0 : 0.0;
    g_idle_last = now;
    double limit_ms;
    const int remote = client->net_connected && !client->offline && !client->game.is_local; // !isLobbyHost
    const int no_kick = client->game.mode && client->game.mode->config.no_kick;
    if (!g_ui.menu_open || !remote || no_kick || !idle_kick_prod(client, &limit_ms)) return;
    g_idle_ms += delta_ms;
    if (g_idle_ms < limit_ms) return;
    g_idle_ms = 0.0;
    menu_log("idle kick: %.0f s in the menu", limit_ms / 1000.0);
    menu_toggle_window(0); // disconnect(): showWindow(0)
    client_net_disconnect(client, "Kicked for inactivity", "000005");
}

// ---------------------------------------------------------------- DOM helpers

static HtmlElem by_id(const char *id) { return html_dom_by_id(id); }

// Bounded copy (strncpy without the padding; always NUL-terminated).
static void copy_str(char *dst, const size_t n, const char *src) {
    size_t len = strlen(src);
    if (len >= n) len = n - 1;
    memcpy(dst, src, len);
    dst[len] = 0;
}

static void set_display(const char *id, const char *value) {
    HtmlElem el = by_id(id);
    if (el) html_dom_set_display(el, value);
}

static void set_style(const char *id, const char *prop, const char *value) {
    HtmlElem el = by_id(id);
    if (el) html_dom_set_style(el, prop, value);
}

static int inline_style_is(const HtmlElem el, const char *prop, const char *value) {
    char buf[64];
    return el && html_dom_get_style(el, prop, buf, sizeof(buf)) >= 0 && !strcmp(buf, value);
}

int menu_window_is_open(void) {
    return inline_style_is(by_id("windowHolder"), "display", "block");
}

void menu_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("[ui log] ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

// JS number -> string for px values computed from the design space (1080, 1035, ...).
static void fmt_number(char *buf, const size_t n, const double value) {
    snprintf(buf, n, "%.15g", value);
}

static int client_pointer_locked(const Client *client) {
    // isMobile: UsePointerLock = false, CONTROLS.locklessChange() owns `enabled`, so a
    // missing pointer lock never reopens the menu.
    if (ui_mobile_active()) return 1;
#ifdef __EMSCRIPTEN__
    (void) client;
    return EM_ASM_INT({ return document.pointerLockElement ? 1 : 0; });
#else
    return glfwGetInputMode(client->window, GLFW_CURSOR) == GLFW_CURSOR_DISABLED;
#endif
}

static void request_pointer_lock_if_web(void) {
    if (ui_mobile_active()) { // no pointer lock on phones: fullscreen + landscape instead
        ui_mobile_play_gesture();
        return;
    }
#ifdef __EMSCRIPTEN__
    EM_ASM({
        var canvas = document.getElementById('canvas');
        try { if (canvas && canvas.requestPointerLock) canvas.requestPointerLock(); } catch (e) {}
    });
#endif
}

// ---------------------------------------------------------------- game data

static int client_is_connected(const Client *client) {
    return client->game.is_local || client->offline || client->net_connected;
}

static const char *client_map_name(const Client *client) {
    if (!client->game.map || !client->game.map->raw_data) return "";
    const cJSON *name = cJSON_GetObjectItem(client->game.map->raw_data, "name");
    return cJSON_IsString(name) ? cJSON_GetStringValue(name) : "";
}

static const char *client_mode_name(const Client *client) {
    return client->game.mode && client->game.mode->config.name ? client->game.mode->config.name : "";
}

// Mirrors Game.classes as the real class window renders them (name, primary weapon, HP).
static void menu_build_classes(void) {
    const Client *client = g_ui.client;
    const size_t count = client->game.class_count ? client->game.class_count : sizeof(g_classes) / sizeof(g_classes[0]);
    const ClassConfig *classes = client->game.classes ? client->game.classes : g_classes;
    const Weapon *const *weapons = client->game.weapons ? (const Weapon *const *) client->game.weapons : g_weapons;
    const size_t weapon_count = client->game.weapons ? client->game.weapon_count : sizeof(g_weapons) / sizeof(g_weapons[0]);

    g_ui.class_count = 0;
    for (size_t i = 0; i < count && g_ui.class_count < MENU_CLASS_MAX; i++) {
        MenuClass *c = &g_ui.classes[g_ui.class_count++];
        c->index = (int) i;
        c->name = classes[i].name ? classes[i].name : "";
        c->weapon = "";
        if (classes[i].loadout) {
            const int w = classes[i].loadout[0];
            if (w >= 0 && (size_t) w < weapon_count && weapons[w] && weapons[w]->name) c->weapon = weapons[w]->name;
        }
        c->hp = classes[i].health;
        snprintf(c->icon, sizeof(c->icon), "textures/classes/icon_%d.png", classes[i].icon_index);
    }
    g_ui.last_classes = classes;
    g_ui.last_class_count = count;
}

static void menu_build_maps(void) {
    const Client *client = g_ui.client;
    g_ui.map_count = 0;
    for (size_t i = 0; i < client->game.map_count && g_ui.map_count < MENU_MAP_MAX; i++) {
        const cJSON *map = client->game.maps ? client->game.maps[i] : NULL;
        const cJSON *name = map ? cJSON_GetObjectItem(map, "name") : NULL;
        MenuMap *m = &g_ui.maps[g_ui.map_count++];
        m->index = (int) i;
        if (cJSON_IsString(name)) snprintf(m->name, sizeof(m->name), "%s", cJSON_GetStringValue(name));
        else snprintf(m->name, sizeof(m->name), "Map %zu", i);
    }
    g_ui.last_maps = client->game.maps;
    g_ui.last_map_count = client->game.map_count;
}

const MenuClass *menu_class_at(const int index) {
    for (int i = 0; i < g_ui.class_count; i++) {
        if (g_ui.classes[i].index == index) return &g_ui.classes[i];
    }
    return NULL;
}

const MenuMap *menu_map_by_name(const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < g_ui.map_count; i++) {
        if (!strcmp(g_ui.maps[i].name, name)) return &g_ui.maps[i];
    }
    return NULL;
}

// ---------------------------------------------------------------- previewRect

// Cut the dimming layer (#instructionHolder) open over the preview canvas so the
// engine's render underneath shows undimmed, like Krunker's own canvas that sits
// above the overlay. Coordinates are the holder's local (scaled #uiBase) space; the
// hole covers exactly the whole-pixel viewport rect the preview draws its own dimmed
// backdrop into (x, y, w, h), so the two never leave a one-pixel seam.
// While a popup is open (K.P.D: Officer Buddy, dialogs), #popupBack (rgba(0,0,0,0.75), z-index 20,
// below #menuClassContainer's 99999) gets the same hole: in Krunker the model stays
// bright above the popup backdrop (menu_preview_backdrop_alpha() darkens the box).
static void place_hole(const char *parent_id, const char *hole_id, const char *shadow, const int on,
                       const float x, const float y, const float w, const float h) {
    HtmlElem parent = by_id(parent_id);
    if (!parent) return;
    HtmlElem hole = by_id(hole_id);
    if (!on) {
        if (hole) html_dom_set_display(hole, "none");
        return;
    }
    if (!hole) {
        hole = html_dom_create("div", hole_id);
        if (!hole) return;
        html_dom_append(parent, hole);
    }
    HtmlRect pr = {0};
    html_dom_get_rect(parent, &pr);
    const float offset_width = html_dom_offset_width(parent);
    float k = offset_width > 0.0f ? pr.w / offset_width : 1.0f;
    if (k == 0.0f) k = 1.0f;
    char v[48];
    snprintf(v, sizeof(v), "%.3fpx", (x - pr.x) / k);
    html_dom_set_style(hole, "left", v);
    snprintf(v, sizeof(v), "%.3fpx", (y - pr.y) / k);
    html_dom_set_style(hole, "top", v);
    snprintf(v, sizeof(v), "%.3fpx", w / k);
    html_dom_set_style(hole, "width", v);
    snprintf(v, sizeof(v), "%.3fpx", h / k);
    html_dom_set_style(hole, "height", v);
    if (shadow) {
        html_dom_set_style(hole, "position", "absolute");
        html_dom_set_style(hole, "pointer-events", "none");
        html_dom_set_style(hole, "box-shadow", shadow);
        html_dom_set_display(hole, "block");
    }
}

// #popupHolder is showing (K.P.D, confirm/prompt dialogs, ...): its 75% backdrop sits
// below #menuClassContainer (z-index 99999), so the preview model stays bright.
static int popup_open(void) {
    return inline_style_is(by_id("popupHolder"), "display", "block");
}

static void update_preview_hole(const int visible, const float x, const float y, const float w, const float h) {
    HtmlElem holder = by_id("instructionHolder");
    if (!holder) return;
    if (!visible) {
        html_dom_set_class(holder, "kn-hole", 0);
    } else {
        place_hole("instructionHolder", "knPreviewHole", NULL, 1, x, y, w, h);
        html_dom_set_class(holder, "kn-hole", 1);
    }
    const int police = visible && popup_open();
    HtmlElem back = by_id("popupBack");
    if (back) {
        html_dom_set_style(back, "background-color", police ? "transparent" : "");
        html_dom_set_style(back, "overflow", police ? "hidden" : "");
    }
    place_hole("popupBack", "knPopupHole", "0 0 0 4000px rgba(0, 0, 0, 0.75)", police, x, y, w, h);
}

// The class preview's own backdrop inside its box: #instructionHolder's 50% black, and
// under the K.P.D popup also #popupBack's 75% (1 - 0.5 * 0.25).
float menu_preview_backdrop_alpha(void) {
    return popup_open() && g_ui.menu_open ? 0.875f : 0.5f;
}

void menu_send_preview_rect(void) {
    HtmlElem canvas = by_id("classPreviewCanvas");
    if (!g_ui.dom_ready) {
        HtmlRect r = {0};
        if (canvas && g_ui.menu_open) html_dom_get_rect(canvas, &r);
        update_preview_hole(canvas && g_ui.menu_open, roundf(r.x), roundf(r.y), roundf(r.w), roundf(r.h));
        return;
    }
    float vw, vh, dpr = 1.0f;
    html_dom_viewport(&vw, &vh, &dpr);
    if (dpr <= 0.0f) dpr = 1.0f;

    int visible = 0, x = 0, y = 0, w = 0, h = 0;
    if (canvas && g_ui.menu_open) {
        HtmlRect r = {0};
        html_dom_get_rect(canvas, &r);
        HtmlElem cc = by_id("menuClassContainer");
        const int hidden = r.w == 0.0f || r.h == 0.0f || (cc && inline_style_is(cc, "visibility", "hidden"));
        visible = !hidden;
        x = (int) lroundf(r.x);
        y = (int) lroundf(r.y);
        w = (int) lroundf(r.w);
        h = (int) lroundf(r.h);
    }
    update_preview_hole(visible, (float) x, (float) y, (float) w, (float) h);

    char key[96];
    snprintf(key, sizeof(key), "%d,%d,%d,%d,%d,%g", visible, x, y, w, h, dpr);
    if (!strcmp(key, g_ui.last_rect)) return;
    snprintf(g_ui.last_rect, sizeof(g_ui.last_rect), "%s", key);
    client_class_preview_set_rect(g_ui.client, visible, (float) x, (float) y, (float) w, (float) h, dpr);
}

static void rect_timer_fire(void *user) {
    (void) user;
    g_ui.rect_timer = 0;
    menu_send_preview_rect();
}

void menu_schedule_rect(void) {
    if (g_ui.rect_timer) return;
    g_ui.rect_timer = html_timer_add(0.0, rect_timer_fire, NULL);
}

static void on_observed_resize(const HtmlElem el, void *user) {
    (void) el;
    (void) user;
    menu_schedule_rect();
}

// ---------------------------------------------------------------- scaleBase

// Port of window.scaleBase(): fit a 1700x900 design space, scaled by
// settings.tmp.uiCustomScale (= 1 - scaleUI + 1, scaleUI defaults to 0.8 so the space
// is 2040x1080), into the viewport by scaling #uiBase.
void menu_scale_base(void) {
    HtmlElem base = by_id("uiBase");
    if (!base) return;
    if (ui_setting_is_false("uiScaling")) {
        html_dom_set_style(base, "transform", "");
        html_dom_set_style(base, "width", "");
        html_dom_set_style(base, "height", "");
        menu_send_preview_rect();
        return;
    }
    double scale_ui;
    if (!ui_setting_number("scaleUI", &scale_ui) || scale_ui != scale_ui) scale_ui = 0.8;
    const double custom = 1.0 - scale_ui + 1.0;
    float w, h, dpr;
    html_dom_viewport(&w, &h, &dpr);
    g_ui.viewport_w = w;
    g_ui.viewport_h = h;
    const double dw = custom * 1700.0, dh = custom * 900.0;
    const double sx = w / dw, sy = h / dh;
    char value[64], num[32];
    if (sy < sx) {
        snprintf(value, sizeof(value), "scale(%.3f)", sy);
        html_dom_set_style(base, "transform", value);
        snprintf(value, sizeof(value), "%.3fpx", w / sy);
        html_dom_set_style(base, "width", value);
        fmt_number(num, sizeof(num), dh);
        snprintf(value, sizeof(value), "%spx", num);
        html_dom_set_style(base, "height", value);
    } else {
        snprintf(value, sizeof(value), "scale(%.3f)", sx);
        html_dom_set_style(base, "transform", value);
        fmt_number(num, sizeof(num), dw);
        snprintf(value, sizeof(value), "%spx", num);
        html_dom_set_style(base, "width", value);
        snprintf(value, sizeof(value), "%.3fpx", h / sx);
        html_dom_set_style(base, "height", value);
    }
    menu_send_preview_rect();
}

// ---------------------------------------------------------------- menu

// UI.toggleMenu(j) + UI.toggleMenuHider(j), inline styles included.
static void menu_update_info_holder(void);

void menu_apply_open(const int open) {
    if (!!open != g_ui.menu_open) menu_idle_reset(); // showMenu() / entering the game
    g_ui.menu_open = !!open;
    if (!g_ui.dom_ready && !by_id("uiBase")) return; // applied by menu_dom_init once the page exists
    const int on = g_ui.menu_open;

    html_dom_set_class(html_dom_root(), "kn-menu-open", on);
    set_display("menuHolder", on ? "block" : "none");
    set_display("speakerDisplay", on ? "none" : "block");
    set_style("chatHolder", "bottom", on ? "20px" : "148px");
    set_style("voiceDisplay", "bottom", on ? "18px" : "145px");
    set_display("supportMapCreator", "none");
    // toggleMenuHider
    set_display("menuHider", on ? "block" : "none");
    HtmlElem base = by_id("uiBase");
    if (base) html_dom_set_class(base, "onMenu", on);
    set_display("gameNameHolder", on ? "" : "none");
    set_display("spectButton", on ? "" : "none");
    if (on) {
        set_display("spinUI", "none");
        set_display("respinUI", "none");
        set_display("endUI", "none");
        set_display("instructionHolder", "block");
        if (by_id("baseLinks") && !menu_window_is_open()) set_display("baseLinks", "block");
        set_display("newLeaderDisplay", "none");
        set_display("terminalHolder", "none");
    } else {
        // entering the game: Krunker hides the overlay (hud_ui.c shows #inGameUI)
        set_display("instructionHolder", "none");
        menu_close_window();
    }
    set_style("menuClassContainer", "visibility", "");
    menu_update_info_holder();
    ui_loadout_clear_pops(); // UI.toggleControlUI ends with clearPops()
    menu_update_instructions();
    menu_schedule_rect();
}

void menu_request_open(const int open) {
    client_set_menu_open(g_ui.client, open);
}

// Connected and the map is loaded (INIT processed): the client accepts `play`.
static int can_play(void) {
    return g_ui.connected && g_ui.ready;
}

// updatePlayInstructions(): in the loaded, connected state the real client writes
// exactly i18n "app.play" = "CLICK TO PLAY". Before the socket is up it shows
// generic.loading / generic.connecting; a dead socket gets showConnectionErrorScreen()'s
// markup (error text, error.seek line with the retry link, error.extentions footer).
// KrunkNative adds one line to that screen: a link that hosts an offline game.
void menu_update_instructions(void) {
    HtmlElem el = by_id("instructions");
    if (!el) return;
    StrBuf sb = {0};
    if (!g_ui.dom_ready) {
        sb_append(&sb, "LOADING...");
    } else if (can_play()) {
        // gamepad.active ? "app.play.controller" : aM.isMobile ? "TAP TO PLAY" : "app.play"
        sb_append(&sb, gamepad_active() ? "PRESS START" : ui_mobile_active() ? "TAP TO PLAY" : "CLICK TO PLAY");
    } else if (g_ui.connect_error[0] && !g_ui.connected) {
        sb_append(&sb, "<span style='color: rgba(255, 255, 255, 0.6)'>");
        sb_append_esc(&sb, g_ui.connect_error);
        sb_append(&sb, "</span><br/><span style='color: rgba(255, 255, 255, 0.6)'>Try seeking a new game by clicking <a href='#' onclick='return knRetry()'>here</a>.</span>"
            "<br/><span style='color: rgba(255, 255, 255, 0.6)'>Or <a href='#' onclick='return knHostOffline()'>host an offline game</a>.</span>"
            "<div style='margin-top:10px;font-size:20px;color:rgba(255,255,255,0.4)'>If you have any extensions, disable them (Error 0x");
        sb_append_esc(&sb, g_connect_code[0] ? g_connect_code : "000000");
        sb_append(&sb, ")</div>");
    } else {
        sb_append(&sb, "CONNECTING...");
    }
    char *html = sb_finish(&sb);
    if (strcmp(html, g_ui.last_instructions)) {
        html_dom_set_html(el, html);
        snprintf(g_ui.last_instructions, sizeof(g_ui.last_instructions), "%s", html);
    }
    free(html);
    set_style("instructionHolder", "pointer-events", "all");
}

// Region name override (ui_scenario.c `state.region`); "" = MENU_REGION_NAME.
static char g_region_label[64];

void menu_set_region_label(const char *name) {
    snprintf(g_region_label, sizeof(g_region_label), "%s", name ? name : "");
    menu_update_top_bar();
}

void menu_update_top_bar(void) {
    HtmlElem lbl = by_id("menuRegionLabel");
    if (lbl) html_dom_set_text(lbl, g_ui.connected ? (g_region_label[0] ? g_region_label : MENU_REGION_NAME) : "...");
}

// Krunker's map load: mapInfo.textContent = mode.alias + "_" + map.name, and the
// menu's #infoHolder (map info + terms links) shows once a game session exists.
void menu_set_game_name(const char *name) {
    HtmlElem info = by_id("mapInfo");
    if (info) html_dom_set_text(info, name ? name : "");
    set_display("infoHolder", name && name[0] ? "" : "none");
}

// io.connect callback: infoHolder.style.display = null (map info + Advertise / Support /
// Terms / version links); map load: mapInfo.textContent = mode.alias + "_" + map.name.
// The alias is the running mode's (GameModeConfig.alias, "ffa" before a mode exists).
static void menu_update_info_holder(void) {
#ifdef UI_SCENARIO_SUPPORTED
    if (ui_scenario_active()) return; // the scenario's state.gameName drives it
#endif
    if (!g_ui.connected || !g_ui.menu_open) { // UI.toggleControlUI(j): infoHolder hidden in game
        set_display("infoHolder", "none");
        return;
    }
    char name[96] = "";
    const Client *client = g_ui.client;
    const char *alias = client->game.mode && client->game.mode->config.alias ? client->game.mode->config.alias : "ffa";
    if (g_ui.map_name[0]) snprintf(name, sizeof(name), "%s_%s", alias, g_ui.map_name);
    HtmlElem info = by_id("mapInfo");
    if (info) html_dom_set_text(info, name);
    set_display("infoHolder", "");
}

// The class box: name tag, weapon line, class name (see the real dJ()).
void menu_update_class_box(void) {
    const MenuClass *c = menu_class_at(g_ui.client->menu_class_index);
    HtmlElem name_el = by_id("menuClassName"), sub_el = by_id("menuClassSubtext"), tag_el = by_id("menuClassNameTag");
    if (name_el) html_dom_set_text(name_el, c ? c->name : "");
    if (sub_el) {
        StrBuf sb = {0};
        if (c && c->weapon && c->weapon[0]) {
            sb_append(&sb, "<span style='color:inherit'>");
            sb_append_esc(&sb, c->weapon);
            sb_append(&sb, "</span>");
        }
        char *html = sb_finish(&sb);
        html_dom_set_html(sub_el, html);
        free(html);
    }
    if (tag_el && ui_police_active()) {
        html_dom_set_html(tag_el, "Officer Buddy <i class='material-icons menuClassPlayerPremium'>beenhere</i>");
    } else if (tag_el) {
        StrBuf sb = {0};
        sb_append(&sb, "<span class='menuClassPlayerName'>");
        sb_append_esc(&sb, g_ui.client->menu_name[0] ? g_ui.client->menu_name : "Guest");
        sb_append(&sb, "</span>");
        char *html = sb_finish(&sb);
        html_dom_set_html(tag_el, html);
        free(html);
    }
    menu_schedule_rect();
}

static void bubble_hide(void *user) {
    (void) user;
    g_ui.bubble_timer = 0;
    set_style("bubbleContainer", "transform", "scale(0.0)");
}

void menu_show_speech_bubble(const char *text, const double ms) {
    HtmlElem b = by_id("txtBubble"), c = by_id("bubbleContainer");
    if (!b || !c) return;
    StrBuf sb = {0};
    sb_append_esc(&sb, text);
    char *html = sb_finish(&sb);
    html_dom_set_html(b, html);
    free(html);
    html_dom_set_style(c, "transform", "scale(1.0)");
    if (g_ui.bubble_timer) html_timer_cancel(g_ui.bubble_timer);
    g_ui.bubble_timer = html_timer_add(ms > 0.0 ? ms : SPEECH_BUBBLE_MS, bubble_hide, NULL);
}

void menu_set_class_index(const int index, const int silent) {
    (void) silent; // the class index lives in Client directly; nothing to forward
    g_ui.client->menu_class_index = index;
    char text[16];
    snprintf(text, sizeof(text), "%d", index);
    html_storage_set("classindex", text);
    menu_update_class_box();
}

// window.selectClass(i): bubble, remember, back to the loadout window.
void menu_select_class(const int index) {
    ui_loadout_class_bubble(index); // Game.classes[i].txts, else "Let's Go!"... one time in three
    menu_set_class_index(index, 0);
    menu_show_window(3, 0);
}

void menu_select_map(const int index) {
    g_ui.selected_map = index; // the client may ignore it (the map is the server's)
}

// ---------------------------------------------------------------- windows

static void close_window_state(void) { g_ui.active_window = MENU_WINDOW_NONE; }

// window.showWindow(id). Note: like the JS, closeWindowState() runs before the
// "same window" test, so re-selecting the active window re-renders it.
void menu_show_window(const int id, const int force) {
    if (id != MENU_WINDOW_NONE) menu_idle_reset(); // showWindow(id >= 0): CONTROLS.idleTimer = 0
    if (id == MENU_WINDOW_NONE) {
        menu_toggle_window(0);
        return;
    }
    if (!ui_window_key(id)) {
        menu_log("window %d is not available in KrunkNative", id);
        return;
    }
    const int was_open = menu_window_is_open();
    const int previous = g_ui.active_window;
    close_window_state();
    if (force || !was_open || id != previous) {
        if (!g_ui.menu_open) menu_apply_open(1);
        HtmlElem mw = by_id("menuWindow");
        HtmlElem header_el = by_id("windowHeader");
        const char *header = "";
        int sticky = 0;
        char *html = ui_window_generate(id, &header, &sticky);
        if (mw) {
            html_dom_set_html(mw, html ? html : "");
            html_dom_set_class(mw, "stickyHeader", sticky);
            html_dom_set_style(mw, "overflow-y", "auto");
            html_dom_set_style(mw, "width", "");
        }
        free(html);
        g_ui.active_window = id;
        if (header_el) html_dom_set_html(header_el, header);
        menu_toggle_window(1);
        ui_window_post(id);
    } else {
        menu_toggle_window(0);
    }
}

// UI.toggleWindow(j)
void menu_toggle_window(const int open) {
    set_style("menuClassContainer", "visibility", "");
    set_display("supportMapCreator", "none");
    set_display("windowHolder", open ? "block" : "none");
    set_display("baseLinks", open ? "none" : "block");
    menu_update_instructions();
    if (!open) menu_close_window();
    menu_schedule_rect();
}

void menu_close_window(void) {
    close_window_state();
    set_display("windowHolder", "none");
    HtmlElem mw = by_id("menuWindow");
    if (mw) html_dom_set_html(mw, "");
}

void menu_rerender_if_open(const int id) {
    if (g_ui.active_window == id && menu_window_is_open()) menu_show_window(id, 1);
}

// #menuWindow .menuTabNew: windows[3].switchTab / windows[9].switchTab
void menu_set_active_tab(const int n) {
    HtmlElem tabs[32];
    const int count = html_dom_query_all(0, "#menuWindow .menuTabNew", tabs, 32);
    for (int i = 0; i < count; i++) html_dom_set_class(tabs[i], "tabANew", i == n);
}

// The Account window's username field doubles as the guest nickname.
void menu_commit_account_name(void) {
    HtmlElem input = by_id("accName");
    if (!input) return;
    char raw[256] = "";
    html_dom_get_value(input, raw, sizeof(raw));
    raw[sizeof(raw) - 1] = 0;
    // .replace(/[^\x20-\x7E]/g, '').trim().slice(0, 14)
    char clean[256];
    size_t n = 0;
    for (const char *p = raw; *p; p++) {
        if ((unsigned char) *p >= 0x20 && (unsigned char) *p <= 0x7E) clean[n++] = *p;
    }
    clean[n] = 0;
    char *start = clean;
    while (*start == ' ') start++;
    char *end = start + strlen(start);
    while (end > start && end[-1] == ' ') *--end = 0;
    if (strlen(start) > 14) start[14] = 0;

    HtmlElem resp = by_id("accResp");
    if (!*start) {
        if (resp) html_dom_set_text(resp, "Enter a username");
        return;
    }
    copy_str(g_ui.client->menu_name, sizeof(g_ui.client->menu_name), start);
    g_ui.name_set = 1;
    html_storage_set("knName", start);
    menu_update_class_box();
    menu_show_window(MENU_WINDOW_NONE, 0);
}

// ---------------------------------------------------------------- connection / play

static void connect_timer_fire(void *user) {
    (void) user;
    g_ui.connect_timer = 0;
    if (g_ui.connected || g_ui.connect_error[0]) return;
    snprintf(g_ui.connect_error, sizeof(g_ui.connect_error), "%s", CONNECT_ERROR_DEFAULT);
    snprintf(g_connect_code, sizeof(g_connect_code), "000001"); // Krunker: io connect error -> disconnect(null, "000001")
    menu_update_instructions();
}

static void arm_connect_timer(void) {
    if (g_ui.connect_timer) html_timer_cancel(g_ui.connect_timer);
    g_ui.connect_timer = html_timer_add(CONNECT_TIMEOUT_MS, connect_timer_fire, NULL);
}

// The old `connect {connected, error}` event, applied directly.
static void menu_set_connect(const int connected, const char *error, const char *code) {
    Client *client = g_ui.client;
    g_ui.connected = !!connected;
    if (connected) g_ui.connect_error[0] = 0;
    else snprintf(g_ui.connect_error, sizeof(g_ui.connect_error), "%s", error && error[0] ? error : CONNECT_ERROR_DEFAULT);
    snprintf(g_connect_code, sizeof(g_connect_code), "%s", !connected && code && code[0] ? code : "000000");
    if (connected) menu_idle_reset(); // Krunker's retry link reloads the page: a fresh idle timer
    if (g_ui.connect_timer) {
        html_timer_cancel(g_ui.connect_timer);
        g_ui.connect_timer = 0;
    }
    if (!connected) g_ui.ready = 0; // the next INIT (reconnect) makes the client ready again
    client->menu_connect_sent = connected ? 1 : 0;
    menu_update_top_bar();
    menu_update_instructions();
}

// Enter the game for a `play` that arrived before the map was ready (the INIT packet
// is processed by client_tick_net on a later frame).
static void menu_try_play(Client *client) {
    if (!client->menu_play_pending || !client->game.ready) return;
    if (!client_is_connected(client)) {
        client->menu_play_pending = 0;
        return;
    }
    client->menu_play_pending = 0;
    client_set_menu_open(client, 0);
    client_enter_game(client);
}

// The old `play {classIndex, offline}` call.
static void menu_request_play(const int offline) {
    Client *client = g_ui.client;

    // `offline:true` (Host Game -> Start Server) with no server: play the offline game
    // in-process for this session (net/local_game.c) and stop the transport from retrying.
    if (offline && !client_is_connected(client)) {
        client->net_should_quit = 1;
        if (!local_game_start(client)) {
            fprintf(stderr, "[menu] failed to start the offline game\n");
            return;
        }
        printf("[menu] started the offline game (in-process)\n");
    }

    if (!client_is_connected(client)) return; // the page shows the connection error screen

    // The map may still be loading (INIT not processed yet): enter once game.ready.
    client->menu_play_pending = 1;
    menu_try_play(client);
}

void menu_play(void) {
    if (!can_play()) return;
    request_pointer_lock_if_web();
    menu_request_play(0);
    menu_request_open(0);
}

// Host Game -> "Start Server": play on the embedded local server (no network server
// needed). C starts it when nothing is connected and spawns once the map is ready;
// the page shows CONNECTING... in between.
void menu_host_offline(void) {
    menu_close_window();
    g_ui.connect_error[0] = 0;
    arm_connect_timer();
    menu_update_instructions();
    request_pointer_lock_if_web();
    menu_request_play(1);
}

// The error screen's "here" link: ask for an immediate reconnect (natively the net
// thread retries on its own every 2 s anyway).
void menu_retry_connect(void) {
    g_ui.connect_error[0] = 0;
    arm_connect_timer();
    menu_update_instructions();
    if (!client_is_connected(g_ui.client)) client_net_request_reconnect(g_ui.client);
}

// Main-thread half of the connection events: consume the NetStatus the transport left
// under net_lock (events.c), keep net_connected truthful and update the page.
static void menu_poll_connection(Client *client) {
    char error[sizeof(client->net_status_error)];
    char code[sizeof(client->net_status_code)];

    pthread_mutex_lock(&client->net_lock);
    const int status = client->net_status;
    client->net_status = NET_STATUS_NONE;
    memcpy(error, client->net_status_error, sizeof(error));
    memcpy(code, client->net_status_code, sizeof(code)); // "(Error 0x<code>)": 000005 idle kick, 000001 connect error
    pthread_mutex_unlock(&client->net_lock);

    if (status == NET_STATUS_CONNECTED) {
        client->net_connected = 1;
    } else if (status == NET_STATUS_FAILED) {
        client->net_connected = 0;

        if (!client->offline) {
            // The session is gone: drop its players (the next INIT starts fresh) so the
            // player can enter again after the transport reconnects, and show the menu
            // (Krunker's disconnect() shows the error screen over it).
            if (client->game.player_count) client_clear_players(client);
            client->in_game = 0;
            client->spawn_requested = 0;
            client->menu_play_pending = 0;
            if (html_ui_available() && !html_ui_is_menu_open()) client_set_menu_open(client, 1);
        }
    }

    if (!html_ui_available() || !g_ui.dom_ready) return;

    if (client_is_connected(client)) {
        if (client->menu_connect_sent != 1) {
            menu_set_connect(1, NULL, NULL);
            client->menu_connect_error[0] = 0;
        }
    } else if (status == NET_STATUS_FAILED) {
        const double now = glfwGetTime();
        const int changed = client->menu_connect_sent != 0 || strcmp(error, client->menu_connect_error) != 0;

        if (changed || now - client->menu_connect_error_time >= MENU_CONNECT_ERROR_REPEAT_SECONDS) {
            menu_set_connect(0, error, code);
            memcpy(client->menu_connect_error, error, sizeof(client->menu_connect_error));
            client->menu_connect_error_time = now;
        }
    }
}

// ---------------------------------------------------------------- state mirror

static int menu_state_changed(const Client *client) {
    const Player *me = client->me;
    const int in_game = client->in_game && me && me->active;
    const int health = me ? (int) me->health : 0;

    return !g_ui.last.valid ||
        g_ui.last.in_game != in_game ||
        g_ui.last.connected != client_is_connected(client) ||
        g_ui.last.ready != client->game.ready ||
        g_ui.last.map_index != client->game.current_map_index ||
        g_ui.last.class_index != client->menu_class_index ||
        g_ui.last.health != health;
}

// The old `state` event: top bar, class box and instructions follow the Game struct.
static void menu_apply_state(Client *client) {
    const Player *me = client->me;
    const int in_game = client->in_game && me && me->active;
    const int health = me ? (int) me->health : 0;

    g_ui.in_game = in_game;
    g_ui.connected = client_is_connected(client);
    g_ui.ready = client->game.ready;
    snprintf(g_ui.map_name, sizeof(g_ui.map_name), "%s", client_map_name(client));
    snprintf(g_ui.mode_name, sizeof(g_ui.mode_name), "%s", client_mode_name(client));
    if (g_ui.connected) g_ui.connect_error[0] = 0;

    menu_update_top_bar();
    menu_update_class_box();
    menu_update_instructions();
    menu_update_info_holder();
    if (g_ui.active_window == 3) menu_rerender_if_open(3);

    g_ui.last.in_game = in_game;
    g_ui.last.connected = g_ui.connected;
    g_ui.last.ready = client->game.ready;
    g_ui.last.map_index = client->game.current_map_index;
    g_ui.last.class_index = client->menu_class_index;
    g_ui.last.health = health;
    g_ui.last.valid = 1;
}

// The old `classes` / `maps` events: re-read Game when its tables change.
static void menu_refresh_tables(const Client *client) {
    const ClassConfig *classes = client->game.classes ? client->game.classes : g_classes;
    const size_t class_count = client->game.class_count ? client->game.class_count : sizeof(g_classes) / sizeof(g_classes[0]);
    if (classes != g_ui.last_classes || class_count != g_ui.last_class_count) {
        menu_build_classes();
        menu_update_class_box();
        menu_rerender_if_open(6);
        menu_rerender_if_open(3);
    }
    if ((const void *) client->game.maps != g_ui.last_maps || client->game.map_count != g_ui.last_map_count) {
        menu_build_maps();
        menu_rerender_if_open(10);
        menu_rerender_if_open(MENU_WINDOW_HOST);
    }
}

// ---------------------------------------------------------------- DOM set-up

static void setup_dom(void) {
    // Krunker turns these off once the game has loaded / consent was given.
    set_display("loadingBg", "none");
    set_display("consentBlock", "none");
    set_display("bdayBlock", "none");
    set_display("menuFPSDisplay", "none");
    set_style("vignette", "opacity", "0");
    set_display("teamSelector", "none");
    // The class preview canvas keeps the size the three.js renderer gives it; it stays
    // transparent and the engine draws the player model behind it.
    set_style("classPreviewCanvas", "width", "660px");
    set_style("classPreviewCanvas", "height", "350px");
    set_style("bubbleContainer", "transform", "scale(0.0)");
#ifndef __EMSCRIPTEN__
    // KrunkNative: the native build is the desktop client already, so no "Download
    // Client" button (window 25) in the header; the web build keeps Krunker's. Removed
    // rather than hidden: main.css forces `.imageButton { display: inline-block !important }`.
    // --render-scenario keeps it: uidiff compares the header with v3.1.1's.
    const HtmlElem download = ui_scenario_active() ? 0 : html_dom_query(0, ".downloadClient");
    if (download) html_dom_remove(download);
#endif

    HtmlElem el;
    if ((el = by_id("classPreviewCanvas"))) html_dom_observe_resize(el, on_observed_resize, NULL);
    if ((el = by_id("menuClassContainer"))) html_dom_observe_resize(el, on_observed_resize, NULL);
    if ((el = by_id("uiBase"))) html_dom_observe_resize(el, on_observed_resize, NULL);

    char saved[64];
    if (html_storage_get("classindex", saved, sizeof(saved)) >= 0) {
        char *end;
        const long index = strtol(saved, &end, 10);
        if (end != saved) g_ui.client->menu_class_index = (int) index;
    }
    if (html_storage_get("knName", saved, sizeof(saved)) >= 0 && saved[0]) {
        copy_str(g_ui.client->menu_name, sizeof(g_ui.client->menu_name), saved);
        g_ui.name_set = 1;
    }
}

// init() + the `bridgeReady` handler: runs once the page's DOM exists.
static void menu_dom_init(void) {
    ui_windows_load();
    ui_settings_load();
    ui_settings_collect_defaults();
    setup_dom();
    menu_build_classes();
    menu_build_maps();
    g_ui.last_instructions[0] = 0;
    menu_scale_base();
    menu_apply_open(html_ui_is_menu_open());
    menu_update_top_bar();
    menu_update_class_box();
    menu_update_instructions();

    // bridgeReady
    g_ui.dom_ready = 1;
    if (!g_ui.connected && !g_ui.connect_error[0]) arm_connect_timer();
    ui_settings_push_saved();
    menu_update_instructions();
    g_ui.last_rect[0] = 0;
    menu_send_preview_rect();
}

// ---------------------------------------------------------------- client entry points

void client_set_menu_open(Client *client, const int open) {
    html_ui_set_menu_open(open);
    menu_apply_open(open);

    // isMobile: no pointer lock (UsePointerLock false); emscripten's GLFW would request one.
    if (!ui_mobile_active()) glfwSetInputMode(client->window, GLFW_CURSOR, open ? GLFW_CURSOR_NORMAL : GLFW_CURSOR_DISABLED);
    client->mouse_state.locked = !open;
    client->menu_lock_grace_until = open ? 0.0 : glfwGetTime() + MENU_LOCK_GRACE_SECONDS;

    double x, y;
    glfwGetCursorPos(client->window, &x, &y);
    client->mouse_state.last_pos.x = (float) x; // avoid a camera jump on re-lock
    client->mouse_state.last_pos.y = (float) y;
}

void client_menu_bind(Client *client) {
    client->menu_class_index = 0;
    client->menu_lock_grace_until = 0.0;
    client->menu_connect_sent = -1;
    client->menu_connect_error[0] = 0;
    client->menu_connect_error_time = 0.0;
    client->menu_play_pending = 0;
    if (!client->menu_name[0]) strcpy(client->menu_name, "Guest");

    memset(&g_ui, 0, sizeof(g_ui));
    g_ui.client = client;
    g_ui.menu_open = 1;
    g_ui.selected_map = -1;
    g_ui.settings.sensitivity_x = g_ui.settings.sensitivity_y = 1.0f;
    g_ui.settings.aim_sensitivity_x = g_ui.settings.aim_sensitivity_y = 1.0f;
    g_ui.settings.fov = 90.0f;
    g_ui.settings.ads_fov_mlt = 1.0f;
    g_ui.settings.scale_ui = 0.8f;
    g_ui.settings.ui_scaling = 1;
    g_ui.settings.resolution_pct = 100;
    g_ui.settings.speed_scale = g_ui.settings.dmg_scale = 1.0f;
    g_ui.settings.ambient_volume = g_ui.settings.guns_volume = g_ui.settings.player_volume = g_ui.settings.ui_volume = 1.0f;

    ui_handlers_install(&g_ui);
}

// Per frame: mirror state changes into the page and reopen the menu when the pointer
// lock is lost behind our back (browser ESC, alt-tab, ...).
void client_menu_tick(Client *client) {
    menu_poll_connection(client); // before the HTML check: net_connected must track the transport even with the stub
    if (!html_ui_available()) return;

    if (!g_ui.dom_ready && html_dom_by_id("uiBase")) menu_dom_init();

    menu_try_play(client);
    menu_idle_tick(client);

    if (g_ui.dom_ready) {
        menu_refresh_tables(client);
        if (menu_state_changed(client)) menu_apply_state(client);

        // window.addEventListener('resize', scaleBase) fallback: poll the viewport too.
        float w, h, dpr;
        html_dom_viewport(&w, &h, &dpr);
        if (w > 0.0f && h > 0.0f && (w != g_ui.viewport_w || h != g_ui.viewport_h)) menu_scale_base();
    }

    client_hud_html_tick(client, (float) glfwGetTime()); // HTML HUD (hud_ui.c)
    ui_console_tick(client); // developer console: overlay, autoexec, queued server commands, net_graph

    // Round end: the end screen frees the pointer (Krunker exits pointer lock so its
    // results can be clicked) and relocks when the next round starts.
    static int end_interactive;
    const int end_now = hud_ui_end_screen_interactive();
    if (end_now != end_interactive) {
        end_interactive = end_now;
        if (!html_ui_is_menu_open()) {
            if (!ui_mobile_active()) glfwSetInputMode(client->window, GLFW_CURSOR, end_now ? GLFW_CURSOR_NORMAL : GLFW_CURSOR_DISABLED);
            client->mouse_state.locked = !end_now;
            client->menu_lock_grace_until = glfwGetTime() + MENU_LOCK_GRACE_SECONDS;
        }
    }

    if (!end_now && !html_ui_is_menu_open() && glfwGetTime() >= client->menu_lock_grace_until && !client_pointer_locked(client)) {
        client_set_menu_open(client, 1);
    }
}
