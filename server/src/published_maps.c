// Published (kn_brain) maps as a game server's maps (server/include/published_maps.h): the spec,
// v10's checks, the process's copies and the disk cache. The fetch itself is brain.c's
// (brain_fetch_map_sync; the client's offline sandbox links its stub, which answers "no brain").
#include <published_maps.h>

#include <brain.h>
#include <map_catalog.h>  // map_sanitize_json
#include <map_validate.h> // map_prepare_custom_json, map_validate_error
#include <server_log.h>
#include <shared.h>       // load_json_file

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif

int published_map_parse(const char *spec, char *name, const size_t name_size, char *creator, const size_t creator_size) {
    const size_t prefix = strlen(PUBLISHED_MAP_PREFIX);
    if (!spec || strncasecmp(spec, PUBLISHED_MAP_PREFIX, prefix) != 0) return 0;
    const char *rest = spec + prefix;
    const char *slash = strchr(rest, '/');
    const char *map = slash ? slash + 1 : rest;
    if (!*map || (slash && slash == rest)) return 0; // "published:" / "published:/Arena"
    if (name) snprintf(name, name_size, "%s", map);
    if (creator) snprintf(creator, creator_size, "%.*s", slash ? (int) (slash - rest) : 0, rest);
    return 1;
}

void published_map_format(char *out, const size_t size, const char *name, const char *creator) {
    if (creator && creator[0]) snprintf(out, size, PUBLISHED_MAP_PREFIX "%s/%s", creator, name ? name : "");
    else snprintf(out, size, PUBLISHED_MAP_PREFIX "%s", name ? name : "");
}

const char *published_map_verify(cJSON *map) {
    if (!cJSON_IsObject(map) || !cJSON_IsArray(cJSON_GetObjectItem(map, "objects"))) return "Map error occured.";
    // v7.1.4 / v10 game.prepareGame: "(" / ")" -> full-width, then customGameConfig.validateMap (a
    // published map has a creator, so the featured limits apply) and the prefab / texture checks
    map_prepare_custom_json(map);
    const char *invalid = map_validate_error(map, 1);
    if (invalid) return invalid;
    map_sanitize_json(map);
    return NULL;
}

// ------------------------------------------------------------------ disk cache

static int cache_dir(char *out, const size_t size) {
    const char *xdg = getenv("XDG_CACHE_HOME");
    int n;
    if (xdg && *xdg) {
        n = snprintf(out, size, "%s/krunknative/published-maps", xdg);
    } else {
        const char *home = getenv("HOME");
#ifdef _WIN32
        if (!home || !*home) home = getenv("LOCALAPPDATA");
#endif
        if (!home || !*home) return 0;
        n = snprintf(out, size, "%s/.cache/krunknative/published-maps", home);
    }
    return n > 0 && (size_t) n < size;
}

static void make_dirs(char *path) {
    for (char *p = path + 1;; p++) {
        if (*p && *p != '/' && *p != '\\') continue;
        const char saved = *p;
        *p = 0;
#ifdef _WIN32
        _mkdir(path);
#else
        mkdir(path, 0700);
#endif
        *p = saved;
        if (!saved) break;
    }
}

// <dir>/<creator>~<name>.json, every byte outside [A-Za-z0-9_-] as %XX (names may hold anything)
static int cache_path(char *out, const size_t size, const char *name, const char *creator, char *dir, const size_t dir_size) {
    if (!cache_dir(dir, dir_size)) return 0;
    char file[512];
    size_t n = 0;
    const char *parts[2] = {creator ? creator : "", name ? name : ""};
    for (int k = 0; k < 2; k++) {
        if (k && n + 1 < sizeof(file)) file[n++] = '~';
        for (const unsigned char *p = (const unsigned char *) parts[k]; *p && n + 4 < sizeof(file); p++) {
            const unsigned char c = (unsigned char) tolower(*p); // names and creators are case-insensitive
            if (isalnum(c) || c == '_' || c == '-') file[n++] = (char) c;
            else n += (size_t) snprintf(file + n, sizeof(file) - n, "%%%02X", c);
        }
    }
    file[n] = 0;
    const int w = snprintf(out, size, "%s/%s.json", dir, file);
    return w > 0 && (size_t) w < size;
}

void published_map_cache_store(const char *name, const char *creator, const cJSON *map) {
    char dir[512], path[1100];
    if (!map || !cache_path(path, sizeof(path), name, creator, dir, sizeof(dir))) return;
    make_dirs(dir);
    char *text = cJSON_PrintUnformatted(map);
    char tmp[1120];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = text ? fopen(tmp, "wb") : NULL;
    const int ok = f && fwrite(text, 1, strlen(text), f) == strlen(text);
    if (f) fclose(f);
    if (ok && rename(tmp, path) == 0) server_log("[maps] published map '%s' cached at %s", name, path);
    else {
        remove(tmp);
        server_log_error("[maps] cannot cache published map '%s' at %s: %s", name, path, strerror(errno));
    }
    free(text);
}

cJSON *published_map_cache_read(const char *name, const char *creator) {
    char dir[512], path[1100];
    if (!cache_path(path, sizeof(path), name, creator, dir, sizeof(dir))) return NULL;
    cJSON *map = NULL;
    if (load_json_file(path, &map) || !cJSON_IsObject(map)) {
        cJSON_Delete(map);
        return NULL;
    }
    return map;
}

// ------------------------------------------------------------------ the process's copies

typedef struct {
    char name[96], creator[64];
    cJSON *map;
    int map_id;              // the brain's map_id (v10.0.3 customMapData.id), 0 unknown (the disk cache's copy)
} Loaded;

static Loaded *g_loaded;
static size_t g_loaded_count;

static const cJSON *loaded_find(const char *name, const char *creator) {
    for (size_t i = 0; i < g_loaded_count; i++) {
        if (!strcasecmp(g_loaded[i].name, name) && !strcasecmp(g_loaded[i].creator, creator ? creator : "")) return g_loaded[i].map;
    }
    return NULL;
}

static const cJSON *loaded_keep(const char *name, const char *creator, cJSON *map, const int map_id) {
    Loaded *grown = realloc(g_loaded, (g_loaded_count + 1) * sizeof(*grown));
    if (!grown) {
        cJSON_Delete(map);
        return NULL;
    }
    g_loaded = grown;
    Loaded *l = &g_loaded[g_loaded_count++];
    snprintf(l->name, sizeof(l->name), "%s", name);
    snprintf(l->creator, sizeof(l->creator), "%s", creator ? creator : "");
    l->map = map;
    l->map_id = map_id;
    return map;
}

// Ids of maps the process did not keep (a hosted game's map, fetched by accounts.c through brain_fetch_map)
typedef struct {
    char name[96];
    int map_id;
} NotedId;

static NotedId *g_noted;
static size_t g_noted_count;

void published_map_note_id(const char *name, const int map_id) {
    if (!name || !*name) return;
    for (size_t i = 0; i < g_noted_count; i++) {
        if (!strcasecmp(g_noted[i].name, name)) {
            if (map_id > 0) server_log("[maps] published map '%s' is map id %d", name, map_id);
            g_noted[i].map_id = map_id;
            return;
        }
    }
    if (map_id <= 0) return;
    server_log("[maps] published map '%s' is map id %d", name, map_id);
    NotedId *grown = realloc(g_noted, (g_noted_count + 1) * sizeof(*grown));
    if (!grown) return;
    g_noted = grown;
    snprintf(g_noted[g_noted_count].name, sizeof(g_noted[g_noted_count].name), "%s", name);
    g_noted[g_noted_count++].map_id = map_id;
}

int published_map_id(const char *name) {
    for (size_t i = 0; name && *name && i < g_noted_count; i++)
        if (g_noted[i].map_id > 0 && !strcasecmp(g_noted[i].name, name)) return g_noted[i].map_id; // the latest fetch's
    for (size_t i = 0; name && *name && i < g_loaded_count; i++)
        if (!strcasecmp(g_loaded[i].name, name)) return g_loaded[i].map_id; // published names are unique
    return 0;
}

const cJSON *published_map_cached(const char *name, const char *creator) {
    return name && *name ? loaded_find(name, creator) : NULL;
}

const cJSON *published_map_load(const GsConfig *cfg, const char *name, const char *creator, const int timeout_ms, char *err, const size_t err_size) {
    if (err && err_size) err[0] = 0;
    if (!name || !*name) {
        if (err) snprintf(err, err_size, "no map name");
        return NULL;
    }
    const cJSON *have = loaded_find(name, creator);
    if (have) return have;

    char fetch_err[160] = "";
    int missing = 0;
    int map_id = 0;
    cJSON *map = brain_fetch_map_sync_id(cfg, name, creator, timeout_ms, &missing, &map_id, fetch_err, sizeof(fetch_err));
    if (map) {
        const char *invalid = published_map_verify(map);
        if (invalid) {
            // the brain's copy does not pass: v10 would not play it; neither is an older cache
            server_log_error("[maps] published map '%s' rejected: %s", name, invalid);
            if (err) snprintf(err, err_size, "%s", invalid);
            cJSON_Delete(map);
            return NULL;
        }
        published_map_cache_store(name, creator, map);
        server_log("[maps] published map '%s'%s%s fetched from the brain (map id %d)", name, creator && creator[0] ? " by " : "", creator ? creator : "",
                   map_id);
        return loaded_keep(name, creator, map, map_id);
    }
    // the brain could not answer (down, unreachable, no brain configured): the cached copy (it
    // passed the checks when stored). A brain that answered "not found" (deleted, another
    // creator) is believed: no cached copy then.
    map = missing ? NULL : published_map_cache_read(name, creator);
    const char *cache_invalid = map ? published_map_verify(map) : NULL;
    if (map && !cache_invalid) {
        server_log_error("[maps] published map '%s': %s; playing the cached copy", name, fetch_err[0] ? fetch_err : "fetch failed");
        return loaded_keep(name, creator, map, 0);
    }
    cJSON_Delete(map);
    server_log_error("[maps] published map '%s'%s%s unavailable: %s (%s%s)", name, creator && creator[0] ? " by " : "",
                     creator ? creator : "", fetch_err[0] ? fetch_err : "fetch failed",
                     missing ? "the brain has no such map" : cache_invalid ? "the cached copy is rejected: " : "no cached copy",
                     cache_invalid ? cache_invalid : "");
    if (err) snprintf(err, err_size, "%s", fetch_err[0] ? fetch_err : "fetch failed");
    return NULL;
}
