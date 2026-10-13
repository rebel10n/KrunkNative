// backend_online_test: the online-mode defaults and the native brain transport.
//   * online_mode.c: online by default, with the public finder and account service (never
//     localhost); the knMode / knFinderUrl / env / CLI precedence; "" / off turns the finder off.
//   * backend_native.c: backend_init picks the account service; then a real HTTP round trip
//     against a loopback server in this process (POSIX sockets / Winsock), one 404, and one
//     failed connection whose callback gets status 0 plus the reason.
// On Windows this runs backend_winhttp.c (WinHTTP). That binary used to compile
// backend_native.c to no-op stubs: no account service, an empty server finder, and a game
// connection to 127.0.0.1. CI runs it on Windows too (.github/workflows/client.yml).
#include <backend.h>
#include <online_mode.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET Sock;
#define SOCK_BAD INVALID_SOCKET
#define sock_close closesocket
static void sleep_ms(const int ms) { Sleep((DWORD) ms); }
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
typedef int Sock;
#define SOCK_BAD (-1)
#define sock_close close
static void sleep_ms(const int ms) {
    struct timespec ts = {ms / 1000, (long) (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}
#endif
#include <pthread.h>

static int failures, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

// ------------------------------------------------------------------ stubs of the client around the units
// storage (html_dom_core.c): an in-memory map, so no test touches ~/.config
static struct { const char *key; char value[256]; int set; } g_store[8];
int html_storage_get(const char *key, char *buf, const size_t n) {
    for (int i = 0; i < 8; i++) {
        if (g_store[i].set && !strcmp(g_store[i].key, key)) {
            if (buf && n) snprintf(buf, n, "%s", g_store[i].value);
            return (int) strlen(g_store[i].value);
        }
    }
    return -1;
}
void html_storage_set(const char *key, const char *value) {
    for (int i = 0; i < 8; i++) {
        if (g_store[i].set && !strcmp(g_store[i].key, key)) g_store[i].set = 0;
    }
    if (!value) return;
    for (int i = 0; i < 8; i++) {
        if (!g_store[i].set) {
            g_store[i].key = key;
            snprintf(g_store[i].value, sizeof(g_store[i].value), "%s", value);
            g_store[i].set = 1;
            return;
        }
    }
}
static void store_clear(void) { memset(g_store, 0, sizeof(g_store)); }
unsigned html_dom_by_id(const char *id) { (void) id; return 0; }
void html_dom_set_html(unsigned e, const char *html) { (void) e; (void) html; }
static int g_headless;
int headless_enabled(void) { return g_headless; }
double glfwGetTime(void) { return 0.0; }
const void *account_get(void) { return NULL; }
const char *account_session_token(void) { return ""; }
void client_net_send(void *client, int type, const void *data, unsigned short len) { (void) client; (void) type; (void) data; (void) len; }
int kn_auth_is_hex(const char *s, size_t max_len) { (void) s; (void) max_len; return 1; }
void kn_server_hash(const char *a, const char *b, const char *c, char out[65]) { (void) a; (void) b; (void) c; out[0] = 0; }

static void set_env(const char *k, const char *v) {
#ifdef _WIN32
    _putenv_s(k, v ? v : "");
#else
    if (v) setenv(k, v, 1);
    else unsetenv(k);
#endif
}
static int is_local(const char *url) { return strstr(url, "127.0.0.1") || strstr(url, "localhost"); }

// ------------------------------------------------------------------ online_mode.c
static void test_mode_defaults(void) {
    store_clear();
    set_env("KRUNKNATIVE_MODE", NULL);
    set_env("KRUNKNATIVE_FINDER_URL", NULL);
    online_mode_init(NULL, NULL);
    CHECK(online_mode_online(), "online by default");
    CHECK(!strcmp(online_finder_url(), ONLINE_DEFAULT_FINDER_URL), "default finder, got %s", online_finder_url());
    CHECK(!strcmp(online_default_account_url(), ONLINE_DEFAULT_ACCOUNT_URL), "default account service, got %s", online_default_account_url());
    CHECK(!strncmp(ONLINE_DEFAULT_FINDER_URL, "https://", 8) && !is_local(ONLINE_DEFAULT_FINDER_URL), "the finder default is public");
    CHECK(!strncmp(ONLINE_DEFAULT_ACCOUNT_URL, "https://", 8) && !is_local(ONLINE_DEFAULT_ACCOUNT_URL), "the account default is public");

    html_storage_set("knMode", "offline");
    online_mode_init(NULL, NULL);
    CHECK(!online_mode_online() && !online_default_account_url()[0], "knMode=offline: offline, no account service");
    CHECK(!strcmp(online_finder_url(), ONLINE_DEFAULT_FINDER_URL), "offline keeps the finder (both modes)");
    set_env("KRUNKNATIVE_MODE", "online");
    online_mode_init(NULL, NULL);
    CHECK(online_mode_online(), "KRUNKNATIVE_MODE beats the stored mode");
    online_mode_init("offline", NULL);
    CHECK(!online_mode_online(), "--mode beats KRUNKNATIVE_MODE");
    set_env("KRUNKNATIVE_MODE", NULL);

    store_clear();
    html_storage_set("knFinderUrl", "http://finder.example/servers//");
    online_mode_init(NULL, NULL);
    CHECK(!strcmp(online_finder_url(), "http://finder.example/servers"), "stored finder, slashes trimmed: %s", online_finder_url());
    set_env("KRUNKNATIVE_FINDER_URL", "off");
    online_mode_init(NULL, NULL);
    CHECK(!online_finder_url()[0], "KRUNKNATIVE_FINDER_URL=off disables it");
    online_mode_init(NULL, "http://cli.example/servers");
    CHECK(!strcmp(online_finder_url(), "http://cli.example/servers"), "--finder beats the env");
    set_env("KRUNKNATIVE_FINDER_URL", NULL);
    store_clear();
    online_mode_init(NULL, NULL);
    CHECK(!strcmp(online_finder_url(), ONLINE_DEFAULT_FINDER_URL), "back to the default after a re-init");
}

// ------------------------------------------------------------------ backend_init
static void test_backend_url(void) {
    store_clear();
    set_env("KRUNKNATIVE_BRAIN_URL", NULL);
    g_headless = 0;
    online_mode_init(NULL, NULL);
    backend_init(NULL);
    CHECK(backend_enabled() && !strcmp(backend_url(), ONLINE_DEFAULT_ACCOUNT_URL), "online: the public account service, got '%s'", backend_url());
    CHECK(backend_url_is_default(), "the default URL is probed");
    html_storage_set("knBrainUrl", "http://brain.example:5050");
    backend_init(NULL);
    CHECK(!strcmp(backend_url(), "http://brain.example:5050") && !backend_url_is_default(), "knBrainUrl, got '%s'", backend_url());
    set_env("KRUNKNATIVE_BRAIN_URL", "http://env.example");
    backend_init(NULL);
    CHECK(!strcmp(backend_url(), "http://env.example"), "KRUNKNATIVE_BRAIN_URL beats storage");
    backend_init("http://cli.example");
    CHECK(!strcmp(backend_url(), "http://cli.example"), "--brain beats the env");
    backend_init("");
    CHECK(!backend_enabled(), "offline mode: no account service");
    set_env("KRUNKNATIVE_BRAIN_URL", NULL);
    store_clear();
    g_headless = 1;
    backend_init(NULL);
    CHECK(!backend_enabled(), "headless with nothing configured: no account service");
    g_headless = 0;
}
// ------------------------------------------------------------------ the transport (loopback)
static Sock g_listen = SOCK_BAD;
static int g_port;
static char g_seen[512]; // the last request line + Host header seen by the server

static void *server_thread(void *arg) {
    (void) arg;
    for (int served = 0; served < 2; served++) {
        Sock c = accept(g_listen, NULL, NULL);
        if (c == SOCK_BAD) break;
        char req[2048];
        int n = 0;
        while (n < (int) sizeof(req) - 1) {
            const int r = (int) recv(c, req + n, (int) sizeof(req) - 1 - n, 0);
            if (r <= 0) break;
            n += r;
            req[n] = 0;
            if (strstr(req, "\r\n\r\n")) break;
        }
        req[n] = 0;
        snprintf(g_seen, sizeof(g_seen), "%s", req);
        const int found = strncmp(req, "GET /servers", 12) == 0;
        const char *body = found ? "{\"ok\":true,\"servers\":[{\"host\":\"test-1.example\",\"port\":21016}]}" : "{\"error\":\"nope\"}";
        char resp[512];
        const int len = snprintf(resp, sizeof(resp), "HTTP/1.1 %s\r\nContent-Type: application/json\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s",
                                 found ? "200 OK" : "404 Not Found", (int) strlen(body), body);
        send(c, resp, len, 0);
        sock_close(c);
    }
    return NULL;
}

static int start_server(void) {
    g_listen = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen == SOCK_BAD) return 0;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0; // the OS picks a free port
    socklen_t al = sizeof(a);
    if (bind(g_listen, (struct sockaddr *) &a, sizeof(a)) != 0 || listen(g_listen, 4) != 0 ||
        getsockname(g_listen, (struct sockaddr *) &a, &al) != 0)
        return 0;
    g_port = ntohs(a.sin_port);
    pthread_t t;
    return pthread_create(&t, NULL, server_thread, NULL) == 0;
}

typedef struct {
    int done, status;
    char body[512];
} Result;

static void on_done(void *user, const int status, const char *body, const size_t length) {
    Result *r = user;
    r->status = status;
    snprintf(r->body, sizeof(r->body), "%.*s", (int) length, body ? body : "");
    r->done = 1;
}

static int wait_for(Result *r) {
    for (int i = 0; i < 1500 && !r->done; i++) { // 15 s
        backend_poll();
        sleep_ms(10);
    }
    return r->done;
}

static void test_transport(void) {
    CHECK(start_server(), "loopback server");
    char url[128];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/servers", g_port);
    Result ok = {0};
    CHECK(backend_http_request("GET", url, NULL, on_done, &ok), "the request is queued (the Windows stub returned 0)");
    CHECK(wait_for(&ok), "the request completes");
    CHECK(ok.status == 200 && strstr(ok.body, "test-1.example"), "200 + the server list, got %d '%s'", ok.status, ok.body);
    CHECK(strstr(g_seen, "GET /servers HTTP/1.1") != NULL, "request line: %.60s", g_seen);
    char host[64];
    snprintf(host, sizeof(host), "127.0.0.1:%d", g_port);
    CHECK(strstr(g_seen, host) != NULL, "Host header names the port");

    snprintf(url, sizeof(url), "http://127.0.0.1:%d/missing", g_port);
    Result nf = {0};
    backend_http_request("GET", url, NULL, on_done, &nf);
    CHECK(wait_for(&nf) && nf.status == 404 && strstr(nf.body, "nope"), "404 keeps its body, got %d '%s'", nf.status, nf.body);
    sock_close(g_listen); // nothing listens there any more

    Result down = {0};
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/servers", g_port);
    backend_http_request("GET", url, NULL, on_done, &down);
    CHECK(wait_for(&down), "a failed request still completes");
    CHECK(down.status == 0 && down.body[0], "status 0 with the reason, got %d '%s'", down.status, down.body);
}

int main(void) {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    test_mode_defaults();
    test_backend_url();
    test_transport();
    printf("backend_online_test: %d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
