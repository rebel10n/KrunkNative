// The inline-handler table: every `on*` callee in ui/index.html and the captured window
// markup of Krunker v7.1.4 (ui/windows), with the bodies v7's src/index.js gives them, plus
// the KrunkNative-only names C writes (knRetry, knHostOffline, knDialog, knClient*). Names
// owned by one module (settings, controls, loadout, HUD, account, store, servers, ranked,
// clans) are answered by that module's *_handle() through h_fallback. Analytics / ad /
// payment hooks (logBtnFRVR, XsollaPopup, ...) are no-ops. Also installs the non-inline
// listeners (setupDom's addEventListener calls).
#include <v10_menu.h>
#include "klog.h"
#include <v10_social.h>
#include <v10_studios.h>  // v10 Studios (v10_studios.c)
#include <ui_logic.h>
#include <v10_wild_offer.h>
#include <v10_wallet.h> // v10 Wallet and Inventory (v10_wallet.c)
#include <seek_prefs.h> // seek_prefs_handle
#include <lobby_ui.h> // WP-E voteOnMatch
#include <ui_host_config.h> // WP-E createPrivateRoom: the host config request
#include <host_flow_ui.h>   // the Host Game window: openHostWindow, selectHostMap, showGames(true), Start Game
#include <game_config.h>    // GAME_HOST_CUSTOM
#include <cJSON.h>
#include <map_catalog.h>  // Host Game / Games window maps
#include <community_host.h> // community_pick / community_host_slot (the Host Game seam)
#include <sound_events.h> // ui_sound_play_tick / ui_sound_play
#include <spectate.h>     // spectate_set_requested (toggleSpect)
#include <console.h>      // #knConsoleInput keys (developer console, ui_console.c)
#include <ui_mobile.h>    // isMobile: Escape resumes without a pointer lock (ui_mobile.c)
#include <backend_ui.h>   // brain backend: account (account_ui.c) + matchmaker (servers_ui.c)
#include <alt_manager_ui.h> // the saved accounts window (alt_manager_ui.c)
#include <v10_ui.h> // v10 Leaderboards / view stack (v10_competitive.c)
#include <v10_party.h>    // v10 Invite / Join (v10_party.c)
#include <strike_econ.h>  // Krunker Strike: the economy's knStrike* handlers (strike_econ.c)
#include <paint_picker.h> // the owned card's paint popup (paint_picker_handle)
#include <client_settings.h> // Settings > Client tab + the Exit menu item (ui_client_settings.c)
#include <live_events_client.h> // v10.0.3 live events: eventPopup (v10_events.c)
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
    KN_LOG_VERBOSE2("[ui] handler setSetting %s=%s\n", key, text);
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
// A same-site game link (v10's Friends tab Join: openURL('/?game=<gid>', 1)) joins it in place.
H(h_open_url) {
    UNUSED;
    const char *url = arg_str(ev, args, argc, 0, "");
    if (!v10_party_open_url(url)) ui_open_external(url);
    return 1;
}

// openHostWindow(false, type): the chooser's server-type cards (window 42) open the Host Game window
// (host_flow_ui.c, v10's host_games) for that config.hostTypes index.
H(h_open_host_window) {
    UNUSED;
    host_flow_open(arg_int(args, argc, 1, GAME_HOST_CUSTOM));
    return 1;
}
// v10 Invite / Join (v10_party.c): openJoinWindow(a, skipClipboard), copyInviteLink(toast)
H(h_open_join_window) { UNUSED; v10_party_open_join_window(arg_bool(ev, args, argc, 1, 0)); return 1; }
H(h_copy_invite_link) { UNUSED; v10_party_copy_invite_link(arg_bool(ev, args, argc, 0, 0)); return 1; }

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
// joinGame(): _72569(gameURL.value || "") - a game link or code (v10_party.c)
H(h_join_game) {
    UNUSED;
    char text[512] = "";
    HtmlElem input = html_dom_by_id("gameURL");
    if (input) html_dom_get_value(input, text, sizeof(text));
    v10_party_join_field(text);
    return 1;
}
// Host a map of the catalogue (shared/src/map_catalog.c) on the embedded local server:
// it joins the client's map list (map_catalog_slot: Shipyard, Soul Sanctum and the
// community maps are not in the default rotation) and the offline game starts or reloads
// on it with `mode` (Krunker mode id, -1 = keep). Connected to a server, Krunker's room
// request has no native equivalent: the client plays there (menu_host_offline).
void host_catalog_map(const int catalog_index, const int mode) {
    Client *client = menu_ui()->client;
    const int slot = catalog_index >= 0 ? map_catalog_slot(&client->game.maps, &client->game.map_count, (size_t) catalog_index) : -1;
    if (catalog_index >= 0 && slot < 0) menu_log("host: map %s could not be loaded", map_catalog_at((size_t) catalog_index)->key);
    community_host_slot(client, slot, mode); // community_maps.c: the Host Game seam
}

// createPrivateRoom(): Krunker collects the checked #gameMap<i> boxes and creates the room;
// here the first checked map becomes the selection and the embedded server is started.
H(h_create_private_room) {
    UNUSED;
    // v7: if (config.isComp) custom.host.ranked
    if (ranked_ui_in_ranked()) {
        HtmlElem msg = html_dom_by_id("hostGameMsg");
        if (msg) {
            char html[256];
            snprintf(html, sizeof(html), "<span class='error'>%s</span>", I18N("custom.host.ranked", "Can't host match while in ranked."));
            html_dom_set_html(msg, html);
        }
        return 1;
    }
    host_flow_start(); // the Start Game step: krunker.io's hosting or Box Office servers (host_flow_ui.c)
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
// KrunkNative joins a lobby on it (servers_ui.c), else plays the catalogue map offline (the
// embedded local server; Free for All unless Host Game picked another mode).
H(h_play_map) {
    UNUSED;
    const char *name = arg_str(ev, args, argc, 0, "");
    const int index = community_catalogue_index(name, NULL);
    // brain: a lobby on that map (servers_ui.c); a published map: its JSON, played offline there
    if (servers_ui_play_map(name, index >= 0)) return 1;
    if (index < 0) {
        menu_log("playMap: %s is not in the map catalogue", name);
        return 1;
    }
    // no brain lobby to join: the offline game on it, leaving the server this client is joined to
    // (the console's offline <map>; host_catalog_map would keep playing on that server)
    ui_console_go_offline(menu_ui()->client, index, "play");
    return 1;
}

// The end screen's raid promo "Play Raid" (hud_endscreen_v10.c). v10's card opens the Raid Server
// Setup (openHostWindow(false, 4)): forceDat maps [18] Tortuga, modes [23] Raid, 8 players, and
// Create Game makes a private room. KrunkNative hosts nothing from the client (doc/backend.md 8.11),
// so the card seeks a public Raid lobby on Tortuga through the Server Type's matchmaker
// (servers_ui_play_raid), and v10's "Couldnt find any" when there is none. Never the offline game.
// doc/v10/end-raid.md has v10's card and its offsets.
#define RAID_PLAY_MAP "Tortuga"
H(h_play_raid) {
    UNUSED;
    servers_ui_play_raid(RAID_PLAY_MAP);
    return 1;
}

// selectHostMap(name, modURL, id, creator): Krunker remembers the community game for the
// Host Game window (selectedCustomMapId). KrunkNative opens the Host Game flow at its Mode step with
// that map chosen: a catalogue map at once; a published map (kn_brain) once its copy is downloaded
// and passes v10's checks (community_fetch), else v10's message in an alert.
typedef struct {
    char name[96], creator[64];
} HostPick;

static void on_host_map_fetched(void *user, const cJSON *json, const char *error) {
    HostPick *pick = user;
    if (json) {
        menu_log("selectHostMap: %s -> host flow", pick->name);
        host_flow_open_published(pick->name, pick->creator);
    } else {
        menu_log("selectHostMap: %s refused: %s", pick->name, error ? error : "?");
        ui_alert(error ? error : I18N("matchmaker.InvalidGameId", "Game not found."));
    }
    free(pick);
}

H(h_select_host_map) {
    UNUSED;
    const char *name = arg_str(ev, args, argc, 0, ""), *creator = arg_str(ev, args, argc, 3, "");
    const int index = community_catalogue_index(name, creator);
    if (index >= 0) {
        host_flow_open_map(index);
        return 1;
    }
    HostPick *pick = calloc(1, sizeof(*pick));
    if (!pick) return 1;
    snprintf(pick->name, sizeof(pick->name), "%s", name);
    snprintf(pick->creator, sizeof(pick->creator), "%s", creator);
    community_fetch(pick->name, on_host_map_fetched, pick);
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
H(h_kpd_demo_search) { UNUSED; ui_loadout_kpd_demo_search(arg_bool(ev, args, argc, 0, 0)); return 1; } // kpdDemoSearch(clear)
H(h_kpd_demo_page) { UNUSED; ui_loadout_kpd_demo_page(arg_int(args, argc, 0, 0)); return 1; }          // kpdDemoPage(delta)
H(h_watch_demo) { UNUSED; hud_votekick_watch_demo(arg_str(ev, args, argc, 0, ""), arg_str(ev, args, argc, 1, "")); return 1; } // watchDemo(tok, ct)
H(h_copy_demo_link) { UNUSED; hud_votekick_copy_demo_link(arg_str(ev, args, argc, 1, ""), arg_str(ev, args, argc, 2, "")); return 1; } // copyDemoLink(this, tok, ct)
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
    ui_alert(I18N("kn.ads.failed", "Ad Failed. Contact your Ad Provider"));
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
// showGames(hostType): windows[9].hostType = hostType || null; showWindow(10). The chooser's Community
// Games card (showGames(true)) browses the community maps for the Host Game window; Custom Games browses too.
H(h_show_games) {
    UNUSED;
    if (arg_bool(ev, args, argc, 0, 0)) {
        host_flow_browse_community();
        return 1;
    }
    menu_show_window(10, 0);
    return 1;
}
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
    {"knCarousel", h_servers}, // the server browser carousel's prev / next (servers_ui.c)
    {"joinGame", h_join_game},
    {"createPrivateRoom", h_create_private_room},
    {"playMap", h_play_map},
    {"knPlayRaid", h_play_raid},
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
    {"kpdDemoSearch", h_kpd_demo_search},
    {"kpdDemoPage", h_kpd_demo_page},
    {"watchDemo", h_watch_demo},
    {"copyDemoLink", h_copy_demo_link},
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
    // WP-E Host Game rows (ui_host_config.c, routed through servers_ui_handle; the flow's own rows: host_flow_ui.c)
    {"windows.7.toggle", h_servers},
    {"windows.7.changeWep", h_servers},
    {"windows.7.toggleWep", h_servers},
    {"windows.7.changeCls", h_servers},
    {"windows.7.toggleCls", h_servers},
    {"windows.7.changeEffects", h_servers},
    {"windows.7.toggleEffects", h_servers},
    {"updateSliderLabel2", h_servers},
    {"windows.7.presetAction", h_noop},
    {"windows.7.updateSet", h_servers}, // the Server Upgrades' cost line (ui_host_config.c)
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
    host_flow_ui_handle,       // host_flow_ui.c: the Host Game flow (knHostFlow), every Host Game entry point
    v10_menu_handle,           // v10_menu.c / v10_login.c: the v10 page's menu shell, loginOrRegister
    v10_rewards_handle,        // v10_rewards.c: v10.0.2 signup rewards, windows[32] (before account_ui.c's v7 ones)
    v10_events_handle,         // v10_events.c: v10.0.3 live events (eventPopup)
    account_ui_handle,         // account_ui.c: Account window, logout, mail, profile
    servers_ui_handle,         // servers_ui.c: Servers / Server Finder / Games / Host / Join
    store_ui_handle,           // store_ui.c: Store (windows[13])
    clan_ui_handle,            // clan_ui.c: Clans (windows[12])
    ranked_ui_handle,          // ranked_ui.c: Ranked (windows[26])
    turf_ui_handle,            // turf_ui.c: v10 Turf Wars
    v10_news_handle,           // v10_news.c: What's New (windows[34]), Guide, Community & Events
    v10_competitive_handle,    // v10_competitive.c: v10 Leaderboards, the view stack
    v10_social_handle,         // v10_social.c: v10 Social hub, player profile, clan view (openSocial, openPlayerProfile, ...)
    v10_studios_handle,        // v10_studios.c: v10 Studios (openStudios, knStudios)
    v10_market_handle,         // v10_market.c: v10 Market & Trading (openExchange, v10mk.*)
    ui_settings_handle,        // ui_settings.c: Game Settings (windows[0])
    ui_controls_handle,        // ui_controls.c: Change Controls (windows[6])
    v10_customize_handle,      // v10_customize.c: the v10 page's Change Loadout tabs, Title window
    v10_daily_spin_handle,     // v10_daily_spin.c: the sidebar's Daily Spin
    v10_wild_offer_handle,     // v10_wild_offer.c: v10 Wild Offers (knWo*)
    v10_wallet_handle,         // v10_wallet.c: v10 Wallet and Inventory (knV10Wallet*)
    paint_picker_handle,       // paint_picker.c: the owned card's paint popup (paintOpen, paintPick, paintApply)
    ui_loadout_handle,         // ui_loadout.c: Change Loadout (windows[2]) and the pickers
    hud_ui_handle,             // hud_ui.c: chat, player list, end screen, death screen
    trade_ui_handle,           // trade_ui.c: the live trade (windows[36].*, acceptRequest, requestTrade)
    ui_client_settings_handle, // ui_client_settings.c: Settings > Client, the Exit item
    seek_prefs_handle,         // seek_prefs.c: Settings > Quickplay's Seek Modes / Seek Maps switches (knSeekSwitch)
    update_ui_handle,          // update_ui.c: knUpdateNow / knUpdateDownload (the Outdated box)
    object_manager_ui_handle,  // object_manager_ui.c: the KrunkNative Object Manager (knOm*)
    alt_manager_ui_handle,     // alt_manager_ui.c: the KrunkNative saved accounts (knAlt*)
    v10_party_ui_handle,       // v10_party.c: the server password popup (submitGameCode)
    strike_econ_handle,        // strike_econ.c: Krunker Strike's economy (knStrikeEquip, knStrikeSpin, knStrikeBuy, ...)
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
        // KrunkNative: also from the password and email fields (an email signs in too), and the
        // reset form submits its step
        if (html_key_is_enter(ev->key) && !v10_menu_login_open() &&
            (ev->target == html_dom_by_id("accName") || ev->target == html_dom_by_id("accPass") || ev->target == html_dom_by_id("accEmail")))
            account_ui_handle("loginAcc", ev, NULL, 0);
        if (html_key_is_enter(ev->key) && account_ui_reset_enter(ev->target)) return 0;
        return 1;
    }
    if (html_key_is_escape(ev->key)) {
        if (v10_menu_login_escape()) return 0; // the v10 login / register modal goes first
        if (v10_social_key(ev->key)) return 0; // v10's view stack (profile, clan view, post): Escape pops it
        if (v10_wallet_key(ev->key)) return 0; // the Wallet and Inventory modal (and the Quick Sell / item page over it)
        if (v10_studios_key(ev->key)) return 0; // the Studios window, a studio, its dialog and prompts
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
