// The Krunker v7.1.4 game-overlay canvas items of the objective modes (src/libs/game-overlay.js
// overlay.render), drawn on a Canvas2D layer like the Build Tool's wheel (build_ui.c):
//
//   flags       (1216-1242) mode.flags / oneflag: textures/flag_b.png for the viewer's team's
//               flag, flag_r.png otherwise, at img + 7 (the carrier + flagHOff while carried),
//               max(0.3, 1 - d / 600) * 70 px wide; none while the camera is inside it.
//
// v7 draws these only with #deathUIHolder hidden and neither #endUI nor #menuHolder up; a marker
// within 45-55 % x / 40-60 % y of the screen while aiming takes adsObjOpac (0.5) instead of
// objOpac (1). The viewer's team is the player's, 1 while spectating.
// KrunkNative difference: the layer is composited over the page (as build_ui.c / sprays.c),
// where v7's #game-overlay canvas sits under #uiBase; the HUD never overlaps these items.
#include <gl.h>
#include <client.h>
#include <canvas2d.h>
#include <headless.h>
#include <html_dom.h>
#include <html_ui.h>
#include <ui_logic.h>
#include <hud_modes.h>
#include <nametags.h>
#include <spectate.h>
#include <modes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Player *client_find_player(Client*, int uid); // client/src/main.c

#define HO_MARKER_UP 7.0f          // v9918.set(x, y + 7, z)
#define HO_FADE_DIST 600.0f        // 1 - getD3D / 600
#define HO_MIN_SCALE 0.3f
#define HO_SIZE 70.0               // * 70

static struct {
    Canvas2D *canvas;
    int drawn;
} g_ho;

typedef struct {
    Canvas2D *c;
    const Camera *camera;
    double w, h;        // canvas units (viewport / scale)
    double aim;         // the viewer's aimVal (1 not aiming)
    double opacity, ads_opacity;
} HoFrame;

static int in_box(const vec3 p, const vec3 center, const vec3 half) {
    return p.x >= center.x - half.x && p.x <= center.x + half.x && p.y >= center.y - half.y && p.y <= center.y + half.y &&
           p.z >= center.z - half.z && p.z <= center.z + half.z;
}

// max(0.3, 1 - d / 600) * 70, the projected point in canvas units; 0 when off screen
static int ho_place(const HoFrame *f, const vec3 p, double *x, double *y, double *size) {
    const vec3 cam = f->camera->position;
    const float dx = cam.x - p.x, dy = cam.y - p.y, dz = cam.z - p.z;
    float s = 1.0f - sqrtf(dx * dx + dy * dy + dz * dz) / HO_FADE_DIST;
    if (s < HO_MIN_SCALE) s = HO_MIN_SCALE;
    *size = s * HO_SIZE;
    double fx, fy;
    if (*size < 1.0 || !nametags_project(f->camera, p, &fx, &fy)) return 0;
    *x = fx * f->w;
    *y = fy * f->h;
    return 1;
}

// objectiveOpacity, or adsObjectiveOpacity for a marker near the crosshair while aiming
static void ho_alpha(const HoFrame *f, const double x, const double y) {
    const int ads = x >= f->w * 0.45 && x <= f->w * 0.55 && y >= f->h * 0.4 && y <= f->h * 0.6 && f->aim != 1.0;
    c2d_set_global_alpha(f->c, ads ? f->ads_opacity : f->opacity);
}

static C2DImage *ho_texture(Canvas2D *c, const char *name) {
    static const char *const names[] = {"flag_r", "flag_b", "marker_0", "marker_0_next"};
    static unsigned char fetched[4];
    char rel[96];
    snprintf(rel, sizeof(rel), "textures/%s.png", name);
    char *full = concat(assets_path(), rel);
    if (!full) return NULL;
    for (int i = 0; i < 4; i++) { // web: fetch each once (the canvas caches the image by path)
        if (!strcmp(name, names[i]) && !fetched[i]) fetched[i] = (unsigned char) (ensure_asset_file(full) ? 1 : 0);
    }
    C2DImage *img = c2d_image(c, full);
    free(full);
    return img && c2d_image_state(img) == 1 ? img : NULL;
}

// ------------------------------------------------------------------ flags

static void draw_flags(const HoFrame *f, const Client *client, const GameMode *mode, const int team) {
    for (int i = 0; i < hud_modes_flag_count() && i < mode->flag_count; i++) {
        if (!mode->flags[i].active) continue; // (!oneflag || neutral) && (oneflag || !neutral)
        vec3 p;
        int flag_team = 0, carrier = -1;
        if (!hud_modes_flag(i, &p, &flag_team, &carrier)) continue;
        if (carrier >= 0) { // img: the carrier + flagHOff (refreshed by "ufp"; KN: the live model)
            const Player *holder = client_find_player((Client *) client, carrier);
            if (holder) p = (vec3) {holder->position.x, holder->position.y + MODE_FLAG_HOFF, holder->position.z};
        }
        const vec3 half = {MODE_FLAG_SCALE, MODE_FLAG_SCALE * 0.5f, MODE_FLAG_SCALE};
        if (in_box(f->camera->position, p, half)) continue;
        double x, y, size;
        if (!ho_place(f, (vec3) {p.x, p.y + HO_MARKER_UP, p.z}, &x, &y, &size)) continue;
        C2DImage *img = ho_texture(f->c, flag_team == team ? "flag_b" : "flag_r");
        if (!img) continue;
        int iw = 0, ih = 0;
        c2d_image_size(img, &iw, &ih);
        if (iw <= 0 || ih <= 0) continue;
        ho_alpha(f, x, y);
        const double k = size / iw;
        c2d_save(f->c);
        c2d_translate(f->c, x, y);
        c2d_scale(f->c, k, k);
        c2d_draw_image(f->c, img, -iw / 2.0, -ih / 2.0, iw, ih);
        c2d_restore(f->c);
    }
}

// ------------------------------------------------------------------ frame

void hud_objectives_render(Client *client) {
    const GameMode *mode = client && client->game.ready ? client->game.mode : NULL;

    // v9926: no death screen, end screen or menu
    const HtmlElem death = html_dom_by_id("deathUIHolder");
    const int shown = mode && client->game.map && !html_ui_is_menu_open() && !hud_ui_end_screen_up() && !(death && html_dom_is_shown(death)) &&
                      (mode->config.flags || mode->config.oneflag);
    if (!shown && !g_ho.drawn) return;

    GLFWwindow *window = headless_current_window();
    int fb_w = 0, fb_h = 0;
    if (window) glfwGetFramebufferSize(window, &fb_w, &fb_h);
    if (fb_w <= 0 || fb_h <= 0) return;
    float vw = 0, vh = 0, dpr = 1;
    html_dom_viewport(&vw, &vh, &dpr);
    if (vw <= 0 || vh <= 0) vw = (float) fb_w, vh = (float) fb_h;
    if (!g_ho.canvas) g_ho.canvas = c2d_create();
    Canvas2D *c = g_ho.canvas;
    if (!c) return;
    if (c2d_width(c) != (int) vw || c2d_height(c) != (int) vh) c2d_set_size(c, (int) vw, (int) vh, fb_w, fb_h);
    c2d_clear_rect(c, 0, 0, vw, vh);
    g_ho.drawn = shown;
    if (shown) {
        const double scale = nametags_ui_scale(vw, vh);
        const Player *me = client->me && client->me->active ? client->me : NULL;
        HoFrame f = {.c = c, .camera = &client->camera, .w = vw / scale, .h = vh / scale, .aim = me ? me->aim_val : 1.0,
                     .opacity = 1.0, .ads_opacity = 0.5};
        double v;
        if (ui_setting_number("objOpac", &v) && v == v) f.opacity = v;
        if (ui_setting_number("adsObjOpac", &v) && v == v) f.ads_opacity = v;
        const int team = client->me && client->me->team ? client->me->team : spectate_active() ? 1 : 0;
        c2d_save(c);
        c2d_scale(c, scale, scale);
        if (mode->config.flags || mode->config.oneflag) draw_flags(&f, client, mode, team);
        c2d_restore(c);
        c2d_set_global_alpha(c, 1);
    }
    c2d_composite(c);
}

int hud_objectives_shown(void) {
    return g_ho.drawn;
}

void hud_objectives_fini(void) {
    if (g_ho.canvas) c2d_destroy(g_ho.canvas);
    g_ho.canvas = NULL;
    g_ho.drawn = 0;
}
