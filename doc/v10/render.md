# Krunker v10.0.3 renderer parity: lighting, fog, output, GLB reflections

Reference: the decoded v10.0.3 bundle (`/home/user/jails/kru/src/Krunker_v10.0.3/deob/game.deob.js`, cited as
`deob-10.0.3.js:<line>` or `deob:<line>`), checked 2026-10-24 against v10.0.2 (`deob/v10.0.2/game.deob.js`) with
a name-normalised diff (obfuscated identifiers replaced, 7 100 diff lines, the renderer hunks read one by one).
v10.0.3 runs THREE r18x's `WebGPURenderer` (WebGL2 fallback) with TSL node materials and turns
`ColorManagement` off (`deob:230030`).

## Lighting and fog defaults

`render.sceneInit(data, mode, keepScene)` (`deob:234561-234634`) builds the lights and the fog from the map's
JSON as it is (the main map: `MapManager.generate` passes the raw data to `render.init`, `deob:188648`):

| key | rule | default | KN |
|---|---|---|---|
| `ambient` | AmbientLight only when `mode.ambCol != null \|\| map.ambient` | none (no light) | client/src/main.c `client_apply_map_lighting` |
| `ambientI` | `(map.ambientI ?? 1) * lightIntensityScaling` (PI, `deob:21272`) | 1 | same; numbers as JS coerces them (`json_js_number_or`) |
| `light` | DirectionalLight only when `mode.lightCol != null \|\| map.light` | none | same |
| `lightI` | `(map.lightI ?? 1.3) * PI` | 1.3 | same |
| `lightD` | position `(map.lightD ?? 500) * (cos v, sin v sin u, sin v cos u)` | 500 (`deob:21136`) | shadow.c |
| `sunAngY` / `sunAngX` | `u = PI * (sunAngY ?? 54) / -180`, `v = PI * (sunAngX ?? 90) / -180` | 54 / 90 | main.c |
| `shadowR` | `mapSize = highResShad ? 4096 : shadowR \|\| 1024` | 1024 (`deob:21135`) | shadow.c (KN caps at 4096) |
| `shadowD` | `shadow.camera.far = shadowD \|\| 1200` | 1200 (`deob:21134`) | shadow.c |
| bias | `shadow.bias = -0.0004`, `normalBias = isNumber(shadowNB) ? limitMM(shadowNB, 0, 5) : 0` | | shadow.c, basic.frag |
| `fog` / `fogD` | `new THREE.Fog(mode.fogC ?? map.fog, 1, mode.fogD ?? map.fogD)`; `Color(undefined)` is white | no `fogD`: no fog (`smoothstep(1, NaN, z)` is 0) | main.c |
| `sky` | `renderer.setClearColor(mode.skyCol ?? map.sky)`; undefined keeps the previous colour | | main.c |
| `toneMapping` / `toneMappingExposure` | `toneTypes[limitMM(.., 0, 4)]`, exposure `limitMM(.., 0, 10).round(1)` (`deob:231102-231104`) | 0 / 1 | map_environment.c |

An ENVIRONMENT_ZONE's data is `MapManager.addEnvironmentZone`'s (`deob:185271-185302`): every key gets a value,
`ambientC` / `light` / `sky` / `fogC` (hex-checked, else `#97a0a8` / `#f2f8fc` / `#dce8ed` / `#8d9aa0`),
`ambientI` 1 and `lightI` 1.3 (`parseFloat`), `lightD` 500, `sunAngX` 90, `sunAngY` 54, `shadowR` 1024, `shadowD`
1200 and `fogD` 2000 (`parseInt`: truncated), so a zone always has an ambient light, a sun and fog. KN:
shared/src/map.c `map_parse_environment` (zone: `map_int_or`), test `map_v7_test` ("ENVIRONMENT_ZONE parseInt").

Leaving every zone, `updateEnvironment(-1)` re-runs `init(map data, mode, keepScene = true)`
(`deob:234508-234531`). sceneInit then keeps the scene's lights: a map without `ambient` / `light` keeps the
zone's ambient light / sun (colour, intensity, direction, shadow camera), and a missing `fog` / `sky` colour keeps
the zone's (`Color.set(undefined)`), while the fog distance, tone mapping and the sky dome are the map's again. KN
does the same (client/src/map_environment.c `env_restore_map`).

## Output

- Fog, then tone mapping: the node material fogs in `setupOutput` (`deob:68245-68253`) and the renderer's output
  stage (`getOutput` / `RenderOutputNode`, `deob:67960-67961`, `deob:63870-63884`) tone maps afterwards. v7
  (r150) tone mapped first. KN: basic.frag's tail; the sky dome multiplies its gradient in before tone mapping
  (`outputNode`, `deob:108785-108793`; sky.frag).
- No output encoding: `outputEncoding` still sets `renderer.outputColorSpace` (0 is forced to Linear,
  `deob:231105-231109`), but with ColorManagement off the conversion node is a no-op (`deob:63117-63136`), so
  `outputEncoding: 1` renders like 0. Texture colour spaces still apply (an `SRGBColorSpace` texture is an
  `SRGB8_ALPHA8` texture, `deob:93001`). KN: no `output_encoding` step in basic.frag / sky.frag.
- Post-processing: `render.postfx` (`deob:222710-222900`) only runs passes when a resource pack or mod inserts a
  "post" shader graph (`insertShaderPass`, `deob:231247`; mods' `shaders/*.json`, `deob:256139`). There is no
  bloom, colour grading or FXAA in the default pipeline. KN has no shader graph runtime; mod_pack.c already skips
  `shaders/` (documented there).

## GLB materials (v10.0.3 model setup, `deob:233630-233718`)

Every GLTF / GLB model (skins with `hasAnim`, map assets, AI, KrunkScript models) goes through the same material
pass:

- Colour spaces (`deob:233656-233658`): unless the asset has `env` and no `texEncode`, every map of the material
  takes `encodingTypes[texEncode || 0]`: NoColorSpace by default, so the base colour and emissive maps are read
  raw (GLTFLoader's sRGB tag dropped); `texEncode` 1 decodes sRGB. KN used to decode every GLB base / emissive map
  (the v7.1.4 rule). Now gltf.c uploads raw; `BasicMaterial.gltf_srgb_maps` keeps the decode for a skin with an
  env map, `texture_srgb` / `emissive_srgb` for a map asset with `ten` 1 (user_asset.c).
- Fog: the asset's `noFog`, or the material's glTF extras `noFog` (`material.userData.noFog`).
- Environment reflections (the 10.0.3 changelog's "opt-in environment reflections for GLB materials"):
  `env = parseFloat(userData.env) || (asset.refl && material.metalness > 0 ? asset.refl : 0)`; when `env > 0` and
  the material has no envMap, `envMap = render.getEnvMap()`, `envMapIntensity = limitMM(env, 0, 4)`
  (`deob:233665-233669`). A skin `env` loads its own map afterwards and wins.
  - `getEnvMap()` (`deob:231119-231157`): a 256 x 128 canvas, vertical linear gradient with stops 0
    `tint(top, .35)`, .42 `tint(mid, .25)`, .5 `tint(fog, .55)`, .56 `tint(fog, .1)`, 1 `fog * .25`, where
    `tint(c, w) = Color(c).lerp(#8c9098, .55).lerp(#ffffff, w)`, `top = skyDomeCols[1] || skyDomeCols[0] || fog`,
    `mid = skyDomeCols[0] || fog`, `fog = scene.fog colour` (`#8899aa` without a scene). `skyDomeCols` is set
    when a dome is built (`deob:231159`) and never cleared. EquirectangularReflection, SRGBColorSpace, cached by
    the colour key.
  - MeshStandardMaterial turns it into a PMREM (64 px cube, 9 levels GGX-filtered with roughness
    `sqrt(r_i^2 - r_{i-1}^2) * r_i * 1.25`, 256 samples, `deob:70739-71011`) and EnvironmentNode / the physical
    lighting model add the radiance (`roughness^4`-bent reflection) and the irradiance (level 8 along the normal)
    with the DFG LUT's split-sum and multi-scattering terms (`deob:71245-71275`, `deob:70081-70091`,
    `deob:70268-70318`).
  - KN (client/include/env_map.h, client/src/env_map.c, basic.frag `krunker_scene_env`): the gradient depends on
    the elevation only, so env_map.c paints the 128 rows, runs the same GGX cascade on the CPU into a 9 x 128
    RGBA16F table and uploads the 16 x 16 DFG LUT; basic.frag samples level `6 - roughnessToMip(r)` and applies the
    same terms. A GLB env material is shaded as MeshStandardMaterial with its factors (as in v10). gltf.c reads
    the extras.
  - Differences (env_map.h): the table is built at map load (a model keeps no load-time snapshot; models loaded
    inside a zone reflect the map's colours); the PMREM faces' own texel blur is not modelled.
  - AI.spawn's `reflections` (`refl`, `deob:211721`, `deob:205080`): ported (doc/v10/krunkscript-1003.md section 3). The
    AI's metallic materials without an extras `env` get `scene_env_intensity = limitMM(refl, 0, 4)` (`air_apply_refl`,
    client/src/ai_render.c). `refl` reaches clients in EXT `AI_REFL_EXT_SUBTYPE` after the `"aai"`.

No GLB under assets/ has material extras (checked 2026-10-24), so no shipped item changes look from the
reflections; the colour-space change brightens every raw GLB texture (v10's look).

## Weapon fog

v10.0.3 renders the viewmodel as layer 1 of the same scene from `fpsCamera` (`render(scene, fpsCamera)`,
`deob:231552-231554`): scene fog applies at the viewmodel's depth, and neither the weapon (`deob:194600-194660`) nor
the arms (`genArms` getMat, `deob:232484`) set noFog. Only the map's weapon pickups do (`genWeaponMeshData`
`noFog: true`, `deob:184768-184783`). KN: fps_scene stays fogged (main.c), pickups unfogged (map_post.c). The
2026-10-24 audit's "addWeapon/arms material noFog" (rendering-network.md #6) cited a byte offset in UI code; it is
the pickup, not the viewmodel.

## 10.0.3 fixes checked

| official line | 10.0.3 change | KN |
|---|---|---|
| Fixed a checkered texture appearing on some skies and map assets | the sky dome loads `skyDomeEmisTex` as a user asset (`asset: true`, `deob:223062-223064`); v10.0.2 looked it up under /textures (missing: the checker) | sky.c always reads `user-assets/<id>/texture.png`: nothing to port |
| Fixed explosions sometimes not showing | `ExplosionManager.mesh.frustumCulled = false` (`deob:225620`): the InstancedMesh was culled by its 1 x 1 x 1 base bounds | vfx.c draws the debris in its batch with no culling: nothing to port |
| Added opt-in environment reflections for GLB materials | above | ported (AI `reflections` too) |
