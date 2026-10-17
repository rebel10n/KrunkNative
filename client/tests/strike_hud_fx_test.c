// Krunker Strike's damage numbers and Dynamic crosshair rules (client/src/ui/strike_hud_fx.c) against
// FRVR 1.37.2's bundle: the three Damage Number Display modes (spawn, merge, life, rise, fade, colours,
// text) and the crosshair's spread radius, line span and opacity. Run: ./<build>/strike_hud_fx_test
#include <strike_hud_fx.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_failures, g_checks;
#define CHECK(cond, ...) do { \
    g_checks++; \
    if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_failures++; } \
} while (0)

static int near(const double a, const double b, const double eps) { return fabs(a - b) <= eps; }

static int live(const StrikeDamageSet *s) {
    int n = 0;
    for (int i = 0; i < STRIKE_DMG_MAX; i++) n += s->nums[i].used;
    return n;
}

static void colour(const StrikeDamageNum *n, int stroke, char *out, size_t len) {
    strike_damage_color(n, stroke, out, len);
}

int main(void) {
    // ---- the mode labels (the Display tab's options, strike_data.json settings.interface.damageNumberDisplayMode.*)
    CHECK(!strcmp(strike_damage_mode_label(STRIKE_DMG_STACKING), "Stacking"), "mode 0 is Stacking");
    CHECK(!strcmp(strike_damage_mode_label(STRIKE_DMG_INDIVIDUAL), "Individual"), "mode 1 is Individual");
    CHECK(!strcmp(strike_damage_mode_label(STRIKE_DMG_LEGACY), "Legacy"), "mode 2 is Legacy");
    CHECK(!strcmp(strike_damage_mode_label(7), ""), "an unknown mode has no label");

    // ---- Stacking: a second hit on the same victim is merged into its number
    StrikeDamageSet s;
    strike_damage_reset(&s);
    const float ph = 11.0f; // player_height
    strike_damage_add(&s, STRIKE_DMG_STACKING, 5, 100.0f, 10.0f, 0.0f, 40.0, 0, ph, 0.0f);
    CHECK(live(&s) == 1, "one Stacking number for one hit");
    strike_damage_step(&s, 200.0, 0, 0, 0, 1.0f, 3.0f, ph);
    strike_damage_add(&s, STRIKE_DMG_STACKING, 5, 101.0f, 10.0f, 0.0f, 15.0, 1, ph, 0.0f);
    CHECK(live(&s) == 1, "a second hit on the same victim merges");
    const StrikeDamageNum *st = NULL;
    for (int i = 0; i < STRIKE_DMG_MAX; i++) if (s.nums[i].used) st = &s.nums[i];
    CHECK(st && near(st->value, 55.0, 1e-9), "the merged value is the sum (55)");
    CHECK(st && near(st->life, 1000.0, 1e-9), "a merge restarts the life (1000 ms)");
    CHECK(st && near(st->y, 10.0 + 1.25 * ph, 1e-4), "Stacking sits 1.25 * playerHeight above the feet");
    CHECK(st && st->crit == 1, "the merged number takes the latest hit's crit");
    strike_damage_add(&s, STRIKE_DMG_STACKING, 9, 200.0f, 0.0f, 0.0f, 10.0, 0, ph, 0.0f);
    CHECK(live(&s) == 2, "another victim gets its own Stacking number");
    // the bots share one key (FRVR passes id undefined): their hits stack into one number
    strike_damage_reset(&s);
    strike_damage_add(&s, STRIKE_DMG_STACKING, STRIKE_DMG_KEY_AI, 0.0f, 0.0f, 0.0f, 10.0, 0, ph, 0.0f);
    strike_damage_add(&s, STRIKE_DMG_STACKING, STRIKE_DMG_KEY_AI, 90.0f, 0.0f, 0.0f, 20.0, 0, ph, 0.0f);
    CHECK(live(&s) == 1, "bot hits share one Stacking number (FRVR's undefined id)");

    // ---- Stacking life and fade: 1000 ms; alpha 1 for the first half, then linear to 0
    strike_damage_reset(&s);
    strike_damage_add(&s, STRIKE_DMG_STACKING, 1, 0.0f, 0.0f, 0.0f, 30.0, 0, ph, 0.0f);
    strike_damage_step(&s, 999.0, 0, 0, 0, 1.0f, 3.0f, ph);
    CHECK(live(&s) == 1, "a Stacking number is alive at 999 ms");
    CHECK(near(strike_damage_alpha(&s.nums[0]), 1.0 / 500.0, 1e-9), "alpha 1 / 500 at 1 ms left (the second half)");
    strike_damage_step(&s, 1.0, 0, 0, 0, 1.0f, 3.0f, ph);
    CHECK(live(&s) == 0, "a Stacking number is gone at 1000 ms");

    strike_damage_reset(&s);
    strike_damage_add(&s, STRIKE_DMG_STACKING, 1, 0.0f, 0.0f, 0.0f, 30.0, 0, ph, 0.0f);
    s.nums[0].life = 250.0; // a quarter of the life: half of the half
    CHECK(near(strike_damage_alpha(&s.nums[0]), 0.5, 1e-9), "alpha 0.5 at 250 ms left (linear over the second half)");
    s.nums[0].life = 0.0;
    CHECK(near(strike_damage_alpha(&s.nums[0]), 0.0, 1e-9), "alpha 0 at the end");

    // ---- the rise: slow (0.001 / ms) for the first half of life, fast (0.025 / ms) after
    strike_damage_reset(&s);
    strike_damage_add(&s, STRIKE_DMG_STACKING, 1, 0.0f, 0.0f, 0.0f, 30.0, 0, ph, 0.0f);
    const float y0 = s.nums[0].y;
    strike_damage_step(&s, 100.0, 1000.0f, 0, 1000.0f, 1.0f, 3.0f, ph); // far away: no nudge applies beyond the cap
    CHECK(near(s.nums[0].y - y0, 0.1, 1e-4), "slow rise of 0.1 in 100 ms while life > half");
    s.nums[0].life = 450.0;
    const float y1 = s.nums[0].y;
    strike_damage_step(&s, 100.0, 1000.0f, 0, 1000.0f, 1.0f, 3.0f, ph); // life 350 < 500: fast
    CHECK(near(s.nums[0].y - y1, 2.5, 1e-4), "fast rise of 2.5 in 100 ms in the second half");

    // ---- the screen nudge: offset_x = min(0.03, playerScale * 0.5 / (75 / w * 100) * 2 * zoom), offset_y above 30 units
    // the eye at feet height + 13.75 (the number sits 1.25 * 11 above the feet): w is the 3D distance
    strike_damage_reset(&s);
    strike_damage_add(&s, STRIKE_DMG_STACKING, 1, 0.0f, 0.0f, 50.0f, 30.0, 0, ph, 0.0f);
    strike_damage_step(&s, 0.0, 0.0f, 13.75f, 0.0f, 1.0f, 3.0f, ph); // w = 50
    CHECK(near(s.nums[0].offset_x, 0.02, 1e-4), "offset_x 0.02 at 50 units (playerScale 3)");
    CHECK(near(s.nums[0].offset_y, 0.03, 1e-9), "offset_y is capped at 0.03 beyond 30 units");
    strike_damage_reset(&s);
    strike_damage_add(&s, STRIKE_DMG_STACKING, 1, 0.0f, 0.0f, 20.0f, 30.0, 0, ph, 0.0f);
    strike_damage_step(&s, 0.0, 0.0f, 13.75f, 0.0f, 1.0f, 3.0f, ph); // w = 20: no offset_y (FRVR's w > 30)
    CHECK(near(s.nums[0].offset_x, 0.008, 1e-4), "offset_x 0.008 at 20 units");
    CHECK(near(s.nums[0].offset_y, 0.0, 1e-9), "no offset_y within 30 units (FRVR skips it)");
    strike_damage_reset(&s);
    strike_damage_add(&s, STRIKE_DMG_STACKING, 1, 0.0f, 0.0f, 300.0f, 30.0, 0, ph, 0.0f);
    strike_damage_step(&s, 0.0, 0.0f, 13.75f, 0.0f, 1.0f, 3.0f, ph);
    CHECK(near(s.nums[0].offset_x, 0.03, 1e-9), "offset_x is capped at 0.03");
    CHECK(near(s.nums[0].offset_y, 0.03, 1e-9), "offset_y is capped at 0.03 beyond 30 units");

    // ---- Individual: one number per hit; a number near a new one is lifted once
    strike_damage_reset(&s);
    strike_damage_add(&s, STRIKE_DMG_INDIVIDUAL, 1, 100.0f, 0.0f, 0.0f, 25.0, 0, ph, 0.0f);
    strike_damage_add(&s, STRIKE_DMG_INDIVIDUAL, 1, 105.0f, 0.0f, 0.0f, 25.0, 0, ph, 0.0f);
    CHECK(live(&s) == 2, "Individual makes a number per hit, same victim or not");
    CHECK(near(s.nums[0].life, 750.0, 1e-9) && near(s.nums[0].initial_life, 750.0, 1e-9), "Individual lives 750 ms");
    CHECK(near(s.nums[0].y, 11.0, 1e-4), "Individual sits at playerHeight above the feet");
    CHECK(s.nums[0].height_boost == 1 && s.nums[1].height_boost == 0, "the number within 15 units is flagged, the new one is not");
    strike_damage_reset(&s);
    strike_damage_add(&s, STRIKE_DMG_INDIVIDUAL, 1, 100.0f, 0.0f, 0.0f, 25.0, 0, ph, 0.0f);
    strike_damage_add(&s, STRIKE_DMG_INDIVIDUAL, 2, 200.0f, 0.0f, 0.0f, 25.0, 0, ph, 0.0f);
    CHECK(s.nums[0].height_boost == 0, "a number 15 units or more away is not lifted");
    strike_damage_reset(&s);
    strike_damage_add(&s, STRIKE_DMG_INDIVIDUAL, 1, 100.0f, 0.0f, 0.0f, 25.0, 0, ph, 0.0f);
    strike_damage_add(&s, STRIKE_DMG_INDIVIDUAL, 1, 105.0f, 0.0f, 0.0f, 25.0, 0, ph, 0.0f);
    strike_damage_step(&s, 100.0, 0.0f, 0.0f, 0.0f, 1.0f, 3.0f, ph);
    // the lift: 2 + 13 * (w / 300), w = 100 from the eye (the lifted number's distance) + the 0.001 / ms rise
    const double lift = 2.0 + 13.0 * (100.0 / 300.0);
    CHECK(near(s.nums[0].y - (11.0 + 0.1), lift, 1e-3), "the lift is V.lerp(2, 15, w / 300) once");
    CHECK(s.nums[0].height_boost == 0, "the lift applies once");

    // ---- Legacy: its own drifting number, 1 s, no fade, no merge
    strike_damage_reset(&s);
    strike_damage_add(&s, STRIKE_DMG_LEGACY, 1, 0.0f, 0.0f, 0.0f, 12.0, 0, ph, 0.0f);
    strike_damage_add(&s, STRIKE_DMG_LEGACY, 1, 0.0f, 0.0f, 0.0f, 12.0, 0, ph, 0.0f);
    CHECK(live(&s) == 2, "Legacy never merges");
    CHECK(near(s.nums[0].y, 7.0, 1e-4), "Legacy sits 7 units above the feet");
    CHECK(near(strike_damage_alpha(&s.nums[0]), 1.0, 1e-9), "Legacy does not fade");
    strike_damage_step(&s, 100.0, 0.0f, 0.0f, 0.0f, 1.0f, 3.0f, ph);
    CHECK(near(s.nums[0].y - 7.0, 2.2, 1e-4), "Legacy rises 0.022 / ms");
    strike_damage_step(&s, 950.0, 0.0f, 0.0f, 0.0f, 1.0f, 3.0f, ph);
    CHECK(live(&s) == 0, "Legacy ends after 1000 ms");

    // ---- colours: the start (t = 1: the hit colour) and the end (t = 0: white / black)
    char buf[16];
    strike_damage_reset(&s);
    strike_damage_add(&s, STRIKE_DMG_STACKING, 1, 0.0f, 0.0f, 0.0f, 30.0, 0, ph, 0.0f);
    colour(&s.nums[0], 0, buf, sizeof(buf));
    CHECK(!strcmp(buf, "#f87c7c"), "a fresh non-crit fill is #f87c7c (got %s)", buf);
    colour(&s.nums[0], 1, buf, sizeof(buf));
    CHECK(!strcmp(buf, "#8b0000"), "a fresh non-crit stroke is #8b0000 (got %s)", buf);
    s.nums[0].life = 500.0;
    colour(&s.nums[0], 0, buf, sizeof(buf));
    CHECK(!strcmp(buf, "#ffffff"), "the fill is white at half life (got %s)", buf);
    colour(&s.nums[0], 1, buf, sizeof(buf));
    CHECK(!strcmp(buf, "#000000"), "the stroke is black at half life (got %s)", buf);
    strike_damage_add(&s, STRIKE_DMG_STACKING, 2, 0.0f, 0.0f, 0.0f, 30.0, 1, ph, 0.0f);
    colour(&s.nums[1], 0, buf, sizeof(buf));
    CHECK(!strcmp(buf, "#ffd542"), "a fresh crit fill is gold (got %s)", buf);
    colour(&s.nums[1], 1, buf, sizeof(buf));
    CHECK(!strcmp(buf, "#d40b0b"), "a fresh crit stroke is #d40b0b (got %s)", buf);

    // ---- text and font
    char text[32];
    strike_damage_text(40.0, text, sizeof(text));
    CHECK(!strcmp(text, "40"), "a whole number has no decimals (got %s)", text);
    strike_damage_text(33.5, text, sizeof(text));
    CHECK(!strcmp(text, "33.5"), "a fraction keeps its decimal (got %s)", text);
    strike_damage_text(0.0, text, sizeof(text));
    CHECK(!strcmp(text, "0"), "zero reads 0 (got %s)", text);
    CHECK(near(strike_damage_font_px(&s.nums[0], 1.0f), 30.0, 1e-4), "dmgScale 1 is 30 px");
    CHECK(near(strike_damage_font_px(&s.nums[1], 1.0f), 33.0, 1e-3), "a crit is 1.1 times the size");
    CHECK(near(strike_damage_font_px(&s.nums[0], 2.0f), 60.0, 1e-4), "dmgScale 2 is 60 px");
    strike_damage_reset(&s);
    strike_damage_add(&s, STRIKE_DMG_LEGACY, 1, 0.0f, 0.0f, 0.0f, 30.0, 1, ph, 0.0f);
    CHECK(near(strike_damage_font_px(&s.nums[0], 1.0f), 30.0, 1e-4), "a Legacy crit is the plain dmgScale (its draw has no crit size)");

    // ---- the Dynamic crosshair: radius from the spread, the line span, the opacity
    CHECK(near(strike_crosshair_scale(0.0f, 693.0f), 58.0, 1e-4), "no spread: the 58 px floor");
    CHECK(near(strike_crosshair_scale(100.0f, 693.0f), 85.0, 1e-3), "spread 100 at QR 1: 85 px (0.85 * spread)");
    CHECK(near(strike_crosshair_scale(100.0f, 1386.0f), 170.0, 1e-3), "spread 100 at QR 2: 170 px");
    float near_d = 0.0f, far_d = 0.0f;
    strike_crosshair_span(58.0f, &near_d, &far_d);
    CHECK(near(near_d, 7.0, 1e-4) && near(far_d, 30.0, 1e-4), "a 58 px scale spans 7 to 30 px from the centre");
    strike_crosshair_span(170.0f, &near_d, &far_d);
    CHECK(near(near_d, 63.0, 1e-4) && near(far_d, 86.0, 1e-4), "a 170 px scale spans 63 to 86 px (the lines move out)");
    CHECK(near(STRIKE_CROSS_BAR_THICK, 6.0, 1e-9) && near(STRIKE_CROSS_BAR_LEN, 23.0, 1e-9), "the bar is 6 x 23 px");

    CHECK(near(strike_crosshair_opacity(0.0f, 0, 0, 0.0f), 1.0, 1e-6), "at the hip (aim 0) the crosshair is fully shown");
    CHECK(near(strike_crosshair_opacity(1.0f, 0, 0, 0.0f), 0.0, 1e-6), "fully aimed (aim 1) it is hidden (Dynamic)");
    CHECK(near(strike_crosshair_opacity(0.25f, 0, 0, 0.0f), 0.75, 1e-6), "a quarter aimed: 0.75");
    CHECK(near(strike_crosshair_opacity(0.0f, 0, 1, 0.0f), 0.0, 1e-6), "a noCross weapon hides it");
    CHECK(near(strike_crosshair_opacity(0.0f, 0, 0, 0.5f), 0.0, 1e-6), "a reload hides it");
    CHECK(near(strike_crosshair_opacity(0.0f, 1, 0, 0.0f), 0.0, 1e-6), "inspecting hides it");

    // ---- the aim reticles (FRVR's toggleAim): aimDot for an attachment sight, aimRecticle for a scope weapon,
    // both once fully aimed (KN aim_val 1) and never in third person
    CHECK(!strike_aim_dot_shown(0.0f, 1, 0) && !strike_aim_dot_shown(0.99f, 1, 0), "the sight's dot is not shown before fully aimed");
    CHECK(strike_aim_dot_shown(1.0f, 1, 0), "fully aimed with a sight: the dot is shown (FRVR's reticle_0)");
    CHECK(!strike_aim_dot_shown(1.0f, 0, 0), "fully aimed without a sight (no attach): no dot");
    CHECK(!strike_aim_dot_shown(1.0f, 1, 1), "third person: no dot");
    CHECK(!strike_scope_shown(1.0f, 0, 0), "a weapon without scope: no scope when aimed");
    CHECK(!strike_scope_shown(0.5f, 1, 0), "a scope weapon half aimed: no scope yet");
    CHECK(strike_scope_shown(1.0f, 1, 0), "fully aimed with a scope weapon: the scope shows");
    CHECK(!strike_scope_shown(1.0f, 1, 1), "third person: no scope");

    if (g_failures) {
        printf("strike_hud_fx_test: %d of %d checks failed\n", g_failures, g_checks);
        return 1;
    }
    printf("strike_hud_fx_test: all %d checks passed\n", g_checks);
    return 0;
}
