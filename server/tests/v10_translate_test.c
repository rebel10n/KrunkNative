// v10 <-> native packet mappings (server/bridge/v10_translate.c): the C port of the pure part of
// net-bridge/v10Translate.test.ts's fixtures, the way net-bridge/v10Proxy.test.ts builds them (its
// layout blocks, before its socket part), plus the msgpack codec of the v10 frames against bytes the
// TS codec (net-bridge/utils.ts, msgpack-lite) produced. Each CHECK names the TS assertion it ports.
// Prints "v10_translate_test: N checks, M failed" and exits 1 on any failure.
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "krunker_proto.h"
#include "v10_translate.h"


static int g_checks, g_failures;
#define T_EN_LENGTH_V10 35 // the v10 "en" array (v10.js 208129)

static int32_t rd(const uint8_t *b, size_t at) { return (int32_t)((uint32_t)b[at] | (uint32_t)b[at + 1] << 8 | (uint32_t)b[at + 2] << 16 | (uint32_t)b[at + 3] << 24); }
static float rdf(const uint8_t *b, size_t at) { uint32_t u = (uint32_t)rd(b, at); float f; memcpy(&f, &u, 4); return f; }
static V10Out g_state_out; // keeps the arena of the state records the checks read
static KrunkerValue state_rec_value(const V10StateRec *r) { return v10_state_record(&g_state_out, r); }

#define CHECK(cond, ...) do { \
    g_checks++; \
    if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_failures++; } \
} while (0)

// ---- expected values, built with the KRUNKER_V_* macros; vequal compares exactly (doubles included)

static KrunkerValue kv_arr(int n, const KrunkerValue *items) {
    KrunkerValue *copy = malloc(sizeof(KrunkerValue) * (size_t)(n ? n : 1));
    if (n) memcpy(copy, items, sizeof(KrunkerValue) * (size_t)n);
    return KRUNKER_V_ARRAY(copy, (uint32_t)n);
}
static KrunkerValue kv_map(int n, const char *const *keys, const KrunkerValue *vals) {
    KrunkerValue *kv = malloc(sizeof(KrunkerValue) * (size_t)(2 * (n ? n : 1)));
    for (int i = 0; i < n; i++) {
        kv[2 * i] = KRUNKER_V_STR(keys[i]);
        kv[2 * i + 1] = vals[i];
    }
    return KRUNKER_V_MAP(kv, (uint32_t)n);
}
#define ARR(...) kv_arr((int)(sizeof((KrunkerValue[]){__VA_ARGS__}) / sizeof(KrunkerValue)), (KrunkerValue[]){__VA_ARGS__})
#define N(x) KRUNKER_V_NUM(x)
#define S(s) KRUNKER_V_STR(s)
#define B(b) KRUNKER_V_BOOL(b)
#define NIL KRUNKER_V_NIL

static int vequal(const KrunkerValue *a, const KrunkerValue *b) {
    if (!a || !b) return a == b;
    if (a->type != b->type) return 0;
    switch ((KrunkerValueType)a->type) {
    case KRUNKER_NIL: return 1;
    case KRUNKER_BOOL: return (a->u.boolean != 0) == (b->u.boolean != 0);
    case KRUNKER_NUM: return a->u.num == b->u.num || (isnan(a->u.num) && isnan(b->u.num));
    case KRUNKER_STR: return a->len == b->len && memcmp(a->u.str, b->u.str, a->len) == 0;
    case KRUNKER_ARRAY:
        if (a->len != b->len) return 0;
        for (uint32_t i = 0; i < a->len; i++)
            if (!vequal(&a->u.items[i], &b->u.items[i])) return 0;
        return 1;
    case KRUNKER_MAP:
        if (a->len != b->len) return 0;
        for (uint32_t i = 0; i < 2 * a->len; i++)
            if (!vequal(&a->u.items[i], &b->u.items[i])) return 0;
        return 1;
    default: return 0;
    }
}

// the message `idx` of o: its label and args equal the expected ones
static int msg_is(const V10Out *o, size_t idx, const char *label, const KrunkerValue *want, uint32_t wantc) {
    if (idx >= o->count) return 0;
    const V10Msg *m = &o->msgs[idx];
    if (strcmp(m->label, label) || m->argc != wantc) return 0;
    for (uint32_t i = 0; i < wantc; i++)
        if (!vequal(&m->args[i], &want[i])) return 0;
    return 1;
}
#define MSG_IS(o, idx, label, ...) do { \
    const KrunkerValue want_[] = {__VA_ARGS__}; \
    CHECK(msg_is((o), (idx), (label), want_, sizeof want_ / sizeof want_[0]), "message %zu is %s with the TS arguments", (size_t)(idx), (label)); \
} while (0)

static const KrunkerValue *map_get_v(const KrunkerValue *m, const char *key) {
    for (uint32_t i = 0; m && m->type == KRUNKER_MAP && i < m->len; i++) {
        const KrunkerValue *k = &m->u.items[2 * i];
        if (k->len == strlen(key) && !memcmp(k->u.str, key, k->len)) return &m->u.items[2 * i + 1];
    }
    return NULL;
}
static int stop_has(const V10Out *o, const char *part) { return o->stop && strstr(o->stop, part) != NULL; }

// ---- the TS buffer builders (v10Proxy.test.ts): little endian, the layouts of shared.h

static void put_i32(uint8_t *b, size_t at, int32_t v) { uint32_t u = (uint32_t)v; for (int i = 0; i < 4; i++) b[at + i] = (uint8_t)(u >> (8 * i)); }
static void put_u16(uint8_t *b, size_t at, uint16_t v) { b[at] = (uint8_t)v; b[at + 1] = (uint8_t)(v >> 8); }
static void put_f32(uint8_t *b, size_t at, float v) { uint32_t u; memcpy(&u, &v, 4); put_i32(b, at, (int32_t)u); }
static void put_f64(uint8_t *b, size_t at, double v) { uint64_t u; memcpy(&u, &v, 8); put_i32(b, at, (int32_t)(uint32_t)u); put_i32(b, at + 4, (int32_t)(uint32_t)(u >> 32)); }
static void put_str(uint8_t *b, size_t at, const char *s, size_t cap) { size_t n = strlen(s); if (n > cap) n = cap; memcpy(b + at, s, n); }

typedef struct { int32_t x; } Unused;

static void mode_state(uint8_t *b, int32_t team0, int32_t team1, int32_t lives, int locked, const char *message, int32_t objective,
                       int32_t move_lock, int32_t objective_weapon /* -1 none */) {
    memset(b, 0, 387);
    put_i32(b, 0, 0);
    put_i32(b, 4, team0);
    put_i32(b, 8, team1);
    put_str(b, 12, "AT BASE", 32);
    put_str(b, 44, "AT BASE", 32);
    put_i32(b, 76, objective);
    put_i32(b, 80, objective_weapon);
    b[84] = (uint8_t)move_lock;
    b[85] = (uint8_t)locked;
    put_i32(b, 86, lives);
    put_str(b, 291, message, 96);
}
typedef struct { float x, y, z; int32_t carrier, team, uid; int active; } Flag;
static void mode_state_flags(uint8_t *b, const Flag *flags, int n) {
    b[90] = (uint8_t)n;
    for (int f = 0; f < n; f++) {
        const size_t at = 91 + (size_t)f * 25;
        put_f32(b, at, flags[f].x);
        put_f32(b, at + 4, flags[f].y);
        put_f32(b, at + 8, flags[f].z);
        put_i32(b, at + 12, flags[f].carrier);
        put_i32(b, at + 16, flags[f].team);
        put_i32(b, at + 20, flags[f].uid);
        b[at + 24] = flags[f].active ? 1 : 0;
    }
}
static void object_state(uint8_t *b, int32_t uid, uint8_t kind, int32_t value) { put_i32(b, 0, uid); b[4] = kind; put_i32(b, 5, value); }

typedef struct {
    int32_t uid; int is_you, active, class_index, team; float health; int32_t max_health;
    float pos[3]; float dir[2]; const char *name; int32_t secondary;
    int32_t hat, body, hair, skin_color, dye, waist, face, head, back, shoe, pet, wrist, charm0, charm1, player_card;
    float head_mlt;
} TSpawn;
static void spawn_buf(uint8_t *b, const TSpawn *s) {
    memset(b, 0, 150);
    put_i32(b, 0, s->uid);
    b[4] = s->is_you; b[5] = s->active;
    put_i32(b, 6, s->class_index);
    put_i32(b, 10, s->team);
    put_f32(b, 14, s->health);
    put_i32(b, 18, s->max_health);
    for (int i = 0; i < 3; i++) put_f32(b, 22 + 4 * (size_t)i, s->pos[i]);
    for (int i = 0; i < 2; i++) put_f32(b, 34 + 4 * (size_t)i, s->dir[i]);
    put_str(b, 42, s->name, 32);
    put_i32(b, 74, s->secondary);
    const int32_t cos[7] = {s->skin_color, s->hair, s->hat, s->body, s->waist, s->face, s->dye};
    for (int i = 0; i < 7; i++) put_i32(b, 78 + 4 * (size_t)i, cos[i]);
    put_i32(b, 106, -1);
    put_f32(b, 110, s->head_mlt);
    const int32_t v7[9] = {s->head, s->back, s->shoe, s->pet, s->wrist, s->charm0, s->charm1, s->player_card, -1};
    for (int i = 0; i < 9; i++) put_i32(b, 114 + 4 * (size_t)i, v7[i]);
}
static TSpawn default_spawn(void) {
    TSpawn s;
    memset(&s, 0, sizeof s);
    s.is_you = 0; s.active = 1; s.health = 100; s.max_health = 100; s.head_mlt = 1;
    s.hat = s.body = s.hair = s.skin_color = s.dye = s.waist = s.face = -1;
    s.head = s.back = s.shoe = s.pet = s.wrist = s.charm0 = s.charm1 = s.player_card = -1;
    s.secondary = -1;
    return s;
}


int main(void) {
    // ---- layout of the translated ENT (v10Proxy.test.ts 38-100)
    {
        KrunkerValue en[T_EN_LENGTH_V10];
        for (int i = 0; i < T_EN_LENGTH_V10; i++) en[i] = N(-1);
        en[0] = N(2);                       // EN.CLASS
        en[5] = N(5);                       // SECONDARY
        en[1] = N(7);                       // SPRAY
        en[2] = ARR(N(11), N(12));          // SKINS
        en[9] = N(13);                      // MELEE
        en[11] = N(0.5);                    // LAG_COMP
        en[12] = N(-1);                     // ATTACH
        en[6] = N(0);                       // SPECTATE
        en[7] = N(1);                       // CHALLENGE
        en[19] = N(1);                      // ANON
        en[3] = N(3);                       // HAT
        en[4] = N(4);                       // BODY
        en[14] = N(6);                      // DYE
        en[16] = N(8);                      // WAIST
        en[20] = N(9);                      // FACE
        en[10] = N(10);                     // SKIN_COLOR
        en[17] = N(-1);                     // HAIR
        en[22] = N(1);                      // VERIFIED
        en[23] = N(0);                      // PREMIUM
        en[24] = N(1);                      // CUSTOM_BADGE
        en[15] = N(15);                     // SHOE
        en[21] = N(21);                     // PET
        en[25] = N(25);                     // WRIST
        en[29] = ARR(N(30), N(31));         // CHARMS
        en[30] = N(33);                     // BACK
        en[31] = N(34);                     // HEAD
        en[32] = N(35);                     // PLAYER_CARD
        uint8_t ent[V10_ENT_SIZE];
        CHECK(v10_encode_enter(&KRUNKER_V_ARRAY(en, T_EN_LENGTH_V10), ent), "encodeEnter of a v10 array");
        CHECK(V10_ENT_SIZE == 101, "ENT_V9_SIZE is 101");
        CHECK(rd(ent, 0) == 2, "ENT class");
        CHECK(rd(ent, 4) == 5, "ENT secondary");
        CHECK(rd(ent, 16) == 3, "ENT hat (cosmetics block at 8)");
        CHECK(rd(ent, 20) == 4, "ENT body (back slot)");
        CHECK(rd(ent, 24) == 8, "ENT waist");
        CHECK(rd(ent, 28) == 9, "ENT face");
        CHECK(rd(ent, 32) == 6, "ENT dye");
        CHECK(ent[36] == 0, "ENT spectate");
        CHECK(ent[37] == 1, "ENT slidCont: default");
        CHECK(rd(ent, 38) == 7, "ENT spray");
        CHECK(rd(ent, 42) == 11, "ENT primary skin");
        CHECK(rd(ent, 46) == 12, "ENT secondary skin");
        CHECK(rd(ent, 50) == 13, "ENT melee skin");
        CHECK(fabsf(rdf(ent, 54) - 0.5f) < 1e-6f, "ENT lag comp");
        CHECK(rd(ent, 58) == -1, "ENT attach");
        CHECK(ent[62] == 1, "ENT challMode");
        CHECK(ent[63] == 1, "ENT anonMode");
        CHECK(rd(ent, 64) == 34, "ENT v7 head");
        CHECK(rd(ent, 68) == 33, "ENT v7 back (en[30] is 33)");
        CHECK(rd(ent, 72) == 15, "ENT v7 shoe");
        CHECK(rd(ent, 76) == 21, "ENT v7 pet");
        CHECK(rd(ent, 80) == 25, "ENT v7 wrist");
        CHECK(rd(ent, 84) == 30, "ENT v7 charm 0");
        CHECK(rd(ent, 88) == 31, "ENT v7 charm 1");
        CHECK(rd(ent, 92) == 35, "ENT v7 player card");
        CHECK(rd(ent, 96) == -1, "ENT v7 badge");
        CHECK(ent[100] == 0x09, "ENT show badges: verified (bit 0) + custom (bit 3)");

        // PICKUP (128): NetPickupPacket (26 bytes) -> inat [-1, null, null, uid, weapon|null, true, active, pos|null]
        {
            uint8_t pk[26];
            memset(pk, 0, sizeof pk);
            put_i32(pk, 0, 7); put_i32(pk, 4, 3); pk[8] = 1; pk[9] = 1;
            put_f32(pk, 10, 1.5f); put_f32(pk, 14, 2); put_f32(pk, 18, -3); put_i32(pk, 22, 12);
            V10Out po;
            v10_out_init(&po);
            v10_map_pickup(&po, pk, 26);
            MSG_IS(&po, 0, "inat", N(-1), NIL, NIL, N(7), N(3), B(1), B(1), ARR(N(1.5), N(2), N(-3)));
            v10_out_free(&po);
            put_i32(pk, 4, -1); pk[8] = 0; pk[9] = 0;
            v10_out_init(&po);
            v10_map_pickup(&po, pk, 26);
            MSG_IS(&po, 0, "inat", N(-1), NIL, NIL, N(7), NIL, B(1), B(0), NIL); /* an empty pickup: null weapon (173166) */
            v10_out_free(&po);
            v10_out_init(&po);
            v10_map_pickup(&po, pk, 25);
            CHECK(stop_has(&po, "malformed"), "a short pickup is malformed");
            v10_out_free(&po);
            put_i32(pk, 0, -1); /* mapPickup has no uid check: the message goes as it is */
            v10_out_init(&po);
            v10_map_pickup(&po, pk, 26);
            CHECK(!po.stop && po.count == 1, "a pickup with uid -1 is sent as the TS sends it");
            v10_out_free(&po);
        }

        // NET 9 (NetChatPacket, 325 bytes): the kinds of the v7 line, channel -1 and account null (mapChat)
        {
            uint8_t c9[325];
            V10Out co;
            memset(c9, 0, sizeof c9);
            c9[4] = 2;
            put_str(c9, 37, "Round 2 starts", 288);
            v10_out_init(&co);
            v10_map_chat(&co, c9, 325);
            MSG_IS(&co, 0, "ch", N(-1), NIL, S("Round 2 starts"), N(2), NIL, NIL, NIL, NIL);
            v10_out_free(&co);
            memset(c9, 0, sizeof c9);
            put_i32(c9, 0, 3);
            c9[4] = 0;
            put_str(c9, 5, "Ada", 32);
            put_str(c9, 37, "hi <b>", 288);
            v10_out_init(&co);
            v10_map_chat(&co, c9, 325);
            MSG_IS(&co, 0, "ch", N(-1), S("Ada"), S("hi &lt;b&gt;"), N(0), NIL, NIL, NIL, NIL);
            v10_out_free(&co);
            memset(c9, 0, sizeof c9);
            c9[4] = 3;
            put_str(c9, 37, "You found [1.25] Junk", 288);
            v10_out_init(&co);
            v10_map_chat(&co, c9, 325);
            MSG_IS(&co, 0, "ch", N(-1), NIL, S("<span style='color:#fff'>You</span> found <span style='color:#cc51f8'>[1.25]</span> Junk"), N(1));
            v10_out_free(&co);
            c9[4] = 7;
            v10_out_init(&co);
            v10_map_chat(&co, c9, 325);
            CHECK(stop_has(&co, "chat kind 7"), "an unknown NET 9 kind stops");
            v10_out_free(&co);
            v10_out_init(&co);
            v10_map_chat(&co, c9, 324);
            CHECK(stop_has(&co, "malformed"), "a short NET 9 chat is malformed");
            v10_out_free(&co);
        }

        // MATCH_VOTE_STATE (170 bytes): mv [vo, mts]; ours under the socket id, the others under vote<i> (mapMatchVoteState)
        {
            uint8_t mv[170];
            V10Out mo;
            memset(mv, 0, sizeof mv);
            mv[0] = 2; mv[1] = 1;
            put_i32(mv, 2, 0); put_i32(mv, 6, 5); put_u16(mv, 10, 2);
            put_i32(mv, 44, 1); put_i32(mv, 48, 9); put_u16(mv, 52, 1);
            v10_out_init(&mo);
            v10_map_match_vote(&mo, mv, 170, "sock");
            CHECK(!mo.stop && mo.count == 1, "mv translates");
            if (mo.count == 1) {
                const KrunkerValue vote0 = ARR(S("0,5"), N(2));
                const KrunkerValue sock = ARR(S("1,9"), N(1));
                const KrunkerValue mts_want = ARR(S("0,5"), S("1,9"));
                const KrunkerValue *vo = &mo.msgs[0].args[0];
                CHECK(vo->len == 2 && vequal(map_get_v(vo, "vote0"), &vote0) && vequal(map_get_v(vo, "sock"), &sock), "mv: vote0 and sock");
                CHECK(vequal(&mo.msgs[0].args[1], &mts_want), "mv: the option list mts");
            }
            v10_out_free(&mo);
            memset(mv, 0, sizeof mv);
            mv[0] = 1; mv[1] = 0;
            put_i32(mv, 2, 2); put_i32(mv, 6, 3); put_u16(mv, 10, 4);
            v10_out_init(&mo);
            v10_map_match_vote(&mo, mv, 170, "sock");
            if (mo.count == 1) {
                const KrunkerValue ours = ARR(S("2,3"), N(1));
                const KrunkerValue rest = ARR(S("2,3"), N(3));
                const KrunkerValue *vo = &mo.msgs[0].args[0];
                CHECK(vequal(map_get_v(vo, "sock"), &ours) && vequal(map_get_v(vo, "vote0"), &rest), "mv: ours is 1, the rest of the weight is vote0");
            }
            v10_out_free(&mo);
            memset(mv, 0, sizeof mv);
            v10_out_init(&mo);
            v10_map_match_vote(&mo, mv, 170, "sock");
            CHECK(mo.count == 1 && mo.msgs[0].args[0].len == 0 && mo.msgs[0].args[1].len == 0, "no vote clears the cards: {} and []");
            v10_out_free(&mo);
            mv[0] = 5;
            v10_out_init(&mo);
            v10_map_match_vote(&mo, mv, 170, "sock");
            CHECK(stop_has(&mo, "malformed"), "more than 4 cards is malformed");
            v10_out_free(&mo);
        }

        // LEAVE (20, NetLeavePacket, 4 bytes) -> "2" [uid] (REMOVE_PLAYER, one sid)
        {
            uint8_t lv[4];
            V10Out lo;
            put_i32(lv, 0, 9);
            v10_out_init(&lo);
            v10_map_leave(&lo, lv, 4);
            MSG_IS(&lo, 0, "2", N(9));
            v10_out_free(&lo);
            v10_out_init(&lo);
            v10_map_leave(&lo, lv, 3);
            CHECK(stop_has(&lo, "malformed"), "a short leave is malformed");
            v10_out_free(&lo);
        }

        // a joiner waiting to spawn: KILL_PLAYER "3" [uid, 0, 0, [], nil, false] (v10.js 220335, the respawn countdown)
        {
            V10Out dc;
            v10_out_init(&dc);
            v10_map_death_countdown(&dc, 1);
            MSG_IS(&dc, 0, "3", N(1), N(0), N(0), ARR(), NIL, B(0));
            v10_out_free(&dc);
        }

        // then "frs" [true], the server's ENTER_GAME: the client's handler calls enterGame(true), which sends "en"
        {
            V10Out eg;
            v10_out_init(&eg);
            v10_map_enter_game(&eg);
            MSG_IS(&eg, 0, "frs", B(1));
            v10_out_free(&eg);
        }

        // SHOT (21, NetShotPacket, 31 bytes) -> the tracer "9": [sid, hit (0.1), normal (0.01) | nil, trail, origin (0.1), 0, 0]
        {
            uint8_t sh[31];
            V10Out so;
            V10Shooter known = {9, 1.26, 2, 3, 9.2};
            memset(sh, 0, sizeof sh);
            put_i32(sh, 0, 9);
            put_f32(sh, 8, 10.44f); put_f32(sh, 12, 2); put_f32(sh, 16, -5.06f);
            sh[20] = 1;
            put_f32(sh, 21, 0.5f); put_f32(sh, 25, 1.2f);
            sh[29] = 1;
            v10_out_init(&so);
            v10_map_shot(&so, sh, 31, &known);
            CHECK(!so.stop && so.count == 1 && !strcmp(so.msgs[0].label, "9") && so.msgs[0].argc == 1, "shot -> the tracer \"9\", one array argument");
            const KrunkerValue want[13] = {N(9), N(10.4), N(2), N(-5.1), N(0.5), N(1.2), N(1), N(1.3), N(2), N(9.2), N(3), N(0), N(0)};
            if (so.count == 1 && so.msgs[0].args[0].type == KRUNKER_ARRAY && so.msgs[0].args[0].len == 13) {
                for (int i = 0; i < 13; i++) CHECK(vequal(&so.msgs[0].args[0].u.items[i], &want[i]), "tracer slot %d", i);
            } else {
                CHECK(0, "the tracer has 13 slots");
            }
            v10_out_free(&so);
            memset(sh + 20, 0, 1);
            sh[20] = 0;
            put_f32(sh, 8, 4); put_f32(sh, 12, 5); put_f32(sh, 16, 6);
            sh[29] = 0;
            v10_out_init(&so);
            v10_map_shot(&so, sh, 31, &known);
            if (so.count == 1 && so.msgs[0].args[0].type == KRUNKER_ARRAY) {
                CHECK(so.msgs[0].args[0].u.items[4].type == KRUNKER_NIL && so.msgs[0].args[0].u.items[5].type == KRUNKER_NIL,
                      "no normal: nil angles");
            }
            v10_out_free(&so);
            v10_out_init(&so);
            v10_map_shot(&so, sh, 31, NULL);
            if (so.count == 1 && so.msgs[0].args[0].type == KRUNKER_ARRAY) {
                const KrunkerValue *t = so.msgs[0].args[0].u.items;
                CHECK(vequal(&t[7], &KRUNKER_V_NUM(4)) && vequal(&t[8], &KRUNKER_V_NUM(5)) && vequal(&t[9], &KRUNKER_V_NUM(0)) && vequal(&t[10], &KRUNKER_V_NUM(6)),
                      "an unknown shooter: the hit point is the origin, height 0");
            }
            v10_out_free(&so);
            v10_out_init(&so);
            v10_map_shot(&so, sh, 30, &known);
            CHECK(stop_has(&so, "malformed"), "a short shot is malformed");
            v10_out_free(&so);
        }

        // EXT 0x8120 (NetServerLinePacket, 324 bytes): the template's first "$" is the name, a server line (mapServerLine)
        {
            uint8_t sl[324];
            V10Out so;
            memset(sl, 0, sizeof sl);
            put_i32(sl, 0, 3);
            put_str(sl, 4, "Ada", 32);
            put_str(sl, 36, "$ joined the game", 288);
            v10_out_init(&so);
            v10_map_server_line(&so, sl, 324);
            MSG_IS(&so, 0, "ch", N(-1), NIL, S("Ada joined the game"), N(2), NIL, NIL, NIL, NIL);
            v10_out_free(&so);
            memset(sl + 36, 0, 288);
            put_str(sl, 36, "Round starts", 288);
            v10_out_init(&so);
            v10_map_server_line(&so, sl, 324);
            MSG_IS(&so, 0, "ch", N(-1), NIL, S("Round starts"), N(2), NIL, NIL, NIL, NIL);
            v10_out_free(&so);
            v10_out_init(&so);
            v10_map_server_line(&so, sl, 323);
            CHECK(stop_has(&so, "malformed"), "a short server line is malformed");
            v10_out_free(&so);
        }

        // RECON (110, NetReconPacket, 89 bytes) -> l, one argument: the 26-slot sync array (mapRecon, v10Translate.ts)
        {
            uint8_t rc[89];
            V10Out ro;
            memset(rc, 0, sizeof rc);
            put_i32(rc, 0, 12);
            put_f32(rc, 4, 1.5f); put_f32(rc, 8, 2.25f); put_f32(rc, 12, -3);
            put_f32(rc, 16, 0.5f); put_f32(rc, 20, -1); put_f32(rc, 24, 2);
            put_f32(rc, 28, 0.75f);
            put_f32(rc, 32, 0.25f);
            put_f32(rc, 36, 0.5f);
            put_i32(rc, 40, 2);
            put_f32(rc, 44, 0.5f);
            put_f32(rc, 48, 3); put_f32(rc, 52, 4);
            rc[56] = 1;
            rc[57] = 0x01 | 0x10 | 0x20;
            put_u16(rc, 58, 3);
            put_f32(rc, 60, 0.25f);
            put_f64(rc, 64, 420);
            rc[72] = 0;
            v10_out_init(&ro);
            v10_map_recon(&ro, rc, 89);
            CHECK(!ro.stop && ro.count == 1 && !strcmp(ro.msgs[0].label, "l") && ro.msgs[0].argc == 1, "recon -> l, one argument");
            const KrunkerValue want[26] = {N(12), N(0), N(1.5), N(2.25), N(-3), N(-1), N(0.5), N(2), N(0.75), B(1), B(0), B(0),
                                           N(3), N(1), B(0), N(0.75), N(0.5), N(2), N(420), B(1), B(1), N(3), N(4), NIL, N(250), NIL};
            if (ro.count == 1 && ro.msgs[0].argc == 1 && ro.msgs[0].args[0].type == KRUNKER_ARRAY && ro.msgs[0].args[0].len == 26) {
                for (int i = 0; i < 26; i++) CHECK(vequal(&ro.msgs[0].args[0].u.items[i], &want[i]), "l slot %d", i);
            } else {
                CHECK(0, "the recon array has 26 slots");
            }
            v10_out_free(&ro);
            v10_out_init(&ro);
            v10_map_recon(&ro, rc, 58);
            CHECK(stop_has(&ro, "malformed"), "a short recon is malformed");
            v10_out_free(&ro);
            rc[72] = 1;
            put_f32(rc, 73, 10); put_f32(rc, 77, 11); put_f32(rc, 81, 12); put_f32(rc, 85, 13);
            v10_out_init(&ro);
            v10_map_recon(&ro, rc, 89);
            CHECK(ro.count == 1 && ro.msgs[0].args[0].u.items[23].len == 4 && vequal(&ro.msgs[0].args[0].u.items[23].u.items[3], &KRUNKER_V_NUM(13)),
                  "a grapple: its point and range");
            v10_out_free(&ro);
        }

        // INPUT (q, v10.js 221577): the window's frames field by field (decodeInputBatch in v10Translate.ts)
        {
            static const char *const keys[11] = {"0-4", "0-5", "0-6", "0-7", "0-8", "0-9", "0-10", "0-11", "0-12", "0-13", "0-14"};
            /* a window of one frame: field `field` set to `value`, the others 0 (move -> the move index, so 0 is direction 7) */
            NetInputPacket f1;
            #define INPUT_ONE(field, value) do { \
                V10Input in_ = {0}; v10_input_init(&in_); \
                KrunkerValue vals_[11]; for (int i_ = 0; i_ < 11; i_++) vals_[i_] = N(0); \
                vals_[(field) - 4] = N(value); \
                KrunkerValue ch_ = kv_map(11, keys, vals_); \
                KrunkerValue args_[6] = {N(1), N(1), S("16"), N(2), ARR(N(0), N(0)), ch_}; \
                size_t n_ = 0; NetInputPacket *fr_ = v10_decode_input(&in_, args_, 6, &n_); \
                CHECK(fr_ && n_ == 1, "one frame for field %d", (field)); \
                f1 = fr_ ? fr_[0] : f1; free(fr_); v10_input_free(&in_); \
            } while (0)
            INPUT_ONE(4, -1);
            CHECK(f1.move_dir == -1, "move index -1: no move direction (moveLock)");
            INPUT_ONE(4, 0);
            CHECK(f1.move_dir == 7, "move index 0 is direction 7 (movDirs order)");
            INPUT_ONE(4, 3);
            CHECK(f1.move_dir == 2, "move index 3 is direction 2");
            INPUT_ONE(4, 8);
            CHECK(f1.move_dir == -1, "a move index past the eight directions is no direction");
            INPUT_ONE(5, 1);
            CHECK(f1.flags == NET_INPUT_SHOOT, "fire (shoot) is field 5");
            INPUT_ONE(6, 1);
            CHECK(f1.flags == NET_INPUT_SCOPE, "ADS (scope) is field 6");
            INPUT_ONE(7, 1);
            CHECK(f1.flags == NET_INPUT_JUMP, "jump is field 7");
            INPUT_ONE(8, 1);
            CHECK(f1.flags == NET_INPUT_RELOAD, "reload is field 8");
            INPUT_ONE(9, 1);
            CHECK(f1.flags == NET_INPUT_CROUCH, "crouch is field 9");
            INPUT_ONE(10, -1);
            CHECK(f1.scroll == -1, "scroll is field 10");
            INPUT_ONE(11, 1);
            CHECK(f1.swap == 1, "weapon swap is field 11");
            INPUT_ONE(12, 0.5);
            CHECK(f1.slowdown == 5, "the slowdown (1 - speedLmt, tenths) is field 12");
            INPUT_ONE(13, 1);
            CHECK(f1.flags == NET_INPUT_RESET_START, "reset 1 is reset to start");
            INPUT_ONE(13, 2);
            CHECK(f1.flags == NET_INPUT_RESET_LAST, "reset 2 is reset to last");
            INPUT_ONE(14, 1);
            CHECK(f1.flags == NET_INPUT_INTERACT, "interact is field 14");
            #undef INPUT_ONE
        }
        {
            /* the official window: seq 5 and 6, dt 16, pitch 0.1 and yaw -0.2 then the deltas 5 and 2, shoot on frame 0 */
            static const char *const keys[11] = {"0-4", "0-5", "0-6", "0-7", "0-8", "0-9", "0-10", "0-11", "0-12", "0-13", "0-14"};
            KrunkerValue vals[11] = {N(3), N(1), N(0), N(0), N(0), N(0), N(0), N(0), N(0), N(0), N(0)};
            KrunkerValue args[6] = {N(0), N(5), S("1616"), N(2), ARR(N(100), N(-200), N(5), N(2)), kv_map(11, keys, vals)};
            V10Input in;
            v10_input_init(&in);
            size_t n = 0;
            NetInputPacket *fr = v10_decode_input(&in, args, 6, &n);
            CHECK(fr && n == 2, "a two-frame window");
            if (fr && n == 2) {
                CHECK(fr[0].seq == 5 && fr[1].seq == 6, "sequence 5 and 6 (firstIsn + j)");
                CHECK(fabs(fr[0].delta - 0.016) < 1e-6 && fabs(fr[1].delta - 0.016) < 1e-6, "dt 16 ms is 0.016 s");
                CHECK(fabs(fr[0].x_dir - 0.1) < 1e-6 && fabs(fr[1].x_dir - 0.105) < 1e-6, "x_dir is the pitch, accumulated from the deltas");
                CHECK(fabs(fr[0].y_dir + 0.2) < 1e-6 && fabs(fr[1].y_dir + 0.198) < 1e-6, "y_dir is the yaw, accumulated from the deltas");
                CHECK(fr[0].flags == NET_INPUT_SHOOT && fr[1].flags == NET_INPUT_SHOOT, "shoot holds across the frames");
            }
            free(fr);
            /* a window with no angles keeps the window before's; a resync restarts the fields */
            KrunkerValue args2[6] = {N(0), N(7), S("16"), N(2), N(0), kv_map(0, NULL, NULL)};
            fr = v10_decode_input(&in, args2, 6, &n);
            CHECK(fr && n == 1 && fabs(fr[0].x_dir - 0.1) < 1e-6 && fr[0].flags == NET_INPUT_SHOOT, "no angles: the last ones, the shoot carries");
            free(fr);
            KrunkerValue args3[6] = {N(1), N(9), S("16"), N(2), N(0), kv_map(0, NULL, NULL)};
            fr = v10_decode_input(&in, args3, 6, &n);
            CHECK(fr && n == 1 && fr[0].flags == 0 && fr[0].move_dir == -1, "a resync restarts the fields: no shoot, no move");
            free(fr);
            KrunkerValue bad[6] = {N(0), N(1), N(0), N(0), N(0), N(0)};
            CHECK(v10_decode_input(&in, bad, 6, &n) == NULL, "a number for the dt list is malformed");
            KrunkerValue empty_w[6] = {N(0), N(1), S(""), N(0), N(0), N(0)};
            fr = v10_decode_input(&in, empty_w, 6, &n);
            CHECK(fr && n == 0, "an empty window has no frames");
            free(fr);
            v10_input_free(&in);
            /* the payload: count, then 24 bytes per frame (NetInputPacket) */
            NetInputPacket two[2];
            memset(two, 0, sizeof two);
            two[0].seq = 5; two[1].seq = 6; two[0].flags = NET_INPUT_SHOOT;
            uint8_t payload[1 + 24 * 2];
            CHECK(v10_encode_input_batch(two, 2, payload) == 1 + 24 * 2 && payload[0] == 2, "the batch: count 2, 1 + 24 bytes per frame");
            CHECK(rd(payload, 1) == 5 && rd(payload, 1 + 24) == 6 && payload[1 + 20] == NET_INPUT_SHOOT, "the frames' seq and flags");
        }

        // CHEAT (60): [flag, on], the on / off byte 0 or 1; the flags with a v10 label (noc, unl, fh, frz)
        uint8_t cflag = 0;
        int con = 0;
        const uint8_t c_ok[2] = {1, 0};
        const uint8_t c_bad[2] = {1, 2};
        CHECK(v10_parse_cheat(c_ok, 2, &cflag, &con) && cflag == 1 && con == 0, "CHEAT [1, 0] parses: flag 1, off");
        CHECK(!v10_parse_cheat(c_bad, 2, &cflag, &con), "CHEAT with an on byte past 1 is malformed");
        CHECK(!v10_parse_cheat(c_ok, 1, &cflag, &con), "a one-byte CHEAT is malformed");
        CHECK(v10_cheat_label(0) && !strcmp(v10_cheat_label(0), "noc") && !strcmp(v10_cheat_label(1), "unl") &&
              !strcmp(v10_cheat_label(2), "fh") && !strcmp(v10_cheat_label(7), "frz"), "CHEAT labels noc, unl, fh, frz");
        CHECK(v10_cheat_label(3) == NULL && v10_cheat_label(4) == NULL && v10_cheat_label(5) == NULL && v10_cheat_label(6) == NULL,
              "borders, forcecol, third person and held have no label");

        KrunkerValue short_en[2] = {N(1), N(2)};
        uint8_t junk[V10_ENT_SIZE];
        CHECK(!v10_encode_enter(&KRUNKER_V_ARRAY(short_en, 2), junk), "encodeEnter refuses a short array");
        KrunkerValue blank = S("   ");
        uint8_t chat[V10_CHAT_TEXT_SIZE];
        CHECK(v10_encode_chat(&blank, chat) == 0, "encodeChat of blank text is null");
        KrunkerValue hi = S("hello there");
        CHECK(v10_encode_chat(&hi, chat) == 12 && !memcmp(chat, "hello there\0", 12), "encodeChat keeps the text, NUL-terminated");
        KrunkerValue num_text = N(5);
        CHECK(v10_encode_chat(&num_text, chat) == 0, "encodeChat of a non-string is null");
    }

    // ---- MODE_STATE and OBJECT_STATE (v10Proxy.test.ts 181-215)
    {
        uint8_t b[387];
        V10Out o;
        mode_state(b, 5, 7, 3, 1, "wt", -1, 0, -1);
        v10_out_init(&o);
        v10_map_mode_state(&o, b, 387);
        CHECK(!o.stop, "mode state translates");
        CHECK(o.count == 5, "FFA mode state: 5 messages");
        MSG_IS(&o, 0, "ts", N(1), N(5));
        MSG_IS(&o, 1, "ts", N(2), N(7));
        MSG_IS(&o, 2, "lv", N(3));
        MSG_IS(&o, 3, "lock", B(1));
        MSG_IS(&o, 4, "gmsg", S("wt"));
        v10_out_free(&o);

        mode_state(b, 0, 0, -1, 0, "", -1, 0, -1);
        v10_out_init(&o);
        v10_map_mode_state(&o, b, 387);
        CHECK(o.count == 2, "quiet mode state: no lives, not locked, no message");
        MSG_IS(&o, 0, "ts", N(1), N(0));
        MSG_IS(&o, 1, "ts", N(2), N(0));
        v10_out_free(&o);

        // flags in play: ufl(uid, x, y, z, carrier || 0) after the team lines; a flag not in play is skipped
        mode_state(b, 0, 0, -1, 0, "", -1, 0, -1);
        const Flag fl[2] = {{1, 2, 3, -1, 1, 7, 1}, {4, 5, 6, 42, 2, 9, 0}};
        mode_state_flags(b, fl, 2);
        v10_out_init(&o);
        v10_map_mode_state(&o, b, 387);
        CHECK(o.count == 5, "flagged mode state: 5 messages");
        MSG_IS(&o, 2, "ufi", N(1), S("AT BASE"));
        MSG_IS(&o, 3, "ufi", N(2), S("AT BASE"));
        MSG_IS(&o, 4, "ufl", N(7), N(1), N(2), N(3), N(0));
        v10_out_free(&o);

        mode_state(b, 0, 0, -1, 0, "", -1, 0, -1);
        const Flag carried[1] = {{0.5f, 2, 3, 42, 2, 9, 1}};
        mode_state_flags(b, carried, 1);
        v10_out_init(&o);
        v10_map_mode_state(&o, b, 387);
        MSG_IS(&o, 4, "ufl", N(9), N(0.5), N(2), N(3), N(42));
        v10_out_free(&o);

        mode_state(b, 0, 0, 0, 0, "", -1, 0, -1);
        const Flag nameless[1] = {{0, 0, 0, -1, 1, -1, 1}};
        mode_state_flags(b, nameless, 1);
        v10_out_init(&o);
        v10_map_mode_state(&o, b, 387);
        CHECK(stop_has(&o, "map object id"), "an active flag with no map object id stops");
        v10_out_free(&o);

        // an active objective is sent by MODE_SYNC_V7 (with its time): MODE_STATE sends none of it
        mode_state(b, 0, 0, 0, 0, "", 2, 0, -1);
        v10_out_init(&o);
        v10_map_mode_state(&o, b, 387);
        CHECK(!o.stop && o.count == 3, "objective: no obj in the mode state");
        MSG_IS(&o, 2, "lv", N(0));
        v10_out_free(&o);

        // a weapon objective is a wep (the objective weapon's uid)
        mode_state(b, 0, 0, -1, 0, "", 2, 0, 2);
        v10_out_init(&o);
        v10_map_mode_state(&o, b, 387);
        CHECK(o.count == 3, "weapon objective: wep");
        MSG_IS(&o, 2, "wep", N(2));
        v10_out_free(&o);

        mode_state(b, 0, 0, 0, 0, "", -1, 1, -1);
        v10_out_init(&o);
        v10_map_mode_state(&o, b, 387);
        CHECK(stop_has(&o, "lock"), "a move lock stops (lm needs a position)");
        v10_out_free(&o);

        uint8_t short_buf[10] = {0};
        v10_out_init(&o);
        v10_map_mode_state(&o, short_buf, 10);
        CHECK(o.stop != NULL, "a short mode state is malformed");
        v10_out_free(&o);

        uint8_t ob[9];
        const struct { uint8_t kind; int32_t value; const char *label; } kinds[] = {
            {0, 0, "do"}, {1, 0, "ro"}, {2, 0, "gte"}, {2, 1, "gte"}, {3, 5, "bnk"}, {4, 4, "mo"}, {5, 0, "mf"},
        };
        for (size_t k = 0; k < sizeof kinds / sizeof kinds[0]; k++) {
            object_state(ob, 7, kinds[k].kind, kinds[k].value);
            v10_out_init(&o);
            v10_map_object_state(&o, ob, 9);
            CHECK(o.count == 1 && !strcmp(o.msgs[0].label, kinds[k].label), "object kind %u -> %s", kinds[k].kind, kinds[k].label);
            v10_out_free(&o);
        }
        object_state(ob, 7, 2, 0);
        v10_out_init(&o); v10_map_object_state(&o, ob, 9);
        MSG_IS(&o, 0, "gte", N(7), B(0));
        v10_out_free(&o);
        object_state(ob, 7, 2, 1);
        v10_out_init(&o); v10_map_object_state(&o, ob, 9);
        MSG_IS(&o, 0, "gte", N(7), B(1));
        v10_out_free(&o);
        object_state(ob, 7, 3, 5);
        v10_out_init(&o); v10_map_object_state(&o, ob, 9);
        MSG_IS(&o, 0, "bnk", N(7), N(5));
        v10_out_free(&o);
        object_state(ob, 7, 4, 4);
        v10_out_init(&o); v10_map_object_state(&o, ob, 9);
        MSG_IS(&o, 0, "mo", N(7), N(4));
        v10_out_free(&o);
        object_state(ob, 7, 5, 0);
        v10_out_init(&o); v10_map_object_state(&o, ob, 9);
        MSG_IS(&o, 0, "mf", N(7));
        v10_out_free(&o);
        object_state(ob, 7, 9, 0);
        v10_out_init(&o); v10_map_object_state(&o, ob, 9);
        CHECK(stop_has(&o, "kind"), "an unknown object kind stops");
        v10_out_free(&o);
    }

    // ---- SKINS, SPAWN, LEADERBOARD (v10Proxy.test.ts 217-242)
    {
        uint8_t sk[16];
        int32_t vals[4] = {2, 7, 8, 9};
        for (int i = 0; i < 4; i++) put_i32(sk, (size_t)i * 4, vals[i]);
        V10Skins skins;
        CHECK(v10_parse_skins(sk, 16, &skins) && skins.uid == 2 && skins.primary == 7 && skins.secondary == 8 && skins.melee == 9,
              "SKINS parse");

        TSpawn other_s = default_spawn();
        other_s.uid = 2; other_s.is_you = 0; other_s.class_index = 3; other_s.team = 1; other_s.health = 87.6f; other_s.max_health = 100;
        other_s.pos[0] = 10.4f; other_s.pos[1] = 20.6f; other_s.pos[2] = -5; other_s.dir[0] = 1.5f; other_s.dir[1] = 0.25f;
        other_s.name = "Ada"; other_s.secondary = 4; other_s.hat = 3; other_s.body = 5; other_s.hair = 0x112233; other_s.skin_color = -1;
        other_s.dye = 8; other_s.waist = 6; other_s.face = 7; other_s.head = 15; other_s.back = 16; other_s.shoe = 17; other_s.pet = 18;
        other_s.wrist = 19; other_s.charm0 = 20; other_s.charm1 = 21; other_s.player_card = 22; other_s.head_mlt = 1;
        uint8_t buf[150];
        spawn_buf(buf, &other_s);
        V10Spawn other;
        CHECK(v10_parse_spawn(buf, 150, &other), "SPAWN parse");
        V10Skins sk2 = {2, 7, 8, 9};
        V10Out o;
        v10_out_init(&o);
        const KrunkerValue rec = v10_player_add_record(&o, &other, &sk2, 1, "SOCKET");
        const KrunkerValue want[53] = {
            S("2"), N(2), N(10), N(21), N(-5), S("Ada"), N(3), N(88), N(100), N(1), N(0), N(0), ARR(N(7), N(8)), N(3), N(5), N(4),
            N(0), NIL, N(0.25), N(9), N(-1), N(-1), N(-1), N(-1), N(8), N(0), N(-1), N(0), S("Ada"), N(17), N(6), N(0),
            S("#112233"), N(7), N(18), N(-1), N(19), N(0), N(1), ARR(N(20), N(21)), N(0), N(16), N(15), N(22), N(0), N(0), N(0),
            N(0), N(0), N(0), N(0), N(0), N(-1),
        };
        CHECK(rec.type == KRUNKER_ARRAY && rec.len == 53, "the add record has 53 fields");
        for (uint32_t i = 0; i < 53 && rec.type == KRUNKER_ARRAY; i++) {
            if (!vequal(&rec.u.items[i], &want[i])) {
                printf("  field %u differs\n", i);
                CHECK(0, "add record field %u as the TS test", i);
            }
        }
        v10_out_free(&o);

        TSpawn mine_s = default_spawn();
        mine_s.uid = 1; mine_s.is_you = 1; mine_s.class_index = 4; mine_s.team = 0; mine_s.name = "me";
        uint8_t mbuf[150];
        spawn_buf(mbuf, &mine_s);
        V10Spawn mine;
        CHECK(v10_parse_spawn(mbuf, 150, &mine), "our SPAWN parse");
        v10_out_init(&o);
        const KrunkerValue mrec = v10_player_add_record(&o, &mine, NULL, 0, "SOCKET");
        CHECK(vequal(&mrec.u.items[0], &KRUNKER_V_STR("SOCKET")), "our id is the socket id");
        KrunkerValue start[8];
        v10_start_game(&mine, 300, 1, start);
        const KrunkerValue want_start[8] = {N(300), N(0), N(0), N(1), N(0), N(1), NIL, N(4)};
        CHECK(memcmp(start, want_start, 0) == 0 && vequal(&start[0], &want_start[0]) && vequal(&start[3], &want_start[3]) &&
                  vequal(&start[7], &want_start[7]) && start[6].type == KRUNKER_NIL, "buildStartGame(mine, 300, 1)");
        v10_out_free(&o);
        CHECK(!v10_parse_spawn(buf, 78, &other), "a short SPAWN is refused");

        uint8_t board[257];
        memset(board, 0, sizeof board);
        board[0] = 2;
        const int32_t rows_in[8] = {1, 40, 3, 1, 2, 10, 0, 4};
        for (int i = 0; i < 8; i++) put_i32(board, 1 + (size_t)i * 4, rows_in[i]);
        int32_t rows[64];
        size_t row_count = 0;
        v10_out_init(&o);
        v10_map_leaderboard(&o, board, 257, 0, rows, &row_count);
        MSG_IS(&o, 0, "7", ARR(N(1), N(40), N(3), N(1), N(2), N(10), N(0), N(4)));
        v10_out_free(&o);
        v10_out_init(&o);
        v10_map_leaderboard(&o, board, 257, 1, rows, &row_count);
        CHECK(stop_has(&o, "Free for All"), "another mode's leaderboard stops");
        v10_out_free(&o);
    }

    // ---- MODE_SYNC_V7, DOM_UPDATE, AI_SYNC_V7, the bot add (v10Proxy.test.ts 244-329)
    {
        uint8_t sync[65];
        V10Out o;
        // scores, rounds, lockout, objective (uid, ms)
        memset(sync, 0, 65);
        // scores [0, 4, 0, 9, 0, 0]: team_scores[t] at t * 4 (the TS fixture's writeInt32LE(v, t * 4))
        put_i32(sync, 4, 4); put_i32(sync, 12, 9);
        put_i32(sync, 53, 0); put_i32(sync, 57, -1); put_i32(sync, 61, 0);
        v10_out_init(&o);
        v10_map_mode_sync(&o, sync, 65, 0);
        CHECK(o.count == 2, "mode sync: the scored teams");
        MSG_IS(&o, 0, "ts", N(1), N(4));
        MSG_IS(&o, 1, "ts", N(3), N(9));
        v10_out_free(&o);

        memset(sync, 0, 65);
        put_i32(sync, 53, 1500); put_i32(sync, 57, -1);
        v10_out_init(&o); v10_map_mode_sync(&o, sync, 65, 0);
        MSG_IS(&o, 0, "lkt", N(1500));
        v10_out_free(&o);

        memset(sync, 0, 65);
        put_i32(sync, 53, 0); put_i32(sync, 57, 3); put_i32(sync, 61, 4000);
        v10_out_init(&o); v10_map_mode_sync(&o, sync, 65, 0);
        MSG_IS(&o, 0, "obj", N(3), N(4000), B(1));
        v10_out_free(&o);
        v10_out_init(&o); v10_map_mode_sync(&o, sync, 65, 1);
        CHECK(o.count == 0 && !o.stop, "a weapon objective: wep was sent");
        v10_out_free(&o);

        memset(sync, 0, 65);
        put_i32(sync, 24, 2);
        v10_out_init(&o); v10_map_mode_sync(&o, sync, 65, 0);
        CHECK(stop_has(&o, "rounds"), "rounds stop the mode sync");
        v10_out_free(&o);

        // DOM_UPDATE (142): one flat uct list of [object id, capturedBy, captureT, lastTeamOn, teamsOn, mlt] per zone
        uint8_t dom[497];
        memset(dom, 0, sizeof dom);
        dom[0] = 2;
        put_i32((uint8_t *)dom, 1 + 0 * 31 + 0, 0);
        put_i32(dom, 1 + 0 * 31 + 4, 1);
        put_f32(dom, 1 + 0 * 31 + 8, 250);
        put_i32(dom, 1 + 0 * 31 + 12, 0);
        dom[1 + 0 * 31 + 16] = 1;
        dom[1 + 0 * 31 + 17] = 1;
        put_f32(dom, 1 + 0 * 31 + 23, 1.5f);
        put_i32(dom, 1 + 0 * 31 + 27, 42);
        put_i32(dom, 1 + 1 * 31 + 27, 57);
        v10_out_init(&o);
        v10_map_dom_update(&o, dom, 497);
        MSG_IS(&o, 0, "uct", ARR(N(42), N(1), N(250), NIL, ARR(N(1)), N(1.5), N(57), N(0), N(0), NIL, kv_arr(0, NULL), N(1)));
        v10_out_free(&o);
        put_i32(dom, 1 + 1 * 31 + 27, -1);
        v10_out_init(&o); v10_map_dom_update(&o, dom, 497);
        CHECK(stop_has(&o, "map object id"), "a zone with no map object id stops");
        v10_out_free(&o);

        // AI_SYNC_V7 (161): one chunk of bots -> the list of 9 values per bot; a short chunk ends the sync
        uint8_t one[2 + 31];
        memset(one, 0, sizeof one);
        one[0] = 1; one[1] = 1;
        put_i32(one, 2, 5);
        put_f32(one, 6, 1.26f); put_f32(one, 10, 2.04f); put_f32(one, 14, -3.5f);
        put_f32(one, 18, 0.123f); put_f32(one, 22, -0.5f);
        put_i32(one, 26, 88);
        one[30] = 1;
        put_u16(one, 31, 2);
        V10BotChunk chunk;
        CHECK(v10_parse_bot_sync_chunk(one, sizeof one, &chunk), "AI_SYNC chunk parse");
        CHECK(chunk.first == 1 && chunk.count == 1, "AI_SYNC: first chunk of 1 bot");
        CHECK(chunk.entries[0].sid == 5 && chunk.entries[0].x == 1.3 && chunk.entries[0].y == 2 && chunk.entries[0].z == -3.5 &&
              chunk.entries[0].xd == 0.12 && chunk.entries[0].yd == -0.5 && chunk.entries[0].health == 88 &&
              chunk.entries[0].did_move == 1 && chunk.entries[0].team == 2,
              "AI_SYNC entry: sid, x 1.3, y 2, z -3.5, xD 0.12, yD -0.5, health 88, didMov true, team 2");
        uint8_t empty_chunk[2] = {0, 0};
        CHECK(v10_parse_bot_sync_chunk(empty_chunk, 2, &chunk) && chunk.count == 0, "an empty AI_SYNC chunk is a chunk");
        CHECK(v10_parse_bot_sync_chunk(empty_chunk, 1, &chunk) == 0, "a one-byte AI_SYNC is malformed");
        v10_out_init(&o);
        v10_map_mode_sync(&o, sync, 10, 0);
        CHECK(stop_has(&o, "malformed"), "a short mode sync is malformed");
        v10_out_free(&o);

        // AI_ADD_V7 (162): the v7 bot record -> aai [sid, index, name, x, y, z, dat]
        uint8_t bot[237];
        memset(bot, 0, sizeof bot);
        put_i32(bot, 0, 9); put_i32(bot, 4, 4);
        put_str(bot, 8, "Bot", 40);
        put_f32(bot, 48, 1.5f); put_f32(bot, 52, 2); put_f32(bot, 56, 3);
        bot[60] = 1; bot[61] = 1; bot[62] = 1;
        put_u16(bot, 66, 2);
        put_f32(bot, 68, 0.5f); put_f32(bot, 72, 30); put_f32(bot, 76, 100); put_f32(bot, 80, 1.5f);
        put_f32(bot, 84, 0.25f); put_f32(bot, 88, 0.5f); put_f32(bot, 92, 0);
        put_str(bot, 96, "46544e", 16);
        put_i32(bot, 112, 7);
        put_str(bot, 116, "a/b.glb", 24);
        put_f32(bot, 140, 1.25f);
        put_u16(bot, 144, 3);
        bot[146] = 1;
        put_f32(bot, 147, 0.5f); put_f32(bot, 151, 0.5f);
        bot[155] = 2; bot[156] = 4;
        put_f32(bot, 157, 0.25f); put_f32(bot, 161, 0.75f); bot[165] = 1;
        put_f32(bot, 166, 0.125f); put_f32(bot, 170, 2);
        bot[63] = 0; bot[64] = 1; bot[65] = 1;
        v10_out_init(&o);
        v10_map_bot_add(&o, bot, 237);
        CHECK(o.count == 1 && !strcmp(o.msgs[0].label, "aai") && o.msgs[0].argc == 1, "bot add -> aai (one record argument)");
        const KrunkerValue head6[6] = {N(9), N(4), S("Bot"), N(1.5), N(2), N(3)};
        for (int i = 0; i < 6 && o.count; i++) CHECK(vequal(&o.msgs[0].args[0].u.items[i], &head6[i]), "aai head field %d", i);
        const char *dk[28] = {"hasAnim", "animS", "soundR", "team", "health", "mSize", "mYOff", "mROff", "hbh", "asset", "idl", "src",
                              "noVis", "tagScale", "lvl", "side", "opacity", "alphaTest", "texEncode", "noSmooth", "noFog", "fTrans",
                              "frames", "frameT", "rAnim", "rAnimA", "bobD", "bobS"};
        const KrunkerValue dv[28] = {N(1), N(0.5), N(30), N(2), N(100), N(1.5), N(0.25), N(0.5), N(1), S("46544e"), N(7), S("a/b.glb"),
                                     N(1), N(1.25), N(3), N(1), N(0.5), N(0.5), N(2), B(1), B(0), B(1), N(4), N(0.25), N(0.75), N(1),
                                     N(0.125), N(2)};
        KrunkerValue want_dat = kv_map(28, dk, dv);
        if (o.count) CHECK(vequal(&o.msgs[0].args[0].u.items[6], &want_dat), "aai dat: the TS object");
        v10_out_free(&o);
        bot[60] = 0;
        v10_out_init(&o); v10_map_bot_add(&o, bot, 237);
        CHECK(o.count == 1 && vequal(&o.msgs[0].args[0].u.items[6], &KRUNKER_V_NUM(0)), "a bot with no data: dat 0");
        v10_out_free(&o);
    }

    // ---- the per-tick state record (v10Proxy.test.ts 586-606), the chat kinds (575-586), the native STATE
    {
        uint8_t st[79];
        memset(st, 0, sizeof st);
        put_i32(st, 0, 4);
        put_f32(st, 13, 1.256f);
        put_f32(st, 17, 2);
        put_f32(st, 21, -3.5f);
        put_f32(st, 37, 0.5f);
        put_f32(st, 41, 1);
        put_f32(st, 57, 0.25f);
        st[61] = 1;
        put_f32(st, 53, 0.6f);
        put_i32(st, 9, 2);
        put_f32(st, 49, 1);
        st[62] = 1;
        put_f32(st, 63, 1); put_f32(st, 67, 2); put_f32(st, 71, 3);
        put_f32(st, 75, 42.4f);
        V10StateRec rec;
        CHECK(v10_map_state(st, 79, &rec), "STATE parse");
        KrunkerValue rv = state_rec_value(&rec);
        const KrunkerValue want[13] = {N(4), N(1.26), N(2), N(-3.5), N(57), N(29), N(0.25), N(1), N(1), N(2), N(1), ARR(N(1), N(2), N(3)), N(42)};
        CHECK(rv.type == KRUNKER_ARRAY && rv.len == 13, "the state record has 13 fields");
        for (uint32_t i = 0; i < 13 && rv.type == KRUNKER_ARRAY; i++) CHECK(vequal(&rv.u.items[i], &want[i]), "state field %u as the TS record", i);
        CHECK(!v10_map_state(st, 53, &rec), "a short STATE is malformed");

        uint8_t junk[330];
        memset(junk, 0, sizeof junk);
        junk[9] = 3;
        put_str(junk, 42, "You found [1.25] Junk", 288);
        V10Out o;
        v10_out_init(&o);
        v10_map_chat_v7(&o, junk, 330);
        MSG_IS(&o, 0, "ch", N(-1), NIL, S("<span style='color:#fff'>You</span> found <span style='color:#cc51f8'>[1.25]</span> Junk"), N(1));
        v10_out_free(&o);
        put_str(junk, 42, "You found [1.2] Junk x", 288);
        v10_out_init(&o); v10_map_chat_v7(&o, junk, 330);
        CHECK(stop_has(&o, "found-junk"), "a found-junk line that is not the v10 form stops");
        v10_out_free(&o);

        uint8_t ranked_chat[330];
        memset(ranked_chat, 0, sizeof ranked_chat);
        ranked_chat[9] = 4;
        put_str(ranked_chat, 42, "Queued for ranked", 288);
        v10_out_init(&o); v10_map_chat_v7(&o, ranked_chat, 330);
        MSG_IS(&o, 0, "ch", N(-1), NIL, S("Queued for ranked"), N(2), S("#5ce05a"));
        v10_out_free(&o);

        uint8_t chat[330];
        memset(chat, 0, sizeof chat);
        chat[0] = 0;
        put_i32(chat, 1, 7);
        put_i32(chat, 5, 3);
        chat[9] = 0;
        put_str(chat, 10, "Ada", 32);
        put_str(chat, 42, "hi <b>", 288);
        v10_out_init(&o); v10_map_chat_v7(&o, chat, 330);
        MSG_IS(&o, 0, "ch", N(0), S("Ada"), S("hi &lt;b&gt;"), N(0), NIL, NIL, N(7), NIL);
        v10_out_free(&o);
        v10_out_init(&o); v10_map_chat_v7(&o, chat, 329);
        CHECK(stop_has(&o, "malformed"), "a short CHAT_V7 is malformed");
        v10_out_free(&o);
    }

    // ---- the native INIT / MAP / CONFIG / NETWORK layouts (v10Translate.ts: parseMapInfo, parseMapData, ...)
    {
        uint8_t info[44];
        memset(info, 0, sizeof info);
        put_i32(info, 0, 3);
        put_i32(info, 4, (int32_t)0xc0ffee);
        put_i32(info, 8, 723);
        put_str(info, 12, "Test Map", 32);
        V10MapInfo mi;
        CHECK(v10_parse_map_info(info, 44, &mi) && mi.map_index == 3 && mi.hash == 0xc0ffee && mi.size == 723 &&
              !strcmp(mi.name, "Test Map"), "MAP_INFO parse");
        uint8_t req[4];
        v10_encode_map_request(0xc0ffee, req);
        CHECK(req[0] == 0xee && req[1] == 0xff && req[2] == 0xc0 && req[3] == 0, "MAP_REQUEST is the hash");
        uint8_t md[10 + 4] = {0};
        put_i32(md, 0, (int32_t)0xc0ffee); put_i32(md, 4, 0); put_u16(md, 8, 4);
        memcpy(md + 10, "abcd", 4);
        uint32_t h, off; const uint8_t *data; size_t dl;
        CHECK(v10_parse_map_data(md, 14, &h, &off, &data, &dl) && dl == 4 && h == 0xc0ffee && off == 0, "MAP_DATA parse");
        CHECK(!v10_parse_map_data(md, 13, &h, &off, &data, &dl), "a MAP_DATA with a short tail is refused");
        uint8_t netd[10];
        put_u16(netd, 0, 10); put_u16(netd, 2, 30); put_u16(netd, 4, 1);
        V10Network net;
        CHECK(v10_parse_network_data(netd, 10, &net) && net.snf == 10 && net.tf == 30 && net.sf == 1, "NETWORK_DATA parse");
        uint8_t init[18];
        memset(init, 0, sizeof init);
        init[0] = 1; init[1] = 4; init[2] = 2;
        put_i32(init, 3, 3); put_i32(init, 7, 0); put_u16(init, 11, 30); put_i32(init, 13, 1); init[17] = 3;
        V10Init vi;
        CHECK(v10_parse_init(init, 18, &vi) && vi.map_index == 3 && vi.mode_index == 0 && vi.tick_rate == 30 &&
              vi.client_id == 1 && vi.ruleset == 3, "INIT parse with the ruleset byte");
        CHECK(v10_parse_init(init, 16, &vi) == 0, "a short INIT is malformed");
        uint8_t gv[80];
        memset(gv, 0, sizeof gv);
        gv[0] = 0x01;
        V10GlobalValue g;
        CHECK(v10_parse_global_value(gv, 80, &g) && g.reset == 1 && g.key[0] == 0, "a reset-only gv parses with no key");
        gv[0] = 0;
        memcpy(gv + 1, "score_limit", 11);
        put_f64(gv, 72, 2.5);
        CHECK(v10_parse_global_value(gv, 80, &g) && g.reset == 0 && !strcmp(g.key, "score_limit") && g.value == 2.5, "gv parse");
        uint8_t snd[39];
        memset(snd, 0, sizeof snd);
        put_i32(snd, 0, 2); snd[4] = 1; snd[6] = 1; put_f32(snd, 7, 0.5f); put_f32(snd, 11, 1.25f); put_str(snd, 15, "footstep", 24);
        V10PlayerSound ps;
        CHECK(v10_parse_player_sound(snd, 39, &ps) && ps.uid == 2 && ps.add && ps.loop && !ps.force && ps.volume == 0.5 &&
              ps.rate == 1.25 && !strcmp(ps.name, "footstep"), "PLAYER_SOUND parse");
        uint8_t tm[5];
        put_i32(tm, 0, 300); tm[4] = 0;
        V10Timer tt;
        CHECK(v10_parse_timer(tm, 5, &tt) && tt.seconds == 300 && !tt.end, "TIMER parse");
    }

    // ---- the config (buildConfig) and the INIT_GAME arguments
    {
        const char *text = "{\"host\":1,\"settings\":{\"hitBoxPad\":1,\"timeLimit\":5},\"classes\":[1,2]}";
        cJSON *root = v10_config_parse((const uint8_t *)text, strlen(text));
        CHECK(root != NULL, "the config is JSON");
        V10Out o;
        v10_out_init(&o);
        const KrunkerValue cfg = v10_config_value(&o, root);
        const char *ck[3] = {"hitBoxPad", "timeLimit", "classes"};
        const KrunkerValue cv[3] = {N(1), N(5), ARR(N(1), N(2))};
        KrunkerValue want_cfg = kv_map(3, ck, cv);
        CHECK(vequal(&cfg, &want_cfg), "buildConfig: the settings, then the classes");
        v10_out_free(&o);
        cJSON_Delete(root);
        CHECK(v10_config_parse((const uint8_t *)"null", 4) == NULL, "a null config is refused (buildConfig throws)");
        CHECK(v10_config_parse((const uint8_t *)"{", 1) == NULL, "a config that is not JSON is refused");

        V10Out o2;
        v10_out_init(&o2);
        V10Init init = {3, 0, 30, 1, 3};
        const char *map = "{\"name\":\"Test Map\"}";
        V10Network net = {10, 30, 1};
        const KrunkerValue nv = v10_network_value(&o2, &net);
        const KrunkerValue cfg2 = KRUNKER_V_MAP(NULL, 0);
        v10_init_game(&o2, &init, (const uint8_t *)map, strlen(map), cfg2, nv, 0);
        CHECK(o2.count == 1 && !strcmp(o2.msgs[0].label, "init") && o2.msgs[0].argc == 20, "INIT_GAME has 20 arguments");
        if (o2.count) {
            const V10Msg *m = &o2.msgs[0];
            CHECK(vequal(&m->args[0], &KRUNKER_V_NUM(3)), "init arg 0 is the map index");
            CHECK(vequal(&m->args[1], &KRUNKER_V_NUM(0)), "init arg 1 is the mode");
            CHECK(m->args[9].type == KRUNKER_MAP && m->args[9].u.items[1].len == strlen(map) &&
                  !memcmp(m->args[9].u.items[1].u.str, map, strlen(map)), "init arg 9: the map JSON text in customMapData.data");
            CHECK(m->args[12].type == KRUNKER_MAP, "init arg 12: the networking object");
            CHECK(vequal(&m->args[13], &KRUNKER_V_BOOL(1)), "init arg 13 is true");
            CHECK(vequal(&m->args[16], &KRUNKER_V_BOOL(0)), "init arg 16 is isRanked");
            CHECK(m->args[11].type == KRUNKER_MAP && m->args[11].len == 1 && vequal(&m->args[11].u.items[1], &KRUNKER_V_NUM(1)),
                  "init arg 11 is the sync data {gor: 1} (the v10 init reads it)");
        }
        v10_out_free(&o2);
    }

    // ---- chunk assembly (ChunkAssembly): in order, refused out of order or overlong
    {
        V10Chunks c;
        CHECK(v10_chunks_init(&c, 4) == 0, "a chunk assembly of 4");
        CHECK(!v10_chunks_add(&c, 2, (const uint8_t *)"ab", 2), "an out-of-order chunk is refused");
        CHECK(v10_chunks_add(&c, 0, (const uint8_t *)"ab", 2) && v10_chunks_add(&c, 2, (const uint8_t *)"cd", 2), "in-order chunks");
        CHECK(v10_chunks_complete(&c) && !memcmp(c.buf, "abcd", 4), "the assembled bytes");
        CHECK(!v10_chunks_add(&c, 4, (const uint8_t *)"e", 1), "an overlong chunk is refused");
        v10_chunks_free(&c);
    }

    // ---- JS value semantics that the ENT and the INIT rely on
    {
        KrunkerValue arr_one_hex = ARR(S("#ABCDEF"));
        CHECK(v10_truthy(&arr_one_hex), "an array is truthy");
        CHECK(!v10_truthy(&KRUNKER_V_NUM(0)) && v10_truthy(&KRUNKER_V_NUM(2)), "numbers: 0 is falsy");
        CHECK(!v10_truthy(&KRUNKER_V_STR("")) && v10_truthy(&KRUNKER_V_STR("x")), "strings");
    }

    // ---- the msgpack frames of the v10 client (krunker_proto: the official codec) against the TS codec's bytes
    // (net-bridge/utils.ts: msgpack-lite + the 2-byte ahNum trailer). Fixtures from /tmp generated by the TS codec.
    {
        // msgpack-lite writes integers >= 2^32 as float64; the official dialect writes them as uint64 / int64: the
        // values decode the same, so those are compared as values, not bytes.
        static const struct { const char *name; const char *hex; } fx[] = {
            {"io_init", "92a7696f2d696e6974ac6162636465666768696a6b6c0000"},
            {"load", "93a46c6f6164cd4e20ac6162636465666768696a6b6c0000"},
            {"inst_id", "92a7696e73742d6964a84b4e3a6c6f63616c0000"},
            {"k_record", "94a16b919d09cb3ff428f5c28f5c290203391dcb3fd000000000000001010201002a0acd04d20000"},
            {"ts_pair", "93a2747301050000"},
            {"gv", "93a26776ab73636f72655f6c696d6974cb40040000000000000000"},
            {"lv", "92a26c76030000"},
            {"sync_empty", "92a26169000000"},
        };
        (void)fx;
        // the frame of label + args with trailer 0 (encode_ah(0)): the bytes of the TS frame
        struct { const char *hex; const char *label; KrunkerValue args[8]; uint32_t argc; } cases[] = {
            {"92a7696f2d696e6974ac6162636465666768696a6b6c0000", "io-init", {S("abcdefghijkl")}, 1},
            {"93a46c6f6164cd4e20ac6162636465666768696a6b6c0000", "load", {N(20000), S("abcdefghijkl")}, 2},
            {"92a7696e73742d6964a84b4e3a6c6f63616c0000", "inst-id", {S("KN:local")}, 1},
            {"93a2747301050000", "ts", {N(1), N(5)}, 2},
            {"93a26776ab73636f72655f6c696d6974cb40040000000000000000", "gv", {S("score_limit"), N(2.5)}, 2},
            {"92a26c76030000", "lv", {N(3)}, 1},
            {"92a26169000000", "ai", {N(0)}, 1},
        };
        for (size_t c = 0; c < sizeof cases / sizeof cases[0]; c++) {
            KrunkerBuf buf = {0};
            CHECK(krunker_proto_encode_ah(0, cases[c].label, cases[c].args, cases[c].argc, &buf) == 0, "encode %s", cases[c].label);
            uint8_t want[256];
            size_t wn = strlen(cases[c].hex) / 2;
            for (size_t i = 0; i < wn; i++) {
                unsigned v = 0;
                sscanf(cases[c].hex + 2 * i, "%2x", &v);
                want[i] = (uint8_t)v;
            }
            CHECK(buf.len == wn && !memcmp(buf.data, want, wn), "%s frame bytes = the TS codec's", cases[c].label);
            krunker_buf_free(&buf);
        }

        // k with one player record: [[13 fields]], sendRate 10, server time 1234 (the TS "k" frame)
        {
            KrunkerValue rec[13] = {N(9), N(1.26), N(2), N(3), N(57), N(29), N(0.25), N(1), N(1), N(2), N(1), N(0), N(42)};
            KrunkerValue recs = kv_arr(1, (KrunkerValue[]){kv_arr(13, rec)});
            KrunkerValue args[3] = {recs, N(10), N(1234)};
            KrunkerBuf buf = {0};
            CHECK(krunker_proto_encode_ah(0, "k", args, 3, &buf) == 0, "encode k");
            const char *hex = "94a16b919d09cb3ff428f5c28f5c290203391dcb3fd000000000000001010201002a0acd04d20000";
            size_t wn = strlen(hex) / 2;
            int same = buf.len == wn;
            for (size_t i = 0; same && i < wn; i++) {
                unsigned v = 0;
                sscanf(hex + 2 * i, "%2x", &v);
                same = buf.data[i] == v;
            }
            CHECK(same, "the k frame bytes = the TS codec's (its trailer is the ahNum bytes of the fixture)");
            krunker_buf_free(&buf);
        }

        // the 53-field add record: the same bytes as the TS codec's frame (the record of the TS test)
        {
            KrunkerValue f[53] = {
                S("2"), N(2), N(10), N(21), N(-5), S("Ada"), N(3), N(88), N(100), N(1), N(0), N(0), ARR(N(7), N(8)), N(3), N(5), N(4),
                N(0), NIL, N(0.25), N(9), N(-1), N(-1), N(-1), N(-1), N(8), N(0), N(-1), N(0), S("Ada"), N(17), N(6), N(0),
                S("#112233"), N(7), N(18), N(-1), N(19), N(0), N(1), ARR(N(20), N(21)), N(0), N(16), N(15), N(22), N(0), N(0), N(0),
                N(0), N(0), N(0), N(0), N(0), N(-1),
            };
            KrunkerValue args[2] = {kv_arr(53, f), N(1)};
            KrunkerBuf buf = {0};
            CHECK(krunker_proto_encode_ah(0, "0", args, 2, &buf) == 0, "encode the add record");
            const char *hex = "93a130dc0035a132020a15fba341646103586401000092070803050400c0cb3fd000000000000009ffffffff0800ff00a3416461110600a7233131323233330712ff13000192141500100f160000000000000000ff010000";
            size_t wn = strlen(hex) / 2;
            int same = buf.len == wn;
            for (size_t i = 0; i < wn && same; i++) {
                unsigned v = 0;
                sscanf(hex + 2 * i, "%2x", &v);
                same = buf.data[i] == v;
            }
            CHECK(same, "the add record frame bytes = the TS codec's");
            krunker_buf_free(&buf);
        }

        // the integers past 2^32 decode to the TS values (msgpack-lite's float64 frame, the official dialect's uint64)
        {
            KrunkerValue big[6] = {N(4294967296.0), N(-2147483649.0), N(9007199254740991.0), N(-9007199254740991.0), N(0.5), N(0)};
            KrunkerBuf buf = {0};
            CHECK(krunker_proto_encode_ah(0, "x", big, 6, &buf) == 0, "encode the big integers");
            KrunkerFrame fr;
            memset(&fr, 0, sizeof fr);
            CHECK(krunker_proto_decode(buf.data, buf.len, 0, &fr) == 0, "decode the big integers");
            CHECK(fr.argc == 6, "six values back");
            const double want_big[6] = {4294967296.0, -2147483649.0, 9007199254740991.0, -9007199254740991.0, 0.5, 0};
            for (uint32_t i = 0; i < fr.argc && i < 6; i++) CHECK(fr.args[i].type == KRUNKER_NUM && fr.args[i].u.num == want_big[i], "big value %u", i);
            krunker_frame_free(&fr);
            krunker_buf_free(&buf);
        }

        // client frames (ahNum 0x2a in the trailer): en and ct decode to the TS args
        {
            const char *en_hex = "98a2656e9304ffff1012c2c3a0c0020a";
            uint8_t fr_bytes[64];
            size_t n = strlen(en_hex) / 2;
            for (size_t i = 0; i < n; i++) { unsigned v = 0; sscanf(en_hex + 2 * i, "%2x", &v); fr_bytes[i] = (uint8_t)v; }
            KrunkerFrame fr;
            memset(&fr, 0, sizeof fr);
            CHECK(krunker_proto_decode(fr_bytes, n, 0, &fr) == 0, "decode the en frame");
            CHECK(fr.ah_echo == 0x2a && !strcmp(fr.name, "en") && fr.argc == 7, "en: ahNum 42, label, seven args");
            if (fr.argc == 7) {
                CHECK(fr.args[0].type == KRUNKER_ARRAY && fr.args[0].len == 3, "en arg 0: the join array");
                CHECK(vequal(&fr.args[1], &KRUNKER_V_NUM(16)), "en arg 1 is 16");
                CHECK(vequal(&fr.args[3], &KRUNKER_V_BOOL(0)), "en arg 3 is false");
                CHECK(vequal(&fr.args[4], &KRUNKER_V_BOOL(1)), "en arg 4 is true");
            }
            krunker_frame_free(&fr);
            const char *ct_hex = "92a26374ab68656c6c6f207468657265020a";
            n = strlen(ct_hex) / 2;
            for (size_t i = 0; i < n; i++) { unsigned v = 0; sscanf(ct_hex + 2 * i, "%2x", &v); fr_bytes[i] = (uint8_t)v; }
            memset(&fr, 0, sizeof fr);
            CHECK(krunker_proto_decode(fr_bytes, n, 0, &fr) == 0 && !strcmp(fr.name, "ct") && fr.ah_echo == 0x2a, "decode the ct frame");
            KrunkerValue chat = fr.args[0];
            uint8_t text[V10_CHAT_TEXT_SIZE];
            CHECK(v10_encode_chat(&chat, text) == 12 && !memcmp(text, "hello there\0", 12), "ct text -> CHAT_SEND bytes");
            krunker_frame_free(&fr);
        }

        // a frame too short for a trailer and a non-array payload are refused
        {
            KrunkerFrame fr;
            uint8_t two[2] = {0x92, 0x00};
            CHECK(krunker_proto_decode(two, 2, 0, &fr) != 0, "a 2-byte frame is refused");
            uint8_t not_array[3] = {0x01, 0x00, 0x00};
            CHECK(krunker_proto_decode(not_array, 3, 0, &fr) != 0, "a non-array frame is refused");
        }
    }

    /* ---- NET_PACKET_END (16, 330 bytes): end [endType, didWin, endData, winner, endIndex, reward, tmpBPData];
     * endData has an empty vo and mts (the client's map pick takes the fallback when mts is empty) */
    {
        uint8_t endp[330];
        memset(endp, 0, sizeof endp);
        endp[0] = 1; /* a victory */
        V10Out o;
        v10_out_init(&o);
        v10_map_end(&o, endp, 330);
        {
            const KrunkerValue kv[8] = {S("vo"), KRUNKER_V_MAP(NULL, 0), S("mts"), KRUNKER_V_ARRAY(NULL, 0), S("tms"), NIL, S("ed"), KRUNKER_V_ARRAY(NULL, 0)};
            const KrunkerValue end_data = KRUNKER_V_MAP(kv, 4);
            const KrunkerValue want[] = {N(0), B(1), end_data, N(-1), N(0), NIL, NIL};
            CHECK(msg_is(&o, 0, "end", want, 7), "END -> end [0, true, endData with an empty vo and mts, -1, 0, null, null]");
        }
        v10_out_free(&o);
        endp[0] = 0xff; /* a draw */
        v10_out_init(&o);
        v10_map_end(&o, endp, 330);
        CHECK(o.count == 1 && o.msgs[0].args[1].type == KRUNKER_NUM && o.msgs[0].args[1].u.num == -1, "END with result -1 is a draw (didWin -1)");
        v10_out_free(&o);
        v10_out_init(&o);
        v10_map_end(&o, endp, 329);
        CHECK(o.stop != NULL, "a short END stops the translation");
        v10_out_free(&o);
    }

    printf("v10_translate_test: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
