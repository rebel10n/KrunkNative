// Official Krunker session: packet translation (client/include/krunker_link.h). Pure: no sockets,
// no Client; the transport (krunker_net.c) owns the WebSocket and the client glue.
//
// Where Krunker's shapes and ours differ, this file is the translation layer:
//   - ids: Krunker's numeric player sid is our uid; "you" is the "0" entry whose id equals the
//     socket id io-init gave (players.add, game.deob.js:220268).
//   - "k" carries no health, our STATE does: the last "0" / "h" health per sid is kept here.
//   - "k" angles are degrees (iiïìííï: x * PI / 180), "0" yaw and "l" yaw radians.
//   - "l" slideTimer / canSlideT are JS milliseconds; our RECON keeps seconds (+ slide_ms).
//   - aimVal runs the other way: Krunker's 1 is hip fire and 0 fully aimed (procInputs: ADS
//     lowers it, weapon.noAim sets 1), ours 0 hip and 1 aimed. "l" [15] and "k" [10] are 1 - v.
//   - movement keys: Krunker's input[4] indexes movDirs[i] = -PI + (i + 1) * PI / 4 (:16415);
//     our Input.move_dir m is the angle -PI / 2 + m * PI / 4 (forward 0), so input[4] = (m + 1) % 8.
//   - init carries the lobby's config and network rates (arg 6, 7, 12): they become the same
//     GAME_CONFIG / NETWORK_DATA our own server sends, so prediction runs the server's physics.
//   - our client sends one NetInputPacket per input frame, batched per clientSendRate window
//     (recon.c, NET_PACKET_INPUT_BATCH): that window becomes one delta-compressed "q".
//   - cosmetics / skins / account fields are not mapped (a guest has none): SPAWNs carry none.
#include <krunker_link.h>
#include "krunker_link_internal.h" // krunker_link_world.c: mode objectives / world objects
#include <krunker_cheat.h>
#include <krunker_build_live.h> // the current build's strides, offsets and argument positions
#include <krunker_altcha.h>
#include <cJSON.h>
#include <game_config.h>
#include <map_catalog.h>
#include <net_ext.h>     // NET_EXT_KN_OFFICIAL_* (clusters C / D)
#include <ruleset.h>
#include <krunker_maps.h>
#include <krunker_sounds.h> // "s" / "so" / "sso": the official sound-name table
#include <ai.h>             // "ana": NetAIAnimPacket
#include <progression.h> // overlay.medalsList: the medals "6" / "am" give by index
#include <social_v7.h>
#include <voice.h> // the v7 voice framing ("vc" <-> NET_PACKET_VOICE_V7)
#include <zombies.h> // NET_PACKET_SHARING ("sic")
#include <net_ext.h> // the player list's vote kick / K.P.D (NET_PACKET_EXT subtypes)
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// init's arguments read here (iíìííiî :219300, called with init's array): [6] the lobby config
// (applyConfig), [7] {weps, cls, prks} (config.weps / cls / prks), [12] the networking rates
// (applyNetworking :187250: snf, tf, sf, aist, los, adst)
#define KRUNKER_INIT_ARG_CONFIG 6
#define KRUNKER_INIT_ARG_CUSTOM 7
#define KRUNKER_INIT_ARG_NETWORKING 12

// ---------------------------------------------------------------- helpers

static const KrunkerValue *arg(const KrunkerFrame *f, const uint32_t i) { return krunker_frame_arg(f, i); }
// the argument of a role in the current build (KrunkerBuild kill / damage / health: -1 absent)
static const KrunkerValue *role(const KrunkerFrame *f, const int16_t *positions, const int r) {
    return positions[r] >= 0 ? krunker_frame_arg(f, (uint32_t) positions[r]) : NULL;
}
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
void krunker_link_emit(KrunkerLink *l, const uint8_t type, const void *payload, const uint16_t length) { emit(l, type, payload, length); }

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
    social_assembly_free(&l->voice_out); // a clip half sent on the old socket
    memset(l, 0, sizeof(*l));
    l->io = io;
    krunker_session_init(&l->codec, ahkey);
    l->my_sid = -1;
    l->unsupported_map = -1;
    l->game_state = -1;
    l->send_hz = 10; // applyNetworking() defaults until init's networking
    l->tick_hz = 30;
    l->sync_hz = 1;
    l->last_isn_sent = l->last_ack = -1;
    l->pc_stat = -1;
    l->team_pick = 1; // toggleTeam's default (iïíiîiì = 1, :210435)
}

int krunker_link_inputs_paused(const KrunkerLink *l) { return l->locked || l->you_gone; }

// ---------------------------------------------------------------- inbound

// A Krunker value as cJSON (NULL: nil / not representable).
static cJSON *value_json(const KrunkerValue *v) {
    if (is_nil(v)) return NULL;
    KrunkerBuf buf = {0};
    cJSON *out = krunker_value_to_json(v, &buf) == KRUNKER_PROTO_OK && buf.data ? cJSON_ParseWithLength((const char *) buf.data, buf.len) : NULL;
    krunker_buf_free(&buf);
    return out;
}

// init's lobby settings (iíìííiî :219300): applyNetworking(arg 12) and applyConfig(arg 6, with
// arg 7's weps / cls / prks replacing the config's own, :219365-219368), sent to the client the way
// our own server does (game_config_send: GAME_CONFIG chunks, then NETWORK_DATA) ahead of the INIT,
// whose game_config_on_init would otherwise fall back to the defaults. Physics reads them:
// gravMlt, jumpMlt, strafeSpd, airStrf, fixMov, slidSpd / slidTime / canSlide, wallJP, autoJump,
// headClipFix, ... and the per-class / per-weapon overrides (speed, spdMlt, ...). Host type 0
// (applyConfig's third argument is undefined: hostTypes[0]); the server clamped already.
static void emit_lobby_config(KrunkerLink *l, const KrunkerFrame *f) {
    cJSON *msg = cJSON_CreateObject();
    if (!msg) return;
    cJSON_AddNumberToObject(msg, "host", GAME_HOST_CUSTOM);
    cJSON *settings = value_json(arg(f, KRUNKER_INIT_ARG_CONFIG));
    if (!cJSON_IsObject(settings)) {
        cJSON_Delete(settings);
        settings = cJSON_CreateObject();
    }
    static const char *const tables[] = {"weps", "cls", "prks"};
    const KrunkerValue *custom = arg(f, KRUNKER_INIT_ARG_CUSTOM);
    for (size_t i = 0; settings && i < sizeof(tables) / sizeof(tables[0]); i++) {
        cJSON_DeleteItemFromObjectCaseSensitive(settings, tables[i]);
        cJSON *table = krunker_is(custom, KRUNKER_MAP) ? value_json(krunker_get(custom, tables[i])) : NULL;
        if (table) cJSON_AddItemToObject(msg, tables[i], table);
    }
    cJSON *classes = settings ? cJSON_DetachItemFromObjectCaseSensitive(settings, "classes") : NULL;
    if (classes) cJSON_AddItemToObject(msg, "classes", classes);
    l->config_keys = settings ? cJSON_GetArraySize(settings) : 0;
    if (settings) cJSON_AddItemToObject(msg, "settings", settings);
    char *text = cJSON_PrintUnformatted(msg);
    cJSON_Delete(msg);
    const size_t total = text ? strlen(text) : 0;
    if (text && total <= NET_GAME_CONFIG_MAX) {
        NetGameConfigChunk chunk;
        chunk.total = (uint32_t) total;
        for (size_t offset = 0; offset < total; offset += NET_GAME_CONFIG_CHUNK_SIZE) {
            const size_t n = total - offset < NET_GAME_CONFIG_CHUNK_SIZE ? total - offset : NET_GAME_CONFIG_CHUNK_SIZE;
            chunk.offset = (uint32_t) offset;
            memcpy(chunk.data, text + offset, n);
            emit(l, NET_PACKET_GAME_CONFIG, &chunk, (uint16_t) (NET_GAME_CONFIG_HEADER_SIZE + n));
        }
    }
    cJSON_free(text);

    // applyNetworking: snf || 10, tf || 30, sf || 1 (falsy -> the default, like JS ||)
    const KrunkerValue *net = arg(f, KRUNKER_INIT_ARG_NETWORKING);
    const double snf = num(krunker_is(net, KRUNKER_MAP) ? krunker_get(net, "snf") : NULL, 0);
    const double tf = num(krunker_is(net, KRUNKER_MAP) ? krunker_get(net, "tf") : NULL, 0);
    const double sf = num(krunker_is(net, KRUNKER_MAP) ? krunker_get(net, "sf") : NULL, 0);
    NetNetworkDataPacket rates;
    memset(&rates, 0, sizeof(rates));
    rates.send_hz = (uint16_t) (snf > 0 && snf < 1000 ? snf : 10);
    rates.tick_hz = (uint16_t) (tf > 0 && tf < 1000 ? tf : 30);
    rates.sync_hz = (uint16_t) (sf > 0 && sf < 1000 ? sf : 1);
    l->send_hz = rates.send_hz;
    l->tick_hz = rates.tick_hz;
    l->sync_hz = rates.sync_hz;
    emit(l, NET_PACKET_NETWORK_DATA, &rates, sizeof(rates));
}
static void on_account(KrunkerLink *l, const KrunkerFrame *f);
static void on_update_account(KrunkerLink *l, const KrunkerFrame *f);
static void on_balances(KrunkerLink *l, const KrunkerFrame *f);
static void on_challenge_rewards(KrunkerLink *l, const KrunkerFrame *f);

// init arg 9 (customsData, :219300 -> prepareGame :188875): {data: "<map JSON>", id, creator, ...} of
// a custom game. data is a JSON string (prepareGame JSON.parses it); an object is taken as well.
static cJSON *custom_map(const KrunkerValue *custom, char *id, const size_t id_size) {
    id[0] = 0;
    if (!krunker_is(custom, KRUNKER_MAP)) return NULL;
    id_string(krunker_get(custom, "id"), id, id_size);
    const KrunkerValue *data = krunker_get(custom, "data");
    if (krunker_is(data, KRUNKER_STR)) return cJSON_ParseWithLength(krunker_str(data), data->len);
    if (!krunker_is(data, KRUNKER_MAP)) return NULL;
    KrunkerBuf buf = {0};
    cJSON *json = krunker_value_to_json(data, &buf) == KRUNKER_PROTO_OK ? cJSON_ParseWithLength((const char *) buf.data, buf.len) : NULL;
    krunker_buf_free(&buf);
    return json;
}

// init [mapIndex, modeIndex, eventIndex, ..., customsData] (INIT_GAME, game.deob.js:219289 -> 219300):
// mapIndex is a position in Krunker's official list (the live client GETs <gapi>/maps/<mapIndex> when it
// does not have it, :219292; this client ships the list, krunker_maps.h); a custom game sends its map
// in arg 9. io.map turns either into one of the client's maps (krunker_net.c, krunker_map_source.h).
static void on_init(KrunkerLink *l, const KrunkerFrame *f) {
    const int map = krunker_is(arg(f, 0), KRUNKER_NUM) ? (int) num(arg(f, 0), -1) : -1, mode = (int) num(arg(f, 1), 0);
    KrunkerLinkInit info;
    memset(&info, 0, sizeof(info));
    info.map_index = map;
    info.mode_index = mode;
    info.custom = custom_map(arg(f, KRUNKER_MAPS_INIT_CUSTOM_ARG), info.custom_id, sizeof(info.custom_id));
    // initGame drops our player (you = null) until the new round's "0": no input frame meanwhile, so the
    // last round's player (still alive on our side until INIT reaches the game) sends nothing into this one
    if (l->my_sid >= 0) l->you_gone = 1;
    id_string(arg(f, 5), l->host_id, sizeof(l->host_id)); // game.host (:219329): a custom lobby's host
    if (map < 0 && !info.custom && !info.custom_id[0]) {
        l->unsupported_map = map;
        note(l, KRUNKER_LINK_EV_UNSUPPORTED, "the server's init names no map");
        return;
    }
    l->unsupported_map = -1;
    l->is_comp = truthy(arg(f, 15));   // initGame iîIiiïï (:219366)
    l->is_ranked = truthy(arg(f, 16)); // iiÎîîíî
    l->is_official = truthy(arg(f, 14)); // iìÎíììì
    l->vote_count = 0;
    emit_lobby_config(l, f);
    NetInitPacket init;
    memset(&init, 0, sizeof(init));
    init.major = NET_PROTOCOL_MAJOR;
    init.minor = NET_PROTOCOL_MINOR;
    init.patch = NET_PROTOCOL_PATCH;
    init.map_index = map;
    init.mode_index = mode; // INIT carries the Krunker mode id (main.c client_apply_init)
    init.tick_rate = l->tick_hz; // applyNetworking tf (tickFrequency, default 30, game.deob.js:186955)
    init.client_id = 0;
    init.ruleset = NET_INIT_RULESET;
    note(l, KRUNKER_LINK_EV_NOTE, "init: map %d%s%s%s, mode %d, %d config keys, rates snf %d tf %d sf %d", map,
         info.custom_id[0] || info.custom ? " (custom " : "", info.custom_id[0] ? info.custom_id : info.custom ? "inline" : "",
         info.custom_id[0] || info.custom ? ")" : "", mode, l->config_keys, l->send_hz, l->tick_hz, l->sync_hz);
    // after the INIT (the transport holds what follows io.map until it emits it): the official server
    // takes the v7 social shapes, so our chat goes out with its channel ("ct" [tab, msg]), the spray
    // as "spry", the prop taunt as "prpt" (hud_social.c social_server_v7; INIT resets it)
    static const uint8_t social = NET_SOCIAL_V7_VERSION;
    if (l->io.map) {
        info.init = init;
        l->io.map(l->io.user, &info); // takes info.custom
        emit(l, NET_PACKET_SOCIAL_V7, &social, 1);
        return;
    }
    cJSON_Delete(info.custom);
    emit(l, NET_PACKET_INIT, &init, sizeof(init));
    emit(l, NET_PACKET_SOCIAL_V7, &social, 1);
}

// A player's account fields as v10 keeps them from its "0" entry (players.add :220273-220283): the
// leaderboard / Tab board / end table / nametags read them through NET_PACKET_PLAYER_INFO. HOST is
// game.host (init arg 5, "uh") == the player's socket id.
static void emit_player_info(KrunkerLink *l, KrunkerLinkPlayer *p) {
    p->info.flags = (uint8_t) ((p->info.flags & ~NET_PLAYER_INFO_HOST) |
                               (l->host_id[0] && p->id[0] && !strcmp(l->host_id, p->id) ? NET_PLAYER_INFO_HOST : 0));
    emit(l, NET_PACKET_PLAYER_INFO, &p->info, sizeof(p->info));
}

// "0" ADD_PLAYERS: a flat list, 53 (0x35) values per player (game.deob.js:220266-220330); the
// players.add arguments (:171902, player.init :169089): [0] id (socket id) [1] sid [2] x [3] y
// [4] z [5] name [6] classIndex [7] health (null: maxHealth) [8] maxHealth [9] team [15] secIndex
// [18] xDire (yaw, radians) [22] attachIndex [28] alias. Stride and offsets: the current build's
// (krunker_build_live.h; the compiled ones are KRUNKER_ADD_* of krunker_build.h).
static void on_add_players(KrunkerLink *l, const KrunkerFrame *f) {
    const KrunkerBuild *b = krunker_build();
    const KrunkerValue *list = arg(f, 0);
    if (!krunker_is(list, KRUNKER_ARRAY)) return;
    for (uint32_t i = 0; i + 1 < list->len; i += (uint32_t) b->add_stride) {
        const KrunkerValue *e = list->u.items + i;
        const uint32_t left = list->len - i;
#define E(slot) (b->add[(slot)] >= 0 && (uint32_t) b->add[(slot)] < left ? &e[b->add[(slot)]] : NULL)
        if (!krunker_is(E(KRB_ADD_SID), KRUNKER_NUM)) continue;
        NetSpawnPacket s;
        memset(&s, 0, sizeof(s));
        s.uid = (int32_t) E(KRB_ADD_SID)->u.num;
        char id[64];
        id_string(E(KRB_ADD_ID), id, sizeof(id));
        s.is_you = l->socket_id[0] && !strcmp(id, l->socket_id);
        s.active = 1;
        s.class_index = (int32_t) num(E(KRB_ADD_CLASS_INDEX), 0);
        s.team = (int32_t) num(E(KRB_ADD_TEAM), 0);
        s.max_health = (int32_t) num(E(KRB_ADD_MAX_HEALTH), 100);
        s.health = (float) (is_nil(E(KRB_ADD_HEALTH)) ? s.max_health : num(E(KRB_ADD_HEALTH), s.max_health));
        s.position[0] = (float) num(E(KRB_ADD_X), 0);
        s.position[1] = (float) num(E(KRB_ADD_Y), 0);
        s.position[2] = (float) num(E(KRB_ADD_Z), 0);
        s.direction[0] = 0.0f;                     // yDire is not in "0"
        s.direction[1] = (float) num(E(KRB_ADD_X_DIRE), 0);    // xDire
        // the name everything shows is the alias: players.add `alias = [28] || name` (:171939)
        const char *name = krunker_str(E(KRB_ADD_ALIAS));
        if (!name || !name[0]) name = krunker_str(E(KRB_ADD_NAME));
        if (name && name[0]) snprintf(s.name, sizeof(s.name), "%s", name);
        else snprintf(s.name, sizeof(s.name), "Guest_%d", s.uid); // players.add's default
        s.secondary_index = (int32_t) num(E(KRB_ADD_SEC_INDEX), -1);
        s.cosmetics = PLAYER_COSMETICS_NONE;
        s.attach_index = (int32_t) num(E(KRB_ADD_ATTACH_INDEX), 0); // was 23 (pcStatIndex)
        s.head_mlt = 1.0f;
        s.cosmetics_v7 = PLAYER_COSMETICS_V7_NONE;
        KrunkerLinkPlayer *p = player(l, s.uid, 1);
        if (p) {
            p->health = s.health;
            p->max_health = s.max_health;
            p->team = s.team;
            snprintf(p->id, sizeof(p->id), "%s", id); // "chgp" findById
            p->class_index = s.class_index;
            const char *real = krunker_str(E(KRB_ADD_NAME));
            snprintf(p->name, sizeof(p->name), "%s", real && real[0] ? real : s.name);
            NetPlayerInfoPacket *in = &p->info;
            memset(in, 0, sizeof(*in));
            in->uid = s.uid;
            in->account_id = (int32_t) num(E(KRB_ADD_ACCID), 0);
            in->level = (int32_t) num(E(KRB_ADD_LEVEL), 0);
            const char *clan = krunker_str(E(KRB_ADD_CLAN));
            if (clan) snprintf(in->clan, sizeof(in->clan), "%s", clan);
            in->flags = (uint8_t) ((num(E(KRB_ADD_PREMIUM_T), 0) > 0 ? NET_PLAYER_INFO_PREMIUM : 0) |
                                   (truthy(E(KRB_ADD_FEATURED)) ? NET_PLAYER_INFO_VERIFIED : 0) |
                                   (truthy(E(KRB_ADD_IS_HACKER)) ? NET_PLAYER_INFO_HACKER : 0) |
                                   (truthy(E(KRB_ADD_IS_BOT)) ? NET_PLAYER_INFO_BOT : 0) |
                                   (truthy(E(KRB_ADD_CREATOR_BADGE)) ? NET_PLAYER_INFO_CREATOR : 0));
        }
#undef E
        if (s.is_you) {
            const int first = l->my_sid != s.uid;
            l->my_sid = s.uid;
            l->you_gone = 0;
            if (first) note(l, KRUNKER_LINK_EV_JOINED, "joined as sid %d (%s)", s.uid, s.name);
        }
        emit(l, NET_PACKET_SPAWN, &s, sizeof(s));
        if (p) emit_player_info(l, p); // after SPAWN, as our own server sends it
        // Prop Hunt: players.add's updateProp(player, propID || 1) for a Prop-team player (krunker_link_prophunt.c)
        // players.add's propID ("0" [21], :171902) has no position in the layout yet (tools/krunker/extract_build.py):
        // a Prop-team spawn gets the default prop (1) as updateProp(player, propID || 1) does without one
        krunker_link_prophunt_spawned(l, s.uid, s.team, NULL);
    }
}

static void fx_grapple(KrunkerLink *l, int sid, const KrunkerValue *g); // combat visuals (fx, below)

// "k" UPDATE_PLAYERS [list, sendRateA, servTim] (game.deob.js:220529): 13 values per player,
// [0] sid [1..3] x y z [4] xDire (deg) [5] yDire (deg) [6] stepVal [7] onGround [8] crouchVal
// [9] weapon (loadout index) [10] aimVal [11] grapple [12] ping. Our own sid is skipped (as in JS).
// Stride and offsets: the current build's (krunker_build_live.h; compiled: KRUNKER_K_*).
static double link_now_ms(const KrunkerLink *l) { return l->io.now_ms ? l->io.now_ms(l->io.user) : 0.0; }

static void on_update_players(KrunkerLink *l, const KrunkerFrame *f) {
    const KrunkerBuild *b = krunker_build();
    // io_updatePlayers (index.js:17111): Game.servTim = servTim; lastSync = newSync || 0; newSync = now.
    // Before the list: an empty "k" counts too, the "q" syncTimes read these.
    l->serv_tim = num(arg(f, 2), 0);
    l->sync_known = 1;
    l->last_sync = l->new_sync;
    l->new_sync = link_now_ms(l);
    const KrunkerValue *list = arg(f, 0);
    if (!krunker_is(list, KRUNKER_ARRAY)) return;
    const uint32_t stride = (uint32_t) b->k_stride;
    for (uint32_t i = 0; i + stride <= list->len; i += stride) {
        const KrunkerValue *e = list->u.items + i;
#define K(slot) (b->k[(slot)] >= 0 && (uint32_t) b->k[(slot)] < stride ? &e[b->k[(slot)]] : NULL)
        if (!krunker_is(K(KRB_K_SID), KRUNKER_NUM)) continue;
        const int sid = (int) K(KRB_K_SID)->u.num;
        if (sid == l->my_sid) continue;
        const KrunkerLinkPlayer *p = player(l, sid, 0);
        NetStatePacket s;
        memset(&s, 0, sizeof(s));
        s.uid = sid;
        s.ack_seq = 0;
        s.active = 1;
        s.loadout_index = (int32_t) num(K(KRB_K_WEAPON), 0);
        s.position[0] = (float) num(K(KRB_K_X), 0);
        s.position[1] = (float) num(K(KRB_K_Y), 0);
        s.position[2] = (float) num(K(KRB_K_Z), 0);
        s.direction[0] = (float) (num(K(KRB_K_Y_DIRE), 0) * (M_PI / 180.0)); // pitch
        s.direction[1] = (float) (num(K(KRB_K_X_DIRE), 0) * (M_PI / 180.0)); // yaw
        s.health = p ? p->health : 100.0f;
        s.aim_val = 1.0f - (float) num(K(KRB_K_AIM_VAL), 1); // Krunker 1 = hip, ours 0
        s.crouch_val = (float) num(K(KRB_K_CROUCH_VAL), 0);
        emit(l, NET_PACKET_STATE, &s, sizeof(s));
        fx_grapple(l, sid, K(KRB_K_GRAPPLE));
#undef K
    }
}

// "l" SYNC_PLAYER [state] (game.deob.js:220335): [0] inputSN [1] ping [2..4] x y z [5] velY
// [6] velX [7] velZ [8] xDire [9] onGround [10] didJump [11] didJumpW [12] wallJumpC [13] onWall
// [14] onLadder [15] aimVal [16] crouchVal [17] weapon index [18] slideTimer (ms) [19] canSlide
// [20] onTerrain [21] xVelC [22] zVelC [23] grapple || null [24] canSlideT (ms). Always answered
// with PONG "v" (first statement of the handler). Offsets: the current build's (krunker_build_live.h;
// compiled: KRUNKER_SYNC_*).
static void on_sync_player(KrunkerLink *l, const KrunkerFrame *f) {
    const KrunkerBuild *b = krunker_build();
    send_c2s(l, KRUNKER_C2S_PONG, NULL, 0);
    const KrunkerValue *a = arg(f, 0);
    if (!krunker_is(a, KRUNKER_ARRAY)) return;
#define A(slot) (b->sync[(slot)] >= 0 ? krunker_at(a, (uint32_t) b->sync[(slot)]) : NULL)
    NetReconPacket r;
    memset(&r, 0, sizeof(r));
    r.ack_seq = (int32_t) num(A(KRB_SYNC_INPUT_SN), NET_RECON_SEQ_ALL);
    r.position[0] = (float) num(A(KRB_SYNC_X), 0);
    r.position[1] = (float) num(A(KRB_SYNC_Y), 0);
    r.position[2] = (float) num(A(KRB_SYNC_Z), 0);
    r.velocity[0] = (float) num(A(KRB_SYNC_VEL_X), 0);
    r.velocity[1] = (float) num(A(KRB_SYNC_VEL_Y), 0);
    r.velocity[2] = (float) num(A(KRB_SYNC_VEL_Z), 0);
    r.yaw = (float) num(A(KRB_SYNC_X_DIRE), 0);
    r.wall_jump_count = (uint16_t) num(A(KRB_SYNC_WALL_JUMP_C), 0);
    const double on_wall = num(A(KRB_SYNC_ON_WALL), 0); // the wall's side 1..4 (procInputs), 0 none
    r.on_wall = (uint8_t) (on_wall >= 1 && on_wall <= 4 ? on_wall : 0);
    r.aim_val = 1.0f - (float) num(A(KRB_SYNC_AIM_VAL), 1); // Krunker 1 = hip, ours 0
    r.crouch_val = (float) num(A(KRB_SYNC_CROUCH_VAL), 0);
    r.loadout_index = (int32_t) num(A(KRB_SYNC_WEAPON), 0);
    r.slide_ms = num(A(KRB_SYNC_SLIDE_TIMER), 0);
    r.slide_timer = (float) (r.slide_ms / 1000.0);
    r.x_vel_cylinder = (float) num(A(KRB_SYNC_X_VEL_C), 0);
    r.z_vel_cylinder = (float) num(A(KRB_SYNC_Z_VEL_C), 0);
    r.can_slide_timer = (float) (num(A(KRB_SYNC_CAN_SLIDE_T), 0) / 1000.0);
    r.flags = (truthy(A(KRB_SYNC_ON_GROUND)) ? NET_RECON_ON_GROUND : 0) |
              (truthy(A(KRB_SYNC_DID_JUMP)) ? NET_RECON_DID_JUMP : 0) |
              (truthy(A(KRB_SYNC_DID_JUMP_W)) ? NET_RECON_DID_JUMP_W : 0) |
              (truthy(A(KRB_SYNC_ON_LADDER)) ? NET_RECON_ON_LADDER : 0) |
              (truthy(A(KRB_SYNC_CAN_SLIDE)) ? NET_RECON_CAN_SLIDE : 0) |
              (truthy(A(KRB_SYNC_ON_TERRAIN)) ? NET_RECON_ON_TERRAIN : 0) |
              (l->noclip ? NET_RECON_NOCLIP : 0); // "l" has no noclip: the live client keeps "noc"'s through reconcilePlayer
    const KrunkerValue *g = A(KRB_SYNC_GRAPPLE);
    if (krunker_is(g, KRUNKER_ARRAY) && g->len >= 3) {
        r.grapple_active = 1;
        for (uint32_t k = 0; k < 4 && k < g->len; k++) r.grapple[k] = (float) num(&g->u.items[k], 0);
    }
    const KrunkerValue *ping = A(KRB_SYNC_PING);
#undef A
    emit(l, NET_PACKET_RECON, &r, sizeof(r));
    l->syncs++;
    l->last_ack = r.ack_seq;
    memcpy(l->sync_pos, r.position, sizeof(l->sync_pos));
    if (krunker_is(ping, KRUNKER_NUM)) {
        const NetPingResultPacket pr = {.ping_ms = (int32_t) ping->u.num};
        emit(l, NET_PACKET_PING_RESULT, &pr, sizeof(pr));
    }
}

static KrunkerLinkStats *stats(KrunkerLink *l, int sid); // the HUD stats of clusters C / D (below)

static void js_string(const KrunkerValue *v, char *out, size_t n); // below

// "3" deathStats to the victim (:220591-220616, iîïïiìî): [weaponId, killerClassStat, [pcStatIndex,
// value] | null, [timeAliveMs, shots, accuracy, kills, score]] -> NET_PACKET_DEATH_INFO ahead of the
// KILL (the death card's stats panel, the killer's class level and player-card stat), as our server
// sends it (combat: player.c)
static void emit_death_info(KrunkerLink *l, const NetKillPacket *k, const KrunkerValue *stats, const KrunkerValue *info) {
    if (!krunker_is(stats, KRUNKER_ARRAY)) return;
    NetDeathInfoPacket d;
    memset(&d, 0, sizeof(d));
    d.victim_uid = k->victim_uid;
    d.killer_uid = k->killer_uid;
    d.weapon_index = (int32_t) num(krunker_at(stats, 0), -1);
    d.class_stat = (int32_t) num(krunker_at(stats, 1), 0);
    d.pc_stat_index = -1;
    const KrunkerValue *pc = krunker_at(stats, 2);
    if (krunker_is(pc, KRUNKER_ARRAY) && krunker_is(krunker_at(pc, 0), KRUNKER_NUM)) {
        d.pc_stat_index = (int8_t) num(krunker_at(pc, 0), -1);
        char value[sizeof(d.pc_stat_value) + 1];
        js_string(krunker_at(pc, 1), value, sizeof(value));
        memcpy(d.pc_stat_value, value, sizeof(d.pc_stat_value));
    }
    const KrunkerValue *life = krunker_at(stats, 3);
    if (krunker_is(life, KRUNKER_ARRAY)) {
        d.time_alive_ms = (int32_t) num(krunker_at(life, 0), 0);
        d.shots = (int32_t) num(krunker_at(life, 1), 0);
        char acc[sizeof(d.accuracy) + 1];
        js_string(krunker_at(life, 2), acc, sizeof(acc));
        memcpy(d.accuracy, acc, sizeof(d.accuracy));
        d.kills = (int32_t) num(krunker_at(life, 3), 0);
        d.score = (int32_t) num(krunker_at(life, 4), 0);
    }
    d.thrown = krunker_is(info, KRUNKER_MAP) && truthy(krunker_get(info, "pj"));
    d.fists = (k->fists & NET_KILL_FISTS) ? 1 : 0;
    emit(l, NET_PACKET_DEATH_INFO, &d, sizeof(d));
}

// "3" KILL_PLAYER [victimSid, victimDeaths, killerSid, deathStats, killInfo, hideFeed]
// (game.deob.js:220579; the feed row :221222-221246 reads killInfo {wId (null: the killer's held
// weapon), stk (+ pkg), fs, pj, wb, hs}; hideFeed: no row, killedBy still counts, :220629)
static void on_kill(KrunkerLink *l, const KrunkerFrame *f) {
    const KrunkerBuild *b = krunker_build();
    if (!krunker_is(role(f, b->kill, KRB_KILL_VICTIM), KRUNKER_NUM)) return;
    NetKillPacket k;
    memset(&k, 0, sizeof(k));
    k.victim_uid = (int32_t) role(f, b->kill, KRB_KILL_VICTIM)->u.num;
    k.victim_deaths = (int32_t) num(role(f, b->kill, KRB_KILL_DEATHS), 0);
    k.killer_uid = (int32_t) num(role(f, b->kill, KRB_KILL_KILLER), -1);
    k.weapon_index = -1;
    const KrunkerValue *info = role(f, b->kill, KRB_KILL_INFO);
    if (krunker_is(info, KRUNKER_MAP)) {
        k.weapon_index = (int32_t) num(krunker_get(info, "wId"), -1);
        const KrunkerValue *stk = krunker_get(info, "stk");
        const int streak = krunker_is(stk, KRUNKER_NUM) && stk->u.num >= 0 && stk->u.num < 15 ? (int) stk->u.num : -1;
        k.fists = (uint8_t) ((truthy(krunker_get(info, "fs")) ? NET_KILL_FISTS : 0) | (streak == 0 ? NET_KILL_NUKE : 0) |
                             (truthy(krunker_get(info, "pj")) ? NET_KILL_THROWN : 0) | (streak >= 0 ? (streak + 1) << NET_KILL_STREAK_SHIFT : 0));
        k.headshot = truthy(krunker_get(info, "hs")) ? 1 : 0;
        k.wallbang = truthy(krunker_get(info, "wb")) ? 1 : 0;
    } else if (krunker_is(info, KRUNKER_NUM)) {
        k.weapon_index = (int32_t) info->u.num;
    }
    if (truthy(role(f, b->kill, KRB_KILL_HIDE_FEED))) k.fists |= NET_KILL_HIDE_FEED;
    const KrunkerLinkPlayer *killer = k.killer_uid >= 0 ? player(l, k.killer_uid, 0) : NULL;
    k.killer_class = killer ? killer->class_index : -1; // the kill card's class icon (killer.classIndex, :220616)
    k.respawn_delay = GAME_DEATH_DELAY;
    // killer_kills (#killsVal, the board): the live server sends the killer its "6" (totalKills)
    // right before this "3" (players.kill :174098), so ours is already counted; others count here
    KrunkerLinkStats *ks = k.killer_uid != k.victim_uid && player(l, k.killer_uid, 0) ? stats(l, k.killer_uid) : NULL;
    if (ks && k.killer_uid != l->my_sid) ks->kills++;
    k.killer_kills = ks ? ks->kills : 0;
    KrunkerLinkStats *vs = stats(l, k.victim_uid);
    if (vs) vs->deaths = k.victim_deaths;
    if (k.victim_uid == l->my_sid) emit_death_info(l, &k, role(f, b->kill, KRB_KILL_STATS), info);
    emit(l, NET_PACKET_KILL, &k, sizeof(k));
}

static void emit_ext(KrunkerLink *l, uint16_t subtype, const void *body, uint16_t length);

// "4" DO_DAMAGE [targetSid, damage, crit, kill, isAI, silent] (game.deob.js:221254): the whole number is
// Math.ceil(dmg) (settings dmgType 0, :221259); a damage with a fraction goes first as
// NET_EXT_KN_HIT_DAMAGE, so dmgType 1 "Float" (the setting's val) draws Krunker's exact value. Not
// carried: silent (no hit sound) and the live build's crit_0 sound (10.0.3; v10.0.2 and our assets have
// hit_0 only).
static void on_damage(KrunkerLink *l, const KrunkerFrame *f) {
    const KrunkerBuild *b = krunker_build();
    NetHitPacket h;
    memset(&h, 0, sizeof(h));
    const double damage = num(role(f, b->damage, KRB_DAMAGE_DAMAGE), 0);
    h.target_uid = (int32_t) num(role(f, b->damage, KRB_DAMAGE_TARGET), -1);
    h.damage = (int32_t) ceil(damage);
    h.crit = truthy(role(f, b->damage, KRB_DAMAGE_CRIT)) ? 1 : 0;
    h.kill = truthy(role(f, b->damage, KRB_DAMAGE_KILL)) ? 1 : 0;
    h.ai = truthy(role(f, b->damage, KRB_DAMAGE_AI)) ? 1 : 0;
    const int32_t tenths = (int32_t) js_round(damage * 10.0);
    if (h.damage && tenths % 10) {
        const NetHitDamagePacket exact = {.target_uid = h.target_uid, .damage_tenths = tenths, .ai = h.ai};
        emit_ext(l, NET_EXT_KN_HIT_DAMAGE, &exact, sizeof(exact));
    }
    emit(l, NET_PACKET_HIT, &h, sizeof(h));
}

// "h" CHANGE_HEALTH [health, sid (null: you), srcX, srcZ, ..., resetMax] (game.deob.js:221040):
// (health > maxHealth || resetMax) && (maxHealth = health): a resetMax may lower it (Juggernaut ends)
static void on_health(KrunkerLink *l, const KrunkerFrame *f) {
    const KrunkerBuild *b = krunker_build();
    const int sid = is_nil(role(f, b->health, KRB_HEALTH_SID)) ? l->my_sid : (int) num(role(f, b->health, KRB_HEALTH_SID), -1);
    if (sid < 0) return;
    NetHealthPacket h;
    memset(&h, 0, sizeof(h));
    h.uid = sid;
    h.health = (float) num(role(f, b->health, KRB_HEALTH_HEALTH), 0);
    // the hit direction only without noHitMarker (:221062-221075: falls, zones, regen send none)
    if (krunker_is(role(f, b->health, KRB_HEALTH_SRC_X), KRUNKER_NUM) && krunker_is(role(f, b->health, KRB_HEALTH_SRC_Z), KRUNKER_NUM) &&
        !truthy(role(f, b->health, KRB_HEALTH_NO_HIT_MARKER))) {
        h.has_source = 1;
        h.source_x = (float) role(f, b->health, KRB_HEALTH_SRC_X)->u.num;
        h.source_z = (float) role(f, b->health, KRB_HEALTH_SRC_Z)->u.num;
    }
    KrunkerLinkPlayer *p = player(l, sid, 1);
    const int reset_max = truthy(role(f, b->health, KRB_HEALTH_RESET_MAX));
    int max_changed = 0;
    if (p) {
        p->health = h.health;
        if (h.health > (float) p->max_health || reset_max) {
            max_changed = p->max_health != (int) ceilf(h.health);
            p->max_health = (int) ceilf(h.health);
        }
    }
    emit(l, NET_PACKET_HEALTH, &h, sizeof(h));
    if (max_changed) { // HEALTH carries no maximum: the TEAM of the same team does (as our server's class change)
        const NetTeamPacket t = {.uid = sid, .team = p->team, .health = h.health, .max_health = p->max_health};
        emit(l, NET_PACKET_TEAM, &t, sizeof(t));
    }
}

// "ch" ADD_CHAT [team, name, text, kind, color, nameColor, senderAccountId, extra] (game.deob.js:221172):
// kind 0 player line, 1 / 4 game line, 2 server line (color), 3 trade request (text = the sender's sid,
// not shown). Every line goes out as NET_EXT_KN_OFFICIAL_CHAT: its tab (arg 0), a server line's colour,
// a player name's nameColor and the sender's account (the mute list). Not carried: extra.actions.
static void emit_hud_ext(KrunkerLink *l, uint16_t subtype, const void *body, uint16_t length); // below

// addChat's tab (iíïíííï: `team || 0`, -1 outside team modes, which the HUD applies): 1 team, -1 every tab, else 0
static int8_t chat_channel(const KrunkerValue *team) {
    const double t = num(team, 0);
    return (int8_t) (t == CHAT_CHANNEL_TEAM ? CHAT_CHANNEL_TEAM : t < 0 ? CHAT_CHANNEL_ALL : CHAT_CHANNEL_GLOBAL);
}

static void on_chat(KrunkerLink *l, const KrunkerFrame *f) {
    const char *text = krunker_str(arg(f, 2));
    const int kind = (int) num(arg(f, 3), 0);
    if (!text || kind == 3) return;
    NetOfficialChatHeader h;
    memset(&h, 0, sizeof(h));
    h.channel = chat_channel(arg(f, 0));
    h.kind = (uint8_t) (kind == 0 ? NET_CHAT_PLAYER : kind == 2 ? NET_CHAT_SERVER : NET_CHAT_GAME);
    h.account_id = (int32_t) num(arg(f, 6), 0);
    const char *color = krunker_str(arg(f, 4)), *name_color = krunker_str(arg(f, 5));
    if (color) snprintf(h.color, sizeof(h.color), "%s", color);
    if (name_color) snprintf(h.name_color, sizeof(h.name_color), "%s", name_color);
    const char *name = krunker_str(arg(f, 1));
    if (!name) name = "";
    uint8_t body[NET_EXT_BODY_MAX];
    const size_t nl = strlen(name) + 1;
    size_t tl = strlen(text) + 1;
    if (sizeof(h) + nl + 1 > sizeof(body)) return;
    if (sizeof(h) + nl + tl > sizeof(body)) tl = sizeof(body) - sizeof(h) - nl; // cut the text, keep it terminated
    memcpy(body, &h, sizeof(h));
    memcpy(body + sizeof(h), name, nl);
    memcpy(body + sizeof(h) + nl, text, tl);
    body[sizeof(h) + nl + tl - 1] = 0;
    emit_hud_ext(l, NET_EXT_KN_OFFICIAL_CHAT, body, (uint16_t) (sizeof(h) + nl + tl));
}

static void emit_ext(KrunkerLink *l, const uint16_t subtype, const void *body, const uint16_t length); // below

// "vk" VOTE_KICK [alias, votes, needed, isNew, voter] (iïiiìíí, game.deob.js:219974): a running
// vote (isNew: opened by `voter`), or a null alias: the vote ended
static void on_vote_kick(KrunkerLink *l, const KrunkerFrame *f) {
    NetVoteKickStatePacket s;
    memset(&s, 0, sizeof(s));
    const char *alias = krunker_str(arg(f, 0));
    if (alias && alias[0]) {
        s.active = 1;
        s.votes = (int32_t) num(arg(f, 1), 0);
        s.needed = (int32_t) num(arg(f, 2), 0);
        const KrunkerValue *is_new = arg(f, 3);
        s.opened = (uint8_t) (!is_nil(is_new) && num(is_new, 0) != 0); // true / 1
        snprintf(s.target, sizeof(s.target), "%s", alias);
        const char *voter = krunker_str(arg(f, 4));
        snprintf(s.voter, sizeof(s.voter), "%s", voter ? voter : "");
    }
    emit_ext(l, NET_EXT_VOTE_KICK_STATE, &s, sizeof(s));
}

// An officer's list (policeRes codes 1..5, data an array of rows) as NET_EXT_KPD_RESPONSE: the rows as
// JSON after the struct, in pages of whole rows when they do not fit one packet (name "page/pages",
// as our own server sends them: server/src/accounts.c acc_kpd_send_list). A row too long for a page
// is left out. data that is no array (an object, nil) goes as one page holding it, or "[]".
static void kpd_list_page(KrunkerLink *l, const int code, const char *message, const char *tag, const char *json, const size_t len) {
    uint8_t body[NET_EXT_BODY_MAX];
    NetKpdResponsePacket r;
    memset(&r, 0, sizeof(r));
    r.code = (uint8_t) code;
    r.status = NET_KPD_REPORTED;
    snprintf(r.message, sizeof(r.message), "%s", message ? message : "");
    snprintf(r.name, sizeof(r.name), "%s", tag ? tag : "");
    memcpy(body, &r, sizeof(r));
    memcpy(body + sizeof(r), json, len);
    emit_ext(l, NET_EXT_KPD_RESPONSE, body, (uint16_t) (sizeof(r) + len));
}

static void kpd_list(KrunkerLink *l, const int code, const char *message, const KrunkerValue *data) {
    enum { ROOM = NET_EXT_BODY_MAX - (int) sizeof(NetKpdResponsePacket), PAGES_MAX = 64 };
    cJSON *rows = value_json(data);
    if (rows && !cJSON_IsArray(rows)) { // one value: a list of one
        cJSON *one = cJSON_CreateArray();
        cJSON_AddItemToArray(one, rows);
        rows = one;
    }
    char *pages[PAGES_MAX];
    int count = 0;
    char page[ROOM + 1];
    size_t len = 0;
    const cJSON *row;
    cJSON_ArrayForEach(row, rows) {
        char *text = cJSON_PrintUnformatted(row);
        const size_t n = text ? strlen(text) : 0;
        if (n && n + 2 <= ROOM) {
            if (len && len + 1 + n + 1 > ROOM) { // the page is full: close it
                page[len++] = ']';
                page[len] = 0;
                if (count < PAGES_MAX) pages[count++] = strdup(page);
                len = 0;
            }
            page[len] = len ? ',' : '[';
            len++;
            memcpy(page + len, text, n);
            len += n;
        }
        cJSON_free(text);
    }
    cJSON_Delete(rows);
    if (len) {
        page[len++] = ']';
        page[len] = 0;
        if (count < PAGES_MAX) pages[count++] = strdup(page);
    }
    if (!count) {
        kpd_list_page(l, code, message, "", "[]", 2);
        return;
    }
    for (int i = 0; i < count; i++) {
        char tag[16];
        if (count > 1) snprintf(tag, sizeof(tag), "%d/%d", i + 1, count);
        else tag[0] = 0;
        if (pages[i]) kpd_list_page(l, code, message, tag, pages[i], strlen(pages[i]));
        free(pages[i]);
    }
}

// "kpd" KPD_RESPONSE [code, message, data] (policeRes, :216287): code 0 answers a call ("reported"
// with data.n the suspect, else the message to show); codes 1..5 are the officer's lists (an account
// the server knows as K.P.D asked for them with ["kpd", tab]).
static void on_kpd_response(KrunkerLink *l, const KrunkerFrame *f) {
    const int code = (int) num(arg(f, 0), -1);
    if (code >= 1 && code <= 5) {
        kpd_list(l, code, krunker_str(arg(f, 1)), arg(f, 2));
        return;
    }
    if (code != 0) {
        note(l, KRUNKER_LINK_EV_NOTE, "kpd: response %d ignored", code);
        return;
    }
    NetKpdResponsePacket r;
    memset(&r, 0, sizeof(r));
    const char *message = krunker_str(arg(f, 1));
    const char *name = krunker_str(krunker_get(arg(f, 2), "n"));
    snprintf(r.name, sizeof(r.name), "%s", name ? name : "");
    if (message && !strcmp(message, "reported")) {
        r.status = NET_KPD_REPORTED;
        snprintf(r.message, sizeof(r.message), "Reported %s to the K.P.D", r.name);
    } else {
        r.status = NET_KPD_BUSY;
        snprintf(r.message, sizeof(r.message), "%s", message ? message : "");
    }
    emit_ext(l, NET_EXT_KPD_RESPONSE, &r, sizeof(r));
}

// startGame's kpdData {sus, caller} (socket ids, :218939): the officer's case as our uids; none = no case
static int sid_of_socket(const KrunkerLink *l, const char *socket_id) {
    for (int i = 0; socket_id && socket_id[0] && i < l->player_count; i++) {
        if (!strcmp(l->players[i].id, socket_id)) return l->players[i].sid;
    }
    return -1;
}

static void on_kpd_data(KrunkerLink *l, const KrunkerValue *kpd) {
    NetKpdDataPacket d = {.sus = -1, .caller = -1};
    if (kpd && !is_nil(kpd)) {
        d.sus = sid_of_socket(l, krunker_str(krunker_get(kpd, "sus")));
        d.caller = sid_of_socket(l, krunker_str(krunker_get(kpd, "caller")));
        if (d.sus < 0) d.sus = INT32_MAX; // a case whose suspect is not added yet: the panel asks about "Suspect"
    } else if (!l->kpd_case) return; // no case before, none now: nothing to say
    l->kpd_case = d.sus >= 0;
    emit_ext(l, NET_EXT_KPD_DATA, &d, sizeof(d));
}

// ---------------------------------------------------------------- score / kills / ammo HUD (audit cluster C)

// JS String(v) of a value (i18n arguments, leaderboard cells): numbers without a trailing ".0"
static void js_string(const KrunkerValue *v, char *out, const size_t n) {
    out[0] = 0;
    if (!v) {
        snprintf(out, n, "undefined");
    } else if (v->type == KRUNKER_STR) {
        snprintf(out, n, "%s", krunker_str(v));
    } else if (v->type == KRUNKER_BOOL) {
        snprintf(out, n, "%s", v->u.boolean ? "true" : "false");
    } else if (v->type == KRUNKER_NIL) {
        snprintf(out, n, "null");
    } else if (v->type == KRUNKER_NUM) {
        const double d = v->u.num;
        if (isnan(d)) snprintf(out, n, "NaN");
        else if (isinf(d)) snprintf(out, n, "%sInfinity", d < 0 ? "-" : "");
        else if (d == floor(d) && fabs(d) < 1e21) snprintf(out, n, "%.0f", d == 0 ? 0.0 : d); // -0 -> "0"
        else {
            for (int prec = 15; prec <= 17; prec++) { // the shortest form that reads back (Number.prototype.toString)
                snprintf(out, n, "%.*g", prec, d);
                if (strtod(out, NULL) == d) break;
            }
        }
    }
}

static void emit_hud_ext(KrunkerLink *l, const uint16_t subtype, const void *body, const uint16_t length) {
    uint8_t buf[NET_MAX_PAYLOAD];
    const uint16_t n = net_ext_pack(buf, sizeof(buf), subtype, body, length);
    if (n) emit(l, NET_PACKET_EXT, buf, n);
}

// The per-sid stats LEADERBOARD carries. A full table recycles a player who left.
static KrunkerLinkStats *stats(KrunkerLink *l, const int sid) {
    for (int i = 0; i < l->stats_count; i++) {
        if (l->stats[i].sid == sid) return &l->stats[i];
    }
    KrunkerLinkStats *s = NULL;
    if (l->stats_count < KRUNKER_LINK_MAX_PLAYERS) s = &l->stats[l->stats_count++];
    for (int i = 0; !s && i < l->stats_count; i++) {
        if (!player(l, l->stats[i].sid, 0)) s = &l->stats[i];
    }
    if (s) {
        memset(s, 0, sizeof(*s));
        s->sid = sid;
    }
    return s;
}

// mode.leaderStats ("7" columns) / mode.endStats ("end" ed columns, official servers append "reward")
// of the mode "init" announced: our mode table carries Krunker's lists (shared/src/modes/modes.c)
static const char *const k_default_leader_stats[] = {"score", NULL};
static const char *const k_default_end_stats[] = {"sid", "name", "score", "kills", "deaths", "reward", NULL}; // game.deob.js:18307

static int name_list(const char *const *list, const char *const **out) {
    int n = 0;
    while (list[n]) n++;
    *out = list;
    return n;
}

static int leader_stats(const KrunkerLink *l, const char *const **names) {
    const GameModeConfig *c = mode_config(l->mode);
    return name_list(c && c->leader_stats ? c->leader_stats : k_default_leader_stats, names);
}

// The medal queue's list (iîíìiïì, game.deob.js:220960): 4 values per medal, [name or overlay.medalsList
// index, score, sound, count]; "" names are the plain kill score (no popup). A score that is a text
// ("Lvl 5", "x2", "10 KR") shows no number here (NetMedalPacket scores are numbers).
static int medals_from(const KrunkerValue *list, NetMedalPacket *out, const int max) {
    int n = 0;
    if (krunker_is(list, KRUNKER_STR) && list->len && max > 0) { // a bare text, as a trigger's "am"
        memset(&out[0], 0, sizeof(out[0]));
        snprintf(out[0].text, sizeof(out[0].text), "%s", krunker_str(list));
        return 1;
    }
    if (!krunker_is(list, KRUNKER_ARRAY)) return 0;
    for (uint32_t i = 0; i < list->len && n < max; i += 4) {
        const KrunkerValue *name = &list->u.items[i];
        const KrunkerValue *score = krunker_at(list, i + 1);
        NetMedalPacket m;
        memset(&m, 0, sizeof(m));
        if (krunker_is(name, KRUNKER_NUM)) {
            const int index = (int) name->u.num;
            if (index < 0 || index >= PROGRESSION_MEDAL_COUNT) continue;
            snprintf(m.text, sizeof(m.text), "%s", g_progression_medals[index]);
        } else if (krunker_is(name, KRUNKER_STR) && name->len) {
            snprintf(m.text, sizeof(m.text), "%s", krunker_str(name));
        } else {
            continue;
        }
        m.score = krunker_is(score, KRUNKER_NUM) && isfinite(score->u.num) ? (int32_t) score->u.num : 0;
        out[n++] = m;
    }
    return n;
}

// "ts" TEAM_SCORE [team, score] (iïììïíì :220857): teamScores[team] = score || 0; no team (the
// best-of round reset, broadcast('ts') :191172) clears them: our TEAM_SCORE with team 0.
static void on_team_score(KrunkerLink *l, const KrunkerFrame *f) {
    NetTeamScorePacket t = {.team = 0, .score = 0};
    if (krunker_is(arg(f, 0), KRUNKER_NUM)) {
        t.team = (int32_t) arg(f, 0)->u.num;
        t.score = (int32_t) js_round(num(arg(f, 1), 0));
        if (t.team <= 0) return;
    }
    emit(l, NET_PACKET_TEAM_SCORE, &t, sizeof(t));
}

// "5" GET_SCORE [delta, relay, absolute, classIndex] (iiìïíìî :220916): unless absolute the "+N"
// popup; unless relay (a spectator's copy of the followed player's, :173709) our score becomes delta
// (absolute) or score + delta. classIndex feeds account classScores only (not modelled).
static void on_get_score(KrunkerLink *l, const KrunkerFrame *f) {
    const KrunkerBuild *b = krunker_build();
    const double d = num(role(f, b->score, KRB_SCORE_DELTA), 0);
    const int delta = isfinite(d) ? (int) js_round(d) : 0;
    const int absolute = truthy(role(f, b->score, KRB_SCORE_ABSOLUTE));
    if (!truthy(role(f, b->score, KRB_SCORE_RELAY))) {
        l->my_score = absolute ? delta : l->my_score + delta;
        KrunkerLinkStats *s = l->my_sid >= 0 ? stats(l, l->my_sid) : NULL;
        if (s) s->score = l->my_score;
    }
    const NetScorePacket p = {.delta = absolute ? 0 : delta, .score = l->my_score};
    emit(l, NET_PACKET_SCORE, &p, sizeof(p));
}

// "6" GET_KILL [medals, killSound, totalKills, inc] (iíïïîìí :220948), to the killer, right before
// the "3" everyone gets (players.kill :174098): the medals, our kills = totalKills (killsVal)
static void on_get_kill(KrunkerLink *l, const KrunkerFrame *f) {
    NetKillMedalsPacket k;
    memset(&k, 0, sizeof(k));
    k.count = (uint8_t) medals_from(arg(f, 0), k.medals, NET_KILL_MEDALS_MAX);
    KrunkerLinkStats *s = l->my_sid >= 0 ? stats(l, l->my_sid) : NULL;
    if (s) {
        if (krunker_is(arg(f, 2), KRUNKER_NUM)) s->kills = (int) arg(f, 2)->u.num;
        else s->kills += (int) num(arg(f, 3), 1); // killsC += inc || 1
    }
    k.kills = s ? s->kills : (int32_t) num(arg(f, 2), 0);
    emit(l, NET_PACKET_KILL_MEDALS, &k, sizeof(k));
}

// "7" UPDATE_LEADERS [sid, ...leaderStats values]* (iîììíìï :220727): the mode's leaderStats ("score"
// by default) per row, in the server's board order. LEADERBOARD takes score / kills / deaths from
// the columns that name them (else what "3" / "6" told us; ours is always our "5" score, as the live
// client's myScoreVal), LEADER_STATS the columns as sent.
static void on_update_leaders(KrunkerLink *l, const KrunkerFrame *f) {
    const KrunkerValue *list = arg(f, 0);
    if (!krunker_is(list, KRUNKER_ARRAY)) return;
    const char *const *names;
    const int columns = leader_stats(l, &names);
    const uint32_t stride = (uint32_t) columns + 1;
    NetLeaderboardPacket board;
    NetLeaderStatsPacket cols;
    memset(&board, 0, sizeof(board));
    memset(&cols, 0, sizeof(cols));
    cols.stat_count = (uint8_t) (columns < NET_LEADER_STATS_MAX ? columns : NET_LEADER_STATS_MAX);
    for (uint32_t i = 0; i + stride <= list->len && board.count < GAME_LEADERBOARD_MAX; i += stride) {
        const KrunkerValue *e = list->u.items + i;
        if (!krunker_is(&e[0], KRUNKER_NUM)) continue;
        const int sid = (int) e[0].u.num;
        KrunkerLinkStats *s = stats(l, sid);
        for (int c = 0; c < columns; c++) {
            const double v = num(&e[1 + c], 0);
            const int iv = isfinite(v) ? (int) js_round(v) : 0;
            if (c < NET_LEADER_STATS_MAX) cols.values[cols.count][c] = iv;
            if (!s) continue;
            if (!strcmp(names[c], "score")) s->score = iv;
            else if (!strcmp(names[c], "kills")) s->kills = iv;
            else if (!strcmp(names[c], "deaths")) s->deaths = iv;
        }
        if (s && sid == l->my_sid) s->score = l->my_score; // the board never sets you.score (myScoreVal): "5" does
        cols.uid[cols.count++] = sid;
        board.entries[board.count++] = (NetLeaderboardEntry) {.uid = sid, .score = s ? s->score : 0, .kills = s ? s->kills : 0,
                                                              .deaths = s ? s->deaths : 0};
    }
    emit(l, NET_PACKET_LEADERBOARD, &board, sizeof(board));
    emit(l, NET_PACKET_LEADER_STATS, &cols, sizeof(cols));
}

// "ulb" UPDATE_LEADER_BOARD [index, [player, value, ...]] (iìïîìiï :220065, sent :178418): a map
// LEADERBOARD object's rows (the leaderboard service's "pl" / "kv" pairs), whole rows that fit
static void on_leader_board(KrunkerLink *l, const KrunkerFrame *f) {
    const KrunkerValue *rows = arg(f, 1);
    if (!krunker_is(arg(f, 0), KRUNKER_NUM) || !krunker_is(rows, KRUNKER_ARRAY)) return;
    uint8_t body[NET_EXT_BODY_MAX];
    NetOfficialLeaderBoardHeader h = {.index = (int32_t) arg(f, 0)->u.num, .cells = 0};
    size_t len = sizeof(h);
    for (uint32_t i = 0; i + 1 < rows->len && h.cells < 254; i += 2) {
        char name[96], value[48];
        js_string(&rows->u.items[i], name, sizeof(name));
        js_string(&rows->u.items[i + 1], value, sizeof(value));
        const size_t a = strlen(name) + 1, b = strlen(value) + 1;
        if (len + a + b > sizeof(body)) break;
        memcpy(body + len, name, a);
        memcpy(body + len + a, value, b);
        len += a + b;
        h.cells += 2;
    }
    memcpy(body, &h, sizeof(h));
    emit_hud_ext(l, NET_EXT_KN_OFFICIAL_LEADER_BOARD, body, (uint16_t) len);
}

// "uamo" UPDATE_AMMO [ammos, reserveAmmos] (iìíiiìì :220425): our magazines per loadout slot (and the
// finiteAmmo reserves), then updatePlayerAmmo (#ammoVal / #ammoMax)
static int ammo_list(const KrunkerValue *list, int32_t *out) {
    int n = 0;
    for (uint32_t i = 0; krunker_is(list, KRUNKER_ARRAY) && i < list->len && n < NET_OFFICIAL_AMMO_SLOTS; i++) {
        const KrunkerValue *v = &list->u.items[i];
        out[n++] = krunker_is(v, KRUNKER_NUM) && isfinite(v->u.num) ? (int32_t) v->u.num : -1;
    }
    return n;
}

static void on_update_ammo(KrunkerLink *l, const KrunkerFrame *f) {
    if (!krunker_is(arg(f, 0), KRUNKER_ARRAY)) return;
    NetOfficialAmmoPacket a;
    memset(&a, 0, sizeof(a));
    a.count = (uint8_t) ammo_list(arg(f, 0), a.ammo);
    a.reserve_count = (uint8_t) ammo_list(arg(f, 1), a.reserve);
    emit_hud_ext(l, NET_EXT_KN_OFFICIAL_AMMO, &a, sizeof(a));
}

// "lv" UPDATE_LIVES [lives] (iiiiíìí :220940): #livesDisp
static void on_lives(KrunkerLink *l, const KrunkerFrame *f) {
    if (!krunker_is(arg(f, 0), KRUNKER_NUM)) return;
    const NetLivesPacket p = {.lives = (int32_t) arg(f, 0)->u.num};
    emit(l, NET_PACKET_LIVES, &p, sizeof(p));
}

// "st" KILL_STREAK [streakId, count] (iìîiïiï :220651): our callable streaks (count 0 / null: gone)
static void on_streak(KrunkerLink *l, const KrunkerFrame *f) {
    const int id = (int) num(arg(f, 0), -1), count = (int) num(arg(f, 1), 0);
    if (id < 0 || id > 255) return;
    const NetStreakPacket p = {.streak = (uint8_t) id, .count = (uint8_t) (count < 0 ? 0 : count > 255 ? 255 : count)};
    emit(l, NET_PACKET_STREAK, &p, sizeof(p));
}

// "kst" KILL_STREAK_M [sid, streak] (iiiïiíi :220705, sent every 5 kills :174120): the chat line
// "player.killstreak"
static void on_kill_streak_message(KrunkerLink *l, const KrunkerFrame *f) {
    if (!krunker_is(arg(f, 0), KRUNKER_NUM)) return;
    const NetKillStreakPacket p = {.uid = (int32_t) arg(f, 0)->u.num, .streak = (int32_t) num(arg(f, 1), 0)};
    emit(l, NET_PACKET_KILL_STREAK, &p, sizeof(p));
}

// "wstk" UPDATE_WEAPON_STREAK [currentStreakWeapon, weaponStreak] (iìîíîîí :220669)
static void on_weapon_streak(KrunkerLink *l, const KrunkerFrame *f) {
    const NetWeaponStreakPacket p = {.weapon = (int32_t) num(arg(f, 0), -1), .count = (int32_t) num(arg(f, 1), 0)};
    emit(l, NET_PACKET_WEAPON_STREAK, &p, sizeof(p));
}

// "am" ADD_MEDAL [[medal, score, sound, count]] (iîíìiïì :220960): one popup per medal
// "am" ADD_MEDAL [[name | medal index, score, sound, count]*] (iîíìiïì :220960): each medal as v10 shows
// it: the score as given (a text such as the reward's "12 KR", a number as "+N"), its sound and count
// (NET_EXT_KN_OFFICIAL_MEDAL); a bare text (a trigger's popup) stays a NET_PACKET_MEDAL without score
static void on_add_medal(KrunkerLink *l, const KrunkerFrame *f) {
    const KrunkerValue *list = arg(f, 0);
    if (!krunker_is(list, KRUNKER_ARRAY)) {
        NetMedalPacket medals[NET_KILL_MEDALS_MAX];
        const int n = medals_from(list, medals, NET_KILL_MEDALS_MAX);
        for (int i = 0; i < n; i++) emit(l, NET_PACKET_MEDAL, &medals[i], sizeof(medals[i]));
        return;
    }
    for (uint32_t i = 0; i < list->len; i += 4) {
        const KrunkerValue *name = &list->u.items[i], *score = krunker_at(list, i + 1);
        NetOfficialMedalPacket m;
        memset(&m, 0, sizeof(m));
        if (krunker_is(name, KRUNKER_NUM)) {
            const int index = (int) name->u.num;
            if (index < 0 || index >= PROGRESSION_MEDAL_COUNT) continue;
            snprintf(m.text, sizeof(m.text), "%s", g_progression_medals[index]);
        } else if (krunker_is(name, KRUNKER_STR) && name->len) {
            snprintf(m.text, sizeof(m.text), "%s", krunker_str(name));
        } else {
            continue; // '' != name
        }
        if (krunker_is(score, KRUNKER_NUM) && isfinite(score->u.num) && score->u.num != 0)
            snprintf(m.score, sizeof(m.score), "%s%d", score->u.num > 0 ? "+" : "", (int) score->u.num);
        else if (krunker_is(score, KRUNKER_STR)) snprintf(m.score, sizeof(m.score), "%s", krunker_str(score));
        const double sound = num(krunker_at(list, i + 2), 0), count = num(krunker_at(list, i + 3), 1);
        m.sound = (uint8_t) (sound > 0 && sound < 256 ? sound : 0);
        m.count = (uint8_t) (count >= 1 && count < 256 ? count : 1); // iìÏiïiî[i + 3] || 1
        emit_hud_ext(l, NET_EXT_KN_OFFICIAL_MEDAL, &m, sizeof(m));
    }
}

// "krt" UPDATE_KR_TAGS [dogTags, depositType] (iííîïii :220680): our tag count (#krTagHolder). A
// depositType (gift / event tags, the live client's #giftTagHolder) is not our HUD's: it is not ours
// to show, so it must not overwrite the KR count.
static void on_kr_tags(KrunkerLink *l, const KrunkerFrame *f) {
    if (l->my_sid < 0 || num(arg(f, 1), 0) != 0) return;
    const NetModeCounterPacket p = {.kind = NET_MODE_COUNTER_KR_TAGS, .uid = l->my_sid, .value = (float) num(arg(f, 0), 0)};
    emit(l, NET_PACKET_MODE_COUNTER, &p, sizeof(p));
}

// ---------------------------------------------------------------- objectives / flags / rounds / end (audit cluster D)

// The flag with that map object uid: its slot in init's syncData.flg order (= our map-order flag
// index); a uid the init did not list takes the next slot
static int flag_slot(KrunkerLink *l, const int uid) {
    for (int i = 0; i < l->flag_count; i++) {
        if (l->flags[i].uid == uid) return i;
    }
    if (l->flag_count >= MODE_MAX_FLAGS) return -1;
    KrunkerLinkFlag *flag = &l->flags[l->flag_count];
    memset(flag, 0, sizeof(*flag));
    flag->uid = uid;
    return l->flag_count++;
}

// NET_PACKET_FLAG of one slot: team 0 and info "" keep the client's (its map knows the flag's team,
// "ufi" brings the line)
static void emit_flag(KrunkerLink *l, const int slot) {
    NetFlagPacket p;
    memset(&p, 0, sizeof(p));
    p.index = slot;
    memcpy(p.flag.position, l->flags[slot].position, sizeof(p.flag.position));
    p.flag.carrier_uid = l->flags[slot].carrier > 0 ? l->flags[slot].carrier : -1;
    emit(l, NET_PACKET_FLAG, &p, sizeof(p));
}

static void flag_position(KrunkerLinkFlag *flag, const KrunkerValue *x, const KrunkerValue *y, const KrunkerValue *z) {
    if (krunker_is(x, KRUNKER_NUM)) flag->position[0] = (float) x->u.num;
    if (krunker_is(y, KRUNKER_NUM)) flag->position[1] = (float) y->u.num;
    if (krunker_is(z, KRUNKER_NUM)) flag->position[2] = (float) z->u.num;
}

// "ufl" UPDATE_FLAG [uid, x, y, z, carrier || 0] (game.updateFlag :188774, syncFlag :188796)
static void on_flag(KrunkerLink *l, const KrunkerFrame *f) {
    if (!krunker_is(arg(f, 0), KRUNKER_NUM)) return;
    const int slot = flag_slot(l, (int) arg(f, 0)->u.num);
    if (slot < 0) return;
    flag_position(&l->flags[slot], arg(f, 1), arg(f, 2), arg(f, 3));
    l->flags[slot].carrier = (int) num(arg(f, 4), 0);
    emit_flag(l, slot);
}

// "ufp" UPDATE_FLAG_PING [uid, x, y, z] (game.updateFlagPing :188830): where the HUD shows it (img)
static void on_flag_ping(KrunkerLink *l, const KrunkerFrame *f) {
    if (!krunker_is(arg(f, 0), KRUNKER_NUM)) return;
    const int slot = flag_slot(l, (int) arg(f, 0)->u.num);
    if (slot < 0) return;
    flag_position(&l->flags[slot], arg(f, 1), arg(f, 2), arg(f, 3));
    emit_flag(l, slot);
}

// "ufi" UPDATE_FLAG_INFO [team, text] (iïîíîíí :220911): teamFlags[team] ("AT BASE", "AWAY", the
// carrier's name): our FLAG with index -1 updates that line only
static void on_flag_info(KrunkerLink *l, const KrunkerFrame *f) {
    if (!krunker_is(arg(f, 0), KRUNKER_NUM)) return;
    NetFlagPacket p;
    memset(&p, 0, sizeof(p));
    p.index = -1;
    p.flag.team = (int32_t) arg(f, 0)->u.num;
    p.flag.carrier_uid = -1;
    js_string(arg(f, 1), p.info, sizeof(p.info));
    if (is_nil(arg(f, 1))) p.info[0] = 0;
    emit(l, NET_PACKET_FLAG, &p, sizeof(p));
}

// "obj" SET_OBJECTIVE [uid, objtvTime ms, initial] (game.setObjective :189407, nextObjective :189433)
static void on_objective(KrunkerLink *l, const KrunkerFrame *f) {
    const NetObjectiveV7Packet p = {.uid = (int32_t) num(arg(f, 0), -1), .time_ms = (int32_t) num(arg(f, 1), 0),
                                    .initial = (uint8_t) truthy(arg(f, 2))};
    emit(l, NET_PACKET_OBJECTIVE_V7, &p, sizeof(p));
}

// "uobj" UPDATE_OBJECTIVE [uid, onZone] (game.updateObjective :189448)
static void on_objective_occupancy(KrunkerLink *l, const KrunkerFrame *f) {
    const NetObjectiveOccupancyPacket p = {.uid = (int32_t) num(arg(f, 0), -1), .on_zone = (int32_t) num(arg(f, 1), 0)};
    emit(l, NET_PACKET_OBJECTIVE_OCCUPANCY, &p, sizeof(p));
}

// "rds" UPDATE_ROUNDS_DIS [roundCount, noSwitch, winTeam, teamRoundWins] (iïìíììî :219243, the server
// always sends noSwitch false :191198): "Switching Sides", the lead, roundsVal and the round wins.
// A noSwitch frame (none so far) only redraws roundsVal, which our ROUNDS cannot say: dropped.
static void on_rounds(KrunkerLink *l, const KrunkerFrame *f) {
    if (truthy(arg(f, 1))) return;
    NetRoundsPacket p;
    memset(&p, 0, sizeof(p));
    p.rounds_left = (int32_t) num(arg(f, 0), 0);
    p.win_team = (int32_t) num(arg(f, 2), 0);
    const KrunkerValue *wins = arg(f, 3);
    p.has_round_wins = (uint8_t) krunker_is(wins, KRUNKER_MAP);
    for (int t = 1; p.has_round_wins && t < MODE_TEAMS; t++) {
        char key[8];
        snprintf(key, sizeof(key), "%d", t);
        p.team_round_wins[t] = (int32_t) num(krunker_get(wins, key), 0);
    }
    emit(l, NET_PACKET_ROUNDS, &p, sizeof(p));
    // Switching Sides (:219244-219262): every player inactive (our own included: no input frame leaves
    // until our next "0", as with no `you`), aim off, the death card hidden, the overview camera and
    // the menu after 3 s unless we were respawned; the client does the visible part
    if (l->my_sid >= 0) l->you_gone = 1;
    emit_ext(l, NET_EXT_KN_OFFICIAL_ROUND_SWITCH, NULL, 0);
}

// "zn" UPDATE_ZONE [[x, z, sizeX, sizeZ]] (game.updateZone :189401 -> zone.update(0, data))
static void zone_packet(KrunkerLink *l, const KrunkerValue *data) {
    if (!krunker_is(data, KRUNKER_ARRAY) || data->len < 4) return;
    const NetZonePacket p = {.x = (float) num(krunker_at(data, 0), 0), .z = (float) num(krunker_at(data, 1), 0),
                             .size_x = (float) num(krunker_at(data, 2), 0), .size_z = (float) num(krunker_at(data, 3), 0)};
    emit(l, NET_PACKET_ZONE, &p, sizeof(p));
}

// "ac" ACTION_FEED [sid, sid2, text] (iìîïiíí :220711): "<you|name> <text> <you|name2>"
static void on_action(KrunkerLink *l, const KrunkerFrame *f) {
    if (!krunker_is(arg(f, 0), KRUNKER_NUM)) return;
    NetActionPacket p;
    memset(&p, 0, sizeof(p));
    p.uid = (int32_t) arg(f, 0)->u.num;
    p.other_uid = krunker_is(arg(f, 1), KRUNKER_NUM) ? (int32_t) arg(f, 1)->u.num : -1;
    const char *text = krunker_str(arg(f, 2));
    snprintf(p.text, sizeof(p.text), "%s", text ? text : "");
    emit(l, NET_PACKET_ACTION, &p, sizeof(p));
}

// "chi" ADD_CHATI18N [team, name, key | [key, ...args], kind, color, ...] (iïîíïîï :221142):
// addChat(team, name, i18n.t(key, ...args), kind, color). Translated by the client (main thread).
// One localized chat line (Krunker's addChat(team, name, i18n.t(key, ...args), kind, color)) as
// NET_EXT_KN_OFFICIAL_CHAT_I18N; kind is Krunker's (0 player, 2 server, else a game line).
static void emit_chat_i18n(KrunkerLink *l, const int8_t channel, const int kind, const char *name, const char *key,
                           const char *const *args, const int argc, const char *color, const char *name_color) {
    uint8_t body[NET_EXT_BODY_MAX];
    NetOfficialChatI18nHeader h;
    memset(&h, 0, sizeof(h));
    h.kind = (uint8_t) (kind == 0 ? NET_CHAT_PLAYER : kind == 2 ? NET_CHAT_SERVER : NET_CHAT_GAME);
    h.channel = channel;
    if (color) snprintf(h.color, sizeof(h.color), "%s", color);
    if (name_color) snprintf(h.name_color, sizeof(h.name_color), "%s", name_color);
    size_t len = sizeof(h);
    const char *head[2] = {name ? name : "", key};
    for (int i = 0; i < 2; i++) {
        const size_t n = strlen(head[i]) + 1;
        if (len + n > sizeof(body)) return;
        memcpy(body + len, head[i], n);
        len += n;
    }
    for (int i = 0; i < argc && h.argc < 16; i++) {
        const size_t n = strlen(args[i]) + 1;
        if (len + n > sizeof(body)) break;
        memcpy(body + len, args[i], n);
        len += n;
        h.argc++;
    }
    memcpy(body, &h, sizeof(h));
    emit_hud_ext(l, NET_EXT_KN_OFFICIAL_CHAT_I18N, body, (uint16_t) len);
}

static void on_chat_i18n(KrunkerLink *l, const KrunkerFrame *f) {
    const KrunkerValue *k = arg(f, 2);
    const KrunkerValue *key = krunker_is(k, KRUNKER_ARRAY) ? krunker_at(k, 0) : k;
    const uint32_t argc = krunker_is(k, KRUNKER_ARRAY) && k->len ? k->len - 1 : 0;
    const char *ks = krunker_str(key);
    const int kind = (int) num(arg(f, 3), 0);
    if (!ks || !ks[0] || kind == 3) return;
    char store[16][128];
    const char *args[16];
    int n = 0;
    for (uint32_t i = 0; i < argc && n < 16; i++, n++) {
        js_string(krunker_at(k, i + 1), store[n], sizeof(store[n]));
        args[n] = store[n];
    }
    emit_chat_i18n(l, chat_channel(arg(f, 0)), kind, krunker_str(arg(f, 1)), ks, args, n, krunker_str(arg(f, 4)), krunker_str(arg(f, 5)));
}

// "uh" UPDATE_HOST [hostSocketId | null, silent] (iíîíïìï :219273): game.host (the player list's star:
// PLAYER_INFO HOST) and, unless silent, "You are now Host!" / "Host Transfered to <name>" (kind 2)
static void on_update_host(KrunkerLink *l, const KrunkerFrame *f) {
    char host[sizeof(l->host_id)];
    id_string(arg(f, 0), host, sizeof(host));
    const int changed = strcmp(host, l->host_id) != 0;
    snprintf(l->host_id, sizeof(l->host_id), "%s", host);
    for (int i = 0; changed && i < l->player_count; i++) emit_player_info(l, &l->players[i]);
    if (truthy(arg(f, 1)) || !host[0]) return;
    if (l->socket_id[0] && !strcmp(host, l->socket_id)) {
        emit_chat_i18n(l, CHAT_CHANNEL_GLOBAL, 2, NULL, "host.transfer.you", NULL, 0, NULL, NULL); // addChat(0, null, ..)
        return;
    }
    for (int i = 0; i < l->player_count; i++) {
        if (strcmp(l->players[i].id, host) != 0) continue;
        const char *args[1] = {l->players[i].name};
        emit_chat_i18n(l, CHAT_CHANNEL_GLOBAL, 2, NULL, "host.transfer", args, 1, NULL, NULL);
        return;
    }
}

// endForm's reward cell: player.reward ? "<span style='color:#F8C55C'>+N</span> KR" : "NONE", or
// [that, skin] with a skin reward; a number from a server that sends it raw
static int end_reward(const KrunkerValue *v) {
    if (krunker_is(v, KRUNKER_ARRAY)) v = krunker_at(v, 0);
    if (krunker_is(v, KRUNKER_NUM)) return (int) v->u.num;
    const char *s = krunker_str(v), *plus = s ? strchr(s, '+') : NULL;
    return plus ? atoi(plus + 1) : 0;
}

// endForm's time cell: player.timer, the formatted clock ("1:23.4", "DNF"): tenths + 1, 0 = DNF
static int end_time(const KrunkerValue *v) {
    const char *s = krunker_str(v);
    if (!s || !s[0] || !(s[0] >= '0' && s[0] <= '9')) return 0;
    int total = 0, field = 0, tenths = 0;
    for (; *s; s++) {
        if (*s >= '0' && *s <= '9') field = field * 10 + (*s - '0');
        else if (*s == ':') total = total * 60 + field, field = 0;
        else if (*s == '.') {
            tenths = s[1] >= '0' && s[1] <= '9' ? s[1] - '0' : 0;
            break;
        }
    }
    const int t = (total * 60 + field) * 10 + tenths + 1;
    return t > 32767 ? 32767 : t;
}

// an "end" ed column -> NetModeStat (-1: none; score / kills / deaths / reward ride END's board)
static int end_stat(const char *name) {
    static const struct {
        const char *name;
        int stat;
    } k[] = {{"caps", NET_MODE_STAT_CAPS}, {"confirms", NET_MODE_STAT_CONFIRMS}, {"denies", NET_MODE_STAT_DENIES},
             {"obj", NET_MODE_STAT_OBJECTIVE}, {"objective", NET_MODE_STAT_OBJECTIVE}, {"weapon", NET_MODE_STAT_WEAPON_TIER},
             {"setbacks", NET_MODE_STAT_SETBACKS}, {"melees", NET_MODE_STAT_MELEES}, {"infected", NET_MODE_STAT_CONVS},
             {"found", NET_MODE_STAT_CONVS}, {"lives", NET_MODE_STAT_LIVES}, {"assists", NET_MODE_STAT_ASSISTS},
             {"time", NET_MODE_STAT_TIME}, {"deposits", NET_MODE_STAT_DEPOSITS}, {"pickups", NET_MODE_STAT_PICKUPS}};
    for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
        if (!strcmp(name, k[i].name)) return k[i].stat;
    }
    return -1;
}

static int16_t clamp16(const double v) {
    return (int16_t) (!isfinite(v) ? 0 : v > 32767 ? 32767 : v < -32768 ? -32768 : (int) v);
}

// "end" END_GAME [type, didWin, endData, winTeam, endIndex, reward, bpData, items] (iíiiiiì :219712,
// sent per player :191402). endData {ed, tms, mdls, mts, vo, modeIndex}: ed is the end table, one row
// of mode.endStats values per player in the server's end order (endStats + "reward" on official
// servers, endForm applied: the name cell is [name, alias]). type 2 shows no end screen (iìÏïíîì);
// didWin -1 is a draw. The "Next Round in" clock comes with the "t" frames that follow (endTimer
// 25 s until then, our GAME_END_TIMER). mts / vo: on_match_vote. Not mapped: the KR / battle
// pass / item rewards of the account (a guest has none), endData.mdls (END_MEDALS wants our medal
// indexes; the medals tab stays empty).
static void on_match_vote(KrunkerLink *l, const KrunkerValue *vo, const KrunkerValue *mts); // below
static void on_end_game(KrunkerLink *l, const KrunkerFrame *f) {
    const KrunkerValue *data = arg(f, 2);
    // the vote cards (:219805-219826, updMatchVote(vo, mts)) ahead of the END that shows them
    if (krunker_is(krunker_get(data, "mts"), KRUNKER_ARRAY)) on_match_vote(l, krunker_get(data, "vo"), krunker_get(data, "mts"));
    const KrunkerValue *ed = krunker_get(data, "ed");
    const GameModeConfig *mode = mode_config(l->mode);
    const char *const *cols;
    int ncols = name_list(mode && mode->end_stats ? mode->end_stats : k_default_end_stats, &cols);
    const char *names[24];
    int n = 0;
    for (; n < ncols && n < 23; n++) names[n] = cols[n];
    int has_reward = 0;
    for (int i = 0; i < n; i++) has_reward |= !strcmp(names[i], "reward");
    if (!has_reward && l->is_official) names[n++] = "reward"; // isOfficial: endStats.push("reward") (:191350, :219864)
    // the row length: the columns, unless they do not fit the rows (another build's endStats): then
    // the distance between two name cells ([name, alias]), and only the sids are read
    uint32_t stride = (uint32_t) n;
    int fits = krunker_is(ed, KRUNKER_ARRAY) && ed->len % stride == 0;
    for (uint32_t i = 0; fits && i < ed->len; i += stride) {
        const KrunkerValue *name = &ed->u.items[i + 1];
        fits = krunker_is(&ed->u.items[i], KRUNKER_NUM) && (krunker_is(name, KRUNKER_ARRAY) || krunker_is(name, KRUNKER_STR));
    }
    if (!fits && krunker_is(ed, KRUNKER_ARRAY)) {
        uint32_t first = 0, second = 0;
        for (uint32_t i = 1; i < ed->len && !second; i++) {
            const KrunkerValue *v = &ed->u.items[i];
            if (!krunker_is(v, KRUNKER_ARRAY) || !krunker_is(krunker_at(v, 0), KRUNKER_STR)) continue;
            if (!first) first = i;
            else second = i;
        }
        stride = second > first ? second - first : first ? ed->len : 0;
        if (!stride) return;
    }

    NetEndPacket end;
    NetModeEndPacket me;
    memset(&end, 0, sizeof(end));
    memset(&me, 0, sizeof(me));
    for (uint32_t i = 0; krunker_is(ed, KRUNKER_ARRAY) && i + stride <= ed->len && end.board.count < GAME_LEADERBOARD_MAX; i += stride) {
        const KrunkerValue *row = ed->u.items + i;
        if (!krunker_is(&row[0], KRUNKER_NUM)) continue;
        const int sid = (int) row[0].u.num;
        KrunkerLinkStats *s = stats(l, sid);
        NetLeaderboardEntry *e = &end.board.entries[end.board.count];
        NetModeEndEntry *m = me.count < NET_MODE_END_MAX ? &me.entries[me.count] : NULL;
        e->uid = sid;
        if (m) m->uid = sid;
        NetOfficialEndRowPacket er;
        memset(&er, 0, sizeof(er));
        er.uid = sid;
        er.dmg = -1;
        // the name cell [name, alias] (alias || name), as v10 prints a player who already left (:219880)
        const KrunkerValue *cell = stride > 1 ? &ed->u.items[i + 1] : NULL;
        const KrunkerValue *shown = krunker_is(cell, KRUNKER_ARRAY) ? krunker_at(cell, 1) : cell;
        if (!krunker_is(shown, KRUNKER_STR) || !shown->len) shown = krunker_is(cell, KRUNKER_ARRAY) ? krunker_at(cell, 0) : NULL;
        if (krunker_is(shown, KRUNKER_STR)) snprintf(er.name, sizeof(er.name), "%s", krunker_str(shown));
        for (int c = 1; fits && c < n; c++) {
            const KrunkerValue *v = &row[c];
            if (!strcmp(names[c], "reward")) {
                end.rewards[end.board.count] = end_reward(v);
                er.reward = end.rewards[end.board.count];
            } else if (!strcmp(names[c], "dmg")) {
                er.dmg = (int32_t) num(v, 0); // endForm.dmg: round(dmgDealt || 0)
            } else if (!strcmp(names[c], "score")) {
                er.score = (int32_t) js_round(num(v, s ? s->score : 0));
                if (s) s->score = er.score;
            } else if (!strcmp(names[c], "kills")) {
                er.kills = (int32_t) num(v, s ? s->kills : 0);
                if (s) s->kills = er.kills;
            } else if (!strcmp(names[c], "deaths")) {
                er.deaths = (int32_t) num(v, s ? s->deaths : 0);
                if (s) s->deaths = er.deaths;
            } else if (m) {
                const int stat = end_stat(names[c]);
                if (stat == NET_MODE_STAT_TIME) m->stats[stat] = (int16_t) end_time(v);
                else if (stat >= 0) m->stats[stat] = clamp16(num(v, 0));
            }
        }
        if (s) {
            e->score = s->score;
            e->kills = s->kills;
            e->deaths = s->deaths;
        }
        if (s) er.score = e->score, er.kills = e->kills, er.deaths = e->deaths;
        emit_hud_ext(l, NET_EXT_KN_OFFICIAL_END_ROW, &er, sizeof(er));
        end.board.count++;
        if (m) me.count++;
    }

    const int type = (int) num(arg(f, 0), 0);
    if (type == 2) { // no end screen: the table still updates the players
        emit(l, NET_PACKET_LEADERBOARD, &end.board, sizeof(end.board));
        return;
    }
    const KrunkerValue *won = arg(f, 1);
    end.result = (int8_t) (krunker_is(won, KRUNKER_NUM) && won->u.num == -1 ? -1 : truthy(won) ? 1 : 0);
    end.next_round = (int32_t) GAME_END_TIMER;
    const int win_team = (int) num(arg(f, 3), -1);
    end.winner_uid = win_team <= 0 && end.result != -1 && end.board.count ? end.board.entries[0].uid : -1; // FFA: the first row
    // type 1: the end screen with its results at once, also for a player who is not in the game (one who
    // joins the lobby while its round is over: v10's end handler, v10.js:219715): the client is told first
    const uint8_t end_type = (uint8_t) (type == 1);
    emit_ext(l, NET_EXT_KN_END_TYPE, &end_type, sizeof(end_type));
    emit(l, NET_PACKET_END, &end, sizeof(end));

    me.mode_id = l->mode;
    me.winner_team = win_team > 0 ? win_team : -1;
    const KrunkerValue *tms = krunker_get(data, "tms"); // getTeamScores(true): [team, score, flag line]*
    for (uint32_t i = 0; krunker_is(tms, KRUNKER_ARRAY) && i < tms->len; i++) {
        const KrunkerValue *t = &tms->u.items[i];
        const int team = (int) num(krunker_at(t, 0), 0);
        if (team == 1 || team == 2) me.team_scores[team - 1] = (int32_t) num(krunker_at(t, 1), 0);
    }
    emit(l, NET_PACKET_MODE_END, &me, sizeof(me));
}

// "init" (INIT_GAME, game.deob.js:219289; sent :191450 / :222514) for clusters C / D: [3] the round
// wins, [4] getTeamScores() [team, score, flag line]*, [11] getSyncData() {flg: [uid, x, y, z,
// carrier, timer]* in map order, obj: [uid, ms], zone, ...}. The mode picks the "7" / "end" columns;
// a joiner's team scores, flags, objective and zone come from here (the live client's initGame).
static void on_init_hud(KrunkerLink *l, const KrunkerFrame *f) {
    l->mode = (int) num(arg(f, 1), 0);
    l->my_score = 0;
    l->stats_count = 0;
    l->flag_count = 0;
    if (l->unsupported_map >= 0) return; // no INIT went out: nothing to seed
    const KrunkerValue *scores = arg(f, 4);
    for (uint32_t i = 0; krunker_is(scores, KRUNKER_ARRAY) && i < scores->len; i++) {
        const KrunkerValue *t = &scores->u.items[i];
        if (!krunker_is(krunker_at(t, 0), KRUNKER_NUM)) continue;
        const NetTeamScorePacket ts = {.team = (int32_t) krunker_at(t, 0)->u.num, .score = (int32_t) num(krunker_at(t, 1), 0)};
        if (ts.team > 0) emit(l, NET_PACKET_TEAM_SCORE, &ts, sizeof(ts));
        if (ts.team > 0 && krunker_is(krunker_at(t, 2), KRUNKER_STR)) {
            NetFlagPacket info;
            memset(&info, 0, sizeof(info));
            info.index = -1;
            info.flag.team = ts.team;
            info.flag.carrier_uid = -1;
            snprintf(info.info, sizeof(info.info), "%s", krunker_str(krunker_at(t, 2)));
            emit(l, NET_PACKET_FLAG, &info, sizeof(info));
        }
    }
    const KrunkerValue *sync = arg(f, 11);
    const KrunkerValue *flg = krunker_get(sync, "flg");
    for (uint32_t i = 0; krunker_is(flg, KRUNKER_ARRAY) && i < flg->len && l->flag_count < MODE_MAX_FLAGS; i++) {
        const KrunkerValue *e = &flg->u.items[i];
        const int slot = flag_slot(l, (int) num(krunker_at(e, 0), -1 - (double) i));
        if (slot < 0) continue;
        flag_position(&l->flags[slot], krunker_at(e, 1), krunker_at(e, 2), krunker_at(e, 3));
        l->flags[slot].carrier = (int) num(krunker_at(e, 4), 0);
        emit_flag(l, slot);
    }
    // obj [uid, ms]: mode.objWep ? setWeapon(obj[0]) : setObjective(obj[0], obj[1], true) (:219395)
    const KrunkerValue *obj = krunker_get(sync, "obj");
    const GameModeConfig *mode = mode_config(l->mode);
    if (krunker_is(krunker_at(obj, 0), KRUNKER_NUM) && mode && mode->obj_weapon) {
        const NetObjectivePacket o = {.kind = NET_OBJECTIVE_WEAPON, .value = (int32_t) krunker_at(obj, 0)->u.num};
        emit(l, NET_PACKET_OBJECTIVE, &o, sizeof(o));
    } else if (krunker_is(krunker_at(obj, 0), KRUNKER_NUM)) {
        const NetObjectiveV7Packet o = {.uid = (int32_t) krunker_at(obj, 0)->u.num, .time_ms = (int32_t) num(krunker_at(obj, 1), 0), .initial = 1};
        emit(l, NET_PACKET_OBJECTIVE_V7, &o, sizeof(o));
    }
    zone_packet(l, krunker_get(sync, "zone"));
    krunker_link_world_init(l, f); // round wins / roundC / lockT, dest, mov, spwns, pkups, gates, banks, dyno, gbl, doms, zombies
    krunker_link_script_init(l, f);  // live blocks, entities and their attachments, moved objects (krunker_link_script.c)
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

// "t" UPDATE_TIMER [formatted, state, rawMs] (handler iìíîiïì game.deob.js:220234, server broadcast
// :191565 broadcast('t', formatTimer(gameTimer), 0, gameTimer)). arg0 is the already-formatted clock
// string, arg1 the state (0 normal countdown, 1 round-end "Next Round in", 2 match over, 3 abandoned),
// arg2 the raw gameTimer in ms. The HUD shows whole seconds (MM:SS): floor rawMs/1000 like formatTimer
// (:23893 uses ms/1000%60 with floored components) so the seconds match the string, and parse the
// "hh:mm:ss"/"mm:ss" string when rawMs is absent (e.g. START_GAME, which carries only the string).
static int timer_seconds(const KrunkerValue *raw, const KrunkerValue *str) {
    if (krunker_is(raw, KRUNKER_NUM) && isfinite(raw->u.num)) return (int) (raw->u.num / 1000.0);
    const char *s = krunker_str(str);
    if (!s) return 0;
    int total = 0, field = 0, digits = 0;
    for (; *s && *s != '.'; s++) { // stop before the showMS fraction (mm:ss.t)
        if (*s >= '0' && *s <= '9') {
            field = field * 10 + (*s - '0');
            digits = 1;
        } else if (*s == ':') {
            total = total * 60 + field;
            field = 0;
            digits = 0;
        }
    }
    return digits || total ? total * 60 + field : 0;
}

static void on_timer(KrunkerLink *l, const KrunkerFrame *f) {
    const int state = (int) num(arg(f, 1), 0);
    NetTimerPacket t;
    memset(&t, 0, sizeof(t));
    t.seconds = timer_seconds(arg(f, 2), arg(f, 0));
    // state 0 is the running round clock; 1/2/3 are the end screen's #endTimer (:220235): 1 "Next Round in"
    // + the time, 2 "MATCH OVER", 3 "MATCH ABANDONED" (the state goes first: the TIMER redraws #endTimer)
    t.end = state != 0 ? 1 : 0;
    if (state >= 1 && state <= 3) {
        const uint8_t st = (uint8_t) state;
        emit_hud_ext(l, NET_EXT_KN_OFFICIAL_CLOCK_STATE, &st, sizeof(st));
    }
    emit(l, NET_PACKET_TIMER, &t, sizeof(t));
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

// ---------------------------------------------------------------- movement locks / buffs (audit cluster A)

static void emit_lock(KrunkerLink *l, const NetLockKind kind, const int locked) {
    const NetLockPacket p = {.kind = (uint8_t) kind, .locked = (uint8_t) !!locked};
    emit(l, NET_PACKET_LOCK, &p, sizeof(p));
}

static void emit_ext(KrunkerLink *l, const uint16_t subtype, const void *body, const uint16_t length) {
    uint8_t buf[NET_MAX_PAYLOAD];
    const uint16_t n = net_ext_pack(buf, sizeof(buf), subtype, body, length);
    if (n) emit(l, NET_PACKET_EXT, buf, n);
}

// Our client resets game.moveLock and its players at INIT (game_init -> mode_round_start): so do the
// "q" / "l" shapes that follow them. The server repeats the lock in "start" (arg 4).
static void locks_reset(KrunkerLink *l) {
    l->move_lock = l->locked = l->noclip = 0;
}

// The "q" encoder forgets what it sent: the delta bases and the previous angles. game.deob.js
// startGame (:218971-218974) clears them (iîîíîîi = {}, iîiìîií = []) and sets resync 0; SYNC_FAILED
// (:220333) clears them too and sets resync 1. The next window then carries every field and absolute
// angles, so the server takes its state from that window and not from what it still holds from the
// last life: a window the server dropped while we were dead (io_q) was never decoded on its side.
static void input_encoder_forget(KrunkerLink *l) {
    memset(l->last_set, 0, sizeof(l->last_set));
    l->prev_angle_count = 0;
}

// "start" [timer, team, spectating, challMode, moveLock, ...] (iìïìiìi game.deob.js:218939, sent
// :208260): moveLock != null -> lockMove() / unlockMove() (no position), then iíiiiïî(false): unlocked.
static void on_start_locks(KrunkerLink *l, const KrunkerFrame *f) {
    if (!is_nil(arg(f, 4))) {
        l->move_lock = truthy(arg(f, 4));
        emit_lock(l, NET_LOCK_MOVE, l->move_lock);
    }
    if (l->locked) {
        l->locked = 0;
        emit_lock(l, NET_LOCK_PLAYER, 0);
    }
}

// What game.moveLock does to one "q" frame (fields of input_fields): moveDir -1 and no jump (:221748,
// Uiblms(..., moveLock) predicts the same). window.locked is not here: the live client builds no input
// frame at all while it is set (krunker_link_inputs_paused; our client samples none, net/recon.c), so
// nothing is sent and no isn is used up. Frames built before the "lock" reached us go out as built.
static void input_locks(const KrunkerLink *l, double v[15]) {
    if (l->move_lock) {
        v[4] = -1; // moveDir
        v[7] = 0;  // jump
    }
}

// "lm" LOCK_MOVE [x, y, z, xDire] (iîiîìïì :220428): game.lockMove(you, ...) -> moveLock = true and
// you.resetPosition(x, y, z, xDire) (:169438: y + 0.01, no velocity, on the ground). The position
// rides our io_tp ack (net_player_teleport: the same resets); xDire, the server's copy of our own
// yaw, is not applied (our look keeps the player's mouse).
static void on_lock_move(KrunkerLink *l, const KrunkerFrame *f) {
    l->move_lock = 1;
    emit_lock(l, NET_LOCK_MOVE, 1);
    if (l->my_sid < 0 || !krunker_is(arg(f, 0), KRUNKER_NUM) || !krunker_is(arg(f, 1), KRUNKER_NUM) || !krunker_is(arg(f, 2), KRUNKER_NUM)) return;
    const NetTeleportPacket tp = {.x = (float) arg(f, 0)->u.num, .y = (float) (arg(f, 1)->u.num + 0.01), .z = (float) arg(f, 2)->u.num};
    emit(l, NET_PACKET_TELEPORT_ACK, &tp, sizeof(tp));
}

// "ulm" UNLOCK_MOVE []: game.unlockMove -> moveLock = false
static void on_unlock_move(KrunkerLink *l) {
    l->move_lock = 0;
    emit_lock(l, NET_LOCK_MOVE, 0);
}

// "lock" LOCK_PLAYER [locked] (iíiiiïî :219705): window.locked + #blockerDiv
static void on_lock_player(KrunkerLink *l, const KrunkerFrame *f) {
    l->locked = truthy(arg(f, 0));
    emit_lock(l, NET_LOCK_PLAYER, l->locked);
}

// "frz" / "noc" / "unl" [on] (:220495 / :220488 / :220504): our NET_PACKET_CHEAT flags (noclip and
// unlimited ammo print Krunker's commands.* line there, freeze none)
static void on_cheat_flag(KrunkerLink *l, const KrunkerFrame *f, const NetCheatFlag flag) {
    const int on = truthy(arg(f, 0));
    if (flag == NET_CHEAT_NOCLIP) l->noclip = on;
    const NetCheatPacket c = {.flag = (uint8_t) flag, .on = (uint8_t) on};
    emit(l, NET_PACKET_CHEAT, &c, sizeof(c));
}

// "spb" SET_SPEED_BOOST [speedBoost, silent] (iìîïììî :220498; the server clamps it to 0.1..10, :180931)
static void on_speed_boost(KrunkerLink *l, const KrunkerFrame *f) {
    const double boost = num(arg(f, 0), 0);
    const NetSpeedBoostPacket b = {.boost = isfinite(boost) && boost > 0 ? (float) boost : 0.0f, .silent = (uint8_t) truthy(arg(f, 1))};
    emit_ext(l, NET_EXT_PLAYER_SPEED_BOOST, &b, sizeof(b));
}

// "sic" SET_SHARING_IS_CARING [on] (iiïîïíí :220511): half-price mystery boxes + its popup
static void on_sharing(KrunkerLink *l, const KrunkerFrame *f) {
    const NetSharingPacket s = {.on = (uint8_t) truthy(arg(f, 0))};
    emit(l, NET_PACKET_SHARING, &s, sizeof(s));
}

// "jg" SET_JUGGERNAUT [sid, on] (iíîiìîí :220482): isJuggernaut only; the server's "h" with
// resetMax carries the 2000 / class maximum
static void on_juggernaut(KrunkerLink *l, const KrunkerFrame *f) {
    if (!krunker_is(arg(f, 0), KRUNKER_NUM)) return;
    const NetJuggernautPacket j = {.uid = (int32_t) arg(f, 0)->u.num, .on = (uint8_t) truthy(arg(f, 1)), .max_health = 0};
    emit(l, NET_PACKET_JUGGERNAUT, &j, sizeof(j));
}

// "busy" SET_BUSY [sid, on] (iîíîìîï :220479 -> game.setBusy :188630): isBusy, which only draws a
// nametag icon in the live client; kept on the player
static void on_busy(KrunkerLink *l, const KrunkerFrame *f) {
    KrunkerLinkPlayer *p = player(l, (int) num(arg(f, 0), -1), 0);
    if (!p || p->busy == truthy(arg(f, 1))) return;
    p->busy = !p->busy;
    note(l, KRUNKER_LINK_EV_NOTE, "busy: sid %d %s", p->sid, p->busy ? "busy" : "back");
}

// "gsc" GAME_STATE_CHANGED [gameState] (iiíiîìí :219143): game.gameState (nothing in this client reads it)
static void on_game_state(KrunkerLink *l, const KrunkerFrame *f) {
    const int state = (int) num(arg(f, 0), -1);
    if (state == l->game_state) return;
    l->game_state = state;
    note(l, KRUNKER_LINK_EV_NOTE, "gsc: game state %d", state);
}

// "chrg" SET_PLAYER_CHARGED [sid, krankedTimer ms] (iïíïìïi :220033): isKranked, its timer
static void on_charged(KrunkerLink *l, const KrunkerFrame *f) {
    if (!krunker_is(arg(f, 0), KRUNKER_NUM)) return;
    const NetModeCounterPacket c = {.kind = NET_MODE_COUNTER_CHARGE, .uid = (int32_t) arg(f, 0)->u.num, .value = (float) num(arg(f, 1), 0)};
    emit(l, NET_PACKET_MODE_COUNTER, &c, sizeof(c));
}

// ---------------------------------------------------------------- player-state corrections (audit cluster B)

// "chgp" CHANGE_POSITION [id, x, y, z, doRecon, stopMomentum, dir] (iiíïiíi :220937): players.findById
// (the socket id of the player's "0", not its sid) -> changePosition (:173813): x y z (and old*),
// dir != null -> xDire = -(dir + 1) * PI / 2 (ours: the camera follows, so the next "q" carries it),
// stopMomentum -> velocities and stepVal 0. Our client's changePosition mirror (map_triggers.c) snaps
// it; for us the pending inputs stay (turned to the new yaw) and the next "l" replays them from the
// server's new state.
static void on_change_position(KrunkerLink *l, const KrunkerFrame *f) {
    char id[64];
    id_string(arg(f, 0), id, sizeof(id));
    const KrunkerLinkPlayer *p = NULL;
    for (int i = 0; id[0] && i < l->player_count && !p; i++) {
        if (!strcmp(l->players[i].id, id)) p = &l->players[i];
    }
    if (!p || !krunker_is(arg(f, 1), KRUNKER_NUM) || !krunker_is(arg(f, 2), KRUNKER_NUM) || !krunker_is(arg(f, 3), KRUNKER_NUM)) return;
    NetChangePositionPacket c;
    memset(&c, 0, sizeof(c));
    c.uid = p->sid;
    c.x = (float) arg(f, 1)->u.num;
    c.y = (float) arg(f, 2)->u.num;
    c.z = (float) arg(f, 3)->u.num;
    c.recon = (uint8_t) truthy(arg(f, 4));
    c.stop_momentum = (uint8_t) truthy(arg(f, 5));
    c.has_direction = (uint8_t) !is_nil(arg(f, 6));
    c.direction = (int32_t) num(arg(f, 6), 0); // (dir || 0)
    emit(l, NET_PACKET_CHANGE_POSITION, &c, sizeof(c));
}

// "tm" UPDATE_TEAM [sid, team] (iiíïiîï :220422): findBySid -> team (ours: the HUD team name follows)
static void on_update_team(KrunkerLink *l, const KrunkerFrame *f) {
    KrunkerLinkPlayer *p = player(l, (int) num(arg(f, 0), -1), 0);
    if (!p || !krunker_is(arg(f, 1), KRUNKER_NUM)) return;
    p->team = (int) arg(f, 1)->u.num;
    const NetTeamPacket t = {.uid = p->sid, .team = p->team, .health = p->health, .max_health = p->max_health};
    emit(l, NET_PACKET_TEAM, &t, sizeof(t));
}

// "spk" SET_PLAYER_KEY [key, value] (iíîíîîï :220514): you[key] = value, unchecked. The keys this
// client models go through their own packets; the rest are noted. (This build's server sends none.)
static void on_set_player_key(KrunkerLink *l, const KrunkerFrame *f) {
    const char *key = krunker_str(arg(f, 0));
    const KrunkerValue *v = arg(f, 1);
    if (!key || l->my_sid < 0) return;
    KrunkerLinkPlayer *me = player(l, l->my_sid, 0);
    if (!strcmp(key, "noclip") || !strcmp(key, "unlimitedAmmo")) {
        const int on = truthy(v);
        if (key[0] == 'n') l->noclip = on;
        const NetCheatPacket c = {.flag = key[0] == 'n' ? NET_CHEAT_NOCLIP : NET_CHEAT_UNLIMITED_AMMO, .on = (uint8_t) on};
        emit(l, NET_PACKET_CHEAT, &c, sizeof(c));
    } else if (!strcmp(key, "speedBoost")) {
        const double boost = num(v, 0);
        const NetSpeedBoostPacket b = {.boost = isfinite(boost) && boost > 0 ? (float) boost : 0.0f, .silent = 1};
        emit_ext(l, NET_EXT_PLAYER_SPEED_BOOST, &b, sizeof(b));
    } else if (!strcmp(key, "canThrow") && truthy(v)) {
        emit(l, NET_PACKET_REFILL, NULL, 0);
    } else if ((!strcmp(key, "health") || !strcmp(key, "team")) && krunker_is(v, KRUNKER_NUM) && me) {
        if (key[0] == 'h') me->health = (float) v->u.num;
        else me->team = (int) v->u.num;
        const NetTeamPacket t = {.uid = me->sid, .team = me->team, .health = me->health, .max_health = me->max_health};
        emit(l, NET_PACKET_TEAM, &t, sizeof(t));
    } else {
        note(l, KRUNKER_LINK_EV_NOTE, "spk %s: a player key this client does not model", key);
    }
}

// "wep" SET_WEAPON [weaponId] (game.setWeapon :189457): activeObjective = the Sharp Shooter weapon;
// the loadouts themselves arrive in "inatA"
static void on_set_weapon(KrunkerLink *l, const KrunkerFrame *f) {
    if (!krunker_is(arg(f, 0), KRUNKER_NUM)) return;
    const NetObjectivePacket o = {.kind = NET_OBJECTIVE_WEAPON, .value = (int32_t) arg(f, 0)->u.num};
    emit(l, NET_PACKET_OBJECTIVE, &o, sizeof(o));
}

// "rg" REGEN_PLAYER [sid, changeClass, items] (iîiîïìi :220027 -> updateItems(game, null, changeClass,
// items, true), :169313). items = [skins, face, shoe, hat, head, body, back, waist, melee, skinCol,
// hairCol, dye, pcStat, attach, secIndex (14), classIndex (15), pet, wrist, playerCard]. Without
// changeClass only the cosmetics change (a guest client shows none). With it, setClass (:169388):
// the class loadout [primary, secondary if the class has one, knife], full ammo, health = maxHealth
// = the class health; ours: the LOADOUT and TEAM our server sends for a mid-life class change. Not
// modelled: the class's speed / wall-jump stats on our player (no packet carries a class index).
static void on_regen(KrunkerLink *l, const KrunkerFrame *f) {
    KrunkerLinkPlayer *p = player(l, (int) num(arg(f, 0), -1), 0);
    const KrunkerValue *items = arg(f, 2);
    if (!p || !truthy(arg(f, 1)) || !krunker_is(items, KRUNKER_ARRAY)) return;
    const int ci = (int) num(krunker_at(items, 15), -1), sec = (int) num(krunker_at(items, 14), -1);
    if (ci < 0 || (size_t) ci >= rules_class_count()) return;
    const ClassConfig *c = &rules_classes()[ci];
    p->class_index = ci;
    NetLoadoutPacket lo;
    memset(&lo, 0, sizeof(lo));
    lo.uid = p->sid;
    lo.weapons[lo.count++] = c->loadout ? c->loadout[0] : 1;
    if (c->secondary) { // secIndex when it names a secondary weapon, else 2 (the Pistol) (:169383)
        const Weapon *sw = rules_weapon(sec);
        lo.weapons[lo.count++] = sw && sw->secondary ? sec : WEAPON_ID_PISTOL;
    }
    int has_knife = 0;
    for (int i = 0; i < lo.count; i++) has_knife |= lo.weapons[i] == WEAPON_ID_KNIFE;
    if (!has_knife && lo.count < NET_LOADOUT_MAX) lo.weapons[lo.count++] = WEAPON_ID_KNIFE;
    for (int i = 0; i < lo.count; i++) {
        const Weapon *w = rules_weapon(lo.weapons[i]);
        lo.ammo[i] = w ? (int32_t) w->ammo : 0;
    }
    emit(l, NET_PACKET_LOADOUT, &lo, sizeof(lo));
    p->max_health = c->health > 0 ? c->health : p->max_health;
    p->health = (float) p->max_health;
    const NetTeamPacket t = {.uid = p->sid, .team = p->team, .health = p->health, .max_health = p->max_health};
    emit(l, NET_PACKET_TEAM, &t, sizeof(t));
}

// "00" UPDATE_NAMES [[id, name, alias, ...], keepOpen] (iíîîîíi :220322): per (socket) id the name and
// alias (what is shown: alias || name); keepOpen falsy -> socket.close(): the server ends our session
static void on_update_names(KrunkerLink *l, const KrunkerFrame *f) {
    const KrunkerValue *list = arg(f, 0);
    for (uint32_t i = 0; krunker_is(list, KRUNKER_ARRAY) && i + 1 < list->len; i += 3) {
        char id[64];
        id_string(&list->u.items[i], id, sizeof(id));
        const char *alias = i + 2 < list->len ? krunker_str(&list->u.items[i + 2]) : NULL;
        const char *name = alias && alias[0] ? alias : krunker_str(&list->u.items[i + 1]); // alias || name, as "0"
        for (int k = 0; id[0] && name && k < l->player_count; k++) {
            if (strcmp(l->players[k].id, id) != 0) continue;
            const char *real = krunker_str(&list->u.items[i + 1]);
            if (real && real[0]) snprintf(l->players[k].name, sizeof(l->players[k].name), "%s", real);
            NetPlayerNamePacket n;
            memset(&n, 0, sizeof(n));
            n.uid = l->players[k].sid;
            snprintf(n.name, sizeof(n.name), "%s", name);
            emit_ext(l, NET_EXT_PLAYER_NAME, &n, sizeof(n));
        }
    }
    if (!truthy(arg(f, 1))) note(l, KRUNKER_LINK_EV_CLOSE, "the server ended the session (UPDATE_NAMES without keepOpen)");
}

// ---------------------------------------------------------------- combat visuals / sounds (fx)

// A sound as "s" / "so" / "sso" / "asnd" carry it (game.playerSound: `isAsset || !isFinite(i) ? i :
// sounds[i]`): an index into the official sound-name table (krunker_sounds.h), a name, or with
// isAsset a user asset id ("asset:<id>", audio.c). 0: nothing to play (unknown index, a name our
// NetSoundPacket cannot hold: truncated it would name another file).
static int fx_sound_name(const KrunkerValue *v, const int asset, char out[NET_SOUND_NAME_SIZE]) {
    char id[64];
    out[0] = 0;
    if (asset) {
        id_string(v, id, sizeof(id));
        if (!id[0]) return 0;
        return snprintf(out, NET_SOUND_NAME_SIZE, "asset:%s", id) < NET_SOUND_NAME_SIZE;
    }
    const char *name = krunker_is(v, KRUNKER_NUM) && isfinite(v->u.num) ? krunker_sound_name((int) v->u.num) : krunker_str(v);
    if (!name || !name[0] || strlen(name) >= NET_SOUND_NAME_SIZE) return 0;
    snprintf(out, NET_SOUND_NAME_SIZE, "%s", name);
    return 1;
}

// JS `a || 1` of a volume / rate
static float fx_or_one(const KrunkerValue *v) {
    const double x = num(v, 0);
    return x != 0 && isfinite(x) ? (float) x : 1.0f;
}

// "9" SHOW_TRACER [[sid, x, y, z, impYaw, impPitch, trail, sx, sy, sh, sz, bulletT, impactT]] (iíïîîîi
// :220792): the shooter's tracer to the hit and, when [4] is set (a map object was hit), the impact.
// The shot's sound is its own "s" (shoot -> playSound), so first stays 0 (our client would add a
// second gun sound); the weapon is the shooter's current one on the client (-1). [7..10] (the origin
// before the shooter's first "k") and [11] / [12] (skin tracer / impact types) are not mapped.
static void on_tracer(KrunkerLink *l, const KrunkerFrame *f) {
    const KrunkerValue *a = arg(f, 0);
    if (!krunker_is(a, KRUNKER_ARRAY) || a->len < 4 || !krunker_is(krunker_at(a, 0), KRUNKER_NUM)) return;
    NetShotPacket s;
    memset(&s, 0, sizeof(s));
    s.uid = (int32_t) krunker_at(a, 0)->u.num;
    s.weapon_index = -1;
    for (int k = 0; k < 3; k++) s.hit[k] = (float) num(krunker_at(a, (uint32_t) (1 + k)), 0);
    s.has_normal = !is_nil(krunker_at(a, 4)) ? 1 : 0;
    s.normal_yaw = (float) num(krunker_at(a, 4), 0);
    s.normal_pitch = (float) num(krunker_at(a, 5), 0);
    s.trail = (uint8_t) num(krunker_at(a, 6), 0);
    s.first = 0;
    emit(l, NET_PACKET_SHOT, &s, sizeof(s));
}

// "s" PLAYER_SOUND [sound, sid, x, y, z, vol, rate, dist, roll, forcePos, isAsset] (iîîîïìï :220021 ->
// game.playerSound :187796): play3D at the player (unknown: nothing) or, forcePos, at x y z
static void on_player_sound(KrunkerLink *l, const KrunkerFrame *f) {
    NetSoundPacket s;
    memset(&s, 0, sizeof(s));
    if (!fx_sound_name(arg(f, 0), truthy(arg(f, 10)), s.name)) return;
    s.uid = truthy(arg(f, 9)) ? -1 : (int32_t) num(arg(f, 1), -1);
    for (int k = 0; k < 3; k++) s.position[k] = (float) num(arg(f, (uint32_t) (2 + k)), 0);
    s.volume = fx_or_one(arg(f, 5));
    s.rate = fx_or_one(arg(f, 6));
    emit(l, NET_PACKET_SOUND, &s, sizeof(s));
}

// "so" PLAY_SOUND [sound, vol, mode, x, y, z, rate, dist, roll, isAsset] (iíîiíîí :220017): mode 1 a
// 2D play (map_triggers.c play_sound: uid -2), else play3D at x y z
static void on_play_sound(KrunkerLink *l, const KrunkerFrame *f) {
    NetSoundPacket s;
    memset(&s, 0, sizeof(s));
    if (!fx_sound_name(arg(f, 0), truthy(arg(f, 9)), s.name)) return;
    s.uid = num(arg(f, 2), 0) == 1 ? -2 : -1;
    for (int k = 0; k < 3; k++) s.position[k] = (float) num(arg(f, (uint32_t) (3 + k)), 0);
    s.volume = fx_or_one(arg(f, 1));
    s.rate = fx_or_one(arg(f, 6));
    emit(l, NET_PACKET_SOUND, &s, sizeof(s));
}

// "sso" STOP_SOUND [sound, isAsset] (iíîíîíî :220013)
static void on_stop_sound(KrunkerLink *l, const KrunkerFrame *f) {
    NetStopSoundPacket s;
    memset(&s, 0, sizeof(s));
    const int asset = truthy(arg(f, 1));
    if (asset) id_string(arg(f, 0), s.name, sizeof(s.name)); // the client stops no asset sounds (its own rule)
    else if (!fx_sound_name(arg(f, 0), 0, s.name)) return;
    s.asset = (uint8_t) asset;
    emit(l, NET_PACKET_STOP_SOUND, &s, sizeof(s));
}

// Our projectile slot of the server's projectile sid: the slot already holding it, else (create)
// a free one, else the next one round-robin (a projectile that left its range without a "pre").
static int fx_proj_slot(KrunkerLink *l, const int sid, const int create) {
    const int n = (int) (sizeof(l->proj_sid) / sizeof(l->proj_sid[0]));
    for (int i = 0; i < n; i++) {
        if (l->proj_sid[i] == sid + 1) return i;
    }
    if (!create) return -1;
    for (int i = 0; i < n; i++) {
        if (!l->proj_sid[i]) return l->proj_sid[i] = sid + 1, i;
    }
    const int i = l->proj_next;
    l->proj_next = (l->proj_next + 1) % n;
    l->proj_sid[i] = sid + 1;
    return i;
}

// Bombardment bombs (v10 stored[2], :220832 "bmb" and :220839 "pr" with isStored): the streak_3 model,
// spd 0.4 times the bomb's pjSp, no gravity, range 1500. A bmb drops it straight down (yaw 0, pitch
// -PI / 2); its visual (noLookAt, spin about Y) is projectiles_render.c's.
#define BOMB_SPEED 0.4f
#define BOMB_RANGE 1500.0f
#define BOMB_STRIDE 5 // "bmb" list: x, y, z, the server's projectile sid, pjSp per bomb

// A bomb in flight: the server's projectile sid gets its slot, the velocity as Projectile.init (:180002).
static void emit_bomb(KrunkerLink *l, const double pos[3], const double yaw, const double pitch, const double pj_speed, const int sid,
                      const int owner) {
    NetProjectilePacket p;
    memset(&p, 0, sizeof(p));
    const float speed = (float) (BOMB_SPEED * pj_speed);
    const double d = yaw - M_PI;
    p.id = fx_proj_slot(l, sid, 1);
    p.owner_uid = owner;
    p.weapon_index = -1;
    p.bomb = 1;
    for (int k = 0; k < 3; k++) p.position[k] = (float) pos[k];
    p.velocity[0] = (float) (speed * sin(d) * cos(pitch));
    p.velocity[1] = (float) (speed * sin(pitch));
    p.velocity[2] = (float) (speed * cos(d) * cos(pitch));
    p.range = BOMB_RANGE;
    emit(l, NET_PACKET_PROJECTILE, &p, sizeof(p));
}

// "pr" SHOW_PROJECTILE [x, y, z, yaw, pitch, index, sid, isStored, customProj, owner] (iìíîíìî
// :220839): physObj(x, y, z, yaw, pitch, cfg.range, .., cfg, sid) with cfg = weaponConfig[index]
// .projectile, or projectiles.stored[index] (0 the thrown knife, 1 an AI's orb, 2 a bombardment
// bomb). Velocity as Projectile.init (:180002, shared/src/projectile.c): d = yaw - PI, spd * (sin d
// cos pitch, sin pitch, cos d cos pitch).
static void on_projectile(KrunkerLink *l, const KrunkerFrame *f) {
    if (!krunker_is(arg(f, 6), KRUNKER_NUM)) return;
    const int index = (int) num(arg(f, 5), -1);
    NetProjectilePacket p;
    memset(&p, 0, sizeof(p));
    float speed = 0, gravity = 0, range = 0;
    if (truthy(arg(f, 7))) {
        if (index == 0) { // stored[0]: the thrown knife (spd 0.38, grav 0.0002, range 1500)
            p.weapon_index = WEAPON_ID_KNIFE;
            speed = 0.38f, gravity = 0.0002f, range = 1500.0f;
        } else if (index == 1) { // stored[1]: an AI's orb (spd 0.1, grav 0, range 500)
            p.weapon_index = -1;
            p.ai_kind = 1;
            speed = 0.1f, range = 500.0f;
        } else if (index == 2) { // stored[2]: a bombardment bomb, its pjSp from customProj (1 when absent)
            const KrunkerValue *pj = krunker_get(arg(f, 8), "pjSp");
            const double pos[3] = {num(arg(f, 0), 0), num(arg(f, 1), 0), num(arg(f, 2), 0)};
            emit_bomb(l, pos, num(arg(f, 3), 0), num(arg(f, 4), 0), num(pj, 1), (int) arg(f, 6)->u.num, (int) num(arg(f, 9), -1));
            return;
        } else {
            return;
        }
    } else {
        const Weapon *w = index >= 0 ? rules_weapon(index) : NULL;
        if (!w || w->projectile_config.speed <= 0.0f) return;
        p.weapon_index = index;
        speed = w->projectile_config.speed, gravity = w->projectile_config.gravity, range = w->projectile_config.range;
    }
    p.id = fx_proj_slot(l, (int) arg(f, 6)->u.num, 1);
    p.owner_uid = (int32_t) num(arg(f, 9), -1);
    for (int k = 0; k < 3; k++) p.position[k] = (float) num(arg(f, (uint32_t) k), 0);
    const double d = num(arg(f, 3), 0) - M_PI, pitch = num(arg(f, 4), 0);
    p.velocity[0] = (float) (speed * sin(d) * cos(pitch));
    p.velocity[1] = (float) (speed * sin(pitch));
    p.velocity[2] = (float) (speed * cos(d) * cos(pitch));
    p.gravity = gravity;
    p.range = range;
    emit(l, NET_PACKET_PROJECTILE, &p, sizeof(p));
}

// "bmb" SHOW_BOMBARDMENT [bombs] (:220832): the handler's list, BOMB_STRIDE values a bomb. Each one is
// physObj(x, y, z, 0, yD -PI / 2, range, trails, .., stored[2], sid, .., {pjSp}): a bomb falling from the
// bomber's drop height with no owner; the server's "pre" ends it. The list's position is the build's
// role (absent: no bombs drawn).
static void on_bombardment(KrunkerLink *l, const KrunkerFrame *f) {
    const KrunkerValue *list = role(f, krunker_build()->bomb, KRB_BOMB_LIST);
    for (uint32_t i = 0; krunker_is(list, KRUNKER_ARRAY) && i + BOMB_STRIDE <= list->len; i += BOMB_STRIDE) {
        const KrunkerValue *sid = krunker_at(list, i + 3);
        if (!krunker_is(sid, KRUNKER_NUM)) continue;
        const double pos[3] = {num(krunker_at(list, i), 0), num(krunker_at(list, i + 1), 0), num(krunker_at(list, i + 2), 0)};
        emit_bomb(l, pos, 0.0, -M_PI / 2, num(krunker_at(list, i + 4), 1), (int) sid->u.num, -1);
    }
}

// "pre" END_PROJECTILE [sid | ownShotCnt, isOwn, x, y, z, impact] (iiiîïîî :220828): disablePhys
// (a knife lingers) + effect on an impact. isOwn: our own predicted projectile, which never had a
// "pr", named by our shotCntP (NET_PROJECTILE_END_OWN). An unknown sid still shows the impact (id -1).
static void on_projectile_end(KrunkerLink *l, const KrunkerFrame *f) {
    NetProjectileEndPacket e;
    memset(&e, 0, sizeof(e));
    const int n = (int) num(arg(f, 0), -1);
    if (truthy(arg(f, 1))) {
        if (n < 0) return;
        e.id = NET_PROJECTILE_END_OWN - n;
    } else {
        e.id = n >= 0 ? fx_proj_slot(l, n, 0) : -1;
        if (e.id >= 0) l->proj_sid[e.id] = 0;
    }
    for (int k = 0; k < 3; k++) e.position[k] = (float) num(arg(f, (uint32_t) (2 + k)), 0);
    e.impact = (uint8_t) truthy(arg(f, 5));
    emit(l, NET_PACKET_PROJECTILE_END, &e, sizeof(e));
}

// "ex" EXPLOSION [x, y, z, radius, smCnt, lgCnt] (iïïííìî :220848): shake, "explosion", explodeAt.
// smCnt / lgCnt (debris counts) are not mapped: our explosion picks its own.
static void on_explosion(KrunkerLink *l, const KrunkerFrame *f) {
    NetExplosionPacket e;
    memset(&e, 0, sizeof(e));
    for (int k = 0; k < 3; k++) e.position[k] = (float) num(arg(f, (uint32_t) k), 0);
    e.radius = (float) num(arg(f, 3), 0);
    emit(l, NET_PACKET_EXPLOSION, &e, sizeof(e));
}

// "k" [11] grapple: [x, y, z] while another player is hooked, null otherwise (players.js sync); our
// client draws the rope from NET_PACKET_GRAPPLE, sent when it changes
static void fx_grapple(KrunkerLink *l, const int sid, const KrunkerValue *g) {
    KrunkerLinkPlayer *p = player(l, sid, 0);
    if (!p) return;
    const int on = krunker_is(g, KRUNKER_ARRAY) && g->len >= 3;
    float point[3] = {0, 0, 0};
    for (int k = 0; on && k < 3; k++) point[k] = (float) num(krunker_at(g, (uint32_t) k), 0);
    if (on == p->grapple && (!on || !memcmp(point, p->grapple_point, sizeof(point)))) return;
    p->grapple = on;
    memcpy(p->grapple_point, point, sizeof(point));
    NetGrapplePacket gp;
    memset(&gp, 0, sizeof(gp));
    gp.uid = sid;
    gp.active = (uint8_t) on;
    memcpy(gp.point, point, sizeof(point));
    emit(l, NET_PACKET_GRAPPLE, &gp, sizeof(gp));
}

// "pk" EFFECT [index, count] (iîïìîìî :220696): our player's effect stacks (#perkHolder)
static void on_effect(KrunkerLink *l, const KrunkerFrame *f) {
    const int index = (int) num(arg(f, 0), -1);
    if (index < 0 || index > 255) return;
    const NetEffectPacket e = {.effect = (uint8_t) index, .count = (uint8_t) num(arg(f, 1), 0)};
    emit(l, NET_PACKET_EFFECT, &e, sizeof(e));
}

// "upk" UPDATE_ZOMBIE_PERKS [sid, perks] (iîïïíìî :220688); the 250 maxHealth comes as its own "h"
static void on_zombie_perks(KrunkerLink *l, const KrunkerFrame *f) {
    if (!krunker_is(arg(f, 0), KRUNKER_NUM)) return;
    NetPerksPacket p;
    memset(&p, 0, sizeof(p));
    p.uid = (int32_t) arg(f, 0)->u.num;
    const KrunkerValue *list = arg(f, 1);
    for (uint32_t i = 0; krunker_is(list, KRUNKER_ARRAY) && i < list->len && p.count < PLAYER_PERK_MAX; i++)
        p.perks[p.count++] = (int8_t) num(krunker_at(list, i), -1);
    emit(l, NET_PACKET_PERKS, &p, sizeof(p));
}

// "bh" UPDATE_HEAD_SIZE [sid, headMlt, ..] (iïîiíiï :221034): Big Head's head scale
static void on_head_size(KrunkerLink *l, const KrunkerFrame *f) {
    if (!krunker_is(arg(f, 0), KRUNKER_NUM)) return;
    const NetModeCounterPacket c = {.kind = NET_MODE_COUNTER_HEAD, .uid = (int32_t) arg(f, 0)->u.num, .value = (float) num(arg(f, 1), 1)};
    emit(l, NET_PACKET_MODE_COUNTER, &c, sizeof(c));
}

// "asnd" ADD_PLAYER_SOUND [sid, sound, {vol, rate, dist, roll, loop}] (game.addPlayerSound :190261) and
// "rsnd" REMOVE_PLAYER_SOUND [sid, sound | null, force] (:190275). The Juggernaut's jug_0 loop is
// skipped: our NET_PACKET_JUGGERNAUT ("jg") already plays it, it would play twice.
static void on_player_sound_loop(KrunkerLink *l, const KrunkerFrame *f, const int add) {
    NetPlayerSoundPacket s;
    memset(&s, 0, sizeof(s));
    if (!fx_sound_name(arg(f, 1), 0, s.name) && (add || !is_nil(arg(f, 1)))) return;
    if (!strcmp(s.name, "jug_0")) return;
    s.uid = (int32_t) num(arg(f, 0), -1);
    s.add = (uint8_t) add;
    if (add) {
        const KrunkerValue *d = arg(f, 2);
        s.loop = (uint8_t) truthy(krunker_get(d, "loop"));
        s.volume = fx_or_one(krunker_get(d, "vol"));
        s.rate = fx_or_one(krunker_get(d, "rate"));
    } else {
        s.force = (uint8_t) (truthy(arg(f, 2)) || !s.name[0]);
    }
    emit(l, NET_PACKET_PLAYER_SOUND, &s, sizeof(s));
}

// "camb" CHANGE_AMBIENT [soundscape, custom, fade] (window.changeAmbient :220206): config.soundScapes
// value; a custom soundscape (asset) and the fade are not supported by our client
static void on_ambient(KrunkerLink *l, const KrunkerFrame *f) {
    if (truthy(arg(f, 1))) return;
    char text[32];
    id_string(arg(f, 0), text, sizeof(text));
    if (!text[0]) return;
    const NetAmbientPacket a = {.ambient = (int32_t) strtol(text, NULL, 10)}; // parseInt
    emit(l, NET_PACKET_AMBIENT_CHANGE, &a, sizeof(a));
}

// "ana" SERV_ANIM [uid | sid, clip, raw, isObj] (iîiiììì :220526): a map object's (isObj) or an AI's
// clip, by name when raw, else an index into config.clipNames
static void on_serv_anim(KrunkerLink *l, const KrunkerFrame *f) {
    static const char *const clip_names[] = {"Idle", "Move", "MoveFast", "Jump", "Melee", "Shoot", "Crouch"};
    if (!krunker_is(arg(f, 0), KRUNKER_NUM)) return;
    const int raw = truthy(arg(f, 2));
    char clip[GAME_AI_ANIM_SIZE] = "";
    if (raw) {
        id_string(arg(f, 1), clip, sizeof(clip));
    } else {
        const int i = (int) num(arg(f, 1), -1);
        if (i >= 0 && i < (int) (sizeof(clip_names) / sizeof(clip_names[0]))) snprintf(clip, sizeof(clip), "%s", clip_names[i]);
    }
    if (!clip[0]) return;
    if (truthy(arg(f, 3))) {
        NetObjectAnimPacket a;
        memset(&a, 0, sizeof(a));
        a.uid = (int32_t) arg(f, 0)->u.num;
        a.is_object = 1;
        a.raw = (uint8_t) raw;
        snprintf(a.clip, sizeof(a.clip), "%s", clip);
        emit(l, NET_PACKET_OBJECT_ANIM, &a, sizeof(a));
    } else {
        NetAIAnimPacket a;
        memset(&a, 0, sizeof(a));
        a.sid = (int32_t) arg(f, 0)->u.num;
        a.raw = (uint8_t) raw;
        snprintf(a.clip, sizeof(a.clip), "%s", clip);
        emit(l, NET_PACKET_AI_ANIM, &a, sizeof(a));
    }
}

// The fx packets without arguments worth a function of their own. 1 = handled.
static int on_fx(KrunkerLink *l, const KrunkerS2C id, const KrunkerFrame *f) {
    switch (id) {
        case KRUNKER_S2C_SHOW_TRACER: on_tracer(l, f); return 1;
        case KRUNKER_S2C_PLAYER_SOUND: on_player_sound(l, f); return 1;
        case KRUNKER_S2C_PLAY_SOUND: on_play_sound(l, f); return 1;
        case KRUNKER_S2C_STOP_SOUND: on_stop_sound(l, f); return 1;
        case KRUNKER_S2C_SHOW_PROJECTILE: on_projectile(l, f); return 1;
        case KRUNKER_S2C_SHOW_BOMBARDMENT: on_bombardment(l, f); return 1;
        case KRUNKER_S2C_END_PROJECTILE: on_projectile_end(l, f); return 1;
        case KRUNKER_S2C_EXPLOSION: on_explosion(l, f); return 1;
        case KRUNKER_S2C_EFFECT: on_effect(l, f); return 1;
        case KRUNKER_S2C_UPDATE_ZOMBIE_PERKS: on_zombie_perks(l, f); return 1;
        case KRUNKER_S2C_UPDATE_HEAD_SIZE: on_head_size(l, f); return 1;
        case KRUNKER_S2C_ADD_PLAYER_SOUND: on_player_sound_loop(l, f, 1); return 1;
        case KRUNKER_S2C_REMOVE_PLAYER_SOUND: on_player_sound_loop(l, f, 0); return 1;
        case KRUNKER_S2C_CHANGE_AMBIENT: on_ambient(l, f); return 1;
        case KRUNKER_S2C_SERV_ANIM: on_serv_anim(l, f); return 1;
        case KRUNKER_S2C_NUKE_UPDATE: { // "n" [0 siren | 1 detonated] (iíííiíì :220228)
            const NetNukePacket n = {.state = (uint8_t) num(arg(f, 0), 0)};
            emit(l, NET_PACKET_NUKE, &n, sizeof(n));
            return 1;
        }
        case KRUNKER_S2C_BOMBARDMENT_UPDATE: { // "bbm" [state] (iííiíïí :220231): 0 plays adrop_0, 1 nothing
            if (num(arg(f, 0), 1) != 0) return 1;
            NetSoundPacket s = {.name = "adrop_0", .uid = -2, .volume = 1.0f, .rate = 1.0f};
            emit(l, NET_PACKET_SOUND, &s, sizeof(s));
            return 1;
        }
        case KRUNKER_S2C_TRIGGER_IMG_SOUND: { // "is" [sound, image] (game.triggerImgSound :187805): the 2D sound
            NetSoundPacket s;                  // (the #imgFlash image is not shown)
            memset(&s, 0, sizeof(s));
            if (!fx_sound_name(arg(f, 0), 0, s.name)) return 1;
            s.uid = -2, s.volume = 1.0f, s.rate = 1.0f;
            emit(l, NET_PACKET_SOUND, &s, sizeof(s));
            return 1;
        }
        case KRUNKER_S2C_JUMP_SCARE: { // "scr" [index] (window.jumpScare :222997)
            const NetJumpScarePacket j = {.index = (int32_t) num(arg(f, 0), 0)};
            emit(l, NET_PACKET_JUMP_SCARE, &j, sizeof(j));
            return 1;
        }
        case KRUNKER_S2C_SET_HIDDEN: on_cheat_flag(l, f, NET_CHEAT_FORCE_HIDE); return 1; // "fh" [on] (:220508)
        case KRUNKER_S2C_SHAKE: { // "shk" [amount] (renderer.shake)
            const NetOfficialShakePacket s = {.amount = (float) num(arg(f, 0), 0)};
            if (s.amount > 0) emit_ext(l, NET_EXT_KN_OFFICIAL_SHAKE, &s, sizeof(s));
            return 1;
        }
        default: return 0;
    }
}

// ---------------------------------------------------------------- player actions (inbound)

// Krunker's official map list by index (krunker_maps.h): a public mts option carries no name
static const char *official_map_name(const int index) {
    static const char *const names[] = {
#define X(i, name, file, objects) name,
        KRUNKER_OFFICIAL_MAPS(X)
#undef X
    };
    return index >= 0 && index < (int) (sizeof(names) / sizeof(names[0])) ? names[index] : NULL;
}

// endData.mts / "mv" [vo, mts] (updMatchVote :220107, the cards :219805-219826): mts is the option list,
// "modeIndex,mapIndex[,mapName,communityMapId]" each; vo {socketId: [option, weight]}: a card's total
// is the weights naming its option, ours is the option under our socket id. Our client's cards are
// keyed (mode, map); "maVote" sends the option's own string back (krunker_link_send_ours).
static void on_match_vote(KrunkerLink *l, const KrunkerValue *vo, const KrunkerValue *mts) {
    if (krunker_is(mts, KRUNKER_ARRAY)) {
        l->vote_count = 0;
        for (uint32_t i = 0; i < mts->len && l->vote_count < NET_MATCH_PICKS; i++) {
            char option[sizeof(l->vote_mts[0])];
            js_string(&mts->u.items[i], option, sizeof(option));
            int mode = -1, map = -1;
            if (sscanf(option, "%d,%d", &mode, &map) != 2) continue;
            snprintf(l->vote_mts[l->vote_count], sizeof(l->vote_mts[0]), "%s", option);
            l->vote_mode[l->vote_count] = mode;
            l->vote_map[l->vote_count] = map;
            l->vote_count++;
        }
    }
    NetMatchVoteStatePacket st;
    memset(&st, 0, sizeof(st));
    st.count = (uint8_t) l->vote_count;
    st.voted = -1;
    for (int i = 0; i < l->vote_count; i++) {
        NetMatchVoteEntry *e = &st.entries[i];
        e->mode_id = l->vote_mode[i];
        e->map_index = l->vote_map[i];
        // the third field (official games: iiIïîîì[2] || maps[i].name)
        const char *name = strchr(l->vote_mts[i], ',');
        name = name ? strchr(name + 1, ',') : NULL;
        if (name && name[1] && name[1] != ',') {
            const char *end = strchr(name + 1, ',');
            const int len = end ? (int) (end - name - 1) : (int) strlen(name + 1);
            snprintf(e->map_name, sizeof(e->map_name), "%.*s", len, name + 1);
        } else if (official_map_name(e->map_index)) {
            snprintf(e->map_name, sizeof(e->map_name), "%s", official_map_name(e->map_index));
        }
    }
    for (uint32_t k = 0; krunker_is(vo, KRUNKER_MAP) && k < vo->len; k++) {
        const KrunkerValue *key = &vo->u.items[2 * k], *v = &vo->u.items[2 * k + 1];
        if (!krunker_is(v, KRUNKER_ARRAY)) continue; // a falsy entry: no vote
        char option[sizeof(l->vote_mts[0])], who[64];
        js_string(krunker_at(v, 0), option, sizeof(option));
        id_string(key, who, sizeof(who));
        for (int i = 0; i < l->vote_count; i++) {
            if (strcmp(option, l->vote_mts[i]) != 0) continue;
            const double w = num(krunker_at(v, 1), 0);
            const double total = st.entries[i].votes + (w > 0 ? w : 0);
            st.entries[i].votes = (uint16_t) (total < 65535 ? total : 65535);
            if (l->socket_id[0] && !strcmp(who, l->socket_id)) st.voted = (int8_t) i;
        }
    }
    emit(l, NET_PACKET_MATCH_VOTE_STATE, &st, sizeof(st));
}

// "sp" ADD_SPRAY [id "<sid>_<n>", sprayIndex, x, y, z, rotY, rotX, rotZ, dyeIndex] (addSpray :189765; the
// server's broadcast :172286: rotations in degrees, positions round(2)): our v7 spray packet as is
static void on_add_spray(KrunkerLink *l, const KrunkerFrame *f) {
    char id[48];
    js_string(arg(f, 0), id, sizeof(id));
    int sid = -1;
    unsigned slot = 0;
    if (sscanf(id, "%d_%u", &sid, &slot) < 1 || sid < 0) return;
    NetSprayV7Packet s;
    memset(&s, 0, sizeof(s));
    s.uid = sid;
    s.slot = (uint16_t) slot;
    s.skin = (int32_t) num(arg(f, 1), SPRAY_DEFAULT_INDEX);
    for (int k = 0; k < 3; k++) s.position[k] = (float) num(arg(f, 2 + (uint32_t) k), 0);
    s.y_rot = (int16_t) js_round(num(arg(f, 5), 0));
    s.x_rot = (int16_t) js_round(num(arg(f, 6), 0));
    s.z_rot = (int16_t) js_round(num(arg(f, 7), 0));
    s.dye = (int32_t) num(arg(f, 8), -1);
    emit(l, NET_PACKET_SPRAY_V7, &s, sizeof(s));
}

// "mpos" MARK_POSITION [key, {x, y, z}] (:223423 -> addPingIcon :160954): a ping marker per key
static void on_mark_position(KrunkerLink *l, const KrunkerFrame *f) {
    const KrunkerValue *p = arg(f, 1);
    if (!krunker_is(p, KRUNKER_MAP)) return;
    NetMarkPositionPacket m;
    memset(&m, 0, sizeof(m));
    js_string(arg(f, 0), m.key, sizeof(m.key));
    m.x = (float) num(krunker_get(p, "x"), 0);
    m.y = (float) num(krunker_get(p, "y"), 0);
    m.z = (float) num(krunker_get(p, "z"), 0);
    emit_ext(l, NET_EXT_MARK_POSITION, &m, sizeof(m));
}

// ---------------------------------------------------------------- social / account lines

// "vf" VOTE_FORFEIT [voterAlias | null, votes, needed, isNew, initiatorAlias, updateOnly] (iíìiíìí
// :220002): v10 handles it only with an account signed in (iîïììîí, set by "a": our account_name)
static void on_vote_forfeit(KrunkerLink *l, const KrunkerFrame *f) {
    if (!l->account_name[0]) return;
    NetVoteForfeitStatePacket v;
    memset(&v, 0, sizeof(v));
    const char *voter = krunker_str(arg(f, 0));
    v.opened = truthy(arg(f, 3)) ? 1 : 0;
    v.update_only = truthy(arg(f, 5)) ? 1 : 0;
    v.active = (uint8_t) (v.opened || v.update_only || (voter && voter[0]));
    v.votes = (int32_t) num(arg(f, 1), 0);
    v.needed = (int32_t) num(arg(f, 2), 0);
    if (voter) snprintf(v.voter, sizeof(v.voter), "%s", voter);
    const char *initiator = krunker_str(arg(f, 4));
    if (initiator) snprintf(v.initiator, sizeof(v.initiator), "%s", initiator);
    emit_ext(l, NET_EXT_VOTE_FORFEIT_STATE, &v, sizeof(v));
}

// "prc" PLAYER_RECONNECTED [oldSocketId, newSocketId] (iììïïïì :220316): findById(old).id = new. Every
// later socket-id lookup ("chgp", "skinF", "uh", the vote's "vo") needs the new one.
static void on_player_reconnected(KrunkerLink *l, const KrunkerFrame *f) {
    char old_id[64], new_id[64];
    id_string(arg(f, 0), old_id, sizeof(old_id));
    id_string(arg(f, 1), new_id, sizeof(new_id));
    if (!old_id[0] || !new_id[0]) return;
    for (int i = 0; i < l->player_count; i++) {
        if (!strcmp(l->players[i].id, old_id)) {
            snprintf(l->players[i].id, sizeof(l->players[i].id), "%s", new_id);
            break;
        }
    }
    if (!strcmp(l->host_id, old_id)) snprintf(l->host_id, sizeof(l->host_id), "%s", new_id);
}

// "fe" FAILED_TO_ENDER [message] (iíìiîîì :208203): the server refused our "en": the menu comes back
// (showMenu, the "click to play" instructions) and the message is a red server line (kind 2, #ed4242)
static void on_failed_to_enter(KrunkerLink *l, const KrunkerFrame *f) {
    char body[NET_CHAT_TEXT_SIZE];
    const char *msg = krunker_str(arg(f, 0));
    snprintf(body, sizeof(body), "%s", msg ? msg : "");
    emit_ext(l, NET_EXT_KN_OFFICIAL_ENTER_FAILED, body, (uint16_t) (strlen(body) + 1));
}

// "skin" SKIN_REWARD [skinIndex, silent] (iíîìîîì :218208): we found an item (account.addSkin, the
// "Items Found" card, "You found [x]"); "skinF" SKIN_FOUND_MESSAGE [socketId, skinIndex] (iîïîîìï
// :217992): another player found one ("<alias> found [x]" with Show Findings on). Both are our
// season-system's NET_PACKET_SKIN_FOUND. Indexes are store.skins positions: the live 10.0.3 store
// table is the one v10.0.2's viewer_skin() table was built from; a newer item past it shows nothing.
static void on_skin_found(KrunkerLink *l, const int sid, const KrunkerValue *index) {
    if (sid < 0 || !krunker_is(index, KRUNKER_NUM) || index->u.num < 0) return;
    const NetSkinFoundPacket s = {.uid = sid, .skin = (int32_t) index->u.num};
    emit(l, NET_PACKET_SKIN_FOUND, &s, sizeof(s));
}

static void on_skin_found_message(KrunkerLink *l, const KrunkerFrame *f) {
    char id[64];
    id_string(arg(f, 0), id, sizeof(id));
    const int sid = sid_of_socket(l, id);
    // the finder's own line comes with its "skin" (our SKIN_FOUND for us shows the card + "You found")
    if (sid < 0 || sid == l->my_sid) return;
    on_skin_found(l, sid, arg(f, 1));
}

// ---------------------------------------------------------------- voice chat

static int voice_chunk_emit(void *user, const NetPacketType type, const void *payload, const uint16_t length) {
    emit(user, (uint8_t) type, payload, length);
    return 1;
}

// "vc" VOICE_CHAT [dataURL, alias, sid, channel, volume, isFinal, senderAccountId] (iîiïììî :209668):
// the clip goes to our client as the v7 relay our own servers send (voice.h: chunk id = the speaker's
// sid, "<channel> <volume> <isFinal> <accountId> <alias length>\n" + alias + data URL). The client
// plays it like v10: only on channel -1 or its own chat channel, volume > 0, not muted / blocked.
static void on_voice_chat(KrunkerLink *l, const KrunkerFrame *f) {
    const char *audio = krunker_str(arg(f, 0));
    if (!audio || strncmp(audio, VOICE_V7_DATA_PREFIX, strlen(VOICE_V7_DATA_PREFIX)) != 0) return;
    const double volume = num(arg(f, 4), 1);
    const VoiceV7Meta meta = {.channel = (int) num(arg(f, 3), -1), .volume = (float) (volume > 1 ? 1 : volume < 0 ? 0 : volume),
                              .is_final = truthy(arg(f, 5)), .account_id = (int) num(arg(f, 6), 0)};
    size_t len = 0;
    char *msg = voice_relay_v7_message(&meta, krunker_str(arg(f, 1)), audio, &len);
    if (msg && social_send_message(NET_PACKET_VOICE_V7, (uint32_t) (int32_t) num(arg(f, 2), -1), msg, len, voice_chunk_emit, l)) l->voice_in++;
    free(msg);
}

// Our NET_PACKET_VOICE_V7 chunks ("<channel> <volume> <quality>\n" + the data URL, voice.c send_clip):
// a whole clip is v10's reader "load": ["vc", dataURL, chatChannel, micVolume, parseInt(micQuality)]
// (:209711). Our recorder's data URL already carries v10's "data:audio/ogg; codecs=opus;base64," label.
static int send_voice(KrunkerLink *l, const void *payload, const uint16_t length) {
    char *msg = NULL;
    uint32_t id = 0;
    if (social_assembly_push(&l->voice_out, payload, length, &msg, &id) != 1) return 0;
    VoiceV7Send meta;
    const char *audio = NULL;
    int sent = 0;
    if (voice_parse_send_v7(msg, &meta, &audio)) {
        const KrunkerValue args[] = {KRUNKER_V_STR(audio), KRUNKER_V_NUM(meta.channel == CHAT_CHANNEL_TEAM ? 1 : 0),
                                     KRUNKER_V_NUM(meta.volume), KRUNKER_V_NUM(meta.quality)};
        sent = send_c2s(l, KRUNKER_C2S_VOICE_CHAT, args, 4);
        if (sent) l->voice_sent++;
    }
    free(msg);
    return sent;
}

// The account-scoped frames io.account_frame takes (KrunkerLinkIO), and how many arguments of each it
// passes on ("a": [error, id, username, data], never arg 4, the account's session token)
static int account_scoped(const KrunkerFrame *f, uint32_t *argc) {
    static const char *const names[] = {"a", "ua", "uf", "bpdl", "bppd", "chg", "uchp", "cln"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (strlen(names[i]) != f->name_len || memcmp(names[i], f->name, f->name_len)) continue;
        *argc = i == 0 && f->argc > 4 ? 4 : (uint32_t) f->argc;
        return 1;
    }
    return 0;
}

static void account_frame(KrunkerLink *l, const KrunkerFrame *f) {
    uint32_t argc = 0;
    if (!l->io.account_frame || !account_scoped(f, &argc)) return;
    KrunkerBuf args = {0};
    const KrunkerValue arr = KRUNKER_V_ARRAY(f->args, argc);
    if (krunker_value_to_json(&arr, &args) == KRUNKER_PROTO_OK && args.len >= 2) {
        // ["name", args...]: the name, then the args array without its brackets
        const size_t n = f->name_len + args.len + 8;
        char *json = malloc(n);
        if (json) {
            const int inner = (int) args.len - 2;
            const int w = snprintf(json, n, "[\"%.*s\"%s%.*s]", (int) f->name_len, f->name, inner > 0 ? "," : "", inner > 0 ? inner : 0,
                                   (const char *) args.data + 1);
            if (w > 0) l->io.account_frame(l->io.user, json, (size_t) w);
            free(json);
        }
    }
    krunker_buf_free(&args);
}

int krunker_link_on_frame(KrunkerLink *l, const uint8_t *data, const size_t len) {
    KrunkerFrame f;
    const int err = krunker_proto_decode(data, len, 0, &f);
    if (err != KRUNKER_PROTO_OK) {
        note(l, KRUNKER_LINK_EV_NOTE, "malformed frame (%zu bytes): %s", len, krunker_proto_strerror(err));
        return 0;
    }
    l->frames_in++;
    account_frame(l, &f);
    switch (krunker_s2c_lookup(f.name, f.name_len)) {
        case KRUNKER_S2C_IO_INIT:
            id_string(arg(&f, 0), l->socket_id, sizeof(l->socket_id));
            note(l, KRUNKER_LINK_EV_NOTE, "io-init: socket id %s", l->socket_id);
            break;
        case KRUNKER_S2C_INIT_LOAD: // iiïiîïí: the client answers "load" once it may load
            send_c2s(l, KRUNKER_C2S_LOAD, NULL, 0);
            break;
        case KRUNKER_S2C_INIT_GAME: on_init(l, &f); locks_reset(l); on_init_hud(l, &f); break;
        case KRUNKER_S2C_ENTER_GAME: note(l, KRUNKER_LINK_EV_NOTE, "frs: the server took our en"); break;
        case KRUNKER_S2C_START_GAME: { // [formattedTimer, ...] (iìïìiìi :218939 -> iìíîiïì(arg0) :218948):
            // team / class ride our SPAWN; arg0 seeds the round clock so the HUD shows it before the
            // first "t" arrives (the string only, no raw ms, like START_GAME's own timer call).
            // startGame's input reset (:218971-218974, resync 0): the first "q" of the life is a full state
            input_encoder_forget(l);
            l->resync = 0;
            const KrunkerValue *s = arg(&f, 0);
            if (krunker_is(s, KRUNKER_STR)) {
                const NetTimerPacket t = {.seconds = timer_seconds(NULL, s), .end = 0};
                emit(l, NET_PACKET_TIMER, &t, sizeof(t));
            }
            on_start_locks(l, &f);
            on_kpd_data(l, arg(&f, 6)); // iìIiîìí: kpdData {sus, caller} (socket ids) for a K.P.D officer
            { // [2] window.spectating (:218944): the client's free cam / follow cam, or back to playing
                const NetSpectatePacket sp = {.spectating = (uint8_t) truthy(arg(&f, 2))};
                emit(l, NET_PACKET_SPECTATE, &sp, sizeof(sp));
            }
            break;
        }
        case KRUNKER_S2C_ADD_PLAYERS: on_add_players(l, &f); break;
        case KRUNKER_S2C_UPDATE_PLAYERS: on_update_players(l, &f); break;
        case KRUNKER_S2C_SYNC_PLAYER: on_sync_player(l, &f); break;
        case KRUNKER_S2C_SYNC_FAILED: { // iiîïîìï: the server dropped our inputs; the next "q" restarts the
            // deltas, and tmpInpts (the pending prediction inputs) empties: none of them will be acked
            input_encoder_forget(l);
            l->resync = 1;
            l->sync_failed++;
            uint8_t ext[NET_EXT_HEADER_SIZE];
            const uint16_t n = net_ext_pack(ext, sizeof(ext), NET_EXT_KN_INPUTS_DROPPED, NULL, 0);
            if (n) emit(l, NET_PACKET_EXT, ext, n);
            note(l, KRUNKER_LINK_EV_NOTE, "input sync failed (%d): pending inputs dropped", (int) num(arg(&f, 0), -1));
            break;
        }
        // movement locks / buffs of our player (packet-audit cluster A)
        case KRUNKER_S2C_LOCK_MOVE: on_lock_move(l, &f); break;
        case KRUNKER_S2C_UNLOCK_MOVE: on_unlock_move(l); break;
        case KRUNKER_S2C_LOCK_PLAYER: on_lock_player(l, &f); break;
        case KRUNKER_S2C_SET_FROZEN: on_cheat_flag(l, &f, NET_CHEAT_FROZEN); break;
        case KRUNKER_S2C_SET_NOCLIP: on_cheat_flag(l, &f, NET_CHEAT_NOCLIP); break;
        case KRUNKER_S2C_SET_UNLIMITED: on_cheat_flag(l, &f, NET_CHEAT_UNLIMITED_AMMO); break;
        case KRUNKER_S2C_SET_SPEED_BOOST: on_speed_boost(l, &f); break;
        case KRUNKER_S2C_SET_SHARING_IS_CARING: on_sharing(l, &f); break;
        case KRUNKER_S2C_SET_JUGGERNAUT: on_juggernaut(l, &f); break;
        case KRUNKER_S2C_SET_BUSY: on_busy(l, &f); break;
        case KRUNKER_S2C_SET_PLAYER_CHARGED: on_charged(l, &f); break;
        case KRUNKER_S2C_REMOVE_PLAYER: {
            const NetLeavePacket leave = {.uid = (int32_t) num(arg(&f, 0), -1)};
            player_remove(l, leave.uid);
            emit(l, NET_PACKET_LEAVE, &leave, sizeof(leave));
            break;
        }
        // player-state corrections (packet-audit cluster B)
        case KRUNKER_S2C_CHANGE_POSITION: on_change_position(l, &f); break;
        case KRUNKER_S2C_UPDATE_TEAM: on_update_team(l, &f); break;
        case KRUNKER_S2C_SET_PLAYER_KEY: on_set_player_key(l, &f); break;
        case KRUNKER_S2C_SET_WEAPON: on_set_weapon(l, &f); break;
        case KRUNKER_S2C_REGEN_PLAYER: on_regen(l, &f); break;
        case KRUNKER_S2C_REFILL_KNIFE: emit(l, NET_PACKET_REFILL, NULL, 0); break; // iïíìiìì: you.refillKnife()
        case KRUNKER_S2C_GAME_STATE_CHANGED: on_game_state(l, &f); break;
        case KRUNKER_S2C_UPDATE_NAMES: on_update_names(l, &f); break;
        case KRUNKER_S2C_KILL_PLAYER: on_kill(l, &f); break;
        case KRUNKER_S2C_DO_DAMAGE: on_damage(l, &f); break;
        case KRUNKER_S2C_CHANGE_HEALTH: on_health(l, &f); break;
        case KRUNKER_S2C_UPDATE_TIMER: on_timer(l, &f); break;
        case KRUNKER_S2C_ADD_CHAT: on_chat(l, &f); break;
        // score / kills / ammo HUD (audit cluster C)
        case KRUNKER_S2C_TEAM_SCORE: on_team_score(l, &f); break;
        case KRUNKER_S2C_GET_SCORE: on_get_score(l, &f); break;
        case KRUNKER_S2C_GET_KILL: on_get_kill(l, &f); break;
        case KRUNKER_S2C_UPDATE_LEADERS: on_update_leaders(l, &f); break;
        case KRUNKER_S2C_UPDATE_LEADER_BOARD: on_leader_board(l, &f); break;
        case KRUNKER_S2C_UPDATE_LIVES: on_lives(l, &f); break;
        case KRUNKER_S2C_UPDATE_AMMO: on_update_ammo(l, &f); break;
        case KRUNKER_S2C_KILL_STREAK: on_streak(l, &f); break;
        case KRUNKER_S2C_KILL_STREAK_M: on_kill_streak_message(l, &f); break;
        case KRUNKER_S2C_UPDATE_WEAPON_STREAK: on_weapon_streak(l, &f); break;
        case KRUNKER_S2C_GET_ASSIST: emit(l, NET_PACKET_ASSIST, NULL, 0); break; // iíîíiíï: the "Assist" medal
        case KRUNKER_S2C_ADD_MEDAL: on_add_medal(l, &f); break;
        case KRUNKER_S2C_UPDATE_KR_TAGS: on_kr_tags(l, &f); break;
        case KRUNKER_S2C_SOCKET_PING: send_c2s(l, KRUNKER_C2S_PING, NULL, 0); break; // iìîíîïí: "po"
        case KRUNKER_S2C_CHEAT_CHECK: on_cheat_check(l, &f); break;
        case KRUNKER_S2C_cpt: on_captcha(l, &f); break;
        case KRUNKER_S2C_cptR: note(l, KRUNKER_LINK_EV_NOTE, "security challenge accepted"); break; // _hideCaptcha
        case KRUNKER_S2C_ERROR: on_error(l, &f); break;
        case KRUNKER_S2C_VOTE_KICK: on_vote_kick(l, &f); break;
        case KRUNKER_S2C_KPD_RESPONSE: on_kpd_response(l, &f); break;
        case KRUNKER_S2C_KPD_RESET: emit_ext(l, NET_EXT_KPD_RESET, NULL, 0); break; // "kpdf" (iiíìîìi)
        case KRUNKER_S2C_DISCONNECT: note(l, KRUNKER_LINK_EV_ERROR, "the server disconnected us (dc)"); break;
        case KRUNKER_S2C_ACCOUNT_RESPONSE: on_account(l, &f); break;
        case KRUNKER_S2C_UPDATE_ACCOUNT: on_update_account(l, &f); break;
        case KRUNKER_S2C_UPDATE_BALANCES: on_balances(l, &f); break;
        case KRUNKER_S2C_UPDATE_CHALLENGE_REWARDS: on_challenge_rewards(l, &f); break;
        // objectives / flags / rounds / end (audit cluster D)
        case KRUNKER_S2C_UPDATE_FLAG_INFO: on_flag_info(l, &f); break;
        case KRUNKER_S2C_UPDATE_FLAG: on_flag(l, &f); break;
        case KRUNKER_S2C_UPDATE_FLAG_PING: on_flag_ping(l, &f); break;
        case KRUNKER_S2C_SET_OBJECTIVE: on_objective(l, &f); break;
        case KRUNKER_S2C_UPDATE_OBJECTIVE: on_objective_occupancy(l, &f); break;
        case KRUNKER_S2C_UPDATE_ROUNDS_DIS: on_rounds(l, &f); break;
        case KRUNKER_S2C_UPDATE_ZONE: zone_packet(l, arg(&f, 0)); break;
        case KRUNKER_S2C_END_GAME: on_end_game(l, &f); break;
        case KRUNKER_S2C_ACTION_FEED: on_action(l, &f); break;
        case KRUNKER_S2C_ADD_CHATI18N: on_chat_i18n(l, &f); break;
        // player actions (official-ingame-actions)
        case KRUNKER_S2C_UPD_MATCH_VOTE: on_match_vote(l, arg(&f, 0), arg(&f, 1)); break; // updMatchVote(vo, mts)
        case KRUNKER_S2C_ADD_SPRAY: on_add_spray(l, &f); break;
        case KRUNKER_S2C_MARK_POSITION: on_mark_position(l, &f); break;
        case KRUNKER_S2C_VOICE_CHAT: on_voice_chat(l, &f); break;
        case KRUNKER_S2C_UPDATE_HOST: on_update_host(l, &f); break;
        // social / account lines of a match (official-ingame-social)
        case KRUNKER_S2C_VOTE_FORFEIT: on_vote_forfeit(l, &f); break;
        case KRUNKER_S2C_PLAYER_RECONNECTED: on_player_reconnected(l, &f); break;
        case KRUNKER_S2C_FAILED_TO_ENDER: on_failed_to_enter(l, &f); break;
        case KRUNKER_S2C_SKIN_REWARD: on_skin_found(l, l->my_sid, arg(&f, 0)); break;
        case KRUNKER_S2C_SKIN_FOUND_MESSAGE: on_skin_found_message(l, &f); break;
        default: { // combat visuals / sounds (fx); the mode objectives / world objects / AI / zombies / map triggers (krunker_link_world.c)
            const KrunkerS2C id = (KrunkerS2C) krunker_s2c_lookup(f.name, f.name_len);
            if (!on_fx(l, id, &f) && !krunker_link_world_frame(l, id, &f) && !krunker_link_lobby_frame(l, id, &f) &&
                !krunker_link_script_frame(l, id, &f) && !krunker_link_prophunt_frame(l, id, &f))
                l->unknown_in++;
            break;
        }
    }
    krunker_frame_free(&f);
    return 1;
}

// "a" ACCOUNT_RESPONSE [error, id, username, data, token, ...] (iîïïiii, game.deob.js:210016): the
// game server signed the socket's account in (the `at=` of krunker_account.h). data is the account
// row (new Account(id, name, data), :114314): its level is floor(rankVar * sqrt(player_score))
// (updateLevel, rankVar 0.03). An error string (arg 0) = not signed in. The token (arg 4, the
// legacy krunker_token) is not kept: krunker_account.h holds the session.
// v10's Account.setData(row) reads row[47] as rankedPoints (v10:112873): JS's row[47] is the array's item
// 47 or the "47" key of an object, so the row is read both ways. NULL: the row carries no such value.
static const KrunkerValue *row_points(const KrunkerValue *row) {
    if (krunker_is(row, KRUNKER_ARRAY)) return krunker_at(row, 47);
    return krunker_get(row, "47");
}

// v10's Account.setData reads score from row[6] (v10:112832), and updateLevel derives the level from it
// (v10:112893: max(1, floor(0.03 * sqrt(score))), _7340 = 0.03 at v10:16549). The map form is the constructor's
// player_score (v10:112762). NULL: the row has no score.
static const KrunkerValue *row_score(const KrunkerValue *row) {
    if (krunker_is(row, KRUNKER_ARRAY)) return krunker_at(row, 6);
    return krunker_get(row, "player_score");
}

static void on_account(KrunkerLink *l, const KrunkerFrame *f) {
    const char *error = krunker_str(arg(f, 0));
    if (error && error[0]) {
        note(l, KRUNKER_LINK_EV_NOTE, "account: the server did not sign the account in (%s)", error);
        return;
    }
    const char *name = krunker_str(arg(f, 2));
    if (!name || !name[0]) return;
    snprintf(l->account_name, sizeof(l->account_name), "%s", name);
    const KrunkerValue *score = row_score(arg(f, 3));
    const double s = krunker_num(score, 0);
    const int level = (int) floor(0.03 * sqrt(s > 0 ? s : 0)); // progression_level (shared/src/progression.c)
    l->account_level = level < 1 ? 1 : level;
    const KrunkerValue *data = arg(f, 3);
    l->account_role = (int) krunker_num(krunker_get(data, "player_role"), 0);
    l->account_dev = (int) krunker_num(krunker_get(data, "player_developer"), 0);
    l->account_job_rating = (int) krunker_num(krunker_get(data, "jobrating"), 0);
    l->account_job_rating_pos = (int) krunker_num(krunker_get(data, "jobratingpositive"), 0);
    const KrunkerValue *points = row_points(data);
    if (krunker_is(points, KRUNKER_NUM)) {
        l->account_rp_known = 1;
        l->account_ranked_points = krunker_num(points, 0);
    }
    note(l, KRUNKER_LINK_EV_ACCOUNT, "%s", l->account_name);
}

// The balance from a wallet packet: the account's rankedPoints, as the link keeps it (note: the event tells krunker_net)
static void set_points(KrunkerLink *l, const double points) {
    l->account_rp_known = 1;
    l->account_ranked_points = points;
    note(l, KRUNKER_LINK_EV_ACCOUNT_POINTS, "rankedPoints %.0f", l->account_ranked_points);
}

// "ua" UPDATE_ACCOUNT [row] (v10: setData(row), :222148): the same row; only its rankedPoints is kept here
static void on_update_account(KrunkerLink *l, const KrunkerFrame *f) {
    const KrunkerValue *points = row_points(arg(f, 0));
    if (krunker_is(points, KRUNKER_NUM)) set_points(l, krunker_num(points, 0));
}

// The item at index i of an array, or the "i" key of a map (JS's x[i] reads both, as row_points does)
static const KrunkerValue *item_at(const KrunkerValue *v, const uint32_t i) {
    if (krunker_is(v, KRUNKER_ARRAY)) return krunker_at(v, i);
    char key[16];
    snprintf(key, sizeof(key), "%u", (unsigned) i);
    return krunker_get(v, key);
}

// "ub" UPDATE_BALANCES [balances] (v10: _69707, game.deob.js:208200, registered at :222187): v10 takes
// balances[3] (the ranked points, type 3) as the wallet's rankedPoints: its {amount, change}
static void on_balances(KrunkerLink *l, const KrunkerFrame *f) {
    const KrunkerValue *rp = item_at(arg(f, 0), 3);
    if (!krunker_is(rp, KRUNKER_MAP)) return;
    const KrunkerValue *amount = krunker_get(rp, "amount");
    if (krunker_is(amount, KRUNKER_NUM)) set_points(l, krunker_num(amount, 0));
}

// "chlR" UPDATE_CHALLENGE_REWARDS (v10: _69709 at :208212-208248): argument 6 is the ranked points the challenges
// gave (v10 applies the rest only when it is set), argument 7 the balance after them (parseInt, || 0)
static void on_challenge_rewards(KrunkerLink *l, const KrunkerFrame *f) {
    if (!krunker_num(arg(f, 6), 0)) return;
    set_points(l, krunker_num(arg(f, 7), 0));
}

// ---------------------------------------------------------------- outbound

// "en" ENTER_GAME (game.deob.js:208279): the 35-value enter array, then 11 client flags. A guest
// owns no cosmetics, so every cosmetic is a fresh krunker.io guest's saved value (:210435-210491:
// -1, team 1, spray 2482, secondary 2, streak indexes parseInt(null) = NaN, charms [undefined, -1]);
// only the class, the secondary, spectate, lag compensation, the sight, challenge and anon mode
// come from our ENT. On a socket signed in to a krunker.io account (l->account) the player's own
// loadout goes out instead, like the live client's saved selections: the spray, the weapon and melee
// skins and the cosmetic slots of our ENT (krunker.io keeps what the account owns).
static int send_enter(KrunkerLink *l, const void *payload, const uint16_t length) {
    NetEntPacket ent;
    memset(&ent, 0, sizeof(ent));
    ent.secondary_index = -1;
    ent.lag_comp = 1.0f;
    ent.cosmetics = PLAYER_COSMETICS_NONE;
    ent.skins = (PlayerSkins) {-1, -1, -1};
    ent.cosmetics_v7 = (PlayerCosmeticsV7) {-1, -1, -1, -1, -1, {-1, -1}, -1, -1};
    memcpy(&ent, payload, length < sizeof(ent) ? length : sizeof(ent));
    if (length <= offsetof(NetEntPacket, secondary_index)) ent.secondary_index = -1;
    if (length <= offsetof(NetEntPacket, lag_comp)) ent.lag_comp = 1.0f;
    if (length <= offsetof(NetEntPacket, attach_index)) ent.attach_index = 0;
    // streakIndex1..3 as the live client cleans them (:210481-210505): -1 none, the third slot 0 (the
    // Nuke) when unset; our STREAK_SLOTS (sent right before the ENT) when it came
    double streak[3] = {-1, -1, 0};
    for (int i = 0; l->streak_slots_set && i < 3; i++) streak[i] = l->streak_slots[i];
    const int own = l->account; // the account's loadout, else a guest's
#define OWN(v) ((own && (v) >= 0) ? (double) (v) : -1.0)
    const KrunkerValue skins[] = {KRUNKER_V_NUM(OWN(ent.skins.primary)), KRUNKER_V_NUM(OWN(ent.skins.secondary))};
    const KrunkerValue charms[] = {own && ent.cosmetics_v7.charms[0] >= 0 ? KRUNKER_V_NUM(ent.cosmetics_v7.charms[0]) : KRUNKER_V_NIL,
                                   KRUNKER_V_NUM(OWN(ent.cosmetics_v7.charms[1]))};
    const KrunkerValue enter[35] = {
        KRUNKER_V_NUM(ent.class_index),                                   // [0] classIndex
        KRUNKER_V_NUM(own && ent.spray_index > 0 ? ent.spray_index : 2482), // [1] sprayIndex (0x9b2)
        KRUNKER_V_ARRAY(skins, 2),                                        // [2] weapon skins
        KRUNKER_V_NUM(OWN(ent.cosmetics.hat)),                            // [3] hatIndex
        KRUNKER_V_NUM(OWN(ent.cosmetics.body)),                           // [4] bodyIndex
        KRUNKER_V_NUM(ent.secondary_index >= 0 ? ent.secondary_index : 2),// [5] secondaryInd
        KRUNKER_V_NUM(ent.spectate ? 1 : 0),                              // [6] spectate
        KRUNKER_V_NUM(ent.chall_mode ? 1 : 0),                            // [7] challenge mode
        KRUNKER_V_NUM(l->team_pick),                                      // [8] team pick (iïíiîiì, toggleTeam)
        KRUNKER_V_NUM(OWN(ent.skins.melee)),                              // [9] meleeIndex
        KRUNKER_V_NUM(-1),                                                // [10] skinColIndex
        KRUNKER_V_NUM(ent.lag_comp),                                      // [11] lagComp
        KRUNKER_V_NUM(ent.attach_index >= 0 ? ent.attach_index : 0),      // [12] attachIndex
        KRUNKER_V_NUM(l->pc_stat),                                        // [13] pcStatIndex (PC_STAT, -1 none)
        KRUNKER_V_NUM(OWN(ent.cosmetics.dye)),                            // [14] dyeIndex
        KRUNKER_V_NUM(OWN(ent.cosmetics_v7.shoe)),                        // [15] shoeIndex
        KRUNKER_V_NUM(OWN(ent.cosmetics.waist)),                          // [16] waistIndex
        KRUNKER_V_NUM(-1),                                                // [17] hairCol
        KRUNKER_V_NUM(-1),                                                // [18] chatCol
        KRUNKER_V_NUM(ent.anon_mode ? 1 : 0),                             // [19] anonMode
        KRUNKER_V_NUM(OWN(ent.cosmetics.face)),                           // [20] faceIndex
        KRUNKER_V_NUM(OWN(ent.cosmetics_v7.pet)),                         // [21] petIndex
        KRUNKER_V_NUM(0),                                                 // [22] verifiedBadge
        KRUNKER_V_NUM(0),                                                 // [23] premiumBadge
        KRUNKER_V_NUM(0),                                                 // [24] customBadge
        KRUNKER_V_NUM(OWN(ent.cosmetics_v7.wrist)),                       // [25] wristIndex
        KRUNKER_V_NUM(streak[0]), KRUNKER_V_NUM(streak[1]), KRUNKER_V_NUM(streak[2]), // [26..28] streakIndex1..3
        KRUNKER_V_ARRAY(charms, 2),                                       // [29] charms
        KRUNKER_V_NUM(OWN(ent.cosmetics_v7.back)),                        // [30] backIndex
        KRUNKER_V_NUM(OWN(ent.cosmetics_v7.head)),                        // [31] headIndex
        KRUNKER_V_NUM(OWN(ent.cosmetics_v7.player_card)),                 // [32] playerCardIndex
        KRUNKER_V_NUM(0),                                                 // [33] rankBadge
        KRUNKER_V_NUM(-1),                                                // [34] titleIndex
    };
#undef OWN
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
void krunker_link_input_fields(const NetInputPacket *in, double v[15]) {
    v[0] = in->seq;
    v[1] = js_round(in->delta * 1000.0); // * config.deltaMlt (1)
    v[2] = js_round(1000.0 * (js_round(in->x_dir * 1000.0) / 1000.0));
    v[3] = js_round(1000.0 * (js_round(in->y_dir * 1000.0) / 1000.0));
    v[4] = in->move_dir >= 0 && in->move_dir <= 7 ? (in->move_dir + 1) % 8 : -1; // movDirs index (see the top)
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

// queueInputFrame's syncTime (index.js:17085; v10.js:221512): the frame fired. Its servTim is the
// last "k"'s; its fraction (round 2) is where now - 1000 / sendFrequency lies between the last two "k"s,
// 0 without a previous one. No "k" yet: nothing to send (JS would send undefined).
void krunker_link_mark_sync(KrunkerLink *l, const int index) {
    if (index < 0 || index >= NET_INPUT_BATCH_MAX || !l->sync_known) return;
    const double now = link_now_ms(l);
    const double send_ms = 1000.0 / (l->send_hz > 0 ? l->send_hz : 10);
    double prg = 0.0;
    if (l->last_sync) {
        const double span = l->last_sync - l->new_sync;
        if (span != 0.0) prg = js_round(((now - send_ms) - l->new_sync) / span * 100.0) / 100.0;
    }
    l->sync_tim[index] = l->serv_tim;
    l->sync_prg[index] = prg;
    if (!l->sync_set[index]) l->sync_frames++;
    l->sync_set[index] = 1;
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
    double first_isn = 0, last_isn = 0;
    for (int j = 0; j < count && j < NET_INPUT_BATCH_MAX; j++) {
        double v[15];
        krunker_link_input_fields(&frames[j], v);
        input_locks(l, v);
        if (j == 0) first_isn = v[0];
        last_isn = v[0];
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

    // syncTimes (v10.js:221512-221521, sent at 221573-221577): {"<frame>": servTim, "<frame>p": fraction} for the
    // frames that fired
    // (krunker_link_mark_sync); the fraction only when nonzero. Only the frames of this window.
    char sync_keys[2 * NET_INPUT_BATCH_MAX][12];
    KrunkerValue sync_kv[4 * NET_INPUT_BATCH_MAX];
    int sync_pairs = 0;
    for (int j = 0; j < n && l->sync_frames; j++) {
        if (!l->sync_set[j]) continue;
        const int key = 2 * sync_pairs; // KRUNKER_V_STR evaluates its argument twice: no side effects in it
        snprintf(sync_keys[sync_pairs], sizeof(sync_keys[sync_pairs]), "%d", j);
        sync_kv[key] = KRUNKER_V_STR(sync_keys[sync_pairs]);
        sync_kv[key + 1] = KRUNKER_V_NUM(l->sync_tim[j]);
        sync_pairs++;
        if (l->sync_prg[j]) {
            snprintf(sync_keys[sync_pairs], sizeof(sync_keys[sync_pairs]), "%dp", j);
            sync_kv[2 * sync_pairs] = KRUNKER_V_STR(sync_keys[sync_pairs]);
            sync_kv[2 * sync_pairs + 1] = KRUNKER_V_NUM(l->sync_prg[j]);
            sync_pairs++;
        }
    }
    // the window's optional tail: the angle and change slots are filled whenever a later one is (v10.js:221567-221576:
    // (syncTimes || changes || angles) pushes angles or 0, then changes or 0, then the syncTimes)
    KrunkerValue args[7];
    size_t argc = 0;
    args[argc++] = KRUNKER_V_NUM(l->resync ? 1 : 0);
    args[argc++] = KRUNKER_V_NUM(first_isn);
    args[argc++] = same_width ? KRUNKER_V_STRN(dt_str, used) : KRUNKER_V_ARRAY(dt_items, n);
    args[argc++] = KRUNKER_V_NUM(same_width ? (double) dt_len : 0);
    if (changes || sync_pairs || (angle_count && !same_angles)) args[argc++] = same_angles ? KRUNKER_V_NUM(0) : KRUNKER_V_ARRAY(angle_items, angle_count);
    if (changes || sync_pairs) args[argc++] = changes ? KRUNKER_V_MAP(change_kv, changes) : KRUNKER_V_NUM(0);
    if (sync_pairs) args[argc++] = KRUNKER_V_MAP(sync_kv, sync_pairs);
    const int sent = send_c2s(l, KRUNKER_C2S_INPUT, args, argc);
    // the window is spent whether or not it left: its frames' syncTimes go with it
    memset(l->sync_set, 0, sizeof(l->sync_set));
    l->sync_frames = 0;
    if (sent) {
        l->q_sent++;
        l->q_frames += (unsigned) n;
        l->last_isn_sent = (int) last_isn;
    }
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
        case NET_PACKET_VOICE_V7: return send_voice(l, payload, length);
        case NET_PACKET_CHAT_V7: {
            NetChatV7SendPacket c;
            memset(&c, 0, sizeof(c));
            memcpy(&c, payload, length < sizeof(c) ? length : sizeof(c));
            c.text[sizeof(c.text) - 1] = 0;
            const KrunkerValue args[] = {KRUNKER_V_NUM(c.channel == 1 ? 1 : 0), KRUNKER_V_STR(c.text)};
            return send_c2s(l, KRUNKER_C2S_CHAT_MESSAGE, args, 2);
        }
        // the ENT's companions: kept for the "en" that follows (send_enter [26..28], [13])
        case NET_PACKET_STREAK_SLOTS:
            if (length < sizeof(NetStreakSlotsPacket)) return 0;
            memcpy(l->streak_slots, payload, sizeof(l->streak_slots));
            l->streak_slots_set = 1;
            return 0;
        case NET_PACKET_PC_STAT:
            if (length < sizeof(NetPcStatPacket)) return 0;
            l->pc_stat = ((const int8_t *) payload)[0];
            return 0;
        case NET_PACKET_USE_STREAK: { // callInStreak(n) (:220943): "k" + n, no args
            if (length < sizeof(NetUseStreakPacket)) return 0;
            const int slot = ((const uint8_t *) payload)[0];
            if (slot > 2) return 0;
            return send_c2s(l, (KrunkerC2S) (KRUNKER_C2S_KILL_STREAK_0 + slot), NULL, 0);
        }
        case NET_PACKET_SPECTATE_TARGET: { // ["fpSp", sid] the followed player, ["fpSp"] the free cam (:209610 / :209618)
            NetSpectateTargetPacket t = {.uid = -1};
            memcpy(&t, payload, length < sizeof(t) ? length : sizeof(t));
            const KrunkerValue args[] = {KRUNKER_V_NUM(t.uid)};
            return send_c2s(l, KRUNKER_C2S_fpSp, args, t.uid >= 0 ? 1 : 0);
        }
        case NET_PACKET_SPRAY_V7: { // the spray key: ["spry"]; the premium wheel: ["spry", index] (:221434 / :221489)
            NetSprayRequestPacket r = {.index = -1};
            memcpy(&r, payload, length < sizeof(r) ? length : sizeof(r));
            const KrunkerValue args[] = {KRUNKER_V_NUM(r.index)};
            return send_c2s(l, KRUNKER_C2S_SPRAY, args, r.index >= 0 ? 1 : 0);
        }
        case NET_PACKET_TAUNT: { // the Prop team's taunt: ["prpt", i] (:221444)
            if (length < sizeof(NetTauntPacket)) return 0;
            const KrunkerValue args[] = {KRUNKER_V_NUM(((const uint8_t *) payload)[0])};
            return send_c2s(l, KRUNKER_C2S_PROP_HUNT_TAUNT, args, 1);
        }
        case NET_PACKET_INTERACT: { // v7: action 1 ["int" / "intSec", type, id] (:221454), 2 ["drpw"]; 0 (v3's spray) ["spry"]
            NetInteractV7Packet in = {.type = -1, .uid = -1};
            if (length < sizeof(NetInteractPacket)) return 0;
            memcpy(&in, payload, length < sizeof(in) ? length : sizeof(in));
            if (in.action == 0) return send_c2s(l, KRUNKER_C2S_SPRAY, NULL, 0);
            if (in.action == 2) return send_c2s(l, KRUNKER_C2S_DROP_WEAPON, NULL, 0);
            if (in.action != 1) return 0;
            if (length < sizeof(NetInteractV7Packet)) in.type = in.uid = -1; // v3's "int" 1: nothing scanned
            const KrunkerValue args[] = {KRUNKER_V_NUM(in.type), KRUNKER_V_NUM(in.uid)};
            return send_c2s(l, in.secondary ? KRUNKER_C2S_SECONDARY_INTERACT : KRUNKER_C2S_INTERACT, args, 2);
        }
        case NET_PACKET_CONFIRM_INTERACT: { // confirmKey: ["cnfm"] a pending trigger, ["cnfm", 2] a pending spin (:221452)
            NetConfirmPacket c = {.uid = -1};
            memcpy(&c, payload, length < sizeof(c) ? length : sizeof(c));
            const KrunkerValue args[] = {KRUNKER_V_NUM(2)};
            return send_c2s(l, KRUNKER_C2S_CUSTOM_TRIGGER_INTERACTION, args, c.kind == 2 ? 1 : 0);
        }
        case NET_PACKET_COMP_SET: { // updSyncDat (:219146): ["cd", "t", team] (toggleTeam), ["cd", "c", {c, s}] (selectClass :210761)
            NetCompSetPacket c;
            if (length < sizeof(c)) return 0;
            memcpy(&c, payload, sizeof(c));
            if (c.key == 't') {
                l->team_pick = c.value; // iïíiîiì = team || 0: the next "en" [8]
                const KrunkerValue args[] = {KRUNKER_V_STR("t"), KRUNKER_V_NUM(c.value)};
                return send_c2s(l, KRUNKER_C2S_SYNC_CONNECTION_DATA, args, 2);
            }
            if (c.key != 'c') return 0;
            // s: the class's secondary pick (parseInt(secondaryInd)) when it has one, else null; our
            // COMP_SET carries no secondary, so the server keeps the one the next "en" [5] names
            const KrunkerValue kv[] = {KRUNKER_V_STR("c"), KRUNKER_V_NUM(c.value), KRUNKER_V_STR("s"), KRUNKER_V_NIL};
            const KrunkerValue args[] = {KRUNKER_V_STR("c"), KRUNKER_V_MAP(kv, 2)};
            return send_c2s(l, KRUNKER_C2S_SYNC_CONNECTION_DATA, args, 2);
        }
        case NET_PACKET_MATCH_VOTE: { // voteOnMatch(option) (:220127): ["maVote", the option's own mts string]
            NetMatchVotePacket v;
            if (length < sizeof(v)) return 0;
            memcpy(&v, payload, sizeof(v));
            for (int i = 0; i < l->vote_count; i++) {
                if (l->vote_mode[i] != v.mode_id || l->vote_map[i] != v.map_index) continue;
                const KrunkerValue args[] = {KRUNKER_V_STR(l->vote_mts[i])};
                return send_c2s(l, KRUNKER_C2S_MATCH_VOTE, args, 1);
            }
            return 0;
        }
        case NET_PACKET_EXT: { // the player list's vote kick / K.P.D call, the ping; other subtypes are KrunkNative's
            uint16_t subtype, body_length;
            const uint8_t *body;
            if (!net_ext_unpack(payload, length, &subtype, &body, &body_length)) return 0;
            const int prop = krunker_link_prophunt_send(l, subtype, body, body_length); // "prp" / "prpR"
            if (prop >= 0) return prop;
            if (subtype == NET_EXT_MARK_POSITION && body_length >= sizeof(NetMarkPositionSendPacket)) {
                // markPosition (:170208, sent :221369 only `isRanked || isComp`): ["mpos", {x, y, z}]
                if (!l->is_ranked && !l->is_comp) return 0;
                NetMarkPositionSendPacket m;
                memcpy(&m, body, sizeof(m));
                const KrunkerValue kv[] = {KRUNKER_V_STR("x"), KRUNKER_V_NUM(m.x), KRUNKER_V_STR("y"), KRUNKER_V_NUM(m.y),
                                           KRUNKER_V_STR("z"), KRUNKER_V_NUM(m.z)};
                const KrunkerValue args[] = {KRUNKER_V_MAP(kv, 3)};
                return send_c2s(l, KRUNKER_C2S_MARK_POSITION, args, 1);
            }
            if (subtype == NET_EXT_KN_OFFICIAL_GAME_CODE && body_length >= 1) { // submitGameCode (:219238): ["code", value]
                char code[128];
                const size_t n = body_length < sizeof(code) ? body_length : sizeof(code) - 1;
                memcpy(code, body, n);
                code[n] = 0;
                if (!code[0]) return 0;
                const KrunkerValue args[] = {KRUNKER_V_STR(code)};
                return send_c2s(l, KRUNKER_C2S_CODE, args, 1);
            }
            if (subtype == NET_EXT_VOTE_KICK && body_length >= sizeof(NetVoteKickPacket)) {
                NetVoteKickPacket v; // kickVote / [1]: ["v2k", alias, 1]; [2]: ["v2k", alias]
                memcpy(&v, body, sizeof(v));
                v.target[sizeof(v.target) - 1] = 0;
                const KrunkerValue args[] = {KRUNKER_V_STR(v.target), KRUNKER_V_NUM(1)};
                return send_c2s(l, KRUNKER_C2S_VOTE_KICK, args, v.vote ? 2 : 1);
            }
            if (subtype == NET_EXT_KPD_CALL && body_length >= sizeof(NetKpdCallPacket)) {
                NetKpdCallPacket c;
                memcpy(&c, body, sizeof(c));
                if (c.tab != 0) { // switchKPDTab: ["kpd", tab]
                    const KrunkerValue args[] = {KRUNKER_V_NUM(c.tab)};
                    return send_c2s(l, KRUNKER_C2S_KPD, args, 1);
                }
                const KrunkerLinkPlayer *p = player(l, c.uid, 0);
                if (!p || !p->id[0]) return 0;
                const KrunkerValue kv[] = {KRUNKER_V_STR("id"), KRUNKER_V_STR(p->id)}; // callPolice: ["kpd", 0, {id}]
                const KrunkerValue args[] = {KRUNKER_V_NUM(0), KRUNKER_V_MAP(kv, 1)};
                return send_c2s(l, KRUNKER_C2S_KPD, args, 2);
            }
            if (subtype == NET_EXT_KPD_RATE && body_length >= sizeof(NetKpdRatePacket)) { // the officer's verdict: ["rkpd", 0 | 1]
                const KrunkerValue args[] = {KRUNKER_V_NUM(body[0] ? 1 : 0)};
                return send_c2s(l, KRUNKER_C2S_RATE_KPD, args, 1);
            }
            return 0;
        }
        default:
            return 0; // HELLO, JOIN, PONG (we answer "pi" ourselves), the KrunkNative-only packets
    }
}
