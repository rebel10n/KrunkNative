# Challenges window: v10.0.3 structure and KrunkNative parity

The Challenges window (menu `#menuBtnChall`, `windows[32].openHome()` -> `showWindow(33)`) lists the
account's challenges by type. KrunkNative draws it in `client/src/ui/v10_rewards.c` (the live v10 page;
`account_ui.c`'s `gen_challenges` is the v7 page's copy and is never reached on the v10 page, because
`ui_window_generate` asks `v10_rewards_window_gen(33)` first).

Offsets: `deob` is `~/.cache/kn-v103/deob-10.0.3.js` (line numbers, the decoded bundle the repo cites);
`game.js` is `/home/user/jails/kru/src/Krunker_v10.0.3/game.js` (byte offsets of a greppable token, `grep -b -F`).
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

Order, as v10.0.3: Map Challenges header (`hed_1`) -> every map card -> "Complete all" cards (no map) ->
Weapon Challenges (`hed_2`, margin 30) -> weapon cards -> Nightmare (`hed_4`, margin 30) -> Ranked
(`challRankedHeadr`, `hed_3`, margin 30) -> ranked cards and note.

Differences from v10.0.3, all intentional and listed here:

1. **Every catalogue map is listed** (v10_rewards.c, the home loop). v10 draws the cards of the maps that have
   challenges. krunker.io's list has challenges for every map (v10.0.3: "a Complete all challenge for every map"),
   so on the official type the grid is v10's. On Box Office a map with no challenge shows `0/0` and is not
   greyed (`chalCardH` needs `total > 0`), so no map can go missing. The Box Office data gap is in section 4.
2. **No Loading and no error on either type.** A list that has not arrived (no `activeChallenges` yet, or the
   brain has not answered or cannot be reached) gives the full layout with empty counts, and the answer
   re-renders the open window (`account_ui.c` `a` / `ua` frames, and the existing `chg` / `chgL` handlers).
   "No active challenges." stays for a list that arrived empty, as in v10.
3. The weapon and map headers are complete only when the list has arrived (`list && done >= total`); with a
   list they are v10's rule (`done >= total`, so an empty type is complete, as in v10).

## 4. Box Office data (kn_brain)

The brain's list is its own seed (`services/db/migrations/sqlite/0107_progression.sql:54-80`, 26 rows): map
challenges for mapIndex 0, 2 and 10 only (Burg, Sandstorm, Kanji), 4 weapon rows, 2 nightmare rows, daily and
weekly rotation rows, no ranked rows and no "Complete all". So with the same client code, Box Office shows
every map (with `0/0` for the 43 maps the seed has no challenge for) but not the official set of challenges.

Making the Box Office set identical to the official one needs krunker.io's challenge definitions (ids, `DAT`,
`REW`, names, icons, the map / weapon / ranked / complete-all rows). They are server data, not in the client
bundle, and this change does not invent them. The data step is a new brain migration from that definition
set (all three dialects) and is not part of this change.

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

## 5. Tests

- `server/tests/krunker_challenges_e2e.py` (`make krunker-challenges-e2e`): the krunker.io stub with an account
  that carries `activeChallenges` (one challenge per map, Complete all, weapons, nightmare, ranked) and the
  Box Office brain. Checks: every map name on Box Office; the official list rows and maps, and no brain row
  on the official type; no "Loading" or error text on either type, including an official account without
  `activeChallenges` and a brain that does not answer.
