/*
 * services/brain/finder_table.c - finder server table (finder_table.h).
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "finder_table.h"
#include "knsvc/json.h"

struct FinderTable {
    FinderConfig cfg;
    FinderEntry **items;
    size_t count, cap;
};

static const uint8_t k_prefix[4] = {0xFF, 0xFF, 0xFF, 0xFF};

void finder_config_defaults(FinderConfig *c) {
    c->poll_interval_ms = 30000;
    c->poll_timeout_ms = 3000;
    c->max_failures = 3;
    c->expire_ms = 180000;
    c->max_per_ip = 64;
    c->heartbeat_interval_s = 60;
}

FinderTable *finder_table_new(const FinderConfig *cfg) {
    FinderTable *t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    if (cfg) t->cfg = *cfg;
    else finder_config_defaults(&t->cfg);
    return t;
}

static void entry_free(FinderEntry *e, void (*free_addr)(void *)) {
    if (!e) return;
    cJSON_Delete(e->info);
    if (free_addr) free_addr(e->addr);
    else free(e->addr);
    free(e);
}

void finder_table_free(FinderTable *t) {
    if (!t) return;
    for (size_t i = 0; i < t->count; i++) entry_free(t->items[i], NULL);
    free(t->items);
    free(t);
}

const FinderConfig *finder_table_config(const FinderTable *t) { return &t->cfg; }
size_t finder_table_count(const FinderTable *t) { return t ? t->count : 0; }
FinderEntry *finder_table_at(FinderTable *t, size_t i) { return t && i < t->count ? t->items[i] : NULL; }

static FinderEntry *find(FinderTable *t, const char *host, int port, size_t *index) {
    for (size_t i = 0; i < t->count; i++) {
        FinderEntry *e = t->items[i];
        if (e->port == port && !strcasecmp(e->host, host)) {
            if (index) *index = i;
            return e;
        }
    }
    return NULL;
}

static void remove_at(FinderTable *t, size_t i, void (*free_addr)(void *)) {
    entry_free(t->items[i], free_addr);
    t->items[i] = t->items[--t->count];
}

/* host names / IPs only: letters, digits, '.', '-', ':' (IPv6), no spaces or schemes */
static int valid_host(const char *h) {
    if (!h || !*h || strlen(h) >= FINDER_HOST_MAX) return 0;
    for (const char *p = h; *p; p++) {
        if (!(isalnum((unsigned char) *p) || *p == '.' || *p == '-' || *p == ':')) return 0;
    }
    return 1;
}

static void copy_clean(char *dst, size_t cap, const char *src) {
    size_t o = 0;
    for (; src && *src && o + 1 < cap; src++) {
        if ((unsigned char) *src >= 0x20) dst[o++] = *src;
    }
    dst[o] = 0;
}

FinderEntry *finder_heartbeat(FinderTable *t, const char *host, int port, int query_port, const char *name,
                              const char *community, int online_mode, const char *submitter, uint64_t now_ms,
                              int *created, const char **err) {
    *created = 0;
    *err = NULL;
    if (!valid_host(host)) {
        *err = "invalid host";
        return NULL;
    }
    if (port <= 0 || port > 65535) {
        *err = "invalid port";
        return NULL;
    }
    if (query_port <= 0 || query_port > 65535) query_port = port;
    FinderEntry *e = find(t, host, port, NULL);
    if (!e) {
        size_t mine = 0;
        for (size_t i = 0; i < t->count; i++) mine += !strcmp(t->items[i]->submitter, submitter ? submitter : "");
        if (t->cfg.max_per_ip > 0 && (int) mine >= t->cfg.max_per_ip) {
            *err = "too many servers from this address";
            return NULL;
        }
        if (t->count == t->cap) {
            size_t cap = t->cap ? t->cap * 2 : 32;
            FinderEntry **grown = realloc(t->items, cap * sizeof(*grown));
            if (!grown) {
                *err = "out of memory";
                return NULL;
            }
            t->items = grown;
            t->cap = cap;
        }
        e = calloc(1, sizeof(*e));
        if (!e) {
            *err = "out of memory";
            return NULL;
        }
        snprintf(e->host, sizeof(e->host), "%s", host);
        e->port = port;
        e->ping_ms = -1;
        e->next_poll_ms = now_ms; /* poll right away */
        t->items[t->count++] = e;
        *created = 1;
    }
    if (e->query_port != query_port) {
        free(e->addr); /* re-resolve */
        e->addr = NULL;
    }
    e->query_port = query_port;
    snprintf(e->submitter, sizeof(e->submitter), "%s", submitter ? submitter : "");
    copy_clean(e->hb_name, sizeof(e->hb_name), name);
    copy_clean(e->hb_community, sizeof(e->hb_community), community);
    e->hb_online_mode = online_mode ? 1 : 0;
    e->last_heartbeat_ms = now_ms;
    return e;
}

int finder_remove(FinderTable *t, const char *host, int port, const char *submitter) {
    size_t i;
    FinderEntry *e = find(t, host ? host : "", port, &i);
    if (!e || strcmp(e->submitter, submitter ? submitter : "") != 0) return 0;
    remove_at(t, i, NULL);
    return 1;
}

size_t finder_build_query(const char *nonce, uint8_t *out, size_t cap) {
    const size_t nlen = nonce ? strlen(nonce) : 0;
    const size_t need = 4 + 6 + (nlen ? 1 + nlen : 0);
    if (cap < need || nlen > 32) return 0;
    memcpy(out, k_prefix, 4);
    memcpy(out + 4, "KNINFO", 6);
    if (nlen) {
        out[10] = 0;
        memcpy(out + 11, nonce, nlen);
    }
    return need;
}

cJSON *finder_parse_reply(const uint8_t *data, size_t len) {
    if (len < 12 || memcmp(data, k_prefix, 4) != 0 || memcmp(data + 4, "KNinfo", 6) != 0 || data[10] != 0)
        return NULL;
    const size_t jlen = len - 11;
    if (jlen > FINDER_INFO_MAX) return NULL;
    cJSON *o = kn_json_parse((const char *) data + 11, jlen);
    if (!cJSON_IsObject(o)) {
        cJSON_Delete(o);
        return NULL;
    }
    return o;
}

size_t finder_due_polls(FinderTable *t, uint64_t now_ms, FinderEntry **out, size_t max) {
    size_t n = 0;
    for (size_t i = 0; i < t->count && n < max; i++) {
        FinderEntry *e = t->items[i];
        if (!e->nonce[0] && now_ms >= e->next_poll_ms) out[n++] = e;
    }
    return n;
}

void finder_poll_stamp(FinderTable *t, FinderEntry *e, const char *nonce, uint64_t now_ms) {
    (void) t;
    snprintf(e->nonce, sizeof(e->nonce), "%s", nonce ? nonce : "");
    e->poll_sent_ms = now_ms;
}

int finder_poll_reply(FinderTable *t, FinderEntry *e, cJSON *info, uint64_t now_ms, int64_t wall_s) {
    const char *nonce = kn_json_str(info, "nonce", "");
    if (!e->nonce[0] || strcmp(nonce, e->nonce) != 0 || strcmp(kn_json_str(info, "game", ""), "krunknative") != 0) {
        cJSON_Delete(info);
        return 0;
    }
    cJSON_Delete(e->info);
    e->info = info;
    e->last_ok_ms = now_ms;
    e->last_ok_wall_s = wall_s;
    e->ping_ms = (int) (now_ms - e->poll_sent_ms);
    e->failures = 0;
    e->nonce[0] = 0;
    e->next_poll_ms = now_ms + (uint64_t) t->cfg.poll_interval_ms;
    return 1;
}

size_t finder_sweep(FinderTable *t, uint64_t now_ms, void (*free_addr)(void *)) {
    size_t removed = 0;
    for (size_t i = 0; i < t->count;) {
        FinderEntry *e = t->items[i];
        if (e->nonce[0] && now_ms - e->poll_sent_ms >= (uint64_t) t->cfg.poll_timeout_ms) {
            e->nonce[0] = 0;
            e->failures++;
            /* retry sooner than a full interval: a lost datagram should not delist for long */
            const uint64_t retry = (uint64_t) t->cfg.poll_interval_ms / 6;
            e->next_poll_ms = now_ms + (retry < 1000 ? 1000 : retry);
        }
        const int dead = e->failures >= t->cfg.max_failures ||
                         now_ms - e->last_heartbeat_ms >= (uint64_t) t->cfg.expire_ms;
        if (dead) {
            remove_at(t, i, free_addr);
            removed++;
        } else {
            i++;
        }
    }
    return removed;
}

/* ---------------------------------------------------------------- listing */

static int ieq(const char *a, const char *b) { return a && b && !strcasecmp(a, b); }

static int icontains(const char *hay, const char *needle) {
    if (!needle || !*needle) return 1;
    if (!hay) return 0;
    const size_t n = strlen(needle);
    for (; *hay; hay++) {
        if (!strncasecmp(hay, needle, n)) return 1;
    }
    return 0;
}

static int has_tag(const cJSON *info, const char *tag) {
    const cJSON *tags = cJSON_GetObjectItemCaseSensitive(info, "tags"), *it;
    cJSON_ArrayForEach(it, tags) {
        if (cJSON_IsString(it) && !strcasecmp(it->valuestring, tag)) return 1;
    }
    return 0;
}

/* The flavor pool a listing is for: a base client (no flavor, or "base") must never see Strike
 * lobbies, and a Strike client sees only Strike lobbies. A server advertises its flavor in the
 * KNINFO reply; absent = the base game. "all"/"*" lifts the filter (dashboards). */
static int flavor_ok(const cJSON *info, const char *want) {
    if (want && (!strcasecmp(want, "all") || !strcmp(want, "*"))) return 1;
    const char *have = kn_json_str(info, "flavor", "");
    if (!have[0]) have = "base"; /* absent = base */
    if (!want || !*want) want = "base";
    return !strcasecmp(have, want);
}

static int matches(const FinderEntry *e, const FinderFilter *f) {
    const cJSON *info = e->info;
    if (!flavor_ok(info, f->flavor)) return 0;
    if (f->map && *f->map && !ieq(kn_json_str(info, "map", NULL), f->map)) return 0;
    if (f->mode && *f->mode) {
        char id[16];
        snprintf(id, sizeof(id), "%lld", (long long) kn_json_int(info, "modeId", -1));
        if (!ieq(kn_json_str(info, "mode", NULL), f->mode) && strcmp(id, f->mode) != 0) return 0;
    }
    if (f->tag && *f->tag && !has_tag(info, f->tag)) return 0;
    if (f->community && *f->community && !ieq(kn_json_str(info, "community", NULL), f->community)) return 0;
    if (f->region && *f->region && !ieq(kn_json_str(info, "region", NULL), f->region)) return 0;
    if (f->q && *f->q && !icontains(kn_json_str(info, "name", NULL), f->q)) return 0;
    if (f->space && kn_json_int(info, "players", 0) >= kn_json_int(info, "max", 0)) return 0;
    if (f->online_mode >= 0 && (kn_json_bool(info, "online_mode", 0) ? 1 : 0) != f->online_mode) return 0;
    return 1;
}

static void copy_str(cJSON *dst, const cJSON *info, const char *key) {
    const char *v = kn_json_str(info, key, "");
    char clean[128];
    copy_clean(clean, sizeof(clean), v);
    cJSON_AddStringToObject(dst, key, clean);
}

/* host:port of e is one of the comma-separated entries of list */
static int excluded(const FinderEntry *e, const char *list) {
    char want[FINDER_HOST_MAX + 16];
    const int n = snprintf(want, sizeof(want), "%s:%d", e->host, e->port);
    if (n <= 0 || (size_t) n >= sizeof(want)) return 0;
    for (const char *p = list; p && *p;) {
        const char *end = strchr(p, ',');
        const size_t len = end ? (size_t) (end - p) : strlen(p);
        if (len == (size_t) n && !strncmp(p, want, len)) return 1;
        p = end ? end + 1 : NULL;
    }
    return 0;
}

cJSON *finder_list(FinderTable *t, const FinderFilter *f, uint64_t now_ms) {
    cJSON *arr = cJSON_CreateArray();
    const int limit = f->limit > 0 && f->limit < 500 ? f->limit : 500;
    /* fresh = verified by a poll within two intervals plus the timeout */
    const uint64_t fresh = (uint64_t) t->cfg.poll_interval_ms * 2 + (uint64_t) t->cfg.poll_timeout_ms;
    const char *exclude = f->exclude && *f->exclude ? f->exclude : NULL;
    /* an exclude list (the servers a seek leaves) is never listed: a seek must not be sent back */
    int n = 0;
    for (size_t i = 0; i < t->count && n < limit; i++) {
        const FinderEntry *e = t->items[i];
        if (!e->info || now_ms - e->last_ok_ms > fresh || !matches(e, f)) continue;
        if (excluded(e, exclude)) continue;
        const cJSON *info = e->info;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "host", e->host);
        cJSON_AddNumberToObject(o, "port", e->port);
        cJSON_AddNumberToObject(o, "query_port", e->query_port);
        copy_str(o, info, "name");
        copy_str(o, info, "map");
        copy_str(o, info, "mode");
        cJSON_AddNumberToObject(o, "modeId", (double) kn_json_int(info, "modeId", 0));
        cJSON_AddNumberToObject(o, "players", (double) kn_json_int(info, "players", 0));
        cJSON_AddNumberToObject(o, "max", (double) kn_json_int(info, "max", 0));
        /* bot players (server.cfg bot_quota): never in "players", so bot-filled servers do not look full */
        cJSON_AddNumberToObject(o, "bots", (double) kn_json_int(info, "bots", 0));
        cJSON *tags = cJSON_AddArrayToObject(o, "tags");
        const cJSON *it;
        cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(info, "tags")) {
            if (cJSON_IsString(it) && cJSON_GetArraySize(tags) < 16) {
                char clean[32];
                copy_clean(clean, sizeof(clean), it->valuestring);
                cJSON_AddItemToArray(tags, cJSON_CreateString(clean));
            }
        }
        copy_str(o, info, "community");
        cJSON_AddBoolToObject(o, "online_mode", kn_json_bool(info, "online_mode", 0));
        cJSON_AddBoolToObject(o, "password", kn_json_bool(info, "password", 0));
        copy_str(o, info, "region");
        copy_str(o, info, "version");
        /* build identity (server_info.h): releases.c turns it into outdated / incompatible */
        copy_str(o, info, "commit");
        cJSON_AddNumberToObject(o, "commit_time", (double) kn_json_int(info, "commit_time", 0));
        copy_str(o, info, "build_time");
        copy_str(o, info, "protocol");
        copy_str(o, info, "ruleset"); /* the Krunker version (ruleset.h key, "v10"); absent = a server from before the version toggle */
        /* product flavor (flavor.h key, "strike"); absent = the base game, so base listings are
         * byte-identical to before Krunker Strike */
        if (kn_json_str(info, "flavor", "")[0]) copy_str(o, info, "flavor");
        cJSON_AddNumberToObject(o, "ping_ms", e->ping_ms);
        cJSON_AddNumberToObject(o, "last_seen", (double) e->last_ok_wall_s);
        cJSON_AddItemToArray(arr, o);
        n++;
    }
    return arr;
}
