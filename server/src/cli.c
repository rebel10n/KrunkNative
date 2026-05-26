#include <errno.h>
#include <server.h>
#include <time.h>

ReplxxActionResult cli_sigint(int ignored, void *user_data) {
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
            struct timespec ts = {0, 100000000L};
            nanosleep(&ts, NULL);
            continue;
        }

        // TODO: handle CLI input
    }
}
