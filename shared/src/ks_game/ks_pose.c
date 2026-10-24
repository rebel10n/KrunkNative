// KrunkScript player.pose (Krunker v10.0.3; shared/include/player_pose.h). Line numbers: deob-10.0.3.js.
//
//   set pose(v) (:208220-208224): the server's script only (the classIndex guard: game, isServer, io);
//       game.setPlayerPose(player, isObject(v) ? sanitize(v) : null): a non-object clears the pose.
//   get pose() (:208225-208231): sanitize(poseData), null without a pose; both sides (the client's poseData
//       is what "pz" carried).
//   game.sanitizePose (:220978-221034): only these keys, in this order, each when `!= null`:
//       legs    Math.max(0, Math.min(2, Math.round(parseFloat(v) || 0)))
//       weapon, bob, faceRide, hold, holdL   v ? 1 : 0
//       legX +-3.2, bodyY +-12, torsoX +-1.6   iîÍiïíi: parseFloat, finite or dropped, rounded to 1/1000, clamped
//       armL, armR   arrays only: [0..2] each iîÍiïíi(+-3.2) || 0 (a non-finite item is 0)
//   and an object with none of them is null (no pose).
#include "ks_game_internal.h"

#include <player_pose.h>

#include <math.h>
#include <stddef.h>
#include <string.h>

// parseFloat (shared/src/ks/ks_number.c; the ks library's own, like ks_to_float above it in
// ks_game_internal.h): NaN for what does not start with a number
double ks_parse_float(const char *s, size_t len);

// parseFloat(v) of a sanitized value: numbers as they are, strings parsed, arrays through their
// ToString ("1,2" -> 1), everything else (booleans, objects) NaN
static double pose_parse_float(KsScript *s, const KsValue v) {
    if (v.tag == KS_NUM) return v.as.num;
    if (v.tag == KS_STR || v.tag == KS_ARR) {
        char buf[64];
        const size_t n = ks_to_string(s, v, buf, sizeof(buf));
        return ks_parse_float(buf, n < sizeof(buf) ? n : sizeof(buf) - 1);
    }
    return NAN;
}

// JS ToBoolean of a sanitized value
static int pose_truthy(const KsValue v) {
    switch (v.tag) {
        case KS_UNDEF:
        case KS_NULL: return 0;
        case KS_BOOL: return v.as.b != 0;
        case KS_NUM: return v.as.num != 0.0 && !isnan(v.as.num);
        case KS_STR: {
            size_t n = 0;
            ks_str_utf8(v, &n);
            return n != 0;
        }
        default: return 1;
    }
}

static int pose_given(const KsValue v) { return v.tag != KS_UNDEF && v.tag != KS_NULL; } // v != null

static int ks_pose_from_value(KsScript *s, const KsValue v, PlayerPose *out) {
    memset(out, 0, sizeof(*out));
    if (v.tag != KS_OBJ) return 0; // isObject: a script object (a host wrapper carries no pose keys either)
    KsValue k = ks_obj_get(s, v, "legs");
    if (pose_given(k)) {
        double legs = pose_parse_float(s, k);
        if (isnan(legs)) legs = 0; // parseFloat(v) || 0
        legs = floor(legs + 0.5);  // Math.round
        out->legs = (uint8_t) (legs < 0 ? 0 : legs > 2 ? 2 : legs);
        out->has |= PLAYER_POSE_LEGS;
    }
    static const struct { const char *key; int bit; size_t offset; } flags[] = {
        {"weapon", PLAYER_POSE_WEAPON, offsetof(PlayerPose, weapon)},
        {"bob", PLAYER_POSE_BOB, offsetof(PlayerPose, bob)},
        {"faceRide", PLAYER_POSE_FACE_RIDE, offsetof(PlayerPose, face_ride)},
        {"hold", PLAYER_POSE_HOLD, offsetof(PlayerPose, hold)},
        {"holdL", PLAYER_POSE_HOLD_L, offsetof(PlayerPose, hold_l)}, // 10.0.3 (:221007-221009)
    };
    for (size_t i = 0; i < sizeof(flags) / sizeof(flags[0]); i++) {
        k = ks_obj_get(s, v, flags[i].key);
        if (!pose_given(k)) continue;
        *((uint8_t *) out + flags[i].offset) = (uint8_t) pose_truthy(k);
        out->has |= (uint16_t) flags[i].bit;
    }
    static const struct { const char *key; int bit; float max; size_t offset; } numbers[] = {
        {"legX", PLAYER_POSE_LEG_X, PLAYER_POSE_LEG_X_MAX, offsetof(PlayerPose, leg_x)},
        {"bodyY", PLAYER_POSE_BODY_Y, PLAYER_POSE_BODY_Y_MAX, offsetof(PlayerPose, body_y)},
        {"torsoX", PLAYER_POSE_TORSO_X, PLAYER_POSE_TORSO_X_MAX, offsetof(PlayerPose, torso_x)},
    };
    for (size_t i = 0; i < sizeof(numbers) / sizeof(numbers[0]); i++) {
        k = ks_obj_get(s, v, numbers[i].key); // iîÍiïíi(undefined) is null: parseFloat(undefined) is NaN
        float value;
        if (!pose_given(k) || !player_pose_clamp(pose_parse_float(s, k), numbers[i].max, &value)) continue;
        memcpy((uint8_t *) out + numbers[i].offset, &value, sizeof(value));
        out->has |= (uint16_t) numbers[i].bit;
    }
    static const char *const arms[2] = {"armL", "armR"};
    for (int side = 0; side < 2; side++) {
        k = ks_obj_get(s, v, arms[side]);
        if (k.tag != KS_ARR) continue; // Array.isArray
        float *dst = side ? out->arm_r : out->arm_l;
        for (size_t i = 0; i < 3; i++) {
            float value = 0.0f;
            if (i < ks_arr_len(k) && !player_pose_clamp(pose_parse_float(s, ks_arr_get(s, k, i)), PLAYER_POSE_ARM_MAX, &value)) value = 0.0f;
            dst[i] = value; // iîÍiïíi(...) || 0
        }
        out->has |= side ? PLAYER_POSE_ARM_R : PLAYER_POSE_ARM_L;
    }
    return out->has != 0; // Object.keys(pose).length ? pose : null
}

// The poses are stored as floats; a read gives back the 1/1000 sanitizePose rounded to (0.1, not the
// float's widening 0.10000000149...), which is what v10's double holds
static double pose_read(const float v) { return floor((double) v * 1000.0 + 0.5) / 1000.0; }

static KsValue ks_pose_to_value(KsScript *s, const PlayerPose *pose) {
    if (!pose || !pose->has) return ks_null();
    KsValue obj = ks_obj_new(s);
    ks_handle_pin(s, obj); // the arrays below allocate
    if (pose->has & PLAYER_POSE_LEGS) ks_obj_set(s, obj, "legs", ks_num(pose->legs));
    if (pose->has & PLAYER_POSE_WEAPON) ks_obj_set(s, obj, "weapon", ks_num(pose->weapon));
    if (pose->has & PLAYER_POSE_BOB) ks_obj_set(s, obj, "bob", ks_num(pose->bob));
    if (pose->has & PLAYER_POSE_FACE_RIDE) ks_obj_set(s, obj, "faceRide", ks_num(pose->face_ride));
    if (pose->has & PLAYER_POSE_HOLD) ks_obj_set(s, obj, "hold", ks_num(pose->hold));
    if (pose->has & PLAYER_POSE_HOLD_L) ks_obj_set(s, obj, "holdL", ks_num(pose->hold_l));
    if (pose->has & PLAYER_POSE_LEG_X) ks_obj_set(s, obj, "legX", ks_num(pose_read(pose->leg_x)));
    if (pose->has & PLAYER_POSE_BODY_Y) ks_obj_set(s, obj, "bodyY", ks_num(pose_read(pose->body_y)));
    if (pose->has & PLAYER_POSE_TORSO_X) ks_obj_set(s, obj, "torsoX", ks_num(pose_read(pose->torso_x)));
    for (int side = 0; side < 2; side++) {
        if (!(pose->has & (side ? PLAYER_POSE_ARM_R : PLAYER_POSE_ARM_L))) continue;
        const float *src = side ? pose->arm_r : pose->arm_l;
        KsValue arr = ks_arr_new(s);
        ks_obj_set(s, obj, side ? "armR" : "armL", arr); // reachable from the pinned object before it grows
        for (int i = 0; i < 3; i++) ks_arr_push(s, arr, ks_num(pose_read(src[i])));
    }
    ks_handle_unpin(s, obj);
    return obj;
}

KsValue ks_player_pose_get(KsScript *s, void *native, const uint32_t id) {
    (void) id;
    const Player *p = ks_player_of(native);
    if (!p) return ks_undef();
    return ks_pose_to_value(s, &p->pose); // poseData ? sanitize(poseData) : null
}

void ks_player_pose_set(KsScript *s, void *native, const uint32_t id, const KsValue v) {
    (void) id;
    KsRef *r = native;
    Player *p = ks_player_of(native);
    if (!p || !r || !r->kg || r->kg->side != KS_SIDE_SERVER || !game_is_authority(r->kg->game)) return;
    PlayerPose pose;
    const int has = ks_pose_from_value(s, v, &pose); // isObject(v) ? sanitize(v) : null, then sanitizePose
    game_set_player_pose(p->game, p, has ? &pose : NULL);
}
