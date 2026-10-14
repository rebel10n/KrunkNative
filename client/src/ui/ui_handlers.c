// The inline-handler table: every `on*` callee in ui/index.html and the captured window
// markup of Krunker v7.1.4 (ui/windows), with the bodies v7's src/index.js gives them, plus
// the KrunkNative-only names C writes (knRetry, knHostOffline, knDialog, knClient*). Names
// owned by one module (settings, controls, loadout, HUD, account, store, servers, ranked,
// clans) are answered by that module's *_handle() through h_fallback. Analytics / ad /
// payment hooks (logBtnFRVR, XsollaPopup, ...) are no-ops. Also installs the non-inline
// listeners (setupDom's addEventListener calls).
#include <v10_menu.h>
#include <ui_logic.h>
#include <lobby_ui.h> // WP-E voteOnMatch
#include <ui_host_config.h> // WP-E createPrivateRoom: the host config request
#include <cJSON.h>
#include <map_catalog.h>  // Host Game / Games window maps
#include <sound_events.h> // ui_sound_play_tick / ui_sound_play
#include <spectate.h>     // spectate_set_requested (toggleSpect)
#include <console.h>      // #knConsoleInput keys (developer console, ui_console.c)
#include <ui_mobile.h>    // isMobile: Escape resumes without a pointer lock (ui_mobile.c)
#include <backend_ui.h>   // brain backend: account (account_ui.c) + matchmaker (servers_ui.c)
#include <v10_ui.h> // v10 Leaderboards / view stack (v10_competitive.c)
#include <client_settings.h> // Settings > Client tab + the Exit menu item (ui_client_settings.c)
#include <math.h>
#include <stdio.h>
#include <update_ui.h>
#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

// ---------------------------------------------------------------- arg helpers

static int arg_int(const HtmlArg *args, const int argc, const int i, const int fallback) {
    if (i >= argc) return fallback;
    const HtmlArg *a = &args[i];
    if (a->kind == HTML_ARG_NUM || a->kind == HTML_ARG_BOOL) return (int) a->num;
    if (a->kind == HTML_ARG_STR && a->str) {
        char *end;
        const long v = strtol(a->str, &end, 10);
        return end != a->str ? (int) v : fallback;
    }
    return fallback;
}

static const char *arg_str(const HtmlEvent *ev, const HtmlArg *args, const int argc, const int i, const char *fallback) {
    if (i >= argc) return fallback;
    const HtmlArg *a = &args[i];
    if (a->kind == HTML_ARG_STR) return a->str ? a->str : "";
    if (a->kind == HTML_ARG_THIS_VALUE) return ev && ev->value ? ev->value : "";
    return fallback;
}

static int arg_bool(const HtmlEvent *ev, const HtmlArg *args, const int argc, const int i, const int fallback) {
    if (i >= argc) return fallback;
    const HtmlArg *a = &args[i];
    switch (a->kind) {
        case HTML_ARG_BOOL:
        case HTML_ARG_NUM: return a->num != 0.0;
        case HTML_ARG_THIS_CHECKED: return ev ? ev->checked : fallback;
        case HTML_ARG_STR: return a->str && a->str[0] && strcmp(a->str, "false") != 0 && strcmp(a->str, "0") != 0;
        default: return fallback;
    }
}

// `this.value` of the event's control. The native engine's event snapshot reads a
// <select>'s value as "" (events.c elem_value: dom_get_value never returns NULL), so an
// empty snapshot falls back to the live control value (html_dom_get_value -> widgets).
static const char *this_value(const HtmlEvent *ev, char *buf, const size_t n) {
    if (ev && ev->value && ev->value[0]) return ev->value;
    if (ev && ev->target && html_dom_get_value(ev->target, buf, n) >= 0) return buf;
    return "";
}

// arg_str() with `this.value` resolved through this_value().
static const char *arg_value(const HtmlEvent *ev, const HtmlArg *args, const int argc, const int i, char *buf, const size_t n) {
    if (i < argc && args[i].kind == HTML_ARG_THIS_VALUE) return this_value(ev, buf, n);
    return arg_str(ev, args, argc, i, "");
}

// Resolve `this.value` / `this.checked` args to literal values for the settings store.
// `buf` backs a resolved `this.value`.
static HtmlArg resolve_value_arg(const HtmlEvent *ev, const HtmlArg *args, const int argc, const int i, char *buf, const size_t n) {
    HtmlArg out = {HTML_ARG_STR, 0.0, ""};
    if (i >= argc) return out;
    out = args[i];
    if (out.kind == HTML_ARG_THIS_VALUE) {
        out.kind = HTML_ARG_STR;
        out.str = this_value(ev, buf, n);
    } else if (out.kind == HTML_ARG_THIS_CHECKED) {
        out.kind = HTML_ARG_BOOL;
        out.num = ev && ev->checked ? 1.0 : 0.0;
    } else if (out.kind == HTML_ARG_THIS || out.kind == HTML_ARG_EVENT) {
        out.kind = HTML_ARG_STR;
        out.str = "";
    }
    return out;
}

#define H(name) static int name(void *user, const HtmlEvent *ev, const HtmlArg *args, int argc)
#define UNUSED (void) user; (void) ev; (void) args; (void) argc

// ---------------------------------------------------------------- handlers

H(h_noop) { UNUSED; return 1; }

// window.setSetting(key, value): what Krunker's settings inputs call.
H(h_set_setting) {
    (void) user;
    const char *key = arg_str(ev, args, argc, 0, NULL);
    if (!key) return 1;
    char buf[256];
    const HtmlArg value = resolve_value_arg(ev, args, argc, 1, buf, sizeof(buf));
    char text[128];
    if (value.kind == HTML_ARG_STR) snprintf(text, sizeof(text), "%s", value.str);
    else if (value.kind == HTML_ARG_BOOL) snprintf(text, sizeof(text), "%s", value.num != 0.0 ? "true" : "false");
    else snprintf(text, sizeof(text), "%.15g", value.num);
    printf("[ui] handler setSetting %s=%s\n", key, text);
    ui_settings_set(key, &value);
    return 1;
}


H(h_show_window) { UNUSED; menu_show_window(arg_int(args, argc, 0, 0), arg_bool(ev, args, argc, 1, 0)); return 1; } // showWindow(id, force)
H(h_close_window) { UNUSED; menu_close_window(); return 1; }
H(h_select_class) { UNUSED; menu_select_class(arg_int(args, argc, 0, 0)); return 1; }
// WP-E voteOnMatch("mode,map"): the end screen's match vote cards (client/src/net/lobby_events.c)
H(h_vote_on_match) { (void) user; return lobby_vote_handler(menu_ui()->client, ev, args, argc); }
H(h_toggle_team) { (void) user; return comp_ui_toggle_team(menu_ui()->client, ev, args, argc); } // comp menu: updSyncDat("t", team)

// openURL(url) / openTab(url) / window.open(url, "_blank"): a new browser tab in Krunker;
// the system browser natively, window.open on the web (ui_dialog.c, http(s) only).
H(h_open_url) { UNUSED; ui_open_external(arg_str(ev, args, argc, 0, "")); return 1; }

// Hosting is the dedicated server's job (doc/backend.md 8.7): the client no longer creates
// servers or custom lobbies. Host Game (hidden outside --render-scenario) points there;
// practice play on the embedded offline game stays (playMap / knHostOffline).
H(h_open_host_window) {
    UNUSED;
    ui_alert("To host a server, run the dedicated server: server_bin (see \"Hosting a server\" in the README). "
             "Players join it from the server list or by direct connect.");
    return 1;
}
H(h_open_join_window) { UNUSED; menu_show_window(MENU_WINDOW_JOIN, 0); return 1; }

static void invite_reset(void *user) {
    (void) user;
    menu_ui()->invite_timer = 0;
    HtmlElem b = html_dom_by_id("inviteButton");
    if (b) html_dom_set_text(b, "Invite");
}

H(h_copy_invite_link) {
    UNUSED;
    HtmlElem b = html_dom_by_id("inviteButton");
    if (!b) return 1;
    char link[512];
    servers_ui_invite_link(link, sizeof(link)); // location.protocol + "//" + location.host + "/?game=" + gameId
    html_clipboard_write(link);
    html_dom_set_text(b, "Copied");
    // copyInviteLink(1) (the competitive menu's link button): io_addChat(-1, null, ..., 2)
    if (arg_bool(ev, args, argc, 0, 0)) hud_ui_add_chat(NULL, "Link Copied. Send it to someone to invite them", "#fc03ec");
    MenuUi *ui = menu_ui();
    if (ui->invite_timer) html_timer_cancel(ui->invite_timer);
    ui->invite_timer = html_timer_add(1250, invite_reset, NULL); // copyInviteLink: setTimeout 1250
    return 1;
}

// onclick="errorNotification.style.display = 'none'" (bare expression the handler
// parser names kn.hideErrorNotification)
H(h_hide_error_notification) {
    UNUSED;
    HtmlElem el = html_dom_by_id("errorNotification");
    if (el) html_dom_set_display(el, "none");
    return 1;
}

H(h_bday_play) {
    UNUSED;
    HtmlElem el = html_dom_by_id("bdayBlock");
    if (el) html_dom_set_display(el, "none");
    return 1;
}

H(h_check_terms) {
    UNUSED;
    if (arg_bool(ev, args, argc, 0, 0)) {
        HtmlElem el = html_dom_by_id("consentBlock");
        if (el) html_dom_set_display(el, "none");
        html_storage_set("consent", "1");
    }
    return 1;
}

H(h_play) { UNUSED; menu_play(); return 1; }                       // quickJoinRegion
// joinGame(): f9(gameURL.value || "") - a game link or lobby id (servers_ui.c)
H(h_join_game) {
    UNUSED;
    char text[512] = "";
    HtmlElem input = html_dom_by_id("gameURL");
    if (input) html_dom_get_value(input, text, sizeof(text));
    servers_ui_join_link(text);
    return 1;
}
// Host a map of the catalogue (shared/src/map_catalog.c) on the embedded local server:
// it joins the client's map list (map_catalog_slot: Shipyard, Soul Sanctum and the
// community maps are not in the default rotation) and the offline game starts or reloads
// on it with `mode` (Krunker mode id, -1 = keep). Connected to a server, Krunker's room
// request has no native equivalent: the client plays there (menu_host_offline).
static void host_catalog_map(const int catalog_index, const int mode) {
    Client *client = menu_ui()->client;
    const int slot = catalog_index >= 0 ? map_catalog_slot(&client->game.maps, &client->game.map_count, (size_t) catalog_index) : -1;
    if (catalog_index >= 0 && slot < 0) menu_log("host: map %s could not be loaded", map_catalog_at((size_t) catalog_index)->key);
    if (slot >= 0) menu_select_map(slot);
    if (mode >= 0) client->net_mode_index = mode; // local_game_start reads it (Krunker mode id)
    // Krunker creates a new room: a running offline game reloads on the picked map and
    // mode; otherwise the offline game starts directly on it (offline_start_map).
    if (client->offline && slot >= 0) local_game_host(client, slot, client->net_mode_index);
    else if (slot >= 0) client->offline_start_map = client->game.maps[slot];
    menu_host_offline();
    client->offline_start_map = NULL; // not consumed when a server connection took the request
}

// A Games-window row (name, creatorname) -> catalogue index: rows by "Krunker" are the
// official maps (and the 311_ maps, "Burg (3.1.1)"), the others community maps; a bare
// name (playMap) prefers the official map, like the server's cmd_map. -1 = unknown.
static int catalog_index_for_row(const char *name, const char *creator) {
    const int want_custom = creator && creator[0] && strcmp(creator, "Krunker") != 0;
    for (int pass = 0; pass < 2; pass++) {
        for (size_t i = 0; i < map_catalog_count(); i++) {
            const MapCatalogEntry *e = map_catalog_at(i);
            if (pass == 0 && (e->source == MAP_SOURCE_CUSTOM) != want_custom) continue;
            if (!strcmp(e->name, name)) return (int) i;
        }
    }
    return map_catalog_find(name);
}

// createPrivateRoom(): Krunker collects the checked #gameMap<i> boxes and creates the room;
// here the first checked map becomes the selection and the embedded server is started.
H(h_create_private_room) {
    UNUSED;
    // createPrivateRoom's checks, in order, into #hostGameMsg: a map (or raw game data),
    // then at least one mode
    HtmlElem msg = html_dom_by_id("hostGameMsg");
    if (ranked_ui_in_ranked()) { // v7: if (config.isComp) custom.host.ranked
        if (msg) html_dom_set_html(msg, "<span class='error'>Can't host match while in ranked.</span>");
        return 1;
    }
    const int catalog_index = ui_host_checked_map_index();
    char raw[64] = "";
    HtmlElem raw_el = html_dom_by_id("rawMapData");
    if (raw_el) html_dom_get_value(raw_el, raw, sizeof(raw));
    if (catalog_index < 0 && !raw[0]) {
        if (msg) html_dom_set_html(msg, "<span class='error'>Select at least one Game or Map</span>");
        return 1;
    }
    // WP-E: the Game Settings / Advanced / Server rows (ui_host_config.c): at least one mode and
    // one class, the password length; the request carries the config the game plays with
    char err[128] = "";
    cJSON *request = ui_host_config_collect(servers_ui_host_type(), err, sizeof(err));
    if (!request) {
        if (msg) {
            StrBuf sb = {0};
            sb_append(&sb, "<span class='error'>");
            sb_append_esc(&sb, err[0] ? err : "Invalid game settings");
            sb_append(&sb, "</span>");
            char *html = sb_finish(&sb);
            html_dom_set_html(msg, html);
            free(html);
        }
        return 1;
    }
    const int mode = ui_host_checked_mode(); // the offline game plays one mode; -1 (restricted types): keep
#ifdef LOCAL_GAME_HAVE_HOST_CONFIG
    local_game_host_config(menu_ui()->client, request, servers_ui_host_type()); // the next round's game config
#endif
    cJSON_Delete(request);
    // No brain /mm/host-game from the client any more (hosting = server_bin): the window
    // (only reachable from --render-scenario / the console now) starts offline practice.
    host_catalog_map(catalog_index, mode);
    return 1;
}

// registerAcc / loginAcc: the brain account (account_ui.c), else the guest nickname
H(h_commit_account) {
    UNUSED;
    if (account_ui_handle(html_ui_current_handler(), ev, args, argc) >= 0) return 1;
    menu_commit_account_name();
    return 1;
}
// Servers window handlers (servers_ui.c); without a brain: Krunker's no-ops / play
H(h_servers) {
    UNUSED;
    if (servers_ui_handle(html_ui_current_handler(), ev, args, argc) >= 0) return 1;
    if (!strcmp(html_ui_current_handler(), "quickJoinRegion")) menu_play();
    return 1;
}
// resetSettings(): confirm first; the confirmed path also resets the binds (ui_settings.c).
H(h_reset_settings) { UNUSED; ui_settings_reset_confirm(); return 1; }
H(h_dialog) { UNUSED; ui_dialog_answer(arg_int(args, argc, 0, 0)); return 1; } // knDialog: the #confPop buttons
H(h_export_settings) { UNUSED; ui_settings_export(); return 1; }

// playMap(name): Krunker opens /?play=<name> and autoHostMap creates a room on that map;
// natively the local server hosts the catalogue map (Free for All unless Host Game picked
// another mode).
H(h_play_map) {
    UNUSED;
    const char *name = arg_str(ev, args, argc, 0, "");
    const int index = catalog_index_for_row(name, NULL);
    // brain: a lobby on that map, else /mm/host-game; a published map: its JSON (servers_ui.c)
    if (servers_ui_play_map(name, index >= 0)) return 1;
    if (index < 0) {
        menu_log("playMap: %s is not in the map catalogue", name);
        return 1;
    }
    host_catalog_map(index, -1);
    return 1;
}

// selectHostMap(name, modURL, id, creator): Krunker remembers the community game for the
// Host Game window (selectedCustomMapId); KrunkNative selects the catalogue map.
H(h_select_host_map) {
    UNUSED;
    const int index = catalog_index_for_row(arg_str(ev, args, argc, 0, ""), arg_str(ev, args, argc, 3, ""));
    Client *client = menu_ui()->client;
    const int slot = index >= 0 ? map_catalog_slot(&client->game.maps, &client->game.map_count, (size_t) index) : -1;
    if (slot >= 0) menu_select_map(slot);
    return 1;
}

H(h_kn_host_map) {
    UNUSED;
    if (arg_bool(ev, args, argc, 1, 0)) menu_select_map(arg_int(args, argc, 0, -1));
    return 1;
}

H(h_kn_retry) { UNUSED; menu_retry_connect(); return 0; }
H(h_kn_host_offline) { UNUSED; menu_host_offline(); return 0; }

// GUI.endScreen.changeTab('leaderboard'|'performance'|'clanwar'|'medals'): end-screen tabs (hud_ui.c)
H(h_end_screen_tab) {
    UNUSED;
    hud_ui_end_screen_tab(arg_str(ev, args, argc, 0, ""));
    return 1;
}

// windows[0]: the settings window
H(h_settings_change_tab) {
    UNUSED;
    menu_ui()->settings_tab = arg_int(args, argc, 0, 0);
    menu_show_window(1, 1);
    return 1;
}

H(h_settings_search_list) {
    UNUSED;
    char q[128] = "";
    HtmlElem search = html_dom_by_id("settSearch");
    if (argc > 0 && args[0].kind == HTML_ARG_STR && args[0].str) { // a query argument (uidiff scenarios): typed into the box
        snprintf(q, sizeof(q), "%s", args[0].str);
        if (search) html_dom_set_value(search, q);
    } else if (search) {
        html_dom_get_value(search, q, sizeof(q));
    }
    ui_settings_search(q);
    return 1;
}

H(h_settings_collapse_folder) {
    UNUSED;
    HtmlElem hd = 0;
    if (argc > 0 && args[0].kind == HTML_ARG_THIS) hd = (HtmlElem) args[0].num; // element args carry the id
    if (!hd && ev) hd = ev->target;
    ui_settings_collapse_folder(hd);
    return 1;
}

H(h_switch_tab) { UNUSED; menu_set_active_tab(arg_int(args, argc, 0, 0)); return 1; } // windows[3]/[9].switchTab

// ---- loadout / pickers / K.P.D / changelog (client/src/ui/ui_loadout.c)
static HtmlElem arg_elem(const HtmlEvent *ev, const HtmlArg *args, const int argc, const int i) {
    if (i < argc && args[i].kind == HTML_ARG_THIS) return (HtmlElem) args[i].num; // element args carry the id
    return ev ? ev->target : 0;
}
H(h_select_item) { UNUSED; ui_loadout_select(html_ui_current_handler(), arg_int(args, argc, 0, -1)); return 1; }
H(h_select_skin_color) { UNUSED; ui_loadout_select_skin_color(arg_int(args, argc, 0, -1)); return 1; }
H(h_select_stat) {
    UNUSED;
    const char *v = arg_str(ev, args, argc, 0, "-1");
    ui_loadout_select_stat((int) strtol(v, NULL, 10));
    return 1;
}
H(h_skin_selector) { UNUSED; ui_loadout_skin_selector(arg_int(args, argc, 0, 0), arg_bool(ev, args, argc, 1, 0)); return 1; }
H(h_select_skin) { UNUSED; ui_loadout_select_skin(arg_int(args, argc, 0, -1), arg_int(args, argc, 1, 0)); return 1; }
H(h_loadout_action) { UNUSED; ui_loadout_action(arg_int(args, argc, 0, 0)); return 1; }
H(h_toggle_log) { UNUSED; v10_news_toggle_log(arg_elem(ev, args, argc, 0)); return 1; } // v10_news.c
// toggleSpect(this.checked): `co`, read by enterGame (client/src/spectate.c)
H(h_toggle_spect) {
    UNUSED;
    spectate_set_requested(arg_bool(ev, args, argc, 0, 0));
    menu_update_instructions(); // KrunkNative: "CLICK TO SPECTATE" follows the switch at once (menu_ui.c)
    return 1;
}
H(h_police_pop) { UNUSED; ui_loadout_show_police_pop(); return 1; }
H(h_hide_police_pop) { UNUSED; ui_loadout_hide_police_pop(); return 1; }
H(h_clear_pops) { UNUSED; ui_loadout_clear_pops(); return 1; }
H(h_games_tab) { UNUSED; ui_games_switch_tab(arg_int(args, argc, 0, 0)); return 1; } // windows[9].switchTab
// windows[13].premium()/purchase(): the Store's Premium / Purchase KR tab (store_ui.c)
H(h_store_tab) {
    UNUSED;
    store_ui_handle(html_ui_current_handler(), ev, args, argc);
    return 1;
}
// claimReward(): the v3.1.1 client only has alert("Ad Failed. Contact your Ad Provider").
H(h_claim_reward) {
    UNUSED;
    if (account_ui_handle("claimReward", ev, args, argc) >= 0) return 1; // signed in with a brain: ["clm"]
    ui_alert("Ad Failed. Contact your Ad Provider");
    return 1;
}
// updateWindow(id, force): re-render the open window when it is `id` (or any with force).
H(h_update_window) {
    UNUSED;
    const MenuUi *ui = menu_ui();
    const int force = arg_bool(ev, args, argc, 1, 0);
    if (argc > 0 && args[0].kind == HTML_ARG_NUM) menu_rerender_if_open(arg_int(args, argc, 0, 0));
    else if (force) menu_rerender_if_open(ui->active_window);
    return 1;
}

// ---- #chatInput's focusChat (the Change Controls handlers are ui_controls_handle's,
// the settings rows' delayExecute / aspectSelect / import ui_settings_handle's).
H(h_focus_chat) { UNUSED; ui_controls_chat_focus(arg_bool(ev, args, argc, 0, 0)); return 1; }
// clientExitPopup / closeClient (v7.1.4 #clientExit) and the Client tab's knClientSet / knClientAction
H(h_client) { (void) user; ui_client_settings_handle(html_ui_current_handler(), ev, args, argc); return 1; }

// closWind(): UI.toggleWindow(false) (the window's close button / backdrop)
H(h_close_wind) { UNUSED; menu_toggle_window(0); return 1; }
// showGames(hostType): windows[9].hostType = hostType || null; showWindow(10)
H(h_show_games) { UNUSED; menu_show_window(10, 0); return 1; }
// openDiscord(): window.open(config.discordInvite)
H(h_open_discord) { UNUSED; ui_open_external("https://discord.gg/krunker"); return 1; }
// openGuide(on): the "New to Krunker?" guide popup; v10's openGuide(section) is its tutorial
// popup (the v10 page has no #guidePopupH; v10_news.c)
H(h_open_guide) {
    UNUSED;
    HtmlElem el = html_dom_by_id("guidePopupH");
    if (!el) return v10_news_handle("openGuide", ev, args, argc);
    html_dom_set_display(el, arg_bool(ev, args, argc, 0, 0) ? "block" : "none");
    return 1;
}
// setSpect(this.checked): #specBtn (v7 menu Spectate switch) = spectateEnabled
H(h_set_spect) {
    UNUSED;
    spectate_set_requested(arg_bool(ev, args, argc, 0, 0));
    menu_update_instructions();
    return 1;
}
// purchaseCancel(): closePurchaseSelector()
H(h_purchase_cancel) {
    UNUSED;
    HtmlElem el = html_dom_by_id("purchaseHolder");
    if (el) html_dom_set_display(el, "none");
    return 1;
}
// window.canShowExternalLinks (`canShowExternalLinks ? openTab(terms) : showWindow(47)`):
// the desktop client and the web build both open external links in a browser tab.
H(h_can_show_external_links) { UNUSED; return 1; }
// windows[20].tabIndex = n (Contact opens Advertise on its Contact tab)
static int g_advertise_tab;
H(h_advertise_tab_index) { UNUSED; g_advertise_tab = arg_int(args, argc, 0, 0); return 1; }
H(h_advertise_switch_tab) { UNUSED; g_advertise_tab = arg_int(args, argc, 0, 0); menu_set_active_tab(g_advertise_tab); return 1; }

static const HtmlHandlerEntry HANDLERS[] = {
    // settings inputs
    {"setSetting", h_set_setting},
    {"updateWSliderLabel", h_noop},   // undefined in Krunker's own JS
    {"updateSliderLabel", h_servers}, // WP-E: the host settings' customSet<n> labels (ui_host_config.c)
    // windows
    {"showWindow", h_show_window},
    {"closeWindow", h_close_window},
    {"selectClass", h_select_class},
    {"voteOnMatch", h_vote_on_match}, // WP-E
    {"toggleTeam", h_toggle_team},     // updSyncDat("t"): the comp menu Join / Leave Team, #selectT (comp_ui.c)
    {"openHostWindow", h_open_host_window},
    {"openJoinWindow", h_open_join_window},
    // sounds
    {"playTick", ui_sound_play_tick},  // client/src/ui/ui_sound.c
    {"playSelect", ui_sound_play_select},
    {"SOUND.play", ui_sound_play},
    // analytics, ads, payments, FTUE: nothing in KrunkNative
    {"logBtnFRVR", h_noop},
    {"XsollaPopup", h_noop},
    {"dropsPopup", h_noop},
    {"abTestsPopup", h_noop}, // its footer link is dropped by tools/ui-import (index_page.py)
    {"expertMode", h_noop},
    {"purchaseCancel", h_purchase_cancel},
    // v7 menu chrome
    {"closWind", h_close_wind},
    {"showGames", h_show_games},
    {"openDiscord", h_open_discord},
    {"openGuide", h_open_guide},
    {"setSpect", h_set_spect},
    {"window.canShowExternalLinks", h_can_show_external_links},
    {"windows.20.tabIndex=", h_advertise_tab_index},
    {"windows.20.switchTab", h_advertise_switch_tab},
    // navigation is never allowed
    {"openURL", h_open_url},
    {"openTab", h_open_url},
    {"window.open", h_open_url},
    // play / host / join
    {"quickJoinRegion", h_servers},
    {"joinGame", h_join_game},
    {"createPrivateRoom", h_create_private_room},
    {"playMap", h_play_map},
    {"selectHostMap", h_select_host_map},
    {"knHostMap", h_kn_host_map},
    {"knRetry", h_kn_retry},
    {"knHostOffline", h_kn_host_offline},
    {"copyInviteLink", h_copy_invite_link},
    // account
    {"registerAcc", h_commit_account},
    {"loginAcc", h_commit_account},
    // settings window buttons
    {"resetSettings", h_reset_settings},
    {"exportSettings", h_export_settings},
    {"knDialog", h_dialog},
    // desktop client chrome (ui_client_settings.c)
    {"clientExitPopup", h_client},
    {"closeClient", h_client},
    {"window.closeClient", h_client},
    {"knClientSet", h_client},
    {"knClientAction", h_client},
    // popups / misc page chrome
    {"bdayPlay", h_bday_play},
    {"kn.hideErrorNotification", h_hide_error_notification},
    {"checkTerms", h_check_terms},
    {"clearPops", h_clear_pops},
    {"toggleSpect", h_toggle_spect},
    {"claimReward", h_claim_reward},
    {"shoPolicePop", h_police_pop},
    {"sub2FA", h_noop},
    {"focusChat", h_focus_chat},
    {"sendReaction", h_noop},
    {"closeTwitch", h_noop},
    {"toggleHF", h_servers},
    {"setExpandedRegion", h_servers},
    // controls / loadout / customisation
    {"skinSelector", h_skin_selector},
    {"selectSkin", h_select_skin},
    // selectHat / selectBody / ... / selectReticle(i, url) / selectStreak / selectCharm: ui_loadout_handle
    {"hidePolicePop", h_hide_police_pop},
    {"updateWindow", h_update_window},
    {"windows.34.toggleLog", h_toggle_log},
    {"selectSkinColor", h_select_skin_color},
    {"selectStat", h_select_stat},
    {"favoriteM", h_servers},          // servers_ui.c (Custom Games cards)
    {"GUI.endScreen.changeTab", h_end_screen_tab},
    // windows[N].*
    {"windows.0.changeTab", h_settings_change_tab},
    {"windows.0.searchList", h_settings_search_list},
    {"windows.0.collapseFolder", h_settings_collapse_folder},
    {"windows.1.searchList", h_servers},
    {"windows.2.action", h_loadout_action},
    {"windows.3.switchTab", h_switch_tab},
    // WP-E Host Game rows (ui_host_config.c, routed through servers_ui_handle)
    {"windows.7.toggle", h_servers},
    {"windows.7.changeWep", h_servers},
    {"windows.7.toggleWep", h_servers},
    {"windows.7.changeCls", h_servers},
    {"windows.7.toggleCls", h_servers},
    {"windows.7.changeEffects", h_servers},
    {"windows.7.toggleEffects", h_servers},
    {"updateSliderLabel2", h_servers},
    {"windows.7.presetAction", h_noop},
    {"windows.7.updateSet", h_noop},  // undefined in Krunker's own JS
    {"windows.9.switchTab", h_games_tab},
    {"windows.9.searchMaps", h_servers}, // servers_ui.c: the catalogue / loaded games by name
    {"windows.9.loadMore", h_servers}, // brain: the next /api/maps page (servers_ui.c); offline: one page
    {"windows.13.premium", h_store_tab},
    {"windows.13.purchase", h_store_tab},
    // the settings head writes `window.windows[0].x(...)`; accept the prefixed spelling too
    {"window.windows.0.changeTab", h_settings_change_tab},
    {"window.windows.0.searchList", h_settings_search_list},
    {"window.windows.0.collapseFolder", h_settings_collapse_folder},
    {"window.setSetting", h_set_setting},
};

// Names one module owns: each *_handle() returns -1 when the name is not its own, else the
// handler result (0 = `return false` cancels the default action).
typedef int (*UiModuleHandler)(const char *name, const HtmlEvent *ev, const HtmlArg *args, int argc);
static const UiModuleHandler MODULE_HANDLERS[] = {
    v10_menu_handle,           // v10_menu.c / v10_login.c: the v10 page's menu shell, loginOrRegister
    account_ui_handle,         // account_ui.c: Account window, logout, mail, profile
    servers_ui_handle,         // servers_ui.c: Servers / Server Finder / Games / Host / Join
    store_ui_handle,           // store_ui.c: Store (windows[13])
    clan_ui_handle,            // clan_ui.c: Clans (windows[12])
    ranked_ui_handle,          // ranked_ui.c: Ranked (windows[26])
    turf_ui_handle,            // turf_ui.c: v10 Turf Wars
    v10_news_handle,           // v10_news.c: What's New (windows[34]), Guide, Community & Events
    v10_competitive_handle,    // v10_competitive.c: v10 Leaderboards, the view stack
    ui_settings_handle,        // ui_settings.c: Game Settings (windows[0])
    ui_controls_handle,        // ui_controls.c: Change Controls (windows[6])
    v10_customize_handle,      // v10_customize.c: the v10 page's Change Loadout tabs, Title window
    v10_daily_spin_handle,     // v10_daily_spin.c: the sidebar's Daily Spin
    ui_loadout_handle,         // ui_loadout.c: Change Loadout (windows[2]) and the pickers
    hud_ui_handle,             // hud_ui.c: chat, player list, end screen, death screen
    ui_client_settings_handle, // ui_client_settings.c: Settings > Client, the Exit item
    update_ui_handle,          // update_ui.c: knUpdateNow / knUpdateDownload (the Outdated box)
    object_manager_ui_handle,  // object_manager_ui.c: the KrunkNative Object Manager (knOm*)
};

static int h_fallback(void *user, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    UNUSED;
    const char *name = html_ui_current_handler(); // logoutAcc, windows[4].*, checkedSwitchServer, ...
    for (size_t i = 0; i < sizeof(MODULE_HANDLERS) / sizeof(MODULE_HANDLERS[0]); i++) {
        const int r = MODULE_HANDLERS[i](name, ev, args, argc);
        if (r >= 0) return r;
    }
    menu_log("unhandled inline handler %s on %s event", name ? name : "?", ev && ev->type ? ev->type : "?");
    return 1;
}

// ---------------------------------------------------------------- listeners (setupDom)

// #instructionHolder is the click-to-play target (Krunker: mousedown on the renderer
// canvas -> CONTROLS.toggle(true)); a click there with a window open just closes it.
static int l_instruction_holder_mousedown(void *user, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) user; (void) args; (void) argc;
    if (!menu_ui()->menu_open || (ev && ev->button != 0)) return 1;
    if (menu_window_is_open()) {
        menu_toggle_window(0);
        return 1;
    }
    menu_play();
    return 1;
}

static int point_in_elem(const HtmlElem el, const float x, const float y) {
    HtmlRect r;
    if (!el || !html_dom_get_rect(el, &r) || r.w <= 0.0f || r.h <= 0.0f) return 0;
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

// `if (e.target === wholder) toggleWindow(false)`: listeners get the deepest target, so
// a mousedown whose target is the holder itself hit the backdrop.
static int l_window_holder_mousedown(void *user, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) user; (void) args; (void) argc;
    if (!ev) return 1;
    HtmlElem holder = html_dom_by_id("windowHolder");
    if (ev->target == holder) {
        menu_toggle_window(0);
        return 1;
    }
    // Fallback for backends that resolve the target to the listener element: geometric test.
    if (ev->target) return 1;
    if (point_in_elem(html_dom_by_id("menuWindow"), ev->x, ev->y)) return 1;
    if (point_in_elem(html_dom_by_id("windowHeader"), ev->x, ev->y)) return 1;
    menu_toggle_window(0);
    return 1;
}

// The page itself never navigates: links to other pages / external sites (the import
// neutralised external hrefs to href="#" data-ext-href=...) open in a browser tab
// (ui_open_external: system browser natively, window.open on the web).
static int l_document_click(void *user, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) user; (void) args; (void) argc;
    if (!ev || !ev->target) return 1;
    HtmlElem a = html_dom_closest(ev->target, "a");
    if (!a) return 1;
    char href[512] = "";
    if (html_dom_get_attr(a, "href", href, sizeof(href)) < 0) href[0] = 0;
    char ext[512];
    if (html_dom_get_attr(a, "data-ext-href", ext, sizeof(ext)) >= 0 && ext[0]) {
        ui_open_external(ext);
        return 0;
    }
    if (!href[0] || !strcmp(href, "#")) return 0;
    if (!strncmp(href, "javascript:", 11)) return 1;
    ui_open_external(href);
    return 0;
}

static int target_is_form_control(const HtmlElem el) {
    char tag[16];
    if (!el || html_dom_tag(el, tag, sizeof(tag)) <= 0) return 0;
    return !strcmp(tag, "input") || !strcmp(tag, "textarea") || !strcmp(tag, "select");
}

// A control that takes typed text (Escape leaves it): not a checkbox / radio / range / button,
// which keep the focus after a click (the menu's Spectate switch, a settings toggle) and must
// not swallow the Escape that closes the menu.
static int target_takes_text(const HtmlElem el) {
    char tag[16], type[16] = "";
    if (!el || html_dom_tag(el, tag, sizeof(tag)) <= 0) return 0;
    if (strcmp(tag, "input") != 0) return !strcmp(tag, "textarea") || !strcmp(tag, "select");
    if (html_dom_get_attr(el, "type", type, sizeof(type)) < 0) return 1;
    for (char *c = type; *c; c++) *c = (char) (*c >= 'A' && *c <= 'Z' ? *c + 32 : *c);
    static const char *const NO_TEXT[] = {"checkbox", "radio", "range", "button", "submit", "reset", "color", "file", "image"};
    for (size_t i = 0; i < sizeof(NO_TEXT) / sizeof(NO_TEXT[0]); i++) {
        if (!strcmp(type, NO_TEXT[i])) return 0;
    }
    return 1;
}

// Keys typed into menu inputs must not reach the game: Escape blurs the focused input
// (document capture listener in krunknative.js, which also stopped propagation) and
// Enter in #accName commits the nickname. Otherwise ESC closes an open window first
// (CONTROLS.toggle), else toggles the menu. (Natively main.c consumes ESC before the
// page sees it; on the web the page owns ESC while the menu is open.)
static int l_window_keydown(void *user, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) user; (void) args; (void) argc;
    if (!ev) return 1;
    menu_idle_reset(); // controls.js keydown: idleTimer = 0
    MenuUi *ui = menu_ui();
    // web: GLFW's keydown (window capture) already bound this key in the controls window
    if (ui_controls_take_swallowed_key()) return 0;
    if (ui_console_page_key(ev)) return 0; // Enter/Escape/Tab/arrows/` in the console input
    // Escape on a focused switch / button (focused by its click) blurs it and goes on to the
    // menu below; a text field keeps it (blur only).
    const int escape_control = html_key_is_escape(ev->key) && target_is_form_control(ev->target) && !target_takes_text(ev->target);
    if (escape_control) html_dom_blur(ev->target);
    if (target_is_form_control(ev->target) && !escape_control) {
        // Enter in #chatInput (menu chat natively, every chat Enter on the web): send/blur.
        // Natively in game main.c routes the chat key before the page sees it.
        if (html_key_is_enter(ev->key) && ev->target == html_dom_by_id("chatInput")) {
            ui_controls_chat_enter(ui->client);
            return 0;
        }
        // the native prompt popup (ui_dialog.c): Enter = OK, Escape = Cancel
        // (clipboard shortcuts are the engine's text-input widgets)
        if (ui_dialog_prompt_focused(ev->target) && (html_key_is_enter(ev->key) || html_key_is_escape(ev->key))) {
            ui_dialog_answer(html_key_is_enter(ev->key));
            return 0;
        }
        if (html_key_is_escape(ev->key)) {
            html_dom_blur(ev->target);
            return 0;
        }
        // Enter in the login form logs in (a real login, account_ui.c, never a fake nickname);
        // KrunkNative: also from the password field, and the reset form submits its step
        if (html_key_is_enter(ev->key) && !v10_menu_login_open() && (ev->target == html_dom_by_id("accName") || ev->target == html_dom_by_id("accPass")))
            account_ui_handle("loginAcc", ev, NULL, 0);
        if (html_key_is_enter(ev->key) && account_ui_reset_enter(ev->target)) return 0;
        return 1;
    }
    if (html_key_is_escape(ev->key)) {
        if (v10_menu_login_escape()) return 0; // the v10 login / register modal goes first
        if (ui->menu_open && menu_window_is_open()) {
            if (object_manager_ui_escape()) return 0; // the Object Manager opened in game: straight back to it
            menu_toggle_window(0);
            return 0;
        }
        // The Lost Connection screen stays (v7 f2040: no way back into the dead session); its
        // tabs (Reconnect, Find a Game, ...) leave it.
        if (ui->menu_open && menu_error_screen_shown()) return 0;
#ifdef __EMSCRIPTEN__
        // Web: Escape never leaves the menu (v3.1.1 pressButton(27) only toggles the end
        // screen). Escape is no user activation, and the browser ends a pointer lock taken
        // during it on the same key, so the game showed with a dead mouse until the
        // lock-loss check reopened the menu. Phones have no lock: CONTROLS.toggle(false).
        if (ui->menu_open && !ui_mobile_active()) return 0;
#endif
        // menu_request_open(0) requests the pointer lock itself (no second raw request:
        // two pending requests make Chrome answer pointerlockchange(null) and grant neither).
        menu_request_open(!ui->menu_open);
        return 0;
    }
    return 1;
}

static int l_window_resize(void *user, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) user; (void) ev; (void) args; (void) argc;
    menu_scale_base();
    return 1;
}

void ui_handlers_install(MenuUi *ui) {
    html_ui_set_handlers(HANDLERS, sizeof(HANDLERS) / sizeof(HANDLERS[0]), ui, h_fallback);
    html_ui_set_listener("instructionHolder", "mousedown", l_instruction_holder_mousedown);
    html_ui_set_listener("windowHolder", "mousedown", l_window_holder_mousedown);
    html_ui_set_listener(NULL, "click", l_document_click);
    html_ui_set_listener(NULL, "keydown", l_window_keydown);
    html_ui_set_listener(NULL, "resize", l_window_resize);
}
