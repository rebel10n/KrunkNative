#include <server.h>
#include <server_log.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <pcg_basic.h>

#ifdef WIN32
#include <windows.h>
#define NET_ERRNO WSAGetLastError()
#define CLOSE_SOCKET closesocket
#define WOULD_BLOCK(err) ((err) == WSAEWOULDBLOCK)
#define SLEEP_MILLIS(ms) Sleep(ms)
#else
#include <unistd.h>
#define NET_ERRNO errno
#define CLOSE_SOCKET close
#define WOULD_BLOCK(err) ((err) == EAGAIN || (err) == EWOULDBLOCK)
#define SLEEP_MILLIS(ms) do { struct timespec ts = {0, (ms) * 1000000L}; nanosleep(&ts, NULL); } while (0)
#endif

#define NET_ERROR(fd, ...) { server_log_error(__VA_ARGS__); if ((fd) >= 0) CLOSE_SOCKET(fd); g_server->should_quit = 1; return; }

// Wall clock for ping RTTs: g_server->now only advances once per tick.
static double net_time() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec + (double) ts.tv_nsec / 1e9;
}

static ClientConnection *net_client_by_id(const int id) {
    for (size_t i = 0; i < g_server->client_count; i++) {
        if (g_server->clients[i].id == id) return &g_server->clients[i];
    }

    return NULL;
}

// ------------------------------------------------------------------ spectators
// Krunker v3.1.1 spectating (packetHandlers.ts io_ent with ent[6] = 1, ws.spectating):
// the connection has no Player, so it never spawns, scores or shows on the leaderboard,
// and it receives every player's state. Kept by connection id here; ClientConnection
// entries move when the array is compacted.
#define NET_MAX_SPECTATORS 64

static struct {
    int id;
    int target_uid; // "fpSp": the player it follows, -1 free cam (informational, like the v3.1.1 server)
} g_spectators[NET_MAX_SPECTATORS];
static int g_spectator_count;

static int net_spectator_index(const int id) {
    for (int i = 0; i < g_spectator_count; i++) {
        if (g_spectators[i].id == id) return i;
    }

    return -1;
}

static int net_is_spectating(const ClientConnection *client) {
    return client && net_spectator_index(client->id) >= 0;
}

static void net_set_spectating(const ClientConnection *client, const int on) {
    const int index = net_spectator_index(client->id);

    if (on && index < 0 && g_spectator_count < NET_MAX_SPECTATORS) {
        g_spectators[g_spectator_count].id = client->id;
        g_spectators[g_spectator_count].target_uid = -1;
        g_spectator_count++;
    } else if (!on && index >= 0) {
        g_spectators[index] = g_spectators[--g_spectator_count];
    }
}

static void net_send_chat_to(ClientConnection *client, const int uid, const NetChatKind kind, const char *name, const char *text) {
    NetChatPacket packet;
    net_packet_chat(&packet, uid, kind, name, text);
    net_send_packet(client->fd, NET_PACKET_CHAT, &packet, sizeof(packet));
}

static void net_broadcast_chat_except(const ClientConnection *skip, const int uid, const NetChatKind kind, const char *name, const char *text) {
    for (size_t i = 0; i < g_server->client_count; i++) {
        ClientConnection *client = &g_server->clients[i];
        if (client != skip) net_send_chat_to(client, uid, kind, name, text);
    }
}

// config.serverMessages[type] with "$" replaced by the player's name, as a server
// chat line (broadcastServerMessage in the v3.1.1 server).
static void net_broadcast_server_message(const ClientConnection *skip, const char *template, const char *name) {
    char text[NET_CHAT_TEXT_SIZE];
    const char *dollar = strchr(template, '$');

    if (dollar) snprintf(text, sizeof(text), "%.*s%s%s", (int) (dollar - template), template, name && name[0] ? name : "unknown", dollar + 1);
    else snprintf(text, sizeof(text), "%s", template);

    net_broadcast_chat_except(skip, -1, NET_CHAT_SERVER, NULL, text);
}

void net_broadcast_chat(const char *from, const char *msg) {
    char text[NET_CHAT_TEXT_SIZE];
    if (!net_chat_sanitize(text, sizeof(text), msg, msg ? strlen(msg) : 0)) return;

    net_broadcast_chat_except(NULL, -1, NET_CHAT_SERVER, from, text);
}

// Game hooks (GameHooks.send): player uid == ClientConnection.id.
static void net_game_send(Game *game, const int uid, const int type, const void *payload, const uint16_t length) {
    (void) game;

    if (uid <= NET_TARGET_ALL) {
        const int skip = uid <= NET_TARGET_EXCEPT(0) ? NET_TARGET_EXCEPT_UID(uid) : -1;

        for (size_t i = 0; i < g_server->client_count; i++) {
            const ClientConnection *client = &g_server->clients[i];
            if (uid == NET_TARGET_NON_PLAYERS && client->player) continue;
            if (skip >= 0 && client->id == skip) continue;
            net_send_packet(client->fd, (NetPacketType) type, payload, length);
        }
        return;
    }

    ClientConnection *client = net_client_by_id(uid);
    if (client) net_send_packet(client->fd, (NetPacketType) type, payload, length);
}

static void net_change_map_internal(int map_index, int mode_index, int respawn_joined);

// GameHooks.next_round: the end screen ran out, start the next round on a map from
// the rotation (pickWinningMap without votes) and respawn everyone who played.
static void net_game_next_round(Game *game) {
    net_change_map_internal(game_pick_next_map(game), game_pick_next_mode(game), 1);
}

static ClientConnection *net_client_by_id(int id);
static void net_spawn_client(ClientConnection *client);

// GameHooks.respawn: a mode respawns a dead player itself (forceSpawn, Krunker "frs").
static void net_game_respawn(Game *game, const int uid) {
    (void) game;
    ClientConnection *client = net_client_by_id(uid);
    if (client && client->player && !client->player->active && g_server->game.end_timer <= 0.0f) net_spawn_client(client);
}

static int net_alloc_player_number() {
    const unsigned short limit = g_server->config.max_players ? g_server->config.max_players : 8;

    for (int i = 1; i <= limit && i < 32; i++) {
        const unsigned int bit = 1u << i;
        if (g_server->used_player_numbers & bit) continue;

        g_server->used_player_numbers |= bit;
        return i;
    }

    return 0;
}

static void net_release_player_number(ClientConnection *client) {
    if (!client || client->player_number <= 0 || client->player_number >= 32) return;

    g_server->used_player_numbers &= ~(1u << client->player_number);
    client->player_number = 0;
}

static void net_assign_guest_name(ClientConnection *client) {
    if (!client) return;

    snprintf(client->name, sizeof(client->name), "Guest_%d", client->player_number);
}

static void net_free_player(Player *player) {
    if (!player) return;

    free(player->loadout);
    free(player->ammo);
    free(player->reloads);
    free(player->input_queue);
    free(player);
}

static void net_remove_game_player(Player *player) {
    if (!player) return;

    size_t index = SIZE_MAX;

    for (size_t i = 0; i < g_server->game.player_count; i++) {
        if (g_server->game.players[i] == player) {
            index = i;
            break;
        }
    }

    if (index == SIZE_MAX) {
        net_free_player(player);
        return;
    }

    if (index + 1 < g_server->game.player_count) {
        memmove(&g_server->game.players[index], &g_server->game.players[index + 1], (g_server->game.player_count - index - 1) * sizeof(Player*));
    }

    g_server->game.player_count--;

    if (!g_server->game.player_count) {
        free(g_server->game.players);
        g_server->game.players = NULL;
    } else {
        Player **new_players = realloc(g_server->game.players, g_server->game.player_count * sizeof(Player*));
        if (new_players) g_server->game.players = new_players;
    }

    net_free_player(player);
}

static void net_mark_client_spawned(ClientConnection *client) {
    if (!client || !client->player) return;

    client->spawned_at = g_server->now;
    client->last_significant_move_at = g_server->now;
    client->idle_position = client->player->position;
    client->has_idle_position = 1;
}

static void net_set_nonblocking(const int fd) {
#ifdef WIN32
    unsigned long non_blocking = 1;
    ioctlsocket(fd, FIONBIO, &non_blocking);
#else
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
}

static const cJSON *net_current_map_json() {
    const Game *game = &g_server->game;
    const int index = game->current_map_index;
    return index >= 0 && (size_t) index < game->map_count ? game->maps[index] : NULL;
}

// MAP_INFO before INIT: map indices differ between server and client map lists
// (a custom server map, other rotations), so the client resolves the map by hash
// and asks for the JSON (MAP_REQUEST) when it does not have it.
static void net_send_map_info(ClientConnection *client) {
    const cJSON *map = net_current_map_json();
    size_t size = 0;
    const uint32_t hash = net_map_hash(map, NULL, &size);
    if (!hash) return;

    NetMapInfoPacket packet = {.map_index = g_server->game.current_map_index, .hash = hash, .size = (uint32_t) size};
    const cJSON *name = cJSON_GetObjectItem(map, "name");
    if (cJSON_IsString(name)) snprintf(packet.name, sizeof(packet.name), "%s", name->valuestring);

    net_send_packet(client->fd, NET_PACKET_MAP_INFO, &packet, sizeof(packet));
}

// Stream the map JSON the client asked for (any map of the rotation, by hash).
static void net_handle_map_request(ClientConnection *client, const NetPacket *packet) {
    if (packet->length != sizeof(NetMapRequestPacket)) return;

    NetMapRequestPacket request;
    memcpy(&request, packet->payload, sizeof(request));

    for (size_t i = 0; i < g_server->game.map_count; i++) {
        const cJSON *map = g_server->game.maps[i];
        size_t size = 0;
        if (net_map_hash(map, NULL, &size) != request.hash) continue;

        char *json = NULL;
        net_map_hash(map, &json, &size);
        if (!json) return;

        if (size > NET_MAP_MAX_SIZE) {
            server_log("Not sending map %zu to %s: %zu bytes is over the %u byte limit", i, client->name, size, NET_MAP_MAX_SIZE);
            cJSON_free(json);
            return;
        }

        server_log("Sending map %zu (%zu bytes) to %s", i, size, client->name);

        NetMapDataPacket chunk = {.hash = request.hash};
        for (size_t offset = 0; offset < size; offset += NET_MAP_CHUNK_SIZE) {
            const size_t length = size - offset < NET_MAP_CHUNK_SIZE ? size - offset : NET_MAP_CHUNK_SIZE;
            chunk.offset = (uint32_t) offset;
            chunk.length = (uint16_t) length;
            memcpy(chunk.data, json + offset, length);
            if (!net_send_packet(client->fd, NET_PACKET_MAP_DATA, &chunk, (uint16_t) (NET_MAP_DATA_HEADER_SIZE + length))) break;
        }

        cJSON_free(json);
        return;
    }
}

static void net_send_init(ClientConnection *client) {
    net_send_map_info(client);

    const NetInitPacket packet = {
        .major = 1,
        .minor = 0,
        .patch = 0,
        .map_index = g_server->game.current_map_index,
        .mode_index = game_mode_id(&g_server->game), // Krunker "init" modeIndex: the mode id
        .tick_rate = g_server->config.tick_rate,
        .client_id = client->id,
    };

    net_send_packet(client->fd, NET_PACKET_INIT, &packet, sizeof(packet));
}

static void net_send_spawn(ClientConnection *client, const Player *player, const int is_you) {
    NetSpawnPacket packet = {
        .uid = player->uid,
        .is_you = (uint8_t) is_you,
        .active = player->active,
        .class_index = player->class_index,
        .secondary_index = player->secondary_index,
        .cosmetics = player->cosmetics,
        .team = player->team,
        .health = player->health,
        .max_health = player->max_health,
        .position = {player->position.x, player->position.y, player->position.z},
        .direction = {player->direction.x, player->direction.y},
    };

    strncpy(packet.name, player->name, sizeof(packet.name) - 1);

    net_send_packet(client->fd, NET_PACKET_SPAWN, &packet, sizeof(packet));
}

static void net_broadcast_spawn(const Player *player) {
    for (size_t i = 0; i < g_server->client_count; i++) {
        ClientConnection *client = &g_server->clients[i];
        net_send_spawn(client, player, client->player == player);
    }
}

// Every known player, dead ones too (active = 0): the scoreboard needs their names.
static void net_send_existing_spawns(ClientConnection *client) {
    for (size_t i = 0; i < g_server->game.player_count; i++) {
        Player *player = g_server->game.players[i];
        if (!player || player == client->player) continue;
        net_send_spawn(client, player, 0);
    }
}

// "start" with spectating = true plus what io_ent streams to a new spectator: every
// player (dead ones too, for names), the leaderboard and the round clock.
static void net_send_spectate_state(ClientConnection *client) {
    const NetSpectatePacket ack = {.spectating = 1};
    net_send_packet(client->fd, NET_PACKET_SPECTATE, &ack, sizeof(ack));
    net_send_existing_spawns(client);
    game_sync_leaders(&g_server->game, client->id);
    game_send_round_state(&g_server->game, client->id);
}

// ENT with spectate = 1: a live player leaves the match first (LEAVE to the others),
// then the connection spectates until an ENT without the flag spawns it.
static void net_start_spectating(ClientConnection *client) {
    net_set_spectating(client, 1);

    if (client->player) {
        Player *player = client->player;
        player->active = 0;
        game_player_left(&g_server->game, player);

        const NetLeavePacket leave = {.uid = player->uid};
        for (size_t i = 0; i < g_server->client_count; i++) {
            if (&g_server->clients[i] != client) net_send_packet(g_server->clients[i].fd, NET_PACKET_LEAVE, &leave, sizeof(leave));
        }

        net_remove_game_player(player);
        client->player = NULL;
        client->joined = 0;
        client->spawned_at = 0.0f;
        client->has_idle_position = 0;
    }

    server_log("%s is spectating", client->name);
    net_send_spectate_state(client);
}

static void net_spawn_client(ClientConnection *client) {
    const int first_spawn = !client->player;

    if (!client->player) {
        Player *player = player_init(&g_server->game);
        if (!player) return;

        player->uid = client->id;
        client->player = player;
        game_players_add(&g_server->game, player);
    }

    snprintf(client->player->name, sizeof(client->player->name), "%s", client->name);
    client->player->class_index = client->class_index;
    client->player->secondary_index = client->secondary_index;
    client->player->cosmetics = client->cosmetics;
    player_spawn(client->player);
    client->joined = 1;
    net_mark_client_spawned(client);
    if (first_spawn) net_send_existing_spawns(client);
    net_broadcast_spawn(client->player);

    if (first_spawn) {
        // io_ent first spawn: leaderboard + round clock for the joiner, then
        // broadcastServerMessage(0). The joiner's own HUD already seeds its
        // "joined the game" line (hud_ui.c hud_seed_chat), so it is skipped here.
        game_sync_leaders(&g_server->game, client->id);
        game_send_round_state(&g_server->game, client->id);

        if (!client->announced) {
            client->announced = 1;
            net_broadcast_server_message(client, "$ joined the game", client->name);
        }
    }
}

// ENT from a client: (re)spawn unless the round is over, the player is alive, or
// it died less than config.deathDelay ago.
static void net_request_spawn(ClientConnection *client) {
    if (!game_can_spawn(&g_server->game, client->player)) return; // shared with the offline client

    net_spawn_client(client);
}

static void net_free_players() {
    for (size_t i = 0; i < g_server->client_count; i++) {
        g_server->clients[i].player = NULL;
        g_server->clients[i].spawned_at = 0.0f;
        g_server->clients[i].has_idle_position = 0;
    }

    for (size_t i = 0; i < g_server->game.player_count; i++) {
        Player *player = g_server->game.players[i];
        net_free_player(player);
    }

    free(g_server->game.players);
    g_server->game.players = NULL;
    g_server->game.player_count = 0;
}

// Re-init the game on another map/mode and announce it (INIT). With
// respawn_joined every client that played the round respawns (next round);
// otherwise only the players alive right now do (CYCLE / CLI `map`).
static void net_change_map_internal(const int map_index, const int mode_index, const int respawn_joined) {
    if (!g_server->game.map_count) return;

    uint8_t *respawn_clients = NULL;

    if (g_server->client_count) {
        respawn_clients = calloc(g_server->client_count, sizeof(uint8_t));

        if (respawn_clients) {
            for (size_t i = 0; i < g_server->client_count; i++) {
                const ClientConnection *client = &g_server->clients[i];
                respawn_clients[i] = client->player && (client->player->active || (respawn_joined && client->joined));
            }
        }
    }

    net_free_players();

    for (size_t i = 0; i < g_server->client_count; i++) g_server->clients[i].joined = 0;

    game_init(&g_server->game, map_index, mode_index, 0);

    server_log("Loaded map %d mode %d", g_server->game.current_map_index, g_server->game.current_mode_index);

    for (size_t i = 0; i < g_server->client_count; i++) {
        net_send_init(&g_server->clients[i]);
    }

    if (respawn_clients) {
        for (size_t i = 0; i < g_server->client_count; i++) {
            if (respawn_clients[i]) net_spawn_client(&g_server->clients[i]);
        }

        free(respawn_clients);
    }

    // INIT resets every client's session: spectators get their spectate state again.
    for (size_t i = 0; i < g_server->client_count; i++) {
        if (net_is_spectating(&g_server->clients[i])) net_send_spectate_state(&g_server->clients[i]);
    }
}

void net_change_map(const int map_index, const int mode_index) {
    net_change_map_internal(map_index, mode_index, 0);
}

static void net_cycle_map() {
    if (!g_server->game.map_count) return;
    net_change_map((g_server->game.current_map_index + 1) % (int) g_server->game.map_count, g_server->game.current_mode_index);
}

// ------------------------------------------------------------------ chat commands
// Krunker v3.1.1 "/" commands (shared/src/chat_commands.c) over the connections. Kicks
// and bans close the target after the current poll pass (net_main), so a command never
// compacts g_server->clients while net_poll_client() is walking it.

#define NET_PENDING_KICKS_MAX 32

static struct {
    int id;
    NetErrorPacket error;
} g_pending_kicks[NET_PENDING_KICKS_MAX];
static int g_pending_kick_count;
static ChatCommandHost g_chat_host;

static int net_chat_peer_count(void *user) {
    (void) user;
    return (int) g_server->client_count;
}

static int net_chat_peer_at(void *user, const int index, ChatPeer *out) {
    (void) user;
    if (index < 0 || (size_t) index >= g_server->client_count) return 0;
    const ClientConnection *client = &g_server->clients[index];
    *out = (ChatPeer) {
        .id = client->id,
        .name = client->name,
        .address = client->address,
        .ping_ms = client->ping_ms,
        .player = client->player,
        .spectating = net_is_spectating(client),
        // perms on: CLI-op'd connections and the server's own machine count as DEV
        .dev = client->op || !strcmp(client->address, "127.0.0.1"),
    };
    return 1;
}

static void net_chat_disconnect(void *user, const int id, const char *message, const char *code) {
    (void) user;
    if (g_pending_kick_count >= NET_PENDING_KICKS_MAX) return;
    g_pending_kicks[g_pending_kick_count].id = id;
    NetErrorPacket *error = &g_pending_kicks[g_pending_kick_count].error;
    memset(error, 0, sizeof(*error));
    snprintf(error->message, sizeof(error->message), "%s", message ? message : "Disconnected");
    snprintf(error->code, sizeof(error->code), "%s", code ? code : "000000");
    g_pending_kick_count++;
}

static void net_chat_change_map(void *user, const int map_index, const int mode_index) {
    (void) user;
    net_change_map(map_index, mode_index);
}

static void net_chat_log(void *user, const char *text) {
    (void) user;
    server_log("[Server] %s", text);
}

ChatCommandHost *net_chat_host(void) {
    g_chat_host.game = &g_server->game;
    g_chat_host.perms = g_server->config.perms;
    g_chat_host.lobby = "native";
    g_chat_host.region = "local";
    g_chat_host.tick_rate = g_server->config.tick_rate ? (float) g_server->config.tick_rate : 30.0f;
    g_chat_host.peer_count = net_chat_peer_count;
    g_chat_host.peer_at = net_chat_peer_at;
    g_chat_host.disconnect = net_chat_disconnect;
    g_chat_host.change_map = net_chat_change_map;
    g_chat_host.log = net_chat_log;
    return &g_chat_host;
}

// ws.close(4002 / 4003) for the targets queued by the last commands. Caller holds the lock.
static void net_process_pending_kicks(void) {
    for (int i = 0; i < g_pending_kick_count; i++) {
        ClientConnection *client = net_client_by_id(g_pending_kicks[i].id);
        if (!client) continue;
        net_send_packet(client->fd, NET_PACKET_ERROR, &g_pending_kicks[i].error, sizeof(g_pending_kicks[i].error));
        server_log("%s %s (id %d)", g_pending_kicks[i].error.message, client->name, client->id);
        net_remove_client(client);
    }
    g_pending_kick_count = 0;
}

void net_add_client(ClientConnection *client) {
    if (g_server->config.max_players && g_server->client_count >= g_server->config.max_players) {
        server_log("Rejecting client: server full (%zu/%u)", g_server->client_count, (unsigned int) g_server->config.max_players);
        CLOSE_SOCKET(client->fd);
        return;
    }

    snprintf(client->address, sizeof(client->address), "%s", inet_ntoa(client->addr.sin_addr));
    if (chat_command_ip_banned(net_chat_host(), client->address)) { // pnhBannedAddresses: close(4003, "banned")
        const NetErrorPacket error = {.message = "Banned", .code = "004003"};
        net_send_packet(client->fd, NET_PACKET_ERROR, &error, sizeof(error));
        server_log("Rejecting %s: IP banned", client->address);
        CLOSE_SOCKET(client->fd);
        return;
    }

    ClientConnection *new_clients = realloc(g_server->clients, (g_server->client_count + 1) * sizeof(ClientConnection));

    if (!new_clients) {
        CLOSE_SOCKET(client->fd);
        return;
    }

    g_server->clients = new_clients;
    client->id = g_server->next_client_id++;
    client->player_number = net_alloc_player_number();

    if (!client->player_number) {
        CLOSE_SOCKET(client->fd);
        return;
    }

    net_assign_guest_name(client);
    client->connected_at = g_server->now;
    client->spawned_at = 0.0f;
    client->last_significant_move_at = g_server->now;
    client->has_idle_position = 0;
    client->joined = 0;
    client->announced = 0;
    client->voice_player = 0;
    client->class_index = 0;
    client->secondary_index = -1;
    client->cosmetics = PLAYER_COSMETICS_NONE;
    client->chat_limiter = (NetChatLimiter) {0};
    client->ping_seq = 0;
    client->ping_ms = -1;
    client->next_ping_at = 0.0; // first probe right away (pi on connect)
    client->social = (SocialAssembly) {0};
    client->voice = (SocialAssembly) {0};
    g_server->clients[g_server->client_count] = *client;

    ClientConnection *stored_client = &g_server->clients[g_server->client_count++];
    on_client_connect(stored_client);
    net_send_init(stored_client);
    game_send_round_state(&g_server->game, stored_client->id);
}

void net_remove_client(ClientConnection *client) {
    size_t index = SIZE_MAX;

    for (size_t i = 0; i < g_server->client_count; i++) {
        if (&g_server->clients[i] == client) {
            index = i;
            break;
        }
    }

    if (index == SIZE_MAX) return;

    on_client_disconnect(client);
    net_set_spectating(client, 0);
    if (client->player) {
        client->player->active = 0;
        game_player_left(&g_server->game, client->player);
        const NetLeavePacket leave = {.uid = client->player->uid};
        for (size_t i = 0; i < g_server->client_count; i++) {
            if (&g_server->clients[i] != client) net_send_packet(g_server->clients[i].fd, NET_PACKET_LEAVE, &leave, sizeof(leave));
        }
        net_remove_game_player(client->player);
        client->player = NULL;
    }
    if (client->announced) {
        client->announced = 0;
        net_broadcast_server_message(client, "$ left the game", client->name); // serverMessages[1]
    }
    net_release_player_number(client);
    social_assembly_free(&client->social);
    social_assembly_free(&client->voice);
    CLOSE_SOCKET(client->fd);

    if (index + 1 < g_server->client_count) {
        memmove(&g_server->clients[index], &g_server->clients[index + 1], (g_server->client_count - index - 1) * sizeof(ClientConnection));
    }

    g_server->client_count--;

    if (!g_server->client_count) {
        free(g_server->clients);
        g_server->clients = NULL;
        return;
    }

    ClientConnection *new_clients = realloc(g_server->clients, g_server->client_count * sizeof(ClientConnection));
    if (new_clients) g_server->clients = new_clients;
}

// io_ct: burst limit (closing the socket like the v3.1.1 server, code 4008),
// 70 characters, blank lines dropped, then "ch" to everyone.
static int net_handle_chat(ClientConnection *client, const NetPacket *packet) {
    if (!net_chat_allow(&client->chat_limiter, (float) net_time())) {
        server_log("Kicking %s: chat rate limit exceeded", client->name);
        net_remove_client(client);
        return 0;
    }

    char text[NET_CHAT_TEXT_SIZE];
    if (!net_chat_sanitize(text, sizeof(text), (const char *) packet->payload, packet->length)) return 1;

    if (chat_command_handle(net_chat_host(), client->id, text)) { // "/" commands answer the sender only
        server_log("[cmd] %s: %s", client->name, text);
        return 1;
    }

    server_log("[chat] %s: %s", client->name, text);
    net_broadcast_chat_except(NULL, client->id, NET_CHAT_PLAYER, client->name, text);
    return 1;
}

static void net_handle_pong(ClientConnection *client, const NetPacket *packet) {
    if (packet->length != sizeof(NetPingPacket) || !client->ping_seq) return;

    NetPingPacket pong;
    memcpy(&pong, packet->payload, sizeof(pong));
    if (pong.seq != client->ping_seq) return;

    const double now = net_time();
    client->ping_ms = net_ping_update(client->ping_ms, (float) (now - client->ping_sent_at));
    client->ping_seq = 0;
    client->next_ping_at = now + NET_PING_INTERVAL;

    const NetPingResultPacket result = {.ping_ms = client->ping_ms};
    net_send_packet(client->fd, NET_PACKET_PING_RESULT, &result, sizeof(result));
}

static int net_social_send(void *user, const NetPacketType type, const void *payload, const uint16_t length) {
    const ClientConnection *client = user;
    return net_send_packet(client->fd, type, payload, length);
}

// Social hub frames (shared/include/social.h): reassemble the request, answer it from
// the server's map list, stream the reply back in chunks.
static void net_handle_social(ClientConnection *client, const NetPacket *packet) {
    char *request = NULL;
    uint32_t id = 0;
    if (social_assembly_push(&client->social, packet->payload, packet->length, &request, &id) != 1) return;

    SocialMapInfo maps[64];
    const SocialHost host = {.maps = maps, .map_count = social_maps_from_game(&g_server->game, maps, 64)};
    char *reply = social_service_handle(&host, request);
    free(request);
    if (!reply) return;
    social_send_message(NET_PACKET_SOCIAL_REPLY, id, reply, strlen(reply), net_social_send, client);
    free(reply);
}

static int net_voice_send(void *user, const int peer, const NetPacketType type, const void *payload, const uint16_t length) {
    (void) user;
    return net_send_packet(g_server->clients[peer].fd, type, payload, length);
}

// io_vc (shared/src/voice.c): a clip from a connection that has a v3.1.1 player (voice_player) goes
// to every other connection as ["vc", audio, alias, sid].
static void net_handle_voice(ClientConnection *client, const NetPacket *packet) {
    const VoiceSpeaker speaker = {
        .sid = client->id, // player.sid: the uid net_spawn_client gives this connection's player
        .alias = client->name,
        .has_player = client->voice_player,
        .peer = (int) (client - g_server->clients),
    };
    voice_relay(&client->voice, packet->payload, packet->length, &speaker, (int) g_server->client_count, net_voice_send, NULL);
}

// Returns 0 when the client was removed.
static int net_handle_packet(ClientConnection *client, const NetPacket *packet) {
    switch (packet->type) {
        case NET_PACKET_HELLO:
            break;
        case NET_PACKET_ENT: {
            NetEntPacket ent;
            if (!net_packet_read_ent(&ent, packet)) break;

            client->class_index = ent.class_index;
            client->secondary_index = ent.secondary_index;
            client->cosmetics = ent.cosmetics;

            if (ent.spectate) {
                client->voice_player = 0; // io_ent spectate: ctx.player = null
                net_start_spectating(client);
            } else {
                client->voice_player = 1; // io_ent: ctx.player = ctx.server.addPlayer(...) on the first ENT
                net_request_spawn(client);
                // A spectator plays again once it actually spawned (not on the end screen).
                if (client->player && client->player->active) net_set_spectating(client, 0);
            }
            break;
        }
        case NET_PACKET_SPECTATE_TARGET: {
            const int index = net_spectator_index(client->id);
            if (packet->length != sizeof(NetSpectateTargetPacket) || index < 0) break;

            NetSpectateTargetPacket target;
            memcpy(&target, packet->payload, sizeof(target));
            g_spectators[index].target_uid = target.uid;
            break;
        }
        case NET_PACKET_CHAT_SEND:
            if (packet->length && packet->length <= sizeof(NetChatSendPacket)) return net_handle_chat(client, packet);
            break;
        case NET_PACKET_PONG:
            net_handle_pong(client, packet);
            break;
        case NET_PACKET_MAP_REQUEST:
            net_handle_map_request(client, packet);
            break;
        case NET_PACKET_INPUT: {
            NetInputPacket net_input;
            Input input;

            if (!client->player || !client->player->active || !net_packet_read_input(&net_input, packet)) break;
            net_packet_to_input(&input, &net_input);
            player_queue_input(client->player, &input);
            break;
        }
        case NET_PACKET_CYCLE:
            if (!packet->length) net_cycle_map();
            break;
        case NET_PACKET_SOCIAL_REQUEST:
            net_handle_social(client, packet);
            break;
        case NET_PACKET_VOICE_SEND:
            net_handle_voice(client, packet);
            break;
        default:
            on_client_event(client);
            break;
    }

    return 1;
}

int net_poll_client(ClientConnection *client) {
    char buffer[1024];
    int read;

    if ((read = recv(client->fd, buffer, sizeof(buffer), 0)) <= 0) {
        if (read < 0 && WOULD_BLOCK(NET_ERRNO)) return 1;

        net_remove_client(client);
        return 0;
    }

    if (!net_buffer_push(&client->read_buffer, buffer, (size_t) read)) {
        net_remove_client(client);
        return 0;
    }

    NetPacket packet;
    int result;

    while ((result = net_buffer_next(&client->read_buffer, &packet)) > 0) {
        if (!net_handle_packet(client, &packet)) return 0;
    }

    if (result < 0) {
        net_remove_client(client);
        return 0;
    }

    return 1;
}

void net_main() {

#ifdef WIN32
    WSADATA wsa_data;
    WSAStartup(MAKEWORD(2, 2), &wsa_data);
#endif

    const int server_socket = (int) socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server_socket < 0) NET_ERROR(-1, "Failed to create server socket, quitting...");

    // Rebind right after a restart even while old connections sit in TIME_WAIT.
    // Windows' SO_REUSEADDR would let a second server share a listening port, so
    // it gets SO_EXCLUSIVEADDRUSE instead (TIME_WAIT never blocks a bind there).
    const int reuse = 1;
#ifdef WIN32
    setsockopt(server_socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *) &reuse, sizeof(reuse));
#else
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, (const char *) &reuse, sizeof(reuse));
#endif

    // Match events leave the shared game through these hooks (see GameHooks).
    pthread_mutex_lock(&g_server->lock);
    g_server->game.hooks.user = g_server;
    g_server->game.hooks.send = net_game_send;
    g_server->game.hooks.next_round = net_game_next_round;
    g_server->game.hooks.respawn = net_game_respawn;
    pthread_mutex_unlock(&g_server->lock);

    struct sockaddr_in listen_addr;

    listen_addr.sin_family = AF_INET;
    listen_addr.sin_addr.s_addr = INADDR_ANY;
    listen_addr.sin_port = htons(g_server->config.listen_port);

    if (bind(server_socket, (struct sockaddr *) &listen_addr, sizeof(listen_addr)) < 0) {
        NET_ERROR(server_socket, "Failed to bind to port %d (is it in use?), quitting...", g_server->config.listen_port);
    }

    if (listen(server_socket, 5) < 0) NET_ERROR(server_socket, "Failed to listen on port %d, quitting...", g_server->config.listen_port);

    net_set_nonblocking(server_socket);

    server_log("Listening on port %d", g_server->config.listen_port);

    while (!g_server->should_quit) {
        pthread_mutex_lock(&g_server->lock);

        for (size_t i = 0; i < g_server->client_count; i++) {
            if (!net_poll_client(&g_server->clients[i])) i--;
        }
        net_process_pending_kicks(); // chat /kick, /ban, /banip (and the CLI running them)

        pthread_mutex_unlock(&g_server->lock);

        struct sockaddr_in client_addr;
#ifdef WIN32
        int client_addr_size = sizeof(client_addr);
#else
        socklen_t client_addr_size = sizeof(client_addr);
#endif

        const int client = (int) accept(server_socket, (struct  sockaddr *) &client_addr, &client_addr_size);

        if (client < 0) {
            if (WOULD_BLOCK(NET_ERRNO)) {
                SLEEP_MILLIS(1);
                continue;
            }

            break;
        }

        net_set_nonblocking(client);

        ClientConnection conn = {};

        conn.fd = client;
        conn.addr = client_addr;

        pthread_mutex_lock(&g_server->lock);
        net_add_client(&conn);
        pthread_mutex_unlock(&g_server->lock);
    }

    CLOSE_SOCKET(server_socket);
}

// kickIdleSocket: ["error", "Kicked for inactivity", "000005"], then close.
static void net_kick_idle(ClientConnection *client, const char *phase) {
    NetErrorPacket error = {.message = "Kicked for inactivity", .code = "000005"};
    net_send_packet(client->fd, NET_PACKET_ERROR, &error, sizeof(error));
    server_log("Kicking %s for inactivity %s", client->name, phase);
    net_remove_client(client);
}

// gameserver.ts checkIdleTimeouts (v3.1.1 server): 60 s without moving 1 unit kicks,
// dead players included; before the first spawn the clock runs from the connect.
// Nobody is idle on the end screen (players.clear() leaves no player to check); the
// respawn of the next round counts as movement.
void net_check_idle_clients(const float now) {
    const float timeout = g_server->config.idle_timeout;
    if (timeout <= 0.0f) return;

    const float threshold = g_server->config.idle_move_threshold;
    const float threshold_sq = threshold * threshold;

    for (size_t i = 0; i < g_server->client_count; i++) {
        ClientConnection *client = &g_server->clients[i];
        Player *player = client->player;

        if (g_server->game.end_timer > 0.0f) {
            client->last_significant_move_at = now;
            continue;
        }

        if (!player || !player->active) {
            if (net_is_spectating(client)) continue; // spectators are never idle-kicked (index.js: !window.Bf5)

            const float idle_since = client->spawned_at > 0.0f ? client->last_significant_move_at : client->connected_at;
            if (now - idle_since >= timeout) {
                net_kick_idle(client, client->spawned_at > 0.0f ? "in game (dead)" : "before spawn");
                i--;
            }
            continue;
        }

        if (!client->has_idle_position) {
            net_mark_client_spawned(client);
            continue;
        }

        const float dx = player->position.x - client->idle_position.x;
        const float dy = player->position.y - client->idle_position.y;
        const float dz = player->position.z - client->idle_position.z;
        const float dist_sq = dx * dx + dy * dy + dz * dz;

        if (dist_sq >= threshold_sq) {
            client->idle_position = player->position;
            client->last_significant_move_at = now;
        }

        if (now - client->last_significant_move_at >= timeout) {
            net_kick_idle(client, "in game");
            i--;
        }
    }
}

// pi probes: one outstanding ping per client, NET_PING_INTERVAL after each pong
// (a lost probe is retried after twice the interval).
static void net_send_pings() {
    const double now = net_time();

    for (size_t i = 0; i < g_server->client_count; i++) {
        ClientConnection *client = &g_server->clients[i];
        const int stale = client->ping_seq && now - client->ping_sent_at >= NET_PING_INTERVAL * 2.0f;
        if ((client->ping_seq && !stale) || now < client->next_ping_at) continue;

        client->ping_seq = pcg32_random() | 1u;
        client->ping_sent_at = now;

        const NetPingPacket packet = {.seq = client->ping_seq};
        net_send_packet(client->fd, NET_PACKET_PING, &packet, sizeof(packet));
    }
}

// Per tick (main thread): pings, then the player sync (Krunker "k"; none on the end screen).
void net_broadcast_states() {
    net_send_pings();
    if (g_server->game.end_timer > 0.0f) return;

    for (size_t i = 0; i < g_server->client_count; i++) {
        ClientConnection *client = &g_server->clients[i];

        for (size_t j = 0; j < g_server->game.player_count; j++) {
            Player *player = g_server->game.players[j];
            if (!player || !player->active) continue;

            NetStatePacket packet;
            net_packet_from_player(&packet, player);
            net_send_packet(client->fd, NET_PACKET_STATE, &packet, sizeof(packet));
        }
    }
}

void net_cleanup() {
#ifdef WIN32
    WSACleanup();
#endif
}
