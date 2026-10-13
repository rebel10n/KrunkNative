# libopus (vendored)

- Upstream: https://opus-codec.org (https://gitlab.xiph.org/xiph/opus)
- Version: **1.5.2**, release tarball `opus-1.5.2.tar.gz` from https://downloads.xiph.org/releases/opus/
  (sha256 `65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1`, as listed in
  xiph's `SHA256SUMS.txt`).
- License: BSD 3-Clause (plus the patent grants listed there); see `COPYING`.
- Removed from the tarball: `tests/`, `doc/`, the meson build, and every file under `dnn/` except
  its headers and `.mk` lists (the DNN weights for DRED / OSCE / deep PLC, ~18 MB, which the
  build turns off). Everything else, upstream's `CMakeLists.txt` and the autotools files it
  reads its source lists from included, is unmodified.

## Why this dependency

Voice chat (client/src/voice.c) records and plays Opus clips, the codec every Krunker client
uses. libopus used to be loaded at run time, and the Windows and macOS packages shipped none:
those clients sent 8 kHz WAV and could not play anyone's Opus clips. Compiling it into every
client (native and web) makes that impossible. Built by the root CMakeLists.txt
(`krunknative_add_opus`): static, float API, `-O2` in every build type, no intrinsics on wasm.

## Updating

Download the new release tarball and its `SHA256SUMS.txt` into a scratch directory, verify,
unpack, drop the same parts, replace this tree and update the version and hash above.
