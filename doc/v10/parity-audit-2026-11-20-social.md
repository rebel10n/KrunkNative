# Social and backend parity audit against Krunker v10.0.7 (2026-11-20)

Scope: the hub (`ui/social.html`, `client/src/ui/social_ui.c`), profiles, clans, friends, market / trading,
store / spins / bundles / battle pass / challenges / events, leaderboards, K.P.D, notifications, account
features and creator content, plus the kn_brain HTTP and social-socket API they use. Gameplay / server and the
menu's UI windows are other audits.

## Sources and method

- `deob-10.0.7.js` = `~/jails/kru/src/Krunker_v10.0.7/deob/game.deob.js` (the game page bundle).
- `hub-10.0.7.js` = the hub page bundle `https://krunker.io/static/social-CURLIuB7.js` (sha256
  `059e3aeb7a17abb462195dce6c651b3ab9676b08aa5d229efc08e24cd0e5ba70`, fetched 2026-11-20 through the asset
  proxy), decoded with `Krunker_v10.0.3/deob/tools/deob.cjs` (7161 lines). Regenerate: fetch it, then
  `node deob.cjs social-CURLIuB7.js hub.deob.js`. The hub bundle used to be `social-HkHNbDV5.js` (doc/v10/editor-social.md).
- Checks run:
  1. Every quoted API path in game.js, diffed across 10.0.3 to 10.0.7. The only paths 10.0.7 adds are the
     OAuth "Login with Krunker" / Connected apps routes (`/account/apps`, `/oauth/actions/:id/approve|deny`,
     `/oauth/authorize`, `/activity`; deob-10.0.7.js:27656-27696). kn_brain serves them (`services/brain/oauth.c`).
     Every 10.0.7 path literal is found in KN code (the bulk market and item lock routes are written without the
     leading slash in `db_economy.c` / `v10_market.c`).
  2. The colon-style socket events: 10.0.7 adds only `msg:echo`, which `client/src/ui/v10_social.c` handles.
  3. The hub bundle's 50 social-socket ops (`io.send("<op>", ...)`) against kn_brain's route table
     (`services/brain/social_ws.c` k_routes). Unanswered before this audit: `glb`, `sd`, `dd`, `dkd`,
     `mfnds`, `plm`, `prm`.
  4. The hub bundle's reply table (hub-10.0.7.js:7020-7110) against what kn_brain sends. Never sent before:
     `glbR`, `sdR`, `ddR`, `mfr`, `plm`, `pubMOR`. (`clog`, `dbE` and `pir` are no-ops in the hub.)
  5. The hub bundle's 151 `window.*` functions against the Box Office hub's handler table
     (`social_ui.c` k_handlers plus `ui_handlers.c`).
  6. kn_brain's remaining refusals (`NOT_IMPLEMENTED`, `WHY_*`) against their 10.0.7 consumers.
- The 10.0.7 changelog's social and backend items (doc/v10/target.md "10.0.7") were already ported by
  f9736a64 and 913b3e01 and are not repeated here.

## Fixed by this audit

| # | Item | 10.0.7 source | KN location | Severity | Commit |
|---|------|---------------|-------------|----------|--------|
| 1 | Map page Leaderboard tab: the hub sends `glb` when a map page opens and draws `glbR`. kn_brain never answered, so the tab always read "Not Set Yet!". | hub-10.0.7.js:3168, 3804-3822 | `v10_catalog.c` sh_map_board; `social_ui.c` hub_map_opened / map_tools_board | high | 4a15b6c7 + client commit |
| 2 | Map Data tab (the creator's KrunkScript storage): Search (`sd` -> `sdR`), a row's delete (`dd` -> `ddR`) and Delete By Key (`dkd`). The tab's buttons had no handler and the ops no answer. | hub-10.0.7.js:3649-3696 | `v10_catalog.c` sh_map_data_search / _delete / sh_map_key_delete; `social_ui.c` h_search_pdata, h_delete_data, h_delete_data_pop / _conf, h_delete_key_data | high | 4a15b6c7 + client commit |
| 3 | The map page's Edit button (`openEdit`) did nothing: no popup, so no map funds (`mfnds` -> `mfr`), no Leaderboard Display (`ulb`) and no Open in Editor (`grabMap` -> `edMp`) from the hub. | hub-10.0.7.js:6652-6693, 3796-3803 | `v10_catalog.c` sh_map_funds; `social_ui.c` h_open_edit, h_grab_map, h_chng_map_funds, h_update_map_lb | high | 4a15b6c7 + client commit |
| 4 | `POST /maps/:id/funds` (the game's Creations window, deob-10.0.7.js:107760-107764) failed on PostgreSQL, which production runs: both UPDATEs bound an unused `$1` that PostgreSQL cannot type. SQLite accepted it, so the SQLite-only route test never caught it. | deob-10.0.7.js:107760 | `v10_catalog.c` run_map_funds | high (a production bug) | 4a15b6c7 |

Map funds fee (2026-11-23): the hub's Edit popup (and the editor's copy of it) shows a "KR Fee" of
floor(amount * 0.1) for either button (calcMapFee, hub-10.0.7.js:6657-6672). kn_brain now charges exactly that on
`mfnds`, out of what arrives: a deposit takes the whole amount from the account and the map gets the rest; a
withdrawal takes the whole amount from the map and the account gets the rest. Nobody receives the fee. The ledger
row (source `map_funds`) records it in `fee`, as the market and gift fees do. Under 10 KR there is no fee, as the
popup's "None" says. `POST /maps/:id/funds` (the game's Creations window, deob-10.0.7.js:107760-107764) shows no
fee line and still moves the exact amount.

Tests: `services/brain/tests/community_maps_test.py`, section "the hub's map page tools", on SQLite, PostgreSQL 18
and MariaDB 11.8.

## Open: missing or wrong, not fixed yet

| # | Item | 10.0.7 source | KN state | Severity |
|---|------|---------------|----------|----------|
| 5 | Mod page Edit (`updateMod` -> `plm` -> the publish popup prefilled, then `/upload-user-mod`). The button is drawn (`social_ui.c` creation actions) but has no handler, and kn_brain has no `plm`. | hub-10.0.7.js:6640-6651, 6814-6838 | dead button | medium |
| 6 | Gift Premium (`confPremium` / `premiumGiftConfirmed` -> `prm`). | hub-10.0.7.js:5751-5759 | done (2026-11-23): `confPremium` / `premiumGiftConfirmed` in `social_ui.c`, brain `prm` (`sh_premium_gift`) | medium: Premium is real money only (Xsolla), so `prm` checks the target and option and answers v10's error frame with Box Office's no-payments text |
| 7 | Hub functions with no Box Office handler. Each needs checking, since some are drawn under other names and some (`getSavedVal`, `saveVal`, `genericPopup`, `resetPops`, `createTip`, `cancelTip`) are page helpers the C side replaces: `removeFriend`, `showFriends`, `chatBanUser`, `copyProfile`, `shareGameTweet`, `socPostRes`, `updateChatList`, `clanRSr`, `changeRegion`, `hideAll`, `openListing`, `buyblackM`, `confListPopup`, `confQSPopup`, `loadMoreTradeHistory`, `bundleFilters`, `openReport`, `viewReporter`, `uploadMod` / `selectFile` / `updateFile`, `setCustomImages`, `createBadgeTip`. These are drawn but dead: `clanRSr`, `changeRegion`, `shareGameTweet`, `updateChatList`, `socPostRes`, `hideAll`. | hub-10.0.7.js `window.*` | done (2026-11-23): rechecked, no dead hub button left; `createBadgeTip` and the badge cards added | medium |
| 8 | Item styles: `POST /items/:id/style {item_id, style}` (the style picker, deob-10.0.7.js:108820-108848), a copy's `m.style` (inventory 110719, cards 222514, 223502) and the `_s<n>` texture of a styled item in game (173602). Two items carry `styles`: KrunkCup Jerseys (48 styles, from bundle #30, which Box Office sells) and the KrunkCup Participation Band. kn_brain refuses the route (`WHY_ITEM_STYLE`). | deob-10.0.7.js:108820-108848, 173602 | refused | medium (low reach) |
| 9 | Map versions: the Creations window's Versions tab (`getMapVersions` / `revertMapVersion`, deob-10.0.7.js:107794-107835; the list reads `version`, `created`, `data_size`, 108235-108266). kn_brain answers `[]` and refuses the revert (`WHY_VERSIONS`); the client has no Versions tab. | deob-10.0.7.js:107794-108270 | empty / refused | medium |
| 10 | Email preferences (`GET/POST /email/preferences`, the settings toggle at deob-10.0.7.js:116916-116930). kn_brain refuses (`WHY_PREF`). | deob-10.0.7.js:116916 | refused | low |

## By design: no change

| Item | Why |
|------|-----|
| `/iap/*`, PayPal, `/shop/cancel-subscription` (`WHY_PAYMENT`) | Box Office has no payment provider; real-money checkout is a user decision (doc/v10/checkout-*.md). |
| Username reserve / swap / renew (`/account/usernames/*`, deob-10.0.7.js:26405-26430, 115380-115540) | Usernames are paid for with real money (the `usernamePurchased` / `usernameRenewed` checkout events, 115430-115445), so the same decision applies. |
| Krunker Cup wristband claim (`WHY_WRISTBAND`) | A physical event item. |
| Kid / age-consent and the migrate / username / password challenge states (`/auth/kid/*`, `/auth/*-challenge`) | Krunker's backend issues these; Box Office's sign-in never enters those states. A real age-consent flow would be a legal and product decision. |
| Studio skins (`/studios/:id/skins*`) | Open: the Skin Manager's skins exist since 2026-11-23 (below), but a studio does not take them in yet (studios.c refuses STUDIO_SKIN_NOT_FOUND, db_studios.c lists none, the Studios window's Your Skins list is empty). |
| Item instances (`/items/:id/instances`, Mints / serial / wear) | No item on Box Office is minted per copy; the route answers `[]`. |

## Status 2026-11-20 (end of the session)

- 5 Mod page Edit: done (`plm`, the Update Mod popup; asset_upload_test, hub_creator_e2e).
- 6 Gift Premium: done 2026-11-23. The hub's `confPremium(i)` popup (`windows.store.premium.gift`, YES / NO) and
  `premiumGiftConfirmed(i)` send `["prm", id, i]` as hub-10.0.7.js:5751-5759 does. v10.0.7 sells Premium for real money
  only, so the brain checks sign-in, the target and the option (0-3) and answers `["po", null, "Purchases are not
  available in Krunker Box Office."]`. No price is invented. social_ws_test (all three stores), hub_creator_e2e.
- 7 Hub handlers: clanRSr, changeRegion, updateChatList, socPostRes, hideAll, confListPopup, confQSPopup and
  bundleFilters were already implemented under the same names; buyblackM is KN's buyIllicit (`pbm`); openListing is
  empty in Krunker and copyProfile unreferenced. Added: shareGameTweet, removeFriend and the friends sidebar (`chtb`,
  `msg`), chatBanUser, the staff Reports tab (`rps`), loadMoreTradeHistory, createTip / cancelTip, the image
  collection. 2026-11-23: badge cards and `createBadgeTip` (hub-10.0.7.js:460-463) added: the profile header's
  `.customBadge` (stats.bdg), the Badges tab's `.badgeCard` grid in table order, and the tooltip (name in #FBC02D,
  italic 8px description). The art is v10's `textures/badges/badge_<id>.png` from the asset host, the same files the
  nametags, HUD and menu already use. Rechecked the dead list: none is still dead.
- 8 Item styles: brain done (POST /items/:id/style, socket `ist`, item_styles 0393, m.style on the owned list;
  0be47980). Left for the client: the style picker popup, the inventory's Style button, the `_s<n>` thumbnails and
  in-game texture (deob-10.0.7.js:108855-108905, 135291, 173602), and the game server's loadout carrying m.style.
- 9 Map versions: brain done (map_versions 0392, list and revert; 60aad066). Left for the client: the map detail
  popup (svelte-1kkpw01) with its Versions tab is not ported.
- 10 Email preferences: done (0391, the settings toggle, the register box, the opt-in popup on the game page;
  0db2983a, 55bd7a2b; email_prefs_e2e). Box Office asks only when it can mail (`accounts.email.promotional_optin`).


## Sweep 2026-11-23

Every `window.*` function in hub-10.0.7.js and every handler its markup calls was checked against social_ui.c's
handlers (`window.<name>` resolves to `<name>`, html_dom_core.c), and every genericPopup class against KN's.

- Profile header `rankedRank` (hub-10.0.7.js:2780): ported. The rank icon (`img/ranked/ranks/rank_<tier>.svg`,
  index-D_a-cl5k.js's table, unknown ranks the unranked icon) follows the selected badge as `.playerProfileRank`
  for a truthy rank. Box Office's `["r", "profile"]` row now carries `rankedRank` (profiles.c: the running season's
  rank, the same rule as the gapi profile's `ranked_rank`, social_profile_rank); a krunker.io account's profile row
  is krunker.io's own, read the same way. social_v10_test, hub_creator_e2e.
- Map page tools: the markup was Box Office's own; now v10's. The Edit popup (openEdit :6653-6656, with the KR Fee
  line of calcMapFee :6657-6671 and chngMapFunds sending parseInt of the field), the Leaderboard board (glbR
  :3804-3822) and Data rows (sdR :3663-3681) with getTime for "time", Delete By Key (deleteDataPop / deleteDataConf
  :3627-3642: "No Key Provided", YES / NO) and the funds answer (mfr: SUCCESS, else the default error). kn_brain charges
  the KR Fee the popup shows (see "Map funds fee" above). hub_creator_e2e, community_maps_test.
- Publish / Update Mod popup: the markup was the game window's (mods_ui.c publish_form); the hub's is now v10's
  (publishModPopup :6734-6754: publishLeft / publishRight, selectFile, uploadMod, #modResHolder). Box Office keeps
  its own upload transport (the platform file dialog, chunked to kn_brain). hub_creator_e2e.
- Left as they are: `buyblackM` is KN's `buyIllicit` (same frame, KN adds an idempotency key); `openOneTrust` (the
  ad consent manager, Box Office shows no ads); the documented Box Office additions (KR in trade offers, the
  developers' creator badge, the map page's Versions tab from the game's Creations window).

## Sweep 2026-11-23, second pass

The hub's socket frames (every `send("…")` in hub-10.0.7.js) and its user-facing strings were checked against
social_ui.c, mods_ui.c and kn_brain.

- Map funds: kn_brain charges the Edit popup's 10% KR Fee on `mfnds` (see "Map funds fee" above).
  community_maps_test on SQLite, PostgreSQL and MySQL; hub_creator_e2e.
- `favoriteM(kind, this, id)` (:6616-6622) was a no-op in the hub. It now flips the star's `sel` and sends
  `["fvM", kind, id, 1|0]`. That is v10's order (the game sends the same, deob-10.0.7.js:228231-228236). kn_brain
  read `[id, kind, flag]`, so it now reads v10's order. The Mod Manager (mods_ui.c) sends v10's order too. The
  Games window (servers_ui.c, another agent's file) still sends `[id, 0, flag]`. kn_brain still reads that form: its
  third element is the kind 0, where v10's is an id. hub_creator_e2e.
- Followers / following popups (fwsr / fwr :5360-5394): ported to v10. The header is "Your followers" / "You
  follow" for the viewer's own lists, else "<name>'s followers" / "<name> follows". The viewer's own following
  rows say Unfollow (`deleteFollower(index, this)` :5395-5406: unfollows, removes the row, counts down
  #followC and #followCP), other rows say Follow. `viewFollowers(1)` / `viewFollowing(1)` (the feed's counters)
  show the signed-in account's lists. The request is v10's `["gfs" | "gfg", name, id]`. kn_brain answers
  `[rows, name, id]`, rows with `friendid`. social_ws_test, hub_creator_e2e.
- Market Inventory / My Sales stats (#invStats :3308-3349): KN wrote two elements the page does not have, so the
  line never showed. Now v10's: "N / <skins> Items" and "Est. Value", or "N / 20 (40 premium) On Sale" and
  "Potential Earnings" (the listings' prices). hub_creator_e2e.
- The hub's mod upload says "Loading..." and "Mod Uploaded!" (:6755-6773, 6829), and "Mod.zip size Limit
  Reached" for a .zip over the limit.
- Left as they are: the pfp image of a follows row or post (`./img/player_pfp.png`; v10 builds a per-player
  avatar URL that Box Office does not serve); the page titles ("Mods - Krunker Hub", ...); the badge tables
  (served by kn_brain).

## Upload Mod window (window 19), 2026-11-23

The game's Upload Mod window (mods_pub, deob-10.0.7.js:220943-221050) now uses v10.0.7's own markup:
#modPubBody, Thumbnail (PNG) with "Level 10 required" / Locked under level 10, Mod File (.zip), Select Thumbnail /
Select Mod Zip (`selectFile`), the name, #modUpResp and Publish (`windows[18].submit()`). This is not the hub
popup's markup: the game has its own form, and v10.0.7 decides. The flow is v10's too:
- `onFileChange` writes the file name into #<id>H's title and "round(size, 2) / limit" into #<id>Size.
- `submit` checks, in order: "Mod name is required", "Select a .zip mod file", "Mod .zip exceeds N MB limit",
  "Thumbnails require level 10+".
- Then "Requesting upload key...", "Uploading...". Errors go through errors.mods.*, else "Mod upload failed.
  Please try again.".
- On success the window closes on "Mod Published!" and the Mod Manager opens on My Mods.

As before, selectFile opens the platform file dialog, and the upload is chunked to kn_brain. hub_creator_e2e
covers it: a stand-in file dialog, the size lines, and a publish landing in published_mods.
