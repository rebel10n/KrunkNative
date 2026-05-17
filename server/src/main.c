#include <server.h>
#include <pthread.h>
#include <errno.h>

static const ServerVersion g_version = {1, 0, 0};

Replxx *g_replxx;
Server *g_server;

ReplxxActionResult sigint(int ignored, void *user_data) {
    g_server->should_quit = 1;
    return 0;
}

[[noreturn]] void cli_main() {
    while (1) {
        const char *line;

        do {
            line = replxx_input(g_replxx, "> ");
        } while (!line && errno == EAGAIN);

        if (!line) {
            replxx_print(g_replxx, "\n");
            continue;
        }

        // TODO: handle CLI input
    }
}

int main() {
    g_replxx = replxx_init();
    replxx_bind_key(g_replxx, REPLXX_KEY_CONTROL('C'), sigint, NULL);

    static Server INSTANCE = {};

    g_server = &INSTANCE;
    g_server->tick_rate = 30;

    pthread_t cli_thread;
    pthread_create(&cli_thread, NULL, (void *) cli_main, NULL);

    replxx_print(g_replxx, "Starting KrunkNative server v%d.%d.%d... \n", g_version.major, g_version.minor, g_version.patch);

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

        const long long expected_nsec = 1000000000 / g_server->tick_rate;
        long long elapsed_nsec = 0;

        do {
            clock_gettime(CLOCK_MONOTONIC, &tick_end);

            const time_t elapsed_sec = tick_end.tv_sec - tick_start.tv_sec;
            elapsed_nsec = elapsed_sec * 1000000000 + (tick_end.tv_nsec - tick_start.tv_nsec);
        } while (elapsed_nsec < expected_nsec && !g_server->should_quit);
    }

    // TODO: destructor logic
    return 0;
}
