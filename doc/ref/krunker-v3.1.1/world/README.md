# World rendering parity captures (map geometry)

Camera-matched pairs of the REAL Krunker v3.1.1 client and KrunkNative, taken
2026-10-10 at 1280x720, vertical FOV 90, render resolution 1, UI hidden.

Per pose `<name>`:

- `<name>-krunker.png` — real game (reference server, headless Chromium/SwiftShader)
- `<name>-native.png` — KrunkNative after the world-rendering fixes
- `<name>-native-before.png` — KrunkNative at the start of that work (same harness;
  stored as 256-colour PNGs to fit the size budget; none for the two setting variants of
  existing poses, the old build had no lighting setting)

The "before" images were rendered from a snapshot of the tree as it was when the
world work started (other agents' in-progress lighting changes included), so they
also differ in lighting; compare geometry/texturing in them, lighting/shadows in
`doc/ref/krunker-v3.1.1/sky/`.

## Poses

Pose = `x,y,z,pitch,yaw` in Krunker units / radians. The camera uses Krunker's
YXZ Euler order (yaw about Y, then pitch about X; yaw 0 looks down -Z, pitch > 0
looks up). The list is also `tools/kref/world-poses.txt`.

| name | map | pose | what it covers |
| --- | --- | --- | --- |
| burg-street | burg | -80,30,35,-0.1300,-1.5577 | crate model, walls with repeated textures, door, cube colour gradient, AO planes |
| burg-ramp | burg | 140,25,40,-0.2332,1.4667 | ramp (single double-sided plane + ambient_1 edge strips), walls |
| freight-yard | freight | 316,40,-156,-0.2531,1.5708 | light cone (additive), containers, roofs, noise planes, AO |
| freight-sign | freight | 270,50,-100,-0.0500,-3.1416 | SIGN canvas text ("A-13", r[2] = -PI) |
| industry-windows | industry | 45,52,-26,-0.0540,1.5708 | window models (transparent), brick/plaster walls |
| industry-lightcone | industry | -130,48,38,-0.1544,-2.4942 | street lamp light cone, containers, grass |
| industry-ladder | industry | 255,22,135,-0.1371,-3.1416 | ladder (stretched floor texture), crates, barrel, grass |
| kanji-billboard | kanji | -440,-100,-406,-0.1019,1.5708 | billboard poster (r[2] = PI/2), emissive neon |
| kanji-neon | kanji | 297,154,-272,-0.25,2.3 | emissive cubes, glass, night lighting |
| kanji-glass | kanji | -399,300,-930,-0.0983,-0.0000 | link_0 chain-link fences (alpha test, transparent opacity) |
| sandstorm-billboard | sandstorm | 7,107,-60,0,0 | billboard poster (r[2] = 0), windows, crates, barrels, grass |
| sandstorm-ad | sandstorm | -10,100,-52,-0.0634,-1.5708 | "sad" billboard: the "Your Ad Here" canvas (r[2] = -PI/2) |
| sandstorm-billboard-rot | sandstorm | 243,99,70,0,-3.1416 | billboard poster (r[2] = PI), ladder |
| shipyard-containers | shipyard | -77,46,45,-0.15,0.8 | containers, chain-link fence, "sad" billboard, grass colour sharing |
| freight-noise | freight | 281,30,-60,-0.4266,-0.0000 | noise planes (`en`): snow mounds generated around object bases |
| freight-pumpkin | freight | 259,9,-48,-0.4182,-0.0000 | pumpkin models with their `_e` glow texture (the carved face of the background one) |
| shipyard-vehicle | shipyard | 58,18,158,-0.3753,-3.1416 | vehicle model with its `_e` glow texture |
| oasis-fountain | oasis | 4,30,-205,-0.5266,-0.0000 | liquids (two coplanar layers, 10x10 wave grid), transparent water |
| oasis-sea | oasis | 4,25,-380,-0.2934,0.0648 | large liquid with waves (`wh`/`ws`) and a scrolling texture |
| sandstorm-lighting2 | sandstorm | 7,107,-60,0,0 | lighting setting 2: every material Phong (specular) |
| kanji-lighting0 | kanji | 297,154,-272,-0.25,2.3 | lighting setting 0: every material MeshBasicMaterial (unlit) |
| pltest-lighting2 | `tools/kref/world-maps/pltest.json` | 0,40,90,-0.2630,-0.0000 | lighting 2 with three POINT_LIGHTs (falloff/decay variants); none of the stock maps has one |

Liquid waves and scrolling textures depend on time, so their exact phase differs between
the two images. Noise-plane bumps use `Math.random()` in v3.1.1, so their heights differ.
Stacked cubes losing their bottom face (93.js) are not visible from outside; 244-436
cubes per stock map are affected. The vehicle shows horizontal lines natively with the sun
shadow on (see the report: shadow self-shadowing on props), none with `shadows` off.

Setting runs (4th column of `world-poses.txt`): KrunkNative reads them from
`$XDG_CONFIG_HOME/krunknative/knSettings.json`, the real game from
`localStorage.kro_setngss_<key>` in a separate Chromium profile.

Billboards without `bb` pick a random poster in both clients (`randInt(1, 6)`), so
those posters can legitimately differ between the two images.

## How they were produced

KrunkNative (`client/src/debug_capture.c`):

```sh
tools/kref/world-native.sh sandstorm "7,107,-60,0,0" out.png <build dir>
# Captures ignore stored settings: with KRUNKNATIVE_CAMERA/SCREENSHOT set and no
# XDG_CONFIG_HOME, the client uses an empty private config dir (debug_capture_startup).
# = KRUNKNATIVE_CAMERA="x,y,z,pitch,yaw" KRUNKNATIVE_FOV=90 KRUNKNATIVE_HIDE_UI=1 \
#   KRUNKNATIVE_SCREENSHOT=out.png KRUNKNATIVE_RESOLUTION=1 XDG_CONFIG_HOME=<empty dir> \
#   xvfb-run ... client_bin --offline assets/maps/sandstorm.json
```

Real game: `cd <Krunker_v3.1.1> && PORT=6773 LOBBY_COUNT=1 npm run server`, then
`node tools/kref/world-ref.js sandstorm "7,107,-60,0,0" out.png`. The script loads
the map as the offline sandbox's custom map (`localStorage.custMap` =
`maps/<map>.json`, identical to `assets/maps/`), forces the render resolution
setting to 1, captures the renderer object (55.js) through an `Object.prototype`
`fpsCamera` setter and wraps its `render()` so the camera (parents reset) sits at
the pose before every frame, then hides every DOM element except the WebGL canvas.
`tools/kref/world-capture.sh` runs every pose and writes 50% side-by-sides. Nothing
in the reference tree was modified.
