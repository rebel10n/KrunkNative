/*
 * services/brain/streams_norm.c - Twitch Helix answers -> v10's /streams/top shape (streams.h).
 * Pure: no sockets, no config, no clock.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "streams.h"

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

cJSON *streams_normalise(const cJSON *data) {
    cJSON *out = cJSON_CreateObject();
    cJSON *list = cJSON_AddArrayToObject(out, "streams");
    cJSON_AddBoolToObject(out, "promo", 0);
    cJSON_AddItemToObject(out, "dropCampaigns", cJSON_CreateArray());

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
        cJSON_AddStringToObject(e, "username", login); /* twitch.tv/<username>, the card's link */
        cJSON_AddStringToObject(e, "title", str_of(s, "title", ""));
        cJSON_AddNumberToObject(e, "viewerCount", viewers);
        cJSON_AddStringToObject(e, "thumbnailUrl", thumb);
        cJSON_AddStringToObject(e, "languageCode", lang);
        cJSON_AddItemToObject(e, "eventTags", cJSON_CreateArray());
        cJSON_AddBoolToObject(e, "featured", 0);

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
