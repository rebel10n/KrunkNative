// client/src/net/krunker_link.c (the official-Krunker packet translation) without sockets: Krunker
// frames in (built with the codec, as a server would send them) -> NetPackets out; our packets in
// -> Krunker frames out (decoded again and checked field by field, ahNum trailers included).
//   cmake-build-debug/krunker_link_test
#include <krunker_link.h>
#include <auth_join.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail, g_pass;
#define CHECK(cond, ...) do { if (cond) g_pass++; else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// ---- test doubles: the cheat evaluator (the link takes io.cheat; this satisfies its fallback symbol)
int krunker_cheat_answer(const char *nonce, const char *ciphertext_b64, KrunkerCcr *out) {
    (void) nonce;
    (void) ciphertext_b64;
    memset(out, 0, sizeof(*out));
    return -1;
}
void krunker_ccr_free(KrunkerCcr *out) {
    free(out->nonce);
    free(out->payload_b64);
    out->nonce = out->payload_b64 = NULL;
}

static int fake_cheat(void *user, const char *nonce, const char *ct, KrunkerCcr *out) {
    (void) user;
    memset(out, 0, sizeof(*out));
    if (!strcmp(ct, "garbage")) return -1;                  // undecodable: no ccr at all
    out->nonce = strdup(nonce);
    out->payload_b64 = strdup(!strcmp(ct, "undef") ? "" : "AAEC"); // "" = an undefined result, still sent
    memcpy(out->fnv, "0123abcd", 9);
    return 0;
}

// ---- captured I/O
static NetPacket g_in[256];
static int g_in_count;
static KrunkerFrame g_out[256];
static int g_out_count;
static int g_events[16];

static void cap_emit(void *user, const NetPacket *p) {
    (void) user;
    if (g_in_count < 256) g_in[g_in_count++] = *p;
}
static void cap_send(void *user, const uint8_t *frame, const size_t len) {
    (void) user;
    if (g_out_count < 256 && krunker_proto_decode(frame, len, 0, &g_out[g_out_count]) == KRUNKER_PROTO_OK) g_out_count++;
}
static void cap_event(void *user, const KrunkerLinkEvent kind, const char *text) {
    (void) user;
    (void) text;
    if ((int) kind < 16) g_events[kind]++;
}

static void clear(void) {
    for (int i = 0; i < g_out_count; i++) krunker_frame_free(&g_out[i]);
    g_in_count = g_out_count = 0;
    memset(g_events, 0, sizeof(g_events));
}

static void feed(KrunkerLink *l, const char *wire, const KrunkerValue *args, const size_t argc) {
    KrunkerBuf b = {0};
    if (krunker_proto_encode_ah(0x2a, wire, args, argc, &b) != KRUNKER_PROTO_OK) {
        printf("encode %s failed\n", wire);
        g_fail++;
        return;
    }
    krunker_link_on_frame(l, b.data, b.len);
    krunker_buf_free(&b);
}

static const NetPacket *find_in(const int type, const int nth) {
    int n = 0;
    for (int i = 0; i < g_in_count; i++) {
        if (g_in[i].type == type && n++ == nth) return &g_in[i];
    }
    return NULL;
}

static double fnum(const KrunkerFrame *f, const uint32_t i) { return krunker_num(krunker_frame_arg(f, i), -12345); }

static size_t b64_decode(const char *in, char *out, const size_t n) {
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    unsigned v = 0;
    int bits = 0;
    for (; *in && *in != '='; in++) {
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

int main(void) {
    KrunkerLink link;
    const KrunkerLinkIO io = {.emit = cap_emit, .send = cap_send, .event = cap_event, .cheat = fake_cheat};
    krunker_link_init(&link, &io, KRUNKER_AHKEY);
    unsigned ah = 0;
#define NEXT_AH() (ah = (ah + KRUNKER_AHKEY) & 0xff)

    // io-init, load -> "load"
    feed(&link, "io-init", (KrunkerValue[]) {KRUNKER_V_STR("sockA")}, 1);
    CHECK(!strcmp(link.socket_id, "sockA"), "socket id %s", link.socket_id);
    feed(&link, "load", NULL, 0);
    CHECK(g_out_count == 1 && !strcmp(g_out[0].name, "load") && g_out[0].argc == 0, "load answered");
    CHECK(g_out_count == 1 && g_out[0].ah_echo == (int) NEXT_AH(), "load ahNum %d", g_out_count ? g_out[0].ah_echo : -1);
    clear();

    // init -> INIT
    feed(&link, "init", (KrunkerValue[]) {KRUNKER_V_NUM(10), KRUNKER_V_NUM(0), KRUNKER_V_NUM(0)}, 3);
    const NetPacket *p = find_in(NET_PACKET_INIT, 0);
    NetInitPacket init;
    if (p) memcpy(&init, p->payload, sizeof(init));
    CHECK(p && p->length == sizeof(NetInitPacket) && init.map_index == 10 && init.mode_index == 0, "INIT map 10 mode 0");
    CHECK(link.unsupported_map == -1, "a catalogue map is not flagged (%d)", link.unsupported_map);
    clear();
    feed(&link, "init", (KrunkerValue[]) {KRUNKER_V_NUM(77), KRUNKER_V_NUM(0)}, 2);
    CHECK(!find_in(NET_PACKET_INIT, 0) && g_events[KRUNKER_LINK_EV_UNSUPPORTED] == 1, "unknown map index refused");
    CHECK(link.unsupported_map == 77, "the unknown map index is kept for the error screen (%d)", link.unsupported_map);
    clear();

    // "0": two players, the second is us
    KrunkerValue add[2 * 53];
    for (int i = 0; i < 2 * 53; i++) add[i] = KRUNKER_V_NIL;
    add[0] = KRUNKER_V_STR("sockB"); add[1] = KRUNKER_V_NUM(7); add[2] = KRUNKER_V_NUM(1.5); add[3] = KRUNKER_V_NUM(2);
    add[4] = KRUNKER_V_NUM(-3); add[5] = KRUNKER_V_STR("Other"); add[6] = KRUNKER_V_NUM(1); add[7] = KRUNKER_V_NUM(60);
    add[8] = KRUNKER_V_NUM(100); add[9] = KRUNKER_V_NUM(2); add[18] = KRUNKER_V_NUM(0.5);
    add[53] = KRUNKER_V_STR("sockA"); add[54] = KRUNKER_V_NUM(9); add[58] = KRUNKER_V_STR("Me"); add[61] = KRUNKER_V_NUM(100);
    feed(&link, "0", (KrunkerValue[]) {KRUNKER_V_ARRAY(add, 2 * 53)}, 1);
    NetSpawnPacket s0, s1;
    p = find_in(NET_PACKET_SPAWN, 0);
    if (p) memcpy(&s0, p->payload, sizeof(s0));
    CHECK(p && s0.uid == 7 && !s0.is_you && s0.health == 60.0f && s0.max_health == 100 && s0.team == 2 && s0.class_index == 1 &&
          s0.position[0] == 1.5f && s0.position[2] == -3.0f && fabsf(s0.direction[1] - 0.5f) < 1e-6f && !strcmp(s0.name, "Other"),
          "SPAWN other");
    p = find_in(NET_PACKET_SPAWN, 1);
    if (p) memcpy(&s1, p->payload, sizeof(s1));
    CHECK(p && s1.uid == 9 && s1.is_you && s1.health == 100.0f && link.my_sid == 9 && g_events[KRUNKER_LINK_EV_JOINED] == 1, "SPAWN you");
    clear();

    // "k": only the other one becomes a STATE; degrees -> radians; health from "0"
    KrunkerValue k[26];
    for (int i = 0; i < 26; i++) k[i] = KRUNKER_V_NUM(0);
    k[0] = KRUNKER_V_NUM(7); k[1] = KRUNKER_V_NUM(10); k[4] = KRUNKER_V_NUM(90); k[5] = KRUNKER_V_NUM(-45); k[10] = KRUNKER_V_NUM(1);
    k[13] = KRUNKER_V_NUM(9);
    feed(&link, "k", (KrunkerValue[]) {KRUNKER_V_ARRAY(k, 26), KRUNKER_V_NUM(10), KRUNKER_V_NUM(1234)}, 3);
    NetStatePacket st;
    p = find_in(NET_PACKET_STATE, 0);
    if (p) memcpy(&st, p->payload, sizeof(st));
    CHECK(p && !find_in(NET_PACKET_STATE, 1) && st.uid == 7 && st.position[0] == 10.0f && fabsf(st.direction[1] - (float) (M_PI / 2)) < 1e-5f &&
          fabsf(st.direction[0] + (float) (M_PI / 4)) < 1e-5f && st.health == 60.0f && st.aim_val == 1.0f, "STATE");
    clear();

    // "l" -> RECON + PING_RESULT, answered "v"
    KrunkerValue l[25];
    for (int i = 0; i < 25; i++) l[i] = KRUNKER_V_NUM(0);
    l[0] = KRUNKER_V_NUM(41); l[1] = KRUNKER_V_NUM(63); l[2] = KRUNKER_V_NUM(5); l[5] = KRUNKER_V_NUM(-2); l[6] = KRUNKER_V_NUM(3);
    l[9] = KRUNKER_V_BOOL(1); l[18] = KRUNKER_V_NUM(250); l[23] = KRUNKER_V_NIL; l[24] = KRUNKER_V_NUM(100);
    feed(&link, "l", (KrunkerValue[]) {KRUNKER_V_ARRAY(l, 25)}, 1);
    NetReconPacket r;
    p = find_in(NET_PACKET_RECON, 0);
    if (p) memcpy(&r, p->payload, sizeof(r));
    CHECK(p && r.ack_seq == 41 && r.position[0] == 5.0f && r.velocity[0] == 3.0f && r.velocity[1] == -2.0f && (r.flags & NET_RECON_ON_GROUND) &&
          r.slide_ms == 250.0 && fabsf(r.slide_timer - 0.25f) < 1e-6f && fabsf(r.can_slide_timer - 0.1f) < 1e-6f && !r.grapple_active, "RECON");
    p = find_in(NET_PACKET_PING_RESULT, 0);
    CHECK(p && ((const NetPingResultPacket *) p->payload)->ping_ms == 63, "PING_RESULT");
    CHECK(g_out_count == 1 && !strcmp(g_out[0].name, "v") && g_out[0].ah_echo == (int) NEXT_AH(), "PONG v");
    clear();

    // pi -> po
    feed(&link, "pi", NULL, 0);
    CHECK(g_out_count == 1 && !strcmp(g_out[0].name, "po") && g_out[0].ah_echo == (int) NEXT_AH(), "po");
    clear();

    // h / 4 / 3 / ch / 2 / error
    feed(&link, "h", (KrunkerValue[]) {KRUNKER_V_NUM(45), KRUNKER_V_NIL}, 2);
    p = find_in(NET_PACKET_HEALTH, 0);
    CHECK(p && ((const NetHealthPacket *) p->payload)->uid == 9 && ((const NetHealthPacket *) p->payload)->health == 45.0f, "HEALTH you");
    feed(&link, "4", (KrunkerValue[]) {KRUNKER_V_NUM(7), KRUNKER_V_NUM(24.6), KRUNKER_V_BOOL(1), KRUNKER_V_BOOL(0)}, 4);
    p = find_in(NET_PACKET_HIT, 0);
    CHECK(p && ((const NetHitPacket *) p->payload)->target_uid == 7 && ((const NetHitPacket *) p->payload)->damage == 25 &&
          ((const NetHitPacket *) p->payload)->crit, "HIT");
    const KrunkerValue info[] = {KRUNKER_V_STR("wId"), KRUNKER_V_NUM(3), KRUNKER_V_STR("fs"), KRUNKER_V_BOOL(0)};
    feed(&link, "3", (KrunkerValue[]) {KRUNKER_V_NUM(7), KRUNKER_V_NUM(2), KRUNKER_V_NUM(9), KRUNKER_V_NIL, KRUNKER_V_MAP(info, 2)}, 5);
    NetKillPacket kill;
    p = find_in(NET_PACKET_KILL, 0);
    if (p) memcpy(&kill, p->payload, sizeof(kill));
    CHECK(p && kill.victim_uid == 7 && kill.killer_uid == 9 && kill.weapon_index == 3 && kill.victim_deaths == 2, "KILL");
    feed(&link, "ch", (KrunkerValue[]) {KRUNKER_V_NUM(-1), KRUNKER_V_STR("Other"), KRUNKER_V_STR("hello there"), KRUNKER_V_NUM(0)}, 4);
    p = find_in(NET_PACKET_CHAT, 0);
    CHECK(p && !strcmp(((const NetChatPacket *) p->payload)->text, "hello there") && !strcmp(((const NetChatPacket *) p->payload)->name, "Other") &&
          ((const NetChatPacket *) p->payload)->kind == NET_CHAT_PLAYER, "CHAT");
    feed(&link, "2", (KrunkerValue[]) {KRUNKER_V_NUM(7)}, 1);
    p = find_in(NET_PACKET_LEAVE, 0);
    CHECK(p && ((const NetLeavePacket *) p->payload)->uid == 7, "LEAVE");
    feed(&link, "error", (KrunkerValue[]) {KRUNKER_V_STR("Kicked"), KRUNKER_V_STR("000005")}, 2);
    p = find_in(NET_PACKET_ERROR, 0);
    CHECK(p && !strcmp(((const NetErrorPacket *) p->payload)->message, "Kicked") && !strcmp(((const NetErrorPacket *) p->payload)->code, "000005") &&
          g_events[KRUNKER_LINK_EV_ERROR] == 1, "ERROR");
    clear();

    // t UPDATE_TIMER [formatted, state, rawMs] -> NET_PACKET_TIMER (seconds = floor(ms/1000), like
    // formatTimer); state 0 is the running round clock, 1/2/3 the end-screen countdown
    NetTimerPacket tp;
    feed(&link, "t", (KrunkerValue[]) {KRUNKER_V_STR("2:05"), KRUNKER_V_NUM(0), KRUNKER_V_NUM(125400)}, 3);
    p = find_in(NET_PACKET_TIMER, 0);
    if (p) memcpy(&tp, p->payload, sizeof(tp));
    CHECK(p && tp.seconds == 125 && tp.end == 0, "TIMER round clock (%d end %d)", p ? tp.seconds : -1, p ? tp.end : -1);
    clear();
    // state 1 end countdown, rawMs absent: the "M:SS" string is parsed instead
    feed(&link, "t", (KrunkerValue[]) {KRUNKER_V_STR("0:08"), KRUNKER_V_NUM(1)}, 2);
    p = find_in(NET_PACKET_TIMER, 0);
    if (p) memcpy(&tp, p->payload, sizeof(tp));
    CHECK(p && tp.seconds == 8 && tp.end == 1, "TIMER end countdown (%d end %d)", p ? tp.seconds : -1, p ? tp.end : -1);
    clear();
    // start [formattedTimer, ...] seeds the round clock from the string (no raw ms)
    feed(&link, "start", (KrunkerValue[]) {KRUNKER_V_STR("4:00"), KRUNKER_V_NUM(0)}, 2);
    p = find_in(NET_PACKET_TIMER, 0);
    if (p) memcpy(&tp, p->payload, sizeof(tp));
    CHECK(p && tp.seconds == 240 && tp.end == 0, "START seeds the timer (%d)", p ? tp.seconds : -1);
    clear();

    // cc: answered, undefined result (empty payload still sent), garbage (no ccr)
    feed(&link, "cc", (KrunkerValue[]) {KRUNKER_V_STR("n0nce"), KRUNKER_V_NIL, KRUNKER_V_STR("Y3Q=")}, 3);
    CHECK(g_out_count == 1 && !strcmp(g_out[0].name, "ccr") && g_out[0].argc == 3 && !strcmp(krunker_str(krunker_frame_arg(&g_out[0], 0)), "n0nce") &&
          !strcmp(krunker_str(krunker_frame_arg(&g_out[0], 1)), "AAEC") && !strcmp(krunker_str(krunker_frame_arg(&g_out[0], 2)), "0123abcd") &&
          g_out[0].ah_echo == (int) NEXT_AH(), "ccr");
    clear();
    feed(&link, "cc", (KrunkerValue[]) {KRUNKER_V_STR("n1"), KRUNKER_V_NIL, KRUNKER_V_STR("undef")}, 3);
    CHECK(g_out_count == 1 && krunker_is(krunker_frame_arg(&g_out[0], 1), KRUNKER_STR) && krunker_frame_arg(&g_out[0], 1)->len == 0 &&
          g_out[0].ah_echo == (int) NEXT_AH(), "ccr with an empty payload");
    clear();
    feed(&link, "cc", (KrunkerValue[]) {KRUNKER_V_STR("n2"), KRUNKER_V_NIL, KRUNKER_V_STR("garbage")}, 3);
    CHECK(g_out_count == 0 && link.cc_failed == 1, "garbage cc: no ccr");
    clear();

    // cpt: the ALTCHA proof of work -> "cptR" [base64 JSON], without an ahNum step
    {
        const char *salt = "s4lt?expires=1700000000";
        char pre[64], hex[65];
        uint8_t digest[32];
        snprintf(pre, sizeof(pre), "%s%d", salt, 1234);
        kn_sha256_bytes(pre, strlen(pre), digest);
        kn_sha256_hex(digest, hex);
        const KrunkerValue chal[] = {KRUNKER_V_STR("algorithm"), KRUNKER_V_STR("SHA-256"), KRUNKER_V_STR("challenge"), KRUNKER_V_STR(hex),
                                     KRUNKER_V_STR("maxnumber"), KRUNKER_V_NUM(5000), KRUNKER_V_STR("salt"), KRUNKER_V_STR(salt),
                                     KRUNKER_V_STR("signature"), KRUNKER_V_STR("abcdef")};
        feed(&link, "cpt", (KrunkerValue[]) {KRUNKER_V_MAP(chal, 5)}, 1);
        char json[512] = "";
        if (g_out_count == 1) b64_decode(krunker_str(krunker_frame_arg(&g_out[0], 0)), json, sizeof(json));
        char want[256];
        snprintf(want, sizeof(want), "{\"algorithm\":\"SHA-256\",\"challenge\":\"%s\",\"number\":1234,\"salt\":\"%s\",\"signature\":\"abcdef\",\"took\":", hex, salt);
        CHECK(g_out_count == 1 && !strcmp(g_out[0].name, "cptR") && !strncmp(json, want, strlen(want)) && g_out[0].ah_echo == (int) ah &&
              link.captchas_solved == 1, "cptR (%s)", json);
        clear();
        const KrunkerValue bad[] = {KRUNKER_V_STR("algorithm"), KRUNKER_V_STR("SHA-256"), KRUNKER_V_STR("challenge"), KRUNKER_V_STR(hex),
                                    KRUNKER_V_STR("maxnumber"), KRUNKER_V_NUM(1000), KRUNKER_V_STR("salt"), KRUNKER_V_STR(salt)};
        feed(&link, "cpt", (KrunkerValue[]) {KRUNKER_V_MAP(bad, 4)}, 1);
        CHECK(g_out_count == 0 && g_events[KRUNKER_LINK_EV_CAPTCHA] == 1, "unsolvable cpt: no cptR, a captcha event");
        clear();
        const KrunkerValue huge[] = {KRUNKER_V_STR("algorithm"), KRUNKER_V_STR("SHA-256"), KRUNKER_V_STR("challenge"), KRUNKER_V_STR(hex),
                                     KRUNKER_V_STR("maxnumber"), KRUNKER_V_NUM(2e7), KRUNKER_V_STR("salt"), KRUNKER_V_STR(salt)};
        feed(&link, "cpt", (KrunkerValue[]) {KRUNKER_V_MAP(huge, 4)}, 1);
        CHECK(g_out_count == 0 && g_events[KRUNKER_LINK_EV_CAPTCHA] == 1, "cpt above the maxnumber cap refused");
        clear();
        const KrunkerValue sha1[] = {KRUNKER_V_STR("algorithm"), KRUNKER_V_STR("SHA-1"), KRUNKER_V_STR("challenge"), KRUNKER_V_STR(hex),
                                     KRUNKER_V_STR("salt"), KRUNKER_V_STR(salt)};
        feed(&link, "cpt", (KrunkerValue[]) {KRUNKER_V_MAP(sha1, 3)}, 1);
        CHECK(g_out_count == 0 && g_events[KRUNKER_LINK_EV_CAPTCHA] == 1, "cpt with another algorithm refused");
        clear();
    }

    // our ENT -> "en"
    NetEntPacket ent;
    memset(&ent, 0, sizeof(ent));
    ent.class_index = 3;
    ent.secondary_index = -1;
    ent.lag_comp = 0.5f;
    krunker_link_send_ours(&link, NET_PACKET_ENT, &ent, sizeof(ent));
    const KrunkerValue *enter = g_out_count ? krunker_frame_arg(&g_out[0], 0) : NULL;
    CHECK(g_out_count == 1 && !strcmp(g_out[0].name, "en") && g_out[0].argc == 12 && krunker_is(enter, KRUNKER_ARRAY) && enter->len == 35 &&
          krunker_num(krunker_at(enter, 0), -1) == 3 && krunker_num(krunker_at(enter, 5), -1) == 2 && krunker_num(krunker_at(enter, 11), -1) == 0.5 &&
          isnan(krunker_num(krunker_at(enter, 26), 0)) && fnum(&g_out[0], 1) == 16 && fnum(&g_out[0], 2) == 18 &&
          g_out[0].ah_echo == (int) NEXT_AH(), "en");
    clear();

    // input windows -> "q"
    NetInputPacket in[3];
    memset(in, 0, sizeof(in));
    for (int i = 0; i < 3; i++) {
        in[i].seq = 100 + i;
        in[i].delta = i == 1 ? 0.017f : 0.016f;
        in[i].move_dir = 2;
        in[i].x_dir = 0.1f;
        in[i].y_dir = 1.0f + 0.01f * (float) i;
        in[i].flags = i == 2 ? NET_INPUT_JUMP : 0;
    }
    uint8_t batch[1 + sizeof(in)];
    batch[0] = 3;
    memcpy(batch + 1, in, sizeof(in));
    krunker_link_send_ours(&link, NET_PACKET_INPUT_BATCH, batch, sizeof(batch));
    const KrunkerFrame *q = &g_out[0];
    const KrunkerValue *angles = g_out_count ? krunker_frame_arg(q, 4) : NULL;
    const KrunkerValue *changes = g_out_count ? krunker_frame_arg(q, 5) : NULL;
    CHECK(g_out_count == 1 && !strcmp(q->name, "q") && q->argc == 6 && fnum(q, 0) == 0 && fnum(q, 1) == 100 &&
          !strcmp(krunker_str(krunker_frame_arg(q, 2)), "161716") && fnum(q, 3) == 2, "q head");
    CHECK(krunker_is(angles, KRUNKER_ARRAY) && angles->len == 6 && krunker_num(krunker_at(angles, 0), 0) == 100 &&
          krunker_num(krunker_at(angles, 1), 0) == 1000 && krunker_num(krunker_at(angles, 3), 0) == 10 && krunker_num(krunker_at(angles, 5), 0) == 10,
          "q angles");
    CHECK(krunker_is(changes, KRUNKER_MAP) && changes->len == 12 && krunker_num(krunker_get(changes, "0-4"), -1) == 2 &&
          krunker_num(krunker_get(changes, "2-7"), -1) == 1 && krunker_num(krunker_get(changes, "0-12"), -1) == 0, "q changes (%u)",
          changes ? changes->len : 0);
    CHECK(q->ah_echo == (int) NEXT_AH(), "q ahNum");
    clear();
    // the same window again, no turn: only the head (angles equal, nothing changed... but jump released: one change)
    for (int i = 0; i < 3; i++) { in[i].seq = 103 + i; in[i].delta = 0.016f; in[i].y_dir = 1.02f; in[i].flags = 0; }
    memcpy(batch + 1, in, sizeof(in));
    krunker_link_send_ours(&link, NET_PACKET_INPUT_BATCH, batch, sizeof(batch));
    angles = g_out_count ? krunker_frame_arg(&g_out[0], 4) : NULL;
    changes = g_out_count ? krunker_frame_arg(&g_out[0], 5) : NULL;
    CHECK(g_out_count == 1 && g_out[0].argc == 6 && !strcmp(krunker_str(krunker_frame_arg(&g_out[0], 2)), "161616") &&
          krunker_is(angles, KRUNKER_ARRAY) && angles->len == 2 && krunker_is(changes, KRUNKER_MAP) && changes->len == 1 &&
          krunker_num(krunker_get(changes, "0-7"), -1) == 0, "q window 2 (jump released)");
    clear();
    krunker_link_send_ours(&link, NET_PACKET_INPUT_BATCH, batch, sizeof(batch));
    CHECK(g_out_count == 1 && g_out[0].argc == 4, "q window 3: nothing changed, head only (argc %u)", g_out_count ? g_out[0].argc : 0);
    clear();
    // SYNC_FAILED: the next window is flagged and repeats everything
    feed(&link, "f", (KrunkerValue[]) {KRUNKER_V_NUM(3)}, 1);
    krunker_link_send_ours(&link, NET_PACKET_INPUT_BATCH, batch, sizeof(batch));
    changes = g_out_count ? krunker_frame_arg(&g_out[0], 5) : NULL;
    CHECK(g_out_count == 1 && fnum(&g_out[0], 0) == 1 && krunker_is(changes, KRUNKER_MAP) && changes->len == 11, "q after SYNC_FAILED");
    clear();

    // chat out
    NetChatSendPacket chat;
    memset(&chat, 0, sizeof(chat));
    snprintf(chat.text, sizeof(chat.text), "gg");
    krunker_link_send_ours(&link, NET_PACKET_CHAT_SEND, &chat, sizeof(chat));
    CHECK(g_out_count == 1 && !strcmp(g_out[0].name, "ct") && fnum(&g_out[0], 0) == 0 && !strcmp(krunker_str(krunker_frame_arg(&g_out[0], 1)), "gg"),
          "ct");
    clear();
    CHECK(!krunker_link_send_ours(&link, NET_PACKET_HELLO, "abc", 3) && g_out_count == 0, "HELLO dropped");

    printf("%d/%d passed\n", g_pass, g_pass + g_fail);
    return g_fail ? 1 : 0;
}
