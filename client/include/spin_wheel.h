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
    // Krunker Strike's SPIN (strike_menu.c): the page's own <canvas> (an HtmlElem) to draw into
    // (html_dom_canvas_gl, canvas_w x canvas_h px, centre at the middle), so the wheel keeps its place
    // in the page's paint order (over .frvr-dark-blur, under the cost buttons). 0: under the page.
    uint32_t canvas_elem;
    // The won item (v10 #spinItemCanvas > the spin renderer's <canvas>): drawn this frame into
    // item_canvas (an HtmlElem, 0 = none) at item_size CSS px with the camera at (item_cam_x, 10, 0)
    // and the holder's bob clock item_bob (see spin_item_set)
    uint32_t item_canvas;
    float item_size, item_cam_x, item_bob;
} SpinWheelDraw;

// Krunker Strike's REWARD stars (FRVR oO / gI / aO, strike_reward.c): 4-point white stars in the
// screen's own <canvas>, each bobbing by sin(now * 0.002 + timing) * animation px
#define SPIN_STARS_MAX 64
typedef struct SpinStarsDraw {
    int visible;
    uint32_t canvas_elem;  // the page's <canvas> (html_dom_canvas_gl)
    float canvas_w, canvas_h;
    int count;
    struct { float x, y, outer, inner, opacity, timing, animation; } stars[SPIN_STARS_MAX];
} SpinStarsDraw;

// store_ui.c: the wheel as it is this frame (visible == 0 while #spinUI is hidden)
const SpinWheelDraw *store_ui_wheel_draw(void);

// confetti({spread: 360, origin: {x: 0.5, y: 0.5}, particleCount, colors}) / confetti.reset()
void spin_wheel_confetti(int particle_count, const char *const *hex_colors, int color_count);
void spin_wheel_confetti_reset(void);

// The won item on the wheel (v10 #spinItemCanvas: a three.js scene in its own WebGLRenderer whose
// <canvas> sits in #spinItemCanvas). Here the scene is built by spin_item_set and drawn offscreen by
// spin_wheel_render into the page's <canvas> named by SpinWheelDraw.item_canvas (html_dom_canvas_gl).
// spin_item_set(skin): the store item's model, 1 when it is built (0: this path has no model for it,
// the caller shows its PNG); spin_item_set_coin(): the Free KR wheel's KR coin; spin_item_clear():
// nothing on show.
int  spin_item_set(int skin_index);
int  spin_item_set_coin(void);
void spin_item_clear(void);

// client_tick: the wheel before html_ui_render(), the confetti after it
void spin_wheel_render(Client *client);
void spin_wheel_render_confetti(Client *client);
void spin_wheel_fini(void);
