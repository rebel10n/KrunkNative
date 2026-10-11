/*
 * knsvc/loop.h implementation.
 *
 * Fds live in a table indexed by fd with a generation counter: epoll events
 * carry (fd, generation) so an fd closed and reused inside the same dispatch
 * batch is never handed to the wrong watcher. Timers are a binary min-heap.
 * Posted tasks: a local queue plus a mutex-protected queue for other threads,
 * woken through an eventfd (Linux) or a pipe.
 */
#include "knsvc/loop.h"
#include "knsvc/log.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/epoll.h>
#include <sys/eventfd.h>
#define KN_HAVE_EPOLL 1
#endif

typedef struct fd_watch {
    kn_io_cb cb;
    void *ud;
    unsigned events;
    uint32_t gen;
    int active;
} fd_watch;

typedef struct timer {
    uint64_t when;
    uint64_t repeat;
    kn_timer_id id;
    kn_timer_cb cb;
    void *ud;
} timer;

typedef struct task {
    kn_task_cb cb;
    void *ud;
    struct task *next;
} task;

typedef struct task_queue {
    task *head, *tail;
} task_queue;

#define KN_MAX_SIGNAL 65

struct kn_loop {
    int use_epoll;
    int epfd;
    fd_watch *fds;
    int fd_cap;
    int fd_count;
    struct pollfd *pfds;     /* poll backend scratch */
    int pfd_cap;

    timer *timers;           /* heap */
    size_t ntimers, timer_cap;
    kn_timer_id next_timer_id;

    task_queue local;
    task_queue remote;
    pthread_mutex_t remote_lock;
    int wake_rd, wake_wr;    /* eventfd: rd == wr */

    volatile sig_atomic_t stop;
    struct {
        kn_signal_cb cb;
        void *ud;
    } signals[KN_MAX_SIGNAL];
};

/* ------------------------------------------------------------------ time / fd utils */

uint64_t kn_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

int64_t kn_wall_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int kn_set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    return (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) ? -1 : 0;
}

int kn_set_cloexec(int fd) {
    int fl = fcntl(fd, F_GETFD, 0);
    return (fl < 0 || fcntl(fd, F_SETFD, fl | FD_CLOEXEC) < 0) ? -1 : 0;
}

/* ------------------------------------------------------------------ signals (self-pipe) */

static int g_sig_pipe[2] = {-1, -1};
static kn_loop *g_sig_loop;

static void sig_handler(int signo) {
    int saved = errno;
    unsigned char b = (unsigned char)signo;
    if (g_sig_pipe[1] >= 0) {
        ssize_t r = write(g_sig_pipe[1], &b, 1);
        (void)r;
    }
    errno = saved;
}

static void sig_pipe_cb(kn_loop *loop, int fd, unsigned ev, void *ud) {
    unsigned char buf[64];
    ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0) {
        for (ssize_t i = 0; i < n; i++) {
            int s = buf[i];
            if (s < KN_MAX_SIGNAL && loop->signals[s].cb) loop->signals[s].cb(loop, s, loop->signals[s].ud);
        }
    }
}

int kn_loop_on_signal(kn_loop *loop, int signo, kn_signal_cb cb, void *ud) {
    if (signo <= 0 || signo >= KN_MAX_SIGNAL) return -1;
    if (g_sig_loop && g_sig_loop != loop) return -1; /* one loop owns process signals */
    if (g_sig_pipe[0] < 0) {
        if (pipe(g_sig_pipe) != 0) return -1;
        for (int i = 0; i < 2; i++) {
            kn_set_nonblock(g_sig_pipe[i]);
            kn_set_cloexec(g_sig_pipe[i]);
        }
    }
    if (!g_sig_loop) {
        if (kn_loop_add_fd(loop, g_sig_pipe[0], KN_EV_READ, sig_pipe_cb, NULL) != 0) return -1;
        g_sig_loop = loop;
    }
    loop->signals[signo].cb = cb;
    loop->signals[signo].ud = ud;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = sig_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    return sigaction(signo, &sa, NULL);
}

static void stop_signal_cb(kn_loop *loop, int signo, void *ud) {
    KN_INFO("loop", "signal %d: stopping", signo);
    kn_loop_stop(loop);
}

int kn_loop_stop_on_signals(kn_loop *loop) {
    signal(SIGPIPE, SIG_IGN);
    if (kn_loop_on_signal(loop, SIGINT, stop_signal_cb, NULL) != 0) return -1;
    return kn_loop_on_signal(loop, SIGTERM, stop_signal_cb, NULL);
}

/* ------------------------------------------------------------------ wakeup */

static void wake_cb(kn_loop *loop, int fd, unsigned ev, void *ud) {
    char buf[64];
    while (read(fd, buf, sizeof buf) > 0) {
    }
}

static int wake_init(kn_loop *loop) {
#ifdef KN_HAVE_EPOLL
    int efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (efd >= 0) {
        loop->wake_rd = loop->wake_wr = efd;
        return kn_loop_add_fd(loop, efd, KN_EV_READ, wake_cb, NULL);
    }
#endif
    int p[2];
    if (pipe(p) != 0) return -1;
    for (int i = 0; i < 2; i++) {
        kn_set_nonblock(p[i]);
        kn_set_cloexec(p[i]);
    }
    loop->wake_rd = p[0];
    loop->wake_wr = p[1];
    return kn_loop_add_fd(loop, p[0], KN_EV_READ, wake_cb, NULL);
}

static void wake(kn_loop *loop) {
    if (loop->wake_wr < 0) return;
#ifdef KN_HAVE_EPOLL
    if (loop->wake_wr == loop->wake_rd) {
        uint64_t one = 1;
        ssize_t r = write(loop->wake_wr, &one, sizeof one);
        (void)r;
        return;
    }
#endif
    char c = 1;
    ssize_t r = write(loop->wake_wr, &c, 1);
    (void)r;
}

/* ------------------------------------------------------------------ lifecycle */

kn_loop *kn_loop_new(void) {
    kn_loop *loop = calloc(1, sizeof *loop);
    if (!loop) return NULL;
    loop->epfd = -1;
    loop->wake_rd = loop->wake_wr = -1;
    loop->next_timer_id = 1;
    pthread_mutex_init(&loop->remote_lock, NULL);
#ifdef KN_HAVE_EPOLL
    const char *be = getenv("KN_LOOP_BACKEND");
    if (!be || strcmp(be, "poll") != 0) {
        loop->epfd = epoll_create1(EPOLL_CLOEXEC);
        loop->use_epoll = loop->epfd >= 0;
    }
#endif
    if (wake_init(loop) != 0) {
        kn_loop_free(loop);
        return NULL;
    }
    return loop;
}

static void free_queue(task_queue *q) {
    for (task *t = q->head; t;) {
        task *n = t->next;
        free(t);
        t = n;
    }
    q->head = q->tail = NULL;
}

void kn_loop_free(kn_loop *loop) {
    if (!loop) return;
    if (g_sig_loop == loop) {
        kn_loop_del_fd(loop, g_sig_pipe[0]);
        g_sig_loop = NULL;
        for (int s = 1; s < KN_MAX_SIGNAL; s++)
            if (loop->signals[s].cb) signal(s, SIG_DFL);
    }
    if (loop->wake_rd >= 0) {
        kn_loop_del_fd(loop, loop->wake_rd);
        close(loop->wake_rd);
        if (loop->wake_wr != loop->wake_rd) close(loop->wake_wr);
    }
    if (loop->epfd >= 0) close(loop->epfd);
    free_queue(&loop->local);
    free_queue(&loop->remote);
    pthread_mutex_destroy(&loop->remote_lock);
    free(loop->fds);
    free(loop->pfds);
    free(loop->timers);
    free(loop);
}

const char *kn_loop_backend(const kn_loop *loop) {
    return loop->use_epoll ? "epoll" : "poll";
}

/* ------------------------------------------------------------------ fds */

static int ensure_fd(kn_loop *loop, int fd) {
    if (fd < loop->fd_cap) return 0;
    int cap = loop->fd_cap ? loop->fd_cap : 64;
    while (cap <= fd) cap *= 2;
    fd_watch *n = realloc(loop->fds, (size_t)cap * sizeof *n);
    if (!n) return -1;
    memset(n + loop->fd_cap, 0, (size_t)(cap - loop->fd_cap) * sizeof *n);
    loop->fds = n;
    loop->fd_cap = cap;
    return 0;
}

#ifdef KN_HAVE_EPOLL
static uint32_t to_epoll(unsigned ev) {
    return (ev & KN_EV_READ ? EPOLLIN : 0u) | (ev & KN_EV_WRITE ? EPOLLOUT : 0u);
}

static int epoll_set(kn_loop *loop, int op, int fd) {
    struct epoll_event e;
    memset(&e, 0, sizeof e);
    e.events = to_epoll(loop->fds[fd].events);
    e.data.u64 = (uint64_t)(uint32_t)fd | ((uint64_t)loop->fds[fd].gen << 32);
    return epoll_ctl(loop->epfd, op, fd, &e);
}
#endif

int kn_loop_add_fd(kn_loop *loop, int fd, unsigned events, kn_io_cb cb, void *ud) {
    if (fd < 0 || !cb || ensure_fd(loop, fd) != 0) return -1;
    fd_watch *w = &loop->fds[fd];
    if (w->active) return -1;
    w->cb = cb;
    w->ud = ud;
    w->events = events & (KN_EV_READ | KN_EV_WRITE);
    w->gen++;
    w->active = 1;
#ifdef KN_HAVE_EPOLL
    if (loop->use_epoll && epoll_set(loop, EPOLL_CTL_ADD, fd) != 0) {
        w->active = 0;
        return -1;
    }
#endif
    loop->fd_count++;
    return 0;
}

int kn_loop_mod_fd(kn_loop *loop, int fd, unsigned events) {
    if (fd < 0 || fd >= loop->fd_cap || !loop->fds[fd].active) return -1;
    events &= KN_EV_READ | KN_EV_WRITE;
    if (loop->fds[fd].events == events) return 0;
    loop->fds[fd].events = events;
#ifdef KN_HAVE_EPOLL
    if (loop->use_epoll) return epoll_set(loop, EPOLL_CTL_MOD, fd);
#endif
    return 0;
}

void kn_loop_del_fd(kn_loop *loop, int fd) {
    if (fd < 0 || fd >= loop->fd_cap || !loop->fds[fd].active) return;
#ifdef KN_HAVE_EPOLL
    if (loop->use_epoll) epoll_ctl(loop->epfd, EPOLL_CTL_DEL, fd, NULL);
#endif
    loop->fds[fd].active = 0;
    loop->fds[fd].gen++;
    loop->fd_count--;
}

/* ------------------------------------------------------------------ timers (min-heap) */

static void heap_swap(timer *a, timer *b) {
    timer t = *a;
    *a = *b;
    *b = t;
}

static int timer_less(const timer *a, const timer *b) {
    return a->when < b->when || (a->when == b->when && a->id < b->id);
}

static void heap_up(timer *h, size_t i) {
    while (i > 0) {
        size_t p = (i - 1) / 2;
        if (!timer_less(&h[i], &h[p])) break;
        heap_swap(&h[i], &h[p]);
        i = p;
    }
}

static void heap_down(timer *h, size_t n, size_t i) {
    for (;;) {
        size_t l = 2 * i + 1, r = l + 1, m = i;
        if (l < n && timer_less(&h[l], &h[m])) m = l;
        if (r < n && timer_less(&h[r], &h[m])) m = r;
        if (m == i) break;
        heap_swap(&h[i], &h[m]);
        i = m;
    }
}

static int timer_push(kn_loop *loop, timer t) {
    if (loop->ntimers == loop->timer_cap) {
        size_t cap = loop->timer_cap ? loop->timer_cap * 2 : 32;
        timer *n = realloc(loop->timers, cap * sizeof *n);
        if (!n) return -1;
        loop->timers = n;
        loop->timer_cap = cap;
    }
    loop->timers[loop->ntimers] = t;
    heap_up(loop->timers, loop->ntimers++);
    return 0;
}

static void timer_remove_at(kn_loop *loop, size_t i) {
    loop->ntimers--;
    if (i == loop->ntimers) return;
    loop->timers[i] = loop->timers[loop->ntimers];
    heap_down(loop->timers, loop->ntimers, i);
    heap_up(loop->timers, i);
}

kn_timer_id kn_loop_add_timer(kn_loop *loop, uint64_t delay_ms, uint64_t repeat_ms, kn_timer_cb cb, void *ud) {
    if (!cb) return 0;
    timer t = {kn_now_ms() + delay_ms, repeat_ms, loop->next_timer_id++, cb, ud};
    return timer_push(loop, t) == 0 ? t.id : 0;
}

void kn_loop_cancel_timer(kn_loop *loop, kn_timer_id id) {
    if (!id) return;
    for (size_t i = 0; i < loop->ntimers; i++) {
        if (loop->timers[i].id == id) {
            timer_remove_at(loop, i);
            return;
        }
    }
}

static void run_timers(kn_loop *loop) {
    uint64_t now = kn_now_ms();
    /* bounded so a 0 ms repeating timer cannot starve I/O */
    for (int budget = 1024; budget > 0 && loop->ntimers && loop->timers[0].when <= now; budget--) {
        timer t = loop->timers[0];
        timer_remove_at(loop, 0);
        if (t.repeat) {
            timer again = t;
            again.when = now + t.repeat;
            timer_push(loop, again);
        }
        t.cb(loop, t.ud);
        if (loop->stop) break;
    }
}

/* ------------------------------------------------------------------ tasks */

static task *task_new(kn_task_cb cb, void *ud) {
    task *t = malloc(sizeof *t);
    if (t) {
        t->cb = cb;
        t->ud = ud;
        t->next = NULL;
    }
    return t;
}

static void queue_push(task_queue *q, task *t) {
    if (q->tail) q->tail->next = t;
    else q->head = t;
    q->tail = t;
}

int kn_loop_post(kn_loop *loop, kn_task_cb cb, void *ud) {
    task *t = task_new(cb, ud);
    if (!t) return -1;
    queue_push(&loop->local, t);
    return 0;
}

int kn_loop_post_threadsafe(kn_loop *loop, kn_task_cb cb, void *ud) {
    task *t = task_new(cb, ud);
    if (!t) return -1;
    pthread_mutex_lock(&loop->remote_lock);
    queue_push(&loop->remote, t);
    pthread_mutex_unlock(&loop->remote_lock);
    wake(loop);
    return 0;
}

static void run_tasks(kn_loop *loop) {
    pthread_mutex_lock(&loop->remote_lock);
    task *remote = loop->remote.head;
    loop->remote.head = loop->remote.tail = NULL;
    pthread_mutex_unlock(&loop->remote_lock);
    /* tasks posted while running go to the next iteration */
    task *local = loop->local.head;
    loop->local.head = loop->local.tail = NULL;
    for (task *lists[2] = {remote, local}, **l = lists; l < lists + 2; l++) {
        for (task *t = *l; t;) {
            task *n = t->next;
            t->cb(loop, t->ud);
            free(t);
            t = n;
        }
    }
}

static int has_tasks(kn_loop *loop) {
    if (loop->local.head) return 1;
    pthread_mutex_lock(&loop->remote_lock);
    int r = loop->remote.head != NULL;
    pthread_mutex_unlock(&loop->remote_lock);
    return r;
}

/* ------------------------------------------------------------------ run */

static int compute_timeout(kn_loop *loop, int timeout_ms) {
    if (has_tasks(loop)) return 0;
    if (loop->ntimers) {
        uint64_t now = kn_now_ms();
        uint64_t when = loop->timers[0].when;
        int t = when <= now ? 0 : (when - now > 3600000u ? 3600000 : (int)(when - now));
        if (timeout_ms < 0 || t < timeout_ms) timeout_ms = t;
    }
    return timeout_ms;
}

static void dispatch(kn_loop *loop, int fd, uint32_t gen, unsigned ev, int check_gen) {
    if (fd < 0 || fd >= loop->fd_cap) return;
    fd_watch *w = &loop->fds[fd];
    if (!w->active || (check_gen && w->gen != gen)) return;
    w->cb(loop, fd, ev, w->ud);
}

#ifdef KN_HAVE_EPOLL
static int wait_epoll(kn_loop *loop, int timeout) {
    struct epoll_event evs[128];
    int n = epoll_wait(loop->epfd, evs, 128, timeout);
    if (n < 0) return errno == EINTR ? 0 : -1;
    for (int i = 0; i < n; i++) {
        int fd = (int)(uint32_t)evs[i].data.u64;
        uint32_t gen = (uint32_t)(evs[i].data.u64 >> 32);
        uint32_t e = evs[i].events;
        unsigned ev = (e & EPOLLIN ? KN_EV_READ : 0u) | (e & EPOLLOUT ? KN_EV_WRITE : 0u) |
                      (e & EPOLLERR ? KN_EV_ERROR : 0u) | (e & (EPOLLHUP | EPOLLRDHUP) ? KN_EV_HUP : 0u);
        dispatch(loop, fd, gen, ev, 1);
    }
    return 0;
}
#endif

static int wait_poll(kn_loop *loop, int timeout) {
    if (loop->pfd_cap < loop->fd_count) {
        struct pollfd *n = realloc(loop->pfds, (size_t)loop->fd_count * sizeof *n);
        if (!n) return -1;
        loop->pfds = n;
        loop->pfd_cap = loop->fd_count;
    }
    int n = 0;
    for (int fd = 0; fd < loop->fd_cap && n < loop->fd_count; fd++) {
        if (!loop->fds[fd].active) continue;
        loop->pfds[n].fd = fd;
        loop->pfds[n].events = (short)((loop->fds[fd].events & KN_EV_READ ? POLLIN : 0) |
                                       (loop->fds[fd].events & KN_EV_WRITE ? POLLOUT : 0));
        loop->pfds[n].revents = 0;
        n++;
    }
    /* generations snapshot: a watcher replaced during dispatch is skipped */
    int r = poll(loop->pfds, (nfds_t)n, timeout);
    if (r < 0) return errno == EINTR ? 0 : -1;
    for (int i = 0; i < n && r > 0; i++) {
        short re = loop->pfds[i].revents;
        if (!re) continue;
        r--;
        unsigned ev = (re & POLLIN ? KN_EV_READ : 0u) | (re & POLLOUT ? KN_EV_WRITE : 0u) |
                      (re & (POLLERR | POLLNVAL) ? KN_EV_ERROR : 0u) | (re & POLLHUP ? KN_EV_HUP : 0u);
        dispatch(loop, loop->pfds[i].fd, 0, ev, 0);
    }
    return 0;
}

int kn_loop_run_once(kn_loop *loop, int timeout_ms) {
    int timeout = compute_timeout(loop, timeout_ms);
    int rc;
#ifdef KN_HAVE_EPOLL
    if (loop->use_epoll) rc = wait_epoll(loop, timeout);
    else
#endif
        rc = wait_poll(loop, timeout);
    run_timers(loop);
    run_tasks(loop);
    return rc;
}

int kn_loop_run(kn_loop *loop) {
    loop->stop = 0;
    while (!loop->stop) {
        if (kn_loop_run_once(loop, -1) != 0) {
            KN_ERROR("loop", "backend wait failed: %s", strerror(errno));
            return -1;
        }
    }
    return 0;
}

void kn_loop_stop(kn_loop *loop) {
    loop->stop = 1;
    wake(loop);
}

int kn_loop_stopped(const kn_loop *loop) {
    return loop->stop != 0;
}
