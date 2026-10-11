# miniaudio (vendored)

- Upstream: https://github.com/mackron/miniaudio
- Version: **0.11.25** (release tag `0.11.25`, 2026-03-03), `miniaudio.h` copied unmodified
  (sha256 `ac7af4de748b7e26b777f37e01cee313a308a7296a3eb080e2906b320cc55c89`).
- License: public domain (Unlicense) or MIT No Attribution, at your choice; see `LICENSE`.

## Why this dependency

KrunkNative had no audio. Krunker plays its sounds through Howler.js (Web Audio): 2D
one-shots and loops, 3D `PannerNode` sounds (inverse distance model) and a master volume.
miniaudio covers all of that in one public-domain C header with no build system and no
system libraries to link on Linux/Windows/macOS (backends are loaded at runtime):

- playback backends: PulseAudio/PipeWire/ALSA/JACK, WASAPI/DirectSound, Core Audio, and
  Web Audio under Emscripten (same C code for the web build);
- the bundled MP3 decoder (dr_mp3) reads Krunker's `assets/sound/*.mp3` directly, so no
  second decoder (stb_vorbis etc.) is vendored;
- `ma_engine` provides mixing, per-voice pitch (Howler `rate`) and 3D spatialisation with
  `ma_attenuation_model_inverse`, the same formula as Web Audio's `inverse` model.

The implementation is compiled once, in `client/src/miniaudio.c`, with unused parts
switched off (`MA_NO_FLAC`, `MA_NO_ENCODING`, `MA_NO_GENERATION`, `MA_NO_RESOURCE_MANAGER`).
The game-facing API is `client/include/audio.h` (`client/src/audio.c`).
