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
| 8 | `debugBots`, `botsPopup` | :224128-224157, :221123 | `hud_ui.c` botsPopup / debugBots, `chat_command_bot_debug` (shared/src/chat_commands.c), shared/include/bot_debug.h | low (DEV) | done (2026-11-26): the DEV row's smart_toy button opens v10's Bot Debugging popup (`confPop`). A left click toggles a row or runs an action, a right click (`debugBots(code, false)`) turns a toggle off. KN servers take `NET_EXT_BOT_CONFIG` / `NET_EXT_BOT_DEBUG` (0x080B, 0x080C) from a DEV only; krunker.io gets `bcfg` and `bots` |
| 9 | `premiumConfirmed`, `clearName` | :223529, :219105 | `kio_checkout.c` confPremium / premiumConfirmed; `account_ui.c` clearName; brain `nme` with no name, `clear_alias` | low | done (2026-11-23): the Confirmation popup and Premium flow as v10 up to checkout; the payment step is Box Office's no-payments stub (doc/v10/checkout-*.md). clearName removes the display name (`name_changes` records it) |
| 10 | `pfpPopup` / `uploadProfilePic` | :218978, :223294 | `account_ui.c` `knPfpPick` | none | Box Office's own picker covers it |
| 11 | `getDCExport` / `disconnectPopup` (copy disconnect diagnostics for support) | :229972-229975 | `disconnect_report.c` | low | done (2026-11-23): v10's popup and its JSON (same keys, order and 2-space format) with copy-to-clipboard. v10 names no support link, so none is added |
| 12 | `grabMap` / `openEdit` | :228260-228276 | `editor_net.c` | none | handled in the editor page |
| 13 | Menu chat: a guest saw "Enter Message" on the menu; 10.0.7's `toggleMenuHider(on)` runs `hideChatIfGuestUser`, so the menu shows `#chatUIHolder` to an account only (logout hides it, login shows it; the end screen and the game keep it) | :215587-215589, :163415, :216833, :216910 | `hud_v10.c` hud_v10_tick | medium | fixed (2026-11-23); e2e: hud_chat, v10_hud, v10_menu, chat_ingame_gate, v10_endscreen. A login shows the chat in the same frame, as v10's login handler does (:216833; 1f2dbb7e, `v10_menu_e2e` "2: signed in") |
| 14 | Menu ads: KN hid `#aHider` and `#topRightAdHolder`; 10.0.7 shows them with `canShowAds` (`!(isMsPwa \|\| isSteamClient \|\| isEpicClient \|\| isOAuthPage)`, the live index.html) unless premium, and its swap slot flips between the bundle card and `#adCon` every 20 s | :226526-226527, :215644-215684, :216881, :229393 | `menu_ui.c` menu_ads_apply, `bundle_offers.c`, `comp_ui.c`, `v10_menu.c` | low | fixed (2026-11-23): the empty `#aContainer` strip as in the captures (uidiff main-menu ad-strip); Box Office fills no display ad; Manage Ads (:218181) stays stripped |
| 15 | Skin Manager: the sandbox menu's item did nothing | :204459-204480, :124230-124437, :229392 | `v10_skin_manager.c`, brain `skins.c` (migration 0394) | medium | fixed (2026-11-23): the window, its table / editor (General, Appearance, Advanced) and `/skins` CRUD; then the 3D preview (viewer iframe), model / texture / sound uploads (`asset_upload.c`, migration 0395), Capture Thumbnail, Dye Colors and Shader Studio's sections, Test's skin swap in the sandbox; then the node canvas and the Paintable, Paint, Wear, Kounter (itemConfig.attach) and Charm editors (4a80e155), the Kounter and charm gizmo and snap tools (5f316eab), a paint's shader graph live on the preview (c9765cff), the skin's own wear map on the preview (1f538bfd) and Capture Thumbnail on the web (8b4375f0); `v10_skin_manager_e2e` |
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
| Battle Pass | 11 | the skip cap, `claimItemRes` with funds and junk, no creator code, the 3.5 s double-charge guard, v10's gift form, purchase errors, the wheel scroll, no end-screen panel in custom games, a level purchase's badge (:219183-219588) | none: v10 draws the same status div without a class (:219368); KN's `bpStatusText` class is a hook for `battlepass_ui_tick` (doc/v10/kn-additions.md) | `bp_scenario_e2e`, `brain_season`, `brain_rewards` |
| Challenges / daily rewards | 13 | displayProgress / displayGoal, the 8-field `chlR` with junk / titles / RP, `chgC` / `uchp`, RP and title tiles, the capped bar, the locked Daily Spin click, daily BPP (migration 0397) (:180768-180806, :215154-215197, :225195-225229) | none: the game server's mid-match `chgC` is in (45864eab, `challenge_mid_match_e2e`; round 12 below) | `chal_card_e2e`, `brain_progress`, `brain_daily_spin` |
| Events / spins / raffle | 20 | the brain's `/data/spins` and `swu`, a ticket's own wheel, pools as slices, promo / infoText, ticket spins, the "+ More" modal, the lottery's data, charges, pushes and texts, eventPopup's `challengeSkins` and colour, the spin history filter, years in getReadableTime, the map spinners on the same data (:224283-224790, :127588-128541, :228451-228488) | none: round 11 did all three, once spins mint copies (migration 0398, `doc/v10/paint-copy.md`): spin `instances` (the sixth value / `data.instances`, :224680-224697) and Spin Error's "Purchase More" through `store.catalog`'s Paint Ticket entry 90001 (:224246-224256) in 45c385e3; the lottery bulbs' blink (:127770-127793) in f44eaa60 | `prize_wheel_scenario_e2e`, `lottery_scenario_e2e`, `brain_lottery`, `brain_live_events`, `brain_shop` |
| Ranked | 17 | start / cancel sounds, `$rankedAllRegions` before any queue, FlyingCurrency to Shop, Escape on the overlay, a closed popup's pending error, `has-scroll-bar`, the merged overlay state, the banner's Found / Connecting text, ticking shop countdowns, per-row Buying, the RP tween, refetch on tab mount; the chat line's [Join Queue] action (NET_EXT_KN_CHAT_ACTIONS) (:113305-114975, :228042-228061) | none | `server/tests/ranked_menu_e2e.py`, `ranked_krunker_e2e.py`, `chat_actions_e2e` |
| Turf Wars / clan war | 12 | clan logos everywhere v10 draws one, tab switches resetting state, Deputy take-downs, the panel's locked territory, v10's error texts, no extra sounds, Daily View's flow, the take-down callback (:143421-146591). v10.0.7 has no clan-war popups | none | `brain_turf` (+ sqlite / mysql), uidiff `kn-turf*` |
| Leaderboards / profile | 17 | v10's fetch errors and resets (also the ranked Rating / Matches tabs), `player_eventcount` / `player_skinvalue` boards, KPD fields, staff Chat Ban / Flag / Verify, Reports rows, Sales chart / errors / listing cards, the hidden classes, deep links (:112109-113283, :140052-140850, :230147-230168) | none: KrunkCup's teams, contributors and top players come from `krunkcup_contributions` (round 11, migration 0399) | `brain_social_v10`, `brain_v10_social_e2e` |

Nothing in these windows needed the menu agent's files except one: `v10_menu.c`'s shop-button note
(`store_item_update`) read the compiled-in wheels; it now reads `store_ui_spins()` like the store (round 6 below).

## Menu, settings, servers, HUD and loadout, line by line (2026-11-23, round 6)

The same element-by-element treatment for the windows the earlier rounds had called done: the main menu (buttons,
panels, header, right side, mobile, signed-in / guest states), Settings (every tab: label, default, range, effect,
reset / import / export), the server browser and Host Game, the in-game HUD, the pause / death / end screens, the
in-game chat, and class select / the loadout. Four read-only audits found 87 gaps; every one was checked against
`deob-10.0.7.js` again before porting. Each commit body cites its lines.

| Area | Gaps found | Main fixes | Left open | Tests |
| --- | --- | --- | --- | --- |
| Main menu | 6 | the header's equipped title (`ph-title`, :148780-148838) and "Logging in..." (:148211, :148818); a guest is `Guest_<sid>` in the class box and the loadout toolbar (:217898, :217967); the team selector with `selTeam`, its names and `toggleTeam` (:163431, :226066-226072, :226386-226421); selectClass's `{c, s}` (:217552-217570); a krunker.io custom lobby opens every class (:102636); the Store item's spin messages read the live spins (:149187) | none: round 7 (2026-11-23) sends team 1 until a pick (:217234) and applies `?tm=` on the first selTeam game (:204367, :226421) | `v10_menu_e2e`, `nettspend_test`, `krunker_link_test`, `modes_test`, `strike_swap_team_e2e`, `team_select_e2e` |
| Settings | 19 | toggleADS per weapon with clones, crosshairSho / M rows, the Custom preset, customize export / import (`krk_favList` included), set()'s value checks, updateRate, ENTER_GAME's badge settings, nametagStyle 4, mods (canLoadMods, autoLoadLast, canChangeLogo, settings.txt, postProcessing), showExplo, bulletCasings / impactHoles, ocean reflection, lowSpec, weaponShine, sniperFlap, roundArms, noPaintAnim, softShad / shadowsDynamic (:205957-218149) | none (checked 2026-11-24): set()'s `showWindow(1, true)` opens a closed Settings (:214445, :216584-216621; 161e6346, `settings_perf_e2e` 35/35, now under CTest); a full model's arms and guns cast under highResShad, never our first-person viewmodel (:206820, :174143, :207421, :174260; 77af2fcc, `shadow_casters_e2e` 12/12); the skins' `noShine` (:174083-174156; f692978c, `weapons_v10_test`'s `shine`, `skin_define_test`'s Eternal Flame row; the material choice itself has no in-game test); `attachBypass` / `noAttach` on the viewmodel, zoom and aim dot (:120157, :173380-174581; 9fca20e3, 377a1188, `ui_transitions_e2e loadout-preview` 22/22) | `settings_rules_test`, `settings_perf_e2e`, `shadow_casters_e2e`, `mod_pack_test`, `v10_translate_test`, `weapons_v10_test`, `skin_define_test` |
| Servers / Host Game | 13 | the mode browser (Official / Custom tabs, groups, scores, titles, Quick Join), "+ Host Game" opens the chooser, slider limits by account type (and the server's clamp), `server.config.*` labels, the community-map card and preset loading, Raw Game Data, the private lobby, Start Game's wait state, v10's host checks and messages, Rehost, the hosting captcha for guests only (:204617-220150) | a Box Office row's region cell names the server (doc/backend.md). Force Default Settings is modelled (49a26111): the host's hidden rows are dropped and the map's config applies; KR Rewards forces it (:204284); online storage needs it, then a published map | `mm_token_e2e`, `server_tabs_e2e`, `host_game_window_e2e`, `host_game_timeout_e2e`, `kio_hostmap_e2e`, `captcha_e2e`, `config_test`, `krunker_host_custom_test`, `brain_ks_storage_e2e` |
| HUD, pause, death, end screen | 24 | kill feed colours and clan tags, the melee icon, reload / ammo / health, damage-number colours and heals, the silent hit, the death card, the default end tab, the end table's cells and buttons, the Performance stats from the end rows (scoreStat / scoreLabel / noScoreXP), the Rewards tab (Junk first), spectate info, team names (Alpha..Echo), K/D at spawn, the package streak icon, the pause menu's Spectate switch (allowSpect) (:104139-228070) | none of the end table / scoreboard data: the end table's flag and ranked icon and the Tab board's rank (50bd3949, 96ec74fd: EXT 0x81B0; `player_rank_flag_e2e`, `player_rank_flag_server_e2e`), the reward thumbnail (aac4cf5a: `.skinReward` from EXT 0x81D0; `kill_pools_server_e2e` 5, `player_rank_flag_e2e` endreward) and the Tab board's `.leaderTitle` (569a7860: ENT [34] / ADD_PLAYERS [52] titleIndex, :215052, :176753, :227597-227603, :227638, through `NET_EXT_KN_LOOK` and EXT 0x81B1, an owned title only; `player_rank_flag_e2e`, `player_rank_flag_server_e2e`, `krunker_link_test`, `demo_test`) are in. `img/frvr-ui/junk-symbol.svg` is in no dump: round 7 draws v10's own Junk glyph (Material Icons `plumbing`, #cc51f8) there | `v10_hud_e2e`, `v10_endscreen_e2e`, `ui_transitions_e2e`, `modes_test` |
| In-game chat | 6 | v10's profanity Filter (bad-words, byte for byte), row lifetime and Show Messages, chatBGOp / chatTextOutline on live rows, the player list search on aliases, no client rate limit (:226586, :228050) | `/bhop` and `/clear` stay (doc/console.md) | `chat_filter_test`, `hud_chat_e2e`, `chat_ingame_gate_e2e`, `chat_switch_e2e` |
| Class select / loadout | 19 | premium hex colours (picker values, ENTER_GAME [10] / [17] / [18] / [34] to official lobbies), presets' colours and titleIndex, the premium pair rule as v10 walks it, favourites (`krk_favList`, favItem, favourites first, Random prefers them), getSkins(account, true)'s free items for everyone (and the server accepts them), class-only items ("Req <class>", locked), name / wear / serial tags, Select Spray's order, creator caption and buttons, attachBypass, the Grappler's "Customs Only" (:24042, :214619-214655, :215052, :217478-217813, :218688, :220243-220897, :221076-221384, :222451-222535, :223487-223700, :227092-227104) | none in these files: round 7 carries `"<id>#<ind>"` slots through storage, the preview, ENT (`NET_EXT_KN_SKIN_COPY_IDS`) and the server, gives a dev every skin and a streamer / deputy / skin maker (0401 `player_skinmaker`) their items on both sides; `account_ui.c` reads `player_skinmaker` (the account info's, and a krunker.io account's setData [42], :115136 / :115193) and puts a krunker.io account's `tw_twitchname` (setData [23]) in data[31] since 37b76415; `kio_owner_items_e2e` checks both through a krunker.io "a" and an "ua" row (CTest, with `v10_customize_e2e`) | `v10_customize_e2e`, `kio_owner_items_e2e`, `krunker_link_test` |

Gaps in other owners' files, all closed by round 11 ("Menu agent's findings" below): `account_ui.c`'s logout clears
the loadout keys and selections as v10's does (:216910; `ui_loadout_logout`); `store_ui.c` sets `#spectButton` only
where allowSpect allows (:163418; `store_ui.c:388-392`); the ranked icons on the scoreboard and end table (50bd3949,
96ec74fd); `kn/en.json` lost its 28 unused `kn.*` strings (`kn.host.creating` among them).

## Round 11: the remaining in-game windows, line by line (2026-11-23)

Round 10's open items, then the Clan window, friends and party, Studios, Rent Servers, the account settings tab
and Account Manager, Report Player / Votekick and the Social and Notifications panels, each compared element by
element and handler by handler with `deob-10.0.7.js`. Every gap is fixed unless its row says otherwise; the
differences kept on purpose are in `doc/v10/kn-additions.md` ("Round 11").

| Area | Gaps | Main fixes | Left open | Tests |
| --- | --- | --- | --- | --- |
| Spins (round 10 leftovers) | 13 | item copies with wear and serial from a spin (migration 0398, `item_mints`), `#spinItemName`'s wear tier and `serialTag`, the model's wear and paint, `data.instances` over HTTP, copies kept through listings / trades / quick sell / craft, Spin Error's Purchase More with the Paint Ticket, the client-side KR and ticket checks, a ticket's own wheel, the lottery bulbs' blink (:224246-224790, :115267-115345, :127787-127793) | none: round 12 did them ("Item copies and the end table" below). A worn copy's wear on the model: the weapon, melee and cosmetic wearMaps at the worn copy's m.wear (:173646-173651, :174102-174107, :174184-174189), ours from the slot's copy, others' from SKIN_COPIES / SKIN_STYLES V2 (`skin_fx.c`, 2bcf4a68, 0bcc7acb, 8adbaa47, aac4cf5a); the assets ship no weapon-skin wearMap, so a weapon skin's wear is computed but not drawn (`doc/v10/paint-copy.md`); the market's instance view with the Mints and Wear tabs and name tags on copies (migration 0402, d7bffb29, 39074e10, cd763eda); item styles on copies (0393, the market card's `m.style` preview :110719, 0bcc7acb, ab0b47d7, 9cabc890) | `prize_wheel_scenario_e2e`, `lottery_scenario_e2e`, `brain_daily_spin`, `brain_shop`, `market_copies_e2e`, `copy_pick_e2e`, `skin_copy_ids_test`, `open_sales_e2e` |
| KrunkCup, challenges, Sales | 5 | KrunkCup teams, contributors and top players from `krunkcup_contributions` (0399), credit by the worn jersey style, the Participation Band status and claim; the game server's mid-match `chgC` with kn_brain still crediting at commit; the Sales tab's line chart (:140281-141145, :27142-27440, :180731, :225216-225229, :109206-109369) | official daily BPP values (none exist); the e2e test of a real mid-match completion is round 12's `challenge_mid_match_e2e` (45864eab) | `brain_progress` (3 drivers), `brain_v10_social_e2e`, `chal_card_e2e` |
| Clan window | 24 | the two-step logo buy and upload, the pending and approved logo, v10's error states and texts, component mounts on every show, the Discord popup staying open, no tab reset on demotion or leave, the loot defaults and clamps, live inputs (:96058-96888, :11859-12030, :220402-220435, :223808-223850) | none | `brain_clan_v10`, `kio_clan_e2e.py`, `loading_screens_e2e.py` |
| Friends, party, notifications | 22 | `gfrnd` rows into window 5's Friends tab, Remove / Follow, windows[29] with `kml` / `rml`, unread counts per tab, the read text's cleaning and the Post link (0400 `mail.link`, `mail.mail_type`), the Friends panel's conversation and send rules, the unread store, Join always switching, the filter and sort rules, keyboard tabs (:141336-142961, :221216-221354) | none | `brain_social_v10`, `brain_v10_social_e2e`, `brain_v10_party_e2e`, `brain_social_ws` |
| Studios and Rent Servers | 19 | currency rows with `is_active`, the ID copy button, Publish limits, v10's Edit Currency popup; the rent window's markup, wording, thumbnails, states, the duration select, `rentDediServer`'s reads and texts, the list order of the brain's replies (:138566-139534, :204642-204668, :219811-219935) | none | `brain_studios`, `brain_rentals_sqlite`, `brain_v10_studios_e2e`, `rent_servers_e2e` |
| Account settings, Report, Votekick | 18 | the password change without the old password, the 2FA io forms, the deletion popups and the sign-in cancel, Twitch and Discord texts and results, the connected apps list, the Usernames popup, Report Player's 10-character rule; Votekick was already at parity (:216941-217146, :219136-219176, :115381-115846, :111760-111843) | none: the Votekick "Vote Submitted - a/b" is v10's chat line, not a toast (:226852-226855); 5fd82b8d gives it on a krunker.io lobby, KN's own server already says it to the voter (`votekick_official_e2e`; the server side in `votekick_test.c`) | `brain_account_settings_e2e`, `v10_misc_popups_e2e`, `account_manager_e2e`, `brain_account_links` |
| Scoreboard and end table (from the menu round) | 11 | `rankedRank` and `flagIndex` to the client (EXT 0x81B0 after each PLAYER_INFO, gated by `showBadges.rank` and anonMode), the Tab board's `newLeaderRanked` icon, the end table's flag, picture, link, badges and rank in one cell (:176753, :226755-226764, :227637, :98188-98191) | none: the reward thumbnail is `endData.ed`'s reward cell `[reward, skinReward]` (:226765-226770), drawn since aac4cf5a (EXT 0x81D0; `kill_pools_server_e2e` 5, `player_rank_flag_e2e` endreward); `.specFlag` in `#specStatsInner` since 27ea59d7 (:216359, :216367; `v10_hud_e2e`); the Tab board's title since 569a7860 (EXT 0x81B1) | `player_rank_flag_e2e`, `player_rank_flag_server_e2e`, `krunker_link_test`, `demo_test` |
| Menu agent's findings | 5 | logout clears the loadout keys and selections (:216910) and runs `updateWindow(5)`, not `showWindow(5)`: the login modal only replaces an open Account window, a logout with no window up leaves the menu as it is (:216644); the wheel's toggle shows Spectate only where allowed (:163418); 28 unused `kn.*` strings dropped; a render scenario keeps its clan through the account change | none | `v10_menu_e2e.py`, uidiff `clan-settings` |

For the menu agent: the end screen's junk reward card from a mid-match `chgC` (`hud_ui.c`, :210022-210035); the
`#mailCount` cap of "99" with `inline-block` (`v10_menu.c`, :221232-221235); the end table's Report button's title,
icon and onclick (`end_share.c`); the Votekick "Vote Submitted" toast (`hud_votekick.c`); `specFlag` in
`hud_spectate.c` (:216359, :216367); `#newUserGuide`, the guest's tip box that logoutAcc shows and
`iíiïííi` rotates every 20 s, is not in the menu markup (`v10_menu.c`, :148225, :215666, :216910); weapon wear in `skin_fx_wear_key`; a copy's wear and serial in the loadout
tooltip (`ui_loadout.c`). `v10_email_login_e2e.py` (not in CTest) fails from its first logout on: with the login
modal up on the menu and its Email field focused, the first Escape reaches `knV10LoginKey` and closes the modal and
the second leaves the menu for the game, where the test expects a blur and then the modal closing (`v10_login.c`, the
menu's Escape).

Round 7 (2026-11-23) took that list:
- `v10_email_login_e2e` is in CTest (`brain_v10_email_login_e2e`). v10's login view has no key handler of its own
  (:117131-117455) and closes only on an overlay click (:119429), so Escape in a field now leaves the field and the
  next closes the modal, as KN's page does elsewhere.
- `#mailCount` reads "99" above 99 (:221231-221235).
- The end table's Report Player / Share Match Results buttons follow :226775-226777.
- "Vote Submitted - a/b" is a chat line on krunker.io lobbies (:226852-226855).
- `.specFlag` is in `#specStatsInner` (:216359, :216367).
- `#newUserGuide` was already in the markup, hidden by v10's own css; its inline display now follows login and
  logout (:216887, :216910). The 20 s rotation is the bundle card / ad swap.
- A mid-match `chgC` adds `REW.jnk` to the Rewards tab's Junk card (:225216-225229); there is no HUD card.
  `account_ui.c:558` calls `hud_ui_add_reward_junk` (37b76415).
- Weapon wear uses v10's wearMap key (:174184-174189). The item view shows a copy's wear and serial
  (:223653-223690). v10's createTip shows wear only from `data-wear`, which only the trade cards set.

uidiff `customize-customize` (content 3.95 %) was a sub-pixel offset of every card row (KN about 0.33 px higher;
row 1's labels a whole pixel higher, rows 2 and 3 rounded alike). Not the line boxes: Chrome lays the cards out
exactly where KN does (headless Chrome on KN's own DOM: the first card's top at 136.3907 px in both) and its
screenshot of that DOM matches the reference to the pixel. The difference was KN's scrollable overflow:
`.otherContainer` (205 px) has rows that end 4 px into `#loadoutContent`'s 18 px bottom padding, and KN added the end
padding after those descendants (scrollHeight 913 > clientHeight 909), so it treated `#loadoutContent` as a composited
scroller and moved its contents to a whole device pixel (`scroller_layer_snap`, -0.565 CSS px). Chrome adds the end
padding to the in-flow children's boxes only (ScrollableOverflowCalculator; its scrollHeight there is 909): fixed in
`layout_block.c` `compute_overflow` (2026-11-23), content 3.95 -> 0.40 %.

## Round 12: round 11's leftovers and the areas no round had covered (2026-11-23)

Round 11's open items, then the map editor (against the editor's own bundles: `editor-10.0.7.js`,
`index-10.0.7.js`, `editor-config-10.0.7.js`), the Mod Manager, the K.P.D. replay viewer, the Twitch / streams
panels, the loading screens, the captcha and anti-cheat popups and the ad boxes, compared element by element and
handler by handler. Every gap is fixed unless its row says otherwise; the differences kept on purpose are in
`doc/v10/kn-additions.md` ("Round 12").

| Area | Gaps | Main fixes | Left open | Tests |
| --- | --- | --- | --- | --- |
| Item copies and the end table | 11 | endGame's skin reward in the end table's reward cell (`.skinReward`, openSales; EXT 0x81D0); a worn copy's wear, serial and name tag to every client (EXT 0x81D1, the slot's named copy first; `skin_meta_of`); name tags on copies (migration 0402) kept through listings and trades; `/items/:id/instances` with the Mints and Wear tabs; the Market's card wear line, listing spans, the copy overlay and the `wears=` filter; a styled copy loses its style when it moves (:195123-195291, :226765-226770, :24335, :109727-109935, :110688-110790, :130983-130991, :136679-136700, :223620-223650) | none | `market_copies_e2e`, `kill_pools_server_e2e.py`, `player_rank_flag_e2e`, `brain_v10_api_market`, `brain_schema_parity`, `brain_economy`, `brain_live_trade`, `weap_nametag_e2e.py`, `open_sales_e2e.py` |
| Challenges | 3 | a real challenge completed mid-match end to end; the mid-match UPDATE_CHALLENGES_PROGRESS after each event's chgC (`challenge_progress_reported`, :180731-180741, :180925-180934, :225206-225215); a mid-match challenge's Junk into the Rewards tab (:225219) | official daily BPP values (none exist locally) | `challenge_mid_match_e2e`, `brain_progress_sqlite` / `_mysql`, `season_test`, `chal_card_e2e`, `krunker_challenges_e2e.py` |
| Account | 3 | skin makers (`player_skinmaker`, :115136) and the krunker.io account's `tw_twitchname` / `player_skinmaker` (setData [23] / [42]) reach getSkins; a Twitch link or unlink updates the streamer items at once | none | `v10_customize_e2e.py`, `kio_owner_items_e2e.py` (the krunker.io account's "a" keys and setData [23] / [42]) |
| Map editor | 27 | entities and archetypes; Publish Game and Publish Asset as v10 sends them (`uploadM`, `plm`, migration 0403 `map_mobile`); multiplayer co-editing (windows 16 / 17, `#mp-chat`); the inspector, T-key picker and AI Rarity Chances; settings, history, resets, view resolution and object visuals; Tools and Quick GUI as v10 binds them, v10's own quirks included (E10:6610-20982, IDX:6124-15193, CFG:4406-9215) | none: the default map Burg imports from the catalogue's `maps/official/00_burg.json`, v10's `maps.getMap(0)` answer (E10:20175-20184; `ed_load_quick`, 2026-11-25), and publish_ui_e2e runs in CTest as editor_publish_ui_e2e | `panels_e2e`, `inspector_check`, `objects_e2e`, `windows_check`, `tools_check`, `logic_parity`, `io_e2e`, `entities_e2e`, `mp_e2e`, `publish_e2e`, `asset_upload_test`, `brain_editor_rooms`, `editor_serialize_test` |
| Mods, replays, streams, Twitch | 18 | the Mod Manager's cards, favourites, header, mod view, `#modLInfo` texts, `?mod=`, Reset Mods, Ctrl+/; the stream thumbnails as images, event tags and the promo without Twitch; v10's Twitch link, unlink and drops popups; a same-map replay seek (:18566-18600, :97248-97966, :108350-108478, :146848-148720, :201934-201954, :209629-209632, :218751-218932, :223416-223459, :225733-226475, :228180-228245, :229274-229551) | none: round 13 closed the replay viewer, Demos / Reviewers and the streams overlay | `mod_pack_test`, `brain_streams`, `brain_streams_e2e`, `brain_v10_api_mods`, `brain_v10_social_e2e`, `brain_hub_creator_e2e`, `demo_test`, `kpd_replay_vote_e2e.py`, `menu_sources_e2e` |
| Loading, captcha, anti-cheat, ads | 7 | the map card's mode in capitals (:226159); the game socket's challenge on v10's Security Challenge card (ALTCHA 1.0.6, :98801-98984, :119735-119741); a hack-flagged login's BANNED popup and the fart_0 sounds (:216841-216855, :200724-200746); showAd's onShow after the ad (:229900-229912). v10.0.7 rotates no loading tips | none | `v10_loading_test`, `kn_captcha_test`, `krunker_link_test`, `krunker_host_custom_test`, `brain_captcha_game_e2e`, `krunker_official_e2e` (native runs), `native_auth_e2e`, `captcha_e2e.py`, `loading_screens_e2e`, `loading_switch_e2e` |
| Tests | 2 | the web e2e tests drive the checkout's own `dist/web` before `KN_WEB_DIST`; `html_anim` counts the ALTCHA spinner | none | `brain_email_web_e2e`, `html_anim_test` |

`uidiff clan-settings` has no reference to re-render: it has no regions file, so `make smoke-ui` only renders it, and
it renders the Settings tab as round 11 left it.

For the menu agent (all closed by 2026-11-24: round 13 and the three sections after it):

- **Copies** (`skin_meta.h`: `skin_meta_of(uid, skin, &meta)`, `skin_meta_wear(uid, skin)`):
  - the HUD weapItem's `.weapNametag` `"<name>"` (:176440);
  - `store_ui.c`'s openSales calls `v10_market_open_item(index, instance)` from those attributes (:223620-223650, `open_sales_e2e.py`);
  - the loadout cards and the trade item cards put data-nametag, data-wear, data-wear-float, data-serial, data-paint, data-paint-shader and data-seed on a copy (`ui_loadout_tooltip_attrs`, :221824, :223708).
- **`hud_replay.c`** (deob-10.0.7.js):
  - "SUSPECT: Player_%d" (:229769);
  - the voted dot's title "You voted on this demo" (:223259);
  - "Vote Failed" (:229656), also in place of the client-side sign-in check;
  - "Open the demo from the list to vote" (:229633);
  - `myv` from the answer, else -1 (:229643);
  - "Downloading Demo..." and "Loading Replay..." (:229696, :229725);
  - openDemoLink's errors: the body's text, "Demo not found", "Demo not found or not authorized" (:229823-229832);
  - the popup title "KPD Demo Review" and the copy button title 'Copy demo link' (:229829);
  - `?ct=` only when present, URL-encoded (:27456);
  - RESOLVE and HACKER? (:229611, :229619), with `kpd_replay_vote_e2e.py:219-220` and the uidiff baselines;
  - Exit Replay as a reload (:229805);
  - clearPops only on the first round start.
- **`hud_votekick.c`:**
  - copyDemoLink with the page origin and the 1250 ms check icon (:229835-229844);
  - the guest tag only for a non-banned row (:223203);
  - the Demos and Reviewers lists from `/kpd/demos?off&q` and `/kpd/reviewers` (:223066-223076);
  - the group-ban flag on demo rows (:223206).
- **`v10_menu.c` streams overlay:**
  - `rest = nfeat ? n - nfeat : 0` (:148357, :148470);
  - the featured-grid class with no featured stream (:146855);
  - the promo card inside the grid (:148720);
  - event tags and `has-event` only on featured cards (:146848-146859);
  - the thumbnail's children order (:146848);
  - no `.streams-body` wrapper (:146863);
  - proxy only `static-cdn.jtvnw.net`.
- **`servers_ui.c`:**
  - Box Office guest hosting through the Security Challenge card: `kn_captcha_fetch` + `altcha_modal_show`, then `captcha_e2e.py`'s hosting step clicks `#altcha_checkbox` (:119735-119741);
  - `:3872` passes `ev->target` when `this` carries no element.
- **Other files:**
  - `v10_menu.c`: no premium ad hiding for a hack-flagged login (:216881).
  - `hud_endscreen_v10.c`: the double-XP button has no `doubleXPButton` class (:210089).
  - Check that `classPreviewCanvas` hides while a map loads (:228626).
  - `loading_switch_e2e` still waits for "offline: playing map N", which `offline.c` no longer prints.

## Round 13: the K.P.D. replay and streams leftovers (2026-11-24)

The replay viewer and its two officer lists were compared again with `deob-10.0.7.js:223066-223076,
223195-223260, 229603-229844, 27443-27479`. The streams overlay was checked against its own Svelte
templates and derived stores at `:146840-146866, 148350-148490, 148536-148744`. The remaining differences
are closed:

- The bar says RESOLVE, HACKER? and SUSPECT; the loading cards, KPD Demo Review popup, copy title and
  Vote Failed text are v10's. A vote answer with no `myv` clears the old vote. Voting without a session
  reaches the API and shows its answer rather than a client-only sign-in message. The first round start
  alone clears popups. Exit Replay is `app_reload` (`location.reload()` on web, a native re-exec).
- `openDemoLink` shows the body's error, "Demo not found", or the request-catch text as appropriate;
  `?ct=` is present and URL-encoded only when a call token exists.
- Demos and Reviewers call `/kpd/demos?off&q` and `/kpd/reviewers` directly, through the official gapi
  when an official game is active. The grouped HTTP responses are normalized to the existing policeRes
  renderer, including each group's ban flag; only a non-banned suspect gets the guest tag. Stale answers
  and the 15 s failure path are kept in check.
- `copyDemoLink` copies `<origin>/?demo=<tok>&sus=<ct>`, shows `check`, and restores `content_copy`
  after 1250 ms.
- The streams overlay has no `.streams-body` wrapper; its plain grid also uses `featured-grid`, and the
  promo belongs inside its grid (including the expanded rest row). `regularStreams` is empty when there
  are no featured streams, so two ordinary streams no longer get a More button. Only featured cards draw
  event chips / `has-event`; thumbnail children follow v10's viewer, language, drops, event order. Only
  `static-cdn.jtvnw.net` uses the brain's thumbnail proxy; other hosts keep their direct URL.
- A hack-flagged premium login keeps its ad slots: v10 returns for the BANNED popup before the premium
  hide (:216877-216881). The Double XP button no longer carries a non-v10 `doubleXPButton` class
  (:210089); the native CSS uses its id attribute instead.

Tests: `kpd_phone_test` (grouped HTTP lists, errors, group / guest flags, copy feedback),
`kpd_replay_vote_e2e.py` (RESOLVE -> HACKER? -> vote, and reload-on-exit), `menu_sources_e2e` (the
unfeatured and featured / rest structures, thumbnail order and proxy hosts), `native_auth_e2e` (the
premium hack flag), `double_xp_e2e`, `v10_endscreen_e2e`, and the three replay uidiff scenarios. The
replay scenarios' KN regression baselines were re-rendered for the intended v10 text.

## The worn copy on the HUD and in openSales

- The weapon rack's label is `.weapNametag` `"name"` when the worn copy has `m.name` (:176440), from
  `skin_meta_of` (and `state.skinMeta` in a scenario). Otherwise it stays `.weapSkin`.
- `openSales(index, element)` opens the item page for that element (:223623-223650), signed in or not.
  The instance is built from `data-wear-float`, `data-serial`, `data-paint`, `data-paint-shader`,
  `data-nametag` and `data-seed`, and only when one of wear, serial, paint, shader or seed is set.

Tests: `weap_nametag_e2e.py`, `open_sales_e2e.py`.

## Loadout tooltips, the hosting challenge, and the class preview (2026-11-24)

- A loadout copy card carries `data-nametag`, `data-wear`, `data-wear-float`, `data-serial`, `data-paint`,
  `data-paint-shader` and `data-seed`, so `createTip` and `openSales(this)` see the copy (:223628, ui_tooltip.c).
- A guest's Start Game fetches the hosting challenge and shows the Security Challenge card (`altcha_modal_show`,
  hidden once solved). `favoriteM` uses `ev->target` when `this` is not an element.
- `#classPreviewCanvas` stays at main.css opacity 0 while a map loads (`menu_boot_arm`) and fades in four
  frames after the session's menu is up (:226519, :228625-228626). A scenario is a settled page, so it shows at once.
- `loading_switch_e2e` treats `[sandbox] Loaded map` as the host switch. `offline.c` prints "playing map" only
  when that map is already built; the switch often loads it on the next frame.
