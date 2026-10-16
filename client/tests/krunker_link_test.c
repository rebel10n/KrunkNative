// client/src/net/krunker_link.c (the official-Krunker packet translation) without sockets: Krunker
// frames in (built with the codec, as a server would send them) -> NetPackets out; our packets in
// -> Krunker frames out (decoded again and checked field by field, ahNum trailers included).
//   cmake-build-debug/krunker_link_test
#include <krunker_link.h>
#include <krunker_build_live.h>
#include <krunker_maps.h>
#include <auth_join.h>
#include <net_ext.h>
#include <ruleset.h>
#include <zombies.h>
#include <social.h>
#include <voice.h>
#include <social_v7.h>
#include <ai.h> // test_fx / test_world: the AI packets
#include <krunker_sounds.h>
#include <progression.h> // test_rounds: overlay.medalsList
#include <cJSON.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail, g_pass;
#define CHECK(cond, ...) do { if (cond) g_pass++; else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// ---- test double: the official map table social.c's service reads (linked for the voice chunking only)
const cJSON *g_maps[32];

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
static const char *const k_point_end[] = {"sid", "name", "score", "kills", "deaths", "obj", "dmg", "reward", NULL};
const GameModeConfig *mode_config(const int id) {
    static const GameModeConfig ctf = {.leader_stats = k_ctf_lead, .end_stats = k_ctf_end};
    static const GameModeConfig shrp = {.obj_weapon = 1}, parkour = {.show_ms = 1}; // test_world: 22, 5
    static const GameModeConfig point = {.end_stats = k_point_end};                  // test_rounds: Hardpoint (2)
    static const GameModeConfig prop = {.prop = 1, .team_options = {"Prop", "Seekers"}}; // test_prophunt: Prop Hunt (11)
    return id == 3 ? &ctf : id == 22 ? &shrp : id == 5 ? &parkour : id == 2 ? &point : id == 11 ? &prop : NULL;
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

static int find_ext(uint16_t subtype, int nth, uint16_t *len); // the nth EXT of a subtype (below)

// the nth NET_EXT_KN_OFFICIAL_CHAT ("ch"): its header, sender name and text; 0 = none
static int official_chat(const int nth, NetOfficialChatHeader *h, const char **name, const char **text) {
    uint16_t len = 0;
    const int k = find_ext(NET_EXT_KN_OFFICIAL_CHAT, nth, &len);
    const uint8_t *b = k >= 0 ? ext_body(k, NET_EXT_KN_OFFICIAL_CHAT, &len) : NULL;
    if (!b || len <= sizeof(*h)) return 0;
    memcpy(h, b, sizeof(*h));
    *name = (const char *) b + sizeof(*h);
    *text = *name + strlen(*name) + 1;
    return 1;
}

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
        CHECK(o_p && o.uid == 3 && o.time_ms == 5000 && o.initial, "init: objective 3, 5 s (setObjective(.., true))");
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
        const NetOfficialMedalPacket *m = NULL;
        for (int k = 0; !m && find_in(NET_PACKET_EXT, k); k++) {
            uint16_t ml = 0;
            const uint8_t *mb = ext_body(k, NET_EXT_KN_OFFICIAL_MEDAL, &ml);
            if (mb && ml == sizeof(NetOfficialMedalPacket)) m = (const NetOfficialMedalPacket *) mb;
        }
        CHECK(m && !strcmp(m->text, "Pickup") && !strcmp(m->score, "+20") && m->count == 1 && !find_in(NET_PACKET_MEDAL, 0),
              "am: medalsList[24] Pickup +20 (NET_EXT_KN_OFFICIAL_MEDAL)");
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
        uint16_t len = 0;
        const int k = find_ext(NET_EXT_KN_END_TYPE, 0, &len); // after the END_ROW per endData row (official-ingame-rounds)
        const uint8_t *type = k >= 0 ? ext_body(k, NET_EXT_KN_END_TYPE, &len) : NULL;
        CHECK(type && len == 1 && type[0] == 0, "end type 0: END_TYPE 0 (the title for a player in the game only)");
    }
    clear();
    // type 1 (a joiner's end screen, v10.js:219715): END_TYPE 1 right before the END
    end_args[0] = KRUNKER_V_NUM(1);
    feed(&link, "end", end_args, 8);
    {
        uint16_t len = 0;
        const int k = find_ext(NET_EXT_KN_END_TYPE, 0, &len);
        const uint8_t *type = k >= 0 ? ext_body(k, NET_EXT_KN_END_TYPE, &len) : NULL;
        const NetPacket *xp = k >= 0 ? find_in(NET_PACKET_EXT, k) : NULL, *ep = find_in(NET_PACKET_END, 0);
        CHECK(type && len == 1 && type[0] == 1 && ep && xp < ep, "end type 1: END_TYPE 1, then the END (the results at once)");
    }
    end_args[0] = KRUNKER_V_NUM(0);
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

    // "ch": a team line with the sender's account; a server line with its colour
    feed(&link, "ch", (KrunkerValue[]) {KRUNKER_V_NUM(1), KRUNKER_V_STR("Ace"), KRUNKER_V_STR("push b"), KRUNKER_V_NUM(0), KRUNKER_V_NIL,
                                        KRUNKER_V_NIL, KRUNKER_V_NUM(4242)}, 7);
    feed(&link, "ch", (KrunkerValue[]) {KRUNKER_V_NUM(-1), KRUNKER_V_NIL, KRUNKER_V_STR("server says"), KRUNKER_V_NUM(2), KRUNKER_V_STR("#00ff00")}, 5);
    {
        NetOfficialChatHeader v, c;
        const char *vn = "", *vt = "", *cn = "", *ct = "";
        const int vp = official_chat(0, &v, &vn, &vt), cp = official_chat(1, &c, &cn, &ct);
        CHECK(vp && v.channel == CHAT_CHANNEL_TEAM && v.kind == NET_CHAT_PLAYER && v.account_id == 4242 && !strcmp(vn, "Ace") &&
              !strcmp(vt, "push b"), "ch: team tab + account");
        CHECK(cp && c.kind == NET_CHAT_SERVER && c.channel == CHAT_CHANNEL_ALL && !strcmp(c.color, "#00ff00") && !strcmp(ct, "server says"),
              "ch: a server line in its colour");
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

// Combat visuals / sounds (fx): "9" "s" "so" "sso" "pr" "bmb" "pre" "ex" "k"[11] "pk" "upk" "bh" "n" "bbm"
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
    feed(&link, "pr", (KrunkerValue[]) {KRUNKER_V_NUM(0), KRUNKER_V_NUM(0), KRUNKER_V_NUM(0), KRUNKER_V_NUM(0), KRUNKER_V_NUM(-M_PI / 2), KRUNKER_V_NUM(2),
                                        KRUNKER_V_NUM(302), KRUNKER_V_NUM(1)}, 8); // stored[2]: a bombardment bomb, dropped straight down
    {
        READ_IN(NetProjectilePacket, r, PROJECTILE, 0);
        READ_IN(NetProjectilePacket, k, PROJECTILE, 1);
        READ_IN(NetProjectilePacket, bo, PROJECTILE, 2);
        CHECK(r_p && r.weapon_index == 9 && r.owner_uid == 7 && r.id >= 0 && r.id < GAME_MAX_PROJECTILES && r.position[2] == 3 &&
              fabsf(r.velocity[2] - 0.6f) < 1e-5f && fabsf(r.velocity[0]) < 1e-5f && r.gravity == 0.0001f && r.range == 1200.0f,
              "pr: rocket, yaw PI flies +z at its spd (v %g %g %g)", r.velocity[0], r.velocity[1], r.velocity[2]);
        CHECK(k_p && k.weapon_index == WEAPON_ID_KNIFE && k.id != r.id && fabsf(k.velocity[0] + 0.38f) < 1e-5f && k.range == 1500.0f && !k.ai_kind,
              "pr: stored[0] the thrown knife (v %g %g %g)", k.velocity[0], k.velocity[1], k.velocity[2]);
        CHECK(bo_p && bo.bomb && bo.weapon_index == -1 && !bo.ai_kind && bo.owner_uid == -1 && bo.range == 1500.0f &&
              fabsf(bo.velocity[1] + 0.4f) < 1e-5f && fabsf(bo.velocity[0]) < 1e-5f && bo.id != r.id && bo.id != k.id,
              "pr: stored[2] is a bomb at 0.4 down (v %g %g %g)", bo.velocity[0], bo.velocity[1], bo.velocity[2]);
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

    // "bmb" [x, y, z, sid, pjSp, ...]: two bombs (sids 401 at pjSp 0.5, 402 at 1) and one without a sid
    {
        const KrunkerValue bombs[] = {KRUNKER_V_NUM(1.5), KRUNKER_V_NUM(300), KRUNKER_V_NUM(-2), KRUNKER_V_NUM(401), KRUNKER_V_NUM(0.5),
                                      KRUNKER_V_NUM(4), KRUNKER_V_NUM(310), KRUNKER_V_NUM(7), KRUNKER_V_NUM(402), KRUNKER_V_NUM(1),
                                      KRUNKER_V_NUM(0), KRUNKER_V_NUM(0), KRUNKER_V_NUM(0), KRUNKER_V_NIL, KRUNKER_V_NUM(1)};
        feed(&link, "bmb", (KrunkerValue[]) {KRUNKER_V_ARRAY(bombs, 15)}, 1);
        READ_IN(NetProjectilePacket, b1, PROJECTILE, 0);
        READ_IN(NetProjectilePacket, b2, PROJECTILE, 1);
        CHECK(!find_in(NET_PACKET_PROJECTILE, 2), "bmb: a bomb without a sid is skipped");
        CHECK(b1_p && b1.bomb && b1.position[0] == 1.5f && b1.position[1] == 300 && b1.position[2] == -2 && b1.owner_uid == -1 &&
              b1.range == 1500.0f && fabsf(b1.velocity[1] + 0.2f) < 1e-5f && fabsf(b1.velocity[0]) < 1e-5f,
              "bmb: a bomb at 1.5 300 -2 falls at 0.4 x pjSp 0.5 (v %g %g %g)", b1.velocity[0], b1.velocity[1], b1.velocity[2]);
        CHECK(b2_p && b2.bomb && b2.position[1] == 310 && b2.id != b1.id && fabsf(b2.velocity[1] + 0.4f) < 1e-5f,
              "bmb: the second bomb (sid 402) at pjSp 1");
        // its "pre" ends the first bomb's slot, with its impact
        feed(&link, "pre", (KrunkerValue[]) {KRUNKER_V_NUM(401), KRUNKER_V_NUM(0), KRUNKER_V_NUM(1), KRUNKER_V_NUM(2), KRUNKER_V_NUM(3), KRUNKER_V_NUM(1)}, 6);
        READ_IN(NetProjectileEndPacket, be, PROJECTILE_END, 0);
        CHECK(be_p && be.id == b1.id && be.impact, "pre: the bomb's slot ends with its impact");
    }
    clear();

    // "pr" stored[2] with customProj { pjSp: 0.25 }: a bomb at 0.1
    {
        const KrunkerValue pj_kv[] = {KRUNKER_V_STR("pjSp"), KRUNKER_V_NUM(0.25)};
        const KrunkerValue pj[] = {KRUNKER_V_NUM(0), KRUNKER_V_NUM(0), KRUNKER_V_NUM(0), KRUNKER_V_NUM(0), KRUNKER_V_NUM(-M_PI / 2),
                                   KRUNKER_V_NUM(2), KRUNKER_V_NUM(403), KRUNKER_V_NUM(1), KRUNKER_V_MAP(pj_kv, 1)};
        feed(&link, "pr", pj, 9);
        READ_IN(NetProjectilePacket, pb, PROJECTILE, 0);
        CHECK(pb_p && pb.bomb && fabsf(pb.velocity[1] + 0.1f) < 1e-5f, "pr: stored[2] customProj pjSp scales the bomb (v %g)", pb.velocity[1]);
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

// krunker_link_world.c: mode objectives, world objects, AI, zombies, loadouts, map triggers
static void test_world(void) {
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

    // init (Domination 25): round wins, syncData roundC / lockT / dest / mov / spwns / pkups / gates /
    // banks / dyno / gbl / doms / mybx / krm / pwup
    const KrunkerValue w1[] = {KRUNKER_V_NUM(1), KRUNKER_V_NUM(2)}, w2[] = {KRUNKER_V_NUM(2), KRUNKER_V_NUM(1)};
    const KrunkerValue wins[] = {KRUNKER_V_ARRAY(w1, 2), KRUNKER_V_ARRAY(w2, 2)};
    const KrunkerValue dest[] = {KRUNKER_V_NUM(12), KRUNKER_V_NUM(13)};
    const KrunkerValue mov[] = {KRUNKER_V_NUM(20), KRUNKER_V_NUM(4)};
    const KrunkerValue sp_data[] = {KRUNKER_V_STR("c"), KRUNKER_V_NUM(3)};
    const KrunkerValue sp0[] = {KRUNKER_V_NUM(3), KRUNKER_V_NUM(501), KRUNKER_V_NUM(1), KRUNKER_V_NUM(2), KRUNKER_V_NUM(3), KRUNKER_V_NUM(2),
                                KRUNKER_V_MAP(sp_data, 1)};
    const KrunkerValue spwns[] = {KRUNKER_V_ARRAY(sp0, 7)};
    const KrunkerValue pos[] = {KRUNKER_V_NUM(7), KRUNKER_V_NUM(8), KRUNKER_V_NUM(9)};
    const KrunkerValue pk0[] = {KRUNKER_V_NUM(30), KRUNKER_V_NUM(3), KRUNKER_V_NIL, KRUNKER_V_BOOL(1), KRUNKER_V_ARRAY(pos, 3)};
    const KrunkerValue pkups[] = {KRUNKER_V_ARRAY(pk0, 5)};
    const KrunkerValue gates[] = {KRUNKER_V_NUM(40), KRUNKER_V_BOOL(1)};
    const KrunkerValue banks[] = {KRUNKER_V_NUM(41), KRUNKER_V_NUM(250)};
    const KrunkerValue dy0[] = {KRUNKER_V_NUM(1), KRUNKER_V_NUM(10), KRUNKER_V_NUM(20), KRUNKER_V_NUM(30), KRUNKER_V_NUM(8),
                                KRUNKER_V_NUM(4), KRUNKER_V_NUM(8), KRUNKER_V_NUM(0), KRUNKER_V_NUM(2000)};
    const KrunkerValue dyno[] = {KRUNKER_V_ARRAY(dy0, 9)};
    const KrunkerValue gbl[] = {KRUNKER_V_STR("door"), KRUNKER_V_NUM(1), KRUNKER_V_STR("lvl"), KRUNKER_V_NUM(3)};
    const KrunkerValue doms[] = {KRUNKER_V_NUM(0), KRUNKER_V_NUM(1), KRUNKER_V_NUM(1), KRUNKER_V_NIL};
    const KrunkerValue mybx[] = {KRUNKER_V_NUM(50), KRUNKER_V_BOOL(1), KRUNKER_V_NUM(2), KRUNKER_V_NUM(3000), KRUNKER_V_NUM(4), KRUNKER_V_NIL};
    const KrunkerValue pwup[] = {KRUNKER_V_NUM(0), KRUNKER_V_NUM(9000), KRUNKER_V_NUM(0)};
    const KrunkerValue sync[] = {KRUNKER_V_STR("roundC"), KRUNKER_V_NUM(2), KRUNKER_V_STR("lockT"), KRUNKER_V_NUM(4500),
                                 KRUNKER_V_STR("dest"), KRUNKER_V_ARRAY(dest, 2), KRUNKER_V_STR("mov"), KRUNKER_V_ARRAY(mov, 2),
                                 KRUNKER_V_STR("spwns"), KRUNKER_V_ARRAY(spwns, 1), KRUNKER_V_STR("pkups"), KRUNKER_V_ARRAY(pkups, 1),
                                 KRUNKER_V_STR("gates"), KRUNKER_V_ARRAY(gates, 2), KRUNKER_V_STR("banks"), KRUNKER_V_ARRAY(banks, 2),
                                 KRUNKER_V_STR("dyno"), KRUNKER_V_ARRAY(dyno, 1), KRUNKER_V_STR("gbl"), KRUNKER_V_ARRAY(gbl, 4),
                                 KRUNKER_V_STR("doms"), KRUNKER_V_ARRAY(doms, 4), KRUNKER_V_STR("mybx"), KRUNKER_V_ARRAY(mybx, 6),
                                 KRUNKER_V_STR("pwup"), KRUNKER_V_ARRAY(pwup, 3)};
    KrunkerValue init[12];
    for (int i = 0; i < 12; i++) init[i] = KRUNKER_V_NIL;
    init[0] = KRUNKER_V_NUM(10);
    init[1] = KRUNKER_V_NUM(25);
    init[3] = KRUNKER_V_ARRAY(wins, 2);
    init[11] = KRUNKER_V_MAP(sync, 13);
    feed(&link, "init", init, 12);
    {
        READ_IN(NetModeSyncV7Packet, ms, MODE_SYNC_V7, 0);
        CHECK(ms_p && ms.rounds_left == 2 && ms.lockout_ms == 4500 && ms.has_round_wins && ms.team_round_wins[1] == 2 &&
              ms.team_round_wins[2] == 1 && ms.objective_uid == -1, "init: MODE_SYNC_V7 roundC 2, lockT, round wins 2 / 1");
        READ_IN(NetObjectStatePacket, d0, OBJECT_STATE, 0);
        READ_IN(NetObjectStatePacket, d1, OBJECT_STATE, 1);
        READ_IN(NetObjectStatePacket, mv, OBJECT_STATE, 2);
        READ_IN(NetObjectStatePacket, gt, OBJECT_STATE, 3);
        READ_IN(NetObjectStatePacket, bk, OBJECT_STATE, 4);
        CHECK(d0_p && d0.uid == 12 && d0.kind == NET_OBJECT_DESTROY && d1_p && d1.uid == 13 && mv_p && mv.uid == 20 &&
              mv.kind == NET_OBJECT_MOVE_TO_NODE && mv.value == 4 && gt_p && gt.uid == 40 && gt.kind == NET_OBJECT_GATE && gt.value == 1 &&
              bk_p && bk.uid == 41 && bk.kind == NET_OBJECT_BANK && bk.value == 250, "init: dest / mov / gates / banks");
        READ_IN(NetSpawnableV7Packet, sp, SPAWNABLE_V7, 0);
        CHECK(sp_p && sp.type == 3 && sp.sid == 501 && sp.active && sp.team == 2 && sp.position[2] == 3.0f && sp.data_c == 3 && sp.data_w == -1,
              "init: spwns -> a KR Tag (c 3)");
        READ_IN(NetPickupPacket, pk, PICKUP, 0);
        CHECK(pk_p && pk.uid == 30 && pk.weapon == 3 && pk.active && pk.moved && pk.position[1] == 8.0f && pk.ammo == 13,
              "init: pkups (ammo = the weapon's own, %d)", pk.ammo);
        READ_IN(NetBuildPacket, bo, BUILD, 0);
        CHECK(bo_p && bo.uid == 2000 && bo.build_index == 1 && bo.position[1] == 18.0f && bo.size[1] == 4.0f, "init: dyno centred (y 20 - 4 / 2)");
        READ_IN(NetValuePacket, g0, GLOBAL_VALUE, 0);
        READ_IN(NetValuePacket, g1, GLOBAL_VALUE, 1);
        CHECK(g0_p && !strcmp(g0.key, "door") && g0.value == 1 && (g0.flags & NET_VALUE_RESET) && g1_p && !strcmp(g1.key, "lvl") && !g1.flags,
              "init: gbl, the first resets");
        READ_IN(NetDomUpdatePacket, dm, DOM_UPDATE, 0);
        CHECK(dm_p && dm.count == 2 && dm.zones[0].uid == 0 && dm.zones[0].captured_by == 1 && dm.zones[0].mlt == 1.0f && dm.zones[1].uid == 1 &&
              !dm.zones[1].captured_by, "init: doms");
        READ_IN(NetZombieMachinePacket, mb, ZOMBIE_MACHINE, 0);
        CHECK(mb_p && mb.kind == ZOMBIE_MACHINE_MYSTERY_BOX && mb.uid == 50 && mb.active && mb.state == 2 && mb.timer == 3000.0f &&
              mb.pending_weapon == 4 && mb.purchased_by == ZOMBIES_NONE, "init: mybx");
        READ_IN(NetPowerupPacket, pw, POWERUP, 1);
        CHECK(pw_p && pw.index == 1 && pw.ms == 9000, "init: pwup fire sale 9 s");
    }
    clear();

    // Domination: uct (stride 6), cct
    const KrunkerValue on1[] = {KRUNKER_V_NUM(1)}, on2[] = {KRUNKER_V_NUM(1), KRUNKER_V_NUM(2)};
    const KrunkerValue uct[] = {KRUNKER_V_NUM(0), KRUNKER_V_NIL, KRUNKER_V_NUM(2500), KRUNKER_V_NUM(1), KRUNKER_V_ARRAY(on1, 1), KRUNKER_V_NUM(2),
                                KRUNKER_V_NUM(2), KRUNKER_V_NUM(2), KRUNKER_V_NUM(0), KRUNKER_V_NIL, KRUNKER_V_ARRAY(on2, 2), KRUNKER_V_NIL};
    feed(&link, "uct", (KrunkerValue[]) {KRUNKER_V_ARRAY(uct, 12)}, 1);
    feed(&link, "cct", (KrunkerValue[]) {KRUNKER_V_NUM(1), KRUNKER_V_NUM(2)}, 2);
    {
        READ_IN(NetDomUpdatePacket, d, DOM_UPDATE, 0);
        CHECK(d_p && d.count == 2 && !d.zones[0].captured_by && d.zones[0].capture_t == 2500.0f && d.zones[0].last_team_on == 1 &&
              d.zones[0].teams_on_count == 1 && d.zones[0].mlt == 2.0f && d.zones[1].captured_by == 2 && d.zones[1].teams_on_count == 2 &&
              d.zones[1].teams_on[1] == 2 && d.zones[1].mlt == 1.0f, "uct: two zones");
        READ_IN(NetDomCapturePacket, c, DOM_CAPTURE, 0);
        CHECK(c_p && c.uid == 1 && c.team == 2, "cct: zone 1 by team 2");
    }
    clear();

    // crsp / dsp, do / ro / gte / bnk / mo / mf, bo
    const KrunkerValue wb[] = {KRUNKER_V_STR("wId"), KRUNKER_V_NUM(6), KRUNKER_V_STR("xD"), KRUNKER_V_NUM(1.5)};
    feed(&link, "crsp", (KrunkerValue[]) {KRUNKER_V_NUM(6), KRUNKER_V_NUM(77), KRUNKER_V_NUM(5), KRUNKER_V_NUM(6), KRUNKER_V_NUM(7), KRUNKER_V_NUM(0),
                                          KRUNKER_V_MAP(wb, 2)}, 7);
    feed(&link, "dsp", (KrunkerValue[]) {KRUNKER_V_NUM(77)}, 1);
    feed(&link, "do", (KrunkerValue[]) {KRUNKER_V_NUM(5)}, 1);
    feed(&link, "ro", (KrunkerValue[]) {KRUNKER_V_NUM(5)}, 1);
    feed(&link, "gte", (KrunkerValue[]) {KRUNKER_V_NUM(6), KRUNKER_V_BOOL(0)}, 2);
    feed(&link, "bnk", (KrunkerValue[]) {KRUNKER_V_NUM(7), KRUNKER_V_NUM(30)}, 2);
    feed(&link, "mo", (KrunkerValue[]) {KRUNKER_V_NUM(8), KRUNKER_V_NUM(2)}, 2);
    feed(&link, "mf", (KrunkerValue[]) {KRUNKER_V_NUM(8)}, 1);
    feed(&link, "bo", (KrunkerValue[]) {KRUNKER_V_NUM(2), KRUNKER_V_NUM(1), KRUNKER_V_NUM(2), KRUNKER_V_NUM(3), KRUNKER_V_NUM(10), KRUNKER_V_NUM(20),
                                        KRUNKER_V_NUM(30), KRUNKER_V_NUM(3), KRUNKER_V_NUM(2001)}, 9);
    {
        READ_IN(NetSpawnableV7Packet, a, SPAWNABLE_V7, 0);
        READ_IN(NetSpawnableV7Packet, b, SPAWNABLE_V7, 1);
        CHECK(a_p && a.type == 6 && a.sid == 77 && a.active && a.data_w == 6 && a.data_xd == 1.5f && a.position[1] == 6.0f && b_p && b.sid == 77 &&
              !b.active, "crsp weapon box, dsp");
        int kinds[6] = {-1, -1, -1, -1, -1, -1}, values[6] = {0};
        for (int i = 0; i < 6; i++) {
            READ_IN(NetObjectStatePacket, o, OBJECT_STATE, i);
            if (o_p) kinds[i] = o.kind, values[i] = o.value;
        }
        CHECK(kinds[0] == NET_OBJECT_DESTROY && kinds[1] == NET_OBJECT_RESPAWN && kinds[2] == NET_OBJECT_GATE && values[2] == 0 &&
              kinds[3] == NET_OBJECT_BANK && values[3] == 30 && kinds[4] == NET_OBJECT_MOVE_TO_NODE && values[4] == 2 &&
              kinds[5] == NET_OBJECT_MOVE_FROM_NODE, "do / ro / gte / bnk / mo / mf");
        READ_IN(NetBuildPacket, bo, BUILD, 0);
        CHECK(bo_p && bo.uid == 2001 && bo.build_index == 2 && bo.direction == 3 && bo.position[1] == 2.0f && bo.size[0] == 10.0f &&
              bo.size[2] == 30.0f, "bo: not centred");
    }
    clear();

    // airdrops, AI
    const KrunkerValue ad[] = {KRUNKER_V_NUM(2), KRUNKER_V_NUM(300), KRUNKER_V_NUM(1), KRUNKER_V_NUM(400), KRUNKER_V_NUM(3), KRUNKER_V_NUM(5)};
    feed(&link, "ad", (KrunkerValue[]) {KRUNKER_V_ARRAY(ad, 6)}, 1);
    feed(&link, "rad", (KrunkerValue[]) {KRUNKER_V_NUM(2)}, 1);
    feed(&link, "krp", (KrunkerValue[]) {KRUNKER_V_NUM(9), KRUNKER_V_NUM(7)}, 2);
    KrunkerValue ai[17 * 9];
    for (int i = 0; i < 17; i++) {
        const KrunkerValue e[9] = {KRUNKER_V_NUM(100 + i), KRUNKER_V_NUM(i), KRUNKER_V_NUM(1), KRUNKER_V_NUM(2), KRUNKER_V_NUM(0.5),
                                   KRUNKER_V_NUM(0), KRUNKER_V_NUM(80), KRUNKER_V_BOOL(1), KRUNKER_V_NUM(2)};
        memcpy(&ai[i * 9], e, sizeof(e));
    }
    feed(&link, "ai", (KrunkerValue[]) {KRUNKER_V_ARRAY(ai, 17 * 9)}, 1);
    feed(&link, "ai", (KrunkerValue[]) {KRUNKER_V_NUM(0)}, 1);
    const KrunkerValue dat[] = {KRUNKER_V_STR("hasAnim"), KRUNKER_V_NUM(1), KRUNKER_V_STR("health"), KRUNKER_V_NUM(500),
                                KRUNKER_V_STR("asset"), KRUNKER_V_NUM(0), KRUNKER_V_STR("src"), KRUNKER_V_STR("ghost_0"),
                                KRUNKER_V_STR("team"), KRUNKER_V_NUM(2)};
    const KrunkerValue aai[] = {KRUNKER_V_NUM(100), KRUNKER_V_NUM(3), KRUNKER_V_STR("Zombie"), KRUNKER_V_NUM(1), KRUNKER_V_NUM(2),
                                KRUNKER_V_NUM(3), KRUNKER_V_MAP(dat, 5)};
    feed(&link, "aai", (KrunkerValue[]) {KRUNKER_V_ARRAY(aai, 7)}, 1);
    feed(&link, "rai", (KrunkerValue[]) {KRUNKER_V_NUM(100)}, 1);
    {
        READ_IN(NetAirDropPacket, d, AIRDROP, 0);
        CHECK(d_p && d.sid == 2 && d.fall == 300.0f && d.position[1] == 400.0f && d.index == 5 && d.owner_uid == -1, "ad");
        READ_IN(NetAirDropRemovePacket, r, AIRDROP_REMOVE, 0);
        READ_IN(NetAirDropClaimPacket, c, AIRDROP_CLAIM, 0);
        CHECK(r_p && r.sid == 2 && c_p && c.owner_uid == 9 && c.claimer_uid == 7, "rad, krp");
        READ_IN(NetAISyncV7Packet, s0, AI_SYNC_V7, 0);
        READ_IN(NetAISyncV7Packet, s1, AI_SYNC_V7, 1);
        READ_IN(NetAISyncV7Packet, s2, AI_SYNC_V7, 2);
        CHECK(s0_p && s0.first && s0.count == 16 && s0.entries[3].sid == 103 && s0.entries[3].position[0] == 3.0f && s0.entries[3].health == 80 &&
              s0.entries[3].did_move && s0.entries[3].team == 2 && s1_p && !s1.first && s1.count == 1 && s1.entries[0].sid == 116 && s2_p &&
              s2.first && !s2.count, "ai: 17 AIs in two chunks (hideAll once), then none");
        READ_IN(NetAIAddV7Packet, a, AI_ADD_V7, 0);
        CHECK(a_p && a.sid == 100 && a.index == 3 && !strcmp(a.name, "Zombie") && a.has_data && a.has_anim && a.max_health == 500.0f && !a.asset[0] &&
              !strcmp(a.src, "ghost_0") && a.team == 2 && a.position[2] == 3.0f, "aai");
        READ_IN(NetAIRemovePacket, rm, AI_REMOVE, 0);
        CHECK(rm_p && rm.sid == 100, "rai");
    }
    clear();

    // messages: gmsg (wt + suffix, a text popup), tx, lkt, chp, gt (Parkour's showMS)
    feed(&link, "gmsg", (KrunkerValue[]) {KRUNKER_V_STR("wt")}, 1);
    feed(&link, "gmsg", (KrunkerValue[]) {KRUNKER_V_STR("wt"), KRUNKER_V_STR("01:00")}, 2);
    const KrunkerValue pop[] = {KRUNKER_V_STR("Bomb"), KRUNKER_V_STR("Planted")};
    feed(&link, "gmsg", (KrunkerValue[]) {KRUNKER_V_STR("next round in"), KRUNKER_V_STR("00:05"), KRUNKER_V_NUM(3), KRUNKER_V_ARRAY(pop, 2)}, 4);
    feed(&link, "gmsg", NULL, 0);
    feed(&link, "tx", (KrunkerValue[]) {KRUNKER_V_STR("Overtime"), KRUNKER_V_STR("+60 Seconds")}, 2);
    feed(&link, "lkt", (KrunkerValue[]) {KRUNKER_V_NUM(90500)}, 1);
    feed(&link, "chp", (KrunkerValue[]) {KRUNKER_V_BOOL(0)}, 1);
    feed(&link, "chp", (KrunkerValue[]) {KRUNKER_V_BOOL(1)}, 1);
    {
        const char *want[] = {"wt", "wt 01:00", "next round in 00:05", "", "warmup starts in  01:30"};
        int ok = 1;
        for (int i = 0; i < 5; i++) {
            READ_IN(NetGameMessagePacket, m, GAME_MESSAGE, i);
            if (!m_p || strcmp(m.text, want[i])) {
                ok = 0;
                printf("  gmsg %d: \"%s\"\n", i, m_p ? m.text : "(none)");
            }
        }
        CHECK(ok, "gmsg: wt, wt + suffix, msg + suffix, cleared; lkt's warmup line");
        // the text popups and the countdown travel as NET_EXT_KN_OFFICIAL_TEXT_POPUP / _COUNTDOWN (test_popups)
        const char *titles[2] = {0}, *subs[2] = {0};
        int pops = 0, cd = 0;
        for (int k = 0; find_in(NET_PACKET_EXT, k); k++) {
            uint16_t len = 0;
            const uint8_t *b = ext_body(k, NET_EXT_KN_OFFICIAL_TEXT_POPUP, &len);
            if (b && pops < 2) {
                titles[pops] = (const char *) b + sizeof(NetOfficialTextPopupHeader);
                subs[pops] = titles[pops] + strlen(titles[pops]) + 1;
                pops++;
            }
            b = ext_body(k, NET_EXT_KN_OFFICIAL_COUNTDOWN, &len);
            if (b && len == sizeof(NetOfficialCountdownPacket)) memcpy(&cd, b, sizeof(cd));
        }
        CHECK(pops == 2 && !strcmp(titles[0], "Bomb") && !strcmp(subs[0], "Planted") && !strcmp(titles[1], "Overtime") &&
              !strcmp(subs[1], "+60 Seconds") && cd == 3 && !find_in(NET_PACKET_TEXT_POPUP, 0), "gmsg arg 3 / tx text popups, gmsg countdown 3");
        READ_IN(NetLockoutPacket, lk, LOCKOUT, 0);
        READ_IN(NetCheckpointPacket, c0, CHECKPOINT, 0);
        READ_IN(NetCheckpointPacket, c1, CHECKPOINT, 1);
        CHECK(lk_p && lk.ms == 90500 && c0_p && !c0.reset && c1_p && c1.reset, "lkt, chp");
    }
    clear();
    feed(&link, "init", (KrunkerValue[]) {KRUNKER_V_NUM(10), KRUNKER_V_NUM(5)}, 2); // Parkour
    clear();
    feed(&link, "gt", (KrunkerValue[]) {KRUNKER_V_NUM(12345), KRUNKER_V_NUM(2)}, 2);
    {
        READ_IN(NetLogTimePacket, t, LOG_TIME, 0);
        CHECK(t_p && t.time_ms == 12345 && t.time_log == 2 && t.kind == NET_LOG_TIME_ZONE && t.show_ms, "gt (Parkour shows ms)");
    }
    clear();

    // zombies
    feed(&link, "upz", (KrunkerValue[]) {KRUNKER_V_NUM(4), KRUNKER_V_STR("FRENZY")}, 2);
    feed(&link, "pwup", (KrunkerValue[]) {KRUNKER_V_NUM(2), KRUNKER_V_NUM(30000)}, 2);
    feed(&link, "krm", (KrunkerValue[]) {KRUNKER_V_NUM(60), KRUNKER_V_BOOL(1), KRUNKER_V_NUM(1), KRUNKER_V_NUM(0), KRUNKER_V_NUM(-1), KRUNKER_V_NUM(9)}, 6);
    feed(&link, "smybx", (KrunkerValue[]) {KRUNKER_V_NUM(50), KRUNKER_V_BOOL(1)}, 2);
    {
        READ_IN(NetZombieWavePacket, w, ZOMBIE_WAVE, 0);
        READ_IN(NetPowerupPacket, p, POWERUP, 0);
        READ_IN(NetZombieMachinePacket, k, ZOMBIE_MACHINE, 0);
        READ_IN(NetMysteryBoxSwitchPacket, s, MYSTERY_BOX_SWITCH, 0);
        CHECK(w_p && w.wave == 4 && !strcmp(w.special, "FRENZY") && p_p && p.index == 2 && p.ms == 30000 && k_p && k.kind == ZOMBIE_MACHINE_KRUM &&
              k.pending_weapon == -1 && k.purchased_by == 9 && s_p && s.uid == 50 && s.enable, "upz / pwup / krm / smybx");
    }
    clear();

    // loadouts: inat (null slots skipped, ammo by compacted slot, a pickup), inatA (everyone)
    const KrunkerValue lo[] = {KRUNKER_V_NUM(3), KRUNKER_V_NIL, KRUNKER_V_NUM(1)};
    const KrunkerValue am[] = {KRUNKER_V_NUM(5)};
    feed(&link, "inat", (KrunkerValue[]) {KRUNKER_V_NUM(9), KRUNKER_V_ARRAY(lo, 3), KRUNKER_V_NUM(1), KRUNKER_V_NUM(31), KRUNKER_V_NIL,
                                          KRUNKER_V_NIL, KRUNKER_V_NIL, KRUNKER_V_NIL, KRUNKER_V_ARRAY(am, 1)}, 9);
    feed(&link, "inat", (KrunkerValue[]) {KRUNKER_V_NUM(7), KRUNKER_V_ARRAY(lo, 3), KRUNKER_V_NUM(0), KRUNKER_V_NUM(32), KRUNKER_V_NUM(4)}, 5);
    const KrunkerValue all[] = {KRUNKER_V_NUM(2)};
    feed(&link, "inatA", (KrunkerValue[]) {KRUNKER_V_ARRAY(all, 1), KRUNKER_V_NUM(0)}, 2);
    {
        READ_IN(NetLoadoutPacket, a, LOADOUT, 0);
        CHECK(a_p && a.uid == 9 && a.count == 2 && a.weapons[0] == 3 && a.weapons[1] == 1 && a.ammo[0] == 5 && a.ammo[1] == -1 && a.loadout_index == 1,
              "inat: loadout 3 / 1, ammo 5 / the weapon's");
        CHECK(!find_in(NET_PACKET_PICKUP, 1), "inat: doPickup null = no pickup; absent = a pickup");
        READ_IN(NetPickupPacket, pk, PICKUP, 0);
        CHECK(pk_p && pk.uid == 32 && pk.weapon == 4 && pk.active && !pk.moved && pk.ammo == 14, "inat: the absent doPickup / active default true");
        READ_IN(NetLoadoutPacket, x, LOADOUT, 2);
        READ_IN(NetLoadoutPacket, y, LOADOUT, 3);
        CHECK(x_p && y_p && x.count == 1 && x.weapons[0] == 2 && ((x.uid == 9 && y.uid == 7) || (x.uid == 7 && y.uid == 9)), "inatA: both players");
    }
    clear();

    // map triggers
    feed(&link, "cv", (KrunkerValue[]) {KRUNKER_V_STR("coins"), KRUNKER_V_NUM(12)}, 2);
    feed(&link, "gv", (KrunkerValue[]) {KRUNKER_V_STR("door"), KRUNKER_V_BOOL(1)}, 2);
    feed(&link, "tgui", (KrunkerValue[]) {KRUNKER_V_NUM(4), KRUNKER_V_NUM(2)}, 2);
    feed(&link, "cnfm", (KrunkerValue[]) {KRUNKER_V_NUM(15), KRUNKER_V_NUM(2)}, 2);
    feed(&link, "cnfm", (KrunkerValue[]) {KRUNKER_V_NIL, KRUNKER_V_NUM(0)}, 2);
    {
        READ_IN(NetValuePacket, cv, CUSTOM_VALUE, 0);
        READ_IN(NetValuePacket, gv, GLOBAL_VALUE, 0);
        READ_IN(NetGuiTogglePacket, tg, GUI_TOGGLE, 0);
        READ_IN(NetConfirmPacket, c0, CONFIRM_INTERACT, 0);
        READ_IN(NetConfirmPacket, c1, CONFIRM_INTERACT, 1);
        CHECK(cv_p && !strcmp(cv.key, "coins") && cv.value == 12 && gv_p && gv.value == 1 && !gv.flags && tg_p && tg.id == 4 && tg.status == 2 &&
              c0_p && c0.uid == 15 && c0.kind == 2 && c1_p && c1.uid == -1 && !c1.kind, "cv / gv / tgui / cnfm");
    }
    clear();

    // Sharp Shooter (objWep 22): init's obj is the weapon (setWeapon), not an objective
    const KrunkerValue o[] = {KRUNKER_V_NUM(6), KRUNKER_V_NUM(0)};
    const KrunkerValue sync2[] = {KRUNKER_V_STR("obj"), KRUNKER_V_ARRAY(o, 2)};
    for (int i = 0; i < 12; i++) init[i] = KRUNKER_V_NIL;
    init[0] = KRUNKER_V_NUM(10);
    init[1] = KRUNKER_V_NUM(22);
    init[11] = KRUNKER_V_MAP(sync2, 1);
    feed(&link, "init", init, 12);
    {
        READ_IN(NetObjectivePacket, w, OBJECTIVE, 0);
        CHECK(w_p && w.kind == NET_OBJECTIVE_WEAPON && w.value == 6 && !find_in(NET_PACKET_OBJECTIVE_V7, 0), "init objWep: setWeapon(6)");
    }
    clear();
    CHECK(link.unknown_in == 0, "every world packet was handled (%u unknown)", link.unknown_in);
}

// the last frame the link sent (NULL: none)
static const KrunkerFrame *last_out(void) { return g_out_count ? &g_out[g_out_count - 1] : NULL; }

// Player actions (official-ingame-actions): the social v7 announcement, spectating, the match vote, sprays,
// pings and every outbound action translation
static void test_actions(void) {
    KrunkerLink link;
    const KrunkerLinkIO io = {.emit = cap_emit, .send = cap_send, .event = cap_event, .cheat = fake_cheat};
    krunker_link_init(&link, &io, KRUNKER_AHKEY);
    feed(&link, "io-init", (KrunkerValue[]) {KRUNKER_V_STR("sockA")}, 1);
    clear();

    // init (a ranked game: [16]): INIT, then NET_PACKET_SOCIAL_V7 (the v7 chat / spray / taunt shapes)
    KrunkerValue init[17];
    for (int i = 0; i < 17; i++) init[i] = KRUNKER_V_NIL;
    init[0] = KRUNKER_V_NUM(10);
    init[1] = KRUNKER_V_NUM(0);
    init[16] = KRUNKER_V_BOOL(1);
    feed(&link, "init", init, 17);
    int i_init = -1, i_social = -1;
    for (int i = 0; i < g_in_count; i++) {
        if (g_in[i].type == NET_PACKET_INIT) i_init = i;
        if (g_in[i].type == NET_PACKET_SOCIAL_V7) i_social = i;
    }
    CHECK(i_init >= 0 && i_social > i_init && g_in[i_social].length == 1 && g_in[i_social].payload[0] == NET_SOCIAL_V7_VERSION,
          "init: SOCIAL_V7 after the INIT (%d, %d)", i_init, i_social);
    CHECK(link.is_ranked == 1 && link.is_comp == 0, "init: ranked [16]");
    clear();

    // "start" [timer, team, spectating]: NET_PACKET_SPECTATE both ways
    feed(&link, "start", (KrunkerValue[]) {KRUNKER_V_STR("4:00"), KRUNKER_V_NUM(1), KRUNKER_V_BOOL(1)}, 3);
    feed(&link, "start", (KrunkerValue[]) {KRUNKER_V_STR("4:00"), KRUNKER_V_NUM(1), KRUNKER_V_BOOL(0)}, 3);
    {
        READ_IN(NetSpectatePacket, a, SPECTATE, 0);
        READ_IN(NetSpectatePacket, b, SPECTATE, 1);
        CHECK(a_p && a.spectating == 1 && b_p && b.spectating == 0, "start: spectating 1, then 0");
    }
    clear();

    // end: endData.mts / vo -> the vote cards; a public option has no name (the official list's), an
    // official one "mode,map,name,id"; vo counts weights, ours (sockA) is the voted entry
    const KrunkerValue mts[] = {KRUNKER_V_STR("0,1"), KRUNKER_V_STR("1,43,Bastion,0"), KRUNKER_V_STR("3,7")};
    const KrunkerValue v1[] = {KRUNKER_V_STR("1,43,Bastion,0"), KRUNKER_V_NUM(1)};
    const KrunkerValue v2[] = {KRUNKER_V_STR("1,43,Bastion,0"), KRUNKER_V_NUM(2)};
    const KrunkerValue v3[] = {KRUNKER_V_STR("0,1"), KRUNKER_V_NUM(1)};
    const KrunkerValue vo[] = {KRUNKER_V_STR("sockA"), KRUNKER_V_ARRAY(v1, 2), KRUNKER_V_STR("sockB"), KRUNKER_V_ARRAY(v2, 2),
                               KRUNKER_V_STR("sockC"), KRUNKER_V_ARRAY(v3, 2), KRUNKER_V_STR("sockD"), KRUNKER_V_NIL};
    const KrunkerValue end_kv[] = {KRUNKER_V_STR("ed"), KRUNKER_V_ARRAY(NULL, 0), KRUNKER_V_STR("mts"), KRUNKER_V_ARRAY(mts, 3),
                                   KRUNKER_V_STR("vo"), KRUNKER_V_MAP(vo, 4)};
    feed(&link, "end", (KrunkerValue[]) {KRUNKER_V_NUM(0), KRUNKER_V_BOOL(1), KRUNKER_V_MAP(end_kv, 3), KRUNKER_V_NUM(-1)}, 4);
    {
        READ_IN(NetMatchVoteStatePacket, st, MATCH_VOTE_STATE, 0);
        int vote_first = 1;
        for (int i = 0; i < g_in_count && g_in[i].type != NET_PACKET_MATCH_VOTE_STATE; i++)
            if (g_in[i].type == NET_PACKET_END) vote_first = 0;
        CHECK(st_p && st.count == 3 && st.voted == 1 && st.entries[0].mode_id == 0 && st.entries[0].map_index == 1 &&
              !strcmp(st.entries[0].map_name, "Littletown") && st.entries[0].votes == 1 && st.entries[1].mode_id == 1 &&
              st.entries[1].map_index == 43 && !strcmp(st.entries[1].map_name, "Bastion") && st.entries[1].votes == 3 &&
              st.entries[2].votes == 0 && vote_first, "end: vote cards (%d entries, ours %d)", st.count, st.voted);
    }
    clear();
    // "mv" [vo, mts]: our vote moves to "3,7"
    const KrunkerValue v4[] = {KRUNKER_V_STR("3,7"), KRUNKER_V_NUM(1)};
    const KrunkerValue vo2[] = {KRUNKER_V_STR("sockA"), KRUNKER_V_ARRAY(v4, 2)};
    feed(&link, "mv", (KrunkerValue[]) {KRUNKER_V_MAP(vo2, 1), KRUNKER_V_ARRAY(mts, 3)}, 2);
    {
        READ_IN(NetMatchVoteStatePacket, st, MATCH_VOTE_STATE, 0);
        CHECK(st_p && st.count == 3 && st.voted == 2 && st.entries[2].votes == 1 && st.entries[1].votes == 0, "mv: our vote on 3,7");
    }
    clear();
    // our card click -> "maVote" with the option's own string; an unknown pick sends nothing
    NetMatchVotePacket mv = {.mode_id = 1, .map_index = 43};
    krunker_link_send_ours(&link, NET_PACKET_MATCH_VOTE, &mv, sizeof(mv));
    CHECK(g_out_count == 1 && !strcmp(g_out[0].name, "maVote") && g_out[0].argc == 1 &&
          !strcmp(krunker_str(krunker_frame_arg(&g_out[0], 0)), "1,43,Bastion,0"), "maVote: the raw option");
    mv.map_index = 99;
    CHECK(!krunker_link_send_ours(&link, NET_PACKET_MATCH_VOTE, &mv, sizeof(mv)) && g_out_count == 1, "maVote: not an option");
    clear();

    // "sp" ADD_SPRAY -> NET_PACKET_SPRAY_V7
    feed(&link, "sp", (KrunkerValue[]) {KRUNKER_V_STR("7_1"), KRUNKER_V_NUM(2490), KRUNKER_V_NUM(1.5), KRUNKER_V_NUM(2), KRUNKER_V_NUM(-3.25),
                                        KRUNKER_V_NUM(90), KRUNKER_V_NUM(0), KRUNKER_V_NUM(-90), KRUNKER_V_NIL}, 9);
    {
        READ_IN(NetSprayV7Packet, sp, SPRAY_V7, 0);
        CHECK(sp_p && sp.uid == 7 && sp.slot == 1 && sp.skin == 2490 && sp.position[0] == 1.5f && sp.position[2] == -3.25f && sp.y_rot == 90 &&
              sp.z_rot == -90 && sp.dye == -1, "sp: spray 7_1");
    }
    clear();

    // "mpos" [key, {x, y, z}] -> NET_EXT_MARK_POSITION
    const KrunkerValue pos[] = {KRUNKER_V_STR("x"), KRUNKER_V_NUM(1), KRUNKER_V_STR("y"), KRUNKER_V_NUM(2), KRUNKER_V_STR("z"), KRUNKER_V_NUM(3)};
    feed(&link, "mpos", (KrunkerValue[]) {KRUNKER_V_STR("sockB"), KRUNKER_V_MAP(pos, 3)}, 2);
    {
        uint16_t n = 0;
        const uint8_t *b = ext_body(0, NET_EXT_MARK_POSITION, &n);
        NetMarkPositionPacket m = {0};
        if (b && n == sizeof(m)) memcpy(&m, b, sizeof(m));
        CHECK(b && !strcmp(m.key, "sockB") && m.x == 1 && m.y == 2 && m.z == 3, "mpos: ping of sockB");
    }
    clear();

    // outbound: our ping -> "mpos" [{x, y, z}] (ranked); not outside ranked / comp
    {
        uint8_t ext[64];
        const NetMarkPositionSendPacket m = {4, 5, 6};
        const uint16_t n = net_ext_pack(ext, sizeof(ext), NET_EXT_MARK_POSITION, &m, sizeof(m));
        krunker_link_send_ours(&link, NET_PACKET_EXT, ext, n);
        const KrunkerValue *p = g_out_count ? krunker_frame_arg(&g_out[0], 0) : NULL;
        CHECK(g_out_count == 1 && !strcmp(g_out[0].name, "mpos") && krunker_num(krunker_get(p, "x"), 0) == 4 &&
              krunker_num(krunker_get(p, "z"), 0) == 6, "mpos out (ranked)");
        clear();
        link.is_ranked = 0;
        CHECK(!krunker_link_send_ours(&link, NET_PACKET_EXT, ext, n) && g_out_count == 0, "mpos out: not in a casual game");
        clear();
    }

    // STREAK_SLOTS / PC_STAT are kept (nothing sent) and fill the next "en" [26..28] / [13]; COMP_SET 't' -> [8]
    const NetStreakSlotsPacket slots = {{3, -1, 5}};
    const NetPcStatPacket pc = {.index = 2};
    CHECK(!krunker_link_send_ours(&link, NET_PACKET_STREAK_SLOTS, &slots, sizeof(slots)) &&
          !krunker_link_send_ours(&link, NET_PACKET_PC_STAT, &pc, sizeof(pc)) && g_out_count == 0, "STREAK_SLOTS / PC_STAT: not sent");
    const NetCompSetPacket team = {.key = 't', .value = 2};
    krunker_link_send_ours(&link, NET_PACKET_COMP_SET, &team, sizeof(team));
    CHECK(g_out_count == 1 && !strcmp(g_out[0].name, "cd") && !strcmp(krunker_str(krunker_frame_arg(&g_out[0], 0)), "t") &&
          fnum(&g_out[0], 1) == 2, "cd t 2");
    clear();
    const NetCompSetPacket cls = {.key = 'c', .value = 4};
    krunker_link_send_ours(&link, NET_PACKET_COMP_SET, &cls, sizeof(cls));
    {
        const KrunkerValue *c = g_out_count ? krunker_frame_arg(&g_out[0], 1) : NULL;
        CHECK(g_out_count == 1 && !strcmp(g_out[0].name, "cd") && !strcmp(krunker_str(krunker_frame_arg(&g_out[0], 0)), "c") &&
              krunker_num(krunker_get(c, "c"), -1) == 4 && krunker_is(krunker_get(c, "s"), KRUNKER_NIL), "cd c {c: 4, s: null}");
    }
    clear();
    NetEntPacket ent;
    memset(&ent, 0, sizeof(ent));
    ent.secondary_index = -1;
    krunker_link_send_ours(&link, NET_PACKET_ENT, &ent, sizeof(ent));
    {
        const KrunkerValue *e = g_out_count ? krunker_frame_arg(&g_out[0], 0) : NULL;
        CHECK(e && krunker_num(krunker_at(e, 26), 0) == 3 && krunker_num(krunker_at(e, 27), 0) == -1 && krunker_num(krunker_at(e, 28), 0) == 5 &&
              krunker_num(krunker_at(e, 13), 0) == 2 && krunker_num(krunker_at(e, 8), 0) == 2, "en: streaks 3/-1/5, pcStat 2, team 2");
    }
    clear();

    // USE_STREAK -> "k1" (no args)
    const NetUseStreakPacket use = {.slot = 1};
    krunker_link_send_ours(&link, NET_PACKET_USE_STREAK, &use, sizeof(use));
    CHECK(g_out_count == 1 && !strcmp(g_out[0].name, "k1") && g_out[0].argc == 0, "USE_STREAK -> k1");
    clear();
    // SPECTATE_TARGET -> "fpSp" [sid] / []
    NetSpectateTargetPacket tgt = {.uid = 7};
    krunker_link_send_ours(&link, NET_PACKET_SPECTATE_TARGET, &tgt, sizeof(tgt));
    tgt.uid = -1;
    krunker_link_send_ours(&link, NET_PACKET_SPECTATE_TARGET, &tgt, sizeof(tgt));
    CHECK(g_out_count == 2 && !strcmp(g_out[0].name, "fpSp") && g_out[0].argc == 1 && fnum(&g_out[0], 0) == 7 && !strcmp(g_out[1].name, "fpSp") &&
          g_out[1].argc == 0, "fpSp 7, then the free cam");
    clear();
    // SPRAY_V7 -> "spry" [] / [index]; TAUNT -> "prpt" [i]
    NetSprayRequestPacket spr = {.index = -1};
    krunker_link_send_ours(&link, NET_PACKET_SPRAY_V7, &spr, sizeof(spr));
    spr.index = 2500;
    krunker_link_send_ours(&link, NET_PACKET_SPRAY_V7, &spr, sizeof(spr));
    const NetTauntPacket taunt = {.index = 3};
    krunker_link_send_ours(&link, NET_PACKET_TAUNT, &taunt, sizeof(taunt));
    CHECK(g_out_count == 3 && !strcmp(g_out[0].name, "spry") && g_out[0].argc == 0 && !strcmp(g_out[1].name, "spry") && fnum(&g_out[1], 0) == 2500 &&
          !strcmp(g_out[2].name, "prpt") && fnum(&g_out[2], 0) == 3, "spry, spry 2500, prpt 3");
    clear();
    // INTERACT: int / intSec [type, id], drpw, v3 spray
    NetInteractV7Packet it = {.action = 1, .secondary = 0, .type = 2, .uid = 15};
    krunker_link_send_ours(&link, NET_PACKET_INTERACT, &it, sizeof(it));
    it.secondary = 1;
    krunker_link_send_ours(&link, NET_PACKET_INTERACT, &it, sizeof(it));
    it.action = 2;
    krunker_link_send_ours(&link, NET_PACKET_INTERACT, &it, sizeof(it));
    const NetInteractPacket v3spray = {.action = 0};
    krunker_link_send_ours(&link, NET_PACKET_INTERACT, &v3spray, sizeof(v3spray));
    CHECK(g_out_count == 4 && !strcmp(g_out[0].name, "int") && fnum(&g_out[0], 0) == 2 && fnum(&g_out[0], 1) == 15 &&
          !strcmp(g_out[1].name, "intSec") && fnum(&g_out[1], 1) == 15 && !strcmp(g_out[2].name, "drpw") && g_out[2].argc == 0 &&
          !strcmp(g_out[3].name, "spry") && g_out[3].argc == 0, "int, intSec, drpw, spry");
    clear();
    // CONFIRM_INTERACT -> "cnfm" [] / [2]
    NetConfirmPacket cf = {.uid = 4, .kind = 0};
    krunker_link_send_ours(&link, NET_PACKET_CONFIRM_INTERACT, &cf, sizeof(cf));
    cf.kind = 2;
    krunker_link_send_ours(&link, NET_PACKET_CONFIRM_INTERACT, &cf, sizeof(cf));
    CHECK(g_out_count == 2 && !strcmp(g_out[0].name, "cnfm") && g_out[0].argc == 0 && g_out[1].argc == 1 && fnum(&g_out[1], 0) == 2 &&
          last_out() == &g_out[1], "cnfm, cnfm 2");
    clear();
}

// ---- a live build: tools/krunker/build.json changed the way a new Krunker build changes it
#ifndef KN_SOURCE_DIR
#define KN_SOURCE_DIR "."
#endif
static cJSON *jfind(cJSON *list, const char *name) {
    cJSON *e;
    cJSON_ArrayForEach(e, list) {
        if (!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(e, "name")), name)) return e;
    }
    return NULL;
}
static void jset(cJSON *o, const char *key, cJSON *v) {
    if (o && cJSON_GetObjectItem(o, key)) cJSON_ReplaceItemInObject(o, key, v);
    else if (o) cJSON_AddItemToObject(o, key, v);
    else cJSON_Delete(v);
}
static KrunkerBuild *next_build(void) {
    FILE *f = fopen(KN_SOURCE_DIR "/tools/krunker/build.json", "rb");
    if (!f) return NULL;
    static char text[1 << 20];
    const size_t n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    cJSON *d = cJSON_ParseWithLength(text, n);
    cJSON *c2s = cJSON_GetObjectItem(d, "packets_c2s"), *s2c = cJSON_GetObjectItem(d, "packets_s2c");
    cJSON *lay = cJSON_GetObjectItem(d, "layouts");
    cJSON *add = cJSON_GetObjectItem(lay, "add_players"), *k = cJSON_GetObjectItem(lay, "update_players");
    jset(d, "ahkey", cJSON_CreateNumber(0x17));
    jset(d, "build_version", cJSON_CreateString("LiveTestBuildVersion0123456789ab"));
    jset(d, "client_version", cJSON_CreateString("10.0.9r2"));
    jset(jfind(s2c, "ADD_PLAYERS"), "wire", cJSON_CreateString("A0"));
    jset(jfind(s2c, "UPDATE_PLAYERS"), "wire", cJSON_CreateString("Ak"));
    jset(jfind(s2c, "KILL_PLAYER"), "wire", cJSON_CreateString("A3"));
    jset(jfind(c2s, "LOAD"), "wire", cJSON_CreateString("Aload"));
    jset(jfind(c2s, "PONG"), "wire", cJSON_CreateString("Av"));
    jset(add, "stride", cJSON_CreateNumber(60));
    jset(jfind(cJSON_GetObjectItem(add, "fields"), "id"), "offset", cJSON_CreateNumber(58));
    jset(jfind(cJSON_GetObjectItem(add, "fields"), "sid"), "offset", cJSON_CreateNumber(55));
    jset(jfind(cJSON_GetObjectItem(add, "fields"), "name"), "offset", cJSON_CreateNumber(56));
    jset(jfind(cJSON_GetObjectItem(add, "fields"), "x"), "offset", cJSON_CreateNumber(57));
    jset(k, "stride", cJSON_CreateNumber(15));
    jset(jfind(cJSON_GetObjectItem(k, "fields"), "sid"), "offset", cJSON_CreateNumber(13));
    jset(jfind(cJSON_GetObjectItem(k, "fields"), "x"), "offset", cJSON_CreateNumber(14));
    cJSON *ka = cJSON_GetObjectItem(cJSON_GetObjectItem(lay, "kill_player"), "args");
    jset(ka, "victim", cJSON_CreateNumber(2));
    jset(ka, "killer", cJSON_CreateNumber(0));
    char *out = cJSON_PrintUnformatted(d);
    KrunkerBuild *b = NULL;
    char why[240] = "";
    if (krunker_build_parse(out, strlen(out), &b, why, sizeof(why)) != KRUNKER_BUILD_OK) printf("next build refused: %s\n", why);
    free(out);
    cJSON_Delete(d);
    return b;
}

// Social / account lines of a match (official-ingame-social): "vf", "prc", "fe", "skin", "skinF"
static void test_social(void) {
    KrunkerLink link;
    const KrunkerLinkIO io = {.emit = cap_emit, .send = cap_send, .event = cap_event, .cheat = fake_cheat};
    krunker_link_init(&link, &io, KRUNKER_AHKEY);
    feed(&link, "io-init", (KrunkerValue[]) {KRUNKER_V_STR("sockA")}, 1);
    feed(&link, "init", (KrunkerValue[]) {KRUNKER_V_NUM(10), KRUNKER_V_NUM(0), KRUNKER_V_NUM(0), KRUNKER_V_NIL, KRUNKER_V_NIL,
                                          KRUNKER_V_STR("sockB")}, 6);
    KrunkerValue add[2 * 53];
    for (int i = 0; i < 2 * 53; i++) add[i] = KRUNKER_V_NIL;
    add[0] = KRUNKER_V_STR("sockA"); add[1] = KRUNKER_V_NUM(9); add[8] = KRUNKER_V_NUM(100);
    add[53] = KRUNKER_V_STR("sockB"); add[54] = KRUNKER_V_NUM(7); add[61] = KRUNKER_V_NUM(100);
    feed(&link, "0", (KrunkerValue[]) {KRUNKER_V_ARRAY(add, 2 * 53)}, 1);
    clear();

    // "vf": nothing for a guest (v10 gates on the account)
    feed(&link, "vf", (KrunkerValue[]) {KRUNKER_V_NIL, KRUNKER_V_NUM(1), KRUNKER_V_NUM(3), KRUNKER_V_BOOL(1), KRUNKER_V_STR("Ann")}, 5);
    CHECK(!find_in(NET_PACKET_EXT, 0), "vf: a guest sees no forfeit vote");
    snprintf(link.account_name, sizeof(link.account_name), "me");
    feed(&link, "vf", (KrunkerValue[]) {KRUNKER_V_NIL, KRUNKER_V_NUM(1), KRUNKER_V_NUM(3), KRUNKER_V_BOOL(1), KRUNKER_V_STR("Ann")}, 5);
    feed(&link, "vf", (KrunkerValue[]) {KRUNKER_V_NIL, KRUNKER_V_NUM(2), KRUNKER_V_NUM(3), KRUNKER_V_BOOL(0), KRUNKER_V_NIL, KRUNKER_V_BOOL(1)}, 6);
    feed(&link, "vf", (KrunkerValue[]) {KRUNKER_V_STR("Bob"), KRUNKER_V_NUM(3), KRUNKER_V_NUM(3)}, 3);
    feed(&link, "vf", (KrunkerValue[]) {KRUNKER_V_NIL}, 1);
    {
        NetVoteForfeitStatePacket v[4];
        int ok = 1;
        for (int i = 0; i < 4; i++) {
            uint16_t len = 0;
            const uint8_t *b = ext_body(i, NET_EXT_VOTE_FORFEIT_STATE, &len);
            if (!b || len != sizeof(v[i])) ok = 0;
            else memcpy(&v[i], b, sizeof(v[i]));
        }
        CHECK(ok && v[0].active && v[0].opened && !strcmp(v[0].initiator, "Ann") && v[0].votes == 1 && v[0].needed == 3 && v[1].active &&
              v[1].update_only && v[1].votes == 2 && v[2].active && !v[2].opened && !strcmp(v[2].voter, "Bob") && v[2].votes == 3 && !v[3].active,
              "vf: opened by Ann, a count, Bob voted, ended");
    }
    clear();

    // "prc": sockB is now sockB2; "chgp" / "skinF" find it by the new id, the host follows
    feed(&link, "prc", (KrunkerValue[]) {KRUNKER_V_STR("sockB"), KRUNKER_V_STR("sockB2")}, 2);
    int found = 0;
    for (int i = 0; i < link.player_count; i++) found |= link.players[i].sid == 7 && !strcmp(link.players[i].id, "sockB2");
    CHECK(found && !strcmp(link.host_id, "sockB2"), "prc: the socket id (and the host) remapped");
    feed(&link, "skinF", (KrunkerValue[]) {KRUNKER_V_STR("sockB2"), KRUNKER_V_NUM(1234)}, 2);
    feed(&link, "skinF", (KrunkerValue[]) {KRUNKER_V_STR("sockB"), KRUNKER_V_NUM(55)}, 2);   // the old id: nobody
    feed(&link, "skinF", (KrunkerValue[]) {KRUNKER_V_STR("sockA"), KRUNKER_V_NUM(66)}, 2);   // us: the "skin" shows it
    feed(&link, "skin", (KrunkerValue[]) {KRUNKER_V_NUM(77)}, 1);
    {
        READ_IN(NetSkinFoundPacket, a, SKIN_FOUND, 0);
        READ_IN(NetSkinFoundPacket, b, SKIN_FOUND, 1);
        CHECK(a_p && a.uid == 7 && a.skin == 1234 && b_p && b.uid == 9 && b.skin == 77 && !find_in(NET_PACKET_SKIN_FOUND, 2),
              "skinF by the new socket id; skin is ours; the old id and our own skinF show nothing");
    }
    clear();

    // "fe": the message for the red line
    feed(&link, "fe", (KrunkerValue[]) {KRUNKER_V_STR("Server Full")}, 1);
    {
        uint16_t len = 0;
        const uint8_t *b = ext_body(0, NET_EXT_KN_OFFICIAL_ENTER_FAILED, &len);
        CHECK(b && len == 12 && !strcmp((const char *) b, "Server Full"), "fe: ENTER_FAILED with the message");
    }
    clear();
    CHECK(link.unknown_in == 0, "every social packet was handled (%u unknown)", link.unknown_in);
}

// Switching Sides, official medals with a text score, end rows of players who left (official-ingame-rounds)
static int find_ext(const uint16_t subtype, const int nth, uint16_t *len) {
    for (int k = 0, n = 0; find_in(NET_PACKET_EXT, k); k++) {
        if (ext_body(k, subtype, len) && n++ == nth) return k;
    }
    return -1;
}

static void test_rounds(void) {
    KrunkerLink link;
    const KrunkerLinkIO io = {.emit = cap_emit, .send = cap_send, .event = cap_event, .cheat = fake_cheat};
    krunker_link_init(&link, &io, KRUNKER_AHKEY);
    feed(&link, "io-init", (KrunkerValue[]) {KRUNKER_V_STR("sockA")}, 1);
    KrunkerValue init[15];
    for (int i = 0; i < 15; i++) init[i] = KRUNKER_V_NIL;
    init[0] = KRUNKER_V_NUM(10);
    init[1] = KRUNKER_V_NUM(2); // Hardpoint
    init[14] = KRUNKER_V_BOOL(1); // isOfficial
    feed(&link, "init", init, 15);
    KrunkerValue add[53];
    for (int i = 0; i < 53; i++) add[i] = KRUNKER_V_NIL;
    add[0] = KRUNKER_V_STR("sockA"); add[1] = KRUNKER_V_NUM(9); add[8] = KRUNKER_V_NUM(100); add[9] = KRUNKER_V_NUM(1);
    feed(&link, "0", (KrunkerValue[]) {KRUNKER_V_ARRAY(add, 53)}, 1);
    CHECK(link.is_official && link.my_sid == 9 && !krunker_link_inputs_paused(&link), "rounds: official Hardpoint, in the game");
    clear();

    // "rds" with its update-only flag: nothing; Switching Sides: ROUNDS, then the switch, no input until our "0"
    feed(&link, "rds", (KrunkerValue[]) {KRUNKER_V_NUM(2), KRUNKER_V_BOOL(1)}, 2);
    CHECK(!find_in(NET_PACKET_ROUNDS, 0) && !find_in(NET_PACKET_EXT, 0) && !krunker_link_inputs_paused(&link), "rds update-only: nothing");
    feed(&link, "rds", (KrunkerValue[]) {KRUNKER_V_NUM(2), KRUNKER_V_BOOL(0), KRUNKER_V_NUM(1)}, 3);
    {
        uint16_t len = 0;
        const int k = find_ext(NET_EXT_KN_OFFICIAL_ROUND_SWITCH, 0, &len);
        const NetPacket *r = find_in(NET_PACKET_ROUNDS, 0);
        CHECK(r && k >= 0 && len == 0 && r < find_in(NET_PACKET_EXT, k) && krunker_link_inputs_paused(&link),
              "rds: ROUNDS, the switch after it, our inputs paused");
    }
    feed(&link, "0", (KrunkerValue[]) {KRUNKER_V_ARRAY(add, 53)}, 1);
    CHECK(!krunker_link_inputs_paused(&link), "rds: our next \"0\" resumes the inputs");
    clear();

    // "am": the reward's text score, a medal index with its sound and count, a bare trigger text
    const KrunkerValue am[] = {KRUNKER_V_STR("Reward"), KRUNKER_V_STR("12 KR"), KRUNKER_V_NUM(0), KRUNKER_V_NUM(1),
                               KRUNKER_V_NUM(9), KRUNKER_V_NUM(100), KRUNKER_V_NUM(3), KRUNKER_V_NUM(2)};
    feed(&link, "am", (KrunkerValue[]) {KRUNKER_V_ARRAY(am, 8)}, 1);
    feed(&link, "am", (KrunkerValue[]) {KRUNKER_V_STR("Trigger text")}, 1);
    {
        uint16_t l0 = 0, l1 = 0;
        const int k0 = find_ext(NET_EXT_KN_OFFICIAL_MEDAL, 0, &l0), k1 = find_ext(NET_EXT_KN_OFFICIAL_MEDAL, 1, &l1);
        const NetOfficialMedalPacket *a = k0 >= 0 ? (const NetOfficialMedalPacket *) ext_body(k0, NET_EXT_KN_OFFICIAL_MEDAL, &l0) : NULL;
        const NetOfficialMedalPacket *b = k1 >= 0 ? (const NetOfficialMedalPacket *) ext_body(k1, NET_EXT_KN_OFFICIAL_MEDAL, &l1) : NULL;
        CHECK(a && !strcmp(a->text, "Reward") && !strcmp(a->score, "12 KR") && a->count == 1, "am: Reward / 12 KR");
        CHECK(b && !strcmp(b->text, g_progression_medals[9]) && !strcmp(b->score, "+100") && b->sound == 3 && b->count == 2,
              "am: medal 9, +100, sound 3, count 2");
        READ_IN(NetMedalPacket, t, MEDAL, 0);
        CHECK(t_p && !strcmp(t.text, "Trigger text") && !t.score, "am: a bare text stays a MEDAL");
    }
    clear();

    // "end": Hardpoint's columns + "reward" (isOfficial); sid 31 left before the end: its row still comes
    const KrunkerValue n9[] = {KRUNKER_V_STR("Guest_9"), KRUNKER_V_STR("Guest_9")};
    const KrunkerValue n31[] = {KRUNKER_V_STR("acct31"), KRUNKER_V_STR("Gone31")};
    const KrunkerValue ed[] = {KRUNKER_V_NUM(9), KRUNKER_V_ARRAY(n9, 2), KRUNKER_V_NUM(300), KRUNKER_V_NUM(5), KRUNKER_V_NUM(2), KRUNKER_V_NUM(40),
                               KRUNKER_V_NUM(812), KRUNKER_V_STR("<span style='color:#F8C55C'>+12</span> KR"),
                               KRUNKER_V_NUM(31), KRUNKER_V_ARRAY(n31, 2), KRUNKER_V_NUM(150), KRUNKER_V_NUM(3), KRUNKER_V_NUM(4), KRUNKER_V_NUM(10),
                               KRUNKER_V_NUM(377), KRUNKER_V_STR("NONE")};
    const KrunkerValue data[] = {KRUNKER_V_STR("ed"), KRUNKER_V_ARRAY(ed, 16)};
    feed(&link, "end", (KrunkerValue[]) {KRUNKER_V_NUM(0), KRUNKER_V_BOOL(1), KRUNKER_V_MAP(data, 1), KRUNKER_V_NUM(1)}, 4);
    {
        uint16_t l0 = 0, l1 = 0;
        const int k0 = find_ext(NET_EXT_KN_OFFICIAL_END_ROW, 0, &l0), k1 = find_ext(NET_EXT_KN_OFFICIAL_END_ROW, 1, &l1);
        NetOfficialEndRowPacket a, b;
        memset(&a, 0, sizeof(a));
        memset(&b, 0, sizeof(b));
        if (k0 >= 0) memcpy(&a, ext_body(k0, NET_EXT_KN_OFFICIAL_END_ROW, &l0), sizeof(a));
        if (k1 >= 0) memcpy(&b, ext_body(k1, NET_EXT_KN_OFFICIAL_END_ROW, &l1), sizeof(b));
        CHECK(k0 >= 0 && a.uid == 9 && !strcmp(a.name, "Guest_9") && a.score == 300 && a.kills == 5 && a.deaths == 2 && a.dmg == 812 &&
              a.reward == 12, "end row 9: values, dmg 812, reward 12");
        CHECK(k1 >= 0 && b.uid == 31 && !strcmp(b.name, "Gone31") && b.score == 150 && b.kills == 3 && b.deaths == 4 && b.dmg == 377 && !b.reward,
              "end row 31 (left): its alias, values, dmg 377");
        READ_IN(NetEndPacket, e, END, 0);
        CHECK(e_p && e.board.count == 2 && find_in(NET_PACKET_EXT, k1) < e_p, "end: the rows come before END");
    }
    clear();
    // not official: no "reward" column is appended (Hardpoint's own list has one, so the row stays 8 wide)
    link.is_official = 0;
    feed(&link, "end", (KrunkerValue[]) {KRUNKER_V_NUM(0), KRUNKER_V_BOOL(1), KRUNKER_V_MAP(data, 1), KRUNKER_V_NUM(1)}, 4);
    {
        READ_IN(NetEndPacket, e, END, 0);
        CHECK(e_p && e.board.count == 2 && e.rewards[0] == 12, "end (custom lobby): the same rows");
    }
    clear();
}


// Voice chat (official-ingame-voice): "vc" in -> NET_PACKET_VOICE_V7 relay chunks; our VOICE_V7 clip
// chunks -> one "vc" out
static KrunkerLink *g_voice_link;
static int g_voice_sends;
static int voice_to_link(void *user, const NetPacketType type, const void *payload, const uint16_t length) {
    (void) user;
    g_voice_sends += krunker_link_send_ours(g_voice_link, type, payload, length);
    return 1;
}

static void test_voice(void) {
    KrunkerLink link;
    const KrunkerLinkIO io = {.emit = cap_emit, .send = cap_send, .event = cap_event, .cheat = fake_cheat};
    krunker_link_init(&link, &io, KRUNKER_AHKEY);
    feed(&link, "io-init", (KrunkerValue[]) {KRUNKER_V_STR("sockA")}, 1);
    clear();
    // a clip longer than one chunk (3 x NET_SOCIAL_CHUNK_DATA)
    const size_t audio_len = 3 * NET_SOCIAL_CHUNK_DATA;
    char *audio = malloc(audio_len + 1);
    const size_t prefix = strlen(VOICE_DATA_URL_PREFIX);
    memcpy(audio, VOICE_DATA_URL_PREFIX, prefix);
    for (size_t i = prefix; i < audio_len; i++) audio[i] = "ABCDEFGHabcdefgh0123456789+/"[i % 28];
    audio[audio_len] = 0;
    feed(&link, "vc", (KrunkerValue[]) {KRUNKER_V_STR(audio), KRUNKER_V_STR("Talker"), KRUNKER_V_NUM(7), KRUNKER_V_NUM(1), KRUNKER_V_NUM(0.5),
                                        KRUNKER_V_BOOL(0), KRUNKER_V_NUM(321)}, 7);
    {
        SocialAssembly a = {0};
        char *msg = NULL;
        uint32_t id = 0;
        int chunks = 0, done = 0;
        for (int k = 0; find_in(NET_PACKET_VOICE_V7, k); k++, chunks++) {
            const NetPacket *pk = find_in(NET_PACKET_VOICE_V7, k);
            if (social_assembly_push(&a, pk->payload, pk->length, &msg, &id) == 1) done = 1;
        }
        VoiceV7Meta meta = {0};
        char alias[64] = "";
        const char *got = NULL;
        CHECK(done && chunks >= 3 && id == 7 && voice_parse_relay_v7(msg, &meta, alias, sizeof(alias), &got) && meta.channel == 1 &&
              fabsf(meta.volume - 0.5f) < 1e-3f && !meta.is_final && meta.account_id == 321 && !strcmp(alias, "Talker") && got &&
              !strcmp(got, audio) && link.voice_in == 1, "vc in: %d chunks, the v7 relay of sid 7 on channel 1", chunks);
        free(msg);
        social_assembly_free(&a);
    }
    clear();
    // not a data:audio URL: dropped
    feed(&link, "vc", (KrunkerValue[]) {KRUNKER_V_STR("javascript:alert(1)"), KRUNKER_V_STR("x"), KRUNKER_V_NUM(7)}, 3);
    CHECK(!find_in(NET_PACKET_VOICE_V7, 0) && link.voice_in == 1, "vc in: a non-audio URL is dropped");
    clear();

    // out: our clip chunks (team channel, volume 0.75, quality 6000) -> one "vc" when the last chunk is in
    g_voice_link = &link;
    g_voice_sends = 0;
    const VoiceV7Send send = {.channel = 1, .volume = 0.75f, .quality = 6000};
    CHECK(voice_send_clip_v7(audio, audio_len, 3, &send, voice_to_link, NULL), "voice_send_clip_v7");
    const KrunkerFrame *f = g_out_count ? &g_out[g_out_count - 1] : NULL;
    CHECK(g_voice_sends == 1 && g_out_count == 1 && f && !strcmp(f->name, "vc") && f->argc == 4 &&
          krunker_str(krunker_frame_arg(f, 0)) && !strcmp(krunker_str(krunker_frame_arg(f, 0)), audio) && fnum(f, 1) == 1 &&
          fabs(fnum(f, 2) - 0.75) < 1e-6 && fnum(f, 3) == 6000 && link.voice_sent == 1,
          "vc out: one frame [dataURL, 1, 0.75, 6000] for a %d-chunk clip", (int) ((audio_len + 40) / NET_SOCIAL_CHUNK_DATA + 1));
    clear();
    // a global-channel clip: channel 0
    const VoiceV7Send glob = {.channel = 0, .volume = 1.0f, .quality = 6000};
    voice_send_clip_v7(VOICE_DATA_URL_PREFIX "AAAA", strlen(VOICE_DATA_URL_PREFIX "AAAA"), 4, &glob, voice_to_link, NULL);
    CHECK(g_out_count == 1 && fnum(&g_out[0], 1) == 0 && fnum(&g_out[0], 2) == 1, "vc out: global channel 0, volume 1");
    clear();
    // reset mid-clip frees the half assembly (no leak under ASan / no stale join)
    NetSocialChunk c = {.id = 9, .index = 0, .count = 2};
    memcpy(c.data, "0 1 6000\ndata:audio/", 20);
    krunker_link_send_ours(&link, NET_PACKET_VOICE_V7, &c, NET_SOCIAL_CHUNK_HEADER + 20);
    krunker_link_reset(&link);
    CHECK(!link.voice_out.buf && !g_out_count, "reset drops a half-sent clip");
    free(audio);
}

// v10's textPopup and the gmsg countdown (official-ingame-popups): "tx" [title, sub, silent, subSize],

// The JSON record of the nth NetExtJsonChunk EXT of `subtype` (slices reassembled), malloc'd; NULL none
static char *json_ext(const uint16_t subtype, const int nth, int *slices) {
    char *out = NULL;
    size_t len = 0;
    int n = -1;
    *slices = 0;
    for (int k = 0; find_in(NET_PACKET_EXT, k); k++) {
        uint16_t blen = 0;
        const uint8_t *b = ext_body(k, subtype, &blen);
        if (!b || blen < sizeof(NetExtJsonChunk)) continue;
        NetExtJsonChunk h;
        memcpy(&h, b, sizeof(h));
        if (h.part == 0) n++;
        if (n != nth) continue;
        char *grown = realloc(out, len + blen - sizeof(h) + 1);
        if (!grown) break;
        out = grown;
        memcpy(out + len, b + sizeof(h), blen - sizeof(h));
        len += blen - sizeof(h);
        out[len] = 0;
        (*slices)++;
        if (h.part + 1 == h.parts) break;
    }
    return out;
}

// KrunkScript entities / runtime blocks / objmv / plk (official-ingame-script: krunker_link_script.c)
static void test_script(void) {
    KrunkerLink link;
    const KrunkerLinkIO io = {.emit = cap_emit, .send = cap_send, .event = cap_event, .cheat = fake_cheat};
    krunker_link_init(&link, &io, KRUNKER_AHKEY);
    clear();
    // "e+" [eid, archId, tick, state, tag, mode, hp]
    const KrunkerValue st[] = {KRUNKER_V_STR("x"), KRUNKER_V_NUM(1.5), KRUNKER_V_STR("yaw"), KRUNKER_V_NUM(0.5)};
    const KrunkerValue payload[] = {KRUNKER_V_NUM(10000), KRUNKER_V_NUM(1), KRUNKER_V_NUM(7), KRUNKER_V_MAP(st, 2), KRUNKER_V_STR("lift"),
                                    KRUNKER_V_STR("sim"), KRUNKER_V_NUM(0)};
    feed(&link, "e+", (KrunkerValue[]) {KRUNKER_V_ARRAY(payload, 7)}, 1);
    int slices = 0;
    char *j = json_ext(NET_EXT_ENTITY_SPAWN, 0, &slices);
    cJSON *rec = j ? cJSON_Parse(j) : NULL;
    CHECK(rec && slices == 1 && cJSON_GetArraySize(rec) == 7 && cJSON_GetArrayItem(rec, 0)->valuedouble == 10000 &&
          cJSON_GetObjectItem(cJSON_GetArrayItem(rec, 3), "yaw")->valuedouble == 0.5 && !strcmp(cJSON_GetArrayItem(rec, 4)->valuestring, "lift"),
          "e+: the spawn payload as one JSON record (%s)", j ? j : "-");
    cJSON_Delete(rec);
    free(j);
    clear();
    // a state too large for one body: sliced, reassembled exactly
    char big[64 * 8 + 1];
    memset(big, 'q', 64);
    big[64] = 0;
    KrunkerValue bst[16];
    char keys[8][8];
    for (int i = 0; i < 8; i++) {
        snprintf(keys[i], sizeof(keys[i]), "k%d", i);
        bst[2 * i] = KRUNKER_V_STR(keys[i]);
        bst[2 * i + 1] = KRUNKER_V_STR(big);
    }
    const KrunkerValue bpay[] = {KRUNKER_V_NUM(10001), KRUNKER_V_NUM(1), KRUNKER_V_NUM(0), KRUNKER_V_MAP(bst, 8)};
    feed(&link, "e+", (KrunkerValue[]) {KRUNKER_V_ARRAY(bpay, 4)}, 1);
    j = json_ext(NET_EXT_ENTITY_SPAWN, 0, &slices);
    rec = j ? cJSON_Parse(j) : NULL;
    CHECK(rec && slices == 2 && cJSON_GetObjectItem(cJSON_GetArrayItem(rec, 3), "k7") &&
          strlen(cJSON_GetObjectItem(cJSON_GetArrayItem(rec, 3), "k7")->valuestring) == 64, "e+: a %d-byte state in %d slices", j ? (int) strlen(j) : 0,
          slices);
    cJSON_Delete(rec);
    free(j);
    clear();
    // "es" [[eid, tick, state], ...]: one record each; "e-", "ee", "ea+", "ea-": the args
    const KrunkerValue s1[] = {KRUNKER_V_STR("y"), KRUNKER_V_NUM(4)};
    const KrunkerValue b1[] = {KRUNKER_V_NUM(10000), KRUNKER_V_NUM(8), KRUNKER_V_MAP(s1, 1)};
    const KrunkerValue b2[] = {KRUNKER_V_NUM(10002), KRUNKER_V_NUM(8), KRUNKER_V_MAP(s1, 1)};
    const KrunkerValue bases[] = {KRUNKER_V_ARRAY(b1, 3), KRUNKER_V_ARRAY(b2, 3)};
    feed(&link, "es", (KrunkerValue[]) {KRUNKER_V_ARRAY(bases, 2)}, 1);
    feed(&link, "e-", (KrunkerValue[]) {KRUNKER_V_NUM(10000), KRUNKER_V_STR("gone")}, 2);
    feed(&link, "ee", (KrunkerValue[]) {KRUNKER_V_NUM(10000), KRUNKER_V_STR("hit"), KRUNKER_V_NUM(1), KRUNKER_V_NIL}, 4);
    const KrunkerValue opt[] = {KRUNKER_V_STR("lk"), KRUNKER_V_BOOL(1)};
    feed(&link, "ea+", (KrunkerValue[]) {KRUNKER_V_NUM(9), KRUNKER_V_NUM(10000), KRUNKER_V_MAP(opt, 1)}, 3);
    feed(&link, "ea-", (KrunkerValue[]) {KRUNKER_V_NUM(9), KRUNKER_V_NUM(10000)}, 2);
    {
        char *a = json_ext(NET_EXT_ENTITY_STATE, 0, &slices), *b = json_ext(NET_EXT_ENTITY_STATE, 1, &slices);
        char *r = json_ext(NET_EXT_ENTITY_REMOVE, 0, &slices), *ev = json_ext(NET_EXT_ENTITY_EVENT, 0, &slices);
        char *at = json_ext(NET_EXT_ENTITY_ATTACH, 0, &slices), *dt = json_ext(NET_EXT_ENTITY_DETACH, 0, &slices);
        CHECK(a && b && !strcmp(a, "[10000,8,{\"y\":4}]") && !strcmp(b, "[10002,8,{\"y\":4}]"), "es: one record per baseline (%s %s)", a ? a : "-",
              b ? b : "-");
        CHECK(r && !strcmp(r, "[10000,\"gone\"]") && ev && !strcmp(ev, "[10000,\"hit\",1,null]") && at && !strcmp(at, "[9,10000,{\"lk\":true}]") &&
              dt && !strcmp(dt, "[9,10000]"), "e- / ee / ea+ / ea-: their args (%s %s %s %s)", r ? r : "-", ev ? ev : "-", at ? at : "-", dt ? dt : "-");
        free(a), free(b), free(r), free(ev), free(at), free(dt);
    }
    clear();
    // "lo" [[id, {type, x, y, z, w, l, h, s, data}]], "slo" [[id, "remove"]], "objmv", "plk"
    const KrunkerValue blk[] = {KRUNKER_V_STR("type"), KRUNKER_V_STR("block"), KRUNKER_V_STR("x"), KRUNKER_V_NUM(5), KRUNKER_V_STR("y"), KRUNKER_V_NUM(0),
                                KRUNKER_V_STR("w"), KRUNKER_V_NUM(10)};
    const KrunkerValue lo1[] = {KRUNKER_V_NUM(0), KRUNKER_V_MAP(blk, 4)};
    feed(&link, "lo", (KrunkerValue[]) {KRUNKER_V_ARRAY(((KrunkerValue[]) {KRUNKER_V_ARRAY(lo1, 2)}), 1)}, 1);
    const KrunkerValue rm[] = {KRUNKER_V_NUM(0), KRUNKER_V_STR("remove")};
    feed(&link, "slo", (KrunkerValue[]) {KRUNKER_V_ARRAY(((KrunkerValue[]) {KRUNKER_V_ARRAY(rm, 2)}), 1)}, 1);
    feed(&link, "objmv", (KrunkerValue[]) {KRUNKER_V_NUM(12), KRUNKER_V_NUM(1), KRUNKER_V_NUM(2.5), KRUNKER_V_NUM(-3)}, 4);
    feed(&link, "plk", (KrunkerValue[]) {KRUNKER_V_BOOL(1), KRUNKER_V_NUM(0)}, 2);
    {
        char *lo = json_ext(NET_EXT_KN_OFFICIAL_LIVE_OBJECT, 0, &slices), *slo = json_ext(NET_EXT_KN_OFFICIAL_LIVE_SYNC, 0, &slices);
        CHECK(lo && !strcmp(lo, "[0,{\"type\":\"block\",\"x\":5,\"y\":0,\"w\":10}]") && slo && !strcmp(slo, "[0,\"remove\"]"), "lo / slo (%s %s)",
              lo ? lo : "-", slo ? slo : "-");
        free(lo), free(slo);
        uint16_t len = 0;
        const int km = find_ext(NET_EXT_KN_OFFICIAL_OBJECT_MOVED, 0, &len), kp = find_ext(NET_EXT_PLAYER_LOCKS, 0, &len);
        NetObjectMovedPacket m = {0};
        NetPlayerLocksPacket pl = {0};
        const uint8_t *mb = km >= 0 ? ext_body(km, NET_EXT_KN_OFFICIAL_OBJECT_MOVED, &len) : NULL;
        if (mb && len == sizeof(m)) memcpy(&m, mb, sizeof(m));
        const uint8_t *pb = kp >= 0 ? ext_body(kp, NET_EXT_PLAYER_LOCKS, &len) : NULL;
        if (pb && len == sizeof(pl)) memcpy(&pl, pb, sizeof(pl));
        CHECK(mb && m.uid == 12 && m.x == 1.0f && m.y == 2.5f && m.z == -3.0f, "objmv: uid 12 to 1, 2.5, -3");
        CHECK(pb && pl.disable_shooting == 1 && pl.disable_melee == 0, "plk: shooting locked, melee not");
    }
    clear();
    // init syncData: lo, ent, entAtt, chg
    const KrunkerValue att[] = {KRUNKER_V_NUM(9), KRUNKER_V_NUM(10000), KRUNKER_V_NIL};
    const KrunkerValue chg[] = {KRUNKER_V_NUM(12), KRUNKER_V_NUM(0), KRUNKER_V_NUM(1), KRUNKER_V_NUM(2)};
    const KrunkerValue sync[] = {KRUNKER_V_STR("lo"), KRUNKER_V_ARRAY(((KrunkerValue[]) {KRUNKER_V_ARRAY(lo1, 2)}), 1),
                                 KRUNKER_V_STR("ent"), KRUNKER_V_ARRAY(((KrunkerValue[]) {KRUNKER_V_ARRAY(payload, 7)}), 1),
                                 KRUNKER_V_STR("entAtt"), KRUNKER_V_ARRAY(((KrunkerValue[]) {KRUNKER_V_ARRAY(att, 3)}), 1),
                                 KRUNKER_V_STR("chg"), KRUNKER_V_ARRAY(((KrunkerValue[]) {KRUNKER_V_ARRAY(chg, 4)}), 1)};
    KrunkerValue init[12];
    for (int i = 0; i < 12; i++) init[i] = KRUNKER_V_NIL;
    init[0] = KRUNKER_V_NUM(10);
    init[1] = KRUNKER_V_NUM(0);
    init[11] = KRUNKER_V_MAP(sync, 4);
    feed(&link, "init", init, 12);
    {
        char *lo = json_ext(NET_EXT_KN_OFFICIAL_LIVE_OBJECT, 0, &slices), *en = json_ext(NET_EXT_ENTITY_SPAWN, 0, &slices);
        char *at = json_ext(NET_EXT_ENTITY_ATTACH, 0, &slices);
        uint16_t len = 0;
        CHECK(lo && en && at && !strncmp(en, "[10000,1,7,", 11) && !strcmp(at, "[9,10000,null]") && find_ext(NET_EXT_KN_OFFICIAL_OBJECT_MOVED, 0, &len) >= 0 &&
              find_in(NET_PACKET_INIT, 0) && (const void *) find_in(NET_PACKET_INIT, 0) < (const void *) find_in(NET_PACKET_EXT, 0),
              "init syncData: lo, ent, entAtt, chg after the INIT");
        free(lo), free(en), free(at);
    }
    clear();
    CHECK(link.unknown_in == 0, "every script packet was handled (%u unknown)", link.unknown_in);
}

// "stlkpd", "gmsg" [.., n, [title, sub]] -> NET_EXT_KN_OFFICIAL_TEXT_POPUP / _COUNTDOWN
static void test_popups(void) {
    KrunkerLink link;
    const KrunkerLinkIO io = {.emit = cap_emit, .send = cap_send, .event = cap_event, .cheat = fake_cheat};
    krunker_link_init(&link, &io, KRUNKER_AHKEY);
    clear();
    feed(&link, "tx", (KrunkerValue[]) {KRUNKER_V_STR("Souls"), KRUNKER_V_STR("3 escaped"), KRUNKER_V_BOOL(1), KRUNKER_V_NUM(40)}, 4);
    feed(&link, "stlkpd", NULL, 0);
    feed(&link, "gmsg", (KrunkerValue[]) {KRUNKER_V_STR("infection in"), KRUNKER_V_STR("00:03"), KRUNKER_V_NUM(3)}, 3);
    feed(&link, "gmsg", (KrunkerValue[]) {KRUNKER_V_STR("x"), KRUNKER_V_NIL, KRUNKER_V_NUM(0)}, 3); // 0: no countdown
    feed(&link, "tx", (KrunkerValue[]) {KRUNKER_V_NIL, KRUNKER_V_NIL}, 2);                       // nothing to show
    NetOfficialTextPopupHeader h[3];
    const char *t[3] = {0}, *sub[3] = {0};
    int pops = 0, cds = 0, cd = 0;
    for (int k = 0; find_in(NET_PACKET_EXT, k); k++) {
        uint16_t len = 0;
        const uint8_t *b = ext_body(k, NET_EXT_KN_OFFICIAL_TEXT_POPUP, &len);
        if (b && pops < 3) {
            memcpy(&h[pops], b, sizeof(h[pops]));
            t[pops] = (const char *) b + sizeof(NetOfficialTextPopupHeader);
            sub[pops] = t[pops] + strlen(t[pops]) + 1;
            pops++;
        }
        b = ext_body(k, NET_EXT_KN_OFFICIAL_COUNTDOWN, &len);
        if (b && len == sizeof(NetOfficialCountdownPacket)) {
            memcpy(&cd, b, sizeof(cd));
            cds++;
        }
    }
    CHECK(pops == 2 && !strcmp(t[0], "Souls") && !strcmp(sub[0], "3 escaped") && h[0].silent && h[0].sub_size == 40,
          "tx: title, sub, silent, subSize (%d popups)", pops);
    CHECK(pops == 2 && !strcmp(t[1], "You are the Living") && !strcmp(sub[1], "Hunt the stalker and survive the round!") && !h[1].silent &&
          !h[1].sub_size, "stlkpd: the whole sub line");
    CHECK(cds == 1 && cd == 3, "gmsg countdown 3 only (%d)", cds);
    CHECK(link.unknown_in == 0, "every popup packet was handled (%u unknown)", link.unknown_in);
    clear();
}

// The official lobby UI (official-ingame-lobby): the clock's end states ("t" [fmt, state, ms]), chat tabs
// and colours ("ch" / "chi"), the private lobby's password ("code" / "codeT" / "codeP", our ["code", value])
static void test_lobby(void) {
    KrunkerLink link;
    const KrunkerLinkIO io = {.emit = cap_emit, .send = cap_send, .event = cap_event, .cheat = fake_cheat};
    krunker_link_init(&link, &io, KRUNKER_AHKEY);
    feed(&link, "io-init", (KrunkerValue[]) {KRUNKER_V_STR("sockA")}, 1);
    clear();
    // "t": state 0 a running clock (no state EXT); 1 / 2 / 3 the end screen's, each before its TIMER
    for (int st = 0; st <= 3; st++) {
        feed(&link, "t", (KrunkerValue[]) {KRUNKER_V_STR("0:09"), KRUNKER_V_NUM(st), KRUNKER_V_NUM(9000)}, 3);
        uint16_t len = 0;
        const int k = find_ext(NET_EXT_KN_OFFICIAL_CLOCK_STATE, 0, &len);
        const uint8_t *b = k >= 0 ? ext_body(k, NET_EXT_KN_OFFICIAL_CLOCK_STATE, &len) : NULL;
        const NetPacket *xp = k >= 0 ? find_in(NET_PACKET_EXT, k) : NULL, *tp = find_in(NET_PACKET_TIMER, 0);
        NetTimerPacket t = {0};
        if (tp) memcpy(&t, tp->payload, sizeof(t));
        if (st == 0) CHECK(!b && tp && !t.end && t.seconds == 9, "t state 0: the round clock only");
        else CHECK(b && len == 1 && b[0] == st && xp < tp && t.end == 1, "t state %d: CLOCK_STATE %d before the end TIMER", st, st);
        clear();
    }
    // "ch" kind 0 with nameColor; kind 1 on the global tab; a trade request (kind 3) is not shown
    feed(&link, "ch", (KrunkerValue[]) {KRUNKER_V_NUM(0), KRUNKER_V_STR("Vip"), KRUNKER_V_STR("hi"), KRUNKER_V_NUM(0), KRUNKER_V_NIL,
                                        KRUNKER_V_STR("#ffd700"), KRUNKER_V_NUM(77)}, 7);
    feed(&link, "ch", (KrunkerValue[]) {KRUNKER_V_NUM(0), KRUNKER_V_NIL, KRUNKER_V_STR("a game line"), KRUNKER_V_NUM(1)}, 4);
    feed(&link, "ch", (KrunkerValue[]) {KRUNKER_V_NUM(0), KRUNKER_V_STR("T"), KRUNKER_V_NUM(5), KRUNKER_V_NUM(3)}, 4);
    {
        NetOfficialChatHeader a, g, x;
        const char *an = "", *at = "", *gn = "", *gt = "", *xn = "", *xt = "";
        CHECK(official_chat(0, &a, &an, &at) && a.kind == NET_CHAT_PLAYER && a.channel == CHAT_CHANNEL_GLOBAL && !strcmp(a.name_color, "#ffd700") &&
              a.account_id == 77 && !strcmp(an, "Vip") && !strcmp(at, "hi"), "ch kind 0: nameColor, account, global tab");
        CHECK(official_chat(1, &g, &gn, &gt) && g.kind == NET_CHAT_GAME && g.channel == CHAT_CHANNEL_GLOBAL && !g.color[0] && !gn[0] &&
              !strcmp(gt, "a game line"), "ch kind 1: a game line");
        CHECK(!official_chat(2, &x, &xn, &xt), "ch kind 3: no line");
    }
    clear();
    // "chi" [team, name, key, kind, color, nameColor]: the tab and both colours ride its header
    feed(&link, "chi", (KrunkerValue[]) {KRUNKER_V_NUM(1), KRUNKER_V_STR("Mate"), KRUNKER_V_STR("modes.round.win"), KRUNKER_V_NUM(0),
                                         KRUNKER_V_NIL, KRUNKER_V_STR("#00ffff")}, 6);
    {
        uint16_t len = 0;
        const int k = find_ext(NET_EXT_KN_OFFICIAL_CHAT_I18N, 0, &len);
        const uint8_t *b = k >= 0 ? ext_body(k, NET_EXT_KN_OFFICIAL_CHAT_I18N, &len) : NULL;
        NetOfficialChatI18nHeader h;
        memset(&h, 0, sizeof(h));
        if (b) memcpy(&h, b, sizeof(h));
        CHECK(b && h.channel == CHAT_CHANNEL_TEAM && h.kind == NET_CHAT_PLAYER && !strcmp(h.name_color, "#00ffff"), "chi: team tab, nameColor");
    }
    clear();
    // the password: "code" -> the prompt, "codeT" 4 -> "Submit - 4s", "codeP" -> cleared
    feed(&link, "code", NULL, 0);
    feed(&link, "codeT", (KrunkerValue[]) {KRUNKER_V_NUM(4)}, 1);
    feed(&link, "codeP", NULL, 0);
    {
        uint16_t len = 0;
        const int p0 = find_ext(NET_EXT_KN_OFFICIAL_CODE_PROMPT, 0, &len), t0 = find_ext(NET_EXT_KN_OFFICIAL_CODE_TIMER, 0, &len);
        const uint8_t *tb = t0 >= 0 ? ext_body(t0, NET_EXT_KN_OFFICIAL_CODE_TIMER, &len) : NULL;
        int32_t sec = 0;
        if (tb && len == sizeof(sec)) memcpy(&sec, tb, sizeof(sec));
        const int c0 = find_ext(NET_EXT_KN_OFFICIAL_CODE_PASSED, 0, &len);
        CHECK(p0 == 0 && t0 == 1 && sec == 4 && c0 == 2 && !link.unknown_in, "code / codeT 4 / codeP in order");
    }
    clear();
    // our typed password -> ["code", value]; an empty one sends nothing
    {
        uint8_t ext[NET_EXT_HEADER_SIZE + 16];
        uint16_t n = net_ext_pack(ext, sizeof(ext), NET_EXT_KN_OFFICIAL_GAME_CODE, "hunter2", 8);
        CHECK(krunker_link_send_ours(&link, NET_PACKET_EXT, ext, n) == 1 && g_out_count == 1 && !strcmp(g_out[0].name, "code") &&
              g_out[0].argc == 1 && krunker_str(krunker_frame_arg(&g_out[0], 0)) &&
              !strcmp(krunker_str(krunker_frame_arg(&g_out[0], 0)), "hunter2"), "our password -> [\"code\", \"hunter2\"]");
        clear();
        n = net_ext_pack(ext, sizeof(ext), NET_EXT_KN_OFFICIAL_GAME_CODE, "", 1);
        CHECK(!krunker_link_send_ours(&link, NET_PACKET_EXT, ext, n) && !g_out_count, "an empty password sends nothing");
    }
    clear();
}

// Prop Hunt (official-ingame-prophunt): "0"[21] (propID of a Prop-team player), "up" [sid, propID, skip,
// propSwitches] and "prpR" [sid, propRot, propRotSaved, reset] -> NET_EXT_PROP_UPDATE / _ROTATION; our
// NET_EXT_PROP_PICK / _ROTATE out as "prp" / "prpR" (["prpR", true] for a reset).
static void test_prophunt(void) {
    KrunkerLink link;
    const KrunkerLinkIO io = {.emit = cap_emit, .send = cap_send, .event = cap_event, .cheat = fake_cheat};
    krunker_link_init(&link, &io, KRUNKER_AHKEY);
    feed(&link, "io-init", (KrunkerValue[]) {KRUNKER_V_STR("sockA")}, 1);
    clear();

    // init: Prop Hunt (mode 11, MODE_ID_PROP)
    KrunkerValue init[17];
    for (int i = 0; i < 17; i++) init[i] = KRUNKER_V_NIL;
    init[0] = KRUNKER_V_NUM(10);
    init[1] = KRUNKER_V_NUM(MODE_ID_PROP);
    init[16] = KRUNKER_V_BOOL(0);
    feed(&link, "init", init, 17);
    clear();

    // us (9) on team 1, the Prop team; a Seeker (7, team 2)
    KrunkerValue add[2 * 53];
    for (int i = 0; i < 2 * 53; i++) add[i] = KRUNKER_V_NIL;
    add[0] = KRUNKER_V_STR("sockA"); add[1] = KRUNKER_V_NUM(9); add[8] = KRUNKER_V_NUM(100); add[9] = KRUNKER_V_NUM(1);
    add[53] = KRUNKER_V_STR("sockB"); add[54] = KRUNKER_V_NUM(7); add[61] = KRUNKER_V_NUM(100); add[62] = KRUNKER_V_NUM(2);
    feed(&link, "0", (KrunkerValue[]) {KRUNKER_V_ARRAY(add, 2 * 53)}, 1);
    CHECK(link.my_sid == 9, "prop: joined as 9");
    {
        uint16_t len = 0;
        const int k = find_ext(NET_EXT_PROP_UPDATE, 0, &len);
        const uint8_t *b = k >= 0 ? ext_body(k, NET_EXT_PROP_UPDATE, &len) : NULL;
        NetPropUpdatePacket u = {0};
        if (b && len == sizeof(u)) memcpy(&u, b, sizeof(u));
        CHECK(b && u.uid == 9 && u.prop_id == 1 && u.skip == 0 && u.switches == -1, "spawn: the Prop-team player wears the default prop");
        CHECK(find_ext(NET_EXT_PROP_UPDATE, 1, &len) < 0, "spawn: a Seeker gets no prop");
    }
    clear();

    // "up" for a Seeker-free game: prop 12 with 2 switches left; then a skip (no prop applied, switches -1)
    feed(&link, "up", (KrunkerValue[]) {KRUNKER_V_NUM(7), KRUNKER_V_NUM(12), KRUNKER_V_NIL, KRUNKER_V_NUM(2)}, 4);
    feed(&link, "up", (KrunkerValue[]) {KRUNKER_V_NUM(9), KRUNKER_V_NUM(3), KRUNKER_V_BOOL(1), KRUNKER_V_NIL}, 4);
    {
        uint16_t len = 0;
        const int k = find_ext(NET_EXT_PROP_UPDATE, 0, &len);
        const uint8_t *b = k >= 0 ? ext_body(k, NET_EXT_PROP_UPDATE, &len) : NULL;
        NetPropUpdatePacket u = {0};
        if (b && len == sizeof(u)) memcpy(&u, b, sizeof(u));
        CHECK(b && u.uid == 7 && u.prop_id == 12 && u.skip == 0 && u.switches == 2, "up: 7 wears prop 12 with 2 switches");
        const int k2 = find_ext(NET_EXT_PROP_UPDATE, 1, &len);
        const uint8_t *b2 = k2 >= 0 ? ext_body(k2, NET_EXT_PROP_UPDATE, &len) : NULL;
        NetPropUpdatePacket u2 = {0};
        if (b2 && len == sizeof(u2)) memcpy(&u2, b2, sizeof(u2));
        CHECK(b2 && u2.uid == 9 && u2.prop_id == 3 && u2.skip == 1 && u2.switches == -1, "up: a skip applies nothing but the switches");
    }
    clear();

    // "prpR" [sid, propRot, propRotSaved, reset]: rotation on with a saved yaw 0.5, then a reset
    feed(&link, "prpR", (KrunkerValue[]) {KRUNKER_V_NUM(7), KRUNKER_V_BOOL(1), KRUNKER_V_NUM(0.5), KRUNKER_V_BOOL(0)}, 4);
    feed(&link, "prpR", (KrunkerValue[]) {KRUNKER_V_NUM(7), KRUNKER_V_BOOL(0), KRUNKER_V_NIL, KRUNKER_V_BOOL(1)}, 4);
    {
        uint16_t len = 0;
        const int k = find_ext(NET_EXT_PROP_ROTATION, 0, &len);
        const uint8_t *b = k >= 0 ? ext_body(k, NET_EXT_PROP_ROTATION, &len) : NULL;
        NetPropRotationPacket r = {0};
        if (b && len == sizeof(r)) memcpy(&r, b, sizeof(r));
        CHECK(b && r.uid == 7 && r.rot == 1 && r.saved_set == 1 && r.saved == 0.5f && r.reset == 0, "prpR: rotation on, saved yaw 0.5");
        const int k2 = find_ext(NET_EXT_PROP_ROTATION, 1, &len);
        const uint8_t *b2 = k2 >= 0 ? ext_body(k2, NET_EXT_PROP_ROTATION, &len) : NULL;
        NetPropRotationPacket r2 = {0};
        if (b2 && len == sizeof(r2)) memcpy(&r2, b2, sizeof(r2));
        CHECK(b2 && r2.rot == 0 && r2.saved_set == 0 && r2.reset == 1, "prpR: the reset with no saved yaw");
    }
    clear();

    // outbound: the wheel's pick -> "prp" [propID]; the rotation toggle -> "prpR" []; the reset -> "prpR" [true]
    {
        uint8_t ext[64];
        const NetPropPickPacket pick = {.prop_id = 12};
        uint16_t n = net_ext_pack(ext, sizeof(ext), NET_EXT_PROP_PICK, &pick, sizeof(pick));
        krunker_link_send_ours(&link, NET_PACKET_EXT, ext, n);
        CHECK(g_out_count == 1 && !strcmp(g_out[0].name, "prp") && krunker_num(krunker_frame_arg(&g_out[0], 0), 0) == 12, "prp out: prop 12");
        clear();
        const NetPropRotatePacket toggle = {.reset = 0};
        n = net_ext_pack(ext, sizeof(ext), NET_EXT_PROP_ROTATE, &toggle, sizeof(toggle));
        krunker_link_send_ours(&link, NET_PACKET_EXT, ext, n);
        CHECK(g_out_count == 1 && !strcmp(g_out[0].name, "prpR") && g_out[0].argc == 0, "prpR out: a toggle sends no argument");
        clear();
        const NetPropRotatePacket reset = {.reset = 1};
        n = net_ext_pack(ext, sizeof(ext), NET_EXT_PROP_ROTATE, &reset, sizeof(reset));
        krunker_link_send_ours(&link, NET_PACKET_EXT, ext, n);
        CHECK(g_out_count == 1 && !strcmp(g_out[0].name, "prpR") && g_out[0].argc == 1, "prpR out: a reset sends true");
        clear();
    }
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
    { // dmgType "Float": the exact 24.6 rides ahead of the HIT (NET_EXT_KN_HIT_DAMAGE)
        const NetPacket *x = NULL;
        uint16_t sub = 0, blen = 0;
        const uint8_t *body = NULL;
        NetHitDamagePacket exact = {0};
        for (int i = 0; (x = find_in(NET_PACKET_EXT, i)); i++) {
            if (net_ext_unpack(x->payload, x->length, &sub, &body, &blen) && sub == NET_EXT_KN_HIT_DAMAGE && blen == sizeof(exact)) {
                memcpy(&exact, body, sizeof(exact));
                break;
            }
        }
        CHECK(x && exact.target_uid == 7 && exact.damage_tenths == 246, "HIT exact damage 24.6 (EXT)");
    }
    const KrunkerValue info[] = {KRUNKER_V_STR("wId"), KRUNKER_V_NUM(3), KRUNKER_V_STR("fs"), KRUNKER_V_BOOL(0)};
    feed(&link, "3", (KrunkerValue[]) {KRUNKER_V_NUM(7), KRUNKER_V_NUM(2), KRUNKER_V_NUM(9), KRUNKER_V_NIL, KRUNKER_V_MAP(info, 2)}, 5);
    NetKillPacket kill;
    p = find_in(NET_PACKET_KILL, 0);
    if (p) memcpy(&kill, p->payload, sizeof(kill));
    CHECK(p && kill.victim_uid == 7 && kill.killer_uid == 9 && kill.weapon_index == 3 && kill.victim_deaths == 2, "KILL");
    feed(&link, "ch", (KrunkerValue[]) {KRUNKER_V_NUM(-1), KRUNKER_V_STR("Other"), KRUNKER_V_STR("hello there"), KRUNKER_V_NUM(0)}, 4);
    {
        NetOfficialChatHeader ch;
        const char *cn = "", *ct = "";
        CHECK(official_chat(0, &ch, &cn, &ct) && !strcmp(ct, "hello there") && !strcmp(cn, "Other") && ch.kind == NET_CHAT_PLAYER &&
              ch.channel == CHAT_CHANNEL_ALL && !find_in(NET_PACKET_CHAT, 0) && !find_in(NET_PACKET_CHAT_V7, 0), "CHAT"); // channel -1: every tab
    }
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
          krunker_num(krunker_at(enter, 26), 0) == -1 && krunker_num(krunker_at(enter, 28), -1) == 0 && // no STREAK_SLOTS: v10's clean defaults
          krunker_num(krunker_at(enter, 13), 0) == -1 && krunker_num(krunker_at(enter, 8), 0) == 1 && fnum(&g_out[0], 1) == 16 && fnum(&g_out[0], 2) == 18 &&
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

    // rankedPoints: v10's row[47] (Account.setData, v10:112873). The row as an array (index 47) or as a map
    // with the "47" key (JS reads both); "ua" (UPDATE_ACCOUNT) carries the same row; a row without it changes nothing
    {
        KrunkerValue arr[48];
        for (int i = 0; i < 48; i++) arr[i] = KRUNKER_V_NUM(0);
        arr[47] = KRUNKER_V_NUM(777);
        feed(&link, "a", (KrunkerValue[]) {KRUNKER_V_NIL, KRUNKER_V_NUM(123), KRUNKER_V_STR("KioPlayer"), KRUNKER_V_ARRAY(arr, 48), KRUNKER_V_STR("legacy")}, 5);
        CHECK(link.account_rp_known == 1 && link.account_ranked_points == 777, "a: rankedPoints is row[47] of the array row (%.0f)", link.account_ranked_points);
        clear();
        const KrunkerValue keyed[] = {KRUNKER_V_STR("player_score"), KRUNKER_V_NUM(10), KRUNKER_V_STR("47"), KRUNKER_V_NUM(4242)};
        feed(&link, "a", (KrunkerValue[]) {KRUNKER_V_NIL, KRUNKER_V_NUM(123), KRUNKER_V_STR("KioPlayer"), KRUNKER_V_MAP(keyed, 2), KRUNKER_V_STR("legacy")}, 5);
        CHECK(link.account_ranked_points == 4242 && g_events[KRUNKER_LINK_EV_ACCOUNT] == 1, "a: rankedPoints is the \"47\" key of a map row (%.0f)", link.account_ranked_points);
        clear();
        arr[47] = KRUNKER_V_NUM(999);
        feed(&link, "ua", (KrunkerValue[]) {KRUNKER_V_ARRAY(arr, 48)}, 1);
        CHECK(g_events[KRUNKER_LINK_EV_ACCOUNT_POINTS] == 1 && link.account_ranked_points == 999 && g_out_count == 0,
              "ua: the row's rankedPoints replaces the balance (%.0f)", link.account_ranked_points);
        clear();
        const KrunkerValue short_row[] = {KRUNKER_V_NUM(1), KRUNKER_V_NUM(2)};
        feed(&link, "ua", (KrunkerValue[]) {KRUNKER_V_ARRAY(short_row, 2)}, 1);
        CHECK(g_events[KRUNKER_LINK_EV_ACCOUNT_POINTS] == 0 && link.account_ranked_points == 999, "ua without row[47]: the balance is kept");
        clear();
        feed(&link, "a", (KrunkerValue[]) {KRUNKER_V_STR("Invalid session"), KRUNKER_V_NIL, KRUNKER_V_NIL}, 3);
        clear();
    }

    // the wallet packets: "ub" (v10 :208200) balances[3].amount is the ranked points; "chlR" (v10 :208212-208248)
    // gives the balance after the challenges' ranked points (argument 6 set; argument 7 the balance)
    {
        const KrunkerValue rp_map[] = {KRUNKER_V_STR("amount"), KRUNKER_V_NUM(5000), KRUNKER_V_STR("change"), KRUNKER_V_NUM(100)};
        const KrunkerValue balances[] = {KRUNKER_V_NUM(1), KRUNKER_V_NUM(2), KRUNKER_V_NUM(3), KRUNKER_V_MAP(rp_map, 2)};
        feed(&link, "ub", (KrunkerValue[]) {KRUNKER_V_ARRAY(balances, 4)}, 1);
        CHECK(link.account_rp_known && link.account_ranked_points == 5000 && g_events[KRUNKER_LINK_EV_ACCOUNT_POINTS] == 1,
              "ub: balances[3].amount is the balance (%.0f)", link.account_ranked_points);
        clear();
        const KrunkerValue chl[] = {KRUNKER_V_NUM(0), KRUNKER_V_NUM(0), KRUNKER_V_NUM(0), KRUNKER_V_NUM(0), KRUNKER_V_NUM(0),
                                    KRUNKER_V_NUM(0), KRUNKER_V_NUM(50), KRUNKER_V_NUM(7777)};
        feed(&link, "chlR", chl, 8);
        CHECK(link.account_ranked_points == 7777, "chlR: the ranked points won set the balance to argument 7 (%.0f)", link.account_ranked_points);
        clear();
        KrunkerValue none[8];
        for (int i = 0; i < 8; i++) none[i] = KRUNKER_V_NUM(0);
        none[7] = KRUNKER_V_NUM(1);
        feed(&link, "chlR", none, 8);
        CHECK(link.account_ranked_points == 7777, "chlR without ranked points: the balance is kept");
        clear();
    }

    // the level from an array row (v10's setData: score is row[6], v10:112832; updateLevel: max(1, floor(0.03 * sqrt(score))),
    // v10:112893): 40000 -> 6, 0 -> 1 (the map form, player_score, gives the same level)
    {
        KrunkerValue row[55];
        for (int i = 0; i < 55; i++) row[i] = KRUNKER_V_NUM(0);
        row[6] = KRUNKER_V_NUM(40000);
        feed(&link, "a", (KrunkerValue[]) {KRUNKER_V_NIL, KRUNKER_V_NUM(123), KRUNKER_V_STR("ArrPlayer"), KRUNKER_V_ARRAY(row, 55), KRUNKER_V_STR("legacy")}, 5);
        CHECK(g_events[KRUNKER_LINK_EV_ACCOUNT] == 1 && link.account_level == 6, "a: level from row[6] of an array row (%d)", link.account_level);
        clear();
        row[6] = KRUNKER_V_NUM(0);
        feed(&link, "a", (KrunkerValue[]) {KRUNKER_V_NIL, KRUNKER_V_NUM(123), KRUNKER_V_STR("ArrPlayer"), KRUNKER_V_ARRAY(row, 55), KRUNKER_V_STR("legacy")}, 5);
        CHECK(link.account_level == 1, "a: an array row with no score is level 1 (%d)", link.account_level);
        clear();
    }

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
    test_world();
    test_rounds();
    test_actions();
    test_social();
    test_voice();

    { // a live build (krunker_build_live.h): its packet names, AHKEY, strides, offsets and argument positions
        KrunkerBuild *nb = next_build();
        CHECK(nb != NULL, "the next build parses");
        if (nb) {
            krunker_build_use(nb);
            KrunkerLink lv;
            krunker_link_init(&lv, &io, krunker_build()->ahkey);
            clear();
            feed(&lv, "io-init", (KrunkerValue[]) {KRUNKER_V_STR("sockL")}, 1);
            feed(&lv, "load", NULL, 0);
            CHECK(g_out_count == 1 && !strcmp(g_out[0].name, "Aload") && g_out[0].ah_echo == 0x17, "load answered as Aload, ahNum 0x17 (%s %d)",
                  g_out_count ? g_out[0].name : "-", g_out_count ? g_out[0].ah_echo : -1);
            clear();
            // "A0": 60 values per player, id at 58, sid at 55, name at 56, x at 57 (y / z where they were)
            KrunkerValue e[120];
            for (int i = 0; i < 120; i++) e[i] = KRUNKER_V_NIL;
            e[58] = KRUNKER_V_STR("sockL"); e[55] = KRUNKER_V_NUM(31); e[56] = KRUNKER_V_STR("Me"); e[57] = KRUNKER_V_NUM(12.5);
            e[KRUNKER_ADD_Z] = KRUNKER_V_NUM(-3); e[KRUNKER_ADD_MAX_HEALTH] = KRUNKER_V_NUM(100);
            e[60 + 58] = KRUNKER_V_STR("sockO"); e[60 + 55] = KRUNKER_V_NUM(32); e[60 + 56] = KRUNKER_V_STR("Other");
            e[60 + 57] = KRUNKER_V_NUM(7);
            const KrunkerValue add_args[] = {KRUNKER_V_ARRAY(e, 120)};
            feed(&lv, "A0", add_args, 1);
            const NetPacket *sp0 = find_in(NET_PACKET_SPAWN, 0), *sp1 = find_in(NET_PACKET_SPAWN, 1);
            NetSpawnPacket s0, s1;
            memset(&s0, 0, sizeof(s0));
            memset(&s1, 0, sizeof(s1));
            if (sp0) memcpy(&s0, sp0->payload, sizeof(s0));
            if (sp1) memcpy(&s1, sp1->payload, sizeof(s1));
            CHECK(sp0 && s0.uid == 31 && s0.is_you && !strcmp(s0.name, "Me") && s0.position[0] == 12.5f && s0.position[2] == -3.0f,
                  "A0 player 1 from the new offsets (uid %d you %d name %s x %.1f)", s0.uid, s0.is_you, s0.name, s0.position[0]);
            CHECK(sp1 && s1.uid == 32 && !s1.is_you && !strcmp(s1.name, "Other") && s1.position[0] == 7.0f, "A0 player 2 (stride 60)");
            CHECK(lv.my_sid == 31, "joined as sid 31 (%d)", lv.my_sid);
            clear();
            // the old wire names are no packet in this build
            KrunkerValue old[KRUNKER_ADD_STRIDE];
            for (int i = 0; i < KRUNKER_ADD_STRIDE; i++) old[i] = KRUNKER_V_NIL;
            old[KRUNKER_ADD_SID] = KRUNKER_V_NUM(40);
            const KrunkerValue old_args[] = {KRUNKER_V_ARRAY(old, KRUNKER_ADD_STRIDE)};
            feed(&lv, "0", old_args, 1);
            CHECK(!find_in(NET_PACKET_SPAWN, 0), "\"0\" is ignored in this build");
            clear();
            // "Ak": 15 values per player, sid at 13, x at 14
            KrunkerValue ke[15];
            for (int i = 0; i < 15; i++) ke[i] = KRUNKER_V_NUM(0);
            ke[13] = KRUNKER_V_NUM(32); ke[14] = KRUNKER_V_NUM(9.25); ke[KRUNKER_K_Z] = KRUNKER_V_NUM(4);
            const KrunkerValue k_args[] = {KRUNKER_V_ARRAY(ke, 15), KRUNKER_V_NUM(10), KRUNKER_V_NUM(0)};
            feed(&lv, "Ak", k_args, 3);
            const NetPacket *st = find_in(NET_PACKET_STATE, 0);
            NetStatePacket sv;
            memset(&sv, 0, sizeof(sv));
            if (st) memcpy(&sv, st->payload, sizeof(sv));
            CHECK(st && sv.uid == 32 && sv.position[0] == 9.25f && sv.position[2] == 4.0f, "Ak from the new offsets (uid %d x %.2f)", sv.uid,
                  sv.position[0]);
            clear();
            // "A3": victim and killer swapped places
            feed(&lv, "A3", (KrunkerValue[]) {KRUNKER_V_NUM(31), KRUNKER_V_NUM(0), KRUNKER_V_NUM(32), KRUNKER_V_NIL, KRUNKER_V_NUM(2)}, 5);
            const NetPacket *kp = find_in(NET_PACKET_KILL, 0);
            NetKillPacket kv;
            memset(&kv, 0, sizeof(kv));
            if (kp) memcpy(&kv, kp->payload, sizeof(kv));
            CHECK(kp && kv.victim_uid == 32 && kv.killer_uid == 31, "A3 victim at 2, killer at 0 (%d %d)", kv.victim_uid, kv.killer_uid);
            clear();
            krunker_build_use(NULL);
            krunker_build_free(nb);
        }
        clear();
    }
    test_popups();
    test_lobby();
    test_prophunt();
    test_script();

    printf("%d/%d passed\n", g_pass, g_pass + g_fail);
    return g_fail ? 1 : 0;
}
