// The ranked backend rule (client/include/ranked_route.h): krunker.io first, then KrunkNative, else none.
#include <ranked_route.h>
#include <stdio.h>
#include <string.h>

RankedBackend ranked_backend_route(const int krunker_signed_in, const int kn_signed_in) {
    if (krunker_signed_in) return RANKED_BACKEND_KRUNKER;
    if (kn_signed_in) return RANKED_BACKEND_KN;
    return RANKED_BACKEND_NONE;
}

int ranked_match_host(const char *connection, char *host, const size_t host_n) {
    if (!host || !host_n) return 0;
    host[0] = 0;
    if (!connection) return 0;
    const char *colon = strchr(connection, ':');
    const size_t n = colon ? (size_t) (colon - connection) : strlen(connection);
    if (!n || n >= host_n) return 0;
    memcpy(host, connection, n);
    host[n] = 0;
    return 1;
}

// parseInt of the text after the ':' (v10:97583 uses 3000 when there is none)
static int match_port(const char *connection) {
    const char *colon = strchr(connection, ':');
    if (!colon || colon[1] < '0' || colon[1] > '9') return 3000;
    long port = 0;
    for (const char *c = colon + 1; *c >= '0' && *c <= '9' && port < 100000; c++) port = port * 10 + (*c - '0');
    return (int) port;
}

int ranked_match_url(const char *connection, const int production, const int secure, const char *token,
                     char *url, const size_t url_n, char *log, const size_t log_n) {
    char host[256];
    if (!url || !url_n || !log || !log_n || !ranked_match_host(connection, host, sizeof(host))) return 0;
    const char *access = token ? token : "";
    int written;
    if (production) written = snprintf(log, log_n, "wss://%s/ws?", host);
    else written = snprintf(log, log_n, "%s://%s:%d/ws?", secure ? "wss" : "ws", host, match_port(connection));
    if (written < 0 || (size_t) written >= log_n) return 0;
    written = snprintf(url, url_n, "%s%s%s", log, access[0] ? "at=" : "", access);
    return written >= 0 && (size_t) written < url_n;
}
