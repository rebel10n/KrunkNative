# Game content audit (2026-10-15)

What players actually get, checked against what the game asks for. Requests R103 ("missing ...
sounds for weapons, junk textures and models, crate models ... everything missing from each map")
and R25 ("the sawed off is missing its preview image ... what other images we're missing").

## Method

The content set players get is the launcher's: a fresh `krunknative --cli --no-launch` from the
`client-latest` linux-x64 package against `https://dl.krunk.io/updates/client/latest.json`
synced content `442d6fc77b68be68` (22,039 files, 2.5 GB, every file sha256-checked), plus the
package's own `assets/` files. The git-tracked `assets/` files of `main` were laid over it (what
the next package ships, see "Tracked files" below). Then, with `KRUNKNATIVE_ASSET_FETCH=0` and
the `tools/ui-import/live-inventory/misslog.c` LD_PRELOAD shim logging every failed open under
`assets/`:

- **every map file**: all 76 under `assets/maps/` (the 32 official v7.1.4 maps of the catalogue,
  the 311_ / 152_ legacy maps, the 13 community maps, the 7 Strike maps), each an offline match of
  7 s with bots (`client_bin --offline <map>`, input script `play`): map textures, user-asset
  models / textures / sounds, prefab models (crates, junk, trees, teddies, pickups), sky, the
  weapons the player and bots carry and every sound the client preloads;
- **every menu**: all 207 `tools/uidiff/scenarios` rendered with `--render-scenario` (store,
  customize / loadout pickers, market, spins, profiles, settings, guide, HUD, end screens);
- **every weapon** statically: `shared/src/weapons/*.c` icon (`textures/weapons/<icon>.png`),
  model (`models/weapons/<src>.obj|gltf`), texture and `.sound`.

Not counted: `.mtl` probes (the OBJ loader looks for an optional material file; Krunker's tree has
none) and `.ttf` probes next to a `.woff2` the HTML engine then decodes.

## Findings

| missing | where it shows | cause | status |
| --- | --- | --- | --- |
| `user-assets/707/model.obj`, `model.png` | Strike Habitat (`aid "707t"`) | never imported | fetched from user-assets.krunker.io, `live_assets_v10.lock` |
| `textures/previews/weapons/weapon_30.png`, `weapon_31.png` | Customize > secondary picker: Boulder, Compressor (v10's new weapons) | never imported | fetched from assets.krunker.io, `live_assets_v10.lock` |
| `img/mouse/10002.png` | Settings > Controls, middle mouse button | never imported | fetched from assets.krunker.io, committed to `ui/img/mouse/` + lock |
| `sound/kn_drank_*.mp3`, `kn_laced_ambience.mp3`, `kn_money_throw.mp3` | Nettspend Purple Drank / Money Spread | git-tracked but in neither the packages (fixed list of directories) nor kru's web tree | `package-client.sh` ships every tracked file; deploy syncs them |
| `titles.json` | Customize > Title (the v10 titles catalogue) | same | same |
| `videos/limited_shop_banner.webm.knv`, `videos/tutorial/basics.mp4.knv`, `scares/1.mp4.knv` | Eterno shop banner, Guide video, jump scare (native `<video>` frames) | generated (`make video-frames`, gitignored) on the dev machine, never synced to kru | needs a `web_assets` sync from the main checkout |
| `sound/weapon_18.mp3` | the Bomb's shot | Krunker never had it (404 on assets.krunker.io too, `assets/README.md`) | none: v7 / v10 play nothing either |

The Sawed Off's own preview (`textures/previews/weapons/weapon_16.png`) and its icon, model,
texture and sounds are all there (fixed by 476770f2 / c077de5c). No map is missing a texture,
model, crate, junk or tree prop, or a user-asset sound besides Habitat's 707. Every file pinned in
`live_assets_v10.lock` and `live_assets.lock` is on the CDN.

## Kru lagged main (fixed by the deploy change)

The web client, the Pages CDN (`assets.krunk.io`, `assets1.krunk.io`) and the launcher's content
index all read kru's web asset tree `/opt/krunknative-web/assets`, which CI never updated
(`deploy-kru.yml` ran with `kn_web_sync_assets=false`). Of the 131 git-tracked `assets/` files,
18 were absent there and 2 stale on 2026-10-15:

- absent: `maps/strike/*.json` (7), `maps/custom/bot_ftue_v2.json`, `shaders/laced.frag`,
  `shaders/outline.frag`, `shaders/outline.vert`, the 5 Nettspend sounds, `titles.json`,
  `README.md` (excluded on purpose);
- stale: `shaders/html_ui.frag`, `shaders/spin_wheel.frag` (web players ran the old shaders).

The `kn_web` role now syncs the tracked subset whenever the full sync is off, i.e. on every CI
deploy (`deploy/ansible/roles/kn_web/tasks/tracked_assets.yml`: rsync `--files-from` the
`git ls-files assets` list, `--checksum`, temp + rename, never `--delete`: the untracked 1.5 GB
already on kru stays as it is), before `kn-updates assets` re-indexes and the Asset CDN workflow
mirrors it.

## To do on the dev machine (not CI)

The untracked content is only synced by hand. After this lands, from the main checkout:

```sh
make assets-import-live      # installs the new live_assets_v10.lock files into assets/
cd deploy/ansible && ansible-playbook central.yml --tags web_assets   # assets/ -> kru, re-index
```

## Community maps' user assets (2026-10-15)

Report: "missing textures / the purple-black box thingy in custom maps". The box is render.js's
`missingTexture` checker (`client/src/prefab.c` `prefab_missing_texture`), drawn for a texture that
does not load.

Sample: 109 maps rendered headless, each from its first spawn (4 directions) and from above: the 46
official maps Krunker serves today (`assets/maps/official/`), the bundled community maps
(`assets/maps/custom/`), every map of the v7.1.4 tree and the v7.1.9 viewer's `custom_maps/`, the
v3.1.1 custom maps, and Zombie_Bulwark (Keybi, 10.7 M plays: a live custom game's map from the
client's Krunker map cache). gapi has no public endpoint for a community map's data
(`/maps/by-name/<name>` is metadata only; custom games send it inline), so live custom maps come
only from games joined; the brain has no player-published map yet. Placeholder pixels are counted
by rendering each view twice, the second time with the checker painted one flat colour
(`KRUNKNATIVE_MISSING_TEXTURE=00ffff`), and counting the pixels only that run has in it; every
failed open under the asset dir is logged (`misslog.c`, as above).

Findings:

- Every texture of Krunker's map texture table (`t`, `tv` variants) is in the asset tree: no map
  misses a `textures/<src>.png`.
- Every placeholder and every invisible object came from **Krunker user assets** the tree never
  had: `user-assets/<id>/texture.png` (`at` textures) and `model.obj` + `model.png` / `model.gltf` /
  `model.glb` (`aid` models; a missing model is not a checker, the object is just absent), a
  resource pack (`md43867/mod.zip`) and a sound. 220 files of 171 asset ids in 8 maps: Zombie_Bulwark
  (37), Khepri (44), Bastion (59), Piazza (11), Eterno Simulator (7), Vivo (5), Laboratory (2),
  Tortuga (1), plus their model textures.
- Cause: nothing fetched them. Natively the on-demand fetch only knew the launcher's content
  manifest (and is off in a dev tree); the web asks the asset host, which has only the 397 imported
  v7 assets. `user-assets.krunker.io` itself answers 403 (Cloudflare) to some networks, the dev
  machine's among them, and sends no CORS headers, so a page on krunk.io cannot read it.
- Formats: all 139 textures are real PNGs (no WebP, no JPEG); the GLTF / GLB models embed PNG
  images (two use `EXT_meshopt_compression` / `KHR_texture_transform`, both supported). Nothing to
  convert.
- Krunker still serves every one of the 220 files. The only 404s are the optional companion maps
  (`model_e.png` / `model_l.png`) of assets whose flags do not ask for them; nothing in the sample
  is gone from Krunker's host.

| map | placeholder pixels before | after |
| --- | --- | --- |
| Eterno Simulator (39) | 21,257 | 0 |
| Khepri (35) | 13,263 | 0 |
| Zombie_Bulwark | 2,817 | 0 |
| Bastion (43) | 2,681 | 0 |
| the other 105 | 0 | 0 |

Fix: user assets are fetched on demand like game content.

- Native (`client/src/net/asset_fetch.c`, `KRUNKNATIVE_USER_ASSETS`, `_USER_ASSET_ORIGINS`): a path
  of `shared/include/user_asset_path.h` that the content set lacks comes from Krunker's host, then
  the brain's mirror (`https://coordinator.krunk.io/api/user-assets/`), into
  `<assets>/user-assets/<id>/`; also in a dev tree or without a manifest (user-assets/ is
  git-ignored). A 403 takes an origin out for 5 minutes; an HTML page answered 200 is refused. The
  map's load-time prefetch (`prefab.c`) now runs natively too and waits for them (at most
  `KRUNKNATIVE_ASSET_WAIT_MS`, 10 s), so models (GLTF too) are built with their files; textures that
  arrive later pop in. Sky, minimap image and resource pack wait the same way.
- Web (`shared/src/utils.c`): a user asset the asset origins lack comes from the brain's mirror
  (`<brain or this origin>/api/user-assets/<id>/<file>`; krunk.io's openresty sends `/api/` to the
  brain).
- Brain (`services/brain/user_assets.c`): `GET /api/user-assets/<id>/<file>` answers from the asset
  tree, its cache (`user_assets.cache_dir`, on kru `/var/lib/krunknative/user-assets`), else
  Krunker's host (`user_assets.proxy` / `KN_USER_ASSETS_PROXY` when kru's network is refused).

Late files and videos (follow-up):

- A model whose file arrives after the load-time wait is built into the running map: an OBJ is
  re-centred on its real geometry (loadMesh's bounding-box centring; the placeholder had none), a GLTF
  / GLB instance is created under the carrier mesh the object got at build time and added to the scene
  (`user_asset_arrived`, from the native fetch's arrivals).
- GLTF map assets used to be destroyed whenever the map left the scene, also for a new round on the
  same map, which keeps the map's meshes (game_init's `map_reset`); the offline game does exactly that
  at its first INIT, so GLTF user assets never showed offline. Their instances now live as long as their
  carrier meshes (`user_asset_mesh_freed` from `map_fini` / `map_object_free`).
- Video assets (`"at": "<id>v"`, Krunker's VideoTexture of `<id>/video.mp4`) play: the brain's mirror
  makes `<id>/video.mp4.knv`, the frame sidecar the HTML engine's `<video>` already plays
  (`shared/include/knv.h`; ffmpeg on the brain's host, 512 px / 15 fps / 120 s at most), and
  `client/src/map_video.c` decodes it onto the material, looped, on both platforms. No sidecar: the
  asset's `texture.png`, else black (what a VideoTexture shows before its first frame). Real example:
  `30538v` (Strike's Krunk Plaza), 48 s, 719 frames, 9.7 MB.

Tests: `client/tests/user_assets_late_e2e.py` (`make user-assets-test` runs both: models held back
past the wait, a looping red / blue sidecar, a texture.png fallback), `client/tests/user_assets_e2e.py` (the fixture map
`client/tests/data/user_assets_map.json` from stub hosts), `asset_fetch_test` (user assets),
`services/brain/tests/user_assets_test.py` (ctest `brain_user_assets_e2e`). Nothing is vendored:
the files are fetched at run time, not committed.

## Re-running the audit

```sh
gcc -shared -fPIC -O2 -o /tmp/misslog.so tools/ui-import/live-inventory/misslog.c -ldl
# content as players get it: an install synced by the launcher (no --updates-url: the live one)
krunknative --cli --no-launch            # in an unpacked client-latest package
env -u DISPLAY KRUNKNATIVE_HEADLESS=1 KRUNKNATIVE_AUDIO_BACKEND=null KRUNKNATIVE_ASSET_FETCH=0 \
  KRUNKNATIVE_ASSETS_DIR=<install>/assets MISSLOG=/tmp/<map>.miss LD_PRELOAD=/tmp/misslog.so \
  KRUNKNATIVE_INPUT_SCRIPT=<(printf '0 play\n7000 quit\n') client_bin --offline assets/maps/<map>.json
# the same environment with client_bin --render-scenario tools/uidiff/scenarios/<s>.json out.png
```

Fetching (`tools/ui-import/fetch_live_assets.py fetch`): `user-assets.krunker.io` answers 403 to
direct requests now (also for files already in the lock); `KN_FETCH_PROXY` gets through.
