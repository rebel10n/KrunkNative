// The GL-free half of the v10.0.3 map prize wheel drawing (client/include/prize_wheel_geom.h). Line numbers:
// deob-10.0.3.js.
#include <prize_wheel_geom.h>
#include <math.h>
#include <string.h>

int prize_wheel_canvas_px(const float size) {
    // iïÏîïíì = Math.max(iíÏîiíi / 7.5 * 265, 768); Math.min(iïÏîïíì, 4096); canvas.width = iïÏîïíì * 1 (:232700-232703)
    double px = (double) size / 7.5 * 265.0;
    if (!(px > 768.0)) px = 768.0; // Math.max(NaN, 768) is NaN in JS; a map's sy is a number (parseFloat || 0)
    if (px > 4096.0) px = 4096.0;
    return (int) px;
}

void prize_wheel_layout(const PrizeWheelState *s, PrizeWheelLayout *o) {
    memset(o, 0, sizeof(*o));
    const double width = s->canvas;
    o->centre = width / 2.0;
    const double r = width / 2.0 * 0.95 * s->spin_whl_mlt; // iïIïiíï (:219578)
    o->radius = r;
    o->slice_radius = r * 0.95;
    const double target = s->end_anim_target, val = s->end_anim_val;
    const double done = target != 0.0 ? val / target : 0.0;
    const int count = s->count < 0 ? 0 : s->count > PRIZE_WHEEL_GEOM_SLICES ? PRIZE_WHEEL_GEOM_SLICES : s->count;
#define WEIGHT(i) ((i) < s->weight_count ? s->weights[i] : NAN) // wheel.rarities[i] past its end: undefined
    if (target != 0.0) { // the turn that brings the won slice's middle under the pointer (:219589-219601)
        double turn = 0.0;
        for (int i = 0; i < count; i++) {
            if (s->won_rarity == i) {
                turn += s->pool ? WEIGHT(i) : WEIGHT(i) / 2.0; // poolWeights[i] whole (v10's precedence), rarities[i] / 2
                break;
            }
            turn += WEIGHT(i);
        }
        turn /= 100.0;
        turn = fmod(M_PI * 2.0 * turn * done, M_PI * 2.0);
        o->turn = turn - M_PI * done;
    }
    double start = 0.0; // iìÍiíïi
    o->count = count;
    for (int i = 0; i < count; i++) {
        double w = WEIGHT(i); // iiÍìîîì (:219605-219617)
        if (target != 0.0) {
            if (s->won_rarity == i) w += val;
            else w -= val * (s->pool ? WEIGHT(i) : WEIGHT(i) / target);
        }
        o->slices[i].start = start;
        o->slices[i].sweep = w > 0.0 && isfinite(start) ? M_PI * 2.0 * (w / 100.0) : 0.0; // `if (w > 0)`
        start += M_PI * 2.0 * (w / 100.0); // also for a slice it did not draw (a negative one shifts the rest back)
    }
#undef WEIGHT
    const double hub = r * PRIZE_WHEEL_POINTER_MLT, k = 1.0 - done; // iíÌiîíï (:219577)
    o->pointer_rotation = s->spin_rotation;
    o->pointer[0][0] = 0.0;
    o->pointer[0][1] = (hub * -1.2 - hub * 0.0 * 0.3) * k; // iìIíîïí = iïIïiíï * iîÏiíîì * 0 * 0.3 (:219630-219632)
    o->pointer[1][0] = hub * k;
    o->pointer[1][1] = 0.0;
    o->pointer[2][0] = -hub * k;
    o->pointer[2][1] = 0.0;
    o->hub_radius = hub;
    o->label = !s->item_shown; // `if (!iiIîîíï.meshPrev)` (:219642)
    o->label_radius = hub * 0.85;
    o->spin_font = hub * 0.3;
    o->spin_y = hub * 0.1;
    o->price_font = hub * 0.15;
    o->price_y = hub * 0.25;
}

int prize_wheel_shadow_levels(const double blur, const int max, double *grow, double *alpha) {
    if (!(blur > 0.0) || max <= 0) return 0;
    const double sigma = blur / 2.0; // the canvas spec: a Gaussian of standard deviation shadowBlur / 2
    double prev = 0.0;
    for (int i = 0; i < max; i++) {
        const double cover = 0.5 * (double) (i + 1) / (double) max; // coverage at the band's outer edge, up to 0.5 at the shape
        // t with 0.5 erfc(t / (sigma sqrt 2)) = cover: erfc falls monotonically, so bisect
        double lo = 0.0, hi = sigma * 8.0;
        for (int k = 0; k < 60; k++) {
            const double mid = (lo + hi) / 2.0;
            if (0.5 * erfc(mid / (sigma * M_SQRT2)) > cover) lo = mid;
            else hi = mid;
        }
        grow[i] = (lo + hi) / 2.0;
        alpha[i] = 1.0 - (1.0 - cover) / (1.0 - prev); // stacked over the bands outside it, the coverage is `cover`
        prev = cover;
    }
    return max;
}

void prize_wheel_plane(const float position[3], const float size, const float dir, float centre[3], float *yaw) {
    // manager.addSpinner: y += sy / 2; render.addSpinner: moveMesh(x, y, z), rotateY(PI / 2 - d * PI / 4) (:186562, :232687)
    centre[0] = position[0];
    centre[1] = position[1] + size / 2.0f;
    centre[2] = position[2];
    *yaw = (float) (M_PI / 2.0 - (double) dir * M_PI / 4.0);
}

void prize_wheel_item_pose(const float size, const float dir, const int type, const float y_off, const float x_off, const float scl,
                           const float spn_scl, PrizeWheelItemPose *o) {
    const float width = size / 4.0f; // the spinner GameObject's w: (sy / 2) / 2 (:186547)
    o->frame_yaw = (float) (M_PI - (double) dir * M_PI / 4.0); // iíÍíìiï.rotateY(PI - dir * PI / 4) (:219493)
    o->position[0] = width / 8.0f * -5.0f;                      // meshPrev.position.x = width / 8 * -5 (:219497)
    o->position[1] = 1.0f + y_off;                              // 1 + (yOff || 0) (:219498)
    o->position[2] = x_off;                                     // xOff || 0 (:219499)
    o->rotation[0] = type ? 0.0f : (float) (M_PI / 11.0);       // rotateX(type ? 0 : PI / 11) (:219500)
    o->rotation[1] = type == PRIZE_WHEEL_ITEM_SPRAY || type == PRIZE_WHEEL_ITEM_DYE ? (float) (M_PI / 2.0) : 0.0f; // (:219501)
    o->scale = width / 4.0f;                                    // scale.setScalar(width / 4) (:219496)
    if (type == PRIZE_WHEEL_ITEM_SPRAY) o->item_rot_y = (float) M_PI; // createSpray, rotation.y = PI (:219519)
    else o->item_rot_y = type && type != PRIZE_WHEEL_ITEM_MELEE ? (float) M_PI : (float) (M_PI / 2.0); // (:219565)
    o->item_scale = (spn_scl != 0.0f ? spn_scl : scl != 0.0f ? scl : 1.0f) * 2.0f; // (spnScl || scl || 1) * 2
}

void prize_wheel_item_world(const float centre[3], const PrizeWheelItemPose *p, float out[3]) {
    const float c = cosf(p->frame_yaw), s = sinf(p->frame_yaw); // three.js rotateY: (x, z) -> (x c + z s, -x s + z c)
    out[0] = centre[0] + p->position[0] * c + p->position[2] * s;
    out[1] = centre[1] + p->position[1];
    out[2] = centre[2] - p->position[0] * s + p->position[2] * c;
}

float prize_wheel_item_bob(const float org_y, const float bob_anim) {
    return org_y + sinf(bob_anim) * 0.55f;
}
