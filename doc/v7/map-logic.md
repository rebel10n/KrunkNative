# Map logic: Krunker v3.1.1 → v7.1.4 delta specification

Status: survey and specification only. No code has been changed. Written 2026-10-12 for the Krunker Box Office in-place upgrade to v7.1.4.

Contents:
- §0 Scope and policy
- §1 Map JSON format
  - 1.1 top-level fields
  - 1.2 object envelope and shims
  - 1.3 per-prefab key changes
  - 1.4 prefab table 0-96
  - 1.5 textures and keys
  - 1.6 collision
  - 1.7 builds
  - 1.8 terrain, zone, ocean, sky, environment, AO
  - 1.9 embedded config
  - 1.10 compatibility
- §2 Map-driven runtime
- §3 Triggers
- §4 Modes, templates, config, airdrops
- §5 Server/client authority and packets
- §6 Gap analysis
- §7 Implementation plan (WP-A … WP-H)
- Appendices
  - A: test map coverage
  - B: missing models
  - C: decisions
  - D: quirks

## 0. Scope, policy, conventions

**What this document covers.**
- The map JSON format: top-level fields, object records, prefabs, textures, collision, terrain, zone, lighting, and embedded config.
- The map-driven runtime.
- The trigger system.
- Game modes, templates, custom game config and airdrops.
- Server/client authority and packets.
- A Krunker Box Office gap analysis.
- A staged implementation plan.

Implementation agents work from this document alone, so every claim carries a source reference.

**Policy (user decision, relayed by the coordinator).**
- Krunker Box Office upgrades **in place** to Krunker v7.1.4. There is no v3.1.1 compatibility mode; v3.1.1 is preserved on branch/tag `v3.1.1`.
- Wherever v7 changed v3 behaviour (map fields, prefabs, triggers, modes, physics), KN adopts the v7 behaviour directly, with no toggles.
- v3.1.1 map JSON must still load, **exactly as v7.1.4 loads old maps**: v7's own defaults, shims and ignored keys are the compatibility specification (§1.10).
- Where v7.1.4 is self-contradictory or broken (reconstruction gaps, obvious bugs), this document says so and records a decision. Search for **Decision:**.

**Architecture constraint.**
- Offline mode is being converted into an in-process sandbox of the real server. This mirrors v7's own `IOSandbox` model (`v7 src/libs/io-sandbox.js`, `index.js:18275-18560`).
- All authority logic therefore belongs in `shared/` (simulation) or server-linked code that both the dedicated server and the offline sandbox run, and is reached through `GameHooks.send` (`KN shared/include/shared.h:853`).
- `client/` only mirrors and renders.
- As of this writing the sandbox work has landed in the working tree: `server/src/sandbox.c` (`server/include/server_sandbox.h`) runs `server_bin`'s core in the client process over loopback connections, driven by `client/src/net/offline.c`; `client/src/net/local_game.c` is deleted. Code in `server/` therefore also runs offline. Logic that the native server needs must still sit in `shared/` or `server/`, never in `client/`.

**Companion specs.** KrunkScript is specified separately; this document gives only the interface (§3.10).

**Path roots.**

| prefix | path |
|---|---|
| **v7** | `/home/user/jails/kru/src/Krunker_v7.1.4/src/` (readable de-minified source: module and property names are original, locals renamed). Paths starting `server/`, `packets.json` or `public/` are relative to `/home/user/jails/kru/src/Krunker_v7.1.4/` |
| **v3** | `/home/user/jails/kru/src/Krunker_v3.1.1/src/` (same convention) |
| **KN** | `/home/user/src/KrunkNative/` |

The `public/js/*.js.map` source maps were not needed: `src/` already carries the original names.

**Status vocabulary.**
- **same**: same index and behaviour as v3.1.1.
- **changed**: same index, different behaviour.
- **NEW**: absent from v3.1.1.
- **removed**: v7 parses but ignores, or no longer has.
- KN status: **supported**, **partial**, or **missing**.

### 0.1 Headline changes (v3.1.1 → v7.1.4)

1. **Prefabs.** The table grows from 60 to 97. Ids 0-59 keep their indices; 33 is renamed SPECTATE_CAM→CAMERA and 55 BOT→AI_SPAWNER; TERMINAL (41) becomes a no-op.
   - Ids 60-96 add lights (directional, spot, rect-area), path nodes, many zones (NoPVP/PVP/KR/Partner/KPD/Environment/Domination), rope ladders, sprites, sprays, leaderboards, zombie machines, server nodes, AI redirection, bot spawners, map anchors and 13 new decorative models.
   - Today KN turns every id ≥60 into a solid cube.
2. **Textures.** The table grows from 17 to 20 (METAL, SAND, SNOW) and gains per-texture variants (`tv`). There are also about 30 new generic object keys: collision opt-outs, texture transform and animation, render side, alpha test, step sounds, spawn chance, hitbox scaling, and more.
3. **Triangle collision.** A top-level `tris` array is collided with a capsule test (`collisions.js`). `cpx` objects leave the AABB grid.
4. **Triggers.** A trigger now holds a list of up to 5 actions (`tas`), each with its own condition (12 condition types), delay and value source (constant / trigger / player / global value).
   - There are 32 events (16 new) and 72 actions (40 new).
   - New concepts: custom player values, global values, action costs and requirements, per-player single use, enter/exit tracking, path-node object moves, animation, GUI toggles and KrunkScript `onCustomTrigger`.
   - No index was reordered.
5. **Modes.** The table grows from 24 to 37. **Indices 13 and 14 change meaning** (ranked → Classic FFA, Deposit). There are new modes (Domination, Squad DM, Kranked, Team Defender, Deposit FFA, Carrier, All or Nothing, Mag Dump, Chaos Snipers, Bighead, Zombies, Impulsed, Blitz) and many mode modifier keys. The rotation is now `[0,2,3,19,25]`.
6. **Config.** Custom game config grows from 39 to 85 server settings, plus per-class overrides (`cls`, 79 fields), effect overrides (`prks`), host types, rounds/intermission/best-of, and an embedded map `config` block. The v3 `map.game` block is ignored at runtime.
7. **Runtime changes.** Jump pads become directional and scalable. Wall-jump eligibility is inverted (opt-out). `borderH` doubles to 500. Objectives switch to uid-based ids with `objtvTime`. The battle-royale zone becomes multi-stage. There are spawn `teamOnly` and `spread`, effects, movement schemes, builds, airdrops and streaks, the zombies economy, and an AI rewrite (paths, teams, physics, rewards, 40 AIs).
8. **Netcode.** `init` carries 17 arguments and a much larger `syncData`. `int` now targets `(type, uid)`. The input frame gains reset (`[13]`) and interact-hold (`[14]`). There are many new S→C packets.

## 1. Map JSON format

**Loader.**
- v7: `data/map.js` `generate()` (172-1490) dispatches on the prefab name and calls builders in `libs/MapManager.js`.
- v3: `data/map.js` `generate()` (238-1492).
- The custom-map entry point is `game.prepareGame` (v7 `data/game.js:606-678`). Validation is in `data/customGameConfig.js` (`validateMap` :152-368).

**Corpus.** The 53 v7 official maps in `/home/user/jails/kru/src/Krunker_v7.1.4/public/maps/*.json` were used to check how often each key and prefab appears.

### 1.1 Top-level map fields

Side: **C** = client only (render, UI, sound), **S** = server / shared logic, **B** = both.

| Field | Type / default / range | Status | Semantics | Side | Refs |
|---|---|---|---|---|---|
| `name` | string ≤16, no `<>` | same | Map name. Editor default "New Krunker Map" | B | `customGameConfig.js:154-162` |
| `objects` | array, ≥1 | changed | Object records (§1.2). At most 5000 objects (7500 for featured creators); collidable at most 4500/6000 (v3 3500/6000) | B | `customGameConfig.js:200-205`, `config.js:916-919` |
| `spawns` | array, 1..20 rows | **changed** | `[x,y,z,team,dir,comp,teamOnly,spread]`. Row length was 3..6 (v3), is now 3..8. `[6]` teamOnly is 0/1; `[7]` spread is `limitMM(0,50)` (§2.5) | S | `map.js:1466-1480`, `customGameConfig.js:169-187`, `editor.js:3417-3432` |
| `camPos` | `[x,y,z]` → `[x,y,z,static,rx,ry,rz]` | **changed** | Menu/spectate camera. Client calls `moveCam(p0,p1,p2)` then `rotateCam(p5,p4,p6)`. `[3]` is the static-menu-camera flag. v3 always rotated to 0 | C | `index.js:13687-13693, 17267-17269`; v3 `index.js:9928-9930` |
| `colors` | string[] | same | Palette indexed by object `ci`/`ei` | B | `map.js:255-305` |
| `xyz` | number[] | changed | Size pool indexed by object `si` (×3). v7 expands it at generate and validate time; v3 only had a server sanitizer (`v3 server/game.ts:362`) | B | `map.js:268-279`, `customGameConfig.js:212-221` |
| `tris` | number[], 9 floats per triangle, world space | **NEW** | Complex-collision triangles baked from `cpx` objects (§1.6). Limit: `tris.length/9 < 5000` | S | `map.js:1461-1465`, `MapManager.js:62-128`, `collisions.js` |
| `dthY` | number, default −100 | same | Death height (`config.deathY`). Editor range −1000..3000. `deathHeight` is legacy and has no consumer | S | `map.js:175` |
| `welMsg` | string ≤70, regex `[\w:;()?,.\|[\] \-+*$@=^!{}]` | same | Chat welcome message | C | `map.js:197-202` |
| `modURL` | URL | changed | Mod-pack zip. v7 loads it only for the custom map at game init (v3 loaded it in generate). KN: ignore it (no mod packs) | C | `index.js:13737-13741` |
| `ambient`, `light`, `sky`, `fog` | colour; editor defaults `#97a0a8`, `#f2f8fc`, `#dce8ed`, `#8d9aa0` | same | A missing `ambient` or `light` means no such light. Mode `ambCol`/`lightCol`/`fogC`/`fogD`/`skyCol` override them | C | `render.js:2738-2784` |
| `ambientI` / `lightI` / `lightD` | 1 [0.1,3] / 1.3 [0.1,3] / 500 [10,5000] | same | Intensities and sun distance | C | `render.js:2744-2765` |
| `sunAngX` / `sunAngY` | 90 / 54, [0,360] | same | Sun position `lightD*(cos v, sin v sin u, sin v cos u)`, with u=π·Y/−180 and v=π·X/−180 | C | `render.js:2763-2768` |
| `shadowR` / `shadowD` | 1024 / 1200 [128,4096] | same | Shadow map resolution and far plane | C | `render.js:2770-2774` |
| `fogD` | 2000 [10,15000] | same | Linear fog far distance (near = 1) | C | `render.js:2777` |
| `shadLength` / `shadWidth` | computed | same | Overwritten at load from the CUBE extents (`round(|max−min|/2.5)`), or the terrain size ×1.2; the JSON value is ignored | C | `map.js:1351-1358` |
| `nAmb` | bool | same | Turns off the per-object cube shading gradient | C | `map.js:367` |
| `skyDome`, `skyDomeCol0/1/2`, `skyDomeTex`, `skyDomeEmis`, `skyDomeMovT`, `skyDomeMovD` | — | same | Gradient sky sphere (§1.8.4) | C | `SkyDome.js` |
| `skyDomeTexA` | asset id | **NEW** | Custom sky texture `<id>/texture` | C | `SkyDome.js:13-17` |
| `skyDomeEmisTex` | asset id | **NEW** | Emissive map `<id>/texture`, otherwise `<src>_e` | C | `SkyDome.js:56-61` |
| `ambInd` | int, 1 | changed list | Soundscape `ambient_<n>`. v7 `config.soundScapes`: Default 1, City 2, Desert 3, Market 4, Scary 5, Storm 6, Purgatory 7, Black Market 8, Tomb 11, Adventure 15, Sea 16, Facility 17, Spooky 18, Festive 19. `amvVols` {6:.3, 7:.4, 15:.04, 16:.1, 17:.2}, otherwise 0.12 | C | `config.js:2404-2425`, `game.js:3700-3712` |
| `ambIndC` | asset id | **NEW** | Custom ambient sound; overrides `ambInd` | C | `game.js:3701-3703` |
| `mmId` | asset id | **NEW** | Minimap image `userAssets/<digits>/texture.png` | C | `Minimap.js:369-381` |
| `oldAO` | bool | **NEW** | Forces the generated AO planes (§1.8.6) | C | `map.js:190-192, 1362` |
| `hasLM` | bool | **NEW** | The map has baked lightmaps, so AO planes are skipped. `prepareGame` overwrites it from the publish info (`game.js:661`) | C | `map.js:182-190` |
| `toneMapping` | 0..4, 0 | **NEW** | No, Linear, Reinhard, Cineon, ACESFilmic | C | `render.js:403-405`, `config.js:959` |
| `toneMappingExposure` | 1 [0,10] | **NEW** | Exposure, rounded to 0.1 | C | `render.js:405` |
| `outputEncoding` | 0..2, 0 | **NEW** | 0 Default (Linear), 1 sRGB, 2 Linear | C | `render.js:406-407` |
| `correctLights` | bool | **NEW** | `useLegacyLights = !correctLights` (physical falloff for point and spot lights) | C | `render.js:408` |
| `ocean`, `ocCol`, `ocOpac`, `ocDist`, `ocSpd`, `ocYOff`, `ocRefRes`, `ocW`, `ocL` | false, `#001e0f`, 0.9, 3.7, 1, −1, 3, 1000, 1000 | **NEW** | Reflective water plane (§1.8.3) | C | `Ocean.js`, `render.js:388-394` |
| `terrainSeed`, `terrainTSeed`, `terrainWidth`, `terrainHeight`, `terrainMntMlt`, `terrain{Mnt,Snw,Grs,Drt,Snd}Col`, `terrainEase`, `sizeMlt` | "", "", 3000, 3000, 1, `#707070`, `#e8fdff`, `#5e692f`, `#7f6238`, `#967f51`, 12, 2 | same | Procedural terrain (§1.8.1). Built only if seed, width and height are all truthy. `sizeMlt` is forced to 5.7 | B | `map.js:224-243`, `Terrain.js` |
| `zone`, `zoneSize`, `zoneSpeed`, `zoneRnd`, `zoneCol0/1/2` | false, 500, 1, false, `colors.zones.BR` | same | Battle-royale ring (§1.8.2). Built only if `zone && zoneSize && zoneSpeed` | B | `map.js:244-252`, `Zone.js` |
| `zoneHeight`, `zoneStages`, `zoneWait` | 1000 [100,10000], 0 [0,10], 5 s [5,500] | **NEW** | Multi-stage zone | B | `Zone.js:12-23, 176` |
| `modes` | int[] | same | Official maps: startup and vote mode candidates (intersected with the rotation) | S | `server/index.ts:1148-1205` |
| `config` | object | **changed** | Embedded game preset (§1.9) | B | §1.9, §4.3 |
| `game` | object | **removed** | v3 map-wide player/camera physics. v7 sets `map.game = map.game \|\| {}` and nothing reads it. Only the v7 *editor* import migrates it into `config.cls[*]` (`editor.js:4036-4049`). **KN: ignore it, as the v7 runtime does** | – | v3 `map.js:241-275`; v7 `map.js:173` |
| `scripts` | `{client, server}` base64 KrunkScript | **NEW** | Decoded in `prepareGame` and passed to `addScripts`. The server half is stripped before the map is sent to clients (KrunkScript spec) | B | `game.js:610-624` |
| `gui` | array ≤80 | **NEW** | Custom HUD elements (§3.8) | C (values from S) | `game.js:3449-3455`, `utils.js:1225-1290`, `overlayHelpers.js:18-69` |
| `botPaths` | `[{p:[[x,y,z]…], r}]` | **NEW** | Editor export of BOT_PATH_NODE chains. **No runtime consumer in v7.1.4.** KN keeps the key on round-trip and does not execute it | – | `editor.js:3434-3446` |
| `player` | `{pos, rot}` | **NEW** (editor test) | Sandbox spawns the player in noclip at the editor camera | C | `index.js:18312-18322` |
| `official`, `creator`, `id`, `didLoad` | — | — | Set by the server or `mapWorker` (`didLoad` is a hydrate artifact); not authored. Ignore them | – | `game.js:658-662`, `mapWorker.js:28` |
| `skyDomeSize`, `ambO`, `ambS`, `shadScale`, `deathHeight`, `biomes`, `ambFadeTime`, `gammaFactor` | — | dead | No runtime consumer in v3 or v7 | – | — |

`prepareGame` (`game.js:606-678`) does the following, in order:
1. replaces `(` and `)` with full-width characters;
2. decodes the scripts;
3. runs `validateMap`, unless skipped;
4. rejects maps whose objects use an unknown prefab id or texture index;
5. sets `creator`, `hasLM`, `official` and `id`;
6. for official maps, sets `maps[mapIndex]`; otherwise `maps=[map]` and `config.maps=[0]`.

The map generation cache key is `index_genID_e<eventIndex>` (`map.js:1534-1536`).

### 1.2 Object record envelope and load-time shims

| Key | Meaning | Notes (v7) |
|---|---|---|
| `i` (alias `id`) | prefab index | Numeric. A string name resolves to undefined, which becomes 0 (CUBE) in generate. The editor rejects it (`ObjectInstance.js:378-381`) |
| `p` | `[x,y,z]` | Clamped to ±500000 (`positionRange`), rounded to 2 decimals. Boxes use the **bottom centre** (y is the base) |
| `s` / `si` | full size / index into top-level `xyz` (×3) | `parseFloat \|\| 0`, rounded to 2 decimals |
| `c`, `e` / `ci`, `ei` | colour, emissive / index into `colors` | `ci`/`ei` are resolved only for render-capable generation |
| `r` | `[r0,r1,r2]` radians | 3 numbers. Builders mostly pass `yR=r0, xR=r1, zR=r2`; exceptions are in §1.3 |
| `f` | face mask, hex string | `parseInt(f,16).toString(2).padStart(6,"0")` gives 6 bits (`utils.js:956`); missing means all faces on |
| `tr` | **legacy** texture rotation in radians | Converted to `tro = toDeg(tr)` (`map.js:340-345`) |
| `l` / `col` | no collision | `noCol = l \|\| col` |
| `hp`, `wid` | aliases of `h` and `w` | |
| single trigger keys | legacy | Wrapped into `tas[0]` (§3.4) |

**v3 behaviours that v7 removed** (KN adopts the removal):
- Automatic hiding of a CUBE's top face (`f[3]`) when another CUBE fully covers it (v3 `map.js:394-409`). v7 relies on the editor's "Optimize Faces" tool writing `f`.
- CUBE dynamic physics keys `dyn` and `grv` (v3 `map.js:455-456`).
- BILLBOARD `sad` (show ad).
- TERMINAL objects (§3.1).

The common per-object keys are tabulated in §1.5.2. Additional semantics from the map slice:

| Key | v7 semantics | v3 |
|---|---|---|
| `pe` (penetrable) | **float 0..1** (`limitMM`). Wallbang damage = `(dmg − baseDmg·density·pierce)·pe`; weapons with null `pierce` stop (`players.js:3172-3176`) | truthy flag (v3 `players.js:4334-4341`) — **changed** |
| `bo` (border) | Collision box extends up by `borderH` = **500** (v3 250). `disableB` turns it off. Accepted on CUBE, GATE, DEPOSIT_BOX, PERK/KRUM machines and every *_ZONE | same, wider set |
| `wj` | `notWallJ` (absent = wall-jumpable) | v3 `wallJumpable = wj == null` (KN `map.c:595-599` already matches) |
| `v` | noVis = 1 when present | v3 passed the raw value |
| `spn` | float spin speed | v3 int |
| `in` | clamped 0..2000 | 0..1000 |
| `sci` | canScript. The v7 serializer writes `this.sci` (`ObjectInstance.js:1903-1905`), so the parse side must accept any truthy value | NEW |
| `pmv` | AI physics movement; read but never serialized by the editor (`ObjectInstance.js:686`) | NEW |

### 1.3 Prefab-specific key and geometry changes

Line numbers are v7 `data/map.js`. **Rotation-order changes are parity-critical:** v3 maps carry radians that v7 interprets with a new argument order, and KN must use the v7 order.

| Prefab | Change |
|---|---|
| CUBE (322) | adds `gp ns nai ip rsc fct fs tro tsm ft ten sat cpx rs da smm`; drops `dyn grv` and auto top-face culling |
| SPHERE (406) | adds `fct fs tro wj gp ns nai ip ft ten cpx` |
| POINT_LIGHT (438) | same (`c`, `li` 1, `lds` 0, `ld` 1) |
| DIRECTIONAL_LIGHT (445) | NEW: target `lt` [x,y,z] (required), `li` |
| SPOT_LIGHT (453) | NEW: `lt`, `li`, `lds` (50), `ld`, `la` (builder default 45; the editor writes 15), `lp` penumbra |
| RECTAREA_LIGHT (465) | NEW: width `s[0]/2`, height `s[1]/2`, `li`, rotation `(r[2],r[0],r[1])` |
| GATE (492) | now takes faces `f`, plus `inm tro ft tsm ten sat nf smm gp ns nai ip` |
| TRIGGER (537) | `tas` + the §3.2 keys |
| TERMINAL (646) | empty branch: nothing is built |
| DEPOSIT_BOX (648) | faces + generic keys; `wd`/`dp` 1000 |
| LIGHT_CONE (689) | `thl` theta length (360), `sds` sides (**default 12**), `fc` full cone; mapDet only |
| LIGHT_BAR (706) | NEW: plane `s[0]/2 × s[2]/2`, emissive `e`, `msp`; mapDet only |
| CYLINDER (720) | `sds` (12), `rtp`/`rbt` (1), `ths` (0), `thl` (360), `opn`, float `spn`, `pe`, `cpx` + generic keys |
| SOUND_EMITTER (768) | adds `edm` (distance model 0..2); `ero` rolloff |
| BOOST_PAD (788) | `bm`: v3 `parseFloat(bm)\|\|1`, v7 `bm==null?1:(parseFloat(bm)\|\|0)` (0 now allowed). `d` direction is NEW. **Rotation swapped:** v3 `yR=r0, xR=r1`, v7 `xR=r0, yR=r1` |
| WEAPON_PICKUP (798) | adds `rwg wpi inm ntx` |
| SHOWCASE (812) | adds `spn spa` |
| SPRITE (838) | NEW: plane `s[0]/2 × s[2]/2`; texture scale 1 unless `tsr` |
| PLANE (862) / BILLBOARD (908) | now `addFixedPlane(x,y,z, s[0]/2, s[2]/2, …, r0, r1, r2)`, rotation = `Rx(r0)·Ry(r1)·Rz(r2)·Rx(−π/2)` (`render.js:1993-2004`). v3 called `addPlane(x,y,z, s[2]/2, s[0]/2, …, r2, r0−π/2, r1)` (v3 `map.js:733-777`). **Width and length are swapped and the rotation order changed.** Plane UV scale is 1 (v3 2) unless `tsr`. PLANE adds `alt cpx da msp` |
| LIQUID (948) | same keys + `tro ten`; same rotation change as PLANE |
| SIGN (969) | adds `so` (outline, `#111111` editor / `#626262` runtime default), `sot` (1), `nsf`, `msp`, `ts`/`td`; font ≤240; **rotation now `(r0,r1,r2)`**, v3 `(−r2, r0−π/2, r1)` |
| LEADERBOARD (985) | NEW (§1.4) |
| CAMERA (1001) | `camt`, `frc`, `fp`, **`obl`** (v3 SPECTATE_CAM used `obi`, which v7 no longer reads, so old cams link to 0). camType 0 pushes `spectateCams[objLink]` with y+5 (`MapManager.js:2640-2670`) |
| RAMP (1011) | adds `tsm fct fs tro ft ec ten sat smm`, `l`/`col`, `nai` |
| AI_REDIRECTION (1040) | NEW: `d`, `b` |
| *_ZONE (1044-1133) | Premium, Verified, Partner, Team, KR, NoPVP, PVP, KPD all accept `bo h cdy sd rsc rt rr in`; KR adds `krs` |
| DEATH_ZONE (1136) | adds `h cdy sd rsc rt rr in` |
| TELEPORTER (1152) | adds `tcp` |
| LADDER (1162) | adds `tro gp ip ft ten sat` |
| ROPE_LADDER (1184) | NEW |
| AI_SPAWNER (1217) / BOT_SPAWNER (1239) | §2.8 |
| MAP_ANCHOR (1241) | `ant` (0 Corner, 1 Center); minimap bounds use anchors instead of spawns (`Minimap.js:69-90`) |
| ENVIRONMENT_ZONE (1243) | §1.8.5 |
| PATH_NODE (1245) | `nid` → `pathNodes[nid]` |
| CUSTOM_ASSET (1276) | adds `rsc spn spa wj gp ns nai ip cst rcv as ast fcr ten alt nf rs ft hbh hbw cpx smm` |
| SERVER_NODE (394) | NEW: `ssp`, `nt`, health and interface keys |
| MYSTERY_BOX (470) / PERK_MACHINE (474) / KRUM_MACHINE (487) | NEW: `sp`, `bo`; perk also `zp` and health/interface |
| PRIZE_WHEEL (628) | NEW: `sty`, tRadius 7, at most 10 |
| SPRAY (637) | NEW: `spt` (default 2482) |

Model prefabs without a dedicated branch fall through to `addMesh(src = name.toLowerCase())` (`map.js:1317-1345`). v7 adds `texVar` (`tv`) and `sound` (`sat`) to this path.

**Object creation order is a parity requirement.** Object `uid`/`sid` values are array indices that interfaces, triggers, `do`/`ro`/`mo` and the join sync all reference. The server and client must create objects in identical order, skipping the same records: EVENT (40) on custom maps, TERMINAL (41), editor-only prefabs, and `mapDet`-only render objects. KN must keep a single shared parse in `shared/src/map.c` that both sides use. **Decision:** render-only objects (lights, LIGHT_BAR, LIGHT_CONE, LIQUID, PARTICLES, SPRITE, SPRAY) still get a uid on both sides, even when the client renders them conditionally.

### 1.4 Prefab table (v7 ids 0-96)

**Sources:**

| What | v7 | v3 |
|---|---|---|
| Prefab tables | `data/prefabs.js:329-2570`, geometry generators `:2618-3265` | `data/prefabs.js:108-1350` |
| Map-object dispatch | `data/map.js:259-1310` (loop `:264`, per-prefab `if` chain `:346-1310`) | `data/map.js:336-1056` |
| Runtime builders | `libs/MapManager.js` (3849 lines, `add*` list `:7-37`) | `data/objects.js` |
| Runtime fields | `libs/GameObject.js:3-383` | — |
| Editor ⇄ JSON | `libs/ObjectInstance.js:372-780` (deserialize), `:1006-1965` (serialize) | — |

**Index stability:** ids 0-59 keep their v3 index. Two are renamed:

| id | v3 name | v7 name |
|---|---|---|
| 33 | `SPECTATE_CAM` | `CAMERA` |
| 55 | `BOT` | `AI_SPAWNER` |

KN keeps the old names as aliases in `map.c`'s name table for string lookups. Ids 60-96 are new.

**`BOT_PATH_NODE` is not a prefab.** The editor references it (`editor.js:2611,3944,4726`; `ObjectInstance.js:1258`), but bot paths are exported as a top-level `botPaths: [{p:[[x,y,z]…], r}]` (`editor.js:3433-3445`).

**Legend for the table:**
- **cube** = `generateCube` box with faces `f`.
- **model** = `addMesh` → `models/<src>.obj` + `textures/<src>.png`, scaled by `ms || prefab.scale` (`MapManager.addMesh:2671-2778`).
- **AABB** = solid box. **none** = `noCol`. **zone** = non-solid volume with behaviour. **cyl** = cylinder collision (`collT:"cylinder"`).
- Every v7 **model** prefab (1, 2, 7, 8, 15-22, 28, 36, 37, 49-51, 54, 56, 58-62, 73, 74, 81, 83-88, 90) gains these keys over v3: `gp`, `ip`, `ns`, `nai`, `sat`, `smm`.

| id | name | status | geometry / render | collision | behaviour, prefab-specific JSON keys (v7 ref) |
|---|---|---|---|---|---|
| 0 | CUBE | changed | cube, faces `f`, texturable, `at`, `tv`, `ts/td` scroll, `fct/fs` frames, `tsr/tsm/tro/tox/toy`, `ten`, `rs`, `da`, `ft` | AABB; `l`/`col` → none; `cpx`, `ns`, `nai`, `gp`, `ip`, `wj` | health/destructible, `in`, `bo` border, `rsc`, `sat`, `smm` (`map.js:346-393`, `MapManager.addBlock:2469`) |
| 1 | CRATE | +variants | model crate_0 ×6, isProp; `tv` 0-4 (2-4 use a different model, `diffM`) | AABB | prop-hunt prop |
| 2 | BARREL | +variants | barrel_0 ×4; `tv` 0-5 (diffM, complex) | AABB | — |
| 3 | LADDER | changed | `generateLadder`: 2 rails + a rung every 6u (`MapManager.addLadder:1308`); default texture FLOOR (2) | ladder volume, width `ladderWidth` 3.2 / `ladderScale` 0.5, rotated by `d` | climb; adds `sat`, `ft`, `ten`, `tro`, `gp`, `ip`, `wj` (`map.js:1162`) |
| 4 | PLANE | changed | `addFixedPlane` (v3 used `addPlane`) with rotation `r`; UV scale 1 (v3: 2) unless `tsr`; `en` edge noise; `alt`, `msp`, `fct/fs`, `da` | zero-height box `type:1`, single-sided unless noise / side 2 / asset; `cpx` | health/interface (`map.js:862-907`) |
| 5 | SPAWN_POINT | changed | editor-only; exported to `spawns` | — | row `[x,y,z,team,dir,comp,teamOnly,spread]`. `[6]` teamOnly (`to`) and `[7]` spread 0..50 (`ssp`) are new (`editor.js:3414-3428`, `map.js:1466-1479`) |
| 6 | CAMERA_POSITION | changed | editor-only; exported to `camPos` | — | `camPos=[x,y,z,static(cstc),rx,ry,rz]`; `[3..6]` are new (`editor.js:3412,3914-3920`) |
| 7 | VEHICLE | +variants | vehicle_0 ×20, emissive; `tv` 0-3 | AABB | — |
| 8 | STACK | same+ | stack_0 ×6 | AABB | — |
| 9 | RAMP | changed | `addRamp` plane; `ec` encloses the sides with triangles (`prefabs.js:3018`); `fct/fs`, `ten`, `tro`, `tsm`, `sat`, `smm` | ramp `{sX,sZ,eX,eZ}` `type:2`; `nai` | boost `b` clamped ±`boostRange` 100; `boostDr=asin(h/len)` (`MapManager.addRamp:1405-1493`) |
| 10 | SCORE_ZONE | changed | tool | zone | `sp` (default 1, `map.js:1134`); scores once per life and calls `logTime` (§2.1) |
| 11 | BILLBOARD | changed | `addFixedPlane` side 2; `bb` clamped to 1..`config.billboards.length-1` (6), random when absent; default size z 20 (v3 10); `msp`, `alt` | plane | v3 `sad` (show ad) and the ad text are **removed** (`map.js:908`); KN: a server billboard replaces every poster (decisions.md "Billboard ads") |
| 12 | DEATH_ZONE | changed | tool | zone (forceCollision) | kill. **New:** health, `cdy`, `sd`, `rsc`, `rt`, `rr`, `in` (destructible and interface-togglable; `MapManager.addDeathZone:560`) |
| 13 | PARTICLES | same | `PARTICLES.area` | none | `pr` type (0 default, 1 grass, 2 fog, 3 ember; `config.particleIDS`), `psp`, `pd`, `pc`, `pg`, `psc`, `ps`, `d`, `c`, `pb` blending 0-4 (`map.js:823`) |
| 14 | OBJECTIVE | changed | `generateObjective`; texture `objective_4` with glow `#ffd542` pulse (v3 `objective_0`) | zone | `obi` 1..10 → uid `obi-1`, otherwise auto (`MapManager.addObjective:452-500`) |
| 15 | TREE | +variants | tree_0 ×10; `tv` 0-8 diffM | AABB | — |
| 16 | CONE | +variants | cone_0; `tv` 0-1 | AABB | — |
| 17 | CONTAINER | same+ | container_0 ×7 | AABB | — |
| 18 | GRASS | same | grass_0 ×32, 4 frames / 180 ms, alphaTest 0.02, double-sided, transparent, `noDetail` | AABB (unless `l`) | — |
| 19 | CONTAINERR | same+ | containerr_0 | AABB | — |
| 20 | ACIDBARREL | +variants | emissive; `tv` 1 = HD (noEmis, complex) | AABB | — |
| 21 | DOOR | +variants | `tv` 0-3 | AABB | — |
| 22 | WINDOW | +variants | transparent; `tv` 0-6 | AABB | — |
| 23 | FLAG | changed | crystal_0 flag + zone ring | flag + trigger zone (`flagZoneS` 24 × `flagZoneH` 36) | **neutral flag:** `tm` −1 → neutral (`canNeutral`). v7 passes `tm` raw and adds 1 inside `addFlag` (`MapManager.addFlag:221-270`); v3 added 1 in map.js |
| 24 | GATE | changed | cube + faces `f`; `fct/fs`, `ten`, `tsm`, `ft` | AABB while active | interact (tRadius 7): `sp`, `md` method 0-7 (§2.3), `cl`/`stc`, `in`, `tm`, `inm`, `nm`, `sat`, `smm` (`map.js:492`, `MapManager.addGate:1678`) |
| 25 | CHECK_POINT | same | tool | zone | `su`, `d`, `tm` (`map.js:1146`) |
| 26 | WEAPON_PICKUP | changed | weapon mesh (×2 for equipment), rotation `r` | zone (pickupZone 12×4×2) | `w`/`wid`, `wr` (no replace), `rw` random, `rwg` random group 1..10000 (same weapon across the group), `wpi` (hide icon), `ntx` (no texture), `inm`, `sp`, `md`, `nm`, `tm` (`map.js:798`, `game.js:2621-2657`) |
| 27 | TELEPORTER | changed | tool | zone | `d`, `fd`, `ch` 0..100, `n` (0 send / 1 receive / 2 both), `m`, `tm`, **`tcp`** (teleport to the player's last checkpoint) (`map.js:1152`, `game.js:3012-3032`) |
| 28 | TEDDY | same+ | teddy_0 | AABB | prop |
| 29 | TRIGGER | changed | cube, or an asset model when `aid` is set (animated if `aid` has g/q/f); `f`, `spn`/`spa`, `cst`/`rcv`, `as`/`ast`, `fcr`, `alt`, `rs`, `fct/fs` | AABB; `pl` = no player collision; `hbh`/`hbw` ×0.1..10; `ns`, `nai`, `gp`, `ip` | trigger system §3 (`map.js:537-627`, `MapManager.addTrigger:1845`) |
| 30 | SIGN | changed | canvas text; `so` outline colour (runtime default #626262, editor #111111), `sot` outline thickness, `nsf` (no SCF), `msp`; `sf` font 1..240 (v3 10..120) | none | `st`, `sa`, `sc`, `sb`, `bv` (`map.js:969`, `MapManager.addCanvas:2599`) |
| 31 | DEPOSIT_BOX | changed | cube + `f`; `fct/fs`, `ten`, `tsm`, `ft` | AABB | bank: `wd`/`dp` (default 1000), `in`, `tm`, `sat`, `smm` (`map.js:648`) |
| 32 | LIGHT_CONE | changed | additive cone `lightcone_0/1` (`fc`); `thl` theta length 0.1..360; `sds` sides 3..64 (v3 fixed at 10); `ten` | none | mapDet only (`map.js:689`, `MapManager.addLightCone:2290`) |
| 33 | CAMERA | renamed (SPECTATE_CAM) | none | none | `camt` 0 Spectate / 1 Player (`config.cameraTypes`), `frc` force cam, `fp` follow path ("1,2,3" path-node ids), **`obl`** objective link (v3 read `obi`) (`map.js:1001`, `MapManager.addCamera:2640`). A player camera (camt 1) does nothing at map load |
| 34 | SPHERE | changed | sphere, double-sided, no shadows; `fct/fs`, `ten`, `ft`, `cpx` | none (render only; `cpx` adds triangles) | `map.js:406` |
| 35 | PLACEHOLDER | same | editor-only (`noExport`) | — | — |
| 36 | CARDB | same+ | cardb_0 | AABB | prop |
| 37 | PALLET | same+ | pallet_0 | AABB | prop |
| 38 | LIQUID | changed | animated wave plane (`wh` 0.5, `ws` 10, `tx`/`tz` 10 tiles), mapDet only; `tro`, `ten` | none (**no swimming in v7**) | `map.js:948`, `MapManager.addLiquid:3636-3667` |
| 39 | SOUND_EMITTER | changed | none | none | `es`/`esa`, `ev`, `er`, `ed` distance 0..5000; **`edm`** distance model 0..2; **`ero`** rolloff (`map.js:768`, `MapManager.addEmitter:2228`) |
| 40 | EVENT | changed | event_0; `tv` 0 Random / 1 Yellow / 2 Pink / 3 Blue / 4 Golden / 5 Diamond; `texRange` 3 | AABB, `isEvent` pass-through | skipped on custom maps (`map.js:1305`); `noExport`, `localOnly` |
| 41 | TERMINAL | **removed** | — | — | v7 `map.js:646-647` is an empty branch: nothing is built |
| 42 | PREMIUM_ZONE | changed | tool | solid unless the player has premium | now has health, `cdy`, `sd`, `rsc`, `rt`, `rr`, `in`, `bo` |
| 43 | VERIFIED_ZONE | changed | tool | solid unless the player is featured | same additions as 42 |
| 44 | CUSTOM_ASSET | changed | user asset `aid` (§1.5.4), `ms`, `spn`/`spa`, `txs`, `cst`/`rcv`, `as`/`ast`, `fcr`, `ten`, `alt`, `nf`, `rs`, `ft` | AABB scaled by `hbh`/`hbw`; `cpx` | `map.js:1276`, `MapManager.addAsset:2779-2916` |
| 45 | BOMB_SITE | changed | `generateObjective` look, scrolling `objective_b` texture | zone | `map.js:784` |
| 46 | BOOST_PAD | **changed** | pad_0 model ×3.6 scaled by min(w,l)/10; **scalable** (v3 fixed) | volume w=sx/2, l=sz/2, h=max(sy,3) (v3 fixed 5×5×1) | `bm` bounce clamped −3..3 (default 1); `cr` crouch halves; **`d` direction 0..5** (0 up, 1 down, 2-5 sideways ±90°) (`map.js:788-797`, `MapManager.addPad:271-314`, physics `player.js:1415-1452`) |
| 47 | TEAM_ZONE | changed | tool | solid unless `teamCheck` passes (own team passes) | + health/interface keys |
| 48 | CYLINDER | changed | `sds` 3..64 sides; `rtp`/`rbt` top/bottom radius 0..1; `ths`/`thl` theta start/length; `opn` open-ended (DoubleSide); `fct/fs`, `tro`, `ten`, `tsm` | cyl, radius min(sx,sz)/2; `spn` −20..20 spin carry; `cpx`, `pe` | `map.js:720`, `MapManager.addCylinder:2332` |
| 49 | POLICE | same+ | police_0 | AABB | — |
| 50 | CAGE | same+ | cage_0 | AABB | prop |
| 51 | EBARREL | +variants | `tv` 1 Poly, 2 Fuel (diffM) | AABB | prop |
| 52 | SHOWCASE | changed | store item model (`skin`), `ms`, `spn`/`spa` | none | `map.js:812` |
| 53 | POINT_LIGHT | same | PointLight, mapDet only | none | `li` 0..15, `lds` 0..1000, `ld` 0.1..2 (`MapManager.addPointLight:2240`) |
| 54 | GHOST | same+ | ghost_0, emissive (isProp removed) | AABB | — |
| 55 | AI_SPAWNER | renamed (BOT), many new keys | ghost_0 or asset `aid`; `fct/fs`, `rs`, `alt`, `ten`, `bbS`/`bbD` bob, `spn` | AI hitbox `hbh`/`hbw` | about 70 AI keys (§2.8; `MapManager.buildAIData:2929-3035`, clamps `:3036-3270`); `aiLimit` 40 (v3 20); default event `te` 15 |
| 56 | PUMPKIN | same+ | emissive | AABB | prop |
| 57 | RUNE | same+ | rune_0 plane, alphaTest 0.9, `msp` | none | — |
| 58 | SKELETON | same+ | — | AABB | prop |
| 59 | KNIGHT | same+ | CPY 4 | AABB | prop |
| 60 | GIFT | NEW | gift_0 ×2; `tv` 0 Random / 1 Pink / 2 Yellow / 3 Green / 4 White; texRange 3 | AABB | prop |
| 61 | CHRISTMASTREE | NEW | christmastree_0 ×2 | AABB | canInterface |
| 62 | CANE | NEW | cane_0 ×2 | AABB | — |
| 63 | LIGHT_BAR | NEW | additive plane `lightcone_1`, MeshBasic, no depth write; `msp` → sprite | none | mapDet only (`map.js:706`, `MapManager.addLightBar:2308`) |
| 64 | PATH_NODE | NEW | editor octahedron | none | `nid` → `manager.pathNodes[nid]` (`MapManager.js:2918-2928`). Used by AI `phi`/`rewn`, CAMERA `fp`, and trigger actions 40/41 (`game.js:818`, `AI.js:628,1115`) |
| 65 | DIRECTIONAL_LIGHT | NEW | DirectionalLight aimed at `lt` [x,y,z] | none | `c`, `li` 0..15 (`map.js:445`, `MapManager:2253`) |
| 66 | RECTAREA_LIGHT | NEW | RectAreaLight sx×sy, rotation `(r[2],r[0],r[1])` | none | `c`, `li` (`map.js:465`) |
| 67 | SPOT_LIGHT | NEW | SpotLight aimed at `lt` | none | `li`; `lds` 0..1000 (default 50); `ld`; `la` angle 1..90 (default 45; the editor compares against 15 when serializing); `lp` penumbra 0..1 (`map.js:453`, `MapManager:2264`) |
| 68 | PRIZE_WHEEL | NEW | spinner canvas plane, `d` in 45° steps (`generateSpinner`) | spinner list, tRadius 7 | `sty` wheel 0..2 (`store.wheels`); interact 9; KR purchase via `cnfm` (`map.js:628`, `MapManager.addSpinner:2195`, `game.js:3044-3100`). Monetisation: KN renders it and treats it as unaffordable |
| 69 | KR_ZONE | NEW | tool | solid unless the player has ≥ `krs` KR (0..1e7) | + health/interface (`map.js:1089`, `player.js:1473`) |
| 70 | NOPVP_ZONE | NEW | tool | non-solid | sets `player.noPVP` while inside (`player.js:1471-1475`) |
| 71 | PVP_ZONE | NEW | tool | non-solid | sets `player.forcePVP` (`player.js:1477`) |
| 72 | LEADERBOARD | NEW | canvas plane (`createLeaderText`), `msp` | none | `ldt` 0 score / 1 time; `ldc` custom value key (≤10, a-z0-9_); `ldo` order 0/1; `ldr` rows 1..50 (default 25); `sf`, `sc`, `sb`, `bv`, `nsf`. Server: `game.addPendingLeaderboards` (`game.js:3799-3806`); `MapManager.addLeaderBoard:2619`, `updatePendingLeader:3371`; `lbLimit` 2 |
| 73 | TILES | NEW | tiles_0 ×10 | AABB | — |
| 74 | CLOUDS | NEW | clouds_0 ×3 | AABB | — |
| 75 | PARTNER_ZONE | NEW | tool | solid unless `account.partnerApp>=2` | `map.js:1066` |
| 76 | SPRITE | NEW | SpriteMaterial billboard sx×sz, texturable, `fct/fs`, `ts/td`, `tro`, `ten` | none | `map.js:838`, `MapManager.addSprite:3612` |
| 77 | DOMINATION_ZONE | NEW | objective_4 cube | zone | `manager.domzones` (§4.1a) (`map.js:783`, `MapManager.addDominationZone:502-533`) |
| 78 | ROPE_LADDER | NEW | box of min(sx,sz) × sy; default texture FLOOR | cyl ladder (`ropeLadder`) | climbable from any side (`player.js:1512-1521`); `sat`, `sds`, `opn`, `rtp`, `rbt` (`map.js:1184`, `MapManager:1360-1404`) |
| 79 | ENVIRONMENT_ZONE | NEW | tool | none | per-zone lighting override while the **camera** is inside (§1.8) (`MapManager:879-926`; client `index.js:17004-17016` → `RENDER.updateGameEnvironment`, `render.js:2698-2720`) |
| 80 | KPD_ZONE | NEW | tool | solid unless `account.canFlag` | `player.js:1469` |
| 81 | DUMPSTER | NEW | dumpster_0 ×4 | AABB | — |
| 82 | SERVER_NODE | NEW | invisible tool | non-solid (`serverNode` pass-through), `noAICol` | `nt` tag (≤70), `ssp` spread 0..50, health/interface. zom tags: `zombie_spawn`, `skull_spawn`, `portal_spawn`, `frog_spawn` (`modes/zom.js:418-562`); also exposed to KrunkScript (`MapManager.addServerNode:2418-2468`) |
| 83 | LAMP | NEW | lamp_0 ×1; `tv` 0 Wall Lantern / 1 Street Light / 2 Barn Light (diffM) | AABB | — |
| 84 | WATER_TOWER | NEW | water_tower_0 ×10; `tv` 1 tank only | AABB | — |
| 85 | AC_UNIT | NEW | ac_unit_0 ×5 | AABB | — |
| 86 | SHEET_METAL | NEW | sheet_metal_0 ×0.5 | AABB | — |
| 87 | ANTENNA | NEW | antenna_0 ×10; `tv` 0-3 | AABB | — |
| 88 | SATELLITE_DISH | NEW | satellite_dish_0 ×5 | AABB | — |
| 89 | SPRAY | NEW | plane with texture `sprays/<id>`; `spt` item id (default 2482, must be type SPRAY) | none | decal (`map.js:637`, `MapManager.addSpray:2160`) |
| 90 | WEAPONBOX | NEW | weaponbox_0 ×5 | AABB | decor, canInterface |
| 91 | MYSTERY_BOX | NEW | weaponbox_0 + `mysterybox_beam`; fixed size 10×7.27×17.91; starts inactive | interact tRadius 7 | `sp` price (default 1); interact 13 (§2.7) |
| 92 | PERK_MACHINE | NEW | perkmachine_0 + `_<zp>` texture, emissive | AABB + interact | `zp` perk 0..6 (Krum, Double-Up, Western Hands, Swift, Power Slide, Drift Fuze, Holster); `sp`, `bo`, health/interface; interact 14; at most 4 perks; +50 score the first time the player crouches on it (`player.js:1385-1389`, `players.js:3434`) |
| 93 | AI_REDIRECTION | NEW | tool | non-solid for players | AI-only steering volume (`d`, `b`) (`AI.js:853-857`, `MapManager.js:1494-1520`) |
| 94 | KRUM_MACHINE | NEW | krummachine_0 ×7 | interact | weapon upgrade ("pack-a-punch"); `sp`, `bo`; interact 15 (`players.js:3408-3433`, `game.js:2420-2588`) |
| 95 | BOT_SPAWNER | NEW | none (localOnly tool) | none | map bot (§2.8.1); `botLimit` 16 (`map.js:1239`, `MapManager.buildBotData:3285-3329`, `addBot:3331`) |
| 96 | MAP_ANCHOR | NEW | editor pole (check_0, 1×500×1) | none | `ant` type → `manager.mapAnchors`. Minimap bounds use the anchors instead of the spawns (`Minimap.js:69-75`); also exposed to KrunkScript; `anchorLimit` 1000 |

**Unknown prefab ids:**
- v7 `prepareGame` rejects a custom map whose objects reference a prefab id that does not exist, or a texture `t` that does not exist (`game.js:606-678`).
- KN currently maps every id ≥60 to CUBE (`map.c:111-131`), which turns v7 objects into solid cubes (Krunk_Plaza, Slide_Neon_Stage, zombie_high, SniperRoyale2, Bhop_VIP, eterno_jump and slide_moonlight are affected today). Under v7, ids 0-96 are valid.
- KN must treat ids ≥97 like v7 does: reject the custom map in validation, and skip the object when loading already-validated data.

#### 1.4.1 Prefab flag → JSON key glossary (`ObjectInstance.serialize`, `:1006-1965`)

| Flag | Keys | v3? |
|---|---|---|
| editFaces | `f` | yes |
| editFog | `nf` | yes |
| movingTexture | `ts`, `td` | yes |
| frameTexture | `fct`, `fs` | new |
| hasHealth | `h`, `cdy`, `rt`, `rr`, `sd`, `rsc` | `rsc` new |
| editPen | `pe` | yes |
| editBulletCol | `ns` | new |
| editAICol | `nai` | new |
| canWallJump | `wj` | yes (**meaning inverted**, §2.1) |
| canGrapple | `gp` | new |
| canImpulse | `ip` | new |
| canTrans | `ft` | new |
| stepSrc | `sat` | new |
| canComplexColl | `cpx` | new |
| editRenderSide | `rs` | new |
| editAlphaTest | `alt` | new |
| showMinimap | `smm` | new |
| canSprite | `msp` | new |
| canStretch | `tsr`, `tsm` | `tsm` new |
| alterHitbox | `hbh`, `hbw` | new |
| canScript | `sci` | new |
| hasSpin / hasSpinDir | `spn` / `spa` | `spa` new |
| teamable | `tm` | yes |
| customDirection | `d` | yes |
| optionalDirection | `fd` | yes |
| customScore | `sp` | yes |
| canInterface / canTargetInterface | `in` / `tin` | yes |
| hasBorder | `bo` | yes |
| interactMsg | `inm` | new |
| texturable | `t`, `tv`, `at`, `spt`, `tox`, `toy`, `tro` | `tv`, `spt`, `tro` new |
| variants | `tv` | new |
| canDirectionalScale, zone, tool, notStyleable, edCat | editor only | — |

Removed flags:
- `canBeDynamic` (CUBE)
- `hasSound` (TRIGGER)
- `interact2` (TERMINAL)
- `objectiveLink`, which moved from OBJECTIVE to CAMERA

### 1.5 Textures, materials, generic object keys

#### 1.5.1 Texture table (`t`; v7 `prefabs.js:13-292`, v3 `:9-80`)

| t | name (alias) | src | v7 variants (`tv`) | v3 |
|---|---|---|---|---|
| 0 | WALL (Stone) | wall_0 | 0 Default, 1 Classic, 2 Light, 3 Voxel, 4 Pebbles, 5 Cobble, 6 Light Pebbles, 7 Light Cobble | no variants |
| 1 | DIRT | dirt_0 | 0-3 (Default, Classic, Voxel, Modern) | yes |
| 2 | FLOOR (Wood) | floor_0 | 0-3 (Default, Classic, Classic Alt, Modern) | yes |
| 3 | GRID | grid_0 | — | yes |
| 4 | GREY | grey_0 | 0-2 (Default, Template, Modern) | yes |
| 5 | DEFAULT (None) | default | — | yes |
| 6 | ROOF | roof_0 | 0-2 (Default, Classic, Tiled) | yes |
| 7 | FLAG | flag_0 | 0-2 | yes |
| 8 | GRASS | grass_1 | 0-3 | yes |
| 9 | CHECK | check_0 | — | yes |
| 10 | LINES | lines_0 | 0-2 (Default, Thick, Pipes) | yes |
| 11 | BRICK | brick_0 | 0-10 | yes |
| 12 | LINK | link_0 (transparent) | 0-2 | yes |
| 13 | LIQUID | liquid_0 (transparent) | 0 Water, 1 Voxel Water, 2 Lava, 3 Greek Water | yes |
| 14 | GRAIN | grain_0 | 0-2 | yes |
| 15 | FABRIC | fabric_0 | 0-3 | yes |
| 16 | TILE | tile_0 | 0-3 | yes |
| 17 | METAL | metal_0 | 0-1 | **new** |
| 18 | SAND | sand_0 | 0-2 | **new** |
| 19 | SNOW | snow_0 | — | **new** |

**Resolution** (`map.js:266-292`):
- Missing or out-of-range `t` → 0 (2 for LADDER and ROPE_LADDER).
- `src = name.toLowerCase()+"_"+(GRASS?1:0)`. If `tv` is truthy and `variants[tv]` exists, append `"_"+variants[tv].id`. Example: WALL `tv:3` → `wall_0_3`.
- `at` overrides `t`. Digits are a user asset id; a `v` anywhere marks a video (`"<id>/video"`, otherwise `"<id>/texture"`).
- `UTILS.isTransTex(src)` is true when src starts with `link_` (`utils.js:1604`). Transparent textures get alphaTest 0.02, and write depth only when opacity is 1.
- v3 also accepted `t` as a texture **name** string, converted in v3 `map.js:40-47`. The v7 game only takes numeric `t`; only the v7 editor converts strings (`ObjectInstance.js:461`). Adopt v7: a string `t` behaves as out-of-range → 0.

**Model variants** (`MapManager.addMesh:2734-2747`):
- `tv` with `diffM` → model **and** texture `"<src>_0_<id>"`.
- `tv` without `diffM` → texture only.
- `noEmis` disables the `_e` emissive.
- `texRange` (EVENT, GIFT) picks a random suffix 0..3 when `tv` is absent.

#### 1.5.2 Generic object keys added or changed in v7

Defaults come from `ObjectInstance.js:372-780`; clamps from the `MapManager` builders.

| key | meaning | default / range |
|---|---|---|
| `tv` | texture or model variant | 0, clamped to the variant count |
| `tro` | texture rotation, degrees 0..360. Legacy `tr` in radians is converted (`map.js:340-343`) | 0 |
| `tsm` | texture scale multiplier | 0 |
| `ten` | texture encoding: 0 Default (Linear) / 1 sRGB / 2 Linear (`config.encodingTypes`, `config.js:948`) | 0 |
| `fct`, `fs` | sprite-sheet frames 0..100 (10 per row); speed 0..10 (×100 ms per frame) | 0 |
| `rs` | render side: 0 front / 1 back / 2 double (editor −1 = prefab default) | — |
| `alt` | alphaTest 0..1 | 0 |
| `ft` | force transparent | — |
| `da` | darken (multiplies the vertex colour) | 0 |
| `msp` | render as a camera-facing sprite | — |
| `ns` | noShoot: bullets ignore the object | — |
| `nai` | AIs ignore the collision | — |
| `gp` | no grapple (`players.js:2553`) | — |
| `ip` | no impulse / knockback (`players.js:3015`) | — |
| `pl` | no player collision (TRIGGER) | — |
| `cpx` | complex triangle collision (§1.6) | — |
| `smm` | hide on the minimap | — |
| `sat` | step sound 0 Default / 1 Wood / 2 Water / 3 Sand / 4 Snow (`config.stepSounds`, `config.js:2045`; `players.js:1608`) | 0 |
| `rsc` | spawn chance 0..100 % for a start-destroyed object at round reset (`MapManager.resetAll:3776`; not under lowPop or comp, `game.js:3466`) | 0 |
| `inm` | interact message, ≤70 chars, regex `[\w:;()?,.\|[\] \-+*$@=^!{}]` | "" |
| `spa` | spin axis 0 y / 1 x | 0 |
| `hbh`, `hbw` | hitbox height / width multipliers 0.1..10 | 1 |
| `cst`, `rcv` | no cast / no receive shadow | — |
| `as`, `ast`, `fcr` | animation speed, start-animation name (≤32), force render | 1, "", — |
| `si` | index into the size pool `xyz`: `s = xyz.slice(si*3, si*3+3)` (`map.js:270-279`) | — |
| `r` | rotation in radians, now on many more prefabs | — |

- **Unchanged keys:** `p`, `s`, `c`/`ci`, `e`/`ei`, `o`, `f`, `l`/`col`, `pe`, `v`, `ab`, `nf`, `bo`, `h`/`hp`, `cdy`, `sd`, `rt`, `rr`, `in`, `tin`, `ts`, `td`, `tox`, `toy`, `tsr`, `tm`, `d`, `sp`, `ms`, `spn`, `aid`, `txs`, `wj`.
- **Removed:** `sad` (billboard ad). The v3 BOT keys were renamed (§2.8).

**Limits** (v7 `config.js:895-944`, v3 `config.js:524-537`; enforced by `customGameConfig.validateMap`, `customGameConfig.js:152-368`):

| limit | v3 | v7 |
|---|---|---|
| aiLimit | 20 | 40 |
| triggerLimit (chain depth) | 20 | 30 |
| triggerActionLimit | — | 5 |
| interfaceIDMax | 1000 | 2000 |
| interfaceLimit (per id) | 20 | 30 |
| signLimit | 20 | 100 |
| signFontMax / signFontMin | 120 / 10 | 240 / 1 |
| objectColLimit (featured) | 3500 (6000) | 4500 (6000) |
| objects (featured) | 5000 (7500) | 5000 (7500) |
| triangleLimit (`tris.length/9`) | — | 5000 |
| lbLimit / anchorLimit / wheelLimit / botLimit / eventObjectLimit | — | 2 / 1000 / 10 / 16 / 10 |
| gui elements | 20/30/40 | 80 |
| spawns | — | ≤20, each an array of 3-8 numbers |
| name / welMsg | — | ≤16, no `<>` / ≤70 chars, restricted charset |
| buildMax (runtime builds) | — | 1200 |

#### 1.5.3 Materials

- The map material is unchanged: `StrippedLambertMaterial` with vertex colours.
- New material inputs: `ten`, `alt`, `rs`, `da`, `ft`, `msp`, `fct/fs`.
- `data/shaders.js` (8 ShaderToy programs in v7, 4 in v3) is used only by store items (`shaderNum`, `render.js:1223`). It is **not** a map material.
- The baked ambient-occlusion pass (`map.js:1323-1450`) skips noVis, complexMesh, health, canDest, noAmb and gate objects, and props in prop mode.

#### 1.5.4 Custom asset id flag letters (`aid`, `MapManager.addAsset:2779-2916`)

The digits are the asset id. Letter flags:

| letter | meaning | v3 |
|---|---|---|
| e | emissive | yes |
| n | normal map | yes |
| s | specular | yes |
| a | alphaTest 0.02 | yes |
| g | glTF | yes |
| m | metalness | new |
| r | roughness | new |
| l | lightmap (uv2) | new |
| p | displacement | new |
| b | bump | new |
| q | glb | new |
| f | fbx | new |

- `g`, `q` or `f` → `loadAnimMesh`, `hasAnim` (targets for Play Animation).
- The validation regex also allows `t`, `d`, `x`, `u`, `c` and `v`.
- KN only has 3 user-asset ids under `assets/user-assets/`.

### 1.6 Collision model changes

**Spatial hash** (`MapManager.applyToQuad:62-110`, `getGridArrays:131-164`):
- Cells are `"x_y_z"` at `config.quadSize`.
- Each AABB is inflated by the player scale and height, and by `borderH` for border objects.
- `borderH` is **500 in v7** (v3 250; `v7 config.js:2403`, `v3 config.js:1592`).

**Triangle collision (`cpx` + top-level `tris`), new:**
- The editor bakes world-space triangles of `cpx` meshes into the map's top-level `tris` array: 9 floats per triangle, rounded (`editor.js:3278-3310`).
- The game loads them into `manager.pendingTriangles` (`map.js:1461-1464`). `genQuads` (`MapManager.js:111-129`) builds triangles with `isTri` and min/max bounds (`Triangle.fromConfig`, `map.js:17-28`).
- `complexColl` objects are left **out** of the AABB grid (`applyToQuad:69`) and skipped by bullets (`game.js:2728`) and by `canSee`.
- `cpx` is disabled when the object is destructible (`addBlock:2489`, `addCylinder:2350`, `addAsset`, `addFixedPlane`).
- `collisions.js` (new, 64 lines):
  - `collBounds(tri, x,y,z, r, h)` does the AABB pre-check;
  - `capsuleColl(tri, x,y,z, radius, height)` → `{normal, depth}`;
  - `triCapsInter(capsule, tri)` computes the plane distance, then the closest points on the edge segments.
- Player response (`player.js:1337-1350`): push out by `normal*depth`, remove the velocity component along the normal; `normal.y>0.7` means on ground.
- **Port precisely:** `collisions.js` is short and self-contained.

**Player-vs-object resolution order** (`player.js:1351-1636`, per grid object, padded by `collidesE`/`collPad`):
1. triangles
2. interact prompts within `tRadius` (perk 14, krum 15, mystery 13, gate 0, bank 1, onInteractKey 7, spinner 9)
3. trigger events (onCollision, onHoldMouse, onEnter/onExit)
4. pass-through objects (isEvent, redirection, serverNode)
5. jumpPad
6. scoreZone
7. qualifying zones (premium, verified, partner, kpd, teamZ, kr) pass through; otherwise they fall through to solid
8. noPVP / forcePVP
9. teleporter
10. checkpoint
11. pickup / flag
12. trigger (flag capture)
13. kill
14. ladder (`dir` facing test)
15. noPCol (skip)
16. solid: ropeLadder climb → wall-jump detection (not for ramps, cylinders or `wj`) → ramp → cylinder (spin carry `diskSpin` 0.0002·spn) → step-up `climbHeight` → top landing (+`borderH` if the object has a border and `disableB` is off) → side push-out → ceiling (with `headClipFix`)

**Bullets / raycast** (`game.js:2728`) skip `noShoot`, `complexColl`, inactive and transparent objects (unless requested).

**AI collision** (`AI.js:830-880`) skips `noAICol`, triggers, flags and enter/exit objects. `redirection` volumes steer AIs.

### 1.7 Player build objects (`data/buildObjs.js`, new)

Blocks are placed with the **Build Tool** weapon (`weapons.js:2002`, `build:true`).

**Placement** (`players.build`, `players.js:2074-2200`):
- ray to the nearest object or terrain;
- pick the face (Left / Right / Front / Back / Top / Bottom);
- snap with the target's `buildSnap`;
- check for overlap with `collisionC`.

| idx | name | size w×h×l | cost | health | places | src | flags |
|---|---|---|---|---|---|---|---|
| 0 | Build Block | 10×10×10 | 10 | 300 | all | grain_0 | snaps on 6 faces |
| 1 | Platform | 30×2×30 | 50 | 500 | all | grain_0 | 1/3-grid snap |
| 2 | Wall | 30×30×2 | 50 | 500 | objs | grain_0 | rotFacing, rotDir |
| 3 | Stairs | 30×30×30 | 25 | 150 | objs | grain_0 | rotDir; custMod → `addRamp` (type 2) |

- Colour `#5e432b` (Stairs `#473220`), `breakSnd:"break_wood"`, icon `img/build/build_<icon>.png`.
- **Server** `game.buildObj(buildInd, x,y,z, w,h,l, dir, uido, grounded, player)` (`game.js:750-785`):
  - costs score (`players.js:2184-2186`);
  - capped at `config.buildMax` 1200;
  - `addBlock(..., {dynamic:true, buildInd, health})` / `addRamp`, then `applyToQuad`;
  - broadcasts `bo`.
- **Clearing:** `removeBuilds` (`game.js:785-806`) clears every dynamic object.
- **Client:** `bo` handler at `index.js:18220`; join sync `dyno`.
- Only the Build Tool weapon uses this; no map field enables it.

### 1.8 Terrain, zone, ocean, sky, environment zones, AO and lightmaps

#### 1.8.1 Terrain (same algorithm in v3 and v7)

**Noise library.** Only the library changed:
- v3: `simplex-noise` 2.4, `new SimplexNoise(seed)`.
- v7: `createNoise3D(alea(seed))`.

Both build the same permutation (Fisher–Yates over 256 with alea) and use the same 0.6-falloff ×32 noise3D (`v7 libs/noise.js:67-180, 314-330`; v3 `libs/Terrain.js:240-241`). Treat the two as bit-identical.

**KN already has a pure-C port** in the editor: `KN client/src/editor/editor_terrain.c:20-490` (simplex 2.4 + alea).

**Generation** (`v7 libs/Terrain.js`):
- Setup (`:153-195`):
  - `sizeMlt` is forced to 5.7.
  - `sizeX/Y = limitMM(width/height, 0, 8000)`.
  - `mntMlt = limitMM(terrainMntMlt, 0, 5)`.
  - `edgeSize = 500`.
  - Cells are 25 units: `xSegmentCount = floor(sizeX/25)`.
- Ease function: `easing[EASE_LIST[terrainEase]]`. Default `terrainEase` is 12 (`easeInOutQuint`).
- Biomes:

| Biome | Power | Height term | Extra |
|---|---|---|---|
| GRASS | 2100 | `floor(sample(300)·4)/4·18·w·5.7 + w` | slopeSton 0.8 |
| MOUNTAIN | 1900 | `abs((1−abs(0.4+0.3·s200 − s500)·2)·(0.4+1.4·s400)·mntMlt·220·w)·5.7` | — |
| SNOW | 1000 | `floor(s300·3)/3·15·w·5.7 + 90w` | forceCol SNOW, slopeSton 0.75 |
| HILLS | 2100 | `abs((1−2·s2000)·(0.4+0.4·s400)·mntMlt·−100·w)·5.7` | forceCol GRASS, slopeSton 0.7 |

- Sampling (`:540-555`): `_sample(x,y,s) = noise3D(x/s, y/s, ++sampleIndex·10000)/2 + 0.5`. `sampleIndex` resets on every `evaluate`, so **call order matters**.
- `evaluate(x,y)` (`:488-516`):
  1. Sample each biome at its power and normalise.
  2. Sort the biomes in descending order.
  3. Weight them: `w[top] = ease(ease(a/(a+b)))`, `w[i] = ease(ease(v/(top+v)))`.
  4. Height = Σ genTer, multiplied by `fade = edgeFade(x)·edgeFade(y)` (easeOutQuint), plus `(fade−1)·2000`.
  5. Outside the bounds the height is −1000.
- Colour (`:517-539`), in order:
  1. height > 100 → MOUNTAIN; > 5 → GRASS; otherwise DIRT.
  2. The biome's forceCol, if any.
  3. > 950 → SNOW.
  4. ≤ −10 → SAND.
  5. > 250 → MOUNTAIN.
  6. Steep slopes → MOUNTAIN, where steep means `(1−atan2(|nx|,|nz|)/2π)·(1−atan2(|ny|,|nz|)/2π) ≤ slopeSton‖0.85`.
- Colour defaults: DIRT `#8c6835`, GRASS/HILLS `#4a6904`, MOUNTAIN `#a0a0a0`, SAND `#967f51`, SNOW `#e8fdff`.
- Vertex AO `calcAOWeight`: ≥0.5 → 0.65, ≥0.2 → 0.3, ≥0.1 → 0.25.
- Terrain objects are placed only when `terrainTSeed` is set, using `_sample3D` (`:70-150, 339-366`; `MapManager.addTerrObj:1084-1307`). Positions are `[x·5.7, z, −y·5.7, biome]`.

| Object | Max count | Density | Max height | Side |
|---|---|---|---|---|
| TREE | 2000 | 0.18 | 230 | both |
| STUMP | 200 | 0.02 | — | both |
| LOOT | 50 | 0.02 | — | both |
| ROCK | 500 | 0.075 | 400 | both |
| BUSH | — | — | — | client only |
| GRASS | — | — | — | client only |

- Mesh: rotated −π/2 about x, scaled 5.7 in x and y.
- **Server use:**
  - Player ground: `terrain.raycast(x, −z, 10000, 0, 0, −20000, true)`. If `y ≤ hit.z`, snap to the hit, set `onGround` and zero `vy` (`player.js:1779-1797`).
  - Bullets and line of sight use terrain `raycast` (`players.js:2083-2090`, `game.js:2750-2760`, `projectiles.js:215`).
  - `heightAt` has an argument bug and is unused.
- **KN:** move the pure generator from the editor into `shared/` so the server and the sandbox can raycast it. Test map: `assets/maps/custom/ClashRoyale.json` (the only terrain map in the corpus).

#### 1.8.2 Battle-royale zone (`v7 libs/Zone.js:1-178`; v3 `libs/Zone.js`)

`CONFIG_PROPS` grew from 6 to 9 keys.

**Parameters:**
- `_size = limitMM(zoneSize‖50, 0, 10000)`
- `_height = limitMM(zoneHeight‖1000, 100, 10000)`. v3 hard-coded 1000.
- `circleShrinkSpeed = 2.5·limitMM(zoneSpeed‖1, 1, 50)`
- `waitTimer = limitMM(zoneWait,5,500)·1000` (new)
- `stages = limitMM(zoneStages,0,10)` (new)

**Server tick** (`game.js:4936-4955`):
- Every frame: `zone.update(dt·deltaMlt/1000)`.
- Every 1000 ms: broadcast `zn [cx,cz,sizeX,sizeZ]`.
- Every active player outside the circle (`dist2D > circleSize.x`) takes **10 damage**. Players on the "Infected" team are exempt.

**`update`:**
- Moves size and position toward the target at `shrinkSpeed·dt`.
- When it reaches the target and `stages` remain:
  1. Push a `waitTimers` entry `{time, contTime, canDMG, msg:"Zone closing in "}`.
  2. After `zoneWait`: `stages--` and `nextCircle()`.
- `nextCircle`: the target point is random within half the current size (if `zoneRnd`); the target size is `stages ? size/2 : 0`.
- v3 shrank once, straight to 0.

**Client:** tweens to each received value over 900 ms and hides the zone at size 0.

**Sync and reset:** joiners get `getSyncData().zone` (`game.js:3847`). `resetZone` runs each round (`game.js:2847-2851`).

**Visual:** 32-segment open cylinder, texture `zone_0` repeated 20×, gradient `zoneCol0..2`.

#### 1.8.3 Ocean (NEW, client only)

`Ocean.js`, `Water.js`, `render.js:388-394`.

- A `Water` shader plane, `ocW × ocL` (100..20000).
- Parameters:
  - opacity `ocOpac` 0..1
  - distortion `ocDist` 0..10
  - speed `ocSpd` 0.1..5 (`time += dt/1000·speed`)
  - y offset `ocYOff` −1000..1000
- Reflection resolution is `[32,64,128,256,1024,2048][ocRefRes−1]`. The user setting `reflectionQuality` overrides it.
- Normals come from `textures/water_0.png`; sun direction and colour come from the sky light.
- Exported only when `ocean` is true. Fixture: gameTemplates 1 "Ocean" and 4 "Pirate" (§4.2).

#### 1.8.4 Sky dome (`SkyDome.js:6-66`)

- Sphere: radius 50, 32×16 segments, BackSide.
- Gradient shader mixing `skyDomeCol0/1/2` (mix at h=0.5).
- Texture `sky_6` or `<skyDomeTexA>/texture`.
- Emissive: white when an emissive texture asset exists, otherwise the `skyDomeEmis` colour (`#000000` means none). Emissive map is `<skyDomeEmisTex>/texture` or `<src>_e`.
- Scroll: `skyDomeMovT/1e6` along axis `skyDomeMovD`.
- Skipped when the mode sets `skyCol`.

#### 1.8.5 ENVIRONMENT_ZONE (prefab 79, NEW, client only)

**Stored keys** (`MapManager.js:879-926`):
- sound: `ambInd`, `ambIndC`
- sky dome: `skyDome`, `skyDomeCol0-2`, `skyDomeEmis`, `skyDomeEmisTex`, `skyDomeTex`, `skyDomeTexA`, `skyDomeMovD`, `skyDomeMovT`
- shadows: `shadowR`, `shadowD`
- lights: **`ambientC`**→ambient, `ambientI`, `light`, `lightD`, `lightI`, `sunAngX`, `sunAngY`
- sky and fog: `sky`, **`fogC`**→fog, `fogD`
- output: `correctLights`, `toneMapping`, `toneMappingExposure`, `outputEncoding`

Note the renames: the object uses `ambientC` and `fogC`, not `ambient` and `fog`.

**Behaviour:**
- Every frame the client tests the **camera** position with `pointInBox3D` against `envZones` (`index.js:17004-17016`).
- On entering a zone it calls `RENDER.updateEnvironment(idx)`, which re-runs the environment init (lights, fog, sky dome, tone mapping), updates the shadow map and switches the ambient sound. Leaving restores the map's environment (`−1`) (`render.js:2698-2723`).
- `ambFadeTime` and `gammaFactor` appear in the corpus but are ignored.

**KN:** this needs `RenderLighting` to become swappable (`KN client/src/main.c:131-213`, `sky.c`, `shadow.c`). The directional light used by the shadow pass must follow the active environment (AGENTS.md shadow rules).

#### 1.8.6 Ambient-occlusion planes and lightmaps

- **Generator:** same as v3 (`v7 map.js:1361-1460`, v3 `:1056-1470`).
- **Gate changed.** v7 generates AO planes only when `ambientShading` is set, the device is not mobile, and at least one of these holds:
  - `oldAO`
  - `groupData.official`
  - `!hasLM && custom`, where custom means `mapIndex==0 && name!="Burg"`, or `isCustom`

  As a result, bundled v7 maps (which ship asset lightmaps) get **no** AO planes unless they set `oldAO` (freight and lagoon do). v3 always generated them.
- **Decision for KN:** KN's official maps are the v3 maps and have no baked lightmaps. Under the v7 gate, KN's official maps count as `official`, so AO stays on for them. Custom maps follow `hasLM`/`oldAO`.
- **Lightmaps** are per asset: the `l` flag in `aid` loads `<asset>_l` as `lightMap` (intensity 1.5) using `uv2` (`render.js:1263-1266`, `MapManager.js:2117, 2903`).

### 1.9 Embedded game config (`config` key)

**Shape:**

```json
{"modes":[…], "classes":[…], "settings":{serverConfig varN: value},
 "weps":{"<weaponIdx>":{weaponConfig…}}, "cls":{"<classIdx>":{classConfig…}}, "prks":{"<effectIdx>":{effectConfig…}}}
```

- `cls` and `prks` are new in v7. v3 had `{modes, classes, settings, weps}`.
- The editor exports only non-default values (`editor.js:3448-3523`).
- Validation: `customGameConfig.validateConfigs` (:8-151), 15000-character cap.

**How it reaches a game:**
1. **Host Game.** When raw map data is pasted, the map's `config` becomes the host preset (modes default `[0]`) (`index.js:6435-6450`). The server then runs `setupGameConfig(hostIndex, requested)` → `game.applyConfig(cfg, 3, hostIndex)` (`server/index.ts:1224-1260`, `customGameConfig.js:369-448`).
2. **Sandbox / editor test.** `parseSandboxMapConfig` (`index.js:18366-18404`) flattens `settings`, adds `weps cls prks classes modes modURL welMsg`, sets `maps:[0]`, defaults modes to `[0]` and classes to 0..13.
   - **v7 bug:** the serverConfig loop then reads `vO374.settings`, which is undefined after flattening, so every server setting resets to its default. Map `settings` are effectively ignored in the v7 sandbox.
   - **Decision:** KN's offline mode is a sandbox *of the real server*, so it uses the server path (1). Map `settings` are applied, as they would be when the same map is hosted.
3. **Matchmaking.** `config.modes` lists the modes a map can be matched with (`game.js:5068-5096`). The server falls back to the top-level `modes` (`server/index.ts:1164-1176`).
4. Nothing applies `config` to a public game automatically; it is a host preset. The option tables are in §4.3.

**v3 `map.game`:**
- The v7 runtime ignores it.
- KN's game runtime already ignores it in effect: `map->config = g_default_map_config` (`KN shared/src/map.c:337`), and no `game` key is ever parsed.
- **Decision:** the game keeps ignoring it, as v7 does. `MapConfig` (`KN shared/include/shared.h:270-286`) is replaced by the per-class `cls` overrides (§4.3).
- The KN editor's import does what the v7 editor does: copy every `game` key into every `cls[i]` (`editor.js:4036-4049`, `aID` only if empty).
- Note the camType change: v3 option 1 was Top Down, v7 option 1 is 3rd Person. The v7 editor copies the value unchanged; KN does the same.
- `arA` changes from a multiplier (v3, default 1) to an absolute acceleration (v7, default 0.0047). The v7 editor does not convert it either.
- No shipped KN map has a `game` key.

### 1.10 Versioning and compatibility: how v7 loads v3 maps (the KN compat spec)

There is **no version field**. Compatibility is additive: absent keys take their v7 defaults and unknown keys are ignored.

KN must reproduce exactly these v7 behaviours for old maps:

| # | v3 input | v7 behaviour (adopted) | Ref |
|---|---|---|---|
| 1 | `game` block | ignored at runtime | §1.9 |
| 2 | SPECTATE_CAM (33) with `obi` | read as CAMERA with `obl`, so objLink = 0 | `map.js:1001` |
| 3 | TERMINAL (41) | nothing is built (no object, no uid). Its sibling objects keep the same relative order | `map.js:646-647` |
| 4 | BOT (55) | parsed as AI_SPAWNER. The old BOT keys (`aid spd tsd grv fr vd idl dth atk agr sp pj rew bh cs cnh xv ms ain tc tct tce te ct cha ta tin es esa ev er ef`) are a subset of the new ones | `MapManager.buildAIData` |
| 5 | covered CUBE top faces | no longer culled automatically (only `f` counts) | v3 `map.js:394-409` |
| 6 | `dyn`, `grv` on CUBE; `sad` on BILLBOARD | ignored | — |
| 7 | BOOST_PAD, PLANE, BILLBOARD, LIQUID, SIGN rotation | interpreted in the v7 argument order (§1.3); rotated old planes can therefore look different, exactly as in v7 | `map.js:788, 862, 908, 948, 969` |
| 8 | `pe` truthy | treated as a 0..1 fraction (`limitMM`), so `pe:1` means full damage through | `players.js:3172-3176` |
| 9 | single-action trigger keys | wrapped into `tas[0]`, including the `tce`→value-type quirk | §3.4 |
| 10 | `t` as a string | not resolved by the game; falls back to texture 0 | §1.5.1 |
| 11 | `tr` (radians) | converted to `tro` (degrees) | `map.js:340-345` |
| 12 | spawns of length 3..6 | `[6]` teamOnly and `[7]` spread default to off | `map.js:1466-1480` |
| 13 | `camPos` of length 3 | static=false, rotation 0 | `index.js:13687-13693` |
| 14 | wall-jumpable surfaces | every solid non-ramp, non-cylinder box is wall-jumpable unless `wj` | §2.1 |
| 15 | `bo` borders | 500 units high (v3 250) | `config.js:2403` |
| 16 | onTimer with `tce` 0 | fires every 1 s (v3 fired once) | §3.5 |
| 17 | event 5 onRespawn | fires without a `destroyedBy` | §3.5 |
| 18 | mode index 13/14 in `config.modes` | Classic FFA / Deposit (v3 ranked) | §4.1 |
| 19 | OBJECTIVE order | by uid from `obi` (1..10 → uid 0..9); otherwise auto-assigned in file order | §2.3 |
| 20 | emitter sound index 16 | `scare_0` (v3 `jumpS_0`) | §3.3 |

**Load-time shims v7 performs** (`map.js:262-345, 538-556`):
- `si`/`xyz` expansion
- `ci`/`ei` colour lookup
- `f` hex → bits
- `tr` → `tro`
- legacy trigger → `tas[0]`
- aliases `id`, `hp`, `wid`
- `at` video detection (`v`)

KN implements all of them in `shared/src/map.c`. `si`/`xyz` already exists in `map_catalog.c:240-270`.

**Server generation note.** v7 generates maps in a `worker_threads` worker (`mapWorker.js`). The worker serialises `config.mapGenProps` (manager lists, spawns, terrain/zone configs, deathY) and the main thread rehydrates the `GameObject`s and the `flagObj` references. If the worker fails, v7 falls back to a synchronous `generate(..., true)` that skips faces, colours and textures (`map.js:72-170, 1492-1531`). KN parses synchronously and only needs the same object order (§1.3).

## 2. Map-driven runtime systems

### 2.0 Engine structure (v7)

**One Game class for every role.** `data/game.js` is a single `Game` constructor used by:
- the server (`serverIO` set, `isServer`);
- the client (`RENDER` set);
- the in-browser **sandbox** (`isSandbox`, `serverIO={send(){}}`, `game.sandbox=IOSandbox`).

Authority checks are always `game.isServer`/`serverIO` (`game.js:29-37`).

**Sandbox routing.**
- `send`/`broadcast`/`broadcastEx` go to `game.sandbox` when `isSandbox` is set (`game.js:5165-5173`).
- `IOSandbox` (`libs/io-sandbox.js`, 37 lines) delivers `send(sid,label,...)` directly to the client's handler table when `sid` is null or equals `socketId`. It deep-clones only `"end"` payloads.
- Sandbox setup: `initSandboxGame` (`index.js:18275-18330`) and `connectToSandbox` (`:18405-18560`).
- Input reaches the sandbox through `sandboxPlayer.procInputs(...)` (`index.js:16842-16844`). Interactions call `sandboxGame.players.interact(...)` directly (`:16503-16514`).
- **This is the model for KN's offline-as-sandbox.** KN already routes authority through `game_is_authority` (`KN shared/src/game.c:212`) and `GameHooks.send`.

**Tick and send rates.**
- Server tick: `setInterval(tick, config.serverTickRate)` (`game.js:5174-5180`). `tick()` runs `update` → `syncState` → `updateState` (`:4728-4742`).
- Rates (`config.js:33-37`): tick 30 Hz, `"k"` send 10 Hz, `"l"` sync 1 Hz.
- Per-lobby overrides: `applyNetworking` (`game.js:325-337`; `mode.sendR`, `sendRateOvr`, `tickRateOvr`). They are sent to clients as `networkData` in `init` (§5). This is **NEW** (v3 rates were fixed).

**Server `update()` order** (`game.js:4743-4997`; v3 `game.js:3002-3277`). KN's tick must keep this order:
1. timers, waitTimers, `endTimer`, `lockoutTimer`
2. streaks
3. vote-kick and forfeit
4. bomb
5. **trigger `timerObjs`**
6. **`triggerQueue`**
7. objective time
8. power-ups
9. bots
10. players
11. player sounds
12. AI
13. pets
14. airdrop
15. teleporter cooldowns
16. spinners
17. destructables
18. flags
19. spawnables
20. dom zones
21. mystery boxes
22. krum machines
23. `mode.update`
24. zone
25. projectiles
26. `mainScript.update` (KrunkScript)
27. per-player `interProg`/`interMProg` → `p`/`mp` to spectators
28. team scores `ts`
29. leaders

### 2.1 Player ↔ map-object collision

The main loop is `player.js:1330-1720` (`procInputs`). It runs on **both** client (prediction) and server (authority). v3 equivalent: `players.js:880-1230`. The resolution order is in §1.6.

| Feature | v7 semantics | Status | Ref |
|---|---|---|---|
| Object source | quad grid `getGridArrays(x,y,z)` (`quadSize` 150, v3 50); each `sid` once per tick | changed constant | `player.js:1333-1379` |
| Triangle collision | §1.6 | NEW | `player.js:1337-1351` |
| Terrain ground | raycast snap (§1.8.1) | NEW at runtime | `player.js:1779-1797` |
| Proximity interact prompts | perk 14, krum 15, mystery 13, gate 0, bank 1, onInteractKey 7, spinner 9; needs `teamCheck` + `collides(o,tRadius)` | v3: gate/bank/terminal (2) only | `player.js:1352-1371` |
| onCollision / onHoldMouse / onEnterObject / onExitObject | §3.5 | changed / NEW | `player.js:1381-1408, 1650-1664` |
| Jump pad | `vel = bounce·0.1 / (crouched && crouch ? 2 : 1)`. Direction `d`: 0 up, 1 down (`velocity.y=-v`), 2..5 horizontal (velocity.x/z from yaw −90/90/90/180, see code). Sets `onGround=false`, `onPad`. Plays `bounce_0` locally once per pad entry (`wasOnPad`). Server challenge `"bst"`. Several collision branches skip zeroing `velocity.y` while `wasOnPad` (`:1594, 1617, 1641`) | **changed** (v3 vertical only, `velY=bounce`) | `player.js:1415-1452`; v3 `players.js:944-952` |
| Score zone | `scoreObjs[sid]` once per life: `score(scoreP)`, then `players.logTime(player)` when `game.doLogTime`: `timeLog++`, send `gt(time,timeLog)`. On the last zone: chat "X finished in T" and `t` | changed (logTime NEW) | `player.js:1453-1458`, `players.js:4040-4054` |
| Qualifying zones | premium (`premiumT`), verified (`featured`), partner (`partnerApp>=2`), kpd (`canFlag`), teamZ (`teamCheck`), kr (`manager.enoughKR(krs)`): pass through if the player qualifies, else solid | partner/kpd/kr NEW; teamZ uses `teamCheck` | `player.js:1459-1470`, `MapManager.js:612-878` |
| NoPVP / PVP zones | inside sets `player.noPVP` / `player.forcePVP`. Explosions, projectiles and hitscan skip pairs where either side is noPVP or the forcePVP flags differ | NEW | `player.js:1471-1477`, `game.js:880`, `projectiles.js:183-191` |
| Pass-through objects | `isEvent`, `redirection`, `serverNode`, `noPCol` | NEW | `player.js:1509` |
| Teleporter | §2.3 | changed | `player.js:1480` |
| Checkpoint | §2.3 | changed | `player.js:1482` |
| Kill zone | resets the carried flag; the server kills unless godMode. Death zones can be destructible or interface-toggled | changed | `player.js:1492-1499`, `MapManager.js:560-591` |
| deathY | `y <= map.deathY` kills (players **skip godMode**; v3 did not), also AIs and airdrops | changed | `players.js:4201-4207`, `AI.js:955`, `airDrop.js:177` |
| Ladder | below the top, not crouched, **no grapple**: `velocity.y=0`, `onLadder`. Climb `ladderSpeed(0.035)·class.ldA·weapon.spdMlt·dt·f`, with `f=(|angleDst(obj.dir, moveDir−xDire)|−π/2)/(π/2)`. Decel 0.97 | changed (grapple check) | `player.js:1500-1508`, `config.js:2118-2119` |
| Rope ladder | climbable from any facing (uses the player's own yaw); still solid afterwards | NEW | `player.js:1512-1535` |
| Wall-jump surface | **opt-out**: any non-ramp, non-cylinder solid taller than `climbHeight` (3) qualifies unless `notWallJ` (`wj`). The jump itself is shaped by mode/class `wallJ*` and `config.wallJP` (`player.js:694-695, 1220-1222, 1955-1971`) | **changed** (v3 opt-in `wallJumpable`) | `player.js:1521-1537`; v3 `players.js:1038-1066` |
| Ramp + boost | height by `progressOnLine`; boost ramps add `boost·boosterSpd(0.002)·dt` along `dir`/`boostDr`; `rampFix` | same | `player.js:1535-1565` |
| Cylinder + spin | top landing; standing rotates the player by `spin·dt·diskSpin(0.0002)` and sets `xVelC`/`zVelC` (also sent in `"l"`) | changed (sync) | `player.js:1566-1607` |
| Borders | `borderH` 500 (v3 250); `disableB` | changed | `config.js:2403` |
| Head clip fix | `config.headClipFix` uses `heightOld` while uncrouching | NEW | `player.js:1628-1646` |
| Mode hook | `mode.collisionCheck(player, obj)` | NEW | `player.js:1688-1693` |
| Airdrop claim | hold interact (`input[14]`) for `claimTime` within radius 6; interact type 10 | NEW | `player.js:1718-1729` |
| Spawnables | touch → `pickupSpawnable`; a `stopInteract` string gives a type 12 prompt | changed | `player.js:1741-1749` |
| Liquid | **render only**; no swimming exists in v7 (no swim/inWater anywhere in `data/`) | same | `MapManager.js:3636-3667` |

### 2.2 Destructibles, object state, moved and animated objects

- **Damage**, server only, and only while no blocking waitTimer is running (or `waitTimers[0].contTime`):
  - hitscan: `players.js:3100-3128`;
  - melee: `:2420-2465`, adds onPunch when `!canThrow`;
  - projectiles: `projectiles.js:249-287`, adds onThrowingMelee for weapon 12, and onProjectile.

  At health 0: `active=false`, `destroyed=true`, `destroyedBy`, push to `destObjs`, broadcast `do uid`, fire onDestroy, then onShoot/onDamage. Same as v3 apart from the waitTimer gate and the new events.
- **Respawn** (`updateDestructables`, `game.js:3316-3342`):
  - `respawnR` → random 1..5000 ms;
  - `respawnT` is clamped to ≥100 ms (`GameObject.js:132-143`);
  - on respawn: broadcast `ro`, fire onRespawn (**no `destroyedBy` needed in v7**).
- **Round reset** (`MapManager.resetAll`, §3.8):
  - `active = !startClosed && !startDest && !mysteryBox`;
  - the `rsc` spawn chance is skipped when `lowPop` (pub with ≤2 clients, or comp; `game.js:3466`).
- **Client mirror:**
  - `destroyObj` (`game.js:729-749`) hides the object, plays `breakSnd`, and removes builds;
  - `respawnObj` (`:1634-1653`);
  - join: `syncAll(dest)` (`MapManager.js:3807-3821`).
- **Moved objects** (NEW, `game.js:809-858`):
  - `teleportToNode(objIndex,nodeIndex)` moves the object so its feet sit at `pathNodes[n]`, re-quads it, records `movedObjs=[uid,node,…]`, and broadcasts `mo`;
  - `teleportFromNode` restores `orgX/Y/Z` and broadcasts `mf`;
  - joiners receive `sync.mov`.
- **Animated objects** (NEW, `game.js:288-310`):
  - `playAnim(obj, clip, force, raw)` sends `ana uid,clipIndex|name,raw,true` to active and spectating players;
  - `clipNames=["Idle","Move","MoveFast","Jump","Melee","Shoot","Crouch"]` (`config.js:2091`);
  - client handler `io_servAnim` (`index.js:15085`); a 4th argument of true means a map object, otherwise an AI.
- **Player looping sounds** (NEW, `game.js:3955-4000`): `asnd`/`rsnd`, synced as `sync.asnd`.

### 2.3 Gates, banks, checkpoints, teleporters, parkour reset, objectives

- **Gates** (`game.js:2796-2834`, interact `players.js:3433-3447`):
  - toggled by `int 0,uid` when `(active || closeable)`, inside `tRadius`, `teamCheck` passes, and the **method** `md` allows it:

    | `md` | requirement | pays? |
    |---|---|---|
    | 0 | score ≥ `sp` | yes |
    | 1 | score ≥ `sp` | no |
    | 2 | kills ≥ `sp` | no |
    | 3 | account | no |
    | 4 | verified | no |
    | 5 | premium | no |
    | 6 | partner (`partnerApp>=2`) | no |
    | 7 | level ≥ `sp` | no |

    v3 had methods 0-2 only.
  - broadcasts `gte uid,active`;
  - `resetGates`: `active=!startClosed`;
  - prompt: `interactMsg` overrides "Open gate" (`players.updateInteract`, `players.js:3815`).
- **Banks**: deposit with `int 1,uid`; withdraw with **`intSec`** (v3 used `int` with a secondary flag). `depositAmnt`/`withdrawAmnt`; broadcasts `bnk uid,deposited` (`players.js:3470-3496`).
- **Checkpoint** (`setCheckPoint`, `game.js:2997-3011`):
  - same as v3, plus it stores **`checkPointData=[timeLog, gameTime]`** and uses `teamCheck`;
  - sends `chp`; `chp true` resets the client checkpoint (`players.js:3994`, client `index.js:15735`);
  - singleUse checkpoints are tracked in `checkPointList`.
- **Parkour reset** (NEW; only when `mode.allowReset`) through input `[13]`:
  - 1 = `resetToStart`: back to `initPos`; clears time, score and checkpoints; `chp true`;
  - 2 = `resetToLast`: to `(cp.x, cp.y−height+0.01, cp.z)`, zeroes velocity, applies the `forceDir` yaw, and restores `timeLog`/`gameTime` when `config.lstChkT`.

  Refs: `player.js:2006-2009`, `players.js:3974-4039`.
- **Teleporter** (`checkTeleport`, `game.js:3012-3032`):
  - same channel and node rules as v3: sender `node!=1`; receivers `node>0`; node 2 sets a `telCooldown` of 2000 ms; `teamCheck` on both ends;
  - **NEW `tcp`** teleports to the player's checkpoint object at `y−height+0.01`;
  - moves via `changePosition(p,x,y−h,z,recon=false,telStopMo,forceDir?dir:undefined)`;
  - client and server both run it (prediction);
  - cooldowns tick in `updateTeleporters` (`:3033-3043`).
- **changePosition** (`players.js:4304-4335`):
  - sets the position, optional yaw `−(dir+1)π/2`, and optionally stops velocity (zeroes `stepVal`/`lastXYZ` too);
  - calls `endGrapple`;
  - a NEW 8th argument broadcasts `chgp id,x,y,z,recon,stop,dir`.
- **Objectives** (`game.js:2852-2913`):
  - `activeObjective` is an object **uid** (v3 used an array index);
  - `getNextObjective` takes `uid==active+1`, else the first one; `randObjective` exists;
  - the duration is `config.objtvTime` minutes (v3 a fixed 60 s);
  - broadcasts `obj uid,timeMs,isInitial` and fires `onObjectiveSwitch`;
  - `uobj uid,onZone` reports hill occupancy;
  - scoring moved **into the modes** (§4.1). v3 scored generically in `game.update` (`v3 game.js:3188-3209`), and KN must remove that generic tick.

### 2.4 Zone, environment, terrain

See §1.8.

### 2.5 Spawn point selection (`getSpawnPoint(team, player, useFirst, compMode)`, `game.js:952-1049`; v3 `:749-849`)

- Skips `comp` spawns outside comp mode, and `teamOnly` spawns that belong to another team.
- **`flippedSpawns`** swaps teams 1 and 2 (set on a round swap).
- Scoring otherwise matches v3: fewest enemies with line of sight, then the largest summed distance to enemies plus enemy flags. **NEW:** domination zones not owned by your team are added to the distance metric.
- A chosen spawn with `spread>0` returns `getPointSpread(x,z,y,spread+7,dir)`: random angle, a ray against active object AABBs to clamp the distance, then `spread−=10`. Comp spawns in the waitTimer `comp` phase use spread 40.
- KN's picker (`KN shared/src/modes/mode_round.c:355-453`) documents its own deliberate deviations. Under the v7 policy, re-verify it against v7 and add `teamOnly`, `spread`, `flippedSpawns`, dom weighting and `compMode`.

### 2.6 Weapon pickups (`players.js:3282-3378`; v3 `4480-4545`)

- The client sends **`int 6,uid`** for one targeted pickup (`lastInteractData`). v3 scanned every pickup.
- `md` methods 0..7 work as for gates.
- `mode.unrestrictedWeapons`: rebuy or upgrade at half price, or 4500 if the weapon is already upgraded.
- Ammo travels with the pickup (`pickupAmmo`). `pickupRep` swaps your old weapon into the pickup together with its ammo.
- The loadout is sorted by weapon type, keeping ammo, reserves and upgrades aligned.
- Broadcast: `inat sid,loadout,idx,uid,pickupLeft,active,true,null,ammos,reserves,upgrades,true` (12 arguments; v3 had 6).
- `resetPickups` (`game.js:2621-2657`) chooses `pickupRand`/`pickupRandGroup` (the same random weapon per `rwg` group) at round reset. Bomb pickups are active only in site modes.
- Drop: `drpw` → `players.dropWeapon` (`server/index.ts:3323`).
- **KN:** pickups are not ported at all (`KN shared/src/interact.c:6`).

### 2.7 Zombies machines and power-ups (NEW; map-placed, usable in any mode, owned by zom)

- **Mystery box** (`game.js:2201-2419`; interact 13, `players.js:3379-3407`):
  - states: 0 idle → 1 rolling (`mysteryBoxAnim` 5000 ms) → 2 collect (`mysteryBoxDespawn` 15000, teddy 8000) → reset;
  - price `scoreP`, 10 during fire sale, halved by `sharingIsCaring`;
  - 20% teddy chance, which moves the box (`switchMysteryBox`);
  - weapons are rolled from `weapons.mysteryBoxRarities`;
  - packets `mybx` (6 fields per box) and `smybx uid,enable`;
  - constants `config.js:937-943`.
- **Krum machine** (`game.js:2420-2588`; interact 15): upgrades the held weapon; packet `krm`.
- **Perk machine** (interact 14): `givePerk` with `zp`; `config.maxZombiePerks` 4; +50 score once for crouching on it (`player.js:1385-1389`).
- **Power-ups** (`game.js:1409-1450`): `instaKill`, `fireSale`, `doublePoints` timers; packet `pwup i,ms`.

### 2.8 Map AI (`AI_SPAWNER`): `data/AI.js` (1336 lines) vs v3 `data/ai.js` (681)

**Data and limits.**
- Data: `MapManager.buildAIData:2929-3035`; clamps in `addAI:3036-3271`; `addPendingAIs:3272` honours `sd`/`rsc`.
- `aiLimit` 40 (v3 20). `addAI` is skipped when `RENDER` is set, so the server is the only spawner.

**AI keys** (all NEW unless the BOT row says otherwise):

| Group | Keys |
|---|---|
| BOT keys kept from v3 | `aid spd tsd grv fr vd idl dth atk agr sp pj rew bh cs cnh xv ms ain tc tct tce te ct cha ta tin es esa ev er ef` |
| Pathing | `phi` (comma list of PATH_NODE ids); `pht` path type: Loop / Do Once / Ping Pong / Random (`config.pathTypes`, `config.js:2327`) |
| Roaming | `rmr` radius 0..500; `rmt` 1000..10000 ms |
| Targeting | `tm` team; `tAi` target AIs; `tPl` target players (default on); `furthTarg`; `xv` x-ray; `aif` friendly; `aic` chase distance |
| Physics | `pmv` (gravity, ground and air decel as for players); `grv` 0..1; `nai` |
| Stuck | `igos` ignore target; `dos` drift; `ivos` invincible after 2000 ms stuck |
| Behaviour `bh` 0..20 | 1 Weeping (orb ring, blink); 2 Lord of Souls (spawns Jack O' Lanterns / Lost Souls); 3 enrage; 4 mirror image; 5 Drowned Sailor (`AI.js:521-617`) |
| Melee | `mlr mlra mld mldy hr mls` |
| Projectiles | `pra prg prsc prms prs pjs pjd prgl prgt exp`; `noObjC` for bh 5 |
| Shooting | `aac` spread; `shb sha shof shdy ss` |
| Effects | `se`/`he`: effect index on shot / hit |
| Crits | `crd`/`crh` |
| Sounds | `idl dth atk atkh mel melh agr` |
| Render | `hbh hbw ms myo mro as ast sor spn spa fct fs bbS bbD ten nf rs ft txs alt o` |
| Rewards | `rew` skin ids; `rewm` 0 give / 1 drop at AI / 2 drop at node `rewn`; `rewr` 0 all active / 1 damagers / 2 last hitter. **Public games only** (`AI.js:1096-1140`); KN: no-op |
| Triggers | `ct` + `tas` (onAI* events only, §3.1) |
| Spawn | `cdy sd rsc rt rr` |

**Runtime** (`AI.js:445-970`):
- targets via `findNearestTo`/`findFurthestFrom` (`game.js:2669-2710`), including AIs and teams;
- path following: arrival distance 1, advance by path type (`AI.js:546-552, 708-735`);
- roaming;
- physics;
- map collision skips `noAICol`, triggers, flags and enter/exit objects (`AI.js:830-880`);
- `AI_REDIRECTION` volumes push the AI along `dir`/`boostDr` (`AI.js:853-857`);
- stuck handling; effect and zap ticks; deathY;
- kill (`AI.js:1096-1140`): onAIDeath, `mode.onAIDeath`, rewards;
- onAIDamage, onAIHalfHealth (`AI.js:1065-1068`) and onAIKillPlayer (`:748`).

**Sync** (`AI.js:1239-1311`):
- `ai`: stride **9**, `[sid,x,y,z,xD,yD,hp,didMov,team]`, to players within **800** units. v3 used stride 8 (`tFrame` instead of `didMov`/`team`) within 600.
- `aai` is `getData()`, which now has many more fields (`AI.js:386-417`).
- NEW `rai sid` removes an AI.
- `ana` plays AI clips (4th argument falsy).
- Clients interpolate over `1000/aiSyncT` ms (`AI.js:453-462`).

**Zombie AI templates** (`modes/zom.js:2-343`) use the same data format.

#### 2.8.1 Map bots (`BOT_SPAWNER`, NEW)

- `buildBotData`/`addBot`/`addPendingBots` (`MapManager.js:3285-3370`).
- Keys:
  - identity: `btn` name ≤16, `btc` class, `btl` lives, `btt` team;
  - multipliers: `bthm` health, `btdm` damage, `btspm` speed, `btrm` range, `btsm` spread;
  - behaviour flags: `btfc bttc bttt btfcr btat btft btwt btmt btsh btcr btsd btrd btny btno btjm btlm btvc btgm btr btrs btrc`;
  - cosmetics (−1 = none): `btcpr btcd btcf btcht btch btcbk btcb btcw btcwr btcs btcp btcm`.
- `botLimit` 16. Bots are added on the server when the game is a sandbox, dev or pub (`game.js:3494-3499`) via `bots.add(pos,true,true,data)`.
- v7 `bots.js` uses the map only through `getSpawnPoint` (`bots.js:108`); v7 has no navmesh.
- **KN:** TFUE bots exist (`KN shared/src/bots.c`, own navmesh `bot_nav.c`). Hook BOT_SPAWNER into them.

### 2.9 Projectiles, explosions, line of sight

- **Projectiles** (`projectiles.js:150-287`):
  - segment test against object AABBs (`!noShoot && active`, skipped when `noObjColl`), players (lag-compensated), AIs (team-filtered) and **terrain**;
  - the earliest hit wins; there is no bounce or stick physics;
  - NEW noPVP/forcePVP filters;
  - destructible damage only when the owner is a player.
- **Explosions** (`game.js:876-943`):
  - NEW `mode.explSMlt` (radius) and `explMlt` (damage);
  - `data.aiOnly`; `stk` streak self-immunity; AI damage + `pointsPerHit`;
  - v3: `game.js:685-748`.
- **canSee** (`game.js:2711-2765`) gains a ramp-aware slab test (`includeRamp`), skips `complexColl`, adds `returnNearest`, and raycasts terrain. v3: `:1955-2022`.

### 2.10 Effects (`data/effects.js`, NEW)

| # | name | key | max | mlt | timer |
|---|---|---|---|---|---|
| 0 | Boosted | speed | 5 | 0.1 | — |
| 1 | Sleight of Hand | reload | 5 | 0.1 | — |
| 2 | Rapid Fire | fRate | 5 | 0.05 | — |
| 3 | Siphon | siphon | 5 | 1 | — |
| 4 | Rooted | rooted | 2 | 0.25 | 25 s |
| 5 | Dizzy | drunk | 4 | 0.25 | 35 s |
| 6 | Poisoned | enraged | 1 | 1 | 40 s |
| 7 | Jump Boost | jump | 5 | 0.1 | — |
| 8 | Zapped | zap | 10 | 0.5 | 5 s |
| 9 | Strength | dmg | 10 | 0.1 | — |

- Applied by trigger actions 64/66, AI `se`/`he`, streaks, modes (shrp, aon) and airdrop traps.
- Ticked every 500 ms (`players.js:4181-4199`, `AI.js:423-444`).
- `giveEffect`/`removeEffect`/`clearEffect` (`players.js:4628-4692`); packet `pk idx,stacks`.
- Map overrides come through `config.prks` (§4.3).

### 2.11 Movement schemes (`data/movement.js`, NEW)

- `game.movement[player.config.movSch]`: Default, Follow Mouse, Move Right, Fly, Dir Only, SRM, Vehicle, MMORPG.
- Extra state (`sendVals`) is appended to the `"l"` sync (`game.js:5047-5055`):
  - SRM: `onGroundFrames`, `onWallFrames`, `wishDash`, `dashTimer`, `walldashTimer`, `tmpVel`;
  - Vehicle: `forwardV`, `reverseV`, `angVel` (`movement.js:472-628`).
- Selected per class through `cls[i].movSch` (§4.3). Movement and physics are owned by the player-physics work package (WP-F in §7); this document only records that maps can select a scheme.

### 2.12 KrunkScript runtime hooks (pointer only)

- `mainScript.start()` runs at `finishInit`.
- `update(dt)` runs every tick; the client calls `render`.
- Scripts come from map `scripts.server`/`scripts.client` (`game.js:163-168, 606-622, 3784-3786, 4952-4954`).
- Logs go out as `ksL`; network messages use `sd` both ways.
- Owned by the KrunkScript spec. The trigger interface is in §3.10.

## 3. Triggers and map logic

Path roots: **v7** = `/home/user/jails/kru/src/Krunker_v7.1.4/src`, **v3** = `/home/user/jails/kru/src/Krunker_v3.1.1/src`, **KN** = `/home/user/src/KrunkNative`.

Main v7 sources:
- trigger tables: `v7 data/triggers.js` (events :7-103, conditions :104-320, actions :321-1610);
- runtime: `v7 data/game.js:4000-4210` (`onTrigger`, `playerTrigger`, `objectTrigger`, `multiObjectTrigger`, `getActionValue`);
- parse and sanitize: `v7 data/map.js:537-627` and `v7 libs/MapManager.js:1845-2100` (`addTrigger`);
- runtime object fields: `v7 libs/GameObject.js:211-242`;
- editor: `v7 libs/ObjectInstance.js:1181-1284` (serialize) and `v7 ui/editor.js:4137-4492` (trigger panel).

The v3 baseline is `v3 data/triggers.js` (16 events, 32 actions, no conditions), `v3 data/objects.js:1305` (`addTrigger`) and `v3 data/game.js` (`onTrigger`).

**Status legend:** **same** means the same index and behaviour as v3.1.1. **changed** means the same index with different behaviour. **NEW** means it does not exist in v3.1.1.

**Index compatibility:**
- **No index was reordered.** Events 0-15 and actions 0-31 keep their v3 indices. Events 16-31 and actions 32-71 are new.
- Two events were renamed with no change in behaviour: event 3 `onEnter`→`onEnterObject` and event 10 `onInteract`→`onHoldMouse`.

### 3.1 Objects that carry triggers

| Carrier | v7 source | Notes |
|---|---|---|
| TRIGGER (prefab 29) | `map.js:537-627` → `MapManager.addTrigger:1845-2100` | The main carrier. It can render as a cube or as a custom asset (`aid`) with animations. `pl` makes it non-solid for players. `hbh`/`hbw` scale the hitbox 0.1..10 |
| AI_SPAWNER (prefab 55, was BOT) | `map.js:1219-1238` → `MapManager.buildAIData:2985-3033`, `addAI:3036-3270`, action sanitize `:3150-3255` | Fires only `onAI*` events, and only when `ct` (Can Trigger) is set. Missing `te` → 15 (onAIDeath) (`MapManager.js:3008`). Event names without `"onAI"` are dropped (`:3133-3136`). Carries its own `tas` list |
| Any `canInterface` prefab | `prefabs.js` flag `canInterface`; `MapManager.js:1833-1843` | Gets `in` (interface id) and is a target of interface actions. v7 adds AI_SPAWNER, SERVER_NODE, PERK_MACHINE, every *_ZONE, WEAPONBOX, CHRISTMASTREE, GIFT and more to the v3 list of 32 |
| TERMINAL (prefab 41) | `map.js:646-647` (empty branch) | **Removed in v7.** v7 parses nothing for it. v3 built `addTerminal` and used the `ot`/`tmc` packets (`v3 data/players.js:4581-4588`). KN must stop building terminals and retire `client/src/ui/ui_terminal.c` together with its packets |

Limits:

| Limit | v3 | v7 | Source |
|---|---|---|---|
| `interfaceIDMax` (clamp for `in`/`tin`) | 1000 | 2000 | `v7 config.js:934`, `v3 config.js:537` |
| `interfaceLimit` (objects per interface id, enforced at publish) | 20 | 30 | `config.js:933`, `customGameConfig.js:342` |
| `triggerLimit` (trigger chain depth) | 20 | 30 | `config.js:931`, `game.js:4025` |
| `triggerActionLimit` (actions saved by the editor; the runtime has no cap) | — | 5 | `config.js:932`, `ObjectInstance.js:1281` |
| `custValLimit` (custom value keys per player) | 20 (declared, unused by v3 triggers) | 30 | `config.js:1625`, `v3 config.js:2406` |
| `playerVals` | health, maxHealth, score | + classIndex | `config.js:1624` |
| emitter rate min / vol min | 0.5 / 0.05 | 0.1 / 0.05 | `config.js:2313-2315` |
| `teamLimit` | 5 (but v3 action 19 hard-clamped to 1..2) | 5 (action 19 clamps to 1..`teamLimit`) | `config.js:351`, `v3 config.js:2391` |

### 3.2 Object-level JSON keys

| key | runtime field | meaning | v3 | v7 |
|---|---|---|---|---|
| `te` | `trigEvent` index | event index into `triggers.events` | 0..15 | 0..31. Out-of-range → eventN `null`, and the trigger never fires (`map.js:586`). Trigger objects never accept `onAI*` events (`MapManager.js:1919`) |
| `tce` | `trigConstE` | "Event Value", clamped 0..10000. It is the period in seconds for onTimer, onHoldMouse and the onCollision cooldown (`(constE‖1)*1000`, `GameObject.js:231-236`), and the threshold for onStorage* and onCustomVal* | same | same, plus the legacy-wrap copy (§3.4) |
| `cha` | `chance` | 0..100; 0 means always | same | same |
| `su` | `singleUse` | fires once per round | same | same |
| `sup` | `singleUseP` + `uses[]` | fires once per player id per round | — | NEW (`map.js:600`, `game.js:4016-4021`) |
| `sfd` | `selfDestruct` | runs action 24 after the actions | needs `su && canDestroy` | needs `canDest` only (`MapManager.js:2059`) |
| `tac` | `actionCost` | cost 0..10000. Zeroed unless the event is onInteractKey (16) or `tam` is set (`MapManager.js:1922-1924`). The editor saves it only when `te==16` (`ObjectInstance.js:1181-1186`) | — | NEW |
| `tam` | `actionMethod` | 0 Cost KR, 1 Cost Score, 2 Require Score, 3 Require Kills, 4 Requires Account, 5 Verified, 6 Premium, 7 Partner, 8 Requires Level (`ui/editor.js:4137-4172`) | — | NEW |
| `inm` | `interactMsg` | prompt text "Press [E] to `<inm>`". ≤70 chars, same charset as `tct` (`MapManager.js:1928-1937`) | — | NEW |
| `tas` | `actions[]` | action list (§3.3). Missing or empty → the legacy keys are wrapped (§3.4) | — | NEW |
| `in` / `tin` | `interface` / `interfaceT` | source id / target interface id, 0..`interfaceIDMax` | same | same (new clamp) |
| `h`/`hp`, `cdy`, `sd`, `rt`, `rr` | health, canDest, startDestroyed, respawnT (s), randomRespawn | destructible state | same | same |
| `rsc` | spawn chance | % chance that a start-destroyed object spawns at round reset (`MapManager.resetAll:3776`) | — | NEW |
| `ns`, `nai`, `pl`, `gp`, `ip`, `wj` | noShoot, noAICol, noPCol, noGrapple, noImpulse, no wall-jump | collision opt-outs | `wj` only | NEW. `ns` is forced on when `te` is onEnterObject or onExitObject (`MapManager.js:2075`; v3 did the same for onEnter, `v3 objects.js:1361`) |
| `hbh`, `hbw` | hitbox height and width multipliers | 0.1..10 (`MapManager.js:2044-2048`) | — | NEW |
| `aid`, `as`, `ast`, `fcr`, `cst`, `rcv` | asset model, anim speed, start anim, force render, no cast / receive shadow | used by Play Animation (39/63) | — | NEW |
| `sci` | canScript | marks the object for KrunkScript access (prefab flag `canScript`) | — | NEW |

Two v7 changes to object behaviour:
- onInteractKey triggers get `tRadius = 7` (`MapManager.js:1925`).
- Objects with `in` and collision go into `manager.interfaces`; objects with onTimer go into `timerObjs`; objects whose event name contains "onPlayer" go into `playerObjs` (`MapManager.js:1833-1843`).

### 3.3 Per-action JSON keys (`tas[i]`, v7 only)

Sanitized in `MapManager.js:1938-2041` for triggers and `:3150-3255` for AIs.

| key | runtime | meaning / sanitize |
|---|---|---|
| `ta` | `trigAct` | action index (`parseInt‖0`) |
| `tc` | `trigConst` | integer (`parseInt‖0`). The editor slider is −10000..10000, or a select index |
| `tc2` | `trigConst2` | second integer, for actions with `hasExtra` |
| `tct` | `trigConstTx` | text, ≤70 chars, must match `[\w:;()?,.\|[\] \-+*$@=^!{}]` (otherwise dropped). For ta 37/38 it is reduced to `[a-z0-9_]{0,10}` |
| `tcc` | `trigConstC` | value key, reduced to `[a-z0-9_]{0,10}` |
| `tce` | `trigConstT` | **value type** (`config.inputTypes`, `config.js:947`): 0 Input Value, 1 Trigger Value (the trigger's own `custVals[tcc]`), 2 Player Value (`playerVals` or `player.custVals[tcc]`), 3 Global Value (`game.globalVals[tcc]`). `parseInt‖0`, **not clamped** |
| `tad` | `trigDelay` | delay in seconds, clamped 0..60, rounded to 0.1 |
| `cd` | `trigCond[0]` | condition index; an unknown index becomes 0 |
| `cv0`..`cv2` | `trigCond[1..3]` | condition params, sanitized per the condition's `vals`. Missing → `def`; `bool` → `!!v`; `input` → `[a-z0-9_]` cut to `len`; `select` → `parseInt‖def` clamped to the option range; number → `parseFloat‖def` clamped min..max; anything else → `def` |
| `es` / `esa`, `ev`, `er`, `ef` | `triggerSound=[name,vol,rate,emitFrom,isAsset]` | sound for ta 14/34/54. `es` indexes `config.emitterSounds` (an invalid index → index 0); `esa` is a user asset id. vol 0.05..2, rate 0.1..4, `ef` 0 = at the trigger (3D), 1 = at the player (3D), 2 = global (2D) (`ui/editor.js:4477-4492`). Runtime default `["nuke_0",1,1,0,false]` (`triggers.js:6`) |

The emitter sound table grows from 52 to 73 entries (`v7 config.js:2321`, `v3 config.js:1420`). **Index 16 changed from `jumpS_0` (v3) to `scare_0` (v7).** Adopt the v7 table as is: v3 maps that use `es:16` play `scare_0`, exactly as in v7.

### 3.4 Legacy (v3) trigger wrap: the compat spec

`v7 map.js:538-557`: if `tas` is missing, not an array, or empty, v7 builds
`tas = [{tc, tc2, tct, tcc, tce, ta, esa, es, ev, er, ef, tad, cd, cv0, cv1, cv2}]` from the object-level keys.

Consequences that KN must reproduce (v7 behaviour, adopted as is):
1. **Object-level `tce` (Event Value) is also copied into the action's `trigConstT`.** For a v3 trigger that has `tce≠0` and a `hasCust` action (0, 3, 6, 8, 13, 25, 26, 27, 35, 40, 41, 48, 49, 69):
   - `getActionValue` sees `trigConstT≠0` with no `trigConstC` and falls through, returning `undefined` (`game.js:4163-4218`);
   - the action is skipped.

   This is v7 behaviour and is kept. **None of the shipped maps is affected** (checked: no TRIGGER in `assets/maps/*.json` or `assets/maps/custom/*.json` combines a missing `tas`, `tce≠0` and a `hasCust` `ta`).
2. A v3 onTimer with `tce=0` used to fire once (v3 reload was `(tce‖0)*1000`, `v3 game.js:3179`). In v7 it fires every 1 s (`(trigConstE‖1)*1000`).
3. The v3 fields `triggerConstant`, `triggerConstantTxt`, `triggerConstantEvent` and `triggerTimer` (`v3 objects.js:78-86`) become the per-action fields `trigConst` and `trigConstTx` and the per-object fields `trigConstE` and `trigT` (`GameObject.js:211-242`).

### 3.5 Events (`v7 triggers.js:7-103`; v3 `triggers.js:3-20`)

"player" is the player argument passed to `onTrigger`.

| # | v7 name | status | fired where (v7) | player arg |
|---|---|---|---|---|
| 0 | onShoot | same | hitscan hit `players.js:3126`; projectile hit `projectiles.js:285` | shooter |
| 1 | onMelee | same | melee hit `players.js:2442` | attacker |
| 2 | onDamage | same | shoot, melee and projectile hits. Also chained from actions 4, 12 and 24 when the target has onDestroy‖onDamage, and from action 6 when `trigConst<0` | damage dealer |
| 3 | onEnterObject | **changed** (renamed from onEnter) | `player.js:1400-1408`. Fires once on entry, using the per-player `collidingObjs` set; v3 tracked a single `inObject` sid (`v3 players.js:938-942`). Collisions are only saved when the map has enter or exit triggers (`game.js:3507`) | player |
| 4 | onDestroy | same | health reaches 0 from a hit (`projectiles.js:259`, players.js shoot/melee), or actions 4/6/12/24 | destroyer |
| 5 | onRespawn | **changed** | `updateDestructables` `game.js:3334`, actions 11/12/21. v3 required `destroyedBy` (`v3 game.js:2287`); v7 fires with a null player | `destroyedBy` (may be null) |
| 6 | onTimer | **changed** | `game.js:4890-4898` over `timerObjs`. Server only, while not in a wait timer (or when `contTime` is set). Reload `(trigConstE‖1)*1000` | null |
| 7 | onProjectile | same | `projectiles.js:285` | projectile owner |
| 8 | onThrowingMelee | same | projectile with `weaponId==12` | owner |
| 9 | onPunch | same | melee while `!canThrow` | attacker |
| 10 | onHoldMouse | same (renamed from onInteract) | `player.js:1389-1399`. Hold primary fire while grounded inside the box; progress `+= dt/onInteractT`; interact type 4 | player |
| 11 | onCollision | same | `player.js:1381-1384`. Per-object cooldown `onColT=(constE‖1) s` | player |
| 12 | onStorageEquals | same | only from actions 25-28 on the target | passed through |
| 13 | onStorageGreater | same | same as 12 | passed through |
| 14 | onStorageLesser | same | same as 12 | passed through |
| 15 | onAIDeath | same | `AI.js:1108-1110` | killer |
| 16 | onInteractKey | NEW | interact key packet `int 7,uid` → `players.js:3456-3467`; prompt `player.js:1352-1365`; radius 7; team check; KR-cost confirmation via `cnfm` (`players.js:3500-3507`) | player |
| 17 | onPlayerSpawn | NEW | **No object dispatch in the v7.1.4 reconstruction.** Only `mainScript.onPlayerSpawn` runs (client `index.js:14700`). zombie_high.json uses te 17 six times. **Decision:** dispatch `playerTrigger("onPlayerSpawn", p)` from the server spawn, and document it in code as a reconstruction gap | player |
| 18 | onPlayerKill | NEW | `players.js:4881` `playerTrigger` | killer |
| 19 | onPlayerDie | NEW | `players.js:4879` | victim |
| 20 | onAIDamage | NEW (AI only) | `AI.js:1065` (damage from players only) | attacker |
| 21 | onNukeStart | NEW | `streaks.js:21` `findTriggers`, only if `game.isCustom` | nuker |
| 22 | onNukeEnd | NEW | `streaks.js:42`, custom games only | nuker |
| 23 | onExitObject | NEW | `player.js:1652-1664`: objects in `collidingObjs` that were not touched this tick → `objectTrigger` | player |
| 24 | onCustomValEquals | NEW | only from actions 43/44 on the target; compares the new value with the target's `trigConstE` | passed through |
| 25 | onCustomValLesser | NEW | same as 24 | passed through |
| 26 | onGameStart | NEW | `game.js:3696` `multiObjectTrigger`, after `mode.gameStart` | null |
| 27 | onObjectiveSwitch | NEW | `game.js:2898` | null |
| 28 | onPlayerDamage | NEW | `players.js:4296` (damage > 0 and the victim survives) | victim |
| 29 | onAIHalfHealth | NEW (AI only) | `AI.js:1068` (when health crosses maxHealth/2) | attacker |
| 30 | onCustomValGreater | NEW | same as 24 | passed through |
| 31 | onAIKillPlayer | NEW (AI only) | `AI.js:748` (AI melee kill) | victim |

Dispatchers (`v7 game.js`):
- `playerTrigger(name, player)` (:4137) iterates `playerObjs`.
- `objectTrigger(obj, name, player)` (:4145).
- `multiObjectTrigger(name, player)` (:4153) runs only if `triggerTypes` contains the event name. `triggerTypes` is built by `generateTriggerTypes` (:3807-3820) at init.
- `findTriggers` (:1457) scans every object.

### 3.6 Conditions (NEW in v7, `triggers.js:104-320`)

Conditions are evaluated per action. Signature `check(game, serverIO, player, obj, action)`; params come from `action.trigCond=[cd, cv0, cv1, cv2]`.

Compare types (`UTILS.compareVal`, `libs/utils.js:1354`): 0 `==`, 1 `!=`, 2 `>`, 3 `>=`, 4 `<`, 5 `<=`.

| # | name | params (name, default, range) | semantics |
|---|---|---|---|
| 0 | None | — | true |
| 1 | Player Value | Key ("", input len 10); Type (0, compare select); Val (0, 0..1000 step .01) | `player[key]` if key ∈ `playerVals`, else `player.custVals[key]‖0`; compared with Val. False without a player |
| 2 | Current Class | Class (0, select of class names) | `player.classIndex == cv0` |
| 3 | Trigger Value | Key, Type, Val (as #1) | `obj.custVals[key]‖0` (the trigger's own values) |
| 4 | Player Count | Type; Filter (0 Active / 1 Spectating / 2 Overall Connected); Amount (0, 0..50) | counts `active` players, `spectating` players, or `game.connectedClients` |
| 5 | Global Value | Key, Type, Val | `game.globalVals[key]‖0` |
| 6 | Player has Account | — | `!!player.account` |
| 7 | Player has Premium | — | `account.premiumT > 0` |
| 8 | Player is Verified | — | `account.featured` |
| 9 | Player is Partner | — | `account.partnerApp >= 2` |
| 10 | Player Level | Type; Level (0, 0..1000) | `account.level` compared |
| 11 | Player Inventory Value | Type; Val (0, 0..1e8 step 1000) | `account.invVal` compared |

Conditions 1-11 return false when there is no player.

Conditions 6-11 need account data. KN gets accounts only through kn_brain (the client has no DB access). In the offline sandbox, or when there is no account, they evaluate false, like v7 with a guest.

### 3.7 Actions (`v7 triggers.js:321-1610`; v3 `triggers.js:21-611`)

Signature: v7 `execute(game, serverIO, player h, triggerObj j, action a, chain)`; v3 `execute(game, io, h, j, chain, UTILS)`.

**Targets** are the map interface objects `d` where:
- `j.isAI || d.sid != j.sid`, and
- `d.interface != j.interface`, and
- `d.interface == j.interfaceT`.

They are iterated while `triggerChains[chain] > 0`.

**`val`** is `getActionValue(h, target‖j, a, "number", default)` (`game.js:4163-4218`):
- type 0 returns `default`, which is the constant;
- types 1, 2 and 3 read `j.custVals`, the player value, or `globalVals[tcc]`, and return `parseInt(v)‖undefined` (so a stored 0 also gives undefined);
- the action is **skipped** on undefined.

**Flags:** `hasCust` means the action supports value types. `hasExtra` means it uses `tc2`. `devOnly` means it is hidden in the public editor.

| # | name | params | v7 semantics | status |
|---|---|---|---|---|
| 0 | Give Player Score | tc (hasCust, def 1) | `players.score(h, val)` | same (value types added) |
| 1 | Kill Player | — | kill h unless godMode | same |
| 2 | Respawn Player | — | teleport h to the `h.checkPoint` object if set, else to `getSpawnPoint`; `changePosition(...,true,false,undefined,true)` | **changed** (checkpoint first) |
| 3 | Change Player Health | tc (hasCust, def 0) | `changeHealth(h, null, -val, true)`; kills at 0 | same |
| 4 | Destroy Interface | — | for targets with health‖gate‖(canDest&&!destroyed): gate → `active=false` + `gte`; otherwise `destroyed` + `do`; fires the target's onDestroy/onDamage | same |
| 5 | Toggle Interface Gate | — | toggles target gates and sends `gte` (no chain check) | same |
| 6 | Change Interface Health | tc (hasCust) | targets need health && canDest && !destroyed; `health -= -val`; at 0 → destroy + onDestroy; fires onDamage if `tc<0` (the raw constant) | **changed** (canDest/!destroyed now required) |
| 7 | Teleport Player To Interface | — | `changePosition(h, d.x, d.y-d.height, d.z, true, true, …, true)` for each target; the last one wins | same (renamed) |
| 8 | Change Interface Deposit Box Amount | tc (hasCust) | `bank.deposited += val`, `bnk` | same |
| 9 | Kill Opposing Team | — | kills active, non-godMode players with no team or a different team | same |
| 10 | Respawn Opposing Team | — | #2 for each opponent (checkpoint aware) | **changed** |
| 11 | Respawn Destructible Interface | — | revives destroyed targets, resets respawnT and health, sends `ro`, fires onRespawn | **changed** (no `destroyedBy` needed) |
| 12 | Toggle Destructible Interface | — | destroys or revives targets that have startHealth‖canDest | same (onRespawn change as #11) |
| 13 | Set Player Score | tc (hasCust, def 0) | `score(h, val, 0, true)` | same |
| 14 | Broadcast Sound | sound | `ef==2`: broadcasts `so` (2D). Otherwise `playSound` at the player (if `ef` and h; head height) or at the trigger | **changed** (global mode) |
| 15 | Clear Player Loadout | — | loadout becomes `[12]`; broadcasts `inat` | same |
| 16 | Broadcast Message | tct | broadcasts `ch -1,null,text,2` | packet shape changed (v3 `ch null,text,2`) |
| 17 | Send Message | tct | the same, to h only | packet shape changed |
| 18 | Swap Player Team | — | 1↔2, `updateTeam` | same |
| 19 | Set Player Team | tc | team clamped to 1..`teamLimit` (5) | **changed** (v3 1..2) |
| 20 | Round Time | tc | `gameTimer += tc` ms; ≤0 → "skip" | same |
| 21 | Reset Interface | — | for any target: revive if destroyed (`ro`, onRespawn); `trigT=(trigConstE‖1)*1000`; reset health; `active`; `triggered=false`; clear `uses` | **changed** (v3 only reset onTimer targets) |
| 22 | End Round | — | `gameTimer="skip"` | same |
| 23 | Refill Player Knife | — | `refillPlayer(h)` → `rfl` | same |
| 24 | Destroy Self | — | destroys or closes j; fires its own onDestroy/onDamage unless selfDestruct | same |
| 25 | Increase Interface Storage | tc (hasCust) | target `storage += val`, then fires the target's onStorage* against its `trigConstE` | same |
| 26 | Decrease Interface Storage | tc (hasCust) | `storage -= val`, same follow-up | same |
| 27 | Set Interface Storage | tc (hasCust) | `storage = val`, same follow-up | same |
| 28 | Reset Interface Storage | — | `storage=0`; only targets with an Equals or Greater event (a v3 quirk that is kept) | same |
| 29 | Broadcast Popup | tct | `am [text,null,0,1]` to all | shape changed (v3 `[text,null]`) |
| 30 | Send Popup | tct | the same, to h | shape changed |
| 31 | Jump Scare | — | `scr 0` to h | same |
| 32 | Reward KR | tc "KR Amount" | `rewardKR` via DB call 137; published custom maps only (`game.js:4341`) | NEW (backend) |
| 33 | Clear Player Checkpoints | — | clears checkPoint, List and Data; sends `chp true` | NEW |
| 34 | Send Sound | sound | to h only: `so` (2D) or `s` (3D at the player or the trigger) | NEW |
| 35 | Explosion | tc "Explosion Power" (hasCust, def 100) | `game.explosion(j.x,j.y,j.z,30,val,h,{})` | NEW |
| 36 | Spawn AI | — | for each pending AI spawner whose `data.interface==j.interfaceT` (and ≠ `j.interface`): `AI.spawn(...)`. Each call spawns a new AI | NEW |
| 37 | Set Player Value | tct Key (≤10, `[a-z0-9_]`), tc Value | `h.custVals[key] = parseFloat(tc)`; at most `custValLimit` 30 keys; sends `cv key,val` to h | NEW |
| 38 | Increase/Decrease Player Value | tct Key, tc | `h.custVals[key] += parseFloat(tc)`; sends `cv` | NEW |
| 39 | Play Animation | tct Clip, tc "Interface ID" (**the interface id, not `tin`**) | AIs with `interface==tc` → `AI.playAnim` (`ana sid,clip,1`); interface objects with `hasAnim` → `ana uid,clip,1,true` | NEW |
| 40 | Teleport Player to Path Node | tc Node ID (hasCust) | `changePosition` to `pathNodes[id]` | NEW |
| 41 | Teleport Destructible Interface to Path Node | tc (hasCust) | `teleportToNode(uid,node)` (`game.js:818`): moves the object, updates the quad grid, adds it to `movedObjs`, sends `mo` | NEW |
| 42 | Reset Destructible Interface Position | — | `teleportFromNode` → original position; sends `mf` | NEW |
| 43 | Set Interface Trigger Value | tct Key, tc | target `custVals[key] = tc`, then fires the target's onCustomVal* against its `trigConstE` | NEW |
| 44 | Increase/Decrease Interface Trigger Value | tct Key, tc | target `custVals[key] += tc`, same follow-up | NEW |
| 45 | Set Trigger Value | tct, tc | `j.custVals[key] = tc` (no events) | NEW |
| 46 | Increase/Decrease Trigger Value | tct, tc | `j.custVals[key] += tc` | NEW |
| 47 | Change Class | tc Class Index (select) | sets `classIndex` and calls `updateItems` | NEW |
| 48 | Set Interface Deposit Box Amount | tc (hasCust) | `deposited = val`, `bnk` | NEW |
| 49 | Save Data | tct Data Name, tc (hasCust, def 1), tc2 Save Type (0 Set / 1 If Higher / 2 If Lower / 3 Increase / 4 Decrease) | `storageUpdt` into DB private storage (name lowercased, ≤12). The name `"time"` stores the player's game time. Not available in the sandbox | NEW (backend) |
| 50 | Change Primary Weapon | tc Weapon (select) | replaces (or inserts) the primary slot of the loadout; broadcasts `inat` | NEW |
| 51 | Change Secondary Weapon | tc Weapon (select) | the same for the secondary slot | NEW |
| 52 | Set Global Value | tct Key, tc | `game.globalVals[key] = tc`; broadcasts `gv k,v`; reset on init (`game.js:3487`); late joiners get `gbl` in the sync data (`game.js:3869-3873`, client `index.js:13598`) | NEW |
| 53 | Change Global Value | tct Key, tc | `globalVals[key] += tc`; `gv` | NEW |
| 54 | Stop Sound | sound | broadcasts `sso name,isAsset` | NEW |
| 55 | Show/Hide GUI Element (All) | tc Element ID, tc2 Status (0 Toggle / 1 Show / 2 Hide) | broadcasts `tgui id,status` → the client toggles `nv` on the map `gui` element (`index.js:14385`) | NEW |
| 56 | Show/Hide GUI Element (Player) | same | `tgui` to h only | NEW |
| 57 | Load Data | — | DB call 163 → `h.custVals[mk]=kv`, then `cv` per key | NEW (backend) |
| 58 | Custom Action | tct "Custom Param", tc | `mainScript.onCustomTrigger(h?h.id:"NONE", tct, int(tc))` (§3.10) | NEW (KrunkScript) |
| 59 | Reward Junk | tct (devOnly) | public games only | NEW (backend; KN no-op) |
| 60 | Reward Skin | tct "id\|chance,…" (devOnly) | `giveSkin` | NEW (backend; KN no-op) |
| 61 | Change Ambient Sound | tc (select of `config.soundScapes` keys, `config.js:2404-2419`) | broadcasts `camb id` | NEW |
| 62 | Give Challenge Progress (player) | tct key, tc (devOnly) | `challenges.update` | NEW (backend; KN no-op) |
| 63 | Play Animation (Self) | tct Clip | `playAnim(j, clip)` | NEW |
| 64 | Give Effect | tc (select from effects.js) | `players.giveEffect(h, idx)` | NEW |
| 65 | Give Challenge Progress (all) | tct, tc (devOnly) | `challenges.update` for every player | NEW (backend; KN no-op) |
| 66 | Give Random Effect | — | `giveEffect(h,0,1,true)` | NEW |
| 67 | Play Video Ad | — | `vad` to h (not in the sandbox) | NEW (KN no-op) |
| 68 | Drop Powerup | devOnly | zom mode only: `dropSpawnable(randInt(9,15))` at the trigger | NEW |
| 69 | Spawn Bot at Node | tc Node (devOnly) | hard-coded "Uncle Joe" bot via `bots.add` | NEW |
| 70 | Force AI Target | — | `AI.forceTarget` = the trigger position for 30 s | NEW |
| 71 | Mode Specific | — | `game.mode.onTrigger(game, j)`. Only zom implements it (pack-a-punch portal, `modes/zom.js:508`) | NEW |

**Select-index quirk:** for ta 47, 50, 51, 61 and 64 the select value is an index into the editor's option array, but `execute` indexes `weaponConfig`, `classes` or `effects` directly. KN must index the runtime table directly, as v7 does, because the map JSON carries the raw value.

### 3.8 `game.onTrigger` runtime (`v7 game.js:4000-4136`)

**Order of checks:**
1. Require `game.isServer && (obj.isAI || teamCheck(obj, player))`. `teamCheck` (`game.js:5161`) passes when the object has no team, the mode has no teams, or the teams match.
2. Return if there are no actions.
3. Return if `triggerChains[chain] <= 0`.
4. Return if `singleUse && triggered`.
5. Chance roll: continue when `randInt(0,100) <= cha`.
6. `singleUseP`: return if the player id is already in `uses`, otherwise push it. This happens before the cost checks.
7. Set `triggered = true`.
8. If `chain == null`: `chain = obj.uid` and `triggerChains[chain] = triggerLimit` (30). Then `triggerChains[chain]--`, once per `onTrigger` call (not per action).

**Cost paths:**
- **No cost and `tam ≤ 3`:** for each action in order, the condition is checked **now**. If it passes:
  - with `tad>0` the action goes to `triggerQueue` as `{action, player, obj, chain, timer}`;
  - otherwise it executes immediately.

  After the loop, `selfDestruct` runs action 24 once.
- **`tam == 0` with a cost (KR):** server only. Requires a dbClient, an account not flagged as hacker, a custom map with an id, and `canUseKRTrigger`. DB call 138 deducts the KR, then the server sends `uf` and `am ["Spent","-N KR",0,1]`. The actions then run **without checking conditions** (v7 quirk, kept).

  KN: with no kn_brain KR wallet, KR-cost triggers do nothing (treated as unaffordable).
- **Other methods:** the requirement check runs first. If `cost≠0` the server sends `am ["Unlocked"|"Purchased", …]`; methods below 2 deduct score. Then the normal path with conditions.

**Delayed actions:**
- The queue is ticked at `game.js:4900-4907`, on the server only, while not waiting.
- The condition is **not** re-checked when the action executes.
- The queue is cleared at init (`game.js:3485`).

**State resets:**
- Per round (`MapManager.resetAll` :3760-3800):
  - `storage=0`, `triggered=false`, `trigT` reset, `uses` cleared, object `custVals={}`;
  - destroyed and active state restored from `sd` and `stc`;
  - the `rsc` spawn chance re-rolled.
- `globalVals={}` at init.
- **Player `custVals` are never reset** (`player.js:15`; `reset()` at `player.js:825-833` only clears checkpoints).

**Checkpoints and teleporters:**
- `setCheckPoint` (`game.js:2997`) and `checkTeleport` (`game.js:3011`).
- A teleporter with `tcp` sends the player to their last checkpoint.
- Round reset sends `chp true` (`players.js:3992`).

**Interact flow (onInteractKey, gates, banks, wheels, machines):**
- The client computes `lastInteractData` from `changeInteract` (`player.js:1352-1365`, radius `tRadius`).
- Interact key → `int type,id`; secondary interact → `intSec`. The server rate limit is `interactTimer = 100 ms`.
- KR-cost onInteractKey: the server sends `cnfm uid`; the client shows "Press [confirm] to Confirm"; the client answers `cnfm`; the server calls `confirmInteract` (`server/index.ts:3319`).
- Prompt text (`players.js:3866-3872`): "Press [E] to `<inm‖Interact>`" plus `[N KR]`, `[N Kills]`, `[Account Only]`, etc., coloured by affordability.
- Interact type codes: 0 gate, 1 bank, 4 hold, 7 onInteractKey, 9 prize wheel, 13 mystery box, 14 perk machine, 15 krum machine.

**Custom GUI (map `gui`):**
- The map's top-level `gui` array holds elements `{id, t (0 rect / 1 text / 2 image), vt (0 literal / 1 player value / 2 global value), v, x, y, w, h, c, o, f, nv (hidden)}`.
- They are rendered by `libs/overlayHelpers.js:20-58` and `libs/game-overlay.js:1700-1725`.
- This is how `custVals` and `globalVals` reach the HUD. `tgui` toggles `nv`.

### 3.9 Server vs client (triggers)

**Authority:**
- Every `onTrigger` effect runs on the server (`game.isServer`); the v7 sandbox Game counts as a server.
- Clients run the same collision code (`player.js`) for prediction, but `onTrigger` is a no-op on the client.
- Prompts and hold-progress UI are client-side.

**Server → client packets used by triggers:**

| packet | payload | purpose | v3 |
|---|---|---|---|
| `do` | uid | object destroyed | same |
| `ro` | uid | object respawned | same |
| `gte` | uid, active | gate state | same |
| `bnk` | uid, amt | deposit amount | same |
| `ch` | -1, null, text, 2 | chat message from a trigger | changed (v3 `null,text,2`) |
| `am` | [text, sub, 0, 1] | popup | changed (v3 `[text,null]`) |
| `scr` | 0 | jump scare | same |
| `inat` | sid, loadout, idx, _, _, false | loadout change | same packet, new use |
| `rfl` | — | refill | same |
| `so` | name, vol, 1, 0, 0, 0, rate, null, null, isAsset | 2D sound | same packet |
| `s` | … | 3D sound | same |
| `sso` | name, isAsset | stop sound | NEW |
| `cv` | key, val | player custom value | NEW |
| `gv` | key, val | global value | NEW |
| `gbl` (in sync data) | {k:v} | global values for late joiners | NEW |
| `tgui` | id, status | GUI element toggle | NEW |
| `camb` | id | ambient soundscape | NEW |
| `chp` | [clear] | checkpoint set / clear | changed |
| `mo` | uid, node | object moved to a path node | NEW |
| `mf` | uid | object moved back | NEW |
| `mov` (in sync data) | [[uid,node]…] | moved objects for late joiners | NEW |
| `ana` | uid‖sid, clip, raw, isObj | play animation | NEW |
| `cnfm` | uid[,2] | KR purchase confirmation | NEW |
| `vad` | — | video ad | NEW (KN no-op) |
| `uf` | funds | KR balance | backend |

The client handler table is at `v7 index.js:18100-18260`.

Late-join sync (`getSyncData`, `game.js:3842+`) carries `dest`, `mov`, `gates`, `banks` and `gbl`. Player `custVals` are **not** re-synced (v7 behaviour).

**Client → server packets:** `int type,id` (KN `NET_PACKET_INTERACT` 61 currently carries only the v3 `int 1`), `intSec`, `cnfm [2]`, `vad 1|2|3|-1`.

### 3.10 KrunkScript interface (interface only; the KrunkScript spec is a separate document)

**What v7 provides:**
- Server script hook `public action onCustomTrigger(str playerID, str customParam, num value)` (`krunkScriptSyntax.js:4`, line 36 of the server template). Action 58 calls it on `game.mainScript` (`triggers.js:1376-1391`).
- Hooks that fire at the same points as trigger events:
  - `onPlayerDamage(id, doerID, amount)` (`players.js:4297`);
  - `onPlayerDeath(id, killerID)` (`players.js:4889`). Quirk: it is guarded by the existence of `mainScript.onPlayerSpawn`;
  - `onPlayerSpawn(id)` (client `index.js:14700`).
- `GAME.TRIGGERS.list()` and `GAME.TRIGGERS.execute(index, ...args)` are declared (`krunkScriptSyntax.js:427-431`), but they are **empty stubs** in v7.1.4 (`libs/createKrunkScriptApiContext.js:3186-3189`).
- A `trigger` wrapper type exposes read-only position and scale (`createKrunkScriptApiContext.js:1057`). The `sci` (canScript) object flag exposes objects to scripts.
- There is no KrunkScript API for `custVals` or `globalVals`. The bridge between them is the map `gui` plus the `cv` and `gv` packets.
- Fixture: the "Triggers & Scripts" template (`data/scriptTemplates.js:58`), a 2-trigger map `{"i":29,"te":1,"tas":[{"ta":58,"tct":"2"}]}`.

**Proposed C interface** (shared; default NULL means no-op; the KrunkScript work package fills it):

```c
/* shared/include/triggers.h */
typedef struct TriggerScriptHooks {
    void (*on_custom_trigger)(Game *game, const char *player_id, const char *param, int value); /* action 58 */
    void (*on_player_spawn)(Game *game, int player_sid);
    void (*on_player_damage)(Game *game, int victim_sid, int doer_sid, int amount);
    void (*on_player_death)(Game *game, int victim_sid, int killer_sid);
} TriggerScriptHooks;
void triggers_set_script_hooks(Game *game, const TriggerScriptHooks *hooks);

/* value access for scripts and GUI */
double trigger_global_value_get(const Game *game, const char *key);
void   trigger_global_value_set(Game *game, const char *key, double value);   /* sends gv */
double player_custom_value_get(const Player *player, const char *key);
void   player_custom_value_set(Game *game, Player *player, const char *key, double value); /* sends cv */
```

## 4. Game modes, templates, custom game config, airdrops

**Where v7 mode code lives.**
- `data/gameModes.js` (one line) is the 37-entry engine table. It is used by `game.js:7` and `map.js:8` and requires `gameModes/*.js`.
- `data/modes.js` requires `modes/*.js`, the client copy. The two trees are identical apart from local variable names. `gameModes/parkour.js` is `modes/bhop.js`.
- Line references below are to `modes/<id>.js`.

**Where v3 mode code lives.**
- `data/modes.js` (24 entries) is the engine table, and KN mirrors it.
- `data/gameModes.js` (25 entries, adds 24 blitz) is UI only.

**Hook authority (v7).** These hooks run **on the server only**:
- `mode.update` (`game.js:4933`)
- `gameStart` (`:3699`)
- `onPreEnd` (`:4549`)
- `onAIDeath` (`AI.js:1111`)
- `onTrigger` (`triggers.js:1606`)

`collisionCheck` runs on both sides (`player.js:1693`); any scoring it causes is server-side. `custUI` is client-only.

### 4.1 Mode table (v7 index = wire / `config.modes` index)

**Index changes (breaking):**
- v3 indices 13 and 14 were both `ranked`. In v7 they are **13 clas** and **14 depo**.
- 0-12 and 15-23 keep their index. 24 blitz was only in v3's UI table. 25-36 are new.
- **Decision:** adopt the v7 table. Ranked modes are dropped. v7's ranked is a comp lobby playing Hardpoint (decisions.md "Ranked"); ids 37 / 38 stay reserved for ranked with no table entry.

**Rotation:** `rotationModes` changes from v3 `[0,1,2,3,19]` to v7 `[0,2,3,19,25]` (`map.js:28`).

**Mode keys removed in v7:** `aK3` (became `maxHealth`), `infEndTimer`, `isRanked`, `killSort`, `leaderStat` (became `leaderStats[]`), `needAll`, `rewardMlt`.

| # | id | name | status | teams | win / limits | timers / lives | required map objects | restrictions / mechanics (refs `modes/<id>.js` unless noted) |
|---|---|---|---|---|---|---|---|---|
| 0 | ffa | Free for All | same (+leaderStats) | – | top score | config | spawns | – |
| 1 | tdm | Team Deathmatch | same | 2 | team score | config | spawns (team) | – |
| 2 | point | Hardpoint | **changed** | 2 | team score; **comp:** ends early via `notWinnable` (`point.js:46-59`, `chi`) | objective switches every `objtvTime` min (v3 60 s) | OBJECTIVE (`obi`) | `update` (`point.js:34-62`): every 1.5 s, players on the active objective get `players.score(p, objScore‖10, 2)`. Pub `onKill`: 0.015% Krunkanium drop (spawnable 18) |
| 3 | ctf | Capture the Flag | changed | 2 | caps (sort by caps, then score) | config | FLAG `tm` 0/1 | **NEW:** a dropped flag returns after `flagIdle` 45 s (`game.js:1864-1869, 1923-1944`); neutral flags are ignored (`game.js:1831`) |
| 4 | bhop | Parkour | changed | friendly | none | `sendR` 5; `ownTime` when gameTime=0 (`game.js:3527`) | SCORE_ZONE, CHECK_POINT | NEW `logTime`, `allowReset`, `showHour`, `showMS`; speed-run `gt`; `lstChkT` rewind; `viewDis` 1000; custUI panel |
| 5 | hide | Hide & Seek | +`viewDis` 2000 | Hiders/Seekers | team 1 active > 0 → 1 | pickSeeker 10 s, release 45 s | – | spdMlt {1:0.8}, noWeap, killConv → team 2, hideNames |
| 6 | infect | Infected | +`modelOverrides` | 2 | team 1 active | infectRandom 20 s (1, 2 at ≥8 players, 3 at ≥14) | – | teamClass {2:9}, spdMlt {2:1.15}, Infected look `{headIndex:122, skinColor:5857617}` (`players.js:699-701`) |
| 7 | race | Race | changed | friendly | everyone reaches `targetScore` (Σ SCORE_ZONE `sp`); endSort by gameTimer | lockMove 15 s; 10 s after the first finisher | SCORE_ZONE `sp` | +logTime, viewDis 1000, custUI, noWeap |
| 8 | lms | Last Man Standing | same | 2 | lives | 300 s | – | lives 1, noRegen |
| 9 | simon | Simon Says | +viewDis 1000 | 2 | team 1 active | pickSimon 10 s | – | teamSee |
| 10 | gun | Gun Game | **changed** | – | top `weaponTier`; ends at `weaponOrder.length` | minPlayers 2 | – | `weaponOrder` **`[2,27,16,10,4,11,21,5,3,9,1,14,6,0,18,7,8,13,22,12]`** (v3 `[2,16,10,4,11,5,3,9,1,14,6,0,7,8,13,15,12]`; adds 27, 21, 18, 22 and drops 15). Rewards and streaks on except `disabledStreaks [4,5,6]`; `party`; `randClassScr` 0.5. Streak kills (`stk`) do not promote (`gun.js:50-69`) |
| 11 | prop | Prop Hunt | +`prop`, `propSwitches` 3, viewDis 2000 | 2 | team 1 active | lockSeekers 10 s, release 30 s, taunts every 20 s (`game.js:2930`) | – | – |
| 12 | boss | Boss Hunt | same | 2 | team 2 active → 2 | pickBoss 15 s | – | noReward |
| **13** | **clas** | Classic FFA | **NEW at this index** | – | score | config | – | noSliding, noKnives, noStreaks, noWallJ, impulseMlt 0, noPickups, noMelee, realMov; party (`clas.js:1-17`) |
| **14** | **depo** | Deposit | **NEW at this index** | 2 | first to `tagLimit` 65 team deposits. Ties: denies → kills → score (`depo.js:36-83`; the bug where team 2's "score" sums kills is kept) | `randObjective` every `objtvTime` | OBJECTIVE | `onKill` drops spawnable 3 "KR Tag" holding `victim.dogTags`+1, in clusters of ≤`maxPerCluster` 3 with ±10 jitter (`depo.js:89-131`). `update` every 500 ms: a carrier on the active objective deposits 1 → deposits++, team +1, `objScore` 50, `krt` (`depo.js:132-151`). Carry cap `tagHoldLimit` 20; enemy pickup +10, teammate deny +10 (`medals.js:150-200`) |
| 15 | stalk | Stalker | +`modelOverrides` (all black, noCosmetics) | 2 | team 2 active | pickStalker 15 s | – | onMelee −40 hp + jumpscare |
| 16 | king | King of the Hill | **rewritten** | – | score | **random** objective each period | OBJECTIVE | Every 1.5 s, +`objScore` **10** only when **exactly one** player is on the hill (v3: everyone on it, 20). `uobj uid,count`. `collisionCheck` sets `player.onZone`. Kill while on the hill: +50 "Defender" and `player.killMlt += hillMlt 0.05`; kill of someone on the hill: +25 "Attacker". Kill score × (`killMlt` 0.15 + player.killMlt) (`king.js:1-67`, `players.js:4091`). **v3 `noScoreK` removed** |
| 17 | oinc | One in the Chamber | key renamed | – | last alive | 10 s, then 100 ms | – | forceClass 10, `maxHealth` 10, ammoLimit 1, noReloads, forceSpawn 3, lives 3; rewards on; party |
| 18 | trade | Trade | changed flags | friendly | – | gameTime 0 | – | `noClientKick` (was noKick), noHPbars, noMinimap, nameDist 60, viewDis 500, maxNoMov 180000 (KN: the server's idle kick in the mode; canTrade: the live trade, doc/v10/trade.md) |
| 19 | kc | Kill Confirmed | +leaderSort | 2 | tagLimit 65 | config | – | Tag model `tag_0` (v3 `krunkie_0`), padMlt 0.8; confirm +25 and team +1, deny +10 (`medals.js:16-53`) |
| 20 | def | Defuse | same | 2 | bomb / defuse / survivors | comp start 10 s; 1 life | BOMB_SITE, bomb weapon 17 | realMov; the mode's `hitBoxPad` key is unused (the server config `hitBoxPad` applies) |
| 21 | shrp | Sharp Shooter | **changed** | – | score | weapon rotates every 20 s from `weapons.onlyGoodWeapons` (v3 `onlyWeapons`, `game.js:2918`) | – | NEW `onKill`: killstreak %2 → effect 1, %3 → effect 2, %5 → effect 0 (`shrp.js:16-28`). Streaks on except [4,5,6]; forceClass 9; party |
| 22 | trai | Traitor | same | 2 | lives | pickTraitors 15 s; 1 life | – | ghost AI on death |
| 23 | raid | Raid | **changed** | – | NEW winCondition: anyone alive with time left → 1, else 2 (`raid.js:19`) | 20 s start, 360 s; 1 life | AI / boss | **`preventCustomGames` removed** (hosted via hostType 4); `onEnd` records times; botsAsAI, botBypassClass, noMinimap, viewDis 500, skipStats |
| 24 | blitz | Blitz | NEW to the engine | 2 | caps | – | FLAG | `rush`: you can capture while your own flag is away; the capturer is sent back to spawn; no `ac` feed (`game.js:1816-1822`, `player.js:1489`) |
| 25 | dom | Domination | NEW | 2 | team score | – | **DOMINATION_ZONE** (uid = zone index) | §4.1a |
| 26 | sdm | Squad Deathmatch | NEW | **4** (`multiTeam`) | team score | config | spawns with team 1..4 | Joiners go to the smallest team (`players.js:127-145`). Lives condition variant (`game.js:3615`). Uses `nameTeam3..5` and `t3..5Dmg` |
| 27 | krank | Kranked FFA | NEW | – | score | forceSpawn 5 | – | A kill sets `krankedTimer` 15 s and `isKranked`: speed ×1.25, reload ×0.35, fire rate ×0.85. When the timer expires the player explodes (radius 50, 300 dmg) and dies. Kills while kranked, or of a kranked victim, give +35 "Recharge"/"Clock Work". Packet `chrg sid,ms` |
| 28 | tdf | Team Defender | NEW | 2 | score | – | FLAG with `tm:-1` (neutral) | `oneflag`: anyone can pick it up (+20). The carrier gets `objScore` 20 every 1.5 s (1 when there are <4 clients, no host and not comp). Kill score × carrier 1.25 / carrier's team 1 / others 0.5 (`players.js:4094-4098`). No capture |
| 29 | depoffa | Deposit FFA | NEW | – | first to 65 deposits | `randObjective` | OBJECTIVE | Like depo but FFA: objScore 75, deposit every 750 ms, killMlt 0.5, pickup +5 |
| 30 | car | Carrier | NEW | – | score | – | neutral FLAG | tdf without teams; preventCustomGames |
| 31 | aon | All or Nothing | NEW (event 4) | – | score | forceSpawn 3 | – | forceClass 9, loadout [16], ammoLimit 0, noReloads, noKnives, noPickups. Victim drops spawnable 6 Ammo Pack (`c` = victim ammo) and a 30% Melee Pickup (8). A killer on a streak ≥5 has a 50% chance of a random effect from {0,2,9}. doubleXP; preventCustomGames |
| 32 | md | Mag Dump | NEW (event 5) | – | score | forceSpawn 3 | – | forceClass 9, loadout [2], noReloads. Victim drops spawnable 7 Weapon Box (random weapon from `[0..11,14,16,18,21,27]`) and a 20% Melee Pickup; preventCustomGames |
| 33 | chs | Chaos Snipers | NEW | – | score | forceSpawn 3 | – | forceClass 0, loadout [0], maxHealth 10, ammoLimit 1, noKnives, gravMlt 0.92, jumpMlt 1.5, groundAccel 2, airAccel 1.4 (unused), reloadMlt 0.5, frSlid, wallJ (M 100, D 0.05, V 0.06, YV 2) |
| 34 | bhffa | Bighead FFA | NEW | – | score | – | – | Killer's `headMlt = min(3, 1+0.1·kills)` → `updateHeadSize` (head hitbox, `players.js:1440`); packet `bh sid,mlt` |
| 35 | zom | Zombies | NEW | co-op | `winCondition` always 2; ends when no one is alive; score = wave×100 (`onPreEnd`) | endless waves; 1 life; downed players revive each wave | SERVER_NODE `nt` = `zombie_spawn` (required), `skull_spawn`, `portal_spawn`, `frog_spawn`; MYSTERY_BOX, PERK_MACHINE, KRUM_MACHINE, GATE, DEPOSIT_BOX, WEAPON_PICKUP | §4.1b |
| 36 | imp | Impulsed | NEW | – | score | forceSpawn 3 | – | forceClass 0, loadout [5,22]. Explosion damage ×5 (others only, `explMlt`), radius ×2 (`explSMlt`), impulse ×3, `nonObjImpulse`, refill on kill when the knife was thrown; movement as chs; preventCustomGames |

**New mode keys and their consumers** (all v7):

- **Physics**
  - `gravMlt`: `player.js:1224`
  - `jumpMlt`: `players.js:1699,1704`
  - `groundAccel` (ground and air): `player.js:1167`
  - `frSlid`: `:1206`
  - `wallJ`/`wallJM`: `:694-695`
  - `wallJD`: `:1221`
  - `wallJV`/`wallJYV`: `:1970-1971`
  - `noWallJ`: `:1220`
  - `noSliding`: `players.js:1632`
  - `impulseMlt`: `players.js:2892,3016`
  - `nonObjImpulse`: `:2891,3014`
- **Weapons**
  - `reloadMlt`: `player.js:1847`, `players.js:373,1822`
  - `explMlt`/`explSMlt`: `game.js:877-897`
  - `noKnives`: `player.js:376`, `players.js:1201`
  - `noMelee`: `player.js:591,685`
  - `finiteAmmo`: `player.js:1837,1854`
  - `unrestrictedWeapons`: `players.js:1565,3282,3857,4339`
  - `forceCharge`: `player.js:1070`
  - `bonuses`: `player.js:1236,1847`, `players.js:2239,2707`
  - `bighead`/`headMlt`: `players.js:1444`
- **Health and score**
  - `maxHealth`: `player.js:691`, `bots.js:134`
  - `regenMlt`: `player.js:699`
  - `killMlt`: `players.js:4091`
  - `carrierMlt`/`teamCarrierMlt`/`nonCarrierMlt`: `players.js:4097`
  - `pointsPerHit`: `game.js:932`, `projectiles.js:276`, `players.js:2412,3092`
  - `noScrLimit`: `players.js:4113`
  - `noClassScr`/`randClassScr`: `:4126-4127`
  - `doubleXP`: `game.js:3448`
  - `disabledStreaks`: `players.js:4591`, `airDrop.js:80`
- **Teams**
  - `multiTeam`: `players.js:127`, `game.js:3615`
  - `modelOverrides`: `players.js:699`
- **Objectives**
  - `dom`, `captureTime`, `perPlayerMlt`, `lockoutMlt`: `game.js:1006,1673,1709-1817`
  - `oneflag`: `game.js:996,1826,1936`, `player.js:1489,1732`
  - `deposit`: `index.js:14686`
  - `tagHoldLimit`: `medals.js:151,167`
  - `propSwitches`: `index.js:15842`
- **Hooks**
  - `update`: `game.js:4933`
  - `collisionCheck`: `player.js:1693`
  - `onAIDeath`: `AI.js:1111`
  - `onTrigger`: `triggers.js:1606`
  - `onPreEnd`: `game.js:4549`
  - `onEnd`
- **Network and visibility**
  - `viewDis` (server relevance culling): `game.js:5012`, `players.js:514`
  - `noMinimap`: `Minimap.js:350`
  - `botsAsAI`: `players.js:3951,4702`
  - `botBypassClass`: `player.js:645`
- **Speed-run**
  - `logTime`, `ownTime`, `allowReset`, `showHour`: `game.js:3529`, `players.js:4027-4050`
- **Client-only metadata:** `skipStats`, `party`, `tag`, `event`, `sub`, `leaderStats`, `leaderSort`, `forceOldScoreboard`, `showAllLeaders`, `custUI`.

**Engine flow changes that apply to every mode** (`game.js:3500-3700, 4440-4530`):
- `finishInit` resets, in this order: builds, flags, spawnables, pickups, gates, mystery boxes, krum machines, banks, zone, sites, dom zones, streaks. Then it calls `gameStart`.
- `forceSpawn = config.forceSpawn ‖ mode.forceSpawn`.
- `minPlayers = config.minPlayers ‖ mode.minPlayers`, or 2 when lives are set.
- `roundCount = config.gamRounds − 1`.
- `bstOfR` sets `bestOf`; `noDraws` sets `needWinner` (overtime "+60 Seconds" via `tx`, `game.js:4462,4529`).
- Warmup is `warmupTime` minutes plus a 5 s countdown (`getPop`/`getSnd`).
- New conditions: a `lives` condition for multiTeam (`game.js:3615`) and a scoreLimit condition (`:3667`).
- `multiObjectTrigger("onGameStart")` fires.
- **Rounds:**
  - Between rounds there is an intermission of `intermTmr` s. Movement is locked, spawns are flipped (`flippedSpawns`) and the objective is re-picked (`game.js:4470-4510`).
  - Packets: `rds roundsLeft,false,winTeam,teamRoundWins` and `lkt` (lockout timer, `game.js:4770-4778`).
  - With `bestOf`, a team wins when `teamRoundWins ≥ gamRounds/2`.
- **v7 removed the generic objective scoring** that v3 ran in `game.update` (`v3 game.js:3189-3210`). Each mode now scores its own objective (point, king, depo, depoffa).

#### 4.1a Domination (`game.js:1660-1823`, `dom.js`)

**Zone state:** `capturedBy`, `captureT`, `lastTeamOn`, `teamsOn[]`, `onPoint[]`, `mlt`.

**Server tick:**
- Every 1.5 s, each captured zone gives its owner team +10 (`giveTeamScore`).
- When exactly one team is on a zone it does not own, `captureT += dt·mlt`:
  - `mlt = 1+(n−1)·perPlayerMlt` (0.2), where n is the number of its players on the zone;
  - ×`lockoutMlt` 1.5 when another team holds every zone.
- When `captureT` reaches `captureTime` 10000:
  - set `capturedBy` and broadcast `cct uid,team`;
  - every capper gets +200 "Secured" (caps++), plus +25 "Team Work" when more than 2 are on the zone;
  - a team that now holds every zone gets +250.
- With no team on the zone, `captureT` and `lastTeamOn` reset. While contested, `captureT` is frozen.

**Packets:** `uct [uid,capturedBy,captureT,lastTeamOn,teamsOn,mlt]…` on change; `sync.doms` for joiners.

**Client:** runs the same function to interpolate `captureT`.

**Kills:** killing while on your own captured zone, or killing someone on their team's zone, gives +25 "Defender"/"Attacker" (`dom.js:26-46`).

#### 4.1b Zombies (`modes/zom.js`)

- **AI templates** (`zom.js:2-343`): Walker (200 hp), Brute (550), Flesh Colossal (2500), Runner, Rascal.
- **Special waves** on every 5th wave (`:344-358`): FRENZY, ONSLAUGHT, DEVASTATION, ANNIHILATION, LIMBO.
- **Wave loop** (`:482-620`): every 10 s, if no AI is alive and none are left to spawn:
  - `zWave++`;
  - downed players revive;
  - concurrency +2 per wave (cap 30);
  - type counts grow (caps 30/10/5/4);
  - HP ×+0.1 every 2 waves; damage and speed ×+0.1 every 4 waves;
  - broadcast `upz wave,specialName`.
- **Spawning:** at random `zombie_spawn` nodes via `map.manager.addAI` + `AI.spawn`.
- **Economy:**
  - starterScore 150;
  - pointsPerHit 0.085×damage;
  - when a player dies, everyone loses 10% of their score.
- **Drops:**
  - power-ups on AI death with rarities `[89,5,4,2]` over spawnables `[[],[9,14],[11,15],[10,12,13]]`;
  - 1% chance of a Perk Bottle (19);
  - an Ammo Cache (9) on the last kill of every 5th wave.
- **Easter eggs:**
  - pack-a-punch order (`eePAPOrder`);
  - `onTrigger` (action 71) spawns a Zombie Portal (20) at `portal_spawn`;
  - a Frog (17) at `frog_spawn` every random 4-7 waves (20% chance);
  - a dance timer with skull spawns.
- **Power-ups:** `updatePowerups` (`game.js:1412`) handles insta-kill, fire sale and double points; packet `pwup type,ms`.
- **Perks:** at most 4; `upk sid,perks`.
- **Hosting:** hostType 3 only (§4.3.6).

#### 4.1c Spawnables (`data/medals.js` = the v7 spawnable table; v3 `spawnables.js` had only 0 "Tag")

| idx | name | idx | name | idx | name |
|---|---|---|---|---|---|
| 0 | Tag (8 s) | 9 | Ammo Cache | 18 | Krunkanium |
| 1 | Gift | 10 | Nuke | 19 | Perk Bottle |
| 2 | Golden Egg | 11 | Minigun | 20 | Zombie Portal (aiOnly) |
| 3 | KR Tag (12 s) | 12 | Insta-kill | 21 | Skull EE (canShoot) |
| 4 | Diamond Egg | 13 | Discount | 22 | Free Upgrade |
| 5 | Slime Pad (alwaysActive) | 14 | Double Points | 23 | Mega Slime Pad |
| 6 | Ammo Pack | 15 | Grenade Launcher | 24 | Loot Drop (playerSpecific) |
| 7 | Weapon Box | 16 | Joe Tag | | |
| 8 | Melee Pickup | 17 | Frog | | |

**Fields:** `scale`, `padMlt`, `yOff`, `timer`, `onPickup`, `onCollision`, `onShoot`, `onDestroy`, `stopInteract`, `getScale`, plus the flags `aiOnly`, `playerSpecific`, `alwaysActive`, `dontDestroy`.

**Runtime:** `dropSpawnable(type, owner, data, pos)` (`game.js:2033-2052`); pickup and shoot at `:2053-2111`; despawn at `:2162-2184`.

**Packets:** `crsp type,sid,x,y,z,team,data` (v3 had a timer and no team or data), `dsp sid`. A `playerSpecific` spawnable sends `crsp` to its owner only.

### 4.2 gameTemplates (`data/gameTemplates.js`; NEW, editor only)

These are the editor's "new map from template" entries (`editor.js:60, 866, 6135-6199`). Each entry is `{name, icon, data:"<map JSON string>"}`. Dump one with:

```sh
node -e 'console.log(require("…/gameTemplates.js")[i].data)'
```

They are small, pure JSON, and cover every config shape, so they make **ready-made test fixtures**.

| # | name | notable keys | `config` | prefab ids |
|---|---|---|---|---|
| 0 | Quickstart | lighting | – | 0 |
| 1 | Ocean | `ocean, ocCol, ocOpac, ocDist, ocSpd, ocYOff, ocRefRes, ocW, ocL` | – | 0,32,44,63 |
| 2 | Vehicle | `game {camType 2, camOffY 5, camOffZ 40, mdl 2, aID, mdlS, mdPS, mdPY, mdlDr, movSch 6, trnSp, grA, grDec, arA, airDec}` | – | 0,1 |
| 3 | SRM Template | skyDome*, sunAngY, `game` (movSch 5) | – | 0,9,27 |
| 4 | Pirate | ocean*, terrain*, sizeMlt | classes [7]; settings {bDrop:1}; weps {8:{…}}; cls {0:{camType 2, mdl 2, aID, movSch 6, …}} | 0 |
| 5 | Character Animation | `game` | – | 0,29,55 |
| 6 | Flappy Bird | `game {camType 2, movSch 2, speedX 3.6, infJ, ovrJump, mdl 1}` | modes [4], classes [9], settings {gravMlt .2, jumpMlt 3} | 0,29,30 |
| 7 | Battle Royale | terrain*, skyDome*, `zone, zoneSize, zoneSpeed, zoneRnd, zoneCol0..2` | modes [8], classes [9], {maxPlayers 16}, weps {18:{spdMlt 3}} | 0,26,29,38 |
| 8 | Advanced Triggers | – | – | 0,29,30,64×10 |
| 9 | Parkour | dthY | modes [4] | 0,10,25 |
| 10 | Zombie Infected | – | modes [6] | 0,3,26 |
| 11 | City | skyDome*, camPos, `game` | modes [4], classes [9], {maxPlayers 20, gameTime 0} | 0,3,9,15,21,29,48,55,64 |
| 12 | Top Down | dthY, `game` | classes [9], {maxPlayers 20, gameTime 0, jumpMlt 1.4} | 0 |
| 13 | Trading | `game` | modes [18], classes [9] | 0,30,42,43 |
| 14 | Defuse | – | modes [20] | 0,1,9,26,45 |
| 15 | Spleef | dthY, `game` | as Top Down | 0 |
| 16 | 2D Platformer | dthY, `game` | modes [4], classes [9], {jumpMlt 1.5}, weps {18:{spdMlt 3}} | 0,46 |
| 17 | Basic .io | `game` | modes [4], classes [9] | 0,29 |
| 18 | Racing | – | modes [4] | 0,27,29 |
| 19 | Mining | – | modes [1] | 0,24,26,29 |
| 20 | Moba | dthY, `game` | modes [3] | 0,23 |
| 21 | Aim Trainer | – | modes [4], classes [9], {maxPlayers 20, gameTime 0} | 0,26,29 |
| 22 | Dog Fight | skyDome*, dthY, `game` | – | 0,9 |
| 23 | Trivia | – | {maxPlayers 20, gameTime 0, throwMel 0}, weps {12:{range 100}} | 0,29,30 |
| 24 | Sniper VS Runner | – | modes [6], classes [1], {gameTime 8, noSecondary 1, throwMel 0} | 0,27,46 |
| 25 | Puzzle | – | modes [1] | 0,10,29,30 |
| 26 | Sniper Arena | – | modes [1] | 0,9,47 |
| 27 | Raid | – | modes [23] | 0,24,26,29,55 |
| 28 | Brawl | dthY | modes [8], classes [9], {maxPlayers 4, gameTime 0, lives 1, healthMlt 2, killRewards 0, throwMel 0, t1Dmg .3, t2Dmg .3} | 0,9,29 |

### 4.3 Custom game config

- **v3** already had custom game config: `validConfig` (`v3 game.js:15`), `applyConfig` (`v3 game.js:369`; `weps/classes/modes/maps`), 39 `serverConfig` entries.
- **v7** moves validation to `data/customGameConfig.js` (`validateConfigs` :62, `validateMap` :152, `setupGameConfig` :369), grows `serverConfig` to 85 entries, and adds `cls`, `prks`, host types and the map-embedded `config` (§1.9).

**Apply semantics** (`game.js:346-604`, `applyConfig(cfg, hostType, hostIndex)`):
- **bool:** `cfg[k]==null` → (comp && `defC`!=null ? `defC` : `def`); otherwise `!!cfg[k]`.
- **input:** `sanitizeStr`, ≤16 characters.
- **select:** an invalid value falls back to the default.
- **number:** a non-number falls back to the default, then the value is clamped to [min, max].
  - The max depends on the host type: `maxD` for −2, `maxF` for 3, `maxP` for 2, `maxG` for 0.
- **`setupGameConfig`:**
  - resets every option whose `hostTypes` list (default [0]) excludes the current host type;
  - applies `overDat`, then the requested overrides, then `forceDat`/`allowedDat`;
  - strips `preventCustomGames` modes (`customGameConfig.js:369-448`).
- `dontChange`, `basic`, `sub`, `head`, `hide` and `hideE` are UI metadata only.

#### 4.3.1 `serverConfig` (`config.js` serverConfig; N = new, C = changed, = = same)

| Group | varN | label | def (defC) | range | consumer (v7) | |
|---|---|---|---|---|---|---|
| Size | maxPlayers | Players | 2 | 1..10 (maxP 16, maxF 20, maxD 50, maxG 6) | applyConfig, `server/index.ts:1258` | C |
| | minPlayers | Min Players | 0 | 0..10 | `game.js:3510` | C |
| Timers | gameTime | Minutes | 4 (4) | 0..60, hostTypes [0,1] | `game.js:3522` (mode.gameTime overrides) | C |
| | warmupTime | Warmup | 0 | 0..4 min step .1 | `game.js:3550-3592` | = |
| | gamRounds | Rounds | 1 (3) | 1..10 | `game.js:3533, 4468-4564` | N |
| | intermTmr | Intermission (s) | 30 (30) | 5..120 | `game.js:4492` | N |
| | forceSpawn | Auto Respawn (s) | 0 | 0..15 | `game.js:3508` | = |
| Health & Lives | lives | Lives | 0 | 0..10 | `game.js:3511, 3550, 3602`; `player.js:92` | = |
| | healthMlt | Health Multiplier | 1 | 0.1..3 | `player.js:691`; `bots.js:126-134` | = |
| | hpRegen | Health Regen | true | bool | `player.js:699` | = |
| Score | scoreLimit | Score Limit | 0 | 0..10000 step 100 | `game.js:3667` | = |
| | keepTScore | Keep Team Score | false | bool | `players.js:4117` | N |
| | killRewards | Kill Rewards | true | bool | `players.js:4828` | = |
| | noAssists | No Assists | false | bool | `players.js:4855` | N |
| Mode | objtvTime | Objective Time (min) | 1 (1) | 0.1..10 | `game.js:2876-2880` | N |
| | forceC | Force Mode Classes | true | bool | `player.js:655` | N |
| | logTim | Log Time (Race/Bhop) | true | bool | `game.js:3529` | N |
| | lstChkT | Checkpoint Time Rewind | false | bool | `players.js:4027` | N |
| Physics | gravMlt | Gravity | 1 | **0**..2 step .001 | `player.js:1224` | C (v3 min .1) |
| | deltaMlt | Time Scale | 1 | 0.1..1.5 | `game.js:4911-4919`; `AI.js:456` | = |
| | fallDmg | Fall Damage | 0 | 0..10 | `players.js:1649-1653` | N |
| | fallDmgThr | Fall Damage Thresh | 0 | 0..10 | `players.js:1651` | N |
| | bDrop | Bullet Drop | false | bool | `players.js:2820` | = |
| Jumping | jumpMlt | Jump Force | 1 | 0.1..3 step .001 | `players.js:1699,1704` | C |
| | wallJP | Wall Jump Power | 1 | 0..2 | `player.js:1220` | = |
| | autoJump | Auto Jump | false | bool | `players.js:1741` | = |
| Movement | fixMov | Fixed Movement | false | bool | `movement.js:57` | N |
| | slidTime | Slide Time | 1 | 1..5 | `players.js:1643` | = |
| | slidSpd | Slide Speed | 1 | 0..2 | `players.js:1645` | N |
| | strafeSpd | Strafe Speed | 1.2 | 1..2 | `movement.js:46` (ignored when realMov) | = |
| | canSlide | Sliding | true | bool | `players.js:1633` | = |
| | airStrf | Air Strafe | false | bool | `player.js:1256` | = |
| Gameplay | impulseMlt | Weapon Impulse | 1 | 0..3 | `players.js:2892,3016` | = |
| | fiRat | Fire Rate | 1 | 0.5..2 | `players.js:2239,2707,2739` | = |
| | reSpd | Reload Speed | 1 | 0.5..2 | `player.js:1847`; `players.js:373,1822` | = |
| | headshotOnly | Headshots Only | false | bool | `projectiles.js:301`; `players.js:3056` | = |
| | noSecondary | No Secondaries | false | bool | `player.js:676` | = |
| | throwMel | Throwable Melees | true | bool | `player.js:1034` | = |
| | chrgWeps | Chargable Weapons | true | bool | `player.js:1067`; `players.js:2244,2677` | N |
| Collisions | hitBoxPad | Hitbox Padding | 0.6 (0.1) | −2..2, hostTypes [0,1] | `player.js:272,277`; `players.js:2354,2946` | N |
| Teams | selTeam | Select Team | false | bool | `game.js:201` (`updLobbyP`, team cap `tmSize+1`) | = |
| | frFire | Friendly Fire | false | bool | `players.js:2343,2372,2935,2966,4239`; `projectiles.js:266,310` | N |
| | nameTeam1..2 | Alpha/Bravo Team Name | "Alpha"/"Bravo" | str ≤16 | UI | C (v3 "Team 1/2") |
| | nameTeam3..5 | Charlie/Delta/Echo | … | str, hidden | sdm | N |
| | t1Dmg, t2Dmg | Team damage | 1 | 0..2 | `players.js:4239` (`config["t"+team+"Dmg"]`) | = |
| | t3Dmg..t5Dmg | | 1 | 0..2, hidden | same | N |
| Other | noStreaks | Disable Streaks | false | bool | `players.js:4738,4790` | = |
| | disableB | Disable Borders | false | bool | `player.js:814-818,1606-1615` | = |
| | allowSpect | Spectating | true | bool | `index.js:13676,17255` | = |
| | thirdPerson | 3rd Person | false | bool | `players.js:255,318,694` | = |
| | nameTags | Hide Nametags (Enemies) | false | bool | `index.js:14819` | C (label) |
| | nameTagsFR | Hide Nametags (Allies) | false | bool | `index.js:14820` | N |
| | kCams | Kill Cams | true | bool | `index.js:15197` | N |
| | aAnon | Allow Anonymous | true | bool | no consumer | N |
| | specSlots | Designated Spectators | 2 | 0..4, comp | `customGameConfig.js:400` | N |
| | tmSize | Team Size | 3 | select 1v1..4v4, comp | `game.js:203`; `point.js:48` | N |
| | noCosm | Hide Cosmetics | false (true) | comp | `players.js:599` | N |
| | tstCmp | Test Mode | false (true) | comp | `index.js:13826` | N |
| | noDraws | No Draws | false (true) | comp | `game.js:3535` | N |
| | bstOfR | Best of | false (true) | comp | `game.js:3534,4468` | N |
| | headClipFix | Stop Head Clipping | false (true) | bool | `player.js:1125,1636,1640` | N |
| | minimaps | Minimaps | true | bool | `Minimap.js:350` | N |
| Class Limits | limitClasses | Limit Classes | false (true) | comp | `game.js:182` | N |
| | classLim0..15 | per class | 0 (1/0) | 0..20, comp | `game.js:184` | N |

Removed from v3:
- `maxPS` and `promServ` moved to `config.serverUpg` (`config.js:322-330`), with costs 1000/2500.
- `requireCaptcha`.

#### 4.3.2 `weaponConfig` (`weps[id]`; 48 keys, v3 had 26)

**New keys:**
- `name` (str 24), `lsMlt`
- `burst`/`burstC`/`burstR`
- `reloY`
- `ovrChrg`/`chrgTime`
- `movSpread`, `adSpread`
- `equipment`, `assetIcon`
- `refDist`, `rollOff`
- `pj_disable/asset/dmg/spd/scale/length/range/grav`. These are copied into `weapon.projectile` and build `customProj` (`game.js:394-427`).

**Range changes:**

| Key | v3 | v7 |
|---|---|---|
| dmg min | 1 | −200 |
| ammo max | 100 | 500 |
| rate step | 10 | 5 |
| spread min / step | 10 / 10 | 0 / 0.1 |
| minSpread min | 5 | 0 |
| zoom max | 3 | 15 |
| recoilYM max | 1 | 2 |
| physRang max | 100 | 2000 |
| physPow min | 0 | −2 |

#### 4.3.3 `classConfig` (`cls[id]`; 79 keys; v3 had 17, applied map-wide through `game`)

| Group | Keys |
|---|---|
| Camera | `camType` (FPS / 3rd Person / Platformer X / Platformer Z / Orthographic; index 1 was Top Down in v3), `camOffX/Y/Z`, `orthoF`, `lockCamP`, `lckCmYR`, `yCmLim`, `camRot`, `fPLock` |
| Model | `mdl` (Default / Sprite / Model), `aID`, `mdlFP`, `animS`, `mdlS`, `mdPS`, `mdPY`, `mdlDr`, `mdlYO` |
| Movement | `movSch` (§2.11), `speed`, `speedX/Y/Z` (±50), `spdLnTh`, `movDrP`, `trnSp`, `grA` .042, `grDec` .99, `arA` **.0047 absolute**, `airDec` .9996, `canCrch`, `crchSpd` .3, `ldA`, `slT`, `slA`, `frSlid`, `bkSlid`, `stpFr`, `stpSD` |
| Vehicle | `vhdr` .97, `vhaDr` .94, `vhtL` −2.7, `vhtrsh` .0025, `vhtrsv` 0, `vhmP` .12, `vhpF` .0012, `vhmR` .0375, `vhrF` .0005 |
| Health | `health`, `segs`, `regen`, `regenDelay` |
| Hitbox | `hbS`, `hbH` |
| Jumping | `jpC`, `disJump`, `infJ`, `ovrJump` |
| Shooting | `canShoot`, `shtToCam`, `shtDrg`, `shotX`, `shotY`, `hipF`, `croSh`, `airSh` |
| Collision | `pColl` |
| Weapons | `canMelee`, `throwChrg`, `melRate`, `secondary` |
| Wall jump | `wallJ`, `wallJM`, `wallJD` .3, `wallJV` .03 |
| Crosshair | `cID`, `ccID` |

Applied in `game.js:428-475`. The physics semantics belong to the player-physics work package (WP-F).

#### 4.3.4 `effectConfig` (`prks[id]`, NEW)

Keys: `name`, `max` (1..10), `timer` (0..120 s), `mlt` (0..10), `hp` (0..100). Applied over `data/effects.js` in `game.js:476-520`.

#### 4.3.5 Validation limits (`customGameConfig.js:152-368`)

See §1.5.2. Also:
- `tct`/`tcc` must be ≤70 characters and match the allowed character set.
- `cv*` values are type-checked against `triggers.conditions[cd].vals`.
- The interface-per-id limit is 30; the v7 error string still says 20.

#### 4.3.6 `hostTypes` (`config.js`; v3 had 3)

| # | name | data |
|---|---|---|
| 0 | Custom | – |
| 1 | Comp | `overDat {private, allowSpect, maps:[0,2,4,5,11,12,14,17,23], modes:[2], maxPlayers 10, minPlayers 8, fixMov, jumpMlt 1.005, slidSpd .97, warmupTime .2, selTeam, throwMel false}`. `comp` sets `minPlayers=(tmSize+1)·2` and `isComp` |
| 2 | Rent | – |
| 3 | Zombies | `forceDat {private, maxPlayers 4, maps:[27], modes:[35], mimicPubs}`; `allowedDat {maps:[27]}` |
| 4 | Raid | `forceDat {maxPlayers 8, maps:[18], modes:[23], mimicPubs}`; `allowedDat {maps:[18,29,31]}` |

The `maps` values are v7 official-map indices (§4.6).

### 4.4 Airdrops (`data/airDrop.js`, `airDropPackages.js`; NEW, pub only)

**Source and placement:**
- Airdrops come only from **streak 1 "Airdrop"** (12 kills, `noCustoms`; `streaks.js:49-71`). `activate` returns false in custom games.
- The spot needs a clear sky: `canSee(player,x,y+500,z)==null`, otherwise "This place is not suitable for an Airdrop". The drop spawns at `y+500`.
- **Maps cannot place or configure airdrops.** Every package is `noCustoms`. The only map influence is geometry (the fall raycast) and `dthY`: a drop below deathY is removed (`airDrop.js:177`).

**Fall and claim:**
- `calcFallDist` raycasts 1000 units down (`airDrop.js:131`). The drop falls at 0.05 u/ms and becomes claimable on landing. Size: scale 4, box 10³.
- Claim: a player within radius 6, on the ground, holds interact. `interProg += dt/claimTime` (2500-4500 ms; `player.js:1718-1728`), then `game.claimAirDrop` (`game.js:1357`, server). Claiming another player's drop is "thief".

**Package selection:**
- The tier depends on the owner's account level:

  | Tier | Level | Rarity weights |
  |---|---|---|
  | Starter | <25 | `[75,22,3,0,0,0]` |
  | Elite | ≥25 | `[50,30,15,5,0,0]` |
  | Heroic | ≥50 | `[0,48,35,14,2.49,0.5,0.01]` |

  Refs: `airDrop.js:4-16, 59-79`.
- A package carrying a streak is allowed only if `mode.disabledStreaks` exists and does not contain that streak (`airDrop.js:80`, literal semantics). So in modes without `disabledStreaks`, streak packages never drop.

**Packages:**

| idx | package | contents / effect |
|---|---|---|
| 0-6 | KR | 5 / 10 / 15 / 20 / 30 / 100 / 250 KR |
| 7-13 | Junk | junk |
| 14 | Trapped: Poisoned | effect 6. Trapped packages `imitate` a random real package for everyone except the owner; claimTime 1000 |
| 15 | Trapped: Rooted + Dizzy | effects 4, 5 |
| 16 | Trapped: explosion | radius 50, 300 dmg, credited to the owner |
| 17 | Skin | random `package` skin |
| 18 | Juggernaut | streak 5 |
| 19 | War Machine | streak 6 |
| 20 | Nuke | streak 0 |
| 21 | Slimer | streak 4 |

**Packets:**
- `ad [sid,fallDist,x,y,z,dropIndex|imitate]`, sent once per player (`sentTo`), every `airDSyncT`.
- `rad sid`, `krp owner,claimer`.
- Sounds `adrop_0`/`adrop_1`.

**Streaks** (`streaks.js`; v3 had only the Nuke):

| # | streak | kills | notes |
|---|---|---|---|
| 0 | Nuke | 25 | `n`; fires onNukeStart / onNukeEnd in custom games |
| 1 | Airdrop | 12 | – |
| 2 | Refill | 5 | dev only |
| 3 | Bombardment | 20 | dev only; `bbm` |
| 4 | Slimer | 7 | – |
| 5 | Juggernaut | 30 | 2000 hp + Minigun; `jg` |
| 6 | War Machine | 15 | – |

Weapon-streak packet: `wstk`. Controlled by `config.noStreaks` and `mode.disabledStreaks`.

**KN:** offline and custom games never see airdrops (they are pub only and require account levels). **Decision:** implement them last (WP-H), and only for public-server lobbies.

### 4.5 Other map-configurable systems

- Rounds, intermission, best-of, overtime: §4.1.
- Effects: §2.10.
- Spawnables: §4.1c.
- Per-class camera, movement and vehicle schemes: §4.3.3.
- Battle-royale zone: §1.8.2.
- More than 2 teams: sdm.
- `botsAsAI`: raid.
- Builds: §1.7.

### 4.6 Official map catalogue and rotation (policy consequence)

v7 `data/maps.js` lists **32** official maps whose indices differ from v3/KN.

**v7 indices:**

| idx | map | idx | map | idx | map | idx | map |
|---|---|---|---|---|---|---|---|
| 0 | Burg (`burg_new`) | 8 | Citadel | 16 | Lagoon | 24 | Old_Burg (`burg_2018`) |
| 1 | Littletown | 9 | Oasis (`oasis_v3`) | 17 | Bureau | 25 | Throwback (`throwback_2018`) |
| 2 | Sandstorm (`sandstorm_v3`) | 10 | Kanji | 18 | Tortuga (`shipwrecked`) | 26 | Stockade (`stockade_2018`) |
| 3 | Subzero (`subzero_new`) | 11 | Industry | 19 | Tropicano | 27 | Facility |
| 4 | Undergrowth | 12 | Lumber | 20 | Krunk_Plaza (`krunk_plaza_v2`) | 28 | Clockwork |
| 5 | Shipment | 13 | Evacuation | 21 | Arena | 29 | Laboratory |
| 6 | Freight | 14 | Site | 22 | Habitat (`habitat_v2`) | 30 | Shipyard |
| 7 | Lostworld (`lostworld_s6`) | 15 | SkyTemple (`skytemple_v2`) | 23 | Atomic | 31 | Soul Sanctum (`sanctum`) |

- `rotationMaps = [0,1,2,3,4,6,7,8,9,10,11,12,13,14,15,16,19,21,22,23,24,25,26,28]` (`map.js:30`).
- The files are in `/home/user/jails/kru/src/Krunker_v7.1.4/public/maps/`.
- KN's catalogue (`KN shared/src/map_catalog.c:24-39`, `MAP_CATALOG_OFFICIAL_COUNT 15`) uses the v3 indices: 5 Shipyard, 11 Newtown, 12 Industry, 13 Soul Sanctum, 14 Evacuation.

**Decision:** under the in-place policy, the official catalogue becomes v7's 32 entries and indices. This is required because hostTypes, `config.maps`, comp lists and the end-of-game `mts` reference these indices.
- Importing the v7 map JSON files is a content import, as `make assets-import` already does for textures. It is isolated in WP-A step A6 so it can be scheduled separately.
- The v3 versions of the maps stay available as custom maps: v7's 32 files sit at `assets/maps/<fileName>.json` under their v7 names, the v3.1.1 official maps at `assets/maps/311_<name>.json` (catalogue key `311_<name>`, shown as "Burg (3.1.1)") and v3.1.1's own `old_*` maps at `assets/maps/152_<name>.json` ("Burg (1.5.2)"). The old keys (`v3_<name>`, `old_<name>`) and v3.1.1 file stems (`burg`, `sandstorm`, ...) resolve to them through the alias table in `map_catalog.c` (decisions.md, "Official maps").

## 5. Server vs client responsibilities and packets

### 5.1 Authority matrix

"Both" means the code runs in the shared player simulation: the client runs it for prediction, the server is authoritative, and the client is corrected through `"l"` reconciliation. In KN, every "server" or "both" row lives in `shared/` (or in server code linked into the offline sandbox), and its output goes through `GameHooks.send`.

| Feature | Authority | Client role | Packets (S→C unless marked) |
|---|---|---|---|
| Solid / ramp / cylinder / border / triangle / terrain collision | both | predicts | `l` (incl. `xVelC`, `zVelC`, `onTerrain`) |
| Jump pads, boost ramps, ladders, rope ladders, wall-jump | both | predicts; plays sounds | — |
| Teleporters (incl. `tcp`) | both | predicts | `chgp` only for forced moves |
| Checkpoint set, parkour reset | both state; server confirms | popup / sound | `chp`; C→S input `[13]` |
| Score zones + logTime | server | HUD timer | `5`, `gt`, `t`, `ch` |
| Kill / death zones, deathY | server | — | `3` (kill) |
| BR zone | server (shrink + damage) | tween | `zn`, `sync.zone` |
| Environment zones, ocean, sky, liquids, emitters | client only | camera-based | — |
| NoPVP / PVP flags | both set the flag; damage filtered on the server | — | — |
| Gates / banks / pickups / machines / interact-key triggers / prize wheels | server (`players.interact`) | prompt only (`changeInteract`) | C→S `int type,uid`, `intSec`, `cnfm`; S→C `gte`, `bnk`, `inat`, `mybx`, `smybx`, `krm`, `cnfm` |
| Hold-mouse triggers, airdrop claim | progress on both; firing on the server | progress bar | `p`, `mp`, `rad`, `krp` |
| Triggers: events, conditions, actions, timers, queue, chains | server only | applies the results | §3.9 |
| Destructibles (damage, respawn, spawn chance) | server | hide / show mesh | `do`, `ro`, `sync.dest` |
| Object node moves, object animation | server | moves the mesh and re-quads / plays the clip | `mo`, `mf`, `sync.mov`, `ana` |
| Global / player custom values, GUI toggles | server | HUD | `gv`, `cv`, `sync.gbl`, `tgui` |
| Builds | server | build mesh | `bo`, `sync.dyno` |
| Map AIs | server only (`addAI` skipped when `RENDER`) | interpolate / render | `aai`, `ai`, `rai`, `ana` |
| Map bots | server | — | ordinary player packets |
| Spawn selection | server | — | spawn |
| Mode rules, rounds, objectives, dom, flags, spawnables, streaks, effects, airdrops | server | render / HUD; client interpolates dom `captureT` | `obj`, `uobj`, `uct`, `cct`, `ufl`/`ufp`/`ufi`/`ac`, `crsp`/`dsp`, `krt`, `chrg`, `bh`, `rds`, `lkt`, `tx`, `upz`, `pwup`, `upk`, `jg`, `wstk`, `pk`, `ad`/`rad`/`krp`, `n`, `bbm` |
| Config apply (`applyConfig`) | server; the client re-runs it from init data | — | `init.config` / `customData` |
| Map load / rotation / votes | server | fetches official map JSON; custom maps arrive inline in `init.customMapData.data` (`index.js:13455-13470`) | `init`, `end`, `mv`; C→S `maVote` |
| KrunkScript | server script on the server, client script on the client | — | `sd`, `ksL` |

### 5.2 Map / object / trigger / mode packets

**Sources:**
- `packets.json` (`PacketNames` S→C, `OutgoingPacketNames` C→S).
- Client handlers: `index.js:18051-18275` (online) and `:18416-18560` (sandbox, the same table).
- Server inbound: `server/index.ts:4100-4300`.
- v3 client table: `v3 index.js:13699-13830`.

**S→C**

| key (name) | payload | origin | v3 |
|---|---|---|---|
| `init` (INIT_GAME) | `mapIndex, modeIndex, eventIndex, teamRoundWins, teamScores, host, config, customData, modURL\|0, customMapData{data,n,id,accType,creator,info,thumb,hasLM}, syncData, networkData, isSandbox, isOfficial, isComp, sessionId, roundId, gameTimer` | `game.js:4757-4759`; `index.js:18315` | changed: v3 had 11 args (`v3 server/gameserver.ts:1898-1950`) |
| `init.syncData` | `{gor, lockT, roundC, zone[x,z,sx,sz], lck, obj[uid,ms], pwup[3], flg[[uid,x,y,z,carrier,timer]], spwns[[i,sid,x,y,z,team,data]], gbl[k,v,…], doms[…], pkups[[uid,pickup,org,active,[x,y,z]]], mybx[…], krm[…], gates[uid,act,…] (flat), banks[uid,dep,…] (flat), dyno[[type,x,y,z,w,h,l,0,uid]], dest[uids], mov[uid,node,…], exf, asnd[sid,name,data,…]}` | `game.js:3842-3954` | changed: v3 had `{dest, flg, spwns(timer), gor, pkups, gates[[uid,act]], banks[[uid,dep]], bill, zone, lck}` |
| `do` / `ro` | uid | destroy / respawn | same |
| `gte` | uid, active | gate | same |
| `bnk` | uid, deposited | bank | same |
| `mo` / `mf` | objIndex, nodeIndex / objIndex | node move | NEW |
| `ana` | uid\|sid, clip, raw, isObj | animation | NEW |
| `bo` | buildInd, x, y, z, w, h, l, dir, uid | build | NEW |
| `chp` | `()` set / `(true)` reset | checkpoint | changed |
| `chgp` | id, x, y, z, recon, stopMo, dir | forced move | NEW |
| `lm` / `ulm` / `lock` / `lkt` | lock move / unlock / lock / lockout timer | — | `lkt` NEW |
| `zn` | [x, z, sx, sz] | zone | same |
| `obj` | uid, timeMs, initial | objective | changed (v3: index) |
| `uobj` | uid, onZone | hill occupancy | NEW |
| `gt` | time, zoneIndex | speed-run | NEW |
| `p` / `mp` | progress | interact progress to spectators | `mp` NEW |
| `inat` / `inatA` | sid, loadout, idx, uid, pickupLeft, active, true, null, ammos, reserves, upgrades, true / all | loadout | changed (v3: 6 args) |
| `uamo` | ammos, reserves | ammo | changed |
| `crsp` / `dsp` | type, sid, x, y, z, team, data / sid | spawnables | changed |
| `aai` / `ai` / `rai` | getData / stride-9 sync / sid | AI | `rai` NEW; the other two changed |
| `ad` / `rad` / `krp` | airdrop | — | NEW |
| `mybx` / `smybx` / `krm` / `pwup` / `upk` / `upz` | zombies economy | — | NEW |
| `uct` / `cct` | dom zones | — | NEW |
| `krt`, `chrg`, `bh`, `rds`, `tx`, `jg`, `wstk`, `pk`, `bbm` | mode outputs | — | NEW |
| `cv` / `gv` / `tgui` / `camb` / `sso` / `asnd` / `rsnd` / `spk` / `rg` / `uai` / `shk` / `ulb` | trigger and KrunkScript outputs | — | NEW |
| `s` / `so` / `is` / `scr` / `am` / `ch` | sounds, jump scare, popup, chat | — | same packets; `am` and `ch` payloads changed (§3.9) |
| `cnfm` | uid, type (2 = spinner) | KR confirmation | NEW |
| `mv` | `vo{id:[voteId,weight]}`, `mts["mode,map",…]` | match vote | changed (v3 vote values were plain ids) |
| `ksL`, `sd` | KrunkScript | — | NEW |
| `ot` / `tmc` | terminal | — | **removed in v7** |
| `lo`, `slo`, `objmv`, `gsc`, `prc` | listed in `packets.json`, but no sender or handler exists anywhere | — | **do not port** |

**C→S**

| key | payload | handler | v3 |
|---|---|---|---|
| `q` | frames `[isn, dt, pitch·1000, yaw·1000, moveDir, shoot, aim, jump, reload, crouch, scroll, wSwap, 1−speedLmt, reset(0/1/2), interactHeld]` | `server/index.ts:4178-4192`; client `index.js:16828` | changed: v3 had 13 fields; `[13]` and `[14]` are new (`[15]` read at `player.js:949`) |
| `int` / `intSec` | type, uid (the client's `lastInteractData`) | `server/index.ts:3296-3307` → `players.interact` | changed: v3 `int(secondary?)` scanned every object |
| `cnfm` | `()` trigger confirm / `(2)` spinner | `server/index.ts:3309-3321` → `confirmInteract` | NEW |
| `drpw` | — | drop weapon (`:3323`) | NEW |
| `maVote` | `"mode,map"` | `:1292-1297` → `voteMatch(id, voteId, 1)` | changed (v3 `mv`/`vote`) |
| `custom` | host config incl. `rawMapData` | `:4155` → `prepareHostedGame` | changed |
| `sd` | KrunkScript network message | KrunkScript spec | NEW |
| `vad` | 1 / 2 / 3 / −1 | video ad | NEW (KN: ignore) |

### 5.3 KN packet allocation

KN packets are binary structs with numeric `uint8_t` ids (`KN shared/include/shared.h:1070-1161`). Ids in use: 1-35, 40-56, 60-64, 70-74, 80-85, 90-91, 100-105, 110-111.

**Decision:**
- New v7 messages get **new `NET_PACKET_*` ids in 120-199**, each with a fixed payload struct. Do not use v7 string keys.
- Extend `NetObjectStateKind` (`shared.h:1604-1612`) instead of adding packets wherever a message is just "object uid + small value".
- Reserve **200-239** for the KrunkScript spec, so the two specs can be implemented in parallel without id collisions.
- Each work package in §7 owns one block:

| Block | Owner | Contents |
|---|---|---|
| `NetObjectStateKind` + | WP-B | MOVE_TO_NODE (`mo`), MOVE_FROM_NODE (`mf`), ANIM (`ana`, clip index or name hash) |
| 120-129 | WP-C (triggers) | CUSTOM_VALUE (`cv`), GLOBAL_VALUE (`gv`; also the `gbl` join list), GUI_TOGGLE (`tgui`), AMBIENT_CHANGE (`camb`), STOP_SOUND (`sso`), PLAYER_SOUND add/remove (`asnd`/`rsnd`), CONFIRM_INTERACT S→C/C→S (`cnfm`), CHANGE_POSITION broadcast (`chgp`). Interact becomes `{type, uid}` (C→S, replaces the v3-style payload of `NET_PACKET_INTERACT` 61) |
| 130-139 | WP-B (world) | ZONE (`zn`), BUILD (`bo`), LOG_TIME (`gt`), CHECKPOINT reset flag (extend `NET_PACKET_CHECKPOINT` 101 with a payload), ENV/terrain sync (if needed) |
| 140-149 | WP-D (modes) | OBJECTIVE v7 (`obj uid,ms,initial`; replaces the v3 OBJECTIVE payload), OBJECTIVE_OCCUPANCY (`uobj`), DOM_UPDATE (`uct`), DOM_CAPTURE (`cct`), ROUNDS (`rds`), LOCKOUT (`lkt`), TEXT_POPUP (`tx`), MODE_COUNTER (`krt`/`chrg`/`bh`), SPAWNABLE v7 (`crsp` with type, team, data) |
| 150-159 | WP-G (zombies) | MYSTERY_BOX (`mybx`/`smybx`), KRUM (`krm`), POWERUP (`pwup`), PERKS (`upk`), WAVE (`upz`) |
| 160-169 | WP-E (AI/bots) | AI_REMOVE (`rai`), AI_SYNC v7 (stride 9), AI_ADD v7 (`aai`) |
| 170-179 | WP-H (effects/streaks/airdrops) | EFFECT (`pk`), STREAK (`wstk`/`jg`/`bbm`), AIRDROP (`ad`/`rad`/`krp`) |
| 180-189 | WP-A (lobby/maps) | MATCH_VOTE C→S (`maVote`), MATCH_VOTE_STATE (`mv`), NETWORK_DATA (rates) |

The join sync must carry, through the per-feature packets above or a single `SYNC_STATE` blob, every `syncData` key that KN implements: `dest`, `gates`, `banks`, `mov`, `gbl`, `doms`, `pkups`, `mybx`, `krm`, `dyno`, `zone`, `obj`, `asnd`, `spwns`, `flg`. KN's existing join path is `game_send_object_state` (`KN shared/src/triggers.c:620-638`).

### 5.4 Map loading pipeline

**v7.**
1. **Catalogue.** `public/maps.json` plus the local custom maps (`server/index.ts:42, 273-330`). The rotation is described in §4.6.
2. **Startup.** `getStartupMapCandidates` and `getStartupModeCandidates` take the rotation ∩ the map's `modes` (`server/index.ts:1096-1215`).
3. **Custom / hosted maps.** `prepareGame` (§1.1) → `prepareHostedGame` (`server/index.ts:1226-1260`) → `applyConfig(cfg, 3, hostIndex)`. The dev path is `resetGameToLocalMap` (`:531-592`).
4. **`init(mapIndex, modeIndex, eventIndex, mapDataExt, async)`** (`game.js:3378-3463`):
   - **Vote tally.** `endData.vo[id] = ["mode,map", weight]`; the highest summed weight wins, ties go to the first entry, and with no votes the pick is random.
   - Sets the mode and the network rates.
   - Parses `gui`.
   - `map.asyncGenerate` (a worker) on the server, or `generate` in the sandbox / client.
5. **`finishInit`** (`game.js:3464-3797`), in this order:
   1. `resetAll(lowPop)`, then builds, flags, spawnables, pickups, gates, mystery boxes, krum machines, banks, zone, sites, dom zones, streaks, `triggerQueue`, `playerSounds`, `globalVals`, powerups.
   2. `addPendingAIs`, then `addPendingBots`.
   3. Leaderboards, `switchMysteryBox`.
   4. `generateTriggerTypes`, `saveColls`.
   5. Timers, warmup, conditions, scoreLimit.
   6. `mode.gameStart`, then `multiObjectTrigger("onGameStart")`.
   7. Ambient sound (`ambInd`/`ambIndC`), matchmaker metadata, `mainScript.start()`.
6. **End and rotation** (`game.js:4623-4660`):
   - `endTimer` is 25000 ms, or `"inf"` for comp.
   - `endData.mts` holds 4 picks:
     - `forceRaid` gives `"23,raidMapId"`;
     - `forceMap` / `forceMode` apply when set;
     - pubs use `genMatchList` (`matchPicks`=4; per-map `config.modes`; normal / party / event buckets; FFA guaranteed; `game.js:5068-5133`);
     - custom and official games use random `mode,map` pairs from `config.modes` × `config.maps`.
   - Votes arrive as `maVote`; the server broadcasts `mv`.
   - When `endTimer` expires: `init(...)`, then broadcast `init` (`game.js:4754-4761`).

**v3.**
- `game.customMap` (`v3 game.js:515-604`): no scripts, no `validateMap`.
- `init(mapIndex, modeIndex, genMap)` (`:2313-2373`).
- `voteMatch` toggles plain ids (`:2299-2312`); `mts` is 4 random pairs, or the raid pair `"23,13"` (`:2888-2910`).

**KN today:**
- `game_pick_next_map` is random, with no votes (`KN shared/src/game.c:676-692`), and `next_round` was duplicated in `server/src/net.c:130-134` and the former `client/src/net/local_game.c:158-161`. That file is being replaced by the in-process sandbox (`server/src/sandbox.c` + `client/src/net/offline.c`), which runs `server_bin`'s own core, so the server path is now the only one.
- **Decision:** a single shared `game_end_round` / `game_next_match` in `shared/` implements the v7 `mts` / vote / `init` flow for both the server and the sandbox. This removes the duplication.

## 6. Gap analysis against Krunker Box Office

**Status legend:**
- **supported**: matches v7.
- **partial**: works, but with v3 semantics or with v7 pieces missing.
- **missing**: not implemented.
- **wrong**: conflicts with v7 and must change.

The WP column names the owning work package in §7. KN paths are relative to `/home/user/src/KrunkNative/`.

### 6.1 Map format and parsing

| Feature | KN status | KN owner file(s) | WP |
|---|---|---|---|
| Prefab ids 0-59 by number | supported | `shared/src/map.c:33-131`, `shared/include/shared.h:89-148` | A |
| Renames 33 CAMERA, 55 AI_SPAWNER (keep the v3 names as aliases) | partial (v3 names) | same | A |
| Prefab ids 60-96 | **wrong**: mapped to a solid CUBE (`map.c:111-131`). Affects Krunk_Plaza, Slide_Neon_Stage, zombie_high, SniperRoyale2, Bhop_VIP, eterno_jump, slide_moonlight | `map.c`, `shared.h` enum | A |
| Unknown ids ≥97: reject in validation, skip in generation | missing | `map.c`, `shared/src/map_catalog.c` | A |
| TERMINAL is a no-op (v7) | **wrong**: KN builds terminals | `map.c:640-691`, `shared/src/interact.c`, `client/src/ui/ui_terminal.c` (retire it) | A (parse), C (interact/UI removal) |
| Object creation order identical on both sides (uids) | supported (single parse) | `map.c` | A |
| `p` / `s` / `si`+`xyz` / `c` / `e` / `ci` / `ei` / `colors` / `f` / `r` / `o` / `v` / `ab` / `nAmb` / `nf` / `l` / `col` / `bo` / `wj` / `h` / `hp` / `cdy` / `sd` / `rt` / `rr` / `in` / `tin` / `tm` / `ts` / `td` / `tox` / `toy` / `tsr` | supported (v3 semantics) | `map.c:318-650`, `map_catalog.c:240-270` | A |
| Legacy shims `tr`→`tro`, `hp` / `wid` / `id` aliases, `at` video, legacy trigger → `tas[0]` | partial (no `tro` / `tas`) | `map.c` | A |
| New generic keys `tv at tro tsm ten fct fs rs alt ft da msp ns nai gp ip pl cpx smm sat rsc inm spa hbh hbw cst rcv as ast fcr sci` | missing | `map.c` (Object fields in `shared.h:174-249`) | A |
| `pe` as a 0..1 float | partial (boolean) | `map.c:440`, `shared/src/player.c` hitscan, `projectile.c` | A (parse), B (damage) |
| BOOST_PAD `bm:0` and `d` 0..5; `Object.direction` is a 2-bit field | **wrong** (direction bitfield too narrow, `shared.h:230`) | `shared.h`, `map.c:470-488` | A |
| Spawns `[6]` teamOnly, `[7]` spread | missing | `shared.h:251-256`, `map.c:350-418` | A |
| `camPos[3..6]` | missing | `map.c:333-344`, `client/src/menu_view.c:141` | A, G |
| Top-level `tris`, `gui`, `scripts`, `config`, `botPaths`, `ocean*`, `zone*`, `terrain*`, `toneMapping*`, `outputEncoding`, `correctLights`, `skyDomeTexA`, `skyDomeEmisTex`, `ambIndC`, `mmId`, `oldAO`, `hasLM` | missing (`scripts` is the KrunkScript spec's) | `map.c` (Map struct) | A |
| v3 `game` ignored at runtime | supported in effect (`map.c:337` always uses defaults). `MapConfig` should be removed in favour of `cls` | `map.c`, `shared.h:270-286`, `player.c` | E |
| v7 `validateMap` limits (§1.5.2), paren replacement, id / texture checks | partial (v3 sanitize) | `map_catalog.c`, `server/src/accounts.c:1335` | A |
| Official catalogue = v7's 32 maps and indices | **wrong** (15 v3 maps, v3 indices) | `map_catalog.c:24-39`, `map_catalog.h:39`, `assets/maps/` | A (step A6) |
| Editor data tables (97 prefabs, 20 textures, v7 trigger lists, cls / prks / templates) | v3-generated | `tools/editor/gen_editor_data.py`, `dump_*.js`, `client/src/editor/editor_data.c`, `editor_prefab_flags.h` | A (generator), G (editor UI) |
| Editor export / import of v7 keys (`tas`, `cls`, `prks`, `botPaths`, `tris` bake, `ocean`, `game`→`cls` import migration) | v3 format | `client/src/editor/editor_io.c` | G |

### 6.2 World runtime and collision (shared)

| Feature | KN status | KN owner | WP |
|---|---|---|---|
| Quad grid / linear scan | KN scans linearly (no parity impact) | `player.c` | B (optional) |
| Triangle (`tris`) capsule collision; `cpx` objects leave the AABB set | missing | new `shared/src/collision_tri.c`, `player.c`, `projectile.c` | B |
| Terrain runtime (generation, ground snap, raycasts, terrain objects) | missing (editor only: `client/src/editor/editor_terrain.c`) | new `shared/src/terrain.c` (moved from the editor), `player.c`, `projectile.c`, `game.c` canSee | B |
| BR zone (multi-stage, `zn`, 10 dmg/s, Infected exempt) | missing | new `shared/src/zone.c`, `game.c` tick | B |
| Jump pad v7 (size volume, `d`, `wasOnPad`, sound) | partial (vertical 5×5×1, `player.c:634-640`) | `player.c` | B |
| Wall-jump opt-out | partial (parse ok, `map.c:595-599`; gate in `player.c:700`) | `player.c` | B |
| `borderH` 500 | **wrong** (250) | `shared/src/constants.c:17` | B |
| Ladder grapple check; ROPE_LADDER | partial / missing | `player.c:678-698` | B |
| Score zone + `logTime` + `gt` | missing (`player.c:641` TODO) | `player.c`, `game.c` | B |
| Premium / verified / partner / kpd / kr / team zones (account-gated pass-through) | partial (TODO stubs, `player.c:645-647`) | `player.c` + account hook | B |
| NoPVP / PVP zones and the damage filter | missing | `player.c`, `projectile.c`, `game.c` (explosion, hitscan) | B |
| Death zone destructible / interface | missing | `map.c`, `triggers.c` | A, C |
| deathY skips godMode (v7), not noclip | partial (KN skips noclip) | `game.c:640-645` | B |
| Teleporter `tcp`; `chgp` broadcast | partial | `triggers.c:561-572` → move to `shared/src/world.c` | B |
| Checkpoint data, `chp` reset, parkour reset input `[13]`, `[14]` interact-hold | partial / missing | `triggers.c:547-585`, `shared.h` Input (`:357-396`) | B |
| Destructible `rsc` spawn chance; onRespawn without destroyedBy | missing / wrong | `triggers.c:210-233, 587-617` | C |
| Object node moves `mo` / `mf` (+ `sync.mov`), PATH_NODE table | missing | new world module, `map.c` | A (nodes), B (moves) |
| Object animation `ana` | missing | B (server packet), G (client clip playback) | B, G |
| Builds (`bo`, `dyno`, Build Tool) | missing | new `shared/src/build.c` | B |
| Spawn selection v7 (`teamOnly`, `spread`, `flippedSpawns`, dom weighting, compMode) | partial (KN variant, `mode_round.c:355-453`) | `shared/src/modes/mode_round.c` | D |
| Headclip fix, `hitBoxPad`, fall damage, etc. (config-driven physics) | missing | `player.c` | E (config), B (physics) |
| Movement schemes `movSch`, class physics `cls` | missing | `player.c`, `shared/src/classes.c` | E (data), with the player-physics owner |
| Projectiles: noPVP, terrain, `noObjC` | partial | `shared/src/projectile.c` | B |
| canSee v7 (ramp slab, complexColl skip, terrain) | partial | `game.c` | B |

### 6.3 Triggers and interaction

| Feature | KN status | KN owner | WP |
|---|---|---|---|
| Data model: `tas[]`, `cd` / `cv*`, `tad`, `tce` type / `tcc`, `tc2`, `sup`, `tac` / `tam`, `inm`, `hbh` / `hbw`, `pl` | missing (single action, `map.c:194-226`, `triggers.h:17-130`) | `shared/include/triggers.h`, `shared/src/triggers.c` (split into `triggers_actions.c`, `triggers_conditions.c`) | C |
| Events 0-2, 4, 7-9, 11-15 | supported | `triggers.c:239-279`, `ai.c:397` | C |
| Event 3 onEnterObject (multi-object `collidingObjs`), 23 onExitObject, `saveColls` | partial / missing (`player.c:626-631`) | `player.c` (B calls into C) | C (+B hook) |
| Event 5 onRespawn without destroyedBy; 6 onTimer `(tce‖1)` | wrong | `triggers.c:229, 603` | C |
| Event 10 onHoldMouse | supported (as onInteract) | `player.c:610-624` | C |
| Events 16-31 (interact key, player spawn / kill / die / damage, AI damage / half / kill-player, nuke start / end, custom value ×3, game start, objective switch) | missing (`te≥16` → NONE, `map.c:200`) | `triggers.c`, hooks in `game.c`, `ai.c`, `modes/*` | C (+D, F hooks) |
| Conditions 0-11 | missing | `triggers_conditions.c` | C |
| Actions 0-31 with v7 changes (2 / 10 checkpoint, 6 canDest, 11 / 12 / 21 reset rules, 14 global sound, 19 teams 1..5) | partial | `triggers.c:337-505` | C |
| Actions 32-71 | missing (32, 49, 57, 59, 60, 62, 65, 67 are backend → no-op unless kn_brain supports them) | `triggers_actions.c` | C |
| Value types (`getActionValue`), player `custVals` (≤30), `globalVals`, `cv` / `gv` / `gbl` | missing | `triggers.c`, `shared.h` Player / Game | C |
| Delays (`triggerQueue`), chain limit 30, single-use per player, chance, costs / methods 0-8, KR `cnfm` | missing | `triggers.c`, `interact.c` | C |
| Emitter sound table (73, index 16 = `scare_0`), rate min 0.1, `ef` 2 global | partial | `shared/src/triggers.c:300`, sound table in `shared/src/constants.c` / `client/src/sound_events.c` | C, G |
| Interact protocol `int type,uid` / `intSec` / `cnfm`; prompt text with `inm` and cost tags | partial (v3 scan-all, `interact.c:79-107`, `NET_PACKET_INTERACT` 61) | `shared/src/interact.c`, `client/src/ui/hud_ui.c` | C |
| Gate / pickup methods 0-7 (account gates via an account hook) | partial (0-2) | `interact.c:37-52` | C |
| Weapon pickups (incl. `rw` / `rwg`, ammo, `inat` v7, `drpw`) | missing (`interact.c:6`) | `interact.c`, `game.c` | C |
| Custom GUI (`gui` + `tgui`) | missing | `client/src/ui/hud_ui.c` (render), C (packets) | C, G |
| KrunkScript hook (`onCustomTrigger` etc.) | missing | `triggers.h` interface (§3.10) | C (interface only) |
| Terminal UI (`ot` / `tmc`) | KN-only, removed in v7 | delete `client/src/ui/ui_terminal.c` usage | C |

### 6.4 Modes, config, lobby

| Feature | KN status | KN owner | WP |
|---|---|---|---|
| Mode table v7 (13 clas, 14 depo, drop ranked, 24-36) | **wrong** (`shared/include/modes.h:14-40`, `modes.c:98-186`) | `modes.h`, `shared/src/modes/modes.c` | D |
| Rotation `[0,2,3,19,25]` | wrong | `modes.c` `g_rotation_modes` | D |
| Remove generic objective scoring; per-mode updates (point, king v7, depo, depoffa) | wrong (`mode_objectives.c:123-145`) | `mode_objectives.c`, `mode_rules.c` | D |
| Objectives by uid (`obi`), `randObjective`, `objtvTime`, `obj` v7, `uobj`, onObjectiveSwitch | partial | `mode_actions.c:80-98`, `mode_objectives.c:44-145`, `mode_net.c` | D |
| CTF flag idle 45 s; neutral flags (`tm:-1`); blitz rush; oneflag (tdf / car) | missing | `mode_objectives.c` | D |
| Domination (DOMINATION_ZONE, `uct` / `cct`, HUD) | missing | new `shared/src/modes/mode_dom.c`, `mode_net.c`, `client/src/ui/hud_modes.c`, `client/src/mode_world.c` | D, G |
| sdm (4 teams, `MODE_TEAMS ≥ 6`, names / damage 3-5) | missing | `modes.h`, `mode_actions.c`, `shared.h` GameConfig | D |
| Deposit / Deposit FFA (KR Tag spawnable clusters, `krt`) | missing | `mode_objectives.c` | D |
| krank, chs, bhffa, imp, clas, aon, md (modifier modes) | missing | `modes.c` + mode-key hooks in `player.c` / `projectile.c` / `game.c` | D (+B for physics hooks) |
| Gun Game v7 order (needs weapons 18 / 21 / 22 / 27); shrp effects; raid hostable; kc `tag_0` | partial / wrong | `modes.c`, `shared/src/weapons/` (weapons are owned by the weapons spec) | D |
| Spawnables table (25 types, `crsp` with team / data) | partial (Tag only, `mode_objectives.c:285-360`) | `mode_objectives.c` → `shared/src/spawnables.c` | D |
| Rounds / intermission / best-of / overtime / `rds` / `lkt` / `tx` | missing | `mode_round.c` | D |
| `viewDis` relevance culling | missing | server send loop, `game.c` | E |
| GameConfig v7 (85 keys, v7 clamps, comp `defC`) | partial (about 37 / 85; v3 clamps) | `shared.h:778-818`, `server/src/config.c:272-345`, `client/src/ui/ui_windows.c` | E |
| `weps` / `cls` / `prks` overrides at runtime | missing (`game_configure` gets NULL, `game.c:58`) | `shared/src/game.c` (new `game_apply_config`), `shared/src/weapons/`, `classes.c` | E |
| Map `config` block applied when hosting / offline (server path, §1.9) | missing | `game.c`, `map_catalog.c`, `server/src/accounts.c:1350-1420`, `server/src/sandbox.c` / `client/src/net/offline.c` (offline host path) | E |
| hostTypes (comp / zombies / raid presets) | missing | `server/src/config.c`, `ui_windows.c` | E |
| Match votes (`mts`, `maVote`, `mv`, `genMatchList`) and the single shared next-match flow | missing (random, duplicated) | `game.c:676-692`, `server/src/net.c:130-134` (the sandbox reuses it) | E |
| `networkData` rates | missing (fixed) | `shared.h:1171-1177` | E |
| Effects system (10 effects, `pk`, `prks`) | missing | new `shared/src/effects.c` | H |
| Streaks 1-6 (+ nuke trigger events) | partial (nuke only, `game.c:587-606`) | new `shared/src/streaks.c` | H |
| Airdrops | missing | new `shared/src/airdrop.c` | H |
| Zombies (zom mode, machines, power-ups, perks, `SERVER_NODE` tags, finiteAmmo) | missing | new `shared/src/modes/mode_zombies.c`, `interact.c` | H |
| gameTemplates in the editor | missing | `client/src/editor/` | G |

### 6.5 AI and bots

| Feature | KN status | KN owner | WP |
|---|---|---|---|
| AI_SPAWNER v7 keys (§2.8), limit 40 | partial (v3 BOT subset, limit 20, `shared/include/ai.h`) | `shared/src/ai.c` | F |
| Paths (PATH_NODE `phi` / `pht`), roam, redirection volumes, teams / targeting, `pmv` physics, stuck flags, effects `se` / `he`, boss behaviours 1-5 | missing (bosses 1-2 partly) | `ai.c` | F |
| AI trigger lists `tas` and onAI* events 20 / 29 / 31 | missing | `ai.c` → C's API | F |
| AI sync stride 9, range 800, `rai`, `ana` | wrong (v3 stride 8 / 600) | `ai.c`, `mode_net.c` / new packets | F |
| Rewards (`rew*`) | n/a (public games only; KN no-op) | — | — |
| BOT_SPAWNER → TFUE bots | missing | `shared/src/bots.c`, `map.c` | F |

### 6.6 Client rendering and audio

| Feature | KN status | KN owner | WP |
|---|---|---|---|
| Texture table 20 + variants `tv` + model variants `diffM` / `texRange` | partial (17, no variants, `client/src/prefab.c:93-110`) | `client/src/prefab.c` | G |
| Asset textures `at` (incl. video → static fallback), asset model flags | missing (3 user assets) | `prefab.c`, `client/src/loaders.c` | G |
| Material keys `tro tsm ten fct fs rs alt ft da msp` | missing | `prefab.c`, `client/src/materials/basic.c`, `assets/shaders/basic.*` | G |
| New model props (60-62, 73, 74, 81, 83-88, 90) + copy the missing models from v7 `public/models` (list in Appendix B) | missing | `prefab.c`, `assets/models/` | G |
| PLANE / BILLBOARD / LIQUID / SIGN / RUNE v7 rotation and UV scale; no auto face cull | wrong (v3) | `prefab.c:1599-1667` | G |
| SIGN outline `so` / `sot` / `nsf`, font 1..240 | missing | `prefab.c` | G |
| LIGHT_CONE `sds` (12) / `thl`; CYLINDER `sds rtp rbt ths thl opn`; RAMP `ec` | partial | `prefab.c:1683-1690` | G |
| Spot / directional / rect-area lights, LIGHT_BAR, SPRITE, SPRAY, LEADERBOARD canvas | missing | `client/src/map_post.c`, `prefab.c`, `basic.frag` | G |
| ENVIRONMENT_ZONE swapping `RenderLighting`; tone mapping, `outputEncoding`, `correctLights` | missing | `client/src/main.c:131-213`, `sky.c`, `shadow.c`, new `client/src/environment.c` | G |
| Sky dome `skyDomeTexA` / `skyDomeEmisTex`; ocean | missing | `client/src/sky.c`, new `client/src/ocean.c` | G |
| AO gate (`oldAO` / official / `hasLM`), asset lightmaps | partial | `client/src/ambient.c`, `map_post.c:218-225` | G |
| Terrain mesh, zone cylinder mesh | missing (editor only) | `client/src/` (reuse the editor meshes) | G |
| Soundscapes v7 table / volumes, `ambIndC`, `camb`, `sso`, step sounds `sat`, emitter `edm` | partial | `client/src/sound_events.c:96-215` | G |
| Minimap (`mmId`, MAP_ANCHOR bounds, `smm`) | missing (no KN minimap found) | `client/src/ui/hud_ui.c` | G |
| Spectate CAMERA objects (`camt`, `obl`, `fp`) | missing (`client/src/spectate.c:8`) | `spectate.c` | G |
| HUD: dom zones, speed-run timer, deposit counter, round wins, custom GUI, interact prompts with `inm` / cost | missing | `client/src/ui/hud_modes.c`, `hud_ui.c` | G (with D / C packets) |

## 7. Implementation plan

The work is split into **8 work packages** (A-H):
- **A** is the foundation and lands first.
- **B-G** then run in parallel.
- **H** is staged: its effects/streaks part is parallel, its zombies/airdrops part comes last.

Every package must keep **all existing `assets/maps/*.json` and `assets/maps/custom/*.json` loading** under v7 semantics (§1.10) and must pass `make` and `make test`. Packages that touch the HTML UI also run `make smoke-native`.

### 7.0 Shared rules for every work package

- **Authority** lives in `shared/` (or in server code linked into the offline sandbox). The client only mirrors and renders (§0, §5.1). New messages use the packet block assigned to the package (§5.3).
- **`shared/include/shared.h`** is edited append-only. Each package adds its fields and packets in a section commented with its WP letter, so merges stay mechanical. Only WP-A restructures existing structs.
- **Cross-package calls** go through the headers WP-A creates with stub bodies (§7.A step A4). The owning package replaces the stub; nobody else edits the body.
- **Tests:**
  - New unit tests go in `shared/tests/<name>_test.c`, registered in `CMakeLists.txt` next to the existing `*_test` targets (lines 317-382) and added to `make test`.
  - New fixtures go in `shared/tests/maps/` (hand-written, small).
  - Do not edit the original content under `assets/maps/`. The exception is step A6's catalogue import, which adds files.
  - Renderer changes need headless screenshots (AGENTS.md "Headless runs"), checking at least Sandstorm, Kanji and the package's fixture maps.
- **Interpreting v7:** the v7 behaviour described in this document is the target. When code and document disagree, read the cited v7 line, then fix the code, or fix the document in the same change.

### WP-A: Map schema, prefab table, catalogue, cross-package interfaces (foundation)

**Owns:**
- `shared/include/shared.h` (Object, Map, Spawn structs; prefab enum)
- `shared/src/map.c`
- `shared/src/map_catalog.c`, `shared/include/map_catalog.h`
- `tools/editor/` (generator), regenerated `client/src/editor/editor_data.c` and `editor_prefab_flags.h`
- new headers with stubs: `shared/include/world.h`, `effects.h`, `spawnables.h`, `account.h`, plus the v7 additions to `triggers.h`

**Depends on:** nothing.

**Steps:**

1. **Prefab enum and name table, 0-96.**
   - 33 = CAMERA, 55 = AI_SPAWNER; the old names stay as string aliases.
   - `Object.direction` widens to hold 0..5.
   - Unknown ids are rejected by validation and skipped by generation.
   - TERMINAL builds nothing.
   - EVENT is skipped on custom maps.
2. **Parse every v7 key** in §1.1-§1.5 into `Map`/`Object` fields.
   - Apply the v7 defaults and clamps.
   - Apply the shims: `tr`→`tro`, aliases, `at` video, legacy trigger → `tas[0]` including the `tce` quirk (§3.4), `pe` float, `bm` v7 semantics.
   - Store `tas[]` (≤ any length at runtime; validation caps it at 5).
   - Store the AI and bot key bags as raw cJSON references for WP-F.
   - Store the `config`, `gui`, `scripts`, `tris`, `botPaths`, terrain, zone, ocean and environment fields.
   - Store PATH_NODEs in `Map.path_nodes[nid]`, plus SERVER_NODE, MAP_ANCHOR and ENVIRONMENT_ZONE lists.
3. **Port `validateMap`** (`customGameConfig.js:152-368`) with the v7 limits (§1.5.2), into `map_catalog.c` (`map_sanitize_json`). Add the `prepareGame` paren replacement and id/texture checks to the custom-map ingest path.
4. **Interfaces with stubs.** Each owning WP fills in its body.

   | Header | Declarations | Filled by |
   |---|---|---|
   | `world.h` | `world_teleport_object_to_node`, `world_reset_object_position`, `world_play_anim`, `zone_*`, `terrain_raycast` | B |
   | `effects.h` | `effect_give(game, player, idx, stacks, random)` | H |
   | `spawnables.h` | `spawnable_drop(game, type, owner, data, pos)` | D |
   | `triggers.h` | v7 dispatch API: `trigger_object_event`, `trigger_player_event`, `trigger_multi_event`, the §3.10 script hooks | C |
   | `account.h` | `GameHooks.account_info(game, player, AccountInfo*)` (level, premium, verified/featured, partner, canFlag, invVal, KR), NULL = guest | server implements; offline stays NULL |

   Also add the v7 `update()` order (§2.0) to `game.c` as calls into these stubs.
5. **Regenerate the editor tables** from the v7 sources. Point `gen_editor_data.py` at `/home/user/jails/kru/src/Krunker_v7.1.4/src` (readable; no source-map extraction needed) to get 97 prefabs, 20 textures with variants, v7 trigger events/conditions/actions, `cls`/`prks` config lists, and gameTemplates.
6. **Official catalogue → v7's 32 maps** (§4.6).
   - Import the v7 official JSON files from `/home/user/jails/kru/src/Krunker_v7.1.4/public/maps/` with a tool step (like `make assets-import`).
   - Renumber `map_catalog.c`.
   - Keep the v3 maps as `old_*` customs.
   - This step is independent and can be scheduled last within A.

**Tests:**
- New `shared/tests/map_v7_test.c`. It loads every map in `assets/maps/`, `assets/maps/custom/` and `shared/tests/maps/` and asserts:
  - no id ≥ 60 becomes a CUBE;
  - object counts and uid order match a reference dump. Generate the reference with a node script over v7 `map.js` if feasible; otherwise use hand-checked counts per prefab;
  - the shims produce the expected values;
  - validation rejects over-limit fixtures.
- Fixtures:
  - `v7_all_prefabs.json`: one object of each id 0-96, with typical keys.
  - `v7_legacy_v3.json`: v3-only constructs, i.e. the `game` block, SPECTATE_CAM with `obi`, TERMINAL, BOT, `tr`, string `t`, 6-element spawns, 3-element `camPos`, a single-action trigger with `tce`.
  - `v7_limits_*.json`: limit violations.
- Existing tests: `map_catalog_test`, `proc_input_test`, `spawn_test`.

**Done when:**
- Krunk_Plaza, Slide_Neon_Stage, zombie_high, SniperRoyale2, Bhop_VIP, eterno_jump and slide_moonlight load with no stray cubes, and the zones/anchors/lights are non-solid.
- All existing tests pass.

### WP-B: World runtime and collision (shared)

**Owns:**
- `shared/src/player.c` (map-object collision loop, §1.6 order)
- new `shared/src/collision_tri.c` (port of `collisions.js`)
- new `shared/src/terrain.c` (pure generator moved from `client/src/editor/editor_terrain.c`; the editor then calls the shared one)
- new `shared/src/zone.c`
- new `shared/src/world.c` (checkpoints, teleporters, node moves, animations, destructible ticking moved out of `triggers.c`, builds)
- new `shared/src/build.c`
- `shared/src/constants.c` (`borderH` 500, `quadSize`)
- the damage filters in `shared/src/projectile.c` and `game.c` (explosion, hitscan, canSee)
- input frame fields `[13]` and `[14]` in `shared.h`
- packet block 130-139

**Depends on:** A.

**Steps:**
1. `tris` capsule collision; `cpx` objects leave the AABB set and the bullet set.
2. v7 collision order. Includes:
   - pads (size and `d`, `wasOnPad` rules);
   - wall-jump opt-out;
   - borders at 500;
   - ladders (grapple check) and rope ladders;
   - qualifying zones through `account_info`;
   - NoPVP/PVP flags and the damage filter;
   - pass-through kinds;
   - the `mode.collisionCheck` hook (calls into D);
   - enter/exit/hold/collision callbacks (call into C; B maintains the per-player `collidingObjs` set and `saveColls`).
3. Score zones with `logTime`/`gt`. Checkpoints with `checkPointData`, `chp` reset, parkour reset input `[13]`, and interact-hold `[14]`. Teleporter `tcp` and `chgp`.
4. Terrain runtime: player ground snap, plus hitscan, projectile and canSee raycasts. Zone runtime: shrink, stages, `zn`, damage; `sync.zone`.
5. Node moves `mo`/`mf` with `sync.mov`, object animation `ana` (server side), and builds `bo`/`dyno`.
6. deathY skips godMode instead of noclip. `pe` float wallbang.

**Tests:**
- New `shared/tests/world_v7_test.c`, driven by scripted inputs like `proc_input_test`. Cases:
  - pad directions 0-5;
  - rope ladder climb from all sides;
  - wall-jump on a box without `wj` and refusal with `wj`;
  - border height;
  - triangle ramp walk and wall push-out;
  - terrain ground snap on ClashRoyale;
  - zone stage timing and damage;
  - teleporter `tcp`;
  - parkour reset;
  - noPVP damage filter;
  - score-zone `gt`.
- Fixtures:
  - `v7_movement.json` (pads, ropes, `wj`, borders, a `tris` ramp);
  - `v7_zones.json` (every zone kind, a destructible death zone, score zones);
  - `v7_br_zone.json` (stages 2, wait 5);
  - `assets/maps/custom/ClashRoyale.json` (terrain);
  - after A6: v7 `krunk_plaza_v2.json` and `shipment.json` (real `tris`).
- Must still pass `proc_input_test` (Sandstorm/Kanji movement). Any expected diffs must come only from the v7 rule changes listed in §1.10 and be documented in the test.

### WP-C: Triggers v7 and the interaction protocol

**Owns:**
- `shared/include/triggers.h`
- `shared/src/triggers.c`, split into `triggers.c` (dispatch, onTrigger), `triggers_conditions.c` and `triggers_actions.c`
- `shared/src/interact.c` (`int type,uid`, `intSec`, `cnfm`, gate/bank/pickup methods 0-7, weapon pickups, prompts)
- `client/src/map_triggers.c`
- removal of `client/src/ui/ui_terminal.c` and its packets
- packet block 120-129

**Depends on:** A. It calls into B (`world_*`), D (`spawnable_drop`, `mode_on_trigger`), F (AI spawn, anim, force target) and H (`effect_give`) through the A stubs.

**Steps:**
1. Implement §3 in full:
   - data model, events 0-31 with their dispatch points (hooks into `game.c`, `ai.c` and the modes at the cited v7 places; event 17 onPlayerSpawn dispatches at spawn per the **Decision** in §3.5);
   - conditions 0-11 (6-11 through `account_info`);
   - actions 0-71 (backend actions are no-ops unless kn_brain exposes KR/storage; record that in code);
   - value types, `custVals` (limit 30), `globalVals`, delays queue, chain limit 30, `sup`, chance, costs and methods with `cnfm`;
   - per-round reset (§3.8).
2. Interaction:
   - client sends `{type, uid}`;
   - the server validates `tRadius`, `teamCheck` and the 100 ms rate limit;
   - gates/banks/pickups v7 (incl. `rw`/`rwg`, ammo carry, `inat` v7, `drpw`);
   - onInteractKey with `inm` and cost tags.
3. Client: mirror `cv`/`gv`/`gbl`/`tgui`/`camb`/`sso`/`asnd`/`rsnd`/`chgp`. Prompt text goes to `hud_ui.c` (coordinate with G for rendering the custom `gui`).
4. KrunkScript interface (§3.10): the hook struct and value accessors. NULL hooks are no-ops.

**Tests:**
- New `shared/tests/triggers_test.c`, using the `modes_test`-style packet capture. Covers:
  - every event's dispatch;
  - each condition;
  - each non-backend action;
  - the `tce` legacy quirk;
  - onTimer `(tce‖1)`;
  - delays (condition not re-checked);
  - chain limit;
  - `sup`;
  - `tam` 1-8 gating;
  - interface targeting rules (`d.interface == j.interfaceT`, self excluded).
- Fixtures:
  - `v7_triggers_basic.json` (enter/exit + global value + `gui` text);
  - `v7_triggers_values.json` (value types 1/2/3, custom-value events 24/25/30, storage events);
  - `v7_triggers_nodes.json` (actions 40-42, PATH_NODE);
  - `v7_legacy_trigger.json`;
  - the "Triggers & Scripts" template (`scriptTemplates.js:58`).
- Real maps:
  - `zombie_high.json` (284 triggers, te 2-26, ta 0-67, conditions, `tad`/`sup`/`tam`/`inm`);
  - `Bhop_VIP.json` (`tac` ×50);
  - `Krunk_Plaza.json` (`inm`, ta 40/47/58/64);
  - `Verb_Arena.json` (nuke events);
  - `eterno_jump.json` and `slide_moonlight.json` (value types);
  - `ESP_Parkour.json` (te 16 + `tac`);
  - `soul_sanctum.json` (v3 baseline regression).
- Pass criterion: no trigger in these maps hits an unimplemented path. Log and count them in the test.

### WP-D: Modes v7, objectives, spawnables, rounds, spawn selection

**Owns:**
- `shared/include/modes.h`
- `shared/src/modes/*`, plus new `mode_dom.c` and `mode_deposit.c`
- new `shared/src/spawnables.c`
- `mode_round.c` (rounds, spawn selection)
- packet block 140-149
- the mode packet handlers in `client/src/net/game_events.c`, together with G for the HUD

**Depends on:** A. Uses H's `effect_give` (shrp, aon) through the stub.

**Steps:**
1. Re-index the mode table to v7 (§4.1).
   - Remove ranked and its rules.
   - Add 24-36, except zom (H).
   - Rotation `[0,2,3,19,25]`.
   - Update every mode-id consumer: `chat_commands.c`, console, host window lists, `match_test.c`.
2. Replace the generic objective tick with per-mode `update`s:
   - point, with comp `notWinnable`;
   - king (v7 rules);
   - depo / depoffa.
3. Objectives by uid with `randObjective`, `objtvTime`, `obj`/`uobj`, and the onObjectiveSwitch hook into C.
4. Flags: idle return after 45 s, neutral `tm:-1`, blitz rush, oneflag for tdf/car with the carrier multipliers.
5. Domination (§4.1a) with `uct`/`cct` and `sync.doms`.
6. sdm: `MODE_TEAMS` grows to 6 (team 0 + 5), smallest-team assignment, team names and damage 3-5.
7. Modifier modes: clas, krank, chs, bhffa, imp, aon, md. Add mode keys (`gravMlt`, `jumpMlt`, `groundAccel`, `reloadMlt`, `explMlt`, `explSMlt`, `maxHealth`, `noKnives`, `noMelee`, `impulseMlt`, `nonObjImpulse`, `killMlt`, `bonuses`, `headMlt`, …) to the `ModeConfig` table. Read them in the B-owned files through small accessors; coordinate edits to `player.c` with B.
8. Spawnables table (§4.1c) with `crsp` v7.
9. Rounds, intermission, best-of, overtime (`rds`, `lkt`, `tx`) and `flippedSpawns`. Spawn selection v7 (§2.5).

**Tests:**
- Extend `shared/tests/modes_test.c` (and `match_test.c`) to the v7 table. Cover:
  - the per-mode win conditions;
  - king single-occupant scoring;
  - dom capture timing incl. the multipliers;
  - depo clusters and deposits;
  - neutral flag;
  - rounds with best-of.
- Fixtures:
  - `v7_dom.json`: 3 DOMINATION_ZONEs, team spawns 1-4 (dom + sdm);
  - `v7_objectives.json`: OBJECTIVE with `obi` 1-3, neutral FLAG, team FLAGs;
  - gameTemplates 14 (Defuse) and 28 (Brawl);
  - after A6: v7 `atomic.json` and `lagoon.json` (DOMINATION_ZONE).
- `spawn_test` gets cases for `teamOnly`, `spread` and `flippedSpawns`.

### WP-E: Custom game config, map `config`, host types, lobby flow and votes

**Owns:**
- `GameConfig` in `shared.h`
- new `shared/src/game_config.c` (`applyConfig`/`setupGameConfig` port)
- `server/src/config.c`
- `client/src/ui/ui_windows.c` (Host Game)
- `shared/src/game.c` (`game_configure`, the shared next-match flow)
- `server/src/net.c` (next round; the offline sandbox `server/src/sandbox.c` runs the same code)
- `server/src/accounts.c` (host path)
- packet block 180-189
- `weps`/`cls`/`prks` override plumbing into `shared/src/weapons/` and `shared/src/classes.c`

**Depends on:** A. The class-physics semantics of `cls` (movSch, vehicle, camera) belong to the player-physics owner; E delivers the data and override plumbing.

**Steps:**
1. GameConfig with all 85 `serverConfig` keys: v7 defaults, `defC`, clamps per host type, removed keys.
2. `weps`/`cls`/`prks` overrides clamped to the v7 config tables. `pj_*` builds a custom projectile.
3. Map `config` block applied via the server path when hosting a map and in the offline sandbox (§1.9 Decision). Retire `MapConfig`.
4. hostTypes 0-4, with `overDat`/`forceDat`/`allowedDat` and `preventCustomGames` filtering.
5. End-of-game flow in shared code:
   - `mts` generation (`genMatchList` with per-map `config.modes` buckets);
   - `maVote` / `mv` weighted tally;
   - `endTimer`;
   - `init`.
   - Includes `networkData` rates and `viewDis` culling.

**Tests:**
- New `shared/tests/config_test.c`. Cover:
  - clamps per host type;
  - `defC` for comp;
  - `setupGameConfig` precedence;
  - weapon/class/effect overrides;
  - the map `config` application for `custom/Krunk_Plaza.json` (classes, cls movSch 6/7, prks, weps, settings, modes [18]) and `custom/eterno_jump.json`;
  - gameTemplates 6/7/16/28 dumped as fixtures.
- `match_test.c`: votes and `mts` (forceMap/forceMode, ties go to the first entry, no votes → random from `mts`).

### WP-F: Map AI v7 and map bots

**Owns:**
- `shared/src/ai.c`, `shared/include/ai.h`
- the BOT_SPAWNER hook in `shared/src/bots.c`
- `client/src/ai_render.c` (sync v7 and `ana` clips)
- packet block 160-169

**Depends on:** A (key bags, path nodes, redirection objects). Uses C's trigger API (onAI* events and `tas`), B's collision helpers, and H's `effect_give`.

**Steps:**
1. Data: the full `buildAIData` key set and clamps; limit 40; `rsc`/`sd`.
2. Runtime (§2.8): targeting incl. teams and AIs, paths and path types, roam, `pmv` physics, map collision with skips, redirection volumes, stuck flags, projectiles/melee/shooting v7, effects `se`/`he`, boss behaviours 1-5, deathY, onAI* events and `tas`.
3. Sync: stride 9 within range 800, `aai` v7, `rai`, `ana`.
4. BOT_SPAWNER feeding TFUE bots with `bt*` data. `botLimit` 16.

**Tests:**
- Extend `shared/tests/ai_test.c` and `bot_test.c`. Cover:
  - path following for each path type;
  - roam radius;
  - redirection volume push;
  - team targeting;
  - the onAIDeath/onAIDamage/onAIHalfHealth/onAIKillPlayer triggers firing their `tas`.
- Fixtures:
  - `v7_ai_paths.json`: AI_SPAWNER with `phi`/`pht`, PATH_NODE ×4, AI_REDIRECTION, two AI teams, BOT_SPAWNER ×2;
  - `soul_sanctum.json` (v3 BOT regression);
  - gameTemplates 5 and 27 (Raid).

### WP-G: Client rendering, audio, HUD, editor

**Owns:**
- `client/src/prefab.c`, `client/src/map_post.c`, `client/src/materials/basic.c`
- `assets/shaders/basic.*`
- `client/src/sky.c`, `client/src/shadow.c`, `client/src/ambient.c`, `client/src/sound_events.c`, `client/src/spectate.c`, `client/src/menu_view.c`
- `client/src/main.c` (RenderLighting)
- new `client/src/environment.c`, `ocean.c`, `terrain_mesh.c`, `zone_mesh.c`
- `client/src/ui/hud_ui.c` and `hud_modes.c` (rendering of custom `gui`, prompts, mode HUDs)
- `client/src/editor/*` (v7 import/export, templates)
- `assets/models/` additions

**Depends on:** A. Consumes B/C/D packets as they land; stub the HUD for packets that have not landed yet.

**Steps:**
1. Textures: 20 entries with variants, `at` assets (videos fall back to a static first frame or a placeholder, documented), model variants. Copy the missing v7 models (Appendix B).
2. Materials: `tro tsm ten fct fs rs alt ft da msp`; v7 plane/sign/liquid/rune/billboard transforms and UV scale; no automatic face cull.
3. New prefabs:
   - model props;
   - LIGHT_BAR, SPRITE, SPRAY, LEADERBOARD canvas, sign outline;
   - light cone and cylinder parameters, ramp `ec`;
   - spot, directional and rect-area lights in the forward pipeline;
   - keep the directional sunlight as the shadow source, per the AGENTS.md shadow rules.
4. Environment:
   - ENVIRONMENT_ZONE swaps `RenderLighting`, sky dome, fog and ambient sound by camera position;
   - tone mapping, `outputEncoding`, `correctLights`;
   - sky dome `TexA`/`EmisTex`;
   - ocean;
   - AO gate and asset lightmaps;
   - terrain and zone meshes.
5. Audio: v7 soundscape table and volumes, `ambIndC`, `camb`, `sso`, step sounds `sat`, emitter `edm`.
6. HUD:
   - custom `gui` elements (with values from `cv`/`gv`);
   - interact prompts with `inm` and cost tags;
   - dom/speed-run/deposit/rounds HUD;
   - minimap with `mmId` and anchors, if KN gets a minimap;
   - spectate CAMERA objects;
   - `camPos` static camera and rotation.
7. Editor: import/export of the v7 keys (`tas`, `cls`, `prks`, `botPaths`, `tris` bake from `cpx`, ocean/zone/terrain keys) and the `game`→`cls` import migration (§1.9). gameTemplates are listed in the new-map dialog.

**Tests:**
- Headless screenshots (`KRUNKNATIVE_HEADLESS=1 … KRUNKNATIVE_SCREENSHOT`) of:
  - Sandstorm and Kanji (regression);
  - `Krunk_Plaza.json` (env zones, variants, cylinders);
  - `eterno_jump.json` (variants, `tro`/`tsm`, `rs`);
  - `zombie_high.json` (LIGHT_BAR, leaderboards, `gui`);
  - `Slide_Neon_Stage.json` (env zones);
  - a new `v7_render.json` (one of each light, sprite, spray, sign outline, rotated planes with known v7 orientation, texture variants);
  - gameTemplates 1 (Ocean) and 7 (terrain + zone).
- Compare against the v7 web client where possible (`/home/user/jails/kru/src/Krunker_v7.1.4` server and viewer), or against archived screenshots.
- `make smoke-native` and `make uidiff` for HUD changes. Use the NVIDIA EGL device as well as llvmpipe (user GPU note).

### WP-H: Effects, streaks, airdrops, zombies

**Owns:**
- new `shared/src/effects.c`, `streaks.c`, `airdrop.c`
- new `shared/src/modes/mode_zombies.c`
- zombie machines in `shared/src/interact.c` (types 13/14/15), coordinated with C
- packet blocks 150-159 and 170-179
- the matching client HUD bits, coordinated with G

**Depends on:** A. Stage 2 depends on C, D and F.

**Stage 1** (parallel with B-G):
1. Effects (§2.10) with `pk` and `prks` overrides.
2. Streaks 0-6, with nuke trigger events 21/22 in custom games.
3. shrp/aon effect hooks consumed by D.

**Stage 2** (after C, D and F):
1. Zombies mode 35 (§4.1b): waves, AI templates through F, SERVER_NODE tags, mystery box / perk / krum machines, power-ups, perks, finiteAmmo / unrestrictedWeapons, action 71 portal.
2. Airdrops for public-server lobbies only (§4.4).

**Tests:**
- New `shared/tests/effects_test.c`: stacking, timers, `prks` override, effect multipliers applied to speed/reload/fire rate/damage/jump.
- New `shared/tests/zombies_test.c`: wave progression, power-up drop rates with a seeded RNG, mystery box states, perk cap.
- Fixtures:
  - `v7_zombies.json` (SERVER_NODE `zombie_spawn` ×4, MYSTERY_BOX, PERK_MACHINE, KRUM_MACHINE, GATE, DEPOSIT_BOX, WEAPON_PICKUP);
  - after A6: v7 `facility.json` (prefabs 81-94).

### 7.1 Dependency graph and order

```
A ──┬── B ──┐
    ├── C ──┤
    ├── D ──┼── H(stage 2: zombies, airdrops)
    ├── E   │
    ├── F ──┘
    ├── G   (consumes B/C/D packets as they land)
    └── H(stage 1: effects, streaks)
```

Suggested merge order after A: E, B, C, D, F, H1, G (continuous), H2.

Each package must merge with `make`, `make test` and its own tests green. It must not regress `proc_input_test`, `modes_test`, `ai_test`, `bot_test`, `spawn_test` or `map_catalog_test` beyond the v7 changes it documents.

### 7.2 Backward compatibility checklist (every WP)

- Every map under `assets/maps/` and `assets/maps/custom/` still loads, with no errors in `map_v7_test`.
- Old-map behaviour follows §1.10, and only §1.10. Any other difference is a bug.
- Old network clients are not supported: client and server ship together. Bump the protocol version once in WP-A.

## Appendix A: Test map coverage

| Map (`assets/maps/…`) | Exercises | WPs |
|---|---|---|
| `custom/Krunk_Plaza.json` | `config` block (classes, cls movSch 6/7 + camType, prks, weps, settings, modes [18]); `scripts`; 7-element `camPos`; ENVIRONMENT_ZONE ×3; PARTNER/KPD zones; PATH_NODE ×5; LIGHT_BAR ×9; SATELLITE_DISH; `tv` (151 objects); `t` 17; `sds/rtp/rbt/thl/ths`; `cpx` ×64 (no `tris`); `aid`, `at`, `pl`, `tro`, `tsm`, `ns/nai/gp/ip`, `smm`, `sat`, `ten`, `txs`, `cst/rcv`; triggers with `inm` ×24, ta 40/47/58/64 | A, B, C, E, G |
| `custom/eterno_jump.json` | `config` (modes [4], classes [9], cls `{9:{wallJ:false}}`, settings); `scripts`; `at` ×196, `tsm` ×1021, `tro` ×735, `tv` ×301, `t` 17, `rs` ×187; LEADERBOARD; value types (`tcc`/`tce`); ta 37/49/58 | A, C, E, G |
| `custom/zombie_high.json` | 284 triggers (te 2/3/4/5/7/12/13/16/17/18/19/22/26; ta 0-67; cd 1/2/5; `tad`, `sup`, `tam`, `cha`, `sfd`, `tc2`, `inm`, `tcc`); `gui`; LIGHT_BAR ×22; LEADERBOARD ×2 (`ldc/ldo/ldr`); `cpx`; `tcp`; `tris`; `hasLM` | A, C, G |
| `custom/Slide_Neon_Stage.json` | ENVIRONMENT_ZONE ×11; LEADERBOARD; `tcp` ×21; `tam`, `inm`; `tris` | A, B, C, G |
| `custom/Bhop_VIP.json` | `tac` ×50; cd 1; `tad`; ta 32/37/49; RAMP `ec`; `gp`; LEADERBOARD; bhop logTime | B, C, D |
| `custom/SniperRoyale2.json` | NOPVP_ZONE ×2; `scripts`; sky dome assets | B, G |
| `custom/Verb_Arena.json` | te 21/22 (nuke); `cha`; `tad`; `at`, `aid`, `fct/fs`; `scripts` | C, H, G |
| `custom/ESP_Parkour.json` | te 16 + `tac`; `at` ×93; `aid`; `fct/fs`; parkour | B, C, G |
| `custom/slide_moonlight.json` | value types; ta 37/49/58; LEADERBOARD; `scripts` | C |
| `custom/ClashRoyale.json` | terrain (the only terrain map) | B, G |
| `soul_sanctum.json` | v3 BOT (55) ×20; v3-format triggers (te 0-15, `ct`, `su`); `welMsg` | C, F (regression) |
| `sandstorm.json`, `kanji.json` | movement and render regression (AGENTS.md) | all |
| `burg.json`, `lostworld.json`, … | top-level `modes`; `pe` 1 (all shipped `pe` values are numeric 1) | A, E |

No shipped map contains TERMINAL (41) or EVENT (40) objects, or any v3 `game` block.

**v7 official maps worth importing as fixtures** (A6), from `/home/user/jails/kru/src/Krunker_v7.1.4/public/maps/`:

| Map | Exercises |
|---|---|
| `facility.json` | prefabs 64, 74, 81, 82, 83, 87, 89, 91-94 (zombies) |
| `krunk_plaza_v2.json` | `tris`, 6120 floats |
| `shipment.json` | `tris` + `botPaths` |
| `atomic.json`, `lagoon.json` | DOMINATION_ZONE, MAP_ANCHOR, `oldAO` |
| `laboratory.json`, `sanctum.json` | `gui` + `scripts` |
| `subzero_new.json` | — |

26 official v7 maps use MAP_ANCHOR (96).

**gameTemplates** (§4.2) are ready fixtures too. Dump them with node into `shared/tests/maps/template_<n>.json`.

**Not covered by any existing map** (new fixtures listed in §7):
- pads with `d`
- ROPE_LADDER, SPRITE, SPRAY
- spot / rect / directional lights
- PRIZE_WHEEL
- KR / PVP zones
- SERVER_NODE, MAP_ANCHOR, BOT_SPAWNER
- machines
- AI_REDIRECTION
- non-empty `tris` (before A6)
- model variants
- gate methods 3-7
- builds
- the BR zone
- the ocean

## Appendix B: Models present in v7 `public/models` but missing from KN `assets/models`

Copy them from `/home/user/jails/kru/src/Krunker_v7.1.4/public/models/`. Every v7 map texture and variant is already in KN `assets/textures`; the only textures still missing are non-map folders (badges, dyes, head, playercards, wrist, `marker_0*`).

| Group | Models |
|---|---|
| Barrels | acidbarrel_0_1, acidbarrel_0_2; barrel_0_1 … barrel_0_5; ebarrel_0_1, ebarrel_0_2 |
| AC units | ac_unit_0, acunit_0 |
| Pickups and boxes | adrp_0, ammobox_0, weaponbox_0, mysterybox_beam |
| Antennas | antenna_0, antenna_0_1 … antenna_0_3 |
| Build pieces | beam_0, build_0 … build_3 |
| Holiday props | cane_0, christmastree_0, gift_0 |
| Props | cactus, clouds_0, cone_0_1, dumpster_0, fence_0, sign_0, skull_0, tag_0, tiles_0 |
| Crates and doors | crate_0_2 … crate_0_4; door_0_1 … door_0_3 |
| Pickups and streaks | doublepoints_0, event_0, sale_0, streak_0, streak_3 |
| Machines | krummachine_0, perkmachine_0, upgrademachine_0 |
| Lamps | lamp_0, lamp_0_1, lamp_0_2 |
| Pads | pad_1 |
| Satellite dishes | satellite_dish_0, satellitedish_0 |
| Sheet metal | sheet_metal_0, sheetmetal_0 |
| Trees | tree_0_1 … tree_0_8 |
| Vehicles | vehicle_0_2, vehicle_0_3 |
| Water towers | water_tower_0, water_tower_0_1, watertower_0, watertower_0_1 |
| Windows | window_0_1 … window_0_6 |

## Appendix C: Decisions recorded in this document

| # | Decision | § |
|---|---|---|
| 1 | Adopt v7 everywhere. Old maps load exactly as v7 loads them (the §1.10 table is the whole compat surface) | 0, 1.10 |
| 2 | Event 17 onPlayerSpawn is dispatched to objects at the server spawn, filling a v7.1.4 reconstruction gap. Document it in code | 3.5 |
| 3 | The legacy trigger wrap keeps the `tce`→value-type quirk (no shipped map is affected) | 3.4 |
| 4 | Backend-only trigger actions (32, 49, 57, 59, 60, 62, 65, 67) and account conditions 6-11 are no-ops / false without kn_brain account data. KR-cost triggers count as unaffordable | 3.6-3.8 |
| 5 | The map `config` block is applied through the server host path, also offline; the v7 sandbox bug that drops `settings` is not reproduced | 1.9 |
| 6 | v3 `game` is ignored by the game runtime; the KN editor import migrates it to `cls`, as the v7 editor does | 1.9 |
| 7 | Mode indices adopt v7 and ranked is dropped (or moved to ids ≥37 if still wanted) | 4.1 |
| 8 | The official catalogue becomes v7's 32 maps (step A6); the v3 maps remain as `old_*` customs | 4.6 |
| 9 | New packets use binary `NET_PACKET_*` ids 120-199 in per-WP blocks; 200-239 are reserved for KrunkScript | 5.3 |
| 10 | One shared next-match/vote flow for the server and the offline sandbox | 5.4 |
| 11 | AO planes stay on for KN official maps (they count as `official`) | 1.8.6 |
| 12 | Render-only objects still take a uid on both sides, to keep the object order identical | 1.3 |
| 13 | Airdrops are implemented last and only for public-server lobbies | 4.4 |

## Appendix D: Known v7.1.4 quirks to reproduce (not fix)

- `getActionValue` returns undefined for a stored 0, so the action is skipped.
- KR-cost triggers run their actions without checking conditions.
- Delayed actions do not re-check their condition.
- Action 28 resets only Equals/Greater storage targets.
- Action select indices are applied directly to the runtime tables (§3.7).
- The depo team-2 tie-break sums kills.
- The `_normal3D` terrain bug: `sqrt(x²+y²+z+z)` (`Terrain.js:597`).
- Zone shrink moves along a normalised 3-vector, so the height dilutes the radial rate (also in v3).
- The interface-limit error message says 20 while the limit is 30.
- The `sci` serializer writes `this.sci`.
- Airdrop streak packages need `mode.disabledStreaks` to exist.
- AI `airAccel` has no consumer. The trade `maxNoMov` has none in the client either; KrunkNative's server uses it as the mode's in-game idle kick (doc/v10/trade.md).

Quirks that are **not** reproduced:
- the sandbox `settings` drop (Decision 5);
- the missing onPlayerSpawn dispatch (Decision 2).

