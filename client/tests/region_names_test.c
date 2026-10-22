// client/src/ui/region_names.c without a window: the v10 id a KrunkNative edge code is known by (the seek and the
// saved Default Region use it), and the picker labels (the v10 name, the edge places in brackets).
//   cmake-build-debug/region_names_test
#include <region_names.h>

#include <krunker_build.h> // KRUNKER_REGIONS: v10's dropdown, the order the picker must keep

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
    CHECK(region_hosted("us-ca-sv") && region_hosted("us-nj") && region_hosted("local"), "the edges' ids are hosted");
    CHECK(!region_hosted("de-fra") && !region_hosted("SV") && !region_hosted("") && !region_hosted(NULL), "other ids are not");
}

// The Default Region picker's values: the whole Krunker v10 dropdown (availableRegions, KRUNKER_REGIONS in its
// order), then local, then anything else the brain lists; the same list whatever the Server Type (the brain's
// list on Box Office, no list on krunker.io or offline). Before: only the ids the brain listed (Silicon Valley,
// New York, Local).
static void picker(const char *const *listed, const size_t listed_n, const char *what) {
    static const char *const want[] = {KRUNKER_REGIONS};
    const char *ids[64];
    const size_t n = region_picker_ids(listed, listed_n, ids, 64);
    CHECK(n >= KRUNKER_REGION_COUNT + 1, "%s: %zu ids, want the %d v10 regions and local at least", what, n, KRUNKER_REGION_COUNT);
    for (size_t i = 0; i < sizeof(want) / sizeof(want[0]) && i < n; i++) {
        CHECK(!strcmp(ids[i], want[i]), "%s: ids[%zu] = %s, want %s (v10's order)", what, i, ids[i], want[i]);
    }
    CHECK(n > KRUNKER_REGION_COUNT && !strcmp(ids[KRUNKER_REGION_COUNT], "local"), "%s: local follows the v10 list", what);
    for (size_t i = 0; i < listed_n; i++) {
        int found = 0;
        for (size_t k = 0; k < n; k++) found |= !strcmp(ids[k], listed[i]);
        CHECK(found, "%s: the listed %s is offered", what, listed[i]);
    }
    for (size_t i = 0; i < n; i++) {
        for (size_t k = i + 1; k < n; k++) CHECK(strcmp(ids[i], ids[k]), "%s: %s is offered once", what, ids[i]);
    }
    // every v10 dropdown id has a label a v10 player knows
    for (size_t i = 0; i < n; i++) CHECK(region_v10_name(ids[i]) != NULL || i >= KRUNKER_REGION_COUNT + 1, "%s: %s has a v10 name", what, ids[i]);
}

static void test_picker(void) {
    static const char *const brain_list[] = {"us-ca-sv", "us-nj", "local"}; // the production brain's /mm/ping-list
    picker(brain_list, 3, "Box Office");
    picker(NULL, 0, "krunker.io (no brain list)");
    static const char *const odd_list[] = {"us-nj", "iow"}; // a node in a region v10's dropdown lacks: offered after
    picker(odd_list, 2, "a brain listing Iowa");
    const char *ids[64];
    const size_t n = region_picker_ids(odd_list, 2, ids, 64);
    CHECK(n == KRUNKER_REGION_COUNT + 2 && !strcmp(ids[n - 1], "iow"), "Iowa is offered last: got %zu ids, last %s", n, n ? ids[n - 1] : "");
    // a small out is never overrun
    const char *two[2];
    CHECK(region_picker_ids(brain_list, 3, two, 2) == 2 && !strcmp(two[0], "de-fra") && !strcmp(two[1], "us-ca-sv"), "max caps the ids");
}

static void label(const char *id, const char *want) {
    char out[160];
    region_picker_label(id, out, sizeof(out));
    CHECK(!strcmp(out, want), "label of %s: got \"%s\", want \"%s\"", id, out, want);
}

static void test_labels(void) {
    // the v10 name alone, never the edge places in brackets (the user: the labels got too long)
    label("us-ca-sv", "Silicon Valley");
    label("us-nj", "New York");
    char out[160];
    region_picker_label("us-ca-sv", out, sizeof(out));
    CHECK(!strchr(out, '('), "no parenthesis in a region label");
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
    test_picker();
    test_labels();
    printf("region_names_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
