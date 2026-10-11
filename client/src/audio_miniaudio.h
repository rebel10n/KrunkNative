#pragma once
// miniaudio build options, shared by the implementation unit (miniaudio.c) and its users
// (audio.c) so both see the same struct layouts. See client/lib/miniaudio/README.md.
#define MA_NO_FLAC
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#include "../lib/miniaudio/miniaudio.h"
