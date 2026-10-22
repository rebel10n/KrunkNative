// shared/src/krunker_regions.c against Krunker v10.0.3 (deob-10.0.3.js 19306-19344, 21566-21596): the
// dropdown's ids in order, the dedicated-only ids, the names, which ids getRegion pings, the nearest-region
// rule (lowest ping first, then great circle) and getRegion's ping URL rule.
//   cmake-build-debug/krunker_regions_test
#include <krunker_build.h>
#include <krunker_regions.h>

#include <stdio.h>
#include <string.h>

static int g_fail, g_pass;
#define CHECK(cond, ...) do { if (cond) g_pass++; else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void test_dropdown(void) {
    // availableRegions (19306), the Default Region dropdown's options in v10's order: the same list
    // krunker_build.h's KRUNKER_REGIONS holds (krunker_proto_test pins it against the live build)
    static const char *const want[] = {KRUNKER_REGIONS};
    const char *ids[32];
    const size_t n = krunker_region_dropdown(ids, 32);
    CHECK(n == KRUNKER_REGION_COUNT && n == sizeof(want) / sizeof(want[0]), "dropdown has %zu ids, want %d", n, KRUNKER_REGION_COUNT);
    for (size_t i = 0; i < n && i < sizeof(want) / sizeof(want[0]); i++) {
        CHECK(!strcmp(ids[i], want[i]), "dropdown[%zu] = %s, want %s", i, ids[i], want[i]);
        CHECK(krunker_region(ids[i]) && (krunker_region(ids[i])->flags & KR_AVAILABLE), "%s is available", ids[i]);
    }
    CHECK(!strcmp(ids[0], "de-fra") && !strcmp(ids[1], "us-ca-sv") && !strcmp(ids[5], "us-nj") && !strcmp(ids[9], "me-bhn"),
          "the order is v10's (de-fra, us-ca-sv, ..., us-nj, ..., me-bhn)");
    // the whole table is the dropdown first, in that order
    for (size_t i = 0; i < n; i++) CHECK(!strcmp(krunker_region_at(i)->id, ids[i]), "table[%zu] is %s", i, ids[i]);
    CHECK(krunker_region_dropdown(NULL, 0) == KRUNKER_REGION_COUNT, "a NULL out still counts");
}

static void test_dedicated_only(void) {
    // dedicatedOnlyRegions (19313): in the table, flagged, not in the dropdown, never pinged
    static const char *const want[] = {KRUNKER_DEDICATED_ONLY_REGIONS};
    size_t flagged = 0;
    for (size_t i = 0; i < krunker_region_count(); i++) flagged += (krunker_region_at(i)->flags & KR_DEDICATED_ONLY) != 0;
    CHECK(flagged == sizeof(want) / sizeof(want[0]), "%zu dedicated-only ids, want %zu", flagged, sizeof(want) / sizeof(want[0]));
    for (size_t i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        const KrunkerRegion *r = krunker_region(want[i]);
        CHECK(r && (r->flags & KR_DEDICATED_ONLY) && !(r->flags & KR_AVAILABLE), "%s is dedicated-only", want[i]);
        CHECK(!krunker_region_pinged(want[i]), "%s is never pinged", want[i]);
    }
    // the table keeps v10's order for them too
    CHECK(!strcmp(krunker_region_at(KRUNKER_REGION_COUNT + 1)->id, "us-wa") && !strcmp(krunker_region_at(KRUNKER_REGION_COUNT + 5)->id, "as-seoul"),
          "dedicated-only ids follow sss in v10's order");
}

static void test_names(void) {
    // the name table (21566-21596), 29 entries
    static const struct {
        const char *id, *name;
    } want[] = {
        {"local", "Local"}, {"us-nj", "New York"}, {"us-il", "Chicago"}, {"us-tx", "Dallas"}, {"us-wa", "Seattle"},
        {"us-ca-la", "Los Angeles"}, {"us-ga", "Atlanta"}, {"nl-ams", "Amsterdam"}, {"gb-lon", "London"},
        {"de-fra", "Frankfurt"}, {"us-ca-sv", "Silicon Valley"}, {"au-syd", "Sydney"}, {"fr-par", "Paris"},
        {"jb-hnd", "Tokyo"}, {"us-fl", "Miami"}, {"sgp", "Singapore"}, {"blr", "India"}, {"as-mb", "Mumbai"},
        {"brz", "Brazil"}, {"me-bhn", "Middle East"}, {"af-ct", "South Africa"}, {"as-seoul", "South Korea"},
        {"mx-cmx", "Mexico"}, {"eu-stm", "Sweden"}, {"tor", "Toronto"}, {"pol", "Poland"}, {"us-hi", "Hawaii"},
        {"sss", "EU Super Secret Servers"}, {"iow", "Iowa"},
    };
    CHECK(krunker_region_count() == sizeof(want) / sizeof(want[0]), "table has %zu regions, want %zu", krunker_region_count(), sizeof(want) / sizeof(want[0]));
    for (size_t i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        const char *name = krunker_region_v10_name(want[i].id);
        CHECK(name && !strcmp(name, want[i].name), "%s is %s, got %s", want[i].id, want[i].name, name ? name : "(none)");
    }
    CHECK(krunker_region_v10_name("zz-unknown") == NULL && krunker_region(NULL) == NULL && krunker_region("") == NULL, "unknown ids have no entry");
    // continents (19315-19344): 1 oc, 2 as, 3 eu, 4 na, 5 sa
    CHECK(!strcmp(krunker_region("au-syd")->continent, "oc") && !strcmp(krunker_region("sgp")->continent, "as") &&
              !strcmp(krunker_region("de-fra")->continent, "eu") && !strcmp(krunker_region("us-nj")->continent, "na") &&
              !strcmp(krunker_region("brz")->continent, "sa") && !strcmp(krunker_region("mx-cmx")->continent, "sa") &&
              !strcmp(krunker_region("af-ct")->continent, "eu"),
          "continents are v10's");
}

static void test_pinged(void) {
    // getRegion pings a listed key when availableRegions has it and dedicatedOnlyRegions does not
    static const char *const yes[] = {KRUNKER_REGIONS, "sss", "local"};
    static const char *const no[] = {KRUNKER_DEDICATED_ONLY_REGIONS, "us-ca-la", "us-fl", "tor", "iow", "XX", ""};
    for (size_t i = 0; i < sizeof(yes) / sizeof(yes[0]); i++) CHECK(krunker_region_pinged(yes[i]), "%s is pinged", yes[i]);
    for (size_t i = 0; i < sizeof(no) / sizeof(no[0]); i++) CHECK(!krunker_region_pinged(no[i]), "%s is not pinged", no[i]);
    CHECK(!krunker_region_pinged(NULL), "NULL is not pinged");
    CHECK(krunker_region("local")->flags & KR_LOCAL, "local is the dev host's");
    CHECK(krunker_region("sss")->flags & KR_BETA_ONLY, "sss is beta-only");
}

static void test_distance(void) {
    const double fra_ny = krunker_region_distance_km("de-fra", "us-nj"), fra_sv = krunker_region_distance_km("de-fra", "us-ca-sv");
    CHECK(fra_ny > 6000 && fra_ny < 6500, "Frankfurt - New York is ~6,200 km, got %.0f", fra_ny);
    CHECK(fra_sv > 9000 && fra_sv < 9400, "Frankfurt - Silicon Valley is ~9,150 km, got %.0f", fra_sv);
    CHECK(krunker_region_distance_km("us-nj", "us-nj") == 0, "a region is 0 km from itself");
    CHECK(krunker_region_distance_km("us-il", "us-nj") > 1100 && krunker_region_distance_km("us-il", "us-nj") < 1200, "Chicago - New York ~1,150 km");
    CHECK(krunker_region_distance_km("local", "us-nj") < 0 && krunker_region_distance_km("XX", "us-nj") < 0 && krunker_region_distance_km("us-nj", NULL) < 0,
          "no place: -1");
}

static void test_nearest(void) {
    // KrunkNative's hosted regions (doc/v10/regions.md): Silicon Valley and New York
    static const char *const hosted[] = {"us-ca-sv", "us-nj"};
    // a hosted id is itself
    CHECK(krunker_region_nearest("us-nj", hosted, 2, NULL) == 1 && krunker_region_nearest("us-ca-sv", hosted, 2, NULL) == 0, "a hosted id is itself");
    // great circle: Europe, Middle East, Africa, Brazil -> New York; Asia-Pacific, Hawaii, Seattle -> Silicon Valley
    static const struct {
        const char *id;
        int want;
    } gc[] = {
        {"de-fra", 1}, {"gb-lon", 1}, {"me-bhn", 1}, {"af-ct", 1}, {"brz", 1}, {"us-il", 1}, {"us-fl", 1}, {"tor", 1},
        {"au-syd", 0}, {"jb-hnd", 0}, {"sgp", 0}, {"us-hi", 0}, {"us-wa", 0}, {"us-ca-la", 0}, {"mx-cmx", 0},
        {"us-tx", 0}, /* Dallas: ~2,300 km to Santa Clara, ~2,200 km to New York... */
    };
    for (size_t i = 0; i < sizeof(gc) / sizeof(gc[0]) - 1; i++) {
        const int got = krunker_region_nearest(gc[i].id, hosted, 2, NULL);
        CHECK(got == gc[i].want, "%s -> %s by great circle, got %d", gc[i].id, hosted[gc[i].want], got);
    }
    // Dallas is between: whichever is nearer by the table, it is one of the two
    CHECK(krunker_region_nearest("us-tx", hosted, 2, NULL) >= 0, "Dallas resolves");
    // Mumbai: ~12,500 km to Santa Clara and ~12,500 km to New York by great circle: either, but never -1
    CHECK(krunker_region_nearest("as-mb", hosted, 2, NULL) >= 0, "Mumbai resolves");
    // the player's measured pings win over geography (Krunker's nearest is the lowest latency): a European
    // player whose Silicon Valley ping is lower goes there
    const double west[] = {140, 95}, east[] = {40, 180}, only_nj[] = {-1, 110}, none[] = {-1, -1};
    CHECK(krunker_region_nearest("de-fra", hosted, 2, west) == 1, "de-fra with pings 140/95 -> us-nj");
    CHECK(krunker_region_nearest("de-fra", hosted, 2, east) == 0, "de-fra with pings 40/180 -> us-ca-sv (lowest ping)");
    CHECK(krunker_region_nearest("de-fra", hosted, 2, only_nj) == 1, "an unmeasured region is skipped");
    CHECK(krunker_region_nearest("de-fra", hosted, 2, none) == 1, "every ping failed: great circle");
    CHECK(krunker_region_nearest("us-nj", hosted, 2, east) == 1, "a hosted id stays itself whatever the pings");
    // no place to measure from: pings, else the first hosted region
    CHECK(krunker_region_nearest("local", hosted, 2, east) == 0 && krunker_region_nearest("local", hosted, 2, NULL) == 0, "local: pings, else the first");
    CHECK(krunker_region_nearest("XX", hosted, 2, west) == 1 && krunker_region_nearest("XX", hosted, 2, NULL) == 0, "unknown id: pings, else the first");
    // a hosted region without a place comes after every placed one
    static const char *const with_local[] = {"local", "us-nj"};
    CHECK(krunker_region_nearest("de-fra", with_local, 2, NULL) == 1, "local loses to a placed region");
    static const char *const only_local[] = {"local"};
    CHECK(krunker_region_nearest("de-fra", only_local, 1, NULL) == 0, "the only node is taken, wherever it is");
    CHECK(krunker_region_nearest("de-fra", hosted, 0, NULL) == -1 && krunker_region_nearest("de-fra", NULL, 2, NULL) == -1, "nothing hosted: -1");
}

static void url(const char *value, const char *want) {
    char out[256];
    krunker_region_ping_url(value, out, sizeof(out));
    CHECK(!strcmp(out, want), "ping url of %s: got %s, want %s", value, out, want);
}

static void test_ping_url(void) {
    // v10 getRegion (147489-147491): ip_* / localhost / 127.0.0.1 keep their port, http unless :443; else https
    // without the port. KrunkNative: a brain's :6767 stays plain HTTP
    url("ip_1_2_3_4:8080", "http://ip_1_2_3_4:8080/ping");
    url("localhost:5050", "http://localhost:5050/ping");
    url("127.0.0.1:443", "https://127.0.0.1:443/ping");
    url("127.0.0.1:6767", "http://127.0.0.1:6767/ping");
    url("brain.example:6767", "http://brain.example:6767/ping");
    url("ny.krunker.io:443", "https://ny.krunker.io/ping");
    url("ny.krunker.io", "https://ny.krunker.io/ping");
    url("", "https:///ping");
}

int main(void) {
    test_dropdown();
    test_dedicated_only();
    test_names();
    test_pinged();
    test_distance();
    test_nearest();
    test_ping_url();
    printf("krunker_regions_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
