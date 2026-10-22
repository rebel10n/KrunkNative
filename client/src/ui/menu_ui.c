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
#include <quick_play_retry.h> // leaving the menu stops a Quick Play search
#include <player_names.h> // Hide player names (player_names.h)
#include <v10_wild_offer.h>
#include <krunker_net.h> // the official error screen's Find Game
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
#include <strike_ui.h> // Krunker Strike: FRVR's mobile page instead of the v10 one (strike_ui.c)
#include <ui_interact.h> // the interact prompt (client/src/ui/ui_interact.c)
#include <game_config.h> // Game.config.classes / isCustom (the class list)
#include <client_settings.h> // Exit menu item + Settings > Client (client/src/ui/ui_client_settings.c)
#include <lobby_ui.h> // the comp menu (comp_ui.c)
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
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
#include <audio.h> // the auto-respawn countdown's tick_0
#include <nettspend.h>   // the Nettspend's weapon previews
#include <krunker_captcha.h> // krunker_captcha_needs_player: the security challenge frees the pointer

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

// After closing the menu the pointer lock request is asynchronous in browsers; do not
// treat "not locked yet" as "lock lost" for this long.
#define MENU_LOCK_GRACE_SECONDS 1.0
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
    const AccountInfo *account = active_account();
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
    if (ui_mobile_active()) return; // no pointer lock on phones (v10's locklessChange): no fullscreen or orientation lock
    // the developer console keeps the pointer free (pointer_lock.h): a lock requested now would be
    // taken and held over the console, its clicks and typing lost to the locked canvas
    if (ui_console_shown()) return;
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
    if (client_pointer_locked(client) || ui_console_shown()) return 1; // no lock is wanted over the console: nothing to wait for
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
    in.modal = (unsigned char) krunker_captcha_needs_player(); // the security challenge: clicks and typing go to it
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

// The game is in play by the UI state, the window's focus left out (pointer_lock.h `play`): the
// round start enters a player in play (main.c client_tick_game), as the pointer lock does with focus.
int client_pointer_play_wanted(const Client *client) {
    if (!client || !pointer_owner_active()) return 0;
    return g_ptr.want.play;
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
    // KrunkNative's own classes (past CLASS_COUNT: the Nettspend) are never in an official krunker.io game's
    // list, which krunker.io sends once the game loads. They keep the default rule there, so the card stays
    // (locked by gen_tab_class through game_class_allowed) rather than vanishing when that list arrives.
    const int kn_only_official = game->is_official && index >= CLASS_COUNT;
    if (game->config.class_list_count > 0 && !kn_only_official) {
        for (int i = 0; i < game->config.class_list_count; i++) {
            if (game->config.class_list[i] == index) return 1;
        }
        return 0;
    }
    return !classes[index].hide || game_config_is_custom(game);
}

// What menu_class_listed reads, to rebuild the list when it changes (GAME_CONFIG, Game.is_official on join/leave).
static unsigned menu_class_filter_key(const Game *game) {
    unsigned key = 2166136261u ^ (unsigned) game_config_is_custom(game) ^ ((unsigned) game->is_official << 1);
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
            else if (nettspend_weapon_kind(w)) // KrunkNative Nettspend: the page's own previews (ui/img/kn, ui_loadout.c's)
                snprintf(c->preview, sizeof(c->preview), "img/kn/preview_%s.png", nettspend_weapon_kind(w) == NETTSPEND_WEAPON_DRANK ? "drank" : "money_spread");
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
    // The fit scale is printed to 3 decimals, as window.scaleBase() writes it, but that rounding
    // can sit a fraction below the exact fit (0.627 for 0.62745). The scaled box is a transformed
    // layer, and the engine clips everything inside it to its box (box-shadow rings included), so
    // the box must cover the window at the printed scale: whole CSS px, rounded up. Only what the
    // box anchors to its far edges moves, by a pixel (the 3D scene showed through the window's
    // right and bottom strip otherwise).
    const double fit = sy < sx ? sy : sx;
    const double scale = round(fit * 1000.0) / 1000.0;
    fmt_number(num, sizeof(num), ceil(w / scale));
    snprintf(value, sizeof(value), "%spx", num);
    html_dom_set_style(base, "width", value);
    fmt_number(num, sizeof(num), ceil(h / scale));
    snprintf(value, sizeof(value), "%spx", num);
    html_dom_set_style(base, "height", value);
    snprintf(value, sizeof(value), "scale(%.3f)", scale);
    html_dom_set_style(base, "transform", value);
    menu_send_preview_rect();
}

// ---------------------------------------------------------------- menu

// UI.toggleMenu(j) + UI.toggleMenuHider(j), inline styles included.
static void menu_update_info_holder(void);
static void menu_respawn_stop(void);

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
        menu_respawn_stop(); // enterGame(): the auto-respawn countdown ends
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
    if (!open) quick_play_retry_cancel(); // leaving the menu stops a Quick Play search (quick_play_retry.h)
#ifdef __EMSCRIPTEN__
    // Leaving the menu waits for the pointer lock (menu_lock_ready, client_menu_tick); over an open
    // console there is no lock to wait for (the pointer stays free for it).
    if (!open && html_ui_is_menu_open() && !client_pointer_locked(g_ui.client) && !ui_console_shown()) {
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
    if (on) menu_log("error screen: %s", g_ui.connect_error);
    // v10: both go above the menu layer while the box shows (main.css .onError)
    HtmlElem holder = by_id("instructionHolder"), update_bg = by_id("instructionsUpdateBG");
    if (holder) html_dom_set_class(holder, "onError", on);
    if (update_bg) html_dom_set_class(update_bg, "onError", on);
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

// ---- the boot curtain, #loadGamNm and the Game Updating art (Krunker v10.0.2)
// The page boots with #instructionsFadeBG (main.css: #222, opacity 1, `transition: opacity 0.7s
// ease-in-out`) over the menu and #instructions holding index.html's own loading ring (scale
// 1.4) and #loadGamNm. v10 leaves both until the game's INIT, which sets the curtain's opacity
// to 0 (g:219640, the 0.7 s fade) and hides #loadingBg (g:219397); INIT's updatePlayInstructions()
// replaces the ring and #loadGamNm with CLICK TO PLAY. Nothing raises the curtain again: every
// later join is a page load in v10 (switchGame / backToOrigin navigate), so KrunkNative, which
// switches games in place, raises it again (at once, like a fresh page) when it leaves for
// another game: menu_show_connecting, menu_host_offline. The web build reloads anyway.
static int g_boot_curtain = 1; // #instructionsFadeBG is up, waiting for an INIT
static int g_boot_settled;     // ... and has been drawn once at opacity 1 (the fade's start value)
// The game it waits past: an in-place switch (menu_show_connecting, menu_host_offline) lifts the
// curtain only on a later INIT, not on the game still loaded. Every INIT's game_init swaps
// Game.mode for a fresh allocation (made while the old one is still alive, and nothing else frees
// it), so "the mode is not the one we armed on" is "an INIT ran since"; NULL = any ready game (the
// page's boot, scenarios).
static int g_boot_any = 1;
static const void *g_boot_mode;
// #loadGamNm under the boot ring: v10 writes "Loading <play || host> ..." for ?play= / ?host=
// (g:197836), then autoHostMap(host, true) "No Games found" until the map preset is fetched and
// "Hosting <map> ..." (g:218512-14). KrunkNative's matching boots: a startup map to seek
// (web ?map=, a map argument online) is ?play=, an offline / Host Game start on the in-process
// server is ?host=. v10 shows "No Games found" while it fetches the map preset; KrunkNative's
// preset is local (no wait), so it goes straight to "Hosting"; only a scenario renders "No Games
// found" (state.boot.loadGamNm).
enum { LOAD_GAM_NONE, LOAD_GAM_LOADING, LOAD_GAM_HOSTING, LOAD_GAM_NO_GAMES };
static int g_load_gam = -1;    // -1: from the client's start (menu_load_gam_auto)
static char g_load_gam_map[96];

static int scenario_on(void);

static const char *cjson_map_name(const cJSON *map) {
    const cJSON *name = map ? cJSON_GetObjectItem(map, "name") : NULL;
    return cJSON_IsString(name) ? cJSON_GetStringValue(name) : "";
}

static void menu_load_gam_set(const int kind, const char *map) {
    g_load_gam = kind;
    snprintf(g_load_gam_map, sizeof(g_load_gam_map), "%s", map ? map : "");
}

// The page's own boot: what the client was started with.
static void menu_load_gam_auto(void) {
    const Client *client = g_ui.client;
    const char *map = client ? cjson_map_name(client->startup_map) : "";
    if (!client || !map[0]) menu_load_gam_set(LOAD_GAM_NONE, "");
    else menu_load_gam_set(client->offline || client->game.is_local ? LOAD_GAM_HOSTING : LOAD_GAM_LOADING, map);
}

static void menu_load_gam_text(StrBuf *sb) {
    if (g_load_gam < 0) menu_load_gam_auto();
    char text[160];
    switch (g_load_gam) {
    case LOAD_GAM_LOADING:
    case LOAD_GAM_HOSTING:
        snprintf(text, sizeof(text), "%s %s ...", g_load_gam == LOAD_GAM_LOADING ? I18N("generic.loading.simple", "Loading") : I18N("generic.hosting", "Hosting"), g_load_gam_map);
        sb_append_esc(sb, text);
        break;
    case LOAD_GAM_NO_GAMES:
        sb_append_esc(sb, I18N("generic.hosting.noGames", "No Games found"));
        break;
    default:
        break;
    }
}

// Raise the curtain at once (a fresh page): an inline 1 ms transition until the fade. Not
// `none` or 0 s: the engine keeps the before-change value a transition starts from only while
// the element declares a transition of some length (anim.c has_transition_decl; Chrome keeps
// it always), and the fade must start from this opacity 1.
static void menu_boot_arm(const int any) {
    g_boot_curtain = 1;
    g_boot_settled = 0;
    g_boot_any = any;
    g_boot_mode = !any && g_ui.client ? (const void *) g_ui.client->game.mode : NULL;
    set_style("instructionsFadeBG", "transition", "opacity 0.001s");
    set_style("instructionsFadeBG", "opacity", "1");
    set_display("loadingBg", "");
    g_ui.last_instructions[0] = 0;
}

// The curtain is up and its INIT has not come: the page is still loading its game.
static int menu_boot_waiting(void) {
    if (!g_boot_curtain) return 0;
    if (!g_ui.ready) return 1;
    return !g_boot_any && g_ui.client && (const void *) g_ui.client->game.mode == g_boot_mode;
}

// Per frame: the first INIT after the curtain went up (and was drawn once) starts the fade.
static void menu_boot_tick(void) {
    if (!g_boot_curtain) return;
    if (!g_boot_settled) {
        g_boot_settled = 1;
        return;
    }
    if (menu_boot_waiting()) return;
    g_boot_curtain = 0;
    set_style("instructionsFadeBG", "transition", ""); // main.css: opacity 0.7s ease-in-out
    set_style("instructionsFadeBG", "opacity", "0");
    set_display("loadingBg", "none");
    menu_update_instructions();
}

// ui_scenario.c state.boot: the page before its first INIT (true, or {loadGamNm: "play" | "host" |
// "noGames" | ""}, the map from state.mapName / the scenario's map). With ready:true (+ after_ms)
// the fade starts with the state, so after_ms picks a frame of it.
void menu_boot_scenario(const cJSON *boot) {
    if (!boot || cJSON_IsFalse(boot)) return;
    const cJSON *kind = cJSON_IsObject(boot) ? cJSON_GetObjectItem(boot, "loadGamNm") : NULL;
    const cJSON *map = cJSON_IsObject(boot) ? cJSON_GetObjectItem(boot, "map") : NULL;
    const char *name = cJSON_IsString(map) ? map->valuestring : g_ui.map_name;
    if (cJSON_IsString(kind)) {
        const char *k = kind->valuestring;
        menu_load_gam_set(!strcmp(k, "play") ? LOAD_GAM_LOADING : !strcmp(k, "host") ? LOAD_GAM_HOSTING : !strcmp(k, "noGames") ? LOAD_GAM_NO_GAMES : LOAD_GAM_NONE, name);
    }
    menu_boot_arm(1);
}

// The Game Updating screen's art (g:222104-222112): a random entry of v10's menu-bg list
// (g:19437) as #instructionsUpdateBG's background, "Art By <creator>" in
// #instructionsUpdateCredit opening the artist's page. ui/img/menu-bg/ (live_assets_v10.lock).
static const struct {
    int id;
    const char *creator, *link;
} k_menu_bg_art[] = {
    {0, "@EmpoTheDon", "https://twitter.com/EmpoTheDon"},
    {1, "@EmpoTheDon", "https://twitter.com/EmpoTheDon"},
    {2, "@14julink", "https://twitter.com/14julink"},
    {3, "@14julink", "https://twitter.com/14julink"},
    {4, "@14julink", "https://twitter.com/14julink"},
    {8, "@Flair741", "https://twitter.com/Flair741"},
    {9, "@polluxvisuals", "https://twitter.com/polluxvisuals"},
};
#define MENU_BG_ART_COUNT ((int) (sizeof(k_menu_bg_art) / sizeof(k_menu_bg_art[0])))
static int g_update_art = -1; // the shown entry while Game Updating is up
static int g_update_art_pin;  // the entry a render shows (state.updateArt: menu_update_art_scenario)

static void menu_apply_update_art(const int updating) {
    if (updating == (g_update_art >= 0)) return;
    HtmlElem credit = by_id("instructionsUpdateCredit");
    if (updating) {
        g_update_art = scenario_on() ? g_update_art_pin : rand() % MENU_BG_ART_COUNT; // randInt(0, length - 1); pinned in renders
        char buf[160];
        snprintf(buf, sizeof(buf), "url(\"img/menu-bg/%d.png\")", k_menu_bg_art[g_update_art].id);
        set_style("instructionsUpdateBG", "background-image", buf);
        set_display("instructionsUpdateBG", "block");
        if (credit) {
            snprintf(buf, sizeof(buf), "Art By %s", k_menu_bg_art[g_update_art].creator);
            html_dom_set_html(credit, buf);
            snprintf(buf, sizeof(buf), "openURL('%s')", k_menu_bg_art[g_update_art].link);
            html_dom_set_attr(credit, "onclick", buf);
            html_dom_set_display(credit, "block");
        }
    } else {
        g_update_art = -1;
        set_display("instructionsUpdateBG", "none");
        if (credit) html_dom_set_display(credit, "none");
    }
}

// ui_scenario.c state.updateArt: the menu-bg id a render's Game Updating screen shows (default the
// first entry), so a scenario can show the art a v10 capture's seeded Math.random picked
void menu_update_art_scenario(const cJSON *id) {
    if (!cJSON_IsNumber(id)) return;
    for (int i = 0; i < MENU_BG_ART_COUNT; i++)
        if (k_menu_bg_art[i].id == id->valueint) g_update_art_pin = i;
}

// ---- the auto-respawn countdown (v10 g:197900-197906, g:209976)
// A death in a forceSpawn mode runs autoRespawn(forceSpawn + 1) when the death delay ends:
// tick_0 at 0.2 and the play prompt's subtitle "Auto Respawning in {0}" (respawn.auto), again
// every second down to 1, then cleared; enterGame() stops it. The subtitle setter keeps it null
// while the player is still in the game (the death delay has not shown the menu yet), the pointer
// is locked or the end screen's #endMidHolder shows, and the chain stops at a null subtitle, so
// as in v10 the countdown only runs when the menu was already up at that moment (ESC during the
// death screen); otherwise it is one tick_0.
static char g_respawn_sub[96]; // the subtitle (v10's iìíííìi), "" none
static int g_respawn_timer;

static void menu_respawn_subtitle(const char *text) {
    const char *now = g_respawn_sub[0] ? g_respawn_sub : NULL;
    if (!g_ui.connected) return;
    if ((!text && !now) || (text && now && !strcmp(text, now))) return;
    HtmlElem end_mid = by_id("endMidHolder");
    const int hidden = !g_ui.menu_open || (g_ui.client && client_pointer_locked(g_ui.client)) || inline_style_is(end_mid, "display", "inline-block");
    snprintf(g_respawn_sub, sizeof(g_respawn_sub), "%s", text && !hidden ? text : "");
    menu_update_instructions();
}

static void menu_respawn_step(int seconds);

static void menu_respawn_timer_fire(void *user) {
    g_respawn_timer = 0;
    const int seconds = (int) (intptr_t) user;
    if (g_respawn_sub[0]) menu_respawn_step(seconds);
    else menu_respawn_subtitle(NULL);
}

static void menu_respawn_step(const int seconds) {
    audio_play("tick_0", 0.2f, 0, 1.0f);
    char text[96], n[16];
    snprintf(n, sizeof(n), "%d", seconds - 1);
    menu_respawn_subtitle(I18N_F(text, "respawn.auto", "Auto Respawning in {0}", n));
    if (seconds > 1 && g_respawn_sub[0]) g_respawn_timer = html_timer_add(1000.0, menu_respawn_timer_fire, (void *) (intptr_t) (seconds - 1));
    else menu_respawn_subtitle(NULL);
}

static void menu_respawn_stop(void) {
    if (!g_respawn_timer) return;
    html_timer_cancel(g_respawn_timer);
    g_respawn_timer = 0;
    menu_respawn_subtitle(NULL);
}

// game_events.c, as the death delay ends (before the menu opens): Game.forceSpawn is
// config.forceSpawn || mode.forceSpawn (g:189942).
void menu_auto_respawn_begin(const Client *client) {
    if (!client || !client->game.mode) return;
    const int force = client->game.config.auto_respawn > 0 ? client->game.config.auto_respawn : (int) client->game.mode->config.force_spawn;
    if (force <= 0 || !g_ui.dom_ready) return;
    if (g_respawn_timer) html_timer_cancel(g_respawn_timer);
    g_respawn_timer = 0;
    menu_respawn_step(force + 1);
}

// ui_scenario.c state.autoRespawn: the subtitle as the countdown shows it with N seconds left
// (no sound, no timer).
void menu_auto_respawn_scenario(const cJSON *seconds) {
    if (!cJSON_IsNumber(seconds)) return;
    char text[96], n[16];
    snprintf(n, sizeof(n), "%d", seconds->valueint);
    snprintf(g_respawn_sub, sizeof(g_respawn_sub), "%s", I18N_F(text, "respawn.auto", "Auto Respawning in {0}", n));
    g_ui.last_instructions[0] = 0;
    menu_update_instructions();
}

#define MENU_LOAD_SPINNER "<div style='width:100%;text-align:center;transform:scale(1.3);'><div class='lds-ring'><div></div><div></div><div></div><div></div></div></div>"
// index.html's #instructions as the page boots (the ring at scale 1.4 and #loadGamNm)
#define MENU_BOOT_RING "<div style=\"width:100%;text-align:center;transform:scale(1.4)\"><div class=\"lds-ring\"><div></div><div></div><div></div><div></div></div></div><div style=\"color:#fff;width:50vw\" id=\"loadGamNm\">"

#define MENU_ERROR_TAB(handler, label) "\n\t\t\t\t<div class=\"instructionsTab\" onmouseenter=\"playTick();\" onclick=\"playSelect(0.1);" handler "\">" label "</div>"
// The Gamepad.active the text was last written with (-1: never). KrunkNative: the text follows the
// flag (client_menu_tick), where v10 only rewrites it at menu toggles; a "PRESS START" written while a
// pad was active (a plugged-in pad counts as active at start-up) stayed up after the mouse took over.
static int g_instructions_pad = -1;

void menu_update_instructions(void) {
    HtmlElem el = by_id("instructions");
    if (!el) return;
    g_instructions_pad = gamepad_active();
    StrBuf sb = {0};
    const int error = g_ui.dom_ready && !can_play() && g_ui.connect_error[0] && !g_ui.connected;
    const int updating = error && !strcmp(g_ui.connect_error, "GAME UPDATING");
    if (!g_ui.dom_ready || (menu_boot_waiting() && !error)) {
        sb_append(&sb, MENU_BOOT_RING);
        menu_load_gam_text(&sb);
        sb_append(&sb, "</div>");
    } else if (can_play()) {
        const int spect = spectate_requested();
        sb_append(&sb, gamepad_active() ? (spect ? I18N("app.spectate.controller", "START to SPECTATE") : I18N("app.play.controller", "PRESS START"))
                       : ui_mobile_active() ? (spect ? I18N("app.spectate.mobile", "TAP TO SPECTATE") : I18N("app.play.mobile", "TAP TO PLAY"))
                       : spect ? I18N("app.spectate", "CLICK TO SPECTATE") : I18N("app.play", "CLICK TO PLAY"));
        // v10's subtitle is the auto-respawn countdown; KrunkNative's ranked queue status (v7's,
        // ranked_ui.c: NULL on the v10 page, whose banner shows it) takes the same place
        const char *sub = g_respawn_sub[0] ? g_respawn_sub : ranked_ui_status();
        if (sub) {
            sb_append(&sb, "<div id='instructionsSubtitle'>");
            for (const char *p = sub; *p; p++) sb_appendf(&sb, "%c", *p >= 'a' && *p <= 'z' ? *p - 32 : *p);
            sb_append(&sb, "</div>");
            if (!g_respawn_sub[0] && ranked_ui_ready() && ranked_ui_ready()[0]) {
                sb_append(&sb, "<div id='instructionsSubtitle'>");
                sb_append_esc(&sb, ranked_ui_ready());
                sb_append(&sb, "</div>");
            }
        }
    } else if (!error) {
        sb_append(&sb, MENU_LOAD_SPINNER);
    }
    char *html = sb_finish(&sb);
    // the error screen: #instructionsUpdate (v10 g:222098-222120)
    StrBuf eb = {0};
    const char *find = servers_ui_pinned() ? I18N("kn.error.reconnect", "Reconnect") : NULL;
    if (updating) {
        // Game Updating ("GAME UPDATING": a server stopping for an update, or the matchmaker's
        // NoServersMatchQuery): v10's header, the Twitch line and Find a Game / Settings /
        // Changelog over the menu-bg art. Its two Twitch iframes stay display:none in v10 and are
        // left out. KrunkNative: while servers_ui.c rejoins with a fresh ticket its status takes
        // the Twitch line's place, and Play Offline joins the tabs (as on Lost Connection).
        sb_append(&eb, "<div class='instructionsHeader'>Game Updating</div>");
        if (servers_ui_rejoining())
            sb_appendf(&eb, "<div style='margin-top:10px;font-size:15px;color:rgba(255,255,255,0.8)'>%s</div>", servers_ui_rejoin_text());
        else
            sb_append(&eb, "<div style='margin-top:10px;font-size:15px;color:rgba(255,255,255,0.4)'>While you wait <a target='_blank' href='https://www.twitch.tv/directory/game/Krunker'>go chat</a> with some Krunker Streamers</a></div>");
        sb_appendf(&eb, "<div class=\"instructionsTabs\">" MENU_ERROR_TAB("knRetry()", "%s") MENU_ERROR_TAB("knHostOffline()", "%s")
                        MENU_ERROR_TAB("showWindow(1)", "Settings") MENU_ERROR_TAB("showWindow(35)", "Changelog") "\n\t\t\t</div>",
                   find ? find : "Find a Game", I18N("kn.error.playOffline", "Play Offline"));
    } else if (error) {
        // Lost Connection. v10: an inactivity kick drops the ID / region lines and every tab but
        // Find Game (and leaves its message div open, so the tabs sit inside it), a ban names the
        // appeal address. KrunkNative: the server's text is escaped; the transport's close code
        // and cause stand for the socket's; Play Offline is its own tab (knHostOffline); "Log in"
        // under the message when the matchmaker turned a guest away (servers_ui.c).
        const char *text = connect_error_text(g_ui.connect_error);
        char lower[256];
        size_t i = 0;
        for (; text[i] && i + 1 < sizeof(lower); i++) lower[i] = (char) (text[i] >= 'A' && text[i] <= 'Z' ? text[i] + 32 : text[i]);
        lower[i] = 0;
        const int inactive = !strcmp(g_ui.connect_error, "Kicked for inactivity") || strstr(lower, "disconnected due to inactivity") != NULL;
        sb_appendf(&eb, "<div class='instructionsHeader'>Lost Connection</div><div style='color: rgba(255, 255, 255, 0.6);margin-top: 5px;font-size: 28px;'>"
                        "<div style=\"font-size: 28px;color: rgba(255, 255, 255, 0.6);%s\"> ",
                   inactive ? "margin-top: 50px;margin-bottom: 55px;" : "");
        sb_append_esc(&eb, text);
        sb_append(&eb, " </div>");
        if (servers_ui_account_required()) {
            char login[192];
            I18N_F(login, "kn.error.loginToJoin", "{0}Log in{1} to join it.", "<a href='#' onclick='showWindow(5);return false'>", "</a>");
            sb_appendf(&eb, "<div style='font-size:20px'>%s</div>", login);
        }
        if (!inactive) {
            sb_append(&eb, "</div><div style='margin-top:10px;font-size:15px;color:rgba(255,255,255,0.4)'>ID: 0x");
            sb_append_esc(&eb, g_connect_code[0] ? g_connect_code : "000000");
            sb_appendf(&eb, " | Code: %d | Reason: ", g_connect_close_code);
            sb_append_esc(&eb, g_connect_close_reason[0] ? g_connect_close_reason : "0");
            sb_append(&eb, "</div><div style='margin-top:10px;font-size:15px;color:rgba(255,255,255,0.4)'>Region: ");
            sb_append_esc(&eb, g_region_label[0] ? g_region_label : MENU_REGION_NAME);
            sb_append(&eb, "</div>");
            if (servers_ui_rejoining())
                sb_appendf(&eb, "<div style='margin-top:10px;font-size:15px;color:rgba(255,255,255,0.8)'>%s</div>", servers_ui_rejoin_text());
            else if (strstr(lower, "connection banned"))
                sb_append(&eb, "<div style='margin-top:10px;font-size:13px;color:rgba(255,255,255,0.4)'>Contact <span style='color:#fff'>krunker+banappeal@frvr.com</span> to Appeal the Ban</div><div></div>");
            else
                sb_appendf(&eb, "<div style='margin-top:10px;font-size:15px;color:rgba(255,255,255,0.4)'>%s</div>", I18N("error.extentions", "If you have any extensions, disable them"));
        }
        // v10's first tab is Find Game on every Lost Connection screen (g:222118: playMap / backToOrigin,
        // a new game); KrunkNative's knRetry is that fresh seek (menu_retry_connect), and with a pinned
        // server it tries that server again, so it says Reconnect. An inactivity kick keeps it alone.
        // An official krunker.io failure (a lobby gone, the matchmaker refusing) has it too: a new official
        // game, not the same one again (menu_retry_connect -> servers_ui_official_find_game)
        sb_appendf(&eb, "<div class=\"instructionsTabs\">" MENU_ERROR_TAB("knRetry()", "%s"), find ? find : "Find Game");
        if (!inactive)
            sb_appendf(&eb, MENU_ERROR_TAB("knHostOffline()", "%s") MENU_ERROR_TAB("showWindow(2)", "Servers") MENU_ERROR_TAB("showWindow(1)", "Settings"),
                       I18N("kn.error.playOffline", "Play Offline"));
        sb_append(&eb, "\n\t\t\t</div>");
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
    menu_apply_update_art(updating);
    set_style("instructionHolder", "pointer-events", "all");
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

// The map's welMsg (shared map->welcome: NULL when absent or not a valid welcome) is the menu's
// line in #mapWelcome, above the Contact / Terms row of #infoHolder, and it is hidden on a map
// without one. v10 sends the welMsg to chat on a map init (window.welcome, _69625); Box Office
// shows it in the menu instead and never in chat.
static void menu_set_map_welcome(const Client *client) {
    const char *msg = client->game.map && client->game.map->welcome ? client->game.map->welcome : "";
    HtmlElem el = by_id("mapWelcome");
    if (el) html_dom_set_text(el, msg);
    set_display("mapWelcome", msg[0] ? "" : "none");
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
    menu_set_map_welcome(client);
    set_display("infoHolder", "");
}

// v10 getClanCol (game.deob.js iïïìíìi): these clans' tags are gold, the others take the default.
static const char *menu_clan_colour(const char *clan) {
    static const char *const gold[] = {"arae", "lain", "scte", "fame", "kpd", "jump", "art", "dev", "mmok", "nl"};
    for (size_t i = 0; i < sizeof(gold) / sizeof(gold[0]); i++) {
        if (!strcasecmp(clan, gold[i])) return "#FBC02D";
    }
    return "rgba(255,255,255,0.5)";
}

#define MENU_POLICE_CLASS 5 // dI.class: Detective (ui_loadout.c POLICE_CLASS)

// The class box (v10 updateClassPreview, game.deob.js iììîîii): #menuClassName = the class (classN
// for the K.P.D popup, else the class's name); #menuClassSubtext = each loadout weapon's name
// (weaponConfig.name, never the skin's) in " - " order, coloured by the skin it wears: the store
// rarity colour, rainbowText for an animated rarity (Unobtainable), a white text stroke for
// Contraband, "inherit" without a skin; #menuClassIcn = the last weapon's skin thumbnail, or its
// preview without one; #menuClassNameTag = the player tag or "Guest". The K.P.D popup shows
// Detective's loadout (classConfig[5]) in the player's skins.
void menu_update_class_box(void) {
    const int police = ui_police_active();
    const MenuClass *c = menu_class_at(police ? MENU_POLICE_CLASS : g_ui.client->menu_class_index);
    HtmlElem name_el = by_id("menuClassName"), sub_el = by_id("menuClassSubtext"), tag_el = by_id("menuClassNameTag");
    if (name_el) html_dom_set_text(name_el, police ? ui_police_class_name() : c ? c->name : ""); // vO349.classN (K.P.D)
    char icon[256] = "";
    StrBuf sb = {0};
    for (int i = 0; c && i < c->loadout_count; i++) {
        if (sb.len) sb_append(&sb, " - ");
        const int skin_index = ui_loadout_weapon_skin_shown(c->loadout_ids[i]);
        const ViewerSkin *skin = viewer_skin(skin_index);
        const int rarity = skin && skin->rarity >= 0 && skin->rarity < g_viewer_rarity_count ? skin->rarity : -1;
        const int animate = rarity == 6 && !client_render_settings()->disable_rarity_anim; // rarities[r].animate (ui_loadout.c rarity_anim)
        sb_appendf(&sb, "<span %sstyle='color:%s%s'>", animate ? "class='rainbowText' " : "",
                   rarity >= 0 && g_viewer_rarities[rarity].color ? g_viewer_rarities[rarity].color : "inherit",
                   rarity == 5 ? ";-webkit-text-stroke:1px #fff" : ""); // iiÏiíìì: Contraband
        sb_append_esc(&sb, c->loadout_names[i]);
        sb_append(&sb, "</span>");
        if (i == c->loadout_count - 1) { // the last weapon pictures the box
            if (skin) store_skin_preview(skin_index, icon, sizeof(icon)); // v10874 = skin.thumbnail
            else if (c->preview[0]) html_dom_asset_url(c->preview, icon, sizeof(icon));
        }
    }
    char *html = sb_finish(&sb);
    if (sub_el) html_dom_set_html(sub_el, html);
    free(html);
    HtmlElem icn = by_id("menuClassIcn");
    if (icn && icon[0]) html_dom_set_attr(icn, "src", icon);
    set_style("menuClassContainer", "z-index", "99999");
    if (tag_el && police) {
        html_dom_set_html(tag_el, "Officer Buddy <i class='material-icons menuClassPlayerPremium'>beenhere</i>"); // vO349: no lvl / clan
    } else if (tag_el && active_account()) {
        // account: level box, name, [clan], creator (verified, creatorBadge), premium (beenhere),
        // featured (check_circle) and the custom badge, each behind its privacy setting (the creator
        // and featured marks behind verifiedBadge, as v10:210861). No partner mark: v10's box never
        // had one.
        const AccountInfo *acc = active_account();
        const int premium = acc->premium && !ui_setting_is_false("premiumBadge");
        StrBuf sb = {0};
        sb_appendf(&sb, "<div class='menuClassLevelBox' style='color:white;'>%d</div><span class='menuClassPlayerName'>", acc->level);
        sb_append_esc(&sb, player_names_account(premium && acc->alias[0] ? acc->alias : acc->name)); // Hide player names: "Player"
        sb_append(&sb, "</span>");
        if (player_names_clan(acc->clan)[0]) {
            sb_appendf(&sb, "<span class='menuClassPlayerClan' style='color:%s'>[", menu_clan_colour(acc->clan));
            sb_append_esc(&sb, acc->clan);
            sb_append(&sb, "]</span>");
        }
        if (acc->creator && !ui_setting_is_false("verifiedBadge")) sb_append(&sb, "<i class='material-icons menuClassPlayerCreator'>verified</i>");
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
        // v10: a guest is "Guest" (Guest_<sid> only while a game's player object exists), not the name typed in the menu
        sb_append(&sb, "<span class='menuClassPlayerName'>Guest</span>");
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
        // UI.toggleMenu(true), as the client's menu state: a window opened over the end screen's
        // results (a hotkey, the header) left html_ui_is_menu_open() at 0, so Click to Play after
        // closing it "opened" the menu again instead of bringing the results back (toggleEndScreen)
        // and CLICK TO PLAY stayed up for the rest of the round.
        if (!g_ui.menu_open) {
            if (g_ui.client) client_set_menu_open(g_ui.client, 1);
            else menu_apply_open(1);
        }
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

// ---- the connecting spinner: a server switch, a reconnect, a retry within the transport's budget
// A server change looks like a game load, not like a lost connection (v10 loads every game on a
// fresh page: the boot curtain and the ring, with "Loading <map> ..." as loadGamNm). The spinner
// stays up until the connection is in (its INIT lifts the curtain) or the transport gives up (the
// Lost Connection screen with Find Game / Reconnect). g_connect_map is the target's map ("" = the ring alone, the
// map is not known yet).
static int g_connecting;          // the spinner is up for a connect under way
static char g_connect_map[96];    // its target's map, for "Loading <map> ..."

int menu_connecting(void) {
    return g_connecting;
}

// A Quick Play search this client was running is stopped (quick_play_retry.h): its spinner goes, with
// no error: the menu is as the player left it
void menu_connect_stopped(void) {
    g_connecting = 0;
    menu_update_instructions();
}

// The game is still loading: a connect under way (the spinner), the v10 boot curtain waiting for its INIT,
// or Krunker Strike's loading screen. Escape does nothing meanwhile (main.c key_callback).
int menu_loading(void) {
    if (strike_ui_active()) return strike_loading_up() || g_connecting;
    return g_connecting || menu_boot_waiting();
}

// Starts the spinner for a connect to a server whose game (map) is `map` ("" or NULL: not known).
// Idempotent: a later call (the lobby's map once the join is answered) only updates the text.
void menu_show_connecting_map(const char *map) {
    snprintf(g_connect_map, sizeof(g_connect_map), "%s", map ? map : "");
    if (!g_connecting) {
        g_connecting = 1;
        g_ui.connected = 0;
        g_ui.ready = 0;
        g_ui.connect_error[0] = 0;
        if (g_ui.client) g_ui.client->menu_connect_sent = 0;
        if (g_ui.dom_ready && !g_boot_curtain) menu_boot_arm(0);
    }
    if (g_connect_map[0]) menu_load_gam_set(LOAD_GAM_LOADING, g_connect_map);
    else menu_load_gam_set(LOAD_GAM_NONE, "");
    menu_log("connecting (%s)", g_connect_map[0] ? g_connect_map : "the lobby's map not known yet");
    menu_update_top_bar();
    menu_update_instructions();
}

// The old `connect {connected, error}` event, applied directly.
static void menu_set_connect(const int connected, const char *error, const char *code) {
    Client *client = g_ui.client;
    g_connecting = 0;
    g_ui.connected = !!connected;
    if (connected) g_ui.connect_error[0] = 0;
    else snprintf(g_ui.connect_error, sizeof(g_ui.connect_error), "%s", error && error[0] ? error : CONNECT_ERROR_DEFAULT);
    snprintf(g_connect_code, sizeof(g_connect_code), "%s", !connected && code && code[0] ? code : "000000");
    if (strike_ui_active()) { // Krunker Strike's popups (strike_connect.c): the Game Updating / Lost Connection text
        const StrikeConnState st = connected ? STRIKE_CONN_CONNECTED
                                 : !strcmp(g_ui.connect_error, "GAME UPDATING") ? STRIKE_CONN_UPDATING : STRIKE_CONN_LOST;
        strike_connection_state(st, connect_error_text(g_ui.connect_error), g_connect_code, g_connect_close_code, g_connect_close_reason);
    }
    if (connected) menu_idle_reset(); // Krunker's retry link reloads the page: a fresh idle timer
    if (connected) krunker_net_clear_failure(); // a later error screen is this session's, not the official game's before it
    if (!connected) g_ui.ready = 0; // the next INIT (reconnect) makes the client ready again
    client->menu_connect_sent = connected ? 1 : 0;
    menu_update_top_bar();
    menu_update_instructions();
}

// The connection error screen with a message of our own (servers_ui_resume_rejoin: the new
// process after a restart for a server update shows "GAME UPDATING" while it rejoins).
void menu_show_connect_error(const char *error, const char *code) {
    menu_set_connect(0, error, code);
    if (g_ui.client) {
        snprintf(g_ui.client->menu_connect_error, sizeof(g_ui.client->menu_connect_error), "%s", error ? error : "");
        g_ui.client->menu_connect_error_time = glfwGetTime();
    }
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

// The transport is up (a game server or the sandbox): the Strike PLAY decides between entering and reconnecting
int menu_connected(void) {
    return g_ui.client && client_is_connected(g_ui.client);
}

// Connected and the map is in: menu_play enters now
int menu_can_play(void) {
    return can_play();
}

// A PLAY waiting for its connection or map is dropped (the connection was lost for good: the Lost Connection popup is
// the next step, so a later join does not enter a game it never asked for)
void menu_play_cancel(void) {
    if (g_ui.client) g_ui.client->menu_play_pending = 0;
}

// Krunker Strike's PLAY while the map is not in yet (or the connection is down): the game is entered once it can
// be (menu_try_play, each frame). Not connected: the reconnect runs (menu_retry_connect) and the play waits for it;
// a reconnect that fails is the Lost Connection popup (strike_connect.c), never a silent stop.
void menu_play_when_ready(void) {
    Client *client = g_ui.client;
    if (!client) return;
    client->menu_play_pending = 1;
    if (!client_is_connected(client)) {
        menu_retry_connect();
        return;
    }
    menu_try_play(client);
}

// Host Game -> "Start Server": play on the embedded local server (no network server
// needed). C starts it when nothing is connected and spawns once the map is ready;
// the page shows CONNECTING... in between.
void menu_host_offline(void) {
    menu_close_window();
    // v10 hosts on a fresh page: the curtain and "Hosting <map> ..." until the game's INIT
    // (only when the in-process server starts: on a live connection play just starts)
    if (!client_is_connected(g_ui.client)) {
        const char *map = cjson_map_name(g_ui.client->offline_start_map);
        menu_load_gam_set(LOAD_GAM_HOSTING, map[0] ? map : cjson_map_name(g_ui.client->startup_map));
        if (!g_load_gam_map[0]) g_load_gam = LOAD_GAM_NONE;
        menu_boot_arm(0);
    }
    g_connecting = 0; // a local game is not a connect: "Hosting", not the spinner
    g_ui.connect_error[0] = 0;
    menu_update_instructions();
    request_pointer_lock_if_web();
    menu_request_play(1);
}

// The loading spinner for a reconnect / retry to the page's own game: v10's "Loading <map> ..." with
// the curtain until its INIT. A server switch names the target's map (menu_show_connecting_map).
void menu_show_connecting(void) {
    menu_show_connecting_map(g_ui.map_name);
}

// The error screen's first tab (Find Game, or Reconnect for a pinned server): a new try at the same
// server (pinned: the transport) or a fresh seek for a brain session (servers_ui.c).
void menu_retry_connect(void) {
    if (client_is_connected(g_ui.client)) {
        g_boot_any = 1; // nothing is left: the game still loaded lifts the curtain again
        return;
    }
    // the official error screen's Find Game: a new official game (v10). Server Type krunker.io takes it for any failure:
    // a krunker.io list with no room is no krunker session, and the brain seek below would join a Box Office game
    if (krunker_net_active() || krunker_net_failure_shown(g_ui.connect_error) || servers_ui_official_type()) {
        if (!servers_ui_official_find_game()) client_net_request_reconnect(g_ui.client);
        return;
    }
    menu_show_connecting();
    if (servers_ui_retry()) return; // brain matchmaker: seek a new game (servers_ui.c)
    client_net_request_reconnect(g_ui.client);
}

// Main-thread half of the connection events: consume the NetStatus the transport left
// under net_lock (events.c), keep net_connected truthful and update the page.
// The spinner (menu_show_connecting_map) with the target's text kept as it is.
static void connecting_spinner(void) {
    char map[sizeof(g_connect_map)];
    snprintf(map, sizeof(map), "%s", g_connect_map);
    menu_show_connecting_map(map);
}

// A player in the match was lost (FAILED) and its rejoin (servers_ui.c) is under way: the rejoined
// session is entered at once, as Play enters one (menu_play), instead of waiting at the menu.
static int g_play_on_rejoin;

// Main-thread half of the connection events: consume the NetStatus the transport left
// under net_lock (events.c), keep net_connected truthful and update the page.
// What the page shows for each event (v10 loads every server change like a game load):
//   CONNECTING                              the spinner (a switch: the old session closed on purpose)
//   FAILED, the transport retries itself    the spinner (a connect attempt failed, within the budget)
//   FAILED, a rejoin is under way           the spinner ("Rejoining a game..." is not shown)
//   FAILED, a server update (004004)        the Game Updating screen with its rejoin line (v10's notice)
//   FAILED, held (a server ERROR, a
//     disconnect, a give-up)                the Lost Connection screen with Find Game (the give-up's text)
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
    const int gave_up = client->net_status_gave_up;
    memcpy(close_reason, client->net_status_close_reason, sizeof(close_reason));
    pthread_mutex_unlock(&client->net_lock);

    // a session that had been joined (its INIT made the menu ready, or its players exist), as
    // opposed to a connect the server refused (a drain answers that with 004004 too)
    const int had_session = status == NET_STATUS_FAILED && !client->offline &&
                            (g_ui.ready || client->me != NULL || client->game.player_count > 0);
    // a player in the match (not spectating) when its session is lost: its rejoin enters the game again
    const int in_play = client->in_game && client->me && !spectate_active();
    if (status == NET_STATUS_CONNECTED) {
        client->net_connected = 1;
        if (g_play_on_rejoin) {
            g_play_on_rejoin = 0;
            // the rejoin's session (not one a player picked since, which rejoin_cancel ended): entered as Play enters one
            if (servers_ui_rejoin_active()) {
                client->menu_play_pending = 1; // menu_try_play enters once the map is ready
                client_set_menu_open(client, 0);
            }
        }
    } else if (status == NET_STATUS_CONNECTING) {
        client->net_connected = 0; // a switch closed the session on purpose: the next one follows (spinner below)
        if (!client->offline) client_leave_game(client); // that game's chat, feed, players and end screen go now
    } else if (status == NET_STATUS_FAILED) {
        client->net_connected = 0;
        online_mode_session_ended(); // its rights and live sign-ins went with it

        if (!client->offline) {
            // The session is gone: its game is left (client_leave_game: players, chat, kill feed,
            // end screen, ...; the next INIT starts fresh) so the player can enter again after the
            // transport reconnects, and show the menu (Krunker's disconnect() shows the error
            // screen over it). Escape must not bring that game's results back.
            client_leave_game(client);
            if (client->game.player_count) client_clear_players(client);
            hud_ui_hide_end_screen(client);
            client->in_game = 0;
            client->spawn_requested = 0;
            client->menu_play_pending = 0;
            if (html_ui_available() && !html_ui_is_menu_open()) client_set_menu_open(client, 1);
        }
    }

    // KrunkNative: a server shutdown / a refused join key rejoins through the matchmaker with a fresh
    // ticket (servers_ui.c); a lost session (no server ERROR) is the Lost Connection screen at once, as
    // v10 (connection_dropped); a give-up is final and ends every rejoin.
    int rejoining = 0;
    if (status == NET_STATUS_FAILED) {
        if (gave_up) servers_ui_gave_up();
        else rejoining = servers_ui_connection_lost(error, sizeof(error), code, dropped, had_session);
        // kept while the rejoin runs (each failed attempt ends the session the first one lost)
        if (!rejoining) g_play_on_rejoin = 0;
        else if (in_play) g_play_on_rejoin = 1;
    }

    if (!html_ui_available() || !g_ui.dom_ready) return;

    if (client_is_connected(client)) {
        if (client->menu_connect_sent != 1) {
            menu_set_connect(1, NULL, NULL);
            client->menu_connect_error[0] = 0;
        }
        return;
    }
    if (status == NET_STATUS_CONNECTING) {
        connecting_spinner();
        return;
    }
    if (status != NET_STATUS_FAILED) return;

    // the Game Updating screen: a server update (004004) while we rejoin, or it stays up for the rejoin
    // (its failed connects do not replace the server's message)
    const int updating_box = !strcmp(g_ui.connect_error, "GAME UPDATING");
    if (rejoining && (updating_box || !strcmp(code, NET_ERROR_CODE_UPDATING))) {
        if (!updating_box) {
            g_connect_close_code = close_code;
            snprintf(g_connect_close_reason, sizeof(g_connect_close_reason), "%s", close_reason);
            menu_set_connect(0, "GAME UPDATING", code);
        }
        return;
    }

    // a transport that retries by itself (within its budget) or a rejoin under way: the spinner
    const int transport_retries = !gave_up && !client->net_hold_retry && !client->net_should_quit;
    if (rejoining || transport_retries) {
        // a dropped session reconnects behind Krunker Strike's "Reconnecting..." loading screen (strike_connect.c)
        if (had_session && strike_ui_active()) strike_connection_state(STRIKE_CONN_RECONNECTING, NULL, NULL, 0, NULL);
        connecting_spinner();
        return;
    }

    // held: a server's ERROR (kick, ban, refused join key with no rejoin), a user disconnect, or the give-up
    g_connect_close_code = close_code;
    snprintf(g_connect_close_reason, sizeof(g_connect_close_reason), "%s", close_reason);
    const double now = glfwGetTime();
    const int changed = client->menu_connect_sent != 0 || strcmp(error, client->menu_connect_error) != 0;
    if (changed || now - client->menu_connect_error_time >= MENU_CONNECT_ERROR_REPEAT_SECONDS) {
        menu_set_connect(0, error, code);
        memcpy(client->menu_connect_error, error, sizeof(client->menu_connect_error));
        client->menu_connect_error_time = now;
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
    // #loadingBg and #instructionsFadeBG stay as the page boots them until the first INIT
    // (menu_boot_tick); v10 never touches #loadingBg1 (empty, like #loadingBg)
    set_display("loadingBg1", "none");
    set_display("consentBlock", "none");
    // ads: none in KrunkNative, as v10 with !window.canShowAds or premium (v10.js:219405): so no
    // #aContainer, the dark rgba(0,0,0,.6) box v10's browser capture shows under Now Playing.
    // The phone menu keeps #aHider (v10's phone capture never sets its inline display, so the
    // strip shows at the bottom centre: doc/ref/krunker-v10/mobile/menu-mobile.html.gz).
    if (!ui_mobile_active()) set_display("aHider", "none");
    set_display("topRightAdHolder", "none");
    set_display("topLeftAdHolder", "none");
    set_display("menuFPSDisplay", "none");
    set_style("vignette", "opacity", "0");
    set_display("teamSelector", "none");
    set_display("fullMenHider", "block");
    set_display("infoHider", "block");
    set_display("mMenuHolDef", "block");
    set_display("instructionHider", "block");
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
    // Ranked and Turf Wars are the v10 page's own entries: #menuBtnRanked is an action card in
    // #subLogoButtons (the main bar, next to Quick Match), #menuBtnTurfWars a side-nav item
    // (v10_competitive.c wires both). Krunker v10 keeps Host Game in its action cards (its
    // chooser, window 42: v10_menu.c)
    servers_ui_menu_setup(); // KrunkNative: the Connect button next to Invite / Join (servers_ui.c)
    v10_news_setup(); // the v10 menu's Guide / What's New / Community & Events items (v10_news.c)
    v10_competitive_setup(); // the v10 page: Ranked / Turf Wars / Leaderboards (v10_competitive.c)
    v10_market_menu_setup(); // v10 Market & Trading: #menuBtnMarket (v10_market.c)

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
    // a fresh page: the curtain is up (main.css) until an INIT, no update art yet
    g_boot_curtain = 1;
    g_boot_settled = 0;
    g_boot_any = 1;
    g_update_art = -1;
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
    if (strike_ui_active()) strike_ui_menu_open(client, open); // the Strike screens follow (TAP_TO_PLAY / game)

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

    if (!g_ui.dom_ready && strike_ui_active()) {
        // Krunker Strike: FRVR's page has none of v10's markup; its own setup (strike_ui.c) and the
        // menu state below (connection, play, settings) drive it. The v10 DOM writes find nothing.
        if (strike_ui_dom_present()) {
            i18n_init();
            ui_settings_load();
            menu_build_classes();
            menu_build_maps();
            g_ui.dom_ready = 1;
            ui_settings_push_saved();
            strike_ui_dom_init(client);
        }
    } else if (!g_ui.dom_ready && html_dom_by_id("uiBase")) menu_dom_init();

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

    if (g_ui.dom_ready && strike_ui_active()) {
        menu_refresh_tables(client);
        if (menu_state_changed(client)) menu_apply_state(client);
        strike_ui_tick(client); // the Strike page (strike_ui.c): screens, in-game UI, touch controls
    } else if (g_ui.dom_ready) {
        menu_refresh_tables(client);
        if (menu_state_changed(client)) menu_apply_state(client);
        menu_boot_tick();
        if (gamepad_active() != g_instructions_pad) menu_update_instructions(); // PRESS START <-> CLICK TO PLAY
        menu_update_ping(client);
        download_ui_tick(); // ?download opens the Download window (download_ui.c)
        v10_news_tick();    // the v10 menu's What's New item (v10_news.c)
        v10_wild_offer_tick(client); // the account's Wild Offer: fetched after sign-in, notified once (v10_wild_offer.c)
        ui_loadout_tick();  // item pickers: only the cards near the scroll position exist (ui_vgrid.h)
        v10_market_tick();  // Create Trade's inventory lists: the same (v10_market.c)

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
    if (pointer_owner_active() && g_ptr.want.lock && !hud_ui_end_screen_up() && !html_ui_is_menu_open() && !menu_loading() &&
        (grace_left <= 0.0 || (lock_denied && grace_left <= MENU_LOCK_GRACE_SECONDS)) && !client_pointer_locked(client)) {
        client_set_menu_open(client, 1);
    }
}
