// The socket captcha of official Krunker games (client/include/krunker_altcha.h).
//
// game.deob.js:98132-98143 / 98295-98304: on "cpt" [challenge] the client shows a "Security
// Challenge" modal with an <altcha-widget challengejson="JSON(challenge)">, polls for the widget's
// hidden input[name='altcha'] and sends its value as "cptR" (the one packet without an ahNum step);
// "cptR" from the server hides the modal, "error" ends it. The widget itself is the open-source
// ALTCHA web component, loaded by the page, not part of the bundle: it is a proof of work, no
// human input. Its algorithm (ALTCHA v1 challenge format, altcha-lib):
//   challenge = {algorithm: "SHA-256", challenge: hex, salt: string, signature: hex, maxnumber?}
//   find n in [0, maxnumber] with hex(SHA-256(salt + String(n))) == challenge
//   value = btoa(JSON.stringify({algorithm, challenge, number: n, salt, signature, took: ms}))
// UNVERIFIED ASSUMPTION: the payload shape is upstream ALTCHA's widget format, not readable in
// game.deob.js: verify against a real "cpt" before relying on it (gap list in krunker_net.h). The matchmaker's CaptchaRequired is a
// different captcha (Cloudflare Turnstile, iiïîìïì at :115623) and needs a browser.
#include <krunker_altcha.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
    if (!algorithm || !challenge || !salt || strcmp(algorithm, "SHA-256") != 0 || strlen(challenge) != 64) return -1;
    if (!salt[0] || !printable(salt, sizeof(a->salt)) || (signature && !printable(signature, sizeof(a->signature)))) return -1;
    if (max_number > KRUNKER_ALTCHA_MAX_CAP) return -1; // not burning minutes of CPU on it: the notice instead
    for (int i = 0; i < 32; i++) {
        const int hi = hex_digit(challenge[2 * i]), lo = hex_digit(challenge[2 * i + 1]);
        if (hi < 0 || lo < 0) return -1;
        a->want[i] = (uint8_t) (hi << 4 | lo);
    }
    snprintf(a->algorithm, sizeof(a->algorithm), "%s", algorithm);
    snprintf(a->challenge, sizeof(a->challenge), "%s", challenge);
    snprintf(a->salt, sizeof(a->salt), "%s", salt);
    snprintf(a->signature, sizeof(a->signature), "%s", signature ? signature : "");
    a->max = max_number > 0 ? max_number : KRUNKER_ALTCHA_DEFAULT_MAX;
    a->next = 0;
    a->started_ms = now_ms();
    kn_sha256_init(&a->base);
    kn_sha256_update(&a->base, a->salt, strlen(a->salt));
    return 0;
}

// The widget's value: btoa(JSON.stringify({algorithm, challenge, number, salt, signature, took})).
// UNVERIFIED ASSUMPTION: this is upstream ALTCHA's payload (altcha-lib / the altcha web component);
// game.deob.js only shows that input[name='altcha'].value goes out as "cptR".
static char *payload(const KrunkerAltcha *a, const long number) {
    const long took = (long) (now_ms() - a->started_ms);
    char al[64], c[80], sa[1100], si[1100];
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
        KnSha256 s = a->base;
        uint8_t got[32];
        kn_sha256_update(&s, digits, (size_t) d);
        kn_sha256_final(&s, got);
        if (memcmp(got, a->want, 32) != 0) continue;
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
