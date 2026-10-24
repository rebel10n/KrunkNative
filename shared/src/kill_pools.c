// Krunker v10.0.3 event kill pools: getKillReward and the endGame draws (shared/include/kill_pools.h,
// doc/v10/kill-pools.md). deob-10.0.3.js:220807-220924 (getKillReward), :221797-221807 (endGame).
#include <kill_pools.h>
#include <pcg_basic.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static double kp_pcg(void *user) {
    (void) user;
    return ldexp((double) pcg32_random(), -32);
}

// days since 1970-01-01 of a proleptic Gregorian date (Howard Hinnant's days_from_civil)
static int64_t kp_days(int64_t y, const int m, const int d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

// new Date(startsAt).getTime(): epoch ms, or an ISO 8601 UTC date ("2026-10-24T18:00:00Z", ".sss" and a
// space for the T allowed). Unreadable: 0 (the event sorts first).
static double kp_time(const cJSON *v) {
    if (cJSON_IsNumber(v)) return v->valuedouble;
    const char *s = cJSON_GetStringValue(v);
    int y, mo, d, h = 0, mi = 0, sec = 0, ms = 0;
    if (!s || sscanf(s, "%d-%d-%d", &y, &mo, &d) != 3 || mo < 1 || mo > 12 || d < 1 || d > 31) return 0.0;
    const char *t = strpbrk(s, "T ");
    if (t) {
        sscanf(t + 1, "%d:%d:%d", &h, &mi, &sec);
        const char *dot = strchr(t, '.');
        if (dot) ms = atoi(dot + 1) % 1000;
    }
    return (double) ((kp_days(y, mo, d) * 86400 + h * 3600 + mi * 60 + sec) * 1000 + ms);
}

static double kp_num(const cJSON *o, const char *key) { // `x || 0`
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (cJSON_IsNumber(v)) return isfinite(v->valuedouble) ? v->valuedouble : 0.0;
    if (cJSON_IsString(v)) return atof(v->valuestring);
    return 0.0;
}

static int kp_int(const cJSON *v) { // parseInt(content_ref): NaN -> -1
    if (cJSON_IsNumber(v)) return isfinite(v->valuedouble) ? (int) v->valuedouble : -1;
    const char *s = cJSON_GetStringValue(v);
    if (!s) return -1;
    while (*s == ' ' || *s == '\t') s++;
    char *end;
    const long n = strtol(s, &end, 10);
    return end == s ? -1 : (int) n;
}

int kill_pools_parse(KillPoolEvents *out, const cJSON *events) {
    memset(out, 0, sizeof(*out));
    const cJSON *e;
    cJSON_ArrayForEach(e, events) {
        if (!cJSON_IsObject(e) || out->count >= KILL_POOL_EVENTS_MAX) continue;
        KillPoolEvent *ev = &out->events[out->count++];
        ev->id = kp_int(cJSON_GetObjectItemCaseSensitive(e, "id"));
        if (ev->id < 0) ev->id = 0;
        ev->starts_at_ms = kp_time(cJSON_GetObjectItemCaseSensitive(e, "startsAt"));
        ev->ends_at_ms = kp_time(cJSON_GetObjectItemCaseSensitive(e, "endsAt"));
        ev->weight = kp_num(e, "killPoolsWeight");
        const char *mode = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(e, "killPoolsMode"));
        ev->dilute = mode && !strcmp(mode, "dilute");
        const char *icon = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(e, "killPoolsIcon"));
        // ./img/<icon>.webp: a plain name (letters, digits, _ and -) keeps the markup and the URL inside img/
        if (icon && icon[0] && strlen(icon) < sizeof(ev->icon) && strspn(icon, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") == strlen(icon))
            snprintf(ev->icon, sizeof(ev->icon), "%s", icon);
        const cJSON *pool;
        cJSON_ArrayForEach(pool, cJSON_GetObjectItemCaseSensitive(e, "killPools")) {
            if (ev->pool_count >= KILL_POOL_POOLS_MAX) break;
            KillPool *p = &ev->pools[ev->pool_count++];
            p->weight = cJSON_IsObject(pool) ? kp_num(pool, "weight") : 0.0;
            const cJSON *contents = cJSON_IsObject(pool) ? cJSON_GetObjectItemCaseSensitive(pool, "contents") : NULL;
            const cJSON *c;
            cJSON_ArrayForEach(c, contents) {
                if (p->content_count >= KILL_POOL_CONTENTS_MAX) break;
                KillPoolContent *k = &p->contents[p->content_count++];
                const char *type = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(c, "content_type"));
                k->type = type && !strcmp(type, "skin") ? KILL_POOL_CONTENT_SKIN : KILL_POOL_CONTENT_OTHER;
                k->ref = kp_int(cJSON_GetObjectItemCaseSensitive(c, "content_ref"));
            }
        }
    }
    return out->count;
}

int kill_pools_event_index(const KillPoolEvents *events, const double now_ms) {
    for (int i = 0; events && i < events->count; i++) {
        const KillPoolEvent *e = &events->events[i];
        if (e->id > 0 && now_ms >= e->starts_at_ms && (e->ends_at_ms <= 0.0 || now_ms <= e->ends_at_ms)) return e->id;
    }
    return 0;
}

int kill_pools_any(const KillPoolEvents *events) {
    for (int i = 0; events && i < events->count; i++)
        if (events->events[i].pool_count > 0) return 1;
    return 0;
}

typedef struct {
    const KillPool *pool;
    const KillPoolEvent *event;
    double weight;
} KpEntry;

int kill_pools_draw(const KillPoolEvents *events, KillPoolRandom random, void *user, KillReward *out) {
    if (!events || !events->count) return 0; // !ongoingEvents || !ongoingEvents.length
    if (!random) random = kp_pcg;
    // filter(killPools.length > 0 && killPoolsWeight > 0).sort(startsAt): a stable insertion sort, as V8's
    const KillPoolEvent *sorted[KILL_POOL_EVENTS_MAX];
    int n = 0;
    for (int i = 0; i < events->count; i++) {
        const KillPoolEvent *e = &events->events[i];
        if (e->pool_count <= 0 || !(e->weight > 0.0)) continue;
        int at = n;
        while (at > 0 && sorted[at - 1]->starts_at_ms > e->starts_at_ms) {
            sorted[at] = sorted[at - 1];
            at--;
        }
        sorted[at] = e;
        n++;
    }
    if (!n) return 0;

    KpEntry entries[KILL_POOL_EVENTS_MAX * KILL_POOL_POOLS_MAX];
    int count = 0;
    double total = 0.0;
    for (int i = 0; i < n; i++) {
        const KillPoolEvent *e = sorted[i];
        const double share = e->weight / 100.0, rest = (100.0 - e->weight) / 100.0;
        if (e->dilute) { // the earlier events' entries shrink by the rest
            for (int k = 0; k < count; k++) entries[k].weight *= rest;
            total *= rest;
        }
        for (int p = 0; p < e->pool_count; p++) {
            const double w = e->pools[p].weight * share;
            if (w > 0.0) {
                entries[count++] = (KpEntry) {&e->pools[p], e, w};
                total += w;
            }
        }
    }
    if (!count || total <= 0.0) return 0;
    // under 100 in all, the rest of the 100 is "nothing": randFloat(0, 100) > total
    if (total < 100.0 && random(user) * 100.0 > total) return 0;
    const double r = random(user) * total; // randFloat(0, total)
    double acc = 0.0;
    const KpEntry *pick = NULL;
    for (int k = 0; k < count && !pick; k++) {
        acc += entries[k].weight;
        if (r <= acc) pick = &entries[k];
    }
    if (!pick || pick->pool->content_count <= 0) return 0;
    const int index = (int) floor(random(user) * pick->pool->content_count); // randInt(0, length - 1)
    const KillPoolContent *c = &pick->pool->contents[index < pick->pool->content_count ? index : pick->pool->content_count - 1];
    if (c->type != KILL_POOL_CONTENT_SKIN || c->ref < 0) return 0; // currency and the rest: null
    out->skin = c->ref;
    snprintf(out->icon, sizeof(out->icon), "%s", pick->event->icon);
    return 1;
}

int kill_pools_draw_kills(const KillPoolEvents *events, const unsigned int kills, KillPoolRandom random, void *user, KillReward *out,
                          const int max) {
    if (!kill_pools_any(events)) return 0;
    const unsigned int draws = kills < KILL_POOL_DRAWS_MAX ? kills : KILL_POOL_DRAWS_MAX;
    int got = 0;
    for (unsigned int i = 0; i < draws && got < max; i++)
        if (kill_pools_draw(events, random, user, &out[got])) got++;
    return got;
}
