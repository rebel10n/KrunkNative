// Krunker v10.0.3 player poses on the client: the "pz" packet, the pending poses and the model
// (client/include/player_pose_render.h). Line numbers: deob-10.0.3.js.
//
// v10 builds a third-person model's arms merged (one mesh for both, mergedArmMeshes) and, since 10.0.3,
// splits them (setSplitArms :193477-193514) when a pose moves them apart (armL, armR or holdL) so that
// armL / armR turn only their own arm ("player.pose armL and armR now move only their own arm in third
// person"). KrunkNative's arms are two meshes with a pivot each already (PlayerArmMesh.pivot = armMeshes[i]),
// so a split is the two pivots turned apart and a merged turn is both pivots turned alike.
//
// v10 mirrors the whole upper body for the leftHand setting (upperBody.applyMatrix4(mirroredMatrix)
// :194848-194850); KN builds the arms mirrored instead (generate_arm). A pose's turn [x, y, z] of the
// "l" arm (genArms side 1: KN's off arm) is therefore applied as [x, -y, -z] there, and a holdL offset's
// x is negated: what v10's mirror makes of them.
#include <player_pose_render.h>

#include <entities.h>
#include <gltf.h>
#include <net_ext.h>
#include <player_pose.h>
#include <nettspend.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------- the pose (setPose, pendingPoses)

#define POSE_PENDING_MAX 64 // players a lobby can hold

typedef struct {
    int uid;
    PlayerPose pose;
} PendingPose;
static PendingPose g_pending[POSE_PENDING_MAX];
static int g_pending_count;

void player_pose_reset(void) { g_pending_count = 0; }

static int pending_find(const int uid) {
    for (int i = 0; i < g_pending_count; i++)
        if (g_pending[i].uid == uid) return i;
    return -1;
}

static void pending_drop(const int i) { g_pending[i] = g_pending[--g_pending_count]; }

// The packet as a pose: the keys it names, numbers kept to sanitizePose's limits (our server sends them
// sanitized; this keeps a stray peer's from turning a model inside out)
static void pose_from_packet(PlayerPose *pose, const NetPlayerPosePacket *p) {
    memset(pose, 0, sizeof(*pose));
    pose->has = p->has & PLAYER_POSE_KEYS;
    if (!pose->has) return;
    pose->legs = p->legs > 2 ? 2 : p->legs;
    pose->weapon = !!p->weapon;
    pose->bob = !!p->bob;
    pose->face_ride = !!p->face_ride;
    pose->hold = !!p->hold;
    pose->hold_l = !!p->hold_l;
    if (!player_pose_clamp(p->leg_x, PLAYER_POSE_LEG_X_MAX, &pose->leg_x)) pose->has &= (uint16_t) ~PLAYER_POSE_LEG_X;
    if (!player_pose_clamp(p->body_y, PLAYER_POSE_BODY_Y_MAX, &pose->body_y)) pose->has &= (uint16_t) ~PLAYER_POSE_BODY_Y;
    if (!player_pose_clamp(p->torso_x, PLAYER_POSE_TORSO_X_MAX, &pose->torso_x)) pose->has &= (uint16_t) ~PLAYER_POSE_TORSO_X;
    for (int i = 0; i < 3; i++) {
        if (!player_pose_clamp(p->arm_l[i], PLAYER_POSE_ARM_MAX, &pose->arm_l[i])) pose->arm_l[i] = 0.0f;
        if (!player_pose_clamp(p->arm_r[i], PLAYER_POSE_ARM_MAX, &pose->arm_r[i])) pose->arm_r[i] = 0.0f;
    }
    if (!pose->has) memset(pose, 0, sizeof(*pose));
}

static void pose_weapons(Player *player, PlayerMesh *pm, int hide);

// setPose without a pose: resetMergedArms (:195371-195377), meshHoldObj back to 0, the weapon meshes
// back (refreshWeaponVisibility)
static void pose_model_rest(Player *player) {
    PlayerMesh *pm = player->mesh;
    if (!pm) return;
    pm->hold.position.y = 0.0f;
    for (size_t i = 0; pm->arms && i < (size_t) player->loadout_size; i++) {
        PlayerArms *arms = pm->arms[i];
        if (!arms || arms->melee_pose) continue; // armMeshes: posed again every frame (player_update_melee_arms)
        arms->pose_merged_position = arms->pose_merged_rotation = (vec3) {0.0f, 0.0f, 0.0f};
        for (int k = 0; k < 2; k++) {
            PlayerArmMesh *arm = k ? arms->right : arms->left;
            if (!arm) continue;
            arm->pivot.position = arm->pivot.rotation = (vec3) {0.0f, 0.0f, 0.0f};
            arm->pivot.rotation_order = ROTATION_ORDER_INTRINSIC;
        }
    }
    pose_weapons(player, pm, 0);
}

void player_pose_client_set(Player *player, const PlayerPose *pose) {
    if (!player) return;
    const int has = pose && (pose->has & PLAYER_POSE_KEYS);
    if (!has || !player->pose.has) player->pose_lerp.active = 0; // if (!pose || !player.poseData) poseLerp = null
    if (has) {
        player->pose = *pose;
        player->pose.has &= PLAYER_POSE_KEYS;
    } else {
        memset(&player->pose, 0, sizeof(player->pose));
        pose_model_rest(player);
    }
    player_update_height(player); // updateHeight reads poseData.bodyY (the local player's eye)
}

void player_pose_on_ext(Client *client, const uint8_t *body, const uint16_t length) {
    NetPlayerPosePacket packet;
    if (!client || length < sizeof(packet)) return;
    memcpy(&packet, body, sizeof(packet));
    PlayerPose pose;
    pose_from_packet(&pose, &packet);
    Player *player = game_find_player(&client->game, packet.uid);
    if (player) {
        player_pose_client_set(player, &pose);
        return;
    }
    // not here yet: pendingPoses[sid] = pose (or deleted for pose 0)
    const int i = pending_find(packet.uid);
    if (!pose.has) {
        if (i >= 0) pending_drop(i);
        return;
    }
    if (i >= 0) g_pending[i].pose = pose;
    else if (g_pending_count < POSE_PENDING_MAX) g_pending[g_pending_count++] = (PendingPose) {packet.uid, pose};
}

// ---------------------------------------------------------------- rotations (three.js "XYZ": R = Rx Ry Rz)

static void mat3_from_euler(float m[3][3], const vec3 r) {
    const float cx = cosf(r.x), sx = sinf(r.x), cy = cosf(r.y), sy = sinf(r.y), cz = cosf(r.z), sz = sinf(r.z);
    m[0][0] = cy * cz, m[0][1] = -cy * sz, m[0][2] = sy;
    m[1][0] = cx * sz + sx * sy * cz, m[1][1] = cx * cz - sx * sy * sz, m[1][2] = -sx * cy;
    m[2][0] = sx * sz - cx * sy * cz, m[2][1] = sx * cz + cx * sy * sz, m[2][2] = cx * cy;
}

static vec3 euler_from_mat3(const float m[3][3]) { // three.js Euler.setFromRotationMatrix, "XYZ"
    const float y = asinf(fminf(fmaxf(m[0][2], -1.0f), 1.0f));
    if (fabsf(m[0][2]) < 0.9999999f) return (vec3) {atan2f(-m[1][2], m[2][2]), y, atan2f(-m[0][1], m[0][0])};
    return (vec3) {atan2f(m[2][1], m[1][1]), y, 0.0f};
}

// IÍìiìïi (:193325-193329): the node turned by Euler(x, y, z) about `c` (its parent's frame): position
// (p - c) q + c, quaternion premultiplied by q. `mirror` -1: the turn as v10's leftHand mirror shows it.
static void pose_turn(MeshTransform *t, const float r[3], const vec3 c, const float mirror) {
    float q[3][3], cur[3][3], out[3][3];
    mat3_from_euler(q, (vec3) {r[0], r[1] * mirror, r[2] * mirror});
    mat3_from_euler(cur, t->rotation_order == ROTATION_ORDER_INTRINSIC ? t->rotation : (vec3) {0.0f, 0.0f, 0.0f});
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) out[i][j] = q[i][0] * cur[0][j] + q[i][1] * cur[1][j] + q[i][2] * cur[2][j];
    const float px = t->position.x - c.x, py = t->position.y - c.y, pz = t->position.z - c.z;
    t->position = (vec3) {q[0][0] * px + q[0][1] * py + q[0][2] * pz + c.x, q[1][0] * px + q[1][1] * py + q[1][2] * pz + c.y,
                          q[2][0] * px + q[2][1] * py + q[2][2] * pz + c.z};
    t->rotation = euler_from_mat3(out);
    t->rotation_order = ROTATION_ORDER_INTRINSIC;
}

// The node moved by a parent-frame transform M (position pm, rotation rm): p' = Rm p + pm, R' = Rm R. On a node at
// rest it simply takes M (the merged arms' kept transform onto KN's two pivots).
static void pose_apply(MeshTransform *t, const vec3 pm, const vec3 rm) {
    float m[3][3], cur[3][3], out[3][3];
    mat3_from_euler(m, rm);
    mat3_from_euler(cur, t->rotation_order == ROTATION_ORDER_INTRINSIC ? t->rotation : (vec3) {0.0f, 0.0f, 0.0f});
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) out[i][j] = m[i][0] * cur[0][j] + m[i][1] * cur[1][j] + m[i][2] * cur[2][j];
    const vec3 p = t->position;
    t->position = (vec3) {m[0][0] * p.x + m[0][1] * p.y + m[0][2] * p.z + pm.x, m[1][0] * p.x + m[1][1] * p.y + m[1][2] * p.z + pm.y,
                          m[2][0] * p.x + m[2][1] * p.y + m[2][2] * p.z + pm.z};
    t->rotation = euler_from_mat3(out);
    t->rotation_order = ROTATION_ORDER_INTRINSIC;
}

// quaternion.copy(node.quaternion).invert(): (Rx Ry Rz)^-1 = Rz(-z) Ry(-y) Rx(-x), the extrinsic order
static void pose_undo_turn(MeshTransform *t, const MeshTransform *of) {
    t->rotation = (vec3) {-of->rotation.x, -of->rotation.y, -of->rotation.z};
    t->rotation_order = of->rotation_order == ROTATION_ORDER_INTRINSIC ? ROTATION_ORDER_EXTRINSIC : ROTATION_ORDER_INTRINSIC;
}

// ---------------------------------------------------------------- the model

// genArms side 1 ("l", armL, vecs.l: the left hold) / side 2 ("r"): KN's off / trigger arm
static PlayerArmMesh *pose_arm(const PlayerArms *arms, const int side, const int left_handed) {
    if (side == 0) return left_handed ? arms->right : arms->left;
    return left_handed ? arms->left : arms->right;
}

static PlayerArms *held_arms(const Player *player) {
    const PlayerMesh *pm = player->mesh;
    if (!pm || !pm->arms || player->loadout_index < 0 || player->loadout_index >= player->loadout_size) return NULL;
    return pm->arms[player->loadout_index];
}

static int drank_arms(const Player *player, const PlayerArms *arms) {
    return arms && arms->hose && player->loadout_index >= 0 && player->loadout_index < player->loadout_size &&
           nettspend_weapon_kind(player->loadout[player->loadout_index]) == NETTSPEND_WEAPON_DRANK;
}

// legMeshes[0..3]: the standing legs, then the crouched ones
static int leg_exists(const PlayerMesh *pm, const int i) { return i < 2 ? pm->legs[i] != NULL : pm->crouched_legs[i - 2] != NULL; }
static MeshTransform *leg_transform(PlayerMesh *pm, const int i) { return i < 2 ? &pm->legs[i]->transform : &pm->crouched_legs[i - 2]->anchor; }
static int leg_visible(const PlayerMesh *pm, const int i) {
    if (i < 2) return pm->legs[i]->mesh_count && pm->legs[i]->meshes[0]->visible;
    const PlayerCrouchedLeg *leg = pm->crouched_legs[i - 2];
    return leg->upper && leg->upper->visible;
}
static void leg_set_visible(PlayerMesh *pm, const int i, const int visible) {
    const ColorCube *cube = i < 2 ? pm->legs[i] : pm->crouched_legs[i - 2]->lower;
    for (size_t k = 0; cube && k < cube->mesh_count; k++) cube->meshes[k]->visible = visible;
    if (i >= 2) {
        PlayerCrouchedLeg *leg = pm->crouched_legs[i - 2];
        if (leg->upper) leg->upper->visible = visible;
        if (leg->joint) leg->joint->visible = visible;
    }
}

// refreshWeaponVisibility (:195386-195411): a pose that hides the weapon model hides every weapon mesh
// (not the arms, not the hands anchor) and remembers them; without it they show again. KN re-checks every
// frame: a swap or the scope / melee code may show one meanwhile. A mesh comes back only in the slot in
// hand (the swap has hidden the others since).
static void pose_weapons(Player *player, PlayerMesh *pm, const int hide) {
    if (!hide) {
        for (int i = 0; i < pm->pose_hidden_count; i++) {
            if (pm->pose_hidden_slot[i] != player->loadout_index) continue;
            const PlayerArms *arms = held_arms(player);
            // a first-person melee mesh shows only while the knife is in hand (player_update_melee_arms)
            pm->pose_hidden[i]->visible = arms && arms->melee_pose && pm->pose_hidden[i] == arms->weapon_right ? !!player->can_throw : 1;
        }
        pm->pose_hidden_count = 0;
        return;
    }
    for (int slot = 0; pm->arms && slot < player->loadout_size; slot++) {
        const PlayerArms *arms = pm->arms[slot];
        if (!arms) continue;
        Mesh *meshes[3] = {arms->weapon_right, arms->weapon_left, arms->attachment}; // the charm follows weapon_right
        for (int k = 0; k < 3; k++) {
            Mesh *m = meshes[k];
            if (!m || !m->visible) continue;
            m->visible = 0;
            int known = 0;
            for (int i = 0; i < pm->pose_hidden_count; i++) known |= pm->pose_hidden[i] == m;
            if (known || pm->pose_hidden_count >= (int) (sizeof(pm->pose_hidden) / sizeof(pm->pose_hidden[0]))) continue;
            pm->pose_hidden[pm->pose_hidden_count] = m;
            pm->pose_hidden_slot[pm->pose_hidden_count++] = (signed char) slot;
        }
    }
}

// syncHands (:193452-193476): first person with the melee arms, at IÌïìïìi in the knife's arm with that
// arm's turn undone; else in the held weapon at IÏiîìïï (0, -1.5, 0), turned with armR when a third-person
// pose holds (armR or hold)
static void sync_hands(Player *player, PlayerMesh *pm, PlayerArms *arms, const int first_person, const vec3 shoulder, const float mirror) {
    MeshTransform *h = &pm->hands_anchor;
    h->scale = (vec3) {1.0f, 1.0f, 1.0f};
    if (!arms) {
        h->parent = NULL;
        return;
    }
    if (first_person && arms->melee_pose && arms->weapon_right && arms->weapon_right->transform.parent) {
        MeshTransform *arm = arms->weapon_right->transform.parent;
        h->parent = arm;
        h->position = (vec3) {0.9f, -0.95f, 0.72f};
        pose_undo_turn(h, arm);
        return;
    }
    h->parent = &arms->anchor;
    h->position = (vec3) {0.0f, -1.5f, 0.0f};
    h->rotation = (vec3) {0.0f, 0.0f, 0.0f};
    h->rotation_order = ROTATION_ORDER_INTRINSIC;
    const PlayerPose *pose = &player->pose;
    if (!first_person && pose->has && player->pose_lerp.active && ((pose->has & PLAYER_POSE_ARM_R) || pose->hold))
        pose_turn(h, player->pose_lerp.arm_r, shoulder, mirror);
}

// syncHandL (:193527-193549): at the left hand of the "l" arm (the split one, the melee one or the merged
// mesh: KN's off arm in every case), the arm's turn undone
static void sync_hand_l(Player *player, PlayerMesh *pm, PlayerArms *arms) {
    MeshTransform *h = &pm->hand_l_anchor;
    h->scale = (vec3) {1.0f, 1.0f, 1.0f};
    PlayerArmMesh *arm = arms ? pose_arm(arms, 0, player->left_handed) : NULL;
    if (!arm || !arm->lower || !arm->lower->mesh_count) {
        h->parent = NULL;
        return;
    }
    const Mesh *skin = arm->lower->meshes[arm->lower->mesh_count - 1]; // the hand: the forearm's last segment
    h->parent = &arm->pivot;
    h->position = transform_point_to(&skin->transform, (vec3) {0.0f, 0.5f, 0.0f}, &arm->pivot);
    pose_undo_turn(h, &arm->pivot);
}

int player_pose_face_ride_yaw(const Player *player, float *yaw) {
    // updateMsh (:193675-193680): poseData.faceRide && attachedEnt != null -> (entity.renderPose || entity).yaw
    if (!player || !player->pose.has || !player->pose.face_ride) return 0;
    return entities_player_ride_yaw(player->uid, yaw);
}

MeshTransform *player_pose_hands_anchor(Player *player) {
    PlayerMesh *pm = player ? player->mesh : NULL;
    return pm && pm->hands_anchor.parent ? &pm->hands_anchor : NULL;
}

MeshTransform *player_pose_hand_l_anchor(Player *player) {
    PlayerMesh *pm = player ? player->mesh : NULL;
    return pm && pm->hand_l_anchor.parent ? &pm->hand_l_anchor : NULL;
}

void player_pose_describe(const Player *player, char *out, const size_t n) {
    const PlayerMesh *pm = player ? player->mesh : NULL;
    const PlayerArms *arms = pm ? held_arms(player) : NULL;
    if (!pm || !arms || !arms->left || !arms->right) {
        snprintf(out, n, "has=0x%x -", player ? player->pose.has : 0);
        return;
    }
    const vec3 l = arms->left->pivot.rotation, r = arms->right->pivot.rotation, m = arms->pose_merged_rotation;
    snprintf(out, n, "has=0x%x hold=%.3f l=%.3f,%.3f,%.3f r=%.3f,%.3f,%.3f merged=%.3f,%.3f,%.3f", player->pose.has, pm->hold.position.y,
             l.x, l.y, l.z, r.x, r.y, r.z, m.x, m.y, m.z);
}

int player_pose_hand_l_first_person(Player *player) {
    PlayerMesh *pm = player ? player->mesh : NULL;
    if (!pm || !pm->render_you || !pm->hand_l_anchor.parent) return 0;
    const PlayerArms *arms = held_arms(player);
    const PlayerArmMesh *arm = arms ? pose_arm(arms, 0, player->left_handed) : NULL;
    return arm && arm->extender != NULL; // a first-person build's arm (the third-person one has upper / joint)
}

void player_pose_render(Player *player, const int first_person, const float step_anim) {
    PlayerMesh *pm = player ? player->mesh : NULL;
    if (!pm) return;
    if (g_pending_count) { // players.add: a pose that came before the player (:195135-195139)
        const int i = pending_find(player->uid);
        if (i >= 0) {
            PlayerPose pose = g_pending[i].pose;
            pending_drop(i);
            player->pose_lerp.active = 0;
            player->pose = pose;
            player_update_height(player);
        }
    }
    const PlayerPose *pose = &player->pose;
    PlayerArms *arms = held_arms(player);
    const float mirror = player->left_handed ? -1.0f : 1.0f;
    // IÍiîiîì (:193407): the shoulder line, 0.6 behind the held weapon's origin
    const vec3 shoulder = {0.0f, 0.0f, arms ? 0.6f - arms->anchor.position.z : 0.0f};
    pm->hold.position.y = 0.0f;

    if (pose->has && first_person) {
        // holdL: armMeshes[0], first person's own left arm (the melee arms), moved aside by IIìiïïï.fp
        // [-0.5, 0.35, 0.5] (its rotation part is 0) (:193343-193353)
        PlayerArmMesh *left = arms && arms->melee_pose ? pose_arm(arms, 0, player->left_handed) : NULL;
        if (pose->hold_l && left) {
            left->pivot.position.x += -0.5f * mirror;
            left->pivot.position.y += 0.35f;
            left->pivot.position.z += 0.5f;
        }
    } else if (pose->has) {
        PlayerPoseLerp *l = &player->pose_lerp;
        if (!l->active) { // poseLerp ||= {legX, bodyY, torsoX: the pose's, arms from 0}
            memset(l, 0, sizeof(*l));
            l->active = 1;
            l->leg_x = pose->leg_x;
            l->body_y = pose->body_y;
            l->torso_x = pose->torso_x;
        }
        l->leg_x += (pose->leg_x - l->leg_x) * PLAYER_POSE_LERP;
        l->body_y += (pose->body_y - l->body_y) * PLAYER_POSE_LERP;
        l->torso_x += (pose->torso_x - l->torso_x) * PLAYER_POSE_LERP;
        for (int side = 0; side < 2; side++) {
            float target[3];
            player_pose_arm_target(pose, side, target);
            float *cur = side ? l->arm_r : l->arm_l;
            for (int k = 0; k < 3; k++) cur[k] += (target[k] - cur[k]) * PLAYER_POSE_LERP;
        }

        // legs: 0 none, 1 the standing pair, 2 the crouched pair; legX turns the visible ones (:193377-193398)
        const int no_legs = (pose->has & PLAYER_POSE_LEGS) && pose->legs == 0;
        const int crouched_legs = (pose->has & PLAYER_POSE_LEGS) && pose->legs == 2;
        for (int i = 0; i < 4; i++) {
            if (!leg_exists(pm, i)) continue;
            if (no_legs) leg_set_visible(pm, i, 0);
            else if (pose->has & PLAYER_POSE_LEGS) leg_set_visible(pm, i, (i > 1) == crouched_legs);
            const int visible = leg_visible(pm, i);
            MeshTransform *t = leg_transform(pm, i);
            if (visible && (pose->has & PLAYER_POSE_LEG_X)) t->rotation.x = l->leg_x;
            if (i < 2) { // shoeMeshes: shown with their leg, turned with it
                const int shoe = PLAYER_COSMETIC_SHOE_R + i;
                const int shoe_visible = visible && !pm->shoe_hidden[i];
                if (pm->cosmetics[shoe]) pm->cosmetics[shoe]->visible = shoe_visible;
                if (pm->cosmetic_models[shoe]) gltf_instance_set_visible(pm->cosmetic_models[shoe], shoe_visible);
                pm->cosmetic_holders[shoe].rotation.x = t->rotation.x;
            }
        }
        // meshHoldObj.position.y = bodyY ? poseLerp.bodyY : 0 (:193400-193402)
        if ((pose->has & PLAYER_POSE_BODY_Y) && pose->body_y != 0.0f) pm->hold.position.y = l->body_y;
        // upperBody.rotation.x += poseLerp.torsoX (:193403-193405)
        if ((pose->has & PLAYER_POSE_TORSO_X) && pose->torso_x != 0.0f) pm->upper_body_anchor->rotation.x += l->torso_x;

        // the arms (:193406-193422): armL with armL / hold / holdL, armR with armR / hold
        const int use[2] = {(pose->has & PLAYER_POSE_ARM_L) || pose->hold || pose->hold_l, (pose->has & PLAYER_POSE_ARM_R) || pose->hold};
        const float *lerp[2] = {l->arm_l, l->arm_r};
        if (arms && arms->left && arms->right) {
            if (arms->melee_pose) {
                // armMeshes (the own model's melee arms): bob 0 takes the step swing back off, then each turns
                if ((pose->has & PLAYER_POSE_BOB) && !pose->bob) {
                    arms->left->pivot.position.z -= step_anim; // player_update_melee_arms: left +stepAnim, right -stepAnim
                    arms->right->pivot.position.z += step_anim;
                }
                for (int side = 0; side < 2; side++)
                    if (use[side]) pose_turn(&pose_arm(arms, side, player->left_handed)->pivot, lerp[side], shoulder, mirror);
            } else {
                // the merged arms: at rest every frame (split arms are reset to 0 before the turn); the Drank's
                // pour-up poses its pivots itself, the turn goes on top
                if (!drank_arms(player, arms)) {
                    for (int k = 0; k < 2; k++) {
                        PlayerArmMesh *arm = k ? arms->right : arms->left;
                        arm->pivot.position = arm->pivot.rotation = (vec3) {0.0f, 0.0f, 0.0f};
                        arm->pivot.rotation_order = ROTATION_ORDER_INTRINSIC;
                    }
                }
                if (player_pose_splits_arms(pose)) { // setSplitArms(true): each arm its own turn
                    for (int side = 0; side < 2; side++)
                        if (use[side]) pose_turn(&pose_arm(arms, side, player->left_handed)->pivot, lerp[side], shoulder, mirror);
                } else {
                    // mergedArmMeshes[i]: with armR / hold it is put at rest and turned by armR (:193428-193434);
                    // without, it keeps the transform it had (a previous pose's turn) until resetMergedArms at
                    // the pose's end. Both of KN's pivots take that kept transform.
                    if (use[1]) {
                        MeshTransform merged = {.scale = {1.0f, 1.0f, 1.0f}};
                        pose_turn(&merged, lerp[1], shoulder, mirror);
                        arms->pose_merged_position = merged.position;
                        arms->pose_merged_rotation = merged.rotation;
                    }
                    pose_apply(&arms->left->pivot, arms->pose_merged_position, arms->pose_merged_rotation);
                    pose_apply(&arms->right->pivot, arms->pose_merged_position, arms->pose_merged_rotation);
                }
            }
        }
    }
    if (!pose->has) player->pose_lerp.active = 0;

    pose_weapons(player, pm, player_pose_hides_weapon(pose));
    sync_hands(player, pm, arms, first_person, shoulder, mirror);
    sync_hand_l(player, pm, arms);
}
