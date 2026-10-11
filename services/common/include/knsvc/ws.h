/*
 * knsvc/ws.h - RFC 6455 WebSocket server (upgrade from kn_http) and client.
 *
 * Handshake, framing, client masking, fragmentation (reassembled before
 * on_message; control frames may interleave), ping/pong (automatic pong;
 * optional keep-alive pings with a pong timeout), close handshake with codes,
 * UTF-8 validation of text messages and close reasons, message size limit
 * (1009), send-buffer backpressure. No extensions (permessage-deflate is
 * declined). TLS (wss://) is terminated by nginx in deployment; the client
 * speaks ws:// only.
 *
 * Lifetime: a kn_ws is created by kn_ws_accept/kn_ws_connect and freed by
 * knsvc right after on_close returns. on_close is called exactly once (also
 * when the connection fails before on_open: code 1006). Do not use the
 * pointer after on_close. All callbacks run on the loop thread.
 *
 * Backpressure: kn_ws_buffered() reports queued outbound bytes; on_drain fires
 * when the queue drops below max_send_buffer/4 after having exceeded
 * max_send_buffer/2.
 * A send that would push the queue over max_send_buffer closes the socket with
 * 1008 ("send buffer overflow") and returns -1 (slow consumer protection).
 */
#ifndef KNSVC_WS_H
#define KNSVC_WS_H

#include <stddef.h>
#include <stdint.h>

#include "knsvc/buf.h"
#include "knsvc/http.h"
#include "knsvc/loop.h"

typedef struct kn_ws kn_ws;

enum {
    KN_WS_CLOSE_NORMAL = 1000,
    KN_WS_CLOSE_GOING_AWAY = 1001,
    KN_WS_CLOSE_PROTOCOL = 1002,
    KN_WS_CLOSE_UNSUPPORTED = 1003,
    KN_WS_CLOSE_NO_STATUS = 1005,    /* reported when the peer sent no code */
    KN_WS_CLOSE_ABNORMAL = 1006,     /* reported when the TCP connection dropped */
    KN_WS_CLOSE_INVALID_DATA = 1007,
    KN_WS_CLOSE_POLICY = 1008,
    KN_WS_CLOSE_TOO_BIG = 1009,
    KN_WS_CLOSE_INTERNAL = 1011
};

typedef struct kn_ws_callbacks {
    void (*on_open)(kn_ws *ws, void *ud);
    /* Full (reassembled) message. data is valid only during the call;
     * text messages are valid UTF-8 and NUL-terminated (data[len] == 0). */
    void (*on_message)(kn_ws *ws, const void *data, size_t len, int is_binary, void *ud);
    void (*on_close)(kn_ws *ws, int code, const char *reason, void *ud);
    void (*on_drain)(kn_ws *ws, void *ud);              /* optional */
    void (*on_pong)(kn_ws *ws, const void *data, size_t len, void *ud); /* optional */
} kn_ws_callbacks;

typedef struct kn_ws_options {
    size_t max_message_bytes;     /* default 1 MiB -> close 1009 */
    size_t max_send_buffer;       /* default 4 MiB */
    int ping_interval_ms;         /* 0 = no automatic pings */
    int pong_timeout_ms;          /* close if no pong within this (default 2x interval) */
    int handshake_timeout_ms;     /* client: connect + upgrade must finish, default 10000 */
    const char *subprotocol;      /* server: accept this one if offered; client: request it */
    /* client only: extra request headers, "Name: value" each (e.g. "Authorization: Bearer x") */
    const char *const *headers;
    size_t header_count;
    const char *origin;           /* client: Origin header (optional) */
} kn_ws_options;

/* Server: upgrade an HTTP request (GET with Upgrade: websocket) inside a route
 * handler. Consumes req. On a bad handshake answers 400/426, returns NULL and
 * calls nothing. opts may be NULL (defaults). on_open runs right after the 101. */
kn_ws *kn_ws_accept(kn_http_req *req, const kn_ws_callbacks *cb, const kn_ws_options *opts, void *ud);

/* Client: connect to ws://host[:port]/path?query. Returns NULL only for an
 * invalid URL / out of memory; network errors arrive as on_close(1006, reason).
 * DNS resolution is synchronous (getaddrinfo). */
kn_ws *kn_ws_connect(kn_loop *loop, const char *url, const kn_ws_callbacks *cb,
                     const kn_ws_options *opts, void *ud);

int kn_ws_send_text(kn_ws *ws, const char *text, size_t len);    /* len (size_t)-1 = strlen */
int kn_ws_send_binary(kn_ws *ws, const void *data, size_t len);
int kn_ws_send_ping(kn_ws *ws, const void *data, size_t len);     /* len <= 125 */
/* Start the close handshake (code 0 = 1000). on_close follows once the peer
 * answers or after a 5 s timeout. */
void kn_ws_close(kn_ws *ws, int code, const char *reason);
/* Drop the TCP connection now (on_close(1006) is still delivered, synchronously). */
void kn_ws_abort(kn_ws *ws);

size_t kn_ws_buffered(const kn_ws *ws);
int kn_ws_is_open(const kn_ws *ws);
void kn_ws_set_userdata(kn_ws *ws, void *ud);  /* replaces the ud passed to callbacks */
void *kn_ws_userdata(const kn_ws *ws);
const char *kn_ws_remote_addr(const kn_ws *ws); /* honours trust_proxy of the HTTP server */
const char *kn_ws_path(const kn_ws *ws);        /* server: request path; client: URL path */
const char *kn_ws_query(const kn_ws *ws);       /* server: raw query string ("" if none) */
const char *kn_ws_header(const kn_ws *ws, const char *name); /* server: upgrade request header */
const char *kn_ws_subprotocol(const kn_ws *ws); /* negotiated, or NULL */
kn_loop *kn_ws_loop(const kn_ws *ws);

/* ---- low-level framing (exposed for tests) */
typedef struct kn_ws_frame {
    int fin;
    int opcode;          /* 0 cont, 1 text, 2 binary, 8 close, 9 ping, 10 pong */
    int masked;
    uint8_t mask[4];
    uint64_t payload_len;
    size_t header_len;   /* bytes before the payload */
} kn_ws_frame;

/* Parse a frame header. Returns 1 when a header is complete (payload may still
 * be incomplete: check len >= header_len + payload_len), 0 need more, -1 protocol
 * error (reserved bits, bad opcode, fragmented/oversized control frame,
 * non-minimal length encoding or 64-bit length with the high bit set). */
int kn_ws_frame_parse(const uint8_t *data, size_t len, kn_ws_frame *out);
/* Encode a frame (header + payload, masked when mask != NULL). Appends to out. */
int kn_ws_frame_encode(kn_buf *out, int opcode, int fin, const void *payload, size_t len, const uint8_t mask[4]);
void kn_ws_mask(uint8_t *data, size_t len, const uint8_t mask[4], size_t offset);
/* Sec-WebSocket-Accept for a client key (out >= 29 bytes). */
int kn_ws_accept_key(const char *client_key, char out[29]);
int kn_utf8_valid(const uint8_t *s, size_t len);

#endif
