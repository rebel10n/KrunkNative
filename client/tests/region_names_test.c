// client/src/ui/region_names.c without a window: the v10 id a KrunkNative edge code is known by (the seek and the
// saved Default Region use it), and the picker labels (the v10 name, the edge places in brackets).
//   cmake-build-debug/region_names_test
#include <region_names.h>

#include <stdio.h>
#include <string.h>

static int g_fail, g_pass;
#define CHECK(cond, ...) do { if (cond) g_pass++; else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void test_ids(void) {
    CHECK(!strcmp(region_id_of("SV"), "us-ca-sv"), "SV -> us-ca-sv: got %s", region_id_of("SV"));
    CHECK(!strcmp(region_id_of("LA"), "us-ca-sv"), "LA -> us-ca-sv: got %s", region_id_of("LA"));
    CHECK(!strcmp(region_id_of("CH"), "us-nj"), "CH -> us-nj: got %s", region_id_of("CH"));
    CHECK(!strcmp(region_id_of("VA"), "us-nj"), "VA -> us-nj: got %s", region_id_of("VA"));
    CHECK(!strcmp(region_id_of("local"), "local"), "local -> local: got %s", region_id_of("local"));
    // a v10 id is itself; an unknown code is itself (the brain does not list it, so it never comes back)
    CHECK(!strcmp(region_id_of("us-nj"), "us-nj"), "us-nj stays: got %s", region_id_of("us-nj"));
    CHECK(!strcmp(region_id_of("de-fra"), "de-fra"), "de-fra stays: got %s", region_id_of("de-fra"));
    CHECK(!strcmp(region_id_of("XX"), "XX"), "XX stays: got %s", region_id_of("XX"));
    CHECK(!strcmp(region_id_of(NULL), ""), "NULL is empty");
    // the KrunkNative names stay with their codes
    CHECK(region_edge("SV") && !strcmp(region_edge("SV")->name, "Santa Clara"), "SV is Santa Clara");
    CHECK(region_edge("VA") && !strcmp(region_edge("VA")->name, "Ashburn, Virginia"), "VA is Ashburn, Virginia");
    CHECK(region_edge("us-nj") == NULL, "a v10 id is no edge code");
}

static void test_names(void) {
    CHECK(!strcmp(region_v10_name("us-nj"), "New York"), "us-nj is New York");
    CHECK(!strcmp(region_v10_name("us-ca-sv"), "Silicon Valley"), "us-ca-sv is Silicon Valley");
    CHECK(!strcmp(region_v10_name("de-fra"), "Frankfurt"), "de-fra is Frankfurt");
    CHECK(!strcmp(region_v10_name("us-tx"), "Dallas"), "us-tx is Dallas");
    CHECK(region_v10_name("zz-unknown") == NULL, "an unknown id has no v10 name");
}

static void label(const char *id, const char *want) {
    char out[160];
    region_picker_label(id, out, sizeof(out));
    CHECK(!strcmp(out, want), "label of %s: got \"%s\", want \"%s\"", id, out, want);
}

static void test_labels(void) {
    // the v10 name first (what a v10 player knows), the KrunkNative places it holds in brackets
    label("us-ca-sv", "Silicon Valley (Santa Clara, Los Angeles)");
    label("us-nj", "New York (Chicago, Ashburn, Virginia)");
    label("local", "Local");
    label("us-tx", "Dallas");
    label("de-fra", "Frankfurt");
    label("zz-unknown", "zz-unknown");
    // a small buffer is never overrun
    char tiny[8];
    region_picker_label("us-ca-sv", tiny, sizeof(tiny));
    CHECK(strlen(tiny) < sizeof(tiny), "a tiny buffer is NUL-terminated");
}

int main(void) {
    test_ids();
    test_names();
    test_labels();
    printf("region_names_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
