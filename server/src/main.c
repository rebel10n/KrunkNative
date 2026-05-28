#include <server.h>
#include <pthread.h>
#include <pcg_basic.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const ServerVersion g_version = {1, 0, 0};

Replxx *g_replxx;
Server *g_server;

static int parse_port(const char *value, unsigned short *port) {
    if (!value || !value[0]) return 0;

    char *end = NULL;
    errno = 0;
    const long parsed = strtol(value, &end, 10);

    if (errno || !end || *end || parsed <= 0 || parsed > 65535) return 0;

    *port = (unsigned short) parsed;
    return 1;
}

static int parse_args(const int argc, char **argv, const char **map_path, unsigned short *port) {
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (!strcmp(arg, "--port")) {
            if (++i >= argc || !parse_port(argv[i], port)) return 0;
            continue;
        }

        if (!strncmp(arg, "--port=", 7)) {
            if (!parse_port(arg + 7, port)) return 0;
            continue;
        }

        if (arg[0] == '-') return 0;

        if (*map_path) return 0;
        *map_path = arg;
    }

    return 1;
}

int main(int argc, char **argv) {
    char *rand_memory = malloc(1);
    pcg32_srandom(time(NULL), *(unsigned int *) &rand_memory);
    free(rand_memory);

    cJSON *startup_map = NULL;
    const char *startup_map_path = NULL;
    unsigned short listen_port = 21015;

    if (!parse_args(argc, argv, &startup_map_path, &listen_port)) {
        fprintf(stderr, "Usage: %s [--port PORT] [map.json]\n", argv[0]);
        return -1;
    }

    if (startup_map_path && load_json_file(startup_map_path, &startup_map)) {
        fprintf(stderr, "Failed to load map JSON: %s\n", startup_map_path);
        return -1;
    }

    g_replxx = replxx_init();
    replxx_bind_key(g_replxx, REPLXX_KEY_CONTROL('C'), cli_sigint, NULL);

    static Server INSTANCE = {};
    g_server = &INSTANCE;

    g_server->config.tick_rate = 30;
    g_server->config.listen_port = listen_port;
    g_server->config.max_players = 8;
    g_server->config.idle_timeout = 60.0f;
    g_server->config.idle_move_threshold = 1.0f;
    pthread_mutex_init(&g_server->lock, NULL);

    size_t startup_map_count = 0;
    const cJSON **startup_maps = map_list_with_custom(startup_map, &startup_map_count);

    if (startup_map && !startup_maps) {
        fprintf(stderr, "Failed to allocate startup map list\n");
        return -1;
    }

    g_server->game.server = g_server;
    game_configure(&g_server->game, NULL, startup_maps, startup_map_count, NULL, 0, NULL, 0, NULL, 0);
    game_init(&g_server->game, startup_map ? 0 : -1, -1, 0);

    replxx_print(g_replxx, "Starting KrunkNative server v%d.%d.%d... \n", g_version.major, g_version.minor, g_version.patch);
    replxx_print(g_replxx, "Loaded map %d mode %d \n", g_server->game.current_map_index, g_server->game.current_mode_index);

    pthread_t cli_thread;
    pthread_create(&cli_thread, NULL, (void *) cli_main, NULL);

    pthread_t net_thread;
    pthread_create(&net_thread, NULL, (void *) net_main, NULL);

    struct timespec last_tick;
    float now = 0;

    clock_gettime(CLOCK_MONOTONIC, &last_tick);

    while (!g_server->should_quit) {
        struct timespec tick_start;
        struct timespec tick_end;

        clock_gettime(CLOCK_MONOTONIC, &tick_start);

        const float delta = (float) (tick_start.tv_sec - last_tick.tv_sec) + (float) ((tick_start.tv_nsec - last_tick.tv_nsec) / 1e9);

        now += delta;
        last_tick = tick_start;

        pthread_mutex_lock(&g_server->lock);
        g_server->now = now;
        game_tick(&g_server->game, now, delta);
        net_check_idle_clients(now);
        net_broadcast_states();
        pthread_mutex_unlock(&g_server->lock);

        const long long expected_nsec = 1000000000 / g_server->config.tick_rate;
        long long elapsed_nsec = 0;

#ifdef WIN32
        do {
            clock_gettime(CLOCK_MONOTONIC, &tick_end);

            const time_t elapsed_sec = tick_end.tv_sec - tick_start.tv_sec;
            elapsed_nsec = elapsed_sec * 1000000000 + (tick_end.tv_nsec - tick_start.tv_nsec);
        } while (elapsed_nsec < expected_nsec && !g_server->should_quit);
#else
        long long sleep_nsec = expected_nsec - elapsed_nsec;
        struct timespec sleep_ts;

        sleep_ts.tv_sec = sleep_nsec / 1000000000LL;
        sleep_ts.tv_nsec = (long) (sleep_nsec % 1000000000LL);

        nanosleep(&sleep_ts, NULL);
#endif
    }

    // TODO: destructor logic

    net_cleanup();
    return 0;
}
