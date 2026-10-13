// Offline play (client_bin --offline, web ?offline=1 / ?sandbox, Host Game without a server,
// console `offline` / `map`, the editor's Test Map, published maps practised offline).
//
// The offline game is a sandbox server: server_bin's own core (server/include/server_sandbox.h:
// packet handlers, chat commands, accounts gate, bots, map triggers, interact, terminals, lag
// compensation, modes, the tick) runs inside this process, and the client joins it like any
// server, over an in-memory loopback connection that carries the exact framed packets of the
// TCP protocol. client->game is a normal predicted client game (prediction, reconciliation,
// interpolation), so everything a map or mode does offline is what it does on server_bin.
// Like Krunker v7.1.4's ioSandbox (src/libs/io-sandbox.js + sandboxGame in index.js): a
// GameObj in the page fed the socket's packets.
//
// No thread on either platform: client_offline_pump() steps the sandbox on the main loop
// (the frame's delta, at most one server tick per frame at 30 Hz) and drains what it sent.
// The offline conveniences are sandbox server config (server_sandbox.h): no backend accounts,
// but the player counts as having one (GameHooks.account_info has_account = 1, decisions.md
// "Offline account": account-locked classes and reqAccount streaks unlock), every skin owned,
// perms off and the local player is the host (DEV: dev console / chat commands, bot_* and the
// cheats), no idle kicks. The sandbox plays as a hosted game, so every map's embedded "config"
// applies (WP-E, decisions.md "Embedded map config") and reaches this client as GAME_CONFIG.
#include <client.h>
#include <ruleset_ui.h>
#include <server_sandbox.h>
#include <map_catalog.h>
#include <ks_storage.h> // WP6: the sandbox's offline GAME.STORAGE / Save Data store
#include <html_dom.h>   // html_storage_get / set
#include <knet.h>
#include <modes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static NetPacketBuffer g_offline_buffer; // server -> client bytes being decoded
// Our end of the sandbox connection. Not client->net_socket: a native transport thread that
// is still winding down (a late connect, its disconnect) writes that one.
static int g_offline_fd = -1;

static int offline_socket(Client *client) {
    (void) client;
    return g_offline_fd;
}

// client_net_send() offline: the frame goes into the sandbox connection.
void client_offline_send(Client *client, const NetPacketType type, const void *payload, const uint16_t length) {
    const int socket = offline_socket(client);
    if (socket < NET_LOOPBACK_FD_BASE) return;
    client_net_log_packet("=>", type, payload, length); // KRUNKNATIVE_PACKET_LOG=1
    net_send_packet(socket, type, payload, length);
}

// HELLO + a guest JOIN with the menu nickname (no brain ticket or session: nothing to verify).
static void offline_hello(Client *client) {
    const NetHelloPacket hello = net_hello_make(); // the minor gates the v7 packets (KrunkScript 200-203), the patch WP-C sizes, the mask the rulesets
    client_offline_send(client, NET_PACKET_HELLO, &hello, sizeof(hello));
    NetJoinPacket join;
    memset(&join, 0, sizeof(join));
    if (strcmp(client->menu_name, "Guest") != 0) snprintf(join.name, sizeof(join.name), "%s", client->menu_name);
    client_offline_send(client, NET_PACKET_JOIN, &join, (uint16_t) NET_JOIN_PACKET_V2_SIZE);
}

// A new connection to the running sandbox: what a transport's connect does (net.c).
static int offline_connect(Client *client) {
    const int socket = sandbox_connect();
    if (socket < 0) return 0;
    g_offline_buffer.length = 0;
    g_offline_fd = socket;
    client->net_ping_ms = 0; // in-process: nothing to ping (Krunker's #pingText default)
    offline_hello(client);
    return 1;
}

// The sandbox closed our connection (a kick, an error): what a dropped session does, then
// join again (the offline game never stays disconnected).
static void offline_reconnect(Client *client) {
    const int socket = offline_socket(client);
    if (socket >= 0) sandbox_disconnect(socket);
    char reason[sizeof(client->net_disconnect_reason)];
    pthread_mutex_lock(&client->net_lock);
    snprintf(reason, sizeof(reason), "%s", client->net_disconnect_reason[0] ? client->net_disconnect_reason : "closed");
    client->net_disconnect_reason[0] = 0;
    client->net_disconnect_code[0] = 0;
    pthread_mutex_unlock(&client->net_lock);
    g_offline_fd = -1;
    printf("offline: the sandbox server closed the connection (%s), joining again\n", reason);
    client_clear_players(client);
    if (!offline_connect(client)) fprintf(stderr, "offline: could not join the sandbox server again\n");
}

void client_offline_pump(Client *client, const float delta) {
    if (!client->offline || !sandbox_running()) return;
    sandbox_step(delta > 0.0f ? delta : 0.0);

    const int socket = offline_socket(client);
    if (socket < NET_LOOPBACK_FD_BASE) return;
    uint8_t bytes[2048];
    int n;
    while ((n = sandbox_receive(socket, bytes, sizeof(bytes))) > 0) {
        // like net_poll(): through a NetPacketBuffer in slices, whole packets to the queue
        for (int at = 0; at < n;) {
            const size_t room = sizeof(g_offline_buffer.data) - g_offline_buffer.length;
            const size_t take = (size_t) (n - at) < room ? (size_t) (n - at) : room;
            if (!take || !net_buffer_push(&g_offline_buffer, bytes + at, take)) {
                fprintf(stderr, "offline: receive buffer overflow\n");
                offline_reconnect(client);
                return;
            }
            at += (int) take;
            NetPacket packet;
            int result;
            while ((result = net_buffer_next(&g_offline_buffer, &packet)) > 0) {
                client_net_log_packet("<=", packet.type, packet.payload, packet.length);
                if (packet.type == NET_PACKET_PING && packet.length == sizeof(NetPingPacket)) { // "pi" -> "po" at once
                    client_offline_send(client, NET_PACKET_PONG, packet.payload, packet.length);
                    continue;
                }
                client_net_queue_packet(client, &packet);
            }
            if (result < 0) {
                fprintf(stderr, "offline: malformed packet from the sandbox server\n");
                offline_reconnect(client);
                return;
            }
        }
    }
    if (n < 0) offline_reconnect(client);
}

// Runs the sandbox and applies what it sent right away: a start or a map change shows its
// map before the caller returns (the menu enters the game on game.ready).
static void offline_sync(Client *client) {
    client_offline_pump(client, 0.0f);
    client_tick_net(client);
}

// ---------------------------------------------------------------- start / stop

// The offline game's mode: client->net_mode_index (a Krunker mode id, Host Game) or
// KRUNKNATIVE_MODE (alias / name / id) for scripted runs; Free for All by default.
static int offline_mode_id(const Client *client) {
    const char *mode_env = getenv("KRUNKNATIVE_MODE");
    const int mode_id = mode_env && mode_env[0] ? mode_find(mode_env) : client->net_mode_index;
    return mode_config(mode_id) ? mode_id : MODE_ID_FFA;
}

// Host Game's request (local_game_host_config): the sandbox plays it, until local_game_stop
static cJSON *g_host_request;
static int g_host_type;

void local_game_host_config(Client *client, const cJSON *request, const int host_type) {
    (void) client;
    cJSON_Delete(g_host_request);
    g_host_request = cJSON_IsObject(request) ? cJSON_Duplicate(request, 1) : NULL;
    g_host_type = host_type;
    if (sandbox_running()) sandbox_host_config(g_host_request, g_host_type);
}

static int offline_begin(Client *client, const int map_index) {
    Game *game = &client->game;
    if (!game->map_count) return 0;

    const SandboxConfig cfg = {
        .maps = game->maps,
        .map_count = game->map_count,
        .map_index = map_index,
        .mode_id = offline_mode_id(client),
        .host_request = g_host_request,
        .host_type = g_host_type,
    };
    // GAME.STORAGE / Save / Load Data offline go where the client keeps its own data:
    // $XDG_CONFIG_HOME/krunknative natively, localStorage on the web (the page's MEMFS is lost)
    ks_storage_set_offline_io(html_storage_get, html_storage_set);
    if (!sandbox_start(&cfg)) {
        fprintf(stderr, "offline: could not start the sandbox server\n");
        return 0;
    }

    client->offline = 1;
    client->net_connected = 0;
    client->in_game = 0;
    client->spawn_requested = 0;
    if (!offline_connect(client)) {
        fprintf(stderr, "offline: could not join the sandbox server\n");
        local_game_stop(client);
        return 0;
    }
    offline_sync(client); // INIT: the map loads now
    if (!game->ready) {
        fprintf(stderr, "offline: could not load a map\n");
        local_game_stop(client);
        return 0;
    }

    printf("offline: playing map %d, mode %d on the sandbox server\n", game->current_map_index, game_mode_id(game));
    return 1;
}

int local_game_start(Client *client) {
    // Host Game's pick (offline_start_map), else the startup map (the first of the client's
    // map list, map_list_with_custom), else a random rotation map, like server_bin.
    int map_index = client->startup_map ? 0 : -1;
    const cJSON *pick = client->offline_start_map;
    client->offline_start_map = NULL;
    for (size_t i = 0; pick && i < client->game.map_count; i++) {
        if (client->game.maps[i] == pick) map_index = (int) i;
    }
    client_ruleset_before_offline(client); // the sandbox plays the saved Krunker version (ruleset_ui.h)
    return offline_begin(client, map_index);
}

void local_game_stop(Client *client) {
    if (!client->offline) return;

    const int socket = offline_socket(client);
    if (socket >= 0) sandbox_disconnect(socket);
    sandbox_stop();
    cJSON_Delete(g_host_request); // a later offline game is a plain one again
    g_host_request = NULL;
    g_offline_buffer.length = 0;
    g_offline_fd = -1;
    client->offline = 0;
    client->net_ping_ms = -1;
}

// ---------------------------------------------------------------- host controls

void local_game_host(Client *client, const int map_index, const int mode_id) {
    if (!client->offline || !client->game.map_count || !mode_config(mode_id)) return;
    sandbox_set_maps(client->game.maps, client->game.map_count); // the catalogue may have added maps (map_catalog_slot)
    sandbox_host(map_index >= 0 && (size_t) map_index < client->game.map_count ? map_index : -1, mode_id);
    offline_sync(client);
}

static int readable_file(const char *path) {
    FILE *file = fopen(path, "rb");
    if (file) fclose(file);
    return file != NULL;
}

int local_game_map_lookup(const char *name) {
    if (!name || !*name) return -1;
    const int key = map_catalog_index_of_key(name);
    if (key >= 0) return key;
    // a path under assets/maps: "custom/ESP_Parkour", "maps/custom/ESP_Parkour.json"
    const char *stem = !strncmp(name, "maps/", 5) ? name + 5 : name;
    size_t length = strlen(stem);
    if (length > 5 && !strcmp(stem + length - 5, ".json")) length -= 5;
    char path[256];
    if (length < sizeof(path) - 12) {
        snprintf(path, sizeof(path), "maps/%.*s.json", (int) length, stem);
        for (size_t i = 0; i < map_catalog_count(); i++) {
            const MapCatalogEntry *entry = map_catalog_at(i);
            if (entry && entry->path && !strcmp(entry->path, path)) return (int) i;
        }
    }
    if (strchr(name, '/')) {
        // a file that exists (as given or under assets/maps) is that file, not a catalogue map
        char *under_assets = length < sizeof(path) - 12 ? concat(assets_path(), path) : NULL;
        const int exists = readable_file(name) || (under_assets && ensure_asset_file(under_assets) && readable_file(under_assets));
        free(under_assets);
        // else an old path of a renamed map: "v7/kanji", "custom/old_burg", "assets/maps/burg.json"
        // (v7's Burg; v3.1.1's is 311_burg now, map_catalog.h)
        return exists ? -1 : map_catalog_index_of_file(name);
    }
    const int found = map_catalog_resolve(name, NULL, 0, NULL); // -2 ambiguous; "v3_burg" / "old_burg" too
    return found >= 0 ? found : -1;
}

int local_game_pick_map(Client *client, const int catalog_index) {
    if (catalog_index < 0) return -1;
    const int slot = map_catalog_slot(&client->game.maps, &client->game.map_count, (size_t) catalog_index);
    if (slot < 0) {
        const MapCatalogEntry *entry = map_catalog_at((size_t) catalog_index);
        fprintf(stderr, "offline: map %s could not be loaded\n", entry ? entry->key : "?");
        return -1;
    }
    const Game *game = &client->game;
    if (client->offline) local_game_host(client, slot, game->ready && mode_config(game_mode_id(game)) ? game_mode_id(game) : offline_mode_id(client));
    else client->offline_start_map = client->game.maps[slot];
    return slot;
}

// The editor's Test Map: one map slot of the client's list holds our copy of the last tested
// map (replaced by the next test, freed once neither game plays it).
int local_game_host_custom(Client *client, const cJSON *map) {
    static int test_slot = -1;
    Game *game = &client->game;
    if (!cJSON_IsObject(map)) return 0;

    cJSON *copy = cJSON_Duplicate(map, 1);
    if (!copy) return 0;

    cJSON *old = NULL;
    if (test_slot >= 0 && (size_t) test_slot < game->map_count) {
        old = (cJSON *) game->maps[test_slot];
        game->maps[test_slot] = copy;
    } else {
        const cJSON **list = realloc((void *) game->maps, (game->map_count + 1) * sizeof(*list));
        if (!list) {
            cJSON_Delete(copy);
            return 0;
        }
        list[game->map_count] = copy;
        game->maps = list;
        test_slot = (int) game->map_count++;
    }

    int ok;
    const int mode_id = game->ready && game_mode_id(game) >= 0 ? game_mode_id(game) : MODE_ID_FFA;
    if (client->offline) {
        local_game_host(client, test_slot, mode_id);
    } else {
        // leave a remote session first, like the console's `offline`
        client_net_disconnect(client, "Offline", "000000");
        client->net_should_quit = 1;
        offline_begin(client, test_slot);
    }
    ok = client->offline && game->ready && game->current_map_index == test_slot;

    // both games left the old copy (the sandbox's map changed with the host, ours with its INIT)
    if (old && (!game->map || game->map->raw_data != old)) cJSON_Delete(old);
    return ok;
}
