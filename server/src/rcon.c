// Remote console: local TCP listener + coordinator frames (server/include/rcon.h).
#include <rcon.h>
#include <brain.h>
#include <server_cli.h>
#include <server_config.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RCON_OUTPUT_MAX 16384
#define RCON_LINE_MAX 512

#ifndef WIN32
#include <netdb.h>
#include <poll.h>
#include <unistd.h>

static struct {
    int fd;
    pthread_t thread;
    int started;
    volatile int stopping;
} g_rcon = {.fd = -1};

static int rcon_send_all(const int fd, const char *text, const size_t len) {
    size_t sent = 0;
    while (sent < len) {
        const ssize_t n = send(fd, text + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) return -1;
        sent += (size_t) n;
    }
    return 0;
}

// Reads one line (\n, \r stripped); 0 on EOF / timeout / overlong line.
static int rcon_read_line(const int fd, char *line, const size_t size) {
    size_t n = 0;
    for (;;) {
        char c;
        const ssize_t got = recv(fd, &c, 1, 0);
        if (got <= 0) return 0;
        if (c == '\n') break;
        if (c == '\r') continue;
        if (n + 1 >= size) return 0;
        line[n++] = c;
    }
    line[n] = 0;
    return 1;
}

// constant time compare (the password)
static int rcon_streq(const char *a, const char *b) {
    const size_t la = strlen(a), lb = strlen(b);
    unsigned char diff = (unsigned char) (la != lb);
    for (size_t i = 0; i < la; i++) diff |= (unsigned char) (a[i] ^ b[i % (lb ? lb : 1)]);
    return diff == 0;
}

static void *rcon_session(void *arg) {
    const int fd = (int) (intptr_t) arg;
    const struct timeval tv = {300, 0}; // idle sessions close after 5 minutes
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    char line[RCON_LINE_MAX];
    if (!rcon_read_line(fd, line, sizeof(line)) || !rcon_streq(line, g_gs_config.rcon_password)) {
        rcon_send_all(fd, "ERR bad password\n", 17);
        server_log_error("[rcon] rejected a connection (bad password)");
        sleep(1); // slows down guessing
        close(fd);
        return NULL;
    }
    rcon_send_all(fd, "OK\n", 3);
    server_log("[rcon] session opened");
    char *out = malloc(RCON_OUTPUT_MAX);
    while (out && !g_rcon.stopping && rcon_read_line(fd, line, sizeof(line))) {
        if (!line[0]) continue;
        server_log("[rcon] %s", line);
        cli_run_command(line, out, RCON_OUTPUT_MAX);
        if (rcon_send_all(fd, out, strlen(out)) || rcon_send_all(fd, ".\n", 2)) break;
    }
    free(out);
    close(fd);
    return NULL;
}

static void *rcon_listen_main(void *arg) {
    (void) arg;
    while (!g_rcon.stopping) {
        struct pollfd pfd = {.fd = g_rcon.fd, .events = POLLIN};
        if (poll(&pfd, 1, 200) <= 0) continue;
        const int client = accept(g_rcon.fd, NULL, NULL);
        if (client < 0) continue;
        pthread_t t;
        if (pthread_create(&t, NULL, rcon_session, (void *) (intptr_t) client)) close(client);
        else pthread_detach(t);
    }
    return NULL;
}

int rcon_start(void) {
    const GsConfig *cfg = &g_gs_config;
    if (!cfg->rcon_port || !cfg->rcon_password[0]) return 0;
    struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM, .ai_flags = AI_PASSIVE}, *res = NULL;
    char port[8];
    snprintf(port, sizeof(port), "%u", (unsigned int) cfg->rcon_port);
    if (getaddrinfo(cfg->rcon_host[0] ? cfg->rcon_host : "127.0.0.1", port, &hints, &res) || !res) {
        server_log_error("[rcon] cannot resolve rcon_host '%s'", cfg->rcon_host);
        return -1;
    }
    const int fd = (int) socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    const int reuse = 1;
    if (fd >= 0) setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    if (fd < 0 || bind(fd, res->ai_addr, res->ai_addrlen) || listen(fd, 4)) {
        server_log_error("[rcon] cannot listen on %s:%s", cfg->rcon_host, port);
        if (fd >= 0) close(fd);
        freeaddrinfo(res);
        return -1;
    }
    freeaddrinfo(res);
    g_rcon.fd = fd;
    if (pthread_create(&g_rcon.thread, NULL, rcon_listen_main, NULL)) {
        close(fd);
        g_rcon.fd = -1;
        return -1;
    }
    g_rcon.started = 1;
    server_log("[rcon] listening on %s:%s (TCP, password required)", cfg->rcon_host, port);
    return 0;
}

void rcon_stop(void) {
    g_rcon.stopping = 1;
    if (g_rcon.started) pthread_join(g_rcon.thread, NULL);
    g_rcon.started = 0;
    if (g_rcon.fd >= 0) close(g_rcon.fd);
    g_rcon.fd = -1;
}

// ------------------------------------------------------------------ client mode

static int rcon_client_command(const int fd, const char *command) {
    if (rcon_send_all(fd, command, strlen(command)) || rcon_send_all(fd, "\n", 1)) return -1;
    char line[4096];
    while (rcon_read_line(fd, line, sizeof(line))) {
        if (!strcmp(line, ".")) return 0;
        printf("%s\n", line);
    }
    return -1;
}

int rcon_client_main(const char *target, const char *password, const int argc, char **argv) {
    char host[256] = "127.0.0.1", port[8] = "27015";
    const char *colon = target ? strrchr(target, ':') : NULL;
    if (target && colon) {
        snprintf(host, sizeof(host), "%.*s", (int) (colon - target), target);
        snprintf(port, sizeof(port), "%s", colon + 1);
    } else if (target && target[0]) {
        snprintf(port, sizeof(port), "%s", target);
    }
    if (!password || !password[0]) password = getenv("KN_RCON_PASSWORD");
    if (!password || !password[0]) {
        fprintf(stderr, "rcon: --rcon-password (or KN_RCON_PASSWORD) is required\n");
        return 2;
    }
    struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM}, *res = NULL;
    if (getaddrinfo(host, port, &hints, &res) || !res) {
        fprintf(stderr, "rcon: cannot resolve %s\n", host);
        return 1;
    }
    int fd = -1;
    for (struct addrinfo *a = res; a && fd < 0; a = a->ai_next) {
        fd = (int) socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd >= 0 && connect(fd, a->ai_addr, a->ai_addrlen)) {
            close(fd);
            fd = -1;
        }
    }
    freeaddrinfo(res);
    if (fd < 0) {
        fprintf(stderr, "rcon: cannot connect to %s:%s\n", host, port);
        return 1;
    }
    char line[RCON_LINE_MAX];
    if (rcon_send_all(fd, password, strlen(password)) || rcon_send_all(fd, "\n", 1) || !rcon_read_line(fd, line, sizeof(line)) ||
        strcmp(line, "OK")) {
        fprintf(stderr, "rcon: %s\n", line[0] ? line : "authentication failed");
        close(fd);
        return 1;
    }
    int rc = 0;
    if (argc > 0) {
        char command[RCON_LINE_MAX] = "";
        for (int i = 0; i < argc; i++) {
            const size_t used = strlen(command);
            snprintf(command + used, sizeof(command) - used, "%s%s", used ? " " : "", argv[i]);
        }
        rc = rcon_client_command(fd, command) ? 1 : 0;
    } else {
        while (fgets(line, sizeof(line), stdin)) {
            line[strcspn(line, "\r\n")] = 0;
            if (!line[0]) continue;
            if (rcon_client_command(fd, line)) { rc = 1; break; }
        }
    }
    close(fd);
    return rc;
}

#else
int rcon_start(void) { return 0; }
void rcon_stop(void) {}
int rcon_client_main(const char *target, const char *password, int argc, char **argv) {
    (void) target; (void) password; (void) argc; (void) argv;
    fprintf(stderr, "rcon client: not supported on Windows builds yet\n");
    return 2;
}
#endif

// ------------------------------------------------------------------ coordinator rcon

typedef struct {
    cJSON *rid;
    char command[RCON_LINE_MAX];
} RconJob;

static void *rcon_brain_job(void *arg) {
    RconJob *job = arg;
    char *out = malloc(RCON_OUTPUT_MAX);
    if (out) cli_run_command(job->command, out, RCON_OUTPUT_MAX);
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddStringToObject(reply, "type", "rcon-reply");
    cJSON_AddItemToObject(reply, "rid", job->rid);
    cJSON_AddBoolToObject(reply, "ok", out != NULL);
    cJSON_AddStringToObject(reply, "output", out ? out : "");
    brain_post(reply, 0);
    free(out);
    free(job);
    return NULL;
}

void rcon_handle_brain_frame(const cJSON *frame) {
    const char *command = cJSON_GetStringValue(cJSON_GetObjectItem(frame, "command"));
    if (!command || !command[0]) return;
    RconJob *job = calloc(1, sizeof(*job));
    if (!job) return;
    const cJSON *rid = cJSON_GetObjectItem(frame, "rid");
    job->rid = rid ? cJSON_Duplicate(rid, 1) : cJSON_CreateNull();
    snprintf(job->command, sizeof(job->command), "%s", command);
    server_log("[rcon] coordinator: %s", job->command);
    // the console commands take g_server->lock themselves; the caller holds it: run detached
    pthread_t t;
    if (pthread_create(&t, NULL, rcon_brain_job, job)) {
        cJSON_Delete(job->rid);
        free(job);
        return;
    }
    pthread_detach(t);
}
