// Outgoing proxy selection (client/include/http_proxy.h): the client setting, else the standard
// environment variables. The tunnels themselves are http_conn.c's (POSIX) and WinHTTP's (Windows).
#include <http_proxy.h>
#if !defined(__EMSCRIPTEN__)
#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static struct {
    int set;              // the client setting names a proxy
    HttpProxyScope scope;
    HttpProxy proxy;
    char logged[200];     // the last configuration logged (one line per change)
} g_cfg;
static pthread_mutex_t g_cfg_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned g_generation;

static int hex(const int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// percent-decoded copy of s[0..len) (user info); 0 when it does not fit
static int decode(const char *s, const size_t len, char *out, const size_t n) {
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        int c = (unsigned char) s[i];
        if (c == '%' && i + 2 < len && hex(s[i + 1]) >= 0 && hex(s[i + 2]) >= 0) {
            c = hex(s[i + 1]) * 16 + hex(s[i + 2]);
            i += 2;
        }
        if (o + 1 >= n) return 0;
        out[o++] = (char) c;
    }
    out[o] = 0;
    return 1;
}

int http_proxy_parse(const char *url, HttpProxy *out) {
    memset(out, 0, sizeof(*out));
    if (!url) return 0;
    while (*url && isspace((unsigned char) *url)) url++;
    size_t len = strlen(url);
    while (len && isspace((unsigned char) url[len - 1])) len--;
    if (!len || len >= 512) return 0;
    char buf[512];
    memcpy(buf, url, len);
    buf[len] = 0;
    while (len && buf[len - 1] == '/') buf[--len] = 0; // "http://host:port/"
    static const struct {
        const char *scheme;
        HttpProxyKind kind;
        const char *port;
    } SCHEMES[] = {{"http://", HTTP_PROXY_HTTP, "80"}, {"https://", HTTP_PROXY_HTTPS, "443"},
                   {"socks5://", HTTP_PROXY_SOCKS5, "1080"}, {"socks5h://", HTTP_PROXY_SOCKS5H, "1080"}};
    const char *p = buf, *def_port = "80";
    out->kind = HTTP_PROXY_HTTP; // host:port alone: an HTTP proxy (curl's default too)
    if (strstr(buf, "://")) {
        out->kind = HTTP_PROXY_NONE;
        for (size_t i = 0; i < sizeof(SCHEMES) / sizeof(SCHEMES[0]); i++) {
            if (!strncasecmp(buf, SCHEMES[i].scheme, strlen(SCHEMES[i].scheme))) {
                out->kind = SCHEMES[i].kind;
                def_port = SCHEMES[i].port;
                p = buf + strlen(SCHEMES[i].scheme);
                break;
            }
        }
        if (out->kind == HTTP_PROXY_NONE) return 0; // socks4://, ftp://, ...
    }
    if (strchr(p, '/')) return 0; // no path
    const char *at = strrchr(p, '@');
    if (at) {
        const char *colon = memchr(p, ':', (size_t) (at - p));
        const char *user_end = colon ? colon : at;
        if (!decode(p, (size_t) (user_end - p), out->user, sizeof(out->user))) return 0;
        if (colon && !decode(colon + 1, (size_t) (at - colon - 1), out->pass, sizeof(out->pass))) return 0;
        p = at + 1;
    }
    const char *host = p, *host_end;
    if (*p == '[') { // [IPv6]
        host = p + 1;
        host_end = strchr(host, ']');
        if (!host_end) return 0;
        p = host_end + 1;
    } else {
        host_end = p;
        while (*host_end && *host_end != ':') host_end++;
        p = host_end;
    }
    const size_t host_len = (size_t) (host_end - host);
    if (!host_len || host_len >= sizeof(out->host)) return 0;
    for (const char *c = host; c < host_end; c++) {
        if (!(isalnum((unsigned char) *c) || *c == '.' || *c == '-' || *c == '_' || *c == ':')) return 0;
    }
    memcpy(out->host, host, host_len);
    out->host[host_len] = 0;
    if (*p == ':') {
        p++;
        size_t n = 0;
        while (*p >= '0' && *p <= '9' && n + 1 < sizeof(out->port)) out->port[n++] = *p++;
        out->port[n] = 0;
        if (!n || *p || atoi(out->port) <= 0 || atoi(out->port) > 65535) return 0;
    } else if (*p) {
        return 0;
    } else {
        snprintf(out->port, sizeof(out->port), "%s", def_port);
    }
    return 1;
}

void http_proxy_label(const HttpProxy *p, char *out, const size_t n) {
    static const char *const SCHEME[] = {"", "http", "https", "socks5", "socks5h"};
    if (!p || p->kind == HTTP_PROXY_NONE) {
        snprintf(out, n, "none");
        return;
    }
    const int v6 = strchr(p->host, ':') != NULL;
    snprintf(out, n, "%s://%s%s%s:%s", SCHEME[p->kind], v6 ? "[" : "", p->host, v6 ? "]" : "", p->port);
}

void http_proxy_basic_auth(const HttpProxy *p, char *out, const size_t n) {
    static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (!n) return;
    out[0] = 0;
    if (!p || !p->user[0]) return;
    char raw[260];
    const int len = snprintf(raw, sizeof(raw), "%s:%s", p->user, p->pass);
    if (len <= 0 || (size_t) len >= sizeof(raw) || (size_t) (len + 2) / 3 * 4 + 1 > n) return;
    size_t o = 0;
    for (int i = 0; i < len; i += 3) {
        const unsigned v = (unsigned) (unsigned char) raw[i] << 16 | (i + 1 < len ? (unsigned) (unsigned char) raw[i + 1] << 8 : 0) |
                           (i + 2 < len ? (unsigned) (unsigned char) raw[i + 2] : 0);
        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
        out[o++] = i + 1 < len ? B64[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < len ? B64[v & 63] : '=';
    }
    out[o] = 0;
}

static int ends_with_domain(const char *host, const char *domain) {
    const size_t h = strlen(host), d = strlen(domain);
    if (!d || h < d) return 0;
    if (strcasecmp(host + h - d, domain)) return 0;
    return h == d || host[h - d - 1] == '.';
}

// krunker.io and its subdomains, plus crucible.frvr.com: the FRVR auth service the krunker.io account
// refreshes its session at (krunker_account.h), so the krunker.io scope carries the whole account flow
int http_proxy_host_is_krunker(const char *host) {
    return host && (ends_with_domain(host, "krunker.io") || ends_with_domain(host, "crucible.frvr.com"));
}

static int is_loopback(const char *host) {
    return !strcasecmp(host, "localhost") || ends_with_domain(host, "localhost") || !strncmp(host, "127.", 4) ||
           !strcmp(host, "::1") || !strcmp(host, "[::1]") || !strcmp(host, "0.0.0.0");
}

static const char *env2(const char *lower, const char *upper) {
    const char *v = getenv(lower);
    if (!v || !*v) v = getenv(upper);
    return v && *v ? v : NULL;
}

// NO_PROXY: "*", or comma / space separated names; ".example.com", "*.example.com" and
// "example.com" all cover example.com and its subdomains; a ":port" suffix is ignored.
static int no_proxy_match(const char *host) {
    const char *list = env2("no_proxy", "NO_PROXY");
    if (!list) return 0;
    const char *p = list;
    while (*p) {
        while (*p == ',' || isspace((unsigned char) *p)) p++;
        const char *start = p;
        while (*p && *p != ',' && !isspace((unsigned char) *p)) p++;
        size_t len = (size_t) (p - start);
        if (!len) continue;
        char entry[256];
        if (len >= sizeof(entry)) continue;
        memcpy(entry, start, len);
        entry[len] = 0;
        if (!strcmp(entry, "*")) return 1;
        char *colon = strrchr(entry, ':');
        if (colon && !strchr(entry, ']') && colon == strchr(entry, ':')) *colon = 0; // host:port (not IPv6)
        const char *name = entry;
        if (!strncmp(name, "*.", 2)) name += 2;
        else if (name[0] == '.') name++;
        if (name[0] && ends_with_domain(host, name)) return 1;
    }
    return 0;
}

void http_proxy_configure(const char *url, const HttpProxyScope scope) {
    HttpProxy p;
    const int set = url && url[0] && http_proxy_parse(url, &p);
    char line[200], label[160];
    if (set) {
        http_proxy_label(&p, label, sizeof(label));
        snprintf(line, sizeof(line), "%s for %s", label, scope == HTTP_PROXY_SCOPE_ALL ? "all client traffic" : "krunker.io");
    } else {
        snprintf(line, sizeof(line), "%s", url && url[0] ? "not a proxy URL: none" : "none");
    }
    pthread_mutex_lock(&g_cfg_lock);
    g_cfg.set = set;
    g_cfg.scope = scope;
    if (set) g_cfg.proxy = p;
    else memset(&g_cfg.proxy, 0, sizeof(g_cfg.proxy));
    const int changed = strcmp(line, g_cfg.logged) != 0;
    if (changed) snprintf(g_cfg.logged, sizeof(g_cfg.logged), "%s", line);
    pthread_mutex_unlock(&g_cfg_lock);
    __atomic_add_fetch(&g_generation, 1, __ATOMIC_RELAXED);
    if (changed) {
        const char *env = env2("https_proxy", "HTTPS_PROXY");
        if (!env) env = env2("http_proxy", "HTTP_PROXY");
        if (!env) env = env2("all_proxy", "ALL_PROXY");
        printf("[http] proxy %s%s\n", line, !set && env ? " (the environment's proxy variables apply)" : "");
    }
}

unsigned http_proxy_generation(void) { return __atomic_load_n(&g_generation, __ATOMIC_RELAXED); }

int http_proxy_for(const char *host, const int tls, HttpProxy *out) {
    if (!host || !host[0] || is_loopback(host)) return 0;
    pthread_mutex_lock(&g_cfg_lock);
    const int set = g_cfg.set;
    const HttpProxyScope scope = g_cfg.scope;
    if (set) *out = g_cfg.proxy;
    pthread_mutex_unlock(&g_cfg_lock);
    if (set) return scope == HTTP_PROXY_SCOPE_ALL || http_proxy_host_is_krunker(host);
    // KN_FETCH_PROXY (the dev tools' convention: fetch_maps.py, fetch_live_assets.py): krunker.io
    // hosts only, e.g. user-assets.krunker.io for community maps' user assets from a network
    // Cloudflare refuses. Unset for players: their clients go direct, then to the brain's mirror.
    const char *fetch = getenv("KN_FETCH_PROXY");
    if (fetch && *fetch && http_proxy_host_is_krunker(host) && http_proxy_parse(fetch, out)) return 1;
    const char *env = tls ? env2("https_proxy", "HTTPS_PROXY") : env2("http_proxy", "HTTP_PROXY");
    if (!env) env = env2("all_proxy", "ALL_PROXY");
    if (!env || no_proxy_match(host)) return 0;
    if (!http_proxy_parse(env, out)) {
        static int warned;
        if (!__atomic_exchange_n(&warned, 1, __ATOMIC_RELAXED)) fprintf(stderr, "[http] ignoring the proxy environment variable %s: not a proxy URL\n", env);
        return 0;
    }
    return 1;
}

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>

int http_proxy_winhttp_apply(void *request, const char *host, const int tls) {
    HttpProxy p;
    if (!request || !http_proxy_for(host, tls, &p)) return 0;
    if (p.kind != HTTP_PROXY_HTTP) {
        static int warned;
        char label[160];
        http_proxy_label(&p, label, sizeof(label));
        if (!__atomic_exchange_n(&warned, 1, __ATOMIC_RELAXED))
            fprintf(stderr, "[http] WinHTTP takes http:// proxies only: %s skipped (the system proxy applies)\n", label);
        return 0;
    }
    char spec[160];
    const int v6 = strchr(p.host, ':') != NULL;
    snprintf(spec, sizeof(spec), "%s%s%s:%s", v6 ? "[" : "", p.host, v6 ? "]" : "", p.port);
    wchar_t wspec[160];
    if (!MultiByteToWideChar(CP_UTF8, 0, spec, -1, wspec, 160)) return 0;
    WINHTTP_PROXY_INFO info;
    memset(&info, 0, sizeof(info));
    info.dwAccessType = WINHTTP_ACCESS_TYPE_NAMED_PROXY;
    info.lpszProxy = wspec;
    info.lpszProxyBypass = WINHTTP_NO_PROXY_BYPASS;
    if (!WinHttpSetOption((HINTERNET) request, WINHTTP_OPTION_PROXY, &info, sizeof(info))) {
        fprintf(stderr, "[http] WinHTTP proxy %s: error %lu\n", spec, (unsigned long) GetLastError());
        return 0;
    }
    if (p.user[0]) { // Basic, sent up front
        wchar_t wuser[128], wpass[128];
        if (MultiByteToWideChar(CP_UTF8, 0, p.user, -1, wuser, 128) && MultiByteToWideChar(CP_UTF8, 0, p.pass, -1, wpass, 128))
            WinHttpSetCredentials((HINTERNET) request, WINHTTP_AUTH_TARGET_PROXY, WINHTTP_AUTH_SCHEME_BASIC, wuser, wpass, NULL);
    }
    return 1;
}
#endif
#endif
