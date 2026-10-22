// client/src/ui/seek_choice.c without a window: the region a seek is made in (the saved Default Region
// is what the player chose: a listed one is sought, an unlisted one is never swapped for the first
// listed region), and a seek answer whose lobby is full (never joined).
//   cmake-build-debug/seek_choice_test
#include <seek_choice.h>

#include <stdio.h>
#include <string.h>

static int g_fail, g_pass;
#define CHECK(cond, ...) do { if (cond) g_pass++; else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// The ping list of a brain whose first node is Middle East (the first key of the object).
static const char *k_list = "{\"me-bhn\":\"me.example:6767\",\"us-nj\":\"ny.example\",\"VA\":\"va.example\"}";
static const char *k_list_no_va = "{\"me-bhn\":\"me.example:6767\",\"us-nj\":\"ny.example\"}";

static void choose(const char *list_json, const char *saved, const char *cached, SeekRegionChoice *how, char *out, size_t n) {
    cJSON *list = list_json ? cJSON_Parse(list_json) : NULL;
    *how = seek_region_choose(list, saved, cached, out, n);
    cJSON_Delete(list);
}

static void test_regions(void) {
    char out[32];
    SeekRegionChoice how;

    // the Ashburn key is listed: the seek is made in it, not in the first region of the list
    choose(k_list, "VA", "", &how, out, sizeof(out));
    CHECK(!strcmp(out, "VA") && how == SEEK_REGION_SAVED, "saved VA listed: got %s (%d)", out, how);

    // Ashburn (VA) is not listed (its node is down): no error, an available region is sought instead:
    // the first listed one (me-bhn here), never the saved one
    choose(k_list_no_va, "VA", "", &how, out, sizeof(out));
    CHECK(!strcmp(out, "me-bhn") && how == SEEK_REGION_UNLISTED, "saved VA unlisted: got %s (%d), want me-bhn unlisted", out, how);

    // ... the cache does not replace the first listed region either (no error, no cache for a default)
    choose(k_list_no_va, "VA", "us-nj", &how, out, sizeof(out));
    CHECK(!strcmp(out, "me-bhn") && how == SEEK_REGION_UNLISTED, "saved VA unlisted, cache us-nj: got %s (%d)", out, how);

    // no saved choice: the cached region when the list has it
    choose(k_list, "", "us-nj", &how, out, sizeof(out));
    CHECK(!strcmp(out, "us-nj") && how == SEEK_REGION_CACHED, "no saved, cache us-nj listed: got %s (%d)", out, how);

    // no saved choice and a cache the list does not name: the list's first region (as before)
    choose(k_list, "", "gone", &how, out, sizeof(out));
    CHECK(!strcmp(out, "me-bhn") && how == SEEK_REGION_FIRST, "no saved, cache gone: got %s (%d)", out, how);

    // no saved choice, no cache: the first region
    choose(k_list, "", "", &how, out, sizeof(out));
    CHECK(!strcmp(out, "me-bhn") && how == SEEK_REGION_FIRST, "nothing saved or cached: got %s (%d)", out, how);

    // no ping list (the matchmaker did not answer): the saved choice, else the cache, else local
    choose(NULL, "VA", "us-nj", &how, out, sizeof(out));
    CHECK(!strcmp(out, "VA") && how == SEEK_REGION_SAVED, "no list, saved VA: got %s (%d)", out, how);
    choose(NULL, "", "us-nj", &how, out, sizeof(out));
    CHECK(!strcmp(out, "us-nj") && how == SEEK_REGION_CACHED, "no list, cache only: got %s (%d)", out, how);
    choose(NULL, "", "", &how, out, sizeof(out));
    CHECK(!strcmp(out, "local") && how == SEEK_REGION_NONE, "no list, nothing: got %s (%d)", out, how);

    // an empty list (no node registered): the saved choice is unlisted and nothing is picked for it
    // (the seek falls back to the cache or "local")
    choose("{}", "VA", "", &how, out, sizeof(out));
    CHECK(!strcmp(out, "local") && how == SEEK_REGION_UNLISTED, "empty list, saved VA: got %s (%d)", out, how);
    choose("{}", "", "", &how, out, sizeof(out));
    CHECK(!strcmp(out, "local") && how == SEEK_REGION_NONE, "empty list, nothing: got %s (%d)", out, how);
}

static int full(const char *json) {
    cJSON *answer = cJSON_Parse(json);
    const int f = seek_answer_full(answer);
    cJSON_Delete(answer);
    return f;
}

static void test_full(void) {
    // a lobby with every seat taken is full: never joined
    CHECK(full("{\"gameId\":\"CHITOWN2:kz862\",\"players\":8,\"maxPlayers\":8}"), "8/8 must be full");
    CHECK(full("{\"players\":9,\"maxPlayers\":8}"), "9/8 (a stale count) must be full");
    // a lobby with a free seat is joined
    CHECK(!full("{\"players\":7,\"maxPlayers\":8}"), "7/8 must not be full");
    CHECK(!full("{\"players\":0,\"maxPlayers\":8}"), "0/8 must not be full");
    // an answer without the counts (an older brain) is not refused
    CHECK(!full("{\"gameId\":\"CHITOWN2:kz862\",\"host\":\"h\",\"port\":22001}"), "no counts must not be full");
    CHECK(!full("{\"players\":8,\"maxPlayers\":0}"), "maxPlayers 0 must not be full");
    CHECK(!full("{\"players\":\"8\",\"maxPlayers\":8}"), "a string count must not be full");
    CHECK(!full("null"), "no answer must not be full");
}

// The region Settings > Default Region shows while none is saved: the region the seek is made in. Every ping
// failing seeks in the list's first region (v10: "All pings failed" then the first), which the row must show too,
// never a fixed "local" (a live run: the seek joined Silicon Valley while the row read Local).
static void test_shown(void) {
    CHECK(!strcmp(seek_region_shown("VA", "us-nj", "us-ca-sv"), "VA"), "saved VA wins over the cache and the seek");
    CHECK(!strcmp(seek_region_shown("", "us-nj", "us-ca-sv"), "us-nj"), "no saved: the confirmed race region is shown");
    CHECK(!strcmp(seek_region_shown("", "", "us-ca-sv"), "us-ca-sv"), "no saved, every ping failed: the region sought (us-ca-sv) is shown");
    CHECK(!strcmp(seek_region_shown("", "", ""), "local"), "nothing known: local");
    CHECK(!strcmp(seek_region_shown(NULL, NULL, NULL), "local"), "NULL inputs: local");
}

int main(void) {
    test_regions();
    test_full();
    test_shown();
    printf("seek_choice_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
