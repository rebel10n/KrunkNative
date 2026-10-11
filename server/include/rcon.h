#pragma once
// Remote console (doc/backend.md 8.7). Two ways in, both run the same console commands as
// the stdin console (cli_run_command) and return what they logged:
//   * local TCP (gameserver.rcon_port + rcon_password, bound to rcon_host = 127.0.0.1 by
//     default). Line protocol: first line = password -> "OK" / "ERR bad password" (closed);
//     then one command per line -> its output lines, then a line with a single ".".
//     `server_bin --rcon HOST:PORT --rcon-password PW [command...]` is the client.
//   * the coordinator: internal-WS frame {"type":"rcon","rid","command"} ->
//     {"type":"rcon-reply","rid","ok","output"} (the coordinator authenticated its admin).
#include <cJSON.h>

int rcon_start(void);                      // main thread; no-op when rcon_port or rcon_password is unset
void rcon_stop(void);
void rcon_handle_brain_frame(const cJSON *frame); // main thread, lock held: runs on a worker thread
// The client mode: connect, authenticate, run each command (or stdin lines). Returns the exit code.
int rcon_client_main(const char *target, const char *password, int argc, char **argv);
