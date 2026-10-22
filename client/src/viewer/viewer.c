// Krunker v7.1.4's model viewer (public/viewer.html + src/viewer.js) as a KrunkNative page:
// ui/viewer.html is the page, this module is viewer.js. app_mode.c shows it for
// `client_bin --viewer [query]`, the web page viewer.html?<query> and the menu's links to
// /viewer.html; the query is viewer.js's URLSearchParams:
//
//   skinIdP skinIdS face hat head body back waist shoe pet wrist melee skinCol dye secIndex class
//   attach season    initial panel values (parseInt(x) || -1, class || 0, secIndex || 2, season ||
//                    config.currentSeason). body is store type 2, back type 14 (v3.1.1's "back" was type 2)
//   hidePlayer hideBody hideBack hideWaist hideFace hideHat hideHead hidePet hideWrist hideArms
//   hideLegs hideWep start with that "Show ..." box unticked
//   anim / crouch    start animating / crouched
//   swap=-1|1       show the secondary / melee      nosup   keep skinIdP/S through the first f451()
//   cosmetic=<i>    render page for one store item (512 x 512 canvas, "Render" button)
//   auto            with cosmetic: export every item from <i> on      size=<px> export size
//   showcase=<i>    one store item on its own, auto-rotating (the store's item view)
//   nogui           no panel, the weapon close up    noTex   untextured models
//   nobg            no background gradient
//   dev             the Render <slot> buttons, the <Slot> / <Weapon> Config editors with Export
//                   Config (viewer_config.c) and every dropdown newest store index first
// KRUNKNATIVE_VIEWER_FREEZE=1 stops the clock (reference captures, tools/kref/viewer-ref.js);
// KRUNKNATIVE_VIEWER_GUI="<Name or Folder/Name>[=<value>];..." acts on the panel after the load.
//
// The panel is dat.gui (viewer_gui.c), the scene viewer_scene.c, Load / drop / zip imports and
// downloads viewer_import.c, the dev config editors viewer_config.c. The same showcase scene
// also fills the store item view's iframe in the game and hub pages (viewer_embed.h, below).
#include "viewer_internal.h"
#include "klog.h"
#include <viewer_embed.h>
#include <cosmetics.h>
#include <app_mode.h>
#include <html_ui.h>
#include <ui_logic.h> // ui_text_input_focused
#include <cJSON.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ruleset.h>

#define VIEWER_GUI_WIDTH 322           // e.width = 322
#define VIEWER_RENDER_DELAY_MS 750.0   // $(): setTimeout(..., 750)
#define VIEWER_POSE_DELAY_MS 500.0     // renderHat & co: setTimeout(..., 500)
#define VIEWER_EXPORT_CROP 550         // drawImage(..., 550 * dpr, 550 * dpr, ...)
#define VIEWER_MAX_PARAMS 48
#define VIEWER_MAX_HANDLERS 32

Viewer g_viewer;

static struct {
    char *keys[VIEWER_MAX_PARAMS];
    char *values[VIEWER_MAX_PARAMS];
    int count;
} g_params;

static struct {
    VgFolder *player, *weapons;
    VgCtrl *m[64];   // vA346: the Player folder's controllers
    int m_count;
    VgCtrl *b[16];   // vA345: the Weapons folder's controllers
    int b_count;
    int keep_skins;  // v5706 = params.has("nosup")
    int render_timer;
    int entered;
    float last_x, last_y;
} g_ui;

// ---------------------------------------------------------------- URL params

static int hex_value(const char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static char *url_decode(const char *s, const size_t n) {
    char *out = malloc(n + 1);
    if (!out) return NULL;
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '+') out[o++] = ' ';
        else if (s[i] == '%' && i + 2 < n + 0 && hex_value(s[i + 1]) >= 0 && hex_value(s[i + 2]) >= 0) {
            out[o++] = (char) (hex_value(s[i + 1]) * 16 + hex_value(s[i + 2]));
            i += 2;
        } else out[o++] = s[i];
    }
    out[o] = 0;
    return out;
}

static void params_clear(void) {
    for (int i = 0; i < g_params.count; i++) {
        free(g_params.keys[i]);
        free(g_params.values[i]);
    }
    g_params.count = 0;
}

static void params_parse(const char *query) {
    params_clear();
    if (!query) return;
    if (*query == '?') query++;

    while (*query && g_params.count < VIEWER_MAX_PARAMS) {
        const char *end = strchr(query, '&');
        const size_t length = end ? (size_t) (end - query) : strlen(query);
        if (length) {
            const char *eq = memchr(query, '=', length);
            const size_t key_length = eq ? (size_t) (eq - query) : length;
            g_params.keys[g_params.count] = url_decode(query, key_length);
            g_params.values[g_params.count] = eq ? url_decode(eq + 1, length - key_length - 1) : url_decode("", 0);
            g_params.count++;
        }
        query += length + (end ? 1 : 0);
    }
}

int viewer_param_has(const char *key) {
    return viewer_param_get(key) != NULL;
}

const char *viewer_param_get(const char *key) {
    for (int i = 0; i < g_params.count; i++) {
        if (g_params.keys[i] && !strcmp(g_params.keys[i], key)) return g_params.values[i];
    }
    return NULL;
}

// parseInt(params.get(key)): leading integer, *ok = 0 for NaN
static int js_parse_int(const char *s, int *ok) {
    *ok = 0;
    if (!s) return 0;
    while (*s == ' ' || *s == '\t' || *s == '\n') s++;
    char *end;
    const long v = strtol(s, &end, 10);
    *ok = end != s;
    return (int) v;
}

// parseInt(params.get(key)) || fallback (0 and NaN both fall back)
static int param_int_or(const char *key, const int fallback) {
    int ok;
    const int v = js_parse_int(viewer_param_get(key), &ok);
    return ok && v ? v : fallback;
}

static void value_set(VgValue *value, const int num) {
    memset(value, 0, sizeof(*value));
    value->num = num;
}

// ---------------------------------------------------------------- panel (H / G / W)

static void Z(void) {
    vscene_rebuild_player();
}

static int classes_count(void) {
    return (int) (rules_class_count());
}

static const ClassConfig *current_class(void) {
    const int index = g_viewer.e.class_index.num;
    return &rules_classes()[index >= 0 && index < classes_count() ? index : 0];
}

// .options(obj): the labels / values of an option object, in key order
typedef struct {
    int count;
    char labels[640][96]; // the longest list (season All, faces) has 545 entries
    char values[640][16];
} OptionBuffer;

static OptionBuffer g_options;

static void options_reset(void) {
    g_options.count = 0;
}

static void options_push(const char *label, const int value) {
    // an existing key keeps its place and takes the new value (JS object semantics)
    for (int i = 0; i < g_options.count; i++) {
        if (!strcmp(g_options.labels[i], label)) {
            snprintf(g_options.values[i], sizeof(g_options.values[i]), "%d", value);
            return;
        }
    }
    if (g_options.count >= (int) (sizeof(g_options.labels) / sizeof(g_options.labels[0]))) return;
    snprintf(g_options.labels[g_options.count], sizeof(g_options.labels[0]), "%s", label);
    snprintf(g_options.values[g_options.count], sizeof(g_options.values[0]), "%d", value);
    g_options.count++;
}

static VgCtrl *add_options(VgFolder *folder, VgValue *value, const char *name, const VgFn on_change) {
    static const char *labels[640], *values[640];
    for (int i = 0; i < g_options.count; i++) {
        labels[i] = g_options.labels[i];
        values[i] = g_options.values[i];
    }
    return vg_add_options(folder, value, name, g_options.count, labels, values, on_change);
}

// f459 / f460 / the melee list in dev mode: newest store index first (the generated lists are
// the player's order: by season, then rarity, then id)
static void options_from_dev(const ViewerListKind kind, const int key) {
    options_reset();
    options_push("Default", -1);
    const int season = g_viewer.e.season.num;
    for (int i = g_viewer_skin_count - 1; i >= 0; i--) {
        const ViewerSkin *skin = &g_viewer_skins[i];
        if (skin->flags & VS_BLOCKED) continue;
        if (season && season != (skin->seas ? skin->seas : 1)) continue;
        if (kind == VIEWER_LIST_TYPE ? skin->type != key || skin->weapon : kind == VIEWER_LIST_MELEE ? skin->type != ITEM_MELEE : skin->weapon != key) continue;
        const char *rarity = (skin->flags & VS_FREE) ? "Free" : skin->rarity >= 0 && skin->rarity < g_viewer_rarity_count ? g_viewer_rarities[skin->rarity].name : "";
        char label[128];
        snprintf(label, sizeof(label), "%s [%s]", skin->name, rarity);
        options_push(label, i);
    }
}

static void options_from_list(const ViewerListKind kind, const int key) {
    if (g_viewer.dev) {
        options_from_dev(kind, key);
        return;
    }
    options_reset();
    const ViewerOptionList *list = viewer_option_list(kind, key, g_viewer.e.season.num);
    if (!list) {
        options_push("Default", -1);
        return;
    }
    for (int i = 0; i < list->count; i++) {
        char label[96];
        viewer_option_label(list, i, label, sizeof(label));
        options_push(label, viewer_option_value(list, i));
    }
}

static void W(int first);
static void G(void);

// a dropdown / box changed; in dev mode a slot dropdown also retargets its config folder
static void on_change_z(void) {
    if (g_viewer.dev) vconfig_changed();
    Z();
}

// Show Model: Z() and the camera to the model or the weapon
static void on_show_player(void) {
    Z();
    vscene_camera_player(g_viewer.e.show_player);
}

static void on_class(void) {
    g_viewer.swap = 0;
    W(0);
}

static void on_secondary(void) {
    Z();
    W(0);
}

static void on_season(void) {
    G();
    W(0);
}

static void on_lighting(void) {
    vscene_update_lights();
}

static void on_crouch(void) {
    Z();
}

// ---- function controllers

// f461(type): Load Custom <slot> (up to 3 files) / Load Skin (1 file, the weapon)
static void fn_custom_face(void) { viewer_import_pick(3, ITEM_FACE); }
static void fn_custom_hat(void) { viewer_import_pick(3, ITEM_HAT); }
static void fn_custom_head(void) { viewer_import_pick(3, ITEM_HEAD); }
static void fn_custom_body(void) { viewer_import_pick(3, ITEM_BODY); }
static void fn_custom_back(void) { viewer_import_pick(3, ITEM_BACK); }
static void fn_custom_waist(void) { viewer_import_pick(3, ITEM_WAIST); }
static void fn_custom_shoes(void) { viewer_import_pick(3, ITEM_SHOE); }
static void fn_custom_pet(void) { viewer_import_pick(3, ITEM_PET); }
static void fn_custom(void) { viewer_import_pick(1, 0); }

static void fn_reset(void) {
    vscene_reset_overrides();
    Z();
}

static void fn_swap(void) {
    g_viewer.swap = g_viewer.swap ? 0 : -1;
    Z();
}

static void fn_swap_melee(void) {
    g_viewer.swap = g_viewer.swap ? 0 : 1;
    Z();
}

static void fn_screenshot(void) {
    float x, y, w, h, vw = 0.0f, vh = 0.0f, dpr = 1.0f;
    vscene_canvas_rect(&x, &y, &w, &h);
    html_dom_viewport(&vw, &vh, &dpr);
    const float ratio = dpr * (g_viewer.hi_quality ? 2.0f : 1.0f); // setPixelRatio(devicePixelRatio * (I ? 2 : 1))
    const int cw = (int) lroundf(w * ratio), ch = (int) lroundf(h * ratio);
    unsigned char *rgba = vscene_capture(cw, ch, g_viewer.e.gradient);
    if (rgba) viewer_download_png("render.png", rgba, cw, ch);
    free(rgba);
}

// drawImage(canvas, sx, sy, sw, sh, 0, 0, size, size): crop + bilinear scale
static unsigned char *crop_scale(const unsigned char *src, const int sw_img, const int sh_img, const float sx, const float sy, const float sw, const float sh, const int size) {
    unsigned char *out = calloc((size_t) size * size, 4);
    if (!out) return NULL;
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            const float fx = sx + ((float) x + 0.5f) * sw / (float) size - 0.5f;
            const float fy = sy + ((float) y + 0.5f) * sh / (float) size - 0.5f;
            const int x0 = (int) floorf(fx), y0 = (int) floorf(fy);
            const float tx = fx - (float) x0, ty = fy - (float) y0;
            for (int c = 0; c < 4; c++) {
                float acc = 0.0f;
                for (int k = 0; k < 4; k++) {
                    const int px = x0 + (k & 1), py = y0 + (k >> 1);
                    const float wgt = ((k & 1) ? tx : 1.0f - tx) * ((k >> 1) ? ty : 1.0f - ty);
                    if (px < 0 || py < 0 || px >= sw_img || py >= sh_img) continue;
                    acc += wgt * src[((size_t) py * sw_img + px) * 4 + c];
                }
                out[((size_t) y * size + x) * 4 + c] = (unsigned char) fminf(255.0f, acc + 0.5f);
            }
        }
    }
    return out;
}

static void capture_crop_download(const char *name, const float sx, const float sy, const float s, const int size) {
    float x, y, w, h, vw = 0.0f, vh = 0.0f, dpr = 1.0f;
    vscene_canvas_rect(&x, &y, &w, &h);
    html_dom_viewport(&vw, &vh, &dpr);
    const float ratio = dpr * (g_viewer.hi_quality ? 2.0f : 1.0f);
    const int cw = (int) lroundf(w * ratio), ch = (int) lroundf(h * ratio);
    unsigned char *rgba = vscene_capture(cw, ch, 0);
    if (!rgba) return;
    unsigned char *out = crop_scale(rgba, cw, ch, sx * dpr, sy * dpr, s * dpr, s * dpr, size);
    if (out) viewer_download_png(name, out, size, size);
    free(out);
    free(rgba);
}

// f463(): the crop origin, vO205 per weapon (weapons 0-29) or vO206 per item type, with the per-model fixes
static void render_crop(const ViewerSkin *e, float *sx, float *sy) {
    static const short weapons[30][2] = {
        [0] = {210, 256}, [1] = {400, 400}, [2] = {310, 310}, [3] = {220, 286}, [4] = {235, 256}, [5] = {225, 300},
        [7] = {256, 300}, [8] = {310, 310}, [10] = {210, 270}, [11] = {210, 330}, [12] = {210, 256}, [13] = {210, 286},
        [14] = {210, 320}, [15] = {210, 220}, [16] = {245, 220}, [17] = {225, 285}, [18] = {190, 256}, [19] = {256, 256},
        [21] = {195, 256}, [22] = {220, 286}, [23] = {205, 220}, [24] = {205, 220}, [25] = {220, 256}, [28] = {220, 286},
        [29] = {350, 400},
    };
    static const short types[17][2] = {
        [1] = {240, 120}, [15] = {240, 120}, [3] = {210, 256}, [4] = {230, 210}, [5] = {256, 435}, [7] = {240, 120},
        [8] = {240, 450}, [9] = {235, 220}, [10] = {210, 256}, [12] = {230, 330}, [13] = {230, 195},
    };
    const short *t = NULL;
    if (!e->type) t = e->weapon >= 0 && e->weapon < 30 ? weapons[e->weapon] : NULL;
    else if (e->type > 0 && e->type < 17) t = types[e->type];
    *sx = t && t[0] ? (float) t[0] : 256.0f;
    *sy = t && t[1] ? (float) t[1] : 256.0f;
    if (e->weapon == 1 && e->mid >= 0) *sx = 350.0f;
    if (e->type == ITEM_MELEE) {
        if (e->id == 1) *sx = 170.0f;
        else if (e->id == 10 || e->id == 18 || e->id == 33 || e->id == 34) *sx = 245.0f;
        else if (e->id == 3 || e->id == 14) *sx = 145.0f;
    }
}

static void render_next(void);

static void render_capture(void *user) {
    (void) user;
    g_ui.render_timer = 0;
    const ViewerSkin *e = viewer_skin(g_viewer.cosmetic);
    if (!e) return;
    float sx, sy;
    render_crop(e, &sx, &sy);
    char name[128];
    viewer_preview_export_name(e, name, sizeof(name));
    capture_crop_download(name, sx, sy, VIEWER_EXPORT_CROP, g_viewer.export_size);
    if (g_viewer.auto_render) render_next();
}

// f447(C) with `auto`: skip to the next showable item; past the end auto stops
static void cosmetic_show(void) {
    while (g_viewer.auto_render && !vscene_cosmetic_shown(g_viewer.cosmetic)) {
        if (++g_viewer.cosmetic >= g_viewer_skin_count) {
            g_viewer.cosmetic = 0;
            g_viewer.auto_render = 0;
            return;
        }
    }
    vscene_build_cosmetic(g_viewer.cosmetic);
}

static void fn_render_wep(void) {
    const ViewerSkin *e = viewer_skin(g_viewer.cosmetic);
    if (!e) return;
    if (vscene_cosmetic_shown(g_viewer.cosmetic)) {
        if (g_ui.render_timer) html_timer_cancel(g_ui.render_timer);
        g_ui.render_timer = html_timer_add(VIEWER_RENDER_DELAY_MS, render_capture, NULL);
    } else if (g_viewer.auto_render) {
        render_next();
    }
}

static void render_next(void) {
    g_viewer.cosmetic++;
    cosmetic_show();
    if (g_viewer.auto_render) fn_render_wep();
}

static int g_pose_type;

static void pose_capture(void *user) {
    (void) user;
    float x, y, w, h, vw = 0.0f, vh = 0.0f, dpr = 1.0f;
    vscene_canvas_rect(&x, &y, &w, &h);
    html_dom_viewport(&vw, &vh, &dpr);
    const float half = w / 2.0f;
    const ViewerState *e = &g_viewer.e;
    const VgValue *values[17] = {[ITEM_HAT] = &e->hat, [ITEM_HEAD] = &e->head, [ITEM_BODY] = &e->body, [ITEM_BACK] = &e->back,
                                 [ITEM_DYE] = &e->dye, [ITEM_WAIST] = &e->waist, [ITEM_FACE] = &e->face, [ITEM_SHOE] = &e->shoe,
                                 [ITEM_PET] = &e->pet};
    const ViewerSkin *skin = values[g_pose_type] ? viewer_skin(values[g_pose_type]->num) : NULL;
    char name[128];
    if (skin) viewer_preview_export_name(skin, name, sizeof(name));
    else snprintf(name, sizeof(name), "%d_default.png", g_pose_type == ITEM_HEAD ? 1 : g_pose_type == ITEM_BACK ? 2 : g_pose_type);

    // drawImage source rectangles of renderHat / Head / Face / Body / Back / Waist / Shoes / Dye / Pet (CSS px, times dpr)
    switch (g_pose_type) {
        case ITEM_HAT: case ITEM_HEAD: capture_crop_download(name, half - 128.0f, 100.0f, 256.0f, g_viewer.export_size); break;
        case ITEM_PET: capture_crop_download(name, half - 100.0f, 128.0f, 256.0f, g_viewer.export_size); break;
        case ITEM_FACE: capture_crop_download(name, half - 100.0f, 150.0f, 200.0f, g_viewer.export_size); break;
        case ITEM_BODY: case ITEM_BACK: case ITEM_SHOE: capture_crop_download(name, half - 256.0f, 100.0f, 512.0f, g_viewer.export_size); break;
        case ITEM_WAIST: capture_crop_download(name, half - 256.0f, 100.0f + (skin && (skin->flags & VS_LOW_R) ? 100.0f : 0.0f), 512.0f, g_viewer.export_size); break;
        case ITEM_DYE: capture_crop_download(name, half - 256.0f, 200.0f, 512.0f, g_viewer.export_size); break;
        default: break;
    }
}

static void render_pose(const int type) {
    g_pose_type = type;
    vscene_camera_render_pose(type);
    html_timer_add(VIEWER_POSE_DELAY_MS, pose_capture, NULL);
}

static void fn_render_hat(void) { render_pose(ITEM_HAT); }
static void fn_render_head(void) { render_pose(ITEM_HEAD); }
static void fn_render_face(void) { render_pose(ITEM_FACE); }
static void fn_render_body(void) { render_pose(ITEM_BODY); }
static void fn_render_back(void) { render_pose(ITEM_BACK); }
static void fn_render_waist(void) { render_pose(ITEM_WAIST); }
static void fn_render_shoes(void) { render_pose(ITEM_SHOE); }
static void fn_render_dye(void) { render_pose(ITEM_DYE); }
static void fn_render_pet(void) { render_pose(ITEM_PET); }

// ---- f450(): the Player folder

#define PUSH_M(c) do { if (g_ui.m_count < 64) g_ui.m[g_ui.m_count++] = (c); } while (0)
#define PUSH_B(c) do { if (g_ui.b_count < 16) g_ui.b[g_ui.b_count++] = (c); } while (0)

// <Name> dropdown (f460(type)), Load Custom <Name>, [dev] <Name> Config folder (f453; not for
// pets / wrists) and Render <Name>, Show <Name>
static void slot_rows(VgValue *value, const int type, const char *name, const VgFn custom, const VgFn render, int *show) {
    char label[48];
    options_from_list(VIEWER_LIST_TYPE, type);
    PUSH_M(add_options(g_ui.player, value, name, on_change_z));
    if (custom) {
        snprintf(label, sizeof(label), "Load Custom %s", name);
        PUSH_M(vg_add_function(g_ui.player, custom, label));
    }
    if (g_viewer.dev && type != ITEM_PET && type != ITEM_WRIST) {
        snprintf(label, sizeof(label), "%s Config", name);
        vconfig_folder(g_ui.player, type, label);
    }
    if (render && viewer_param_has("dev")) {
        snprintf(label, sizeof(label), "Render %s", name);
        PUSH_M(vg_add_function(g_ui.player, render, label));
    }
    if (show) {
        snprintf(label, sizeof(label), "Show %s", name);
        PUSH_M(vg_add_bool(g_ui.player, show, label, on_change_z));
    }
}

static void G(void) {
    for (int i = 0; i < g_ui.m_count; i++) vg_remove(g_ui.player, g_ui.m[i]);
    g_ui.m_count = 0;
    vconfig_remove_folders(g_ui.player);
    ViewerState *e = &g_viewer.e;

    PUSH_M(vg_add_bool(g_ui.player, &e->show_player, "Show Player", on_show_player));
    PUSH_M(vg_add_bool(g_ui.player, &e->show_arms, "Show Arms", on_change_z));
    PUSH_M(vg_add_bool(g_ui.player, &e->show_legs, "Show Legs", on_change_z));

    options_reset();
    for (int i = 0; i < classes_count(); i++) options_push(rules_classes()[i].name, i);
    PUSH_M(add_options(g_ui.player, &e->class_index, "Class", on_class));

    slot_rows(&e->face, ITEM_FACE, "Face", fn_custom_face, fn_render_face, &e->show_face);
    slot_rows(&e->hat, ITEM_HAT, "Hat", fn_custom_hat, fn_render_hat, &e->show_hat);
    slot_rows(&e->head, ITEM_HEAD, "Head", fn_custom_head, fn_render_head, &e->show_head);
    slot_rows(&e->body, ITEM_BODY, "Body", fn_custom_body, fn_render_body, &e->show_body);
    slot_rows(&e->back, ITEM_BACK, "Back", fn_custom_back, fn_render_back, &e->show_back);
    slot_rows(&e->waist, ITEM_WAIST, "Waist", fn_custom_waist, fn_render_waist, &e->show_waist);
    slot_rows(&e->shoe, ITEM_SHOE, "Shoes", fn_custom_shoes, fn_render_shoes, NULL);

    options_reset();
    options_push("Default", -1);
    for (int i = 0; i < g_viewer_skin_color_count; i++) options_push(g_viewer_skin_colors[i].name, g_viewer_skin_colors[i].id);
    PUSH_M(add_options(g_ui.player, &e->skin_col, "Skin Color", on_change_z));

    slot_rows(&e->dye, ITEM_DYE, "Dye", NULL, fn_render_dye, NULL);
    slot_rows(&e->pet, ITEM_PET, "Pet", fn_custom_pet, fn_render_pet, &e->show_pet);
    slot_rows(&e->wrist, ITEM_WRIST, "Wrist", NULL, NULL, &e->show_wrist);

    vg_folder_open(g_ui.player);
    vconfig_snapshot();
}

// ---- W(): the Weapons folder

static void W(const int first) {
    for (int i = 0; i < g_ui.b_count; i++) vg_remove(g_ui.weapons, g_ui.b[i]);
    g_ui.b_count = 0;
    vconfig_remove_folders(g_ui.weapons);
    ViewerState *e = &g_viewer.e;

    if (!g_ui.keep_skins) {
        value_set(&e->skin_prim, -1);
        value_set(&e->skin_sec, -1);
    }
    g_ui.keep_skins = 0;

    const ClassConfig *cls = current_class();
    const int primary = cls->loadout ? cls->loadout[0] : 0;

    PUSH_B(vg_add_bool(g_ui.weapons, &e->show_wep, "Show Weapon", on_change_z));

    options_from_list(VIEWER_LIST_WEAPON, primary + 1);
    PUSH_B(add_options(g_ui.weapons, &e->skin_prim, "Primary Skin", on_change_z));
    if (g_viewer.dev) vconfig_folder(g_ui.weapons, -1, "Primary Config");

    options_reset();
    options_push("None", -1);
    const Weapon *primary_weapon = primary >= 0 && primary < rules_krunker_weapon_count() ? rules_weapon(primary) : NULL;
    if (primary_weapon && primary_weapon->has_attach) {
        for (int i = 0; i < ATTACHMENT_COUNT; i++) options_push(g_attachments[i].name, i);
    }
    PUSH_B(add_options(g_ui.weapons, &e->attach, "Attachments", on_change_z));

    if (cls->secondary) {
        options_reset();
        for (int i = 0; i < rules_krunker_weapon_count(); i++) {
            if (rules_weapon(i) && rules_weapon(i)->secondary) options_push(rules_weapon(i)->name, i);
        }
        PUSH_B(add_options(g_ui.weapons, &e->secondary, "Secondary", on_secondary));

        options_from_list(VIEWER_LIST_WEAPON, e->secondary.num + 1);
        PUSH_B(add_options(g_ui.weapons, &e->skin_sec, "Secondary Skin", on_change_z));
        if (g_viewer.dev) vconfig_folder(g_ui.weapons, -2, "Secondary Config");
    }

    options_from_list(VIEWER_LIST_MELEE, 3);
    PUSH_B(add_options(g_ui.weapons, &e->melee, "Melee", on_change_z));
    if (g_viewer.dev) vconfig_folder(g_ui.weapons, -3, "Melee Config");

    if (cls->secondary) PUSH_B(vg_add_function(g_ui.weapons, fn_swap, "Swap Secondary"));
    PUSH_B(vg_add_function(g_ui.weapons, fn_swap_melee, "Swap Melee"));
    PUSH_B(vg_add_function(g_ui.weapons, fn_custom, "Load Skin"));
    PUSH_B(vg_add_function(g_ui.weapons, fn_reset, "Reset Skins"));

    vg_folder_open(g_ui.weapons);
    vconfig_snapshot();
    Z();
    if (!e->show_player && first) vscene_camera_weapon();
}

static void H(void) {
    vg_init(VIEWER_GUI_WIDTH);
    VgFolder *root = vg_root();
    ViewerState *e = &g_viewer.e;

    if (g_viewer.cosmetic_set) {
        vg_add_function(root, fn_render_wep, "Render");
        return;
    }

    // {All: 0, 1: 1, ..., currentSeason}: integer keys first
    options_reset();
    for (int i = 1; i <= g_viewer_current_season; i++) {
        char label[8];
        snprintf(label, sizeof(label), "%d", i);
        options_push(label, i);
    }
    options_push("All", 0);
    add_options(root, &e->season, "Season", on_season);

    g_ui.player = vg_add_folder(root, "Player");
    G();
    g_ui.weapons = vg_add_folder(root, "Weapons");
    W(1);
    vg_add_bool(root, &e->animate, "Animate", NULL);
    vg_add_bool(root, &e->crouch, "Crouch", on_crouch);
    options_reset();
    g_options.count = 2;
    snprintf(g_options.labels[0], sizeof(g_options.labels[0]), "Viewer");
    snprintf(g_options.values[0], sizeof(g_options.values[0]), "Viewer");
    snprintf(g_options.labels[1], sizeof(g_options.labels[1]), "In-game");
    snprintf(g_options.values[1], sizeof(g_options.values[1]), "In-game");
    add_options(root, &e->lighting, "Lighting", on_lighting);
    vg_add_bool(root, &e->gradient, "Screenshot Gradient", NULL);
    vg_add_function(root, fn_screenshot, "Take a Screenshot");
}

// ---------------------------------------------------------------- state from the URL

static void state_from_params(void) {
    ViewerState *e = &g_viewer.e;
    memset(e, 0, sizeof(*e));

    value_set(&e->skin_prim, param_int_or("skinIdP", -1));
    value_set(&e->skin_sec, param_int_or("skinIdS", -1));
    value_set(&e->shoe, param_int_or("shoe", -1));
    value_set(&e->face, param_int_or("face", -1));
    value_set(&e->hat, param_int_or("hat", -1));
    value_set(&e->head, param_int_or("head", -1));
    value_set(&e->body, param_int_or("body", -1));
    value_set(&e->back, param_int_or("back", -1));
    value_set(&e->waist, param_int_or("waist", -1));
    value_set(&e->pet, param_int_or("pet", -1));
    value_set(&e->wrist, param_int_or("wrist", -1));
    value_set(&e->melee, param_int_or("melee", -1));
    value_set(&e->skin_col, param_int_or("skinCol", -1));
    value_set(&e->dye, param_int_or("dye", -1));
    value_set(&e->secondary, param_int_or("secIndex", 2));
    value_set(&e->class_index, param_int_or("class", 0));
    value_set(&e->attach, param_int_or("attach", -1));
    value_set(&e->season, param_int_or("season", g_viewer_current_season));
    e->lighting.is_string = 1;
    snprintf(e->lighting.str, sizeof(e->lighting.str), "In-game");

    e->show_player = !viewer_param_has("hidePlayer");
    e->show_body = !viewer_param_has("hideBody");
    e->show_back = !viewer_param_has("hideBack");
    e->show_waist = !viewer_param_has("hideWaist");
    e->show_face = !viewer_param_has("hideFace");
    e->show_hat = !viewer_param_has("hideHat");
    e->show_head = !viewer_param_has("hideHead");
    e->show_pet = !viewer_param_has("hidePet");
    e->show_wrist = !viewer_param_has("hideWrist");
    e->show_arms = !viewer_param_has("hideArms");
    e->show_legs = !viewer_param_has("hideLegs");
    e->show_wep = !viewer_param_has("hideWep");
    e->gradient = 1;
    e->animate = viewer_param_has("anim");
    e->crouch = viewer_param_has("crouch");
    const char *freeze = getenv("KRUNKNATIVE_VIEWER_FREEZE");
    g_viewer.freeze = freeze && *freeze && strcmp(freeze, "0") != 0;

    // T = params.has("swap") && params.get("swap"); compared loosely with -1 and 1
    const char *swap = viewer_param_get("swap");
    const double swap_value = swap && *swap ? strtod(swap, NULL) : 0.0;
    g_viewer.swap = swap_value == -1.0 ? -1 : swap_value == 1.0 ? 1 : 0;

    int ok;
    g_viewer.cosmetic = js_parse_int(viewer_param_get("cosmetic"), &ok);
    g_viewer.cosmetic_set = ok;
    if (!ok) g_viewer.cosmetic = -1;
    g_viewer.showcase = js_parse_int(viewer_param_get("showcase"), &ok);
    g_viewer.showcase_set = ok;
    if (!ok) g_viewer.showcase = -1;
    // the shown copy's paint (v10.0.3 deob:123160-123173: "&paint=" + colour without its "#", "&paintShader=" + n)
    g_viewer.paint = WORN_SKIN_PAINT_NONE;
    const char *paint = viewer_param_get("paint");
    if (paint && *paint) {
        char *end;
        const long rgb = strtol(paint[0] == '#' ? paint + 1 : paint, &end, 16);
        if (*end == '\0' && rgb >= 0 && rgb <= 0xFFFFFF) g_viewer.paint.color = (int) rgb;
    }
    const int shader = js_parse_int(viewer_param_get("paintShader"), &ok);
    if (ok && g_viewer.paint.color < 0) g_viewer.paint.shader = shader;
    const char *dev = viewer_param_get("dev");
    g_viewer.dev = dev != NULL;
    g_viewer.hi_quality = g_viewer.cosmetic_set || (dev && *dev); // isNumber(C) || params.get("dev")
    g_viewer.auto_render = viewer_param_has("auto");
    g_viewer.no_tex = viewer_param_has("noTex");
    g_viewer.no_bg = viewer_param_has("nobg");
    g_viewer.nogui = viewer_param_has("nogui");
    g_ui.keep_skins = viewer_param_has("nosup");
    const int size = param_int_or("size", 256); // params.get("size") || 256
    g_viewer.export_size = size > 0 ? size : 256;
}

// ---------------------------------------------------------------- page hooks

static float css_scale(void) {
    int fb_w = 0, fb_h = 0;
    if (g_viewer.client && g_viewer.client->window) glfwGetFramebufferSize(g_viewer.client->window, &fb_w, &fb_h);
    float vw = 0.0f, vh = 0.0f, dpr = 1.0f;
    html_dom_viewport(&vw, &vh, &dpr);
    return vh > 0.0f && fb_h > 0 ? (float) fb_h / vh : 1.0f;
}

static void viewer_enter(Client *client, const char *query) {
    memset(&g_viewer, 0, sizeof(g_viewer));
    memset(&g_ui, 0, sizeof(g_ui));
    g_viewer.client = client;
    params_parse(query);
    state_from_params();
    vconfig_reset();
    KN_LOG_VERBOSE2("[viewer] enter query=\"%s\"\n", query ? query : "");

    static HtmlHandlerEntry handlers[VIEWER_MAX_HANDLERS];
    int count = 0;
    vg_register_handlers(handlers, &count);
    viewer_dialog_register_handlers(handlers, &count);
    html_ui_set_handlers(handlers, (size_t) count, NULL, NULL);

    if (!vscene_init()) return;
    viewer_import_init(client);
    g_ui.entered = 1;

    vscene_update_lights(); // U()
    if (g_viewer.showcase_set) {
        vscene_build_showcase(g_viewer.showcase);
    } else if (g_viewer.cosmetic_set) {
        H();
        cosmetic_show();
    } else if (g_viewer.nogui) {
        Z();
        vscene_camera_weapon();
    } else {
        H();
    }
    if (g_viewer.cosmetic_set && g_viewer.auto_render) fn_render_wep();

    // KRUNKNATIVE_VIEWER_GUI="<Name or Folder/Name>[=<value>];...": panel actions in order, as
    // picked / typed / clicked (tests: tools/kref/viewer-scenarios.txt)
    const char *actions = getenv("KRUNKNATIVE_VIEWER_GUI");
    if (actions && *actions) {
        const size_t n = strlen(actions);
        char *list = malloc(n + 1);
        if (list) memcpy(list, actions, n + 1);
        for (char *item = list; item && *item;) {
            char *end = strchr(item, ';');
            if (end) *end = 0;
            char *eq = strchr(item, '=');
            if (eq) *eq = 0;
            if (*item && !vg_test_action(item, eq ? eq + 1 : NULL)) fprintf(stderr, "[viewer] gui action: no controller \"%s\"\n", item);
            item = end ? end + 1 : NULL;
        }
        free(list);
    }

    // KRUNKNATIVE_VIEWER_IMPORT=<file>[,<target>]: import a file as if dropped / loaded (tests)
    const char *import = getenv("KRUNKNATIVE_VIEWER_IMPORT");
    if (import && *import) {
        char path[1024];
        snprintf(path, sizeof(path), "%s", import);
        char *comma = strrchr(path, ',');
        const int target = comma ? atoi(comma + 1) : 0;
        if (comma) *comma = 0;
        viewer_import_path(path, target);
    }
}

static void viewer_leave(Client *client) {
    (void) client;
    if (g_ui.render_timer) html_timer_cancel(g_ui.render_timer);
    viewer_import_fini();
    vg_fini();
    vscene_fini();
    params_clear();
    memset(&g_ui, 0, sizeof(g_ui));
}

static void viewer_tick(Client *client, const float now, const float delta) {
    (void) now;
    if (!g_ui.entered) return;
    viewer_import_tick();

    int fb_w = 0, fb_h = 0;
    glfwGetFramebufferSize(client->window, &fb_w, &fb_h);
    const float dt_ms = fminf(delta, 0.1f) * 1000.0f;
    vscene_tick(dt_ms);
    vscene_render(fb_w, fb_h);
}

static int dialog_open(void) {
    const HtmlElem dialog = html_dom_by_id("knViewerDialog");
    return dialog && !html_dom_has_class(dialog, "hidden");
}

static int viewer_mouse_button(Client *client, const int button, const int action, const int mods, const double x, const double y) {
    (void) client;
    if (!g_ui.entered) return 0;
    const float s = css_scale();
    const float cx = (float) x / s, cy = (float) y / s;

    if (action == GLFW_PRESS) {
        if (dialog_open() || vg_contains(cx, cy) || g_viewer.cosmetic_set) return 0;
        vscene_orbit_begin(button, mods, cx, cy);
        return vscene_orbit_active();
    }
    if (action == GLFW_RELEASE && vg_dragging()) vg_drag_end(); // dat.gui's window mouseup
    if (action == GLFW_RELEASE && vscene_orbit_active()) {
        vscene_orbit_end();
        return 1;
    }
    return 0;
}

static int viewer_cursor(Client *client, const double x, const double y) {
    (void) client;
    if (!g_ui.entered) return 0;
    const float s = css_scale();
    if (vg_dragging()) { // dat.gui's window mousemove while a box / colour field is dragged
        vg_drag_move((float) x / s, (float) y / s);
        return 0;
    }
    if (!vscene_orbit_active()) return 0;
    vscene_orbit_move((float) x / s, (float) y / s);
    return 1;
}

static int viewer_scroll(Client *client, const double dx, const double dy) {
    (void) dx;
    if (!g_ui.entered || g_viewer.cosmetic_set || dialog_open()) return 0;
    double mx = 0.0, my = 0.0;
    glfwGetCursorPos(client->window, &mx, &my);
    int ww = 0, wh = 0, fw = 0, fh = 0;
    glfwGetWindowSize(client->window, &ww, &wh);
    glfwGetFramebufferSize(client->window, &fw, &fh);
    const float s = css_scale();
    const float cx = (float) mx * (ww > 0 ? (float) fw / (float) ww : 1.0f) / s;
    const float cy = (float) my * (wh > 0 ? (float) fh / (float) wh : 1.0f) / s;
    if (vg_contains(cx, cy)) return 0; // the panel scrolls
    vscene_orbit_wheel((float) -dy);     // GLFW +y = wheel up = deltaY < 0
    return 1;
}

static int viewer_key(Client *client, const int key, const int scancode, const int action, const int mods) {
    (void) client;
    (void) scancode;
    (void) mods;
    if (!g_ui.entered || action == GLFW_RELEASE || dialog_open()) return 0;
    if (ui_text_input_focused()) return 0; // dat.GUI / OrbitControls skip keys typed into a field
    if (key == GLFW_KEY_H && action == GLFW_PRESS) vg_toggle_hide(); // dat.GUI.toggleHide (keyCode 72)
    if (!g_viewer.cosmetic_set) {
        switch (key) { // OrbitControls keys on window
            case GLFW_KEY_UP: vscene_orbit_key(38); break;
            case GLFW_KEY_DOWN: vscene_orbit_key(40); break;
            case GLFW_KEY_LEFT: vscene_orbit_key(37); break;
            case GLFW_KEY_RIGHT: vscene_orbit_key(39); break;
            default: break;
        }
    }
    return 0;
}

static void viewer_resize(Client *client, const int fb_w, const int fb_h) {
    (void) client;
    (void) fb_w;
    (void) fb_h;
    vg_resize(); // window resize: dat.gui onResizeDebounced; the camera aspect follows the viewport
}

const AppPage *viewer_app_page(void) {
    static const AppPage page = {
        .file = "viewer.html",
        .enter = viewer_enter,
        .leave = viewer_leave,
        .tick = viewer_tick,
        .mouse_button = viewer_mouse_button,
        .cursor = viewer_cursor,
        .scroll = viewer_scroll,
        .key = viewer_key,
        .resize = viewer_resize,
    };
    return &page;
}

// ---------------------------------------------------------------- the store item view (viewer_embed.h)

static struct {
    int active;
    int index;
} g_embed;

void viewer_embed_fini(void) {
    if (!g_embed.active) return;
    vscene_fini();
    params_clear();
    memset(&g_viewer, 0, sizeof(g_viewer));
    memset(&g_embed, 0, sizeof(g_embed));
}

// the iframe of #itemViewPop that shows viewer.html?...showcase=<i>: its store index, -1 = none
static HtmlElem embed_frame(int *index) {
    *index = -1;
    const HtmlElem pop = html_dom_by_id("itemViewPop");
    char display[16] = "";
    if (!pop) return 0;
    html_dom_get_style(pop, "display", display, sizeof(display));
    if (!strcmp(display, "none") || !display[0]) return 0;
    const HtmlElem frame = html_dom_query(pop, "iframe");
    char src[160];
    if (!frame || html_dom_get_attr(frame, "src", src, sizeof(src)) < 0 || !strstr(src, "viewer.html")) return 0;
    const char *query = strchr(src, '?');
    const char *showcase = query ? strstr(query, "showcase=") : NULL;
    if (!showcase) return 0;
    *index = atoi(showcase + 9);
    return frame;
}

void viewer_embed_frame(Client *client, const float delta) {
#ifdef __EMSCRIPTEN__
    (void) client;
    (void) delta;
    return; // the browser shows the iframe's own viewer page
#else
    if (g_ui.entered || !client || !client->window) return; // the viewer page owns the scene
    int index = -1;
    const HtmlElem frame = embed_frame(&index);
    HtmlRect rect = {0};
    if (!frame || !html_dom_get_rect(frame, &rect) || rect.w < 1.0f || rect.h < 1.0f) {
        viewer_embed_fini();
        return;
    }
    if (g_embed.active && g_embed.index != index) viewer_embed_fini();
    if (!g_embed.active) {
        char query[64];
        snprintf(query, sizeof(query), "nobg&showcase=%d", index);
        memset(&g_viewer, 0, sizeof(g_viewer));
        g_viewer.client = client;
        params_parse(query);
        state_from_params();
        g_embed.active = 1;
        g_embed.index = index;
        if (!vscene_init()) {
            viewer_embed_fini();
            return;
        }
        vscene_update_lights();
        vscene_build_showcase(index);
        KN_LOG_VERBOSE2("[viewer] item view showcase %d\n", index);
    }

    int fb_w = 0, fb_h = 0;
    glfwGetFramebufferSize(client->window, &fb_w, &fb_h);
    float vw = 0.0f, vh = 0.0f, dpr = 1.0f;
    html_dom_viewport(&vw, &vh, &dpr);
    const float scale = vh > 0.0f ? (float) fb_h / vh : 1.0f;
    const int x = (int) lroundf(rect.x * scale), w = (int) lroundf(rect.w * scale), h = (int) lroundf(rect.h * scale);
    const int y = fb_h - (int) lroundf(rect.y * scale) - h;
    vscene_tick(fminf(delta, 0.1f) * 1000.0f);
    vscene_render_rect(x, y, w, h, fb_w, fb_h);
#endif
}
