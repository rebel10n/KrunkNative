# Game assets

Everything here except `maps/`, `img/`, `shaders/`, `skins.json`, `previews.json` and this
file is gitignored: the content is imported from the Krunker reference trees (or `mod.zip`)
and deployed to the asset host separately (`deploy/ansible` role `kn_web`, tag `web_assets`).

## Krunker v7.1.4 content (current)

KrunkNative is being upgraded in place to Krunker v7.1.4 (`doc/v7/decisions.md`). Import the v7
content set with:

```sh
make assets-import-v7    # python3 -I tools/ui-import/game_assets.py --v7 + gen_cosmetics.py
make assets-check-v7     # drift check only: lists missing/different files, exit 1 on any
```

Provenance: `/home/user/jails/kru/src/Krunker_v7.1.4` (`public/` is what the v7 client loads
from its asset host). The import copies byte-identical, never deletes, and replaces the v3
file of the same name when v7 changed it:

| assets/ | v7.1.4 source | notes |
|---|---|---|
| `models/` | `public/models/` | props, `weapons/` (incl. `weapon_<w>_<mid>.gltf` remodels), `melee/`, `attach/` and the cosmetic dirs `hats/ body/ back/ waist/ faces/ shoes/ pets/ charms/ wrist/ collectibles/ tickets/`. `.obj`, plus `.gltf` for the animated (`hasAnim`) items, loaded by `client/src/gltf.c` |
| `textures/` | `public/textures/` | model textures, `previews/` (item previews, `UTILS.getPreview`), `badges/`, `playercards/` (`.webp`), `classes/`, ... |
| `sound/` | `public/sound/` | weapon, class and per-skin sounds |
| `videos/` | `public/videos/` | skin `video` textures |
| `user-assets/` | `public/user-assets/` | map/AI user assets (`model.gltf`/`.glb`, `sound.mp3`, ...) |
| `scares/` | `public/scares/` | jump-scare trigger media |
| `skins.json` | `public/skins.jspck` | the v7 store table (6448 skins, 17 item types), unpacked by `tools/ui-import/v7_skins.js` with v7's own `src/shim/jspck.js`, `store.js` templates merged in; checked against the reference's `skins.json` |
| `previews.json` | `previews.json` | v7's item type enum (`src/viewer.js`) |

`skins.json` is the store table every component indexes by **array position**
(`services/brain/store.c`, `shared/src/bots.c`, inventory `skin_id`): the importer refuses to
write a table that moves an existing index (same `id`, `weapon` and item type, except v7's
HAT 1 -> HEAD 15 and BODY 2 -> BACK 14 retypes, which keep their slot). All 1917 v3.1.1 indices
are unchanged in v7 (185 are retyped, 3 renamed). `client/src/cosmetics_table.c` is generated
from it by `tools/ui-import/gen_cosmetics.py`.

v7.1.4's `public/` is ~1.8 GB for these directories; the v7 import adds ~3500 files
(~720 MB) to a v3.1.1 tree.

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
