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
