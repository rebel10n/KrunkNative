// Krunker Strike's mobile aim assist and auto-fire (client/src/strike_assist.c) with FRVR 1.37.2's
// constants (strike_rules()->aim_assist, tools/strike/strike_data.json): the target box, the active
// rule, the look pull and its horizontal lock, the vertical deadband, fw(), and auto-fire for automatic
// and semi-automatic weapons. Run: ./<build>/strike_assist_test
#include <strike_assist.h>
#include <math.h>
#include <stdio.h>

static int g_failures, g_checks;
#define CHECK(cond, ...) do { \
    g_checks++; \
    if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_failures++; } \
} while (0)

static int near(const float a, const float b) { return fabsf(a - b) <= 1e-4f * fmaxf(1.0f, fabsf(b)); }

static StrikeAssistFrame frame(const double now, const int semi) {
    return (StrikeAssistFrame) {.width = 1000.0f, .height = 500.0f, .zoom = 1.0f, .mobile = 1, .aim_assist = 1, .auto_fire = 1,
                                .joystick_active = 1, .semi_auto = (unsigned char) semi, .now_ms = now};
}

static StrikeAssistTarget target(const int uid, const float sx, const float sy, const float dist) {
    return (StrikeAssistTarget) {.uid = uid, .sx = sx, .sy = sy, .dist = dist, .in_frustum = 1, .visible = 1, .on_ground = 1};
}

int main(void) {
    const StrikeAimAssist *k = &strike_rules()->aim_assist;
    CHECK(k->grow_width == 1.5f && k->grow_height == 1.1f && k->pull_initial == 2.0f && k->pull_locked == 3.0f &&
              k->locked_look_mlt == 0.75f && k->autofire_buffer_ms == 260, "FRVR 1.37.2 constants");

    // ---- mbe: base half size playerScale * .5 / dist (2.15 * .5 / 20 = 0.05375 of the width),
    // playerHeight * .5 / dist (11 * .5 / 20 = 0.275 of the height)
    const float W = 1000.0f, H = 500.0f, dist = 20.0f;
    const float half_w = W * game_constants.player_scale * 0.5f / dist, half_h = H * game_constants.player_height * 0.5f / dist;
    int semi, on;
    strike_assist_box(k, W, H, W * 0.5f, H * 0.5f, dist, 1.0f, 1, &semi, &on);
    CHECK(semi && on, "semi-auto, centred: fire and on target");
    strike_assist_box(k, W, H, W * 0.5f, H * 0.5f, dist, 1.0f, 0, &semi, &on);
    CHECK(!semi && on, "automatic, centred: on target only (no base-box check)");
    strike_assist_box(k, W, H, W * 0.5f + half_w * 1.4f, H * 0.5f, dist, 1.0f, 1, &semi, &on);
    CHECK(!semi && on, "1.4 x the base width: outside the base box, inside the x1.5 grown one");
    strike_assist_box(k, W, H, W * 0.5f + half_w * 1.6f, H * 0.5f, dist, 1.0f, 1, &semi, &on);
    CHECK(!semi && !on, "1.6 x the base width: off target");
    strike_assist_box(k, W, H, W * 0.5f, H * 0.5f + half_h * 1.05f, dist, 1.0f, 0, &semi, &on);
    CHECK(on, "1.05 x the base height: inside the x1.1 grown box");
    strike_assist_box(k, W, H, W * 0.5f, H * 0.5f + half_h * 1.15f, dist, 1.0f, 0, &semi, &on);
    CHECK(!on, "1.15 x the base height: off target");

    // ---- Abe
    StrikeAssist a;
    strike_assist_reset(&a);
    StrikeAssistFrame f = frame(0.0, 0);
    CHECK(strike_assist_active(&a, &f), "mobile + a joystick held: active");
    f.joystick_active = 0;
    CHECK(!strike_assist_active(&a, &f), "no joystick, nothing tracked: idle");
    a.has_dist = 1;
    CHECK(strike_assist_active(&a, &f), "no joystick but tracking: active");
    f.aim_assist = f.auto_fire = 0;
    CHECK(!strike_assist_active(&a, &f), "both settings off: idle");
    f = frame(0.0, 0);
    f.mobile = 0;
    CHECK(!strike_assist_active(&a, &f), "desktop: never");

    // ---- an automatic weapon on a centred target: tracking, slowed look, shoot held
    strike_assist_reset(&a);
    StrikeAssistTarget t[2] = {target(7, 0.52f, 0.5f, dist), target(9, 0.5f, 0.5f, dist)};
    f = frame(1000.0, 0);
    int key = strike_assist_frame(k, &a, &f, t, 2);
    CHECK(key == 1 && a.autofiring && a.uid == 7, "auto weapon: the first target in range, shoot held (key %d, uid %d)", key, a.uid);
    CHECK(a.has_dist && near(a.dist_x, 0.02f) && !a.has_dist_y, "aimAssistDist [.02, null] (inside the vertical deadband)");
    CHECK(near(strike_assist_look_mlt(k, &a), 0.75f), "fw() = 0.75 once a target is held");
    // the pull: dist.x * innerWidth * pull_initial * dt / fw
    float lx = 0.0f, ly = 0.0f;
    strike_assist_pull(k, &a, 0.016f, W, H, &lx, &ly);
    CHECK(near(lx, 0.02f * W * 2.0f * 0.016f / 0.75f) && ly == 0.0f && !a.locked, "initial pull %g", (double) lx);
    // locked on the same player: a centred player 9 is ignored while 7 is tracked
    t[0].sx = 0.501f;
    f.now_ms = 1016.0;
    strike_assist_frame(k, &a, &f, t, 2);
    CHECK(a.uid == 7, "stays on the tracked player");
    lx = 0.0f;
    strike_assist_pull(k, &a, 0.016f, W, H, &lx, &ly);
    CHECK(a.locked && near(lx, 0.001f * W * 2.0f * 0.016f / 0.75f), "|dist.x| < .005 with no vertical pull: the lock flips on");
    lx = 0.0f;
    strike_assist_pull(k, &a, 0.016f, W, H, &lx, &ly);
    CHECK(near(lx, 0.001f * W * 3.0f * 0.016f / 0.75f), "locked: pull_locked 3");
    // the vertical pull outside the deadband but inside the box (y up .76: 120 px from the top, the box
    // starts at 250 - 151, the deadband at 250 - 82.5), x .3 while the target is grounded
    t[0].sy = 0.76f;
    f.now_ms = 1032.0;
    strike_assist_frame(k, &a, &f, t, 2);
    CHECK(a.has_dist_y && near(a.dist_y, 0.26f), "vertical pull outside the deadband (%g)", (double) a.dist_y);
    lx = ly = 0.0f;
    strike_assist_pull(k, &a, 0.016f, W, H, &lx, &ly);
    CHECK(near(ly, -0.26f * H * 3.0f * 0.3f * 0.016f / 0.75f), "locked vertical pull x0.3 (%g)", (double) ly);
    t[0].on_ground = 0;
    strike_assist_frame(k, &a, &f, t, 2);
    lx = ly = 0.0f;
    strike_assist_pull(k, &a, 0.016f, W, H, &lx, &ly);
    CHECK(near(ly, -0.26f * H * 3.0f * 0.016f / 0.75f), "airborne target: full vertical pull");

    // the target leaves: everything released, auto-fire holds 260 ms then lets go
    f.now_ms = 1100.0;
    key = strike_assist_frame(k, &a, &f, NULL, 0);
    CHECK(key == 1 && !a.has_dist && a.uid == -1 && !a.locked && !a.slowed, "target lost: tracking released, shoot still held (%d)", key);
    f.now_ms = 1048.0 + 260.0 + 1.0;
    key = strike_assist_frame(k, &a, &f, NULL, 0);
    CHECK(key == -1 && !a.autofiring && !a.shoot_key, "260 ms after the last on-target frame: released (%d)", key);

    // ---- a semi-automatic weapon: the key toggles every frame while the base box holds
    strike_assist_reset(&a);
    StrikeAssistTarget c = target(3, 0.5f, 0.5f, dist);
    f = frame(2000.0, 1);
    const int k1 = strike_assist_frame(k, &a, &f, &c, 1);
    f.now_ms += 16.0;
    const int k2 = strike_assist_frame(k, &a, &f, &c, 1);
    f.now_ms += 16.0;
    const int k3 = strike_assist_frame(k, &a, &f, &c, 1);
    CHECK(k1 == 1 && k2 == 0 && k3 == 1, "semi-auto: press, release, press (%d %d %d)", k1, k2, k3);
    // on target in the grown box only: no shot, the key stays
    c.sx = 0.5f + 1.4f * (half_w / W);
    f.now_ms += 16.0;
    const int k4 = strike_assist_frame(k, &a, &f, &c, 1);
    CHECK(k4 == 1 && a.has_dist, "semi-auto outside the base box: tracked, no new shot (%d)", k4);
    // lost: a semi-auto weapon releases at once
    f.now_ms += 16.0;
    CHECK(strike_assist_frame(k, &a, &f, NULL, 0) == -1, "semi-auto: released when the target is lost");

    // aim assist off, auto-fire on: shoots but no pull
    strike_assist_reset(&a);
    f = frame(3000.0, 0);
    f.aim_assist = 0;
    c = target(3, 0.52f, 0.5f, dist);
    CHECK(strike_assist_frame(k, &a, &f, &c, 1) == 1 && !a.has_dist, "auto-fire without aim assist: no aimAssistDist");
    // friendly modes and hidden targets
    strike_assist_reset(&a);
    f = frame(4000.0, 0);
    f.friendly = 1;
    CHECK(strike_assist_frame(k, &a, &f, &c, 1) == -1, "mode.friendly: nothing");
    f.friendly = 0;
    c.visible = 0;
    CHECK(strike_assist_frame(k, &a, &f, &c, 1) == -1, "a target behind a wall: nothing");

    if (g_failures) {
        printf("strike_assist_test: %d of %d checks failed\n", g_failures, g_checks);
        return 1;
    }
    printf("strike_assist_test: all %d checks passed\n", g_checks);
    return 0;
}
