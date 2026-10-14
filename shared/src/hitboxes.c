#include <math.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <string.h>
#include <shared.h>
#include <hitboxes.h>
#include <ruleset.h>

// Krunker v10.0.2's posed hit boxes (shared/include/hitboxes.h; doc/v10/gameplay.md section 3).
// "v10:N" = line N of the renamed 10.0.2 bundle (~/.cache/kn-v10/v10.js, the same lines as the
// original ~/Downloads/v10.0.2.js). The THREE hierarchy v10 keeps per player (complexHitboxes):
//
//   hitboxHoldObj   pos (x, y + |6 sin(sV) stepAnim|, z), rot.y xD - 2 sin(sV) stepAnim
//    +- upperBodyHoldObj   pos.y legHeight - crouchDst cV, rot.x crouchLean cV + 0.5 yD
//    |   +- headHitbox     headScale cube, scale hurtboxScale x headMlt          headshot
//    |   +- torsoHitbox    chestWidth x (playerHeight - headScale - legHeight) x chestScale  torsoshot
//    |   +- armHoldObj     rot.x 0.5 yD, pos.y recoilAnimY recoilYM animMult + playerHeight - cameraHeight - legHeight
//    |       +- arms       the viewmodel sway pose (v10:117791-117823)
//    |           +- armLeftHolder / armRightHolder (rotation order YXZ): upper / connector / lower arm   torsoshot
//    +- legsHoldObj
//        +- LegRightHolder, LegLeftHolder (standing), crouchedLegRightHolder, crouchedLegLeftHolder   legshot
//
// Every box mesh is a BoxGeometry centred on its own origin, so its OBB (fromBox3 then
// applyMatrix4(matrixWorld), v10:117590-117607) is centred on the mesh's world position, with
// the world rotation's columns as axes and the geometry half size x the mesh scale. The meshes'
// own scale is the hurtbox scale (the head's also x headMlt); every holder has scale 1.
//
// Not ported (documented deviations):
//   - the arms' viewmodel sway reads client animation values off the live player (inspectX,
//     jumpRotM, leanAnim*, swapTweenR, landBobY(R), jumpBobY, bobAnimY/Z, recoilX/Z, recoilTween*,
//     recoilAnim, idleAnim, interProg): taken as 0 here (idleAnim 0 keeps its cos term). They move
//     the arm boxes by a few tenths of a unit, and an arm hit is a torsoshot like the torso.
//   - v10 caches the arm boxes per player id and keeps the last weapon's arms when the state has
//     none; a player whose states never held a weapon is not hittable at all there ([null, ''],
//     v10:117841). Here a state without a weapon has no arm boxes and the rest still traces.

typedef struct { // an affine transform: rotation (row-major 3x3) and translation
    float r[9];
    vec3 t;
} Xform;

// THREE Matrix4.makeRotationFromEuler, row-major. order 0 = "XYZ" (the default), 1 = "YXZ".
static void euler(const float x, const float y, const float z, const int yxz, float m[9]) {
    const float a = cosf(x), b = sinf(x), c = cosf(y), d = sinf(y), e = cosf(z), f = sinf(z);
    if (!yxz) {
        const float ae = a * e, af = a * f, be = b * e, bf = b * f;
        m[0] = c * e;       m[1] = -c * f;      m[2] = d;
        m[3] = af + be * d; m[4] = ae - bf * d; m[5] = -b * c;
        m[6] = bf - ae * d; m[7] = be + af * d; m[8] = a * c;
    } else {
        const float ce = c * e, cf = c * f, de = d * e, df = d * f;
        m[0] = ce + df * b; m[1] = de * b - cf; m[2] = a * d;
        m[3] = a * f;       m[4] = a * e;       m[5] = -b;
        m[6] = cf * b - de; m[7] = df + ce * b; m[8] = a * c;
    }
}

// child world = parent world x local (Object3D.updateWorldMatrix)
static Xform xf_child(const Xform *parent, const vec3 position, const float rx, const float ry, const int yxz) {
    float l[9];
    euler(rx, ry, 0.0f, yxz, l);
    Xform out;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            out.r[i * 3 + j] = parent->r[i * 3] * l[j] + parent->r[i * 3 + 1] * l[3 + j] + parent->r[i * 3 + 2] * l[6 + j];
        }
    }
    const float *p = parent->r;
    out.t = (vec3) {parent->t.x + p[0] * position.x + p[1] * position.y + p[2] * position.z,
                    parent->t.y + p[3] * position.x + p[4] * position.y + p[5] * position.z,
                    parent->t.z + p[6] * position.x + p[7] * position.y + p[8] * position.z};
    return out;
}

typedef struct {
    HitBox *out;
    int max;
    int count;
    float pad; // the moving target's +1 (v10:117863), added before the mesh scale
} BoxSink;

// A box mesh under `parent`: geometry `size`, local position / rotation.x, uniform `scale`.
static void box_mesh(BoxSink *sink, const Xform *parent, const vec3 position, const float rx, const vec3 size, const float scale,
                     const int kind, const int part) {
    if (sink->count >= sink->max) return;
    const Xform w = xf_child(parent, position, rx, 0.0f, 0);
    HitBox *b = &sink->out[sink->count++];
    b->center = w.t;
    for (int k = 0; k < 3; k++) b->axis[k] = (vec3) {w.r[k], w.r[3 + k], w.r[6 + k]};
    b->half = (vec3) {(size.x * 0.5f + sink->pad) * scale, (size.y * 0.5f + sink->pad) * scale, (size.z * 0.5f + sink->pad) * scale};
    b->kind = kind;
    b->part = part;
}

// One arm (_40318, v10:117609-117663): the holder at the shoulder, turned (order YXZ) so the
// two-bone IK chain reaches the weapon's hold point; upper arm, elbow connector, lower arm.
static void arm_boxes(BoxSink *sink, const Xform *arms, const Weapon *w, const int left, const float hurtbox, HitBox *upper,
                      HitBox *connector, HitBox *lower) {
    const float u = game_constants.upper_arm_length, l = game_constants.lower_arm_length;
    const float x = (left ? 1.0f : -1.0f) * (-game_constants.chest_width + game_constants.arm_scale * 0.5f - game_constants.arm_inset);
    const float y = game_constants.arm_offset;
    const vec3 hold = left ? w->left_hold : w->right_hold;
    const float sx = left && w->akimbo ? -1.0f : 1.0f;
    const float dx = x - (w->offset.x - hold.x) * sx, dy = y - (w->offset.y + hold.y), dz = 0.0f - (w->offset.z - hold.z);
    const float reach = fminf(u + l - 0.01f, sqrtf(dx * dx + dy * dy + dz * dz));
    // UTILS.anglesFromSides(reach, u, l) (v10:23799): [-a0 - PI/2, a1]
    const float a0 = acosf(CLAMP((u * u + l * l - reach * reach) / (2.0f * u * l), -1.0f, 1.0f));
    const float a1 = acosf(CLAMP((l * l + reach * reach - u * u) / (2.0f * l * reach), -1.0f, 1.0f));
    const float ang0 = -a0 - (float) M_PI / 2.0f;
    const float half_pi = (float) M_PI / 2.0f;
    const float r = game_constants.arm_scale * 0.5f;
    const float len = sqrtf(r * r + r * r - 2.0f * r * r * cosf((float) M_PI + ang0 + half_pi));
    const float thick = 2.0f * sqrtf(fmaxf(0.0f, r * r - len * 0.5f * (len * 0.5f)));

    // holder: rot.x -a1 - getDir(0, y, zOff - holdZ, yOff + holdY), rot.y getDir(-x, 0, s (xOff - holdX), zOff - holdZ) - PI/2,
    // getDir(a, b, c, d) = atan2(b - d, a - c) (v10:23819)
    const float rx = -a1 - atan2f(y - (w->offset.y + hold.y), 0.0f - (w->offset.z - hold.z));
    const float ry = atan2f(0.0f - (w->offset.z - hold.z), -x - (left && w->akimbo ? 1.0f : -1.0f) * (w->offset.x - hold.x)) - half_pi;
    const Xform holder = xf_child(arms, (vec3) {x - w->offset.x, y - w->offset.y, -w->offset.z}, rx, ry, 1);

    const float arm = game_constants.arm_scale;
    const int start = sink->count;
    box_mesh(sink, &holder, (vec3) {0.0f, -u * 0.5f * cosf(half_pi), -u * 0.5f * sinf(half_pi)}, half_pi, (vec3) {arm, u, arm}, hurtbox,
             HIT_KIND_TORSO, HIT_PART_ARM);
    box_mesh(sink, &holder, (vec3) {0.0f, -u * cosf(half_pi), -u * sinf(half_pi)}, (half_pi + ang0) * 0.5f, (vec3) {arm, len, thick},
             hurtbox, HIT_KIND_TORSO, HIT_PART_ARM);
    box_mesh(sink, &holder, (vec3) {0.0f, -u * cosf(half_pi) - l * 0.5f * cosf(ang0), -u * sinf(half_pi) - l * 0.5f * sinf(ang0)}, ang0,
             (vec3) {arm, l, arm}, hurtbox, HIT_KIND_TORSO, HIT_PART_ARM);
    // hand the three back to the caller, which puts them in v10's trace order
    if (sink->count == start + 3) {
        *upper = sink->out[start];
        *connector = sink->out[start + 1];
        *lower = sink->out[start + 2];
    }
    sink->count = start;
}

int hitboxes_build(const HitPose *pose, HitBox *out, const int max) {
    if (!pose || !out || max <= 0) return 0;
    const GameConstants *k = &game_constants;
    const float hurtbox = pose->hurtbox_scale > 0.0f ? pose->hurtbox_scale : 1.0f;
    const float head_mlt = pose->head_mlt > 0.0f ? pose->head_mlt : 1.0f;
    const float c_v = pose->crouch_val;
    const float step_anim = k->step_anim;
    const float anim = k->anim_mlt;
    BoxSink sink = {out, max, 0, pose->moving ? 1.0f : 0.0f};

    // v10:117751-117761
    const float step_chase_bob = -cosf(2.0f * pose->step_chase) / 2.0f * step_anim; // _40394
    const float swing_raw = sinf(pose->step_val) * step_anim;                       // _40395
    const float bob = pose->no_bob ? 0.0f : fabsf(6.0f * swing_raw);
    const Xform world = {{1, 0, 0, 0, 1, 0, 0, 0, 1}, {0, 0, 0}};
    const Xform root = xf_child(&world, (vec3) {pose->position.x, pose->position.y + bob, pose->position.z}, 0.0f,
                                pose->x_dir + 2.0f * -swing_raw, 0);
    const float step_y = cosf(2.0f * pose->step_val) / 2.0f * step_anim * (1.0f - c_v * k->crouch_anim_mlt); // _40398
    const float swing = swing_raw - swing_raw * (c_v * k->crouch_anim_mlt);

    // upperBodyHoldObj (v10:117762-117768)
    const Xform upper = xf_child(&root, (vec3) {0.0f, k->leg_height - k->crouch_distance * c_v, 0.0f},
                                 k->crouch_lean * c_v + 0.5f * pose->y_dir, 0.0f, 0);

    // headHitbox (v10:169456-169462, posed v10:117770-117776)
    const float head_scale = hurtbox * head_mlt;
    box_mesh(&sink, &upper, (vec3) {0.0f, k->player_height - 0.5f * k->head_scale - k->leg_height + k->head_scale * (head_mlt - 1.0f) / 2.0f, 0.0f},
             0.0f, (vec3) {k->head_scale, k->head_scale, k->head_scale}, head_scale, HIT_KIND_HEAD, HIT_PART_HEAD);
    // torsoHitbox (v10:169467-169478)
    const float torso_h = k->player_height - k->head_scale - k->leg_height;
    box_mesh(&sink, &upper, (vec3) {0.0f, 0.5f * torso_h, 0.0f}, 0.0f, (vec3) {k->chest_width, torso_h, k->chest_scale}, hurtbox,
             HIT_KIND_TORSO, HIT_PART_TORSO);

    // armHoldObj and the arms (v10:117778-117823), only with a weapon
    const Weapon *w = pose->weapon;
    if (w) {
        const float recoil_ym = w->recoil_y_mlt ? w->recoil_y_mlt : 0.3f;
        const Xform hold = xf_child(&upper, (vec3) {0.0f, pose->recoil_anim_y * (recoil_ym * anim) + (k->player_height - k->camera_height - k->leg_height), 0.0f},
                                    0.5f * pose->y_dir, 0.0f, 0);
        const float chase = -sinf(pose->step_chase) * step_anim; // _40418
        const float crouch_sway = 1.0f - 0.8f * c_v;             // _40419
        const float sway = anim * 0.7f;                          // _40421 (aimAnimMlt term x 0: aimVal is not posed)
        const float idle = 1.0f - 0.5f * c_v;                    // _40422
        const float rot_y = (0.16f * -chase * anim * crouch_sway) * sway;
        const float rot_x = 1.5f * -step_chase_bob - cosf(0.0f) * idle * 0.01f * (1.75f * 0.7f) + w->rotation_offset * anim +
                            w->rotation_offset_aim * (1.0f - anim) + -0.9f * step_chase_bob * sway;
        const vec3 arms_pos = {(-w->crouch_rotation * c_v * anim + 0.5f * swing * crouch_sway * anim) * sway + w->offset.x,
                               (0.85f * step_y - w->crouch_drop * c_v * anim) * sway + w->offset.y,
                               w->offset.z};
        const Xform arms = xf_child(&hold, arms_pos, rot_x, rot_y, 0);
        HitBox lu, lc, ll, ru, rc, rl;
        memset(&lu, 0, sizeof(lu));
        lc = ll = ru = rc = rl = lu;
        arm_boxes(&sink, &arms, w, 1, hurtbox, &lu, &lc, &ll);
        arm_boxes(&sink, &arms, w, 0, hurtbox, &ru, &rc, &rl);
        // v10:117893-117899: left upper, right upper, left connector, left lower, right connector, right lower
        const HitBox *order[6] = {&lu, &ru, &lc, &ll, &rc, &rl};
        for (int i = 0; i < 6 && sink.count < sink.max; i++) sink.out[sink.count++] = *order[i];
    }

    // legsHoldObj (v10:169488-169493 builds [right standing, left standing, right crouched, left crouched]);
    // rotation.x = swing x (1, 3: +1, 0, 2: -1) x 9 + (crouched: -0.6) (v10:117784-117787)
    const float leg_scale = k->leg_scale;
    for (int i = 0; i < 4; i++) {
        const int crouched = i >= 2;
        const float side = (i == 0 || i == 2) ? 1.0f : -1.0f; // right +x, left -x
        const float rx = swing * ((i == 1 || i == 3) ? 1.0f : -1.0f) * 9.0f + (crouched ? -0.6f : 0.0f);
        if (!crouched) {
            // _40347 (v10:117664-117681)
            const Xform holder = xf_child(&root, (vec3) {0.5f * leg_scale * side, k->leg_height, 0.0f}, rx, 0.0f, 0);
            box_mesh(&sink, &holder, (vec3) {0.0f, 0.5f * -k->leg_height, 0.0f}, 0.0f, (vec3) {leg_scale, k->leg_height, leg_scale}, hurtbox,
                     HIT_KIND_LEG, HIT_PART_LEG_STANDING);
        } else {
            // _40356 (v10:117682-117724): rot.y PI/8 left, -PI/6 right; the crouched leg's joint angles [0.5, 2]
            const Xform holder = xf_child(&root, (vec3) {0.5f * leg_scale * side, k->leg_height - k->crouch_distance + 0.5f, 0.0f}, rx,
                                          side < 0.0f ? (float) M_PI / 8.0f : -(float) M_PI / 6.0f, 0);
            const float lh = k->leg_height / 2.0f, r = leg_scale / 2.0f, j0 = 0.5f, j1 = 2.0f;
            const float len = sqrtf(r * r + r * r - 2.0f * r * r * cosf(j0 - j1));
            const float thick = 2.0f * sqrtf(fmaxf(0.0f, r * r - len / 2.0f * (len / 2.0f)));
            box_mesh(&sink, &holder, (vec3) {0.0f, -lh / 2.0f * cosf(j1), -lh / 2.0f * sinf(j1)}, j1, (vec3) {leg_scale, lh, leg_scale}, hurtbox,
                     HIT_KIND_LEG, HIT_PART_LEG_CROUCHED);
            box_mesh(&sink, &holder, (vec3) {0.0f, -lh * cosf(j1), -lh * sinf(j1)}, (j1 + j0) / 2.0f, (vec3) {leg_scale, len, thick}, hurtbox,
                     HIT_KIND_LEG, HIT_PART_LEG_CROUCHED);
            box_mesh(&sink, &holder, (vec3) {0.0f, -lh * cosf(j1) - lh / 2.0f * cosf(j0), -lh * sinf(j1) - lh / 2.0f * sinf(j0)}, j0,
                     (vec3) {leg_scale, lh, leg_scale}, hurtbox, HIT_KIND_LEG, HIT_PART_LEG_CROUCHED);
        }
    }
    return sink.count;
}

// OBB.intersectRay (v10:117535-117543): the ray in the box frame against Box3 (Ray.intersectBox:
// the entry point, or the exit point when the origin is inside). Returns the distance along the
// unit `dir`, -1 for a miss.
static float ray_obb(const vec3 origin, const vec3 dir, const HitBox *b) {
    const vec3 d = {origin.x - b->center.x, origin.y - b->center.y, origin.z - b->center.z};
    float tmin = -INFINITY, tmax = INFINITY;
    for (int i = 0; i < 3; i++) {
        const vec3 a = b->axis[i];
        const float half = i == 0 ? b->half.x : i == 1 ? b->half.y : b->half.z;
        const float o = d.x * a.x + d.y * a.y + d.z * a.z;
        const float v = dir.x * a.x + dir.y * a.y + dir.z * a.z;
        if (v == 0.0f) {
            if (o < -half || o > half) return -1.0f;
            continue;
        }
        float t1 = (-half - o) / v, t2 = (half - o) / v;
        if (t1 > t2) {
            const float s = t1;
            t1 = t2;
            t2 = s;
        }
        if (t1 > tmin) tmin = t1;
        if (t2 < tmax) tmax = t2;
        if (tmin > tmax) return -1.0f;
    }
    if (tmax < 0.0f) return -1.0f;
    return tmin >= 0.0f ? tmin : tmax;
}

int hitboxes_ray(const HitPose *pose, const vec3 origin, const vec3 dir, const float range, float *fraction, int *kind) {
    HitBox boxes[HIT_BOX_MAX];
    const int count = hitboxes_build(pose, boxes, HIT_BOX_MAX);
    if (range <= 0.0f) return 0;
    const int crouched = pose->crouch_val != 0.0f;
    float best = INFINITY;
    int best_kind = HIT_KIND_TORSO;
    int torso_hit = 0, arm_hit = 0, leg_hit = 0;
    for (int i = 0; i < count; i++) {
        const HitBox *b = &boxes[i];
        if (b->part == HIT_PART_ARM && (torso_hit || arm_hit)) continue; // arms only when the torso missed; first arm hit
        // legs: the userData.active pair only (standing when cV is 0), first leg hit only
        if (b->kind == HIT_KIND_LEG && (leg_hit || (b->part == HIT_PART_LEG_STANDING) == crouched)) continue;
        const float t = ray_obb(origin, dir, b);
        if (t < 0.0f || t > range) continue; // v10's caller drops a distance > range (dst <= 1, v10:172897)
        if (b->part == HIT_PART_TORSO) torso_hit = 1;
        if (b->part == HIT_PART_ARM) arm_hit = 1;
        if (b->kind == HIT_KIND_LEG) leg_hit = 1;
        // distance / range, toFixed(4); sort by distance keeps the first of equal ones (head first)
        const float f = floorf(t / range * 10000.0f + 0.5f) / 10000.0f;
        if (f < best) {
            best = f;
            best_kind = b->kind;
        }
    }
    if (best == INFINITY) return 0;
    if (fraction) *fraction = best;
    if (kind) *kind = best_kind;
    return 1;
}

void hitboxes_pose_player(const Player *target, const PlayerStoredState *state, HitPose *out) {
    const Game *game = target->game;
    memset(out, 0, sizeof(*out));
    out->position = state->position;
    out->x_dir = state->x_dir;
    out->y_dir = state->y_dir;
    out->step_val = state->step_val;
    out->step_chase = state->step_chase;
    out->crouch_val = state->crouch_val;
    // weaponConfig[state.weapon] (v10:117733): the game's (host-edited) table, never the upgraded entry
    const size_t weapon_count = game && game->weapon_count ? game->weapon_count : rules_weapon_count();
    const Weapon *const *weapons = game && game->weapons ? (const Weapon *const *) game->weapons : rules_weapons();
    out->weapon = state->weapon_index >= 0 && (size_t) state->weapon_index < weapon_count ? weapons[state->weapon_index] : NULL;
    out->hurtbox_scale = player_hurtbox_scale(target);
    out->head_mlt = (target->head_mlt > 0.0f ? target->head_mlt : 1.0f) * mode_head_mlt(target);
    out->recoil_anim_y = target->recoil_anim_y;
    out->moving = target->velocity.x != 0.0f || target->velocity.y != 0.0f || target->velocity.z != 0.0f;
    out->no_bob = player_class_model(target) != 0;
}

int hitboxes_active(const Game *game) {
    return !(game && game->mode && game->mode->config.prop);
}

// v10's broad test: lineInRect (v10:24029) `!(tmax < 0) && !(tmin > tmax) && tmin` then `<= 1`
// (v10:172895, 180125). Unlike shared/src/utils.c line_in_rect (-1 unless the entry is ahead), a
// ray or step that starts inside the box passes (tmin < 0 is truthy): a point-blank shot, and a
// projectile or Piss stream step that is already within 5 of the player.
static int broad_in_rect(const vec3 o, const vec3 d, const vec3 lo, const vec3 hi) {
    const float tx1 = (lo.x - o.x) / d.x, tx2 = (hi.x - o.x) / d.x;
    const float ty1 = (lo.y - o.y) / d.y, ty2 = (hi.y - o.y) / d.y;
    const float tz1 = (lo.z - o.z) / d.z, tz2 = (hi.z - o.z) / d.z;
    const float tmin = fmaxf(fmaxf(fminf(tx1, tx2), fminf(ty1, ty2)), fminf(tz1, tz2));
    const float tmax = fminf(fminf(fmaxf(tx1, tx2), fmaxf(ty1, ty2)), fmaxf(tz1, tz2));
    return !(tmax < 0.0f) && !(tmin > tmax) && tmin != 0.0f && tmin <= 1.0f;
}

int hitboxes_trace_player(const Player *target, const PlayerStoredState *state, const vec3 origin, const vec3 step, const int broad,
                          float *fraction, int *kind) {
    if (broad) { // lineInRect with a fixed padding of 5 (not hitBoxPad), v10:172895 / 180125
        const float half = target->scale + 5.0f;
        const vec3 lo = {state->position.x - half, state->position.y, state->position.z - half};
        const vec3 hi = {state->position.x + half, state->position.y + state->adjusted_height + 5.0f, state->position.z + half};
        if (!broad_in_rect(origin, step, lo, hi)) return 0;
    }

    const float range = sqrtf(step.x * step.x + step.y * step.y + step.z * step.z);
    if (range <= 0.0f) return 0;
    const vec3 dir = {step.x / range, step.y / range, step.z / range};
    HitPose pose;
    hitboxes_pose_player(target, state, &pose);
    return hitboxes_ray(&pose, origin, dir, range, fraction, kind);
}
