// The offline sandbox server (server/include/server_sandbox.h): server_bin's core in the
// client process over in-memory connections, stepped by the client's main loop.
#include <server_sandbox.h>
#include <server.h>
#include <server_accounts.h>
#include <server_config.h>
#include <server_log.h>
#include <knet.h>   // NET_LOOPBACK_FD_BASE, net_set_loopback_send
#include <ks_server.h> // KrunkScript server host
#include <bots.h>
#include <ai.h>
#include <modes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SANDBOX_LINKS 4
#define SANDBOX_QUEUE_MAX (16u * 1024u * 1024u) // a client that stopped reading is dropped past this

// Descriptors: link i has a server end (ClientConnection.fd, what the server sends to) and a
// client end (what the client sends to).
#define SANDBOX_SERVER_FD(i) (NET_LOOPBACK_FD_BASE + (i) * 2)
#define SANDBOX_CLIENT_FD(i) (NET_LOOPBACK_FD_BASE + (i) * 2 + 1)

typedef struct {
    uint8_t *data;
    size_t length, capacity;
} SandboxQueue;

typedef struct {
    unsigned char used:1;
    unsigned char added:1;         // net_add_client took it (the server end is a ClientConnection)
    unsigned char server_closed:1; // the server closed it (kick, error, stop): the client end drains then reads -1
    unsigned char client_closed:1; // sandbox_disconnect: the server removes it on the next step
    SandboxQueue to_server;        // client -> server frames
    SandboxQueue to_client;        // server -> client frames
} SandboxLink;

static struct {
    int running;
    double clock;         // sandbox wall clock (s): server_time() while running, never goes back
    double started_at;    // clock at sandbox_start (g_server->now counts from here, like server_bin's loop)
    double last_tick;     // clock of the last tick
    double next_tick;     // clock the next tick is due at
    unsigned short tick_rate;
    SandboxLink links[SANDBOX_LINKS];
} g_sb;

static Server g_sandbox_server;

// ------------------------------------------------------------------ byte queues

static int queue_append(SandboxQueue *q, const uint8_t *data, const size_t length) {
    if (q->length + length > SANDBOX_QUEUE_MAX) return 0;
    if (q->length + length > q->capacity) {
        size_t capacity = q->capacity ? q->capacity : 4096;
        while (capacity < q->length + length) capacity *= 2;
        uint8_t *grown = realloc(q->data, capacity);
        if (!grown) return 0;
        q->data = grown;
        q->capacity = capacity;
    }
    memcpy(q->data + q->length, data, length);
    q->length += length;
    return 1;
}

static size_t queue_take(SandboxQueue *q, uint8_t *out, const size_t size) {
    const size_t n = q->length < size ? q->length : size;
    if (!n) return 0;
    memcpy(out, q->data, n);
    q->length -= n;
    if (q->length) memmove(q->data, q->data + n, q->length);
    return n;
}

static void queue_free(SandboxQueue *q) {
    free(q->data);
    *q = (SandboxQueue) {0};
}

static SandboxLink *link_by_fd(const int fd, int *server_end) {
    if (fd < NET_LOOPBACK_FD_BASE) return NULL;
    const int index = (fd - NET_LOOPBACK_FD_BASE) / 2;
    if (index < 0 || index >= SANDBOX_LINKS || !g_sb.links[index].used) return NULL;
    if (server_end) *server_end = (fd - NET_LOOPBACK_FD_BASE) % 2 == 0;
    return &g_sb.links[index];
}

static void link_free(SandboxLink *link) {
    queue_free(&link->to_server);
    queue_free(&link->to_client);
    *link = (SandboxLink) {0};
}

// net_send_packet() for loopback fds: the frame exactly as shared/src/net.c writes it to a
// socket, appended to the other end's queue.
static int sandbox_send(const int fd, const uint8_t type, const void *payload, const uint16_t length) {
    int server_end = 0;
    SandboxLink *link = link_by_fd(fd, &server_end);
    if (!link || length > NET_MAX_PAYLOAD) return 0;
    if (server_end ? link->server_closed : link->client_closed) return 0;

    uint8_t frame[NET_PACKET_HEADER_SIZE + NET_MAX_PAYLOAD];
    frame[0] = type;
    frame[1] = (uint8_t) (length >> 8);
    frame[2] = (uint8_t) (length & 0xff);
    if (payload && length) memcpy(frame + NET_PACKET_HEADER_SIZE, payload, length);
    return queue_append(server_end ? &link->to_client : &link->to_server, frame, NET_PACKET_HEADER_SIZE + length);
}

static double sandbox_clock(void) {
    return g_sb.clock;
}

// ------------------------------------------------------------------ server core transport

// Like the TCP transport's recv loop: the client's bytes through the connection's
// NetPacketBuffer, every whole packet to net_handle_packet.
static int sandbox_poll(ClientConnection *client) {
    SandboxLink *link = link_by_fd(client->fd, NULL);
    if (!link || link->client_closed) {
        net_remove_client(client);
        return 0;
    }

    while (link->to_server.length) {
        uint8_t chunk[1024];
        const size_t room = sizeof(client->read_buffer.data) - client->read_buffer.length;
        const size_t n = queue_take(&link->to_server, chunk, room < sizeof(chunk) ? room : sizeof(chunk));
        if (!n || !net_buffer_push(&client->read_buffer, chunk, n)) {
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
    }
    return 1;
}

static void sandbox_send_frames(const int fd, const uint8_t *frames, const size_t length) {
    int server_end = 0;
    SandboxLink *link = link_by_fd(fd, &server_end);
    if (link && server_end && !link->server_closed) queue_append(&link->to_client, frames, length);
}

static void sandbox_close(const int fd) {
    SandboxLink *link = link_by_fd(fd, NULL);
    if (!link) return;
    link->server_closed = 1; // the client end still reads what was sent before (kick reasons)
    link->added = 0;
    if (link->client_closed) link_free(link);
}

static double sandbox_received_at(const int fd) {
    (void) fd;
    return 0.0; // stamped when the step handles them: no network in between
}

static const ServerTransport g_sandbox_transport = {
    .name = "sandbox",
    .poll = sandbox_poll,
    .send_frames = sandbox_send_frames,
    .close = sandbox_close,
    .pong_received_at = sandbox_received_at,
    .input_received_at = sandbox_received_at,
};

// ------------------------------------------------------------------ log

static void sandbox_log_write(void *user, const char *text, const int length) {
    (void) user;
    printf("[sandbox] %.*s", length, text);
    fflush(stdout);
}

// ------------------------------------------------------------------ lifecycle

int sandbox_running(void) {
    return g_sb.running;
}

int sandbox_start(const SandboxConfig *cfg) {
    if (g_sb.running) sandbox_stop();
    if (!cfg || !cfg->maps || !cfg->map_count) return 0;

    // gameserver defaults, then the offline game's rules (server_sandbox.h)
    gs_config_defaults(&g_gs_config);
    g_gs_config.idle_timeout = 0.0f;   // never kicked for inactivity offline
    g_gs_config.bans_file[0] = 0;
    g_gs_config.brain_url[0] = 0;
    g_gs_config.password[0] = 0;
    g_gs_config.online_mode = 0;
    g_gs_config.earn_kr = 0;
    g_gs_config.enforce_skins = 0;
    if (cfg->tick_rate) g_gs_config.tick_rate = cfg->tick_rate;

    memset(&g_sandbox_server, 0, sizeof(g_sandbox_server));
    g_server = &g_sandbox_server;
    g_server->config.tick_rate = g_gs_config.tick_rate;
    g_server->config.listen_port = 0;
    g_server->config.max_players = (unsigned short) g_gs_config.max_players;
    g_server->config.idle_timeout = g_gs_config.idle_timeout;
    g_server->config.idle_move_threshold = g_gs_config.idle_move_threshold;
    g_server->config.perms = 0;
    g_server->config.cheats_open = 1; // v3.1.1's perms-off server: cheats for everyone (the one local player)
    g_server->game.lag_comp = (unsigned char) !!g_gs_config.lag_comp;
    g_server->game.lag_comp_anchor = (unsigned char) (g_gs_config.lag_comp_anchor & 3);
    g_server->game.lag_comp_scale = g_gs_config.lag_comp_scale;
    g_server->next_client_id = 0;
    pthread_mutex_init(&g_server->lock, NULL);

    const cJSON **maps = malloc(cfg->map_count * sizeof(*maps));
    int *modes = malloc(sizeof(int));
    if (!maps || !modes) {
        free(maps);
        free(modes);
        pthread_mutex_destroy(&g_server->lock);
        g_server = NULL;
        return 0;
    }
    memcpy(maps, cfg->maps, cfg->map_count * sizeof(*maps));
    modes[0] = cfg->mode_id >= 0 && mode_config(cfg->mode_id) ? cfg->mode_id : MODE_ID_FFA;

    g_sb.tick_rate = g_gs_config.tick_rate ? g_gs_config.tick_rate : 30;
    g_sb.started_at = g_sb.clock;
    g_sb.last_tick = g_sb.clock;
    g_sb.next_tick = g_sb.clock + 1.0 / g_sb.tick_rate;
    g_sb.running = 1;

    server_set_clock(sandbox_clock);
    net_set_loopback_send(sandbox_send);
    server_log_set_writer(sandbox_log_write, NULL);

    g_server->game.server = g_server; // the authority, built without meshes (game_init: map_init_ex)
    game_configure(&g_server->game, NULL, maps, cfg->map_count, modes, 1, NULL, 0, NULL, 0);
    const int map_index = cfg->map_index >= 0 && (size_t) cfg->map_index < cfg->map_count ? cfg->map_index : -1;
    game_init(&g_server->game, map_index, 0, 0);

    pthread_mutex_lock(&g_server->lock);
    net_install_game_hooks();
    ks_server_round_init(); // KrunkScript: the map's server script runs in the sandbox too
    accounts_init(); // standalone: lobby id, first round's lobby size / round length
    pthread_mutex_unlock(&g_server->lock);

    if (!g_server->game.ready) {
        server_log_error("No playable map loaded");
        sandbox_stop();
        return 0;
    }
    const cJSON *name = cJSON_GetObjectItem(g_server->game.maps[g_server->game.current_map_index], "name");
    server_log("Loaded map %s (%d), mode %s (%d)", cJSON_IsString(name) ? name->valuestring : "?", g_server->game.current_map_index,
               mode_name(game_mode_id(&g_server->game)), game_mode_id(&g_server->game));
    return 1;
}

static void sandbox_free_player(Player *player) {
    if (!player) return;
    free(player->loadout);
    free(player->ammo);
    free(player->reloads);
    free(player->input_queue);
    free(player);
}

void sandbox_stop(void) {
    if (!g_sb.running) return;

    pthread_mutex_lock(&g_server->lock);
    accounts_shutdown();
    while (g_server->client_count) net_remove_client(&g_server->clients[0]); // closes every link's server end
    ks_server_shutdown(); // KrunkScript onServerClosed, then the script is freed
    pthread_mutex_unlock(&g_server->lock);

    Game *game = &g_server->game;
    for (size_t i = 0; i < game->player_count; i++) sandbox_free_player(game->players[i]); // bot players
    free(game->players);
    game->players = NULL;
    game->player_count = 0;
    game_bots_free(game);
    game_ai_free(game); // the AI list and its data records (shared/src/ai.c)
    if (game->map) map_fini(game->map);
    if (game->mode) mode_fini(game->mode);
    free(game->vote_kick);
    free((void *) game->maps);
    free(game->modes);
    free(game->weapons);
    free(game->classes);

    pthread_mutex_destroy(&g_server->lock);
    memset(&g_sandbox_server, 0, sizeof(g_sandbox_server));
    g_server = NULL;

    for (int i = 0; i < SANDBOX_LINKS; i++) link_free(&g_sb.links[i]);
    server_log_set_writer(NULL, NULL);
    net_set_loopback_send(NULL);
    server_set_clock(NULL);
    g_sb.running = 0;
}

// ------------------------------------------------------------------ connections

int sandbox_connect(void) {
    if (!g_sb.running) return -1;
    for (int i = 0; i < SANDBOX_LINKS; i++) {
        SandboxLink *link = &g_sb.links[i];
        if (link->used) continue;
        *link = (SandboxLink) {.used = 1};

        ClientConnection conn = {0};
        conn.fd = SANDBOX_SERVER_FD(i);
        conn.addr.sin_family = AF_INET;
        conn.addr.sin_addr.s_addr = htonl(0x7f000001); // 127.0.0.1: the host's own machine (DEV)
        conn.transport = &g_sandbox_transport;

        pthread_mutex_lock(&g_server->lock);
        link->added = 1;
        net_add_client(&conn); // INIT ... into the client end's queue (a refusal closes the link)
        pthread_mutex_unlock(&g_server->lock);
        return SANDBOX_CLIENT_FD(i);
    }
    return -1;
}

void sandbox_disconnect(const int client_fd) {
    int server_end = 1;
    SandboxLink *link = link_by_fd(client_fd, &server_end);
    if (!link || server_end) return;
    link->client_closed = 1;
    link->to_client.length = 0;
    if (!link->added) link_free(link); // the server already let go of it
}

int sandbox_receive(const int client_fd, uint8_t *out, const size_t size) {
    int server_end = 1;
    SandboxLink *link = link_by_fd(client_fd, &server_end);
    if (!link || server_end) return -1;
    const size_t n = queue_take(&link->to_client, out, size);
    if (n) return (int) n;
    return link->server_closed ? -1 : 0;
}

// ------------------------------------------------------------------ step

void sandbox_step(const double seconds) {
    if (!g_sb.running) return;
    if (seconds > 0.0) g_sb.clock += seconds;

    pthread_mutex_lock(&g_server->lock);
    net_poll_clients(); // what server_bin's net thread does between ticks

    const double step = 1.0 / g_sb.tick_rate;
    if (g_sb.clock + 1e-6 >= g_sb.next_tick) {
        const float delta = (float) (g_sb.clock - g_sb.last_tick);
        g_sb.last_tick = g_sb.clock;
        g_sb.next_tick += step;
        if (g_sb.next_tick <= g_sb.clock) g_sb.next_tick = g_sb.clock + step; // fell behind: no burst of ticks
        server_core_tick((float) (g_sb.clock - g_sb.started_at), delta, g_sb.clock);
    }
    pthread_mutex_unlock(&g_server->lock);
}

// ------------------------------------------------------------------ host controls

void sandbox_host(const int map_index, const int mode_id) {
    if (!g_sb.running) return;
    pthread_mutex_lock(&g_server->lock);
    Game *game = &g_server->game;
    if (mode_id >= 0 && mode_config(mode_id)) { // config.modes = [id]: later rounds keep it
        int *modes = malloc(sizeof(int));
        if (modes) {
            modes[0] = mode_id;
            free(game->modes);
            game->modes = modes;
            game->mode_count = 1;
        }
    }
    const int map = map_index >= 0 && (size_t) map_index < game->map_count ? map_index : game->current_map_index;
    net_change_map(map, mode_id >= 0 ? 0 : game->current_mode_index);
    pthread_mutex_unlock(&g_server->lock);
}

int sandbox_set_maps(const cJSON **maps, const size_t count) {
    if (!g_sb.running || !maps || !count) return 0;
    const cJSON **copy = malloc(count * sizeof(*copy));
    if (!copy) return 0;
    memcpy(copy, maps, count * sizeof(*copy));
    pthread_mutex_lock(&g_server->lock);
    Game *game = &g_server->game;
    free((void *) game->maps);
    game->maps = copy;
    game->map_count = count;
    if (game->current_map_index >= (int) count) game->current_map_index = 0; // the next cycle starts over
    pthread_mutex_unlock(&g_server->lock);
    return 1;
}
