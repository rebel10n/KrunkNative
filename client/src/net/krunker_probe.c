// The input-bank probe: see client/include/krunker_probe.h and doc/v10/input-bank-probe.md.
#include "krunker_probe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__ // the browser's socket cannot hold its frames: no probe on the web build
int krunker_probe_active(void) { return 0; }
void krunker_probe_on_sync(const int32_t ack, const float position[3]) { (void) ack; (void) position; }
int krunker_probe_hold(void) { return 0; }
void krunker_probe_released(const int frames) { (void) frames; }
int krunker_probe_move_dir(void) { return -2; }
void krunker_probe_input_failed(const int code) { (void) code; }
#else

#include <pthread.h>
#include <time.h>

enum { STEP_STAND, STEP_CONTROL, STEP_HELD };
enum { MOVE_FORWARD = 0, MOVE_BACK = 4 };
#define PROBE_STEPS_MAX 160
#define PROBE_LENGTHS_MAX 32
#define PROBE_WARMUP_MS 3000.0
#define PROBE_SETTLE_MS 1500.0 // standing after a walk: the burst's positions arrive and the player stops
#define PROBE_REFILL_MS 5000.0 // standing before a held walk: the bank is full again whatever its rate

typedef struct {
    int kind, dir;
    double ms;
    char label[24];
} ProbeStep;

static struct {
    int configured; // 0 not read yet, 1 on, -1 off
    pthread_mutex_t lock;
    FILE *log;
    ProbeStep steps[PROBE_STEPS_MAX];
    int step_count;
    double armed_at; // monotonic ms of the first server position, 0 = not armed
    int logged_step; // the last step written to the log, -1 none
    int done;
    int holding;
} P = {.lock = PTHREAD_MUTEX_INITIALIZER, .logged_step = -1};

static double mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec * 1000.0 + (double) ts.tv_nsec / 1e6;
}

static int parse_list(const char *s, double *out, const int max) {
    int n = 0;
    while (s && *s && n < max) {
        char *end = NULL;
        const double v = strtod(s, &end);
        if (end == s) break;
        if (v > 0.0 && v <= 10000.0) out[n++] = v;
        s = *end == ',' ? end + 1 : end;
        if (*end != ',') break;
    }
    return n;
}

static void add_step(const int kind, const int dir, const double ms, const char *label) {
    if (P.step_count >= PROBE_STEPS_MAX) return;
    ProbeStep *s = &P.steps[P.step_count++];
    s->kind = kind;
    s->dir = dir;
    s->ms = ms;
    snprintf(s->label, sizeof(s->label), "%s", label);
}

// The schedule (header): warmup; per length L a control walk forward and, after a full rest, a held walk back;
// per rest g two held walks of the longest length with g between them. Directions alternate so the player stays
// near where it started.
static void configure(void) {
    const char *lengths_env = getenv("KRUNKNATIVE_INPUT_BANK_PROBE");
    double lengths[PROBE_LENGTHS_MAX], gaps[PROBE_LENGTHS_MAX];
    const int nl = lengths_env ? parse_list(lengths_env, lengths, PROBE_LENGTHS_MAX) : 0;
    if (nl <= 0) {
        P.configured = -1;
        return;
    }
    const int ng = parse_list(getenv("KRUNKNATIVE_INPUT_BANK_PROBE_GAPS"), gaps, PROBE_LENGTHS_MAX);
    const char *path = getenv("KRUNKNATIVE_INPUT_BANK_PROBE_LOG");
    P.log = fopen(path && *path ? path : "input_bank_probe.tsv", "w");
    if (!P.log) {
        fprintf(stderr, "[probe] cannot write the input-bank probe log; probe off\n");
        P.configured = -1;
        return;
    }
    double longest = 0.0;
    char label[24];
    add_step(STEP_STAND, -1, PROBE_WARMUP_MS, "warmup");
    for (int i = 0; i < nl; i++) {
        if (lengths[i] > longest) longest = lengths[i];
        snprintf(label, sizeof(label), "control-%d", (int) lengths[i]);
        add_step(STEP_CONTROL, MOVE_FORWARD, lengths[i], label);
        add_step(STEP_STAND, -1, PROBE_REFILL_MS, "rest");
        snprintf(label, sizeof(label), "held-%d", (int) lengths[i]);
        add_step(STEP_HELD, MOVE_BACK, lengths[i], label);
        add_step(STEP_STAND, -1, PROBE_SETTLE_MS, "settle");
    }
    for (int i = 0; i < ng; i++) {
        add_step(STEP_STAND, -1, PROBE_REFILL_MS, "rest");
        snprintf(label, sizeof(label), "pairA-%d", (int) gaps[i]);
        add_step(STEP_HELD, MOVE_FORWARD, longest, label);
        snprintf(label, sizeof(label), "gap-%d", (int) gaps[i]);
        add_step(STEP_STAND, -1, gaps[i], label);
        snprintf(label, sizeof(label), "pairB-%d", (int) gaps[i]);
        add_step(STEP_HELD, MOVE_BACK, longest, label);
        add_step(STEP_STAND, -1, PROBE_SETTLE_MS, "settle");
    }
    add_step(STEP_STAND, -1, 2000.0, "end");
    fprintf(P.log, "# Krunker Box Office input-bank probe (doc/v10/input-bank-probe.md); lengths %s; gaps %s\n", lengths_env,
            getenv("KRUNKNATIVE_INPUT_BANK_PROBE_GAPS") ? getenv("KRUNKNATIVE_INPUT_BANK_PROBE_GAPS") : "-");
    fprintf(P.log, "t_ms\tevent\tstep\tlabel\ta\tb\tc\td\n");
    fflush(P.log);
    P.configured = 1;
}

int krunker_probe_active(void) {
    pthread_mutex_lock(&P.lock);
    if (!P.configured) configure();
    const int on = P.configured > 0 && !P.done;
    pthread_mutex_unlock(&P.lock);
    return on;
}

// The step running now (P.lock held); -1 before arming, step_count when the schedule is over. Writes each step's
// start to the log once, and "done" at the end.
static int current_step(const double now, double *elapsed_in_step) {
    if (P.configured <= 0 || P.armed_at <= 0.0) return -1;
    double t = now - P.armed_at;
    int i = 0;
    while (i < P.step_count && t >= P.steps[i].ms) t -= P.steps[i++].ms;
    if (elapsed_in_step) *elapsed_in_step = t;
    for (int k = P.logged_step + 1; k <= i && k < P.step_count; k++) {
        const ProbeStep *s = &P.steps[k];
        fprintf(P.log, "%.1f\tstep\t%d\t%s\t%s\t%d\t%.0f\t\n", now - P.armed_at, k, s->label,
                s->kind == STEP_HELD ? "held" : s->kind == STEP_CONTROL ? "control" : "stand", s->dir, s->ms);
        P.logged_step = k;
    }
    if (i >= P.step_count && !P.done) {
        fprintf(P.log, "%.1f\tdone\t\t\t\t\t\t\n", now - P.armed_at);
        fflush(P.log);
        fprintf(stderr, "[probe] input-bank probe done\n");
        P.done = 1;
    }
    return i;
}

void krunker_probe_on_sync(const int32_t ack, const float position[3]) {
    if (!krunker_probe_active()) return;
    pthread_mutex_lock(&P.lock);
    const double now = mono_ms();
    if (P.armed_at <= 0.0) {
        P.armed_at = now;
        fprintf(stderr, "[probe] input-bank probe armed (%d steps)\n", P.step_count);
    }
    const int step = current_step(now, NULL);
    if (P.log && !P.done)
        fprintf(P.log, "%.1f\tsync\t%d\t\t%d\t%.4f\t%.4f\t%.4f\n", now - P.armed_at, step, (int) ack, position[0], position[1], position[2]);
    pthread_mutex_unlock(&P.lock);
}

int krunker_probe_hold(void) {
    if (!krunker_probe_active()) return 0;
    pthread_mutex_lock(&P.lock);
    const double now = mono_ms();
    const int step = current_step(now, NULL);
    const int hold = step >= 0 && step < P.step_count && P.steps[step].kind == STEP_HELD;
    if (hold && !P.holding && P.log) fprintf(P.log, "%.1f\thold\t%d\t%s\t\t\t\t\n", now - P.armed_at, step, P.steps[step].label);
    P.holding = hold;
    pthread_mutex_unlock(&P.lock);
    return hold;
}

void krunker_probe_released(const int frames) {
    pthread_mutex_lock(&P.lock);
    if (P.log && P.armed_at > 0.0) {
        fprintf(P.log, "%.1f\trelease\t%d\t\t%d\t\t\t\n", mono_ms() - P.armed_at, P.logged_step, frames);
        fflush(P.log);
    }
    pthread_mutex_unlock(&P.lock);
}

int krunker_probe_move_dir(void) {
    if (!krunker_probe_active()) return -2;
    pthread_mutex_lock(&P.lock);
    const int step = current_step(mono_ms(), NULL);
    const int dir = step < 0 || step >= P.step_count ? -2 : P.steps[step].kind == STEP_STAND ? -1 : P.steps[step].dir;
    pthread_mutex_unlock(&P.lock);
    return dir;
}

void krunker_probe_input_failed(const int code) {
    if (!krunker_probe_active()) return;
    pthread_mutex_lock(&P.lock);
    if (P.log && P.armed_at > 0.0) {
        fprintf(P.log, "%.1f\tfail\t%d\t\t%d\t\t\t\n", mono_ms() - P.armed_at, P.logged_step, code);
        fflush(P.log);
    }
    pthread_mutex_unlock(&P.lock);
}
#endif
