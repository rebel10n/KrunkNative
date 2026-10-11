/* Crypto vectors: SHA-1/256 (FIPS 180), HMAC (RFC 4231), PBKDF2 (RFC 7914 / RFC 6070-style),
 * base64 (RFC 4648), WebSocket accept (RFC 6455), JWT HS256 (jwt.io sample), password hashing. */
#include "kn_test.h"
#include "knsvc/crypto.h"
#include "knsvc/ws.h"

#include <time.h>

static void hex(const uint8_t *d, size_t n, char *out) { kn_hex_encode(d, n, out, n * 2 + 1); }

static void test_digests(void) {
    uint8_t d[64];
    char h[129];
    kn_sha256("abc", 3, d);
    hex(d, 32, h);
    CHECK_EQ_STR(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    kn_sha1("abc", 3, d);
    hex(d, 20, h);
    CHECK_EQ_STR(h, "a9993e364706816aba3e25717850c26c9cd0d89d");
    uint8_t key[20];
    memset(key, 0x0b, sizeof key);
    kn_hmac_sha256(key, sizeof key, "Hi There", 8, d);
    hex(d, 32, h);
    CHECK_EQ_STR(h, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
    kn_hmac_sha256("Jefe", 4, "what do ya want for nothing?", 28, d);
    hex(d, 32, h);
    CHECK_EQ_STR(h, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    char hx[65];
    CHECK(kn_hmac_sha256_hex("Jefe", "what do ya want for nothing?", 28, hx) == 0);
    CHECK_EQ_STR(hx, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    kn_pbkdf2_sha256("passwd", 6, (const uint8_t *)"salt", 4, 1, d, 64);
    hex(d, 64, h);
    CHECK_EQ_STR(h, "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc"
                    "49ca9cccf179b645991664b39d77ef317c71b845b1e30bd509112041d3a19783");
    kn_pbkdf2_sha256("password", 8, (const uint8_t *)"salt", 4, 4096, d, 32);
    hex(d, 32, h);
    CHECK_EQ_STR(h, "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a");
}

static void test_encodings(void) {
    static const char *in[] = {"", "f", "fo", "foo", "foob", "fooba", "foobar"};
    static const char *out[] = {"", "Zg==", "Zm8=", "Zm9v", "Zm9vYg==", "Zm9vYmE=", "Zm9vYmFy"};
    char b[32];
    uint8_t raw[32];
    for (int i = 0; i < 7; i++) {
        CHECK(kn_base64_encode(in[i], strlen(in[i]), b, sizeof b, 0, 1) == (int)strlen(out[i]));
        CHECK_EQ_STR(b, out[i]);
        int n = kn_base64_decode(out[i], strlen(out[i]), raw, sizeof raw);
        CHECK(n == (int)strlen(in[i]) && memcmp(raw, in[i], (size_t)n) == 0);
        /* unpadded form decodes too */
        kn_base64_encode(in[i], strlen(in[i]), b, sizeof b, 1, 0);
        CHECK_EQ_INT(kn_base64_decode(b, strlen(b), raw, sizeof raw), strlen(in[i]));
    }
    uint8_t bytes[3] = {0xfb, 0xff, 0xfe};
    kn_base64_encode(bytes, 3, b, sizeof b, 1, 0);
    CHECK_EQ_STR(b, "-__-");
    kn_base64_encode(bytes, 3, b, sizeof b, 0, 0);
    CHECK_EQ_STR(b, "+//+");
    CHECK(kn_base64_decode("Zg=x", 4, raw, sizeof raw) < 0);
    CHECK(kn_base64_decode("Z", 1, raw, sizeof raw) < 0);
    CHECK(kn_base64_decode("Zh", 2, raw, sizeof raw) < 0); /* non-canonical trailing bits */
    CHECK(kn_base64_encode("foobar", 6, b, 8, 0, 1) < 0);   /* too small */
    CHECK_EQ_INT(kn_hex_decode("00ffA0", 6, raw, sizeof raw), 3);
    CHECK(raw[1] == 0xff && raw[2] == 0xa0);
    CHECK(kn_hex_decode("0g", 2, raw, sizeof raw) < 0);
    CHECK(kn_hex_decode("abc", 3, raw, sizeof raw) < 0);
    char acc[29];
    CHECK(kn_ws_accept_key("dGhlIHNhbXBsZSBub25jZQ==", acc) == 0);
    CHECK_EQ_STR(acc, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

static void test_random(void) {
    char a[65], b[65], t[64];
    CHECK(kn_random_hex(a, sizeof a, 32) == 0 && strlen(a) == 64);
    CHECK(kn_random_hex(b, sizeof b, 32) == 0);
    CHECK(strcmp(a, b) != 0);
    CHECK(kn_random_hex(a, 10, 32) != 0);
    CHECK(kn_random_token(t, sizeof t, 24) == 0 && strlen(t) == 32);
    CHECK(strspn(t, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_") == 32);
    CHECK(kn_random_u64() != kn_random_u64());
    CHECK(kn_ct_equal("abc", "abc", 3) && !kn_ct_equal("abc", "abd", 3));
    CHECK(kn_ct_streq("token", "token") && !kn_ct_streq("token", "tokens") && !kn_ct_streq("", "x"));
    CHECK(kn_ct_streq("", "") && !kn_ct_streq(NULL, "x"));
}

static void test_passwords(void) {
    kn_password_set_iterations(20000);
    char h1[KN_PASSWORD_HASH_MAX], h2[KN_PASSWORD_HASH_MAX];
    CHECK(kn_password_hash("correct horse", h1, sizeof h1) == 0);
    CHECK(kn_password_hash("correct horse", h2, sizeof h2) == 0);
    CHECK(strncmp(h1, "$pbkdf2-sha256$i=20000$", 23) == 0);
    CHECK(strcmp(h1, h2) != 0); /* per-hash salt */
    CHECK(strlen(h1) < KN_PASSWORD_HASH_MAX);
    CHECK_EQ_INT(kn_password_verify("correct horse", h1), 1);
    CHECK_EQ_INT(kn_password_verify("correct horsf", h1), 0);
    CHECK_EQ_INT(kn_password_verify("", h1), 0);
    CHECK_EQ_INT(kn_password_verify("x", "garbage"), -1);
    CHECK_EQ_INT(kn_password_verify("x", "$pbkdf2-sha256$i=abc$x$y"), -1);
    CHECK_EQ_INT(kn_password_needs_rehash(h1), 0);
    kn_password_set_iterations(30000);
    CHECK_EQ_INT(kn_password_needs_rehash(h1), 1);
    CHECK_EQ_INT(kn_password_verify("correct horse", h1), 1); /* old cost still verifies */
    kn_password_set_iterations(5);
    CHECK_EQ_INT(kn_password_get_iterations(), 10000);
    /* known-answer: salt "saltsaltsaltsalt" (b64 c2FsdHNhbHRzYWx0c2FsdA), 10000 iterations */
    char known[KN_PASSWORD_HASH_MAX];
    uint8_t dk[32];
    char dk64[64];
    kn_pbkdf2_sha256("hunter2", 7, (const uint8_t *)"saltsaltsaltsalt", 16, 10000, dk, 32);
    kn_base64_encode(dk, 32, dk64, sizeof dk64, 0, 0);
    snprintf(known, sizeof known, "$pbkdf2-sha256$i=10000$c2FsdHNhbHRzYWx0c2FsdA$%s", dk64);
    CHECK_EQ_INT(kn_password_verify("hunter2", known), 1);
#ifdef __linux__
    /* crypt(3) formats (when libcrypt was linked; -1 otherwise) */
    int y = kn_password_verify("hunter2", "$y$j9T$saltsaltsaltsalt$pQKob88sNg1.ktD6ni0OAUKcK4w22JpXSbU9UtU38wC");
    int s6 = kn_password_verify("hunter2", "$6$saltsalt$8iYtNHxjWRl.NF6oNZ5tF.iKFlQREaXBLlSmZKP6dy9l5z3vsooWNW0/GZ6Nej73/TFug6pIPSqbJoCT6dfnj.");
    CHECK(y == 1 || y == -1);
    CHECK(s6 == 1 || s6 == -1);
    if (y == 1) {
        CHECK_EQ_INT(kn_password_verify("hunter3", "$y$j9T$saltsaltsaltsalt$pQKob88sNg1.ktD6ni0OAUKcK4w22JpXSbU9UtU38wC"), 0);
        CHECK_EQ_INT(kn_password_needs_rehash("$y$j9T$saltsaltsaltsalt$pQKob88sNg1.ktD6ni0OAUKcK4w22JpXSbU9UtU38wC"), 1);
    }
    printf("crypt(3) support: yescrypt=%d sha512crypt=%d\n", y, s6);
#endif
}

static void test_tokens(void) {
    /* jwt.io sample */
    const char *jwt = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJzdWIiOiIxMjM0NTY3ODkwIiwibmFtZSI6IkpvaG4gRG9lIiwiaWF0IjoxNTE2MjM5MDIyfQ.SflKxwRJSMeKKF2QT4fwpMeJf36POk6yJV_adQssw5c";
    char err[64];
    cJSON *c = kn_signed_token_verify("your-256-bit-secret", jwt, 0, err, sizeof err);
    CHECK(c != NULL);
    CHECK_EQ_STR(cJSON_GetObjectItem(c, "name") ? cJSON_GetObjectItem(c, "name")->valuestring : NULL, "John Doe");
    cJSON_Delete(c);
    CHECK(kn_signed_token_verify("wrong", jwt, 0, err, sizeof err) == NULL);
    CHECK_EQ_STR(err, "bad signature");

    cJSON *claims = cJSON_CreateObject();
    cJSON_AddStringToObject(claims, "sid", "lobby-7");
    cJSON_AddNumberToObject(claims, "uid", 42);
    char *tok = kn_signed_token_create("s3cret", claims, 60);
    CHECK(tok != NULL);
    cJSON *v = kn_signed_token_verify("s3cret", tok, 0, err, sizeof err);
    CHECK(v != NULL);
    CHECK_EQ_STR(cJSON_GetObjectItem(v, "sid")->valuestring, "lobby-7");
    CHECK_EQ_INT(cJSON_GetObjectItem(v, "uid")->valuedouble, 42);
    CHECK(cJSON_GetObjectItem(v, "exp") && cJSON_GetObjectItem(v, "iat"));
    cJSON_Delete(v);
    /* expired */
    CHECK(kn_signed_token_verify("s3cret", tok, (int64_t)time(NULL) + 120, err, sizeof err) == NULL);
    CHECK_EQ_STR(err, "token expired");
    /* tampered payload */
    char *bad = strdup(tok);
    char *dot = strchr(bad, '.');
    dot[3] = dot[3] == 'A' ? 'B' : 'A';
    CHECK(kn_signed_token_verify("s3cret", bad, 0, err, sizeof err) == NULL);
    /* alg none / malformed */
    CHECK(kn_signed_token_verify("s3cret", "eyJhbGciOiJub25lIn0.e30.", 0, err, sizeof err) == NULL);
    CHECK(kn_signed_token_verify("s3cret", "a.b", 0, err, sizeof err) == NULL);
    CHECK(kn_signed_token_verify("s3cret", "a.b.c.d", 0, err, sizeof err) == NULL);
    CHECK(kn_signed_token_create("", claims, 10) == NULL);
    free(bad);
    free(tok);
    cJSON_Delete(claims);
}

int main(void) {
    test_digests();
    test_encodings();
    test_random();
    test_passwords();
    test_tokens();
    TEST_DONE("knsvc_crypto");
}
