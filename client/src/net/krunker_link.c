// Official Krunker session: packet translation (client/include/krunker_link.h). Pure: no sockets,
// no Client; the transport (krunker_net.c) owns the WebSocket and the client glue.
//
// Where Krunker's shapes and ours differ, this file is the translation layer:
//   - ids: Krunker's numeric player sid is our uid; "you" is the "0" entry whose id equals the
//     socket id io-init gave (players.add, game.deob.js:220268).
//   - "k" carries no health, our STATE does: the last "0" / "h" health per sid is kept here.
//   - "k" angles are degrees (iiïìííï: x * PI / 180), "0" yaw and "l" yaw radians.
//   - "l" slideTimer / canSlideT are JS milliseconds; our RECON keeps seconds (+ slide_ms).
//   - our client sends one NetInputPacket per input frame, batched per clientSendRate window
//     (recon.c, NET_PACKET_INPUT_BATCH): that window becomes one delta-compressed "q".
//   - cosmetics / skins / account fields are not mapped (a guest has none): SPAWNs carry none.
#include <krunker_link.h>
#include <krunker_cheat.h>
#include <krunker_altcha.h>
#include <cJSON.h>
#include <map_catalog.h>
#include <social_v7.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------- helpers

static const KrunkerValue *arg(const KrunkerFrame *f, const uint32_t i) { return krunker_frame_arg(f, i); }
static int is_nil(const KrunkerValue *v) { return !v || v->type == KRUNKER_NIL; }
static double num(const KrunkerValue *v, const double def) { return krunker_num(v, def); }
static int truthy(const KrunkerValue *v) {
    if (is_nil(v)) return 0;
    if (v->type == KRUNKER_BOOL) return v->u.boolean;
    if (v->type == KRUNKER_NUM) return v->u.num != 0 && !isnan(v->u.num);
    if (v->type == KRUNKER_STR) return v->len > 0;
    return 1;
}
// JS String(v) of a string / number id ("abc", 12 -> "12"); "" otherwise
static void id_string(const KrunkerValue *v, char *out, const size_t n) {
    out[0] = 0;
    if (krunker_is(v, KRUNKER_STR)) snprintf(out, n, "%s", krunker_str(v));
    else if (krunker_is(v, KRUNKER_NUM)) snprintf(out, n, "%.17g", v->u.num);
}
static double js_round(const double x) { return floor(x + 0.5); } // Math.round (-0.5 -> -0, not -1)

static void note(KrunkerLink *l, const KrunkerLinkEvent kind, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void note(KrunkerLink *l, const KrunkerLinkEvent kind, const char *fmt, ...) {
    char text[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    if (l->io.event) l->io.event(l->io.user, kind, text);
}

static void emit(KrunkerLink *l, const uint8_t type, const void *payload, const uint16_t length) {
    if (!l->io.emit || length > NET_MAX_PAYLOAD) return;
    NetPacket packet;
    memset(&packet, 0, sizeof(packet));
    packet.type = type;
    packet.length = length;
    if (length) memcpy(packet.payload, payload, length);
    l->io.emit(l->io.user, &packet);
}

static KrunkerLinkPlayer *player(KrunkerLink *l, const int sid, const int create) {
    for (int i = 0; i < l->player_count; i++) {
        if (l->players[i].sid == sid) return &l->players[i];
    }
    if (!create || l->player_count >= KRUNKER_LINK_MAX_PLAYERS) return NULL;
    KrunkerLinkPlayer *p = &l->players[l->player_count++];
    p->sid = sid;
    p->health = 100.0f;
    p->max_health = 100;
    return p;
}

static void player_remove(KrunkerLink *l, const int sid) {
    for (int i = 0; i < l->player_count; i++) {
        if (l->players[i].sid != sid) continue;
        l->players[i] = l->players[--l->player_count];
        return;
    }
}

int krunker_link_send(KrunkerLink *l, const char *wire, const KrunkerValue *args, const size_t argc) {
    KrunkerBuf buf = {0};
    if (!wire || krunker_proto_encode(&l->codec, wire, args, argc, &buf) != KRUNKER_PROTO_OK) {
        krunker_buf_free(&buf);
        return 0;
    }
    if (l->io.send) l->io.send(l->io.user, buf.data, buf.len);
    l->frames_out++;
    krunker_buf_free(&buf);
    return 1;
}

static int send_c2s(KrunkerLink *l, const KrunkerC2S id, const KrunkerValue *args, const size_t argc) {
    return krunker_link_send(l, krunker_c2s_wire(id), args, argc);
}

// ---------------------------------------------------------------- lifecycle

void krunker_link_init(KrunkerLink *l, const KrunkerLinkIO *io, const int64_t ahkey) {
    memset(l, 0, sizeof(*l));
    if (io) l->io = *io;
    krunker_session_init(&l->codec, ahkey);
    krunker_link_reset(l);
}

void krunker_link_reset(KrunkerLink *l) {
    const KrunkerLinkIO io = l->io;
    const int64_t ahkey = l->codec.ahkey;
    memset(l, 0, sizeof(*l));
    l->io = io;
    krunker_session_init(&l->codec, ahkey);
    l->my_sid = -1;
    l->unsupported_map = -1;
}

// ---------------------------------------------------------------- inbound

// init [mapIndex, modeIndex, eventIndex, ...] (INIT_GAME, game.deob.js:219289 -> 219300):
// mapIndex indexes the server's map list (fetched by index from the API, :219292); our catalogue's
// first MAP_CATALOG_OFFICIAL_COUNT entries are Krunker's official maps in that order.
static void on_init(KrunkerLink *l, const KrunkerFrame *f) {
    const int map = (int) num(arg(f, 0), -1), mode = (int) num(arg(f, 1), 0);
    if (map < 0 || map >= MAP_CATALOG_OFFICIAL_COUNT) {
        l->unsupported_map = map;
        note(l, KRUNKER_LINK_EV_UNSUPPORTED, "the server plays map index %d, which this client does not have", map);
        return;
    }
    l->unsupported_map = -1;
    NetInitPacket init;
    memset(&init, 0, sizeof(init));
    init.major = NET_PROTOCOL_MAJOR;
    init.minor = NET_PROTOCOL_MINOR;
    init.patch = NET_PROTOCOL_PATCH;
    init.map_index = map;
    init.mode_index = mode; // INIT carries the Krunker mode id (main.c client_apply_init)
    init.tick_rate = 30;    // Krunker tickFrequency default (game.deob.js:186955)
    init.client_id = 0;
    init.ruleset = NET_INIT_RULESET;
    emit(l, NET_PACKET_INIT, &init, sizeof(init));
    note(l, KRUNKER_LINK_EV_NOTE, "init: map %d, mode %d", map, mode);
}

// "0" ADD_PLAYERS: a flat list, 53 (0x35) values per player (game.deob.js:220266-220330); the
// players.add arguments (:171902, player.init :169089): [0] id (socket id) [1] sid [2] x [3] y
// [4] z [5] name [6] classIndex [7] health (null: maxHealth) [8] maxHealth [9] team [15] secIndex
// [18] xDire (yaw, radians) [23] attachIndex [28] alias.
#define KRUNKER_ADD_STRIDE 53
static void on_add_players(KrunkerLink *l, const KrunkerFrame *f) {
    const KrunkerValue *list = arg(f, 0);
    if (!krunker_is(list, KRUNKER_ARRAY)) return;
    for (uint32_t i = 0; i + 1 < list->len; i += KRUNKER_ADD_STRIDE) {
        const KrunkerValue *e = list->u.items + i;
        const uint32_t left = list->len - i;
#define E(k) ((uint32_t) (k) < left ? &e[(k)] : NULL)
        if (!krunker_is(E(1), KRUNKER_NUM)) continue;
        NetSpawnPacket s;
        memset(&s, 0, sizeof(s));
        s.uid = (int32_t) E(1)->u.num;
        char id[64];
        id_string(E(0), id, sizeof(id));
        s.is_you = l->socket_id[0] && !strcmp(id, l->socket_id);
        s.active = 1;
        s.class_index = (int32_t) num(E(6), 0);
        s.team = (int32_t) num(E(9), 0);
        s.max_health = (int32_t) num(E(8), 100);
        s.health = (float) (is_nil(E(7)) ? s.max_health : num(E(7), s.max_health));
        s.position[0] = (float) num(E(2), 0);
        s.position[1] = (float) num(E(3), 0);
        s.position[2] = (float) num(E(4), 0);
        s.direction[0] = 0.0f;                     // yDire is not in "0"
        s.direction[1] = (float) num(E(18), 0);    // xDire
        const char *name = krunker_str(E(5));
        if (name && name[0]) snprintf(s.name, sizeof(s.name), "%s", name);
        else snprintf(s.name, sizeof(s.name), "Guest_%d", s.uid); // players.add's default
        s.secondary_index = (int32_t) num(E(15), -1);
        s.cosmetics = PLAYER_COSMETICS_NONE;
        s.attach_index = (int32_t) num(E(23), 0);
        s.head_mlt = 1.0f;
        s.cosmetics_v7 = PLAYER_COSMETICS_V7_NONE;
#undef E
        KrunkerLinkPlayer *p = player(l, s.uid, 1);
        if (p) {
            p->health = s.health;
            p->max_health = s.max_health;
        }
        if (s.is_you) {
            const int first = l->my_sid != s.uid;
            l->my_sid = s.uid;
            if (first) note(l, KRUNKER_LINK_EV_JOINED, "joined as sid %d (%s)", s.uid, s.name);
        }
        emit(l, NET_PACKET_SPAWN, &s, sizeof(s));
    }
}

// "k" UPDATE_PLAYERS [list, sendRateA, servTim] (game.deob.js:220529): 13 values per player,
// [0] sid [1..3] x y z [4] xDire (deg) [5] yDire (deg) [6] stepVal [7] onGround [8] crouchVal
// [9] weapon (loadout index) [10] aimVal [11] grapple [12] ping. Our own sid is skipped (as in JS).
#define KRUNKER_K_STRIDE 13
static void on_update_players(KrunkerLink *l, const KrunkerFrame *f) {
    const KrunkerValue *list = arg(f, 0);
    if (!krunker_is(list, KRUNKER_ARRAY)) return;
    for (uint32_t i = 0; i + KRUNKER_K_STRIDE <= list->len; i += KRUNKER_K_STRIDE) {
        const KrunkerValue *e = list->u.items + i;
        if (!krunker_is(&e[0], KRUNKER_NUM)) continue;
        const int sid = (int) e[0].u.num;
        if (sid == l->my_sid) continue;
        const KrunkerLinkPlayer *p = player(l, sid, 0);
        NetStatePacket s;
        memset(&s, 0, sizeof(s));
        s.uid = sid;
        s.ack_seq = 0;
        s.active = 1;
        s.loadout_index = (int32_t) num(&e[9], 0);
        s.position[0] = (float) num(&e[1], 0);
        s.position[1] = (float) num(&e[2], 0);
        s.position[2] = (float) num(&e[3], 0);
        s.direction[0] = (float) (num(&e[5], 0) * (M_PI / 180.0)); // pitch
        s.direction[1] = (float) (num(&e[4], 0) * (M_PI / 180.0)); // yaw
        s.health = p ? p->health : 100.0f;
        s.aim_val = (float) num(&e[10], 0);
        s.crouch_val = (float) num(&e[8], 0);
        emit(l, NET_PACKET_STATE, &s, sizeof(s));
    }
}

// "l" SYNC_PLAYER [state] (game.deob.js:220335): [0] inputSN [1] ping [2..4] x y z [5] velY
// [6] velX [7] velZ [8] xDire [9] onGround [10] didJump [11] didJumpW [12] wallJumpC [13] onWall
// [14] onLadder [15] aimVal [16] crouchVal [17] weapon index [18] slideTimer (ms) [19] canSlide
// [20] onTerrain [21] xVelC [22] zVelC [23] grapple || null [24] canSlideT (ms). Always answered
// with PONG "v" (first statement of the handler).
static void on_sync_player(KrunkerLink *l, const KrunkerFrame *f) {
    send_c2s(l, KRUNKER_C2S_PONG, NULL, 0);
    const KrunkerValue *a = arg(f, 0);
    if (!krunker_is(a, KRUNKER_ARRAY)) return;
#define A(k) krunker_at(a, (k))
    NetReconPacket r;
    memset(&r, 0, sizeof(r));
    r.ack_seq = (int32_t) num(A(0), NET_RECON_SEQ_ALL);
    r.position[0] = (float) num(A(2), 0);
    r.position[1] = (float) num(A(3), 0);
    r.position[2] = (float) num(A(4), 0);
    r.velocity[0] = (float) num(A(6), 0);
    r.velocity[1] = (float) num(A(5), 0);
    r.velocity[2] = (float) num(A(7), 0);
    r.yaw = (float) num(A(8), 0);
    r.wall_jump_count = (uint16_t) num(A(12), 0);
    r.on_wall = truthy(A(13)) ? 1 : 0;
    r.aim_val = (float) num(A(15), 0);
    r.crouch_val = (float) num(A(16), 0);
    r.loadout_index = (int32_t) num(A(17), 0);
    r.slide_ms = num(A(18), 0);
    r.slide_timer = (float) (r.slide_ms / 1000.0);
    r.x_vel_cylinder = (float) num(A(21), 0);
    r.z_vel_cylinder = (float) num(A(22), 0);
    r.can_slide_timer = (float) (num(A(24), 0) / 1000.0);
    r.flags = (truthy(A(9)) ? NET_RECON_ON_GROUND : 0) | (truthy(A(10)) ? NET_RECON_DID_JUMP : 0) |
              (truthy(A(11)) ? NET_RECON_DID_JUMP_W : 0) | (truthy(A(14)) ? NET_RECON_ON_LADDER : 0) |
              (truthy(A(19)) ? NET_RECON_CAN_SLIDE : 0) | (truthy(A(20)) ? NET_RECON_ON_TERRAIN : 0);
    const KrunkerValue *g = A(23);
    if (krunker_is(g, KRUNKER_ARRAY) && g->len >= 3) {
        r.grapple_active = 1;
        for (uint32_t k = 0; k < 4 && k < g->len; k++) r.grapple[k] = (float) num(&g->u.items[k], 0);
    }
    const KrunkerValue *ping = A(1);
#undef A
    emit(l, NET_PACKET_RECON, &r, sizeof(r));
    if (krunker_is(ping, KRUNKER_NUM)) {
        const NetPingResultPacket pr = {.ping_ms = (int32_t) ping->u.num};
        emit(l, NET_PACKET_PING_RESULT, &pr, sizeof(pr));
    }
}

// "3" KILL_PLAYER [victimSid, victimDeaths, killerSid, deathStats, killInfo, hideFeed]
// (game.deob.js:220579; killInfo {wId, fs, stk, pkg} at :220620's feed)
static void on_kill(KrunkerLink *l, const KrunkerFrame *f) {
    if (!krunker_is(arg(f, 0), KRUNKER_NUM)) return;
    NetKillPacket k;
    memset(&k, 0, sizeof(k));
    k.victim_uid = (int32_t) arg(f, 0)->u.num;
    k.victim_deaths = (int32_t) num(arg(f, 1), 0);
    k.killer_uid = (int32_t) num(arg(f, 2), -1);
    k.weapon_index = -1;
    const KrunkerValue *info = arg(f, 4);
    if (krunker_is(info, KRUNKER_MAP)) {
        k.weapon_index = (int32_t) num(krunker_get(info, "wId"), -1);
        k.fists = truthy(krunker_get(info, "fs")) ? NET_KILL_FISTS : 0;
        k.headshot = truthy(krunker_get(info, "hs")) ? 1 : 0;
    } else if (krunker_is(info, KRUNKER_NUM)) {
        k.weapon_index = (int32_t) info->u.num;
    }
    k.killer_class = -1;
    k.respawn_delay = GAME_DEATH_DELAY;
    emit(l, NET_PACKET_KILL, &k, sizeof(k));
}

// "4" DO_DAMAGE [targetSid, damage, crit, kill, isAI, silent] (game.deob.js:221254)
static void on_damage(KrunkerLink *l, const KrunkerFrame *f) {
    NetHitPacket h;
    memset(&h, 0, sizeof(h));
    h.target_uid = (int32_t) num(arg(f, 0), -1);
    h.damage = (int32_t) js_round(num(arg(f, 1), 0));
    h.crit = truthy(arg(f, 2)) ? 1 : 0;
    h.kill = truthy(arg(f, 3)) ? 1 : 0;
    h.ai = truthy(arg(f, 4)) ? 1 : 0;
    emit(l, NET_PACKET_HIT, &h, sizeof(h));
}

// "h" CHANGE_HEALTH [health, sid (null: you), srcX, srcZ, ..., resetMax] (game.deob.js:221040)
static void on_health(KrunkerLink *l, const KrunkerFrame *f) {
    const int sid = is_nil(arg(f, 1)) ? l->my_sid : (int) num(arg(f, 1), -1);
    if (sid < 0) return;
    NetHealthPacket h;
    memset(&h, 0, sizeof(h));
    h.uid = sid;
    h.health = (float) num(arg(f, 0), 0);
    if (krunker_is(arg(f, 2), KRUNKER_NUM) && krunker_is(arg(f, 3), KRUNKER_NUM)) {
        h.has_source = 1;
        h.source_x = (float) arg(f, 2)->u.num;
        h.source_z = (float) arg(f, 3)->u.num;
    }
    KrunkerLinkPlayer *p = player(l, sid, 1);
    if (p) {
        p->health = h.health;
        if (h.health > (float) p->max_health) p->max_health = (int) ceilf(h.health);
    }
    emit(l, NET_PACKET_HEALTH, &h, sizeof(h));
}

// "ch" ADD_CHAT [team, name, text, kind, color, ...] (game.deob.js:221172): kind 0 player line,
// 1 / 4 game line, 2 server line (color), 3 trade request (text = the sender's sid)
static void on_chat(KrunkerLink *l, const KrunkerFrame *f) {
    const char *text = krunker_str(arg(f, 2));
    const int kind = (int) num(arg(f, 3), 0);
    if (!text || kind == 3) return;
    NetChatPacket c;
    memset(&c, 0, sizeof(c));
    c.uid = -1;
    c.kind = kind == 0 ? NET_CHAT_PLAYER : kind == 2 ? NET_CHAT_SERVER : NET_CHAT_GAME;
    const char *name = krunker_str(arg(f, 1));
    snprintf(c.name, sizeof(c.name), "%s", name ? name : "");
    snprintf(c.text, sizeof(c.text), "%s", text);
    emit(l, NET_PACKET_CHAT, &c, sizeof(c));
}

// "cc" CHEAT_CHECK [nonce, unused, ciphertextB64] -> "ccr" [nonce, payloadB64, fnvHex]
static void on_cheat_check(KrunkerLink *l, const KrunkerFrame *f) {
    char nonce[256];
    id_string(arg(f, 0), nonce, sizeof(nonce));
    const char *ct = krunker_str(arg(f, 2));
    KrunkerCcr ccr;
    memset(&ccr, 0, sizeof(ccr));
    // a non-zero answer sends no ccr at all (the game's outer try/catch swallows those failures)
    const int rc = !nonce[0] || !ct ? -1 : l->io.cheat ? l->io.cheat(l->io.user, nonce, ct, &ccr) : krunker_cheat_answer(nonce, ct, &ccr);
    if (rc != 0) {
        l->cc_failed++;
        note(l, KRUNKER_LINK_EV_NOTE, "cheat check %s: not answered (%s)", nonce, ct ? "no evaluator / undecodable" : "no ciphertext");
        krunker_ccr_free(&ccr);
        return;
    }
    // payload_b64 may be "" (an undefined result): still sent
    const KrunkerValue args[] = {KRUNKER_V_STR(ccr.nonce ? ccr.nonce : nonce), KRUNKER_V_STR(ccr.payload_b64 ? ccr.payload_b64 : ""),
                                 KRUNKER_V_STR(ccr.fnv)};
    if (send_c2s(l, KRUNKER_C2S_CHEAT_CHECK_RESPONSE, args, 3)) l->cc_answered++;
    krunker_ccr_free(&ccr);
}

// "cpt" [challenge] (game.deob.js:98132): the ALTCHA widget's proof of work, answered as "cptR"
// [payload] (krunker_altcha.c). The challenge is an object ({algorithm, challenge, salt, signature,
// maxnumber}); a JSON string of it is taken too.
static void on_captcha(KrunkerLink *l, const KrunkerFrame *f) {
    const KrunkerValue *c = arg(f, 0);
    char alg[32] = "", chal[80] = "", salt[512] = "", sig[512] = "";
    long max = 0;
    if (krunker_is(c, KRUNKER_MAP)) {
        const char *v;
        if ((v = krunker_str(krunker_get(c, "algorithm")))) snprintf(alg, sizeof(alg), "%s", v);
        if ((v = krunker_str(krunker_get(c, "challenge")))) snprintf(chal, sizeof(chal), "%s", v);
        if ((v = krunker_str(krunker_get(c, "salt")))) snprintf(salt, sizeof(salt), "%s", v);
        if ((v = krunker_str(krunker_get(c, "signature")))) snprintf(sig, sizeof(sig), "%s", v);
        max = (long) num(krunker_get(c, "maxnumber"), num(krunker_get(c, "maxNumber"), 0));
    } else if (krunker_is(c, KRUNKER_STR)) {
        cJSON *j = cJSON_Parse(krunker_str(c));
        const char *v;
        if ((v = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "algorithm")))) snprintf(alg, sizeof(alg), "%s", v);
        if ((v = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "challenge")))) snprintf(chal, sizeof(chal), "%s", v);
        if ((v = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "salt")))) snprintf(salt, sizeof(salt), "%s", v);
        if ((v = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "signature")))) snprintf(sig, sizeof(sig), "%s", v);
        const cJSON *m = cJSON_GetObjectItemCaseSensitive(j, "maxnumber");
        if (cJSON_IsNumber(m)) max = (long) m->valuedouble;
        cJSON_Delete(j);
    }
    char *payload = NULL;
    const int rc = l->io.altcha ? l->io.altcha(l->io.user, alg, chal, salt, sig, max, &payload)
                                : krunker_altcha_solve(alg, chal, salt, sig, max, &payload);
    if (rc == 1) { // the transport solves it in slices and sends "cptR" itself
        note(l, KRUNKER_LINK_EV_NOTE, "security challenge (%s proof of work, up to %ld): solving", alg, max);
        return;
    }
    if (rc != 0 || !payload) {
        free(payload);
        note(l, KRUNKER_LINK_EV_CAPTCHA, "a security challenge (%s) this client cannot answer", alg[0] ? alg : "unknown");
        return;
    }
    const KrunkerValue args[] = {KRUNKER_V_STR(payload)};
    if (send_c2s(l, KRUNKER_C2S_cptR, args, 1)) l->captchas_solved++;
    note(l, KRUNKER_LINK_EV_NOTE, "security challenge answered (%s proof of work)", alg);
    free(payload);
}

static void on_error(KrunkerLink *l, const KrunkerFrame *f) {
    NetErrorPacket e;
    memset(&e, 0, sizeof(e));
    const char *msg = krunker_str(arg(f, 0));
    snprintf(e.message, sizeof(e.message), "%s", msg && msg[0] ? msg : "Disconnected by krunker.io");
    char code[16];
    id_string(arg(f, 1), code, sizeof(code));
    snprintf(e.code, sizeof(e.code), "%s", code[0] ? code : "000000");
    emit(l, NET_PACKET_ERROR, &e, sizeof(e));
    note(l, KRUNKER_LINK_EV_ERROR, "%s", e.message);
}

int krunker_link_on_frame(KrunkerLink *l, const uint8_t *data, const size_t len) {
    KrunkerFrame f;
    const int err = krunker_proto_decode(data, len, 0, &f);
    if (err != KRUNKER_PROTO_OK) {
        note(l, KRUNKER_LINK_EV_NOTE, "malformed frame (%zu bytes): %s", len, krunker_proto_strerror(err));
        return 0;
    }
    l->frames_in++;
    switch (krunker_s2c_lookup(f.name, f.name_len)) {
        case KRUNKER_S2C_IO_INIT:
            id_string(arg(&f, 0), l->socket_id, sizeof(l->socket_id));
            note(l, KRUNKER_LINK_EV_NOTE, "io-init: socket id %s", l->socket_id);
            break;
        case KRUNKER_S2C_INIT_LOAD: // iiïiîïí: the client answers "load" once it may load
            send_c2s(l, KRUNKER_C2S_LOAD, NULL, 0);
            break;
        case KRUNKER_S2C_INIT_GAME: on_init(l, &f); break;
        case KRUNKER_S2C_ENTER_GAME: note(l, KRUNKER_LINK_EV_NOTE, "frs: the server took our en"); break;
        case KRUNKER_S2C_START_GAME: break; // timers / team / class for the menu: our SPAWN path covers it
        case KRUNKER_S2C_ADD_PLAYERS: on_add_players(l, &f); break;
        case KRUNKER_S2C_UPDATE_PLAYERS: on_update_players(l, &f); break;
        case KRUNKER_S2C_SYNC_PLAYER: on_sync_player(l, &f); break;
        case KRUNKER_S2C_SYNC_FAILED: // iiîïîìï: the server dropped our inputs; the next "q" restarts the deltas
            memset(l->last_set, 0, sizeof(l->last_set));
            l->prev_angle_count = 0;
            l->resync = 1;
            break;
        case KRUNKER_S2C_REMOVE_PLAYER: {
            const NetLeavePacket leave = {.uid = (int32_t) num(arg(&f, 0), -1)};
            player_remove(l, leave.uid);
            emit(l, NET_PACKET_LEAVE, &leave, sizeof(leave));
            break;
        }
        case KRUNKER_S2C_KILL_PLAYER: on_kill(l, &f); break;
        case KRUNKER_S2C_DO_DAMAGE: on_damage(l, &f); break;
        case KRUNKER_S2C_CHANGE_HEALTH: on_health(l, &f); break;
        case KRUNKER_S2C_ADD_CHAT: on_chat(l, &f); break;
        case KRUNKER_S2C_SOCKET_PING: send_c2s(l, KRUNKER_C2S_PING, NULL, 0); break; // iìîíîïí: "po"
        case KRUNKER_S2C_CHEAT_CHECK: on_cheat_check(l, &f); break;
        case KRUNKER_S2C_cpt: on_captcha(l, &f); break;
        case KRUNKER_S2C_cptR: note(l, KRUNKER_LINK_EV_NOTE, "security challenge accepted"); break; // _hideCaptcha
        case KRUNKER_S2C_ERROR: on_error(l, &f); break;
        case KRUNKER_S2C_DISCONNECT: note(l, KRUNKER_LINK_EV_ERROR, "the server disconnected us (dc)"); break;
        default: l->unknown_in++; break;
    }
    krunker_frame_free(&f);
    return 1;
}

// ---------------------------------------------------------------- outbound

// "en" ENTER_GAME (game.deob.js:208279): the 35-value enter array, then 11 client flags. A guest
// owns no cosmetics, so every cosmetic is a fresh krunker.io guest's saved value (:210435-210491:
// -1, team 1, spray 2482, secondary 2, streak indexes parseInt(null) = NaN, charms [undefined, -1]);
// only the class, the secondary, spectate, lag compensation, the sight, challenge and anon mode
// come from our ENT.
static int send_enter(KrunkerLink *l, const void *payload, const uint16_t length) {
    NetEntPacket ent;
    memset(&ent, 0, sizeof(ent));
    ent.secondary_index = -1;
    ent.lag_comp = 1.0f;
    memcpy(&ent, payload, length < sizeof(ent) ? length : sizeof(ent));
    if (length <= offsetof(NetEntPacket, secondary_index)) ent.secondary_index = -1;
    if (length <= offsetof(NetEntPacket, lag_comp)) ent.lag_comp = 1.0f;
    if (length <= offsetof(NetEntPacket, attach_index)) ent.attach_index = 0;
    const double nan = NAN;
    const KrunkerValue skins[] = {KRUNKER_V_NUM(-1), KRUNKER_V_NUM(-1)};
    const KrunkerValue charms[] = {KRUNKER_V_NIL, KRUNKER_V_NUM(-1)};
    const KrunkerValue enter[35] = {
        KRUNKER_V_NUM(ent.class_index),                                   // [0] classIndex
        KRUNKER_V_NUM(2482),                                              // [1] sprayIndex (0x9b2)
        KRUNKER_V_ARRAY(skins, 2),                                        // [2] weapon skins
        KRUNKER_V_NUM(-1),                                                // [3] hatIndex
        KRUNKER_V_NUM(-1),                                                // [4] bodyIndex
        KRUNKER_V_NUM(ent.secondary_index >= 0 ? ent.secondary_index : 2),// [5] secondaryInd
        KRUNKER_V_NUM(ent.spectate ? 1 : 0),                              // [6] spectate
        KRUNKER_V_NUM(ent.chall_mode ? 1 : 0),                            // [7] challenge mode
        KRUNKER_V_NUM(1),                                                 // [8] team pick (iïíiîiì)
        KRUNKER_V_NUM(-1),                                                // [9] meleeIndex
        KRUNKER_V_NUM(-1),                                                // [10] skinColIndex
        KRUNKER_V_NUM(ent.lag_comp),                                      // [11] lagComp
        KRUNKER_V_NUM(ent.attach_index >= 0 ? ent.attach_index : 0),      // [12] attachIndex
        KRUNKER_V_NUM(-1),                                                // [13] pcStatIndex
        KRUNKER_V_NUM(-1),                                                // [14] dyeIndex
        KRUNKER_V_NUM(-1),                                                // [15] shoeIndex
        KRUNKER_V_NUM(-1),                                                // [16] waistIndex
        KRUNKER_V_NUM(-1),                                                // [17] hairCol
        KRUNKER_V_NUM(-1),                                                // [18] chatCol
        KRUNKER_V_NUM(ent.anon_mode ? 1 : 0),                             // [19] anonMode
        KRUNKER_V_NUM(-1),                                                // [20] faceIndex
        KRUNKER_V_NUM(-1),                                                // [21] petIndex
        KRUNKER_V_NUM(0),                                                 // [22] verifiedBadge
        KRUNKER_V_NUM(0),                                                 // [23] premiumBadge
        KRUNKER_V_NUM(0),                                                 // [24] customBadge
        KRUNKER_V_NUM(-1),                                                // [25] wristIndex
        KRUNKER_V_NUM(nan), KRUNKER_V_NUM(nan), KRUNKER_V_NUM(nan),       // [26..28] streakIndex1..3
        KRUNKER_V_ARRAY(charms, 2),                                       // [29] charms
        KRUNKER_V_NUM(-1),                                                // [30] backIndex
        KRUNKER_V_NUM(-1),                                                // [31] headIndex
        KRUNKER_V_NUM(-1),                                                // [32] playerCardIndex
        KRUNKER_V_NUM(0),                                                 // [33] rankBadge
        KRUNKER_V_NUM(-1),                                                // [34] titleIndex
    };
    const KrunkerValue args[] = {
        KRUNKER_V_ARRAY(enter, 35),
        KRUNKER_V_NUM(16), KRUNKER_V_NUM(18), // constants of the call
        KRUNKER_V_BOOL(0), KRUNKER_V_BOOL(0), KRUNKER_V_BOOL(0), KRUNKER_V_BOOL(0), // tamper flags: all clear
        KRUNKER_V_BOOL(0),                     // Object.prototype.createDate defined
        KRUNKER_V_BOOL(0),                     // localStorage "ssjunkerconfig"
        KRUNKER_V_NIL,                         // windows[11].header ('' -> undefined)
        KRUNKER_V_BOOL(0),                     // window.chonkercheats
        KRUNKER_V_BOOL(0),
    };
    l->joined = 1;
    return send_c2s(l, KRUNKER_C2S_ENTER_GAME, args, sizeof(args) / sizeof(args[0]));
}

// One input frame as the 15 fields of game.deob.js:221748:
// [isn, dt ms, pitch*1000, yaw*1000, moveDir, shoot, ads, jump, reload, crouch, scroll, wSwap,
//  1 - speedLmt, reset (2 last, 1 start), interact]
static void input_fields(const NetInputPacket *in, double v[15]) {
    v[0] = in->seq;
    v[1] = js_round(in->delta * 1000.0); // * config.deltaMlt (1)
    v[2] = js_round(1000.0 * (js_round(in->x_dir * 1000.0) / 1000.0));
    v[3] = js_round(1000.0 * (js_round(in->y_dir * 1000.0) / 1000.0));
    v[4] = in->move_dir >= -1 && in->move_dir <= 7 ? in->move_dir : -1;
    v[5] = in->flags & NET_INPUT_SHOOT ? 1 : 0;
    v[6] = in->flags & NET_INPUT_SCOPE ? 1 : 0;
    v[7] = in->flags & NET_INPUT_JUMP ? 1 : 0;
    v[8] = in->flags & NET_INPUT_RELOAD ? 1 : 0;
    v[9] = in->flags & NET_INPUT_CROUCH ? 1 : 0;
    v[10] = in->scroll;
    v[11] = in->swap;
    v[12] = in->slowdown / 10.0;
    v[13] = in->flags & NET_INPUT_RESET_LAST ? 2 : in->flags & NET_INPUT_RESET_START ? 1 : 0;
    v[14] = in->flags & NET_INPUT_INTERACT ? 1 : 0;
}

// The "q" of one send window (game.deob.js:221755-221830):
//   ["q", resync, firstIsn, dts, dtLen, angles?, changes?, syncTimes?]
//   dts      every frame's dt as one string of equal-width numbers (dtLen = the width), or the
//            plain array with dtLen 0 when the widths differ
//   angles   pitch, yaw of the first frame, then per frame the change from the frame before
//            (cut to the first pair when nothing turned); 0 when equal to the last window's
//   changes  {"<frame>-<field>": value} for fields 4..14 that differ from the last value sent
//            (kept across windows); 0 when none
// angles and changes are only appended when needed (see the push conditions below). syncTimes
// (the frames that fired, servTim) are not produced: see the gap list in krunker_net.h.
static int send_input_window(KrunkerLink *l, const NetInputPacket *frames, const int count) {
    if (count <= 0) return 0;
    double dts[NET_INPUT_BATCH_MAX];
    double angles[2 + 2 * NET_INPUT_BATCH_MAX];
    int angle_count = 0;
    char keys[NET_INPUT_BATCH_MAX * 11][8];
    KrunkerValue change_kv[NET_INPUT_BATCH_MAX * 11 * 2];
    int changes = 0;
    double first_isn = 0;
    for (int j = 0; j < count && j < NET_INPUT_BATCH_MAX; j++) {
        double v[15];
        input_fields(&frames[j], v);
        if (j == 0) first_isn = v[0];
        dts[j] = v[1];
        for (int k = 2; k < 4; k++) {
            if (angle_count < 2) angles[angle_count++] = v[k];
            else angles[angle_count++] = v[k] - (l->last_set[k] ? l->last_vals[k] : 0);
            l->last_vals[k] = v[k];
            l->last_set[k] = 1;
        }
        for (int k = 4; k < 15; k++) {
            if (l->last_set[k] && l->last_vals[k] == v[k]) continue;
            l->last_vals[k] = v[k];
            l->last_set[k] = 1;
            snprintf(keys[changes], sizeof(keys[changes]), "%d-%d", j, k);
            change_kv[2 * changes] = KRUNKER_V_STR(keys[changes]);
            change_kv[2 * changes + 1] = KRUNKER_V_NUM(v[k]);
            changes++;
        }
    }
    const int n = count < NET_INPUT_BATCH_MAX ? count : NET_INPUT_BATCH_MAX;
    // dts: one string when every dt prints with the same width
    char dt_str[NET_INPUT_BATCH_MAX * 24] = "";
    size_t dt_len = 0, used = 0;
    int same_width = 1;
    for (int j = 0; j < n; j++) {
        char s[24];
        const int w = snprintf(s, sizeof(s), "%.0f", dts[j]);
        if (j == 0) dt_len = (size_t) w;
        else if ((size_t) w != dt_len) {
            same_width = 0;
            break;
        }
        memcpy(dt_str + used, s, (size_t) w);
        used += (size_t) w;
        dt_str[used] = 0;
    }
    KrunkerValue dt_items[NET_INPUT_BATCH_MAX];
    for (int j = 0; j < n; j++) dt_items[j] = KRUNKER_V_NUM(dts[j]);
    // angles: only the first pair when every later delta is 0
    if (angle_count > 2) {
        int all_zero = 1;
        for (int i = 2; i < angle_count; i++) {
            if (angles[i] != 0) {
                all_zero = 0;
                break;
            }
        }
        if (all_zero) angle_count = 2;
    }
    int same_angles = 1; // JS compares a[i] != prev[i] over a's length (a missing prev[i] differs)
    for (int i = 0; i < angle_count; i++) {
        if (i >= l->prev_angle_count || angles[i] != l->prev_angles[i]) {
            same_angles = 0;
            break;
        }
    }
    KrunkerValue angle_items[2 + 2 * NET_INPUT_BATCH_MAX];
    for (int i = 0; i < angle_count; i++) angle_items[i] = KRUNKER_V_NUM(angles[i]);

    KrunkerValue args[7];
    size_t argc = 0;
    args[argc++] = KRUNKER_V_NUM(l->resync ? 1 : 0);
    args[argc++] = KRUNKER_V_NUM(first_isn);
    args[argc++] = same_width ? KRUNKER_V_STRN(dt_str, used) : KRUNKER_V_ARRAY(dt_items, n);
    args[argc++] = KRUNKER_V_NUM(same_width ? (double) dt_len : 0);
    if (changes || (angle_count && !same_angles)) args[argc++] = same_angles ? KRUNKER_V_NUM(0) : KRUNKER_V_ARRAY(angle_items, angle_count);
    if (changes) args[argc++] = KRUNKER_V_MAP(change_kv, changes);
    const int sent = send_c2s(l, KRUNKER_C2S_INPUT, args, argc);
    memcpy(l->prev_angles, angles, sizeof(double) * (size_t) angle_count);
    l->prev_angle_count = angle_count;
    l->resync = 0;
    return sent;
}

int krunker_link_send_ours(KrunkerLink *l, const NetPacketType type, const void *payload, const uint16_t length) {
    switch ((int) type) {
        case NET_PACKET_ENT:
            if (length < NET_ENT_PACKET_MIN_SIZE) return 0;
            return send_enter(l, payload, length);
        case NET_PACKET_INPUT_BATCH: {
            if (length < 1) return 0;
            const uint8_t *p = payload;
            int count = p[0];
            if (count > NET_INPUT_BATCH_MAX || (size_t) length < 1 + (size_t) count * sizeof(NetInputPacket)) return 0;
            NetInputPacket frames[NET_INPUT_BATCH_MAX];
            memcpy(frames, p + 1, (size_t) count * sizeof(NetInputPacket));
            return send_input_window(l, frames, count);
        }
        case NET_PACKET_INPUT: {
            NetInputPacket in;
            memset(&in, 0, sizeof(in));
            memcpy(&in, payload, length < sizeof(in) ? length : sizeof(in));
            return send_input_window(l, &in, 1);
        }
        case NET_PACKET_CHAT_SEND: { // ["ct", channel, text] (iííïiïì, game.deob.js:219709)
            NetChatSendPacket c;
            memset(&c, 0, sizeof(c));
            memcpy(&c, payload, length < sizeof(c) ? length : sizeof(c));
            c.text[sizeof(c.text) - 1] = 0;
            const KrunkerValue args[] = {KRUNKER_V_NUM(0), KRUNKER_V_STR(c.text)};
            return send_c2s(l, KRUNKER_C2S_CHAT_MESSAGE, args, 2);
        }
        case NET_PACKET_CHAT_V7: {
            NetChatV7SendPacket c;
            memset(&c, 0, sizeof(c));
            memcpy(&c, payload, length < sizeof(c) ? length : sizeof(c));
            c.text[sizeof(c.text) - 1] = 0;
            const KrunkerValue args[] = {KRUNKER_V_NUM(c.channel == 1 ? 1 : 0), KRUNKER_V_STR(c.text)};
            return send_c2s(l, KRUNKER_C2S_CHAT_MESSAGE, args, 2);
        }
        default:
            return 0; // HELLO, JOIN, PONG (we answer "pi" ourselves), the KrunkNative-only packets
    }
}
