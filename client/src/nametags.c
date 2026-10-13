// World-anchored overlay items of Krunker v3.1.1's #game-overlay canvas: player nametags,
// and bomb site letters (v7's flag pings and Hardpoint / Domination markers are
// client/src/ui/hud_objectives.c). See client/include/nametags.h.
//
// Reference: Krunker_v3.1.1/src/libs/game-overlay.js overlay.render (the AI.ais, players,
// flags, objectives and sites blocks), src/index.js gameLoop (the nameVisRate pass that
// sets player.isSpectating, i.e. "nametag visible") and g1() (the killer's tag),
// src/data/game.js canSee, src/data/colors.js, src/config.js (nameOffset*, nameVisRate).
//
// Not ported (no data in KrunkNative): the K.P.D red name (no reports), the site mode's bomb pickup "warning" icon
// (map objects keep no pickup id), Game.mode.teamSee (it compares team names with team
// numbers in v3.1.1, so it never applies) and the terrain term of canSee (no terrain).
// Level box, clan tag and premium / featured badges come from the account data a
// brain-linked server sends (NET_PACKET_PLAYER_INFO, player_accounts.h); guests and
// standalone servers have none. They also show
// with KRUNKNATIVE_NAMETAG_DEMO=1 (test values on real players) or =2 (fixed demo tags;
// ?nametagDemo=1/2 on the web), both for layout checks against the real game only.
#include <client.h>
#include <ai.h>
#include <html_dom.h>
#include <html_ui.h>
#include <hud_modes.h>
#include <menu_view.h>
#include <nametags.h>
#include <player_accounts.h>
#include <render_settings.h>
#include <spectate.h>
#include <ui_logic.h>
#include <voice_chat.h>
#include <badges.h> // WP-E part 2c: the custom badge image

#include <math.h>
#include <stdio.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif
#include <stdlib.h>
#include <string.h>

#define NT_MAX_TAGS 64
#define NT_MAX_SITES 8
#define NT_VIS_RATE_S 0.2        // config.nameVisRate (ms / 1000)
#define NT_WATCHDOG_MS 250.0     // hide everything when nametags_tick stops running this long
#define NT_NAME_OFFSET 0.6f      // config.nameOffset
#define NT_NAME_OFFSET_HAT 0.8f  // config.nameOffsetHat
#define NT_FADE_DIST 600.0f      // max(0.3, 1 - distance / 600)
#define NT_MIN_SCALE 0.3f
#define NT_MARKER_UP 7.0f        // flags / objective / sites: pos.y + 7
#define NT_BAR_W 130.0f          // fillRect(-65, -aW, 130, aW)
#define NT_GAMEFONT_ASCENT 1.125f // font2.ttf hhea ascender / em: canvas baseline -> CSS box top (line-height 1.375 = A + D)
#define NT_ICON_FONT "26px"      // premiumFont = "26px 'Material Icons'" (ascent 1 em, line-height 1)
#define NT_PARTNER_COLOR "#993299"    // COLORS.partner
#define NT_PREMIUM_COLOR "#FBC02D"    // COLORS.verified.clan
#define NT_FEATURED_COLOR "#40C4FF"   // COLORS.verified.player
#define NT_SITE_COLOR "#FBC02D"
#define NT_STROKE "#2e2e2e"
#define NT_DEMO_UID -100         // KRUNKNATIVE_NAMETAG_DEMO=2 tags: uid NT_DEMO_UID - i
#define NT_DEMO_TAGS 5

// b2 partner "verified", b1 premiumT "beenhere", b0 featured "check_circle", in drawing order
enum { NT_BADGE_PARTNER, NT_BADGE_PREMIUM, NT_BADGE_FEATURED, NT_BADGES };

// What a nametag shows about a player (Krunker player fields; account ones are empty here).
typedef struct {
    char name[64];       // hideNames (streamer mode) ? "Player " + sid : premiumT ? alias : name
    char clan[16];       // "" none
    int level;           // 0: no level box
    int badge[NT_BADGES];
    int custom_badge;    // WP-E part 2c: badgeIndex (badges.js), -1 none
    int demo_voice;      // KRUNKNATIVE_NAMETAG_DEMO=2: show the speaker icon without a voice queue
} TagInfo;

// Last written value of one inline style property (writes only on change).
typedef struct { char value[48]; } StyleCache;

typedef struct {
    HtmlElem root, bar, chase, hp, num, lvl, lvl_text, row, name, clan, badge[NT_BADGES], voice;
    HtmlElem custom;          // WP-E part 2c: the custom badge image
    int custom_index;         // its badge, -1 hidden
    char custom_ml[16];       // its margin-left
    int uid;                 // player sid, -1 = free slot, <= NT_DEMO_UID demo tags
    int visible;             // player.isSpectating (nameVisRate result)
    int shown;               // last written root display
    int seen;                // still in the player list this frame
    int style;               // nametagStyle the geometry was written for (-1 none)
    int has_level;
    char text_name[64], text_clan[24], text_level[16], text_num[16], text_voice[16];
    StyleCache left, top, transform, hp_w, hp_color, chase_w, chase_disp, num_disp, voice_disp, opacity, clan_style, badge_ml[NT_BADGES];
} Tag;

typedef struct {
    HtmlElem el;
    int shown;
    StyleCache left, top, size, margin, transform, src;
} Marker;

// One AI's tag (overlay.render's Game.AI.ais block): name + health bar, Game.ai index = slot.
typedef struct {
    HtmlElem root, bar, hp, name;
    int shown;
    char text_name[GAME_AI_NAME_SIZE];
    StyleCache left, top, transform, hp_w, hp_color, geometry;
} AITag;

static struct {
    HtmlElem layer;          // #knOverlayWorld (0 until created)
    unsigned serial;         // html_ui_page_serial() the layer belongs to
    int layer_shown;
    StyleCache layer_w, layer_h, layer_transform;
    Tag tags[NT_MAX_TAGS];
    Marker sites[NT_MAX_SITES];
    AITag ai_tags[GAME_AI_MAX];
    double next_vis;         // next nameVisRate pass (glfwGetTime seconds)
    double last_tick_ms;     // watchdog: last nametags_tick
    int watchdog;            // timer id armed
    int demo;                // KRUNKNATIVE_NAMETAG_DEMO: -1 unread, 0 off, 1 account fields, 2 fixed tags
} g_nt = {.demo = -1};

/* ------------------------------------------------------------------ small helpers */

static void nt_style(const HtmlElem el, const char *prop, const char *value, StyleCache *cache) {
    if (!el) return;
    if (cache) {
        if (!strcmp(cache->value, value) && cache->value[0]) return;
        snprintf(cache->value, sizeof(cache->value), "%s", value);
    }
    html_dom_set_style(el, prop, value);
}

static void nt_text(const HtmlElem el, const char *text, char *cache, const size_t n) {
    if (!el || !strcmp(cache, text)) return;
    snprintf(cache, n, "%s", text);
    html_dom_set_text(el, text);
}

static void nt_display(const HtmlElem el, const int on, int *shown) {
    if (!el || *shown == on) return;
    *shown = on;
    html_dom_set_display(el, on ? "block" : "none");
}

static void nt_px(char *out, const size_t n, const double v) {
    snprintf(out, n, "%.2fpx", v);
}

// Krunker `el.style.display != "none"` (an unset inline display counts as shown).
static int nt_dom_shown(const char *id) {
    const HtmlElem el = html_dom_by_id(id);
    if (!el) return 0;
    char value[32];
    if (html_dom_get_style(el, "display", value, sizeof(value)) < 0) return 1;
    return strcmp(value, "none") != 0;
}

// game-overlay aa(): the colour with its last character swapped for a random A..F
// (v3.1.1's anti-colour-bot jitter on the health bar fill).
static void nt_jitter_color(const char *color, char *out, const size_t n) {
    snprintf(out, n, "%s", color);
    const size_t len = strlen(out);
    if (len > 1) out[len - 1] = (char) ('A' + rand() % 6);
}

// COLORS.getClanCol(clan, null) (v7.1.4 src/data/colors.js): only the DEV / VIP / CMD tags keep
// a colour; v7 removed v3.1.1's clan war rank colours (clanR: rainbow, red, gold, pink, blue)
// and the challenge level colour of the level box (getChallCol) (WP-E part 2c).
static const char *nt_clan_color(const char *clan) {
    if (!strcmp(clan, "DEV")) return "#40C4FF";
    if (!strcmp(clan, "VIP")) return "#11D445";
    if (!strcmp(clan, "CMD")) return "#993299";
    return NULL;
}

/* ------------------------------------------------------------------ projection */

// pos.project(RENDER.camera) after RENDER.frustum.containsPoint(pos): viewport fractions
// (0..1, y down). Row-major matrices like vfx.c / hud_ui.c.
static int nt_project(const Camera *camera, const vec3 p, double *fx, double *fy) {
    const float in[4] = {p.x, p.y, p.z, 1.0f};
    float view[4], clip[4];
    for (int r = 0; r < 4; r++) {
        view[r] = camera->world_inverse_matrix[r * 4] * in[0] + camera->world_inverse_matrix[r * 4 + 1] * in[1] +
                  camera->world_inverse_matrix[r * 4 + 2] * in[2] + camera->world_inverse_matrix[r * 4 + 3] * in[3];
    }
    for (int r = 0; r < 4; r++) {
        clip[r] = camera->projection_matrix[r * 4] * view[0] + camera->projection_matrix[r * 4 + 1] * view[1] +
                  camera->projection_matrix[r * 4 + 2] * view[2] + camera->projection_matrix[r * 4 + 3] * view[3];
    }
    if (clip[3] <= 0.0f) return 0;
    const double nx = clip[0] / clip[3], ny = clip[1] / clip[3], nz = clip[2] / clip[3];
    if (nx < -1.0 || nx > 1.0 || ny < -1.0 || ny > 1.0 || nz < -1.0 || nz > 1.0) return 0;
    *fx = (nx + 1.0) / 2.0;
    *fy = 1.0 - (ny + 1.0) / 2.0;
    return 1;
}

// aO = max(0.3, 1 - getD3D(camera, point) / 600)
static float nt_distance_scale(const vec3 camera, const vec3 p) {
    const float dx = camera.x - p.x, dy = camera.y - p.y, dz = camera.z - p.z;
    const float s = 1.0f - sqrtf(dx * dx + dy * dy + dz * dz) / NT_FADE_DIST;
    return s > NT_MIN_SCALE ? s : NT_MIN_SCALE;
}

// UTILS.pointInBox3D(point, {x, y, z, width, height, length}) with half extents.
static int nt_point_in_box(const vec3 p, const vec3 center, const vec3 half) {
    return p.x >= center.x - half.x && p.x <= center.x + half.x && p.y >= center.y - half.y &&
           p.y <= center.y + half.y && p.z >= center.z - half.z && p.z <= center.z + half.z;
}

/* ------------------------------------------------------------------ line of sight */

// JS Math.min / Math.max: NaN propagates (fminf/fmaxf would drop it).
static double js_min(const double a, const double b) { return a != a || b != b ? NAN : a < b ? a : b; }
static double js_max(const double a, const double b) { return a != a || b != b ? NAN : a > b ? a : b; }

// Map objects Game.canSee tests: active, not noShoot, not transparent (o != 1). KrunkNative
// objects carry no noShoot for the special prefabs, so this is the hitscan filter
// (player.c combat_object_shootable) plus the transparent material bit.
static int nt_blocks_sight(const Object *object) {
    if (!object->active || object->no_shoot || object->collision_type == COLLISION_TYPE_NONE) return 0;
    if (object->kill || object->score_zone || object->team_zone || object->teleporter || object->checkpoint ||
        object->pickup || object->flag || object->premium || object->verified ||
        object->objective || object->bomb_site || object->ladder || object->ramp) return 0;
    switch (object->prefab) {
        case PREFAB_TEAM_ZONE:
        case PREFAB_SPAWN_POINT:
        case PREFAB_CAMERA_POSITION:
        case PREFAB_SPECTATE_CAM:
            return 0;
        default:
            break;
    }
    const Mesh *mesh = object->mesh;
    if (mesh && mesh->material && mesh->material->transparent) return 0;
    return 1;
}

// Game.canSee(origin, target) == null: the ray starts at the origin's eye (y + height -
// cameraHeight) and runs along origin-feet -> target, scaled to that length; an object
// whose box it enters before the end (UTILS.lineInRect result in (-inf, 1), non-zero)
// blocks it. Object boxes: x/z centred, y from the base (KrunkNative), full size.
static int nt_can_see(const Game *game, const Player *origin, const vec3 target) {
    if (!game->map) return 1;
    const double ox = origin->position.x, oz = origin->position.z;
    const double oy = origin->position.y + origin->height - game_constants.camera_height;
    const double dx = target.x - origin->position.x, dy = target.y - origin->position.y, dz = target.z - origin->position.z;
    const double ix = 1.0 / dx, iy = 1.0 / dy, iz = 1.0 / dz;

    for (size_t i = 0; i < game->map->object_count; i++) {
        const Object *object = game->map->objects[i];
        if (!nt_blocks_sight(object)) continue;
        const double hx = object->scale.x * 0.5, hz = object->scale.z * 0.5;
        const double min_x = object->position.x - hx, max_x = object->position.x + hx;
        const double min_z = object->position.z - hz, max_z = object->position.z + hz;
        const double min_y = object->position.y, max_y = object->position.y + object->scale.y;

        const double a = (min_x - ox) * ix, b = (max_x - ox) * ix;
        const double c = (min_y - oy) * iy, d = (max_y - oy) * iy;
        const double e = (min_z - oz) * iz, f = (max_z - oz) * iz;
        const double near = js_max(js_max(js_min(a, b), js_min(c, d)), js_min(e, f));
        const double far = js_min(js_min(js_max(a, b), js_max(c, d)), js_max(e, f));
        // lineInRect: !(P < 0) && !(O > P) && O, then `ah && ah < 1`
        if (!(far < 0.0) && !(near > far) && near == near && near != 0.0 && near < 1.0) return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ DOM: layer */

static void nt_watchdog(void *user);

// The canvas sits before #uiBase with z-index 1; this layer takes its place in the
// stacking order (inserted right after it) and the canvas's ctx.scale(scale).
static HtmlElem nt_layer(void) {
    const unsigned serial = html_ui_page_serial();
    if (g_nt.layer && g_nt.serial == serial) return g_nt.layer;
    if (g_nt.layer && html_dom_by_id("knOverlayWorld") == g_nt.layer) {
        g_nt.serial = serial; // back on the game page: the document (and our layer) survived
        return g_nt.layer;
    }

    // A fresh document: every element handle is stale.
    memset(g_nt.tags, 0, sizeof(g_nt.tags));
    memset(g_nt.sites, 0, sizeof(g_nt.sites));
    memset(g_nt.ai_tags, 0, sizeof(g_nt.ai_tags));
    for (int i = 0; i < NT_MAX_TAGS; i++) g_nt.tags[i].uid = -1;
    memset(&g_nt.layer_w, 0, sizeof(g_nt.layer_w));
    memset(&g_nt.layer_h, 0, sizeof(g_nt.layer_h));
    memset(&g_nt.layer_transform, 0, sizeof(g_nt.layer_transform));
    g_nt.layer_shown = -1;
    g_nt.layer = 0;

    const HtmlElem canvas = html_dom_by_id("game-overlay");
    if (!canvas) return 0;
    html_dom_insert_html(canvas, HTML_INSERT_AFTER_END,
        "<div id='knOverlayWorld' style='position:absolute;left:0;top:0;width:100%;height:100%;"
        "pointer-events:none;z-index:1;overflow:hidden;transform-origin:0 0;display:none'></div>");
    g_nt.layer = html_dom_by_id("knOverlayWorld");
    g_nt.serial = serial;
    return g_nt.layer;
}

// scaleBase's `scale` (1700 x 900 * uiCustomScale fit; stays 1 with uiScaling off).
static double nt_ui_scale(const float vw, const float vh) {
    if (ui_setting_is_false("uiScaling")) return 1.0;
    double scale_ui;
    if (!ui_setting_number("scaleUI", &scale_ui) || scale_ui != scale_ui) scale_ui = 0.8;
    const double custom = 1.0 - scale_ui + 1.0;
    const double sx = vw / (custom * 1700.0), sy = vh / (custom * 900.0);
    return sy < sx ? sy : sx;
}

/* ------------------------------------------------------------------ DOM: one nametag */

static HtmlElem nt_child(const HtmlElem root, const char *cls) {
    char selector[40];
    snprintf(selector, sizeof(selector), ".%s", cls);
    return html_dom_query(root, selector);
}

// One player's tag in canvas units around its anchor (the projected head point, scaled by
// aO). Static geometry here; per-style tops and per-frame values in nt_tag_update.
static int nt_tag_create(Tag *tag, const int index) {
    char id[32];
    snprintf(id, sizeof(id), "knNameTag_%d", index);
    tag->root = html_dom_create("div", id);
    if (!tag->root) return 0;
    html_dom_set_style(tag->root, "position", "absolute");
    html_dom_set_style(tag->root, "width", "0");
    html_dom_set_style(tag->root, "height", "0");
    html_dom_set_style(tag->root, "transform-origin", "0 0");
    html_dom_set_display(tag->root, "none");
    html_dom_set_html(tag->root,
        "<div class='knNtBar' style='position:absolute;left:-65px;width:130px;background:rgba(0,0,0,0.4)'></div>"
        "<div class='knNtChase' style='position:absolute;left:-65px;background:#FFFFFF;display:none'></div>"
        "<div class='knNtHp' style='position:absolute;left:-65px'></div>"
        "<div class='knNtNum' style='position:absolute;left:-63px;top:-13.125px;font-family:gamefont;font-size:9px;"
            "line-height:1.375;color:white;white-space:nowrap;display:none'></div>"
        "<div class='knNtLvl' style='position:absolute;right:65px;height:23px;padding:7px 9px 0 9px;"
            "background:rgba(0,0,0,0.65);font-family:gamefont;font-size:16px;line-height:1.375;white-space:nowrap;display:none'>"
            "<span class='knNtLvlT' style='color:white'></span></div>"
        "<div class='knNtRow' style='position:absolute;font-family:gamefont;font-size:20px;line-height:1.375;"
            "white-space:nowrap;color:white'>"
            "<span class='knNtName' style='color:white'></span><span class='knNtClan' style='display:none'></span>"
            "<span class='knNtB0 material-icons' style='display:none;line-height:0;font-size:" NT_ICON_FONT ";position:relative;top:1px;color:" NT_PARTNER_COLOR "'>verified</span>"
            "<span class='knNtB1 material-icons' style='display:none;line-height:0;font-size:" NT_ICON_FONT ";position:relative;top:1px;color:" NT_PREMIUM_COLOR "'>beenhere</span>"
            "<span class='knNtB2 material-icons' style='display:none;line-height:0;font-size:" NT_ICON_FONT ";color:" NT_FEATURED_COLOR "'>check_circle</span>"
            "<img class='knNtBdg' style='display:none;width:23px;height:23px;vertical-align:top;position:relative;top:-1.5px'>"
        "</div>"
        "<span class='knNtVoice material-icons' style='position:absolute;left:-25px;font-size:55px;line-height:1;"
            "color:#FFF;display:none'>volume_down</span>");
    tag->bar = nt_child(tag->root, "knNtBar");
    tag->chase = nt_child(tag->root, "knNtChase");
    tag->hp = nt_child(tag->root, "knNtHp");
    tag->num = nt_child(tag->root, "knNtNum");
    tag->lvl = nt_child(tag->root, "knNtLvl");
    tag->lvl_text = nt_child(tag->root, "knNtLvlT");
    tag->row = nt_child(tag->root, "knNtRow");
    tag->name = nt_child(tag->root, "knNtName");
    tag->clan = nt_child(tag->root, "knNtClan");
    tag->badge[0] = nt_child(tag->root, "knNtB0");
    tag->badge[1] = nt_child(tag->root, "knNtB1");
    tag->badge[2] = nt_child(tag->root, "knNtB2");
    tag->custom = nt_child(tag->root, "knNtBdg");
    tag->custom_index = -1;
    tag->voice = nt_child(tag->root, "knNtVoice");
    html_dom_append(g_nt.layer, tag->root);
    tag->shown = 0;
    tag->style = -1;
    tag->has_level = -1;
    return 1;
}

// nametagStyle geometry: aW = style 1 ? 6 : 16; the bar (styles 0 and 3) spans -aW..0;
// the name row (styles 0..2) has its baseline at T = -aW - 10, the level box (styles 0
// and 2) spans T - 28..T + 2 with its text baseline at T - 3; the row shifts 5 px right
// of -65 when there is a level box.
static void nt_tag_geometry(Tag *tag, const int style, const int has_level) {
    if (tag->style == style && tag->has_level == has_level) return;
    tag->style = style;
    tag->has_level = has_level;
    const double aw = style == 1 ? 6.0 : 16.0;
    const double t = -aw - 10.0;
    char v[32];
    const int bar = style == 0 || style == 3;
    nt_px(v, sizeof(v), -aw);
    html_dom_set_style(tag->bar, "top", v);
    html_dom_set_style(tag->chase, "top", v);
    html_dom_set_style(tag->hp, "top", v);
    nt_px(v, sizeof(v), aw);
    html_dom_set_style(tag->bar, "height", v);
    html_dom_set_style(tag->chase, "height", v);
    html_dom_set_style(tag->hp, "height", v);
    html_dom_set_display(tag->bar, bar ? "block" : "none");
    html_dom_set_display(tag->hp, bar ? "block" : "none");

    nt_px(v, sizeof(v), t - 28.0);
    html_dom_set_style(tag->lvl, "top", v);
    html_dom_set_display(tag->lvl, has_level && style < 3 && style != 1 ? "block" : "none");
    nt_px(v, sizeof(v), t - NT_GAMEFONT_ASCENT * 20.0);
    html_dom_set_style(tag->row, "top", v);
    nt_px(v, sizeof(v), t - 25.0 - 55.0); // fillText(icon, -25, -25) in 55px Material Icons (ascent 1 em)
    html_dom_set_style(tag->voice, "top", v);
    nt_px(v, sizeof(v), (has_level && style != 1 ? 5.0 : 0.0) - 65.0);
    html_dom_set_style(tag->row, "left", v);
    html_dom_set_display(tag->row, style < 3 ? "block" : "none");
}

// The name row: name, clan (+5 px), then the partner / premium / featured badges with
// v3.1.1's offsets (b8 = name + 5, b9 = clan + 3, +b7 + 1, +b6 + 1; b0's width is unused) and
// v7's custom badge image, 23 px, 3 px after the featured icon's place (game-overlay.js:1101).
static void nt_tag_row(Tag *tag, const TagInfo *info, const float opacity) {
    nt_text(tag->name, info->name, tag->text_name, sizeof(tag->text_name));

    const int any_badge = info->badge[0] || info->badge[1] || info->badge[2] || info->custom_badge >= 0;
    const int extras = info->clan[0] || any_badge;
    double gap = extras ? 5.0 : 0.0;
    char v[64];

    if (info->clan[0]) {
        char clan[24];
        snprintf(clan, sizeof(clan), "[%s]", info->clan);
        nt_text(tag->clan, clan, tag->text_clan, sizeof(tag->text_clan));
        const char *col = nt_clan_color(info->clan);
        const char *fill = col ? col : "white";
        snprintf(v, sizeof(v), "inline;margin-left:%.0fpx;color:%s;opacity:%.3f", gap, fill, (col ? 1.0f : 0.4f) * opacity);
        if (strcmp(tag->clan_style.value, v)) {
            snprintf(tag->clan_style.value, sizeof(tag->clan_style.value), "%s", v);
            html_dom_set_display(tag->clan, "inline");
            snprintf(v, sizeof(v), "%.0fpx", gap);
            html_dom_set_style(tag->clan, "margin-left", v);
            html_dom_set_style(tag->clan, "color", fill);
            snprintf(v, sizeof(v), "%.3f", (col ? 1.0f : 0.4f) * opacity);
            html_dom_set_style(tag->clan, "opacity", v);
        }
        gap = any_badge ? 3.0 : 0.0;
    } else if (tag->clan_style.value[0]) {
        tag->clan_style.value[0] = 0;
        html_dom_set_display(tag->clan, "none");
    }

    for (int b = 0; b < NT_BADGES; b++) {
        if (info->badge[b]) {
            snprintf(v, sizeof(v), "%.0fpx", gap);
            if (strcmp(tag->badge_ml[b].value, v)) {
                snprintf(tag->badge_ml[b].value, sizeof(tag->badge_ml[b].value), "%s", v);
                html_dom_set_display(tag->badge[b], "inline");
                html_dom_set_style(tag->badge[b], "margin-left", v);
            }
            gap = 1.0;
        } else {
            if (tag->badge_ml[b].value[0]) {
                tag->badge_ml[b].value[0] = 0;
                html_dom_set_display(tag->badge[b], "none");
            }
            gap += 1.0;
        }
    }

    if (info->custom_badge >= 0) {
        if (tag->custom_index != info->custom_badge) {
            char tex[96], url[256];
            html_dom_asset_url(badge_texture(info->custom_badge, tex, sizeof(tex)), url, sizeof(url)); // Game.badges[i].url
            html_dom_set_attr(tag->custom, "src", url);
            tag->custom_index = info->custom_badge;
            tag->custom_ml[0] = 0;
        }
        snprintf(v, sizeof(v), "%.0fpx", gap + 2.0); // the featured icon's +1 replaced by v7's +3
        if (strcmp(tag->custom_ml, v)) {
            snprintf(tag->custom_ml, sizeof(tag->custom_ml), "%s", v);
            html_dom_set_style(tag->custom, "margin-left", v);
            html_dom_set_display(tag->custom, "inline-block");
        }
    } else if (tag->custom_index >= 0) {
        tag->custom_index = -1;
        html_dom_set_display(tag->custom, "none");
    }
}

static void nt_tag_hide(Tag *tag) {
    nt_display(tag->root, 0, &tag->shown);
}

// overlay.render's player block for one visible tag at viewport fraction (fx, fy).
static void nt_tag_update(Tag *tag, const Player *player, const TagInfo *info, const double fx, const double fy,
                          const float scale, const int teammate) {
    const ClientRenderSettings *rs = client_render_settings();
    const int style = rs->nametag_style < 0 || rs->nametag_style > 3 ? 0 : rs->nametag_style;
    const float opacity = rs->nametag_opacity;
    char v[64];

    nt_tag_geometry(tag, style, info->level > 0);

    snprintf(v, sizeof(v), "%.4f%%", fx * 100.0);
    nt_style(tag->root, "left", v, &tag->left);
    snprintf(v, sizeof(v), "%.4f%%", fy * 100.0);
    nt_style(tag->root, "top", v, &tag->top);
    snprintf(v, sizeof(v), "scale(%.4f)", scale);
    nt_style(tag->root, "transform", v, &tag->transform);
    snprintf(v, sizeof(v), "%.3f", opacity);
    if (strcmp(tag->opacity.value, v)) {
        // globalAlpha = nametagOpacity on every draw (per element, not as a group)
        snprintf(tag->opacity.value, sizeof(tag->opacity.value), "%s", v);
        html_dom_set_style(tag->bar, "opacity", v);
        html_dom_set_style(tag->chase, "opacity", v);
        html_dom_set_style(tag->hp, "opacity", v);
        html_dom_set_style(tag->num, "opacity", v);
        html_dom_set_style(tag->lvl, "opacity", v);
        html_dom_set_style(tag->name, "opacity", v);
        for (int b = 0; b < NT_BADGES; b++) html_dom_set_style(tag->badge[b], "opacity", v);
        html_dom_set_style(tag->custom, "opacity", v);
        html_dom_set_style(tag->voice, "opacity", v);
        tag->clan_style.value[0] = 0; // clan alpha is relative to it
    }

    if (style == 0 || style == 3) {
        const float max_health = player->max_health > 0 ? (float) player->max_health : 100.0f;
        const float ratio = player->health / max_health;
        snprintf(v, sizeof(v), "%.2fpx", (ratio > 0.0f ? ratio : 0.0f) * NT_BAR_W);
        nt_style(tag->hp, "width", v, &tag->hp_w);
        const int chase = rs->dynamic_hp && player->hp_chase > ratio;
        nt_style(tag->chase, "display", chase ? "block" : "none", &tag->chase_disp);
        if (chase) {
            snprintf(v, sizeof(v), "%.2fpx", player->hp_chase * NT_BAR_W);
            nt_style(tag->chase, "width", v, &tag->chase_w);
        }
        nt_jitter_color(teammate ? rs->health_col_team : rs->health_col_enemy, v, sizeof(v));
        nt_style(tag->hp, "background-color", v, &tag->hp_color);
        nt_style(tag->num, "display", rs->health_num ? "block" : "none", &tag->num_disp);
        if (rs->health_num) {
            snprintf(v, sizeof(v), "%g", roundf(player->health * 1000.0f) / 1000.0f);
            nt_text(tag->num, v, tag->text_num, sizeof(tag->text_num));
        }
    } else {
        nt_style(tag->chase, "display", "none", &tag->chase_disp);
        nt_style(tag->num, "display", "none", &tag->num_disp);
    }

    if (style < 3) {
        if (info->level > 0 && style != 1) {
            snprintf(v, sizeof(v), "%d", info->level);
            nt_text(tag->lvl_text, v, tag->text_level, sizeof(tag->text_level));
        }
        nt_tag_row(tag, info, opacity);
    }
    // voiceStreamQueues[aX] && .length: the speaker icon ai[ag] ("volume_down" / "volume_up",
    // flipped every 300 ms) above the name, keyed by the name the tag shows
    const int speaking = style < 3 && (info->demo_voice || voice_is_speaking(info->name));
    nt_style(tag->voice, "display", speaking ? "block" : "none", &tag->voice_disp);
    if (speaking) nt_text(tag->voice, voice_speaker_icon(), tag->text_voice, sizeof(tag->text_voice));
    nt_display(tag->root, 1, &tag->shown);
}

/* ------------------------------------------------------------------ players */

static void nt_demo_tags(Client *client);

static Tag *nt_tag_for(const int uid, const int create) {
    Tag *free_tag = NULL;
    for (int i = 0; i < NT_MAX_TAGS; i++) {
        if (g_nt.tags[i].uid == uid) return &g_nt.tags[i];
        if (!free_tag && g_nt.tags[i].uid == -1) free_tag = &g_nt.tags[i];
    }
    if (!create || !free_tag) return NULL;
    free_tag->uid = uid;
    free_tag->visible = 0;
    return free_tag;
}

static void nt_info(const Player *player, TagInfo *info) {
    memset(info, 0, sizeof(*info));
    info->custom_badge = -1;
    if (client_render_settings()->stream_mode) snprintf(info->name, sizeof(info->name), "Player %d", player->uid);
    else snprintf(info->name, sizeof(info->name), "%s", player->name);
    // account data from a brain-linked server (NET_PACKET_PLAYER_INFO): level box, clan, badges
    const NetPlayerInfoPacket *account = player_accounts_get(player->uid);
    if (account && account->account_id > 0) {
        info->level = account->level > 0 ? account->level : 1;
        if (!client_render_settings()->stream_mode && account->clan[0]) snprintf(info->clan, sizeof(info->clan), "%s", account->clan);
        info->badge[NT_BADGE_PREMIUM] = (account->flags & NET_PLAYER_INFO_PREMIUM) != 0;
        info->badge[NT_BADGE_FEATURED] = (account->flags & NET_PLAYER_INFO_VERIFIED) != 0;
    }
    if (player->game && player->game->config.comp) info->level = 0; // game-overlay: game.isComp ? 0 : level
    // v7 badgeIndex from the SPAWN (-1 unless the player shows its custom badge, server/src/net.c)
    if (badge_get(player->cosmetics_v7.badge)) info->custom_badge = player->cosmetics_v7.badge;
    if (g_nt.demo == 1) { // KRUNKNATIVE_NAMETAG_DEMO=1: account fields KrunkNative does not have
        info->level = 10 + player->uid * 7 % 90;
        if (!client_render_settings()->stream_mode) snprintf(info->clan, sizeof(info->clan), "%s", player->uid % 2 ? "KN" : "DEV");
        info->custom_badge = player->uid % 3 == 0 ? 11 : -1;
        info->badge[NT_BADGE_PARTNER] = player->uid % 2;
        info->badge[NT_BADGE_PREMIUM] = 1;
        info->badge[NT_BADGE_FEATURED] = player->uid % 3 != 1;
    }
}

// index.js gameLoop, every config.nameVisRate ms: player.isSpectating for every other
// active player with a model.
static void nt_visibility_pass(Client *client) {
    const Game *game = &client->game;
    const Player *me = client->me;
    const int spectating = spectate_active();
    if (client->offline) return;                        // !Game.singlePlayer (the offline sandbox game)
    if (!((me && me->active) || spectating)) return;    // dead: states (and the killer's tag) stay

    const ClientRenderSettings *rs = client_render_settings();
    const GameModeConfig *mode = game->mode ? &game->mode->config : NULL;
    const int local_team = me ? me->team : 0;
    const int target_uid = spectate_target_uid();

    for (size_t i = 0; i < game->player_count; i++) {
        const Player *player = game->players[i];
        if (!player || !player->active || !player->mesh || player == me) continue;
        Tag *tag = nt_tag_for(player->uid, 1);
        if (!tag) continue;

        int visible = !spectating || player->uid != target_uid;
        // latestData: the native server sends every active player each tick.
        // v7.1.4: spectators see every tag only with showSpectatorNameTags (specNames toggles it);
        // without, the rules are the followed player's (its team, its line of sight); with no one
        // followed the tags of the view are kept (Game.canSee(null) finds nothing in the way).
        const Player *viewer = spectating ? client_find_player(client, target_uid) : me;
        const int view_team = spectating ? (viewer ? viewer->team : 0) : local_team;
        const int forced = spectating && spectate_show_names();
        if (!forced && (spectating || me) &&
            (!view_team || view_team != player->team || (mode && mode->fake_names)) &&
            (game->config.hide_nametags || (mode && mode->hide_names) || (viewer && !nt_can_see(game, viewer, player->position)))) {
            visible = 0;
        }
        if ((rs->hide_names == 1 && (!(mode && mode->teams) || (local_team && local_team != player->team))) ||
            (rs->hide_names == 2 && local_team && local_team == player->team) ||
            rs->hide_names == 3 || (mode && mode->hide_names)) {
            visible = 0;
        }
        tag->visible = visible;
    }
}

static void nt_players(Client *client, const vec3 camera, const int hidden) {
    const Game *game = &client->game;
    const Player *me = client->me;
    const ClientRenderSettings *rs = client_render_settings();
    const GameModeConfig *mode = game->mode ? &game->mode->config : NULL;
    const int spectating = spectate_active();
    const int target_uid = spectate_target_uid();
    // aD = player && player.team ? player.team : window.spectating ? 1 : 0
    const int local_team = me && me->team ? me->team : spectating ? 1 : 0;

    // g1(): the killer's isSpectating = true while the death camera runs
    const int killer = menu_view_killer_uid();
    if (killer >= 0) {
        Tag *tag = nt_tag_for(killer, 1);
        if (tag) tag->visible = 1;
    }

    for (int i = 0; i < NT_MAX_TAGS; i++) g_nt.tags[i].seen = 0;

    for (size_t i = 0; i < game->player_count; i++) {
        const Player *player = game->players[i];
        if (!player) continue;
        Tag *tag = nt_tag_for(player->uid, 0);
        if (!tag) continue;
        tag->seen = 1;

        if (hidden || rs->nametag_opacity <= 0.0f || !player->active || player == me || player->is_you ||
            !player->mesh || (spectating && player->uid == target_uid) || !tag->visible) {
            nt_tag_hide(tag);
            continue;
        }

        // objInstances.position + playerHeight + nameOffset - crouchVal * crouchDst (+ nameOffsetHat)
        const PlayerMesh *mesh = player->mesh;
        vec3 anchor = mesh->anchor ? mesh->anchor->position : player->position;
        anchor.y += game_constants.player_height + NT_NAME_OFFSET - player->crouch_val * game_constants.crouch_distance;
        if (player->cosmetics.hat >= 0 || player->cosmetics_v7.head >= 0) anchor.y += NT_NAME_OFFSET_HAT; // hatIndex || headIndex (v7)

        double fx, fy;
        if (!nt_project(&client->camera, anchor, &fx, &fy)) {
            nt_tag_hide(tag);
            continue;
        }
        if (!tag->root && !nt_tag_create(tag, (int) (tag - g_nt.tags))) continue;

        TagInfo info;
        nt_info(player, &info);
        const int teammate = (player->team && local_team == player->team) || (mode && mode->fake_names && local_team != 2);
        nt_tag_update(tag, player, &info, fx, fy, nt_distance_scale(camera, anchor), teammate);
    }

    if (g_nt.demo == 2) {
        if (!hidden) nt_demo_tags(client);
        else for (int i = 0; i < NT_DEMO_TAGS; i++) nt_tag_hide(&g_nt.tags[NT_MAX_TAGS - 1 - i]);
    }

    for (int i = 0; i < NT_MAX_TAGS; i++) { // players that left
        Tag *tag = &g_nt.tags[i];
        if (tag->uid < 0 || tag->seen) continue; // free or demo
        nt_tag_hide(tag);
        tag->uid = -1;
        tag->visible = 0;
    }
}

/* ------------------------------------------------------------------ AI */

static int nt_ai_tag_create(AITag *tag, const int index) {
    char id[32], html[512];
    snprintf(id, sizeof(id), "knAiTag_%d", index);
    // drawn before the players: first in the layer
    snprintf(html, sizeof(html),
        "<div id='%s' style='position:absolute;width:0;height:0;transform-origin:0 0;display:none'>"
        "<div class='knAiBar' style='position:absolute;background:rgba(0,0,0,0.4)'></div>"
        "<div class='knAiHp' style='position:absolute'></div>"
        "<div class='knAiName' style='position:absolute;left:-500px;width:1000px;text-align:center;font-family:gamefont;"
            "line-height:1.375;white-space:nowrap;color:white'></div></div>", id);
    html_dom_insert_html(g_nt.layer, HTML_INSERT_AFTER_BEGIN, html);
    tag->root = html_dom_by_id(id);
    if (!tag->root) return 0;
    tag->bar = nt_child(tag->root, "knAiBar");
    tag->hp = nt_child(tag->root, "knAiHp");
    tag->name = nt_child(tag->root, "knAiName");
    tag->shown = 0;
    return 1;
}

// overlay.render, Game.AI.ais (under !aA): active, canBSeen and a visible mesh (v7.1.4: at any
// health). aG = 1 + scale / 30: name in aG * 20 px GameFont centred with its baseline at
// -aV - aG * 7, the bar aG * 120 * tagScale wide and (1 + (aG - 1) * 0.3) * 16 high above the anchor
// (mesh position + height * (assetID ? 4 : 2) + nameOffset + 1.5); a teammate AI's bar is healthColT.
// Not ported: the level box (ail) and the skull above level 60.
static void nt_ais(Client *client, const vec3 camera, const int hidden) {
    GameAIs *list = client->game.ai;
    const ClientRenderSettings *rs = client_render_settings();
    for (int i = 0; i < GAME_AI_MAX; i++) {
        AITag *tag = &g_nt.ai_tags[i];
        const GameAI *ai = list && i < list->count ? &list->ais[i] : NULL;
        if (hidden || !ai || !ai->active || ai->removed || !ai->can_be_seen || !ai->shown || ai->data.no_vis) {
            if (tag->root) nt_display(tag->root, 0, &tag->shown);
            continue;
        }
        const float top = ai->height * (ai->data.asset ? 4.0f : 2.0f);
        const vec3 anchor = {ai->position.x, ai->position.y + sinf(ai->bob_val) * ai->bob_d + top + NT_NAME_OFFSET + 1.5f, ai->position.z};
        double fx, fy;
        if (!nt_project(&client->camera, anchor, &fx, &fy)) {
            if (tag->root) nt_display(tag->root, 0, &tag->shown);
            continue;
        }
        if (!tag->root && !nt_ai_tag_create(tag, i)) continue;

        const double tag_scale = ai->data.tag_scale ? ai->data.tag_scale : 1.0;
        const double g = 1.0 + ai->scale / 30.0, bar_w = g * 120.0 * tag_scale, bar_h = (1.0 + (g - 1.0) * 0.3) * 16.0;
        char v[96];
        snprintf(v, sizeof(v), "%.3f|%.3f|%d", g, tag_scale, ai->max_health ? 1 : 0);
        if (strcmp(tag->geometry.value, v)) {
            snprintf(tag->geometry.value, sizeof(tag->geometry.value), "%s", v);
            char px[32];
            nt_px(px, sizeof(px), -bar_w / 2.0);
            html_dom_set_style(tag->bar, "left", px);
            html_dom_set_style(tag->hp, "left", px);
            nt_px(px, sizeof(px), -bar_h);
            html_dom_set_style(tag->bar, "top", px);
            html_dom_set_style(tag->hp, "top", px);
            nt_px(px, sizeof(px), bar_w);
            html_dom_set_style(tag->bar, "width", px);
            nt_px(px, sizeof(px), bar_h);
            html_dom_set_style(tag->bar, "height", px);
            html_dom_set_style(tag->hp, "height", px);
            html_dom_set_display(tag->bar, ai->max_health ? "block" : "none");
            html_dom_set_display(tag->hp, ai->max_health ? "block" : "none");
            nt_px(px, sizeof(px), g * 20.0);
            html_dom_set_style(tag->name, "font-size", px);
            nt_px(px, sizeof(px), -bar_h - g * 7.0 - NT_GAMEFONT_ASCENT * g * 20.0); // canvas baseline -> box top
            html_dom_set_style(tag->name, "top", px);
        }
        nt_text(tag->name, ai->name, tag->text_name, sizeof(tag->text_name));
        if (ai->max_health) {
            const double ratio = ai->health / ai->max_health;
            snprintf(v, sizeof(v), "%.2fpx", (ratio > 0.0 ? ratio : 0.0) * bar_w);
            nt_style(tag->hp, "width", v, &tag->hp_w);
            // (!ai.team || !mode.teams || ai.team != your team) && (!mode.friendly || ai.team != 1): healthColE
            const GameModeConfig *mode = client->game.mode ? &client->game.mode->config : NULL;
            const int my_team = client->me ? client->me->team : 0;
            const int enemy = (!ai->team || !mode || !mode->teams || ai->team != my_team) && (!mode || !mode->friendly || ai->team != 1);
            nt_style(tag->hp, "background-color", enemy ? rs->health_col_enemy : rs->health_col_team, &tag->hp_color);
        }
        snprintf(v, sizeof(v), "%.4f%%", fx * 100.0);
        nt_style(tag->root, "left", v, &tag->left);
        snprintf(v, sizeof(v), "%.4f%%", fy * 100.0);
        nt_style(tag->root, "top", v, &tag->top);
        snprintf(v, sizeof(v), "scale(%.4f)", nt_distance_scale(camera, anchor));
        nt_style(tag->root, "transform", v, &tag->transform);
        nt_display(tag->root, 1, &tag->shown);
    }
}

/* ------------------------------------------------------------------ mode markers */

static Marker *nt_marker(Marker *marker, const char *id, const int image) {
    if (marker->el) return marker;
    marker->el = html_dom_create(image ? "img" : "div", id);
    if (!marker->el) return NULL;
    html_dom_set_style(marker->el, "position", "absolute");
    html_dom_set_style(marker->el, "pointer-events", "none");
    if (!image) {
        // ctx.font = "50px GameFont", fillStyle #FBC02D, strokeText (lineWidth 4, #2e2e2e) 1 px right
        html_dom_set_style(marker->el, "width", "0");
        html_dom_set_style(marker->el, "height", "0");
        html_dom_set_style(marker->el, "transform-origin", "0 0");
        html_dom_set_html(marker->el,
            "<div style='position:absolute;left:-25px;top:-81.25px;font-family:gamefont;font-size:50px;line-height:1.375;"
            "white-space:nowrap;color:" NT_SITE_COLOR ";text-shadow:3px 0 " NT_STROKE ",-1px 0 " NT_STROKE ",1px 2px " NT_STROKE
            ",1px -2px " NT_STROKE ",3px 2px " NT_STROKE ",-1px -2px " NT_STROKE ",3px -2px " NT_STROKE ",-1px 2px " NT_STROKE "'></div>");
    }
    html_dom_set_display(marker->el, "none");
    html_dom_append(g_nt.layer, marker->el);
    marker->shown = 0;
    return marker;
}

static void nt_hide_markers(void) {
    for (int s = 0; s < NT_MAX_SITES; s++) nt_display(g_nt.sites[s].el, 0, &g_nt.sites[s].shown);
}

// Game.map.manager.sites (BOMB_SITE objects in map order): "A", "B", ... in 50 px GameFont
// scaled by aP / 50 with aP = aO * 40, drawn at (-25, -25).
static void nt_sites(Client *client) {
    const Map *map = client->game.map;
    int index = 0;
    for (size_t i = 0; map && i < map->object_count && index < NT_MAX_SITES; i++) {
        const Object *object = map->objects[i];
        if (!object->bomb_site) continue;
        Marker *marker = &g_nt.sites[index];
        const vec3 half = {object->scale.x * 0.5f, object->scale.y * 0.5f, object->scale.z * 0.5f};
        vec3 p = {object->position.x, object->position.y + half.y, object->position.z};
        double fx, fy;
        const int inside = nt_point_in_box(client->camera.position, p, half);
        p.y += NT_MARKER_UP;
        const char letter[2] = {(char) ('A' + index), 0};
        index++;
        if (inside || !nt_project(&client->camera, p, &fx, &fy)) {
            nt_display(marker->el, 0, &marker->shown);
            continue;
        }
        char id[32];
        snprintf(id, sizeof(id), "knSiteMark_%d", index - 1);
        if (!nt_marker(marker, id, 0)) continue;
        if (!marker->src.value[0]) {
            snprintf(marker->src.value, sizeof(marker->src.value), "%s", letter);
            html_dom_set_text(html_dom_first_child(marker->el), letter);
        }
        char v[48];
        snprintf(v, sizeof(v), "%.4f%%", fx * 100.0);
        nt_style(marker->el, "left", v, &marker->left);
        snprintf(v, sizeof(v), "%.4f%%", fy * 100.0);
        nt_style(marker->el, "top", v, &marker->top);
        snprintf(v, sizeof(v), "scale(%.4f)", nt_distance_scale(client->camera.position, p) * 40.0f / 50.0f);
        nt_style(marker->el, "transform", v, &marker->transform);
        nt_display(marker->el, 1, &marker->shown);
    }
    for (; index < NT_MAX_SITES; index++) nt_display(g_nt.sites[index].el, 0, &g_nt.sites[index].shown);
}

/* ------------------------------------------------------------------ layout demo */

// KRUNKNATIVE_NAMETAG_DEMO=2 (tests only): five tags at fixed camera-space points
// (right, up, forward) with fixed health / team / account values, so the layout can be
// compared with v3.1.1's overlay.render fed the same points (style, opacity, healthNum
// and streamer mode come from the settings as usual). Index 2 carries a level, a clan
// and all three badges, index 0 the voice speaker icon; 1 and 4 are teammates.
static void nt_demo_tags(Client *client) {
    static const float spec[NT_DEMO_TAGS][6] = { // right, up, forward, health, hpChase, teammate
        {-7.0f, -1.0f, 14.0f, 100.0f, 1.0f, 0.0f}, {6.0f, 1.0f, 24.0f, 45.0f, 0.8f, 1.0f},
        {-12.0f, 3.0f, 60.0f, 100.0f, 1.0f, 0.0f}, {25.0f, 6.0f, 150.0f, 70.0f, 0.7f, 0.0f},
        {-40.0f, 10.0f, 400.0f, 100.0f, 1.0f, 1.0f},
    };
    const float *m = client->camera.world_inverse_matrix; // rows: camera right, up, back in world space
    for (int i = 0; i < NT_DEMO_TAGS; i++) {
        Tag *tag = &g_nt.tags[NT_MAX_TAGS - 1 - i];
        tag->uid = NT_DEMO_UID - i;
        const float *s = spec[i];
        const vec3 anchor = {
            client->camera.position.x + m[0] * s[0] + m[4] * s[1] - m[8] * s[2],
            client->camera.position.y + m[1] * s[0] + m[5] * s[1] - m[9] * s[2],
            client->camera.position.z + m[2] * s[0] + m[6] * s[1] - m[10] * s[2],
        };
        Player fake = {0};
        fake.uid = 10 + i;
        fake.health = s[3];
        fake.max_health = 100;
        fake.hp_chase = s[4];
        TagInfo info = {0};
        info.custom_badge = -1;
        if (client_render_settings()->stream_mode) snprintf(info.name, sizeof(info.name), "Player %d", fake.uid);
        else snprintf(info.name, sizeof(info.name), "Guest_%d", i + 2);
        info.demo_voice = i == 0;
        if (i == 2) {
            info.level = 57;
            info.custom_badge = 11; // WP-E part 2c: Nuclear I
            if (!client_render_settings()->stream_mode) snprintf(info.clan, sizeof(info.clan), "KN");
            info.badge[NT_BADGE_PARTNER] = info.badge[NT_BADGE_PREMIUM] = info.badge[NT_BADGE_FEATURED] = 1;
        }
        double fx, fy;
        if (!nt_project(&client->camera, anchor, &fx, &fy)) {
            nt_tag_hide(tag);
            continue;
        }
        if (!tag->root && !nt_tag_create(tag, NT_MAX_TAGS - 1 - i)) continue;
        nt_tag_update(tag, &fake, &info, fx, fy, nt_distance_scale(client->camera.position, anchor), s[5] != 0.0f);
    }
}

/* ------------------------------------------------------------------ frame */

static void nt_hide_all(void) {
    if (!g_nt.layer || g_nt.serial != html_ui_page_serial()) return;
    nt_display(g_nt.layer, 0, &g_nt.layer_shown);
}

// KRUNKNATIVE_NAMETAG_DEMO (natively) / ?nametagDemo= (web): test-only layout fixtures.
static int nt_demo_setting(void) {
#ifdef __EMSCRIPTEN__
    return EM_ASM_INT({
        var value = null;
        try { value = new URLSearchParams(location.search).get('nametagDemo'); } catch (e) {}
        return value ? (parseInt(value, 10) || 0) : 0;
    });
#else
    const char *demo = getenv("KRUNKNATIVE_NAMETAG_DEMO");
    return demo && demo[0] ? atoi(demo) : 0;
#endif
}

static void nt_watchdog(void *user) {
    (void) user;
    g_nt.watchdog = 0;
    const double now_ms = glfwGetTime() * 1000.0;
    if (now_ms - g_nt.last_tick_ms >= NT_WATCHDOG_MS) {
        if (html_ui_available() && !strcmp(html_ui_current_page(), "index.html")) nt_hide_all();
        return;
    }
    g_nt.watchdog = html_timer_add(NT_WATCHDOG_MS, nt_watchdog, NULL);
}

void nametags_tick(Client *client) {
    if (!client || !html_ui_available() || !html_ui_is_ready() || strcmp(html_ui_current_page(), "index.html")) return;
    const HtmlElem layer = nt_layer();
    if (!layer) return;

    const double now = glfwGetTime();
    g_nt.last_tick_ms = now * 1000.0;
    if (!g_nt.watchdog) g_nt.watchdog = html_timer_add(NT_WATCHDOG_MS, nt_watchdog, NULL);
    if (g_nt.demo < 0) g_nt.demo = nt_demo_setting();

    if (!client->game.ready || !client->game.map) {
        nt_hide_all();
        return;
    }

    // The layer is the canvas: viewport-sized, in overlay units (width / scale) under ctx.scale(scale).
    float vw, vh, dpr;
    html_dom_viewport(&vw, &vh, &dpr);
    const double scale = nt_ui_scale(vw, vh);
    char v[48];
    nt_px(v, sizeof(v), vw / scale);
    nt_style(layer, "width", v, &g_nt.layer_w);
    nt_px(v, sizeof(v), vh / scale);
    nt_style(layer, "height", v, &g_nt.layer_h);
    snprintf(v, sizeof(v), "scale(%.4f)", scale);
    nt_style(layer, "transform", v, &g_nt.layer_transform);
    nt_display(layer, 1, &g_nt.layer_shown);

    if (now >= g_nt.next_vis) {
        g_nt.next_vis = now + NT_VIS_RATE_S;
        nt_visibility_pass(client);
    }

    // aA: #endUI or #menuHolder up; aB: also no kill card
    const int menu_up = nt_dom_shown("endUI") || nt_dom_shown("menuHolder");
    const int card_up = nt_dom_shown("killCardHolder");
    nt_ais(client, client->camera.position, menu_up);
    nt_players(client, client->camera.position, menu_up);

    const GameModeConfig *mode = client->game.mode ? &client->game.mode->config : NULL;
    if (menu_up || card_up || !mode) {
        nt_hide_markers();
        return;
    }
    // the flag pings and the Hardpoint / Domination markers are canvas drawing in v7
    // (game-overlay.js f1565, the dom zone circles): client/src/ui/hud_objectives.c
    if (mode->site) nt_sites(client);
    else for (int s = 0; s < NT_MAX_SITES; s++) nt_display(g_nt.sites[s].el, 0, &g_nt.sites[s].shown);
}

int nametags_project(const Camera *camera, const vec3 point, double *fx, double *fy) {
    return nt_project(camera, point, fx, fy);
}

double nametags_ui_scale(const float vw, const float vh) {
    return nt_ui_scale(vw, vh);
}
