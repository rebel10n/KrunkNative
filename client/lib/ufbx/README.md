# ufbx (vendored)

Single-file FBX loader, used by `client/src/fbx_gltf.c` to turn FBX class models (Krunker user
assets, `user-assets/<id>/model.fbx`) into an in-memory GLB for the shared glTF path.

- Source: https://github.com/ufbx/ufbx
- Commit: `1d097696e33b367bb3152b175aeb2d95193f9b6e` (master, 2026-10-25)
- Files: `ufbx.c`, `ufbx.h`, `LICENSE` (MIT), unmodified.
- sha256 (`hashes.txt`):
  - `ufbx.c` `4a956c26a708e40d82ecb0aeaee54da709d5ba7c144c9642174970059bda7e1d`
  - `ufbx.h` `f787be529af577efc04f3e3e735844a08556718dbb613d1211c2b313d72895dc`

Update by downloading both files at a newer pinned commit and updating this note and `hashes.txt`.
The test assets in `client/tests/data/fbx/` come from the same repository's `data/` directory
(Blender sample files): `blender_272_cube_7400_binary.fbx` (cube.fbx) and
`blender_279_sausage_7400_binary.fbx` (sausage.fbx, two animated clips).
