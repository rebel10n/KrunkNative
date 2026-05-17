#include <server.h>
#include <pthread.h>

static const ServerVersion g_version = {1, 0, 0};

Replxx *g_replxx;
Server *g_server;

int main() {
    g_replxx = replxx_init();
    replxx_bind_key(g_replxx, REPLXX_KEY_CONTROL('C'), cli_sigint, NULL);

    static Server INSTANCE = {};
    g_server = &INSTANCE;

    g_server->config.tick_rate = 30;
    g_server->config.listen_port = 21015;

    g_server->game.server = g_server;

    replxx_print(g_replxx, "Starting KrunkNative server v%d.%d.%d... \n", g_version.major, g_version.minor, g_version.patch);

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

        game_tick(&g_server->game, now, delta);

        const long long expected_nsec = 1000000000 / g_server->config.tick_rate;
        long long elapsed_nsec = 0;

        do {
            clock_gettime(CLOCK_MONOTONIC, &tick_end);

            const time_t elapsed_sec = tick_end.tv_sec - tick_start.tv_sec;
            elapsed_nsec = elapsed_sec * 1000000000 + (tick_end.tv_nsec - tick_start.tv_nsec);
        } while (elapsed_nsec < expected_nsec && !g_server->should_quit);
    }

    // TODO: destructor logic

    net_cleanup();
    return 0;
}
