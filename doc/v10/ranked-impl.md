# Ranked and clan wars: Krunker v10.0.2 port

Evidence is from `~/jails/kru/src/Krunker_v10.0.3/deob/v10.0.2/game.js` (strings plus the readable control flow; the
byte offsets below are into that file). The ranked *server* (matchmaker, rating, match creation)
is not in the bundle. Where v10 leaves something server-side, KrunkNative's rule is described and
marked **(KN)**. The research notes are `doc/v10/ranked.md` and `doc/v10/clans.md`.

## 1. What v10 changes from v7.1.4

| | v7.1.4 (KN before) | v10.0.2 |
| --- | --- | --- |
| Queue | party queue, v3.1.1 ClientManager HTTP shapes, `/mm/queue/*` | solo queue: `MatchmakingService`, a WebSocket `gamefrontend/v1/matchmaking/queue?token&maps&regions` with `{type: QUEUE_STATUS\|ERROR, payload}`; states idle / searching / found; on MATCHED the client waits 1 s and joins `assignment.connection` (@4152000-4163000) |
| Regions | one queue region | per region (`as` / `eu` / `na`, regionIndex 2 / 3 / 4), an "All regions" checkbox (`queueAllRegions`, `s_rankedAllRegions`) |
| Level | minRankedLevel 15 | 10 ("You must be level 10 or higher to access Ranked Mode.") |
| Errors | `RankedCooldown`, `LevelTooLow` | `INTERNAL_ERROR`, `NO_RANK_DATA`, `MIN_LEVEL_REQUIREMENT`, `COOLDOWN {cooldown ms}`, `ALREADY_IN_MATCH {serverUrl}` (@4147338) |
| Rating | Elo (eloK 32, divisor 330), ranks every 120 | `conservative_rating`, display `1000 + round(100 * cr)` (@4121841); the client never computes it |
| Ranks | 8 numbered | 14: Unranked, Bronze 1-3, Silver 1-3, Gold 1-3, Platinum, Diamond, Master, Kracked; thresholds 0 / 200 / 400 / 700 / 900 / 1100 / 1300 / 1600 / 2000 / 2300 / 3000 / 3300 / 4700, colours and `img/ranked/ranks/rank_*.svg` icons (@4118000) |
| Placement | none | "N games remaining until ranked" while Unranked: 6 matches |
| Seasons | `accounts.current_season` (7) | monthly: `12*(Y-2025) + (M-8+1)` UTC, 0 (Beta) before 2025-08 (@601149) |
| Maps | the comp maps | `getRankedMaps(season)`: Sandstorm always, plus 4 of {Undergrowth 4, Industry 11, Site 14, Bureau 17, Eterno Simulator 39, Frontier 42} by a seeded LCG (@601149) |
| Rewards | none | Ranked Points: per match (`floor(reward/2)` capped 60, game.js endGame @9297902), match-count milestones (`claim-milestone`), the month-end conversion of the best region's rating (rate `min(matches/100, 1)`, 0 below 6 matches, Beta 1:1), an RP shop (`/ranked/shop`, `/ranked/purchase`, `minMMR` / `maxMMR` requirements) |
| Decay | none | "Your rating will decay after 7+ days of inactivity" |
| In game | needAll 50 s | needAll / `rankedTimeForFirstConnect` 60 s, `LATEST_RANKED_MMR_CHANGE` "lrmr" ("Rating +N" on the end screen), vote forfeit, reconnects, 2x KR, no streaks, no class XP |
| UI | window 27 | Svelte popup (rankedMenuModal): Home / Profile / Leaderboard / Match History / Shop, a `.ranked-matchmaking-status` banner, end-screen rank card |

## 2. Clan wars in v10

v10 has no scheduled clan-vs-clan matches. Its clan war is **Turf Wars** (`territorialWars`,
"Territory Wars", intro `TERRITORY_WARS_INTRO_DATE` 2026-03-17):

- The game's `territorialWars` tracker (@9203000) counts, on the server, a kill by a player with
  an account and a clan against a player not of that clan (`onPlayerDeath`), in pub games
  (`isPubType`) on maps [0, 2, 3] (Burg, Sandstorm, Subzero) and modes [0, 1] (FFA, TDM), never
  on custom maps. At the game end it sends `[{clan_name, player_id, map_id, kills, deaths}]` for
  `getDayNumber()` (days since the intro, UTC).
- Each day, per map: the clan with the most kills (top 20 contributors per clan) controls the map
  the next day, and its banner shows on the map's `wars_banner*` nodes (`WARS_CLAN` packet).
  The day's kills become a KR pool that the top 3 clans split 60 / 30 / 10
  (`territorialWarsRewardSplit`). Each clan shares its cut by its loot config (Clan Bank /
  Even Split / Top Weighted / Performance / Top N by Performance, clan cut %, min games).
- The UI is a popup (`#menuBtnTurfWars`) with tabs Overview (the territory map, the controller,
  today's kills, guaranteed / estimated reward, top clans), My Clan, Leaderboards (top
  performing clans / top grossing clans / top performing players) and How to Play. The routes
  are `/territorial-wars/*` and `/clans/:id/loot-config`.
- `mode.clanWar` survives in `players.setTeam` only (`team = account.clan || 1`, @8330326). No
  v10 mode sets it.

## 3. Port plan and what was built

| piece | where | notes |
| --- | --- | --- |
| rules | `shared/include/ranked.h`, `shared/src/ranked.c` | seasons, map pool (checked against an independent port of v10's JS), ranks / colours / icons, TrueSkill, decay, season seed, RP rules, regions |
| Turf rules | `shared/include/turf_wars.h`, `shared/src/turf_wars.c` | day number, eligibility, counted kills, the split, the loot modes |
| schema | `services/db/migrations/<dialect>/0205_ranked_v10.sql`, `0206_turf_wars.sql` | **additive**: new tables plus nullable / defaulted columns on `mm_ranked_matches` / `mm_ranked_match_players`; nothing dropped. 0205 seeds every v7 `ranked_ratings` history into the Beta season (0) profile of region na |
| brain procs | `services/brain/db_ranked.c`, `db_turf.c` | C on every driver (`.pg` NULL); the matcher and the season close take their own advisory lock in one transaction |
| brain routes | `services/brain/ranked.c`, `turf.c` | below |
| game server | `server/src/accounts.c` | ranked-result with v10 stats, the rating chat (v10's lrmr text), 60 s first connect; the Turf Wars tally (`GameHooks.kill`) and the `turf-kills` frame; the clan war host options |

### Match rules in the game server (in-match and first connect)

- A leave does not end the match (v10 `checkLeave`, v10:188210 only logs it). The roster player's
  slot is marked left, the team plays short, and the leave counts as an abandon at the result
  (the rating loss stays a KrunkNative rule).
- A roster player may come back into its slot while the lobby is open (v10 `allowsReconnection`,
  v10:191796; the kept lobby player with its team and class, v10:187185). It keeps its team and class
  and is not an abandon. Accounts outside the roster are refused ("Game in Progress").
- A ranked match is one round: the round clock's end ends the match (v10's round logic is for pub
  types, v10:191041, and a ranked match is hosted). The end screen lasts the ruleset's 120 s shutdown
  (v10 shutdownTimer 120000, v10:191216-191217), then the lobby returns to the public rotation.
- "In the match" is v10's players.list / activeCount: a roster player has spawned (pressed play, `ENT`), not
  only connected. The match starts when the whole roster is in (v10 waitTimers need activeCount >= minPlayers,
  and ranked minPlayers is the roster).
- The first connect (60 s, v10:189820): while the roster is not all in, the 60 s run. At the end, if at least
  as many roster players have a class as the roster has (v10:191377-191395, lobbyPlayers with `c`), every roster
  player not in yet is pushed in (v10 sends ENTER_GAME; KrunkNative runs the forced ENT, `net_resume_ent`) and has
  3 s (v10 needAllTimer 3000); the match starts if all are in then, else it is cancelled. Otherwise (fewer classed
  players) it is cancelled at once.
- A cancel (v10 endGame(3), v10:191376-191413) sends every connection the ERROR "Ranked Game: The required number
  of players did not join the match. The game has been cancelled."; at the first-connect check a connection with
  no class gets "Ranked Game: You failed to select a class. The game has been cancelled." instead, and the 3 s
  timeout sends the "did not join" text to everyone. The connections close, the match ends with no rating, and the
  brain aborts it and removes its queue rows (`ranked-cancel`).
- KrunkNative choice: no partial start. v10 cancels the match whenever the roster is not complete (after the
  push-in window); KrunkNative used to start with the players who were in.
- KrunkNative choice: the ping (`mpos`, NET_EXT_MARK_POSITION) is relayed to every other connection
  of a ranked or comp game, never echoed to the pinger (v10 draws no local marker either).
| client | `client/src/ui/ranked_ui.c`, `turf_ui.c`, `clan_ui.c` | window 27 in v10's modal layout; the Turf Wars window (`TURF_WINDOW` = KN 101, a menu button after Ranked); the clan window's Settings tab with Turf Wars Loot Distribution |

### Routes

| route | answer |
| --- | --- |
| `GET /mm/ranked/queue?token&regions=eu[,as,na]` | `{type: "QUEUE_STATUS", payload: {status: "QUEUED", clientKey, regions, season}}` or `{type: "ERROR", payload: {code, message, payload}}` (LOGIN_REQUIRED, MIN_LEVEL_REQUIREMENT, COOLDOWN `{cooldown}`, ALREADY_IN_MATCH `{serverUrl, assignment}`) |
| `GET /mm/ranked/poll?clientKey&wait=s` | long poll (at most 20 s): QUEUED `{queueSize, timeInQueue}` or MATCHED `{assignment: {connection "host:port", gameId, host, port, webUrl, clientId (this player's ticket), matchId, region}}`; 404 for an unknown key |
| `GET /mm/ranked/leave?clientKey` | `{left: true}` |
| `GET /mm/ranked/connection?token` | `{serverUrl, assignment}` of a live match (v10 `/ranked/me/connection`, rejoin) |
| `GET /mm/ranked/info` | `{season, maps: [{id, name, available}], regions, minLevel, teamSize, placementMatches}` |
| `GET /mm/ranked/profile?token&season&region` | `{profile: {rank, conservative_rating, mmr, wins, losses, ..., total_matches, placement_left, last_milestone_claimed}, milestones, rankedPoints, season, currentSeason, maps}` |
| `GET /mm/ranked/leaderboard?season&region&limit&offset` | `{leaderboard: [{position, rank, mmr, profile: {player_name, ...}}], totalPages}` (placed players only) |
| `GET /mm/ranked/history?token&season&region&limit&offset` | `{matchHistory: {matches: [{..., mmr_change, victory, map_name}], totalPages}}` |
| `GET\|POST /mm/ranked/claim-milestone?token&region` | `{success, reward, balance}` (the next milestone, in order, once) |
| `GET /mm/ranked/shop[?token]` | `{skins: [{id, skin_id, price, max_purchase, min_mmr, max_mmr, purchase_count}]}` (rows are added by staff: `ranked_shop`) |
| `GET\|POST /mm/ranked/purchase?token&id` | `{success, balance, skin}` (RP, then the skin into the inventory) |
| `/mm/queue/*`, `/queue/*` | 410 `{"status": "ClientOutdated"}` |
| `GET /api/territorial-wars/status` | `{data: [{map_id, map_name, clan_id, clan_name, clan_logo, kills, total_kills_today, guaranteed_reward, estimated_reward, leaderboard}]}` |
| `GET /api/territorial-wars/clan/:id[/players\|/earnings\|/payouts]` | v10's clan views (`:id` = clan id or tag) |
| `GET /api/territorial-wars/leaderboards/{top-performing-clans,top-grossing-clans,top-performing-players}` | `{data: {leaderboard}}` |
| `GET /api/territorial-wars/player/:id[/contributions\|/earnings]` | v10's player views (`:id` = account id or name) |
| `GET\|PUT\|POST /api/clans/:id/loot-config` | the loot config (PUT / POST: owner or Commander, `token`, body or `?config=`) |

Everything is on the brain's existing `/mm` and `/api` prefixes, which the edges already proxy
(play. / matchmaker. / coordinator.krunk.io, net-bridge `PROXIED_PREFIXES`), so it works over
HTTPS and from the web build with no new listener. The client has no database access; every
call goes through kn_brain.

### Rating (KN)

Two-team TrueSkill (Herbrich et al. 2007): mu0 25, sigma0 25/3, beta 25/6, tau 25/300, 10 %
draw probability. `conservative_rating = mu - 3 sigma`. With v10's thresholds applied to the
displayed MMR, a new player (cr 0 = MMR 1000) places at Silver 2 after 6 matches. An abandoner
is rated as a loss of its team whatever the result, keeps its sigma, loses 1 more mu (100 MMR)
and gets a 30 min queue ban (`brain.ranked.cooldown_s`). Idle decay: sigma grows by tau per day
idle past 7 days, capped at sigma0 (applied when the player's next match is rated). A new season
starts from a soft reset of the player's latest profile (mu halfway back to mu0, sigma at least
2/3 sigma0). A v7 Elo rating seeds `mu = 25 + min(elo / 60, 10)`. `players.player_elo` mirrors
the latest MMR (v7 profile field).

### Matchmaking (KN)

Every second, the brain holding the lock takes the queue per region. The longest-waiting player
anchors; players within `rating_window + window_growth * wait` MMR (at most `window_max`) fill
2 x `team_size`. Teams are a snake draft by mu (1-4 vs 2-3 ...). The map is chosen at random
from the season's pool that the map catalogue has; Eterno Simulator (39) and Frontier (42) are
not in KrunkNative's catalogue yet, so they are skipped and listed `available: false`. Hosting
uses the existing ranked `host` config (a 4v4 Hardpoint comp lobby; the comp lobby rules are
unchanged, doc/backend.md 8.6). If hosting fails, the players go back to the queue, held for 10 s.

### Ranked Points (KN where v10 is silent)

Per match `clamp(floor(reward / 2), 0, 60)` (v10 game.js). Milestones at 10 / 25 / 50 / 75 /
100 matches give 100 / 200 / 350 / 500 / 750 RP; the Beta season has none (v10's text). At a
season's end (the first tick of a brain in the next season), each account's best region
converts at `floor(MMR * min(matches, 100) / 100)` (0 below 6 matches). This happens once
(`ranked_season_payouts`). The Beta season (0) is never closed: on KrunkNative it only holds the
v7 ratings seeded by 0205, which would otherwise pay out on the first deploy (`ranked_season_points`
keeps v10's 1:1 Beta rate for completeness).

### Turf Wars (KN where v10 is silent)

The pool is `brain.turf.kr_per_kill` (1) KR per counted kill. A clan alone on a map gets only its
60 %; the unpaid shares are not redistributed. The clan-bank share and rounding remainders go to
`clans.clan_funds`. Each day is settled once by whichever brain sees the day finished first
(lock + `turf_days`). Map banners (`wars_banner` nodes, `WARS_CLAN`) are not ported: KrunkNative's
maps carry no such nodes yet.

### Clan war games (v10 `mode.clanWar`)

Host options of team modes: `knClanWar`, `knClanA`, `knClanB` (shown in Host Game > Teams). Team
1 is clan A and team 2 is clan B. An empty tag is claimed by the first clan that joins. Everyone
else (no clan, another clan, guests) only spectates, with the notice "Clan War: [A] vs [B]: only
members of these clans can play. Spectating." A clan war is a hosted custom game, so it never
counts for Turf Wars (v10's `isEligibleGame` = official public games). There is no challenge or
booking flow because v10 has none.

## 4. Deviations from v10 (intentional)

- The queue uses HTTP long-poll with v10's message shapes, not v10's WebSocket. It goes through
  the existing edge relays and the web build, and the native client has no WS client for it.
- The rating formula, matchmaking, milestones table, RP pool size and abandon rules are
  KrunkNative's (server-side in v10).
- Ranked is window slot 27 and Turf Wars a KN window (101), not Svelte popups.
- The clan Settings tab has only the loot distribution. v10's logo moderation, paint banner,
  MOTD, recruitment board and ownership transfer are not ported.
- Turf Wars draws v10's territory polygons as an SVG. v10's `territory_map.webp` backdrop and its
  `turf/<map>-raised.png` overlays are not shipped.
- Vote forfeit, reconnect into ranked, the ping marker and the lrmr packet itself are
  v10-gameplay's; this port sends the rating change as a chat line.
- A krunker.io account uses v10's own ranked services at krunker.io (client/src/net/krunker_ranked.c):
  the api / gapi requests with its Bearer token and v10's queue WebSocket. krunker.io wins when a
  KrunkNative account is signed in beside it (client/src/net/ranked_route.c, ranked_active_backend in
  ranked_ui.c). The paths and answers are v10's; the popup reshapes them into the brain's shape.
- A krunker.io MATCHED joins the match's game socket like v10's joinAssignment (ranked_ui.c join_kio_match,
  krunker_net.c krunker_net_join_match): JOINED_RANK goes on the open game socket when there is one (v10's
  `connected` check, v10:98686), then the socket at the assignment's connection (v10:97583, 222104-222135)
  as `ws(s)://host:port/ws?at=<the account's fresh token>` (no gameId / clientKey; production is wss with no
  port, the mirror keeps the port). A MATCHED while the open game socket is a ranked game on another host is
  ignored (v10:98528). REJOIN (/ranked/me/connection's serverUrl) joins the same way (v10:98729).
  From the menu no game socket is open, so no joinedRank leaves this client for a MATCHED (v10 does the same:
  it sends it only on a connected socket).
- The RP balance is the account row's rankedPoints, row[47] (v10:112873, v10:70129 / setData): the game
  server's "a" ACCOUNT_RESPONSE and "ua" UPDATE_ACCOUNT carry the row (krunker_link.c row_points), held for the
  session by krunker_account.c. It is unknown ("--") until a krunker.io game has signed the account in; v10
  never asks krunker.io for the balance, so /player/me is not called.
- Season and payout: the Ranked reads are v10's, with the current season (ranked_season_at, v10's _7307)
  and the region index: profile (player-profile/me/season/<s>/region/<r>, v10:25953), leaderboard
  (leaderboards/rankings/season/<s>/region/<r>?limit=50&offset=<page-1>, v10:25961), match history
  (match-history/me/season/<s>/gameMode/undefined/isRanked/true/region/<r>?limit=5&offset=<page-1>, v10:25923).
  v10's season picker only changes <s> ($selectedSeason, v10:34189); KrunkNative's popup has none.
  No client write path exists in v10 for the season close, the RP conversion, the rating and
  placement updates or the ranked result: the server owns them and the client only reads them.

## 5. Tests

- `shared/tests/comp_ranked_test.c`: seasons, the map pool, ranks, TrueSkill (symmetry, draws,
  upsets, abandons), decay, seeds, RP, regions, Turf rules and every loot mode.
- `services/brain/tests/ranked_test.py` (`ctest -R brain_ranked`, postgres + sqlite + redis): the
  live upgrade (a 0204 database with v7 ratings migrated to 0205), the queue on two brains, the
  assignment, rejoin, results, profiles, history, placement, the leaderboard, milestones,
  COOLDOWN, the gates, leave, a host failure, the season close and the RP shop.
- `services/brain/tests/turf_test.py` (`ctest -R brain_turf`): turf-kills, settlement, the split,
  loot configs and the routes.
- `server/tests/ranked_e2e.py`, `server/tests/turf_e2e.py`: a 4v4 on a real server_bin; Turf kills
  from a real public round; a hosted clan war's teams and its exclusion from Turf Wars.
- uidiff `kn-ranked-home`, `kn-turf` (KrunkNative-only references).
