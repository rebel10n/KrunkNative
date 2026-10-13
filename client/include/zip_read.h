#pragma once
// A small .zip reader over stb_image's inflate (client/src/zip_read.c), for the zip.js uses of
// Krunker v7.1.4: mod packs (resource packs, client/src/mod_pack.c, and the viewer's mod import,
// client/src/viewer/viewer_import.c). Stored and deflated entries; sizes and offsets come from
// the central directory (entries written with a data descriptor work), else from the local
// headers of a zip without one. No ZIP64, encryption or multi-disk archives.
#include <stddef.h>

#define ZIP_READ_ENTRY_MAX (256u << 20) // an entry inflating past 256 MB is skipped (a zip bomb)

// fn(name, data, size, user) for every file entry in archive order (directories are counted but
// not passed); `data` is the uncompressed content, valid during the call. Entries that cannot be
// read (unknown method, corrupt data) are skipped. fn returning 0 stops the walk. Returns the
// number of entries seen (directories included), 0 when `zip` is not a zip archive.
typedef int (*ZipEntryFn)(const char *name, const unsigned char *data, size_t size, void *user);
int zip_read_entries(const unsigned char *zip, size_t size, ZipEntryFn fn, void *user);
