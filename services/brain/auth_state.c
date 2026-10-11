/*
 * services/brain/auth_state.c - join records and rate limiters of the auth role
 * (auth_state.h). Chained hash tables keyed by strings; expiry is lazy (purge on a timer
 * and on lookup).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "auth_state.h"

#define BUCKETS 4096

static uint32_t str_hash(const char *s) {
    uint32_t h = 2166136261u; /* FNV-1a */
    for (; *s; s++) h = (h ^ (uint8_t) *s) * 16777619u;
    return h;
}

/* ---------------------------------------------------------------- joins */

typedef struct JoinRec {
    struct JoinRec *next;
    char hash[72];
    char username[40];
    char ip[64];
    int64_t account_id;
    cJSON *profile;
    uint64_t expires_ms;
} JoinRec;

struct AuthJoins {
    JoinRec *b[BUCKETS];
    size_t count;
};

AuthJoins *auth_joins_new(void) { return calloc(1, sizeof(AuthJoins)); }

static void rec_free(JoinRec *r) {
    cJSON_Delete(r->profile);
    free(r);
}

void auth_joins_free(AuthJoins *j) {
    if (!j) return;
    for (size_t i = 0; i < BUCKETS; i++) {
        for (JoinRec *r = j->b[i], *n; r; r = n) {
            n = r->next;
            rec_free(r);
        }
    }
    free(j);
}

/* Remove the record with the soonest expiry (cap reached). */
static void evict_oldest(AuthJoins *j) {
    JoinRec **best = NULL;
    for (size_t i = 0; i < BUCKETS; i++) {
        for (JoinRec **pp = &j->b[i]; *pp; pp = &(*pp)->next) {
            if (!best || (*pp)->expires_ms < (*best)->expires_ms) best = pp;
        }
    }
    if (!best) return;
    JoinRec *r = *best;
    *best = r->next;
    rec_free(r);
    j->count--;
}

int auth_joins_put(AuthJoins *j, const char *server_hash, int64_t account_id, const char *username,
                   const char *ip, const cJSON *profile, uint64_t now_ms) {
    if (!j || !server_hash || !*server_hash || strlen(server_hash) >= sizeof(((JoinRec *) 0)->hash)) return -1;
    if (!username || strlen(username) >= sizeof(((JoinRec *) 0)->username)) return -1;
    JoinRec **head = &j->b[str_hash(server_hash) % BUCKETS];
    for (JoinRec **pp = head; *pp; pp = &(*pp)->next) {
        if (!strcmp((*pp)->hash, server_hash)) {
            JoinRec *old = *pp;
            *pp = old->next;
            rec_free(old);
            j->count--;
            break;
        }
    }
    if (j->count >= AUTH_JOIN_MAX) {
        auth_joins_purge(j, now_ms);
        if (j->count >= AUTH_JOIN_MAX) evict_oldest(j);
    }
    JoinRec *r = calloc(1, sizeof(*r));
    if (!r) return -1;
    strcpy(r->hash, server_hash);
    strcpy(r->username, username);
    snprintf(r->ip, sizeof(r->ip), "%s", ip ? ip : "");
    r->account_id = account_id;
    r->profile = profile ? cJSON_Duplicate(profile, 1) : cJSON_CreateObject();
    r->expires_ms = now_ms + AUTH_JOIN_TTL_MS;
    r->next = *head;
    *head = r;
    j->count++;
    return 0;
}

const cJSON *auth_joins_get(AuthJoins *j, const char *server_hash, const char *username, const char *ip,
                            int check_ip, uint64_t now_ms) {
    if (!j || !server_hash || !username) return NULL;
    JoinRec **head = &j->b[str_hash(server_hash) % BUCKETS];
    for (JoinRec **pp = head; *pp; pp = &(*pp)->next) {
        JoinRec *r = *pp;
        if (strcmp(r->hash, server_hash) != 0) continue;
        if (r->expires_ms <= now_ms) {
            *pp = r->next;
            rec_free(r);
            j->count--;
            return NULL;
        }
        if (strcasecmp(r->username, username) != 0) return NULL;
        if (check_ip && ip && *ip && r->ip[0] && strcmp(r->ip, ip) != 0) return NULL;
        return r->profile;
    }
    return NULL;
}

size_t auth_joins_purge(AuthJoins *j, uint64_t now_ms) {
    if (!j) return 0;
    for (size_t i = 0; i < BUCKETS; i++) {
        for (JoinRec **pp = &j->b[i]; *pp;) {
            if ((*pp)->expires_ms <= now_ms) {
                JoinRec *r = *pp;
                *pp = r->next;
                rec_free(r);
                j->count--;
            } else {
                pp = &(*pp)->next;
            }
        }
    }
    return j->count;
}

size_t auth_joins_count(const AuthJoins *j) { return j ? j->count : 0; }

/* ---------------------------------------------------------------- limiter */

typedef struct LimRec {
    struct LimRec *next;
    char key[80];
    uint64_t window_start;
    int hits;
} LimRec;

struct AuthLimiter {
    LimRec *b[BUCKETS];
    int limit;
    uint64_t window_ms;
    uint64_t last_sweep;
};

AuthLimiter *auth_limiter_new(int limit, uint64_t window_ms) {
    AuthLimiter *l = calloc(1, sizeof(*l));
    if (!l) return NULL;
    l->limit = limit;
    l->window_ms = window_ms ? window_ms : 1;
    return l;
}

void auth_limiter_free(AuthLimiter *l) {
    if (!l) return;
    for (size_t i = 0; i < BUCKETS; i++) {
        for (LimRec *r = l->b[i], *n; r; r = n) {
            n = r->next;
            free(r);
        }
    }
    free(l);
}

static void limiter_sweep(AuthLimiter *l, uint64_t now_ms) {
    for (size_t i = 0; i < BUCKETS; i++) {
        for (LimRec **pp = &l->b[i]; *pp;) {
            if (now_ms - (*pp)->window_start >= l->window_ms) {
                LimRec *r = *pp;
                *pp = r->next;
                free(r);
            } else {
                pp = &(*pp)->next;
            }
        }
    }
    l->last_sweep = now_ms;
}

int auth_limiter_hit(AuthLimiter *l, const char *key, uint64_t now_ms) {
    if (!l || l->limit <= 0) return 1;
    if (!key) key = "";
    if (now_ms - l->last_sweep >= l->window_ms * 4) limiter_sweep(l, now_ms);
    LimRec **head = &l->b[str_hash(key) % BUCKETS];
    LimRec *r = *head;
    while (r && strncmp(r->key, key, sizeof(r->key) - 1) != 0) r = r->next;
    if (!r) {
        r = calloc(1, sizeof(*r));
        if (!r) return 1;
        snprintf(r->key, sizeof(r->key), "%s", key);
        r->window_start = now_ms;
        r->next = *head;
        *head = r;
    }
    if (now_ms - r->window_start >= l->window_ms) {
        r->window_start = now_ms;
        r->hits = 0;
    }
    if (r->hits >= l->limit) return 0;
    r->hits++;
    return 1;
}
