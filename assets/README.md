# Game assets

Please unpack Krunker's `mod.zip` into this folder, more specifically:

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

> [!TIP]
> It is recommended to use a 2020-2021 version of `mod.zip` if possible.

> [!CAUTION]
> Do not delete the pre-existing assets (files that are already in this folder).