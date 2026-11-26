// krunker_social_test: the hub page's krunker.io social socket, its framing (client/src/net/krunker_social_wire.c,
// client/include/krunker_social.h) against the game socket's (deob-10.0.7.js:98766-98980) and the hub's use of it
// (hub-10.0.7.js:7020-7110). No sockets: frames are encoded and decoded through krunker_proto.c.
#include <krunker_social.h>
#include <krunker_build.h>
#include <auth_join.h> // kn_sha256_bytes / kn_sha256_hex: a challenge with a known answer
#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail, g_pass;
#define CHECK(cond, ...)                                                                                                      \
    do {                                                                                                                      \
        if (cond) {                                                                                                           \
            g_pass++;                                                                                                         \
        } else {                                                                                                              \
            g_fail++;                                                                                                         \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                       \
            printf(__VA_ARGS__);                                                                                              \
            printf("\n");                                                                                                     \
        }                                                                                                                     \
    } while (0)

static size_t b64_decode(const char *in, char *out, const size_t n) {
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    unsigned v = 0;
    int bits = 0;
    for (; in && *in && *in != '='; in++) {
        const char *p = strchr(t, *in);
        if (!p) break;
        v = v << 6 | (unsigned) (p - t);
        bits += 6;
        if (bits >= 8 && o + 1 < n) {
            bits -= 8;
            out[o++] = (char) (v >> bits & 0xff);
        }
    }
    out[o] = 0;
    return o;
}

// the frame's args as JSON text ("[...]"), the name apart
static void args_json(const KrunkerFrame *f, char *out, const size_t n) {
    size_t o = 0;
    out[o++] = '[';
    for (uint32_t i = 0; i < f->argc && o + 2 < n; i++) {
        KrunkerBuf b = {0};
        krunker_value_to_json(&f->args[i], &b);
        if (i) out[o++] = ',';
        const size_t put = b.len < n - o - 2 ? b.len : n - o - 2;
        memcpy(out + o, b.data, put);
        o += put;
        krunker_buf_free(&b);
    }
    out[o++] = ']';
    out[o] = 0;
}

// what the server would send: msgpack([name, ...args]) + a trailer
static KrunkerBuf server_frame(const char *name, const KrunkerValue *args, const size_t argc) {
    KrunkerBuf b = {0};
    krunker_proto_encode_ah(0, name, args, argc, &b);
    return b;
}

static uint8_t ah_after(const int frames) {
    unsigned ah = 0;
    for (int i = 0; i < frames; i++) ah = (ah + KRUNKER_AHKEY) & 0xff;
    return (uint8_t) ah;
}

static void test_encode(void) {
    KrunkerSocialWire w;
    krunker_social_wire_init(&w);
    CHECK(w.codec.ahkey == 0x43e1 && w.codec.ah_num == 0 && w.socket_id == -1, "a new connection: AHKEY 0x43e1, ahNum 0");

    // the hub's view request: send("r", page, sub, query, name, null, region, ...) (iìíìììì, hub-10.0.7.js:7114)
    cJSON *r = cJSON_Parse("[\"r\",\"profile\",null,\"Bob\",\"Me\",null,0,null]");
    KrunkerBuf b = {0};
    CHECK(krunker_social_wire_encode(&w, r, &b) == KRUNKER_PROTO_OK, "encode r");
    KrunkerFrame f;
    char json[1024];
    CHECK(krunker_proto_decode(b.data, b.len, KRUNKER_DECODE_STRICT_TRAILER, &f) == KRUNKER_PROTO_OK, "the frame decodes");
    args_json(&f, json, sizeof(json));
    CHECK(!strcmp(f.name, "r") && !strcmp(json, "[\"profile\",null,\"Bob\",\"Me\",null,0,null]"), "r args %s", json);
    // ahNum = (ahNum + 0x43e1) & 0xff before the frame: 0xe1, the trailer [0xe, 0x1]
    CHECK(f.ah_echo == 0xe1 && b.data[b.len - 2] == 0xe && b.data[b.len - 1] == 0x1, "first trailer 0x%x", f.ah_echo);
    krunker_frame_free(&f);
    krunker_buf_free(&b);
    cJSON_Delete(r);

    // numbers as @msgpack/msgpack encodes JS numbers, objects as maps, nesting
    cJSON *ps = cJSON_Parse("[\"ps\",{\"txt\":\"hello\",\"kr\":2.5,\"tags\":[1,true,false]},-7,4294967296]");
    CHECK(krunker_social_wire_encode(&w, ps, &b) == KRUNKER_PROTO_OK && krunker_proto_decode(b.data, b.len, 0, &f) == KRUNKER_PROTO_OK, "encode ps");
    args_json(&f, json, sizeof(json));
    CHECK(!strcmp(json, "[{\"txt\":\"hello\",\"kr\":2.5,\"tags\":[1,true,false]},-7,4294967296]"), "ps args %s", json);
    CHECK(f.ah_echo == ah_after(2), "second trailer 0x%x (want 0x%x)", f.ah_echo, ah_after(2));
    krunker_frame_free(&f);
    krunker_buf_free(&b);
    cJSON_Delete(ps);

    // PAT_REFRESH ["patR", token] (hub-10.0.7.js:7067-7072): the build's wire name, an ordinary ahNum step
    cJSON *pat = cJSON_CreateArray();
    cJSON_AddItemToArray(pat, cJSON_CreateString(krunker_c2s_wire(KRUNKER_C2S_PAT_REFRESH)));
    cJSON_AddItemToArray(pat, cJSON_CreateString("pat-2"));
    CHECK(krunker_social_wire_encode(&w, pat, &b) == KRUNKER_PROTO_OK && krunker_proto_decode(b.data, b.len, 0, &f) == KRUNKER_PROTO_OK, "encode patR");
    CHECK(!strcmp(f.name, "patR") && !strcmp(krunker_str(krunker_frame_arg(&f, 0)), "pat-2") && f.ah_echo == ah_after(3),
          "patR frame %s 0x%x", f.name, f.ah_echo);
    krunker_frame_free(&f);
    krunker_buf_free(&b);
    cJSON_Delete(pat);

    // rpc(LOGIN, token): "_0" [rpcCounter++, "login", token]
    cJSON *args = cJSON_Parse("[\"tok.en\"]");
    const int id0 = krunker_social_wire_rpc(&w, krunker_c2s_wire(KRUNKER_C2S_LOGIN), args, &b);
    CHECK(id0 == 0 && krunker_proto_decode(b.data, b.len, 0, &f) == KRUNKER_PROTO_OK, "rpc 0");
    args_json(&f, json, sizeof(json));
    CHECK(!strcmp(f.name, "_0") && !strcmp(json, "[0,\"login\",\"tok.en\"]") && f.ah_echo == ah_after(4), "rpc frame %s %s", f.name, json);
    krunker_frame_free(&f);
    krunker_buf_free(&b);
    CHECK(krunker_social_wire_rpc(&w, "login", NULL, &b) == 1 && w.rpc_counter == 2, "the next rpc takes id 1");
    krunker_buf_free(&b);
    cJSON_Delete(args);

    // not a frame: refused, nothing rotates
    const uint8_t before = w.codec.ah_num;
    cJSON *bad = cJSON_Parse("[42]");
    CHECK(krunker_social_wire_encode(&w, bad, &b) != KRUNKER_PROTO_OK && w.codec.ah_num == before, "a frame without a type is refused");
    cJSON_Delete(bad);
    krunker_buf_free(&b);

    // a new connection starts over at ahNum 0 (socket.reset)
    krunker_social_wire_init(&w);
    cJSON *po = cJSON_Parse("[\"po\"]");
    CHECK(krunker_social_wire_encode(&w, po, &b) == KRUNKER_PROTO_OK && krunker_proto_decode(b.data, b.len, 0, &f) == KRUNKER_PROTO_OK &&
              f.ah_echo == 0xe1 && f.argc == 0, "po after a reconnect: trailer 0x%x", f.ah_echo);
    krunker_frame_free(&f);
    krunker_buf_free(&b);
    cJSON_Delete(po);
}

static void test_decode(void) {
    KrunkerSocialWire w;
    krunker_social_wire_init(&w);
    cJSON *frame = NULL;

    // io-init [socketId]: the socket's own
    KrunkerBuf b = server_frame("io-init", (KrunkerValue[]) {KRUNKER_V_NUM(77)}, 1);
    CHECK(krunker_social_wire_decode(&w, b.data, b.len, &frame) == KSW_NONE && !frame && w.socket_id == 77, "io-init");
    krunker_buf_free(&b);

    // "a" [null, id, name, data, token]: the page's handler table (iïîîiìî)
    const KrunkerValue stats[] = {KRUNKER_V_STR("c"), KRUNKER_V_NUM(3)};
    const KrunkerValue data[] = {KRUNKER_V_STR("player_funds"), KRUNKER_V_NUM(1234), KRUNKER_V_STR("stats"), KRUNKER_V_MAP(stats, 1)};
    const KrunkerValue a[] = {KRUNKER_V_NIL, KRUNKER_V_NUM(4242), KRUNKER_V_STR("KioHub"), KRUNKER_V_MAP(data, 2), KRUNKER_V_NIL};
    b = server_frame("a", a, 5);
    CHECK(krunker_social_wire_decode(&w, b.data, b.len, &frame) == KSW_PAGE && frame, "a goes to the page");
    char *text = frame ? cJSON_PrintUnformatted(frame) : NULL;
    CHECK(text && !strcmp(text, "[\"a\",null,4242,\"KioHub\",{\"player_funds\":1234,\"stats\":{\"c\":3}},null]"), "a frame %s", text ? text : "");
    free(text);
    cJSON_Delete(frame);
    krunker_buf_free(&b);

    // "0" [page, query, data]: iiiíííî
    const KrunkerValue rows[] = {KRUNKER_V_STR("x"), KRUNKER_V_BOOL(1)};
    b = server_frame("0", (KrunkerValue[]) {KRUNKER_V_STR("profile"), KRUNKER_V_STR("Bob"), KRUNKER_V_ARRAY(rows, 2)}, 3);
    CHECK(krunker_social_wire_decode(&w, b.data, b.len, &frame) == KSW_PAGE, "0 goes to the page");
    text = frame ? cJSON_PrintUnformatted(frame) : NULL;
    CHECK(text && !strcmp(text, "[\"0\",\"profile\",\"Bob\",[\"x\",true]]"), "0 frame %s", text ? text : "");
    free(text);
    cJSON_Delete(frame);
    krunker_buf_free(&b);

    // rpc answers settle the rpc: nothing for the page
    b = server_frame("_0", (KrunkerValue[]) {KRUNKER_V_NUM(0), KRUNKER_V_BOOL(1)}, 2);
    CHECK(krunker_social_wire_decode(&w, b.data, b.len, &frame) == KSW_NONE && !frame, "_0 answer");
    krunker_buf_free(&b);
    b = server_frame("_1", (KrunkerValue[]) {KRUNKER_V_NUM(1), KRUNKER_V_STR("nope")}, 2);
    CHECK(krunker_social_wire_decode(&w, b.data, b.len, &frame) == KSW_NONE && !frame, "_1 answer");
    krunker_buf_free(&b);

    // garbage: dropped (the page's decoder throws)
    const uint8_t junk[] = {0xc1, 0, 0};
    CHECK(krunker_social_wire_decode(&w, junk, sizeof(junk), &frame) == KSW_BAD && !frame, "junk is KSW_BAD");
}

static void test_captcha(void) {
    KrunkerSocialWire w;
    krunker_social_wire_init(&w);
    // two frames first: cptR must not step ahNum
    cJSON *po = cJSON_Parse("[\"po\"]");
    KrunkerBuf b = {0};
    krunker_social_wire_encode(&w, po, &b);
    krunker_buf_free(&b);
    krunker_social_wire_encode(&w, po, &b);
    krunker_buf_free(&b);
    cJSON_Delete(po);
    const uint8_t ah = w.codec.ah_num;

    const char *salt = "hub5alt?expires=1900000000";
    char pre[96], hex[65];
    uint8_t digest[32];
    snprintf(pre, sizeof(pre), "%s%d", salt, 4321);
    kn_sha256_bytes(pre, strlen(pre), digest);
    kn_sha256_hex(digest, hex);
    const KrunkerValue chal[] = {KRUNKER_V_STR("algorithm"), KRUNKER_V_STR("SHA-256"), KRUNKER_V_STR("challenge"), KRUNKER_V_STR(hex),
                                 KRUNKER_V_STR("maxnumber"), KRUNKER_V_NUM(200000), KRUNKER_V_STR("salt"), KRUNKER_V_STR(salt),
                                 KRUNKER_V_STR("signature"), KRUNKER_V_STR("5ig")};
    b = server_frame("cpt", (KrunkerValue[]) {KRUNKER_V_MAP(chal, 5)}, 1);
    cJSON *frame = NULL;
    CHECK(krunker_social_wire_decode(&w, b.data, b.len, &frame) == KSW_CAPTCHA && w.solving && w.captcha_shown, "cpt starts the search");
    krunker_buf_free(&b);
    int rc = 0, steps = 0;
    KrunkerBuf out = {0};
    while ((rc = krunker_social_wire_captcha_step(&w, 1000, &out)) == 0 && steps < 1000) steps++;
    CHECK(rc == 1 && steps >= 4, "solved in slices (%d steps, rc %d)", steps, rc);
    KrunkerFrame f;
    char json[512] = "", want[256];
    CHECK(out.data && krunker_proto_decode(out.data, out.len, 0, &f) == KRUNKER_PROTO_OK, "cptR decodes");
    if (out.data) {
        b64_decode(krunker_str(krunker_frame_arg(&f, 0)), json, sizeof(json));
        snprintf(want, sizeof(want), "{\"algorithm\":\"SHA-256\",\"challenge\":\"%s\",\"number\":4321,\"salt\":\"%s\",\"signature\":\"5ig\",\"took\":", hex, salt);
        CHECK(!strcmp(f.name, "cptR") && !strncmp(json, want, strlen(want)), "cptR payload %s", json);
        CHECK(f.ah_echo == ah && w.codec.ah_num == ah, "cptR keeps ahNum (0x%x, want 0x%x)", f.ah_echo, ah);
        krunker_frame_free(&f);
    }
    krunker_buf_free(&out);
    CHECK(!w.solving && w.captcha_shown, "the modal stays until the server's cptR");
    b = server_frame("cptR", NULL, 0);
    CHECK(krunker_social_wire_decode(&w, b.data, b.len, &frame) == KSW_NONE && !w.captcha_shown, "the server's cptR hides it");
    krunker_buf_free(&b);

    // the JSON-string form of the challenge works too
    char text[512];
    snprintf(text, sizeof(text), "{\"algorithm\":\"SHA-256\",\"challenge\":\"%s\",\"maxnumber\":5000,\"salt\":\"%s\",\"signature\":\"5ig\"}", hex, salt);
    b = server_frame("cpt", (KrunkerValue[]) {KRUNKER_V_STR(text)}, 1);
    CHECK(krunker_social_wire_decode(&w, b.data, b.len, &frame) == KSW_CAPTCHA, "cpt as a JSON string");
    krunker_buf_free(&b);
    while ((rc = krunker_social_wire_captcha_step(&w, 100000, &out)) == 0) {}
    CHECK(rc == 1, "the string challenge is solved");
    krunker_buf_free(&out);
    // the server's "error" while the modal is up: _hideCaptcha, and the page still gets the event (deob-10.0.7.js:98822)
    b = server_frame("error", (KrunkerValue[]) {KRUNKER_V_STR("Bad challenge")}, 1);
    CHECK(krunker_social_wire_decode(&w, b.data, b.len, &frame) == KSW_PAGE && !w.captcha_shown && frame
              && !strcmp(cJSON_GetStringValue(cJSON_GetArrayItem(frame, 0)), "error"), "\"error\" hides the modal and reaches the page");
    cJSON_Delete(frame);
    frame = NULL;
    krunker_buf_free(&b);

    // one this client cannot answer (an algorithm the widget lacks, or no number up to maxnumber)
    const KrunkerValue md5[] = {KRUNKER_V_STR("algorithm"), KRUNKER_V_STR("MD5"), KRUNKER_V_STR("challenge"), KRUNKER_V_STR(hex),
                                KRUNKER_V_STR("maxnumber"), KRUNKER_V_NUM(1000), KRUNKER_V_STR("salt"), KRUNKER_V_STR(salt),
                                KRUNKER_V_STR("signature"), KRUNKER_V_STR("5ig")};
    b = server_frame("cpt", (KrunkerValue[]) {KRUNKER_V_MAP(md5, 5)}, 1);
    CHECK(krunker_social_wire_decode(&w, b.data, b.len, &frame) == KSW_CAPTCHA_FAIL && !w.captcha_shown, "an MD5 cpt fails");
    krunker_buf_free(&b);
    const KrunkerValue small[] = {KRUNKER_V_STR("algorithm"), KRUNKER_V_STR("SHA-256"), KRUNKER_V_STR("challenge"), KRUNKER_V_STR(hex),
                                  KRUNKER_V_STR("maxnumber"), KRUNKER_V_NUM(1000), KRUNKER_V_STR("salt"), KRUNKER_V_STR(salt),
                                  KRUNKER_V_STR("signature"), KRUNKER_V_STR("5ig")};
    b = server_frame("cpt", (KrunkerValue[]) {KRUNKER_V_MAP(small, 5)}, 1);
    CHECK(krunker_social_wire_decode(&w, b.data, b.len, &frame) == KSW_CAPTCHA, "a cpt whose answer is past maxnumber starts");
    krunker_buf_free(&b);
    while ((rc = krunker_social_wire_captcha_step(&w, 300, &out)) == 0) {}
    CHECK(rc == -1 && !w.solving && !w.captcha_shown, "... and fails at maxnumber");
    krunker_buf_free(&out);
}

int main(void) {
    test_encode();
    test_decode();
    test_captcha();
    printf("krunker_social_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
