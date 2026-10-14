// WinHTTP transport of the brain client (backend_winhttp.h): Windows has no http_conn.c
// (POSIX sockets + OpenSSL), so HTTP(S) and the WS(S) social socket go through WinHTTP, which
// brings TLS (the Windows certificate store), the system proxy and WebSockets (Windows 8+).
// The client's own proxy (http_proxy.h, http:// only here) is set per request over the system one.
#ifdef _WIN32
#include "backend_winhttp.h"
#include <http_proxy.h>
#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KN_WINHTTP_MAX_BODY (4u * 1024u * 1024u)
#define KN_WINWS_MAX_MESSAGE (2u * 1024u * 1024u)

static void set_err(char *err, const size_t errn, const char *what, const DWORD code) {
    if (!err || !errn) return;
    if (code) snprintf(err, errn, "%s (WinHTTP error %lu)", what, (unsigned long) code);
    else snprintf(err, errn, "%s", what);
}

static wchar_t *widen(const char *s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = n > 0 ? malloc((size_t) n * sizeof(wchar_t)) : NULL;
    if (w) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

static HINTERNET open_session(const int timeout_ms) {
    HINTERNET s = WinHttpOpen(L"KrunkNative", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) // before Windows 8.1: no automatic proxy
        s = WinHttpOpen(L"KrunkNative", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (s) WinHttpSetTimeouts(s, timeout_ms, timeout_ms, timeout_ms, timeout_ms);
    return s;
}

typedef struct {
    HINTERNET session, connect, request;
} Handles;

static void close_handles(Handles *h) {
    if (h->request) WinHttpCloseHandle(h->request);
    if (h->connect) WinHttpCloseHandle(h->connect);
    if (h->session) WinHttpCloseHandle(h->session);
    memset(h, 0, sizeof(*h));
}

// session + connection + request for url (ws(s):// mapped to http(s)://). 0 on failure.
static int open_request(Handles *h, const char *method, const char *url, const int timeout_ms, char *err, const size_t errn) {
    memset(h, 0, sizeof(*h));
    char mapped[2048];
    if (!strncmp(url, "wss://", 6)) snprintf(mapped, sizeof(mapped), "https://%s", url + 6);
    else if (!strncmp(url, "ws://", 5)) snprintf(mapped, sizeof(mapped), "http://%s", url + 5);
    else snprintf(mapped, sizeof(mapped), "%s", url);
    wchar_t *wurl = widen(mapped);
    wchar_t *wmethod = widen(method);
    URL_COMPONENTS uc;
    memset(&uc, 0, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256];
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.dwUrlPathLength = (DWORD) -1; // pointers into wurl
    uc.dwExtraInfoLength = (DWORD) -1;
    int ok = 0;
    if (!wurl || !wmethod || !WinHttpCrackUrl(wurl, 0, 0, &uc)) {
        set_err(err, errn, "bad URL", wurl ? GetLastError() : 0);
        goto out;
    }
    // path + ?query (the crack points into wurl: the path runs on into the extra info)
    wchar_t path[2048];
    const size_t plen = uc.dwUrlPathLength + uc.dwExtraInfoLength;
    if (plen >= 2048) {
        set_err(err, errn, "URL too long", 0);
        goto out;
    }
    if (plen) memcpy(path, uc.lpszUrlPath ? uc.lpszUrlPath : uc.lpszExtraInfo, plen * sizeof(wchar_t));
    path[plen] = 0;
    h->session = open_session(timeout_ms);
    if (!h->session) {
        set_err(err, errn, "WinHttpOpen failed", GetLastError());
        goto out;
    }
    h->connect = WinHttpConnect(h->session, host, uc.nPort, 0);
    if (!h->connect) {
        set_err(err, errn, "cannot connect", GetLastError());
        goto out;
    }
    const DWORD flags = uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    h->request = WinHttpOpenRequest(h->connect, wmethod, path[0] ? path : L"/", NULL, WINHTTP_NO_REFERER,
                                    WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!h->request) {
        set_err(err, errn, "cannot open the request", GetLastError());
        goto out;
    }
    char host8[256]; // Settings > Client > Proxy / HTTPS_PROXY ... (http_proxy.h): this request's proxy
    if (WideCharToMultiByte(CP_UTF8, 0, host, -1, host8, sizeof(host8), NULL, NULL))
        http_proxy_winhttp_apply(h->request, host8, uc.nScheme == INTERNET_SCHEME_HTTPS);
    ok = 1;
out:
    free(wurl);
    free(wmethod);
    if (!ok) close_handles(h);
    return ok;
}

// The error text of a failed send / receive: the common causes by name.
static void request_error(char *err, const size_t errn, const DWORD code) {
    const char *what = "request failed";
    switch (code) {
    case ERROR_WINHTTP_NAME_NOT_RESOLVED: what = "host not found"; break;
    case ERROR_WINHTTP_CANNOT_CONNECT: what = "connection refused"; break;
    case ERROR_WINHTTP_TIMEOUT: what = "timed out"; break;
    case ERROR_WINHTTP_SECURE_FAILURE: what = "TLS certificate rejected"; break;
    case ERROR_WINHTTP_CONNECTION_ERROR: what = "connection reset"; break;
    default: break;
    }
    set_err(err, errn, what, code);
}
int kn_winhttp_request(const char *method, const char *url, const char *json_body, const int timeout_ms, char **body,
                       size_t *length, char *err, const size_t errn) {
    *body = NULL;
    if (length) *length = 0;
    Handles h;
    if (!open_request(&h, method && method[0] ? method : "GET", url, timeout_ms, err, errn)) return 0;
    const wchar_t *headers = json_body ? L"Accept: application/json\r\nContent-Type: application/json\r\n" : L"Accept: application/json\r\n";
    const DWORD body_len = json_body ? (DWORD) strlen(json_body) : 0;
    int status = 0;
    if (!WinHttpSendRequest(h.request, headers, (DWORD) -1L, (void *) json_body, body_len, body_len, 0) ||
        !WinHttpReceiveResponse(h.request, NULL)) {
        request_error(err, errn, GetLastError());
        close_handles(&h);
        return 0;
    }
    DWORD code = 0, size = sizeof(code);
    WinHttpQueryHeaders(h.request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &size,
                        WINHTTP_NO_HEADER_INDEX);
    char *data = NULL;
    size_t len = 0, cap = 0;
    for (;;) {
        char buf[16384];
        DWORD got = 0;
        if (!WinHttpReadData(h.request, buf, sizeof(buf), &got)) {
            request_error(err, errn, GetLastError());
            free(data);
            close_handles(&h);
            return 0;
        }
        if (!got) break;
        if (len + got + 1 > cap) {
            size_t ncap = cap ? cap * 2 : 8192;
            while (ncap < len + got + 1) ncap *= 2;
            char *grown = realloc(data, ncap);
            if (!grown) break;
            data = grown;
            cap = ncap;
        }
        memcpy(data + len, buf, got); // WinHTTP removes chunked encoding itself
        len += got;
        if (len > KN_WINHTTP_MAX_BODY) {
            set_err(err, errn, "response too large", 0);
            free(data);
            close_handles(&h);
            return 0;
        }
    }
    if (!data) data = calloc(1, 1);
    if (data) data[len] = 0;
    status = (int) code;
    close_handles(&h);
    *body = data;
    if (length) *length = len;
    return data ? status : 0;
}

int kn_winhttp_stream(const char *method, const char *url, const char *headers, const char *body, const int timeout_ms,
                      void (*on_head)(void *user, int status, const char *content_type), int (*on_data)(void *user, const char *data, size_t len),
                      void *user, char *err, const size_t errn) {
    Handles h;
    if (!open_request(&h, method && method[0] ? method : "GET", url, timeout_ms, err, errn)) return 0;
    // the defaults + the caller's lines, as one wide header block
    char all[4096];
    snprintf(all, sizeof(all), "Accept: text/event-stream, application/json\r\n%s%s", body ? "Content-Type: application/json\r\n" : "",
             headers ? headers : "");
    wchar_t *wh = widen(all);
    const DWORD body_len = body ? (DWORD) strlen(body) : 0;
    const int sent = wh && WinHttpSendRequest(h.request, wh, (DWORD) -1L, (void *) body, body_len, body_len, 0) &&
                     WinHttpReceiveResponse(h.request, NULL);
    free(wh);
    if (!sent) {
        request_error(err, errn, GetLastError());
        close_handles(&h);
        return 0;
    }
    DWORD code = 0, size = sizeof(code);
    WinHttpQueryHeaders(h.request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &size,
                        WINHTTP_NO_HEADER_INDEX);
    wchar_t wtype[128] = L"";
    DWORD tsize = sizeof(wtype);
    char ctype[128] = "";
    if (WinHttpQueryHeaders(h.request, WINHTTP_QUERY_CONTENT_TYPE, WINHTTP_HEADER_NAME_BY_INDEX, wtype, &tsize, WINHTTP_NO_HEADER_INDEX))
        WideCharToMultiByte(CP_UTF8, 0, wtype, -1, ctype, sizeof(ctype), NULL, NULL);
    if (on_head) on_head(user, (int) code, ctype);
    for (;;) {
        char buf[8192];
        DWORD got = 0;
        if (!WinHttpReadData(h.request, buf, sizeof(buf), &got)) {
            request_error(err, errn, GetLastError());
            close_handles(&h);
            return 0;
        }
        if (!got) break;
        if (on_data && !on_data(user, buf, got)) {
            set_err(err, errn, "cancelled", 0);
            close_handles(&h);
            return 0;
        }
    }
    close_handles(&h);
    return (int) code;
}

// ------------------------------------------------------------------ WebSocket (Windows 8+)

// The WebSocket functions are looked up at run time: winhttp.dll of Windows 7 lacks them, and
// a static import would stop the whole client from starting there.
typedef HINTERNET(WINAPI *CompleteUpgradeFn)(HINTERNET, DWORD_PTR);
typedef DWORD(WINAPI *WsSendFn)(HINTERNET, WINHTTP_WEB_SOCKET_BUFFER_TYPE, PVOID, DWORD);
typedef DWORD(WINAPI *WsReceiveFn)(HINTERNET, PVOID, DWORD, DWORD *, WINHTTP_WEB_SOCKET_BUFFER_TYPE *);
typedef DWORD(WINAPI *WsCloseFn)(HINTERNET, USHORT, PVOID, DWORD);
static struct {
    int loaded;
    CompleteUpgradeFn upgrade;
    WsSendFn send;
    WsReceiveFn receive;
    WsCloseFn close;
} g_ws_api;

static int ws_api(void) {
    if (!g_ws_api.loaded) {
        g_ws_api.loaded = 1;
        HMODULE m = GetModuleHandleW(L"winhttp.dll");
        if (!m) m = LoadLibraryW(L"winhttp.dll");
        if (m) {
            g_ws_api.upgrade = (CompleteUpgradeFn) (void *) GetProcAddress(m, "WinHttpWebSocketCompleteUpgrade");
            g_ws_api.send = (WsSendFn) (void *) GetProcAddress(m, "WinHttpWebSocketSend");
            g_ws_api.receive = (WsReceiveFn) (void *) GetProcAddress(m, "WinHttpWebSocketReceive");
            g_ws_api.close = (WsCloseFn) (void *) GetProcAddress(m, "WinHttpWebSocketClose");
        }
    }
    return g_ws_api.upgrade && g_ws_api.send && g_ws_api.receive && g_ws_api.close;
}

struct KnWinWs {
    HINTERNET session, connect, socket;
    volatile LONG aborted;
};

KnWinWs *kn_winws_open(const char *url, const int timeout_ms, char *err, const size_t errn) {
    if (!ws_api()) {
        set_err(err, errn, "WinHTTP has no WebSocket support (Windows 8 or newer needed)", 0);
        return NULL;
    }
    Handles h;
    if (!open_request(&h, "GET", url, timeout_ms, err, errn)) return NULL;
    if (!WinHttpSetOption(h.request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, NULL, 0) ||
        !WinHttpSendRequest(h.request, L"User-Agent: KrunkNative\r\n", (DWORD) -1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(h.request, NULL)) {
        request_error(err, errn, GetLastError());
        close_handles(&h);
        return NULL;
    }
    DWORD code = 0, size = sizeof(code);
    WinHttpQueryHeaders(h.request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &size,
                        WINHTTP_NO_HEADER_INDEX);
    HINTERNET socket = code == 101 ? g_ws_api.upgrade(h.request, 0) : NULL;
    if (!socket) {
        char what[64];
        snprintf(what, sizeof(what), "WebSocket upgrade refused (HTTP %lu)", (unsigned long) code);
        set_err(err, errn, what, code == 101 ? GetLastError() : 0);
        close_handles(&h);
        return NULL;
    }
    WinHttpCloseHandle(h.request); // the socket handle lives on its own
    KnWinWs *ws = calloc(1, sizeof(*ws));
    if (!ws) {
        WinHttpCloseHandle(socket);
        WinHttpCloseHandle(h.connect);
        WinHttpCloseHandle(h.session);
        return NULL;
    }
    ws->session = h.session;
    ws->connect = h.connect;
    ws->socket = socket;
    // no receive timeout: the social socket sits idle between frames (the brain pings it)
    DWORD none = 0;
    WinHttpSetOption(ws->socket, WINHTTP_OPTION_RECEIVE_TIMEOUT, &none, sizeof(none));
    return ws;
}

int kn_winws_send_text(KnWinWs *ws, const char *text, const size_t len) {
    if (!ws || ws->aborted) return 0;
    return g_ws_api.send(ws->socket, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, (PVOID) text, (DWORD) len) == NO_ERROR;
}

int kn_winws_recv(KnWinWs *ws, char **out, size_t *len, int *is_text) {
    *out = NULL;
    *len = 0;
    char *msg = NULL;
    size_t n = 0, cap = 0;
    for (;;) {
        if (ws->aborted) break;
        if (n + 8192 + 1 > cap) {
            size_t ncap = cap ? cap * 2 : 16384;
            char *grown = realloc(msg, ncap);
            if (!grown) break;
            msg = grown;
            cap = ncap;
        }
        DWORD got = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
        if (g_ws_api.receive(ws->socket, msg + n, (DWORD) (cap - n - 1), &got, &type) != NO_ERROR) break;
        if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) break;
        n += got;
        if (n > KN_WINWS_MAX_MESSAGE) break;
        if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE || type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) {
            msg[n] = 0;
            *out = msg;
            *len = n;
            *is_text = type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE;
            return 1;
        }
        // *_FRAGMENT_BUFFER_TYPE: more of the same message follows
    }
    free(msg);
    return 0;
}

void kn_winws_abort(KnWinWs *ws) {
    if (!ws) return;
    InterlockedExchange(&ws->aborted, 1);
    g_ws_api.close(ws->socket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, NULL, 0);
}

void kn_winws_free(KnWinWs *ws) {
    if (!ws) return;
    if (ws->socket) WinHttpCloseHandle(ws->socket);
    if (ws->connect) WinHttpCloseHandle(ws->connect);
    if (ws->session) WinHttpCloseHandle(ws->session);
    free(ws);
}
#endif
