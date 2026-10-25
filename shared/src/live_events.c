// Krunker v10.0.3 live events: config.events and its readers (shared/include/live_events.h, doc/v10/events.md).
#include <live_events.h>
#include <kill_pools.h> // kill_pools_time: the feed's dates
#include <pcg_basic.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// store.lootRars (deob-10.0.3.js:116770): getItemByWeights' weights when the event gives none
static const double k_loot_rars[LIVE_EVENT_RARITIES] = {0, 0, 70.395, 25.49, 4, 0.105, 0.01};

static double le_pcg(void *user) {
    (void) user;
    return ldexp((double) pcg32_random(), -32);
}

static int le_int(const cJSON *v, const int fallback) { // a number, or a string parseInt reads
    if (cJSON_IsNumber(v)) return isfinite(v->valuedouble) ? (int) v->valuedouble : fallback;
    const char *s = cJSON_GetStringValue(v);
    if (!s) return fallback;
    char *end;
    const long n = strtol(s, &end, 10);
    return end == s ? fallback : (int) n;
}

static int le_true(const cJSON *v) { // JS truthiness of a JSON value
    if (cJSON_IsBool(v)) return cJSON_IsTrue(v);
    if (cJSON_IsNumber(v)) return v->valuedouble != 0.0 && !isnan(v->valuedouble);
    if (cJSON_IsString(v)) return v->valuestring && v->valuestring[0];
    return v && !cJSON_IsNull(v);
}

// a date by either name (KrunkNative's startsAt / endsAt first, then v10's start / end)
static double le_time(const cJSON *e, const char *kn, const char *v10) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(e, kn);
    if (!v || cJSON_IsNull(v)) v = cJSON_GetObjectItemCaseSensitive(e, v10);
    return v && !cJSON_IsNull(v) ? kill_pools_time(v) : 0.0;
}

static void le_text(char *out, const size_t n, const char *s) { // control characters become spaces
    size_t k = 0;
    for (; s && *s && k + 1 < n; s++) out[k++] = (unsigned char) *s < 0x20 ? ' ' : *s;
    out[k] = 0;
}

static void le_desc(LiveEvent *ev, const cJSON *v) {
    ev->long_desc[0] = 0;
    if (cJSON_IsString(v)) {
        le_text(ev->long_desc, sizeof(ev->long_desc), v->valuestring);
        return;
    }
    if (!cJSON_IsArray(v)) return;
    ev->long_desc_list = 1;
    size_t used = 0;
    const cJSON *line;
    cJSON_ArrayForEach(line, v) {
        if (!cJSON_IsString(line) || used + 2 >= sizeof(ev->long_desc)) continue;
        if (used) ev->long_desc[used++] = '\n';
        le_text(ev->long_desc + used, sizeof(ev->long_desc) - used, line->valuestring);
        used = strlen(ev->long_desc);
    }
}

int live_events_parse(LiveEvents *out, const cJSON *events) {
    memset(out, 0, sizeof(*out));
    const cJSON *e;
    cJSON_ArrayForEach(e, events) {
        if (!cJSON_IsObject(e) || out->count >= LIVE_EVENTS_MAX) continue;
        LiveEvent *ev = &out->events[out->count++];
        ev->id = le_int(cJSON_GetObjectItemCaseSensitive(e, "id"), 0);
        if (ev->id < 0) ev->id = 0;
        le_text(ev->name, sizeof(ev->name), cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(e, "name")));
        ev->start_ms = le_time(e, "startsAt", "start");
        ev->end_ms = le_time(e, "endsAt", "end");
        const cJSON *reveal = cJSON_GetObjectItemCaseSensitive(e, "reveal");
        ev->reveal_ms = cJSON_IsNumber(reveal) && reveal->valuedouble > 0.0 ? reveal->valuedouble : 0.0;
        // imgExt goes into a URL: letters and digits only, else the default "png"
        const char *ext = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(e, "imgExt"));
        if (ext && ext[0] && strlen(ext) < sizeof(ev->img_ext) &&
            strspn(ext, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789") == strlen(ext))
            snprintf(ev->img_ext, sizeof(ev->img_ext), "%s", ext);
        le_desc(ev, cJSON_GetObjectItemCaseSensitive(e, "longDesc"));
        ev->no_challenges = (unsigned char) le_true(cJSON_GetObjectItemCaseSensitive(e, "noChallenges"));
        ev->limited_time_mode = (unsigned char) le_true(cJSON_GetObjectItemCaseSensitive(e, "limitedTimeMode"));
        ev->event_objects = (unsigned char) le_true(cJSON_GetObjectItemCaseSensitive(e, "eventObjects"));
        const cJSON *rewards = cJSON_GetObjectItemCaseSensitive(e, "rewards");
        const cJSON *skins = cJSON_IsObject(rewards) ? cJSON_GetObjectItemCaseSensitive(rewards, "skins") : NULL;
        ev->has_reward_skins = (unsigned char) cJSON_IsArray(skins);
        const cJSON *skin;
        cJSON_ArrayForEach(skin, (cJSON_IsArray(skins) ? skins : NULL)) {
            if (ev->reward_skin_count < LIVE_EVENT_SKINS_MAX) ev->reward_skins[ev->reward_skin_count++] = le_int(skin, -1);
        }
        const cJSON *weights = cJSON_IsObject(rewards) ? cJSON_GetObjectItemCaseSensitive(rewards, "weights") : NULL;
        if (cJSON_IsArray(weights)) {
            ev->has_weights = 1;
            int r = 0;
            const cJSON *w;
            cJSON_ArrayForEach(w, weights) {
                if (r < LIVE_EVENT_RARITIES) ev->reward_weights[r++] = cJSON_IsNumber(w) && isfinite(w->valuedouble) ? w->valuedouble : 0.0;
            }
        }
        const int chance = le_int(cJSON_GetObjectItemCaseSensitive(e, "rewardChance"), 0);
        ev->reward_chance = chance ? chance : 100; // rewardChance || 100
        const cJSON *maps = cJSON_GetObjectItemCaseSensitive(e, "eligibleMaps");
        if (cJSON_IsArray(maps)) {
            ev->has_eligible_maps = 1;
            const cJSON *m;
            cJSON_ArrayForEach(m, maps) {
                if (ev->eligible_map_count < LIVE_EVENT_MAPS_MAX && cJSON_IsNumber(m)) ev->eligible_maps[ev->eligible_map_count++] = m->valueint;
            }
        }
    }
    return out->count;
}

const LiveEvent *live_events_find(const LiveEvents *events, const int id) {
    for (int i = 0; events && id > 0 && i < events->count; i++)
        if (events->events[i].id == id) return &events->events[i];
    return NULL;
}

static int le_running(const LiveEvent *e, const double now_ms) { // now >= start && now <= end (no end: open-ended)
    return now_ms >= e->start_ms && (e->end_ms <= 0.0 || now_ms <= e->end_ms);
}

int live_events_index(const LiveEvents *events, const double now_ms) {
    for (int i = 0; events && i < events->count; i++)
        if (events->events[i].id > 0 && le_running(&events->events[i], now_ms)) return events->events[i].id;
    return 0;
}

int live_events_any_running(const LiveEvents *events, const double now_ms) {
    for (int i = 0; events && i < events->count; i++)
        if (le_running(&events->events[i], now_ms)) return 1;
    return 0;
}

const LiveEvent *live_events_active(const LiveEvents *events, const int id, const double now_ms) {
    const LiveEvent *found = live_events_find(events, id);
    if (found) return found;
    for (int i = 0; events && i < events->count; i++) {
        const LiveEvent *e = &events->events[i];
        if (e->reveal_ms <= 0.0) continue; // if (event.reveal)
        const double until_start = e->start_ms - now_ms;
        if ((e->end_ms <= 0.0 || e->end_ms - now_ms > 0.0) && until_start <= e->reveal_ms) return e;
    }
    return NULL;
}

int live_events_show_objects(const LiveEvents *events, const int event_index, const int spectating) {
    if (spectating || event_index <= 0) return 0;
    if (event_index == LIVE_EVENT_EGG_EVENT) return 1; // v10: eventIndex == 3
    const LiveEvent *e = live_events_find(events, event_index);
    return e && e->event_objects; // KrunkNative eventObjects
}

int live_event_map_eligible(const LiveEvent *event, const int map_index) {
    if (!event || !event->has_eligible_maps) return 1;
    for (int i = 0; i < event->eligible_map_count; i++)
        if (event->eligible_maps[i] == map_index) return 1;
    return 0;
}

void live_event_image(const LiveEvent *event, const int id, char *out, const size_t n) {
    snprintf(out, n, "img/events/event_%d.%s", event ? event->id : id, event && event->img_ext[0] ? event->img_ext : "png");
}

int live_events_pick_by_weights(const int *items, const int count, const double *weights, int (*rarity_of)(void *user, int skin),
                                void *user, LiveEventRandom random, void *random_user) {
    if (count <= 0 || !items || !rarity_of) return -1;
    if (!weights) weights = k_loot_rars;
    if (!random) random = le_pcg;
    int present[LIVE_EVENT_RARITIES] = {0}; // the Set of the items' rarities
    for (int i = 0; i < count; i++) {
        const int r = items[i] >= 0 ? rarity_of(user, items[i]) : -1;
        if (r >= 0 && r < LIVE_EVENT_RARITIES) present[r] = 1;
    }
    double total = 0.0;
    for (int r = 0; r < LIVE_EVENT_RARITIES; r++)
        if (present[r]) total += weights[r];
    if (total == 0.0) return -1;
    // randFloat(0.0001, total), then the first present rarity whose running sum passes it (rarity 0 when none does)
    const double roll = random(random_user) * (total - 0.0001) + 0.0001;
    int rarity = 0;
    double sum = 0.0;
    for (int r = 0; r < LIVE_EVENT_RARITIES; r++) {
        if (present[r] && roll < (sum += weights[r])) {
            rarity = r;
            break;
        }
    }
    int of_rarity[LIVE_EVENT_SKINS_MAX * 4];
    int n = 0;
    for (int i = 0; i < count && n < (int) (sizeof(of_rarity) / sizeof(*of_rarity)); i++)
        if (items[i] >= 0 && rarity_of(user, items[i]) == rarity) of_rarity[n++] = items[i];
    if (!n) return -1;
    return of_rarity[(int) floor(random(random_user) * n)]; // randInt(0, length - 1)
}

typedef struct {
    const LiveEventSkin *store;
    int count;
} LeStore;

static int le_store_rarity(void *user, const int skin) {
    const LeStore *s = user;
    return skin >= 0 && skin < s->count ? s->store[skin].rarity : -1;
}

int live_events_reward(const LiveEvent *event, const int event_index, const int map_index, const int season, const LiveEventSkin *store,
                       const int store_count, LiveEventOwns owns, void *owns_user, LiveEventRandom random, void *random_user) {
    if (!event || event_index <= 0 || !store || store_count <= 0) return -1;
    if (!random) random = le_pcg;
    if (!live_event_map_eligible(event, map_index)) return -1; // the filter's eligibleMaps term holds for every skin
    int candidates[LIVE_EVENT_SKINS_MAX * 4];
    int n = 0;
    const int cap = (int) (sizeof(candidates) / sizeof(*candidates));
    // store.skins.filter(eventReward == eventIndex && seas == currentSeason && eligible map)
    for (int i = 0; i < store_count && n < cap; i++)
        if (store[i].event_reward == event_index && store[i].season == season) candidates[n++] = i;
    // KrunkNative: the event's rewards.skins too (store skins, each once)
    for (int k = 0; k < event->reward_skin_count && n < cap; k++) {
        const int skin = event->reward_skins[k];
        int dup = skin < 0 || skin >= store_count;
        for (int j = 0; j < n && !dup; j++) dup = candidates[j] == skin;
        if (!dup) candidates[n++] = skin;
    }
    // randInt(1, 100) <= (rewardChance || 100), drawn only when there is a candidate
    if (!n || (int) floor(random(random_user) * 100.0) + 1 > event->reward_chance) return -1;
    int lacking[LIVE_EVENT_SKINS_MAX * 4];
    int m = 0;
    for (int i = 0; i < n; i++)
        if (!owns || !owns(owns_user, candidates[i])) lacking[m++] = candidates[i];
    if (!m) return -1;
    LeStore s = {store, store_count};
    return live_events_pick_by_weights(lacking, m, event->has_weights ? event->reward_weights : NULL, le_store_rarity, &s, random,
                                       random_user);
}

int live_events_limited_time_mode(const LiveEvents *events, const int event_index) {
    const LiveEvent *e = live_events_find(events, event_index);
    return e && e->limited_time_mode;
}

int live_events_map_objects(Map *map, const int custom) {
    int count = 0;
    for (size_t i = 0; map && i < map->object_count; i++) {
        Object *object = map->objects[i];
        if (!object || object->prefab != PREFAB_EVENT) continue;
        count++;
        // A custom game's eggs are inert. Restore an ordinary public map object when the same client later
        // enters a public game with this map; a destroyed or initially inactive object stays inactive.
        object->active = custom || object->start_closed || object->start_destroyed || object->start_inactive || object->destroyed ? 0 : 1;
    }
    return count;
}
