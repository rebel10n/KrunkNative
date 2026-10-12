#pragma once
// The prize wheel's 2D canvas (#spinWindow) and the unboxing confetti, drawn with GL.
//
// v7.1.4 draws the wheel every frame into a 2D canvas (index.js updateSpinUi: #2b2b2b disc,
// one pie slice per rarity sized by its odds, a red pointer per item rotated to its roll, the
// #2b2b2b centre disc; pointer and centre with shadowBlur 20) and fires canvas-confetti when the
// wheel stops. The page has no 2D canvas natively or on the web, so store_ui.c (the spin logic,
// client/src/ui/store_ui.c) fills a SpinWheelDraw each frame and client/src/ui/spin_wheel.c
// draws it into the element's rectangle of the framebuffer before the page composites on top
// (like the class preview), and the confetti after the page (canvas-confetti's canvas sits at
// z-index 100 above it).
#include <client.h>

#define SPIN_WHEEL_MAX_SLICES 8
#define SPIN_WHEEL_MAX_POINTERS 16 // one per item of a multi-spin (wheel.multi is 10)

typedef struct SpinWheelDraw {
    int visible;
    float x, y, w, h;          // #spinWindow's getBoundingClientRect, CSS px (after the page's scale)
    float canvas_w, canvas_h;  // its clientWidth / clientHeight: the canvas's own px span the rect
    float radius;              // width / 2 * 0.5 * spinWhlMlt (v7.1.4; canvas px == CSS px)
    float slice_rotation;      // canvas rotate() before the slices (end animation)
    int slices;
    float slice_from[SPIN_WHEEL_MAX_SLICES], slice_to[SPIN_WHEEL_MAX_SLICES]; // radians, canvas arc angles
    float slice_rgb[SPIN_WHEEL_MAX_SLICES][3];
    int pointer_count;         // vA630.length
    float pointer_rotation[SPIN_WHEEL_MAX_POINTERS]; // vA630: each pointer's angle
    float pointer_scale;       // spinButtonOpacity (the pointer shrinks away once the item shows)
} SpinWheelDraw;

// store_ui.c: the wheel as it is this frame (visible == 0 while #spinUI is hidden)
const SpinWheelDraw *store_ui_wheel_draw(void);

// confetti({spread: 360, origin: {x: 0.5, y: 0.5}, particleCount, colors}) / confetti.reset()
void spin_wheel_confetti(int particle_count, const char *const *hex_colors, int color_count);
void spin_wheel_confetti_reset(void);

// client_tick: the wheel before html_ui_render(), the confetti after it
void spin_wheel_render(Client *client);
void spin_wheel_render_confetti(Client *client);
void spin_wheel_fini(void);
