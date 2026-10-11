// Brain client, web transports (client/include/backend.h): XMLHttpRequest through
// emscripten_async_wget2_data and the browser WebSocket (emscripten/websocket.h, the
// same library net_web.c uses). Callbacks already run on the main thread; HTTP
// completions and social frames are still delivered from backend_poll() so both
// platforms call back at the same point of the frame.
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
        if (params && params.get('offline') && params.get('offline') !== '0') return 0;
        if (params) url = params.get('brain');
        if (url === null && typeof Module !== 'undefined' && Module['krunkNativeBrainUrl'] !== undefined) url = Module['krunkNativeBrainUrl'];
        if (url === null && location.protocol.indexOf('http') === 0) url = '\x01' + location.origin;
        return url ? stringToNewUTF8(String(url)) : 0;
    });
    g_url_default = url && url[0] == '\x01';
    if (g_url_default) memmove(url, url + 1, strlen(url));
    backend_store_url(url);
    free(url);
    if (backend_enabled()) printf("[backend] brain %s\n", backend_url());
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
} WebReq;

static void http_onload(unsigned handle, void *arg, void *data, unsigned size) {
    (void) handle;
    WebReq *req = arg;
    WebItem *item = calloc(1, sizeof(*item));
    if (item) {
        item->fn = req->fn;
        item->user = req->user;
        item->status = 200;
        item->body = malloc(size + 1);
        if (item->body) {
            memcpy(item->body, data, size);
            item->body[size] = 0;
            item->length = size;
        } else {
            item->status = 0;
        }
        ready_push(item);
    }
    free(req);
}

static void http_onerror(unsigned handle, void *arg, int status, const char *text) {
    (void) handle;
    (void) text;
    WebReq *req = arg;
    WebItem *item = calloc(1, sizeof(*item));
    if (item) {
        item->fn = req->fn;
        item->user = req->user;
        item->status = status; // 0 = network error; the body is not available here
        ready_push(item);
    }
    free(req);
}

int backend_http_get(const char *path, const BackendHttpFn fn, void *user) {
    if (!backend_enabled() || !path) return 0;
    WebReq *req = calloc(1, sizeof(*req));
    if (!req) return 0;
    req->fn = fn;
    req->user = user;
    const size_t n = strlen(backend_url()) + strlen(path) + 1;
    char *url = malloc(n);
    if (!url) {
        free(req);
        return 0;
    }
    snprintf(url, n, "%s%s", backend_url(), path);
    emscripten_async_wget2_data(url, "GET", "", req, 1, http_onload, http_onerror, NULL);
    free(url);
    return 1;
}

// Any URL / method with a JSON body (auth service, server finder): the browser fetch()
// (wget2 cannot send a body). The completion comes back through backend_web_fetch_done.
EMSCRIPTEN_KEEPALIVE void backend_web_fetch_done(WebReq *req, const int status, char *body, const int length) {
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
    free(req);
}

EM_JS(void, backend_web_fetch, (const char *method, const char *url, const char *body, void *req), {
    var opts = { method: UTF8ToString(method), headers: { 'Accept': 'application/json' } };
    if (body) {
        opts.body = UTF8ToString(body);
        opts.headers['Content-Type'] = 'application/json';
    }
    fetch(UTF8ToString(url), opts).then(function (r) {
        return r.text().then(function (t) {
            var p = stringToNewUTF8(t);
            _backend_web_fetch_done(req, r.status, p, 0);
        });
    }).catch(function () { _backend_web_fetch_done(req, 0, 0, 0); });
});

int backend_http_request(const char *method, const char *url, const char *json_body, const BackendHttpFn fn, void *user) {
    if (!url || !url[0]) return 0;
    WebReq *req = calloc(1, sizeof(*req));
    if (!req) return 0;
    req->fn = fn;
    req->user = user;
    backend_web_fetch(method && method[0] ? method : "GET", url, json_body, req);
    return 1;
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

int backend_social_send_text(const char *json) {
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
