#pragma once
// Outgoing proxy of the native client's HTTP(S) / WS(S) connections (client/src/net/http_proxy.c).
//
// Settings > Client > Network > Proxy (ui_client_settings.c) configures it: http://, https://,
// socks5:// (the client resolves the target) or socks5h:// (the proxy resolves it), with an
// optional user:password@, and a scope: krunker.io only (the default: *.krunker.io, the official
// lobby list krunker.io blocks some networks from) or all client traffic. With the setting empty
// the standard environment applies to every host: HTTPS_PROXY / https_proxy for https:// and
// wss:// URLs, HTTP_PROXY / http_proxy for http:// and ws://, then ALL_PROXY / all_proxy, minus
// NO_PROXY / no_proxy ("*", host names and domain suffixes); KN_FETCH_PROXY (a dev convention, unset
// for players) goes before them for krunker.io hosts. Loopback hosts never go through it.
//
// What uses it: http_conn.c (POSIX: Linux, macOS; the brain's HTTP + social WebSocket, streamed
// requests, the asset fetch, the launcher's downloads) tunnels every proxied connection, plain or
// TLS, through HTTP CONNECT or SOCKS5; WinHTTP (Windows: backend_winhttp.c, launcher/src/http_fetch.c)
// takes http:// proxies per request (WinHTTP has no SOCKS, so socks5 / https proxies are skipped
// there with a warning and WinHTTP's system proxy stays). The game connection itself (TCP / UDP /
// WebTransport to a game server) never goes through it. The web build has none of this: the
// browser is the network stack.
#if !defined(__EMSCRIPTEN__)
#include <stddef.h>

typedef enum {
    HTTP_PROXY_NONE = 0,
    HTTP_PROXY_HTTP,   // http://host[:80]: CONNECT host:port
    HTTP_PROXY_HTTPS,  // https://host[:443]: TLS to the proxy, then CONNECT inside it (POSIX only)
    HTTP_PROXY_SOCKS5, // socks5://host[:1080]: the client resolves the target
    HTTP_PROXY_SOCKS5H // socks5h://host[:1080]: the proxy resolves the target name
} HttpProxyKind;

typedef enum {
    HTTP_PROXY_SCOPE_KRUNKER = 0, // krunker.io and its subdomains (+ crucible.frvr.com, the krunker.io account's refresh)
    HTTP_PROXY_SCOPE_ALL = 1,     // every HTTP(S) / WS(S) connection of the client
} HttpProxyScope;

typedef struct {
    HttpProxyKind kind;
    char host[128];
    char port[8];
    char user[128], pass[128]; // percent-decoded credentials, "" = none
} HttpProxy;

// scheme://[user[:pass]@]host[:port][/]; a bare host[:port] is http://. 1 = valid ("" = 0).
int http_proxy_parse(const char *url, HttpProxy *out);
// The client setting (process-wide, any thread): url "" = the environment variables. Logged.
void http_proxy_configure(const char *url, HttpProxyScope scope);
// The proxy a connection to host goes through (tls: an https:// / wss:// URL). 1 = *out is set.
int http_proxy_for(const char *host, int tls, HttpProxy *out);
// host is krunker.io or one of its subdomains, or crucible.frvr.com (the krunker.io account's auth service)
int http_proxy_host_is_krunker(const char *host);
// Bumped by every http_proxy_configure: kept-alive connections opened before it are dropped.
unsigned http_proxy_generation(void);
// "http://host:port" of p (no credentials), for logs and the UI
void http_proxy_label(const HttpProxy *p, char *out, size_t n);
// base64 of user:pass (Proxy-Authorization: Basic), "" without credentials
void http_proxy_basic_auth(const HttpProxy *p, char *out, size_t n);

#ifdef _WIN32
// WinHTTP: sets the request's proxy (+ Basic credentials) when host goes through an http://
// proxy; otherwise leaves the session's (system) proxy. request is a HINTERNET. 1 = proxied.
int http_proxy_winhttp_apply(void *request, const char *host, int tls);
#endif
#endif
