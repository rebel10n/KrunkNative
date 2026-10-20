// kn_webview_live_test: the captcha webview run for real (WebKitGTK, a display). The test serves its own
// stub page on a loopback port (STUB_HTML below, the same origin rule as krunker.io's): it defines
// window.turnstile like Cloudflare's api.js, rendering ONE button into the element the page passes to
// render(), which answers a token when clicked (the widget's callback). Checked:
//   1. in the game UI (in_ui, shown at once): no window (kn_webview_captcha_windowed 0), the view paints
//      the widget into its frames (kn_webview_captcha_frame: opaque pixels in a transparent frame), a click
//      forwarded through kn_webview_captcha_pointer reaches the page, and the token comes back (KN_WEBVIEW_TOKEN);
//   2. the same in the game UI with the keys only: Tab moves the focus to the widget's button, Enter
//      presses it (kn_webview_captcha_key), and the token comes back;
//   3. KRUNKNATIVE_CAPTCHA_WINDOW=1 (the window the challenge had before): ignored, the same challenge in the game;
//   4. the game UI takes no frame for 15 s: the challenge stays in the game (no window, no second prompt).
// Not part of ctest: it needs a display. Run it on a private one, never the desktop's:
//   xvfb-run -a ./kn_webview_live_test            (no argument: the test serves its own page)
//   xvfb-run -a ./kn_webview_live_test <url>      (a page of yours, same contract)
#define _DEFAULT_SOURCE
#include <kn_webview.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

// setenv / unsetenv for MinGW, whose CRT has only _putenv_s (the same guard as client/src/headless.c).
// An empty value removes the variable.
static int test_setenv(const char *name, const char *value, const int overwrite) {
#ifdef _WIN32
    if (!overwrite && getenv(name)) return 0;
    return _putenv_s(name, value);
#else
    return setenv(name, value, overwrite);
#endif
}

static int test_unsetenv(const char *name) {
#ifdef _WIN32
    return _putenv_s(name, "");
#else
    return unsetenv(name);
#endif
}

#define STUB_TOKEN "0.STUBTOKEN.abcdefghijklmnopqrstuvwxyz0123456789"
#define TOKEN_MAX 4096

// The stub page: a Turnstile stand-in. turnstile.render(el, opts) puts the widget INTO el (as Cloudflare's
// does: the holder the page passes), so the game UI's frame shows it; its button answers the token.
static const char STUB_HTML[] =
    "<!doctype html><html><head><meta charset=\"utf-8\"><title>stub</title></head><body>\n"
    "<script>\n"
    "window.turnstile = {\n"
    "  render: function (el, opts) {\n"
    "    var b = document.createElement('button');\n"
    "    b.id = 'stub-tick';\n"
    "    b.textContent = 'Tick';\n"
    "    b.style.cssText = 'width:100%;height:65px;font-size:18px;cursor:pointer;background:#2196f3;color:#fff;border:0';\n"
    "    b.onclick = function () { opts.callback('" STUB_TOKEN "'); };\n"
    "    el.appendChild(b);\n"
    "    // Tab reaches the page, whose own focus order gives the button the focus (WebKit's Tab advance does not run for\n"
    "    // keys sent as events in this check: the check is that Tab gets to the widget and Enter presses what has the focus)\n"
    "    document.addEventListener('keydown', function (e) { if (e.key === 'Tab') { e.preventDefault(); b.focus(); } }, true);\n"
    "    return 'stub-widget';\n"
    "  },\n"
    "  remove: function () {}\n"
    "};\n"
    "</script></body></html>\n";

static int failures = 0;
static int skipped = 0; // checks not run on this platform (see case 2)

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
            failures++;                                                   \
        }                                                                 \
    } while (0)

// ---------------------------------------------------------------- the stub page's server (loopback, one thread)

static int stub_fd = -1;
static int stub_port = 0;
static volatile int stub_stop = 0;

static void *stub_serve(void *arg) {
    (void) arg;
    for (;;) {
        const int c = accept(stub_fd, NULL, NULL);
        if (stub_stop) { // (stub_stop_now's own connection wakes the accept)
            if (c >= 0) close(c);
            break;
        }
        if (c < 0) continue;
        struct timeval tv = {2, 0};
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        char req[2048];
        (void) recv(c, req, sizeof(req) - 1, 0); // the request line is not needed: every GET gets the page
        char head[256];
        const int hl = snprintf(head, sizeof(head),
                                "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %zu\r\n"
                                "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
                                sizeof(STUB_HTML) - 1);
        (void) send(c, head, (size_t) hl, 0);
        (void) send(c, STUB_HTML, sizeof(STUB_HTML) - 1, 0);
        close(c);
    }
    return NULL;
}

// Binds 127.0.0.1 on a free port and serves the stub page; url gets its address.
static int stub_start(char *url, const size_t n, pthread_t *th) {
    stub_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (stub_fd < 0) return 0;
    int one = 1;
    setsockopt(stub_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    socklen_t len = sizeof(a);
    if (bind(stub_fd, (struct sockaddr *) &a, sizeof(a)) != 0 || listen(stub_fd, 8) != 0 ||
        getsockname(stub_fd, (struct sockaddr *) &a, &len) != 0 || pthread_create(th, NULL, stub_serve, NULL) != 0) {
        close(stub_fd);
        stub_fd = -1;
        return 0;
    }
    stub_port = (int) ntohs(a.sin_port);
    snprintf(url, n, "http://127.0.0.1:%d/stub.html", stub_port);
    return 1;
}

static void stub_stop_now(const pthread_t th) {
    stub_stop = 1;
    // a connection wakes the accept (on macOS shutdown() does not wake a blocked accept on a listening socket)
    const int c = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((uint16_t) stub_port);
    if (c >= 0) {
        (void) connect(c, (struct sockaddr *) &a, sizeof(a));
        close(c);
    }
    shutdown(stub_fd, SHUT_RDWR);
    pthread_join(th, NULL);
    close(stub_fd);
}

// ---------------------------------------------------------------- the checks

static void sleep_ms(const long ms) {
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// Pumps the webview until an event arrives (or timeout_ms passes); returns it (KN_WEBVIEW_NONE on timeout).
static int wait_event(const int want, const int timeout_ms, char *token, const size_t tokn) {
    const long long end = now_ms() + timeout_ms;
    char msg[320];
    while (now_ms() < end) {
        const int rc = kn_webview_captcha_poll(token, tokn, msg, sizeof(msg));
        if (rc == KN_WEBVIEW_NONE) {
            sleep_ms(5);
            continue;
        }
        if (rc != want) printf("  (event %d instead: %s)\n", rc, msg);
        return rc;
    }
    return KN_WEBVIEW_NONE;
}

// A frame of the game UI that shows the widget: some opaque pixels (its button), the rest transparent. A
// frame that is all transparent (nothing painted yet) or all opaque (a white page) does not count. Within 20 s.
static int wait_frame(int *w, int *h) {
    const unsigned char *px = NULL;
    int last = 0;
    const long long end = now_ms() + 20000;
    while (now_ms() < end) {
        kn_webview_pump();
        if (kn_webview_captcha_frame(&px, w, h)) {
            int opaque = 0;
            for (int i = 3; i < *w * *h * 4; i += 4) opaque += px[i] != 0;
            last = opaque;
            if (opaque > 0 && opaque < *w * *h) return opaque;
        } else {
            sleep_ms(5);
        }
    }
    printf("  no widget pixels in 20 s (the last frame: %d opaque px of %d)\n", last, *w * *h);
    return 0;
}

// The challenge in the game UI from open to token; keys: Tab + Enter instead of the click.
// Returns 1 when the stub's token came back through the poll.
static int game_ui_run(const char *url, const int keys, const char *name) {
    char err[256] = "", token[TOKEN_MAX + 1] = "";
    printf("%s\n", name);
    CHECK(kn_webview_captcha_open(url, "stub_sitekey_1", "Security Challenge", NULL, 1, 1, err, sizeof(err)) == 1);
    CHECK(wait_event(KN_WEBVIEW_SHOWN, 30000, token, sizeof(token)) == KN_WEBVIEW_SHOWN);
    CHECK(kn_webview_captcha_inline() == 1);
    CHECK(kn_webview_captcha_windowed() == 0); // no window: the game UI holds the challenge
    int w = 0, h = 0;
    const int opaque = wait_frame(&w, &h);
    printf("  frame %dx%d, %d opaque px\n", w, h, opaque);
    CHECK(w == KN_WEBVIEW_INLINE_W && h == KN_WEBVIEW_INLINE_H);
    CHECK(opaque > 0);
    if (keys) {
        CHECK(kn_webview_captcha_key(KN_WEBVIEW_KEY_TAB, 1) == 1);
        CHECK(kn_webview_captcha_key(KN_WEBVIEW_KEY_TAB, 0) == 1);
        sleep_ms(200); // the Tab moves the focus to the button before Enter
        CHECK(kn_webview_captcha_key(KN_WEBVIEW_KEY_ENTER, 1) == 1);
        CHECK(kn_webview_captcha_key(KN_WEBVIEW_KEY_ENTER, 0) == 1);
    } else {
        // the click at the view's centre (the stub button fills the widget's box) reaches the page
        CHECK(kn_webview_captcha_pointer(KN_WEBVIEW_POINTER_MOVE, w / 2.0, h / 2.0, 0) == 1);
        CHECK(kn_webview_captcha_pointer(KN_WEBVIEW_POINTER_PRESS, w / 2.0, h / 2.0, 1) == 1);
        CHECK(kn_webview_captcha_pointer(KN_WEBVIEW_POINTER_RELEASE, w / 2.0, h / 2.0, 1) == 1);
    }
    const int got = wait_event(KN_WEBVIEW_TOKEN, 15000, token, sizeof(token));
    CHECK(got == KN_WEBVIEW_TOKEN);
    const int is_stub = strcmp(token, STUB_TOKEN) == 0;
    CHECK(is_stub);
    memset(token, 0, sizeof(token));
    CHECK(kn_webview_captcha_inline() == 0); // the token closed it
    CHECK(kn_webview_captcha_is_open() == 0);
    kn_webview_captcha_close(); // (a no-op after the token; a failed run must not hold the next one)
    return got == KN_WEBVIEW_TOKEN && is_stub;
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN); // the stub server writes to a connection WebKit may have closed: a failed send, not a crash
    char url[256] = "";
    pthread_t th;
    int own_server = 0;
    if (argc >= 2) {
        snprintf(url, sizeof(url), "%s", argv[1]);
    } else if (stub_start(url, sizeof(url), &th)) {
        own_server = 1;
    } else {
        printf("FAIL: no loopback socket for the stub page\n");
        return 1;
    }
    printf("page: %s\n", url);
    if (!kn_webview_available()) {
        printf("FAIL: no webview in this build (KN_WEBVIEW)\n");
        if (own_server) stub_stop_now(th);
        return 1;
    }

    game_ui_run(url, 0, "1. the game UI, the pointer:");
    game_ui_run(url, 1, "2. the game UI, the keys (Tab, Enter):");

    // 3. KRUNKNATIVE_CAPTCHA_WINDOW=1 (the window the challenge had before) is ignored: the challenge is the widget
    //    in the game, drawn in its frames, and never a window
    char err[256] = "", token[TOKEN_MAX + 1] = "";
    printf("3. KRUNKNATIVE_CAPTCHA_WINDOW=1 (no window any more):\n");
    test_setenv("KRUNKNATIVE_CAPTCHA_WINDOW", "1", 1);
    CHECK(kn_webview_captcha_open(url, "stub_sitekey_1", NULL, NULL, 1, 1, err, sizeof(err)) == 1);
    CHECK(wait_event(KN_WEBVIEW_SHOWN, 30000, token, sizeof(token)) == KN_WEBVIEW_SHOWN);
    CHECK(kn_webview_captcha_inline() == 1);
    CHECK(kn_webview_captcha_windowed() == 0); // never a window, whatever the environment says
    int w = 0, h = 0;
    CHECK(wait_frame(&w, &h) > 0); // the widget is drawn in the game's frames
    kn_webview_captcha_close();
    test_unsetenv("KRUNKNATIVE_CAPTCHA_WINDOW");

    // 4. the game UI draws nothing for 15 s (slow frames are a lower frame rate, not a window): the challenge stays
    //    in the game, with no window and no second prompt
    printf("4. the game UI draws nothing for 15 s: still the game UI, no window:\n");
    CHECK(kn_webview_captcha_open(url, "stub_sitekey_1", NULL, NULL, 1, 1, err, sizeof(err)) == 1);
    CHECK(wait_event(KN_WEBVIEW_SHOWN, 30000, token, sizeof(token)) == KN_WEBVIEW_SHOWN);
    CHECK(kn_webview_captcha_inline() == 1 && kn_webview_captcha_windowed() == 0);
    CHECK(wait_event(KN_WEBVIEW_SHOWN, 15000, token, sizeof(token)) == KN_WEBVIEW_NONE); // no second prompt, no fallback
    CHECK(kn_webview_captcha_inline() == 1 && kn_webview_captcha_windowed() == 0);
    kn_webview_captcha_close();

    if (own_server) stub_stop_now(th);
    if (failures) printf("kn_webview_live_test: %d failure(s)\n", failures);
    else if (skipped) printf("kn_webview_live_test: all passed (%d skipped on this platform)\n", skipped);
    else printf("kn_webview_live_test: all passed\n");
    return failures ? 1 : 0;
}
