#pragma once
// Package extraction for the launcher: .tar.gz (gzip + ustar/GNU/pax tar) and .zip (stored or
// deflate), inflated with stb_image's zlib decoder (client/src/stb_image.c). The archive is
// read into memory (executable packages are tens of MB).
//
// A single top-level directory shared by every entry (a wrapped package from before the flat
// BoxOffice layout) is stripped; a flat package (BoxOffice, runtime/) has none. Unsafe entries
// (absolute, "..", '\\', ':') fail the whole extraction; symlinks are kept only when relative and
// inside the tree (.app frameworks).
// The executable bit (tar mode, zip Unix attributes) is kept on POSIX.
#include <stddef.h>

// Extracts the archive file into dest_dir (created; must be empty or missing). 1 on success.
int kn_archive_extract(const char *archive_path, const char *dest_dir, char *err, size_t errn);
// Same, from memory.
int kn_archive_extract_mem(const unsigned char *data, size_t len, const char *dest_dir, char *err, size_t errn);
// One gzip member into memory (malloc'd, NUL not added), at most max bytes uncompressed. NULL on
// error (err says why). The gzip CRC is not checked: callers verify a sha256 of the result.
unsigned char *kn_gunzip_mem(const unsigned char *data, size_t len, size_t max, size_t *out_len, char *err, size_t errn);
