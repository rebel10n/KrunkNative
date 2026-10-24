// The socket captcha of official Krunker games (client/include/krunker_altcha.h).
//
// deob-10.0.3.js:111424-111437 / 111558-111625: on "cpt" [challenge] the client shows a "Security
// Challenge" modal with an <altcha-widget challengejson="JSON(challenge)">, polls for the widget's
// hidden input[name='altcha'] and sends its value as "cptR" (the one packet without an ahNum step,
// :111513); "cptR" from the server hides the modal, "error" ends it. The same widget answers
// rpc("bCpt", "hosting") before a custom game is hosted (:133912-133919). The widget is the
// open-source ALTCHA web component, which ships with v10.0.3 as libs/altcha.1.0.6.min.js (not in
// the game bundle): a proof of work, no human input. Its algorithm (ALTCHA v1, checked against that
// file: the worker's b() search, kn() / kt() and B(), the value):
//   challenge = {algorithm: "SHA-256", challenge: hex, salt: string, signature: hex, maxnumber?}
//   find n in [0, maxnumber] (default 1e6) with hex(SHA-256(salt + String(n))) == challenge
//   value = btoa(JSON.stringify({algorithm, challenge, number: n, salt, signature, test?, took: ms}))
// (test is only set by the widget's test mode, so it is left out.) The widget takes SHA-256, SHA-384
// and SHA-512 (its `G = ['SHA-256', 'SHA-384', 'SHA-512']`, `G.includes(algorithm.toUpperCase())`; the
// worker digests with crypto.subtle and that algorithm): all three are solved here. The matchmaker's
// CaptchaRequired is a different captcha (Cloudflare Turnstile, deob-10.0.3.js:130320-130369) and needs
// a browser.
#include <krunker_altcha.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> // strcasecmp
#include <time.h>

// ---- SHA-384 / SHA-512 (FIPS 180-4): the constants are the first 64 bits of the fractional parts of the cube roots of
// the first 80 primes (K) and of the square roots of the first 8 (SHA-512) / the 9th to 16th (SHA-384) primes (H)
static const uint64_t K512[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
    0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL};
static const uint64_t H512[8] = {
    0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
    0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL, 0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL};
static const uint64_t H384[8] = {
    0xcbbb9d5dc1059ed8ULL, 0x629a292a367cd507ULL, 0x9159015a3070dd17ULL, 0x152fecd8f70e5939ULL,
    0x67332667ffc00b31ULL, 0x8eb44a8768581511ULL, 0xdb0c2e0d64f98fa7ULL, 0x47b5481dbefa4fa4ULL};

#define ROR64(x, n) (((x) >> (n)) | ((x) << (64 - (n))))

static void sha512_block(uint64_t h[8], const uint8_t *p) {
    uint64_t w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = 0;
        for (int j = 0; j < 8; j++) w[i] = w[i] << 8 | p[8 * i + j];
    }
    for (int i = 16; i < 80; i++) {
        const uint64_t s0 = ROR64(w[i - 15], 1) ^ ROR64(w[i - 15], 8) ^ (w[i - 15] >> 7);
        const uint64_t s1 = ROR64(w[i - 2], 19) ^ ROR64(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint64_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 80; i++) {
        const uint64_t t1 = hh + (ROR64(e, 14) ^ ROR64(e, 18) ^ ROR64(e, 41)) + ((e & f) ^ (~e & g)) + K512[i] + w[i];
        const uint64_t t2 = (ROR64(a, 28) ^ ROR64(a, 34) ^ ROR64(a, 39)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += hh;
}

static void sha512_init(KrunkerSha512 *s, const int bits) {
    memcpy(s->h, bits == 384 ? H384 : H512, sizeof(s->h));
    s->used = 0;
    s->bytes = 0;
}

static void sha512_update(KrunkerSha512 *s, const void *data, size_t len) {
    const uint8_t *p = data;
    s->bytes += len;
    while (len) {
        const size_t n = len < 128 - s->used ? len : 128 - s->used;
        memcpy(s->buf + s->used, p, n);
        s->used += n;
        p += n;
        len -= n;
        if (s->used == 128) {
            sha512_block(s->h, s->buf);
            s->used = 0;
        }
    }
}

// the digest: 48 bytes for SHA-384, 64 for SHA-512
static void sha512_final(KrunkerSha512 *s, uint8_t *out, const int bits) {
    const uint64_t total_bits = s->bytes * 8;
    s->buf[s->used++] = 0x80;
    if (s->used > 112) {
        memset(s->buf + s->used, 0, 128 - s->used);
        sha512_block(s->h, s->buf);
        s->used = 0;
    }
    memset(s->buf + s->used, 0, 120 - s->used); // the length's high 64 bits stay 0
    for (int i = 0; i < 8; i++) s->buf[120 + i] = (uint8_t) (total_bits >> (56 - 8 * i));
    sha512_block(s->h, s->buf);
    for (int i = 0; i < bits / 64; i++) {
        for (int j = 0; j < 8; j++) out[8 * i + j] = (uint8_t) (s->h[i] >> (56 - 8 * j));
    }
}

// the widget's algorithm names, any case (G.includes(algorithm.toUpperCase())): 256 / 384 / 512, 0 another
static int algorithm_bits(const char *algorithm) {
    if (!algorithm) return 0;
    if (!strcasecmp(algorithm, "SHA-256")) return 256;
    if (!strcasecmp(algorithm, "SHA-384")) return 384;
    if (!strcasecmp(algorithm, "SHA-512")) return 512;
    return 0;
}

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec * 1000.0 + (double) ts.tv_nsec / 1e6;
}

static int hex_digit(const char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// JSON string literal of s into out (with quotes); 0 when it does not fit
static int json_str(char *out, const size_t n, const char *s) {
    size_t o = 0;
    if (o + 1 >= n) return 0;
    out[o++] = '"';
    for (const unsigned char *p = (const unsigned char *) s; *p; p++) {
        if (o + 7 >= n) return 0;
        if (*p == '"' || *p == '\\') {
            out[o++] = '\\';
            out[o++] = (char) *p;
        } else if (*p < 0x20) {
            o += (size_t) snprintf(out + o, n - o, "\\u%04x", *p);
        } else {
            out[o++] = (char) *p;
        }
    }
    if (o + 2 > n) return 0;
    out[o++] = '"';
    out[o] = 0;
    return 1;
}

static char *b64(const char *data, const size_t len) {
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char *out = malloc(4 * ((len + 2) / 3) + 1);
    if (!out) return NULL;
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        const unsigned v = (unsigned) (unsigned char) data[i] << 16 | (i + 1 < len ? (unsigned) (unsigned char) data[i + 1] << 8 : 0) |
                           (i + 2 < len ? (unsigned char) data[i + 2] : 0);
        out[o++] = t[v >> 18 & 63];
        out[o++] = t[v >> 12 & 63];
        out[o++] = i + 1 < len ? t[v >> 6 & 63] : '=';
        out[o++] = i + 2 < len ? t[v & 63] : '=';
    }
    out[o] = 0;
    return out;
}

static int printable(const char *s, const size_t max) {
    const size_t n = strlen(s);
    if (n >= max) return 0;
    for (size_t i = 0; i < n; i++) {
        if ((unsigned char) s[i] < 0x20 || s[i] == 0x7f) return 0;
    }
    return 1;
}

int krunker_altcha_begin(KrunkerAltcha *a, const char *algorithm, const char *challenge, const char *salt, const char *signature,
                         const long max_number) {
    memset(a, 0, sizeof(*a));
    const int bits = algorithm_bits(algorithm);
    if (!bits || !challenge || !salt || strlen(challenge) != (size_t) bits / 4) return -1;
    if (!salt[0] || !printable(salt, sizeof(a->salt)) || (signature && !printable(signature, sizeof(a->signature)))) return -1;
    if (max_number > KRUNKER_ALTCHA_MAX_CAP) return -1; // not burning minutes of CPU on it: the notice instead
    for (int i = 0; i < bits / 8; i++) {
        const int hi = hex_digit(challenge[2 * i]), lo = hex_digit(challenge[2 * i + 1]);
        if (hi < 0 || lo < 0) return -1;
        a->want[i] = (uint8_t) (hi << 4 | lo);
    }
    a->bits = bits;
    snprintf(a->algorithm, sizeof(a->algorithm), "%s", algorithm);
    snprintf(a->challenge, sizeof(a->challenge), "%s", challenge);
    snprintf(a->salt, sizeof(a->salt), "%s", salt);
    snprintf(a->signature, sizeof(a->signature), "%s", signature ? signature : "");
    a->max = max_number > 0 ? max_number : KRUNKER_ALTCHA_DEFAULT_MAX;
    a->next = 0;
    a->started_ms = now_ms();
    if (bits == 256) {
        kn_sha256_init(&a->base);
        kn_sha256_update(&a->base, a->salt, strlen(a->salt));
    } else {
        sha512_init(&a->base512, bits);
        sha512_update(&a->base512, a->salt, strlen(a->salt));
    }
    return 0;
}

// The widget's value: btoa(JSON.stringify({algorithm, challenge, number, salt, signature, took}))
// (altcha.1.0.6.min.js B(); deob-10.0.3.js:111609-111612 sends input[name='altcha'].value as "cptR").
static char *payload(const KrunkerAltcha *a, const long number) {
    const long took = (long) (now_ms() - a->started_ms);
    char al[64], c[160], sa[1100], si[1100];
    if (!json_str(al, sizeof(al), a->algorithm) || !json_str(c, sizeof(c), a->challenge) || !json_str(sa, sizeof(sa), a->salt) ||
        !json_str(si, sizeof(si), a->signature))
        return NULL;
    char json[2600];
    const int len = snprintf(json, sizeof(json), "{\"algorithm\":%s,\"challenge\":%s,\"number\":%ld,\"salt\":%s,\"signature\":%s,\"took\":%ld}",
                             al, c, number, sa, si, took);
    if (len <= 0 || (size_t) len >= sizeof(json)) return NULL;
    return b64(json, (size_t) len);
}

int krunker_altcha_step(KrunkerAltcha *a, long budget, char **payload_b64) {
    *payload_b64 = NULL;
    for (; budget > 0 && a->next <= a->max; budget--, a->next++) {
        char digits[24];
        const int d = snprintf(digits, sizeof(digits), "%ld", a->next);
        uint8_t got[64];
        if (a->bits == 256) {
            KnSha256 s = a->base;
            kn_sha256_update(&s, digits, (size_t) d);
            kn_sha256_final(&s, got);
        } else {
            KrunkerSha512 s = a->base512;
            sha512_update(&s, digits, (size_t) d);
            sha512_final(&s, got, a->bits);
        }
        if (memcmp(got, a->want, (size_t) a->bits / 8) != 0) continue;
        *payload_b64 = payload(a, a->next);
        return *payload_b64 ? 1 : -1;
    }
    return a->next > a->max ? -1 : 0;
}

int krunker_altcha_solve(const char *algorithm, const char *challenge, const char *salt, const char *signature, const long max_number,
                         char **payload_b64) {
    *payload_b64 = NULL;
    KrunkerAltcha a;
    if (krunker_altcha_begin(&a, algorithm, challenge, salt, signature, max_number) != 0) return -1;
    return krunker_altcha_step(&a, a.max + 1, payload_b64) == 1 ? 0 : -1;
}

int krunker_altcha_digest_hex(const char *algorithm, const void *data, const size_t len, char out[129]) {
    static const char hex[] = "0123456789abcdef";
    const int bits = algorithm_bits(algorithm);
    uint8_t d[64];
    if (!bits) return -1;
    if (bits == 256) {
        KnSha256 s;
        kn_sha256_init(&s);
        kn_sha256_update(&s, data, len);
        kn_sha256_final(&s, d);
    } else {
        KrunkerSha512 s;
        sha512_init(&s, bits);
        sha512_update(&s, data, len);
        sha512_final(&s, d, bits);
    }
    for (int i = 0; i < bits / 8; i++) {
        out[2 * i] = hex[d[i] >> 4];
        out[2 * i + 1] = hex[d[i] & 15];
    }
    out[bits / 4] = 0;
    return 0;
}
