/*
 * services/brain/captcha.h - Box Office's captcha: an ALTCHA proof of work in front of a password sign-in, a
 * registration and a hosted game (doc/v10/captcha.md, doc/backend.md 7.10).
 *
 * Krunker v10.0.3 asks for two captchas (doc/v10/captcha.md has every call site):
 *   - Cloudflare Turnstile, invisible ("interaction-only"): before every loginUsername, its token in the body's
 *     "b" (deob-10.0.3.js:130762-130763, 30111-30122), and when /seek-game answers CaptchaRequired
 *     (deob-10.0.3.js:164680-164699, 164897). Its token is checked with Cloudflare's secret for a sitekey bound
 *     to krunker.io; Box Office has no such site, and the native client is no web page.
 *   - ALTCHA (libs/altcha.1.0.6.min.js, the <altcha-widget>) on the game socket: "cpt" pushes a challenge and
 *     "cptR" answers it (deob-10.0.3.js:111424-111437, 111558-111625), and rpc("bCpt", "hosting") asks for one
 *     before a custom game is hosted, its solution riding on the "custom" packet (deob-10.0.3.js:133912-133919,
 *     229992-229998, 255893-255897).
 * Box Office uses ALTCHA for every captcha it asks for: a challenge this brain issues and verifies itself, that
 * the C client solves on every platform (client/src/net/kn_captcha.c). Deliberate difference: the sign-in's
 * captcha is a proof of work, not Turnstile.
 *
 * The challenge (altcha-lib v1, the format the 1.0.6 widget solves; altcha.1.0.6.min.js kn() / B()):
 *   salt      = 24 random hex digits + "?expires=<unix s>&for=<purpose>"
 *   number    = random in [0, max_number]
 *   challenge = hex(SHA-256(salt + decimal(number)))
 *   signature = hex(HMAC-SHA256(key, challenge))
 *   -> {"algorithm":"SHA-256","challenge":...,"maxnumber":max_number,"salt":...,"signature":...}
 * The payload (the widget's input value, altcha.1.0.6.min.js B()):
 *   base64(JSON {algorithm, challenge, number, salt, signature, took})
 * A payload is accepted once: SHA-256 only, the hash and the signature match (constant time), the salt's expires
 * is in the future and its for= names the flow, and the challenge was not spent on this brain before (spent ones
 * are remembered in memory until they expire). The key is HMAC-SHA256(brain.join_secret, "kn-captcha-altcha-v1"),
 * so every brain of a network verifies what another one issued; the spent set is per brain (like mm_token.c's).
 */
#ifndef KN_BRAIN_CAPTCHA_H
#define KN_BRAIN_CAPTCHA_H

#include <stddef.h>
#include <stdint.h>

#define CAPTCHA_DEFAULT_MAX_NUMBER 100000L /* captcha.max_number: the widget's own default is 1e6 (altcha.1.0.6.min.js) */
#define CAPTCHA_DEFAULT_TTL_S 300          /* captcha.ttl_s */
#define CAPTCHA_PAYLOAD_MAX 4096           /* longer payloads are refused unread */

typedef enum CaptchaResult {
    CAPTCHA_OK = 0,
    CAPTCHA_MISSING, /* no payload (an older client, or the client got no challenge) */
    CAPTCHA_INVALID, /* malformed, a wrong number, a foreign or forged signature */
    CAPTCHA_EXPIRED, /* ours, but past its expires */
    CAPTCHA_SPENT,   /* ours, already accepted once on this brain (a replay) */
    CAPTCHA_PURPOSE, /* ours, but issued for another flow (a login challenge offered to host a game) */
} CaptchaResult;

typedef struct CaptchaSettings {
    const char *join_secret; /* brain.join_secret: the key is derived from it */
    long max_number;         /* captcha.max_number (<= 0: CAPTCHA_DEFAULT_MAX_NUMBER) */
    int ttl_s;               /* captcha.ttl_s (<= 0: CAPTCHA_DEFAULT_TTL_S) */
    int enforce;             /* captcha.enforce (default off): refuse a flow without an accepted payload */
} CaptchaSettings;

/* Loads the key and knobs. 0 ok, -1 no key (blank join secret): nothing is issued, every payload is invalid. */
int captcha_configure(const CaptchaSettings *s);
int captcha_enforced(void); /* captcha.enforce, and a key to issue with */
long captcha_max_number(void);
int captcha_ttl(void);

/* A purpose a challenge may be issued for: "login", "register", "hosting". */
int captcha_purpose_ok(const char *purpose);

/* A fresh challenge JSON for purpose (malloc'd), NULL: no key, an unknown purpose, out of memory.
 * now_ms 0 = the wall clock. */
char *captcha_issue(const char *purpose, int64_t now_ms);

/* Checks payload for purpose and, when it is accepted, spends it. NULL or "" = CAPTCHA_MISSING. */
CaptchaResult captcha_verify(const char *payload, const char *purpose, int64_t now_ms);
const char *captcha_result_name(CaptchaResult r);

/* The text a refused sign-in or registration answers: v10.0.3's own captcha strings
 * ("Please complete the security challenge to continue." for a missing one, deob-10.0.3.js:111601;
 * "CAPTCHA FAILED" for a refused one, deob-10.0.3.js:111430). */
const char *captcha_refusal_text(CaptchaResult r);
/* The matchmaker error name a refused host-game answers (v10.0.3's ERRORS, deob-10.0.3.js:164975-164976):
 * "CaptchaRequired" (missing) or "RecaptchaFailed" (refused). */
const char *captcha_refusal_error(CaptchaResult r);

/* The captcha of one flow (purpose), at its entry point and before any password hashing: payload verified and,
 * when accepted, spent. 1 = go on: accepted, or refused with captcha.enforce off (logged at DEBUG: the rollout's
 * first step, doc/backend.md 7.10). 0 = refused (logged at INFO): answer captcha_refusal_text /
 * captcha_refusal_error of *result. who = the client's address, for the log. */
int captcha_gate(const char *payload, const char *purpose, const char *who, CaptchaResult *result);

/* The brain module (captcha_http.c): captcha.* config, GET /captcha/challenge?for=<purpose>. */
struct Brain;
int captcha_init(struct Brain *b);

#endif
