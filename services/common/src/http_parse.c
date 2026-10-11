/*
 * HTTP/1.1 request parser (RFC 9112) used by http.c; exposed for tests.
 *
 * Strict where ambiguity enables request smuggling: Content-Length together
 * with Transfer-Encoding, conflicting Content-Length values, whitespace before
 * the header colon, obsolete line folding and non-chunked transfer codings are
 * rejected. Bare LF line endings are accepted (RFC 9112 2.2).
 */
#include "knsvc/http.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define DEF_MAX_HEADER 16384
#define DEF_MAX_BODY (1024 * 1024)
#define DEF_MAX_HEADERS 64

static int is_tchar(unsigned char c) {
    return isalnum(c) || strchr("!#$%&'*+-.^_`|~", c) != NULL;
}

/* Find the end of the header block; returns offset just past it, 0 if absent. */
static size_t find_head_end(const char *d, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (d[i] != '\n') continue;
        if (i + 1 < len && d[i + 1] == '\n') return i + 2;
        if (i + 2 < len && d[i + 1] == '\r' && d[i + 2] == '\n') return i + 3;
    }
    return 0;
}

/* Next line in [p, end): returns line length without CR/LF and sets *next. */
static size_t next_line(char *p, char *end, char **next) {
    char *nl = memchr(p, '\n', (size_t)(end - p));
    if (!nl) {
        *next = end;
        return (size_t)(end - p);
    }
    *next = nl + 1;
    size_t n = (size_t)(nl - p);
    if (n && p[n - 1] == '\r') n--;
    return n;
}

static kn_http_parse_status fail(kn_http_parsed *out, int status) {
    kn_http_parsed_free(out);
    out->error_status = status;
    return KN_HTTP_PARSE_ERROR;
}

static int parse_request_line(char *line, size_t n, kn_http_parsed *out) {
    line[n] = 0;
    char *sp1 = strchr(line, ' ');
    if (!sp1 || sp1 == line) return 400;
    *sp1 = 0;
    for (char *c = line; *c; c++)
        if (!is_tchar((unsigned char)*c)) return 400;
    char *target = sp1 + 1;
    char *sp2 = strchr(target, ' ');
    if (!sp2 || sp2 == target) return 400;
    *sp2 = 0;
    for (unsigned char *c = (unsigned char *)target; *c; c++)
        if (*c <= 0x20 || *c == 0x7f) return 400;
    const char *ver = sp2 + 1;
    if (strncmp(ver, "HTTP/", 5) != 0 || !isdigit((unsigned char)ver[5]) || ver[6] != '.' ||
        !isdigit((unsigned char)ver[7]) || ver[8])
        return 400;
    if (ver[5] != '1') return 505;
    out->method = line;
    out->target = target;
    out->version_minor = ver[7] - '0';
    return 0;
}

static int parse_header_line(char *line, size_t n, kn_http_header *h) {
    line[n] = 0;
    if (line[0] == ' ' || line[0] == '\t') return 400; /* obs-fold */
    char *colon = strchr(line, ':');
    if (!colon || colon == line) return 400;
    for (char *c = line; c < colon; c++)
        if (!is_tchar((unsigned char)*c)) return 400; /* includes whitespace before ':' */
    *colon = 0;
    char *v = colon + 1;
    while (*v == ' ' || *v == '\t') v++;
    char *e = v + strlen(v);
    while (e > v && (e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
    for (unsigned char *c = (unsigned char *)v; *c; c++)
        if ((*c < 0x20 && *c != '\t') || *c == 0x7f) return 400;
    h->name = line;
    h->value = v;
    return 0;
}

static const char *find_header(const kn_http_parsed *p, const char *name) {
    for (size_t i = 0; i < p->header_count; i++)
        if (strcasecmp(p->headers[i].name, name) == 0) return p->headers[i].value;
    return NULL;
}

/* Does the comma separated list contain token (case-insensitive)? */
static int list_has(const char *list, const char *token) {
    size_t tl = strlen(token);
    for (const char *p = list; p && *p;) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        const char *e = p + strcspn(p, ",");
        const char *t = e;
        while (t > p && (t[-1] == ' ' || t[-1] == '\t')) t--;
        if ((size_t)(t - p) == tl && strncasecmp(p, token, tl) == 0) return 1;
        p = e;
    }
    return 0;
}

/* Last transfer coding must be chunked and the only one we support. 0 ok, else status. */
static int check_te(const char *te) {
    const char *p = te;
    while (*p == ' ' || *p == '\t') p++;
    size_t n = strlen(p);
    while (n && (p[n - 1] == ' ' || p[n - 1] == '\t')) n--;
    if (n == 7 && strncasecmp(p, "chunked", 7) == 0) return 0;
    return 501;
}

/* Content-Length: digits only; repeated headers must agree. -1 invalid. */
static long long parse_content_length(const kn_http_parsed *p) {
    long long cl = -2;
    for (size_t i = 0; i < p->header_count; i++) {
        if (strcasecmp(p->headers[i].name, "Content-Length") != 0) continue;
        const char *v = p->headers[i].value;
        if (!*v || strlen(v) > 15) return -1;
        for (const char *c = v; *c; c++)
            if (!isdigit((unsigned char)*c)) return -1;
        long long x = strtoll(v, NULL, 10);
        if (cl != -2 && cl != x) return -1;
        cl = x;
    }
    return cl;
}

/* Decode a chunked body starting at d[0]. Returns 1 done, 0 incomplete, -status error. */
static int decode_chunked(const char *d, size_t len, size_t max_body, char **body, size_t *body_len, size_t *used) {
    size_t pos = 0, total = 0;
    char *out = NULL;
    for (;;) {
        const char *nl = memchr(d + pos, '\n', len - pos);
        if (!nl) {
            free(out);
            return len - pos > 1024 ? -400 : 0;
        }
        size_t i = pos;
        unsigned long long size = 0;
        int digits = 0;
        while (i < (size_t)(nl - d) && isxdigit((unsigned char)d[i])) {
            int c = tolower((unsigned char)d[i]);
            size = size * 16 + (unsigned)(isdigit(c) ? c - '0' : c - 'a' + 10);
            if (++digits > 15) break;
            i++;
        }
        if (!digits || digits > 15) {
            free(out);
            return -400;
        }
        /* chunk extensions after ';' are ignored; nothing else may follow */
        while (i < (size_t)(nl - d) && (d[i] == ' ' || d[i] == '\t')) i++;
        if (i < (size_t)(nl - d) && d[i] != ';' && !(d[i] == '\r' && d + i + 1 == nl)) {
            free(out);
            return -400;
        }
        pos = (size_t)(nl - d) + 1;
        if (size == 0) break;
        if (total + size > max_body) {
            free(out);
            return -413;
        }
        if (len - pos < size + 1) {
            free(out);
            return 0;
        }
        char *n = realloc(out, total + size + 1);
        if (!n) {
            free(out);
            return -500;
        }
        out = n;
        memcpy(out + total, d + pos, size);
        total += size;
        pos += size;
        if (d[pos] == '\r') {
            if (pos + 1 >= len) {
                free(out);
                return 0;
            }
            pos++;
        }
        if (d[pos] != '\n') {
            free(out);
            return -400;
        }
        pos++;
    }
    /* trailers until an empty line */
    for (;;) {
        const char *nl = memchr(d + pos, '\n', len - pos);
        if (!nl) {
            free(out);
            return len - pos > 8192 ? -431 : 0;
        }
        size_t ll = (size_t)(nl - d) - pos;
        pos = (size_t)(nl - d) + 1;
        if (ll == 0 || (ll == 1 && d[pos - 2] == '\r')) break;
    }
    if (!out) out = malloc(1);
    if (!out) return -500;
    out[total] = 0;
    *body = out;
    *body_len = total;
    *used = pos;
    return 1;
}

static int split_target(kn_http_parsed *out, char *arena_end_space) {
    char *t = out->target;
    const char *path_start = t;
    if (strcmp(t, "*") == 0) {
        if (strcasecmp(out->method, "OPTIONS") != 0) return 400;
    } else if (t[0] != '/') {
        /* absolute-form: scheme://authority/path */
        const char *s = strstr(t, "://");
        if (!s) return 400;
        path_start = strchr(s + 3, '/');
        if (!path_start) path_start = "/";
    }
    const char *q = strchr(path_start, '?');
    size_t plen = q ? (size_t)(q - path_start) : strlen(path_start);
    out->query = q ? (char *)q + 1 : t + strlen(t);
    char *path = arena_end_space;
    int n = kn_url_decode(path_start, plen, path, plen + 1, 0);
    if (n < 0 || memchr(path, 0, (size_t)n)) return 400;
    out->path = path;
    return 0;
}

kn_http_parse_status kn_http_parse_request(const char *data, size_t len, const kn_http_config *limits,
                                           kn_http_parsed *out, size_t *consumed) {
    memset(out, 0, sizeof *out);
    size_t max_header = limits && limits->max_header_bytes ? limits->max_header_bytes : DEF_MAX_HEADER;
    size_t max_body = limits && limits->max_body_bytes ? limits->max_body_bytes : DEF_MAX_BODY;
    size_t max_headers = limits && limits->max_headers > 0 ? (size_t)limits->max_headers : DEF_MAX_HEADERS;

    size_t skip = 0; /* leading empty lines */
    while (skip < len && (data[skip] == '\r' || data[skip] == '\n')) skip++;
    if (skip > 64) return fail(out, 400);
    size_t head_end = find_head_end(data + skip, len - skip);
    if (!head_end) {
        if (len - skip > max_header) return fail(out, 431);
        return KN_HTTP_PARSE_INCOMPLETE;
    }
    if (head_end > max_header) return fail(out, 431);

    /* arena: header copy + decoded path + header array */
    size_t arena_len = head_end + 1 + head_end + 1 + max_headers * sizeof(kn_http_header) + 16;
    char *arena = malloc(arena_len);
    if (!arena) return fail(out, 500);
    out->storage_ = arena;
    memcpy(arena, data + skip, head_end);
    arena[head_end] = 0;
    char *path_space = arena + head_end + 1;
    size_t hdr_off = (2 * head_end + 2 + 15) & ~(size_t)15;
    out->headers = (kn_http_header *)(void *)(arena + hdr_off);

    char *end = arena + head_end, *next;
    size_t n = next_line(arena, end, &next);
    int st = parse_request_line(arena, n, out);
    if (st) return fail(out, st);
    for (char *p = next; p < end;) {
        n = next_line(p, end, &next);
        if (n == 0) break;
        if (out->header_count == max_headers) return fail(out, 431);
        st = parse_header_line(p, n, &out->headers[out->header_count]);
        if (st) return fail(out, st);
        out->header_count++;
        p = next;
    }
    if ((st = split_target(out, path_space)) != 0) return fail(out, st);
    if (out->version_minor >= 1 && !find_header(out, "Host")) return fail(out, 400);

    const char *conn = find_header(out, "Connection");
    out->keep_alive = out->version_minor >= 1 ? !(conn && list_has(conn, "close")) : (conn && list_has(conn, "keep-alive"));

    const char *te = find_header(out, "Transfer-Encoding");
    long long cl = parse_content_length(out);
    size_t body_at = skip + head_end;
    if (te) {
        if (cl != -2) return fail(out, 400);
        if (out->version_minor == 0) return fail(out, 400);
        if ((st = check_te(te)) != 0) return fail(out, st);
        size_t used = 0;
        int r = decode_chunked(data + body_at, len - body_at, max_body, &out->body, &out->body_len, &used);
        if (r < 0) return fail(out, -r);
        if (r == 0) {
            kn_http_parsed_free(out);
            return KN_HTTP_PARSE_INCOMPLETE;
        }
        *consumed = body_at + used;
        return KN_HTTP_PARSE_DONE;
    }
    if (cl == -1) return fail(out, 400);
    size_t blen = cl > 0 ? (size_t)cl : 0;
    if (blen > max_body) return fail(out, 413);
    if (len - body_at < blen) {
        kn_http_parsed_free(out);
        return KN_HTTP_PARSE_INCOMPLETE;
    }
    out->body = malloc(blen + 1);
    if (!out->body) return fail(out, 500);
    memcpy(out->body, data + body_at, blen);
    out->body[blen] = 0;
    out->body_len = blen;
    *consumed = body_at + blen;
    return KN_HTTP_PARSE_DONE;
}

void kn_http_parsed_free(kn_http_parsed *p) {
    free(p->storage_);
    free(p->body);
    int keep = p->error_status;
    memset(p, 0, sizeof *p);
    p->error_status = keep;
}
