// The v10 bridge over real sockets (server/bridge/v10_bridge.c): the port of main() in
// net-bridge/v10Proxy.test.ts. One kn_loop runs a fake KrunkerBox server (TCP, knet frames), the bridge
// (knsvc HTTP + WebSocket on loopback) and a fake v10 client (knsvc's WebSocket client, msgpack through
// the official codec with the 0x2a ahNum trailer). The steps and their assertions are the TS test's, in
// its order; the checks name the TS assertion they port. Prints "v10_bridge_test: N checks, M failed".
#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "ai.h"
#include "net_ext.h" /* NET_PACKET_EXT */
#include "knsvc/buf.h"
#include "knsvc/loop.h"
#include "knsvc/ws.h"
#include "krunker_proto.h"
#include "shared.h"
#include "social_v7.h"
#include "v10_bridge.h"
#include "v10_translate.h"

static int g_checks, g_failures;
#define CHECK(cond, ...) do { \
    g_checks++; \
    if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_failures++; } \
} while (0)

static kn_loop *g_loop;
#define T_EN_LEN 35 // the v10 "en" array

// ---- a value equality for the decoded frames (exact doubles, the TS deepStrictEqual)
static int32_t rd_i32_test(const uint8_t *b, size_t at) {
    return (int32_t)((uint32_t)b[at] | (uint32_t)b[at + 1] << 8 | (uint32_t)b[at + 2] << 16 | (uint32_t)b[at + 3] << 24);
}

static int vequal(const KrunkerValue *a, const KrunkerValue *b) {
    if (!a || !b) return a == b;
    if (a->type != b->type) return 0;
    switch ((KrunkerValueType)a->type) {
    case KRUNKER_NIL: return 1;
    case KRUNKER_BOOL: return (a->u.boolean != 0) == (b->u.boolean != 0);
    case KRUNKER_NUM: return a->u.num == b->u.num;
    case KRUNKER_STR: return a->len == b->len && memcmp(a->u.str, b->u.str, a->len) == 0;
    case KRUNKER_ARRAY:
        if (a->len != b->len) return 0;
        for (uint32_t i = 0; i < a->len; i++)
            if (!vequal(&a->u.items[i], &b->u.items[i])) return 0;
        return 1;
    case KRUNKER_MAP:
        if (a->len != b->len) return 0;
        for (uint32_t i = 0; i < 2 * a->len; i++)
            if (!vequal(&a->u.items[i], &b->u.items[i])) return 0;
        return 1;
    default: return 0;
    }
}
// the value of a string key of a map (NULL when absent)
static const KrunkerValue *map_get(const KrunkerValue *m, const char *key) {
    if (!m || m->type != KRUNKER_MAP) return NULL;
    for (uint32_t i = 0; i < m->len; i++) {
        const KrunkerValue *k = &m->u.items[2 * i];
        if (k->type == KRUNKER_STR && k->len == strlen(key) && !memcmp(k->u.str, key, k->len)) return &m->u.items[2 * i + 1];
    }
    return NULL;
}
static int is_str(const KrunkerValue *v, const char *s) {
    return v && v->type == KRUNKER_STR && v->len == strlen(s) && !memcmp(v->u.str, s, v->len);
}

// ---- the fake KrunkerBox server (the bridge's TCP peer): every frame it gets, and the frames it sends
typedef struct {
    uint8_t type;
    uint8_t *payload;
    size_t len;
} NFrame;
typedef struct {
    int lfd, cfd;
    kn_buf in;
    NFrame *frames;
    size_t n, cap;
    int port;
} Native;
static Native g_native;

static void native_store(Native *nt, uint8_t type, const uint8_t *p, size_t len) {
    if (nt->n == nt->cap) {
        nt->cap = nt->cap ? nt->cap * 2 : 32;
        nt->frames = realloc(nt->frames, nt->cap * sizeof *nt->frames);
    }
    NFrame *f = &nt->frames[nt->n++];
    f->type = type;
    f->len = len;
    f->payload = malloc(len ? len : 1);
    if (len) memcpy(f->payload, p, len);
}

static void native_on_conn(kn_loop *loop, int fd, unsigned events, void *ud) {
    (void)loop;
    (void)events;
    Native *nt = ud;
    uint8_t buf[65536];
    const ssize_t r = recv(fd, buf, sizeof buf, 0);
    if (r <= 0) {
        if (r < 0 && (errno == EAGAIN || errno == EINTR)) return;
        kn_loop_del_fd(g_loop, fd);
        close(fd);
        nt->cfd = -1;
        return;
    }
    kn_buf_append(&nt->in, buf, (size_t)r);
    while (nt->in.len >= 3) {
        const size_t size = ((uint8_t)nt->in.data[1] << 8) | (uint8_t)nt->in.data[2];
        if (nt->in.len < 3 + size) break;
        native_store(nt, (uint8_t)nt->in.data[0], (const uint8_t *)nt->in.data + 3, size);
        kn_buf_consume(&nt->in, 3 + size);
    }
}

static void native_on_listen(kn_loop *loop, int fd, unsigned events, void *ud) {
    (void)events;
    Native *nt = ud;
    const int c = accept(fd, NULL, NULL);
    if (c < 0) return;
    nt->cfd = c; /* blocking writes: the test sends small frames only */
    kn_loop_add_fd(loop, c, KN_EV_READ, native_on_conn, nt);
}

static int native_start(Native *nt) {
    nt->lfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    int one = 1;
    setsockopt(nt->lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (bind(nt->lfd, (struct sockaddr *)&a, sizeof a) != 0 || listen(nt->lfd, 4) != 0) return -1;
    socklen_t al = sizeof a;
    getsockname(nt->lfd, (struct sockaddr *)&a, &al);
    nt->port = ntohs(a.sin_port);
    nt->cfd = -1;
    kn_set_nonblock(nt->lfd);
    return kn_loop_add_fd(g_loop, nt->lfd, KN_EV_READ, native_on_listen, nt);
}

static void native_send(Native *nt, uint8_t type, const uint8_t *p, size_t len) {
    uint8_t head[3] = {type, (uint8_t)(len >> 8), (uint8_t)len};
    size_t sent = 0;
    uint8_t *all = malloc(3 + len);
    memcpy(all, head, 3);
    if (len) memcpy(all + 3, p, len);
    while (sent < 3 + len) {
        const ssize_t w = send(nt->cfd, all + sent, 3 + len - sent, MSG_NOSIGNAL);
        if (w > 0) sent += (size_t)w;
        else if (w < 0 && errno == EINTR) continue;
        else break;
    }
    free(all);
}

static int native_count(uint8_t type) {
    int c = 0;
    for (size_t i = 0; i < g_native.n; i++) c += g_native.frames[i].type == type;
    return c;
}
static NFrame *native_nth(uint8_t type, int nth) {
    int k = 0;
    for (size_t i = 0; i < g_native.n; i++)
        if (g_native.frames[i].type == type && k++ == nth) return &g_native.frames[i];
    return NULL;
}

// ---- the fake v10 client (knsvc ws client): every message the bridge sent, as bytes
typedef struct {
    uint8_t *data;
    size_t len;
} Blob;
static Blob g_blobs[256];
static size_t g_nblobs;
static kn_ws *g_client;
static int g_client_open, g_client_closed, g_client_close_code;

static void client_on_open(kn_ws *ws, void *ud) {
    (void)ws;
    (void)ud;
    g_client_open = 1;
}
static void client_on_message(kn_ws *ws, const void *data, size_t len, int is_binary, void *ud) {
    (void)ws;
    (void)is_binary;
    (void)ud;
    if (g_nblobs < 256) {
        g_blobs[g_nblobs].data = malloc(len ? len : 1);
        memcpy(g_blobs[g_nblobs].data, data, len);
        g_blobs[g_nblobs].len = len;
        g_nblobs++;
    }
}
static void client_on_close(kn_ws *ws, int code, const char *reason, void *ud) {
    (void)ws;
    (void)reason;
    (void)ud;
    g_client = NULL;
    g_client_closed = 1;
    g_client_close_code = code;
}
static const kn_ws_callbacks CLIENT_CB = {.on_open = client_on_open, .on_message = client_on_message, .on_close = client_on_close};

// the nth message with this label, decoded (the frame owns its memory: krunker_frame_free)
static int client_get(const char *label, int nth, KrunkerFrame *out) {
    int k = 0;
    memset(out, 0, sizeof *out);
    for (size_t i = 0; i < g_nblobs; i++) {
        KrunkerFrame fr;
        memset(&fr, 0, sizeof fr);
        if (krunker_proto_decode(g_blobs[i].data, g_blobs[i].len, 0, &fr) != KRUNKER_PROTO_OK) continue;
        if (!strcmp(fr.name, label) && k++ == nth) {
            *out = fr;
            return 1;
        }
        krunker_frame_free(&fr);
    }
    return 0;
}
static int client_count(const char *label) {
    int c = 0;
    for (size_t i = 0; i < g_nblobs; i++) {
        KrunkerFrame fr;
        memset(&fr, 0, sizeof fr);
        if (krunker_proto_decode(g_blobs[i].data, g_blobs[i].len, 0, &fr) == KRUNKER_PROTO_OK && !strcmp(fr.name, label)) c++;
        krunker_frame_free(&fr);
    }
    return c;
}
static void client_send(const char *label, const KrunkerValue *args, uint32_t argc) {
    KrunkerBuf buf = {0};
    if (krunker_proto_encode_ah(0x2a, label, args, argc, &buf) == KRUNKER_PROTO_OK && g_client) kn_ws_send_binary(g_client, buf.data, buf.len);
    krunker_buf_free(&buf);
}

// run the loop until cond holds (or 5 s): the TS waitFor
#define WAIT_FOR(cond) do { \
    const uint64_t t0_ = kn_now_ms(); \
    while (!(cond) && kn_now_ms() - t0_ < 5000) kn_loop_run_once(g_loop, 5); \
    CHECK(cond, "timed out waiting for %s", #cond); \
} while (0)
static void pump(int ms) {
    const uint64_t t0 = kn_now_ms();
    while (kn_now_ms() - t0 < (uint64_t)ms) kn_loop_run_once(g_loop, 5);
}

// the TS test's fixtures for the native side
static uint8_t *mk(size_t n) { return calloc(1, n ? n : 1); }
static void w_i32(uint8_t *b, size_t at, int32_t v) { uint32_t u = (uint32_t)v; for (int i = 0; i < 4; i++) b[at + i] = (uint8_t)(u >> (8 * i)); }
static void w_u16(uint8_t *b, size_t at, uint16_t v) { b[at] = (uint8_t)v; b[at + 1] = (uint8_t)(v >> 8); }
static void w_f32(uint8_t *b, size_t at, float f) { uint32_t u; memcpy(&u, &f, 4); w_i32(b, at, (int32_t)u); }
static void w_f64(uint8_t *b, size_t at, double d) { uint64_t u; memcpy(&u, &d, 8); w_i32(b, at, (int32_t)(uint32_t)u); w_i32(b, at + 4, (int32_t)(uint32_t)(u >> 32)); }
static void w_str(uint8_t *b, size_t at, const char *s) { memcpy(b + at, s, strlen(s)); }

static void mode_state_buf(uint8_t *b, int32_t s0, int32_t s1, int32_t lives, int locked, const char *msg, int32_t objective, int32_t move_lock) {
    memset(b, 0, 387);
    w_i32(b, 0, 0);
    w_i32(b, 4, s0);
    w_i32(b, 8, s1);
    w_str(b, 12, "AT BASE");
    w_str(b, 44, "AT BASE");
    w_i32(b, 76, objective);
    w_i32(b, 80, -1);
    b[84] = (uint8_t)move_lock;
    b[85] = (uint8_t)locked;
    w_i32(b, 86, lives);
    b[90] = 0;
    w_str(b, 291, msg);
}
static void object_state_buf(uint8_t *b, int32_t uid, uint8_t kind, int32_t value) { w_i32(b, 0, uid); b[4] = kind; w_i32(b, 5, value); }

static void spawn_buf(uint8_t *b, int32_t uid, int is_you, int active, int32_t class_index, int32_t team, float health,
                      float px, float py, float pz, float dir0, float dir1, const char *name) {
    memset(b, 0, 150);
    w_i32(b, 0, uid);
    b[4] = (uint8_t)is_you;
    b[5] = (uint8_t)active;
    w_i32(b, 6, class_index);
    w_i32(b, 10, team);
    w_f32(b, 14, health);
    w_i32(b, 18, 100);
    w_f32(b, 22, px); w_f32(b, 26, py); w_f32(b, 30, pz);
    w_f32(b, 34, dir0); w_f32(b, 38, dir1);
    w_str(b, 42, name);
    w_i32(b, 74, -1);
    w_i32(b, 78, -1); w_i32(b, 82, -1); w_i32(b, 86, -1); w_i32(b, 90, -1); w_i32(b, 94, -1); w_i32(b, 98, -1); w_i32(b, 102, -1);
    w_i32(b, 106, -1);
    w_f32(b, 110, 1);
    for (int i = 114; i <= 146; i += 4) w_i32(b, (size_t)i, -1);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    g_loop = kn_loop_new();
    CHECK(g_loop != NULL, "a loop");
    CHECK(native_start(&g_native) == 0, "the fake native server listens");

    // the bridge in v10 mode, on loopback (the TS: v10Proxy.attach on an http server)
    V10BridgeConfig cfg = {.bind_host = "127.0.0.1", .listen_port = 0, .native_host = "127.0.0.1", .native_port = g_native.port, .lobby_id = NULL};
    V10Bridge *bridge = v10_bridge_new(g_loop, &cfg);
    CHECK(bridge != NULL && v10_bridge_listen(bridge) == 0, "the bridge listens");
    const int bport = v10_bridge_port(bridge);

    // the fake v10 client connects
    char url[128];
    snprintf(url, sizeof url, "ws://127.0.0.1:%d/ws?gameId=test", bport);
    kn_ws_options copts;
    memset(&copts, 0, sizeof copts);
    g_client = kn_ws_connect(g_loop, url, &CLIENT_CB, &copts, NULL);
    CHECK(g_client != NULL, "the client starts connecting");
    WAIT_FOR(g_client_open);

    // 1. the connect packets the v10 client needs (io-init, load, inst-id), each with a trailer
    WAIT_FOR(client_count("io-init") == 1);
    KrunkerFrame io, ld, inst;
    CHECK(client_get("io-init", 0, &io) && io.argc == 1 && io.args[0].type == KRUNKER_STR && io.args[0].len == 12, "io-init carries a 12-char socket id");
    char socket_id[13] = "";
    if (io.argc) memcpy(socket_id, io.args[0].u.str, io.args[0].len);
    WAIT_FOR(client_count("load") == 1);
    CHECK(client_get("load", 0, &ld) && ld.argc == 2 && vequal(&ld.args[0], &KRUNKER_V_NUM(20000)) && is_str(&ld.args[1], socket_id),
          "load is [20000, socket id]");
    WAIT_FOR(client_count("inst-id") == 1);
    CHECK(client_get("inst-id", 0, &inst) && inst.argc == 1 && is_str(&inst.args[0], "KN:local"), "inst-id is KN:local");
    krunker_frame_free(&io);
    krunker_frame_free(&ld);
    krunker_frame_free(&inst);
    CHECK(g_client && g_client_open, "the client is open");

    // 2. the bridge opens the native socket and writes the forwarded address first
    WAIT_FOR(g_native.cfd >= 0 && native_count(NET_PACKET_FORWARDED) == 1);
    NFrame *fwd = native_nth(NET_PACKET_FORWARDED, 0);
    CHECK(fwd != NULL && fwd->len == 9 && !memcmp(fwd->payload, "127.0.0.1", 9), "the forwarded frame is the loopback peer (9 bytes)");
    CHECK(g_native.n == 1, "nothing else until the client speaks");

    // 3. LOAD -> HELLO: protocol 1.4.2 (the TS HELLO), the Krunker v10 ruleset bit
    client_send("load", NULL, 0);
    WAIT_FOR(native_count(NET_PACKET_HELLO) == 1);
    NFrame *hello = native_nth(NET_PACKET_HELLO, 0);
    CHECK(hello && hello->len == 4 && hello->payload[0] == 1 && hello->payload[1] == 4 && hello->payload[2] == 5 && hello->payload[3] == 4,
          "HELLO is [1, 4, 5, 4] (NET_PROTOCOL_PATCH 5)");
    client_send("load", NULL, 0); /* a second LOAD sends no second HELLO */
    pump(100);
    CHECK(native_count(NET_PACKET_HELLO) == 1, "a second LOAD sends no second HELLO");

    // 4. the map, the config and the network data come before the INIT: MAP_REQUEST, the held INIT, one init
    char pad[701];
    memset(pad, 'm', 700);
    pad[700] = 0;
    char map_json_buf[900];
    snprintf(map_json_buf, sizeof map_json_buf, "{\"name\":\"Test Map\",\"objects\":[],\"padding\":\"%s\"}", pad);
    const char *map_json_text = map_json_buf;
    const size_t map_len = strlen(map_json_text);
    CHECK(map_len > 496, "the map JSON spans two MAP_DATA chunks");
    const uint32_t map_hash = 0xc0ffee;
    uint8_t *map_info = mk(44);
    w_i32(map_info, 0, 3);
    w_i32(map_info, 4, (int32_t)map_hash);
    w_i32(map_info, 8, (int32_t)map_len);
    w_str(map_info, 12, "Test Map");
    const char config_text[] = "{\"host\":1,\"settings\":{\"hitBoxPad\":1,\"timeLimit\":5}}";
    const size_t config_len = strlen(config_text);
    const size_t cut = 20;
    uint8_t *network = mk(10);
    w_u16(network, 0, 10); w_u16(network, 2, 30); w_u16(network, 4, 1);
    uint8_t init[18];
    memset(init, 0, sizeof init);
    init[0] = 1; init[1] = 4; init[2] = 2;
    w_i32(init, 3, 3); w_i32(init, 7, 0); w_u16(init, 11, 30); w_i32(init, 13, 1); init[17] = 3;

    native_send(&g_native, NET_PACKET_MAP_INFO, map_info, 44);
    native_send(&g_native, NET_PACKET_INIT, init, sizeof init);
    uint8_t *chunk1 = mk(8 + cut);
    w_i32(chunk1, 0, (int32_t)config_len); w_i32(chunk1, 4, 0); memcpy(chunk1 + 8, config_text, cut);
    native_send(&g_native, NET_PACKET_GAME_CONFIG, chunk1, 8 + cut);
    uint8_t *chunk2 = mk(8 + config_len - cut);
    w_i32(chunk2, 0, (int32_t)config_len); w_i32(chunk2, 4, (int32_t)cut); memcpy(chunk2 + 8, config_text + cut, config_len - cut);
    native_send(&g_native, NET_PACKET_GAME_CONFIG, chunk2, 8 + config_len - cut);
    native_send(&g_native, NET_PACKET_NETWORK_DATA, network, 10);
    const uint8_t ranked_off[3] = {0x50, 0x80, 0};
    native_send(&g_native, NET_PACKET_EXT, ranked_off, 3);
    WAIT_FOR(native_count(NET_PACKET_MAP_REQUEST) == 1);
    NFrame *req = native_nth(NET_PACKET_MAP_REQUEST, 0);
    CHECK(req && req->len == 4 && req->payload[0] == 0xee && req->payload[1] == 0xff && req->payload[2] == 0xc0 && req->payload[3] == 0,
          "MAP_REQUEST carries the map hash");
    pump(100);
    CHECK(client_count("init") == 0, "the init is held until the map is whole");

    // the map JSON in two chunks (MAP_DATA: hash, offset, length, bytes)
    const size_t half = 496;
    uint8_t *mc1 = mk(10 + half);
    w_i32(mc1, 0, (int32_t)map_hash); w_i32(mc1, 4, 0); w_u16(mc1, 8, (uint16_t)half); memcpy(mc1 + 10, map_json_text, half);
    native_send(&g_native, NET_PACKET_MAP_DATA, mc1, 10 + half);
    const size_t rest = map_len - half;
    uint8_t *mc2 = mk(10 + rest);
    w_i32(mc2, 0, (int32_t)map_hash); w_i32(mc2, 4, (int32_t)half); w_u16(mc2, 8, (uint16_t)rest); memcpy(mc2 + 10, map_json_text + half, rest);
    native_send(&g_native, NET_PACKET_MAP_DATA, mc2, 10 + rest);
    WAIT_FOR(client_count("init") == 1);
    CHECK(client_count("init") == 1, "one init");
    KrunkerFrame ini;
    CHECK(client_get("init", 0, &ini), "the init frame");
    if (ini.argc == 20) {
        const KrunkerValue *a = ini.args;
        CHECK(vequal(&a[0], &KRUNKER_V_NUM(3)), "init arg 0 is the map index");
        CHECK(vequal(&a[1], &KRUNKER_V_NUM(0)), "init arg 1 is the mode");
        // the TS: args[6] deepStrictEqual settings, the GAME_CONFIG's settings object itself
        CHECK(a[6].type == KRUNKER_MAP && a[6].len == 2 && vequal(map_get(&a[6], "hitBoxPad"), &KRUNKER_V_NUM(1)) &&
              vequal(map_get(&a[6], "timeLimit"), &KRUNKER_V_NUM(5)), "init arg 6: the config's settings {hitBoxPad 1, timeLimit 5}");
        CHECK(a[9].type == KRUNKER_MAP && is_str(map_get(&a[9], "data"), map_json_text), "init arg 9: the map JSON text");
        CHECK(vequal(map_get(&a[12], "snf"), &KRUNKER_V_NUM(10)) && vequal(map_get(&a[12], "tf"), &KRUNKER_V_NUM(30)) &&
              vequal(map_get(&a[12], "sf"), &KRUNKER_V_NUM(1)), "init arg 12: networkData {snf 10, tf 30, sf 1}");
        CHECK(vequal(&a[13], &KRUNKER_V_BOOL(1)), "init arg 13 is true");
        CHECK(vequal(&a[16], &KRUNKER_V_BOOL(0)), "init arg 16 is isRanked");
        CHECK(a[11].type == KRUNKER_MAP && a[11].len == 1 && vequal(map_get(&a[11], "gor"), &KRUNKER_V_NUM(1)), "init arg 11 is the sync data {gor: 1}");
    } else {
        CHECK(0, "the init has 20 arguments");
    }
    krunker_frame_free(&ini);

    // a reset-only global value before any gv clears nothing the client holds: no message, no stop
    uint8_t reset[80];
    memset(reset, 0, sizeof reset);
    reset[0] = 0x01;
    native_send(&g_native, NET_PACKET_GLOBAL_VALUE, reset, 80);
    // a global value after the INIT is a gv to the client (flags 0, the key, a double)
    uint8_t gvb[80];
    memset(gvb, 0, sizeof gvb);
    memcpy(gvb + 1, "score_limit", 11);
    w_f64(gvb, 72, 2.5);
    native_send(&g_native, NET_PACKET_GLOBAL_VALUE, gvb, 80);
    WAIT_FOR(client_count("gv") == 1);
    KrunkerFrame gv;
    CHECK(client_get("gv", 0, &gv) && gv.argc == 2 && is_str(&gv.args[0], "score_limit") && vequal(&gv.args[1], &KRUNKER_V_NUM(2.5)),
          "gv is [score_limit, 2.5]");
    krunker_frame_free(&gv);

    // a player sound (add) after the INIT is an asnd: uid, name, {loop, vol, rate}
    uint8_t snd[39];
    memset(snd, 0, sizeof snd);
    w_i32(snd, 0, 2); snd[4] = 1; snd[6] = 1; w_f32(snd, 7, 0.5f); w_f32(snd, 11, 1.25f); w_str(snd, 15, "footstep");
    native_send(&g_native, NET_PACKET_PLAYER_SOUND, snd, 39);
    WAIT_FOR(client_count("asnd") == 1);
    KrunkerFrame asnd;
    CHECK(client_get("asnd", 0, &asnd) && asnd.argc == 3 && vequal(&asnd.args[0], &KRUNKER_V_NUM(2)) && is_str(&asnd.args[1], "footstep") &&
          vequal(map_get(&asnd.args[2], "loop"), &KRUNKER_V_BOOL(1)) && vequal(map_get(&asnd.args[2], "vol"), &KRUNKER_V_NUM(0.5)) &&
          vequal(map_get(&asnd.args[2], "rate"), &KRUNKER_V_NUM(1.25)), "asnd is [2, footstep, {loop, vol, rate}]");
    krunker_frame_free(&asnd);

    // a player sound (remove) is an rsnd: uid, name, force
    snd[4] = 0;
    native_send(&g_native, NET_PACKET_PLAYER_SOUND, snd, 39);
    WAIT_FOR(client_count("rsnd") == 1);
    KrunkerFrame rsnd;
    CHECK(client_get("rsnd", 0, &rsnd) && rsnd.argc == 3 && vequal(&rsnd.args[0], &KRUNKER_V_NUM(2)) && is_str(&rsnd.args[1], "footstep") &&
          vequal(&rsnd.args[2], &KRUNKER_V_BOOL(0)), "rsnd is [2, footstep, false]");
    krunker_frame_free(&rsnd);

    // the round timer is a t [seconds, end]
    uint8_t timer[5];
    w_i32(timer, 0, 300);
    timer[4] = 0;
    native_send(&g_native, NET_PACKET_TIMER, timer, 5);
    WAIT_FOR(client_count("t") == 1);
    KrunkerFrame t;
    CHECK(client_get("t", 0, &t) && t.argc == 2 && vequal(&t.args[0], &KRUNKER_V_NUM(300)) && vequal(&t.args[1], &KRUNKER_V_NUM(0)), "t is [300, 0]");
    krunker_frame_free(&t);

    // the joiner's mode snapshot: team scores, lives and the lock, then the object list
    uint8_t *ms = mk(387);
    mode_state_buf(ms, 5, 7, 3, 0, "", -1, 0);
    native_send(&g_native, NET_PACKET_MODE_STATE, ms, 387);
    WAIT_FOR(client_count("ts") >= 2);
    KrunkerFrame lv;
    WAIT_FOR(client_count("lv") == 1);
    CHECK(client_get("lv", 0, &lv) && lv.argc == 1 && vequal(&lv.args[0], &KRUNKER_V_NUM(3)), "lv is [3]");
    krunker_frame_free(&lv);
    KrunkerFrame ts0, ts1;
    CHECK(client_get("ts", 0, &ts0) && client_get("ts", 1, &ts1) && vequal(&ts0.args[0], &KRUNKER_V_NUM(1)) && vequal(&ts0.args[1], &KRUNKER_V_NUM(5)) &&
          vequal(&ts1.args[0], &KRUNKER_V_NUM(2)) && vequal(&ts1.args[1], &KRUNKER_V_NUM(7)), "ts are [1, 5] and [2, 7]");
    krunker_frame_free(&ts0);
    krunker_frame_free(&ts1);
    free(ms);
    uint8_t ob[9];
    object_state_buf(ob, 4, 0, 0);
    native_send(&g_native, NET_PACKET_OBJECT_STATE, ob, 9); /* do 4 */
    object_state_buf(ob, 9, 2, 1);
    native_send(&g_native, NET_PACKET_OBJECT_STATE, ob, 9); /* gte 9 open */
    WAIT_FOR(client_count("gte") == 1);
    KrunkerFrame dof, gte;
    CHECK(client_get("do", 0, &dof) && dof.argc == 1 && vequal(&dof.args[0], &KRUNKER_V_NUM(4)), "do [4]");
    CHECK(client_get("gte", 0, &gte) && gte.argc == 2 && vequal(&gte.args[0], &KRUNKER_V_NUM(9)) && vequal(&gte.args[1], &KRUNKER_V_BOOL(1)), "gte [9, true]");
    krunker_frame_free(&dof);
    krunker_frame_free(&gte);

    // the ping result answers the bridge's PONG: v10's socket-ping result handler is empty, so it is consumed
    const uint8_t pir[4] = {0x40, 0, 0, 0};
    native_send(&g_native, NET_PACKET_PING_RESULT, pir, 4);
    pump(50);
    CHECK(v10_bridge_untranslated("PING_RESULT") == 0, "PING_RESULT is consumed");

    // the v7 capability notice (249): the bridge sends chat as CHAT_SEND, so it needs nothing
    const uint8_t social[1] = {1};
    native_send(&g_native, NET_PACKET_SOCIAL_V7, social, 1);
    pump(50);
    CHECK(v10_bridge_untranslated("SOCIAL_V7") == 0, "SOCIAL_V7 is consumed");

    // the server repeats the config and the networking after the init: the same values are no change
    native_send(&g_native, NET_PACKET_GAME_CONFIG, chunk1, 8 + cut);
    native_send(&g_native, NET_PACKET_GAME_CONFIG, chunk2, 8 + config_len - cut);
    native_send(&g_native, NET_PACKET_NETWORK_DATA, network, 10);
    pump(50);
    CHECK(v10_bridge_untranslated("GAME_CONFIG") == 0, "a repeated config is no change");

    // the match phase after the init: gsc [3] (KN GameState 3 IN_PROGRESS)
    const uint8_t gsc[3] = {0x01, 0x09, 0x03};
    native_send(&g_native, NET_PACKET_EXT, gsc, 3);
    WAIT_FOR(client_count("gsc") == 1);
    KrunkerFrame gs;
    CHECK(client_get("gsc", 0, &gs) && gs.argc == 1 && vequal(&gs.args[0], &KRUNKER_V_NUM(3)), "gsc is [3]");
    krunker_frame_free(&gs);

    // the game announcement card (EXT 0x0302): gameAnnouncement {message, actions}
    uint8_t *card = mk(2 + 321);
    card[0] = 0x02; card[1] = 0x03;
    w_str(card, 2, "Welcome to the test");
    native_send(&g_native, NET_PACKET_EXT, card, 2 + 321);
    WAIT_FOR(client_count("gameAnnouncement") == 1);
    KrunkerFrame ann;
    CHECK(client_get("gameAnnouncement", 0, &ann) && ann.argc == 1 && is_str(map_get(&ann.args[0], "message"), "Welcome to the test") &&
          map_get(&ann.args[0], "actions") && map_get(&ann.args[0], "actions")->len == 0, "gameAnnouncement {message, actions: []}");
    krunker_frame_free(&ann);
    free(card);

    // SKINS, then the SPAWN of another player and of ours: the add record (ADD_PLAYERS "0") and our start
    uint8_t skins[16];
    w_i32(skins, 0, 2); w_i32(skins, 4, 7); w_i32(skins, 8, 8); w_i32(skins, 12, 9);
    native_send(&g_native, NET_PACKET_SKINS, skins, 16);
    uint8_t sp[150];
    spawn_buf(sp, 2, 0, 1, 3, 1, 90, 1, 2, 3, 0, 0, "other");
    native_send(&g_native, NET_PACKET_SPAWN, sp, 150);
    // another player's add waits for our start: the client's init (it fetches the map) runs before it, and a player in its
    // list before that breaks the init (the join's TypeErrors). It goes out after our START, below.
    pump(100);
    CHECK(client_count("0") == 0, "another player's add is held until our start");
    uint8_t sp_me[150];
    spawn_buf(sp_me, 1, 1, 1, 4, 0, 100, 1, 2, 3, 0, 0, "me");
    native_send(&g_native, NET_PACKET_SPAWN, sp_me, 150);
    WAIT_FOR(client_count("start") == 1);
    KrunkerFrame addm, st;
    int found_mine = 0;
    for (int k = 0; k < client_count("0") && !found_mine; k++) {
        KrunkerFrame f;
        if (client_get("0", k, &f) && f.argc == 2 && f.args[0].type == KRUNKER_ARRAY && vequal(&f.args[0].u.items[1], &KRUNKER_V_NUM(1))) {
            addm = f;
            found_mine = 1;
        } else {
            krunker_frame_free(&f);
        }
    }
    CHECK(found_mine && is_str(&addm.args[0].u.items[0], socket_id), "our add record's id is the io-init socket id");
    if (found_mine) krunker_frame_free(&addm);
    CHECK(client_get("start", 0, &st) && st.argc == 8 && vequal(&st.args[0], &KRUNKER_V_NUM(300)) && vequal(&st.args[7], &KRUNKER_V_NUM(4)) &&
          st.args[6].type == KRUNKER_NIL && vequal(&st.args[5], &KRUNKER_V_NUM(1)), "start is [300, 0, 0, 0, 0, 1, null, 4]");
    krunker_frame_free(&st);
    // the held add goes out after our start: the record of uid 2 with its SKINS (field 12), ADD_PLAYERS takes 1
    WAIT_FOR(client_count("0") >= 2);
    KrunkerFrame addo;
    int found_other = 0;
    for (int k = 0; k < client_count("0") && !found_other; k++) {
        KrunkerFrame f;
        if (client_get("0", k, &f) && f.argc == 2 && f.args[0].type == KRUNKER_ARRAY && f.args[0].len == 53 && vequal(&f.args[0].u.items[1], &KRUNKER_V_NUM(2))) {
            addo = f;
            found_other = 1;
        } else {
            krunker_frame_free(&f);
        }
    }
    CHECK(found_other, "ADD_PLAYERS for the other player, after our start");
    if (found_other) {
        const KrunkerValue *rec = &addo.args[0];
        CHECK(rec->len == 53 && rec->u.items[12].type == KRUNKER_ARRAY && vequal(&rec->u.items[12].u.items[0], &KRUNKER_V_NUM(7)) &&
              vequal(&rec->u.items[12].u.items[1], &KRUNKER_V_NUM(8)), "the add record has the SKINS of uid 2 at field 12");
        CHECK(addo.args[1].type == KRUNKER_NUM && addo.args[1].u.num == 1, "ADD_PLAYERS takes 1");
        krunker_frame_free(&addo);
    }

    // the leaderboard (14, free for all) and its LEADER_STATS repeat (consumed)
    uint8_t board[257];
    memset(board, 0, sizeof board);
    board[0] = 1;
    w_i32(board, 1, 1); w_i32(board, 5, 12); w_i32(board, 9, 2); w_i32(board, 13, 3);
    native_send(&g_native, NET_PACKET_LEADERBOARD, board, 257);
    WAIT_FOR(client_count("7") == 1);
    KrunkerFrame rows;
    CHECK(client_get("7", 0, &rows) && rows.argc == 1 && rows.args[0].type == KRUNKER_ARRAY && rows.args[0].len == 4 &&
          vequal(&rows.args[0].u.items[1], &KRUNKER_V_NUM(12)), "the leaderboard rows [1, 12, 2, 3]");
    krunker_frame_free(&rows);
    uint8_t stats[322];
    memset(stats, 0, sizeof stats);
    stats[0] = 1; stats[1] = 3;
    w_i32(stats, 2, 1);
    w_i32(stats, 66, 12); w_i32(stats, 70, 2); w_i32(stats, 74, 3);
    native_send(&g_native, NET_PACKET_LEADER_STATS, stats, 322);
    pump(50);
    CHECK(v10_bridge_untranslated("LEADER_STATS") == 0, "the LEADER_STATS repeat is consumed");

    // the bot (AI_ADD 90) is consumed; the sync (91) with no entries too
    uint8_t bot3[101];
    memset(bot3, 0, sizeof bot3);
    w_i32(bot3, 0, 5);
    w_str(bot3, 8, "Bot");
    native_send(&g_native, NET_PACKET_AI_ADD, bot3, 101);
    pump(50);
    CHECK(client_count("aai") == 0 && v10_bridge_untranslated("AI_ADD") == 0, "the v3 bot add is consumed");
    const uint8_t ai_empty[2] = {1, 0};
    native_send(&g_native, NET_PACKET_AI_SYNC, ai_empty, 2);
    pump(50);
    CHECK(client_count("ai") == 0, "the v3 sync is consumed");

    // the joiner's mode sync (149): the scored teams as ts, no rounds, the lockout and the objective
    uint8_t msync[65];
    memset(msync, 0, sizeof msync);
    w_i32(msync, 4, 5); w_i32(msync, 8, 7);
    w_i32(msync, 57, -1);
    native_send(&g_native, NET_PACKET_MODE_SYNC_V7, msync, 65);
    WAIT_FOR(client_count("ts") >= 4);
    KrunkerFrame ts2, ts3;
    CHECK(client_get("ts", 2, &ts2) && client_get("ts", 3, &ts3) && vequal(&ts2.args[0], &KRUNKER_V_NUM(1)) && vequal(&ts2.args[1], &KRUNKER_V_NUM(5)) &&
          vequal(&ts3.args[0], &KRUNKER_V_NUM(2)) && vequal(&ts3.args[1], &KRUNKER_V_NUM(7)), "the mode sync's ts [1, 5] and [2, 7]");
    krunker_frame_free(&ts2);
    krunker_frame_free(&ts3);

    // a bot (162) reaches the client as aai [sid, index, name, x, y, z, dat] with no data: dat 0
    uint8_t bot7[237];
    memset(bot7, 0, sizeof bot7);
    w_i32(bot7, 0, 5); w_i32(bot7, 4, 0); w_str(bot7, 8, "Bot");
    bot7[60] = 0;
    native_send(&g_native, NET_PACKET_AI_ADD_V7, bot7, 237);
    WAIT_FOR(client_count("aai") == 1);
    KrunkerFrame aai;
    CHECK(client_get("aai", 0, &aai) && aai.argc == 1 && aai.args[0].type == KRUNKER_ARRAY && aai.args[0].len == 7 &&
          vequal(&aai.args[0].u.items[0], &KRUNKER_V_NUM(5)) && is_str(&aai.args[0].u.items[2], "Bot") &&
          vequal(&aai.args[0].u.items[6], &KRUNKER_V_NUM(0)), "aai [[5, 0, Bot, 0, 0, 0, 0]] (one record argument)");
    krunker_frame_free(&aai);

    // the domination zones (142): one uct list; the zone's map object id is its uid
    uint8_t dom[497];
    memset(dom, 0, sizeof dom);
    dom[0] = 1;
    w_i32((uint8_t *)dom, 1, 0);
    w_i32(dom, 5, 1);
    w_f32(dom, 9, 0);
    w_i32(dom, 13, 0);
    w_f32(dom, 24, 1);
    w_i32(dom, 28, 42);
    native_send(&g_native, NET_PACKET_DOM_UPDATE, dom, 497);
    WAIT_FOR(client_count("uct") == 1);
    KrunkerFrame uct;
    CHECK(client_get("uct", 0, &uct) && uct.argc == 1 && uct.args[0].type == KRUNKER_ARRAY && uct.args[0].len == 6 &&
          vequal(&uct.args[0].u.items[0], &KRUNKER_V_NUM(42)) && vequal(&uct.args[0].u.items[1], &KRUNKER_V_NUM(1)) &&
          uct.args[0].u.items[3].type == KRUNKER_NIL && uct.args[0].u.items[4].len == 0 && vequal(&uct.args[0].u.items[5], &KRUNKER_V_NUM(1)),
          "uct [[42, 1, 0, null, [], 1]]");
    krunker_frame_free(&uct);

    // the bot positions (161): one short chunk is one ai list with the bot in it
    uint8_t aisync[2 + 31];
    memset(aisync, 0, sizeof aisync);
    aisync[0] = 1; aisync[1] = 1;
    w_i32(aisync, 2, 5);
    w_f32(aisync, 6, 1); w_f32(aisync, 10, 2); w_f32(aisync, 14, 3);
    w_i32(aisync, 26, 88);
    native_send(&g_native, NET_PACKET_AI_SYNC_V7, aisync, sizeof aisync);
    WAIT_FOR(client_count("ai") == 1);
    KrunkerFrame ai;
    CHECK(client_get("ai", 0, &ai) && ai.argc == 1 && ai.args[0].type == KRUNKER_ARRAY && ai.args[0].len == 9 &&
          vequal(&ai.args[0].u.items[0], &KRUNKER_V_NUM(5)) && vequal(&ai.args[0].u.items[6], &KRUNKER_V_NUM(88)) &&
          vequal(&ai.args[0].u.items[7], &KRUNKER_V_BOOL(0)), "ai [[5, 1, 2, 3, 0, 0, 88, false, 0]]");
    krunker_frame_free(&ai);

    // a chat line for a v7 peer (245): ch (channel, name, text, kind, _, _, account, _), the text escaped
    uint8_t chat[330];
    memset(chat, 0, sizeof chat);
    chat[0] = 0; w_i32(chat, 1, 7); w_i32(chat, 5, 3); chat[9] = 0;
    w_str(chat, 10, "Ada");
    w_str(chat, 42, "hi <b>");
    native_send(&g_native, NET_PACKET_CHAT_V7, chat, 330);
    WAIT_FOR(client_count("ch") == 1);
    KrunkerFrame chf;
    CHECK(client_get("ch", 0, &chf) && chf.argc == 8 && vequal(&chf.args[0], &KRUNKER_V_NUM(0)) && is_str(&chf.args[1], "Ada") &&
          is_str(&chf.args[2], "hi &lt;b&gt;") && vequal(&chf.args[3], &KRUNKER_V_NUM(0)) && chf.args[4].type == KRUNKER_NIL &&
          chf.args[5].type == KRUNKER_NIL && vequal(&chf.args[6], &KRUNKER_V_NUM(7)) && chf.args[7].type == KRUNKER_NIL, "ch [0, Ada, escaped, 0, null, null, 7, null]");
    krunker_frame_free(&chf);

    // the per-tick states: ours is not sent back; another player's is a k with its 13-field record
    uint8_t st1[79], st9[79];
    memset(st1, 0, 79); w_i32(st1, 0, 1); w_f32(st1, 13, 9); w_f32(st1, 17, 2); w_f32(st1, 21, 3); w_f32(st1, 37, 0.5f); w_f32(st1, 41, 1);
    w_f32(st1, 57, 0.25f); st1[61] = 1; w_f32(st1, 53, 0.6f); w_i32(st1, 9, 2); w_f32(st1, 49, 1); st1[62] = 0; w_f32(st1, 75, 42.4f);
    memset(st9, 0, 79); w_i32(st9, 0, 9); w_f32(st9, 13, 1.256f); w_f32(st9, 17, 2); w_f32(st9, 21, 3); w_f32(st9, 37, 0.5f); w_f32(st9, 41, 1);
    w_f32(st9, 57, 0.25f); st9[61] = 1; w_f32(st9, 53, 0.6f); w_i32(st9, 9, 2); w_f32(st9, 49, 1); st9[62] = 0; w_f32(st9, 75, 42.4f);
    native_send(&g_native, NET_PACKET_STATE, st1, 79);
    native_send(&g_native, NET_PACKET_STATE, st9, 79);
    WAIT_FOR(client_count("k") >= 1);
    KrunkerFrame kf;
    CHECK(client_get("k", 0, &kf) && kf.argc == 3 && kf.args[0].type == KRUNKER_ARRAY && kf.args[0].len == 1, "k with one record");
    if (kf.argc == 3 && kf.args[0].len == 1) {
        const KrunkerValue *r = &kf.args[0].u.items[0];
        const KrunkerValue want_rec[13] = {KRUNKER_V_NUM(9), KRUNKER_V_NUM(1.26), KRUNKER_V_NUM(2), KRUNKER_V_NUM(3), KRUNKER_V_NUM(57),
                                           KRUNKER_V_NUM(29), KRUNKER_V_NUM(0.25), KRUNKER_V_NUM(1), KRUNKER_V_NUM(1), KRUNKER_V_NUM(2),
                                           KRUNKER_V_NUM(1), KRUNKER_V_NUM(0), KRUNKER_V_NUM(42)};
        int same = r->len == 13;
        for (uint32_t i = 0; same && i < 13; i++) same = vequal(&r->u.items[i], &want_rec[i]);
        CHECK(same, "the k record is the TS 13-field record");
        CHECK(vequal(&kf.args[1], &KRUNKER_V_NUM(10)), "k sendRate 10");
    }
    krunker_frame_free(&kf);

    // NET 9 (the v3 chat, game and server lines): a server line (kind 2) and a player line (kind 0), channel -1 (all)
    uint8_t c9[325];
    memset(c9, 0, sizeof c9);
    c9[4] = 2;
    w_str(c9, 37, "Round 2 starts");
    native_send(&g_native, NET_PACKET_CHAT, c9, 325);
    WAIT_FOR(client_count("ch") == 2);
    KrunkerFrame ch9;
    CHECK(client_get("ch", 1, &ch9) && ch9.argc == 8 && vequal(&ch9.args[0], &KRUNKER_V_NUM(-1)) && ch9.args[1].type == KRUNKER_NIL &&
          is_str(&ch9.args[2], "Round 2 starts") && vequal(&ch9.args[3], &KRUNKER_V_NUM(2)) && ch9.args[6].type == KRUNKER_NIL,
          "NET 9 server line: ch [-1, null, text, 2, null, null, null, null]");
    krunker_frame_free(&ch9);
    memset(c9, 0, sizeof c9);
    w_i32(c9, 0, 3);
    c9[4] = 0;
    w_str(c9, 5, "Ada");
    w_str(c9, 37, "hi <b>");
    native_send(&g_native, NET_PACKET_CHAT, c9, 325);
    WAIT_FOR(client_count("ch") == 3);
    KrunkerFrame ch9p;
    CHECK(client_get("ch", 2, &ch9p) && ch9p.argc == 8 && vequal(&ch9p.args[0], &KRUNKER_V_NUM(-1)) && is_str(&ch9p.args[1], "Ada") &&
          is_str(&ch9p.args[2], "hi &lt;b&gt;") && ch9p.args[6].type == KRUNKER_NIL, "NET 9 player line: ch [-1, Ada, escaped, 0, ..., null]");
    krunker_frame_free(&ch9p);

    // MATCH_VOTE_STATE (181): the cards' votes as mv [vo, mts]; ours under our socket id (io-init)
    uint8_t mvb[170];
    memset(mvb, 0, sizeof mvb);
    mvb[0] = 1; mvb[1] = 0;
    w_i32(mvb, 2, 0); w_i32(mvb, 6, 5); w_u16(mvb, 10, 1);
    native_send(&g_native, NET_PACKET_MATCH_VOTE_STATE, mvb, 170);
    WAIT_FOR(client_count("mv") == 1);
    KrunkerFrame mvf;
    const int got_mv = client_get("mv", 0, &mvf);
    const KrunkerValue *mine = got_mv ? map_get(&mvf.args[0], socket_id) : NULL;
    CHECK(got_mv && mvf.argc == 2 && mine && mine->type == KRUNKER_ARRAY && mine->len == 2 && is_str(&mine->u.items[0], "0,5") &&
          vequal(&mine->u.items[1], &KRUNKER_V_NUM(1)) && mvf.args[1].type == KRUNKER_ARRAY && mvf.args[1].len == 1,
          "mv [{socket id: [\"0,5\", 1]}, [\"0,5\"]] (ours under the socket id, weight 1)");
    krunker_frame_free(&mvf);

    // EXT 0x8001 (KN's account rights): no v10 message, consumed with no stop
    const uint8_t rights[6] = {0x01, 0x80, 0, 0, 0, 0};
    native_send(&g_native, NET_PACKET_EXT, rights, 6);
    pump(50);
    CHECK(v10_bridge_untranslated("EXT 0x8001") == 0 && client_count("mv") == 1, "account rights are consumed");

    // PLAYER_INFO (34): KN's account record after SPAWN, no v10 message: consumed with no stop
    uint8_t pinfo[21] = {0};
    native_send(&g_native, NET_PACKET_PLAYER_INFO, pinfo, 21);
    pump(50);
    CHECK(v10_bridge_untranslated("native 34") == 0, "player info is consumed");

    // END_MEDALS (243, 169 bytes): the player's medals for its END, kept for it; no v10 message of its own, no stop
    uint8_t medals[169] = {0};
    native_send(&g_native, NET_PACKET_END_MEDALS, medals, 169);
    pump(50);
    CHECK(v10_bridge_untranslated("native 243") == 0, "end medals are kept, no stop");

    // PICKUP (128): one pickup's state is the pickup half of "inat" (the v10 client's updatePickup): uid, weapon,
    // true, active, pos when moved; an empty pickup (weapon -1) has a null weapon and a null position
    uint8_t pk[26];
    memset(pk, 0, sizeof pk);
    w_i32(pk, 0, 7); w_i32(pk, 4, 3); pk[8] = 1; pk[9] = 1;
    w_f32(pk, 10, 1.5f); w_f32(pk, 14, 2); w_f32(pk, 18, -3); w_i32(pk, 22, 12);
    native_send(&g_native, NET_PACKET_PICKUP, pk, 26);
    WAIT_FOR(client_count("inat") == 1);
    KrunkerFrame inat1;
    CHECK(client_get("inat", 0, &inat1) && inat1.argc == 8 && vequal(&inat1.args[0], &KRUNKER_V_NUM(-1)) && inat1.args[1].type == KRUNKER_NIL &&
          inat1.args[2].type == KRUNKER_NIL && vequal(&inat1.args[3], &KRUNKER_V_NUM(7)) && vequal(&inat1.args[4], &KRUNKER_V_NUM(3)) &&
          vequal(&inat1.args[5], &KRUNKER_V_BOOL(1)) && vequal(&inat1.args[6], &KRUNKER_V_BOOL(1)) && inat1.args[7].type == KRUNKER_ARRAY &&
          inat1.args[7].len == 3 && vequal(&inat1.args[7].u.items[0], &KRUNKER_V_NUM(1.5)) && vequal(&inat1.args[7].u.items[2], &KRUNKER_V_NUM(-3)),
          "inat [-1, null, null, 7, 3, true, true, [1.5, 2, -3]]");
    krunker_frame_free(&inat1);
    pk[4] = 0xff; pk[5] = 0xff; pk[6] = 0xff; pk[7] = 0xff; /* weapon -1 */
    pk[8] = 0; pk[9] = 0;
    native_send(&g_native, NET_PACKET_PICKUP, pk, 26);
    WAIT_FOR(client_count("inat") == 2);
    KrunkerFrame inat2;
    CHECK(client_get("inat", 1, &inat2) && inat2.argc == 8 && inat2.args[4].type == KRUNKER_NIL && vequal(&inat2.args[6], &KRUNKER_V_BOOL(0)) &&
          inat2.args[7].type == KRUNKER_NIL, "an empty pickup: null weapon (173166), inactive, null position (mapPickup)");
    krunker_frame_free(&inat2);

    // RECON (110): the owner's state after an input goes to the v10 client as one "l" (the 26-slot array)
    {
        uint8_t rc[89];
        memset(rc, 0, sizeof rc);
        w_i32(rc, 0, 4);
        w_f32(rc, 4, 7);
        native_send(&g_native, NET_PACKET_RECON, rc, 89);
        WAIT_FOR(client_count("l") == 1);
        KrunkerFrame lf;
        CHECK(client_get("l", 0, &lf) && lf.argc == 1 && lf.args[0].type == KRUNKER_ARRAY && lf.args[0].len == 26 &&
              vequal(&lf.args[0].u.items[0], &KRUNKER_V_NUM(4)), "l carries the inputSN as slot 0 (one argument)");
        CHECK(v10_bridge_untranslated("RECON") == 0, "RECON is translated");
        krunker_frame_free(&lf);
    }

    // SHOT (21): player 9 fires one ray: the tracer "9" for the observer, its origin is 9's state (1.26, 2, 3, height 9.2);
    // the session stays up (no stop), so the chat below still goes through
    {
        uint8_t ray[31];
        memset(ray, 0, sizeof ray);
        w_i32(ray, 0, 9);
        w_f32(ray, 8, 4); w_f32(ray, 12, 5); w_f32(ray, 16, 6);
        ray[29] = 1;
        native_send(&g_native, NET_PACKET_SHOT, ray, 31);
        WAIT_FOR(client_count("9") == 1);
        KrunkerFrame tr;
        CHECK(client_get("9", 0, &tr) && tr.argc == 1 && tr.args[0].type == KRUNKER_ARRAY && tr.args[0].len == 13 &&
              vequal(&tr.args[0].u.items[0], &KRUNKER_V_NUM(9)) && vequal(&tr.args[0].u.items[7], &KRUNKER_V_NUM(1.3)) &&
              vequal(&tr.args[0].u.items[9], &KRUNKER_V_NUM(9.2)) && tr.args[0].u.items[4].type == KRUNKER_NIL,
              "tracer 9: sid 9, the hit 4, 5, 6, the origin 1.3, 2, 9.2, 3");
        krunker_frame_free(&tr);
        CHECK(v10_bridge_untranslated("SHOT") == 0, "a shot does not stop the observer's session");
    }

    // our own SPAWN while we wait to join (not active): the add for us, then v10's death for us, KILL_PLAYER "3",
    // which starts the respawn countdown on the client
    {
        uint8_t sp_wait[150];
        const int adds_before_dead = client_count("0");
        spawn_buf(sp_wait, 1, 1, 0, 4, 0, 100, 1, 2, 3, 0, 0, "me");
        native_send(&g_native, NET_PACKET_SPAWN, sp_wait, 150);
        WAIT_FOR(client_count("3") == 1);
        KrunkerFrame dc;
        CHECK(client_get("3", 0, &dc) && dc.argc == 6 && vequal(&dc.args[0], &KRUNKER_V_NUM(1)) && dc.args[4].type == KRUNKER_NIL,
              "3 [1, 0, 0, [], nil, false]: the death for the countdown");
        krunker_frame_free(&dc);
        /* then "frs" (ENTER_GAME): the client calls enterGame(true), which sends "en" (checked at ENTER_GAME -> ENT below) */
        WAIT_FOR(client_count("frs") == 1);
        KrunkerFrame frs;
        CHECK(client_get("frs", 0, &frs) && frs.argc == 1 && frs.args[0].type == KRUNKER_BOOL && frs.args[0].u.boolean == 1,
              "frs [true] right after the countdown");
        krunker_frame_free(&frs);
        // our dead SPAWN adds no player: the client's init (which fetches the map) must not see one before it has run
        // player before it has run, or init breaks (the join's TypeErrors)
        CHECK(client_count("0") == adds_before_dead, "our dead SPAWN adds no player (the add is for players in play)");
        CHECK(v10_bridge_untranslated("SPAWN") == 0, "a waiting joiner's spawn does not stop the observer's session");
    }

    // LEAVE (20): player 9 disconnects: the observer's "2" [9]; the session stays up (the next state still comes through)
    {
        uint8_t lv[4];
        w_i32(lv, 0, 9);
        native_send(&g_native, NET_PACKET_LEAVE, lv, 4);
        WAIT_FOR(client_count("2") == 1);
        KrunkerFrame gone;
        CHECK(client_get("2", 0, &gone) && gone.argc == 1 && vequal(&gone.args[0], &KRUNKER_V_NUM(9)), "2 [9]: the player left");
        krunker_frame_free(&gone);
        CHECK(v10_bridge_untranslated("LEAVE") == 0, "a leave does not stop the observer's session");
        uint8_t st11[79];
        memset(st11, 0, sizeof st11);
        w_i32(st11, 0, 11);
        w_f32(st11, 13, 2.5f); w_f32(st11, 17, 2); w_f32(st11, 21, 3);
        w_f32(st11, 41, 1); w_f32(st11, 57, 0.25f); st11[61] = 1; w_f32(st11, 53, 0.6f); w_i32(st11, 9, 2); w_f32(st11, 49, 1);
        native_send(&g_native, NET_PACKET_STATE, st11, 79);
        WAIT_FOR(client_count("k") >= 2);
    }

    // the round's end (free-for-all): END (16) then MODE_END (51), which the client reads as one "end" with the table (endData.ed)
    {
        uint8_t endp[330];
        memset(endp, 0, sizeof endp);
        endp[0] = 1;   /* a victory */
        endp[9] = 1;   /* one board row: uid 1 (our spawn's name "me") */
        w_i32(endp, 10, 1); w_i32(endp, 14, 9); w_i32(endp, 18, 2); w_i32(endp, 22, 3);
        w_i32(endp, 266, 4);
        native_send(&g_native, NET_PACKET_END, endp, sizeof endp);
        NetModeEndPacket mend;
        memset(&mend, 0, sizeof mend);
        mend.mode_id = 0; /* Free for All */
        mend.winner_team = -1;
        native_send(&g_native, NET_PACKET_MODE_END, (const uint8_t *)&mend, sizeof mend);
        WAIT_FOR(client_count("end") == 1);
        KrunkerFrame ef;
        CHECK(client_get("end", 0, &ef) && ef.argc == 7 && ef.args[2].type == KRUNKER_MAP, "one end [0, true, endData, ...] after END and MODE_END");
        const KrunkerValue *ed = ef.argc == 7 ? map_get(&ef.args[2], "ed") : NULL;
        CHECK(ed && ed->type == KRUNKER_ARRAY && ed->len == 1 && ed->u.items[0].type == KRUNKER_ARRAY && ed->u.items[0].len == 6 &&
              vequal(&ed->u.items[0].u.items[0], &KRUNKER_V_NUM(1)) && vequal(&ed->u.items[0].u.items[2], &KRUNKER_V_NUM(9)) &&
              is_str(&ed->u.items[0].u.items[1].u.items[0], "me") && vequal(&ed->u.items[0].u.items[5], &KRUNKER_V_NUM(4)),
              "endData.ed: the row [uid, [name, name], score, kills, deaths, reward] with our name from the spawn");
        if (ef.argc) krunker_frame_free(&ef);
        CHECK(v10_bridge_untranslated("END") == 0 && v10_bridge_untranslated("MODE_END") == 0, "END and MODE_END are translated, the session goes on");
    }

    // the next round: the round change is the game state ENDED (gsc 5), then the next map's MAP_INFO (its MAP_REQUEST), the
    // config, the networking, INIT for that map (the same v10 "init" again: the client runs its init for the new round),
    // RANKED after it (net_send_init), and the map's MAP_DATA. Our respawn then starts the round (START again).
    {
        const uint8_t gsc_end[3] = {0x01, 0x09, 0x05};
        native_send(&g_native, NET_PACKET_EXT, gsc_end, 3);
        WAIT_FOR(client_count("gsc") == 2);
        KrunkerFrame ge;
        CHECK(client_get("gsc", 1, &ge) && ge.argc == 1 && vequal(&ge.args[0], &KRUNKER_V_NUM(5)), "gsc [5]: the round ended");
        krunker_frame_free(&ge);

        const char map2_text[] = "{\"name\":\"Second Map\",\"objects\":[]}";
        const size_t map2_len = strlen(map2_text);
        const uint32_t map2_hash = 0xbeef01;
        uint8_t *mi2 = mk(44);
        w_i32(mi2, 0, 4);
        w_i32(mi2, 4, (int32_t)map2_hash);
        w_i32(mi2, 8, (int32_t)map2_len);
        w_str(mi2, 12, "Second Map");
        native_send(&g_native, NET_PACKET_MAP_INFO, mi2, 44);
        WAIT_FOR(native_count(NET_PACKET_MAP_REQUEST) == 2);
        NFrame *req2 = native_nth(NET_PACKET_MAP_REQUEST, 1);
        CHECK(req2 && req2->len == 4 && rd_i32_test(req2->payload, 0) == (int32_t)map2_hash, "MAP_REQUEST for the next round's map hash");

        native_send(&g_native, NET_PACKET_GAME_CONFIG, chunk1, 8 + cut);
        native_send(&g_native, NET_PACKET_GAME_CONFIG, chunk2, 8 + config_len - cut);
        native_send(&g_native, NET_PACKET_NETWORK_DATA, network, 10);
        uint8_t init2[18];
        memcpy(init2, init, sizeof init2);
        w_i32(init2, 3, 4); /* the next map's index */
        native_send(&g_native, NET_PACKET_INIT, init2, sizeof init2);
        native_send(&g_native, NET_PACKET_EXT, ranked_off, 3);
        uint8_t *mc2n = mk(10 + map2_len);
        w_i32(mc2n, 0, (int32_t)map2_hash); w_i32(mc2n, 4, 0); w_u16(mc2n, 8, (uint16_t)map2_len); memcpy(mc2n + 10, map2_text, map2_len);
        native_send(&g_native, NET_PACKET_MAP_DATA, mc2n, 10 + map2_len);
        free(mc2n);
        WAIT_FOR(client_count("init") == 2);
        KrunkerFrame ini2;
        CHECK(client_get("init", 1, &ini2) && ini2.argc == 20, "the next round's init has 20 arguments");
        if (ini2.argc == 20) {
            const KrunkerValue *a = ini2.args;
            CHECK(vequal(&a[0], &KRUNKER_V_NUM(4)), "the next round's init arg 0 is its map index");
            CHECK(a[9].type == KRUNKER_MAP && is_str(map_get(&a[9], "data"), map2_text), "the next round's init arg 9: its map JSON");
        }
        krunker_frame_free(&ini2);
        CHECK(v10_bridge_untranslated("MAP_INFO") == 0, "the round change is translated, no stop at MAP_INFO");

        // our respawn in the next round: the START again (the bridge held the others' adds for it)
        uint8_t sp_next[150];
        spawn_buf(sp_next, 1, 1, 1, 4, 0, 100, 1, 2, 3, 0, 0, "me");
        native_send(&g_native, NET_PACKET_SPAWN, sp_next, 150);
        WAIT_FOR(client_count("start") == 2);
        KrunkerFrame st2;
        CHECK(client_get("start", 1, &st2) && st2.argc == 8, "start again in the next round");
        krunker_frame_free(&st2);
    }

    // CHEAT (60): one chat command's flag, and on / off, is the v10 label's one argument (noc, unl, fh, frz)
    static const struct { uint8_t flag, on; const char *label; } cheats[] = {
        {0, 1, "noc"}, {1, 1, "unl"}, {2, 1, "fh"}, {7, 1, "frz"}, {1, 0, "unl"},
    };
    for (size_t c = 0; c < sizeof cheats / sizeof cheats[0]; c++) {
        const int before = client_count(cheats[c].label);
        const uint8_t body[2] = {cheats[c].flag, cheats[c].on};
        native_send(&g_native, NET_PACKET_CHEAT, body, 2);
        WAIT_FOR(client_count(cheats[c].label) > before);
        KrunkerFrame cf;
        CHECK(client_get(cheats[c].label, before, &cf) && cf.argc == 1 && vequal(&cf.args[0], &KRUNKER_V_BOOL(cheats[c].on == 1)),
              "CHEAT %u -> %s %u", (unsigned)cheats[c].flag, cheats[c].label, (unsigned)cheats[c].on);
        krunker_frame_free(&cf);
    }
    // borders (3) has no v10 label: it still stops the translation, named CHEAT
    const uint8_t borders[2] = {3, 1};
    native_send(&g_native, NET_PACKET_CHEAT, borders, 2);
    WAIT_FOR(v10_bridge_untranslated("CHEAT") == 1);

    // ENTER_GAME -> ENT: the client's "en" array becomes the KN join packet (the bytes of encodeEnter)
    KrunkerValue en_items[T_EN_LEN];
    for (int i = 0; i < T_EN_LEN; i++) en_items[i] = KRUNKER_V_NUM(-1);
    en_items[0] = KRUNKER_V_NUM(4);
    en_items[5] = KRUNKER_V_NUM(-1);
    en_items[1] = KRUNKER_V_NUM(-1);
    KrunkerValue skins_pair[2] = {KRUNKER_V_NUM(-1), KRUNKER_V_NUM(-1)};
    KrunkerValue charms_pair[2] = {KRUNKER_V_NUM(-1), KRUNKER_V_NUM(-1)};
    en_items[2] = KRUNKER_V_ARRAY(skins_pair, 2);
    en_items[9] = KRUNKER_V_NUM(-1);
    en_items[11] = KRUNKER_V_NUM(1);
    en_items[12] = KRUNKER_V_NUM(-1);
    en_items[6] = KRUNKER_V_NUM(0);
    en_items[7] = KRUNKER_V_NUM(0);
    en_items[19] = KRUNKER_V_NUM(0);
    en_items[29] = KRUNKER_V_ARRAY(charms_pair, 2);
    KrunkerValue join_args[10] = {
        KRUNKER_V_ARRAY(en_items, T_EN_LEN), KRUNKER_V_NUM(16), KRUNKER_V_NUM(18), KRUNKER_V_BOOL(0), KRUNKER_V_BOOL(0),
        KRUNKER_V_BOOL(0), KRUNKER_V_BOOL(0), KRUNKER_V_BOOL(0), KRUNKER_V_STR(""), KRUNKER_V_BOOL(0),
    };
    client_send("en", join_args, 10);
    WAIT_FOR(native_count(NET_PACKET_ENT) == 1);
    NFrame *ent = native_nth(NET_PACKET_ENT, 0);
    CHECK(ent != NULL && ent->len == V10_ENT_SIZE && (int32_t)(ent->payload[0] | ent->payload[1] << 8 | ent->payload[2] << 16 | ent->payload[3] << 24) == 4,
          "ENT: 101 bytes, class 4");

    // q (INPUT): the official client's window (seq 5 and 6, shoot) reaches the native input batch (111)
    {
        KrunkerValue ch_vals[11] = {KRUNKER_V_NUM(3), KRUNKER_V_NUM(1), KRUNKER_V_NUM(0), KRUNKER_V_NUM(0), KRUNKER_V_NUM(0),
                                    KRUNKER_V_NUM(0), KRUNKER_V_NUM(0), KRUNKER_V_NUM(0), KRUNKER_V_NUM(0), KRUNKER_V_NUM(0), KRUNKER_V_NUM(0)};
        static const char *const ch_keys[11] = {"0-4", "0-5", "0-6", "0-7", "0-8", "0-9", "0-10", "0-11", "0-12", "0-13", "0-14"};
        KrunkerValue *map_kv = malloc(sizeof(KrunkerValue) * 22);
        for (int i = 0; i < 11; i++) { map_kv[2 * i] = KRUNKER_V_STR(ch_keys[i]); map_kv[2 * i + 1] = ch_vals[i]; }
        KrunkerValue *angle_items = malloc(sizeof(KrunkerValue) * 4);
        angle_items[0] = KRUNKER_V_NUM(100); angle_items[1] = KRUNKER_V_NUM(-200);
        angle_items[2] = KRUNKER_V_NUM(5); angle_items[3] = KRUNKER_V_NUM(2);
        KrunkerValue q_args[6] = {KRUNKER_V_NUM(0), KRUNKER_V_NUM(5), KRUNKER_V_STR("1616"), KRUNKER_V_NUM(2),
                                  KRUNKER_V_ARRAY(angle_items, 4), KRUNKER_V_MAP(map_kv, 11)};
        client_send("q", q_args, 6);
        WAIT_FOR(native_count(NET_PACKET_INPUT_BATCH) == 1);
        NFrame *in_batch = native_nth(NET_PACKET_INPUT_BATCH, 0);
        CHECK(in_batch && in_batch->len == 1 + 24 * 2 && in_batch->payload[0] == 2, "q -> INPUT_BATCH with two frames");
        CHECK(in_batch && rd_i32_test(in_batch->payload, 1) == 5 && rd_i32_test(in_batch->payload, 25) == 6, "the frames' seq 5 and 6");
        CHECK(in_batch && in_batch->payload[1 + 20] == NET_INPUT_SHOOT, "frame 0 fires");
    }

    // chat -> CHAT_SEND
    KrunkerValue hi = KRUNKER_V_STR("hello there");
    client_send("ct", &hi, 1);
    WAIT_FOR(native_count(NET_PACKET_CHAT_SEND) == 1);
    NFrame *chat_out = native_nth(NET_PACKET_CHAT_SEND, 0);
    CHECK(chat_out && chat_out->len == 12 && !memcmp(chat_out->payload, "hello there\0", 12), "CHAT_SEND is the text, NUL-terminated");

    // native PING -> PONG, echoed
    const uint8_t ping[4] = {0x78, 0x56, 0x34, 0x12};
    native_send(&g_native, NET_PACKET_PING, ping, 4);
    WAIT_FOR(native_count(NET_PACKET_PONG) == 1);
    NFrame *pong = native_nth(NET_PACKET_PONG, 0);
    CHECK(pong && pong->len == 4 && !memcmp(pong->payload, ping, 4), "PONG echoes the PING");

    // the stop is final: one init, one stop, one gv
    CHECK(v10_bridge_untranslated("CHEAT") == 1, "one stop at CHEAT borders");
    CHECK(client_count("init") == 2, "two inits: the round's and the next round's");
    CHECK(client_count("gv") == 1, "one gv");

    // close: the client closes and the bridge drops the session
    if (g_client) kn_ws_close(g_client, 1000, "test done");
    WAIT_FOR(g_client_closed);
    pump(50);
    v10_bridge_free(bridge);
    if (g_native.cfd >= 0) close(g_native.cfd);
    close(g_native.lfd);
    kn_loop_free(g_loop);
    printf("v10_bridge_test: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
