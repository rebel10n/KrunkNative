/*
 * services/brain/brain.h - shared context of the kn_brain executable (the v3.1.1
 * matchmaker "brain": matchmaker/index.ts).
 *
 * One Brain per process, created by main.c. Every brain module (registry, public
 * routes, internal WS, federation, accounts, social WS, ...) hangs off it and
 * registers its HTTP routes / WebSocket endpoints on `http` from its init hook.
 * All callbacks run on `loop`'s thread (knsvc is single threaded).
 *
 * Module hooks: main.c calls, in order, every `init` of the k_brain_modules table
 * (main.c), then kn_loop_run(), then every `shutdown` in reverse order. An init
 * returning non-zero aborts startup. Add a module with ONE anchored line in that
 * table; keep module state in the module (static or hung off `Brain.mod[]`).
 *
 * Ownership: main.c owns cfg/loop/db/http and frees them after the shutdowns.
 */
#ifndef KN_BRAIN_H
#define KN_BRAIN_H

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

struct kn_config;
struct kn_loop;
struct kn_http_server;
struct kn_db;

typedef struct Registry Registry;  /* registry.h (svc-matchmaker) */

typedef struct Brain {
    struct kn_config *cfg;         /* config/config.json + env + CLI (read-only) */
    struct kn_loop *loop;
    struct kn_http_server *http;   /* public listener: /mm/..., /api/..., /social/..., ws */
    struct kn_db *db;              /* database pool, any driver (NULL with --no-db: registry is local-only) */

    char id[64];                   /* brain id (config brain.id, default "<hostname>-<port>") */
    char region[64];               /* default region of this brain (brain.region) */
    char version[32];              /* KN_BRAIN_VERSION */
    int64_t started_ms;            /* wall clock */
    int draining;                  /* SIGTERM/SIGUSR1 received: stop accepting new work */

    Registry *registry;            /* lobby/node view (registry.c) */
} Brain;

typedef struct BrainModule {
    const char *name;
    int (*init)(Brain *b);         /* register routes, timers; 0 ok */
    void (*shutdown)(Brain *b);    /* may be NULL; called once, reverse order */
} BrainModule;

#define KN_BRAIN_VERSION "1.0.0"

/* Process-wide brain (set by main.c before the first init). */
Brain *brain_get(void);

/* CORS-aware JSON helper used by the public routes (no-cache headers like
 * v3.1.1 publicRoutes.ts JSON_HEADERS). json is not consumed. */
struct kn_http_req;
void brain_reply_json(struct kn_http_req *req, int status, const cJSON *json);
/* Pieces of it: the nginx cors.conf headers, and a bare "application/json" body. */
void brain_cors_headers(struct kn_http_req *req);
void brain_reply_json_raw(struct kn_http_req *req, int status, const cJSON *json);

#endif
