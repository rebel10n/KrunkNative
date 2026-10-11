#pragma once
// Minimal dependency-free PNG writer (stored deflate blocks, no compression), shared by
// ui_test (render-page) and client_bin --render-scenario (client/src/ui/ui_scenario.c).
// Pure C: no GL, no client state.

// Writes `rgba` (w * h * 4 bytes, 8-bit RGBA, top row first) to `path` as an RGBA PNG.
// Returns 1 on success, 0 on allocation or I/O failure (a message goes to stderr).
int png_write_rgba(const char *path, const unsigned char *rgba, int w, int h);
