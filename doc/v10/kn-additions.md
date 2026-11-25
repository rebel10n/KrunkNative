# KrunkNative additions on top of v10

Krunker Box Office plays Krunker v10.0.4 (`target.md`). A few things are KrunkNative's own, added on
top of v10 on purpose. They are mods, not parity gaps: a parity audit compares KN with
`deob-10.0.4.js` and leaves these out. Each one is also marked at its definition in the code.

## The Nettspend class

The Nettspend is not part of v10's class table. v10.0.3's `classConfig` has 16 classes, Triggerman
to Infiltrator (`deob-10.0.3.js:115405-115747`, `deob/tables/cls-1003.json`); that is `CLASS_COUNT`
(16). KrunkNative's classes come after every v10 class (user, 2026-10-24: "the id can be changed to
always be last, it's a mod on top of v10"):

| macro (`shared/include/shared.h`) | value | meaning |
|---|---|---|
| `CLASS_COUNT` | 16 | v10's classes, ids 0-15: menus, challenges, class stats, `classLim0..15` (`GAME_CONFIG_CLASS_LIMITS`) |
| `KN_CLASS_BASE` | `CLASS_COUNT` | the first KrunkNative class id |
| `CLASS_ID_NETTSPEND` | `KN_CLASS_BASE + 0` (16) | the Nettspend, the last class |
| `CLASS_TABLE_COUNT` | `KN_CLASS_BASE + KN_CLASS_COUNT` (17) | the ruleset's table (`rules_class_count()`, `Game.class_count`) |
| `KN_CLASS_NETTSPEND_STORED` | 16 | the id that saved data holds |

This works like `WEAPON_ID_KN_BASE` (64) for the KN weapons, except that the class base follows v10's
table. If v10 adds a class, the Nettspend's id moves up with it and never overlaps a v10 class. The
table is `g_classes_v10` (`shared/src/rules/v10/classes_v10.c`): v10's 16 in order, then
`[CLASS_ID_NETTSPEND] = nettspend`. The code tells KrunkNative's classes from v10's with
`>= KN_CLASS_BASE`: the official-server gate `game_class_allowed`, `game_class_listed`
(`shared/src/game_config.c`), `comp_lobby_class_limit` (no classLim, so the team's size) and the v10 menu
avatar (`client/src/ui/v10_menu.c`). The class design (loadout, gates, comp, bots) is in
`doc/v7/decisions.md` "Nettspend class", and its balance in `gameplay.md` "Nettspend balance".

The id is stored as a number in these places:

| where | what |
|---|---|
| client html storage `classindex` (`client/src/ui/menu_ui.c`) | the menu's class |
| loadout presets `classindex` (`client/src/ui/ui_loadout.c`) | a saved preset's class |
| brain per-class stats `c<id>` (`services/brain/db_accounts.c`) | the account's class XP / stats |
| `match_results.class_id` (`db_accounts.c`) | match history |
| account `class` / `classIndex` (`services/brain/accounts.c`) | the account's last class |
| `mm_ranked_members.class_index` (`services/brain/db_mm.c`) | a ranked queue entry (short-lived) |
| `ui/textures/classes/icon_16.png` (`tools/nettspend/icons.py`) | `icon_index` = the id |
| wire: SPAWN / ENT class_index, comp lobby `c`, GAME_CONFIG `classes` | both ends build from one table |

The id has been 16 since 2026-10-14, and v10.0.4 still has 16 classes (Triggerman to Infiltrator,
`deob-10.0.4.js:102448`), so nothing is migrated today.
If v10's table grows, `CLASS_ID_NETTSPEND` no longer equals `KN_CLASS_NETTSPEND_STORED` and a
`_Static_assert` in `classes_v10.c` stops the build. To fix it, migrate the stored rows and
settings (16 becomes the new id; a stored 16 then means the new v10 class), rename the icon, and
update `KN_CLASS_NETTSPEND_STORED`.

Tests: `ruleset_test`, `nettspend_test` and `class_spawn_test` check that the Nettspend is the last
class at `KN_CLASS_BASE` and that the stored id still resolves to it. `config_test` checks that
GAME_CONFIG `classes` accepts it and refuses the id one past it. The class table comparison with
Krunker's (`weapons_v10_test`, against the v10.0.3 fixtures in `shared/tests/data/v10/`) covers only the
first `CLASS_COUNT` classes.
`client/tests/nettspend_loadout_e2e.py` reads the id from `shared.h`.

## The v3.1.1 and v1.5.2-era maps (`311_*`, `152_*`)

v10's catalogue has none of these maps. KrunkNative keeps them on purpose (user, 2026-10-12; and
2026-10-24: "old maps should be preserved"):

- `311_<key>`: v3.1.1's 15 official maps, shown as "Burg (3.1.1)" (`assets/maps/311_*.json`,
  thumbnails `ui/img/maps/v311/`);
- `152_<key>`: v3.1.1's 9 `custom_maps/old_*.json` maps from the v1.5.2 era, shown as "Burg (1.5.2)"
  (`assets/maps/152_*.json`).

They are listed in `shared/src/map_catalog.c` (`V311` / `V152`) as community maps
(`MAP_SOURCE_CUSTOM`, mapIndex -1). They are not in the rotation or in Krunker's official list, and
the old keys `v3_<key>` / `old_<key>` still resolve to them. They are map content, not code for an
older version. AGENTS.md's v10-only rule (no version switches, no older pages or rulesets) does not
retire them, and they load through the v3.1.1 / v7.1.4 map JSON compatibility that AGENTS.md
requires. `map_catalog_test` loads every one of them. The audit row
`deob/audit-2026-10-24/v1.5-audit.md` #9 ("retained extra: policy conflict") is settled: keep them.

## The Bhop Mode row (Settings > Controls / Game > Gameplay)

v10.0.3's `SETTINGS.list` (`deob-10.0.3.js:236845-241193`) has no bhop setting; its only `bhop` is the
Parkour game mode's alias (`deob-10.0.3.js:117747-117749`). The "Bhop Mode" select that heads the
Gameplay group (the advanced Controls tab and the basic Game tab) is KrunkNative's: the v3.1.1 offline
build's client mod (`/home/user/jails/kru/src/Krunker_v3.1.1/mod/mods.ts`: BhopMode, BHOP_MODE_NAMES),
ported 1:1 in `client/src/ui/ui_bhop.c`. Modes: Disabled (default), Slidehop (while moving), Hold
Jump/Crouch, Always Jump (while moving), Hold Jump. It is stored as knSettings `bhopMode`, toggled by
the bhop key (`krk_bhop_mode_bind`, KeyB) and `/bhop` (`ui_console.c`), and it only rewrites the input
frame's jump / crouch slots, so prediction and the server see ordinary inputs. The row is inserted at run
time (`ui_bhop.c` insert_row), not in the captured `ui/windows/settings-*.html`, so a pixel diff of the
Controls tab against v10's capture (`settings-advanced-controls`) shows it as an extra row. Kept on
purpose (2026-10-24).

## The Servers window's backend tabs (krunk.io / krunker.io)

v10's Servers window has no backend tabs. The native client lists two backends, so `servers_ui.c` `gen_list` puts a
`bigMenTabs srvTabs` strip (krunk.io, krunker.io) above the map carousel and the groups. The strip is 48 px high with a
28 px bottom margin: the centred window is 76 CSS px taller than v10's, 38 px each way, a whole 22 device px at
1280x720, so the uidiff captures line up at a whole-pixel `ref_offset` (a 30 px margin gave 22.58 px; 619f3e5d,
2026-11-25). The web build has no strip.

## The Medals stat tab (Account window)

v10.0.3's Account window (windows[4], `showWindow(5)`) has five stat tabs, `statTabs`: General Stats,
Killstreaks, Other Stats, Class Stats, Raids (`deob-10.0.3.js:21520`). KrunkNative adds a sixth,
Medals, after them (`statTab5` / `.statTabB5`, `STAT_TAB_MEDALS` in `client/src/ui/account_ui.c`): the
account's medal counts (kn_brain's `["mdl"]` answer, one `.pSt` card per medal with a count, or "No medals
yet"). Only a Box Office account has it; the krunker.io account's window has v10's five tabs (krunker.io
keeps no medal counts). v10's own tab ids 0-4 are unchanged, so `switchStat(0..4)` are v10's. The
Account window's main tabs are v10.0.3's `profileTabs` with no KrunkNative tab (v7's Partner and promo
code tabs were dropped with v10).

## The 2-Factor popup on the game page (`#twoFAPop`)

v10 has no 2FA code popup on any page (krunker.io's 2FA runs through its own login card). Box Office
accounts keep v7.1.4's brain 2FA: Account Security's Enable 2FA (QR code, then six code inputs) and a 2FA
login. Since round 11 the Settings tab enables and disables 2FA through v10.0.7's own io form (act2FA / dact2FA,
deob-10.0.7.js:217005-217146), so `#twoFAPop` is left for the 2FA step of a socket password login only.
`client/src/ui/account_ui.c` (`twofa_res`) shows that step in v7.1.4's `#twoFAPop`, so the game page
(`ui/index.html`), like the hub and the editor, carries that popup and `css/kn-twofa.css`
(`tools/ui-import/v10_hub_editor.py` `add_twofa`; the game page's variant `KN_TWOFA_GAME` has the QR
block). Test: `services/brain/tests/email_verify_web_e2e.py` (CTest `brain_email_web_e2e`).

## Intentional fixes where v10.0.3 itself is wrong (user, 2026-10-25)

The user chose "keep the fixes, documented" over copying v10.0.3 bug for bug. These are deliberate
differences from the bundle, not unported gaps; each is also marked in the code. Audits should leave
them out of the gap list.

| Behaviour | v10.0.3 | KrunkNative (kept on purpose) | where |
|---|---|---|---|
| Zombies Runner count | keeps v7's Runner-count bug | the corrected count (the long-standing documented fix) | `shared/src/modes/` Zombies rules, `zombies_test` |
| SRM movement keys | one copy of the last keys shared by every player on the server | each player's own last keys | `shared/src/player.c`, `doc/v10/movement-schemes.md` |
| SRM ramp wall-dash | computes NaN from the player's slope | uses the ramp's slope | `shared/src/player.c`, same doc |
| Defuse bomb pickup with a same-slot weapon (Boulder, Minigun) | swaps the two, the plant never drops the bomb and "Bomb Planted" fires every tick | the bomb is added to the loadout; only one bomb planted at a time | `shared/src/defuse.c`, `defuse_test` |
| Multi-round Defuse | the bomb, the defuse count and lives are not reset between rounds | the same missing reset is kept for parity in the single-round flow; multi-round hosted Defuse is the case this affects, so this entry is the one to revisit if it should be fixed instead | `shared/src/defuse.c`, `doc/v10/rules-1003.md` |

Multi-round Defuse is listed separately in the follow-ups because the user picked "keep the fixes"
for the group while this one is a v10 bug KN currently *copies*. If multi-round Defuse should be
playable, reset the bomb, the defuse count and lives at round start and delete this row.

## In-game windows: differences kept in the v10.0.7 sweep (2026-11-23)

The line-by-line sweep of the store, Battle Pass, challenges, events / spins / raffle, ranked, Turf Wars,
leaderboards and profile windows against `deob-10.0.7.js` (doc/v10/parity-audit-2026-11-20-ui.md, "In-game
windows") ported every gap it found except these, which are kept on purpose. Audits should leave them out.

| Window | v10.0.7 | KrunkNative (kept) | Why / where |
|---|---|---|---|
| Ranked popup | `.home-container` watches resizes and DOM mutations for `has-scroll-bar` | measured after each render and on open | no MutationObserver in the native page; `ranked_ui.c` |
| Ranked popup | "Connecting to match..." until the game says so | the same, capped at 15 s | a join that never settles must not leave the banner up; `ranked_ui.c` |
| Ranked shop | the row is looked up again after the confirm | the same, but a shop still loading uses the row the dialog opened for | `ranked_ui.c` |
| Chat | the official server decides which lines carry `extra.actions` | Box Office puts [Join Queue] on the brain's queued-for-ranked line | the only ranked line Box Office's server sends; `server/src/net.c` |
| Battle Pass > Challenges | daily challenges only | also a "Weekly Challenges" block | Box Office's weekly rows (23-26) would be shown nowhere else; `battlepass_ui.c` |
| Battle Pass | the "Loading Battle Pass" / "No Battle Pass Found" div has no class (:219368) | the same div with class `bpStatusText` | `battlepass_ui_tick` redraws the window after an unanswered `bpdl` only while that div is on screen; `battlepass_ui.c` |
| Challenges | official daily BPP values | 50, and 25 for the spray and assist rows (migration 0397) | no official capture of the dailies; the values are the offline v10 sandbox fixture's |
| Profile > Sales | Chart.js measures its tick labels with `measureText`; its tooltip has a caret | the same line chart on the native canvas, labels measured by a GameFont estimate (0.9 x the font size per character), no caret | no text metrics on the native canvas; `v10_social.c` `sales_chart_paint` |
| Profile staff actions | `/moderation/*` and `/players/:name/verify` over gapi | the same routes to kn_brain over HTTP with `?token=` | the social-socket tunnel does not carry them |
| Profile data | `player_elo4` is the junk balance | the brain fills `player_elo4` with the junk wallet's text | v10 reads Junk from that field; `db_social.c` |
| Profile / leaderboards deep links | `?profile=` / `?leaderboards=` | web build only | the desktop client has no page URL; `client/src/main.c` |
| Market listings on a profile | a listing's nametag | Box Office listings have none | Box Office sells no Name Tag items; copies do carry their wear and serial now (0398) |
| Community events | only `http(s)://` links | Box Office's relative `/?play=` and `/social.html` links too | the brain knows no public web base; `v10_news.c`, `community.c` |
| Prize wheels | spin data and lottery pushes on the game socket | `swu`, `lotEnt`, `lotWinP`, `lotEnd` on the social socket | the client talks only to kn_brain |
| Prize wheels | Spin Error's "Purchase More" opens a USD catalogue product | the Paint Ticket is a 50 KR product (90001, `kn.paintticket`, migration 0398), bought through the no-payments checkout | no real payments on Box Office |
| Prize wheels | no Free KR wheel in 10.0.7 | the Free KR wheel through `claimReward` | earlier Box Office addition; `store_ui.c` |
| Raffle | no deadline | a 15 s deadline and a Retry button on a load that never answers | `v10_misc_popups.c` |
| Events | every event ends | an open-ended event shows no "Ends in" line | operators may run one without an end |
| Turf Wars | `clans/<id>.png?v=<n>` user assets | Box Office logos from `/api/clans/<id>/logo?v=<n>` | the brain serves the approved logo; `turf_ui.c` |
| Turf Wars | a failed day change shows the error | the prompt and the error | `turf_ui.c` |
| Turf Wars | the post is filtered out after a take-down | the board reloads | `turf_ui.c` |
| Turf Wars | Daily View fetched each time | a failed read is cached for 30 s like every Turf read | `turf_ui.c` `TTL_MS` |
| Turf Wars | `isDeputy` from the account | `is_deputy` comes from krunker.io accounts only | Box Office accounts have no Deputy role |

## Round 11: differences kept in the v10.0.7 sweep (2026-11-23)

The line-by-line sweep of the Clan window, friends and party, the Studios windows, Rent Servers, the account
settings tab and Account Manager, Report Player / Votekick and the Social and Notifications panels, plus round
10's leftovers (spin instances, Purchase More, the lottery bulbs, KrunkCup, the mid-match `chgC`, the Sales chart),
ported every gap except these (doc/v10/parity-audit-2026-11-20-ui.md, "Round 11").

| Window | v10.0.7 | KrunkNative (kept) | Why / where |
|---|---|---|---|
| Prize wheels | a copy's wear comes from Krunker's server-side distribution | uniform in [0, 1) | the distribution is not in the bundle; `services/brain/store.c` |
| Prize wheels | a weapon copy shows its wear map on the model | typed items show wear; weapons do not | the assets have no `weapons/weapon_N_wear` textures; `spin_wheel.c` |
| Prize wheels | Purchase More writes the catalogue key into `onclick` with raw quotes, which breaks its own button | the quotes are escaped (`&quot;`) | a v10 bug; `store_ui.c` `spin_error` |
| KrunkCup | `/krunkcup/wristband` and its claim are defined but never called | `{claimed, eligible, team, item_id}`, a `{team}` claim and 400 `KRUNKCUP_REFUSED` | the shape is KrunkNative's own; `v10_account.c` |
| KrunkCup | top players' team from Krunker's tournament data | the team the player scored most for | `services/brain/leaderboards.c` |
| Challenges | official daily BPP values | 50, and 25 for the spray and assist rows (0397) | still no official values: no capture of a real daily row exists (round 12) |
| Clans | the owner transfer goes over the socket; loot and recruitment reads too | gapi paths for the transfer, kn_brain HTTP routes for the reads | Box Office's clan API; `clan_ui.c` |
| Clans | the pending logo is `clans/<id>_pending.png` on user-assets | the file this client sent, shown on that client only; the approved logo is `/api/clans/<id>/logo` | kn_brain keeps no pending picture; `clan_ui.c` |
| Clans | the clan and Settings loads have no deadline (the socket reconnects) | 15 s: "Couldn't load the clan. Try again." with Retry, and v10's "Failed to fetch" state in Settings | the brain can go away; `clan_ui.c` |
| Notifications | clan notifications (type 1) carry Krunker's server text | the tab and its empty state only | the text is not in the bundle; `account_ui.c` `gen_mail` |
| Notifications | creator-code mail (type 5) | drawn as v10 draws it, never sent | Box Office has no real-money purchases |
| Friends | JS `toLowerCase` / `localeCompare` | Latin, Greek and Cyrillic case folding, root collation for ASCII | no full Unicode tables in the C client; `v10_social.c` `js_lower`, `js_name_cmp` |
| Rent Servers | Krunker's reply texts | Box Office's own (name taken, ...) | server-side, not in the bundle; `services/brain/rentals.c` |
| Rent Servers | a request is queued while the socket reconnects | "Not connected to Box Office servers." | `rent_servers_ui.c` |
| Rent Servers | `rdsrv` has the game id | also `cr` and `key`, so kn_rentd knows the map | `rentals.c` |
| Studios | analytics' circulating and holders from Krunker's currencies | 0 | those figures are the game server's currencies (gameplay); `v10_studios.c` |
| Studios | the list opens again as a new view | the open list is reused | `v10_studios.c` |
| Account | no Account Manager | the Account Manager (encrypted alt vault) | a KrunkNative window; `alt_manager_ui.c` |
| Account | a sign-in may stop at the verify-email challenge (`ensure_verified`, :115846) | not shown | kn_brain sets no email gate on the socket sign-in |
| Account | Usernames: buying a reservation is a payment | "no purchases" (`kn.store.noPurchases`) | no real payments |
| Account | Discord linking through Krunker's OAuth app; the result on the game socket | Box Office's own Discord app (`discord.c`); `dlnkR` on the social socket | Krunker's app and host are not ours |
| Report Player | REST `POST /players/{name}/report` | the social socket's `rpt` frame, same fields and outcome | `v10_social.c`, kn_brain |
| Scoreboard / end table | `rankedRank` and `flagIndex` ride in ADD_PLAYERS [46] / [26] | a KrunkNative EXT, `NET_EXT_KN_PLAYER_RANK_FLAG` (0x81B0), right after each PLAYER_INFO | KrunkNative's PLAYER_INFO keeps its size for older clients; `server/src/accounts.c` |
| Scoreboard | `titleIndex` rides in ENTER_GAME [34] and ADD_PLAYERS [52] (:215052, :176753) | the client sends it to our server in `NET_EXT_KN_LOOK` before the ENT; the server sends `NET_EXT_KN_PLAYER_TITLE` (0x81B1) right after the rank / flag, and only a title the account owns (kn_brain's account `titles`, player_titles), else -1 | v10.0.7's own server code (the ownership check) is not in the bundle; Box Office checks it so a client cannot wear any title; `server/src/accounts.c` |
| Scoreboard / end table | Hide player names (KN setting) | blanks the other rows' flag, rank, title, clan and icons too | KrunkNative's masking; `hud_ui.c` |

## Round 12: differences kept in the v10.0.7 sweep (2026-11-23)

Round 12 closed round 11's leftovers (the end table's skin reward, item copies' wear, serial and name tag in a match
and in the Market, a real mid-match challenge end to end, skin makers) and swept the areas no round had covered:
the map editor, the Mod Manager, the K.P.D. replay viewer, the Twitch / streams panels, the loading screens, the
captcha and anti-cheat popups and the ad boxes (doc/v10/parity-audit-2026-11-20-ui.md, "Round 12"). It ported every
gap except these.

| Window | v10.0.7 | KrunkNative (kept) | Why / where |
|---|---|---|---|
| Item copies | a player's worn instances ride in the add data (`{i, id, m}`, :24335) and endGame's skin reward in the end data | `NET_EXT_KN_SKIN_META` (0x81D1) with each SKINS and `NET_EXT_KN_END_REWARD` (0x81D0) before the END | older clients' packets keep their size; `server/src/accounts.c`, `shared/src/game.c` |
| Item copies | a name tag is set by Krunker's own tools; the client has no rename route | `POST /mm/admin/items/:copy/nametag`, operator secret only | no v10 client route to copy; `services/brain/v10_market.c` |
| Item copies | every slot's instance is known | a slot naming a copy (`"<copyId>#<index>"`) shows that copy's m, a plain index the owner's first copy of it | the ENT carries plain indices for older clients; `accounts_skin_meta_send` |
| Challenges | official daily BPP values | 50, and 25 for the spray and assist rows (0397) | no repo file or kru capture carries real daily rows; the bundle's `bpp: 0` are running totals, not challenge data (doc/v10/challenges.md §4d) |
| Challenges | `chgC` and `uchp` on the game socket | the same frames, same order, on the social socket (kn_brain) | Box Office's game server reports to kn_brain, which owns the account |
| Map editor | no AI assistant | KrunkNative's assistant panel, in the editor's GameFont and panel colours | a KrunkNative feature; `client/src/editor/editor_ai*.c`, `ui/css/editor-ai.css` |
| Map editor | `alert()` popups | `#chatList` lines | KrunkNative's editor convention; `editor_message` |
| Map editor | Optimize Faces and Color Merging cannot be undone | one undo step each | KN's history records every object change; `editor_tools.c` |
| Map editor | an import parse error is the browser's SyntaxError text | KrunkNative's parser's wording after v10's "Failed to import map with error:" | no JS engine; `editor_io.c` |
| Map editor | `noimg2.png` for a missing preview | `./img/noimg.png` | `noimg2.png` is not in the tree |
| Map editor | uploads in one request | chunked uploads | kn_brain's upload limits; `editor_net.c` |
| Map editor | multiplayer rooms on Krunker's editor server | kn_brain's social socket relay; its error texts, the Guest name, the palette, invites through `KN_PUBLIC_ORIGIN` and the camera format are KrunkNative's | Krunker's host is not ours; `services/brain/editor_rooms.c`, `editor_mp.c` |
| Map editor | the Object Painter's div stays when the switch is off; the entity stage preview | the div follows the switch; small drawing differences in the stage | `editor_panels.c`, `editor_entities.c` |
| Mods | mods from any URL (Dropbox and others), extracted file by file ("extracting i/N") | the user-asset host only, loaded in one step (only "Success!") | Box Office serves mods itself; `mods_ui.c`, `mod_pack.c` |
| Mods | the mod view's Use opens a new tab; ClientSession's downloadPack button | Use loads the mod in place; no downloadPack button (official desktop client only) | one window; `mods_ui.c` |
| Mods | — | `ui_mods.c` and `ui_mod_keys.c` | KrunkNative v3.1.1 mod features, not v10's |
| Twitch | a drops claim lists the redeemed skins ("Drops Redeemed") | "Drops claimed." | Box Office has no drop campaigns; `account_ui.c` |
| Streams | Krunker's streams service | kn_brain's own Twitch Helix source; featured, promo, campaigns and event tags from config | `services/brain/streams.c` |
| Captcha | the page always shows the Security Challenge card | with no page (headless, the console), the challenge is solved in slices with nothing shown; an unsolvable official one still ends with CAPTCHA FAILED (v10.0.3's branch) | `kn_captcha_game.c`, `altcha_modal.c` |
| Captcha | — | `KRUNKNATIVE_ALTCHA_CLICK=1` clicks the checkbox as the card shows | a test and screenshot switch, like `KRUNKNATIVE_AD_SECONDS` |
| Ads | `FRVR.ads.show` plays an ad | KrunkNative's placeholder card, with v10's onShow / onComplete timing | Box Office has no ad network; `kn_ad.c` |
