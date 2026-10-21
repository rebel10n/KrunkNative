/*
 * v10_bridge.c - the v10 client's WebSocket <-> the native TCP server (see v10_bridge.h).
 *
 * The order of the work is net-bridge/v10Proxy.ts: each handler below is the TS function of the same
 * name, with the same stops. Connect sequence (v10.js _73891 joinServer, 222104): the bridge sends
 * io-init, load (INIT_LOAD) and inst-id, opens TCP to the native server and writes the forwarded-address
 * frame first. The client's LOAD -> HELLO; MAP_INFO -> MAP_REQUEST and MAP_DATA chunks; GAME_CONFIG and
 * NETWORK_DATA; the INIT is held until the map JSON, the config, the networking and the ranked flag are
 * all in, and the packets behind it wait in a queue. After the init, each native packet is a v10 message
 * or consumed; the first one with no translation stops the session (nothing more goes to the client).
 *
 * Single-threaded: every socket and the WebSocket run on one kn_loop. A session is freed only when the
 * WebSocket has closed and no callback of it is on the stack (busy), so a send that closes it
 * synchronously is safe.
 */
#include "v10_bridge.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "cJSON.h"
#include "knsvc/buf.h"
#include "knsvc/crypto.h"
#include "knsvc/http.h"
#include "knsvc/loop.h"
#include "knsvc/ws.h"
#include "krunker_proto.h"
#include "net_ext.h"
#include "ai.h"        /* NET_PACKET_AI_SYNC_V7, NET_PACKET_AI_ADD_V7 */
#include "shared.h"
#include "social_v7.h" /* NET_PACKET_CHAT_V7, NET_PACKET_SOCIAL_V7 */
#include "v10_translate.h"

#define V10_WS_PATH "/ws"
#define V10_STATE_SEND_HZ 10       /* NET_STATE_SEND_HZ: the "k" sendRate */
#define V10_INIT_LOAD_ARG 20000    /* the first INIT_LOAD argument, as proxy.ts sends it */
#define V10_NET_HEADER 3
#define V10_NATIVE_MAX_PAYLOAD 512 /* writeNative: a larger payload is a protocol error */
#define V10_READ_CHUNK 65536
#define V10_MAX_MAP_BYTES ((uint32_t)128 * 1024 * 1024)
#define V10_SOCKET_ID_LEN 12
#define V10_FORWARDED_MAX 48
#define V10_DEFAULT_NATIVE_PORT 21015
#define V10_DEFAULT_LOBBY "KN:local"
#define V10_MAX_SEND_BUFFER ((size_t)64 * 1024 * 1024)
#define V10_MAX_MESSAGE ((size_t)100 * 1024 * 1024) /* the ws library's default maxPayload (the TS bridge) */

/* EXT subtypes (net_ext.h): the game state, the announcement and the ranked-match flag */
#define V10_EXT_RANKED NET_EXT_KN_RANKED_MATCH

typedef struct V10Queued {
    uint8_t type;
    uint8_t *payload;
    size_t len;
} V10Queued;

typedef struct V10SkinsEntry {
    V10Skins skins;
} V10SkinsEntry;

typedef struct V10Session {
    V10Bridge *bridge;
    struct V10Session *prev, *next;
    kn_ws *ws;
    int busy;        /* callbacks of this session on the stack */
    int dead;        /* the WebSocket has closed: free when not busy */
    int closed;      /* the session is closing: nothing more goes to the client */
    int fd;          /* the TCP connection to the native server, -1 none */
    int connected;
    char forwarded_addr[V10_FORWARDED_MAX + 1];
    char socket_id[V10_SOCKET_ID_LEN + 1];
    int64_t started_ms;
    kn_buf out;      /* native frames waiting for the socket (the forwarded address is the first) */
    size_t out_off;
    kn_buf in;       /* native bytes not yet framed */

    int hello_sent;
    V10MapInfo map_info;
    int have_map_info;
    V10Chunks map_assembly;
    int have_map_assembly;
    uint8_t *map_text;   /* the map JSON, whole */
    size_t map_len;
    int have_map_text;
    V10Chunks config_assembly;
    int have_config_assembly;
    cJSON *config_root;  /* init argument 6: the parsed GAME_CONFIG */
    uint8_t *config_text;   /* the GAME_CONFIG text the init carries: a repeat of it is no change */
    size_t config_len;
    int have_config_text;
    int network_received;
    V10Network network;
    int32_t mode_index;
    V10StateRec *kbatch;     /* the player records of the states in this chunk: one "k" per chunk */
    size_t kn, kcap;
    int32_t client_id;
    int objective_weapon;
    V10BotEntry *ai_sync;    /* the bot positions of the sync being assembled (AI_SYNC_V7 chunks) */
    size_t ai_n, ai_cap;
    int ai_active;
    int32_t leader_rows[64]; /* the rows of the last LEADERBOARD, for the LEADER_STATS that repeats them */
    size_t leader_count;
    int leader_have;
    V10SkinsEntry *skins;    /* SKINS by uid, kept for the SPAWN that follows */
    size_t skins_n, skins_cap;
    int32_t game_timer;
    int32_t chall_mode;
    int ranked_received;
    int ranked;
    V10Init pending_init;
    int have_pending_init;
    V10Queued *queue;
    size_t queue_n, queue_cap;
    int init_sent;
    int global_values_sent;
    int stopped;
} V10Session;

struct V10Bridge {
    kn_loop *loop;
    kn_http_server *http;
    char bind_host[64];
    int listen_port;
    char native_host[256];
    int native_port;
    char lobby_id[64];
    int trace;          /* KRUNK_NATIVE_V10_TRACE=1: every label each way (the TS trace, the same lines) */
    V10Session *head;
};

/* ---- native packet readers and the stop table */

static int32_t rd_i32(const uint8_t *p, size_t at) {
    return (int32_t)((uint32_t)p[at] | (uint32_t)p[at + 1] << 8 | (uint32_t)p[at + 2] << 16 | (uint32_t)p[at + 3] << 24);
}
static uint16_t rd_u16(const uint8_t *p, size_t at) { return (uint16_t)(p[at] | p[at + 1] << 8); }

#define STOP_SLOTS 48
static struct {
    char name[48];
    int count;
} g_stops[STOP_SLOTS];

static void count_stop(const char *name) {
    int free_slot = -1;
    for (int i = 0; i < STOP_SLOTS; i++) {
        if (g_stops[i].count && !strcmp(g_stops[i].name, name)) {
            g_stops[i].count++;
            return;
        }
        if (!g_stops[i].count && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) return;
    snprintf(g_stops[free_slot].name, sizeof g_stops[free_slot].name, "%s", name);
    g_stops[free_slot].count = 1;
}

int v10_bridge_untranslated(const char *name) {
    for (int i = 0; i < STOP_SLOTS; i++)
        if (g_stops[i].count && !strcmp(g_stops[i].name, name)) return g_stops[i].count;
    return 0;
}

/* nativeName (v10Proxy.ts): the packet's name for the log and the stop count. */
static const char *native_name(uint8_t type, const uint8_t *p, size_t n) {
    static char other[32];
    if (type == NET_PACKET_EXT && n >= 2) {
        const uint16_t sub = rd_u16(p, 0);
        if (sub == NET_EXT_GAME_STATE) return "EXT 0x0901 GAME_STATE";
        snprintf(other, sizeof other, "EXT 0x%04x", (unsigned)sub);
        return other;
    }
    switch (type) {
    case NET_PACKET_HELLO: return "HELLO";
    case NET_PACKET_INIT: return "INIT";
    case NET_PACKET_ENT: return "ENT";
    case NET_PACKET_MAP_INFO: return "MAP_INFO";
    case NET_PACKET_MAP_REQUEST: return "MAP_REQUEST";
    case NET_PACKET_MAP_DATA: return "MAP_DATA";
    case NET_PACKET_NETWORK_DATA: return "NETWORK_DATA";
    case NET_PACKET_GAME_CONFIG: return "GAME_CONFIG";
    case NET_PACKET_GLOBAL_VALUE: return "GLOBAL_VALUE";
    case NET_PACKET_PLAYER_SOUND: return "PLAYER_SOUND";
    case NET_PACKET_TIMER: return "TIMER";
    case NET_PACKET_CHEAT: return "CHEAT";
    case NET_PACKET_PICKUP: return "PICKUP";
    case NET_PACKET_CHAT: return "CHAT";
    case NET_PACKET_MATCH_VOTE_STATE: return "MATCH_VOTE_STATE";
    case NET_PACKET_MODE_STATE: return "MODE_STATE";
    case NET_PACKET_OBJECT_STATE: return "OBJECT_STATE";
    case NET_PACKET_MODE_SYNC_V7: return "MODE_SYNC_V7";
    case NET_PACKET_LEADERBOARD: return "LEADERBOARD";
    case NET_PACKET_LEADER_STATS: return "LEADER_STATS";
    case NET_PACKET_AI_ADD_V7: return "AI_ADD_V7";
    case NET_PACKET_DOM_UPDATE: return "DOM_UPDATE";
    case NET_PACKET_CHAT_V7: return "CHAT_V7";
    case NET_PACKET_STATE: return "STATE";
    case NET_PACKET_AI_SYNC_V7: return "AI_SYNC_V7";
    default: break;
    }
    snprintf(other, sizeof other, "native %d", (int)type);
    return other;
}

static void stop_at(V10Session *s, const char *name, const char *reason) {
    if (s->stopped) return;
    s->stopped = 1;
    for (size_t i = 0; i < s->queue_n; i++) free(s->queue[i].payload);
    s->queue_n = 0;
    count_stop(name);
    fprintf(stderr, "[v10-bridge] stop: native %s has no v10 translation yet (%s)\n", name, reason);
}

/* ---- session lifetime */

static void session_link(V10Session *s) {
    V10Bridge *b = s->bridge;
    s->next = b->head;
    s->prev = NULL;
    if (b->head) b->head->prev = s;
    b->head = s;
}

static void session_unlink(V10Session *s) {
    V10Bridge *b = s->bridge;
    if (s->prev) s->prev->next = s->next;
    else if (b->head == s) b->head = s->next;
    if (s->next) s->next->prev = s->prev;
    s->prev = s->next = NULL;
}

static void tcp_close(V10Session *s) {
    if (s->fd >= 0) {
        kn_loop_del_fd(s->bridge->loop, s->fd);
        close(s->fd);
        s->fd = -1;
    }
    s->connected = 0;
}

static void session_free(V10Session *s) {
    session_unlink(s);
    tcp_close(s);
    kn_buf_free(&s->out);
    kn_buf_free(&s->in);
    v10_chunks_free(&s->map_assembly);
    v10_chunks_free(&s->config_assembly);
    free(s->map_text);
    free(s->config_text);
    cJSON_Delete(s->config_root);
    free(s->kbatch);
    free(s->ai_sync);
    free(s->skins);
    for (size_t i = 0; i < s->queue_n; i++) free(s->queue[i].payload);
    free(s->queue);
    free(s);
}

static void hold(V10Session *s) { s->busy++; }
static void release(V10Session *s) {
    if (--s->busy == 0 && s->dead) session_free(s);
}

/* Ends the session: the native socket now, the WebSocket by its close handshake (or at once, force).
 * force: the WebSocket's on_close runs before this returns, so the caller must not touch s after it. */
static void close_session(V10Session *s, int code, const char *reason, int force) {
    if (s->closed) return;
    s->closed = 1;
    tcp_close(s);
    if (!s->ws) return;
    if (force) kn_ws_abort(s->ws);
    else kn_ws_close(s->ws, code, reason);
}

/* ---- the WebSocket side */

#define TRACE(s, ...) do { if ((s)->bridge->trace) { fprintf(stderr, "[v10-proxy] "); fprintf(stderr, __VA_ARGS__); } } while (0)

static void send_ws(V10Session *s, const char *label, const KrunkerValue *args, uint32_t argc) {
    if (s->closed || !s->ws) return;
    TRACE(s, "%.5s -> %s\n", s->socket_id, label);
    KrunkerBuf buf = {0};
    if (krunker_proto_encode_ah(0, label, args, argc, &buf) == KRUNKER_PROTO_OK) kn_ws_send_binary(s->ws, buf.data, buf.len);
    krunker_buf_free(&buf);
}

static void send_out(V10Session *s, V10Out *o) {
    for (size_t i = 0; i < o->count; i++) send_ws(s, o->msgs[i].label, o->msgs[i].args, o->msgs[i].argc);
}

/* A mapping's messages go out, or the packet stops the session when the mapping stopped. */
static void finish_out(V10Session *s, V10Out *o, const char *name) {
    if (o->stop || o->oom) stop_at(s, name, o->stop ? o->stop : "out of memory");
    else send_out(s, o);
    v10_out_free(o);
}

/* ---- the native side */

static void flush_out(V10Session *s) {
    if (!s->connected || s->fd < 0) return;
    while (s->out_off < s->out.len) {
        const ssize_t w = send(s->fd, s->out.data + s->out_off, s->out.len - s->out_off, MSG_NOSIGNAL);
        if (w > 0) {
            s->out_off += (size_t)w;
            continue;
        }
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            kn_loop_mod_fd(s->bridge->loop, s->fd, KN_EV_READ | KN_EV_WRITE);
            return;
        }
        close_session(s, 1011, "native server unavailable", 0);
        return;
    }
    s->out.len = 0;
    s->out_off = 0;
    if (s->fd >= 0) kn_loop_mod_fd(s->bridge->loop, s->fd, KN_EV_READ);
}

/* writeNative: a frame to the native server (queued until the connection is up). */
static void write_native(V10Session *s, uint8_t type, const uint8_t *p, size_t n) {
    if (s->closed) return;
    if (n > V10_NATIVE_MAX_PAYLOAD) {
        close_session(s, 1011, "Native server protocol error", 0);
        return;
    }
    const uint8_t head[V10_NET_HEADER] = {type, (uint8_t)(n >> 8), (uint8_t)n};
    if (kn_buf_append(&s->out, head, sizeof head) || (n && kn_buf_append(&s->out, p, n))) {
        close_session(s, 1011, "out of memory", 0);
        return;
    }
    flush_out(s);
}

static void queue_push(V10Session *s, uint8_t type, const uint8_t *p, size_t n) {
    if (s->queue_n == s->queue_cap) {
        const size_t cap = s->queue_cap ? s->queue_cap * 2 : 8;
        V10Queued *q = realloc(s->queue, cap * sizeof *q);
        if (!q) {
            close_session(s, 1011, "out of memory", 0);
            return;
        }
        s->queue = q;
        s->queue_cap = cap;
    }
    uint8_t *copy = malloc(n ? n : 1);
    if (!copy) {
        close_session(s, 1011, "out of memory", 0);
        return;
    }
    if (n) memcpy(copy, p, n);
    s->queue[s->queue_n++] = (V10Queued){.type = type, .payload = copy, .len = n};
}

static void tcp_on_io(kn_loop *loop, int fd, unsigned events, void *ud);

static void tcp_read(V10Session *s) {
    uint8_t *buf = malloc(V10_READ_CHUNK);
    if (!buf) {
        close_session(s, 1011, "out of memory", 0);
        return;
    }
    for (;;) {
        const ssize_t r = recv(s->fd, buf, V10_READ_CHUNK, 0);
        if (r > 0) {
            if (kn_buf_append(&s->in, buf, (size_t)r)) {
                free(buf);
                close_session(s, 1011, "out of memory", 0);
                return;
            }
            continue;
        }
        if (r == 0) {
            free(buf);
            TRACE(s, "%.5s native socket closed\n", s->socket_id);
            close_session(s, 1011, "native server closed", 0);
            return;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        free(buf);
        close_session(s, 1011, "native server unavailable", 0);
        return;
    }
    free(buf);
}

static void handle_native(V10Session *s, uint8_t type, const uint8_t *p, size_t n);

/* handleNativeData: every whole frame of the bytes read, then the player states of the chunk as one "k". */
static void flush_player_states(V10Session *s);

static void handle_native_data(V10Session *s) {
    while (!s->closed && s->in.len >= V10_NET_HEADER) {
        /* the native frame length is big endian (the TS readUInt16BE; shared.h NET_PACKET_HEADER) */
        const uint8_t *hdr = (const uint8_t *)s->in.data;
        const size_t size = (size_t)hdr[1] << 8 | hdr[2];
        if (s->in.len < V10_NET_HEADER + size) break;
        const uint8_t type = (uint8_t)s->in.data[0];
        handle_native(s, type, (const uint8_t *)s->in.data + V10_NET_HEADER, size);
        kn_buf_consume(&s->in, V10_NET_HEADER + size);
    }
    if (!s->closed) flush_player_states(s);
}

static void tcp_on_io(kn_loop *loop, int fd, unsigned events, void *ud) {
    (void)loop;
    V10Session *s = ud;
    hold(s);
    if (s->closed || s->fd != fd) {
        release(s);
        return;
    }
    if (!s->connected) {
        int err = 0;
        socklen_t el = sizeof err;
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) != 0) err = errno;
        if (err || (events & KN_EV_ERROR) || ((events & KN_EV_HUP) && !(events & KN_EV_WRITE))) {
            close_session(s, 1011, "native server unavailable", 0);
            release(s);
            return;
        }
        s->connected = 1;
        const int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        kn_loop_mod_fd(s->bridge->loop, fd, KN_EV_READ);
        flush_out(s); /* the forwarded address and whatever was queued before the connection */
    }
    if (!s->closed && (events & (KN_EV_READ | KN_EV_HUP | KN_EV_ERROR))) {
        tcp_read(s);
        if (!s->closed) handle_native_data(s);
    }
    if (!s->closed && (events & KN_EV_WRITE) && s->connected) flush_out(s);
    release(s);
}

/* connectNative: the TCP connection to the native server, non-blocking. */
static int connect_native(V10Session *s) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char port[12];
    snprintf(port, sizeof port, "%d", s->bridge->native_port);
    if (getaddrinfo(s->bridge->native_host, port, &hints, &res) != 0 || !res) return -1;
    const int fd = kn_socket_nb(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
        return -1;
    }
    if (connect(fd, res->ai_addr, res->ai_addrlen) != 0 && errno != EINPROGRESS) {
        close(fd);
        freeaddrinfo(res);
        return -1;
    }
    freeaddrinfo(res);
    if (kn_loop_add_fd(s->bridge->loop, fd, KN_EV_WRITE, tcp_on_io, s) != 0) {
        close(fd);
        return -1;
    }
    s->fd = fd;
    return 0;
}

/* ---- the connect sequence */

static int is_loopback(const char *a) { return !strcmp(a, "::1") || !strncmp(a, "127.", 4) || !strncmp(a, "::ffff:127.", 11); }

static const char *trim_span(const char *s, size_t *len) {
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) n--;
    *len = n;
    return s;
}

/* UTILS.peerAddress: the socket's peer; behind our own loopback proxy, the forwarded client (the
 * rightmost X-Forwarded-For, else X-Real-IP); IPv4-mapped prefix dropped; 48 bytes at most. */
static void peer_address(kn_ws *ws, char out[V10_FORWARDED_MAX + 1]) {
    char addr[80];
    const char *raw = kn_ws_remote_addr(ws);
    snprintf(addr, sizeof addr, "%s", raw ? raw : "");
    if (is_loopback(addr)) {
        const char *xff = kn_ws_header(ws, "X-Forwarded-For");
        char last[80] = "";
        size_t ll = 0;
        if (xff) {
            const char *comma = strrchr(xff, ',');
            const char *seg = comma ? comma + 1 : xff;
            size_t sl = 0;
            const char *t = trim_span(seg, &sl);
            if (sl && sl < sizeof last) {
                memcpy(last, t, sl);
                last[sl] = 0;
                ll = sl;
            }
        }
        if (ll) {
            snprintf(addr, sizeof addr, "%s", last);
        } else {
            const char *xri = kn_ws_header(ws, "X-Real-IP");
            size_t xl = 0;
            if (xri) {
                const char *t = trim_span(xri, &xl);
                if (xl && xl < sizeof addr) {
                    memcpy(addr, t, xl);
                    addr[xl] = 0;
                }
            }
        }
    }
    const char *a = addr;
    if (!strncmp(a, "::ffff:", 7)) a += 7;
    size_t n = strlen(a);
    if (n > V10_FORWARDED_MAX) n = V10_FORWARDED_MAX;
    if (n == 0) {
        memcpy(out, "?", 2);
        return;
    }
    memcpy(out, a, n);
    out[n] = 0;
}

static void make_socket_id(char out[V10_SOCKET_ID_LEN + 1], int *ok) {
    static const char alpha[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    uint8_t r[V10_SOCKET_ID_LEN];
    *ok = kn_random_bytes(r, sizeof r) == 0;
    for (int i = 0; i < V10_SOCKET_ID_LEN; i++) out[i] = alpha[r[i] % (sizeof alpha - 1)];
    out[V10_SOCKET_ID_LEN] = 0;
}

/* ---- client packets (handleClientPacket) */

static void handle_client(V10Session *s, const char *label, const KrunkerValue *args, uint32_t argc) {
    const KrunkerValue *a0 = argc ? &args[0] : NULL;
    if (!strcmp(label, V10_OUT_LOAD)) {
        /* the client is ready: the native server answers with INIT once it has HELLO */
        if (s->hello_sent) return;
        s->hello_sent = 1;
        uint8_t hello[V10_HELLO_PAYLOAD_SIZE];
        v10_hello_payload(hello);
        write_native(s, NET_PACKET_HELLO, hello, sizeof hello);
    } else if (!strcmp(label, V10_OUT_ENTER_GAME)) {
        uint8_t ent[V10_ENT_SIZE];
        if (v10_encode_enter(a0, ent)) write_native(s, NET_PACKET_ENT, ent, sizeof ent);
        /* challMode: the join's array item 7 (EN.CHALLENGE), truthy */
        s->chall_mode = a0 && a0->type == KRUNKER_ARRAY && a0->len > 7 && v10_truthy(&a0->u.items[7]) ? 1 : 0;
    } else if (!strcmp(label, V10_OUT_CHAT_MESSAGE)) {
        uint8_t text[V10_CHAT_TEXT_SIZE];
        const size_t n = v10_encode_chat(a0, text);
        if (n) write_native(s, NET_PACKET_CHAT_SEND, text, n);
    }
    /* anything else (movement input, pings, the rest of the v10 labels) is not translated yet: dropped */
}

/* ---- the native packets (handleNativePacket) */

static int skins_index(const V10Session *s, int32_t uid) {
    for (size_t i = 0; i < s->skins_n; i++)
        if (s->skins[i].skins.uid == uid) return (int)i;
    return -1;
}

static void flush_bot_sync(V10Session *s) {
    if (!s->ai_active) return;
    s->ai_active = 0;
    V10Out o;
    v10_out_init(&o);
    KrunkerValue list = s->ai_n ? v10_bot_sync_list(&o, s->ai_sync, s->ai_n) : KRUNKER_V_NUM(0);
    v10_push(&o, V10_IN_SYNC_ALL_AI, &list, 1);
    send_out(s, &o);
    v10_out_free(&o);
    s->ai_n = 0;
}

static void flush_player_states(V10Session *s) {
    if (s->kn == 0 || s->stopped) {
        s->kn = 0;
        return;
    }
    V10Out o;
    v10_out_init(&o);
    KrunkerValue *records = v10_arena(&o, s->kn * sizeof *records);
    for (size_t i = 0; records && i < s->kn; i++) records[i] = v10_state_record(&o, &s->kbatch[i]);
    if (records) {
        const KrunkerValue args[3] = {
            KRUNKER_V_ARRAY(records, (uint32_t)s->kn),
            KRUNKER_V_NUM(V10_STATE_SEND_HZ),
            KRUNKER_V_NUM((double)(kn_now_ms() - (uint64_t)s->started_ms)),
        };
        send_ws(s, V10_IN_UPDATE_PLAYERS, args, 3);
    }
    v10_out_free(&o);
    s->kn = 0;
}

static int kbatch_push(V10Session *s, const V10StateRec *r) {
    if (s->kn == s->kcap) {
        const size_t cap = s->kcap ? s->kcap * 2 : 16;
        V10StateRec *k = realloc(s->kbatch, cap * sizeof *k);
        if (!k) return -1;
        s->kbatch = k;
        s->kcap = cap;
    }
    s->kbatch[s->kn++] = *r;
    return 0;
}

static int ai_push(V10Session *s, const V10BotChunk *c) {
    for (size_t i = 0; i < c->count; i++) {
        if (s->ai_n == s->ai_cap) {
            const size_t cap = s->ai_cap ? s->ai_cap * 2 : 32;
            V10BotEntry *e = realloc(s->ai_sync, cap * sizeof *e);
            if (!e) return -1;
            s->ai_sync = e;
            s->ai_cap = cap;
        }
        s->ai_sync[s->ai_n++] = c->entries[i];
    }
    return 0;
}

static void set_skins(V10Session *s, const V10Skins *skins) {
    const int at = skins_index(s, skins->uid);
    if (at >= 0) {
        s->skins[at].skins = *skins;
        return;
    }
    if (s->skins_n == s->skins_cap) {
        const size_t cap = s->skins_cap ? s->skins_cap * 2 : 8;
        V10SkinsEntry *e = realloc(s->skins, cap * sizeof *e);
        if (!e) return;
        s->skins = e;
        s->skins_cap = cap;
    }
    s->skins[s->skins_n++].skins = *skins;
}

static void emit_init(V10Session *s);

static void try_emit_init(V10Session *s) {
    if (!s->have_pending_init || !s->have_map_text || !s->have_config_text || !s->network_received || !s->ranked_received) return;
    emit_init(s);
}

static void emit_init(V10Session *s) {
    if (!s->have_pending_init) return;
    const V10Init init = s->pending_init;
    s->have_pending_init = 0;
    V10Out o;
    v10_out_init(&o);
    const KrunkerValue config = v10_config_value(&o, s->config_root);
    const KrunkerValue network = v10_network_value(&o, &s->network);
    v10_init_game(&o, &init, s->map_text, s->map_len, config, network, s->ranked);
    send_out(s, &o);
    v10_out_free(&o);
    s->init_sent = 1;
    s->mode_index = init.mode_index;
    s->client_id = init.client_id;
    /* the held packets run in order; a stop empties the rest */
    const size_t held_n = s->queue_n;
    V10Queued *held = s->queue;
    s->queue = NULL;
    s->queue_n = 0;
    s->queue_cap = 0;
    for (size_t i = 0; i < held_n; i++) {
        if (!s->closed) handle_native(s, held[i].type, held[i].payload, held[i].len);
        free(held[i].payload);
    }
    free(held);
}

static void handle_native(V10Session *s, uint8_t type, const uint8_t *p, size_t n) {
    if (s->closed) return;
    if (s->bridge->trace) {
        char hex[24] = "";
        if (n <= 8) {
            for (size_t i = 0; i < n; i++) snprintf(hex + 1 + 2 * i, sizeof hex - 1 - 2 * i, "%02x", p[i]);
            hex[0] = n ? ' ' : 0;
        }
        TRACE(s, "%.5s native %s (%zu B)%s\n", s->socket_id, native_name(type, p, n), n, hex);
    }
    if (type == NET_PACKET_PING) {
        write_native(s, NET_PACKET_PONG, p, n);
        return;
    }
    if (s->stopped) return;
    const int ranked = type == NET_PACKET_EXT && n >= 3 && rd_u16(p, 0) == V10_EXT_RANKED;
    const int init_part = type == NET_PACKET_MAP_DATA || type == NET_PACKET_GAME_CONFIG || type == NET_PACKET_NETWORK_DATA || ranked;
    if (s->have_pending_init && !init_part) {
        queue_push(s, type, p, n);
        return;
    }
    V10Out o;
    if (s->init_sent && type == NET_PACKET_GLOBAL_VALUE) {
        V10GlobalValue gv;
        if (!v10_parse_global_value(p, n, &gv)) { stop_at(s, "GLOBAL_VALUE", "malformed"); return; }
        /* a reset clears the client's set: v10 gv cannot, once a value was sent */
        if (gv.reset && s->global_values_sent) { stop_at(s, "GLOBAL_VALUE", "reset after values: v10 gv cannot clear"); return; }
        if (gv.key[0] == 0) return;
        s->global_values_sent = 1;
        v10_out_init(&o);
        const KrunkerValue args[2] = {KRUNKER_V_STR(gv.key), KRUNKER_V_NUM(gv.value)};
        v10_push(&o, V10_IN_UPDATE_GLOBAL_VALUE, args, 2);
        finish_out(s, &o, "GLOBAL_VALUE");
        return;
    }
    if (s->init_sent && (type == NET_PACKET_MODE_STATE || type == NET_PACKET_OBJECT_STATE)) {
        v10_out_init(&o);
        if (type == NET_PACKET_MODE_STATE) v10_map_mode_state(&o, p, n);
        else v10_map_object_state(&o, p, n);
        if (type == NET_PACKET_MODE_STATE && !o.stop) s->objective_weapon = rd_i32(p, 80) >= 0;
        finish_out(s, &o, native_name(type, p, n));
        return;
    }
    if (ranked) {
        const int value = p[2] != 0;
        if (s->init_sent && value != s->ranked) {
            { stop_at(s, "EXT 0x8050 RANKED", "the ranked flag changed after the init: v10 takes it in the init only"); return; }
        }
        s->ranked = value;
        s->ranked_received = 1;
        try_emit_init(s);
        return;
    }
    /* the match phase (0 INITIALIZING .. 5 ENDED, v10's enum): gsc [gameState] (v10.js 218899) */
    if (s->init_sent && type == NET_PACKET_EXT && n >= 3 && rd_u16(p, 0) == NET_EXT_GAME_STATE) {
        v10_out_init(&o);
        const KrunkerValue arg = KRUNKER_V_NUM(p[2]);
        v10_push(&o, V10_IN_GAME_STATE_CHANGED, &arg, 1);
        finish_out(s, &o, "EXT 0x0901 GAME_STATE");
        return;
    }
    if (s->init_sent && type == NET_PACKET_EXT && n >= 2 && rd_u16(p, 0) == NET_EXT_GAME_ANNOUNCEMENT) {
        v10_out_init(&o);
        v10_map_game_announcement(&o, p + 2, n - 2);
        finish_out(s, &o, "EXT 0x0302 ANNOUNCEMENT");
        return;
    }
    if (s->init_sent && type == NET_PACKET_PING_RESULT) return; /* v10's "pir" handler does nothing */
    if (s->init_sent && type == NET_PACKET_SOCIAL_V7) return;   /* a v7 capability notice: chat goes as CHAT_SEND */
    /* KN's account rights (EXT 0x8001): no v10 message, consumed (as the TS) */
    if (s->init_sent && type == NET_PACKET_EXT && n >= 2 && rd_u16(p, 0) == NET_EXT_KN_ACCOUNT_RIGHTS) return;
    if (s->init_sent && type == NET_PACKET_LEADERBOARD) {
        v10_out_init(&o);
        size_t rows = 0;
        v10_map_leaderboard(&o, p, n, s->mode_index, s->leader_rows, &rows);
        if (!o.stop && !o.oom) {
            s->leader_count = rows;
            s->leader_have = 1;
        }
        finish_out(s, &o, "LEADERBOARD");
        return;
    }
    if (s->init_sent && type == NET_PACKET_LEADER_STATS) {
        const char *stop = v10_check_leader_stats(p, n, s->leader_rows, s->leader_count, s->leader_have);
        if (stop) { stop_at(s, "LEADER_STATS", stop); return; }
        return; /* the rows it repeats were sent as "7" */
    }
    /* AI_ADD (90) is the v3 encoding of a bot; a 1.3+ peer gets AI_ADD_V7 (162) as well: the v3 one is consumed */
    if (s->init_sent && type == NET_PACKET_STATE) {
        V10StateRec rec;
        if (!v10_map_state(p, n, &rec)) { stop_at(s, "STATE", "malformed"); return; }
        if (rec.uid != s->client_id && kbatch_push(s, &rec)) close_session(s, 1011, "out of memory", 0);
        return;
    }
    /* MATCH_VOTE_STATE (181): the match cards' votes as "mv" [vo, mts] (mapMatchVoteState) */
    if (s->init_sent && type == NET_PACKET_MATCH_VOTE_STATE) {
        v10_out_init(&o);
        v10_map_match_vote(&o, p, n, s->socket_id);
        finish_out(s, &o, "MATCH_VOTE_STATE");
        return;
    }
    /* NET 9: a v3 chat line (game, server and trigger lines; the player lines of a v7 peer take CHAT_V7) */
    if (s->init_sent && type == NET_PACKET_CHAT) {
        v10_out_init(&o);
        v10_map_chat(&o, p, n);
        finish_out(s, &o, "CHAT");
        return;
    }
    if (s->init_sent && type == NET_PACKET_CHAT_V7) {
        v10_out_init(&o);
        v10_map_chat_v7(&o, p, n);
        finish_out(s, &o, "CHAT_V7");
        return;
    }
    if (s->init_sent && type == NET_PACKET_DOM_UPDATE) {
        v10_out_init(&o);
        v10_map_dom_update(&o, p, n);
        finish_out(s, &o, "DOM_UPDATE");
        return;
    }
    if (s->init_sent && type == NET_PACKET_AI_SYNC_V7) {
        V10BotChunk chunk;
        if (!v10_parse_bot_sync_chunk(p, n, &chunk)) { stop_at(s, "AI_SYNC_V7", "malformed"); return; }
        /* a sync starts with first; the previous one is complete by then. It is sent when a chunk comes short
         * (under 16 bots) or when the next sync starts: a full last chunk does not say it is the last. */
        if (chunk.first) flush_bot_sync(s);
        if (!s->ai_active) {
            if (!chunk.first) { stop_at(s, "AI_SYNC_V7", "a continuation without its first chunk"); return; }
            s->ai_active = 1;
            s->ai_n = 0;
        }
        if (ai_push(s, &chunk)) { close_session(s, 1011, "out of memory", 0); return; }
        if (chunk.count < 16) flush_bot_sync(s);
        return;
    }
    if (s->init_sent && type == NET_PACKET_AI_ADD_V7) {
        v10_out_init(&o);
        v10_map_bot_add(&o, p, n);
        finish_out(s, &o, "AI_ADD_V7");
        return;
    }
    if (s->init_sent && type == NET_PACKET_MODE_SYNC_V7) {
        v10_out_init(&o);
        v10_map_mode_sync(&o, p, n, s->objective_weapon);
        finish_out(s, &o, "MODE_SYNC_V7");
        return;
    }
    if (s->init_sent && (type == NET_PACKET_AI_ADD || type == NET_PACKET_AI_SYNC)) return;
    /* SKINS come right before the SPAWN they belong to: kept for it, nothing sent */
    if (s->init_sent && type == NET_PACKET_SKINS) {
        V10Skins skins;
        if (!v10_parse_skins(p, n, &skins)) { stop_at(s, "SKINS", "malformed"); return; }
        set_skins(s, &skins);
        return;
    }
    /* SPAWN: a living player's add record (ADD_PLAYERS "0", one player) and, for our own spawn, the start */
    if (s->init_sent && type == NET_PACKET_SPAWN) {
        V10Spawn spawn;
        if (!v10_parse_spawn(p, n, &spawn)) { stop_at(s, "SPAWN", "malformed"); return; }
        if (!spawn.active) return; /* a player who is dead now: the v10 add is for players in play */
        const int at = skins_index(s, spawn.uid);
        v10_out_init(&o);
        const KrunkerValue rec = v10_player_add_record(&o, &spawn, at >= 0 ? &s->skins[at].skins : NULL, at >= 0, s->socket_id);
        const KrunkerValue add_args[2] = {rec, KRUNKER_V_NUM(1)};
        v10_push(&o, V10_IN_ADD_PLAYERS, add_args, 2);
        if (spawn.is_you) {
            KrunkerValue start[8];
            v10_start_game(&spawn, s->game_timer, s->chall_mode, start);
            v10_push(&o, V10_IN_START_GAME, start, 8);
        }
        finish_out(s, &o, "SPAWN");
        return;
    }
    if (s->init_sent && type == NET_PACKET_TIMER) {
        V10Timer timer;
        if (!v10_parse_timer(p, n, &timer)) { stop_at(s, "TIMER", "malformed"); return; }
        s->game_timer = timer.seconds;
        v10_out_init(&o);
        const KrunkerValue args[2] = {KRUNKER_V_NUM(timer.seconds), KRUNKER_V_NUM(timer.end ? 1 : 0)};
        v10_push(&o, V10_IN_UPDATE_TIMER, args, 2);
        finish_out(s, &o, "TIMER");
        return;
    }
    if (s->init_sent && type == NET_PACKET_PLAYER_SOUND) {
        V10PlayerSound sound;
        if (!v10_parse_player_sound(p, n, &sound)) { stop_at(s, "PLAYER_SOUND", "malformed"); return; }
        /* asnd: addPlayerSound(sid, name, {loop, vol, rate}); rsnd: removePlayerSound(sid, name, force) */
        v10_out_init(&o);
        if (!sound.add) {
            const KrunkerValue args[3] = {KRUNKER_V_NUM(sound.uid), KRUNKER_V_STR(sound.name), KRUNKER_V_BOOL(sound.force)};
            v10_push(&o, V10_IN_REMOVE_PLAYER_SOUND, args, 3);
        } else {
            static const char *const keys[3] = {"loop", "vol", "rate"};
            const KrunkerValue opts[3] = {KRUNKER_V_BOOL(sound.loop), KRUNKER_V_NUM(sound.volume), KRUNKER_V_NUM(sound.rate)};
            const KrunkerValue obj = v10_obj(&o, keys, opts, 3);
            const KrunkerValue args[3] = {KRUNKER_V_NUM(sound.uid), KRUNKER_V_STR(sound.name), obj};
            v10_push(&o, V10_IN_ADD_PLAYER_SOUND, args, 3);
        }
        finish_out(s, &o, "PLAYER_SOUND");
        return;
    }
    /* PICKUP: one pickup's state, the pickup half of v10's "inat" (see v10_map_pickup) */
    if (s->init_sent && type == NET_PACKET_PICKUP) {
        v10_out_init(&o);
        v10_map_pickup(&o, p, n);
        finish_out(s, &o, "PICKUP");
        return;
    }
    /* CHEAT: one chat command's flag, on or off: the v10 handler for that label takes the value (v10.js 220244-220264) */
    if (s->init_sent && type == NET_PACKET_CHEAT) {
        uint8_t flag = 0;
        int on = 0;
        if (!v10_parse_cheat(p, n, &flag, &on)) { stop_at(s, "CHEAT", "malformed"); return; }
        const char *label = v10_cheat_label(flag);
        if (!label) {
            char why[48];
            snprintf(why, sizeof why, "flag %d has no v10 label", (int)flag);
            stop_at(s, "CHEAT", why);
            return;
        }
        v10_out_init(&o);
        const KrunkerValue arg = KRUNKER_V_BOOL(on);
        v10_push(&o, label, &arg, 1);
        finish_out(s, &o, "CHEAT");
        return;
    }
    /* after the init, the server repeats the config for a peer whose HELLO came after its INIT: the same text is no change */
    if (s->init_sent && type == NET_PACKET_GAME_CONFIG) {
        uint32_t total = 0, offset = 0;
        const uint8_t *data = NULL;
        size_t dlen = 0;
        if (!v10_parse_game_config_chunk(p, n, &total, &offset, &data, &dlen) || total == 0) { stop_at(s, "GAME_CONFIG", "malformed"); return; }
        if (offset == 0) {
            v10_chunks_free(&s->config_assembly);
            if (v10_chunks_init(&s->config_assembly, total)) { stop_at(s, "GAME_CONFIG", "malformed"); return; }
            s->have_config_assembly = 1;
        }
        if (!s->have_config_assembly || !v10_chunks_add(&s->config_assembly, offset, data, dlen)) { stop_at(s, "GAME_CONFIG", "out of order"); return; }
        if (!v10_chunks_complete(&s->config_assembly)) return;
        const int same = s->have_config_text && s->config_assembly.total == s->config_len && !memcmp(s->config_assembly.buf, s->config_text, s->config_len);
        v10_chunks_free(&s->config_assembly);
        s->have_config_assembly = 0;
        if (same) return;
        { stop_at(s, "GAME_CONFIG", "the config changed after the init: v10 has no message for it"); return; }
    }
    if (s->init_sent && type == NET_PACKET_NETWORK_DATA) {
        V10Network net;
        if (!v10_parse_network_data(p, n, &net)) { stop_at(s, "NETWORK_DATA", "malformed"); return; }
        if (net.snf == s->network.snf && net.tf == s->network.tf && net.sf == s->network.sf) return;
        { stop_at(s, "NETWORK_DATA", "the networking changed after the init: v10 has no message for it"); return; }
    }
    if (s->init_sent) {
        stop_at(s, native_name(type, p, n), "after the init");
        return;
    }

    switch (type) {
    case NET_PACKET_MAP_INFO: {
        V10MapInfo info;
        if (!v10_parse_map_info(p, n, &info) || info.size == 0) { stop_at(s, "MAP_INFO", "malformed or empty map"); return; }
        if (info.size > V10_MAX_MAP_BYTES) { stop_at(s, "MAP_INFO", "malformed or empty map"); return; }
        s->map_info = info;
        s->have_map_info = 1;
        s->have_map_text = 0;
        v10_chunks_free(&s->map_assembly);
        if (v10_chunks_init(&s->map_assembly, info.size)) { stop_at(s, "MAP_INFO", "malformed or empty map"); return; }
        s->have_map_assembly = 1;
        /* the bridge never has the map: it asks by hash, as the native client does */
        uint8_t req[4];
        v10_encode_map_request(info.hash, req);
        write_native(s, NET_PACKET_MAP_REQUEST, req, sizeof req);
        return;
    }
    case NET_PACKET_MAP_DATA: {
        uint32_t hash = 0, offset = 0;
        const uint8_t *data = NULL;
        size_t dlen = 0;
        if (!v10_parse_map_data(p, n, &hash, &offset, &data, &dlen) || !s->have_map_info || !s->have_map_assembly ||
            hash != s->map_info.hash)
            { stop_at(s, "MAP_DATA", "not the map asked for"); return; }
        if (!v10_chunks_add(&s->map_assembly, offset, data, dlen)) { stop_at(s, "MAP_DATA", "out of order"); return; }
        if (!v10_chunks_complete(&s->map_assembly)) return;
        uint8_t *text = s->map_assembly.buf;
        const size_t len = s->map_assembly.total;
        s->map_assembly.buf = NULL;
        s->map_assembly.total = s->map_assembly.end = 0;
        s->have_map_assembly = 0;
        cJSON *probe = v10_config_parse(text, len);
        if (!probe) {
            free(text);
            { stop_at(s, "MAP_DATA", "the map is not JSON"); return; }
        }
        cJSON_Delete(probe);
        free(s->map_text);
        s->map_text = text;
        s->map_len = len;
        s->have_map_text = 1;
        try_emit_init(s);
        return;
    }
    case NET_PACKET_GAME_CONFIG: {
        uint32_t total = 0, offset = 0;
        const uint8_t *data = NULL;
        size_t dlen = 0;
        if (!v10_parse_game_config_chunk(p, n, &total, &offset, &data, &dlen) || total == 0) { stop_at(s, "GAME_CONFIG", "malformed"); return; }
        if (offset == 0) {
            v10_chunks_free(&s->config_assembly);
            if (v10_chunks_init(&s->config_assembly, total)) { stop_at(s, "GAME_CONFIG", "malformed"); return; }
            s->have_config_assembly = 1;
        }
        if (!s->have_config_assembly || !v10_chunks_add(&s->config_assembly, offset, data, dlen)) { stop_at(s, "GAME_CONFIG", "out of order"); return; }
        if (!v10_chunks_complete(&s->config_assembly)) return;
        uint8_t *text = s->config_assembly.buf;
        const size_t len = s->config_assembly.total;
        s->config_assembly.buf = NULL;
        s->config_assembly.total = s->config_assembly.end = 0;
        s->have_config_assembly = 0;
        cJSON *root = v10_config_parse(text, len);
        if (!root) {
            free(text);
            { stop_at(s, "GAME_CONFIG", "the config is not JSON"); return; }
        }
        cJSON_Delete(s->config_root);
        s->config_root = root;
        free(s->config_text);
        s->config_text = text;
        s->config_len = len;
        s->have_config_text = 1;
        try_emit_init(s);
        return;
    }
    case NET_PACKET_NETWORK_DATA: {
        V10Network net;
        if (!v10_parse_network_data(p, n, &net)) { stop_at(s, "NETWORK_DATA", "malformed"); return; }
        s->network = net;
        s->network_received = 1;
        try_emit_init(s);
        return;
    }
    case NET_PACKET_INIT: {
        V10Init init;
        if (!v10_parse_init(p, n, &init)) { stop_at(s, "INIT", "malformed"); return; }
        if (!s->have_map_info) { stop_at(s, "INIT", "no MAP_INFO before it"); return; }
        if (!s->have_map_text && !s->have_map_assembly) { stop_at(s, "INIT", "the map was not requested"); return; }
        s->pending_init = init;
        s->have_pending_init = 1;
        try_emit_init(s);
        return;
    }
    default:
        if (type == NET_PACKET_BILLBOARD || type == NET_PACKET_BILLBOARD_IMAGE) return; /* not translated, sent before the init */
        stop_at(s, native_name(type, p, n), "before the init");
    }
}

/* ---- WebSocket callbacks */

static void on_ws_open(kn_ws *ws, void *ud) {
    V10Session *s = ud;
    hold(s);
    s->ws = ws;
    int ok = 0;
    make_socket_id(s->socket_id, &ok);
    s->started_ms = (int64_t)kn_now_ms();
    peer_address(ws, s->forwarded_addr);
    if (!ok) {
        close_session(s, 1011, "no random source", 0);
        release(s);
        return;
    }
    /* what the v10 client needs before it answers: the socket id, the load request and the instance id */
    send_ws(s, V10_IN_IO_INIT, (KrunkerValue[]){KRUNKER_V_STR(s->socket_id)}, 1);
    send_ws(s, V10_IN_INIT_LOAD, (KrunkerValue[]){KRUNKER_V_NUM(V10_INIT_LOAD_ARG), KRUNKER_V_STR(s->socket_id)}, 2);
    send_ws(s, V10_IN_GAME_INSTANCE_ID, (KrunkerValue[]){KRUNKER_V_STR(s->bridge->lobby_id)}, 1);
    /* the forwarded address first: the server accepts it only from the bridge's loopback peer */
    const size_t alen = strlen(s->forwarded_addr);
    const uint8_t head[V10_NET_HEADER] = {NET_PACKET_FORWARDED, (uint8_t)(alen >> 8), (uint8_t)alen};
    kn_buf_append(&s->out, head, sizeof head);
    kn_buf_append(&s->out, s->forwarded_addr, alen);
    if (connect_native(s)) {
        close_session(s, 1011, "native server unavailable", 0);
    }
    release(s);
}

static void on_ws_message(kn_ws *ws, const void *data, size_t len, int is_binary, void *ud) {
    (void)ws;
    (void)is_binary;
    V10Session *s = ud;
    hold(s);
    KrunkerFrame frame;
    memset(&frame, 0, sizeof frame);
    if (krunker_proto_decode(data, len, 0, &frame) != KRUNKER_PROTO_OK) {
        krunker_frame_free(&frame);
        close_session(s, 1002, "Invalid client packet", 0);
        release(s);
        return;
    }
    TRACE(s, "%.5s <- %s\n", s->socket_id, frame.name);
    handle_client(s, frame.name, frame.args, frame.argc);
    krunker_frame_free(&frame);
    release(s);
}

static void on_ws_close(kn_ws *ws, int code, const char *reason, void *ud) {
    (void)ws;
    (void)code;
    (void)reason;
    V10Session *s = ud;
    TRACE(s, "%.5s client socket closed %d %.*s\n", s->socket_id, code, (int)strlen(reason ? reason : ""), reason ? reason : "");
    s->ws = NULL;
    close_session(s, 1000, "client closed", 0);
    s->dead = 1;
    if (!s->busy) session_free(s);
}

static const kn_ws_callbacks WS_CALLBACKS = {
    .on_open = on_ws_open,
    .on_message = on_ws_message,
    .on_close = on_ws_close,
};

static void route_ws(kn_http_req *req, void *ud) {
    V10Bridge *b = ud;
    V10Session *s = calloc(1, sizeof *s);
    if (!s) {
        kn_http_respond_text(req, 503, "out of memory");
        return;
    }
    s->bridge = b;
    s->fd = -1;
    session_link(s);
    kn_ws_options opts;
    memset(&opts, 0, sizeof opts);
    opts.max_message_bytes = V10_MAX_MESSAGE;
    opts.max_send_buffer = V10_MAX_SEND_BUFFER;
    kn_ws *ws = kn_ws_accept(req, &WS_CALLBACKS, &opts, s);
    if (!ws) {
        /* a bad handshake: knsvc answered it and calls no callback */
        session_unlink(s);
        free(s);
        return;
    }
    s->ws = ws;
}

/* ---- the bridge */

V10Bridge *v10_bridge_new(kn_loop *loop, const V10BridgeConfig *cfg) {
    V10Bridge *b = calloc(1, sizeof *b);
    if (!b) return NULL;
    b->loop = loop;
    snprintf(b->bind_host, sizeof b->bind_host, "%s", cfg->bind_host ? cfg->bind_host : "127.0.0.1");
    b->listen_port = cfg->listen_port;
    snprintf(b->native_host, sizeof b->native_host, "%s", cfg->native_host ? cfg->native_host : "127.0.0.1");
    b->native_port = cfg->native_port > 0 ? cfg->native_port : V10_DEFAULT_NATIVE_PORT;
    snprintf(b->lobby_id, sizeof b->lobby_id, "%s", cfg->lobby_id ? cfg->lobby_id : V10_DEFAULT_LOBBY);
    const char *trace = getenv("KRUNK_NATIVE_V10_TRACE");
    b->trace = trace && !strcmp(trace, "1");
    kn_http_config hc;
    memset(&hc, 0, sizeof hc);
    kn_http_config_defaults(&hc);
    hc.host = b->bind_host;
    hc.port = b->listen_port;
    b->http = kn_http_server_new(loop, &hc);
    if (!b->http || kn_http_route(b->http, "GET", V10_WS_PATH, route_ws, b) != 0) {
        if (b->http) kn_http_server_free(b->http);
        free(b);
        return NULL;
    }
    return b;
}

int v10_bridge_listen(V10Bridge *b) { return kn_http_server_listen(b->http); }

int v10_bridge_port(const V10Bridge *b) { return kn_http_server_port(b->http); }

void v10_bridge_close_all(V10Bridge *b) {
    for (V10Session *s = b->head; s;) {
        V10Session *next = s->next;
        hold(s);
        close_session(s, 1001, "bridge stopping", 0);
        release(s);
        s = next;
    }
}

void v10_bridge_free(V10Bridge *b) {
    while (b->head) {
        V10Session *s = b->head;
        hold(s);
        close_session(s, 1001, "bridge stopping", 1); /* abort: on_close has run by now */
        s->dead = 1;                                   /* freed by release: the close is complete */
        release(s);
    }
    if (b->http) kn_http_server_free(b->http);
    free(b);
}
