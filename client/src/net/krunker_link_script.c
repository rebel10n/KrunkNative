// Official Krunker session: KrunkScript entities, runtime blocks, moved objects and the script's
// player locks (client/src/net/krunker_link_internal.h). Pure translation, like krunker_link.c: each
// official record becomes the EXT client/src/entities.c consumes. Records whose body is free-form
// JSON (an entity's state, a block's data) travel as JSON text (NetExtJsonChunk slices, net_ext.h).
// Line numbers: ~/.cache/kn-official/game.deob.js (10.0.3; v10.0.2 has the same entity system).
//
// Not translated (documented gaps):
//   - "pz" PLAYER_POSE (players.setPose :172087): seat / skeletal poses of a player model; our player
//     meshes have no pose layer.
//   - "ee" ENTITY_EVENT is forwarded, but its only consumer in v10 is the map's client / shared
//     KrunkScript (clientEventQueue -> onEntityEvent hooks), which our client does not run.
#include "krunker_link_internal.h"
#include <net_ext.h>
#include <math.h>
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

// init's syncData (arg 11) parts of these (:219478-219496): lo (live blocks), ent (entity spawn
// payloads, applySyncData), entAtt ([sid, eid, opts] attachments), chg ([uid, x, y, z] moved objects)
void krunker_link_script_init(KrunkerLink *l, const KrunkerFrame *f) {
    const KrunkerValue *sync = arg(f, 11);
    if (!krunker_is(sync, KRUNKER_MAP)) return;
    emit_each(l, NET_EXT_KN_OFFICIAL_LIVE_OBJECT, krunker_get(sync, "lo"));
    emit_each(l, NET_EXT_ENTITY_SPAWN, krunker_get(sync, "ent"));
    emit_each(l, NET_EXT_ENTITY_ATTACH, krunker_get(sync, "entAtt"));
    const KrunkerValue *chg = krunker_get(sync, "chg");
    for (uint32_t i = 0; krunker_is(chg, KRUNKER_ARRAY) && i < chg->len; i++) {
        const KrunkerValue *c = krunker_at(chg, i);
        if (krunker_is(c, KRUNKER_ARRAY)) emit_moved(l, krunker_at(c, 0), krunker_at(c, 1), krunker_at(c, 2), krunker_at(c, 3));
    }
}
