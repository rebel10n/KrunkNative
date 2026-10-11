/*
 * knsvc/ws.h connection implementation (server after HTTP upgrade, and client).
 *
 * States: CONNECTING (client TCP connect) -> HANDSHAKE (client waiting for 101)
 * -> OPEN -> CLOSING (close frame sent, waiting for the peer's) -> CLOSED
 * (on_close delivered; the object lingers only to flush the last bytes).
 * Re-entrancy: callbacks may send/close/abort; the object is freed at the end
 * of the outermost event handler (busy counter), never inside a user callback.
 */
#include "http_internal.h"
#include "knsvc/crypto.h"
#include "knsvc/log.h"
#include "knsvc/ws.h"

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#define CLOSE_TIMEOUT_MS 5000
#define LINGER_MS 2000
#define READ_CHUNK 16384

enum { ST_CONNECTING, ST_HANDSHAKE, ST_OPEN, ST_CLOSING, ST_CLOSED };

struct kn_ws {
    kn_loop *loop;
    int fd;
    int client;
    int state;
    kn_ws_callbacks cb;
    void *ud;
    kn_ws_options opts;
    char *subprotocol_opt;    /* owned copy of opts.subprotocol */
    kn_buf in, out;
    kn_buf msg;               /* fragmented message being assembled */
    int msg_opcode;           /* 0 = none in progress */
    int busy, dead;
    int on_close_done;
    int drain_armed;
    int fail_pending;
    int close_code_sent;
    kn_timer_id ping_timer, close_timer, handshake_timer, linger_timer;
    uint64_t pong_deadline;
    char remote[256];
    char *path, *query;
    kn_http_header *headers;  /* server: upgrade request headers (owned strings) */
    size_t header_count;
    char *subprotocol;        /* negotiated */
    char client_key[32];
    char host_header[300];
    char *req_headers;        /* client: extra header block */
};

static void ws_on_io(kn_loop *loop, int fd, unsigned ev, void *ud);

/* ------------------------------------------------------------------ lifetime */

static void ws_free(kn_ws *ws) {
    if (ws->fd >= 0) {
        kn_loop_del_fd(ws->loop, ws->fd);
        close(ws->fd);
    }
    kn_loop_cancel_timer(ws->loop, ws->ping_timer);
    kn_loop_cancel_timer(ws->loop, ws->close_timer);
    kn_loop_cancel_timer(ws->loop, ws->handshake_timer);
    kn_loop_cancel_timer(ws->loop, ws->linger_timer);
    kn_buf_free(&ws->in);
    kn_buf_free(&ws->out);
    kn_buf_free(&ws->msg);
    for (size_t i = 0; i < ws->header_count; i++) {
        free((char *)ws->headers[i].name);
        free((char *)ws->headers[i].value);
    }
    free(ws->headers);
    free(ws->path);
    free(ws->query);
    free(ws->subprotocol);
    free(ws->subprotocol_opt);
    free(ws->req_headers);
    free(ws);
}

static void ws_enter(kn_ws *ws) { ws->busy++; }

static void ws_leave(kn_ws *ws) {
    if (--ws->busy == 0 && ws->dead) ws_free(ws);
}

static void linger_done(kn_loop *loop, void *ud) {
    kn_ws *ws = ud;
    ws->linger_timer = 0;
    ws_enter(ws);
    ws->dead = 1;
    ws_leave(ws);
}

/* Deliver on_close once, then let the object go (after flushing what is queued). */
static void ws_finish(kn_ws *ws, int code, const char *reason) {
    if (ws->on_close_done) return;
    ws->on_close_done = 1;
    ws->state = ST_CLOSED;
    kn_loop_cancel_timer(ws->loop, ws->ping_timer);
    kn_loop_cancel_timer(ws->loop, ws->close_timer);
    kn_loop_cancel_timer(ws->loop, ws->handshake_timer);
    ws->ping_timer = ws->close_timer = ws->handshake_timer = 0;
    ws_enter(ws);
    if (ws->cb.on_close) ws->cb.on_close(ws, code, reason ? reason : "", ws->ud);
    if (ws->fd >= 0 && ws->out.len && code != KN_WS_CLOSE_ABNORMAL) {
        /* flush the close frame; give up after LINGER_MS */
        kn_loop_mod_fd(ws->loop, ws->fd, KN_EV_WRITE);
        ws->linger_timer = kn_loop_add_timer(ws->loop, LINGER_MS, 0, linger_done, ws);
    } else {
        ws->dead = 1;
    }
    ws_leave(ws);
}

static void finish_task(kn_loop *loop, void *ud) {
    kn_ws *ws = ud; /* kept alive by the busy count taken in ws_fail_later */
    ws_finish(ws, KN_WS_CLOSE_ABNORMAL, ws->in.len ? ws->in.data : "connection failed");
    ws_leave(ws);
}

/* Client failure before/while connecting: on_close(1006) from the loop, never inside kn_ws_connect. */
static void ws_fail_later(kn_ws *ws, const char *why) {
    kn_buf_reset(&ws->in);
    kn_buf_append_str(&ws->in, why);
    if (ws->fd >= 0) {
        kn_loop_del_fd(ws->loop, ws->fd);
        close(ws->fd);
        ws->fd = -1;
    }
    if (ws->fail_pending) return;
    ws->fail_pending = 1;
    ws->state = ST_CLOSED;
    ws_enter(ws); /* released by finish_task */
    kn_loop_post(ws->loop, finish_task, ws);
}

/* ------------------------------------------------------------------ output */

static void ws_update_events(kn_ws *ws) {
    if (ws->fd < 0) return;
    unsigned ev = ws->state == ST_CLOSED ? 0u : KN_EV_READ;
    if (ws->out.len || ws->state == ST_CONNECTING) ev |= KN_EV_WRITE;
    kn_loop_mod_fd(ws->loop, ws->fd, ev);
}

static int ws_flush(kn_ws *ws) {
    while (ws->out.len) {
        ssize_t n = send(ws->fd, ws->out.data, ws->out.len, MSG_NOSIGNAL);
        if (n > 0) {
            kn_buf_consume(&ws->out, (size_t)n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        return -1;
    }
    ws_update_events(ws);
    if (ws->drain_armed && ws->out.len < ws->opts.max_send_buffer / 4 && ws->state == ST_OPEN) {
        ws->drain_armed = 0;
        if (ws->cb.on_drain) ws->cb.on_drain(ws, ws->ud);
    }
    return 0;
}

static int ws_queue_frame(kn_ws *ws, int opcode, const void *data, size_t len) {
    uint8_t mask[4];
    if (ws->client && kn_random_bytes(mask, 4) != 0) return -1;
    return kn_ws_frame_encode(&ws->out, opcode, 1, data, len, ws->client ? mask : NULL);
}

static void send_close_frame(kn_ws *ws, int code, const char *reason) {
    uint8_t p[125];
    size_t n = 0;
    if (code && code != KN_WS_CLOSE_NO_STATUS && code != KN_WS_CLOSE_ABNORMAL) {
        p[0] = (uint8_t)(code >> 8);
        p[1] = (uint8_t)code;
        n = 2;
        size_t rl = reason ? strlen(reason) : 0;
        if (rl > 123) rl = 123;
        if (rl && kn_utf8_valid((const uint8_t *)reason, rl)) {
            memcpy(p + 2, reason, rl);
            n += rl;
        }
    }
    ws->close_code_sent = code ? code : KN_WS_CLOSE_NO_STATUS;
    ws_queue_frame(ws, 8, p, n);
}

/* Protocol violation / local error: send close and drop without waiting. */
static void ws_fail(kn_ws *ws, int code, const char *reason) {
    if (ws->state == ST_OPEN || ws->state == ST_CLOSING) {
        if (!ws->close_code_sent) send_close_frame(ws, code, reason);
        ws_flush(ws);
    }
    ws_finish(ws, code, reason);
    if (ws->fd >= 0) shutdown(ws->fd, SHUT_RDWR);
}

static int ws_send(kn_ws *ws, int opcode, const void *data, size_t len) {
    if (!ws || ws->state != ST_OPEN) return -1;
    if (ws->out.len + len > ws->opts.max_send_buffer) {
        KN_WARN("ws", "%s: send buffer overflow (%zu bytes queued), closing", ws->remote, ws->out.len);
        ws_enter(ws);
        ws_fail(ws, KN_WS_CLOSE_POLICY, "send buffer overflow");
        ws_leave(ws);
        return -1;
    }
    if (ws_queue_frame(ws, opcode, data, len) != 0) return -1;
    if (ws->out.len > ws->opts.max_send_buffer / 2) ws->drain_armed = 1;
    ws_enter(ws);
    if (!ws->busy || ws->busy == 1) {
        if (ws_flush(ws) != 0) ws_fail(ws, KN_WS_CLOSE_ABNORMAL, "write failed");
    } else {
        ws_update_events(ws); /* inside a callback: flush on the next writable event */
    }
    ws_leave(ws);
    return 0;
}

int kn_ws_send_text(kn_ws *ws, const char *text, size_t len) {
    if (len == (size_t)-1) len = strlen(text);
    return ws_send(ws, 1, text, len);
}

int kn_ws_send_binary(kn_ws *ws, const void *data, size_t len) { return ws_send(ws, 2, data, len); }

int kn_ws_send_ping(kn_ws *ws, const void *data, size_t len) {
    if (len > 125) return -1;
    return ws_send(ws, 9, data, len);
}

static void close_timeout(kn_loop *loop, void *ud) {
    kn_ws *ws = ud;
    ws->close_timer = 0;
    ws_enter(ws);
    ws_finish(ws, KN_WS_CLOSE_ABNORMAL, "close handshake timeout");
    ws_leave(ws);
}

void kn_ws_close(kn_ws *ws, int code, const char *reason) {
    if (!ws) return;
    ws_enter(ws);
    if (ws->state == ST_OPEN) {
        send_close_frame(ws, code ? code : KN_WS_CLOSE_NORMAL, reason);
        ws->state = ST_CLOSING;
        ws->close_timer = kn_loop_add_timer(ws->loop, CLOSE_TIMEOUT_MS, 0, close_timeout, ws);
        if (ws_flush(ws) != 0) ws_finish(ws, KN_WS_CLOSE_ABNORMAL, "write failed");
    } else if (ws->state == ST_CONNECTING || ws->state == ST_HANDSHAKE) {
        ws_finish(ws, KN_WS_CLOSE_ABNORMAL, "closed before open");
    }
    ws_leave(ws);
}

void kn_ws_abort(kn_ws *ws) {
    if (!ws) return;
    ws_enter(ws);
    kn_buf_reset(&ws->out);
    ws_finish(ws, KN_WS_CLOSE_ABNORMAL, "aborted");
    ws_leave(ws);
}

/* ------------------------------------------------------------------ accessors */

size_t kn_ws_buffered(const kn_ws *ws) { return ws->out.len; }
int kn_ws_is_open(const kn_ws *ws) { return ws && ws->state == ST_OPEN; }
void kn_ws_set_userdata(kn_ws *ws, void *ud) { ws->ud = ud; }
void *kn_ws_userdata(const kn_ws *ws) { return ws->ud; }
const char *kn_ws_remote_addr(const kn_ws *ws) { return ws->remote; }
const char *kn_ws_path(const kn_ws *ws) { return ws->path ? ws->path : "/"; }
const char *kn_ws_query(const kn_ws *ws) { return ws->query ? ws->query : ""; }
const char *kn_ws_subprotocol(const kn_ws *ws) { return ws->subprotocol; }
kn_loop *kn_ws_loop(const kn_ws *ws) { return ws->loop; }

const char *kn_ws_header(const kn_ws *ws, const char *name) {
    for (size_t i = 0; i < ws->header_count; i++)
        if (strcasecmp(ws->headers[i].name, name) == 0) return ws->headers[i].value;
    return NULL;
}

/* ------------------------------------------------------------------ incoming frames */

static int valid_close_code(int c) {
    if (c >= 3000 && c <= 4999) return 1;
    return c >= 1000 && c <= 1014 && c != 1004 && c != 1005 && c != 1006;
}

static void deliver(kn_ws *ws, int opcode, uint8_t *data, size_t len) {
    if (opcode == 1 && !kn_utf8_valid(data, len)) {
        ws_fail(ws, KN_WS_CLOSE_INVALID_DATA, "invalid utf-8");
        return;
    }
    if (ws->cb.on_message) ws->cb.on_message(ws, data, len, opcode == 2, ws->ud);
}

static void handle_close_frame(kn_ws *ws, uint8_t *p, size_t len) {
    int code = KN_WS_CLOSE_NO_STATUS;
    char reason[124] = "";
    if (len == 1) {
        ws_fail(ws, KN_WS_CLOSE_PROTOCOL, "bad close frame");
        return;
    }
    if (len >= 2) {
        code = p[0] << 8 | p[1];
        if (!valid_close_code(code)) {
            ws_fail(ws, KN_WS_CLOSE_PROTOCOL, "bad close code");
            return;
        }
        if (!kn_utf8_valid(p + 2, len - 2)) {
            ws_fail(ws, KN_WS_CLOSE_INVALID_DATA, "invalid close reason");
            return;
        }
        memcpy(reason, p + 2, len - 2);
        reason[len - 2] = 0;
    }
    if (ws->state == ST_OPEN) {
        if (code == KN_WS_CLOSE_NO_STATUS) {
            ws->close_code_sent = code;
            ws_queue_frame(ws, 8, NULL, 0); /* echo an empty close */
        } else {
            send_close_frame(ws, code, NULL);
        }
    }
    ws_flush(ws);
    ws_finish(ws, code, reason);
    if (!ws->client && ws->fd >= 0 && !ws->out.len) shutdown(ws->fd, SHUT_WR); /* server closes TCP first */
}

/* One complete frame (payload unmasked). Returns -1 when processing must stop. */
static int handle_frame(kn_ws *ws, const kn_ws_frame *f, uint8_t *p) {
    size_t len = (size_t)f->payload_len;
    switch (f->opcode) {
    case 1:
    case 2:
        if (ws->msg_opcode) return ws_fail(ws, KN_WS_CLOSE_PROTOCOL, "expected continuation"), -1;
        if (ws->state != ST_OPEN) return 0; /* data after our close: ignore */
        if (f->fin) {
            uint8_t saved = p[len];
            p[len] = 0;
            deliver(ws, f->opcode, p, len);
            p[len] = saved;
            return 0;
        }
        ws->msg_opcode = f->opcode;
        kn_buf_reset(&ws->msg);
        kn_buf_append(&ws->msg, p, len);
        return 0;
    case 0:
        if (!ws->msg_opcode) return ws_fail(ws, KN_WS_CLOSE_PROTOCOL, "unexpected continuation"), -1;
        if (ws->msg.len + len > ws->opts.max_message_bytes)
            return ws_fail(ws, KN_WS_CLOSE_TOO_BIG, "message too big"), -1;
        kn_buf_append(&ws->msg, p, len);
        if (f->fin) {
            int op = ws->msg_opcode;
            ws->msg_opcode = 0;
            if (ws->state == ST_OPEN) deliver(ws, op, (uint8_t *)ws->msg.data, ws->msg.len);
            kn_buf_reset(&ws->msg);
        }
        return 0;
    case 8:
        handle_close_frame(ws, p, len);
        return -1;
    case 9:
        if (ws->state == ST_OPEN) {
            ws_queue_frame(ws, 10, p, len);
            ws_update_events(ws);
        }
        return 0;
    case 10:
        ws->pong_deadline = 0;
        if (ws->cb.on_pong) ws->cb.on_pong(ws, p, len, ws->ud);
        return 0;
    }
    return 0;
}

static void process_frames(kn_ws *ws) {
    size_t pos = 0;
    while (!ws->on_close_done && pos < ws->in.len) {
        kn_ws_frame f;
        uint8_t *d = (uint8_t *)ws->in.data + pos;
        size_t avail = ws->in.len - pos;
        int r = kn_ws_frame_parse(d, avail, &f);
        if (r < 0) {
            ws_fail(ws, KN_WS_CLOSE_PROTOCOL, "protocol error");
            break;
        }
        if (r == 0) break;
        if (f.masked != !ws->client) {
            ws_fail(ws, KN_WS_CLOSE_PROTOCOL, ws->client ? "masked server frame" : "unmasked client frame");
            break;
        }
        if (f.payload_len > ws->opts.max_message_bytes) {
            ws_fail(ws, KN_WS_CLOSE_TOO_BIG, "message too big");
            break;
        }
        if (avail - f.header_len < f.payload_len) break;
        uint8_t *p = d + f.header_len;
        if (f.masked) kn_ws_mask(p, (size_t)f.payload_len, f.mask, 0);
        pos += f.header_len + (size_t)f.payload_len;
        /* p[len] may be overwritten temporarily (NUL for text): in.data has a guard byte */
        if (handle_frame(ws, &f, p) < 0) break;
    }
    if (!ws->on_close_done) kn_buf_consume(&ws->in, pos);
}

/* ------------------------------------------------------------------ ping keepalive */

static void ping_tick(kn_loop *loop, void *ud) {
    kn_ws *ws = ud;
    if (ws->state != ST_OPEN) return;
    ws_enter(ws);
    uint64_t now = kn_now_ms();
    if (ws->pong_deadline && now > ws->pong_deadline) {
        KN_DEBUG("ws", "%s: pong timeout", ws->remote);
        kn_buf_reset(&ws->out);
        ws_finish(ws, KN_WS_CLOSE_ABNORMAL, "pong timeout");
    } else {
        if (!ws->pong_deadline) ws->pong_deadline = now + (uint64_t)ws->opts.pong_timeout_ms;
        ws_queue_frame(ws, 9, "kn", 2);
        if (ws_flush(ws) != 0) ws_finish(ws, KN_WS_CLOSE_ABNORMAL, "write failed");
    }
    ws_leave(ws);
}

static void ws_opened(kn_ws *ws) {
    ws->state = ST_OPEN;
    kn_loop_cancel_timer(ws->loop, ws->handshake_timer);
    ws->handshake_timer = 0;
    if (ws->opts.ping_interval_ms > 0)
        ws->ping_timer = kn_loop_add_timer(ws->loop, (uint64_t)ws->opts.ping_interval_ms,
                                           (uint64_t)ws->opts.ping_interval_ms, ping_tick, ws);
    if (ws->cb.on_open) ws->cb.on_open(ws, ws->ud);
}

/* ------------------------------------------------------------------ client handshake */

static const char *find_resp_header(char *head, const char *name, char *out, size_t cap) {
    size_t nl = strlen(name);
    for (char *p = strchr(head, '\n'); p; p = strchr(p, '\n')) {
        p++;
        if (strncasecmp(p, name, nl) == 0 && p[nl] == ':') {
            const char *v = p + nl + 1;
            while (*v == ' ' || *v == '\t') v++;
            size_t n = strcspn(v, "\r\n");
            if (n >= cap) n = cap - 1;
            memcpy(out, v, n);
            out[n] = 0;
            while (n && (out[n - 1] == ' ' || out[n - 1] == '\t')) out[--n] = 0;
            return out;
        }
    }
    return NULL;
}

static void client_check_response(kn_ws *ws) {
    char *end = strstr(ws->in.data, "\r\n\r\n");
    if (!end) {
        if (ws->in.len > 16384) ws_fail_later(ws, "handshake response too large");
        return;
    }
    size_t head_len = (size_t)(end - ws->in.data) + 4;
    char *head = strndup(ws->in.data, head_len);
    if (!head) {
        ws_fail_later(ws, "out of memory");
        return;
    }
    char v[256], expect[29];
    const char *err = NULL;
    int status = 0;
    if (sscanf(head, "HTTP/1.%*d %d", &status) != 1 || status != 101) err = "server refused upgrade";
    else if (!find_resp_header(head, "Upgrade", v, sizeof v) || strcasecmp(v, "websocket") != 0) err = "bad Upgrade header";
    else if (!find_resp_header(head, "Sec-WebSocket-Accept", v, sizeof v) ||
             kn_ws_accept_key(ws->client_key, expect) != 0 || strcmp(v, expect) != 0)
        err = "bad Sec-WebSocket-Accept";
    else if (find_resp_header(head, "Sec-WebSocket-Extensions", v, sizeof v)) err = "unrequested extension";
    if (!err && find_resp_header(head, "Sec-WebSocket-Protocol", v, sizeof v)) {
        if (!ws->subprotocol_opt || strcmp(v, ws->subprotocol_opt) != 0) err = "unrequested subprotocol";
        else ws->subprotocol = strdup(v);
    }
    if (err) {
        char msg[300];
        snprintf(msg, sizeof msg, "%s (status %d)", err, status);
        free(head);
        ws_fail_later(ws, msg);
        return;
    }
    free(head);
    kn_buf_consume(&ws->in, head_len);
    ws_opened(ws);
}

static void client_send_handshake(kn_ws *ws) {
    uint8_t key[16];
    kn_random_bytes(key, sizeof key);
    kn_base64_encode(key, sizeof key, ws->client_key, sizeof ws->client_key, 0, 1);
    kn_buf *o = &ws->out;
    kn_buf_appendf(o, "GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                      "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\nUser-Agent: knsvc\r\n",
                   ws->path, ws->host_header, ws->client_key);
    if (ws->subprotocol_opt) kn_buf_appendf(o, "Sec-WebSocket-Protocol: %s\r\n", ws->subprotocol_opt);
    if (ws->req_headers) kn_buf_append_str(o, ws->req_headers);
    kn_buf_append_str(o, "\r\n");
    ws->state = ST_HANDSHAKE;
}

static void handshake_timeout(kn_loop *loop, void *ud) {
    kn_ws *ws = ud;
    ws->handshake_timer = 0;
    ws_enter(ws);
    if (ws->state < ST_OPEN) {
        if (ws->fd >= 0) {
            kn_loop_del_fd(ws->loop, ws->fd);
            close(ws->fd);
            ws->fd = -1;
        }
        ws_finish(ws, KN_WS_CLOSE_ABNORMAL, "handshake timeout");
    }
    ws_leave(ws);
}

/* ------------------------------------------------------------------ I/O */

static void ws_read(kn_ws *ws) {
    for (;;) {
        if (kn_buf_reserve(&ws->in, READ_CHUNK) != 0) {
            ws_fail(ws, KN_WS_CLOSE_INTERNAL, "out of memory");
            return;
        }
        ssize_t n = recv(ws->fd, ws->in.data + ws->in.len, READ_CHUNK, 0);
        if (n > 0) {
            ws->in.len += (size_t)n;
            ws->in.data[ws->in.len] = 0;
            if (ws->state == ST_HANDSHAKE) {
                client_check_response(ws);
                if (ws->state != ST_OPEN) return;
            }
            process_frames(ws);
            if (ws->on_close_done) return;
            continue;
        }
        if (n == 0) {
            ws_finish(ws, KN_WS_CLOSE_ABNORMAL, "connection closed");
            return;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        ws_finish(ws, KN_WS_CLOSE_ABNORMAL, strerror(errno));
        return;
    }
}

static void ws_on_io(kn_loop *loop, int fd, unsigned ev, void *ud) {
    kn_ws *ws = ud;
    ws_enter(ws);
    if (ws->state == ST_CLOSED) {
        /* lingering: flush the close frame, then go */
        if (ws_flush(ws) != 0 || !ws->out.len) ws->dead = 1;
        ws_leave(ws);
        return;
    }
    if (ws->state == ST_CONNECTING) {
        int err = 0;
        socklen_t el = sizeof err;
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
        if (err || (ev & KN_EV_ERROR)) {
            ws_fail_later(ws, strerror(err ? err : ECONNREFUSED));
            ws_leave(ws);
            return;
        }
        if (!(ev & KN_EV_WRITE)) {
            ws_leave(ws);
            return;
        }
        client_send_handshake(ws);
    }
    if (ev & KN_EV_WRITE) {
        if (ws_flush(ws) != 0) ws_finish(ws, KN_WS_CLOSE_ABNORMAL, "write failed");
    }
    if (!ws->on_close_done && (ev & (KN_EV_READ | KN_EV_HUP | KN_EV_ERROR))) ws_read(ws);
    if (!ws->on_close_done && ws->out.len) {
        if (ws_flush(ws) != 0) ws_finish(ws, KN_WS_CLOSE_ABNORMAL, "write failed");
    }
    ws_leave(ws);
}

/* ------------------------------------------------------------------ construction */

static void apply_defaults(kn_ws *ws, const kn_ws_options *opts) {
    if (opts) ws->opts = *opts;
    if (!ws->opts.max_message_bytes) ws->opts.max_message_bytes = 1024 * 1024;
    if (!ws->opts.max_send_buffer) ws->opts.max_send_buffer = 4 * 1024 * 1024;
    if (ws->opts.ping_interval_ms > 0 && !ws->opts.pong_timeout_ms) ws->opts.pong_timeout_ms = 2 * ws->opts.ping_interval_ms;
    if (!ws->opts.handshake_timeout_ms) ws->opts.handshake_timeout_ms = 10000;
    if (ws->opts.subprotocol) ws->subprotocol_opt = strdup(ws->opts.subprotocol);
    ws->opts.subprotocol = NULL;
    ws->opts.headers = NULL;
    ws->opts.header_count = 0;
    ws->opts.origin = NULL; /* used immediately by the client handshake builder only */
}

static kn_ws *ws_new(kn_loop *loop, const kn_ws_callbacks *cb, const kn_ws_options *opts, void *ud) {
    kn_ws *ws = calloc(1, sizeof *ws);
    if (!ws) return NULL;
    ws->loop = loop;
    ws->fd = -1;
    if (cb) ws->cb = *cb;
    ws->ud = ud;
    apply_defaults(ws, opts);
    return ws;
}

/* Is token in the comma separated header value? */
static int header_has_token(const char *v, const char *tok) {
    size_t tl = strlen(tok);
    for (const char *p = v; p && *p;) {
        while (*p == ' ' || *p == ',' || *p == '\t') p++;
        size_t n = strcspn(p, ", \t");
        if (n == tl && strncasecmp(p, tok, tl) == 0) return 1;
        p += n;
    }
    return 0;
}

kn_ws *kn_ws_accept(kn_http_req *req, const kn_ws_callbacks *cb, const kn_ws_options *opts, void *ud) {
    const char *key = kn_http_header_get(req, "Sec-WebSocket-Key");
    const char *ver = kn_http_header_get(req, "Sec-WebSocket-Version");
    uint8_t raw[32];
    if (!kn_http_is_ws_upgrade(req)) {
        kn_http_set_header(req, "Upgrade", "websocket");
        kn_http_respond_error(req, 426, "websocket upgrade required");
        return NULL;
    }
    if (!ver || strcmp(ver, "13") != 0) {
        kn_http_set_header(req, "Sec-WebSocket-Version", "13");
        kn_http_respond_error(req, 426, "unsupported websocket version");
        return NULL;
    }
    if (!key || kn_base64_decode(key, strlen(key), raw, sizeof raw) != 16) {
        kn_http_respond_error(req, 400, "bad Sec-WebSocket-Key");
        return NULL;
    }
    char accept[29];
    if (kn_ws_accept_key(key, accept) != 0) {
        kn_http_respond_error(req, 400, "bad Sec-WebSocket-Key");
        return NULL;
    }
    kn_ws *ws = ws_new(kn_http_server_loop(req->server), cb, opts, ud);
    if (!ws) {
        kn_http_respond_error(req, 500, "out of memory");
        return NULL;
    }
    ws->path = strdup(req->path);
    ws->query = strdup(req->query);
    ws->headers = calloc(req->header_count ? req->header_count : 1, sizeof *ws->headers);
    for (size_t i = 0; ws->headers && i < req->header_count; i++) {
        ws->headers[i].name = strdup(req->headers[i].name);
        ws->headers[i].value = strdup(req->headers[i].value);
        ws->header_count++;
    }
    const char *offered = kn_http_header_get(req, "Sec-WebSocket-Protocol");
    if (ws->subprotocol_opt && offered && header_has_token(offered, ws->subprotocol_opt))
        ws->subprotocol = strdup(ws->subprotocol_opt);

    kn_http_takeover t;
    if (kn_http__takeover(req, &t) != 0) {
        ws_free(ws);
        return NULL;
    }
    ws->fd = t.fd;
    ws->loop = t.loop;
    ws->in = t.leftover;
    snprintf(ws->remote, sizeof ws->remote, "%s", t.remote_addr);
    kn_buf_appendf(&ws->out, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                             "Sec-WebSocket-Accept: %s\r\n", accept);
    if (ws->subprotocol) kn_buf_appendf(&ws->out, "Sec-WebSocket-Protocol: %s\r\n", ws->subprotocol);
    kn_buf_append_str(&ws->out, "\r\n");
    if (kn_loop_add_fd(ws->loop, ws->fd, KN_EV_READ | KN_EV_WRITE, ws_on_io, ws) != 0) {
        ws_free(ws);
        return NULL;
    }
    ws_enter(ws);
    if (ws_flush(ws) != 0) {
        ws->state = ST_OPEN;
        ws_finish(ws, KN_WS_CLOSE_ABNORMAL, "write failed");
        ws_leave(ws);
        return NULL;
    }
    ws_opened(ws);
    if (!ws->on_close_done && ws->in.len) process_frames(ws);
    kn_ws *ret = ws->dead ? NULL : ws;
    ws_leave(ws);
    return ret;
}

static int start_connect(kn_ws *ws, const kn_url *u) {
    char port[16];
    snprintf(port, sizeof port, "%d", u->port);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int rc = getaddrinfo(u->host, port, &hints, &res);
    if (rc != 0) {
        ws_fail_later(ws, gai_strerror(rc));
        return 0;
    }
    int fd = -1, err = 0;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC, ai->ai_protocol);
        if (fd < 0) {
            err = errno;
            continue;
        }
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0 || errno == EINPROGRESS) break;
        err = errno;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) {
        ws_fail_later(ws, strerror(err ? err : ECONNREFUSED));
        return 0;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    ws->fd = fd;
    if (kn_loop_add_fd(ws->loop, fd, KN_EV_READ | KN_EV_WRITE, ws_on_io, ws) != 0) {
        ws_fail_later(ws, "loop registration failed");
        return 0;
    }
    return 0;
}

kn_ws *kn_ws_connect(kn_loop *loop, const char *url, const kn_ws_callbacks *cb, const kn_ws_options *opts, void *ud) {
    kn_url u;
    if (!url || kn_url_parse(url, &u) != 0 || strcmp(u.scheme, "ws") != 0) {
        KN_ERROR("ws", "unsupported websocket url: %s", url ? url : "(null)");
        return NULL;
    }
    kn_ws *ws = ws_new(loop, cb, opts, ud);
    if (!ws) return NULL;
    ws->client = 1;
    ws->state = ST_CONNECTING;
    ws->path = strdup(u.path);
    const char *q = strchr(u.path, '?');
    ws->query = strdup(q ? q + 1 : "");
    int v6 = strchr(u.host, ':') != NULL;
    if (u.port == 80) snprintf(ws->host_header, sizeof ws->host_header, v6 ? "[%s]" : "%s", u.host);
    else snprintf(ws->host_header, sizeof ws->host_header, v6 ? "[%s]:%d" : "%s:%d", u.host, u.port);
    snprintf(ws->remote, sizeof ws->remote, "%s", u.host);
    if (opts && opts->origin) {
        /* keep origin in the extra-header block (opts strings are not retained) */
        kn_buf hb = {0};
        kn_buf_appendf(&hb, "Origin: %s\r\n", opts->origin);
        ws->req_headers = kn_buf_detach(&hb, NULL);
    }
    if (opts && opts->header_count) {
        kn_buf hb = {0};
        if (ws->req_headers) kn_buf_append_str(&hb, ws->req_headers);
        for (size_t i = 0; i < opts->header_count; i++) {
            const char *h = opts->headers[i];
            if (!h || strpbrk(h, "\r\n")) continue;
            kn_buf_appendf(&hb, "%s\r\n", h);
        }
        free(ws->req_headers);
        ws->req_headers = kn_buf_detach(&hb, NULL);
    }
    if (!ws->path || !ws->query) {
        ws_free(ws);
        return NULL;
    }
    ws->handshake_timer = kn_loop_add_timer(loop, (uint64_t)ws->opts.handshake_timeout_ms, 0, handshake_timeout, ws);
    start_connect(ws, &u);
    return ws;
}
