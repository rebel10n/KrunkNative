// The seek decisions without a window (seek_choice.h): the region a seek is made in, and the full
// lobby an answer names. servers_ui.c runs them; client/tests/seek_choice_test.c checks them.
#include <seek_choice.h>

#include <stdio.h>
#include <string.h>

static int listed(const cJSON *list, const char *region) {
    return cJSON_GetObjectItem(list, region) != NULL;
}

static void set_region(char *out, const size_t n, const char *region) {
    snprintf(out, n, "%s", region);
}

SeekRegionChoice seek_region_choose(const cJSON *list, const char *saved, const char *cached, char *out, const size_t n) {
    if (!out || !n) return SEEK_REGION_NONE;
    saved = saved ? saved : "";
    cached = cached ? cached : "";
    if (!list) { // no ping list (the matchmaker did not answer): the saved choice, else the cache
        if (saved[0]) {
            set_region(out, n, saved);
            return SEEK_REGION_SAVED;
        }
        set_region(out, n, cached[0] ? cached : "local");
        return cached[0] ? SEEK_REGION_CACHED : SEEK_REGION_NONE;
    }
    if (saved[0]) {
        set_region(out, n, saved);
        return listed(list, saved) ? SEEK_REGION_SAVED : SEEK_REGION_UNLISTED;
    }
    if (cached[0] && listed(list, cached)) {
        set_region(out, n, cached);
        return SEEK_REGION_CACHED;
    }
    if (list->child && list->child->string) {
        set_region(out, n, list->child->string);
        return SEEK_REGION_FIRST;
    }
    set_region(out, n, cached[0] ? cached : "local");
    return cached[0] ? SEEK_REGION_CACHED : SEEK_REGION_NONE;
}

int seek_answer_full(const cJSON *answer) {
    const cJSON *players = cJSON_GetObjectItem(answer, "players");
    const cJSON *max = cJSON_GetObjectItem(answer, "maxPlayers");
    if (!cJSON_IsNumber(players) || !cJSON_IsNumber(max) || max->valuedouble <= 0) return 0;
    return players->valuedouble >= max->valuedouble;
}
