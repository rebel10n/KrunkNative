/*
 * services/brain/streams_norm.c - Twitch Helix answers -> v10's /streams/top shape (streams.h).
 * Pure: no sockets, no config, no clock.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "streams.h"

#include <strings.h>

#include "knsvc/crypto.h"

/* Twitch's thumbnail size for the card (the card is 180px wide, v10 svelte-60shhe) */
#define THUMB_W 440
#define THUMB_H 248

/* Twitch logins: letters, digits and '_', 1..25 characters */
static int login_ok(const char *s) {
    const size_t n = strlen(s);
    if (n < 1 || n > 25) return 0;
    for (size_t i = 0; i < n; i++) {
        const unsigned char c = (unsigned char) s[i];
        if (!isalnum(c) && c != '_') return 0;
    }
    return 1;
}

/* the Helix thumbnail template ("https://.../live_user_x-{width}x{height}.jpg") at the card size */
static void thumb_fill(const char *in, char *out, const size_t cap) {
    out[0] = 0;
    if (strncmp(in, "https://", 8) != 0) return;
    const char *w = strstr(in, "{width}");
    const char *h = strstr(in, "{height}");
    if (!w || !h || h < w) {
        snprintf(out, cap, "%s", in);
        return;
    }
    char wh[24];
    snprintf(wh, sizeof(wh), "%d", THUMB_W);
    char hh[24];
    snprintf(hh, sizeof(hh), "%d", THUMB_H);
    size_t k = 0;
    for (const char *p = in; *p && k + 1 < cap;) {
        if (p == w) {
            for (const char *q = wh; *q && k + 1 < cap; q++) out[k++] = *q;
            p += strlen("{width}");
        } else if (p == h) {
            for (const char *q = hh; *q && k + 1 < cap; q++) out[k++] = *q;
            p += strlen("{height}");
        } else {
            out[k++] = *p++;
        }
    }
    out[k] = 0;
}

static const char *str_of(const cJSON *o, const char *key, const char *def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : def;
}

static void lower_copy(char *out, const size_t cap, const char *in) {
    size_t k = 0;
    for (; in[k] && k + 1 < cap; k++) out[k] = (char) tolower((unsigned char) in[k]);
    out[k] = 0;
}

/* streams.featured: a login in the configured list (case-insensitive) */
static int featured_login(const cJSON *featured, const char *login) {
    const cJSON *f = NULL;
    cJSON_ArrayForEach(f, featured) {
        if (cJSON_IsString(f) && !strcasecmp(f->valuestring, login)) return 1;
    }
    return 0;
}

cJSON *streams_normalise(const cJSON *data, const cJSON *featured, int promo, const cJSON *campaigns) {
    cJSON *out = cJSON_CreateObject();
    cJSON *list = cJSON_AddArrayToObject(out, "streams");
    cJSON_AddBoolToObject(out, "promo", promo ? 1 : 0);
    cJSON_AddItemToObject(out, "dropCampaigns", cJSON_IsArray(campaigns) ? cJSON_Duplicate(campaigns, 1) : cJSON_CreateArray());

    /* the kept rows, then sorted by viewers (insertion sort: stable, the list is at most 100) */
    cJSON *rows[100];
    int n = 0;
    const cJSON *s = NULL;
    cJSON_ArrayForEach(s, data) {
        if (n >= 100) break;
        if (strcmp(str_of(s, "type", ""), "live") != 0) continue;
        char login[64];
        lower_copy(login, sizeof(login), str_of(s, "user_login", ""));
        if (!login_ok(login)) continue;
        char thumb[320];
        thumb_fill(str_of(s, "thumbnail_url", ""), thumb, sizeof(thumb));
        char lang[16];
        lower_copy(lang, sizeof(lang), str_of(s, "language", ""));
        const cJSON *vc = cJSON_GetObjectItemCaseSensitive(s, "viewer_count");
        const double viewers = cJSON_IsNumber(vc) && vc->valuedouble > 0 ? vc->valuedouble : 0;

        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "id", str_of(s, "id", ""));
        cJSON_AddStringToObject(e, "username", login); /* twitch.tv/<username>, the card's link */
        cJSON_AddStringToObject(e, "title", str_of(s, "title", ""));
        cJSON_AddNumberToObject(e, "viewerCount", viewers);
        cJSON_AddStringToObject(e, "thumbnailUrl", thumb);
        cJSON_AddStringToObject(e, "startedAt", str_of(s, "started_at", ""));
        cJSON_AddStringToObject(e, "provider", "twitch");
        cJSON_AddStringToObject(e, "languageCode", lang);
        cJSON_AddBoolToObject(e, "featured", featured_login(featured, login));

        int j = n - 1;
        while (j >= 0 && cJSON_GetObjectItemCaseSensitive(rows[j], "viewerCount")->valuedouble < viewers) {
            rows[j + 1] = rows[j];
            j--;
        }
        rows[j + 1] = e;
        n++;
    }
    for (int i = 0; i < n; i++) cJSON_AddItemToArray(list, rows[i]);
    return out;
}

int streams_parse_token(const char *body, char *token, const size_t cap, long *expires_in) {
    if (!body || !token || cap < 2) return 0;
    cJSON *root = cJSON_Parse(body);
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(root, "access_token");
    int ok = 0;
    if (cJSON_IsString(t) && t->valuestring[0]) {
        snprintf(token, cap, "%s", t->valuestring);
        const cJSON *ex = cJSON_GetObjectItemCaseSensitive(root, "expires_in");
        if (expires_in) *expires_in = cJSON_IsNumber(ex) ? (long) ex->valuedouble : 0;
        ok = 1;
    }
    cJSON_Delete(root);
    return ok;
}

int streams_parse_game_id(const char *body, char *id, const size_t cap) {
    if (!body || !id || cap < 2) return 0;
    cJSON *root = cJSON_Parse(body);
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    const cJSON *first = cJSON_IsArray(data) ? cJSON_GetArrayItem(data, 0) : NULL;
    const cJSON *gid = cJSON_GetObjectItemCaseSensitive(first, "id");
    int ok = 0;
    if (cJSON_IsString(gid) && gid->valuestring[0]) {
        snprintf(id, cap, "%s", gid->valuestring);
        ok = 1;
    }
    cJSON_Delete(root);
    return ok;
}

/* ------------------------------------------------------------------ stream thumbnails */

#define THUMB_URL_MAX 400

static int ends_ci(const char *s, const size_t n, const char *suffix) {
    const size_t l = strlen(suffix);
    return n >= l && !strncasecmp(s + n - l, suffix, l);
}

int streams_thumb_url_ok(const char *url, const char *const *hosts, const size_t n, char *path, const size_t cap) {
    if (path && cap) path[0] = 0;
    if (!url || strncasecmp(url, "https://", 8) != 0) return 0;
    const size_t len = strlen(url);
    if (len > THUMB_URL_MAX) return 0;
    const char *host = url + 8;
    const char *slash = strchr(host, '/');
    if (!slash || slash == host) return 0;
    const size_t host_len = (size_t) (slash - host);
    for (size_t i = 0; i < host_len; i++) { /* a plain DNS name: no user info, port, IPv6 brackets */
        const unsigned char c = (unsigned char) host[i];
        if (!isalnum(c) && c != '.' && c != '-') return 0;
    }
    int allowed = 0;
    for (size_t i = 0; i < n && !allowed; i++)
        allowed = hosts[i] && strlen(hosts[i]) == host_len && !strncasecmp(hosts[i], host, host_len);
    if (!allowed) return 0;
    const size_t path_len = len - (size_t) (slash - url);
    for (size_t i = 0; i < path_len; i++) {
        const unsigned char c = (unsigned char) slash[i];
        if (!isalnum(c) && c != '-' && c != '.' && c != '_' && c != '~' && c != '/') return 0;
        if (c == '/' && (slash[i + 1] == '/' || (slash[i + 1] == '.' && (slash[i + 2] == '/' || slash[i + 2] == 0 ||
                                                                         (slash[i + 2] == '.' && (slash[i + 3] == '/' || slash[i + 3] == 0))))))
            return 0;
    }
    if (!ends_ci(slash, path_len, ".jpg") && !ends_ci(slash, path_len, ".jpeg") && !ends_ci(slash, path_len, ".png")) return 0;
    if (path) {
        if (path_len >= cap) return 0;
        memcpy(path, slash, path_len + 1);
    }
    return 1;
}

const char *streams_image_mime(const unsigned char *b, const size_t len) {
    if (!b) return NULL;
    if (len >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF) return "image/jpeg";
    if (len >= 8 && !memcmp(b, "\x89PNG\r\n\x1a\n", 8)) return "image/png";
    return NULL;
}

char *streams_data_url(const char *mime, const unsigned char *bytes, const size_t len) {
    char head[48];
    const int h = snprintf(head, sizeof(head), "data:%s;base64,", mime ? mime : "application/octet-stream");
    if (h < 0 || (size_t) h >= sizeof(head)) return NULL;
    const size_t b64 = 4 * ((len + 2) / 3) + 1;
    char *out = malloc((size_t) h + b64);
    if (!out) return NULL;
    memcpy(out, head, (size_t) h);
    if (kn_base64_encode(bytes, len, out + h, b64, 0, 1) < 0) {
        free(out);
        return NULL;
    }
    return out;
}
