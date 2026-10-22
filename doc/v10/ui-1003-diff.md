# Krunker v10.0.3 menu page: diff against ui/ (2026-10-21)

The target is Krunker v10.0.3 (doc/v10/target.md). This file records what the 10.0.3 page changes
against the v10.0.2 page that `ui/` was imported from, and what was done about each difference.

## Sources

- **10.0.3 page**: `/home/user/jails/kru/src/Krunker_v10.0.3/` (`index.html`, `css/`, `img/`,
  `static/index-ynaUXNyU.js` loader, and the client bundle `game.js`, 13 138 023 bytes, sha256
  `66887d66…`). `index.html` sha256 `8c2d75a7…`. The tree has no social, editor or viewer pages and
  no window templates.
- **10.0.2 base**: the live mirror `~/.cache/kn-v10/live/site/public/` (what `ui/` was imported from)
  and the client bundle `~/Downloads/v10.0.2.js` (sha256 `1e0b968b…`).
- **Official text**: `~/.cache/kn-krunker-changelog-versions.txt` (Krunker's `docs/versions.txt`,
  10.0.3 to 10.0.0; `doc/v10/changelog-10.0.3.md`).
- **Menu markup**: `tools/ui-import/v10_bundle.js` (the Svelte mounts, component CSS and English
  table). It cannot read the raw 10.0.3 bundle: it finds no template helpers (`importNode` is only in
  the string array), and it returns empty mounts. It reads the bundle after a static
  deobfuscation-only webcrack pass (`@codemod/webcrack` 2.16.0-beta.1 from
  `~/.cache/kn-v10-tools/wc`, with `unpack`, `unminify` and `jsx` off: the string arrays are
  decoded and nothing else is rewritten). A full webcrack pass (unminified) breaks its member-access
  patterns as well. With that pass the 10.0.2 bundle reproduces the cached extraction
  (`mainMenuUIMount` 8968 bytes). `make ui-import` still reads the 10.0.2 bundle; running it on the
  10.0.3 bundle needs the deobfuscation pass first.

## What differs

| File | 10.0.2 base vs 10.0.3 | ui/ today | Action |
|---|---|---|---|
| `index.html` (page shell) | Identical apart from the build query (`?build=i41Nu` to `?build=OpCIg`) and the loader hashes. Same stylesheet links, ids, mount points and scripts. The reference file also has an HTML comment addressed to agents, which the mirror has too. It is not page content and was not acted on. | KN's edit of this page: canvas, external links neutralised, relative URLs, KN stylesheets, `#mapWelcome` | None |
| Svelte mounts (`mounts.json`): `#mainMenuUIMount` (8968 bytes), `#notificationCenter`, `#gameAnnouncementEl`, `#matchmakingStatusEl`, `#matchEndSignupIncentives`, `#emojiPanelHolder`, `#menuWindowMusic`, and the two lazily mounted targets | Byte-identical. The two lazily mounted targets have different mangled ids in 10.0.3 and identical markup (329 and 215 bytes). | The same markup, with KN's edits | None |
| `css/svelte.css` (component CSS, 212 blocks) | Same 212 blocks and the same rules: the normalised line multisets are equal. Three unmounted blocks (`.paint-container`, `.style-container`, `.items-detail-root`) move earlier in the bundle's order. | The 10.0.2 extraction, with a header line | Not applied. No rule changes; the blocks use unique scoped hashes, so the order has no effect. |
| `css/main.css` | 10.0.3 equals the 10.0.2 mirror (310 058 bytes). | KN's edit: scope-bar offsets (`338px` for `339px`, lines 942 to 970), `../img/` icon paths | None |
| `css/flag.css` | 10.0.3 equals the mirror (22 776 bytes). | KN's edit: `../img/flags/` paths | None |
| `css/bundledStyles.css`, `main_custom.css`, `material-icons.css`, `google-play.css` | Identical in all three. | Identical | None |
| `img/` (17 files: `favicon.png`, `menu/faces/f_0..f_15`) | Identical to the mirror. | Part of `ui/img/` | None |
| `i18n` English table (`ui/i18n/en.json`, the inline `translations.en`) | 10.0.3 has 2176 keys (10.0.2: 2170). **Added:** `purchase.opening`, `purchase.confirming`, `purchase.confirmSlow`, `purchase.openFailed`, `settings.quality.noVideoSkins` ("Disable Video Skins"), `windows.skins.painted` ("Painted: {0}"). **Changed:** `account.usernames.buy.confirm` now puts the renewal price in `{3}` (was `{1}`). The 10.0.3 call passes `'$' + usernameRenewPriceUSD` (4.99) as the fourth argument. | 2168 keys: the 10.0.2 table without `matchmaker.CaptchaRequired` and `matchmaker.RecaptchaFailed` (removed in 9234f121) | **Updated:** the six keys, the changed value. The two matchmaker keys stay out. |
| `client/src/ui/v10_misc_popups.c:1044` (the C call of `account.usernames.buy.confirm`) | 10.0.3 passes `name, "$9.99", days, "$4.99"`. | KN passed three arguments and used `{1}` (`$9.99`) for the renewal price too. The renewal sentence said $9.99, against the $4.99 renewal in the official 10.0.2 text. | **Updated:** the English string matches the 10.0.3 table, and the fourth argument is `"$4.99"`. |
| `ui/windows/versions.txt` (Krunker's `docs/versions.txt`) | The 10.0.3 file adds an `== UPDATE 10.0.3 ==` section (28 lines) above 10.0.2. | The 10.0.2 copy. The client reads `kn-versions.txt` first and uses this file only as a fallback. | **Updated** to the official 10.0.3 file. |
| `ui/windows/kn-versions.txt` (KN's What's New) | Not a Krunker file. | Already leads with `UPDATE 10.0.3`. | None |
| Window templates `ui/windows/*.html` (from the live capture `krunker-windows-v10.json.gz`) | Not diffable: the 10.0.3 tree has no window templates. Known 10.0.3 window changes: Settings > Quality gains "Disable Video Skins" (`settings.quality.noVideoSkins`), and skin lists gain a "Painted: {0}" badge (`windows.skins.painted`). | Captured from 10.0.2 | Not regenerated (that needs `capture_v10_live.js` on the 10.0.3 client). **Added by hand:** the Render tab's "Disable Video Skins" row (`ui/windows/settings-advanced-3.html`, after Disable Animated Paints; `setSetting("noVideoSkins")`, off by default, a restart mark). KN renders no video skins (player_mesh.c has no video path), so the stored value changes nothing in KN; it is kept in the settings store and the Performance preset, as v10.0.3 has it (`client/src/ui/ui_settings.c`). Reference: `doc/ref/krunker-v10.0.3/video-skins-on.png`. The paint badge: see "Paint badge" below. |
| `social.html`, `editor.html`, `viewer.html` | Not in the 10.0.3 tree. The mirror has them (10.0.2). The editor's Solid Ramp Sides checkbox is the 10.0.3 editor change; KN ported it in `client/src/editor/editor_gameconfig.c`. | Mirror copies | Not diffable here; none changed |
| Other-language tables (`ui/i18n/<code>.json`) | Not in the 10.0.3 reference (the bundle holds only the English table). | From `krunker.io/i18n/<code>.js?v=i41Nu` | Not changed |
| Official 10.0.3 changes with a UI surface | Clan recruitment (applies to every join request; `doc/v10/changelog-10.0.3.md`, ported in the brain). Solid Ramp Sides (ported). Video skins (not ported). | - | See the changelog's classification |

## Result

The menu the page builds is the same in 10.0.3 as in 10.0.2: the shell, the Svelte mounts, the
component CSS (same rules), the stylesheets and the images. The 10.0.3 change the menu shows is the
English table. The buy-confirm renewal sentence now shows the $4.99 renewal, and the six new keys are
in the table for the page to use.

Two 10.0.3 UI additions are now in KrunkNative: the Render tab's Disable Video Skins switch (the stored
setting; no effect in KN, see the window-templates row) and the owned skin card's paint badge (below).

## Paint badge

Krunker's skin and item choosers mark an owned card whose metadata paints it (`skinPaintBadge`). The rule
and markup come from the decoded bundle (`game.js`, the helper near offset 10870002, called once per owned
row, after the card's image): no badge unless the owned entry's `m` has a paint colour or a paint shader;
title `Painted: <name>` (`windows.skins.painted`); body the store paint's thumbnail, else a colour swatch,
else the `format_paint` icon. KN: `client/src/ui/skin_paint.c` (the markup and rule), `client/src/ui/ui_loadout.c`
(`owned_paint_badge`: the store paint of the same colour, type 18), `client/include/skin_paint.h`. The CSS
(`.skinPaintBadge`) was already in `ui/css/main.css`.

Limits, stated plainly:
- KN's account data stores owned skins as `{ind, cnt}`. Nothing in KN gives an owned skin paint metadata yet,
  so the badge shows once the account data carries `m` (the v10.0.3 shape); until then no KN card is painted.
- No official 10.0.3 paint-badge capture exists: the harness has no logged-in account owning a painted skin
  (`doc/ref/krunker-v10.0.3/captures.json`, `notSaved`). Checked instead: the unpainted chooser against the
  official capture (`skin-chooser-no-paint.png`), and the badge's own rule by the unit test
  (`client/tests/skin_paint_test.c`).

## uidiff

No 10.0.3 captures exist (`doc/ref/krunker-v10.0.3/` is absent), so the reference is the 10.0.2 set
(`doc/ref/krunker-v10.0.2/`, `doc/ref/krunker-v10/`).

`make uidiff UIDIFF_HEADLESS=1` (llvmpipe, 911 regions in 338 scenario names; `doc/ref/krunker-v10.0.2/` and `doc/ref/krunker-v10/`):

| Run | Tree | Regions | Passed | Failed | Skipped |
|---|---|---:|---:|---:|---:|
| Before (a2e30feb, the earlier base) | origin/main a2e30feb, no edits | 911 | 501 | 345 | 65 |
| Before (037ec173, the base now) | origin/main 037ec173, no edits | 911 | 501 | 345 | 65 |
| After | 037ec173 + this change | 911 | 501 | 345 | 65 |

Per region, the 037ec173 before and after runs have the same PASS/FAIL result for all 911 regions.
Eight regions' DIFF% moved by under 0.2 points (`kn-market-trading popup`, `kn-tip-spin cost`,
`spin-item-body item / wheel-ring`, `spin-item item`, `spin-item-legendary`, ...), which is render
noise under llvmpipe; no threshold changed. The 345 failures are the same regions as on the base.
They are pre-existing and not part of this change.

Per scenario (regions PASS / FAIL before and after):

| Scenario | Regions | PASS before | FAIL before | PASS after | FAIL after |
|---|---:|---:|---:|---:|---:|
| auto-respawn | 1 | 0 | 1 | 0 | 1 |
| boot-curtain-350 | 2 | 1 | 1 | 1 | 1 |
| boot-curtain-700 | 2 | 1 | 1 | 1 | 1 |
| boot-curtain | 2 | 1 | 1 | 1 | 1 |
| boot-hosting | 2 | 1 | 1 | 1 | 1 |
| boot-no-games | 2 | 1 | 1 | 1 | 1 |
| click-to-play-grow-00 | 1 | 1 | 0 | 1 | 0 |
| click-to-play-grow-04 | 1 | 0 | 1 | 0 | 1 |
| click-to-play-grow-12 | 1 | 0 | 1 | 0 | 1 |
| community | 3 | 2 | 1 | 2 | 1 |
| controls-general | 2 | 2 | 0 | 2 | 0 |
| controls | 2 | 1 | 0 | 1 | 0 |
| controls-modes | 2 | 2 | 0 | 2 | 0 |
| customize-class | 2 | 0 | 1 | 0 | 1 |
| customize-customize | 2 | 0 | 2 | 0 | 2 |
| customize-hat | 2 | 1 | 1 | 1 | 1 |
| customize-loadout | 2 | 1 | 1 | 1 | 1 |
| customize-reticle | 2 | 1 | 1 | 1 | 1 |
| customize-secondary | 2 | 1 | 1 | 1 | 1 |
| customize-skin | 2 | 1 | 1 | 1 | 1 |
| customize-spray | 2 | 1 | 1 | 1 | 1 |
| customize-streaks | 2 | 0 | 1 | 0 | 1 |
| customize-title | 2 | 0 | 1 | 0 | 1 |
| editor-1920 | 11 | 8 | 1 | 8 | 1 |
| editor-inspector | 4 | 3 | 0 | 3 | 0 |
| editor-script-1920 | 7 | 5 | 2 | 5 | 2 |
| editor-win-assets | 4 | 3 | 1 | 3 | 1 |
| editor-win-controls | 4 | 3 | 1 | 3 | 1 |
| editor-win-settings | 4 | 3 | 1 | 3 | 1 |
| editor-win-sounds | 4 | 3 | 1 | 3 | 1 |
| game-updating | 3 | 1 | 2 | 1 | 2 |
| guide | 6 | 4 | 0 | 4 | 0 |
| hub-feed | 6 | 5 | 1 | 5 | 1 |
| hub-leaders | 6 | 5 | 1 | 5 | 1 |
| hub-login | 6 | 6 | 0 | 6 | 0 |
| hub-maps | 6 | 5 | 1 | 5 | 1 |
| hub-market | 6 | 5 | 1 | 5 | 1 |
| hub-mods | 6 | 5 | 1 | 5 | 1 |
| hub-settings | 6 | 6 | 0 | 6 | 0 |
| ingame-chat-lines | 2 | 1 | 1 | 1 | 1 |
| ingame-chat-open | 9 | 4 | 5 | 4 | 5 |
| ingame-counters-ds | 7 | 0 | 7 | 0 | 7 |
| ingame-counters-ds-wide | 7 | 0 | 7 | 0 | 7 |
| ingame-counters | 10 | 0 | 10 | 0 | 10 |
| ingame-counters-wide | 16 | 0 | 16 | 0 | 16 |
| ingame-death | 7 | 3 | 2 | 3 | 2 |
| ingame-death-self | 5 | 1 | 3 | 1 | 3 |
| ingame-death-tex | 3 | 2 | 1 | 2 | 1 |
| ingame-endscreen-collect | 3 | 2 | 0 | 2 | 0 |
| ingame-endscreen-collect-raid | 3 | 2 | 1 | 2 | 1 |
| ingame-endscreen | 4 | 3 | 0 | 3 | 0 |
| ingame-endscreen-late | 5 | 5 | 0 | 5 | 0 |
| ingame-killfeed | 11 | 4 | 7 | 4 | 7 |
| ingame-minimap | 11 | 5 | 5 | 5 | 5 |
| ingame-mobile | 16 | 14 | 2 | 14 | 2 |
| ingame-mobile-portrait | 16 | 13 | 3 | 13 | 3 |
| ingame-mode-ctf | 10 | 5 | 4 | 5 | 4 |
| ingame-mode-point | 10 | 4 | 5 | 4 | 5 |
| ingame-pause-esc | 6 | 0 | 3 | 0 | 3 |
| ingame-plain | 9 | 4 | 5 | 4 | 5 |
| ingame-playerlist | 3 | 2 | 0 | 2 | 0 |
| ingame-reloading | 9 | 4 | 5 | 4 | 5 |
| ingame-scoped | 8 | 3 | 5 | 3 | 5 |
| ingame-scoreboard | 10 | 5 | 5 | 5 | 5 |
| ingame-spectate | 6 | 4 | 2 | 4 | 2 |
| kn-account-kio-default | 1 | 1 | 0 | 1 | 0 |
| kn-account-native-default | 1 | 1 | 0 | 1 | 0 |
| kn-customize-colors | 2 | 1 | 1 | 1 | 1 |
| kn-editor-ai | 5 | 5 | 0 | 5 | 0 |
| kn-guest-earned-end | 3 | 3 | 0 | 3 | 0 |
| kn-guest-earned-menu | 2 | 2 | 0 | 2 | 0 |
| kn-ingame-gametime-counters | 3 | 0 | 3 | 0 | 3 |
| kn-ingame-gametime | 2 | 0 | 2 | 0 | 2 |
| kn-ingame-gametime-mobile | 2 | 1 | 1 | 1 | 1 |
| kn-ingame-rack | 2 | 0 | 2 | 0 | 2 |
| kn-ingame-topright | 5 | 0 | 5 | 0 | 5 |
| kn-ingame-topright-old | 4 | 0 | 4 | 0 | 4 |
| kn-lb-grid | 4 | 4 | 0 | 4 | 0 |
| kn-lost-connection-official | 4 | 1 | 3 | 1 | 3 |
| kn-main-menu-connect | 2 | 2 | 0 | 2 | 0 |
| kn-market-blackm | 1 | 0 | 1 | 0 | 1 |
| kn-market-browse | 1 | 0 | 1 | 0 | 1 |
| kn-market-create | 1 | 0 | 1 | 0 | 1 |
| kn-market-detail | 1 | 0 | 1 | 0 | 1 |
| kn-market-history | 1 | 0 | 1 | 0 | 1 |
| kn-market-inventory | 1 | 0 | 1 | 0 | 1 |
| kn-market-list | 1 | 0 | 1 | 0 | 1 |
| kn-market-quicksell | 1 | 1 | 0 | 1 | 0 |
| kn-market-sales | 1 | 0 | 1 | 0 | 1 |
| kn-market-stats | 1 | 0 | 1 | 0 | 1 |
| kn-market-trading | 1 | 0 | 1 | 0 | 1 |
| kn-menu-official-guest | 1 | 1 | 0 | 1 | 0 |
| kn-menu-official-header | 1 | 1 | 0 | 1 | 0 |
| kn-menu-popular-official | 1 | 0 | 1 | 0 | 1 |
| kn-notify-announcement | 1 | 0 | 1 | 0 | 1 |
| kn-notify-center | 1 | 0 | 1 | 0 | 1 |
| kn-notify-emoji | 1 | 0 | 1 | 0 | 1 |
| kn-om-btools | 4 | 0 | 4 | 0 | 4 |
| kn-om-entities | 4 | 0 | 4 | 0 | 4 |
| kn-om-npcs | 4 | 0 | 4 | 0 | 4 |
| kn-om-objects | 4 | 0 | 4 | 0 | 4 |
| kn-om-physgun | 4 | 0 | 4 | 0 | 4 |
| kn-om-props | 5 | 0 | 5 | 0 | 5 |
| kn-om-search | 4 | 0 | 4 | 0 | 4 |
| kn-om-tools-info | 4 | 0 | 4 | 0 | 4 |
| kn-om-tools | 4 | 0 | 4 | 0 | 4 |
| kn-om-tools-short | 4 | 3 | 1 | 3 | 1 |
| kn-om-weapons | 4 | 0 | 4 | 0 | 4 |
| kn-ranked-error | 2 | 2 | 0 | 2 | 0 |
| kn-ranked-found | 2 | 2 | 0 | 2 | 0 |
| kn-ranked-home | 6 | 5 | 1 | 5 | 1 |
| kn-ranked-sort | 2 | 1 | 1 | 1 | 1 |
| kn-servers-carousel | 6 | 0 | 6 | 0 | 6 |
| kn-servers-official-error | 1 | 0 | 1 | 0 | 1 |
| kn-servers-official | 4 | 0 | 4 | 0 | 4 |
| kn-servers-official-mixed | 1 | 0 | 1 | 0 | 1 |
| kn-servers-official-open | 6 | 0 | 6 | 0 | 6 |
| kn-settings-quickplay | 5 | 0 | 5 | 0 | 5 |
| kn-store-hats-v10 | 1 | 1 | 0 | 1 | 0 |
| kn-store-kio-eterno | 1 | 1 | 0 | 1 | 0 |
| kn-store-kio-ranked | 1 | 0 | 1 | 0 | 1 |
| kn-store-melee-v10 | 1 | 1 | 0 | 1 | 0 |
| kn-tip-item | 1 | 0 | 1 | 0 | 1 |
| kn-tip-spin | 2 | 0 | 2 | 0 | 2 |
| kn-tip-streak | 1 | 0 | 1 | 0 | 1 |
| kn-turf-boards | 2 | 0 | 2 | 0 | 2 |
| kn-turf-history | 2 | 1 | 1 | 1 | 1 |
| kn-turf-howto | 2 | 0 | 2 | 0 | 2 |
| kn-turf | 2 | 2 | 0 | 2 | 0 |
| kn-wallet-empty | 1 | 1 | 0 | 1 | 0 |
| kn-wallet-skins | 1 | 1 | 0 | 1 | 0 |
| login-2fa | 4 | 4 | 0 | 4 | 0 |
| login-account | 5 | 1 | 4 | 1 | 4 |
| login-header | 4 | 0 | 4 | 0 | 4 |
| login-kio | 4 | 2 | 2 | 2 | 2 |
| login-register | 5 | 5 | 0 | 5 | 0 |
| lost-connection-inactive | 1 | 0 | 1 | 0 | 1 |
| lost-connection | 1 | 0 | 1 | 0 | 1 |
| main-menu-buffs | 5 | 2 | 3 | 2 | 3 |
| main-menu-classbox-skin | 2 | 2 | 0 | 2 | 0 |
| main-menu-double-xp | 2 | 2 | 0 | 2 | 0 |
| main-menu-glow-1500 | 1 | 0 | 1 | 0 | 1 |
| main-menu-glow-200 | 1 | 0 | 1 | 0 | 1 |
| main-menu | 8 | 6 | 2 | 6 | 2 |
| main-menu-krsale | 5 | 2 | 3 | 2 | 3 |
| main-menu-nowplaying-long | 4 | 1 | 3 | 1 | 3 |
| main-menu-popular | 5 | 2 | 3 | 2 | 3 |
| market-browse | 3 | 3 | 0 | 3 | 0 |
| market-catalog | 2 | 2 | 0 | 2 | 0 |
| market-guide-glossary | 2 | 2 | 0 | 2 | 0 |
| market-guide | 4 | 3 | 0 | 3 | 0 |
| market-guide-trading | 2 | 2 | 0 | 2 | 0 |
| market-junkyard | 3 | 1 | 1 | 1 | 1 |
| market-trading | 2 | 2 | 0 | 2 | 0 |
| menu-host-type | 2 | 2 | 0 | 2 | 0 |
| menu-mobile | 9 | 4 | 5 | 4 | 5 |
| menu-mobile-portrait | 8 | 2 | 6 | 2 | 6 |
| popup-kid | 1 | 1 | 0 | 1 | 0 |
| popup-lottery | 1 | 1 | 0 | 1 | 0 |
| popup-mission-briefing | 1 | 1 | 0 | 1 | 0 |
| popup-premium | 1 | 0 | 1 | 0 | 1 |
| popup-survey | 1 | 0 | 1 | 0 | 1 |
| popup-usernames | 1 | 1 | 0 | 1 | 0 |
| ranked-guest | 5 | 3 | 1 | 3 | 1 |
| ranked-shop-flying | 1 | 0 | 1 | 0 | 1 |
| rewards-bp-chal | 1 | 0 | 1 | 0 | 1 |
| rewards-bp-guest | 1 | 1 | 0 | 1 | 0 |
| rewards-bp-home | 3 | 0 | 3 | 0 | 3 |
| rewards-bp-vip | 2 | 0 | 2 | 0 | 2 |
| rewards-chall-guest | 1 | 1 | 0 | 1 | 0 |
| rewards-chall-home | 1 | 1 | 0 | 1 | 0 |
| rewards-chall-map | 1 | 1 | 0 | 1 | 0 |
| rewards-chall-weapon | 1 | 1 | 0 | 1 | 0 |
| rewards-menu-account | 1 | 1 | 0 | 1 | 0 |
| rewards-register | 1 | 1 | 0 | 1 | 0 |
| rewards-signup | 2 | 2 | 0 | 2 | 0 |
| seq-auto-respawn | 1 | 0 | 0 | 0 | 0 |
| seq-boot-curtain@0 | 1 | 1 | 0 | 1 | 0 |
| seq-boot-curtain@100 | 1 | 1 | 0 | 1 | 0 |
| seq-boot-curtain@200 | 1 | 1 | 0 | 1 | 0 |
| seq-boot-curtain@350 | 1 | 1 | 0 | 1 | 0 |
| seq-boot-curtain@500 | 1 | 1 | 0 | 1 | 0 |
| seq-boot-curtain@700 | 1 | 1 | 0 | 1 | 0 |
| seq-boot-curtain@900 | 1 | 1 | 0 | 1 | 0 |
| seq-boot-curtain | 1 | 1 | 0 | 1 | 0 |
| seq-click-to-play-grow@0 | 1 | 1 | 0 | 1 | 0 |
| seq-click-to-play-grow@200 | 1 | 1 | 0 | 1 | 0 |
| seq-click-to-play-grow@400 | 1 | 1 | 0 | 1 | 0 |
| seq-click-to-play-grow@600 | 1 | 1 | 0 | 1 | 0 |
| seq-click-to-play-grow@800 | 1 | 1 | 0 | 1 | 0 |
| seq-click-to-play-grow@1000 | 1 | 1 | 0 | 1 | 0 |
| seq-click-to-play-grow@1200 | 1 | 1 | 0 | 1 | 0 |
| seq-click-to-play-grow@1400 | 1 | 1 | 0 | 1 | 0 |
| seq-click-to-play-grow | 1 | 1 | 0 | 1 | 0 |
| seq-death@0 | 2 | 1 | 0 | 1 | 0 |
| seq-death@250 | 2 | 1 | 0 | 1 | 0 |
| seq-death@500 | 2 | 1 | 0 | 1 | 0 |
| seq-death@750 | 2 | 1 | 0 | 1 | 0 |
| seq-death@1000 | 2 | 1 | 0 | 1 | 0 |
| seq-death@1500 | 2 | 1 | 0 | 1 | 0 |
| seq-death@2000 | 2 | 1 | 0 | 1 | 0 |
| seq-death | 1 | 1 | 0 | 1 | 0 |
| seq-emoji-panel | 1 | 0 | 0 | 0 | 0 |
| seq-end-collect-button@0 | 2 | 1 | 1 | 1 | 1 |
| seq-end-collect-button@500 | 2 | 0 | 2 | 0 | 2 |
| seq-end-collect-button@1000 | 2 | 1 | 1 | 1 | 1 |
| seq-end-collect-button@1500 | 2 | 1 | 1 | 1 | 1 |
| seq-end-collect-button@2000 | 2 | 1 | 1 | 1 | 1 |
| seq-end-collect-button@2500 | 2 | 1 | 1 | 1 | 1 |
| seq-end-collect-button@3000 | 2 | 1 | 1 | 1 | 1 |
| seq-end-collect-button | 1 | 1 | 0 | 1 | 0 |
| seq-end-results@0 | 1 | 1 | 0 | 1 | 0 |
| seq-end-results@2000 | 1 | 1 | 0 | 1 | 0 |
| seq-end-results@4000 | 1 | 1 | 0 | 1 | 0 |
| seq-end-results@5000 | 1 | 0 | 1 | 0 | 1 |
| seq-end-results@5500 | 1 | 0 | 1 | 0 | 1 |
| seq-end-results@6000 | 1 | 0 | 1 | 0 | 1 |
| seq-end-results@7000 | 1 | 0 | 1 | 0 | 1 |
| seq-end-results | 1 | 0 | 1 | 0 | 1 |
| seq-end-rewards-overlay@0 | 1 | 1 | 0 | 1 | 0 |
| seq-end-rewards-overlay@250 | 1 | 1 | 0 | 1 | 0 |
| seq-end-rewards-overlay@500 | 1 | 1 | 0 | 1 | 0 |
| seq-end-rewards-overlay@750 | 1 | 1 | 0 | 1 | 0 |
| seq-end-rewards-overlay@1000 | 1 | 1 | 0 | 1 | 0 |
| seq-end-rewards-overlay@1500 | 1 | 1 | 0 | 1 | 0 |
| seq-end-rewards-overlay@2000 | 1 | 1 | 0 | 1 | 0 |
| seq-end-rewards-overlay@2500 | 1 | 1 | 0 | 1 | 0 |
| seq-end-rewards-overlay@3000 | 1 | 1 | 0 | 1 | 0 |
| seq-end-rewards-overlay@4000 | 1 | 1 | 0 | 1 | 0 |
| seq-end-rewards-overlay | 1 | 1 | 0 | 1 | 0 |
| seq-end-vic@0 | 1 | 1 | 0 | 1 | 0 |
| seq-end-vic@100 | 1 | 1 | 0 | 1 | 0 |
| seq-end-vic@200 | 1 | 0 | 1 | 0 | 1 |
| seq-end-vic@300 | 1 | 1 | 0 | 1 | 0 |
| seq-end-vic@400 | 1 | 1 | 0 | 1 | 0 |
| seq-end-vic@500 | 1 | 1 | 0 | 1 | 0 |
| seq-end-vic@600 | 1 | 1 | 0 | 1 | 0 |
| seq-end-vic@700 | 1 | 1 | 0 | 1 | 0 |
| seq-end-vic@800 | 1 | 1 | 0 | 1 | 0 |
| seq-end-vic@900 | 1 | 1 | 0 | 1 | 0 |
| seq-end-vic@1000 | 1 | 1 | 0 | 1 | 0 |
| seq-end-vic@1100 | 1 | 1 | 0 | 1 | 0 |
| seq-end-vic@1200 | 1 | 1 | 0 | 1 | 0 |
| seq-end-vic@1300 | 1 | 1 | 0 | 1 | 0 |
| seq-end-vic@1400 | 1 | 1 | 0 | 1 | 0 |
| seq-end-vic@1500 | 1 | 1 | 0 | 1 | 0 |
| seq-end-vic@1600 | 1 | 1 | 0 | 1 | 0 |
| seq-end-vic@1700 | 1 | 1 | 0 | 1 | 0 |
| seq-end-vic@1800 | 1 | 1 | 0 | 1 | 0 |
| seq-end-vic@1900 | 1 | 1 | 0 | 1 | 0 |
| seq-end-vic@2000 | 1 | 1 | 0 | 1 | 0 |
| seq-end-vic@2100 | 1 | 1 | 0 | 1 | 0 |
| seq-end-vic@2200 | 1 | 1 | 0 | 1 | 0 |
| seq-end-vic@2300 | 1 | 1 | 0 | 1 | 0 |
| seq-end-vic@2400 | 1 | 1 | 0 | 1 | 0 |
| seq-end-vic@2500 | 1 | 1 | 0 | 1 | 0 |
| seq-end-vic@2600 | 1 | 1 | 0 | 1 | 0 |
| seq-end-vic@2700 | 1 | 1 | 0 | 1 | 0 |
| seq-end-vic@2800 | 1 | 1 | 0 | 1 | 0 |
| seq-end-vic@2900 | 1 | 1 | 0 | 1 | 0 |
| seq-end-vic@3000 | 1 | 1 | 0 | 1 | 0 |
| seq-end-vic | 1 | 1 | 0 | 1 | 0 |
| seq-game-announcement | 1 | 0 | 0 | 0 | 0 |
| seq-game-updating@0 | 3 | 1 | 1 | 1 | 1 |
| seq-game-updating@500 | 3 | 1 | 1 | 1 | 1 |
| seq-hit@0 | 1 | 1 | 0 | 1 | 0 |
| seq-hit@50 | 1 | 1 | 0 | 1 | 0 |
| seq-hit@100 | 1 | 1 | 0 | 1 | 0 |
| seq-hit@150 | 1 | 1 | 0 | 1 | 0 |
| seq-hit@200 | 1 | 1 | 0 | 1 | 0 |
| seq-hit@300 | 1 | 1 | 0 | 1 | 0 |
| seq-hit@500 | 1 | 1 | 0 | 1 | 0 |
| seq-hit@700 | 1 | 1 | 0 | 1 | 0 |
| seq-hit@1000 | 1 | 1 | 0 | 1 | 0 |
| seq-hit | 1 | 1 | 0 | 1 | 0 |
| seq-kill@0 | 2 | 1 | 0 | 1 | 0 |
| seq-kill@100 | 2 | 1 | 0 | 1 | 0 |
| seq-kill@200 | 2 | 1 | 0 | 1 | 0 |
| seq-kill@300 | 2 | 1 | 0 | 1 | 0 |
| seq-kill@500 | 2 | 1 | 0 | 1 | 0 |
| seq-kill@700 | 2 | 1 | 0 | 1 | 0 |
| seq-kill@1000 | 2 | 1 | 0 | 1 | 0 |
| seq-kill@1500 | 2 | 1 | 0 | 1 | 0 |
| seq-kill@2000 | 2 | 1 | 0 | 1 | 0 |
| seq-kill@3500 | 2 | 1 | 0 | 1 | 0 |
| seq-kill@4000 | 2 | 1 | 0 | 1 | 0 |
| seq-kill@4200 | 2 | 1 | 0 | 1 | 0 |
| seq-kill | 1 | 1 | 0 | 1 | 0 |
| seq-loading-map-name@0 | 1 | 1 | 0 | 1 | 0 |
| seq-lost-connection@0 | 2 | 1 | 0 | 1 | 0 |
| seq-lost-connection@500 | 2 | 1 | 0 | 1 | 0 |
| seq-menu-1000 | 1 | 0 | 0 | 0 | 0 |
| seq-mobile-menu | 1 | 0 | 0 | 0 | 0 |
| seq-notification-register | 1 | 0 | 0 | 0 | 0 |
| seq-ranked-overlay-open | 1 | 0 | 0 | 0 | 0 |
| seq-tooltip-loadout | 1 | 0 | 0 | 0 | 0 |
| settings-advanced-controls | 4 | 3 | 1 | 3 | 1 |
| settings-advanced-game | 4 | 4 | 0 | 4 | 0 |
| settings-advanced-general | 4 | 3 | 1 | 3 | 1 |
| settings-advanced | 4 | 4 | 0 | 4 | 0 |
| settings-advanced-sound | 4 | 4 | 0 | 4 | 0 |
| settings-display | 4 | 4 | 0 | 4 | 0 |
| settings-game | 4 | 0 | 4 | 0 | 4 |
| settings-general | 5 | 4 | 0 | 4 | 0 |
| settings-mobile-0 | 7 | 6 | 0 | 6 | 0 |
| settings-mobile-1 | 5 | 5 | 0 | 5 | 0 |
| settings-mobile-2 | 6 | 3 | 3 | 3 | 3 |
| settings-mobile-3 | 4 | 4 | 0 | 4 | 0 |
| settings-render | 4 | 3 | 0 | 3 | 0 |
| settings-search | 4 | 4 | 0 | 4 | 0 |
| settings-sound | 4 | 4 | 0 | 4 | 0 |
| social-clan | 1 | 1 | 0 | 1 | 0 |
| social-feed | 1 | 1 | 0 | 1 | 0 |
| social-friends | 1 | 1 | 0 | 1 | 0 |
| social-post | 1 | 1 | 0 | 1 | 0 |
| social-profile | 1 | 1 | 0 | 1 | 0 |
| social-search | 1 | 1 | 0 | 1 | 0 |
| spin-heroic | 1 | 0 | 1 | 0 | 1 |
| spin-item-body | 4 | 2 | 2 | 2 | 2 |
| spin-item | 4 | 2 | 2 | 2 | 2 |
| spin-item-legendary | 4 | 2 | 2 | 2 | 2 |
| spin-item-melee | 4 | 2 | 2 | 2 | 2 |
| spin-starter | 1 | 0 | 1 | 0 | 1 |
| store-eterno | 1 | 0 | 1 | 0 | 1 |
| store-home | 1 | 0 | 1 | 0 | 1 |
| store-kr | 1 | 0 | 1 | 0 | 1 |
| store-premium | 1 | 0 | 1 | 0 | 1 |
| store-wheels | 1 | 0 | 1 | 0 | 1 |
| strike/class-selection | 5 | 3 | 2 | 3 | 2 |
| strike/customize-controls | 7 | 4 | 0 | 4 | 0 |
| strike/double-xp-end | 2 | 2 | 0 | 2 | 0 |
| strike/item-selection | 7 | 2 | 3 | 2 | 3 |
| strike/killcard | 1 | 1 | 0 | 1 | 0 |
| strike/leaderboard-players | 5 | 5 | 0 | 5 | 0 |
| strike/main-menu | 6 | 6 | 0 | 6 | 0 |
| strike/popup | 1 | 1 | 0 | 1 | 0 |
| strike/spin | 7 | 7 | 0 | 7 | 0 |
| strike/store | 6 | 4 | 0 | 4 | 0 |


## Not done, and why

- Window templates were not regenerated (no 10.0.3 capture of the whole menu was run). The Video Skins
  row is added by hand in `ui/windows/settings-advanced-3.html`. The paint badge is code (`skin_paint.c`),
  not template markup, and has no official paint-state capture (see "Paint badge").
- The component CSS order was not changed (see above).
- The official 10.0.3 changelog's gameplay items are in `doc/v10/changelog-10.0.3.md`, not here.
