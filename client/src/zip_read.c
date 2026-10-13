// A small .zip reader, see client/include/zip_read.h.
#include <zip_read.h>
#include <stb_image.h>
#include <stdlib.h>
#include <string.h>

#define ZIP_LOCAL_SIG 0x04034b50u
#define ZIP_CENTRAL_SIG 0x02014b50u
#define ZIP_END_SIG 0x06054b50u
#define ZIP_NAME_MAX 512

static unsigned int le16(const unsigned char *p) { return (unsigned int) p[0] | (unsigned int) p[1] << 8; }
static unsigned int le32(const unsigned char *p) { return le16(p) | le16(p + 2) << 16; }

// one entry: `local` is its local header; method / sizes as the directory (or the header) says
static int zip_entry(const unsigned char *zip, const size_t size, const size_t local, const unsigned int method, const size_t compressed,
                     const size_t uncompressed, const char *name, const ZipEntryFn fn, void *user) {
    if (local + 30 > size || le32(zip + local) != ZIP_LOCAL_SIG) return 1;
    const size_t data_at = local + 30 + le16(zip + local + 26) + le16(zip + local + 28);
    if (data_at > size || compressed > size - data_at) return 1;
    const size_t name_length = strlen(name);
    if (!name_length || name[name_length - 1] == '/') return 1; // a directory
    if (uncompressed > ZIP_READ_ENTRY_MAX) return 1;
    if (method == 0) {
        if (compressed != uncompressed) return 1;
        return fn(name, zip + data_at, uncompressed, user);
    }
    if (method != 8 || compressed > 0x7fffffffu || uncompressed > 0x7fffffffu) return 1;
    char *out = malloc(uncompressed ? uncompressed : 1);
    if (!out) return 1;
    const int got = uncompressed ? stbi_zlib_decode_noheader_buffer(out, (int) uncompressed, (const char *) zip + data_at, (int) compressed) : 0;
    int keep = 1;
    if (got >= 0 && (size_t) got == uncompressed) keep = fn(name, (const unsigned char *) out, uncompressed, user);
    free(out);
    return keep;
}

static void entry_name(const unsigned char *p, const size_t length, char *out) {
    const size_t n = length < ZIP_NAME_MAX - 1 ? length : ZIP_NAME_MAX - 1;
    memcpy(out, p, n);
    out[n] = 0;
}

int zip_read_entries(const unsigned char *zip, const size_t size, const ZipEntryFn fn, void *user) {
    if (!zip || size < 22 || !fn) return 0;
    char name[ZIP_NAME_MAX];

    // the end of central directory record: the last 22 bytes plus up to 64 KB of comment
    size_t end = size - 22;
    const size_t stop = size - 22 > 65535 ? size - 22 - 65535 : 0;
    for (;; end--) {
        if (le32(zip + end) == ZIP_END_SIG) break;
        if (end == stop) {
            end = (size_t) -1;
            break;
        }
    }
    if (end != (size_t) -1) {
        const unsigned int count = le16(zip + end + 10);
        size_t at = le32(zip + end + 16);
        int entries = 0;
        for (unsigned int i = 0; i < count; i++) {
            if (at + 46 > size || le32(zip + at) != ZIP_CENTRAL_SIG) break;
            const unsigned int name_length = le16(zip + at + 28);
            const size_t next = at + 46 + name_length + le16(zip + at + 30) + le16(zip + at + 32);
            if (next > size) break;
            entry_name(zip + at + 46, name_length, name);
            entries++;
            if (!zip_entry(zip, size, le32(zip + at + 42), le16(zip + at + 10), le32(zip + at + 20), le32(zip + at + 24), name, fn, user))
                return entries;
            at = next;
        }
        if (entries) return entries;
    }

    // no usable central directory: walk the local headers (sizes must be in them)
    size_t at = 0;
    int entries = 0;
    while (at + 30 <= size && le32(zip + at) == ZIP_LOCAL_SIG) {
        const unsigned int flags = le16(zip + at + 6), method = le16(zip + at + 8);
        const size_t compressed = le32(zip + at + 18), uncompressed = le32(zip + at + 22);
        const unsigned int name_length = le16(zip + at + 26), extra = le16(zip + at + 28);
        const size_t data_at = at + 30 + name_length + extra;
        if (data_at > size || (flags & 8) || compressed > size - data_at) break;
        entry_name(zip + at + 30, name_length, name);
        entries++;
        if (!zip_entry(zip, size, at, method, compressed, uncompressed, name, fn, user)) break;
        at = data_at + compressed;
    }
    return entries;
}
