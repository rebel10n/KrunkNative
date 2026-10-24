#include <math.h>
#include <shared.h>

static const float arm_scale = 1.2f;
static const float arm_inset = -0.1f;
static const float chest_width = 2.6f;

static const float player_height = 11.0f;
static const float leg_height = 4.2f;
static const float head_scale = 2.0f;

// Krunker v10.0.3's physics and animation constants, each checked against the decoded bundle: the frozen
// config object (deob-10.0.3.js:22367-23014; a `var _N = <num>` value is resolved at the line given) and,
// for the movement knobs the bundle keeps as class-editor defaults, their `def` (grA / arA / crchSpd,
// deob-10.0.3.js:20415-20466) and the procInputs code that reads them (deob-10.0.3.js:191622-191631).
// The values are v7.1.4's but for crouchLean. KrunkNative stores per-ms values per second (x 1000) and
// times in seconds (doc/v10/gameplay.md section 1). v7.1.4's playerSpeed / airSpeed / slippingSpeed,
// terrainSlideThreshold, terrainSlipDecel and playerSlippingJumpCooldown are not in v10.0.3 (and no v7
// code read them): the speeds are the knobs below, the other four are gone (doc/v10/constants-1003.md).
// The hurtbox scale and the posed hit boxes are rules, not constants (shared/src/hitboxes.c).
const GameConstants game_constants = {
    .billboard_count = 6, // config.billboards (deob-10.0.3.js:20134): six posters after "Random", the bb range 1..length - 1 (:189337)
    .mouse_sensitivity = 0.0024f, // mouseSens (deob-10.0.3.js:22765, :21162)
    .world_uv_scale = 60.0f,      // worldUV (:23008, :21269)
    .player_height = player_height, // playerHeight 11 (:22796, :21182)
    .border_height = 500.0f, // borderH (:22454, :21324; v3.1.1: 250)
    .camera_height = 1.5f,   // cameraHeight (:22465, :21172)
    .climb_height = 3.0f,    // climbHeight (:22478)
    // procInputs' floor at deltaMlt 1: Math.max(1 * deltaMlt, Math.min(input[1], dltMx)) ms, a smaller
    // delta becomes the floor (deob-10.0.3.js:191276-191280; v7.1.4: 0.1 ms)
    .min_delta = 0.001f,
    .max_delta = 0.033f,     // dltMx = Math.round(1000 / 30) = 33 ms (:19242, :22516)
    .jump_velocity = 0.072f, // jumpVel (:22648)
    .jump_aim_slow = 0.85f,  // aimJumpSlow (:22383)
    .jump_push = 0.1f,       // jumpPush (:22647)
    .crouch_jump = 0.25f,    // crouchJump (:22491)
    .crouch_speed = 7.0f,    // crouchSpd 0.007 per ms (:22493, :21186)
    .crouch_lean = 0.0f,     // crouchLean 0 (:22492; v7 config.js:2164 -(PI * 0.1)): no crouch lean in the pose or the hitboxes
    .player_speed = 0.42f,   // the grA knob 0.042 / 100 per ms (def :20418, read :191624)
    .air_speed = 0.047f,     // the arA knob 0.0047 / 100 per ms (def :20438, read :191624)
    .aim_slow = 0.55f,       // aimSlow (:22384; procInputs `aimVal ? 1 : 0.55`, :191625)
    .crouch_slow = 0.3f,     // the crchSpd knob (def :20465, read :191627)
    .terrain_slide_decel = 0.9996f, // terrainSlideDecel (:22897, :21174)
    .terrain_decel = 0.99f,  // terrainDecel (:22895; procInputs `onTerrain ? 0.99`, :191631)
    .slide_decel = 0.999f,   // slideDecel (:22854)
    .air_decel = 0.9996f,    // airDecel (:22386, :21176)
    .ground_decel = 0.99f,   // groundDecel (:22610, :21175)
    .ladder_decel = 0.97f,   // ladderDecel (:22663)
    .crouch_distance = 3.0f, // crouchDst (:22490)
    .gravity = 0.15f,        // gravity 0.00015 per ms (:22609, :21132)
    .arm_scale = arm_scale,     // armScale (:22401)
    .arm_inset = arm_inset,     // armInset (:22399)
    .chest_width = chest_width, // chestWidth (:22472)
    .chest_scale = 1.3f,        // chestScale (:22471)
    .leg_scale = 1.3f,          // legScale (:22678)
    .leg_height = leg_height,   // legHeight (:22677, :21184)
    .head_scale = head_scale,   // headScale (:22616)
    .body_height = player_height - head_scale - leg_height,
    .player_scale = (arm_scale * 2.0f + chest_width + arm_inset) * 0.5f, // playerScale 2.45 (:22797, :21185)
    .min_decel = 0.0001f,    // decelMin (:22507, :21177)
    .slide_time = 0.35f,     // slideTime 350 ms (:22855)
    .player_slide_velocity_mlt = 1.2f,         // playerSlideVelMlt (:22798)
    .player_terrain_slide_velocity_mlt = 0.4f, // playerTerrainSlideVelMlt (:22799)
    .idle_anim_speed = 1.5f, // idleAnimS 0.0015 per ms (:22622, :21166)
    .interpolation = 1.1f,   // interpolation (:22631)
    .lean_sensitivity = 0.05f, // leanSens (:22676, :21169)
    .lean_max = 0.16f,       // leanMax (:22672, :21170)
    .lean_pull = 0.99f,      // leanPull (:22674, :21168)
    .lean_pull_z = 0.99f,    // leanPullZ (:22675)
    .lean_mlt_z = 0.03f,     // leanMltZ (:22673)
    .bob_pull_y = 0.985f,    // bobPullY (:22450)
    .bob_pull_z = 0.99f,     // bobPullZ (:22451)
    .bob_mlt_y = 0.024f,     // bobMltY (:22448)
    .bob_mlt_z = 0.02f,      // bobMltZ (:22449)
    .crouch_anim = 0.8f,     // crouchAnim 0.0008 per ms (:22488, :21187)
    .collision_padding = 1.1f, // collPad (:22480, :21171)
    .booster_speed = 2.0f,   // boosterSpd 0.002 per ms (:22453)
    .ladder_speed = 35.0f,   // ladderSpeed 0.035 per ms (:22665, :21173)
    .ladder_scale = 0.5f,    // ladderScale (:22664, :21139)
    .ladder_width = 3.2f,    // ladderWidth (:22666, :21138)
    .death_y = -100.0f,      // deathY (:22506, :21133)
    .recoil_mlt = 0.3f,      // recoilMlt (:22816, :21189)
    .spread_adjustment = 0.00063f, // spreadAdj (:22872, :21188)
    .upper_arm_length = 2.7f, // uArmLength (:22984)
    .lower_arm_length = 2.7f, // lArmLength (:22662)
    .arm_offset = -0.8f,     // armOff (:22400, :21183)
    .anim_mlt = 1.2f,        // animMult (:22393, :21167)
    .aim_anim_mlt = 0.18f,   // aimAnimMlt (:22382)
    .crouch_anim_mlt = 0.5f, // crouchAnimMlt (:22489)
    .step_anim = 0.075f,     // stepAnim (:22882)
    .step_pull = 0.995f,     // stepPull (:22884)
    .disk_spin = 0.2f,       // diskSpin 0.0002 per ms (:22515, :21137)
    .wrist_cuff = 0.225f,    // wristCuff (:23009)
    .wrist_scale = 0.5f,     // wristScale (:23010)
};
