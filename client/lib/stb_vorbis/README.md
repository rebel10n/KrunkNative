# stb_vorbis (vendored)

- Upstream: http://nothings.org/stb_vorbis/ (the `nothings/stb` repository, `stb_vorbis.c`)
- Version: **1.25** (2026-11-19), `stb_vorbis.c` copied unmodified
  (sha256 `97a8febc738e6a29fafc8d8bc557c7f17ad3db7ba6544485f59395cc6008b764`).
- License: public domain (Unlicense) or MIT, at your choice; see `LICENSE` (the text at the end of
  `stb_vorbis.c`).

## Why this dependency

Krunker v10.0.7's Skin Manager takes `.mp3`, `.wav` and `.ogg` sounds (`acceptedTypes`,
deob-10.0.7.js:127038), and a browser plays an Ogg Vorbis file wherever it plays an MP3.
miniaudio (`client/lib/miniaudio`) decodes Vorbis only through stb_vorbis: when
`stb_vorbis.c` is included before the miniaudio implementation it defines `MA_HAS_VORBIS`, and
`ma_decoder` / `ma_decode_memory` then recognise Ogg Vorbis data like MP3 and WAV. So every
sound the client loads through `audio.c` (Krunker's sounds, a custom skin's shoot / swap /
inspect sound, a map's or mod's sound) plays an Ogg Vorbis file as well, natively and on the web.

It is compiled once, in `client/src/miniaudio.c`: the header part (`STB_VORBIS_HEADER_ONLY`)
before `MINIAUDIO_IMPLEMENTATION`, the implementation after it, as miniaudio's documentation
describes.
