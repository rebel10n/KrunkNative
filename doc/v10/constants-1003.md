# Krunker v10.0.3: the ten unverified constants

Checked 2026-10-22 against the official v10.0.3 client. The ten rows are the unverified rows of
`doc/v10/gameplay-1003-audit.md` (300-305, 314, 330, 332, 333); those rows now point here.

Result: no value in the ten differs from what the official client uses. Five match (crouch slow,
collision padding, ranked rules, map 0, INIT_GAME). Four are constants the 10.0.3 client does not
have and no code reads (slipping speed, terrain slide threshold, terrain slip decel, slipping jump
cooldown; the jump cooldown itself matches: 0 after a jump). One is a session placeholder, not a
constant. One audit statement was wrong about our code (row 332, isOfficial), and the audit is
corrected. No ruleset or movement code changed then; on 2026-10-24 the four dead constants were removed
from `GameConstants` (rows 1-4), and every constant of `constants_v10.c` now cites its `deob-10.0.3.js`
line and is checked against the bundle's config object by `weapons_v10_test` (`shared/tests/data/v10/config.json`).

## Reference and method

- Official bundle: `~/jails/kru/src/Krunker_v10.0.3/game.js` (minified; read only).
  Decoded copy `~/jails/kru/src/Krunker_v10.0.3/deob/game.deob.js` (webcrack of the same file, same literals). "deob N"
  is its line N; "min byte N" is the UTF-8 byte offset in the minified file.
- v10.0.2 (history only): `~/jails/kru/src/Krunker_v10.0.3/deob/v10.0.2/game.js` ("v10 N"). v7.1.4: `/home/user/jails/kru/src/Krunker_v7.1.4/src/config.js`
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
| 1 | Slipping speed | removed (was 0.3, `slipping_speed`) | no `slippingSpeed` key | not found (dead): removed 2026-10-24 |
| 2 | Terrain slide threshold | removed (was 1.9, `terrain_slide_threshold`) | no `terrainSlideThreshold` key | not found (dead): removed 2026-10-24 |
| 3 | Terrain slip decel | removed (was 0.99, `terrain_slip_decel`) | no `terrainSlipDecel` key | not found (dead): removed 2026-10-24; the terrain decel that is read matches |
| 4 | Slipping jump cooldown | removed (was 0.5 s, `player_slipping_jump_cooldown`) | no `playerSlippingJumpCooldown` key; the jump sets the cooldown to 0 | not found (dead): removed 2026-10-24; behaviour matches |
| 5 | Crouch slow | 0.3 (`constants_v10.c`, used `player.c:1757`) | setting `crchSpd`, default 0.3 | match |
| 6 | Collision padding | 1.1 (`constants_v10.c`) | `collPad` = 1.1 | match |
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
- KN read it only in the Strike export (`strike_rules.c`, Strike's config carries the v7 key). Removed
  2026-10-24 with rows 2-4: the field is gone from `GameConstants`, and `tools/strike/gen_rules.py` no
  longer lists the four keys among Strike's physics keys.

### 2. Terrain slide threshold (`terrain_slide_threshold` 1.9)

- 10.0.3: no `terrainSlideThreshold` key (the only `terrainSlide` key is `terrainSlideDecel`, deob 22897).
  The terrain test (deob 192234) is `y <= terrain height`, with no slope threshold. The literal 1.9 in
  movement code is absent (the 1.9 literals are a weapon inspect value and a HUD scale).
- v7.1.4: `terrainSlideThreshold = 1.9` (v7 2135), no reader.
- Removed 2026-10-24.

### 3. Terrain slip decel (`terrain_slip_decel` 0.99)

- 10.0.3: no `terrainSlipDecel` key (v7 2125: 0.99, no reader). The decel on terrain is
  `this.onTerrain ? 0.99` (deob 191631), which is KN's `terrain_decel` (`constants_v10.c`, the value
  that is read). That value matches.
- Removed 2026-10-24.

### 4. Slipping jump cooldown (`player_slipping_jump_cooldown` 0.5 s)

- 10.0.3: no `playerSlippingJumpCooldown` key (v7 2139: 500 ms, no reader). The jump function sets
  `jumpCooldown = config.jpC * 0` (deob 195251), so the cooldown after a jump is 0 whatever the
  "Jump Cooldown" setting is.
- KN: `player->jump_timer = 0` (`shared/src/world.c:415`), and the comment at `shared/src/player.c:1268`
  says the same. Behaviour matches.
- Removed 2026-10-24 (the constant was unread in movement).

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

## 2026-10-24: the whole constant set against the decoded bundle

The integer constants the 2026-10-22 pass could not read (string-array encoded bundle) were checked against
`~/jails/kru/src/Krunker_v10.0.3/deob/game.deob.js` ("deob-10.0.3.js:N"). `tools/kref/v10-fixtures.py` writes
the config object's scalar keys to `shared/tests/data/v10/config.json`, and `weapons_v10_test` checks
GameConstants and the limits below against it, so a later bundle or a stray edit shows up as a failure.

| Constant | 10.0.3 (deob-10.0.3.js) | KrunkNative | Result |
|---|---|---|---|
| GameConstants (all 71: 64 config keys, the delta pair, grA / arA / crchSpd, the billboard count; body_height derived) | config object :22367-23014; grA / arA / crchSpd defaults :20418, 20438, 20465; config.billboards :20134 | `constants_v10.c` (each line cites its key) | match |
| Delta floor / ceiling | `Math.max(1 * deltaMlt, Math.min(input[1], dltMx))`, a smaller delta becomes the floor (:191276-191280); dltMx `Math.round(1000 / 30)` (:19242) | `min_delta` 1 ms x deltaMlt (`Ruleset.delta_floor_scaled`), `max_delta` 33 ms | match |
| chatMaxLength / chatListLength | 70 / 35 (:22470, :22469; the list cap is read at :241757) | `NET_CHAT_MAX_CHARS` 70 (shared.h) | match (the HUD list cap is the client's) |
| chatInterval | 800 (:22468); no client reader (the client sends every line, :260042-260044) | `NET_CHAT_INTERVAL` 0.8 s, `net_chat_spaced` in io_ct (server/src/net.c) | was missing: wired (a line sooner than 800 ms after the sender's last one is dropped; "/" commands are not paced) |
| tracerMinDst / tracerMaxDst | 20 / 500 (:22951, :22950; players.shoot :196958, :196980) | `VFX_TRACER_MIN_DST` / `_MAX_DST` (client/src/vfx.c) | match |
| maxNameLength | 14 (:22725), no reader in the bundle (the account service's rule) | kn_brain `name_max` (default 16, `services/brain/accounts.c`) | differs: left to the brain owner (a registration rule, existing names) |
| stateHistory | 500 (:22878; storeState drops entries 500 ms old, :193261) | `GAME_STATE_HISTORY` 0.5 s | match |
| serverTickFrequency / serverTickRate | 30 / 1000 / 30 ms (:22840, :22841, :19241) | server.cfg `tick_rate` default 30 (240 max, a KN option) | match |
| serverSendFrequency / serverSyncFrequency | 10 / 1 (:22836, :22838) | `NET_STATE_SEND_HZ` 10, `GAME_LAG_COMP_SEND_RATE` 10, `NET_RECON_SEND_HZ` 1 | match |
| Map limits (name, welcome, objects, collidables, spawns, signs, sign text / font, wheels, leaderboards, AI, entities, archetypes, interfaces, tris, GUI, position range) | validateMap :216477-216697, per-object check :198858, config | `map_validate.h`, `Ruleset.entity_limit` / `archetype_limit` | match |
| anchorLimit (v7.1.4: 1000) | gone: addMapAnchor takes every anchor (:186497) | the validator's 1000-anchor warning | removed |
| botLimit | 16 (:22455; addBot stops at 16 pending bots, :187637) | `MAP_V7_BOT_LIMIT` | match |
| Trigger limits (triggerLimit, triggerActionLimit, interfaceIDMax, teamLimit, custValLimit, emitter volume / rate) | :22958, :22957, :22628, :22893, :22500, :22528-22534 | `triggers.h` | match |
| config.emitterSounds | 74 entries: v7.1.4's 73 + `scare_1` (:21278) | `g_trigger_emitter_sounds` | was 73: `scare_1` added (an "es" of 73 no longer falls back to 0); the client's SOUND_EMITTERs read it too now (they had v3.1.1's 51, rate floor 0.5, and skipped "esa") |
| config.networkSounds | 399 names (:21141, config key :22774), sent as an index (`indexOf(name)`) in "s" / "so" / "sso", the "6" kill sound and config.taunts' id | `g_network_sounds` (`network_sounds.h`, generated by `tools/krunker/extract_sounds.py`; `network_sounds_test`) | was v3.1.1's 51 (`g_trigger_network_sounds`, read by nothing): the full table, shared by the official link, the pickup / spray sends and the test |
| voiceChatMaxLength / voiceRate | 0.65 / 500 (:22994, :22995) | `VOICE_CHAT_MAX_LENGTH`, `VOICE_RATE_MS` | match |
| voiceChatInterval / voiceDelay | absent | were `VOICE_CHAT_INTERVAL_MS`, `VOICE_DELAY_MS` (read by nothing) | removed |
| Ranked: reward x2, no streaks, no class XP, needAll 60 s, shutdown 120 s | :221760-221761, :198604, :197794, :219964, :221814-221815 | `ruleset.h` / `ruleset.c` | match (citations updated in ruleset.h) |

config.networkSounds keeps one id space across versions (v3.1.1 51 names, v7.1.4 265, v10.0.3 399,
each a prefix of the next but v3.1.1's id 32), and no map field holds a network id: SOUND_EMITTER,
TRIGGER and AI "es" index config.emitterSounds (v3.1.1's editor picker lists emitterSounds; its runtime
read a hand-written BOT "es" as a network id, v7.1.4 and 10.0.3 read emitterSounds, as KN does), so
old maps load untranslated. A name the list lacks is id -1, which plays nothing: 10.0.3's melee
pickup ("melee_equip", :176880) and spray sounds past `spray_1` are silent, and so are KN's now.
