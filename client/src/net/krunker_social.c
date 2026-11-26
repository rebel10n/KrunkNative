// The hub page's krunker.io social socket (client/include/krunker_social.h): the connect of hub-10.0.7.js:7020-7110
// (the FRVR token, the /ws/ping pre-auth token, the URL), the 20 s PAT_REFRESH, the LOGIN rpc and the "cpt" challenge,
// over krunker_ws.h with the framing of krunker_social_wire.c.
#include <krunker_social.h>
#include <krunker_account.h>
#include <krunker_build.h>
#include <krunker_ws.h>
#include <backend.h>
#include <html_dom.h> // html_storage_get: the page's saved testCode
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PING_TIMEOUT_MS 8000
#define PAT_REFRESH_S 20.0         // setInterval(..., 20000)
#define CAPTCHA_BUDGET 50000L      // numbers searched per frame: the frame loop never stalls on the proof of work
#define QUEUE_MAX 64
#define URL_MAX 6144

typedef enum { ST_OFF, ST_TOKEN, ST_PING, ST_CONNECTING, ST_OPEN } State;

static struct {
    KrunkerWs *ws;
    KrunkerSocialWire wire;
    State state;
    unsigned gen;          // a close / reopen makes older callbacks stale
    char at[4096];         // the FRVR token while the URL is built
    double next_pat;       // the next PAT_REFRESH (monotonic seconds; 0 = none)
    int pat_pending;       // a /ws/ping for PAT_REFRESH in flight
    cJSON *queue[QUEUE_MAX]; // the sendQueue: frames sent before the socket opened
    int queued;
    int kio_signed_in;     // the krunker.io account's sign-in state last seen (the LOGIN listener)
    int login_pending;
} S;

static void wipe(void *p, size_t n) {
    volatile unsigned char *v = p;
    while (n--) *v++ = 0;
}

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec + ts.tv_nsec / 1e9;
}

static void *gen_tag(void) { return (void *) (uintptr_t) S.gen; }
static int stale(void *user) { return (unsigned) (uintptr_t) user != S.gen; }

// SOCIAL_URI without a trailing slash
static void social_base(char *out, const size_t n) {
    krunker_service_url(KRUNKER_SVC_SOCIAL, "", out, n);
    size_t len = strlen(out);
    while (len && out[len - 1] == '/') out[--len] = 0;
}

static void queue_clear(void) {
    for (int i = 0; i < S.queued; i++) cJSON_Delete(S.queue[i]);
    S.queued = 0;
}

static int send_buf(const KrunkerBuf *b) { return b->data && krunker_ws_send(S.ws, b->data, b->len, 1); }

static int send_now(const cJSON *frame) {
    KrunkerBuf buf = {0};
    const int ok = krunker_social_wire_encode(&S.wire, frame, &buf) == KRUNKER_PROTO_OK && send_buf(&buf);
    krunker_buf_free(&buf);
    return ok;
}

// ---------------------------------------------------------------- connect: the token, /ws/ping, the URL

// iíîíïiî: the pre-auth token of GET SOCIAL_URI/ws/ping ({token}); NULL when the fetch fails or answers non-2xx
static const char *ping_token(const int status, const char *body, const size_t length, cJSON **owner) {
    *owner = status >= 200 && status < 300 && body ? cJSON_ParseWithLength(body, length) : NULL;
    if (status && (status < 200 || status >= 300)) fprintf(stderr, "[krunker-social] Pre-auth failed: %d\n", status);
    const char *t = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(*owner, "token"));
    return t && t[0] ? t : NULL;
}

static void on_connect_ping(void *user, const int status, const char *body, const size_t length) {
    if (stale(user) || S.state != ST_PING) return;
    cJSON *owner = NULL;
    const char *pat = ping_token(status, body, length, &owner);
    char base[512], tc[256] = "", url[URL_MAX];
    social_base(base, sizeof(base));
    if (html_storage_get("testCode", tc, sizeof(tc)) < 0) tc[0] = 0;
    // https:// -> wss://, http:// -> ws:// (the socket's connect strips the scheme and takes the page's)
    const char *host = !strncmp(base, "https://", 8) ? base + 8 : !strncmp(base, "http://", 7) ? base + 7 : base;
    const char *scheme = !strncmp(base, "http://", 7) ? "ws://" : "wss://";
    int n = snprintf(url, sizeof(url), "%s%s/ws?", scheme, host);
    const char *parts[][2] = {{"tc", tc}, {"at", S.at}, {"pat", pat}};
    int first = 1;
    for (size_t i = 0; i < 3 && n > 0 && (size_t) n < sizeof(url); i++) {
        if (!parts[i][1] || !parts[i][1][0]) continue;
        n += snprintf(url + n, sizeof(url) - (size_t) n, "%s%s=%s", first ? "" : "&", parts[i][0], parts[i][1]);
        first = 0;
    }
    wipe(S.at, sizeof(S.at));
    cJSON_Delete(owner);
    printf("[krunker-social] connecting to %s/ws (%s%s%s)\n", host, tc[0] ? "tc, " : "", pat ? "pat" : "no pat",
           strstr(url, "at=") ? ", at" : ", no at");
    krunker_social_wire_init(&S.wire);
    S.state = ST_CONNECTING;
    if (n <= 0 || (size_t) n >= sizeof(url) || !krunker_ws_open(S.ws, url)) {
        S.state = ST_OFF; // tick reports the close
        S.next_pat = -1;
    }
    wipe(url, sizeof(url));
}

static void on_connect_token(void *user, const char *token) {
    if (stale(user) || S.state != ST_TOKEN) return;
    snprintf(S.at, sizeof(S.at), "%s", token ? token : "");
    if (!token) fprintf(stderr, "[krunker-social] no access token: the socket opens without at=\n");
    char base[512], url[600];
    social_base(base, sizeof(base));
    snprintf(url, sizeof(url), "%s/ws/ping", base);
    S.state = ST_PING;
    if (!backend_http_get_remote(url, PING_TIMEOUT_MS, on_connect_ping, gen_tag())) on_connect_ping(gen_tag(), 0, NULL, 0);
}

int krunker_social_open(void) {
    krunker_social_close();
    if (!S.ws) S.ws = krunker_ws_new("krunker-social");
    if (!S.ws) return 0;
    S.state = ST_TOKEN;
    S.kio_signed_in = krunker_account_signed_in();
    krunker_account_frvr_token(on_connect_token, gen_tag()); // FRVR.auth.getFreshAccessToken()
    return 1;
}

void krunker_social_close(void) {
    S.gen++;
    if (S.ws) krunker_ws_close(S.ws);
    S.state = ST_OFF;
    S.next_pat = 0;
    S.pat_pending = S.login_pending = 0;
    wipe(S.at, sizeof(S.at));
    queue_clear();
    krunker_social_wire_init(&S.wire);
}

int krunker_social_is_open(void) { return S.state == ST_OPEN && krunker_ws_is_open(S.ws); }
int krunker_social_captcha_shown(void) { return S.state == ST_OPEN && S.wire.captcha_shown; }

int krunker_social_send(const cJSON *frame) {
    if (!cJSON_IsArray(frame)) return 0;
    if (krunker_social_is_open()) return send_now(frame);
    if (S.state == ST_OFF || S.queued >= QUEUE_MAX) return 0;
    S.queue[S.queued++] = cJSON_Duplicate(frame, 1); // sendQueue.push: sent on open, ahNum rotated then
    return 1;
}

int krunker_social_rpc(const char *name, const cJSON *args) {
    if (!krunker_social_is_open()) return 0;
    KrunkerBuf buf = {0};
    const int id = krunker_social_wire_rpc(&S.wire, name, args, &buf);
    const int ok = id >= 0 && send_buf(&buf);
    if (buf.data) wipe(buf.data, buf.len); // LOGIN carries the token
    krunker_buf_free(&buf);
    return ok;
}

// ---------------------------------------------------------------- PAT_REFRESH and LOGIN

static void on_pat_ping(void *user, const int status, const char *body, const size_t length) {
    if (stale(user)) return;
    S.pat_pending = 0;
    cJSON *owner = NULL;
    const char *pat = ping_token(status, body, length, &owner);
    const char *wire = krunker_c2s_wire(KRUNKER_C2S_PAT_REFRESH); // NULL on a build that dropped it (OPTIONAL_C2S)
    if (pat && wire && krunker_social_is_open()) { // iïìïìîì.send(PAT_REFRESH, token) while connected
        cJSON *f = cJSON_CreateArray();
        cJSON_AddItemToArray(f, cJSON_CreateString(wire));
        cJSON_AddItemToArray(f, cJSON_CreateString(pat));
        send_now(f);
        cJSON_Delete(f);
    }
    cJSON_Delete(owner);
}

static void on_login_token(void *user, const char *token) {
    if (stale(user)) return;
    S.login_pending = 0;
    if (!token || !krunker_social_is_open()) return;
    cJSON *args = cJSON_CreateArray();
    cJSON_AddItemToArray(args, cJSON_CreateString(token));
    if (krunker_social_rpc(krunker_c2s_wire(KRUNKER_C2S_LOGIN), args)) printf("[krunker-social] rpc LOGIN (the account signed in)\n");
    cJSON *t = cJSON_GetArrayItem(args, 0);
    if (t && t->valuestring) wipe(t->valuestring, strlen(t->valuestring));
    cJSON_Delete(args);
}

// ---------------------------------------------------------------- tick

typedef struct {
    KrunkerSocialFn fn;
    void *user;
    unsigned gen;
} Poll;

static void emit(const Poll *p, const KrunkerSocialEvent ev, const cJSON *frame, const char *text) {
    if (p->fn && p->gen == S.gen) p->fn(p->user, ev, frame, text);
}

static void closed(const Poll *p) {
    S.state = ST_OFF;
    S.next_pat = 0;
    queue_clear();
    emit(p, KRUNKER_SOCIAL_CLOSED, NULL, NULL);
}

static void on_ws(void *user, const int event, const void *data, const size_t len, const int binary, const int code) {
    const Poll *p = user;
    if (p->gen != S.gen) return; // the page closed the socket from an earlier event
    if (event == KRUNKER_WS_OPEN) {
        S.state = ST_OPEN;
        S.next_pat = now_s() + PAT_REFRESH_S;
        emit(p, KRUNKER_SOCIAL_OPEN, NULL, NULL);
        // then the sendQueue, in order (each frame takes its ahNum now)
        for (int i = 0; i < S.queued && p->gen == S.gen; i++) send_now(S.queue[i]);
        queue_clear();
    } else if (event == KRUNKER_WS_MESSAGE) {
        if (!binary) return; // binaryType arraybuffer: a string frame fails the decoder
        cJSON *frame = NULL;
        const KrunkerSocialDecode rc = krunker_social_wire_decode(&S.wire, data, len, &frame);
        if (rc == KSW_PAGE) {
            emit(p, KRUNKER_SOCIAL_FRAME, frame, NULL);
        } else if (rc == KSW_CAPTCHA) {
            printf("[krunker-social] security challenge (%s, up to %ld): solving\n", S.wire.altcha.algorithm, S.wire.altcha.max);
        } else if (rc == KSW_CAPTCHA_FAIL) {
            fprintf(stderr, "[krunker-social] a security challenge this client cannot answer\n");
            emit(p, KRUNKER_SOCIAL_ERROR, NULL, "CAPTCHA FAILED");
            if (p->gen == S.gen) {
                krunker_ws_close(S.ws);
                closed(p);
            }
        }
        cJSON_Delete(frame);
    } else if (event == KRUNKER_WS_CLOSED) {
        fprintf(stderr, "[krunker-social] closed (%d)\n", code);
        closed(p);
    }
}

void krunker_social_tick(const int signed_in, const KrunkerSocialFn fn, void *user) {
    const Poll p = {fn, user, S.gen};
    if (S.state == ST_OFF) {
        if (S.next_pat < 0) { // krunker_ws_open refused: the connect callback's error
            S.next_pat = 0;
            emit(&p, KRUNKER_SOCIAL_CLOSED, NULL, NULL);
        }
        return;
    }
    if (S.ws) krunker_ws_poll(S.ws, on_ws, (void *) &p);
    if (p.gen != S.gen || S.state != ST_OPEN) return;
    if (S.wire.solving) {
        KrunkerBuf buf = {0};
        const int rc = krunker_social_wire_captcha_step(&S.wire, CAPTCHA_BUDGET, &buf);
        if (rc == 1) {
            send_buf(&buf);
            printf("[krunker-social] security challenge answered\n");
        } else if (rc < 0) {
            emit(&p, KRUNKER_SOCIAL_ERROR, NULL, "CAPTCHA FAILED");
            if (p.gen == S.gen) {
                krunker_ws_close(S.ws);
                closed(&p);
            }
        }
        krunker_buf_free(&buf);
        if (p.gen != S.gen || S.state != ST_OPEN) return;
    }
    if (S.next_pat > 0 && now_s() >= S.next_pat) {
        S.next_pat += PAT_REFRESH_S;
        if (!S.pat_pending) {
            char base[512], url[600];
            social_base(base, sizeof(base));
            snprintf(url, sizeof(url), "%s/ws/ping", base);
            S.pat_pending = 1;
            if (!backend_http_get_remote(url, PING_TIMEOUT_MS, on_pat_ping, gen_tag())) S.pat_pending = 0;
        }
    }
    // the auth listener (hub-10.0.7.js:7035-7039): an account signed in while connected and none shown yet
    // (a sign-in, not a token refresh: the at= fetch itself refreshes the token)
    const int kio = krunker_account_signed_in();
    const int signed_now = kio && !S.kio_signed_in;
    S.kio_signed_in = kio;
    if (signed_now && !signed_in && !S.login_pending) {
        S.login_pending = 1;
        krunker_account_fresh_token(on_login_token, gen_tag());
    }
}
