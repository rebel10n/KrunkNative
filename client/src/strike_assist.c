// Krunker Strike's mobile aim assist and auto-fire, the GL-free core (client/include/strike_assist.h).
// FRVR Krunker Strike 1.37.2 (assets/index-*.js, the module after ControlBind): mbe (box), Tbe (vertical
// deadband), gbe / vbe (target pick), Pbe / Bbe (state), GE / fw (look pull), and gameLoop's auto-fire.
#include <strike_assist.h>
#include <math.h>

void strike_assist_reset(StrikeAssist *a) {
    *a = (StrikeAssist) {.uid = -1};
}

static int point_in_box(const float x, const float y, const float x0, const float y0, const float x1, const float y1) {
    return x >= x0 && x <= x1 && y >= y0 && y <= y1; // UTILS.pointInBox
}

void strike_assist_box(const StrikeAimAssist *k, const float width, const float height, const float px, const float py, const float dist,
                       const float zoom, const int semi_auto, int *semi_fire, int *on_target) {
    const GameConstants *c = &game_constants; // playerScale / playerHeight: Strike's equal v10's (strike_rules_test)
    const float d = c->player_scale * k->box_scale_mlt / dist * zoom;
    const float p = c->player_height * k->box_height_mlt / dist * zoom;
    float half_h = height * p, half_w = width * d;
    int u = 0, f = 0;
    if (semi_auto && point_in_box(px, py, width * 0.5f - half_w, height * 0.5f - half_h, width * 0.5f + half_w, height * 0.5f + half_h)) {
        u = f = 1;
    }
    if (!u) {
        half_h *= k->grow_height;
        half_w *= k->grow_width;
        if (point_in_box(px, py, width * 0.5f - half_w, height * 0.5f - half_h, width * 0.5f + half_w, height * 0.5f + half_h)) f = 1;
    }
    *semi_fire = u;
    *on_target = f;
}

int strike_assist_active(const StrikeAssist *a, const StrikeAssistFrame *f) {
    const int idle = !f->joystick_active;           // !joystick.active && !moveJoystick.active
    const int wanted = f->auto_fire || f->aim_assist;
    return f->mobile && wanted && !(idle && !a->has_dist);
}

// Pbe / Bbe: the pass found a target (its aim point, y cleared by Tbe) or none.
static void track(StrikeAssist *a, const StrikeAssistFrame *f, const float x, const float y, const int has_y) {
    if (!f->aim_assist) return; // Mbe(settings): aimAssist
    a->has_dist = 1;
    a->dist_x = x - 0.5f;
    a->has_dist_y = has_y && y != 0.0f; // `i.y ? i.y - .5 : null`
    a->dist_y = a->has_dist_y ? y - 0.5f : 0.0f;
}

static void release(StrikeAssist *a) {
    a->slowed = 0;  // mB() && yH()
    a->locked = 0;  // wH() && bH()
    a->has_dist = a->has_dist_y = 0;
    a->dist_x = a->dist_y = 0.0f;
    a->uid = -1;    // gH(): _m = {id: null, jumping: false}
    a->target_jumping = 0;
}

int strike_assist_frame(const StrikeAimAssist *k, StrikeAssist *a, const StrikeAssistFrame *f, const StrikeAssistTarget *targets,
                        const size_t count) {
    int found = 0, semi_fire = 0;
    float fx = 0.0f, fy = 0.0f;
    int fy_set = 0;
    if (strike_assist_active(a, f) && !f->friendly) {
        for (size_t i = 0; i < count && !found; i++) {
            const StrikeAssistTarget *t = &targets[i];
            if (a->uid >= 0 && t->uid != a->uid) continue; // locked on one player
            if (!t->in_frustum || !t->visible || !(t->dist > 0.0f)) continue; // pbe
            const float px = t->sx * f->width, py = (1.0f - t->sy) * f->height;
            int u, on;
            strike_assist_box(k, f->width, f->height, px, py, t->dist, f->zoom, f->semi_auto, &u, &on);
            if (!u && !on) continue;
            // Tbe: no vertical pull while the point is within the deadband around the centre
            const float up = game_constants.player_height * k->deadband_up / t->dist * f->zoom;
            const float down = game_constants.player_height * k->deadband_down / t->dist * f->zoom;
            const float lo = f->height * 0.5f - f->height * down, hi = f->height * 0.5f + f->height * up;
            fx = t->sx;
            fy = t->sy;
            fy_set = !(py >= lo && py <= hi);
            a->uid = t->uid; // gH: _m = {id, jumping: !onGround}, refreshed every frame it holds
            a->target_jumping = !t->on_ground;
            if (!a->slowed) a->slowed = 1; // mB() || yH()
            semi_fire = u;
            found = 1;
        }
    }
    int fire = 0;
    if (found) {
        track(a, f, fx, fy, fy_set);
        fire = f->auto_fire;
    } else {
        release(a);
    }
    if (fire) {
        if (f->semi_auto) {
            if (semi_fire) {
                a->last_auto_fired_ms = f->now_ms;
                a->autofiring = 1;
                a->shoot_key = !a->shoot_key; // keys[shoot] = keys[shoot] ? !keys[shoot] : 1
            }
        } else {
            a->last_auto_fired_ms = f->now_ms;
            a->autofiring = 1;
            a->shoot_key = 1;
        }
    } else if (a->autofiring) {
        const int expired = f->now_ms - a->last_auto_fired_ms > (double) k->autofire_buffer_ms;
        if (f->semi_auto || expired) {
            a->autofiring = 0;
            a->shoot_key = 0;
        }
    }
    return a->autofiring ? a->shoot_key : -1;
}

float strike_assist_look_mlt(const StrikeAimAssist *k, const StrikeAssist *a) {
    return a->slowed ? k->locked_look_mlt : 1.0f;
}

// GE(controls, dt, vertical)
static float pull_factor(const StrikeAimAssist *k, StrikeAssist *a, const float dt, const int vertical) {
    float n, s = 1.0f;
    if (a->locked) {
        n = k->pull_locked;
        if (vertical && !a->target_jumping) s = k->locked_vertical_mlt;
    } else {
        if (fabsf(a->dist_x) < k->center_threshold && !a->has_dist_y) a->locked = !a->locked;
        n = k->pull_initial;
    }
    return n * s * dt;
}

void strike_assist_pull(const StrikeAimAssist *k, StrikeAssist *a, const float dt, const float width, const float height, float *look_x,
                        float *look_y) {
    if (!a->has_dist) return;
    const float fw = strike_assist_look_mlt(k, a);
    *look_x += a->dist_x * width * pull_factor(k, a, dt, 0) / fw;
    if (a->has_dist_y) *look_y -= a->dist_y * height * pull_factor(k, a, dt, 1) / fw;
}
