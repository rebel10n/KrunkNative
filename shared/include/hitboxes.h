#pragma once
// Krunker v10.0.2's hit model (doc/v10/gameplay.md section 3): the posed oriented-box hit boxes
// ("complexHitboxes") the v10 server traces shots and projectiles against, instead of v7.1.4's
// single padded AABB. v10 builds them as a THREE hierarchy (players.updateComplexHitboxes
// v10:169443-169498, the arm / leg builders v10:117609-117724), poses it from the lag-comp state
// (v10:117728-117831) and rays it with OBB.intersectRay (v10:117833-117946). shared/src/hitboxes.c
// is the same hierarchy and the same ray in plain C over PlayerStoredState, with no renderer types.
//
// Every box carries v10's hitType: headshot (head), torsoshot (torso and the six arm boxes) and
// legshot (the leg boxes). v10 tags the arms torsoshot, so an arm hit is body damage; "armshot"
// only appears in v10's damage checks (v10:172979, 180187) and nothing in 10.0.2 produces it.
// Every mode but Prop Hunt traces them (hitboxes_active); Prop Hunt keeps players.js' padded AABB.
#include <shared.h> // vec3, Player, PlayerStoredState, Weapon

typedef enum {
    HIT_KIND_HEAD = 0,  // headshot
    HIT_KIND_TORSO = 1, // torsoshot (torso and arms)
    HIT_KIND_LEG = 2,   // legshot
} HitKind;

// One oriented box of the posed hierarchy (OBB.applyMatrix4 of the box mesh's matrixWorld).
typedef struct {
    vec3 center;
    vec3 half;    // half extents along axis[0..2]
    vec3 axis[3]; // unit axes (the world rotation's columns)
    int kind;     // HitKind
    int part;     // HitPart
} HitBox;

typedef enum {
    HIT_PART_HEAD,
    HIT_PART_TORSO,
    HIT_PART_ARM,          // the six arm boxes, v10's trace order (see hitboxes_build)
    HIT_PART_LEG_STANDING, // legsHoldObj children 0 / 1 (userData.active when cV == 0)
    HIT_PART_LEG_CROUCHED, // children 2 / 3 (active when cV != 0)
} HitPart;

// What the v10 pose reads. The state fields come from the lag-comp entry (players.storeState
// v10:170708-170722); the rest is the target's live value, as v10 reads it off the player object.
typedef struct {
    vec3 position;       // state x, y, z (feet)
    float x_dir, y_dir;  // state xD, yD
    float step_val;      // state sV
    float step_chase;    // state stepChase
    float crouch_val;    // state cV (0 / 1)
    const Weapon *weapon;           // the state's weapon (NULL: none, no arm boxes)
    float hurtbox_scale; // live: game hurtboxScale x class hurtboxScale (v10:169065), 0 = 1
    float head_mlt;      // live headMlt, 0 = 1
    float recoil_anim_y; // live recoilAnimY (the arm holder's height, v10:117779)
    unsigned char moving;// live velocity != 0: every box grows by 1 (v10:117863)
    unsigned char no_bob;// class config.mdl: no step bob on the root (v10:117755)
} HitPose;

#define HIT_BOX_MAX 20 // head, torso, 6 arms, 2 standing legs, 2 x 3 crouched legs (16 today)

// The posed boxes in v10's trace order: head, torso, the arms (left upper, right upper, left
// connector, left lower, right connector, right lower; only with a weapon), then the legs:
// right standing, left standing, right crouched (upper, connector, lower), left crouched (same).
int hitboxes_build(const HitPose *pose, HitBox *out, int max);

// v10's narrow phase (_40442): a ray from `origin` along the unit `dir` against the posed boxes.
// Head, then torso; the arms only when the torso missed (first arm hit only); then the active leg
// pair (first hit only); the nearest of those. *fraction is distance / range rounded to 4
// decimals like v10's toFixed(4) (it is not clamped to 1, as in v10). 0 = "nearmiss".
int hitboxes_ray(const HitPose *pose, vec3 origin, vec3 dir, float range, float *fraction, int *kind);

// The pose of `target` in its lag-comp `state` (the fields above).
void hitboxes_pose_player(const Player *target, const PlayerStoredState *state, HitPose *out);

// v10's player test for a shot / projectile ray `origin` + `step` (step = dir x range): with
// `broad`, first lineInRect against x/z +-(scale + 5), y .. y + ah + 5 (v10:172895, 180125; a
// hitBox projectile gates on its sphere instead, v10:180125), then the pose and hitboxes_ray.
// 1 on a hit with *fraction (0..) and *kind.
int hitboxes_trace_player(const Player *target, const PlayerStoredState *state, vec3 origin, vec3 step, int broad,
                          float *fraction, int *kind);

// 1 when `game` traces players with these boxes: every mode but Prop Hunt (v10 keeps the padded
// AABB there, v10:172886).
int hitboxes_active(const struct Game_t *game);
