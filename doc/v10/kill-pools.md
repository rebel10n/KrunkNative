# Event kill pools and the v10.0.3 kill streaks

What Krunker v10.0.3 does with its kill streaks (the Bombardment and the Refill above all) and with the
live events' kill pools, and how Krunker Box Office plays them. Line numbers are the decoded bundle's,
`deob-10.0.3.js:<line>` (`~/jails/kru/src/Krunker_v10.0.3/deob/game.deob.js`).

## 1. The streak table (deob-10.0.3.js:203338-203523)

| index | name | slot | kills | flags | activate | KrunkNative |
|---|---|---|---|---|---|---|
| 0 | Nuke | 2 | 25 | reqAccount | `!nukeTimer`: incStat "n", 10 s ("n" 0, then "n" 1 kills every enemy spawned), :203344-203386 | `game_start_nuke`, `game.c` |
| 1 | Airdrop | 1 | 12 | noCustoms | `!isCustom`, a drop 500 above the caller when the sky is clear, :203393-203395 | `airdrop_streak_activate` |
| 2 | Refill | 0 | 5 | **devOnly** ("Dev Streak") | `!canThrow && now - lastThrowTime >= 5000`: refillPlayer (the knife back), :203402-203404 | `streak_refill`, `effects.c` |
| 3 | Bombardment | 2 | 20 | none | `!bombardmentTimer && !bombardmentBombs.length`: start, :203411-203412 | `game_start_bombardment`, `effects.c` |
| 4 | Slimer | 0 | 7 | none | the Slimer (weapon 23) unless holding equipment | `player_give_weapon` |
| 5 | Juggernaut | 2 | 30 | none | setJuggernaut unless holding equipment | `player_set_juggernaut` |
| 6 | War Machine | 1 | 15 | none | the War Machine (weapon 26) unless holding equipment | `player_give_weapon` |

`shared/src/effects.c` `g_streaks` carries it; the client's Select Streaks window keeps a copy
(`client/src/ui/ui_loadout.c` `STREAKS`), the tooltips the descriptions (`ui_tooltip.c`).

### devOnly: what v10 intended for the Bombardment

v7.1.4's `streaks.js` had the Bombardment `devOnly: true`, next to the Refill. v10 removed it: the
10.0.3 entry (:203406-203496) has no `devOnly`, a player description ("Deploys a Bomber that carpet
bombs the entire map"), and a complete server side (calcPos, start, update, end, reset, onCollision,
onKill, projectiles.initBombs, stored[2], the "bbm" / "bmb" packets and their client handlers). It is a
normal slot 3 pick at 20 kills for everyone. The Refill stays devOnly ("Dev Streak").

devOnly does two things, both on the account's `isDev`:

- the Select Streaks window lists a devOnly streak only for a dev (:251688-251689);
- players.updateStreaks earns it only for a dev (`!devOnly || player.account && player.account.isDev`,
  :198375). The pick itself is kept for anyone (maxPossibleStreak counts it, :242055-242060).

KrunkNative: `player_update_streaks` gates on `Player.dev` (the server's `net_client_is_dev` at every
spawn); the loadout window on `AccountInfo.is_dev`. Before this port both streaks were refused to
everyone and neither could be activated.

The other gates are unchanged from v7: `mode.disabledStreaks` ([4, 5, 6] in the modes that set it),
`mode.noStreaks` / `config.noStreaks`, no streak counting in ranked (`!isRanked`, :198604), noCustoms
(Airdrop) and reqAccount (Nuke, which every 10.0.3 player meets). `players.isBoosted` (:198477) is
`!!(killer & sockets[killer.id])`, a bitwise and of an object, so it is always false: no boost check.

### The Bombardment (deob-10.0.3.js:203406-203496, :206749-206775, :207081-207109, :216994-217029)

1. **start**: bombardmentTimer 500 ms, the caller is bombardmentPlayer, calcPos, "bbm" 0 to everyone
   (the client plays `adrop_0`, :258062-258066).
2. **calcPos**: the bounds of the map's spawns, starting from 0 (the origin is always inside). Cells of a
   sixth of the bounds; the grid starts two cells before the low x / z edges and runs 10 x 10 cells
   (two out on the low side, one on the high). Each bomb: the cell corner + randFloat(-15, 15) on x and
   z, the highest spawn + randFloat(250, 500) on y.
3. **update** (not while the end screen runs): every 500 ms one column of 10 bombs
   (projectiles.initBombs), 500 ms after the last column the end ("bbm" 1, which the client ignores).
   100 bombs over 5 s.
4. **a bomb** is projectiles.stored[2]: streak_3 model, spd 0.4 x pjSp (randFloat(0.3, 0.7), to 0.001),
   straight down (yD -PI / 2), no gravity, range 1500, dmg 150, explode 150 with eDmg 150, dontPred,
   info {stk: 3}. Sent as one "bmb" [x, y, z, sid, {pjSp}] list per wave; the client draws each bomb
   with a trail (physObj trails 1, :259104-259109 -> addTrail(physObj, 1) when Bullet Trails is on,
   :234850-234851): the bullet-trail box (tType 1, :225530-225597), from where the bomb starts, along its
   fall, growing at its speed up to half its range (750), 0.45 thick, thinning by 0.00036 a ms (1.25 s).
5. **a hit** on a player (never the caller): 150 damage. A kill carries {stk: 3} (the feed's
   `img/streaks/3.png`), skips the kill rewards, and onKill gives the caller "6" [50, 100, 0, 1] (the
   "Bombed" medal, +100) and score +100. A hit that does not kill (or cannot hurt: a team mate, god
   mode) runs onCollision: Rooted and Dizzy, one stack each.
6. **the blast** (game.explosion with info.stk): radius 150, 150 at the centre, falling off linearly; the
   caller is spared (no self damage), kills and survivors as in 5.

KrunkNative: `effects.c` (calcPos, start, update, onKill, onCollision), `projectile.c` (`game_spawn_bomb`,
`game_explosion_streak`, the streak path of a projectile hit), `game.c` (the tick, the reset at every
round and at the end). Differences, documented in the code:

- no "bbm" packet: the start sends the `adrop_0` 2D sound (NET_PACKET_SOUND), which is all the client
  does with "bbm" 0; "bbm" 1 is not sent;
- each bomb is its own NET_PACKET_PROJECTILE with `.bomb` (no owner on the wire, as "bmb"); its trail is
  `vfx_physobj_trail` (`client/src/vfx.c`, called from `projectiles_render.c`), whose pool grew from 32 to 64
  trails (a bombardment keeps about 25 alive);
- a caller who leaves stops the waves and its bombs in the air end (v10 keeps bombing for the departed
  player object; KrunkNative frees the Player);
- the projectile pool is 192 slots (it was 64): a bombardment keeps up to 100 bombs in the air (a bomb
  that hits nothing flies its 1500 units, up to 12.5 s). combat_test measures the official maps.

The Nuke's medal is now v10's too: "6" [40, reward, 0, 1] is overlay.medalsList[40], "Nuked" (KrunkNative
sent the text "Nuke", which drew a text banner), and its score is not a kill score (players.score with
no isKill, :203377).

## 2. Event kill pools (deob-10.0.3.js:220807-220924, :221797-221807, :217260-217282, :236769-236823)

During a live event a public match pays its players items, one draw per kill:

- **events**: v10's game server holds `game.ongoingEvents`, which its host sets from the events API (the
  bundle only reads it). An event has `startsAt`, `killPools` [{weight, contents [{content_type,
  content_ref}]}], `killPoolsWeight` (a percentage), `killPoolsMode` and `killPoolsIcon`.
- **getKillReward**: the events with kill pools and a weight, by startsAt. Each adds its pools at
  `pool.weight x killPoolsWeight / 100`; a "dilute" event first shrinks the earlier entries by
  `(100 - killPoolsWeight) / 100`. Under 100 in all, the rest is "nothing" (randFloat(0, 100) > total).
  Then a weighted pick of the pool and a uniform pick of its contents. Only `content_type` "skin" pays
  ({t: "skin", i: parseInt(content_ref), r: killPoolsIcon}); currency and the rest return null.
- **endGame** (public games, the round's normal end): every account that is not hack-flagged (nor a K.P.D
  officer's watch) draws `min(kills, 30)` times. The items go out with the END (`otherData.items`) and
  into the account in updateAccounts (`onUpdateAccounts` {skins: [{id, quantity}]}, `!mode.noReward`).
- **the end screen**: items with an `r` get the Collect Rewards button (`./img/<r>.webp`, a 10 s
  auto-collect) and its card overlay; collected, they join the Rewards tab.

KrunkNative:

| part | where |
|---|---|
| getKillReward, the draws | `shared/src/kill_pools.c` (`kill_pools_draw`, `kill_pools_draw_kills`), `shared/include/kill_pools.h`; kill_pools_test |
| the events | kn_brain's community feed file (`brain.community.feed`, default `config/community.json`): `"gameEvents"` with v10's fields plus `endsAt` (`config/community.example.json`). The ones running now go to every game server in the internal WS `welcome` (`gameEvents`) and in a `game-events` push when the set changes (checked every ping) (`services/brain/community.c`, `internal_ws.c`) |
| the draw at the end | `server/src/accounts.c` `acc_kill_pools_draw`, at the round's commit (the END is out): public-type games, accounts not hack-flagged, Box Office only (Krunker Strike's items are its own) |
| the items to the player | `NET_EXT_KN_END_ITEM` (0x8150) `NetEndItemPacket` {skin, icon} per item, after the END; the client holds the ones with an icon for the Collect button (`hud_endscreen_v10_offer_item`) |
| the grant | the stats commit's `itemRewards` [{id, quantity}]; kn_brain's "stats" op adds them once per match (a duplicate commit adds nothing), at most 30, source "killpool" (`give_skin_n`, `services/brain/accounts.c`, `db_accounts.c`) |
| tests | kill_pools_test (the draws), brain_kill_pools (`services/brain/tests/kill_pools_test.py`: the welcome / push, the grant), `make kill-pools-server-e2e` (`server/tests/kill_pools_server_e2e.py`: kn_brain + server_bin, an account's kills pay one END item each and land in its inventory, a guest's do not) |

Differences: a noReward mode (Raid) draws nothing (v10 draws and shows items that updateAccounts then
drops); a K.P.D officer's own account draws (KrunkNative's officers play their own account; kpdData is
not modelled here); events end at `endsAt` (v10's API decides which events are ongoing). An account that
signs in during a round (a live sign-in) draws from the next round on: the round it joined as a guest is
not committed to it.

The same events give a public round its `eventIndex` (v10 updateEventIndex at game.init, deob-10.0.3.js:219733-219751:
0, and with no host, not custom / official / comp, the `id` of the first event running now). KrunkNative's server takes
the brain's list (`id`, `startsAt`, `endsAt`; `kill_pools_event_index`, `server/src/accounts.c accounts_round_changed`)
and sends it after every INIT as `NET_EXT_INIT_EVENT` (`Game.event_index`; the official link forwards init's [2]).
10.0.3's consumers of the index (the EVENT prefab's `event_0` meshes for event 3, the event popup, an event's reward
skins and limited-time mode) need event fields KrunkNative's feed does not have: not ported.

### A feed entry

```json
"gameEvents": [
  {
    "id": 7,
    "name": "Halloween 2026",
    "startsAt": "2026-10-25T00:00:00Z",
    "endsAt": "2026-11-02T00:00:00Z",
    "killPoolsWeight": 5,
    "killPoolsMode": "dilute",
    "killPoolsIcon": "gift",
    "killPools": [
      {"weight": 90, "contents": [{"content_type": "skin", "content_ref": "1234"}]},
      {"weight": 10, "contents": [{"content_type": "skin", "content_ref": "1235"}]}
    ]
  }
]
```

With `killPoolsWeight` 5 and pools of 90 and 10, a draw pays the first pool 4.5% of the time and the
second 0.5%; a 20-kill match draws 20 times. The icon must be a plain name (letters, digits, `_`, `-`)
of an image in `ui/img/` (`.webp`).
