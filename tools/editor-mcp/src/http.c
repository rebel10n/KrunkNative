// MCP Streamable HTTP transport (--http HOST:PORT): POST /mcp with one JSON-RPC message or a
// batch, answered as application/json (no SSE streams: every response is immediate);
// notifications get 202. GET /mcp is 405 (no server-initiated stream), DELETE ends the session.
// Binds 127.0.0.1 unless told otherwise and rejects non-local Origin headers (DNS rebinding).
#define _GNU_SOURCE
#include "jsonw.h"
#include "mcp.h"
#include "util.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

static char g_session[40];

static void respond(const int fd, const int code, const char *reason, const char *ctype, const char *body, const int with_session) {
    char head[512];
    const size_t blen = body ? strlen(body) : 0;
    const int n = snprintf(head, sizeof(head),
                           "HTTP/1.1 %d %s\r\nContent-Length: %zu\r\n%s%s%s%s%s%sCache-Control: no-store\r\nConnection: keep-alive\r\n\r\n",
                           code, reason, blen, ctype ? "Content-Type: " : "", ctype ? ctype : "", ctype ? "\r\n" : "",
                           with_session ? "Mcp-Session-Id: " : "", with_session ? g_session : "", with_session ? "\r\n" : "");
    if (write(fd, head, (size_t) n) < 0) return;
    for (size_t off = 0; off < blen;) {
        const ssize_t w = write(fd, body + off, blen - off);
        if (w <= 0) return;
        off += (size_t) w;
    }
}

static const char *header(const char *headers, const char *name, char *out, const size_t n) {
    const size_t len = strlen(name);
    for (const char *line = headers; line && *line;) {
        const char *eol = strstr(line, "\r\n");
        if (!eol) break;
        if ((size_t) (eol - line) > len && !strncasecmp(line, name, len) && line[len] == ':') {
            const char *v = line + len + 1;
            while (*v == ' ') v++;
            snprintf(out, n, "%.*s", (int) (eol - v), v);
            return out;
        }
        line = eol + 2;
    }
    return NULL;
}

static int origin_ok(const char *origin) {
    if (!origin) return 1;
    return !strncmp(origin, "http://127.0.0.1", 16) || !strncmp(origin, "http://localhost", 16) || !strncmp(origin, "http://[::1]", 12) ||
           !strcmp(origin, "null");
}

// one request from the connection buffer; returns 0 to keep the connection, -1 to close
static int serve_one(Ctx *ctx, const int fd, Sb *in) {
    char *end = in->buf ? strstr(in->buf, "\r\n\r\n") : NULL;
    if (!end) return 1; // need more
    const size_t head_len = (size_t) (end - in->buf) + 4;
    char lenbuf[32], origin[256], sid[64];
    const char *cl = header(in->buf, "Content-Length", lenbuf, sizeof(lenbuf));
    const size_t body_len = cl ? (size_t) strtoul(cl, NULL, 10) : 0;
    if (body_len > (64u << 20)) {
        respond(fd, 413, "Payload Too Large", NULL, NULL, 0);
        return -1;
    }
    if (in->len < head_len + body_len) return 1;
    char method[16] = "", path[256] = "";
    sscanf(in->buf, "%15s %255s", method, path);
    const char *org = header(in->buf, "Origin", origin, sizeof(origin));
    const char *got_sid = header(in->buf, "Mcp-Session-Id", sid, sizeof(sid));
    char *body = strndup(in->buf + head_len, body_len);
    // consume
    memmove(in->buf, in->buf + head_len + body_len, in->len - head_len - body_len + 1);
    in->len -= head_len + body_len;
    char *q = strchr(path, '?');
    if (q) *q = 0;
    int keep = 0;
    if (!origin_ok(org)) respond(fd, 403, "Forbidden", "text/plain", "origin not allowed", 0);
    else if (strcmp(path, "/mcp") && strcmp(path, "/")) respond(fd, 404, "Not Found", "text/plain", "MCP endpoint is /mcp", 0);
    else if (!strcmp(method, "GET")) respond(fd, 405, "Method Not Allowed", "text/plain", "no server-initiated stream; POST JSON-RPC to /mcp", 0);
    else if (!strcmp(method, "DELETE")) {
        g_session[0] = 0;
        respond(fd, 200, "OK", NULL, NULL, 0);
    } else if (!strcmp(method, "POST")) {
        cJSON *msg = cJSON_Parse(body ? body : "");
        if (!msg) respond(fd, 400, "Bad Request", "application/json", "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32700,\"message\":\"Parse error\"}}", 0);
        else {
            const int is_init = cJSON_IsObject(msg) && !strcmp(js_str(msg, "method", ""), "initialize");
            if (is_init) {
                unsigned char rnd[16];
                FILE *ur = fopen("/dev/urandom", "rb");
                if (!ur || fread(rnd, 1, sizeof(rnd), ur) != sizeof(rnd)) for (int i = 0; i < 16; i++) rnd[i] = (unsigned char) (rand() & 0xff);
                if (ur) fclose(ur);
                for (int i = 0; i < 16; i++) snprintf(g_session + i * 2, 3, "%02x", rnd[i]);
            }
            if (!is_init && got_sid && g_session[0] && strcmp(got_sid, g_session)) {
                respond(fd, 404, "Not Found", "text/plain", "unknown session", 0);
            } else {
                cJSON *res = mcp_handle(ctx, msg);
                if (!res) respond(fd, 202, "Accepted", NULL, NULL, 0);
                else {
                    char *text = jsonw_print(res, JSONW_COMPACT);
                    respond(fd, 200, "OK", "application/json", text, is_init);
                    free(text);
                    cJSON_Delete(res);
                }
            }
            cJSON_Delete(msg);
        }
        keep = 1;
    } else respond(fd, 405, "Method Not Allowed", NULL, NULL, 0);
    free(body);
    return keep ? 0 : 0;
}

int mcp_run_http(Ctx *ctx, const char *listen_addr, const char *announce_path) {
    char host[128] = "127.0.0.1";
    int port = 0;
    const char *colon = strrchr(listen_addr, ':');
    if (colon) {
        snprintf(host, sizeof(host), "%.*s", (int) (colon - listen_addr), listen_addr);
        port = atoi(colon + 1);
    } else port = atoi(listen_addr);
    if (!host[0] || !strcmp(host, "localhost")) snprintf(host, sizeof(host), "127.0.0.1");
    signal(SIGPIPE, SIG_IGN);
    const int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) return 2;
    const int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons((uint16_t) port)};
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1 || bind(lfd, (struct sockaddr *) &addr, sizeof(addr)) != 0 || listen(lfd, 8) != 0) {
        fprintf(stderr, "kn_editor_mcp: cannot listen on %s:%d (%s)\n", host, port, strerror(errno));
        close(lfd);
        return 2;
    }
    socklen_t alen = sizeof(addr);
    getsockname(lfd, (struct sockaddr *) &addr, &alen);
    char url[256];
    snprintf(url, sizeof(url), "http://%s:%d/mcp", host, ntohs(addr.sin_port));
    fprintf(stderr, "kn_editor_mcp: MCP Streamable HTTP on %s\n", url);
    if (announce_path) {
        char line[300];
        snprintf(line, sizeof(line), "%s\n", url);
        file_write(announce_path, line, strlen(line));
    }
    for (;;) {
        const int fd = accept(lfd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR) continue;
            break;
        }
        Sb in = {0};
        for (;;) {
            struct pollfd p = {.fd = fd, .events = POLLIN};
            if (poll(&p, 1, 30000) <= 0) break;
            char buf[65536];
            const ssize_t n = read(fd, buf, sizeof(buf));
            if (n <= 0) break;
            sb_putn(&in, buf, (size_t) n);
            int rc;
            while ((rc = serve_one(ctx, fd, &in)) == 0 && in.len) {}
            if (rc < 0) break;
        }
        sb_free(&in);
        close(fd);
    }
    close(lfd);
    return 0;
}
