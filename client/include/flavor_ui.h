#pragma once
// Krunker Strike menu branding (doc/v10/krunker-strike.md, the flavor-ui module). Pure DOM-ops
// over html_dom.h, driven by the process flavor (shared/include/flavor.h): a FLAVOR_BASE build
// never touches the page, so the base menu stays byte-for-byte unchanged. Only the player-facing
// brand changes (flavor_display_name(), the logo art); identifiers stay KrunkNative/krunknative.
//
// The brand itself is the real Krunker Strike FRVR one (the same trademark as this FRVR variant
// of Krunker): the logo ui/img/kn-strike-logo.png (krunkerstrike.frvr.com/img/krunker_logo.png,
// 1024x348, pinned in tools/ui-import/live_assets_v10.lock), the Strike palette and ChakraPetch
// 700 as CSS variables / @font-face in ui/css/krunknative.css under body.kn-flavor-strike, and the
// Strike page's loading-screen curtains (purple #5736b0 | blue #008dcb) while the menu loads.

// Brand the main menu for the current flavor: the body class kn-flavor-<key> and the
// #knFlavorLogo image (img/kn-<key>-logo.png) in #gameNameHolder in place of Krunker's #mainLogo.
// Call once, from the menu's setup_dom(), after the page markup exists. No-op for FLAVOR_BASE.
void flavor_ui_apply(void);

// The menu's loading state (connecting / loading, the spinner in #instructions): the body class
// kn-flavor-loading, which the flavor CSS turns into the Strike loading-screen curtains over
// #instructionHolder. Idempotent; no-op for FLAVOR_BASE.
void flavor_ui_set_loading(int loading);
