// The Krunker v7.1.4 game-overlay canvas items of the objective modes (src/libs/game-overlay.js
// overlay.render), drawn on a Canvas2D layer like the Build Tool's wheel (build_ui.c):
//
//   flags       (1216-1242) mode.flags / oneflag: textures/flag_b.png for the viewer's team's
//               flag, flag_r.png otherwise, at img + 7 (the carrier + flagHOff while carried),
//               max(0.3, 1 - d / 600) * 70 px wide; none while the camera is inside it.
//   objectives  (1244-1272, f1565 2119-2176) mode.objective: the active objective's marker, a
//               45-degree #FBC02D square (80 px, a 12 px rgba(0,0,0,.4) backing) with
//               textures/marker_0.png (64 px), "Capture" ("Deposit" in mode.deposit) above it
//               and UTILS.getChargedTime(objtvTime) ("SS:d") below, 35 px GameFont #fff with a
//               4 px #2e2e2e stroke, scaled max(0.3, 1 - d / 600) * 70 / 144; none while the
//               camera is inside it. With 15 s or less left (and 5 s more in the match) the next
//               objective (uid active + 1, else 0) shows "Next" on a #fff square with
//               marker_0_next.png and no time. "CONTESTED" (a #333 box at .8, 20 px #fff at
//               captureProg) while the viewer stands on the active objective with more players
//               on it (obj.onZone > 1: King of the Hill's "uobj").
//   dom zones   (1333-1438) mode.dom: a circle (r 62.5) #dbdbdb (nobody's) / #5699EB (the viewer's
//               team) / #eb5656, its letter (50 px, #fff, #2e2e2e stroke), DEFEND / LOSING /
//               SECURE / SECURING on a #333 box, the capture ring (captureT / captureTime, 10 px,
//               #5699EB when lastTeamOn is the viewer's team) scaled max(0.3, 1 - d / 600) * 70 /
//               128; and, while the viewer stands on a zone being captured, captureProg's bar
//               (320 x 24 at 50 % / 80 %) under "SECURING X" (#fff fill) / "CONTESTING X" (#eb5656).
//
//   the bomb  (v10.0.3 deob-10.0.3.js:179681-179698) mode.site, the attackers (team 2) only: every pickup holding
//               the Bomb (17: lying on the floor, or planted) not around the camera gets textures/marker_0.png at
//               pickup y + 4, max(0.3, 1 - d / 600) * 40 px wide, with the letter of activeObjective || 0 ("A":
//               Defuse has none) in 50 px #fff GameFont above it. v10 inherits the last site letter's alpha there;
//               KN sets the objective opacity as for the others.
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
#include <defuse.h> // the bomb marker (DEFUSE_BOMB_WEAPON, the attackers' team)
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Player *client_find_player(Client*, int uid); // client/src/main.c
void build_ui_round_rect(Canvas2D*, double x, double y, double w, double h, double r); // build_ui.c: roundRect(.., fill)

#define HO_MARKER_UP 7.0f          // v9918.set(x, y + 7, z)
#define HO_FADE_DIST 600.0f        // 1 - getD3D / 600
#define HO_MIN_SCALE 0.3f
#define HO_SIZE 70.0               // * 70
#define HO_NEXT_SHOW_MS 15000.0    // objtvTime <= 15000
#define HO_NEXT_MARGIN_MS 5000.0   // gameTimer - objtvTime >= 5000
// overlay.captureProg = {color "#ffffff", left 50, top 80, width 400, height 30, opacity 1, scale 0.8}
#define HO_PROG_LEFT 0.5
#define HO_PROG_TOP 0.8
#define HO_PROG_W (400.0 * 0.8)
#define HO_PROG_H (30.0 * 0.8)

static struct {
    Canvas2D *canvas;
    int drawn;
    double last_time;
    float objtv_seen;   // mode->objective_time as last set by a packet (s), to see new ones
    double objtv_ms;    // game.objtvTime: counts down on the client while game.objActive
} g_ho = {.objtv_seen = -1.0f};

typedef struct {
    Canvas2D *c;
    const Camera *camera;
    double w, h;        // canvas units (viewport / scale)
    double aim;         // the viewer's aimVal (1 not aiming)
    double opacity, ads_opacity;
} HoFrame;

static void rgb(Canvas2D *c, const unsigned hex, const float a) {
    c2d_set_fill_color(c, ((hex >> 16) & 255) / 255.0f, ((hex >> 8) & 255) / 255.0f, (hex & 255) / 255.0f, a);
}

static void stroke_rgb(Canvas2D *c, const unsigned hex) {
    c2d_set_stroke_color(c, ((hex >> 16) & 255) / 255.0f, ((hex >> 8) & 255) / 255.0f, (hex & 255) / 255.0f, 1.0f);
}

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
    static const char *const names[] = {"flag_r", "flag_b", "marker_0", "marker_0_next", "jugg_1", "jugg_2"};
    static unsigned char fetched[6];
    char rel[96];
    snprintf(rel, sizeof(rel), "textures/%s.png", name);
    char *full = concat(assets_path(), rel);
    if (!full) return NULL;
    for (int i = 0; i < 6; i++) { // web: fetch each once (the canvas caches the image by path)
        if (!strcmp(name, names[i]) && !fetched[i]) fetched[i] = (unsigned char) (ensure_asset_file(full) ? 1 : 0);
    }
    C2DImage *img = c2d_image(c, full);
    free(full);
    return img && c2d_image_state(img) == 1 ? img : NULL;
}

static void stroked_text(Canvas2D *c, const char *text, const double x, const double y, const double stroke_dx) {
    stroke_rgb(c, 0x2e2e2e);
    c2d_set_line_width(c, 4);
    c2d_stroke_text(c, text, x + stroke_dx, y, 0, 0);
    c2d_fill_text(c, text, x, y, 0, 0);
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

// ------------------------------------------------------------------ the bomb (Defuse)

#define HO_BOMB_UP 4.0f   // v9918.set(x, y + 4, z)
#define HO_BOMB_SIZE 40.0 // * 40
static int bomb_marked(const Client *client, const int team) {
    if (!defuse_enabled(&client->game) || team != DEFUSE_ATTACKERS) return 0; // mode.site && map.manager.sites.length
    for (size_t i = 0; i < client->game.map->object_count; i++) {
        GamePickup k;
        if (client->game.map->objects[i]->pickup && game_pickup_state(&client->game, (int) i, &k) && k.weapon == DEFUSE_BOMB_WEAPON) return 1;
    }
    return 0;
}

static void draw_bomb(const HoFrame *f, const Client *client, const GameMode *mode) {
    const Map *map = client->game.map;
    // iìïïïìi[activeObjective || 0]: Defuse's activeObjective is null on the client, so "A" (KN keeps -1 for none)
    char letter[2] = {(char) ('A' + (mode->active_objective > 0 && mode->active_objective < 26 ? mode->active_objective : 0)), 0};
    for (size_t i = 0; i < map->object_count; i++) {
        GamePickup k;
        if (!map->objects[i]->pickup || !game_pickup_state(&client->game, (int) i, &k) || k.weapon != DEFUSE_BOMB_WEAPON) continue;
        // the pickup's GameObject (pickupZoneX / Z / H) around the camera: none
        if (in_box(f->camera->position, (vec3) {k.position.x, k.position.y + 1.0f, k.position.z}, (vec3) {12.0f, 4.0f, 4.0f})) continue;
        const vec3 p = {k.position.x, k.position.y + HO_BOMB_UP, k.position.z};
        const vec3 cam = f->camera->position;
        const float dx = cam.x - p.x, dy = cam.y - p.y, dz = cam.z - p.z;
        float s = 1.0f - sqrtf(dx * dx + dy * dy + dz * dz) / HO_FADE_DIST;
        if (s < HO_MIN_SCALE) s = HO_MIN_SCALE;
        const double size = s * HO_BOMB_SIZE;
        double fx, fy;
        if (size < 1.0 || !nametags_project(f->camera, p, &fx, &fy)) continue;
        C2DImage *img = ho_texture(f->c, "marker_0");
        if (!img) continue;
        int iw = 0, ih = 0;
        c2d_image_size(img, &iw, &ih);
        if (iw <= 0 || ih <= 0) continue;
        const double x = fx * f->w, y = fy * f->h, scale = size / iw;
        ho_alpha(f, x, y);
        c2d_save(f->c);
        c2d_translate(f->c, x, y);
        c2d_scale(f->c, scale, scale);
        rgb(f->c, 0xffffff, 1);
        c2d_set_font(f->c, 50, "GameFont");
        c2d_set_text_align(f->c, C2D_ALIGN_START);
        c2d_set_text_baseline(f->c, C2D_BASELINE_ALPHABETIC);
        c2d_fill_text(f->c, letter, -50.0 / 2.0, -50.0 / 2.0 - 40.0, 0, 0); // fillText(letter, -size / 2, -size / 2 - 40)
        c2d_draw_image(f->c, img, -iw / 2.0, -ih / 2.0, iw, ih);
        c2d_restore(f->c);
    }
}

// ------------------------------------------------------------------ juggernauts

// v10.0.2's overlay (game.deob.js :160079): a Juggernaut (player.isJuggernaut, the "jg" streak) not
// around the camera gets textures/jugg_1.png (a teammate in a team mode) or jugg_2.png at its
// x, y + height / 2, z, max(0.3, 1 - d / 600) * 70 px wide, objectiveOpacity (adsObjectiveOpacity
// near the crosshair while aiming), in any mode.
static int g_jugg_drawn; // markers drawn by the last render (tests: hud_objectives_juggernauts)

static int juggernauts_up(const Client *client) {
    for (size_t i = 0; i < client->game.player_count; i++) {
        const Player *p = client->game.players[i];
        if (p && p->active && p->is_juggernaut && p != client->me) return 1;
    }
    return 0;
}

static void draw_juggernauts(const HoFrame *f, const Client *client, const GameMode *mode, const int team) {
    for (size_t i = 0; i < client->game.player_count; i++) {
        const Player *p = client->game.players[i];
        if (!p || !p->active || !p->is_juggernaut || p == client->me) continue;
        const vec3 center = {p->position.x, p->position.y + p->height / 2.0f, p->position.z};
        if (in_box(f->camera->position, center, (vec3) {p->scale > 0 ? p->scale : 1.0f, p->height / 2.0f, p->scale > 0 ? p->scale : 1.0f}))
            continue;
        double x, y, size;
        if (!ho_place(f, center, &x, &y, &size)) continue;
        const int ally = p->team && mode && mode->config.teams && p->team == team;
        C2DImage *img = ho_texture(f->c, ally ? "jugg_1" : "jugg_2");
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
        g_jugg_drawn++;
    }
}

int hud_objectives_juggernauts(void) {
    return g_jugg_drawn;
}

// ------------------------------------------------------------------ objectives

// UTILS.getChargedTime: "SS:d" (seconds modulo 60, two digits, then tenths)
static void charged_time(const double ms, char *out, const size_t size) {
    const int tenths = (int) (fmod(ms, 1000.0) / 100.0);
    const int secs = (int) floor(fmod(ms / 1000.0, 60.0));
    snprintf(out, size, "%02d:%d", secs, tenths);
}

// f1565(camera, game, w, h, player, objective, cameraPos, next)
static void draw_objective_marker(const HoFrame *f, const GameMode *mode, const ModeObjective *obj, const int next) {
    double x, y, size;
    if (!ho_place(f, (vec3) {obj->position.x, obj->position.y + HO_MARKER_UP, obj->position.z}, &x, &y, &size)) return;
    C2DImage *img = ho_texture(f->c, next ? "marker_0_next" : "marker_0");
    if (!img) return;
    Canvas2D *c = f->c;
    ho_alpha(f, x, y);
    const double k = size / 144.0, side = 80.0, pad = 12.0, gap = 12.0;
    c2d_save(c);
    c2d_translate(c, x, y);
    c2d_scale(c, k, k);
    c2d_set_font(c, 35, "GameFont");
    c2d_set_text_align(c, C2D_ALIGN_CENTER);
    c2d_set_text_baseline(c, C2D_BASELINE_MIDDLE);
    rgb(c, 0xffffff, 1);
    stroked_text(c, next ? "Next" : mode->config.deposit ? "Deposit" : "Capture", 0, -(side + pad + gap), 0);
    if (!next) {
        char t[16];
        charged_time(g_ho.objtv_ms, t, sizeof(t));
        c2d_set_text_baseline(c, C2D_BASELINE_TOP);
        rgb(c, 0xffffff, 1);
        stroked_text(c, t, 0, side + pad + gap, 0);
    }
    c2d_save(c);
    c2d_translate(c, 0.5, 0.5);
    c2d_rotate(c, M_PI / 4.0);
    c2d_set_fill_color(c, 0, 0, 0, 0.4f);
    c2d_fill_rect(c, -side / 2 - pad, -side / 2 - pad, side + pad * 2, side + pad * 2);
    rgb(c, next ? 0xffffff : 0xFBC02D, 1);
    c2d_fill_rect(c, -side / 2, -side / 2, side, side);
    c2d_restore(c);
    const double m = side * 0.8;
    c2d_draw_image(c, img, -m / 2, -m / 2, m, m);
    c2d_restore(c);
}

// the "CONTESTED" / "SECURING X" box at captureProg: a #333 box at .8 and 20 px text
static void capture_label(const HoFrame *f, const char *text) {
    Canvas2D *c = f->c;
    c2d_set_font(c, 20, "GameFont");
    const double tw = c2d_measure_text(c, text);
    rgb(c, 0x333333, 1);
    c2d_set_global_alpha(c, 0.8);
    c2d_fill_rect(c, -(tw + 50) / 2, -57.5, tw + 50, 50);
    c2d_set_global_alpha(c, 1);
    c2d_set_text_align(c, C2D_ALIGN_CENTER);
    c2d_set_text_baseline(c, C2D_BASELINE_ALPHABETIC);
    rgb(c, 0xffffff, 1);
    c2d_fill_text(c, text, 0, -20, 0, 0);
}

static void draw_objectives(const HoFrame *f, const Client *client, const GameMode *mode, const Player *me) {
    const int active = mode->active_objective;
    int next_uid = 0;
    for (int i = 0; i < mode->objective_count; i++) {
        if (mode->objectives[i].uid == active + 1) next_uid = active + 1;
    }
    const double game_ms = client->game.round_timer * 1000.0;
    const int show_next = !mode->config.deposit && g_ho.objtv_ms <= HO_NEXT_SHOW_MS && game_ms - g_ho.objtv_ms >= HO_NEXT_MARGIN_MS;
    const vec3 cam = f->camera->position;
    for (int i = 0; i < mode->objective_count; i++) {
        const ModeObjective *obj = &mode->objectives[i];
        const int inside = in_box(cam, obj->position, obj->half);
        if (obj->uid != active || inside) {
            if (show_next && obj->uid == next_uid && !inside) draw_objective_marker(f, mode, obj, 1);
        } else {
            draw_objective_marker(f, mode, obj, 0);
        }
        if (obj->uid == active && me && me->mode.on_zone == obj->uid && obj->on_zone > 1) {
            c2d_save(f->c);
            c2d_translate(f->c, f->w * HO_PROG_LEFT, f->h * HO_PROG_TOP);
            capture_label(f, "CONTESTED");
            c2d_restore(f->c);
        }
    }
}

// ------------------------------------------------------------------ domination

static void draw_dom(const HoFrame *f, const GameMode *mode, const Player *me, const int team) {
    Canvas2D *c = f->c;
    const double capture_ms = mode->config.capture_time > 0.0f ? mode->config.capture_time : 10.0f;
    for (int z = 0; z < mode->dom_zone_count && z < MODE_MAX_DOM_ZONES; z++) {
        const ModeDomZone *zone = &mode->dom_zones[z];
        char letter[2] = {(char) ('A' + z % 26), 0};
        const double prog = zone->capture_t / capture_ms;
        double x, y, size;
        if (!in_box(f->camera->position, zone->position, zone->half) &&
            ho_place(f, (vec3) {zone->position.x, zone->position.y + HO_MARKER_UP, zone->position.z}, &x, &y, &size)) {
            const unsigned fill = !zone->captured_by ? 0xdbdbdb : zone->captured_by == team ? 0x5699EB : 0xeb5656;
            const unsigned ring = zone->last_team_on == team ? 0x5699EB : 0xeb5656;
            const int ads = x >= f->w * 0.45 && x <= f->w * 0.55 && y >= f->h * 0.4 && y <= f->h * 0.6 && f->aim != 1.0;
            const double alpha = ads ? f->ads_opacity : f->opacity;
            const double s = 50.0, r = s * 1.25, cx = -s / 10, cy = -s - s / 10;
            c2d_set_global_alpha(c, alpha);
            c2d_save(c);
            c2d_translate(c, x, y);
            c2d_scale(c, size / 128.0, size / 128.0);
            c2d_begin_path(c);
            c2d_arc(c, cx, cy, r, 0, M_PI * 2, 0);
            rgb(c, fill, 1);
            c2d_fill(c);
            c2d_set_font(c, s, "GameFont");
            c2d_set_text_align(c, C2D_ALIGN_LEFT);
            c2d_set_text_baseline(c, C2D_BASELINE_ALPHABETIC);
            rgb(c, 0xffffff, 1);
            stroked_text(c, letter, -s / 2, -s / 2, 1);
            const char *status = zone->captured_by == team ? (zone->capture_t > 0.0f ? "LOSING" : "DEFEND")
                               : zone->capture_t > 0.0f && zone->last_team_on == team ? "SECURING" : "SECURE";
            c2d_set_font(c, s / 2, "GameFont");
            rgb(c, 0x333333, 1);
            c2d_set_global_alpha(c, alpha * 0.8);
            const double tw = c2d_measure_text(c, status);
            c2d_fill_rect(c, -(tw + 50) / 2, -(170 + s / 2), tw + 50, s / 2 + 30);
            rgb(c, 0xffffff, 1);
            c2d_set_text_align(c, C2D_ALIGN_CENTER);
            stroked_text(c, status, 0, -150, 1);
            if (zone->capture_t > 0.0f) {
                c2d_set_global_alpha(c, alpha);
                c2d_set_line_width(c, 10);
                stroke_rgb(c, ring);
                c2d_begin_path(c);
                c2d_translate(c, cx, cy);
                c2d_rotate(c, -M_PI / 2);
                c2d_translate(c, -cx, -cy);
                c2d_arc(c, cx, cy, r + 5, M_PI * 2 * prog, 0, 1);
                c2d_stroke(c);
            }
            c2d_restore(c);
        }
        // captureProg while the viewer stands on a zone being captured
        if (zone->capture_t > 0.0f && me && me->mode.on_zone == z) {
            const int securing = zone->last_team_on == team && zone->captured_by != team;
            char text[32];
            snprintf(text, sizeof(text), "%s %s", securing ? "SECURING" : "CONTESTING", letter);
            c2d_save(c);
            c2d_set_global_alpha(c, 1);
            c2d_translate(c, f->w * HO_PROG_LEFT, f->h * HO_PROG_TOP);
            capture_label(f, text);
            c2d_set_fill_color(c, 0, 0, 0, 0.4f);
            build_ui_round_rect(c, -HO_PROG_W / 2, 0, HO_PROG_W, HO_PROG_H, 4);
            rgb(c, securing ? 0xffffff : 0xeb5656, 1);
            build_ui_round_rect(c, -HO_PROG_W / 2, 0, HO_PROG_W * (prog < 1.0 ? prog : 1.0), HO_PROG_H, 2);
            c2d_restore(c);
        }
    }
}

// ------------------------------------------------------------------ frame

// game.objtvTime: set by "obj" (setObjective) and the join sync, then counted down by the
// client while game.objActive (the game message starts with "next", io_updateGameMessage).
static void ho_objective_clock(const GameMode *mode, const double dt_ms) {
    if (mode->objective_time != g_ho.objtv_seen) {
        g_ho.objtv_seen = mode->objective_time;
        g_ho.objtv_ms = mode->objective_time * 1000.0;
        return;
    }
    if (!strncmp(hud_modes_game_message(), "next", 4)) {
        g_ho.objtv_ms -= dt_ms;
        if (g_ho.objtv_ms < 0.0) g_ho.objtv_ms = 0.0;
    }
}

void hud_objectives_render(Client *client) {
    const GameMode *mode = client && client->game.ready ? client->game.mode : NULL;
    const double now = glfwGetTime();
    const double dt_ms = g_ho.last_time > 0.0 ? (now - g_ho.last_time) * 1000.0 : 0.0;
    g_ho.last_time = now;
    if (mode) ho_objective_clock(mode, dt_ms > 0.0 && dt_ms < 1000.0 ? dt_ms : 0.0);

    // v9926: no death screen, end screen or menu
    const HtmlElem death = html_dom_by_id("deathUIHolder");
    const int shown = mode && client->game.map && !html_ui_is_menu_open() && !hud_ui_end_screen_up() && !(death && html_dom_is_shown(death)) &&
                      ((mode->config.flags || mode->config.oneflag) || (mode->config.objective && mode->objective_count) ||
                       (mode->config.dom && mode->dom_zone_count) || juggernauts_up(client) ||
                      bomb_marked(client, client->me && client->me->team ? client->me->team : spectate_active() ? 1 : 0));
    g_jugg_drawn = 0;
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
        if (mode->config.objective && mode->objective_count) draw_objectives(&f, client, mode, me);
        if (mode->config.dom && mode->dom_zone_count) draw_dom(&f, mode, me, team);
        if (bomb_marked(client, team)) draw_bomb(&f, client, mode);
        draw_juggernauts(&f, client, mode, team);
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
    g_ho.objtv_seen = -1.0f;
}
