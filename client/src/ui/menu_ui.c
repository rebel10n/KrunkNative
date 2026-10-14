// Menu state machine over the DOM-ops API: the C port of Krunker v7.1.4's menu JavaScript
// (src/index.js, src/libs/UI.js) plus the net/state/connection glue. No JS runs natively;
// on the web ui/js/kn-shim.js only forwards page events here. The page is Krunker's own
// markup + main.css; this mirrors what the real client does in its loaded, logged-out,
// menu-open state (a returning player: v7's fullMenu(), no FTUE gradual menu):
//   - scaleBase()                   fit the 1700x900 * uiCustomScale design space
//   - UI.toggleMenu/toggleMenuHider menu open/close inline styles
//   - updatePlayInstructions()      #instructions = "CLICK TO PLAY" / the loading ring /
//                                   the "Lost Connection" screen in #instructionsUpdate
//   - showWindow/closeWindow        #windowHolder / #windowHeader / #menuWindow
//   - updateClassPreview()          the class box (#menuClassContainer) population
// Window contents come from ui/windows (ui_windows.c); state, classes, maps, connection
// and menu open/close are direct DOM writes.
#include <ui_logic.h>
#include <game_reload.h> // F5: game_reload_tick
#include <badges.h> // WP-E part 2c: the custom badge in the class box
#include <ks_client.h> // ks_client_pointer_free
#include <backend_ui.h>
#include <html_ui.h>
#include <weapons.h>
#include <ui_mobile.h> // isMobile: lockless play (client/src/ui/ui_mobile.c)
#include <gamepad.h> // Gamepad.active: "PRESS START" (client/src/gamepad.c)
#include <spectate.h> // the Spectate switch: "CLICK TO SPECTATE" (client/src/spectate.c)
#include <console.h> // developer console overlay (client/src/ui/ui_console.c)
#include <flavor_ui.h> // Krunker Strike menu branding (client/src/ui/flavor_ui.c)
#include <ui_interact.h> // the interact prompt (client/src/ui/ui_interact.c)
#include <game_config.h> // Game.config.classes / isCustom (the class list)
#include <client_settings.h> // Exit menu item + Settings > Client (client/src/ui/ui_client_settings.c)
#include <lobby_ui.h> // the comp menu (comp_ui.c)
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <update_ui.h>
#include <download_ui.h> // showWindow(25) / ?download (client/src/ui/download_ui.c)
#include <v10_ui.h> // the v10 page's Ranked / Turf Wars / Leaderboards entries
#include <stdlib.h>
#include <string.h>
#include <ruleset.h>
#include <v10_menu.h> // the v10 page's menu shell (client/src/ui/v10_menu.c)
#include <render_settings.h> // disable_rarity_anim
#include "../viewer/viewer_store.h" // the class box: a loadout weapon's skin name, rarity, thumbnail
#include <app_mode.h> // the game page owns the pointer lock (not the hub / editor / viewer)
#include <pointer_lock.h> // "should the pointer be locked" (client/src/pointer_lock.c)
#include <online_mode.h> // live sign-in: the session's rights end with it
#include <nettspend.h>   // KN-only class icon art (ui/img/classes/)

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
// Region name override (ui_scenario.c `state.region`); "" = MENU_REGION_NAME.
static char g_region_label[64];
static char g_connect_code[8]; // Krunker's error code on the connection error screen ("000000")
static int g_connect_close_code;        // ... its "Code:" (a lost session's WebSocket close code), 0 none
static char g_connect_close_reason[24]; // ... its "Reason:" (a lost session's cause), "" = "0"
static int g_error_screen;      // the Lost Connection screen shows (menu_apply_error_screen)

// Krunker v7.1.4 idle kick (index.js gameLoop + config.kickTimer 120000): while the menu is
// up (menuHolder shown), not spectating, on a production page, not the game host
// (isGameHost), not a developer account, `!Game.config` and the mode has neither `noKick` nor
// `noClientKick`, CONTROLS.idleTimer += delta; at 120 s disconnect(i18n "player.inactive",
// "000005"). The timer resets on any key, opening a window (showWindow), showMenu(), entering
// the game and every INIT (a map change).
// v7 bug, not replicated: Game.config is always an object there (game.js `config = {}`,
// applyConfig), so `!Game.config` disables the kick everywhere. KrunkNative reads it as its
// v3.1.1 meaning, "not a hosted game's config": no menu kick in custom games
// (game_config_is_custom, which also covers their host), the kick stays in public lobbies.
#define MENU_KICK_TIMER_MS 120000.0
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
    const int remote = client->net_connected && !client->offline && !client->game.is_local; // !isGameHost (offline: the host)
    const int no_kick = client->game.mode && (client->game.mode->config.no_kick || client->game.mode->config.no_client_kick);
    const AccountInfo *account = account_get();
    const int exempt = no_kick || spectate_active() || (account && account->is_dev) || game_config_is_custom(&client->game);
    if (!g_ui.menu_open || !remote || exempt || !idle_kick_prod(client, &limit_ms)) return;
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

#ifdef __EMSCRIPTEN__
// Pointer lock on the web. Browsers grant it asynchronously and may refuse it: no user
// activation (unless the page itself released the last lock), and Chrome refuses every
// request for ~1.25 s after the user left the lock with Escape. A refusal fires
// pointerlockerror (counted here) and rejects the promise Chrome returns, which is
// swallowed instead of logging "Uncaught (in promise)". Used instead of emscripten's
// glfwSetInputMode(GLFW_CURSOR_DISABLED), which requests the lock with no such handling.
EM_JS(void, web_pointer_lock, (int on), {
    var canvas = document.getElementById('canvas');
    if (!Module.knLockWatch) {
        Module.knLockWatch = true;
        Module.knLockErrors = 0;
        Module.knLockPending = 0;
        document.addEventListener('pointerlockerror', function () { Module.knLockErrors++; Module.knLockPending = 0; });
        document.addEventListener('pointerlockchange', function () { if (document.pointerLockElement) Module.knLockPending = 0; });
    }
    if (!on) {
        Module.knLockPending = 0;
        if (document.pointerLockElement && document.exitPointerLock) document.exitPointerLock();
        return;
    }
    if (!canvas || !canvas.requestPointerLock || document.pointerLockElement === canvas) return;
    // one request at a time: a second one while the first is pending only fires a
    // spurious pointerlockchange (pointerLockElement still null)
    var now = performance.now();
    if (Module.knLockPending && now - Module.knLockPending < 1500) return;
    Module.knLockPending = now;
    try {
        var p = canvas.requestPointerLock();
        if (p && p.catch) p.catch(function () {});
    } catch (e) { Module.knLockErrors++; Module.knLockPending = 0; }
});

EM_JS(int, web_pointer_lock_errors, (void), { return Module.knLockErrors | 0; });

// Krunker only enters the game from its pointerlockchange handler (CONTROLS: enabled ->
// enterGame(), toggleControlUI(true)): Play leaves the menu up until the lock is held,
// so a refused request (above) keeps the menu instead of showing a game whose mouse
// does nothing until the lock-loss check reopens the menu a second later.
#define MENU_LOCK_WAIT_SECONDS 5.0 // a request the browser never answers
static struct {
    int waiting;       // Play / Escape asked to leave the menu: close it once locked
    int errors;        // knLockErrors when the request was made
    double since;
    int denied_seen;   // knLockErrors already acted on by client_menu_tick
} g_lock;
#endif

static void request_pointer_lock_if_web(void) {
    if (ui_mobile_active()) { // no pointer lock on phones: fullscreen + landscape instead
        ui_mobile_play_gesture();
        return;
    }
#ifdef __EMSCRIPTEN__
    if (!g_lock.waiting) g_lock.errors = web_pointer_lock_errors();
    g_lock.since = glfwGetTime();
    web_pointer_lock(1);
#endif
}

// 1 when entering the game may proceed now (natively, on phones and once the web pointer
// lock is held); a refused or unanswered web request drops the Play request instead.
static int menu_lock_ready(Client *client) {
#ifdef __EMSCRIPTEN__
    if (client_pointer_locked(client)) return 1;
    if (web_pointer_lock_errors() != g_lock.errors || glfwGetTime() - g_lock.since > MENU_LOCK_WAIT_SECONDS) {
        client->menu_play_pending = 0;
        g_lock.waiting = 0;
    }
    return 0;
#else
    (void) client;
    return 1;
#endif
}

// ---------------------------------------------------------------- pointer lock owner

// The one place the game page's pointer lock is decided and applied (pointer_lock.h): every menu /
// end-screen / console / focus / KrunkScript change calls client_pointer_update, and main.c once a
// frame before it samples the mouse. Natively the GLFW cursor mode follows the wanted lock at once
// (level: nothing else may leave it out of step); on the web the canvas lock is requested when it
// becomes wanted (edge: a request needs a user gesture, so it is not retried every frame) and a lock
// that is not granted brings the menu with CLICK TO PLAY back (client_menu_tick).
static struct {
    PointerLockTracker tracker;
    PointerLockWant want; // the last step
    int applied;          // want.lock as last applied (-1: not applied on this page yet)
    int unfocused;        // the native window lost the keyboard focus (glfwSetWindowFocusCallback)
} g_ptr = {.applied = -1};

static int pointer_owner_active(void) {
    // HTML_ENGINE=OFF keeps main.c's click-to-lock; the hub / editor / viewer pages own their cursor
    return html_ui_available() && app_mode() == APP_MODE_GAME;
}

static void pointer_reset_last_pos(Client *client) {
    double x, y;
    glfwGetCursorPos(client->window, &x, &y);
    client->mouse_state.last_pos.x = (float) x; // a cursor mode change moves the cursor: no look jump
    client->mouse_state.last_pos.y = (float) y;
}

void client_pointer_update(Client *client) {
    if (!client) return;
    PointerLockInputs in = {0};
    in.game = (unsigned char) pointer_owner_active();
    in.menu_open = (unsigned char) html_ui_is_menu_open();
    in.console_open = (unsigned char) ui_console_shown(); // only once the overlay is on screen: never a freed mouse with no console
    in.focused = (unsigned char) !g_ptr.unfocused;
    in.end_up = (unsigned char) hud_ui_end_screen_up();
    in.end_results = (unsigned char) hud_ui_end_screen_interactive();
    in.script_unlock = (unsigned char) ks_client_input_unlocked();
    in.script_free = (unsigned char) (ks_client_pointer_free() && !ks_client_input_unlocked());
    const int had_controls = g_ptr.want.controls;
    g_ptr.want = pointer_lock_step(&g_ptr.tracker, &in);
    if (!in.game) {
        g_ptr.applied = -1;
        return;
    }
    client->mouse_state.locked = g_ptr.want.controls;
    // The controls are back (menu closed, the next round, a respawn): the keyboard is the game's
    // again, as after the browser's click; a chat line typed on the end screen / death card kept
    // the movement keys otherwise.
    if (!had_controls && g_ptr.want.controls) ui_controls_release_focus();
    // isMobile: UsePointerLock = false (CONTROLS.locklessChange); emscripten's GLFW would request one
    if (ui_mobile_active() || !client->window) return;
    const int lock = g_ptr.want.lock;
#ifdef __EMSCRIPTEN__
    const int change = lock != g_ptr.applied;
    if (change) web_pointer_lock(lock); // relock without a gesture: allowed after exitPointerLock()
#else
    const int mode = lock ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL;
    const int change = lock != g_ptr.applied || glfwGetInputMode(client->window, GLFW_CURSOR) != mode;
    if (change) glfwSetInputMode(client->window, GLFW_CURSOR, mode);
#endif
    if (change) {
        pointer_reset_last_pos(client);
        if (lock) client->menu_lock_grace_until = glfwGetTime() + MENU_LOCK_GRACE_SECONDS;
    }
    g_ptr.applied = lock;
}

// main.c: this frame's mouse movement may turn the camera (v7 f2137: only while CONTROLS.enabled,
// i.e. under the pointer lock). A free cursor never looks: menu, end screen, console, freeMouse.
int client_pointer_look_allowed(const Client *client) {
    if (!client) return 0;
    if (!pointer_owner_active()) return client->mouse_state.locked; // main.c's own lock (no HTML UI)
    return pointer_lock_look_allowed(&g_ptr.want, client_pointer_locked(client));
}

// A press on the game view (no page under the mouse). CONTROLS mousedown f2139: with the controls
// off (the end title after a menu / alt-tab) it is toggle(true) and nothing else: the lock again;
// on the web also while the controls are on but the browser dropped the lock (its Escape during
// the end title, where no menu comes). Returns 1 when the press was taken for that.
int client_pointer_click(Client *client) {
    if (!client || !pointer_owner_active() || ui_mobile_active()) return 0;
    if (g_ptr.want.controls && (!g_ptr.want.lock || client_pointer_locked(client))) return 0;
    if (html_ui_is_menu_open() || hud_ui_end_screen_interactive() || ui_console_is_open()) return 0;
    pointer_lock_resume(&g_ptr.tracker);
#ifdef __EMSCRIPTEN__
    g_ptr.applied = -1; // request again (inside this click: the user gesture the browser needs)
#endif
    client_pointer_update(client);
    return 1;
}

// glfwSetWindowFocusCallback (main.c): no lock while another window has the keyboard.
void client_pointer_focus(Client *client, const int focused) {
    g_ptr.unfocused = !focused;
    client_pointer_update(client);
}

// The window changed monitor / size (F11, Settings > windowMode: ui_render_settings.c). A disabled
// GLFW cursor keeps its X11 pointer grab (and confinement) from before the switch on some window
// managers; release and take it again on the new geometry when the game has the lock.
void client_pointer_refresh(Client *client) {
    if (!client || !client->window || !pointer_owner_active() || ui_mobile_active()) return;
#ifndef __EMSCRIPTEN__
    if (glfwGetInputMode(client->window, GLFW_CURSOR) == GLFW_CURSOR_DISABLED) glfwSetInputMode(client->window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
#endif
    g_ptr.applied = -1;
    client_pointer_update(client);
}

// The lock went during the end title (alt-tab, main.c client_window_focus_lost). v7 shows no menu
// there (hideUI set UI.active = false): the pointer stays free until a click on the game view.
void client_pointer_end_free(Client *client) {
    pointer_lock_end_free(&g_ptr.tracker);
    client_pointer_update(client);
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

// windows[2] tab 0 lists the classes in Game.config.classes; without a list, resetClasses():
// every class but the hidden ones (Deagler, Survivor), all of them in a sandbox / custom game.
static int menu_class_listed(const Game *game, const ClassConfig *classes, const int index) {
    if (game->config.class_list_count > 0) {
        for (int i = 0; i < game->config.class_list_count; i++) {
            if (game->config.class_list[i] == index) return 1;
        }
        return 0;
    }
    return !classes[index].hide || game_config_is_custom(game);
}

// What menu_class_listed reads, to rebuild the list when it changes (GAME_CONFIG).
static unsigned menu_class_filter_key(const Game *game) {
    unsigned key = 2166136261u ^ (unsigned) game_config_is_custom(game);
    for (int i = 0; i < game->config.class_list_count; i++) key = (key ^ (unsigned) game->config.class_list[i]) * 16777619u;
    return key * 16777619u + (unsigned) game->config.class_list_count;
}
static unsigned g_class_filter_key;

// Mirrors Game.classes as the real class window renders them (name, primary weapon, HP).
static void menu_build_classes(void) {
    const Client *client = g_ui.client;
    const size_t count = client->game.class_count ? client->game.class_count : rules_class_count();
    const ClassConfig *classes = client->game.classes ? client->game.classes : rules_classes();
    const Weapon *const *weapons = client->game.weapons ? (const Weapon *const *) client->game.weapons : rules_weapons();
    const size_t weapon_count = client->game.weapons ? client->game.weapon_count : rules_weapon_count();

    g_ui.class_count = 0;
    g_class_filter_key = menu_class_filter_key(&client->game);
    for (size_t i = 0; i < count && g_ui.class_count < MENU_CLASS_MAX; i++) {
        if (!menu_class_listed(&client->game, classes, (int) i)) continue;
        MenuClass *c = &g_ui.classes[g_ui.class_count++];
        c->index = (int) i;
        c->name = classes[i].name ? classes[i].name : "";
        c->weapon = "";
        if (classes[i].loadout) {
            const int w = classes[i].loadout[0];
            if (w >= 0 && (size_t) w < weapon_count && weapons[w] && weapons[w]->name) c->weapon = weapons[w]->name;
        }
        c->hp = classes[i].health;
        if (!nettspend_class_icon_src(classes[i].icon_index, c->icon, sizeof(c->icon))) // a KN class: ui/img/classes/ (nettspend.h)
            snprintf(c->icon, sizeof(c->icon), "textures/classes/icon_%d.png", classes[i].icon_index);
        // updateClassPreview: every loadout weapon's name; the icon is the last one's preview
        // (weaponConfig.src, the Bomb's cosmetics/3_default)
        c->loadout_count = 0;
        c->preview[0] = 0;
        // (ClassConfig.loadout lists the class weapon only; v7's Rocketeer-style multi-weapon
        // loadouts arrive with WP-B's class table)
        for (int k = 0; classes[i].loadout && k < 1 && c->loadout_count < MENU_CLASS_LOADOUT_MAX; k++) {
            const int w = classes[i].loadout[k];
            if (w < 0 || (size_t) w >= weapon_count || !weapons[w]) continue;
            c->loadout_ids[c->loadout_count] = w;
            c->loadout_names[c->loadout_count++] = weapons[w]->name ? weapons[w]->name : "";
            if (w == 12) snprintf(c->preview, sizeof(c->preview), "textures/previews/cosmetics/3_default.png");
            else if (weapons[w]->src) snprintf(c->preview, sizeof(c->preview), "textures/previews/weapons/%s.png", weapons[w]->src);
        }
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

// The class preview's canvas box in whole device pixels: class_preview.c renders a bitmap of
// exactly w x h device px, which the page draws 1:1 over the box (natively the page is laid out
// in framebuffer px, dpr 1; on the web dpr = devicePixelRatio, e.g. 1.25 under 125% scaling).
static void preview_device_rect(const HtmlRect *r, const float dpr, int *x, int *y, int *w, int *h) {
    *x = (int) lroundf(r->x * dpr);
    *y = (int) lroundf(r->y * dpr);
    *w = (int) lroundf(r->w * dpr);
    *h = (int) lroundf(r->h * dpr);
}

// sendPreviewRect: where #classPreviewCanvas is and whether it shows. The preview is the canvas's
// own content (html_dom_canvas_gl), painted at its place in the page, so the page's shades
// (#instructionHolder's 50%, #popupBack's 75% above it) dim around and over it as in Krunker.
void menu_send_preview_rect(void) {
    HtmlElem canvas = by_id("classPreviewCanvas");
    float vw, vh, dpr = 1.0f;
    html_dom_viewport(&vw, &vh, &dpr);
    if (dpr <= 0.0f) dpr = 1.0f;
    if (!g_ui.dom_ready) return;

    int visible = 0, x = 0, y = 0, w = 0, h = 0;
    if (canvas && g_ui.menu_open) {
        HtmlRect r = {0};
        html_dom_get_rect(canvas, &r);
        HtmlElem cc = by_id("menuClassContainer");
        const int hidden = r.w == 0.0f || r.h == 0.0f || (cc && inline_style_is(cc, "visibility", "hidden"));
        visible = !hidden;
        preview_device_rect(&r, dpr, &x, &y, &w, &h);
    }

    char key[96];
    snprintf(key, sizeof(key), "%d,%d,%d,%d,%d,%g", visible, x, y, w, h, dpr);
    if (!strcmp(key, g_ui.last_rect)) return;
    snprintf(g_ui.last_rect, sizeof(g_ui.last_rect), "%s", key);
    client_class_preview_set_rect(g_ui.client, visible, (float) x / dpr, (float) y / dpr, (float) w / dpr, (float) h / dpr, dpr);
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
// settings.tmp.scaleUI (= 1 - scaleUI + 1; scaleUI defaults to 0.7 on desktop in v7.1.4, so
// the space is 2210x1170), into the viewport by scaling #uiBase.
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
    if (!ui_setting_number("scaleUI", &scale_ui) || scale_ui != scale_ui) scale_ui = 0.7; // v7 default (desktop)
    const double custom = 1.0 - scale_ui + 1.0;
    float w, h, dpr;
    html_dom_viewport(&w, &h, &dpr);
    g_ui.viewport_w = w;
    g_ui.viewport_h = h;
    const double dw = custom * 1700.0, dh = custom * 900.0;
    const double sx = w / dw, sy = h / dh;
    char value[64], num[32];
    // v10 scaleBase also publishes the scale on <body> (--ui-scale / --ui-width / --ui-height):
    // the Svelte view stack (.view-wrapper) scales with it (v10_social.c)
    const HtmlElem body = html_dom_body();
    if (body) {
        const double s = sy < sx ? sy : sx;
        snprintf(value, sizeof(value), "%.3f", s);
        html_dom_set_style(body, "--ui-scale", value);
        snprintf(value, sizeof(value), "%.3fpx", sy < sx ? w / sy : dw);
        html_dom_set_style(body, "--ui-width", value);
        snprintf(value, sizeof(value), "%.3fpx", sy < sx ? dh : h / sx);
        html_dom_set_style(body, "--ui-height", value);
    }
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

// The menu chrome of toggleMenu(j) + toggleMenuHider(j); the Lost Connection screen keeps it
// hidden (menu_apply_error_screen).
static void menu_apply_chrome(void) {
    const int on = g_ui.menu_open;
    const int chrome = on && !g_error_screen;
    // UI.toggleMenu(j)
    set_display("menuHolder", chrome ? "block" : "none");
    set_display("speakerDisplay", on || g_error_screen ? "none" : "block");
    // UI.toggleMenuHider(j)
    set_display("menuHider", chrome ? "block" : "none");
    HtmlElem base = by_id("uiBase");
    if (base) {
        html_dom_set_class(base, "onMenu", chrome);
        html_dom_set_class(base, "onDeathScrn", 0);
        html_dom_set_class(base, "onSpect", spectate_active() && !on);
    }
    set_display("gameNameHolder", chrome ? "" : "none");
    set_display("spectButton", chrome ? "" : "none");
}

void menu_apply_open(const int open) {
    if (!!open != g_ui.menu_open) menu_idle_reset(); // showMenu() / entering the game
    g_ui.menu_open = !!open;
    if (!g_ui.dom_ready && !by_id("uiBase")) return; // applied by menu_dom_init once the page exists
    const int on = g_ui.menu_open;

    html_dom_set_class(html_dom_root(), "kn-menu-open", on);
    menu_apply_chrome();
    HtmlElem base = by_id("uiBase");
    if (on) {
        set_display("spinUI", "none");
        set_display("spinUIBtns", "none");
        // toggleMenu(true)
        set_display("endUI", "none");
        if (base) html_dom_set_class(base, "onEndScrn", 0);
        set_display("instructionHolder", "block");
        if (!menu_window_is_open()) set_display("baseLinks", "block");
        set_display("centerLeaderDisplay", "none");
    } else {
        // entering the game: toggleControlUI(true) hides the overlay (hud_ui.c shows #inGameUI)
        set_display("instructionHolder", "none");
        menu_close_window();
    }
    set_style("menuClassContainer", "visibility", "");
    menu_update_info_holder();
    ui_loadout_clear_pops(); // toggleMenuHider(false) / toggleControlUI end with clearPops()
    menu_update_instructions();
    menu_schedule_rect();
}

void menu_request_open(const int open) {
#ifdef __EMSCRIPTEN__
    // Leaving the menu waits for the pointer lock (menu_lock_ready, client_menu_tick).
    if (!open && html_ui_is_menu_open() && !client_pointer_locked(g_ui.client)) {
        request_pointer_lock_if_web();
        g_lock.waiting = 1;
        return;
    }
    g_lock.waiting = 0;
#endif
    client_set_menu_open(g_ui.client, open);
}

// Connected and the map is loaded (INIT processed): the client accepts `play`.
static int can_play(void) {
    return g_ui.connected && g_ui.ready;
}

// updatePlayInstructions() (v7 f2041/f2042): connected and loaded, #instructions is i18n
// "app.play" = "CLICK TO PLAY" ("app.spectate" with the menu's Spectate switch on,
// ".controller" / ".mobile" variants) plus the queue status in #instructionsSubtitle. Before
// that UI.loadSpinner()'s ring. A dead connection is v7's error screen (f2040): #instructions
// hidden and #instructionsUpdate = "Lost Connection", the error, the ID / region lines and the
// instructionsTabs row (Find a Game / Servers / Settings / Changelog); KrunkNative adds one
// tab that plays offline (knHostOffline).
// The Lost Connection screen comes and goes: v7's io_error / io_disconnect run UI.hideUI()
// (toggleMenu(false): #menuHolder, toggleMenuHider(false): #menuHider, .onMenu,
// #gameNameHolder, the Spectate switch #spectButton) and UI.hideDiscon() (#chatHolder,
// #speakerDisplay, #purchaseHolder), then f2040 shows #instructionHolder with the error box
// and hides #menuHolder again, so the box stands alone over the scene. KrunkNative leaves the
// screen without a reload (Find a Game / Reconnect show CONNECTING..., a reconnect clears it),
// so leaving it puts the menu chrome back. Intentional difference: v7's showWindow() runs
// toggleMenu(true), which brings the whole menu (Spectate switch included) back over the error
// box when Servers / Settings / Changelog open; KrunkNative keeps the chrome hidden while the
// error box shows (the windows sit outside #menuHolder).
int menu_error_screen_shown(void) {
    return g_error_screen;
}

static void menu_apply_error_screen(const int on) {
    if (on == g_error_screen) return;
    g_error_screen = on;
    if (on) set_display("purchaseHolder", "none");
    set_display("chatHolder", on ? "none" : "");
    menu_apply_chrome();
    menu_schedule_rect(); // the class preview goes with #menuHolder
}

// The error screen's message in the page's language: the client's own texts (the idle kick is
// v10's disconnect(t("player.inactive")), which says "Disconnected due to inactivity" where v7 and
// the server say "Kicked for inactivity"; the connect timeout is KrunkNative's); a server's
// error text shows as it came (v10 too).
static const char *connect_error_text(const char *error) {
    if (!strcmp(error, "Could not connect to the game server"))
        return I18N("kn.error.connectFailed", "Could not connect to the game server");
    if (!strcmp(error, "Kicked for inactivity")) return I18N("player.inactive", "Disconnected due to inactivity");
    return error;
}

#define MENU_LOAD_SPINNER "<div style='width:100%;text-align:center;transform:scale(1.3);'><div class='lds-ring'><div></div><div></div><div></div><div></div></div></div>"
void menu_update_instructions(void) {
    HtmlElem el = by_id("instructions");
    if (!el) return;
    StrBuf sb = {0};
    const int error = g_ui.dom_ready && !can_play() && g_ui.connect_error[0] && !g_ui.connected;
    if (!g_ui.dom_ready) {
        sb_append(&sb, MENU_LOAD_SPINNER);
    } else if (can_play()) {
        const int spect = spectate_requested();
        sb_append(&sb, gamepad_active() ? (spect ? I18N("app.spectate.controller", "START to SPECTATE") : I18N("app.play.controller", "PRESS START"))
                       : ui_mobile_active() ? (spect ? I18N("app.spectate.mobile", "TAP TO SPECTATE") : I18N("app.play.mobile", "TAP TO PLAY"))
                       : spect ? I18N("app.spectate", "CLICK TO SPECTATE") : I18N("app.play", "CLICK TO PLAY"));
        // the ranked queue status under it (v10794.toUpperCase(), ranked_ui.c)
        if (ranked_ui_status()) {
            sb_append(&sb, "<div id='instructionsSubtitle'>");
            for (const char *p = ranked_ui_status(); *p; p++) sb_appendf(&sb, "%c", *p >= 'a' && *p <= 'z' ? *p - 32 : *p);
            sb_append(&sb, "</div>");
            if (ranked_ui_ready() && ranked_ui_ready()[0]) {
                sb_append(&sb, "<div id='instructionsSubtitle'>");
                sb_append_esc(&sb, ranked_ui_ready());
                sb_append(&sb, "</div>");
            }
        }
    } else if (!error) {
        sb_append(&sb, MENU_LOAD_SPINNER);
    }
    char *html = sb_finish(&sb);
    // the error screen: #instructionsUpdate
    StrBuf eb = {0};
    if (error) {
        eb.len = 0;
        // v7.1.4 f2040: "GAME UPDATING" (a server stopping for an update, or the matchmaker's
        // NoServersMatchQuery) is the Game Updating screen. KrunkNative keeps the error screen's
        // layout for it (no update art / Twitch embeds), says what happened for the server's
        // ERROR and, while servers_ui.c rejoins with a fresh ticket, "Rejoining..."
        const int updating = !strcmp(g_ui.connect_error, "GAME UPDATING");
        sb_appendf(&eb, "<div class='instructionsHeader'>%s</div><div style='color: rgba(255, 255, 255, 0.6);margin-top: 5px;font-size: 28px;'>",
                   updating ? "Game Updating" : "Lost Connection");
        sb_append_esc(&eb, updating && !strcmp(g_connect_code, NET_ERROR_CODE_UPDATING) ? I18N("kn.error.restartingUpdate", "Restarting for an update") : connect_error_text(g_ui.connect_error));
        // KrunkNative: the matchmaker turned a guest away (AccountRequired: a node of a server_bin
        // that still refused guests): log in (the lobby is joined right after, servers_ui.c)
        if (servers_ui_account_required()) {
            char login[192];
            I18N_F(login, "kn.error.loginToJoin", "{0}Log in{1} to join it.", "<a href='#' onclick='showWindow(5);return false'>", "</a>");
            sb_appendf(&eb, "<div style='font-size:20px'>%s</div>", login);
        }
        sb_append(&eb, "</div><div style='margin-top:10px;font-size:15px;color:rgba(255,255,255,0.4)'>ID: 0x");
        sb_append_esc(&eb, g_connect_code[0] ? g_connect_code : "000000");
        // v7.1.4 f2040: "Code: <close code> | Reason: <close reason>" of the socket's close event; here
        // the transport's (events.c: the WebSocket close code, a short cause such as timeout / ice)
        sb_appendf(&eb, " | Code: %d | Reason: ", g_connect_close_code);
        sb_append_esc(&eb, g_connect_close_reason[0] ? g_connect_close_reason : "0");
        sb_append(&eb, "</div><div style='margin-top:10px;font-size:15px;color:rgba(255,255,255,0.4)'>Region: ");
        sb_append_esc(&eb, g_region_label[0] ? g_region_label : MENU_REGION_NAME);
        // while servers_ui.c rejoins with a fresh ticket, its status replaces the extensions hint
        if (servers_ui_rejoining())
            sb_appendf(&eb, "</div><div style='margin-top:10px;font-size:15px;color:rgba(255,255,255,0.8)'>%s</div>", servers_ui_rejoin_text());
        else
            sb_appendf(&eb, "</div><div style='margin-top:10px;font-size:15px;color:rgba(255,255,255,0.4)'>%s</div>",
                       I18N("error.extentions", "If you have any extensions, disable them"));
        // Find a Game: seek a new game (or retry a pinned server: console connect / Direct Connect).
        // v7's row is table-layout: fixed, four equal cells of the box's 610px; KrunkNative's fifth
        // tab (Play Offline) leaves 112px of text per cell, less than "Find a Game" / "Play
        // Offline" take at 16px, so they wrapped onto two lines. The row sizes its cells by their
        // text instead (auto layout, no wrapping); the five labels fit the box with room to spare.
        sb_appendf(&eb, "<div class=\"instructionsTabs\" style=\"table-layout:auto;white-space:nowrap\">\n\t\t\t\t<div class=\"instructionsTab\" onmouseenter=\"playTick();\" onclick=\"playSelect(0.1);knRetry()\">%s</div>"
                        "\n\t\t\t\t<div class=\"instructionsTab\" onmouseenter=\"playTick();\" onclick=\"playSelect(0.1);knHostOffline()\">%s</div>"
                        "\n\t\t\t\t<div class=\"instructionsTab\" onmouseenter=\"playTick();\" onclick=\"playSelect(0.1);showWindow(2)\">Servers</div>"
                        "\n\t\t\t\t<div class=\"instructionsTab\" onmouseenter=\"playTick();\" onclick=\"playSelect(0.1);showWindow(1)\">Settings</div>"
                        "\n\t\t\t\t<div class=\"instructionsTab\" onmouseenter=\"playTick();\" onclick=\"playSelect(0.1);showWindow(35)\">Changelog</div>\n\t\t\t</div>",
                   servers_ui_pinned() ? I18N("kn.error.reconnect", "Reconnect") : "Find a Game", I18N("kn.error.playOffline", "Play Offline"));
    }
    char *update = sb_finish(&eb);
    char key[sizeof(g_ui.last_instructions)];
    snprintf(key, sizeof(key), "%d|%s|%s", error, html, update);
    if (strcmp(key, g_ui.last_instructions)) {
        html_dom_set_html(el, html);
        html_dom_set_display(el, error ? "none" : "");
        HtmlElem upd = by_id("instructionsUpdate");
        if (upd) {
            html_dom_set_html(upd, update);
            html_dom_set_display(upd, error ? "block" : "");
        }
        if (error) set_display("frvrFTUEVideoHolder", "none");
        snprintf(g_ui.last_instructions, sizeof(g_ui.last_instructions), "%s", key);
    }
    free(html);
    free(update);
    menu_apply_error_screen(error);
    set_style("instructionHolder", "pointer-events", "all");
    // Krunker Strike: the spinner states (page loading, connecting) are the brand's loading
    // screen (the Strike curtains over #instructionHolder, flavor_ui.c); no-op for base
    flavor_ui_set_loading(!error && (!g_ui.dom_ready || !can_play()));
}


void menu_set_region_label(const char *name) {
    snprintf(g_region_label, sizeof(g_region_label), "%s", name ? name : "");
    menu_update_top_bar();
}

void menu_update_top_bar(void) {
    HtmlElem lbl = by_id("menuRegionLabel");
    if (lbl) html_dom_set_text(lbl, g_ui.connected ? (g_region_label[0] ? g_region_label : MENU_REGION_NAME) : "...");
}

// The top bar's #menuPingText / #menuPingIcon. v3.1.1 reconcilePlayer writes them on every
// change of the "l" ping while #menuHolder shows: text = ping == -1 ? "-" : ping, icon
// COLORS.ping (src/data/colors.js: low < 50 <= medium < 150 <= high). KrunkNative's ping
// comes in PING_RESULT (one per pong, NET_PING_INTERVAL) instead of "l", so the menu also
// follows it before the first spawn and while dead, and catches up when it reopens.
// Offline, disconnected or before the first sample: the page's own "0" in gray
// (clearPingCounter). -1 = that state; the page starts in it.
#define MENU_PING_COLOR_LOW "rgb(158, 235, 86)"
#define MENU_PING_COLOR_MEDIUM "rgb(255, 213, 66)"
#define MENU_PING_COLOR_HIGH "rgb(255, 75, 66)"
static int g_menu_ping = -1;

static void menu_update_ping(const Client *client) {
#ifdef UI_SCENARIO_SUPPORTED
    if (ui_scenario_active()) return; // the reference captures keep the page's "0"
#endif
    if (!html_ui_is_menu_open()) return;
    const int ping = client->offline || client->game.is_local || !client->net_connected || client->net_ping_ms < 0 ? -1 : client->net_ping_ms;
    // KrunkNative: the tooltip names the game transport (net_notes.c), e.g. "Ping 41 ms over WebRTC"
    static const char *menu_transport;
    const char *transport = client_net_transport_name(client);
    if (transport != menu_transport) {
        menu_transport = transport;
        char title[64];
        if (ping >= 0 || strcmp(transport, "-")) snprintf(title, sizeof(title), "Game transport: %s", transport);
        else title[0] = 0;
        const HtmlElem holder = by_id("menuPingDisplay");
        if (holder) html_dom_set_attr(holder, "title", title);
    }
    if (ping == g_menu_ping) return;
    g_menu_ping = ping;

    char text[16];
    snprintf(text, sizeof(text), "%d", ping < 0 ? 0 : ping);
    const HtmlElem el = by_id("menuPingText");
    if (el) html_dom_set_text(el, text);
    set_style("menuPingIcon", "color", ping < 0 ? "gray" : ping >= 150 ? MENU_PING_COLOR_HIGH : ping >= 50 ? MENU_PING_COLOR_MEDIUM : MENU_PING_COLOR_LOW);
}

// Map load (v7 src/data/map.js): mapInfo.textContent = (custom map ? mode.alias :
// mode.name) + " on " + map.name; the menu's #infoHolder (Contact / Terms / Changelog; KN
// drops v7's Experiments link, ui/README.md) shows once a game session exists (io.connect:
// infoHolder.style.display = null).
void menu_set_game_name(const char *name) {
    HtmlElem info = by_id("mapInfo");
    if (info) html_dom_set_text(info, name ? name : "");
    set_display("infoHolder", name && name[0] ? "" : "none");
}

static void menu_update_info_holder(void) {
#ifdef UI_SCENARIO_SUPPORTED
    if (ui_scenario_active()) return; // the scenario's state.gameName drives it
#endif
    if (!g_ui.connected || !g_ui.menu_open) { // UI.toggleControlUI(j): infoHolder hidden in game
        set_display("infoHolder", "none");
        return;
    }
    char name[128] = "";
    const Client *client = g_ui.client;
    const int custom = client->game.map && client->game.current_map_index < 0; // not an official map: the mode's alias
    const char *mode = client->game.mode ? (custom && client->game.mode->config.alias ? client->game.mode->config.alias : client->game.mode->config.name) : NULL;
    if (g_ui.map_name[0]) snprintf(name, sizeof(name), "%s on %s", mode && mode[0] ? mode : "Free for All", g_ui.map_name);
    HtmlElem info = by_id("mapInfo");
    if (info) html_dom_set_text(info, name);
    set_display("infoHolder", "");
}

// The class box (v7 updateClassPreview): #menuClassName = the class, #menuClassSubtext =
// its loadout weapons (" - " separated), #menuClassIcn = the last loadout weapon's preview,
// #menuClassNameTag = the player tag (level box, name, [clan], badges) or "Guest". f1802: a weapon
// wearing a store skin (skinsLoadout[weapon], as the Loadout tab shows it) is named after the skin
// in its rarity colour (rainbowText for animated rarities) and pictured by the skin's thumbnail,
// so the box shows what the Loadout tab, the class preview and the viewmodel show.
void menu_update_class_box(void) {
    const MenuClass *c = menu_class_at(g_ui.client->menu_class_index);
    HtmlElem name_el = by_id("menuClassName"), sub_el = by_id("menuClassSubtext"), tag_el = by_id("menuClassNameTag");
    if (name_el) html_dom_set_text(name_el, ui_police_active() ? "Officer" : c ? c->name : ""); // vO349.classN (K.P.D)
    char icon[256] = "";
    if (c && c->preview[0]) html_dom_asset_url(c->preview, icon, sizeof(icon));
    StrBuf sb = {0};
    for (int i = 0; c && i < c->loadout_count; i++) {
        if (sb.len) sb_append(&sb, " - ");
        const int skin_index = ui_police_active() ? -1 : ui_loadout_weapon_skin_shown(c->loadout_ids[i]);
        const ViewerSkin *skin = viewer_skin(skin_index);
        const int rarity = skin && skin->rarity >= 0 && skin->rarity < g_viewer_rarity_count ? skin->rarity : -1;
        const int animate = rarity == 6 && !client_render_settings()->disable_rarity_anim; // rarities[r].animate (ui_loadout.c rarity_anim)
        sb_appendf(&sb, "<span %sstyle='color:%s'>", animate ? "class='rainbowText' " : "", rarity >= 0 && g_viewer_rarities[rarity].color ? g_viewer_rarities[rarity].color : "inherit");
        sb_append_esc(&sb, skin && skin->name ? skin->name : c->loadout_names[i]);
        sb_append(&sb, "</span>");
        if (skin) store_skin_preview(skin_index, icon, sizeof(icon)); // v10874 = skin.thumbnail
    }
    char *html = sb_finish(&sb);
    if (sub_el) html_dom_set_html(sub_el, html);
    free(html);
    HtmlElem icn = by_id("menuClassIcn");
    if (icn && icon[0]) html_dom_set_attr(icn, "src", icon);
    set_style("menuClassContainer", "z-index", "99999");
    if (tag_el && ui_police_active()) {
        html_dom_set_html(tag_el, "Officer Buddy <i class='material-icons menuClassPlayerPremium'>beenhere</i>"); // vO349: no lvl / clan
    } else if (tag_el && account_get()) {
        // account: level box, name, [clan], partner / premium / featured badges and the custom
        // badge, each behind its v7 privacy setting (partnerBadge / premiumBadge / verifiedBadge /
        // customBadge); the clan colour is v7's getClanCol (DEV / VIP / CMD only, WP-E part 2c)
        const AccountInfo *acc = account_get();
        const int premium = acc->premium && !ui_setting_is_false("premiumBadge");
        StrBuf sb = {0};
        sb_appendf(&sb, "<div class='menuClassLevelBox' style='color:white;'>%d</div><span class='menuClassPlayerName'>", acc->level);
        sb_append_esc(&sb, premium && acc->alias[0] ? acc->alias : acc->name);
        sb_append(&sb, "</span>");
        if (acc->clan[0]) {
            const char *ccol = !strcmp(acc->clan, "DEV") ? "#40C4FF" : !strcmp(acc->clan, "VIP") ? "#11D445" : !strcmp(acc->clan, "CMD") ? "#993299" : "rgba(255,255,255,0.5)";
            sb_appendf(&sb, "<span class='menuClassPlayerClan' style='color:%s'>[", ccol);
            sb_append_esc(&sb, acc->clan);
            sb_append(&sb, "]</span>");
        }
        if (acc->data && cJSON_GetNumberValue(cJSON_GetArrayItem(acc->data, 19)) && !ui_setting_is_false("partnerBadge")) sb_append(&sb, "<i class='material-icons menuClassPlayerPartner'>verified</i>");
        if (premium) sb_append(&sb, "<i class='material-icons menuClassPlayerPremium'>beenhere</i>");
        if (acc->featured && !ui_setting_is_false("verifiedBadge")) sb_append(&sb, "<i class='material-icons menuClassPlayerVerified'>check_circle</i>");
        const int badge = account_custom_badge();
        if (badge >= 0) {
            char tex[96], url[256];
            html_dom_asset_url(badge_texture(badge, tex, sizeof(tex)), url, sizeof(url));
            sb_appendf(&sb, "<img src='%s' class='menuClassPlayerCustom'>", url);
        }
        char *html = sb_finish(&sb);
        html_dom_set_html(tag_el, html);
        free(html);
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
    ranked_ui_class_changed(index); // setClassIndex: client.queueUpdateClass when in a party
}

// window.selectClass(i): bubble, remember, back to Change Loadout's Loadout tab
// (windows[2].switchTab(1, true)).
void menu_select_class(const int index) {
    ui_loadout_class_bubble(index); // Game.classes[i].txts, else "Let's Go!"... one time in three
    menu_set_class_index(index, 0);
    ui_loadout_select_tab(1);
    comp_ui_class_selected(g_ui.client, index); // updSyncDat("c", i): the comp lobby's class
}

void menu_select_map(const int index) {
    g_ui.selected_map = index; // the client may ignore it (the map is the server's)
}

// ---------------------------------------------------------------- windows

static void close_window_state(void) { g_ui.active_window = MENU_WINDOW_NONE; }

// window.showWindow(id). Note: like the JS, closeWindowState() runs before the
// "same window" test, so re-selecting the active window re-renders it.
void menu_show_window(int id, const int force) {
    id = v10_customize_window_alias(id); // v10 page: showWindow(53) is window 3's Customize tab
    if (id != MENU_WINDOW_NONE) menu_idle_reset(); // showWindow(id >= 0): CONTROLS.idleTimer = 0
    if (id == MENU_WINDOW_NONE) {
        menu_toggle_window(0);
        return;
    }
    if (!ui_window_key(id)) {
        menu_log("window %d is not available in Krunker Box Office", id);
        return;
    }
    if (id == 5 && v10_login_guest_window()) return; // v10 windows[4].gen: a guest gets loginOrRegister's modal
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
            // menuWindow.className = extraCls; innerHTML = gen(); stickyHeader; overflowY /
            // width / height / maxHeight / borderRadius; "dark"; forceFill (the captured
            // class and inline style of that window)
            const char *cls = "dark", *style = "overflow-y: auto;";
            ui_window_meta(id, &cls, &style);
            v10_customize_before_render(); // the class preview canvas leaves the old window first
            html_dom_set_attr(mw, "class", cls);
            html_dom_set_attr(mw, "style", style);
            html_dom_set_html(mw, html ? html : "");
            html_dom_set_class(mw, "stickyHeader", sticky);
            html_dom_set_class(mw, "dark", 1);
            i18n_translate_tree(mw); // the captured English template in the page's language
        }
        HtmlElem holder = by_id("windowHolder");
        if (holder) html_dom_set_class(holder, "popupWin", 1);
        free(html);
        g_ui.active_window = id;
        if (header_el) {
            html_dom_set_html(header_el, header);
            i18n_translate_tree(header_el);
        }
        menu_toggle_window(1);
        ui_window_post(id);
        if (mw) i18n_translate_tree(mw); // what post() filled in
    } else {
        menu_toggle_window(0);
    }
}

// UI.toggleWindow(j)
void menu_toggle_window(const int open) {
    set_style("menuClassContainer", "visibility", "");
    set_display("windowHolder", open ? "block" : "none");
    set_display("baseLinks", open ? "none" : "block");
    menu_update_instructions();
    if (!open) menu_close_window();
    set_style("aHolder", "z-index", open ? "999999" : "99999999999");
    menu_schedule_rect();
}

void menu_close_window(void) {
    close_window_state();
    html_dom_set_class(html_dom_root(), "kn-bare-window", 0); // ui_window_post: the Object Manager's bare window
    set_display("windowHolder", "none");
    v10_customize_before_render(); // the class preview canvas goes home (toggleWindowCharPreview(null))
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
        if (resp) html_dom_set_text(resp, I18N("kn.account.enterName", "Enter a username"));
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
    if (!menu_lock_ready(client)) return; // web: enterGame() runs on pointerlockchange
    client->menu_play_pending = 0;
    client_set_menu_open(client, 0);
    client_enter_game(client);
}

// The old `play {classIndex, offline}` call.
static void menu_request_play(const int offline) {
    Client *client = g_ui.client;

    // `offline:true` (Host Game -> Start Server) with no server: play on the in-process
    // sandbox server for this session (net/offline.c) and stop the transport from retrying.
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
    // A click while the controls are off is CONTROLS.toggle(true); once the end screen shows its
    // results, CONTROLS.disable() has locked them, so it only runs UI.toggleEndScreen(): the
    // results come back over the menu and the pointer stays free for them (no lock request).
    if (hud_ui_end_screen_interactive()) {
        hud_ui_toggle_end_screen(g_ui.client);
        return;
    }
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

// CONNECTING... (the error screen goes) until the server answers or the timer fires.
void menu_show_connecting(void) {
    g_ui.connect_error[0] = 0;
    arm_connect_timer();
    menu_update_instructions();
}

// The error screen's "here" link: ask for an immediate reconnect (natively the net
// thread retries on its own with a backoff anyway). Pinned to a direct server
// (servers_ui.c) it retries that server; otherwise with a brain it seeks a new game.
void menu_retry_connect(void) {
    menu_show_connecting();
    if (client_is_connected(g_ui.client)) return;
    if (servers_ui_retry()) return; // brain matchmaker: seek a new game (servers_ui.c)
    client_net_request_reconnect(g_ui.client);
}

// Main-thread half of the connection events: consume the NetStatus the transport left
// under net_lock (events.c), keep net_connected truthful and update the page.
static void menu_poll_connection(Client *client) {
    char error[sizeof(client->net_status_error)];
    char code[sizeof(client->net_status_code)];
    char close_reason[sizeof(client->net_status_close_reason)];

    pthread_mutex_lock(&client->net_lock);
    const int status = client->net_status;
    client->net_status = NET_STATUS_NONE;
    memcpy(error, client->net_status_error, sizeof(error));
    memcpy(code, client->net_status_code, sizeof(code)); // "(Error 0x<code>)": 000005 idle kick, 000001 connect error / lost session
    const int dropped = client->net_status_dropped, close_code = client->net_status_close_code;
    memcpy(close_reason, client->net_status_close_reason, sizeof(close_reason));
    pthread_mutex_unlock(&client->net_lock);

    if (status == NET_STATUS_CONNECTED) {
        client->net_connected = 1;
    } else if (status == NET_STATUS_FAILED) {
        client->net_connected = 0;
        online_mode_session_ended(); // its rights and live sign-ins went with it

        if (!client->offline) {
            // The session is gone: drop its players (the next INIT starts fresh) so the
            // player can enter again after the transport reconnects, and show the menu
            // (Krunker's disconnect() shows the error screen over it).
            if (client->game.player_count) client_clear_players(client);
            hud_ui_hide_end_screen(client); // its end screen went with it (Escape must not bring the results back)
            client->in_game = 0;
            client->spawn_requested = 0;
            client->menu_play_pending = 0;
            if (html_ui_available() && !html_ui_is_menu_open()) client_set_menu_open(client, 1);
        }
    }

    // KrunkNative: a server shutdown / a refused join key rejoins through the matchmaker with a
    // fresh ticket (servers_ui.c); meanwhile its connect errors keep the server's message up
    int keep_screen = 0;
    if (status == NET_STATUS_FAILED) {
        const int was_rejoining = servers_ui_rejoining();
        servers_ui_connection_lost(error, sizeof(error), code, dropped);
        keep_screen = was_rejoining && !dropped && !strcmp(code, "000001");
        if (!keep_screen) {
            g_connect_close_code = close_code;
            snprintf(g_connect_close_reason, sizeof(g_connect_close_reason), "%s", close_reason);
        }
    }

    if (!html_ui_available() || !g_ui.dom_ready) return;

    if (client_is_connected(client)) {
        if (client->menu_connect_sent != 1) {
            menu_set_connect(1, NULL, NULL);
            client->menu_connect_error[0] = 0;
        }
    } else if (status == NET_STATUS_FAILED && !keep_screen) {
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
        g_ui.last.mode_id != game_mode_id(&client->game) || // same map, other mode (a hosted lobby): #mapInfo
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
    g_ui.last.mode_id = game_mode_id(&client->game);
    g_ui.last.class_index = client->menu_class_index;
    g_ui.last.health = health;
    g_ui.last.valid = 1;
}

// The old `classes` / `maps` events: re-read Game when its tables change.
static void menu_refresh_tables(const Client *client) {
    const ClassConfig *classes = client->game.classes ? client->game.classes : rules_classes();
    const size_t class_count = client->game.class_count ? client->game.class_count : rules_class_count();
    if (classes != g_ui.last_classes || class_count != g_ui.last_class_count || menu_class_filter_key(&client->game) != g_class_filter_key) {
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

static int scenario_on(void) {
#ifdef UI_SCENARIO_SUPPORTED
    return ui_scenario_active();
#else
    return 0;
#endif
}

// The inline state v7.1.4's client leaves on its static page once loaded, connected and
// on the menu (DOM of the real page, tools/uidiff/capture-v7.js), minus the HUD's (hud_ui.c).
static void setup_dom(void) {
    set_display("loadingBg", "none");
    set_display("loadingBg1", "none");
    set_display("consentBlock", "none");
    set_display("aHider", "none");            // ads: none in KrunkNative
    set_display("topRightAdHolder", "none");
    set_display("topLeftAdHolder", "none");
    set_display("menuFPSDisplay", "none");
    set_style("vignette", "opacity", "0");
    set_display("teamSelector", "none");
    set_display("fullMenHider", "block");
    set_display("infoHider", "block");
    set_display("mMenuHolDef", "block");
    set_display("instructionHider", "block");
    set_style("instructionsFadeBG", "opacity", "0");
    set_style("spectButton", "opacity", "1");
    set_display("vsHolder", "none");
    set_style("menuRegionLabel", "display", "flex");
    // f2045 (featured panel): the stream list ("Loading Streams..." while API_CLIENT.listStreams
    // has no answer), not the ad slot
    set_display("streamCon", "block");
    set_display("adCon", "none");
    // the FTUE / gradual-menu chrome stays as fullMenu() leaves it
    set_display("frvrFTUEVideoHolder", "none");
    set_display("expertModeBtn", "none");
    // The class preview canvas keeps the size the three.js renderer gives it (750x600 CSS
    // px); class_preview.c renders the player model into it (html_dom_canvas_gl).
    set_style("classPreviewCanvas", "width", "750px");
    set_style("classPreviewCanvas", "height", "600px");
    set_style("classPreviewCanvas", "opacity", "1");
    set_style("bubbleContainer", "transform", "scale(0.0)");
    // #mainLogoFace: index.html's mainLogoFace.src = "./img/menu/faces/f_" + randInt(0, 15) + ".png"
    // (f_0 in --render-scenario renders, the reference captures pin it)
    {
        HtmlElem face = by_id("mainLogoFace");
        char src[48];
        snprintf(src, sizeof(src), "./img/menu/faces/f_%d.png", scenario_on() ? 0 : rand() % 16);
        if (face) html_dom_set_attr(face, "src", src);
    }
#ifndef __EMSCRIPTEN__
    // KrunkNative: the native build is the desktop client already, so no "Download Client"
    // image (showWindow(25)) in the header; the web build keeps Krunker's. --render-scenario
    // keeps it: uidiff compares the header with v7.1.4's.
    if (!scenario_on()) {
        HtmlElem imgs[4];
        const int n = html_dom_query_all(0, "#headerRightSocial img", imgs, 4);
        for (int i = 0; i < n; i++) {
            char src[64] = "";
            if (html_dom_get_attr(imgs[i], "src", src, sizeof(src)) >= 0 && strstr(src, "client.png")) html_dom_set_display(imgs[i], "none");
        }
    }
#endif
    // KrunkNative addition: v7.1.4 draws the interact progress bar on its overlay canvas
    // (game-overlay interactProg); KrunkNative keeps v3.1.1's #interactProg > #interProgB
    // divs (styled by css/krunknative.css), driven by the interact prompt code.
    if (!by_id("interactProg")) {
        HtmlElem parent = by_id("midGameUIHider");
        HtmlElem holder = parent ? html_dom_create("div", "interactProg") : 0;
        HtmlElem bar = holder ? html_dom_create("div", "interProgB") : 0;
        if (bar) {
            html_dom_set_attr(bar, "class", "interProg");
            html_dom_append(holder, bar);
            html_dom_append(parent, holder);
        }
    }
    // Ranked (doc/v7/decisions.md "Ranked", ranked_ui.c / windows[26] = showWindow(27)): v7.1.4 keeps
    // the window and its "menu.btn.ranked" label but its page has no button left, so KrunkNative adds
    // one to the main menu bar (#subLogoButtons), right after Server Finder: both start a match
    // (Invite / Join / Connect / KPD are lobby links and tools), so the two matchmaking entries sit
    // together. Styled with v7's own `button small buttonO` (#ff6a0b): v7's ranked colour (store.js
    // limT "Ranked" #ff5500) and unlike its neighbours. --render-scenario leaves the bar as v7 has it.
    if (!scenario_on() && !by_id("menuBtnRanked")) {
        HtmlElem finder = by_id("menuBtnBrowser");
        if (finder) html_dom_insert_html(finder, HTML_INSERT_AFTER_END,
            "<div class=\"button small buttonO\" id=\"menuBtnRanked\" onmouseenter=\"playTick()\" "
            "onclick=\"playSelect(),openRankedMenu()\">Ranked</div>");
    }
    // v10's #menuBtnTurfWars (icon flag): the Turf Wars window, after Ranked
    if (!scenario_on() && !by_id("menuBtnTurfWars")) {
        HtmlElem ranked = by_id("menuBtnRanked");
        if (ranked) html_dom_insert_html(ranked, HTML_INSERT_AFTER_END,
            "<div class=\"button small buttonG\" id=\"menuBtnTurfWars\" onmouseenter=\"playTick()\" "
            "onclick=\"playSelect(),openTurfWars()\">Turf Wars</div>");
    }
    // Krunker v10 keeps Host Game in its action cards (its chooser, window 42: v10_menu.c)
    servers_ui_menu_setup(); // KrunkNative: the Connect button next to Invite / Join (servers_ui.c)
    v10_news_setup(); // the v10 menu's Guide / What's New / Community & Events items (v10_news.c)
    v10_competitive_setup(); // the v10 page: Ranked / Turf Wars / Leaderboards (v10_competitive.c)
    v10_market_menu_setup(); // v10 Market & Trading: #menuBtnMarket (v10_market.c)

    flavor_ui_apply(); // Krunker Strike: brand the wordmark + theming hooks (no-op for base)

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
    // windows[0].settingType = settingsViewMode || "basic" (getSavedVal("krk_advanced"))
    g_ui.settings_view = html_storage_get("krk_advanced", saved, sizeof(saved)) >= 0 && atoi(saved) == 1;
}

// The language changed (Settings > Language, or its table arrived on the web): i18n.c already
// translated the page in place; what the menu writes from i18n strings is written again.
static void menu_on_locale(void *user) {
    (void) user;
    if (!g_ui.dom_ready) return;
    g_ui.last_instructions[0] = 0;
    menu_update_instructions();
    menu_update_top_bar();
    menu_update_class_box();
    // the open window again (its gen() strings); Settings stays as it is (no scroll / tab reset:
    // its labels are plain table strings, translated in place)
    if (g_ui.active_window != MENU_WINDOW_NONE && g_ui.active_window != 1 && menu_window_is_open())
        menu_show_window(g_ui.active_window, 1);
    // the Language select follows the language (a change that did not come from it: krk_lang at
    // start on the web, where the table arrives later)
    HtmlElem sel = by_id("knLangSelect");
    if (sel) html_dom_set_value(sel, i18n_locale());
    hud_ui_relayout();
}

// init() + the `bridgeReady` handler: runs once the page's DOM exists.
static void menu_dom_init(void) {
    i18n_init(); // translations.en + the stored locale (krk_lang)
    i18n_on_change(menu_on_locale, NULL);
    ui_windows_load();
    ui_settings_load();
    ui_settings_collect_defaults();
    setup_dom();
    v10_menu_dom_init(); // Krunker v10's Svelte menu markup (ui/): its handlers, nav, header (v10_menu.c)
    menu_build_classes();
    menu_build_maps();
    g_ui.last_instructions[0] = 0;
    menu_scale_base();
    menu_apply_open(html_ui_is_menu_open());
    menu_update_top_bar();
    menu_update_class_box();
    menu_update_instructions();
    g_menu_ping = -1; // a fresh page shows "0" in gray

    // the page's markup (index.html, the Svelte mounts) in the stored language
    html_dom_set_lang(i18n_lang_find(i18n_locale())->bcp47);
    i18n_translate_tree(0);

    // bridgeReady
    g_ui.dom_ready = 1;
    if (!g_ui.connected && !g_ui.connect_error[0]) arm_connect_timer();
    ui_settings_push_saved();
    ui_client_settings_dom_init(); // Exit button, custom CSS, auto spectate, Discord/auto-join timer
    v10_rewards_dom_init();        // v10.0.2 rewards: the sidebar's Battle Pass item, windows[32] (v10_rewards.c)
    menu_update_instructions();
    g_ui.last_rect[0] = 0;
    menu_send_preview_rect();
}

// ---------------------------------------------------------------- client entry points

void client_set_menu_open(Client *client, const int open) {
    html_ui_set_menu_open(open);
    menu_apply_open(open);

#ifdef __EMSCRIPTEN__
    if (open) g_lock.waiting = 0;
#endif
    // The pointer follows (pointer lock owner above): free with the menu; locked when it closes
    // over the game, unless the end-screen results / a free end title / KrunkScript keep it free.
    client_pointer_update(client);
    client->menu_lock_grace_until = open ? 0.0 : glfwGetTime() + MENU_LOCK_GRACE_SECONDS;
    pointer_reset_last_pos(client); // avoid a camera jump on re-lock
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
    g_ui.settings.scale_ui = 0.7f;
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
#ifdef __EMSCRIPTEN__
    // Play / Escape waiting for the pointer lock: leave the menu once it is held; a
    // refused or unanswered request keeps the menu (Krunker's pointerlockchange flow).
    if (g_lock.waiting && html_ui_is_menu_open()) {
        if (client_pointer_locked(client)) {
            g_lock.waiting = 0;
            client_set_menu_open(client, 0);
        } else if (web_pointer_lock_errors() != g_lock.errors || glfwGetTime() - g_lock.since > MENU_LOCK_WAIT_SECONDS) {
            g_lock.waiting = 0;
        }
    } else {
        g_lock.waiting = 0;
    }
#endif
    menu_idle_tick(client);

    if (g_ui.dom_ready) {
        menu_refresh_tables(client);
        if (menu_state_changed(client)) menu_apply_state(client);
        menu_update_ping(client);
        download_ui_tick(); // ?download opens the Download window (download_ui.c)
        v10_news_tick();    // the v10 menu's What's New item (v10_news.c)
        ui_loadout_tick();  // item pickers: only the cards near the scroll position exist (ui_vgrid.h)

        // window.addEventListener('resize', scaleBase) fallback: poll the viewport too.
        float w, h, dpr;
        html_dom_viewport(&w, &h, &dpr);
        if (w > 0.0f && h > 0.0f && (w != g_ui.viewport_w || h != g_ui.viewport_h)) menu_scale_base();
    }

    client_hud_html_tick(client, (float) glfwGetTime()); // HTML HUD (hud_ui.c)
    hud_zombies_tick(client); // power-up timers, finiteAmmo reserve, zombie round (hud_zombies.c)
    game_reload_tick(client); // F5 / `reload`, and the game a reload restart comes back to (game_reload.c)
    ui_console_tick(client); // developer console (ui_console.c)
    update_ui_tick(client);  // Outdated / Update now (update_ui.c)
    ui_interact_tick(client); // the interact prompt and hold progress (ui_interact.c)
    object_manager_ui_tick(client); // the Build Tool cost waiver (OM caps)

    // Round end: the end screen frees the pointer (Krunker exits pointer lock so its
    // results can be clicked) and relocks when the next round starts (client_pointer_update).
    static int end_interactive;
    const int end_now = hud_ui_end_screen_interactive();
    if (end_now != end_interactive) {
        end_interactive = end_now;
        html_dom_set_class(html_dom_root(), "kn-end-open", end_now); // lifts the HUD's pointer-events: none (krunknative.css)
    }
    client_pointer_update(client); // after this frame's HUD pass (the END, the results, the next INIT)

    // Web: a refused request (pointerlockerror) ends the grace at once, so the game is
    // never shown with a dead mouse for the whole grace period.
    int lock_denied = 0;
#ifdef __EMSCRIPTEN__
    const int lock_errors = web_pointer_lock_errors();
    lock_denied = lock_errors != g_lock.denied_seen;
    g_lock.denied_seen = lock_errors;
#endif
    const double grace_left = client->menu_lock_grace_until - glfwGetTime(); // tests park it far ahead (hud_autoplay)
    // The lock is wanted (menu closed over the game: g_ptr.want.lock) but not held: the browser's
    // Escape / a refused request, natively the cursor mode changed behind the owner's back. v7
    // pointerlockchange -> toggleControlUI(false): the menu with CLICK TO PLAY (a click is the user
    // gesture the browser needs to lock again). Not over the end screen (hideUI: UI.active = false;
    // a click on the game view locks again, client_pointer_click); KrunkScript's unlockMouse /
    // freeMouse released the lock on purpose (want.lock is 0 then).
    if (pointer_owner_active() && g_ptr.want.lock && !hud_ui_end_screen_up() && !html_ui_is_menu_open() &&
        (grace_left <= 0.0 || (lock_denied && grace_left <= MENU_LOCK_GRACE_SECONDS)) && !client_pointer_locked(client)) {
        client_set_menu_open(client, 1);
    }
}
