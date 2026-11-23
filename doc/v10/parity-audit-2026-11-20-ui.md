# Client UI parity audit against Krunker v10.0.7 (2026-11-20)

Scope: the menu page (`ui/`), every window (`windows[]`), the settings, keybinds, HUD / player list, the
prize wheel and the account window, compared with `Krunker_v10.0.7/deob/game.deob.js` (cited
`deob-10.0.7.js:<line>`) and the live page (`Krunker_v10.0.7/live/`). The 10.0.7 changelog items themselves
are in `target.md` "10.0.7"; this audit looks for anything else the client draws or calls but does not do.

## Method and results

| Check | How | Result |
| --- | --- | --- |
| i18n keys | live `i18n/en.js` (2394 keys) vs `ui/i18n/en.json` (2341) | 53 missing: 51 `oauth.*` strings of the OAuth consent page (not used by game.js: Box Office's consent page is the brain's own, doc/v10/oauth-provider.md) and the two matchmaker captcha keys removed on purpose. Nothing to port. |
| Static element ids | live `index.html` (404 ids) vs `ui/index.html` (474) | none missing |
| Static inline callees | the 18 functions the live page's own markup calls | all handled in C |
| Settings | `SETTINGS.list` (:210172-214408): 292 entries, name / val / min / max / step / hide | every key is in the settings tabs or `ui_settings_defaults.c` (`scoreBoardTrigger` / `voiceChatTrigger` are hidden and defaulted there); 10.0.6 -> 10.0.7 changes no default (only renamed variables) |
| Keybinds | the 47 `binds` (:208949-) storage keys and defaults | all present in `ui_controls.c` with the same defaults |
| Windows | `windows[]` (:217977-222594), `showWindow(1..54)` | every used window has a gen or template; showWindow 11, 12, 22, 28, 29 are empty "unused" stubs in 10.0.7 too |
| Page-callable globals | 337 functions game.js puts on `window`, minus the ones KN's C names, minus internals (timers, storage, ads, FRVR, OneTrust, Steam, Meta) | 20 called from generated markup; triaged below |

## Findings

| # | Item | deob-10.0.7.js | KN location | Severity | Status |
| --- | --- | --- | --- | --- | --- |
| 1 | Settings > Account "Hide Detailed Online Status" switch drawn but `toggleStatus` unhandled; brain had no `hds` route | :217688-217693, HIDE_STATUS `hdsr` :98648, answer :225259-225261 | `account_ui.c`, brain `progress.c` / `db_progress.c` | medium | fixed (e8fc4abb, 8584382b) |
| 2 | Charm already on the other slot: 10.0.7 shows a Charm Error popup with Override (clears both slots, reselects); KN showed a plain alert | :217418-217446 | `ui_loadout.c` select_charm | low | fixed (8584382b) |
| 3 | Punish popup lacked moderator Flag / Unflag and Chat Ban / Unban | :224082, :224064-224125 | `hud_ui.c` punishPopup | medium (staff) | fixed (8584382b): brain staff flag `["flg", name, on]`, `POST /moderation/chat` |
| 4 | Prize wheel "+ More" was inert; 10.0.7 opens every item the wheel can roll | :224381, :224391-224445, modal :127588-127700 | `store_ui.c` show_more_items, `kn-alt.css` | medium | fixed (e6da86b9) |
| 5 | Mod post-processing shaders: a mod's shaders add "Shader Mod #N" toggles and params to Settings (`setShaderInput`) | :205829-205935, :216668, :218092-218120 | done (240da032, 5e582764): mod zips' shaders/ graphs run as post passes (doc/v10/mod-shaders.md), with the Shader Inputs rows on the advanced Render tab | medium | done |
| 6 | `copyGameLink` (host message "Click to Copy", team links) | :204616-204621 | n/a | none | dead in 10.0.7: its only caller passes the try-again text without a link (:204804) |
| 7 | `selectPack` / `selectPackPop`, `downloadPack` | :217738-217746, :223432 | n/a | none | official desktop client only (`window.ClientSession`) |
| 8 | `debugBots`, `botsPopup` | :224130-, :221123 | n/a | none | developer tools (`isDev`) |
| 9 | `premiumConfirmed`, `clearName` | :223529, :219105 | `kio_checkout.c` confPremium / premiumConfirmed; `account_ui.c` clearName; brain `nme` with no name, `clear_alias` | low | done (2026-11-23): the Confirmation popup and Premium flow as v10 up to checkout; the payment step is Box Office's no-payments stub (doc/v10/checkout-*.md). clearName removes the display name (`name_changes` records it) |
| 10 | `pfpPopup` / `uploadProfilePic` | :218978, :223294 | `account_ui.c` `knPfpPick` | none | Box Office's own picker covers it |
| 11 | `getDCExport` / `disconnectPopup` (copy disconnect diagnostics for support) | :229972-229975 | `disconnect_report.c` | low | done (2026-11-23): v10's popup and its JSON (same keys, order and 2-space format) with copy-to-clipboard. v10 names no support link, so none is added |
| 12 | `grabMap` / `openEdit` | :228260-228276 | `editor_net.c` | none | handled in the editor page |
| 13 | Menu chat: a guest saw "Enter Message" on the menu; 10.0.7's `toggleMenuHider(on)` runs `hideChatIfGuestUser`, so the menu shows `#chatUIHolder` to an account only (logout hides it, login shows it; the end screen and the game keep it) | :215587-215589, :163415, :216833, :216910 | `hud_v10.c` hud_v10_tick | medium | fixed (2026-11-23); e2e: hud_chat, v10_hud, v10_menu, chat_ingame_gate, v10_endscreen |
| 14 | Menu ads: KN hid `#aHider` and `#topRightAdHolder`; 10.0.7 shows them with `canShowAds` (`!(isMsPwa \|\| isSteamClient \|\| isEpicClient \|\| isOAuthPage)`, the live index.html) unless premium, and its swap slot flips between the bundle card and `#adCon` every 20 s | :226526-226527, :215644-215684, :216881, :229393 | `menu_ui.c` menu_ads_apply, `bundle_offers.c`, `comp_ui.c`, `v10_menu.c` | low | fixed (2026-11-23): the empty `#aContainer` strip as in the captures (uidiff main-menu ad-strip); Box Office fills no display ad; Manage Ads (:218181) stays stripped |
| 15 | Skin Manager: the sandbox menu's item did nothing | :204459-204480, :124230-124437, :229392 | `v10_skin_manager.c`, brain `skins.c` (migration 0394) | medium | fixed (2026-11-23): the window, its table / editor (General, Appearance, Advanced) and `/skins` CRUD; then the 3D preview (viewer iframe), model / texture / sound uploads (`asset_upload.c`, migration 0395), Capture Thumbnail, Dye Colors and Shader Studio's sections, Test's skin swap in the sandbox; open: the wear / paint / Kounter / attachment / charm editors, the node canvas and drawing a node graph |
| 16 | Host chooser (uidiff menu-host-type) failed structurally | ui/windows/42.html | `rent_servers_ui.c` | low | fixed (2026-11-23): the Rent a Server card's 15 px margin moved the centred window 31.3 px at the #uiBase scale; 14 px keeps a whole-pixel offset |

## What remains

- **Mod shaders (5):** done: `client/src/mod_shader_graph.c` (graph -> GLSL), `mod_shader.c` (the passes on the
  scene target), the Shader Inputs rows in `ui_settings.c` / `ui_windows.c`; doc/v10/mod-shaders.md.
- **Disconnect report (11):** done: `client/src/ui/disconnect_report.c` builds v10's blob (ua, savedRegion,
  connectedRegion, connectedHost, ping, v, steam, client, mobile, browser, code, reason, time) and copies it.
  Nothing in 10.0.7 opens the popup, so no caller is added. premium_dc_e2e covers it.
- **Ban appeal (2026-11-23):** the Lost Connection screen's "Contact … to Appeal the Ban" line (g:229053) keeps
  krunker.io's address only for an official krunker.io game's "Connection Banned". A Box Office ban (its servers
  reject with "Banned" / "Banned: <reason>") gets the same line naming Krunker Box Office, which opens the menu's
  Contact (`windows[20].tabIndex=1,showWindow(21)`, as #termsInfo does): the repository has no other support
  destination (no support mail in config.example.json or the docs). That window is still v10's own text (FRVR's
  support desk); Box Office has not written its own. connect_ux_e2e (banned).
- **Not checked here:** the svelte components' inner layouts window by window (the uidiff scenarios cover
  them where captures exist), and the HUD's in-game behaviour (owned by the gameplay audit).
