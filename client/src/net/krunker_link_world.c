// Official Krunker session: mode objectives, world objects, AI, zombies and map-trigger packets
// (client/src/net/krunker_link_internal.h). Same rules as krunker_link.c: pure translation of the
// official packets into the NetPackets our own server sends for the same thing, so the client's
// existing consumers (hud_modes.c, map_triggers.c, world_events.c, zombie_events.c, game_events.c)
// show them. Line numbers: ~/.cache/kn-official/game.deob.js.
//
// Known gaps (documented, not translated):
//   - "inat" arg 11 (no auto knife): our LOADOUT always adds the knife unless the mode is noMelee.
//   - "inat" / "inatA" pick_0 sound, "do" breakSnd: the consumers play neither.
//   - init "dyno" with h = 0 keeps y (the build's default height is not known here).
#include "krunker_link_internal.h"
#include <ai.h>
#include <modes.h>
#include <net_ext.h>
#include <ruleset.h>
#include <zombies.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

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
// a string value, or a number printed as JS would (integers only matter here); "" otherwise
static void text_of(const KrunkerValue *v, char *out, const size_t n) {
    out[0] = 0;
    if (krunker_is(v, KRUNKER_STR)) snprintf(out, n, "%s", krunker_str(v));
    else if (krunker_is(v, KRUNKER_NUM) && v->u.num == floor(v->u.num)) snprintf(out, n, "%.0f", v->u.num);
    else if (krunker_is(v, KRUNKER_NUM)) snprintf(out, n, "%g", v->u.num);
}
#define EMIT(type, value) krunker_link_emit(l, (uint8_t) (type), &(value), (uint16_t) sizeof(value))

// ---------------------------------------------------------------- Domination

// updateDomZone(uid, capturedBy, captureT || 0, lastTeamOn || null, teamsOn || [], mlt || 1) (:188674)
static void dom_entry(NetDomZoneState *z, const KrunkerValue *uid, const KrunkerValue *by, const KrunkerValue *t,
                      const KrunkerValue *last, const KrunkerValue *on, const KrunkerValue *mlt) {
    memset(z, 0, sizeof(*z));
    z->uid = (int32_t) num(uid, -1);
    z->object_id = z->uid; // the official link's zones are v10's uids (its own numbering, no KN map object)
    z->captured_by = (int32_t) num(by, 0);
    z->capture_t = (float) num(t, 0);
    z->last_team_on = (int32_t) num(last, 0);
    for (uint32_t i = 0; krunker_is(on, KRUNKER_ARRAY) && i < on->len && z->teams_on_count < MODE_TEAMS; i++)
        z->teams_on[z->teams_on_count++] = (int8_t) num(krunker_at(on, i), 0);
    const double m = num(mlt, 0);
    z->mlt = m ? (float) m : 1.0f;
}

// "uct" UPDATE_DOM_ZONE [uid, capturedBy, captureT, lastTeamOn, teamsOn, mlt]* (iïïíîiì :220906)
static void on_dom_update(KrunkerLink *l, const KrunkerValue *list) {
    NetDomUpdatePacket p;
    memset(&p, 0, sizeof(p));
    for (uint32_t i = 0; krunker_is(list, KRUNKER_ARRAY) && i + 6 <= list->len; i += 6) {
        const KrunkerValue *e = list->u.items + i;
        dom_entry(&p.zones[p.count++], &e[0], &e[1], &e[2], &e[3], &e[4], &e[5]);
        if (p.count == MODE_MAX_DOM_ZONES) {
            EMIT(NET_PACKET_DOM_UPDATE, p);
            memset(&p, 0, sizeof(p));
        }
    }
    if (p.count) EMIT(NET_PACKET_DOM_UPDATE, p);
}

// ---------------------------------------------------------------- spawnables

// "crsp" CREATE_SPAWNABLE [type, sid, x, y, z, team, data] (iìiìíïì :221282 -> createSpawnable
// :188891); data {c} (KR Tag count), {wId} (Weapon Box), {xD, zD} / Loot Drop {itemId, reason, xD}
static void spawnable(KrunkerLink *l, const KrunkerValue *a, const uint32_t argc) {
#define A(i) ((i) < argc ? &a[(i)] : NULL)
    NetSpawnableV7Packet p;
    memset(&p, 0, sizeof(p));
    p.type = (uint8_t) num(A(0), 0);
    p.sid = (int32_t) num(A(1), -1);
    p.active = 1;
    p.position[0] = (float) num(A(2), 0);
    p.position[1] = (float) num(A(3), 0);
    p.position[2] = (float) num(A(4), 0);
    p.team = (int32_t) num(A(5), 0);
    const KrunkerValue *data = A(6);
    p.data_c = (int32_t) num(krunker_get(data, "c"), 0);
    p.data_w = (int32_t) num(krunker_get(data, "wId"), -1);
    p.data_xd = (float) num(krunker_get(data, "xD"), 0);
    p.data_zd = (float) num(krunker_get(data, "zD"), 0);
#undef A
    EMIT(NET_PACKET_SPAWNABLE_V7, p);
}

// ---------------------------------------------------------------- world objects

static void object_state(KrunkerLink *l, const KrunkerValue *uid, const NetObjectStateKind kind, const int32_t value) {
    if (!krunker_is(uid, KRUNKER_NUM)) return;
    NetObjectStatePacket p;
    memset(&p, 0, sizeof(p));
    p.uid = (int32_t) uid->u.num;
    p.kind = (uint8_t) kind;
    p.value = value;
    EMIT(NET_PACKET_OBJECT_STATE, p);
}

// "bo" BUILD_OBJECT [buildInd, x, y, z, w, h, l, dir, uid] (game.buildObj :187837); center (init
// "dyno"): the y given is the build's middle, buildObj lowers it by h / 2
static void build(KrunkerLink *l, const KrunkerValue *a, const uint32_t argc, const int center) {
#define A(i) ((i) < argc ? &a[(i)] : NULL)
    if (!krunker_is(A(8), KRUNKER_NUM)) return;
    NetBuildPacket p;
    memset(&p, 0, sizeof(p));
    p.uid = (int32_t) A(8)->u.num;
    p.build_index = (uint8_t) num(A(0), 0);
    // dir is radians (rotDir: Left 0, Front PI / 2, Right PI, Back -PI / 2, deob-10.0.3.js:195816-195826, sent
    // by buildObj :216805); NetBuildPacket.direction counts quarter turns 0..3 (shared/src/build.c build_quarter)
    const long quarter = lround(num(A(7), 0) / (M_PI / 2.0));
    p.direction = (int8_t) (((quarter % 4) + 4) % 4);
    p.position[0] = (float) num(A(1), 0);
    p.position[1] = (float) num(A(2), 0);
    p.position[2] = (float) num(A(3), 0);
    p.size[0] = (float) num(A(4), 0);
    p.size[1] = (float) num(A(5), 0);
    p.size[2] = (float) num(A(6), 0);
    if (center && p.size[1] > 0.0f) p.position[1] -= p.size[1] / 2.0f;
#undef A
    EMIT(NET_PACKET_BUILD, p);
}

// ---------------------------------------------------------------- AI

// "ai" SYNC_ALL_AI [[sid, x, y, z, xD, yD, floor(hp), didMov, team]* | 0] (AI.syncAll :179948): hideAll,
// then the listed ones. Chunks of NET_AI_SYNC_V7_MAX; only the first hides the rest (first = 1).
static void ai_sync(KrunkerLink *l, const KrunkerValue *list) {
    NetAISyncV7Packet p;
    memset(&p, 0, sizeof(p));
    p.first = 1;
    for (uint32_t i = 0; krunker_is(list, KRUNKER_ARRAY) && i + 9 <= list->len; i += 9) {
        const KrunkerValue *e = list->u.items + i;
        NetAISyncV7Entry *o = &p.entries[p.count++];
        o->sid = (int32_t) num(&e[0], -1);
        o->position[0] = (float) num(&e[1], 0);
        o->position[1] = (float) num(&e[2], 0);
        o->position[2] = (float) num(&e[3], 0);
        o->x_dir = (float) num(&e[4], 0);
        o->y_dir = (float) num(&e[5], 0);
        o->health = (int32_t) num(&e[6], 0);
        o->did_move = (uint8_t) truthy(&e[7]);
        o->team = (int16_t) num(&e[8], 0);
        if (p.count == NET_AI_SYNC_V7_MAX) {
            EMIT(NET_PACKET_AI_SYNC_V7, p);
            memset(&p, 0, sizeof(p));
        }
    }
    if (p.count || p.first) EMIT(NET_PACKET_AI_SYNC_V7, p);
}

// "aai" C_SPAWN [getData()] = [sid, index, name, x, y, z, dat ? {...} : 0] (AI.cSpawn :179892, getData :179381)
static void ai_add(KrunkerLink *l, const KrunkerValue *d) {
    if (!krunker_is(d, KRUNKER_ARRAY) || !krunker_is(krunker_at(d, 0), KRUNKER_NUM)) return;
    NetAIAddV7Packet p;
    memset(&p, 0, sizeof(p));
    p.sid = (int32_t) num(krunker_at(d, 0), -1);
    p.index = (int32_t) num(krunker_at(d, 1), 0);
    text_of(krunker_at(d, 2), p.name, sizeof(p.name));
    for (int k = 0; k < 3; k++) p.position[k] = (float) num(krunker_at(d, (uint32_t) (3 + k)), 0);
    const KrunkerValue *dat = krunker_at(d, 6);
    p.has_data = (uint8_t) krunker_is(dat, KRUNKER_MAP);
    p.sound_range = 1.0f;
    p.hbh = 1.0f;
    p.tag_scale = 1.0f;
    if (p.has_data) {
#define F(key, def) num(krunker_get(dat, key), def)
        p.has_anim = (uint8_t) truthy(krunker_get(dat, "hasAnim"));
        p.anim_speed = (float) F("animS", 0);
        p.sound_range = (float) F("soundR", 1);
        p.team = (int16_t) F("team", 0);
        p.max_health = (float) F("health", 0);
        p.m_size = (float) F("mSize", 0);
        p.m_y_off = (float) F("mYOff", 0);
        p.m_r_off = (float) F("mROff", 0);
        p.hbh = (float) F("hbh", 1);
        const KrunkerValue *asset = krunker_get(dat, "asset");
        if (truthy(asset)) text_of(asset, p.asset, sizeof(p.asset)); // `dat.asset || 0`: 0 = none
        p.idle_sound = (int32_t) F("idl", 0);
        text_of(krunker_get(dat, "src"), p.src, sizeof(p.src));
        p.no_vis = (uint8_t) truthy(krunker_get(dat, "noVis"));
        p.tag_scale = (float) F("tagScale", 1);
        p.lvl = (int16_t) F("lvl", 0);
        p.side = (uint8_t) F("side", 0);
        p.opacity = (float) F("opacity", 0);
        p.tex_encode = (uint8_t) F("texEncode", 0);
        p.alpha_test = (float) F("alphaTest", 0);
        p.no_smooth = (uint8_t) truthy(krunker_get(dat, "noSmooth"));
        p.no_fog = (uint8_t) truthy(krunker_get(dat, "noFog"));
        p.f_trans = (uint8_t) truthy(krunker_get(dat, "fTrans"));
        p.frames = (uint8_t) F("frames", 0);
        p.frame_time = (float) F("frameT", 0);
        p.r_anim = (float) F("rAnim", 0);
        p.r_anim_axis = (uint8_t) F("rAnimA", 0);
        p.bob_d = (float) F("bobD", 0);
        p.bob_s = (float) F("bobS", 0);
#undef F
    }
    EMIT(NET_PACKET_AI_ADD_V7, p);
}

// ---------------------------------------------------------------- zombies

// "mybx" / "krm" [uid, active, state, timer ms, pendingWeapon, purchasedBy] (updateMysteryBox :189059,
// updateKrumMachine :189180); undefined / null pendingWeapon / purchasedBy -> ZOMBIES_NONE
static void zombie_machine(KrunkerLink *l, const ZombieMachineKind kind, const KrunkerValue *a, const uint32_t argc) {
#define A(i) ((i) < argc ? &a[(i)] : NULL)
    if (!krunker_is(A(0), KRUNKER_NUM)) return;
    NetZombieMachinePacket p;
    memset(&p, 0, sizeof(p));
    p.kind = (uint8_t) kind;
    p.uid = (int32_t) A(0)->u.num;
    p.active = (uint8_t) truthy(A(1));
    p.state = (uint8_t) num(A(2), 0);
    p.timer = (float) num(A(3), 0);
    p.pending_weapon = is_nil(A(4)) ? ZOMBIES_NONE : (int32_t) num(A(4), ZOMBIES_NONE);
    p.purchased_by = is_nil(A(5)) ? ZOMBIES_NONE : (int32_t) num(A(5), ZOMBIES_NONE);
#undef A
    EMIT(NET_PACKET_ZOMBIE_MACHINE, p);
}

static void powerup(KrunkerLink *l, const int index, const KrunkerValue *ms) {
    if (index < 0 || index > ZOMBIES_POWERUP_DOUBLE_POINTS) return;
    NetPowerupPacket p;
    memset(&p, 0, sizeof(p));
    p.index = (uint8_t) index;
    p.ms = (int32_t) num(ms, 0);
    EMIT(NET_PACKET_POWERUP, p);
}

// ---------------------------------------------------------------- loadouts / pickups

// a weapon's full magazine (weaponConfig[w].ammo), 0 unknown
static int32_t weapon_ammo(const int w) {
    const Weapon *weapon = w >= 0 ? rules_weapon(w) : NULL;
    return weapon ? (int32_t) weapon->ammo : 0;
}

// updateLoadout(game, loadoutIndex, true, loadout, ammos) (:169317): the loadout's non-null weapons in
// order, ammos by the compacted slot; -1 = the weapon's own (the consumer keeps mode_set_player_loadout's)
static void loadout(KrunkerLink *l, const int sid, const KrunkerValue *weapons, const KrunkerValue *index, const KrunkerValue *ammos) {
    NetLoadoutPacket p;
    memset(&p, 0, sizeof(p));
    p.uid = sid;
    for (uint32_t i = 0; krunker_is(weapons, KRUNKER_ARRAY) && i < weapons->len && p.count < NET_LOADOUT_MAX; i++) {
        const KrunkerValue *w = krunker_at(weapons, i);
        if (!krunker_is(w, KRUNKER_NUM)) continue;
        p.weapons[p.count] = (int32_t) w->u.num;
        const KrunkerValue *a = krunker_at(ammos, (uint32_t) p.count);
        p.ammo[p.count] = krunker_is(a, KRUNKER_NUM) ? (int32_t) a->u.num : -1;
        p.count++;
    }
    if (!p.count) return;
    p.loadout_index = (int32_t) num(index, 0);
    EMIT(NET_PACKET_LOADOUT, p);
}

// updatePickup(uid, weapon, orgPickup, active, [x, y, z] | null, ammo) (:189213)
static void pickup(KrunkerLink *l, const KrunkerValue *uid, const KrunkerValue *weapon, const KrunkerValue *active,
                   const KrunkerValue *pos, const KrunkerValue *ammo) {
    if (!krunker_is(uid, KRUNKER_NUM)) return;
    NetPickupPacket p;
    memset(&p, 0, sizeof(p));
    p.uid = (int32_t) uid->u.num;
    p.weapon = krunker_is(weapon, KRUNKER_NUM) ? (int32_t) weapon->u.num : -1;
    p.active = (uint8_t) truthy(active);
    if (krunker_is(pos, KRUNKER_ARRAY) && pos->len >= 3) {
        p.moved = 1;
        for (uint32_t k = 0; k < 3; k++) p.position[k] = (float) num(krunker_at(pos, k), 0);
    }
    p.ammo = krunker_is(ammo, KRUNKER_NUM) ? (int32_t) ammo->u.num : weapon_ammo(p.weapon < 0 ? 0 : p.weapon);
    EMIT(NET_PACKET_PICKUP, p);
}

// "inat" PLAYER_INTERACTIONS [sid, loadout, loadoutIndex, pickupUid, pickupWeapon, doPickup = true,
// pickupActive = true, pos, ammos, reserves, upgrades, noKnife] (iîïìïíì :220071)
static void on_interactions(KrunkerLink *l, const KrunkerFrame *f) {
    if (krunker_is(arg(f, 0), KRUNKER_NUM)) loadout(l, (int) arg(f, 0)->u.num, arg(f, 1), arg(f, 2), arg(f, 8));
    const int do_pickup = f->argc <= 5 || truthy(arg(f, 5)); // a default parameter: absent = true
    const int active = f->argc <= 6 || truthy(arg(f, 6));
    const KrunkerValue on = KRUNKER_V_BOOL(1);
    if (do_pickup) pickup(l, arg(f, 3), arg(f, 4), active ? &on : NULL, arg(f, 7), NULL);
}

// ---------------------------------------------------------------- messages

// "gmsg" UPDATE_GAME_MESSAGE [msg, suffix, countdown, [title, sub] | 0, hideUI] (iíïììií :220406):
// msg "wt" = player.waiting; the suffix follows a space. "wt" with a suffix travels as "wt <suffix>"
// (hud_modes.c translates the head).
static void game_message(KrunkerLink *l, const KrunkerValue *msg, const KrunkerValue *suffix) {
    NetGameMessagePacket p;
    memset(&p, 0, sizeof(p));
    char head[NET_GAME_MESSAGE_SIZE], tail[NET_GAME_MESSAGE_SIZE];
    text_of(msg, head, sizeof(head));
    text_of(suffix, tail, sizeof(tail));
    if (head[0]) snprintf(p.text, sizeof(p.text), "%s%s%s", head, truthy(suffix) ? " " : "", truthy(suffix) ? tail : "");
    EMIT(NET_PACKET_GAME_MESSAGE, p);
}

// textPopup(title, sub, silent, subSize) (iìîíììì :220929) as NET_EXT_KN_OFFICIAL_TEXT_POPUP: the
// strings whole (NetTextPopupPacket's 32 bytes would cut "Hunt the stalker and survive the round!")
static void text_popup_strings(KrunkerLink *l, const char *title, const char *sub, const int silent, const int sub_size) {
    if (!title[0] && !sub[0]) return;
    uint8_t body[NET_EXT_BODY_MAX - 8];
    const NetOfficialTextPopupHeader h = {.silent = (uint8_t) !!silent, .sub_size = (uint8_t) (sub_size > 0 && sub_size < 256 ? sub_size : 0)};
    const size_t tl = strlen(title) + 1, sl = strlen(sub) + 1;
    if (sizeof(h) + tl + sl > sizeof(body)) return;
    memcpy(body, &h, sizeof(h));
    memcpy(body + sizeof(h), title, tl);
    memcpy(body + sizeof(h) + tl, sub, sl);
    uint8_t ext[NET_EXT_HEADER_SIZE + sizeof(body)];
    const uint16_t n = net_ext_pack(ext, sizeof(ext), NET_EXT_KN_OFFICIAL_TEXT_POPUP, body, (uint16_t) (sizeof(h) + tl + sl));
    if (n) krunker_link_emit(l, NET_PACKET_EXT, ext, n);
}

static void text_popup(KrunkerLink *l, const KrunkerValue *title, const KrunkerValue *text, const KrunkerValue *silent,
                       const KrunkerValue *sub_size) {
    char t[160], s[160];
    text_of(title, t, sizeof(t));
    text_of(text, s, sizeof(s));
    text_popup_strings(l, t, s, truthy(silent), (int) num(sub_size, 0));
}

// gmsg's countdown n (truthy only): cntd_<n> + timerText (:220406)
static void countdown(KrunkerLink *l, const KrunkerValue *n) {
    if (!truthy(n)) return;
    const NetOfficialCountdownPacket c = {.n = (int32_t) num(n, 0)};
    uint8_t ext[NET_EXT_HEADER_SIZE + sizeof(c)];
    const uint16_t len = net_ext_pack(ext, sizeof(ext), NET_EXT_KN_OFFICIAL_COUNTDOWN, &c, sizeof(c));
    if (len) krunker_link_emit(l, NET_PACKET_EXT, ext, len);
}

// MM:SS of a ms count (iîìiiiì :23893 without its flags)
static void mm_ss(const double ms, char *out, const size_t n) {
    const long t = ms > 0 ? (long) ms : 0;
    snprintf(out, n, "%02ld:%02ld", (t / 60000) % 60, (t / 1000) % 60);
}

static void value(KrunkerLink *l, const int type, const KrunkerValue *key, const KrunkerValue *v, const uint8_t flags) {
    NetValuePacket p;
    memset(&p, 0, sizeof(p));
    p.flags = flags;
    text_of(key, p.key, sizeof(p.key));
    if (!p.key[0] && !(flags & NET_VALUE_RESET)) return;
    p.value = krunker_is(v, KRUNKER_BOOL) ? (double) v->u.boolean : num(v, 0);
    EMIT(type, p);
}

// ---------------------------------------------------------------- dispatch

int krunker_link_world_frame(KrunkerLink *l, const KrunkerS2C id, const KrunkerFrame *f) {
    switch (id) {
        // Domination
        case KRUNKER_S2C_UPDATE_DOM_ZONE: on_dom_update(l, arg(f, 0)); return 1;
        case KRUNKER_S2C_CAPTURE_CONTROL: { // "cct" [uid, team] (iiîïìií :220902): captured, "Zone X Captured"
            if (!krunker_is(arg(f, 0), KRUNKER_NUM)) return 1;
            const NetDomCapturePacket p = {.uid = (int32_t) arg(f, 0)->u.num, .team = (int32_t) num(arg(f, 1), 0)};
            EMIT(NET_PACKET_DOM_CAPTURE, p);
            return 1;
        }
        // spawnables (KC / Deposit tags, pickups, Loot Drops)
        case KRUNKER_S2C_CREATE_SPAWNABLE: spawnable(l, f->args, f->argc); return 1;
        case KRUNKER_S2C_DISPOSE_SPAWNABLE: { // "dsp" [sid] (disposeSpawnable :188921)
            NetSpawnableV7Packet p;
            memset(&p, 0, sizeof(p));
            p.sid = (int32_t) num(arg(f, 0), -1);
            p.data_w = -1;
            EMIT(NET_PACKET_SPAWNABLE_V7, p);
            return 1;
        }
        // world objects (map order uids, as our map loader keeps them)
        case KRUNKER_S2C_DESTROY_GAMEOBJECT: object_state(l, arg(f, 0), NET_OBJECT_DESTROY, 0); return 1;  // destroyObj :187822
        case KRUNKER_S2C_RESPAWN_OBJ: object_state(l, arg(f, 0), NET_OBJECT_RESPAWN, 0); return 1;         // respawnObj :188640
        case KRUNKER_S2C_UPDATE_GATE: object_state(l, arg(f, 0), NET_OBJECT_GATE, truthy(arg(f, 1))); return 1; // updateGate :189383
        case KRUNKER_S2C_UPDATE_BANK: object_state(l, arg(f, 0), NET_OBJECT_BANK, (int32_t) num(arg(f, 1), 0)); return 1; // updateBank :189370
        case KRUNKER_S2C_TELEPORT_TO_NODE: object_state(l, arg(f, 0), NET_OBJECT_MOVE_TO_NODE, (int32_t) num(arg(f, 1), 0)); return 1; // :187875
        case KRUNKER_S2C_TELEPORT_FROM_NODE: object_state(l, arg(f, 0), NET_OBJECT_MOVE_FROM_NODE, 0); return 1; // :187952
        case KRUNKER_S2C_BUILD_OBJECT: build(l, f->args, f->argc, 0); return 1;
        // airdrops ("ad" [[sid, fallDist, x, y, z, dropIndex]], airDrop.cSync :178967: the owner is not sent)
        case KRUNKER_S2C_SYNC_AIRDROP: {
            const KrunkerValue *d = arg(f, 0);
            if (!krunker_is(d, KRUNKER_ARRAY) || d->len < 6) return 1;
            NetAirDropPacket p;
            memset(&p, 0, sizeof(p));
            p.sid = (int32_t) num(krunker_at(d, 0), 0);
            p.fall = (float) num(krunker_at(d, 1), 0);
            for (uint32_t k = 0; k < 3; k++) p.position[k] = (float) num(krunker_at(d, 2 + k), 0);
            p.index = (uint8_t) num(krunker_at(d, 5), 0);
            p.owner_uid = -1;
            EMIT(NET_PACKET_AIRDROP, p);
            return 1;
        }
        case KRUNKER_S2C_REMOVE_AIRDROP_BY_SID: {
            const NetAirDropRemovePacket p = {.sid = (int32_t) num(arg(f, 0), 0)};
            EMIT(NET_PACKET_AIRDROP_REMOVE, p);
            return 1;
        }
        case KRUNKER_S2C_KR_PACKAGE: { // "krp" [owner, claimer] (iiîïíîi :220642): "claimed a package" / "stole"
            const NetAirDropClaimPacket p = {.owner_uid = (int32_t) num(arg(f, 0), -1), .claimer_uid = (int32_t) num(arg(f, 1), -1)};
            EMIT(NET_PACKET_AIRDROP_CLAIM, p);
            return 1;
        }
        // AI
        case KRUNKER_S2C_SYNC_ALL_AI: ai_sync(l, arg(f, 0)); return 1;
        case KRUNKER_S2C_C_SPAWN: ai_add(l, arg(f, 0)); return 1;
        case KRUNKER_S2C_REMOVE_BY_SID: {
            const NetAIRemovePacket p = {.sid = (int32_t) num(arg(f, 0), -1)};
            EMIT(NET_PACKET_AI_REMOVE, p);
            return 1;
        }
        // speed runs: "gt" [time ms, timeLog] (game.logTime :189497)
        case KRUNKER_S2C_LOG_TIME: {
            const GameModeConfig *mode = mode_config(l->mode);
            NetLogTimePacket p;
            memset(&p, 0, sizeof(p));
            p.time_ms = (int32_t) num(arg(f, 0), 0);
            p.time_log = (int32_t) num(arg(f, 1), 0);
            p.kind = NET_LOG_TIME_ZONE;
            p.show_ms = (uint8_t) (mode && mode->show_ms);
            EMIT(NET_PACKET_LOG_TIME, p);
            return 1;
        }
        // messages
        case KRUNKER_S2C_UPDATE_GAME_MESSAGE:
            countdown(l, arg(f, 2)); // the countdown first, then the popup, then the message (v10's order)
            if (krunker_is(arg(f, 3), KRUNKER_ARRAY)) text_popup(l, krunker_at(arg(f, 3), 0), krunker_at(arg(f, 3), 1), NULL, NULL);
            game_message(l, arg(f, 0), arg(f, 1));
            return 1;
        case KRUNKER_S2C_TEXT_POPUP: text_popup(l, arg(f, 0), arg(f, 1), arg(f, 2), arg(f, 3)); return 1; // "tx" [title, sub, silent, size] :220929
        case KRUNKER_S2C_STALKER_PICKED: // "stlkpd" (iiîîíìî :218088): to everyone but the stalker
            text_popup_strings(l, "You are the Living", "Hunt the stalker and survive the round!", 0, 0);
            return 1;
        case KRUNKER_S2C_UPDATE_LOCKOUT_TIM: { // "lkt" [ms] (iïìîiîí :220401): gmsg("warmup starts in ", MM:SS)
            const NetLockoutPacket p = {.ms = (int32_t) num(arg(f, 0), 0)};
            EMIT(NET_PACKET_LOCKOUT, p);
            char t[16];
            mm_ss(p.ms, t, sizeof(t));
            const KrunkerValue msg = KRUNKER_V_STR("warmup starts in "), suffix = KRUNKER_V_STR(t);
            game_message(l, &msg, &suffix);
            return 1;
        }
        case KRUNKER_S2C_CHECK_POINT_SET: { // "chp" [reset] (iïiíììì :220934)
            const NetCheckpointPacket p = {.reset = (uint8_t) truthy(arg(f, 0))};
            EMIT(NET_PACKET_CHECKPOINT, p);
            return 1;
        }
        // zombies
        case KRUNKER_S2C_UPDATE_ZOMBIE_ROUND: { // "upz" [wave, special] (iííiìïi): "Round N"
            NetZombieWavePacket p;
            memset(&p, 0, sizeof(p));
            p.wave = (int32_t) num(arg(f, 0), 0);
            text_of(arg(f, 1), p.special, sizeof(p.special));
            EMIT(NET_PACKET_ZOMBIE_WAVE, p);
            return 1;
        }
        case KRUNKER_S2C_UPDATE_POWER_UP: powerup(l, (int) num(arg(f, 0), -1), arg(f, 1)); return 1; // "pwup" [index, ms] :220438
        case KRUNKER_S2C_UPDATE_MYSTERY_BOX: zombie_machine(l, ZOMBIE_MACHINE_MYSTERY_BOX, f->args, f->argc); return 1;
        case KRUNKER_S2C_UPDATE_KRUM_MACHINE: zombie_machine(l, ZOMBIE_MACHINE_KRUM, f->args, f->argc); return 1;
        case KRUNKER_S2C_SWITCH_MYSTERY_BOX: { // "smybx" [uid, enable] (switchMysteryBox :189049)
            const NetMysteryBoxSwitchPacket p = {.uid = (int32_t) num(arg(f, 0), -1), .enable = (uint8_t) truthy(arg(f, 1))};
            EMIT(NET_PACKET_MYSTERY_BOX_SWITCH, p);
            return 1;
        }
        // loadouts / pickups
        case KRUNKER_S2C_PLAYER_INTERACTIONS: on_interactions(l, f); return 1;
        case KRUNKER_S2C_ALL_PLAYER_INTERACTIONS: // "inatA" [weapons, loadoutIndex] (iîìiïiì :220082): everyone
            for (int i = 0; i < l->player_count; i++) loadout(l, l->players[i].sid, arg(f, 0), arg(f, 1), NULL);
            return 1;
        // map triggers
        case KRUNKER_S2C_CUSTOM_VAL: value(l, NET_PACKET_CUSTOM_VALUE, arg(f, 0), arg(f, 1), 0); return 1;      // you.custVals :220517
        case KRUNKER_S2C_UPDATE_GLOBAL_VALUE: value(l, NET_PACKET_GLOBAL_VALUE, arg(f, 0), arg(f, 1), 0); return 1; // globalVals :220068
        case KRUNKER_S2C_TOGGLE_ELEMENT: { // "tgui" [id, status 0 toggle / 1 show / 2 hide] :220005
            const NetGuiTogglePacket p = {.id = (int32_t) num(arg(f, 0), 0), .status = (uint8_t) num(arg(f, 1), 0)};
            EMIT(NET_PACKET_GUI_TOGGLE, p);
            return 1;
        }
        case KRUNKER_S2C_CONFIRM_INTERACTION: { // "cnfm" [uid | null, kind] :220523: pendingSpin (2) / pendingTrigger
            const NetConfirmPacket p = {.uid = krunker_is(arg(f, 0), KRUNKER_NUM) ? (int32_t) arg(f, 0)->u.num : -1,
                                        .kind = (uint8_t) (num(arg(f, 1), 0) == 2 ? 2 : 0)};
            EMIT(NET_PACKET_CONFIRM_INTERACT, p);
            return 1;
        }
        default: return 0;
    }
}

// ---------------------------------------------------------------- init (a joiner / a new round)

void krunker_link_world_init(KrunkerLink *l, const KrunkerFrame *f) {
    const KrunkerValue *sync = arg(f, 11);
    // the round counters and lockout: init arg 3 roundWins [[team, wins]*] (:219535) and syncData
    // roundC / lockT; team scores (arg 4) and the objective ride along so nothing is zeroed
    NetModeSyncV7Packet ms;
    memset(&ms, 0, sizeof(ms));
    const KrunkerValue *scores = arg(f, 4);
    for (uint32_t i = 0; krunker_is(scores, KRUNKER_ARRAY) && i < scores->len; i++) {
        const int t = (int) num(krunker_at(&scores->u.items[i], 0), 0);
        if (t > 0 && t < MODE_TEAMS) ms.team_scores[t] = (int32_t) num(krunker_at(&scores->u.items[i], 1), 0);
    }
    const KrunkerValue *wins = arg(f, 3);
    ms.has_round_wins = (uint8_t) krunker_is(wins, KRUNKER_ARRAY);
    for (uint32_t i = 0; ms.has_round_wins && i < wins->len; i++) {
        const int t = (int) num(krunker_at(&wins->u.items[i], 0), 0);
        if (t > 0 && t < MODE_TEAMS) ms.team_round_wins[t] = (int32_t) num(krunker_at(&wins->u.items[i], 1), 0);
    }
    ms.rounds_left = (int32_t) num(krunker_get(sync, "roundC"), 0);
    ms.lockout_ms = (int32_t) num(krunker_get(sync, "lockT"), 0);
    const KrunkerValue *obj = krunker_get(sync, "obj");
    const GameModeConfig *mode = mode_config(l->mode);
    ms.objective_uid = krunker_is(krunker_at(obj, 0), KRUNKER_NUM) && !(mode && mode->obj_weapon) ? (int32_t) krunker_at(obj, 0)->u.num : -1;
    ms.objective_ms = (int32_t) num(krunker_at(obj, 1), 0);
    if (krunker_is(sync, KRUNKER_MAP) || ms.has_round_wins) EMIT(NET_PACKET_MODE_SYNC_V7, ms);
    if (!krunker_is(sync, KRUNKER_MAP)) return;

    // the order of initGame (:219395-219470)
    const KrunkerValue *v = krunker_get(sync, "dest");
    for (uint32_t i = 0; krunker_is(v, KRUNKER_ARRAY) && i < v->len; i++) object_state(l, krunker_at(v, i), NET_OBJECT_DESTROY, 0);
    v = krunker_get(sync, "mov");
    for (uint32_t i = 0; krunker_is(v, KRUNKER_ARRAY) && i + 1 < v->len; i += 2)
        object_state(l, krunker_at(v, i), NET_OBJECT_MOVE_TO_NODE, (int32_t) num(krunker_at(v, i + 1), 0));
    v = krunker_get(sync, "spwns");
    for (uint32_t i = 0; krunker_is(v, KRUNKER_ARRAY) && i < v->len; i++) {
        const KrunkerValue *e = krunker_at(v, i);
        if (krunker_is(e, KRUNKER_ARRAY)) spawnable(l, e->u.items, e->len);
    }
    v = krunker_get(sync, "pkups"); // updatePickup.apply(pkups[i]): [uid, weapon, orgPickup, active, pos, ammo]
    for (uint32_t i = 0; krunker_is(v, KRUNKER_ARRAY) && i < v->len; i++) {
        const KrunkerValue *e = krunker_at(v, i);
        if (krunker_is(e, KRUNKER_ARRAY)) pickup(l, krunker_at(e, 0), krunker_at(e, 1), krunker_at(e, 3), krunker_at(e, 4), krunker_at(e, 5));
    }
    v = krunker_get(sync, "gates");
    for (uint32_t i = 0; krunker_is(v, KRUNKER_ARRAY) && i + 1 < v->len; i += 2)
        object_state(l, krunker_at(v, i), NET_OBJECT_GATE, truthy(krunker_at(v, i + 1)));
    v = krunker_get(sync, "banks");
    for (uint32_t i = 0; krunker_is(v, KRUNKER_ARRAY) && i + 1 < v->len; i += 2)
        object_state(l, krunker_at(v, i), NET_OBJECT_BANK, (int32_t) num(krunker_at(v, i + 1), 0));
    v = krunker_get(sync, "dyno"); // buildObj.apply(dyno[i].concat([true])): centred
    for (uint32_t i = 0; krunker_is(v, KRUNKER_ARRAY) && i < v->len; i++) {
        const KrunkerValue *e = krunker_at(v, i);
        if (krunker_is(e, KRUNKER_ARRAY)) build(l, e->u.items, e->len, 1);
    }
    v = krunker_get(sync, "gbl"); // the join's global values: the first clears the client's set
    for (uint32_t i = 0; krunker_is(v, KRUNKER_ARRAY) && i + 1 < v->len; i += 2)
        value(l, NET_PACKET_GLOBAL_VALUE, krunker_at(v, i), krunker_at(v, i + 1), i == 0 ? NET_VALUE_RESET : 0);
    v = krunker_get(sync, "doms"); // updateDomZone(uid, capturedBy): pairs
    if (krunker_is(v, KRUNKER_ARRAY) && v->len >= 2) {
        NetDomUpdatePacket p;
        memset(&p, 0, sizeof(p));
        for (uint32_t i = 0; i + 1 < v->len && p.count < MODE_MAX_DOM_ZONES; i += 2)
            dom_entry(&p.zones[p.count++], krunker_at(v, i), krunker_at(v, i + 1), NULL, NULL, NULL, NULL);
        EMIT(NET_PACKET_DOM_UPDATE, p);
    }
    v = krunker_get(sync, "mybx");
    for (uint32_t i = 0; krunker_is(v, KRUNKER_ARRAY) && i + 6 <= v->len; i += 6) zombie_machine(l, ZOMBIE_MACHINE_MYSTERY_BOX, v->u.items + i, 6);
    v = krunker_get(sync, "krm");
    for (uint32_t i = 0; krunker_is(v, KRUNKER_ARRAY) && i + 6 <= v->len; i += 6) zombie_machine(l, ZOMBIE_MACHINE_KRUM, v->u.items + i, 6);
    v = krunker_get(sync, "pwup"); // [instaKill, fireSale, doublePoints] ms
    for (uint32_t i = 0; krunker_is(v, KRUNKER_ARRAY) && i < v->len; i++) powerup(l, (int) i, krunker_at(v, i));
}
