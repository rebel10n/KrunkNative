# Krunker v10.0.3: the ten unverified constants

Checked 2026-10-22 against the official v10.0.3 client. The ten rows are the unverified rows of
`doc/v10/gameplay-1003-audit.md` (300-305, 314, 330, 332, 333); those rows now point here.

Result: no value in the ten differs from what the official client uses. Five match (crouch slow,
collision padding, ranked rules, map 0, INIT_GAME). Four are constants the 10.0.3 client does not
have and no code reads (slipping speed, terrain slide threshold, terrain slip decel, slipping jump
cooldown; the jump cooldown itself matches: 0 after a jump). One is a session placeholder, not a
constant. One audit statement was wrong about our code (row 332, isOfficial), and the audit is
corrected. No ruleset or movement code changed.

## Reference and method

- Official bundle: `/home/user/jails/kru/src/Krunker_v10.0.3/game.js` (minified; read only).
  Decoded copy `~/.cache/kn-v103/deob-10.0.3.js` (webcrack of the same file, same literals). "deob N"
  is its line N; "min byte N" is the UTF-8 byte offset in the minified file.
- v10.0.2 readable: `~/.cache/kn-v10/v10.js` ("v10 N"). v7.1.4: `/home/user/jails/kru/src/Krunker_v7.1.4/src/config.js`
  ("v7 N").
- Searches: every key name and literal of the ten rows in both 10.0.3 files (`slip`, `terrainSlide`,
  `terrainSlip`, `playerSlipping`, `crchSpd`, `crouchSlow`, `collPad`, `jpC`, `jumpCooldown`, `isRanked`,
  `saveClassScores`, `Burg`, `INIT_GAME`, `isOfficial`, `gameTimer`, `challMode`, and the literal
  `1.9` in movement code). Each reader was read in full: procInputs (deob 191600-191680, 192225-192260),
  the jump function (deob 195246-195260), the INIT handler (deob 256734-256870), the two INIT_GAME
  broadcasts (deob 221968, 261892-261893), START_GAME (deob 242075), and the ranked gates (deob 197792,
  198604, 221758-221764). The other places a key or literal appears were checked by search.
- Keys in the 10.0.3 config object are plain text in the minified file too, so a key that is absent
  from the decoded copy is absent from the bundle.

## The ten rows

| # | Audit row | KN value (file:line) | Krunker v10.0.3 | Result |
|---|---|---|---|---|
| 1 | Slipping speed | 0.3 (`constants_v10.c:35`) | no `slippingSpeed` key | not found (dead) |
| 2 | Terrain slide threshold | 1.9 (`constants_v10.c:40`) | no `terrainSlideThreshold` key | not found (dead) |
| 3 | Terrain slip decel | 0.99 (`constants_v10.c:41`) | no `terrainSlipDecel` key | not found (dead); the terrain decel that is read matches |
| 4 | Slipping jump cooldown | 0.5 s (`constants_v10.c:39`) | no `playerSlippingJumpCooldown` key; the jump sets the cooldown to 0 | not found (dead); behaviour matches |
| 5 | Crouch slow | 0.3 (`constants_v10.c:38`, used `player.c:1757`) | setting `crchSpd`, default 0.3 | match |
| 6 | Collision padding | 1.1 (`constants_v10.c:75`) | `collPad` = 1.1 | match |
| 7 | Ranked: no kill streaks, no class XP | `ruleset.c:37` `.no_streaks`, `.no_class_xp` | `!isRanked` on streaks and class scores | match |
| 8 | Map index 0 (Burg) | `krunker_maps.h:23` `X(0, "Burg", ...)` | map 0 is "Burg" | match |
| 9 | INIT_GAME arguments | `net-bridge/v10Translate.ts:1227-1250` | same positions and values | match (isOfficial corrected, see below) |
| 10 | Session placeholders gameTimer 0, challMode 0 | `net-bridge/v10Proxy.ts:644-645` | not a constant; live values from TIMER and ENTER | placeholder |

## Details

### 1. Slipping speed (`slipping_speed` 0.3)

- 10.0.3: no `slippingSpeed` key in the config object (deob 22425-22900, keys in alphabetical order); the only "slipp" text is the
  tip "Slippery Ice Effect" (deob 22907). No code reads a slipping speed.
- v7.1.4: `slippingSpeed = 0.0003` per ms (v7 2117), i.e. 0.3 per second in KN's units. No v7 game code
  reads it (`grep -rn slippingSpeed` in v7 `src/` finds only the export line).
- KN reads it only in the Strike export (`shared/src/rules/strike/strike_rules.c:93`). Left as is.

### 2. Terrain slide threshold (`terrain_slide_threshold` 1.9)

- 10.0.3: no `terrainSlideThreshold` key (the only `terrainSlide` key is `terrainSlideDecel`, deob 22897).
  The terrain test (deob 192234) is `y <= terrain height`, with no slope threshold. The literal 1.9 in
  movement code is absent (the 1.9 literals are a weapon inspect value and a HUD scale).
- v7.1.4: `terrainSlideThreshold = 1.9` (v7 2135), no reader.
- Left as is.

### 3. Terrain slip decel (`terrain_slip_decel` 0.99)

- 10.0.3: no `terrainSlipDecel` key (v7 2125: 0.99, no reader). The decel on terrain is
  `this.onTerrain ? 0.99` (deob 191631), which is KN's `terrain_decel` (`constants_v10.c:43`, the value
  that is read). That value matches.
- Left as is.

### 4. Slipping jump cooldown (`player_slipping_jump_cooldown` 0.5 s)

- 10.0.3: no `playerSlippingJumpCooldown` key (v7 2139: 500 ms, no reader). The jump function sets
  `jumpCooldown = config.jpC * 0` (deob 195251), so the cooldown after a jump is 0 whatever the
  "Jump Cooldown" setting is.
- KN: `player->jump_timer = 0` (`shared/src/world.c:415`), and the comment at `shared/src/player.c:1268`
  says the same. Behaviour matches.
- Left as is (the constant is unread in movement).

### 5. Crouch slow (`crouch_slow` 0.3): match

- 10.0.3: the custom-game setting "Crouch Speed", `varN: "crchSpd"`, `def: 0.3`, min 0, max 4 (deob
  20462; v10 17578 has the same default). The decel reads it: `accel *= crouchVal ? config.crchSpd : 1`
  (deob 191627).
- KN: `crchSpd` in `shared/src/game_config.c:222`, `.def = 0.3f`. `player.c:719` (`MOVEMENT_OR`) makes an
  unset knob fall back to 0.3, and `shared.h:694` documents 0 as 0.3.
- The audit's "no key found" was a miss: the key is the setting `crchSpd`.

### 6. Collision padding (`collision_padding` 1.1): match

- 10.0.3: `var iìiîíîì = 1.1` (deob 21171; min byte 2191892), read as the config key `collPad`
  (deob 22480; min byte 2224738). The uses match KN's: the side tests of the wall jump and the ramp
  side (deob 191195-191205, 191976-191982), and `collidesE` (deob 191838). KN: `player.c:910-1108`,
  `world.c:321`.
- v7.1.4: `collPad = 1.1` (v7 2112).

### 7. Ranked: no kill streaks, no class XP: match

- 10.0.3 streaks: the killer's streak counter is bumped only when
  `!mode.noStreaks && !config.noStreaks && !isRanked && !isBoosted` (deob 198604; the death streak is
  separate). KN: `player.c:2969` (`!rules_ranked_no_streaks(game)`), `ruleset.c:37` `.no_streaks = 1`.
- 10.0.3 class XP: `saveClassScores` runs only when `classScores && isPubType() && !isRanked`
  (deob 197792-197794). KN: `server/src/accounts.c:1200` (`rules_ranked_no_class_xp`), `ruleset.c:37`
  `.no_class_xp = 1`.
- The 10.0.2 cites (v10 174076, 173631) are the same code.

### 8. Map index 0 (Burg): match

- 10.0.3: the map filter list has `{value: 0, label: "Burg"}` (deob 160271). The map code treats index 0
  as Burg: a map at index 0 that is not named "Burg" is a custom map (deob 188614), and the thumbnail
  falls back to "noimg" for index 0 unless the name is "Burg" (deob 257421).
- KN: `shared/include/krunker_maps.h:23`, `X(0, "Burg", ...)`.
- Note: 10.0.3 picks a random map from `config.maps` when a game is started with no map (deob 219869).
  The bridge's default of 0 (`net-bridge/krunkerProxy.ts:901`) is a KN choice, not a 10.0.3 value. It
  is a real map index (Burg).

### 9. INIT_GAME arguments: match (isOfficial corrected)

Client handler (deob 256734 wrapper, 256748 handler, the packet's positions):

| index | 10.0.3 client reads | KN bridge (`buildInitArgs`) |
|---|---|---|
| 0 | mapIndex | `init.mapIndex` |
| 1 | modeIndex | `init.modeIndex` |
| 2 | eventIndex | `0` (create-game literal 0, deob 261892; `initNewGameAsync` default `eventIndex \|\| 0`, deob 221963) |
| 3-5 | round wins, team scores, host | `null` |
| 6 | config | `config` |
| 7-8 | customData, modURL | `null`, `0` |
| 9 | customMapData (`accType`: `customMapData ? accType : 1`, deob 256851) | `{data: mapText, ..., accType: 1}` |
| 10 | mapHashes | `null` |
| 11 | sync (disconnects unless `gor`, deob 256833) | `{gor: 1}` |
| 12 | networkData | `networkData` |
| 13 | the 4th argument of `init(..., false)` (deob 256860) | `true` (the create-game path, below) |
| 14 | isOfficial (`iiïìîii.isOfficial = !!...`, deob 256787) | `false` |
| 15 | isComp (deob 256852) | `false` |
| 16 | isRanked (deob 256853) | `ranked` |
| 17-18 | sessionId, roundId | `null` |
| 19 | gameTimer (analytics only, deob 257180-257185) | `null` |

- Two broadcasts exist in 10.0.3 and in 10.0.2. The create-game path (`init(..., 0, true, false)`, deob
  261892; `broadcast(INIT_GAME, ..., true, !!isOfficial, isComp, false, ...)` at deob 261893; v10 222269-222270)
  puts `true` at 13. The `initNewGameAsync` path (deob 221968; v10 191320) puts `false` at 13. The bridge
  follows the create-game path, which is what its `true` means.
- The values that change play match: eventIndex 0, accType 1 (the default when there is no customMapData),
  gor truthy (the check passes), isComp false, isRanked from the ranked flag.
- **isOfficial.** The audit row said "isOfficial true". The bridge sends `false` at index 14. The audit
  had read the `true` at index 13 (the init's 4th argument). `false` is the right value for KN: the
  client's `isOfficial` marks a krunker.io official game (the host button, the end-screen reward row
  (deob 257471, 257616), the match-history tab), and Box Office servers are not official krunker.io games
  (`Game.is_official` is set only by `client/src/net/krunker_net.c` on a krunker.io join; see
  `doc/v10/account-split.md`). No code change. `net-bridge/v10Proxy.test.ts` now asserts indices 14 and 15.
- **gameTimer (19).** The bridge sends `null`; the official server sends the game timer in ms. The client
  uses it only for the `game_session_start` analytics event (`game_seconds_left`). No gameplay effect, so
  left as is.

### 10. Session placeholders gameTimer 0, challMode 0: placeholder

- These are session fields, not 10.0.3 constants. TIMER overwrites `gameTimer` (`net-bridge/v10Proxy.ts:461`),
  and ENTER_GAME overwrites `challMode` (`net-bridge/v10Proxy.ts:593`).
- 10.0.3 START_GAME is `[gameTimer, team, spectating, challMode, moveLock, 1, null, classIndex]`
  (deob 242075), the order `buildStartGame` uses (`net-bridge/v10Translate.ts:804`). The 0 is the value
  before the first TIMER packet.
- Left as is.

## Changes

- No ruleset or movement code changed: none of the ten values differs from 10.0.3.
- `doc/v10/gameplay-1003-audit.md`: rows 300-305, 314, 330, 332 and 333 now give these results. Row 332's
  isOfficial is corrected to `false`.
- `net-bridge/v10Proxy.test.ts`: the INIT test asserts indices 14 (isOfficial, false) and 15 (isComp,
  false).
