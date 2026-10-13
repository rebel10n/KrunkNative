// Two v7.1.4 game-overlay canvas items of the held weapon (src/libs/game-overlay.js render), drawn
// on a Canvas2D layer over the HUD like the spray wheel (sprays.c):
//   - interactMProg (game-overlay.js:1781-1830, index.js io_interMProg): the charge bar of the
//     Charge Rifle, Crossbow, Zapper and the knife throw. player.js procInputs sets
//     interMProg = chargeTime / (weapon.chrgTime || throwCharge) while charging or decaying and 0
//     once the charge is gone; it shows while > 0 or overCharged. Past 1 (an ovrChrg weapon
//     creeping towards chrgTime * config.overCharge) the fill turns red, grows and shakes; an
//     overCharged weapon shows a red cross instead of the fill.
//   - the Build Tool's pick wheel (game-overlay.js:1655-1700): while selectBuild (aim held with a
//     build weapon) the four buildObjs.js entries round a ring, the picked one's segment blue,
//     its name and cost ("N Score", red when the score is short) in the middle. KrunkNative
//     shows "Free" where the cost is waived (Player.build_free, doc/v7/decisions.md "Build Tool
//     cost").
//   - the Build Tool's crosshair (game-overlay.js:1641-1650): the branch after `crosshairType > 0`,
//     so only with the crosshair setting off (crosshairSho 0): a dot and four bars, gap 12.
// The Build Tool's placement helper (blue / red cube) is a world item: client/src/vfx.c.
#include <gl.h>
#include <client.h>
#include <canvas2d.h>
#include <headless.h>
#include <html_dom.h>
#include <html_ui.h>
#include <ui_logic.h>
#include <world.h>
#include <render_settings.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define BUILD_UI_OVER_CHARGE 1.2 // config.overCharge (shared/src/player.c COMBAT_OVER_CHARGE)
#define BUILD_UI_THROW_CHARGE 0.9f // config.throwCharge (s)

// buildObjs.js icon per WORLD_BUILD index: img/build/build_<icon>.png
static const int k_build_icons[WORLD_BUILD_COUNT] = {0, 3, 1, 4};

static struct {
    Canvas2D *canvas;
    int drawn;
} g_bui;

// scaleBase's scale (1700 x 900 * uiCustomScale fit), as sprays.c wheel_ui_scale
static double build_ui_scale(const float vw, const float vh) {
    if (ui_setting_is_false("uiScaling")) return 1.0;
    double scale_ui;
    if (!ui_setting_number("scaleUI", &scale_ui) || scale_ui != scale_ui) scale_ui = 0.8;
    const double custom = 2.0 - scale_ui;
    const double sx = vw / (custom * 1700.0), sy = vh / (custom * 900.0);
    return sy < sx ? sy : sx;
}

// CanvasRenderingContext2D.prototype.roundRect(x, y, w, h, r, fill) (game-overlay.js:2289)
static void round_rect_fill(Canvas2D *c, const double x, const double y, const double w, const double h, const double r) {
    c2d_begin_path(c);
    c2d_move_to(c, x + r, y);
    c2d_line_to(c, x + w - r, y);
    c2d_quadratic_curve_to(c, x + w, y, x + w, y + r);
    c2d_line_to(c, x + w, y + h - r);
    c2d_quadratic_curve_to(c, x + w, y + h, x + w - r, y + h);
    c2d_line_to(c, x + r, y + h);
    c2d_quadratic_curve_to(c, x, y + h, x, y + h - r);
    c2d_line_to(c, x, y + r);
    c2d_quadratic_curve_to(c, x, y, x + r, y);
    c2d_close_path(c);
    c2d_fill(c);
}

// interMProg of the local player (player.js:1068-1107, 1776): the charge over the full charge
float build_ui_charge_progress(const Player *me) {
    if (!me || !me->active || !me->weapon || !me->game || me->game->config.no_charge_weapons) return 0.0f;
    const float full = me->weapon->charge_time > 0.0f ? me->weapon->charge_time
                       : me->throw_charge > 0.0f ? me->throw_charge : BUILD_UI_THROW_CHARGE;
    return me->charge_time > 0.0f && full > 0.0f ? me->charge_time / full : 0.0f;
}

static void draw_charge(Canvas2D *c, const Player *me, const double w, const double h) {
    const double prog = build_ui_charge_progress(me);
    const int over = me->over_charged;
    if (!(prog > 0.0) && !over) return; // interactMProg.display = prog > 0 || overCharged
    // interactMProg: left 56 %, top 50 %, 400 x 30 at scale 0.5, colour #fff, oColor #eb2f2f
    double bw = 400.0 * 0.5, bh = 30.0 * 0.5, shake = 0.0;
    if (prog > 1.0) {
        const double k = (prog - 1.0) / (1.0 - BUILD_UI_OVER_CHARGE) * 300.0;
        shake = sin((prog - 1.0) * k) * 8.0;
        bh *= 1.0 + (prog - 1.0) * 2.0;
    }
    c2d_save(c);
    c2d_translate(c, w * 0.56 + shake, h * 0.5);
    c2d_set_fill_color(c, 0.0f, 0.0f, 0.0f, 0.4f);
    round_rect_fill(c, -bw / 2, 0, bw, bh, 4);
    if (!over) {
        if (prog > 1.0) c2d_set_fill_color(c, 0xeb / 255.0f, 0x2f / 255.0f, 0x2f / 255.0f, 1.0f);
        else c2d_set_fill_color(c, 1.0f, 1.0f, 1.0f, 1.0f);
        round_rect_fill(c, -bw / 2, 0, bw * fmin(1.0, prog), bh, 2);
    } else { // the cross: a black 14 px stroke under a red 10 px one, arms 30 / 28
        c2d_translate(c, 0, bh / 2);
        for (int i = 0; i < 2; i++) {
            if (i) c2d_set_stroke_color(c, 0xeb / 255.0f, 0x2f / 255.0f, 0x2f / 255.0f, 1.0f);
            else c2d_set_stroke_color(c, 0.0f, 0.0f, 0.0f, 1.0f);
            c2d_set_line_width(c, i ? 10 : 14);
            const double a = i ? 28 : 30;
            c2d_begin_path(c);
            c2d_move_to(c, -a, -a);
            c2d_line_to(c, a, a);
            c2d_move_to(c, a, -a);
            c2d_line_to(c, -a, a);
            c2d_stroke(c);
        }
    }
    c2d_restore(c);
}

static void draw_build_wheel(Canvas2D *c, const Player *me, const double h) {
    const int index = me->build_index >= 0 && me->build_index < WORLD_BUILD_COUNT ? me->build_index : 0;
    const WorldBuildObject *pick = &g_build_objects[index];
    const double radius = h / 2 * 0.4;
    c2d_set_global_alpha(c, 0.7);
    c2d_begin_path(c);
    c2d_arc(c, 0, 0, radius, 0, M_PI * 2, 0);
    c2d_set_fill_color(c, 0x17 / 255.0f, 0x17 / 255.0f, 0x17 / 255.0f, 1); // #171717
    c2d_fill(c);
    const double ring = radius * 1.33;
    c2d_set_global_alpha(c, 1);
    c2d_set_line_width(c, radius / 1.9);
    c2d_set_stroke_color(c, 0xcf / 255.0f, 0xcf / 255.0f, 0xcf / 255.0f, 1); // #cfcfcf
    c2d_begin_path(c);
    c2d_arc(c, 0, 0, ring, 0, M_PI * 2, 0);
    c2d_stroke(c);
    const double step = M_PI * 2 / WORLD_BUILD_COUNT;
    c2d_set_stroke_color(c, 0x55 / 255.0f, 0xac / 255.0f, 0xee / 255.0f, 1); // #55acee: the pick
    c2d_begin_path(c);
    c2d_arc(c, 0, 0, ring, step * index, step * index + step, 0);
    c2d_stroke(c);

    const double offset = 50.0;
    c2d_set_fill_color(c, 0xdb / 255.0f, 0xdb / 255.0f, 0xdb / 255.0f, 1);
    c2d_set_font(c, 40, "GameFont");
    c2d_fill_text(c, pick->name, -c2d_measure_text(c, pick->name) / 2, 40.0 / 2 + offset, 0, 0);
    char cost[32];
    if (me->build_free) snprintf(cost, sizeof(cost), "Free"); // KrunkNative: OM_CAP_SPAWN waives it
    else snprintf(cost, sizeof(cost), "%d Score", pick->cost);
    if (me->build_free || me->score >= pick->cost) c2d_set_fill_color(c, 0xa3 / 255.0f, 0xa3 / 255.0f, 0xa3 / 255.0f, 1);
    else c2d_set_fill_color(c, 0xd6 / 255.0f, 0x3e / 255.0f, 0x3e / 255.0f, 1);
    c2d_set_font(c, 25, "GameFont");
    c2d_fill_text(c, cost, -c2d_measure_text(c, cost) / 2, 25.0 / 2 + offset + 50, 0, 0);

    double angle = -(step / 2);
    for (int i = -1; i < WORLD_BUILD_COUNT; i++) {
        char rel[48];
        snprintf(rel, sizeof(rel), "img/build/build_%d.png", k_build_icons[i < 0 ? index : i]);
        char *full = concat(ui_default_path(), rel); // the page's img/build (UTILS.assetsUrl("/img/build/..."))
        C2DImage *img = full ? c2d_image(c, full) : NULL;
        free(full);
        if (i < 0) { // the pick, 100 px above the name
            if (img && c2d_image_state(img) == 1) c2d_draw_image(c, img, -50, -50 - offset * 1.5, 100, 100);
            continue;
        }
        angle += step;
        if (img && c2d_image_state(img) == 1) c2d_draw_image(c, img, ring * cos(angle) - 40, ring * sin(angle) - 40, 80, 80);
    }
}


// f1571(ctx, width, length, gap, shadowThickness, rotation, dot): f1568 gap * 1 + 3, f1569 /
// f1570 width and length / 10 * 19; the bar runs outwards from the gap.
static void cross_bar(Canvas2D *c, const ClientRenderSettings *rs, const double width, const double length, const double gap,
                      const double rotation, const int dot) {
    const double w = width / 10.0 * 19.0;
    const double g = dot ? 0.0 : gap * 1.0 + 3.0;
    const double l = dot ? w : -(length / 10.0 * 19.0);
    float rgba[4] = {1, 1, 1, 1};
    c2d_parse_color(rs->crosshair_color, rgba);
    c2d_save(c);
    c2d_set_fill_color(c, rgba[0], rgba[1], rgba[2], rgba[3]);
    if (!dot) c2d_rotate(c, rotation * M_PI / 180.0);
    c2d_begin_path(c);
    c2d_rect(c, -(w / 2), -(w / 2 + g), w, l);
    c2d_fill(c);
    if (rs->crosshair_shadow_thickness && !dot) {
        float shadow[4] = {0, 0, 0, 1};
        c2d_parse_color(rs->crosshair_shadow, shadow);
        c2d_set_stroke_color(c, shadow[0], shadow[1], shadow[2], shadow[3]);
        c2d_set_line_width(c, rs->crosshair_shadow_thickness);
        c2d_stroke(c);
    }
    c2d_restore(c);
}

static void draw_build_cross(Canvas2D *c, const ClientRenderSettings *rs, const double w, const double h) {
    c2d_save(c);
    c2d_translate(c, w / 2, h / 2);
    cross_bar(c, rs, 3, 0, 0, 0, 1);
    const double length = rs->crosshair_len * 0.5;
    for (int i = 0; i < 4; i++) cross_bar(c, rs, 3, length, 12, i == 0 ? 90 : i == 1 ? -90 : i == 2 ? 0 : 180, 0);
    c2d_restore(c);
}

void build_ui_render(Client *client) {
    const Player *me = client ? client->me : NULL;
    const int in_game = me && me->active && me->weapon && !html_ui_is_menu_open() && !hud_ui_end_screen_up();
    const int wheel = in_game && me->select_build && me->weapon->build;
    const int charge = in_game && (build_ui_charge_progress(me) > 0.0f || me->over_charged);
    const ClientRenderSettings *rs = client_render_settings();
    const int cross = in_game && me->weapon->build && !me->select_build && rs && rs->crosshair_type == 0;
    const int show = wheel || charge || cross;
    if (!show && !g_bui.drawn) return;

    GLFWwindow *window = headless_current_window();
    int fb_w = 0, fb_h = 0;
    if (window) glfwGetFramebufferSize(window, &fb_w, &fb_h);
    if (fb_w <= 0 || fb_h <= 0) return;
    float vw = 0, vh = 0, dpr = 1;
    html_dom_viewport(&vw, &vh, &dpr);
    if (vw <= 0 || vh <= 0) vw = (float) fb_w, vh = (float) fb_h;
    if (!g_bui.canvas) g_bui.canvas = c2d_create();
    Canvas2D *c = g_bui.canvas;
    if (!c) return;
    if (c2d_width(c) != (int) vw || c2d_height(c) != (int) vh) c2d_set_size(c, (int) vw, (int) vh, fb_w, fb_h);
    c2d_clear_rect(c, 0, 0, vw, vh);
    g_bui.drawn = show;
    if (show) {
        const double scale = build_ui_scale(vw, vh);
        const double w = vw / scale, h = vh / scale;
        c2d_save(c);
        c2d_scale(c, scale, scale);
        if (cross) draw_build_cross(c, rs, w, h);
        if (charge) draw_charge(c, me, w, h);
        if (wheel) {
            c2d_save(c);
            c2d_translate(c, w / 2, h / 2);
            draw_build_wheel(c, me, h);
            c2d_restore(c);
        }
        c2d_restore(c);
    }
    c2d_composite(c);
}

int build_ui_shown(void) {
    return g_bui.drawn;
}

void build_ui_fini(void) {
    if (g_bui.canvas) c2d_destroy(g_bui.canvas);
    g_bui.canvas = NULL;
    g_bui.drawn = 0;
}
