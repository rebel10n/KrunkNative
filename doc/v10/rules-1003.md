# Krunker v10.0.3: round wins outside best-of, and Battle Pass points per public match

Two rules that `doc/v10/gameplay-1003-audit.md` left open. Reference: `/home/user/jails/kru/src/Krunker_v10.0.3/game.js` (read only). Offsets are in the decoded copy `~/.cache/kn-v103/deob-10.0.3.js` (webcrack; it has no undecoded string-array calls left, so every reference to a name in it is listed here): "deob byte N (line L)" is the UTF-8 byte offset and line. "raw byte N" is the offset in the minified `game.js`. Krunker v7.1.4 (`/home/user/jails/kru/src/Krunker_v7.1.4/src/data/game.js`) is cited where it has the same code.

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

1. The winner override (deob 221681-221682, `this.winner = -1` for public, non-raid, non-sandbox matches with players, because `isEmptyLobby` is always true): KrunkNative gives public matches a winner. Porting it would change who wins and who gets "w" credit in public games. Needs a decision.
2. KrunkNative's `playTime` is the time since the account's first spawn in the match (`match_since`, `server/src/accounts.c:2647`), not 10.0.3's per-player tick from join. The elapsed clock is the current round's `total - round_timer`, not the cumulative `gameTimerP`. Both only matter if the play term is ever switched on.
3. Challenge rewards (`REW.bpp`) are a separate rule and are unchanged.
