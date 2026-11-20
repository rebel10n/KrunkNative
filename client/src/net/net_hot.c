// The hot seek (client/include/net_hot.h, doc/web-hot-loader.md). The web: window.__knHot is written by
// platform/web/hot-loader.js (inlined in <head> of the game page); this only reads it. Native: sent from C.
#include <net_hot.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

EM_JS(int, kn_hot_status, (void), {
    var H = typeof window !== 'undefined' ? window.__knHot : null;
    if (!H || H.used) return -1;
    return H.status < 0 ? -2 : (H.status | 0);
});

EM_JS(double, kn_hot_age, (void), {
    var H = typeof window !== 'undefined' ? window.__knHot : null;
    return H ? performance.now() - H.t0 : 0;
});

EM_JS(char *, kn_hot_game, (void), {
    var H = typeof window !== 'undefined' ? window.__knHot : null;
    return stringToNewUTF8(H && H.game ? H.game : "");
});

EM_JS(char *, kn_hot_take, (void), {
    var H = typeof window !== 'undefined' ? window.__knHot : null;
    if (!H || H.used || H.status !== 200) return 0;
    H.used = true;
    H.times.taken = Math.round(performance.now() - H.t0);
    console.log('[hot] the client takes the hot seek (answer at ' + H.times.answer + ' ms, taken at ' + H.times.taken + ' ms)');
    return stringToNewUTF8(H.body);
});

EM_JS(void, kn_hot_drop, (const char *why), {
    var H = typeof window !== 'undefined' ? window.__knHot : null;
    if (!H || H.used) return;
    H.used = true;
    console.log('[hot] the hot seek is not used: ' + UTF8ToString(why));
});

int net_hot_seek_start(const char *path, const char *game) {
    (void) path;
    (void) game;
    return 0; // the page's hot loader sent it
}

int net_hot_seek_status(void) { return kn_hot_status(); }
double net_hot_age_ms(void) { return kn_hot_age(); }

void net_hot_seek_game(char *out, const int size) {
    char *g = kn_hot_game();
    snprintf(out, (size_t) size, "%s", g ? g : "");
    free(g);
}

char *net_hot_seek_take(void) { return kn_hot_take(); }
void net_hot_seek_drop(const char *why) { kn_hot_drop(why ? why : ""); }

#else
// Native: the same shortcut inside the process. backend_hot_start (backend_startup.c) sends the
// /mm/hot-seek right after the command line is parsed, while the window, GL, the fonts and the
// menu page are still being set up; its answer waits here for servers_ui.c's startup seek.
#include <backend.h>
#include <GLFW/glfw3.h>

static struct {
    int state;        // -1 none / taken / dropped, 0 out, else the HTTP status (-2: no answer)
    double t0;        // glfwGetTime() when it was sent
    double answer_ms; // when it answered (ms after t0)
    char *body;
    char game[128];
} g_hot = {-1, 0.0, 0.0, NULL, ""};

static void on_hot(void *user, const int status, const char *body, const size_t length) {
    (void) user;
    if (g_hot.state != 0) return; // dropped while it was out
    g_hot.answer_ms = (glfwGetTime() - g_hot.t0) * 1000.0;
    g_hot.state = status > 0 ? status : -2;
    free(g_hot.body);
    g_hot.body = NULL;
    if (status == 200 && body) {
        g_hot.body = malloc(length + 1);
        if (g_hot.body) {
            memcpy(g_hot.body, body, length);
            g_hot.body[length] = 0;
        } else {
            g_hot.state = -2;
        }
    }
    printf("[hot] the hot seek answered HTTP %d in %.0f ms\n", status, g_hot.answer_ms);
}

int net_hot_seek_start(const char *path, const char *game) {
    if (!path || g_hot.state == 0) return 0;
    snprintf(g_hot.game, sizeof(g_hot.game), "%s", game ? game : "");
    g_hot.t0 = glfwGetTime();
    g_hot.state = 0;
    if (!backend_http_get(path, on_hot, NULL)) {
        g_hot.state = -1;
        return 0;
    }
    return 1;
}

int net_hot_seek_status(void) { return g_hot.state; }
double net_hot_age_ms(void) { return g_hot.state == -1 ? 0.0 : (glfwGetTime() - g_hot.t0) * 1000.0; }

void net_hot_seek_game(char *out, const int size) {
    if (out && size > 0) snprintf(out, (size_t) size, "%s", g_hot.game);
}

char *net_hot_seek_take(void) {
    if (g_hot.state != 200 || !g_hot.body) return NULL;
    char *body = g_hot.body;
    g_hot.body = NULL;
    g_hot.state = -1;
    printf("[hot] the client takes the hot seek (answer at %.0f ms, taken at %.0f ms)\n", g_hot.answer_ms,
           (glfwGetTime() - g_hot.t0) * 1000.0);
    return body;
}

void net_hot_seek_drop(const char *why) {
    if (g_hot.state == -1) return;
    g_hot.state = -1; // a late answer is ignored (on_hot)
    free(g_hot.body);
    g_hot.body = NULL;
    printf("[hot] the hot seek is not used: %s\n", why ? why : "");
}

#endif
