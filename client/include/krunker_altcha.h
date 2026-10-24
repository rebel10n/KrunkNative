#pragma once
// The "cpt" socket captcha of official Krunker games: an ALTCHA proof of work (SHA-256 search),
// solved without a browser (client/src/net/krunker_altcha.c has the format and its sources: the
// ALTCHA 1.0.6 widget that ships with v10.0.3, libs/altcha.1.0.6.min.js). Box Office's own captcha
// (kn_captcha.h, the brain's challenges) is solved by the same search.
// krunker_link hands "cpt" to the transport, which answers "cptR" (no ahNum step): natively on its
// socket thread, on the web in slices from the frame callback (krunker_net_tick), never in one go
// on the frame loop.
#include <auth_join.h> // KnSha256
#include <stdint.h>

#define KRUNKER_ALTCHA_DEFAULT_MAX 1000000L // altcha-lib's default maxnumber
#define KRUNKER_ALTCHA_MAX_CAP 10000000L    // a bigger maxnumber is refused (the notice instead of minutes of CPU)

typedef struct {
    KnSha256 base;      // SHA-256 state after the salt
    uint8_t want[32];
    long next, max;
    double started_ms;
    char algorithm[16], challenge[65], salt[512], signature[512];
} KrunkerAltcha;

// Validates the challenge (SHA-256 only, a 64-hex challenge, a salt and signature that fit, no
// control characters, 0 < maxnumber <= KRUNKER_ALTCHA_MAX_CAP; max_number <= 0 means the default).
// 0 = ready to search, -1 = refused.
int krunker_altcha_begin(KrunkerAltcha *a, const char *algorithm, const char *challenge, const char *salt, const char *signature, long max_number);
// Tries up to `budget` numbers. 1 = solved (*payload_b64 malloc'd: the widget's input value),
// 0 = not yet, -1 = no number up to maxnumber (or out of memory).
int krunker_altcha_step(KrunkerAltcha *a, long budget, char **payload_b64);
// begin + step to the end (the native socket thread). 0 = solved, -1 = refused / unsolvable.
int krunker_altcha_solve(const char *algorithm, const char *challenge, const char *salt, const char *signature, long max_number,
                         char **payload_b64);
