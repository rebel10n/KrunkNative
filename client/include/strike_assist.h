#pragma once
// Krunker Strike's mobile aim assist and auto-fire (FRVR Krunker Strike 1.37.2, the gameLoop pass
// after the render: vbe / gbe / mbe / Tbe target pick, Pbe / Bbe state, GE / fw look pull, and the
// shoot key it drives). The constants are data: StrikeRules.aim_assist (shared/include/strike_rules.h,
// tools/strike/strike_data.json "input.aim_assist"). Only a mobile client under the Strike flavor runs
// it (client/src/strike_assist_client.c); the base game has no aim assist.
//
// This file is the GL-free core: screen-space maths on targets the caller projected (Vector3.project:
// x, y in 0..1, y up), so client/tests/strike_assist_test.c drives it without a window.
#include <stddef.h>
#include <strike_rules.h>

typedef struct {
    int uid;                      // _m.id: the tracked target, -1 none
    unsigned char target_jumping; // _m.jumping: !onGround when it was picked
    unsigned char locked;         // zM: the horizontal lock (pull_locked from now on)
    unsigned char slowed;         // VM: fw() = locked_look_mlt
    unsigned char has_dist;       // aimAssistDist != null
    unsigned char has_dist_y;     // aimAssistDist[1] != null
    float dist_x, dist_y;         // aimAssistDist: the target's screen offset from the centre (fraction, y up)
    unsigned char autofiring;     // controls.autoFiring
    unsigned char shoot_key;      // keys[binds.shoot] as auto-fire leaves it
    double last_auto_fired_ms;    // controls.lastAutoFired
} StrikeAssist;

typedef struct {
    int uid;
    float sx, sy;                 // projected aim point (0..1, x right, y up)
    float dist;                   // camera to aim point
    unsigned char in_frustum;     // camera frustum contains the aim point
    unsigned char visible;        // cnBSeen || !canSee(camera, point, pad)
    unsigned char on_ground;
} StrikeAssistTarget;

typedef struct {
    float width, height;          // window.innerWidth / innerHeight (CSS px)
    float zoom;                   // renderer zoomVal
    unsigned char mobile;         // window._isMobile
    unsigned char aim_assist;     // settings aimAssist
    unsigned char auto_fire;      // settings autoFireMbl
    unsigned char joystick_active;// joystick.active || moveJoystick.active
    unsigned char semi_auto;      // the held weapon's nAuto
    unsigned char friendly;       // mode.friendly: no assist
    double now_ms;
} StrikeAssistFrame;

void strike_assist_reset(StrikeAssist *a);
// mbe: is the projected point (px, py: pixels, y down) inside the box around the screen centre?
// *semi_fire: the base box held by a semi-auto weapon; *on_target: the grown box (or the base one).
void strike_assist_box(const StrikeAimAssist *k, float width, float height, float px, float py, float dist, float zoom,
                       int semi_auto, int *semi_fire, int *on_target);
// Abe: the pass runs on a phone with auto-fire or aim assist on, while a joystick is held or a
// target is already tracked.
int strike_assist_active(const StrikeAssist *a, const StrikeAssistFrame *f);
// One gameLoop pass over the candidate targets (the order the game lists its players). Returns the
// shoot key the pass leaves while auto-fire drives it (0 / 1), or -1 when the player's own input rules.
int strike_assist_frame(const StrikeAimAssist *k, StrikeAssist *a, const StrikeAssistFrame *f, const StrikeAssistTarget *targets,
                        size_t count);
// fw(): the look multiplier (locked_look_mlt while slowed, else 1).
float strike_assist_look_mlt(const StrikeAimAssist *k, const StrikeAssist *a);
// updateMouseInp's pull: adds aimAssistDist * innerWidth|Height * GE(dt) / fw() to the look sums
// (movementX / movementY px, before sensitivity); GE also flips the horizontal lock. dt in seconds.
void strike_assist_pull(const StrikeAimAssist *k, StrikeAssist *a, float dt, float width, float height, float *look_x, float *look_y);

