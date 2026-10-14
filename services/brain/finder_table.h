/*
 * services/brain/finder_table.h - the finder's server table (finder.h), pure: explicit
 * clocks, no sockets, so tests/test_finder.c covers heartbeats, polling, verification,
 * expiry and filters. finder.c owns the UDP socket and the HTTP routes.
 */
#ifndef KN_BRAIN_FINDER_TABLE_H
#define KN_BRAIN_FINDER_TABLE_H

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

#define FINDER_HOST_MAX 128
#define FINDER_NONCE_LEN 32
#define FINDER_INFO_MAX 1200       /* contract: JSON object of at most 1200 bytes */

typedef struct FinderConfig {
    int poll_interval_ms;          /* re-poll every (30 s) */
    int poll_timeout_ms;           /* a poll without reply after this counts as failed (3 s) */
    int max_failures;              /* failed polls in a row before the entry is dropped (3) */
    int expire_ms;                 /* no heartbeat for this long -> dropped (180 s) */
    int max_per_ip;                /* entries one submitter IP may hold (64) */
    int heartbeat_interval_s;      /* told to servers in the reply (60) */
} FinderConfig;

void finder_config_defaults(FinderConfig *c);

typedef struct FinderEntry {
    char host[FINDER_HOST_MAX];    /* where clients connect (and the query goes) */
    int port, query_port;
    char submitter[64];            /* IP that sent the heartbeats */
    char hb_name[64], hb_community[64];
    int hb_online_mode;
    uint64_t last_heartbeat_ms;

    cJSON *info;                   /* last verified KNINFO reply (NULL until verified) */
    uint64_t last_ok_ms;           /* monotonic time of that reply */
    int64_t last_ok_wall_s;
    int ping_ms;
    int failures;
    char nonce[FINDER_NONCE_LEN + 1]; /* outstanding poll ("" = none) */
    uint64_t poll_sent_ms, next_poll_ms;
    void *addr;                    /* finder.c: resolved query address (owned, free()d) */
} FinderEntry;

typedef struct FinderTable FinderTable;

FinderTable *finder_table_new(const FinderConfig *cfg);
void finder_table_free(FinderTable *t);
const FinderConfig *finder_table_config(const FinderTable *t);
size_t finder_table_count(const FinderTable *t);
FinderEntry *finder_table_at(FinderTable *t, size_t i);

/* Heartbeat: create or refresh host:port. err (static text) set when refused (bad
 * fields, per-IP cap). created = 1 for a new entry (poll it now). */
FinderEntry *finder_heartbeat(FinderTable *t, const char *host, int port, int query_port, const char *name,
                              const char *community, int online_mode, const char *submitter, uint64_t now_ms,
                              int *created, const char **err);
/* shutdown heartbeat: removes the entry when the same submitter owns it. 1 = removed. */
int finder_remove(FinderTable *t, const char *host, int port, const char *submitter);

/* KNINFO wire format (contract-info-finder.md). Request into out (>= 11 + 32 bytes):
 * returns its length. */
size_t finder_build_query(const char *nonce, uint8_t *out, size_t cap);
/* Parse a reply: JSON object (caller frees) or NULL when malformed / too long. */
cJSON *finder_parse_reply(const uint8_t *data, size_t len);

/* Polling: entries whose next poll is due get `nonce` set and poll_sent_ms = now; the
 * caller sends the query. Returns how many were stamped into out (at most max). */
size_t finder_due_polls(FinderTable *t, uint64_t now_ms, FinderEntry **out, size_t max);
void finder_poll_stamp(FinderTable *t, FinderEntry *e, const char *nonce, uint64_t now_ms);
/* A reply arrived for e: verified when its nonce matches the outstanding poll. 1 = ok. */
int finder_poll_reply(FinderTable *t, FinderEntry *e, cJSON *info, uint64_t now_ms, int64_t wall_s);
/* Timeouts, failure limit and heartbeat expiry. free_addr(e->addr) is called for removed
 * entries. Returns the number removed. */
size_t finder_sweep(FinderTable *t, uint64_t now_ms, void (*free_addr)(void *));

typedef struct FinderFilter {
    const char *map, *mode, *tag, *community, *region, *q;
    const char *flavor;            /* product flavor pool (flavor.h key): NULL/"" or "base" = the base
                                    * game (hides Strike lobbies); "strike" = only Strike lobbies;
                                    * "all"/"*" = every flavor (dashboards). A server advertises its
                                    * flavor in the KNINFO reply; absent = base. */
    int space;                     /* 1 = players < max */
    int online_mode;               /* -1 any, 0, 1 */
    int limit;                     /* 0 = 500 */
} FinderFilter;

/* Verified entries matching f, as the GET /servers array (caller frees). */
cJSON *finder_list(FinderTable *t, const FinderFilter *f, uint64_t now_ms);

#endif
