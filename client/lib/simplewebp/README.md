# simplewebp

Single-file WebP decoder (lossy VP8 with alpha, lossless VP8L), used by the HTML engine's image loader
(`client/src/html/engine/webp.c`) so `.webp` images decode natively instead of through `.webp.png`
sidecars.

- Upstream: https://github.com/MikuAuahDark/simplewebp, `simplewebp.h` at commit
  679ee727829df44962e3de0cb8fbcc3db9dab962 (header version 20260718), unmodified.
- Licence: BSD-3-Clause (Google's libwebp code it derives from, and Miku AuahDark), with the WebM
  patent grant; both are at the bottom of `simplewebp.h`.
- Not supported upstream: animation (an animated WebP still falls back to its sidecar's first frame).
