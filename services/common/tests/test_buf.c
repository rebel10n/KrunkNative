/* kn_buf + kn_json + kn_url helpers. */
#include "kn_test.h"
#include "knsvc/buf.h"
#include "knsvc/http.h"
#include "knsvc/json.h"

static void test_buf(void) {
    kn_buf b = {0};
    CHECK(kn_buf_append_str(&b, "hello") == 0);
    CHECK_EQ_INT(b.len, 5);
    CHECK_EQ_STR(b.data, "hello");
    kn_buf_appendf(&b, " %d %s", 42, "x");
    CHECK_EQ_STR(b.data, "hello 42 x");
    kn_buf_consume(&b, 6);
    CHECK_EQ_STR(b.data, "42 x");
    for (int i = 0; i < 10000; i++) kn_buf_append_byte(&b, 'a');
    CHECK_EQ_INT(b.len, 10004);
    CHECK(b.data[b.len] == 0);
    size_t len;
    char *d = kn_buf_detach(&b, &len);
    CHECK_EQ_INT(len, 10004);
    CHECK(b.data == NULL && b.len == 0);
    free(d);
    kn_buf_consume(&b, 3);
    kn_buf_free(&b);
}

static void test_json(void) {
    cJSON *j = kn_json_parse("{\"a\":\"x\",\"n\":12.5,\"i\":7,\"t\":true}xx", 33);
    CHECK(j != NULL);
    CHECK_EQ_STR(kn_json_str(j, "a", "d"), "x");
    CHECK_EQ_STR(kn_json_str(j, "n", "d"), "d");
    CHECK(kn_json_num(j, "n", 0) == 12.5);
    CHECK_EQ_INT(kn_json_int(j, "i", 0), 7);
    CHECK_EQ_INT(kn_json_bool(j, "t", 0), 1);
    CHECK_EQ_INT(kn_json_int(j, "missing", -3), -3);
    char *s = kn_json_print(j);
    CHECK_EQ_STR(s, "{\"a\":\"x\",\"n\":12.5,\"i\":7,\"t\":true}");
    free(s);
    cJSON_Delete(j);
    CHECK(kn_json_parse("{bad", 4) == NULL);
}

static void test_url(void) {
    kn_url u;
    CHECK(kn_url_parse("ws://127.0.0.1:9000/internal?x=1#frag", &u) == 0);
    CHECK_EQ_STR(u.scheme, "ws");
    CHECK_EQ_STR(u.host, "127.0.0.1");
    CHECK_EQ_INT(u.port, 9000);
    CHECK_EQ_STR(u.path, "/internal?x=1");
    CHECK(kn_url_parse("HTTP://example.com", &u) == 0);
    CHECK_EQ_INT(u.port, 80);
    CHECK_EQ_STR(u.path, "/");
    CHECK(kn_url_parse("wss://[::1]/a", &u) == 0);
    CHECK_EQ_STR(u.host, "::1");
    CHECK_EQ_INT(u.port, 443);
    CHECK(kn_url_parse("ws://host:99999/", &u) != 0);
    CHECK(kn_url_parse("nohost", &u) != 0);
    CHECK(kn_url_parse("ws://:80/", &u) != 0);
    CHECK(kn_url_parse("ftp://h/", &u) != 0);
    char out[64];
    CHECK_EQ_INT(kn_url_decode("a%20b+c%2F", 10, out, sizeof out, 1), 6);
    CHECK_EQ_STR(out, "a b c/");
    CHECK(kn_url_decode("%zz", 3, out, sizeof out, 0) < 0);
    CHECK(kn_url_decode("%2", 2, out, sizeof out, 0) < 0);
    CHECK(kn_url_encode("a b/é", out, sizeof out) > 0);
    CHECK_EQ_STR(out, "a%20b%2F%C3%A9");
}

int main(void) {
    test_buf();
    test_json();
    test_url();
    TEST_DONE("knsvc_buf");
}
