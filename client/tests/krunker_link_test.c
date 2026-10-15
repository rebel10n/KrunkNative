// client/src/net/krunker_link.c (the official-Krunker packet translation) without sockets: Krunker
// frames in (built with the codec, as a server would send them) -> NetPackets out; our packets in
// -> Krunker frames out (decoded again and checked field by field, ahNum trailers included).
//   cmake-build-debug/krunker_link_test
#include <krunker_link.h>
#include <auth_join.h>
#include <net_ext.h>
#include <ruleset.h>
#include <zombies.h>
#include <cJSON.h>
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

// the ruleset the "rg" class change reads (krunker_link.c on_regen): two classes, the second with a secondary
static const int k_loadout_a[] = {1}, k_loadout_b[] = {3};
static const ClassConfig k_classes[2] = {{.name = "A", .loadout = k_loadout_a, .health = 100},
                                         {.name = "B", .loadout = k_loadout_b, .secondary = 1, .health = 150}};
static Weapon k_weapons[WEAPON_ID_KNIFE + 1];
const ClassConfig *rules_classes(void) { return k_classes; }
size_t rules_class_count(void) { return 2; }
const Weapon *rules_weapon(const int id) {
    if (id < 0 || id > WEAPON_ID_KNIFE) return NULL;
    k_weapons[id].ammo = (unsigned) (10 + id); // a distinct full magazine per id
    k_weapons[id].secondary = id == WEAPON_ID_PISTOL || id == 4; // two secondaries
    return &k_weapons[id];
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

    // init's lobby settings: [6] config, [7] weps / cls / prks, [12] networking -> GAME_CONFIG + NETWORK_DATA before INIT
    {
        const KrunkerValue classes[] = {KRUNKER_V_NUM(0), KRUNKER_V_NUM(2)};
        const KrunkerValue cfg_kv[] = {KRUNKER_V_STR("gravMlt"), KRUNKER_V_NUM(0.5), KRUNKER_V_STR("strafeSpd"), KRUNKER_V_NUM(1.5),
                                       KRUNKER_V_STR("classes"), KRUNKER_V_ARRAY(classes, 2), KRUNKER_V_STR("weps"), KRUNKER_V_NUM(1)};
        const KrunkerValue speed_kv[] = {KRUNKER_V_STR("speed"), KRUNKER_V_NUM(1.1)};
        const KrunkerValue cls_kv[] = {KRUNKER_V_STR("0"), KRUNKER_V_MAP(speed_kv, 1)};
        const KrunkerValue custom_kv[] = {KRUNKER_V_STR("cls"), KRUNKER_V_MAP(cls_kv, 1)};
        const KrunkerValue net_kv[] = {KRUNKER_V_STR("snf"), KRUNKER_V_NUM(20), KRUNKER_V_STR("tf"), KRUNKER_V_NUM(60)};
        KrunkerValue init_args[13];
        for (int i = 0; i < 13; i++) init_args[i] = KRUNKER_V_NIL;
        init_args[0] = KRUNKER_V_NUM(10);
        init_args[1] = KRUNKER_V_NUM(0);
        init_args[6] = KRUNKER_V_MAP(cfg_kv, 4);
        init_args[7] = KRUNKER_V_MAP(custom_kv, 1);
        init_args[12] = KRUNKER_V_MAP(net_kv, 2);
        feed(&link, "init", init_args, 13);
        int cfg_at = -1, net_at = -1, init_at = -1;
        for (int i = 0; i < g_in_count; i++) {
            if (g_in[i].type == NET_PACKET_GAME_CONFIG && cfg_at < 0) cfg_at = i;
            if (g_in[i].type == NET_PACKET_NETWORK_DATA) net_at = i;
            if (g_in[i].type == NET_PACKET_INIT) init_at = i;
        }
        CHECK(cfg_at >= 0 && net_at > cfg_at && init_at > net_at, "GAME_CONFIG, NETWORK_DATA, then INIT (%d %d %d)", cfg_at, net_at, init_at);
        cJSON *msg = NULL;
        if (cfg_at >= 0) {
            NetGameConfigChunk chunk;
            memcpy(&chunk, g_in[cfg_at].payload, g_in[cfg_at].length);
            const size_t n = g_in[cfg_at].length - NET_GAME_CONFIG_HEADER_SIZE;
            CHECK(chunk.offset == 0 && chunk.total == n, "one config chunk (%u of %u)", (unsigned) n, chunk.total);
            msg = cJSON_ParseWithLength((const char *) chunk.data, n);
        }
        const cJSON *settings = cJSON_GetObjectItemCaseSensitive(msg, "settings");
        const cJSON *cls0 = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(msg, "cls"), "0");
        CHECK(cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(settings, "gravMlt")) == 0.5 &&
              cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(settings, "strafeSpd")) == 1.5 &&
              !cJSON_GetObjectItemCaseSensitive(settings, "weps") && !cJSON_GetObjectItemCaseSensitive(msg, "weps") &&
              cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(msg, "classes")) == 2 &&
              cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(cls0, "speed")) == 1.1 &&
              cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(msg, "host")) == 0,
              "the lobby config as our GAME_CONFIG message");
        cJSON_Delete(msg);
        NetNetworkDataPacket rates;
        memset(&rates, 0, sizeof(rates));
        if (net_at >= 0) memcpy(&rates, g_in[net_at].payload, sizeof(rates));
        NetInitPacket init2;
        memset(&init2, 0, sizeof(init2));
        if (init_at >= 0) memcpy(&init2, g_in[init_at].payload, sizeof(init2));
        CHECK(rates.send_hz == 20 && rates.tick_hz == 60 && rates.sync_hz == 1 && init2.tick_rate == 60 && link.tick_hz == 60,
              "applyNetworking: snf 20, tf 60, sf || 1 (got %u %u %u, INIT %d)", rates.send_hz, rates.tick_hz, rates.sync_hz, init2.tick_rate);
        clear();
        // a bare init (no settings): the defaults (applyNetworking({}), applyConfig({}))
        feed(&link, "init", (KrunkerValue[]) {KRUNKER_V_NUM(10), KRUNKER_V_NUM(0)}, 2);
        p = find_in(NET_PACKET_NETWORK_DATA, 0);
        if (p) memcpy(&rates, p->payload, sizeof(rates));
        CHECK(p && find_in(NET_PACKET_GAME_CONFIG, 0) && rates.send_hz == 10 && rates.tick_hz == 30 && rates.sync_hz == 1, "bare init: default rates");
    }
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
          fabsf(st.direction[0] + (float) (M_PI / 4)) < 1e-5f && st.health == 60.0f && st.aim_val == 0.0f, "STATE (aimVal 1 = hip = our 0)");
    clear();

    // "l" -> RECON + PING_RESULT, answered "v"
    KrunkerValue l[25];
    for (int i = 0; i < 25; i++) l[i] = KRUNKER_V_NUM(0);
    l[0] = KRUNKER_V_NUM(41); l[1] = KRUNKER_V_NUM(63); l[2] = KRUNKER_V_NUM(5); l[5] = KRUNKER_V_NUM(-2); l[6] = KRUNKER_V_NUM(3);
    l[9] = KRUNKER_V_BOOL(1); l[18] = KRUNKER_V_NUM(250); l[23] = KRUNKER_V_NIL; l[24] = KRUNKER_V_NUM(100);
    l[13] = KRUNKER_V_NUM(3); l[15] = KRUNKER_V_NUM(0.25); // onWall side 3, aimVal 0.25 (mostly aimed)
    feed(&link, "l", (KrunkerValue[]) {KRUNKER_V_ARRAY(l, 25)}, 1);
    NetReconPacket r;
    p = find_in(NET_PACKET_RECON, 0);
    if (p) memcpy(&r, p->payload, sizeof(r));
    CHECK(p && r.ack_seq == 41 && r.position[0] == 5.0f && r.velocity[0] == 3.0f && r.velocity[1] == -2.0f && (r.flags & NET_RECON_ON_GROUND) &&
          r.slide_ms == 250.0 && fabsf(r.slide_timer - 0.25f) < 1e-6f && fabsf(r.can_slide_timer - 0.1f) < 1e-6f && !r.grapple_active, "RECON");
    CHECK(p && r.on_wall == 3 && fabsf(r.aim_val - 0.75f) < 1e-6f, "RECON onWall side %d, aimVal 0.25 -> ours 0.75 (%f)", p ? r.on_wall : -1,
          p ? r.aim_val : -1.0f);
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

    // the loadout: a guest's "en" keeps a fresh guest's cosmetics; a socket signed in to a krunker.io
    // account (link.account, krunker_account.h) sends the player's own
    ent.cosmetics = PLAYER_COSMETICS_NONE;
    ent.cosmetics.hat = 5;
    ent.cosmetics.body = 6;
    ent.cosmetics.face = 11;
    ent.skins = (PlayerSkins) {100, 101, 200};
    ent.spray_index = 3000;
    ent.cosmetics_v7 = (PlayerCosmeticsV7) {7, 12, 13, 14, 15, {8, 9}, 16, -1};
    krunker_link_send_ours(&link, NET_PACKET_ENT, &ent, sizeof(ent));
    enter = g_out_count ? krunker_frame_arg(&g_out[0], 0) : NULL;
    NEXT_AH();
    CHECK(enter && krunker_num(krunker_at(enter, 3), 0) == -1 && krunker_num(krunker_at(enter, 1), 0) == 2482 &&
          krunker_num(krunker_at(krunker_at(enter, 2), 0), 0) == -1 && krunker_num(krunker_at(enter, 9), 0) == -1 &&
          krunker_is(krunker_at(krunker_at(enter, 29), 0), KRUNKER_NIL), "guest en: no cosmetics");
    clear();
    link.account = 1;
    krunker_link_send_ours(&link, NET_PACKET_ENT, &ent, sizeof(ent));
    enter = g_out_count ? krunker_frame_arg(&g_out[0], 0) : NULL;
    CHECK(enter && enter->len == 35 && krunker_num(krunker_at(enter, 0), -1) == 3 && krunker_num(krunker_at(enter, 1), 0) == 3000 &&
          krunker_num(krunker_at(krunker_at(enter, 2), 0), 0) == 100 && krunker_num(krunker_at(krunker_at(enter, 2), 1), 0) == 101 &&
          krunker_num(krunker_at(enter, 3), 0) == 5 && krunker_num(krunker_at(enter, 4), 0) == 6 && krunker_num(krunker_at(enter, 9), 0) == 200 &&
          krunker_num(krunker_at(enter, 15), 0) == 13 && krunker_num(krunker_at(enter, 20), 0) == 11 && krunker_num(krunker_at(enter, 21), 0) == 14 &&
          krunker_num(krunker_at(enter, 25), 0) == 15 && krunker_num(krunker_at(krunker_at(enter, 29), 0), 0) == 8 &&
          krunker_num(krunker_at(krunker_at(enter, 29), 1), 0) == 9 && krunker_num(krunker_at(enter, 30), 0) == 12 &&
          krunker_num(krunker_at(enter, 31), 0) == 7 && krunker_num(krunker_at(enter, 32), 0) == 16 && krunker_num(krunker_at(enter, 14), 0) == -1 &&
          g_out[0].ah_echo == (int) NEXT_AH(), "signed-in en: the player's own loadout");
    clear();
    link.account = 0;

    // "a" ACCOUNT_RESPONSE: the account signed in on the socket (name, level from player_score)
    const KrunkerValue acc_data[] = {KRUNKER_V_STR("player_score"), KRUNKER_V_NUM(40000), KRUNKER_V_STR("player_kills"), KRUNKER_V_NUM(3)};
    feed(&link, "a", (KrunkerValue[]) {KRUNKER_V_NIL, KRUNKER_V_NUM(123), KRUNKER_V_STR("KioPlayer"), KRUNKER_V_MAP(acc_data, 2), KRUNKER_V_STR("legacy")}, 5);
    CHECK(g_events[KRUNKER_LINK_EV_ACCOUNT] == 1 && !strcmp(link.account_name, "KioPlayer") && link.account_level == 6 && g_out_count == 0,
          "a: account %s level %d", link.account_name, link.account_level);
    clear();
    feed(&link, "a", (KrunkerValue[]) {KRUNKER_V_STR("Invalid session"), KRUNKER_V_NIL, KRUNKER_V_NIL}, 3);
    CHECK(g_events[KRUNKER_LINK_EV_ACCOUNT] == 0 && !strcmp(link.account_name, "KioPlayer"), "a with an error: not signed in");
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
    CHECK(krunker_is(changes, KRUNKER_MAP) && changes->len == 12 && krunker_num(krunker_get(changes, "0-4"), -1) == 3 && // right: movDirs 3
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
    p = find_in(NET_PACKET_EXT, 0);
    {
        uint16_t subtype = 0, body_length = 1;
        const uint8_t *drop_body = NULL;
        CHECK(p && net_ext_unpack(p->payload, p->length, &subtype, &drop_body, &body_length) && subtype == NET_EXT_KN_INPUTS_DROPPED && body_length == 0 &&
              link.sync_failed == 1, "SYNC_FAILED drops the pending inputs (NET_EXT_KN_INPUTS_DROPPED)");
    }
    clear();
    // start (startGame, game.deob.js:218971-218974): the encoder forgets what it sent, so the next window
    // repeats every field with absolute angles (resync 0), though nothing changed since the last one
    feed(&link, "start", (KrunkerValue[]) {KRUNKER_V_STR("3:00")}, 1);
    krunker_link_send_ours(&link, NET_PACKET_INPUT_BATCH, batch, sizeof(batch));
    changes = g_out_count ? krunker_frame_arg(&g_out[0], 5) : NULL;
    CHECK(g_out_count == 1 && fnum(&g_out[0], 0) == 0 && krunker_is(changes, KRUNKER_MAP) && changes->len == 11 &&
              krunker_is(krunker_frame_arg(&g_out[0], 4), KRUNKER_ARRAY),
          "q after start: every field and the angles again (changes %u)", changes ? changes->len : 0);
    clear();

    // movement keys: our move_dir m (angle -PI/2 + m PI/4, forward 0) -> movDirs index (m + 1) % 8, none -1
    static const int dirs[][2] = {{-1, -1}, {0, 1}, {1, 2}, {2, 3}, {4, 5}, {6, 7}, {7, 0}};
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        NetInputPacket one;
        memset(&one, 0, sizeof(one));
        one.move_dir = dirs[i][0];
        double v[15];
        krunker_link_input_fields(&one, v);
        CHECK(v[4] == dirs[i][1], "move_dir %d -> input[4] %d (got %g)", dirs[i][0], dirs[i][1], v[4]);
    }

    // chat out
    NetChatSendPacket chat;
    memset(&chat, 0, sizeof(chat));
    snprintf(chat.text, sizeof(chat.text), "gg");
    krunker_link_send_ours(&link, NET_PACKET_CHAT_SEND, &chat, sizeof(chat));
    CHECK(g_out_count == 1 && !strcmp(g_out[0].name, "ct") && fnum(&g_out[0], 0) == 0 && !strcmp(krunker_str(krunker_frame_arg(&g_out[0], 1)), "gg"),
          "ct");
    clear();
    CHECK(!krunker_link_send_ours(&link, NET_PACKET_HELLO, "abc", 3) && g_out_count == 0, "HELLO dropped");
    clear();

    // ---- packet-audit cluster A: movement locks / buffs of our player
    feed(&link, "0", (KrunkerValue[]) {KRUNKER_V_ARRAY(add, KRUNKER_ADD_STRIDE)}, 1); // the other player (sid 7, socket sockB, team 2) is back
    clear();
#define PAYLOAD(type, var, packet) type var; memset(&var, 0, sizeof(var)); if (packet) memcpy(&var, (packet)->payload, sizeof(var))
    feed(&link, "lm", (KrunkerValue[]) {KRUNKER_V_NUM(6), KRUNKER_V_NUM(1), KRUNKER_V_NUM(-6), KRUNKER_V_NUM(0.5)}, 4);
    p = find_in(NET_PACKET_LOCK, 0);
    PAYLOAD(NetLockPacket, lk, p);
    CHECK(p && lk.kind == NET_LOCK_MOVE && lk.locked == 1 && link.move_lock == 1, "lm: LOCK move");
    p = find_in(NET_PACKET_TELEPORT_ACK, 0);
    PAYLOAD(NetTeleportPacket, tpa, p);
    CHECK(p && tpa.x == 6.0f && fabsf(tpa.y - 1.01f) < 1e-5f && tpa.z == -6.0f, "lm: resetPosition (y + 0.01) as a teleport");
    clear();
    // a "q" while move-locked: W held (moveDir 0) and a jump go out as moveDir -1, no jump
    for (int i = 0; i < 3; i++) { in[i].seq = 300 + i; in[i].move_dir = 0; in[i].flags = i == 1 ? NET_INPUT_JUMP : 0; }
    memcpy(batch + 1, in, sizeof(in));
    krunker_link_send_ours(&link, NET_PACKET_INPUT_BATCH, batch, sizeof(batch));
    changes = g_out_count ? krunker_frame_arg(&g_out[0], 5) : NULL;
    CHECK(krunker_is(changes, KRUNKER_MAP) && krunker_num(krunker_get(changes, "0-4"), 9) == -1 && !krunker_get(changes, "1-7") &&
          !krunker_get(changes, "2-4"), "q while lm holds: moveDir -1, no jump (%u changes)", changes ? changes->len : 0);
    clear();
    feed(&link, "ulm", NULL, 0);
    p = find_in(NET_PACKET_LOCK, 0);
    PAYLOAD(NetLockPacket, ulk, p);
    CHECK(p && ulk.kind == NET_LOCK_MOVE && ulk.locked == 0 && link.move_lock == 0, "ulm: LOCK move off");
    for (int i = 0; i < 3; i++) in[i].seq = 303 + i;
    memcpy(batch + 1, in, sizeof(in));
    krunker_link_send_ours(&link, NET_PACKET_INPUT_BATCH, batch, sizeof(batch));
    changes = g_out_count ? krunker_frame_arg(&g_out[g_out_count - 1], 5) : NULL;
    CHECK(krunker_is(changes, KRUNKER_MAP) && krunker_num(krunker_get(changes, "0-4"), 9) == 1 && krunker_num(krunker_get(changes, "1-7"), 9) == 1, // our move_dir 0 = movDirs 1
          "q after ulm: moveDir and jump back");
    clear();
    // lock (window.locked): nothing pressed goes out, the frames keep their isn
    feed(&link, "lock", (KrunkerValue[]) {KRUNKER_V_BOOL(1)}, 1);
    p = find_in(NET_PACKET_LOCK, 0);
    PAYLOAD(NetLockPacket, plk, p);
    CHECK(p && plk.kind == NET_LOCK_PLAYER && plk.locked == 1 && link.locked, "lock: LOCK player");
    for (int i = 0; i < 3; i++) { in[i].seq = 306 + i; in[i].flags = NET_INPUT_SHOOT | NET_INPUT_CROUCH; }
    memcpy(batch + 1, in, sizeof(in));
    krunker_link_send_ours(&link, NET_PACKET_INPUT_BATCH, batch, sizeof(batch));
    const KrunkerFrame *lq = g_out_count ? &g_out[g_out_count - 1] : NULL;
    changes = lq ? krunker_frame_arg(lq, 5) : NULL;
    CHECK(lq && fnum(lq, 1) == 306 && krunker_is(changes, KRUNKER_MAP) && krunker_num(krunker_get(changes, "0-4"), 9) == -1 &&
          !krunker_get(changes, "0-5") && !krunker_get(changes, "0-9"), "q while locked: isn kept, nothing pressed");
    feed(&link, "start", (KrunkerValue[]) {KRUNKER_V_STR("3:00"), KRUNKER_V_NUM(1), KRUNKER_V_BOOL(0), KRUNKER_V_NUM(0), KRUNKER_V_NUM(1)}, 5);
    CHECK(link.move_lock == 1 && link.locked == 0 && find_in(NET_PACKET_LOCK, 1) && find_in(NET_PACKET_LOCK, 2),
          "start: moveLock arg 4 locks, the player lock ends");
    clear();
    feed(&link, "start", (KrunkerValue[]) {KRUNKER_V_STR("3:00"), KRUNKER_V_NUM(1), KRUNKER_V_BOOL(0), KRUNKER_V_NUM(0), KRUNKER_V_NUM(0)}, 5);
    CHECK(link.move_lock == 0, "start: moveLock 0 unlocks");
    clear();
    // frz / noc / unl -> CHEAT flags; noclip rides every later "l"
    feed(&link, "frz", (KrunkerValue[]) {KRUNKER_V_BOOL(1)}, 1);
    feed(&link, "noc", (KrunkerValue[]) {KRUNKER_V_BOOL(1)}, 1);
    feed(&link, "unl", (KrunkerValue[]) {KRUNKER_V_BOOL(0)}, 1);
    const NetPacket *c0 = find_in(NET_PACKET_CHEAT, 0), *c1 = find_in(NET_PACKET_CHEAT, 1), *c2 = find_in(NET_PACKET_CHEAT, 2);
    CHECK(c0 && c0->payload[0] == NET_CHEAT_FROZEN && c0->payload[1] == 1 && c1 && c1->payload[0] == NET_CHEAT_NOCLIP && c1->payload[1] == 1 &&
          c2 && c2->payload[0] == NET_CHEAT_UNLIMITED_AMMO && c2->payload[1] == 0 && link.noclip, "frz / noc / unl");
    feed(&link, "l", (KrunkerValue[]) {KRUNKER_V_ARRAY(l, 25)}, 1);
    p = find_in(NET_PACKET_RECON, 0);
    PAYLOAD(NetReconPacket, rn, p);
    CHECK(p && (rn.flags & NET_RECON_NOCLIP), "noclip kept through \"l\"");
    clear();
    // spb / sic / jg / busy / chrg
    feed(&link, "spb", (KrunkerValue[]) {KRUNKER_V_NUM(2.5), KRUNKER_V_BOOL(0)}, 2);
    uint16_t sub = 0, blen = 0;
    const uint8_t *body = NULL;
    p = find_in(NET_PACKET_EXT, 0);
    NetSpeedBoostPacket sb;
    memset(&sb, 0, sizeof(sb));
    if (p && net_ext_unpack(p->payload, p->length, &sub, &body, &blen) && blen == sizeof(sb)) memcpy(&sb, body, sizeof(sb));
    CHECK(p && sub == NET_EXT_PLAYER_SPEED_BOOST && sb.boost == 2.5f && !sb.silent, "spb -> EXT speed boost");
    feed(&link, "sic", (KrunkerValue[]) {KRUNKER_V_BOOL(1)}, 1);
    p = find_in(NET_PACKET_SHARING, 0);
    CHECK(p && p->length == sizeof(NetSharingPacket) && p->payload[0] == 1, "sic -> SHARING");
    feed(&link, "jg", (KrunkerValue[]) {KRUNKER_V_NUM(7), KRUNKER_V_BOOL(1)}, 2);
    p = find_in(NET_PACKET_JUGGERNAUT, 0);
    PAYLOAD(NetJuggernautPacket, jg, p);
    CHECK(p && jg.uid == 7 && jg.on == 1 && jg.max_health == 0, "jg -> JUGGERNAUT");
    feed(&link, "busy", (KrunkerValue[]) {KRUNKER_V_NUM(7), KRUNKER_V_BOOL(1)}, 2);
    int busy7 = 0;
    for (int i = 0; i < link.player_count; i++) if (link.players[i].sid == 7) busy7 = link.players[i].busy;
    CHECK(busy7 == 1, "busy kept on the player");
    feed(&link, "chrg", (KrunkerValue[]) {KRUNKER_V_NUM(7), KRUNKER_V_NUM(15000)}, 2);
    p = find_in(NET_PACKET_MODE_COUNTER, 0);
    PAYLOAD(NetModeCounterPacket, mc, p);
    CHECK(p && mc.kind == NET_MODE_COUNTER_CHARGE && mc.uid == 7 && mc.value == 15000.0f, "chrg -> MODE_COUNTER charge");
    clear();

    // ---- packet-audit cluster B: player-state corrections
    // chgp finds the player by its socket id (findById), never by its sid
    feed(&link, "chgp", (KrunkerValue[]) {KRUNKER_V_STR("sockB"), KRUNKER_V_NUM(10), KRUNKER_V_NUM(2), KRUNKER_V_NUM(3), KRUNKER_V_BOOL(1),
                                          KRUNKER_V_BOOL(1), KRUNKER_V_NUM(1)}, 7);
    feed(&link, "chgp", (KrunkerValue[]) {KRUNKER_V_NUM(7), KRUNKER_V_NUM(0), KRUNKER_V_NUM(0), KRUNKER_V_NUM(0)}, 4);
    feed(&link, "chgp", (KrunkerValue[]) {KRUNKER_V_STR("sockA"), KRUNKER_V_NUM(-4), KRUNKER_V_NUM(0), KRUNKER_V_NUM(8), KRUNKER_V_BOOL(0),
                                          KRUNKER_V_BOOL(0), KRUNKER_V_NIL}, 7);
    p = find_in(NET_PACKET_CHANGE_POSITION, 0);
    PAYLOAD(NetChangePositionPacket, cp, p);
    CHECK(p && cp.uid == 7 && cp.x == 10.0f && cp.y == 2.0f && cp.z == 3.0f && cp.recon && cp.stop_momentum && cp.has_direction && cp.direction == 1,
          "chgp sockB -> CHANGE_POSITION sid 7");
    p = find_in(NET_PACKET_CHANGE_POSITION, 1);
    PAYLOAD(NetChangePositionPacket, cp2, p);
    CHECK(p && cp2.uid == 9 && cp2.x == -4.0f && cp2.z == 8.0f && !cp2.has_direction && !cp2.stop_momentum && !find_in(NET_PACKET_CHANGE_POSITION, 2),
          "chgp sockA -> us; a sid as the id finds nobody");
    clear();
    feed(&link, "tm", (KrunkerValue[]) {KRUNKER_V_NUM(9), KRUNKER_V_NUM(2)}, 2);
    p = find_in(NET_PACKET_TEAM, 0);
    PAYLOAD(NetTeamPacket, tmp, p);
    CHECK(p && tmp.uid == 9 && tmp.team == 2 && tmp.health == 45.0f && tmp.max_health == 100, "tm -> TEAM (health kept)");
    feed(&link, "tm", (KrunkerValue[]) {KRUNKER_V_NUM(4242), KRUNKER_V_NUM(1)}, 2);
    CHECK(!find_in(NET_PACKET_TEAM, 1), "tm for an unknown sid: nothing");
    clear();
    feed(&link, "wep", (KrunkerValue[]) {KRUNKER_V_NUM(5)}, 1);
    p = find_in(NET_PACKET_OBJECTIVE, 0);
    PAYLOAD(NetObjectivePacket, ob, p);
    CHECK(p && ob.kind == NET_OBJECTIVE_WEAPON && ob.value == 5, "wep -> OBJECTIVE weapon 5");
    feed(&link, "rfl", NULL, 0);
    p = find_in(NET_PACKET_REFILL, 0);
    CHECK(p && p->length == 0, "rfl -> REFILL");
    feed(&link, "gsc", (KrunkerValue[]) {KRUNKER_V_NUM(3)}, 1);
    CHECK(link.game_state == 3, "gsc -> game_state");
    feed(&link, "spk", (KrunkerValue[]) {KRUNKER_V_STR("speedBoost"), KRUNKER_V_NUM(3)}, 2);
    p = find_in(NET_PACKET_EXT, 0);
    memset(&sb, 0, sizeof(sb));
    if (p && net_ext_unpack(p->payload, p->length, &sub, &body, &blen) && blen == sizeof(sb)) memcpy(&sb, body, sizeof(sb));
    CHECK(p && sub == NET_EXT_PLAYER_SPEED_BOOST && sb.boost == 3.0f && sb.silent, "spk speedBoost -> silent EXT speed boost");
    clear();
    // rg with a class change: the class loadout (primary, secondary, knife; full ammo) and its health
    KrunkerValue items[19];
    for (int i = 0; i < 19; i++) items[i] = KRUNKER_V_NUM(-1);
    items[14] = KRUNKER_V_NUM(4);
    items[15] = KRUNKER_V_NUM(1);
    feed(&link, "rg", (KrunkerValue[]) {KRUNKER_V_NUM(7), KRUNKER_V_BOOL(0), KRUNKER_V_ARRAY(items, 19)}, 3);
    CHECK(!find_in(NET_PACKET_LOADOUT, 0) && !find_in(NET_PACKET_TEAM, 0), "rg without a class change: cosmetics only");
    feed(&link, "rg", (KrunkerValue[]) {KRUNKER_V_NUM(7), KRUNKER_V_BOOL(1), KRUNKER_V_ARRAY(items, 19)}, 3);
    p = find_in(NET_PACKET_LOADOUT, 0);
    PAYLOAD(NetLoadoutPacket, lo, p);
    CHECK(p && lo.uid == 7 && lo.count == 3 && lo.weapons[0] == 3 && lo.weapons[1] == 4 && lo.weapons[2] == WEAPON_ID_KNIFE && lo.ammo[0] == 13 &&
          lo.ammo[1] == 14 && lo.ammo[2] == 10 + WEAPON_ID_KNIFE, "rg class change -> LOADOUT (the picked secondary)");
    p = find_in(NET_PACKET_TEAM, 0);
    PAYLOAD(NetTeamPacket, rgt, p);
    CHECK(p && rgt.uid == 7 && rgt.team == 2 && rgt.health == 150.0f && rgt.max_health == 150, "rg class change -> TEAM (class health)");
    clear();
    items[14] = KRUNKER_V_NUM(5); // not a secondary weapon: setClass falls back to 2, the Pistol
    feed(&link, "rg", (KrunkerValue[]) {KRUNKER_V_NUM(7), KRUNKER_V_BOOL(1), KRUNKER_V_ARRAY(items, 19)}, 3);
    p = find_in(NET_PACKET_LOADOUT, 0);
    PAYLOAD(NetLoadoutPacket, lo2, p);
    CHECK(p && lo2.count == 3 && lo2.weapons[1] == WEAPON_ID_PISTOL, "rg: a non-secondary secIndex becomes the Pistol");
    clear();
    // CHANGE_HEALTH: health above maxHealth raises it; resetMax sets it, also downwards (the TEAM carries it)
    feed(&link, "h", (KrunkerValue[]) {KRUNKER_V_NUM(2000), KRUNKER_V_NUM(7)}, 2);
    p = find_in(NET_PACKET_TEAM, 0);
    PAYLOAD(NetTeamPacket, rai, p);
    CHECK(p && rai.uid == 7 && rai.max_health == 2000 && rai.health == 2000.0f, "h above maxHealth raises it (TEAM)");
    clear();
    feed(&link, "h", (KrunkerValue[]) {KRUNKER_V_NUM(1500), KRUNKER_V_NUM(7)}, 2);
    CHECK(find_in(NET_PACKET_HEALTH, 0) && !find_in(NET_PACKET_TEAM, 0), "h below maxHealth: no TEAM");
    clear();
    feed(&link, "h", (KrunkerValue[]) {KRUNKER_V_NUM(120), KRUNKER_V_NUM(7), KRUNKER_V_NIL, KRUNKER_V_NIL, KRUNKER_V_NIL, KRUNKER_V_BOOL(1)}, 6);
    int max7 = 0;
    for (int i = 0; i < link.player_count; i++) if (link.players[i].sid == 7) max7 = link.players[i].max_health;
    p = find_in(NET_PACKET_TEAM, 0);
    PAYLOAD(NetTeamPacket, rmt, p);
    CHECK(max7 == 120 && p && rmt.uid == 7 && rmt.max_health == 120 && rmt.health == 120.0f && !find_in(NET_PACKET_TEAM, 1),
          "h resetMax lowers maxHealth (%d)", max7);
    clear();
    // 00: renames by socket id; keepOpen falsy closes
    const KrunkerValue names[] = {KRUNKER_V_STR("sockB"), KRUNKER_V_STR("Renamed"), KRUNKER_V_STR("al"), KRUNKER_V_STR("nobody"), KRUNKER_V_STR("X"),
                                  KRUNKER_V_STR("")};
    feed(&link, "00", (KrunkerValue[]) {KRUNKER_V_ARRAY(names, 6), KRUNKER_V_BOOL(1)}, 2);
    p = find_in(NET_PACKET_EXT, 0);
    NetPlayerNamePacket nm;
    memset(&nm, 0, sizeof(nm));
    if (p && net_ext_unpack(p->payload, p->length, &sub, &body, &blen) && blen == sizeof(nm)) memcpy(&nm, body, sizeof(nm));
    CHECK(p && sub == NET_EXT_PLAYER_NAME && nm.uid == 7 && !strcmp(nm.name, "Renamed") && !find_in(NET_PACKET_EXT, 1) &&
          !g_events[KRUNKER_LINK_EV_CLOSE], "00 keepOpen: one rename, no close");
    clear();
    feed(&link, "00", (KrunkerValue[]) {KRUNKER_V_ARRAY(NULL, 0), KRUNKER_V_BOOL(0)}, 2);
    CHECK(g_events[KRUNKER_LINK_EV_CLOSE] == 1, "00 keepOpen false: close");
    clear();
    feed(&link, "00", (KrunkerValue[]) {KRUNKER_V_ARRAY(NULL, 0)}, 1);
    CHECK(g_events[KRUNKER_LINK_EV_CLOSE] == 1, "00 without keepOpen: close");
    clear();
    // init: our client resets game.moveLock and its players, so do the link's locks
    feed(&link, "lm", (KrunkerValue[]) {KRUNKER_V_NUM(0), KRUNKER_V_NUM(0), KRUNKER_V_NUM(0), KRUNKER_V_NUM(0)}, 4);
    feed(&link, "init", (KrunkerValue[]) {KRUNKER_V_NUM(10), KRUNKER_V_NUM(0), KRUNKER_V_NUM(0)}, 3);
    CHECK(link.move_lock == 0 && link.noclip == 0, "init resets the locks");
    clear();
#undef PAYLOAD

    // the player list: vote kick out ("v2k" [alias, 1] / [alias]) and the "vk" state in
    {
        uint8_t buf[NET_EXT_HEADER_SIZE + sizeof(NetVoteKickPacket)];
        NetVoteKickPacket v;
        memset(&v, 0, sizeof(v));
        snprintf(v.target, sizeof(v.target), "Other");
        v.vote = 1;
        uint16_t n = net_ext_pack(buf, sizeof(buf), NET_EXT_VOTE_KICK, &v, sizeof(v));
        CHECK(krunker_link_send_ours(&link, NET_PACKET_EXT, buf, n) && g_out_count == 1 && !strcmp(g_out[0].name, "v2k") &&
              g_out[0].argc == 2 && !strcmp(krunker_str(krunker_frame_arg(&g_out[0], 0)), "Other") && fnum(&g_out[0], 1) == 1,
              "v2k [alias, 1]");
        clear();
        v.vote = 0;
        n = net_ext_pack(buf, sizeof(buf), NET_EXT_VOTE_KICK, &v, sizeof(v));
        CHECK(krunker_link_send_ours(&link, NET_PACKET_EXT, buf, n) && g_out_count == 1 && g_out[0].argc == 1, "v2k [alias] (the [2] key)");
        clear();
    }
    {
        uint8_t buf[NET_EXT_HEADER_SIZE + sizeof(NetKpdCallPacket)];
        const NetKpdCallPacket c = {.tab = 0, .uid = 9}; // sid 9 = socket "sockA" (the "0" above; 7 left since)
        const uint16_t n = net_ext_pack(buf, sizeof(buf), NET_EXT_KPD_CALL, &c, sizeof(c));
        CHECK(krunker_link_send_ours(&link, NET_PACKET_EXT, buf, n) && g_out_count == 1 && !strcmp(g_out[0].name, "kpd") &&
              fnum(&g_out[0], 0) == 0 && !strcmp(krunker_str(krunker_get(krunker_frame_arg(&g_out[0], 1), "id")), "sockA"),
              "kpd [0, {id: socket id}]");
        clear();
    }
    feed(&link, "vk", (KrunkerValue[]) {KRUNKER_V_STR("Other"), KRUNKER_V_NUM(1), KRUNKER_V_NUM(4), KRUNKER_V_BOOL(1), KRUNKER_V_STR("Me")}, 5);
    {
        const NetPacket *e = find_in(NET_PACKET_EXT, 0);
        uint16_t sub = 0, bl = 0;
        const uint8_t *b = NULL;
        NetVoteKickStatePacket st;
        memset(&st, 0, sizeof(st));
        if (e && net_ext_unpack(e->payload, e->length, &sub, &b, &bl) && bl >= sizeof(st)) memcpy(&st, b, sizeof(st));
        CHECK(sub == NET_EXT_VOTE_KICK_STATE && st.active && st.opened && st.votes == 1 && st.needed == 4 && !strcmp(st.target, "Other") &&
              !strcmp(st.voter, "Me"), "vk -> VOTE_KICK_STATE opened");
        clear();
    }
    feed(&link, "vk", (KrunkerValue[]) {KRUNKER_V_NIL}, 1);
    {
        const NetPacket *e = find_in(NET_PACKET_EXT, 0);
        uint16_t sub = 0, bl = 0;
        const uint8_t *b = NULL;
        NetVoteKickStatePacket st;
        memset(&st, 0xff, sizeof(st));
        if (e && net_ext_unpack(e->payload, e->length, &sub, &b, &bl) && bl >= sizeof(st)) memcpy(&st, b, sizeof(st));
        CHECK(sub == NET_EXT_VOTE_KICK_STATE && !st.active, "vk null -> the vote ended");
        clear();
    }
    const KrunkerValue kpd_data[] = {KRUNKER_V_STR("n"), KRUNKER_V_STR("Other")};
    feed(&link, "kpd", (KrunkerValue[]) {KRUNKER_V_NUM(0), KRUNKER_V_STR("reported"), KRUNKER_V_MAP(kpd_data, 1)}, 3);
    {
        const NetPacket *e = find_in(NET_PACKET_EXT, 0);
        uint16_t sub = 0, bl = 0;
        const uint8_t *b = NULL;
        NetKpdResponsePacket r;
        memset(&r, 0, sizeof(r));
        if (e && net_ext_unpack(e->payload, e->length, &sub, &b, &bl) && bl >= sizeof(r)) memcpy(&r, b, sizeof(r));
        CHECK(sub == NET_EXT_KPD_RESPONSE && r.code == 0 && r.status == NET_KPD_REPORTED && !strcmp(r.name, "Other") &&
              !strcmp(r.message, "Reported Other to the K.P.D"), "kpd reported -> KPD_RESPONSE");
        clear();
    }
    { // the officer's verdict out ("rkpd" [1]) and "kpdf" in
        uint8_t buf[NET_EXT_HEADER_SIZE + sizeof(NetKpdRatePacket)];
        const NetKpdRatePacket rate = {.tagged = 1};
        const uint16_t n = net_ext_pack(buf, sizeof(buf), NET_EXT_KPD_RATE, &rate, sizeof(rate));
        CHECK(krunker_link_send_ours(&link, NET_PACKET_EXT, buf, n) && g_out_count == 1 && !strcmp(g_out[0].name, "rkpd") &&
                  g_out[0].argc == 1 && fnum(&g_out[0], 0) == 1,
              "rkpd [1]");
        clear();
        feed(&link, "kpdf", NULL, 0);
        const NetPacket *e = find_in(NET_PACKET_EXT, 0);
        uint16_t sub = 0, bl = 0;
        const uint8_t *b = NULL;
        CHECK(e && net_ext_unpack(e->payload, e->length, &sub, &b, &bl) && sub == NET_EXT_KPD_RESET, "kpdf -> KPD_RESET");
        clear();
    }

    printf("%d/%d passed\n", g_pass, g_pass + g_fail);
    return g_fail ? 1 : 0;
}
