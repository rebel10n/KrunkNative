/* Unit test of the pure parts of the Live Streams list (streams_norm.c): no network, no config. */
#include <stdio.h>
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

    if (g_fail) {
        fprintf(stderr, "streams test: FAILED\n");
        return 1;
    }
    printf("streams test: ok\n");
    return 0;
}
