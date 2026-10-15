// The map of an official Krunker game: the generated table, inline custom maps, the user cache and
// GET <API>/maps/<index> (client/include/krunker_map_source.h). Main thread only.
#include <krunker_map_source.h>
#include <krunker_maps.h>
#include <map_catalog.h>
#include <shared.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define kms_mkdir(p) _mkdir(p)
#else
#define kms_mkdir(p) mkdir((p), 0755)
#endif

#if KRUNKER_MAPS_SCHEMA != 1
#error "krunker_maps.h: unknown schema (tools/krunker/maps.py)"
#endif

#define KMS_MAX_BYTES (64u << 20) // a fetched / cached map JSON (Krunker's largest official map: ~1 MB)

typedef struct {
    const char *name, *file;
    int objects;
} OfficialEntry;

#define KMS_ENTRY(index_, name_, file_, objects_) {name_, file_, objects_},
static const OfficialEntry k_official[] = {KRUNKER_OFFICIAL_MAPS(KMS_ENTRY)};
_Static_assert(sizeof(k_official) / sizeof(k_official[0]) == KRUNKER_OFFICIAL_MAP_COUNT, "krunker_maps.h table size");

static cJSON *g_official[KRUNKER_OFFICIAL_MAP_COUNT];
static unsigned char g_official_failed[KRUNKER_OFFICIAL_MAP_COUNT];

// maps that are not table files: custom games by id (newest first), official indices past the table.
// Kept for the program's lifetime: the client's map list points at them.
typedef struct Extra {
    struct Extra *next;
    int index;     // past-the-table official index, -1 for a custom map
    char id[48];   // custom map id ("" for an official one)
    cJSON *json;
} Extra;
static Extra *g_extra;

int krunker_official_map_count(void) { return KRUNKER_OFFICIAL_MAP_COUNT; }

const char *krunker_official_map_name(const int index) {
    return index >= 0 && index < KRUNKER_OFFICIAL_MAP_COUNT ? k_official[index].name : NULL;
}

const char *krunker_official_map_file(const int index) {
    return index >= 0 && index < KRUNKER_OFFICIAL_MAP_COUNT ? k_official[index].file : NULL;
}

static void say(char *why, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void say(char *why, const size_t n, const char *fmt, ...) {
    if (!why || !n) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(why, n, fmt, ap);
    va_end(ap);
}

// a map JSON: an object with an "objects" array (what map_init needs), sanitised like the catalogue's
static cJSON *parse_map(const char *text, const size_t length) {
    cJSON *json = cJSON_ParseWithLength(text, length);
    if (!cJSON_IsObject(json) || !cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(json, "objects"))) {
        cJSON_Delete(json);
        return NULL;
    }
    map_sanitize_json(json);
    return json;
}

static int valid_map(const cJSON *json) {
    return cJSON_IsObject(json) && cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(json, "objects"));
}

const cJSON *krunker_official_map_json(const int index) {
    if (index < 0 || index >= KRUNKER_OFFICIAL_MAP_COUNT) return NULL;
    if (g_official[index] || g_official_failed[index]) return g_official[index];
    char *path = concat(assets_path(), k_official[index].file);
    unsigned char *buffer = NULL;
    size_t length = 0;
    cJSON *json = NULL;
    if (path && !read_file(path, &buffer, &length)) json = parse_map((const char *) buffer, length);
    free(buffer);
    if (!json) {
        fprintf(stderr, "[krunker] official map %d (%s): %s is missing or not a map JSON\n", index, k_official[index].name,
                path ? path : k_official[index].file);
        g_official_failed[index] = 1;
        free(path);
        return NULL;
    }
    free(path);
    if (!cJSON_IsString(cJSON_GetObjectItemCaseSensitive(json, "name"))) {
        cJSON_DeleteItemFromObjectCaseSensitive(json, "name");
        cJSON_AddStringToObject(json, "name", k_official[index].name);
    }
    g_official[index] = json;
    return json;
}

static Extra *extra_find(const int index, const char *id) {
    for (Extra *e = g_extra; e; e = e->next) {
        if (id && id[0] ? !strcmp(e->id, id) : e->index == index && !e->id[0]) return e;
    }
    return NULL;
}

static const cJSON *extra_add(const int index, const char *id, cJSON *json) {
    Extra *e = calloc(1, sizeof(*e));
    if (!e) {
        cJSON_Delete(json);
        return NULL;
    }
    e->index = id && id[0] ? -1 : index;
    snprintf(e->id, sizeof(e->id), "%s", id ? id : "");
    e->json = json;
    e->next = g_extra; // newest first: a custom map re-sent under its id wins
    g_extra = e;
    return json;
}

// ---------------------------------------------------------------- the user cache

const char *krunker_map_cache_dir(void) {
#ifdef __EMSCRIPTEN__
    return "";
#else
    static char dir[1024];
    static int done;
    if (done) return dir;
    done = 1;
    const char *over = getenv("KRUNKNATIVE_KRUNKER_MAP_CACHE");
    const char *xdg = getenv("XDG_CACHE_HOME");
    int n;
    if (over && *over) n = snprintf(dir, sizeof(dir), "%s", over);
    else if (xdg && *xdg) n = snprintf(dir, sizeof(dir), "%s/krunknative/krunker-maps", xdg);
    else {
#ifdef _WIN32
        const char *base = getenv("LOCALAPPDATA");
        n = base && *base ? snprintf(dir, sizeof(dir), "%s\\krunknative\\krunker-maps", base) : 0;
#else
        const char *home = getenv("HOME");
        n = home && *home ? snprintf(dir, sizeof(dir), "%s/.cache/krunknative/krunker-maps", home) : 0;
#endif
    }
    if (n <= 0 || (size_t) n >= sizeof(dir)) dir[0] = 0;
    return dir;
#endif
}

// cache file names come from server data: only [A-Za-z0-9_-], 1..40 characters
static int safe_id(const char *id) {
    const size_t n = id ? strlen(id) : 0;
    if (!n || n > 40) return 0;
    for (size_t i = 0; i < n; i++) {
        const char c = id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return 0;
    }
    return 1;
}

static int cache_path(char *out, const size_t n, const char *kind, const char *key) {
    const char *dir = krunker_map_cache_dir();
    if (!dir[0] || !safe_id(key)) return 0;
    const int w = snprintf(out, n, "%s/%s_%s.json", dir, kind, key);
    return w > 0 && (size_t) w < n;
}

static void mkdirs(const char *dir) {
    char path[1024];
    snprintf(path, sizeof(path), "%s", dir);
    for (char *p = path + 1; *p; p++) {
        if (*p != '/' && *p != '\\') continue;
        const char c = *p;
        *p = 0;
        kms_mkdir(path);
        *p = c;
    }
    kms_mkdir(path);
}

static void cache_write(const char *kind, const char *key, const cJSON *json) {
    char path[1100], tmp[1110];
    if (!cache_path(path, sizeof(path), kind, key)) return;
    char *text = cJSON_PrintUnformatted(json);
    if (!text) return;
    mkdirs(krunker_map_cache_dir());
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    const size_t len = strlen(text);
    int ok = f && fwrite(text, 1, len, f) == len;
    if (f && fclose(f)) ok = 0;
    free(text);
    if (ok) {
#ifdef _WIN32
        remove(path);
#endif
        ok = rename(tmp, path) == 0;
    }
    if (!ok) {
        remove(tmp);
        fprintf(stderr, "[krunker] cannot cache %s: %s\n", path, strerror(errno));
    }
}

static cJSON *cache_read(const char *kind, const char *key) {
    char path[1100];
    if (!cache_path(path, sizeof(path), kind, key)) return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    cJSON *json = NULL;
    if (fseek(f, 0, SEEK_END) == 0) {
        const long size = ftell(f);
        char *buf = size > 0 && (unsigned long) size <= KMS_MAX_BYTES ? malloc((size_t) size) : NULL;
        if (buf && fseek(f, 0, SEEK_SET) == 0 && fread(buf, 1, (size_t) size, f) == (size_t) size) json = parse_map(buf, (size_t) size);
        free(buf);
    }
    fclose(f);
    if (!json) fprintf(stderr, "[krunker] ignoring the unreadable cached map %s\n", path);
    return json;
}

// ---------------------------------------------------------------- GET <API>/maps/<index>

static KrunkerMapFetchFn g_fetch_fn;
static struct {
    unsigned gen;   // the download in flight (results of older ones are ignored)
    int state;      // 0 none, 1 running, 2 done (json), 3 failed (err)
    int custom;     // 1: a community map by id, 0: an official index past the table
    cJSON *json;
    char key[48];   // the URL's key: the index in decimal, or the community map's id
    char err[160];
} g_dl;
static unsigned g_dl_counter;

void krunker_map_source_set_fetch(const KrunkerMapFetchFn fn) { g_fetch_fn = fn; }

static const char *api_origin(void) {
#ifndef __EMSCRIPTEN__
    const char *over = getenv("KRUNKNATIVE_KRUNKER_MAPS_API");
    if (over && *over) return over;
#endif
    return KRUNKER_MAPS_API_URL;
}

static void dl_done(void *ctx, const int status, const char *body, const size_t length) {
    const unsigned gen = (unsigned) (uintptr_t) ctx;
    if (gen != g_dl.gen || g_dl.state != 1) return; // cancelled / superseded
    cJSON *env = status >= 200 && status < 300 && body && length <= KMS_MAX_BYTES ? cJSON_ParseWithLength(body, length) : NULL;
    cJSON *data = cJSON_DetachItemFromObjectCaseSensitive(env, "data"); // {"data": map, "error": ...}
    cJSON_Delete(env);
    if (valid_map(data)) {
        map_sanitize_json(data);
        g_dl.json = data;
        g_dl.state = 2;
        return;
    }
    cJSON_Delete(data);
    if (status >= 200 && status < 300) snprintf(g_dl.err, sizeof(g_dl.err), "the answer is not a map");
    else if (status) snprintf(g_dl.err, sizeof(g_dl.err), "HTTP %d", status);
    else snprintf(g_dl.err, sizeof(g_dl.err), "no response");
    g_dl.state = 3;
}

// GET <API>/maps/<key>: an official index (the live client's getMap(mapIndex), game.deob.js:26600). A community
// map id takes the same route here, UNVERIFIED: the live client never asks for one (its custom games arrive inline
// in init arg 9), so a community map that is not inline depends on the API answering /maps/<id>.
static unsigned dl_start(const char *key, const int custom) {
    if (g_dl.state == 1 && g_dl.custom == custom && !strcmp(g_dl.key, key)) return g_dl.gen; // already downloading it
    cJSON_Delete(g_dl.json);
    memset(&g_dl, 0, sizeof(g_dl));
    if (!g_fetch_fn) return 0;
    g_dl.gen = ++g_dl_counter ? g_dl_counter : ++g_dl_counter;
    g_dl.custom = custom;
    snprintf(g_dl.key, sizeof(g_dl.key), "%s", key);
    g_dl.state = 1;
    char url[512];
    snprintf(url, sizeof(url), "%s%s%s", api_origin(), KRUNKER_MAP_PATH, key);
    if (custom) printf("[krunker] custom map %s is not in this client's cache: GET %s\n", key, url);
    else printf("[krunker] map %s is not in this client's table (%d maps): GET %s\n", key, KRUNKER_OFFICIAL_MAP_COUNT, url);
    if (!g_fetch_fn(url, dl_done, (void *) (uintptr_t) g_dl.gen)) {
        snprintf(g_dl.err, sizeof(g_dl.err), "the request could not be started");
        g_dl.state = 3;
    }
    return g_dl.gen;
}

// ---------------------------------------------------------------- requests

enum { ST_DECIDE = 0, ST_TABLE, ST_FETCH, ST_DONE };

void krunker_map_request_begin(KrunkerMapRequest *r, const int index, cJSON *custom, const char *custom_id) {
    memset(r, 0, sizeof(*r));
    r->index = index;
    r->custom = custom;
    snprintf(r->custom_id, sizeof(r->custom_id), "%s", custom_id ? custom_id : "");
    r->stage = ST_DECIDE;
}

void krunker_map_request_cancel(KrunkerMapRequest *r) {
    cJSON_Delete(r->custom);
    r->custom = NULL;
    if (r->fetch_gen && r->fetch_gen == g_dl.gen && g_dl.state == 1) g_dl.state = 0; // its answer is dropped
    r->fetch_gen = 0;
    r->stage = ST_DONE;
}

static KrunkerMapStatus ready(KrunkerMapRequest *r, const cJSON *map, const cJSON **out) {
    r->stage = ST_DONE;
    if (out) *out = map;
    return KRUNKER_MAP_READY;
}

static KrunkerMapStatus unknown(KrunkerMapRequest *r) {
    r->stage = ST_DONE;
    return KRUNKER_MAP_UNKNOWN;
}

// the request's key in the API URL: its community map id, else its index
static const char *request_key(const KrunkerMapRequest *r, char *buf, const size_t n) {
    if (r->custom_id[0]) return r->custom_id;
    snprintf(buf, n, "%d", r->index);
    return buf;
}

KrunkerMapStatus krunker_map_request_poll(KrunkerMapRequest *r, const cJSON **out, char *why, const size_t why_size) {
    if (out) *out = NULL;
    if (why && why_size) why[0] = 0;
    const int index = r->index;
    switch (r->stage) {
        case ST_DECIDE:
            if (r->custom) { // 1. a custom game's map, sent with init
                cJSON *json = r->custom;
                r->custom = NULL;
                if (!valid_map(json)) {
                    cJSON_Delete(json);
                    say(why, why_size, "the server sent custom map %s, which is not a map JSON", r->custom_id[0] ? r->custom_id : "?");
                    return unknown(r);
                }
                map_sanitize_json(json);
                if (r->custom_id[0]) cache_write("custom", r->custom_id, json);
                const cJSON *map = extra_add(-1, r->custom_id, json);
                if (!map) return unknown(r);
                return ready(r, map, out);
            }
            if (index >= 0 && index < KRUNKER_OFFICIAL_MAP_COUNT && !r->custom_id[0]) { // 2. the table
                r->stage = ST_TABLE;
                return krunker_map_request_poll(r, out, why, why_size);
            }
            if (r->custom_id[0]) { // 3. a community map by id, without its data: the cache, else GET <API>/maps/<id>
                Extra *e = extra_find(-1, r->custom_id);
                if (e) return ready(r, e->json, out);
                cJSON *json = cache_read("custom", r->custom_id);
                if (json) {
                    const cJSON *map = extra_add(-1, r->custom_id, json);
                    if (map) return ready(r, map, out);
                }
                if (!safe_id(r->custom_id)) {
                    say(why, why_size, "custom map id %s is not one this client can fetch", r->custom_id);
                    return unknown(r);
                }
                r->fetch_gen = dl_start(r->custom_id, 1);
                if (!r->fetch_gen) {
                    say(why, why_size, "custom map %s is not in this client's cache and cannot be downloaded here", r->custom_id);
                    return unknown(r);
                }
                r->stage = ST_FETCH;
                return krunker_map_request_poll(r, out, why, why_size);
            }
            if (index < 0) {
                say(why, why_size, "the server named no map (index %d)", index);
                return unknown(r);
            }
            { // 4. past the table: the cache, else the API
                char key[16];
                snprintf(key, sizeof(key), "%d", index);
                Extra *e = extra_find(index, NULL);
                if (e) return ready(r, e->json, out);
                cJSON *json = cache_read("official", key);
                if (json) {
                    const cJSON *map = extra_add(index, NULL, json);
                    if (map) return ready(r, map, out);
                }
                char ikey[16];
                snprintf(ikey, sizeof(ikey), "%d", index);
                r->fetch_gen = dl_start(ikey, 0);
                if (!r->fetch_gen) {
                    say(why, why_size, "map %d is newer than this client's table of %d maps and cannot be downloaded here", index,
                        KRUNKER_OFFICIAL_MAP_COUNT);
                    return unknown(r);
                }
                r->stage = ST_FETCH;
                return krunker_map_request_poll(r, out, why, why_size);
            }
        case ST_TABLE: {
            char *path = concat(assets_path(), k_official[index].file);
            const int have = path ? asset_prefetch(path) : -1; // web: a background download, never a blocked frame
            free(path);
            if (have == 0) return KRUNKER_MAP_PENDING;
            const cJSON *map = have > 0 ? krunker_official_map_json(index) : NULL;
            if (map) return ready(r, map, out);
            say(why, why_size, "its file is missing or damaged"); // the path is on stderr (krunker_official_map_json)
            return unknown(r);
        }
        case ST_FETCH: {
            char key[48];
            request_key(r, key, sizeof(key));
            if (r->fetch_gen != g_dl.gen) {
                say(why, why_size, "the download of map %s was superseded", key);
                return unknown(r);
            }
            if (g_dl.state == 1) return KRUNKER_MAP_PENDING;
            if (g_dl.state == 2) {
                cJSON *json = g_dl.json;
                g_dl.json = NULL;
                g_dl.state = 0;
                const cJSON *map;
                if (r->custom_id[0]) {
                    cache_write("custom", r->custom_id, json);
                    map = extra_add(-1, r->custom_id, json);
                } else {
                    cache_write("official", key, json);
                    map = extra_add(index, NULL, json);
                }
                if (map) return ready(r, map, out);
                return unknown(r);
            }
            say(why, why_size, "GET %s%s%s: %s", api_origin(), KRUNKER_MAP_PATH, key, g_dl.err[0] ? g_dl.err : "failed");
            g_dl.state = 0;
            return unknown(r);
        }
        default:
            return KRUNKER_MAP_UNKNOWN;
    }
}
