/*
 * krunker_cheat.c - CHEAT_CHECK (`cc`) responder for the official Krunker
 * protocol. See krunker_cheat.h for the public contract.
 *
 * ============================================================================
 * The algorithm (reverse-engineered from game.deob.js, READ ONLY, never run)
 * ============================================================================
 *
 * cc arrives as [nonce, <unused>, ciphertextB64] (the dispatcher applies the
 * decoded frame's args to the handler; arg 1 is never read).
 *
 * Handler  iiiïíìì(nonce, _unused, ciphertextB64)   @ game.deob.js:218169
 *   1. bytes   = atob(ciphertextB64) as bytes           // Buffer is undefined in a page
 *   2. ks      = iïìiîïì(nonce, bytes.length)           // keystream, below
 *   3. plain   = bytes[i] ^ ks[i]
 *   4. text    = new TextDecoder().decode(plain)        // BOM dropped, bad UTF-8 -> U+FFFD
 *   5. src     = JSON.parse(text)                       // normally a JS source string
 *   6. R       = (0, eval)(src)                         // indirect: global scope, sloppy;
 *                                                       // a non-string src comes back as is
 *          on throw: R = { r: null, e: { _eval: err.message || "eval_error" } }
 *          (`throw null` makes err.message itself throw: see below)
 *   7. out     = new TextEncoder().encode(JSON.stringify(R))
 *   8. ks2     = iïìiîïì(nonce, out.length)
 *   9. payload = btoa(out[i] ^ ks2[i])
 *  10. hash    = fnv(nonce + JSON.stringify(R))          // JS-float FNV, below
 *  11. send CHEAT_CHECK_RESPONSE(nonce, payload, hash)   // ccr = [nonce, b64, hash]
 *   The whole body sits in try { } catch { }: anything that throws outside the
 *   inner eval try (atob, JSON.parse, `.message` of a null/undefined throw,
 *   JSON.stringify of a cyclic/BigInt R) means no ccr at all. answer() returns
 *   non-zero in exactly those cases.

 * Keystream  iïìiîïì(nonce, len)                       @ game.deob.js:194974
 *   const SALT = [0x4b,0x52,0x55,0x4e,0x4b,0x33,0x52,0x5f,0x41,0x43];
 *          // "KRUNK3R_AC": the current build's cheat_salt (krunker_build_live.h)
 *   for i in 0..len:
 *       ks[i] = (nonce.charCodeAt(i % nonce.length) ^ SALT[i % SALT.length])
 *               + i  & 0xff
 *   JS precedence makes it ((unit ^ salt) + i) & 0xff, so only the low byte
 *   of each UTF-16 unit matters, but `i % nonce.length` counts UTF-16 units.
 *   We expand the UTF-8 nonce to its UTF-16 units first (nonce_units), so a
 *   non-ASCII nonce matches too; for the live base64url (ASCII) nonces the
 *   units are just the bytes. An empty nonce gives charCodeAt(NaN) = NaN and
 *   NaN ^ salt = salt, which the 0 unit reproduces.
 *
 * Hash  iìÎììii(R, nonce)                              @ game.deob.js:218200
 *   let h = 0x811c9dc5;                 // FNV offset basis
 *   for each UTF-16 code unit c of (nonce + JSON.stringify(R)):
 *       h = 0x1000193 * (h ^= c) | 0;   // NOTE: plain '*', not Math.imul
 *   return (h >>> 0).toString(16).padStart(8, '0');
 *
 *   The multiply is the subtle part. `(h ^ c)` is a signed 32-bit int in
 *   [-2^31, 2^31). Times the prime 16777619 the exact product can reach
 *   ~3.6e16 (~2^55), beyond the 2^53 range of exact doubles, so JS rounds it
 *   to the nearest double (ties-to-even) BEFORE `| 0` truncates to int32. A
 *   32-bit Math.imul would instead keep the exact low 32 bits, giving a
 *   different answer (e.g. 'abc12345{"r":1}' -> c3639e34, imul 69b94211). We
 *   reproduce the float rounding exactly with 64-bit integer arithmetic
 *   (fnv_mul below; a build with cheat_fnv_double 0 switches to imul), so no
 *   FPU rounding mode or long double is relied upon.
 *
 * Matching JS string semantics:
 *   - Steps 4-7 run inside a JS engine (the page on web, QuickJS natively), so
 *     JSON.parse, eval and JSON.stringify are real JS: key order, shortest
 *     round-trip number text, NaN/Infinity -> null, dropped undefined/function
 *     members, \uXXXX escapes of control characters and lone surrogates (the
 *     well-formed JSON.stringify of ES2019, which both QuickJS and V8 follow).
 *     The engine hands back String(out) as UTF-8; out is always well-formed,
 *     so that is exactly TextEncoder's byte sequence for step 7.
 *   - Step 4 natively: krunker_cheat_utf8_sanitize is the Encoding Standard's
 *     UTF-8 decoder (BOM strip, maximal-subpart U+FFFD), and the text goes to
 *     QuickJS with its length, so embedded NULs reach JSON.parse (which then
 *     rejects them, as in the browser). On the web TextDecoder itself runs.
 *   - JSON.stringify(R) === undefined (R undefined / a function / a symbol):
 *     TextEncoder.encode(undefined) encodes "" but `nonce + undefined` hashes
 *     the text "undefined"; answer() reproduces both.
 *   - The hash walks UTF-16 code units: UTF-8 is decoded to code points and
 *     astral ones are fed as their surrogate pair, i.e. JS .length/.charCodeAt.
 *   - Residual engine differences (documented, not fixable here): error
 *     message wording in {_eval: msg} (QuickJS vs V8), and anything only a
 *     real browser has (see krunker_cheat_qjs.c).
 */

#include "krunker_cheat.h"
#include "krunker_cheat_internal.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Keystream + XOR                                                     */
/* ------------------------------------------------------------------ */

/*
 * The nonce as JS sees it: its UTF-16 code units. Only the low byte of each
 * unit can reach the keystream (`(unit ^ salt) + i & 0xff` keeps 8 bits), but
 * the index `i % nonce.length` counts UTF-16 units, so a non-ASCII nonce must
 * be expanded to units first. Returns the unit count; *units is malloc'd
 * (NULL when the nonce is empty).
 */
static size_t nonce_units(const char *nonce, uint8_t **units);

static void xor_stream(const char *nonce, const uint8_t *data, uint8_t *out,
                       size_t len)
{
    /* the current build's salt (krunker_build_live.h; compiled: KRUNKER_CHEAT_SALT) */
    const KrunkerBuild *b = krunker_build();
    const unsigned char *salt = b->cheat_salt;
    const size_t slen = b->cheat_salt_len;
    uint8_t *u = NULL;
    const size_t ulen = nonce_units(nonce, &u);
    for (size_t i = 0; i < len; i++) {
        /* empty nonce: charCodeAt(NaN) is NaN, NaN ^ x == x */
        unsigned nc = ulen ? u[i % ulen] : 0;
        unsigned sc = slen ? salt[i % slen] : 0;
        uint8_t k = (uint8_t)(((nc ^ sc) + (unsigned)i) & 0xff);
        out[i] = data ? (uint8_t)(data[i] ^ k) : k;
    }
    free(u);
}

void krunker_cheat_keystream(const char *nonce, uint8_t *out, size_t len)
{
    xor_stream(nonce, NULL, out, len);
}

void krunker_cheat_xor(const char *nonce, const uint8_t *data, uint8_t *out,
                       size_t len)
{
    xor_stream(nonce, data, out, len);
}

/* ------------------------------------------------------------------ */
/* Base64 (RFC 4648 standard alphabet, as atob/btoa)                   */
/* ------------------------------------------------------------------ */

static const char b64_enc[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

char *krunker_cheat_b64_encode(const uint8_t *data, size_t len)
{
    size_t olen = ((len + 2) / 3) * 4;
    char *out = (char *)malloc(olen + 1);
    if (!out)
        return NULL;
    size_t o = 0;
    size_t i = 0;
    while (i + 3 <= len) {
        uint32_t n = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8) |
                     data[i + 2];
        out[o++] = b64_enc[(n >> 18) & 0x3f];
        out[o++] = b64_enc[(n >> 12) & 0x3f];
        out[o++] = b64_enc[(n >> 6) & 0x3f];
        out[o++] = b64_enc[n & 0x3f];
        i += 3;
    }
    size_t rem = len - i;
    if (rem == 1) {
        uint32_t n = (uint32_t)data[i] << 16;
        out[o++] = b64_enc[(n >> 18) & 0x3f];
        out[o++] = b64_enc[(n >> 12) & 0x3f];
        out[o++] = '=';
        out[o++] = '=';
    } else if (rem == 2) {
        uint32_t n = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8);
        out[o++] = b64_enc[(n >> 18) & 0x3f];
        out[o++] = b64_enc[(n >> 12) & 0x3f];
        out[o++] = b64_enc[(n >> 6) & 0x3f];
        out[o++] = '=';
    }
    out[o] = '\0';
    return out;
}

static int b64_val(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/*
 * WHATWG forgiving-base64 decode, i.e. what atob() accepts (the browser path
 * of the handler; Buffer is undefined there): ASCII whitespace is ignored,
 * one or two '=' may end a length%4==0 input, length%4==1 or any other
 * character (including an inner '=') fails, and leftover bits are dropped.
 * Where atob throws, the game's handler throws and sends no ccr.
 */
uint8_t *krunker_cheat_b64_decode(const char *b64, size_t *out_len)
{
    size_t cap = strlen(b64);
    char *sym = (char *)malloc(cap + 1);
    if (!sym)
        return NULL;
    size_t n = 0;
    for (const char *p = b64; *p; p++) {
        char c = *p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r')
            continue;
        sym[n++] = c;
    }
    if (n % 4 == 0 && n > 0 && sym[n - 1] == '=') {
        n--;
        if (sym[n - 1] == '=')
            n--;
    }
    uint8_t *out = (n % 4 == 1) ? NULL : (uint8_t *)malloc(n * 3 / 4 + 1);
    if (!out) {
        free(sym);
        return NULL;
    }
    uint32_t acc = 0;
    int nbits = 0;
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        int v = b64_val((unsigned char)sym[i]);
        if (v < 0) {
            free(sym);
            free(out);
            return NULL;
        }
        acc = (acc << 6) | (uint32_t)v;
        nbits += 6;
        if (nbits >= 8) {
            nbits -= 8;
            out[o++] = (uint8_t)((acc >> nbits) & 0xff);
        }
    }
    free(sym);
    *out_len = o;
    return out;
}

/* ------------------------------------------------------------------ */
/* JS-float FNV-1a over UTF-16 code units                              */
/* ------------------------------------------------------------------ */

/*
 * One hash step, `PRIME * (h ^= c) | 0` exactly as JS evaluates it, done in
 * integers so the result does not depend on the FPU (x87 excess precision,
 * fast-math, wasm) or on long double.
 *
 * x = h ^ c is a signed 32-bit int. The exact product P = |x| * PRIME
 * (< 2^31 * 2^32 = 2^63, fits in uint64). JS turns it into the nearest
 * IEEE-754 double: below 2^53 that is P itself; above, the low
 * (bitlen(P) - 53) bits are dropped with round-half-to-even. ToInt32 (`| 0`)
 * then keeps the low 32 bits of that integer-valued double, and the sign
 * applies symmetrically (round-to-nearest-even is sign-symmetric), so we round
 * |P|, reduce mod 2^32, and negate mod 2^32 for a negative x.
 */
static uint64_t round_to_double_u64(uint64_t p)
{
    if (p < (1ull << 53))
        return p;
    int shift = 0;
    while ((p >> shift) >= (1ull << 53))
        shift++;
    uint64_t high = p >> shift;
    uint64_t low = p & ((1ull << shift) - 1);
    uint64_t half = 1ull << (shift - 1);
    if (low > half || (low == half && (high & 1)))
        high++; /* may carry to 2^53: still exactly representable */
    return high << shift;
}

static int32_t fnv_mul(int32_t x)
{
    /* the current build's prime and step (krunker_build_live.h; compiled: KRUNKER_CHEAT_FNV_*) */
    const KrunkerBuild *b = krunker_build();
    if (!b->cheat_fnv_double)
        return (int32_t)((uint32_t)x * b->cheat_fnv_prime); /* Math.imul */
    const uint64_t prime = (uint64_t)b->cheat_fnv_prime;
    int neg = x < 0;
    uint64_t ax = neg ? (uint64_t)(-(int64_t)x) : (uint64_t)x;
    uint32_t r = (uint32_t)round_to_double_u64(ax * prime);
    if (neg)
        r = 0u - r;
    return (int32_t)r;
}

/* Fold one UTF-16 code unit into the running hash. */
static int32_t fnv_step(int32_t h, uint32_t unit)
{
    return fnv_mul((int32_t)((uint32_t)h ^ unit));
}

/*
 * Decode the next code point from a UTF-8 string; advance *i. Our strings
 * come from the JS engine (QuickJS / browser) or the server's msgpack str, so
 * they are well-formed UTF-8 in practice. QuickJS encodes a lone surrogate as
 * its 3-byte (CESU/WTF-8) form, which this decodes back to the same unit; a
 * truly invalid byte maps to U+FFFD like TextDecoder would.
 */
static uint32_t utf8_next(const unsigned char *s, size_t len, size_t *i)
{
    unsigned char c = s[*i];
    if (c < 0x80) { (*i)++; return c; }
    if ((c & 0xe0) == 0xc0 && *i + 1 < len) {
        uint32_t cp = ((c & 0x1fu) << 6) | (s[*i + 1] & 0x3fu);
        *i += 2; return cp;
    }
    if ((c & 0xf0) == 0xe0 && *i + 2 < len) {
        uint32_t cp = ((c & 0x0fu) << 12) | ((s[*i + 1] & 0x3fu) << 6) |
                      (s[*i + 2] & 0x3fu);
        *i += 3; return cp;
    }
    if ((c & 0xf8) == 0xf0 && *i + 3 < len) {
        uint32_t cp = ((c & 0x07u) << 18) | ((s[*i + 1] & 0x3fu) << 12) |
                      ((s[*i + 2] & 0x3fu) << 6) | (s[*i + 3] & 0x3fu);
        *i += 4; return cp;
    }
    (*i)++; return 0xfffd;
}

/* Calls fn(unit, ctx) for every UTF-16 code unit of a UTF-8 string, in the
 * order JS charCodeAt would visit them (astral = high then low surrogate). */
typedef void (*utf16_unit_fn)(uint32_t unit, void *ctx);
static void utf8_for_each_unit(const char *str, utf16_unit_fn fn, void *ctx)
{
    const unsigned char *s = (const unsigned char *)str;
    size_t len = strlen(str);
    size_t i = 0;
    while (i < len) {
        uint32_t cp = utf8_next(s, len, &i);
        if (cp <= 0xffff) {
            fn(cp, ctx);
        } else {
            cp -= 0x10000;
            fn(0xd800 + (cp >> 10), ctx);   /* high surrogate */
            fn(0xdc00 + (cp & 0x3ff), ctx); /* low surrogate  */
        }
    }
}

static void fnv_unit(uint32_t unit, void *ctx)
{
    int32_t *h = (int32_t *)ctx;
    *h = fnv_step(*h, unit);
}

typedef struct { uint8_t *buf; size_t n; } UnitBuf;
static void count_unit(uint32_t unit, void *ctx) { (void)unit; ((UnitBuf *)ctx)->n++; }
static void store_unit(uint32_t unit, void *ctx)
{
    UnitBuf *b = (UnitBuf *)ctx;
    b->buf[b->n++] = (uint8_t)(unit & 0xff);
}

static size_t nonce_units(const char *nonce, uint8_t **units)
{
    UnitBuf b = { NULL, 0 };
    *units = NULL;
    utf8_for_each_unit(nonce, count_unit, &b);
    if (b.n == 0)
        return 0;
    b.buf = (uint8_t *)malloc(b.n);
    if (!b.buf)
        return 0; /* OOM: degrade to the empty-nonce stream */
    size_t n = b.n;
    b.n = 0;
    utf8_for_each_unit(nonce, store_unit, &b);
    *units = b.buf;
    return n;
}

static void fnv_hex(int32_t h, char out9[9])
{
    static const char hex[] = "0123456789abcdef";
    uint32_t u = (uint32_t)h; /* h >>> 0 */
    for (int i = 7; i >= 0; i--) {
        out9[i] = hex[u & 0xf];
        u >>= 4;
    }
    out9[8] = '\0';
}

#define FNV_OFFSET ((int32_t)krunker_build()->cheat_fnv_offset)

void krunker_cheat_fnv1a_js(const char *str, char out9[9])
{
    int32_t h = FNV_OFFSET;
    utf8_for_each_unit(str, fnv_unit, &h);
    fnv_hex(h, out9);
}

/* ------------------------------------------------------------------ */
/* Eval seam (krunker_cheat_internal.h)                                */
/* ------------------------------------------------------------------ */

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

/*
 * Web build: the game's own steps, in the real browser page. The plaintext
 * goes through TextDecoder exactly like the game (BOM, U+FFFD replacement,
 * embedded NULs), then JSON.parse and an indirect (global, sloppy) eval.
 * Returns stringToNewUTF8("J" + String(out)) / "U" (out undefined), or 0 when
 * the game's handler would throw out and send no ccr. The C side frees it.
 */
EM_JS(char *, kn_cheat_eval_web, (const unsigned char *plain, int len), {
    try {
        var src = JSON.parse(new TextDecoder().decode(HEAPU8.slice(plain, plain + len)));
        var r;
        try {
            r = (0, eval)(src);
        } catch (e) {
            r = { r: null, e: { _eval: e.message || "eval_error" } };
        }
        var out = JSON.stringify(r);
        return stringToNewUTF8(out === undefined ? "U" : "J" + String(out));
    } catch (outer) {
        return 0;
    }
});

int krunker_cheat_eval(const uint8_t *plain, size_t len, char **json)
{
    *json = NULL;
    char *tagged = kn_cheat_eval_web(plain, (int)len);
    if (!tagged)
        return -1;
    if (tagged[0] == 'U') {
        free(tagged);
        return 0;
    }
    memmove(tagged, tagged + 1, strlen(tagged)); /* drop the tag, keep the NUL */
    *json = tagged;
    return 0;
}
#endif /* __EMSCRIPTEN__ (native: krunker_cheat_qjs.c) */

/* ------------------------------------------------------------------ */
/* Public: full cc -> ccr                                              */
/* ------------------------------------------------------------------ */

void krunker_ccr_free(KrunkerCcr *out)
{
    if (!out)
        return;
    free(out->nonce);
    free(out->payload_b64);
    out->nonce = NULL;
    out->payload_b64 = NULL;
    out->fnv[0] = '\0';
}

static char *dup_str(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = (char *)malloc(n);
    if (d)
        memcpy(d, s, n);
    return d;
}

int krunker_cheat_answer(const char *nonce, const char *ciphertext_b64,
                         KrunkerCcr *out)
{
    if (!nonce || !ciphertext_b64 || !out)
        return -1;
    memset(out, 0, sizeof(*out));

    /* 1-3: base64 decode + XOR decrypt to the plaintext bytes. */
    size_t clen = 0;
    uint8_t *plain = krunker_cheat_b64_decode(ciphertext_b64, &clen);
    if (!plain)
        return -2;
    krunker_cheat_xor(nonce, plain, plain, clen);

    /* 4-6: TextDecoder -> JSON.parse -> (0,eval) -> JSON.stringify. */
    char *json = NULL;
    int rc = krunker_cheat_eval(plain, clen, &json);
    free(plain);
    if (rc != 0)
        return -4; /* the game's handler throws here: it sends no ccr */

    /* 7-9: TextEncoder().encode(out): encode(undefined) is encode("") (the
     * argument's default), so an undefined stringify encrypts "". */
    const char *text = json ? json : "";
    size_t tlen = strlen(text);
    uint8_t *enc = (uint8_t *)malloc(tlen + 1);
    if (!enc) {
        free(json);
        return -3;
    }
    krunker_cheat_xor(nonce, (const uint8_t *)text, enc, tlen);
    out->payload_b64 = krunker_cheat_b64_encode(enc, tlen);
    free(enc);

    /* 10: hash over nonce + out; string concatenation turns an undefined
     * stringify into the text "undefined". */
    int32_t h = FNV_OFFSET;
    utf8_for_each_unit(nonce, fnv_unit, &h);
    utf8_for_each_unit(json ? json : "undefined", fnv_unit, &h);
    fnv_hex(h, out->fnv);
    free(json);

    out->nonce = dup_str(nonce);
    if (!out->nonce || !out->payload_b64) {
        krunker_ccr_free(out);
        return -3;
    }
    return 0;
}
