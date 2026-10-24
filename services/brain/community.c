/*
 * services/brain/community.c - the community feed of the Krunker v10.0.2 menu
 * (doc/backend.md "Community feed"; client/src/ui/v10_news.c):
 *
 *   GET /creators/events -> {"data":{"events":[event...],"polls":[poll...]}}
 *       v10 gapiClient.creators.getUpcomingEvents(): the Community & Events popup
 *   GET /social/news     -> {"data":{"news":["<html>", ...]}}
 *       v10 gapiClient.social.getNews(): the hub's News tab; Krunker Box Office's What's New
 *       window shows the same items
 *
 * Shapes are v10's gapi answers (event: id, player_id, player_name, title, description,
 * banner_url, start_date, duration_hours, ended_at, links [{label, url}], tags; poll: id, title,
 * description, ends_in seconds). Sources:
 *   - the operators' feed file, `brain.community.feed` (default config/community.json, resolved
 *     against the config root; config/community.example.json shows the format). It is read again
 *     when its modification time changes, so editing it needs no restart. Events whose end
 *     (ended_at, else start_date + duration_hours) has passed and polls past ends_at are left out.
 *     News entries are plain text, or {text, url, label}: the brain writes their HTML itself
 *     (escaped text, one link), so the feed never carries markup to the clients.
 *   - with a database, the published maps and mods staff featured (map_featured / mod_featured,
 *     the hub's Featured tab), newest first, as events tagged "Featured Map" / "Featured Mod" with
 *     Play / Details links into the game (/?play=<name>, /social.html?p=map&q=<name>). Their
 *     start_date is the last update and ended_at is set to it, so v10's card shows that date and
 *     no LIVE badge (KN addition: v10 lists creator events only).
 * No feed and no featured creation answers empty lists (the popup's "No upcoming events").
 *
 * The same file's "gameEvents" are the game's live events (Krunker v10.0.3 game.ongoingEvents, which the
 * official host sets from its events API; doc/v10/kill-pools.md): {id, name, startsAt, endsAt, killPools
 * [{weight, contents [{content_type, content_ref}]}], killPoolsWeight, killPoolsMode, killPoolsIcon}. The
 * ones running now (startsAt <= now < endsAt, no endsAt: open-ended) go to the game servers in the
 * internal WS "welcome" and in a "game-events" push whenever the set changes (internal_ws.c), and their
 * kill pools pay items at the end of public matches (shared/src/kill_pools.c).
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "brain_internal.h"
#include "db_procs.h"
#include "knsvc/config.h"
#include "knsvc/db.h"
#include "knsvc/http.h"
#include "knsvc/json.h"
#include "knsvc/log.h"

#define MOD "community"
#define FEATURED_MAX 12
#define FEED_MAX_BYTES (1024 * 1024)

static struct {
    Brain *b;
    char path[1024];
    time_t mtime;
    off_t size;
    int loaded;
    cJSON *feed; /* the parsed file, NULL when absent / invalid */
} C;

#define GAME_EVENTS_MAX 8 /* KILL_POOL_EVENTS_MAX: what a game server keeps */

/* ------------------------------------------------------------------ time */

/* "2026-10-20T18:00:00Z" / "2026-10-20T18:00:00.000Z" / "2026-10-20 18:00:00" (UTC) -> unix s; -1 */
static int64_t parse_iso(const char *s) {
    if (!s) return -1;
    int y, mo, d, h = 0, mi = 0, sec = 0;
    if (sscanf(s, "%4d-%2d-%2d", &y, &mo, &d) != 3) return -1;
    if (strlen(s) > 10 && (s[10] == 'T' || s[10] == ' ')) {
        if (sscanf(s + 11, "%2d:%2d:%2d", &h, &mi, &sec) < 2) return -1;
    }
    struct tm tm = {0};
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_sec = sec;
    return (int64_t) timegm(&tm);
}

static void format_iso(const int64_t t, char *out, const size_t n) {
    const time_t tt = (time_t) t;
    struct tm tm;
    gmtime_r(&tt, &tm);
    strftime(out, n, "%Y-%m-%dT%H:%M:%S.000Z", &tm);
}

/* ------------------------------------------------------------------ feed file */

static void feed_reload(void) {
    struct stat st;
    if (!C.path[0] || stat(C.path, &st) != 0) {
        if (C.feed) KN_INFO(MOD, "feed %s is gone: empty feed", C.path);
        cJSON_Delete(C.feed);
        C.feed = NULL;
        C.loaded = 0;
        return;
    }
    if (C.loaded && st.st_mtime == C.mtime && st.st_size == C.size) return;
    C.loaded = 1;
    C.mtime = st.st_mtime;
    C.size = st.st_size;
    cJSON_Delete(C.feed);
    C.feed = NULL;
    if (st.st_size <= 0 || st.st_size > FEED_MAX_BYTES) {
        KN_WARN(MOD, "feed %s: %lld bytes, ignored (at most %d)", C.path, (long long) st.st_size, FEED_MAX_BYTES);
        return;
    }
    FILE *f = fopen(C.path, "rb");
    if (!f) return;
    char *text = malloc((size_t) st.st_size + 1);
    const size_t got = text ? fread(text, 1, (size_t) st.st_size, f) : 0;
    fclose(f);
    if (!text) return;
    text[got] = 0;
    C.feed = cJSON_Parse(text);
    free(text);
    if (!cJSON_IsObject(C.feed)) {
        KN_WARN(MOD, "feed %s is not a JSON object: ignored", C.path);
        cJSON_Delete(C.feed);
        C.feed = NULL;
        return;
    }
    KN_INFO(MOD, "feed %s: %d news, %d events, %d polls", C.path, cJSON_GetArraySize(cJSON_GetObjectItem(C.feed, "news")),
            cJSON_GetArraySize(cJSON_GetObjectItem(C.feed, "events")), cJSON_GetArraySize(cJSON_GetObjectItem(C.feed, "polls")));
}

static const cJSON *feed_list(const char *key) {
    feed_reload();
    const cJSON *a = C.feed ? cJSON_GetObjectItem(C.feed, key) : NULL;
    return cJSON_IsArray(a) ? a : NULL;
}

static void copy_str(cJSON *dst, const cJSON *src, const char *key) {
    const char *s = cJSON_GetStringValue(cJSON_GetObjectItem(src, key));
    if (s) cJSON_AddStringToObject(dst, key, s);
    else cJSON_AddNullToObject(dst, key);
}

static void copy_num(cJSON *dst, const cJSON *src, const char *key) {
    const cJSON *v = cJSON_GetObjectItem(src, key);
    if (cJSON_IsNumber(v)) cJSON_AddNumberToObject(dst, key, v->valuedouble);
    else cJSON_AddNullToObject(dst, key);
}

/* One feed event in v10's shape, or NULL when it is malformed or over. */
static cJSON *feed_event(const cJSON *e, const int64_t now) {
    const char *title = cJSON_GetStringValue(cJSON_GetObjectItem(e, "title"));
    const char *start = cJSON_GetStringValue(cJSON_GetObjectItem(e, "start_date"));
    const int64_t t0 = parse_iso(start);
    if (!title || !title[0] || t0 < 0) return NULL;
    const char *ended = cJSON_GetStringValue(cJSON_GetObjectItem(e, "ended_at"));
    const cJSON *dur = cJSON_GetObjectItem(e, "duration_hours");
    if (ended && ended[0] && parse_iso(ended) >= 0 && parse_iso(ended) <= now) return NULL;
    if (cJSON_IsNumber(dur) && dur->valuedouble > 0 && t0 + (int64_t) (dur->valuedouble * 3600.0) <= now) return NULL;
    cJSON *o = cJSON_CreateObject();
    copy_num(o, e, "id");
    copy_num(o, e, "player_id");
    copy_str(o, e, "player_name");
    cJSON_AddStringToObject(o, "title", title);
    copy_str(o, e, "description");
    copy_str(o, e, "banner_url");
    char iso[40];
    format_iso(t0, iso, sizeof iso);
    cJSON_AddStringToObject(o, "start_date", iso);
    copy_num(o, e, "duration_hours");
    copy_str(o, e, "ended_at");
    cJSON *links = cJSON_AddArrayToObject(o, "links");
    const cJSON *l;
    cJSON_ArrayForEach(l, cJSON_GetObjectItem(e, "links")) {
        const char *url = cJSON_GetStringValue(cJSON_GetObjectItem(l, "url"));
        if (!url || !url[0]) continue;
        cJSON *lo = cJSON_CreateObject();
        cJSON_AddStringToObject(lo, "url", url);
        copy_str(lo, l, "label");
        cJSON_AddItemToArray(links, lo);
    }
    cJSON *tags = cJSON_AddArrayToObject(o, "tags");
    const cJSON *t;
    cJSON_ArrayForEach(t, cJSON_GetObjectItem(e, "tags")) {
        if (cJSON_IsString(t) && t->valuestring[0]) cJSON_AddItemToArray(tags, cJSON_CreateString(t->valuestring));
    }
    return o;
}

static int by_start(const void *a, const void *b) {
    const cJSON *x = *(const cJSON *const *) a, *y = *(const cJSON *const *) b;
    const int64_t tx = parse_iso(cJSON_GetStringValue(cJSON_GetObjectItem(x, "start_date")));
    const int64_t ty = parse_iso(cJSON_GetStringValue(cJSON_GetObjectItem(y, "start_date")));
    return tx < ty ? -1 : tx > ty;
}

/* The feed's upcoming / live events, soonest first, appended to `out`. */
static void feed_events(cJSON *out, const int64_t now) {
    cJSON *list[256];
    int n = 0;
    const cJSON *e;
    cJSON_ArrayForEach(e, feed_list("events")) {
        cJSON *o = n < 256 ? feed_event(e, now) : NULL;
        if (o) list[n++] = o;
    }
    qsort(list, (size_t) n, sizeof list[0], by_start);
    for (int i = 0; i < n; i++) cJSON_AddItemToArray(out, list[i]);
}

static void feed_polls(cJSON *out, const int64_t now) {
    const cJSON *p;
    cJSON_ArrayForEach(p, feed_list("polls")) {
        const char *title = cJSON_GetStringValue(cJSON_GetObjectItem(p, "title"));
        const char *url = cJSON_GetStringValue(cJSON_GetObjectItem(p, "url"));
        const int64_t end = parse_iso(cJSON_GetStringValue(cJSON_GetObjectItem(p, "ends_at")));
        if (!title || !title[0] || !url || !url[0] || end <= now) continue;
        cJSON *o = cJSON_CreateObject();
        copy_num(o, p, "id");
        cJSON_AddStringToObject(o, "title", title);
        copy_str(o, p, "description");
        cJSON_AddNumberToObject(o, "ends_in", (double) (end - now));
        cJSON_AddStringToObject(o, "url", url); /* KN: v10 links every poll to community.krunker.io/polls/<id> */
        cJSON_AddItemToArray(out, o);
    }
}

typedef struct {
    char *s;
    size_t len, cap;
} Buf;

static void buf_add(Buf *b, const char *s, const size_t n) {
    if (b->len + n + 1 > b->cap) {
        const size_t nc = (b->len + n + 1) * 2 + 64;
        char *g = realloc(b->s, nc);
        if (!g) return;
        b->s = g;
        b->cap = nc;
    }
    memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = 0;
}

static void buf_raw(Buf *b, const char *s) { buf_add(b, s, strlen(s)); }

static void buf_esc(Buf *b, const char *s) {
    for (; s && *s; s++) {
        const char *rep = *s == '&' ? "&amp;" : *s == '<' ? "&lt;" : *s == '>' ? "&gt;" : *s == '"' ? "&quot;" : *s == '\'' ? "&#39;" : NULL;
        if (rep) buf_raw(b, rep);
        else buf_add(b, s, 1);
    }
}

static int safe_url(const char *u) {
    return u && (!strncmp(u, "https://", 8) || !strncmp(u, "http://", 7) || (u[0] == '/' && u[1] != '/'));
}

/* One news item's HTML (the client inserts it as markup, like v10's {@html}): escaped text,
 * then the optional link. NULL for an empty item. */
static char *news_html(const cJSON *n) {
    const char *text = cJSON_IsString(n) ? n->valuestring : cJSON_GetStringValue(cJSON_GetObjectItem(n, "text"));
    if (!text || !text[0]) return NULL;
    Buf b = {0};
    buf_esc(&b, text);
    const char *url = cJSON_IsObject(n) ? cJSON_GetStringValue(cJSON_GetObjectItem(n, "url")) : NULL;
    if (safe_url(url)) {
        const char *label = cJSON_GetStringValue(cJSON_GetObjectItem(n, "label"));
        buf_raw(&b, " <a target=\"_blank\" href=\"");
        buf_esc(&b, url);
        buf_raw(&b, "\">");
        buf_esc(&b, label && label[0] ? label : url);
        buf_raw(&b, "</a>");
    }
    return b.s;
}

/* ------------------------------------------------------------------ routes */

static void reply_data(kn_http_req *req, cJSON *data) {
    cJSON *res = cJSON_CreateObject();
    cJSON_AddItemToObject(res, "data", data);
    brain_reply_json(req, 200, res);
    cJSON_Delete(res);
}

static void h_news(kn_http_req *req, void *ud) {
    (void) ud;
    cJSON *data = cJSON_CreateObject();
    cJSON *news = cJSON_AddArrayToObject(data, "news");
    const cJSON *n;
    cJSON_ArrayForEach(n, feed_list("news")) {
        char *html = news_html(n);
        if (html) cJSON_AddItemToArray(news, cJSON_CreateString(html));
        free(html);
    }
    reply_data(req, data);
}

typedef struct {
    kn_http_req *req;
    cJSON *data;
} EventsReq;

static void url_encode(char *out, const size_t n, const char *s) {
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (; s && *s && o + 4 < n; s++) {
        const unsigned char c = (unsigned char) *s;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out[o++] = (char) c;
        } else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 15];
        }
    }
    out[o] = 0;
}

static void add_link(cJSON *links, const char *label, const char *url) {
    cJSON *l = cJSON_CreateObject();
    cJSON_AddStringToObject(l, "label", label);
    cJSON_AddStringToObject(l, "url", url);
    cJSON_AddItemToArray(links, l);
}

/* A featured published map / mod row -> a v10 event card */
static cJSON *featured_event(const cJSON *row) {
    const int mod = (int) kn_json_int(row, "kind", 0) == 1;
    const char *name = kn_json_str(row, "name", "");
    if (!name[0]) return NULL;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "id", (double) kn_json_int(row, "id", 0));
    cJSON_AddNullToObject(o, "player_id");
    cJSON_AddStringToObject(o, "player_name", kn_json_str(row, "creator", "Community"));
    cJSON_AddStringToObject(o, "title", name);
    cJSON_AddStringToObject(o, "description", kn_json_str(row, "descr", ""));
    const char *img = kn_json_str(row, "img", "");
    if (!strncmp(img, "https://", 8) || !strncmp(img, "http://", 7)) cJSON_AddStringToObject(o, "banner_url", img);
    else cJSON_AddNullToObject(o, "banner_url");
    char iso[40];
    format_iso((int64_t) (kn_json_num(row, "ms", 0) / 1000.0), iso, sizeof iso);
    cJSON_AddStringToObject(o, "start_date", iso);
    cJSON_AddNullToObject(o, "duration_hours");
    cJSON_AddStringToObject(o, "ended_at", iso);
    cJSON *links = cJSON_AddArrayToObject(o, "links");
    char enc[300], url[400];
    url_encode(enc, sizeof enc, name);
    if (!mod) {
        snprintf(url, sizeof url, "/?play=%s", enc);
        add_link(links, "Play", url);
    }
    snprintf(url, sizeof url, "/social.html?p=%s&q=%s", mod ? "mod" : "map", enc);
    add_link(links, "Details", url);
    cJSON *tags = cJSON_AddArrayToObject(o, "tags");
    cJSON_AddItemToArray(tags, cJSON_CreateString(mod ? "Featured Mod" : "Featured Map"));
    return o;
}

static void featured_done(kn_db_result *res, void *ud) {
    EventsReq *r = ud;
    cJSON *events = cJSON_GetObjectItem(r->data, "events");
    if (kn_db_ok(res)) {
        cJSON *rows = kn_db_result_to_json(res);
        const cJSON *row;
        cJSON_ArrayForEach(row, rows) {
            cJSON *e = featured_event(row);
            if (e) cJSON_AddItemToArray(events, e);
        }
        cJSON_Delete(rows);
    } else {
        KN_WARN(MOD, "featured creations: %s", kn_db_error(res));
    }
    if (kn_http_req_alive(r->req)) reply_data(r->req, r->data);
    else cJSON_Delete(r->data);
    free(r);
}

static void h_events(kn_http_req *req, void *ud) {
    (void) ud;
    const int64_t now = (int64_t) time(NULL);
    cJSON *data = cJSON_CreateObject();
    feed_events(cJSON_AddArrayToObject(data, "events"), now);
    feed_polls(cJSON_AddArrayToObject(data, "polls"), now);
    EventsReq *r = C.b && C.b->db ? calloc(1, sizeof *r) : NULL;
    if (!r) {
        reply_data(req, data);
        return;
    }
    r->req = req;
    r->data = data;
    char limit[16];
    snprintf(limit, sizeof limit, "%d", FEATURED_MAX);
    kn_db_query_sql(C.b->db,
                    SQL(.any = "SELECT 0 AS kind__i, m.map_id AS id__i, m.map_name AS name__t, m.map_description AS descr__t, "
                               "coalesce(m.map_image, '') AS img__t, coalesce(a.username, 'Community') AS creator__t, "
                               "{epoch_ms:m.map_date} AS ms__f FROM published_maps m LEFT JOIN accounts a ON a.id = m.creator_id "
                               "WHERE m.deleted_at IS NULL AND m.map_featured > 0 "
                               "UNION ALL SELECT 1, d.mod_id, d.mod_name, d.mod_description, coalesce(d.mod_image, ''), "
                               "coalesce(a.username, 'Community'), {epoch_ms:d.mod_date} FROM published_mods d "
                               "LEFT JOIN accounts a ON a.id = d.creator_id WHERE d.deleted_at IS NULL AND d.mod_featured > 0 "
                               "ORDER BY 7 DESC LIMIT {int:$1}"),
                    KN_DB_ARGS(limit), featured_done, r);
}

/* The feed's game events running at `now_ms` (unix ms), in v10's fields, soonest start first: a new array. */
cJSON *community_game_events(const int64_t now_ms) {
    cJSON *out = cJSON_CreateArray();
    const int64_t now = now_ms / 1000;
    const cJSON *e;
    cJSON_ArrayForEach(e, feed_list("gameEvents")) {
        if (!cJSON_IsObject(e) || cJSON_GetArraySize(out) >= GAME_EVENTS_MAX) continue;
        const int64_t start = parse_iso(cJSON_GetStringValue(cJSON_GetObjectItem(e, "startsAt")));
        const char *ends = cJSON_GetStringValue(cJSON_GetObjectItem(e, "endsAt"));
        const int64_t end = ends && ends[0] ? parse_iso(ends) : -1;
        if (start < 0 || start > now || (end >= 0 && end <= now)) continue;
        cJSON *o = cJSON_CreateObject();
        copy_num(o, e, "id");
        copy_str(o, e, "name");
        char iso[40];
        format_iso(start, iso, sizeof iso);
        cJSON_AddStringToObject(o, "startsAt", iso);
        copy_num(o, e, "killPoolsWeight");
        copy_str(o, e, "killPoolsMode");
        copy_str(o, e, "killPoolsIcon");
        const cJSON *pools = cJSON_GetObjectItem(e, "killPools");
        cJSON_AddItemToObject(o, "killPools", cJSON_IsArray(pools) ? cJSON_Duplicate(pools, 1) : cJSON_CreateArray());
        /* soonest start first (getKillReward sorts by startsAt as well) */
        int at = 0;
        const cJSON *x;
        cJSON_ArrayForEach(x, out) {
            if (parse_iso(cJSON_GetStringValue(cJSON_GetObjectItem(x, "startsAt"))) > start) break;
            at++;
        }
        cJSON_InsertItemInArray(out, at, o);
    }
    return out;
}

int community_init(Brain *b) {
    C.b = b;
    const char *feed = kn_config_str(b->cfg, "brain.community.feed", "config/community.json");
    if (feed && feed[0] && !kn_config_resolve_path(b->cfg, feed, C.path, sizeof C.path)) C.path[0] = 0;
    if (!b->http) {
        feed_reload(); /* the game events still reach the game servers */
        return 0;
    }
    kn_http_route(b->http, "GET", "/creators/events", h_events, NULL);
    kn_http_route(b->http, "GET", "/social/news", h_news, NULL);
    feed_reload();
    KN_INFO(MOD, "community feed on /creators/events, /social/news (%s%s)", C.path[0] ? C.path : "no feed file",
            C.feed ? "" : C.path[0] ? ": absent" : "");
    return 0;
}

void community_shutdown(Brain *b) {
    (void) b;
    cJSON_Delete(C.feed);
    C.feed = NULL;
    C.loaded = 0;
}
