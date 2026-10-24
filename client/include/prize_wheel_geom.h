#pragma once
// The GL-free half of the v10.0.3 map prize wheel drawing (client/src/prize_wheel_geom.c; the canvas and the mesh are
// client/src/prize_wheel_render.c): render.addSpinner's canvas size (deob-10.0.3.js:232681-232707) and what
// game.updateSpinners paints into it every 50 ms step while spinInfo.needsUpdate (:219576-219675), in canvas px about
// the canvas centre (canvas angles: 0 = +x, clockwise on screen, y down):
//   the wheel   a white disc of R = width / 2 * 0.95 * spinWhlMlt, then one pie slice per rarity (store.rarities,
//               wheel.rarities[i] %) or pool (wheel.pools, poolWeights[i] %) of radius R * 0.95, coloured
//               store.rarities[i].color, from angle 0 on; once stopped (endAnimTarget) the won slice widens by
//               endAnimVal while the others give it room and the wheel turns so the won slice ends under the pointer
//   the pointer a #F44336 triangle (0, -1.2 * R * 0.6 * k), (+-R * 0.6 * k, 0) turned by spinRotation,
//               k = 1 - endAnimVal / endAnimTarget (it shrinks away as the slice widens), with a black 20 px shadow
//   the hub     a white disc of R * 0.6; without the won item's model (meshPrev) a #F44336 disc of R * 0.6 * 0.85,
//               "SPIN" (GameFont R * 0.6 * 0.3 px, baseline bottom, y = R * 0.6 * 0.1) and "-<price> KR" (alpha 0.7,
//               R * 0.6 * 0.15 px, baseline top, y = R * 0.6 * 0.25), white
#include <stddef.h>

#define PRIZE_WHEEL_GEOM_SLICES 8   // store.rarities.length (8) or a wheel's pools (STORE_WHEEL_SLICES)
#define PRIZE_WHEEL_POINTER_MLT 0.6 // iîÏiíîì = 0.6 (:219433)
#define PRIZE_WHEEL_SHADOW_BLUR 20.0 // shadowBlur = 20 (:219627)

typedef struct {
    int canvas;              // the canvas' width = height (px)
    int pool;                // wheel.pools: weights[] are poolWeights
    int count;               // slices: pools.length, or store.rarities.length (rarity wheels)
    double weights[PRIZE_WHEEL_GEOM_SLICES]; // the slices' %; a rarity wheel's missing rarities (i >= weight_count) are undefined
    int weight_count;        // wheel.rarities.length / poolWeights.length
    int won_rarity;          // the won item's rarity (store.skins[itemID].rarity), -1 none
    float spin_whl_mlt, end_anim_val, end_anim_target, spin_rotation; // spinInfo
    int item_shown;          // meshPrev: the won item's model is up (no SPIN / price label)
} PrizeWheelState;

typedef struct {
    double start, sweep;     // the arc's start angle and its extent (radians); sweep <= 0: not drawn (`if (w > 0)`)
} PrizeWheelSlice;

typedef struct {
    double centre;           // canvas.width / 2 (= height / 2)
    double radius;           // R (iïIïiíï)
    double slice_radius;     // R * 0.95
    double turn;             // the rotate() before the slices (the stop's turn), 0 while spinning
    int count;               // slices[] entries (a NaN start ends the list: the rest of v10's arcs are NaN too)
    PrizeWheelSlice slices[PRIZE_WHEEL_GEOM_SLICES];
    double pointer_rotation; // spinRotation
    double pointer[3][2];    // the triangle before its rotation
    double hub_radius;       // R * 0.6
    int label;               // !meshPrev: the red disc and its two lines
    double label_radius;     // R * 0.6 * 0.85
    double spin_font, spin_y;   // "SPIN"
    double price_font, price_y; // "-<price> KR"
} PrizeWheelLayout;

// render.addSpinner: min(max(size / 7.5 * 265, 768), 4096) px, as canvas.width truncates it; size = the object's sy.
int prize_wheel_canvas_px(float size);
// The drawing of one updateSpinners step.
void prize_wheel_layout(const PrizeWheelState *state, PrizeWheelLayout *out);

// The canvas' Gaussian shadow (shadowBlur b: sigma = b / 2) as `levels` nested copies of a shape grown by
// grow[i] px (largest first), each filled black at alpha[i]: stacked, the band between grow[i + 1] and grow[i] has
// the shadow's coverage at its outer edge, 0.5 erfc(t / (sigma sqrt 2)) at distance t outside the shape. Returns
// the levels written (<= max).
int prize_wheel_shadow_levels(double blur, int max, double *grow, double *alpha);

// The won item (meshPrev, :219485-219574): a frame at the wheel's centre turned rotateY(PI - dir * PI / 4), meshPrev in
// it at (-width / 8 * 5, 1 + yOff, xOff) (width = the spinner's w = sy / 4), rotateX(type ? 0 : PI / 11) then rotateY(PI / 2
// for a spray or a dye), scaled width / 4; the model in meshPrev turned item_rot_y (type && type != 3 ? PI : PI / 2; a
// spray's plane PI) and scaled (spnScl || scl || 1) * 2. yOff / xOff / scl are the item's own, a weapon skin's from
// store.getPreview. Each step bobs meshPrev: y = orgYP + sin(bobAnim) * 0.55.
typedef struct {
    float frame_yaw;     // the frame's rotation.y
    float position[3];   // meshPrev.position in the frame
    float rotation[2];   // meshPrev.rotation x, y (Euler XYZ: rotateX, then rotateY)
    float scale;         // meshPrev.scale (all axes)
    float item_rot_y;    // the model's rotation.y
    float item_scale;    // the model's scale (all axes); a spray's plane is (9, 7.2, 1) instead
} PrizeWheelItemPose;

#define PRIZE_WHEEL_ITEM_SPRAY 4 // store type 4: createSpray's plane
#define PRIZE_WHEEL_ITEM_DYE 5   // store type 5: a class-9 preview player wearing it
#define PRIZE_WHEEL_ITEM_MELEE 3

void prize_wheel_item_pose(float size, float dir, int type, float y_off, float x_off, float scl, float spn_scl, PrizeWheelItemPose *out);
// meshPrev.getWorldPosition() (:219571): the frame's centre (the wheel's centre) + rotateY(frame_yaw) of meshPrev's position
void prize_wheel_item_world(const float centre[3], const PrizeWheelItemPose *pose, float out[3]);
// meshPrev.position.y after a step: orgYP + sin(bobAnim) * 0.55 (:219576)
float prize_wheel_item_bob(float org_y, float bob_anim);

// The plane (the mesh's world transform): render.addSpinner's mesh at the object's bottom centre + sy / 2, rotateY
// (PI / 2 - dir * PI / 4), scaled sy x sy. *yaw is that rotation.
void prize_wheel_plane(const float position[3], float size, float dir, float centre[3], float *yaw);
