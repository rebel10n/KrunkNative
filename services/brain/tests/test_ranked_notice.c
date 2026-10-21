/* Unit test of the queued-for-ranked notice's social frame (ranked_notice.c): no database, no network.
 * Hide player names: the frame must not carry the queued player's name inside a composed line, so a client
 * that hides names can format the notice itself from the template. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ranked_notice.h"

static int g_fail;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)

static int count_substr(const char *hay, const char *needle) {
    int n = 0;
    const size_t len = strlen(needle);
    for (const char *p = hay; (p = strstr(p, needle)) != NULL; p += len) n++;
    return n;
}

int main(void) {
    cJSON *o = ranked_notice_social_event(42, "Quentin");
    CHECK(o != NULL);
    if (!o) return 1;
    char *json = cJSON_PrintUnformatted(o);
    CHECK(json != NULL);
    CHECK(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, "type")), "ranked:queued"));
    CHECK(cJSON_GetObjectItemCaseSensitive(o, "account") && cJSON_GetObjectItemCaseSensitive(o, "account")->valuedouble == 42);
    CHECK(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, "name")), "Quentin"));
    CHECK(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, "template")), "$ queued for ranked"));
    CHECK(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, "color")), "#5ce05a"));
    /* no composed line: nothing but the name field carries the name, and no "text" with "<name> queued for ranked" */
    CHECK(cJSON_GetObjectItemCaseSensitive(o, "text") == NULL);
    CHECK(json && count_substr(json, "Quentin") == 1);
    CHECK(json && !strstr(json, "Quentin queued"));
    free(json);
    cJSON_Delete(o);

    /* no name: the template reads "A player", and nothing is named */
    o = ranked_notice_social_event(7, "");
    CHECK(o != NULL);
    if (o) {
        CHECK(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, "template")), "A player queued for ranked"));
        CHECK(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, "name")), ""));
        CHECK(cJSON_GetObjectItemCaseSensitive(o, "text") == NULL);
        cJSON_Delete(o);
    }

    if (g_fail) {
        printf("test_ranked_notice: FAILED\n");
        return 1;
    }
    printf("test_ranked_notice: OK\n");
    return 0;
}
