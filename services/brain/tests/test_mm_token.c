/* Unit test for services/brain/mm_token.c (seek-game validation tokens). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mm_token.h"
#include "knsvc/crypto.h"
#include "knsvc/loop.h"

static int g_fail;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)

int main(void) {
    MmTokenSettings s = {.join_secret = "a-join-secret-of-enough-length", .ttl_s = 60};
    CHECK(mm_token_init(&s) == 0);
    const int64_t now = kn_wall_ms();

    /* issued, accepted once, then spent */
    char *t = mm_token_issue("10.0.0.1");
    CHECK(t && strlen(t) < 400);
    CHECK(mm_token_check(t, "10.0.0.1", now) == MM_TOKEN_OK);
    CHECK(mm_token_check(t, "10.0.0.1", now) == MM_TOKEN_INVALID);
    free(t);

    /* missing: allowed unless required (no error name) */
    CHECK(mm_token_check(NULL, "x", now) == MM_TOKEN_MISSING && mm_token_error_name(MM_TOKEN_MISSING) == NULL);
    CHECK(mm_token_check("", "x", now) == MM_TOKEN_MISSING);
    CHECK(mm_token_check("garbage", "x", now) == MM_TOKEN_INVALID);
    CHECK(!strcmp(mm_token_error_name(MM_TOKEN_INVALID), "InvalidClientValidationToken"));

    /* expired: after ttl */
    t = mm_token_issue("10.0.0.1");
    CHECK(mm_token_check(t, "10.0.0.1", now + 61000) == MM_TOKEN_EXPIRED);
    CHECK(!strcmp(mm_token_error_name(MM_TOKEN_EXPIRED), "ExpiredClientValidationToken"));
    /* the expired check did not spend it */
    CHECK(mm_token_check(t, "10.0.0.1", now) == MM_TOKEN_OK);
    free(t);

    /* a join ticket (signed with join_secret itself) is not a validation token */
    cJSON *claims = cJSON_Parse("{\"typ\":\"mmv\",\"jti\":\"0123456789abcdef01234567\",\"ip\":\"\"}");
    char *ticket = kn_signed_token_create("a-join-secret-of-enough-length", claims, 60);
    CHECK(mm_token_check(ticket, "10.0.0.1", now) == MM_TOKEN_INVALID);
    free(ticket);
    cJSON_Delete(claims);

    /* the right key but no validation claims: Data error */
    char key[65];
    kn_hmac_sha256_hex("a-join-secret-of-enough-length", "kn-mm-validation-v1", strlen("kn-mm-validation-v1"), key);
    claims = cJSON_Parse("{\"brn\":\"b\",\"nid\":\"N\",\"lid\":\"x\",\"jti\":\"j\"}");
    char *other = kn_signed_token_create(key, claims, 60);
    CHECK(mm_token_check(other, "10.0.0.1", now) == MM_TOKEN_DATA);
    free(other);
    cJSON_Delete(claims);

    /* another network's key */
    MmTokenSettings s2 = {.join_secret = "another-network-secret-xxxxxxxx", .ttl_s = 60};
    CHECK(mm_token_init(&s2) == 0);
    char *foreign = mm_token_issue("10.0.0.1");
    CHECK(mm_token_init(&s) == 0);
    CHECK(mm_token_check(foreign, "10.0.0.1", now) == MM_TOKEN_INVALID);
    free(foreign);

    /* IP binding (opt-in); ::ffff:a.b.c.d == a.b.c.d */
    s.bind_ip = 1;
    CHECK(mm_token_init(&s) == 0);
    t = mm_token_issue("10.0.0.1");
    CHECK(mm_token_check(t, "10.0.0.2", now) == MM_TOKEN_IP);
    CHECK(!strcmp(mm_token_error_name(MM_TOKEN_IP), "InvalidClientIP"));
    CHECK(mm_token_check(t, "::ffff:10.0.0.1", now) == MM_TOKEN_OK);
    free(t);

    /* required: a missing token is refused with Krunker's name */
    s.bind_ip = 0;
    s.require = 1;
    CHECK(mm_token_init(&s) == 0 && mm_token_required());
    CHECK(!strcmp(mm_token_error_name(MM_TOKEN_MISSING), "InvalidClientValidationToken"));

    /* no key at all */
    MmTokenSettings none = {.join_secret = "", .ttl_s = 60};
    CHECK(mm_token_init(&none) != 0 && mm_token_issue("x") == NULL);

    if (!g_fail) printf("brain mm_token: OK\n");
    return g_fail;
}
