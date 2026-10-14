// KN_KRUNKER_OFFICIAL transport + session (client/include/krunker_net.h): the matchmaker requests,
// the WebSocket (browser on the web, an RFC 6455 client on http_conn.c natively) and the glue
// between krunker_link (packet translation) and the client's network state.
//
// Threads: the HTTP callbacks and krunker_net_tick run on the main thread; natively the socket
// has its own thread, which decodes and translates frames there (client_net_queue_packet and the
// on_client_socket_* events are made for a transport thread). Every use of the link (its ahNum,
// the input compression state) holds K.lock, so frames leave in encode order from both threads.
#include <krunker_net.h>
#include <krunker_link.h>
#include <krunker_altcha.h>
#include <krunker_captcha.h>
#include <backend.h>
#include <html_ui.h>
#include <ui_logic.h>
#include <cJSON.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/websocket.h>
#elif !defined(_WIN32)
#include <http_conn.h>
#include <http_proxy.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#define KRUNKER_NATIVE_WS 1
#endif

#if KRUNKER_BUILD_SCHEMA != 1
#error "krunker_build.h: unknown schema (tools/krunker/extract_build.py)"
#endif

#define KRUNKER_DEFAULT_MATCHMAKER KRUNKER_MATCHMAKER_URL
static const char *const k_regions[] = {KRUNKER_REGIONS};
#define KRUNKER_DEFAULT_REGION (k_regions[0]) // the prod list's first ("de-fra")
#define KRUNKER_HTTP_TIMEOUT_MS 8000
#define KRUNKER_WS_MAX_MESSAGE (4u * 1024u * 1024u)
// client->net_socket while the official socket is open: never a real descriptor (net.c's
// shutdown() of it fails harmlessly), distinct from net_web.c's 1
#define KRUNKER_FAKE_FD 0x4b524e00

typedef enum {
    KS_OFF = 0,    // not in an official game
    KS_TOKEN,      // GET /generate-token (after a captcha: the retry's fresh validation token)
    KS_SEEK,       // GET /seek-game
    KS_CAPTCHA,    // seek-game said CaptchaRequired: the player is solving Turnstile (krunker_captcha.h)
    KS_CONNECTING, // WebSocket opening
    KS_OPEN,
    KS_CLOSED,     // dropped / refused: the error screen; reconnect seeks again
} KrunkerState;

typedef struct OutFrame {
    struct OutFrame *next;
    size_t len;
    uint8_t data[];
} OutFrame;

static struct {
    Client *client;
    KrunkerState state;
    unsigned generation; // bumped per join / leave: late callbacks of an older attempt are ignored
    char game_id[96], region[48], token[512], matchmaker[512];
    char ws_url[1024];
    int connected;       // on_client_socket_connect was reported for this socket
    KrunkerLink link;
    pthread_mutex_t lock;
    // main-thread notices (krunker_net_tick)
    char notice[256];
    int notice_pending, captcha_pending;
    KrunkerAltcha altcha; // web: the "cpt" being solved in slices
    int altcha_pending;
    int pow_shown;           // the "cpt" modal shows "verifying…" (krunker_captcha_pow_begin)
    volatile int pow_result; // native: the socket thread's "cpt" outcome for the modal (1 solved, 2 not)
    // the matchmaker's Turnstile (game.deob.js seek, :147445-147467): prompts so far this join (the live
    // client gives up after 2), the player's token until the retry's seek-game takes it (then wiped),
    // and whether the last seek carried one (a CaptchaRequired after it = the token was refused)
    int captcha_attempts, captcha_sent;
    char captcha[KRUNKER_CAPTCHA_TOKEN_MAX + 1];
    // the player's texts the socket thread needs, looked up on the main thread per join (i18n is the
    // main thread's): {0} placeholders filled with i18n_replace where they are used
    struct {
        char joined[160], error[160], pow[200], map[160], closed[160];
        char ws_failed[200], ws_denied[200], ws_unreachable[200];
    } tpl;
    volatile int leave_unsupported; // the socket thread saw a map we lack: krunker_net_tick leaves the game
    volatile int leave_closed;      // the server told us to close ("00" without keepOpen): krunker_net_tick leaves the game
#ifdef __EMSCRIPTEN__
    EMSCRIPTEN_WEBSOCKET_T ws;
#elif defined(KRUNKER_NATIVE_WS)
    pthread_t thread;
    int thread_running;
    volatile int quit;
    int wake[2];
    OutFrame *out_head, *out_tail;
#endif
} K = {.lock = PTHREAD_MUTEX_INITIALIZER
#ifdef KRUNKER_NATIVE_WS
       , .wake = {-1, -1}
#endif
};

// ---------------------------------------------------------------- switches

#ifdef __EMSCRIPTEN__
static char *query(const char *name) { // malloc'd ?name= value, NULL when absent
    return (char *) EM_ASM_PTR({
        var v = null;
        try { v = new URLSearchParams(location.search).get(UTF8ToString($0)); } catch (e) {}
        return v !== null ? stringToNewUTF8(v) : 0;
    }, name);
}
#endif

// The env variable (native) / query parameter (web) into out; 1 = set and non-empty.
static int option(const char *env, const char *param, char *out, const size_t n) {
    out[0] = 0;
#ifdef __EMSCRIPTEN__
    (void) env;
    char *v = query(param);
    if (v) snprintf(out, n, "%s", v);
    free(v);
#else
    (void) param;
    const char *v = getenv(env);
    if (v) snprintf(out, n, "%s", v);
#endif
    return out[0] != 0;
}

// In-client unless the opt-out (krunker_net.h): KRUNKNATIVE_KRUNKER_JOIN / ?krunkerJoin = browser
// ("0" / "off" too); "client", "1" or unset = in-client.
// Windows has no native WebSocket client for this mode yet (KRUNKER_NATIVE_WS): the browser stays its
// default there, "client" still asks for the in-client path (which then says why it cannot start).
int krunker_net_in_client(void) {
    char v[32];
    if (!option("KRUNKNATIVE_KRUNKER_JOIN", "krunkerJoin", v, sizeof(v))) {
#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
        return 0;
#else
        return 1;
#endif
    }
    return strcmp(v, "browser") != 0 && strcmp(v, "0") != 0 && strcmp(v, "off") != 0;
}

static int trace_on(void) {
    static int cached = -1;
    if (cached < 0) {
        char v[8];
        cached = option("KRUNKNATIVE_KRUNKER_TRACE", "krunkerTrace", v, sizeof(v)) && strcmp(v, "0") != 0;
    }
    return cached;
}

static void trace_frame(const char *dir, const uint8_t *data, const size_t len) {
    if (!trace_on()) return;
    KrunkerFrame f;
    if (krunker_proto_decode(data, len, 0, &f) != KRUNKER_PROTO_OK) {
        printf("[krunker] %s (%zu bytes, undecodable)\n", dir, len);
        return;
    }
    KrunkerBuf json = {0};
    const KrunkerValue args = KRUNKER_V_ARRAY(f.args, f.argc);
    krunker_value_to_json(&args, &json);
    printf("[krunker] %s %s ah=%d %.*s\n", dir, f.name, f.ah_echo, json.len > 600 ? 600 : (int) json.len, json.data ? (const char *) json.data : "");
    krunker_buf_free(&json);
    krunker_frame_free(&f);
}

static void set_notice(const int captcha, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void set_notice(const int captcha, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(K.notice, sizeof(K.notice), fmt, ap);
    va_end(ap);
    K.notice_pending = 1;
    if (captcha) K.captcha_pending = 1;
}

// ---------------------------------------------------------------- link I/O

static void link_emit(void *user, const NetPacket *packet) {
    (void) user;
    if (!K.client) return;
    client_net_log_packet("<=", (NetPacketType) packet->type, packet->payload, packet->length);
    client_net_queue_packet(K.client, packet);
}

static void link_send(void *user, const uint8_t *frame, const size_t len);

static void link_event(void *user, const KrunkerLinkEvent kind, const char *text) {
    (void) user;
    printf("[krunker] %s%s\n", kind == KRUNKER_LINK_EV_CAPTCHA ? "captcha required " : "", text ? text : "");
    char out[300];
    switch (kind) {
        case KRUNKER_LINK_EV_CAPTCHA: // a "cpt" krunker_altcha cannot answer (not SHA-256 ALTCHA, too big)
            set_notice(1, "%s", K.tpl.pow);
            break;
        case KRUNKER_LINK_EV_ERROR:
            set_notice(0, "%s", i18n_replace(out, sizeof(out), K.tpl.error, 1, I18N_ARGV(text ? text : "error")));
            break;
        case KRUNKER_LINK_EV_UNSUPPORTED: // a map our catalogue lacks: nothing to show, krunker_net_tick leaves
            K.leave_unsupported = 1;
            break;
        case KRUNKER_LINK_EV_CLOSE: // "00" without keepOpen: the live client closes its socket; krunker_net_tick leaves
            K.leave_closed = 1;
            break;
        case KRUNKER_LINK_EV_JOINED:
            set_notice(0, "%s", i18n_replace(out, sizeof(out), K.tpl.joined, 1, I18N_ARGV(K.game_id)));
            break;
        default:
            break;
    }
}

// "cc" with K.lock released: natively the evaluation (QuickJS) may take ~250 ms, the main thread's
// sends must not wait for it (the link's state is not touched meanwhile; ccr is encoded after it)
static int link_cheat(void *user, const char *nonce, const char *ciphertext_b64, KrunkerCcr *out) {
    (void) user;
#ifdef KRUNKER_NATIVE_WS
    pthread_mutex_unlock(&K.lock);
    const int rc = krunker_cheat_answer(nonce, ciphertext_b64, out);
    pthread_mutex_lock(&K.lock);
    return rc;
#else
    return krunker_cheat_answer(nonce, ciphertext_b64, out);
#endif
}

// "cpt" (ALTCHA proof of work, up to ~1 s at the default maximum). Natively on the socket thread
// with K.lock released, like link_cheat; on the web (no threads) in slices from krunker_net_tick,
// so the frame loop never stalls on it.
#define KRUNKER_ALTCHA_SLICE 20000L // numbers per frame on the web (a few ms)
static int link_altcha(void *user, const char *alg, const char *challenge, const char *salt, const char *signature, const long max, char **payload) {
    (void) user;
#ifdef __EMSCRIPTEN__
    (void) payload;
    if (krunker_altcha_begin(&K.altcha, alg, challenge, salt, signature, max) != 0) return -1;
    K.altcha_pending = 1;
    return 1;
#else
    pthread_mutex_unlock(&K.lock);
    const int rc = krunker_altcha_solve(alg, challenge, salt, signature, max, payload);
    pthread_mutex_lock(&K.lock);
    K.pow_result = rc == 0 ? 1 : 2; // shown by krunker_net_tick (the UI is the main thread's)
    return rc;
#endif
}

static void link_setup(void) {
    const KrunkerLinkIO io = {.emit = link_emit, .send = link_send, .event = link_event, .cheat = link_cheat, .altcha = link_altcha, .user = NULL};
    krunker_link_init(&K.link, &io, KRUNKER_AHKEY);
}

// ---------------------------------------------------------------- socket state -> client

static void socket_opened(void) {
    Client *client = K.client;
    pthread_mutex_lock(&client->net_lock);
    client->net_socket = KRUNKER_FAKE_FD;
    pthread_mutex_unlock(&client->net_lock);
    K.connected = 1;
    printf("[krunker] connected to %s\n", K.ws_url);
    on_client_socket_connect(client);
}

// The socket ended (any thread). reason: the close reason (logs, the drop text); message: the
// player's sentence for a connect that never opened (NULL: K.tpl's, from reason).
static void socket_closed(const int code, const char *reason, const char *failed) {
    Client *client = K.client;
    pthread_mutex_lock(&client->net_lock);
    if (client->net_socket == KRUNKER_FAKE_FD) client->net_socket = -1; // (another transport may own it by now)
    pthread_mutex_unlock(&client->net_lock);
    printf("[krunker] socket closed (code %d%s%s): %u frames in, %u out, %u cheat checks answered, %u unanswered\n", code,
           reason && reason[0] ? ": " : "", reason ? reason : "", K.link.frames_in, K.link.frames_out, K.link.cc_answered, K.link.cc_failed);
    if (K.connected) {
        K.connected = 0;
        client_net_set_drop(client, "closed", code, "krunker.io closed the connection (code %d%s%s)", code, reason && reason[0] ? ": " : "",
                            reason ? reason : "");
        on_client_socket_disconnect(client);
    } else {
        char message[300];
        if (failed && failed[0]) snprintf(message, sizeof(message), "%s", failed);
        else if (reason && reason[0]) i18n_replace(message, sizeof(message), K.tpl.ws_failed, 1, I18N_ARGV(reason));
        else snprintf(message, sizeof(message), "%s", K.tpl.ws_unreachable);
        on_client_socket_failed(client, message);
        set_notice(0, "%s", message);
    }
}

static void fail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void fail(const char *fmt, ...) {
    char message[300];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(message, sizeof(message), fmt, ap);
    va_end(ap);
    fprintf(stderr, "[krunker] %s\n", message);
    K.state = KS_CLOSED;
    if (K.client) on_client_socket_failed(K.client, message);
    set_notice(0, "%s", message);
}

// ---------------------------------------------------------------- web socket

#ifdef __EMSCRIPTEN__
static void link_send(void *user, const uint8_t *frame, const size_t len) {
    (void) user;
    trace_frame("=>", frame, len);
    if (K.ws > 0) emscripten_websocket_send_binary(K.ws, (void *) frame, (uint32_t) len);
}

static bool ws_on_open(int type, const EmscriptenWebSocketOpenEvent *e, void *user) {
    (void) type;
    if ((unsigned) (uintptr_t) user != K.generation || e->socket != K.ws) return true;
    K.state = KS_OPEN;
    socket_opened();
    return true;
}

static bool ws_on_message(int type, const EmscriptenWebSocketMessageEvent *e, void *user) {
    (void) type;
    if ((unsigned) (uintptr_t) user != K.generation || e->socket != K.ws || e->isText) return true;
    trace_frame("<=", e->data, e->numBytes);
    pthread_mutex_lock(&K.lock);
    krunker_link_on_frame(&K.link, e->data, e->numBytes);
    pthread_mutex_unlock(&K.lock);
    return true;
}

static bool ws_on_error(int type, const EmscriptenWebSocketErrorEvent *e, void *user) {
    (void) type;
    (void) e;
    (void) user;
    fprintf(stderr, "[krunker] websocket error (%s)\n", K.ws_url);
    return true;
}

static bool ws_on_close(int type, const EmscriptenWebSocketCloseEvent *e, void *user) {
    (void) type;
    emscripten_websocket_delete(e->socket);
    if (e->socket != K.ws) return true; // an older socket
    K.ws = 0;
    if ((unsigned) (uintptr_t) user == K.generation && K.state != KS_OFF) K.state = KS_CLOSED;
    socket_closed(e->code, e->reason, NULL);
    return true;
}

static void ws_open(void) {
    EmscriptenWebSocketCreateAttributes attr;
    emscripten_websocket_init_create_attributes(&attr);
    attr.url = K.ws_url;
    attr.createOnMainThread = true;
    K.ws = emscripten_websocket_new(&attr);
    if (K.ws <= 0) {
        K.ws = 0;
        fail("Could not open a WebSocket to %s", K.ws_url);
        return;
    }
    void *gen = (void *) (uintptr_t) K.generation;
    emscripten_websocket_set_onopen_callback(K.ws, gen, ws_on_open);
    emscripten_websocket_set_onmessage_callback(K.ws, gen, ws_on_message);
    emscripten_websocket_set_onerror_callback(K.ws, gen, ws_on_error);
    emscripten_websocket_set_onclose_callback(K.ws, gen, ws_on_close);
}

// The leave is reported at once (the close callback of the old socket only frees it): another
// session may be up before that callback lands.
static void ws_close(void) {
    if (K.ws <= 0) return;
    const EMSCRIPTEN_WEBSOCKET_T old = K.ws;
    K.ws = 0;
    emscripten_websocket_close(old, 1000, "leave");
    if (K.connected) socket_closed(1000, "left", NULL);
}

// ---------------------------------------------------------------- native socket

#elif defined(KRUNKER_NATIVE_WS)
static void link_send(void *user, const uint8_t *frame, const size_t len) {
    (void) user;
    trace_frame("=>", frame, len);
    OutFrame *f = malloc(sizeof(OutFrame) + len);
    if (!f) return;
    f->next = NULL;
    f->len = len;
    memcpy(f->data, frame, len);
    // (K.lock is held by every caller: the link is only used under it)
    if (K.out_tail) K.out_tail->next = f;
    else K.out_head = f;
    K.out_tail = f;
    if (K.wake[1] >= 0) {
        const char b = 1;
        if (write(K.wake[1], &b, 1) < 0) { /* the pipe is full: the thread wakes anyway */ }
    }
}

static int ws_send_frame(HttpConn *c, const int opcode, const uint8_t *data, const size_t len) {
    uint8_t head[14];
    size_t n = 0;
    head[n++] = (uint8_t) (0x80 | opcode);
    if (len < 126) {
        head[n++] = (uint8_t) (0x80 | len);
    } else if (len < 65536) {
        head[n++] = 0x80 | 126;
        head[n++] = (uint8_t) (len >> 8);
        head[n++] = (uint8_t) len;
    } else {
        head[n++] = 0x80 | 127;
        for (int i = 7; i >= 0; i--) head[n++] = (uint8_t) ((uint64_t) len >> (i * 8));
    }
    uint8_t mask[4];
    for (int i = 0; i < 4; i++) mask[i] = (uint8_t) (rand() & 0xff);
    memcpy(head + n, mask, 4);
    n += 4;
    uint8_t *masked = malloc(len ? len : 1);
    if (!masked) return 0;
    for (size_t i = 0; i < len; i++) masked[i] = data[i] ^ mask[i & 3];
    const int ok = http_conn_send(c, head, n) && (!len || http_conn_send(c, masked, len));
    free(masked);
    return ok;
}

static int recv_exact(HttpConn *c, void *out, size_t len) {
    uint8_t *p = out;
    while (len) {
        if (K.quit) return 0;
        const ssize_t r = http_conn_recv(c, p, len);
        if (r < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (r <= 0) return 0;
        p += r;
        len -= (size_t) r;
    }
    return 1;
}

// ws(s)://host[:port]/path?query -> the connection address and the request target
static int ws_url_split(const char *url, HttpAddr *a, char *target, const size_t n) {
    memset(a, 0, sizeof(*a));
    const char *p;
    if (!strncmp(url, "wss://", 6)) {
        a->tls = 1;
        p = url + 6;
    } else if (!strncmp(url, "ws://", 5)) {
        p = url + 5;
    } else {
        return 0;
    }
    size_t h = strcspn(p, ":/?");
    if (!h || h >= sizeof(a->host)) return 0;
    memcpy(a->host, p, h);
    a->host[h] = 0;
    p += h;
    snprintf(a->port, sizeof(a->port), "%s", a->tls ? "443" : "80");
    if (*p == ':') {
        const size_t pl = strcspn(p + 1, "/?");
        if (!pl || pl >= sizeof(a->port)) return 0;
        memcpy(a->port, p + 1, pl);
        a->port[pl] = 0;
        p += 1 + pl;
    }
    snprintf(target, n, "%s%s", *p == '/' ? "" : "/", p);
    return 1;
}

// 1 = upgraded; 0 = failed, *status = the HTTP status of a refused upgrade (0: no answer)
static int ws_handshake(HttpConn *c, const HttpAddr *a, const char *target, int *status_out) {
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    uint8_t raw[16];
    char key[25];
    for (int i = 0; i < 16; i++) raw[i] = (uint8_t) (rand() & 0xff);
    int k = 0;
    for (int i = 0; i < 16; i += 3) {
        const unsigned v = (unsigned) raw[i] << 16 | (i + 1 < 16 ? (unsigned) raw[i + 1] << 8 : 0) | (i + 2 < 16 ? raw[i + 2] : 0);
        key[k++] = b64[(v >> 18) & 63];
        key[k++] = b64[(v >> 12) & 63];
        key[k++] = i + 1 < 16 ? b64[(v >> 6) & 63] : '=';
        key[k++] = i + 2 < 16 ? b64[v & 63] : '=';
    }
    key[24] = 0;
    const int default_port = !strcmp(a->port, a->tls ? "443" : "80");
    char req[2048];
    // Origin: the page Krunker's game servers serve (a browser sends its page origin)
    const int len = snprintf(req, sizeof(req),
                             "GET %s HTTP/1.1\r\nHost: %s%s%s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                             "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\nOrigin: https://krunker.io\r\n"
                             "User-Agent: KrunkNative\r\n\r\n",
                             target, a->host, default_port ? "" : ":", default_port ? "" : a->port, key);
    *status_out = 0;
    if (len <= 0 || (size_t) len >= sizeof(req) || !http_conn_send(c, req, (size_t) len)) return 0;
    char resp[4096];
    size_t n = 0;
    while (n + 1 < sizeof(resp)) {
        if (!recv_exact(c, resp + n, 1)) return 0;
        n++;
        if (n >= 4 && !memcmp(resp + n - 4, "\r\n\r\n", 4)) break;
    }
    resp[n] = 0;
    int status = 0;
    if (sscanf(resp, "HTTP/%*s %d", &status) != 1 || status != 101) {
        fprintf(stderr, "[krunker] websocket upgrade refused: %.*s\n", (int) strcspn(resp, "\r\n"), resp);
        *status_out = status;
        return 0;
    }
    return 1;
}

static void out_flush(HttpConn *c, int *ok) {
    pthread_mutex_lock(&K.lock);
    OutFrame *f = K.out_head;
    K.out_head = K.out_tail = NULL;
    pthread_mutex_unlock(&K.lock);
    while (f) {
        OutFrame *next = f->next;
        if (*ok) *ok = ws_send_frame(c, 2, f->data, f->len);
        free(f);
        f = next;
    }
}

static void *ws_thread(void *arg) {
    const unsigned generation = (unsigned) (uintptr_t) arg;
    HttpAddr a;
    char target[1024];
    HttpConn c = {.fd = -1};
    int code = 1006, status = 0;
    const char *why = "";
    char failed[300] = ""; // the player's sentence (K.tpl: looked up on the main thread)
    if (!ws_url_split(K.ws_url, &a, target, sizeof(target))) {
        why = "bad WebSocket URL";
        i18n_replace(failed, sizeof(failed), K.tpl.ws_failed, 1, I18N_ARGV(why));
    } else if (!http_conn_open(&c, &a, KRUNKER_HTTP_TIMEOUT_MS)) {
        why = "cannot reach the game server";
        snprintf(failed, sizeof(failed), "%s", K.tpl.ws_unreachable);
    } else if (!ws_handshake(&c, &a, target, &status)) {
        why = "the WebSocket upgrade failed";
        if (status == 403) { // krunker.io's block of this network, as on the lobby list (krunker_lobbies.c)
            snprintf(failed, sizeof(failed), "%s", K.tpl.ws_denied);
        } else {
            char detail[64];
            if (status) snprintf(detail, sizeof(detail), "HTTP %d", status);
            else snprintf(detail, sizeof(detail), "no answer to the WebSocket upgrade");
            i18n_replace(failed, sizeof(failed), K.tpl.ws_failed, 1, I18N_ARGV(detail));
        }
    } else if (!K.quit && generation == K.generation) {
        K.state = KS_OPEN;
        socket_opened();
        uint8_t *message = NULL;
        size_t message_len = 0;
        int ok = 1;
        while (ok && !K.quit) {
            out_flush(&c, &ok);
            if (!ok) break;
            struct pollfd pfd[2] = {{c.fd, POLLIN, 0}, {K.wake[0], POLLIN, 0}};
            const int ready = http_conn_pending(&c) ? 1 : poll(pfd, 2, 100);
            if (ready < 0 && errno == EINTR) continue;
            if (ready < 0) break;
            if (pfd[1].revents & POLLIN) {
                char drain[64];
                while (read(K.wake[0], drain, sizeof(drain)) > 0) {}
            }
            if (!http_conn_pending(&c) && !(pfd[0].revents & (POLLIN | POLLERR | POLLHUP))) continue;
            uint8_t head[2];
            if (!recv_exact(&c, head, 2)) break;
            const int fin = head[0] & 0x80, opcode = head[0] & 0x0f;
            uint64_t len = head[1] & 0x7f;
            if (len == 126) {
                uint8_t ext[2];
                if (!recv_exact(&c, ext, 2)) break;
                len = (uint64_t) ext[0] << 8 | ext[1];
            } else if (len == 127) {
                uint8_t ext[8];
                if (!recv_exact(&c, ext, 8)) break;
                len = 0;
                for (int i = 0; i < 8; i++) len = len << 8 | ext[i];
            }
            uint8_t mask[4] = {0};
            if ((head[1] & 0x80) && !recv_exact(&c, mask, 4)) break;
            if (len > KRUNKER_WS_MAX_MESSAGE || message_len + len > KRUNKER_WS_MAX_MESSAGE) break;
            uint8_t *payload = malloc((size_t) len + 1);
            if (!payload || (len && !recv_exact(&c, payload, (size_t) len))) {
                free(payload);
                break;
            }
            if (head[1] & 0x80) for (uint64_t i = 0; i < len; i++) payload[i] ^= mask[i & 3];
            if (opcode == 8) { // close: status code + reason
                code = len >= 2 ? payload[0] << 8 | payload[1] : 1005;
                free(payload);
                why = "closed by the server";
                break;
            }
            if (opcode == 9) {
                ok = ws_send_frame(&c, 10, payload, (size_t) len);
                free(payload);
                continue;
            }
            if (opcode == 10) {
                free(payload);
                continue;
            }
            if (opcode == 1 || opcode == 2) message_len = 0; // a new message (text ones are not Krunker's)
            uint8_t *grown = realloc(message, message_len + (size_t) len + 1);
            if (!grown) {
                free(payload);
                break;
            }
            message = grown;
            memcpy(message + message_len, payload, (size_t) len);
            message_len += (size_t) len;
            free(payload);
            if (fin && message_len) {
                trace_frame("<=", message, message_len);
                pthread_mutex_lock(&K.lock);
                if (generation == K.generation) krunker_link_on_frame(&K.link, message, message_len);
                pthread_mutex_unlock(&K.lock);
                message_len = 0;
            }
        }
        if (K.quit) {
            const uint8_t bye[2] = {1000 >> 8, 1000 & 0xff};
            ws_send_frame(&c, 8, bye, 2);
            code = 1000;
            why = "left";
        }
        free(message);
    }
    if (c.fd >= 0) http_conn_close(&c);
    if (generation == K.generation && K.state != KS_OFF) K.state = KS_CLOSED;
    if (K.quit && !K.connected) return NULL; // left while connecting: nothing to report
    socket_closed(code, why, failed);
    return NULL;
}

static void ws_open(void) {
    if (K.wake[0] < 0 && pipe(K.wake) == 0) {
        fcntl(K.wake[0], F_SETFL, O_NONBLOCK);
        fcntl(K.wake[1], F_SETFL, O_NONBLOCK);
    }
    K.quit = 0;
    if (pthread_create(&K.thread, NULL, ws_thread, (void *) (uintptr_t) K.generation) != 0) {
        fail("Failed to start the krunker.io network thread");
        return;
    }
    K.thread_running = 1;
}

static void ws_close(void) {
    if (!K.thread_running) return;
    K.quit = 1;
    if (K.wake[1] >= 0) {
        const char b = 1;
        if (write(K.wake[1], &b, 1) < 0) { /* full: it wakes anyway */ }
    }
    pthread_join(K.thread, NULL);
    K.thread_running = 0;
    pthread_mutex_lock(&K.lock);
    while (K.out_head) {
        OutFrame *next = K.out_head->next;
        free(K.out_head);
        K.out_head = next;
    }
    K.out_tail = NULL;
    pthread_mutex_unlock(&K.lock);
}

#else // Windows: no native WebSocket client for this mode yet
static void link_send(void *user, const uint8_t *frame, const size_t len) {
    (void) user;
    (void) frame;
    (void) len;
}
static void ws_open(void) {
    fail("%s", I18N("kn.kio.noWindowsSocket", "This Windows build has no WebSocket client for krunker.io's game servers yet: pick a KrunkNative server."));
}
static void ws_close(void) {}
#endif

// ---------------------------------------------------------------- matchmaker

static const char *matchmaker(void) {
    char v[512];
    if (option("KRUNKNATIVE_KRUNKER_MATCHMAKER", "krunkerMM", v, sizeof(v)) && strcmp(v, "off") != 0 && strcmp(v, "0") != 0) {
        snprintf(K.matchmaker, sizeof(K.matchmaker), "%s", v);
    } else {
        snprintf(K.matchmaker, sizeof(K.matchmaker), "%s", KRUNKER_DEFAULT_MATCHMAKER);
    }
    size_t n = strlen(K.matchmaker);
    while (n && K.matchmaker[n - 1] == '/') K.matchmaker[--n] = 0;
    return K.matchmaker;
}

static int is_production(void) { return !strcmp(K.matchmaker, KRUNKER_DEFAULT_MATCHMAKER); }

// The proxy (Settings > Client, http_proxy.h) a connection to url's host goes through: its label in
// label, 1 = proxied. The web has none (the browser is the network stack).
static int proxy_for_url(const char *url, char *label, const size_t n) {
    label[0] = 0;
#ifdef KRUNKER_NATIVE_WS
    const char *p = strstr(url, "://");
    const int tls = !strncmp(url, "https:", 6) || !strncmp(url, "wss:", 4);
    p = p ? p + 3 : url;
    char host[128];
    size_t len = 0;
    while (p[len] && p[len] != ':' && p[len] != '/' && p[len] != '?' && len + 1 < sizeof(host)) {
        host[len] = p[len];
        len++;
    }
    host[len] = 0;
    HttpProxy px;
    if (!http_proxy_for(host, tls, &px)) return 0;
    http_proxy_label(&px, label, n);
    return 1;
#else
    (void) url;
    (void) n;
    return 0;
#endif
}

// krunker.io's block page: "ACCESS DENIED" HTML (a 403) instead of JSON (as krunker_lobbies.c sees it)
static int access_denied(const int status, const char *body) {
    static const char WORD[] = "access denied";
    for (const char *p = body; p && *p; p++) {
        size_t i = 0;
        while (i < sizeof(WORD) - 1 && p[i] && (p[i] | 0x20) == WORD[i]) i++;
        if (i == sizeof(WORD) - 1) return 1;
    }
    return status == 403;
}

// krunker.io refused this network (ACCESS DENIED): the proxy setting is the way round it (natively)
static void denied_text(char *out, const size_t n, const char *url) {
    char proxy[160];
#ifdef __EMSCRIPTEN__
    (void) url;
    (void) proxy;
    snprintf(out, n, "%s", I18N("kn.kio.deniedWeb", "krunker.io refused this network (ACCESS DENIED). Try another network, or the desktop client with a proxy."));
#else
    if (proxy_for_url(url, proxy, sizeof(proxy)))
        i18n_replace(out, n, I18N("kn.kio.deniedProxy", "krunker.io refused this network (ACCESS DENIED), also through the proxy {0}. Try another proxy in Settings > Client."), 1, I18N_ARGV(proxy));
    else
        snprintf(out, n, "%s", I18N("kn.kio.denied", "krunker.io refused this network (ACCESS DENIED). Set a proxy in Settings > Client."));
#endif
}

// what (the matchmaker / the game server) did not answer at url
static void unreachable_text(char *out, const size_t n, const char *url, const int game_server) {
    char proxy[160];
    if (proxy_for_url(url, proxy, sizeof(proxy))) {
        if (game_server)
            i18n_replace(out, n, I18N("kn.kio.gsProxyUnreachable", "Could not reach the krunker.io game server through the proxy {0}. Check it in Settings > Client."), 1, I18N_ARGV(proxy));
        else
            i18n_replace(out, n, I18N("kn.kio.mmProxyUnreachable", "Could not reach krunker.io's matchmaker through the proxy {0}. Check it in Settings > Client."), 1, I18N_ARGV(proxy));
        return;
    }
#ifdef __EMSCRIPTEN__
    snprintf(out, n, "%s", game_server ? I18N("kn.kio.gsUnreachableWeb", "Could not reach the krunker.io game server. Check your connection.")
                                       : I18N("kn.kio.mmUnreachableWeb", "Could not reach krunker.io's matchmaker. Check your connection."));
#else
    snprintf(out, n, "%s", game_server ? I18N("kn.kio.gsUnreachable", "Could not reach the krunker.io game server. Check your connection, or set a proxy in Settings > Client.")
                                       : I18N("kn.kio.mmUnreachable", "Could not reach krunker.io's matchmaker. Check your connection, or set a proxy in Settings > Client."));
#endif
}

static void fail_unreachable(void) {
    char text[300];
    unreachable_text(text, sizeof(text), K.matchmaker, 0);
    fail("%s", text);
}

// The texts the socket thread uses (K.tpl), in the page's language at join time (main thread).
static void load_texts(void) {
    snprintf(K.tpl.joined, sizeof(K.tpl.joined), "%s", I18N("kn.kio.joined", "Joined the official krunker.io game {0}"));
    snprintf(K.tpl.error, sizeof(K.tpl.error), "%s", I18N("kn.kio.serverError", "krunker.io: {0}"));
    snprintf(K.tpl.pow, sizeof(K.tpl.pow), "%s",
             I18N("kn.kio.powUnsupported", "krunker.io's game server asked for a proof of work in a form this client cannot compute yet: it may close the game."));
    snprintf(K.tpl.map, sizeof(K.tpl.map), "%s", I18N("kn.kio.mapMissing", "Map index {0} of this official game is not in this client's catalogue: pick another lobby."));
    snprintf(K.tpl.closed, sizeof(K.tpl.closed), "%s", I18N("kn.kio.sessionClosed", "The krunker.io game server ended this session."));
    snprintf(K.tpl.ws_failed, sizeof(K.tpl.ws_failed), "%s", I18N("kn.kio.gsFailed", "Could not connect to the krunker.io game server: {0}"));
}

// per seek answer: the game server's denied / unreachable sentences for its URL (K.ws_url)
static void load_ws_texts(void) {
    denied_text(K.tpl.ws_denied, sizeof(K.tpl.ws_denied), K.ws_url);
    unreachable_text(K.tpl.ws_unreachable, sizeof(K.tpl.ws_unreachable), K.ws_url, 1);
}

// URLSearchParams.toString(): application/x-www-form-urlencoded (space '+', * - . _ kept)
static void form_encode(char *out, const size_t n, const char *s) {
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *) s; *p && o + 4 < n; p++) {
        const unsigned char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '*' || c == '-' || c == '.' || c == '_') {
            out[o++] = (char) c;
        } else if (c == ' ') {
            out[o++] = '+';
        } else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 15];
        }
    }
    out[o] = 0;
}

static void append_param(char *url, const size_t n, const char *key, const char *value) {
    char enc[KRUNKER_CAPTCHA_TOKEN_MAX * 3 + 1];
    form_encode(enc, sizeof(enc), value);
    const size_t len = strlen(url);
    snprintf(url + len, n - len, "%s%s=%s", strchr(url, '?') ? "&" : "?", key, enc);
}

static void wipe(void *p, size_t n) {
    volatile unsigned char *v = p;
    while (n--) *v++ = 0;
}

static int http_get(const char *url, BackendHttpFn fn) {
    void *gen = (void *) (uintptr_t) K.generation;
#ifdef __EMSCRIPTEN__
    return backend_http_request("GET", url, NULL, fn, gen);
#else
    return backend_http_get_external(url, KRUNKER_HTTP_TIMEOUT_MS, fn, gen);
#endif
}

static int stale(void *user) { return (unsigned) (uintptr_t) user != K.generation || K.state == KS_OFF; }

static void on_seek(void *user, int status, const char *body, size_t length);

static void seek(void) {
    // matchmakerSeek (game.deob.js:147610-147631): hostname, region, autoChangeGame,
    // validationToken, then game / excludeGame / dataQuery / captchaToken / accessToken when set
    // (null ones are dropped, matchmakerRequest :147647). The first seek has no validationToken
    // (iïîìîíì passes undefined for a guest); only the captcha retry fetches one (:147466).
    static char url[16384];
    snprintf(url, sizeof(url), "%s/seek-game", K.matchmaker);
    append_param(url, sizeof(url), "hostname", "krunker.io");
    append_param(url, sizeof(url), "region", K.region);
    append_param(url, sizeof(url), "autoChangeGame", "false");
    if (K.token[0]) append_param(url, sizeof(url), "validationToken", K.token);
    if (K.game_id[0]) append_param(url, sizeof(url), "game", K.game_id);
    append_param(url, sizeof(url), "dataQuery", "{\"v\":\"" KRUNKER_BUILD_VERSION "\"}"); // iïîìîíì: {v: buildVersion}
    const size_t logged = strlen(url); // the log stops before the captcha token: never logged
    K.captcha_sent = K.captcha[0] != 0;
    if (K.captcha_sent) append_param(url, sizeof(url), "captchaToken", K.captcha);
    K.state = KS_SEEK;
    printf("[krunker] seek-game: %.*s%s\n", (int) logged, url, K.captcha_sent ? "&captchaToken=(the player's Turnstile token)" : "");
    const int queued = http_get(url, on_seek); // (the request keeps its own copy)
    wipe(K.captcha, sizeof(K.captcha));          // single-use: never sent twice
    wipe(url, sizeof(url));
    if (!queued) fail_unreachable();
}

// The player solved (or gave up on) the matchmaker's Turnstile: a fresh validation token, then the
// same seek with captchaToken (game.deob.js:147459-147466).
static void on_token(void *user, int status, const char *body, size_t length);

static void captcha_done(void *user, const char *token) {
    if (stale(user) || K.state != KS_CAPTCHA) return;
    if (!token) {
        // the modal's Cancel (krunker_captcha_ui.c finish(NULL)), or a build with no page to show it in
        if (!html_ui_available())
            fail("%s", I18N("kn.kio.captchaNoUi", "krunker.io asks for a security challenge, and this build has no menu to show it in: no game was joined."));
        else
            fail("%s", I18N("kn.kio.captchaCancelled", "You closed krunker.io's security challenge, so no game was joined. Pick the lobby again to retry."));
        return;
    }
    snprintf(K.captcha, sizeof(K.captcha), "%s", token);
    K.state = KS_TOKEN;
    char url[600];
    snprintf(url, sizeof(url), "%s/generate-token", K.matchmaker); // fetchValidationToken (game.deob.js:147576)
    if (!http_get(url, on_token)) {
        wipe(K.captcha, sizeof(K.captcha));
        fail_unreachable();
    }
}

static void on_token(void *user, const int status, const char *body, const size_t length) {
    if (stale(user) || K.state != KS_TOKEN) return;
    K.token[0] = 0;
    if (status == 200 && body && length && length < sizeof(K.token)) {
        size_t n = 0;
        for (size_t i = 0; i < length && n + 1 < sizeof(K.token); i++) {
            if ((unsigned char) body[i] > ' ' && body[i] != '"') K.token[n++] = body[i];
        }
        K.token[n] = 0;
    }
    if (K.token[0]) printf("[krunker] generate-token: %zu chars\n", strlen(K.token));
    else fprintf(stderr, "[krunker] generate-token failed (HTTP %d): seeking without a validation token\n", status);
    seek();
}

// {host, port, gameId, clientId, data} -> the WebSocket URL (game.deob.js:222360-222371)
static void on_seek(void *user, const int status, const char *body, const size_t length) {
    (void) length;
    if (stale(user) || K.state != KS_SEEK) return;
    cJSON *res = body ? cJSON_Parse(body) : NULL;
    const char *error = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(res, "error"));
    const char *host = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(res, "host"));
    const char *game = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(res, "gameId"));
    const char *client_id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(res, "clientId"));
    const cJSON *port = cJSON_GetObjectItemCaseSensitive(res, "port");
    char port_s[16] = "";
    if (cJSON_IsNumber(port)) snprintf(port_s, sizeof(port_s), "%d", port->valueint);
    else if (cJSON_IsString(port)) snprintf(port_s, sizeof(port_s), "%s", port->valuestring);
    if (error && !strcmp(error, "CaptchaRequired")) {
        // the live client asks getCaptchaToken at most twice per seek, then gives up (:147454)
        const int refused = K.captcha_sent;
        K.captcha_sent = 0;
        if (K.captcha_attempts >= 2) {
            // (every prompt after the first follows a refused token: a cancel ends the join at once)
            char tries[16], text[300];
            snprintf(tries, sizeof(tries), "%d", K.captcha_attempts);
            I18N_F(text, "kn.kio.captchaRefused", "krunker.io refused the security challenge's token {0} times, so no game was joined. Wait a minute and try again.", tries);
            fail("%s", text);
        } else {
            K.captcha_attempts++;
            K.state = KS_CAPTCHA;
            printf("[krunker] seek-game: CaptchaRequired%s: the player solves Turnstile\n", refused ? " again (the token was refused)" : "");
            krunker_captcha_ask(K.captcha_attempts, refused, captcha_done, user);
        }
    } else if (status == 0) { // no answer at all (DNS, refused, timeout, the proxy)
        fail_unreachable();
    } else if (status != 200 && !error && access_denied(status, body)) {
        char text[300];
        denied_text(text, sizeof(text), K.matchmaker);
        fail("%s", text);
    } else if (status != 200 || error || !host || !game || !client_id) {
        char detail[200], code[16], text[300];
        // the matchmaker's own error word, else the start of its body on one line
        snprintf(detail, sizeof(detail), "%.120s", error ? error : body && body[0] ? body : "(no reason given)");
        for (char *c = detail; *c; c++) if (*c == '\r' || *c == '\n' || *c == '\t') *c = ' ';
        snprintf(code, sizeof(code), "%d", status);
        I18N_F(text, "kn.kio.seekRefused", "krunker.io's matchmaker turned the join down (HTTP {0}) {1}", code, detail);
        fail("%s", text);
    } else {
        snprintf(K.game_id, sizeof(K.game_id), "%s", game);
        char query[600];
        snprintf(query, sizeof(query), "gameId=%s&clientKey=%s", game, client_id); // no clientUID / bpt / tc / at (guest) / dm
        if (is_production() || !port_s[0]) {
            snprintf(K.ws_url, sizeof(K.ws_url), "wss://%s/ws?%s", host, query);
        } else {
            snprintf(K.ws_url, sizeof(K.ws_url), "%s://%s:%s/ws?%s", strncmp(K.matchmaker, "https:", 6) ? "ws" : "wss", host, port_s, query);
        }
        printf("[krunker] seek-game: game %s on %s:%s, opening %s\n", game, host, port_s, K.ws_url);
        load_ws_texts();
        K.state = KS_CONNECTING;
        pthread_mutex_lock(&K.lock);
        krunker_link_reset(&K.link);
        pthread_mutex_unlock(&K.lock);
        ws_open();
    }
    cJSON_Delete(res);
}

// ---------------------------------------------------------------- API

void krunker_net_attach(Client *client) {
    K.client = client;
    link_setup();
}

int krunker_net_active(void) { return K.state != KS_OFF; }
int krunker_net_open(void) { return K.state == KS_OPEN; }

static void stop_socket(void) {
    K.generation++;
    ws_close();
}

int krunker_net_join(Client *client, const char *game_id, const char *region) {
    if (!client) return 0;
    if (!K.client) krunker_net_attach(client);
    if (K.state != KS_OFF) krunker_net_leave(client, NULL); // another official game: an intended switch
    // our own transports and the offline game make way (an intended switch, not a drop)
    client_net_release(client);
    if (client->game.player_count) client_clear_players(client);
    client->in_game = 0;
    client->menu_play_pending = 0;

    K.generation++;
    snprintf(K.game_id, sizeof(K.game_id), "%s", game_id ? game_id : "");
    char r[48];
    if (region && region[0]) snprintf(K.region, sizeof(K.region), "%s", region);
    else if (option("KRUNKNATIVE_KRUNKER_REGION", "krunkerRegion", r, sizeof(r))) snprintf(K.region, sizeof(K.region), "%s", r);
    else snprintf(K.region, sizeof(K.region), "%s", KRUNKER_DEFAULT_REGION);
    K.token[0] = 0;
    K.connected = 0;
    K.altcha_pending = 0;
    K.captcha_attempts = K.captcha_sent = 0;
    K.pow_shown = 0;
    K.leave_unsupported = 0;
    K.leave_closed = 0;
    wipe(K.captcha, sizeof(K.captcha));
    load_texts();
    matchmaker();
    printf("[krunker] joining official game %s (region %s) through %s\n", K.game_id[0] ? K.game_id : "(any)", K.region, K.matchmaker);
    // the live client's order: seek-game at once, no validation token; generate-token only before a
    // captcha retry (captcha_done)
    seek();
    return K.state != KS_CLOSED;
}

void krunker_net_leave(Client *client, const char *reason) {
    if (K.state == KS_OFF) return;
    if (client && K.connected) {
        pthread_mutex_lock(&client->net_lock);
        if (reason) {
            snprintf(client->net_disconnect_reason, sizeof(client->net_disconnect_reason), "%s", reason);
            snprintf(client->net_disconnect_code, sizeof(client->net_disconnect_code), "000000");
        } else {
            client->net_close_intended = 1;
        }
        pthread_mutex_unlock(&client->net_lock);
    }
    printf("[krunker] leaving the official game%s%s\n", reason ? ": " : "", reason ? reason : "");
    K.state = KS_OFF;
    K.altcha_pending = K.pow_shown = 0;
    krunker_captcha_close(); // a pending Turnstile request ends with the session
    wipe(K.captcha, sizeof(K.captcha));
    stop_socket(); // native: joins the thread, which reports the disconnect; web: the close callback does
}

void krunker_net_reconnect(Client *client) {
    if (K.state == KS_OFF) return;
    char game[sizeof(K.game_id)], region[sizeof(K.region)];
    snprintf(game, sizeof(game), "%s", K.game_id);
    snprintf(region, sizeof(region), "%s", K.region);
    krunker_net_join(client, game, region);
}

int krunker_net_send_packet(const NetPacketType type, const void *payload, const uint16_t length) {
    if (K.state != KS_OPEN) return 0;
    client_net_log_packet("=>", type, payload, length);
    pthread_mutex_lock(&K.lock);
    const int sent = krunker_link_send_ours(&K.link, type, payload, length);
    pthread_mutex_unlock(&K.lock);
    return sent;
}

void krunker_net_tick(Client *client) {
    krunker_captcha_tick(); // the matchmaker's Turnstile modal (the localhost callback natively)
    if (K.leave_unsupported && K.state != KS_OFF) {
        // init named a map outside our catalogue (krunker_link on_init): the game cannot be shown, so
        // the session ends on the error screen saying which map, instead of a loading screen forever
        K.leave_unsupported = 0;
        char index[16], text[sizeof(K.tpl.map) + 16];
        pthread_mutex_lock(&K.lock);
        snprintf(index, sizeof(index), "%d", K.link.unsupported_map);
        pthread_mutex_unlock(&K.lock);
        i18n_replace(text, sizeof(text), K.tpl.map, 1, I18N_ARGV(index));
        printf("[krunker] the game plays map index %s, not in this client's catalogue: leaving\n", index);
        krunker_net_leave(client ? client : K.client, text);
        ui_notify(text);
        return;
    }
    if (K.leave_closed && K.state != KS_OFF) {
        // "00" UPDATE_NAMES without keepOpen (game.deob.js:220322): the live client closes the socket
        // itself; ours leaves the game the same way, with the session's end as the reason
        K.leave_closed = 0;
        printf("[krunker] the server ended the session (UPDATE_NAMES): closing the socket\n");
        krunker_net_leave(client ? client : K.client, K.tpl.closed);
        return;
    }
    if (K.pow_result) { // native: the socket thread answered a "cpt"
        const int solved = K.pow_result == 1;
        K.pow_result = 0;
        krunker_captcha_pow_end(solved);
    }
    if (K.altcha_pending && !K.pow_shown) { // web: "verifying…" while the slices run
        K.pow_shown = 1;
        krunker_captcha_pow_begin();
    }
    if (K.altcha_pending) {
        char *payload = NULL;
        const int rc = K.state == KS_OPEN ? krunker_altcha_step(&K.altcha, KRUNKER_ALTCHA_SLICE, &payload) : -1;
        if (rc != 0) {
            K.altcha_pending = K.pow_shown = 0;
            krunker_captcha_pow_end(rc == 1);
        }
        if (rc == 1) {
            const KrunkerValue args[] = {KRUNKER_V_STR(payload)};
            pthread_mutex_lock(&K.lock);
            if (krunker_link_send(&K.link, krunker_c2s_wire(KRUNKER_C2S_cptR), args, 1)) K.link.captchas_solved++;
            pthread_mutex_unlock(&K.lock);
            printf("[krunker] security challenge answered (%s proof of work, number %ld)\n", K.altcha.algorithm, K.altcha.next);
        } else if (rc < 0 && K.state == KS_OPEN) {
            set_notice(1, "%s", K.tpl.pow);
        }
        free(payload);
    }
    if (!K.notice_pending) return;
    pthread_mutex_lock(&K.lock);
    char text[sizeof(K.notice)];
    snprintf(text, sizeof(text), "%s", K.notice);
    const int captcha = K.captcha_pending;
    K.notice_pending = K.captcha_pending = 0;
    pthread_mutex_unlock(&K.lock);
    if (captcha) {
        // a "cpt" this client cannot answer (not SHA-256 ALTCHA, or too big): the Security Challenge
        // modal shows it failed, the toast says what (the session goes on until the server decides)
        krunker_captcha_pow_end(0);
    }
    char html[sizeof(text) * 2];
    size_t o = 0; // the toast takes HTML: escape the server's text
    for (const char *p = text; *p && o + 6 < sizeof(html); p++) {
        if (*p == '<') o += (size_t) snprintf(html + o, sizeof(html) - o, "&lt;");
        else if (*p == '>') o += (size_t) snprintf(html + o, sizeof(html) - o, "&gt;");
        else if (*p == '&') o += (size_t) snprintf(html + o, sizeof(html) - o, "&amp;");
        else html[o++] = *p;
    }
    html[o] = 0;
    ui_notify(html);
}
