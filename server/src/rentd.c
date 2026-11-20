// kn_rentd: the rented servers' supervisor on a game host (doc/v10/rented-servers.md, server/include/rentd.h).
//
//   kn_rentd --cfg /etc/krunknative/rentd.cfg [--once]
//
// Every poll_s it asks kn_brain's agent API which slots of this host are rented (GET /mm/internal/rentals?host=,
// Bearer coordinator_secret) and reconciles: a rented slot runs one server_bin --headless --no-cli --cfg
// <state_dir>/rent-<id>.cfg (written from the slot), a changed generation restarts it, a slot no longer rented is
// stopped (SIGTERM, SIGKILL after stop_grace_s), and a crash restarts it after a backoff (1 s doubling to 60 s).
// Each child gets RLIMIT_AS child_mem_mb, no core files and nice 5; the systemd unit caps the whole group
// (MemoryMax, CPUQuota). After every pass it posts each slot's state (POST /mm/internal/rentals/status). With the
// brain unreachable the children keep running as they are. SIGTERM / SIGINT stop every child and exit.
#include <http_client.h>
#include <rentd.h>

#include <cJSON.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define RUNNING_AFTER_S 5.0 // a child alive this long counts as running (and resets its crash count)
#define STATUS_REPEAT_S 60.0 // an unchanged status is posted again after this long

typedef struct {
    int id;
    pid_t pid;
    int gen;           // the generation the child runs
    double started;    // when it started
    double kill_at;    // SIGTERM sent: SIGKILL at this time (0 = not stopping)
    int crashes;       // consecutive early exits
    double not_before; // the backoff
    int failed;        // the last start failed (exec / cfg)
    int seen;          // in the brain's last answer
} Child;

static struct {
    RentdShared sh;
    char brain_url[256];
    char server_bin[512];
    char workdir[512];
    char state_dir[512];
    int poll_s, stop_grace_s, child_mem_mb;
    Child kids[RENTD_MAX_SLOTS];
    int nkids;
} g;

static volatile sig_atomic_t g_stop;

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec + ts.tv_nsec / 1e9;
}

static void on_signal(int sig) {
    (void) sig;
    g_stop = 1;
}

static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[rentd] ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

// ------------------------------------------------------------------ rentd.cfg (key value lines, "quoted" strings)

static void set_str(char *dst, size_t size, const char *v) { snprintf(dst, size, "%s", v); }

static int load_cfg(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        logf_("cannot read %s: %s", path, strerror(errno));
        return -1;
    }
    g.poll_s = 10;
    g.stop_grace_s = 40;
    g.child_mem_mb = 1024;
    g.sh.max_players = 8;
    set_str(g.sh.region, sizeof(g.sh.region), "VA");
    set_str(g.sh.listen_host, sizeof(g.sh.listen_host), "0.0.0.0");
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '\n' || *p == '#' || (p[0] == '/' && p[1] == '/')) continue;
        char *key = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
        if (*p) *p++ = 0;
        while (*p == ' ' || *p == '\t') p++;
        char *val = p;
        size_t len = strcspn(val, "\r\n");
        val[len] = 0;
        if (len >= 2 && val[0] == '"' && val[len - 1] == '"') {
            val[len - 1] = 0;
            val++;
        }
#define S(k, dst) else if (!strcmp(key, k)) set_str(dst, sizeof(dst), val)
#define I(k, dst) else if (!strcmp(key, k)) dst = atoi(val)
        if (0) {}
        S("host_id", g.sh.host_id);
        S("brain_url", g.brain_url);
        S("server_bin", g.server_bin);
        S("workdir", g.workdir);
        S("state_dir", g.state_dir);
        S("region", g.sh.region);
        S("listen_host", g.sh.listen_host);
        S("public_host", g.sh.public_host);
        S("coordinator_url", g.sh.coordinator_url);
        S("coordinator_secret", g.sh.coordinator_secret);
        S("join_secret", g.sh.join_secret);
        S("auth_url", g.sh.auth_url);
        S("brain_http_url", g.sh.brain_http_url);
        I("max_players", g.sh.max_players);
        I("poll_s", g.poll_s);
        I("stop_grace_s", g.stop_grace_s);
        I("child_mem_mb", g.child_mem_mb);
        else logf_("%s: unknown key %s", path, key);
#undef S
#undef I
    }
    fclose(f);
    if (!g.sh.host_id[0] || !g.brain_url[0] || !g.server_bin[0] || !g.state_dir[0]) {
        logf_("%s needs host_id, brain_url, server_bin and state_dir", path);
        return -1;
    }
    if (!g.sh.brain_http_url[0]) set_str(g.sh.brain_http_url, sizeof(g.sh.brain_http_url), g.brain_url);
    if (g.poll_s < 1) g.poll_s = 1;
    if (g.stop_grace_s < 1) g.stop_grace_s = 1;
    return 0;
}

// ------------------------------------------------------------------ children

static Child *child_of(int id, int create) {
    for (int i = 0; i < g.nkids; i++)
        if (g.kids[i].id == id) return &g.kids[i];
    if (!create || g.nkids >= RENTD_MAX_SLOTS) return NULL;
    Child *c = &g.kids[g.nkids++];
    memset(c, 0, sizeof(*c));
    c->id = id;
    return c;
}

static int write_file(const char *path, const char *text) {
    char tmp[600];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    const int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0640); // the secrets are in it
    if (fd < 0) return -1;
    const size_t len = strlen(text);
    const int ok = write(fd, text, len) == (ssize_t) len;
    close(fd);
    return ok && rename(tmp, path) == 0 ? 0 : -1;
}

static void start_child(Child *c, const RentdSlot *s, double now) {
    char cfg_path[600], log_path[600], cfg[8192];
    snprintf(cfg_path, sizeof(cfg_path), "%s/rent-%d.cfg", g.state_dir, s->id);
    snprintf(log_path, sizeof(log_path), "%s/rent-%d.log", g.state_dir, s->id);
    c->failed = 0;
    if (rentd_cfg_text(s, &g.sh, g.state_dir, cfg, sizeof(cfg)) < 0 || write_file(cfg_path, cfg) < 0) {
        logf_("slot %d: no server.cfg (game \"%s\"): not started", s->id, s->map);
        c->failed = 1;
        c->not_before = now + 60.0;
        return;
    }
    const pid_t pid = fork();
    if (pid < 0) {
        logf_("slot %d: fork failed: %s", s->id, strerror(errno));
        c->failed = 1;
        c->not_before = now + rentd_backoff(++c->crashes);
        return;
    }
    if (pid == 0) {
        setpgid(0, 0);
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        const int fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0640);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            close(fd);
        }
        const int nul = open("/dev/null", O_RDONLY);
        if (nul >= 0) dup2(nul, STDIN_FILENO);
        const struct rlimit mem = {(rlim_t) g.child_mem_mb << 20, (rlim_t) g.child_mem_mb << 20}, core = {0, 0};
        if (g.child_mem_mb > 0) setrlimit(RLIMIT_AS, &mem);
        setrlimit(RLIMIT_CORE, &core);
        (void) setpriority(PRIO_PROCESS, 0, 5); // below the public servers' share
        if (g.workdir[0] && chdir(g.workdir) != 0) _exit(126);
        execl(g.server_bin, g.server_bin, "--headless", "--no-cli", "--cfg", cfg_path, (char *) NULL);
        _exit(127);
    }
    c->pid = pid;
    c->gen = s->generation;
    c->started = now;
    c->kill_at = 0;
    logf_("slot %d (%s): started pid %d, generation %d, map %s", s->id, s->name, (int) pid, s->generation,
          s->key[0] ? s->key : s->map);
}

static void stop_child(Child *c, double now) {
    if (c->pid <= 0 || c->kill_at > 0) return;
    kill(-c->pid, SIGTERM);
    kill(c->pid, SIGTERM);
    c->kill_at = now + g.stop_grace_s;
    logf_("slot %d: stopping pid %d", c->id, (int) c->pid);
}

// waitpid every child; a SIGTERM past its grace gets SIGKILL
static void reap(double now) {
    for (int i = 0; i < g.nkids; i++) {
        Child *c = &g.kids[i];
        if (c->pid <= 0) continue;
        int st = 0;
        const pid_t r = waitpid(c->pid, &st, WNOHANG);
        if (r == c->pid) {
            const int stopping = c->kill_at > 0;
            const int early = now - c->started < RUNNING_AFTER_S * 6;
            logf_("slot %d: pid %d exited (%s %d)%s", c->id, (int) c->pid, WIFSIGNALED(st) ? "signal" : "status",
                  WIFSIGNALED(st) ? WTERMSIG(st) : WEXITSTATUS(st), stopping ? "" : ": restarting");
            c->pid = 0;
            c->kill_at = 0;
            if (!stopping) {
                c->crashes = early ? c->crashes + 1 : 1;
                c->not_before = now + rentd_backoff(c->crashes);
            }
        } else if (c->kill_at > 0 && now >= c->kill_at) {
            kill(-c->pid, SIGKILL);
            kill(c->pid, SIGKILL);
            c->kill_at = now + 5;
        }
    }
}

static const char *state_of(const Child *c, const RentdSlot *s, double now) {
    if (c && c->pid > 0 && c->kill_at == 0 && s && s->run && c->gen == s->generation)
        return now - c->started >= RUNNING_AFTER_S ? "running" : "starting";
    if (s && s->run) return c && (c->failed || c->crashes >= 3) ? "failed" : "starting";
    return "stopped";
}

// ------------------------------------------------------------------ the pass

static void auth_header(char *out, size_t size) { snprintf(out, size, "Authorization: Bearer %s", g.sh.coordinator_secret); }

static void post_status(const RentdSlot *slots, int n, double now) {
    cJSON *body = cJSON_CreateObject(), *arr = cJSON_AddArrayToObject(body, "slots");
    cJSON_AddStringToObject(body, "host", g.sh.host_id);
    for (int i = 0; i < n; i++) {
        const Child *c = child_of(slots[i].id, 0);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "id", slots[i].id);
        cJSON_AddNumberToObject(o, "generation", c && c->pid > 0 ? c->gen : slots[i].generation);
        cJSON_AddStringToObject(o, "state", state_of(c, &slots[i], now));
        cJSON_AddItemToArray(arr, o);
    }
    char *text = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    // only a change, or once a minute: the poll is the heartbeat (agent_seen_s), and every call to the internal API
    // counts against the brain's per-address limit (30 a minute, brain_util.c mm_internal_authorized)
    static char *last;
    static double last_at;
    if (text && last && !strcmp(text, last) && now - last_at < STATUS_REPEAT_S) {
        free(text);
        return;
    }
    char url[512], auth[256];
    snprintf(url, sizeof(url), "%s/mm/internal/rentals/status", g.brain_url);
    auth_header(auth, sizeof(auth));
    HttpResult r = {0};
    if (text && http_request_hdr("POST", url, "application/json", auth, text, 10000, 65536, &r) != 0)
        logf_("status post failed: %s", r.error);
    else if (text && r.status != 200)
        logf_("status post: HTTP %d", r.status);
    const int ok = text && r.status == 200;
    if (text) http_result_free(&r);
    free(last);
    last = ok ? text : NULL; // a failed post is sent again on the next pass
    last_at = now;
    if (!ok) free(text);
}

static void pass(void) {
    const double now = now_s();
    reap(now);
    char url[512], auth[256], host[160];
    http_url_encode(host, sizeof(host), g.sh.host_id);
    snprintf(url, sizeof(url), "%s/mm/internal/rentals?host=%s", g.brain_url, host);
    auth_header(auth, sizeof(auth));
    HttpResult r;
    static RentdSlot slots[RENTD_MAX_SLOTS];
    int n = -1;
    if (http_request_hdr("GET", url, NULL, auth, NULL, 10000, 1 << 20, &r) == 0 && r.status == 200) n = rentd_parse_slots(r.body, slots, RENTD_MAX_SLOTS);
    else logf_("poll failed: %s", r.status ? "HTTP error" : r.error);
    http_result_free(&r);
    if (n < 0) return; // the brain is unreachable: keep everything as it is
    for (int i = 0; i < g.nkids; i++) g.kids[i].seen = 0;
    for (int i = 0; i < n; i++) {
        Child *c = child_of(slots[i].id, slots[i].run);
        if (!c) continue;
        c->seen = 1;
        if (c->pid > 0 && c->kill_at > 0) continue; // still stopping: decided again once it exited
        switch (rentd_decide(&slots[i], c->pid > 0 && c->kill_at == 0, c->gen, now, c->not_before)) {
            case RENTD_START:
                start_child(c, &slots[i], now);
                break;
            case RENTD_RESTART:
                logf_("slot %d: generation %d -> %d: restart", c->id, c->gen, slots[i].generation);
                c->crashes = 0;
                c->not_before = 0;
                stop_child(c, now);
                break;
            case RENTD_STOP:
                stop_child(c, now);
                break;
            case RENTD_KEEP:
                if (c->pid > 0 && now - c->started >= RUNNING_AFTER_S * 6) c->crashes = 0;
                break;
        }
    }
    for (int i = 0; i < g.nkids; i++)
        if (!g.kids[i].seen) stop_child(&g.kids[i], now); // the slot left this host
    post_status(slots, n, now);
}

static void stop_all(void) {
    const double now = now_s();
    for (int i = 0; i < g.nkids; i++) stop_child(&g.kids[i], now);
    for (;;) {
        int alive = 0;
        reap(now_s());
        for (int i = 0; i < g.nkids; i++) alive |= g.kids[i].pid > 0;
        if (!alive) break;
        usleep(200 * 1000);
    }
}

int main(int argc, char **argv) {
    const char *cfg = "/etc/krunknative/rentd.cfg";
    int once = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--cfg") && i + 1 < argc) cfg = argv[++i];
        else if (!strcmp(argv[i], "--once")) once = 1;
        else {
            fprintf(stderr, "usage: %s [--cfg rentd.cfg] [--once]\n", argv[0]);
            return 2;
        }
    }
    if (load_cfg(cfg) < 0) return 1;
    mkdir(g.state_dir, 0750);
    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    logf_("host %s, brain %s, poll %d s", g.sh.host_id, g.brain_url, g.poll_s);
    double next = 0;
    while (!g_stop) {
        const double now = now_s();
        if (now >= next) {
            pass();
            next = now + g.poll_s;
            if (once) break;
        } else {
            reap(now);
        }
        usleep(250 * 1000);
    }
    stop_all();
    logf_("stopped");
    return 0;
}
