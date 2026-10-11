/* Private pieces of the store layer (store*.c and the built-in drivers). */
#ifndef KNSVC_STORE_INTERNAL_H
#define KNSVC_STORE_INTERNAL_H

#include "knsvc/loop.h"
#include "knsvc/store.h"

struct kn_dbconn {
    const kn_store_driver *drv;
    void *conn;
    int owned;          /* kn_dbconn_close disconnects */
};

/* ---- worker pool (store_worker.c): blocking driver ops on threads, results on the loop */
typedef struct kn_wpool kn_wpool;
typedef struct kn_wtx kn_wtx;

kn_wpool *kn_wpool_new(kn_loop *loop, const kn_store_driver *drv, const kn_db_config *cfg, int nthreads);
/* Fails queued work with 08003 (callbacks run), waits for the threads. */
void kn_wpool_free(kn_wpool *p);
int kn_wpool_ready(const kn_wpool *p);
size_t kn_wpool_queue_len(const kn_wpool *p);
/* sql: resolved text, taken over (freed by the pool); NULL = fail with 42883 "no SQL for this driver". */
int kn_wpool_query(kn_wpool *p, char *sql, int n, const char *const *params, kn_db_cb cb, void *ud);
int kn_wpool_call(kn_wpool *p, const kn_proc *proc, int n, const char *const *params, kn_db_cb cb, void *ud);
int kn_wpool_begin(kn_wpool *p, kn_store_tx_cb cb, void *ud);
int kn_wtx_query(kn_wtx *tx, char *sql, int n, const char *const *params, kn_db_cb cb, void *ud);
int kn_wtx_call(kn_wtx *tx, const kn_proc *proc, int n, const char *const *params, kn_db_cb cb, void *ud);
int kn_wtx_end(kn_wtx *tx, int commit, kn_db_cb cb, void *ud);
/* Deliver an error to cb asynchronously (next loop iteration). */
int kn_wpool_fail_async(kn_wpool *p, const char *sqlstate, const char *msg, kn_db_cb cb, void *ud);

/* Keep a native object on an error result (freed with it; kn_db_result_native_ptr). */
void kn_db__result_attach(kn_db_result *r, const kn_store_result_ops *ops, void *native);

/* ---- driver registry helpers (store_drivers.c) */
const kn_store_driver *kn_store_default_driver(int network_configured);

#endif
