/*
 * knsvc/loop.h - single-threaded non-blocking event loop (epoll on Linux, poll()
 * elsewhere or with KN_LOOP_BACKEND=poll).
 *
 * Everything in knsvc that does I/O (HTTP, WebSocket, PostgreSQL) registers
 * with a kn_loop and runs its callbacks on the loop's thread. Only
 * kn_loop_post_threadsafe() and kn_loop_stop() may be called from other threads.
 *
 * Callbacks may add/remove fds and timers (including their own) freely.
 * Typical program:
 *     kn_loop *loop = kn_loop_new();
 *     kn_loop_stop_on_signals(loop);          // SIGINT/SIGTERM -> clean stop
 *     ... servers, pools ...
 *     kn_loop_run(loop);
 *     ... free in reverse order ...
 *     kn_loop_free(loop);
 */
#ifndef KNSVC_LOOP_H
#define KNSVC_LOOP_H

#include <stdint.h>

typedef struct kn_loop kn_loop;

enum {
    KN_EV_READ = 1u << 0,
    KN_EV_WRITE = 1u << 1,
    KN_EV_ERROR = 1u << 2, /* reported only, never requested */
    KN_EV_HUP = 1u << 3    /* reported only */
};

typedef void (*kn_io_cb)(kn_loop *loop, int fd, unsigned events, void *ud);
typedef void (*kn_timer_cb)(kn_loop *loop, void *ud);
typedef void (*kn_task_cb)(kn_loop *loop, void *ud);
typedef void (*kn_signal_cb)(kn_loop *loop, int signo, void *ud);
typedef uint64_t kn_timer_id; /* 0 = invalid */

kn_loop *kn_loop_new(void);
void kn_loop_free(kn_loop *loop);     /* does not close registered fds */
const char *kn_loop_backend(const kn_loop *loop); /* "epoll" or "poll" */

/* fd watching. The fd should be non-blocking (kn_set_nonblock). One watcher per fd. */
int kn_loop_add_fd(kn_loop *loop, int fd, unsigned events, kn_io_cb cb, void *ud);
int kn_loop_mod_fd(kn_loop *loop, int fd, unsigned events);
void kn_loop_del_fd(kn_loop *loop, int fd);

/* Timers (monotonic ms). repeat_ms 0 = one shot. Cancelling inside the callback is fine. */
kn_timer_id kn_loop_add_timer(kn_loop *loop, uint64_t delay_ms, uint64_t repeat_ms, kn_timer_cb cb, void *ud);
void kn_loop_cancel_timer(kn_loop *loop, kn_timer_id id);

/* Run cb on the loop thread at the start of the next iteration. */
int kn_loop_post(kn_loop *loop, kn_task_cb cb, void *ud);
/* Same, callable from any thread (wakes the loop through a pipe/eventfd). */
int kn_loop_post_threadsafe(kn_loop *loop, kn_task_cb cb, void *ud);

/* Signals: delivered on the loop thread (self-pipe). One callback per signal. */
int kn_loop_on_signal(kn_loop *loop, int signo, kn_signal_cb cb, void *ud);
int kn_loop_stop_on_signals(kn_loop *loop); /* SIGINT + SIGTERM -> kn_loop_stop; ignores SIGPIPE */

/* Run until kn_loop_stop(). Returns 0 (or -1 on a fatal backend error). */
int kn_loop_run(kn_loop *loop);
/* One iteration: wait up to timeout_ms (-1 = until something happens, 0 = poll)
 * and dispatch. Lets another main loop (e.g. server_bin's tick) drive knsvc. */
int kn_loop_run_once(kn_loop *loop, int timeout_ms);
void kn_loop_stop(kn_loop *loop);   /* thread-safe */
int kn_loop_stopped(const kn_loop *loop);

uint64_t kn_now_ms(void);           /* CLOCK_MONOTONIC milliseconds */
int64_t kn_wall_ms(void);           /* unix epoch milliseconds */
int kn_set_nonblock(int fd);
int kn_set_cloexec(int fd);
/* socket(2) / accept(2) returning a non-blocking close-on-exec fd (-1 + errno on failure):
 * SOCK_NONBLOCK|SOCK_CLOEXEC and accept4 where the OS has them (Linux, BSD), else fcntl
 * (macOS). */
int kn_socket_nb(int domain, int type, int protocol);
int kn_accept_nb(int listen_fd);

#endif
