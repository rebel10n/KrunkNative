# Stalker (v10.0.3): HUD, model, sounds, taunts

Krunker v10.0.3's Stalker mode (`stalk`, mode index 15) outside its rules. The rules (pickStalker's
health and knife, onKill, onMelee, camping) are in `shared/src/modes/mode_rules.c`, the stalker's moves
(leap, wall stick, coyote time, wall jumps off `notWallJ`) in `shared/src/player.c` (`movement_stalker`);
`doc/v10/gameplay.md` covers both. This page is the rest: everything a player sees and hears.
References are lines of `deob-10.0.3.js` (`/home/user/jails/kru/src/Krunker_v10.0.3/deob/game.deob.js`).

All of it applies in the v10 mode only (`GameModeConfig.stalker_v10`): Krunker Strike's v7.1.4
Stalker (`stalker_v7`) had none of it.

## Code

| piece | file |
|---|---|
| `isStalker`, noMovT / isHidden, `game_player_stalker_taunt`, `stalker_send_picked` | `shared/include/stalker.h`, `shared/src/stalker.c` |
| procInputs hook (outside replays) and the spawn reset | `shared/src/player.c` |
| STALKER_PICKED from pickStalker | `shared/src/modes/mode_rules.c` (`mode_trigger_pick_stalker`) |
| a hidden player left out of the state stream; `"stlkrt"` | `server/src/net.c` |
| HUD, popups, taunt keys | `client/src/ui/hud_stalker.c` (ticked from `hud_ui.c`; keys from `ui_controls.c`; `NET_EXT_STALKER_PICKED` from `net/game_events.c`) |
| model look, hiding a still remote stalker | `client/src/stalker_render.c`, `client/src/player_mesh.c`, `client/src/main.c` (around the world pass) |
| silent steps | `client/src/sound_events.c` |
| official lobbies | `client/src/net/krunker_link.c` (`"stlkrt"` out; `"stlkpd"` in was already the text popup) |
| tests | `shared/tests/v10_gameplay_test.c` (`test_stalker_client`) |

## Packets

| v10 | KrunkNative | direction |
|---|---|---|
| `STALKER_TAUNT` `"stlkrt"` i (:111022, sent :260109) | `NET_EXT_STALKER_TAUNT` (0x0a10), `NetStalkerTauntPacket {u8 index}` | C -> S |
| `STALKER_PICKED` `"stlkpd"` (:111363, broadcastEx :217949) | `NET_EXT_STALKER_PICKED` (0x0a11), no body, to everyone but the stalker | S -> C |
| `"asnd"` sid, `"staunt_" + i`, {vol 1, rate 1, dist 20, roll 1.1} (:198335) | `NET_PACKET_PLAYER_SOUND` add, loop 0 | S -> C |

## Behaviour

- **isStalker** (:190464-190466): `mode.teamOptions[team - 1] == "Stalker"`.
- **Hidden after 300 ms** (procInputs :191766-191779, not in replays): `noMovT` adds the frame's delta
  while the velocity is 0 and is reset by any velocity; `isHidden = isStalker && noMovT >= 300 ||
  forceHide`. Player.init resets both (:190674, :190729). The server leaves a hidden player out of
  every player's `"k"` (:222346, spectators included). v10's client hides every player on each `"k"`
  and shows the listed ones (players.hideAll, :258580-258588); KrunkNative streams one STATE per player,
  so the client applies the rule itself to a remote stalker: received velocity 0 and an unchanged
  received position for 300 ms hides it (official lobbies send no velocity, so the position is what
  decides there), the next STATE that moves shows it. A hidden stalker's meshes leave the world scene for
  the draw. The local player's `#hiddenMsg` ("You are hidden") follows its own isHidden.
- **Model** (`modelOverrides.Stalker`, :118195-118205, applied :194107-194181): noCosmetics (also the
  melee skin), every colour 0 (v10 tests `!== undefined`, so the zeros apply; v7.1.4's truthy test
  skipped them), `opacity: 0.1` with `transparent` on the head, body, legs and arms (first-person arms
  too, :194337-194566). The knife keeps an opaque material and takes the texture `melee/melee_0_5`
  (:194611). A model built for the other side of isStalker is rebuilt.
- **Popups** (textPopup :259252, pop_3 at 0.3): becoming the stalker (`"tm"` with isStalker,
  :258386-258392) shows "You are the stalker" / "Hunt your prey!" and resets the aim (resetAim
  :195475: zoom 1, the scope and dot reticles at 0); STALKER_PICKED shows the others "You are the
  Living" / "Hunt the stalker and survive the round!" (:255146-255148). The strings are v10 literals
  (not in its translation table). KrunkNative watches the local isStalker turn on rather than hooking
  the team packet.
- **HUD**: `#stalkerControlHolder` (Leap [aim], Stick To Wall [jump], Taunts [stalkerTaunt0..5],
  labels from updateKeybindUI :244244-244267) while we are the stalker; `#stalkerLivingInfoHolder`
  ("Keep moving or you'll get dizzy.") while our team is Living (spawn, :258175-258176); both hidden
  while spectating (:180751-180755). `#zPerksHolder` holds the leap item `img/stalker_leap.png`:
  plain while the leap is ready, `.stalkerLeapTimer` with `opacity: 1 - left / 3000` and the seconds
  left (`(Math.round(left / 100) / 10).toFixed(1) + "s"`) while it recharges (:261068-261080).
- **Taunts** (:260106-260110 / :198332-198342): the stalkerTaunt keys (1-6) send `"stlkrt"` i; the
  server plays `staunt_<i>` at the stalker for everyone, 10 s apart (lastTaunt, shared with the prop
  taunts). A one-shot player sound is not kept for joiners (:220429) and the client drops it once it
  played (updatePlayerSounds :220465-220467).
- **Sounds**: a stalker's footsteps, landing and slide are silent (`!isStalker`, :195189, :195226).
- **Throwing / aiming the knife** is off for the stalker (`!this.isStalker` in procInputs' aim check,
  :191450): movement's (`shared/src/player.c`), not ported here.

## Deliberate differences

- v10's taunt guard reads `p && p.active && i >= 0 && i < 6 && !p.lastTaunt || now - p.lastTaunt >= 0`
  (:198333): past the first taunt the active and index checks drop out by precedence. KrunkNative checks
  them on every taunt and takes taunts from the stalker only.
- `NetPlayerSoundPacket` has no dist / roll: the taunt plays with the client's default 3D distance model
  (25 / 1) instead of 20 / 1.1, at the stalker's position when it started (v10's follows the player).
- forceHide on a non-stalker: the server leaves the player out of the state stream as v10 does, but a
  KrunkNative client has no hideAll and keeps the player where it was last seen.

## Assets

`ui/img/stalker_leap.png`: byte copy of `https://krunker.io/img/stalker_leap.png` (128x128 PNG, fetched
2026-10-24). The taunt sounds `assets/sound/staunt_0..5.mp3` and `assets/textures/melee/melee_0_5.png`
were already in the tree.
