#pragma once
// Editor bus: the local IPC channel between map editor windows/processes and tools such as
// kn_editor_mcp. This header is the wire format and the transport; the editor side
// (routing, projects, permissions) lives in client/src/editor/editor_bus_link.c.
//
// Transport (native)
//   dir     $KRUNKNATIVE_EDITOR_BUS_DIR, else $XDG_RUNTIME_DIR/krunknative, else
//           /tmp/krunknative-<uid> (created 0700; `--bus-dir DIR` overrides)
//   socket  <dir>/editor-bus.sock   AF_UNIX stream, mode 0600
//   token   <dir>/editor-bus.token  64 hex chars + "\n", mode 0600, new per host session
//   lock    <dir>/editor-bus.lock   flock()ed by the host for its lifetime (takeover race)
//   port    <dir>/editor-bus.port   TCP fallback only (Windows, or KRUNKNATIVE_EDITOR_BUS_TCP=1):
//                                   127.0.0.1:<port>, same token
//   frame   4-byte big-endian length N, then N bytes of UTF-8 JSON (one object), N <= 64 MiB
//
// Protocol v1 (all messages are JSON objects with "t")
//   client -> host  {"t":"hello","token","role":"editor"|"mcp"|"tool","client_id","name","version","pid"}
//   host -> client  {"t":"welcome","proto":1,"peer":<id>,"host_pid"} | {"t":"error","code","message"} + close
//   client -> host  {"t":"permission_request","id","scopes":["read","edit","project"],"reason"?}
//   host -> client  {"t":"permission_result","id","granted":[..],"decision":"allow_once"|"always"|"deny"|"auto","reason"}
//   any             {"t":"req","id","op","args"}  ->  {"t":"res","id","ok":true,"result"} |
//                                                     {"t":"res","id","ok":false,"error":{"code","message"}}
//   host -> client  {"t":"ev","ev":"selection"|"change"|"projects"|"clipboard",...} (after subscribe)
// Ops, scopes and error codes: doc in client/src/editor/editor_bus_link.c (and edbus_scope_for).
#include <cJSON.h>
#include <stddef.h>
#include <stdint.h>

#define EDBUS_PROTO 1
#define EDBUS_MAX_FRAME (64u * 1024u * 1024u)
#define EDBUS_TOKEN_LEN 64

// permission scopes (bit mask)
#define EDBUS_SCOPE_READ 1u    // list, info, query, get, render, export, history, log, subscribe, copy
#define EDBUS_SCOPE_EDIT 2u    // add, update, remove, batch, paste, select, undo, redo, save
#define EDBUS_SCOPE_PROJECT 4u // open, close, focus, new
#define EDBUS_SCOPE_ALL 7u

typedef struct {
    char dir[512];
    char sock[600];
    char token[600];
    char lock[600];
    char port[600];
    int tcp; // TCP fallback (Windows, KRUNKNATIVE_EDITOR_BUS_TCP=1)
} EdBusPaths;

// Resolves the bus directory (override, env, XDG_RUNTIME_DIR, /tmp/krunknative-<uid>) and
// creates it 0700. Returns 0 when no usable directory exists.
int edbus_paths_init(EdBusPaths *p, const char *dir_override);

// 64 random hex chars (/dev/urandom; time/pid mixed fallback). out: EDBUS_TOKEN_LEN + 1.
int edbus_token_generate(char *out);
int edbus_token_write(const EdBusPaths *p, const char *token); // mode 0600
int edbus_token_read(const EdBusPaths *p, char *out);          // out: EDBUS_TOKEN_LEN + 1
int edbus_token_equal(const char *a, const char *b);            // constant time

// --- framing (pure; unit tested)
// Writes the 4-byte header + json into out (capacity >= len + 4). Returns bytes written.
size_t edbus_frame_encode(const char *json, size_t len, unsigned char *out);
// 1: a complete frame of *payload_len bytes starts at buf + 4 (*consumed = 4 + payload);
// 0: need more bytes; -1: oversized frame (protocol error).
int edbus_frame_decode(const unsigned char *buf, size_t len, size_t *payload_len, size_t *consumed);

// --- connections (non-blocking fds; buffered both ways)
typedef struct {
    int fd;
    unsigned char *in;
    size_t in_len, in_cap;
    unsigned char *out;
    size_t out_len, out_off, out_cap;
    int closed; // EOF / error seen
} EdBusConn;

void edbus_conn_init(EdBusConn *c, int fd);  // fd may be -1 (closed connection)
void edbus_conn_close(EdBusConn *c);         // closes the fd, frees the buffers
int edbus_send(EdBusConn *c, const cJSON *msg);                   // queue + flush what fits; 0 on error
int edbus_send_text(EdBusConn *c, const char *json, size_t len);  // same, pre-printed JSON
int edbus_flush(EdBusConn *c);                // 1 all sent, 0 pending, -1 error
int edbus_flush_blocking(EdBusConn *c, int timeout_ms); // 1 all sent
int edbus_read(EdBusConn *c);                 // reads what is available; -1 on EOF/error
// Next complete message (owned by the caller), NULL when none; *bad = 1 on a bad frame/JSON
// (the connection should be dropped).
cJSON *edbus_next(EdBusConn *c, int *bad);
// Simple clients: waits for the next message up to timeout_ms (-1 forever). NULL on
// timeout or EOF (c->closed set).
cJSON *edbus_recv(EdBusConn *c, int timeout_ms);
int edbus_wait_readable(int fd, int timeout_ms); // 1 readable, 0 timeout, -1 error

// --- sockets
// Host: listens on the socket (or 127.0.0.1:0 with the port file). Returns the fd or -1.
int edbus_listen(const EdBusPaths *p);
int edbus_accept(int listen_fd);             // non-blocking; -1 when nothing is pending
int edbus_connect(const EdBusPaths *p);      // -1 when no host answers
void edbus_close_fd(int fd);
// The host lock: returns a held lock fd, or -1 when another process holds it.
int edbus_lock_try(const EdBusPaths *p);
void edbus_lock_release(int fd);
// Host shutdown: removes the socket/port/token files it created.
void edbus_unlink(const EdBusPaths *p);

// Synchronous client handshake (kn_editor_mcp, tests): connect, hello with the token, wait
// for welcome. Returns 1 and fills *c; *welcome (optional) gets the welcome message.
int edbus_client_connect(EdBusConn *c, const EdBusPaths *p, const char *role, const char *client_id, const char *name,
                         const char *version, int timeout_ms, cJSON **welcome);

// --- message helpers
cJSON *edbus_msg(const char *type);                                  // {"t":type}
cJSON *edbus_req(int id, const char *op, cJSON *args);               // args owned (NULL = {})
cJSON *edbus_res_ok(const cJSON *id, cJSON *result);                 // result owned (NULL = {})
cJSON *edbus_res_err(const cJSON *id, const char *code, const char *message);
unsigned edbus_scope_parse(const char *list);    // "read,edit" / "all" / "1" -> mask
unsigned edbus_scope_from_json(const cJSON *arr); // ["read","edit"] -> mask
cJSON *edbus_scope_json(unsigned mask);
// The scope an op needs (op = host op; tool = the call tool for op "call"). 0 = none.
unsigned edbus_scope_for(const char *op, const char *tool);

// --- base64 (renders)
char *edbus_base64(const unsigned char *data, size_t len); // malloc'd, NUL-terminated
