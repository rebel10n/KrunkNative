// Community maps: the published-map download and the Host Game seam (community_maps.h,
// community_host.h).
//
// community_fetch(): a published map is fetched once from the brain (GET /api/map-data?name=),
// checked with map_prepare_custom (v10 prepareGame: the parentheses, then validateMap) and kept in
// a session cache, so a Play click, the Host pick and a second pick share one copy. A map that does
// not validate is refused with v10's message (the brain validated it at publish; an older row may
// not pass). Without a brain a published map cannot be had; the catalogue still works offline.

#include <client.h>
#include "klog.h"
#include <community_host.h>
#include <backend.h>
#include <i18n.h>
#include <map_catalog.h>
#include <map_validate.h>
#include <ui_logic.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define CACHE_MAX 64

typedef struct {
    char name[96];
    cJSON *json; // owned: the checked map
} CachedMap;

static CachedMap g_cache[CACHE_MAX];
static size_t g_cache_count;

static const cJSON *cache_find(const char *name) {
    for (size_t i = 0; i < g_cache_count; i++) {
        if (!strcasecmp(g_cache[i].name, name)) return g_cache[i].json;
    }
    return NULL;
}

// ---- community_fetch

typedef struct {
    char name[96];
    CommunityFetchFn done;
    void *user;
} FetchReq;

static void on_map_data(void *user, const int status, const char *body, const size_t length) {
    FetchReq *req = user;
    const char *error = I18N("matchmaker.InvalidGameId", "Game not found.");
    cJSON *json = NULL;
    char *text = status == 200 && body && length ? malloc(length + 1) : NULL;
    if (text) {
        memcpy(text, body, length);
        text[length] = 0;
        const char *invalid = NULL;
        json = map_prepare_custom(text, 0, &invalid); // prepareGame + validateMap
        if (!json) error = invalid ? invalid : "Map error occured.";
        free(text);
    }
    const cJSON *kept = NULL;
    if (json) {
        // a second download of the same name (two requests in flight) keeps the first copy
        kept = cache_find(req->name);
        if (kept) {
            cJSON_Delete(json);
        } else if (g_cache_count < CACHE_MAX) {
            snprintf(g_cache[g_cache_count].name, sizeof(g_cache[0].name), "%s", req->name);
            g_cache[g_cache_count++].json = json;
            kept = json;
        } else {
            // the cache is full: the oldest map that no game plays makes room
            Client *client = menu_ui()->client;
            size_t victim = CACHE_MAX;
            for (size_t i = 0; i < g_cache_count && victim == CACHE_MAX; i++) {
                int used = 0;
                for (size_t k = 0; client && k < client->game.map_count; k++) used |= client->game.maps[k] == g_cache[i].json;
                if (!used) victim = i;
            }
            if (victim < CACHE_MAX) {
                cJSON_Delete(g_cache[victim].json);
                snprintf(g_cache[victim].name, sizeof(g_cache[0].name), "%s", req->name);
                g_cache[victim].json = json;
                kept = json;
            } else {
                cJSON_Delete(json);
                error = I18N("kn.games.loadFailed", "Failed to load the game.");
            }
        }
    }
    if (!kept) fprintf(stderr, "[community] %s: %s\n", req->name, error);
    else KN_LOG_DEBUG1("[community] downloaded %s\n", req->name);
    if (req->done) req->done(req->user, kept, kept ? NULL : error);
    free(req);
}

void community_fetch(const char *name, const CommunityFetchFn done, void *user) {
    const char *missing = I18N("matchmaker.InvalidGameId", "Game not found.");
    if (!name || !*name) {
        if (done) done(user, NULL, missing);
        return;
    }
    const cJSON *cached = cache_find(name);
    if (cached || backend_available() != 1) {
        if (done) done(user, cached, cached ? NULL : missing);
        return;
    }
    FetchReq *req = calloc(1, sizeof(*req));
    if (!req) {
        if (done) done(user, NULL, missing);
        return;
    }
    snprintf(req->name, sizeof(req->name), "%s", name);
    req->done = done;
    req->user = user;
    char path[320], enc[288];
    backend_url_encode(enc, sizeof(enc), name);
    snprintf(path, sizeof(path), "/api/map-data?name=%s", enc);
    if (!backend_http_get(path, on_map_data, req)) {
        free(req);
        if (done) done(user, NULL, missing);
    }
}

// ---- community_pick

typedef struct {
    Client *client;
    CommunityPickFn done;
    void *user;
} PickReq;

static void on_pick_fetched(void *user, const cJSON *json, const char *error) {
    (void) error;
    PickReq *req = user;
    const int slot = json ? community_slot_of(&req->client->game.maps, &req->client->game.map_count, json) : -1;
    if (req->done) req->done(req->user, slot);
    free(req);
}

void community_pick(Client *client, const CommunityMapRef *ref, const CommunityPickFn done, void *user) {
    if (!client || !ref || !ref->name || !*ref->name) {
        if (done) done(user, -1);
        return;
    }
    // a catalogue map (official, or a bundled community map): the catalogue's own copy
    const int index = community_catalogue_index(ref->name, ref->creator);
    if (index >= 0) {
        const int slot = map_catalog_slot(&client->game.maps, &client->game.map_count, (size_t) index);
        if (done) done(user, slot);
        return;
    }
    PickReq *req = calloc(1, sizeof(*req));
    if (!req) {
        if (done) done(user, -1);
        return;
    }
    *req = (PickReq) {client, done, user};
    community_fetch(ref->name, on_pick_fetched, req);
}

void community_host_slot(Client *client, const int slot, const int mode) {
    if (!client) return;
    if (slot >= 0) menu_select_map(slot);
    if (mode >= 0) client->net_mode_index = mode; // local_game_start reads it (Krunker mode id)
    // Krunker creates a new room: a running offline game reloads on the picked map and mode;
    // otherwise the offline game starts directly on it (offline_start_map).
    if (client->offline && slot >= 0) {
        menu_host_switch(client->game.maps[slot]); // a new room: v10's fresh page (the spinner, not the map card)
        local_game_host(client, slot, client->net_mode_index);
    } else if (slot >= 0) {
        client->offline_start_map = client->game.maps[slot];
    }
    menu_host_offline();
    client->offline_start_map = NULL; // not consumed when a server connection took the request
}

// ---- the published maps a dedicated server can host (the Host Game flow's map step)

static CommunityPublished *g_published;
static size_t g_published_count;
static int g_published_waiting;
static void (*g_published_changed)(void); // the last refresh's listener

static void published_clear(void) {
    for (size_t i = 0; i < g_published_count; i++) free(g_published[i].image);
    free(g_published);
    g_published = NULL;
    g_published_count = 0;
}

static void on_published(void *user, const int status, const char *body, const size_t length) {
    (void) user;
    g_published_waiting = 0;
    cJSON *res = status == 200 && body ? cJSON_ParseWithLength(body, length) : NULL;
    const cJSON *rows = cJSON_GetObjectItem(res, "data");
    if (cJSON_IsArray(rows)) {
        published_clear();
        const int n = cJSON_GetArraySize(rows);
        g_published = n > 0 ? calloc((size_t) n, sizeof(*g_published)) : NULL;
        const cJSON *r, *list = g_published ? rows : NULL;
        cJSON_ArrayForEach(r, list) {
            const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(r, "map_name"));
            const char *creator = cJSON_GetStringValue(cJSON_GetObjectItem(r, "creatorname"));
            const char *image = cJSON_GetStringValue(cJSON_GetObjectItem(r, "map_image"));
            if (!name || !name[0]) continue;
            CommunityPublished *p = &g_published[g_published_count];
            snprintf(p->name, sizeof(p->name), "%s", name);
            snprintf(p->creator, sizeof(p->creator), "%s", creator ? creator : "");
            if (!community_published_spec(p->spec, sizeof(p->spec), p->name, p->creator)) continue;
            p->image = image && image[0] ? strdup(image) : NULL;
            g_published_count++;
        }
        KN_LOG_DEBUG1("[community] %zu published map(s) to host\n", g_published_count);
    }
    cJSON_Delete(res);
    if (g_published_changed) g_published_changed();
}

size_t community_published_maps(const CommunityPublished **out) {
    if (out) *out = g_published;
    return g_published_count;
}

void community_published_refresh(void (*changed)(void)) {
    g_published_changed = changed;
    if (g_published_waiting || backend_available() != 1) return;
    char path[128];
    if (!community_search_path(path, sizeof(path), "", -1, COMMUNITY_SOURCE_PUBLISHED)) return;
    g_published_waiting = backend_http_get(path, on_published, NULL) != 0;
}
