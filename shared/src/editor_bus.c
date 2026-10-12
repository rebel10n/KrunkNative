// Editor bus transport and wire format (shared/include/editor_bus.h). Pure C11 + cJSON +
// sockets; no GL. Native only: the web build compiles the stubs at the end (tabs talk
// over BroadcastChannel there, client/src/html/web_dom.c).
#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE // struct ucred (SO_PEERCRED)
#endif
#include <editor_bus.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// ---------------------------------------------------------------------------
// pure helpers (every platform)

size_t edbus_frame_encode(const char *json, const size_t len, unsigned char *out) {
    out[0] = (unsigned char) (len >> 24);
    out[1] = (unsigned char) (len >> 16);
    out[2] = (unsigned char) (len >> 8);
    out[3] = (unsigned char) len;
    memcpy(out + 4, json, len);
    return len + 4;
}

int edbus_frame_decode(const unsigned char *buf, const size_t len, size_t *payload_len, size_t *consumed) {
    if (len < 4) return 0;
    const uint32_t n = (uint32_t) buf[0] << 24 | (uint32_t) buf[1] << 16 | (uint32_t) buf[2] << 8 | (uint32_t) buf[3];
    if (n > EDBUS_MAX_FRAME) return -1;
    if (len < 4 + (size_t) n) return 0;
    *payload_len = n;
    *consumed = 4 + (size_t) n;
    return 1;
}

int edbus_token_equal(const char *a, const char *b) {
    if (!a || !b) return 0;
    const size_t la = strlen(a), lb = strlen(b);
    unsigned char diff = (unsigned char) (la != lb);
    const size_t n = la < lb ? la : lb;
    for (size_t i = 0; i < n; i++) diff |= (unsigned char) (a[i] ^ b[i]);
    return diff == 0 && la == EDBUS_TOKEN_LEN;
}

cJSON *edbus_msg(const char *type) {
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "t", type);
    return m;
}

cJSON *edbus_req(const int id, const char *op, cJSON *args) {
    cJSON *m = edbus_msg("req");
    cJSON_AddNumberToObject(m, "id", id);
    cJSON_AddStringToObject(m, "op", op);
    cJSON_AddItemToObject(m, "args", args ? args : cJSON_CreateObject());
    return m;
}

cJSON *edbus_res_ok(const cJSON *id, cJSON *result) {
    cJSON *m = edbus_msg("res");
    cJSON_AddItemToObject(m, "id", id ? cJSON_Duplicate(id, 1) : cJSON_CreateNull());
    cJSON_AddBoolToObject(m, "ok", 1);
    cJSON_AddItemToObject(m, "result", result ? result : cJSON_CreateObject());
    return m;
}

cJSON *edbus_res_err(const cJSON *id, const char *code, const char *message) {
    cJSON *m = edbus_msg("res");
    cJSON_AddItemToObject(m, "id", id ? cJSON_Duplicate(id, 1) : cJSON_CreateNull());
    cJSON_AddBoolToObject(m, "ok", 0);
    cJSON *e = cJSON_AddObjectToObject(m, "error");
    cJSON_AddStringToObject(e, "code", code ? code : "internal");
    cJSON_AddStringToObject(e, "message", message ? message : "");
    return m;
}

static unsigned scope_word(const char *w, const size_t n) {
    if (n == 4 && !strncmp(w, "read", 4)) return EDBUS_SCOPE_READ;
    if (n == 4 && !strncmp(w, "edit", 4)) return EDBUS_SCOPE_EDIT;
    if (n == 7 && !strncmp(w, "project", 7)) return EDBUS_SCOPE_PROJECT;
    if ((n == 3 && !strncmp(w, "all", 3)) || (n == 1 && w[0] == '1') || (n == 4 && !strncmp(w, "true", 4)) ||
        (n == 3 && !strncmp(w, "yes", 3)))
        return EDBUS_SCOPE_ALL;
    return 0;
}

unsigned edbus_scope_parse(const char *list) {
    if (!list) return 0;
    unsigned mask = 0;
    while (*list) {
        while (*list == ',' || *list == ' ' || *list == '+') list++;
        const char *s = list;
        while (*list && *list != ',' && *list != ' ' && *list != '+') list++;
        if (list > s) mask |= scope_word(s, (size_t) (list - s));
    }
    return mask;
}

unsigned edbus_scope_from_json(const cJSON *arr) {
    unsigned mask = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        if (cJSON_IsString(it)) mask |= scope_word(it->valuestring, strlen(it->valuestring));
    }
    return mask;
}

cJSON *edbus_scope_json(const unsigned mask) {
    cJSON *a = cJSON_CreateArray();
    if (mask & EDBUS_SCOPE_READ) cJSON_AddItemToArray(a, cJSON_CreateString("read"));
    if (mask & EDBUS_SCOPE_EDIT) cJSON_AddItemToArray(a, cJSON_CreateString("edit"));
    if (mask & EDBUS_SCOPE_PROJECT) cJSON_AddItemToArray(a, cJSON_CreateString("project"));
    return a;
}

static int in_list(const char *s, const char *const *list) {
    for (; *list; list++) if (!strcmp(s, *list)) return 1;
    return 0;
}

unsigned edbus_scope_for(const char *op, const char *tool) {
    static const char *const read_ops[] = {"list", "subscribe", "copy", "ping", NULL};
    static const char *const project_ops[] = {"open", "close", "focus", "new", NULL};
    static const char *const edit_ops[] = {"paste", NULL};
    static const char *const read_tools[] = {"info", "query", "get", "render", "export", "history", "log", "copy_objects", NULL};
    if (!op) return EDBUS_SCOPE_ALL;
    if (!strcmp(op, "call")) {
        if (!tool) return EDBUS_SCOPE_ALL;
        return in_list(tool, read_tools) ? EDBUS_SCOPE_READ : EDBUS_SCOPE_EDIT; // unknown tools need edit
    }
    if (in_list(op, read_ops)) return EDBUS_SCOPE_READ;
    if (in_list(op, project_ops)) return EDBUS_SCOPE_PROJECT;
    if (in_list(op, edit_ops)) return EDBUS_SCOPE_EDIT;
    return EDBUS_SCOPE_ALL; // unknown: every scope (the handler rejects it anyway)
}

char *edbus_base64(const unsigned char *data, const size_t len) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const size_t out_len = (len + 2) / 3 * 4;
    char *out = malloc(out_len + 1);
    if (!out) return NULL;
    size_t j = 0;
    for (size_t i = 0; i < len; i += 3) {
        const uint32_t v = (uint32_t) data[i] << 16 | (i + 1 < len ? (uint32_t) data[i + 1] << 8 : 0) | (i + 2 < len ? data[i + 2] : 0);
        out[j++] = tbl[(v >> 18) & 63];
        out[j++] = tbl[(v >> 12) & 63];
        out[j++] = i + 1 < len ? tbl[(v >> 6) & 63] : '=';
        out[j++] = i + 2 < len ? tbl[v & 63] : '=';
    }
    out[j] = 0;
    return out;
}

#ifndef __EMSCRIPTEN__

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <io.h>
#include <process.h>
#include <direct.h>
typedef int socklen_t;
#define EDBUS_CLOSE closesocket
#define EDBUS_EAGAIN(e) ((e) == WSAEWOULDBLOCK)
static int sock_errno(void) { return WSAGetLastError(); }
#else
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#define EDBUS_CLOSE close
#define EDBUS_EAGAIN(e) ((e) == EAGAIN || (e) == EWOULDBLOCK || (e) == EINTR)
static int sock_errno(void) { return errno; }
#endif

static double now_ms(void) {
#ifdef _WIN32
    return (double) GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
#endif
}

static void net_startup(void) {
#ifdef _WIN32
    static int done;
    if (!done) {
        WSADATA w;
        WSAStartup(MAKEWORD(2, 2), &w);
        done = 1;
    }
#endif
}

static void set_nonblock(const int fd) {
#ifdef _WIN32
    u_long on = 1;
    ioctlsocket((SOCKET) fd, FIONBIO, &on);
#else
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC);
#endif
}

static int make_dir_private(const char *dir) {
#ifdef _WIN32
    _mkdir(dir);
    return 1;
#else
    struct stat st;
    if (mkdir(dir, 0700) != 0 && errno != EEXIST) return 0;
    if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) return 0;
    if (st.st_uid != getuid()) {
        fprintf(stderr, "[bus] %s belongs to another user\n", dir);
        return 0;
    }
    if ((st.st_mode & 077) != 0) chmod(dir, 0700);
    return 1;
#endif
}

int edbus_paths_init(EdBusPaths *p, const char *dir_override) {
    memset(p, 0, sizeof(*p));
    const char *env = getenv("KRUNKNATIVE_EDITOR_BUS_DIR");
    if (dir_override && *dir_override) snprintf(p->dir, sizeof(p->dir), "%s", dir_override);
    else if (env && *env) snprintf(p->dir, sizeof(p->dir), "%s", env);
    else {
#ifdef _WIN32
        const char *tmp = getenv("LOCALAPPDATA");
        snprintf(p->dir, sizeof(p->dir), "%s\\krunknative", tmp && *tmp ? tmp : ".");
#else
        const char *run = getenv("XDG_RUNTIME_DIR");
        if (run && *run) snprintf(p->dir, sizeof(p->dir), "%s/krunknative", run);
        else snprintf(p->dir, sizeof(p->dir), "/tmp/krunknative-%u", (unsigned) getuid());
#endif
    }
    const size_t dl = strlen(p->dir);
    if (dl > 1 && (p->dir[dl - 1] == '/' || p->dir[dl - 1] == '\\')) p->dir[dl - 1] = 0;
    if (!make_dir_private(p->dir)) return 0;
    snprintf(p->sock, sizeof(p->sock), "%s/editor-bus.sock", p->dir);
    snprintf(p->token, sizeof(p->token), "%s/editor-bus.token", p->dir);
    snprintf(p->lock, sizeof(p->lock), "%s/editor-bus.lock", p->dir);
    snprintf(p->port, sizeof(p->port), "%s/editor-bus.port", p->dir);
#ifdef _WIN32
    p->tcp = 1;
#else
    const char *tcp = getenv("KRUNKNATIVE_EDITOR_BUS_TCP");
    p->tcp = tcp && *tcp && strcmp(tcp, "0") != 0;
    struct sockaddr_un sa;
    if (strlen(p->sock) >= sizeof(sa.sun_path)) {
        fprintf(stderr, "[bus] socket path too long (%s): using the TCP fallback\n", p->sock);
        p->tcp = 1;
    }
#endif
    return 1;
}

int edbus_token_generate(char *out) {
    unsigned char raw[EDBUS_TOKEN_LEN / 2];
    int ok = 0;
#ifndef _WIN32
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) {
        ok = fread(raw, 1, sizeof(raw), f) == sizeof(raw);
        fclose(f);
    }
#endif
    if (!ok) {
        // fallback: time/pid/address mixed through an LCG (Windows without /dev/urandom)
        uint64_t s = (uint64_t) time(NULL) ^ ((uint64_t) (uintptr_t) out << 16) ^ (uint64_t) clock();
#ifdef _WIN32
        s ^= (uint64_t) _getpid() << 32;
#else
        s ^= (uint64_t) getpid() << 32;
#endif
        for (size_t i = 0; i < sizeof(raw); i++) {
            s = s * 6364136223846793005ull + 1442695040888963407ull;
            raw[i] = (unsigned char) (s >> 33);
        }
    }
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(raw); i++) {
        out[i * 2] = hex[raw[i] >> 4];
        out[i * 2 + 1] = hex[raw[i] & 15];
    }
    out[EDBUS_TOKEN_LEN] = 0;
    return 1;
}

static int write_private_file(const char *path, const char *text) {
#ifdef _WIN32
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    fputs(text, f);
    fclose(f);
    return 1;
#else
    char tmp[700];
    snprintf(tmp, sizeof(tmp), "%s.%d.tmp", path, (int) getpid());
    const int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return 0;
    fchmod(fd, 0600);
    const size_t len = strlen(text);
    const ssize_t w = write(fd, text, len);
    close(fd);
    if (w != (ssize_t) len || rename(tmp, path) != 0) {
        unlink(tmp);
        return 0;
    }
    return 1;
#endif
}

int edbus_token_write(const EdBusPaths *p, const char *token) {
    char line[EDBUS_TOKEN_LEN + 2];
    snprintf(line, sizeof(line), "%s\n", token);
    return write_private_file(p->token, line);
}

int edbus_token_read(const EdBusPaths *p, char *out) {
    FILE *f = fopen(p->token, "rb");
    if (!f) return 0;
    char buf[EDBUS_TOKEN_LEN + 8] = "";
    const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    size_t len = 0;
    while (len < n && ((buf[len] >= '0' && buf[len] <= '9') || (buf[len] >= 'a' && buf[len] <= 'f'))) len++;
    if (len != EDBUS_TOKEN_LEN) return 0;
    memcpy(out, buf, EDBUS_TOKEN_LEN);
    out[EDBUS_TOKEN_LEN] = 0;
    return 1;
}

// ---------------------------------------------------------------------------
// connections

void edbus_conn_init(EdBusConn *c, const int fd) {
    memset(c, 0, sizeof(*c));
    c->fd = fd;
    c->closed = fd < 0;
    if (fd >= 0) set_nonblock(fd);
}

void edbus_close_fd(const int fd) {
    if (fd >= 0) EDBUS_CLOSE(fd);
}

void edbus_conn_close(EdBusConn *c) {
    if (c->fd >= 0) EDBUS_CLOSE(c->fd);
    free(c->in);
    free(c->out);
    memset(c, 0, sizeof(*c));
    c->fd = -1;
    c->closed = 1;
}

int edbus_flush(EdBusConn *c) {
    if (c->fd < 0 || c->closed) return -1;
    while (c->out_off < c->out_len) {
#ifdef _WIN32
        const int w = send((SOCKET) c->fd, (const char *) c->out + c->out_off, (int) (c->out_len - c->out_off), 0);
#else
        const ssize_t w = send(c->fd, c->out + c->out_off, c->out_len - c->out_off, MSG_NOSIGNAL);
#endif
        if (w < 0) {
            if (EDBUS_EAGAIN(sock_errno())) return 0;
            c->closed = 1;
            return -1;
        }
        c->out_off += (size_t) w;
    }
    c->out_off = c->out_len = 0;
    return 1;
}

int edbus_send_text(EdBusConn *c, const char *json, const size_t len) {
    if (!c || c->fd < 0 || c->closed || len > EDBUS_MAX_FRAME) return 0;
    // compact the sent prefix, grow, append
    if (c->out_off) {
        memmove(c->out, c->out + c->out_off, c->out_len - c->out_off);
        c->out_len -= c->out_off;
        c->out_off = 0;
    }
    if (c->out_len + len + 4 > c->out_cap) {
        size_t cap = c->out_cap ? c->out_cap : 4096;
        while (cap < c->out_len + len + 4) cap *= 2;
        unsigned char *n = realloc(c->out, cap);
        if (!n) return 0;
        c->out = n;
        c->out_cap = cap;
    }
    c->out_len += edbus_frame_encode(json, len, c->out + c->out_len);
    return edbus_flush(c) >= 0;
}

int edbus_send(EdBusConn *c, const cJSON *msg) {
    char *text = cJSON_PrintUnformatted(msg);
    if (!text) return 0;
    const int ok = edbus_send_text(c, text, strlen(text));
    cJSON_free(text);
    return ok;
}

int edbus_wait_readable(const int fd, const int timeout_ms) {
    if (fd < 0) return -1;
#ifdef _WIN32
    fd_set set;
    FD_ZERO(&set);
    FD_SET((SOCKET) fd, &set);
    struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    const int r = select(0, &set, NULL, NULL, timeout_ms < 0 ? NULL : &tv);
#else
    struct pollfd pfd = {fd, POLLIN, 0};
    int r;
    do r = poll(&pfd, 1, timeout_ms);
    while (r < 0 && errno == EINTR);
#endif
    return r < 0 ? -1 : r > 0;
}

static int wait_writable(const int fd, const int timeout_ms) {
#ifdef _WIN32
    fd_set set;
    FD_ZERO(&set);
    FD_SET((SOCKET) fd, &set);
    struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    return select(0, NULL, &set, NULL, timeout_ms < 0 ? NULL : &tv) > 0;
#else
    struct pollfd pfd = {fd, POLLOUT, 0};
    int r;
    do r = poll(&pfd, 1, timeout_ms);
    while (r < 0 && errno == EINTR);
    return r > 0;
#endif
}

int edbus_flush_blocking(EdBusConn *c, const int timeout_ms) {
    const double start = now_ms();
    for (;;) {
        const int r = edbus_flush(c);
        if (r != 0) return r > 0;
        if (timeout_ms >= 0 && now_ms() - start > timeout_ms) return 0;
        wait_writable(c->fd, 50);
    }
}

int edbus_read(EdBusConn *c) {
    if (c->fd < 0 || c->closed) return -1;
    for (;;) {
        if (c->in_cap - c->in_len < 65536) {
            const size_t cap = c->in_cap ? c->in_cap * 2 : 65536 * 2;
            if (cap > (size_t) EDBUS_MAX_FRAME * 2 + 65536) {
                c->closed = 1;
                return -1;
            }
            unsigned char *n = realloc(c->in, cap);
            if (!n) return -1;
            c->in = n;
            c->in_cap = cap;
        }
#ifdef _WIN32
        const int r = recv((SOCKET) c->fd, (char *) c->in + c->in_len, (int) (c->in_cap - c->in_len), 0);
#else
        const ssize_t r = recv(c->fd, c->in + c->in_len, c->in_cap - c->in_len, 0);
#endif
        if (r == 0) {
            c->closed = 1;
            return -1;
        }
        if (r < 0) {
            if (EDBUS_EAGAIN(sock_errno())) return 0;
            c->closed = 1;
            return -1;
        }
        c->in_len += (size_t) r;
    }
}

cJSON *edbus_next(EdBusConn *c, int *bad) {
    if (bad) *bad = 0;
    size_t payload = 0, consumed = 0;
    const int r = edbus_frame_decode(c->in, c->in_len, &payload, &consumed);
    if (r == 0) return NULL;
    if (r < 0) {
        if (bad) *bad = 1;
        return NULL;
    }
    cJSON *msg = cJSON_ParseWithLength((const char *) c->in + 4, payload);
    memmove(c->in, c->in + consumed, c->in_len - consumed);
    c->in_len -= consumed;
    if (!cJSON_IsObject(msg)) {
        cJSON_Delete(msg);
        if (bad) *bad = 1;
        return NULL;
    }
    return msg;
}

cJSON *edbus_recv(EdBusConn *c, const int timeout_ms) {
    const double start = now_ms();
    for (;;) {
        int bad = 0;
        cJSON *m = edbus_next(c, &bad);
        if (m) return m;
        if (bad) {
            c->closed = 1;
            return NULL;
        }
        if (c->closed) return NULL;
        if (c->out_off < c->out_len) edbus_flush(c);
        int wait = 100;
        if (timeout_ms >= 0) {
            const double left = timeout_ms - (now_ms() - start);
            if (left <= 0) return NULL;
            if (left < wait) wait = (int) left;
        }
        const int r = edbus_wait_readable(c->fd, wait);
        if (r < 0) {
            c->closed = 1;
            return NULL;
        }
        if (r > 0 && edbus_read(c) < 0) {
            // EOF: frames already buffered are still delivered
            m = edbus_next(c, &bad);
            return m;
        }
    }
}

// ---------------------------------------------------------------------------
// sockets

int edbus_listen(const EdBusPaths *p) {
    net_startup();
    if (p->tcp) {
        const int fd = (int) socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return -1;
        struct sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        sa.sin_port = 0;
        socklen_t sl = sizeof(sa);
        if (bind(fd, (struct sockaddr *) &sa, sizeof(sa)) != 0 || listen(fd, 16) != 0 || getsockname(fd, (struct sockaddr *) &sa, &sl) != 0) {
            EDBUS_CLOSE(fd);
            return -1;
        }
        char port[16];
        snprintf(port, sizeof(port), "%u\n", (unsigned) ntohs(sa.sin_port));
        if (!write_private_file(p->port, port)) {
            EDBUS_CLOSE(fd);
            return -1;
        }
        set_nonblock(fd);
        return fd;
    }
#ifdef _WIN32
    return -1;
#else
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", p->sock);
    unlink(p->sock); // stale socket of a dead host (the caller holds the lock)
    const mode_t old = umask(0177);
    const int rc = bind(fd, (struct sockaddr *) &sa, sizeof(sa));
    umask(old);
    if (rc != 0 || listen(fd, 16) != 0) {
        close(fd);
        return -1;
    }
    chmod(p->sock, 0600);
    set_nonblock(fd);
    return fd;
#endif
}

int edbus_accept(const int listen_fd) {
    if (listen_fd < 0) return -1;
    const int fd = (int) accept(listen_fd, NULL, NULL);
    if (fd < 0) return -1;
#ifndef _WIN32
    // AF_UNIX: only our own uid may talk to the bus (the 0600 socket already enforces it)
    struct sockaddr_storage ss;
    socklen_t sl = sizeof(ss);
    if (getsockname(fd, (struct sockaddr *) &ss, &sl) == 0 && ss.ss_family == AF_UNIX) {
#ifdef SO_PEERCRED
        struct ucred cred;
        socklen_t cl = sizeof(cred);
        if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &cl) == 0 && cred.uid != getuid()) {
            close(fd);
            return -1;
        }
#endif
    }
#endif
    set_nonblock(fd);
    return fd;
}

int edbus_connect(const EdBusPaths *p) {
    net_startup();
    if (p->tcp) {
        FILE *f = fopen(p->port, "rb");
        if (!f) return -1;
        unsigned port = 0;
        const int ok = fscanf(f, "%u", &port) == 1;
        fclose(f);
        if (!ok || !port || port > 65535) return -1;
        const int fd = (int) socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return -1;
        struct sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        sa.sin_port = htons((unsigned short) port);
        if (connect(fd, (struct sockaddr *) &sa, sizeof(sa)) != 0) {
            EDBUS_CLOSE(fd);
            return -1;
        }
        return fd;
    }
#ifdef _WIN32
    return -1;
#else
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", p->sock);
    if (connect(fd, (struct sockaddr *) &sa, sizeof(sa)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
#endif
}

int edbus_lock_try(const EdBusPaths *p) {
#ifdef _WIN32
    HANDLE h = CreateFileA(p->lock, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    return _open_osfhandle((intptr_t) h, 0);
#else
    const int fd = open(p->lock, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        close(fd);
        return -1;
    }
    return fd;
#endif
}

void edbus_lock_release(const int fd) {
    if (fd < 0) return;
#ifdef _WIN32
    _close(fd);
#else
    flock(fd, LOCK_UN);
    close(fd);
#endif
}

void edbus_unlink(const EdBusPaths *p) {
    remove(p->sock);
    remove(p->port);
    remove(p->token);
}

int edbus_client_connect(EdBusConn *c, const EdBusPaths *p, const char *role, const char *client_id, const char *name,
                         const char *version, const int timeout_ms, cJSON **welcome) {
    if (welcome) *welcome = NULL;
    char token[EDBUS_TOKEN_LEN + 1];
    if (!edbus_token_read(p, token)) return 0;
    const int fd = edbus_connect(p);
    if (fd < 0) return 0;
    edbus_conn_init(c, fd);
    cJSON *hello = edbus_msg("hello");
    cJSON_AddStringToObject(hello, "token", token);
    cJSON_AddStringToObject(hello, "role", role ? role : "tool");
    cJSON_AddStringToObject(hello, "client_id", client_id ? client_id : "tool");
    cJSON_AddStringToObject(hello, "name", name ? name : client_id ? client_id : "tool");
    cJSON_AddStringToObject(hello, "version", version ? version : "");
#ifdef _WIN32
    cJSON_AddNumberToObject(hello, "pid", _getpid());
#else
    cJSON_AddNumberToObject(hello, "pid", getpid());
#endif
    const int sent = edbus_send(c, hello) && edbus_flush_blocking(c, timeout_ms);
    cJSON_Delete(hello);
    if (!sent) {
        edbus_conn_close(c);
        return 0;
    }
    cJSON *reply = edbus_recv(c, timeout_ms);
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(reply, "t");
    if (!cJSON_IsString(t) || strcmp(t->valuestring, "welcome") != 0) {
        if (reply) {
            char *text = cJSON_PrintUnformatted(reply);
            fprintf(stderr, "[bus] handshake refused: %s\n", text ? text : "?");
            cJSON_free(text);
        }
        cJSON_Delete(reply);
        edbus_conn_close(c);
        return 0;
    }
    if (welcome) *welcome = reply;
    else cJSON_Delete(reply);
    return 1;
}

#else // __EMSCRIPTEN__: no sockets in the browser

int edbus_paths_init(EdBusPaths *p, const char *dir_override) { (void) dir_override; memset(p, 0, sizeof(*p)); return 0; }
int edbus_token_generate(char *out) { memset(out, '0', EDBUS_TOKEN_LEN); out[EDBUS_TOKEN_LEN] = 0; return 0; }
int edbus_token_write(const EdBusPaths *p, const char *token) { (void) p; (void) token; return 0; }
int edbus_token_read(const EdBusPaths *p, char *out) { (void) p; (void) out; return 0; }
void edbus_conn_init(EdBusConn *c, const int fd) { memset(c, 0, sizeof(*c)); c->fd = fd; c->closed = 1; }
void edbus_conn_close(EdBusConn *c) { free(c->in); free(c->out); memset(c, 0, sizeof(*c)); c->fd = -1; c->closed = 1; }
int edbus_send(EdBusConn *c, const cJSON *msg) { (void) c; (void) msg; return 0; }
int edbus_send_text(EdBusConn *c, const char *json, const size_t len) { (void) c; (void) json; (void) len; return 0; }
int edbus_flush(EdBusConn *c) { (void) c; return -1; }
int edbus_flush_blocking(EdBusConn *c, const int timeout_ms) { (void) c; (void) timeout_ms; return 0; }
int edbus_read(EdBusConn *c) { (void) c; return -1; }
cJSON *edbus_next(EdBusConn *c, int *bad) { (void) c; if (bad) *bad = 0; return NULL; }
cJSON *edbus_recv(EdBusConn *c, const int timeout_ms) { (void) c; (void) timeout_ms; return NULL; }
int edbus_wait_readable(const int fd, const int timeout_ms) { (void) fd; (void) timeout_ms; return -1; }
int edbus_listen(const EdBusPaths *p) { (void) p; return -1; }
int edbus_accept(const int listen_fd) { (void) listen_fd; return -1; }
int edbus_connect(const EdBusPaths *p) { (void) p; return -1; }
void edbus_close_fd(const int fd) { (void) fd; }
int edbus_lock_try(const EdBusPaths *p) { (void) p; return -1; }
void edbus_lock_release(const int fd) { (void) fd; }
void edbus_unlink(const EdBusPaths *p) { (void) p; }
int edbus_client_connect(EdBusConn *c, const EdBusPaths *p, const char *role, const char *client_id, const char *name,
                         const char *version, const int timeout_ms, cJSON **welcome) {
    (void) c; (void) p; (void) role; (void) client_id; (void) name; (void) version; (void) timeout_ms;
    if (welcome) *welcome = NULL;
    return 0;
}

#endif
