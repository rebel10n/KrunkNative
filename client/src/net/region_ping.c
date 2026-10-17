// The region ping race (client/include/region_ping.h): v10.0.2's ClientManager.getRegion,
// v10.js:147469-147507 (pingFetch :147455-147467).
#include <region_ping.h>
#include <backend.h>
#include <GLFW/glfw3.h> // glfwGetTime: the measured round trips
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    RegionPingFn fn;
    void *user;
    int started, failed, done; // pings in flight / failed so far; done = fn ran
    int pending;               // callbacks still to come (the race is freed after the last)
} Race;

typedef struct {
    Race *race;
    char region[64];
} Ping;

static int starts_with(const char *s, const char *prefix) { return !strncmp(s, prefix, strlen(prefix)); }

static int ends_with(const char *s, const char *suffix) {
    const size_t n = strlen(s), m = strlen(suffix);
    return n >= m && !strcmp(s + n - m, suffix);
}

void region_ping_url(const char *value, char *out, const size_t n) {
    if (!out || !n) return;
    const char *v = value ? value : "";
    // v10: proto = endsWith(":443") ? "https:" : "http:"; anything but ip_ / localhost / 127.0.0.1
    // is https: with the port dropped. KrunkNative: a brain's "host:6767" stays plain HTTP.
    if (starts_with(v, "ip_") || starts_with(v, "localhost") || starts_with(v, "127.0.0.1") || ends_with(v, ":6767")) {
        snprintf(out, n, "%s//%s/ping", ends_with(v, ":443") ? "https:" : "http:", v);
    } else {
        snprintf(out, n, "https://%.*s/ping", (int) strcspn(v, ":"), v);
    }
}

static void finish(Race *r, const char *region) {
    if (r->done) return;
    r->done = 1;
    r->fn(r->user, region);
}

static void on_ping(void *user, const int status, const char *body, const size_t length) {
    (void) body;
    (void) length;
    Ping *p = user;
    Race *r = p->race;
    // fetch() resolves on any HTTP answer (v10's .then): only no answer at all (status 0: refused,
    // timed out, no route; BACKEND_HTTP_TLS_FAILED: the handshake) is a failed ping
    if (status > 0) {
        if (!r->done) printf("[mm] ping race: %s answered first\n", p->region);
        finish(r, p->region);
    } else if (++r->failed >= r->started) {
        if (!r->done) printf("[mm] ping race: all pings failed\n");
        finish(r, NULL);
    }
    free(p);
    if (--r->pending <= 0) free(r);
}

int region_ping_race(const cJSON *ping_list, const RegionPingEligible eligible, const RegionPingFn fn, void *user) {
    if (!cJSON_IsObject(ping_list) || !fn) return 0;
    Race *r = calloc(1, sizeof(*r));
    if (!r) return 0;
    r->fn = fn;
    r->user = user;
    r->pending = 1; // this call's own hold, so no callback frees the race while pings are still being started
    const cJSON *item;
    cJSON_ArrayForEach(item, ping_list) {
        if (!item->string || !cJSON_IsString(item) || !item->valuestring[0] || strlen(item->string) >= sizeof(((Ping *) 0)->region)) continue;
        if (eligible && !eligible(item->string)) continue;
        Ping *p = calloc(1, sizeof(*p));
        if (!p) continue;
        p->race = r;
        snprintf(p->region, sizeof(p->region), "%s", item->string);
        char url[512];
        region_ping_url(item->valuestring, url, sizeof(url));
        r->started++;
        r->pending++;
#ifdef __EMSCRIPTEN__
        const int queued = backend_http_request("GET", url, NULL, on_ping, p);
#else
        const int queued = backend_http_get_parallel(url, REGION_PING_TIMEOUT_MS, on_ping, p);
#endif
        if (!queued) {
            r->started--;
            r->pending--;
            free(p);
        }
    }
    const int started = r->started;
    if (--r->pending <= 0) free(r); // nothing in flight: fn is not called (0)
    if (started) printf("[mm] ping race: %d region%s\n", started, started == 1 ? "" : "s");
    return started;
}

// region_ping_measure: every listed region's /ping, timed (the server browser's pingData). Each ping
// keeps its start time; the last answer hands the {region: ms} object to fn.
typedef struct {
    RegionPingsFn fn;
    void *user;
    cJSON *results;
    int started, answered; // pings in flight / answered so far (a failed ping counts too)
    int pending;           // callbacks still to come, plus the call's own hold
} Measure;

typedef struct {
    Measure *measure;
    char region[64];
    double start_ms;
} Timed;

static void measure_release(Measure *m) {
    if (--m->pending > 0) return;
    if (m->answered >= m->started) m->fn(m->user, m->results); // all of them answered (or none started)
    else cJSON_Delete(m->results);
    free(m);
}

static void on_timed(void *user, const int status, const char *body, const size_t length) {
    (void) body;
    (void) length;
    Timed *t = user;
    Measure *m = t->measure;
    // a round trip for any HTTP answer; no answer at all (refused, timed out, no route) is -1 (v10's catch),
    // a failed TLS handshake REGION_PING_TLS_FAILED
    double ms = -1.0;
    if (status > 0) ms = glfwGetTime() * 1000.0 - t->start_ms;
    else if (status == BACKEND_HTTP_TLS_FAILED) ms = REGION_PING_TLS_FAILED;
    cJSON_AddNumberToObject(m->results, t->region, ms);
    m->answered++;
    free(t);
    measure_release(m);
}

int region_ping_measure(const cJSON *ping_list, const RegionPingsFn fn, void *user) {
    if (!cJSON_IsObject(ping_list) || !fn) return 0;
    Measure *m = calloc(1, sizeof(*m));
    if (!m) return 0;
    m->fn = fn;
    m->user = user;
    m->results = cJSON_CreateObject();
    m->pending = 1; // the call's own hold: no callback frees the measure while pings are still being started
    const cJSON *item;
    cJSON_ArrayForEach(item, ping_list) {
        if (!item->string || !cJSON_IsString(item) || !item->valuestring[0] || strlen(item->string) >= sizeof(((Timed *) 0)->region)) continue;
        Timed *t = calloc(1, sizeof(*t));
        if (!t) continue;
        t->measure = m;
        snprintf(t->region, sizeof(t->region), "%s", item->string);
        char url[512];
        region_ping_url(item->valuestring, url, sizeof(url));
        t->start_ms = glfwGetTime() * 1000.0;
        m->started++;
        m->pending++;
#ifdef __EMSCRIPTEN__
        const int queued = backend_http_request("GET", url, NULL, on_timed, t);
#else
        const int queued = backend_http_get_parallel(url, REGION_PING_TIMEOUT_MS, on_timed, t);
#endif
        if (!queued) {
            m->started--;
            m->pending--;
            free(t);
        }
    }
    if (!m->started) {
        cJSON_Delete(m->results);
        free(m);
        return 0;
    }
    measure_release(m); // drop the call's hold; the last answer runs fn
    return m->started;
}
