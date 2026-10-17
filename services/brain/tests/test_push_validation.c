/* Unit test for services/brain/push_rules.c: the Web Push endpoint and key rules of the subscribe route.
 * Pure: no database, no brain. The keys are the RFC 8291 example subscription (p256dh 65 bytes, auth 16). */
#include <stdio.h>
#include <string.h>

#include "push_rules.h"

static int g_fail;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)

static const char *RFC_P256DH = "BCVxsr7N_eNgVRqvHtD0zTZsEc6-VV-JvLexhqUzORcxaOzi6-AYWXvTBHm4bjyPjs7Vd8pZGH6SRpkNoPU2Hcs";
static const char *RFC_AUTH = "BTBZMqHH6r4Tts7J_aSIgg";

/* a string of `len` bytes made of `c` */
static void fill(char *out, size_t len, char c) {
    memset(out, c, len);
    out[len] = 0;
}

int main(void) {
    /* the constants agree with the base64url lengths of the key sizes */
    CHECK(PUSH_P256DH_LEN == 87 && PUSH_AUTH_LEN == 22);
    CHECK(strlen(RFC_P256DH) == PUSH_P256DH_LEN && strlen(RFC_AUTH) == PUSH_AUTH_LEN);

    /* endpoints */
    CHECK(push_endpoint_ok("https://fcm.googleapis.com/fcm/send/abc-DEF_123"));
    CHECK(push_endpoint_ok("https://updates.push.services.mozilla.com/wpush/v2/gAAAA"));
    CHECK(!push_endpoint_ok("http://fcm.googleapis.com/fcm/send/abc")); /* not https */
    CHECK(!push_endpoint_ok("HTTPS://fcm.googleapis.com/x"));           /* scheme is lower case */
    CHECK(!push_endpoint_ok("https://"));                              /* no host */
    CHECK(!push_endpoint_ok("https:///path-without-host"));
    CHECK(!push_endpoint_ok("ftp://example.com/x"));
    CHECK(!push_endpoint_ok("https://example.com/with space"));
    CHECK(!push_endpoint_ok("https://example.com/\x01" "ctl"));
    CHECK(!push_endpoint_ok("https://example.com/\xc3\xa9"));           /* non-ASCII */
    CHECK(!push_endpoint_ok(NULL));
    CHECK(!push_endpoint_ok(""));
    {
        /* 512 bytes: the longest endpoint; 513 is refused */
        char longep[PUSH_ENDPOINT_MAX + 2];
        fill(longep, PUSH_ENDPOINT_MAX, 'a');
        memcpy(longep, "https://h/", 10);
        CHECK(strlen(longep) == PUSH_ENDPOINT_MAX && push_endpoint_ok(longep));
        char toolong[PUSH_ENDPOINT_MAX + 2];
        fill(toolong, PUSH_ENDPOINT_MAX + 1, 'a');
        memcpy(toolong, "https://h/", 10);
        CHECK(strlen(toolong) == PUSH_ENDPOINT_MAX + 1 && !push_endpoint_ok(toolong));
    }

    /* p256dh: 65 bytes, 87 characters of the base64url alphabet */
    CHECK(push_b64url_ok(RFC_P256DH, PUSH_P256DH_BYTES));
    {
        char k[PUSH_P256DH_LEN + 2];
        fill(k, PUSH_P256DH_LEN, 'A');
        CHECK(push_b64url_ok(k, PUSH_P256DH_BYTES));
        fill(k, PUSH_P256DH_LEN - 1, 'A');
        CHECK(!push_b64url_ok(k, PUSH_P256DH_BYTES)); /* 86: too short */
        fill(k, PUSH_P256DH_LEN + 1, 'A');
        CHECK(!push_b64url_ok(k, PUSH_P256DH_BYTES)); /* 88: too long */
        fill(k, PUSH_P256DH_LEN, 'A');
        k[40] = '+';
        CHECK(!push_b64url_ok(k, PUSH_P256DH_BYTES)); /* standard base64, not url-safe */
        k[40] = '/';
        CHECK(!push_b64url_ok(k, PUSH_P256DH_BYTES));
        k[40] = '=';
        CHECK(!push_b64url_ok(k, PUSH_P256DH_BYTES)); /* padding is not used */
        k[40] = '.';
        CHECK(!push_b64url_ok(k, PUSH_P256DH_BYTES));
        k[40] = '-';
        CHECK(push_b64url_ok(k, PUSH_P256DH_BYTES));
        k[40] = '_';
        CHECK(push_b64url_ok(k, PUSH_P256DH_BYTES));
    }
    CHECK(!push_b64url_ok(NULL, PUSH_P256DH_BYTES));
    CHECK(!push_b64url_ok("", PUSH_P256DH_BYTES));

    /* auth: 16 bytes, 22 characters */
    CHECK(push_b64url_ok(RFC_AUTH, PUSH_AUTH_BYTES));
    CHECK(!push_b64url_ok("BTBZMqHH6r4Tts7J_aSIg", PUSH_AUTH_BYTES));  /* 21 */
    CHECK(!push_b64url_ok("BTBZMqHH6r4Tts7J_aSIggA", PUSH_AUTH_BYTES)); /* 23 */
    CHECK(!push_b64url_ok("BTBZMqHH6r4Tts7J_aSI!g", PUSH_AUTH_BYTES));  /* a non-base64 character */
    CHECK(!push_b64url_ok("BTBZMqHH6r4Tts7J_aSI g", PUSH_AUTH_BYTES));  /* a space */
    CHECK(!push_b64url_ok("BTBZMqHH6r4Tts7J_aSI\xc3\xa9", PUSH_AUTH_BYTES)); /* non-ASCII, same byte count as 22 */

    if (g_fail) {
        fprintf(stderr, "push validation: FAILED\n");
        return 1;
    }
    printf("push validation: all checks passed\n");
    return 0;
}
