// Quick Play retry (quick_play_retry.h): the timer and the run state of a Quick Play search that is
// answered "full". The seeks, joins and their answers stay in servers_ui.c and krunker_net.c; they
// call in here to schedule the next try, to mark a search out or answered, and to tell a stale answer.
#include <quick_play_retry.h>
#include "klog.h"

#include <GLFW/glfw3.h>
#include <stdio.h>
#include <ui_logic.h>

// the backoff between two tries of one search: 1 s, then 2, 3, 4 and 5 s (the cap, every later try)
#define QP_RETRY_FIRST_S 1.0
#define QP_RETRY_CAP_S 5.0

static struct {
    int running;              // a search is out (busy) or a retry waits (pending)
    int busy;                 // a seek or a join of the search is in flight
    int pending;              // a retry is due at `at`
    int tries;                // retries scheduled in this search (the backoff's step)
    double at;                // glfwGetTime() of the waiting retry
    QuickPlayRetryFn fn;      // the waiting retry
    void *user;
    unsigned gen;             // bumped by every begin and cancel: answers of an older search are stale
    QuickPlayRetryFn join_fn; // armed by krunker_lobbies for the join that is about to start
    void *join_user;
} Q;

static double backoff(const int tries) {
    const double s = QP_RETRY_FIRST_S + tries;
    return s < QP_RETRY_CAP_S ? s : QP_RETRY_CAP_S;
}

void quick_play_retry_begin(void) {
    Q.gen++;
    Q.running = 1;
    Q.busy = 1;
    Q.pending = 0;
    Q.tries = 0;
    Q.fn = NULL;
    Q.user = NULL;
}

int quick_play_retry_running(void) { return Q.running && (Q.busy || Q.pending); }

void quick_play_retry_out(void) {
    Q.running = 1;
    Q.busy = 1;
}

void quick_play_retry_answered(void) { Q.busy = 0; }

int quick_play_retry_schedule(QuickPlayRetryFn fn, void *user) {
    if (!fn) return 0;
    Q.running = 1;
    Q.busy = 0;
    Q.pending = 1;
    Q.fn = fn;
    Q.user = user;
    Q.at = glfwGetTime() + backoff(Q.tries);
    KN_LOG_VERBOSE2("[mm] quick play: the lobby is full: trying again in %.0f s\n", backoff(Q.tries));
    Q.tries++;
    return 1;
}

void quick_play_retry_tick(void) {
    if (!Q.pending || glfwGetTime() < Q.at) return;
    QuickPlayRetryFn fn = Q.fn;
    void *user = Q.user;
    Q.pending = 0;
    Q.fn = NULL;
    Q.user = NULL;
    Q.busy = 1; // the retry's own seek is out
    KN_LOG_DEBUG1("[mm] quick play: trying again\n");
    fn(user);
}

void quick_play_retry_cancel(void) {
    if (quick_play_retry_running()) {
        KN_LOG_VERBOSE2("[mm] quick play: search stopped\n");
        menu_connect_stopped(); // the search's spinner goes with it
    }
    Q.gen++;
    Q.running = 0;
    Q.busy = 0;
    Q.pending = 0;
    Q.fn = NULL;
    Q.user = NULL;
    Q.tries = 0;
}

void quick_play_retry_end(void) {
    Q.running = 0;
    Q.busy = 0;
    Q.pending = 0;
    Q.fn = NULL;
    Q.user = NULL;
    Q.tries = 0;
}

unsigned quick_play_retry_generation(void) { return Q.gen; }

int quick_play_retry_stale(const unsigned gen) { return gen != Q.gen; }

void quick_play_retry_arm(QuickPlayRetryFn fn, void *user) {
    Q.join_fn = fn;
    Q.join_user = user;
}

int quick_play_retry_take(QuickPlayRetryFn *fn, void **user) {
    if (!Q.join_fn) return 0;
    if (fn) *fn = Q.join_fn;
    if (user) *user = Q.join_user;
    Q.join_fn = NULL;
    Q.join_user = NULL;
    return 1;
}

void quick_play_retry_disarm(void) {
    Q.join_fn = NULL;
    Q.join_user = NULL;
}
