/* Unit test of the pure parts of the Live Streams list (streams_norm.c): no network, no config. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "streams.h"

static int g_fail;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)

static const char *k_helix =
    "["
    " {\"id\":\"1\",\"user_login\":\"zeta_one\",\"title\":\"Zombie labs\",\"viewer_count\":12,\"type\":\"live\","
    "  \"language\":\"EN\",\"thumbnail_url\":\"https://static-cdn.jtvnw.net/x-{width}x{height}.jpg\"},"
    " {\"id\":\"2\",\"user_login\":\"Replay_Guy\",\"title\":\"VOD\",\"viewer_count\":900,\"type\":\"vod\","
    "  \"thumbnail_url\":\"https://x/{width}x{height}.jpg\"},"
    " {\"id\":\"3\",\"user_login\":\"bad login!\",\"title\":\"injected\",\"viewer_count\":5000,\"type\":\"live\"},"
    " {\"id\":\"4\",\"user_login\":\"alpha\",\"title\":\"Parkour\",\"viewer_count\":12,\"type\":\"live\","
    "  \"thumbnail_url\":\"https://x/a-{width}x{height}.jpg\"},"
    " {\"id\":\"5\",\"user_login\":\"big\",\"title\":\"Top\",\"viewer_count\":300,\"type\":\"live\","
    "  \"thumbnail_url\":\"https://x/b-{width}x{height}.jpg\"}"
    "]";

int main(void) {
    /* a populated list: live rows only, valid logins, sorted by viewers (stable), thumbnails filled */
    cJSON *data = cJSON_Parse(k_helix);
    CHECK(data != NULL);
    cJSON *out = streams_normalise(data);
    CHECK(out != NULL);
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(out, "streams");
    CHECK(cJSON_IsArray(list));
    CHECK(cJSON_GetArraySize(list) == 3); /* the vod and the bad login are dropped */
    const cJSON *a = cJSON_GetArrayItem(list, 0), *b = cJSON_GetArrayItem(list, 1);
    const cJSON *c = cJSON_GetArrayItem(list, 2);
    CHECK(a && !strcmp(cJSON_GetObjectItem(a, "username")->valuestring, "big"));
    CHECK(b && !strcmp(cJSON_GetObjectItem(b, "username")->valuestring, "zeta_one")); /* stable: input order */
    CHECK(c && !strcmp(cJSON_GetObjectItem(c, "username")->valuestring, "alpha"));
    CHECK(a && (int) cJSON_GetObjectItem(a, "viewerCount")->valuedouble == 300);
    CHECK(b && !strcmp(cJSON_GetObjectItem(b, "thumbnailUrl")->valuestring, "https://static-cdn.jtvnw.net/x-440x248.jpg"));
    CHECK(b && !strcmp(cJSON_GetObjectItem(b, "languageCode")->valuestring, "en"));
    CHECK(cJSON_IsFalse(cJSON_GetObjectItem(out, "promo")));
    CHECK(cJSON_IsArray(cJSON_GetObjectItem(out, "dropCampaigns")));
    cJSON_Delete(out);

    /* no rows / no data: an empty list, the empty state */
    cJSON *none = streams_normalise(NULL);
    CHECK(none && cJSON_GetArraySize(cJSON_GetObjectItem(none, "streams")) == 0);
    cJSON_Delete(none);
    cJSON *empty = cJSON_Parse("[]");
    cJSON *none2 = streams_normalise(empty);
    CHECK(none2 && cJSON_GetArraySize(cJSON_GetObjectItem(none2, "streams")) == 0);
    cJSON_Delete(none2);
    cJSON_Delete(empty);
    cJSON_Delete(data);

    /* the token and the game id answers */
    char token[512] = "";
    long expires = 0;
    CHECK(streams_parse_token("{\"access_token\":\"abc123\",\"expires_in\":5011271,\"token_type\":\"bearer\"}", token, sizeof token, &expires));
    CHECK(!strcmp(token, "abc123") && expires == 5011271);
    CHECK(!streams_parse_token("{\"status\":400,\"message\":\"invalid client\"}", token, sizeof token, &expires));
    CHECK(!streams_parse_token("not json", token, sizeof token, &expires));

    char gid[32] = "";
    CHECK(streams_parse_game_id("{\"data\":[{\"id\":\"512345\",\"name\":\"Krunker\"}]}", gid, sizeof gid));
    CHECK(!strcmp(gid, "512345"));
    CHECK(!streams_parse_game_id("{\"data\":[]}", gid, sizeof gid));

    /* stream thumbnails: the allow-list (https, exact host, plain image path) */
    const char *hosts[] = {"static-cdn.jtvnw.net", "clips-media-assets2.twitch.tv"};
    char path[512];
    CHECK(streams_thumb_url_ok("https://static-cdn.jtvnw.net/previews-ttv/live_user_zeta_one-440x248.jpg", hosts, 2, path, sizeof path));
    CHECK(!strcmp(path, "/previews-ttv/live_user_zeta_one-440x248.jpg"));
    CHECK(streams_thumb_url_ok("https://STATIC-CDN.jtvnw.net/a.PNG", hosts, 2, path, sizeof path));
    CHECK(streams_thumb_url_ok("https://clips-media-assets2.twitch.tv/x/y-preview.jpeg", hosts, 2, NULL, 0));
    static const char *const k_refused[] = {
        "http://static-cdn.jtvnw.net/a.jpg",                       /* not https */
        "https://static-cdn.jtvnw.net.evil.example/a.jpg",         /* another host */
        "https://evil.example/static-cdn.jtvnw.net/a.jpg",
        "https://xstatic-cdn.jtvnw.net/a.jpg",
        "https://user@static-cdn.jtvnw.net/a.jpg",                 /* user info */
        "https://static-cdn.jtvnw.net@evil.example/a.jpg",
        "https://static-cdn.jtvnw.net:8443/a.jpg",                 /* a port */
        "https://static-cdn.jtvnw.net/a.jpg?x=1",                  /* a query */
        "https://static-cdn.jtvnw.net/a.jpg#f",                    /* a fragment */
        "https://static-cdn.jtvnw.net/%2e%2e/a.jpg",               /* escapes */
        "https://static-cdn.jtvnw.net/../a.jpg",                   /* dot segments */
        "https://static-cdn.jtvnw.net/x/./a.jpg",
        "https://static-cdn.jtvnw.net//evil.example/a.jpg",        /* "//" */
        "https://static-cdn.jtvnw.net/a.gif",                      /* not jpg / png */
        "https://static-cdn.jtvnw.net/a.svg",
        "https://static-cdn.jtvnw.net/",
        "https://static-cdn.jtvnw.net",
        "https:///a.jpg",
        "file:///etc/passwd.jpg",
        "",
    };
    for (size_t i = 0; i < sizeof(k_refused) / sizeof(k_refused[0]); i++) {
        if (streams_thumb_url_ok(k_refused[i], hosts, 2, path, sizeof path)) {
            fprintf(stderr, "allowed but should not be: %s\n", k_refused[i]);
            g_fail = 1;
        }
    }
    CHECK(!streams_thumb_url_ok(NULL, hosts, 2, path, sizeof path));
    CHECK(!streams_thumb_url_ok("https://static-cdn.jtvnw.net/a.jpg", hosts, 0, path, sizeof path)); /* no hosts: nothing */
    char longurl[600];
    snprintf(longurl, sizeof longurl, "https://static-cdn.jtvnw.net/%0450d.jpg", 0);
    CHECK(!streams_thumb_url_ok(longurl, hosts, 2, path, sizeof path)); /* over 400 characters */
    CHECK(!streams_thumb_url_ok("https://static-cdn.jtvnw.net/abcdef.jpg", hosts, 2, path, 8)); /* the path does not fit */

    /* the image sniffing and the data: URL */
    const unsigned char jpg[] = {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10};
    const unsigned char png[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n', 0};
    CHECK(streams_image_mime(jpg, sizeof jpg) && !strcmp(streams_image_mime(jpg, sizeof jpg), "image/jpeg"));
    CHECK(streams_image_mime(png, sizeof png) && !strcmp(streams_image_mime(png, sizeof png), "image/png"));
    CHECK(!streams_image_mime((const unsigned char *) "<html>", 6));
    CHECK(!streams_image_mime((const unsigned char *) "GIF89a", 6));
    CHECK(!streams_image_mime(jpg, 2));
    char *du = streams_data_url("image/jpeg", jpg, sizeof jpg);
    CHECK(du && !strcmp(du, "data:image/jpeg;base64,/9j/4AAQ"));
    free(du);

    if (g_fail) {
        fprintf(stderr, "streams test: FAILED\n");
        return 1;
    }
    printf("streams test: ok\n");
    return 0;
}
