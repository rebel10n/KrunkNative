// overlay.vicTitle: see vic_title.h.
#include "vic_title.h"

void vic_title_init(VicTitle *t) {
    t->opacity = 0.0;
    t->increment = 0.05;
    t->scale = 2.5;
    t->decrement = 0.005;
    t->acc = 0.01;
}

int vic_title_frame(VicTitle *t, const int show, const double dt_ms, double *alpha, double *scale) {
    if (!show) {
        if (t->opacity != 0.0) t->opacity = 0.0;
        if (t->scale != 2.0) t->scale = 2.5;           // sic: v7 compares with 2
        if (t->decrement != 0.01) t->decrement = 0.01; // sic: not the initial 0.005
        return 0;
    }
    const double bp = dt_ms / 16.0;
    // globalAlpha = opacity.val; a canvas ignores values above 1 (the alpha stays 1)
    if (alpha) *alpha = t->opacity < 1.0 ? t->opacity : 1.0;
    t->opacity += t->increment * bp;
    if (scale) *scale = t->scale; // drawImage at width / height * scale.val
    t->scale -= t->decrement * bp;
    if (t->scale < 1.0) t->scale = 1.0;
    t->decrement += t->acc * bp;
    return 1;
}

// v10.0.4 (deob-10.0.4.js:223534): `var type = -1 == didWin ? "dra" : didWin ? "vic" : noWinnerEnd ? "end" : "def"`,
// noWinnerEnd = !didWin && game.mode.noWinner (:223533; Candy Heist, deob-10.0.4.js:105921): ENDED, not DEFEAT.
const char *vic_title_type(const int did_win, const int no_winner) {
    return did_win == -1 ? "dra" : did_win ? "vic" : no_winner ? "end" : "def";
}
