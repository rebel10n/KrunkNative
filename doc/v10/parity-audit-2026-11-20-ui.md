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
| Settings | 19 | toggleADS per weapon with clones, crosshairSho / M rows, the Custom preset, customize export / import (`krk_favList` included), set()'s value checks, updateRate, ENTER_GAME's badge settings, nametagStyle 4, mods (canLoadMods, autoLoadLast, canChangeLogo, settings.txt, postProcessing), showExplo, bulletCasings / impactHoles, ocean reflection, lowSpec, weaponShine, sniperFlap, roundArms, noPaintAnim, softShad / shadowsDynamic (:205957-218149) | `showWindow(1, true)` opening a closed Settings. Round 7: arms and guns cast under highResShad (:206820, :207421), the skins' `noShine` (:174083-174156) and `attachBypass` / `noAttach` in game (:120157, :173380-174581) | `settings_rules_test`, `settings_perf_e2e`, `mod_pack_test`, `v10_translate_test`, `weapons_v10_test` |
| Servers / Host Game | 13 | the mode browser (Official / Custom tabs, groups, scores, titles, Quick Join), "+ Host Game" opens the chooser, slider limits by account type (and the server's clamp), `server.config.*` labels, the community-map card and preset loading, Raw Game Data, the private lobby, Start Game's wait state, v10's host checks and messages, Rehost, the hosting captcha for guests only (:204617-220150) | a Box Office row's region cell names the server (doc/backend.md); Force Default Settings is not modelled by the game server | `mm_token_e2e`, `server_tabs_e2e`, `host_game_window_e2e`, `host_game_timeout_e2e`, `kio_hostmap_e2e`, `captcha_e2e`, `config_test`, `krunker_host_custom_test` |
| HUD, pause, death, end screen | 24 | kill feed colours and clan tags, the melee icon, reload / ammo / health, damage-number colours and heals, the silent hit, the death card, the default end tab, the end table's cells and buttons, the Performance stats from the end rows (scoreStat / scoreLabel / noScoreXP), the Rewards tab (Junk first), spectate info, team names (Alpha..Echo), K/D at spawn, the package streak icon, the pause menu's Spectate switch (allowSpect) (:104139-228070) | country flags, the ranked icon and the reward thumbnail in the end table (no data); scoreboard titles / ranked rank (protocol change, ranked files); `img/frvr-ui/junk-symbol.svg` is in no dump: round 7 draws v10's own Junk glyph (Material Icons `plumbing`, #cc51f8) there | `v10_hud_e2e`, `v10_endscreen_e2e`, `ui_transitions_e2e`, `modes_test` |
| In-game chat | 6 | v10's profanity Filter (bad-words, byte for byte), row lifetime and Show Messages, chatBGOp / chatTextOutline on live rows, the player list search on aliases, no client rate limit (:226586, :228050) | `/bhop` and `/clear` stay (doc/console.md) | `chat_filter_test`, `hud_chat_e2e`, `chat_ingame_gate_e2e`, `chat_switch_e2e` |
| Class select / loadout | 19 | premium hex colours (picker values, ENTER_GAME [10] / [17] / [18] / [34] to official lobbies), presets' colours and titleIndex, the premium pair rule as v10 walks it, favourites (`krk_favList`, favItem, favourites first, Random prefers them), getSkins(account, true)'s free items for everyone (and the server accepts them), class-only items ("Req <class>", locked), name / wear / serial tags, Select Spray's order, creator caption and buttons, attachBypass, the Grappler's "Customs Only" (:24042, :214619-214655, :215052, :217478-217813, :218688, :220243-220897, :221076-221384, :222451-222535, :223487-223700, :227092-227104) | none in these files: round 7 carries `"<id>#<ind>"` slots through storage, the preview, ENT (`NET_EXT_KN_SKIN_COPY_IDS`) and the server, gives a dev every skin and a streamer / deputy / skin maker (0401 `player_skinmaker`) their items on both sides; `account_ui.c` must still read `player_skinmaker` and data[31] for krunker.io accounts | `v10_customize_e2e`, `krunker_link_test` |

Gaps in other owners' files, left to them: `account_ui.c`'s logout should clear the loadout keys and selections as
v10's does (:216910); `store_ui.c:300` sets `#spectButton` without allowSpect; the ranked icons on the scoreboard and
end table need `ranked*.c` and a player-info field; `kn/en.json` is stale (`kn.host.creating` unused).

## Round 11: the remaining in-game windows, line by line (2026-11-23)

Round 10's open items, then the Clan window, friends and party, Studios, Rent Servers, the account settings tab
and Account Manager, Report Player / Votekick and the Social and Notifications panels, each compared element by
element and handler by handler with `deob-10.0.7.js`. Every gap is fixed unless its row says otherwise; the
differences kept on purpose are in `doc/v10/kn-additions.md` ("Round 11").

| Area | Gaps | Main fixes | Left open | Tests |
| --- | --- | --- | --- | --- |
| Spins (round 10 leftovers) | 13 | item copies with wear and serial from a spin (migration 0398, `item_mints`), `#spinItemName`'s wear tier and `serialTag`, the model's wear and paint, `data.instances` over HTTP, copies kept through listings / trades / quick sell / craft, Spin Error's Purchase More with the Paint Ticket, the client-side KR and ticket checks, a ticket's own wheel, the lottery bulbs' blink (:224246-224790, :115267-115345, :127787-127793) | Box Office copies' wear in game (`player_mesh`), the market's instance view, Name Tags and item styles | `prize_wheel_scenario_e2e`, `lottery_scenario_e2e`, `brain_daily_spin`, `brain_shop` |
| KrunkCup, challenges, Sales | 5 | KrunkCup teams, contributors and top players from `krunkcup_contributions` (0399), credit by the worn jersey style, the Participation Band status and claim; the game server's mid-match `chgC` with kn_brain still crediting at commit; the Sales tab's line chart (:140281-141145, :27142-27440, :180731, :225216-225229, :109206-109369) | official daily BPP values (none exist); an e2e test of a real mid-match completion | `brain_progress` (3 drivers), `brain_v10_social_e2e`, `chal_card_e2e` |
| Clan window | 24 | the two-step logo buy and upload, the pending and approved logo, v10's error states and texts, component mounts on every show, the Discord popup staying open, no tab reset on demotion or leave, the loot defaults and clamps, live inputs (:96058-96888, :11859-12030, :220402-220435, :223808-223850) | none | `brain_clan_v10`, `kio_clan_e2e.py`, `loading_screens_e2e.py` |
| Friends, party, notifications | 22 | `gfrnd` rows into window 5's Friends tab, Remove / Follow, windows[29] with `kml` / `rml`, unread counts per tab, the read text's cleaning and the Post link (0400 `mail.link`, `mail.mail_type`), the Friends panel's conversation and send rules, the unread store, Join always switching, the filter and sort rules, keyboard tabs (:141336-142961, :221216-221354) | none | `brain_social_v10`, `brain_v10_social_e2e`, `brain_v10_party_e2e`, `brain_social_ws` |
| Studios and Rent Servers | 19 | currency rows with `is_active`, the ID copy button, Publish limits, v10's Edit Currency popup; the rent window's markup, wording, thumbnails, states, the duration select, `rentDediServer`'s reads and texts, the list order of the brain's replies (:138566-139534, :204642-204668, :219811-219935) | none | `brain_studios`, `brain_rentals_sqlite`, `brain_v10_studios_e2e`, `rent_servers_e2e` |
| Account settings, Report, Votekick | 18 | the password change without the old password, the 2FA io forms, the deletion popups and the sign-in cancel, Twitch and Discord texts and results, the connected apps list, the Usernames popup, Report Player's 10-character rule; Votekick was already at parity (:216941-217146, :219136-219176, :115381-115846, :111760-111843) | the Votekick "Vote Submitted" toast (menu agent's `hud_votekick.c`) | `brain_account_settings_e2e`, `v10_misc_popups_e2e`, `account_manager_e2e`, `brain_account_links` |
| Scoreboard and end table (from the menu round) | 11 | `rankedRank` and `flagIndex` to the client (EXT 0x81B0 after each PLAYER_INFO, gated by `showBadges.rank` and anonMode), the Tab board's `newLeaderRanked` icon, the end table's flag, picture, link, badges and rank in one cell (:176753, :226755-226764, :227637, :98188-98191) | the end table's reward thumbnail (`endData.ed`, not player info); the spectate flag (`hud_spectate.c`) | `player_rank_flag_e2e`, `player_rank_flag_server_e2e`, `krunker_link_test`, `demo_test` |
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
  `account_ui.c` still has to call `hud_ui_add_reward_junk`.
- Weapon wear uses v10's wearMap key (:174184-174189). The item view shows a copy's wear and serial
  (:223653-223690). v10's createTip shows wear only from `data-wear`, which only the trade cards set.

uidiff `customize-customize` (content 3.95 %) is a sub-pixel offset of the first row only. The card borders sit at
the same rows in both images (y 136, 252, 368 at the 1280 x 720 capture), but the reference's top border of row 1 is
brighter at y 136 and darker at y 138, i.e. about 0.3 px higher, and its labels sit 1 px higher (y 140-150 against
141-151). Rows 2 and 3 match to the pixel. So Chrome's first line box in `#customizeContainer` (inline-block cards,
`text-align: center`) starts a fraction of a pixel higher than the later ones, and the text snaps a whole pixel; KN
lays every line box out alike. Not changed: it is the engine's line-box rounding, not the markup.
