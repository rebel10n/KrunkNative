// One-shot 127.0.0.1 callback for a Turnstile token the player's own userscript sends from krunker.io
// (client/include/krunker_captcha_cb.h; doc/v10/official-captcha.md). Main thread only.
#include <krunker_captcha_cb.h>
#include <krunker_captcha.h>
#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define CB_MAX_CONNS 4
#define CB_MAX_REQUEST 8192        // head + body
#define CB_CONN_TIMEOUT_MS 3000.0  // a connection that has not sent its request by then is dropped
#define CB_MAX_BAD_PATHS 16        // wrong secrets before the listener closes (someone is guessing)
#define CB_ORIGIN "https://krunker.io"

typedef struct {
    int fd;
    size_t len;
    double deadline;
    char buf[CB_MAX_REQUEST + 1];
} CbConn;

static struct {
    int fd; // -1 = none
    int port;
    char secret[33];
    double expires;
    int used, bad_paths;
    int have_token;
    char token[KRUNKER_CAPTCHA_TOKEN_MAX + 1];
    CbConn conns[CB_MAX_CONNS];
} CB = {.fd = -1};

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec * 1000.0 + (double) ts.tv_nsec / 1e6;
}

static void wipe(void *p, size_t n) {
    volatile unsigned char *v = p;
    while (n--) *v++ = 0;
}

static int random_secret(char *out) { // 32 hex chars from the kernel's CSPRNG
    unsigned char raw[16];
    const int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return 0;
    size_t got = 0;
    while (got < sizeof(raw)) {
        const ssize_t r = read(fd, raw + got, sizeof(raw) - got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        got += (size_t) r;
    }
    close(fd);
    if (got != sizeof(raw)) return 0;
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        out[i * 2] = hex[raw[i] >> 4];
        out[i * 2 + 1] = hex[raw[i] & 15];
    }
    out[32] = 0;
    wipe(raw, sizeof(raw));
    return 1;
}

static void conns_init(void) { // the static table's fds start at 0 (stdin): mark them unused once
    static int done;
    if (done) return;
    done = 1;
    for (int i = 0; i < CB_MAX_CONNS; i++) CB.conns[i].fd = -1;
}

static void conn_close(CbConn *c) {
    if (c->fd >= 0) close(c->fd);
    wipe(c->buf, c->len); // may hold a token
    c->fd = -1;
    c->len = 0;
}

void krunker_captcha_cb_stop(void) {
    conns_init();
    for (int i = 0; i < CB_MAX_CONNS; i++) conn_close(&CB.conns[i]);
    if (CB.fd >= 0) {
        close(CB.fd);
        printf("[krunker] captcha callback on 127.0.0.1:%d closed\n", CB.port);
    }
    CB.fd = -1;
    wipe(CB.secret, sizeof(CB.secret));
    wipe(CB.token, sizeof(CB.token));
    CB.have_token = CB.used = CB.bad_paths = 0;
}

static int ttl_ms(void) {
    const char *v = getenv("KRUNKNATIVE_KRUNKER_CAPTCHA_TTL_MS");
    const long t = v && *v ? strtol(v, NULL, 10) : 0;
    return t > 0 && t <= KRUNKER_CAPTCHA_CB_TTL_MS ? (int) t : KRUNKER_CAPTCHA_CB_TTL_MS;
}

int krunker_captcha_cb_start(char *url, const size_t n) {
    krunker_captcha_cb_stop();
    if (!random_secret(CB.secret)) return 0;
    // close-on-exec: the browser ui_open_browser starts must not inherit the listener (SOCK_CLOEXEC
    // is not on macOS: fcntl)
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // 127.0.0.1 only, never another interface
    a.sin_port = 0;                             // the OS picks a free port
    socklen_t al = sizeof(a);
    if (bind(fd, (struct sockaddr *) &a, sizeof(a)) != 0 || listen(fd, 4) != 0 || getsockname(fd, (struct sockaddr *) &a, &al) != 0 ||
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK) != 0) {
        close(fd);
        return 0;
    }
    CB.fd = fd;
    CB.port = ntohs(a.sin_port);
    CB.expires = now_ms() + ttl_ms();
    if (url) snprintf(url, n, "http://127.0.0.1:%d/%s", CB.port, CB.secret);
    printf("[krunker] captcha callback listening on 127.0.0.1:%d for %d s (one token)\n", CB.port, ttl_ms() / 1000);
    return 1;
}

int krunker_captcha_cb_listening(void) { return CB.fd >= 0 && now_ms() < CB.expires; }

static void respond(CbConn *c, const int code, const char *reason, const char *body, const int preflight) {
    char out[1024];
    const int len = snprintf(out, sizeof(out),
                             "HTTP/1.1 %d %s\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: %zu\r\nConnection: close\r\n"
                             "Cache-Control: no-store\r\nAccess-Control-Allow-Origin: " CB_ORIGIN "\r\nVary: Origin\r\n%s\r\n%s",
                             code, reason, strlen(body),
                             preflight ? "Access-Control-Allow-Methods: POST\r\nAccess-Control-Allow-Headers: Content-Type\r\n"
                                         "Access-Control-Allow-Private-Network: true\r\nAccess-Control-Max-Age: 60\r\n"
                                       : "",
                             body);
    if (len > 0) {
        const ssize_t w = send(c->fd, out, (size_t) (len < (int) sizeof(out) ? len : (int) sizeof(out) - 1), MSG_NOSIGNAL);
        (void) w; // best effort: the connection closes right after
    }
}

// A header's value (case-insensitive name) from the request head; 1 = present.
static int header(const char *head, const char *name, char *out, const size_t n) {
    const size_t nl = strlen(name);
    for (const char *p = strstr(head, "\r\n"); p && p[2]; p = strstr(p + 2, "\r\n")) {
        const char *h = p + 2;
        if (!strncasecmp(h, name, nl) && h[nl] == ':') {
            h += nl + 1;
            while (*h == ' ' || *h == '\t') h++;
            const size_t vl = strcspn(h, "\r\n");
            snprintf(out, n, "%.*s", (int) (vl < n ? vl : n - 1), h);
            return 1;
        }
        if (h[0] == '\r') break;
    }
    return 0;
}

static int secret_matches(const char *path) { // constant time over the secret's length
    if (path[0] != '/') return 0;
    path++;
    const size_t want = strlen(CB.secret), have = strlen(path);
    unsigned diff = have != want;
    for (size_t i = 0; i < want; i++) diff |= (unsigned) ((unsigned char) (i < have ? path[i] : 0) ^ (unsigned char) CB.secret[i]);
    return want && !diff;
}

// The body: the token as text, or JSON {"token": "..."}. 1 = a plausible token in out.
static int body_token(const char *body, const size_t len, char *out, const size_t n) {
    out[0] = 0;
    while (*body == ' ' || *body == '\r' || *body == '\n' || *body == '\t') body++;
    if (*body == '{') {
        cJSON *j = cJSON_ParseWithLength(body, len);
        const char *t = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "token"));
        if (t && strlen(t) < n) snprintf(out, n, "%s", t);
        cJSON_Delete(j);
    } else {
        size_t l = strcspn(body, " \r\n\t");
        if (l < n) {
            memcpy(out, body, l);
            out[l] = 0;
        }
    }
    const int ok = krunker_captcha_token_shape_ok(out);
    if (!ok) wipe(out, n);
    return ok;
}

// A complete request in c->buf: answer it. Returns 1 when it delivered the token.
static int handle(CbConn *c) {
    char *head_end = strstr(c->buf, "\r\n\r\n");
    char method[8] = "", path[80] = "";
    if (sscanf(c->buf, "%7s %79s HTTP/1.", method, path) != 2) {
        respond(c, 400, "Bad Request", "bad request\n", 0);
        return 0;
    }
    char origin[128];
    if (header(c->buf, "Origin", origin, sizeof(origin)) && strcmp(origin, CB_ORIGIN) != 0) {
        respond(c, 403, "Forbidden", "only https://krunker.io may send a token here\n", 0);
        return 0;
    }
    if (!secret_matches(path)) {
        respond(c, 404, "Not Found", "not found\n", 0);
        if (++CB.bad_paths >= CB_MAX_BAD_PATHS) {
            printf("[krunker] captcha callback: too many wrong paths, closing it\n");
            CB.expires = 0; // krunker_captcha_cb_poll closes it
        }
        return 0;
    }
    if (!strcmp(method, "OPTIONS")) {
        respond(c, 204, "No Content", "", 1);
        return 0;
    }
    if (strcmp(method, "POST") != 0) {
        respond(c, 405, "Method Not Allowed", "POST the token\n", 0);
        return 0;
    }
    if (CB.used) {
        respond(c, 410, "Gone", "this callback already took its one token\n", 0);
        return 0;
    }
    const char *body = head_end + 4;
    if (!body_token(body, c->len - (size_t) (body - c->buf), CB.token, sizeof(CB.token))) {
        respond(c, 400, "Bad Request", "that is not a Turnstile token\n", 0);
        return 0;
    }
    CB.used = CB.have_token = 1;
    respond(c, 200, "OK", "token received: Krunker Box Office continues matchmaking\n", 0);
    printf("[krunker] captcha callback: a token arrived (%zu chars)\n", strlen(CB.token));
    return 1;
}

// 1 = the request is complete (head + Content-Length bytes), -1 = malformed / too big, 0 = wait
static int request_complete(const CbConn *c) {
    const char *head_end = strstr(c->buf, "\r\n\r\n");
    if (!head_end) return c->len >= CB_MAX_REQUEST ? -1 : 0;
    char cl[24];
    long want = 0;
    if (header(c->buf, "Content-Length", cl, sizeof(cl))) want = strtol(cl, NULL, 10);
    if (want < 0 || want > KRUNKER_CAPTCHA_TOKEN_MAX + 64) return -1;
    const size_t have = c->len - (size_t) (head_end + 4 - c->buf);
    return have >= (size_t) want ? 1 : 0;
}

int krunker_captcha_cb_poll(char *token, const size_t n) {
    if (CB.fd < 0) return 0;
    const double now = now_ms();
    if (now >= CB.expires) {
        printf("[krunker] captcha callback expired\n");
        krunker_captcha_cb_stop();
        return -1;
    }
    for (int k = 0; k < 8; k++) { // accept what is waiting
        const int fd = accept(CB.fd, NULL, NULL);
        if (fd < 0) break;
        fcntl(fd, F_SETFD, FD_CLOEXEC); // never inherited by the browser ui_open_browser starts
        int slot = -1;
        for (int i = 0; i < CB_MAX_CONNS && slot < 0; i++) {
            if (CB.conns[i].fd < 0) slot = i;
        }
        if (slot < 0 || fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK) != 0) {
            close(fd); // busy: the userscript retries
            continue;
        }
        CB.conns[slot].fd = fd;
        CB.conns[slot].len = 0;
        CB.conns[slot].deadline = now + CB_CONN_TIMEOUT_MS;
    }
    int got = 0;
    for (int i = 0; i < CB_MAX_CONNS && CB.fd >= 0; i++) {
        CbConn *c = &CB.conns[i];
        if (c->fd < 0) continue;
        for (;;) {
            if (c->len >= CB_MAX_REQUEST) break;
            const ssize_t r = recv(c->fd, c->buf + c->len, CB_MAX_REQUEST - c->len, 0);
            if (r > 0) {
                c->len += (size_t) r;
                continue;
            }
            if (r == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) c->deadline = 0; // closed / failed
            break;
        }
        c->buf[c->len] = 0;
        const int done = request_complete(c);
        if (done > 0) {
            got |= handle(c);
            conn_close(c);
        } else if (done < 0) {
            respond(c, 413, "Payload Too Large", "too large\n", 0);
            conn_close(c);
        } else if (now >= c->deadline) {
            conn_close(c);
        }
    }
    if (CB.fd >= 0 && CB.expires == 0) { // too many wrong paths
        krunker_captcha_cb_stop();
        return -1;
    }
    if (got && CB.have_token) {
        snprintf(token, n, "%s", CB.token);
        wipe(CB.token, sizeof(CB.token));
        CB.have_token = 0;
        return 1;
    }
    return 0;
}

#else // Windows (no POSIX sockets here yet) and the web (a page cannot listen): path 2a only

int krunker_captcha_cb_start(char *url, const size_t n) {
    if (url && n) url[0] = 0;
    return 0;
}
void krunker_captcha_cb_stop(void) {}
int krunker_captcha_cb_listening(void) { return 0; }
int krunker_captcha_cb_poll(char *token, const size_t n) {
    (void) token;
    (void) n;
    return 0;
}
#endif

int krunker_captcha_token_shape_ok(const char *token) {
    if (!token) return 0;
    const size_t n = strlen(token);
    if (n < 20 || n > KRUNKER_CAPTCHA_TOKEN_MAX) return 0;
    for (const char *p = token; *p; p++) {
        const char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' || c == '~' ||
              c == '+' || c == '/' || c == '='))
            return 0;
    }
    return 1;
}
