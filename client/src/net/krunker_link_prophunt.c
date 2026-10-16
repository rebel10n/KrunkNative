// Official lobbies: Prop Hunt (shared/include/prop_hunt.h). Krunker's packets -> our EXTs and back:
//   up    [sid, propID, skip, propSwitches]  NET_EXT_PROP_UPDATE (iìiiìïí :221007; players.updateProp :171837)
//   prpR  [sid, propRot, propRotSaved, reset] NET_EXT_PROP_ROTATION (iîîììîi :221019)
//   0     [21] propID: players.add calls updateProp(player, propID || 1, true) for a Prop-team player
//         (:171957), the same NET_EXT_PROP_UPDATE right after its SPAWN (no switches: no chat line)
// Outbound: NET_EXT_PROP_PICK -> ["prp", propID] (:221439 / :221492), NET_EXT_PROP_ROTATE ->
// ["prpR"] / ["prpR", true] (:221441-221442). The taunt ("prpt") is krunker_link.c's NET_PACKET_TAUNT.
// "0"'s propID (player.add, [21]) is not in the layout yet: the spawn gets prop 1 (updateProp(propID || 1)).
#include "krunker_link_internal.h"
#include <modes.h>
#include <net_ext.h>
#include <string.h>

static double num(const KrunkerValue *v, const double def) { return krunker_num(v, def); }
static int is_nil(const KrunkerValue *v) { return !v || v->type == KRUNKER_NIL; }
static int truthy(const KrunkerValue *v) {
    if (is_nil(v)) return 0;
    if (v->type == KRUNKER_BOOL) return v->u.boolean;
    if (v->type == KRUNKER_NUM) return v->u.num != 0;
    return 1;
}

static void emit_ext(KrunkerLink *l, const uint16_t subtype, const void *body, const uint16_t length) {
    uint8_t buffer[NET_EXT_HEADER_SIZE + 64];
    const uint16_t n = net_ext_pack(buffer, sizeof(buffer), subtype, body, length);
    if (n) krunker_link_emit(l, NET_PACKET_EXT, buffer, n);
}

void krunker_link_prophunt_spawned(KrunkerLink *l, const int sid, const int team, const KrunkerValue *prop_id) {
    const GameModeConfig *mode = mode_config(l->mode);
    if (!mode || !mode->prop || team < 1 || team > 2 || !mode->team_options[team - 1] || strcmp(mode->team_options[team - 1], "Prop") != 0) return;
    const double id = num(prop_id, 0);
    const NetPropUpdatePacket u = {.uid = sid, .prop_id = (int16_t) (id > 0 ? id : 1), .skip = 0, .switches = -1};
    emit_ext(l, NET_EXT_PROP_UPDATE, &u, sizeof(u));
}

int krunker_link_prophunt_frame(KrunkerLink *l, const KrunkerS2C id, const KrunkerFrame *f) {
    switch (id) {
        case KRUNKER_S2C_UPDATE_PROP: { // "up" [sid, propID, skip, propSwitches]
            const KrunkerValue *sid = krunker_frame_arg(f, 0);
            if (!krunker_is(sid, KRUNKER_NUM)) return 1;
            const KrunkerValue *switches = krunker_frame_arg(f, 3);
            const NetPropUpdatePacket u = {.uid = (int32_t) sid->u.num, .prop_id = (int16_t) num(krunker_frame_arg(f, 1), 1),
                                           .skip = (uint8_t) truthy(krunker_frame_arg(f, 2)),
                                           .switches = (int8_t) (is_nil(switches) ? -1 : num(switches, -1))};
            emit_ext(l, NET_EXT_PROP_UPDATE, &u, sizeof(u));
            return 1;
        }
        case KRUNKER_S2C_UPDATE_PROP_ROT: { // "prpR" [sid, propRot, propRotSaved, reset]
            const KrunkerValue *sid = krunker_frame_arg(f, 0);
            if (!krunker_is(sid, KRUNKER_NUM)) return 1;
            const KrunkerValue *saved = krunker_frame_arg(f, 2);
            const NetPropRotationPacket r = {.uid = (int32_t) sid->u.num, .rot = (uint8_t) truthy(krunker_frame_arg(f, 1)),
                                             .saved_set = (uint8_t) krunker_is(saved, KRUNKER_NUM), .reset = (uint8_t) truthy(krunker_frame_arg(f, 3)),
                                             .saved = (float) num(saved, 0)};
            emit_ext(l, NET_EXT_PROP_ROTATION, &r, sizeof(r));
            return 1;
        }
        default: return 0;
    }
}

int krunker_link_prophunt_send(KrunkerLink *l, const uint16_t subtype, const uint8_t *body, const uint16_t length) {
    if (subtype == NET_EXT_PROP_PICK && length >= sizeof(NetPropPickPacket)) {
        NetPropPickPacket pick;
        memcpy(&pick, body, sizeof(pick));
        const KrunkerValue args[] = {KRUNKER_V_NUM(pick.prop_id)};
        return krunker_link_send(l, krunker_c2s_wire(KRUNKER_C2S_PICK_PROP), args, 1);
    }
    if (subtype == NET_EXT_PROP_ROTATE && length >= sizeof(NetPropRotatePacket)) {
        const KrunkerValue args[] = {KRUNKER_V_BOOL(1)};
        return krunker_link_send(l, krunker_c2s_wire(KRUNKER_C2S_PROP_ROTATION), args, body[0] ? 1 : 0);
    }
    return -1; // not a Prop Hunt subtype
}
