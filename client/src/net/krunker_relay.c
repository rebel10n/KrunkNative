// The krunker.io relay flag (client/include/krunker_relay.h): the bridge's /healthz, read once on the web.
#include <krunker_relay.h>
#include <backend.h>
#include <cJSON.h>
#include <stdlib.h>
#include <string.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#ifdef __EMSCRIPTEN__
static int g_started;    // the probe was sent (once per page)
static int g_answered;   // the answer came back, failed, or could not be sent
static int g_on;         // the answer said krunkerRelay: true
static double g_started_ms;

// The bridge's health URL: the origin of the endpoint the page talks to (?ws=, the bridge URL or
// this origin's /native, the same chain as net_web.c's net_web_target_url) with /healthz in place
// of /native. malloc'd; NULL when it cannot be built.
static char *healthz_url(void) {
    return (char *) EM_ASM_PTR({
        var base = null; // (no comma-separated declarations: EM_ASM is a macro)
        try { base = new URLSearchParams(location.search).get('ws'); } catch (e) {}
        if (!base && typeof Module !== 'undefined' && Module['krunkNativeWsUrl']) base = Module['krunkNativeWsUrl'];
        if (!base && typeof Module !== 'undefined' && Module['krunkNativeBridgeUrl']) base = Module['krunkNativeBridgeUrl'];
        if (!base) base = (location.protocol === 'https:' ? 'wss' : 'ws') + '://' + location.host + '/native';
        try {
            var s = new URL(base, location.href).href.split('?')[0].split('#')[0];
            if (s.indexOf('wss:') === 0) s = 'https:' + s.slice(4);
            else if (s.indexOf('ws:') === 0) s = 'http:' + s.slice(3);
            while (s.charAt(s.length - 1) === '/') s = s.slice(0, -1);
            if (s.slice(-7) === '/native') s = s.slice(0, -7);
            return stringToNewUTF8(s + '/healthz');
        } catch (e) {
            return 0;
        }
    });
}

static void probe_done(void *user, const int status, const char *body, const size_t length) {
    (void) user;
    g_answered = 1;
    if (status != 200 || !body || !length) return;
    char *text = malloc(length + 1);
    if (!text) return;
    memcpy(text, body, length);
    text[length] = 0;
    cJSON *root = cJSON_Parse(text);
    free(text);
    if (!root) return;
    g_on = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "krunkerRelay")) ? 1 : 0;
    cJSON_Delete(root);
}

void krunker_relay_probe(void) {
    if (g_started) return;
    g_started = 1;
    g_started_ms = emscripten_get_now();
    char *url = healthz_url();
    if (!url) {
        g_answered = 1;
        return;
    }
    if (!backend_http_request("GET", url, NULL, probe_done, NULL)) g_answered = 1;
    free(url);
}

int krunker_relay_on(void) { return g_on; }

int krunker_relay_probing(void) {
    if (!g_started || g_answered) return 0;
    return emscripten_get_now() - g_started_ms < KRUNKER_RELAY_PROBE_MS;
}

#else // native: krunker.io is joined directly, the relay is never asked for

void krunker_relay_probe(void) {}
int krunker_relay_on(void) { return 0; }
int krunker_relay_probing(void) { return 0; }

#endif
