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

## In-game windows, line by line (2026-11-23)

The windows outside the hub, each compared element by element and handler by handler with `deob-10.0.7.js`
(the earlier rounds had checked them against 10.0.2 / 10.0.3 or by screenshot only). Every gap below is fixed
unless its row says otherwise; the differences kept on purpose are in `doc/v10/kn-additions.md` ("In-game
windows"). Tests: the rows' e2e names, all registered in CTest.

| Area | Gaps found | Main fixes | Left open | Tests |
| --- | --- | --- | --- | --- |
| Store / shop | 14, plus the collection fold | `bundlePopup` in v10's order (NAME, INFO, KR / Junk / Premium days, 12 slots with collections folded into one "+N" slot, music, `lastBundlePopupShown`); the "gbdr" automatic popup with its 8-hour cap and the conditional bundles; the home card's priority order; Premium Benefits modal; getReadableTime; vouchPop on krunker.io; Eterno Retry; View Tickets; the home dots in place; `#purchaseResponse` click-to-hide (:215745-215950, :128613, :224172) | none | `store_bundle_e2e`, `kio_checkout_test` |
| Battle Pass | 11 | the skip cap, `claimItemRes` with funds and junk, no creator code, the 3.5 s double-charge guard, v10's gift form, purchase errors, the wheel scroll, no end-screen panel in custom games, a level purchase's badge (:219183-219588) | `bpStatusText` kept (KN's tick hook) | `bp_scenario_e2e`, `brain_season`, `brain_rewards` |
| Challenges / daily rewards | 13 | displayProgress / displayGoal, the 8-field `chlR` with junk / titles / RP, `chgC` / `uchp`, RP and title tiles, the capped bar, the locked Daily Spin click, daily BPP (migration 0397) (:180768-180806, :215154-215197, :225195-225229) | Box Office's mid-match `chgC` (game-server side) | `chal_card_e2e`, `brain_progress`, `brain_daily_spin` |
| Events / spins / raffle | 20 | the brain's `/data/spins` and `swu`, a ticket's own wheel, pools as slices, promo / infoText, ticket spins, the "+ More" modal, the lottery's data, charges, pushes and texts, eventPopup's `challengeSkins` and colour, the spin history filter, years in getReadableTime, the map spinners on the same data (:224283-224790, :127588-128541, :228451-228488) | spin `instances` and "Purchase More" (no item instances / no catalogue), the lottery bulbs' blink | `prize_wheel_scenario_e2e`, `lottery_scenario_e2e`, `brain_lottery`, `brain_live_events` |
| Ranked | 17 | start / cancel sounds, `$rankedAllRegions` before any queue, FlyingCurrency to Shop, Escape on the overlay, a closed popup's pending error, `has-scroll-bar`, the merged overlay state, the banner's Found / Connecting text, ticking shop countdowns, per-row Buying, the RP tween, refetch on tab mount; the chat line's [Join Queue] action (NET_EXT_KN_CHAT_ACTIONS) (:113305-114975, :228042-228061) | none | `server/tests/ranked_menu_e2e.py`, `ranked_krunker_e2e.py`, `chat_actions_e2e` |
| Turf Wars / clan war | 12 | clan logos everywhere v10 draws one, tab switches resetting state, Deputy take-downs, the panel's locked territory, v10's error texts, no extra sounds, Daily View's flow, the take-down callback (:143421-146591). v10.0.7 has no clan-war popups | none | `brain_turf` (+ sqlite / mysql), uidiff `kn-turf*` |
| Leaderboards / profile | 17 | v10's fetch errors and resets (also the ranked Rating / Matches tabs), `player_eventcount` / `player_skinvalue` boards, KPD fields, staff Chat Ban / Flag / Verify, Reports rows, Sales chart / errors / listing cards, the hidden classes, deep links (:112109-113283, :140052-140850, :230147-230168) | KrunkCup's data (no tournament model) | `brain_social_v10`, `brain_v10_social_e2e` |

Nothing in these windows needed the menu agent's files except one: `v10_menu.c`'s shop-button note
(`store_item_update`) still reads the compiled-in wheels, not the brain's `/data/spins`.
