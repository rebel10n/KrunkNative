// Brain client, web transports (client/include/backend.h): the browser fetch() (status
// and body for every answer, errors included) and the browser WebSocket (emscripten/websocket.h, the
// same library net_web.c uses). Callbacks already run on the main thread; HTTP
// completions and social frames are still delivered from backend_poll() so both
// platforms call back at the same point of the frame.
#include "klog.h"
#ifdef __EMSCRIPTEN__
#include <backend.h>
#include "backend_internal.h"
#include <emscripten.h>
#include <emscripten/websocket.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WS_BACKOFF_MIN_MS 1000
#define WS_BACKOFF_MAX_MS 10000

typedef struct WebItem {
    struct WebItem *next;
    BackendHttpFn fn;  // HTTP completion when set
    void *user;
    int status;
    char *body;        // HTTP body / social frame text
    size_t length;
    const char *event; // social synthetic event
} WebItem;

static WebItem *g_ready;
static EMSCRIPTEN_WEBSOCKET_T g_ws;
static int g_ws_open, g_ws_wanted, g_ws_retry_pending, g_backoff = WS_BACKOFF_MIN_MS;
static char **g_out;
static int g_url_default;          // backend_url() is the page origin, not an explicit ?brain=
static size_t g_out_count;

static void ws_event(const char *event);

static void ready_push(WebItem *item) {
    WebItem **link = &g_ready;
    while (*link) link = &(*link)->next;
    *link = item;
}

void backend_init(const char *cli_url) {
    if (cli_url && !cli_url[0]) { // offline mode (online_mode.h): no account service
        backend_store_url("");
        return;
    }
    char *url = (char *) EM_ASM_PTR({
        var url = null;
        var params = null;
        try { params = new URLSearchParams(location.search); } catch (e) {}
        // same truthiness as main.c web_query_flag: bare ?offline / ?sandbox count
        var off = function (name) { var v = params ? params.get(name) : null; return v !== null && v !== '0' && v !== 'false'; };
        if (off('offline') || off('sandbox') || off('test')) return 0;
        if (params) url = params.get('brain');
        if (url === null && typeof Module !== 'undefined' && Module['krunkNativeBrainUrl'] !== undefined) url = Module['krunkNativeBrainUrl'];
        if (url === null && location.protocol.indexOf('http') === 0) url = '\x01' + location.origin;
        return url ? stringToNewUTF8(String(url)) : 0;
    });
    g_url_default = url && url[0] == '\x01';
    if (g_url_default) memmove(url, url + 1, strlen(url));
    backend_store_url(url);
    free(url);
    if (backend_enabled()) KN_LOG_DEBUG1("[backend] brain %s\n", backend_url());
}

int backend_url_is_default(void) { return g_url_default && backend_enabled(); }

void backend_set_url(const char *url) {
    char before[256];
    snprintf(before, sizeof(before), "%s", backend_url());
    g_url_default = 0;
    backend_store_url(url);
    if (!strcmp(before, backend_url())) return; // same brain: keep the socket
    if (g_ws) {
        emscripten_websocket_close(g_ws, 1000, "url");
        emscripten_websocket_delete(g_ws);
        g_ws = 0;
        if (g_ws_open) ws_event("__close");
        g_ws_open = 0;
    }
    if (g_ws_wanted) backend_social_open();
}

// ------------------------------------------------------------------ HTTP

typedef struct {
    BackendHttpFn fn;
    void *user;
    char *remote; // a krunker.io read (backend_http_request_remote): its path, logged when it is not answered 200
    char *call;   // a request with the brain timeout (backend_http_request): "GET <url without its query>", logged when no answer
} WebReq;

// "GET https://host/mm/host-game" for the log: the URL without its query, where a token may be (never printed)
static char *call_label(const char *method, const char *url) {
    const char *query = strchr(url, '?');
    const size_t path = query ? (size_t) (query - url) : strlen(url);
    const size_t n = strlen(method) + 1 + path + 1;
    char *label = malloc(n);
    if (label) snprintf(label, n, "%s %.*s", method, (int) path, url);
    return label;
}

// Any URL / method with a JSON body (auth service, server finder): the browser fetch()
// (wget2 cannot send a body). The completion comes back through backend_web_fetch_done, exactly once: it frees req.
EMSCRIPTEN_KEEPALIVE void backend_web_fetch_done(WebReq *req, const int status, char *body, const int length) {
    if (req->call && status == 0) fprintf(stderr, "[http] %s failed: %s\n", req->call, body ? body : "");
    WebItem *item = calloc(1, sizeof(*item));
    if (item) {
        item->fn = req->fn;
        item->user = req->user;
        item->status = status;
        item->body = body; // malloc'd by the JS side (stringToNewUTF8), owned now
        item->length = body ? strlen(body) : 0;
        (void) length;
        ready_push(item);
    } else {
        free(body);
    }
    if (req->remote) {
        // status 0: the browser refused the read (CORS, or no answer), so the panel that asked stays empty
        if (status == 0) fprintf(stderr, "[backend] krunker.io read %s: no answer (the browser refused it, CORS or the network)\n", req->remote);
        else if (status != 200) fprintf(stderr, "[backend] krunker.io read %s: status %d\n", req->remote, status);
        free(req->remote);
    }
    free(req->call);
    free(req);
}

// timeout_ms > 0 (a brain request): no answer by then is a status 0 whose body is the reason, as on native (the socket's
// timeout, backend_native.c). fetch() has no timeout of its own, so the request is aborted (which also closes the
// connection) from a browser timer, whatever the frame rate. 0 = no timeout. Exactly one completion per request.
EM_JS(void, backend_web_fetch, (const char *method, const char *url, const char *body, void *req, int timeout_ms), {
    var opts = { method: UTF8ToString(method), headers: { 'Accept': 'application/json' } };
    if (body) {
        opts.body = UTF8ToString(body);
        opts.headers['Content-Type'] = 'application/json';
    }
    var ctl = timeout_ms > 0 && typeof AbortController !== 'undefined' ? new AbortController() : null;
    var timedOut = false, settled = false, timer = 0;
    if (ctl) {
        opts.signal = ctl.signal;
        timer = setTimeout(function () { timedOut = true; ctl.abort(); }, timeout_ms);
    }
    var finish = function (status, text) {
        if (settled) return;
        settled = true;
        if (timer) clearTimeout(timer);
        _backend_web_fetch_done(req, status, text === null ? 0 : stringToNewUTF8(text), 0);
    };
    fetch(UTF8ToString(url), opts).then(function (r) {
        return r.text().then(function (t) { finish(r.status, t); });
    }).catch(function () {
        finish(0, timedOut ? 'no answer within ' + Math.round(timeout_ms / 1000) + ' s (timed out)'
                           : 'no answer (the browser refused it, CORS or the network)');
    });
});

int backend_http_request(const char *method, const char *url, const char *json_body, const BackendHttpFn fn, void *user) {
    if (!url || !url[0] || backend_brain_refuses_url(url)) return 0;
    WebReq *req = calloc(1, sizeof(*req));
    if (!req) return 0;
    req->fn = fn;
    req->user = user;
    const char *verb = method && method[0] ? method : "GET";
    req->call = call_label(verb, url);
    backend_web_fetch(verb, url, json_body, req, BACKEND_HTTP_TIMEOUT_MS);
    return 1;
}

// A krunker.io read (backend.h backend_http_get_remote): the same fetch, its path kept for the log line
int backend_http_request_remote(const char *url, const BackendHttpFn fn, void *user) {
    if (!url || !url[0]) return 0;
    WebReq *req = calloc(1, sizeof(*req));
    if (!req) return 0;
    req->fn = fn;
    req->user = user;
    const char *query = strchr(url, '?');
    const size_t n = query ? (size_t) (query - url) : strlen(url);
    req->remote = calloc(n + 1, 1);
    if (req->remote) memcpy(req->remote, url, n);
    backend_web_fetch("GET", url, NULL, req, 0);
    return 1;
}

// The brain's own routes. fetch() like backend_http_request: wget2's error callback carries
// no body, so a 403 {"error":"AccountRequired"} / 404 InvalidGameId reached the matchmaker as
// a bare status (shown as "Game is full.").
int backend_http_get(const char *path, const BackendHttpFn fn, void *user) {
    if (!backend_enabled() || !path || backend_brain_refuses_path()) return 0;
    const size_t n = strlen(backend_url()) + strlen(path) + 1;
    char *url = malloc(n);
    if (!url) return 0;
    snprintf(url, n, "%s%s", backend_url(), path);
    const int ok = backend_http_request("GET", url, NULL, fn, user);
    free(url);
    return ok;
}

// The web build has one brain lane (fetch): the probe is an ordinary GET.
int backend_http_get_probe(const char *path, const BackendHttpFn fn, void *user) {
    return backend_http_get(path, fn, user);
}

// ------------------------------------------------------------------ social socket

static void ws_event(const char *event) {
    WebItem *item = calloc(1, sizeof(*item));
    if (!item) return;
    item->event = event;
    ready_push(item);
}

static void ws_retry(void *arg) {
    (void) arg;
    g_ws_retry_pending = 0;
    if (g_ws_wanted && !g_ws) backend_social_open();
}

static void ws_schedule_retry(void) {
    if (g_ws_retry_pending || !g_ws_wanted) return;
    g_ws_retry_pending = 1;
    emscripten_async_call(ws_retry, NULL, g_backoff);
    g_backoff = g_backoff * 2 > WS_BACKOFF_MAX_MS ? WS_BACKOFF_MAX_MS : g_backoff * 2;
}

static bool ws_on_open(int type, const EmscriptenWebSocketOpenEvent *e, void *user) {
    (void) type;
    (void) user;
    if (e->socket != g_ws) return true;
    g_ws_open = 1;
    g_backoff = WS_BACKOFF_MIN_MS;
    ws_event("__open");
    for (size_t i = 0; i < g_out_count; i++) {
        emscripten_websocket_send_utf8_text(g_ws, g_out[i]);
        free(g_out[i]);
    }
    g_out_count = 0;
    return true;
}

static bool ws_on_message(int type, const EmscriptenWebSocketMessageEvent *e, void *user) {
    (void) type;
    (void) user;
    if (e->socket != g_ws || !e->isText) return true; // binary = v3.1.1 msgpack, not used here
    WebItem *item = calloc(1, sizeof(*item));
    if (!item) return true;
    item->body = malloc(e->numBytes + 1);
    if (!item->body) {
        free(item);
        return true;
    }
    memcpy(item->body, e->data, e->numBytes);
    item->body[e->numBytes] = 0;
    ready_push(item);
    return true;
}

static void ws_dropped(const EMSCRIPTEN_WEBSOCKET_T socket) {
    if (socket != g_ws) return;
    emscripten_websocket_delete(g_ws);
    g_ws = 0;
    if (g_ws_open) ws_event("__close");
    g_ws_open = 0;
    ws_schedule_retry();
}

static bool ws_on_close(int type, const EmscriptenWebSocketCloseEvent *e, void *user) {
    (void) type;
    (void) user;
    ws_dropped(e->socket);
    return true;
}

static bool ws_on_error(int type, const EmscriptenWebSocketErrorEvent *e, void *user) {
    (void) type;
    (void) user;
    ws_dropped(e->socket);
    return true;
}

void backend_social_open(void) {
    if (!backend_enabled() || !backend_social_allowed()) return;
    g_ws_wanted = 1;
    if (g_ws || g_ws_retry_pending) return;
    char url[320];
    backend_social_ws_url(url, sizeof(url));
    EmscriptenWebSocketCreateAttributes attr;
    emscripten_websocket_init_create_attributes(&attr);
    attr.url = url;
    attr.protocols = NULL;
    attr.createOnMainThread = EM_TRUE;
    const EMSCRIPTEN_WEBSOCKET_T ws = emscripten_websocket_new(&attr);
    if (ws <= 0) {
        ws_schedule_retry();
        return;
    }
    g_ws = ws;
    emscripten_websocket_set_onopen_callback(ws, NULL, ws_on_open);
    emscripten_websocket_set_onmessage_callback(ws, NULL, ws_on_message);
    emscripten_websocket_set_onclose_callback(ws, NULL, ws_on_close);
    emscripten_websocket_set_onerror_callback(ws, NULL, ws_on_error);
}

int backend_social_is_open(void) { return g_ws_open; }

int backend_social_send_raw(const char *json) {
    if (!backend_enabled() || !json) return 0;
    backend_social_open();
    if (g_ws && g_ws_open) return emscripten_websocket_send_utf8_text(g_ws, json) == EMSCRIPTEN_RESULT_SUCCESS;
    char **grown = realloc(g_out, (g_out_count + 1) * sizeof(*g_out));
    if (!grown) return 0;
    g_out = grown;
    g_out[g_out_count] = strdup(json);
    if (!g_out[g_out_count]) return 0;
    g_out_count++;
    return 1;
}

// ------------------------------------------------------------------ main thread

void backend_poll(void) {
    WebItem *items = g_ready;
    g_ready = NULL;
    while (items) {
        WebItem *next = items->next;
        if (items->fn) items->fn(items->user, items->status, items->body ? items->body : "", items->length);
        else if (items->body) backend_dispatch_text(items->body);
        else if (items->event) backend_dispatch_event(items->event);
        free(items->body);
        free(items);
        items = next;
    }
}
#endif // __EMSCRIPTEN__
