#pragma once
// Krunker Strike is a product FLAVOR of Krunker Box Office (doc/v10/krunker-strike.md): the same
// C11 engine, the same shared core and the same Krunker v10.0.2 ruleset, with a distinct brand and
// a small, bounded set of gameplay deltas layered on top via FlavorConfig. Flavor is ORTHOGONAL to
// Ruleset (shared/include/ruleset.h): one flavor per process, chosen once at startup (server:
// --flavor / server.cfg `flavor`; client: --flavor / KN_FLAVOR / the compile-time default), then
// advertised by the server (server-info `flavor`) and adopted / shown by the client.
//
// Only the player-facing DISPLAY name changes. Identifiers (the repo, KRUNKNATIVE_* variables,
// kn_* targets, file and package names, user agents and protocol strings) keep the KrunkNative
// name, exactly like KN_PRODUCT_NAME (shared/include/build_info.h).
//
// Pure C, no GL / GLFW / net dependency: this header and shared/src/flavor.c live in the shared
// core that server_bin, client_bin and the tests all link.
#include <stddef.h>

typedef enum {
    FLAVOR_BASE = 0,   // "Krunker Box Office": the base game, unchanged (the default)
    FLAVOR_STRIKE = 1, // "Krunker Strike": the competitive flavor
    FLAVOR_COUNT
} GameFlavor;

// The gameplay deltas a flavor layers over the ruleset. v1 ships the seam with a single knob; the
// flavor-gameplay module (see the brief) owns setting Strike's values and the code that reads them.
// FLAVOR_BASE leaves every field at its neutral (no-op) value, so a default build is the base game
// byte-for-byte.
typedef struct {
    GameFlavor flavor;
    const char *key;          // wire / config token, lowercase: "base", "strike"
    const char *display_name; // player-facing brand: "Krunker Box Office", "Krunker Strike"
    float damage_mult;        // global outgoing-damage multiplier (dealer-dealt combat damage only);
                              // 1.0 = neutral (base). Strike: 1.25 (a harder-hitting, hardcore feel).
    float regen_mult;         // passive health-regen multiplier, composed with the mode regenMlt;
                              // 1.0 = neutral (base). Strike: 0.0 (no passive regen).
} FlavorConfig;

// ---- the process flavor (set once at startup; never NULL) --------------------------------------

// The config of the current process flavor. Before flavor_set* is called this is FLAVOR_BASE.
const FlavorConfig *flavor_current(void);
GameFlavor flavor_get(void);

// Set the process flavor. Returns 1 on success, 0 for an unknown value (the current one is kept).
// One flavor per process: call once, early in main(), before anything reads flavor_current().
int flavor_set(GameFlavor flavor);
int flavor_set_key(const char *key); // "base" / "strike", case-insensitive; NULL / "" keeps base

// ---- lookups that do not touch the process flavor ----------------------------------------------

const FlavorConfig *flavor_config(GameFlavor flavor);               // bounded; FLAVOR_BASE if out of range
GameFlavor flavor_from_key(const char *key, GameFlavor fallback);   // parse a key, else fallback
const char *flavor_key(GameFlavor flavor);                          // "base" / "strike"
const char *flavor_display_name(GameFlavor flavor);                 // the brand string
