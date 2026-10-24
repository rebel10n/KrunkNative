// Official Krunker session: KrunkScript entities, runtime blocks, moved objects and the script's
// player locks (client/src/net/krunker_link_internal.h). Pure translation, like krunker_link.c: each
// official record becomes the EXT client/src/entities.c consumes. Records whose body is free-form
// JSON (an entity's state, a block's data) travel as JSON text (NetExtJsonChunk slices, net_ext.h).
// Line numbers: ~/.cache/kn-official/game.deob.js (10.0.3; v10.0.2 has the same entity system).
//
// "pz" PLAYER_POSE [sid, pose | 0] (players.setPose deob-10.0.3.js:195354) and init's syncData.plrPose
// [[sid, pose], ...] (:256989-256993) become NET_EXT_PLAYER_POSE (player_pose_render.c applies them).
//
// Not translated (documented gaps):
//   - "ee" ENTITY_EVENT is forwarded, but its only consumer in v10 is the map's client / shared
//     KrunkScript (clientEventQueue -> onEntityEvent hooks), which our client does not run.
#include "krunker_link_internal.h"
#include <net_ext.h>
#include <player_pose.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

static const KrunkerValue *arg(const KrunkerFrame *f, const uint32_t i) { return krunker_frame_arg(f, i); }

// One JSON record (`v` printed as JSON) as NetExtJsonChunk slices of `subtype`, in order.
static void emit_json(KrunkerLink *l, const uint16_t subtype, const KrunkerValue *v) {
    KrunkerBuf buf = {0};
    if (!v || krunker_value_to_json(v, &buf) != KRUNKER_PROTO_OK || !buf.data || !buf.len) {
        krunker_buf_free(&buf);
        return;
    }
    const size_t parts = (buf.len + NET_EXT_JSON_CHUNK_DATA - 1) / NET_EXT_JSON_CHUNK_DATA;
    if (parts > 255) { // never a valid record (states are at most 512 bytes): dropped whole
        krunker_buf_free(&buf);
        return;
    }
    uint8_t ext[NET_EXT_HEADER_SIZE + NET_EXT_BODY_MAX];
    uint8_t body[NET_EXT_BODY_MAX];
    for (size_t p = 0; p < parts; p++) {
        const size_t off = p * NET_EXT_JSON_CHUNK_DATA;
        const size_t n = buf.len - off < (size_t) NET_EXT_JSON_CHUNK_DATA ? buf.len - off : (size_t) NET_EXT_JSON_CHUNK_DATA;
        const NetExtJsonChunk head = {(uint8_t) p, (uint8_t) parts};
        memcpy(body, &head, sizeof(head));
        memcpy(body + sizeof(head), buf.data + off, n);
        const uint16_t len = net_ext_pack(ext, sizeof(ext), subtype, body, (uint16_t) (sizeof(head) + n));
        if (len) krunker_link_emit(l, NET_PACKET_EXT, ext, len);
    }
    krunker_buf_free(&buf);
}

// The frame's arguments as one JSON array (a view: the values stay the frame's).
static void emit_args(KrunkerLink *l, const uint16_t subtype, const KrunkerFrame *f) {
    const KrunkerValue all = KRUNKER_V_ARRAY((KrunkerValue *) f->args, f->argc);
    emit_json(l, subtype, &all);
}

// Every element of a list argument as its own record.
static void emit_each(KrunkerLink *l, const uint16_t subtype, const KrunkerValue *list) {
    for (uint32_t i = 0; krunker_is(list, KRUNKER_ARRAY) && i < list->len; i++) {
        const KrunkerValue *item = krunker_at(list, i);
        if (krunker_is(item, KRUNKER_ARRAY)) emit_json(l, subtype, item);
    }
}

// "objmv" OBJECT_MOVED [uid, x, y, z] (broadcast :191671, game.moveObject :187893); also init's syncData.chg
static void emit_moved(KrunkerLink *l, const KrunkerValue *uid, const KrunkerValue *x, const KrunkerValue *y, const KrunkerValue *z) {
    if (!krunker_is(uid, KRUNKER_NUM) || !krunker_is(x, KRUNKER_NUM) || !krunker_is(y, KRUNKER_NUM) || !krunker_is(z, KRUNKER_NUM)) return;
    const NetObjectMovedPacket m = {(int32_t) uid->u.num, (float) x->u.num, (float) y->u.num, (float) z->u.num};
    uint8_t ext[NET_EXT_HEADER_SIZE + sizeof(m)];
    const uint16_t len = net_ext_pack(ext, sizeof(ext), NET_EXT_KN_OFFICIAL_OBJECT_MOVED, &m, sizeof(m));
    if (len) krunker_link_emit(l, NET_PACKET_EXT, ext, len);
}

// The official pose (the server's sanitizePose output, :220978-221034) as a NetPlayerPosePacket; the same
// limits are applied again (the client's setPose takes the object as it is, a struct cannot)
static void emit_pose(KrunkerLink *l, const KrunkerValue *sid, const KrunkerValue *pose) {
    if (!krunker_is(sid, KRUNKER_NUM)) return;
    NetPlayerPosePacket p;
    memset(&p, 0, sizeof(p));
    p.uid = (int32_t) sid->u.num;
    if (krunker_is(pose, KRUNKER_MAP)) { // pose || 0: anything else clears it
        const KrunkerValue *v;
        if ((v = krunker_get(pose, "legs")) && !krunker_is(v, KRUNKER_NIL)) {
            const double legs = floor(krunker_num(v, 0) + 0.5);
            p.legs = (uint8_t) (legs < 0 ? 0 : legs > 2 ? 2 : legs);
            p.has |= PLAYER_POSE_LEGS;
        }
        static const struct { const char *key; int bit; size_t offset; } flags[] = {
            {"weapon", PLAYER_POSE_WEAPON, offsetof(NetPlayerPosePacket, weapon)},
            {"bob", PLAYER_POSE_BOB, offsetof(NetPlayerPosePacket, bob)},
            {"faceRide", PLAYER_POSE_FACE_RIDE, offsetof(NetPlayerPosePacket, face_ride)},
            {"hold", PLAYER_POSE_HOLD, offsetof(NetPlayerPosePacket, hold)},
            {"holdL", PLAYER_POSE_HOLD_L, offsetof(NetPlayerPosePacket, hold_l)},
        };
        for (size_t i = 0; i < sizeof(flags) / sizeof(flags[0]); i++) {
            if (!(v = krunker_get(pose, flags[i].key)) || krunker_is(v, KRUNKER_NIL)) continue;
            *((uint8_t *) &p + flags[i].offset) = (uint8_t) (krunker_num(v, krunker_is(v, KRUNKER_STR) ? 1 : 0) != 0);
            p.has |= (uint16_t) flags[i].bit;
        }
        static const struct { const char *key; int bit; float max; size_t offset; } numbers[] = {
            {"legX", PLAYER_POSE_LEG_X, PLAYER_POSE_LEG_X_MAX, offsetof(NetPlayerPosePacket, leg_x)},
            {"bodyY", PLAYER_POSE_BODY_Y, PLAYER_POSE_BODY_Y_MAX, offsetof(NetPlayerPosePacket, body_y)},
            {"torsoX", PLAYER_POSE_TORSO_X, PLAYER_POSE_TORSO_X_MAX, offsetof(NetPlayerPosePacket, torso_x)},
        };
        for (size_t i = 0; i < sizeof(numbers) / sizeof(numbers[0]); i++) {
            float value;
            if (!krunker_is(v = krunker_get(pose, numbers[i].key), KRUNKER_NUM) || !player_pose_clamp(v->u.num, numbers[i].max, &value)) continue;
            memcpy((uint8_t *) &p + numbers[i].offset, &value, sizeof(value));
            p.has |= (uint16_t) numbers[i].bit;
        }
        for (int side = 0; side < 2; side++) {
            const KrunkerValue *arm = krunker_get(pose, side ? "armR" : "armL");
            if (!krunker_is(arm, KRUNKER_ARRAY)) continue;
            float *dst = side ? p.arm_r : p.arm_l;
            for (uint32_t i = 0; i < 3; i++) {
                const KrunkerValue *a = krunker_at(arm, i);
                if (!krunker_is(a, KRUNKER_NUM) || !player_pose_clamp(a->u.num, PLAYER_POSE_ARM_MAX, &dst[i])) dst[i] = 0.0f;
            }
            p.has |= side ? PLAYER_POSE_ARM_R : PLAYER_POSE_ARM_L;
        }
    }
    uint8_t ext[NET_EXT_HEADER_SIZE + sizeof(p)];
    const uint16_t len = net_ext_pack(ext, sizeof(ext), NET_EXT_PLAYER_POSE, &p, sizeof(p));
    if (len) krunker_link_emit(l, NET_PACKET_EXT, ext, len);
}

int krunker_link_script_frame(KrunkerLink *l, const KrunkerS2C id, const KrunkerFrame *f) {
    switch (id) {
        case KRUNKER_S2C_ENTITY_SPAWN: // entities.applySpawn(payload) (:222400)
            if (krunker_is(arg(f, 0), KRUNKER_ARRAY)) emit_json(l, NET_EXT_ENTITY_SPAWN, arg(f, 0));
            return 1;
        case KRUNKER_S2C_ENTITY_REMOVE: // applyRemove(eid, reason) (:222403)
            emit_args(l, NET_EXT_ENTITY_REMOVE, f);
            return 1;
        case KRUNKER_S2C_ENTITY_STATE: // applyBaselines([[eid, tick, state], ...]) (:222405)
            emit_each(l, NET_EXT_ENTITY_STATE, arg(f, 0));
            return 1;
        case KRUNKER_S2C_ENTITY_EVENT: // clientEventQueue.push({eid, name, a, b}) (:222410)
            emit_args(l, NET_EXT_ENTITY_EVENT, f);
            return 1;
        case KRUNKER_S2C_ENTITY_ATTACH: // applyAttach(sid, eid, opts, sid == you) (:222417)
            emit_args(l, NET_EXT_ENTITY_ATTACH, f);
            return 1;
        case KRUNKER_S2C_ENTITY_DETACH: // applyDetach(sid, eid) (:222419)
            emit_args(l, NET_EXT_ENTITY_DETACH, f);
            return 1;
        case KRUNKER_S2C_SYNC_NEW_OBJECTS: // liveObjects.syncNewObjects([[id, data], ...]) (:183977)
            emit_each(l, NET_EXT_KN_OFFICIAL_LIVE_OBJECT, arg(f, 0));
            return 1;
        case KRUNKER_S2C_SYNC_LIVE_OBJECTS: // liveObjects.syncLiveObjects([[id, "remove"], ...]) (:184004)
            emit_each(l, NET_EXT_KN_OFFICIAL_LIVE_SYNC, arg(f, 0));
            return 1;
        case KRUNKER_S2C_OBJECT_MOVED:
            emit_moved(l, arg(f, 0), arg(f, 1), arg(f, 2), arg(f, 3));
            return 1;
        case KRUNKER_S2C_PLAYER_POSE: // players.setPose(sid, pose) (:261743, :261943)
            emit_pose(l, arg(f, 0), arg(f, 1));
            return 1;
        case KRUNKER_S2C_PLAYER_LOCKS: { // you.disableShooting = !!a, disableMelee = !!b (:220492)
            const NetPlayerLocksPacket p = {(uint8_t) (krunker_num(arg(f, 0), 0) != 0 || (krunker_is(arg(f, 0), KRUNKER_BOOL) && arg(f, 0)->u.boolean)),
                                            (uint8_t) (krunker_num(arg(f, 1), 0) != 0 || (krunker_is(arg(f, 1), KRUNKER_BOOL) && arg(f, 1)->u.boolean))};
            uint8_t ext[NET_EXT_HEADER_SIZE + sizeof(p)];
            const uint16_t len = net_ext_pack(ext, sizeof(ext), NET_EXT_PLAYER_LOCKS, &p, sizeof(p));
            if (len) krunker_link_emit(l, NET_PACKET_EXT, ext, len);
            return 1;
        }
        default:
            return 0;
    }
}

// "l" SYNC_PLAYER's last element: the entity we control, [eid, tick, encodeState(state)] or 0 (syncState,
// deob-10.0.3.js:222404-222405), applied by entities.applyControlledSync (:213849-213859: the state and tick as they
// are, commitPose, placeBody; no blend, unlike a baseline) -> NET_EXT_ENTITY_CONTROLLED (client/src/entities.c)
void krunker_link_script_controlled(KrunkerLink *l, const KrunkerValue *controlled) {
    if (krunker_is(controlled, KRUNKER_ARRAY) && controlled->len >= 1) emit_json(l, NET_EXT_ENTITY_CONTROLLED, controlled);
}

// init's syncData (arg 11) parts of these (:219478-219496): lo (live blocks), ent (entity spawn
// payloads, applySyncData), entAtt ([sid, eid, opts] attachments), chg ([uid, x, y, z] moved objects)
void krunker_link_script_init(KrunkerLink *l, const KrunkerFrame *f) {
    const KrunkerValue *sync = arg(f, 11);
    if (!krunker_is(sync, KRUNKER_MAP)) return;
    emit_each(l, NET_EXT_KN_OFFICIAL_LIVE_OBJECT, krunker_get(sync, "lo"));
    emit_each(l, NET_EXT_ENTITY_SPAWN, krunker_get(sync, "ent"));
    emit_each(l, NET_EXT_ENTITY_ATTACH, krunker_get(sync, "entAtt"));
    const KrunkerValue *poses = krunker_get(sync, "plrPose"); // [[sid, poseData], ...] (:220294-220302)
    for (uint32_t i = 0; krunker_is(poses, KRUNKER_ARRAY) && i < poses->len; i++) {
        const KrunkerValue *e = krunker_at(poses, i);
        if (krunker_is(e, KRUNKER_ARRAY)) emit_pose(l, krunker_at(e, 0), krunker_at(e, 1));
    }
    const KrunkerValue *chg = krunker_get(sync, "chg");
    for (uint32_t i = 0; krunker_is(chg, KRUNKER_ARRAY) && i < chg->len; i++) {
        const KrunkerValue *c = krunker_at(chg, i);
        if (krunker_is(c, KRUNKER_ARRAY)) emit_moved(l, krunker_at(c, 0), krunker_at(c, 1), krunker_at(c, 2), krunker_at(c, 3));
    }
}
