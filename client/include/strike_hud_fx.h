// Krunker Strike's in-game feedback that FRVR draws on the game canvas, as pure rules (no DOM, no GL), from
// FRVR Krunker Strike 1.37.2's bundle (~/src/KrunkerStrike_v1.37.2/public/assets/index-Cz2uYXJQ.js; the
// same code in 1.27.0's index-2a449dcc.js):
//  - damage numbers: doDamage -> ue.addDamageNumber -> ue.newDmgVal (Stacking / Individual) or ue.dmgVal
//    (Legacy), the step and draw of ue.newDmgNums (eye = .001 / tye = .025 rise, the Kwe.. colours) and of
//    ue.dmgNums inside ue.render. The mode is the Display tab's "Damage Number Display" row
//    (settings damageNumberDisplayMode, val 0, options 0 Stacking / 1 Individual / 2 Legacy:
//    tools/strike/strike_data.json settings.interface.damageNumberDisplayMode.*; the 1.37.2 capture
//    ui/strike/screens/SETTINGS-display.html has the row).
//  - the crosshair: updateCrosshair(max(58, spread * .85 * UH), opacity) in the render loop and ue.render's
//    crosshairType 1 (Dynamic, the default ue.crosshairType = 1 / crosshairSho val 1) lines and dot:
//    Hr(te, Bt + 2, Je + 2, it / 2 - Bt / 2, shadowThickness, angle) with Bt = 4, Je = 21.
// client/src/ui/strike_hud.c lays both out as DOM under #InGameUIMobile; the numbers and the geometry are here.
#ifndef KN_STRIKE_HUD_FX_H
#define KN_STRIKE_HUD_FX_H

#include <stddef.h>

#define STRIKE_DMG_MAX 24            // live numbers (FRVR keeps every one alive; the HUD holds this many)
#define STRIKE_DMG_KEY_AI (-1)       // Stacking's key of a bot's hit: FRVR passes id undefined, so all bots share it

typedef enum {
    STRIKE_DMG_STACKING = 0,         // "Stacking": one number per victim, summed while it lives
    STRIKE_DMG_INDIVIDUAL = 1,       // "Individual": one number per hit; numbers close to each other are lifted apart
    STRIKE_DMG_LEGACY = 2,           // "Legacy" (the default: FRVR's default_damage_numbers_mode): 1 s, drifting, no fade
} StrikeDamageMode;

typedef struct {
    int used;
    int mode;                        // StrikeDamageMode
    int key;                         // Stacking: the victim's uid (STRIKE_DMG_KEY_AI for bots)
    int serial;                      // FRVR's uniqueId: a number never lifts itself
    int crit;
    double value;                    // the summed damage (Stacking) or the hit's; drawn as strike_damage_text
    double life, initial_life;       // ms left / ms of life (Stacking 1000, Individual 750, Legacy 1000)
    float x, y, z;                   // world point: the victim's feet + the mode's height (Legacy: + 7)
    float dir;                       // Legacy: the sideways drift angle (radians, FRVR's randFloat(-PI, PI))
    int height_boost;                // Individual: lifted once on the next step (needsHeightBoost)
    int needs_adjust;                // Stacking / Individual: the screen nudge is recomputed on the next step
    float offset_x, offset_y;        // the screen nudge, fractions of the page (offsetX / offsetY)
} StrikeDamageNum;

typedef struct {
    StrikeDamageNum nums[STRIKE_DMG_MAX];
    int serial;
} StrikeDamageSet;

void strike_damage_reset(StrikeDamageSet *set);

// A hit on a victim whose feet are at (x, feet_y, z): FRVR's addDamageNumber (newDmgVal / dmgVal). The
// caller applies dmgType (Math.ceil for "Rounded"); `value` is the damage as drawn, its sign ignored.
// Stacking adds to the live number with the same key (its life restarts, its point moves to the hit);
// Individual makes a new one; Legacy a new drifting one. `legacy_dir` is the Legacy drift (radians).
// Returns the number written to (never NULL: a full set gives up its number closest to its end).
StrikeDamageNum *strike_damage_add(StrikeDamageSet *set, int mode, int key, float x, float feet_y, float z, double value, int crit,
                                   float player_height, float legacy_dir);

// One frame: dt_ms of FRVR's step (aie, Xte). The camera is the eye (zoom is camera.zoom, 1 unscoped).
// player_scale / player_height are the game constants (playerScale, playerHeight).
void strike_damage_step(StrikeDamageSet *set, double dt_ms, float cam_x, float cam_y, float cam_z, float zoom, float player_scale,
                        float player_height);

// The number's opacity: 1 for its first half of life, then linear to 0 (FRVR's globalAlpha life / (life / 2) clamped
// by the canvas). Legacy does not fade.
double strike_damage_alpha(const StrikeDamageNum *num);

// The fill (stroke = 0) or stroke (stroke = 1) colour of a Stacking / Individual number as "#rrggbb": FRVR's
// interpolateColors from the start colour (t = 1, the hit colour) to the end (t = 0) over the first half of life.
// Crit numbers are gold; the rest white. Legacy numbers are not drawn with this (their colours are the settings').
void strike_damage_color(const StrikeDamageNum *num, int stroke, char *out, size_t n);

// The text FRVR draws: a whole number as "40", a fraction as "33.5" (one decimal, as dmgType "Float" gives).
void strike_damage_text(double value, char *out, size_t n);

// The Display tab's label for a mode (its dropdown options): "Stacking", "Individual", "Legacy"; "" otherwise.
const char *strike_damage_mode_label(int mode);

// Font size in px: FRVR's dmgScale * 30, 1.1 times for a crit on Stacking / Individual (Legacy's draw has no crit
// size); dmg_scale is the "dmgScale" setting, 1 = 30 px.
float strike_damage_font_px(const StrikeDamageNum *num, float dmg_scale);

// ---- the Dynamic crosshair (crosshairSho 1). Sizes are CSS px of the page.
#define STRIKE_CROSS_BAR_THICK 6.0f  // a line's width (FRVR's It + 2)
#define STRIKE_CROSS_BAR_LEN 23.0f   // a line's length (FRVR's ri + 2)
#define STRIKE_CROSS_DOT 6.0f        // the centre dot, a square
#define STRIKE_CROSS_OUTLINE 1.0f    // the shadow outline (crosshairShadowThickness)

// updateCrosshair's scale: the spread radius, max(58, spread * 0.85 * QR) with QR = page height / 693.
float strike_crosshair_scale(float spread, float page_height);

// A line's span along its axis, as distances from the centre: [near, far] = [J - 20, J + 3] with J = scale / 2 - 2.
// The four lines are the top, right, bottom and left ones at the same distances.
void strike_crosshair_span(float scale, float *near, float *far);

// updateCrosshair's opacity outside third person, from KN's aim_val (0 at the hip, 1 fully aimed; FRVR's aimVal
// is 1 - aim_val): 1 - aim_val, and 0 while inspecting, with no_cross (noCross) or while reloading. Clamped to 0..1.
float strike_crosshair_opacity(float aim_val, int inspecting, int no_cross, float reload_timer);

#endif
