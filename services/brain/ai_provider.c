/*
 * services/brain/ai_provider.c - named AI provider profiles (ai_provider.h).
 *
 * "default" is the legacy upstream (ai.upstream_url / ai.upstream_key, KN_AI_UPSTREAM_*); a
 * configuration with only those keeps working unchanged. Counters and limits run on the loop
 * thread only. The keys live here (heap, zeroed on shutdown); ai.c copies a key into each upstream
 * transfer and never logs it.
 */
#include "ai_provider.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "knsvc/log.h"
#include "knsvc/loop.h"

extern char **environ;

#define MOD "ai"
#define PROV_MAX 8
#define PROV_NAME_MAX 16
#define PROV_RPM_RING 240 /* the rpm window keeps this many request times */
#define PROV_DAY_MS 86400000

enum { PC_EDITOR = 0, PC_BANTER, PC_MODERATION, PC_COUNT };
static const char *const kConsumerNames[PC_COUNT] = {"editor", "banter", "moderation"};

typedef struct {
    AiProvider p;                 /* p.name / p.url / p.model point into this slot; p.key = key or NULL */
    char name[PROV_NAME_MAX];
    char url[512];
    char model[96];
    char *key;                    /* heap, zeroed on shutdown */
    int64_t day;                  /* UTC day of the counters */
    int64_t requests_today, tokens_today;
    uint64_t stamps[PROV_RPM_RING];
    int head, count;
} Slot;

typedef struct {
    int slot;                     /* index of the routed profile; -1 = none */
    char provider[PROV_NAME_MAX]; /* the routed profile's name */
    int64_t day;
    int64_t requests_today, tokens_today;
} Consumer;

static struct {
    Slot s[PROV_MAX];
    int n;
    Consumer c[PC_COUNT];
} P;

/* ------------------------------------------------------------------ names */

static int valid_profile_name(const char *n) {
    const size_t len = strlen(n);
    if (!len || len >= PROV_NAME_MAX || n[0] < 'a' || n[0] > 'z') return 0;
    for (size_t i = 0; i < len; i++) {
        if (!((n[i] >= 'a' && n[i] <= 'z') || (n[i] >= '0' && n[i] <= '9'))) return 0;
    }
    return strcmp(n, "upstream") != 0; /* KN_AI_UPSTREAM_* are the legacy default */
}

static int consumer_index(const char *name) {
    if (!name) return -1;
    for (int i = 0; i < PC_COUNT; i++) {
        if (!strcmp(kConsumerNames[i], name)) return i;
    }
    return -1;
}

static int slot_index_of_name(const char *name) {
    for (int i = 0; i < P.n; i++) {
        if (!strcmp(P.s[i].name, name)) return i;
    }
    return -1;
}

static int slot_index_of(const AiProvider *p) {
    for (int i = 0; p && i < P.n; i++) {
        if (&P.s[i].p == p) return i;
    }
    return -1;
}

static int slot_add(const char *name) {
    const int have = slot_index_of_name(name);
    if (have >= 0) return have;
    if (P.n >= PROV_MAX) {
        KN_WARN(MOD, "too many AI provider profiles (max %d): %s ignored", PROV_MAX, name);
        return -1;
    }
    Slot *s = &P.s[P.n];
    memset(s, 0, sizeof(*s));
    snprintf(s->name, sizeof(s->name), "%s", name);
    s->p.name = s->name;
    s->p.url = s->url;
    s->p.model = s->model;
    return P.n++;
}

/* KN_AI_<NAME>_<suffix> for a profile name (upper-cased). */
static void env_name(const char *name, const char *suffix, char *out, size_t cap) {
    size_t o = (size_t) snprintf(out, cap, "KN_AI_");
    for (const char *q = name; *q && o + 1 < cap; q++) out[o++] = (char) toupper((unsigned char) *q);
    snprintf(out + o, cap - o, "_%s", suffix);
}

static const char *env_text(const char *name, const char *suffix) {
    char var[64];
    env_name(name, suffix, var, sizeof(var));
    const char *t = getenv(var);
    return t && *t ? t : NULL;
}

/* ------------------------------------------------------------------ counters */

static void roll_day(int64_t *day, int64_t *requests, int64_t *tokens) {
    const int64_t d = kn_wall_ms() / PROV_DAY_MS;
    if (*day != d) {
        *day = d;
        *requests = 0;
        *tokens = 0;
    }
}

/* drop the request times older than a minute */
static void prune(Slot *s, const uint64_t now) {
    while (s->count && now - s->stamps[(s->head - s->count + PROV_RPM_RING) % PROV_RPM_RING] >= 60000) s->count--;
}

static Consumer *consumer_for(const char *consumer) {
    const int ci = consumer_index(consumer);
    if (ci < 0) return NULL;
    Consumer *c = &P.c[ci];
    roll_day(&c->day, &c->requests_today, &c->tokens_today);
    return c;
}

/* ------------------------------------------------------------------ load */

/* Fill one registered profile from the environment, the file and the legacy keys. */
static void profile_fill(const kn_config *cfg, const int i) {
    Slot *s = &P.s[i];
    const int dflt = !strcmp(s->name, "default");
    char path[128];
    const char *url = env_text(s->name, "URL");
    if (!url) {
        snprintf(path, sizeof(path), "ai.providers.%s.url", s->name);
        url = kn_config_str(cfg, path, "");
    }
    if ((!url || !*url) && dflt) url = kn_config_str(cfg, "ai.upstream_url", "");
    const char *key = env_text(s->name, "KEY");
    if (!key) {
        snprintf(path, sizeof(path), "ai.providers.%s.key", s->name);
        key = kn_config_str(cfg, path, "");
    }
    if ((!key || !*key) && dflt) key = kn_config_str(cfg, "ai.upstream_key", "");
    snprintf(path, sizeof(path), "ai.providers.%s.model", s->name);
    snprintf(s->model, sizeof(s->model), "%s", kn_config_str(cfg, path, ""));
    snprintf(path, sizeof(path), "ai.providers.%s.rpm", s->name);
    int64_t rpm = kn_config_int(cfg, path, 0);
    if (rpm > PROV_RPM_RING) {
        KN_WARN(MOD, "ai.providers.%s.rpm %lld: capped at %d", s->name, (long long) rpm, PROV_RPM_RING);
        rpm = PROV_RPM_RING;
    }
    s->p.limit.rpm = rpm < 0 ? 0 : (int) rpm;
    snprintf(path, sizeof(path), "ai.providers.%s.daily_budget_tokens", s->name);
    s->p.limit.daily_budget_tokens = kn_config_int(cfg, path, 0);
    if (s->p.limit.daily_budget_tokens < 0) s->p.limit.daily_budget_tokens = 0;

    /* the url: an http(s) base without a trailing slash; the key stays out of every message */
    snprintf(s->url, sizeof(s->url), "%s", url ? url : "");
    size_t ul = strlen(s->url);
    while (ul && s->url[ul - 1] == '/') s->url[--ul] = 0;
    if (s->url[0] && strncmp(s->url, "http://", 7) && strncmp(s->url, "https://", 8)) {
        KN_WARN(MOD, "AI provider %s: the url must start with http:// or https://: profile not configured", s->name);
        s->url[0] = 0;
    }
    if (s->key) {
        memset(s->key, 0, strlen(s->key));
        free(s->key);
        s->key = NULL;
    }
    if (key && *key) s->key = strdup(key);
    s->p.key = NULL;
    if (s->url[0] && s->key) {
        s->p.key = s->key;
        KN_INFO(MOD, "AI provider %s: configured (model %s, rpm %d, daily budget %lld tokens)", s->name,
                s->model[0] ? s->model : "the consumer's choice", s->p.limit.rpm, (long long) s->p.limit.daily_budget_tokens);
    } else if (!dflt || s->url[0] || (key && *key)) {
        KN_WARN(MOD, "AI provider %s: not configured (%s)", s->name, !s->url[0] ? "no url" : "no key");
    }
}

/* Names from the file and the environment (names the routing or the env can name). */
static void collect_names(const kn_config *cfg) {
    const cJSON *providers = kn_config_get(cfg, "ai.providers");
    if (cJSON_IsObject(providers)) {
        for (const cJSON *c = providers->child; c; c = c->next) {
            if (!c->string) continue;
            if (valid_profile_name(c->string)) slot_add(c->string);
            else KN_WARN(MOD, "ai.providers.%s: not a profile name (a-z then a-z0-9, up to 15, not 'upstream')", c->string);
        }
    }
    for (char **e = environ; e && *e; e++) {
        if (strncmp(*e, "KN_AI_", 6) != 0) continue;
        const char *eq = strchr(*e, '=');
        if (!eq) continue;
        const char *k = *e + 6; /* NAME_URL or NAME_KEY */
        const size_t kl = (size_t) (eq - k);
        if (kl <= 4 || (memcmp(k + kl - 4, "_URL", 4) && memcmp(k + kl - 4, "_KEY", 4))) continue;
        const size_t nl = kl - 4;
        if (nl >= PROV_NAME_MAX) continue;
        char name[PROV_NAME_MAX];
        for (size_t i = 0; i < nl; i++) name[i] = (char) tolower((unsigned char) k[i]);
        name[nl] = 0;
        if (valid_profile_name(name)) slot_add(name);
    }
}

static void route_load(const kn_config *cfg) {
    for (int ci = 0; ci < PC_COUNT; ci++) {
        Consumer *c = &P.c[ci];
        char path[64];
        snprintf(path, sizeof(path), "ai.%s.provider", kConsumerNames[ci]);
        const char *name = kn_config_str(cfg, path, "default");
        snprintf(c->provider, sizeof(c->provider), "%s", name && *name ? name : "default");
        c->slot = -1;
        const int i = slot_index_of_name(c->provider);
        if (i < 0) {
            if (strcmp(c->provider, "default")) KN_ERROR(MOD, "%s: no AI provider profile named %s: the %s feature is off", path, c->provider, kConsumerNames[ci]);
        } else if (!P.s[i].p.key) {
            if (strcmp(c->provider, "default")) KN_WARN(MOD, "%s: profile %s is not configured: the %s feature is off", path, c->provider, kConsumerNames[ci]);
        } else {
            c->slot = i;
        }
    }
}

void ai_provider_shutdown(void) {
    for (int i = 0; i < P.n; i++) {
        if (P.s[i].key) {
            memset(P.s[i].key, 0, strlen(P.s[i].key));
            free(P.s[i].key);
        }
    }
    memset(&P, 0, sizeof(P));
    for (int ci = 0; ci < PC_COUNT; ci++) P.c[ci].slot = -1;
}

void ai_provider_load(const kn_config *cfg) {
    ai_provider_shutdown();
    slot_add("default");
    collect_names(cfg);
    for (int i = 0; i < P.n; i++) profile_fill(cfg, i);
    route_load(cfg);
}

/* ------------------------------------------------------------------ lookups and limits */

const AiProvider *ai_provider_get(const char *consumer) {
    const int ci = consumer_index(consumer);
    if (ci < 0 || P.c[ci].slot < 0 || P.c[ci].slot >= P.n) return NULL;
    return &P.s[P.c[ci].slot].p;
}

int ai_provider_check(const AiProvider *p, char *msg, const size_t n, int *retry_s) {
    *retry_s = 0;
    const int i = slot_index_of(p);
    if (i < 0) return 0;
    Slot *s = &P.s[i];
    roll_day(&s->day, &s->requests_today, &s->tokens_today);
    const uint64_t now = kn_now_ms();
    prune(s, now);
    if (s->p.limit.rpm > 0 && s->count >= s->p.limit.rpm) {
        const uint64_t oldest = s->stamps[(s->head - s->count + PROV_RPM_RING) % PROV_RPM_RING];
        *retry_s = (int) ((60000 - (now - oldest)) / 1000) + 1;
        snprintf(msg, n, "Too many AI requests this minute (limit %d): try again in %d s", s->p.limit.rpm, *retry_s);
        return -1;
    }
    if (s->p.limit.daily_budget_tokens > 0 && s->tokens_today >= s->p.limit.daily_budget_tokens) {
        snprintf(msg, n, "Daily AI token budget used up (%lld tokens): it resets at 00:00 UTC", (long long) s->p.limit.daily_budget_tokens);
        *retry_s = (int) ((PROV_DAY_MS - kn_wall_ms() % PROV_DAY_MS) / 1000);
        return -1;
    }
    return 0;
}

void ai_provider_begin(const AiProvider *p, const char *consumer) {
    const int i = slot_index_of(p);
    if (i >= 0) {
        Slot *s = &P.s[i];
        roll_day(&s->day, &s->requests_today, &s->tokens_today);
        const uint64_t now = kn_now_ms();
        prune(s, now);
        s->stamps[s->head] = now;
        s->head = (s->head + 1) % PROV_RPM_RING;
        if (s->count < PROV_RPM_RING) s->count++;
        s->requests_today++;
    }
    Consumer *c = consumer_for(consumer);
    if (c) c->requests_today++;
}

void ai_provider_charge(const AiProvider *p, const char *consumer, const int64_t tokens) {
    if (tokens <= 0) return;
    const int i = slot_index_of(p);
    if (i >= 0) {
        Slot *s = &P.s[i];
        roll_day(&s->day, &s->requests_today, &s->tokens_today);
        s->tokens_today += tokens;
    }
    Consumer *c = consumer_for(consumer);
    if (c) c->tokens_today += tokens;
}

/* ------------------------------------------------------------------ status and dumps */

void ai_provider_status_add(cJSON *o) {
    const uint64_t now = kn_now_ms();
    cJSON *provs = cJSON_AddObjectToObject(o, "providers");
    for (int i = 0; i < P.n; i++) {
        Slot *s = &P.s[i];
        roll_day(&s->day, &s->requests_today, &s->tokens_today);
        prune(s, now);
        cJSON *r = cJSON_AddObjectToObject(provs, s->name);
        cJSON_AddBoolToObject(r, "configured", s->p.key != NULL);
        if (s->model[0]) cJSON_AddStringToObject(r, "model", s->model);
        cJSON_AddNumberToObject(r, "rpm", s->p.limit.rpm);
        cJSON_AddNumberToObject(r, "daily_budget_tokens", (double) s->p.limit.daily_budget_tokens);
        cJSON_AddNumberToObject(r, "requests_last_minute", s->count);
        cJSON_AddNumberToObject(r, "requests_today", (double) s->requests_today);
        cJSON_AddNumberToObject(r, "tokens_today", (double) s->tokens_today);
    }
    cJSON *cons = cJSON_AddObjectToObject(o, "consumers");
    for (int ci = 0; ci < PC_COUNT; ci++) {
        Consumer *c = consumer_for(kConsumerNames[ci]);
        cJSON *r = cJSON_AddObjectToObject(cons, kConsumerNames[ci]);
        cJSON_AddStringToObject(r, "provider", c->provider);
        cJSON_AddBoolToObject(r, "configured", c->slot >= 0);
        cJSON_AddNumberToObject(r, "requests_today", (double) c->requests_today);
        cJSON_AddNumberToObject(r, "tokens_today", (double) c->tokens_today);
    }
}

/* scheme://host[:port] only: no path, no query, no user info */
static void origin_of(const char *url, char *out, const size_t n) {
    const char *scheme = strstr(url, "://");
    if (!scheme) {
        snprintf(out, n, "%s", url);
        return;
    }
    const char *host = scheme + 3;
    const char *slash = strchr(host, '/');
    const char *at = strchr(host, '@');
    if (at && (!slash || at < slash)) host = at + 1; /* drop "user:secret@" */
    const size_t hl = slash && slash > host ? (size_t) (slash - host) : strlen(host);
    snprintf(out, n, "%.*s%.*s", (int) (scheme + 3 - url), url, (int) hl, host);
}

char *ai_provider_dump(void) {
    cJSON *o = cJSON_CreateObject();
    cJSON *provs = cJSON_AddObjectToObject(o, "providers");
    for (int i = 0; i < P.n; i++) {
        const Slot *s = &P.s[i];
        cJSON *r = cJSON_AddObjectToObject(provs, s->name);
        char origin[512];
        origin_of(s->url, origin, sizeof(origin));
        cJSON_AddStringToObject(r, "url", origin);
        cJSON_AddStringToObject(r, "key", s->key ? "***" : "");
        if (s->model[0]) cJSON_AddStringToObject(r, "model", s->model);
        cJSON_AddNumberToObject(r, "rpm", s->p.limit.rpm);
        cJSON_AddNumberToObject(r, "daily_budget_tokens", (double) s->p.limit.daily_budget_tokens);
    }
    cJSON *cons = cJSON_AddObjectToObject(o, "consumers");
    for (int ci = 0; ci < PC_COUNT; ci++) cJSON_AddStringToObject(cons, kConsumerNames[ci], P.c[ci].provider);
    char *text = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return text;
}
