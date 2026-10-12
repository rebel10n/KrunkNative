// Map editor 2D overlay: Krunker v7.1.4 libs/EditorOverlay.js on the page's
// <canvas id="editor-overlay"> (sized like #container, under the canvas buttons), drawn here
// with Canvas2D (canvas2d.h) over the 3D view's viewport. v7 draws the FPS counter
// (settings.showFPS) and the custom GUI preview (EditorOverlay.customGUIs, WP4: draw into
// ed_overlay_canvas() from ed_overlay_render's caller before the composite).
#include "editor_internal.h"
#include <canvas2d.h>
#include <stdio.h>

static Canvas2D *g_canvas;
static double g_frames[512]; // frame times (ms) of the last second (vA539)
static int g_frame_count;

Canvas2D *ed_overlay_canvas(void) { return g_canvas; }

void ed_overlay_render(const double now_s) {
    // EditorOverlay.render: clearRect, then the FPS counter: frames in the last 1000 ms at
    // (18, h - 5), 13px GameFont, #00ff00 over a #2e2e2e stroke (lineWidth 2, round joins)
    if (!editor_setting_bool("showFPS") && !g_canvas) return;
    if (!g_canvas) g_canvas = c2d_create();
    if (!g_canvas) return;
    const float scale = (g_ed.css_to_fb > 0 ? g_ed.css_to_fb : 1) * (g_ed.elem_scale > 0 ? g_ed.elem_scale : 1);
    const int cw = (int) (g_ed.view_w / scale + 0.5f), ch = (int) (g_ed.view_h / scale + 0.5f);
    if (cw <= 0 || ch <= 0) return;
    if (c2d_width(g_canvas) != cw || c2d_height(g_canvas) != ch) c2d_set_size(g_canvas, cw, ch, g_ed.view_w, g_ed.view_h);
    c2d_clear_rect(g_canvas, 0, 0, cw, ch);
    const double ms = now_s * 1000.0;
    int keep = 0;
    for (int i = 0; i < g_frame_count; i++) {
        if (g_frames[i] > ms - 1000.0) g_frames[keep++] = g_frames[i];
    }
    g_frame_count = keep;
    if (g_frame_count < (int) (sizeof(g_frames) / sizeof(g_frames[0]))) g_frames[g_frame_count++] = ms;
    if (editor_setting_bool("showFPS")) {
        char text[16];
        snprintf(text, sizeof(text), "%d", g_frame_count);
        c2d_save(g_canvas);
        c2d_set_font(g_canvas, 13, "GameFont");
        c2d_set_fill_color(g_canvas, 0, 1, 0, 1);
        c2d_set_stroke_color(g_canvas, 0x2e / 255.0f, 0x2e / 255.0f, 0x2e / 255.0f, 1);
        c2d_set_miter_limit(g_canvas, 2);
        c2d_set_line_join(g_canvas, C2D_JOIN_ROUND);
        c2d_set_line_width(g_canvas, 2);
        c2d_stroke_text(g_canvas, text, 19, ch - 5, 0, 0);
        c2d_fill_text(g_canvas, text, 18, ch - 5, 0, 0);
        c2d_restore(g_canvas);
    } else {
        g_frame_count = 0;
    }
    c2d_composite(g_canvas); // over the view viewport (the caller's glViewport)
}

void ed_overlay_fini(void) {
    if (g_canvas) c2d_destroy(g_canvas);
    g_canvas = NULL;
    g_frame_count = 0;
}
