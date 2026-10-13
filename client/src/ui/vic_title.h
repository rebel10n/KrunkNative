// overlay.vicTitle (v7.1.4 src/libs/game-overlay.js:318 and its render step at :2087; v10.0.2
// is identical): the round-end "#1 VICTORY" / DEFEAT / DRAW title's fade and zoom. GL- and
// DOM-free so client/tests/vic_title_test.c can check it frame by frame against v7's numbers;
// hud_ui.c draws the values on #knVicTitle.
#pragma once

typedef struct VicTitle {
    double opacity;   // opacity.val
    double increment; // opacity.increment
    double scale;     // scale.val
    double decrement; // scale.decrement
    double acc;       // scale.acc
} VicTitle;

#define VIC_TITLE_DT_MAX_MS 33.0 // config.dltMx = Math.round(1000 / 30): gameTimeDiff's clamp

// The initial object: opacity 0 (+0.05), scale 2.5 (-0.005, +0.01).
void vic_title_init(VicTitle *t);

// One overlay.render frame. `show` = endUI is shown && vicTitle.show; `dt_ms` = that frame's
// gameTimeDiff (already clamped and scaled by the round-end slow motion). Shown: writes the
// globalAlpha / scale this frame draws with (the values before the step), then steps them by
// bp = dt / 16. Hidden: v7's reset branch, kept as it is (it compares scale with 2 and resets
// decrement to 0.01, not the initial 0.005). Returns `show`.
int vic_title_frame(VicTitle *t, int show, double dt_ms, double *alpha, double *scale);

// io_gameEnd: overlay.vicTitle.type = didWin == -1 ? "dra" : didWin ? "vic" : "def" (also the
// img/<type>.png of #victorySub). The title itself runs for all three (vicTitle.show = true
// unless endIndex == 3, the needAll forfeit, which our "end" packet does not carry).
const char *vic_title_type(int did_win);
