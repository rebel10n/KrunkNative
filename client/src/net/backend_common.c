// Brain client, platform-independent half (client/include/backend.h): URL state,
// social listeners and frame dispatch. backend_native.c / backend_web.c own the
// transports and call backend_dispatch_text() / backend_dispatch_event() on the
// main thread.
#include <backend.h>
#include "backend_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char g_url[256];

#define BACKEND_LISTENERS 8
static struct {
    BackendSocialFn fn;
    void *user;
} g_listeners[BACKEND_LISTENERS];

void backend_store_url(const char *url) {
    size_t n = 0;
    if (url) {
        while (*url == ' ') url++;
        n = strlen(url);
        if (n >= sizeof(g_url)) n = sizeof(g_url) - 1;
        memcpy(g_url, url, n);
    }
    g_url[n] = 0;
    while (n > 0 && (g_url[n - 1] == '/' || g_url[n - 1] == ' ' || g_url[n - 1] == '\n')) g_url[--n] = 0;
    // "0" / "off" / "none" disable it like an empty value
    if (!strcmp(g_url, "0") || !strcmp(g_url, "off") || !strcmp(g_url, "none") || !strcmp(g_url, "false")) g_url[0] = 0;
}

int backend_enabled(void) { return g_url[0] != 0; }

// ---- brain probe (default URLs)
static int g_avail = 1;        // backend_available()
static int g_probing;
static int g_social_wanted;    // something asked for the social socket while the probe ran

static void probe_done(void *user, const int status, const char *body, const size_t length) {
    (void) user;
    (void) length;
    g_probing = 0;
    g_avail = status == 200 && strstr(body, "\"brain\"") ? 1 : 0;
    if (g_avail) printf("[backend] brain found at %s\n", backend_url());
    // e.g. `make web-serve`, or `make bridge` with no kn_brain behind it: say what that means rather
    // than leave it to a failing /social/ws (none is attempted) or a silent guest menu
    else printf("[backend] no account server at %s (GET /mm/status: HTTP %d): social features are off, playing as a guest "
                "(run kn_brain, e.g. make backend-up, or pass ?brain=<url>)\n", backend_url(), status);
    if (g_avail && g_social_wanted) backend_social_open();
}

void backend_probe_start(void) {
    if (!backend_enabled() || g_probing || !backend_url_is_default()) return;
    g_probing = 1;
    if (g_avail == 1) g_avail = -1;
    if (!backend_http_get("/mm/status", probe_done, NULL)) {
        g_probing = 0;
        g_avail = 0;
    }
}

int backend_available(void) {
    if (!backend_enabled()) return 0;
    if (!backend_url_is_default()) return 1;
    return g_probing && g_avail == -1 ? -1 : g_avail;
}

int backend_social_allowed(void) {
    if (backend_available() == 1) return 1;
    g_social_wanted = 1;
    if (!g_probing && g_avail != 0) backend_probe_start();
    return 0;
}
const char *backend_url(void) { return g_url; }

// <brain>/social/ws?enc=json with http(s) -> ws(s)
void backend_social_ws_url(char *out, const size_t n) {
    const char *rest = g_url;
    const char *scheme = "ws://";
    if (!strncmp(rest, "https://", 8)) {
        scheme = "wss://";
        rest += 8;
    } else if (!strncmp(rest, "http://", 7)) {
        rest += 7;
    } else if (!strncmp(rest, "wss://", 6)) {
        scheme = "wss://";
        rest += 6;
    } else if (!strncmp(rest, "ws://", 5)) {
        rest += 5;
    }
    snprintf(out, n, "%s%s/social/ws?enc=json", scheme, rest);
}

int backend_social_listen(const BackendSocialFn fn, void *user) {
    for (int i = 0; i < BACKEND_LISTENERS; i++) {
        if (g_listeners[i].fn == fn && g_listeners[i].user == user) return 1;
    }
    for (int i = 0; i < BACKEND_LISTENERS; i++) {
        if (!g_listeners[i].fn) {
            g_listeners[i].fn = fn;
            g_listeners[i].user = user;
            return 1;
        }
    }
    return 0;
}

void backend_social_unlisten(const BackendSocialFn fn, void *user) {
    for (int i = 0; i < BACKEND_LISTENERS; i++) {
        if (g_listeners[i].fn == fn && g_listeners[i].user == user) g_listeners[i].fn = NULL;
    }
}

static void dispatch(const cJSON *frame) {
    for (int i = 0; i < BACKEND_LISTENERS; i++) {
        if (g_listeners[i].fn) g_listeners[i].fn(g_listeners[i].user, frame);
    }
}

void backend_dispatch_event(const char *label) {
    cJSON *frame = cJSON_CreateArray();
    if (!frame) return;
    cJSON_AddItemToArray(frame, cJSON_CreateString(label));
    dispatch(frame);
    cJSON_Delete(frame);
}

void backend_dispatch_text(const char *text) {
    cJSON *frame = cJSON_Parse(text);
    if (!cJSON_IsArray(frame) || !cJSON_IsString(cJSON_GetArrayItem(frame, 0))) {
        cJSON_Delete(frame);
        return;
    }
    const char *label = cJSON_GetArrayItem(frame, 0)->valuestring;
    if (!strcmp(label, "pi")) backend_social_send_text("[\"po\"]"); // io-client: ping -> pong
    dispatch(frame);
    cJSON_Delete(frame);
}

int backend_social_send(const cJSON *frame) {
    if (!frame || !backend_enabled()) return 0;
    char *text = cJSON_PrintUnformatted(frame);
    if (!text) return 0;
    const int ok = backend_social_send_text(text);
    free(text);
    return ok;
}

void backend_url_encode(char *out, const size_t n, const char *s) {
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    if (!n) return;
    for (; s && *s && o + 4 < n; s++) {
        const unsigned char c = (unsigned char) *s;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out[o++] = (char) c;
        } else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 15];
        }
    }
    out[o] = 0;
}
