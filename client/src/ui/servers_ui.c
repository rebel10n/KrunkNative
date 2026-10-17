// Krunker v3.1.1 matchmaker client over the brain's public HTTP routes
// (client/include/backend.h, doc/backend.md section 4). Readable source:
// public/js/src/index.js (the startup seek at the end of the file, joinServer,
// switchServer / checkedSwitchServer, setGameInstanceId, quickJoinRegion,
// windows[1] = the Servers window) and src/libs/ClientManager.js (seek, getRegion,
// fetchGameList / fetchGameInfo, PING_REGION_CACHE_KEY = "pingRegion7").
//
// Joining: /mm/seek-game returns the game server's native TCP host/port, the lobby
// id and a join ticket (clientId). The native client connects over TCP, the web
// client through net-bridge (<bridge>/native?target=host:port, net_web_target_url, or the
// node's webUrl) and
// both send NET_PACKET_JOIN {game_id, client_key = ticket, session, name} right
// after HELLO (net.c / net_web.c). v3.1.1's switchServer reloads the page with
// ?game=<id>: the web build does exactly that, the native client re-seeks in place.
#include <backend.h>
#include <backend_ui.h>
#include <console.h>     // ui_console_go_offline: the Games window's Play leaves a joined lobby
#include <game_reload.h> // game_reload_server_updating: the auto-restart on a server update
#include <html_ui.h>
#include <ui_logic.h>
#include <map_catalog.h>
#include <community_maps.h> // published maps: the Search tab request, the Play download
#include <online_mode.h> // server finder URL, online / offline mode
#include <ruleset.h>       // seek ?ruleset=v10
#include <build_info.h>   // kn_build_protocol (seek-game proto)
#include <modes.h>       // gameModes alias / name (server rows, host modes)
#include <game_config.h> // WP-E host types (GAME_HOST_CUSTOM, hostTypes titles)
#include <ui_host_config.h> // WP-E: the Host Game window's mode / advanced / server bodies
#include <host_flow_ui.h>   // the Host Game window (windows[8]): its markup and tab switch
#include <server_names.h>  // short ?game= ids (CH2:kz862 for CHITOWN2:kz862)
#include <join_errors.h>   // a failed join's text and place (join_errors.c)
#include <region_ping.h>   // the seek region's ping race (v10 getRegion)
#include <seek_prefs.h>    // Settings > Quickplay: seek mode / map / server type (mm-token)
#include <seek_choice.h>   // the seek's region (a saved Default Region is kept) and a full lobby answer
#include <krunker_lobbies.h> // official krunker.io lobbies (native only, mm-token)
#include <krunker_map_source.h> // the official map list: v10 playMap / dsma indices
#include <krunker_net.h>     // krunker_net_in_client: official lobbies join here unless the browser opt-out
#include <v10_party.h>     // the server password popup (ERROR 004005)
#include <strike_director.h> // Krunker Strike: seek through the director, else the finder's Strike pool
#include <strike_ui.h>       // Krunker Strike's own server password prompt (strike_menu.c)
#include <recent_lobbies.h>  // F4 / Quick Match / quick join never land back in the lobby they leave
#include <v10_studios.h>     // My Games: the Studios row (v10_studios.c)
#include <flavor.h>
#include <strings.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#define SERVER_BROWSER_RATE_MS 10000.0 // config.serverBrowserRate
#define RESTORE_WAIT_SECONDS 2.0       // startup seek waits this long for the saved session

// config.regionNames. KrunkNative's regions are its edge locations; the id the game servers
// announce (server.cfg `region`, deploy/ansible kn_gameserver_region) is the 2-letter code.
// Links carry the short form (?game=CH2:kz862: the code + the node's instance number,
// shared/include/server_names.h); the server browser, the region pickers and the menu's
// region label show the long names (the region's place name, a server's "Chicago 2": the
// city + the instance number), the code / node id as the tooltip.
static const struct {
    const char *id, *code, *name, *city;
} k_regions[] = {
    {"SV", "SV", "Santa Clara", "Santa Clara"}, {"LA", "LA", "Los Angeles", "Los Angeles"}, {"CH", "CH", "Chicago", "Chicago"},
    {"VA", "VA", "Ashburn, Virginia", "Ashburn"}, {"local", "LO", "Local", "Local"},
};

static Client *g_client;
static int g_brain_mode;
static int g_started;            // the startup seek ran
static int g_startup_hold;       // a restart's resume holds the startup seek (servers_ui_hold_startup_seek)
static char g_pending_play[96], g_pending_game[160]; // ?play= / ?game= (servers_ui_query), taken by the startup seek / tick
static double g_start_time;
static char g_region[32];        // region id the client seeks in
static char g_exclude_game[160]; // v10 Quick Match: the next seek's excludeGame (the lobby it leaves)
// the lobbies (game ids) and servers (host:port, finder / Strike) this client was in lately, most
// recent first: F4 / Quick Match send them as excludeRecent / the finder's exclude, quick join tries
// them last, so a new seek never bounces between the last two (recent_lobbies.h)
static RecentLobbies g_recent_games, g_recent_servers;
static char g_game_id[160];      // gameId: the lobby this client is in (or joining), canonical (CHITOWN2:kz862)
static char g_game_region[32];   // its region (the seek answer's), for the short link form ("" = unknown)
static int g_seek_inflight;
static int g_director_mode;      // Krunker Strike: the director matchmakes at startup (no brain needed)
static char g_strike_code[16];   // the private lobby code of this session ("" = public)
static char g_seek_game[160];    // the game id the running seek asked for ("" = any)
static char g_account_game[160]; // AccountRequired: the lobby a guest was refused (joined after login)
// Find New Game (F4) / Quick Match (servers_ui_find_new_game): the seek in flight is one, made
// from this lobby (restored when it finds nothing: the player stays in the game, told by a toast),
// and with server type Both an official krunker.io lobby is the fallback
static int g_find_seek, g_find_both;
static char g_find_prev_game[160];
// full lobbies the seek answers named in a row (never joined): at most SEEK_FULL_SKIPS_MAX, then GameFull
#define SEEK_FULL_SKIPS_MAX 3
static int g_full_skips;
// Reload (F5's relaunch, the console's `reload`; game_reload.c): the lobby the player was in, sought
// by its id (no Quickplay launch seek, no excludeGame). Gone or full: one plain seek, and a notice
// says why (reload_fallback). g_reload_game: the new process's lobby, taken by the startup seek;
// g_reload_next marks the next seek() as a reload's, g_reload_seek the one in flight.
static char g_reload_game[160];
static int g_reload_next, g_reload_seek;
static char g_password_game[160]; // the game (lobby id, or "host:port" direct) net_join.password was typed for
static void page_game_url(const char *id, const char *region);
// Source-console style pinning: a server the user picked by hand (console `connect`, Direct
// Connect, the finder list) or a console `disconnect` is never overridden by automatic
// matchmaking (the startup seek, the account-change reseek, a late seek answer). Only an
// explicit matchmaking action (Play / Find New Game on the error screen, the server browser,
// quick join, playMap, a ranked ticket, a server REDIRECT) goes through seek() and unpins.
enum { PIN_NONE, PIN_DIRECT, PIN_IDLE };
static int g_pin;
static int g_last_direct;        // the last session was a direct one: `reconnect` reuses net_address
static int g_left_offline;       // `disconnect` left the offline game: `reconnect` starts it again
static int g_quick_join_left;    // quickJoinRegion: remaining candidates
static char **g_quick_join;      // candidate game ids
static int *g_quick_join_kio;    // per candidate: an official krunker.io lobby (krunker_lobbies_open)
static int g_quick_join_kio_group; // the group quick-joined is a krunker.io group (no seek fallback)
static int g_quick_join_count, g_quick_join_next;

// Rejoining after the server went away, refused the join key or the transport lost the session
// (servers_ui_connection_lost)
enum { REJOIN_NONE, REJOIN_RESTART, REJOIN_KEY, REJOIN_DIRECT, REJOIN_DROP };
static int g_rejoin;             // REJOIN_* of the pending / running rejoin
static double g_rejoin_at;       // glfwGetTime() of its next attempt, 0 = none due
static double g_rejoin_until;    // REJOIN_RESTART / REJOIN_DIRECT: give up after this
static int g_rejoin_next;        // the coming seek() is a rejoin's
static int g_rejoin_seek;        // the seek in flight is a rejoin's
static int g_server_gone;        // the lobby's server stopped / refused us: `reconnect` may change game
static char g_rejoin_game[160];  // the lobby to seek again ("" = the region's best)
#define REJOIN_FIRST_DELAY 2.0   // s after the server's ERROR: its lobby-down reaches the matchmaker first
#define REJOIN_RETRY_DELAY 3.0   // s between seeks while the matchmaker has no lobby for us
#define REJOIN_WINDOW 120.0      // s a restart rejoin keeps trying (a deploy restarts in ~15 s)
#define REJOIN_KEY_WINDOW 30.0   // a second refused join key within this is shown, not retried
#define REJOIN_SETTLED 5.0       // s connected after which a rejoin is over
#define REJOIN_DROP_DELAY 1.0    // s after a lost session (no server ERROR) before the rejoin seek
#define REJOIN_DROP_STABLE 20.0  // a session that lasted this long resets the quick-drop count
#define REJOIN_DROP_MAX 4        // sessions in a row lost within REJOIN_DROP_STABLE: stop rejoining
static double g_session_started; // glfwGetTime() the current / last session connected (rejoin_tick), 0 none
static int g_drop_streak;        // consecutive lost sessions shorter than REJOIN_DROP_STABLE

static void rejoin_cancel(void) {
    if (g_rejoin == REJOIN_NONE && g_rejoin_at <= 0.0) return;
    g_rejoin = REJOIN_NONE;
    g_rejoin_at = g_rejoin_until = 0.0;
    menu_update_instructions(); // the error screen's "Rejoining..." line goes
}

// The transport gave up (client_net_give_up): the retry budget is spent, so no rejoin runs any more.
void servers_ui_gave_up(void) {
    g_rejoin_seek = 0;
    g_rejoin_next = 0;
    rejoin_cancel();
}

// windows[1]
static cJSON *g_games;           // serverListData (array of tuples, owned)
static double g_total_players, g_social_players;
static double g_last_load_ms = -1e18;
static int g_loaded;
static int g_expanded = -1;      // windows[1].expandedRegion
static char g_search[64];        // windows[1].serverSearch
// windows[1].tabIndex: the Servers window's two tabs, one per backend. Each shows v10's layout (the map
// carousel, Official Customs, Custom Games, then the regions) of its own list only: krunk.io (Box Office's
// lobbies and finder servers) or krunker.io (its official lobbies). A tab never mixes the two.
enum { SERVER_TAB_KRUNK = 0, SERVER_TAB_KRUNKER = 1, SERVER_TAB_COUNT = 2 };
static int g_server_tab = SERVER_TAB_KRUNK;
static int g_server_tab_backend = -1; // the backend the default tab last followed (server_tab_follow)
static double g_list_start_ms;   // windows[1].startTime (the list's arrival: time-left base)
static double g_timer_tick_ms;   // updateTimers: the last 1 s step
static int g_hide_full = -1;     // dK (krk_hideFull), -1 = not read yet

static double now_ms(void) { return glfwGetTime() * 1000.0; }

// The coordinator's product flavor pool (services/brain/public_routes.c ?flavor=, doc/backend.md
// "Flavor pools"): Krunker Box Office and the Krunker Strike app share one coordinator and its
// accounts, never a lobby. A base client sends nothing (its requests are unchanged); a Strike client
// adds flavor=strike to every lobby request (seek, game list, game info, ping list, host), so it
// only ever sees and joins Strike lobbies. `path` is the request so far (its '?' decides the joiner).
static const char *mm_flavor_param(const char *path) {
    static char param[40];
    if (flavor_get() == FLAVOR_BASE) return "";
    snprintf(param, sizeof(param), "%cflavor=%s", strchr(path, '?') ? '&' : '?', flavor_current()->key);
    return param;
}

// GET a coordinator lobby path with the flavor pool appended (mm_flavor_param).
static int mm_get(const char *path, BackendHttpFn fn, void *user) {
    char full[512];
    snprintf(full, sizeof(full), "%s%s", path, mm_flavor_param(path));
    return backend_http_get(full, fn, user);
}

// Krunker Strike matchmakes through the director only without a coordinator: with one (the
// default, coordinator.krunk.io) the coordinator's Strike pool is the matchmaker, its join tickets
// the admission (doc/v10/strike-director.md "One coordinator").
static int strike_via_director(void) { return strike_director_enabled() && !g_brain_mode; }

// UTILS.commaNumber
static void comma_number(char *out, const size_t n, const double value) {
    char digits[32];
    snprintf(digits, sizeof(digits), "%.0f", value > 0 ? value : 0);
    const size_t len = strlen(digits);
    size_t o = 0;
    for (size_t i = 0; i < len && o + 2 < n; i++) {
        if (i && (len - i) % 3 == 0) out[o++] = ',';
        out[o++] = digits[i];
    }
    out[o] = 0;
}

// The region's display code ("SV"); an id the table does not know shows as itself.
static const char *region_name(const char *id) {
    for (size_t i = 0; i < sizeof(k_regions) / sizeof(k_regions[0]); i++) {
        if (id && !strcmp(k_regions[i].id, id)) return k_regions[i].code;
    }
    return id && id[0] ? id : "?";
}

// The place name ("" unknown).
static const char *region_full_name(const char *id) {
    for (size_t i = 0; i < sizeof(k_regions) / sizeof(k_regions[0]); i++) {
        if (id && !strcmp(k_regions[i].id, id)) return k_regions[i].name;
    }
    return "";
}

// The long name the browser groups and the region pickers show: the place name, else the id.
static const char *region_long_name(const char *id) {
    const char *name = region_full_name(id);
    return name[0] ? name : region_name(id);
}

// The region's city for server names ("" unknown).
static const char *region_city(const char *id) {
    for (size_t i = 0; i < sizeof(k_regions) / sizeof(k_regions[0]); i++) {
        if (id && !strcmp(k_regions[i].id, id)) return k_regions[i].city;
    }
    return "";
}

// A server's proper name (browser rows, the menu's region label) from its game or node id:
// an edge node (CHITOWN2 in CH, short id CH2) is its region's city + instance number,
// "Chicago 2"; any other node its id with the instance number apart ("NY 2", "LOC").
static void server_display_name(const char *id, const char *region, char *out, const size_t n) {
    char node[96], short_node[KN_SHORT_NODE_MAX];
    snprintf(node, sizeof(node), "%.*s", (int) strcspn(id ? id : "", ":"), id ? id : "");
    kn_node_short_id(node, region, short_node, sizeof(short_node));
    size_t stem = strlen(node);
    while (stem > 0 && isdigit((unsigned char) node[stem - 1])) stem--;
    const char *digits = node + stem, *city = region_city(region);
    if (city[0] && strcmp(short_node, node) && !strncasecmp(short_node, region, strlen(region))) {
        snprintf(out, n, "%s%s%s", city, digits[0] ? " " : "", digits);
    } else if (stem && digits[0]) {
        snprintf(out, n, "%.*s %s", (int) stem, node, digits);
    } else {
        snprintf(out, n, "%s", node);
    }
}

// <option>s of the region pickers (Settings > Default Region, the ranked queue): every known
// region plus the ones the matchmaker lists, `selected` marked.
static cJSON *g_ping_regions;    // the last /mm/ping-list answer (owned)
static cJSON *g_row_pings;       // {region: ms} measured with the last list load (v10 pingData; -1 failed; owned)
char *servers_ui_region_options(const char *selected) {
    StrBuf sb = {0};
    const char *seen[64];
    int count = 0;
    for (size_t i = 0; i < sizeof(k_regions) / sizeof(k_regions[0]) && count < 64; i++) seen[count++] = k_regions[i].id;
    const cJSON *r;
    cJSON_ArrayForEach(r, g_ping_regions) {
        int known = 0;
        for (int i = 0; i < count; i++) known |= r->string && !strcmp(seen[i], r->string);
        if (!known && r->string && count < 64) seen[count++] = r->string;
    }
    for (int i = 0; i < count; i++) {
        sb_append(&sb, "<option value='");
        sb_append_esc(&sb, seen[i]);
        sb_append(&sb, "' title='");
        sb_append_esc(&sb, region_name(seen[i]));
        sb_appendf(&sb, "'%s>", selected && !strcmp(selected, seen[i]) ? " selected" : "");
        sb_append_esc(&sb, region_long_name(seen[i]));
        sb_append(&sb, "</option>");
    }
    return sb_finish(&sb);
}

// A brain-assigned lobby (net_join.c sends the session token only there): matchmaking on
// and not pinned to a direct server.
int servers_ui_brain_mode(void) { return g_brain_mode && g_pin == PIN_NONE; }
int servers_ui_pinned(void) { return g_pin == PIN_DIRECT; }

static int hide_full(void) {
    if (g_hide_full < 0) {
        char v[16] = "";
        g_hide_full = html_storage_get("krk_hideFull", v, sizeof(v)) >= 0 && !strcmp(v, "true");
    }
    return g_hide_full;
}

// ------------------------------------------------------------------ joining

static const char *row_i(const cJSON *t); // the lobby row's map (defined with the list rows)

// The map of a lobby the list knows ("Loading <map> ..." while the switch connects); NULL = not known.
static const char *game_map(const char *id) {
    if (!id || !id[0] || !g_games) return NULL;
    const cJSON *t;
    cJSON_ArrayForEach(t, g_games) {
        const char *gid = cJSON_GetStringValue(cJSON_GetArrayItem(t, 0));
        if (gid && !strcmp(gid, id)) {
            const char *map = row_i(t);
            return map && map[0] ? map : NULL;
        }
    }
    return NULL;
}

// joinServer(host, port, gameId, clientKey): point the transport at the game server.
// region: the lobby's (a seek answer's; NULL = unknown, looked up in the game list).
static void join_server(const char *host, const int port, const char *game_id, const char *client_key, const char *web_url,
                        const char *region) {
    Client *client = g_client;
    g_pin = PIN_NONE; // a matchmade (or server-redirected) session
    g_last_direct = 0;
    g_server_gone = 0;
    snprintf(g_game_id, sizeof(g_game_id), "%s", game_id ? game_id : "");
    snprintf(g_game_region, sizeof(g_game_region), "%s", region ? region : "");
    recent_lobbies_push(&g_recent_games, g_game_id);
    {
        char server[RECENT_LOBBY_ID_MAX];
        snprintf(server, sizeof(server), "%s:%d", host ? host : "", port);
        if (host && host[0]) recent_lobbies_push(&g_recent_servers, server);
    }

#ifdef __EMSCRIPTEN__
    // web: the page connects once (the startup seek); later switches reload with ?game=
    char ws[512];
    if (web_url && web_url[0]) {
        snprintf(ws, sizeof(ws), "%s", web_url);
    } else {
        net_web_target_url(ws, sizeof(ws), host, port); // the deployed bridge (kn-bridge) or this origin's /native
    }
#else
    (void) web_url;
#endif

    pthread_mutex_lock(&client->net_lock);
    if (strcmp(g_game_id, g_password_game) != 0) { // a server password is only sent to its game
        memset(client->net_join.password, 0, sizeof(client->net_join.password));
        g_password_game[0] = 0;
    }
    snprintf(client->net_join.game_id, sizeof(client->net_join.game_id), "%s", g_game_id);
    snprintf(client->net_join.client_key, sizeof(client->net_join.client_key), "%s", client_key ? client_key : "");
    snprintf(client->net_join.session, sizeof(client->net_join.session), "%s", active_account() ? account_session_token() : "");
    snprintf(client->net_address, sizeof(client->net_address), "%s", host);
    client->net_port = (unsigned short) port;
#ifdef __EMSCRIPTEN__
    snprintf(client->net_ws_url, sizeof(client->net_ws_url), "%s", ws);
#endif
    pthread_mutex_unlock(&client->net_lock);

    printf("[mm] joining %s at %s:%d\n", g_game_id, host, port);
    page_game_url(g_game_id, NULL);
    menu_show_connecting_map(game_map(g_game_id)); // the spinner, "Loading <map> ..." for the lobby's map
    client_net_switch_server(client);
}

static void switch_server(const char *id);
static void direct_join(const char *host, int port);

// NET_PACKET_REDIRECT (server console `send`, doc/backend.md 8.10): another lobby of the
// network (seek it: a fresh ticket from the matchmaker) or a direct host:port.
void servers_ui_redirect(const char *game_id, const char *host, const int port, const char *reason) {
    if (!g_client) return;
    printf("[mm] server redirect: %s%s%s:%d (%s)\n", game_id && game_id[0] ? game_id : "", game_id && game_id[0] ? " / " : "",
           host ? host : "", port, reason ? reason : "");
    if (game_id && game_id[0] && g_brain_mode && g_pin == PIN_NONE && backend_available() == 1) {
        switch_server(game_id);
        return;
    }
    if (host && host[0] && port > 0) {
        if (g_pin != PIN_NONE) direct_join(host, port); // a direct server sends us on: still direct
        else join_server(host, port, game_id, NULL, NULL, NULL);
    }
}

void servers_ui_join_ticket(const char *host, const int port, const char *game_id, const char *ticket, const char *web_url) {
    if (!g_client || !host) return;
    join_server(host, port, game_id, ticket, web_url, NULL);
    page_game_url(NULL, NULL); // a ranked match (gameConfig.isFromQueue): the link is "/"
}

// A matchmaker.* value as plain text (the error screen and the toast escape it): v10's
// "<br><small>" lines become ". ", other tags go. Static buffer: one message at a time.
// v3.1.1 matchmaker error names -> i18n matchmaker.* texts (the startup seek's catch)
static void fatal_error(const char *message);

// The matchmaker's error word as the player reads it (join_errors.c: v10's texts, KN's where v10 has none).
// The buffer is reused by the next call: the callers use the text at once.
static const char *seek_error_text(const char *error) {
    static char out[256];
    join_error_text(error, out, sizeof(out));
    return out;
}

// A failed join or lobby search the player hears about (the seek, krunker_lobbies.c's no-room and
// no-list answers): v10's error popup while the player stays in a game, else the Lost Connection
// screen (join_error_place).
void servers_ui_seek_failed(const char *text) {
    if (join_error_place(g_client && g_client->net_connected) == JOIN_ERROR_POPUP) ui_error_popup(text);
    else fatal_error(text); // v10's error screen (join_error_place: the screen in a game too)
}

static int token_refusal(const char *error) {
    return error && (!strcmp(error, "InvalidClientValidationToken") || !strcmp(error, "ExpiredClientValidationToken") ||
                     !strcmp(error, "InvalidClientValidationTokenData") || !strcmp(error, "InvalidClientIP"));
}

// The error screen of an AccountRequired seek offers "Log in" (menu_ui.c).
int servers_ui_account_required(void) { return g_account_game[0] && !active_account(); }

// ------------------------------------------------------------------ game links
// v3.1.1 ClientManager.generateHref(id) = "/?game=" + id, the id unescaped. KrunkNative writes
// the short form of the id (shared/include/server_names.h): the address bar and invite links
// read ?game=CH2:kz862 for the lobby CHITOWN2:kz862 (region CH); old long links still work, the
// brain resolves both (registry_lobby_find). Only what would break the query is escaped
// (% & # + space, quotes, controls). Readers accept both forms: servers_ui_query and
// URLSearchParams decode %3A. The page URL is web only (html_page_set_url: no-op natively).
static char g_link_game[160];    // the lobby the address bar / Invite name, link form ("" = none)

// The region of a lobby the client knows: the current one (its seek answer), else the game
// list's tuple. NULL = unknown.
static const char *game_region(const char *id) {
    if (!id || !id[0]) return NULL;
    if (g_game_region[0] && !strcmp(id, g_game_id)) return g_game_region;
    const cJSON *t;
    cJSON_ArrayForEach(t, g_games) {
        const char *gid = cJSON_GetStringValue(cJSON_GetArrayItem(t, 0));
        const char *region = cJSON_GetStringValue(cJSON_GetArrayItem(t, 1));
        if (gid && region && region[0] && !strcmp(gid, id)) return region;
    }
    return NULL;
}

// The link form of a game id: its short node id when the lobby's region is known (region, else
// game_region), else the id as it is (a short id stays short, an unknown long one long).
static void game_link_id(const char *id, const char *region, char *out, const size_t n) {
    if (!region || !region[0]) region = game_region(id);
    if (region && region[0]) kn_game_link_id(id, region, out, n);
    else snprintf(out, n, "%s", id ? id : "");
}

// The same lobby: a ?game= id (either form) and the canonical id of the lobby we are in.
static int is_current_game(const char *id) {
    return g_game_id[0] && kn_game_id_matches(id, g_game_id, game_region(g_game_id));
}

static size_t link_escape(char *out, const size_t n, const char *id) {
    size_t len = 0;
    for (const unsigned char *p = (const unsigned char *) (id ? id : ""); *p && len + 4 < n; p++) {
        if (*p <= ' ' || *p >= 0x7f || strchr("%&#+\"'<>", *p)) len += (size_t) snprintf(out + len, n - len, "%%%02X", *p);
        else out[len++] = (char) *p;
    }
    if (n) out[len < n ? len : n - 1] = 0;
    return len;
}

// The page parameters that pick the game (main.c's web query, servers_ui_query): the address
// bar rewrites them together, the others (?ws=, ?brain=, ...) stay.
static int page_game_param(const char *tok) {
    static const char *const NAMES[] = {"game", "offline", "sandbox", "test", "map", "testMap"};
    for (size_t i = 0; i < sizeof(NAMES) / sizeof(NAMES[0]); i++) {
        const size_t n = strlen(NAMES[i]);
        if (!strncmp(tok, NAMES[i], n) && (tok[n] == '=' || !tok[n])) return 1;
    }
    return 0;
}

static char g_link_offline[200]; // the offline game's query written last ("" = none)

// "./?<first>&<the page's other parameters>": the game parameters replaced by `first` ("" = none).
// "./" is the page's own directory: "/" for Krunker Box Office, "/strike/" for the Krunker Strike
// app, so a reload (F5) stays in the app it was started as.
static void page_write_query(const char *first) {
    char query[512];
    if (html_page_query(query, sizeof(query)) < 0) return; // native: no address bar
    char href[768] = "./";
    size_t len = 2;
    if (first && first[0]) len += (size_t) snprintf(href + len, sizeof(href) - len, "?%s", first);
    for (char *tok = strtok(query, "&"); tok && len < sizeof(href); tok = strtok(NULL, "&")) {
        if (!tok[0] || page_game_param(tok)) continue;
        len += (size_t) snprintf(href + len, sizeof(href) - len, "%c%s", len == 2 ? '?' : '&', tok);
    }
    html_page_set_url(href, 0);
}

// The address bar follows the lobby (ClientManager.seek replaceState; "/" when there is none,
// like devConsole's disconnect), so a reload (F5) joins the same game again: its link form
// (?game=CH2:kz862), which the startup seek resolves back to the lobby.
static void page_write_game(const char *id, const char *region) {
    game_link_id(id, region, g_link_game, sizeof(g_link_game));
    g_link_offline[0] = 0;
    id = g_link_game;
    char first[480] = "";
    if (id && id[0]) {
        char esc[460];
        link_escape(esc, sizeof(esc), id);
        snprintf(first, sizeof(first), "game=%s", esc);
    }
    page_write_query(first);
}

// The offline game's address: ?sandbox on the sandbox map, else ?offline=1&map=<catalogue key>
// (main.c reads both: --sandbox / --offline --map), so a reload (F5) plays the same map again.
// A map outside the catalogue (the editor's Test Map: ?offline=1&testMap=1, a published map)
// keeps the page's query.
static void page_write_offline(const Client *client) {
    const Game *game = &client->game;
    if (!game->ready || !game->map || !game->map->raw_data) return;
    const int index = map_catalog_index_of_json(game->map->raw_data);
    const MapCatalogEntry *entry = index >= 0 ? map_catalog_at((size_t) index) : NULL;
    if (!entry) return;
    char want[sizeof(g_link_offline)];
    if (!strcmp(entry->key, LOCAL_GAME_SANDBOX_MAP)) {
        snprintf(want, sizeof(want), "sandbox");
    } else {
        char esc[160];
        link_escape(esc, sizeof(esc), entry->key);
        snprintf(want, sizeof(want), "offline=1&map=%s", esc);
    }
    if (!strcmp(want, g_link_offline)) return;
    snprintf(g_link_offline, sizeof(g_link_offline), "%s", want);
    g_link_game[0] = 0;
    page_write_query(want);
}

static void page_game_url(const char *id, const char *region) {
    if (g_client && g_client->offline) return;
    page_write_game(id, region);
}

// Sessions without a matchmade lobby id: a direct server (console connect, Direct Connect,
// a finder row) the brain lists gets its lobby id from the game list, so the address bar and
// Invite name it; switching to the offline game drops ?game= (v3.1.1: the offline page has
// no game link). Natively console connect resolves to an IP, which the brain's hostnames do
// not match: no link there (and no address bar).
static char g_link_lookup[200];  // "host:port" looked up for the current direct session
static void on_link_list(void *user, const int status, const char *body, const size_t length) {
    (void) user;
    (void) length;
    if (status != 200 || !g_client || g_pin != PIN_DIRECT) return;
    cJSON *res = cJSON_Parse(body);
    const cJSON *t;
    cJSON_ArrayForEach(t, cJSON_GetObjectItem(res, "games")) {
        const cJSON *meta = cJSON_GetArrayItem(t, 4);
        const char *h = cJSON_GetStringValue(cJSON_GetObjectItem(meta, "host"));
        const cJSON *p = cJSON_GetObjectItem(meta, "port");
        const char *id = cJSON_GetStringValue(cJSON_GetArrayItem(t, 0));
        if (id && h && cJSON_IsNumber(p) && p->valueint == g_client->net_port && !strcasecmp(h, g_client->net_address)) {
            printf("[mm] direct server %s:%d is lobby %s\n", h, p->valueint, id);
            page_game_url(id, cJSON_GetStringValue(cJSON_GetArrayItem(t, 1)));
            break;
        }
    }
    cJSON_Delete(res);
}

static void link_tick(Client *client) {
    static int was_offline = -1;
    if (was_offline < 0) was_offline = client->offline;
    if (client->offline != was_offline) {
        was_offline = client->offline;
        if (client->offline && g_link_game[0]) page_write_game(NULL, NULL); // left for the offline game
    }
    if (client->offline) {
        page_write_offline(client); // its map, once it plays one (web only)
    } else if (g_link_offline[0]) {
        // left the offline game for a server: its lobby (if any) instead of ?offline / ?sandbox
        page_write_game(g_pin == PIN_NONE ? g_game_id : NULL, NULL);
    }
    if (g_pin != PIN_DIRECT || !client->net_connected || client->offline) {
        if (g_pin != PIN_DIRECT) g_link_lookup[0] = 0;
        return;
    }
    char key[sizeof(g_link_lookup)];
    snprintf(key, sizeof(key), "%s:%u", client->net_address, (unsigned int) client->net_port);
    if (!strcmp(key, g_link_lookup) || backend_available() != 1) return;
    snprintf(g_link_lookup, sizeof(g_link_lookup), "%s", key);
    mm_get("/mm/game-list?hostname=krunknative", on_link_list, NULL);
}

// copyInviteLink: location.protocol + "//" + location.host + "/?game=" + gameId (native: the
// public web client's origin), the id in its short link form (page_write_game). In an official
// krunker.io game the link is krunker.io's own (krunker_net_invite_link: <site>/?game=NY:bbbbb).
// Krunker Strike invites open the Strike app (/strike/?game=: platform/web/pwa.py), whose servers
// a Krunker Box Office client refuses (flavor mismatch).
void servers_ui_invite_link(char *out, const size_t n) {
    if (krunker_net_invite_link(out, n)) return;
    char origin[256], esc[480];
    html_page_origin(origin, sizeof(origin));
    link_escape(esc, sizeof(esc), g_link_game);
    snprintf(out, n, "%s%s?game=%s", origin, flavor_get() == FLAVOR_STRIKE ? "/strike/" : "/", esc);
}

// fatalError(message): the connection error screen (retry link = seek again)
static void fatal_error(const char *message) {
    g_client->net_hold_retry = 1; // a seek that failed: no transport retry, the page shows the error (final)
    on_client_socket_failed(g_client, message);
}

// A seek in flight (mm-token, doc/v10/matchmaker-token.md): GET /mm/generate-token, then
// /mm/seek-game with that ?validationToken= (Krunker's WP_MMToken / fetchValidationToken).
typedef struct SeekReq {
    char *path;   // /mm/seek-game?... without the token
    void *quiet;  // quick join: try the next candidate instead of failing
    int retried;  // a refused token was replaced once
    int play;     // v10's autoPlaySearch seek (?play=<map>): no match -> the plain seek again (on_seek)
} SeekReq;

static void seek_req_free(SeekReq *r) {
    if (!r) return;
    free(r->path);
    free(r);
}

static void seek_unreachable(void);
static void on_token(void *user, int status, const char *body, size_t length);
static void seek(const char *game_id, int auto_change, void *quiet);
void servers_quick_join_next(void);

// A reload's lobby is gone (InvalidGameId: its server stopped) or full (GameFull): one plain seek in
// the region finds another (never excludeGame: nothing to leave). The loading state stays up through
// it (v10 shows no toast while it joins a game); the log says why.
static void reload_fallback(const char *game_id, const int full) {
    char id[sizeof(g_seek_game)];
    snprintf(id, sizeof(id), "%s", game_id ? game_id : "");
    printf("[mm] reload: lobby %s is %s: seeking another lobby\n", id[0] ? id : "(none)", full ? "full" : "gone");
    if (g_client && !g_client->net_connected) menu_show_connecting_map(NULL);
    g_game_id[0] = 0;
    g_exclude_game[0] = 0;
    seek(NULL, 1, NULL);
}

static int seek_request_token(SeekReq *r) {
    return backend_http_get("/mm/generate-token", on_token, r);
}

static void start_seek(const char *game_id);
static void on_play_list(void *user, int status, const char *body, size_t length);

static void on_seek(void *user, const int status, const char *body, const size_t length) {
    (void) length;
    SeekReq *req = user;
    const int quiet = req && req->quiet != NULL;
    const int play = req && req->play;
    const int rejoin = g_rejoin_seek;
    const int find = g_find_seek, find_both = g_find_both;
    const int reload = g_reload_seek;
    g_seek_inflight = 0;
    g_rejoin_seek = 0;
    g_reload_seek = 0;
    g_find_seek = g_find_both = 0;
    if (g_pin != PIN_NONE) {
        // the user connected to / disconnected from a server by hand while this seek ran
        printf("[mm] seek answer ignored: %s\n", g_pin == PIN_DIRECT ? "pinned to a direct server" : "disconnected by the user");
        seek_req_free(req);
        return;
    }
    cJSON *res = cJSON_Parse(body);
    const char *refused = cJSON_GetStringValue(cJSON_GetObjectItem(res, "error"));
    if (status == 403 && token_refusal(refused) && req && !req->retried) {
        // a token spent or expired on the way (a slow link, a brain restart): once more with a
        // fresh one, keeping the seek's state
        printf("[mm] validation token refused (%s): seeking again with a fresh one\n", refused);
        req->retried = 1;
        g_seek_inflight = 1;
        g_rejoin_seek = rejoin;
        g_reload_seek = reload;
        g_find_seek = find;
        g_find_both = find_both;
        cJSON_Delete(res);
        if (!seek_request_token(req)) {
            seek_req_free(req);
            seek_unreachable();
        }
        return;
    }
    seek_req_free(req);
    const cJSON *host = cJSON_GetObjectItem(res, "host");
    const cJSON *port = cJSON_GetObjectItem(res, "port");
    const cJSON *answer_game = cJSON_GetObjectItem(res, "gameId");
    if (status == 200 && find && g_client->net_connected && g_find_prev_game[0] && cJSON_IsString(answer_game) &&
        !strcmp(answer_game->valuestring, g_find_prev_game)) {
        // Find New Game / Quick Match: the matchmaker had nothing but the lobby the player is in (its
        // excludeGame fallback): stay in it, no reconnect. v10 reloads the page into that same game and
        // says nothing (backToOrigin + seek, v10.js:222040-222066): no notice here either, the log says it
        snprintf(g_game_id, sizeof(g_game_id), "%s", g_find_prev_game);
        printf("[mm] find new game: no other lobby has room, staying in %s\n", g_find_prev_game);
    } else if (status == 200 && cJSON_IsString(host) && cJSON_IsNumber(port) && seek_answer_full(res)) {
        // the lobby the answer names has no seat left (the brain's count may be stale): never joined.
        // A quick join goes on to its next candidate; a seek asks for another lobby, excluding this one,
        // SEEK_FULL_SKIPS_MAX times at most, then the game is full.
        const cJSON *game = cJSON_GetObjectItem(res, "gameId");
        const char *id = cJSON_IsString(game) ? game->valuestring : "";
        printf("[mm] seek answer %s is full (%d/%d): not joining it\n", id,
               (int) cJSON_GetNumberValue(cJSON_GetObjectItem(res, "players")), (int) cJSON_GetNumberValue(cJSON_GetObjectItem(res, "maxPlayers")));
        if (quiet && g_quick_join_left > 0) {
            cJSON_Delete(res);
            servers_quick_join_next();
            return;
        }
        if (!quiet && id[0] && g_full_skips < SEEK_FULL_SKIPS_MAX) {
            g_full_skips++;
            snprintf(g_exclude_game, sizeof(g_exclude_game), "%s", id);
            g_find_seek = find; // Find New Game keeps its fallback: the player stays in its lobby when none is left
            g_find_both = find_both;
            cJSON_Delete(res);
            seek(NULL, 1, NULL);
            return;
        }
        cJSON_Delete(res);
        fatal_error(seek_error_text("GameFull"));
        return;
    } else if (status == 200 && cJSON_IsString(host) && cJSON_IsNumber(port)) {
        const cJSON *game = cJSON_GetObjectItem(res, "gameId");
        const cJSON *key = cJSON_GetObjectItem(res, "clientId");
        const cJSON *web = cJSON_GetObjectItem(res, "webUrl");
        const cJSON *region = cJSON_GetObjectItem(res, "region");
        g_full_skips = 0;
        if (cJSON_IsString(region)) snprintf(g_region, sizeof(g_region), "%s", region->valuestring);
        g_quick_join_left = 0;
        g_account_game[0] = 0;
        if (rejoin) printf("[mm] rejoin: %s with a fresh join ticket\n", cJSON_IsString(game) ? game->valuestring : "?");
        join_server(host->valuestring, port->valueint, cJSON_IsString(game) ? game->valuestring : "",
                    cJSON_IsString(key) ? key->valuestring : "", cJSON_IsString(web) ? web->valuestring : NULL,
                    cJSON_IsString(region) ? region->valuestring : NULL);
    } else if (quiet && g_quick_join_left > 0) {
        servers_quick_join_next();
    } else {
        const cJSON *err = cJSON_GetObjectItem(res, "error");
        fprintf(stderr, "[mm] seek failed (HTTP %d): %s\n", status, cJSON_IsString(err) ? err->valuestring : "no response");
        if (rejoin && (g_rejoin == REJOIN_RESTART || g_rejoin == REJOIN_DROP) && glfwGetTime() + REJOIN_RETRY_DELAY < g_rejoin_until) {
            // the restarted server (or another one of the region) is not listed yet: try again,
            // the error screen keeps the server's message ("Rejoining...")
            g_rejoin_at = glfwGetTime() + REJOIN_RETRY_DELAY;
            cJSON_Delete(res);
            return;
        }
        // gave up with no server listed at all (the restart's server never came back): the lobby is gone,
        // not full, so the text is the unreachable one (join_errors.c), not "Servers are at Max Capacity"
        const int gone = rejoin && (g_rejoin == REJOIN_RESTART || g_rejoin == REJOIN_DROP) && cJSON_IsString(err) &&
                         !strcmp(err->valuestring, "NoAvailableServers");
        if (rejoin) rejoin_cancel(); // gave up: the seek's error below
        if (reload && cJSON_IsString(err) && (!strcmp(err->valuestring, "InvalidGameId") || !strcmp(err->valuestring, "GameFull"))) {
            reload_fallback(g_seek_game, !strcmp(err->valuestring, "GameFull"));
            cJSON_Delete(res);
            return;
        }
        const int nothing = cJSON_IsString(err) && (!strcmp(err->valuestring, "NoMatchingServers") ||
                                                    !strcmp(err->valuestring, "NoAvailableServers") ||
                                                    !strcmp(err->valuestring, "NoServersMatchQuery") ||
                                                    !strcmp(err->valuestring, "NoOtherLobby"));
        if (play && cJSON_IsString(err) && (!strcmp(err->valuestring, "NoMatchingServers") || !strcmp(err->valuestring, "NoServersMatchQuery"))) {
            // no lobby plays the map (v10.js:222085 seeks again plainly and then hosts the map; this
            // client never hosts): the lobby list, then the offline game on the map (on_play_list). A
            // plain seek would join some other lobby, and the play would end there, not on the map.
            printf("[mm] play: no lobby plays the map: the offline game on it\n");
            cJSON_Delete(res);
            mm_get("/mm/game-list?hostname=krunknative", on_play_list, NULL);
            return;
        }
        if (find && (nothing || g_client->net_connected)) {
            // Find New Game / Quick Match found nothing here: the player stays in this lobby
            snprintf(g_game_id, sizeof(g_game_id), "%s", g_find_prev_game);
            if (find_both && nothing && krunker_lobbies_seek()) { // Both: an official lobby instead
                printf("[mm] find new game: no KrunkNative server matches, trying krunker.io\n");
            } else if (cJSON_IsString(err) && !strcmp(err->valuestring, "NoOtherLobby") && g_client->net_connected) {
                // KrunkNative's code for "only the lobby you leave has room": v10's matchmaker answers
                // that same game instead and v10 lands back in it without a word, so: nothing shown
                printf("[mm] find new game: NoOtherLobby, staying in %s\n", g_find_prev_game);
            } else {
                printf("[mm] find new game failed: %s\n", cJSON_IsString(err) ? err->valuestring : "no response");
                // v10's error screen, in the game too (v10.js:222076-222090: the menu with its message)
                fatal_error(seek_error_text(cJSON_IsString(err) ? err->valuestring : status ? "GameIssue" : NULL));
            }
            cJSON_Delete(res);
            return;
        }
        if (cJSON_IsString(err) && !strcmp(err->valuestring, "AccountRequired") && g_seek_game[0]) {
            snprintf(g_account_game, sizeof(g_account_game), "%s", g_seek_game); // log in -> join it
        }
        fatal_error(seek_error_text(gone ? "ServerUnreachable" : cJSON_IsString(err) ? err->valuestring : status ? "GameIssue" : NULL));
    }
    cJSON_Delete(res);
}

// ---- Krunker Strike (strike_director.h): the director allocates a game server and a ticket; when
// it cannot (unreachable, no capacity), the finder's Strike pool (?flavor=strike) is the fallback.

static void strike_seek_failed(const char *error) {
    if (g_find_seek && g_client->net_connected) { // Find New Game found nothing: stay in this lobby
        snprintf(g_game_id, sizeof(g_game_id), "%s", g_find_prev_game);
        // only this lobby has room: v10 lands back in the same game silently (no notice, the log says it);
        // a real failure keeps v10's error popup with v10's text
        if (error && !strcmp(error, "NoOtherLobby")) printf("[mm] find new game: NoOtherLobby, staying in %s\n", g_find_prev_game);
        else ui_error_popup(seek_error_text(error));
    } else {
        fatal_error(seek_error_text(error));
    }
    g_find_seek = g_find_both = 0;
}

// The fullest Strike server with room from the finder: a matchmade session without a ticket.
static void on_strike_finder(void *user, const int status, const char *body, const size_t length) {
    (void) length;
    char *error = user;
    g_seek_inflight = 0;
    cJSON *res = status == 200 ? cJSON_Parse(body) : NULL;
    const cJSON *best = NULL, *srv;
    int best_players = -1, best_rank = 0;
    cJSON_ArrayForEach(srv, cJSON_GetObjectItem(res, "servers")) {
        const int players = (int) cJSON_GetNumberValue(cJSON_GetObjectItem(srv, "players"));
        const int max = (int) cJSON_GetNumberValue(cJSON_GetObjectItem(srv, "max"));
        const char *flavor = cJSON_GetStringValue(cJSON_GetObjectItem(srv, "flavor"));
        if (!flavor || strcmp(flavor, "strike") || cJSON_IsTrue(cJSON_GetObjectItem(srv, "password")) || players >= max) continue;
        if (!cJSON_IsString(cJSON_GetObjectItem(srv, "host"))) continue;
        // F4 / Quick Match: a server this client was not in lately first (also with a finder that
        // ignores ?exclude=), then the older recent ones; the fullest among equals
        char server[RECENT_LOBBY_ID_MAX];
        snprintf(server, sizeof(server), "%s:%d", cJSON_GetStringValue(cJSON_GetObjectItem(srv, "host")),
                 (int) cJSON_GetNumberValue(cJSON_GetObjectItem(srv, "port")));
        const int rank = g_find_seek ? recent_lobbies_rank(&g_recent_servers, server) : 0;
        const int better = !best || (best_rank && (!rank || rank > best_rank)) ||
                           (rank == best_rank && players > best_players);
        if (!better) continue;
        best = srv;
        best_players = players;
        best_rank = rank;
    }
    if (g_pin != PIN_NONE) {
        printf("[strike] finder answer ignored: %s\n", g_pin == PIN_DIRECT ? "pinned to a direct server" : "disconnected by the user");
    } else if (best) {
        const char *host = cJSON_GetStringValue(cJSON_GetObjectItem(best, "host"));
        const int port = (int) cJSON_GetNumberValue(cJSON_GetObjectItem(best, "port"));
        printf("[strike] finder pool: %s:%d (%d players)\n", host, port, best_players);
        g_find_seek = g_find_both = 0;
        g_strike_code[0] = 0;
        join_server(host, port, "", NULL, NULL, cJSON_GetStringValue(cJSON_GetObjectItem(best, "region")));
    } else {
        fprintf(stderr, "[strike] finder pool: no Strike server with room (HTTP %d)\n", status);
        // Find New Game / Quick Match: the finder lists nothing it leaves, so from a lobby the answer is
        // that no other one has room (the player stays in it)
        strike_seek_failed(g_find_seek && g_find_prev_game[0] ? "NoOtherLobby" : error && error[0] ? error : "NoAvailableServers");
    }
    cJSON_Delete(res);
    free(error);
}

static int strike_finder_seek(const char *error) {
    const char *finder = online_finder_url();
    if (!finder[0]) return 0;
    char url[2048];
    const int n = snprintf(url, sizeof(url), "%s%cflavor=strike&space=1", finder, strchr(finder, '?') ? '&' : '?');
    if (g_find_seek && g_recent_servers.count && n > 0 && (size_t) n < sizeof(url)) {
        // F4 / Quick Match: the servers it leaves (this one and the recent ones), listed only when
        // nothing else has room (finder.h ?exclude=)
        char recent[RECENT_LOBBIES_MAX * RECENT_LOBBY_ID_MAX], enc[sizeof(url)];
        recent_lobbies_join(&g_recent_servers, NULL, recent, sizeof(recent));
        backend_url_encode(enc, sizeof(enc), recent);
        snprintf(url + n, sizeof(url) - (size_t) n, "&exclude=%s", enc);
    }
    char *copy = strdup(error ? error : "");
    g_seek_inflight = backend_http_request("GET", url, NULL, on_strike_finder, copy);
    if (!g_seek_inflight) free(copy);
    else printf("[strike] falling back to the finder's Strike pool: GET %s\n", url);
    return g_seek_inflight;
}

static void on_strike_allocation(void *user, const StrikeAllocation *a) {
    (void) user;
    g_seek_inflight = 0;
    g_rejoin_seek = 0;
    if (g_pin != PIN_NONE) {
        printf("[strike] allocation ignored: %s\n", g_pin == PIN_DIRECT ? "pinned to a direct server" : "disconnected by the user");
        return;
    }
    // Find New Game / Quick Match: the director takes no excludeGame, so an answer on a server or game
    // this client leaves goes to the finder's Strike pool instead, which never lists those
    if (a->ok && g_find_seek) {
        char server[RECENT_LOBBY_ID_MAX];
        snprintf(server, sizeof(server), "%s:%d", a->host, a->port);
        if (recent_lobbies_rank(&g_recent_servers, server) || (g_find_prev_game[0] && !strcmp(a->game_id, g_find_prev_game))) {
            printf("[strike] director answered the lobby this client leaves (%s): the finder's pool instead\n", a->game_id);
            if (!strike_finder_seek(NULL)) strike_seek_failed("NoOtherLobby");
            return;
        }
    }
    if (a->ok) {
        g_find_seek = g_find_both = 0;
        snprintf(g_strike_code, sizeof(g_strike_code), "%s", a->code);
        join_server(a->host, a->port, a->game_id, a->ticket, NULL, a->region);
        return;
    }
    // an outdated client cannot play anywhere this director serves: no fallback
    if (!strcmp(a->error, "GameVersionTooOld")) {
        strike_seek_failed("GAME UPDATING");
        return;
    }
    // a private code that does not exist (any more) or is full: that is the answer
    if (!strcmp(a->error, "LobbyNotFound") || !strcmp(a->error, "GameFull")) {
        strike_seek_failed(!strcmp(a->error, "GameFull") ? "GameFull" : "InvalidGameId");
        return;
    }
    if (!strike_finder_seek(a->error)) strike_seek_failed(a->status ? a->error : NULL);
}

// priv: a private lobby (code = join it, "" = open one).
static int strike_seek(const int priv, const char *code) {
    g_seek_inflight = strike_director_allocate(priv, g_region, NULL, NULL, code, on_strike_allocation, NULL);
    if (!g_seek_inflight && !strike_finder_seek(NULL)) return 0;
    return 1;
}

// ClientManager.matchmakerSeek(region, gameId, dataQuery, autoChangeGame)
// Every caller is an explicit matchmaking action (the automatic ones check g_pin first):
// it unpins.
static int g_play_seek;          // the next seek is v10's autoPlaySearch (servers_ui_play_map), taken by seek()
static char g_play_query[160];   // its dataQuery: {"i":"<map>","dsma":[<index>]}

static void seek(const char *game_id, const int auto_change_in, void *quiet) {
    char path[6144], enc[3400]; // enc: also the dataQuery with every mode and map picked, URL-encoded
    const int play = g_play_seek; // v10's play seek: region "any", the map only, autoChangeGame false
    g_play_seek = 0;
    const int auto_change = play ? 0 : auto_change_in;
    g_pin = PIN_NONE;
    // a Box Office seek replaces the error screen on the screen: an earlier official krunker.io failure
    // (krunker_net_failure_shown) must not make this seek's own error (a refused token: "Socket Error")
    // route its Find Game to krunker.io's lobbies (menu_retry_connect)
    krunker_net_clear_failure();
    if (g_client && !g_client->net_connected && !menu_connecting()) menu_show_connecting_map(NULL); // looking for the server: the spinner
    g_rejoin_seek = g_rejoin_next;
    g_rejoin_next = 0;
    g_reload_seek = g_reload_next && game_id && game_id[0]; // a reload asks for its lobby by id
    g_reload_next = 0;
    if (g_reload_seek) g_exclude_game[0] = 0; // a reload leaves no lobby: never excludeGame
    if (!g_rejoin_seek) rejoin_cancel(); // any other matchmaking action replaces a pending rejoin
    snprintf(g_seek_game, sizeof(g_seek_game), "%s", game_id ? game_id : "");
    g_account_game[0] = 0;
    if (strike_via_director()) {
        // Krunker Strike without a coordinator: director game ids are not brain lobbies; the session's own private
        // lobby (a rejoin, a reconnect) is sought again by its code, anything else is a public seek
        const int same_private = g_strike_code[0] && game_id && game_id[0] && !strcmp(game_id, g_game_id);
        (void) auto_change;
        (void) quiet;
        g_exclude_game[0] = 0;
        if (!strike_seek(same_private, same_private ? g_strike_code : NULL)) seek_unreachable();
        return;
    }
    backend_url_encode(enc, sizeof(enc), play ? "" : g_region); // regionId 'any' (v10 playMap): no region
    int n = snprintf(path, sizeof(path), "/mm/seek-game?hostname=krunknative&region=%s&autoChangeGame=%s", enc, auto_change ? "true" : "false");
    // the protocol this client speaks: the brain never matches it to a node of another major
    n += snprintf(path + n, sizeof(path) - (size_t) n, "&proto=%s", kn_build_protocol());
    // the Krunker version (v10.0.2, the only one): quick join never picks a lobby announcing another
    n += snprintf(path + n, sizeof(path) - (size_t) n, "&ruleset=%s", ruleset_current()->key);
    // the app's flavor pool: a Strike client is only ever matched into a Strike lobby
    n += snprintf(path + n, sizeof(path) - (size_t) n, "%s", mm_flavor_param(path));
    if (play) g_exclude_game[0] = 0;
    if (game_id && game_id[0]) {
        backend_url_encode(enc, sizeof(enc), game_id);
        n += snprintf(path + n, sizeof(path) - (size_t) n, "&game=%s", enc);
    } else if (g_exclude_game[0]) { // v10 matchmakerSeek(..., excludeGame, ...): any lobby but that one
        backend_url_encode(enc, sizeof(enc), g_exclude_game);
        n += snprintf(path + n, sizeof(path) - (size_t) n, "&excludeGame=%s", enc);
        // KrunkNative: the lobbies before it too (the brain tries them before excludeGame's, after
        // every other one: no bouncing between the last two; older brains ignore it)
        char recent[RECENT_LOBBIES_MAX * RECENT_LOBBY_ID_MAX], recent_enc[sizeof(recent) * 3];
        recent_lobbies_join(&g_recent_games, g_exclude_game, recent, sizeof(recent));
        if (recent[0]) {
            backend_url_encode(recent_enc, sizeof(recent_enc), recent);
            n += snprintf(path + n, sizeof(path) - (size_t) n, "&excludeRecent=%s", recent_enc);
            printf("[mm] seek: excluding %s, then the recent %s\n", g_exclude_game, recent);
        }
    }
    g_exclude_game[0] = 0;
    // v10 dataQuery: the seek_modes / seek_maps switches (Settings > Quickplay) as dsmo / dsma, the public
    // rotation while none is switched on (seek_prefs.h: never Trade or Parkour by default); a requested
    // game is never filtered (the brain ignores them for it anyway)
    char data_query[1100];
    if (play) snprintf(data_query, sizeof(data_query), "%s", g_play_query); // v10's dataQuery of playMap: the map only
    else seek_prefs_data_query(data_query, sizeof(data_query));
    if (data_query[0] && !(game_id && game_id[0])) {
        backend_url_encode(enc, sizeof(enc), data_query);
        n += snprintf(path + n, sizeof(path) - (size_t) n, "&dataQuery=%s", enc);
    }
    if (active_account() && account_session_token()[0]) {
        backend_url_encode(enc, sizeof(enc), account_session_token());
        snprintf(path + n, sizeof(path) - (size_t) n, "&token=%s", enc);
    }
    g_seek_inflight = 1;
    SeekReq *req = calloc(1, sizeof(*req));
    if (req) req->path = strdup(path);
    if (req) req->quiet = quiet;
    if (req) req->play = play;
    if (!req || !req->path || !seek_request_token(req)) {
        seek_req_free(req);
        seek_unreachable();
    }
}

// The matchmaker cannot be asked (no brain URL, out of memory): a restart rejoin tries again,
// anything else shows the error.
static void seek_unreachable(void) {
    g_seek_inflight = 0;
    g_find_seek = g_find_both = 0;
    if (g_rejoin_seek && (g_rejoin == REJOIN_RESTART || g_rejoin == REJOIN_DROP) && glfwGetTime() + REJOIN_RETRY_DELAY < g_rejoin_until) {
        g_rejoin_seek = 0;
        g_rejoin_at = glfwGetTime() + REJOIN_RETRY_DELAY; // the restart rejoin tries again
        return;
    }
    g_rejoin_seek = 0;
    rejoin_cancel();
    fatal_error(I18N("kn.matchmaker.unreachable", "Could not reach the matchmaker"));
}

// /mm/generate-token answered: plain text (Krunker's matchmaker too). A brain from before the
// token (404), one without a key (503) or a rate limit (429): the seek goes without one, which
// such a brain accepts (a brain requiring tokens refuses it with the error shown). A transport
// error: the seek itself reports the unreachable matchmaker the same way.
static void on_token(void *user, const int status, const char *body, const size_t length) {
    SeekReq *req = user;
    char *path = req->path;
    char *full = NULL;
    if (status == 200 && body && length > 0 && length < 1024) {
        char token[1024];
        size_t len = length;
        while (len && (body[len - 1] == '\n' || body[len - 1] == '\r' || body[len - 1] == ' ')) len--; // .trim()
        snprintf(token, sizeof(token), "%.*s", (int) len, body);
        char enc[3 * 1024];
        backend_url_encode(enc, sizeof(enc), token);
        const size_t cap = strlen(path) + strlen(enc) + 32;
        full = malloc(cap);
        if (full) snprintf(full, cap, "%s&validationToken=%s", path, enc);
        printf("[mm] validation token (%zu chars)%s\n", len, req->retried ? ", fresh after a refusal" : "");
    } else {
        printf("[mm] no validation token (generate-token HTTP %d): seeking without one\n", status);
    }
    if (!backend_http_get(full ? full : path, on_seek, req)) {
        seek_req_free(req);
        seek_unreachable();
    }
    free(full);
}

// ClientManager.getRegion() (v10.js:147469-147507) over the brain's /mm/ping-list: Settings > Default
// Region when the brain lists it, else the region cached by an earlier ping race (pingRegion7, v10's
// PING_REGION_CACHE_KEY) when still listed, else the race (region_ping.h: every listed region's /ping
// at once, the first to answer wins and is cached, setDefaultRegion). Every ping failing: the list's
// first region (v10's seek would stop at "All pings failed."), cached in pingRegion7 for the region
// pickers but not as a ping result, so the next seek races again. KrunkNative builds before the race
// wrote the list's first region to pingRegion7 unpinged: only a value knPingRegionPinged confirms counts.
#define PINGED_KEY "knPingRegionPinged"
static int g_region_racing;      // the ping race runs: a second seek waits for it (start_seek)

static int region_listed(const char *region) {
    return region && region[0] && cJSON_GetObjectItemCaseSensitive(g_ping_regions, region) != NULL;
}

const char *servers_ui_auto_region(void) {
    static char cached[32];
    char pinged[32];
    if (html_storage_get("pingRegion7", cached, sizeof(cached)) < 0 || html_storage_get(PINGED_KEY, pinged, sizeof(pinged)) < 0 ||
        strcmp(cached, pinged))
        cached[0] = 0;
    return cached;
}

static void seek_in_region(const char *requested_game) {
    if (!g_region[0]) snprintf(g_region, sizeof(g_region), "%s", "local");
    if (g_pin != PIN_NONE) {
        printf("[mm] seek skipped: %s\n", g_pin == PIN_DIRECT ? "pinned to a direct server" : "disconnected by the user");
        g_exclude_game[0] = 0; // the Quick Match this skipped must not leak into a later seek
        g_reload_next = 0;     // nor the reload's mark
    }
    // v3.1.1's startup seek sends autoChangeGame: false - a ?game= that is gone, full or needs
    // an account shows its error (Game not found. / ...) with the "seek a new game" link
    // instead of quietly landing somewhere else
    else seek(requested_game, !requested_game, NULL);
}

static void on_region_race(void *user, const char *region) {
    char *requested_game = user;
    g_region_racing = 0;
    if (region && region_listed(region)) {
        snprintf(g_region, sizeof(g_region), "%s", region);
        html_storage_set("pingRegion7", g_region); // ClientManager.setDefaultRegion
        html_storage_set(PINGED_KEY, g_region);
        printf("[mm] seek region: %s, the first to answer the ping race\n", g_region);
    } else if (g_ping_regions && g_ping_regions->child && g_ping_regions->child->string) {
        snprintf(g_region, sizeof(g_region), "%s", g_ping_regions->child->string);
        html_storage_set("pingRegion7", g_region);
        printf("[mm] seek region: every ping failed: %s, the list's first\n", g_region);
    }
    seek_in_region(requested_game);
    free(requested_game);
}

static void on_ping_list(void *user, const int status, const char *body, const size_t length) {
    (void) length;
    char *requested_game = user;
    cJSON *list = status == 200 ? cJSON_Parse(body) : NULL;
    char cached[32] = "", saved[32] = "";
    if (html_storage_get("pingRegion7", cached, sizeof(cached)) < 0) cached[0] = 0;
    char pinged[32] = "";
    if (html_storage_get(PINGED_KEY, pinged, sizeof(pinged)) < 0 || strcmp(cached, pinged)) cached[0] = 0; // a race confirmed it
    const cJSON *setting = ui_setting_saved("defaultRegion");
    if (cJSON_IsString(setting) && setting->valuestring[0]) snprintf(saved, sizeof(saved), "%s", setting->valuestring);
    // a saved Default Region the ping list does not name is not an error: the seek goes on in an
    // available region (seek_region_choose: the first listed one, then the ping race below picks the
    // lowest measured one), logged here only
    const int have_list = cJSON_IsObject(list);
    const SeekRegionChoice how = seek_region_choose(have_list ? list : NULL, saved, cached, g_region, sizeof(g_region));
    if (how == SEEK_REGION_UNLISTED) printf("[mm] default region %s is not in the matchmaker's list: seeking in %s instead\n", saved, g_region);
    if (have_list) {
        cJSON_Delete(g_ping_regions);
        g_ping_regions = list; // the region pickers' options
        list = NULL;
    }
    cJSON_Delete(list);
    // ClientManager.setDefaultRegion: an unlisted Default Region's fallback is not a ping result, so it is not cached
    if (how != SEEK_REGION_UNLISTED) html_storage_set("pingRegion7", g_region);
    // v10 getRegion: no Default Region and no confirmed cache (the list's first region, or none): the ping
    // race decides, and seeks in its winner (on_region_race). An unlisted Default Region races the same way
    if ((how == SEEK_REGION_FIRST || how == SEEK_REGION_NONE || how == SEEK_REGION_UNLISTED) && have_list &&
        region_ping_race(g_ping_regions, NULL, on_region_race, requested_game)) {
        g_region_racing = 1;
        return;
    }
    seek_in_region(requested_game);
    free(requested_game);
}

static void start_seek(const char *game_id);

// Auto Seek on Launch (seek_prefs.h): the launch's one seek, the one F4 runs for the same Quickplay
// settings. Official (or Both without a matchmaker) is krunker.io's list; ours is the startup seek
// (start_seek: its dataQuery carries the Seek Mode / Seek Map). Both = ours when one matches, else
// an official lobby (on_seek's Find New Game fallback, with no lobby to stay in).
static int launch_seek(void) {
    const SeekServers type = seek_prefs_servers();
    printf("[mm] launch seek: Quickplay %s\n", type == SEEK_SERVERS_OFFICIAL ? "official krunker.io" : type == SEEK_SERVERS_BOTH ? "both" : "KrunkNative");
    if (type == SEEK_SERVERS_OFFICIAL || (type == SEEK_SERVERS_BOTH && !g_brain_mode)) return krunker_lobbies_seek();
    if (!g_brain_mode) return 0;
    if (type == SEEK_SERVERS_BOTH) {
        g_find_seek = g_find_both = 1;
        g_find_prev_game[0] = 0;
    }
    start_seek(NULL);
    return 1;
}

// The menu is up (its page and setup ran): the launch seek waits for it (seek_prefs_on_launch)
static int menu_ready_for_launch(void) {
    return !html_ui_available() || (html_ui_is_ready() && menu_ui()->dom_ready);
}

// The menu's region label for an official krunker.io game: the region's name as the official list
// gives it ("New York", krunker_region_name), the game asked for when the list does not name it.
// Left when the session ends (v10's setGameInstanceId shows the brain's region the same way).
static char g_official_label[160];
static void official_label_tick(void) {
    const char *game = krunker_net_session_game();
    if (!game[0]) {
        if (g_official_label[0]) {
            g_official_label[0] = 0;
            menu_set_region_label("");
        }
        return;
    }
    if (!strcmp(game, g_official_label)) return;
    snprintf(g_official_label, sizeof(g_official_label), "%s", game);
    const cJSON *t = krunker_lobbies_find(game);
    const char *region = t ? cJSON_GetStringValue(cJSON_GetArrayItem(t, 1)) : NULL;
    menu_set_region_label(krunker_region_name(region && region[0] ? region : krunker_net_region()));
}

static void start_seek(const char *game_id) {
    if (g_seek_inflight || g_region_racing) return;
    g_full_skips = 0;
    if (strike_director_enabled() && !g_brain_mode) { // Krunker Strike without a brain: the director only
        seek(game_id, !game_id, NULL);
        return;
    }
    char *copy = game_id && game_id[0] ? strdup(game_id) : NULL;
    if (!mm_get("/mm/ping-list", on_ping_list, copy)) {
        free(copy);
        g_find_seek = g_find_both = 0;
        g_reload_next = 0;
        fatal_error(I18N("kn.matchmaker.unreachable", "Could not reach the matchmaker"));
    }
}

// switchServer(id): the web reloads the page on ?game=id (v3.1.1 generateHref); the
// native client seeks that game and reconnects in place.
static void switch_server(const char *id) {
    if (!id || !id[0]) return;
#ifdef __EMSCRIPTEN__
    // the parameters that pick an offline game go (page_game_param); the id stays readable in
    // its short link form (generateHref: "?game=CH2:kz862", not %3A); the reload's seek
    // resolves it (a short id or one the list does not name goes as it is)
    char link[160], esc[480];
    game_link_id(id, NULL, link, sizeof(link));
    link_escape(esc, sizeof(esc), link);
    EM_ASM({
        var p = new URLSearchParams(location.search);
        p.delete("game");
        p.delete("offline");
        p.delete("sandbox");
        p.delete("test");
        p.delete("map");
        p.delete("testMap");
        var rest = p.toString();
        location.search = "?game=" + UTF8ToString($0) + (rest ? "&" + rest : "");
    }, esc);
#else
    seek(id, 0, NULL);
#endif
}

// v3.1.1 f9(text) - the Join window (joinGame: gameURL.value) and openJoinWindow's clipboard:
// a game link "[http(s)://][www.]<site>/?game=<id>" (any of our sites; %3A or a bare ':')
// or a bare lobby id "NODE:lobby" (either form: CH2:kz862 or CHITOWN2:kz862) switches to that
// game; anything else (re)opens the Join window (showWindow(24)). Party links are not ported
// (no parties): the window again. Parsing: shared/src/server_names.c kn_game_link_parse.
// Krunker Strike: a private lobby code (4-12 letters / digits, the director's allocate/private
// customMatchId). 1 = it looked like one and the seek started.
static int strike_join_code(const char *text) {
    char code[16];
    size_t n = 0;
    while (*text == ' ') text++;
    for (; *text && *text != ' ' && n + 1 < sizeof(code); text++) {
        if (!isalnum((unsigned char) *text)) return 0;
        code[n++] = (char) toupper((unsigned char) *text);
    }
    code[n] = 0;
    while (*text == ' ') text++;
    if (*text || n < 4 || n > 12 || !strike_via_director() || g_seek_inflight) return 0;
    printf("[strike] joining private lobby %s\n", code);
    menu_show_window(MENU_WINDOW_NONE, 0);
    g_pin = PIN_NONE;
    if (!strike_seek(1, code)) seek_unreachable();
    return 1;
}

// An official krunker.io lobby (a krunker.io game link): joined in this client (krunker_net),
// or opened in the browser with the opt-out (krunker_lobbies_open). The web build has no
// official join. 1 = started, -1 = refused (err says why).
static int join_official(const char *id, char *err, const size_t n) {
    for (const char *p = id; *p; p++) {
        if (!isalnum((unsigned char) *p) && *p != ':' && *p != '-' && *p != '_') {
            snprintf(err, n, "%s", I18N("kn.connect.badAddress", "Not a game link or server address: paste a krunker.io or Box Office game link, or enter host:port"));
            return -1;
        }
    }
#ifdef __EMSCRIPTEN__
    (void) id;
    snprintf(err, n, "%s", I18N("kn.connect.officialWeb", "Official krunker.io lobbies join in the desktop client, not in the browser build"));
    return -1;
#else
    printf("[mm] connect: official krunker.io lobby %s\n", id);
    krunker_lobbies_open(id); // in this client, or the browser (its own message)
    return 1;
#endif
}

// The game-link forms of a typed address: the Connect window, the console `connect`, the Join
// field. A krunker.io link joins the official lobby; a KrunkNative link or a lobby id (CH2:kz862)
// seeks it through the matchmaker, which a client without one cannot do (-1 with err). 1 =
// joining, 0 = not a game link (err untouched: the caller reads it as host[:port]), -1 = refused.
int servers_ui_connect_game(const char *text, char *err, const size_t n) {
    char id[160];
    if (!kn_game_link_parse(text, id, sizeof(id))) return 0;
    if (kn_game_link_is_krunker(text)) return join_official(id, err, n);
    if (!g_brain_mode || backend_available() != 1) {
        snprintf(err, n, "%s", I18N("kn.connect.noMatchmaker", "This game link needs the Box Office matchmaker, which is not reachable: enter host:port instead"));
        return -1;
    }
    printf("[mm] connect: game %s\n", id);
    menu_show_window(MENU_WINDOW_NONE, 0);
    switch_server(id); // web: location -> ?game=<id> (f9: location.href = link); native: seek it
    return 1;
}

// The Join field / a pasted link (v3.1.1 f9, v10 _72569): a game link or a lobby id joins that
// game (servers_ui_connect_game); anything else, or a link that cannot join, is an error. The error
// is shown as the error popup and the Join window stays as it is. 1 = joining.
int servers_ui_join_link(const char *text) {
    if (text && strike_join_code(text)) return 1;
    char err[200] = "";
    const int game = servers_ui_connect_game(text, err, sizeof(err));
    if (game > 0) return 1;
    if (!game) snprintf(err, sizeof(err), "%s", I18N("kn.join.notLink", "Not a game link: paste a krunker.io or Box Office game link, or a lobby code such as CH2:kz862"));
    printf("[mm] join refused: %s\n", err);
    ui_error_popup(err); // the error popup, not a toast: the Join window stays as it is
    return 0;
}

// v10 switchServer(friend.gameInstanceId) (the social panel's "Join <name>'s match", the profile
// window's Friends tab): that game through the matchmaker, like a pasted code. 0 = no matchmaker.
int servers_ui_join_game(const char *id) {
    if (!id || !id[0] || !g_brain_mode || backend_available() != 1) return 0;
    printf("[mm] join game %s\n", id);
    menu_show_window(MENU_WINDOW_NONE, 0);
    switch_server(id);
    return 1;
}

int servers_ui_is_current_game(const char *game_id) { return game_id && game_id[0] && is_current_game(game_id); }

int servers_ui_private_match(void) {
    if (!strike_director_enabled() || g_seek_inflight) return 0;
    printf("[strike] opening a private lobby\n");
    menu_show_window(MENU_WINDOW_NONE, 0);
    g_pin = PIN_NONE;
    if (!strike_seek(1, NULL)) seek_unreachable();
    return 1;
}

int servers_ui_invite_game(char *out, const size_t n) {
    if (!out || !n) return 0;
    snprintf(out, n, "%s", g_strike_code[0] ? g_strike_code : g_game_id);
    return out[0] != 0 && g_client && g_client->net_connected;
}

// v10 submitGameCode(): the server refused the join for its password (ERROR 004005,
// v10_party_password_prompt); the same game again with the password in the JOIN packet. A
// matchmade lobby is sought again (a fresh ticket, no page reload on the web), a direct server
// reconnected.
int servers_ui_join_password(const char *password) {
    if (!g_client || !password) return 0;
    // a Strike director game is sought again too, for a fresh join ticket (the refused join's may not be good again)
    const int lobby = g_pin == PIN_NONE && g_game_id[0] &&
                      (strike_via_director() || (g_brain_mode && backend_available() == 1));
    pthread_mutex_lock(&g_client->net_lock);
    snprintf(g_client->net_join.password, sizeof(g_client->net_join.password), "%s", password);
    if (lobby) snprintf(g_password_game, sizeof(g_password_game), "%s", g_game_id);
    else snprintf(g_password_game, sizeof(g_password_game), "%s:%u", g_client->net_address, (unsigned int) g_client->net_port);
    pthread_mutex_unlock(&g_client->net_lock);
    printf("[mm] joining %s again with the server password\n", g_password_game);
    if (lobby) seek(g_game_id, 0, NULL);
    else client_net_switch_server(g_client);
    return 1;
}

// setGameInstanceId: fetchGameInfo(id) -> menuRegionLabel = regionNames[regionId]; KrunkNative
// labels the server itself ("Chicago 2", server_display_name)
static void on_game_info(void *user, const int status, const char *body, const size_t length) {
    (void) user;
    (void) length;
    cJSON *tuple = status == 200 ? cJSON_Parse(body) : NULL;
    const cJSON *region = cJSON_GetArrayItem(tuple, 1);
    if (cJSON_IsString(region)) {
        char name[128];
        snprintf(g_region, sizeof(g_region), "%s", region->valuestring);
        server_display_name(cJSON_GetStringValue(cJSON_GetArrayItem(tuple, 0)), region->valuestring, name, sizeof(name));
        menu_set_region_label(name);
    }
    cJSON_Delete(tuple);
}

// ------------------------------------------------------------------ windows[1]

// The server finder (online_mode.h: GET <finder>?..., services/brain/finder.c) lists dedicated
// servers it verified with the KNINFO query. Its entries join the brain's lobbies in the same
// region groups (one list, v3.1.1's markup): a finder entry for a host:port the brain already
// lists is the same server and is dropped; the rest become game-list style tuples
//   ["host:port", region, players, max, {i: map, n: name, host, port, finder: true}]
// Their row joins the host directly (finderJoin): no lobby, no ticket.
static cJSON *g_finder_servers;   // GET <finder> "servers" (owned)
static cJSON *g_finder_rows;      // the finder-only entries as tuples (owned, rebuilt by build_groups)
static double g_finder_players;   // players on finder-only servers (the "In Game" total)
static int g_finder_loading;
static double g_finder_loaded_ms = -1e18;
static char g_finder_error[256];  // the last finder request's failure ("" = ok), shown in the browser

// ---- v7.1.4 windows[1].getServers / searchMatches (src/index.js), over KrunkNative's
// tuples [id, region, players, max, meta, timeLeft?]. KrunkNative lobby meta (server_bin
// accounts.c): i (map, "alias_map" for customs), cs (custom), pr (password), earnKR, mode,
// map, name, host, port, timeLeft; finder rows (rebuild_finder_rows): finder, n. Like
// Krunker's list (v10 row badges: OFFICIAL, 24/7, 2XP, KR, star, lock) no row is marked
// account-only: every server admits guests and accounts alike.
// The v7 fields they stand for: c = cs, pw = pr (lock icon), kr = earnKR, g = mode,
// ds (24/7 dedicated) = a finder-listed server, h (the row's name) = its server name,
// tuple[5] (seconds left) = meta.timeLeft. KrunkNative's own lobbies have no official customs (oc)
// and no priority servers (pr star); krunker.io's official customs go to their own kio:official group.

static int str_has_ci(const char *hay, const char *needle) {
    if (!needle || !needle[0]) return 1;
    if (!hay) return 0;
    const size_t n = strlen(needle);
    for (const char *p = hay; *p; p++) {
        if (!strncasecmp(p, needle, n)) return 1;
    }
    return 0;
}

static const cJSON *meta_of(const cJSON *t) {
    const cJSON *m = cJSON_GetArrayItem(t, 4);
    return cJSON_IsObject(m) ? m : NULL;
}

static int meta_flag(const cJSON *t, const char *key) {
    const cJSON *v = cJSON_GetObjectItem(meta_of(t), key);
    return cJSON_IsTrue(v) || (cJSON_IsNumber(v) && v->valuedouble != 0);
}

static const char *meta_str(const cJSON *t, const char *key) {
    const char *v = cJSON_GetStringValue(cJSON_GetObjectItem(meta_of(t), key));
    return v ? v : "";
}

static int tuple_int(const cJSON *t, const int i) {
    const cJSON *v = cJSON_GetArrayItem(t, i);
    return cJSON_IsNumber(v) ? v->valueint : 0;
}

static const char *tuple_str(const cJSON *t, const int i) {
    const char *v = cJSON_GetStringValue(cJSON_GetArrayItem(t, i));
    return v ? v : "";
}

static int row_custom(const cJSON *t) { return meta_flag(t, "cs") || meta_flag(t, "c"); }
static int row_official(const cJSON *t) { return meta_flag(t, "oc"); }
static int row_dedicated(const cJSON *t) { return meta_flag(t, "ds") || meta_flag(t, "finder"); }
static int row_players(const cJSON *t) { const int p = tuple_int(t, 2), m = tuple_int(t, 3); return p < m ? p : m; }

// v7 h: the row's own name (custom game / dedicated server); "" = none
static const char *row_h(const cJSON *t) {
    if (meta_flag(t, "finder")) return meta_str(t, "n");
    return meta_str(t, "h");
}

// v7 i: the map; KrunkNative customs carry "alias_map" in i and the map in `map`
static const char *row_i(const cJSON *t) {
    if (row_custom(t) && meta_str(t, "map")[0]) return meta_str(t, "map");
    return meta_str(t, "i");
}

// gameModes[meta.g].alias / name
static const GameModeConfig *row_mode(const cJSON *t) {
    const cJSON *m = cJSON_GetObjectItem(meta_of(t), "mode");
    if (!cJSON_IsNumber(m)) m = cJSON_GetObjectItem(meta_of(t), "g");
    return cJSON_IsNumber(m) ? mode_config(m->valueint) : NULL;
}

static const char *row_alias(const cJSON *t) {
    const GameModeConfig *mode = row_mode(t);
    return mode && mode->alias ? mode->alias : "";
}

static int old_browser(void) { return !ui_setting_is_false("oldBrowser"); }      // SETTINGS.tmp.oldBrowser (default true)
static int show_region(void) { return !ui_setting_is_false("showRegion"); }      // showRegion (default true)
static int show_timer(void) { return !ui_setting_is_false("showServerTimer"); }  // showServerTimer (default true)
static int default_region_only(void) {                                          // defaultRegionOnly (default false)
    const cJSON *v = ui_setting_value("defaultRegionOnly");
    return cJSON_IsTrue(v) || (cJSON_IsString(v) && !strcmp(v->valuestring, "true"));
}

// SETTINGS.list.defaultRegion.val (else the region the client seeks in)
static void default_region(char *out, const size_t n) {
    const cJSON *setting = ui_setting_saved("defaultRegion");
    if (cJSON_IsString(setting) && setting->valuestring[0]) snprintf(out, n, "%s", setting->valuestring);
    else if (html_storage_get("pingRegion7", out, n) < 0) out[0] = 0;
}

// v7 searchMatches(tuple) against the search text (autoPlaySearch || serverSearch)
static int search_matches(const cJSON *t) {
    if (!meta_of(t)) return 0;
    if (!g_search[0]) return 1;
    const char *alias = row_alias(t);
    const char *h = row_h(t)[0] ? row_h(t) : row_i(t);
    char label[192], label2[200], node[96], short_node[KN_SHORT_NODE_MAX], proper[128];
    if (!row_official(t) && !row_custom(t) && !old_browser() && alias[0]) snprintf(label, sizeof(label), "%s_%s", alias, h);
    else snprintf(label, sizeof(label), "%s", h);
    if (row_custom(t) || old_browser()) snprintf(label2, sizeof(label2), "%s_%s", alias, label);
    else snprintf(label2, sizeof(label2), "%s", label);
    snprintf(node, sizeof(node), "%s", tuple_str(t, 0));
    char *colon = strchr(node, ':');
    if (colon) *colon = 0;
    // KrunkNative: a server is also found by its short id (CH2) and its proper name (Chicago 2)
    kn_node_short_id(node, tuple_str(t, 1), short_node, sizeof(short_node));
    server_display_name(node, tuple_str(t, 1), proper, sizeof(proper));
    return str_has_ci(h, g_search) || str_has_ci(node, g_search) || str_has_ci(label, g_search) || str_has_ci(alias, g_search) ||
           str_has_ci(label2, g_search) || str_has_ci(tuple_str(t, 1), g_search) || str_has_ci(region_full_name(tuple_str(t, 1)), g_search) ||
           str_has_ci(short_node, g_search) || str_has_ci(proper, g_search) ||
           (meta_flag(t, "krunker") && (str_has_ci("krunker.io", g_search) || str_has_ci(krunker_region_name(tuple_str(t, 1)), g_search)));
}

// mm-token: an official krunker.io lobby (krunker_lobbies.c), joined through krunker_net (krunkerJoin)
static int row_krunker(const cJSON *t) { return meta_flag(t, "krunker"); }

static const cJSON *brain_games(void); // the brain's lobbies while it answers (below)
// The lists the current tab shows, its own backend only: krunk.io = our brain lobbies and the finder
// servers, krunker.io = its official lobbies (NULL until listed). Returns how many of `lists` are set.
static int tab_lists(const cJSON *lists[2]) {
    if (g_server_tab == SERVER_TAB_KRUNKER) {
        lists[0] = krunker_lobbies_list();
        lists[1] = NULL;
        return 1;
    }
    lists[0] = brain_games();
    lists[1] = g_finder_rows;
    return 2;
}

// The default tab is the active backend's (krunker_net_active): it follows the backend when that changes,
// and stays where the player put it while it does not.
static void server_tab_follow(void) {
    const int backend = krunker_net_active() ? 1 : 0;
    if (backend == g_server_tab_backend) return;
    g_server_tab_backend = backend;
    g_server_tab = backend ? SERVER_TAB_KRUNKER : SERVER_TAB_KRUNK;
}

typedef struct {
    char key[32];       // "official", "custom", a region id or a mode alias
    char name[64];
    int order;
    int region_rank;    // the default region's group first (regionPriority)
    double players;
    const cJSON *games[256];
    int game_count;
} ServerGroup;
#define SERVER_GROUPS_MAX 48 // ours + the official krunker.io regions (mm-token)

// A fixed game list instead of the brain's (servers_ui_set_games: --render-scenario
// `state.servers`, KRUNKNATIVE_SERVER_LIST=<file.json> {"games": [...], "totalPlayerCount": n}).
static int g_games_override;

// The brain lobbies the list shows (only while the brain answers).
static const cJSON *brain_games(void) { return backend_available() == 1 || g_games_override ? g_games : NULL; }

void servers_ui_set_games(const cJSON *list) {
    cJSON_Delete(g_games);
    g_games = NULL;
    g_games_override = list != NULL;
    if (!list) return;
    const cJSON *games = cJSON_IsArray(list) ? list : cJSON_GetObjectItem(list, "games");
    g_games = cJSON_Duplicate(games, 1);
    const cJSON *total = cJSON_GetObjectItem(list, "totalPlayerCount");
    g_total_players = cJSON_IsNumber(total) ? total->valuedouble : 0;
    if (!cJSON_IsNumber(total)) {
        const cJSON *t;
        cJSON_ArrayForEach(t, g_games) g_total_players += tuple_int(t, 2);
    }
    g_list_start_ms = now_ms();
    g_loaded = 1;
}

// A brain lobby runs on host:port (the game-list meta carries both).
static int brain_lists(const char *host, const int port) {
    const cJSON *t;
    cJSON_ArrayForEach(t, brain_games()) {
        const cJSON *meta = cJSON_GetArrayItem(t, 4);
        const char *h = cJSON_GetStringValue(cJSON_GetObjectItem(meta, "host"));
        const cJSON *p = cJSON_GetObjectItem(meta, "port");
        if (h && cJSON_IsNumber(p) && p->valueint == port && !strcasecmp(h, host)) return 1;
    }
    return 0;
}

// g_finder_servers minus the brain's own servers, as tuples (g_finder_rows).
static int finder_off(void); // a scenario render lists only its own servers (defined with the finder)

static void rebuild_finder_rows(void) {
    cJSON_Delete(g_finder_rows);
    g_finder_rows = cJSON_CreateArray();
    g_finder_players = 0;
    const cJSON *e, *servers = finder_off() ? NULL : g_finder_servers;
    cJSON_ArrayForEach(e, servers) {
        const char *host = cJSON_GetStringValue(cJSON_GetObjectItem(e, "host"));
        const cJSON *port = cJSON_GetObjectItem(e, "port");
        if (!host || !host[0] || !cJSON_IsNumber(port)) continue;
        if (brain_lists(host, port->valueint)) continue;  // the same server as a brain lobby
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(e, "name"));
        const char *map = cJSON_GetStringValue(cJSON_GetObjectItem(e, "map"));
        const char *region = cJSON_GetStringValue(cJSON_GetObjectItem(e, "region"));
        const int players = (int) cJSON_GetNumberValue(cJSON_GetObjectItem(e, "players"));
        const int max = (int) cJSON_GetNumberValue(cJSON_GetObjectItem(e, "max"));
        char id[192];
        snprintf(id, sizeof(id), "%s:%d", host, port->valueint);
        cJSON *t = cJSON_CreateArray();
        cJSON_AddItemToArray(t, cJSON_CreateString(id));
        cJSON_AddItemToArray(t, cJSON_CreateString(region ? region : "")); // "" = unknown: its own "?" group
        cJSON_AddItemToArray(t, cJSON_CreateNumber(players > 0 ? players : 0));
        cJSON_AddItemToArray(t, cJSON_CreateNumber(max > 0 ? max : 0));
        cJSON *meta = cJSON_CreateObject();
        cJSON_AddStringToObject(meta, "i", map && map[0] ? map : name && name[0] ? name : host);
        cJSON_AddStringToObject(meta, "n", name && name[0] ? name : host);
        cJSON_AddStringToObject(meta, "host", host);
        cJSON_AddNumberToObject(meta, "port", port->valueint);
        const cJSON *mode = cJSON_GetObjectItem(e, "mode");
        if (cJSON_IsNumber(mode)) cJSON_AddNumberToObject(meta, "mode", mode->valuedouble);
        cJSON_AddTrueToObject(meta, "finder");
        // build version flags for the row badge (servers_ui_entry_badge_html)
        cJSON_AddBoolToObject(meta, "outdated", servers_ui_entry_outdated(e));
        cJSON_AddBoolToObject(meta, "incompatible", servers_ui_entry_incompatible(e));
        const char *version = cJSON_GetStringValue(cJSON_GetObjectItem(e, "version"));
        const char *protocol = cJSON_GetStringValue(cJSON_GetObjectItem(e, "protocol"));
        if (version) cJSON_AddStringToObject(meta, "version", version);
        if (protocol) cJSON_AddStringToObject(meta, "protocol", protocol);
        cJSON_AddItemToArray(t, meta);
        cJSON_AddItemToArray(g_finder_rows, t);
        g_finder_players += players > 0 ? players : 0;
    }
}

static int find_group(ServerGroup *groups, const int count, const char *key) {
    for (int i = 0; i < count; i++) {
        if (!strcmp(groups[i].key, key)) return i;
    }
    return -1;
}

static char g_sort_region[32];
static int cmp_games_old(const void *a, const void *b) {
    const cJSON *x = *(const cJSON *const *) a, *y = *(const cJSON *const *) b;
    return row_players(y) - row_players(x); // (pr ? 1 : 0) first: no priority servers in KrunkNative
}
// Custom Games / Official Customs with regionPriority (always on): default region first
static int cmp_games_region(const void *a, const void *b) {
    const cJSON *x = *(const cJSON *const *) a, *y = *(const cJSON *const *) b;
    const int dx = !strcmp(tuple_str(x, 1), g_sort_region), dy = !strcmp(tuple_str(y, 1), g_sort_region);
    return dy - dx;
}
static int cmp_groups_order(const void *a, const void *b) {
    const ServerGroup *x = a, *y = b;
    const int d = (y->order ? y->order : -1) - (x->order ? x->order : -1);
    return d ? d : x->region_rank - y->region_rank; // qsort is not stable: insertion order breaks ties
}
// krunker.io's own groups: its Official Customs and Custom Games (v10's top two), then its regions
static int kio_rank(const char *key) {
    return !strcmp(key, "kio:official") ? 0 : !strcmp(key, "kio:custom") ? 1 : 2;
}
// Stable sort (v8's Array.prototype.sort is stable): insertion sort, the lists are short.
static void stable_sort(const cJSON **games, const int n, int (*cmp)(const void *, const void *)) {
    for (int i = 1; i < n; i++) {
        const cJSON *v = games[i];
        int j = i - 1;
        while (j >= 0 && cmp(&games[j], &v) > 0) {
            games[j + 1] = games[j];
            j--;
        }
        games[j + 1] = v;
    }
}

static void group_push(ServerGroup *g, const cJSON *t) {
    if (!g_search[0] || search_matches(t)) g->players += row_players(t);
    if (g->game_count < 256) g->games[g->game_count++] = t;
}

// The group of a krunker.io row (mm-token): kio:official / kio:custom for its official customs and
// custom games, named as krunker.io names them, else kio:<region> (one group per Krunker region, v10's place
// name). The krunker.io tab lists krunker.io's rows only (tab_lists), so these groups hold nothing else.
// The top two and the default region are order 1 (cmp_groups_order: the layout of v10's getServers).
// Returns the group index, -1 = none.
static int kio_group(ServerGroup *groups, int *count, const int max, const cJSON *t) {
    char key[32], name[64], def[32] = "";
    if (row_official(t) || row_custom(t)) {
        snprintf(key, sizeof(key), "%s", row_official(t) ? "kio:official" : "kio:custom");
        snprintf(name, sizeof(name), "%s", row_official(t) ? "Official Customs" : "Custom Games");
    } else {
        snprintf(key, sizeof(key), "kio:%s", tuple_str(t, 1));
        snprintf(name, sizeof(name), "%s", krunker_region_name(tuple_str(t, 1)));
    }
    int gi = find_group(groups, *count, key);
    if (gi < 0 && *count < max) {
        ServerGroup *g = &groups[*count];
        snprintf(g->key, sizeof(g->key), "%s", key);
        snprintf(g->name, sizeof(g->name), "%s", name);
        g->region_rank = 1000 + (kio_rank(key) < 2 ? kio_rank(key) : 2 + *count);
        default_region(def, sizeof(def));
        if (!strcmp(key, "kio:official") || !strcmp(key, "kio:custom") || !strcmp(tuple_str(t, 1), def)) g->order = 1;
        gi = (*count)++;
    }
    return gi;
}

// getServers(): the groups of the current tab, v10's layout (getServers' oldBrowser branch): Official Customs,
// Custom Games, then one group per region, the default region first. The lists are the tab's (tab_lists);
// empty groups are dropped. Returns the group count.
static int build_groups(ServerGroup *groups, const int max) {
    char def[32] = "";
    default_region(def, sizeof(def));
    snprintf(g_sort_region, sizeof(g_sort_region), "%s", def);
    const int only_default = default_region_only();
    memset(groups, 0, sizeof(*groups) * (size_t) max);
    int count = 0;
    rebuild_finder_rows();
    const cJSON *lists[2];
    const int nlists = tab_lists(lists);
    snprintf(groups[0].key, sizeof(groups[0].key), "official");
    snprintf(groups[0].name, sizeof(groups[0].name), "Official Customs");
    groups[0].order = 1;
    snprintf(groups[1].key, sizeof(groups[1].key), "custom");
    snprintf(groups[1].name, sizeof(groups[1].name), "Custom Games");
    groups[1].order = 1;
    groups[1].region_rank = 1;
    count = 2;
    for (int l = 0; l < nlists; l++) {
        const cJSON *t;
        cJSON_ArrayForEach(t, lists[l]) {
            const char *region = tuple_str(t, 1);
            if (!meta_of(t)) continue;
            if (row_krunker(t)) { // krunker.io's customs and its regions (kio_group)
                const int gi = kio_group(groups, &count, max, t);
                if (gi >= 0) group_push(&groups[gi], t);
                continue;
            }
            if (only_default && strcmp(def, region)) continue;
            if (find_group(groups, count, region) < 0 && count < max) {
                ServerGroup *g = &groups[count];
                snprintf(g->key, sizeof(g->key), "%s", region);
                if (show_region()) snprintf(g->name, sizeof(g->name), "%s", region_long_name(region));
                else snprintf(g->name, sizeof(g->name), "%s",
                                   !strcmp(def, region) ? I18N("settings.local.region", "Default Region")
                                   : !strcmp(g_region, region) ? I18N("kn.servers.currentRegion", "Current Region")
                                   : I18N("kn.servers.hiddenRegion", "Hidden Region"));
                if (!strcmp(def, region)) g->order = 1;
                g->region_rank = count;
                count++;
            }
            const int gi = find_group(groups, count, row_official(t) ? "official" : row_custom(t) ? "custom" : region);
            if (gi >= 0) group_push(&groups[gi], t);
        }
    }
    int kept = 0;
    for (int i = 0; i < count; i++) {
        if (groups[i].game_count) groups[kept++] = groups[i];
    }
    count = kept;
    qsort(groups, (size_t) count, sizeof(*groups), cmp_groups_order);
    for (int i = 0; i < count; i++) {
        stable_sort(groups[i].games, groups[i].game_count, cmp_games_old);
        const char *key = groups[i].key;
        if (!strcmp(key, "custom") || !strcmp(key, "official") || !strcmp(key, "kio:custom") || !strcmp(key, "kio:official")) {
            stable_sort(groups[i].games, groups[i].game_count, cmp_games_region);
        }
    }
    return count;
}

// "Ends in 4m 12s " (UTILS.getReadableTime(ms, true, undefined, true)); "" for 0
static void readable_time(char *out, const size_t n, const double ms) {
    long s = (long) floor(ms / 1000.0);
    long m = s / 60;
    s %= 60;
    long h = m / 60;
    m %= 60;
    long d = h / 24;
    h %= 24;
    const long y = d / 365;
    d %= 365;
    size_t len = 0;
    out[0] = 0;
    if (y) len += (size_t) snprintf(out + len, n - len, "%ldy ", y);
    if (d && len < n) len += (size_t) snprintf(out + len, n - len, "%ldd ", d);
    if (h && len < n) len += (size_t) snprintf(out + len, n - len, "%ldh ", h);
    if (m && len < n) len += (size_t) snprintf(out + len, n - len, "%ldm ", m);
    if (s && len < n) snprintf(out + len, n - len, "%lds ", s);
}

static void time_left_text(char *out, const size_t n, const double seconds) {
    char t[64];
    readable_time(t, sizeof(t), seconds * 1000.0);
    if (t[0]) i18n_trf(out, n, "windows.servers.timeLeft", "Ends in {0}", 1, I18N_ARGV(t));
    else snprintf(out, n, "%s", I18N("windows.servers.starting", "Starting"));
}

// COLORS.ping by the region's measured ping (v10's pingData, measured with each list load: on_row_pings;
// krunker.io's regions from krunker_lobbies.c). Not measured, or a failed ping (-1): low, as v10 has it.
static const char *region_ping_color(const char *region, const int krunker) {
    double ping = 0;
    if (krunker) krunker_lobbies_region_ping_ms(region, &ping);
    else {
        const cJSON *p = cJSON_GetObjectItemCaseSensitive(g_row_pings, region);
        if (cJSON_IsNumber(p)) ping = p->valuedouble;
    }
    return ping / 2 >= 150 ? "rgb(255, 75, 66)" : ping / 2 >= 50 ? "rgb(255, 213, 66)" : "rgb(158, 235, 86)";
}

// The row's title: a custom game's own name (h) as it is, else "<mode name> - <map name>": the
// mode's display name (Team Deathmatch, not tdm) and the map's catalogue name (shipwrecked ->
// Tortuga; an official krunker.io name such as Tortuga resolves too), the key when the catalogue
// does not know the map. Dedicated servers (24/7, finder) show their map alone, as v10 does.
static void row_title(StrBuf *sb, const cJSON *t) {
    if (row_h(t)[0]) {
        sb_append(sb, row_h(t));
        return;
    }
    const char *key = row_i(t);
    char map[128] = "";
    // an official krunker.io lobby's `i` is Krunker's own mapName (krunker_maps.h), shown as sent like v10
    // does: the catalogue's fuzzy names would retitle it ("Old_Burg" is the alias of Burg (1.5.2),
    // "Slide Moonlight" the key slide_moonlight, krunker_maps_test test_lobby_names)
    const int index = key[0] && !row_krunker(t) ? map_catalog_resolve(key, NULL, 0, NULL) : -1;
    const MapCatalogEntry *entry = index >= 0 ? map_catalog_at((size_t) index) : NULL;
    snprintf(map, sizeof(map), "%s", entry && entry->name ? entry->name : key);
    const GameModeConfig *mode = row_mode(t);
    if (mode && mode->name && !row_dedicated(t)) {
        sb_append(sb, mode->name);
        if (map[0]) sb_append(sb, " - ");
    }
    sb_append(sb, map);
}

// An official krunker.io row's title exactly as v10's getServers writes it (v10.js:211428-211433):
// h || i (Krunker's own map name, as sent), "<alias>_" before it on a public lobby ("ffa_Kanji"); in
// the region browser a custom game without its own name gets the alias as a prefix before the badges.
// Our own rows keep KrunkNative's "<mode> - <map>" (row_title).
static void krunker_row_title(StrBuf *prefix, StrBuf *name, const cJSON *t, const int old) {
    const int custom = row_custom(t), official = row_official(t), dedicated = row_dedicated(t);
    const char *alias = row_alias(t), *h = row_h(t);
    if ((custom || old) && !(dedicated || h[0]) && alias[0]) sb_appendf(prefix, "%s_", alias);
    if (!old && !(official || dedicated || custom || h[0]) && alias[0]) sb_appendf(name, "%s_", alias);
    sb_append(name, h[0] ? h : row_i(t));
}

// One server row (getServers' settName settNameIn). The row's label markup is v10981.
static void append_row(StrBuf *sb, const cJSON *t, const double now) {
    const int old = old_browser();
    const char *id = tuple_str(t, 0);
    const int custom = row_custom(t), official = row_official(t), dedicated = row_dedicated(t);
    const int finder = meta_flag(t, "finder");
    const int krunker = row_krunker(t); // mm-token: an official krunker.io lobby
    const int current = !finder && !krunker && g_game_id[0] && !strcmp(g_game_id, id);
    char def[32] = "";
    default_region(def, sizeof(def));
    StrBuf name = {0}, prefix = {0};
    if (krunker) krunker_row_title(&prefix, &name, t, old);
    else row_title(&name, t);
    // an official krunker.io row looks like v10's own (no tag); kioRow only marks it for tests
    sb_append(sb, krunker ? "<div class='settName settNameIn kioRow' onmouseenter='playTick()'style='" : "<div class='settName settNameIn' onmouseenter='playTick()'style='");
    if (current) sb_append(sb, "color:#fff !important;");
    sb_append(sb, "padding-top:10px;margin-top:0px;' onclick='playSelect(0.1);");
    if (krunker) {
        // Krunker's protocol (krunker_net.h): joined in this client, or krunker.io/?game=<id> in the
        // system browser with the opt-out (krunker_lobbies_open)
        sb_append(sb, "krunkerJoin(\"");
        sb_append_esc(sb, id);
        sb_append(sb, "\")'>");
    } else if (finder) {
        // a finder-only server: joined directly (finderJoin(host, port))
        sb_append(sb, "finderJoin(\"");
        sb_append_esc(sb, meta_str(t, "host"));
        sb_appendf(sb, "\", %d)'>", (int) cJSON_GetNumberValue(cJSON_GetObjectItem(meta_of(t), "port")));
    } else {
        // v7: checkedSwitchServer(id, this.querySelector(".serverPCount")); the element (the
        // row) is passed and the handler looks the cell up (servers_ui_handle)
        sb_append(sb, "checkedSwitchServer(\"");
        sb_append_esc(sb, id);
        sb_append(sb, "\", this)'>");
    }
    if (krunker) sb_append_esc(sb, prefix.data ? prefix.data : ""); // v10: "ffa_" before the badges
    else if (!custom && !old) sb_append(sb, "[YEND] ");
    sb_free(&prefix);
    if (official) sb_append(sb, " <div class='krIcon' style='margin-top:-14px;vertical-align:middle;background-color: var(--green);'>OFFICIAL</div> ");
    if (official || dedicated) sb_append(sb, " <div class='krIcon' style='margin-top:-14px;vertical-align:middle;background-color: var(--blue);'>24/7</div> ");
    sb_append_esc(sb, name.data ? name.data : "");
    sb_free(&name);
    if (meta_flag(t, "kr") || meta_flag(t, "earnKR")) sb_append(sb, " <div class='krIcon' style='margin-top:-6px;vertical-align:middle'>KR</div> ");
    if (meta_flag(t, "pw") || meta_flag(t, "pr")) sb_append(sb, " <span class='material-icons' style='margin-top:-6px;color:#FBC02D;vertical-align:middle'>lock</span> ");
    char *badge = servers_ui_entry_badge_html(t); // Outdated / Incompatible (build versions)
    sb_append(sb, badge);
    free(badge);
    sb_appendf(sb, "<div class='serverPCount'>%d/%d</div>", row_players(t), tuple_int(t, 3));
    // v7 shows tuple[0].split(":")[0] on custom / official rows.
    // KrunkNative: on every brain lobby, as the server's proper name ("Chicago 2", the node id
    // CHITOWN2 as tooltip): a region group holds several servers (finder rows: their host).
    // an official row as v10 draws it: the cell only on custom / official-custom rows and in the mode
    // browser, reading the id's prefix ("NY")
    if (krunker ? custom || !old || official : custom || !old || official || !finder) {
        sb_append(sb, "<div class='serverRegion'");
        if (!show_region()) {
            sb_append(sb, ">");
            sb_append(sb, !strcmp(def, tuple_str(t, 1)) ? "DEFAULT" : !strcmp(g_region, tuple_str(t, 1)) ? "CURRENT" : "HIDDEN");
        } else {
            char node[96], proper[128];
            snprintf(node, sizeof(node), "%s", id);
            char *colon = strchr(node, ':');
            if (colon) *colon = 0;
            if (finder) snprintf(proper, sizeof(proper), "%s", node);
            else if (krunker) snprintf(proper, sizeof(proper), "%s", node);
            else server_display_name(node, tuple_str(t, 1), proper, sizeof(proper));
            sb_append(sb, " title='");
            sb_append_esc(sb, node);
            sb_append(sb, "'>");
            sb_append_esc(sb, proper);
        }
        sb_append(sb, " -&nbsp;</div>");
    }
    sb_appendf(sb, "<div class='serverPing material-icons' style='color:%s!important;'>signal_cellular_alt</div>", region_ping_color(tuple_str(t, 1), krunker));
    if (!official && !custom && !dedicated && show_timer()) {
        const cJSON *tl = cJSON_GetArrayItem(t, 5);
        if (!cJSON_IsNumber(tl)) tl = cJSON_GetObjectItem(meta_of(t), "timeLeft");
        double left = (cJSON_IsNumber(tl) ? tl->valuedouble : 0.0) - (now - g_list_start_ms) / 1000.0;
        if (left < 0) left = 0;
        char text[96];
        time_left_text(text, sizeof(text), left);
        sb_appendf(sb, "<div class=\"serverTimeLeft\" data-time-left=\"%.15g\">%s</div>", left, text);
    }
    sb_append(sb, "</div>");
}

static void append_krunker_status(StrBuf *sb, int groups);

// a group holds an official krunker.io lobby (the top groups too: official customs, custom games)
static int group_has_krunker(const ServerGroup *g) {
    for (int i = 0; i < g->game_count; i++) {
        if (row_krunker(g->games[i])) return 1;
    }
    return 0;
}

static char *get_servers(void) {
    static ServerGroup groups[SERVER_GROUPS_MAX];
    const int count = build_groups(groups, SERVER_GROUPS_MAX);
    const int old = old_browser();
    const int hide = hide_full();
    const double now = now_ms();
    char def[32] = "";
    default_region(def, sizeof(def));
    StrBuf sb = {0};
    int kio_groups = 0, kio_status = 0;
    for (int h = 0; h < count; h++) kio_groups += group_has_krunker(&groups[h]);
    for (int h = 0; h < count; h++) {
        const ServerGroup *g = &groups[h];
        const int open = g_expanded == h;
        if (!kio_status && !strncmp(g->key, "kio:", 4)) { // the krunker.io tab's groups: their status line first
            append_krunker_status(&sb, kio_groups);
            kio_status = 1;
        }
        // v10's header (name, then Quick Join and "N online" floated right); an official region's own
        // group also carries kioHed, a class no sheet styles (tests find the krunker.io groups by it)
        sb_appendf(&sb, "<div class='setHed%s' style='margin-top:20px;' onmouseenter='playTick()' onclick='playSelect(0.1);setExpandedRegion(%d)' style='cursor: pointer;'",
                   !strncmp(g->key, "kio:", 4) ? " kioHed" : "", open ? -1 : h);
        if (old && show_region() && strcmp(g->key, "custom") && strcmp(g->key, "official") && region_full_name(g->key)[0]) {
            sb_append(&sb, " title='"); // KrunkNative: the place name's region code (the links' prefix)
            sb_append_esc(&sb, region_name(g->key));
            sb_append(&sb, "'");
        }
        sb_appendf(&sb, "><span class='material-icons plusOrMinus'>%s</span> ", open ? "expand_more" : "navigate_next");
        sb_append_esc(&sb, g->name);
        sb_appendf(&sb, "<span style='float:right;color:rgba(255,255,255,0.9)'>%.0f <span style='color:rgba(255,255,255,0.6)'>online</span></span>", g->players);
        // krunker.io groups too: the first joinable lobby (servers_quick_join_next)
        sb_appendf(&sb, "<div class='quickJoin' onclick='quickJoinRegion(%d,%d)'>Quick Join</div></div>", h, old ? 0 : 1);
        if (!open) continue;
        sb_append(&sb, "<div class='setBodH' style='padding-top:10px;padding-bottom:10px;'>");
        int shown = 0, kept = 0;
        for (int i = 0; i < g->game_count; i++) {
            const cJSON *t = g->games[i];
            if (hide && tuple_int(t, 2) >= tuple_int(t, 3)) continue; // hideFull: players != max
            kept++;
            if (default_region_only() && strcmp(def, tuple_str(t, 1))) continue;
            if (g_search[0] && !search_matches(t)) continue;
            shown++;
            append_row(&sb, t, now);
        }
        if (!kept || !shown)
            sb_appendf(&sb, "<div style='margin-left: 30px;margin-top: 5px;margin-bottom: 2px;'>%s</div>", I18N("windows.servers.none", "No Joinable Games Found..."));
        sb_append(&sb, "</div>");
    }
    if (!kio_status && g_server_tab == SERVER_TAB_KRUNKER) append_krunker_status(&sb, kio_groups); // no group of its own: the status alone
    return sb_finish(&sb);
}

// ------------------------------------------------------------------ server finder + direct connect
// Any server is also reachable directly by host:port. Finder rows and Direct Connect join
// without the brain: plain connect; a server's AUTH_CHALLENGE runs the hasJoined handshake
// for a signed-in player, everyone else joins as a guest (online_mode.c).

static void on_finder(void *user, const int status, const char *body, const size_t length) {
    (void) user;
    (void) length;
    g_finder_loading = 0;
    cJSON *res = status == 200 ? cJSON_Parse(body) : NULL;
    const cJSON *servers = cJSON_GetObjectItem(res, "servers");
    if (cJSON_IsArray(servers)) {
        servers_ui_note_list_response(res); // download_url / downloads (build versions)
        cJSON_Delete(g_finder_servers);
        g_finder_servers = cJSON_Duplicate(servers, 1);
        g_finder_error[0] = 0;
        printf("[finder] %s: %d servers\n", online_finder_url(), cJSON_GetArraySize(servers));
    } else {
        // the failure goes to the log and into the browser (it used to look like "no servers"):
        // the transport's reason (status 0, backend.h), the HTTP status, or an unusable answer
        if (!status) snprintf(g_finder_error, sizeof(g_finder_error), "%s", body && body[0] ? body : "unreachable");
        else if (status != 200) snprintf(g_finder_error, sizeof(g_finder_error), "HTTP %d", status);
        else snprintf(g_finder_error, sizeof(g_finder_error), "not a server list");
        fprintf(stderr, "[finder] %s failed: %s\n", online_finder_url(), g_finder_error);
    }
    cJSON_Delete(res);
    g_finder_loaded_ms = now_ms();
    menu_rerender_if_open(2);
}

// --render-scenario: the scenario's lists (state.servers / krunkerLobbies) are the whole list; the live
// server finder would add whatever runs at the moment to a reference render.
static int finder_off(void) {
#ifdef UI_SCENARIO_SUPPORTED
    return ui_scenario_active();
#else
    return 0;
#endif
}

static void finder_refresh(void) {
    if (finder_off() || !online_finder_url()[0] || g_finder_loading || now_ms() - g_finder_loaded_ms < SERVER_BROWSER_RATE_MS) return;
    char url[320];
    snprintf(url, sizeof(url), "%s", online_finder_url()); // every server takes guests: no online_mode filter
    // the product flavor's pool (finder_table.c): a Strike client lists only Strike lobbies
    if (flavor_get() == FLAVOR_STRIKE) snprintf(url + strlen(url), sizeof(url) - strlen(url), "%cflavor=strike", strchr(url, '?') ? '&' : '?');
    g_finder_loading = backend_http_request("GET", url, NULL, on_finder, NULL);
    if (!g_finder_loading) {
        snprintf(g_finder_error, sizeof(g_finder_error), "the request could not be sent");
        fprintf(stderr, "[finder] %s failed: %s\n", url, g_finder_error);
    } else {
        printf("[finder] GET %s\n", url);
    }
}

// The finder's failure as a row above the list (servers_window_gen), "" when it answered.
static void append_finder_error(StrBuf *sb) {
    if (!g_finder_error[0] || !online_finder_url()[0]) return;
    sb_append(sb, "<div id='finderError' class='setBodH' style='padding:10px;margin-left:0px;width:calc(100% - 40px);color:#ff6b6b'>");
    sb_append_esc(sb, I18N("kn.servers.finderUnavailable", "Server finder unavailable:"));
    sb_append(sb, " ");
    sb_append_esc(sb, g_finder_error);
    sb_append(sb, " <span style='color:rgba(255,255,255,0.5)'>(");
    sb_append_esc(sb, online_finder_url());
    sb_append(sb, ")</span></div>");
}

// The official krunker.io lobbies' status line (native), before their first group, or after ours. It
// has no label: the krunker.io tab says which servers these are. It says why there are none (loading,
// krunker.io refusing this network, unreachable, nothing matching) and stays empty when the lobbies
// are listed. The line is a plain one: which account an official game uses is the one account UI's
// (backend_ui.h active_account(), v10's header and login modal once the game is joined), never a
// second login here.
static void append_krunker_status(StrBuf *sb, const int groups) {
    if (!krunker_lobbies_enabled()) return;
    sb_append(sb, "<div id='kioStatus' class='kioSep'>");
    const char *why = NULL;
    int error = 0;
    if (!krunker_lobbies_list() && krunker_lobbies_loading()) why = I18N("kn.servers.kioLoading", "Loading the krunker.io servers...");
    else if (krunker_lobbies_error()[0] && !groups) why = krunker_lobbies_error(), error = 1;
    else if (!groups) why = I18N("kn.servers.kioNoMatch", "No krunker.io server matches the search or filter.");
    else if (!krunker_net_in_client()) why = I18N("kn.servers.kioOpensInBrowser", "krunker.io servers open in your browser.");
    if (why) {
        sb_append(sb, error ? "<div class='kioSepWhy kioSepErr'>" : "<div class='kioSepWhy'>");
        sb_append_esc(sb, why);
        sb_append(sb, "</div>");
    }
    sb_append(sb, "</div>");
}

// Leave the brain's lobby (if any) and connect straight to host:port.
static void direct_join(const char *host, const int port) {
    if (!g_client || !host || !host[0] || port <= 0 || port > 65535) return;
    // a direct session: no seek-game, no lobby id, no automatic matchmaking until the
    // user asks for it (g_pin)
    g_pin = PIN_DIRECT;
    g_last_direct = 1;
    g_game_id[0] = 0;
    g_account_game[0] = 0;
    page_game_url(NULL, NULL); // the lobby id (if a brain lists host:port) follows once connected
    char password[sizeof(g_client->net_join.password)] = "", target[160];
    snprintf(target, sizeof(target), "%s:%d", host, port);
    recent_lobbies_push(&g_recent_servers, target);
    pthread_mutex_lock(&g_client->net_lock);
    if (!strcmp(target, g_password_game)) memcpy(password, g_client->net_join.password, sizeof(password));
    memset(&g_client->net_join, 0, sizeof(g_client->net_join));
    memcpy(g_client->net_join.password, password, sizeof(password)); // the server password of this server
    if (!password[0]) g_password_game[0] = 0;
    snprintf(g_client->net_address, sizeof(g_client->net_address), "%s", host);
    g_client->net_port = (unsigned short) port;
#ifdef __EMSCRIPTEN__
    net_web_target_url(g_client->net_ws_url, sizeof(g_client->net_ws_url), host, port);
#endif
    pthread_mutex_unlock(&g_client->net_lock);
    printf("[mm] direct connect %s:%d (%s mode)\n", host, port, online_mode_name());
    menu_show_window(MENU_WINDOW_NONE, 0);
    menu_show_connecting_map(NULL); // CONNECTING: the spinner until the server answers
    client_net_switch_server(g_client);
}

// Console `connect host[:port]`: the same direct (pinned) session as Direct Connect.
void servers_ui_connect_direct(const char *host, const int port) { direct_join(host, port); }

// Console `disconnect`: leave the server and stay disconnected (the connection error screen
// says "Disconnected") until the user picks Play / a server / `connect` / `reconnect`.
void servers_ui_disconnect(void) {
    if (!g_client) return;
    if (g_pin != PIN_IDLE) g_last_direct = g_pin == PIN_DIRECT || !g_brain_mode || !g_game_id[0];
    g_pin = PIN_IDLE;
    g_left_offline = g_client->offline;
    printf("[mm] disconnected by the user (reconnect: %s)\n", g_last_direct ? "same server" : g_game_id);
    page_game_url(NULL, NULL); // devConsole disconnect: "/"
    if (g_client->offline) local_game_stop(g_client);
    client_net_stop(g_client, "Disconnected");
}

// Console `reconnect` after a `disconnect` from the offline game (and nothing joined since):
// 1 = start the offline game again instead of a server. Consumes the flag.
int servers_ui_reconnect_offline(void) {
    const int again = g_left_offline && g_pin == PIN_IDLE && g_client && !g_client->offline;
    g_left_offline = 0;
    return again;
}

// Console `reconnect`: the last server again, through the same transport fallback (UDP,
// then TCP / WebTransport, then WebSocket). A brain lobby is sought again by its id for a fresh
// join ticket (the old one expires join_ttl_s after its seek: server/src/accounts.c).
// After the server stopped / refused the ticket (g_server_gone) its lobby is likely gone: the seek
// may change game (autoChangeGame), like the automatic rejoin. A brain session never reconnects
// with its old ticket: without a lobby id it seeks the region's best lobby.
int servers_ui_reconnect(void) {
    if (!g_client) return 0;
    if (!g_last_direct && g_brain_mode && g_pin != PIN_DIRECT && (g_game_id[0] || g_client->net_join.client_key[0])) {
        char id[sizeof(g_game_id)];
        snprintf(id, sizeof(id), "%s", g_game_id);
        printf("[mm] reconnect: seeking %s again\n", id[0] ? id : "a game");
        seek(id[0] ? id : NULL, !id[0] || g_server_gone, NULL);
        return 1;
    }
    if (!g_client->net_address[0]) return 0;
    g_pin = g_last_direct ? PIN_DIRECT : PIN_NONE;
    printf("[mm] reconnect: %s:%u\n", g_client->net_address, (unsigned int) g_client->net_port);
    client_net_switch_server(g_client);
    return 1;
}

int servers_ui_session_target(char *out, const size_t n) {
    if (!g_client || !out || !n) return 0;
    // a matchmade lobby: also while it is still being joined (F5 during the load) or rejoined; never
    // one the player left with `disconnect` (PIN_IDLE)
    if (!g_last_direct && g_brain_mode && g_pin == PIN_NONE && g_game_id[0]) {
        snprintf(out, n, "%s", g_game_id); // `connect <id>` seeks it again (servers_ui_connect_game)
        return 2;
    }
    if (!g_client->net_address[0]) return 0;
    snprintf(out, n, "%s:%u", g_client->net_address, (unsigned int) g_client->net_port);
    return 1;
}

// ---- KrunkNative: rejoining when the server goes away or refuses the join key. v7.1.4's server
// sends ["error", "GAME UPDATING"] before an update restart and its client shows the Game
// Updating screen until the player picks Find a Game (backToOrigin: a fresh matchmaker seek).
// The native transport used to retry the same address with the spent join ticket instead, and
// the restarted server answered "Join key expired" (server/src/accounts.c, 004001). Now:
//  * ERROR 004004 / 004006 (server_shutdown_message: update / restart / shutdown): the lobby is
//    sought again with autoChangeGame after REJOIN_FIRST_DELAY, so a fresh ticket for the same
//    game id if it is back, else for the region's best lobby (fill-first, doc/backend.md 6.2);
//    while the matchmaker has nothing (the region's only server restarting) every
//    REJOIN_RETRY_DELAY for REJOIN_WINDOW. The transport holds meanwhile (events.c: a server
//    ERROR stops its retries), so the old ticket is never sent again. A direct (pinned) server
//    has no tickets: the transport retries it after the same delay.
//  * ERROR 004001 about the join key (expired, invalid, another server's: e.g. the transport
//    retried a crashed server's address after its restart): one fresh seek; a second refusal
//    within REJOIN_KEY_WINDOW is shown ("Could not rejoin: ... again").
// The error screen keeps the server's message with a "Rejoining..." line (menu_ui.c).

static int ci_contains(const char *text, const char *needle) {
    const size_t n = strlen(needle);
    for (; *text; text++) {
        if (!strncasecmp(text, needle, n)) return 1;
    }
    return 0;
}

// A lost session (no server ERROR: the transport's own reason is on the screen, events.c).
// Matchmade: a fresh ticket for the same lobby (autoChangeGame: the region's best when it is
// gone; the transport holds meanwhile, so the old ticket is never resent). Direct / no
// matchmaker: the transport reconnects. Sessions that keep dropping right away stop it.
static int connection_dropped(char *error, const size_t error_size) {
    const double now = glfwGetTime();
    const int quick = g_session_started > 0.0 && now - g_session_started < REJOIN_DROP_STABLE;
    g_drop_streak = quick ? g_drop_streak + 1 : 1;
    if (g_drop_streak > REJOIN_DROP_MAX) {
        char original[128];
        snprintf(original, sizeof(original), "%s", error);
        snprintf(error, error_size, "%s (gave up after %d quick reconnects)", original, REJOIN_DROP_MAX);
        fprintf(stderr, "[mm] connection lost %d times in a row within %.0f s: not rejoining (%s)\n", g_drop_streak,
                REJOIN_DROP_STABLE, original);
        rejoin_cancel();
        return 0;
    }
    if (!g_brain_mode || g_pin == PIN_DIRECT || backend_available() != 1) {
        // the transport reconnects on its own, with its backoff (a dead server is not hammered);
        // only a held one (it had a ticket) is asked to
        printf("[mm] connection lost (%s): reconnecting\n", error);
        if (g_rejoin != REJOIN_DIRECT || now >= g_rejoin_until) g_rejoin_until = now + REJOIN_WINDOW;
        g_rejoin = REJOIN_DIRECT;
        g_rejoin_at = g_client->net_hold_retry ? now + REJOIN_DROP_DELAY : 0.0;
        menu_update_instructions();
        return 1;
    }
    if (g_rejoin != REJOIN_DROP || now >= g_rejoin_until) g_rejoin_until = now + REJOIN_WINDOW;
    g_rejoin = REJOIN_DROP;
    g_rejoin_at = now + REJOIN_DROP_DELAY;
    snprintf(g_rejoin_game, sizeof(g_rejoin_game), "%s", g_game_id);
    printf("[mm] connection lost (%s): rejoining %s through the matchmaker\n", error,
           g_rejoin_game[0] ? g_rejoin_game : "the region's best lobby");
    menu_update_instructions();
    return 1;
}

// Main thread, for every NET_STATUS_FAILED (menu_ui.c menu_poll_connection): error / code are
// what the screen shows; error may be rewritten. dropped: the transport lost a live session
// without a server ERROR (events.c). 1 = a rejoin is under way.
int servers_ui_connection_lost(char *error, const size_t error_size, const char *code, const int dropped, const int had_session) {
    if (!g_client || g_client->offline || !code) return servers_ui_rejoining();
    // the game needs its server password (v10 ENTER_CODE -> gameCode()): ask; Submit joins again
    if (!dropped && !strcmp(code, NET_ERROR_CODE_PASSWORD) && !ci_contains(error, "too many")) {
        if (strike_ui_active()) strike_menu_server_password(); // Krunker Strike's prompt (SERVER_PASSWORD)
        else v10_party_password_prompt();
    }
    if (dropped) return g_pin == PIN_IDLE ? servers_ui_rejoining() : connection_dropped(error, error_size);
    const int restart = !strcmp(code, NET_ERROR_CODE_UPDATING) || !strcmp(code, NET_ERROR_CODE_SHUTDOWN);
    const int key = !strcmp(code, NET_ERROR_CODE_TICKET) && ci_contains(error, "join key");
    if ((!restart && !key) || g_pin == PIN_IDLE) return servers_ui_rejoining();
    const double now = glfwGetTime();
    // The server of a live session stops for an update (004004: "restarting for an update,
    // reconnect after"; a drain refuses new connections with the same code, but those never had a
    // session). Settings > Client > Auto-restart (default on): the client restarts itself and the
    // new process rejoins this lobby (the restart rejoin's backoff) or the direct server; no
    // in-place rejoin here, the screen keeps the server's message with "Restarting..." meanwhile.
    if (had_session && !strcmp(code, NET_ERROR_CODE_UPDATING)) {
        const int lobby = g_brain_mode && g_pin != PIN_DIRECT && backend_available() == 1 && g_game_id[0];
        if (game_reload_server_updating(g_client, lobby ? g_game_id : NULL)) {
            g_server_gone = 1;
            rejoin_cancel();
            menu_update_instructions();
            return 1;
        }
    }
    if (!g_brain_mode || g_pin == PIN_DIRECT || backend_available() != 1) {
        if (!restart) return servers_ui_rejoining();
        // a direct server: the transport (holding after the ERROR) tries the address again
        printf("[mm] server stopped (%s): reconnecting to it\n", error);
        if (g_rejoin != REJOIN_DIRECT || now >= g_rejoin_until) g_rejoin_until = now + REJOIN_WINDOW;
        g_rejoin = REJOIN_DIRECT;
        g_rejoin_at = now + REJOIN_FIRST_DELAY;
        menu_update_instructions();
        return 1;
    }
    g_server_gone = 1;
    if (key) {
        static double last_key_rejoin = -1e9;
        if (now - last_key_rejoin < REJOIN_KEY_WINDOW) {
            // the fresh ticket was refused too: say so (the error screen's Find a Game seeks again)
            char original[96];
            snprintf(original, sizeof(original), "%s", error);
            i18n_trf(error, error_size, "kn.mm.rejoinFailed", "Could not rejoin: {0} again", 1, I18N_ARGV(original));
            fprintf(stderr, "[mm] rejoin failed: a fresh join ticket was refused too (%s)\n", original);
            rejoin_cancel();
            return 0;
        }
        last_key_rejoin = now;
        g_rejoin = REJOIN_KEY;
        g_rejoin_at = now; // right away: the server is up, only the ticket was stale
    } else {
        if (g_rejoin != REJOIN_RESTART || now >= g_rejoin_until) g_rejoin_until = now + REJOIN_WINDOW;
        g_rejoin = REJOIN_RESTART;
        g_rejoin_at = now + REJOIN_FIRST_DELAY;
    }
    snprintf(g_rejoin_game, sizeof(g_rejoin_game), "%s", g_game_id);
    printf("[mm] %s (%s): rejoining %s through the matchmaker\n", key ? "join key refused" : "server stopped", error,
           g_rejoin_game[0] ? g_rejoin_game : "the region's best lobby");
    menu_update_instructions();
    return 1;
}

// 1 while a rejoin is due or its seek / connection runs (the error screen says "Rejoining...").
int servers_ui_rejoining(void) {
    if (game_reload_restart_pending()) return 1; // the client restarts for the update (game_reload.c)
    return g_rejoin != REJOIN_NONE && (g_rejoin_at > 0.0 || g_rejoin_seek || g_rejoin == REJOIN_DIRECT);
}

// The error screen's line while rejoining: the same lobby again after a lost connection or on a
// direct server, else a (possibly other) game through the matchmaker; "Restarting..." while the
// client restarts for an update.
const char *servers_ui_rejoin_text(void) {
    if (game_reload_restart_pending()) return I18N("kn.update.restarting", "Restarting...");
    if (g_rejoin == REJOIN_DROP || g_rejoin == REJOIN_DIRECT || servers_ui_pinned() || !g_brain_mode)
        return I18N("kn.mm.reconnecting", "Reconnecting...");
    return I18N("kn.mm.rejoining", "Rejoining a game...");
}

// A restart's resume (game_reload.c): no startup seek until servers_ui_resume_rejoin says which
// lobby, or the resume gives up (hold 0).
void servers_ui_hold_startup_seek(const int hold) {
    g_startup_hold = hold;
}

// A restart's resume went into its session another way (`offline`, `connect`, the official game):
// the startup seek never runs (no launch seek either).
void servers_ui_skip_startup_seek(void) {
    g_startup_hold = 0;
    g_started = 1;
}

// F5's relaunch (game_reload.c): the new process's startup seek is the old one's lobby, by its id.
void servers_ui_reload_resume(const char *game_id) {
    snprintf(g_reload_game, sizeof(g_reload_game), "%s", game_id ? game_id : "");
}

// The console's `reload` in place (game_reload.c): the lobby the player is in, sought again by its id
// for a fresh ticket (gone or full: reload_fallback). 0 = not in a matchmade lobby.
int servers_ui_reload_rejoin(void) {
    if (!g_client || g_last_direct || !g_brain_mode || g_pin != PIN_NONE || !g_game_id[0] || g_seek_inflight || backend_available() != 1)
        return 0;
    char id[sizeof(g_game_id)];
    snprintf(id, sizeof(id), "%s", g_game_id);
    printf("[mm] reload: rejoining lobby %s\n", id);
    g_reload_next = 1;
    seek(id, 0, NULL);
    return 1;
}

// The new process after a restart for a server update: the lobby the old one was in, sought
// again like after its ERROR (REJOIN_RESTART: a fresh ticket for the same game id once the
// matchmaker lists it, every REJOIN_RETRY_DELAY for REJOIN_WINDOW, else the region's best lobby
// with autoChangeGame). The error screen shows the server's message meanwhile.
void servers_ui_resume_rejoin(const char *game_id) {
    g_startup_hold = 0;
    g_started = 1; // no startup seek: this is it
    g_pending_game[0] = 0; // the page's ?game= names this lobby too (the web's address bar)
    if (!g_brain_mode || !g_client) {
        printf("[mm] resume: no matchmaker, the lobby %s cannot be rejoined\n", game_id ? game_id : "");
        return;
    }
    const double now = glfwGetTime();
    g_pin = PIN_NONE;
    g_server_gone = 1;
    g_rejoin = REJOIN_RESTART;
    g_rejoin_at = now + REJOIN_FIRST_DELAY;
    g_rejoin_until = now + REJOIN_WINDOW;
    snprintf(g_rejoin_game, sizeof(g_rejoin_game), "%s", game_id ? game_id : "");
    printf("[mm] resume after the server's update: rejoining %s through the matchmaker\n", g_rejoin_game[0] ? g_rejoin_game : "the region's best lobby");
    menu_show_connect_error("GAME UPDATING", NET_ERROR_CODE_UPDATING); // the Game Updating screen with "Rejoining a game..."
}

static void rejoin_tick(Client *client) {
    static double connected_since = -1.0;
    const double now = glfwGetTime();
    if (!client->net_connected) connected_since = -1.0;
    else if (connected_since < 0.0) connected_since = g_session_started = now;
    if (connected_since >= 0.0 && now - connected_since >= REJOIN_DROP_STABLE) g_drop_streak = 0; // a session that lasted
    if (g_rejoin == REJOIN_NONE) return;
    if (connected_since >= 0.0 && now - connected_since >= REJOIN_SETTLED && g_rejoin_at <= 0.0 && !g_rejoin_seek) {
        rejoin_cancel(); // back in a lobby
        return;
    }
    if (g_rejoin == REJOIN_DIRECT && now >= g_rejoin_until && g_rejoin_at <= 0.0) {
        rejoin_cancel(); // the transport keeps retrying with its backoff, its errors show again
        return;
    }
    if (g_rejoin_at <= 0.0 || now < g_rejoin_at || g_seek_inflight) return;
    g_rejoin_at = 0.0;
    if (g_rejoin == REJOIN_DIRECT) {
        if (!client->net_connected) client_net_request_reconnect(client);
        return;
    }
    if (g_pin != PIN_NONE || !g_brain_mode) {
        rejoin_cancel();
        return;
    }
    char id[sizeof(g_rejoin_game)];
    snprintf(id, sizeof(id), "%s", g_rejoin_game);
    printf("[mm] rejoin: seeking %s\n", id[0] ? id : "a game");
    g_rejoin_next = 1;
    seek(id[0] ? id : NULL, 1, NULL);
}

// ---- KrunkNative addition: the menu's Connect button (#menuBtnConnect, next to Invite / Join
// in #subLogoButtons) and its window (SERVERS_WINDOW_CONNECT, laid out like v7's Join window:
// an .accountInput field and a .joinBtn). The address is parsed like the console's `connect`
// (ui_console.c cmd_connect: scheme / path stripped, "host[:port]" with port 21015 by default,
// "local" / "localhost" = 127.0.0.1, natively IPv4 only) and joins through the same pinned
// direct session (servers_ui_connect_direct), so `reconnect` / `disconnect` keep working.

// A dotted IPv4 address (four 0..255 parts, nothing else).
static int ipv4_ok(const char *h) {
    int parts = 0, digits = 0;
    unsigned int v = 0;
    for (const char *p = h;; p++) {
        if (*p >= '0' && *p <= '9') {
            v = v * 10 + (unsigned int) (*p - '0');
            if (++digits > 3 || v > 255) return 0;
        } else if (*p == '.' || !*p) {
            if (!digits) return 0;
            parts++;
            digits = 0;
            v = 0;
            if (!*p) break;
        } else {
            return 0;
        }
    }
    return parts == 4;
}

// A dotted name (krunker.io, eu.krunk.io): labels of letters, digits and inner hyphens, at least
// one dot, not all-numeric (an all-numeric dotted host is an IPv4 address: ipv4_ok).
static int name_ok(const char *h) {
    if (!h[0] || strlen(h) > 253 || !strchr(h, '.')) return 0;
    const char *label = h;
    for (const char *p = h;; p++) {
        if (*p && *p != '.') {
            if (!isalnum((unsigned char) *p) && *p != '-') return 0;
            continue;
        }
        const size_t n = (size_t) (p - label);
        if (!n || n > 63 || label[0] == '-' || p[-1] == '-') return 0;
        if (!*p) break;
        label = p + 1;
    }
    const char *last = strrchr(h, '.') + 1;
    for (const char *p = last; *p; p++) {
        if (!isdigit((unsigned char) *p)) return 1;
    }
    return 0;
}

// The address of a direct connect (console `connect`, the Connect window): "local" / "default"
// = this machine, else host[:port] (port 21015 by default; a scheme and a path are dropped). The
// host is an IPv4 address, localhost or a dotted name (the transport resolves it, natively and on
// the web); anything else, or a port outside 1..65535, is refused with err, the text the player
// reads. 1 = a valid address (host, port written).
int servers_ui_parse_address(const char *text, char *host, const size_t host_n, int *port, char *err, const size_t err_n) {
    char target[256];
    while (text && (*text == ' ' || *text == '\t')) text++;
    snprintf(target, sizeof(target), "%s", text && *text ? text : "local");
    size_t n = strlen(target);
    while (n && (target[n - 1] == ' ' || target[n - 1] == '\t' || target[n - 1] == '\r' || target[n - 1] == '\n')) target[--n] = 0;
    *port = NET_DEFAULT_PORT;
    if (!strcasecmp(target, "local") || !strcasecmp(target, "default")) {
        snprintf(host, host_n, "127.0.0.1");
        return 1;
    }
    const char *start = strstr(target, "://") ? strstr(target, "://") + 3 : target;
    snprintf(host, host_n, "%s", start);
    char *slash = strchr(host, '/');
    if (slash) *slash = 0;
    char *colon = strrchr(host, ':');
    int ok = 1;
    if (colon) {
        *colon = 0;
        const char *digits = colon + 1;
        const size_t len = strlen(digits);
        ok = len > 0 && len <= 5 && strspn(digits, "0123456789") == len;
        if (ok) *port = atoi(digits);
    }
    if (!strcmp(host, "localhost") || !strcmp(host, "0.0.0.0")) snprintf(host, host_n, "127.0.0.1");
    ok = ok && *port > 0 && *port <= 65535 && (!strcmp(host, "127.0.0.1") || ipv4_ok(host) || name_ok(host));
    if (!ok) {
        snprintf(err, err_n, "%s", I18N("kn.connect.badAddress", "Not a game link or server address: paste a krunker.io or Box Office game link, or enter host:port"));
        return 0;
    }
    return 1;
}

char *servers_connect_window_gen(void) {
    StrBuf sb = {0};
    sb_append(&sb, "<input id='connectAddr' type='text' placeholder='");
    sb_append_esc(&sb, I18N("kn.connect.placeholder", "Enter Server Address (host:port)"));
    sb_append(&sb, "' class='accountInput' style='margin-top:0;margin-bottom:10px;' value='");
    if (g_client && g_last_direct && g_client->net_address[0]) { // the last direct server, ready to reconnect
        sb_append_esc(&sb, g_client->net_address);
        if (g_client->net_port != NET_DEFAULT_PORT) sb_appendf(&sb, ":%u", (unsigned int) g_client->net_port);
    }
    sb_append(&sb, "' onkeydown='if(event.keyCode==13)connectServer()'><div id='connectResp' style='display:none;margin-bottom:10px;font-size:18px;color:rgba(255,255,255,0.5);text-align:center'></div><div class='joinBtn' onclick='connectServer()' style='width:100%'>");
    sb_append_esc(&sb, I18N("kn.connect.title", "Connect"));
    sb_append(&sb, "</div>");
    return sb_finish(&sb);
}

// connectServer(): the window's field (or an address argument); a game link / lobby id
// (CH2:kz862) joins that game instead
static void connect_server(const char *text) {
    char host[160], err[200] = "";
    int port;
    const int game = servers_ui_connect_game(text, err, sizeof(err));
    if (game > 0) return;
    if (game == 0 && servers_ui_parse_address(text, host, sizeof(host), &port, err, sizeof(err))) {
        printf("[mm] connect window: %s:%d\n", host, port);
        servers_ui_connect_direct(host, port); // pinned, like the console's connect (its spinner: direct_join)
        return;
    }
    printf("[mm] connect refused: %s\n", err);
    HtmlElem resp = html_dom_by_id("connectResp");
    if (resp) {
        html_dom_set_text(resp, err);
        html_dom_set_display(resp, "block");
    }
}

// The menu's Connect link, injected once after Join (#menuBtnJoin). KrunkNative addition (Krunker
// v10's match bar ends at Join; doc/v10/ui.md 2): it is one more match action in the same bar,
// written exactly like v10's Invite / Join (a .match-action-sep "-" then a .match-action-btn with
// a white span, svelte.css: 14px bold uppercase, 0.7 opacity, 1 on hover; the same playSelect(0.1)
// click sound v10_menu.c wires on them), so it reads as part of the bar, not a standalone button.
// It opens the Connect window (SERVERS_WINDOW_CONNECT). --render-scenario keeps v10's bar for
// uidiff unless the scenario asks for the client's own additions (state.clientChrome:
// kn-main-menu-connect.json).
void servers_ui_menu_setup(void) {
    if (html_dom_by_id("menuBtnConnect")) return;
#ifdef UI_SCENARIO_SUPPORTED
    if (ui_scenario_active() && !ui_scenario_client_chrome()) return;
#endif
    HtmlElem join = html_dom_by_id("menuBtnJoin");
    if (!join) return;
    StrBuf sb = {0};
    sb_append(&sb, " <span class=\"match-action-sep svelte-1vrfz9w\" style=\"color:#fff\">-</span> "
                   "<div class=\"match-action-btn svelte-1vrfz9w\" id=\"menuBtnConnect\" "
                   "onclick=\"playSelect(0.1),openConnectWindow()\"><span style=\"color:#fff\">");
    sb_append_esc(&sb, I18N("kn.connect.title", "Connect"));
    sb_append(&sb, "</span></div>");
    char *html = sb_finish(&sb);
    if (html) html_dom_insert_html(join, HTML_INSERT_AFTER_END, html);
    free(html);
}

// The header's left slot: v7's "+ Host Game" (showWindow(42)). Hosting is the dedicated
// server in KrunkNative (doc/backend.md 8.7), so natively the link is hidden like the menu's
// Host Game button; --render-scenario keeps it for uidiff. (Direct connects: the menu's
// Connect button, servers_ui_menu_setup.)
// The tab's "+ Host Game" (windows[1].hostGame): the Host Game window on this tab's backend, never the session's
// (host_flow_open_backend). v10's header link, shown on both tabs.
static void append_host_game_button(StrBuf *sb) {
    sb_append(sb, "<div style='display:inline-block'><a id='hostGameBtn' onclick='playSelect(0.1);windows[1].hostGame()'>");
    sb_append_esc(sb, I18N("kn.servers.hostGame", "+ Host Game"));
    sb_append(sb, "</a></div>");
}

// ---- the custom games' carousel (v10 windows[1].customsInfo + genList + post: PureJSCarousel)
// loadData() gathers the custom games that run a published map (meta c and cm), one entry per name
// (h, else the map i) with their players summed, most players first; genList() shows them as slides
// when there are at least 3 (at most 31), each the map's thumbnail from user-assets
// (_9259("/m" + cm + "/thumb.png"): https://user-assets.krunker.io/m<cm>/thumb.png), its name, the
// players and a 24/7 tag (oc or ds); a click searches the list for it (windows[1].searchList(name)).
// post() then builds v10's PureJSCarousel (v7.1.4 public/libs/purejscarousel.js) over it: the slides
// three times in a container (clones, the slides, clones) shifted left by one set, and the prev / next
// buttons, which slide three slides in 400 ms and rotate the set. The thumbnails come through the
// engine's remote images (remote_images.h); KRUNKNATIVE_KRUNKER_USER_ASSETS points them elsewhere (tests).
#define CAROUSEL_MAX 31
#define CAROUSEL_SLIDE_W 295 // .slide: 265px + 2 x 15px border (content-box): its offsetWidth
#define CAROUSEL_STEP 3      // goToNextSlide: blockWidth = slides[0].offsetWidth * 3
#define CAROUSEL_SPEED_MS 400.0

static struct {
    int n, rot, busy;
    struct {
        char name[96];
        char cm[48];
        int players;
        int tag; // oc || ds: 24/7
    } e[CAROUSEL_MAX + 32];
} C;

static const char *user_assets_url(void) {
    const char *over = getenv("KRUNKNATIVE_KRUNKER_USER_ASSETS");
    return over && over[0] ? over : "https://user-assets.krunker.io";
}

// customsInfo from the lists the tab shows (krunker.io's custom games carry cm)
static void carousel_collect(void) {
    const cJSON *lists[2];
    const int count = tab_lists(lists);
    C.n = 0;
    for (int l = 0; l < count; l++) {
        const cJSON *t;
        cJSON_ArrayForEach(t, lists[l]) {
            const cJSON *cm = cJSON_GetObjectItem(meta_of(t), "cm");
            char id[48] = "";
            if (cJSON_IsString(cm)) snprintf(id, sizeof(id), "%s", cm->valuestring);
            else if (cJSON_IsNumber(cm)) snprintf(id, sizeof(id), "%.0f", cm->valuedouble);
            if (!row_custom(t) || !id[0]) continue;
            for (const char *p = id; *p; p++) {
                if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || *p == '_' || *p == '-')) id[0] = 0;
            }
            if (!id[0]) continue;
            const char *name = row_h(t)[0] ? row_h(t) : row_i(t);
            int k = 0;
            while (k < C.n && strcmp(C.e[k].name, name)) k++;
            if (k == C.n) {
                if (C.n >= (int) (sizeof(C.e) / sizeof(C.e[0]))) continue;
                memset(&C.e[k], 0, sizeof(C.e[k]));
                snprintf(C.e[k].name, sizeof(C.e[k].name), "%s", name);
                snprintf(C.e[k].cm, sizeof(C.e[k].cm), "%s", id);
                C.e[k].tag = row_official(t) || meta_flag(t, "ds");
                C.n++;
            }
            C.e[k].players += tuple_int(t, 2);
        }
    }
    // customsInfo.sort((a, b) => (b.c || -1) - (a.c || -1)): most players first, stable like v8's sort
    for (int i = 1; i < C.n; i++) {
        __typeof__(C.e[0]) v = C.e[i];
        const int pv = v.players ? v.players : -1;
        int j = i - 1;
        while (j >= 0 && (C.e[j].players ? C.e[j].players : -1) < pv) {
            C.e[j + 1] = C.e[j];
            j--;
        }
        C.e[j + 1] = v;
    }
    if (C.n > CAROUSEL_MAX) C.n = CAROUSEL_MAX;
    if (C.n < 3) C.rot = 0;
    else C.rot %= C.n;
}

static void carousel_slide(StrBuf *sb, const int i, const int clone) {
    char name[96];
    size_t k = 0;
    for (const char *p = C.e[i].name; *p && k + 1 < sizeof(name); p++) {
        if (*p != '"' && *p != '\\') name[k++] = *p; // (searchList's argument stays one string)
    }
    name[k] = 0;
    sb_append(sb, clone ? "<div class='slide purejscarousel-slide purejscarousel-slide-clone'" : "<div class='slide purejscarousel-slide'");
    sb_append(sb, " onclick='windows[1].searchList(&quot;");
    sb_append_esc(sb, name);
    sb_appendf(sb, "&quot;)'><img class='slideImg' loading='lazy' src='%s/m%s/thumb.png' onerror='this.onerror=null;this.src=\"./img/noimg.png\";'><div class='slideName'>",
               user_assets_url(), C.e[i].cm);
    sb_append_esc(sb, C.e[i].name);
    sb_appendf(sb, "</div><div class='slidePlayers'><span class='material-icons'>person</span>%d</div>", C.e[i].players);
    if (C.e[i].tag) sb_append(sb, "<div class='slideTag'>24/7</div>");
    sb_append(sb, "</div>");
}

// the container's slides: clones, the slides, clones, each set starting at the rotation
static void carousel_slides_html(StrBuf *sb) {
    for (int set = 0; set < 3; set++) {
        for (int j = 0; j < C.n; j++) carousel_slide(sb, (j + C.rot) % C.n, set != 1);
    }
}

static void carousel_html(StrBuf *sb) {
    carousel_collect();
    C.busy = 0;
    if (C.n < 3) return;
    sb_appendf(sb, "<div class='carousel purejscarousel' id='carousel-infinite' style='margin-bottom:25px;'>"
                   "<div class=' purejscarousel-slides-container' style='margin-left: %dpx; width: %dpx;'>",
               -CAROUSEL_SLIDE_W * C.n, CAROUSEL_SLIDE_W * C.n * 3);
    carousel_slides_html(sb);
    sb_append(sb, "</div><div class=' purejscarousel-btn purejscarousel-btn-prev' data-is-native='0' onclick='knCarousel(-1)'>"
                  "<span class=\"material-icons plusOrMinus2\">navigate_before</span></div>"
                  "<div class=' purejscarousel-btn purejscarousel-btn-next' data-is-native='0' onclick='knCarousel(1)'>"
                  "<span class=\"material-icons plusOrMinus2\">navigate_next</span></div></div>");
}

// goToSlide's scrollEnd: the set rotated by the slides that went by, back at its rest position
static int g_carousel_dir;
static void carousel_scroll_end(void *user) {
    (void) user;
    HtmlElem box = html_dom_query(0, "#carousel-infinite .purejscarousel-slides-container");
    C.busy = 0;
    if (!box || C.n < 3) return;
    C.rot = ((C.rot + g_carousel_dir * CAROUSEL_STEP) % C.n + C.n) % C.n;
    StrBuf sb = {0};
    carousel_slides_html(&sb);
    char *html = sb_finish(&sb);
    html_dom_set_html(box, html);
    free(html);
    char margin[32];
    snprintf(margin, sizeof(margin), "%dpx", -CAROUSEL_SLIDE_W * C.n);
    html_dom_set_style(box, "transition", "");
    html_dom_set_style(box, "margin-left", margin);
}

// knCarousel(dir): goToNextSlide (1) / goToPrevSlide (-1); a click while one runs is ignored (setControl(false))
static void carousel_go(const int dir) {
    HtmlElem box = html_dom_query(0, "#carousel-infinite .purejscarousel-slides-container");
    if (!box || C.busy || C.n < 3) return;
    C.busy = 1;
    g_carousel_dir = dir < 0 ? -1 : 1;
    char margin[32];
    snprintf(margin, sizeof(margin), "%dpx", -CAROUSEL_SLIDE_W * C.n - g_carousel_dir * CAROUSEL_SLIDE_W * CAROUSEL_STEP);
    html_dom_set_style(box, "transition", "margin-left 400ms linear 0ms");
    html_dom_set_style(box, "margin-left", margin);
    if (!html_timer_add(CAROUSEL_SPEED_MS, carousel_scroll_end, NULL)) carousel_scroll_end(NULL);
}

// windows[1].genList()
static char *gen_list(void) {
    StrBuf sb = {0};
    sb_append(&sb, "<div class='serverHeader'><div class='setBodH' style='padding:10px;margin-top:0px;margin-bottom:8px;width:calc(100% - 20px) !important;margin-left:0px;'>");
    append_host_game_button(&sb);
    sb_append(&sb, "<div style='display:inline-block;'><input id='serverSearch' style='width:450px;margin-left:16px;margin-right:18px' type='text' placeholder='Search: FFA, SYD, Burg'");
    if (g_search[0]) {
        sb_append(&sb, " value='");
        sb_append_esc(&sb, g_search);
        sb_append(&sb, "'");
    }
    sb_append(&sb, " oninput='windows[1].searchList()'></input></div><div style='float:right;display:inline-block;'>");
    char *download = servers_ui_download_button_html(NULL); // the latest server build (ci-release)
    sb_append(&sb, download);
    free(download);
    sb_append(&sb, "<div id='hostMenuBtn' style='display:inline-block;vertical-align:middle' onclick='windows[0].tabIndex=0;showWindow(1);'><span class='material-icons' style='color:#fff;font-size:40px;margin-bottom:-1px;'>miscellaneous_services</span></div></div><div id=\"hideFull\" style=\"display:inline-block;float:right;margin-right:20px;margin-top:3px;\"><span style='color:#fff'>");
    sb_append(&sb, I18N("windows.servers.hide", "Hide Full"));
    sb_append(&sb, "</span><label class='switch2' style='margin-top: 4px;margin-left: 11px;'><input type='checkbox' onclick='toggleHF(this.checked);windows[1].searchList()'");
    if (hide_full()) sb_append(&sb, " checked");
    sb_append(&sb, "><span class='slider2'></span></label></div></div></div><div style='height:10px'></div>");
    append_finder_error(&sb);
    char *servers = get_servers();
    // the two tabs, one per backend (krunk.io, krunker.io); each shows the map carousel and the groups of its list
    const char *tabs[SERVER_TAB_COUNT] = {I18N("kn.servers.tabKrunk", "krunk.io"), I18N("kn.servers.tabKrunker", "krunker.io")};
    // srvTabs (krunknative.css) sets the tabs' padding and line-height: the label sits in the middle of each tab
    sb_append(&sb, "<div class='bigMenTabs srvTabs' style='margin-top:0px !important;margin-bottom:30px !important;'>");
    for (int i = 0; i < SERVER_TAB_COUNT; i++) sb_appendf(&sb, "<div class='bigMenTab%s' onclick='playSelect();windows[1].switchTab(%d)'>%s</div>", g_server_tab == i ? " bigMenTabA" : "", i, tabs[i]);
    sb_append(&sb, "</div>");
    carousel_html(&sb); // the tab's custom games' carousel (customsInfo.length >= 3)
    sb_append(&sb, "<div style='font-size:20px'><div id='serverHolder'>");
    sb_append(&sb, servers);
    sb_append(&sb, "</div>");
    free(servers);
    char total[32], social[32];
    // after get_servers: + finder-only; + the official krunker.io lobbies' players when they are listed
    // (v10's "In Game" is krunker.io's totalPlayerCount)
    const double players = (brain_games() ? g_total_players : 0) + g_finder_players + (krunker_lobbies_list() ? krunker_lobbies_players() : 0);
    comma_number(total, sizeof(total), players);
    comma_number(social, sizeof(social), g_social_players);
    char in_game[64], in_editor[64], on_social[64];
    I18N_F(in_game, "windows.servers.online", "{0} In Game", total);
    I18N_F(in_editor, "windows.servers.editor", "{0} in Editor", "0");
    I18N_F(on_social, "windows.servers.social", "{0} on Social", social);
    sb_appendf(&sb, "<div class='setBodH' style='padding-bottom:10px;margin-top:30px;padding-top:0px;margin-left:0px;width:calc(100%% - 40px);'><div style='height:10px'></div><b style='font-size:26px;color:rgba(255,255,255,0.8)'><span style='font-size:26px;color:rgba(255,255,255,0.8)' id='playerCnt'>%s</span><span style='float:right;font-size:26px;color:font-size:26px;color:rgba(255,255,255,0.6);position: absolute;left: 50%%;transform: translateX(-50%%);' id='editorCnt'>%s</span><span style='float:right;font-size:26px;color:rgba(255,255,255,0.6)' id='socialCnt'>%s</span></b></div>",
               in_game, in_editor, on_social);
    sb_append(&sb, "</div>");
    return sb_finish(&sb);
}

static void on_game_list(void *user, const int status, const char *body, const size_t length) {
    (void) user;
    (void) length;
    cJSON *res = status == 200 ? cJSON_Parse(body) : NULL;
    const cJSON *games = cJSON_GetObjectItem(res, "games");
    if (!cJSON_IsArray(games)) { // v10's loadData: the failed fetch never sets loadedServers; the window keeps its LOADING text
        printf("[mm] game list failed (HTTP %d): the server list stays loading until the next opening\n", status);
        cJSON_Delete(res);
        return;
    }
    if (cJSON_IsArray(games)) {
        servers_ui_note_list_response(res); // download_url / downloads (build versions)
        cJSON_Delete(g_games);
        g_games = cJSON_Duplicate(games, 1);
        const cJSON *total = cJSON_GetObjectItem(res, "totalPlayerCount");
        g_total_players = cJSON_IsNumber(total) ? total->valuedouble : 0;
        g_list_start_ms = now_ms(); // windows[1].startTime
        // the lobbies the Servers window lists (the Krunker Strike page has no such window; its
        // list data is still this one, from the app's flavor pool: mm_flavor_param)
        char ids[256] = "";
        size_t len = 0;
        const cJSON *g;
        int count = 0;
        cJSON_ArrayForEach(g, games) {
            const char *id = cJSON_GetStringValue(cJSON_GetArrayItem(g, 0));
            if (id && count++ < 4 && len + strlen(id) + 2 < sizeof(ids)) len += (size_t) snprintf(ids + len, sizeof(ids) - len, "%s%s", len ? "," : "", id);
        }
        printf("[mm] game list (%s): %d lobbies %s\n", flavor_current()->key, cJSON_GetArraySize(games), ids);
    }
    cJSON_Delete(res);
    g_loaded = 1;
    if (ui_window_key(2)) menu_rerender_if_open(2); // window.showWindow(2, true)
}

// v10 getRegionPing (loadData): every region of the ping list pinged at once, each round trip timed; the
// Servers window shows the new icons (windows[1].pingData, then showWindow(2, true) when it is open)
static void on_row_pings(void *user, cJSON *pings) {
    (void) user;
    cJSON_Delete(g_row_pings);
    g_row_pings = pings;
    printf("[mm] row pings measured: %d regions\n", cJSON_GetArraySize(pings));
    if (g_loaded) menu_rerender_if_open(2); // v10: only once the list has loaded (loadedServers)
}

static void on_row_ping_list(void *user, const int status, const char *body, const size_t length) {
    (void) user;
    (void) length;
    cJSON *list = status == 200 ? cJSON_Parse(body) : NULL;
    if (cJSON_IsObject(list)) region_ping_measure(list, on_row_pings, NULL); // no regions: the icons stay low
    cJSON_Delete(list);
}

static void on_player_count(void *user, const int status, const char *body, const size_t length) {
    (void) user;
    (void) length;
    cJSON *res = status == 200 ? cJSON_Parse(body) : NULL;
    const cJSON *count = cJSON_GetObjectItem(res, "playerCount");
    if (cJSON_IsNumber(count)) {
        g_social_players = count->valuedouble;
        if (g_loaded) menu_rerender_if_open(2);
    }
    cJSON_Delete(res);
}

static char *games_window_gen(void);

// loadData()'s loading text: "<div style='padding-top:19px;>" lacks its closing quote in v7.1.4, which
// swallows the text (a blank window while loading): fixed
static char *loading_html(void) {
    char loading[128];
    snprintf(loading, sizeof(loading), "<div style='padding-top:19px;'>%s</div>", I18N("generic.loading", "LOADING..."));
    return strdup(loading);
}

char *servers_window_gen(const int id, const char **header) {
    (void) header;
    if (id == 10) return games_window_gen();
    if (id == SERVERS_WINDOW_CONNECT) {
        if (header) *header = I18N("kn.connect.title", "Connect"); // KrunkNative window
        return servers_connect_window_gen();
    }
    if (id == MENU_WINDOW_HOST) return host_flow_window_gen(); // the Host Game window (host_flow_ui.c)
    if (id != 2) return NULL;
    server_tab_follow(); // the default tab: the active backend's (krunker_net_active)
    finder_refresh(); // the server finder (both modes), then the brain's own lobbies when there is one
    krunker_lobbies_refresh(0); // official lobbies (native, cached; listed whatever the server type)
    if (backend_available() != 1 || g_games_override) {
        // no brain: the finder's servers when there is a finder, else the empty list (what
        // v7 shows when client.fetchGameList() has no game)
        g_loaded = 1;
        return gen_list();
    }
    // gen() (v10 windows[1]): within serverBrowserRate the list is not asked again, the re-renders the
    // other answers cause included; while the first answer is in flight, the loading text
    if (now_ms() - g_last_load_ms < SERVER_BROWSER_RATE_MS) return g_loaded ? gen_list() : loading_html();
    g_loaded = 0;
    if (!mm_get("/mm/game-list?hostname=krunknative", on_game_list, NULL)) {
        // refused by the brain gate (krunker.io is the active backend: no brain request goes out, backend.h) or not
        // queued: no answer will come, so the list is what the finder has (tab_lists: the krunk.io tab's finder
        // servers), not LOADING. The brain's lobbies of an earlier load go too: they are not reachable from here.
        // The rate stamp is not set, so the next render asks again (the gate may be open by then).
        cJSON_Delete(g_games);
        g_games = NULL;
        g_loaded = 1;
        return gen_list();
    }
    g_last_load_ms = now_ms();
    mm_get("/mm/ping-list", on_row_ping_list, NULL); // v10 loadData's getRegionPing: the rows' ping icons
    backend_http_get("/api/player-count", on_player_count, NULL);
    return loading_html();
}

// setExpandedRegion / searchList: #serverHolder = getServers()
static void refresh_holder(void) {
    HtmlElem holder = html_dom_by_id("serverHolder");
    if (!holder) return;
    char *html = get_servers();
    html_dom_set_html(holder, html);
    free(html);
}

// windows[1].updateTimers (post, showServerTimer): every second each "Ends in" counts down.
static void servers_timer_tick(void) {
    const double now = now_ms();
    if (now - g_timer_tick_ms < 1000.0) return;
    g_timer_tick_ms = now;
    if (!show_timer() || menu_ui()->active_window != 2) return;
    HtmlElem cells[256];
    const int n = html_dom_query_all(0, "#serverHolder .serverTimeLeft", cells, 256);
    for (int i = 0; i < n; i++) {
        char v[32] = "";
        if (html_dom_get_attr(cells[i], "data-time-left", v, sizeof(v)) < 0) continue;
        int left = atoi(v); // parseInt
        if (left <= 0 && !strcmp(v, "0")) continue; // :not([data-time-left="0"])
        left--;
        if (left < 0) left = 0; // v7 counts below zero ("Ends in -1s") once a fractional start rounds to 0: clamped
        snprintf(v, sizeof(v), "%d", left);
        html_dom_set_attr(cells[i], "data-time-left", v);
        char text[96];
        time_left_text(text, sizeof(text), left);
        html_dom_set_text(cells[i], text);
    }
}

// the recent rank of quick join candidate i: a krunker.io lobby's own list (krunker_net.h)
static int quick_join_rank(const int i) {
    return g_quick_join_kio[i] ? krunker_net_recent_rank(g_quick_join[i]) : recent_lobbies_rank(&g_recent_games, g_quick_join[i]);
}

// quickJoinRegion(i): switchGame on the first joinable candidates (getServers(i)); a krunker.io
// candidate is joined through krunker_lobbies_open (in-client, or the browser), never seek()
void servers_quick_join_next(void) {
    while (g_quick_join_next < g_quick_join_count && g_quick_join_left > 0) {
        const int kio = g_quick_join_kio[g_quick_join_next];
        const char *id = g_quick_join[g_quick_join_next++];
        g_quick_join_left--;
        if (kio) { // an official krunker.io lobby: krunker_lobbies_open joins it in-client, or opens it in the browser
            const char *playing = krunker_net_game();
            if (playing[0] && !strcmp(id, playing)) continue; // the lobby it is in: no rejoin
            g_quick_join_left = 0;
            krunker_lobbies_open(id);
            return;
        }
        if (g_game_id[0] && !strcmp(id, g_game_id)) continue;
#ifdef __EMSCRIPTEN__
        switch_server(id); // reloads the page: only the first candidate matters
        return;
#else
        seek(id, 0, (void *) 1);
        return;
#endif
    }
    g_quick_join_left = 0;
    if (g_quick_join_kio_group) ui_error_popup(I18N("windows.servers.none", "No Joinable Games Found..."));
}

static void quick_join_region(const int index) {
    static ServerGroup groups[SERVER_GROUPS_MAX];
    const int count = build_groups(groups, SERVER_GROUPS_MAX);
    for (int i = 0; i < g_quick_join_count; i++) free(g_quick_join[i]);
    free(g_quick_join);
    free(g_quick_join_kio);
    g_quick_join = NULL;
    g_quick_join_kio = NULL;
    g_quick_join_count = g_quick_join_next = 0;
    if (index < 0 || index >= count) return;
    const ServerGroup *g = &groups[index];
    g_quick_join_kio_group = !strncmp(g->key, "kio:", 4);
    g_quick_join = calloc((size_t) g->game_count + 1, sizeof(char *));
    g_quick_join_kio = calloc((size_t) g->game_count + 1, sizeof(int));
    int brain_rows = 0;
    // the fullest joinable finder-only server (rows are sorted), one this client was not in lately
    // first (recent_lobbies.h)
    const cJSON *finder_pick = NULL;
    int finder_pick_rank = 0;
    for (int i = 0; g_quick_join && g_quick_join_kio && i < g->game_count; i++) {
        const cJSON *t = g->games[i];
        const int players = row_players(t), max_players = tuple_int(t, 3);
        if (g_search[0] && !search_matches(t)) continue;
        if (!meta_flag(t, "finder") && !row_krunker(t)) brain_rows++;
        if (meta_flag(t, "finder")) {
            const cJSON *meta = cJSON_GetArrayItem(t, 4);
            const char *host = cJSON_GetStringValue(cJSON_GetObjectItem(meta, "host"));
            char server[RECENT_LOBBY_ID_MAX];
            snprintf(server, sizeof(server), "%s:%d", host ? host : "", (int) cJSON_GetNumberValue(cJSON_GetObjectItem(meta, "port")));
            const int rank = recent_lobbies_rank(&g_recent_servers, server);
            // not recent (0) beats any recent one; among recent ones the older (higher rank) wins
            if (players < max_players && (!finder_pick || (finder_pick_rank && (!rank || rank > finder_pick_rank)))) {
                finder_pick = t;
                finder_pick_rank = rank;
            }
            continue;
        }
        if (!(players < max_players && players > 0)) continue; // regionID.games.filter(p < max && p > 0)
        const int krunker = row_krunker(t);
        if (krunker && meta_flag(t, "pw")) continue; // a password lobby is not joinable from the list
        const cJSON *id = cJSON_GetArrayItem(t, 0);
        if (cJSON_IsString(id)) {
            g_quick_join_kio[g_quick_join_count] = krunker;
            g_quick_join[g_quick_join_count++] = strdup(id->valuestring);
        }
    }
    // lobbies this client was in lately go last, the older ones before the newer (a stable reorder:
    // the list's order otherwise); the one it is in is skipped by servers_quick_join_next
    for (int rank = RECENT_LOBBIES_MAX; rank >= 1 && g_quick_join && g_quick_join_kio; rank--) {
        for (int i = 0, moved = 0; i + moved < g_quick_join_count;) {
            if (quick_join_rank(i) != rank) {
                i++;
                continue;
            }
            char *id = g_quick_join[i];
            const int kio = g_quick_join_kio[i];
            memmove(&g_quick_join[i], &g_quick_join[i + 1], sizeof(char *) * (size_t) (g_quick_join_count - i - 1));
            memmove(&g_quick_join_kio[i], &g_quick_join_kio[i + 1], sizeof(int) * (size_t) (g_quick_join_count - i - 1));
            g_quick_join[g_quick_join_count - 1] = id;
            g_quick_join_kio[g_quick_join_count - 1] = kio;
            moved++;
        }
    }
    if (g_quick_join_kio_group) { // official krunker.io lobbies: the first joinable one (servers_quick_join_next)
        if (!g_quick_join_count) {
            ui_error_popup(I18N("windows.servers.none", "No Joinable Games Found..."));
            return;
        }
        g_quick_join_left = 10;
        menu_show_window(MENU_WINDOW_NONE, 0);
        servers_quick_join_next();
        return;
    }
    if (!g_quick_join_count && finder_pick && !(brain_rows && backend_available() == 1 && strcmp(g->key, "custom"))) {
        // a region only finder-listed servers run in (or no brain): join the best one directly
        const cJSON *meta = cJSON_GetArrayItem(finder_pick, 4);
        direct_join(cJSON_GetStringValue(cJSON_GetObjectItem(meta, "host")), (int) cJSON_GetNumberValue(cJSON_GetObjectItem(meta, "port")));
        return;
    }
    if (!g_quick_join_count && strcmp(g->key, "custom") && strcmp(g->key, "official") && old_browser() && brain_rows && backend_available() == 1) {
        // no populated public lobby: seek the region (the brain picks or spawns one)
        snprintf(g_region, sizeof(g_region), "%s", g->key);
        menu_show_window(MENU_WINDOW_NONE, 0);
        seek(NULL, 1, NULL);
        return;
    }
    g_quick_join_left = 10;
    menu_show_window(MENU_WINDOW_NONE, 0);
    servers_quick_join_next();
}

// checkedSwitchServer(id, el) (v7 f1736): "..." then game-info; FULL when it is full (the
// count comes back after 1 s), ERROR when the request fails, else switch to it.
static char g_checked_id[160];
static char g_checked_count[32];  // "p/max" written back 1 s after FULL
static int g_checked_timer;

static HtmlElem checked_cell(void) {
    HtmlElem rows[256];
    const int n = html_dom_query_all(0, "#serverHolder .settNameIn", rows, 256);
    for (int i = 0; i < n; i++) {
        char onclick[512] = "";
        html_dom_get_attr(rows[i], "onclick", onclick, sizeof(onclick));
        char want[200];
        snprintf(want, sizeof(want), "checkedSwitchServer(\"%s\"", g_checked_id);
        if (strstr(onclick, want)) return html_dom_query(rows[i], ".serverPCount");
    }
    return 0;
}

static void checked_restore(void *user) {
    (void) user;
    g_checked_timer = 0;
    HtmlElem cell = checked_cell();
    if (cell && g_checked_count[0]) html_dom_set_text(cell, g_checked_count);
}

static void on_checked_info(void *user, const int status, const char *body, const size_t length) {
    (void) user;
    (void) length;
    cJSON *t = status == 200 ? cJSON_Parse(body) : NULL;
    HtmlElem cell = checked_cell();
    if (!cJSON_IsArray(t)) {
        if (cell) html_dom_set_text(cell, "ERROR");
    } else {
        snprintf(g_checked_count, sizeof(g_checked_count), "%d/%d", row_players(t), tuple_int(t, 3));
        if (tuple_int(t, 2) >= tuple_int(t, 3)) {
            if (cell) html_dom_set_text(cell, "FULL");
        } else {
            menu_show_window(MENU_WINDOW_NONE, 0);
            switch_server(g_checked_id);
        }
        if (g_checked_timer) html_timer_cancel(g_checked_timer);
        g_checked_timer = html_timer_add(1000.0, checked_restore, NULL);
    }
    cJSON_Delete(t);
}

// openServerWindow(n) (Server Finder, showWindow(48)): 0 Public / 1 Customs open the Servers
// window (Customs: the active backend's tab); 2 Raids /
// 3 Zombies / 4 Trade Plaza join the official krunker.io custom game of that mode with room
// (krunker_lobbies_find_custom, v10.js:218379-218415), "Couldnt find any" when there is none.
static void open_server_window(const int which) {
    if (which == 0 || which == 1) {
        menu_show_window(2, 0);
        if (which == 1) { // Customs: the active backend's tab (its customs are in its groups)
            g_server_tab = krunker_net_active() ? SERVER_TAB_KRUNKER : SERVER_TAB_KRUNK;
            g_expanded = -1;
            menu_rerender_if_open(2);
        }
        return;
    }
    const int mode = which == 2 ? MODE_ID_RAID : which == 3 ? MODE_ID_ZOM : which == 4 ? MODE_ID_TRADE : -1;
    if (mode < 0) return;
    ui_loading_popup(I18N("kn.finder.searching", "Searching...")); // v10: iîíïíîî('Searching...'), closed by the outcome
    krunker_lobbies_find_custom(mode);
}

static void games_load_more(const HtmlEvent *ev);

static void games_search(const HtmlArg *args, int argc);

int servers_ui_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    if (!name) return -1;
    if (!strcmp(name, "downloadServer")) { // the latest server build (servers_ui_open_download)
        servers_ui_open_download();
        return 1;
    }
    if (!strcmp(name, "openConnectWindow")) {
        menu_show_window(SERVERS_WINDOW_CONNECT, 0);
        HtmlElem input = html_dom_by_id("connectAddr");
        if (input) html_dom_focus(input);
        return 1;
    }
    if (!strcmp(name, "connectServer")) {
        char text[256] = "";
        HtmlElem input = html_dom_by_id("connectAddr");
        if (argc > 0 && args[0].kind == HTML_ARG_STR && args[0].str) snprintf(text, sizeof(text), "%s", args[0].str);
        else if (input) html_dom_get_value(input, text, sizeof(text));
        connect_server(text);
        return 1;
    }
    if (!strcmp(name, "krunkerJoin")) { // an official krunker.io lobby: joined here (or the browser opt-out)
        if (argc > 0 && args[0].kind == HTML_ARG_STR) krunker_lobbies_open(args[0].str);
        return 1;
    }
    if (!strcmp(name, "finderJoin")) {
        if (argc > 1 && args[0].kind == HTML_ARG_STR && args[1].kind == HTML_ARG_NUM) {
            direct_join(args[0].str, (int) args[1].num);
        }
        return 1;
    }
    if (!strcmp(name, "openServerWindow")) {
        open_server_window(argc > 0 && args[0].kind == HTML_ARG_NUM ? (int) args[0].num : 0);
        return 1;
    }
    if (!strcmp(name, "setExpandedRegion")) {
        g_expanded = argc > 0 && args[0].kind == HTML_ARG_NUM ? (int) args[0].num : -1;
        g_last_load_ms = now_ms();
        refresh_holder();
        return 1;
    }
    if (!strcmp(name, "windows.1.switchTab")) {
        const int tab = argc > 0 && args[0].kind == HTML_ARG_NUM ? (int) args[0].num : 0;
        g_server_tab = tab >= 0 && tab < SERVER_TAB_COUNT ? tab : SERVER_TAB_KRUNK;
        g_expanded = -1;
        menu_rerender_if_open(2); // updateWindow(2)
        return 1;
    }
    if (!strcmp(name, "windows.1.hostGame")) { // + Host Game: the Host Game window on the tab's backend
        host_flow_open_backend(GAME_HOST_CUSTOM, g_server_tab == SERVER_TAB_KRUNKER);
        return 1;
    }
    if (!strcmp(name, "knCarousel")) { // the carousel's prev / next (PureJSCarousel's buttons)
        carousel_go(argc > 0 && args[0].kind == HTML_ARG_NUM ? (int) args[0].num : 1);
        return 1;
    }
    if (!strcmp(name, "windows.1.searchList")) {
        HtmlElem input = html_dom_by_id("serverSearch");
        g_search[0] = 0;
        if (argc > 0 && args[0].kind == HTML_ARG_STR && args[0].str) {
            snprintf(g_search, sizeof(g_search), "%s", args[0].str);
            if (input) html_dom_set_value(input, g_search);
            g_expanded = strcmp(g_search, "Krunk_Plaza") ? 1 : 0; // searchList(text): expandedRegion = text == "Krunk_Plaza" ? 0 : 1
        } else if (input) {
            html_dom_get_value(input, g_search, sizeof(g_search));
        }
        refresh_holder();
        return 1;
    }
    if (!strcmp(name, "toggleHF")) {
        g_hide_full = ev ? ev->checked : 0;
        if (argc > 0 && (args[0].kind == HTML_ARG_BOOL || args[0].kind == HTML_ARG_NUM)) g_hide_full = args[0].num != 0;
        html_storage_set("krk_hideFull", g_hide_full ? "true" : "false");
        return 1;
    }
    if (!strcmp(name, "quickJoinRegion")) {
        quick_join_region(argc > 0 && args[0].kind == HTML_ARG_NUM ? (int) args[0].num : 0);
        return 1;
    }
    if (!strcmp(name, "windows.9.loadMore")) {
        games_load_more(ev);
        return 1;
    }
    const int host_config = ui_host_config_handle(name, ev, args, argc); // WP-E windows.7.* rows, slider labels
    if (host_config >= 0) return host_config;
    if (!strcmp(name, "windows.7.switchTab")) {
        host_flow_switch_tab(argc > 0 && args[0].kind == HTML_ARG_NUM ? (int) args[0].num : 0);
        return 1;
    }
    if (!strcmp(name, "favoriteM") || !strcmp(name, "windows.9.searchMaps")) {
        if (name[0] == 'w') games_search(args, argc);
        return 1; // favoriteM: favourites need an account API the brain does not have
    }
    if (!strcmp(name, "checkedSwitchServer") || !strcmp(name, "switchServer")) {
        const char *id = argc > 0 && args[0].kind == HTML_ARG_STR ? args[0].str : NULL;
        if (!id) return 1;
        snprintf(g_checked_id, sizeof(g_checked_id), "%s", id);
        if (!strcmp(name, "switchServer") || backend_available() != 1) {
            menu_show_window(MENU_WINDOW_NONE, 0);
            switch_server(id);
            return 1;
        }
        HtmlElem row = argc > 1 && args[1].kind == HTML_ARG_THIS ? (HtmlElem) args[1].num : ev ? ev->target : 0;
        HtmlElem cell = row ? html_dom_query(row, ".serverPCount") : 0;
        if (cell) html_dom_set_text(cell, "...");
        char path[256], enc[192];
        backend_url_encode(enc, sizeof(enc), id);
        snprintf(path, sizeof(path), "/mm/game-info?game=%s", enc);
        mm_get(path, on_checked_info, NULL);
        return 1;
    }
    return -1;
}

// ------------------------------------------------------------------ windows[9] (Custom Games)

#define GAMES_TABS 8
static struct {
    cJSON *rows;      // accumulated rows (owned)
    int requested;    // listMaps sent
    int waiting;      // a page is in flight
    int pos;          // last requested page (loadInd)
    int more;         // the last page was full (42 rows): Load More
    int failed;       // the first page did not arrive (brain unreachable): the catalogue instead
} g_games_tabs[GAMES_TABS];

static void on_maps(void *user, const int status, const char *body, const size_t length) {
    (void) length;
    const int tab = (int) (intptr_t) user;
    if (tab < 0 || tab >= GAMES_TABS) return;
    g_games_tabs[tab].waiting = 0;
    cJSON *res = status == 200 ? cJSON_Parse(body) : NULL;
    const cJSON *data = cJSON_GetObjectItem(res, "data");
    if (!cJSON_IsArray(data) && !g_games_tabs[tab].rows) {
        // KrunkNative: no answer at all: the map catalogue, like without a brain
        g_games_tabs[tab].failed = 1;
        cJSON_Delete(res);
        menu_rerender_if_open(10);
        return;
    }
    if (!g_games_tabs[tab].rows) g_games_tabs[tab].rows = cJSON_CreateArray();
    int n = 0;
    const cJSON *r;
    cJSON_ArrayForEach(r, data) {
        cJSON_AddItemToArray(g_games_tabs[tab].rows, cJSON_Duplicate(r, 1));
        n++;
    }
    g_games_tabs[tab].more = n >= 42; // TAB_PAGE_SIZE
    cJSON_Delete(res);
    menu_rerender_if_open(10); // mapsLoaded -> updateWindow(10)
}

static void games_request(const int tab) {
    char path[160];
    const AccountInfo *acc = active_account();
    int n = snprintf(path, sizeof(path), "/api/maps?index=%d&pos=%d", tab, g_games_tabs[tab].pos);
    if (acc) snprintf(path + n, sizeof(path) - (size_t) n, "&accountId=%d", acc->id);
    g_games_tabs[tab].waiting = 1;
    if (!backend_http_get(path, on_maps, (void *) (intptr_t) tab)) g_games_tabs[tab].waiting = 0;
}

int servers_games_tab_rows(const int tab, const cJSON **rows) {
    if (backend_available() != 1 || tab < 0 || tab >= GAMES_TABS || g_games_tabs[tab].failed) return -1;
    if (!g_games_tabs[tab].requested) {
        g_games_tabs[tab].requested = 1;
        g_games_tabs[tab].pos = 0;
        games_request(tab);
    }
    if (!g_games_tabs[tab].rows) return 0;
    *rows = g_games_tabs[tab].rows;
    return 1;
}

int servers_games_tab_more(const int tab) {
    return tab >= 0 && tab < GAMES_TABS && g_games_tabs[tab].more && !g_games_tabs[tab].waiting;
}

// ---- windows[9].gen() (v7.1.4, config.mapTabs): Home / Popular / Featured / New /
// Favorites / My Games / Search tabs plus the Editor and Sandbox shortcuts, and the f1825
// ("map") cards. Rows: the brain's GET /api/maps?index=<n> (its v3.1.1 tab indices: 0
// Popular, 1 Featured, 2 New, 3 Favorites, 4 My Games), else the map catalogue as the v3.1.1
// offline server lists it (map_catalog_games_tab). KrunkNative differences, each noted at its
// line: deterministic Home order (v7 shuffles), catalogue thumbnails for official maps, Play
// = playMap (v7 reloads the page on /?play=), Sandbox = the offline game (knHostOffline).
typedef struct {
    const char *n, *txt, *click;
    int home, search, fav, owned, paged, data; // data: the brain/catalogue tab index, -1 none
} GamesTab;

static const GamesTab k_games_tabs[] = {
    {"Home", "<span class='material-icons material-icons-outlined' style='color:#fbc02d;font-size:42px;vertical-align:bottom'>home</span>", NULL, 1, 0, 0, 0, 0, 0},
    {"Popular", NULL, NULL, 0, 0, 0, 0, 1, 0},
    {"Featured", NULL, NULL, 0, 0, 0, 0, 0, 1},
    {"New", NULL, NULL, 0, 0, 0, 0, 1, 2},
    {"Favorites", NULL, NULL, 0, 0, 1, 0, 0, 3},
    {"My Games", NULL, NULL, 0, 0, 0, 1, 0, 4},
    {"Search", "<span class='material-icons' style='color:#ffffff;font-size:42px;vertical-align:bottom'>search</span>", NULL, 0, 1, 0, 0, 0, -1},
    {"Editor", "<span class='material-icons' style='color:#ffffff;font-size:44px;vertical-align:bottom'>construction</span>Editor", "openURL(`./editor.html`)", 0, 0, 0, 0, 0, -1},
    // v7: openURL(`./?sandbox`) reloads into its in-browser sandbox; KrunkNative's is the offline game
    {"Sandbox", "<span class='material-icons' style='color:#ffffff;font-size:44px;vertical-align:bottom'>castle</span>Sandbox", "knHostOffline()", 0, 0, 0, 0, 0, -1},
};
#define GAMES_TAB_COUNT ((int) (sizeof(k_games_tabs) / sizeof(k_games_tabs[0])))

static const char *const k_map_cats[] = {"Other", "Parkour", "Puzzle", "Trading", "Training", "Social", "Horror", "Adventure",
    "Battle Royale", "Realism", "Roleplay", "Rotation", "Racing", "Raid", "Remake", "NCR", "Zombies", "Simulation", "Relaxation",
    "Action", "Tycoon", "Mini-Games", "Single-player", "Education"}; // config.mapCats
static const struct { const char *n; int i; } k_map_cats_p[] = { // config.mapCatsP
    {"Action", 1}, {"Parkour", 8}, {"Remake", 4}, {"Zombies", 7}, {"Horror", 0}, {"Realism", 12}, {"Mini-Games", 5},
};

static int g_games_tab;           // windows[9].tabIndex (v7 index)
static char g_games_search[96];   // the Search tab's last query ("" = none yet)
static int g_games_search_cat = -1;

void ui_games_switch_tab(const int tab) {
    if (tab < 0 || tab >= GAMES_TAB_COUNT || tab == g_games_tab) return; // switchTab: only a different tab
    g_games_tab = tab;
    menu_rerender_if_open(10);
}

static void games_load_more(const HtmlEvent *ev) {
    const int data = k_games_tabs[g_games_tab].data;
    if (data < 0 || !g_games_tabs[data].requested || g_games_tabs[data].waiting) return;
    if (ev && ev->target) {
        html_dom_set_attr(ev->target, "class", "loadMoreW");
        html_dom_set_html(ev->target, "Loading...");
    }
    g_games_tabs[data].pos++;
    games_request(data);
}

// windows[9].playerData[map]: players on that map in the brain's game list
static int games_map_players(const char *map) {
    int n = 0;
    const cJSON *t;
    cJSON_ArrayForEach(t, brain_games()) {
        if (!strcmp(meta_str(t, "map"), map)) n += row_players(t);
    }
    return n;
}

// window.fetchMatches(cb) (v10:215860): GET /game-list, then {map: players} over the custom games
// (data.c: the key is data.i, the map; each game counts min(players, max)), {} when the request
// fails. A fixed list (servers_ui_set_games: scenarios, KRUNKNATIVE_SERVER_LIST) answers at once.
typedef struct {
    ServersMatchesFn fn;
    void *user;
} MatchesReq;

static void matches_add(cJSON *out, const cJSON *games) {
    const cJSON *t;
    cJSON_ArrayForEach(t, games) {
        if (!row_custom(t)) continue;
        const char *map = row_i(t);
        cJSON *have = cJSON_GetObjectItemCaseSensitive(out, map);
        if (have) cJSON_SetNumberValue(have, have->valuedouble + row_players(t));
        else cJSON_AddNumberToObject(out, map, row_players(t));
    }
}

// v10 fetchMatches (v10:215632): the custom games of the game list by map (i), their players summed
// (min(players, max)). KrunkNative: ours, then krunker.io's official custom games (krunker_lobbies.h,
// native), the list v10 itself reads; a name on both sums both.
static cJSON *matches_of(const cJSON *games) {
    cJSON *out = cJSON_CreateObject();
    matches_add(out, games);
    matches_add(out, krunker_lobbies_list());
    return out;
}

static void on_matches(void *user, const int status, const char *body, const size_t length) {
    (void) length;
    MatchesReq *req = user;
    cJSON *res = status == 200 && body ? cJSON_Parse(body) : NULL;
    cJSON *matches = matches_of(cJSON_GetObjectItem(res, "games"));
    req->fn(req->user, matches);
    cJSON_Delete(matches);
    cJSON_Delete(res);
    free(req);
}

void servers_ui_fetch_matches(const ServersMatchesFn fn, void *user) {
    if (!fn) return;
    if (g_games_override || backend_available() != 1) {
        cJSON *matches = matches_of(g_games_override ? g_games : NULL);
        fn(user, matches);
        cJSON_Delete(matches);
        return;
    }
    MatchesReq *req = malloc(sizeof(*req));
    if (!req) return;
    *req = (MatchesReq) {fn, user};
    if (!mm_get("/mm/game-list?hostname=krunknative", on_matches, req)) {
        free(req);
        cJSON *matches = cJSON_CreateObject();
        fn(user, matches);
        cJSON_Delete(matches);
    }
}

typedef struct {
    const char *name, *creator, *mod_url, *image, *category, *friendname;
    char id[24];
    double votes, plays;
    int fav;
    const cJSON *modes; // the row's game mode ids (kn_brain: map_info / the catalogue map's "modes")
    double age_days;    // days since map_date (v10's mapAgeD badge), -1 unknown
    int updates;        // map_updatecounter
} GameCard;

// map_date (ISO 8601, "2026-10-15T10:00:00Z" / "2026-10-15 10:00:00+00") -> days before now, -1
// when it does not parse (v10: (Date.now() - new Date(map_date)) / 86400000)
static double days_since(const char *date) {
    int y, mo, d, h = 0, mi = 0, sec = 0;
    if (!date || sscanf(date, "%d-%d-%d%*[T ]%d:%d:%d", &y, &mo, &d, &h, &mi, &sec) < 3) return -1;
    // days from civil (proleptic Gregorian, UTC), Howard Hinnant's algorithm
    y -= mo <= 2;
    const long era = (y >= 0 ? y : y - 399) / 400;
    const long yoe = y - era * 400, doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const long days = era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
    const double then = (double) days * 86400.0 + h * 3600.0 + mi * 60.0 + sec;
    const double age = ((double) time(NULL) - then) / 86400.0;
    return age >= 0 ? age : 0;
}

// UTILS.formatNumYT: k = floor(digits / 3) thousands; the value at 2 significant digits, else 1,
// whichever leaves at most two digits; one decimal when fractional ("1.2k", "12k", "0.1m").
static void format_num_yt(char *out, const size_t n, const double v) {
    static const char *const suffix[] = {"", "k", "m", "b", "t"};
    if (!(v >= 1000)) {
        snprintf(out, n, "%.15g", v > 0 ? v : 0);
        return;
    }
    char whole[40];
    snprintf(whole, sizeof(whole), "%.15g", v);
    int k = (int) (strlen(whole) / 3);
    if (k > 4) k = 4;
    const double scaled = v / pow(1000.0, k);
    double r = scaled;
    for (int p = 2; p >= 1; p--) {
        char buf[40];
        snprintf(buf, sizeof(buf), "%.*g", p, scaled); // toPrecision(p) -> parseFloat
        r = atof(buf);
        snprintf(buf, sizeof(buf), "%.15g", r);
        size_t d = 0;
        for (const char *q = buf; *q; q++) d += isalnum((unsigned char) *q) != 0; // digits left after the punctuation
        if (d <= 2) break;
    }
    if (r != floor(r)) snprintf(out, n, "%.1f%s", r, suffix[k]);
    else snprintf(out, n, "%.15g%s", r, suffix[k]);
}

// f1825(data, "map", tabIndex) for one row
static void games_card(StrBuf *sb, const GameCard *c, const int tab) {
    const int fav = c->fav || k_games_tabs[tab].fav;
    char plays[32], online[32];
    format_num_yt(plays, sizeof(plays), c->plays);
    snprintf(online, sizeof(online), "%d", games_map_players(c->name));
    sb_append(sb, "<div style='display:inline-block;'><div class='mapListItem'><div class='mapActionOvrl'><div class='mapActionHol'><div class='mapActionB' title='Host' onclick='selectHostMap(&quot;");
    sb_append_esc(sb, c->name);
    sb_append(sb, "&quot;,&quot;");
    sb_append_esc(sb, c->mod_url);
    sb_appendf(sb, "&quot;,&quot;%s&quot;,&quot;", c->id);
    sb_append_esc(sb, c->creator);
    sb_append(sb, "&quot;,0)'><span class='material-icons' style='font-size:70px;color:#fff;'>video_settings</span></div><div class='mapActionSep'></div>");
    // v7: openURL("/?play=<name>", 1) reloads the page into autoHostMap; KrunkNative plays in place
    sb_append(sb, "<div class='hideOnHost mapActionB' onclick='playMap(&quot;");
    sb_append_esc(sb, c->name);
    sb_append(sb, "&quot;)' title='Play' style='margin-top:-5px;'><span class='material-icons' style='font-size:80px;color:#fff;'>play_arrow</span></div><div class='mapActionSep' style='margin-left:2px'></div>");
    sb_appendf(sb, "<div class='mapActionB%s'onclick='favoriteM(0,this, %s);'>%s</div></div></div>", fav ? "" : " sel", c->id,
               fav ? "<span class='material-icons' style='font-size:70px;color:#fff;'>star</span>" : "<span class='material-icons' style='font-size:70px;color:#fff;'>star_border</span>");
    // v10: a map published or updated in the last 7 days wears the NEW! / UPDATED badge
    if (c->age_days >= 0 && c->age_days <= 7) {
        sb_appendf(sb, "<div class='mapAgeD' style='background-image:%s'>%s</div>",
                   c->updates > 1 ? "linear-gradient(to right,#E040FB,#6240fb);" : "linear-gradient(to right,#fb9c40,#fb4040);",
                   c->updates > 1 ? "UPDATED" : "NEW!");
    }
    sb_append(sb, "<div class='mapsVotes' title='Players Online' id='pcnt_");
    sb_append_esc(sb, c->name);
    // KrunkNative: the map's game modes as the thumbnail's tooltip (v10's card has no mode tags;
    // a title keeps the card's look v10's)
    char modes[256];
    community_mode_names(modes, sizeof(modes), c->modes);
    sb_appendf(sb, "' style='left:10px;right:unset'>%s<span style='color:#999999;'> online</span></div><div style='height:200px;width:100%%;overflow: hidden;'", online);
    if (modes[0]) {
        sb_append(sb, " title='");
        sb_append_esc(sb, modes);
        sb_append(sb, "'");
    }
    sb_append(sb, "><img class='mapListThumb' loading='lazy' src='");
    sb_append_esc(sb, c->image && c->image[0] ? c->image : "./img/noimg.png");
    sb_append(sb, "' /></div><div style='margin-top:5px;padding-left:5px;padding-right:5px;line-height:30px;margin-bottom:0px;'><div style='display:inline-block;max-width:250px;overflow:hidden;white-space:nowrap'><a href='javascript:;' onclick='openURL(&quot;https://krunker.io/social.html?q=");
    sb_append_esc(sb, c->name);
    sb_append(sb, "&p=map&quot;)' />");
    sb_append_esc(sb, c->name);
    sb_appendf(sb, "</a></div><span style='font-size:15px;text-align:right;padding-right:2px;float:right;color:#fff;'>%s <span style='color:rgba(255,255,255,0.5)'>plays</span></span></div></div>", plays);
    if (c->friendname && c->friendname[0]) {
        sb_append(sb, "<div class='likdByFrnd'><span class='material-icons' style='color:#fff;font-size:23px;vertical-align:middle;margin-right:6px;margin-top:-4px;'>thumb_up</span>liked by <span style='color:rgba(255,255,255,0.6);'>");
        sb_append_esc(sb, c->friendname);
        sb_append(sb, "</span></div>");
    } else {
        sb_append(sb, "<div style='height:10px;'></div>");
    }
    sb_append(sb, "</div>");
}

static void card_from_brain(GameCard *c, const cJSON *r) {
    memset(c, 0, sizeof(*c));
    c->name = cJSON_GetStringValue(cJSON_GetObjectItem(r, "map_name"));
    if (!c->name) c->name = cJSON_GetStringValue(cJSON_GetObjectItem(r, "mna"));
    if (!c->name) c->name = "";
    c->creator = cJSON_GetStringValue(cJSON_GetObjectItem(r, "creatorname"));
    if (!c->creator) c->creator = "";
    c->mod_url = cJSON_GetStringValue(cJSON_GetObjectItem(r, "mod_url"));
    if (!c->mod_url) c->mod_url = "";
    c->image = cJSON_GetStringValue(cJSON_GetObjectItem(r, "map_image"));
    c->category = cJSON_GetStringValue(cJSON_GetObjectItem(r, "category"));
    c->friendname = cJSON_GetStringValue(cJSON_GetObjectItem(r, "friendname"));
    const cJSON *id = cJSON_GetObjectItem(r, "map_id");
    snprintf(c->id, sizeof(c->id), "%.0f", cJSON_IsNumber(id) ? id->valuedouble : 0.0);
    c->votes = cJSON_GetNumberValue(cJSON_GetObjectItem(r, "map_votes"));
    if (c->votes != c->votes) c->votes = 0;
    c->plays = cJSON_GetNumberValue(cJSON_GetObjectItem(r, "map_pl"));
    if (c->plays != c->plays) c->plays = 0;
    c->fav = cJSON_IsTrue(cJSON_GetObjectItem(r, "fav"));
    c->modes = cJSON_GetObjectItem(r, "modes");
    c->age_days = days_since(cJSON_GetStringValue(cJSON_GetObjectItem(r, "map_date")));
    c->updates = (int) cJSON_GetNumberValue(cJSON_GetObjectItem(r, "map_updatecounter"));
    if (c->updates != c->updates) c->updates = 0;
    // KrunkNative: the official maps' own thumbnail (v7's listMaps gives them a map_image too)
    const int index = !c->image || !c->image[0] ? map_catalog_find(c->name) : -1;
    static char thumb[8][48];
    static int next;
    if (index >= 0 && !strcmp(map_catalog_at((size_t) index)->name, c->name) && map_catalog_thumb(map_catalog_at((size_t) index), thumb[next], sizeof(thumb[0]))) {
        c->image = thumb[next];
        next = (next + 1) % 8;
    }
}

static void card_from_catalogue(GameCard *c, const MapCatalogEntry *e, char *thumb, const size_t n) {
    memset(c, 0, sizeof(*c));
    c->age_days = -1; // no mode tags without kn_brain: the catalogue does not load every map for a card
    c->name = e->name;
    c->creator = e->creator;
    c->mod_url = e->mod_url;
    c->image = map_catalog_thumb(e, thumb, n);
    snprintf(c->id, sizeof(c->id), "%u", (unsigned) map_catalog_map_id(e));
}

// The tab's cards; returns -1 while its first page loads, else the number of cards.
static int games_cards(StrBuf *sb, const int tab) {
    const int data = k_games_tabs[tab].data;
    const int limit = k_games_tabs[tab].home ? 8 : 1 << 30; // Home: at most 8 per category (4 when 4..7)
    const cJSON *rows = NULL;
    const int brain = servers_games_tab_rows(data, &rows);
    if (brain == 0) return -1;
    int count = 0;
    if (brain > 0) {
        int total = cJSON_GetArraySize(rows);
        if (k_games_tabs[tab].home && total > limit) total = limit;
        if (k_games_tabs[tab].home && total < 8 && total >= 4) total = 4;
        const char *category = NULL;
        const cJSON *r;
        cJSON_ArrayForEach(r, rows) {
            if (count >= total) break;
            GameCard c;
            card_from_brain(&c, r);
            if (c.category && c.category[0] && (!category || strcmp(category, c.category))) {
                category = c.category;
                sb_append(sb, "<div class='mapListCat'>");
                sb_append_esc(sb, category);
                sb_append(sb, "</div>");
            }
            games_card(sb, &c, tab);
            count++;
        }
        return count;
    }
    int order[96];
    size_t n = map_catalog_games_tab(data, order, sizeof(order) / sizeof(order[0]));
    if (k_games_tabs[tab].home && n > (size_t) limit) n = (size_t) limit;
    if (k_games_tabs[tab].home && n < 8 && n >= 4) n = 4;
    for (size_t i = 0; i < n; i++) {
        GameCard c;
        char thumb[48];
        card_from_catalogue(&c, map_catalog_at((size_t) order[i]), thumb, sizeof(thumb));
        games_card(sb, &c, tab);
        count++;
    }
    return count;
}

// Home: "Popular Categories" after the cards
static void games_popular_categories(StrBuf *sb) {
    sb_append(sb, "<div class='mapListCat'>Popular Categories</div>");
    for (size_t i = 0; i < sizeof(k_map_cats_p) / sizeof(k_map_cats_p[0]); i++) {
        int cat = 0;
        for (size_t k = 0; k < sizeof(k_map_cats) / sizeof(k_map_cats[0]); k++) {
            if (!strcmp(k_map_cats[k], k_map_cats_p[i].n)) cat = (int) k;
        }
        sb_appendf(sb, "<div class='mapListItem' style='width:624px;height:130px;'><div class='mapLICat' onclick='windows[9].switchTab(7);mpCat.selectedIndex=%d;windows[9].searchMaps();' style='background:url(img/backgrounds/gamecats/c_%d.png)'>%s</div></div>",
                   cat + 1, k_map_cats_p[i].i, k_map_cats_p[i].n);
    }
}

static char *games_window_gen(void) {
    const GamesTab *tab = &k_games_tabs[g_games_tab];
    StrBuf sb = {0};
    sb_append(&sb, "<div style='margin-top:15px;margin-bottom:15px'><div class='bigMenTabs' style='margin-top:0px'>");
    for (int i = 0; i < GAMES_TAB_COUNT; i++) {
        sb_appendf(&sb, "<div class='bigMenTab%s' onclick='playSelect();", g_games_tab == i ? " bigMenTabA" : "");
        if (k_games_tabs[i].click) sb_append(&sb, k_games_tabs[i].click);
        else sb_appendf(&sb, "windows[9].switchTab(%d)", i);
        sb_appendf(&sb, "' onmouseenter='playTick(0.1);'>%s</div>", k_games_tabs[i].txt ? k_games_tabs[i].txt : k_games_tabs[i].n);
    }
    sb_append(&sb, "</div></div>");
    if (tab->home) {
        // the featured game (v12130) is backend data KrunkNative does not have: no feature panel
        sb_append(&sb, "<div class='bigMFeatHold' style='margin-left:20px;background:#0b0b0b;display:inline-block;width:580px;vertical-align:top;'><div class='biMFeatInfH'>Create your own game & earn real money! Over $750,000 USD has been paid out so far!</div><div class='biMFeatInfB' onclick='openURL(`./editor.html`)'>Open Editor</div><div class='biMFeatInfB' onclick='openURL(`./games`)'>Discover Games</div><div class='biMFeatInfB' onclick='openURL(`https://discord.com/invite/Kfypyp5`)'>Dev Community</div></div>");
    }
    if (tab->owned) {
        // v10.js 213312: the editor rows (canShowKrunkerEngine), then Studios with its pending invite
        // count (window.setStudioInviteCount) and Manage (openStudios, v10_studios.c); the invites
        // are asked for at most every 30 s. v10 has no Open Dashboard row any more.
        v10_studios_poll_invites();
        const int invites = v10_studios_invite_count();
        sb_append(&sb, "<div class='setBodH'><div class='settName'>Publish or create a game<a style='float:right' href='./editor.html' target='_blank'>Open Editor</a></div>"
                       "<div class='settName'>Learn how to make a game<a style='float:right' href='//docs.krunker.io' target='_blank'>Open Documentation</a></div>"
                       "<div class='settName'>");
        sb_append(&sb, I18N("studios.title", "Studios"));
        sb_appendf(&sb, "<span id='studioInviteCount' class='studioInviteCount' style='display:%s'>%d</span><a style='float:right' href='javascript:;' "
                        "onclick='openStudios()' id='knStudiosOpen'>",
                   invites ? "inline-block" : "none", invites);
        sb_append(&sb, I18N("studios.open", "Manage"));
        sb_append(&sb, "</a></div></div>");
    }
    sb_append(&sb, "<div id='mapList'>");
    if (tab->search) {
        sb_append(&sb, "<input type='text' id='mpSrch' class='smlInput' placeholder='Game Name or Keyword' onkeydown='if(window.event.keyCode==13)windows[9].searchMaps()' /><select id='mpCat'><option value='-1'>All</option>");
        for (size_t i = 0; i < sizeof(k_map_cats) / sizeof(k_map_cats[0]); i++) sb_appendf(&sb, "<option value='%d'>%s</option>", (int) i, k_map_cats[i]);
        // KrunkNative: the Official / Community split (kn_brain's source=) as the last two options:
        // v10 lists only its players' games here, KrunkNative's catalogue also holds Krunker's own
        // maps; after the categories, so Popular Categories' mpCat.selectedIndex stays v10's
        sb_append(&sb, "<option value='official'>Official</option><option value='community'>Community</option>");
        sb_append(&sb, "</select><div class='searchBtn' onclick='windows[9].searchMaps()'><span class='material-icons' style='color:#fff;font-size:45px;margin-bottom:-30px;line-height:75px'>search</span></div><div style='color:rgba(0,0,0,0.5);margin-top:10px' id='searchResultsMA'></div>");
    } else {
        StrBuf cards = {0};
        const int n = games_cards(&cards, g_games_tab);
        if (n < 0) {
            sb_append(&sb, "<div class='loadMoreW'>Loading...</div>");
        } else if (!n && !tab->home) {
            sb_append(&sb, I18N("windows.maps.none", "No Games found!"));
        } else {
            sb_append(&sb, cards.data ? cards.data : "");
            if (tab->home) games_popular_categories(&sb);
            const int data = tab->data;
            if (data >= 0 && g_games_tabs[data].waiting) sb_append(&sb, "<div class='loadMoreW'>Loading...</div>");
            else if (tab->paged && (backend_available() != 1 || servers_games_tab_more(data))) sb_append(&sb, "<div class='loadMoreD' onclick='windows[9].loadMore(this)'>Load More</div>");
        }
        sb_free(&cards);
    }
    sb_append(&sb, "</div>");
    return sb_finish(&sb);
}

// windows[9].searchMaps() (v10): searchResultsMA = "Loading...", API_CLIENT.searchMap(mpSrch.value,
// mpCat's value) -> searchResp: the cards or "No Games found!". kn_brain's /api/search answers it
// (name, category, and KrunkNative's Official / Community split: mpCat's last two options); without
// a brain the catalogue and the loaded rows are searched by name (and category) here. Script /
// scenario arguments (query, category, "official" | "community") are put into the fields first.
static unsigned g_search_gen; // the latest search: an older answer is dropped

static void search_results(const char *cards, const int count) {
    HtmlElem out = html_dom_by_id("searchResultsMA");
    if (!out) return;
    StrBuf res = {0};
    if (count) {
        sb_append(&res, "<div style='height:10px'></div>");
        sb_append(&res, cards ? cards : "");
    } else {
        sb_append(&res, I18N("windows.maps.none", "No Games found!"));
    }
    char *html = sb_finish(&res);
    html_dom_set_html(out, html);
    free(html);
}

static void on_search(void *user, const int status, const char *body, const size_t length) {
    if ((unsigned) (uintptr_t) user != g_search_gen) return;
    cJSON *res = status == 200 && body ? cJSON_ParseWithLength(body, length) : NULL;
    StrBuf sb = {0};
    int count = 0;
    const cJSON *r;
    cJSON_ArrayForEach(r, cJSON_GetObjectItem(res, "data")) {
        GameCard card;
        card_from_brain(&card, r);
        games_card(&sb, &card, 6);
        count++;
    }
    cJSON_Delete(res);
    char *cards = sb_finish(&sb);
    search_results(cards, count);
    free(cards);
}

static void games_search(const HtmlArg *args, const int argc) {
    HtmlElem input = html_dom_by_id("mpSrch"), cat = html_dom_by_id("mpCat"), out = html_dom_by_id("searchResultsMA");
    if (!out) return;
    if (argc > 0 && args[0].kind == HTML_ARG_STR && args[0].str && input) html_dom_set_value(input, args[0].str);
    if (argc > 1 && args[1].kind == HTML_ARG_NUM && cat) {
        char v[16];
        snprintf(v, sizeof(v), "%d", (int) args[1].num);
        html_dom_set_value(cat, v);
    }
    if (argc > 2 && args[2].kind == HTML_ARG_STR && args[2].str && args[2].str[0] && cat) html_dom_set_value(cat, args[2].str);
    char q[96] = "", c[16] = "-1";
    if (input) html_dom_get_value(input, q, sizeof(q));
    if (cat) html_dom_get_value(cat, c, sizeof(c));
    snprintf(g_games_search, sizeof(g_games_search), "%s", q);
    const int source = community_source_of(c); // "official" / "community": no category
    g_games_search_cat = source == COMMUNITY_SOURCE_ALL ? atoi(c) : -1;
    g_search_gen++;
    if (backend_available() == 1) {
        char path[512];
        if (community_search_path(path, sizeof(path), q, g_games_search_cat, source)) {
            html_dom_set_html(out, "Loading...");
            if (backend_http_get(path, on_search, (void *) (uintptr_t) g_search_gen)) return;
        }
    }
    const char *want_cat = g_games_search_cat >= 0 && g_games_search_cat < (int) (sizeof(k_map_cats) / sizeof(k_map_cats[0])) ? k_map_cats[g_games_search_cat] : NULL;
    StrBuf sb = {0};
    int count = 0;
    const char *seen[256];
    int seen_n = 0;
    for (int data = 0; data < GAMES_TABS && source != COMMUNITY_SOURCE_OFFICIAL; data++) {
        const cJSON *r;
        cJSON_ArrayForEach(r, g_games_tabs[data].rows) {
            GameCard card;
            card_from_brain(&card, r);
            if (!str_has_ci(card.name, q) || (want_cat && (!card.category || strcmp(card.category, want_cat)))) continue;
            int dup = 0;
            for (int i = 0; i < seen_n; i++) dup |= !strcmp(seen[i], card.name);
            if (dup) continue;
            if (seen_n < 256) seen[seen_n++] = card.name;
            games_card(&sb, &card, 6);
            count++;
        }
    }
    for (size_t i = 0; !want_cat && i < map_catalog_count(); i++) {
        const MapCatalogEntry *e = map_catalog_at(i);
        const int official = map_catalog_is_official(e);
        if (!str_has_ci(e->name, q) || (source == COMMUNITY_SOURCE_OFFICIAL && !official) || (source == COMMUNITY_SOURCE_COMMUNITY && official)) continue;
        int dup = 0;
        for (int k = 0; k < seen_n; k++) dup |= !strcmp(seen[k], e->name);
        if (dup) continue;
        if (seen_n < 256) seen[seen_n++] = e->name;
        GameCard card;
        char thumb[48];
        card_from_catalogue(&card, e, thumb, sizeof(thumb));
        games_card(&sb, &card, 6);
        count++;
    }
    char *cards = sb_finish(&sb);
    search_results(cards, count);
    free(cards);
}

// ---- the Host Game window's type (windows[7] / showWindow(8): its markup is host_flow_ui.c's)
static int g_host_type = GAME_HOST_CUSTOM; // openHostWindow(false, type): config.hostTypes index

void servers_ui_set_host_type(const int host_type) {
    g_host_type = game_host_type_name(host_type) ? host_type : GAME_HOST_CUSTOM;
}

int servers_ui_host_type(void) {
    return g_host_type;
}

// A Popular Now card (v10: openURL("/?play=" + name)): one of our custom games on that map is playMap's;
// a map only krunker.io's custom games run joins the busiest of them that has room (an official lobby,
// krunker_lobbies_open), what krunker.io's own /?play= finds. 1 = an official lobby was opened.
int servers_ui_popular_official(const char *map) {
    const cJSON *t;
    cJSON_ArrayForEach(t, brain_games()) {
        if (row_custom(t) && !strcmp(row_i(t), map ? map : "")) return 0;
    }
    char id[80];
    if (!krunker_lobbies_pick_custom(map, id, sizeof(id))) return 0;
    printf("[mm] Popular Now %s: krunker.io's custom game %s\n", map, id);
    krunker_lobbies_open(id);
    return 1;
}

// ------------------------------------------------------------------ playMap / ?play= / ?game=

// v3.1.1 playMap(name) -> "/?play=<name>" -> autoHostMap: join a lobby on that map, else host
// one. Published (DB) maps that are not in the client's catalogue are fetched from the brain
// (GET /api/map-data) and played in the offline game when no brain lobby can host them.
static char g_play_name[96];
static int g_play_catalogue;
static int g_play_mode = -1; // the Krunker mode a play asks for (the raid promo: Raid, 23); -1 any

// a lobby row's Krunker mode id (meta "mode", else "g", as row_mode); -1 none
static int row_mode_id(const cJSON *t) {
    const cJSON *m = cJSON_GetObjectItem(meta_of(t), "mode");
    if (!cJSON_IsNumber(m)) m = cJSON_GetObjectItem(meta_of(t), "g");
    return cJSON_IsNumber(m) ? m->valueint : -1;
}

// a published map from kn_brain (community_fetch: downloaded once, checked like v10's
// prepareGame + validateMap): the offline game hosts it, like the editor's Test Map
static void on_play_fetched(void *user, const cJSON *map, const char *error) {
    (void) user;
    if (!map) {
        ui_alert(error ? error : I18N("matchmaker.InvalidGameId", "Game not found."));
        return;
    }
    if (!g_client->offline) client_net_disconnect(g_client, "Offline", "000000");
    menu_show_window(MENU_WINDOW_NONE, 0);
    if (local_game_host_custom(g_client, map)) printf("[mm] playing published map %s offline\n", g_play_name);
    else ui_alert(I18N("kn.games.loadFailed", "Failed to load the game."));
}

static void fetch_map_data(const char *name) {
    community_fetch(name, on_play_fetched, NULL);
}

// The official map's index (v10's mapIndex: the dsma number of the lobby's map), -1 when v10 has no such map
static int official_map_index(const char *name) {
    for (int i = 0; i < krunker_official_map_count(); i++) {
        const char *m = krunker_official_map_name(i);
        if (m && !strcasecmp(m, name)) return i;
    }
    return -1;
}

static void on_play_list(void *user, const int status, const char *body, const size_t length) {
    (void) user;
    (void) length;
    cJSON *res = status == 200 ? cJSON_Parse(body) : NULL;
    const cJSON *t;
    const char *found = NULL;
    cJSON_ArrayForEach(t, cJSON_GetObjectItem(res, "games")) {
        const cJSON *meta = cJSON_GetArrayItem(t, 4);
        const char *map = cJSON_GetStringValue(cJSON_GetObjectItem(meta, "map"));
        const char *custom = cJSON_GetStringValue(cJSON_GetObjectItem(meta, "m"));
        if (meta_flag(t, "pr") || tuple_int(t, 2) >= tuple_int(t, 3)) continue;
        if (g_play_mode >= 0 && row_mode_id(t) != g_play_mode) continue; // a Raid play joins a Raid lobby
        if ((map && !strcmp(map, g_play_name)) || (custom && !strcmp(custom, g_play_name))) {
            found = cJSON_GetStringValue(cJSON_GetArrayItem(t, 0));
            break;
        }
    }
    if (found) {
        menu_show_window(MENU_WINDOW_NONE, 0);
        switch_server(found);
    } else if (g_play_catalogue) {
        // no server runs it: practice it offline (the client never hosts; doc/backend.md 8.7). The
        // lobby this client is joined to is left first (the console's `offline <map>`): v10's Play
        // ends the page, so the player ends up on the map, not in the lobby they came from.
        const int index = map_catalog_find(g_play_name);
        // no lobby of ours runs it: the offline game on it, leaving the lobby this client is joined to
        // (v10's Play ends the page, so the player ends up on the map, not in that lobby)
        if (index < 0) {
            menu_log("play: %s is not in the map catalogue", g_play_name);
        } else if (g_client->offline) {
            // the running offline game restarts on the map in the play's mode (the raid promo's Raid);
            // ui_console_go_offline would keep the running game's mode (local_game_pick_map)
            const int slot = map_catalog_slot(&g_client->game.maps, &g_client->game.map_count, (size_t) index);
            if (slot >= 0) {
                menu_select_map(slot);
                if (g_play_mode >= 0) g_client->net_mode_index = g_play_mode; // local_game_host reads it (Krunker mode id)
                local_game_host(g_client, slot, g_client->net_mode_index);
                menu_host_offline();
            }
        } else {
            if (g_play_mode >= 0) g_client->net_mode_index = g_play_mode; // local_game_start reads it (Krunker mode id)
            ui_console_go_offline(g_client, index, "play");
        }
    } else {
        fetch_map_data(g_play_name); // a published map: offline practice too
    }
    cJSON_Delete(res);
}

int servers_ui_play_map(const char *name, const int catalogue) { return servers_ui_play_map_mode(name, catalogue, -1); }

int servers_ui_play_map_mode(const char *name, const int catalogue, const int mode) {
    if (!name || !name[0] || backend_available() != 1) return 0;
    snprintf(g_play_name, sizeof(g_play_name), "%s", name);
    g_play_catalogue = catalogue;
    g_play_mode = mode;
    // v10 ?play=<map> (playMap, v10.js:222040-222051): a matchmaker seek for that map in any region, the
    // map as its dataQuery (`i`; the brain matches the map's index, dsma), autoChangeGame false. No lobby
    // plays it: the plain seek (on_seek). Community maps (not one of v10's) keep the list search below.
    // (a mode-specific play, the raid promo's Raid, keeps the list search below: the seek has no mode)
    const int index = official_map_index(name);
    if (mode < 0 && index >= 0 && g_brain_mode && !g_client->offline && g_pin == PIN_NONE) {
        snprintf(g_play_query, sizeof(g_play_query), "{\"i\":\"%s\",\"dsma\":[%d]}", name, index);
        g_exclude_game[0] = 0;
        g_play_seek = 1;
        printf("[mm] play %s: a seek for that map (v10 playMap)\n", name);
        seek(NULL, 0, NULL);
        return 1;
    }
    if (g_brain_mode && !g_client->offline) {
        mm_get("/mm/game-list?hostname=krunknative", on_play_list, NULL);
        return 1;
    }
    if (catalogue) return 0; // the offline game hosts catalogue maps itself
    fetch_map_data(name);
    return 1;
}

// the game page's ?play=<name> / ?game=<id> (hub links, feed "Join Now!", web start URL)
void servers_ui_query(const char *query) {
    if (!query) return;
    char value[160];
    for (int pass = 0; pass < 2; pass++) {
        const char *key = pass ? "game=" : "play=";
        const char *p = strstr(query, key);
        if (!p || (p != query && p[-1] != '&' && p[-1] != '?')) continue;
        p += strlen(key);
        size_t n = 0;
        while (*p && *p != '&' && n + 1 < sizeof(value)) {
            if (*p == '%' && p[1] && p[2]) {
                char hex[3] = {p[1], p[2], 0};
                value[n++] = (char) strtol(hex, NULL, 16);
                p += 3;
            } else {
                value[n++] = *p == '+' ? ' ' : *p;
                p++;
            }
        }
        value[n] = 0;
        if (!value[0]) continue;
        snprintf(pass ? g_pending_game : g_pending_play, pass ? sizeof(g_pending_game) : sizeof(g_pending_play), "%s", value);
    }
}

// ------------------------------------------------------------------ Host Game

static void on_host(void *user, const int status, const char *body, const size_t length) {
    (void) user;
    HtmlElem msg = html_dom_by_id("hostGameMsg");
    cJSON *res = cJSON_Parse(body);
    const cJSON *host = cJSON_GetObjectItem(res, "host");
    if (status == 200 && cJSON_IsString(host)) {
        if (msg) html_dom_set_text(msg, I18N("kn.host.created", "Game Created")); // customRoomResponse(message, gameId)
        g_last_load_ms = -1e18; // windows[1].lastLoadTime = 0
        cJSON_Delete(res);
        menu_show_window(MENU_WINDOW_NONE, 0);
        g_pin = PIN_NONE; // our own hosted game
        on_seek(NULL, status, body, length);
        return;
    }
    const cJSON *err = cJSON_GetObjectItem(res, "error");
    if (msg) {
        StrBuf sb = {0};
        sb_append(&sb, "<span class='error'>");
        sb_append_esc(&sb, seek_error_text(cJSON_IsString(err) ? err->valuestring : status ? "Failed" : NULL));
        sb_append(&sb, "</span>");
        char *html = sb_finish(&sb);
        html_dom_set_html(msg, html);
        free(html);
    }
    cJSON_Delete(res);
}

// Start Game on Box Office servers (host_flow_ui.c's choice): the brain's /mm/host-game with the
// request's config (the game is created on a server it finds; "Game Created" then the join). 0 when
// the brain is not available or the request cannot be sent.
int servers_ui_host_game(const char *config_json) {
    if (backend_available() != 1 || !config_json) return 0;
    if (!g_region[0]) {
        char cached[32] = "";
        if (html_storage_get("pingRegion7", cached, sizeof(cached)) >= 0) snprintf(g_region, sizeof(g_region), "%s", cached);
    }
    const size_t n = strlen(config_json) * 3 + 512;
    char *path = malloc(n), *enc = malloc(strlen(config_json) * 3 + 1), region[96];
    if (!path || !enc) {
        free(path);
        free(enc);
        return 0;
    }
    backend_url_encode(enc, strlen(config_json) * 3 + 1, config_json);
    backend_url_encode(region, sizeof(region), g_region[0] ? g_region : "local");
    int len = snprintf(path, n, "/mm/host-game?region=%s&config=%s", region, enc);
    len += snprintf(path + len, n - (size_t) len, "%s", mm_flavor_param(path));
    if (active_account() && account_session_token()[0]) {
        char tok[256];
        backend_url_encode(tok, sizeof(tok), account_session_token());
        snprintf(path + len, n - (size_t) len, "&token=%s", tok);
    }
    HtmlElem msg = html_dom_by_id("hostGameMsg");
    if (msg) html_dom_set_text(msg, I18N("kn.host.creating", "Creating Game..."));
    const int ok = backend_http_get(path, on_host, NULL);
    free(path);
    free(enc);
    return ok;
}

// ------------------------------------------------------------------ lifecycle

void servers_ui_init(Client *client, const int brain_mode) {
    g_client = client;
    const char *list = getenv("KRUNKNATIVE_SERVER_LIST");
    if (list && *list) {
        unsigned char *data = NULL;
        size_t length = 0;
        cJSON *json = read_file(list, &data, &length) == 0 ? cJSON_Parse((const char *) data) : NULL;
        free(data);
        if (json) servers_ui_set_games(json);
        else fprintf(stderr, "[mm] KRUNKNATIVE_SERVER_LIST %s: unreadable\n", list);
        cJSON_Delete(json);
    }
    g_brain_mode = brain_mode && backend_enabled();
    g_start_time = glfwGetTime();
}

void servers_ui_set_director_mode(const int on) { g_director_mode = on != 0; }

int servers_ui_retry(void) {
    // an official krunker.io session (its failure's error screen included): its own seek, the player
    // stays on krunker.io (krunker_net_seek_new); never a KrunkNative lobby
    if (krunker_net_active()) return krunker_net_seek_new(g_client, NULL, 1);
    // pinned: the link retries that server (menu_retry_connect -> client_net_request_reconnect)
    if (g_pin == PIN_DIRECT) return 0;
    if (g_pin == PIN_IDLE && !g_brain_mode && !g_director_mode) return servers_ui_reconnect(); // the transport is stopped
    if (!g_brain_mode && !g_director_mode) return 0;
    // "Try seeking a new game": a fresh seek in the same region (v3.1.1 reloads the page)
    g_pin = PIN_NONE; // the user asked for matchmaking (also after a console `disconnect`)
    g_game_id[0] = 0;
    start_seek(NULL);
    return 1;
}

// Krunker v10.0.2 Quick Match (#menuBtnQuickMatch, v10_menu.c): backToOrigin(false, {exclude: ?game})
// reloads the page with ?exclude=<this lobby>, whose seek sends excludeGame (v10:222060, 147608):
// a fresh public lobby other than this one ("If you quickly want to move away from your current
// game mode, press the Quick Match button", the guide). Natively the seek runs in place, like
// servers_ui_retry. 0 = no matchmaking in this session (offline, no brain, direct server).
static int quick_match_seek(void) {
    snprintf(g_exclude_game, sizeof(g_exclude_game), "%s", g_game_id);
    printf("[mm] quick match: seeking a lobby other than %s\n", g_exclude_game[0] ? g_exclude_game : "(none)");
    g_pin = PIN_NONE;
    g_game_id[0] = 0;
    start_seek(NULL); // /mm/ping-list, then the seek with excludeGame
    return 1;
}

// The Lost Connection screen's Find Game after an official krunker.io failure (v10: a fresh page, a new
// game): the busiest other lobby of the list (krunker_lobbies_seek: the krunker.io Region first, the
// recent ones last), or without a list krunker.io's own Find New Game. 0 = neither could start.
int servers_ui_official_find_game(void) {
    if (krunker_lobbies_enabled()) return krunker_lobbies_seek();
    return g_client ? krunker_net_seek_new(g_client, NULL, 1) : 0;
}

int servers_ui_quick_match(void) {
    return servers_ui_find_new_game();
}

// Find New Game (F4, Krunker's client) and Quick Match: Settings > Quickplay decides where
// (seek_prefs.h): KrunkNative = a new seek here excluding this lobby and the recent ones
// (recent_lobbies.h; with the seek mode / map);
// Official = an official krunker.io lobby, joined in this client (native; krunker_lobbies_open, the
// browser with the opt-out); Both = ours when one
// matches, else an official one. A seek that finds nothing leaves the player in this game with a
// toast. 0 = nothing to do (offline / direct server with KrunkNative only).
int servers_ui_find_new_game(void) {
    const SeekServers type = seek_prefs_servers();
    // in an official game (krunker.io's Find New Game, a matchmaker seek excluding this game) the
    // player stays on krunker.io whatever the Quickplay server type: only the official list, with the
    // native lobby list, stays the krunker.io lobby browser's own seek (krunker_lobbies_seek)
    if (krunker_net_active() && !(type == SEEK_SERVERS_OFFICIAL && krunker_lobbies_enabled())) return krunker_net_seek_new(g_client, NULL, 1);
    if (type == SEEK_SERVERS_OFFICIAL) return krunker_lobbies_seek();
    if ((!g_brain_mode && !g_director_mode) || g_pin == PIN_DIRECT || g_seek_inflight) {
        return type == SEEK_SERVERS_BOTH && !g_seek_inflight ? krunker_lobbies_seek() : g_seek_inflight;
    }
    g_find_seek = 1;
    g_find_both = type == SEEK_SERVERS_BOTH;
    snprintf(g_find_prev_game, sizeof(g_find_prev_game), "%s", g_game_id);
    return quick_match_seek();
}

// --connect with a brain: the session reaches the direct server through JOIN, so an
// account change in the menu reconnects (same server) to attach or drop it. That session is the
// Box Office (krunk.io) account's: the krunker.io account of an official game never reconnects it.
static void direct_account_tick(Client *client, const int direct) {
    static int last = -1;
    if (krunker_net_active()) return;
    const AccountInfo *acc = krunkio_account();
    const int id = acc ? acc->id : 0;
    if (last < 0) last = id;
    if (id == last) return;
    last = id;
    if (direct && !client->offline && client->net_connected && !client->in_game) client_net_switch_server(client);
}

void servers_ui_tick(Client *client) {
    link_tick(client);
    rejoin_tick(client); // after a server restart / a refused join key: a fresh ticket
    servers_timer_tick(); // windows[1] "Ends in" countdown
    if (backend_enabled()) direct_account_tick(client, !g_brain_mode || g_pin == PIN_DIRECT); // tracks the account either way
    if (g_pin != PIN_NONE) g_started = 1; // connected / disconnected by hand first: no startup seek
    official_label_tick();
    // ?play= / ?game= requests once the startup seek is done (or right away without one)
    if ((g_pending_play[0] || g_pending_game[0]) && (!g_brain_mode || (g_started && !g_seek_inflight && client->net_connected))) {
        // (the startup seek took the page's own ?game=; a later one, e.g. a hub Join link,
        // switches unless it names the lobby we are in: on the web a switch reloads the page)
        if (g_pending_game[0] && g_brain_mode) {
            if (!is_current_game(g_pending_game)) switch_server(g_pending_game); // either id form
        } else if (g_pending_play[0]) {
            const MapCatalogEntry *e = NULL;
            const int idx = map_catalog_find(g_pending_play);
            if (idx >= 0) e = map_catalog_at((size_t) idx);
            if (!servers_ui_play_map(g_pending_play, e != NULL)) menu_log("play: %s needs a brain or the catalogue", g_pending_play);
        }
        g_pending_play[0] = g_pending_game[0] = 0;
    }
    if (g_brain_mode && !g_started && !g_startup_hold && backend_available() == 0) {
        // a default brain URL (page origin / local config) with no kn_brain behind it: play on
        // the default server as before; account actions say "Failed to connect"
        printf("[mm] no brain at %s: direct connection (accounts need a brain: make backend-up)\n", backend_url());
        g_brain_mode = 0;
        client->net_ws_url[0] = 0;
        client_net_switch_server(client); // the default transport target
        return;
    }
    if (g_director_mode && !g_brain_mode) { // Krunker Strike: the director's startup seek
        if (!g_started && g_pin == PIN_NONE) {
            g_started = 1;
            printf("[strike] matchmaking through the director %s\n", strike_director_url());
            seek(NULL, 1, NULL);
        }
        return;
    }
    if (!g_brain_mode) return;
    if (!g_started) {
        if (g_startup_hold) return; // a restart's resume rejoins its lobby instead (servers_ui_resume_rejoin)
        // readyLoad restores the saved session before the seek; wait briefly for it so
        // the lobby sees the account (the ticket carries it).
        const int has_token = account_session_token()[0] != 0; // the saved session (the alt vault's, or legacy)
        if (backend_available() < 0) return; // GET /mm/status still running (backend.h)
        // the restore needs the brain: count the wait from when it answered, not from startup
        // (a slow /mm/status could use the whole window up, and the seek went out as a guest)
        static double brain_ready_at = -1.0;
        if (brain_ready_at < 0.0) brain_ready_at = glfwGetTime() > g_start_time ? glfwGetTime() : g_start_time;
        if (has_token && !active_account() && glfwGetTime() - brain_ready_at < RESTORE_WAIT_SECONDS) return;
        if (g_reload_game[0]) {
            // F5's relaunch (game_reload.c): the lobby the old process was in, by its id. Never the
            // launch seek (Quickplay), nor a ?game= / ?play= / KRUNKNATIVE_GAME of the launch; gone
            // or full: on_seek's reload_fallback seeks another one and says so
            g_started = 1;
            g_pending_game[0] = g_pending_play[0] = 0;
            printf("[mm] reload: rejoining lobby %s\n", g_reload_game);
            g_reload_next = 1;
            start_seek(g_reload_game);
            g_reload_game[0] = 0;
            return;
        }
        char *game = NULL;
        if (g_pending_game[0]) {
            // the page's ?game= / --game "game=<id>" (servers_ui_query) is this startup seek's
            // game: consumed here, so the pending-switch above never re-requests it (on the web
            // that switch reloads the page with ?game=, which used to loop forever)
            game = strdup(g_pending_game);
            g_pending_game[0] = 0;
        } else {
#ifdef __EMSCRIPTEN__
        game = (char *) EM_ASM_PTR({
            var g = null;
            try { g = new URLSearchParams(location.search).get('game'); } catch (e) {}
            return g ? stringToNewUTF8(g) : 0;
        });
#else
        game = getenv("KRUNKNATIVE_GAME") ? strdup(getenv("KRUNKNATIVE_GAME")) : NULL;
#endif
        }
        if (!game && seek_prefs_on_launch()) { // Auto Seek on Launch: the Quickplay seek, once the menu is up
            if (!menu_ready_for_launch()) return;
            g_started = 1;
            launch_seek();
            return;
        }
        g_started = 1;
#ifdef __EMSCRIPTEN__
        // the browser's reload (F5, the Reload Game key) of a game page: its ?game= is the lobby the
        // player was in, so a gone or full one falls back like the native relaunch (reload_fallback)
        if (game && EM_ASM_INT({
                try {
                    var e = performance.getEntriesByType('navigation')[0];
                    return e && e.type === 'reload' ? 1 : 0;
                } catch (x) { return 0; }
            })) {
            printf("[mm] reload: rejoining lobby %s\n", game);
            g_reload_next = 1;
        }
#endif
        start_seek(game);
        free(game);
        return;
    }
    // setGameInstanceId once connected to a lobby
    static char labelled[160];
    if (client->net_connected && g_game_id[0] && strcmp(labelled, g_game_id)) {
        snprintf(labelled, sizeof(labelled), "%s", g_game_id);
        char path[256], enc[192];
        backend_url_encode(enc, sizeof(enc), g_game_id);
        snprintf(path, sizeof(path), "/mm/game-info?game=%s", enc);
        mm_get(path, on_game_info, NULL);
    }
    // the account changed while sitting in the menu of a lobby. A server that takes live sign-ins
    // (it pushed NET_EXT_KN_ACCOUNT_RIGHTS on this connection) re-verifies the connection in place
    // (online_mode.c, like v3.1.1 logging in on the game socket itself); an older one is rejoined
    // so it attaches (or drops) the account
    static int last_account = -1;
    const AccountInfo *acc = active_account();
    const int account_id = acc ? acc->id : 0;
    if (last_account < 0) last_account = account_id;
    if (account_id != last_account) {
        last_account = account_id;
        if (online_mode_live_ok(client)) {
            printf("[mm] account changed: the server re-verifies this connection (no rejoin)\n");
        } else if (account_id && g_account_game[0] && g_pin == PIN_NONE && !client->in_game && !g_seek_inflight) {
            // logged in after the matchmaker turned a guest away (AccountRequired): join that lobby now
            char id[sizeof(g_account_game)];
            snprintf(id, sizeof(id), "%s", g_account_game);
            printf("[mm] logged in: joining %s\n", id);
            menu_show_window(MENU_WINDOW_NONE, 0);
            seek(id, 0, NULL);
        } else if (g_pin == PIN_NONE && g_game_id[0] && !client->in_game && !g_seek_inflight) {
            seek(g_game_id, 1, NULL);
        }
    }
}

// ------------------------------------------------------------------ build versions (ci-release)
// The brain flags every listed server (doc/backend.md "Build version", services/brain/releases.c):
// finder /servers entries carry "outdated" / "incompatible", /mm/game-list tuples carry them in
// meta, and both answers carry the latest release: "latest", "download_url" (release page) and
// "downloads" {linux, windows, macos}. Self-contained helpers: the Servers window markup calls
// them from its rows and its header.

#define SERVERS_RELEASE_PAGE "https://github.com/rebel10n/KrunkNative/releases/tag/server-latest"
#define SERVERS_RELEASE_PREFIX "https://github.com/rebel10n/KrunkNative/"

static char g_server_download[512]; // the URL "Download server" opens (allowlisted)

// Only the project's own GitHub pages are opened from these (they come from the brain).
static int release_url_ok(const char *url) {
    if (!url || strncmp(url, SERVERS_RELEASE_PREFIX, strlen(SERVERS_RELEASE_PREFIX)) != 0 || strlen(url) >= sizeof(g_server_download))
        return 0;
    for (const char *p = url; *p; p++)
        if ((unsigned char) *p <= 0x20 || *p == '"' || *p == '\'' || *p == '\\' || *p == '<' || *p == '>' || *p == 0x7f) return 0;
    return 1;
}

// A list answer (finder /servers or /mm/game-list): remember where "Download server" goes. The
// asset for this platform natively; the release page on the web (any OS) and as the fallback.
void servers_ui_note_list_response(const cJSON *response) {
    if (!cJSON_IsObject(response)) return;
    const char *page = cJSON_GetStringValue(cJSON_GetObjectItem(response, "download_url"));
    const char *asset = NULL;
#ifndef __EMSCRIPTEN__
    const cJSON *downloads = cJSON_GetObjectItem(response, "downloads");
#if defined(_WIN32)
    asset = cJSON_GetStringValue(cJSON_GetObjectItem(downloads, "windows"));
#elif defined(__APPLE__)
    asset = cJSON_GetStringValue(cJSON_GetObjectItem(downloads, "macos"));
#elif defined(__linux__)
    asset = cJSON_GetStringValue(cJSON_GetObjectItem(downloads, "linux"));
#endif
#endif
    const char *url = release_url_ok(asset) ? asset : release_url_ok(page) ? page : NULL;
    if (url) snprintf(g_server_download, sizeof(g_server_download), "%s", url);
}

// downloadServer(): opens it (ui_open_external: system browser natively, window.open on the web)
void servers_ui_open_download(void) {
    ui_open_external(release_url_ok(g_server_download) ? g_server_download : SERVERS_RELEASE_PAGE);
}

// The meta object of a game-list tuple, or a finder /servers entry itself.
static const cJSON *entry_fields(const cJSON *entry) {
    return cJSON_IsArray(entry) ? cJSON_GetArrayItem(entry, 4) : entry;
}

int servers_ui_entry_outdated(const cJSON *entry) {
    return cJSON_IsTrue(cJSON_GetObjectItem(entry_fields(entry), "outdated"));
}

int servers_ui_entry_incompatible(const cJSON *entry) {
    return cJSON_IsTrue(cJSON_GetObjectItem(entry_fields(entry), "incompatible"));
}

// "" or one badge span (malloc'd, caller frees): "Incompatible" (another protocol major: the
// brain never matches it) wins over "Outdated" (older than the latest server release).
char *servers_ui_entry_badge_html(const cJSON *entry) {
    const cJSON *f = entry_fields(entry);
    const char *version = cJSON_GetStringValue(cJSON_GetObjectItem(f, "version"));
    if (!version || !version[0]) version = cJSON_GetStringValue(cJSON_GetObjectItem(f, "build"));
    const char *proto = cJSON_GetStringValue(cJSON_GetObjectItem(f, "protocol"));
    if (!proto || !proto[0]) proto = cJSON_GetStringValue(cJSON_GetObjectItem(f, "proto"));
    StrBuf sb = {0};
    if (servers_ui_entry_incompatible(entry)) {
        char title[192];
        I18N_F(title, "kn.servers.incompatibleTitle", "Protocol {0}: this client speaks {1}", proto && proto[0] ? proto : "?", kn_build_protocol());
        sb_append(&sb, " <span class='serverBadge serverIncompatible' style='color:#fff;background:#e53935;border-radius:4px;padding:0 6px;font-size:14px;vertical-align:middle' title='");
        sb_append_esc(&sb, title);
        sb_append(&sb, "'>");
        sb_append_esc(&sb, I18N("kn.servers.incompatible", "Incompatible"));
        sb_append(&sb, "</span>");
    } else if (servers_ui_entry_outdated(entry)) {
        sb_append(&sb, " <span class='serverBadge serverOutdated' style='color:#fff;background:#fb8c00;border-radius:4px;padding:0 6px;font-size:14px;vertical-align:middle' title='");
        sb_append_esc(&sb, I18N("kn.servers.outdatedTitle", "Older than the latest server release"));
        if (version && version[0]) {
            char runs[128];
            I18N_F(runs, "kn.servers.outdatedRuns", " (runs {0})", version);
            sb_append_esc(&sb, runs);
        }
        sb_append(&sb, "'>");
        sb_append_esc(&sb, I18N("kn.servers.outdated", "Outdated"));
        sb_append(&sb, "</span>");
    }
    char *s = sb_finish(&sb);
    return s ? s : strdup("");
}

// The "Download server" link (malloc'd, caller frees). list_response (may be NULL) is noted
// first (servers_ui_note_list_response); the link itself carries no URL: downloadServer().
char *servers_ui_download_button_html(const cJSON *list_response) {
    servers_ui_note_list_response(list_response);
    StrBuf sb = {0};
    // An icon button beside #hostMenuBtn (v7's header row has no room for a text link next to the
    // search box and Hide Full); the text is the tooltip.
    char title[640];
    I18N_F(title, "kn.servers.downloadTitle", "Download server: {0}", release_url_ok(g_server_download) ? g_server_download : SERVERS_RELEASE_PAGE);
    sb_append(&sb, "<a class='downloadServer' onclick='downloadServer()' style='cursor:pointer;display:inline-block;vertical-align:middle;width:40px;height:40px;margin-right:8px;background-color:#2196f3;border-radius:4px;text-align:center' title='");
    sb_append_esc(&sb, title);
    sb_append(&sb, "'><span class='material-icons' style='color:#fff;font-size:34px;margin-top:3px'>file_download</span></a>");
    char *s = sb_finish(&sb);
    return s ? s : strdup("");
}
