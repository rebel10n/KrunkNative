# Game assets

Everything here except `maps/`, `img/`, `shaders/`, `skins.json`, `previews.json` and this
file is gitignored: the content is imported from the Krunker reference trees (or `mod.zip`)
and deployed to the asset host separately (`deploy/ansible` role `kn_web`, tag `web_assets`).

## Krunker v7.1.4 content (current)

KrunkNative is being upgraded in place to Krunker v7.1.4 (`doc/v7/decisions.md`). Import the v7
content set with:

```sh
make assets-import-v7    # python3 -I tools/ui-import/game_assets.py --v7 + gen_viewer_store.js
make assets-check-v7     # drift check only: lists missing/different files, exit 1 on any
```

Provenance: `/home/user/jails/kru/src/Krunker_v7.1.4` (`public/` is what the v7 client loads
from its asset host). The import copies byte-identical, never deletes, and replaces the v3
file of the same name when v7 changed it:

| assets/ | v7.1.4 source | notes |
|---|---|---|
| `models/` | `public/models/` | props, `weapons/` (incl. `weapon_<w>_<mid>.gltf` remodels), `melee/`, `attach/` and the cosmetic dirs `hats/ body/ back/ waist/ faces/ shoes/ pets/ charms/ wrist/ collectibles/ tickets/`. `.obj`, plus `.gltf` for the animated (`hasAnim`) items, loaded by `client/src/gltf.c` |
| `textures/` | `public/textures/` | model textures, `previews/` (item previews, `UTILS.getPreview`), `badges/`, `playercards/` (`.webp`, plus the importer's `.webp.png` sidecars for the native HTML engine), `classes/`, ... |
| `sound/` | `public/sound/` | weapon, class and per-skin sounds |
| `videos/` | `public/videos/` | skin `video` textures |
| `user-assets/` | `public/user-assets/` | map/AI user assets (`model.gltf`/`.glb`, `sound.mp3`, ...) |
| `scares/` | `public/scares/` | jump-scare trigger media |
| `skins.json` | `public/skins.jspck` | the v7 store table (6448 skins, 17 item types): unpacked with v7's own `src/shim/jspck.js` and run through v7's `store.js` load loop (templates, noSpin / noSale / limited / limT fixups) by `tools/ui-import/v7_skins.js`; identical to the reference's `skins.json` minus `thumbnail` |
| `previews.json` | `previews.json` | v7's item type enum (`src/viewer.js`) |

`skins.json` is the store table every component indexes by **array position**
(`services/brain/store.c`, `shared/src/bots.c`, inventory `skin_id`): the importer refuses to
write a table that moves an existing index (same `id`, `weapon` and item type, except v7's
HAT 1 -> HEAD 15 and BODY 2 -> BACK 14 retypes, which keep their slot). All 1917 v3.1.1 indices
are unchanged in v7 (185 are retyped, 3 renamed). `client/src/viewer/viewer_store_table.c` (the
client's full store table: player model cosmetics, store, inventory, loadout, market, viewer) is
generated from it by `tools/ui-import/gen_viewer_store.js` (checked by `test_gen_viewer_store.js`).
Six GLTF remodels in the v7.1.4 reference are corrupt (invalid base64 buffers: weapon_2_12, 4_9,
15_5, 16_3, 17_5, melee_63); v7 fails to load them too, KN falls back to the plain weapon / knife.

v7.1.4's `public/` is ~1.8 GB for these directories; the v7 import adds ~3500 files
(~720 MB) to a v3.1.1 tree.

## Krunker live asset host content (what the v7.1.4 tree lacks)

The v7.1.4 `public/` tree is incomplete: the v7 client and the catalogue maps reference ~600
files it never had. Those that Krunker's CDNs still serve (509 on 2026-10-12, ~36 MB) are pinned
by sha256 in `tools/ui-import/live_assets.lock` and added with

```sh
make assets-import-live    # python3 -I tools/ui-import/fetch_live_assets.py install (KN_FETCH_PROXY=... optional)
make assets-check-live     # lists pinned files missing from assets/ and ui/img
```

It only adds files (an existing file is never replaced). `assets/` gets 187 files: map user
assets (`user-assets/<id>/texture.png`, `model.obj|glb` + `model.png`, `sound.mp3` of the zombies
/ custom / AI maps, `md23677/mod.zip`), the built-in AI models (`user-assets/30171`, `50838`,
`50578`), weapon-skin / shoe / bundle sounds (`sound/weapon_1_23.mp3`, `shoe_46.mp3`,
`weapon_upgraded.mp3`, ...), the zombie perk machine textures (`textures/perkmachine_0_5|6`),
the zombie portal / perk bottle pickups, a few previews and `models/weapons/weapon_2_20.gltf`.
`ui/img` gets the rest (322 HUD / menu images, committed). Sources: `UTILS.assetsUrl` ->
`https://assets.krunker.io/<path>`, `UTILS.userAssetsUrl` -> `https://user-assets.krunker.io/<path>`.
The ~100 references neither host serves (e.g. `sound/weapon_18.mp3`, `land_0`, `ambient_10|13`,
`limited_5..7`, the Hackusate weapon skin textures) 404 for v7 too. The zombie `.glb` models use
`EXT_meshopt_compression`, which `client/src/gltf.c` does not decode yet (the v3.1.1 stand-ins stay).

## Krunker v3.1.1 content (legacy)

The v3.1.1 tree was unpacked from Krunker's `mod.zip`, more specifically:

- `models/*`
- `textures/*`
- `sound/*`
- `css/fonts/font2.ttf`
- `user-assets/<id>/sound.mp3`: the user-asset sounds AI and map triggers play (copy
  `Krunker_v3.1.1/public/user-assets/*/sound.mp3`; v3.1.1 offline ships 3206 and 3 only)
- `textures/previews/*`: the item preview images (store, inventory, loadout skin pickers,
  market, trades, spins, profiles; `UTILS.getPreview`), which `mod.zip` does not contain:
  `make assets-import` copies them from `Krunker_v3.1.1/public/textures/previews/`
  (`tools/ui-import/game_assets.py`; ~6000 files, ~100 MB)

The v7 import is a superset; running `make assets-import` after it puts v3 previews back
(`make assets-check-v7` then reports them).

> [!CAUTION]
> Do not delete the pre-existing assets (files that are already in this folder).
