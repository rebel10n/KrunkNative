// flavor_storage_test: each flavor's native storage directory (shared/include/flavor.h
// flavor_storage_subdir; client/src/html/html_dom_core.c and client/src/alt/alt_store.c join it onto
// $XDG_CONFIG_HOME or ~/.config). Box Office keeps "krunknative", so its knSettings, knClient, sign-in
// and alt vault stay where they were; Krunker Strike has its own "krunknative-strike", so the two
// flavors never read or change each other's settings. Pure C: no GL, GLFW, net or storage I/O.
// Run: ./<build>/flavor_storage_test
#include <flavor.h>

#include <stdio.h>
#include <string.h>

static int g_failures, g_checks;

#define CHECK(cond, ...) do { g_checks++; \
    if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_failures++; } \
} while (0)

int main(void) {
    char base[64] = "", strike[64] = "", unknown[64] = "", tiny[4] = "";

    // Box Office's directory is the one it always had: an existing install keeps its settings
    CHECK(flavor_storage_subdir(FLAVOR_BASE, base, sizeof(base)), "Box Office's directory name fits");
    CHECK(!strcmp(base, "krunknative"), "Box Office's directory is unchanged: \"krunknative\", got \"%s\"", base);

    // Krunker Strike gets its own directory, named from its flavor key
    CHECK(flavor_storage_subdir(FLAVOR_STRIKE, strike, sizeof(strike)), "Strike's directory name fits");
    CHECK(!strcmp(strike, "krunknative-strike"), "Strike's directory is \"krunknative-strike\", got \"%s\"", strike);
    CHECK(strcmp(base, strike) != 0, "Box Office and Strike resolve to different storage directories");
    CHECK(!strncmp(strike, "krunknative-", 12) && !strcmp(strike + 12, flavor_key(FLAVOR_STRIKE)),
          "Strike's directory suffix is its flavor key (\"%s\")", flavor_key(FLAVOR_STRIKE));

    // an out-of-range flavor resolves to Box Office (flavor_config), never to a bogus directory
    CHECK(flavor_storage_subdir((GameFlavor) 99, unknown, sizeof(unknown)) && !strcmp(unknown, "krunknative"),
          "an unknown flavor resolves to Box Office's directory, got \"%s\"", unknown);

    // a buffer too small for the name fails rather than truncating into another directory
    CHECK(!flavor_storage_subdir(FLAVOR_STRIKE, tiny, sizeof(tiny)), "a short buffer fails for Strike");
    CHECK(!flavor_storage_subdir(FLAVOR_BASE, NULL, sizeof(base)), "a NULL buffer fails");
    CHECK(!flavor_storage_subdir(FLAVOR_BASE, base, 0), "a zero-size buffer fails");

    printf("flavor_storage_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
