/*
 * Worker pool of the store layer: runs a blocking driver (sqlite, mysql, or
 * postgres' blocking ops for C kn_procs) on threads, one connection per thread,
 * and hands results back to the kn_loop thread through a pipe.
 *
 *  - pool jobs (query, call, begin) go to any worker; transaction jobs go to the
 *    worker the BEGIN ran on (pinned until COMMIT/ROLLBACK), so they run in order;
 *  - a failed statement aborts the transaction like PostgreSQL does: later
 *    statements fail with 25P02 and COMMIT rolls back and reports 40000;
 *  - a worker without a connection reconnects with backoff while pool jobs wait
 *    in the queue; a connection lost under a pinned transaction fails its jobs
 *    with 08006;
 *  - kn_wpool_free fails queued jobs with 08003 and delivers every callback.
 */
#include "store_internal.h"
#include "knsvc/log.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define BACKOFF_MIN_MS 250
#define BACKOFF_MAX_MS 10000

enum { J_QUERY, J_CALL, J_BEGIN, J_TXQ, J_TXCALL, J_END, J_DONE };

typedef struct job {
    int kind;
    char *sql;
    const kn_proc *proc;
    int nparams;
    char **params;
    kn_db_cb cb;
    kn_store_tx_cb tx_cb;
    void *ud;
    kn_wtx *tx;          /* J_TXQ / J_TXCALL / J_END */
    int commit;
    kn_db_result *res;   /* completion */
    kn_wtx *tx_out;      /* J_BEGIN completion */
    struct job *next;
} job;

typedef struct jlist {
    job *head, *tail;
    size_t len;
} jlist;

struct kn_wtx {
    kn_wpool *p;
    jlist q;
    int failed;
    int broken;
    int ending;
};

typedef struct worker {
    kn_wpool *p;
    int idx;
    pthread_t th;
    int started;
    void *conn;
    kn_dbconn dc;
    kn_wtx *tx;          /* pinned transaction */
    int busy;
    int backoff_ms;
} worker;

struct kn_wpool {
    kn_loop *loop;
    const kn_store_driver *drv;
    kn_db_config cfg;
    char *strs[8];
    pthread_mutex_t mu;
    pthread_cond_t cv;
    jlist queue;
    jlist done;
    worker *w;
    int nw;
    int stop;
    int ready;
    int down_logged;
    int pipe_r, pipe_w;
    int signaled;
};

/* ------------------------------------------------------------------ lists / jobs */

static void j_push(jlist *l, job *j) {
    j->next = NULL;
    if (l->tail) l->tail->next = j;
    else l->head = j;
    l->tail = j;
    l->len++;
}

static job *j_pop(jlist *l) {
    job *j = l->head;
    if (!j) return NULL;
    l->head = j->next;
    if (!l->head) l->tail = NULL;
    l->len--;
    j->next = NULL;
    return j;
}

static void job_free(job *j) {
    if (!j) return;
    for (int i = 0; i < j->nparams; i++) free(j->params[i]);
    free(j->params);
    free(j->sql);
    kn_db_result_free(j->res);
    free(j);
}

static job *job_new(int kind, char *sql, int n, const char *const *params, kn_db_cb cb, void *ud) {
    job *j = calloc(1, sizeof *j);
    if (!j) {
        free(sql);
        return NULL;
    }
    j->kind = kind;
    j->sql = sql;
    j->cb = cb;
    j->ud = ud;
    if (n > 0) {
        j->params = calloc((size_t)n, sizeof *j->params);
        if (!j->params) goto oom;
        j->nparams = n;
        for (int i = 0; i < n; i++) {
            if (!params[i]) continue;
            j->params[i] = strdup(params[i]);
            if (!j->params[i]) goto oom;
        }
    }
    return j;
oom:
    job_free(j);
    return NULL;
}

/* ------------------------------------------------------------------ completion (loop side) */

static void signal_loop(kn_wpool *p) { /* mu held */
    if (p->signaled) return;
    p->signaled = 1;
    ssize_t r = write(p->pipe_w, "x", 1);
    (void)r;
}

static void deliver(job *j) {
    kn_db_result *res = j->res ? j->res : kn_db_result_error("53200", "out of memory");
    j->res = NULL;
    if (j->kind == J_BEGIN) {
        if (j->tx_cb) j->tx_cb(kn_db_ok(res) ? j->tx_out : NULL, res, j->ud);
    } else if (j->cb) {
        j->cb(res, j->ud);
    }
    kn_db_result_free(res);
    job_free(j);
}

static void drain(kn_wpool *p) {
    pthread_mutex_lock(&p->mu);
    char buf[64];
    while (read(p->pipe_r, buf, sizeof buf) > 0) {}
    p->signaled = 0;
    jlist l = p->done;
    memset(&p->done, 0, sizeof p->done);
    pthread_mutex_unlock(&p->mu);
    job *j;
    while ((j = j_pop(&l)) != NULL) deliver(j);
}

static void on_pipe(kn_loop *loop, int fd, unsigned ev, void *ud) { drain(ud); }

/* mu held */
static void complete_locked(kn_wpool *p, job *j, kn_db_result *res) {
    kn_db_result_free(j->res);
    j->res = res;
    j_push(&p->done, j);
    signal_loop(p);
}

/* ------------------------------------------------------------------ worker thread */

static const char *msg_closed = "database pool closed";

static void drop_conn(worker *w) {
    if (!w->conn) return;
    w->p->drv->blocking->disconnect(w->conn);
    w->conn = NULL;
    w->dc.conn = NULL;
    pthread_mutex_lock(&w->p->mu);
    w->p->ready--;
    pthread_mutex_unlock(&w->p->mu);
}

static int conn_broken(worker *w, const kn_db_result *res) {
    const kn_store_blocking_ops *b = w->p->drv->blocking;
    if (b->broken) return b->broken(w->conn);
    return kn_db_errclass_of(res) == KN_DB_E_CONNECTION;
}

static kn_db_result *exec_on(worker *w, const char *sql, int n, char **params) {
    kn_db_result *r = w->p->drv->blocking->exec(w->conn, sql, n, (const char *const *)params);
    if (!r) r = kn_db_result_error("53200", "out of memory");
    if (!kn_db_ok(r) && conn_broken(w, r)) {
        drop_conn(w);
        if (w->tx) w->tx->broken = 1;
        if (kn_db_errclass_of(r) != KN_DB_E_CONNECTION) {
            char msg[600];
            snprintf(msg, sizeof msg, "%s", kn_db_error(r));
            kn_db_result_free(r);
            r = kn_db_result_error("08006", msg);
        }
    }
    return r;
}

static kn_db_result *run_proc(worker *w, const kn_proc *proc, int n, char **params) {
    kn_db_result *r = proc && proc->run ? proc->run(&w->dc, n, (const char *const *)params)
                                        : kn_db_result_error("42883", "procedure has no C implementation");
    if (!r) r = kn_db_result_error("53200", "out of memory");
    if (!kn_db_ok(r) && w->conn && conn_broken(w, r)) {
        drop_conn(w);
        if (w->tx) w->tx->broken = 1;
    }
    return r;
}

static void end_tx_sql(worker *w, const char *sql) {
    if (!w->conn) return;
    kn_db_result *r = exec_on(w, sql, 0, NULL);
    kn_db_result_free(r);
}

static kn_db_result *run_job_once(worker *w, job *j);

/* Statements and procs outside a caller's transaction are retried a few times
 * on deadlock / serialization / busy (KN_DB_E_RETRY): they ran in a transaction
 * of their own that the database rolled back. */
static kn_db_result *run_job(worker *w, job *j) {
    kn_db_result *r = run_job_once(w, j);
    for (int attempt = 1; attempt < 4 && (j->kind == J_QUERY || j->kind == J_CALL) && w->conn &&
                          kn_db_errclass_of(r) == KN_DB_E_RETRY && !w->p->stop;
         attempt++) {
        kn_db_result_free(r);
        usleep((useconds_t)(attempt * 20000));
        r = run_job_once(w, j);
    }
    return r;
}

/* Runs j on w (mu not held). Returns the result; J_BEGIN sets j->tx_out and pins. */
static kn_db_result *run_job_once(worker *w, job *j) {
    kn_wpool *p = w->p;
    const char *begin = p->drv->blocking->begin_sql ? p->drv->blocking->begin_sql : "BEGIN";
    switch (j->kind) {
    case J_QUERY:
        if (!j->sql) return kn_db_result_error("42883", "no SQL text for this database driver");
        return exec_on(w, j->sql, j->nparams, j->params);
    case J_CALL: {
        kn_db_result *r = exec_on(w, begin, 0, NULL);
        if (!kn_db_ok(r)) return r;
        kn_db_result_free(r);
        r = run_proc(w, j->proc, j->nparams, j->params);
        if (!w->conn) return r;
        if (kn_db_ok(r)) {
            kn_db_result *c = exec_on(w, "COMMIT", 0, NULL);
            if (!kn_db_ok(c)) {
                kn_db_result_free(r);
                return c;
            }
            kn_db_result_free(c);
        } else {
            end_tx_sql(w, "ROLLBACK");
        }
        return r;
    }
    case J_BEGIN: {
        kn_db_result *r = exec_on(w, begin, 0, NULL);
        if (!kn_db_ok(r)) return r;
        kn_wtx *tx = calloc(1, sizeof *tx);
        if (!tx) {
            end_tx_sql(w, "ROLLBACK");
            kn_db_result_free(r);
            return kn_db_result_error("53200", "out of memory");
        }
        tx->p = p;
        j->tx_out = tx;
        pthread_mutex_lock(&p->mu);
        w->tx = tx;
        pthread_mutex_unlock(&p->mu);
        return r;
    }
    case J_TXQ:
    case J_TXCALL: {
        kn_wtx *tx = j->tx;
        if (tx->broken || !w->conn) return kn_db_result_error("08006", "connection lost during transaction");
        if (tx->failed)
            return kn_db_result_error("25P02", "current transaction is aborted, commands ignored until end of "
                                               "transaction block");
        kn_db_result *r;
        if (j->kind == J_TXQ)
            r = j->sql ? exec_on(w, j->sql, j->nparams, j->params)
                       : kn_db_result_error("42883", "no SQL text for this database driver");
        else
            r = run_proc(w, j->proc, j->nparams, j->params);
        if (!kn_db_ok(r)) tx->failed = 1;
        return r;
    }
    case J_END: {
        kn_wtx *tx = j->tx;
        kn_db_result *r;
        if (tx->broken || !w->conn) {
            r = kn_db_result_error("08006", "transaction connection lost");
        } else if (j->commit && !tx->failed) {
            r = exec_on(w, "COMMIT", 0, NULL);
            if (!kn_db_ok(r) && w->conn) end_tx_sql(w, "ROLLBACK");
        } else {
            r = exec_on(w, "ROLLBACK", 0, NULL);
            if (j->commit && kn_db_ok(r)) {
                kn_db_result_free(r);
                r = kn_db_result_error("40000", "transaction rolled back: a statement in it failed");
            }
        }
        pthread_mutex_lock(&p->mu);
        w->tx = NULL;
        while (tx->q.head) complete_locked(p, j_pop(&tx->q), kn_db_result_error("25P02", "transaction ended"));
        free(tx);
        pthread_mutex_unlock(&p->mu);
        j->tx = NULL;
        return r;
    }
    }
    return kn_db_result_error("XX000", "bad job");
}

static void wait_ms_locked(kn_wpool *p, int ms) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    pthread_cond_timedwait(&p->cv, &p->mu, &ts);
}

static void *worker_main(void *arg) {
    worker *w = arg;
    kn_wpool *p = w->p;
    pthread_mutex_lock(&p->mu);
    for (;;) {
        if (p->stop) break;
        if (!w->conn && !w->tx) {
            pthread_mutex_unlock(&p->mu);
            char err[512] = "";
            void *c = p->drv->blocking->connect(&p->cfg, err, sizeof err);
            pthread_mutex_lock(&p->mu);
            if (!c) {
                if (!p->down_logged || w->backoff_ms == 0)
                    KN_WARN("db", "%s connection %d failed: %s", p->drv->name, w->idx, err);
                p->down_logged = 1;
                w->backoff_ms = w->backoff_ms ? w->backoff_ms * 2 : BACKOFF_MIN_MS;
                if (w->backoff_ms > BACKOFF_MAX_MS) w->backoff_ms = BACKOFF_MAX_MS;
                if (!p->stop) wait_ms_locked(p, w->backoff_ms);
                continue;
            }
            if (p->down_logged && w->backoff_ms) KN_INFO("db", "%s connection %d up", p->drv->name, w->idx);
            p->down_logged = 0;
            w->backoff_ms = 0;
            w->conn = c;
            w->dc.conn = c;
            p->ready++;
            if (p->stop) break;
        }
        job *j = w->tx ? j_pop(&w->tx->q) : j_pop(&p->queue);
        if (!j) {
            pthread_cond_wait(&p->cv, &p->mu);
            continue;
        }
        w->busy = 1;
        pthread_mutex_unlock(&p->mu);
        kn_db_result *res = run_job(w, j);
        pthread_mutex_lock(&p->mu);
        w->busy = 0;
        if (p->stop) {
            /* closing: report like a query the pool never ran */
            if (j->kind == J_BEGIN && j->tx_out) {
                if (w->tx == j->tx_out) w->tx = NULL;
                kn_wtx *tx = j->tx_out;
                while (tx->q.head) complete_locked(p, j_pop(&tx->q), kn_db_result_error("08003", msg_closed));
                free(tx);
                j->tx_out = NULL;
                pthread_mutex_unlock(&p->mu);
                end_tx_sql(w, "ROLLBACK");
                pthread_mutex_lock(&p->mu);
            }
            kn_db_result_free(res);
            res = kn_db_result_error("08003", msg_closed);
        }
        complete_locked(p, j, res);
    }
    kn_wtx *tx = w->tx;
    w->tx = NULL;
    if (tx) {
        while (tx->q.head) complete_locked(p, j_pop(&tx->q), kn_db_result_error("08003", msg_closed));
        free(tx);
    }
    pthread_mutex_unlock(&p->mu);
    if (tx) end_tx_sql(w, "ROLLBACK");
    if (w->conn) {
        p->drv->blocking->disconnect(w->conn);
        w->conn = NULL;
    }
    return NULL;
}

/* ------------------------------------------------------------------ public */

static char *dup_or_null(const char *s) { return s ? strdup(s) : NULL; }

kn_wpool *kn_wpool_new(kn_loop *loop, const kn_store_driver *drv, const kn_db_config *cfg, int nthreads) {
    kn_wpool *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->loop = loop;
    p->drv = drv;
    p->cfg = *cfg;
    const char **fields[] = {&p->cfg.host, &p->cfg.name, &p->cfg.user, &p->cfg.password,
                             &p->cfg.search_path, &p->cfg.application_name, &p->cfg.driver, &p->cfg.path};
    for (int i = 0; i < 8; i++) {
        p->strs[i] = dup_or_null(*fields[i]);
        *fields[i] = p->strs[i];
    }
    pthread_mutex_init(&p->mu, NULL);
    pthread_cond_init(&p->cv, NULL);
    int fds[2];
    if (pipe(fds) != 0) goto fail;
    p->pipe_r = fds[0];
    p->pipe_w = fds[1];
    kn_set_nonblock(p->pipe_r);
    kn_set_nonblock(p->pipe_w);
    kn_set_cloexec(p->pipe_r);
    kn_set_cloexec(p->pipe_w);
    if (kn_loop_add_fd(loop, p->pipe_r, KN_EV_READ, on_pipe, p) != 0) goto fail;
    p->nw = nthreads > 0 ? nthreads : 1;
    p->w = calloc((size_t)p->nw, sizeof *p->w);
    if (!p->w) goto fail;
    for (int i = 0; i < p->nw; i++) {
        worker *w = &p->w[i];
        w->p = p;
        w->idx = i;
        w->dc.drv = drv;
        if (pthread_create(&w->th, NULL, worker_main, w) != 0) {
            KN_ERROR("db", "cannot start database worker thread %d", i);
            break;
        }
        w->started = 1;
    }
    return p;
fail:
    KN_ERROR("db", "cannot create the %s worker pool: %s", drv->name, strerror(errno));
    kn_wpool_free(p);
    return NULL;
}

void kn_wpool_free(kn_wpool *p) {
    if (!p) return;
    pthread_mutex_lock(&p->mu);
    p->stop = 1;
    job *j;
    while ((j = j_pop(&p->queue)) != NULL) complete_locked(p, j, kn_db_result_error("08003", msg_closed));
    for (int i = 0; i < p->nw; i++) {
        kn_wtx *tx = p->w ? p->w[i].tx : NULL;
        while (tx && tx->q.head) complete_locked(p, j_pop(&tx->q), kn_db_result_error("08003", msg_closed));
        if (p->w && p->w[i].busy && p->w[i].conn && p->drv->blocking->interrupt)
            p->drv->blocking->interrupt(p->w[i].conn);
    }
    pthread_cond_broadcast(&p->cv);
    pthread_mutex_unlock(&p->mu);
    for (int i = 0; p->w && i < p->nw; i++)
        if (p->w[i].started) pthread_join(p->w[i].th, NULL);
    if (p->pipe_r > 0) {
        kn_loop_del_fd(p->loop, p->pipe_r);
        drain(p);
        close(p->pipe_r);
        close(p->pipe_w);
    }
    free(p->w);
    pthread_mutex_destroy(&p->mu);
    pthread_cond_destroy(&p->cv);
    for (int i = 0; i < 8; i++) free(p->strs[i]);
    free(p);
}

int kn_wpool_ready(const kn_wpool *p) {
    kn_wpool *m = (kn_wpool *)p;
    pthread_mutex_lock(&m->mu);
    int n = m->ready;
    pthread_mutex_unlock(&m->mu);
    return n;
}

size_t kn_wpool_queue_len(const kn_wpool *p) {
    kn_wpool *m = (kn_wpool *)p;
    pthread_mutex_lock(&m->mu);
    size_t n = m->queue.len;
    pthread_mutex_unlock(&m->mu);
    return n;
}

static int submit(kn_wpool *p, job *j, kn_wtx *tx) {
    if (!j) return -1;
    pthread_mutex_lock(&p->mu);
    if (p->stop) {
        complete_locked(p, j, kn_db_result_error("08003", msg_closed));
    } else if (tx) {
        j_push(&tx->q, j);
    } else {
        j_push(&p->queue, j);
    }
    pthread_cond_broadcast(&p->cv);
    pthread_mutex_unlock(&p->mu);
    return 0;
}

int kn_wpool_query(kn_wpool *p, char *sql, int n, const char *const *params, kn_db_cb cb, void *ud) {
    return submit(p, job_new(J_QUERY, sql, n, params, cb, ud), NULL);
}

int kn_wpool_call(kn_wpool *p, const kn_proc *proc, int n, const char *const *params, kn_db_cb cb, void *ud) {
    job *j = job_new(J_CALL, NULL, n, params, cb, ud);
    if (j) j->proc = proc;
    return submit(p, j, NULL);
}

int kn_wpool_begin(kn_wpool *p, kn_store_tx_cb cb, void *ud) {
    job *j = job_new(J_BEGIN, NULL, 0, NULL, NULL, ud);
    if (j) j->tx_cb = cb;
    return submit(p, j, NULL);
}

int kn_wtx_query(kn_wtx *tx, char *sql, int n, const char *const *params, kn_db_cb cb, void *ud) {
    if (!tx || tx->ending) {
        free(sql);
        return -1;
    }
    job *j = job_new(J_TXQ, sql, n, params, cb, ud);
    if (j) j->tx = tx;
    return submit(tx->p, j, tx);
}

int kn_wtx_call(kn_wtx *tx, const kn_proc *proc, int n, const char *const *params, kn_db_cb cb, void *ud) {
    if (!tx || tx->ending) return -1;
    job *j = job_new(J_TXCALL, NULL, n, params, cb, ud);
    if (j) {
        j->tx = tx;
        j->proc = proc;
    }
    return submit(tx->p, j, tx);
}

int kn_wtx_end(kn_wtx *tx, int commit, kn_db_cb cb, void *ud) {
    if (!tx || tx->ending) return -1;
    job *j = job_new(J_END, NULL, 0, NULL, cb, ud);
    if (!j) return -1;
    tx->ending = 1;
    j->tx = tx;
    j->commit = commit;
    return submit(tx->p, j, tx);
}

int kn_wpool_fail_async(kn_wpool *p, const char *sqlstate, const char *msg, kn_db_cb cb, void *ud) {
    job *j = job_new(J_DONE, NULL, 0, NULL, cb, ud);
    if (!j) return -1;
    pthread_mutex_lock(&p->mu);
    complete_locked(p, j, kn_db_result_error(sqlstate, msg));
    pthread_mutex_unlock(&p->mu);
    return 0;
}
