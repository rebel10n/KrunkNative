/*
 * services/brain/ai_search.c - web search for the editor's AI (the server tools of ai.h): a
 * DuckDuckGo MCP server (duckduckgo-mcp-server, pinned; deploy/ansible role kn_ai_search installs
 * it with uv) run as a child process of the brain and spoken to over stdio (MCP JSON-RPC,
 * newline-delimited). No network listener at all: only this brain reaches it, and the client never
 * does. The model sees two tools:
 *
 *   web_search {query, max_results?}            -> MCP "search"        (titles, URLs, snippets)
 *   web_fetch  {url, start_index?, max_length?} -> MCP "fetch_content" (a page's readable text)
 *
 * Config:
 *   ai.search.command   the server's command line (split on spaces; "" = no web search), e.g.
 *                       /opt/krunknative-ai/bin/duckduckgo-mcp-server  or  uvx duckduckgo-mcp-server==0.7.0
 *   ai.search.timeout_ms  per call (default 45000)
 *   ai.search.env       extra "NAME=value" entries for the child (list); the child's environment
 *                       is otherwise only PATH, HOME, LANG and UV_* / XDG_CACHE_HOME of the brain's,
 *                       never the brain's secrets (KN_AI_UPSTREAM_KEY, database passwords, ...)
 *
 * The child is restarted with a backoff when it exits; calls in flight fail with an error the
 * model sees. Tests use a stub MCP server (services/brain/tests/ai_stub.py mcp), never the web.
 */
#include "build_info.h"

#include <errno.h>
#include <stdint.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

#include "ai.h"
#include "ai_proto.h"
#include "knsvc/config.h"
#include "knsvc/json.h"
#include "knsvc/log.h"
#include "knsvc/loop.h"

#define MOD "ai-search"
#define MAX_PENDING 64
#define MAX_ARGS 32

typedef struct {
    int id;                /* JSON-RPC id, 0 = free */
    AiToolDoneFn done;
    void *ud;
    kn_timer_id timer;
} Pending;

static struct {
    Brain *b;
    char cmd[1024];
    char *argv[MAX_ARGS + 1];
    char argbuf[1024];
    const char *extra_env[16];
    size_t nextra;
    int timeout_ms;
    pid_t pid;
    int in_fd, out_fd, err_fd;
    int in_watched;        /* in_fd registered for KN_EV_WRITE (output pending) */
    AiBuf rbuf, ebuf, wbuf;
    size_t woff;
    int state;             /* 0 stopped, 1 initializing, 2 listing, 3 ready */
    int has_search, has_fetch;
    int next_id;
    int init_id, list_id;
    Pending pending[MAX_PENDING];
    kn_timer_id restart_timer;
    int backoff_ms;
    int stopping;
} S = {.pid = -1, .in_fd = -1, .out_fd = -1, .err_fd = -1};

static void child_start(kn_loop *loop, void *ud);

/* ------------------------------------------------------------------ tool surface (ai.h) */

static int owns(const char *name) { return name && (!strcmp(name, "web_search") || !strcmp(name, "web_fetch")); }

static cJSON *defs(void) {
    if (S.state != 3 || !S.has_search) return NULL;
    cJSON *a = cJSON_CreateArray();
    cJSON *t = cJSON_Parse(
        "{\"type\":\"function\",\"function\":{\"name\":\"web_search\",\"description\":\"Search the web (DuckDuckGo). Returns "
        "titles, URLs and snippets. Use specific queries; read a result with web_fetch.\",\"parameters\":{\"type\":\"object\","
        "\"properties\":{\"query\":{\"type\":\"string\",\"description\":\"what to search for\"},\"max_results\":{\"type\":\"integer\","
        "\"minimum\":1,\"maximum\":10,\"description\":\"default 5\"}},\"required\":[\"query\"]}}}");
    if (t) cJSON_AddItemToArray(a, t);
    if (S.has_fetch) {
        t = cJSON_Parse(
            "{\"type\":\"function\",\"function\":{\"name\":\"web_fetch\",\"description\":\"Fetch a web page (a URL or a ref:// "
            "token from web_search) and return its readable text, 8000 characters at a time (start_index pages further).\","
            "\"parameters\":{\"type\":\"object\",\"properties\":{\"url\":{\"type\":\"string\"},\"start_index\":{\"type\":\"integer\","
            "\"minimum\":0},\"max_length\":{\"type\":\"integer\",\"minimum\":500,\"maximum\":20000}},\"required\":[\"url\"]}}}");
        if (t) cJSON_AddItemToArray(a, t);
    }
    return a;
}

/* ------------------------------------------------------------------ pending calls */

static Pending *pending_new(AiToolDoneFn done, void *ud) {
    for (int i = 0; i < MAX_PENDING; i++) {
        if (!S.pending[i].id) {
            Pending *p = &S.pending[i];
            p->id = ++S.next_id;
            p->done = done;
            p->ud = ud;
            return p;
        }
    }
    return NULL;
}

static void pending_finish(Pending *p, const int is_error, const char *text) {
    if (!p || !p->id) return;
    AiToolDoneFn done = p->done;
    void *ud = p->ud;
    if (p->timer) kn_loop_cancel_timer(S.b->loop, p->timer);
    memset(p, 0, sizeof(*p));
    if (done) done(ud, is_error, text);
}

static void fail_all(const char *why) {
    for (int i = 0; i < MAX_PENDING; i++) {
        if (S.pending[i].id) pending_finish(&S.pending[i], 1, why);
    }
}

static void pending_timeout(kn_loop *loop, void *ud) {
    (void) loop;
    Pending *p = ud;
    p->timer = 0;
    pending_finish(p, 1, "the web search service timed out");
}

/* ------------------------------------------------------------------ child I/O */

static void close_fd(int *fd) {
    if (*fd >= 0) {
        if (fd != &S.in_fd || S.in_watched) kn_loop_del_fd(S.b->loop, *fd);
        if (fd == &S.in_fd) S.in_watched = 0;
        close(*fd);
        *fd = -1;
    }
}

static void child_stop(const char *why) {
    close_fd(&S.in_fd);
    close_fd(&S.out_fd);
    close_fd(&S.err_fd);
    if (S.pid > 0) {
        kill(S.pid, SIGTERM);
        int st;
        for (int i = 0; i < 50 && waitpid(S.pid, &st, WNOHANG) == 0; i++) {
            struct timespec ts = {0, 20 * 1000000L};
            nanosleep(&ts, NULL);
        }
        if (waitpid(S.pid, &st, WNOHANG) == 0) {
            kill(S.pid, SIGKILL);
            waitpid(S.pid, &st, 0);
        }
    }
    S.pid = -1;
    S.state = 0;
    ai_buf_free(&S.rbuf);
    ai_buf_free(&S.ebuf);
    ai_buf_free(&S.wbuf);
    S.woff = 0;
    fail_all(why);
}

static void schedule_restart(void) {
    if (S.stopping || S.restart_timer) return;
    S.backoff_ms = S.backoff_ms ? (S.backoff_ms * 2 > 60000 ? 60000 : S.backoff_ms * 2) : 2000;
    KN_WARN(MOD, "web search server stopped: restarting in %d s", S.backoff_ms / 1000);
    S.restart_timer = kn_loop_add_timer(S.b->loop, (uint64_t) S.backoff_ms, 0, child_start, NULL);
}

static void on_write(kn_loop *loop, int fd, unsigned ev, void *ud);

static void flush_writes(void) {
    while (S.in_fd >= 0 && S.woff < S.wbuf.len) {
        const ssize_t n = write(S.in_fd, S.wbuf.data + S.woff, S.wbuf.len - S.woff);
        if (n > 0) {
            S.woff += (size_t) n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (!S.in_watched) S.in_watched = kn_loop_add_fd(S.b->loop, S.in_fd, KN_EV_WRITE, on_write, NULL) == 0;
            return;
        }
        child_stop("the web search service closed its input");
        schedule_restart();
        return;
    }
    if (S.woff >= S.wbuf.len) {
        S.wbuf.len = 0;
        S.woff = 0;
        if (S.in_watched && S.in_fd >= 0) kn_loop_del_fd(S.b->loop, S.in_fd);
        S.in_watched = 0;
    }
}

static void on_write(kn_loop *loop, int fd, unsigned ev, void *ud) {
    (void) loop; (void) fd; (void) ud;
    if (ev & (KN_EV_ERROR | KN_EV_HUP)) {
        child_stop("the web search service exited");
        schedule_restart();
        return;
    }
    flush_writes();
}

static void send_msg(cJSON *msg) {
    char *t = cJSON_PrintUnformatted(msg);
    cJSON_Delete(msg);
    if (!t) return;
    ai_buf_add(&S.wbuf, t, strlen(t));
    ai_buf_add(&S.wbuf, "\n", 1);
    free(t);
    flush_writes();
}

static cJSON *rpc(const char *method, const int id) {
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "jsonrpc", "2.0");
    if (id) cJSON_AddNumberToObject(m, "id", id);
    cJSON_AddStringToObject(m, "method", method);
    return m;
}

/* text of a tools/call result: its text blocks joined */
static char *result_text(const cJSON *res) {
    AiBuf b = {0};
    const cJSON *c;
    cJSON_ArrayForEach(c, cJSON_GetObjectItemCaseSensitive(res, "content")) {
        const char *t = kn_json_str(c, "text", NULL);
        if (!t) continue;
        if (b.len) ai_buf_add(&b, "\n", 1);
        ai_buf_add(&b, t, strlen(t));
    }
    if (!b.data) ai_buf_add(&b, "(no results)", 12);
    return b.data;
}

static void handle_message(const cJSON *m) {
    const cJSON *idv = cJSON_GetObjectItemCaseSensitive(m, "id");
    if (!cJSON_IsNumber(idv)) return; /* notifications (logging) */
    const int id = idv->valueint;
    const cJSON *err = cJSON_GetObjectItemCaseSensitive(m, "error");
    const cJSON *res = cJSON_GetObjectItemCaseSensitive(m, "result");
    if (id == S.init_id) {
        if (err || !res) {
            KN_ERROR(MOD, "web search server: initialize failed (%s)", kn_json_str(err, "message", "no result"));
            child_stop("the web search service did not start");
            schedule_restart();
            return;
        }
        send_msg(rpc("notifications/initialized", 0));
        S.state = 2;
        S.list_id = ++S.next_id;
        send_msg(rpc("tools/list", S.list_id));
        return;
    }
    if (id == S.list_id) {
        S.has_search = S.has_fetch = 0;
        const cJSON *t;
        cJSON_ArrayForEach(t, cJSON_GetObjectItemCaseSensitive(res, "tools")) {
            const char *n = kn_json_str(t, "name", "");
            S.has_search |= !strcmp(n, "search");
            S.has_fetch |= !strcmp(n, "fetch_content");
        }
        S.state = 3;
        S.backoff_ms = 0;
        if (S.has_search) KN_INFO(MOD, "web search ready (pid %d: search%s)", (int) S.pid, S.has_fetch ? ", fetch_content" : "");
        else KN_ERROR(MOD, "the web search server has no \"search\" tool: web search off");
        return;
    }
    for (int i = 0; i < MAX_PENDING; i++) {
        if (S.pending[i].id != id) continue;
        if (err) {
            char msg[300];
            snprintf(msg, sizeof(msg), "web search error: %s", kn_json_str(err, "message", "failed"));
            pending_finish(&S.pending[i], 1, msg);
        } else {
            char *text = result_text(res);
            pending_finish(&S.pending[i], kn_json_bool(res, "isError", 0), text ? text : "");
            free(text);
        }
        return;
    }
}

static void on_read(kn_loop *loop, int fd, unsigned ev, void *ud) {
    (void) loop; (void) ev;
    const int is_err = ud != NULL;
    AiBuf *buf = is_err ? &S.ebuf : &S.rbuf;
    char chunk[16384];
    for (;;) {
        const ssize_t n = read(fd, chunk, sizeof(chunk));
        if (n > 0) {
            ai_buf_add(buf, chunk, (size_t) n);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        /* EOF / error */
        if (!is_err) {
            child_stop("the web search service exited");
            schedule_restart();
        } else {
            close_fd(&S.err_fd);
        }
        return;
    }
    /* whole lines */
    size_t start = 0;
    for (size_t i = 0; i < buf->len; i++) {
        if (buf->data[i] != '\n') continue;
        buf->data[i] = 0;
        const char *line = buf->data + start;
        if (is_err) {
            if (line[0]) KN_DEBUG(MOD, "server: %s", line);
        } else if (line[0]) {
            cJSON *m = cJSON_Parse(line);
            if (m) handle_message(m);
            cJSON_Delete(m);
            if (S.out_fd < 0) return; /* stopped while handling */
        }
        start = i + 1;
    }
    if (start) {
        memmove(buf->data, buf->data + start, buf->len - start);
        buf->len -= start;
        if (buf->data) buf->data[buf->len] = 0;
    }
    if (buf->len > (8u << 20)) { /* a runaway line */
        child_stop("the web search service sent too much");
        schedule_restart();
    }
}

static void split_command(void) {
    snprintf(S.argbuf, sizeof(S.argbuf), "%s", S.cmd);
    int n = 0;
    char *p = S.argbuf;
    while (*p && n < MAX_ARGS) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (*p == '"' || *p == '\'') {
            const char q = *p++;
            S.argv[n++] = p;
            while (*p && *p != q) p++;
        } else {
            S.argv[n++] = p;
            while (*p && *p != ' ' && *p != '\t') p++;
        }
        if (*p) *p++ = 0;
    }
    S.argv[n] = NULL;
}

static void child_start(kn_loop *loop, void *ud) {
    (void) loop; (void) ud;
    S.restart_timer = 0;
    if (S.stopping || S.pid > 0 || !S.argv[0]) return;
    int in[2], out[2], err[2];
    if (pipe(in) || pipe(out) || pipe(err)) {
        KN_ERROR(MOD, "pipe: %s", strerror(errno));
        schedule_restart();
        return;
    }
    /* the child's environment: nothing secret of the brain's */
    static char env_store[16][1024];
    char *envp[40];
    int ne = 0;
    const char *keep[] = {"PATH", "HOME", "LANG", "LC_ALL", "TZ", "XDG_CACHE_HOME", "UV_CACHE_DIR", "UV_TOOL_DIR", "UV_PYTHON_INSTALL_DIR", NULL};
    for (int i = 0; keep[i] && ne < 15; i++) {
        const char *v = getenv(keep[i]);
        if (v) {
            snprintf(env_store[ne], sizeof(env_store[0]), "%s=%s", keep[i], v);
            envp[ne] = env_store[ne];
            ne++;
        }
    }
    if (!getenv("PATH") && ne < 15) {
        snprintf(env_store[ne], sizeof(env_store[0]), "PATH=/usr/local/bin:/usr/bin:/bin");
        envp[ne] = env_store[ne];
        ne++;
    }
    for (size_t i = 0; i < S.nextra && ne < 38; i++) envp[ne++] = (char *) (uintptr_t) S.extra_env[i];
    envp[ne] = NULL;
    /* the executable (PATH searched here: between fork and exec only async-signal-safe calls) */
    static char exe[1024];
    exe[0] = 0;
    if (strchr(S.argv[0], '/')) snprintf(exe, sizeof(exe), "%s", S.argv[0]);
    else {
        const char *path = NULL;
        for (int i = 0; envp[i]; i++) {
            if (!strncmp(envp[i], "PATH=", 5)) path = envp[i] + 5;
        }
        for (const char *p = path ? path : "/usr/bin:/bin"; p && *p && !exe[0];) {
            const char *colon = strchr(p, ':');
            const size_t len = colon ? (size_t) (colon - p) : strlen(p);
            char full[1024];
            snprintf(full, sizeof(full), "%.*s/%s", (int) len, p, S.argv[0]);
            if (access(full, X_OK) == 0) snprintf(exe, sizeof(exe), "%s", full);
            p = colon ? colon + 1 : NULL;
        }
    }
    if (!exe[0]) {
        KN_ERROR(MOD, "ai.search.command: %s not found", S.argv[0]);
        close(in[0]), close(in[1]), close(out[0]), close(out[1]), close(err[0]), close(err[1]);
        schedule_restart();
        return;
    }
    const pid_t pid = fork();
    if (pid < 0) {
        KN_ERROR(MOD, "fork: %s", strerror(errno));
        close(in[0]), close(in[1]), close(out[0]), close(out[1]), close(err[0]), close(err[1]);
        schedule_restart();
        return;
    }
    if (pid == 0) {
#ifdef __linux__
        prctl(PR_SET_PDEATHSIG, SIGTERM); /* dies with the brain */
#endif
        setpgid(0, 0);
        dup2(in[0], 0);
        dup2(out[1], 1);
        dup2(err[1], 2);
        for (int fd = 3; fd < 1024; fd++) close(fd);
        signal(SIGPIPE, SIG_DFL);
        execve(exe, S.argv, envp);
        static const char msg[] = "ai-search: exec failed\n";
        if (write(2, msg, sizeof(msg) - 1) < 0) {}
        _exit(127);
    }
    close(in[0]);
    close(out[1]);
    close(err[1]);
    S.pid = pid;
    S.in_fd = in[1];
    S.out_fd = out[0];
    S.err_fd = err[0];
    kn_set_nonblock(S.in_fd);
    kn_set_nonblock(S.out_fd);
    kn_set_nonblock(S.err_fd);
    kn_set_cloexec(S.in_fd);
    kn_set_cloexec(S.out_fd);
    kn_set_cloexec(S.err_fd);
    S.in_watched = 0;
    kn_loop_add_fd(S.b->loop, S.out_fd, KN_EV_READ, on_read, NULL);
    kn_loop_add_fd(S.b->loop, S.err_fd, KN_EV_READ, on_read, (void *) 1);
    S.state = 1;
    S.init_id = ++S.next_id;
    cJSON *m = rpc("initialize", S.init_id);
    cJSON *p = cJSON_AddObjectToObject(m, "params");
    cJSON_AddStringToObject(p, "protocolVersion", "2025-06-18");
    cJSON_AddObjectToObject(p, "capabilities");
    cJSON *ci = cJSON_AddObjectToObject(p, "clientInfo");
    cJSON_AddStringToObject(ci, "name", "kn_brain");
    cJSON_AddStringToObject(ci, "version", kn_build_commit_id());
    send_msg(m);
    KN_INFO(MOD, "started the web search server (pid %d): %s", (int) pid, S.argv[0]);
}

/* ------------------------------------------------------------------ calls */

static void call(const char *tool_name, const char *args_json, AiToolDoneFn done, void *ud) {
    if (S.state != 3 || !S.has_search) {
        done(ud, 1, "web search is not available right now");
        return;
    }
    cJSON *in = cJSON_Parse(args_json ? args_json : "{}");
    if (!cJSON_IsObject(in)) {
        cJSON_Delete(in);
        done(ud, 1, "the arguments are not a JSON object");
        return;
    }
    cJSON *args = cJSON_CreateObject();
    const char *mcp_name;
    if (!strcmp(tool_name, "web_search")) {
        const char *q = kn_json_str(in, "query", "");
        if (!q[0]) {
            cJSON_Delete(in);
            cJSON_Delete(args);
            done(ud, 1, "query is required");
            return;
        }
        int max = (int) kn_json_int(in, "max_results", 5);
        if (max < 1) max = 1;
        if (max > 10) max = 10;
        cJSON_AddStringToObject(args, "query", q);
        cJSON_AddNumberToObject(args, "max_results", max);
        mcp_name = "search";
    } else if (!strcmp(tool_name, "web_fetch") && S.has_fetch) {
        const char *url = kn_json_str(in, "url", "");
        if (!url[0]) {
            cJSON_Delete(in);
            cJSON_Delete(args);
            done(ud, 1, "url is required");
            return;
        }
        cJSON_AddStringToObject(args, "url", url);
        const int64_t si = kn_json_int(in, "start_index", 0), ml = kn_json_int(in, "max_length", 0);
        if (si > 0) cJSON_AddNumberToObject(args, "start_index", (double) si);
        if (ml >= 500 && ml <= 20000) cJSON_AddNumberToObject(args, "max_length", (double) ml);
        mcp_name = "fetch_content";
    } else {
        cJSON_Delete(in);
        cJSON_Delete(args);
        done(ud, 1, "unknown web tool");
        return;
    }
    cJSON_Delete(in);
    Pending *p = pending_new(done, ud);
    if (!p) {
        cJSON_Delete(args);
        done(ud, 1, "too many web searches at once");
        return;
    }
    cJSON *m = rpc("tools/call", p->id);
    cJSON *params = cJSON_AddObjectToObject(m, "params");
    cJSON_AddStringToObject(params, "name", mcp_name);
    cJSON_AddItemToObject(params, "arguments", args);
    p->timer = kn_loop_add_timer(S.b->loop, (uint64_t) S.timeout_ms, 0, pending_timeout, p);
    send_msg(m);
}

static const AiServerTools k_tools = {"web", defs, owns, call};

int ai_search_init(Brain *b) {
    S.b = b;
    const char *cmd = kn_config_str(b->cfg, "ai.search.command", "");
    snprintf(S.cmd, sizeof(S.cmd), "%s", cmd ? cmd : "");
    S.timeout_ms = (int) kn_config_int(b->cfg, "ai.search.timeout_ms", 45000);
    S.nextra = kn_config_str_list(b->cfg, "ai.search.env", S.extra_env, 16);
    if (S.nextra > 16) S.nextra = 16;
    if (!S.cmd[0]) {
        KN_INFO(MOD, "no ai.search.command: the editor's AI has no web search");
        return 0;
    }
    split_command();
    if (!S.argv[0]) return 0;
    ai_register_server_tools(&k_tools);
    signal(SIGPIPE, SIG_IGN);
    child_start(b->loop, NULL);
    return 0;
}

void ai_search_shutdown(Brain *b) {
    (void) b;
    S.stopping = 1;
    if (S.restart_timer) kn_loop_cancel_timer(S.b->loop, S.restart_timer);
    S.restart_timer = 0;
    if (S.pid > 0 || S.out_fd >= 0) child_stop("the server is shutting down");
}
