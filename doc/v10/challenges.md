# Challenges window: v10.0.3 structure and KrunkNative parity

The Challenges window (menu `#menuBtnChall`, `windows[32].openHome()` -> `showWindow(33)`) lists the
account's challenges by type. KrunkNative draws it in `client/src/ui/v10_rewards.c` (the live v10 page;
`account_ui.c`'s `gen_challenges` is the v7 page's copy and is never reached on the v10 page, because
`ui_window_generate` asks `v10_rewards_window_gen(33)` first).

Offsets: `deob` is `~/jails/kru/src/Krunker_v10.0.3/deob/game.deob.js` (line numbers, the decoded bundle the repo cites);
`game.js` is `~/jails/kru/src/Krunker_v10.0.3/game.js` (byte offsets of a greppable token, `grep -b -F`).
Read-only references; nothing was copied from them.

## 1. v10.0.3 structure

| Part | Where | Notes |
|---|---|---|
| Nav entry `menuBtnChall`, label "Challenges" | deob 164338-164340 (game.js 15928) | `openHome`: login if no account, else `showWindow(33)` (deob ~250735) |
| `openChallengeCat(typ, ind)` / `closeChallengeCat()` | deob 250743 / 250729 | `typ` = `map`, `weapon` or `event`; the detail view is the same list filtered on `DAT.mp` / `DAT.wep` |
| `gen()` | deob 250752 | guest: "to complete challenges!" (250754); level < 1: requirement; no list: "No active challenges." (250757-250758) |
| `genChallengeCard(challenge, marginTop)` | deob 250686 (game.js 78332) | icon `img/chals/icon_<IC>.png`, boss border, done = grayscale + check, `TR` "left", `PV/val` bar or time, KR / jnk, 3 reward tiles (skins, title, RP) |
| Event Challenges | deob 22364 (`var iïìiìîî = []`), 250870-250880 | never filled in the bundle, so the section never shows (as in KN) |
| Map Challenges header `hed_1` | deob 250885 | `challHeadrI`, `chalCardH` when all done, count `map / map.length` |
| Map grid | deob 250890-250897 | ONLY maps with challenges (`if (counts["map_"+i])`), map order = `maps.getMaps()` order (mapIndex), name `capFirst`, picture `img/maps/map_<i>.png`, click `openChallengeCat('map', i)` |
| Map challenges with no map (`DAT.mp == null`, e.g. "Complete all") | deob 250899-250902 | one card each, `margin-top:20px` |
| Weapon Challenges `hed_2` | deob 250904-250905 | `margin-top:30px`, `chalCardH` when `weapon >= length` |
| Weapon grid | deob 250906-250915 | weapons in table order, only `type != 3 && !customOnly` and only weapons with challenges |
| Nightmare Challenges `hed_4` | deob 250918-250935 | cards sorted `(boss ? 2 : PT)` descending, `margin-top:20px` between; empty: "Check back later for new nightmare challanges!" (sic) |
| Ranked Challenges `#challRankedHeadr`, `hed_3` | deob 250944-250953 (game.js 173141) | `windows.chall.ranked`; same card sort; note `windows.chall.rankedNote` if any card, else `rankedEmpty` |
| Sort | deob 250919-250923 | stable, by `(boss ? 2 : PT)` descending, as v7's `Array.sort` |

Names: `capFirst` of the map / weapon name (deob 27251 `iïîïíîi`). Challenge cards: `N`, or the i18n
`chall.name.<ky>` (deob "updateNames").

## 2. The source of each row

- **The list.** There is no challenge request in v10.0.3. The list is `Account.activeChallenges`, which is
  slot 38 of the account row: `this.activeChallenges = iÌìiïìí[38]` (deob 129005; `[]` by default, 128950).
  It arrives with the login (`a`), the account row (`ua`) and live updates: the game socket's `chg`
  (UPDATE_CHALLENGES, deob 111248, game.js 221147), `uchp` (progress, 111249), `chgC` (completed, 111258)
  and `chlR` (rewards, 111175).
- **The map list.** Not in the bundle. `maps.getMaps()` = `GET /maps` (deob 30527-30528, game.js 1452196)
  fills `iïîíîìî` (deob 262626). Challenges name a map by its index in that list (`DAT.mp`, = mapIndex).
  Names therefore come from krunker.io's catalogue, not from a table in the client.

Per server type, KrunkNative:

| Server type | List source | Code |
|---|---|---|
| Official krunker.io (`ACCOUNT_BACKEND_KRUNKER`) | the krunker.io account's `activeChallenges` (`ua` slot 38, `a`), from `g_kio.obj` | `account_challenges()` (account_ui.c:2865), `kio_set_data` (account_ui.c:360-377) |
| Box Office (kn_brain) | the brain's `chgL` answer to `["chg"]` (`services/brain/progress.c`, `P_CHALLENGES` in `db_progress.c`) | `account_challenges()` (account_ui.c:2865), `account_challenges_request()` (account_ui.c:2873) |
| Map names | the v10.0.2 official list in `shared/src/map_catalog.c` (mapIndex 0..45, `krunker_maps.h`) | `map_catalog_official_index`, `map_catalog_at` |

Both server types go through the same `account_challenges()` call in `v10_rewards.c`; the brain's list is
never read on the official type (`g_chal` is only read by the v7 copy in `account_ui.c`).

## 3. KrunkNative's layout (v10_rewards.c)

Order, as v10.0.3: Map Challenges header (`hed_1`) -> a card per map with challenges -> "Complete all" cards (no map) ->
Weapon Challenges (`hed_2`, margin 30) -> weapon cards -> Nightmare (`hed_4`, margin 30) -> Ranked
(`challRankedHeadr`, `hed_3`, margin 30) -> ranked cards and note.

Differences from v10.0.3, all intentional and listed here:

1. **No Loading and no error on either type.** A list that has not arrived (no `activeChallenges` yet, or the
   brain has not answered or cannot be reached) gives the full layout with empty counts, and the answer
   re-renders the open window (`account_ui.c` `a` / `ua` frames, and the existing `chg` / `chgL` handlers).
   "No active challenges." stays for a list that arrived empty, as in v10.
2. The weapon and map headers are complete only when the list has arrived (`list && done >= total`); with a
   list they are v10's rule (`done >= total`, so an empty type is complete, as in v10).

## 4. Box Office data (kn_brain)

The brain's list is its own seed (`services/db/migrations/sqlite/0107_progression.sql:54-80`, 26 rows): map
challenges for mapIndex 0, 2 and 10 only (Burg, Sandstorm, Kanji), 4 weapon rows, 2 nightmare rows, daily and
weekly rotation rows, no ranked rows and no "Complete all". So with the same client code, Box Office shows
the cards of those three maps only (v10's filter, `if (counts["map_"+i])`, deob 250890-250897; until 2026-10-24 KN drew
every catalogue map with `0/0`), and not the official set of challenges.

Making the Box Office set identical to the official one needs krunker.io's challenge definitions (ids, `DAT`,
`REW`, names, icons, the map / weapon / ranked / complete-all rows). They are server data, not in the client
bundle, and nothing here invents them. The data step is the capture path in 4c.

### 4a. Searched for the official definitions (2026-10-22)

The official client has no challenge-definition route. The bundle (`deob-10.0.3.js`) lists every API path, and
none is a challenge list; challenges reach the client only in the signed-in account (`activeChallenges`, slot
38). Read-only GETs through the proxy (`http://127.0.0.1:8888`), raw bodies in `~/.cache/kn-official-challenges/`:

| Endpoint | HTTP | Note |
|---|---|---|
| `GET https://api.krunker.io/challenges` | 404 | "Cannot GET" |
| `GET https://api.krunker.io/challenges/list` | 404 | |
| `GET https://api.krunker.io/challenge` | 404 | |
| `GET https://api.krunker.io/ranked/shop` | 404 | |
| `GET https://api.krunker.io/creators/events` | 404 | |
| `GET https://api.krunker.io/skins` | 404 | |
| `GET https://api.krunker.io/maps` | 401 | needs a signed-in account |
| `GET https://api.krunker.io/player/me` | 401 | needs a signed-in account |

So the definitions are only in a signed-in account's `activeChallenges`. No migration is added: writing one would
invent the rows. Box Office keeps the brain's seed until the definitions can be read from a real account.

### 4b. No generator in the v10.0.3 bundle (checked 2026-10-22)

The official map, weapon and ranked rows cannot be generated from the bundle, so Box Office does not generate
them. What the bundle does carry (deob = `~/jails/kru/src/Krunker_v10.0.3/deob/game.deob.js`):

- Row shape and the check: `checkChallenge` 202974-203022 (`DAT.ky` string or array, `DAT.val`, `DAT.mp`,
  `DAT.wep`, `DAT.cls`, `DAT.uniq`, `DAT.sm`; `PT` 1 = complete; `PV` progress, `SV` start value).
- The "chall" meta key: `finalize` 203074-203135 counts the account's completed rows of the same `T` and filters,
  and fires the `chall` rows (`DAT.uniq`, `DAT.nem`).
- Names: `updateNames` 203295 (`t("chall.name." + DAT.ky, DAT.val, ...)`), with the English templates in the
  translation string (deob line 229241) for `hs ns ls tmk fb mk fk qs wb airk lns tk drk kstk w d score dmg bzk
  cc rk`, and `nwindows.chall.*` headers and notes. There is no `k` (kills) template and no "Complete all"
  string, so no template shows a per-map or per-weapon kill count, and nothing gives a goal, KR or icon per map.
- Reward shape: the `REW` keys `kr`, `jnk`, `bpp`, `rp`, `sk`, `title` (`genChallengeCard` 250686-250726).
- Layout: the headers, order and grids (section 1). Nothing in the bundle lists the maps or weapons a
  challenge is made for; the map list is `GET /maps` and the weapons come from the weapon table.

So "v10 generates per-map challenges from templates" is not true of the client, and a generator in Box Office
would invent names, goals and rewards. Box Office's list is the one the captured account carries (4c).

### 4c. Capture path (the official list, from a real account)

A signed-in krunker.io account receives its list in `activeChallenges` (slot 38). The native client writes it
when `KRUNKNATIVE_CHALLENGE_CAPTURE` is set (`account_ui.c`, `chal_capture`; unset, empty or `0` = off, nothing is
written):

    KRUNKNATIVE_CHALLENGE_CAPTURE=1 client_bin         # ~/.cache/krunknative/official-challenges-<UTC date>.json
    KRUNKNATIVE_CHALLENGE_CAPTURE=/path/cap.json client_bin   # any other value is the file's path

The file (`"format": 1`) holds `source`, `game`, `captured_at`, `activeChallenges` (the latest full list from
`a` / `ua`) and `frames`: every later `chg`, `uchp`, `chgC` and `chlR` frame (UPDATE_CHALLENGES[_PROGRESS],
CHALLENGE_COMPLETED, UPDATE_CHALLENGE_REWARDS; deob 111175, 111248-111258). No account id, name or token.
Sign-in is yours: the capture runs with your own account.

Convert it into a brain migration (all three dialects, the next number, same in each directory):

    python3 -I tools/brain/challenges_from_capture.py ~/.cache/krunknative/official-challenges-<date>.json
    python3 -I tools/brain/challenges_from_capture.py cap.json --stdout    # the SQLite text only, writes nothing

The migration retires (`active = 0`) every row of the Challenges window's types (map, weapon, nightmare, ranked,
event; daily / weekly only when the capture carries them), then upserts the captured rows as `active = 1` with
`challenge_id = 100000 + CI`, `ctype = T`, `dat = DAT`, `rew = REW`, `icon = IC`, `name = N`, `sort` = list order.
Per-account fields (`PV`, `PT`, `SV`, `TR`, `PER`) are dropped. Progress stays in `player_challenges`. A
re-capture is a new migration; a row it drops is retired. Test: `services/brain/tests/challenges_capture_test.py`.

### 4d. v10.0.7 cards, frames and rewards (2026-11-23)

Against `~/jails/kru/src/Krunker_v10.0.7/deob/game.deob.js` (deob-10.0.7.js):

- **Progress.** Both card kinds (`genChallengeCard` 221427-221458 in `v10_rewards.c`, `genDailyCard`
  219232-219243 in `battlepass_ui.c`) draw `displayProgress / displayGoal` (180768-180806, `account_ui.c`
  `account_challenge_progress` / `account_challenge_goal`): a `DAT.bits` row counts the set bits of `PV` and of
  `DAT.val`; a `DAT.lt` row shows the larger of `PV` and the lifetime stat (kills, wins, `stats.n`). The bar is
  `Math.min(100, progress / goal * 100)`.
- **Reward tiles.** `REW.sk` (array, number or comma list; falsy = none), then each `REW.title` (the title's colour
  and name, a click opens the Titles window), then `REW.rp` in `#4caf50` with `commaNumber` (15601, 23949).
- **Frames.** `chg` sets `PV` / `PT` and redraws window 33; `uchp` sets `PV` and `PT = 1` once `PV >= DAT.val`, no
  redraw (225195-225215). `chgC [index, midGame, REW]` (225216-225229): mid-match, the chat line "Finish the game
  to complete: [N] challenge" and the reward skins; at the end, the "Completed!" card. `chlR [skins, funds, junk,
  jnk, kr, titles, rp, rankedPoints]` (215154-215197): the balances, the skins, each title into `account.titles`
  with its "Title unlocked!" card, the ranked points when points were won.
- **Box Office.** kn_brain's challenge finalize (`db_progress.c`, v10's 180855-180902) now credits `REW.jnk` (the
  Junk wallet), `REW.title` (`player_titles`, source `challenge`) and `REW.rp` (`ranked_points`) besides KR, skins
  and `REW.bpp`, and `chlR` carries all eight fields (`accounts.c`). Mid-match, as v10's `challenges.update`
  (180731-180732): server_bin reads the account's active list from kn_brain (internal op `challenges`, the menu's
  `chgL` order) at the join and at each round start, runs the commit's rule over the round's tallies after every
  event (`challenge_progress`, `challenge_reached_mid_match`, `challenge_progress_reported`, shared/src/progression.c)
  and sends the event's `challenge_reached {id, cis, prog}`: the CIs it completed, in list order, and its
  UPDATE_CHALLENGES_PROGRESS list `[CI, progVal, ...]` (180733-180741: progress short of the target or landing on it;
  a set / max key such as `kstk` only when it completes). kn_brain pushes `["chgC", index, true, REW]` for each
  completed one, then `["uchp", prog]`, to the account's social sockets: the chat line and REW's skins in the match
  summary, then PV and PT = 1 once PV >= `DAT.val` (225206-225215; until 2026-11-23 Box Office sent no `uchp`, so a
  challenge stayed unmarked until the match end). kn_brain still decides and pays at the commit
  (`db_progress.c`), whose `chgC` (the list form) gives the "Completed!" cards; the client adds a challenge's
  skins once (the mid-match ones are marked, `account_ui.c`) and requests the list at the login, as v10's account
  carries `activeChallenges`. Migrations 0397 and 0409 set the daily rows'
  (15-22) `REW` to krunker.io's values. On 2026-11-25 the Battle Pass Challenges tab of a signed-in krunker.io
  account showed five dailies, each +50 BPP: "Get 15 Headshots" and "Get 3 Headshots on SkyTemple" (+0 KR, +0.25
  junk), "Play 1 Burg Match" and "Play 1 SkyTemple Match" (+0 KR, +0.1 junk), "Get a 20 killstreak on Burg" (+70 KR,
  +0 junk). So every daily gives 50 BPP (0409 moves the spray and assist rows from 0397's 25), row 15 ("Get 15
  Headshots") and row 18 (5 headshots with one weapon) give 0 KR and 0.25 junk, and the rows with no official
  counterpart in that read keep their KR. Migration 0410 adds the other four as rows 27-30 (their DAT.mp,
  "gp" and "kstk" keys, icons 4 / 2 / 7, the killstreak's Wood skin 3237) and gives row 15 krunker.io's name and icon
  2, and the brain now hands each account five dailies a day (`CHALLENGES_DAILY`, `db_progress.c`), as krunker.io
  listed five. For a krunker.io account KrunkNative draws the `REW` krunker.io sends (`battlepass_ui.c`
  `gen_daily_card` over the account's `activeChallenges`), so 0397 and 0409 only set Box Office's values.
- **Weekly.** v10.0.7's Battle Pass Challenges tab draws the dailies only; KrunkNative keeps its weekly section
  there, since Box Office's weekly rows (23-26) are drawn nowhere else.

## 5. Tests

- `services/brain/tests/challenges_capture_test.py` (run by ctest with the brain tests): the tool on a fixture
  capture (invented rows, not official data) into a scratch migrations tree; the SQLite file applied to a
  `challenges` table; the re-capture, the PostgreSQL / MySQL upserts, the escaping and the refusals.
- `server/tests/krunker_challenges_e2e.py` (`make krunker-challenges-e2e`): T5 converts the stub's activeChallenges
  with the tool into a migration (a copy of the migrations tree), runs kn_brain on it, and checks that Box Office's
  active map and weapon rows equal the stub's (names, types, DAT, REW, icon) and the window shows the stub's
  no-map, nightmare and ranked rows. The stub is a stand-in for an official capture, so T5 checks the pipeline.
- `server/tests/krunker_challenges_e2e.py` (`make krunker-challenges-e2e`): the krunker.io stub with an account
  that carries `activeChallenges` (one challenge per map, Complete all, weapons, nightmare, ranked) and the
  Box Office brain. Checks: every map name on Box Office; the official list rows and maps, and no brain row
  on the official type; no "Loading" or error text on either type, including an official account without
  `activeChallenges` and a brain that does not answer.
- `client/tests/chal_card_e2e.py` (ctest `chal_card_e2e`): the Challenges window and the Battle Pass Challenges tab
  on their `--render-scenario` fixtures with a title + RP reward, a `DAT.bits` row and progress past the goal.
- `services/brain/tests/progress_test.py` (ctest `brain_progress`): the dailies carry `REW.bpp`; a challenge with
  `jnk`, `rp` and a title sends the eight-field `chlR`; `challenge_reached` pushes each completion's `chgC`, then
  `uchp`, and a progress-only event `uchp` alone.
- `server/tests/challenge_mid_match_e2e.py` (ctest `challenge_mid_match_e2e`): kn_brain, a brain-linked server_bin on
  Burg FFA and a signed-in client_bin. The account's seeded daily 16 ("Get 3000 score", kr 5, bpp 50), the seeded
  "Get 25 kills on Burg" and a test row with a skin are completed mid-match by killing bots: `chgC [index, true,
  REW]` with each one's index in the account's list, `uchp` at the targets, the chat line, PT mid-match on the
  client, nothing credited before the round end; the round end credits the KR once (one `challenge` transaction),
  the skin once and the daily's 50 BPP (`bpEnd` cbp), and the end screen shows "Completed!"; after a rejoin no
  `chgC` and no credit again.
