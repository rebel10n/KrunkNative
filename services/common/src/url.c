/* URL helpers from knsvc/http.h. */
#include "knsvc/http.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int hexv(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = tolower(c);
    return (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
}

int kn_url_decode(const char *in, size_t len, char *out, size_t cap, int form) {
    size_t o = 0;
    if (!cap) return -1;
    for (size_t i = 0; i < len; i++) {
        char c = in[i];
        if (c == '%') {
            int hi = i + 1 < len ? hexv((unsigned char)in[i + 1]) : -1;
            int lo = i + 2 < len ? hexv((unsigned char)in[i + 2]) : -1;
            if (hi < 0 || lo < 0) return -1;
            c = (char)(hi << 4 | lo);
            i += 2;
        } else if (c == '+' && form) {
            c = ' ';
        }
        if (o + 1 >= cap) return -1;
        out[o++] = c;
    }
    out[o] = 0;
    return (int)o;
}

int kn_url_encode(const char *in, char *out, size_t cap) {
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
        if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') {
            if (o + 1 >= cap) return -1;
            out[o++] = (char)*p;
        } else {
            if (o + 3 >= cap) return -1;
            out[o++] = '%';
            out[o++] = hex[*p >> 4];
            out[o++] = hex[*p & 15];
        }
    }
    if (o >= cap) return -1;
    out[o] = 0;
    return (int)o;
}

static int default_port(const char *scheme) {
    if (!strcmp(scheme, "http") || !strcmp(scheme, "ws")) return 80;
    if (!strcmp(scheme, "https") || !strcmp(scheme, "wss")) return 443;
    return -1;
}

int kn_url_parse(const char *url, kn_url *out) {
    memset(out, 0, sizeof *out);
    const char *sep = strstr(url, "://");
    if (!sep || sep == url || (size_t)(sep - url) >= sizeof out->scheme) return -1;
    for (size_t i = 0; i < (size_t)(sep - url); i++) out->scheme[i] = (char)tolower((unsigned char)url[i]);
    const char *h = sep + 3;
    const char *path = h + strcspn(h, "/?#");
    const char *at = memchr(h, '@', (size_t)(path - h));
    if (at) h = at + 1; /* userinfo ignored */
    const char *host_end;
    const char *port_s = NULL;
    if (*h == '[') {
        const char *rb = memchr(h, ']', (size_t)(path - h));
        if (!rb) return -1;
        if ((size_t)(rb - h - 1) >= sizeof out->host) return -1;
        memcpy(out->host, h + 1, (size_t)(rb - h - 1));
        host_end = rb + 1;
        if (*host_end == ':') port_s = host_end + 1;
        else if (host_end != path) return -1;
    } else {
        const char *colon = memchr(h, ':', (size_t)(path - h));
        host_end = colon ? colon : path;
        if (host_end == h || (size_t)(host_end - h) >= sizeof out->host) return -1;
        memcpy(out->host, h, (size_t)(host_end - h));
        if (colon) port_s = colon + 1;
    }
    if (!out->host[0]) return -1;
    if (port_s) {
        char *end;
        long p = strtol(port_s, &end, 10);
        if (end == port_s || end != path || p <= 0 || p > 65535) return -1;
        out->port = (int)p;
    } else {
        out->port = default_port(out->scheme);
        if (out->port < 0) return -1;
    }
    const char *frag = strchr(path, '#');
    size_t plen = frag ? (size_t)(frag - path) : strlen(path);
    if (plen + 2 > sizeof out->path) return -1;
    if (plen == 0 || path[0] != '/') {
        out->path[0] = '/';
        memcpy(out->path + 1, path, plen);
    } else {
        memcpy(out->path, path, plen);
    }
    return 0;
}
