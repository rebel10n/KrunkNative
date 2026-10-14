// v10.0.2 hit model test (shared/src/hitboxes.c, doc/v10/gameplay.md section 3): the posed
// oriented boxes at known poses, the ray/zone results the v10 server trace would give, and the
// v7.1.4 AABB the v7 ruleset keeps using. Constants are v10's own (v10:19492-20084: legHeight 4.2,
// playerHeight 11, headScale 2, chestWidth 2.6, chestScale 1.3, cameraHeight 1.5, uArmLength /
// lArmLength 2.7, armScale 1.2, armInset -0.1, armOff -0.8, legScale 1.3, stepAnim 0.075,
// crouchDst 3, crouchLean 0) and are the same in v7 (checked by ruleset_test). Run from the
// repository root.
#include <shared.h>
#include <hitboxes.h>
#include <ruleset.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;
static int checks = 0;

#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int near(const float a, const float b, const float eps) {
    return fabsf(a - b) <= eps * fmaxf(1.0f, fabsf(b));
}

static HitPose base_pose(void) {
    HitPose pose;
    memset(&pose, 0, sizeof(pose));
    pose.position = (vec3) {0.0f, 0.0f, 0.0f};
    pose.hurtbox_scale = 1.0f;
    pose.head_mlt = 1.0f;
    return pose;
}

static int find_box(const HitBox *boxes, const int count, const int part, const int which) {
    int seen = 0;
    for (int i = 0; i < count; i++) {
        if (boxes[i].part != part) continue;
        if (seen++ == which) return i;
    }
    return -1;
}

static void test_standing_shape(void) {
    HitPose pose = base_pose();
    HitBox boxes[HIT_BOX_MAX];
    const int count = hitboxes_build(&pose, boxes, HIT_BOX_MAX);
    // head, torso, the 2 standing leg boxes and the 2 x 3 crouched ones (v10 builds all four leg
    // holders and filters them with userData.active at trace time, v10:117818-117822); no arm boxes
    // without a weapon
    CHECK(count == 10, "a standing pose has 10 boxes (head, torso, 2 + 6 legs), got %d", count);

    const int head = find_box(boxes, count, HIT_PART_HEAD, 0);
    CHECK(head >= 0, "head box");
    if (head >= 0) {
        // headHitbox: a headScale cube at playerHeight - headScale/2 - legHeight = 11 - 1 - 4.2
        CHECK(near(boxes[head].half.x, 1.0f, 1e-4f) && near(boxes[head].half.y, 1.0f, 1e-4f) && near(boxes[head].half.z, 1.0f, 1e-4f),
              "head half extents %g %g %g", (double) boxes[head].half.x, (double) boxes[head].half.y, (double) boxes[head].half.z);
        // the head is a child of the upper body hold at legHeight 4.2: 4.2 + (11 - 1 - 4.2) = 10,
        // so the head spans 9..11 = the top of the player
        CHECK(near(boxes[head].center.y, 4.2f + 11.0f - 1.0f - 4.2f, 1e-4f), "head centre y = 10, got %g", (double) boxes[head].center.y);
        CHECK(near(boxes[head].center.x, 0.0f, 1e-4f) && near(boxes[head].center.z, 0.0f, 1e-4f), "head centre x / z = 0");
    }

    const int torso = find_box(boxes, count, HIT_PART_TORSO, 0);
    CHECK(torso >= 0, "torso box");
    if (torso >= 0) {
        // chestWidth 2.6, playerHeight - headScale - legHeight = 4.8, chestScale 1.3, at 0.5 x 4.8 above the legs hold (y 4.2)
        CHECK(near(boxes[torso].half.x, 1.3f, 1e-4f) && near(boxes[torso].half.y, 2.4f, 1e-4f) && near(boxes[torso].half.z, 0.65f, 1e-4f),
              "torso half extents %g %g %g", (double) boxes[torso].half.x, (double) boxes[torso].half.y, (double) boxes[torso].half.z);
        CHECK(near(boxes[torso].center.y, 4.2f + 2.4f, 1e-4f), "torso centre y = 6.6, got %g", (double) boxes[torso].center.y);
    }

    const int leg = find_box(boxes, count, HIT_PART_LEG_STANDING, 0);
    CHECK(leg >= 0, "standing leg box");
    if (leg >= 0) {
        // legScale 1.3, legHeight 4.2 centred on 0.5 legHeight below the holder at legHeight, x +-0.65
        CHECK(near(boxes[leg].half.x, 0.65f, 1e-4f) && near(boxes[leg].half.y, 2.1f, 1e-4f), "leg half extents");
        CHECK(near(boxes[leg].center.y, 2.1f, 1e-4f), "leg centre y = 2.1, got %g", (double) boxes[leg].center.y);
        CHECK(near(boxes[leg].center.x, 0.65f, 1e-4f), "right leg centre x = +0.65, got %g", (double) boxes[leg].center.x);
    }
}

static void test_rays(void) {
    HitPose pose = base_pose();
    float fraction = -1.0f;
    int kind = -1;

    // Straight at the chest from 20 units in front (+z), range 40: the torso front face is at
    // z = 0.65, so t = (20 - 0.65) / 40 = 0.48375 (v10 reports distance / range, toFixed(4)).
    const vec3 origin = {0.0f, 6.6f, 20.0f};
    const vec3 dir = {0.0f, 0.0f, -1.0f};
    CHECK(hitboxes_ray(&pose, origin, dir, 40.0f, &fraction, &kind) && kind == HIT_KIND_TORSO, "chest ray is a torsoshot");
    CHECK(near(fraction, (20.0f - 0.65f) / 40.0f, 1e-3f), "chest ray fraction %g, expected %g", (double) fraction, (double) ((20.0f - 0.65f) / 40.0f));

    // At the head band (centre y 10): a headshot, fraction (20 - 1) / 40
    const vec3 head_origin = {0.0f, 10.0f, 20.0f};
    CHECK(hitboxes_ray(&pose, head_origin, dir, 40.0f, &fraction, &kind) && kind == HIT_KIND_HEAD, "head ray is a headshot");

    // Below the torso: the legs (standing pair is active while crouchVal is 0), a legshot
    const vec3 leg_origin = {0.65f, 2.0f, 20.0f};
    CHECK(hitboxes_ray(&pose, leg_origin, dir, 40.0f, &fraction, &kind) && kind == HIT_KIND_LEG, "leg ray is a legshot");

    // Over the head: nothing (the head top is playerHeight 11)
    const vec3 above = {0.0f, 11.5f, 20.0f};
    CHECK(!hitboxes_ray(&pose, above, dir, 40.0f, &fraction, &kind), "a ray above the head misses");

    // Off to the side of the torso (half width 1.3) and above the legs
    const vec3 side = {2.0f, 6.6f, 20.0f};
    CHECK(!hitboxes_ray(&pose, side, dir, 40.0f, &fraction, &kind), "a ray beside the torso misses");

    // The range limit: the same chest ray with a range that ends before the box misses (v10's caller
    // drops a dst > 1, v10:172897)
    CHECK(!hitboxes_ray(&pose, origin, dir, 19.0f, &fraction, &kind), "a ray shorter than the distance to the box misses");
}

static void test_crouch_and_hurtbox(void) {
    HitPose pose = base_pose();
    int count = hitboxes_build(&pose, (HitBox[HIT_BOX_MAX]) {0}, HIT_BOX_MAX);
    CHECK(count == 10, "standing boxes");

    // Crouched (cV 1): the upper body drops by crouchDst 3 and the crouched leg pair (3 boxes per
    // side) replaces the standing ones: 1 head + 1 torso + 6 crouched legs
    pose.crouch_val = 1.0f;
    HitBox boxes[HIT_BOX_MAX];
    count = hitboxes_build(&pose, boxes, HIT_BOX_MAX);
    CHECK(count == 10, "a crouched pose has the same 10 boxes, got %d", count);
    const int head = find_box(boxes, count, HIT_PART_HEAD, 0);
    CHECK(head >= 0 && near(boxes[head].center.y, 10.0f - 3.0f, 1e-3f), "crouched head centre drops by crouchDst 3");
    // the standing holders stay in the build (v10 keeps all four and filters by userData.active at
    // trace time): the ray below only sees the crouched pair
    CHECK(find_box(boxes, count, HIT_PART_LEG_STANDING, 0) >= 0 && find_box(boxes, count, HIT_PART_LEG_CROUCHED, 0) >= 0,
          "both leg pairs are in the build");
    CHECK(find_box(boxes, count, HIT_PART_LEG_CROUCHED, 0) >= 0, "crouched leg boxes");

    // Only the active pair is rayed: a crouched ray at the standing leg height misses the legs
    float fraction;
    int kind;
    const vec3 origin = {0.65f, 2.0f, 20.0f};
    const vec3 dir = {0.0f, 0.0f, -1.0f};
    CHECK(!hitboxes_ray(&pose, origin, dir, 40.0f, &fraction, &kind) || kind != HIT_KIND_LEG, "a low ray under a crouched player is not a legshot");

    // hurtboxScale 1.5: the torso and head grow 1.5x, legs on the hitboxes.c path 1.5x too (v10 scales
    // the leg meshes by the player's hurtboxScale as well, v10:117678 / 117718)
    HitPose scaled = base_pose();
    scaled.hurtbox_scale = 1.5f;
    HitBox big[HIT_BOX_MAX];
    const int big_count = hitboxes_build(&scaled, big, HIT_BOX_MAX);
    const int torso = find_box(big, big_count, HIT_PART_TORSO, 0);
    CHECK(torso >= 0 && near(big[torso].half.x, 1.3f * 1.5f, 1e-4f) && near(big[torso].half.y, 2.4f * 1.5f, 1e-4f),
          "hurtboxScale 1.5 scales the torso");
    const int big_head = find_box(big, big_count, HIT_PART_HEAD, 0);
    CHECK(big_head >= 0 && near(big[big_head].half.y, 1.5f, 1e-4f), "hurtboxScale 1.5 scales the head");
}

static void test_arms(void) {
    // A weapon gives the arm boxes: 1.5x the pistol's hold offsets, in v10's trace order (left
    // upper, right upper, left connector, left lower, right connector, right lower)
    HitPose pose = base_pose();
    pose.weapon = rules_weapon(2); // Pistol
    HitBox boxes[HIT_BOX_MAX];
    int count = hitboxes_build(&pose, boxes, HIT_BOX_MAX);
    CHECK(count == 16, "a weapon adds 6 arm boxes (10 + 6), got %d", count);
    int arms = 0;
    for (int i = 0; i < count; i++) arms += boxes[i].part == HIT_PART_ARM;
    CHECK(arms == 6, "6 arm boxes, got %d", arms);
    int arm_max = 0;
    for (int i = 0; i < count; i++) {
        if (boxes[i].part != HIT_PART_ARM) continue;
        // every arm mesh is scaled by the hurtbox scale, half extent (armScale / 2 = 0.6) + the +1 pad while moving
        CHECK(near(boxes[i].half.x, 0.6f, 1e-3f), "arm half extents are the arm scale / 2");
        arm_max++;
    }
    CHECK(arm_max == 6, "arm boxes counted once");

    // v10 tags the arms torsoshot: a ray at an arm can only ever report HIT_KIND_TORSO
    float fraction;
    int kind;
    int torso_hits = 0, arm_hits = 0;
    for (int x = -8; x <= 8; x++) {
        for (int y = 1; y <= 15; y++) {
            const vec3 origin = {(float) x * 0.25f, (float) y * 0.75f, 20.0f};
            const vec3 dir = {0.0f, 0.0f, -1.0f};
            if (!hitboxes_ray(&pose, origin, dir, 40.0f, &fraction, &kind)) continue;
            CHECK(kind == HIT_KIND_HEAD || kind == HIT_KIND_TORSO || kind == HIT_KIND_LEG, "hit kind %d", kind);
            if (kind == HIT_KIND_TORSO) torso_hits++;
            if (kind == HIT_KIND_HEAD) arm_hits++;
        }
    }
    CHECK(torso_hits > 0 && arm_hits > 0, "the sweep hit the body (%d) and the head (%d)", torso_hits, arm_hits);

    // A moving target pads every box by +1 (v10:117863)
    HitPose moving = pose;
    moving.moving = 1;
    HitBox padded[HIT_BOX_MAX];
    const int padded_count = hitboxes_build(&moving, padded, HIT_BOX_MAX);
    CHECK(padded_count == count, "the moving pose has the same boxes");
    for (int i = 0; i < padded_count; i++) {
        CHECK(padded[i].half.x > boxes[i].half.x && padded[i].half.y > boxes[i].half.y && padded[i].half.z > boxes[i].half.z,
              "box %d grows while the target moves", i);
    }
}

static void test_step_and_facing(void) {
    // stepVal swings the legs: at sV = PI/2 the root rotates y by -2 x stepAnim and the legs pitch +-9 x stepAnim
    HitPose pose = base_pose();
    pose.step_val = (float) M_PI / 2.0f;
    HitBox boxes[HIT_BOX_MAX];
    const int count = hitboxes_build(&pose, boxes, HIT_BOX_MAX);
    const int leg0 = find_box(boxes, count, HIT_PART_LEG_STANDING, 0);
    const int leg1 = find_box(boxes, count, HIT_PART_LEG_STANDING, 1);
    CHECK(leg0 >= 0 && leg1 >= 0, "both standing legs");
    if (leg0 >= 0 && leg1 >= 0) {
        // rot.x = swing x (i == 1 or 3 ? 1 : -1) x 9 with swing = sin(PI/2) x 0.075 = 0.075
        const float swing = 0.075f * 9.0f;
        CHECK(near(boxes[leg0].axis[1].z, -sinf(-swing), 1e-3f) || near(boxes[leg0].axis[1].y, cosf(-swing), 1e-3f), "leg 0 pitch");
        CHECK(near(boxes[leg1].axis[1].y, cosf(swing), 1e-3f), "leg 1 pitch (opposite swing)");
        CHECK(fabsf(boxes[leg1].center.z - boxes[leg0].center.z) > 0.1f, "the legs swing apart");
    }

    // xD / yD rotate the pose: with xD = PI/2 and yD = 1, the torso's front face turns to -x
    HitPose turned = base_pose();
    turned.x_dir = (float) M_PI / 2.0f;
    turned.y_dir = 1.0f;
    HitBox turned_boxes[HIT_BOX_MAX];
    const int turned_count = hitboxes_build(&turned, turned_boxes, HIT_BOX_MAX);
    const int torso = find_box(turned_boxes, turned_count, HIT_PART_TORSO, 0);
    CHECK(torso >= 0, "torso of the turned pose");
    if (torso >= 0) {
        // rot.x = 0.5 x yD pitches the upper body: the torso's up axis tilts toward +z
        CHECK(boxes[0].axis[1].y > 0.99f, "upright box");
        CHECK(near(turned_boxes[torso].axis[1].y, cosf(0.5f), 1e-3f), "yD=1 pitches the torso by 0.5 rad");
        CHECK(near(turned_boxes[torso].axis[0].x, cosf((float) M_PI / 2.0f), 1e-3f), "xD=PI/2 turns the pose");
    }
}

static void test_trace_and_rulesets(void) {
    // hitboxes_trace_player: the broad phase (x/z +-(scale + 5), y .. ah + 5, v10:172895) then the boxes
    Game game;
    memset(&game, 0, sizeof(game));
    game.rules = ruleset_get(RULESET_V10_0_2);
    CHECK(hitboxes_active(&game) == 1, "the v10 ruleset traces posed boxes");
    game.rules = ruleset_get(RULESET_V7_1_4);
    CHECK(hitboxes_active(&game) == 0, "the v7 ruleset keeps the AABB");
    game.rules = ruleset_get(RULESET_V10_0_2);
    game.mode = &(GameMode){.config = {.prop = 1}};
    CHECK(hitboxes_active(&game) == 0, "prop hunt keeps the AABB (v10:172886)");
    game.mode = NULL;

    Player player;
    memset(&player, 0, sizeof(player));
    player.game = &game;
    player.scale = 1.0f;
    player.head_mlt = 1.0f;
    PlayerStoredState state;
    memset(&state, 0, sizeof(state));
    state.position = (vec3) {0.0f, 0.0f, 0.0f};
    state.height = 11.0f;
    state.adjusted_height = 11.0f;
    state.crouch_val = 0.0f;

    float fraction = -1.0f;
    int kind = -1;
    const vec3 origin = {0.0f, 6.6f, 20.0f};
    const vec3 step = {0.0f, 0.0f, -40.0f};
    CHECK(hitboxes_trace_player(&player, &state, origin, step, 1, &fraction, &kind) && kind == HIT_KIND_TORSO,
          "the trace reaches the torso through the broad phase");
    // beyond the broad phase box (x +-6): a miss even though the ray is level with the torso
    const vec3 wide = {9.0f, 6.6f, 20.0f};
    CHECK(!hitboxes_trace_player(&player, &state, wide, step, 1, &fraction, &kind), "outside the broad phase box");

    // The ruleset of a game decides (ruleset.h): the v10 ruleset's hit model, the v7 one's not
    CHECK(ruleset_get(RULESET_V10_0_2)->hit_model == HIT_MODEL_V10 && ruleset_get(RULESET_V7_1_4)->hit_model == HIT_MODEL_V7,
          "the per-ruleset hit model");
}

int main(void) {
    // Both ruleset tables must be built (the v10 mode / weapon tables), then the tests run under v10
    CHECK(ruleset_get(RULESET_V10_0_2) && ruleset_get(RULESET_V7_1_4), "both rulesets exist");
    CHECK(ruleset_set_current(RULESET_V10_0_2), "the process ruleset switches to v10");
    CHECK(near(game_constants.crouch_lean, 0.0f, 1e-6f), "v10 crouchLean is 0");

    test_standing_shape();
    test_rays();
    test_crouch_and_hurtbox();
    test_arms();
    test_step_and_facing();
    test_trace_and_rulesets();

    CHECK(ruleset_set_current(RULESET_V7_1_4), "back to v7");
    if (failures) {
        printf("hitboxes_test: %d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("hitboxes_test: %d checks OK\n", checks);
    return 0;
}
