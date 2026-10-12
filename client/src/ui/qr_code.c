// QR Code encoder (qr_code.h). The algorithm follows ISO/IEC 18004 as laid out by Project
// Nayuki's reference encoder: byte-mode segment, Reed-Solomon over GF(256)/0x11D, block
// interleaving, zigzag placement, the eight masks with the four penalty rules.
#include "qr_code.h"

#include <stdio.h>
#include <string.h>

// [ecc][version]: error correction codewords per block, number of blocks (index 0 unused)
static const signed char ECC_PER_BLOCK[2][QR_MAX_VERSION + 1] = {
    {-1, 7, 10, 15, 20, 26, 18, 20, 24, 30, 18},
    {-1, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26},
};
static const signed char NUM_BLOCKS[2][QR_MAX_VERSION + 1] = {
    {-1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 4},
    {-1, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5},
};

#define MAX_CODEWORDS 346 // version 10

typedef struct Grid {
    int size;
    unsigned char dark[QR_MAX_SIZE][QR_MAX_SIZE];
    unsigned char func[QR_MAX_SIZE][QR_MAX_SIZE];
} Grid;

static int raw_modules(const int ver) {
    int r = (16 * ver + 128) * ver + 64;
    if (ver >= 2) {
        const int n = ver / 7 + 2;
        r -= (25 * n - 10) * n - 55;
        if (ver >= 7) r -= 36;
    }
    return r;
}

static int data_codewords(const int ver, const QrEcc ecc) {
    return raw_modules(ver) / 8 - ECC_PER_BLOCK[ecc][ver] * NUM_BLOCKS[ecc][ver];
}

static int align_positions(const int ver, int out[7]) {
    if (ver == 1) return 0;
    const int n = ver / 7 + 2;
    const int step = (ver * 4 + n * 2 + 1) / (n * 2 - 2) * 2;
    out[0] = 6;
    for (int i = n - 1, pos = ver * 4 + 10; i >= 1; i--, pos -= step) out[i] = pos;
    return n;
}

static void set_func(Grid *g, const int x, const int y, const int dark) {
    g->dark[y][x] = (unsigned char) dark;
    g->func[y][x] = 1;
}

static void draw_finder(Grid *g, const int cx, const int cy) {
    for (int dy = -4; dy <= 4; dy++) {
        for (int dx = -4; dx <= 4; dx++) {
            const int x = cx + dx, y = cy + dy;
            if (x < 0 || y < 0 || x >= g->size || y >= g->size) continue;
            const int d = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy) ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy);
            set_func(g, x, y, d != 2 && d != 4);
        }
    }
}

static void draw_alignment(Grid *g, const int cx, const int cy) {
    for (int dy = -2; dy <= 2; dy++)
        for (int dx = -2; dx <= 2; dx++) {
            const int d = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy) ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy);
            set_func(g, cx + dx, cy + dy, d != 1);
        }
}

static void draw_format(Grid *g, const QrEcc ecc, const int mask) {
    const int data = (ecc == QR_ECC_L ? 1 : 0) << 3 | mask; // format ECC bits: L 01, M 00
    int rem = data;
    for (int i = 0; i < 10; i++) rem = (rem << 1) ^ ((rem >> 9) * 0x537);
    const int bits = (data << 10 | rem) ^ 0x5412;
    const int n = g->size;
    for (int i = 0; i <= 5; i++) set_func(g, 8, i, bits >> i & 1);
    set_func(g, 8, 7, bits >> 6 & 1);
    set_func(g, 8, 8, bits >> 7 & 1);
    set_func(g, 7, 8, bits >> 8 & 1);
    for (int i = 9; i < 15; i++) set_func(g, 14 - i, 8, bits >> i & 1);
    for (int i = 0; i < 8; i++) set_func(g, n - 1 - i, 8, bits >> i & 1);
    for (int i = 8; i < 15; i++) set_func(g, 8, n - 15 + i, bits >> i & 1);
    set_func(g, 8, n - 8, 1); // the dark module
}

static void draw_version(Grid *g, const int ver) {
    if (ver < 7) return;
    int rem = ver;
    for (int i = 0; i < 12; i++) rem = (rem << 1) ^ ((rem >> 11) * 0x1F25);
    const long bits = (long) ver << 12 | rem;
    for (int i = 0; i < 18; i++) {
        const int bit = (int) (bits >> i & 1);
        const int a = g->size - 11 + i % 3, b = i / 3;
        set_func(g, a, b, bit);
        set_func(g, b, a, bit);
    }
}

static void draw_function_patterns(Grid *g, const int ver, const QrEcc ecc) {
    const int n = g->size;
    for (int i = 0; i < n; i++) {
        set_func(g, 6, i, i % 2 == 0);
        set_func(g, i, 6, i % 2 == 0);
    }
    draw_finder(g, 3, 3);
    draw_finder(g, n - 4, 3);
    draw_finder(g, 3, n - 4);
    int pos[7];
    const int na = align_positions(ver, pos);
    for (int i = 0; i < na; i++)
        for (int j = 0; j < na; j++)
            if (!((i == 0 && j == 0) || (i == 0 && j == na - 1) || (i == na - 1 && j == 0))) draw_alignment(g, pos[i], pos[j]);
    draw_format(g, ecc, 0); // placeholder, redrawn with the real mask
    draw_version(g, ver);
}

static int gf_mul(const int x, const int y) {
    int z = 0;
    for (int i = 7; i >= 0; i--) {
        z = (z << 1) ^ ((z >> 7) * 0x11D);
        z ^= ((y >> i) & 1) * x;
    }
    return z & 0xFF;
}

static void rs_divisor(const int degree, unsigned char *out) {
    memset(out, 0, (size_t) degree);
    out[degree - 1] = 1;
    int root = 1;
    for (int i = 0; i < degree; i++) {
        for (int j = 0; j < degree; j++) {
            out[j] = (unsigned char) gf_mul(out[j], root);
            if (j + 1 < degree) out[j] ^= out[j + 1];
        }
        root = gf_mul(root, 0x02);
    }
}

static void rs_remainder(const unsigned char *data, const int len, const unsigned char *div, const int degree, unsigned char *out) {
    memset(out, 0, (size_t) degree);
    for (int k = 0; k < len; k++) {
        const int factor = data[k] ^ out[0];
        memmove(out, out + 1, (size_t) degree - 1);
        out[degree - 1] = 0;
        for (int i = 0; i < degree; i++) out[i] ^= (unsigned char) gf_mul(div[i], factor);
    }
}

// Data codewords -> the final interleaved sequence with error correction.
static int add_ecc_interleave(const unsigned char *data, const int ver, const QrEcc ecc, unsigned char *out) {
    const int nblocks = NUM_BLOCKS[ecc][ver], ecc_len = ECC_PER_BLOCK[ecc][ver];
    const int raw = raw_modules(ver) / 8;
    const int nshort = nblocks - raw % nblocks, short_len = raw / nblocks;
    unsigned char blocks[8][MAX_CODEWORDS / 4 + 32];
    unsigned char div[32];
    rs_divisor(ecc_len, div);
    for (int i = 0, k = 0; i < nblocks; i++) {
        const int dlen = short_len - ecc_len + (i < nshort ? 0 : 1);
        memset(blocks[i], 0, sizeof blocks[i]);
        memcpy(blocks[i], data + k, (size_t) dlen);
        k += dlen;
        rs_remainder(data + k - dlen, dlen, div, ecc_len, blocks[i] + short_len + 1 - ecc_len);
    }
    int n = 0;
    for (int i = 0; i < short_len + 1; i++)
        for (int j = 0; j < nblocks; j++)
            if (i != short_len - ecc_len || j >= nshort) out[n++] = blocks[j][i];
    return n == raw ? n : -1;
}

static void draw_codewords(Grid *g, const unsigned char *cw, const int len) {
    int i = 0;
    const int n = g->size;
    for (int right = n - 1; right >= 1; right -= 2) {
        if (right == 6) right = 5;
        for (int vert = 0; vert < n; vert++) {
            for (int j = 0; j < 2; j++) {
                const int x = right - j;
                const int up = ((right + 1) & 2) == 0;
                const int y = up ? n - 1 - vert : vert;
                if (!g->func[y][x] && i < len * 8) {
                    g->dark[y][x] = cw[i >> 3] >> (7 - (i & 7)) & 1;
                    i++;
                }
            }
        }
    }
}

static int mask_bit(const int m, const int x, const int y) {
    switch (m) {
    case 0: return (x + y) % 2 == 0;
    case 1: return y % 2 == 0;
    case 2: return x % 3 == 0;
    case 3: return (x + y) % 3 == 0;
    case 4: return (x / 3 + y / 2) % 2 == 0;
    case 5: return x * y % 2 + x * y % 3 == 0;
    case 6: return (x * y % 2 + x * y % 3) % 2 == 0;
    default: return ((x + y) % 2 + x * y % 3) % 2 == 0;
    }
}

static void apply_mask(Grid *g, const int m) {
    for (int y = 0; y < g->size; y++)
        for (int x = 0; x < g->size; x++)
            if (!g->func[y][x] && mask_bit(m, x, y)) g->dark[y][x] ^= 1;
}

static int at(const Grid *g, const int x, const int y, const int col) { return col ? g->dark[x][y] : g->dark[y][x]; }

static long penalty(const Grid *g) {
    const int n = g->size;
    long p = 0;
    for (int col = 0; col < 2; col++) {
        for (int y = 0; y < n; y++) {
            int run = 1;
            for (int x = 1; x <= n; x++) {
                if (x < n && at(g, x, y, col) == at(g, x - 1, y, col)) {
                    run++;
                    continue;
                }
                if (run >= 5) p += 3 + (run - 5);
                run = 1;
            }
            // 1:1:3:1:1 finder-like runs with four light modules on one side
            for (int x = 0; x + 11 <= n; x++) {
                static const unsigned char a[11] = {1, 0, 1, 1, 1, 0, 1, 0, 0, 0, 0};
                static const unsigned char b[11] = {0, 0, 0, 0, 1, 0, 1, 1, 1, 0, 1};
                int ma = 1, mb = 1;
                for (int k = 0; k < 11; k++) {
                    const int v = at(g, x + k, y, col);
                    ma &= v == a[k];
                    mb &= v == b[k];
                }
                p += 40 * (ma + mb);
            }
        }
    }
    int dark = 0;
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            dark += g->dark[y][x];
            if (x + 1 < n && y + 1 < n) {
                const int c = g->dark[y][x];
                if (c == g->dark[y][x + 1] && c == g->dark[y + 1][x] && c == g->dark[y + 1][x + 1]) p += 3;
            }
        }
    const int total = n * n;
    int dev = dark * 20 - total * 10;
    if (dev < 0) dev = -dev;
    const int k = (dev + total - 1) / total - 1;
    return p + (long) k * 10;
}

int qr_encode(const unsigned char *data, const size_t len, const QrEcc ecc, const int mask, QrCode *out) {
    if (!out || (ecc != QR_ECC_L && ecc != QR_ECC_M) || mask < -1 || mask > 7) return -1;
    int ver = 1;
    for (; ver <= QR_MAX_VERSION; ver++) {
        const int count_bits = ver <= 9 ? 8 : 16;
        if ((long) len < (1L << count_bits) && 4 + count_bits + (long) len * 8 <= (long) data_codewords(ver, ecc) * 8) break;
    }
    if (ver > QR_MAX_VERSION) return -1;
    const int cap = data_codewords(ver, ecc);
    unsigned char dc[MAX_CODEWORDS] = {0};
    int bit = 0;
#define PUT(v, nbits)                                                                   \
    for (int _i = (nbits) - 1; _i >= 0; _i--, bit++)                                    \
        if (((v) >> _i) & 1) dc[bit >> 3] |= (unsigned char) (0x80 >> (bit & 7));
    PUT(4, 4);                                  // byte mode 0100
    PUT((int) len, ver <= 9 ? 8 : 16);
    for (size_t i = 0; i < len; i++) PUT(data[i], 8);
    const int term = cap * 8 - bit < 4 ? cap * 8 - bit : 4;
    bit += term;
    bit = (bit + 7) & ~7;
    for (int pad = 0xEC; bit < cap * 8; pad ^= 0xEC ^ 0x11) PUT(pad, 8);
#undef PUT
    unsigned char all[MAX_CODEWORDS];
    const int total = add_ecc_interleave(dc, ver, ecc, all);
    if (total < 0) return -1;

    static Grid g; // ~6.5 KB; the UI thread only
    memset(&g, 0, sizeof g);
    g.size = 17 + 4 * ver;
    draw_function_patterns(&g, ver, ecc);
    draw_codewords(&g, all, total);
    int best = mask;
    if (best < 0) {
        long best_p = -1;
        for (int m = 0; m < 8; m++) {
            apply_mask(&g, m);
            draw_format(&g, ecc, m);
            const long p = penalty(&g);
            if (best_p < 0 || p < best_p) {
                best_p = p;
                best = m;
            }
            apply_mask(&g, m); // undo (XOR)
        }
    }
    apply_mask(&g, best);
    draw_format(&g, ecc, best);
    memset(out, 0, sizeof *out);
    out->version = ver;
    out->size = g.size;
    out->mask = best;
    memcpy(out->dark, g.dark, sizeof g.dark);
    return 0;
}

int qr_svg(const QrCode *qr, char *out, const size_t cap) {
    const int n = qr->size + 8;
    size_t o = 0;
    int w = snprintf(out, cap, "<svg xmlns='http://www.w3.org/2000/svg' width='%d' height='%d' viewBox='0 0 %d %d'>"
                               "<rect width='%d' height='%d' fill='#fff'/><path fill='#000' d='", n * 8, n * 8, n, n, n, n);
    if (w < 0 || (size_t) w >= cap) return -1;
    o = (size_t) w;
    for (int y = 0; y < qr->size; y++) {
        for (int x = 0; x < qr->size;) {
            if (!qr->dark[y][x]) {
                x++;
                continue;
            }
            int run = 1;
            while (x + run < qr->size && qr->dark[y][x + run]) run++;
            w = snprintf(out + o, cap - o, "M%d %dh%dv1h-%dz", x + 4, y + 4, run, run);
            if (w < 0 || (size_t) w >= cap - o) return -1;
            o += (size_t) w;
            x += run;
        }
    }
    w = snprintf(out + o, cap - o, "'/></svg>");
    if (w < 0 || (size_t) w >= cap - o) return -1;
    return (int) (o + (size_t) w);
}
