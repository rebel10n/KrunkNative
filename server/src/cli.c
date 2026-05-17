#include <server.h>

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
            replxx_print(g_replxx, "\n");
            continue;
        }

        // TODO: handle CLI input
    }
}