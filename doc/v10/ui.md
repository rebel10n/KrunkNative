# v10.0.2 UI: menu, HUD, windows, asset URLs

Source: `/home/user/Downloads/v10.0.2.js` (bundle reports `gameVersion '10.0.1'`, `clientVersion '10.0.1r1'`).
Line numbers `v10:N` refer to the identifier-renamed copy `~/.cache/kn-v10/v10.js`
(made by `~/.cache/kn-v10-tools/rename.py`, line-for-line identical to the original).
The v7 references are `/home/user/jails/kru/src/Krunker_v7.1.4/` (`v7:` prefix). The repo
page is `ui/index.html` and its windows are in `ui/windows/windows.json`. Anything marked
*(guess)* is inferred, not read directly.

## 1. Architecture change: the menu is now Svelte 5

| Aspect | v7.1.4 | v10.0.2 | Evidence |
|---|---|---|---|
| Main menu markup | static `public/index.html` (`#menuItemContainer`, `.menuItem` + inline `onclick`) | Svelte component `_52533` mounted into `#mainMenuUIMount` at boot | v10:222882 `_4495(_52533, {'target': document["getElementById"]('mainMenuUIMount'), 'props': {'showExit': ...}})`; template v10:145953; v7:public/index.html:611-641 |
| Component runtime | none (string `innerHTML`) | Svelte 5 runtime (`_4461` = template, `_4495` = mount, `__click` delegated events, `$$slots`, `$state` stores `_5091`) | v10:11856-11915, 181 scoped CSS blocks `{'hash': "svelte-xxxx", 'code': ...}` (inventory: `~/.cache/kn-v10/svelte-inv.txt`) |
| Windows | `window.windows[]` with `gen()` returning an HTML string | the same `window.windows[]` (54 entries). `gen()` may now return a string, a Svelte component, `{html, component, targetId, props}` or `{html, components:[...]}` | v10:216690-216830 (`showWindow`): `"function" == typeof _72029` -> mount; `"components" in _72029` |
| Popups / modals | `genericPopup(html)` | `genericPopup({component, props}, "<cssClass>")` mounts a Svelte component in the popup holder | v10:215664-215760 |
| Other mounts at boot | - | `gameAnnouncementEl`, `matchmakingStatusEl` (ranked queue banner `.ranked-matchmaking-status`), `emojiPanelHolder`, `notificationCenter` | v10:222866-222880, v10:97416 |
| Per-component CSS | `css/main.css` only | each component injects its own `<style>` block (`'code'` string) | e.g. v10:125584 lottery (18.6 KB CSS) |
| i18n | `src/i18n/en.js` (1427 keys) | inline `translations['en'] = parseText("...")`, 2170 keys (+746, -3, 12 changed) | v10:197610; diff `~/.cache/kn-v10/i18n-diff.txt` |

Port impact: the menu can no longer be imported as static HTML from a reference
`index.html`, and `make ui-import` cannot produce it. Each Svelte template string
(`_4461("<div ...>")`) holds the markup for one component, and its `'code'` string holds
the CSS. A port has to reassemble those into the page (or into `ui/windows/*.html`
fragments) and port the behaviour to C under `client/src/ui/`, as was done for v3.1.1.
*(guess)* Extracting templates and CSS with a script is practical, because both are
plain string literals.

## 2. Main menu (left column + header)

v10 menu items come from `_50477({icon,label,labelId,analyticsLabel,action})` (side
nav, v10:147022-147167). The primary buttons come from `_50410({label,icon,variant,id,action})`
(v10:147318-147370). The v7 layout is in v7:public/index.html:208-211 (buttons) and
:611-641 (items). The repo copy is ui/index.html:185-188 and :589-617.

| Item (id) | v7.1.4 | v10.0.2 action | v10 line |
|---|---|---|---|
| Quick Match `#menuBtnQuickMatch` | - (the big Play button) | `backToOrigin(false, {exclude: ?game})` | 147318 |
| Ranked `#menuBtnRanked` | window 27 from the class/menu header | `openRankedMenu()` -> `genericPopup({component:_37935}, 'rankedMenuModal')`, requires login | 147331, 215664 |
| Host Game `#menuBtnHost` | `showWindow(42)` | same | 147345 |
| Find Game `#menuBtnBrowser` | "Server Finder", `showWindow(48)` | renamed "Find Game", same window | 147353 |
| Custom Games `#menuBtnCustomGames` | `#menuBtnMaps` "Games" -> `showGames()` | new id, `variant:"cyan"` | 147361 |
| Join `#menuBtnJoin` | button | moved into the match-info bar (`#matchInfoHolder` invite/join actions) | 141634 |
| Guide `#menuBtnGuide` | - | `openGuide()` -> `tutorialPopup` (sidebar tutorial with `img/guide/*.png`, `videos/tutorial/basics.mp4`) | 147022, 216856, 197030 |
| What's New | changelog via window 35 | nav item -> `showWindow(35)` | 147037 |
| Turf Wars `#menuBtnTurfWars` | - | `genericPopup({component:_51011}, 'defaultModal')`: territory map (`./img/territory_map.webp`, `./img/turf/<mapId>-raised.png`) | 147059, 142560-142713 |
| Market & Trading `#menuBtnMarket` | shop tab "Marketplace" | `openExchange()` -> `exchangeModal` (market grid, item detail, trading tab, quick-sell) | 147073, 215686 |
| Store `#menuBtnShop` | "Shop", window 14 | "Store", window 14 | 147105 |
| Challenges `#menuBtnChall` | `windows[32].openHome()` | same; new ranked challenges section `#challRankedHeadr` | 147114, 214584 |
| Skin Manager `#menuBtnSkinManager` | - | `showSkinManager()` (hidden `visible:!1`) | 147122 |
| Leaderboards `#menuBtnLeaderboards` | social.html page | `genericPopup({component:_49281}, 'defaultModal')` (categories below) | 147139 |
| Social `#menuBtnSocial` | "Hub" -> `window.open('/social.html')` | `openSocial()` -> `socialModal` (in-game hub) | 147150, 215708 |
| Community & Events `#menuBtnSideCommunity` | - | `openCommunityMenu()` -> `communityPopup` (polls, events; uses `gapiClient`) | 147158, 215754 |
| Exit `#menuBtnExit` (`#clientExit`) | `clientExitPopup()` | same | 147167 |
| Profile / Mods / Settings (`#menuBtnProfile/Mods/Settings`) | menu items | no longer menu items (header bar `signedInHeaderBar` avatar -> window 5; settings via header -> `showWindow(1)`; inbox -> `showWindow(30)`) | 145394-145418, 145948 |
| Battle pass `#menuBtnBattlepass` | ad block | `.bpItem` card (logo `./img/battlepass/logo_0.png`, countdown `#bpCountdownTimer`) | 144975, 212212 |
| Daily spin `#dailySpinDiv` | - | card with `./img/menu/free_spin.png` | 144980 |
| Popular Now | - | list of top games, `menu.popular.header` / `{0} playing` | 147225 |
| Streams overlay | `#streamContainer` | `.streams-overlay` (twitch drops tag, small/expanded mode saved as `streams-overlay-view-mode`) | 145020, 145190 |
| Active buffs | `#doubleXPHolder` | `#activeBuffs` badges: 2x XP, 2x Class XP, `#doubleRaidDropsAd` | 141566 |
| Logo | `/img/logo_6.png` + random face `img/menu/faces/f_N.png` (v7:public/index.html:456-458) | `./img/logo_10.webp`, no face | 145953 |
| Web push | - | `.webpush-container` subscribe toggle (VAPID key `webpushVapidPublicKey`) | 145394 |

## 3. Windows (`window.windows[]`, id = index + 1)

The v10 array starts at v10:210938. A full listing (id, line, label, width, header key)
is in `~/.cache/kn-v10/windows.txt`. v7 is v7:src/index.js:4009-9575 and the repo is
`ui/windows/windows.json`. Ids 1-52 keep the v7 order and labels, so `showWindow(n)` call
sites carry over. Differences:

| Id | Label | v7.1.4 | v10.0.2 | v10 line |
|---|---|---|---|---|
| 3 | customize | width 850 (windows.json) | width 1525. Loadout is now a 3-tab shell (`#loadoutToolbar` tabs Class / Loadout / Customize, `#charPreviewSlot` live preview, `loadoutStyle` windows wrap their items in `.wcpRow`) | 211547, 210927 |
| 5 | profile | width 955. Tabs: home, Friends, **Partner**, Settings, Connections, **vpn_key** | width 1280. Tabs `profileTabs`: home, Friends, Settings, Connections (Partner and 2FA key tabs removed). Discord link section mounts Svelte `discordLinkMount` | 211812, 212065; v10:18630 vs v7:src/config.js:2663 |
| 13 | clan | home, Members, Stats, Requests | adds a 5th tab (settings icon). Tab 0 mounts `clan-ownership-transfer-banner` + `clan-loot-mode-display`, tab 4 mounts `clan-settings` (logo upload 512x512 PNG for 20,000 KR, MOTD, recruitment, loot mode, ownership transfer via `rpc('clan-transfer-initiate'/'clan-transfer-respond')`). Covered in clans.md | 213330; v10:18612 vs v7:src/config.js:2635 |
| 14 | shop_crafting | tabs: home, Prize Wheels, Premium, Purchase KR, Redeem Keys, **Marketplace**, **plumbing** (crafting) | tabs `shopTabs`: home, Prize Wheels, Premium, Purchase KR, Redeem Keys, **Eterno Shop** (tab 5 sets `.eterno` class + music player "Eterno Rules the World"/`eterno_lofi`). Marketplace moved to `openExchange()`. Home tab is Svelte `_44131` (bundles, wild offers, heroic spin) | 213583; v10:16476 vs v7:src/config.js:157 |
| 6 | battlepass | `bpTabs` home, Challenges | unchanged tabs. Background `/img/battlepass/bg_window_5.png` | 212116, 127471 |
| 22 | (unused) | header "Social" | unchanged ("TODO") | 214034 |
| 23 | player_list | - | in sandbox games appends Svelte `#playerInfoExtension` | 214092 |
| 27 | ranked | width 1800, legacy tabs | same window object, but `gen()` returns `''`. The ranked UI is the `rankedMenuModal` popup (tabs Home / Leaderboard / Match History / Profile / Shop). Covered in ranked.md | 214128, 111446 |
| 28 | partner_dashboard | "Partner Dashboard" | header `''`, empty `gen` (Partner program removed) | 214146; v7:src/index.js:8193 |
| 33 | challenges | - | adds a ranked challenges block (`windows.chall.ranked`, `hed_3.png`) | 214584 |
| 47 | terms | iframe | unchanged (`https://frvr.com/legal/krunker/`) | 215275 |
| 53 | customize | - | **new** entry: copy of the v7 window 3 layout (width 850). `showWindow(53)` (15 call sites, e.g. every `selectHat/selectHead/...`) is rewritten in `showWindow` to window 3 with `tabIndex=2` (the Customize tab of the new shell); `updateWindow(53)` re-renders it | 215369, 216695 |
| 54 | title_selection | - | **new**: "Title" selector (`windows.loadout.title`, `titleRow`/`selectTitle`), width 1525, `loadoutStyle`, `loadoutTab:2`, back -> `goBackWindow(52)` (player card) | 215489 |

Numbering: v10 has 54 entries (the listing in `~/.cache/kn-v10/windows.txt` merges 46-48
because `terms` is wrapped in `_69(...)`. The real order is 46 charm, 47 terms, 48
host_type, 49 esports, 50 back, 51 head, 52 playercard). Ids 1-52 match
`ui/windows/windows.json` one for one, including the gaps 11, 12, 19, 22 and 29 that are
`unused`. Only 53 and 54 are new.

## 4. New popups and modals (no v7 equivalent)

All are `genericPopup({component, props}, cls)` or Svelte overlays.

| Feature | Entry point | Class / component | Key strings / ids | v10 line |
|---|---|---|---|---|
| Ranked menu | `openRankedMenu()` | `rankedMenuModal`, `_37935` | tabs Home/Leaderboard/Match History/Profile/Shop; states Searching/Found/Cooldown/"Level Required"/"Rank Data Missing"; `#queueAllRegions` "All regions" | 111440-111446, 215664 |
| Ranked rating/matches tabs | inside leaderboards | `.ranked-rating-tab` (region tabs, season select), `.ranked-matches-tab` | | 138655, 138667 |
| Ranked home card | ranked modal | `.rank-card`, `.season-banner`, `.rank-icon` | rank icons `img/ranked/ranks/rank_*.svg` | 98974, 97509 |
| Leaderboards | `openLeaderboards(cat)` | `defaultModal`, `_49281` | player_score/kills/wins/timeplayed/funds/followed/nukes/skinvalue/eventcount; clan score/members; raid best time per map (Tortuga, Laboratory, Soul Sanctum, Khepri, Bastion); ranked Rating/Matches; KrunkCup `.krunkcup-component` | 138718-138800, 138699 |
| Social hub | `openSocial(tab)` | `socialModal`, `_49999` | tabs Social / Friends / Clan / Search; feeds Hot / Global / Following / News / Giveaways; posts, comments, DMs (`.conversation-root`, emoji panel), follow lists, profile search, player profile (`openPlayerProfile`), clan view (`openClanView`), map/mod detail, Studios | 139579-140479, 215697-215745 |
| Market and trading | `openExchange(tab, idx)` | `exchangeModal`, `_46780` | `.market-grid`, `.item-detail-container`, `.trading-tab`, `.qs-window` (quick sell), price/fee modal. Ported: client/src/ui/v10_market.c (doc/v7/economy.md section 8). Differs: no Junk / crafting (Junkyard shows no recipes), no item instances (wear, mints, serials, paint, styles, name tags), no price-estimate webhook (data/prices.js values), flat previews instead of the 3D showcase iframe, sales charts as SVG images with HTML axis labels, item lists paged client-side (Load More) instead of by API cursor, fees/levels/limits from the server (`ecfg`) | 129991-132599 |
| Items / inventory | `openItems()` | `itemsModal` | `.items-view-root` | 215702 |
| Lottery / raffles | `openLottery(id=3)` | `defaultModal lotteryModal`, `_43358` | `img/lottery_bg*.webp` | 125584, 215678 |
| Turf Wars (Territory Wars) | menu item | `defaultModal`, `_51011` | `TERRITORY_WARS_INTRO_DATE "2026-03-17"`, payouts tab, `img/territory_map.webp`, `img/turf/<mapId>-raised.png` | 141788-142713 |
| Community and events | `openCommunityMenu()` | `communityPopup`, `_66842` | polls (`windows.community.polls.*`) | 215754 |
| Guide / tutorial | `openGuide()` | `tutorialPopup`, `.tutorialContent` | 29 `img/guide/*.png`, `videos/tutorial/basics.mp4` | 197030, 216856 |
| Username purchase | `showUsernamePurchase(name, renew)` | `.usernames-content` | `usernamePriceUSD 9.99`, `usernameValidDays 365`, `usernameSwapDays 7`, `usernameGraceHours 167`, `usernameHoldMins 15`; product id `username-renew:<name>`; blocked on Steam (`account.usernames.steam`) | 113085, 209249 |
| Wild offers | `showWildOfferPurchase` | `.wild-offer-notification`, `.wild-offer-showcase` | product id `wildoffer:<id>` | 126471-126847, 209224 |
| Signup / register rewards | `showSignupRewards()` | `signupRewardsPop`, `registerRewardsPop` | `img/register_rewards_crd.png`, "Welcome to the Family!" (24 h premium) | 115207, 196633, 215763 |
| Guest earnings | after match as guest | `.guest-earned-collect` | `guestKRCap 20`, `guestScoreCap 2500` | 115125 |
| Email opt-in / verify | account flow | `.optin` (`./img/email_optin_bg.webp`), `windows.verifyEmail.*` | | 114510, 113519 |
| KID / GDC parental mode | `?displayMode=gdc-kid` (stored in `krunker-display-mode`) | `.kid-display-dialog` | clears localStorage except `testCode`, clears cookies; `gdc.kid.*` strings (k-ID partnership, parental consent) | 117037-117084, 223120-223161 |
| Captcha | login by username | Cloudflare Turnstile, `appearance:"interaction-only"`, `#turnstileHolder` | sitekey `0x4AAAAAAD19NTFH3Itq7QOn` (prod) / `1x00000000000000000000BB` (dev); replaces v7 reCAPTCHA Enterprise (v7:src/index.js:121) | 114051-114090, 197730 |
| Payments | purchase holder | `#xsollaPayB` (xsolla), `#stripePayB` (FRVR iap, `stripeEnabled`), PayPal; packet `PURCHASE_PRODUCT_XSOLLA` / `xst` | v7 had xsolla + paypal + crypto/cash/apple/android buttons | 208470-208495 |
| Mission briefing | event | `.mission-briefing` "CLASSIFIED INTEL" | | 117106 |
| Survey, game announcement, notification center, music player | various | `.survey-notification`, `.game-announcement`, `.notification-center`, `.music-player` | | 115242, 116779, 197436, 197345 |
| Skin editor / shader graph | Skin Manager, studios | `.sgEditor`, `.paint-container`, skin settings sections | | 118293-122203, 120551, 128599 |
| Retention / client exit | `showRetentionPopup`, `clientExitPopup` | `clientExitPop` | | |

## 5. HUD / in-game changes

Most of the in-game HUD is still legacy DOM, with the same ids as v7 (`#killFeed`,
`#ammoVal`, `#chatList`, `#spectateUI`...). They are reached through global-id access, so
they do not show up in `getElementById` greps. v10 HUD additions:

| Element | What | Evidence |
|---|---|---|
| Ping markers | world-space marker drawn on the 2D overlay canvas from `game.pingMarkers` (life, position, pulse `0.1*sin(t/100)+1`, faded near crosshair), sprite `/img/ping.png`; bound to control `windows.controls.markPosition` "Ping Position (Ranked Only)" | v10:160110-160135, 170189 |
| Vote forfeit | `#voteToForfeit`, `#voteForfeitInfo` "X initiated vote", `#voteForfeitNum` "Votes: a/b"; `voteForfeitTimer 60000`, `voteForfeitDisconnectGrace 30000` (ranked) | v10:219759, 20095 |
| Charge consent | `#chargeConfirm` / `chargeModal`, packet `CHARGE_CONSENT_RESPONSE` | v10:219750 |
| Ranked queue banner | `.ranked-matchmaking-status` mounted in `matchmakingStatusEl` | v10:97416, 222871 |
| Comp roster | host config `#compRosterT1/#compRosterT2/#compRosterSpecs` (comma-separated names) | v10:198052 |
| Speedrun checkpoints | `#spdRunI<n>` / `#spdRunIVal<n>` per scorezone, up to `maxSpdRChck` | v10:104149 |
| Thrown-melee icon | `#weapIconMel` switches to `/textures/melee/icon_<skinId>` when `canThrow` | v10:169339 |
| Replay / KPD | `#replayResolve` "HACKER? YES/NO/x" (`#replayVoteY/N/C`, `#replayResolveBtn`); KPD demo search `#kpdDemoQ` | v10:222429-222444, 216138 |
| Stalker taunts and sandbox binds | controls `stalkerTaunt0..5`, `smiteSB`, `unlimitedSB` | v10:197610 (i18n), 173931, 212553 |
| End screen | `#endLayoutHolder` tab body; "Combat Earnings" layout, raid promo `#raidPromoContainer` | v10:203117-203218 |
| Emoji panel | `emojiPanelHolder` -> `.emoji-panel` (in-game emotes) | v10:222874, 197374 |
| Removed (not referenced anywhere in v10 JS) | 177 v7/ui ids, including `#menuBtnMaps/Mods/Profile/Settings/Join`, `#menuKRCount`, `#menuLevelBar`, `#streamContainer`, `#twoFAWindow`, partner ids, `#shopMainNav`, creator-code fields, `#consentWindow` | list: `~/.cache/kn-v10/ids.txt` + script `~/.cache/kn-v10-tools/ids.py` *(the v10 index.html was not supplied, so ids that exist only in its static markup cannot be ruled out)* |

## 6. Settings / i18n deltas affecting the page

| Key | v7 | v10 |
|---|---|---|
| `settings.serverb.header` | Server Browser | Server Browser & Matchmaking |
| `settings.interface.nametagStyle.all` | Everything | Default |
| `generic.tips.level` | level 20 | level 10 (market/trade; matches `tradeMinLvl 20->10`, `marketMinLVl 15->10`) |
| `player.inactive` | Kicked for inactivity | Disconnected due to inactivity |
| new groups | - | `studios.*` (130+), `editor.entity.*` 53, `account.usernames.*` 34, `generic.auth.*` 33, `errors.exchange.*` 30, `windows.kid.*` 28, `windows.verifyEmail.*` 24, `windows.clans.settings.logo.*` 17, `generic.wildOffer.*`, `gdc.kid.*`, `shaderGraph.*`, `emailPrefs.*`, `social.*`, `friends.*`, `market.*` |
| removed | `windows.account.recover*` (3 keys) | - |

## 7. Asset URLs

The resolver is `_9254(path, keepSlash, local)`: it strips the leading `/` and returns
`"https://assets.krunker.io/" + path + "?build=" + buildVersion`, or `./path` when `local`
is set (v10:24247-24251). This is the same as v7's `utils.assetUrl` (v7:src/libs/utils.js:676).
`buildVersion` is `FArgZqd2pNs7O8lWk4TblHSdDWfMUfD2` (v10:20115; v7 used
`egh9QTeIptdMdJIrIKaFXvKz9fCqj0mt`). A few hard-coded URLs keep a stale
`?build=4Q9IjxljZogSd4f5mvT6QLSHsyBGeq8I` (server_zombies/raids backgrounds, the same as
v7:src/config.js:281,303) or have no query (`gamecats/c_N.png`, `img/levels/`, `img/maps/map_35.png`).

User content goes through `_9259` on `https://user-assets.krunker.io` (overridable,
v10:25560): `skins/...` gets `?v=Date.now()`, other paths get `?v=<n>` or the build query.
Weapon asset icons are `/<id>/texture.png` (v10:173867). The DigitalOcean hosts are
rewritten to user-assets (v10:24347). Profile pictures are
`https://assets.krunker.io/p<pli>/profile.png` when `prm > 0`, else `./img/player_pfp.png`
(v10:106263).

All 268 literal path prefixes are in `~/.cache/kn-v10/v10-paths.txt` (new ones: `v10-new-paths.txt`). Regenerate them with the grep
in this section's footnote. **New in v10** (not referenced by v7 src/public, and absent
from the repo `ui/` and `assets/`):

| Directory | Files / templates | Used by |
|---|---|---|
| `img/ranked/ranks/` | `rank_unranked.svg`, `rank_bronze.svg`, `rank_silver.svg`, `rank_gold.svg`, `rank_platinum.svg`, `rank_diamond.svg`, `rank_master.svg`, `rank_kracked.svg` (sub-divisions Bronze1..N share one icon) | ranked badge, v10:97509-97513 |
| `img/guide/` | challenge_mode(_on/_off), clan_create/manage/profile, classes, customize(_click/_result), host_find_game, host_game_setup, invite_friends(_url), loadout, map_challenges, market, quick_match, ranked, ranked_leaderboard, spin_reward, spins, streaks_equip/find, trading_ui, weapon_challenges, weapons (`.png`) | tutorial popup |
| `videos/tutorial/` | `basics.mp4` | tutorial |
| `img/turf/` | `<mapId>-raised.png` (local `./`) | Turf Wars map |
| `img/` | `territory_map.webp`, `logo_10.webp`, `lottery_bg.webp`, `lottery_bg_easter.webp`, `lottery_bg_neutral.webp`, `email_optin_bg.webp`, `snowy_bg.webp`, `register_rewards_crd.png`, `client-promo.png`, `kr_icon_angled.svg`, `live-icon.png`, `ping.png` | menu, popups, HUD ping |
| `img/menu/` | `free_spin.png` | daily spin card |
| `img/backgrounds/` | `blackmarket_0.png` | exchange black market |
| `img/battlepass/` | `bg_window_5.png` | window 6 |
| `img/chals/` | `hed_3.png` | ranked challenges header |
| `img/events/` | `event_7.png` | dog tags HUD |
| `textures/melee/` | `melee_190_env.png` | env map for melee skin 190 (v10:92827) |
| `promo/` | `og_1200x630.jpg` | share card |
| `scares/` | `3.webp` | jumpscare (v7 had `1.mp4`, repo `ui/scares/`) |

**Unchanged templates** that v10 still uses (v7 has them, and the repo already resolves
most of them): `img/maps/map_<id>.png`, `img/modes/<id>.png`, `img/menu-bg/<id>.png`,
`img/levels/<n>.png`, `img/medals/medal_<i>.png`, `img/streaks/<i>.png`,
`img/streaks/package/<i>.png`, `img/perks/<id>.png`, `img/perks/zombie/<id>.png`,
`img/shop/<name>.png`, `img/shop/spins/*`, `img/flags/3x2/<cc>.svg`,
`img/flags/flag_0..4.png`, `img/props/prop_<n>.png`, `img/mouse/<n>.png`,
`img/backgrounds/gamecats/c_<n>.png`, `img/backgrounds/bundles/b_<n>`, `img/esports/{nack,kpc,cka}.png`,
`textures/classes/icon_<n>.png`, `textures/weapons/<icon>.png`, `textures/melee/icon_<id>`,
`textures/previews/weapons/<src>.png`, `textures/previews/cosmetics/<type>_<id>[_c<pat>|_<tex>]_s<n>.<ext>`,
`textures/previews/attach/attach_<id>.png`, `textures/reticles/reticle_<id>.<type>`,
`textures/scopes/scope_<id>.<type>`, `textures/sprays/<n>.png`, `textures/badges/badge_<id>[_tex]`,
`textures/<prefab>_0.png` + `models/<prefab>_0.obj` (same 40 prefab models as v7),
`sound/<name>`, `videos/video_<n>`.

Footnote, how the list was made:

```sh
grep -o -E "[\"'](\./|/)?(img|textures|videos|sound|models|scares|promo|css|fonts)/[A-Za-z0-9_./-]*" v10.js \
  | sed -E "s/^[\"']//;s#^\./##;s#^/##" | sort -u      # then comm -23 against the same grep over v7 src/ + public/*.html
```

## 8. Porting notes for the repo UI

- `client/src/ui/menu_ui.c` and `ui/index.html` follow the v7 static menu. v10 replaces
  the left column with the nav list in section 2 and the play area with 5 primary buttons
  (Quick Match, Ranked, Host, Find, Custom Games). This is the largest visible change.
- The Svelte popups (ranked, social, exchange, leaderboards, turf wars, guide) have no
  window ids. In the repo they fit best as new `ui/windows/*.html` fragments opened by a
  C popup helper (`ui_dialog.c`), with the CSS taken from each component's `'code'` string
  *(guess: the simplest mapping onto `ui_windows.c`)*.
- The repo already has `ranked_ui.c`, `clan_ui.c`, `social_ui.c`, `store_ui.c` and
  `battlepass_ui.c`, written for the v7 windows 27, 13, 22/5, 14 and 6. Their behaviour
  moves into the popups above.
- Asset fetches: every new file in section 7 must go through `ensure_asset_file()` on the
  web build. None of them are in `assets/` or `ui/img/` today.

### What's New, Guide, Community (ported: `client/src/ui/v10_news.c`)

- **What's New** = windows[34] (`showWindow(35)`: the nav item `#updateAd`, the footer's Changelog
  link) with v10's `gen` (line-height 1.8 notes, `img/backgrounds/whatsnew.png` header),
  `toggleLog`, `switchTab`, `viewAll`, over Krunker Box Office's own changelog
  `ui/windows/kn-versions.txt` (v10's `docs/versions.txt` format; `versions.txt`, Krunker's,
  only when a page tree has no `kn-versions.txt`). The newest update is the version
  (`config.gameVersion`), and `lastViewedUpdate` stores it alone (v10: `<version>_<build>`, which
  would bring What's New back with every deploy). `#updateAd` shows for a signed-in player who has
  not viewed it, with `" - v<version>"` (`_52131`, added to the static page by C); the window's
  What's New view hides it again, as v10's `gen` does. KN addition: the brain's news
  (`GET /social/news`, v10's hub News tab feed, `doc/backend.md` 6.6) as v10 `.news-item` cards
  above the notes, only when there are any.
- **Guide** = `openGuide(section)` -> `#genericPop.tutorialPopup` with `_66971`'s markup, its twelve
  sections (locked ones still open, as in v10; the level comes from the account) and the 27
  `img/guide/*.png` (pinned in `tools/ui-import/live_assets_v10.lock`, root `ui`). Natively the
  Basics video is its first frame (`ui/img/guide/kn_basics_poster.jpg`, from the pinned
  `videos/tutorial/basics.mp4`): the engine has no video decoder. The web build plays the video.
- **Community & Events** = `openCommunityMenu()` -> `#genericPop.communityPopup` with `_66842`'s
  markup: polls and events from `GET /creators/events` (`doc/backend.md` 6.6; "Loading events..."
  while it runs, "No upcoming events" without a brain or events), v10's date / LIVE / duration /
  tags / links rules, the five socials. Links go through `knCommunityLink()`: a `/?play=<map>`
  link plays the map here (`playMap`), the rest open like every external link
  (`ui_open_external`: the hub for `/social.html?...`, the browser for http(s)).
- Native-only differences: no `aspect-ratio` (the event banner gets v10's 1170:150 height in px,
  `ui/css/kn-v10-news.css`), no `-webkit-line-clamp` (descriptions are not cut), remote
  `banner_url` images are not loaded (no banner box then), Twitch's inline `<svg>` icon is drawn
  as an SVG image, and the event date has a plain space before AM/PM (Chrome: U+202F).
