// Discord Rich Presence over the local IPC socket (client/include/discord_rpc.h).
#include <discord_rpc.h>

#if defined(__EMSCRIPTEN__)

void discord_rpc_set_enabled(const int enabled) { (void) enabled; }
int discord_rpc_enabled(void) { return 0; }
void discord_rpc_set_activity(const char *details, const char *state, const long long start_unix) {
    (void) details; (void) state; (void) start_unix;
}
void discord_rpc_poll(void) {}
void discord_rpc_shutdown(void) {}

#else

#include <cJSON.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

// Discord's "Krunker" application (GET /api/v9/applications/detectable: the official
// client's executable "krunker/official krunker.io client.exe").
#define DISCORD_KRUNKER_APP_ID "1124349948201279568"
#define RPC_RETRY_SECONDS 10.0
#define RPC_FIELD_MAX 128
#define RPC_IN_MAX 16384

enum { OP_HANDSHAKE = 0, OP_FRAME = 1, OP_CLOSE = 2, OP_PING = 3, OP_PONG = 4 };

typedef struct {
    int enabled;
    int disabled_by_env;
#ifdef _WIN32
    HANDLE pipe;
#else
    int fd;
#endif
    int connected;     // socket open, handshake sent
    int ready;         // READY received
    int sent;          // current activity delivered
    double next_try;   // monotonic seconds
    unsigned nonce;
    char details[RPC_FIELD_MAX], state[RPC_FIELD_MAX];
    long long start;
    int has_activity;
    unsigned char in[RPC_IN_MAX];
    size_t in_len;
} Rpc;

static Rpc g_rpc = {
#ifdef _WIN32
    .pipe = INVALID_HANDLE_VALUE,
#else
    .fd = -1,
#endif
};

static double now_seconds(void) {
#ifdef _WIN32
    return (double) GetTickCount64() / 1000.0;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec + (double) ts.tv_nsec / 1e9;
#endif
}

static const char *app_id(void) {
    const char *id = getenv("KRUNKNATIVE_DISCORD_APP_ID");
    return id && *id ? id : DISCORD_KRUNKER_APP_ID;
}

// ---------------------------------------------------------------- transport

static void rpc_close(void) {
#ifdef _WIN32
    if (g_rpc.pipe != INVALID_HANDLE_VALUE) CloseHandle(g_rpc.pipe);
    g_rpc.pipe = INVALID_HANDLE_VALUE;
#else
    if (g_rpc.fd >= 0) close(g_rpc.fd);
    g_rpc.fd = -1;
#endif
    if (g_rpc.connected) printf("[discord] disconnected\n");
    g_rpc.connected = g_rpc.ready = g_rpc.sent = 0;
    g_rpc.in_len = 0;
    g_rpc.next_try = now_seconds() + RPC_RETRY_SECONDS;
}

static int rpc_write(const void *data, const size_t len) {
#ifdef _WIN32
    DWORD written = 0;
    return WriteFile(g_rpc.pipe, data, (DWORD) len, &written, NULL) && written == (DWORD) len;
#else
    const unsigned char *p = data;
    size_t left = len;
    int spins = 0;
    while (left) {
#ifdef MSG_NOSIGNAL
        const ssize_t n = send(g_rpc.fd, p, left, MSG_NOSIGNAL);
#else
        const ssize_t n = send(g_rpc.fd, p, left, 0);
#endif
        if (n > 0) {
            p += n;
            left -= (size_t) n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) && spins++ < 1000) continue; // frames are tiny
        return 0;
    }
    return 1;
#endif
}

static int rpc_send(const uint32_t op, const char *json) {
    const size_t len = strlen(json);
    unsigned char header[8];
    const uint32_t l = (uint32_t) len;
    for (int i = 0; i < 4; i++) {
        header[i] = (unsigned char) (op >> (8 * i));
        header[4 + i] = (unsigned char) (l >> (8 * i));
    }
    if (rpc_write(header, sizeof(header)) && rpc_write(json, len)) return 1;
    rpc_close();
    return 0;
}

#ifndef _WIN32
static int try_socket(const char *path) {
    struct sockaddr_un addr;
    if (strlen(path) >= sizeof(addr.sun_path)) return -1;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
#ifdef SO_NOSIGPIPE
    const int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    if (connect(fd, (struct sockaddr *) &addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    return fd;
}
#endif

static int rpc_connect(void) {
#ifdef _WIN32
    for (int n = 0; n < 10; n++) {
        char name[64];
        snprintf(name, sizeof(name), "\\\\.\\pipe\\discord-ipc-%d", n);
        HANDLE h = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) continue;
        g_rpc.pipe = h;
        return 1;
    }
    return 0;
#else
    // the official client, then the Flatpak / Snap / Vesktop sandboxes' socket paths
    static const char *const SUBDIRS[] = {"", "app/com.discordapp.Discord/", "snap.discord/", ".flatpak/dev.vencord.Vesktop/xdg-run/"};
    const char *dirs[5] = {getenv("XDG_RUNTIME_DIR"), getenv("TMPDIR"), getenv("TMP"), getenv("TEMP"), "/tmp"};
    for (size_t d = 0; d < sizeof(dirs) / sizeof(dirs[0]); d++) {
        if (!dirs[d] || !*dirs[d]) continue;
        for (size_t s = 0; s < sizeof(SUBDIRS) / sizeof(SUBDIRS[0]); s++) {
            for (int n = 0; n < 10; n++) {
                char path[512];
                const size_t dl = strlen(dirs[d]);
                snprintf(path, sizeof(path), "%s%s%sdiscord-ipc-%d", dirs[d], dl && dirs[d][dl - 1] == '/' ? "" : "/", SUBDIRS[s], n);
                const int fd = try_socket(path);
                if (fd >= 0) {
                    g_rpc.fd = fd;
                    return 1;
                }
            }
        }
    }
    return 0;
#endif
}

// Bytes waiting on the socket/pipe into g_rpc.in; 0 when the connection dropped.
static int rpc_read(void) {
    if (g_rpc.in_len >= sizeof(g_rpc.in)) return 0; // a frame larger than we accept
#ifdef _WIN32
    DWORD avail = 0;
    if (!PeekNamedPipe(g_rpc.pipe, NULL, 0, NULL, &avail, NULL)) return 0;
    if (!avail) return 1;
    DWORD want = (DWORD) (sizeof(g_rpc.in) - g_rpc.in_len), got = 0;
    if (want > avail) want = avail;
    if (!ReadFile(g_rpc.pipe, g_rpc.in + g_rpc.in_len, want, &got, NULL)) return 0;
    g_rpc.in_len += got;
    return 1;
#else
    for (;;) {
        const ssize_t n = recv(g_rpc.fd, g_rpc.in + g_rpc.in_len, sizeof(g_rpc.in) - g_rpc.in_len, 0);
        if (n > 0) {
            g_rpc.in_len += (size_t) n;
            if (g_rpc.in_len >= sizeof(g_rpc.in)) return 1;
            continue;
        }
        if (n == 0) return 0;
        return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
    }
#endif
}

// ---------------------------------------------------------------- protocol

static void send_activity(void) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "cmd", "SET_ACTIVITY");
    cJSON *args = cJSON_AddObjectToObject(root, "args");
#ifdef _WIN32
    cJSON_AddNumberToObject(args, "pid", (double) GetCurrentProcessId());
#else
    cJSON_AddNumberToObject(args, "pid", (double) getpid());
#endif
    if (g_rpc.has_activity) {
        cJSON *activity = cJSON_AddObjectToObject(args, "activity");
        if (g_rpc.details[0]) cJSON_AddStringToObject(activity, "details", g_rpc.details);
        if (g_rpc.state[0]) cJSON_AddStringToObject(activity, "state", g_rpc.state);
        if (g_rpc.start > 0) cJSON_AddNumberToObject(cJSON_AddObjectToObject(activity, "timestamps"), "start", (double) g_rpc.start);
        cJSON_AddBoolToObject(activity, "instance", 0);
    }
    char nonce[16];
    snprintf(nonce, sizeof(nonce), "%u", ++g_rpc.nonce);
    cJSON_AddStringToObject(root, "nonce", nonce);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json && rpc_send(OP_FRAME, json)) g_rpc.sent = 1;
    free(json);
}

static void handle_frame(const uint32_t op, const char *payload, const uint32_t len) {
    if (op == OP_PING) {
        char *copy = malloc(len + 1);
        if (!copy) return;
        memcpy(copy, payload, len);
        copy[len] = 0;
        rpc_send(OP_PONG, copy);
        free(copy);
        return;
    }
    if (op == OP_CLOSE) {
        printf("[discord] closed by Discord: %.*s\n", (int) (len < 200 ? len : 200), payload);
        rpc_close();
        return;
    }
    if (op != OP_FRAME) return;
    cJSON *msg = cJSON_ParseWithLength(payload, len);
    const char *evt = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "evt"));
    if (evt && !strcmp(evt, "READY")) {
        g_rpc.ready = 1;
        g_rpc.sent = 0;
        const cJSON *user = cJSON_GetObjectItem(cJSON_GetObjectItem(msg, "data"), "user");
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(user, "username"));
        printf("[discord] connected%s%s\n", name ? " as " : "", name ? name : "");
    } else if (evt && !strcmp(evt, "ERROR")) {
        const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(msg, "data"), "message"));
        fprintf(stderr, "[discord] error: %s\n", text ? text : "?");
    }
    cJSON_Delete(msg);
}

static void parse_frames(void) {
    size_t off = 0;
    while (g_rpc.in_len - off >= 8) {
        const unsigned char *h = g_rpc.in + off;
        const uint32_t op = (uint32_t) h[0] | (uint32_t) h[1] << 8 | (uint32_t) h[2] << 16 | (uint32_t) h[3] << 24;
        const uint32_t len = (uint32_t) h[4] | (uint32_t) h[5] << 8 | (uint32_t) h[6] << 16 | (uint32_t) h[7] << 24;
        if (len > sizeof(g_rpc.in) - 8) {
            rpc_close();
            return;
        }
        if (g_rpc.in_len - off - 8 < len) break;
        handle_frame(op, (const char *) h + 8, len);
        if (!g_rpc.connected) return; // closed while handling
        off += 8 + len;
    }
    memmove(g_rpc.in, g_rpc.in + off, g_rpc.in_len - off);
    g_rpc.in_len -= off;
}

// ---------------------------------------------------------------- API

void discord_rpc_set_enabled(const int enabled) {
    const char *env = getenv("KRUNKNATIVE_DISCORD");
    g_rpc.disabled_by_env = env && !strcmp(env, "0");
    const int on = enabled && !g_rpc.disabled_by_env;
    if (on == g_rpc.enabled) return;
    if (!on && g_rpc.ready) {
        const int had = g_rpc.has_activity;
        g_rpc.has_activity = 0; // SET_ACTIVITY without an activity clears it
        send_activity();
        g_rpc.has_activity = had;
    }
    g_rpc.enabled = on;
    if (!on) {
        rpc_close();
        g_rpc.next_try = 0.0;
    } else {
        g_rpc.next_try = 0.0;
    }
}

int discord_rpc_enabled(void) { return g_rpc.enabled; }

void discord_rpc_set_activity(const char *details, const char *state, const long long start_unix) {
    char d[RPC_FIELD_MAX], s[RPC_FIELD_MAX];
    snprintf(d, sizeof(d), "%s", details ? details : "");
    snprintf(s, sizeof(s), "%s", state ? state : "");
    if (g_rpc.has_activity && !strcmp(d, g_rpc.details) && !strcmp(s, g_rpc.state) && start_unix == g_rpc.start) return;
    memcpy(g_rpc.details, d, sizeof(d));
    memcpy(g_rpc.state, s, sizeof(s));
    g_rpc.start = start_unix;
    g_rpc.has_activity = 1;
    g_rpc.sent = 0;
}

void discord_rpc_poll(void) {
    if (!g_rpc.enabled) return;
    if (!g_rpc.connected) {
        if (now_seconds() < g_rpc.next_try) return;
        if (!rpc_connect()) {
            g_rpc.next_try = now_seconds() + RPC_RETRY_SECONDS;
            return;
        }
        g_rpc.connected = 1;
        char hello[96];
        snprintf(hello, sizeof(hello), "{\"v\":1,\"client_id\":\"%s\"}", app_id());
        if (!rpc_send(OP_HANDSHAKE, hello)) return;
    }
    if (!rpc_read()) {
        rpc_close();
        return;
    }
    parse_frames();
    if (g_rpc.connected && g_rpc.ready && !g_rpc.sent && g_rpc.has_activity) send_activity();
}

void discord_rpc_shutdown(void) {
    if (g_rpc.connected) rpc_close();
    g_rpc.enabled = 0;
}

#endif
