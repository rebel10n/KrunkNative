# Krunker v10.0.2 ranked

Source: `~/jails/kru/src/Krunker_v10.0.3/deob/v10.0.2/game.js` (client bundle, `gameVersion` "10.0.1", `clientVersion`
"10.0.1r1"). Line numbers `v10:N` are its lines (they were taken from an identifier-renamed copy with
the same line layout, where `iîïí...` names became `_N`). v7.1.4 refs are
`/home/user/jails/kru/src/Krunker_v7.1.4/src/...`. The ranked *server* (matchmaker, MMR,
match creation) is not in the bundle: everything below is what the client and the shared game
code (the bundle also carries the server-side `Game` class) show. Guesses are marked (guess).

## Summary of change vs v7.1.4

v7.1.4 has only remnants (empty Ranked window `index.js:8171`, `eloK`/`eloPer`/`minRankedLevel`
in `config.js:184-189`, no queue code). v10 has a complete ranked product: a Svelte "Ranked"
modal with 5 tabs, a WebSocket queue on `gamefrontend.svc.krunker.io`, a TrueSkill-like rating
shown as `1000 + 100 * conservative_rating`, 14 rank tiers, monthly seasons, a 5-map monthly map
rotation, Ranked Points (RP) currency with milestones and an RP shop, region-scoped profiles and
leaderboards, ranked match history, forfeit votes, reconnection, anonymised names during warmup
and a ping-position key. The KrunkNative brain's `/mm/queue/*` (doc/backend.md 6.5) is a v3.1.1
shape; v10 needs a different contract (below).

## Constants (v10 config object, `v10:19477-20114`)

| key | v10 value | evidence | v7.1.4 |
| --- | --- | --- | --- |
| `RANKED_SEASON_START_YEAR` / `_MONTH` | 2025 / 8 | `v10:19483-19484`, `_7306 = 2025` `v10:16500` | new |
| `currentRankedSeason` / `getCurrentRankedSeason` | `_7307()`: 0 before 2025-08, else `12*(Y-2025) + (M-8+1)` (UTC) | `v10:16501-16507` | new |
| `currentSeason` | 10 (cosmetic / battle-pass season, not ranked) | cfgdiff | 7 (`config.js`) |
| `RANKED_ALWAYS_MAP` | 2 (Sandstorm) | `v10:19481`, `_7312 = 2` | new |
| `RANKED_ROTATION_SLOTS` | 4 | `v10:19482` | new |
| `rankedMapPoolInfo` | `{2 Sandstorm, 4 Undergrowth, 11 Industry, 14 Site, 17 Bureau, 39 Eterno Simulator, 42 Frontier}` | `v10:16509-16517` | new |
| `getRankedMaps` | see "Map pool" | `v10:16518-16533` | new |
| `rankedClasses` | `[0,1,2,3,4,5,6,8,12,13,15]` | `v10:16550` | same, `config.js:189` |
| `rankedTimeForFirstConnect` | 60000 | `v10:19915` | new |
| `voteForfeitTimer` | 60000 | `v10:20096` | new key (v7 hard-codes the vote, `data/game.js:4880`) |
| `voteForfeitDisconnectGrace` | 30000 (`_7343`) | `v10:20095` | new |
| `shutdownTimer` | 30000 (120000 when ranked) | `v10:191217` | new |
| `minRankedLevel` | removed; client hard-codes level 10 | `v10:111575` | 15 (`config.js:186`) |
| `eloK`, `eloPer`, `rankedCooldown`, `rankedDeduction`, `tmpRankTms` | removed (server-side now) | cfgdiff | `config.js:184-215` |
| `gameModesSavingHistory` | `[2]` (Hardpoint) | cfgdiff | new |
| `leaderQueries` | adds `player_ranked_matches` (formatter `"N matches"`) | `v10:18668`, `v10:19771` | not present |

## Ranks (`v10:97491-97590`)

Enum `Unranked 0, Bronze1-3 1-3, Silver1-3 4-6, Gold1-3 7-9, Platinum 10, Diamond 11, Master 12,
Kracked 13`. A threshold table (display MMR, i.e. `1000 + 100*conservative_rating`) is built at
`v10:97506-97507` but the client never reads it (rank comes from the server as `rank` /
`ranked_rank`).

| rank | min MMR | colour | icon (`assets.krunker.io/img/ranked/ranks/`) |
| --- | --- | --- | --- |
| Unranked | - | #AAAAAA | rank_unranked.svg |
| Bronze 1 / 2 / 3 | 0 / 200 / 400 | #CD7F32 | rank_bronze.svg |
| Silver 1 / 2 / 3 | 700 / 900 / 1100 | #C0C0C0 | rank_silver.svg |
| Gold 1 / 2 / 3 | 1300 / 1600 / 2000 | #FFD700 | rank_gold.svg |
| Platinum | 2300 | #41bdea | rank_platinum.svg |
| Diamond | 3000 | #B9F2FF | rank_diamond.svg |
| Master | 3300 | #FF8C00 | rank_master.svg |
| Kracked | 4700 | #FF323A | rank_kracked.svg |

- Rating: profile field `conservative_rating`; display `1000 + Math.round(100 * r)` (`v10:97591-97596`).
  `conservative_rating` plus `mu`/`sigma`-free naming suggests TrueSkill/OpenSkill `mu - k*sigma` (guess).
- Placement: Unranked shows "`6 - total_matches` games remaining until ranked" (`v10:112027`), so
  6 placement matches (also `minMatchesForPoints` default 6, `v10:112040`).
- Diamond and above show an extra badge block (`_34248`, `v10:112030`).
- Per-match change: server packet `lrmr` (LATEST_RANKED_MMR_CHANGE) `(mmrChange, latestRank)` sets
  stores `$latestMMRChange` / `$latestRank`; the end screen draws a rank card with "Rating +N"
  (`v10:218186`, `v10:195503-195560`). Reset to 0 on game init (`v10:218745`). v7: packet name
  exists in `packets.json:379`, no handler.
- Account field `ranked_rank` (`v10:112816`), shown on profiles (`v10:110117-110127`);
  setting `settings.gameprivacy.rank` "Show Ranked Badge" (same key in v7 i18n line 538).

## Seasons

| item | v10 | evidence |
| --- | --- | --- |
| numbering | monthly, season 1 = 2025-08 (UTC); season 0 = "Beta Season" | `v10:16501-16507`, `v10:112047` (`0 === _7307()`) |
| profile / leaderboard / history | all keyed by `season` and `region` | API table |
| season picker | store `_34189 = writable(currentRankedSeason)`, label "Season N" | `v10:98865`, `v10:111963-111977` |
| Beta Season rules | "1:1 conversion rate (100%)", "No milestones during Beta Season" | `v10:98930`, `v10:98934` |
| normal season | "100 matches = 100% conversion rate", "Milestones give rewards when reached", "Points awarded for your best region at end of month" | `v10:98935-98936` |

v7.1.4: no ranked season concept (store skins carry `rseas` 1-4 only). KrunkNative has
per-season ratings already (`ranked_ratings.season`, doc/backend.md 6.5); the v10 season number
is a calendar formula, not `accounts.current_season`.

## Map pool rotation (`getRankedMaps`, `v10:16518-16533`)

```
season s = currentRankedSeason
pool = keys(rankedMapPoolInfo) minus RANKED_ALWAYS_MAP, sorted ascending   // [4,11,14,17,39,42]
x = (2654435761 * (s + 1)) mod 2^32
for i in 0..3:  x = (1664525*x + 1013904223) mod 2^32
                j = i + x mod (len - i);  swap(pool[i], pool[j])           // partial Fisher-Yates
return sort_asc([2] + pool[0..3])                                           // 5 maps
```
Use exact 32-bit unsigned arithmetic (JS doubles stay exact here since `2654435761*(s+1)` < 2^53).
The Ranked home tab shows these 5 maps (`img/maps/map_<id>.png`). Note the queue WebSocket
still sends a fixed `preferredMaps = [0,2,4,11,14,17,42]` (`v10:98375`), not this list.

## Queue / matchmaking flow (`_34047` MatchmakingService, `v10:98366-98760`)

| step | detail | evidence |
| --- | --- | --- |
| gate | logged in (FRVR account) and level >= 10, else "Login Required" / "Level Required" popups | `v10:111566-111580` |
| already in a match | `GET api /ranked/me/connection` -> `{serverUrl}`; if set, show "You are already in a Ranked game." with rejoin (`quickJoinRankedMatch`) | `v10:98723-98733`, `v10:111539` |
| region | default region `config.regionIND[defaultRegion]` -> `regionIndexNames` {1 oc,2 as,3 eu,4 na,5 sa}; oc->as, sa->na; ranked regions are only `as` Asia, `eu` Europe, `na` North America (default eu, index 3) | `v10:98341-98365` |
| all regions | checkbox `s_rankedAllRegions` (localStorage) queues `as,eu,na` | `v10:111436`, `v10:98583` |
| connect | `wss://gamefrontend.svc.krunker.io/v1/matchmaking/queue?token=<FRVR access token>&maps=0,2,4,11,14,17,42&regions=eu[,as,na]` (beta: `gamefrontend.beta.krunker.io`) | `v10:98438`, `v10:16439` |
| server -> client | JSON `{type, payload}`; `QUEUE_STATUS {status:"QUEUED"}` -> Searching; `QUEUE_STATUS {status:"MATCHED", assignment:{connection:"host:port"}}`; `ERROR {code, message, payload}` | `v10:98458-98520` |
| error codes | `INTERNAL_ERROR`, `NO_RANK_DATA`, `MIN_LEVEL_REQUIREMENT`, `COOLDOWN` (payload.cooldown ms), `ALREADY_IN_MATCH` (payload.serverUrl) | `v10:98316-98322`, `v10:111609-111640` |
| states | `idle` / `searching` / `found`; status text "Searching for match... (m:ss)" / "Match found!"; `estimatedWaitTime` constant 120 | `v10:98310-98315`, `v10:98389` |
| matched | play `pop_3`, wait 1 s, `acceptMatch()` automatically (no accept dialog); ignore if already connected to another ranked game | `v10:98507-98530` |
| join | send game packet `joinedRank` (JOINED_RANK) on the current socket, then connect to `assignment.connection` ("host:port", default port 3000) | `v10:98681-98690`, `v10:97500-97505` |
| cancel | close the WebSocket (no explicit leave message) | `v10:98402-98416` |
| quick entry | menu "Ranked" button (`menuBtnRanked`, icon military_tech) opens the modal; play-card action `ranked-queue` starts the queue directly | `v10:147331-147336`, `v10:116880-116882` |

`joinedRank` makes the old server log the player out with reason 7 (`v10:24487`) and chat
`server.message.ranked` "{0} left for a ranked match"; `server.message.queued` "{0} queued for
ranked" exists too. v7.1.4 has the `joinedRank` name in `packets.json:136` only.

## In-match rules (shared `Game`, `v10:186900-191820`)

| rule | v10 | v7.1.4 |
| --- | --- | --- |
| flag | `INIT_GAME` gains arg 17 `isRanked` (after `isComp`), then `sessionId`, `roundId`, `gameTimer`, `krunkScripts` | `init` has no isRanked |
| game states | `IDLE 1, WARMUP 2, IN_PROGRESS 3, POST_MATCH 4, ENDED 5`; packet `gsc` GAME_STATE_CHANGED | `v10:186520-186525`; v7 none |
| first connect | `needAllTimer = 60000` when ranked (`rankedTimeForFirstConnect`), else afkTimeout/300000; if `lobbyPlayers` with a class `c` < `minPlayers`: ERROR "Ranked Game: The required number of players did not join the match. The game has been cancelled.", endGame(3) | `v10:189820`, `v10:191376-191400` |
| names | while `gameState <= WARMUP`, players are shown as `Player_<lobbyPlayerId>`; real names pushed via `SYNC_P_CON_DATA` at IN_PROGRESS | `v10:191768-191815` |
| reconnection | allowed while WARMUP..POST_MATCH; packet `prc` PLAYER_RECONNECTED | `v10:191797` |
| rosters | comp `config.teams` -> `accountTeams` (frvr ids or names, max 50 each), `lockedTeams` | `v10:187264-187271` (v7 had none) |
| streaks | no killstreaks in ranked; no class score saving | `v10:174076`, `v10:173631` |
| profile stats | ranked games do not add games/kills/wins to the account; instead `rankedPoints += clamp(floor(reward/2), 0, 60)` | `v10:188150-188165` |
| KR reward | x2 in ranked (`challMode` x1.5) | `v10:191171` |
| match history | `matchInfo {map, map_name, mode, is_competitive, is_ranked, date, duration, region, season, historyEntries, events}`; entry `{player_id, frvr_id, frvr_token, player_name, team, score, kills, deaths, assists, accuracy, headshots, damage_done, rounds_won, objective_score, play_time, kr, victory, ip}`; ranked keeps only rostered frvr ids; draw favours team 1 (Alpha) | `v10:186527-186710` |
| UI | invite button disabled, spectate button hidden, host button disabled, comp box title "Krunker Ranked" | `v10:219154-219156`, `v10:219431` |
| ping | key `markPosition` (i18n "Ping Position (Ranked Only)") raycasts 1000 units from camera and sends `mpos` [x,y,z]; server relays `mpos` -> `addPingIcon`; also in comp | `v10:170189-170199`, `v10:221125`, `v10:223179` |
| offers | bundle popups suppressed while ranked | `v10:126427`, `v10:208691` |

### Forfeit vote (`v10:186460-186490`, `v10:190925-191000`, `v10:191439`)

| item | v10 |
| --- | --- |
| eligible voters | same team, not `bypassUser`, disconnected for <= 30000 ms (`voteForfeitDisconnectGrace`) |
| state | `game.voteToForfeit {team, votes[names], num, req, timer}`; `forfeitInitiators[]` (one vote start per player per game: `commands.forfeit.limit`) |
| update | votes pruned to current eligible names; `req = eligible count`; passes when `num >= req` (unanimous); stalls when no eligible voters |
| timer | expires after `voteForfeitTimer` 60000 -> broadcast `vf` null |
| packet | `vf (initiator|null, num, req, isStart, initiatorName, ?)` to the team only |
| resolve | team gets `vf` null + chat `commands.forfeit.ended`, others `commands.forfeit.ended.other`; `prepareForfeit(otherTeam)` (unchanged from v7 `data/game.js:4435`) |
| messages | `commands.forfeit.next/wait/later/voted/limit` (same strings as v7 i18n 1362-1368) |

The chat command that starts the vote is server code not in the bundle (guess: `/forfeit`,
as the v7 strings imply).

## Ranked Points (RP), milestones, shop

| item | v10 | evidence |
| --- | --- | --- |
| currency | account `ranked_points` / array index 47 `rankedPoints`; wallet type 3 in purchases | `v10:112806`, `v10:112873`, `v10:125671` |
| earn | per ranked match `clamp(floor(reward/2),0,60)`; end-of-month conversion `floor(MMR * rate)` where `rate = 0` below 6 matches, else `min(matches/100, 1)` (Beta: 1); best region counts | `v10:188150`, `v10:112040-112045` |
| milestones | from profile response `milestones: [{matches, reward}]`; claim in order via `POST api /player-profile/me/claim-milestone/region/<regionIndex>`; "CLAIM +N RP" animates to the Shop menu item | `v10:98585`, `v10:98736-98760`, `v10:112060-112120` |
| shop | `GET gapi /ranked/shop` -> `{skins:[{id, skin_id, price, expires_at, max_purchase, purchase_count, restricted_by, min_mmr, max_mmr}]}`; `POST gapi /ranked/purchase {skin_id: id}` -> `{success}` | `v10:26378-26392`, `v10:111020-111060`, `v10:112333-112345` |
| ranked skins | store flag `ranked` -> noSale, noSpin, limited, noQuickSell, limT "Ranked", rarity colour `#ff5500` | `v10:103082`, `v10:102803`; v7 `data/store.js` same flags |

## HTTP API used by ranked

Bases (`v10:16439`): prod `api.krunker.io` (client `_10444`, `v10:27594`, Bearer FRVR access
token), `gapi.svc.krunker.io` (client `_10449`, `v10:27600`), `gamefrontend.svc.krunker.io` (WS);
beta `*.beta.krunker.io`; local `127.0.0.1:5080/5081`.

| method / path | host | response (fields used) |
| --- | --- | --- |
| GET `/player-profile/me/season/<s>[/region/<r>]` | api | `{profile:{rank, conservative_rating, wins, losses, total_kills, total_deaths, total_assists, total_score, total_objective_score, total_damage_done, total_time_played, total_accuracy, highest_kill_count, highest_damage_in_match}, milestones:[{matches,reward}]}`; client derives total_matches, averages, `kill_death_average`, `damage_per_minute`, `win_rate` (`v10:98337`) |
| GET `/leaderboards/rankings/season/<s>[/region/<r>]?limit=&offset=` | api | `{leaderboard:[{position, player_name, profile:{...}}], totalPages}` (page 1-based in UI, offset = page-1) |
| GET `/match-history/me/season/<s>/gameMode/<mode>/isRanked/<true|all>[/region/<r>]?limit=&offset=` | api | `{matchHistory:{matches, totalPages}}` |
| POST `/player-profile/me/claim-milestone/region/<r>` | api | `{success}` / `{error}` |
| GET `/ranked/me/connection` | api | `{serverUrl}` |
| GET `/ranked/shop`, POST `/ranked/purchase` | gapi | above |

`<r>` is the numeric `regionIndex` (1 oc, 2 as, 3 eu, 4 na, 5 sa) after the oc->as / sa->na fold.
Responses are cached 60 s by a request cache (`_33948`, `v10:97590-97650`); cache is cleared after
claiming.

## UI

| element | detail | evidence |
| --- | --- | --- |
| main menu | "Ranked" button `menuBtnRanked` between Quick Match and Host Game; i18n `menu.btn.ranked` | `v10:147331` |
| modal | `genericPopup(component, "rankedMenuModal")`; tabs `home` Home, `profile` Profile, `leaderboard` Leaderboard, `matchhistory` Match History, `shop` Shop | `v10:215664-215669`, `v10:111451-111467` |
| home | status banner "Ready to compete? Join the ranked queue!" (icon, colour #5ce05a), queue/cancel, all-regions toggle, region indicator, rank card (icon, name, rating, win rate, K/D), Ranked Progression bar (0-100 matches, milestones), map rotation strip, link to Ranked Challenges | `v10:111476`, `v10:111960-112250` |
| global status | `.ranked-matchmaking-status` banner shown while searching (green glow) | `v10:97415-97420` |
| challenges | Challenges window section "Ranked Challenges" (`challRankedHeadr`, `img/chals/hed_3.png`), `windows.chall.rankedNote` / `rankedEmpty`; `openRankedChallenges()` scrolls to it | `v10:214451-214590`, `v10:215670` |
| social | stat tab "Match History" (`socialStatTabs`), leaderboard `player_ranked_matches` | cfgdiff, `v10:139271` |
| v7 i18n kept | `ranked.join/progress/team/switch/spectate/login/sync/team.spawn/class.spawn`, `windows.ranked.*` | v7 `i18n/en.js:1317-1325` |
| v10 i18n added | `windows.chall.ranked`, `windows.chall.rankedEmpty`, `windows.chall.rankedNote`, `windows.controls.markPosition`, `server.message.ranked`, `server.message.queued` | `v10:197610` |

## Port notes

- Replace the v3.1.1-shaped `/mm/queue/*` HTTP poll with a WebSocket queue that speaks the
  `{type,payload}` protocol above (brain-side), or keep the brain routes and adapt only the client;
  the v10 contract is simpler (no parties, no ready/class, auto-accept).
- Ratings: store `mu`/`sigma` (guess: OpenSkill/TrueSkill) and expose `conservative_rating`; ranks
  come from the server. The threshold table above can be used server-side.
- Season = calendar month formula; the map pool function must match bit-for-bit.
- Mode: client only knows the map list; the match mode is server-chosen. `gameModesSavingHistory = [2]`
  and the comp host type (Hardpoint, `v10:16570`) suggest Hardpoint 5v5 (min 8, max 10) (guess).
- Game-side work is shared code: `isRanked` init arg, game states + `gsc`, first-connect cancel,
  anonymous names, reconnection, forfeit vote, RP reward, `lrmr`, `mpos`.
