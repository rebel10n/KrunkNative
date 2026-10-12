// player-viewer WP-E part 2a: the Krunker v7.1.4 minimap and death markers.
//
// Minimap (v7 src/libs/Minimap.js, game-overlay.js render): a 300 x minimapScale px canvas in
// #miniMapOverlay. While playing (type 1) it is centred on the camera and turns with the view
// (round, .roundMap, with N/E/S/W on a dark ring); while spectating (type 0) it shows the whole
// map with every player. Map blocks are filled darker the higher they are, the active
// objective, the flags, bomb sites and dom zones are outlined, teammates are arrows and enemies
// blip red for config.playerBlipTimer after they fire. It is drawn on the CPU (canvas2d.c) and
// handed to the page with html_dom_canvas_put; the N/E/S/W letters are DOM text over it.
//   Minimap.toggle: off with mode.noMinimap, game config minimaps off, or on a custom map
//   (customsData && config.isProd: v7 production), and with the showMinimap setting off.
//   Settings: minimapScale (setScale), minimapZoom (setZoom), miniMapHudScale (setHudScale).
// Documented differences:
//   - v7 bug fixed: the spectator's specMiniMap key toggles a setting that does not exist
//     (setSetting("specMiniMap"), index.js:16357), so it does nothing. Here it hides / shows
//     the minimap while spectating (a session toggle, like showSpectatorNameTags).
//   Maps with an "mmId" (Kanji among the official ones) draw that user asset image
//   (user-assets/<id>/texture.png, updateImage / drawImage) instead of their blocks.
//
// Death markers (game-overlay.js addDeathMarker / f1566): a skull at the death spot of a
// teammate, ourselves, or anyone while spectating, sized max(0.3, 1 - d / 600) * 70 overlay
// px, hidden within 10 units, deathMarkerAdsOpac near the crosshair while aiming. Settings
// deathMarkers (0 all, 1 self, 2 team, 3 none), deathMarkerOpac, deathMarkerAdsOpac,
// deathMarkerDur. Documented differences:
//   - v7 bug fixed (doc/v7/player-viewer.md 5.1 item 3): v7 sets life = deathMarkerDur (7) and
//     subtracts the frame delta in ms, so a marker lasts about one frame. KrunkNative reads the
//     setting as seconds ("Death Marker Duration (s)"): a marker lasts deathMarkerDur * 1000 ms.
//   - v7 draws other players' markers with img/death_2_1.png, which is not in v7.1.4's public
//     tree (only death_0_1.png is): those markers never appear in v7. KrunkNative draws
//     death_0_1.png for every marker.
// Both are elements of #inGameUI (below the HUD, scaled with #uiBase like the overlay canvas).
#include <hud_map.h>
#include <cpu_canvas.h>
#include <spectate.h>
#include <ui_logic.h>
#include <html_ui.h>
#include <html_dom.h>
#include <map_catalog.h>
#include <modes.h>
#include <cJSON.h>
#include <stb_image.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MM_BASE_WIDTH 300.0f     // Minimap.js vLN3003
#define MM_MAP_SCALE 6.0f        // vLN68
#define MM_HUD_SCALE 6.0f        // vLN69
#define MM_FOCUSED_SCALE 3.0f    // vLN316
#define MM_BLIP_TIMER_MS 2000.0  // config.playerBlipTimer
#define MM_FLAG_HALF 12.0f       // config.flagScale (manager.flags w / l)
#define MM_BLIPS_MAX 64
#define DM_MAX 32
#define DM_HIDE_DIST 10.0f       // pointInBox3D(camera, marker, 10)

/* ------------------------------------------------------------ settings */

static double setting_num(const char *key, const double fallback) {
    double v;
    return ui_setting_number(key, &v) ? v : fallback;
}

static int setting_on(const char *key, const int fallback) {
    const cJSON *v = ui_setting_value(key);
    if (cJSON_IsBool(v)) return cJSON_IsTrue(v);
    if (cJSON_IsNumber(v)) return v->valuedouble != 0.0;
    if (cJSON_IsString(v) && v->valuestring) return !strcmp(v->valuestring, "true");
    return fallback;
}

/* ------------------------------------------------------------ minimap state */

typedef struct {
    int uid;
    double timer_ms;  // minimapTimer
    vec3 pos;         // minimapPos
} MmBlip;

static struct {
    CpuCanvas cv;
    int enabled;           // Minimap.enabled (the toggle), -1 unknown
    int applied;           // the DOM state written for `enabled`, -1 none
    int type;              // 0 spectator (whole map), 1 player (centred, turning); -1 unknown
    float scale;           // setScale: minimapScale
    int width;             // canvas px (300 * scale, truncated like canvas.width)
    float map_scale, focused_scale, hud_scale;
    int custom_override;   // -1: from the map
    int spec_hidden;       // specMiniMap (KrunkNative fix, see the top)
    // setBounds
    const Map *bounds_map;
    float bounds_map_scale;
    float min[3], max[3], cen[3], width_w, height_h, unscaled_width;
    float max_height;
    float pmin[3], pmax[3]; // playerBounds
    MmBlip blips[MM_BLIPS_MAX];
    double last_ms;
    HtmlElem dirs;         // #knMiniMapDirs: the four letters
    char dirs_key[64];
    // updateImage: the map's "mmId" image
    int image_asset;       // the asset the image was loaded for, 0 none
    unsigned char *image;  // straight RGBA8, NULL until loaded / failed
    int image_w, image_h;
} g_mm = {.enabled = -1, .applied = -1, .type = -1, .custom_override = -1, .last_ms = -1.0};

void hud_minimap_set_custom_override(const int custom) { g_mm.custom_override = custom < 0 ? -1 : !!custom; }
void hud_minimap_spectator_toggle(void) { g_mm.spec_hidden = !g_mm.spec_hidden; }
int hud_minimap_spectator_hidden(void) { return g_mm.spec_hidden; }

// customsData: the map is not one of the official catalogue maps.
static int mm_custom_map(const Client *client) {
    if (g_mm.custom_override >= 0) return g_mm.custom_override;
    const Map *map = client->game.map;
    if (!map || !map->name) return 0;
    for (size_t i = 0; i < MAP_CATALOG_OFFICIAL_COUNT && i < map_catalog_count(); i++) {
        const MapCatalogEntry *e = map_catalog_at(i);
        if (e && e->source == MAP_SOURCE_OFFICIAL && e->name && !strcasecmp(e->name, map->name)) return 0;
    }
    return 1;
}

// The blocks Minimap draws: manager.objects without noMinimap. v7 adds the special objects
// (pads, ladders, zones, teleporters, flags, ...) with noMinimap, so only solid blocks remain.
static int mm_block(const Object *o) {
    if (!o || o->no_minimap || o->collision_type == COLLISION_TYPE_NONE) return 0;
    return !(o->jump_pad || o->ladder || o->kill || o->teleporter || o->checkpoint || o->flag || o->objective ||
             o->bomb_site || o->score_zone || o->team_zone || o->trigger || o->spawnable);
}

// v7 GameObject of a block: y = bottom + h / 2 (centre), w / l / h half extents.
static float mm_block_y(const Object *o) { return o->position.y + o->scale.y / 2.0f; }

// setBounds(game) + setZoom: spawns (or map anchors) framed with a margin, then every bound
// pushed outwards by mapScale (a positive min moves up: v7's quirk, kept).
static void mm_set_bounds(const Map *map) {
    float mn[3] = {0, 0, 0}, mx[3] = {0, 0, 0}, cen[3] = {0, 0, 0};
    const int anchors = map->map_anchor_count > 0;
    const size_t count = anchors ? map->map_anchor_count : map->spawn_count;
    for (size_t i = 0; i < count; i++) {
        vec3 p;
        if (anchors) {
            const size_t idx = map->map_anchors[i];
            if (idx >= map->object_count || !map->objects[idx]) continue;
            p = map->objects[idx]->position;
        } else {
            if (!map->spawns[i]) continue;
            p = map->spawns[i]->position;
        }
        if (p.x < mn[0]) mn[0] = p.x;
        if (p.x > mx[0]) mx[0] = p.x;
        if (p.z < mn[2]) mn[2] = p.z;
        if (p.z > mx[2]) mx[2] = p.z;
        if (p.y > mx[1]) mx[1] = p.y;
    }
    const float w = mx[0] - mn[0], h = mx[2] - mn[2];
    cen[0] = mn[0] + w / 2.0f;
    cen[2] = mn[2] + h / 2.0f;
    if (anchors) {
        const float side = fmaxf(w, h);
        mn[0] = cen[0] - side / 2.0f;
        mn[2] = cen[2] - side / 2.0f;
        mx[0] = cen[0] + side / 2.0f;
        mx[2] = cen[2] + side / 2.0f;
    } else {
        mx[0] += w / 8.0f * 2.0f;
        mn[0] -= w / 8.0f * 2.0f;
        mx[2] += h / 8.0f * 2.0f;
        mn[2] -= h / 8.0f * 2.0f;
    }
    g_mm.unscaled_width = fabsf(mx[0] - mn[0]);
    for (int k = 0; k < 3; k++) {
        g_mm.min[k] = mn[k] < 0.0f ? mn[k] - g_mm.map_scale : mn[k] + g_mm.map_scale;
        g_mm.max[k] = mx[k] < 0.0f ? mx[k] - g_mm.map_scale : mx[k] + g_mm.map_scale;
        g_mm.cen[k] = cen[k];
    }
    g_mm.width_w = fabsf(g_mm.max[0] - g_mm.min[0]);
    g_mm.height_h = fabsf(g_mm.max[2] - g_mm.min[2]);
    g_mm.max_height = 0.0f;
    int any = 0;
    for (size_t i = 0; i < map->object_count; i++) {
        const Object *o = map->objects[i];
        if (!mm_block(o)) continue;
        const float y = mm_block_y(o);
        if (!any || y > g_mm.max_height) g_mm.max_height = y;
        any = 1;
    }
    g_mm.bounds_map = map;
    g_mm.bounds_map_scale = g_mm.map_scale;
}

// updateImage(game): mmId -> user-assets/<id>/texture.png (useImage); loaded once per map.
static void mm_update_image(const Map *map) {
    const int asset = map ? map->minimap_asset : 0;
    if (asset == g_mm.image_asset) return;
    stbi_image_free(g_mm.image);
    g_mm.image = NULL;
    g_mm.image_w = g_mm.image_h = 0;
    g_mm.image_asset = asset;
    if (asset <= 0) return;
    char path[1024];
    snprintf(path, sizeof(path), "%suser-assets/%d/texture.png", assets_path(), asset);
    if (!ensure_asset_file(path)) return;
    int n = 0;
    stbi_set_flip_vertically_on_load_thread(0);
    g_mm.image = stbi_load(path, &g_mm.image_w, &g_mm.image_h, &n, 4);
    if (!g_mm.image) fprintf(stderr, "[minimap] %s: %s\n", path, stbi_failure_reason());
}

static float mm_conv(const float v, const int axis) {
    const int player = g_mm.type == 1; // centeredOnPlayer && type == 1
    const float lo = player ? g_mm.pmin[axis] : g_mm.min[axis], hi = player ? g_mm.pmax[axis] : g_mm.max[axis];
    const float half = (float) g_mm.width / 2.0f;
    return hi != lo ? (v - lo) * (half * 2.0f) / (hi - lo) - half : 0.0f;
}

// drawObject(obj, color, fill): a 3 px rgba(0,0,0,0.8) outline, then filled, or outlined 1.5 px.
static void mm_draw_box(const float x, const float z, const float hw, const float hl, const CcColor color, const int fill) {
    CpuCanvas *c = &g_mm.cv;
    const float cx = mm_conv(x, 0), cz = mm_conv(z, 2);
    const float w = mm_conv(x + hw, 0) - mm_conv(x - hw, 0), l = mm_conv(z + hl, 2) - mm_conv(z - hl, 2);
    cc_save(c);
    cc_translate(c, cx, cz);
    cc_stroke_rect(c, -w / 2.0f, -l / 2.0f, w, l, 3.0f, cc_rgba(0, 0, 0, 0.8f));
    if (fill) cc_fill_rect(c, -w / 2.0f, -l / 2.0f, w, l, color);
    else cc_stroke_rect(c, -w / 2.0f, -l / 2.0f, w, l, 1.5f, color);
    cc_restore(c);
}

// drawDirectionalBlip(x, y, z, dir, team, id, grey)
static void mm_draw_arrow(const float x, const float z, const float dir, const int team, const int id, const int grey, const int focused_id) {
    CpuCanvas *c = &g_mm.cv;
    const int own_spect = id == -1 && !g_mm.type;
    const float a = own_spect ? 0.5f : 1.0f, fa = a / 2.0f;
    const float s = g_mm.hud_scale * g_mm.scale;
    const int green = g_mm.type && !grey;
    float r, g, b;
    if (own_spect || grey) r = g = b = 150.0f;
    else if (green) { r = 158; g = 235; b = 86; }
    else if (team == 1) { r = 1; g = 107; b = 237; }
    else { r = 236; g = 107; b = 1; }
    CcColor stroke = cc_rgba(r, g, b, a);
    if (focused_id >= 0 && focused_id == id) stroke = cc_rgba(255, 255, 255, a);
    const float pts[10] = {0, s * 0.5f, -s * 1.1f, s, 0, -s * 1.25f, s * 1.1f, s, 0, s * 0.5f};
    cc_save(c);
    cc_translate(c, mm_conv(x, 0), mm_conv(z, 2));
    cc_rotate(c, -dir);
    cc_fill_poly(c, pts, 4, cc_rgba(r, g, b, fa));
    cc_stroke_poly(c, pts, 4, g_mm.scale * 2.0f, stroke);
    cc_restore(c);
}

// drawBlip(x, z, timer): a red dot fading over playerBlipTimer
static void mm_draw_blip(const float x, const float z, const double timer_ms) {
    CpuCanvas *c = &g_mm.cv;
    const float s = g_mm.hud_scale * g_mm.scale;
    const float alpha = c->alpha;
    c->alpha = (float) (timer_ms / MM_BLIP_TIMER_MS);
    const float cx = mm_conv(x, 0), cz = mm_conv(z, 2);
    cc_fill_circle(c, cx, cz, s, cc_rgba(237, 66, 66, 1));
    cc_stroke_circle(c, cx, cz, s, 1.0f, cc_rgba(0, 0, 0, 1));
    c->alpha = alpha;
}

static int mm_compare_y(const void *a, const void *b) {
    const float ya = mm_block_y(*(const Object *const *) a), yb = mm_block_y(*(const Object *const *) b);
    return ya < yb ? -1 : ya > yb;
}

static MmBlip *mm_blip(const int uid, const int create) {
    MmBlip *free_slot = NULL;
    for (int i = 0; i < MM_BLIPS_MAX; i++) {
        if (g_mm.blips[i].timer_ms > 0.0 && g_mm.blips[i].uid == uid) return &g_mm.blips[i];
        if (!free_slot && g_mm.blips[i].timer_ms <= 0.0) free_slot = &g_mm.blips[i];
    }
    return create ? free_slot : NULL;
}

void hud_minimap_on_shot(Client *client, const int uid) {
    const Player *p = client_find_player(client, uid);
    if (!p) return;
    MmBlip *b = mm_blip(uid, 1);
    // !minimapTimer || minimapTimer < playerBlipTimer / 2
    if (!b || (b->timer_ms > 0.0 && b->timer_ms >= MM_BLIP_TIMER_MS / 2.0)) return;
    b->uid = uid;
    b->timer_ms = MM_BLIP_TIMER_MS;
    b->pos = p->position;
}

/* ------------------------------------------------------------ minimap DOM */

static void mm_class(const char *id, const char *cls, const int on) {
    const HtmlElem el = html_dom_by_id(id);
    if (el && html_dom_has_class(el, cls) != on) html_dom_set_class(el, cls, on);
}

// toggle(game, on): #miniMapOverlay shown or not, .topLeftOld on the timer / match info /
// spectator boxes when it is off.
void hud_minimap_apply_layout(void) {
    const int on = g_mm.enabled == 1;
    static const char *const ids[] = {"timerHolder", "timerIcon", "timerVal", "timerInner", "matchInfo", "specNames", "specStats", "specSUS"};
    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) mm_class(ids[i], "topLeftOld", !on);
    const HtmlElem el = html_dom_by_id("miniMapOverlay");
    if (el) {
        html_dom_set_display(el, on ? "" : "none");
        html_dom_set_style(el, "position", "relative"); // the N/E/S/W letters are placed in it
    }
    g_mm.applied = on;
}

// setScale(minimapScale): canvas.width = 300 * s (a JS canvas size truncates), the overlay box
// 300 * s px, --mm-scale for the spectator boxes under it.
static void mm_apply_scale(const float scale) {
    if (scale == g_mm.scale && g_mm.width > 0) return;
    g_mm.scale = scale;
    g_mm.width = (int) (MM_BASE_WIDTH * scale);
    if (g_mm.width < 1) g_mm.width = 1;
    char px[32];
    snprintf(px, sizeof(px), "%gpx", MM_BASE_WIDTH * scale);
    const HtmlElem el = html_dom_by_id("miniMapOverlay");
    if (el) {
        html_dom_set_style(el, "width", px);
        html_dom_set_style(el, "height", px);
    }
    snprintf(px, sizeof(px), "%g", scale);
    html_dom_set_style(html_dom_root(), "--mm-scale", px);
    cc_resize(&g_mm.cv, g_mm.width, g_mm.width);
    g_mm.dirs_key[0] = 0;
}

// drawCardinalDirections' letters: 8px GameFont, centred W / 2 - 10 * scale from the centre at
// yaw + k * 90 degrees, turned with it (the ring is drawn on the canvas).
static void mm_directions(const int shown, const float yaw) {
    if (!g_mm.dirs && shown) {
        const HtmlElem holder = html_dom_by_id("miniMapOverlay");
        if (!holder) return;
        html_dom_insert_html(holder, HTML_INSERT_BEFORE_END,
            "<div id='knMiniMapDirs' style='position:absolute;left:50%;top:50%;width:0;height:0;pointer-events:none'></div>");
        g_mm.dirs = html_dom_by_id("knMiniMapDirs");
        g_mm.dirs_key[0] = 0;
    }
    if (!g_mm.dirs) return;
    char key[64];
    snprintf(key, sizeof(key), "%d|%.4f|%d", shown, shown ? yaw : 0.0f, g_mm.width);
    if (!strcmp(key, g_mm.dirs_key)) return;
    snprintf(g_mm.dirs_key, sizeof(g_mm.dirs_key), "%s", key);
    html_dom_set_display(g_mm.dirs, shown ? "block" : "none");
    if (!shown) return;
    static const char *const letters[4] = {"N", "E", "S", "W"};
    char html[2048];
    size_t len = 0;
    html[0] = 0;
    const float radius = (float) g_mm.width / 2.0f - g_mm.scale * 10.0f;
    for (int k = 0; k < 4; k++) {
        const float deg = (yaw * 180.0f / (float) M_PI) + (float) k * 90.0f;
        len += (size_t) snprintf(html + len, sizeof(html) - len,
            "<div style='position:absolute;left:0;top:0;width:0;height:0;transform:rotate(%.3fdeg)'>"
            "<div style='position:absolute;left:-10px;width:20px;top:%.2fpx;height:10px;line-height:10px;text-align:center;"
            "font-family:gamefont;font-size:8px;color:#fff'>%s</div></div>", deg, -radius - 5.0f, letters[k]);
        if (len >= sizeof(html)) break;
    }
    html_dom_set_html(g_mm.dirs, html);
}

/* ------------------------------------------------------------ minimap update */

static void mm_update(Client *client, const double dt) {
    CpuCanvas *c = &g_mm.cv;
    const Game *game = &client->game;
    const Map *map = game->map;
    const GameMode *mode = game->mode;
    const int spectating = spectate_active();
    const Player *me = client->me;

    const int type = spectating ? 0 : 1;
    if (type != g_mm.type) {
        g_mm.type = type;
        mm_class("miniMapOverlay", "roundMap", type == 1);
    }
    const Player *focused = NULL;
    if (!type && spectate_target_uid() >= 0) focused = client_find_player(client, spectate_target_uid());
    const int team = me && me->team ? me->team : spectating ? 1 : 0; // v9928

    cc_reset_transform(c);
    cc_fill_all(c, cc_rgba(59, 58, 57, 1));
    // updatePlayerBounds(focusedPlayer || controls.object.position)
    const vec3 centre = focused ? focused->position : client->camera.position;
    g_mm.pmin[0] = centre.x - g_mm.width_w / g_mm.focused_scale;
    g_mm.pmin[2] = centre.z - g_mm.height_h / g_mm.focused_scale;
    g_mm.pmax[0] = centre.x + g_mm.width_w / g_mm.focused_scale;
    g_mm.pmax[2] = centre.z + g_mm.height_h / g_mm.focused_scale;
    cc_translate(c, (float) g_mm.width / 2.0f, (float) g_mm.width / 2.0f);
    const float yaw = me && me->active ? me->direction.y : client->camera.rotation.y; // controls.xDr
    if (type == 1) cc_rotate(c, yaw);

    // useImage: the "mmId" image over the square around the bounds' centre (unscaled width)
    if (g_mm.image_asset > 0) {
        if (g_mm.image) {
            const float half = g_mm.unscaled_width / 2.0f;
            const float x0 = mm_conv(g_mm.cen[0] - half, 0), x1 = mm_conv(g_mm.cen[0] + half, 0);
            const float z0 = mm_conv(g_mm.cen[2] - half, 2), z1 = mm_conv(g_mm.cen[2] + half, 2);
            const float w = x1 - x0, h = z1 - z0;
            cc_save(c);
            cc_translate(c, mm_conv(g_mm.cen[0], 0), mm_conv(g_mm.cen[2], 2));
            cc_draw_image(c, g_mm.image, g_mm.image_w, g_mm.image_h, -w / 2.0f, -h / 2.0f, w, h);
            cc_restore(c);
        }
    } else if (map && map->object_count) { // map blocks, lowest first, hsl(220, 25%, 35 - y / MAX_HEIGHT * 20 %)
        const Object **list = malloc(map->object_count * sizeof(*list));
        size_t n = 0;
        if (list) {
            for (size_t i = 0; i < map->object_count; i++)
                if (mm_block(map->objects[i])) list[n++] = map->objects[i];
            qsort(list, n, sizeof(*list), mm_compare_y);
            for (size_t i = 0; i < n; i++) {
                const Object *o = list[i];
                const float top = mm_block_y(o) + o->scale.y / 2.0f;
                const float l = 35.0f - (g_mm.max_height != 0.0f ? top / g_mm.max_height : 0.0f) * 20.0f;
                mm_draw_box(o->position.x, o->position.z, o->scale.x / 2.0f, o->scale.z / 2.0f, cc_hsl(220, 25, l), 1);
            }
            free(list);
        }
    }
    if (mode && mode->config.objective) {
        const ModeObjective *obj = mode_objective_by_uid(game, mode->active_objective);
        if (obj) mm_draw_box(obj->position.x, obj->position.z, obj->half.x, obj->half.z, cc_rgba(255, 255, 0, 1), 0);
    }
    if (mode && (mode->config.flags || mode->config.oneflag)) {
        for (int i = 0; i < mode->flag_count; i++) {
            const ModeFlag *f = &mode->flags[i];
            if (!f->active || (mode->config.oneflag ? !f->neutral : f->neutral)) continue;
            mm_draw_box(f->position.x, f->position.z, MM_FLAG_HALF, MM_FLAG_HALF, cc_hex(f->team == team ? "#2196F3" : "#ed4242"), 0);
        }
    }
    if (mode && mode->config.site && map) {
        for (size_t i = 0; i < map->object_count; i++) {
            const Object *o = map->objects[i];
            if (o && o->bomb_site) mm_draw_box(o->position.x, o->position.z, o->scale.x / 2.0f, o->scale.z / 2.0f, cc_rgba(255, 255, 0, 1), 0);
        }
    }
    if (mode && mode->config.dom) {
        for (int i = 0; i < mode->dom_zone_count; i++) {
            const ModeDomZone *z = &mode->dom_zones[i];
            const char *col = !z->captured_by ? "#dbdbdb" : z->captured_by == team ? "#5699EB" : "#eb5656";
            mm_draw_box(z->position.x, z->position.z, z->half.x, z->half.z, cc_hex(col), 0);
        }
    }
    // players: teammates (or everyone while spectating) as arrows, enemies as fading blips
    for (int i = 0; i < MM_BLIPS_MAX; i++) {
        if (g_mm.blips[i].timer_ms <= 0.0) continue;
        g_mm.blips[i].timer_ms -= dt;
        if (g_mm.blips[i].timer_ms < 0.0) g_mm.blips[i].timer_ms = 0.0;
    }
    for (size_t i = 0; i < game->player_count; i++) {
        const Player *p = game->players[i];
        if (!p || !p->active || p == me) continue;
        const int mate = (team && team == p->team) || (mode && mode->config.fake_names && team != 2);
        if (mate || spectating) {
            mm_draw_arrow(p->position.x, p->position.z, p->direction.y, p->team, p->uid, 0, focused ? focused->uid : -1);
        } else {
            const MmBlip *b = mm_blip(p->uid, 0);
            if (b) mm_draw_blip(b->pos.x, b->pos.z, b->timer_ms);
        }
    }
    if ((!focused && !type) || type)
        mm_draw_arrow(client->camera.position.x, client->camera.position.z, yaw, 0, -1, type == 1, focused ? focused->uid : -1);
    if (type == 1) cc_stroke_circle(c, 0, 0, (float) g_mm.width / 2.0f - g_mm.scale * 7.0f, 16.0f * g_mm.scale, cc_rgba(25, 25, 25, 0.85f));
    mm_directions(type == 1, yaw);

    const HtmlElem canvas = html_dom_by_id("miniMapCanvas");
    if (canvas) html_dom_canvas_put(canvas, c->px, c->w, c->h);
}

/* ------------------------------------------------------------ death markers */

typedef struct {
    float x, y, z;
    int is_you;
    double life_ms;
    HtmlElem el;
    int shown;
    char key[96];
} DeathMarker;

static DeathMarker g_dm[DM_MAX];
static HtmlElem g_dm_holder;

void hud_death_marker_add(const vec3 position, const int is_you) {
    const int display = (int) setting_num("deathMarkers", 0); // 0 all, 1 self only, 2 team only, 3 none
    if (display == 3 || (display == 1 && !is_you) || (display == 2 && is_you)) return;
    DeathMarker *m = NULL;
    for (int i = 0; i < DM_MAX && !m; i++)
        if (g_dm[i].life_ms <= 0.0) m = &g_dm[i];
    if (!m) { // all in use: the one closest to its end
        m = &g_dm[0];
        for (int i = 1; i < DM_MAX; i++)
            if (g_dm[i].life_ms < m->life_ms) m = &g_dm[i];
    }
    m->is_you = is_you;
    m->life_ms = setting_num("deathMarkerDur", 7) * 1000.0; // v7 bug fixed: seconds, not ms (see the top)
    m->x = position.x;
    m->y = position.y;
    m->z = position.z;
}

static void dm_hide(DeathMarker *m) {
    if (m->el && m->shown) html_dom_set_display(m->el, "none");
    m->shown = 0;
}

static HtmlElem dm_element(DeathMarker *m) {
    if (m->el) return m->el;
    if (!g_dm_holder) {
        const HtmlElem host = html_dom_by_id("inGameUI");
        if (!host) return 0;
        html_dom_insert_html(host, HTML_INSERT_AFTER_BEGIN,
            "<div id='knDeathMarkers' style='position:absolute;left:0;top:0;width:100%;height:100%;pointer-events:none'></div>");
        g_dm_holder = html_dom_by_id("knDeathMarkers");
        if (!g_dm_holder) return 0;
    }
    m->el = html_dom_create("img", NULL);
    html_dom_set_attr(m->el, "src", "./img/death_0_1.png");
    html_dom_set_style(m->el, "position", "absolute");
    html_dom_set_style(m->el, "transform", "translate(-50%, -50%)");
    html_dom_set_display(m->el, "none");
    html_dom_append(g_dm_holder, m->el);
    m->shown = 0;
    m->key[0] = 0;
    return m->el;
}

static void dm_tick(Client *client, const double dt, const int drawn) {
    const vec3 cam = client->camera.position;
    const float aim = client->me ? client->me->aim_val : 0.0f; // KN aim_val = 1 - v7 aimVal
    for (int i = 0; i < DM_MAX; i++) {
        DeathMarker *m = &g_dm[i];
        if (m->life_ms <= 0.0) {
            dm_hide(m);
            continue;
        }
        if (!drawn) { // f1566 runs (and ages the markers) only while the overlay draws the game
            dm_hide(m);
            continue;
        }
        m->life_ms -= dt;
        if (m->life_ms < 0.0) m->life_ms = 0.0;
        const int near = fabsf(cam.x - m->x) <= DM_HIDE_DIST && fabsf(cam.y - m->y) <= DM_HIDE_DIST && fabsf(cam.z - m->z) <= DM_HIDE_DIST;
        double sx = 0.0, sy = 0.0;
        const float ty = m->y + 10.0f;
        const float dx = cam.x - m->x, dy = cam.y - ty, dz = cam.z - m->z;
        const double size = fmax(0.3, 1.0 - sqrt(dx * dx + dy * dy + dz * dz) / 600.0) * 70.0;
        if (m->life_ms <= 0.0 || near || size < 1.0 || !hud_ui_project(client, m->x, ty, m->z, &sx, &sy)) {
            dm_hide(m);
            continue;
        }
        const HtmlElem el = dm_element(m);
        if (!el) continue;
        // near the crosshair while aiming: deathMarkerAdsOpac
        const int ads = sx >= 0.45 && sx <= 0.55 && sy >= 0.4 && sy <= 0.6 && aim != 0.0f;
        const double opacity = setting_num(ads ? "deathMarkerAdsOpac" : "deathMarkerOpac", 1.0);
        char key[96];
        snprintf(key, sizeof(key), "%.3f|%.3f|%.2f|%.2f", sx * 100.0, sy * 100.0, size, opacity);
        if (strcmp(key, m->key)) {
            snprintf(m->key, sizeof(m->key), "%s", key);
            char v[32];
            snprintf(v, sizeof(v), "%.3f%%", sx * 100.0);
            html_dom_set_style(el, "left", v);
            snprintf(v, sizeof(v), "%.3f%%", sy * 100.0);
            html_dom_set_style(el, "top", v);
            snprintf(v, sizeof(v), "%.2fpx", size);
            html_dom_set_style(el, "width", v);
            snprintf(v, sizeof(v), "%.3g", opacity);
            html_dom_set_style(el, "opacity", v);
        }
        if (!m->shown) html_dom_set_display(el, "block");
        m->shown = 1;
    }
}

/* ------------------------------------------------------------ frame */

void hud_map_on_init(Client *client) {
    (void) client;
    for (int i = 0; i < DM_MAX; i++) {
        g_dm[i].life_ms = 0.0;
        dm_hide(&g_dm[i]);
    }
    memset(g_mm.blips, 0, sizeof(g_mm.blips));
    g_mm.bounds_map = NULL; // reset(): setBounds on the new map
    g_mm.enabled = -1;
}

void hud_map_tick(Client *client) {
    if (!html_ui_available() || !html_ui_is_ready()) return;
    const double now = hud_ui_now_ms(client);
    double dt = g_mm.last_ms >= 0.0 ? now - g_mm.last_ms : 0.0;
    if (dt < 0.0) dt = 0.0;
    g_mm.last_ms = now;

    const Game *game = &client->game;
    // settings (Minimap setScale / setZoom / setHudScale)
    mm_apply_scale((float) setting_num("minimapScale", 1.0));
    const float zoom = (float) setting_num("minimapZoom", 1.0);
    g_mm.focused_scale = MM_FOCUSED_SCALE * zoom;
    g_mm.map_scale = MM_FOCUSED_SCALE * zoom; // setZoom sets mapScale too (the constructor's 6 never survives the settings load)
    g_mm.hud_scale = MM_HUD_SCALE * (float) setting_num("miniMapHudScale", 1.0);
    (void) MM_MAP_SCALE;

    const int spectating = spectate_active();
    int enabled = game->ready && game->map && setting_on("showMinimap", 1) && !(game->mode && game->mode->config.no_minimap) &&
                  game->config.minimaps && !mm_custom_map(client);
    if (spectating && g_mm.spec_hidden) enabled = 0;
    if (enabled != g_mm.enabled || g_mm.applied != enabled) {
        g_mm.enabled = enabled;
        hud_minimap_apply_layout();
    }
    if (enabled && (g_mm.bounds_map != game->map || g_mm.bounds_map_scale != g_mm.map_scale)) mm_set_bounds(game->map);
    if (enabled) mm_update_image(game->map);

    // the overlay draws the game: not on the menu or end screen, no death screen (v9925 / v9926)
    const int dead = !spectating && (!client->me || !client->me->active);
    const int drawn = game->ready && client->in_game && !html_ui_is_menu_open() && client->net_end_seconds < 0 && !dead;
    if (enabled && drawn) mm_update(client, dt);
    dm_tick(client, dt, drawn);
}
