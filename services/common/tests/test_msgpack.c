/* msgpack: byte-exact parity with msgpack-lite (v3.1.1's encoder), all formats, malformed input. */
#include "kn_test.h"
#include "knsvc/crypto.h"
#include "knsvc/msgpack.h"

/* node -e "require('msgpack-lite').encode(['init',1,-1,1.5,'x',{a:null,b:true},300,-300,70000,2**40,-0,'a'.repeat(40),[],{}])" */
static const char *LITE_HEX =
    "9ea4696e697401ffcb3ff8000000000000a17882a161c0a162c3cd012cd1fed4ce00011170cb427000000000000000d928"
    "616161616161616161616161616161616161616161616161616161616161616161616161616161619080";

static void test_lite_parity(void) {
    cJSON *a = cJSON_Parse("[\"init\",1,-1,1.5,\"x\",{\"a\":null,\"b\":true},300,-300,70000,1099511627776,-0,"
                           "\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",[],{}]");
    kn_buf b = {0};
    CHECK(kn_msgpack_from_json(a, &b) == 0);
    char hex[1024];
    kn_hex_encode(b.data, b.len, hex, sizeof hex);
    CHECK_EQ_STR(hex, LITE_HEX);
    /* and back */
    cJSON *back = kn_msgpack_to_json(b.data, b.len, NULL);
    char *s1 = cJSON_PrintUnformatted(a), *s2 = cJSON_PrintUnformatted(back);
    CHECK_EQ_STR(s2, s1);
    free(s1);
    free(s2);
    cJSON_Delete(back);
    /* Krunker framing */
    kn_buf f = {0};
    CHECK(kn_krunker_packet_encode(a, &f) == 0);
    CHECK_EQ_INT(f.len, b.len + 2);
    CHECK(f.data[f.len - 1] == 0 && f.data[f.len - 2] == 0);
    cJSON *p = kn_krunker_packet_decode(f.data, f.len);
    CHECK(cJSON_IsArray(p) && strcmp(cJSON_GetArrayItem(p, 0)->valuestring, "init") == 0);
    cJSON_Delete(p);
    CHECK(kn_krunker_packet_decode(b.data, b.len) == NULL);       /* missing trailer */
    CHECK(kn_krunker_packet_decode("\x91\x01\x00\x00\x05", 5) == NULL);
    kn_buf_free(&f);
    kn_buf_free(&b);
    cJSON_Delete(a);
}

static void test_formats(void) {
    kn_buf b = {0};
    int64_t ints[] = {0, 127, 128, 255, 256, 65535, 65536, 4294967295LL, 4294967296LL, -1, -32, -33, -128, -129,
                      -32768, -32769, -2147483648LL, -2147483649LL, INT64_MIN, INT64_MAX};
    size_t sizes[] = {1, 1, 2, 2, 3, 3, 5, 5, 9, 1, 1, 2, 2, 3, 3, 5, 5, 9, 9, 9};
    for (size_t i = 0; i < sizeof ints / sizeof *ints; i++) {
        kn_buf_reset(&b);
        kn_mp_write_int(&b, ints[i]);
        CHECK_EQ_INT(b.len, sizes[i]);
        kn_mp_reader r;
        kn_mp_value v;
        kn_mp_reader_init(&r, b.data, b.len);
        CHECK(kn_mp_read(&r, &v) == 0);
        int64_t got = v.type == KN_MP_UINT ? (int64_t)v.v.u : v.v.i;
        CHECK_EQ_INT(got, ints[i]);
    }
    kn_buf_reset(&b);
    kn_mp_write_uint(&b, UINT64_MAX);
    kn_mp_write_float(&b, 1.5f);
    kn_mp_write_double(&b, -2.25);
    kn_mp_write_bool(&b, 1);
    kn_mp_write_nil(&b);
    char big[70000];
    memset(big, 'z', sizeof big);
    kn_mp_write_str(&b, big, 31);
    kn_mp_write_str(&b, big, 255);
    kn_mp_write_str(&b, big, 65535);
    kn_mp_write_str(&b, big, 65536);
    kn_mp_write_bin(&b, "\x00\x01", 2);
    kn_mp_write_bin(&b, big, 300);
    kn_mp_write_array(&b, 16);
    for (int i = 0; i < 16; i++) kn_mp_write_int(&b, i);
    kn_mp_write_map(&b, 1);
    kn_mp_write_int(&b, 7); /* int key */
    kn_mp_write_cstr(&b, "v");
    /* fixext 1 */
    kn_buf_append(&b, "\xd4\x05\xaa", 3);

    kn_mp_reader r;
    kn_mp_value v;
    kn_mp_reader_init(&r, b.data, b.len);
    CHECK(kn_mp_read(&r, &v) == 0 && v.type == KN_MP_UINT && v.v.u == UINT64_MAX);
    CHECK(kn_mp_read(&r, &v) == 0 && v.type == KN_MP_FLOAT && v.v.f == 1.5);
    CHECK(kn_mp_read(&r, &v) == 0 && v.type == KN_MP_FLOAT && v.v.f == -2.25);
    CHECK(kn_mp_read(&r, &v) == 0 && v.type == KN_MP_BOOL && v.v.b == 1);
    CHECK(kn_mp_read(&r, &v) == 0 && v.type == KN_MP_NIL);
    size_t lens[] = {31, 255, 65535, 65536};
    for (int i = 0; i < 4; i++) CHECK(kn_mp_read(&r, &v) == 0 && v.type == KN_MP_STR && v.v.str.len == lens[i]);
    CHECK(kn_mp_read(&r, &v) == 0 && v.type == KN_MP_BIN && v.v.str.len == 2);
    CHECK(kn_mp_read(&r, &v) == 0 && v.type == KN_MP_BIN && v.v.str.len == 300);
    CHECK(kn_mp_skip(&r) == 0); /* array16 with children */
    size_t at_map = r.pos;
    CHECK(kn_mp_read(&r, &v) == 0 && v.type == KN_MP_MAP && v.v.count == 1);
    cJSON *m = kn_msgpack_to_json(b.data + at_map, b.len - at_map, NULL);
    CHECK(m && cJSON_GetObjectItem(m, "7") && strcmp(cJSON_GetObjectItem(m, "7")->valuestring, "v") == 0);
    cJSON_Delete(m);
    kn_mp_skip(&r);
    kn_mp_skip(&r);
    CHECK(kn_mp_read(&r, &v) == 0 && v.type == KN_MP_EXT && v.ext_type == 5 && v.v.str.len == 1);
    CHECK(kn_mp_read(&r, &v) != 0); /* end */
    kn_buf_free(&b);
}

static void test_malformed(void) {
    /* truncated / invalid inputs must fail cleanly */
    const char *bad[] = {"\xc1", "\xa5" "abc", "\xcd\x01", "\xdc\xff\xff", "\xdf\xff\xff\xff\xff", "\x92\x01",
                         "\xd9\x10" "short", "\xcb\x00\x00", "\xc7\x05\x01" "ab"};
    size_t lens[] = {1, 4, 2, 3, 5, 2, 7, 3, 5};
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        size_t used = 0;
        cJSON *j = kn_msgpack_to_json(bad[i], lens[i], &used);
        CHECK(j == NULL);
        cJSON_Delete(j);
    }
    /* deep nesting is bounded */
    char deep[200];
    memset(deep, 0x91, sizeof deep);
    deep[199] = 0x01;
    CHECK(kn_msgpack_to_json(deep, sizeof deep, NULL) == NULL);
    /* random bytes never crash */
    unsigned char rnd[64];
    for (int i = 0; i < 20000; i++) {
        kn_random_bytes(rnd, sizeof rnd);
        cJSON_Delete(kn_msgpack_to_json(rnd, (size_t)(rnd[0] % 64), NULL));
        kn_mp_reader r;
        kn_mp_reader_init(&r, rnd, sizeof rnd);
        kn_mp_skip(&r);
    }
    CHECK(1);
}

int main(void) {
    test_lite_parity();
    test_formats();
    test_malformed();
    TEST_DONE("knsvc_msgpack");
}
