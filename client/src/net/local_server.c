#include <client.h>
#include <pcg_basic.h>
#include <string.h>

#ifdef WIN32
#include <windows.h>

static double local_server_time() {
    return (double) GetTickCount64() / 1000.0;
}

#define SLEEP_MILLIS(ms) Sleep(ms)
#else
#include <time.h>

static double local_server_time() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec + (double) ts.tv_nsec / 1e9;
}

#define SLEEP_MILLIS(ms) do { struct timespec ts = {0, (ms) * 1000000L}; nanosleep(&ts, NULL); } while (0)
#endif

#define LOCAL_SERVER_TICK_RATE 30

static int net_packet_init(NetPacket *packet, const NetPacketType type, const void *payload, const uint16_t length) {
    if (length > NET_MAX_PAYLOAD) return 0;

    packet->type = (uint8_t) type;
    packet->length = length;
    if (payload && length) memcpy(packet->payload, payload, length);

    return 1;
}

static void local_server_send_packet(Client *client, const NetPacketType type, const void *payload, const uint16_t length) {
    NetPacket packet;
    if (!net_packet_init(&packet, type, payload, length)) return;

    client_net_log_packet("<=", type, payload, length);
    client_net_queue_packet(client, &packet);
}

void local_server_queue_packet(Client *client, const NetPacketType type, const void *payload, const uint16_t length) {
    NetPacket packet;
    if (!net_packet_init(&packet, type, payload, length)) return;

    client_net_log_packet("=>", type, payload, length);

    pthread_mutex_lock(&client->local_server_lock);

    NetPacket *new_packets = realloc(client->local_server_packets, (client->local_server_packet_count + 1) * sizeof(NetPacket));

    if (new_packets) {
        client->local_server_packets = new_packets;
        client->local_server_packets[client->local_server_packet_count++] = packet;
    }

    pthread_mutex_unlock(&client->local_server_lock);
}

static NetPacket *local_server_take_packets(Client *client, size_t *packet_count) {
    pthread_mutex_lock(&client->local_server_lock);

    NetPacket *packets = client->local_server_packets;
    *packet_count = client->local_server_packet_count;

    client->local_server_packets = NULL;
    client->local_server_packet_count = 0;

    pthread_mutex_unlock(&client->local_server_lock);

    return packets;
}

static void local_server_send_init(Client *client) {
    const NetInitPacket packet = {
        .major = 1,
        .minor = 0,
        .patch = 0,
        .map_index = client->local_server_game.current_map_index,
        .mode_index = client->local_server_game.current_mode_index,
        .tick_rate = LOCAL_SERVER_TICK_RATE,
        .client_id = 0,
    };

    local_server_send_packet(client, NET_PACKET_INIT, &packet, sizeof(packet));
}

static void local_server_send_spawn(Client *client, const Player *player) {
    const NetSpawnPacket packet = {
        .uid = player->uid,
        .is_you = 1,
        .active = player->active,
        .position = {player->position.x, player->position.y, player->position.z},
        .direction = {player->direction.x, player->direction.y},
    };

    local_server_send_packet(client, NET_PACKET_SPAWN, &packet, sizeof(packet));
}

static void local_server_send_state(Client *client, const Player *player) {
    NetStatePacket packet;
    net_packet_from_player(&packet, player);

    local_server_send_packet(client, NET_PACKET_STATE, &packet, sizeof(packet));
}

static void local_server_free_players(Client *client) {
    for (size_t i = 0; i < client->local_server_game.player_count; i++) {
        Player *player = client->local_server_game.players[i];
        if (!player) continue;

        free(player->loadout);
        free(player->ammo);
        free(player->reloads);
        free(player->input_queue);
        free(player);
    }

    free(client->local_server_game.players);
    client->local_server_game.players = NULL;
    client->local_server_game.player_count = 0;
    client->local_server_player = NULL;
}

static void local_server_cycle_map(Client *client) {
    if (!client->local_server_game.map_count) return;

    const int next_map = (client->local_server_game.current_map_index + 1) % (int) client->local_server_game.map_count;
    const int mode = client->local_server_game.current_mode_index;

    local_server_free_players(client);

    g_skip_map_meshes = 1;
    game_init(&client->local_server_game, next_map, mode, 0);
    g_skip_map_meshes = 0;

    local_server_send_init(client);
}

static void local_server_handle_ent(Client *client) {
    if (!client->local_server_player) {
        Player *player = player_init(&client->local_server_game);
        if (!player) return;

        player->uid = client->local_server_next_client_id++;
        client->local_server_player = player;
        game_players_add(&client->local_server_game, player);
    }

    player_spawn(client->local_server_player);
    local_server_send_spawn(client, client->local_server_player);
}

static void local_server_handle_packet(Client *client, const NetPacket *packet) {
    switch (packet->type) {
        case NET_PACKET_HELLO:
            break;
        case NET_PACKET_ENT:
            if (packet->length == sizeof(NetEntPacket)) local_server_handle_ent(client);
            break;
        case NET_PACKET_INPUT: {
            if (packet->length != sizeof(NetInputPacket) || !client->local_server_player || !client->local_server_player->active) break;

            NetInputPacket net_input;
            Input input;

            memcpy(&net_input, packet->payload, sizeof(net_input));
            net_packet_to_input(&input, &net_input);
            player_queue_input(client->local_server_player, &input);
            break;
        }
        case NET_PACKET_CYCLE:
            if (!packet->length) local_server_cycle_map(client);
            break;
        default:
            break;
    }
}

static void *local_server_main(void *user_data) {
    Client *client = user_data;

    client->local_server_game.server = client;

    const cJSON **maps = NULL;
    size_t map_count = 0;

    if (client->startup_map) {
        maps = calloc(1, sizeof(cJSON *));

        if (maps) {
            maps[0] = client->startup_map;
            map_count = 1;
        } else {
            client->local_server_should_quit = 1;
            return NULL;
        }
    }

    g_skip_map_meshes = 1;
    game_configure(&client->local_server_game, NULL, maps, map_count, NULL, 0, NULL, 0, NULL, 0);
    game_init(&client->local_server_game, -1, -1, 0);
    g_skip_map_meshes = 0;

    local_server_send_init(client);

    double last_tick = local_server_time();
    float now = 0.0f;

    while (!client->local_server_should_quit) {
        const double tick_start = local_server_time();
        const float delta = (float) (tick_start - last_tick);
        last_tick = tick_start;
        now += delta;

        size_t packet_count = 0;
        NetPacket *packets = local_server_take_packets(client, &packet_count);

        for (size_t i = 0; i < packet_count; i++) {
            local_server_handle_packet(client, &packets[i]);
        }

        free(packets);

        game_tick(&client->local_server_game, now, delta);

        if (client->local_server_player && client->local_server_player->active) {
            local_server_send_state(client, client->local_server_player);
        }

        const double elapsed_ms = (local_server_time() - tick_start) * 1000.0;
        const int sleep_ms = (int) (1000.0 / LOCAL_SERVER_TICK_RATE - elapsed_ms);
        if (sleep_ms > 0) SLEEP_MILLIS(sleep_ms);
    }

    client->local_server_running = 0;
    return NULL;
}

int local_server_start(Client *client) {
    client->local_server_should_quit = 0;
    client->local_server_running = 1;
    client->net_connected = 1;

    if (pthread_create(&client->local_server_thread, NULL, local_server_main, client)) {
        client->local_server_running = 0;
        client->net_connected = 0;
        return 0;
    }

    return 1;
}

void local_server_stop(Client *client) {
    if (!client->local_server_running) return;

    client->local_server_should_quit = 1;
    pthread_join(client->local_server_thread, NULL);
    client->net_connected = 0;
}
