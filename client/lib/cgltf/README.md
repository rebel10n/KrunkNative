# cgltf

Single-file GLTF 2.0 / GLB parser by Johannes Kuhlmann, MIT licensed (`LICENSE`).

- Upstream: https://github.com/jkuhlmann/cgltf
- Version: release tag `v1.15` (commit `360db1a95480fe102ae9c69b27c5d101167ff5ba`)
- Vendored file: `cgltf.h` only, unmodified (sha256
  `e378a21c084bf1f288bb799de827bb26906efb024255f1ecf1705ea13f11c6ec`)

`client/src/gltf.c` defines `CGLTF_IMPLEMENTATION` and includes it; the rest of the client
uses `client/include/gltf.h`. To update, replace `cgltf.h` with a newer release's file and
update the tag, commit and hash above.
