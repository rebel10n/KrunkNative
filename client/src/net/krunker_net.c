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
#include <recent_lobbies.h>
#include <seek_prefs.h>
#include <krunker_account.h>
#include <krunker_map_source.h>
#include <backend.h>
#include <html_ui.h>
#include <ui_logic.h>
#include <join_errors.h> // a failed join's text and place (join_errors.c)
#include <cJSON.h>
#include <GLFW/glfw3.h> // glfwGetTime: the sync trace's second
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
// Find New Game through the matchmaker (krunker_net_seek_new): krunker.io's seek-game takes ONE
// excludeGame, so an answer that is another recent game is excluded in turn, at most this many
// times more, before the best answer so far is taken (recent_lobbies.h)
#define KRUNKER_SEEK_NEW_RETRIES 2
#define KRUNKER_ACCESS_MAX 4100 // an access token (JWT) + NUL

typedef enum {
    KS_OFF = 0,    // not in an official game
    KS_ACCOUNT,    // the krunker.io account's fresh access token (krunker_account.h; a refresh may run)
    KS_LIST,       // Find New Game: GET /game-list, to pick a game this client was not in lately
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
    // Find New Game (krunker_net_seek_new): the seek's excludeGame, the answers re-sought so far, and
    // the best of them (the least recent, rank as recent_lobbies_rank) to fall back on
    int seek_new, seek_new_retries;
    char exclude[96], fallback[96];
    int fallback_rank;
    // a reload's join (krunker_net_reload_join): the game the player was in; gone or full -> one
    // seek without it (no excludeGame) and a notice
    int reload;
    RecentLobbies recent; // the official games joined lately, most recent first (main thread)
    char ws_url[1024 + KRUNKER_ACCESS_MAX];
    char ws_log[1024];   // ws_url without the account's `at=`: what the log shows
    // the krunker.io account's access token for this join (krunker_account_fresh_token), "" = a guest;
    // seek-game's accessToken and the socket's at=. Never logged; wiped on leave.
    char access[KRUNKER_ACCESS_MAX];
    volatile int account_event; // the socket thread saw "a" ACCOUNT_RESPONSE: krunker_net_tick tells krunker_account
    int connected;       // on_client_socket_connect was reported for this socket
    KrunkerLink link;
    pthread_mutex_t lock;
    // the main thread's pending error popup (krunker_net_tick)
    char notice[256];
    int notice_pending, captcha_pending;
    KrunkerAltcha altcha; // web: the "cpt" being solved in slices
    int altcha_pending;
    int pow_shown;           // web: krunker_captcha_pow_begin was told of the "cpt" being solved
    volatile int pow_result; // native: the socket thread's "cpt" outcome for the modal (1 solved, 2 not)
    // the matchmaker's Turnstile (game.deob.js seek, :147445-147467): prompts so far this join (the live
    // client gives up after 2), the player's token until the retry's seek-game takes it (then wiped),
    // and whether the last seek carried one (a CaptchaRequired after it = the token was refused)
    int captcha_attempts, captcha_sent;
    char captcha[KRUNKER_CAPTCHA_TOKEN_MAX + 1];
    // the player's texts the socket thread needs, looked up on the main thread per join (i18n is the
    // main thread's): {0} placeholders filled with i18n_replace where they are used
    struct {
        char error[160], pow[200], map[160], closed[160];
        char ws_failed[200], ws_denied[200], ws_unreachable[200];
    } tpl;
    volatile int leave_unsupported; // a map nothing can show (kmap_tick, or an init naming none): krunker_net_tick leaves
    volatile int leave_closed;      // the server told us to close ("00" without keepOpen): krunker_net_tick leaves the game
    // init's map (krunker_map_source.h): the socket thread hands each init over (map_next, link_map), the
    // main thread resolves it (kmap_tick) while everything the link emits after it waits in held, in order
    KrunkerLinkInit map_next;
    int map_next_set, map_gate;  // K.lock
    NetPacket *held;             // K.lock
    size_t held_count, held_cap;
    KrunkerMapRequest map_req;   // main thread
    NetInitPacket map_init;
    int map_req_active;
    char map_label[64], map_why[200]; // the map nothing could show, and why (krunker_net_tick's message)
    volatile int leave_error;       // the server's "error" / "dc" ended the session: krunker_net_tick leaves the game
    char leave_text[300];           // leave_error's reason for the error screen (K.lock)
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

static void wipe(void *p, size_t n) {
    volatile unsigned char *v = p;
    while (n--) *v++ = 0;
}

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

// KRUNKNATIVE_KRUNKER_TRACE / ?krunkerTrace (krunker_net.h): 0 off (unset, "0"), 1 every frame and the
// sync line ("1", anything else), 2 the sync line only ("sync")
static int trace_mode(void) {
    static int cached = -1;
    if (cached < 0) {
        char v[8];
        if (!option("KRUNKNATIVE_KRUNKER_TRACE", "krunkerTrace", v, sizeof(v)) || !strcmp(v, "0")) cached = 0;
        else cached = !strcmp(v, "sync") ? 2 : 1;
    }
    return cached;
}

static int trace_on(void) { return trace_mode() == 1; }

static void trace_frame(const char *dir, const uint8_t *data, const size_t len) {
    if (!trace_on()) return;
    KrunkerFrame f;
    if (krunker_proto_decode(data, len, 0, &f) != KRUNKER_PROTO_OK) {
        printf("[krunker] %s (%zu bytes, undecodable)\n", dir, len);
        return;
    }
    if (!strcmp(f.name, krunker_s2c_wire(KRUNKER_S2C_ACCOUNT_RESPONSE)) || !strcmp(f.name, krunker_s2c_wire(KRUNKER_S2C_NEW_TOKEN))) {
        // the account's answer / a new session token: tokens are never logged
        printf("[krunker] %s %s ah=%d (account data, not logged)\n", dir, f.name, f.ah_echo);
        krunker_frame_free(&f);
        return;
    }
    KrunkerBuf json = {0};
    const KrunkerValue args = KRUNKER_V_ARRAY(f.args, f.argc);
    krunker_value_to_json(&args, &json);
    printf("[krunker] %s %s ah=%d %.*s\n", dir, f.name, f.ah_echo, json.len > 600 ? 600 : (int) json.len, json.data ? (const char *) json.data : "");
    krunker_buf_free(&json);
    krunker_frame_free(&f);
}

// The main thread shows the pending notice in krunker_net_tick: an error popup the player stays in
// the game past. A failure that ends the join is not a notice: present_failure shows it on the Lost
// Connection screen. A join that works shows nothing (no toast): the loading screen and the game do.
static void set_popup_notice(const int captcha, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void set_popup_notice(const int captcha, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(K.notice, sizeof(K.notice), fmt, ap);
    va_end(ap);
    K.notice_pending = 1;
    if (captcha) K.captcha_pending = 1;
}

// A failure that ends the join (or the session): the Lost Connection screen (on_client_socket_failed),
// or, when the player is in a game and stays in it, v10's error popup over that game (join_errors.h).
static void present_failure(const char *text) {
    Client *client = K.client;
    if (join_error_place(client && client->net_connected) == JOIN_ERROR_POPUP) {
        set_popup_notice(0, "%s", text);
    } else if (client) {
        // final, like servers_ui.c fatal_error: no transport retries it (menu_poll_connection would
        // keep the spinner up), the error screen's Retry seeks again (krunker_net_reconnect)
        client->net_hold_retry = 1;
        on_client_socket_failed(client, text);
    }
}

// ---------------------------------------------------------------- link I/O

#define KRUNKER_HELD_MAX 8192 // packets kept while init's map loads (states beyond this are dropped)

// K.lock held (every link call is)
static void held_push(const NetPacket *packet) {
    if (K.held_count >= KRUNKER_HELD_MAX && packet->type == NET_PACKET_STATE) return;
    if (K.held_count == K.held_cap) {
        const size_t cap = K.held_cap ? K.held_cap * 2 : 64;
        NetPacket *grown = realloc(K.held, cap * sizeof(*grown));
        if (!grown) return;
        K.held = grown;
        K.held_cap = cap;
    }
    K.held[K.held_count++] = *packet;
}

static void link_emit(void *user, const NetPacket *packet) {
    (void) user;
    if (!K.client) return;
    if (K.map_gate) { // init's map is still loading (kmap_tick): after it, in order
        held_push(packet);
        return;
    }
    client_net_log_packet("<=", (NetPacketType) packet->type, packet->payload, packet->length);
    client_net_queue_packet(K.client, packet);
}

static void link_send(void *user, const uint8_t *frame, const size_t len);

static void kmap_fail(int index, const char *custom_id, const char *why);

static void link_event(void *user, const KrunkerLinkEvent kind, const char *text) {
    (void) user;
    printf("[krunker] %s%s\n", kind == KRUNKER_LINK_EV_CAPTCHA ? "captcha required " : "", text ? text : "");
    char out[300];
    switch (kind) {
        case KRUNKER_LINK_EV_CAPTCHA: // a "cpt" krunker_altcha cannot answer (not SHA-256 ALTCHA, too big)
            set_popup_notice(1, "%s", K.tpl.pow);
            break;
        case KRUNKER_LINK_EV_ERROR: // the session is over (v10: the error screen): krunker_net_tick leaves
            snprintf(K.leave_text, sizeof(K.leave_text), "%s", i18n_replace(out, sizeof(out), K.tpl.error, 1, I18N_ARGV(text ? text : "error")));
            K.leave_error = 1;
            break;
        case KRUNKER_LINK_EV_UNSUPPORTED: // an init naming no map: nothing to show, krunker_net_tick leaves
            kmap_fail(K.link.unsupported_map, NULL, text);
            break;
        case KRUNKER_LINK_EV_CLOSE: // "00" without keepOpen: the live client closes its socket; krunker_net_tick leaves
            K.leave_closed = 1;
            break;
        case KRUNKER_LINK_EV_JOINED: // no toast: the loading screen and then the game show the join
            break;
        case KRUNKER_LINK_EV_ACCOUNT: // "a": the account is signed in on this socket (krunker_net_tick tells krunker_account)
            K.account_event = 1;
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

// init (socket thread, K.lock held): the map is resolved on the main thread (kmap_tick); until then the
// link's packets wait (link_emit). A newer init supersedes one not picked up yet, and what was held for
// the older round goes.
static void link_map(void *user, KrunkerLinkInit *info) {
    (void) user;
    if (K.map_next_set) cJSON_Delete(K.map_next.custom);
    K.map_next = *info;
    info->custom = NULL;
    K.map_next_set = 1;
    K.map_gate = 1;
    K.held_count = 0;
}

// the map source's network (an index past krunker_maps.h's table): the client's HTTP, its proxy
static int kmap_fetch(const char *url, KrunkerMapFetchDone done, void *ctx) {
#ifdef __EMSCRIPTEN__
    return backend_http_request("GET", url, NULL, done, ctx); // gapi answers Access-Control-Allow-Origin: *
#else
    return backend_http_get_external(url, 20000, done, ctx);
#endif
}

// main thread: a new socket / leaving drops init's map state
static void kmap_reset(void) {
    if (K.map_req_active) krunker_map_request_cancel(&K.map_req);
    K.map_req_active = 0;
    pthread_mutex_lock(&K.lock);
    if (K.map_next_set) cJSON_Delete(K.map_next.custom);
    K.map_next.custom = NULL;
    K.map_next_set = K.map_gate = 0;
    K.held_count = 0;
    pthread_mutex_unlock(&K.lock);
}

// K.lock held
static void kmap_fail(const int index, const char *custom_id, const char *why) {
    if (custom_id && custom_id[0]) snprintf(K.map_label, sizeof(K.map_label), "%s", custom_id);
    else snprintf(K.map_label, sizeof(K.map_label), "%d", index);
    snprintf(K.map_why, sizeof(K.map_why), "%s", why && why[0] ? why : "unknown map");
    // nothing the server sends after an init we cannot play reaches the game: the link's packets are held
    // until krunker_net_tick leaves (krunker_net_leave -> kmap_reset drops them)
    K.map_gate = 1;
    K.leave_unsupported = 1;
}

// main thread, every frame: resolve init's map; when it is ready, it joins the client's map list and
// INIT (with that slot) goes out, then what the link held meanwhile
static void kmap_tick(Client *client) {
    KrunkerLinkInit next;
    int have_next = 0;
    pthread_mutex_lock(&K.lock);
    if (K.map_next_set) {
        next = K.map_next;
        K.map_next.custom = NULL;
        K.map_next_set = 0;
        have_next = 1;
    }
    pthread_mutex_unlock(&K.lock);
    if (have_next) {
        if (K.map_req_active) krunker_map_request_cancel(&K.map_req);
        krunker_map_request_begin(&K.map_req, next.map_index, next.custom, next.custom_id);
        K.map_init = next.init;
        K.map_req_active = 1;
    }
    if (!K.map_req_active || !client) return;
    const cJSON *json = NULL;
    char why[200];
    const KrunkerMapStatus status = krunker_map_request_poll(&K.map_req, &json, why, sizeof(why));
    if (status == KRUNKER_MAP_PENDING) return;
    K.map_req_active = 0;
    const int index = K.map_req.index;
    if (status == KRUNKER_MAP_READY) {
        const int slot = client_map_list_add(client, json);
        if (slot >= 0) {
            const char *name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, "name"));
            printf("[krunker] map %d%s%s: %s (map slot %d)\n", index, K.map_req.custom_id[0] ? ", custom " : "", K.map_req.custom_id,
                   name ? name : "?", slot);
            pthread_mutex_lock(&K.lock);
            if (!K.map_next_set) { // (a newer init that came meanwhile keeps the gate: its own map is next)
                K.map_init.map_index = slot;
                NetPacket packet;
                memset(&packet, 0, sizeof(packet));
                packet.type = NET_PACKET_INIT;
                packet.length = sizeof(K.map_init);
                memcpy(packet.payload, &K.map_init, sizeof(K.map_init));
                K.map_gate = 0;
                link_emit(NULL, &packet);
                for (size_t i = 0; i < K.held_count; i++) link_emit(NULL, &K.held[i]);
                K.held_count = 0;
            }
            pthread_mutex_unlock(&K.lock);
            return;
        }
        snprintf(why, sizeof(why), "out of memory");
    }
    printf("[krunker] map %d%s%s cannot be shown: %s\n", index, K.map_req.custom_id[0] ? ", custom " : "", K.map_req.custom_id, why);
    pthread_mutex_lock(&K.lock);
    K.link.unsupported_map = index;
    K.held_count = 0;
    kmap_fail(index, K.map_req.custom_id, why);
    pthread_mutex_unlock(&K.lock);
}

static void link_setup(void) {
    const KrunkerLinkIO io = {.emit = link_emit, .send = link_send, .event = link_event, .cheat = link_cheat, .altcha = link_altcha,
                              .map = link_map, .user = NULL};
    krunker_map_source_set_fetch(kmap_fetch);
    krunker_link_init(&K.link, &io, KRUNKER_AHKEY);
}

// ---------------------------------------------------------------- socket state -> client

static void socket_opened(void) {
    Client *client = K.client;
    pthread_mutex_lock(&client->net_lock);
    client->net_socket = KRUNKER_FAKE_FD;
    pthread_mutex_unlock(&client->net_lock);
    K.connected = 1;
    printf("[krunker] connected to %s\n", K.ws_log);
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
        present_failure(message);
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
    present_failure(message);
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
    fprintf(stderr, "[krunker] websocket error (%s)\n", K.ws_log);
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
        fail("Could not open a WebSocket to %s", K.ws_log);
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
    char req[2048 + KRUNKER_ACCESS_MAX]; // (the target carries the account's at=)
    // Origin: the page Krunker's game servers serve (a browser sends its page origin)
    const int len = snprintf(req, sizeof(req),
                             "GET %s HTTP/1.1\r\nHost: %s%s%s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                             "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\nOrigin: https://krunker.io\r\n"
                             "User-Agent: KrunkNative\r\n\r\n",
                             target, a->host, default_port ? "" : ":", default_port ? "" : a->port, key);
    *status_out = 0;
    const int sent = len > 0 && (size_t) len < sizeof(req) && http_conn_send(c, req, (size_t) len);
    wipe(req, sizeof(req));
    if (!sent) return 0;
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
    char target[sizeof(K.ws_url)];
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
    snprintf(K.tpl.error, sizeof(K.tpl.error), "%s", I18N("kn.kio.serverError", "krunker.io: {0}"));
    snprintf(K.tpl.pow, sizeof(K.tpl.pow), "%s",
             I18N("kn.kio.powUnsupported", "krunker.io's game server asked for a proof of work in a form this client cannot compute yet: it may close the game."));
    snprintf(K.tpl.map, sizeof(K.tpl.map), "%s", I18N("kn.kio.mapMissing", "Map {0} of this official game could not be loaded ({1}): pick another lobby."));
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
    static char enc[(KRUNKER_CAPTCHA_TOKEN_MAX > KRUNKER_ACCESS_MAX ? KRUNKER_CAPTCHA_TOKEN_MAX : KRUNKER_ACCESS_MAX) * 3 + 1];
    form_encode(enc, sizeof(enc), value);
    const size_t len = strlen(url);
    snprintf(url + len, n - len, "%s%s=%s", strchr(url, '?') ? "&" : "?", key, enc);
    wipe(enc, sizeof(enc));
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
static void seek(void);

static void seek(void) {
    // matchmakerSeek (game.deob.js:147610-147631): hostname, region, autoChangeGame,
    // validationToken, then game / excludeGame / dataQuery / captchaToken / accessToken when set
    // (null ones are dropped, matchmakerRequest :147647). The first seek has no validationToken
    // (iïîìîíì passes undefined for a guest); only the captcha retry fetches one (:147466).
    static char url[32768];
    snprintf(url, sizeof(url), "%s/seek-game", K.matchmaker);
    append_param(url, sizeof(url), "hostname", "krunker.io");
    append_param(url, sizeof(url), "region", K.region);
    append_param(url, sizeof(url), "autoChangeGame", "false");
    // the log shows where the validation token goes, never its value (like the captcha / access tokens)
    size_t token_at = 0, token_end = 0;
    if (K.token[0]) {
        token_at = strlen(url) + sizeof("&validationToken=") - 1;
        append_param(url, sizeof(url), "validationToken", K.token);
        token_end = strlen(url);
    }
    if (K.game_id[0]) append_param(url, sizeof(url), "game", K.game_id);
    else if (K.exclude[0]) append_param(url, sizeof(url), "excludeGame", K.exclude); // backToOrigin's ?exclude= (:141624, :222302)
    if (K.seek_new && !K.game_id[0]) {
        // iïîìîíì: {v: buildVersion} plus the saved seek_maps / seek_modes as dsma / dsmo (:222271-222275)
        char dq[160];
        int n = snprintf(dq, sizeof(dq), "{\"v\":\"%s\"", KRUNKER_BUILD_VERSION);
        if (seek_prefs_map() >= 0) n += snprintf(dq + n, sizeof(dq) - (size_t) n, ",\"dsma\":[%d]", seek_prefs_map());
        if (seek_prefs_mode() >= 0) n += snprintf(dq + n, sizeof(dq) - (size_t) n, ",\"dsmo\":[%d]", seek_prefs_mode());
        snprintf(dq + n, sizeof(dq) - (size_t) n, "}");
        append_param(url, sizeof(url), "dataQuery", dq);
    } else {
        append_param(url, sizeof(url), "dataQuery", "{\"v\":\"" KRUNKER_BUILD_VERSION "\"}"); // iïîìîíì: {v: buildVersion}
    }
    const size_t logged = strlen(url); // the log stops before the captcha and access tokens: never logged
    K.captcha_sent = K.captcha[0] != 0;
    if (K.captcha_sent) append_param(url, sizeof(url), "captchaToken", K.captcha);
    // the krunker.io account (krunker_account.h): matchmakerSeek's last parameter (:147630)
    if (K.access[0]) append_param(url, sizeof(url), "accessToken", K.access);
    K.state = KS_SEEK;
    if (token_end) printf("[krunker] seek-game: %.*s(redacted)%.*s", (int) token_at, url, (int) (logged - token_end), url + token_end);
    else printf("[krunker] seek-game: %.*s", (int) logged, url);
    printf("%s%s\n", K.captcha_sent ? "&captchaToken=(the player's Turnstile token)" : "", K.access[0] ? "&accessToken=(the krunker.io account's)" : "");
    const int queued = http_get(url, on_seek); // (the request keeps its own copy)
    wipe(K.captcha, sizeof(K.captcha));          // single-use: never sent twice
    wipe(url, sizeof(url));
    if (!queued) fail_unreachable();
}

// Find New Game's lobby list (fetchGameList, game.deob.js :147591-147599): the busiest public game
// with room in the region, for the seek preferences, that this client was not in lately. None (or
// no list): the matchmaker picks, excluding the game the player leaves (seek(): excludeGame).
static void on_game_list(void *user, const int status, const char *body, const size_t length) {
    (void) length;
    if (stale(user) || K.state != KS_LIST) return;
    cJSON *res = status == 200 && body ? cJSON_Parse(body) : NULL;
    const cJSON *g, *best = NULL;
    double best_players = -1;
    int listed = 0;
    cJSON_ArrayForEach(g, cJSON_GetObjectItemCaseSensitive(res, "games")) {
        // [id, region, players, max, data{i map, g mode, c custom, pw password}, timeLeft]
        const char *id = cJSON_GetStringValue(cJSON_GetArrayItem(g, 0));
        const char *region = cJSON_GetStringValue(cJSON_GetArrayItem(g, 1));
        const cJSON *players = cJSON_GetArrayItem(g, 2), *max = cJSON_GetArrayItem(g, 3), *data = cJSON_GetArrayItem(g, 4);
        if (!id || !id[0] || strlen(id) >= sizeof(K.game_id) || !cJSON_IsNumber(players) || !cJSON_IsNumber(max)) continue;
        listed++;
        if (players->valuedouble >= max->valuedouble || cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(data, "c")) ||
            cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(data, "pw")) || !region || strcmp(region, K.region))
            continue;
        const cJSON *mode = cJSON_GetObjectItemCaseSensitive(data, "g");
        if (!seek_prefs_match(cJSON_IsNumber(mode) ? mode->valueint : -1, cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(data, "i"))))
            continue;
        if (recent_lobbies_rank(&K.recent, id) || players->valuedouble <= best_players) continue;
        best = g;
        best_players = players->valuedouble;
    }
    if (best) {
        snprintf(K.game_id, sizeof(K.game_id), "%s", cJSON_GetStringValue(cJSON_GetArrayItem(best, 0)));
        printf("[krunker] game-list: %s (%.0f players), not one of the recent games\n", K.game_id, best_players);
    } else {
        printf("[krunker] game-list: %s: the matchmaker picks (excludeGame %s)\n",
               res ? (listed ? "no other game with room in the region" : "no games") : "unavailable", K.exclude[0] ? K.exclude : "none");
    }
    cJSON_Delete(res);
    seek();
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
        else // the player closed it: KN's sentence says what they did (krunker_net.c's e2e pins it; v10's CaptchaRequired
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
            // v10 gives up after the second refused token with the same CaptchaRequired (v10.js:147444-147450)
            char text[300];
            printf("[krunker] the security challenge's token was refused %d times\n", K.captcha_attempts);
            join_error_text("CaptchaRequired", text, sizeof(text));
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
    } else if (K.reload && K.game_id[0] && error && (!strcmp(error, "InvalidGameId") || !strcmp(error, "GameFull"))) {
        // a reload's game is gone or full: say so, then the matchmaker picks one in the region
        // (Find New Game's seek, with nothing to exclude: the player left no game)
        const int full = !strcmp(error, "GameFull");
        printf("[krunker] reload: official game %s is %s: seeking another game\n", K.game_id, full ? "full" : "gone");
        ui_notify(full ? I18N("kn.reload.lobbyFull", "The game you were in is full: joining another one")
                       : I18N("kn.reload.lobbyGone", "The game you were in has ended: joining another one"));
        K.reload = 0;
        K.game_id[0] = 0;
        K.exclude[0] = 0;
        K.seek_new = 1;
        K.seek_new_retries = 0;
        cJSON_Delete(res);
        seek();
        return;
    } else if (status != 200 || error || !host || !game || !client_id) {
        char detail[200], text[300];
        // the matchmaker's own error word, else the start of its body on one line (the log)
        snprintf(detail, sizeof(detail), "%.120s", error ? error : body && body[0] ? body : "(no reason given)");
        for (char *c = detail; *c; c++) if (*c == '\r' || *c == '\n' || *c == '\t') *c = ' ';
        printf("[krunker] seek-game refused (HTTP %d): %s\n", status, detail);
        join_error_text(error ? error : "GameIssue", text, sizeof(text));
        fail("%s", text);
    } else if (K.seek_new && !K.game_id[0] && recent_lobbies_rank(&K.recent, game) &&
               (K.seek_new_retries < KRUNKER_SEEK_NEW_RETRIES || (K.fallback[0] && strcmp(K.fallback, game) &&
                                                                  recent_lobbies_rank(&K.recent, K.fallback) > recent_lobbies_rank(&K.recent, game)))) {
        // Find New Game: the matchmaker answered a game this client was in lately (excludeGame keeps
        // only one out). Keep the least recent answer, and seek again excluding this one; out of
        // retries, that kept one (seek-game ?game=, its own fresh clientId)
        const int rank = recent_lobbies_rank(&K.recent, game);
        if (!K.fallback[0] || rank >= K.fallback_rank) {
            snprintf(K.fallback, sizeof(K.fallback), "%s", game);
            K.fallback_rank = rank;
        }
        if (K.seek_new_retries < KRUNKER_SEEK_NEW_RETRIES) {
            K.seek_new_retries++;
            snprintf(K.exclude, sizeof(K.exclude), "%s", game);
            printf("[krunker] seek-game: %s is a recent game: seeking again without it\n", game);
        } else {
            snprintf(K.game_id, sizeof(K.game_id), "%s", K.fallback);
            printf("[krunker] seek-game: only recent games have room: %s\n", K.fallback);
        }
        cJSON_Delete(res);
        seek();
        return;
    } else {
        K.reload = 0;
        snprintf(K.game_id, sizeof(K.game_id), "%s", game);
        recent_lobbies_push(&K.recent, game);
        char query[600];
        snprintf(query, sizeof(query), "gameId=%s&clientKey=%s", game, client_id); // no clientUID / bpt / tc / dm
        if (is_production() || !port_s[0]) {
            snprintf(K.ws_log, sizeof(K.ws_log), "wss://%s/ws?%s", host, query);
        } else {
            snprintf(K.ws_log, sizeof(K.ws_log), "%s://%s:%s/ws?%s", strncmp(K.matchmaker, "https:", 6) ? "ws" : "wss", host, port_s, query);
        }
        // the krunker.io account: "at=" + the fresh access token as it is (iîíîiìî :222367, after tc)
        snprintf(K.ws_url, sizeof(K.ws_url), "%s%s%s", K.ws_log, K.access[0] ? "&at=" : "", K.access);
        printf("[krunker] seek-game: game %s on %s:%s, opening %s%s\n", game, host, port_s, K.ws_log,
               K.access[0] ? "&at=(the krunker.io account's token)" : "");
        load_ws_texts();
        K.state = KS_CONNECTING;
        kmap_reset();
        pthread_mutex_lock(&K.lock);
        krunker_link_reset(&K.link);
        K.link.account = K.access[0] != 0; // "en" then carries the player's own loadout
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

// The krunker.io account's token for this join (krunker_account_fresh_token): NULL = as a guest.
// Then Find New Game's lobby list (seek_new 2), or seek-game at once.
static void on_access(void *user, const char *token) {
    if (stale(user) || K.state != KS_ACCOUNT) return;
    snprintf(K.access, sizeof(K.access), "%s", token && strlen(token) < sizeof(K.access) ? token : "");
    if (K.access[0]) printf("[krunker] joining with the krunker.io account %s\n", krunker_account_name());
    else if (krunker_account_signed_in()) printf("[krunker] the krunker.io account has no usable token: joining as a guest\n");
    if (K.seek_new == 2) {
        K.state = KS_LIST;
        char url[600];
        snprintf(url, sizeof(url), "%s/game-list?hostname=krunker.io", K.matchmaker);
        if (http_get(url, on_game_list)) return;
    }
    seek();
}

// krunker_net_join, or (seek_new) Find New Game: no game asked for, excludeGame = exclude; seek_new 2:
// the lobby list first (on_game_list).
static int join(Client *client, const char *game_id, const char *region, const int seek_new, const char *exclude, const int reload) {
    if (!client) return 0;
    if (!K.client) krunker_net_attach(client);
    if (K.state != KS_OFF) krunker_net_leave(client, NULL); // another official game: an intended switch
    // our own transports and the offline game make way (an intended switch, not a drop)
    client_net_release(client);
    if (client->game.player_count) client_clear_players(client);
    client->in_game = 0;
    client->menu_play_pending = 0;
    client->game.is_official = 1; // krunker.io's own lobby: the KrunkNative-only classes are out (game_class_allowed)

    K.generation++;
    snprintf(K.game_id, sizeof(K.game_id), "%s", game_id ? game_id : "");
    K.seek_new = seek_new;
    K.seek_new_retries = 0;
    K.reload = reload && K.game_id[0];
    K.fallback[0] = 0;
    K.fallback_rank = 0;
    snprintf(K.exclude, sizeof(K.exclude), "%s", exclude ? exclude : "");
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
    kmap_reset();
    K.leave_error = 0;
    wipe(K.captcha, sizeof(K.captcha));
    load_texts();
    matchmaker();
    wipe(K.access, sizeof(K.access));
    if (K.exclude[0]) printf("[krunker] finding a new official game (region %s, not %s) through %s\n", K.region, K.exclude, K.matchmaker);
    else printf("[krunker] joining official game %s (region %s) through %s\n", K.game_id[0] ? K.game_id : "(any)", K.region, K.matchmaker);
    // the live client's order (seek, :147412): the account's fresh access token first (a refresh when it
    // is about to expire), then on_access starts Find New Game's list or seek-game, no validation token;
    // generate-token only before a captcha retry (captcha_done)
    K.state = KS_ACCOUNT;
    krunker_account_fresh_token(on_access, (void *) (uintptr_t) K.generation);
    return K.state != KS_CLOSED;
}

int krunker_net_join(Client *client, const char *game_id, const char *region) {
    return join(client, game_id, region, 0, NULL, 0);
}

// Reload (game_reload.c): the official game the player was in, by its id (the copies: K.game_id may be
// the argument itself, and join() leaves the current session first)
int krunker_net_reload_join(Client *client, const char *game_id, const char *region) {
    char game[sizeof(K.game_id)], r[sizeof(K.region)];
    snprintf(game, sizeof(game), "%s", game_id ? game_id : "");
    snprintf(r, sizeof(r), "%s", region ? region : "");
    printf("[krunker] reload: rejoining official game %s\n", game[0] ? game : "(any)");
    return join(client, game, r[0] ? r : NULL, 0, NULL, 1);
}

// krunker.io's Find New Game (F4 / backToOrigin(false, {exclude: game}), game.deob.js :141618-141626,
// :222300-222308): a seek in the same region with excludeGame = the game the player is in (or was in
// last); recent ones the matchmaker still answers are re-sought (on_seek).
int krunker_net_seek_new(Client *client, const char *region, const int use_list) {
    char r[sizeof(K.region)] = "";
    if (region && region[0]) snprintf(r, sizeof(r), "%s", region);
    else if (K.region[0]) snprintf(r, sizeof(r), "%s", K.region); // the region the player is playing in
    return join(client, NULL, r[0] ? r : NULL, use_list ? 2 : 1, K.recent.count ? K.recent.ids[0] : NULL, 0);
}

int krunker_net_recent_rank(const char *game_id) { return recent_lobbies_rank(&K.recent, game_id); }

const char *krunker_net_game(void) { return K.state == KS_OPEN || K.state == KS_CONNECTING ? K.game_id : ""; }

// K.game_id outlives the session (KS_OFF keeps it): the session accessors check the state
const char *krunker_net_session_game(void) { return K.state != KS_OFF ? K.game_id : ""; }
const char *krunker_net_region(void) { return K.region; }

// krunker_lobbies.c opens the same link for the browser opt-out (KRUNKER_DEFAULT_SITE); ids are
// [A-Za-z0-9:_-] (krunker_lobbies.c id_ok), so they need no escaping
int krunker_net_invite_link(char *out, const size_t n) {
    const char *game = krunker_net_session_game();
    if (!out || !n || !game[0]) return 0;
    const char *site = getenv("KRUNKNATIVE_KRUNKER_URL");
    snprintf(out, n, "%s/?game=%s", site && site[0] ? site : "https://krunker.io", game);
    return 1;
}

void krunker_net_leave(Client *client, const char *reason) {
    if (K.state == KS_OFF) return;
    if (client && K.connected) {
        pthread_mutex_lock(&client->net_lock);
        if (!reason) {
            client->net_close_intended = 1;
        } else if (!client->net_disconnect_reason[0]) { // (the server's own "error" text and code stay: client_net_queue_packet)
            snprintf(client->net_disconnect_reason, sizeof(client->net_disconnect_reason), "%s", reason);
            snprintf(client->net_disconnect_code, sizeof(client->net_disconnect_code), "000000");
        }
        pthread_mutex_unlock(&client->net_lock);
    }
    printf("[krunker] leaving the official game%s%s\n", reason ? ": " : "", reason ? reason : "");
    K.state = KS_OFF;
    Client *owner = client ? client : K.client;
    if (owner) owner->game.is_official = 0; // back in KrunkNative's lobbies (the KN-only classes are playable again)
    K.altcha_pending = K.pow_shown = 0;
    kmap_reset();
    wipe(K.access, sizeof(K.access));
    if (!krunker_account_owns_captcha()) krunker_captcha_close(); // a pending Turnstile request ends with the session (not a sign-in's)
    wipe(K.captcha, sizeof(K.captcha));
    stop_socket(); // native: joins the thread, which reports the disconnect; web: the close callback does
    wipe(K.ws_url, sizeof(K.ws_url)); // (it carried the account's at=)
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

int krunker_net_inputs_paused(void) {
    if (K.state != KS_OPEN) return 0;
    pthread_mutex_lock(&K.lock);
    const int paused = krunker_link_inputs_paused(&K.link) || K.map_gate;
    pthread_mutex_unlock(&K.lock);
    return paused;
}

// KRUNKNATIVE_KRUNKER_TRACE=sync: one line a second while we have a player in the official game, the
// numbers that tell "the server does not take my inputs" from "it takes them and disagrees": "q"
// windows and input frames sent in that second (v10: ~tf windows a second, netRate frames), the last
// isn sent and the server's last ack ("l" inputSN; "behind" = isn - ack, a few windows at most when it
// keeps up, growing when it does not), "l" / "f" received, the corrections reconciliation made (above
// NET_RECON_CORRECTION_EPS) and the largest (units), the pending inputs, where the last "l" put us and
// where we are now, the locks (lm, lock, paused) and the CHEAT_CHECKs answered / failed so far.
static void trace_sync_tick(Client *client) {
    static double last_at;
    static unsigned q0, frames0, syncs0, failed0, recons0, corr0;
    static int primed;
    if (!trace_mode() || K.state != KS_OPEN || !client) return;
    const double now = glfwGetTime();
    if (primed && now - last_at < 1.0) return;
    pthread_mutex_lock(&K.lock);
    const unsigned q = K.link.q_sent, frames = K.link.q_frames, syncs = K.link.syncs, failed = K.link.sync_failed;
    const int isn = K.link.last_isn_sent, ack = K.link.last_ack, move_lock = K.link.move_lock, locked = K.link.locked;
    const int paused = krunker_link_inputs_paused(&K.link) || K.map_gate;
    const unsigned cc_ok = K.link.cc_answered, cc_bad = K.link.cc_failed;
    const float sx = K.link.sync_pos[0], sy = K.link.sync_pos[1], sz = K.link.sync_pos[2];
    pthread_mutex_unlock(&K.lock);
    const unsigned recons = client->net_recon_count, corr = client->net_recon_corrections;
    if (primed && client->me && client->me->active) {
        const double dt = now - last_at;
        const Player *me = client->me;
#define SINCE(v, v0) ((v) >= (v0) ? (v) - (v0) : (v)) // a new socket restarts the link's counters

        printf("[krunker] sync %.1fs: q %u (%u inputs) isn %d ack %d (%d behind) l %u f %u corr %u/%u max %.2f pending %zu "
               "srv %.2f %.2f %.2f me %.2f %.2f %.2f lm %d lock %d paused %d cc %u/%u\n",
               dt, SINCE(q, q0), SINCE(frames, frames0), isn, ack, isn >= 0 && ack >= 0 ? isn - ack : -1, SINCE(syncs, syncs0),
               SINCE(failed, failed0), SINCE(corr, corr0), SINCE(recons, recons0), (double) client->net_recon_error_peak, client->net_pending_count, (double) sx, (double) sy, (double) sz,
               (double) me->position.x, (double) me->position.y, (double) me->position.z, move_lock, locked, paused, cc_ok, cc_bad);
#undef SINCE
        fflush(stdout);
    }
    client->net_recon_error_peak = 0.0f;
    q0 = q, frames0 = frames, syncs0 = syncs, failed0 = failed, recons0 = recons, corr0 = corr;
    last_at = now;
    primed = 1;
}

int krunker_net_sync_info(char *out, const size_t n) {
    if (!out || !n) return 0;
    out[0] = 0;
    if (K.state == KS_OFF) return 0;
    pthread_mutex_lock(&K.lock);
    snprintf(out, n, "official: %u syncs (\"l\"), %u sync failures (\"f\"), rates snf %d tf %d sf %d, %d lobby config keys", K.link.syncs,
             K.link.sync_failed, K.link.send_hz, K.link.tick_hz, K.link.sync_hz, K.link.config_keys);
    pthread_mutex_unlock(&K.lock);
    return 1;
}

void krunker_net_tick(Client *client) {
    krunker_captcha_tick(); // the matchmaker's Turnstile modal (the localhost callback natively)
    if (K.account_event) { // the game server's "a": the account's name and level for the account UI
        K.account_event = 0;
        char name[sizeof(K.link.account_name)];
        pthread_mutex_lock(&K.lock);
        snprintf(name, sizeof(name), "%s", K.link.account_name);
        const int level = K.link.account_level;
        pthread_mutex_unlock(&K.lock);
        printf("[krunker] the game server signed in the krunker.io account %s (level %d)\n", name, level);
        if (K.access[0]) krunker_account_note_game(name, level);
    }
    if (K.state != KS_OFF) kmap_tick(client ? client : K.client);
    trace_sync_tick(client ? client : K.client);
    if (K.leave_unsupported && K.state != KS_OFF) {
        // init's map cannot be shown (not in krunker_maps.h's table, the cache or the API; or no map at all):
        // the session ends on the error screen saying which map and why, instead of a loading screen forever
        char label[sizeof(K.map_label)], why[sizeof(K.map_why)], text[sizeof(K.tpl.map) + sizeof(label) + sizeof(why)];
        pthread_mutex_lock(&K.lock);
        K.leave_unsupported = 0;
        snprintf(label, sizeof(label), "%s", K.map_label);
        snprintf(why, sizeof(why), "%s", K.map_why);
        pthread_mutex_unlock(&K.lock);
        i18n_replace(text, sizeof(text), K.tpl.map, 2, I18N_ARGV(label, why));
        printf("[krunker] leaving: %s\n", text);
        Client *leaving = client ? client : K.client;
        krunker_net_leave(leaving, text);
        if (leaving) on_client_socket_failed(leaving, text); // the session ended: the Lost Connection screen says why
        return;
    }
    if (K.leave_error && K.state != KS_OFF) {
        // the server's "error" (an idle kick, ...) or "dc": the session is over. The live client closes
        // its socket and shows the error screen (v10's gW); ours closes it now instead of waiting for
        // the server's close, which stops the game, and the Lost Connection screen says why (the
        // "error" packet's own text and code, client_net_queue_packet; else leave_text)
        K.leave_error = 0;
        char text[sizeof(K.leave_text)];
        pthread_mutex_lock(&K.lock);
        snprintf(text, sizeof(text), "%s", K.leave_text);
        pthread_mutex_unlock(&K.lock);
        printf("[krunker] the server ended the session (%s): closing the socket\n", text);
        krunker_net_leave(client ? client : K.client, text);
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
    if (K.altcha_pending && !K.pow_shown) { // web: the slices start (nothing is shown unless it fails)
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
            set_popup_notice(1, "%s", K.tpl.pow);
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
        // modal's passive card shows it failed, the popup says what (the session goes on until the server decides)
        krunker_captcha_pow_end(0);
    }
    ui_error_popup(text); // an error the player stays in the game past: v10's error popup (it escapes the text)
}
