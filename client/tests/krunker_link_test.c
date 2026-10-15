// client/src/net/krunker_link.c (the official-Krunker packet translation) without sockets: Krunker
// frames in (built with the codec, as a server would send them) -> NetPackets out; our packets in
// -> Krunker frames out (decoded again and checked field by field, ahNum trailers included).
//   cmake-build-debug/krunker_link_test
#include <krunker_link.h>
#include <krunker_maps.h>
#include <auth_join.h>
#include <net_ext.h>
#include <ruleset.h>
#include <zombies.h>
#include <social_v7.h>
#include <ai.h>
#include <krunker_sounds.h>
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
    if (id == 9) k_weapons[id].projectile_config = (ProjectileConfig) {.speed = 0.6f, .gravity = 0.0001f, .range = 1200.0f}; // "pr": a rocket
    return &k_weapons[id];
}

// ---- test double: the mode table (shared/src/modes/modes.c in the client): CTF (3)'s columns
static const char *const k_ctf_lead[] = {"caps", "score", NULL};
static const char *const k_ctf_end[] = {"sid", "name", "score", "kills", "deaths", "caps", "reward", NULL};
const GameModeConfig *mode_config(const int id) {
    static const GameModeConfig ctf = {.leader_stats = k_ctf_lead, .end_stats = k_ctf_end};
    return id == 3 ? &ctf : NULL;
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

// io.map double: the inits the transport would resolve
static KrunkerLinkInit g_inits[8];
static int g_init_count;
static void cap_map(void *user, KrunkerLinkInit *info) {
    (void) user;
    if (g_init_count < 8) {
        g_inits[g_init_count++] = *info;
        info->custom = NULL; // taken (freed by the test)
    }
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

// the body of an official-HUD EXT (net_ext.h NET_EXT_KN_OFFICIAL_*), NULL when the nth EXT is another
static const uint8_t *ext_body(const int nth, const uint16_t subtype, uint16_t *len) {
    const NetPacket *p = find_in(NET_PACKET_EXT, nth);
    if (!p || p->length < 2 || (p->payload[0] | p->payload[1] << 8) != subtype) return NULL;
    *len = (uint16_t) (p->length - 2);
    return p->payload + 2;
}

#define READ_IN(type, var, kind, nth) \
    type var; \
    memset(&var, 0, sizeof(var)); \
    const NetPacket *var##_p = find_in(NET_PACKET_##kind, nth); \
    if (var##_p && var##_p->length == sizeof(type)) memcpy(&var, var##_p->payload, sizeof(type)); \
    else var##_p = NULL

// Clusters C / D of the packet audit: score / kills / ammo HUD, objectives / flags / rounds / end
static void test_hud(void) {
    KrunkerLink link;
    const KrunkerLinkIO io = {.emit = cap_emit, .send = cap_send, .event = cap_event, .cheat = fake_cheat};
    krunker_link_init(&link, &io, KRUNKER_AHKEY);
    feed(&link, "io-init", (KrunkerValue[]) {KRUNKER_V_STR("sockA")}, 1);
    clear();

    // init: CTF (3), the joiner's team scores + flag lines, syncData flg / obj / zone
    const KrunkerValue ts1[] = {KRUNKER_V_NUM(1), KRUNKER_V_NUM(3), KRUNKER_V_STR("AT BASE")};
    const KrunkerValue ts2[] = {KRUNKER_V_NUM(2), KRUNKER_V_NUM(1), KRUNKER_V_STR("AWAY")};
    const KrunkerValue scores[] = {KRUNKER_V_ARRAY(ts1, 3), KRUNKER_V_ARRAY(ts2, 3)};
    const KrunkerValue f0[] = {KRUNKER_V_NUM(40), KRUNKER_V_NUM(1), KRUNKER_V_NUM(2), KRUNKER_V_NUM(3), KRUNKER_V_NUM(0), KRUNKER_V_NIL};
    const KrunkerValue f1[] = {KRUNKER_V_NUM(41), KRUNKER_V_NUM(4), KRUNKER_V_NUM(5), KRUNKER_V_NUM(6), KRUNKER_V_NUM(7), KRUNKER_V_NUM(900)};
    const KrunkerValue flg[] = {KRUNKER_V_ARRAY(f0, 6), KRUNKER_V_ARRAY(f1, 6)};
    const KrunkerValue obj[] = {KRUNKER_V_NUM(3), KRUNKER_V_NUM(5000)};
    const KrunkerValue zone[] = {KRUNKER_V_NUM(10), KRUNKER_V_NUM(20), KRUNKER_V_NUM(30), KRUNKER_V_NUM(40)};
    const KrunkerValue sync[] = {KRUNKER_V_STR("gor"), KRUNKER_V_NUM(1), KRUNKER_V_STR("flg"), KRUNKER_V_ARRAY(flg, 2),
                                 KRUNKER_V_STR("obj"), KRUNKER_V_ARRAY(obj, 2), KRUNKER_V_STR("zone"), KRUNKER_V_ARRAY(zone, 4)};
    KrunkerValue init[12];
    for (int i = 0; i < 12; i++) init[i] = KRUNKER_V_NIL;
    init[0] = KRUNKER_V_NUM(10);
    init[1] = KRUNKER_V_NUM(3);
    init[4] = KRUNKER_V_ARRAY(scores, 2);
    init[11] = KRUNKER_V_MAP(sync, 4);
    feed(&link, "init", init, 12);
    CHECK(link.mode == 3 && find_in(NET_PACKET_INIT, 0), "init: mode 3 (%d)", link.mode);
    {
        READ_IN(NetTeamScorePacket, a, TEAM_SCORE, 0);
        const NetPacket *b = find_in(NET_PACKET_TEAM_SCORE, 1);
        NetTeamScorePacket bb = {0};
        if (b) memcpy(&bb, b->payload, sizeof(bb));
        CHECK(a_p && a.team == 1 && a.score == 3 && b && bb.team == 2 && bb.score == 1, "init: team scores 3 / 1");
        READ_IN(NetFlagPacket, line, FLAG, 1);
        CHECK(line_p && line.index == -1 && line.flag.team == 2 && !strcmp(line.info, "AWAY"), "init: team 2's flag line");
        READ_IN(NetFlagPacket, fl, FLAG, 3);
        CHECK(fl_p && fl.index == 1 && fl.flag.carrier_uid == 7 && fl.flag.position[1] == 5.0f && fl.flag.team == 0 && !fl.info[0],
              "init: flag uid 41 is index 1, carried by 7");
        READ_IN(NetObjectiveV7Packet, o, OBJECTIVE_V7, 0);
        CHECK(o_p && o.uid == 3 && o.time_ms == 5000 && !o.initial, "init: objective 3, 5 s");
        READ_IN(NetZonePacket, z, ZONE, 0);
        CHECK(z_p && z.x == 10 && z.z == 20 && z.size_x == 30 && z.size_z == 40, "init: zone");
    }
    clear();

    // players: us (9), 7, 5
    KrunkerValue add[3 * 53];
    for (int i = 0; i < 3 * 53; i++) add[i] = KRUNKER_V_NIL;
    add[0] = KRUNKER_V_STR("sockA"); add[1] = KRUNKER_V_NUM(9); add[8] = KRUNKER_V_NUM(100); add[9] = KRUNKER_V_NUM(1);
    add[53] = KRUNKER_V_STR("sockB"); add[54] = KRUNKER_V_NUM(7); add[61] = KRUNKER_V_NUM(100); add[62] = KRUNKER_V_NUM(2);
    add[106] = KRUNKER_V_STR("sockC"); add[107] = KRUNKER_V_NUM(5); add[114] = KRUNKER_V_NUM(100); add[115] = KRUNKER_V_NUM(2);
    feed(&link, "0", (KrunkerValue[]) {KRUNKER_V_ARRAY(add, 3 * 53)}, 1);
    CHECK(link.my_sid == 9, "joined as 9");
    clear();

    // ts
    feed(&link, "ts", (KrunkerValue[]) {KRUNKER_V_NUM(1), KRUNKER_V_NUM(5)}, 2);
    feed(&link, "ts", NULL, 0);
    {
        const NetPacket *a = find_in(NET_PACKET_TEAM_SCORE, 0), *b = find_in(NET_PACKET_TEAM_SCORE, 1);
        NetTeamScorePacket x = {0}, y = {9, 9};
        if (a) memcpy(&x, a->payload, sizeof(x));
        if (b) memcpy(&y, b->payload, sizeof(y));
        CHECK(a && x.team == 1 && x.score == 5 && b && y.team == 0 && y.score == 0, "ts: team 1 = 5, then cleared (team 0)");
    }
    clear();

    // "5" GET_SCORE: delta, relay, absolute
    feed(&link, "5", (KrunkerValue[]) {KRUNKER_V_NUM(10), KRUNKER_V_BOOL(0), KRUNKER_V_BOOL(0), KRUNKER_V_NUM(0)}, 4);
    feed(&link, "5", (KrunkerValue[]) {KRUNKER_V_NUM(5)}, 1);
    feed(&link, "5", (KrunkerValue[]) {KRUNKER_V_NUM(100), KRUNKER_V_BOOL(0), KRUNKER_V_BOOL(1)}, 3);
    feed(&link, "5", (KrunkerValue[]) {KRUNKER_V_NUM(7), KRUNKER_V_NUM(1)}, 2);
    {
        NetScorePacket sc[4];
        memset(sc, 0, sizeof(sc));
        int ok = 1;
        for (int i = 0; i < 4; i++) {
            const NetPacket *p = find_in(NET_PACKET_SCORE, i);
            if (!p || p->length != sizeof(NetScorePacket)) ok = 0;
            else memcpy(&sc[i], p->payload, sizeof(sc[i]));
        }
        CHECK(ok && sc[0].delta == 10 && sc[0].score == 10 && sc[1].delta == 5 && sc[1].score == 15 && sc[2].delta == 0 &&
              sc[2].score == 100 && sc[3].delta == 7 && sc[3].score == 100, "5: +10, +5, absolute 100 (no popup), relay +7 (score kept)");
    }
    clear();

    // "6" GET_KILL: medals by index and by name, "" skipped; then our "3" does not count twice
    const KrunkerValue medals[] = {KRUNKER_V_NUM(9), KRUNKER_V_NUM(100), KRUNKER_V_NUM(0), KRUNKER_V_NUM(1), KRUNKER_V_STR(""),
                                   KRUNKER_V_NUM(50), KRUNKER_V_NUM(0), KRUNKER_V_NUM(1), KRUNKER_V_STR("Custom"), KRUNKER_V_STR("x2"),
                                   KRUNKER_V_NUM(0), KRUNKER_V_NUM(1)};
    feed(&link, "6", (KrunkerValue[]) {KRUNKER_V_ARRAY(medals, 12), KRUNKER_V_NUM(3), KRUNKER_V_NUM(4), KRUNKER_V_NUM(1)}, 4);
    {
        READ_IN(NetKillMedalsPacket, k, KILL_MEDALS, 0);
        CHECK(k_p && k.count == 2 && !strcmp(k.medals[0].text, "Headshot") && k.medals[0].score == 100 && !strcmp(k.medals[1].text, "Custom") &&
              k.medals[1].score == 0 && k.kills == 4, "6: Headshot +100, Custom (text score), kills 4");
    }
    feed(&link, "3", (KrunkerValue[]) {KRUNKER_V_NUM(5), KRUNKER_V_NUM(2), KRUNKER_V_NUM(9), KRUNKER_V_NIL, KRUNKER_V_MAP(NULL, 0)}, 5);
    feed(&link, "3", (KrunkerValue[]) {KRUNKER_V_NUM(9), KRUNKER_V_NUM(1), KRUNKER_V_NUM(7), KRUNKER_V_NIL, KRUNKER_V_MAP(NULL, 0)}, 5);
    {
        const NetPacket *a = find_in(NET_PACKET_KILL, 0), *b = find_in(NET_PACKET_KILL, 1);
        NetKillPacket x, y;
        memset(&x, 0, sizeof(x));
        memset(&y, 0, sizeof(y));
        if (a) memcpy(&x, a->payload, sizeof(x));
        if (b) memcpy(&y, b->payload, sizeof(y));
        CHECK(a && x.killer_uid == 9 && x.killer_kills == 4 && b && y.killer_uid == 7 && y.killer_kills == 1 && y.victim_deaths == 1,
              "3: our kill keeps the 6's 4, player 7 counts 1 (%d, %d)", x.killer_kills, y.killer_kills);
    }
    clear();

    // "7" UPDATE_LEADERS with CTF's leaderStats [caps, score]
    const KrunkerValue leaders[] = {KRUNKER_V_NUM(9), KRUNKER_V_NUM(2), KRUNKER_V_NUM(300), KRUNKER_V_NUM(7), KRUNKER_V_NUM(1), KRUNKER_V_NUM(150)};
    feed(&link, "7", (KrunkerValue[]) {KRUNKER_V_ARRAY(leaders, 6)}, 1);
    {
        READ_IN(NetLeaderboardPacket, b, LEADERBOARD, 0);
        CHECK(b_p && b.count == 2 && b.entries[0].uid == 9 && b.entries[0].score == 100 && b.entries[0].kills == 4 && b.entries[0].deaths == 1 &&
              b.entries[1].uid == 7 && b.entries[1].score == 150 && b.entries[1].kills == 1, "7: board rows (score from the column, ours our \"5\" score; kills / deaths kept)");
        READ_IN(NetLeaderStatsPacket, c, LEADER_STATS, 0);
        CHECK(c_p && c.count == 2 && c.stat_count == 2 && c.uid[1] == 7 && c.values[0][0] == 2 && c.values[0][1] == 300 && c.values[1][0] == 1,
              "7: leader stats caps / score");
    }
    clear();

    // "ulb", "lv", "uamo", "st", "kst", "wstk", "10", "am", "krt"
    const KrunkerValue rows[] = {KRUNKER_V_STR("Alice"), KRUNKER_V_NUM(500), KRUNKER_V_STR("Bob"), KRUNKER_V_NUM(1.5), KRUNKER_V_STR("odd")};
    feed(&link, "ulb", (KrunkerValue[]) {KRUNKER_V_NUM(1), KRUNKER_V_ARRAY(rows, 5)}, 2);
    {
        uint16_t len = 0;
        const uint8_t *body = ext_body(0, NET_EXT_KN_OFFICIAL_LEADER_BOARD, &len);
        NetOfficialLeaderBoardHeader h = {0};
        if (body) memcpy(&h, body, sizeof(h));
        const char *c = body ? (const char *) body + sizeof(h) : "";
        CHECK(body && h.index == 1 && h.cells == 4 && !strcmp(c, "Alice") && !strcmp(c + 6, "500") && !strcmp(c + 10, "Bob") && !strcmp(c + 14, "1.5") &&
              len == sizeof(h) + 18, "ulb: whole rows, numbers as JS prints them");
    }
    clear();
    feed(&link, "lv", (KrunkerValue[]) {KRUNKER_V_NUM(3)}, 1);
    const KrunkerValue ammos[] = {KRUNKER_V_NUM(30), KRUNKER_V_NIL}, reserves[] = {KRUNKER_V_NUM(120)};
    feed(&link, "uamo", (KrunkerValue[]) {KRUNKER_V_ARRAY(ammos, 2), KRUNKER_V_ARRAY(reserves, 1)}, 2);
    feed(&link, "st", (KrunkerValue[]) {KRUNKER_V_NUM(2), KRUNKER_V_NUM(1)}, 2);
    feed(&link, "kst", (KrunkerValue[]) {KRUNKER_V_NUM(7), KRUNKER_V_NUM(5)}, 2);
    feed(&link, "wstk", (KrunkerValue[]) {KRUNKER_V_NUM(3), KRUNKER_V_NUM(4)}, 2);
    feed(&link, "10", NULL, 0);
    const KrunkerValue pickup[] = {KRUNKER_V_NUM(24), KRUNKER_V_NUM(20), KRUNKER_V_NUM(0), KRUNKER_V_NUM(1)};
    feed(&link, "am", (KrunkerValue[]) {KRUNKER_V_ARRAY(pickup, 4)}, 1);
    feed(&link, "krt", (KrunkerValue[]) {KRUNKER_V_NUM(3), KRUNKER_V_NUM(0)}, 2);
    {
        READ_IN(NetLivesPacket, lv, LIVES, 0);
        CHECK(lv_p && lv.lives == 3, "lv");
        uint16_t len = 0;
        const uint8_t *body = ext_body(0, NET_EXT_KN_OFFICIAL_AMMO, &len);
        NetOfficialAmmoPacket a;
        memset(&a, 0, sizeof(a));
        if (body && len == sizeof(a)) memcpy(&a, body, sizeof(a));
        CHECK(body && len == sizeof(a) && a.count == 2 && a.ammo[0] == 30 && a.ammo[1] == -1 && a.reserve_count == 1 && a.reserve[0] == 120,
              "uamo: magazines 30 / null, reserve 120");
        READ_IN(NetStreakPacket, st, STREAK, 0);
        READ_IN(NetKillStreakPacket, kst, KILL_STREAK, 0);
        READ_IN(NetWeaponStreakPacket, ws, WEAPON_STREAK, 0);
        CHECK(st_p && st.streak == 2 && st.count == 1 && kst_p && kst.uid == 7 && kst.streak == 5 && ws_p && ws.weapon == 3 && ws.count == 4,
              "st / kst / wstk");
        const NetPacket *as = find_in(NET_PACKET_ASSIST, 0);
        CHECK(as && as->length == 0, "10: ASSIST");
        READ_IN(NetMedalPacket, m, MEDAL, 0);
        CHECK(m_p && !strcmp(m.text, "Pickup") && m.score == 20, "am: medalsList[24] Pickup +20");
        READ_IN(NetModeCounterPacket, kr, MODE_COUNTER, 0);
        CHECK(kr_p && kr.kind == NET_MODE_COUNTER_KR_TAGS && kr.uid == 9 && kr.value == 3.0f, "krt: our 3 tags");
    }
    clear();

    // cluster D: ufi / ufl / ufp
    feed(&link, "ufi", (KrunkerValue[]) {KRUNKER_V_NUM(2), KRUNKER_V_STR("Guest_9")}, 2);
    feed(&link, "ufl", (KrunkerValue[]) {KRUNKER_V_NUM(41), KRUNKER_V_NUM(10), KRUNKER_V_NUM(11), KRUNKER_V_NUM(12), KRUNKER_V_NUM(9)}, 5);
    feed(&link, "ufl", (KrunkerValue[]) {KRUNKER_V_NUM(99), KRUNKER_V_NUM(1), KRUNKER_V_NUM(1), KRUNKER_V_NUM(1), KRUNKER_V_NUM(0)}, 5);
    feed(&link, "ufp", (KrunkerValue[]) {KRUNKER_V_NUM(40), KRUNKER_V_NUM(5), KRUNKER_V_NUM(6), KRUNKER_V_NUM(7)}, 4);
    {
        NetFlagPacket fp[4];
        memset(fp, 0, sizeof(fp));
        int ok = 1;
        for (int i = 0; i < 4; i++) {
            const NetPacket *p = find_in(NET_PACKET_FLAG, i);
            if (!p || p->length != sizeof(NetFlagPacket)) ok = 0;
            else memcpy(&fp[i], p->payload, sizeof(fp[i]));
        }
        CHECK(ok && fp[0].index == -1 && fp[0].flag.team == 2 && !strcmp(fp[0].info, "Guest_9"), "ufi: team 2's line");
        CHECK(ok && fp[1].index == 1 && fp[1].flag.carrier_uid == 9 && fp[1].flag.position[2] == 12.0f && !fp[1].flag.team, "ufl: uid 41 = index 1, we carry it");
        CHECK(ok && fp[2].index == 2 && fp[2].flag.carrier_uid == -1, "ufl: an unknown uid takes the next index");
        CHECK(ok && fp[3].index == 0 && fp[3].flag.position[0] == 5.0f && fp[3].flag.carrier_uid == -1, "ufp: uid 40's ping");
    }
    clear();

    // obj / uobj / rds / zn / ac
    feed(&link, "obj", (KrunkerValue[]) {KRUNKER_V_NUM(4), KRUNKER_V_NUM(60000), KRUNKER_V_BOOL(1)}, 3);
    feed(&link, "uobj", (KrunkerValue[]) {KRUNKER_V_NUM(4), KRUNKER_V_NUM(2)}, 2);
    const KrunkerValue wins[] = {KRUNKER_V_STR("1"), KRUNKER_V_NUM(2), KRUNKER_V_STR("2"), KRUNKER_V_NUM(1)};
    feed(&link, "rds", (KrunkerValue[]) {KRUNKER_V_NUM(2), KRUNKER_V_BOOL(0), KRUNKER_V_NUM(1), KRUNKER_V_MAP(wins, 2)}, 4);
    feed(&link, "rds", (KrunkerValue[]) {KRUNKER_V_NUM(1), KRUNKER_V_BOOL(1), KRUNKER_V_NUM(1), KRUNKER_V_NIL}, 4);
    feed(&link, "zn", (KrunkerValue[]) {KRUNKER_V_ARRAY(zone, 4)}, 1);
    feed(&link, "ac", (KrunkerValue[]) {KRUNKER_V_NUM(7), KRUNKER_V_NIL, KRUNKER_V_STR("captured the Flag")}, 3);
    {
        READ_IN(NetObjectiveV7Packet, o, OBJECTIVE_V7, 0);
        READ_IN(NetObjectiveOccupancyPacket, u, OBJECTIVE_OCCUPANCY, 0);
        CHECK(o_p && o.uid == 4 && o.time_ms == 60000 && o.initial && u_p && u.uid == 4 && u.on_zone == 2, "obj / uobj");
        READ_IN(NetRoundsPacket, r, ROUNDS, 0);
        CHECK(r_p && r.rounds_left == 2 && r.win_team == 1 && r.has_round_wins && r.team_round_wins[1] == 2 && r.team_round_wins[2] == 1 &&
              !find_in(NET_PACKET_ROUNDS, 1), "rds: switching sides, round wins 2 / 1; a noSwitch rds is dropped");
        CHECK(find_in(NET_PACKET_ZONE, 0) != NULL, "zn");
        READ_IN(NetActionPacket, ac, ACTION, 0);
        CHECK(ac_p && ac.uid == 7 && ac.other_uid == -1 && !strcmp(ac.text, "captured the Flag"), "ac");
    }
    clear();

    // chi: [key, ...args] and a bare key
    const KrunkerValue chi_key[] = {KRUNKER_V_STR("event.easter.found.new"), KRUNKER_V_STR("Bob"), KRUNKER_V_NUM(3), KRUNKER_V_NUM(1.5)};
    feed(&link, "chi", (KrunkerValue[]) {KRUNKER_V_NUM(0), KRUNKER_V_NIL, KRUNKER_V_ARRAY(chi_key, 4), KRUNKER_V_NUM(2), KRUNKER_V_STR("#61fff7")}, 5);
    feed(&link, "chi", (KrunkerValue[]) {KRUNKER_V_NUM(-1), KRUNKER_V_NIL, KRUNKER_V_STR("modes.round.win"), KRUNKER_V_NUM(1)}, 4);
    {
        uint16_t len = 0;
        const uint8_t *body = ext_body(0, NET_EXT_KN_OFFICIAL_CHAT_I18N, &len);
        NetOfficialChatI18nHeader h;
        memset(&h, 0, sizeof(h));
        if (body) memcpy(&h, body, sizeof(h));
        const char *c = body ? (const char *) body + sizeof(h) : "x";
        CHECK(body && h.kind == NET_CHAT_SERVER && h.argc == 3 && !strcmp(h.color, "#61fff7") && !c[0] && !strcmp(c + 1, "event.easter.found.new") &&
              !strcmp(c + 24, "Bob") && !strcmp(c + 28, "3") && !strcmp(c + 30, "1.5"), "chi: key + 3 arguments, kind 2, colour");
        body = ext_body(1, NET_EXT_KN_OFFICIAL_CHAT_I18N, &len);
        if (body) memcpy(&h, body, sizeof(h));
        CHECK(body && h.kind == NET_CHAT_GAME && h.argc == 0 && !strcmp((const char *) body + sizeof(h) + 1, "modes.round.win"), "chi: a bare key");
    }
    clear();

    // end: CTF's endStats + "reward" (official), team 1 won, we are on it
    const KrunkerValue n9[] = {KRUNKER_V_STR("Guest_9"), KRUNKER_V_STR("Guest_9")}, n7[] = {KRUNKER_V_STR("Other"), KRUNKER_V_STR("Other")};
    const KrunkerValue ed[] = {KRUNKER_V_NUM(9), KRUNKER_V_ARRAY(n9, 2), KRUNKER_V_NUM(300), KRUNKER_V_NUM(4), KRUNKER_V_NUM(1), KRUNKER_V_NUM(2),
                               KRUNKER_V_STR("<span style='color:#F8C55C'>+12</span> KR"),
                               KRUNKER_V_NUM(7), KRUNKER_V_ARRAY(n7, 2), KRUNKER_V_NUM(150), KRUNKER_V_NUM(1), KRUNKER_V_NUM(0), KRUNKER_V_NUM(1),
                               KRUNKER_V_STR("NONE")};
    const KrunkerValue tm1[] = {KRUNKER_V_NUM(1), KRUNKER_V_NUM(3), KRUNKER_V_NIL}, tm2[] = {KRUNKER_V_NUM(2), KRUNKER_V_NUM(1), KRUNKER_V_NIL};
    const KrunkerValue tms[] = {KRUNKER_V_ARRAY(tm1, 3), KRUNKER_V_ARRAY(tm2, 3)};
    const KrunkerValue end_data[] = {KRUNKER_V_STR("ed"), KRUNKER_V_ARRAY(ed, 14), KRUNKER_V_STR("tms"), KRUNKER_V_ARRAY(tms, 2),
                                     KRUNKER_V_STR("mdls"), KRUNKER_V_ARRAY(NULL, 0)};
    KrunkerValue end_args[] = {KRUNKER_V_NUM(0), KRUNKER_V_BOOL(1), KRUNKER_V_MAP(end_data, 3), KRUNKER_V_NUM(1), KRUNKER_V_NUM(3),
                               KRUNKER_V_NUM(12), KRUNKER_V_NIL, KRUNKER_V_MAP(NULL, 0)};
    feed(&link, "end", end_args, 8);
    {
        const NetPacket *ep = find_in(NET_PACKET_END, 0);
        NetEndPacket e;
        memset(&e, 0, sizeof(e));
        if (ep && ep->length == sizeof(e)) memcpy(&e, ep->payload, sizeof(e));
        CHECK(ep && ep->length == sizeof(e) && e.result == 1 && e.winner_uid == -1 && e.next_round == 25 && e.board.count == 2 &&
              e.board.entries[0].uid == 9 && e.board.entries[0].score == 300 && e.board.entries[0].kills == 4 && e.board.entries[0].deaths == 1 &&
              e.rewards[0] == 12 && e.board.entries[1].uid == 7 && e.rewards[1] == 0, "end: victory, the ed rows, rewards 12 / NONE");
        READ_IN(NetModeEndPacket, me, MODE_END, 0);
        CHECK(me_p && me.mode_id == 3 && me.winner_team == 1 && me.team_scores[0] == 3 && me.team_scores[1] == 1 && me.count == 2 &&
              me.entries[0].stats[NET_MODE_STAT_CAPS] == 2 && me.entries[1].stats[NET_MODE_STAT_CAPS] == 1, "end: MODE_END team 1, caps");
        CHECK(ep && me_p && ep < me_p, "end: END before MODE_END (as our server sends them)");
    }
    clear();
    end_args[0] = KRUNKER_V_NUM(2); // no end screen: the board only
    feed(&link, "end", end_args, 8);
    CHECK(!find_in(NET_PACKET_END, 0) && find_in(NET_PACKET_LEADERBOARD, 0), "end type 2: no end screen, the board");
    clear();
    end_args[0] = KRUNKER_V_NUM(0);
    end_args[1] = KRUNKER_V_NUM(-1);
    end_args[3] = KRUNKER_V_NUM(-1);
    feed(&link, "end", end_args, 8);
    {
        READ_IN(NetEndPacket, e, END, 0);
        CHECK(e_p && e.result == -1 && e.winner_uid == -1, "end: didWin -1 is a draw");
    }
    clear();
    // another build's endStats (FFA, 4 columns sent where ours has 6): the rows are found by the name cells
    feed(&link, "init", (KrunkerValue[]) {KRUNKER_V_NUM(10), KRUNKER_V_NUM(0), KRUNKER_V_NUM(0)}, 3);
    clear();
    const KrunkerValue ed4[] = {KRUNKER_V_NUM(9), KRUNKER_V_ARRAY(n9, 2), KRUNKER_V_NUM(80), KRUNKER_V_NUM(2),
                                KRUNKER_V_NUM(7), KRUNKER_V_ARRAY(n7, 2), KRUNKER_V_NUM(60), KRUNKER_V_NUM(1),
                                KRUNKER_V_NUM(5), KRUNKER_V_ARRAY(n7, 2), KRUNKER_V_NUM(10), KRUNKER_V_NUM(0)};
    const KrunkerValue end4[] = {KRUNKER_V_STR("ed"), KRUNKER_V_ARRAY(ed4, 12)};
    feed(&link, "end", (KrunkerValue[]) {KRUNKER_V_NUM(0), KRUNKER_V_BOOL(0), KRUNKER_V_MAP(end4, 1), KRUNKER_V_NUM(-1)}, 4);
    {
        READ_IN(NetEndPacket, e, END, 0);
        CHECK(e_p && e.result == 0 && e.board.count == 3 && e.board.entries[1].uid == 7 && e.board.entries[2].uid == 5 && e.winner_uid == 9,
              "end: an unknown column list still finds the rows; FFA's winner is the first row (%d rows)", e.board.count);
    }
    clear();
}


// Names, account fields, the kill feed / death card, damage and health, chat tabs, the host
// (official-ingame: krunker_link.c on_add_players / on_kill / on_damage / on_health / on_chat / "uh")
static void test_combat(void) {
    KrunkerLink link;
    const KrunkerLinkIO io = {.emit = cap_emit, .send = cap_send, .event = cap_event, .cheat = fake_cheat};
    krunker_link_init(&link, &io, KRUNKER_AHKEY);
    feed(&link, "io-init", (KrunkerValue[]) {KRUNKER_V_STR("sockA")}, 1);
    feed(&link, "init", (KrunkerValue[]) {KRUNKER_V_NUM(10), KRUNKER_V_NUM(0), KRUNKER_V_NUM(0), KRUNKER_V_NIL, KRUNKER_V_NIL,
                                          KRUNKER_V_STR("sockB")}, 6); // arg 5: the host is sockB
    clear();
    // "0": us (9, class 0) and sockB (7: alias "Ace", name "acct", class 1, level 33, clan, premium,
    // featured, hacker, bot, creator)
    KrunkerValue add[2 * 53];
    for (int i = 0; i < 2 * 53; i++) add[i] = KRUNKER_V_NIL;
    add[0] = KRUNKER_V_STR("sockA"); add[1] = KRUNKER_V_NUM(9); add[5] = KRUNKER_V_STR("me"); add[8] = KRUNKER_V_NUM(100); add[9] = KRUNKER_V_NUM(1);
    KrunkerValue *b = add + 53;
    b[0] = KRUNKER_V_STR("sockB"); b[1] = KRUNKER_V_NUM(7); b[5] = KRUNKER_V_STR("acct"); b[6] = KRUNKER_V_NUM(1); b[8] = KRUNKER_V_NUM(100);
    b[9] = KRUNKER_V_NUM(2); b[KRUNKER_ADD_LEVEL] = KRUNKER_V_NUM(33); b[KRUNKER_ADD_CLAN] = KRUNKER_V_STR("CLN");
    b[KRUNKER_ADD_IS_HACKER] = KRUNKER_V_BOOL(1); b[KRUNKER_ADD_FEATURED] = KRUNKER_V_BOOL(1); b[KRUNKER_ADD_PREMIUM_T] = KRUNKER_V_NUM(5);
    b[KRUNKER_ADD_ALIAS] = KRUNKER_V_STR("Ace"); b[KRUNKER_ADD_ACCID] = KRUNKER_V_NUM(4242); b[KRUNKER_ADD_IS_BOT] = KRUNKER_V_BOOL(1);
    b[KRUNKER_ADD_CREATOR_BADGE] = KRUNKER_V_NUM(1);
    feed(&link, "0", (KrunkerValue[]) {KRUNKER_V_ARRAY(add, 2 * 53)}, 1);
    {
        READ_IN(NetSpawnPacket, s1, SPAWN, 1);
        CHECK(s1_p && s1.uid == 7 && !strcmp(s1.name, "Ace"), "0: the alias is the name (%s)", s1.name);
        READ_IN(NetPlayerInfoPacket, i0, PLAYER_INFO, 0);
        READ_IN(NetPlayerInfoPacket, i1, PLAYER_INFO, 1);
        CHECK(i0_p && i0.uid == 9 && !i0.flags && !i0.level, "0: our PLAYER_INFO (guest)");
        const int want = NET_PLAYER_INFO_PREMIUM | NET_PLAYER_INFO_VERIFIED | NET_PLAYER_INFO_HACKER | NET_PLAYER_INFO_BOT |
                         NET_PLAYER_INFO_CREATOR | NET_PLAYER_INFO_HOST;
        CHECK(i1_p && i1.uid == 7 && i1.level == 33 && i1.account_id == 4242 && !strcmp(i1.clan, "CLN") && i1.flags == want,
              "0: account fields of 7 (flags %#x)", i1_p ? i1.flags : 0);
    }
    clear();

    // "4": Math.ceil(dmg)
    feed(&link, "4", (KrunkerValue[]) {KRUNKER_V_NUM(7), KRUNKER_V_NUM(24.1), KRUNKER_V_BOOL(0), KRUNKER_V_BOOL(0)}, 4);
    {
        READ_IN(NetHitPacket, h, HIT, 0);
        CHECK(h_p && h.damage == 25, "4: 24.1 shows 25 (%d)", h.damage);
    }
    // "h": a source only without noHitMarker
    feed(&link, "h", (KrunkerValue[]) {KRUNKER_V_NUM(80), KRUNKER_V_NIL, KRUNKER_V_NUM(3), KRUNKER_V_NUM(4)}, 4);
    feed(&link, "h", (KrunkerValue[]) {KRUNKER_V_NUM(70), KRUNKER_V_NIL, KRUNKER_V_NUM(3), KRUNKER_V_NUM(4), KRUNKER_V_BOOL(1)}, 5);
    {
        READ_IN(NetHealthPacket, a, HEALTH, 0);
        READ_IN(NetHealthPacket, c, HEALTH, 1);
        CHECK(a_p && a.uid == 9 && a.has_source && a.source_x == 3.0f && c_p && !c.has_source, "h: noHitMarker drops the source");
    }
    clear();

    // "3": 7 kills us with a wallbang headshot, thrown melee; deathStats to the victim
    const KrunkerValue pc[] = {KRUNKER_V_NUM(2), KRUNKER_V_STR("1.52")};
    const KrunkerValue life[] = {KRUNKER_V_NUM(61000), KRUNKER_V_NUM(40), KRUNKER_V_STR("37.5%"), KRUNKER_V_NUM(4), KRUNKER_V_NUM(350)};
    const KrunkerValue stats[] = {KRUNKER_V_NUM(5), KRUNKER_V_NUM(1234), KRUNKER_V_ARRAY(pc, 2), KRUNKER_V_ARRAY(life, 5)};
    const KrunkerValue info[] = {KRUNKER_V_STR("wId"), KRUNKER_V_NUM(5), KRUNKER_V_STR("wb"), KRUNKER_V_BOOL(1), KRUNKER_V_STR("hs"),
                                 KRUNKER_V_BOOL(1), KRUNKER_V_STR("pj"), KRUNKER_V_BOOL(1)};
    feed(&link, "3", (KrunkerValue[]) {KRUNKER_V_NUM(9), KRUNKER_V_NUM(1), KRUNKER_V_NUM(7), KRUNKER_V_ARRAY(stats, 4), KRUNKER_V_MAP(info, 4),
                                       KRUNKER_V_BOOL(0)}, 6);
    {
        READ_IN(NetDeathInfoPacket, d, DEATH_INFO, 0);
        READ_IN(NetKillPacket, k, KILL, 0);
        CHECK(d_p && d.victim_uid == 9 && d.killer_uid == 7 && d.weapon_index == 5 && d.class_stat == 1234 && d.pc_stat_index == 2 &&
              !strncmp(d.pc_stat_value, "1.52", sizeof(d.pc_stat_value)) && d.time_alive_ms == 61000 && d.shots == 40 &&
              !strncmp(d.accuracy, "37.5%", sizeof(d.accuracy)) && d.kills == 4 && d.score == 350 && d.thrown && !d.fists,
              "3: DEATH_INFO for our own death");
        CHECK(k_p && d_p && d_p < k_p && k.killer_class == 1 && k.wallbang && k.headshot && (k.fists & NET_KILL_THROWN) &&
              !(k.fists & NET_KILL_HIDE_FEED) && NET_KILL_STREAK(k.fists) == -1, "3: the killer's class, wb / hs / pj, DEATH_INFO first");
    }
    clear();
    // another player's death: no DEATH_INFO; a streak kill (stk 3) with hideFeed; a Nuke (stk 0)
    const KrunkerValue info2[] = {KRUNKER_V_STR("stk"), KRUNKER_V_NUM(3)};
    const KrunkerValue info3[] = {KRUNKER_V_STR("stk"), KRUNKER_V_NUM(0), KRUNKER_V_STR("fs"), KRUNKER_V_BOOL(1)};
    feed(&link, "3", (KrunkerValue[]) {KRUNKER_V_NUM(7), KRUNKER_V_NUM(1), KRUNKER_V_NUM(9), KRUNKER_V_NIL, KRUNKER_V_MAP(info2, 1), KRUNKER_V_BOOL(1)}, 6);
    feed(&link, "3", (KrunkerValue[]) {KRUNKER_V_NUM(7), KRUNKER_V_NUM(2), KRUNKER_V_NUM(9), KRUNKER_V_NIL, KRUNKER_V_MAP(info3, 2)}, 5);
    {
        READ_IN(NetKillPacket, k, KILL, 0);
        READ_IN(NetKillPacket, n, KILL, 1);
        CHECK(!find_in(NET_PACKET_DEATH_INFO, 0) && k_p && NET_KILL_STREAK(k.fists) == 3 && (k.fists & NET_KILL_HIDE_FEED) &&
              !(k.fists & NET_KILL_NUKE) && k.killer_class == 0, "3: streak 3, hideFeed, no DEATH_INFO for others");
        CHECK(n_p && NET_KILL_STREAK(n.fists) == 0 && (n.fists & NET_KILL_NUKE) && (n.fists & NET_KILL_FISTS), "3: a Nuke kill (stk 0), fists");
    }
    clear();

    // "ch": a team line with the sender's account; a server line stays a v3 chat
    feed(&link, "ch", (KrunkerValue[]) {KRUNKER_V_NUM(1), KRUNKER_V_STR("Ace"), KRUNKER_V_STR("push b"), KRUNKER_V_NUM(0), KRUNKER_V_NIL,
                                        KRUNKER_V_NIL, KRUNKER_V_NUM(4242)}, 7);
    feed(&link, "ch", (KrunkerValue[]) {KRUNKER_V_NUM(-1), KRUNKER_V_NIL, KRUNKER_V_STR("server says"), KRUNKER_V_NUM(2), KRUNKER_V_STR("#00ff00")}, 5);
    {
        READ_IN(NetChatV7Packet, v, CHAT_V7, 0);
        READ_IN(NetChatPacket, c, CHAT, 0);
        CHECK(v_p && v.channel == CHAT_CHANNEL_TEAM && v.account_id == 4242 && !strcmp(v.chat.text, "push b"), "ch: team tab + account");
        CHECK(c_p && c.kind == NET_CHAT_SERVER && !strcmp(c.text, "server says"), "ch: a server line");
    }
    clear();

    // "rg" class change: the kill card's class follows
    feed(&link, "rg", (KrunkerValue[]) {KRUNKER_V_NUM(7), KRUNKER_V_BOOL(1),
                                        KRUNKER_V_ARRAY(((KrunkerValue[]) {KRUNKER_V_NUM(-1), KRUNKER_V_NUM(-1), KRUNKER_V_NUM(-1), KRUNKER_V_NUM(-1),
                                                                           KRUNKER_V_NUM(-1), KRUNKER_V_NUM(-1), KRUNKER_V_NUM(-1), KRUNKER_V_NUM(-1),
                                                                           KRUNKER_V_NUM(-1), KRUNKER_V_NUM(-1), KRUNKER_V_NUM(-1), KRUNKER_V_NUM(-1),
                                                                           KRUNKER_V_NUM(-1), KRUNKER_V_NUM(-1), KRUNKER_V_NUM(-1), KRUNKER_V_NUM(0)}), 16)}, 3);
    clear();
    feed(&link, "3", (KrunkerValue[]) {KRUNKER_V_NUM(9), KRUNKER_V_NUM(2), KRUNKER_V_NUM(7), KRUNKER_V_NIL, KRUNKER_V_NIL, KRUNKER_V_BOOL(0)}, 6);
    {
        READ_IN(NetKillPacket, k, KILL, 0);
        CHECK(k_p && k.killer_class == 0 && !find_in(NET_PACKET_DEATH_INFO, 0), "rg: the killer's new class (%d); no stats, no DEATH_INFO",
              k.killer_class);
    }
    clear();

    // "uh": the host moves to us, then to 7 (by its name, not the alias); silent: no line
    feed(&link, "uh", (KrunkerValue[]) {KRUNKER_V_STR("sockA"), KRUNKER_V_BOOL(0)}, 2);
    {
        READ_IN(NetPlayerInfoPacket, a, PLAYER_INFO, 0);
        READ_IN(NetPlayerInfoPacket, c, PLAYER_INFO, 1);
        uint16_t len = 0;
        const uint8_t *body = ext_body(0, NET_EXT_KN_OFFICIAL_CHAT_I18N, &len);
        CHECK(a_p && c_p && (a.flags & NET_PLAYER_INFO_HOST) == (a.uid == 9 ? NET_PLAYER_INFO_HOST : 0) &&
              (c.flags & NET_PLAYER_INFO_HOST) == (c.uid == 9 ? NET_PLAYER_INFO_HOST : 0), "uh: HOST moves to us");
        CHECK(body && len > sizeof(NetOfficialChatI18nHeader) + 1 && !strcmp((const char *) body + sizeof(NetOfficialChatI18nHeader) + 1,
                                                                             "host.transfer.you"), "uh: You are now Host!");
    }
    clear();
    feed(&link, "uh", (KrunkerValue[]) {KRUNKER_V_STR("sockB")}, 1);
    {
        uint16_t len = 0;
        const uint8_t *body = ext_body(0, NET_EXT_KN_OFFICIAL_CHAT_I18N, &len);
        const char *key = body ? (const char *) body + sizeof(NetOfficialChatI18nHeader) + 1 : "";
        CHECK(body && !strcmp(key, "host.transfer") && !strcmp(key + strlen(key) + 1, "acct"), "uh: Host Transfered to acct");
    }
    clear();
    feed(&link, "uh", (KrunkerValue[]) {KRUNKER_V_STR("sockA"), KRUNKER_V_BOOL(1)}, 2);
    CHECK(find_in(NET_PACKET_PLAYER_INFO, 0) && !find_in(NET_PACKET_EXT, 0), "uh silent: no line");
    clear();
}

// Combat visuals / sounds (fx): "9" "s" "so" "sso" "pr" "pre" "ex" "k"[11] "pk" "upk" "bh" "n" "bbm"
// "asnd" "rsnd" "fh" "scr" "is" "camb" "shk" "ana"
static void test_fx(void) {
    KrunkerLink link;
    const KrunkerLinkIO io = {.emit = cap_emit, .send = cap_send, .event = cap_event, .cheat = fake_cheat};
    krunker_link_init(&link, &io, KRUNKER_AHKEY);
    feed(&link, "io-init", (KrunkerValue[]) {KRUNKER_V_STR("sockA")}, 1);
    KrunkerValue add[2 * 53];
    for (int i = 0; i < 2 * 53; i++) add[i] = KRUNKER_V_NIL;
    add[0] = KRUNKER_V_STR("sockA"); add[1] = KRUNKER_V_NUM(9); add[8] = KRUNKER_V_NUM(100);
    add[53] = KRUNKER_V_STR("sockB"); add[54] = KRUNKER_V_NUM(7); add[61] = KRUNKER_V_NUM(100);
    feed(&link, "0", (KrunkerValue[]) {KRUNKER_V_ARRAY(add, 2 * 53)}, 1);
    clear();

    // "9": a ray of 7 to (10, 2, -3) hitting a wall (impact yaw / pitch), trail 1
    const KrunkerValue ray[] = {KRUNKER_V_NUM(7), KRUNKER_V_NUM(10), KRUNKER_V_NUM(2), KRUNKER_V_NUM(-3), KRUNKER_V_NUM(1.5), KRUNKER_V_NUM(0.25),
                                KRUNKER_V_NUM(1), KRUNKER_V_NUM(0), KRUNKER_V_NUM(0), KRUNKER_V_NUM(11), KRUNKER_V_NUM(0)};
    feed(&link, "9", (KrunkerValue[]) {KRUNKER_V_ARRAY(ray, 11)}, 1);
    const KrunkerValue ray2[] = {KRUNKER_V_NUM(7), KRUNKER_V_NUM(1), KRUNKER_V_NUM(2), KRUNKER_V_NUM(3), KRUNKER_V_NIL, KRUNKER_V_NIL, KRUNKER_V_NUM(0)};
    feed(&link, "9", (KrunkerValue[]) {KRUNKER_V_ARRAY(ray2, 7)}, 1);
    {
        READ_IN(NetShotPacket, a, SHOT, 0);
        READ_IN(NetShotPacket, b, SHOT, 1);
        CHECK(a_p && a.uid == 7 && a.hit[0] == 10 && a.hit[2] == -3 && a.has_normal && a.normal_yaw == 1.5f && a.normal_pitch == 0.25f &&
              a.trail == 1 && !a.first && a.weapon_index == -1, "9: tracer + impact, no gun sound of its own");
        CHECK(b_p && !b.has_normal && b.hit[1] == 2, "9: a ray into the air has no impact");
    }
    clear();

    // "s": sounds[5] ("weapon_1") at sid 7, volume 0.85, rate 1.05; a name; forcePos; an asset; the long name skipped
    feed(&link, "s", (KrunkerValue[]) {KRUNKER_V_NUM(5), KRUNKER_V_NUM(7), KRUNKER_V_NUM(1), KRUNKER_V_NUM(2), KRUNKER_V_NUM(3), KRUNKER_V_NUM(0.85),
                                       KRUNKER_V_NUM(1.05)}, 7);
    feed(&link, "s", (KrunkerValue[]) {KRUNKER_V_STR("spray"), KRUNKER_V_NUM(7), KRUNKER_V_NUM(0), KRUNKER_V_NUM(0), KRUNKER_V_NUM(0), KRUNKER_V_NIL,
                                       KRUNKER_V_NIL, KRUNKER_V_NIL, KRUNKER_V_NIL, KRUNKER_V_BOOL(1)}, 10);
    feed(&link, "s", (KrunkerValue[]) {KRUNKER_V_NUM(9001), KRUNKER_V_NUM(7), KRUNKER_V_NUM(0), KRUNKER_V_NUM(0), KRUNKER_V_NUM(0), KRUNKER_V_NUM(1),
                                       KRUNKER_V_NUM(1), KRUNKER_V_NIL, KRUNKER_V_NIL, KRUNKER_V_NIL, KRUNKER_V_BOOL(1)}, 11);
    int long_index = -1;
    for (int i = 0; i < krunker_sound_count(); i++) {
        if (strlen(krunker_sound_name(i)) >= NET_SOUND_NAME_SIZE) long_index = i;
    }
    feed(&link, "s", (KrunkerValue[]) {KRUNKER_V_NUM(long_index), KRUNKER_V_NUM(7)}, 2);
    feed(&link, "s", (KrunkerValue[]) {KRUNKER_V_NUM(100000), KRUNKER_V_NUM(7)}, 2);
    {
        READ_IN(NetSoundPacket, a, SOUND, 0);
        READ_IN(NetSoundPacket, b, SOUND, 1);
        READ_IN(NetSoundPacket, c, SOUND, 2);
        CHECK(krunker_sound_count() == 399 && !strcmp(krunker_sound_name(0), "spray") && !strcmp(krunker_sound_name(5), "weapon_1"),
              "the official sound table (%d)", krunker_sound_count());
        CHECK(a_p && !strcmp(a.name, "weapon_1") && a.uid == 7 && a.position[2] == 3 && fabsf(a.volume - 0.85f) < 1e-6f &&
              fabsf(a.rate - 1.05f) < 1e-6f, "s: sounds[5] at sid 7 (%s)", a.name);
        CHECK(b_p && !strcmp(b.name, "spray") && b.uid == -1 && b.volume == 1.0f && b.rate == 1.0f, "s: a name, forcePos, vol / rate || 1");
        CHECK(c_p && !strcmp(c.name, "asset:9001"), "s: isAsset (%s)", c.name);
        CHECK(long_index >= 0 && !find_in(NET_PACKET_SOUND, 3), "s: a name past NET_SOUND_NAME_SIZE and an unknown index are dropped");
    }
    clear();

    // "so" 2D / 3D, "sso"
    feed(&link, "so", (KrunkerValue[]) {KRUNKER_V_STR("cheer_0"), KRUNKER_V_NUM(0.5), KRUNKER_V_NUM(1)}, 3);
    feed(&link, "so", (KrunkerValue[]) {KRUNKER_V_NUM(1), KRUNKER_V_NUM(0.4), KRUNKER_V_NUM(0), KRUNKER_V_NUM(4), KRUNKER_V_NUM(5), KRUNKER_V_NUM(6),
                                        KRUNKER_V_NUM(2)}, 7);
    feed(&link, "sso", (KrunkerValue[]) {KRUNKER_V_NUM(1)}, 1);
    {
        READ_IN(NetSoundPacket, a, SOUND, 0);
        READ_IN(NetSoundPacket, b, SOUND, 1);
        READ_IN(NetStopSoundPacket, st, STOP_SOUND, 0);
        CHECK(a_p && !strcmp(a.name, "cheer_0") && a.uid == -2 && fabsf(a.volume - 0.5f) < 1e-6f, "so: mode 1 is 2D");
        CHECK(b_p && !strcmp(b.name, "taunt_0") && b.uid == -1 && b.position[1] == 5 && b.rate == 2.0f, "so: 3D at x y z (%s)", b.name);
        CHECK(st_p && !strcmp(st.name, "taunt_0") && !st.asset, "sso: by index");
    }
    clear();

    // "pr": a rocket (weapon 9) of 7, server sid 300 (past our 64 slots); a thrown knife (stored 0); "pre" for both
    feed(&link, "pr", (KrunkerValue[]) {KRUNKER_V_NUM(1), KRUNKER_V_NUM(2), KRUNKER_V_NUM(3), KRUNKER_V_NUM(M_PI), KRUNKER_V_NUM(0), KRUNKER_V_NUM(9),
                                        KRUNKER_V_NUM(300), KRUNKER_V_NIL, KRUNKER_V_NIL, KRUNKER_V_NUM(7)}, 10);
    feed(&link, "pr", (KrunkerValue[]) {KRUNKER_V_NUM(0), KRUNKER_V_NUM(0), KRUNKER_V_NUM(0), KRUNKER_V_NUM(M_PI / 2), KRUNKER_V_NUM(0), KRUNKER_V_NUM(0),
                                        KRUNKER_V_NUM(301), KRUNKER_V_NUM(1), KRUNKER_V_NIL, KRUNKER_V_NUM(7)}, 10);
    feed(&link, "pr", (KrunkerValue[]) {KRUNKER_V_NUM(0), KRUNKER_V_NUM(0), KRUNKER_V_NUM(0), KRUNKER_V_NUM(0), KRUNKER_V_NUM(0), KRUNKER_V_NUM(2),
                                        KRUNKER_V_NUM(302), KRUNKER_V_NUM(1)}, 8); // a bombardment bomb: no visual of ours
    {
        READ_IN(NetProjectilePacket, r, PROJECTILE, 0);
        READ_IN(NetProjectilePacket, k, PROJECTILE, 1);
        CHECK(r_p && r.weapon_index == 9 && r.owner_uid == 7 && r.id >= 0 && r.id < GAME_MAX_PROJECTILES && r.position[2] == 3 &&
              fabsf(r.velocity[2] - 0.6f) < 1e-5f && fabsf(r.velocity[0]) < 1e-5f && r.gravity == 0.0001f && r.range == 1200.0f,
              "pr: rocket, yaw PI flies +z at its spd (v %g %g %g)", r.velocity[0], r.velocity[1], r.velocity[2]);
        CHECK(k_p && k.weapon_index == WEAPON_ID_KNIFE && k.id != r.id && fabsf(k.velocity[0] + 0.38f) < 1e-5f && k.range == 1500.0f && !k.ai_kind,
              "pr: stored[0] the thrown knife (v %g %g %g)", k.velocity[0], k.velocity[1], k.velocity[2]);
        CHECK(!find_in(NET_PACKET_PROJECTILE, 2), "pr: stored[2] dropped");
        feed(&link, "pre", (KrunkerValue[]) {KRUNKER_V_NUM(300), KRUNKER_V_NUM(0), KRUNKER_V_NUM(5), KRUNKER_V_NUM(6), KRUNKER_V_NUM(7), KRUNKER_V_NUM(1)}, 6);
        feed(&link, "pre", (KrunkerValue[]) {KRUNKER_V_NUM(4), KRUNKER_V_NUM(1), KRUNKER_V_NUM(0), KRUNKER_V_NUM(0), KRUNKER_V_NUM(0), KRUNKER_V_NUM(0)}, 6);
        feed(&link, "pre", (KrunkerValue[]) {KRUNKER_V_NUM(999), KRUNKER_V_NUM(0), KRUNKER_V_NUM(1), KRUNKER_V_NUM(1), KRUNKER_V_NUM(1), KRUNKER_V_NUM(1)}, 6);
        READ_IN(NetProjectileEndPacket, e, PROJECTILE_END, 0);
        READ_IN(NetProjectileEndPacket, o, PROJECTILE_END, 1);
        READ_IN(NetProjectileEndPacket, u, PROJECTILE_END, 2);
        CHECK(e_p && e.id == r.id && e.impact && e.position[1] == 6, "pre: the rocket's slot, impact");
        CHECK(o_p && o.id == NET_PROJECTILE_END_OWN - 4 && !o.impact, "pre: our own prediction by shotCntP 4 (%d)", o.id);
        CHECK(u_p && u.id == -1 && u.impact, "pre: an unknown sid still shows its impact");
        clear();
        // the freed slot is reused; a 65th live projectile reuses one round-robin
        for (int i = 0; i < 70; i++)
            feed(&link, "pr", (KrunkerValue[]) {KRUNKER_V_NUM(0), KRUNKER_V_NUM(0), KRUNKER_V_NUM(0), KRUNKER_V_NUM(0), KRUNKER_V_NUM(0), KRUNKER_V_NUM(9),
                                                KRUNKER_V_NUM(1000 + i), KRUNKER_V_NIL, KRUNKER_V_NIL, KRUNKER_V_NUM(7)}, 10);
        int ok = 1;
        for (int i = 0; i < 70; i++) {
            const NetPacket *p = find_in(NET_PACKET_PROJECTILE, i);
            NetProjectilePacket pp;
            if (!p) { ok = 0; break; }
            memcpy(&pp, p->payload, sizeof(pp));
            ok &= pp.id >= 0 && pp.id < GAME_MAX_PROJECTILES;
        }
        CHECK(ok, "pr: every projectile gets one of our %d slots", GAME_MAX_PROJECTILES);
    }
    clear();

    // "ex"
    feed(&link, "ex", (KrunkerValue[]) {KRUNKER_V_NUM(10), KRUNKER_V_NUM(20), KRUNKER_V_NUM(30), KRUNKER_V_NUM(40), KRUNKER_V_NUM(3), KRUNKER_V_NUM(2)}, 6);
    {
        READ_IN(NetExplosionPacket, x, EXPLOSION, 0);
        CHECK(x_p && x.position[0] == 10 && x.position[2] == 30 && x.radius == 40, "ex: position, radius");
    }
    clear();

    // "k" [11]: 7 hooks on (1, 2, 3), stays hooked (no repeat), lets go
    KrunkerValue k[13], hook[] = {KRUNKER_V_NUM(1), KRUNKER_V_NUM(2), KRUNKER_V_NUM(3)};
    for (int i = 0; i < 13; i++) k[i] = KRUNKER_V_NUM(0);
    k[0] = KRUNKER_V_NUM(7);
    k[11] = KRUNKER_V_ARRAY(hook, 3);
    feed(&link, "k", (KrunkerValue[]) {KRUNKER_V_ARRAY(k, 13), KRUNKER_V_NUM(10), KRUNKER_V_NUM(0)}, 3);
    feed(&link, "k", (KrunkerValue[]) {KRUNKER_V_ARRAY(k, 13), KRUNKER_V_NUM(10), KRUNKER_V_NUM(0)}, 3);
    k[11] = KRUNKER_V_NIL;
    feed(&link, "k", (KrunkerValue[]) {KRUNKER_V_ARRAY(k, 13), KRUNKER_V_NUM(10), KRUNKER_V_NUM(0)}, 3);
    {
        READ_IN(NetGrapplePacket, g0, GRAPPLE, 0);
        READ_IN(NetGrapplePacket, g1, GRAPPLE, 1);
        CHECK(g0_p && g0.uid == 7 && g0.active && g0.point[1] == 2 && g1_p && !g1.active && !find_in(NET_PACKET_GRAPPLE, 2),
              "k[11]: the rope on a change only");
    }
    clear();

    // the rest
    feed(&link, "pk", (KrunkerValue[]) {KRUNKER_V_NUM(2), KRUNKER_V_NUM(3)}, 2);
    const KrunkerValue perks[] = {KRUNKER_V_NUM(0), KRUNKER_V_NUM(4)};
    feed(&link, "upk", (KrunkerValue[]) {KRUNKER_V_NUM(9), KRUNKER_V_ARRAY(perks, 2)}, 2);
    feed(&link, "bh", (KrunkerValue[]) {KRUNKER_V_NUM(7), KRUNKER_V_NUM(2.5)}, 2);
    feed(&link, "n", (KrunkerValue[]) {KRUNKER_V_NUM(1)}, 1);
    feed(&link, "bbm", (KrunkerValue[]) {KRUNKER_V_NUM(0)}, 1);
    feed(&link, "bbm", (KrunkerValue[]) {KRUNKER_V_NUM(1)}, 1);
    const KrunkerValue snd[] = {KRUNKER_V_STR("loop"), KRUNKER_V_BOOL(1), KRUNKER_V_STR("vol"), KRUNKER_V_NUM(0.3)};
    feed(&link, "asnd", (KrunkerValue[]) {KRUNKER_V_NUM(7), KRUNKER_V_STR("staunt_1"), KRUNKER_V_MAP(snd, 2)}, 3);
    feed(&link, "asnd", (KrunkerValue[]) {KRUNKER_V_NUM(7), KRUNKER_V_STR("jug_0"), KRUNKER_V_MAP(snd, 2)}, 3);
    feed(&link, "rsnd", (KrunkerValue[]) {KRUNKER_V_NUM(7), KRUNKER_V_NIL, KRUNKER_V_BOOL(0)}, 3);
    feed(&link, "fh", (KrunkerValue[]) {KRUNKER_V_BOOL(1)}, 1);
    feed(&link, "scr", (KrunkerValue[]) {KRUNKER_V_NUM(1)}, 1);
    feed(&link, "is", (KrunkerValue[]) {KRUNKER_V_STR("nuke_0"), KRUNKER_V_STR("img/x")}, 2);
    feed(&link, "camb", (KrunkerValue[]) {KRUNKER_V_STR("3")}, 1);
    feed(&link, "camb", (KrunkerValue[]) {KRUNKER_V_STR("77"), KRUNKER_V_BOOL(1)}, 2);
    feed(&link, "shk", (KrunkerValue[]) {KRUNKER_V_NUM(0.4)}, 1);
    feed(&link, "ana", (KrunkerValue[]) {KRUNKER_V_NUM(12), KRUNKER_V_NUM(5), KRUNKER_V_BOOL(0), KRUNKER_V_BOOL(1)}, 4);
    feed(&link, "ana", (KrunkerValue[]) {KRUNKER_V_NUM(3), KRUNKER_V_STR("Dance"), KRUNKER_V_BOOL(1)}, 3);
    {
        READ_IN(NetEffectPacket, ef, EFFECT, 0);
        READ_IN(NetPerksPacket, pk, PERKS, 0);
        READ_IN(NetModeCounterPacket, bh, MODE_COUNTER, 0);
        READ_IN(NetNukePacket, nk, NUKE, 0);
        READ_IN(NetSoundPacket, ad, SOUND, 0);
        READ_IN(NetSoundPacket, is, SOUND, 1);
        READ_IN(NetPlayerSoundPacket, as, PLAYER_SOUND, 0);
        READ_IN(NetPlayerSoundPacket, rs, PLAYER_SOUND, 1);
        READ_IN(NetCheatPacket, fh, CHEAT, 0);
        READ_IN(NetJumpScarePacket, sc, JUMP_SCARE, 0);
        READ_IN(NetAmbientPacket, am, AMBIENT_CHANGE, 0);
        READ_IN(NetObjectAnimPacket, oa, OBJECT_ANIM, 0);
        READ_IN(NetAIAnimPacket, aa, AI_ANIM, 0);
        uint16_t sl = 0;
        const uint8_t *shk = NULL;
        for (int i = 0; i < 4 && !shk; i++) shk = ext_body(i, NET_EXT_KN_OFFICIAL_SHAKE, &sl);
        NetOfficialShakePacket shake = {0};
        if (shk && sl == sizeof(shake)) memcpy(&shake, shk, sizeof(shake));
        CHECK(ef_p && ef.effect == 2 && ef.count == 3, "pk");
        CHECK(pk_p && pk.uid == 9 && pk.count == 2 && pk.perks[1] == 4, "upk");
        CHECK(bh_p && bh.kind == NET_MODE_COUNTER_HEAD && bh.uid == 7 && bh.value == 2.5f, "bh");
        CHECK(nk_p && nk.state == 1, "n");
        CHECK(ad_p && !strcmp(ad.name, "adrop_0") && ad.uid == -2 && !find_in(NET_PACKET_SOUND, 2) && is_p && !strcmp(is.name, "nuke_0") &&
              is.uid == -2, "bbm 0 -> adrop_0 (1: nothing), is -> its 2D sound");
        CHECK(as_p && as.add && as.loop && as.uid == 7 && !strcmp(as.name, "staunt_1") && fabsf(as.volume - 0.3f) < 1e-6f && as.rate == 1.0f,
              "asnd");
        CHECK(rs_p && !rs.add && rs.force && rs.uid == 7 && !find_in(NET_PACKET_PLAYER_SOUND, 2), "rsnd without a name: all; asnd jug_0 skipped");
        CHECK(fh_p && fh.flag == NET_CHEAT_FORCE_HIDE && fh.on, "fh");
        CHECK(sc_p && sc.index == 1, "scr");
        CHECK(am_p && am.ambient == 3 && !find_in(NET_PACKET_AMBIENT_CHANGE, 1), "camb parseInt, a custom one dropped");
        CHECK(shk && fabsf(shake.amount - 0.4f) < 1e-6f, "shk -> NET_EXT_KN_OFFICIAL_SHAKE");
        CHECK(oa_p && oa.uid == 12 && oa.is_object && !oa.raw && !strcmp(oa.clip, "Shoot"), "ana: a map object, clipNames[5]");
        CHECK(aa_p && aa.sid == 3 && aa.raw && !strcmp(aa.clip, "Dance"), "ana: an AI's raw clip");
        CHECK(link.unknown_in == 0, "every fx packet translated (%u unknown)", link.unknown_in);
    }
    clear();
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
    // Krunker's whole official list passes (krunker_maps.h): 42 (Frontier, the index the user reported), 43
    // (Bastion) and the highest index; and an index past the table too (the transport fetches
    // GET <gapi>/maps/<index>, as the live client does)
    const int idx[] = {42, 43, KRUNKER_OFFICIAL_MAP_COUNT - 1, KRUNKER_OFFICIAL_MAP_COUNT + 31};
    for (size_t k = 0; k < sizeof(idx) / sizeof(idx[0]); k++) {
        feed(&link, "init", (KrunkerValue[]) {KRUNKER_V_NUM(idx[k]), KRUNKER_V_NUM(1)}, 2);
        p = find_in(NET_PACKET_INIT, 0);
        if (p) memcpy(&init, p->payload, sizeof(init));
        CHECK(p && init.map_index == idx[k] && init.mode_index == 1 && !g_events[KRUNKER_LINK_EV_UNSUPPORTED],
              "init map %d emitted without io.map", idx[k]);
        clear();
    }
    CHECK(!strcmp(KRUNKER_MAPS_API_URL, "https://gapi.svc.krunker.io") && KRUNKER_OFFICIAL_MAP_COUNT >= 46, "maps API / table");
    // an init naming no map at all is the only refusal
    feed(&link, "init", (KrunkerValue[]) {KRUNKER_V_NIL, KRUNKER_V_NUM(0)}, 2);
    CHECK(!find_in(NET_PACKET_INIT, 0) && g_events[KRUNKER_LINK_EV_UNSUPPORTED] == 1, "init without a map refused");
    CHECK(link.unsupported_map == -1, "no map index (%d)", link.unsupported_map);
    clear();

    // with io.map the transport gets the init (and emits INIT itself once the map is loaded)
    KrunkerLinkIO io_map = io;
    io_map.map = cap_map;
    KrunkerLink mlink;
    krunker_link_init(&mlink, &io_map, KRUNKER_AHKEY);
    feed(&mlink, "init", (KrunkerValue[]) {KRUNKER_V_NUM(43), KRUNKER_V_NUM(23), KRUNKER_V_NUM(0)}, 3);
    CHECK(g_init_count == 1 && g_inits[0].map_index == 43 && g_inits[0].mode_index == 23 && g_inits[0].init.map_index == 43 &&
          g_inits[0].init.mode_index == 23 && !g_inits[0].custom && !g_inits[0].custom_id[0] && !find_in(NET_PACKET_INIT, 0),
          "io.map: init 43 handed over, no INIT emitted by the link");
    // a custom game: arg 9 {data: "<map JSON>", id, creator}
    KrunkerValue cust_kv[] = {KRUNKER_V_STR("data"), KRUNKER_V_STR("{\"name\":\"Custom\",\"objects\":[{\"p\":[0,0,0],\"s\":[1,1,1]}]}"),
                              KRUNKER_V_STR("id"), KRUNKER_V_NUM(4242), KRUNKER_V_STR("creator"), KRUNKER_V_STR("someone")};
    KrunkerValue cust_args[10];
    for (int i = 0; i < 10; i++) cust_args[i] = KRUNKER_V_NIL;
    cust_args[0] = KRUNKER_V_NUM(0);
    cust_args[1] = KRUNKER_V_NUM(0);
    cust_args[KRUNKER_MAPS_INIT_CUSTOM_ARG] = KRUNKER_V_MAP(cust_kv, 3);
    feed(&mlink, "init", cust_args, 10);
    CHECK(g_init_count == 2 && g_inits[1].custom && !strcmp(g_inits[1].custom_id, "4242") &&
          !strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(g_inits[1].custom, "name")), "Custom"),
          "io.map: a custom game's inline map (string data) and id");
    // ... data as an object, id as a string
    KrunkerValue obj_kv[] = {KRUNKER_V_STR("name"), KRUNKER_V_STR("Obj"), KRUNKER_V_STR("objects"), KRUNKER_V_ARRAY(NULL, 0)};
    KrunkerValue cust2_kv[] = {KRUNKER_V_STR("data"), KRUNKER_V_MAP(obj_kv, 2), KRUNKER_V_STR("id"), KRUNKER_V_STR("m77")};
    cust_args[KRUNKER_MAPS_INIT_CUSTOM_ARG] = KRUNKER_V_MAP(cust2_kv, 2);
    feed(&mlink, "init", cust_args, 10);
    CHECK(g_init_count == 3 && g_inits[2].custom && !strcmp(g_inits[2].custom_id, "m77") &&
          cJSON_IsArray(cJSON_GetObjectItem(g_inits[2].custom, "objects")), "io.map: object data, string id");
    for (int i = 0; i < g_init_count; i++) cJSON_Delete(g_inits[i].custom);
    g_init_count = 0;
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
    CHECK(link.last_ack == 41 && link.sync_pos[0] == 5.0f && link.syncs >= 1, "\"l\" kept for the sync trace: ack %d, server x %g", link.last_ack,
          (double) link.sync_pos[0]);
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
    p = find_in(NET_PACKET_CHAT_V7, 0); // a player line: our v7 chat (channel -1 = every tab)
    CHECK(p && p->length == sizeof(NetChatV7Packet) && !strcmp(((const NetChatV7Packet *) p->payload)->chat.text, "hello there") &&
          !strcmp(((const NetChatV7Packet *) p->payload)->chat.name, "Other") && ((const NetChatV7Packet *) p->payload)->chat.kind == NET_CHAT_PLAYER &&
          ((const NetChatV7Packet *) p->payload)->channel == CHAT_CHANNEL_ALL && !find_in(NET_PACKET_CHAT, 0), "CHAT");
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

    // "a" ACCOUNT_RESPONSE: the account signed in on the socket (name, level from player_score) and its
    // K.P.D fields (player_role 3 = isDeputy, the Helpful Reports)
    const KrunkerValue acc_data[] = {KRUNKER_V_STR("player_score"), KRUNKER_V_NUM(40000), KRUNKER_V_STR("player_kills"), KRUNKER_V_NUM(3),
                                     KRUNKER_V_STR("player_role"), KRUNKER_V_NUM(3), KRUNKER_V_STR("jobrating"), KRUNKER_V_NUM(9),
                                     KRUNKER_V_STR("jobratingpositive"), KRUNKER_V_NUM(7)};
    feed(&link, "a", (KrunkerValue[]) {KRUNKER_V_NIL, KRUNKER_V_NUM(123), KRUNKER_V_STR("KioPlayer"), KRUNKER_V_MAP(acc_data, 5), KRUNKER_V_STR("legacy")}, 5);
    CHECK(g_events[KRUNKER_LINK_EV_ACCOUNT] == 1 && !strcmp(link.account_name, "KioPlayer") && link.account_level == 6 && g_out_count == 0,
          "a: account %s level %d", link.account_name, link.account_level);
    CHECK(link.account_role == 3 && link.account_dev == 0 && link.account_job_rating == 9 && link.account_job_rating_pos == 7,
          "a: role %d dev %d reports %d/%d", link.account_role, link.account_dev, link.account_job_rating_pos, link.account_job_rating);
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
    CHECK(link.q_sent == 1 && link.q_frames == 3 && link.last_isn_sent == 102, "sync trace counters: %u q, %u frames, last isn %d", link.q_sent,
          link.q_frames, link.last_isn_sent);
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
    // lock (window.locked): v10 builds no input frame while it holds (gameLoop `!window.locked`), so the
    // client is told to sample none (inputs paused); a window built before the lock goes out as built
    CHECK(!krunker_link_inputs_paused(&link), "inputs flow before the lock");
    feed(&link, "lock", (KrunkerValue[]) {KRUNKER_V_BOOL(1)}, 1);
    p = find_in(NET_PACKET_LOCK, 0);
    PAYLOAD(NetLockPacket, plk, p);
    CHECK(p && plk.kind == NET_LOCK_PLAYER && plk.locked == 1 && link.locked, "lock: LOCK player");
    CHECK(krunker_link_inputs_paused(&link), "lock: input frames paused");
    for (int i = 0; i < 3; i++) { in[i].seq = 306 + i; in[i].flags = NET_INPUT_SHOOT | NET_INPUT_CROUCH; }
    memcpy(batch + 1, in, sizeof(in));
    krunker_link_send_ours(&link, NET_PACKET_INPUT_BATCH, batch, sizeof(batch));
    const KrunkerFrame *lq = g_out_count ? &g_out[g_out_count - 1] : NULL;
    changes = lq ? krunker_frame_arg(lq, 5) : NULL;
    CHECK(lq && fnum(lq, 1) == 306 && krunker_is(changes, KRUNKER_MAP) && !krunker_get(changes, "0-4") && // moveDir 1 unchanged, not -1
          krunker_num(krunker_get(changes, "0-5"), 9) == 1 && krunker_num(krunker_get(changes, "0-9"), 9) == 1,
          "a window built before the lock reached us goes out as built (no zeroed frames)");
    feed(&link, "lock", (KrunkerValue[]) {KRUNKER_V_BOOL(0)}, 1);
    CHECK(!link.locked && !krunker_link_inputs_paused(&link), "lock false: input frames again");
    feed(&link, "lock", (KrunkerValue[]) {KRUNKER_V_BOOL(1)}, 1);
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
    CHECK(p && sub == NET_EXT_PLAYER_NAME && nm.uid == 7 && !strcmp(nm.name, "al") && !find_in(NET_PACKET_EXT, 1) &&
          !g_events[KRUNKER_LINK_EV_CLOSE], "00 keepOpen: one rename (the alias is shown), no close");
    clear();
    feed(&link, "00", (KrunkerValue[]) {KRUNKER_V_ARRAY(NULL, 0), KRUNKER_V_BOOL(0)}, 2);
    CHECK(g_events[KRUNKER_LINK_EV_CLOSE] == 1, "00 keepOpen false: close");
    clear();
    feed(&link, "00", (KrunkerValue[]) {KRUNKER_V_ARRAY(NULL, 0)}, 1);
    CHECK(g_events[KRUNKER_LINK_EV_CLOSE] == 1, "00 without keepOpen: close");
    clear();
    // init: our client resets game.moveLock and its players, so do the link's locks
    feed(&link, "lm", (KrunkerValue[]) {KRUNKER_V_NUM(0), KRUNKER_V_NUM(0), KRUNKER_V_NUM(0), KRUNKER_V_NUM(0)}, 4);
    CHECK(link.my_sid == 9 && !krunker_link_inputs_paused(&link), "our player is in: inputs flow");
    feed(&link, "init", (KrunkerValue[]) {KRUNKER_V_NUM(10), KRUNKER_V_NUM(0), KRUNKER_V_NUM(0)}, 3);
    CHECK(link.move_lock == 0 && link.noclip == 0, "init resets the locks");
    // a new round's init drops our player (v10 initGame: you = null) until our "0": the last round's
    // player builds no input frame for the new one meanwhile
    CHECK(krunker_link_inputs_paused(&link), "init: no input frames until our player is back");
    {
        KrunkerValue other[53];
        for (int i = 0; i < 53; i++) other[i] = KRUNKER_V_NIL;
        other[0] = KRUNKER_V_STR("sockB"); other[1] = KRUNKER_V_NUM(7);
        feed(&link, "0", (KrunkerValue[]) {KRUNKER_V_ARRAY(other, 53)}, 1);
        CHECK(krunker_link_inputs_paused(&link), "someone else's \"0\" does not bring our player back");
        other[0] = KRUNKER_V_STR("sockA"); other[1] = KRUNKER_V_NUM(9);
        feed(&link, "0", (KrunkerValue[]) {KRUNKER_V_ARRAY(other, 53)}, 1);
        CHECK(!krunker_link_inputs_paused(&link), "our \"0\": input frames again");
    }
    clear();
    {
        // a fresh connection: no player yet, nothing paused (our client has no player to sample for)
        KrunkerLink fresh;
        krunker_link_init(&fresh, NULL, 3);
        feed(&fresh, "init", (KrunkerValue[]) {KRUNKER_V_NUM(10), KRUNKER_V_NUM(0), KRUNKER_V_NUM(0)}, 3);
        CHECK(!krunker_link_inputs_paused(&fresh) && fresh.last_ack == -1 && fresh.last_isn_sent == -1, "first init: not paused, no ack yet");
    }
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
    { // an officer's list: "kpd" [1, "", [rows]] -> KPD_RESPONSE code 1 with the rows as JSON (one page)
        const KrunkerValue row0[] = {KRUNKER_V_STR("can"), KRUNKER_V_STR("rep"), KRUNKER_V_STR("con"), KRUNKER_V_STR("sus"),
                                     KRUNKER_V_STR("jop"), KRUNKER_V_NUM(2)};
        const KrunkerValue rows[] = {KRUNKER_V_MAP(row0, 3)};
        feed(&link, "kpd", (KrunkerValue[]) {KRUNKER_V_NUM(1), KRUNKER_V_STR(""), KRUNKER_V_ARRAY(rows, 1)}, 3);
        const NetPacket *e = find_in(NET_PACKET_EXT, 0);
        uint16_t sub = 0, bl = 0;
        const uint8_t *b = NULL;
        NetKpdResponsePacket r;
        memset(&r, 0, sizeof(r));
        char json[512] = "";
        if (e && net_ext_unpack(e->payload, e->length, &sub, &b, &bl) && bl >= sizeof(r)) {
            memcpy(&r, b, sizeof(r));
            snprintf(json, sizeof(json), "%.*s", (int) (bl - sizeof(r)), (const char *) b + sizeof(r));
        }
        CHECK(sub == NET_EXT_KPD_RESPONSE && r.code == 1 && r.status == NET_KPD_REPORTED && !r.name[0] && find_in(NET_PACKET_EXT, 1) == NULL &&
                  !strcmp(json, "[{\"can\":\"rep\",\"con\":\"sus\",\"jop\":2}]"),
              "kpd list 1 -> one KPD_RESPONSE with the rows (%s)", json);
        clear();
        // a long list: pages of whole rows, each "page/pages", joined they are every row
        enum { ROWS = 40 };
        KrunkerValue many_kv[ROWS][4], many[ROWS];
        char names[ROWS][24];
        for (int i = 0; i < ROWS; i++) {
            snprintf(names[i], sizeof(names[i]), "suspect_number_%02d", i);
            many_kv[i][0] = KRUNKER_V_STR("con");
            many_kv[i][1] = KRUNKER_V_STR(names[i]);
            many_kv[i][2] = KRUNKER_V_STR("tim");
            many_kv[i][3] = KRUNKER_V_NUM(1700000000000.0 + i);
            many[i] = KRUNKER_V_MAP(many_kv[i], 2);
        }
        feed(&link, "kpd", (KrunkerValue[]) {KRUNKER_V_NUM(2), KRUNKER_V_STR(""), KRUNKER_V_ARRAY(many, ROWS)}, 3);
        int pages = 0, rows_seen = 0, tags_ok = 1;
        for (const NetPacket *p; (p = find_in(NET_PACKET_EXT, pages)) != NULL; pages++) {
            if (!net_ext_unpack(p->payload, p->length, &sub, &b, &bl) || sub != NET_EXT_KPD_RESPONSE || bl < sizeof(r)) break;
            memcpy(&r, b, sizeof(r));
            int page = 0, of = 0;
            tags_ok &= r.code == 2 && sscanf(r.name, "%d/%d", &page, &of) == 2 && page == pages + 1;
            cJSON *arr = cJSON_ParseWithLength((const char *) b + sizeof(r), bl - sizeof(r));
            rows_seen += cJSON_IsArray(arr) ? cJSON_GetArraySize(arr) : 0;
            cJSON_Delete(arr);
        }
        CHECK(pages > 1 && tags_ok && rows_seen == ROWS, "kpd list 2 of %d rows -> %d pages, %d rows", ROWS, pages, rows_seen);
        clear();
        // nil data (an empty list): "[]"
        feed(&link, "kpd", (KrunkerValue[]) {KRUNKER_V_NUM(3), KRUNKER_V_STR(""), KRUNKER_V_NIL}, 3);
        e = find_in(NET_PACKET_EXT, 0);
        json[0] = 0;
        if (e && net_ext_unpack(e->payload, e->length, &sub, &b, &bl) && bl >= sizeof(r)) {
            memcpy(&r, b, sizeof(r));
            snprintf(json, sizeof(json), "%.*s", (int) (bl - sizeof(r)), (const char *) b + sizeof(r));
        }
        CHECK(r.code == 3 && !strcmp(json, "[]"), "kpd list 3 with no data -> [] (%s)", json);
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
    test_hud();
    test_combat();
    test_fx();

    printf("%d/%d passed\n", g_pass, g_pass + g_fail);
    return g_fail ? 1 : 0;
}
