// The server core's connections (server.h): packet dispatch, spawns, map changes, chat
// commands and the per-tick broadcast. The bytes move through each connection's transport
// (ClientConnection.transport): TCP sockets (net_tcp.c), UDP peers (net_udp.c) or the
// in-process sandbox (sandbox.c); the TCP listener and the net thread live in net_tcp.c.
#include <server.h>
#include <server_log.h>
#include <server_accounts.h> // backend: JOIN, PLAYER_INFO, stats, billboards (doc/backend.md section 8)
#include <brain.h>           // brain_enabled(): the brain owns matchmaking and the hub
#include <ai.h>
#include <bots.h>            // Krunker v7.1.4 bot players (shared/src/bots.c)
#include <ks_server.h>       // KrunkScript server host (server/src/ks_server_host.c)
#include <ks_game.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <pcg_basic.h>

#ifdef WIN32
#include <ws2tcpip.h> // inet_pton
#endif

// Client fds close through their transport (sockets, UDP peers, sandbox connections).
#define CLOSE_CLIENT_FD(client) ((client)->transport->close((client)->fd))

// Wall clock for ping RTTs: g_server->now only advances once per tick.
static double net_time() {
    return server_time();
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
    if (client && type == NET_PACKET_END && length == sizeof(NetEndPacket)) accounts_on_end_packet(uid, payload); // didWin for the commit
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

// GameHooks.reward: player.reward at endGame (accounts only, server/src/accounts.c).
static int net_game_reward(Game *game, const Player *player) {
    (void) game;
    return accounts_reward(player);
}

// GameHooks.respawn: a mode respawns a dead player itself (forceSpawn, Krunker "frs").
static void net_game_respawn(Game *game, const int uid) {
    (void) game;
    ClientConnection *client = net_client_by_id(uid);
    if (client && client->player && !client->player->active && g_server->game.end_timer <= 0.0f) net_spawn_client(client);
}

// GameHooks.alloc_uid: a bot player's uid comes from the connection id counter, so no
// connection ever gets the same one.
static int net_game_alloc_uid(Game *game) {
    (void) game;
    return g_server->next_client_id++;
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

// One tick's frames for one client go out in a single send(): Krunker sends its per-tick
// sync as one ws message, and ~one send() per player per client per tick (with
// net_send_packet() spinning on EAGAIN) held g_server->lock for milliseconds at full lobbies.
#define NET_BATCH_SIZE 8192

typedef struct {
    size_t length;
    uint8_t data[NET_BATCH_SIZE];
} NetSendBatch;

static int net_batch_append(NetSendBatch *batch, const NetPacketType type, const void *payload, const uint16_t length) {
    if (batch->length + NET_PACKET_HEADER_SIZE + length > sizeof(batch->data)) return 0;
    uint8_t *frame = batch->data + batch->length;
    frame[0] = (uint8_t) type;
    frame[1] = (uint8_t) (length >> 8);
    frame[2] = (uint8_t) (length & 0xff);
    if (payload && length) memcpy(frame + NET_PACKET_HEADER_SIZE, payload, length);
    batch->length += NET_PACKET_HEADER_SIZE + length;
    return 1;
}

// Sends the batch through the connection's transport (TCP: one send(), UDP: one knet message
// per frame, sandbox: the in-memory queue).
static void net_batch_flush(const ClientConnection *client, NetSendBatch *batch) {
    if (batch->length) client->transport->send_frames(client->fd, batch->data, batch->length);
    batch->length = 0;
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
    accounts_send_billboard(client); // init "bill" (config gameserver.billboards), before the map loads
    net_send_map_info(client);

    const NetInitPacket packet = {
        .major = NET_PROTOCOL_MAJOR,
        .minor = NET_PROTOCOL_MINOR, // 1.1: accepts ENT V5 / V6, 1.2: V7 (client_enter_game sends them only then)
        .patch = 0,
        .map_index = g_server->game.current_map_index,
        .mode_index = game_mode_id(&g_server->game), // Krunker "init" modeIndex: the mode id
        .tick_rate = g_server->config.tick_rate,
        .client_id = client->id,
    };

    net_send_packet(client->fd, NET_PACKET_INIT, &packet, sizeof(packet));
    game_ai_sync_non_players(&g_server->game); // a connection without a player: every AI's "aai" next AI.sync
}

static void net_send_spawn(ClientConnection *client, const Player *player, const int is_you) {
    // The weapon / melee skins first: the client builds the player's meshes on SPAWN.
    const NetSkinsPacket skins = {.uid = player->uid, .skins = player->skins};
    net_send_packet(client->fd, NET_PACKET_SKINS, &skins, sizeof(skins));

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
    accounts_send_infos_to(client);
    game_bots_send_infos(&g_server->game, client->id); // bot players' account-style PLAYER_INFO
    game_sync_leaders(&g_server->game, client->id);
    game_send_round_state(&g_server->game, client->id);
    game_ai_sync_non_players(&g_server->game); // AI.sync sends spectators every AI ("aai" first)
}

// ENT with spectate = 1: a live player leaves the match first (LEAVE to the others),
// then the connection spectates until an ENT without the flag spawns it.
static void net_start_spectating(ClientConnection *client) {
    net_set_spectating(client, 1);

    if (client->player) {
        Player *player = client->player;
        ks_server_on_leave(client); // KrunkScript onPlayerLeave: the player leaves the match
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
        player->team = accounts_assigned_team(client); // ranked roster team (players.setTeam isFromQueue), 0 = mode picks
        player->ping = client->ping_ms > 0 ? (float) client->ping_ms : 0.0f; // lag compensation (getStateConst)
        client->player = player;
        game_players_add(&g_server->game, player);
    }

    snprintf(client->player->name, sizeof(client->player->name), "%s", client->name);
    client->player->class_index = client->class_index;
    client->player->secondary_index = client->secondary_index;
    client->player->cosmetics = client->cosmetics;
    client->player->slid_cont = client->slid_cont; // gameserver.ts: player.slidCont = ent[15]
    client->player->spray_index = client->spray_index; // applyEntData: player.sprayIndex = ent[1]
    client->player->skins = client->skins;             // applyEntData: player.skins = ent[2], meleeIndex = ent[9]
    client->player->lag_comp = client->lag_comp;       // applyLagCompFromEnt: player.lagComp = ent[11]
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
            if (accounts_join_message()[0]) net_broadcast_server_message(client, accounts_join_message(), client->name);
        }
    }

    accounts_on_spawn(client, first_spawn); // PLAYER_INFO (account level / clan), MOTD on the first spawn
    if (first_spawn) game_bots_send_infos(&g_server->game, client->id); // the bots' level / clan, like accounts_send_infos_to
}

// ENT from a client: (re)spawn unless the round is over, the player is alive, or
// it died less than config.deathDelay ago.
static void net_request_spawn(ClientConnection *client) {
    // io_ent in the vote window: spawn anyway, then the late "end" (didWin -1), so the
    // joiner gets the end screen instead of a closed menu over nothing.
    if (game_end_late_spawn(&g_server->game, client->player)) {
        net_spawn_client(client);
        game_send_end_late(&g_server->game, client->id);
        return;
    }
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

    accounts_round_ending(); // stats of a round abandoned before its END
    net_free_players();

    for (size_t i = 0; i < g_server->client_count; i++) g_server->clients[i].joined = 0;

    game_init(&g_server->game, map_index, mode_index, 0);
    ks_server_round_init(); // KrunkScript: (re)load the map's server script, start()
    accounts_round_changed(); // lobby size / round length per mode, billboard rotation, match id

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

// isDev: CLI / config op, a DEV account the brain verified (players.player_developer), or a
// player on the server's own machine (it hosts this server, v3.1.1's offline host). Loopback
// alone is not enough: net-bridge connects from 127.0.0.1 for every browser, so a bridged
// connection counts by the address it forwards (NET_PACKET_FORWARDED, net_handle_forwarded).
static int net_client_is_dev(const ClientConnection *client) {
    return client->op || client->local || (client->acct.flags & NET_PLAYER_INFO_DEV);
}

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
        .dev = net_client_is_dev(client),
        .premium_host = accounts_is_custom_host(client),
        .account_id = client->acct.account_id, // backend account (server/src/accounts.c), 0 guest
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

// Close a connection after the current poll pass with NET_PACKET_ERROR (accounts.c).
void net_kick_client(const int id, const char *message, const char *code) {
    net_chat_disconnect(NULL, id, message, code);
}

static void net_chat_change_map(void *user, const int map_index, const int mode_index) {
    (void) user;
    net_change_map(map_index, mode_index);
}

static void net_chat_log(void *user, const char *text) {
    (void) user;
    server_log("[Server] %s", text);
}

static int net_chat_ban_account(void *user, const int account_id, const char *name) {
    (void) user;
    return accounts_ban_account(account_id, name);
}

static int net_chat_unban_account(void *user, const int account_id) {
    (void) user;
    return accounts_unban_account(account_id);
}

static int net_chat_list_account_bans(void *user, int *out, const int max) {
    (void) user;
    return accounts_list_account_bans(out, max);
}

ChatCommandHost *net_chat_host(void) {
    g_chat_host.game = &g_server->game;
    g_chat_host.perms = g_server->config.perms;
    g_chat_host.cheats_open = g_server->config.cheats_open;
    g_chat_host.lobby = "native";
    g_chat_host.region = "local";
    g_chat_host.tick_rate = g_server->config.tick_rate ? (float) g_server->config.tick_rate : 30.0f;
    g_chat_host.peer_count = net_chat_peer_count;
    g_chat_host.peer_at = net_chat_peer_at;
    g_chat_host.disconnect = net_chat_disconnect;
    g_chat_host.change_map = net_chat_change_map;
    g_chat_host.log = net_chat_log;
    g_chat_host.ban_account = net_chat_ban_account;     // gameserver.bans_file "accounts"
    g_chat_host.unban_account = net_chat_unban_account;
    g_chat_host.list_account_bans = net_chat_list_account_bans;
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
        CLOSE_CLIENT_FD(client);
        return;
    }

    snprintf(client->address, sizeof(client->address), "%s", inet_ntoa(client->addr.sin_addr));
    client->local = (ntohl(client->addr.sin_addr.s_addr) >> 24) == 127; // until a FORWARDED says otherwise
    client->first_packet_seen = 0;
    if (chat_command_ip_banned(net_chat_host(), client->address)) { // pnhBannedAddresses: close(4003, "banned")
        const NetErrorPacket error = {.message = "Banned", .code = "004003"};
        net_send_packet(client->fd, NET_PACKET_ERROR, &error, sizeof(error));
        server_log("Rejecting %s: IP banned", client->address);
        CLOSE_CLIENT_FD(client);
        return;
    }
    NetErrorPacket refuse = {{0}};
    if (accounts_refuse_connect(client->address, &refuse)) { // draining ("GAME UPDATING")
        net_send_packet(client->fd, NET_PACKET_ERROR, &refuse, sizeof(refuse));
        server_log("Rejecting %s: %s", client->address, refuse.message);
        CLOSE_CLIENT_FD(client);
        return;
    }

    ClientConnection *new_clients = realloc(g_server->clients, (g_server->client_count + 1) * sizeof(ClientConnection));

    if (!new_clients) {
        CLOSE_CLIENT_FD(client);
        return;
    }

    g_server->clients = new_clients;
    client->id = g_server->next_client_id++;
    client->player_number = net_alloc_player_number();

    if (!client->player_number) {
        CLOSE_CLIENT_FD(client);
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
    client->slid_cont = 1;
    client->spray_index = 0;
    client->skins = PLAYER_SKINS_NONE;
    client->lag_comp = 1.0f;
    client->chat_limiter = (NetChatLimiter) {0};
    client->ping_seq = 0;
    client->ping_ms = -1;
    client->next_ping_at = 0.0; // first probe right away (pi on connect)
    client->social = (SocialAssembly) {0};
    client->voice = (SocialAssembly) {0};
    g_server->clients[g_server->client_count] = *client;

    ClientConnection *stored_client = &g_server->clients[g_server->client_count++];
    accounts_on_connect(stored_client);
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

    accounts_on_disconnect(client); // partial stats commit, player-leave to the brain
    on_client_disconnect(client);
    net_set_spectating(client, 0);
    ks_server_on_leave(client); // KrunkScript onPlayerLeave, before the player is removed
    ks_server_on_disconnect(client);
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
        if (accounts_leave_message()[0]) net_broadcast_server_message(client, accounts_leave_message(), client->name); // serverMessages[1]
    }
    net_release_player_number(client);
    social_assembly_free(&client->social);
    social_assembly_free(&client->voice);
    CLOSE_CLIENT_FD(client);

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
    ks_server_on_chat(client, text); // KrunkScript onChatMessage (not for "/" commands)
    net_broadcast_chat_except(NULL, client->id, NET_CHAT_PLAYER, client->name, text);
    return 1;
}

static void net_handle_pong(ClientConnection *client, const NetPacket *packet) {
    if (packet->length != sizeof(NetPingPacket) || !client->ping_seq) return;

    NetPingPacket pong;
    memcpy(&pong, packet->payload, sizeof(pong));
    if (pong.seq != client->ping_seq) return;

    const double now = net_time();
    const double readable_at = client->transport->pong_received_at(client->fd); // datagram arrival / socket readable
    const double received_at = readable_at > client->ping_sent_at && readable_at < now ? readable_at : now;
    client->ping_ms = net_ping_update(client->ping_ms, (float) (received_at - client->ping_sent_at));
    client->ping_seq = 0;
    if (client->player) client->player->ping = (float) client->ping_ms; // io_po: player.ping = smoothed
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
    if (brain_enabled()) {
        // a node behind kn_brain: the hub is the brain's social socket and /api routes
        // (v3.1.1 gameserver.ts MM_NODE_MODE=node leaves them to the brain); drop it like
        // socialWs.ts drops what it does not handle
        free(request);
        return;
    }

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

// packetHandlers.ts requireDev for the dev packets, through the chat commands' gate
// (chat_command_allowed: perms, cheats_open, ChatPeer.dev / premium_host); a denial answers
// ["notify", "DEV only"] (the mod's notify toast).
static int net_require_dev(ClientConnection *client, const ChatCommandPerm perm, const char *label) {
    return chat_command_allowed(net_chat_host(), client->id, perm, label);
}

// net-bridge's first frame: the browser's real address. Only a loopback peer (the bridge on
// this machine) may claim one, and only before anything else; the claimed address then
// decides IP bans and whether the player is on the server's machine.
static int net_handle_forwarded(ClientConnection *client, const NetPacket *packet) {
    if (client->first_packet_seen || !client->local || !packet->length || packet->length > NET_FORWARDED_ADDRESS_SIZE) {
        server_log("[gameserver] ignored FORWARDED from %d (%s)", client->id, client->address);
        return 1;
    }
    char address[NET_FORWARDED_ADDRESS_SIZE + 1];
    memcpy(address, packet->payload, packet->length);
    address[packet->length] = 0;
    struct in_addr parsed;
    const int v4 = inet_pton(AF_INET, address, &parsed) == 1;
    client->local = v4 ? (ntohl(parsed.s_addr) >> 24) == 127 : !strcmp(address, "::1");
    snprintf(client->address, sizeof(client->address), "%s", address[0] ? address : "?");
    server_log("[gameserver] %d is bridged for %s%s", client->id, client->address, client->local ? " (this machine)" : "");
    if (chat_command_ip_banned(net_chat_host(), client->address)) {
        net_kick_client(client->id, "Banned", "004003");
    }
    return 1;
}

// io_prmA(index): premiumKeys / the N key (index 0 = noclip), gated like /noclip.
static void net_handle_premium_action(ClientConnection *client, const NetPacket *packet) {
    if (packet->length != 1) return;
    chat_command_premium_action(net_chat_host(), client->id, packet->payload[0]);
}

// io_tp(x, y, z): DEV only with perms; finite coordinates; the feet go EYE_OFFSET_Y below
// the given y; position/velocity/ground state reset; the sender gets the ack.
static void net_handle_teleport(ClientConnection *client, const NetPacket *packet) {
    if (packet->length != sizeof(NetTeleportPacket)) return;
    if (!net_require_dev(client, CHAT_PERM_DEV_CHEAT, "tp")) return;
    NetTeleportPacket tp;
    memcpy(&tp, packet->payload, sizeof(tp));
    if (!isfinite(tp.x) || !isfinite(tp.y) || !isfinite(tp.z)) {
        server_log("[gameserver] tp invalid coords: %g %g %g", tp.x, tp.y, tp.z);
        return;
    }
    Player *player = client->player;
    if (!player) return; // io_tp would throw on ctx.player null
    const NetTeleportPacket ack = {tp.x, tp.y - NET_TELEPORT_EYE_OFFSET, tp.z};
    net_player_teleport(player, ack.x, ack.y, ack.z);
    net_send_packet(client->fd, NET_PACKET_TELEPORT_ACK, &ack, sizeof(ack));
    server_log("[gameserver] tp %d -> %g %g %g", client->id, ack.x, ack.y, ack.z);
}

// One INPUT frame (single or batched): v3.1.1 inputDecode.ts checks in player_accept_input
// (non-finite fields dropped, delta clamped, seq floor, 240-frame cap). Physics only ever runs
// from these inputs: the server never takes a position from the client, so a client-side
// noclip flag or position jump has no effect on the authoritative player.
static void net_handle_input_frame(ClientConnection *client, const NetInputPacket *net_input, const double arrival,
                                   const double received) {
    Input input;
    if (!client->player || !client->player->active) return;
    if (g_server->game.end_timer > 0.0f) return; // io_q: isVoteWindow drops the batch
    net_packet_to_input(&input, net_input);
    input.arrival = arrival;
    input.received = received; // the packet's arrival: exact lag compensation's input clock (LagCompClock)
    player_accept_input(client->player, &input);

    LagCompClock *clock = &client->player->lag_clock;
    if (clock->anomaly_new) {
        clock->anomaly_new = 0;
        server_log("[lagcomp] %s: input clock ran %s real time (summed deltas vs arrivals); arrival-anchored lag "
                   "compensation for the rest of this life", client->name,
                   clock->offset + clock->excess > clock->baseline ? "slower than" : "faster than");
    }
}

// When this client's packet reached the server, on the game.state_now clock that stamps the
// stateHistory (anchored lag compensation, shared/src/player.c): the wall time poll() saw the
// socket readable (TCP; the handler may have waited for the tick's lock) or now, mapped by the
// last tick's start. state_now advances by each tick's wall delta, so state_now - tick_wall
// is constant up to float rounding.
static double net_input_arrival(const ClientConnection *client) {
    const double now = net_time();
    const double readable_at = client->transport->input_received_at(client->fd);
    const double wall = readable_at > 0.0 && readable_at <= now && now - readable_at < 1.0 ? readable_at : now;
    return g_server->game.state_now + (wall - g_server->tick_wall);
}

// "q": every frame of the batch, in order, through the same checks as a single INPUT. The
// client sends a batch once per clientSendRate, so its earlier frames were generated earlier:
// each one arrives "back" by the deltas (seconds, clamped like procInputs) of the frames after it.
static void net_handle_input_batch(ClientConnection *client, const NetPacket *packet) {
    NetInputBatchPacket batch;
    if (!net_packet_read_input_batch(&batch, packet)) return;
    double arrival[NET_INPUT_BATCH_MAX];
    double at = net_input_arrival(client);
    for (int i = batch.count - 1; i >= 0; i--) {
        arrival[i] = at;
        const float delta = batch.frames[i].delta;
        if (isfinite(delta) && delta > 0.0f) at -= MIN(delta, game_constants.max_delta);
    }
    const double received = arrival[batch.count ? batch.count - 1 : 0];
    for (int i = 0; i < batch.count; i++) net_handle_input_frame(client, &batch.frames[i], arrival[i], received);
}

// ENT after its fields were stored (also replayed by accounts.c once a deferred join settled).
void net_resume_ent(ClientConnection *client, const int spectate) {
    if (spectate) {
        client->voice_player = 0; // io_ent spectate: ctx.player = null
        net_start_spectating(client);
    } else {
        client->voice_player = 1; // io_ent: ctx.player = ctx.server.addPlayer(...) on the first ENT
        net_request_spawn(client);
        // A spectator plays again once it actually spawned (not on the end screen).
        if (client->player && client->player->active) net_set_spectating(client, 0);
    }
}

// Returns 0 when the client was removed.
int net_handle_packet(ClientConnection *client, const NetPacket *packet) {
    const int first_packet = !client->first_packet_seen;
    if (packet->type != NET_PACKET_FORWARDED) client->first_packet_seen = 1;
    switch (packet->type) {
        case NET_PACKET_HELLO:
            ks_server_on_hello(client, packet); // the peer's protocol minor (KrunkScript packet gate)
            break;
        case NET_PACKET_SCRIPT_MSG: // KrunkScript GAME.NETWORK.send from the client script
            ks_server_on_script_msg(client, packet);
            break;
        case NET_PACKET_FORWARDED: // net-bridge: the browser's address, before its HELLO
            if (first_packet) net_handle_forwarded(client, packet);
            client->first_packet_seen = 1;
            break;
        case NET_PACKET_PREMIUM_ACTION: // "prmA"
            net_handle_premium_action(client, packet);
            break;
        case NET_PACKET_ENT: {
            NetEntPacket ent;
            if (!net_packet_read_ent(&ent, packet)) break;

            client->class_index = ent.class_index;
            client->secondary_index = ent.secondary_index;
            client->cosmetics = ent.cosmetics;
            client->slid_cont = ent.slid_cont;
            client->spray_index = ent.spray_index;
            client->skins = ent.skins;
            // gameserver.ts applyLagCompFromEnt: a finite ent[11] clamped to 0.1..1 (never 0: a client
            // must not turn off the rewind of its targets); otherwise the previous value stays
            if (isfinite(ent.lag_comp)) client->lag_comp = fminf(1.0f, fmaxf(0.1f, ent.lag_comp));
            if (client->player) client->player->lag_comp = client->lag_comp;

            // brain-linked: the first ENT waits for the JOIN / account lookup (server/src/accounts.c)
            if (accounts_gate_ent(client, ent.spectate)) net_resume_ent(client, ent.spectate);
            break;
        }
        case NET_PACKET_JOIN:
            accounts_handle_join(client, packet);
            break;
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
        case NET_PACKET_INTERACT: { // io_int
            NetInteractPacket interact = {0};
            if (!client->player || !packet->length || packet->length > sizeof(interact)) break;
            memcpy(&interact, packet->payload, packet->length);
            game_player_interact(&g_server->game, client->player, interact.action, interact.secondary);
            break;
        }
        case NET_PACKET_TERMINAL_MSG: { // "tmc" has no handler: io_default logs it
            NetTerminalMsgPacket msg;
            char text[NET_CHAT_TEXT_SIZE];
            if (packet->length < sizeof(int32_t) + 1 || packet->length > sizeof(msg)) break;
            memset(&msg, 0, sizeof(msg));
            memcpy(&msg, packet->payload, packet->length);
            if (net_chat_sanitize(text, sizeof(text), msg.text, packet->length - sizeof(int32_t))) {
                server_log("tmc %d,\"%s\" (%s)", (int) msg.uid, text, client->name);
            }
            break;
        }
        case NET_PACKET_PONG:
            net_handle_pong(client, packet);
            break;
        case NET_PACKET_MAP_REQUEST:
            net_handle_map_request(client, packet);
            break;
        case NET_PACKET_INPUT: {
            NetInputPacket net_input;
            if (net_packet_read_input(&net_input, packet)) {
                const double at = net_input_arrival(client);
                net_handle_input_frame(client, &net_input, at, at);
            }
            break;
        }
        case NET_PACKET_INPUT_BATCH: // "q"
            net_handle_input_batch(client, packet);
            break;
        case NET_PACKET_CYCLE: // io_cycle -> cmd_cycle: requireDev, like /cycle
            if (!packet->length && net_require_dev(client, CHAT_PERM_DEV, "cycle")) net_cycle_map();
            break;
        case NET_PACKET_TELEPORT: // "tp" (the mod's Ctrl TP)
            net_handle_teleport(client, packet);
            break;
        case NET_PACKET_CLEAR_LOG: // "clearLog" (the mod's F1): io_clearLog, no permission check
            if (!packet->length) server_log_clear();
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
    return client->transport->poll(client);
}

void net_poll_clients(void) {
    for (size_t i = 0; i < g_server->client_count; i++) {
        if (!net_poll_client(&g_server->clients[i])) i--;
    }
    net_process_pending_kicks(); // chat /kick, /ban, /banip (and the CLI running them)
}

void net_install_game_hooks(void) {
    // Match events leave the shared game through these hooks (see GameHooks).
    g_server->game.hooks.user = g_server;
    g_server->game.hooks.send = net_game_send;
    g_server->game.hooks.next_round = net_game_next_round;
    g_server->game.hooks.respawn = net_game_respawn;
    g_server->game.hooks.reward = net_game_reward; // endGame player.reward (accounts.c)
    g_server->game.hooks.alloc_uid = net_game_alloc_uid; // bot players (shared/src/bots.c)
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

// gameserver.ts tick: "l" to every playing connection at NET_RECON_SEND_HZ with the last
// input procInputs drained (none before the first one of a life), and at once after a
// server-driven change of its movement (teleport, changePosition recon, a noclip flip).
// Intentional difference: those snaps ack player->input_seq instead of v3.1.1's
// MAX_SAFE_INTEGER. The state is exactly the one after that input, so the client replays its
// in-flight inputs on top of the new position like the server will, instead of dropping them
// and rubber-banding back once the next 1 Hz "l" arrives.
static void net_send_reconciliation(void) {
    const double now = net_time();
    for (size_t i = 0; i < g_server->client_count; i++) {
        ClientConnection *client = &g_server->clients[i];
        Player *player = client->player;
        if (ks_game_flag(&g_server->game, KS_FLAG_NO_SERVER_SYNC)) break; // KrunkScript DEFAULT.disableServerSync
        if (!net_recon_due(player, now, &client->next_recon_at)) continue;

        NetReconPacket packet;
        net_packet_from_recon(&packet, player, player->input_seq);
        net_send_packet(client->fd, NET_PACKET_RECON, &packet, sizeof(packet));
    }
}

// Per tick (main thread): pings, reconciliation ("l"), then the player sync (Krunker "k",
// NET_STATE_SEND_HZ; none on the end screen).
static void net_broadcast_states_tick(void);

// Per tick: the sync below, then the UDP peers' queued frames leave at once (the net
// thread would only flush them after its next poll wake-up).
void net_broadcast_states() {
    net_broadcast_states_tick();
    server_transport_flush_all();
}

static void net_broadcast_states_tick(void) {
    net_send_pings();
    if (g_server->game.end_timer > 0.0f) return;
    net_send_reconciliation();

    static double next_state_at; // gameserver.ts K_PACKET_INTERVAL_MS
    const double state_now = net_time();
    if (state_now < next_state_at) return;
    next_state_at = state_now + 1.0 / NET_STATE_SEND_HZ;

    NetStatePacket states[256];
    size_t state_count = 0;
    for (size_t j = 0; j < g_server->game.player_count && state_count < sizeof(states) / sizeof(states[0]); j++) {
        Player *player = g_server->game.players[j];
        if (player && player->active) net_packet_from_player(&states[state_count++], player);
    }
    if (!state_count) return;

    static NetSendBatch batch; // main thread only
    for (size_t i = 0; i < g_server->client_count; i++) {
        const ClientConnection *client = &g_server->clients[i];
        batch.length = 0;
        for (size_t j = 0; j < state_count; j++) {
            if (net_batch_append(&batch, NET_PACKET_STATE, &states[j], sizeof(states[j]))) continue;
            net_batch_flush(client, &batch); // full (> ~140 players): send and start the next batch
            net_batch_append(&batch, NET_PACKET_STATE, &states[j], sizeof(states[j]));
        }
        net_batch_flush(client, &batch);
    }
}
