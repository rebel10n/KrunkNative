/* Event loop: timers, cancellation, posts (incl. cross-thread), fds, stop; epoll and poll backends. */
#include "kn_test.h"
#include "knsvc/loop.h"

#include <pthread.h>
#include <unistd.h>

static int order[16], norder;
static kn_timer_id self_cancel_id;
static int repeat_count;

static void t_record(kn_loop *l, void *ud) { order[norder++] = (int)(intptr_t)ud; }
static void t_never(kn_loop *l, void *ud) { CHECK(0); }

static void t_repeat(kn_loop *l, void *ud) {
    if (++repeat_count == 3) kn_loop_cancel_timer(l, self_cancel_id);
}

static void t_stop(kn_loop *l, void *ud) { kn_loop_stop(l); }

static int posted;
static void p_task(kn_loop *l, void *ud) {
    posted += (int)(intptr_t)ud;
    if ((intptr_t)ud == 1) kn_loop_post(l, p_task, (void *)(intptr_t)10); /* runs next iteration */
}

static void *thread_poster(void *arg) {
    kn_loop *l = arg;
    for (int i = 0; i < 100; i++) kn_loop_post_threadsafe(l, p_task, (void *)(intptr_t)100);
    usleep(50000);
    kn_loop_stop(l);
    return NULL;
}

static int fd_reads;
static void on_pipe(kn_loop *l, int fd, unsigned ev, void *ud) {
    char buf[16];
    CHECK(ev & KN_EV_READ);
    ssize_t n = read(fd, buf, sizeof buf);
    if (n > 0) fd_reads += (int)n;
    if (fd_reads >= 3) {
        kn_loop_del_fd(l, fd);
        kn_loop_stop(l);
    }
}

static void run_suite(void) {
    kn_loop *l = kn_loop_new();
    CHECK(l != NULL);
    printf("backend: %s\n", kn_loop_backend(l));
    norder = 0;
    repeat_count = 0;
    posted = 0;
    fd_reads = 0;

    /* timers fire in deadline order; equal deadlines in creation order */
    kn_loop_add_timer(l, 30, 0, t_record, (void *)3);
    kn_loop_add_timer(l, 10, 0, t_record, (void *)1);
    kn_loop_add_timer(l, 10, 0, t_record, (void *)2);
    kn_timer_id gone = kn_loop_add_timer(l, 20, 0, t_never, NULL);
    kn_loop_cancel_timer(l, gone);
    self_cancel_id = kn_loop_add_timer(l, 5, 5, t_repeat, NULL);
    kn_loop_add_timer(l, 80, 0, t_stop, NULL);
    uint64_t t0 = kn_now_ms();
    CHECK(kn_loop_run(l) == 0);
    uint64_t dt = kn_now_ms() - t0;
    CHECK(dt >= 75 && dt < 500);
    CHECK_EQ_INT(norder, 3);
    CHECK(order[0] == 1 && order[1] == 2 && order[2] == 3);
    CHECK_EQ_INT(repeat_count, 3);

    /* posts: local + from another thread, plus stop from that thread */
    kn_loop_post(l, p_task, (void *)1);
    pthread_t th;
    pthread_create(&th, NULL, thread_poster, l);
    kn_loop_run(l);
    pthread_join(th, NULL);
    CHECK_EQ_INT(posted, 1 + 10 + 100 * 100);

    /* fd readiness */
    int p[2];
    CHECK(pipe(p) == 0);
    kn_set_nonblock(p[0]);
    CHECK(kn_loop_add_fd(l, p[0], KN_EV_READ, on_pipe, NULL) == 0);
    CHECK(kn_loop_add_fd(l, p[0], KN_EV_READ, on_pipe, NULL) != 0); /* one watcher per fd */
    CHECK(write(p[1], "abc", 3) == 3);
    kn_timer_id safety = kn_loop_add_timer(l, 2000, 0, t_stop, NULL);
    kn_loop_run(l);
    kn_loop_cancel_timer(l, safety);
    CHECK_EQ_INT(fd_reads, 3);
    close(p[0]);
    close(p[1]);

    /* run_once honours its timeout (first drain the pending stop wakeup) */
    kn_loop_run_once(l, 0);
    t0 = kn_now_ms();
    kn_loop_run_once(l, 30);
    dt = kn_now_ms() - t0;
    if (!(dt >= 25 && dt < 300)) printf("run_once dt=%llu\n", (unsigned long long)dt);
    CHECK(dt >= 25 && dt < 300);
    kn_loop_free(l);
}

int main(void) {
    run_suite();
    setenv("KN_LOOP_BACKEND", "poll", 1);
    run_suite();
    TEST_DONE("knsvc_loop");
}
