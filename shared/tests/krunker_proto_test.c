// Official Krunker socket codec (shared/src/krunker_proto.c): ahNum rotation (the golden vectors
// use AHKEY 0x17153, the v10.0.2 build's key, 1597 of the older v10.js build; the live build's is
// KRUNKER_AHKEY, checked for self-consistency, not pinned to a value here), 'cptR' exempt), golden
// frames, encode/decode
// round trips of representative packets, and malformed frames failing cleanly (built with ASan +
// UBSan where available). Prints "Test OK".
//
// The golden bytes are derived by hand, not by running any Krunker JS: game.deob.js's
// socket.send writes @msgpack/msgpack's Encoder.encode([name].concat(args)) (game.deob.js:24186,
// 22484) followed by [ahNum >> 4 & 0xf, ahNum & 0xf] (iîiíïïí / iiíííìï, 24192), with
// ahNum = (ahNum + 0x17153) & 0xff before every send but 'cptR' (98214); the msgpack bytes
// follow the spec under that encoder's rules (safe integer -> smallest int, else float64).
#include <krunker_proto.h>
#include <krunker_build_live.h>
#include <auth_join.h>
#include <cJSON.h>

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

    // 'cptR' does not rotate; everything else does (incl. names that only start like it).
    // Uses the 0x17153 golden-vector key so the expected values hold across Krunker builds
    // (KRUNKER_AHKEY itself changes per build; its self-consistency is checked in test_tables).
    krunker_session_init(&s, 0x17153);
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

// ---------------------------------------------------------------- the live build (krunker_build_live.h)
// A published build.json (tools/krunker/build.json, modified the way a new Krunker build changes it)
// becomes the build the codec speaks; what this client cannot follow is refused with a reason.

#ifndef KN_SOURCE_DIR
#define KN_SOURCE_DIR "."
#endif

static char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = n > 0 ? malloc((size_t) n + 1) : NULL;
    if (buf && fread(buf, 1, (size_t) n, f) != (size_t) n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    if (buf) {
        buf[n] = 0;
        *len = (size_t) n;
    }
    return buf;
}

static cJSON *committed_doc(void) {
    size_t len = 0;
    char *text = slurp(KN_SOURCE_DIR "/tools/krunker/build.json", &len);
    cJSON *d = text ? cJSON_ParseWithLength(text, len) : NULL;
    free(text);
    if (!d) printf("FAIL: cannot read %s/tools/krunker/build.json\n", KN_SOURCE_DIR);
    return d;
}

static cJSON *packet(cJSON *doc, const char *side, const char *name) {
    cJSON *e;
    cJSON_ArrayForEach(e, cJSON_GetObjectItem(doc, side)) {
        if (!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(e, "name")), name)) return e;
    }
    return NULL;
}
static void set_wire(cJSON *doc, const char *side, const char *name, const char *wire) {
    cJSON *e = packet(doc, side, name);
    if (e) cJSON_ReplaceItemInObject(e, "wire", cJSON_CreateString(wire));
}
static cJSON *layout(cJSON *doc, const char *name) { return cJSON_GetObjectItem(cJSON_GetObjectItem(doc, "layouts"), name); }
static cJSON *field(cJSON *doc, const char *lay, const char *name) {
    cJSON *f;
    cJSON_ArrayForEach(f, cJSON_GetObjectItem(layout(doc, lay), "fields")) {
        if (!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(f, "name")), name)) return f;
    }
    return NULL;
}
static void set_offset(cJSON *doc, const char *lay, const char *name, const int off) {
    cJSON *f = field(doc, lay, name);
    if (f) cJSON_ReplaceItemInObject(f, "offset", cJSON_CreateNumber(off));
}
static void set_num(cJSON *o, const char *key, const double v) {
    if (cJSON_GetObjectItem(o, key)) cJSON_ReplaceItemInObject(o, key, cJSON_CreateNumber(v));
    else cJSON_AddNumberToObject(o, key, v);
}
static void set_str(cJSON *o, const char *key, const char *v) {
    if (cJSON_GetObjectItem(o, key)) cJSON_ReplaceItemInObject(o, key, cJSON_CreateString(v));
    else cJSON_AddStringToObject(o, key, v);
}

static int parse(cJSON *doc, KrunkerBuild **b, char *why, const size_t n) {
    char *text = cJSON_PrintUnformatted(doc);
    const int rc = krunker_build_parse(text, strlen(text), b, why, n);
    free(text);
    return rc;
}

// a new Krunker build: another AHKEY and versions, renamed packets, a new one, moved fields, a new field
static cJSON *next_build_doc(void) {
    cJSON *d = committed_doc();
    if (!d) return NULL;
    set_num(d, "ahkey", 0x17);
    set_str(d, "build_version", "LiveTestBuildVersion0123456789ab");
    set_str(d, "client_version", "10.0.9r2");
    set_wire(d, "packets_s2c", "ADD_PLAYERS", "A0");
    set_wire(d, "packets_s2c", "UPDATE_PLAYERS", "Ak");
    set_wire(d, "packets_c2s", "ENTER_GAME", "Aen");
    set_wire(d, "packets_c2s", "cptR", "cR2");
    set_str(cJSON_GetObjectItem(d, "ahnum_exempt"), "wire", "cR2");
    cJSON *np = cJSON_CreateObject();
    cJSON_AddStringToObject(np, "name", "BRAND_NEW_PACKET");
    cJSON_AddStringToObject(np, "wire", "bnp");
    cJSON_AddItemToArray(cJSON_GetObjectItem(d, "packets_s2c"), np);
    set_num(layout(d, "add_players"), "stride", 60);
    set_offset(d, "add_players", "sid", 55);
    set_offset(d, "add_players", "name", 56);
    cJSON *nf = cJSON_CreateObject();
    cJSON_AddNumberToObject(nf, "offset", 59);
    cJSON_AddStringToObject(nf, "name", "someNewField");
    cJSON_AddItemToArray(cJSON_GetObjectItem(layout(d, "add_players"), "fields"), nf);
    set_num(layout(d, "update_players"), "stride", 15);
    set_offset(d, "update_players", "sid", 13);
    set_offset(d, "update_players", "x", 14);
    cJSON *ka = cJSON_GetObjectItem(layout(d, "kill_player"), "args");
    set_num(ka, "victim", 2);
    set_num(ka, "killer", 0);
    cJSON *cheat = cJSON_GetObjectItem(d, "cheat");
    cJSON_DeleteItemFromObject(cheat, "salt_bytes");
    set_str(cheat, "salt", "N3W_SALT");
    set_str(cheat, "fnv_prime", "0x01000197");
    return d;
}

static void test_live_compiled_equal(void) {
    // the committed build.json is the header's build: parsed, it equals krunker_build_compiled()
    cJSON *d = committed_doc();
    KrunkerBuild *b = NULL;
    char why[240] = "";
    const int rc = d ? parse(d, &b, why, sizeof why) : -99;
    if (rc) printf("  committed build.json refused: %s\n", why);
    CHECK(rc == KRUNKER_BUILD_OK && b && b->live);
    const KrunkerBuild *c = krunker_build_compiled();
    CHECK(!c->live && krunker_build() == c);
    if (b) {
        CHECK(!strcmp(b->client_version, c->client_version) && !strcmp(b->build_version, c->build_version));
        CHECK(b->ahkey == c->ahkey && !strcmp(b->ahnum_exempt_wire, c->ahnum_exempt_wire));
        int same = 1;
        for (int i = 0; i < KRUNKER_C2S_COUNT; i++) same &= b->c2s_wire[i] && !strcmp(b->c2s_wire[i], c->c2s_wire[i]);
        for (int i = 0; i < KRUNKER_S2C_COUNT; i++) same &= b->s2c_wire[i] && !strcmp(b->s2c_wire[i], c->s2c_wire[i]);
        CHECK(same);
        CHECK(b->add_stride == KRUNKER_ADD_STRIDE && !memcmp(b->add, c->add, sizeof b->add));
        CHECK(b->k_stride == KRUNKER_K_STRIDE && !memcmp(b->k, c->k, sizeof b->k));
        CHECK(b->sync_send_vals == KRUNKER_SYNC_SEND_VALS && !memcmp(b->sync, c->sync, sizeof b->sync));
        CHECK(!memcmp(b->kill, c->kill, sizeof b->kill) && !memcmp(b->damage, c->damage, sizeof b->damage) &&
              !memcmp(b->health, c->health, sizeof b->health) && !memcmp(b->score, c->score, sizeof b->score));
        CHECK(b->cheat_salt_len == KRUNKER_CHEAT_SALT_LEN && !memcmp(b->cheat_salt, c->cheat_salt, b->cheat_salt_len));
        CHECK(b->cheat_fnv_offset == KRUNKER_CHEAT_FNV_OFFSET && b->cheat_fnv_prime == KRUNKER_CHEAT_FNV_PRIME &&
              b->cheat_fnv_double == KRUNKER_CHEAT_FNV_DOUBLE_MUL);
        CHECK(!strcmp(b->matchmaker_url, KRUNKER_MATCHMAKER_URL) && !strcmp(b->turnstile_site_key, KRUNKER_TURNSTILE_SITE_KEY));
        CHECK(b->region_count == KRUNKER_REGION_COUNT && !strcmp(b->regions[0], c->regions[0]));
        CHECK(c->add[KRB_ADD_X] == KRUNKER_ADD_X && c->k[KRB_K_SID] == KRUNKER_K_SID && c->sync[KRB_SYNC_PING] == KRUNKER_SYNC_PING);
    }
    krunker_build_free(b);
    cJSON_Delete(d);
}

static void test_live_values(void) {
    // the codec speaks the build in use: its wire names, its AHKEY, its exempt packet
    cJSON *d = next_build_doc();
    KrunkerBuild *b = NULL;
    char why[240] = "";
    const int rc = d ? parse(d, &b, why, sizeof why) : -99;
    if (rc) printf("  next build refused: %s\n", why);
    CHECK(rc == KRUNKER_BUILD_OK && b);
    if (!b) {
        cJSON_Delete(d);
        return;
    }
    CHECK(!strcmp(b->build_version, "LiveTestBuildVersion0123456789ab") && !strcmp(b->client_version, "10.0.9r2") && b->ahkey == 0x17);
    CHECK(b->add_stride == 60 && b->add[KRB_ADD_SID] == 55 && b->add[KRB_ADD_NAME] == 56 && b->add[KRB_ADD_X] == KRUNKER_ADD_X);
    CHECK(b->k_stride == 15 && b->k[KRB_K_SID] == 13 && b->k[KRB_K_X] == 14);
    CHECK(b->kill[KRB_KILL_VICTIM] == 2 && b->kill[KRB_KILL_KILLER] == 0);
    CHECK(b->cheat_salt_len == 8 && !memcmp(b->cheat_salt, "N3W_SALT", 8) && b->cheat_fnv_prime == 0x01000197u);
    krunker_build_use(b);
    CHECK(krunker_build() == b);
    CHECK(!strcmp(krunker_c2s_wire(KRUNKER_C2S_ENTER_GAME), "Aen") && !strcmp(krunker_c2s_key(KRUNKER_C2S_ENTER_GAME), "ENTER_GAME"));
    CHECK(krunker_s2c_lookup("A0", 2) == KRUNKER_S2C_ADD_PLAYERS && krunker_s2c_lookup("Ak", 2) == KRUNKER_S2C_UPDATE_PLAYERS);
    CHECK(krunker_s2c_lookup("0", 1) == -1 && krunker_s2c_lookup("k", 1) == -1); // the old names are no packet now
    CHECK(krunker_s2c_lookup("bnp", 3) == -1); // a packet this client does not know: ignored
    KrunkerSession s;
    krunker_session_init(&s, krunker_build()->ahkey);
    CHECK(krunker_proto_next_ahnum(&s, "Aen") == 0x17);
    CHECK(krunker_proto_next_ahnum(&s, "cR2") == 0x17);        // the new exempt packet does not rotate
    CHECK(krunker_proto_next_ahnum(&s, "cptR") == 0x2e);       // the old one does now
    KrunkerBuf out = {0};
    CHECK(krunker_proto_encode(&s, krunker_c2s_wire(KRUNKER_C2S_ENTER_GAME), NULL, 0, &out) == KRUNKER_PROTO_OK &&
          out.len >= 2 && out.data[out.len - 2] == (0x45 >> 4) && out.data[out.len - 1] == (0x45 & 0xf));
    krunker_buf_free(&out);
    krunker_build_use(NULL);
    CHECK(krunker_build() == krunker_build_compiled() && krunker_s2c_lookup("0", 1) == KRUNKER_S2C_ADD_PLAYERS);
    krunker_build_free(b);
    cJSON_Delete(d);
}

typedef void (*Breaker)(cJSON *d);
static void br_no_sid(cJSON *d) { cJSON_DeleteItemFromArray(cJSON_GetObjectItem(layout(d, "add_players"), "fields"), 1); }
static void br_no_sync(cJSON *d) {
    cJSON *list = cJSON_GetObjectItem(d, "packets_s2c");
    int i = 0;
    cJSON *e;
    cJSON_ArrayForEach(e, list) {
        if (!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(e, "name")), "SYNC_PLAYER")) break;
        i++;
    }
    cJSON_DeleteItemFromArray(list, i);
}
static void br_keystream(cJSON *d) { set_str(cJSON_GetObjectItem(d, "cheat"), "keystream_formula", "v2"); }
static void br_offset(cJSON *d) { set_offset(d, "update_players", "x", 40); }
static void br_schema(cJSON *d) { set_num(d, "schema", 2); }
static void br_trailer(cJSON *d) { set_str(d, "ahnum_trailer", "bytes"); }
static void br_victim(cJSON *d) { cJSON_DeleteItemFromObject(cJSON_GetObjectItem(layout(d, "kill_player"), "args"), "victim"); }
static void br_https(cJSON *d) { set_str(cJSON_GetObjectItem(d, "extras"), "matchmaker_url", "http://matchmaker.example"); }
static void br_cc(cJSON *d) { set_num(cJSON_GetObjectItem(cJSON_GetObjectItem(d, "cheat"), "cc_args"), "ciphertext", 1); }
static void br_klist(cJSON *d) { set_num(cJSON_GetObjectItem(layout(d, "update_players"), "args"), "list", 1); }
static void br_mul(cJSON *d) { set_str(cJSON_GetObjectItem(d, "cheat"), "fnv_multiply", "bigint"); }
static void br_no_end(cJSON *d) {
    cJSON *list = cJSON_GetObjectItem(d, "packets_s2c");
    int i = 0;
    cJSON *e;
    cJSON_ArrayForEach(e, list) {
        if (!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(e, "name")), "END_GAME")) break;
        i++;
    }
    cJSON_DeleteItemFromArray(list, i);
}

static void test_live_refusals(void) {
    // a build this client cannot follow is refused (ESHAPE), naming what changed
    const struct {
        Breaker brk;
        const char *want;
    } cases[] = {
        {br_no_sid, "ADD_PLAYERS no longer has \"sid\""},
        {br_no_sync, "server packet SYNC_PLAYER is gone"},
        {br_keystream, "keystream changed (v2)"},
        {br_offset, "UPDATE_PLAYERS field \"x\" at 40 is outside"},
        {br_schema, "schema"},
        {br_trailer, "trailer"},
        {br_victim, "KILL_PLAYER no longer has the argument \"victim\""},
        {br_https, "not https"},
        {br_cc, "cheat check's arguments moved"},
        {br_klist, "player list moved"},
        {br_mul, "hash step changed (bigint)"},
        {br_no_end, "server packet END_GAME is gone"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        cJSON *d = committed_doc();
        if (!d) return;
        cases[i].brk(d);
        KrunkerBuild *b = NULL;
        char why[240] = "";
        const int rc = parse(d, &b, why, sizeof why);
        if (rc != KRUNKER_BUILD_ESHAPE || !strstr(why, cases[i].want))
            printf("  refusal %zu: rc %d why \"%s\" (want \"%s\")\n", i, rc, why, cases[i].want);
        CHECK(rc == KRUNKER_BUILD_ESHAPE && !b && strstr(why, cases[i].want));
        CHECK(krunker_build() == krunker_build_compiled());
        cJSON_Delete(d);
    }
    // an optional (cosmetic) packet may go: the build is accepted, the packet has no wire
    cJSON *od = committed_doc();
    if (od) {
        cJSON *list = cJSON_GetObjectItem(od, "packets_s2c");
        int i = 0;
        cJSON *e;
        cJSON_ArrayForEach(e, list) {
            if (!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(e, "name")), "ADD_MEDAL")) break;
            i++;
        }
        cJSON_DeleteItemFromArray(list, i);
        KrunkerBuild *ob = NULL;
        char owhy[240] = "";
        CHECK(parse(od, &ob, owhy, sizeof owhy) == KRUNKER_BUILD_OK && ob && !ob->s2c_wire[KRUNKER_S2C_ADD_MEDAL] &&
              ob->s2c_wire[KRUNKER_S2C_END_GAME]);
        krunker_build_free(ob);
        cJSON_Delete(od);
    }
    // not a document at all: EJSON (a broken download, the fallback's case)
    KrunkerBuild *b = NULL;
    char why[240];
    CHECK(krunker_build_parse("not json", 8, &b, why, sizeof why) == KRUNKER_BUILD_EJSON && !b);
    CHECK(krunker_build_parse("[1,2]", 5, &b, why, sizeof why) == KRUNKER_BUILD_EJSON && !b);
    // a document without the optional extras keeps the compiled ones
    cJSON *d = committed_doc();
    if (d) {
        cJSON_DeleteItemFromObject(d, "extras");
        CHECK(parse(d, &b, why, sizeof why) == KRUNKER_BUILD_OK && b && !strcmp(b->matchmaker_url, KRUNKER_MATCHMAKER_URL) &&
              b->region_count == KRUNKER_REGION_COUNT);
        krunker_build_free(b);
        cJSON_Delete(d);
    }
}

static void test_live_manifest(void) {
    // the manifest pins the document by sha256 and versions
    cJSON *d = next_build_doc();
    if (!d) return;
    char *doc = cJSON_PrintUnformatted(d);
    const size_t len = strlen(doc);
    uint8_t dig[32];
    char sha[65], json[512];
    kn_sha256_bytes(doc, len, dig);
    kn_sha256_hex(dig, sha);
    snprintf(json, sizeof json,
             "{\"schema\":1,\"build_version\":\"LiveTestBuildVersion0123456789ab\",\"client_version\":\"10.0.9r2\","
             "\"sha256\":\"%s\",\"path\":\"krunker/%s.json\",\"published\":\"2026-10-15T12:00:00Z\"}",
             sha, sha);
    KrunkerBuildManifest m;
    char why[240] = "";
    CHECK(krunker_build_manifest_parse(json, strlen(json), &m, why, sizeof why) == KRUNKER_BUILD_OK && !strcmp(m.sha256, sha));
    KrunkerBuild *b = NULL;
    CHECK(krunker_build_accept(&m, doc, len, &b, why, sizeof why) == KRUNKER_BUILD_OK && b && b->ahkey == 0x17);
    krunker_build_free(b);
    b = NULL;
    doc[len / 2] ^= 1; // one flipped bit: refused as a broken download (EJSON), not as a shape change
    CHECK(krunker_build_accept(&m, doc, len, &b, why, sizeof why) == KRUNKER_BUILD_EJSON && !b && strstr(why, "sha256"));
    doc[len / 2] ^= 1;
    KrunkerBuildManifest other = m;
    snprintf(other.build_version, sizeof other.build_version, "%s", "SomeOtherBuild");
    CHECK(krunker_build_accept(&other, doc, len, &b, why, sizeof why) == KRUNKER_BUILD_EJSON && !b);
    CHECK(krunker_build_manifest_parse("{\"schema\":2,\"build_version\":\"x\"}", 34, &m, why, sizeof why) == KRUNKER_BUILD_ESHAPE);
    CHECK(krunker_build_manifest_parse("{\"schema\":1,\"build_version\":\"x\"}", 34, &m, why, sizeof why) == KRUNKER_BUILD_EJSON);
    char bad[512];
    snprintf(bad, sizeof bad, "{\"schema\":1,\"build_version\":\"a\",\"client_version\":\"1.0.0\",\"sha256\":\"%s\",\"path\":\"../x.json\"}", sha);
    CHECK(krunker_build_manifest_parse(bad, strlen(bad), &m, why, sizeof why) == KRUNKER_BUILD_EJSON);
    // the document's URL: next to the manifest's directory
    KrunkerBuildManifest u = {.path = "krunker/abc.json"};
    char url[256];
    CHECK(krunker_build_doc_url("https://dl.krunk.io/updates/client/krunker-build.json", &u, url, sizeof url) &&
          !strcmp(url, "https://dl.krunk.io/updates/krunker/abc.json"));
    CHECK(krunker_build_doc_url("http://127.0.0.1:8080/updates/client/krunker-build.json?x=1", &u, url, sizeof url) &&
          !strcmp(url, "http://127.0.0.1:8080/updates/krunker/abc.json"));
    CHECK(!krunker_build_doc_url("https://host/krunker-build.json", &u, url, sizeof url));
    free(doc);
    cJSON_Delete(d);
}

// ---------------------------------------------------------------- v10.0.3 argument shapes
// Frames shaped as the v10.0.3 server packs them (the shared server simulation in the decoded bundle,
// cited deob-10.0.3.js:<line>), decoded and read through the compiled build's offsets the way
// client/src/net/krunker_link.c reads them. doc/v10/net.md §9 keeps the packet-by-packet table.

// "0" ADD_PLAYERS: getPlayerAddData (deob-10.0.3.js:198809) packs 53 values a player; the client walks
// the list `i += 53` (:258194) and passes [21] (player.propID) as players.add's 25th parameter (:258132,
// updateProp(player, propID || 1) :195129). [17] is customLoadout, [49] the clan again (players.add's
// `clan` parameter, overwritten by [11] in the handler).
static void test_shapes_1003_add(void) {
    CHECK(KRUNKER_ADD_STRIDE == 53);
    const KrunkerBuild *b = krunker_build_compiled();
    // the offsets the link reads, as players.add / the handler's assignments use them (:258132-258150)
    CHECK(b->add[KRB_ADD_ID] == 0 && b->add[KRB_ADD_SID] == 1 && b->add[KRB_ADD_X] == 2 && b->add[KRB_ADD_Z] == 4);
    CHECK(b->add[KRB_ADD_NAME] == 5 && b->add[KRB_ADD_CLASS_INDEX] == 6 && b->add[KRB_ADD_HEALTH] == 7);
    CHECK(b->add[KRB_ADD_MAX_HEALTH] == 8 && b->add[KRB_ADD_TEAM] == 9 && b->add[KRB_ADD_LEVEL] == 10 && b->add[KRB_ADD_CLAN] == 11);
    CHECK(b->add[KRB_ADD_SEC_INDEX] == 15 && b->add[KRB_ADD_IS_HACKER] == 16 && b->add[KRB_ADD_X_DIRE] == 18);
    CHECK(b->add[KRB_ADD_ATTACH_INDEX] == 22 && b->add[KRB_ADD_PC_STAT_INDEX] == 23 && b->add[KRB_ADD_ALIAS] == 28);
    CHECK(b->add[KRB_ADD_ACCID] == 31 && b->add[KRB_ADD_IS_BOT] == 44 && b->add[KRB_ADD_CREATOR_BADGE] == 51 &&
          b->add[KRB_ADD_TITLE_INDEX] == 52);
    // 21 (propID) and 17 (customLoadout) are no named field: the link reads propID at 21 itself
    for (int slot = 0; slot < KRB_ADD_COUNT; slot++) CHECK(b->add[slot] != 21 && b->add[slot] != 17);

    // one Prop Hunt player as the server sends it: rounded position / health, propID 7
    KrunkerValue e[53];
    for (int i = 0; i < 53; i++) e[i] = KRUNKER_V_NUM(-1);
    e[0] = KRUNKER_V_STR("sockB"); e[1] = KRUNKER_V_NUM(7); e[2] = KRUNKER_V_NUM(12); e[3] = KRUNKER_V_NUM(-3);
    e[4] = KRUNKER_V_NUM(40); e[5] = KRUNKER_V_STR("Guest_7"); e[6] = KRUNKER_V_NUM(2); e[7] = KRUNKER_V_NUM(80);
    e[8] = KRUNKER_V_NUM(100); e[9] = KRUNKER_V_NUM(1); e[10] = KRUNKER_V_NUM(0); e[11] = KRUNKER_V_NUM(0);
    e[12] = KRUNKER_V_NUM(0); e[16] = KRUNKER_V_NUM(0); e[17] = KRUNKER_V_NIL; e[18] = KRUNKER_V_NUM(1.5707963267948966);
    e[21] = KRUNKER_V_NUM(7); e[22] = KRUNKER_V_NUM(0); e[28] = KRUNKER_V_STR("Guest_7"); e[38] = KRUNKER_V_NUM(1.5);
    e[50] = KRUNKER_V_NIL;
    const KrunkerValue add[] = {KRUNKER_V_ARRAY(e, 53)};
    KrunkerBuf buf = {0};
    KrunkerFrame f;
    CHECK(krunker_proto_encode_ah(0, krunker_s2c_wire(KRUNKER_S2C_ADD_PLAYERS), add, 1, &buf) == KRUNKER_PROTO_OK);
    CHECK(krunker_proto_decode(buf.data, buf.len, KRUNKER_DECODE_STRICT_TRAILER, &f) == KRUNKER_PROTO_OK);
    const KrunkerValue *list = krunker_frame_arg(&f, 0);
    CHECK(krunker_is(list, KRUNKER_ARRAY) && list->len == 53);
    if (krunker_is(list, KRUNKER_ARRAY) && list->len == 53) {
        CHECK(!strcmp(krunker_str(krunker_at(list, (uint32_t) b->add[KRB_ADD_ID])), "sockB"));
        CHECK(krunker_num(krunker_at(list, (uint32_t) b->add[KRB_ADD_SID]), 0) == 7);
        CHECK(krunker_num(krunker_at(list, (uint32_t) b->add[KRB_ADD_HEALTH]), 0) == 80);
        CHECK(krunker_num(krunker_at(list, (uint32_t) b->add[KRB_ADD_X_DIRE]), 0) == 1.5707963267948966);
        CHECK(krunker_num(krunker_at(list, 21), 0) == 7);                       // propID
        CHECK(krunker_is(krunker_at(list, 17), KRUNKER_NIL));                   // customLoadout null
        CHECK(krunker_num(krunker_at(list, 38), 0) == 1.5);                     // parseFloat(headMlt): float64
    }
    krunker_frame_free(&f);
    krunker_buf_free(&buf);
}

// "l" SYNC_PLAYER: syncState (deob-10.0.3.js:222389-222404) pushes the 25 movement values, then custSync
// ([25], a flat list or 0), the movement scheme's sendVals, then the controlled entity [eid, tick, state] or
// 0. An inactive player gets a bare 0 (:222407). [1] is Math.round(ping); the handler takes it only when
// truthy (:258311). [13] onWall is sent as 0 / 1 (`onWall ? 1 : 0`).
static void test_shapes_1003_sync(void) {
    const KrunkerBuild *b = krunker_build_compiled();
    CHECK(KRUNKER_SYNC_SEND_VALS == 26 && b->sync[KRB_SYNC_PROTECTED] == 25);
    CHECK(b->sync[KRB_SYNC_INPUT_SN] == 0 && b->sync[KRB_SYNC_PING] == 1 && b->sync[KRB_SYNC_VEL_Y] == 5 &&
          b->sync[KRB_SYNC_VEL_X] == 6 && b->sync[KRB_SYNC_VEL_Z] == 7 && b->sync[KRB_SYNC_ON_WALL] == 13 &&
          b->sync[KRB_SYNC_GRAPPLE] == 23 && b->sync[KRB_SYNC_CAN_SLIDE_T] == 24);
    KrunkerValue s[27];
    for (int i = 0; i < 27; i++) s[i] = KRUNKER_V_NUM(0);
    s[0] = KRUNKER_V_NUM(412);          // inputSN
    s[1] = KRUNKER_V_NUM(0);            // ping 0: the handler keeps the last one
    s[2] = KRUNKER_V_NUM(10.25); s[3] = KRUNKER_V_NUM(4.5); s[4] = KRUNKER_V_NUM(-7.125);
    s[5] = KRUNKER_V_NUM(-0.0025); s[6] = KRUNKER_V_NUM(0.01); s[7] = KRUNKER_V_NUM(-0.02); // velocity.y, .x, .z
    s[13] = KRUNKER_V_NUM(1);           // onWall ? 1 : 0
    s[15] = KRUNKER_V_NUM(1);           // aimVal: 1 = hip fire
    s[23] = KRUNKER_V_NUM(0);           // grapple || 0
    s[24] = KRUNKER_V_NUM(350);         // canSlideT (ms)
    s[25] = KRUNKER_V_NUM(0);           // custSync: none
    s[26] = KRUNKER_V_NUM(0);           // movSch 0 has no sendVals: the controlled entity, none
    const KrunkerValue l[] = {KRUNKER_V_ARRAY(s, 27)};
    KrunkerBuf buf = {0};
    KrunkerFrame f;
    CHECK(krunker_proto_encode_ah(0, krunker_s2c_wire(KRUNKER_S2C_SYNC_PLAYER), l, 1, &buf) == KRUNKER_PROTO_OK);
    CHECK(krunker_proto_decode(buf.data, buf.len, 0, &f) == KRUNKER_PROTO_OK);
    const KrunkerValue *a = krunker_frame_arg(&f, 0);
    CHECK(krunker_is(a, KRUNKER_ARRAY) && a->len == 27);
    CHECK(krunker_num(krunker_at(a, (uint32_t) b->sync[KRB_SYNC_VEL_Y]), 0) == -0.0025);
    CHECK(krunker_num(krunker_at(a, (uint32_t) b->sync[KRB_SYNC_PING]), -1) == 0);
    CHECK(krunker_num(krunker_at(a, (uint32_t) KRUNKER_SYNC_SEND_VALS), -1) == 0);
    krunker_frame_free(&f);
    // an inactive player: ["l", 0]
    const KrunkerValue zero[] = {KRUNKER_V_NUM(0)};
    CHECK(krunker_proto_encode_ah(0, krunker_s2c_wire(KRUNKER_S2C_SYNC_PLAYER), zero, 1, &buf) == KRUNKER_PROTO_OK);
    BYTES_ARE(&buf, 0x92, 0xa1, 'l', 0x00, 0x00, 0x00);
    krunker_buf_free(&buf);
}

// "k" UPDATE_PLAYERS [list, sendFrequency, now - startTime]: iïÍíìïî (deob-10.0.3.js:222324) pushes 13 values
// a player: sid, x / y / z .round(2), Math.round(deg(xDire)), Math.round(deg(yDire)), stepVal, onGround 0 / 1,
// Math.round(crouchVal), loadoutIndex, aimVal ? 1 : 0, [gx, gy, gz] rounded or 0, Math.round(ping).
static void test_shapes_1003_k(void) {
    const KrunkerBuild *b = krunker_build_compiled();
    CHECK(KRUNKER_K_STRIDE == 13 && b->k[KRB_K_SID] == 0 && b->k[KRB_K_X_DIRE] == 4 && b->k[KRB_K_Y_DIRE] == 5 &&
          b->k[KRB_K_STEP_VAL] == 6 && b->k[KRB_K_AIM_VAL] == 10 && b->k[KRB_K_GRAPPLE] == 11 && b->k[KRB_K_PING] == 12);
    const KrunkerValue g[] = {KRUNKER_V_NUM(5), KRUNKER_V_NUM(20), KRUNKER_V_NUM(-3)};
    const KrunkerValue p[] = {KRUNKER_V_NUM(7), KRUNKER_V_NUM(1.25), KRUNKER_V_NUM(2), KRUNKER_V_NUM(-3.5), KRUNKER_V_NUM(-90),
                              KRUNKER_V_NUM(12), KRUNKER_V_NUM(0.42), KRUNKER_V_NUM(1), KRUNKER_V_NUM(0), KRUNKER_V_NUM(1),
                              KRUNKER_V_NUM(0), KRUNKER_V_ARRAY(g, 3), KRUNKER_V_NUM(48)};
    const KrunkerValue k[] = {KRUNKER_V_ARRAY(p, 13), KRUNKER_V_NUM(10), KRUNKER_V_NUM(123456)};
    KrunkerBuf buf = {0};
    KrunkerFrame f;
    CHECK(krunker_proto_encode_ah(0, krunker_s2c_wire(KRUNKER_S2C_UPDATE_PLAYERS), k, 3, &buf) == KRUNKER_PROTO_OK);
    CHECK(krunker_proto_decode(buf.data, buf.len, 0, &f) == KRUNKER_PROTO_OK);
    CHECK(f.argc == 3 && krunker_num(krunker_frame_arg(&f, 1), 0) == 10 && krunker_num(krunker_frame_arg(&f, 2), 0) == 123456);
    const KrunkerValue *list = krunker_frame_arg(&f, 0);
    CHECK(krunker_num(krunker_at(list, (uint32_t) b->k[KRB_K_X_DIRE]), 0) == -90); // degrees, an int
    CHECK(krunker_at(list, 4) && krunker_at(list, 4)->type == KRUNKER_NUM);
    CHECK(krunker_is(krunker_at(list, (uint32_t) b->k[KRB_K_GRAPPLE]), KRUNKER_ARRAY));
    krunker_frame_free(&f);
    krunker_buf_free(&buf);
}

// The per-event packets the shape audit corrected, as the 10.0.3 server packs them:
//   "3" KILL_PLAYER [victim, deaths, killer ? killer.sid : 0, stats, info, hideFeed] (deob-10.0.3.js:198678): no
//       killer is the number 0, not nil (KN reads it as none)
//   "bo" BUILD_OBJECT [buildInd, x, y, z, w, h, l, dir, uid] (:216805): dir in radians (rotDir :195816-195825), a
//       float64 on the wire for PI / 2, PI, -PI / 2
//   "bh" UPDATE_HEAD_SIZE [sid, mlt, reset] (:259406 -> updateHeadSize :194975)
//   "a" ACCOUNT_RESPONSE [error, id, username, row, ...] (:244344): row indexed by Account.setData (:128966)
static void test_shapes_1003_events(void) {
    const KrunkerBuild *b = krunker_build_compiled();
    CHECK(b->kill[KRB_KILL_VICTIM] == 0 && b->kill[KRB_KILL_DEATHS] == 1 && b->kill[KRB_KILL_KILLER] == 2 &&
          b->kill[KRB_KILL_STATS] == 3 && b->kill[KRB_KILL_INFO] == 4 && b->kill[KRB_KILL_HIDE_FEED] == 5);
    KrunkerBuf buf = {0};
    KrunkerFrame f;
    const KrunkerValue kill[] = {KRUNKER_V_NUM(5), KRUNKER_V_NUM(2), KRUNKER_V_NUM(0), KRUNKER_V_ARRAY(NULL, 0), KRUNKER_V_NIL,
                                 KRUNKER_V_BOOL(0)};
    CHECK(krunker_proto_encode_ah(0, krunker_s2c_wire(KRUNKER_S2C_KILL_PLAYER), kill, 6, &buf) == KRUNKER_PROTO_OK);
    BYTES_ARE(&buf, 0x97, 0xa1, '3', 0x05, 0x02, 0x00, 0x90, 0xc0, 0xc2, 0x00, 0x00);
    CHECK(krunker_proto_decode(buf.data, buf.len, 0, &f) == KRUNKER_PROTO_OK);
    const KrunkerValue *killer = krunker_frame_arg(&f, (uint32_t) b->kill[KRB_KILL_KILLER]);
    CHECK(krunker_is(killer, KRUNKER_NUM) && killer->u.num == 0);
    krunker_frame_free(&f);

    const KrunkerValue bo[] = {KRUNKER_V_NUM(2), KRUNKER_V_NUM(10), KRUNKER_V_NUM(0), KRUNKER_V_NUM(-4), KRUNKER_V_NUM(10),
                               KRUNKER_V_NUM(10), KRUNKER_V_NUM(10), KRUNKER_V_NUM(M_PI), KRUNKER_V_NUM(2001)};
    CHECK(krunker_proto_encode_ah(0, krunker_s2c_wire(KRUNKER_S2C_BUILD_OBJECT), bo, 9, &buf) == KRUNKER_PROTO_OK);
    CHECK(krunker_proto_decode(buf.data, buf.len, 0, &f) == KRUNKER_PROTO_OK);
    CHECK(f.argc == 9 && krunker_num(krunker_frame_arg(&f, 7), 0) == M_PI && krunker_num(krunker_frame_arg(&f, 8), 0) == 2001);
    krunker_frame_free(&f);

    const KrunkerValue bh[] = {KRUNKER_V_NUM(4), KRUNKER_V_NUM(1.6), KRUNKER_V_BOOL(1)};
    CHECK(krunker_proto_encode_ah(0, krunker_s2c_wire(KRUNKER_S2C_UPDATE_HEAD_SIZE), bh, 3, &buf) == KRUNKER_PROTO_OK);
    CHECK(krunker_proto_decode(buf.data, buf.len, 0, &f) == KRUNKER_PROTO_OK);
    CHECK(f.argc == 3 && krunker_is(krunker_frame_arg(&f, 2), KRUNKER_BOOL) && krunker_frame_arg(&f, 2)->u.boolean);
    krunker_frame_free(&f);

    KrunkerValue row[48];
    for (int i = 0; i < 48; i++) row[i] = KRUNKER_V_NUM(0);
    row[12] = KRUNKER_V_NUM(1);  // isDev
    row[18] = KRUNKER_V_NUM(3);  // role
    row[47] = KRUNKER_V_NUM(55); // rankedPoints
    const KrunkerValue acc[] = {KRUNKER_V_NIL, KRUNKER_V_NUM(123), KRUNKER_V_STR("RowPlayer"), KRUNKER_V_ARRAY(row, 48)};
    CHECK(krunker_proto_encode_ah(0, krunker_s2c_wire(KRUNKER_S2C_ACCOUNT_RESPONSE), acc, 4, &buf) == KRUNKER_PROTO_OK);
    CHECK(krunker_proto_decode(buf.data, buf.len, 0, &f) == KRUNKER_PROTO_OK);
    const KrunkerValue *r = krunker_frame_arg(&f, 3);
    CHECK(krunker_num(krunker_at(r, 18), 0) == 3 && krunker_num(krunker_at(r, 12), 0) == 1 && krunker_num(krunker_at(r, 47), 0) == 55);
    CHECK(krunker_get(r, "player_role") == NULL); // the row is an array: no column names
    krunker_frame_free(&f);
    krunker_buf_free(&buf);
}

// The net.md §9 gaps' wire shapes, as 10.0.3 builds them (the link's translation: client/tests/krunker_link_test.c
// test_net_gaps_1003).
static void test_net_gaps_1003(void) {
    const KrunkerBuild *b = krunker_build_compiled();
    KrunkerBuf buf = {0};
    KrunkerFrame f;

    // "l" of an SRM player (syncState :222389-222405): custSync [name, value, ...] at [25], the 6 sendVals (:18684,
    // tmpVel a {x, y, z} object), the controlled entity last: 33 values, the tail at len - 1
    KrunkerValue s[33];
    for (int i = 0; i < 33; i++) s[i] = KRUNKER_V_NUM(0);
    const KrunkerValue cust[] = {KRUNKER_V_STR("myVal"), KRUNKER_V_NUM(5)};
    s[25] = KRUNKER_V_ARRAY((KrunkerValue *) cust, 2);
    const KrunkerValue tmp_vel[] = {KRUNKER_V_STR("x"), KRUNKER_V_NUM(0.05), KRUNKER_V_STR("y"), KRUNKER_V_NUM(0), KRUNKER_V_STR("z"), KRUNKER_V_NUM(-0.5)};
    s[31] = KRUNKER_V_MAP((KrunkerValue *) tmp_vel, 3);
    const KrunkerValue state[] = {KRUNKER_V_STR("x"), KRUNKER_V_NUM(12)};
    const KrunkerValue controlled[] = {KRUNKER_V_NUM(9), KRUNKER_V_NUM(120), KRUNKER_V_MAP((KrunkerValue *) state, 1)};
    s[32] = KRUNKER_V_ARRAY((KrunkerValue *) controlled, 3);
    const KrunkerValue l[] = {KRUNKER_V_ARRAY(s, 33)};
    CHECK(krunker_proto_encode_ah(0, krunker_s2c_wire(KRUNKER_S2C_SYNC_PLAYER), l, 1, &buf) == KRUNKER_PROTO_OK);
    CHECK(krunker_proto_decode(buf.data, buf.len, 0, &f) == KRUNKER_PROTO_OK);
    const KrunkerValue *a = krunker_frame_arg(&f, 0);
    const int cust_at = b->sync[KRB_SYNC_PROTECTED];
    CHECK(krunker_is(a, KRUNKER_ARRAY) && a->len == 33 && cust_at == 25 && krunker_is(krunker_at(a, 25), KRUNKER_ARRAY));
    CHECK(a->len - (uint32_t) cust_at - 2 == 6 && krunker_is(krunker_at(a, 31), KRUNKER_MAP) &&
          krunker_num(krunker_get(krunker_at(a, 31), "z"), 0) == -0.5); // SRM: 6 sendVals, tmpVel the object
    CHECK(krunker_is(krunker_at(a, a->len - 1), KRUNKER_ARRAY) && krunker_num(krunker_at(krunker_at(a, a->len - 1), 1), 0) == 120);
    krunker_frame_free(&f);

    // an empty "k" (updateState sends one, :222350): the list is an empty array, still hideAll's trigger (:258582)
    const KrunkerValue k[] = {KRUNKER_V_ARRAY(NULL, 0), KRUNKER_V_NUM(10), KRUNKER_V_NUM(77)};
    CHECK(krunker_proto_encode_ah(0, krunker_s2c_wire(KRUNKER_S2C_UPDATE_PLAYERS), k, 3, &buf) == KRUNKER_PROTO_OK);
    BYTES_ARE(&buf, 0x94, 0xa1, 'k', 0x90, 0x0a, 0x4d, 0x00, 0x00);
    CHECK(krunker_proto_decode(buf.data, buf.len, 0, &f) == KRUNKER_PROTO_OK);
    CHECK(krunker_is(krunker_frame_arg(&f, 0), KRUNKER_ARRAY) && krunker_frame_arg(&f, 0)->len == 0);
    krunker_frame_free(&f);

    // "cv" [key, value]: Load Data keeps a non-number as the string (:183563)
    const KrunkerValue cv[] = {KRUNKER_V_STR("rank"), KRUNKER_V_STR("Gold")};
    CHECK(krunker_proto_encode_ah(0, krunker_s2c_wire(KRUNKER_S2C_CUSTOM_VAL), cv, 2, &buf) == KRUNKER_PROTO_OK);
    BYTES_ARE(&buf, 0x93, 0xa2, 'c', 'v', 0xa4, 'r', 'a', 'n', 'k', 0xa4, 'G', 'o', 'l', 'd', 0x00, 0x00);
    krunker_buf_free(&buf);

    // "krt" [dogTags, depositType || 0] (:176746)
    const KrunkerValue krt[] = {KRUNKER_V_NUM(4), KRUNKER_V_NUM(2)};
    CHECK(krunker_proto_encode_ah(0, krunker_s2c_wire(KRUNKER_S2C_UPDATE_KR_TAGS), krt, 2, &buf) == KRUNKER_PROTO_OK);
    BYTES_ARE(&buf, 0x93, 0xa3, 'k', 'r', 't', 0x04, 0x02, 0x00, 0x00);

    // "0"'s [17] customLoadout: an array (the weapons of updateLoadout, nulls kept) or null (:198810)
    CHECK(b->add_stride == 53);
    for (int i = 0; i < KRB_ADD_COUNT; i++) CHECK(b->add[i] != 17 && b->add[i] != 21); // unnamed offsets (customLoadout, propID)

    // init's 22 arguments (:221968): [20] krunkScripts an object, [21] gameState a number
    KrunkerValue init[22];
    for (int i = 0; i < 22; i++) init[i] = KRUNKER_V_NIL;
    init[2] = KRUNKER_V_NUM(3);
    init[17] = KRUNKER_V_STR("s");
    init[19] = KRUNKER_V_NUM(240000);
    init[21] = KRUNKER_V_NUM(2);
    CHECK(krunker_proto_encode_ah(0, krunker_s2c_wire(KRUNKER_S2C_INIT_GAME), init, 22, &buf) == KRUNKER_PROTO_OK);
    CHECK(krunker_proto_decode(buf.data, buf.len, 0, &f) == KRUNKER_PROTO_OK);
    CHECK(f.argc == 22 && krunker_num(krunker_frame_arg(&f, 2), 0) == 3 && krunker_num(krunker_frame_arg(&f, 19), 0) == 240000 &&
          krunker_num(krunker_frame_arg(&f, 21), 0) == 2);
    krunker_frame_free(&f);

    // C2S "custom" [config, captchaRid, rehost]: createPrivateRoom's object always carries customMap / rawMapData /
    // passCode, null when unused (:229827-229839), and a bool row without its field as 0 (:229834)
    const KrunkerValue cfg[] = {KRUNKER_V_STR("customMap"), KRUNKER_V_NIL, KRUNKER_V_STR("rawMapData"), KRUNKER_V_NIL,
                                KRUNKER_V_STR("passCode"), KRUNKER_V_NIL, KRUNKER_V_STR("hpRegen"), KRUNKER_V_NUM(0)};
    const KrunkerValue custom[] = {KRUNKER_V_MAP((KrunkerValue *) cfg, 4), KRUNKER_V_NIL, KRUNKER_V_BOOL(0)};
    CHECK(krunker_proto_encode_ah(0, krunker_c2s_wire(KRUNKER_C2S_CUSTOM_SERVER), custom, 3, &buf) == KRUNKER_PROTO_OK);
    CHECK(krunker_proto_decode(buf.data, buf.len, 0, &f) == KRUNKER_PROTO_OK);
    const KrunkerValue *c = krunker_frame_arg(&f, 0);
    CHECK(krunker_is(c, KRUNKER_MAP) && c->len == 4 && krunker_get(c, "customMap") && krunker_is(krunker_get(c, "customMap"), KRUNKER_NIL) &&
          krunker_is(krunker_get(c, "passCode"), KRUNKER_NIL) && krunker_is(krunker_get(c, "hpRegen"), KRUNKER_NUM));
    krunker_frame_free(&f);
    krunker_buf_free(&buf);
}

int main(void) {
    test_tables();
    test_ahnum();
    test_golden();
    test_decode_server();
    test_round_trips();
    test_malformed();
    test_fuzz();
    test_live_compiled_equal();
    test_live_values();
    test_live_refusals();
    test_live_manifest();
    test_shapes_1003_add();
    test_shapes_1003_sync();
    test_shapes_1003_k();
    test_shapes_1003_events();
    test_net_gaps_1003();
    if (g_fail) {
        printf("krunker_proto_test FAILED\n");
        return 1;
    }
    printf("Test OK\n");
    return 0;
}
