// Brain client, platform-independent half (client/include/backend.h): URL state,
// social listeners and frame dispatch. backend_native.c / backend_web.c own the
// transports and call backend_dispatch_text() / backend_dispatch_event() on the
// main thread.
#include <backend.h>
#include "backend_internal.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

// ---- the brain gate (backend.h): the brain is not the active backend while the gate answers 1
static BackendBrainGateFn g_brain_gate;
static int g_refused_logged;

void backend_set_brain_gate(const BackendBrainGateFn fn) { g_brain_gate = fn; }

static int refused(const char *what) {
    // a poll that keeps asking must not flood the log: the first few only
    if (g_refused_logged < 4) {
        g_refused_logged++;
        fprintf(stderr, "[backend] %s not sent: the active backend is krunker.io (the brain is krunk.io's)\n", what);
    }
    return 1;
}

int backend_brain_gated(void) { return g_brain_gate && g_url[0] && g_brain_gate(); }

int backend_brain_refuses_path(void) {
    return backend_brain_gated() ? refused("brain request") : 0;
}

int backend_brain_refuses_url(const char *url) {
    if (!url || !g_brain_gate || !g_url[0]) return 0;
    const size_t n = strlen(g_url);
    if (strncmp(url, g_url, n) != 0 || (url[n] && url[n] != '/' && url[n] != '?')) return 0; // another host
    return g_brain_gate() ? refused("brain request") : 0; // the URL may carry a token: not logged
}

// ---- brain probe (default URLs)
// The probe decides whether the brain is there: the social socket (and with it the saved session's
// restore, account_ui.c) opens only once it answers 1. A probe that fails for a while (no answer by
// PROBE_TIMEOUT_S, a network error, 408 / 425 / 429 / 5xx) is asked again in the background, with a
// wait that grows to PROBE_RETRY_MAX_S, so one bad answer at page load does not leave the page a guest
// until the next reload. Meanwhile g_avail is 0, as for a page with no brain, so the matchmaker and the
// account windows do not wait on it. A definitive answer (200 without the brain, 404, ...) is final.
#define PROBE_TIMEOUT_S 20   // a probe with no answer by then has failed: its own request (BACKEND_HTTP_TIMEOUT_MS) and one retry come first
#define PROBE_RETRY_MAX_S 10 // the longest wait between probes after a failure (1, 2, 4, 8, then this)

static int g_avail = 1;        // backend_available()
static int g_probing;
static int g_social_wanted;    // something asked for the social socket while the probe ran
static unsigned g_probe_seq;   // the probe in flight: only its own answer counts (a timed-out one is dropped)
static time_t g_probe_started; // when the probe in flight was sent
static time_t g_retry_at;      // 0 = no background probe pending; else when the next one is due
static int g_retry_wait_s = 1; // the wait after the next failure

static int probe_transient(const int status) {
    return status == 0 || status == 408 || status == 425 || status == 429 || status >= 500;
}

// A probe failed for now: the page is a guest until a later probe finds the brain. The first failure of a
// run says so in the log; the ones after it are retried quietly.
static void probe_retry_later(const int status) {
    g_avail = 0;
    if (g_retry_wait_s == 1) {
        printf("[backend] no account server at %s (GET /mm/status: HTTP %d): social features are off for now, "
               "playing as a guest; asking again in %d s (run kn_brain, e.g. make backend-up, or pass ?brain=<url>)\n",
               backend_url(), status, g_retry_wait_s);
    }
    g_retry_at = time(NULL) + g_retry_wait_s;
    g_retry_wait_s = g_retry_wait_s * 2 > PROBE_RETRY_MAX_S ? PROBE_RETRY_MAX_S : g_retry_wait_s * 2;
}

static void probe_done(void *user, const int status, const char *body, const size_t length) {
    (void) length;
    if ((unsigned) (uintptr_t) user != g_probe_seq) return; // a probe that timed out: its late answer is dropped
    g_probing = 0;
    if (status == 200 && body && strstr(body, "\"brain\"")) {
        g_avail = 1;
        g_retry_at = 0;
        g_retry_wait_s = 1;
        printf("[backend] brain found at %s\n", backend_url());
        if (g_social_wanted) backend_social_open();
        return;
    }
    if (probe_transient(status)) {
        probe_retry_later(status);
        return;
    }
    g_avail = 0;
    g_retry_at = 0;
    g_retry_wait_s = 1;
    // e.g. `make web-serve`, or `make bridge` with no kn_brain behind it: say what that means rather
    // than leave it to a failing /social/ws (none is attempted) or a silent guest menu
    printf("[backend] no account server at %s (GET /mm/status: HTTP %d): social features are off, playing as a guest "
           "(run kn_brain, e.g. make backend-up, or pass ?brain=<url>)\n", backend_url(), status);
}

void backend_probe_start(void) {
    if (!backend_enabled() || g_probing || !backend_url_is_default()) return;
    // krunker.io is the active backend (the brain gate): no probe now, and the state stays as it is, so the
    // probe that runs after the player leaves the official server is a real one (not "no account server")
    if (backend_brain_refuses_path()) return;
    g_probing = 1;
    g_retry_at = 0;
    if (g_avail == 1) g_avail = -1;
    g_probe_seq++;
    g_probe_started = time(NULL);
    if (!backend_http_get_probe("/mm/status", probe_done, (void *) (uintptr_t) g_probe_seq)) {
        g_probing = 0;
        probe_retry_later(0); // not queued: asked again later, never stuck at 0
    }
}

// Every frame (backend_frame): a probe with no answer by PROBE_TIMEOUT_S has failed, and the background
// probe after a failure starts once its wait is over.
void backend_probe_tick(void) {
    if (!backend_enabled() || !backend_url_is_default()) return;
    const time_t now = time(NULL);
    if (g_probing && now - g_probe_started >= PROBE_TIMEOUT_S) {
        g_probe_seq++; // the answer, if one ever comes, is dropped
        g_probing = 0;
        probe_retry_later(0);
        return;
    }
    if (!g_probing && g_retry_at && now >= g_retry_at) backend_probe_start();
}

int backend_available(void) {
    if (!backend_enabled()) return 0;
    if (!backend_url_is_default()) return 1;
    return g_probing && g_avail == -1 ? -1 : g_avail;
}

int backend_social_allowed(void) {
    // the social socket is the brain's: it does not open while krunker.io is the backend (backend.h brain gate), so the
    // saved session's restore (account_ui.c, on "__open") never signs a Box Office account in from an official game
    if (backend_brain_refuses_path()) return 0;
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

static int g_social_hold;
static BackendHeldSender g_held_sender;

void backend_social_hold(const int on) { g_social_hold = on != 0; }
int backend_social_held(void) { return g_social_hold; }
void backend_social_set_held_sender(const BackendHeldSender fn) { g_held_sender = fn; }

int backend_social_send_text(const char *json) {
    if (g_social_hold) return json && g_held_sender ? g_held_sender(json) : 0;
    if (backend_brain_refuses_path()) return 0; // no frame to the brain's socket while krunker.io is the backend
    return backend_social_send_raw(json);
}

static int send_frame(const cJSON *frame, const int session) {
    if (!frame) return 0;
    if (g_social_hold && !session) {
        char *text = g_held_sender ? cJSON_PrintUnformatted(frame) : NULL;
        const int ok = text ? g_held_sender(text) : 0;
        free(text);
        return ok;
    }
    if (backend_brain_refuses_path()) return 0; // (the session frames too: a restore is the brain's sign-in)
    if (!backend_enabled()) return 0;
    char *text = cJSON_PrintUnformatted(frame);
    if (!text) return 0;
    const int ok = backend_social_send_raw(text);
    free(text);
    return ok;
}

int backend_social_send(const cJSON *frame) { return send_frame(frame, 0); }
int backend_social_send_session(const cJSON *frame) { return send_frame(frame, 1); }

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
