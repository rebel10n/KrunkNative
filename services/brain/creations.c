/*
 * services/brain/creations.c - the social hub's creation catalogue over HTTP
 * (Krunker v3.1.1 matchmaker/publicRoutes.ts, called by src/libs/apiClient.js
 * with config.apiURL = the brain):
 *
 *   GET /api/maps?index=&pos=&accountId=&val=&cat=&source=  -> {data:[map rows], index, pos, error}
 *   GET /api/mods?index=&pos=&accountId=&val=  -> {data:[mod rows], index, pos, error}
 *   GET /api/assets?index=&cat=&username=&val= -> {data:[asset rows], index, error}
 *   GET /api/search?type=map|mod&val=&cat=&source= -> {data, index: 0, error} (maps: an empty val
 *       with cat= or source= lists that filter, v10's Popular Categories cards)
 *   GET /api/streams?index=                    -> v3.1.1 placeholder (no streams)
 *   GET /api/prices[?id=]                      -> {} / 0 (v3.1.1 placeholder)
 *
 * Maps = the map catalogue (shared/include/map_catalog.h: v7.1.4's 32 official maps, then 12
 * community maps and the v3.1.1 311_* / 152_* maps in the v3.1.1 offline server's tab orders)
 * followed by the maps
 * players published (published_maps). Tabs (publicRoutes.ts sortMaps):
 * 0 Popular, 1 Featured, 2 New, 3 Favorites (accountId's), 4 all / My Games
 * (accountId's published maps when given). 42 rows per page.
 *
 * Map rows carry the Games window's fields (v10 names: map_name, creatorname, map_votes, map_pl,
 * map_featured, map_updatecounter, map_date, map_info, fav, fund, category) plus KrunkNative's:
 * map_category (index, -1 none), category (its name, "" none), official (creator "Krunker"),
 * modes (the map's game mode ids: the Games window's mode tags). cat=<n> keeps the published maps
 * of that category (the catalogue has no categories, so a category keeps none of them);
 * source=official|community splits the catalogue's official maps from the rest (the bundled
 * community maps and published ones). Publishing (uploadM) checks the map like v10's publish:
 * the editor's validateMap rules (shared/src/map_validate.c), then the name is free.
 */
#include "acct_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "knsvc/http.h"
#include "knsvc/json.h"
#include "knsvc/log.h"
#include "map_catalog.h"
#include "map_validate.h"

#define MOD "creations"
#define TAB_PAGE_SIZE 42

/* ------------------------------------------------------------------ catalogue rows */

/* the map JSON's "modes" (game mode ids) as a new array: the Games window's mode tags */
static cJSON *modes_array(const cJSON *map) {
    cJSON *out = cJSON_CreateArray();
    const cJSON *modes = map ? cJSON_GetObjectItemCaseSensitive(map, "modes") : NULL;
    const cJSON *m;
    if (cJSON_IsArray(modes)) {
        cJSON_ArrayForEach(m, modes) {
            if (cJSON_IsNumber(m)) cJSON_AddItemToArray(out, cJSON_CreateNumber(m->valuedouble));
        }
    }
    return out;
}

static cJSON *catalog_row(size_t index) {
    const MapCatalogEntry *e = map_catalog_at(index);
    if (!e) return NULL;
    const cJSON *json = map_catalog_json(index);
    const char *desc = json ? kn_json_str(json, "description", "") : "";
    cJSON *row = cJSON_CreateObject();
    cJSON_AddNumberToObject(row, "map_id", map_catalog_map_id(e));
    cJSON_AddStringToObject(row, "map_name", e->name);
    cJSON_AddStringToObject(row, "map_date", "");
    cJSON_AddStringToObject(row, "map_initialdate", "");
    cJSON_AddStringToObject(row, "map_info", "{}");
    cJSON_AddStringToObject(row, "map_image", "");
    cJSON_AddNumberToObject(row, "map_votes", 0);
    cJSON_AddNumberToObject(row, "map_pl", 0);
    cJSON_AddNumberToObject(row, "map_updatecounter", 1);
    cJSON_AddNumberToObject(row, "map_featured", e->source == MAP_SOURCE_OFFICIAL);
    cJSON_AddStringToObject(row, "map_description", desc ? desc : "");
    cJSON_AddStringToObject(row, "creatorname", e->creator ? e->creator : "Krunker");
    cJSON_AddNumberToObject(row, "fund", 0);
    cJSON_AddNumberToObject(row, "fav", 0);
    cJSON_AddStringToObject(row, "mod_url", e->mod_url ? e->mod_url : "");
    cJSON_AddStringToObject(row, "mod_id", "");
    cJSON_AddStringToObject(row, "category", ""); /* the catalogue keeps no categories */
    cJSON_AddNumberToObject(row, "map_category", -1);
    cJSON_AddBoolToObject(row, "official", map_catalog_is_official(e));
    cJSON_AddItemToObject(row, "modes", modes_array(json));
    return row;
}

/* a published map row (MAP_COLS): its category name, the Official / Community split and the mode
 * tags the publish stored in map_info ({"m": [mode ids]}) */
static void map_row_fields(cJSON *row) {
    const cJSON *cat = cJSON_GetObjectItemCaseSensitive(row, "map_category");
    const int index = cJSON_IsNumber(cat) ? (int) cat->valuedouble : -1;
    const char *name = map_category_name(index);
    const cJSON *info = cJSON_GetObjectItemCaseSensitive(row, "map_info");
    cJSON *parsed = NULL;
    if (cJSON_IsString(info) && info->valuestring) parsed = cJSON_Parse(info->valuestring);
    else if (cJSON_IsObject(info)) parsed = cJSON_Duplicate(info, 1);
    const cJSON *m = parsed ? cJSON_GetObjectItemCaseSensitive(parsed, "m") : NULL;
    cJSON_DeleteItemFromObjectCaseSensitive(row, "category");
    cJSON_AddStringToObject(row, "category", name ? name : "");
    cJSON_DeleteItemFromObjectCaseSensitive(row, "map_category");
    cJSON_AddNumberToObject(row, "map_category", name ? index : -1);
    cJSON_DeleteItemFromObjectCaseSensitive(row, "official");
    cJSON_AddBoolToObject(row, "official", 0); /* published maps are community maps */
    cJSON_DeleteItemFromObjectCaseSensitive(row, "modes");
    cJSON_AddItemToObject(row, "modes", cJSON_IsArray(m) ? cJSON_Duplicate(m, 1) : cJSON_CreateArray());
    cJSON_Delete(parsed);
}

static int contains_ci(const char *hay, const char *needle) {
    if (!needle || !*needle) return 1;
    const size_t n = strlen(needle);
    for (; hay && *hay; hay++) {
        size_t i = 0;
        while (i < n && hay[i] && tolower((unsigned char) hay[i]) == tolower((unsigned char) needle[i])) i++;
        if (i == n) return 1;
    }
    return 0;
}

/* SOURCE_*: the source= filter (0 all, 1 official maps, 2 community maps) */
enum { SOURCE_ALL, SOURCE_OFFICIAL, SOURCE_COMMUNITY };

/* a catalogue row passes the filters: the source split, and no category (the catalogue has none) */
static int catalog_wanted(size_t index, int category, int source) {
    const MapCatalogEntry *e = map_catalog_at(index);
    if (!e || category >= 0) return 0;
    const int official = map_catalog_is_official(e);
    return !(source == SOURCE_OFFICIAL && !official) && !(source == SOURCE_COMMUNITY && official);
}

/* Catalogue part of a tab (or of a search when val != NULL). */
static void catalog_rows(cJSON *out, int tab, const char *val, int64_t account, int category, int source) {
    if (val && *val) {
        for (size_t i = 0; i < map_catalog_count(); i++)
            if (contains_ci(map_catalog_at(i)->name, val) && catalog_wanted(i, category, source)) cJSON_AddItemToArray(out, catalog_row(i));
        return;
    }
    if (tab == 3 || (tab == 4 && account > 0)) return; /* favourites / my games: DB only */
    int idx[128];
    const size_t n = map_catalog_games_tab(tab, idx, 128);
    for (size_t i = 0; i < n; i++) {
        if (catalog_wanted((size_t) idx[i], category, source)) cJSON_AddItemToArray(out, catalog_row((size_t) idx[i]));
    }
}

/* ------------------------------------------------------------------ request plumbing */

typedef struct ApiReq {
    kn_http_req *req;
    int kind;         /* 0 maps, 1 mods, 2 assets, 3 search maps, 4 search mods */
    int tab;
    int pos;
    int64_t account;
    int category;     /* cat=: a map category index, -1 all */
    int source;       /* source=: SOURCE_* */
    char val[96];
} ApiReq;

static int64_t query_int(kn_http_req *req, const char *key, int64_t def) {
    char buf[32];
    if (kn_http_query_get(req, key, buf, sizeof buf) <= 0) return def;
    char *end = NULL;
    const long long v = strtoll(buf, &end, 10);
    return end && *end == 0 ? v : def;
}

static void reply_page(ApiReq *a, cJSON *rows, const char *error) {
    cJSON *res = cJSON_CreateObject();
    cJSON *page = cJSON_CreateArray();
    const int n = cJSON_GetArraySize(rows);
    const int search = a->kind >= 3;
    const int from = search ? 0 : a->pos * TAB_PAGE_SIZE;
    const int to = search ? n : from + TAB_PAGE_SIZE;
    for (int i = from; i < n && i < to; i++) cJSON_AddItemToArray(page, cJSON_Duplicate(cJSON_GetArrayItem(rows, i), 1));
    cJSON_AddItemToObject(res, "data", page);
    cJSON_AddNumberToObject(res, "index", search ? 0 : a->tab);
    if (!search && a->kind != 2) cJSON_AddNumberToObject(res, "pos", a->pos);
    if (error) cJSON_AddStringToObject(res, "error", error);
    else cJSON_AddNullToObject(res, "error");
    brain_reply_json(a->req, 200, res);
    cJSON_Delete(res);
}

static void db_rows_done(kn_db_result *res, void *ud) {
    ApiReq *a = ud;
    cJSON *rows = cJSON_CreateArray();
    const int maps = a->kind == 0 || a->kind == 3;
    if (maps) catalog_rows(rows, a->tab, a->kind == 3 ? a->val : (a->val[0] ? a->val : NULL), a->account, a->category, a->source);
    if (kn_db_ok(res)) {
        cJSON *db = kn_db_result_to_json(res);
        cJSON *it;
        /* "New" lists the newest published maps before the catalogue */
        int at = 0;
        while (db && (it = cJSON_DetachItemFromArray(db, 0)) != NULL) {
            if (maps) map_row_fields(it);
            if (a->kind == 0 && a->tab == 2 && !a->val[0]) cJSON_InsertItemInArray(rows, at++, it);
            else cJSON_AddItemToArray(rows, it);
        }
        cJSON_Delete(db);
    } else {
        acct_db_error(res, "api");
    }
    if (kn_http_req_alive(a->req)) reply_page(a, rows, NULL);
    else kn_http_respond_error(a->req, 500, "gone");
    cJSON_Delete(rows);
    free(a);
}

/* H(x): kn_html(x) on postgres, {html:x} elsewhere; INFO(c): a JSON column as text */
#define MAP_COLS_(H0, H1, INFO_M)                                                                                  \
    "SELECT m.map_id, " H0 "m.map_name" H1 " AS map_name, m.map_date, m.map_initialdate, " INFO_M ", "             \
    "coalesce(m.map_image, '') AS map_image, m.map_votes, m.map_plays AS map_pl, m.map_updatecounter, " \
    "m.map_featured, " H0 "m.map_description" H1 " AS map_description, coalesce(a.username, 'Community') AS creatorname, " \
    "m.map_funds AS fund, (SELECT count(*) FROM creation_favs f WHERE f.kind = 0 AND f.creation_id = m.map_id) AS fav, " \
    "'' AS mod_url, '' AS mod_id, m.map_category FROM published_maps m LEFT JOIN accounts a ON a.id = m.creator_id "

#define MAP_COLS MAP_COLS_("kn_html(", ")", "m.map_info::text AS map_info")
#define MAP_COLS_ANY MAP_COLS_("{html:", "}", "m.map_info AS map_info__t")

#define MOD_COLS_(H0, H1, INFO_D)                                                                                  \
    "SELECT d.mod_id, " H0 "d.mod_name" H1 " AS mod_name, d.mod_date, d.mod_initialdate, " INFO_D ", "             \
    "coalesce(d.mod_image, '') AS mod_image, d.mod_votes, d.mod_plays AS mod_pl, d.mod_updatecounter, d.mod_featured, " \
    H0 "d.mod_description" H1 " AS mod_description, d.mod_url, coalesce(a.username, 'Community') AS creatorname, " \
    "0 AS fund, (SELECT count(*) FROM creation_favs f WHERE f.kind = 1 AND f.creation_id = d.mod_id) AS fav " \
    "FROM published_mods d LEFT JOIN accounts a ON a.id = d.creator_id "
#define MOD_COLS MOD_COLS_("kn_html(", ")", "d.mod_info::text AS mod_info")
#define MOD_COLS_ANY MOD_COLS_("{html:", "}", "d.mod_info AS mod_info__t")

static void run(ApiReq *a) {
    Brain *b = g_acct_brain;
    const int maps_kind = a->kind == 0 || a->kind == 3;
    if (!b || !b->db || (maps_kind && a->source == SOURCE_OFFICIAL)) {
        /* no database, or the official maps only: catalogue only */
        cJSON *rows = cJSON_CreateArray();
        if (maps_kind) catalog_rows(rows, a->tab, a->val[0] ? a->val : NULL, a->account, a->category, a->source);
        reply_page(a, rows, NULL);
        cJSON_Delete(rows);
        free(a);
        return;
    }
    char acc[24], pat[200];
    acct_itoa(acc, a->account);
    size_t o = 0;
    for (const char *p = a->val; *p && o + 2 < sizeof pat; p++) {
        if (*p == '%' || *p == '_' || *p == '\\') pat[o++] = '\\';
        pat[o++] = *p;
    }
    pat[o] = 0;
    const int maps = a->kind == 0 || a->kind == 3;
    const char *order;
    /* cat=: the category filter goes first in the conditions (maps only; an int we formatted) */
    char catf[48] = "";
    if (maps_kind && a->category >= 0) snprintf(catf, sizeof catf, " AND m.map_category = %d", a->category);
    if (a->kind >= 3) order = "ORDER BY 7 DESC LIMIT 200";
    else if (a->tab == 1) order = "AND x_featured > 0 ORDER BY 7 DESC LIMIT 2000";
    else if (a->tab == 2) order = "ORDER BY 4 DESC LIMIT 2000";
    else if (a->tab == 3) order = "AND x_fav ORDER BY 7 DESC LIMIT 2000";
    else if (a->tab == 4 && a->account > 0) order = "AND x_mine ORDER BY 3 DESC LIMIT 2000";
    else order = "ORDER BY 7 DESC, 3 DESC LIMIT 2000";
    char sql[2048], any[2048];
    for (int dialect = 0; dialect < 2; dialect++) {
        /* x_* placeholders select the tab filter on the right table */
        char filt[512];
        snprintf(filt, sizeof filt, "%s %s", catf, order);
        const char *pfx = maps ? "m." : "d.";
        const char *kind = maps ? "0" : "1";
        const char *idc = maps ? "map_id" : "mod_id";
        char repl[512] = "";
        /* expand placeholders */
        const char *src = filt;
        size_t r = 0;
        while (*src && r + 128 < sizeof repl) {
            if (!strncmp(src, "x_featured", 10)) {
                r += (size_t) snprintf(repl + r, sizeof repl - r, "%s%s_featured", pfx, maps ? "map" : "mod");
                src += 10;
            } else if (!strncmp(src, "x_fav", 5)) {
                r += (size_t) snprintf(repl + r, sizeof repl - r,
                                       "EXISTS (SELECT 1 FROM creation_favs f WHERE f.kind = %s AND f.creation_id = %s%s "
                                       "AND f.account_id = %s)", kind, pfx, idc, dialect ? "{int:$1}" : "$1::int");
                src += 5;
            } else if (!strncmp(src, "x_mine", 6)) {
                r += (size_t) snprintf(repl + r, sizeof repl - r, "%screator_id = %s", pfx, dialect ? "{int:$1}" : "$1::int");
                src += 6;
            } else {
                repl[r++] = *src++;
            }
        }
        repl[r] = 0;
        if (!dialect)
            snprintf(sql, sizeof sql, "%s WHERE %sdeleted_at IS NULL AND $1::int IS NOT NULL AND ($2 = '' OR %s%s_name ILIKE '%%' || $2 || '%%') %s",
                     maps ? MAP_COLS : MOD_COLS, pfx, pfx, maps ? "map" : "mod", repl);
        else
            snprintf(any, sizeof any, "%s WHERE %sdeleted_at IS NULL AND $1 IS NOT NULL AND ($2 = '' OR %s%s_name {ilike} '%%' || $2 || '%%' ESCAPE '\\') %s",
                     maps ? MAP_COLS_ANY : MOD_COLS_ANY, pfx, pfx, maps ? "map" : "mod", repl);
    }
    const kn_sql text = {.pg = sql, .any = any};
    kn_db_query_sql(b->db, &text, KN_DB_ARGS(acc, pat), db_rows_done, a);
}

static ApiReq *api_req(kn_http_req *req, int kind) {
    ApiReq *a = calloc(1, sizeof *a);
    if (!a) {
        kn_http_respond_error(req, 500, "out of memory");
        return NULL;
    }
    a->req = req;
    a->kind = kind;
    a->tab = (int) query_int(req, "index", 0);
    a->pos = (int) query_int(req, "pos", 0);
    if (a->pos < 0 || a->pos > 10000) a->pos = 0;
    a->account = query_int(req, "accountId", 0);
    if (kn_http_query_get(req, "val", a->val, sizeof a->val) < 0) a->val[0] = 0;
    a->category = (int) query_int(req, "cat", -1);
    if (a->category < 0 || a->category >= MAP_CATEGORY_COUNT) a->category = -1;
    char source[16] = "";
    kn_http_query_get(req, "source", source, sizeof source);
    a->source = !strcmp(source, "official") ? SOURCE_OFFICIAL : !strcmp(source, "community") ? SOURCE_COMMUNITY : SOURCE_ALL;
    return a;
}

static void h_maps(kn_http_req *req, void *ud) {
    (void) ud;
    ApiReq *a = api_req(req, 0);
    if (a) run(a);
}

static void h_mods(kn_http_req *req, void *ud) {
    (void) ud;
    ApiReq *a = api_req(req, 1);
    if (a) run(a);
}

static void h_assets(kn_http_req *req, void *ud) {
    (void) ud;
    ApiReq *a = api_req(req, 2);
    if (!a) return;
    a->tab = (int) query_int(req, "cat", -1);
    if (!g_acct_brain || !g_acct_brain->db) {
        run(a);
        return;
    }
    char pat[200] = "", cat[24], user[64] = "";
    kn_http_query_get(req, "username", user, sizeof user);
    size_t o = 0;
    for (const char *p = a->val; *p && o + 2 < sizeof pat; p++) {
        if (*p == '%' || *p == '_' || *p == '\\') pat[o++] = '\\';
        pat[o++] = *p;
    }
    pat[o] = 0;
    kn_db_query_sql(g_acct_brain->db,
                SQL(.pg = "SELECT x.id, kn_html(x.name) AS nm, x.data::text AS dat, x.created_at AS ini, a.username AS plrn, "
                          "x.size_kb AS siz, x.category AS cat FROM published_assets x LEFT JOIN accounts a ON a.id = x.creator_id "
                          "WHERE x.deleted_at IS NULL AND ($1 = '' OR x.name ILIKE '%' || $1 || '%') "
                          "AND ($2::int < 0 OR x.category = $2::int) AND ($3 = '' OR lower(a.username) = lower($3)) "
                          "ORDER BY x.id DESC LIMIT 500",
                    .any = "SELECT x.id, {html:x.name} AS nm, x.data AS dat__t, x.created_at AS ini, a.username AS plrn, "
                           "x.size_kb AS siz, x.category AS cat FROM published_assets x LEFT JOIN accounts a ON a.id = x.creator_id "
                           "WHERE x.deleted_at IS NULL AND ($1 = '' OR x.name {ilike} '%' || $1 || '%' ESCAPE '\\') "
                           "AND ({int:$2} < 0 OR x.category = {int:$2}) AND ($3 = '' OR lower(a.username) = lower($3)) "
                           "ORDER BY x.id DESC LIMIT 500"),
                KN_DB_ARGS(pat, acct_itoa(cat, a->tab), user), db_rows_done, a);
}

static void h_search(kn_http_req *req, void *ud) {
    (void) ud;
    char type[16] = "";
    kn_http_query_get(req, "type", type, sizeof type);
    const int kind = !strcmp(type, "map") ? 3 : !strcmp(type, "mod") ? 4 : -1;
    ApiReq *a = api_req(req, kind < 0 ? 3 : kind);
    if (!a) return;
    /* v10's Popular Categories cards search with an empty name and a category (mpCat only): the
     * category's maps; an empty name without a category or source finds nothing, as before */
    if (kind < 0 || (!a->val[0] && (kind != 3 || (a->category < 0 && a->source == SOURCE_ALL)))) {
        cJSON *rows = cJSON_CreateArray();
        reply_page(a, rows, NULL);
        cJSON_Delete(rows);
        free(a);
        return;
    }
    run(a);
}

/* v3.1.1 placeholders (publicRoutes.ts) */
static void h_streams(kn_http_req *req, void *ud) {
    (void) ud;
    cJSON *res = cJSON_CreateObject();
    cJSON *data = cJSON_AddObjectToObject(res, "data");
    cJSON_AddArrayToObject(data, "streams");
    cJSON_AddFalseToObject(data, "promo");
    cJSON_AddArrayToObject(data, "dropCampaigns");
    if (query_int(req, "index", 0) != 1) {
        cJSON_AddNumberToObject(res, "c", 0);
        cJSON_AddArrayToObject(res, "d");
    }
    cJSON_AddNullToObject(res, "n");
    brain_reply_json(req, 200, res);
    cJSON_Delete(res);
}

static void h_prices(kn_http_req *req, void *ud) {
    (void) ud;
    char id[16];
    cJSON *res = kn_http_query_get(req, "id", id, sizeof id) >= 0 ? cJSON_CreateNumber(0) : cJSON_CreateObject();
    brain_reply_json(req, 200, res);
    cJSON_Delete(res);
}

/* ------------------------------------------------------------------ social socket: publishing */

#define MAP_DATA_MAX 500000   /* social.js uploadGameAsset limit */
#define THUMB_MAX (200 * 1024) /* data URL; config.maxThumbP 150 kb */

static void publish_done(SocialSession *s, kn_db_result *res, void *extra) {
    (void) extra;
    if (!kn_db_ok(res)) {
        social_send_n(s, "upMp", 1, cJSON_CreateString(acct_db_error(res, "uploadM")));
        return;
    }
    cJSON *v = acct_db_json(res);
    const int updated = (int) kn_json_int(v, "updated", 0);
    social_send_n(s, "pubMR", 1, cJSON_CreateString(updated ? "Map Updated!" : "Map Published!"));
    cJSON_Delete(v);
}

/* ["uploadM", mapJson, thumbDataUrl|null, description, videoId, category, postToHub, mobile] ->
 * ["pubMR", msg] | ["upMp", err]. The checks are the editor's publish (v10 editor publishMap /
 * uploadGame, validateMap; here shared/src/map_validate.c): the 500k data limit, a category in
 * the Games window's list, then the map itself (name 1-16 characters without < or >, spawns,
 * objects and the limits, first error as v7 words it). The name must not be a catalogue map.
 * Republishing your own map updates it (map_updatecounter + 1). postToHub / mobile are read and
 * not stored yet (no hub feed post, no mobile flag in the brain). */
void sh_upload_map(SocialSession *s, const cJSON *f) {
    if (s->account_id <= 0) {
        social_send_n(s, "upMp", 1, cJSON_CreateString("Must be logged in"));
        return;
    }
    const char *data = fa_str(f, 1), *thumb = fa_str(f, 2), *desc = fa_str(f, 3), *video = fa_str(f, 4);
    const int64_t cat = fa_int(f, 5, 0);
    if (!data || strlen(data) > MAP_DATA_MAX) {
        social_send_n(s, "upMp", 1, cJSON_CreateString(data ? "Failed. 500k Character Game Data Limit" : "Invalid Map"));
        return;
    }
    if (cat < 0 || cat >= MAP_CATEGORY_COUNT) {
        social_send_n(s, "upMp", 1, cJSON_CreateString("Invalid Category"));
        return;
    }
    const char *err = NULL;
    cJSON *map = map_prepare_custom(data, 0, &err);
    if (!map) {
        social_send_n(s, "upMp", 1, cJSON_CreateString(err ? err : "Map error occured."));
        return;
    }
    char nm[64] = "";
    snprintf(nm, sizeof nm, "%s", kn_json_str(map, "name", ""));
    /* map_info {"m": [mode ids], "t": 1 with a thumbnail}: the Games window's tags (map_row_fields) */
    cJSON *info = cJSON_CreateObject();
    cJSON *modes = cJSON_AddArrayToObject(info, "m");
    if (thumb && strncmp(thumb, "data:image/png;base64,", 22) == 0 && strlen(thumb) <= THUMB_MAX) cJSON_AddNumberToObject(info, "t", 1); /* the v10 thumb flag */
    const cJSON *mode, *list = cJSON_GetObjectItemCaseSensitive(map, "modes");
    if (cJSON_IsArray(list)) {
        cJSON_ArrayForEach(mode, list) {
            if (cJSON_IsNumber(mode)) cJSON_AddItemToArray(modes, cJSON_CreateNumber(mode->valuedouble));
        }
    }
    char *info_s = cJSON_PrintUnformatted(info);
    cJSON_Delete(info);
    cJSON_Delete(map);
    if (thumb && (strncmp(thumb, "data:image/png;base64,", 22) != 0 || strlen(thumb) > THUMB_MAX)) thumb = NULL;
    for (size_t i = 0; i < map_catalog_count(); i++) {
        if (!strcasecmp(map_catalog_at(i)->name, nm)) {
            social_send_n(s, "upMp", 1, cJSON_CreateString("Map name taken"));
            free(info_s);
            return;
        }
    }
    char me[24], c[24];
    social_call(s, &P_PUBLISH_MAP,
                 KN_DB_ARGS(acct_itoa(me, s->account_id), nm, data, desc ? desc : "", video ? video : "",
                            acct_itoa(c, cat), thumb, info_s ? info_s : "{}"),
                 publish_done, NULL);
    free(info_s);
}

static void edit_map_done(SocialSession *s, kn_db_result *res, void *extra) {
    (void) extra;
    const int ok = kn_db_ok(res) && kn_db_rows(res) > 0;
    social_send_n(s, "edMp", 1, ok ? cJSON_CreateString(kn_db_get(res, 0, 0)) : cJSON_CreateNull());
}

/* ["edMp", name] -> ["edMp", mapJson | null] (own published maps; staff any) */
void sh_edit_map(SocialSession *s, const cJSON *f) {
    if (!social_require_auth(s)) return;
    char me[24];
    social_query(s,
                 SQL(.any = "SELECT map_data FROM published_maps WHERE lower(map_name) = lower($2) AND deleted_at IS NULL AND (creator_id = $1 "
                            "OR EXISTS (SELECT 1 FROM players WHERE player_id = $1 AND (player_developer > 0 OR player_mapmod > 0)))"),
                 KN_DB_ARGS(social_viewer(s, me), fa_str(f, 1) ? fa_str(f, 1) : ""), edit_map_done, NULL);
}

static void silent_done(SocialSession *s, kn_db_result *res, void *extra) {
    (void) extra;
    if (!kn_db_ok(res)) social_error(s, acct_db_error(res, "creation"));
}

/* ["fvM", id, kind 0 map | 1 mod, 1 fav | 0 unfav] (no reply) */
void sh_fav_creation(SocialSession *s, const cJSON *f) {
    if (s->account_id <= 0) return;
    char me[24], id[24], kind[24];
    acct_itoa(id, fa_int(f, 1, 0));
    acct_itoa(kind, fa_int(f, 2, 0) ? 1 : 0);
    if (fa_int(f, 3, 1))
        social_query(s, SQL(.pg = "INSERT INTO creation_favs (kind, creation_id, account_id) VALUES ($2, $3, $1) ON CONFLICT DO NOTHING",
                            .any = "{insert_ignore} INTO creation_favs (kind, creation_id, account_id) VALUES ($2, $3, $1) {ignore_conflicts}"),
                     KN_DB_ARGS(social_viewer(s, me), kind, id), silent_done, NULL);
    else
        social_query(s, SQL(.any = "DELETE FROM creation_favs WHERE kind = $2 AND creation_id = $3 AND account_id = $1"),
                     KN_DB_ARGS(social_viewer(s, me), kind, id), silent_done, NULL);
}

/* ["ftC", id, 1|0, kind] feature (devs; no reply) */
void sh_feature_creation(SocialSession *s, const cJSON *f) {
    if (s->account_id <= 0) return;
    char me[24], id[24], on[24];
    const char *sql = fa_int(f, 3, 0)
        ? "UPDATE published_mods SET mod_featured = $3 WHERE mod_id = $2 AND EXISTS (SELECT 1 FROM players WHERE player_id = $1 AND player_developer > 0)"
        : "UPDATE published_maps SET map_featured = $3 WHERE map_id = $2 AND EXISTS (SELECT 1 FROM players WHERE player_id = $1 AND player_developer > 0)";
    social_query(s, SQL(.any = sql), KN_DB_ARGS(social_viewer(s, me), acct_itoa(id, fa_int(f, 1, 0)), acct_itoa(on, fa_int(f, 2, 0) ? 1 : 0)),
                 silent_done, NULL);
}

static void delete_done(SocialSession *s, kn_db_result *res, void *extra) {
    (void) extra;
    if (kn_db_ok(res) && kn_db_affected(res) > 0)
        social_send_n(s, "po", 3, cJSON_CreateString("SUCCESS"), cJSON_CreateNull(), cJSON_CreateString("Deleted"));
    else
        social_error(s, kn_db_ok(res) ? "Not allowed" : acct_db_error(res, "delC"));
}

/* ["delC", id, kind 0 map | 1 mod | 2 asset] (creator, devs, creation mods) -> io_po */
void sh_delete_creation(SocialSession *s, const cJSON *f) {
    if (!social_require_auth(s)) return;
    char me[24], id[24];
    const int64_t kind = fa_int(f, 2, 0);
    /* creator, devs, creation mods; NOW = now() on postgres, {now} elsewhere */
#define DEL_SQL(TABLE, IDCOL, NOW)                                                                              \
    "UPDATE " TABLE " SET deleted_at = " NOW " WHERE " IDCOL " = $2 AND deleted_at IS NULL AND (creator_id = $1 OR " \
    "EXISTS (SELECT 1 FROM players WHERE player_id = $1 AND (player_developer > 0 OR player_mapmod > 0)))"
    static const kn_sql del_mod = {.pg = DEL_SQL("published_mods", "mod_id", "now()"),
                                   .any = DEL_SQL("published_mods", "mod_id", "{now}")},
                        del_asset = {.pg = DEL_SQL("published_assets", "id", "now()"),
                                     .any = DEL_SQL("published_assets", "id", "{now}")},
                        del_map = {.pg = DEL_SQL("published_maps", "map_id", "now()"),
                                   .any = DEL_SQL("published_maps", "map_id", "{now}")};
#undef DEL_SQL
    const kn_sql *sql = kind == 1 ? &del_mod : kind == 2 ? &del_asset : &del_map;
    social_query(s, sql, KN_DB_ARGS(social_viewer(s, me), acct_itoa(id, fa_int(f, 1, 0))), delete_done, NULL);
}

/* "uploadMo" / "uploadA" need the v3.1.1 file upload endpoints (/upload-user-mod,
 * /upload-user-asset), which this server does not host. */
void sh_upload_files(SocialSession *s, const cJSON *f) {
    const char *label = fa_str(f, 0);
    const int mod = label && !strcmp(label, "uploadMo");
    cJSON *fr = social_frame("upMp");
    cJSON_AddItemToArray(fr, cJSON_CreateString("File uploads are not supported on this server"));
    if (mod) cJSON_AddItemToArray(fr, cJSON_CreateNumber(1));
    social_send(s, fr);
}

/* ------------------------------------------------------------------ published map data for game servers */

static void map_data_done(kn_db_result *res, void *ud) {
    kn_http_req *req = ud;
    if (!kn_db_ok(res)) {
        acct_db_error(res, "map data");
        kn_http_respond_error(req, 500, "database error");
    } else if (kn_db_rows(res) < 1) {
        kn_http_respond_error(req, 404, "not_found");
    } else {
        const char *data = kn_db_get(res, 0, 0);
        kn_http_set_header(req, "Cache-Control", "no-cache");
        kn_http_respond(req, 200, "application/json", data, strlen(data));
    }
}

/* GET /api/map-data?name=<map>|id=<map_id> (alias /api/published-map) -> the published
 * map JSON (playing / hosting community maps); 404 {"error":"not_found"}. */
static void h_map_data(kn_http_req *req, void *ud) {
    (void) ud;
    char name[64] = "", id[24] = "";
    const int has_name = kn_http_query_get(req, "name", name, sizeof name) > 0;
    const int has_id = kn_http_query_get(req, "id", id, sizeof id) > 0;
    if (!has_name && !has_id) {
        kn_http_respond_error(req, 400, "name or id required");
        return;
    }
    if (!g_acct_brain || !g_acct_brain->db) {
        kn_http_respond_error(req, 404, "not_found");
        return;
    }
    char *end = NULL;
    const long long idv = has_id ? strtoll(id, &end, 10) : 0;
    if (has_id && (!end || *end || idv <= 0)) {
        kn_http_respond_error(req, 404, "not_found");
        return;
    }
    kn_db_query_sql(g_acct_brain->db,
                SQL(.pg = "SELECT map_data FROM published_maps WHERE deleted_at IS NULL AND "
                          "(($1 <> '' AND lower(map_name) = lower($1)) OR ($2::int > 0 AND map_id = $2::int))",
                    .any = "SELECT map_data FROM published_maps WHERE deleted_at IS NULL AND "
                           "(($1 <> '' AND lower(map_name) = lower($1)) OR ({int:$2} > 0 AND map_id = {int:$2}))"),
                KN_DB_ARGS(name, has_id ? id : "0"), map_data_done, req);
}

int creations_init(Brain *b) {
    if (!b->http) return 0;
    kn_http_route(b->http, "GET", "/api/maps", h_maps, NULL);
    kn_http_route(b->http, "GET", "/api/mods", h_mods, NULL);
    kn_http_route(b->http, "GET", "/api/assets", h_assets, NULL);
    kn_http_route(b->http, "GET", "/api/search", h_search, NULL);
    kn_http_route(b->http, "GET", "/api/streams", h_streams, NULL);
    kn_http_route(b->http, "GET", "/api/prices", h_prices, NULL);
    kn_http_route(b->http, "GET", "/api/map-data", h_map_data, NULL);
    kn_http_route(b->http, "GET", "/api/published-map", h_map_data, NULL);
    KN_INFO(MOD, "hub catalogue: %zu catalogue maps + published maps/mods/assets", map_catalog_count());
    return 0;
}
