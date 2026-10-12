#pragma once
// Minimal dependency-free PNG writer (stored deflate blocks, no compression), shared by
// ui_test (render-page) and client_bin --render-scenario (client/src/ui/ui_scenario.c).
// Pure C: no GL, no client state.

// Writes `rgba` (w * h * 4 bytes, 8-bit RGBA, top row first) to `path` as an RGBA PNG.
// Returns 1 on success, 0 on allocation or I/O failure (a message goes to stderr).
int png_write_rgba(const char *path, const unsigned char *rgba, int w, int h);

// Encodes `rgba` (top row first) as a compressed RGB PNG in memory (alpha dropped). Returns
// a malloc'd buffer of *out_len bytes, NULL on allocation failure.
#include <stddef.h>
unsigned char *png_encode_rgb(const unsigned char *rgba, int w, int h, size_t *out_len);
