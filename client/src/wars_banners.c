// v10.0.3 Turf Wars map banners (client/include/wars_banners.h). Line numbers: deob-10.0.3.js.
//   the nodes   map.manager.serverNodes whose tag starts with "wars_banner" (:215043-215045), in map order; the kind
//               is tag.substring(11) (iïîìiïi, :214934-214973):
//                 ""      size [1, 3, 1]     asset 64295  decal at [0, 0.05, 0]
//                 "_jap"  size [1, 13.17, 1] asset 64301  decal at [-0.15, 1.68, 0]
//                 "_def"  size [1, 13.17, 1] asset 64303  decal at [-0.12, 1.65, 0.487]
//               all {src: "model", mSize: 6, dyn: true}, decal scale [1, 1, 1]; a tag of no kind throws (no banner
//               after it). manager.addAsset(node x / y / z, [xR, yR, zR], size, config) (:215080; addAsset :187110):
//               loadMesh of user asset <asset>/model (OBJ + model.png), its geometry re-centred in y (:233130-233134),
//               at the node's centre (addServerNode y + h / 2, :186771), rotation r, scale 6; transparent (opacity
//               undefined != 1, :187212), not casting, receiving shadows
//   the paint   iiiïìiì(clan_banner_color, asset) (:214975-214993): "s<n>" the paint shader n, else the colour; mask
//               "<asset>/model_p" (its red scales the paint); no colour: no paint
//   the decal   src "clans/<clan_id>" (the user-asset host's clans/<id>.png?v=<clan_logo>) when the clan has a logo,
//               else canvas_gen {200 x 200, the clan name, fontSize 100, noBVis, tAlign 1, yAlign 1} (:215057-215078):
//               createCanvasText's 1024 px square canvas, the name in 100 px GameFont, #000 with a #626262 1 px outline,
//               centred (:27943-27991); box-projected in the model's local space (basic.frag decal_apply)
// KrunkNative differences: a Box Office clan's logo is the brain's /api/clans/<id>/logo?v=<logo> (clan_ui.c); the
// official servers' is user-assets.krunker.io/clans/<id>.png?v=<logo>. Both come through remote_images.h: the name
// stands in until the logo arrives (and for good when it cannot be fetched, and on the web build, which has no
// image read-back). removeBanners' updateShadowMap is not needed: the banners do not cast.
#include <gl.h> // first: glad before any other GL header
#include <wars_banners.h>
#include <user_asset.h>
#include <skin_fx.h>       // the banner colour as v10's paint node
#include <canvas2d.h>
#include <map_post.h>      // prefab_canvas_end
#include <remote_images.h> // the clan logo
#include <backend.h>       // backend_url: a Box Office clan's logo
#include <krunker_net.h>   // krunker_net_active: an official server's clan
#include <asset_fetch.h>
#include <klog.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WARS_BANNERS_MAX 32
#define WARS_BANNER_SCALE 6.0f        // mSize: 6
#define WARS_BANNER_NAME_PX 1024      // createCanvasText's canvas (iîïííîï = 1024, a square)
#define WARS_BANNER_NAME_FONT 100.0   // canvas_gen.fontSize
#define WARS_BANNER_LOGO_POLL 30      // frames between two looks at a logo still downloading

typedef struct {
    const char *suffix;
    const char *asset;
    float decal[3];
} BannerKind;

static const BannerKind k_kinds[] = {
    {"", "64295", {0.0f, 0.05f, 0.0f}},
    {"_jap", "64301", {-0.15f, 1.68f, 0.0f}},
    {"_def", "64303", {-0.12f, 1.65f, 0.487f}},
};
#define WARS_BANNER_KINDS ((int) (sizeof(k_kinds) / sizeof(k_kinds[0])))

typedef struct {
    int object; // the server node's map object index
    int kind;
    Mesh *mesh;
} Banner;

static struct {
    // territorialWars.clanInfo
    int has;
    int clan_id;
    char name[32], logo[64], color[24];
    int dirty;           // setClanInfo or a map load since the banners were put up
    // the banners
    Scene *scene;
    const Map *map;
    int count;
    Banner banners[WARS_BANNERS_MAX];
    // the decal maps
    unsigned int name_texture;
    char name_drawn[32];
    unsigned int logo_texture; // load_texture's (cached by path)
    char logo_url[256];
    int logo_state;            // 0 none, 1 downloading, 2 ready, -1 failed
    int logo_wait;
} g_wb;

int wars_banner_kind(const char *tag) {
    if (!tag || strncmp(tag, "wars_banner", 11) != 0) return -1; // tag.indexOf("wars_banner") === 0
    for (int i = 0; i < WARS_BANNER_KINDS; i++) {
        if (!strcmp(tag + 11, k_kinds[i].suffix)) return i; // iïîìiïi[tag.substring(11)]
    }
    return -2;
}

static int truthy_logo(const char *logo) { // `if (clan_logo)`: String(clan_logo) on the wire
    return logo && logo[0] && strcmp(logo, "0") && strcmp(logo, "null") && strcmp(logo, "undefined") && strcmp(logo, "false");
}

void wars_banners_set(const NetWarsClanPacket *clan) {
    g_wb.has = clan && clan->has;
    g_wb.clan_id = g_wb.has ? clan->clan_id : 0;
    snprintf(g_wb.name, sizeof(g_wb.name), "%.*s", (int) sizeof(clan->clan_name), g_wb.has ? clan->clan_name : "");
    snprintf(g_wb.logo, sizeof(g_wb.logo), "%.*s", (int) sizeof(clan->clan_logo), g_wb.has ? clan->clan_logo : "");
    snprintf(g_wb.color, sizeof(g_wb.color), "%.*s", (int) sizeof(clan->banner_color), g_wb.has ? clan->banner_color : "");
    g_wb.dirty = 1;
    KN_LOG_VERBOSE2("[turf] banners: %s (clan %d, logo %s, colour %s)\n", g_wb.has ? g_wb.name : "none", g_wb.clan_id,
                    g_wb.logo[0] ? g_wb.logo : "-", g_wb.color[0] ? g_wb.color : "-");
}

static void remove_banners(void) { // removeBanners (:215013-215035)
    for (int i = 0; i < g_wb.count; i++) {
        Mesh *mesh = g_wb.banners[i].mesh;
        if (g_wb.scene) scene_remove_mesh(g_wb.scene, mesh);
        skin_fx_forget(mesh);
        mesh_fini(mesh); // the material with it; the model's textures are load_texture's
    }
    g_wb.count = 0;
}

void wars_banners_load(Scene *scene, const Map *map) {
    remove_banners();
    g_wb.scene = scene;
    g_wb.map = map;
    g_wb.dirty = 1;
}

void wars_banners_unload(void) {
    remove_banners();
    g_wb.scene = NULL;
    g_wb.map = NULL;
}

int wars_banners_count(void) { return g_wb.count; }

// utils.js getLines: words joined while measureText(line + " " + word) < max_width; *count lines into out
static int name_lines(Canvas2D *c, const char *text, const double max_width, char out[][64], const int max) {
    int count = 0;
    char line[64] = "";
    int has_line = 0;
    const char *p = text;
    for (;;) {
        const char *space = strchr(p, ' ');
        const size_t n = space ? (size_t) (space - p) : strlen(p);
        char word[64];
        snprintf(word, sizeof(word), "%.*s", (int) n, p);
        if (!has_line) {
            snprintf(line, sizeof(line), "%s", word);
            has_line = 1;
        } else {
            char joined[130];
            snprintf(joined, sizeof(joined), "%s %s", line, word);
            if (c2d_measure_text(c, joined) < max_width) {
                snprintf(line, sizeof(line), "%s", joined);
            } else {
                if (count < max) snprintf(out[count++], 64, "%s", line);
                snprintf(line, sizeof(line), "%s", word);
            }
        }
        if (!space) break;
        p = space + 1;
    }
    if (has_line && count < max) snprintf(out[count++], 64, "%s", line);
    return count;
}

// createCanvasText(400, 400, name, 100, "#000", "#fff", noBVis, 1, "#626262", undefined, false, {yAlign: 1})
static unsigned int name_texture(const char *name) {
    if (g_wb.name_texture && !strcmp(g_wb.name_drawn, name)) return g_wb.name_texture;
    Canvas2D *c = c2d_create();
    if (!c) return 0;
    const int px = WARS_BANNER_NAME_PX;
    c2d_set_size(c, px, px, px, px);
    c2d_set_font(c, WARS_BANNER_NAME_FONT, "GameFont");
    c2d_set_text_align(c, C2D_ALIGN_CENTER);
    const double line_h = c2d_measure_text(c, "M") * 1.2;
    // text.split("\\n") (a literal backslash-n), each segment wrapped at width - fontSize
    char lines[16][64];
    int count = 0;
    char text[64];
    snprintf(text, sizeof(text), "%s", name);
    for (char *segment = text; segment && count < 16;) {
        char *split = strstr(segment, "\\n");
        if (split) *split = 0;
        count += name_lines(c, segment, px - WARS_BANNER_NAME_FONT, lines + count, 16 - count);
        segment = split ? split + 2 : NULL;
    }
    double y = (px - count * line_h) / 2.0 + line_h; // yAlign 1
    for (int i = 0; i < count && y <= px; i++, y += line_h) prefab_canvas_text(c, lines[i], "#000", px / 2.0, y, NULL, 0.0, "#626262", 1.0);
    const unsigned int texture = prefab_canvas_end(c, NULL);
    c2d_destroy(c);
    if (!texture) return 0;
    glBindTexture(GL_TEXTURE_2D, texture); // a three.js Texture's defaults: clamped, linear
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glGenerateMipmap(GL_TEXTURE_2D);
    if (g_wb.name_texture) {
        gl_release_texture(g_wb.name_texture);
        glDeleteTextures(1, &g_wb.name_texture);
        if (g_active_texture == g_wb.name_texture) g_active_texture = 0;
    }
    g_active_texture = texture;
    g_wb.name_texture = texture;
    snprintf(g_wb.name_drawn, sizeof(g_wb.name_drawn), "%s", name);
    return texture;
}

// The logo's URL (KrunkNative: the brain's for a Box Office clan, Krunker's user-asset host for an official one)
static int logo_url(char *out, const size_t n) {
    if (!truthy_logo(g_wb.logo) || g_wb.clan_id <= 0) return 0;
    if (krunker_net_active()) {
        snprintf(out, n, "https://user-assets.krunker.io/clans/%d.png?v=%s", g_wb.clan_id, g_wb.logo);
        return 1;
    }
    const char *base = backend_url();
    if (!base || !base[0]) return 0;
    snprintf(out, n, "%s/api/clans/%d/logo?v=%s", base, g_wb.clan_id, g_wb.logo);
    return 1;
}

static void logo_poll(void) {
    char url[256];
    if (!logo_url(url, sizeof(url))) {
        g_wb.logo_state = 0;
        g_wb.logo_url[0] = 0;
        return;
    }
    if (strcmp(url, g_wb.logo_url) != 0) { // another clan / logo version
        snprintf(g_wb.logo_url, sizeof(g_wb.logo_url), "%s", url);
        g_wb.logo_state = 1;
        g_wb.logo_texture = 0;
        g_wb.logo_wait = 0;
    }
    if (g_wb.logo_state != 1 || g_wb.logo_wait-- > 0) return;
    g_wb.logo_wait = WARS_BANNER_LOGO_POLL;
    char path[1200];
    const int got = remote_images_fetch(url, path, sizeof(path));
    if (got > 0) {
        g_wb.logo_texture = load_texture(path);
        g_wb.logo_state = g_wb.logo_texture ? 2 : -1;
        KN_LOG_VERBOSE2("[turf] clan logo %s: %s\n", url, g_wb.logo_texture ? "loaded" : "unreadable");
    } else if (got < 0) {
        g_wb.logo_state = -1;
        KN_LOG_VERBOSE2("[turf] clan logo %s: not fetched, the name stands in\n", url);
    }
}

static unsigned int decal_texture(void) {
    return g_wb.logo_state == 2 && g_wb.logo_texture ? g_wb.logo_texture : name_texture(g_wb.name);
}

// "#rrggbb" -> its paint; "s<n>" -> the paint shader n; anything else that is no colour: none
static WornSkinPaint banner_paint(const char *css) {
    WornSkinPaint paint = {.color = -1, .shader = -1};
    if (!css || !css[0]) return paint; // `if (!iiÍíîii) return {}`
    if (css[0] == 's') { // {shader: parseInt(slice(1), 10) || 0, intensity: 1}
        paint.shader = (int) strtol(css + 1, NULL, 10);
        return paint;
    }
    float rgba[4];
    if (c2d_parse_color(css, rgba)) {
        paint.color = ((int) lroundf(rgba[0] * 255.0f) << 16) | ((int) lroundf(rgba[1] * 255.0f) << 8) | (int) lroundf(rgba[2] * 255.0f);
    } else {
        paint.color = 0xFFFFFF; // THREE.Color of a string it does not read keeps white
    }
    return paint;
}

static unsigned int mask_texture(const char *asset) {
    char rel[64];
    snprintf(rel, sizeof(rel), "user-assets/%s/model_p.png", asset); // paint.mask {src: "<asset>/model_p", asset: true}
    char *path = concat(assets_path(), rel);
    if (!path) return 0;
    FILE *f = ensure_asset_file(path) ? fopen(path, "rb") : NULL;
    unsigned int texture = 0;
    if (f) {
        fclose(f);
        texture = load_texture(path);
    } else if (asset_prefetch(path) == 0) {
        texture = load_texture(path); // its placeholder, filled when the file lands
    }
    free(path);
    return texture;
}

// loadMesh's place: the node's centre, the geometry's box centre (y) moved onto it
static void place(Banner *b) {
    const Object *node = g_wb.map->objects[b->object];
    Mesh *mesh = b->mesh;
    const vec3 r = node->rotation; // [xR, yR, zR] = r (:188820-188823)
    const float cx = cosf(r.x), sx = sinf(r.x), cy = cosf(r.y), sy = sinf(r.y), cz = cosf(r.z), sz = sinf(r.z);
    const vec3 up = {-cy * sz, cx * cz - sx * sy * sz, sx * cz + cx * sy * sz}; // the second column of Rx * Ry * Rz
    const float centre_y = mesh->geometry ? mesh->geometry->bound_center[1] : 0.0f;
    const float off = centre_y * WARS_BANNER_SCALE;
    const vec3 at = {node->position.x, node->position.y + node->scale.y / 2.0f, node->position.z};
    mesh->transform.position = (vec3) {at.x - up.x * off, at.y - up.y * off, at.z - up.z * off};
    mesh->transform.rotation = r;
    mesh->transform.scale = (vec3) {WARS_BANNER_SCALE, WARS_BANNER_SCALE, WARS_BANNER_SCALE};
    BasicMaterial *material = (BasicMaterial *) mesh->material;
    const float *d = k_kinds[b->kind].decal;
    // krLocalPos is the re-centred geometry's: here the geometry keeps its own y, so the box moves up by its centre
    material->decal_position = (vec3) {d[0], d[1] + centre_y, d[2]};
    material->decal_half_scale = (vec3) {0.5f, 0.5f, 0.5f}; // (scale || 1) * 0.5, z (scale[2] || 0.1) * 0.5 (:232240)
}

static void build(void) {
    remove_banners();
    g_wb.dirty = 0;
    if (!g_wb.has || !g_wb.scene || !g_wb.map) return; // `this.removeBanners(), clanInfo` (:215042)
    const Map *map = g_wb.map;
    const WornSkinPaint paint = banner_paint(g_wb.color);
    for (size_t i = 0; i < map->server_node_count && g_wb.count < WARS_BANNERS_MAX; i++) {
        const size_t index = map->server_nodes[i];
        if (index >= map->object_count) continue;
        const int kind = wars_banner_kind(map->objects[index]->node_tag);
        if (kind == -1) continue;
        if (kind == -2) { // iîÏïiíì.config of undefined: setClanInfo throws, the banners so far stay
            KN_LOG_VERBOSE2("[turf] node %zu: tag \"%s\" names no banner (v10 stops there)\n", index, map->objects[index]->node_tag);
            break;
        }
        UserAssetFlags flags;
        UserAssetModel model;
        if (!user_asset_parse(k_kinds[kind].asset, &flags) || !user_asset_load(&flags, &model) || !model.mesh) continue;
        Mesh *mesh = model.mesh;
        BasicMaterial *material = (BasicMaterial *) mesh->material;
        material->base.transparent = 1; // fTrans || transparent || opacity != 1 with opacity undefined (:187212)
        material->double_sided = 0;     // side: limitMM(parseInt(undefined) || 0, 0, 2) = FrontSide
        if (paint.color >= 0 || paint.shader >= 0) {
            skin_fx_apply_paint(mesh, paint, NULL);
            material->paint_mask_texture = mask_texture(k_kinds[kind].asset);
        }
        material->decal_texture = decal_texture();
        Banner *b = &g_wb.banners[g_wb.count++];
        *b = (Banner) {.object = (int) index, .kind = kind, .mesh = mesh};
        place(b);
        mesh->visible = 1;
        scene_add_mesh(g_wb.scene, mesh);
    }
    KN_LOG_VERBOSE2("[turf] %d banner(s) for %s\n", g_wb.count, g_wb.name);
}

void wars_banners_frame(Client *client) {
    (void) client;
    if (!g_wb.scene || !g_wb.map) return;
    if (g_wb.has) logo_poll();
    if (g_wb.dirty) build();
    const unsigned int decal = g_wb.count ? decal_texture() : 0;
    for (int i = 0; i < g_wb.count; i++) {
        place(&g_wb.banners[i]); // a model still downloading is filled in place (asset_fetch.h): its box centre moves
        ((BasicMaterial *) g_wb.banners[i].mesh->material)->decal_texture = decal;
    }
}
