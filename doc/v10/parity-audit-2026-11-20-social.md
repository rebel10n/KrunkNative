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
| 5 | Mod page Edit (`updateMod` -> `plm` -> the publish popup prefilled, then `/upload-user-mod`). | hub-10.0.7.js:6640-6651, 6814-6838 | done (4b4a74ee): `h_update_mod` in `social_ui.c`, brain `plm` in `creations.c` | medium |
| 6 | Gift Premium (`confPremium` / `premiumGiftConfirmed` -> `prm`). | hub-10.0.7.js:5751-5759 | done (2026-11-23): `confPremium` / `premiumGiftConfirmed` in `social_ui.c`, brain `prm` (`sh_premium_gift`) | medium: Premium is real money only (Xsolla), so `prm` checks the target and option and answers v10's error frame with Box Office's no-payments text |
| 7 | Hub functions with no Box Office handler. Each needs checking, since some are drawn under other names and some (`getSavedVal`, `saveVal`, `genericPopup`, `resetPops`, `createTip`, `cancelTip`) are page helpers the C side replaces: `removeFriend`, `showFriends`, `chatBanUser`, `copyProfile`, `shareGameTweet`, `socPostRes`, `updateChatList`, `clanRSr`, `changeRegion`, `hideAll`, `openListing`, `buyblackM`, `confListPopup`, `confQSPopup`, `loadMoreTradeHistory`, `bundleFilters`, `openReport`, `viewReporter`, `uploadMod` / `selectFile` / `updateFile`, `setCustomImages`, `createBadgeTip`. These are drawn but dead: `clanRSr`, `changeRegion`, `shareGameTweet`, `updateChatList`, `socPostRes`, `hideAll`. | hub-10.0.7.js `window.*` | done (2026-11-23): rechecked, no dead hub button left; `createBadgeTip` and the badge cards added | medium |
| 8 | Item styles: `POST /items/:id/style {item_id, style}` (the style picker, deob-10.0.7.js:108820-108848), a copy's `m.style` (inventory 110719, cards 222514, 223502) and the `_s<n>` texture of a styled item in game (173602). Two items carry `styles`: KrunkCup Jerseys (48 styles, from bundle #30, a USD bundle Box Office does not sell: ui-backend-audit.md row 16) and the KrunkCup Participation Band. kn_brain refuses the route (`WHY_ITEM_STYLE`). | deob-10.0.7.js:108820-108848, 173602 | refused | medium (low reach) |
| 9 | Map versions: the Creations window's Versions tab (`getMapVersions` / `revertMapVersion`, deob-10.0.7.js:107794-107835; the list reads `version`, `created`, `data_size`, 108235-108266). kn_brain answers `[]` and refuses the revert (`WHY_VERSIONS`); the client has no Versions tab. | deob-10.0.7.js:107794-108270 | empty / refused | medium |
| 10 | Email preferences (`GET/POST /email/preferences`, the settings toggle at deob-10.0.7.js:116916-116930). kn_brain refuses (`WHY_PREF`). | deob-10.0.7.js:116916 | refused | low |

## By design: no change

| Item | Why |
|------|-----|
| `/iap/*`, PayPal, `/shop/cancel-subscription` (`WHY_PAYMENT`) | Box Office has no payment provider; real-money checkout is a user decision (doc/v10/checkout-*.md). |
| Username reserve / swap / renew (`/account/usernames/*`, deob-10.0.7.js:26405-26430, 115380-115540) | Usernames are paid for with real money (the `usernamePurchased` / `usernameRenewed` checkout events, 115430-115445), so the same decision applies. |
| Krunker Cup wristband claim (`WHY_WRISTBAND`) | A physical event item. |
| Kid / age-consent and the migrate / username / password challenge states (`/auth/kid/*`, `/auth/*-challenge`) | Krunker's backend issues these; Box Office's sign-in never enters those states. A real age-consent flow would be a legal and product decision. |
| Studio skins (`/studios/:id/skins*`) | Fixed (2026-11-23): import a Skin Manager skin with a thumbnail (the Studios window's Your Skins and Assign Skin, v10_studios.c), list, publish / unpublish (db_studios.c, studio_skins of migration 0395; studios_test.py). |
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
- Left as they are: `buyblackM` was KN's `buyIllicit` (now `buyblackM`, see "Sweep 2026-11-23, third pass"); `openOneTrust` (the
  ad consent manager, Box Office shows no ads); the documented Box Office additions (KR in trade offers, the
  developers' creator badge, the map page's Versions tab from the game's Creations window).

## Sweep 2026-11-23, second pass

The hub's socket frames (every `send("…")` in hub-10.0.7.js) and its user-facing strings were checked against
social_ui.c, mods_ui.c and kn_brain.

- Map funds: kn_brain charges the Edit popup's 10% KR Fee on `mfnds` (see "Map funds fee" above).
  community_maps_test on SQLite, PostgreSQL and MySQL; hub_creator_e2e.
- `favoriteM(kind, this, id)` (:6616-6622) was a no-op in the hub. It now flips the star's `sel` and sends
  `["fvM", kind, id, 1|0]`. That is v10's order (the game sends the same, deob-10.0.7.js:228231-228236). kn_brain
  read `[id, kind, flag]`, so it now reads v10's order. The Mod Manager (mods_ui.c) and the Games window
  (servers_ui.c) send v10's order too, so kn_brain reads only that order (another kind, or no id, is dropped).
  hub_creator_e2e; social_ws_test (fvM on the 3 stores); kio_play_e2e (the Games window's star).
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
- Left as they are: the page titles (now set, see "Sweep 2026-11-23, third pass"); the badge tables (served by kn_brain). The
  per-player pictures were left here too, and are now fixed (see "Profile pictures" below).

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

The thumbnail preview showed a dark square. The cause: KN set the preview's src to `file://<path>`, and the
engine's image cache refuses any URL with a scheme (image.c `image_resolve_url`). The img drew only its own
`background: rgba(0,0,0,0.3)`. Its `onerror` did not run either: the native engine ran no img onerror handlers. v10
reads the picked file with `FileReader.readAsDataURL` (deob-10.0.7.js:220973-220980; the hub's updateFile,
hub-10.0.7.js:6786-6792). KN now does the same: the preview's src is a `data:image/png;base64,` URL of the file, in
the window and in the hub popup. hub_creator_e2e checks the src and samples the preview's colour in a screenshot.

## Profile pictures, 2026-11-23

v10 builds a player's picture as `<userAssetsHost>/p<id>/profile.png?v=<the hour>` (config-BDN_kuhS.js:4749-4757,
where the hour is the player's own upload time after an upload). kn_brain now serves that path from the approved
profile pictures (doc/backend.md, user assets). The hub uses it wherever v10 does, with v10's conditions and
fallbacks:
- navbar #profileImg and the feed composer #postProf: premium, else the class icon / player_pfp.png (:2170, :3582,
  :3608);
- the profile's #socialPic: showPfp and a premium player, else the class icon ("a" for Kamile) (:2770);
- posts and comments: `prm > 0` gives `pli`'s picture (:4944, :4948). The reply box shows the viewer's own picture
  (:4977);
- chat messages: always (:2311). Friend bubbles, the Friends list and startChat's picture: subscribers only, else
  `src=''` (:2340, :5330);
- follows rows: always (:5366, :5388);
- the maps list's "is Playing" pictures (frndPlyingH, :6513-6533). This block was missing and is added. Box
  Office's presence says "In game", not "playing <map>", so it shows only for a krunker.io account.

A krunker.io account's hub uses user-assets.krunker.io. The account page's approval moves the own picture's `?v=`
past the hour, as v10's pfpUploaded does.

The native engine now runs `<img onerror>` (engine.c `img_errors_run`). It runs the handler when a load fails for
good: `src=''`, a refused URL, an undecodable file, or a remote picture the fetcher gave up on (an HTTP 404). It
does not run while a download is pending or while game content may still arrive. It runs the statements the pages
use: `this.onerror=null`, `this.src='…'` and `this.style.<prop>='…'`. The remote fetcher accepts WebP (Box Office's
pictures are WebP) and wakes the engine on a failure. This also gives the maps and mods lists their noimg.png
fallback.

showFriends is v10's: "Your Friends" with "N Total", `target='_blank'` names, "Remove Friend" and "You have no
friends". The pictures' fallback is player_pfp.png, not favicon.png.

Tests: brain_avatar_e2e (p<id>/profile.png under /user-assets and /api/user-assets: served when approved, 404
otherwise) and hub_creator_e2e (a seeded approved WebP drawn in a chat message and a follower row; the 404 and
`src=''` fallbacks).


## Sweep 2026-11-23, third pass

- Page titles: v10 sets `document.title` through iíïïiìi (hub-10.0.7.js:7136-7141). Each page load first resets it
  to "Krunker Hub", then sets the page's own title: "Mods - Krunker Hub" (:3130), "Games - Krunker Hub" (:3149), the
  mod or map name + " - Krunker Hub" (:3143, :3164; the name escaped as config's `am` does), "Leaderboards - Krunker
  Hub" (:3183, when rows arrived), "Skin List - Krunker Hub" (:3417), "<skin> Skin - Krunker Hub" (:3488) and
  "[<clan>] Clan - Krunker Hub" (:3573). `app_set_page_title` (app_mode.c) writes the page's `<title>` and the
  window title: `glfwSetWindowTitle` natively, `document.title` on the web. Each page has its own window, so the
  game window keeps its own title. hub_creator_e2e reads `<title>` on the map page, the Mods page and the market.
- startChat: the chat header shows the raw `sm` the caller passed, as v10 does (:2249-2255). When `sm` is missing or
  "undefined" it shows a grey dot and "Offline", even for a friend who is online. Join (v10's launch icon) shows
  unless the game argument is "". The brain's `chs` presence push is no longer drawn (v10 has no such frame). Friend
  rows pass `sm` and `gid` as v10's bubbles do (:2340, :5337). Opening a chat sets its #msgCnt to "0".
  social_hub_staff_e2e, hub_creator_e2e.
- Trades: the Trading tab used v3.1.1's layout: an Incoming / Outgoing / History tab bar (`updateTrades`) with one
  column shown at a time. It now uses v10's (iiîiïií :2677-2714): three `.offers` columns side by side.
  - Heads are grey, and the History count is `#hisCnt`.
  - Each row follows iíîììîì (:2646-2675). It always has a style attribute. Its onclick is
    `viewOffer(trid, kind, "name", "ids", "ids"[, "message"])`. Received rows with a message show the markunread
    icon.
  - Each side's items are sorted as iiîíïií sorts them, and padded up to a multiple of 5 of the larger side
    (:2629-2636). The KR part of an offer is v10's `.oCurrencyLine` "+N KR".
  - Ages are v10's: one unit only, "3d ago" / "5h ago" / "2m ago" / "just now" (config bI). The staff Reports tab
    uses the same format.
  - Decline All clears the incoming rows and sets the head's count to 0, as iíïìiîí does (:5722-5735). Before, it
    replaced the whole column.
  - A quote in a trade message no longer breaks the row's onclick. v10 breaks there.
  - hub_creator_e2e checks the columns, #hisCnt, the ages and the onclick.
- Black Market: the popup's button calls `buyblackM(index)` (:4834, 5182). It was KN's `buyIllicit(index, key)`.
  The idempotency key is kept per popup on the C side.
- Profile Creations: the Games and Mods cards had a "by <creator>" line. v10's cards (:3023, :3066) have none,
  since it is the creator's own profile. The line is removed.
- Rechecked: every `window.*` function in hub-10.0.7.js has a handler of the same name, is drawn by the C side
  (`genericPopup`, `hideAll`, `socPostRes`, `updateChatList`, `clanRSr`), or is unused in v10 (`copyProfile`,
  `showError`, `openListing`).
- Left as they are:
  - The KR History popup on #profileKR (`krHistory`, the ledger's view) and KR in trade offers (`tradeKR`). Both
    are Box Office additions.
  - The developers' Creator button (`creatorPopup`).
  - The map page's Versions tab (`revertMapVersion`).
  - The catalogue thumbnails where v10 uses `<assets>/m<id>/thumb.png`, and still-frame sprays. Both are done
    since, see "Sprays and map thumbnails" below. (The note here said the skin table lacked `frames`. That was
    wrong: the viewer table has had `.frames` / `.frame_t` from gen_viewer_store.js all along.)


## Sprays and map thumbnails, 2026-11-23

- **Animated sprays.**
  - How v10 knows a spray's frames: from the store item's `frames` and `frameT` (store.skins; a studio skin's are
    copied the same way, config-BDN_kuhS.js:5208-5210). Nothing is derived from the texture.
  - KN's viewer table already has these fields as `.frames` / `.frame_t` (gen_viewer_store.js:86, from
    assets/skins.json). 41 sprays have them.
  - The hub now draws each one as v10 does, through iìïîìiî (config:4870): `animation: <name><frames>
    <1 + frameT/1000>s steps(<frames>) infinite`.
  - Each list's `<style>` carries one `@keyframes <name><frames> {100%{background-position:-<frames * W>px}}` per
    length, with v10's name and width W for each list:

    | List | Name | W |
    |---|---|---|
    | Trade offer rows (:2706) | `sprayAnmOf` | 60 |
    | Market, Inventory, My Sales and Black Market cards (:3264, :3353) | `sprayAnm` | 200 |
    | The profile's Listings (:4149) | `sprayAnm` | 200 |
    | The item sales page (:3432) | `sprayAnm` | 220 |
    | showPopup (:4852) | `sprayPopAnm` | 220 |
    | viewOffer (:4026) | `sprayAnm` | 60 |
    | The trade window (:5518) | `sprayAnm` | 60 |

  - The market lists draw cards lazily (ui_vgrid.c), so their keyframes cover every row of the list.
  - The Market Stats tab is left still. v10 names its cards' animation `sprayAnm` but writes `sprayStatAnm`
    keyframes (:3210, :3224), so nothing moves there in v10 either.
  - The native HTML engine read `<style>` only at page load, so these keyframes (and the events popup's
    `sprayAnmB`, Settings' `sprayAnmD` and the loadout's `sprayAnm` / `custIconAnm`) never registered and the
    sprays stood still. Fixed in engine.c (engine_style_elements):
    - dom.c counts every `<style>` element added, removed or moved, and every change to its text
      (dom_style_epoch);
    - when that count moves, the engine registers the @keyframes of the `<style>` elements added since load. It
      rebuilds the sets as Chrome resolves them: the UA sheet, the author sheets, then those elements in
      document order, the later one winning a name. A removed element's sets go with it;
    - only @keyframes are taken from them, which is all the UI writes in a runtime `<style>`.
  - hub_creator_e2e: an Inventory spray (Clickbait, 2 frames, frameT 550) has `sprayAnm2 1.55s steps(2)`, the
    list has its keyframes, and screenshots of the card show both frames. html_runtime_sheet_test checks the
    epoch.
- **Map thumbnails.**
  - kn_brain answers `m<id>/thumb.png` for a live Box Office map from the PNG the editor's Publish sent (see
    doc/backend.md, user assets). A map without one is a 404, and an id with no Box Office map goes to the
    Krunker mirror.
  - The hub uses v10's URLs wherever v10 does, with the same `map_info.t` / `mod_info.t` / `dat.t` conditions:
    - the Games and Mods lists (:6534, :6611);
    - the profile's Creations (:3023, :3066, :3085);
    - the map and mod pages (:3142, :3163; the map page reads `mi`);
    - the featured map card, always (:6179);
    - the featured mod card. Its string is v10's own: `url(<thumb or ''>.png)`, so `thumb.png.png` (:6322).
  - Feed posts now show v10's `postIMG`: a game's or mod's thumb.png (typ 2, 3, 7), and an unboxed skin's
    picture with its name and "Just unboxed a <name>!" (typ 5) (:4883-4944).
  - The Mod Manager (mods_ui.c) points Box Office mods' thumbnails at kn_brain's host; they were on Krunker's.
  - brain_user_assets_e2e: served, 404, mirror, deleted. hub_creator_e2e: the map page's src, the bytes, and the
    drawn colour.
- **The first page waits for the saved login.**
  - v10's hub socket is signed in from its URL (`pat=`, hub:7050). So the page request it sends on connect
    (:7063-7065) already answers as the account.
  - kn_brain's session signs in with the `a` that account_ui.c's restore sends. When that is still in flight
    ("Logging in..."), hub_connected used to request the page anyway. The brain answered a guest's null, and
    ?p=market&q=trades stayed on "You must be logged in....".
  - Now that request waits for the login reply (io_a, either way).
  - economy_hub_e2e's "bob sees the incoming offer" failed on this.
- **Still different, found while doing this.** All four are done in the next section:
  - the Market Stats tab was v3.1.1's "Rarest Items" list;
  - the market and Black Market cards missed v10's fields;
  - `showPopup` had v3.1.1's signature;
  - feed posts drew no game results (typ 8) or KR giveaways (typ 9).

## Hub round 7, 2026-11-23

There is no deob-10.0.7.js on disk. Line numbers below are from the three bundles the hub loads, de-obfuscated:
hub-10.0.7.js (`hub.deob.js`), config-10.0.7.js (`config-BDN_kuhS.deob.js`) and classes-10.0.7.js
(`classes-Wgqksi18.deob.js`).

Every section below was compared line by line against those sources.

- **Market cards** (hub:3312-3346, `market_card`).
  - id `itemCard<sub>_<uid>` and `data-index`.
  - The copy's `data-nametag`, `data-wear`, `data-wear-float`, `data-serial` and `data-seed`.
  - createTip / cancelTip.
  - The nametag line, the type line, and the developer's `ID:` line (`?dev`).
  - `marketImgP` (paints) and `marketImgS` (sprays).
  - Inventory shows the estimate (`~ N KR`, "Unknown" with no price) and the count. The other lists show the
    price.
  - The badges: the limited tag, the sound icon, and isAnim. KN uses the rarity's animate flag for
    "rarity 6".
  - The action is Purchase / List / Unlist with v10's eleven-argument `showPopup`. A read-only list keeps
    the label without the onclick.
  - Inventory then shows Quick Sell. The other lists show Search, with Share (`shareListing`) everywhere
    except the inventory.
  - Last come Info | Preview.
- **Stats tab** (hub:3197-3226). Market cards with "In Circulation", Preview / Search / Info, and the badges.
  The v3.1.1 Rarest Items list is gone.
  - v10 writes the keyframes as `sprayStatAnm*` (60 px), while the cards animate `sprayAnm`. So the sprays
    stand still in v10, and in KN too.
  - v10's extra trailing `</div>` is kept.
- **Black Market** (hub:3229-3264).
  - Every blackM item that is not hidden and not free, priced (FREE when 0), with `data-index` and the tip.
  - Purchase calls `showPopup("blackm", i, thumb, label, i, funds)`, followed by Info | Preview.
  - An item the account already has gets `blackMDone`. For that, kn_brain's `blackm` page now answers the
    indices bought through the Black Market or owned. A guest gets `[]`.
- **showPopup** (hub:4790-4857) takes twelve arguments: `(page, skin, thumb, label, skinid, funds, c,
  salestart, player, uid, marketid, refresh)`.
  - Changes:
    - `lastPop`;
    - `refreshPopup`;
    - the 300000 ms grace window, with its one-second timer (hub:4363-4377, config:9456);
    - the count overrides that a list reply sets and that io_0 clears (hub:5250, 2721);
    - the 3D button, plus `rainbowBorder` on sprays;
    - `pItemOwn` with a link to `./?profile=`, which KN routes to the hub profile;
    - `pItemTime` (getReadableTime with seconds, config:4311).
  - Callers updated:
    - the market cards;
    - the Black Market;
    - the profile's Listings (`io_ul`, hub:4104-4154);
    - `refreshPopup`.
  - The native inline-handler parser now takes 12 arguments (`EV_HANDLER_MAX_ARGS`).
- **A listing's page** (`hub_render_listing`): v10's `pItemDesc2` markup, `buyItem(id, null)` and the grace
  timer. A blocked item writes into `itemsalesHolder`.
- **Feed posts** (hub:4875-4944).
  - typ 8: the `#postGameRes` table. Its columns are the mode's `endStats` (or config's default,
    config:9067), capitalised, with `sid` left out. The rows are `lnk2`'s flat values, ranked, and the
    poster's name is shown in white.
  - typ 9: "ends in" plus the time left, `krGivPost` with the KR, and "Follow and Like to Enter" outside a
    single post.
  - `yt-<id>` tags are stripped from the text.
  - On a profile, the posts are `socPost fromProfile`, and an empty list shows `noneText`.
  - kn_brain's socket posts (`post_json`) now send typ 8's `lnk2` raw, so the client can parse its JSON
    rows. Each value is escaped when drawn. They also send typ 9's `fds` and `exp`, as GET /social/feed
    does.
- **Items page** (hub:2413-2541, 3364-3417). The card has:
  - the `ID:` line;
  - the Viewer action;
  - the badges;
  - `hasSnd` after the actions.

  The filters:
  - the season and rarity presence;
  - the store.tags limited tags, from classes-10.0.7.js:3586-3618, now complete with Battle Pass
    `#00BCD4` and Premium Spin `#fc038d` (`ui_loadout_tag_name` / `_color`);
  - the spinnable rules;
  - v10's exclusive categories;
  - blocked and opensea items excluded;
  - a two-way search over keywords, drops, limited tag, weapon, name and creators.

  The season list runs up to the current season.
- **The item page** (`itemsales`, hub:3418-3494).
  - `spraySalesImg` / `imgWepP`, the label's position, 3D (`viewItem(i, 2)`) and the bound lock.
  - The creators' links, the contraband name colour, the lore, the event name (one of config's 23), the emoji
    text and the streamers.
  - For an opensea item, the tabs are hidden.
  - For a noSale item, "Cannot be Sold" is shown.
  - The owners' rows are drawn in `#5c5c5c`.
- **viewItem** (hub:2054-2069): `blackShad` except on contraband items; the Black Market / limited / Vaulted /
  Limited labels; card, emoji and paint views for types 16-18.
- **Filter / Sort popups** (hub:4392-4600).
  - The labels Filter/Sort, Inventory Sort, Sort and Search.
  - `seaFilt` / `seaSort` up to the current season.
  - "Hide Non-Tradable Skins".
  - `srt` sends onlyMissing and nonTradable.
  - kn_brain answers Only Show Missing with the store items the filters keep that the account lacks, as
    `str`'s read-only third value. Non-tradable items are dropped on request.
  - **Bug fixed in the brain's filter.** A weapon skin is now tested only against the Weapons tab and a
    cosmetic only against the Cosmetics tab.
    - Before, a weapon skin (type 0) failed the Cosmetics keys, and a cosmetic (weapon 0) failed the
      Weapons keys.
    - So any Filter or Sort with the popup's default all-ticked boxes answered nothing.
- **List / Quick Sell** (hub:3717-3754, 5212-5230).
  - `listItem` refuses noSale and opensea items.
  - `confQSPopup` shows the item's quick-sell KR and Junk (market_qs_value), or "This item has no value" and
    Destroy for noQuickSell, then NO at 15%.
  - `sellConfirmed(skin, mode, uid)` sends `qsi`.
- **Tests.**
  - hub_creator_e2e (`market_cards`), 140 checks in all:
    - the inventory card;
    - the List and Quick Sell popups;
    - Only Show Missing's read-only cards;
    - Stats' In Circulation;
    - the Black Market's 36 cards, `blackMDone` once an item is owned, and its Purchase popup;
    - the item page;
    - a typ 8 table and a typ 9 giveaway in the feed.
  - economy_hub_e2e opens the popups by clicking the cards.

Not compared this round. These were ported in earlier rounds and not re-read line by line here:
- the leaderboards;
- the Games and Mods lists and pages;
- the clan page;
- the profile's header and its other tabs;
- the trading window and viewOffer;
- chat and friends;
- the settings popups.

Still different:
- Other UIs keep their own copy of the limited-tag colours, without Battle Pass or Premium Spin:
  - account_ui.c, store_ui.c, battlepass_ui.c, hud_ui.c;
  - v10_rewards.c, v10_wallet.c, v10_competitive.c, v10_junkyard.c, v10_menu.c;
  - shared/src/triggers.c.

  They could share `ui_loadout_tag_color`.
- Nothing in KN posts a game result (typ 8). The hub draws one when the feed holds it.

## Hub round 8, 2026-11-23

The sources are the same as in round 7: hub-10.0.7.js, config-10.0.7.js and classes-10.0.7.js. The game bundle
is now on disk too, as deob-10.0.7.js (`~/jails/kru/src/Krunker_v10.0.7/deob/game.deob.js`).

### Limited-tag colours: round 7's list was wrong

Round 7 said nine UIs and triggers.c keep their own limited-tag colour tables. They do not: none of them draws a
limited tag (store_ui.c's colours are the KR packs' sale tags). The tag-coloured views, ui_loadout.c and the hub
(social_ui.c), already share `ui_loadout_tag_name` / `_color`, Battle Pass `#00BCD4` and Premium Spin `#fc038d`
included. v10_menu.c has no table either, so it is unchanged.

The real gap was the svelte item views. v10 draws a `limT` badge there in a fixed `#41c300`, not in the
tag's own colour.
- **v10_junkyard.c `item_view`** (deob-10.0.7.js:106727-106746) now does what v10 does. It picks the first
  that applies:
  - Black Market;
  - the `limT` name in `#41c300`;
  - Vaulted, for an item from another season, or a limited, noSpin item that is not in the Item Shop;
  - Limited, for a limited or blocked item.
- **Still open: v10_market.c.** Its item view draws the same badge without the `limT` branch (its line 3160). Its
  category filter keeps its own copy of the tag names (line 576), which is complete. That file belongs to another
  agent, so it is not changed here.

### Game-result posts (typ 8)

**When v10 posts one.** v10 never posts a typ 8 on its own. The end screen's table has a Share button
(deob-10.0.7.js:226776). Custom games have no such button. The button's popup takes a note and sends
`SHARE_MATCH_RESULT` = `["smr", note]` (deob-10.0.7.js:227055-227064). Krunker's server, which is not
shipped, then posts a MatchResult (typ 8, deob-10.0.7.js:26213).
- The post belongs to the player who shared.
- The hub draws `lnk` as the mode, `lnk2` as the end table's flat rows, and `txt` as the note
  (hub-10.0.7.js:4913-4936).

**How Box Office does it.**
- **Game server** (server/src/accounts.c). The final commit (`acc_commit`, final) now carries `ed`. This is
  v10's `endData.ed` (deob-10.0.7.js:195285-195292):
  - one value per `endStats` column (the mode's, or config's default `sid, name, score, kills, deaths,
    reward`) for each player, in the end order;
  - `name` is `[name]`;
  - the other columns go through `endForm` (deob-10.0.7.js:18315-18353). For example, reward is
    `<span style='color:#F8C55C'>+N</span> KR` or `NONE`, and time is the timer or `DNF`.
- **Brain.** `internal_ws.c handle_commit` saves a final, non-custom commit's `ed` with its mode in the new
  `match_ends` table (migration 0396, all three drivers), once per match (`P_MATCH_END_SAVE`).
- **The post.** `smr` (`sh_share_match` → `P_SHARE_MATCH`, db_social_v10.c) posts the sharer's latest
  finished match whose table was kept in the last 30 minutes, as `type 8, link = mode, link2 = ed, body =
  note`. The hub's post rules apply: level 10, the post length, and the `spost` rate limit. With no such
  match, nothing is posted, because v10's client expects no reply.

**Tests.**
- online_mode_e2e checks that a match's commit leaves its `ed` in `match_ends`. It now opens the config named
  by `KN_TEST_CONFIG` and migrates the worktree's own migrations.
- hub_creator_e2e checks `share_match` end to end: the post's table, ranks, reward markup and note in the feed.

### Re-compare of the sections round 7 skipped

Each of these was compared line by line against hub-10.0.7.js, and every gap found was ported.

- **Games / Mods lists** (hub:6100-6560).
  - The category tiles call `windows[0].switchTab(7)` only. That opens the hidden New & Popular list, as v10's
    bug does.
  - The Editor / Sandbox tabs are gone.
  - Tab 0 groups the cards by category: maps after the shuffle, and mods too.
  - Load More shows on every paged tab.
  - The thumbnails use `inf` (maps) and `minf` / `inf` / `mod_info` (mods).
  - Live player counts come from the coordinator's game list (`updatePlayers`, :6107-6131).
  - The featured cards use v10's `fetMapVids` / `fetModVids` (a video link, or "Missing Preview").
  - The ~Revenue box and the season leader buttons are removed.
  - Share copies `origin/?play=` or `?mod=`.
  - `hostMap` hands the map to the menu through storage `mapToLoad`: the game page opens Host Game with it
    (`app_mode.c` → `ui_host_map_select`).
  - Tweet uses v10's text.
- **Map page** (hub:3150-3200, 5988-6029). The Servers tab lists the map's custom games from `/mm/game-list`
  (`fetchGameList`). Starred games come first, then games by player count. The list shows region and
  players, or "No active servers", and `#totalOnline` shows the total. The Versions tab is kept (see below).
- **Clan page.** The Clan Discord button needs `clan_link4`, which kn_brain's `clan_page` did not send. It
  is now the clan's `clan_discord`, from a C query that runs on every driver (0104's pg `kn_clan_page` lacks
  it).
- **Profile header and tabs** (hub:2760-2960).
  - `#socialFlag` is `flag-icon flag-icon-<code>`.
  - The presence dot has no title.
  - The creator badge has v10's markup.
  - The six stat tabs are General / Killstreaks / Other / Class Stats / Raids / Match History, replacing
    KN's Season tab:
    - Class Stats skips hidden classes.
    - Raids sums `t.raidStats` and has a card per raid map. kn_brain's profile now sends `raidStats` from
      `raid_stats`, in both the C and pg queries, and GET /players sends `raid_stats`.
    - Match History has v10's svelte-1irqx9a list, with an entry's scoreboard and sorting.
  - `creator_has` matches a creator exactly.
  - `?autoReport`, `?autoGift` and `?autoTrade` behave as v10's.
  - The sales total is comma-separated.
  - The login's page updates run when the page was drawn signed out (hub:2173-2216). They cover:
    - the staff's chat-ban fetch and `grp`;
    - the Trade button (`tradeMinLvl` on both sides) and `?autoTrade`;
    - Follow;
    - `?autoGift` and `?openPubMo`;
    - the `.canX` staff gates.
- **Trading and viewOffer** (hub:2640-2720, 4960-5120).
  - An offer row skips free items, shows KR with `format_num`, and adds painted copies.
  - A removed offer updates its column's count, or shows "None".
  - Trade items have the right-click `openSales` and the tooltip. removeItem cancels the tooltip.
  - searchItems uses v10's whole predicate.
  - showMissing only hides shared items.
  - viewOffer draws KR as an `oCurrencyLine` before Est. Value.
  - confOffer's NO refreshes the popup.
  - shareProfile has no popup.
- **Chat and friends** (hub:2200-2340, 5280-5350, 6930).
  - The Friends tooltip reads "view friends list".
  - The friends list is kept only when an answer has rows. removeFriend splices it.
  - A chat line's `tim` is an age.
  - A message is appended only while `#chatBox` shows that chat, under startChat's name.
  - `openURL(url, self)` honours `_self`.
  - `instaFollo` spends its button (`follBtnGivD` / `disFolB`).
  - The logout clears the list.
  - follow counts use `format_num`, and the labels go through i18n.
- **Settings popup** (hub:1754-1984).
  - The names are i18n keys (`settings.social.region` ... `settings.social.bg`).
  - "Manage Ad Preferences" is the last row. `openOneTrust` does nothing without OneTrust.
  - URL values are no longer cut at 64 / 256 bytes.
  - The background takes `isURL` (config-10.0.7.js:4102-4108, run by hand in `image_collection_is_url`), plus
    the page's own `img/` paths. That way the built-in background, `img/backgrounds/0.png`, now applies:
    before, the http(s)-only check refused it.
- **Image collection** (hub:6335-6440; the game's own copy is deob-10.0.7.js:214688-214775, with the same
  markup). image_collection.c now draws v10's markup and wording on both the menu and the hub:
  - Image Collection / Add Image;
  - Backgrounds / Overlays / Scopes / Reticles / Icons;
  - Select / Delete tiles marked Custom / Default;
  - the built-in "Midnight Snow";
  - the New Image form.

  The behaviour also matches v10:
  - The handlers carry the setting's key. Select gets the URL.
  - After an add or a delete, tab 0 is shown.
  - Errors use the response popup: Something Went Wrong / Invalid Name / Invalid Image.
  - The extension test is case-sensitive, and the URL must pass isURL.
  - Stored rows are kept as stored. A bad link draws an empty image.

  The KN-only `.kn-image-*` CSS is removed. The uidiff baselines `kn-image-collection[-new]` were re-rendered
  inside the popups' boxes (tools/uidiff/README.md).

**Tests.**
- hub_creator_e2e has new checks for: the six stat tabs and the Raids sums; Match History's empty text; the
  settings rows and Manage Ad Preferences; the image collection (empty Add, a new image, Select on Midnight
  Snow); and the clan page's Discord link.
- image_collection_test covers isURL, the v10 markup, the error popups, and stored rows.

**Kept on purpose (KN additions).**
- The map page's Versions tab (aaf774e7, with its own e2e).
- The developer-only creator button.
- The native `clear:both` divs.

**Still open.**
- **Match History has no data.** No server sends `matchHistory`, so the tab shows "No match history
  available." Recording it needs work in ranked.c, and ranked_ui.c belongs to another agent.
- **v10_market.c's `limT` badge** (another agent's file).
- **The profile's `sm`.** It reads "In game", not v10's "playing <map>".
- **The gift popup's prefill.** v10 prefills "I like your <map> mod/map" from a creation's Gift. Not ported.
- **createTip's vertical position** on trade items is not checked against v10.
