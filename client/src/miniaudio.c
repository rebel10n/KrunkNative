// The miniaudio implementation (client/lib/miniaudio, public domain / MIT-0), compiled once, with
// stb_vorbis (client/lib/stb_vorbis, public domain / MIT) as its Ogg Vorbis decoder: the header
// before the implementation defines MA_HAS_VORBIS, the decoder itself follows it.
#define STB_VORBIS_HEADER_ONLY
#include "../lib/stb_vorbis/stb_vorbis.c"
#undef STB_VORBIS_HEADER_ONLY

#define MINIAUDIO_IMPLEMENTATION
#include "audio_miniaudio.h"

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-value"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#include "../lib/stb_vorbis/stb_vorbis.c"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
