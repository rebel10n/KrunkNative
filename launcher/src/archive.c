// Package extraction (launcher/include/kn_archive.h): .tar.gz and .zip.
#include <kn_archive.h>
#include <kn_fs.h>
#include <kn_update.h> // kn_update_path_ok

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// stb_image's zlib decoder (client/src/stb_image.c, STB_IMAGE_IMPLEMENTATION)
int stbi_zlib_decode_noheader_buffer(char *obuffer, int olen, const char *ibuffer, int ilen);

#define ARCHIVE_MAX (1u << 30) // 1 GiB: an executable package is far smaller

typedef enum { ENT_FILE, ENT_DIR, ENT_SYMLINK } EntType;

typedef struct {
    char *name;            // as stored ("./" and the top-level dir not yet stripped)
    EntType type;
    int exec;
    const unsigned char *data; // tar: the content; zip: the compressed data
    size_t size;               // tar: content size; zip: compressed size
    size_t usize;              // zip: uncompressed size
    int method;                // zip: 0 stored, 8 deflate; tar: -1
    char *link;                // symlink target
} Entry;

typedef struct {
    Entry *v;
    size_t n, cap;
} Entries;

static void set_err(char *err, const size_t errn, const char *msg, const char *arg) {
    if (err && errn) snprintf(err, errn, "%s%s%s", msg, arg ? ": " : "", arg ? arg : "");
}

static int push(Entries *e, Entry x) {
    if (e->n == e->cap) {
        const size_t cap = e->cap ? e->cap * 2 : 64;
        Entry *grown = realloc(e->v, cap * sizeof(*grown));
        if (!grown) return 0;
        e->v = grown;
        e->cap = cap;
    }
    e->v[e->n++] = x;
    return 1;
}

static void entries_free(Entries *e) {
    for (size_t i = 0; i < e->n; i++) {
        free(e->v[i].name);
        free(e->v[i].link);
    }
    free(e->v);
}

static char *dup_n(const char *s, const size_t n) {
    char *d = malloc(n + 1);
    if (!d) return NULL;
    memcpy(d, s, n);
    d[n] = 0;
    return d;
}

// ------------------------------------------------------------------ tar

static uint64_t tar_num(const unsigned char *p, const size_t n) {
    if (p[0] & 0x80) { // GNU base-256
        uint64_t v = p[0] & 0x7f;
        for (size_t i = 1; i < n; i++) v = v << 8 | p[i];
        return v;
    }
    uint64_t v = 0;
    size_t i = 0;
    while (i < n && (p[i] == ' ' || p[i] == 0)) i++;
    for (; i < n && p[i] >= '0' && p[i] <= '7'; i++) v = v * 8 + (uint64_t) (p[i] - '0');
    return v;
}

// pax records "len key=value\n": path / linkpath
static void pax_get(const unsigned char *d, const size_t n, const char *key, char **out) {
    size_t i = 0;
    const size_t kl = strlen(key);
    while (i < n) {
        size_t len = 0, j = i;
        while (j < n && d[j] >= '0' && d[j] <= '9') len = len * 10 + (size_t) (d[j++] - '0');
        if (!len || i + len > n || j >= n || d[j] != ' ') return;
        const char *rec = (const char *) d + j + 1;
        const size_t rl = i + len - (j + 1);
        if (rl > kl + 1 && !memcmp(rec, key, kl) && rec[kl] == '=') {
            free(*out);
            *out = dup_n(rec + kl + 1, rl - kl - 2); // minus "=" and "\n"
        }
        i += len;
    }
}

static int parse_tar(const unsigned char *d, const size_t n, Entries *out, char *err, const size_t errn) {
    size_t off = 0;
    char *long_name = NULL, *long_link = NULL;
    while (off + 512 <= n) {
        const unsigned char *h = d + off;
        int zero = 1;
        for (int i = 0; i < 512 && zero; i++) zero = !h[i];
        if (zero) break;
        const uint64_t size = tar_num(h + 124, 12);
        const char type = (char) h[156];
        off += 512;
        if (size > n - off) {
            set_err(err, errn, "truncated tar", NULL);
            free(long_name);
            free(long_link);
            return 0;
        }
        const unsigned char *data = d + off;
        off += (size_t) ((size + 511) / 512 * 512);
        if (type == 'L') { free(long_name); long_name = dup_n((const char *) data, strnlen((const char *) data, size)); continue; }
        if (type == 'K') { free(long_link); long_link = dup_n((const char *) data, strnlen((const char *) data, size)); continue; }
        if (type == 'x') { pax_get(data, size, "path", &long_name); pax_get(data, size, "linkpath", &long_link); continue; }
        if (type == 'g') continue;
        char *name = long_name;
        long_name = NULL;
        if (!name) {
            char tmp[256 + 2];
            const size_t nl = strnlen((const char *) h, 100);
            const size_t pl = !memcmp(h + 257, "ustar", 5) ? strnlen((const char *) h + 345, 155) : 0;
            if (pl) snprintf(tmp, sizeof(tmp), "%.*s/%.*s", (int) pl, h + 345, (int) nl, h);
            else snprintf(tmp, sizeof(tmp), "%.*s", (int) nl, h);
            name = strdup(tmp);
        }
        char *link = long_link;
        long_link = NULL;
        Entry e = {.name = name, .method = -1, .data = data, .size = (size_t) size};
        e.exec = (tar_num(h + 100, 8) & 0111) != 0;
        if (type == '0' || type == 0 || type == '7') e.type = ENT_FILE;
        else if (type == '5') e.type = ENT_DIR;
        else if (type == '2') {
            e.type = ENT_SYMLINK;
            e.link = link ? link : dup_n((const char *) h + 157, strnlen((const char *) h + 157, 100));
            link = NULL;
        } else { // hard links, devices, fifos: not in our packages
            free(name);
            free(link);
            continue;
        }
        free(link);
        if (!e.name || (e.type == ENT_SYMLINK && !e.link) || !push(out, e)) {
            free(e.name);
            free(e.link);
            set_err(err, errn, "out of memory", NULL);
            free(long_name);
            free(long_link);
            return 0;
        }
    }
    free(long_name);
    free(long_link);
    return 1;
}

static unsigned char *gunzip(const unsigned char *d, const size_t n, size_t *out_len, char *err, const size_t errn) {
    if (n < 18 || d[0] != 0x1f || d[1] != 0x8b || d[2] != 8) {
        set_err(err, errn, "not a gzip file", NULL);
        return NULL;
    }
    const unsigned flags = d[3];
    size_t p = 10;
    if (flags & 4) { // FEXTRA
        if (p + 2 > n) return NULL;
        p += 2 + (size_t) (d[p] | d[p + 1] << 8);
    }
    if (flags & 8) while (p < n && d[p++]) {}  // FNAME
    if (flags & 16) while (p < n && d[p++]) {} // FCOMMENT
    if (flags & 2) p += 2;                     // FHCRC
    if (p + 8 > n) {
        set_err(err, errn, "truncated gzip", NULL);
        return NULL;
    }
    const uint32_t isize = (uint32_t) d[n - 4] | (uint32_t) d[n - 3] << 8 | (uint32_t) d[n - 2] << 16 | (uint32_t) d[n - 1] << 24;
    if (isize == 0 || isize > ARCHIVE_MAX || n - 8 - p > INT32_MAX) {
        set_err(err, errn, "gzip too large or empty", NULL);
        return NULL;
    }
    unsigned char *out = malloc(isize);
    if (!out) {
        set_err(err, errn, "out of memory", NULL);
        return NULL;
    }
    const int got = stbi_zlib_decode_noheader_buffer((char *) out, (int) isize, (const char *) d + p, (int) (n - 8 - p));
    if (got != (int) isize) {
        free(out);
        set_err(err, errn, "corrupt gzip data", NULL);
        return NULL;
    }
    *out_len = isize;
    return out;
}

// ------------------------------------------------------------------ zip

static uint32_t le32(const unsigned char *p) { return (uint32_t) p[0] | (uint32_t) p[1] << 8 | (uint32_t) p[2] << 16 | (uint32_t) p[3] << 24; }
static uint16_t le16(const unsigned char *p) { return (uint16_t) (p[0] | p[1] << 8); }

static int parse_zip(const unsigned char *d, const size_t n, Entries *out, char *err, const size_t errn) {
    if (n < 22) {
        set_err(err, errn, "not a zip file", NULL);
        return 0;
    }
    size_t eocd = n - 22;
    const size_t min = n > 22 + 65535 ? n - 22 - 65535 : 0;
    while (le32(d + eocd) != 0x06054b50) {
        if (eocd == min) {
            set_err(err, errn, "zip: no end of central directory", NULL);
            return 0;
        }
        eocd--;
    }
    const size_t count = le16(d + eocd + 10);
    size_t cd = le32(d + eocd + 16);
    for (size_t i = 0; i < count; i++) {
        if (cd + 46 > n || le32(d + cd) != 0x02014b50) {
            set_err(err, errn, "zip: bad central directory", NULL);
            return 0;
        }
        const unsigned made_by = le16(d + cd + 4) >> 8;
        const unsigned flags = le16(d + cd + 8);
        const unsigned method = le16(d + cd + 10);
        const size_t csize = le32(d + cd + 20), usize = le32(d + cd + 24);
        const size_t nl = le16(d + cd + 28), xl = le16(d + cd + 30), cl = le16(d + cd + 32);
        const uint32_t attrs = le32(d + cd + 38);
        const size_t local = le32(d + cd + 42);
        if (cd + 46 + nl > n || local + 30 > n || le32(d + local) != 0x04034b50) {
            set_err(err, errn, "zip: bad entry", NULL);
            return 0;
        }
        if (flags & 1) {
            set_err(err, errn, "zip: encrypted entry", NULL);
            return 0;
        }
        if (csize == 0xffffffffu || usize == 0xffffffffu) {
            set_err(err, errn, "zip: zip64 is not supported", NULL);
            return 0;
        }
        const size_t data_off = local + 30 + le16(d + local + 26) + le16(d + local + 28);
        if (data_off > n || csize > n - data_off || usize > ARCHIVE_MAX) {
            set_err(err, errn, "zip: entry out of range", NULL);
            return 0;
        }
        Entry e = {.name = dup_n((const char *) d + cd + 46, nl), .data = d + data_off, .size = csize, .usize = usize,
                   .method = (int) method};
        const unsigned mode = made_by == 3 ? attrs >> 16 : 0;
        if (nl && d[cd + 46 + nl - 1] == '/') e.type = ENT_DIR;
        else if ((mode & 0170000) == 0120000) e.type = ENT_SYMLINK;
        else e.type = ENT_FILE;
        e.exec = (mode & 0111) != 0;
        if (!e.name || (method != 0 && method != 8) || !push(out, e)) {
            free(e.name);
            set_err(err, errn, method != 0 && method != 8 ? "zip: unsupported compression" : "out of memory", NULL);
            return 0;
        }
        cd += 46 + nl + xl + cl;
    }
    return 1;
}

// zip entry content (malloc'd), NULL on corrupt data
static unsigned char *zip_content(const Entry *e) {
    unsigned char *out = malloc(e->usize ? e->usize : 1);
    if (!out) return NULL;
    if (e->method == 0) {
        if (e->size != e->usize) {
            free(out);
            return NULL;
        }
        memcpy(out, e->data, e->size);
        return out;
    }
    if (e->size > INT32_MAX || e->usize > INT32_MAX) {
        free(out);
        return NULL;
    }
    const int got = e->usize ? stbi_zlib_decode_noheader_buffer((char *) out, (int) e->usize, (const char *) e->data, (int) e->size) : 0;
    if (got != (int) e->usize) {
        free(out);
        return NULL;
    }
    return out;
}

// ------------------------------------------------------------------ extraction

// "./a/b" -> "a/b"; trailing '/' removed
static void clean_name(char *s) {
    while (s[0] == '.' && s[1] == '/') memmove(s, s + 2, strlen(s + 2) + 1);
    size_t l = strlen(s);
    while (l && s[l - 1] == '/') s[--l] = 0;
}

// The symlink target stays inside the tree: dirname(rel) + target never climbs above the root.
static int link_ok(const char *rel, const char *target) {
    if (!target[0] || target[0] == '/' || strchr(target, '\\') || strchr(target, ':')) return 0;
    int depth = 0;
    for (const char *p = rel; *p; p++)
        if (*p == '/') depth++;
    const char *seg = target;
    for (const char *p = target;; p++) {
        if (*p == '/' || !*p) {
            const size_t sl = (size_t) (p - seg);
            if (sl == 2 && seg[0] == '.' && seg[1] == '.') {
                if (--depth < 0) return 0;
            } else if (sl && !(sl == 1 && seg[0] == '.')) {
                depth++;
            }
            if (!*p) break;
            seg = p + 1;
        }
    }
    return 1;
}

static int extract_entries(Entries *ents, const char *dest, char *err, const size_t errn) {
    for (size_t i = 0; i < ents->n; i++) {
        clean_name(ents->v[i].name);
        if (ents->v[i].name[0] && !kn_update_path_ok(ents->v[i].name)) { // before any stripping
            set_err(err, errn, "unsafe path in package", ents->v[i].name);
            return 0;
        }
    }
    // the single top-level directory every entry shares (krunknative-client-<plat>/, X.app/)
    size_t strip = 0;
    if (ents->n) {
        const char *first = ents->v[0].name;
        const char *slash = strchr(first, '/');
        const size_t top = slash ? (size_t) (slash - first) : strlen(first);
        int shared = top > 0;
        for (size_t i = 0; shared && i < ents->n; i++) {
            const char *nm = ents->v[i].name;
            if (strncmp(nm, first, top) != 0 || (nm[top] != '/' && nm[top] != 0)) shared = 0;
            else if (!nm[top] && ents->v[i].type != ENT_DIR) shared = 0;
        }
        // a package of one plain directory: strip it; a lone file at the top: keep
        if (shared) {
            int has_child = 0;
            for (size_t i = 0; i < ents->n && !has_child; i++) has_child = ents->v[i].name[top] == '/';
            if (has_child) strip = top + 1;
        }
    }
    if (!kn_fs_mkdirs(dest)) {
        set_err(err, errn, "cannot create", dest);
        return 0;
    }
    for (size_t i = 0; i < ents->n; i++) {
        const Entry *e = &ents->v[i];
        if (strlen(e->name) < strip) continue; // the stripped directory itself
        const char *rel = e->name + strip;
        if (!rel[0]) continue;
        if (!kn_update_path_ok(rel)) {
            set_err(err, errn, "unsafe path in package", e->name);
            return 0;
        }
        char path[4096];
        if (!kn_fs_join(path, sizeof(path), dest, rel)) {
            set_err(err, errn, "path too long", rel);
            return 0;
        }
        if (e->type == ENT_DIR) {
            if (!kn_fs_mkdirs(path)) {
                set_err(err, errn, "cannot create", path);
                return 0;
            }
            continue;
        }
        if (!kn_fs_mkdirs_parent(path)) {
            set_err(err, errn, "cannot create the directory of", path);
            return 0;
        }
        if (e->type == ENT_SYMLINK) {
            const unsigned char *raw = NULL;
            char *target = NULL;
            if (e->link) target = strdup(e->link);
            else if ((raw = zip_content(e))) target = dup_n((const char *) raw, e->usize);
            free((void *) raw);
            const int ok = target && link_ok(rel, target);
#ifndef _WIN32
            if (ok) {
                kn_fs_remove(path);
                if (!kn_fs_symlink(target, path)) {
                    free(target);
                    set_err(err, errn, "cannot create symlink", path);
                    return 0;
                }
            }
#endif
            if (!ok) {
                set_err(err, errn, "unsafe symlink in package", e->name);
                free(target);
                return 0;
            }
            free(target);
            continue;
        }
        unsigned char *content = NULL;
        const unsigned char *bytes = e->data;
        size_t len = e->size;
        if (e->method >= 0) {
            content = zip_content(e);
            if (!content) {
                set_err(err, errn, "corrupt entry", e->name);
                return 0;
            }
            bytes = content;
            len = e->usize;
        }
        FILE *f = kn_fs_fopen(path, "wb");
        const int ok = f && (len == 0 || fwrite(bytes, 1, len, f) == len);
        if (f && fclose(f) != 0) {
            free(content);
            set_err(err, errn, "cannot write", path);
            return 0;
        }
        free(content);
        if (!ok) {
            set_err(err, errn, "cannot write", path);
            return 0;
        }
        if (e->exec) kn_fs_set_executable(path, 1);
    }
    return 1;
}

int kn_archive_extract_mem(const unsigned char *data, const size_t len, const char *dest_dir, char *err, const size_t errn) {
    Entries ents = {0};
    unsigned char *tar = NULL;
    int ok = 0;
    if (len >= 4 && data[0] == 'P' && data[1] == 'K' && data[2] == 3 && data[3] == 4) {
        ok = parse_zip(data, len, &ents, err, errn);
    } else if (len >= 2 && data[0] == 0x1f && data[1] == 0x8b) {
        size_t tl = 0;
        tar = gunzip(data, len, &tl, err, errn);
        ok = tar && parse_tar(tar, tl, &ents, err, errn);
    } else {
        set_err(err, errn, "unknown package format (want .tar.gz or .zip)", NULL);
    }
    if (ok) ok = extract_entries(&ents, dest_dir, err, errn);
    entries_free(&ents);
    free(tar);
    return ok;
}

int kn_archive_extract(const char *archive_path, const char *dest_dir, char *err, const size_t errn) {
    char *data = NULL;
    size_t len = 0;
    if (!kn_fs_read(archive_path, &data, &len, ARCHIVE_MAX)) {
        set_err(err, errn, "cannot read", archive_path);
        return 0;
    }
    const int ok = kn_archive_extract_mem((const unsigned char *) data, len, dest_dir, err, errn);
    free(data);
    return ok;
}
