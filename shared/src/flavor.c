// The process flavor and its table (shared/include/flavor.h). Pure C; no engine / net deps.
#include <flavor.h>
#include <build_info.h> // KN_PRODUCT_NAME: FLAVOR_BASE's display name stays the base product name
#include <strings.h>    // strcasecmp

// One entry per GameFlavor, indexed by the enum. FLAVOR_BASE is the neutral no-op (every knob at its
// identity value) so a base build is the base game byte-for-byte; Strike layers a bounded, hardcore
// set of gameplay deltas (flavor-gameplay). Fields: {flavor, key, display_name, damage_mult,
// regen_mult}. Keep the key lowercase and <= 7 chars: it rides the server-info `flavor` field (8 B).
static const FlavorConfig k_flavors[FLAVOR_COUNT] = {
    [FLAVOR_BASE]   = {FLAVOR_BASE,   "base",   KN_PRODUCT_NAME, 1.0f,  1.0f},
    [FLAVOR_STRIKE] = {FLAVOR_STRIKE, "strike", "Krunker Strike", 1.25f, 0.0f},
};

// Compile-time default: -DKN_FLAVOR_DEFAULT=1 (CMake KRUNKNATIVE_FLAVOR=strike) ships a binary whose
// default flavor is Strike; the runtime flag still overrides it. Unset = FLAVOR_BASE.
#ifndef KN_FLAVOR_DEFAULT
#define KN_FLAVOR_DEFAULT 0
#endif

static GameFlavor g_flavor =
    (KN_FLAVOR_DEFAULT >= 0 && KN_FLAVOR_DEFAULT < FLAVOR_COUNT) ? (GameFlavor) KN_FLAVOR_DEFAULT : FLAVOR_BASE;

const FlavorConfig *flavor_config(const GameFlavor flavor) {
    return &k_flavors[(flavor >= 0 && flavor < FLAVOR_COUNT) ? flavor : FLAVOR_BASE];
}

const FlavorConfig *flavor_current(void) { return flavor_config(g_flavor); }
GameFlavor flavor_get(void) { return g_flavor; }

int flavor_set(const GameFlavor flavor) {
    if (flavor < 0 || flavor >= FLAVOR_COUNT) return 0;
    g_flavor = flavor;
    return 1;
}

GameFlavor flavor_from_key(const char *key, const GameFlavor fallback) {
    if (key && key[0])
        for (int i = 0; i < FLAVOR_COUNT; i++)
            if (!strcasecmp(key, k_flavors[i].key)) return (GameFlavor) i;
    return fallback;
}

int flavor_set_key(const char *key) {
    if (!key || !key[0]) return 0;
    const GameFlavor f = flavor_from_key(key, FLAVOR_COUNT);
    if (f == FLAVOR_COUNT) return 0;
    g_flavor = f;
    return 1;
}

const char *flavor_key(const GameFlavor flavor) { return flavor_config(flavor)->key; }
const char *flavor_display_name(const GameFlavor flavor) { return flavor_config(flavor)->display_name; }
