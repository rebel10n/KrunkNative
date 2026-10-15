# Game content audit (2026-10-15)

What players actually get, checked against what the game asks for. Requests R103 ("missing ...
sounds for weapons, junk textures and models, crate models ... everything missing from each map")
and R25 ("the sawed off is missing its preview image ... what other images we're missing").

## Method

The content set players get is the launcher's: a fresh `krunknative --cli --no-launch` from the
`client-latest` linux-amd64 package against `https://dl.krunk.io/updates/client/latest.json`
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
