// Minimal PNG writer: see client/include/png_write.h. Moved from client/src/html/ui_test.c
// so client_bin (--render-scenario) and ui_test share one implementation.
#include <png_write.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t png_crc_table[256];

static uint32_t png_crc(uint32_t crc, const unsigned char *p, const size_t n) {
    if (!png_crc_table[1]) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            png_crc_table[i] = c;
        }
    }
    crc = ~crc;
    for (size_t i = 0; i < n; i++) crc = png_crc_table[(crc ^ p[i]) & 255] ^ (crc >> 8);
    return ~crc;
}

static void png_be32(unsigned char *p, const uint32_t v) {
    p[0] = (unsigned char) (v >> 24);
    p[1] = (unsigned char) (v >> 16);
    p[2] = (unsigned char) (v >> 8);
    p[3] = (unsigned char) v;
}

static void png_chunk(FILE *f, const char *type, const unsigned char *data, const size_t n) {
    unsigned char hdr[8];
    png_be32(hdr, (uint32_t) n);
    memcpy(hdr + 4, type, 4);
    fwrite(hdr, 1, 8, f);
    if (n) fwrite(data, 1, n, f);
    uint32_t crc = png_crc(0, (const unsigned char *) type, 4);
    crc = png_crc(crc, data, n);
    unsigned char c[4];
    png_be32(c, crc);
    fwrite(c, 1, 4, f);
}

int png_write_rgba(const char *path, const unsigned char *rgba, const int w, const int h) {
    const size_t row = (size_t) w * 4 + 1;  // filter byte + pixels
    const size_t raw_len = row * (size_t) h;
    const size_t nblocks = (raw_len + 65534) / 65535;
    const size_t z_len = 2 + raw_len + nblocks * 5 + 4;
    unsigned char *z = malloc(z_len);
    unsigned char *raw = malloc(raw_len);
    if (!z || !raw) {
        free(z);
        free(raw);
        return 0;
    }
    for (int y = 0; y < h; y++) {
        raw[(size_t) y * row] = 0;
        memcpy(raw + (size_t) y * row + 1, rgba + (size_t) y * w * 4, (size_t) w * 4);
    }
    size_t o = 0;
    z[o++] = 0x78;
    z[o++] = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_len; i++) {
        a = (a + raw[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    for (size_t off = 0; off < raw_len; off += 65535) {
        const size_t n = raw_len - off < 65535 ? raw_len - off : 65535;
        z[o++] = off + n >= raw_len ? 1 : 0;
        z[o++] = (unsigned char) (n & 255);
        z[o++] = (unsigned char) (n >> 8);
        z[o++] = (unsigned char) (~n & 255);
        z[o++] = (unsigned char) ((~n >> 8) & 255);
        memcpy(z + o, raw + off, n);
        o += n;
    }
    png_be32(z + o, (b << 16) | a);
    o += 4;
    free(raw);

    FILE *f = fopen(path, "wb");
    if (!f) {
        free(z);
        fprintf(stderr, "png_write: cannot write %s\n", path);
        return 0;
    }
    static const unsigned char sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    fwrite(sig, 1, 8, f);
    unsigned char ihdr[13];
    png_be32(ihdr, (uint32_t) w);
    png_be32(ihdr + 4, (uint32_t) h);
    ihdr[8] = 8;   // bit depth
    ihdr[9] = 6;   // RGBA
    ihdr[10] = 0;
    ihdr[11] = 0;
    ihdr[12] = 0;
    png_chunk(f, "IHDR", ihdr, 13);
    png_chunk(f, "IDAT", z, o);
    png_chunk(f, "IEND", NULL, 0);
    free(z);
    const int ok = !ferror(f);
    fclose(f);
    return ok;
}

// ---------------------------------------------------------------------------
// In-memory PNG with real compression (the editor bus renders travel as base64): RGB,
// "Up" row filter, deflate with LZ77 (hash chains, 32 KiB window) and the fixed Huffman
// code. Several times smaller than the stored blocks above for map renders.

typedef struct {
    unsigned char *buf;
    size_t len, cap;
    uint64_t bits;
    int nbits;
    int failed;
} PngBits;

static void pb_byte(PngBits *b, const unsigned char v) {
    if (b->len == b->cap) {
        const size_t cap = b->cap ? b->cap * 2 : 65536;
        unsigned char *n = realloc(b->buf, cap);
        if (!n) {
            b->failed = 1;
            return;
        }
        b->buf = n;
        b->cap = cap;
    }
    b->buf[b->len++] = v;
}

static void pb_put(PngBits *b, const uint32_t v, const int n) { // LSB first
    b->bits |= (uint64_t) v << b->nbits;
    b->nbits += n;
    while (b->nbits >= 8) {
        pb_byte(b, (unsigned char) b->bits);
        b->bits >>= 8;
        b->nbits -= 8;
    }
}

static void pb_huff(PngBits *b, const uint32_t code, const int n) { // Huffman codes go MSB first
    uint32_t r = 0;
    for (int i = 0; i < n; i++) r |= ((code >> i) & 1u) << (n - 1 - i);
    pb_put(b, r, n);
}

static void pb_lit(PngBits *b, const int v) {
    if (v < 144) pb_huff(b, 0x30u + (uint32_t) v, 8);
    else if (v < 256) pb_huff(b, 0x190u + (uint32_t) (v - 144), 9);
    else if (v < 280) pb_huff(b, (uint32_t) (v - 256), 7);
    else pb_huff(b, 0xC0u + (uint32_t) (v - 280), 8);
}

static const unsigned short k_len_base[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const unsigned char k_len_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const unsigned short k_dist_base[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const unsigned char k_dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static void pb_match(PngBits *b, const int len, const int dist) {
    int li = 28;
    while (k_len_base[li] > len) li--;
    pb_lit(b, 257 + li);
    if (k_len_extra[li]) pb_put(b, (uint32_t) (len - k_len_base[li]), k_len_extra[li]);
    int di = 29;
    while (k_dist_base[di] > dist) di--;
    pb_huff(b, (uint32_t) di, 5);
    if (k_dist_extra[di]) pb_put(b, (uint32_t) (dist - k_dist_base[di]), k_dist_extra[di]);
}

#define PNG_HASH_BITS 15
#define PNG_WINDOW 32768
#define PNG_CHAIN 24

static void deflate_fixed(PngBits *b, const unsigned char *data, const size_t n) {
    int *head = malloc(sizeof(int) << PNG_HASH_BITS);
    int *prev = malloc(sizeof(int) * PNG_WINDOW);
    if (!head || !prev) {
        free(head);
        free(prev);
        b->failed = 1;
        return;
    }
    for (size_t i = 0; i < (1u << PNG_HASH_BITS); i++) head[i] = -1;
    pb_put(b, 1, 1); // BFINAL
    pb_put(b, 1, 2); // BTYPE = fixed Huffman
    size_t i = 0;
#define PNG_HASH(p) ((((uint32_t) (p)[0] << 10) ^ ((uint32_t) (p)[1] << 5) ^ (p)[2]) & ((1u << PNG_HASH_BITS) - 1))
    while (i < n) {
        int best_len = 0, best_dist = 0;
        if (i + 3 <= n) {
            const uint32_t h = PNG_HASH(data + i);
            int cand = head[h];
            const size_t max_len = n - i < 258 ? n - i : 258;
            for (int chain = 0; cand >= 0 && chain < PNG_CHAIN; chain++) {
                const size_t dist = i - (size_t) cand;
                if (dist > PNG_WINDOW - 1) break;
                size_t l = 0;
                while (l < max_len && data[cand + l] == data[i + l]) l++;
                if ((int) l > best_len) {
                    best_len = (int) l;
                    best_dist = (int) dist;
                    if (l == max_len) break;
                }
                cand = prev[cand & (PNG_WINDOW - 1)];
            }
        }
        const size_t step = best_len >= 3 ? (size_t) best_len : 1;
        if (best_len >= 3) pb_match(b, best_len, best_dist);
        else pb_lit(b, data[i]);
        for (size_t k = 0; k < step; k++, i++) {
            if (i + 3 <= n) {
                const uint32_t h = PNG_HASH(data + i);
                prev[i & (PNG_WINDOW - 1)] = head[h];
                head[h] = (int) i;
            }
        }
    }
#undef PNG_HASH
    pb_lit(b, 256);
    if (b->nbits) pb_put(b, 0, 8 - b->nbits);
    free(head);
    free(prev);
}

static void mem_chunk(PngBits *out, const char *type, const unsigned char *data, const size_t n) {
    unsigned char hdr[8];
    png_be32(hdr, (uint32_t) n);
    memcpy(hdr + 4, type, 4);
    for (int i = 0; i < 8; i++) pb_byte(out, hdr[i]);
    for (size_t i = 0; i < n; i++) pb_byte(out, data[i]);
    uint32_t crc = png_crc(0, (const unsigned char *) type, 4);
    crc = png_crc(crc, data, n);
    unsigned char c[4];
    png_be32(c, crc);
    for (int i = 0; i < 4; i++) pb_byte(out, c[i]);
}

unsigned char *png_encode_rgb(const unsigned char *rgba, const int w, const int h, size_t *out_len) {
    *out_len = 0;
    const size_t row = (size_t) w * 3 + 1;
    const size_t raw_len = row * (size_t) h;
    unsigned char *raw = malloc(raw_len ? raw_len : 1);
    if (!raw) return NULL;
    for (int y = 0; y < h; y++) {
        unsigned char *r = raw + (size_t) y * row;
        r[0] = 2; // Up
        const unsigned char *src = rgba + (size_t) y * w * 4, *above = y ? rgba + (size_t) (y - 1) * w * 4 : NULL;
        for (int x = 0; x < w; x++) {
            for (int c = 0; c < 3; c++) r[1 + x * 3 + c] = (unsigned char) (src[x * 4 + c] - (above ? above[x * 4 + c] : 0));
        }
    }
    PngBits z = {0};
    pb_byte(&z, 0x78);
    pb_byte(&z, 0x01);
    deflate_fixed(&z, raw, raw_len);
    uint32_t a = 1, bsum = 0;
    for (size_t i = 0; i < raw_len; i++) {
        a = (a + raw[i]) % 65521u;
        bsum = (bsum + a) % 65521u;
    }
    free(raw);
    unsigned char ad[4];
    png_be32(ad, (bsum << 16) | a);
    for (int i = 0; i < 4; i++) pb_byte(&z, ad[i]);
    if (z.failed) {
        free(z.buf);
        return NULL;
    }
    PngBits out = {0};
    static const unsigned char sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    for (int i = 0; i < 8; i++) pb_byte(&out, sig[i]);
    unsigned char ihdr[13];
    png_be32(ihdr, (uint32_t) w);
    png_be32(ihdr + 4, (uint32_t) h);
    ihdr[8] = 8;
    ihdr[9] = 2; // RGB
    ihdr[10] = ihdr[11] = ihdr[12] = 0;
    mem_chunk(&out, "IHDR", ihdr, 13);
    mem_chunk(&out, "IDAT", z.buf, z.len);
    mem_chunk(&out, "IEND", NULL, 0);
    free(z.buf);
    if (out.failed) {
        free(out.buf);
        return NULL;
    }
    *out_len = out.len;
    return out.buf;
}
