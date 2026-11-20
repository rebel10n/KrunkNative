# Krunker v10.0.7: the target

Krunker Box Office plays **Krunker v10.0.7**, the latest v10 build. It is the target of the
client, the server, the ruleset identity and the UI work (set by the user, 2026-11-19: "update
KrunkNative to the latest version of the game"; v10.0.6 was the target from 2026-10-30, v10.0.5 from
2026-10-29, v10.0.4 from 2026-10-28, v10.0.3 from 2026-10-21).

## Reference bundles

- **Target: v10.0.7. Read `~/jails/kru/src/Krunker_v10.0.7/deob/game.deob.js`.** It is kru's
  decoded copy of the live bundle (`/home/kru/krunkbox/bin/game.deob.js` on kru, mtime 2026-11-16
  19:18 UTC; the page build tag `5fHFc` matched krunker.io on 2026-11-19), byte for byte; its
  `README.md` gives the sha256. Strings are inlined and the file is pretty-printed; identifiers stay
  mangled. Cite it as `deob-10.0.7.js:<line>`. Its version literal is `"10.0.7"` (:16392).
  `deob/game.webcrack.js` is the same build's `game.min.js` through our own decoder (below), a
  cross-check; `../live/` keeps kru's raw artifacts (game.min.js, game.debug.js, index.html,
  skins.jspck) and `../versions.txt` the changelog as fetched. `https://kru.eli.gift/game.deob.js`,
  where earlier builds were fetched, no longer resolves (2026-11-19).
- **v10.0.6: history.** `~/jails/kru/src/Krunker_v10.0.6/deob/game.deob.js` (last-modified
  2026-10-30 16:40 UTC), decoded the same way; the code ported 2026-10-30..11-18 cites it as
  `deob-10.0.6.js:<line>`.
- **v10.0.5: history.** `~/jails/kru/src/Krunker_v10.0.5/deob/` (last-modified 2026-10-29 16:40 UTC),
  decoded the same way; the code ported 2026-10-29..30 cites it as `deob-10.0.5.js:<line>`. Its
  `tools/normalize.py` rewrites the build-to-build noise (mangled names, quote styles, `\xNN` escapes,
  hex numbers) and keeps the line layout, so `diff -a` of two normalised builds lists the real
  changes (10.0.6 to 10.0.7: 523 hunks, 10.0.5 to 10.0.6: 59, below).
- **v10.0.4: history.** `~/jails/kru/src/Krunker_v10.0.4/deob/game.deob.js`, decoded the same way
  (last-modified 2026-10-28 02:37 UTC). The code ported 2026-10-28..29 cites it as
  `deob-10.0.4.js:<line>`. Both builds mangle names and escape strings differently, so compare them
  after normalising both (mangled names, quote styles, `\xNN` escapes, hex numbers): the two files
  then differ in 62 places (below).
- **v10.0.3: history.** `~/jails/kru/src/Krunker_v10.0.3/deob/` (read only; its `README.md` lists
  every file). `deob/game.deob.js` is 10.0.3's `game.js` through webcrack (~263k lines); the code
  ported before 2026-10-28 cites it as `deob-10.0.3.js:<line>` (or `deob:<line>`). Those citations
  stay valid for what they describe; re-check the value against 10.0.4 when the code is touched.
  New extracted tables may be added under `deob/tables/` and listed in that README.
- **Tools and tables.** `Krunker_v10.0.3/deob/tools/deob.cjs` is the decoder (webcrack with a
  `node:vm` sandbox that only evaluates the bundle's string-array decoder; `cd deob/tools && npm
  install --ignore-scripts`). `deob/tools/extract.js` dumps a bundle's string / number / key sets
  (version diffs); `deob/tools/extractors/` are the table dumpers. `deob/tables/` holds the 10.0.3
  weapon, class, mode, prefab, packet and config tables and their diffs against KN's;
  `deob/audit-2026-10-24/` is the per-area parity audit of 10.0.3.
- **v10.0.2: history only.** `Krunker_v10.0.3/deob/v10.0.2/game.js` (the 2026-10-13 bundle,
  formerly `~/Downloads/v10.0.2.js`) and `deob/v10.0.2/game.deob.js`. It explains what 10.0.3
  changed; no value is taken from it. The older `v10:N` / `v10.js:N` citations are lines of
  `deob/v10.0.2/game.js` (they were taken from an identifier-renamed copy with the same line layout;
  its `_N` names are not in the file).
- **Older context.** Krunker v7.1.4 (`/home/user/jails/kru/src/Krunker_v7.1.4`, specs in
  `doc/v7/`) explains the code v10 kept; v3.1.1 and v7.1.4 map JSON must still load.

## Source policy

Every value and rule is checked against the target bundle directly (user, 2026-10-24, once the whole
bundle was decoded): weapon, class and mode stats, physics, map fields, object flags, packet shapes
and UI. There is no "unchanged since" fallback. A ported value cites its `deob-10.0.7.js:<line>` in
a code comment (code ported before 2026-11-19: `deob-10.0.6.js`, before 2026-10-30: `deob-10.0.5.js` /
`deob-10.0.4.js`, before 2026-10-28: `deob-10.0.3.js`; re-check those against 10.0.7 when the code is
touched); a deliberate difference from the target is documented in code. A comment or spec that
cites only v10.0.2 (`v10:N`) or v7.1.4 is not verified yet: re-check it against the target bundle when
it is touched.

## 10.0.7 (2026-11-16)

- Build: `clientVersion` `10.0.7r1`; `buildVersion` `7ccZCb5K8H21WeQNwr1szD2woJ2CP1WI` (deob-10.0.7.js:19598,
  :20148); the ahNum key is `0x43e1`, so the step is `0xe1` (:98877, the add helper :24246; 10.0.6: `0x25`).
  The packet tables (c2s 138 / s2c 264), the cheat check (salt `KRUNK3R_AC`, FNV double) and the per-player
  layouts are unchanged (`make krunker-build`); config.networkSounds is the same 401 names, now at :18260.
  The page's build tag is `5fHFc` (:204288, :230107; 10.0.6: `1qcEh`).
- The normalised diff has 523 hunks. 313 are transpiler noise: 10.0.7 is emitted with native spread
  (`f(...a)`, `[...a]`) and object-spread helpers instead of `apply` / `concat` / `Object.assign`, plus
  optional-chain rewrites and renamed mangled fields (`Wamjfa` -> `W1lotf`, `Mss3ih` -> `Bum0tq`). The rest,
  grouped by the changelog (krunker.io/docs/versions.txt, "UPDATE 10.0.7"), follows; the KrunkNative
  code that ports an item cites its lines.

### General
- "Reduced the stutter when an enemy first comes on screen": a player added that is not you is
  prewarmed (:174280, :174456 `prewarm(objInstances)`); prewarm (:207778-207798) queues the object,
  compileAsync runs for it while its geometry / textures are still loading (prewarmAll, prewarmSince,
  10000 ms cap), the queue drains inside a per-frame time budget (:206033-206067); textures count as
  pending per material (`userData.texPending`, :206177-206178, :206240, :206360-206400) and texture
  uploads are queued while `uploadBudget` is on (:206223-206228, set when warm-up ends :208124, flushed
  :208072).
- "Fixed lag switching letting players teleport": NOT in game.js beyond two new player fields,
  `inputBank` / `inputBankAt` (:171267-171268, reset :171545). The rule is server-only: KN's server
  must bound how much input time a player can bank after a silence (design it, document as intentional).
- "players sometimes kicked with Input Failed after a server hiccup": the client's `f` handler
  (input reset, :227210-227211; registered at :229495) no longer prints "Input Failed <n>" for the new
  reason 8 (6 and 7 were already silent). Server side: a hiccup sends reason 8 (resync) instead of
  failing the player.
- "KR not updating in game after market, trade and giveaway actions": the account store's funds /
  skins / junk are updated in place instead of copied (:107782, :111531, :132203-132206, :132756-132759,
  :132976-132977, :137208, :137716, :147156-147158); a giveaway post's `funds` updates the store
  (:142216-142234).
- "Junk not being added when bought on newer accounts": `junk = parseFloat(player_jnk) || 0`
  (:115130; player_jnk arrives as a string); profile Junk `(parseFloat(junk) || 0).toFixed(2)` (:218993).
- "Gun Game giving two Promotion medals for one promotion on multi-kill shots": isGunGamePromotion
  (:193169-193171): only a kill with the current loadout[0] weapon (or no wId) promotes; the promotion
  and the Promotion medal both check it (:172641, :193173-193188).
- "moving map objects like elevators jittering from the second round on": finishInit resets the
  entityClock with the rest of the round state (:193742; the clock :190779).
- "Random secondary skin always using the default skin after a reload":
  `parseInt(getSavedVal("secondaryInd") || 2)` (:217276; the saved value is a string).
- "Ametwista skins showing black" / "some textures rendering wrong with WebGPU": the crystal skin
  node graph (:196569-199154) drops its worley / split / noise3 nodes for hand-built cell hashing
  (c7_f1 / c7_f2 / c7_id from kBig / kOff / kHash / kSpread), two range params widened (0 -> 15,
  0 -> -15 at :196569, :196582) and modelScale becomes an `objScale` float param (0.0005..20, default
  0.00229, :196778-196785). Check whether KN renders this graph at all.
- "guests getting an error when clicking Double XP on the end screen": `lastDoubleAttempt` only
  when there is an account (:229879).
- "Closing the daily spin wheel now returns to the menu": the spin close goes back to the window
  that was open only when there was one (:224295, :224510 `V && showWindow(V)`, was showWindow(14));
  a KR spin no longer sends AD_FINISHED from the spin path (:224287, :224477-224506).
- "profiles of players with some special characters": a path-segment encoder (:26198-26202:
  `.`, `..` and `search` percent-encoded, else encodeURIComponent) for /players/<name>/* ,
  /moderation/chat/<name>, /studios/<id>/members/<name> (:27209-27568).
- Not located in game.js (backend / matchmaker): "items sometimes being unusable without a game
  reload", "double raid drops sometimes being lost".

### Ranked
- "ranked matches sometimes being saved twice": a forfeit after the match reached POST_MATCH does
  nothing (prepareForfeit :194906-194916; the vote handler :194969-194995 only clears the vote then),
  so endGame cannot run twice; new string commands.forfeit.over "The match is already over".
- "Anonymous players now stay anonymous on the team panel": the lobby payload
  (lobbyPayload / lobbyPayloadAll, :190976-190995) drops `an`, shows `an` as the name unless ranked
  names are on, and zeroes i / v / cl / l and sets f -1 for anonymous players; SYNC_P_CON_DATA /
  SYNC_P_CON_OBJ send it (:191028, :195796). The players.add data hides four more fields of anonymous
  players from others (:176753).
- Not in game.js (backend / matchmaker / shop data): one cooldown penalty per match, NO GAMES
  AVAILABLE on joining as a match starts, the Ranked Shop's Universal Spin Ticket.

### Inventory & Market
- "exchange and social windows not fitting with UI Scaling off": exchange sidebar max-height
  (:133915-133919, svelte-1i76mhh). "market tabs going off screen at high UI Scale": not in the
  bundle's svelte CSS; check the live css/*.css (build 5fHFc) against ui/css.
- "Trade item search now matches weapon names": the item filter also matches the item's weapon
  name (:111394).
- Not located in game.js: Hub inventory listing / unlist-relist fixes (backend).

### Social
- "first DM from some friends showing as DM from Unknown": a message from someone not in the list
  (10.0.6 :141342-141364) plus the new `msg:echo` (:141564-141585): your own message sent from another
  client (an OAuth app) is added to the thread as sent; messages carry `via` (:141519-141520,
  :141652-141653), shown as "via {0}" in the bubble (:142037-142045, :142780-142786) and the in-game
  chat line "[DM from X via app]" (:228062-228063).
- "chat draft being cleared when a message arrives": per-friend drafts in a Map, restored on open
  and saved on close (:142053-142054, :142508-142521).
- "clan feed tab erroring for guests and players without a clan": the clan data path only
  requests (sendReq 3) when there is a clan_id (:220509).
- Not located: clan logo upload right after creating a clan (backend).

### Maps
- "Favoriting a map now shows it in the Favorites tab" / "Favorite stars now show correctly on every
  tab": per-account favourite id sets for maps and mods (:228176-228217), toggling one updates the set
  and drops the cached Favorites list (:228237-228244), stars recomputed on every tab
  (:223451-223452); the list fill only writes into the tab that is showing (:218783-218795 mods,
  :220328-220339 maps).

### Twitch
- "If your Twitch link has expired, claiming drops now asks you to reconnect it": claim with an
  expired link opens a popup (twitch.reconnect / .confirm / .decline, :218914-218927; popup keys
  :97248-97268), confirm reconnects with `force_verify=true` (:229550); the OAuth return strips
  twitch_auth / code / scope / state / error params from the URL and sends VERIFY_TWITCH once
  (:216806-216878); the success toast / window 5 only when canShowTwitch (:229146, :229271-229282).

### Editor
- Not in game.js (the editor bundle): duplicated object's asset ID -> collision size, retrying a
  failed custom asset upload.

### KrunkScript
- "Script-spawned AIs now see 120 units by default (visionDis)": visDis default 120, was 0 (:187284).
- "AI part names now work the same in glowPart, getPartPosition and setPartDamage, and can contain
  spaces and colons": one part-name key (:181588-181590: whitespace -> `_`, strip `[ ] . : /`) used for
  the node map (:181930-181933), partDmg (:182571-182574), glow / position lookups (:182593, :182608,
  :182707, :182724, :182805); the KS name check allows `: / [ ]` and space (:185581).
- "getPartPosition now works for AIs spawned without preciseHits": the model is loaded by assetID
  when none is attached (:182723-182724).
- "AI glow keeps its color and its halo looks smoother": glow keeps the base colour (color *=
  1 - 0.75 g, emissive default #ff5a1e, intensity ei + g^2, :182865-182872), halo radius from the
  box, pulled toward the camera, opacity 0.8 g^2 (:182883-182897), the halo sprite a 128 px DataTexture
  with a pow 2.2 falloff and mipmaps instead of a 64 px canvas gradient (:182249-182264).

### Login with Krunker
- "Login with Krunker" / "Connected apps": the account API's oauth section (:27656-27696:
  authorize/info, authorize, /account/apps, revoke, actions/<id> approve (twofa_code) / deny,
  activity), Settings' Connected apps section (:97493-97673 component, mounted :219140-219142, header
  :219174), 87 new en strings (oauth.*, settings.apps.*) in :204284. The consent page itself is not in
  game.js.

### Not in the changelog (game differences)
- Discord unlink answers DISCORD_DELETION_PENDING -> "deletion_pending" state with
  discord.result.deletion_pending (:97384).
- Discord items: rarity colour "Discord" #5865f2 (:103481); an item with `discord` is
  noSale / noSpin / limited / noQuickSell with limT "Discord" (:103761).
- JNK amounts show exactly two decimals (toFixed(2) without parseFloat: :147275, :147640, :147994,
  :148158, profile :218993).
- windows[9] (maps) ignores an empty playerData object (:220294).
- DM `via` / msg:echo (above).
- config: beta hosts (beta.krunker.io / *.beta.krunker.io) switch assetsURL / userAssetsURL;
  new config keys assetsURL, userAssetsURL, badgePermIds (default [47707163], was a constant :115065),
  betaStorage, restrictBypassHosts (:16428-16438, :19574-20118; host check :229209-229211); two new beta
  flags krunker_gateway_beta / krunker_matchmaker_beta (:16401); user-asset URLs go through one helper
  (:24286-24300, :124438, :125946, :126152, :126536, :139353). Production hosts unchanged.

### Store data (gapi `/data/skins` and `/data/spins`, not the bundle; pinned 2026-11-19)
- Skins: still 8153 items (sha256 `f9694fa9...`, the same bytes as kru's Nov 16 `skins.jspck`); six changed:
  paints 7831-7834 move to season 9, Eterno (8003) drops `mScl` (the weapon's own scale applies, `mScl ||
  scale`, :174199; KN: player_mesh.c, prefab.c), and the Ametwista knife (8078) gets
  `customShaderInfo["*"].shaderArgs.params` {stockX -6.1, muzzleX 7.9, objScale 5.2, bottomY -6.2, topY 1.5}
  over the graph's defaults (objScale default 0.00229, range 0.0005-20, :196777-196785). KN already
  applies `shaderArgs.params` (skin_fx.c custom_graph_params); with the old data the knife rendered
  almost solid black, with these params it shows the crystal cells.
- Spins (sha256 `91089724...`): the 14 wheels now draw from six new `nov26Pool*` pools (Uncommon to
  Contraband, the November spin items); the `s10Pool*` pools stay in the data (49 pools). KN's Wild Offer
  follows the Heroic wheel's pools by label instead of naming the s10 pools (services/brain/wild_offer.c).
- Maps: the official list and all 47 maps are byte-identical to `tools/krunker/maps.lock.json`.
- Assets: every file of the 72 nov26 items (102 required) and every other referenced path (21540,
  `make assets-sync --list-only`) is in assets/ and served by dl.krunk.io. Five files Krunker replaced in
  place (body_328/330.obj, melee_198/200.glb, hat_527.png) were already current; their pins in
  live_assets_v10.lock were re-taken.

### Other gapi data (read 2026-11-19; pinned by `make live-data-fetch`, checked offline by `make live-data-verify` in `make check`)
The 10.0.7 client's gapi client (`new iiíïíiï(GAPI_URI)`, :27773; requests through `iïîiíîï`, :26152) asks for
these public, account-free routes; everything else it calls carries an Authorization header or is live state
Box Office keeps itself.
- `/shop/displays?tag=limited` (the Eterno Shop, :26627, :129566-129580; store.catalog :220848): one product
  more than 0354's copy of 2026-10-22, v10.0.5's "1 new item to the Eterno Shop": Deathweaver (product 47,
  skin 8080, USD 10, one per account) as the first node, every other node one place down. Migration
  `0381_eterno_deathweaver` (all three dialects). As a USD product Box Office stores it but does not list it
  (shop.c lists only what it can sell).
- `/titles` (:27650): 113 titles, two more than assets/titles.json: 10.0.4's Candy Heist titles
  Trick-or-Treater (112) and Candy Heist Master (113), with their `color`. assets/titles.json is now the live
  list. Box Office's Candy Heist (shared/src/candy_heist.c) does not grant them; the event ended 2026-11-07.
- `/shop/displays?tag=premium` (:128638, :220837): krunker.io's Premium monthly subscription (USD 4.99). Not
  copied: Box Office sells no subscription, and the premium shop component is unchanged in 10.0.7 (KN's
  v10_misc_popups.c "premium shop").
- `/maps`, `/maps/<i>`: unchanged (fetch_maps.py). `/data/skins`, `/data/spins`, `/spins/wheels`: above.
- Live state, not data (Box Office keeps its own): `/territorial-wars/status`, `/leaderboards/krunkcup/teams`,
  `/creators/events`, `/social/news`, social's `/player-count`.
- Need an account (401 without one; not fetched): `/ranked/shop` (KN: 0205 + 0371), `/market/black-market`
  and its `/rank`, `/kpd/reviewers`, community's `/polls`; Twitch drops come over the social socket per
  account (`getTwitchDrops`, :98486; Box Office runs no drop campaigns).

### Not ported (no KN counterpart)
- The logger stops printing to the console (:28569-28602).
- three.js / TSL node internals (WebGPU), analytics / gtag rewrites, the transpiler noise above.

## 10.0.6 (2026-10-30)

- Build: `clientVersion` `10.0.6r1` (deob-10.0.6.js:16403-16404); `buildVersion`
  `BMXeeCoe7d0j5zNh4KE3IsWuyP5twK9g`, the ahNum step `0x25`, the packet tables, the cheat check and the
  per-player layouts are unchanged from 10.0.5 (`make krunker-build`). config.networkSounds is the same
  401 names (:18263). The page's build tag is `1qcEh` (the `i18n/*.js` and `maps/*.json` `?v=`, :202628,
  :228306; 10.0.5: `UVmCA`).
- Every change the normalised diff shows:
  - Store: a purchase's `purchase_count` update builds new shop / child / product objects and sets the
    shop again (:115985-116026), so a limited item shows sold out right after it is bought; the Stripe
    claim's `alreadyClaimed` answer updates the display's count and shows the success text instead of
    granting again (:213769-213790): "limited items sometimes being purchasable more than once".
  - Wear on cosmetics: a typed (non-weapon) item with `wear` gets a `<type path><id>_wear` map
    (:173413-173419, and the inspect preview :222884-222893), the "wear and paint on some cosmetics
    from November spin".
  - Hub messages: a message from someone not in the list adds them online with the unread count and the
    last message (:141342-141364), opening a conversation clears its unread count (:141569-141572), and
    the friend list no longer pushes a friend twice (:141258-141265).
  - Not ported (no KN counterpart): three.js node materials pinning their references in `buildAsync`
    (:195832-195854, WebGPU), the editor asset's `fileExt` rewrite (:125150, same behaviour),
    restructured analytics / gtag calls, renamed mangled fields.
- Store data (gapi `/data/skins`, not the bundle): the new skin Scarecrow Peg.
- Changelog (krunker.io/docs/versions.txt, "UPDATE 10.0.6"): wear and paint on some November spin
  cosmetics, the new skin Scarecrow Peg, limited items shown sold out right after buying, and limited
  items no longer purchasable more than once.

## 10.0.5 (2026-10-29)

- Build: `clientVersion` `10.0.5r1` (deob-10.0.5.js:16403-16404), `buildVersion`
  `BMXeeCoe7d0j5zNh4KE3IsWuyP5twK9g` (:19598), ahNum step `0x25` (:98685; 10.0.4: `0x2b`). The packet
  tables, the cheat check and the per-player layouts are unchanged (`make krunker-build`); the
  krunker-build workflow had already published the document to `dl.krunk.io` (2026-10-29 16:52 UTC).
- Every change the normalised diff shows:
  - config.networkSounds gains `weapon_4_37` and `weapon_2_37` (:18263, 401 names) and SOUND.soundCats
    lists both at category 3 (:198862-198863); the clips are pinned in `live_assets_v10.lock`.
  - getClanCol's gold staff clans gain `nv` (:103946).
  - players.clear() and players.remove(sid) drop the pending poses (:174025, :174098): a later player
    with the same sid no longer takes over a pose (the changelog's "holding their arm like the Candy
    Heist pose"). KN already forgot them on INIT; `player_pose_forget` does it on LEAVE.
  - Game.init drops mainScript / sharedScript on a map without customs data (:193594). KN already
    unloads on a script-hash change (a map without scripts hashes 0; `ks_server_round_init`) and
    the client unloads every round.
  - The editor's COLLISION_MESH prefab and generateCollisionMesh (:158847-158862, :159520-159534).
  - Custom skin shader 18 "Ametwista", the first one stored as a node graph (`"type": "graph"`,
    :196197-197428; its serialisation :197431-197438).
  - Not ported (no KN counterpart): the WebGPU `isUnfilterable` cache (:195781-195787), the FBX
    binary magic fix (:151282; KN reads FBX through ufbx), the skin-reward callback's restructure
    (:194207-194233, same behaviour), the renamed mangled fields and the `?v=` asset hashes.
- Store data (not in the bundle: gapi `/data/skins` and `/data/spins`, 2026-10-30): 76 new items
  (8152), among them the 4 Ametwista Twitch drops (shader 18), Deathweaver (Item Shop) and the
  November spin items, and one new item pool (`s10UltraPoolRare`, 2 items, on the Ultra wheel). The
  November items are in no pool yet: Krunker's spins change on November 1, so `make store-import` has to
  run again with that day's `/data/spins`. `make store-import` regenerated `assets/skins.json`,
  `viewer_store_table.c` and `store_spins_table.c`; their 101 asset files are pinned in
  `live_assets_v10.lock`.
- Changelog (krunker.io/docs/versions.txt, "UPDATE 10.0.5"): 71 November spin items (from November
  1), 4 Twitch drops, 1 Eterno Shop item, the Candy Heist arm pose fix, WebGPU map textures, faster
  trade history, a quick-sell fix, and the editor's Collision Size field for custom assets, Convert
  to Collision Mesh, selectable collision triangles on imported maps and collision kept through
  import / export.

## 10.0.4 (2026-10-28)

- Build: `clientVersion` `10.0.4r1` (deob-10.0.4.js:16403-16404), `buildVersion`
  `DV6B3wX1sg41pVR92UCbVWEpILgvBHbR` (:19598), ahNum step `0x2b` (:98686; 10.0.3: `0x3`).
  `shared/include/krunker_build.h` and `tools/krunker/build.json` are regenerated from it
  (`make krunker-build`); official joins take the published copy (`dl.krunk.io`, krunker_build_fetch.h).
- Wire: one new server-to-client packet, `SKIN_STAT` `"sks"` (skin id, stat, value; :184311). The
  10.0.4 client has no handler for it and neither does KN (unknown packets are counted and dropped,
  `krunker_link.c`). The cheat check, the per-player layouts ("0" 53 values, "k", "l") and the
  client-to-server packets are unchanged. config.networkSounds is the same 399 names (:18263).
- Changelog (krunker.io/docs/versions.txt, "UPDATE 10.0.4"):
  - Candy Heist: the Halloween event until November 7 (collect candy, bank it at the Cauldron, avoid
    the Hollowmen; mode 37, map 46), its challenges with KR rewards, titles, event drops and the
    Lil' Cauldron pet.
  - General: a map loading screen, less stutter when a match starts, an Interact button on mobile, a
    Privacy & Age section in profile Settings, item drops equippable without relogging,
    chat-restricted players can vote to forfeit, matches played alone end with ENDED instead of
    DEFEAT, and skin / waist / shoe / Shotgun preview and map-replay fixes.
  - Inventory & Market: item locks (against quick-sell, listing and trades), Select mode to
    quick-sell or list many items at once, Buy multiple on listings, the "Market is busy" and
    listed-paints fixes.
  - Raids: bosses no longer drop nothing; Bastion's Hydra and Catacombs give double drops on double
    drop weekends.
  - KrunkScript & Editor: AI Rarity Chances, `doubleRaidDrops` in `GAME.CONFIG.getMatch()`, studios
    can publish / unpublish / edit currencies, setting a score counts only the difference toward
    score challenges, a better third-person arm for `player.pose.holdL`.
- Real-money checkout (doc/v7/decisions.md): C drives the purchase (the game socket's `xst` /
  `xpurchase` / `xsin`, FRVR's iap requests); the payment provider's own hosted page opens in the
  system browser, never krunker.io's page or the JS game. The provider credits the account; the
  client waits for the server's answer or a funds increase (client/src/ui/kio_checkout.c,
  kio_checkout_stripe.c; doc/v10/checkout-browser.md, doc/v10/checkout-stripe.md). Tested against
  fakes only: no live payment has gone through it yet.

## KrunkNative additions

Some things are KrunkNative's own, added on top of v10 on purpose. They are mods, not parity
gaps, and audits leave them out. One is the Nettspend class: its id is always the last class, after
v10's `CLASS_COUNT`. The other is the kept v3.1.1 and v1.5.2-era maps (`311_*`, `152_*`).
`kn-additions.md` lists them, says why they are kept, and says what to migrate if v10's class table
grows. The KN id ranges above Krunker's tables (weapons from 64, windows from 100, `NET_PACKET_EXT`
subtypes) follow AGENTS.md.

## What the target changes

- Identity: the ruleset `version` is `"10.0.7"` (`shared/src/rules/ruleset.c`). The build's client
  version is `10.0.7r1` (`shared/include/krunker_build.h`, `tools/krunker/build.json`). The client
  logs `[client] Krunker v10.0.7` and the server `Plays Krunker v10.0.7`. The Android and iOS
  version fallbacks name 10.0.7.
- What's New shows Krunker's official changelog, `ui/windows/versions.txt` (krunker.io/docs/versions.txt
  as published, led by UPDATE 10.0.7; CI's shipped version, `tools/release/version-meta.sh`). Box Office
  keeps no changelog of its own: the changelog is parity with Krunker's, never reworded (user rule,
  2026-11-19; `kn-versions.txt` removed).

## What it does not change

- The HELLO / INIT ruleset bytes (HELLO bit 2, INIT ruleset 3) and KN's own packet layouts (wire
  compatibility). `changelog-10.0.3.md` lists what 10.0.3 changed; the 10.0.4, 10.0.5, 10.0.6 and 10.0.7 sections
  above list theirs.
- `doc/ref/krunker-v10.0.2/` and `doc/ref/krunker-v10/` are captures of the 10.0.2 page. The
  uidiff scenarios refer to them by path.
- `ui/` is the v10.0.2 page import, edited by hand for parity since. It is regenerated from a newer
  bundle only by a separate `make ui-import` pass with a pixel diff; `ui/windows/versions.txt`
  (Krunker's served changelog) comes from that pass too.
- The filenames in this directory are unchanged. The spec files describe the 10.0.2 bundle unless
  a section says otherwise; `gameplay-1003-audit.md`, `constants-1003.md`, `rules-1003.md` and
  `Krunker_v10.0.3/deob/audit-2026-10-24/` re-check their values against 10.0.3.
  `changelog-10.0.3.md` is the 10.0.2 to 10.0.3 review.
