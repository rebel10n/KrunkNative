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
