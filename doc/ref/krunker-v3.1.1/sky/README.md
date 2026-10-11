# Sky, lighting, fog and sun shadows: KrunkNative vs real Krunker v3.1.1

Captured on 2026-10-10. The "after" column was re-captured after the clear-colour and
caster-side fixes, with the other agents' world-material, ambient-occlusion and capture-harness fixes in the tree. Each `<map>-before-after-real.jpg` has three columns: KrunkNative
before this work, KrunkNative after it, and the real v3.1.1 client. Rows are the poses
`horizon`, `up` and `down`. `real/` has the reference frames at full size (1280x720).
`poses.json` lists the exact poses.
Native captures ignore stored settings (`debug_capture.c` points `XDG_CONFIG_HOME` at an
empty directory unless one is given): a stored `aspectRatio` had made earlier frames wider.

## Poses

All poses use the map's `camPos`. The vertical FOV is 90 degrees in both clients. Yaw and
pitch are in radians, in Krunker's YXZ order (yaw 0 looks down -Z).

- `horizon`: pitch 0.05, yaw pointing at the centre of the map's object bounds
- `up`: pitch 0.45, that yaw + 2.0
- `down`: pitch -0.35, that yaw - 1.6

| map | camPos | horizon yaw |
| --- | --- | --- |
| sandstorm | [-124, 62, -269] | -2.26 |
| kanji | [297, 154, -272] | -0.9458 |
| subzero | [-13, 93, -6] | 0.9207 |
| undergrowth | [0, 38, -387] | -2.1519 |
| oasis | [4, 32, -200] | -0.4695 |
| freight | [316, 33, -156] | 1.729 |
| lostworld | [-160, 131, -13] | -1.3977 |
| citadel | [108, 130, -291] | 2.5292 |
| industry | [-130, 48, 38] | -0.1079 |

## How the captures were made

- Real game: run `PORT=6774 LOBBY_COUNT=1 npm run server` in the reference tree, then start
  headless Chromium (swiftshader, 1280x720) with the `tools/kref/shim.js` pointer-lock shim.
  `tools/kref/sky/realcap.js` waits for the menu and calls `Game.map.generate(i, ffa, true)`
  for each map, which runs the client's real map/renderer init. It hides the DOM, wraps
  `RENDER.render` to pin `CONTROLS.object`, `pitchObj` and the camera to the pose, sets the
  fov to 90, and takes a screenshot. The server ignores a map argument, so maps are
  switched client-side.
- KrunkNative: `tools/kref/sky/nativecap.sh <build> <repo root> <out> <maps...>` runs
  `client_bin --offline assets/maps/<map>.json` under xvfb with `KRUNKNATIVE_CAMERA`,
  `KRUNKNATIVE_FOV=90`, `KRUNKNATIVE_HIDE_UI=1` and `KRUNKNATIVE_SCREENSHOT`
  (client/src/debug_capture.c).
- `tools/kref/sky/trip.sh` builds the comparison strips.

## RMSE against the real frame (0..1)

| map | pose | before | after |
| --- | --- | --- | --- |
| sandstorm | horizon | 0.134 | 0.033 |
| sandstorm | up | 0.127 | 0.084 |
| sandstorm | down | 0.140 | 0.006 |
| kanji | horizon | 0.185 | 0.008 |
| kanji | up | 0.233 | 0.010 |
| kanji | down | 0.183 | 0.014 |
| subzero | horizon | 0.117 | 0.041 |
| subzero | up | 0.114 | 0.018 |
| subzero | down | 0.134 | 0.018 |
| undergrowth | horizon | 0.161 | 0.012 |
| undergrowth | up | 0.233 | 0.039 |
| undergrowth | down | 0.094 | 0.010 |
| oasis | horizon | 0.153 | 0.042 |
| oasis | up | 0.171 | 0.003 |
| oasis | down | 0.195 | 0.059 |
| freight | horizon | 0.145 | 0.020 |
| freight | up | 0.102 | 0.003 |
| freight | down | 0.193 | 0.009 |
| lostworld | horizon | 0.104 | 0.008 |
| lostworld | up | 0.129 | 0.073 |
| lostworld | down | 0.090 | 0.003 |
| citadel | horizon | 0.130 | 0.009 |
| citadel | up | 0.145 | 0.004 |
| citadel | down | 0.117 | 0.017 |
| industry | horizon | 0.158 | 0.004 |
| industry | up | 0.211 | 0.087 |
| industry | down | 0.116 | 0.009 |

The larger remaining differences (Sandstorm, Lostworld and Industry `up`, Oasis `down`) are
billboard ad text ("i love Yendis" in the real client), a red objective marker in Sandstorm,
and ambient-occlusion plane details. None of them are sky, lighting, fog or shadow.

## What v3.1.1 does (and what is ported)

- Sky (`src/libs/SkyDome.js`, `render.js`): when the map has `skyDome` (and the mode has
  no `skyCol`), a radius-50 32x32 sphere is centred on the camera. It is unlit and has no
  fog. Its vertex colours form a 3-stop gradient over normalised height: `skyDomeCol0` at
  the zenith, `skyDomeCol1` at the horizon, `skyDomeCol2` at the nadir. The defaults are
  `#74a4b9`, `#dce8ed` and `#dce8ed`. With `skyDomeTex` the colours are multiplied by
  `sky_6.png`, which scrolls by `skyDomeMovT`/`skyDomeMovD`. Otherwise only the clear
  colour (`sky`) shows. v3.1.1 has no procedural sun disc, stars or clouds. The suns in
  Oasis and Industry are LIGHT_CONE map objects.
- Lighting (`render.js` initScene, three.js r121): an AmbientLight(`ambient`, `ambientI`
  ?? 1) when `ambient` is set, and a DirectionalLight(`light`, `lightI` ?? 1.3) at
  `lightD` (500) * (cos v, sin v sin u, sin v cos u), with u = PI*`sunAngY`(54)/-180 and
  v = PI*`sunAngX`(90)/-180. Map materials are `StrippedLambertMaterial`, but it copies
  `type: "MeshLambertMaterial"` and so renders with the stock lambert shader:
  `diffuse * (ambient + light * max(N.L, 0) * shadow) + emissive`. Lighting is per vertex,
  which equals per face on Krunker's flat geometry. There is no gamma or tone mapping.
  This was checked on the live client by zeroing each light: the two terms add up per
  channel.
- Fog: `THREE.Fog(fog, 1, fogD)`, i.e. `smoothstep(1, fogD, -viewZ)`. This is view-space
  depth, not distance.
- Shadows: on by default ("shadows" setting). BasicShadowMap, `shadowR` || 1024, camera
  far `shadowD` || 1200, near 0.5, bias -0.0004. The orthographic extents come from the
  solid cubes' bounds divided by 2.5. Only the static map casts, rendered once
  (autoUpdate off). Casters and receivers follow the builders in `src/data/objects.js`:
  - cast and receive: opaque cubes, cylinders, gates, banks, triggers and terminals;
    ladders, boost pads, custom assets and the OBJ props
  - receive only: ramps, planes, billboards, runes, liquids, grass, weapon pickups,
    showcases and bomb sites
  - neither: signs, light cones, spheres, objectives, flags and zones
  Players receive but do not cast. The table is `shadow_prefab_flags()` in
  `client/src/shadow.c`.
- Game-mode overrides (Stalker: skyCol/fogC/fogD/lightCol/ambCol) replace the map's values
  in `client_apply_map_lighting`. A mode skyCol also removes the sky dome.
