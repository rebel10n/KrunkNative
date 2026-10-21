/*
 * v10_bridge.h - the `v10_bridge` process: the official Krunker v10 client's WebSocket (msgpack,
 * v10 labels) <-> a KrunkerBox game server's TCP (knet frames, shared/include/shared.h).
 *
 * C port of net-bridge/v10Proxy.ts (the sockets and the order the packets run in). The packet
 * mappings are in v10_translate.h. One session per WebSocket: its own TCP connection to the native
 * server, opened when the client connects (the forwarded-address frame first, as the TS does).
 *
 * Built on knsvc (services/common): the HTTP server and WebSocket of ws.h, one kn_loop for every
 * socket. Requires the knsvc build (KRUNKNATIVE_SERVICES, OpenSSL).
 */
#ifndef KN_V10_BRIDGE_H
#define KN_V10_BRIDGE_H

#include <stddef.h>

#include "knsvc/http.h"
#include "knsvc/loop.h"

typedef struct V10Bridge V10Bridge;

typedef struct V10BridgeConfig {
    const char *bind_host;    /* the WebSocket listener's address, default 127.0.0.1 */
    int listen_port;          /* 0 = ephemeral (v10_bridge_port) */
    const char *native_host;  /* the KrunkerBox game server, default 127.0.0.1 */
    int native_port;          /* default 21015 */
    const char *lobby_id;     /* the inst-id the client shows, default "KN:local" */
} V10BridgeConfig;

/* Creates the bridge on the loop and routes GET /ws to it. Nothing listens until v10_bridge_listen. */
V10Bridge *v10_bridge_new(kn_loop *loop, const V10BridgeConfig *cfg);
int v10_bridge_listen(V10Bridge *b);  /* 0 ok, -1 (logged) */
int v10_bridge_port(const V10Bridge *b);
/* Starts the close (1001 bridge stopping) of every session; they go on until their close completes. */
void v10_bridge_close_all(V10Bridge *b);
/* Drops every session and the listener. */
void v10_bridge_free(V10Bridge *b);
/* How many times a native packet stopped the translation for this process, by name (the TS untranslatedSeen). */
int v10_bridge_untranslated(const char *name);

#endif
