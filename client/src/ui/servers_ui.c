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
#include <html_ui.h>
#include <ui_logic.h>
#include <map_catalog.h>
#include <online_mode.h> // server finder URL, online / offline mode
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#define SERVER_BROWSER_RATE_MS 10000.0 // config.serverBrowserRate
#define RESTORE_WAIT_SECONDS 2.0       // startup seek waits this long for the saved session

// config.regionNames
static const struct {
    const char *id, *name;
} k_regions[] = {
    {"local", "Local"}, {"us-nj", "New York"}, {"us-il", "Chicago"}, {"us-tx", "Dallas"}, {"us-wa", "Seattle"},
    {"us-ca-la", "Los Angeles"}, {"us-ga", "Atlanta"}, {"nl-ams", "Amsterdam"}, {"gb-lon", "London"},
    {"de-fra", "Frankfurt"}, {"us-ca-sv", "Silicon Valley"}, {"au-syd", "Sydney"}, {"fr-par", "Paris"},
    {"jb-hnd", "Tokyo"}, {"us-fl", "Miami"}, {"sgp", "Singapore"}, {"blr", "India"}, {"brz", "Brazil"},
    {"me-bhn", "Middle East"}, {"af-ct", "South Africa"},
};

static Client *g_client;
static int g_brain_mode;
static int g_started;            // the startup seek ran
static double g_start_time;
static char g_region[32];        // region id the client seeks in
static char g_game_id[160];      // gameId: the lobby this client is in (or joining)
static int g_seek_inflight;
static int g_quick_join_left;    // quickJoinRegion: remaining candidates
static char **g_quick_join;      // candidate game ids
static int g_quick_join_count, g_quick_join_next;

// windows[1]
static cJSON *g_games;           // serverListData (array of tuples, owned)
static double g_total_players, g_social_players;
static double g_last_load_ms = -1e18;
static int g_loaded;
static int g_expanded = -1;
static char g_search[64];
static int g_hide_full = -1;     // dK (krk_hideFull), -1 = not read yet

static double now_ms(void) { return glfwGetTime() * 1000.0; }

static const char *region_name(const char *id) {
    for (size_t i = 0; i < sizeof(k_regions) / sizeof(k_regions[0]); i++) {
        if (id && !strcmp(k_regions[i].id, id)) return k_regions[i].name;
    }
    return id && id[0] ? id : "Unknown";
}

int servers_ui_brain_mode(void) { return g_brain_mode; }

static int hide_full(void) {
    if (g_hide_full < 0) {
        char v[16] = "";
        g_hide_full = html_storage_get("krk_hideFull", v, sizeof(v)) >= 0 && !strcmp(v, "true");
    }
    return g_hide_full;
}

// ------------------------------------------------------------------ joining

// joinServer(host, port, gameId, clientKey): point the transport at the game server.
static void join_server(const char *host, const int port, const char *game_id, const char *client_key, const char *web_url) {
    Client *client = g_client;
    snprintf(g_game_id, sizeof(g_game_id), "%s", game_id ? game_id : "");

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
    snprintf(client->net_join.game_id, sizeof(client->net_join.game_id), "%s", g_game_id);
    snprintf(client->net_join.client_key, sizeof(client->net_join.client_key), "%s", client_key ? client_key : "");
    snprintf(client->net_join.session, sizeof(client->net_join.session), "%s", account_get() ? account_session_token() : "");
    snprintf(client->net_address, sizeof(client->net_address), "%s", host);
    client->net_port = (unsigned short) port;
#ifdef __EMSCRIPTEN__
    snprintf(client->net_ws_url, sizeof(client->net_ws_url), "%s", ws);
#endif
    pthread_mutex_unlock(&client->net_lock);

    printf("[mm] joining %s at %s:%d\n", g_game_id, host, port);
    client_net_switch_server(client);
}

static void switch_server(const char *id);

// NET_PACKET_REDIRECT (server console `send`, doc/backend.md 8.10): another lobby of the
// network (seek it: a fresh ticket from the matchmaker) or a direct host:port.
void servers_ui_redirect(const char *game_id, const char *host, const int port, const char *reason) {
    if (!g_client) return;
    printf("[mm] server redirect: %s%s%s:%d (%s)\n", game_id && game_id[0] ? game_id : "", game_id && game_id[0] ? " / " : "",
           host ? host : "", port, reason ? reason : "");
    if (game_id && game_id[0] && g_brain_mode && backend_available() == 1) {
        switch_server(game_id);
        return;
    }
    if (host && host[0] && port > 0) join_server(host, port, game_id, NULL, NULL);
}

void servers_ui_join_ticket(const char *host, const int port, const char *game_id, const char *ticket, const char *web_url) {
    if (!g_client || !host) return;
    join_server(host, port, game_id, ticket, web_url);
}

// v3.1.1 matchmaker error names -> i18n matchmaker.* texts (the startup seek's catch)
static const char *seek_error_text(const char *error) {
    if (!error) return "Could not reach the matchmaker";
    if (!strcmp(error, "InvalidGameId")) return "Game not found.";
    if (!strcmp(error, "GameFull")) return "Game is full.";
    if (!strcmp(error, "NoServersMatchQuery") || !strcmp(error, "updating")) return "GAME UPDATING";
    if (!strcmp(error, "NoAvailableServers") || !strcmp(error, "no_lobbies")) return "Servers are at Max Capacity";
    return error;
}

// fatalError(message): the connection error screen (retry link = seek again)
static void fatal_error(const char *message) {
    on_client_socket_failed(g_client, message);
}

static void on_seek(void *user, const int status, const char *body, const size_t length) {
    (void) length;
    const int quiet = user != NULL; // quick join: try the next candidate instead of failing
    g_seek_inflight = 0;
    cJSON *res = cJSON_Parse(body);
    const cJSON *host = cJSON_GetObjectItem(res, "host");
    const cJSON *port = cJSON_GetObjectItem(res, "port");
    if (status == 200 && cJSON_IsString(host) && cJSON_IsNumber(port)) {
        const cJSON *game = cJSON_GetObjectItem(res, "gameId");
        const cJSON *key = cJSON_GetObjectItem(res, "clientId");
        const cJSON *web = cJSON_GetObjectItem(res, "webUrl");
        const cJSON *region = cJSON_GetObjectItem(res, "region");
        if (cJSON_IsString(region)) snprintf(g_region, sizeof(g_region), "%s", region->valuestring);
        g_quick_join_left = 0;
        join_server(host->valuestring, port->valueint, cJSON_IsString(game) ? game->valuestring : "",
                    cJSON_IsString(key) ? key->valuestring : "", cJSON_IsString(web) ? web->valuestring : NULL);
    } else if (quiet && g_quick_join_left > 0) {
        void servers_quick_join_next(void);
        servers_quick_join_next();
    } else {
        const cJSON *err = cJSON_GetObjectItem(res, "error");
        fprintf(stderr, "[mm] seek failed (HTTP %d): %s\n", status, cJSON_IsString(err) ? err->valuestring : "no response");
        fatal_error(seek_error_text(cJSON_IsString(err) ? err->valuestring : status ? "GameFull" : NULL));
    }
    cJSON_Delete(res);
}

// ClientManager.matchmakerSeek(region, gameId, dataQuery, autoChangeGame)
static void seek(const char *game_id, const int auto_change, void *quiet) {
    char path[1024], enc[512];
    backend_url_encode(enc, sizeof(enc), g_region);
    int n = snprintf(path, sizeof(path), "/mm/seek-game?hostname=krunknative&region=%s&autoChangeGame=%s", enc, auto_change ? "true" : "false");
    if (game_id && game_id[0]) {
        backend_url_encode(enc, sizeof(enc), game_id);
        n += snprintf(path + n, sizeof(path) - (size_t) n, "&game=%s", enc);
    }
    if (account_get() && account_session_token()[0]) {
        backend_url_encode(enc, sizeof(enc), account_session_token());
        snprintf(path + n, sizeof(path) - (size_t) n, "&token=%s", enc);
    }
    g_seek_inflight = 1;
    if (!backend_http_get(path, on_seek, quiet)) {
        g_seek_inflight = 0;
        fatal_error("Could not reach the matchmaker");
    }
}

// ClientManager.getRegion(): the cached region when the brain still lists it, else
// the first region of the ping list (KrunkNative does not ping each region).
static void on_ping_list(void *user, const int status, const char *body, const size_t length) {
    (void) length;
    const char *requested_game = user;
    cJSON *list = status == 200 ? cJSON_Parse(body) : NULL;
    char cached[32] = "";
    if (html_storage_get("pingRegion7", cached, sizeof(cached)) < 0) cached[0] = 0;
    const cJSON *setting = ui_setting_saved("defaultRegion");
    if (cJSON_IsString(setting) && setting->valuestring[0]) snprintf(cached, sizeof(cached), "%s", setting->valuestring);
    if (cJSON_IsObject(list)) {
        if (cached[0] && cJSON_GetObjectItem(list, cached)) {
            snprintf(g_region, sizeof(g_region), "%s", cached);
        } else if (list->child && list->child->string) {
            snprintf(g_region, sizeof(g_region), "%s", list->child->string);
        }
    }
    if (!g_region[0]) snprintf(g_region, sizeof(g_region), "%s", cached[0] ? cached : "local");
    html_storage_set("pingRegion7", g_region); // ClientManager.setDefaultRegion
    cJSON_Delete(list);
    seek(requested_game, 1, NULL);
    free((void *) requested_game);
}

static void start_seek(const char *game_id) {
    if (g_seek_inflight) return;
    char *copy = game_id && game_id[0] ? strdup(game_id) : NULL;
    if (!backend_http_get("/mm/ping-list", on_ping_list, copy)) {
        free(copy);
        fatal_error("Could not reach the matchmaker");
    }
}

// switchServer(id): the web reloads the page on ?game=id (v3.1.1 generateHref); the
// native client seeks that game and reconnects in place.
static void switch_server(const char *id) {
    if (!id || !id[0]) return;
#ifdef __EMSCRIPTEN__
    EM_ASM({
        var p = new URLSearchParams(location.search);
        p.set('game', UTF8ToString($0));
        location.search = '?' + p.toString();
    }, id);
#else
    seek(id, 0, NULL);
#endif
}

// setGameInstanceId: fetchGameInfo(id) -> menuRegionLabel = regionNames[regionId]
static void on_game_info(void *user, const int status, const char *body, const size_t length) {
    (void) user;
    (void) length;
    cJSON *tuple = status == 200 ? cJSON_Parse(body) : NULL;
    const cJSON *region = cJSON_GetArrayItem(tuple, 1);
    if (cJSON_IsString(region)) {
        snprintf(g_region, sizeof(g_region), "%s", region->valuestring);
        menu_set_region_label(region_name(region->valuestring));
    }
    cJSON_Delete(tuple);
}

// ------------------------------------------------------------------ windows[1]

static int search_matches(const cJSON *tuple) {
    const cJSON *meta = cJSON_GetArrayItem(tuple, 4);
    const cJSON *label = cJSON_GetObjectItem(meta, "i");
    if (!cJSON_IsObject(meta)) return 0;
    if (!g_search[0]) return 1;
    char hay[160], needle[64];
    snprintf(hay, sizeof(hay), "%s", cJSON_IsString(label) ? label->valuestring : "");
    snprintf(needle, sizeof(needle), "%s", g_search);
    for (char *p = hay; *p; p++) *p = (char) tolower((unsigned char) *p);
    for (char *p = needle; *p; p++) *p = (char) tolower((unsigned char) *p);
    return strstr(hay, needle) != NULL;
}

typedef struct {
    char key[32];       // region id or "custom"
    const char *name;
    int order;
    double players;
    const cJSON *games[256];
    int game_count;
} ServerGroup;

static int tuple_int(const cJSON *t, const int i) {
    const cJSON *v = cJSON_GetArrayItem(t, i);
    return cJSON_IsNumber(v) ? v->valueint : 0;
}

static int meta_flag(const cJSON *t, const char *key) {
    const cJSON *v = cJSON_GetObjectItem(cJSON_GetArrayItem(t, 4), key);
    return cJSON_IsTrue(v) || (cJSON_IsNumber(v) && v->valuedouble != 0);
}

static const ServerGroup *g_sort_groups;
static int cmp_games(const void *a, const void *b) {
    const cJSON *x = *(const cJSON *const *) a, *y = *(const cJSON *const *) b;
    const int pr = meta_flag(y, "pr") - meta_flag(x, "pr");
    if (pr) return pr;
    return tuple_int(y, 2) - tuple_int(x, 2);
}
static int cmp_groups(const void *a, const void *b) {
    const ServerGroup *x = a, *y = b;
    return (y->order ? y->order : -1) - (x->order ? x->order : -1);
}

// getServers(regionIndex): builds the region groups; returns the group count.
static int build_groups(ServerGroup *groups, const int max) {
    char preferred[32] = "";
    if (html_storage_get("pingRegion7", preferred, sizeof(preferred)) < 0) preferred[0] = 0;
    int count = 0;
    memset(groups, 0, sizeof(*groups) * (size_t) max);
    snprintf(groups[0].key, sizeof(groups[0].key), "custom");
    groups[0].name = "Custom Games";
    groups[0].order = 1;
    count = 1;
    const cJSON *t;
    cJSON_ArrayForEach(t, g_games) {
        const cJSON *meta = cJSON_GetArrayItem(t, 4);
        const cJSON *region = cJSON_GetArrayItem(t, 1);
        if (!cJSON_IsObject(meta) || !cJSON_IsString(region)) continue;
        int gi = -1;
        for (int i = 1; i < count; i++) {
            if (!strcmp(groups[i].key, region->valuestring)) gi = i;
        }
        if (gi < 0 && count < max) {
            gi = count++;
            snprintf(groups[gi].key, sizeof(groups[gi].key), "%s", region->valuestring);
            groups[gi].name = region_name(region->valuestring);
            if (!strcmp(preferred, region->valuestring)) groups[gi].order = 1;
        }
        if (gi < 0) continue;
        const int target = meta_flag(t, "cs") ? 0 : gi;
        const int players = tuple_int(t, 2), max_players = tuple_int(t, 3);
        if (!g_search[0] || search_matches(t)) groups[target].players += players < max_players ? players : max_players;
        if (groups[target].game_count < 256) groups[target].games[groups[target].game_count++] = t;
    }
    qsort(groups, (size_t) count, sizeof(*groups), cmp_groups);
    g_sort_groups = groups;
    for (int i = 0; i < count; i++) qsort((void *) groups[i].games, (size_t) groups[i].game_count, sizeof(groups[i].games[0]), cmp_games);
    return count;
}

static char *get_servers(void) {
    static ServerGroup groups[24];
    const int count = build_groups(groups, 24);
    StrBuf sb = {0};
    for (int h = 0; h < count; h++) {
        const ServerGroup *g = &groups[h];
        const int open = g_expanded == h;
        sb_appendf(&sb, "<div class='menuSelectorHeader' onclick='setExpandedRegion(%d)' style='cursor: pointer;'><span class='material-icons plusOrMinus'>%s</span> ",
                   open ? -1 : h, open ? "expand_more" : "navigate_next");
        sb_append_esc(&sb, g->name);
        sb_appendf(&sb, "<span style='float:right'>%.0f online</span></div><div class='quickJoin' onclick='quickJoinRegion(%d)'>Quick Join</div>", g->players, h);
        if (!open) continue;
        sb_append(&sb, "<div class='menuSelectorHolder'>");
        int shown = 0;
        for (int i = 0; i < g->game_count; i++) {
            const cJSON *t = g->games[i];
            const int players = tuple_int(t, 2), max_players = tuple_int(t, 3);
            if (hide_full() && players >= max_players) continue;
            if (g_search[0] && !search_matches(t)) continue;
            shown++;
            const cJSON *id = cJSON_GetArrayItem(t, 0);
            const cJSON *label = cJSON_GetObjectItem(cJSON_GetArrayItem(t, 4), "i");
            const char *ids = cJSON_IsString(id) ? id->valuestring : "";
            const int selected = g_game_id[0] && !strcmp(g_game_id, ids);
            sb_appendf(&sb, "<div class='menuSelector%s' onclick='checkedSwitchServer(\"", selected ? " selectedMenuSelector" : "");
            sb_append_esc(&sb, ids);
            sb_append(&sb, "\", this)'>"); // v3.1.1: this.querySelector(".serverPCount") (resolved in C)
            sb_append_esc(&sb, cJSON_IsString(label) ? label->valuestring : ids);
            if (meta_flag(t, "pr")) sb_append(&sb, " <span class='material-icons' style='color:#FBC02D;vertical-align:middle'>star</span> ");
            if (meta_flag(t, "earnKR")) sb_append(&sb, " <i class='material-icons' style='color:#2196F3;font-size:33px;vertical-align:bottom;'>check_circle</i>");
            sb_appendf(&sb, "<div class='serverPCount'>%d/%d</div>", players < max_players ? players : max_players, max_players);
            if (meta_flag(t, "cs")) {
                char node[64];
                snprintf(node, sizeof(node), "%s", ids);
                char *colon = strchr(node, ':');
                if (colon) *colon = 0;
                sb_append(&sb, "<div class='serverRegion'>");
                sb_append_esc(&sb, node);
                sb_append(&sb, " -&nbsp;</div>");
            }
            sb_append(&sb, "</div>");
        }
        if (!g->game_count || !shown) sb_append(&sb, "<div style='margin-left: 30px;margin-top: -5px;margin-bottom: 2px;'>No Joinable Games Found...</div>");
        sb_append(&sb, "</div>");
    }
    return sb_finish(&sb);
}

// ------------------------------------------------------------------ server finder + direct connect
// The central finder (online_mode.h: GET <finder>?..., services/brain/finder.c) lists
// dedicated servers it verified with the KNINFO query; any server is also reachable
// directly by host:port. Both join without the brain: plain connect, the server's
// AUTH_CHALLENGE decides between the online-mode handshake and a guest join.
static cJSON *g_finder_servers;   // GET <finder> "servers" (owned)
static int g_finder_loading;
static double g_finder_loaded_ms = -1e18;

static void on_finder(void *user, const int status, const char *body, const size_t length) {
    (void) user;
    (void) length;
    g_finder_loading = 0;
    cJSON *res = status == 200 ? cJSON_Parse(body) : NULL;
    const cJSON *servers = cJSON_GetObjectItem(res, "servers");
    if (cJSON_IsArray(servers)) {
        cJSON_Delete(g_finder_servers);
        g_finder_servers = cJSON_Duplicate(servers, 1);
    } else if (status != 200) {
        printf("[finder] %s: %s\n", online_finder_url(), status ? "error" : "unreachable");
    }
    cJSON_Delete(res);
    g_finder_loaded_ms = now_ms();
    menu_rerender_if_open(2);
}

static void finder_refresh(void) {
    if (!online_finder_url()[0] || g_finder_loading || now_ms() - g_finder_loaded_ms < SERVER_BROWSER_RATE_MS) return;
    char url[320];
    snprintf(url, sizeof(url), "%s%s", online_finder_url(), online_mode_online() ? "" : "?online_mode=0");
    g_finder_loading = backend_http_request("GET", url, NULL, on_finder, NULL);
}

// The Community Servers group (finder rows, filtered by the window's search / Hide Full).
static void finder_servers_html(StrBuf *sb) {
    if (!online_finder_url()[0]) return;
    sb_append(sb, "<div class='menuSelectorHeader'><span class='material-icons plusOrMinus'>public</span> Community Servers");
    sb_appendf(sb, "<span style='float:right'>%d listed</span></div><div class='menuSelectorHolder'>", cJSON_GetArraySize(g_finder_servers));
    int shown = 0;
    const cJSON *e;
    cJSON_ArrayForEach(e, g_finder_servers) {
        const char *host = cJSON_GetStringValue(cJSON_GetObjectItem(e, "host"));
        const cJSON *port = cJSON_GetObjectItem(e, "port");
        if (!host || !cJSON_IsNumber(port)) continue;
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(e, "name"));
        const char *map = cJSON_GetStringValue(cJSON_GetObjectItem(e, "map"));
        const char *mode = cJSON_GetStringValue(cJSON_GetObjectItem(e, "mode"));
        const char *community = cJSON_GetStringValue(cJSON_GetObjectItem(e, "community"));
        const int players = (int) cJSON_GetNumberValue(cJSON_GetObjectItem(e, "players"));
        const int max = (int) cJSON_GetNumberValue(cJSON_GetObjectItem(e, "max"));
        const int online = cJSON_IsTrue(cJSON_GetObjectItem(e, "online_mode"));
        if (hide_full() && max > 0 && players >= max) continue;
        if (g_search[0]) {
            char hay[256], needle[64];
            snprintf(hay, sizeof(hay), "%s %s %s", name ? name : "", map ? map : "", community ? community : "");
            snprintf(needle, sizeof(needle), "%s", g_search);
            for (char *p = hay; *p; p++) *p = (char) tolower((unsigned char) *p);
            for (char *p = needle; *p; p++) *p = (char) tolower((unsigned char) *p);
            if (!strstr(hay, needle)) continue;
        }
        if (online && !online_mode_online()) continue; // offline mode cannot join online_mode servers
        shown++;
        sb_append(sb, "<div class='menuSelector' onclick='finderJoin(\"");
        sb_append_esc(sb, host);
        sb_appendf(sb, "\", %d)'>", port->valueint);
        sb_append_esc(sb, name && name[0] ? name : host);
        sb_append(sb, " <span class='grey'>");
        sb_append_esc(sb, map ? map : "");
        if (mode && mode[0]) {
            sb_append(sb, " - ");
            sb_append_esc(sb, mode);
        }
        const cJSON *tag;
        cJSON_ArrayForEach(tag, cJSON_GetObjectItem(e, "tags")) {
            if (!cJSON_IsString(tag)) continue;
            sb_append(sb, " [");
            sb_append_esc(sb, tag->valuestring);
            sb_append(sb, "]");
        }
        if (community && community[0]) {
            sb_append(sb, " &middot; ");
            sb_append_esc(sb, community);
        }
        sb_append(sb, "</span>");
        if (online) sb_append(sb, " <i class='material-icons' style='color:#2196F3;font-size:28px;vertical-align:bottom;' title='Online mode: accounts verified'>verified_user</i>");
        sb_appendf(sb, "<div class='serverPCount'>%d/%d</div></div>", players, max);
    }
    if (!shown) sb_append(sb, g_finder_loading && !g_finder_servers ? "<div style='margin-left: 30px;margin-top: -5px;margin-bottom: 2px;'>Loading...</div>"
                                                                   : "<div style='margin-left: 30px;margin-top: -5px;margin-bottom: 2px;'>No Joinable Games Found...</div>");
    sb_append(sb, "</div>");
}

// Leave the brain's lobby (if any) and connect straight to host:port.
static void direct_join(const char *host, const int port) {
    if (!g_client || !host || !host[0] || port <= 0 || port > 65535) return;
    g_brain_mode = 0; // a direct session: no seek-game, no lobby id
    g_game_id[0] = 0;
    pthread_mutex_lock(&g_client->net_lock);
    memset(&g_client->net_join, 0, sizeof(g_client->net_join));
    snprintf(g_client->net_address, sizeof(g_client->net_address), "%s", host);
    g_client->net_port = (unsigned short) port;
#ifdef __EMSCRIPTEN__
    net_web_target_url(g_client->net_ws_url, sizeof(g_client->net_ws_url), host, port);
#endif
    pthread_mutex_unlock(&g_client->net_lock);
    printf("[mm] direct connect %s:%d (%s mode)\n", host, port, online_mode_name());
    menu_show_window(MENU_WINDOW_NONE, 0);
    client_net_switch_server(g_client);
}

// "host[:port]" (default port 21015); [v6]:port accepted.
static void direct_connect_text(const char *text) {
    char host[128] = "";
    int port = 21015;
    while (text && (*text == ' ' || *text == '\t')) text++;
    if (!text || !*text) return;
    if (*text == '[') {
        const char *end = strchr(text, ']');
        if (!end) return;
        snprintf(host, sizeof(host), "%.*s", (int) (end - text - 1), text + 1);
        if (end[1] == ':') port = atoi(end + 2);
    } else {
        snprintf(host, sizeof(host), "%s", text);
        char *colon = strchr(host, ':');
        if (colon && !strchr(colon + 1, ':')) {
            port = atoi(colon + 1);
            *colon = 0;
        }
    }
    size_t n = strlen(host);
    while (n && host[n - 1] == ' ') host[--n] = 0;
    direct_join(host, port);
}

static char *gen_list(void) {
    char total[32], social[32];
    snprintf(total, sizeof(total), "%.0f", g_total_players);
    snprintf(social, sizeof(social), "%.0f", g_social_players);
    StrBuf sb = {0};
    g_search[0] = 0; // genList: this.serverSearch = null
    // KrunkNative: hosting is the dedicated server exe; this tab is Direct Connect (host:port)
    sb_append(&sb, "<div class='serverHeader'><div class='sideTabs'><input id='directConnect' type='text' placeholder='Direct Connect ip:port' onkeydown='if(event.keyCode==13)directConnect()' style='width:170px'></input><a onclick='directConnect()'> Join</a></div><div class='centerTab'><input id='serverSearch' type='text' placeholder='Search Game' oninput='windows[1].searchList()'></input></div><div class='sideTabs'><div id=\"hideFull\" style=\"display:inline-block;\"><span class='grey'>Hide Full</span><label class='switch2' style='margin-top: 4px;margin-left: 11px;'><input type='checkbox' onclick='toggleHF(this.checked);windows[1].searchList()'");
    if (hide_full()) sb_append(&sb, " checked");
    sb_append(&sb, "><span class='slider2'></span></label></div></div></div><div style='height:10px'></div>");
    // the custom-games carousel (customsInfo.length >= 3) needs user-assets thumbnails: not shown
    sb_append(&sb, "<div style='font-size:20px'><div id='serverHolder'>");
    char *servers = backend_available() == 1 ? get_servers() : NULL;
    if (servers) sb_append(&sb, servers);
    free(servers);
    finder_servers_html(&sb);
    sb_appendf(&sb, "</div><div style='height:10px'></div><b style='color:rgba(0,0,0,0.6)'>%s In Game<span style='float:right;color:rgba(0,0,0,0.6)'>%s on Social</span></b></div>", total, social);
    return sb_finish(&sb);
}

static void on_game_list(void *user, const int status, const char *body, const size_t length) {
    (void) user;
    (void) length;
    cJSON *res = status == 200 ? cJSON_Parse(body) : NULL;
    const cJSON *games = cJSON_GetObjectItem(res, "games");
    if (cJSON_IsArray(games)) {
        cJSON_Delete(g_games);
        g_games = cJSON_Duplicate(games, 1);
        const cJSON *total = cJSON_GetObjectItem(res, "totalPlayerCount");
        g_total_players = cJSON_IsNumber(total) ? total->valuedouble : 0;
    }
    cJSON_Delete(res);
    g_loaded = 1;
    if (ui_window_key(2)) menu_rerender_if_open(2); // window.showWindow(2, true)
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

char *servers_window_gen(const int id, const char **header) {
    (void) header;
    if (id != 2) return NULL;
    finder_refresh(); // the server finder (both modes), then the brain's own lobbies when there is one
    if (backend_available() != 1) {
        if (!online_finder_url()[0]) return NULL; // no brain, no finder: the guest capture
        g_loaded = 1;
        return gen_list();
    }
    // gen(): the cached list within serverBrowserRate, else loadData() ("Loading...")
    if (now_ms() - g_last_load_ms < SERVER_BROWSER_RATE_MS && g_loaded) return gen_list();
    g_last_load_ms = now_ms();
    g_loaded = 0;
    backend_http_get("/mm/game-list?hostname=krunknative", on_game_list, NULL);
    backend_http_get("/api/player-count", on_player_count, NULL);
    return strdup("<div style='padding-top:19px;'>LOADING...</div>");
}

static void refresh_holder(void) {
    HtmlElem holder = html_dom_by_id("serverHolder");
    if (!holder) return;
    StrBuf sb = {0};
    char *html = backend_available() == 1 ? get_servers() : NULL;
    if (html) sb_append(&sb, html);
    free(html);
    finder_servers_html(&sb);
    html = sb_finish(&sb);
    html_dom_set_html(holder, html);
    free(html);
}

// quickJoinRegion(i): switchGame on the first joinable candidates (getServers(i))
void servers_quick_join_next(void) {
    while (g_quick_join_next < g_quick_join_count && g_quick_join_left > 0) {
        const char *id = g_quick_join[g_quick_join_next++];
        g_quick_join_left--;
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
}

static void quick_join_region(const int index) {
    static ServerGroup groups[24];
    const int count = build_groups(groups, 24);
    for (int i = 0; i < g_quick_join_count; i++) free(g_quick_join[i]);
    free(g_quick_join);
    g_quick_join = NULL;
    g_quick_join_count = g_quick_join_next = 0;
    if (index < 0 || index >= count) return;
    const ServerGroup *g = &groups[index];
    g_quick_join = calloc((size_t) g->game_count + 1, sizeof(char *));
    for (int i = 0; g_quick_join && i < g->game_count; i++) {
        const cJSON *t = g->games[i];
        const int players = tuple_int(t, 2), max_players = tuple_int(t, 3);
        if (!(players < max_players && players > 0)) continue; // regionID.games.filter(p < max && p > 0)
        if (g_search[0] && !search_matches(t)) continue;
        const cJSON *id = cJSON_GetArrayItem(t, 0);
        if (cJSON_IsString(id)) g_quick_join[g_quick_join_count++] = strdup(id->valuestring);
    }
    if (!g_quick_join_count && strcmp(g->key, "custom")) {
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

// checkedSwitchServer(id, el): game-info first; FULL / ERROR in the count cell
static char g_checked_id[160];
static void on_checked_info(void *user, const int status, const char *body, const size_t length) {
    (void) user;
    (void) length;
    cJSON *t = status == 200 ? cJSON_Parse(body) : NULL;
    HtmlElem cell = 0;
    HtmlElem cells[256];
    const int n = html_dom_query_all(0, "#serverHolder .menuSelector", cells, 256);
    for (int i = 0; i < n; i++) {
        char onclick[512] = "";
        html_dom_get_attr(cells[i], "onclick", onclick, sizeof(onclick));
        if (strstr(onclick, g_checked_id)) cell = html_dom_query(cells[i], ".serverPCount");
    }
    if (!cJSON_IsArray(t)) {
        if (cell) html_dom_set_text(cell, "ERROR");
    } else if (tuple_int(t, 2) >= tuple_int(t, 3)) {
        if (cell) html_dom_set_text(cell, "FULL");
    } else {
        menu_show_window(MENU_WINDOW_NONE, 0);
        switch_server(g_checked_id);
    }
    cJSON_Delete(t);
}

static void games_load_more(const HtmlEvent *ev);

int servers_ui_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    if (!name) return -1;
    if (!strcmp(name, "directConnect")) {
        char text[160] = "";
        HtmlElem input = html_dom_by_id("directConnect");
        if (argc > 0 && args[0].kind == HTML_ARG_STR && args[0].str) snprintf(text, sizeof(text), "%s", args[0].str);
        else if (input) html_dom_get_value(input, text, sizeof(text));
        direct_connect_text(text);
        return 1;
    }
    if (!strcmp(name, "finderJoin")) {
        if (argc > 1 && args[0].kind == HTML_ARG_STR && args[1].kind == HTML_ARG_NUM) direct_join(args[0].str, (int) args[1].num);
        return 1;
    }
    if (backend_available() != 1 && online_finder_url()[0] &&
        (!strcmp(name, "windows.1.searchList") || !strcmp(name, "toggleHF"))) {
        // no brain, but the finder list is up: search / Hide Full still filter it
    } else if (backend_available() != 1) {
        return -1;
    }
    if (!strcmp(name, "setExpandedRegion")) {
        g_expanded = argc > 0 && args[0].kind == HTML_ARG_NUM ? (int) args[0].num : -1;
        g_last_load_ms = now_ms();
        refresh_holder();
        return 1;
    }
    if (!strcmp(name, "windows.1.searchList")) {
        HtmlElem input = html_dom_by_id("serverSearch");
        g_search[0] = 0;
        if (argc > 0 && args[0].kind == HTML_ARG_STR && args[0].str) {
            g_expanded = 0;
            snprintf(g_search, sizeof(g_search), "%s", args[0].str);
            if (input) html_dom_set_value(input, g_search);
        } else if (input) {
            html_dom_get_value(input, g_search, sizeof(g_search));
        }
        refresh_holder();
        return 1;
    }
    if (!strcmp(name, "windows.9.loadMore")) {
        games_load_more(ev);
        return 1;
    }
    if (!strcmp(name, "toggleHF")) {
        g_hide_full = ev ? ev->checked : 0;
        html_storage_set("krk_hideFull", g_hide_full ? "true" : "false");
        return 1;
    }
    if (!strcmp(name, "quickJoinRegion")) {
        quick_join_region(argc > 0 && args[0].kind == HTML_ARG_NUM ? (int) args[0].num : 0);
        return 1;
    }
    if (!strcmp(name, "checkedSwitchServer") || !strcmp(name, "switchServer")) {
        const char *id = argc > 0 && args[0].kind == HTML_ARG_STR ? args[0].str : NULL;
        if (!id) return 1;
        snprintf(g_checked_id, sizeof(g_checked_id), "%s", id);
        if (!strcmp(name, "switchServer")) {
            menu_show_window(MENU_WINDOW_NONE, 0);
            switch_server(id);
            return 1;
        }
        HtmlElem cell = ev && ev->target ? html_dom_query(ev->target, ".serverPCount") : 0;
        if (cell) html_dom_set_text(cell, "...");
        char path[256], enc[192];
        backend_url_encode(enc, sizeof(enc), id);
        snprintf(path, sizeof(path), "/mm/game-info?game=%s", enc);
        backend_http_get(path, on_checked_info, NULL);
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
} g_games_tabs[GAMES_TABS];

static void on_maps(void *user, const int status, const char *body, const size_t length) {
    (void) length;
    const int tab = (int) (intptr_t) user;
    if (tab < 0 || tab >= GAMES_TABS) return;
    g_games_tabs[tab].waiting = 0;
    cJSON *res = status == 200 ? cJSON_Parse(body) : NULL;
    const cJSON *data = cJSON_GetObjectItem(res, "data");
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
    const AccountInfo *acc = account_get();
    int n = snprintf(path, sizeof(path), "/api/maps?index=%d&pos=%d", tab, g_games_tabs[tab].pos);
    if (acc) snprintf(path + n, sizeof(path) - (size_t) n, "&accountId=%d", acc->id);
    g_games_tabs[tab].waiting = 1;
    if (!backend_http_get(path, on_maps, (void *) (intptr_t) tab)) g_games_tabs[tab].waiting = 0;
}

int servers_games_tab_rows(const int tab, const cJSON **rows) {
    if (backend_available() != 1 || tab < 0 || tab >= GAMES_TABS) return -1;
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

static void games_load_more(const HtmlEvent *ev) {
    for (int tab = 0; tab < GAMES_TABS; tab++) {
        if (!g_games_tabs[tab].requested || g_games_tabs[tab].waiting || !g_games_tabs[tab].more) continue;
        HtmlElem active = html_dom_query(0, "#menuWindow .menuTabNew.tabANew");
        char onclick[96] = "", want[48];
        if (active) html_dom_get_attr(active, "onclick", onclick, sizeof(onclick));
        snprintf(want, sizeof(want), "switchTab(%d)", tab);
        if (!strstr(onclick, want)) continue;
        if (ev && ev->target) {
            html_dom_set_attr(ev->target, "class", "loadMoreW");
            html_dom_set_text(ev->target, "Loading...");
        }
        g_games_tabs[tab].pos++;
        games_request(tab);
        return;
    }
}

// ------------------------------------------------------------------ playMap / ?play= / ?game=

// v3.1.1 playMap(name) -> "/?play=<name>" -> autoHostMap: join a lobby on that map, else host
// one. Published (DB) maps that are not in the client's catalogue are fetched from the brain
// (GET /api/map-data) and played in the offline game when no brain lobby can host them.
static char g_play_name[96];
static int g_play_catalogue;
static char g_pending_play[96], g_pending_game[160];

static void on_map_data(void *user, const int status, const char *body, const size_t length) {
    (void) user;
    (void) length;
    cJSON *map = status == 200 ? cJSON_Parse(body) : NULL;
    if (!cJSON_IsObject(map)) {
        cJSON_Delete(map);
        ui_alert("Game not found.");
        return;
    }
    // the offline game hosts the published map (like the editor's Test Map)
    if (!g_client->offline) client_net_disconnect(g_client, "Offline", "000000");
    menu_show_window(MENU_WINDOW_NONE, 0);
    if (local_game_host_custom(g_client, map)) printf("[mm] playing published map %s offline\n", g_play_name);
    else ui_alert("Failed to load the game.");
    cJSON_Delete(map);
}

static void fetch_map_data(const char *name) {
    char path[256], enc[192];
    backend_url_encode(enc, sizeof(enc), name);
    snprintf(path, sizeof(path), "/api/map-data?name=%s", enc);
    backend_http_get(path, on_map_data, NULL);
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
        if ((map && !strcmp(map, g_play_name)) || (custom && !strcmp(custom, g_play_name))) {
            found = cJSON_GetStringValue(cJSON_GetArrayItem(t, 0));
            break;
        }
    }
    if (found) {
        menu_show_window(MENU_WINDOW_NONE, 0);
        switch_server(found);
    } else if (g_play_catalogue) {
        // no server runs it: practice it offline (the client never hosts; doc/backend.md 8.7)
        const int index = map_catalog_find(g_play_name);
        const int slot = index >= 0 ? map_catalog_slot(&g_client->game.maps, &g_client->game.map_count, (size_t) index) : -1;
        if (slot >= 0) {
            menu_select_map(slot);
            if (g_client->offline) local_game_host(g_client, slot, g_client->net_mode_index);
            else g_client->offline_start_map = g_client->game.maps[slot];
            menu_host_offline();
            g_client->offline_start_map = NULL;
        }
    } else {
        fetch_map_data(g_play_name); // a published map: offline practice too
    }
    cJSON_Delete(res);
}

int servers_ui_play_map(const char *name, const int catalogue) {
    if (!name || !name[0] || backend_available() != 1) return 0;
    snprintf(g_play_name, sizeof(g_play_name), "%s", name);
    g_play_catalogue = catalogue;
    if (g_brain_mode && !g_client->offline) {
        backend_http_get("/mm/game-list?hostname=krunknative", on_play_list, NULL);
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
        if (msg) html_dom_set_html(msg, "Game Created"); // customRoomResponse(message, gameId)
        g_last_load_ms = -1e18; // windows[1].lastLoadTime = 0
        cJSON_Delete(res);
        menu_show_window(MENU_WINDOW_NONE, 0);
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

// The client no longer hosts (doc/backend.md 8.7): kept for the brain admin flow only,
// never called by the UI. KRUNKNATIVE_ALLOW_CLIENT_HOST=1 re-enables it for debugging.
int servers_ui_host_game(const char *config_json) {
    if (!getenv("KRUNKNATIVE_ALLOW_CLIENT_HOST")) return 0;
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
    if (account_get() && account_session_token()[0]) {
        char tok[256];
        backend_url_encode(tok, sizeof(tok), account_session_token());
        snprintf(path + len, n - (size_t) len, "&token=%s", tok);
    }
    HtmlElem msg = html_dom_by_id("hostGameMsg");
    if (msg) html_dom_set_html(msg, "Creating Game...");
    const int ok = backend_http_get(path, on_host, NULL);
    free(path);
    free(enc);
    return ok;
}

// ------------------------------------------------------------------ lifecycle

void servers_ui_init(Client *client, const int brain_mode) {
    g_client = client;
    g_brain_mode = brain_mode && backend_enabled();
    g_start_time = glfwGetTime();
}

int servers_ui_retry(void) {
    if (!g_brain_mode) return 0;
    // "Try seeking a new game": a fresh seek in the same region (v3.1.1 reloads the page)
    g_game_id[0] = 0;
    start_seek(NULL);
    return 1;
}

// --connect with a brain: the session reaches the direct server through JOIN, so an
// account change in the menu reconnects (same server) to attach or drop it.
static void direct_account_tick(Client *client) {
    static int last = -1;
    const AccountInfo *acc = account_get();
    const int id = acc ? acc->id : 0;
    if (last < 0) last = id;
    if (id == last) return;
    last = id;
    if (!client->offline && client->net_connected && !client->in_game) client_net_switch_server(client);
}

void servers_ui_tick(Client *client) {
    if (!g_brain_mode && backend_enabled()) direct_account_tick(client);
    // ?play= / ?game= requests once the startup seek is done (or right away without one)
    if ((g_pending_play[0] || g_pending_game[0]) && (!g_brain_mode || (g_started && !g_seek_inflight && client->net_connected))) {
        if (g_pending_game[0] && g_brain_mode) switch_server(g_pending_game);
        else if (g_pending_play[0]) {
            const MapCatalogEntry *e = NULL;
            const int idx = map_catalog_find(g_pending_play);
            if (idx >= 0) e = map_catalog_at((size_t) idx);
            if (!servers_ui_play_map(g_pending_play, e != NULL)) menu_log("play: %s needs a brain or the catalogue", g_pending_play);
        }
        g_pending_play[0] = g_pending_game[0] = 0;
    }
    if (g_brain_mode && !g_started && backend_available() == 0) {
        // a default brain URL (page origin / local config) with no kn_brain behind it: play on
        // the default server as before; account actions say "Failed to connect"
        printf("[mm] no brain at %s: direct connection (accounts need a brain: make backend-up)\n", backend_url());
        g_brain_mode = 0;
        client->net_ws_url[0] = 0;
        client_net_switch_server(client); // the default transport target
        return;
    }
    if (!g_brain_mode) return;
    if (!g_started) {
        // readyLoad restores the saved session before the seek; wait briefly for it so
        // the lobby sees the account (the ticket carries it).
        char token[8] = "";
        const int has_token = html_storage_get("krunker_token", token, sizeof(token)) >= 0 && token[0];
        if (backend_available() < 0) return; // GET /mm/status still running (backend.h)
        if (has_token && !account_get() && glfwGetTime() - g_start_time < RESTORE_WAIT_SECONDS) return;
        g_started = 1;
        char *game = NULL;
#ifdef __EMSCRIPTEN__
        game = (char *) EM_ASM_PTR({
            var g = null;
            try { g = new URLSearchParams(location.search).get('game'); } catch (e) {}
            return g ? stringToNewUTF8(g) : 0;
        });
#else
        game = getenv("KRUNKNATIVE_GAME") ? strdup(getenv("KRUNKNATIVE_GAME")) : NULL;
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
        backend_http_get(path, on_game_info, NULL);
    }
    // the account changed while sitting in the menu of a lobby: rejoin it so the game
    // server attaches (or drops) the account (v3.1.1 logs in on the game socket itself)
    static int last_account = -1;
    const AccountInfo *acc = account_get();
    const int account_id = acc ? acc->id : 0;
    if (last_account < 0) last_account = account_id;
    if (account_id != last_account) {
        last_account = account_id;
        if (g_game_id[0] && !client->in_game && !g_seek_inflight) seek(g_game_id, 1, NULL);
    }
}
