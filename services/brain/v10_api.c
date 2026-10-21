/*
 * services/brain/v10_api.c - the v10 REST routes with KrunkNative data behind them:
 *
 *   GET /maps                  {data: [official map rows]} (v10 maps.getMaps; index = the map's
 *                              mapIndex, the list the Games window and map pictures use)
 *   GET /maps/:id              {data: map JSON} for mapIndex id (v10 maps.getMap)
 *   GET /maps/by-name/:name    {data: map JSON} for an official map name (v10 maps.getMapByName)
 *   GET /titles                {data: [title rows]} from assets/titles.json (v10 titles.getTitles)
 *   GET /data/skins            the packed skins table (assets/skins.json; v10 reads responseText
 *                              with its unpack, v10_pack.h)
 *   GET /data/spins            the packed wheels table (store_spins_json, as /api/data/spins)
 *
 * Maps come from shared/include/krunker_maps.h (the 46 official maps, v10.0.2's list) and
 * assets/maps/official/<NN>_<name>.json; nothing is invented.
 */
#include "v10_api.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "acct_internal.h"
#include <strings.h>

#include "map_catalog.h"
#include "krunker_maps.h"
#include "v10_pack.h"
#include "knsvc/config.h"
#include "knsvc/json.h"
#include "knsvc/log.h"

#define MOD "v10-api"

const char *assets_path(void); /* shared_shim.c */

/* the official list, mapIndex order (KRUNKER_OFFICIAL_MAPS: index, name, file, objects) */
#define OFFICIAL_ROW(i, name, file, objs) {i, name, file},
static const struct {
    int index;
    const char *name;
    const char *file;
} k_official[] = {KRUNKER_OFFICIAL_MAPS(OFFICIAL_ROW)};
#define OFFICIAL_COUNT ((int) (sizeof k_official / sizeof k_official[0]))

void v10_reply_data(kn_http_req *req, int status, cJSON *data) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "data", data ? data : cJSON_CreateNull());
    cJSON_AddNullToObject(o, "error");
    brain_reply_json(req, status, o);
    cJSON_Delete(o);
}

void v10_reply_error(kn_http_req *req, int status, const char *code, const char *message) {
    cJSON *o = cJSON_CreateObject();
    cJSON *e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "code", code);
    cJSON_AddStringToObject(e, "message", message);
    cJSON_AddItemToObject(o, "error", e);
    brain_reply_json(req, status, o);
    cJSON_Delete(o);
}

/* the stem of a v10 file name: "maps/official/00_burg.json" -> "00_burg" */
static void file_stem(const char *file, char *out, size_t cap) {
    const char *base = strrchr(file, '/');
    base = base ? base + 1 : file;
    snprintf(out, cap, "%s", base);
    char *dot = strrchr(out, '.');
    if (dot) *dot = 0;
}

/* the official map row at list position i, or NULL */
static const cJSON *official_map(int index) {
    const int catalog = map_catalog_official_index(index);
    return catalog >= 0 ? map_catalog_official_json(index) : NULL;
}

/* ---- GET /maps */

static void h_maps(kn_http_req *req, void *ud) {
    (void) ud;
    cJSON *list = cJSON_CreateArray();
    for (int i = 0; i < OFFICIAL_COUNT; i++) {
        cJSON *row = cJSON_CreateObject();
        char stem[96];
        file_stem(k_official[i].file, stem, sizeof stem);
        cJSON_AddNumberToObject(row, "index", k_official[i].index);
        cJSON_AddStringToObject(row, "name", k_official[i].name);
        cJSON_AddStringToObject(row, "fileName", stem);
        cJSON_AddStringToObject(row, "creatorname", "Krunker");
        cJSON_AddNumberToObject(row, "map_featured", 1);
        cJSON_AddItemToArray(list, row);
    }
    v10_reply_data(req, 200, list);
}

/* ---- GET /maps/:id */

static void h_map(kn_http_req *req, void *ud) {
    (void) ud;
    const char *id = kn_http_param(req, "id");
    char *end = NULL;
    const long index = id ? strtol(id, &end, 10) : -1;
    const cJSON *map = (id && end && *end == 0 && *id) ? official_map((int) index) : NULL;
    if (!map) {
        v10_reply_error(req, 404, "MAP_NOT_FOUND", "Map not found");
        return;
    }
    v10_reply_data(req, 200, cJSON_Duplicate(map, 1));
}

/* ---- GET /maps/by-name/:name (the official display name, case-insensitive) */

static void h_map_by_name(kn_http_req *req, void *ud) {
    (void) ud;
    const char *name = kn_http_param(req, "name");
    for (int i = 0; name && i < OFFICIAL_COUNT; i++) {
        if (strcasecmp(name, k_official[i].name)) continue;
        const cJSON *map = official_map(k_official[i].index);
        if (map) {
            v10_reply_data(req, 200, cJSON_Duplicate(map, 1));
            return;
        }
    }
    v10_reply_error(req, 404, "MAP_NOT_FOUND", "Map not found");
}

/* ---- GET /titles (assets/titles.json, read once) */

static cJSON *g_titles;

static cJSON *titles_load(void) {
    if (g_titles) return g_titles;
    char path[1024];
    snprintf(path, sizeof path, "%stitles.json", assets_path());
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        KN_WARN(MOD, "titles: %s not readable", path);
        return NULL;
    }
    fseek(fp, 0, SEEK_END);
    const long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buf = n > 0 ? malloc((size_t) n + 1) : NULL;
    if (buf && fread(buf, 1, (size_t) n, fp) == (size_t) n) {
        buf[n] = 0;
        g_titles = cJSON_Parse(buf);
    }
    free(buf);
    fclose(fp);
    return g_titles;
}

static void h_titles(kn_http_req *req, void *ud) {
    (void) ud;
    cJSON *titles = titles_load();
    if (!titles || !cJSON_IsArray(titles)) {
        v10_reply_error(req, 503, "TITLES_UNAVAILABLE", "Titles are not available");
        return;
    }
    v10_reply_data(req, 200, cJSON_Duplicate(titles, 1));
}

/* ---- GET /data/skins and /data/spins (packed text, not JSON) */

static void reply_packed(kn_http_req *req, const cJSON *json) {
    char *packed = v10_pack_json(json);
    if (!packed) {
        v10_reply_error(req, 503, "PACK_FAILED", "The table could not be packed");
        return;
    }
    brain_cors_headers(req);
    kn_http_respond(req, 200, "text/plain; charset=utf-8", packed, strlen(packed));
    free(packed);
}

static char *g_skins_packed; /* assets/skins.json packed once: the file is static */

static void h_data_skins(kn_http_req *req, void *ud) {
    (void) ud;
    if (g_skins_packed) {
        brain_cors_headers(req);
        kn_http_respond(req, 200, "text/plain; charset=utf-8", g_skins_packed, strlen(g_skins_packed));
        return;
    }
    char path[1024];
    snprintf(path, sizeof path, "%sskins.json", assets_path());
    FILE *fp = fopen(path, "rb");
    cJSON *skins = NULL;
    if (fp) {
        fseek(fp, 0, SEEK_END);
        const long n = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        char *buf = n > 0 ? malloc((size_t) n + 1) : NULL;
        if (buf && fread(buf, 1, (size_t) n, fp) == (size_t) n) {
            buf[n] = 0;
            skins = cJSON_Parse(buf);
        }
        free(buf);
        fclose(fp);
    }
    if (!skins || !cJSON_IsArray(skins)) {
        cJSON_Delete(skins);
        v10_reply_error(req, 503, "SKINS_UNAVAILABLE", "Skins are not available");
        return;
    }
    g_skins_packed = v10_pack_json(skins);
    cJSON_Delete(skins);
    if (!g_skins_packed) {
        v10_reply_error(req, 503, "PACK_FAILED", "The table could not be packed");
        return;
    }
    brain_cors_headers(req);
    kn_http_respond(req, 200, "text/plain; charset=utf-8", g_skins_packed, strlen(g_skins_packed));
}

static void h_data_spins(kn_http_req *req, void *ud) {
    (void) ud;
    cJSON *spins = cJSON_Parse(store_spins_json());
    if (!spins) {
        v10_reply_error(req, 503, "SPINS_UNAVAILABLE", "Wheels are not available");
        return;
    }
    reply_packed(req, spins);
    cJSON_Delete(spins);
}

int v10_api_init(Brain *b) {
    kn_http_route(b->http, "GET", "/maps", h_maps, b);
    kn_http_route(b->http, "GET", "/maps/by-name/:name", h_map_by_name, b);
    kn_http_route(b->http, "GET", "/maps/:id", h_map, b);
    kn_http_route(b->http, "GET", "/titles", h_titles, b);
    kn_http_route(b->http, "GET", "/data/skins", h_data_skins, b);
    kn_http_route(b->http, "GET", "/data/spins", h_data_spins, b);
    return 0;
}
