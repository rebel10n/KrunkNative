// Official Krunker socket codec (shared/src/krunker_proto.c): ahNum rotation (AHKEY 0x17153 of
// the live build, 1597 of the older v10.js build, 'cptR' exempt), golden frames, encode/decode
// round trips of representative packets, and malformed frames failing cleanly (built with ASan +
// UBSan where available). Prints "Test OK".
//
// The golden bytes are derived by hand, not by running any Krunker JS: game.deob.js's
// socket.send writes @msgpack/msgpack's Encoder.encode([name].concat(args)) (game.deob.js:24186,
// 22484) followed by [ahNum >> 4 & 0xf, ahNum & 0xf] (iîiíïïí / iiíííìï, 24192), with
// ahNum = (ahNum + 0x17153) & 0xff before every send but 'cptR' (98214); the msgpack bytes
// follow the spec under that encoder's rules (safe integer -> smallest int, else float64).
#include <krunker_proto.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)

static void hex_print(const char *label, const uint8_t *p, size_t n) {
    printf("  %s (%zu):", label, n);
    for (size_t i = 0; i < n; i++) printf(" %02x", p[i]);
    printf("\n");
}

static int bytes_are(const KrunkerBuf *b, const uint8_t *want, size_t n, int line) {
    if (b->len == n && memcmp(b->data, want, n) == 0) return 1;
    printf("FAIL %s:%d: frame bytes differ\n", __FILE__, line);
    hex_print("got ", b->data, b->len);
    hex_print("want", want, n);
    g_fail = 1;
    return 0;
}
#define BYTES_ARE(buf, ...) do { static const uint8_t want_[] = {__VA_ARGS__}; bytes_are((buf), want_, sizeof want_, __LINE__); } while (0)

// ---------------------------------------------------------------- name tables

static void test_tables(void) {
    CHECK(strcmp(krunker_c2s_wire(KRUNKER_C2S_ENTER_GAME), "en") == 0);
    CHECK(strcmp(krunker_c2s_wire(KRUNKER_C2S_INPUT), "q") == 0);
    CHECK(strcmp(krunker_c2s_wire(KRUNKER_C2S_CHAT_MESSAGE), "ct") == 0);
    CHECK(strcmp(krunker_c2s_wire(KRUNKER_C2S_CHEAT_CHECK_RESPONSE), "ccr") == 0);
    CHECK(strcmp(krunker_c2s_key(KRUNKER_C2S_PING), "PING") == 0);
    CHECK(strcmp(krunker_s2c_wire(KRUNKER_S2C_IO_INIT), "io-init") == 0);
    CHECK(strcmp(krunker_s2c_wire(KRUNKER_S2C_ENTER_GAME), "frs") == 0); // differs from C->S 'en'
    CHECK(strcmp(krunker_s2c_key(KRUNKER_S2C_CHEAT_CHECK), "CHEAT_CHECK") == 0);
    CHECK(krunker_s2c_lookup("io-init", 7) == KRUNKER_S2C_IO_INIT);
    CHECK(krunker_s2c_lookup("cc", 2) == KRUNKER_S2C_CHEAT_CHECK);
    CHECK(krunker_s2c_lookup("_0", 2) == KRUNKER_S2C_RPC_SUCCESS);
    CHECK(krunker_c2s_lookup("_0", 2) == KRUNKER_C2S_RPC);
    CHECK(krunker_c2s_lookup("enx", 2) == KRUNKER_C2S_ENTER_GAME); // length-delimited
    CHECK(krunker_c2s_lookup("zzz-none", 8) == -1);
    CHECK(krunker_s2c_lookup(NULL, 0) == -1);
    CHECK(krunker_c2s_wire((KrunkerC2S)KRUNKER_C2S_COUNT) == NULL);
    CHECK(krunker_s2c_key((KrunkerS2C)-1) == NULL);
    // wire names are unique per direction (the reverse lookup is a function)
    for (int i = 0; i < KRUNKER_C2S_COUNT; i++)
        CHECK(krunker_c2s_lookup(krunker_c2s_wire((KrunkerC2S)i), strlen(krunker_c2s_wire((KrunkerC2S)i))) == i);
    for (int i = 0; i < KRUNKER_S2C_COUNT; i++)
        CHECK(krunker_s2c_lookup(krunker_s2c_wire((KrunkerS2C)i), strlen(krunker_s2c_wire((KrunkerS2C)i))) == i);
    CHECK(strlen(KRUNKER_CLIENT_VERSION) > 0 && strlen(KRUNKER_BUILD_VERSION) > 0);
    CHECK((KRUNKER_AHKEY & 0xffu) == KRUNKER_AHNUM_STEP);
    CHECK(strcmp(krunker_c2s_wire(KRUNKER_AHNUM_EXEMPT), KRUNKER_AHNUM_EXEMPT_WIRE) == 0);
}

// ---------------------------------------------------------------- ahNum

static void test_ahnum(void) {
    static const uint8_t live[16] = {83, 166, 249, 76, 159, 242, 69, 152, 235, 62, 145, 228, 55, 138, 221, 48};
    static const uint8_t v10[16] = {61, 122, 183, 244, 49, 110, 171, 232, 37, 98, 159, 220, 25, 86, 147, 208};
    KrunkerSession s;
    krunker_session_init(&s, 0x17153);
    CHECK(s.ah_num == 0);
    for (int i = 0; i < 16; i++) CHECK(krunker_proto_next_ahnum(&s, "q") == live[i]);
    // 256 sends come back to the start (step 0x53 is odd: a full cycle)
    for (int i = 16; i < 256; i++) krunker_proto_next_ahnum(&s, "q");
    CHECK(s.ah_num == 0);

    krunker_session_init(&s, 1597);
    for (int i = 0; i < 16; i++) CHECK(krunker_proto_next_ahnum(&s, "en") == v10[i]);

    // 'cptR' does not rotate; everything else does (incl. names that only start like it)
    krunker_session_init(&s, KRUNKER_AHKEY);
    CHECK(krunker_proto_next_ahnum(&s, "cptR") == 0);
    CHECK(krunker_proto_next_ahnum(&s, "en") == 83);
    CHECK(krunker_proto_next_ahnum(&s, "cptR") == 83);
    CHECK(krunker_proto_next_ahnum(&s, "cptR") == 83);
    CHECK(krunker_proto_next_ahnum(&s, "cpt") == 166);
    CHECK(krunker_proto_next_ahnum(&s, "cptRx") == 249);
    CHECK(krunker_proto_ahnum_exempt("cptR", 4) && !krunker_proto_ahnum_exempt("cptR", 3));
    krunker_session_reset(&s);
    CHECK(s.ah_num == 0 && s.ahkey == 0x17153);
    CHECK(krunker_proto_next_ahnum(&s, "po") == 83);

    // JS (a + k) & 0xff for any integral k: negative keys wrap like int32
    krunker_session_init(&s, -1);
    CHECK(krunker_proto_next_ahnum(&s, "q") == 255);
    CHECK(krunker_proto_next_ahnum(&s, "q") == 254);
}

// ---------------------------------------------------------------- golden frames

static void test_golden(void) {
    KrunkerSession s;
    KrunkerBuf b = {0};
    krunker_session_init(&s, 0x17153);

    // send(CHAT_MESSAGE, 0, "hi"): first send, ahNum 83 = 0x53 -> trailer 05 03
    const KrunkerValue ct[] = {KRUNKER_V_NUM(0), KRUNKER_V_STR("hi")};
    CHECK(krunker_proto_encode(&s, "ct", ct, 2, &b) == KRUNKER_PROTO_OK);
    BYTES_ARE(&b, 0x93, 0xa2, 'c', 't', 0x00, 0xa2, 'h', 'i', 0x05, 0x03);

    // send(ENTER_GAME, [1, -1, "x"], 16, 18, true, false, undefined): ahNum 166 = 0xa6
    const KrunkerValue en_arr[] = {KRUNKER_V_NUM(1), KRUNKER_V_NUM(-1), KRUNKER_V_STR("x")};
    const KrunkerValue en[] = {KRUNKER_V_ARRAY(en_arr, 3), KRUNKER_V_NUM(16), KRUNKER_V_NUM(18), KRUNKER_V_BOOL(1),
                               KRUNKER_V_BOOL(0), KRUNKER_V_NIL};
    CHECK(krunker_proto_encode(&s, "en", en, 6, &b) == KRUNKER_PROTO_OK);
    BYTES_ARE(&b, 0x97, 0xa2, 'e', 'n', 0x93, 0x01, 0xff, 0xa1, 'x', 0x10, 0x12, 0xc3, 0xc2, 0xc0, 0x0a, 0x06);

    // send('cptR', "tok"): no rotation, still 0xa6
    const KrunkerValue cpt[] = {KRUNKER_V_STR("tok")};
    CHECK(krunker_proto_encode(&s, "cptR", cpt, 1, &b) == KRUNKER_PROTO_OK);
    BYTES_ARE(&b, 0x92, 0xa4, 'c', 'p', 't', 'R', 0xa3, 't', 'o', 'k', 0x0a, 0x06);

    // send(INPUT, ...): ahNum 249 = 0xf9; number formats of Encoder.encodeNumber
    const KrunkerValue q_arr[] = {KRUNKER_V_NUM(0.1), KRUNKER_V_NUM(-33), KRUNKER_V_NUM(-32), KRUNKER_V_NUM(127),
                                  KRUNKER_V_NUM(128), KRUNKER_V_NUM(255), KRUNKER_V_NUM(256), KRUNKER_V_NUM(65536)};
    const KrunkerValue q[] = {KRUNKER_V_NUM(1234), KRUNKER_V_NUM(16.5), KRUNKER_V_NUM(-200),
                              KRUNKER_V_NUM(3000000000.0), // uint32 (msgpack-lite would write a float64)
                              KRUNKER_V_ARRAY(q_arr, 8), KRUNKER_V_MAP(NULL, 0)};
    CHECK(krunker_proto_encode(&s, "q", q, 6, &b) == KRUNKER_PROTO_OK);
    BYTES_ARE(&b, 0x97, 0xa1, 'q', 0xcd, 0x04, 0xd2, 0xcb, 0x40, 0x30, 0x80, 0, 0, 0, 0, 0, 0xd1, 0xff, 0x38, 0xce,
              0xb2, 0xd0, 0x5e, 0x00, 0x98, 0xcb, 0x3f, 0xb9, 0x99, 0x99, 0x99, 0x99, 0x99, 0x9a, 0xd0, 0xdf, 0xe0,
              0x7f, 0xcc, 0x80, 0xcc, 0xff, 0xcd, 0x01, 0x00, 0xce, 0x00, 0x01, 0x00, 0x00, 0x80, 0x0f, 0x09);
    CHECK(s.ah_num == 249);

    // 64-bit and edge numbers, explicit trailer 0xff -> 0f 0f
    const KrunkerValue big[] = {KRUNKER_V_NUM(1099511627776.0),     // 2^40: uint64
                                KRUNKER_V_NUM(-8589934592.0),       // -2^33: int64
                                KRUNKER_V_NUM(9007199254740992.0),  // 2^53: not a safe integer -> float64
                                KRUNKER_V_NUM(-0.0),                // isSafeInteger(-0), -0 >= 0 -> 0x00
                                KRUNKER_V_NUM(-2147483648.0), KRUNKER_V_NUM(-2147483649.0),
                                KRUNKER_V_NUM(-32768), KRUNKER_V_NUM(-32769)};
    CHECK(krunker_proto_encode_ah(0xff, "x", big, 8, &b) == KRUNKER_PROTO_OK);
    BYTES_ARE(&b, 0x99, 0xa1, 'x', 0xcf, 0, 0, 0x01, 0, 0, 0, 0, 0, 0xd3, 0xff, 0xff, 0xff, 0xfe, 0, 0, 0, 0, 0xcb,
              0x43, 0x40, 0, 0, 0, 0, 0, 0, 0x00, 0xd2, 0x80, 0, 0, 0, 0xd3, 0xff, 0xff, 0xff, 0xff, 0x7f, 0xff,
              0xff, 0xff, 0xd1, 0x80, 0x00, 0xd2, 0xff, 0xff, 0x7f, 0xff, 0x0f, 0x0f);

    // string / array / map headers at the format boundaries
    char s31[31], s32[32], s256[256];
    memset(s31, 'a', sizeof s31);
    memset(s32, 'b', sizeof s32);
    memset(s256, 'c', sizeof s256);
    KrunkerValue sixteen[16];
    for (int i = 0; i < 16; i++) sixteen[i] = KRUNKER_V_NUM(i);
    const KrunkerValue kv[] = {KRUNKER_V_STR("a"), KRUNKER_V_NUM(1), KRUNKER_V_NUM(7), KRUNKER_V_BOOL(1)};
    const uint8_t bin[2] = {0x01, 0xff};
    const KrunkerValue heads[] = {KRUNKER_V_STRN(s31, 31), KRUNKER_V_STRN(s32, 32), KRUNKER_V_STRN(s256, 256),
                                  KRUNKER_V_ARRAY(sixteen, 16), KRUNKER_V_MAP(kv, 2), KRUNKER_V_BIN(bin, 2)};
    CHECK(krunker_proto_encode_ah(0x10, "h", heads, 6, &b) == KRUNKER_PROTO_OK);
    if (b.len == 3 + 32 + 34 + 259 + 19 + 7 + 4 + 2) {
        const uint8_t *p = b.data;
        CHECK(p[0] == 0x97 && p[1] == 0xa1 && p[2] == 'h');
        CHECK(p[3] == 0xbf && p[4] == 'a');                         // fixstr 31
        CHECK(p[35] == 0xd9 && p[36] == 32 && p[37] == 'b');        // str8
        CHECK(p[69] == 0xda && p[70] == 0x01 && p[71] == 0x00);     // str16
        CHECK(p[328] == 0xdc && p[329] == 0x00 && p[330] == 0x10);  // array16
        CHECK(p[347] == 0x82 && p[348] == 0xa1 && p[349] == 'a' && p[350] == 0x01); // {"a":1,"7":true}
        CHECK(p[351] == 0xa1 && p[352] == '7' && p[353] == 0xc3);
        CHECK(p[354] == 0xc4 && p[355] == 2 && p[356] == 0x01 && p[357] == 0xff); // bin8
        CHECK(p[358] == 0x01 && p[359] == 0x00);                    // trailer of 0x10
    } else {
        printf("FAIL %s:%d: header frame is %zu bytes\n", __FILE__, __LINE__, b.len);
        g_fail = 1;
    }

    // a failed encode leaves the session where it was
    const KrunkerValue bad_key[] = {KRUNKER_V_BOOL(1), KRUNKER_V_NIL};
    const KrunkerValue bad[] = {KRUNKER_V_MAP(bad_key, 1)};
    krunker_session_init(&s, 0x17153);
    CHECK(krunker_proto_encode(&s, "q", bad, 1, &b) == KRUNKER_PROTO_EINVAL);
    CHECK(s.ah_num == 0 && b.len == 0);
    CHECK(krunker_proto_encode(&s, NULL, NULL, 0, &b) == KRUNKER_PROTO_EINVAL);
    CHECK(krunker_proto_encode(&s, "q", NULL, 1, &b) == KRUNKER_PROTO_EINVAL);
    CHECK(s.ah_num == 0);

    // nesting past the Encoder's maxDepth (100; the outer array is depth 1)
    KrunkerValue chain[120];
    for (int i = 0; i < 120; i++) chain[i] = i == 119 ? KRUNKER_V_NIL : KRUNKER_V_ARRAY(&chain[i + 1], 1);
    CHECK(krunker_proto_encode_ah(0, "d", &chain[120 - 99], 1, &b) == KRUNKER_PROTO_OK);   // 98 arrays + nil at depth 100
    CHECK(krunker_proto_encode_ah(0, "d", &chain[120 - 100], 1, &b) == KRUNKER_PROTO_EDEPTH);
    krunker_buf_free(&b);
}

// ---------------------------------------------------------------- decode

static void test_decode_server(void) {
    // io-init from the server: ["io-init", "abc123"] + 00 00
    static const uint8_t io_init[] = {0x92, 0xa7, 'i', 'o', '-', 'i', 'n', 'i', 't', 0xa6, 'a', 'b', 'c', '1', '2', '3', 0, 0};
    KrunkerFrame f;
    CHECK(krunker_proto_decode(io_init, sizeof io_init, 0, &f) == KRUNKER_PROTO_OK);
    CHECK(f.ah_echo == 0 && f.name && strcmp(f.name, "io-init") == 0 && f.name_len == 7 && f.argc == 1);
    CHECK(krunker_s2c_lookup(f.name, f.name_len) == KRUNKER_S2C_IO_INIT);
    CHECK(krunker_str(krunker_frame_arg(&f, 0)) && strcmp(krunker_str(krunker_frame_arg(&f, 0)), "abc123") == 0);
    CHECK(krunker_frame_arg(&f, 1) == NULL);
    krunker_frame_free(&f);
    CHECK(f.mem_ == NULL && f.args == NULL);
    krunker_frame_free(&f); // twice is fine

    // cc: [nonce, ciphertext] with an echo trailer 05 03 -> 83; uint32 nonce, str8 text
    static const uint8_t cc[] = {0x93, 0xa2, 'c', 'c', 0xce, 0x12, 0x34, 0x56, 0x78, 0xd9, 0x20,
                                 'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A',
                                 'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A', '=', '=', 0x05, 0x03};
    CHECK(krunker_proto_decode(cc, sizeof cc, KRUNKER_DECODE_STRICT_TRAILER, &f) == KRUNKER_PROTO_OK);
    CHECK(f.ah_echo == 83 && strcmp(f.name, "cc") == 0 && f.argc == 2);
    CHECK(krunker_num(krunker_frame_arg(&f, 0), -1) == 305419896.0);
    CHECK(krunker_frame_arg(&f, 1)->len == 32 && krunker_str(krunker_frame_arg(&f, 1))[31] == '=');
    krunker_frame_free(&f);

    // every scalar format the Decoder reads: float32, int8..int64, uint64, fixext, ext8, bin, nil key map
    static const uint8_t mix[] = {0x9b, 0xa1, 'm',
                                  0xca, 0x3f, 0xc0, 0x00, 0x00,                    // 1.5f
                                  0xd0, 0x80,                                      // -128
                                  0xd1, 0xff, 0xfe,                                // -2
                                  0xd2, 0xff, 0xff, 0xff, 0xfd,                    // -3
                                  0xd3, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xfc, // -4
                                  0xcf, 0x00, 0x20, 0, 0, 0, 0, 0, 0,              // 2^53
                                  0xd6, 0xff, 0x00, 0x00, 0x00, 0x01,              // timestamp32 ext -1
                                  0xc7, 0x01, 0x05, 0x2a,                          // ext8 type 5, 1 byte
                                  0xc4, 0x03, 0x00, 'z', 0x00,                     // bin with NULs
                                  0x82, 0x05, 0xc3, 0xa1, 'k', 0xa0,               // {5: true, "k": ""}
                                  0x0f, 0x0f};
    CHECK(krunker_proto_decode(mix, sizeof mix, 0, &f) == KRUNKER_PROTO_OK);
    CHECK(f.argc == 10 && f.ah_echo == 0xff);
    if (f.argc == 10) {
        CHECK(krunker_num(&f.args[0], 0) == 1.5);
        CHECK(krunker_num(&f.args[1], 0) == -128 && krunker_num(&f.args[2], 0) == -2);
        CHECK(krunker_num(&f.args[3], 0) == -3 && krunker_num(&f.args[4], 0) == -4);
        CHECK(krunker_num(&f.args[5], 0) == 9007199254740992.0);
        CHECK(f.args[6].type == KRUNKER_EXT && f.args[6].ext_type == -1 && f.args[6].len == 4);
        CHECK(f.args[7].type == KRUNKER_EXT && f.args[7].ext_type == 5 && f.args[7].u.str[0] == 0x2a);
        CHECK(f.args[8].type == KRUNKER_BIN && f.args[8].len == 3 && f.args[8].u.str[1] == 'z');
        CHECK(f.args[9].type == KRUNKER_MAP && f.args[9].len == 2);
        CHECK(krunker_num(krunker_get(&f.args[9], "5"), 0) == 1.0); // number key, read as JS "5"
        CHECK(krunker_str(krunker_get(&f.args[9], "k")) && krunker_get(&f.args[9], "k")->len == 0);
        CHECK(krunker_get(&f.args[9], "x") == NULL && krunker_get(&f.args[0], "k") == NULL);
        CHECK(krunker_at(&f.args[9], 0) == NULL && krunker_str(&f.args[0]) == NULL && krunker_num(&f.args[8], 7) == 7);
        KrunkerBuf j = {0};
        CHECK(krunker_value_to_json(&f.args[9], &j) == KRUNKER_PROTO_OK);
        CHECK(j.len == 17 && memcmp(j.data, "{\"5\":true,\"k\":\"\"}", 17) == 0);
        krunker_buf_free(&j);
    }
    krunker_frame_free(&f);
}

// encode -> decode -> re-encode gives the same bytes, the echo is the sent ahNum
static void round_trip(KrunkerSession *s, const char *name, const KrunkerValue *args, size_t argc) {
    KrunkerBuf b = {0}, again = {0};
    KrunkerFrame f;
    CHECK(krunker_proto_encode(s, name, args, argc, &b) == KRUNKER_PROTO_OK);
    const int rc = krunker_proto_decode(b.data, b.len, KRUNKER_DECODE_STRICT_TRAILER, &f);
    CHECK(rc == KRUNKER_PROTO_OK);
    if (rc == KRUNKER_PROTO_OK) {
        CHECK(f.ah_echo == s->ah_num);
        CHECK(strcmp(f.name, name) == 0 && f.argc == argc);
        CHECK(krunker_proto_encode_ah((uint8_t)f.ah_echo, f.name, f.args, f.argc, &again) == KRUNKER_PROTO_OK);
        if (again.len != b.len || memcmp(again.data, b.data, b.len) != 0) {
            printf("FAIL round trip of '%s'\n", name);
            hex_print("sent", b.data, b.len);
            hex_print("again", again.data, again.len);
            g_fail = 1;
        }
    }
    // every strict prefix fails (msgpack is prefix-free), never reads past the end
    for (size_t n = 0; n < b.len; n++) {
        uint8_t *cut = malloc(n ? n : 1);
        if (n) memcpy(cut, b.data, n);
        KrunkerFrame g;
        CHECK(krunker_proto_decode(cut, n, 0, &g) != KRUNKER_PROTO_OK);
        CHECK(g.mem_ == NULL && g.name == NULL);
        free(cut);
    }
    krunker_frame_free(&f);
    krunker_buf_free(&b);
    krunker_buf_free(&again);
}

static void test_round_trips(void) {
    KrunkerSession s;
    krunker_session_init(&s, 0x17153);

    // en: the 30-odd field profile array, then the flags (game.deob.js:208279, shortened)
    const KrunkerValue prof[] = {KRUNKER_V_NUM(7), KRUNKER_V_NUM(-1), KRUNKER_V_NUM(3), KRUNKER_V_NUM(-1),
                                 KRUNKER_V_NUM(-1), KRUNKER_V_NUM(2), KRUNKER_V_NUM(0), KRUNKER_V_NUM(1),
                                 KRUNKER_V_STR("Guest_42"), KRUNKER_V_NUM(-1), KRUNKER_V_NUM(0),
                                 KRUNKER_V_NUM(0.5), KRUNKER_V_NUM(1), KRUNKER_V_NUM(0), KRUNKER_V_NUM(-1),
                                 KRUNKER_V_NUM(-1), KRUNKER_V_NUM(-1), KRUNKER_V_NIL};
    const KrunkerValue en[] = {KRUNKER_V_ARRAY(prof, sizeof prof / sizeof prof[0]), KRUNKER_V_NUM(16), KRUNKER_V_NUM(18),
                               KRUNKER_V_BOOL(0), KRUNKER_V_BOOL(1), KRUNKER_V_BOOL(0), KRUNKER_V_BOOL(0),
                               KRUNKER_V_BOOL(0), KRUNKER_V_STR("ab"), KRUNKER_V_NIL, KRUNKER_V_BOOL(0),
                               KRUNKER_V_BOOL(0)};
    round_trip(&s, "en", en, sizeof en / sizeof en[0]);

    // q: [tick, delta, yaw, pitch, mouse/keys array, {key: value}]
    const KrunkerValue keys[] = {KRUNKER_V_NUM(1), KRUNKER_V_NUM(0), KRUNKER_V_NUM(-0.0123), KRUNKER_V_NUM(1.5707963267948966)};
    const KrunkerValue extra_kv[] = {KRUNKER_V_STR("s"), KRUNKER_V_NUM(1), KRUNKER_V_STR("w"), KRUNKER_V_NUM(70000)};
    const KrunkerValue q[] = {KRUNKER_V_NUM(4021), KRUNKER_V_NUM(16.666666666666668), KRUNKER_V_NUM(-3.0001),
                              KRUNKER_V_NUM(0.25), KRUNKER_V_ARRAY(keys, 4), KRUNKER_V_MAP(extra_kv, 2)};
    round_trip(&s, "q", q, 6);

    // ct: [chat channel, text] with UTF-8 and an embedded quote
    const KrunkerValue ct[] = {KRUNKER_V_NUM(0), KRUNKER_V_STR("gg \"wp\" \xc3\xa9\xe2\x9c\x93")};
    round_trip(&s, "ct", ct, 2);

    // ccr: [nonce, xorB64(JSON(R)), fnv hex] (game.deob.js:218205)
    const KrunkerValue ccr[] = {KRUNKER_V_NUM(2882400018.0), KRUNKER_V_STR("eyJyIjoxfQ=="), KRUNKER_V_STR("0a1b2c3d")};
    round_trip(&s, "ccr", ccr, 3);

    // io-init (server -> client shape) and a no-arg ping, then cptR (no rotation)
    const KrunkerValue io[] = {KRUNKER_V_STR("socket-7")};
    round_trip(&s, "io-init", io, 1);
    round_trip(&s, "po", NULL, 0);
    const uint8_t before = s.ah_num;
    const KrunkerValue tok[] = {KRUNKER_V_STR("captcha-token")};
    round_trip(&s, "cptR", tok, 1);
    CHECK(s.ah_num == before);

    // long sync-like packet: 20000 numbers (array32 / str16 / big payloads)
    enum { N = 20000 };
    KrunkerValue *many = malloc(N * sizeof *many);
    for (int i = 0; i < N; i++) many[i] = KRUNKER_V_NUM(i % 3 ? i * 0.5 : -i);
    const KrunkerValue l[] = {KRUNKER_V_ARRAY(many, N)};
    KrunkerBuf b = {0};
    KrunkerFrame f;
    CHECK(krunker_proto_encode(&s, "l", l, 1, &b) == KRUNKER_PROTO_OK);
    CHECK(krunker_proto_decode(b.data, b.len, 0, &f) == KRUNKER_PROTO_OK);
    CHECK(f.argc == 1 && f.args[0].len == N);
    CHECK(krunker_num(krunker_at(&f.args[0], N - 1), 0) == (N - 1) * 0.5);
    CHECK(krunker_num(krunker_at(&f.args[0], N - 2), 0) == -(N - 2));
    CHECK(krunker_at(&f.args[0], N) == NULL);
    krunker_frame_free(&f);
    krunker_buf_free(&b);
    free(many);
}

// ---------------------------------------------------------------- malformed frames

static int decode_rc(const uint8_t *p, size_t n, unsigned flags) {
    KrunkerFrame f;
    const int rc = krunker_proto_decode(p, n, flags, &f);
    if (rc != KRUNKER_PROTO_OK) CHECK(f.mem_ == NULL && f.args == NULL && f.argc == 0);
    krunker_frame_free(&f);
    return rc;
}
static int decode_vec(const uint8_t *p, size_t n) { return decode_rc(p, n, 0); }

static void test_malformed(void) {
    // short / empty
    CHECK(decode_vec(NULL, 0) == KRUNKER_PROTO_EINVAL);
    {
        static const uint8_t two[] = {0x90, 0x00};
        CHECK(decode_vec(two, 2) == KRUNKER_PROTO_ESHORT);
    }
    static const uint8_t ct_ok[] = {0x93, 0xa2, 'c', 't', 0x00, 0xa2, 'h', 'i', 0x05, 0x03};
    CHECK(decode_vec(ct_ok, sizeof ct_ok) == KRUNKER_PROTO_OK);
    // no trailer: the payload loses its last 2 bytes
    CHECK(decode_vec(ct_ok, sizeof ct_ok - 2) == KRUNKER_PROTO_EMSGPACK);
    // a 3-byte trailer: one byte left after the value ("Extra 1 of N byte(s)")
    static const uint8_t extra[] = {0x93, 0xa2, 'c', 't', 0x00, 0xa2, 'h', 'i', 0xc0, 0x05, 0x03};
    CHECK(decode_vec(extra, sizeof extra) == KRUNKER_PROTO_EEXTRA);
    // trailer nibbles > 0xf: the JS client ignores them (echo = (a << 4) + b); strict mode refuses
    static const uint8_t wide[] = {0x91, 0xa1, 'x', 0x10, 0xff};
    KrunkerFrame f;
    CHECK(krunker_proto_decode(wide, sizeof wide, 0, &f) == KRUNKER_PROTO_OK && f.ah_echo == 0x10 * 16 + 0xff);
    krunker_frame_free(&f);
    CHECK(decode_rc(wide, sizeof wide, KRUNKER_DECODE_STRICT_TRAILER) == KRUNKER_PROTO_ETRAILER);

    // payload not a non-empty array of [string, ...]
    static const uint8_t str[] = {0xa2, 'h', 'i', 0, 0};
    static const uint8_t empty_arr[] = {0x90, 0, 0};
    static const uint8_t map[] = {0x81, 0xa1, 'a', 0x01, 0, 0};
    static const uint8_t num[] = {0x2a, 0, 0};
    static const uint8_t num_name[] = {0x91, 0x01, 0, 0};
    static const uint8_t nil_name[] = {0x92, 0xc0, 0xa1, 'x', 0, 0};
    CHECK(decode_vec(str, sizeof str) == KRUNKER_PROTO_ENOTARRAY);
    CHECK(decode_vec(empty_arr, sizeof empty_arr) == KRUNKER_PROTO_ENOTARRAY);
    CHECK(decode_vec(map, sizeof map) == KRUNKER_PROTO_ENOTARRAY);
    CHECK(decode_vec(num, sizeof num) == KRUNKER_PROTO_ENOTARRAY);
    CHECK(decode_vec(num_name, sizeof num_name) == KRUNKER_PROTO_ENAME);
    CHECK(decode_vec(nil_name, sizeof nil_name) == KRUNKER_PROTO_ENAME);

    // invalid byte, counts / lengths past the end (no allocation of the claimed size)
    static const uint8_t c1[] = {0x91, 0xc1, 0, 0};
    static const uint8_t arr32[] = {0xdd, 0xff, 0xff, 0xff, 0xff, 0xa1, 'x', 0, 0};
    static const uint8_t map32[] = {0x92, 0xa1, 'x', 0xdf, 0xff, 0xff, 0xff, 0xff, 0, 0};
    static const uint8_t str32[] = {0x92, 0xa1, 'x', 0xdb, 0xff, 0xff, 0xff, 0xff, 'a', 0, 0};
    static const uint8_t bin32[] = {0x92, 0xa1, 'x', 0xc6, 0x7f, 0xff, 0xff, 0xff, 0, 0};
    static const uint8_t ext32[] = {0x92, 0xa1, 'x', 0xc9, 0x00, 0x00, 0x00, 0x09, 0x01, 0, 0};
    static const uint8_t fixext[] = {0x92, 0xa1, 'x', 0xd8, 0x01, 'a', 0, 0};
    static const uint8_t f64[] = {0x92, 0xa1, 'x', 0xcb, 0x40, 0x30, 0, 0};
    static const uint8_t u16[] = {0x92, 0xa1, 'x', 0xcd, 0x01, 0, 0};
    static const uint8_t len16[] = {0x92, 0xa1, 'x', 0xda, 0x01, 0, 0};
    CHECK(decode_vec(c1, sizeof c1) == KRUNKER_PROTO_EMSGPACK);
    CHECK(decode_vec(arr32, sizeof arr32) == KRUNKER_PROTO_EMSGPACK);
    CHECK(decode_vec(map32, sizeof map32) == KRUNKER_PROTO_EMSGPACK);
    CHECK(decode_vec(str32, sizeof str32) == KRUNKER_PROTO_EMSGPACK);
    CHECK(decode_vec(bin32, sizeof bin32) == KRUNKER_PROTO_EMSGPACK);
    CHECK(decode_vec(ext32, sizeof ext32) == KRUNKER_PROTO_EMSGPACK);
    CHECK(decode_vec(fixext, sizeof fixext) == KRUNKER_PROTO_EMSGPACK);
    CHECK(decode_vec(f64, sizeof f64) == KRUNKER_PROTO_EMSGPACK);
    CHECK(decode_vec(u16, sizeof u16) == KRUNKER_PROTO_EMSGPACK);
    CHECK(decode_vec(len16, sizeof len16) == KRUNKER_PROTO_EMSGPACK);

    // map keys: the Decoder refuses non string/number keys and "__proto__"
    static const uint8_t nil_key[] = {0x92, 0xa1, 'x', 0x81, 0xc0, 0x01, 0, 0};
    static const uint8_t arr_key[] = {0x92, 0xa1, 'x', 0x81, 0x90, 0x01, 0, 0};
    static const uint8_t proto_key[] = {0x92, 0xa1, 'x', 0x81, 0xa9, '_', '_', 'p', 'r', 'o', 't', 'o', '_', '_', 0x01, 0, 0};
    CHECK(decode_vec(nil_key, sizeof nil_key) == KRUNKER_PROTO_EKEY);
    CHECK(decode_vec(arr_key, sizeof arr_key) == KRUNKER_PROTO_EKEY);
    CHECK(decode_vec(proto_key, sizeof proto_key) == KRUNKER_PROTO_EKEY);

    // nesting: an arg of 98 nested arrays still decodes (its nil at depth 100), 99 do not; a deep
    // bomb fails cleanly
    for (int depth = 98; depth <= 99; depth++) {
        uint8_t deep[3 + 99 + 1 + 2];
        size_t n = 0;
        deep[n++] = 0x92;
        deep[n++] = 0xa1;
        deep[n++] = 'd';
        for (int i = 0; i < depth; i++) deep[n++] = 0x91;
        deep[n++] = 0xc0;
        deep[n++] = 0;
        deep[n++] = 0;
        CHECK(decode_vec(deep, n) == (depth == 98 ? KRUNKER_PROTO_OK : KRUNKER_PROTO_EDEPTH));
    }
    uint8_t *bomb = malloc(200005);
    bomb[0] = 0x92;
    bomb[1] = 0xa1;
    bomb[2] = 'd';
    memset(bomb + 3, 0x91, 200000);
    bomb[200003] = 0;
    bomb[200004] = 0;
    CHECK(decode_vec(bomb, 200005) == KRUNKER_PROTO_EDEPTH);
    free(bomb);

    CHECK(strcmp(krunker_proto_strerror(KRUNKER_PROTO_EEXTRA), "extra bytes after the msgpack value") == 0);
    CHECK(strcmp(krunker_proto_strerror(12345), "unknown error") == 0);
    CHECK(decode_rc(ct_ok, sizeof ct_ok, 0) == KRUNKER_PROTO_OK);
    CHECK(krunker_proto_decode(ct_ok, sizeof ct_ok, 0, NULL) == KRUNKER_PROTO_EINVAL);
}

// ---------------------------------------------------------------- fuzz-ish

static uint32_t g_rng = 0x2545f491u;
static uint32_t rnd(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

static void test_fuzz(void) {
    // random bytes biased towards msgpack heads
    static const uint8_t heads[] = {0x91, 0x92, 0x9f, 0xdc, 0xdd, 0x81, 0xde, 0xdf, 0xa3, 0xd9, 0xda, 0xdb, 0xc4,
                                    0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xd4, 0xd8, 0xca, 0xcb, 0xcf, 0xd3, 0xc0, 0xc1};
    uint8_t buf[96];
    int ok = 0;
    for (int iter = 0; iter < 200000; iter++) {
        const size_t n = rnd() % sizeof buf;
        for (size_t i = 0; i < n; i++) buf[i] = rnd() % 3 ? heads[rnd() % sizeof heads] : (uint8_t)rnd();
        if (n > 3 && rnd() % 2) { buf[0] = 0x90 | (rnd() % 6); buf[1] = 0xa1; buf[2] = 'z'; }
        uint8_t *exact = malloc(n ? n : 1); // exact-size copy so ASan sees any overread
        memcpy(exact, buf, n);
        KrunkerFrame f;
        const int rc = krunker_proto_decode(exact, n, rnd() & 1, &f);
        if (rc == KRUNKER_PROTO_OK) {
            ok++;
            KrunkerBuf j = {0}, re = {0};
            for (uint32_t i = 0; i < f.argc; i++) CHECK(krunker_value_to_json(&f.args[i], &j) == KRUNKER_PROTO_OK);
            // re-encoding a decoded frame works, except number keys that are not safe integers
            // (JS would print them with Number#toString; the encoder refuses them)
            const int erc = krunker_proto_encode_ah(0, f.name, f.args, f.argc, &re);
            CHECK(erc == KRUNKER_PROTO_OK || erc == KRUNKER_PROTO_EINVAL);
            krunker_buf_free(&j);
            krunker_buf_free(&re);
        }
        krunker_frame_free(&f);
        free(exact);
    }

    // bit flips of valid frames
    KrunkerSession s;
    KrunkerBuf b = {0};
    krunker_session_init(&s, 0x17153);
    const KrunkerValue inner[] = {KRUNKER_V_NUM(1.25), KRUNKER_V_STR("abc"), KRUNKER_V_NUM(-70000), KRUNKER_V_NIL};
    const KrunkerValue kv[] = {KRUNKER_V_STR("k"), KRUNKER_V_ARRAY(inner, 4)};
    const KrunkerValue args[] = {KRUNKER_V_NUM(300), KRUNKER_V_MAP(kv, 1), KRUNKER_V_STR("hello world"),
                                 KRUNKER_V_ARRAY(inner, 4), KRUNKER_V_BOOL(1)};
    CHECK(krunker_proto_encode(&s, "q", args, 5, &b) == KRUNKER_PROTO_OK);
    for (int iter = 0; iter < 50000; iter++) {
        uint8_t *m = malloc(b.len);
        memcpy(m, b.data, b.len);
        for (int k = 1 + (int)(rnd() % 3); k > 0; k--) m[rnd() % b.len] ^= (uint8_t)(1u << (rnd() % 8));
        const size_t n = rnd() % 4 ? b.len : rnd() % (b.len + 1);
        KrunkerFrame f;
        if (krunker_proto_decode(m, n, 0, &f) == KRUNKER_PROTO_OK) {
            KrunkerBuf j = {0};
            for (uint32_t i = 0; i < f.argc; i++) krunker_value_to_json(&f.args[i], &j);
            krunker_buf_free(&j);
        }
        krunker_frame_free(&f);
        free(m);
    }
    krunker_buf_free(&b);
    printf("  fuzz: %d random frames decoded ok\n", ok);
}

int main(void) {
    test_tables();
    test_ahnum();
    test_golden();
    test_decode_server();
    test_round_trips();
    test_malformed();
    test_fuzz();
    if (g_fail) {
        printf("krunker_proto_test FAILED\n");
        return 1;
    }
    printf("Test OK\n");
    return 0;
}
