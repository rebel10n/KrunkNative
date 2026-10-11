#pragma once
// Server-info query responder + optional finder heartbeat (doc/backend.md 8.8).
// Wire format and pure encode/decode: shared/include/server_info.h. This file only owns
// the server's snapshot of what to report, its own UDP socket (until the game transport
// is UDP and answers queries on the game socket) and the finder heartbeat thread.
#include <server_info.h>
#include <stddef.h>
#include <stdint.h>

// Set to 1 (before info_query_start, e.g. at the top of net_main) when the game
// transport answers queries itself on the game socket: no separate UDP socket is opened.
extern volatile int g_info_query_external;

// Any thread, without g_server->lock: the reply to one datagram (0 = not a valid query).
size_t server_info_handle_datagram(const uint8_t *in, size_t len, uint8_t *out, size_t cap);

// Main thread. start: snapshot + UDP socket on query_port (0 = game port) + finder thread.
int info_query_start(void);
// Main thread, holding g_server->lock: refresh the snapshot (cheap; rate-limited inside).
void info_query_update(float now);
// Main thread: final "shutdown" heartbeat, close the socket, join the threads.
void info_query_stop(void);
