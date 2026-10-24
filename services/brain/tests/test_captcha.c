/* Unit test for services/brain/captcha.c: the ALTCHA challenges' issue, verify, replay and expiry (captcha.h). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "captcha.h"
#include "knsvc/crypto.h"
#include "knsvc/json.h"
#include "knsvc/loop.h"

static int g_fail;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)

#define SECRET "a-join-secret-of-enough-length"

/* the widget's value for a challenge and a number (altcha.1.0.6.min.js B()): base64(JSON {...}) */
static char *payload_of(const char *algorithm, const char *challenge, long long number, const char *salt, const char *signature) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "algorithm", algorithm);
    cJSON_AddStringToObject(o, "challenge", challenge);
    cJSON_AddNumberToObject(o, "number", (double) number);
    cJSON_AddStringToObject(o, "salt", salt);
    cJSON_AddStringToObject(o, "signature", signature);
    cJSON_AddNumberToObject(o, "took", 12);
    char *json = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    char *out = malloc(4 * strlen(json) / 3 + 8);
    CHECK(kn_base64_encode(json, strlen(json), out, 4 * strlen(json) / 3 + 8, 0, 1) > 0);
    free(json);
    return out;
}

/* solves a challenge JSON the way the widget's worker does (number by number up to maxnumber) */
static char *solve(const char *challenge_json, long long *number_out) {
    cJSON *c = cJSON_Parse(challenge_json);
    const char *salt = kn_json_str(c, "salt", ""), *want = kn_json_str(c, "challenge", "");
    const long long max = kn_json_int(c, "maxnumber", 0);
    char *payload = NULL;
    for (long long n = 0; n <= max && !payload; n++) {
        char buf[256], hex[65];
        uint8_t h[KN_SHA256_LEN];
        const int len = snprintf(buf, sizeof buf, "%s%lld", salt, n);
        kn_sha256(buf, (size_t) len, h);
        kn_hex_encode(h, sizeof h, hex, sizeof hex);
        if (!strcmp(hex, want)) {
            payload = payload_of(kn_json_str(c, "algorithm", ""), want, n, salt, kn_json_str(c, "signature", ""));
            if (number_out) *number_out = n;
        }
    }
    cJSON_Delete(c);
    return payload;
}

static char *issue_and_solve(const char *purpose, int64_t now) {
    char *c = captcha_issue(purpose, now);
    CHECK(c != NULL);
    char *p = c ? solve(c, NULL) : NULL;
    CHECK(p != NULL);
    free(c);
    return p;
}

int main(void) {
    CaptchaSettings s = {.join_secret = SECRET, .max_number = 1000, .ttl_s = 60, .enforce = 0};
    CHECK(captcha_configure(&s) == 0);
    CHECK(!captcha_enforced() && captcha_max_number() == 1000 && captcha_ttl() == 60);
    const int64_t now = kn_wall_ms();

    /* the challenge's shape: altcha-lib v1's, SHA-256, the salt's expires / for params */
    char *c = captcha_issue("login", now);
    cJSON *cj = c ? cJSON_Parse(c) : NULL;
    CHECK(cj && !strcmp(kn_json_str(cj, "algorithm", ""), "SHA-256") && strlen(kn_json_str(cj, "challenge", "")) == 64 &&
          strlen(kn_json_str(cj, "signature", "")) == 64 && kn_json_int(cj, "maxnumber", 0) == 1000);
    char want_tail[64];
    snprintf(want_tail, sizeof want_tail, "?expires=%lld&for=login", (long long) (now / 1000 + 60));
    const char *salt = kn_json_str(cj, "salt", "");
    CHECK(strlen(salt) == 24 + strlen(want_tail) && !strcmp(salt + 24, want_tail));
    cJSON_Delete(cj);

    /* solved: accepted once, then spent (a replay) */
    long long number = -1;
    char *p = c ? solve(c, &number) : NULL;
    CHECK(p && number >= 0 && number <= 1000);
    CHECK(captcha_verify(p, "login", now) == CAPTCHA_OK);
    CHECK(captcha_verify(p, "login", now) == CAPTCHA_SPENT);
    CHECK(captcha_verify(p, "login", now + 30000) == CAPTCHA_SPENT);
    /* once its expiry passed it is refused as expired (never accepted again) */
    CHECK(captcha_verify(p, "login", now + 61000) == CAPTCHA_EXPIRED);
    free(p);
    free(c);

    /* expired: after ttl; the expired check did not spend it */
    p = issue_and_solve("login", now);
    CHECK(captcha_verify(p, "login", now + 61000) == CAPTCHA_EXPIRED);
    CHECK(captcha_verify(p, "login", now + 59000) == CAPTCHA_OK);
    free(p);

    /* another flow's challenge is refused and not spent: a login challenge cannot host a game */
    p = issue_and_solve("login", now);
    CHECK(captcha_verify(p, "hosting", now) == CAPTCHA_PURPOSE);
    CHECK(captcha_verify(p, "register", now) == CAPTCHA_PURPOSE);
    CHECK(captcha_verify(p, "login", now) == CAPTCHA_OK);
    free(p);
    p = issue_and_solve("hosting", now);
    CHECK(captcha_verify(p, "hosting", now) == CAPTCHA_OK);
    free(p);
    p = issue_and_solve("register", now);
    CHECK(captcha_verify(p, "register", now) == CAPTCHA_OK);
    free(p);

    /* purposes: only the three flows */
    CHECK(captcha_issue("seek", now) == NULL && captcha_issue("", now) == NULL && captcha_issue(NULL, now) == NULL);
    CHECK(captcha_purpose_ok("hosting") && !captcha_purpose_ok("Login") && !captcha_purpose_ok("login&for=hosting"));

    /* a known answer (python hashlib / hmac): SHA-256(salt + "4242"), HMAC-SHA256(HMAC(secret, label) hex, challenge) */
    const char *kat_salt = "00112233445566778899aabb?expires=2000000000&for=login";
    const char *kat_ch = "fc558ed24b8107cbf70d47555129c1d8a2757230d9a534c65827b3c0e290e186";
    const char *kat_sig = "8442973563c19892d2de170874cae31db1e6e9916fee28ff94e98b3ab1a94d64";
    p = payload_of("SHA-256", kat_ch, 4242, kat_salt, kat_sig);
    CHECK(captcha_verify(p, "login", now) == CAPTCHA_OK);
    free(p);

    /* forgeries and garbage: a wrong number, an edited salt (its for= or expires), another key, another algorithm */
    c = captcha_issue("login", now);
    cj = cJSON_Parse(c);
    const char *ch = kn_json_str(cj, "challenge", ""), *sg = kn_json_str(cj, "signature", "");
    salt = kn_json_str(cj, "salt", "");
    long long n2 = -1;
    char *good = solve(c, &n2);
    p = payload_of("SHA-256", ch, n2 + 1, salt, sg);
    CHECK(captcha_verify(p, "login", now) == CAPTCHA_INVALID);
    free(p);
    char edited[256];
    snprintf(edited, sizeof edited, "%.*s?expires=%lld&for=hosting", 24, salt, (long long) (now / 1000 + 60));
    p = payload_of("SHA-256", ch, n2, edited, sg);
    CHECK(captcha_verify(p, "hosting", now) == CAPTCHA_INVALID);
    free(p);
    snprintf(edited, sizeof edited, "%.*s?expires=%lld&for=login", 24, salt, (long long) (now / 1000 + 99999));
    p = payload_of("SHA-256", ch, n2, edited, sg);
    CHECK(captcha_verify(p, "login", now) == CAPTCHA_INVALID);
    free(p);
    p = payload_of("SHA-512", ch, n2, salt, sg);
    CHECK(captcha_verify(p, "login", now) == CAPTCHA_INVALID);
    free(p);
    char foreign_sig[65];
    kn_hmac_sha256_hex("another-key", ch, 64, foreign_sig);
    p = payload_of("SHA-256", ch, n2, salt, foreign_sig);
    CHECK(captcha_verify(p, "login", now) == CAPTCHA_INVALID);
    free(p);
    CHECK(captcha_verify("", "login", now) == CAPTCHA_MISSING);
    CHECK(captcha_verify(NULL, "login", now) == CAPTCHA_MISSING);
    CHECK(captcha_verify("not base64 at all!", "login", now) == CAPTCHA_INVALID);
    CHECK(captcha_verify("eyJ9", "login", now) == CAPTCHA_INVALID);                           /* "{}" -ish */
    CHECK(captcha_verify("eyJhbGdvcml0aG0iOiJTSEEtMjU2In0=", "login", now) == CAPTCHA_INVALID); /* fields missing */
    /* the forgeries spent nothing: the real solution still goes through */
    CHECK(captcha_verify(good, "login", now) == CAPTCHA_OK);
    free(good);
    cJSON_Delete(cj);
    free(c);

    /* another network (another join secret) verifies none of ours */
    p = issue_and_solve("login", now);
    CaptchaSettings other = {.join_secret = "another-network-secret-xxxxxxxx", .max_number = 1000, .ttl_s = 60};
    CHECK(captcha_configure(&other) == 0);
    CHECK(captcha_verify(p, "login", now) == CAPTCHA_INVALID);
    /* every brain of the network (the same join secret) verifies it */
    CHECK(captcha_configure(&s) == 0);
    CHECK(captcha_verify(p, "login", now) == CAPTCHA_OK);
    free(p);

    /* the gate: enforcement off lets everything through (logged); on, it refuses all but an accepted payload */
    CaptchaResult r = CAPTCHA_OK;
    CHECK(captcha_gate(NULL, "login", "10.0.0.1", &r) == 1 && r == CAPTCHA_MISSING);
    CHECK(captcha_gate("garbage", "login", "10.0.0.1", &r) == 1 && r == CAPTCHA_INVALID);
    s.enforce = 1;
    CHECK(captcha_configure(&s) == 0 && captcha_enforced());
    CHECK(captcha_gate(NULL, "login", "10.0.0.1", &r) == 0 && r == CAPTCHA_MISSING);
    CHECK(!strcmp(captcha_refusal_text(r), "Please complete the security challenge to continue."));
    CHECK(!strcmp(captcha_refusal_error(r), "CaptchaRequired"));
    CHECK(captcha_gate("garbage", "register", "10.0.0.1", &r) == 0 && r == CAPTCHA_INVALID);
    CHECK(!strcmp(captcha_refusal_text(r), "CAPTCHA FAILED") && !strcmp(captcha_refusal_error(r), "RecaptchaFailed"));
    p = issue_and_solve("register", 0);
    CHECK(captcha_gate(p, "register", "10.0.0.1", &r) == 1 && r == CAPTCHA_OK);
    CHECK(captcha_gate(p, "register", "10.0.0.1", &r) == 0 && r == CAPTCHA_SPENT);
    CHECK(captcha_refusal_text(CAPTCHA_OK) == NULL && captcha_refusal_error(CAPTCHA_OK) == NULL);
    free(p);

    /* knobs are clamped (the client refuses a maxnumber over 1e7) */
    CaptchaSettings big = {.join_secret = SECRET, .max_number = 99999999, .ttl_s = 99999};
    CHECK(captcha_configure(&big) == 0 && captcha_max_number() == 10000000 && captcha_ttl() == 3600);
    CaptchaSettings dflt = {.join_secret = SECRET};
    CHECK(captcha_configure(&dflt) == 0 && captcha_max_number() == CAPTCHA_DEFAULT_MAX_NUMBER && captcha_ttl() == CAPTCHA_DEFAULT_TTL_S);

    /* no key (a blank join secret): nothing is issued, nothing verifies, and enforcement is off */
    CaptchaSettings none = {.join_secret = "", .enforce = 1};
    CHECK(captcha_configure(&none) == -1);
    CHECK(!captcha_enforced() && captcha_issue("login", now) == NULL);
    CHECK(captcha_verify("eyJ9", "login", now) == CAPTCHA_INVALID);
    CHECK(captcha_gate(NULL, "login", "x", &r) == 1);

    if (g_fail) return 1;
    printf("brain captcha test: ok\n");
    return 0;
}
