# Krunker v10.0.3: round wins outside best-of, Battle Pass points and the winner of a public match, Raid's keys

Rules that `doc/v10/gameplay-1003-audit.md` left open. Reference: `~/jails/kru/src/Krunker_v10.0.3/game.js` (read only). Offsets are in the decoded copy `~/jails/kru/src/Krunker_v10.0.3/deob/game.deob.js` (webcrack; it has no undecoded string-array calls left, so every reference to a name in it is listed here): "deob byte N (line L)" is the UTF-8 byte offset and line. "raw byte N" is the offset in the minified `game.js`. Krunker v7.1.4 (`/home/user/jails/kru/src/Krunker_v7.1.4/src/data/game.js`) is cited where it has the same code.

## 1. Team round wins outside best-of

**Result: KrunkNative matches 10.0.3 now** (`shared/src/modes/mode_round.c`, `mode_end_game`, the `else` of `if (mode->best_of)`).

10.0.3 `this.endGame = function (endIndex, ...)`, which starts at deob line 221572:

| Step | 10.0.3 | Offset |
|---|---|---|
| bestOf branch (only when `endIndex == null`): round winner = `getWinTeam(true)` over the round's team scores; teamScores reset | `if (this.bestOf && iîIîìïí == null)` | deob byte 10687149 (line 221580) |
| its round win | `teamRoundWins[w] = (teamRoundWins[w] \|\| 0) + 1` | deob byte 10687487 (line 221589) |
| its no-winner case with noDraws | `else if (this.needWinner && !this.endIndex)`: "Overtime +60 Seconds", `return` | deob line 221592-221595 |
| match won in bestOf | `teamRoundWins[getWinTeam(true)] >= gamRounds / 2` | deob byte 10687786 (line 221596) |
| **else branch (bestOf off, or any endGame that is not a plain round end)**: round winner = `getWinTeam(true)`; if nonzero, `teamRoundWins[w] += 1`. Nothing is reset, no match-won test | `} else { var w = this.getWinTeam(true); if (w !== 0) { ... this.teamRoundWins[w] = (...) + 1 } }` | deob byte 10687871 (line 221597); increment byte 10688117 (line 221603) |
| next round: `endIndex == null && roundCount > 0 && !matchWon`: everyone out, moveLock, `roundCount--`, `"rds"` broadcast with `teamRoundWins`, flip spawns, intermission (intermTmr, 10 s), `gameTimer = startedTimer`, `return` | `if (iîIîìïí == null && this.roundCount > 0 && !iîÎíïïì)` | deob byte 10688223 (line 221606); rds byte 10688669 (line 221616); `return` after `gameTimer = startedTimer` byte 10689788 (line 221644) |
| match end: `endIndex` stored; winner = `winCondition` if any, else `getWinTeam()` (teams with `teamScores`), else the first player | `this.winner = this.getWinTeam();` | deob byte 10690953 (line 221675) |
| no winner with noDraws: Overtime | `if (this.needWinner && !this.winner && !this.endIndex)` | deob byte 10691337 (line 221684) |
| `getWinTeam(useScores)`: scores = `useScores ? teamScores : (teamRoundWins \|\| teamScores)`; ties go to the later team (`>=`); 0 when the top score is 0, or tied with needWinner | `this.getWinTeam = function` | deob byte 10680850 (line 221435) |

What this means:

- **When a round win is counted.** Every round end that is not a forced end (`endIndex`) counts the round's winner, bestOf or not. In a non-bestOf match every round counts, so `teamRoundWins` is set after the first round.
- **What ends the match outside best-of.** The rounds run out: `roundCount` starts at `gamRounds - 1` and drops once per round end, so the last round ends the match. A `winCondition`, a forced end (`endIndex`), or a noDraws tie in round wins (overtime, 60 s) decide otherwise. There is no "match won" early exit outside bestOf.
- **Who wins the match.** `getWinTeam()` over `teamRoundWins` (it prefers them over `teamScores` once they exist). The team scores carry on from round to round outside bestOf (nothing resets them), so they are not the match score.
- **What the scoreboard shows.** The HUD round counters come from the `rds` packet (deob 221616: `teamRoundWins`). The end screen reads the same numbers: `getEndGameStats` (deob 215438-215453) takes `victory` and `rounds_won` from `teamRoundWins`, and `endData.tms = getTeamScores(true)` (deob 221824; `getTeamScores` at deob 220254) is `teamRoundWins` when present.

KrunkNative, before this change: `mode_end_game` counted round wins only under `best_of` (`shared/src/modes/mode_round.c`, the old lines 351-361), so outside best-of `has_round_wins` stayed 0 and the match winner came from the carried-over team scores.

KrunkNative, now: the same `else` branch (`shared/src/modes/mode_round.c:373-382`, `mode_win_team(game, 1)` = `getWinTeam(true)`). The HUD already reads `has_round_wins` from `rds` (`client/src/ui/hud_modes.c:329-333`), so it shows the counts without client changes.

Not changed (documented differences):

- Forced ends (`endGame(3)`, the ranked "failed to start" path at deob 222083-222103) also reach 10.0.3's else branch and count a round win. KrunkNative's ranked match is one round and the forced end does not go through `mode_end_game`, so it does not count one.
- The forfeit path: 10.0.3 `prepareForfeit` (deob 221463-221472) sets `teamRoundWins` and then calls `endGame()`, which outside best-of adds one more to the forced team. KrunkNative's forfeit (`shared/src/forfeit.c:155`) calls `game_end_round` directly and counts nothing. Not ported here.
- The end screen's team score list (`tms`) is sent as nil by KrunkNative (`server/bridge/v10_translate.c`, the `end` packet): unchanged.

## 2. Battle Pass points per public match

**Result: KrunkNative matches 10.0.3 now**, and the cause is different from the audit's wording. The audit said KrunkNative computes no match BPP. It did: `server/src/accounts.c` (old line 1159) sent `bpp = min(20, floor(score * rewardMlt * 0.01)) + 30 if played >= elapsed / 2` on every counted final commit. Two things differ from 10.0.3: the play term is a binary half-match bonus instead of the proportional term, and the grant was not limited to public matches.

The 10.0.3 block (once per match, in the match-end path: it comes after the next-round `return`, deob 221644):

```
if (this.isPubType() && this.endIndex == null) {            // deob byte 10692430 (line 221705)
  // per player (function iiÌîííi, line 221711):
  // valid = account && !account.hack && !kpdData            // deob byte 10693245 (line 221726)
  if (valid) {
    base = Math.min(20, Math.floor(score * (mode.rewardMlt || 1) * 0.01))   // lines 221756, 221767
    if (!isEmptyLobby && isPubType() && mode.id !== "raid") {              // line 221768
      gt = (mode.gameTime ?? config.gameTime * 60000) - Math.max(0, gameTimerP || 0)   // line 221769
      frac = Math.min(1, Math.floor(playTime / gt * 10) / 10)              // line 221770
      base += frac * 30                                                    // line 221771
    }
    if (account.BP && bp && bp.data) {                                     // line 221773
      requested = Math.round(base)
      award = bp.validateProgress(account.BP.bpp, requested).award        // line 221776
      account.BP.bpp += award                                              // the grant
    }
  }
}
```

Inputs:

- **playTime**: per player, milliseconds. `tickPlayer` adds the tick delta while `noMovTimer < 10000` (deob byte 197921, line 197921-197922). `noMovTimer` is set to 0 when the player fires (deob line 192373) and is never incremented, so `playTime` counts the player's whole stay. `playTime` starts at 0 when the player object is made (deob line 190354).
- **gameTime**: `mode.gameTime ?? config.gameTime * 60000` (ms). `gameTimerP` starts at the same value (deob byte 10614511, line 219974), goes down with `gameTimer` (deob byte 10714908, line 222133) and goes up in timed modes (Race). So `gameTime - max(0, gameTimerP)` is the clock time the match has run, capped at gameTime. The audit's "gameTime" in its formula means this expression.
- **Public only**: `isPubType()` = `!hasHost() || mimicPubs` (deob byte 215915-215917). Ranked and custom (hosted) matches never enter the block.
- **Raid**: no play term (`mode.id !== "raid"`, line 221768). The base still applies.

**The play term never applies in 10.0.3.** The condition at line 221768 is `!isEmptyLobby`. `isEmptyLobby` is set to `true` in the match init (deob byte 10607364, line 219806) and nothing in the bundle ever sets it to false. The decoded bundle has three references: the write at 219806, and the reads at 221681 (the winner override) and 221768 (the play term). The raw file has the same two reads (raw byte 10855087 and 10859911). Krunker v7.1.4 has the same code (`src/data/game.js:3389` init, `:4541` winner, `:4593` `if (!this.isEmptyLobby)` with the `playTime >= gameTime / 2` +30). So the official client gives a public match its score part only, and the play term is a dead branch in both versions. KrunkNative's binary +30 copied the condition without the flag.

Consequence: the 10.0.3 result for a public match is `Math.round(min(20, floor(score * (rewardMlt || 1) * 0.01)))`, with the award then capped by `validateProgress` (the daily cap and the pass total; the function itself is not in the bundle, so `bp_validate_progress` in `shared/src/season.c` is the existing port).

KrunkNative, now:

- `shared/include/season.h` and `shared/src/season.c`: `bp_match_points(const BpMatchInput *)` implements the whole block: 0 unless `pub_type`; the score part; the play term when `!empty_lobby && !raid`, with the same floor, `min(1, ...)`, `* 30` and `Math.round`. NaN (0 over a 0 clock) counts as 0 (JS would store NaN in the balance).
- `server/src/accounts.c` (`acc_add_progress`, lines 1158-1167): `empty_lobby = 1` (10.0.3 never clears the flag), `pub_type = game_is_pub_type(g)`, and `bpp` is sent only for a public match. A ranked or custom match sends no `bpp`, so the brain (`services/brain/db_season.c:251-280`, which already grants only when `bpp` is sent) gives it no match points.
- Test: `shared/tests/season_test.c` `test_match_points` (public: 15 with all or 60% of the clock played; the old code gave 45 for the same inputs; ranked and custom: 0; the play term's arithmetic with `empty_lobby = 0`, documented as the formula's reading, not 10.0.3's behaviour).

Open, not changed:

1. (Ported, section 3: the public-match winner override.)
2. KrunkNative's `playTime` is the time since the account's first spawn in the match (`match_since`, `server/src/accounts.c:2647`), not 10.0.3's per-player tick from join. The elapsed clock is the current round's `total - round_timer`, not the cumulative `gameTimerP`. Both only matter if the play term is ever switched on.
3. Challenge rewards (`REW.bpp`) are a separate rule and are unchanged.

## 3. The winner of a public match

**Result: KrunkNative matches 10.0.3 now** (decision: the user asked for v10 parity, 2026-10-24).

10.0.3 `endGame`, after every winner rule (endIndex 3 / 4, `winCondition`, `getWinTeam()`, `multiWinner`, the first player; deob line 221652-221680):

```
if (this.isPubType() && this.isEmptyLobby && !this.isSandbox && this.mode.id !== "raid" && this.players.list.length > 0) {
  this.winner = -1;                                             // deob line 221681-221683
}
```

`isEmptyLobby` is true for every match (section 2), so the rule is: every public-type match (no host, or a host with `mimicPubs`) that is not Raid and not the sandbox, with anyone in it, ends with `winner = -1`. What -1 does downstream:

| Where | 10.0.3 | Effect |
|---|---|---|
| noDraws overtime | `if (this.needWinner && !this.winner && !this.endIndex)` (line 221684) | -1 is truthy: no overtime |
| didWin | `p.team && p.team == this.winner \|\| p == this.winner \|\| multiWinner && winner.includes(p)` (line 221695) | false for everyone |
| challenges | `if (p.didWin) challenges.update("w")`; "gp" for everyone (lines 221696-221699) | nobody gets "w" |
| rewards | KR and BP do not read the winner, except `isOfficial.reward && didWin` (line 221763) | unchanged |
| END | `send(END_GAME, .., this.winner \|\| endIndex == 3 ? p.didWin : -1, endData, mode.teams && (winner \|\| endIndex == 3) ? winner : -1, ..)` (line 221896) | result false (not the draw's -1), winner team -1 |
| end screen | `didWin == -1 ? "dra" : didWin ? "vic" : "def"` (line 257370); the team trophy goes to `winTeam == 1 / 2` (line 257452) | everyone sees "def", no trophy |
| account | `updateAccounts`: `if (p.didWin) account.wins++` (line 217298-217300) | no win |
| next-map vote | `genMatchList` / `mts` (lines 221827-221850) | unchanged |

Raid keeps its winner because `raid.onEnd` (line 118390) reads `didWin` for its run challenges.

KrunkNative, now:

- `shared/src/modes/mode_round.c`: `mode_public_no_winner` (the condition, with `MODE_EMPTY_LOBBY` 1 for `isEmptyLobby`), `mode_pick_winner` returns `ModeWinner.no_one` for it (`shared/include/modes.h`), and `mode_winner_set` is `this.winner` truthy, so `mode_end_game`'s noDraws overtime does not start.
- `shared/src/game.c` `game_send_end`: `no_one` gives `didWin` 0 for every player and result 0; `mode_send_end` sends winner team -1. The server's `accounts.c` takes `round_result` from the END, so no "won" and no "w".
- `client/src/ui/hud_ui.c`: the team trophy comes from MODE_END's winner team (`hud_modes_end_winner_team`), not from our own result, so a public match shows none.
- The bridge (`server/bridge/v10_translate.c`) already sends winner -1 to official clients.
- `bot_chat.c` (KrunkNative only) treats `no_one` as a draw for the bots' lines.
- Tests: `shared/tests/modes_test.c` `test_public_winner` (public FFA / TDM defeats, no overtime, the sandbox, a hosted game, mimicPubs, Raid); the other mode tests run in the sandbox, which keeps the rules' winner. `shared/tests/match_test.c` round test: a public round's END results are 0 / 0.

## 4. Raid and Zombies keys, and the endGame order

10.0.3 Raid (deob line 118351-118457) and Zombies (line 119112-119240):

| Key | 10.0.3 | KrunkNative |
|---|---|---|
| `skipStats: ["h", "s", "crc", "step"]` (Raid line 118449, Zombies line 119126) | `incStat` skips the keys (line 217356) | `GameModeConfig.skip_stats`, `mode_skip_stat`, read by `game_inc_stat` |
| `noProfileStats` (Raid line 118448) | `updateAccounts`: no games / kills / deaths / wins (line 217283); `transferStats` skips `account.stats` (line 217374); `checkLeave` adds no game (line 217391); `challenges.update` skips `DAT.lt` challenges (line 202974, also in ranked) | `server/src/accounts.c` `acc_player_stats` (`profile`), `acc_add_progress` (no "stats", `ltOff`), `shared/src/progression.c` `match_ok` (`ChallengeMatch.lt_off`, from the commit's `ltOff` in `services/brain`) |
| `specMaxD: 25` (Raid line 118455) | `spect.maxD = mode.specMaxD \|\| 160` (line 257121), the wheel clamps `followDis` (line 235616-235622) | `GameModeConfig.spec_max_d`, `mode_spec_max_d`, `client/src/spectate.c` `spectate_scroll` |
| `update`: `runPeakPlayers` (line 118426) | the most non-bot players an active raider saw | `mode_update_raid` |
| `onEnd` (line 118390-118425) | time left: "time" (ms) for the newly timed; winners: "rnodmg", "r1hp", "rnogun" (on Bastion, map 43, only after "bthrone"), "rsolo", "rsolofull" | `mode_end_raid`; `runShots` (line 196342) from `player_shoot`, `runDmgTaken` (line 198005) from `player_change_health`, `runProg` (line 183656) from the Give Challenge Progress action, all reset in `mode_player_spawned` (Player.init, line 190648) |
| `onAIDeath` "aik" (Raid line 118440, Zombies line 119232) | a player's AI kill, with the held weapon | `mode_ai_kill_challenge` |
| Zombies `onPreEnd` "rnd" (line 119175) | the waves survived, before the wave score | `mode_zombies_on_pre_end` |

The endGame order matters for Raid: `winCondition` and `onEnd` read `gameTimerP`, the clock left, which a "skip" end (the End Round trigger, line 182870-182873) keeps. KrunkNative's `game_end_round` cleared `round_timer` before picking the winner, so a raid could never be won and its onEnd never ran. It is cleared after `mode_on_game_end` now, and the end order is computed once (10.0.3 sorts before onPreEnd, line 221647, and builds the end table from that order; a second sort put Zombies' end table, where everyone has the same score, back in join order).

Zombies' `onPreEnd` bot removal (`bots.remove` of every bot, persistent ones too, line 119168-119172) is ported: `mode_zombies_on_pre_end` calls `game_bots_remove_all`, and `game_end_round` rebuilds its sorted end order by uid after `mode_on_game_end` (players.remove splices the bots out of the sorted `players.list`, so the END / MODE_END tables have the players only). Test: `zombies_test` `test_end_bots`.

Not ported: `onEnd`'s best-wave call (`call(135, [4, account.id, zWave])` past wave 1 in a pub-type game, line 119178-119187). DB call 135 is the official database's; no 10.0.3 page reads its record (no leaderboard, profile or end screen shows a Zombies wave), v7.1.4's server replica has no `proc_135`, and kn_brain has no stat for it.

## 5. The host, the secondary level gate, the warmup, Raid maps

| Rule | 10.0.3 | KrunkNative |
|---|---|---|
| `game.host` | the host's socket id: `hasHost` (deob line 215912-215914), `pickSimon` (217896), KrunkScript `CONFIG.getHost` (211983-212002), INIT_GAME | `GameConfigState.host_uid` (`game_config_set_host`, `game_host_player`, `shared/src/game_config.c`). The server sets it every tick from `accounts_is_custom_host` (`server/src/accounts.c` `acc_sync_game_host`), so a host transfer, a sign-in change and the lobby going public reach it. None in the offline sandbox (10.0.3's local server has no host). `hasHost` / `isPubType` stay on `game_config_is_custom`; the client's host star comes from PLAYER_INFO as before. |
| `pickSimon` | the host when it is an active player, else a random active one (217891-217911) | `mode_trigger_pick_simon`; test `modes_test` `test_simon_host` |
| `CONFIG.getHost` | the host's player | `server/src/ks_server_host.c` `cfg_get_host`: `game_host_player`; the sandbox keeps its documented local player |
| secondary `minRec` | `req` (112377): in a custom game no level tail, else `account && account.level >= minRec`. Enforced by the server: `Player.updateSecIndex` (190934-190948) has no caller in the client bundle and makes a refused pick the Pistol (2); the sandbox's local server takes the pick as is (245363, 245878) | `game_weapon_level_ok` (`shared/src/game_config.c`); `server/src/net.c` applies it at every spawn (the account's level then, a guest 0, a dev any); the Select Secondary card and `ui_loadout_secondary` use it too (a custom game opens every level). Test: `match_test` `test_secondary_level` |
| `warmupReset` | `resetPosition` runs `resetAmmo(true)` (191058; 191010-191036): full magazines (mode `ammoLimit`), full reserves, no reload left | `mode_warmup_reset` refills the loadout and sends the LOADOUT (and the reserves with finite ammo). Test: `modes_test` `test_warmup_v10` |
| warmup / intermission gmsg | the warmup's 5 s "match starting in " (220013-220036) and the intermission (221624-221642) have `getSnd` (n for "00:05".."00:01": `cntd_<n>` at 0.2 and the big #timerText number) and `getPop` (`[mode.name, mode.sub \|\| "Destroy the Enemy"]` at "00:00"); gmsg sends them when the text changes (222038-222048; handler 258349-258368). "warmup ends " has neither | `ModeWaitTimer.countdown`; `mode_tick_wait` sends `mode_send_countdown` (NET_EXT_KN_OFFICIAL_COUNTDOWN, the client's existing countdown) and the TEXT_POPUP. A 5 s timer's first text is "00:04", so the warmup has no `cntd_5`, as in 10.0.3. Test: `modes_test` `test_warmup_v10` |
| end-screen trophy | `winTeam == 1 / 2` (257452) | `client/src/ui/hud_ui.c` `hud_end_results`: MODE_END's winner team (`hud_modes_end_winner_team`) |
| Raid host maps | `allowedDat.maps: [18, 29, 31, 35, 43]` (19501-19503) | `k_host_types[GAME_HOST_RAID]` (KN had 18, 29, 31). Test: `modes_test` `test_raid_maps_v10` (the host config, and Khepri / Bastion played as Raid: the map's config, its spawn, the 20 s wait, a won and a lost run, Bastion's throne gate) |
