// GAME.SCENE (doc/v7/krunkscript.md 3.1.17, 3.2.4, 3.2.5, 3.3.1, 3.3.2, 3.3.4; v7
// createKrunkScriptApiContext.js:667-1027, 1202-1303, 2581-3088): client-side visual objects
// in the client's world Scene, the obj3D and bone objects, lights, signs and the scene-wide
// lighting / fog / sky calls. Objects are local and visual only (no collision, not networked)
// and leave the scene with the script.
//
// Geometry follows v7's generators (src/libs/geos.js): unit cube with its base at y = 0 scaled
// by (w, h, l); SphereGeometry(1, 12, 8) scaled by (w, h, l) (radius = w); PlaneGeometry(1, 1)
// upright in XY scaled by (w, l); ConeGeometry(1, 1, sides, open) lifted by 0.5 for light cones.
// Materials are BasicMaterial with the Krunker getColor saturation boost (geos.js getColor).
//
// Not drawn by the KN renderer yet (7 #18): spot / directional / rect-area lights (and point
// lights until the renderer's runtime light list exists, BASIC_HAVE_LIGHT_LIST): placeholder
// objects whose members all work, one warning per process. FBX models load nothing (warned).
#include <gl.h>
#include "ks_client_internal.h"
#include <ks_client.h>
#include <ks_overlay.h>
#include <gltf.h>
#include <sky.h>
#include <render_settings.h>
#include <player_pose_render.h> // attachTo's "hands" / "handL" points (player.pose hold / holdL)
#include <player_class_model.h> // player.playAnim / stopAnim: the class model
#include <ui_logic.h>
#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    KSO_MESH = 1,   // cube, plane, sphere, custom, light bar, light cone, sign
    KSO_SPRITE,     // sprite / mimicSprite: faces the camera
    KSO_MODEL,      // addAsset (GLTF / GLB / OBJ)
    KSO_LIGHT,      // point / spot / dir / rect light
    KSO_PLAYER_ASSET, // player.getAsset(): a class model's GLTF root, borrowed (player_class_model.h)
};
enum { KSL_POINT = 1, KSL_SPOT, KSL_DIR, KSL_RECT };
enum { KSA_NONE, KSA_PLAYER, KSA_OBJECT, KSA_BONE, KSA_CAMERA };
// attachTo(player, ..., point): the model's anchor, or v10's getHands ("hands", player.pose.hold) /
// getHandL ("handL", v10.0.3 player.pose.holdL) anchors (deob-10.0.3.js:208838-208842)
enum { KSP_ANCHOR, KSP_HANDS, KSP_HAND_L };

typedef struct KsObj3D {
    uint32_t id;
    int kind;
    MeshTransform xf;          // the object (position / rotation / scale)
    MeshTransform pivot;       // attachTo's "attachTo" pivot
    int in_scene;              // RENDER.scene membership (delete() removes it)
    int visible;
    // meshes
    Mesh *mesh;
    unsigned int own_vbo;      // addCustom: geometry owned by the object
    Geometry *own_geometry;
    GltfInstance *model;
    int is_model, model_failed;
    int asset;                 // texture / model asset id (0 none)
    float opacity;
    float anim_speed;
    float tex_move;            // textureMoveSpeed / 10000 (per ms), 0 none
    int tex_move_axis;
    int frames;                // textureFrameCount
    float frame_ms;            // textureFrameDuration * 100
    double tex_time;
    // lights
    int light;
    vec3 light_color;
    float intensity, distance, decay, angle, penumbra, width, height;
    vec3 target;
    int light_handle;
    // attachTo
    int attach;
    uint32_t attach_id;
    int attach_uid;
    int attach_point;          // KSP_*: attachTo's 6th argument on a player
    int fps_layer;             // drawn by the viewmodel pass (fps_scene): v10's layers.mask copied from a first-person "handL"
    // KSO_PLAYER_ASSET: the class model (player uid, PlayerClassModel.serial) whose GLTF root it is; deleted: delete()
    // took the root off the holder (unseen); unparented: detach() put it in the world
    int asset_uid;
    unsigned asset_serial;
    unsigned char asset_deleted, asset_unparented;
} KsObj3D;

typedef struct {
    uint32_t id;               // bone object id
    uint32_t owner;            // obj3D id of the model
    char name[64];
} KsBone;

static struct {
    KsObj3D **objs;
    int count, cap;
    KsBone *bones;
    int bone_count, bone_cap;
    uint32_t next_id;
    MeshTransform camera_xf;   // the camera as an attach target (CAMERA.getObj)
    int sky_changed;
    double last_tex_ms;
} g_sc;

static float world_uv(void) { return game_constants.world_uv_scale > 0 ? game_constants.world_uv_scale : 60.0f; }

static uint32_t new_id(void) {
    if (g_sc.next_id < 1000) g_sc.next_id = 1000;
    return ++g_sc.next_id;
}

// ---------------------------------------------------------------------------
// Colours (geos.js getColor: HSL saturation * 1.5)

static float hue_to_rgb(const float p, const float q, float t) {
    if (t < 0.0f) t += 1.0f;
    if (t > 1.0f) t -= 1.0f;
    if (t < 1.0f / 6.0f) return p + (q - p) * 6.0f * t;
    if (t < 0.5f) return q;
    if (t < 2.0f / 3.0f) return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
    return p;
}

static vec4 krunker_color(const vec4 c) {
    const float r = c.x, g = c.y, b = c.z;
    const float max = fmaxf(r, fmaxf(g, b)), min = fminf(r, fminf(g, b));
    const float l = (min + max) * 0.5f;
    float h = 0.0f, s = 0.0f;
    if (max != min) {
        const float d = max - min;
        s = l <= 0.5f ? d / (max + min) : d / (2.0f - max - min);
        if (max == r) h = (g - b) / d + (g < b ? 6.0f : 0.0f);
        else if (max == g) h = (b - r) / d + 2.0f;
        else h = (r - g) / d + 4.0f;
        h /= 6.0f;
    }
    s = fminf(1.0f, s * 1.5f);
    vec4 out = {l, l, l, c.w};
    if (s > 0.0f) {
        const float q = l <= 0.5f ? l * (1.0f + s) : l + s - l * s;
        const float p = 2.0f * l - q;
        out.x = hue_to_rgb(p, q, h + 1.0f / 3.0f);
        out.y = hue_to_rgb(p, q, h);
        out.z = hue_to_rgb(p, q, h - 1.0f / 3.0f);
    }
    return out;
}

// colr argument: isHexColor ? colr : fallback
static vec4 arg_color(KsScript *s, const KsValue v, const char *fallback) {
    vec4 c = {1, 1, 1, 1};
    char buf[16];
    if (!ksc_is_hex(v) || !ksc_parse_hex(ksc_str(s, v, buf, sizeof(buf)), &c)) ksc_parse_hex(fallback, &c);
    return c;
}

// ---------------------------------------------------------------------------
// Geometry

static Geometry *geometry_build(const vertex *v, const size_t nv, const unsigned int *idx, const size_t ni, unsigned int *vbo_out) {
    Geometry *geometry = calloc(1, sizeof(Geometry));
    if (!geometry) return NULL;
    unsigned int vbo;
    glGenVertexArrays(1, &geometry->vao);
    glGenBuffers(1, &geometry->ebo);
    glGenBuffers(1, &vbo);
    glBindVertexArray(geometry->vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr) (nv * sizeof(vertex)), v, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, geometry->ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr) (ni * sizeof(unsigned int)), idx, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(vertex), NULL);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(vertex), (void *) (3 * sizeof(float)));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);
    geometry->index_count = (int) ni;
    geometry_set_bounds(geometry, v, nv);
    if (vbo_out) *vbo_out = vbo;
    return geometry;
}

static void geometry_free(Geometry *geometry, const unsigned int vbo) {
    if (!geometry) return;
    glDeleteBuffers(1, &vbo);
    glDeleteBuffers(1, &geometry->ebo);
    glDeleteVertexArrays(1, &geometry->vao);
    free(geometry);
}

// THREE.PlaneGeometry(1, 1): upright in XY, centred, facing +Z
static Geometry *plane_geometry(void) {
    static Geometry *g;
    if (g) return g;
    static const vertex v[] = {{{-0.5f, 0.5f, 0.0f}, {0, 1}}, {{0.5f, 0.5f, 0.0f}, {1, 1}}, {{-0.5f, -0.5f, 0.0f}, {0, 0}}, {{0.5f, -0.5f, 0.0f}, {1, 0}}};
    static const unsigned int i[] = {0, 2, 1, 2, 3, 1};
    g = geometry_build(v, 4, i, 6, NULL);
    return g;
}

// THREE.SphereGeometry(1, 12, 8)
static Geometry *sphere_geometry(void) {
    static Geometry *g;
    if (g) return g;
    enum { WS = 12, HS = 8 };
    vertex v[(WS + 1) * (HS + 1)];
    unsigned int idx[WS * HS * 6];
    size_t nv = 0, ni = 0;
    for (int y = 0; y <= HS; y++) {
        const float vv = (float) y / HS;
        for (int x = 0; x <= WS; x++) {
            const float u = (float) x / WS;
            const float phi = u * 2.0f * (float) M_PI, theta = vv * (float) M_PI;
            v[nv++] = (vertex) {{-cosf(phi) * sinf(theta), cosf(theta), sinf(phi) * sinf(theta)}, {u, 1.0f - vv}};
        }
    }
    for (int y = 0; y < HS; y++) {
        for (int x = 0; x < WS; x++) {
            const unsigned int a = (unsigned) (y * (WS + 1) + x + 1), b = (unsigned) (y * (WS + 1) + x);
            const unsigned int c = (unsigned) ((y + 1) * (WS + 1) + x), d = (unsigned) ((y + 1) * (WS + 1) + x + 1);
            if (y != 0) { idx[ni++] = a; idx[ni++] = b; idx[ni++] = d; }
            if (y != HS - 1) { idx[ni++] = b; idx[ni++] = c; idx[ni++] = d; }
        }
    }
    g = geometry_build(v, nv, idx, ni, NULL);
    return g;
}

// ConeGeometry(1, 1, sides, 1, open, PI / 4, theta) translated up by 0.5 (base at y = 0)
static Geometry *cone_geometry(const int sides, const float theta, unsigned int *vbo) {
    vertex v[2 * 65 + 2];
    unsigned int idx[64 * 6];
    size_t nv = 0, ni = 0;
    for (int i = 0; i <= sides; i++) {
        const float u = (float) i / (float) sides;
        const float a = (float) M_PI / 4.0f + u * theta;
        v[nv++] = (vertex) {{0.0f, 1.0f, 0.0f}, {u, 1.0f}};
        v[nv++] = (vertex) {{sinf(a), 0.0f, cosf(a)}, {u, 0.0f}};
    }
    for (int i = 0; i < sides; i++) {
        const unsigned int t = (unsigned) (i * 2), b = t + 1, b2 = t + 3;
        idx[ni++] = t; idx[ni++] = b; idx[ni++] = b2;
    }
    return geometry_build(v, nv, idx, ni, vbo);
}

// ---------------------------------------------------------------------------
// Objects

static KsObj3D *obj_find(const uint32_t id) {
    for (int i = 0; i < g_sc.count; i++) {
        if (g_sc.objs[i]->id == id) return g_sc.objs[i];
    }
    return NULL;
}

// KSO_PLAYER_ASSET: the class model it is a handle of, while that model is drawn (NULL once rebuilt as another model)
static PlayerClassModel *obj_borrowed(const KsObj3D *o) {
    if (o->kind != KSO_PLAYER_ASSET || !g_ksc.client) return NULL;
    PlayerClassModel *m = player_class_model_find(&g_ksc.client->game, o->asset_uid, o->asset_serial);
    return m && m->gltf ? m : NULL;
}

// The GLTF a model object animates (its own) or a player asset's bones live in (the class model's)
static GltfInstance *obj_gltf(const KsObj3D *o) {
    if (o->model) return o->model;
    const PlayerClassModel *m = obj_borrowed(o);
    return m ? m->gltf : NULL;
}

static KsObj3D *obj_new(const int kind) {
    if (g_sc.count == g_sc.cap) {
        const int cap = g_sc.cap ? g_sc.cap * 2 : 64;
        KsObj3D **grown = realloc(g_sc.objs, (size_t) cap * sizeof(KsObj3D *));
        if (!grown) return NULL;
        g_sc.objs = grown;
        g_sc.cap = cap;
    }
    KsObj3D *o = calloc(1, sizeof(KsObj3D));
    if (!o) return NULL;
    o->id = new_id();
    o->kind = kind;
    o->xf.scale = (vec3) {1, 1, 1};
    o->pivot.scale = (vec3) {1, 1, 1};
    o->visible = 1;
    o->opacity = 1.0f;
    o->anim_speed = 1.0f;
    g_sc.objs[g_sc.count++] = o;
    return o;
}

static Scene *world_scene(void) { return g_ksc.client ? g_ksc.client->scene : NULL; }
// RENDER.scene, or the layer-1 objects the fpsCamera draws (KN's viewmodel scene)
static Scene *obj_scene(const KsObj3D *o) { return o->fps_layer && g_ksc.client ? g_ksc.client->fps_scene : world_scene(); }

static void obj_add_to_scene(KsObj3D *o) {
    Scene *scene = obj_scene(o);
    if (!scene || o->in_scene) return;
    if (o->mesh) scene_add_mesh(scene, o->mesh);
    if (o->model) scene_add_gltf(scene, o->model);
    o->in_scene = 1;
}

static void obj_remove_from_scene(KsObj3D *o) {
    Scene *scene = obj_scene(o);
    if (!o->in_scene) return;
    if (scene && o->mesh) scene_remove_mesh(scene, o->mesh);
    if (scene && o->model) scene_remove_gltf(scene, o->model);
    o->in_scene = 0;
}

static void obj_free(KsObj3D *o) {
    PlayerClassModel *borrowed = obj_borrowed(o);
    if (borrowed) { // the class model is the player's: back on its holder, shown
        borrowed->gltf->root.parent = &borrowed->holder;
        gltf_instance_set_visible(borrowed->gltf, 1);
    }
    obj_remove_from_scene(o);
    if (o->mesh) mesh_fini(o->mesh); // frees the material, keeps the geometry
    if (o->own_geometry) geometry_free(o->own_geometry, o->own_vbo);
    if (o->model) gltf_instance_fini(o->model);
#ifdef BASIC_HAVE_LIGHT_LIST
    if (o->light_handle) basic_lights_remove(o->light_handle);
#endif
    free(o);
}

// The world position of an object (its whole parent chain).
static vec3 obj_world_position(const MeshTransform *xf) {
    Mesh tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.transform.scale = (vec3) {1, 1, 1};
    tmp.transform.parent = (MeshTransform *) xf;
    mesh_update_transform_matrix(&tmp);
    return (vec3) {tmp.transform_matrix[3], tmp.transform_matrix[7], tmp.transform_matrix[11]};
}

static unsigned int asset_texture(const int asset) {
    if (asset <= 0) return 0;
    char rel[64];
    snprintf(rel, sizeof(rel), "user-assets/%d/texture.png", asset);
    char *path = concat(assets_path(), rel);
    unsigned int tex = 0;
    if (path) {
        static int failed[64], failed_count;
        for (int i = 0; i < failed_count; i++) {
            if (failed[i] == asset) {
                free(path);
                return texture_checker(); // given up before: the checker again (load_texture does not cache failures)
            }
        }
        tex = load_texture(path); // the checker when the file does not load
        if (tex == texture_checker() && failed_count < 64) failed[failed_count++] = asset;
        free(path);
    }
    return tex;
}

// vO445.obj (API:1202-1284): the option object of cubes / planes / spheres / sprites / custom
// meshes / light bars / cones, applied to a fresh BasicMaterial.
typedef struct {
    int stretch;            // textureStretching
    float scale_m;          // textureScale
    float move;             // textureMoveSpeed / 10000
    int move_axis;
    vec2 offset;            // textureOffsetX / Y
    int frames;
    float frame_ms;
    int has_emissive;
    vec4 emissive;
    int asset;              // aid: digits (video textures are not supported)
    int mimic_sprite;
    float opacity;
    int transparent;
    int side;               // 0 front, 1 back, 2 double
} ObjOptions;

static ObjOptions parse_options(KsScript *s, const KsValue data, const KsValue aid, const int side_default) {
    ObjOptions o = {0};
    o.opacity = 1.0f;
    o.side = side_default;
    o.stretch = ksc_truthy(ksc_opt(s, data, "textureStretching"));
    o.scale_m = (float) ksc_float(ksc_opt(s, data, "textureScale"));
    const KsValue mv = ksc_opt(s, data, "textureMoveSpeed");
    if (ksc_is_num(mv)) o.move = (float) (mv.as.num / 10000.0);
    const KsValue axis = ksc_opt(s, data, "textureMoveAxis");
    char buf[64];
    if (axis.tag == KS_STR) {
        ksc_str(s, axis, buf, sizeof(buf));
        o.move_axis = !strcmp(buf, "y") ? 1 : !strcmp(buf, "x") ? 0 : (atoi(buf) ? 1 : 0);
    } else {
        o.move_axis = ksc_clamp((int) ksc_float(axis), 0, 1) ? 1 : 0;
    }
    const KsValue ox = ksc_opt(s, data, "textureOffsetX"), oy = ksc_opt(s, data, "textureOffsetY");
    o.offset.x = (float) ksc_clamp(ksc_is_num(ox) ? ox.as.num : 0, 0, 1);
    o.offset.y = (float) ksc_clamp(ksc_is_num(oy) ? oy.as.num : 0, 0, 1);
    o.frames = (int) ksc_clamp((int) ksc_float(ksc_opt(s, data, "textureFrameCount")), 0, 100);
    o.frame_ms = (float) (ksc_clamp(ksc_float(ksc_opt(s, data, "textureFrameDuration")), 0, 10) * 100.0);
    const KsValue em = ksc_opt(s, data, "emissive");
    if (ksc_is_hex(em)) {
        o.has_emissive = 1;
        o.emissive = arg_color(s, em, "#000");
    }
    // aid: parseInt(aid); a "v" selects a video texture (not drawn: KN has no video textures)
    ksc_str(s, aid, buf, sizeof(buf));
    const long id = strtol(buf, NULL, 10);
    if (id > 0 && !strchr(buf, 'v')) o.asset = (int) id;
    o.mimic_sprite = ksc_truthy(ksc_opt(s, data, "mimicSprite"));
    const KsValue op = ksc_opt(s, data, "opacity");
    o.opacity = ksc_is_num(op) ? (float) op.as.num : 1.0f;
    o.transparent = ksc_truthy(ksc_opt(s, data, "forceTransparency")) || o.opacity != 1.0f;
    const KsValue side = ksc_opt(s, data, "side");
    if (ksc_is_num(side)) o.side = (int) ksc_clamp(side.as.num, 0, 2);
    if (ksc_truthy(ksc_opt(s, data, "doubleSided"))) o.side = 2;
    if (ksc_truthy(ksc_opt(s, data, "backSided"))) o.side = 1;
    if (ksc_truthy(ksc_opt(s, data, "frontSided"))) o.side = 0;
    return o;
}

static BasicMaterial *make_material(const vec4 color, const ObjOptions *opt) {
    BasicMaterial *m = basic_material_init();
    if (!m) return NULL;
    m->color = krunker_color(color);
    m->color.w = opt->opacity;
    m->color_bottom = m->color;
    m->texture = asset_texture(opt->asset);
    m->texture_base_offset = opt->offset;
    m->base.transparent = opt->transparent;
    m->double_sided = opt->side != 0; // back-only renders both faces (no front-face cull mode)
    if (opt->has_emissive) m->emissive = opt->emissive;
    return m;
}

// The new obj3D in the scene, its wrapper returned.
static KsValue obj_value(KsScript *s, KsObj3D *o);

static KsObj3D *mesh_object(Geometry *geometry, BasicMaterial *material, const int kind) {
    if (!geometry || !material) return NULL;
    KsObj3D *o = obj_new(kind);
    if (!o) {
        material_fini((Material *) material);
        return NULL;
    }
    o->mesh = mesh_init(geometry, (Material *) material);
    o->mesh->transform.parent = &o->xf;
    return o;
}

static void obj_set_options(KsObj3D *o, const ObjOptions *opt) {
    o->asset = opt->asset;
    o->opacity = opt->opacity;
    o->tex_move = opt->move;
    o->tex_move_axis = opt->move_axis;
    o->frames = opt->frames;
    o->frame_ms = opt->frame_ms;
}

#define NUM(i, d) ((float) ksc_num_or(ksc_arg(a, n, (i)), (d)))

// addCube(aid, colr, x, y, z, w, h, l, data)
static KsValue sc_add_cube(KsScript *s, void *u, const KsValue *a, int n) {
    (void) u;
    const vec4 color = arg_color(s, ksc_arg(a, n, 1), "#FFF");
    const float w = NUM(5, 1), h = NUM(6, 1), l = NUM(7, 1);
    const ObjOptions opt = parse_options(s, ksc_arg(a, n, 8), ksc_arg(a, n, 0), 0);
    BasicMaterial *m = make_material(color, &opt);
    if (!m) return ks_undef();
    // geos.js generateCube: per-face UVs scaled by size / worldUV * (textureScale || 1); 0 = stretched
    const float sc = opt.stretch ? 0.0f : (opt.scale_m ? opt.scale_m : 1.0f);
    m->uv_mode = BASIC_UV_CUBE;
    m->uv_size = sc ? (vec3) {w / world_uv() * sc, h / world_uv() * sc, l / world_uv() * sc} : (vec3) {1, 1, 1};
    m->double_sided = opt.side != 0;
    KsObj3D *o = mesh_object(create_cube_geo(), m, KSO_MESH);
    if (!o) return ks_undef();
    obj_set_options(o, &opt);
    o->xf.position = (vec3) {NUM(2, 0), NUM(3, 0), NUM(4, 0)};
    o->xf.scale = (vec3) {w, h, l};
    obj_add_to_scene(o);
    return obj_value(s, o);
}

// upright plane / sprite / sign / light bar body: scale (w, l, 1)
static KsObj3D *plane_object(KsScript *s, const vec4 color, const ObjOptions *opt, const float w, const float l, const float x,
                             const float y, const float z, const int sprite) {
    BasicMaterial *m = make_material(color, opt);
    if (!m) return NULL;
    // generatePlane: UV repeat w / worldUV, l / worldUV times the texture scale (0: stretched)
    const float sc = opt->stretch ? 0.0f : (opt->scale_m ? opt->scale_m : 1.0f);
    if (sc && !sprite) m->texture_repeat = (vec2) {w / world_uv() * sc, l / world_uv() * sc};
    (void) s;
    KsObj3D *o = mesh_object(plane_geometry(), m, sprite ? KSO_SPRITE : KSO_MESH);
    if (!o) return NULL;
    obj_set_options(o, opt);
    o->xf.position = (vec3) {x, y, z};
    o->xf.scale = (vec3) {w, l, 1.0f};
    obj_add_to_scene(o);
    return o;
}

// addPlane(aid, colr, x, y, z, w, l, data): double-sided by default
static KsValue sc_add_plane(KsScript *s, void *u, const KsValue *a, int n) {
    (void) u;
    const vec4 color = arg_color(s, ksc_arg(a, n, 1), "#FFF");
    ObjOptions opt = parse_options(s, ksc_arg(a, n, 7), ksc_arg(a, n, 0), 2);
    vec4 c = color;
    if (opt.mimic_sprite) c = (vec4) {1, 1, 1, 1}; // mimicSprite: colour #FFFFFF, SpriteMaterial
    KsObj3D *o = plane_object(s, c, &opt, NUM(5, 1), NUM(6, 1), NUM(2, 0), NUM(3, 0), NUM(4, 0), opt.mimic_sprite);
    return o ? obj_value(s, o) : ks_undef();
}

// addSprite(aid, colr, x, y, z, w, h, l, data): camera-facing, scale (w, l)
static KsValue sc_add_sprite(KsScript *s, void *u, const KsValue *a, int n) {
    (void) u;
    const vec4 color = arg_color(s, ksc_arg(a, n, 1), "#FFF");
    ObjOptions opt = parse_options(s, ksc_arg(a, n, 8), ksc_arg(a, n, 0), 2);
    KsObj3D *o = plane_object(s, color, &opt, NUM(5, 1), NUM(7, 1), NUM(2, 0), NUM(3, 0), NUM(4, 0), 1);
    return o ? obj_value(s, o) : ks_undef();
}

// addSphere(aid, colr, x, y, z, w, h, l, data): placed at y + h / 2, scale (w, h, l)
static KsValue sc_add_sphere(KsScript *s, void *u, const KsValue *a, int n) {
    (void) u;
    const vec4 color = arg_color(s, ksc_arg(a, n, 1), "#FFF");
    const ObjOptions opt = parse_options(s, ksc_arg(a, n, 8), ksc_arg(a, n, 0), 2);
    BasicMaterial *m = make_material(color, &opt);
    if (!m) return ks_undef();
    KsObj3D *o = mesh_object(sphere_geometry(), m, KSO_MESH);
    if (!o) return ks_undef();
    obj_set_options(o, &opt);
    const float h = NUM(6, 1);
    o->xf.position = (vec3) {NUM(2, 0), NUM(3, 0) + h / 2.0f, NUM(4, 0)};
    o->xf.scale = (vec3) {NUM(5, 1), h, NUM(7, 1)};
    obj_add_to_scene(o);
    return obj_value(s, o);
}

// addCustom(aid, colr, verts, x, y, z, w, h, l, data): a triangle list [x, y, z, ...]
static KsValue sc_add_custom(KsScript *s, void *u, const KsValue *a, int n) {
    (void) u;
    const KsValue verts = ksc_arg(a, n, 2);
    const size_t count = ks_arr_len(verts);
    if (verts.tag != KS_ARR || !count) return ks_undef();
    float *xyz = malloc(count * sizeof(float));
    if (!xyz) return ks_undef();
    size_t k = 0;
    for (size_t i = 0; i < count; i++) {
        const double d = ks_to_number(ks_arr_get(s, verts, i)); // map(Number).filter(isNumber)
        if (isfinite(d)) xyz[k++] = (float) d;
    }
    const size_t nv = k / 3 - (k / 3) % 3;
    if (!nv) {
        free(xyz);
        return ks_undef();
    }
    vertex *v = malloc(nv * sizeof(vertex));
    unsigned int *idx = malloc(nv * sizeof(unsigned int));
    if (!v || !idx) {
        free(xyz);
        free(v);
        free(idx);
        return ks_undef();
    }
    for (size_t i = 0; i < nv; i++) {
        v[i] = (vertex) {{xyz[i * 3], xyz[i * 3 + 1], xyz[i * 3 + 2]}, {0, 0}};
        idx[i] = (unsigned int) i;
    }
    free(xyz);
    unsigned int vbo = 0;
    Geometry *geometry = geometry_build(v, nv, idx, nv, &vbo);
    free(v);
    free(idx);
    const vec4 color = arg_color(s, ksc_arg(a, n, 1), "#FFF");
    const ObjOptions opt = parse_options(s, ksc_arg(a, n, 9), ksc_arg(a, n, 0), 0);
    BasicMaterial *m = make_material(color, &opt);
    KsObj3D *o = m ? mesh_object(geometry, m, KSO_MESH) : NULL;
    if (!o) {
        geometry_free(geometry, vbo);
        return ks_undef();
    }
    o->own_geometry = geometry;
    o->own_vbo = vbo;
    obj_set_options(o, &opt);
    o->xf.position = (vec3) {NUM(3, 0), NUM(4, 0), NUM(5, 0)};
    o->xf.scale = (vec3) {NUM(6, 1), NUM(7, 1), NUM(8, 1)};
    obj_add_to_scene(o);
    return obj_value(s, o);
}

// addAsset(assetID, x, y, z, scale = 1, colr = "#FFF", data, onLoad): letters in the id pick
// the format (g GLTF, q GLB, f FBX, none OBJ) and material options (3.3.4).
static KsValue sc_add_asset(KsScript *s, void *u, const KsValue *a, int n) {
    (void) u;
    char raw[64], digits[32];
    ksc_str(s, ksc_arg(a, n, 0), raw, sizeof(raw));
    size_t k = 0;
    for (const char *p = raw; *p && k + 1 < sizeof(digits); p++) {
        if (*p >= '0' && *p <= '9') digits[k++] = *p;
    }
    digits[k] = 0;
    const int gltf = strchr(raw, 'g') != NULL, glb = strchr(raw, 'q') != NULL, fbx = strchr(raw, 'f') != NULL;
    float scale = (float) ksc_clamp(ksc_num_or(ksc_arg(a, n, 4), 1), 0.01, 100);
    scale = roundf(scale * 100.0f) / 100.0f;
    float y = NUM(2, 0);
    if (!gltf && !glb) y += scale;
    y += ((float) rand() / (float) RAND_MAX) * 0.02f - 0.01f; // UTILS.randFloat(-0.01, 0.01)
    const vec4 color = arg_color(s, ksc_arg(a, n, 5), "#FFF");

    KsObj3D *o = obj_new(KSO_MODEL);
    if (!o) return ks_undef();
    o->is_model = 1;
    o->asset = atoi(digits);
    o->xf.position = (vec3) {NUM(1, 0), y, NUM(3, 0)};
    o->xf.scale = (vec3) {scale, scale, scale};
    const KsValue data = ksc_arg(a, n, 6);
    const KsValue speed = ksc_opt(s, data, "animSpeed");
    o->anim_speed = ksc_is_num(speed) && speed.as.num ? (float) speed.as.num : 1.0f;

    char rel[96];
    if (gltf || glb) {
        snprintf(rel, sizeof(rel), "user-assets/%s/model.%s", digits, glb ? "glb" : "gltf");
        char *path = k ? concat(assets_path(), rel) : NULL;
        GltfModel *model = path ? gltf_model_load(path) : NULL;
        free(path);
        o->model = model ? gltf_instance_create(model, o->anim_speed) : NULL;
        if (o->model) {
            o->model->root.parent = &o->xf;
            if (!(color.x == 1 && color.y == 1 && color.z == 1)) {
                for (size_t i = 0; i < o->model->mesh_count; i++) {
                    Material *mat = o->model->meshes[i]->material;
                    if (mat && mat->vtable == &basic_material_vtable) {
                        BasicMaterial *bm = (BasicMaterial *) mat;
                        bm->color = (vec4) {color.x, color.y, color.z, bm->color.w};
                    }
                }
            }
#ifdef GLTF_HAVE_SCRIPT_API
            char start[40];
            const KsValue st = ksc_opt(s, data, "animStart");
            if (ksc_is_str(st) && strlen(ksc_str(s, st, start, sizeof(start))) <= 32) gltf_instance_play_clip(o->model, start, 0);
#endif
        } else {
            o->model_failed = 1;
        }
    } else if (fbx) {
        o->model_failed = 1;
        ksc_log(1, "SCENE.addAsset(\"%s\"): FBX models are not supported by Krunker Box Office", raw);
    } else {
        snprintf(rel, sizeof(rel), "user-assets/%s/model.obj", digits);
        char *path = k ? concat(assets_path(), rel) : NULL;
        Geometry *geometry = path ? load_obj_model(path, 0) : NULL;
        free(path);
        if (geometry && geometry->index_count > 0) {
            BasicMaterial *m = basic_material_init();
            if (m) {
                char tex_rel[96];
                snprintf(tex_rel, sizeof(tex_rel), "user-assets/%s/model.png", digits);
                char *tex_path = concat(assets_path(), tex_rel);
                m->texture = tex_path ? load_texture(tex_path) : 0;
                free(tex_path);
                m->color = color;
                m->alpha_test = strchr(raw, 'a') ? 0.02f : 0.0f;
                if (strchr(raw, 'e')) m->emissive = (vec4) {1, 1, 1, 1};
                o->mesh = mesh_init(geometry, (Material *) m);
                o->mesh->transform.parent = &o->xf;
            }
        } else {
            o->model_failed = 1;
        }
    }
    if (o->model_failed && k) ksc_log(1, "SCENE.addAsset: user asset %s could not be loaded", digits);
    obj_add_to_scene(o);
    const KsValue value = obj_value(s, o);
    // onLoad: the model is ready right away (loaders are synchronous here)
    const KsValue cb = ksc_arg(a, n, 7);
    if (cb.tag == KS_FN && !o->model_failed) {
        ks_handle_pin(s, value);
        ks_call_value(s, cb, &value, 1, NULL);
        ks_handle_unpin(s, value);
    }
    return value;
}

// light objects (placeholders until the renderer has a runtime light list)
static KsObj3D *light_object(const int type, const vec4 color, const float intensity) {
    KsObj3D *o = obj_new(KSO_LIGHT);
    if (!o) return NULL;
    o->light = type;
    o->light_color = (vec3) {color.x, color.y, color.z};
    o->intensity = intensity;
    o->in_scene = 1;
    return o;
}

static int lighting_setting(void) { return client_render_settings()->lighting; }

static void light_sync(KsObj3D *o);

// addPointLight(colr = "#f2f8fc", x, y, z, dist 0..1000, decay 0.1..2, intensity 0..15, shadow): lighting >= 2
static KsValue sc_add_point_light(KsScript *s, void *u, const KsValue *a, int n) {
    (void) u;
    if (lighting_setting() < 2) return ks_undef();
    KsObj3D *o = light_object(KSL_POINT, arg_color(s, ksc_arg(a, n, 0), "#f2f8fc"), (float) ksc_clamp(NUM(6, 1), 0, 15));
    if (!o) return ks_undef();
    o->xf.position = (vec3) {NUM(1, 0), NUM(2, 0), NUM(3, 0)};
    o->distance = (float) ksc_clamp(NUM(4, 0), 0, 1000);
    o->decay = (float) ksc_clamp(NUM(5, 1), 0.1, 2);
    light_sync(o);
    return obj_value(s, o);
}

// addSpotLight(colr, x, y, z, tx, ty, tz, dist, decay, intensity, angleDeg 1..90 (45), penumbra 0..1, shadow)
static KsValue sc_add_spot_light(KsScript *s, void *u, const KsValue *a, int n) {
    (void) u;
    KsObj3D *o = light_object(KSL_SPOT, arg_color(s, ksc_arg(a, n, 0), "#f2f8fc"), (float) ksc_clamp(NUM(9, 1), 0, 15));
    if (!o) return ks_undef();
    o->xf.position = (vec3) {NUM(1, 0), NUM(2, 0), NUM(3, 0)};
    o->target = (vec3) {NUM(4, 0), NUM(5, 0), NUM(6, 0)};
    o->distance = (float) ksc_clamp(NUM(7, 0), 0, 1000);
    o->decay = (float) ksc_clamp(NUM(8, 1), 0.1, 2);
    o->angle = (float) (ksc_clamp(NUM(10, 45), 1, 90) * M_PI / 180.0);
    o->penumbra = (float) ksc_clamp(NUM(11, 0), 0, 1);
    light_sync(o);
    return obj_value(s, o);
}

// addDirLight(colr, x, y, z, tx, ty, tz, intensity 0..15, shadow): lighting == 3
static KsValue sc_add_dir_light(KsScript *s, void *u, const KsValue *a, int n) {
    (void) u;
    if (lighting_setting() != 3) return ks_undef();
    KsObj3D *o = light_object(KSL_DIR, arg_color(s, ksc_arg(a, n, 0), "#f2f8fc"), (float) ksc_clamp(NUM(7, 1), 0, 15));
    if (!o) return ks_undef();
    o->xf.position = (vec3) {NUM(1, 0), NUM(2, 0), NUM(3, 0)};
    o->target = (vec3) {NUM(4, 0), NUM(5, 0), NUM(6, 0)};
    light_sync(o);
    return obj_value(s, o);
}

// addRectLight(colr, x, y, z, w, l, intensity 0..15): lighting == 3
static KsValue sc_add_rect_light(KsScript *s, void *u, const KsValue *a, int n) {
    (void) u;
    if (lighting_setting() != 3) return ks_undef();
    KsObj3D *o = light_object(KSL_RECT, arg_color(s, ksc_arg(a, n, 0), "#f2f8fc"), (float) ksc_clamp(NUM(6, 1), 0, 15));
    if (!o) return ks_undef();
    o->xf.position = (vec3) {NUM(1, 0), NUM(2, 0), NUM(3, 0)};
    o->width = NUM(4, 1);
    o->height = NUM(5, 1);
    light_sync(o);
    return obj_value(s, o);
}

// addLightCone(colr, x, y, z, w, l, h, data): additive cone (fullCone, sides, thetaLength)
static KsValue sc_add_light_cone(KsScript *s, void *u, const KsValue *a, int n) {
    (void) u;
    const KsValue data = ksc_arg(a, n, 7);
    const vec4 color = arg_color(s, ksc_arg(a, n, 0), "#FFF");
    ObjOptions opt = parse_options(s, data, ks_undef(), 2);
    const KsValue sides_v = ksc_opt(s, data, "sides"), theta_v = ksc_opt(s, data, "thetaLength");
    const int sides = (int) ksc_clamp(ksc_is_num(sides_v) && (int) sides_v.as.num ? (int) sides_v.as.num : 12, 3, 64);
    const double theta_deg = ksc_clamp(ksc_is_num(theta_v) && theta_v.as.num ? theta_v.as.num : 360, 0.1, 360);
    unsigned int vbo = 0;
    Geometry *geometry = cone_geometry(sides, (float) (theta_deg * M_PI / 180.0), &vbo);
    BasicMaterial *m = make_material(color, &opt);
    KsObj3D *o = m && geometry ? mesh_object(geometry, m, KSO_MESH) : NULL;
    if (!o) {
        if (m) material_fini((Material *) m);
        geometry_free(geometry, vbo);
        return ks_undef();
    }
    o->own_geometry = geometry;
    o->own_vbo = vbo;
    // src lightcone_0 / lightcone_1 (fullCone), MeshBasicMaterial, AdditiveBlending, depthWrite false
    char *tex = concat(assets_path(), ksc_opt(s, data, "fullCone").tag != KS_UNDEF && ksc_opt(s, data, "fullCone").tag != KS_NULL
                                          ? "textures/lightcone_1.png" : "textures/lightcone_0.png");
    m->texture = tex ? load_texture(tex) : 0;
    free(tex);
    m->unlit = 1;
    m->additive_blend = 1;
    m->no_depth_write = 1;
    m->base.transparent = 1;
    m->double_sided = 1;
    o->xf.position = (vec3) {NUM(1, 0), NUM(2, 0), NUM(3, 0)};
    o->xf.scale = (vec3) {NUM(4, 1) / 2.0f, NUM(6, 1), NUM(5, 1) / 2.0f};
    obj_add_to_scene(o);
    return obj_value(s, o);
}

// addLightBar(colr, x, y, z, w, l, data): transparent lightcone_1 plane
static KsValue sc_add_light_bar(KsScript *s, void *u, const KsValue *a, int n) {
    (void) u;
    const vec4 color = arg_color(s, ksc_arg(a, n, 0), "#FFF");
    ObjOptions opt = parse_options(s, ksc_arg(a, n, 6), ks_undef(), 2);
    opt.transparent = 1;
    KsObj3D *o = plane_object(s, color, &opt, NUM(4, 1), NUM(5, 1), NUM(1, 0), NUM(2, 0), NUM(3, 0), opt.mimic_sprite);
    if (!o) return ks_undef();
    BasicMaterial *m = (BasicMaterial *) o->mesh->material;
    char *tex = concat(assets_path(), "textures/lightcone_1.png");
    m->texture = tex ? load_texture(tex) : 0;
    free(tex);
    m->texture_repeat = (vec2) {1, 1};
    return obj_value(s, o);
}

// ---------------------------------------------------------------------------
// Signs (3.3.2): the text drawn into a (w * 2) x (l * 2) canvas texture (UTILS.createCanvasText)

static void text_blend(unsigned char *rgba, const int w, const int h, const char *text, float x, const float baseline, const vec4 c) {
    for (const unsigned char *p = (const unsigned char *) text; *p;) {
        unsigned int cp = *p++;
        int extra = cp >= 0xf0 ? 3 : cp >= 0xe0 ? 2 : cp >= 0xc0 ? 1 : 0;
        if (extra) cp &= 0x3fu >> extra;
        while (extra-- && (*p & 0xc0) == 0x80) cp = (cp << 6) | (*p++ & 0x3f);
        if (FT_Load_Char(g_game_font, cp, FT_LOAD_RENDER)) continue;
        const FT_GlyphSlot g = g_game_font->glyph;
        const int left = (int) floorf(x) + g->bitmap_left, top = (int) floorf(baseline) - g->bitmap_top;
        for (int row = 0; row < (int) g->bitmap.rows; row++) {
            const int py = top + row;
            if (py < 0 || py >= h) continue;
            for (int col = 0; col < (int) g->bitmap.width; col++) {
                const int px = left + col;
                if (px < 0 || px >= w) continue;
                const float cov = (float) g->bitmap.buffer[row * g->bitmap.pitch + col] / 255.0f;
                if (cov <= 0.0f) continue;
                unsigned char *d = rgba + ((size_t) py * (size_t) w + (size_t) px) * 4;
                const float da = d[3] / 255.0f, oa = cov + da * (1.0f - cov);
                const float cc[3] = {c.x, c.y, c.z};
                for (int k = 0; k < 3; k++) {
                    const float dv = d[k] / 255.0f;
                    d[k] = (unsigned char) (fminf(1.0f, (cc[k] * cov + dv * da * (1.0f - cov)) / (oa > 0 ? oa : 1)) * 255.0f + 0.5f);
                }
                d[3] = (unsigned char) (oa * 255.0f + 0.5f);
            }
        }
        x += (float) g->advance.x / 64.0f;
    }
}

static float text_width(const char *text) {
    float wsum = 0.0f;
    for (const unsigned char *p = (const unsigned char *) text; *p;) {
        unsigned int cp = *p++;
        int extra = cp >= 0xf0 ? 3 : cp >= 0xe0 ? 2 : cp >= 0xc0 ? 1 : 0;
        if (extra) cp &= 0x3fu >> extra;
        while (extra-- && (*p & 0xc0) == 0x80) cp = (cp << 6) | (*p++ & 0x3f);
        if (!FT_Load_Char(g_game_font, cp, FT_LOAD_DEFAULT)) wsum += (float) g_game_font->glyph->advance.x / 64.0f;
    }
    return wsum;
}

static unsigned int sign_texture(const char *text, int w, int h, const int font_size, const vec4 fg, const vec4 bg, const int no_bg,
                                 const int align, const vec4 outline, const int outline_px) {
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (w > 2048) w = 2048;
    if (h > 2048) h = 2048;
    unsigned char *rgba = calloc((size_t) w * (size_t) h, 4);
    if (!rgba) return 0;
    for (size_t i = 0; i < (size_t) w * (size_t) h; i++) {
        const vec4 c = no_bg ? fg : bg;
        rgba[i * 4] = (unsigned char) (c.x * 255.0f + 0.5f);
        rgba[i * 4 + 1] = (unsigned char) (c.y * 255.0f + 0.5f);
        rgba[i * 4 + 2] = (unsigned char) (c.z * 255.0f + 0.5f);
        rgba[i * 4 + 3] = no_bg ? 0 : 255;
    }
    if (g_game_font) {
        FT_Set_Pixel_Sizes(g_game_font, 0, (unsigned int) font_size);
        // lines split at "\n", vertically centred, aligned left / centre / right
        char buf[1024];
        snprintf(buf, sizeof(buf), "%s", text);
        int lines = 1;
        for (const char *p = buf; *p; p++) lines += *p == '\n';
        float baseline = (float) h / 2.0f - (float) (lines - 1) * font_size * 0.6f + font_size * 0.35f;
        for (char *line = buf; line;) {
            char *next = strchr(line, '\n');
            if (next) *next++ = 0;
            const float lw = text_width(line);
            const float x = align == 2 ? (float) w - lw - font_size * 0.5f : align == 1 ? ((float) w - lw) / 2.0f : font_size * 0.5f;
            if (outline_px > 1) {
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++)
                        if (dx || dy) text_blend(rgba, w, h, line, x + dx * (outline_px / 2.0f), baseline + dy * (outline_px / 2.0f), outline);
            }
            text_blend(rgba, w, h, line, x, baseline, fg);
            baseline += font_size * 1.2f;
            line = next;
        }
    }
    // upload, rows bottom-up (three.js flipY)
    const size_t row = (size_t) w * 4;
    unsigned char *flipped = malloc(row * (size_t) h);
    if (flipped) for (int y = 0; y < h; y++) memcpy(flipped + (size_t) y * row, rgba + (size_t) (h - 1 - y) * row, row);
    unsigned int tex = 0;
    glGenTextures(1, &tex);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    g_active_texture = tex;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, flipped ? flipped : rgba);
    free(flipped);
    free(rgba);
    return tex;
}

// addSign(x, y, z, w, l, text = "Hello World", data)
static KsValue sc_add_sign(KsScript *s, void *u, const KsValue *a, int n) {
    (void) u;
    const KsValue data = ksc_arg(a, n, 6);
    char text[1024] = "Hello World";
    if (ksc_is_str(ksc_arg(a, n, 5))) ksc_str(s, ksc_arg(a, n, 5), text, sizeof(text));
    const float w = NUM(3, 1), l = NUM(4, 1);
    const KsValue fs = ksc_opt(s, data, "fontSize"), ta = ksc_opt(s, data, "textAlign"), ot = ksc_opt(s, data, "outlineThickness");
    int font_size = ksc_is_num(fs) ? (int) fs.as.num : 10;
    font_size = (int) ksc_clamp(font_size ? font_size : 10, 10, 120);
    int align = 0;
    if (ta.tag == KS_STR) {
        char b[16];
        ksc_str(s, ta, b, sizeof(b));
        align = !strcmp(b, "center") ? 1 : !strcmp(b, "right") ? 2 : 0;
    } else if (ksc_is_num(ta)) {
        align = (int) ksc_clamp((int) ta.as.num, 0, 2);
    }
    int othick = ksc_is_num(ot) ? (int) ot.as.num : 0;
    othick = (int) ksc_clamp(othick ? othick : 1, 1, 50);
    const unsigned int tex = sign_texture(text, (int) (w * 2), (int) (l * 2), font_size, arg_color(s, ksc_opt(s, data, "fontColor"), "#000"),
                                          arg_color(s, ksc_opt(s, data, "backgroundColor"), "#fff"),
                                          ksc_truthy(ksc_opt(s, data, "backgroundInvisible")), align,
                                          arg_color(s, ksc_opt(s, data, "outlineColor"), "#626262"), othick);
    ObjOptions opt = {0};
    opt.opacity = 1.0f;
    opt.transparent = 1;
    opt.side = 2;
    opt.stretch = 1;
    KsObj3D *o = plane_object(s, (vec4) {1, 1, 1, 1}, &opt, w, l, NUM(0, 0), NUM(1, 0), NUM(2, 0), ksc_truthy(ksc_opt(s, data, "mimicSprite")));
    if (!o) return ks_undef();
    BasicMaterial *m = (BasicMaterial *) o->mesh->material;
    m->texture = tex;
    m->color = (vec4) {1, 1, 1, 1}; // the canvas carries the colours (no getColor boost)
    m->alpha_test = 0.02f;
    m->unlit = 1;              // noShader
    return obj_value(s, o);
}

// ---------------------------------------------------------------------------
// obj3D members (3.2.4)

static KsObj3D *obj_of(void *native) { return native; }

static void obj_material_each(KsObj3D *o, void (*fn)(BasicMaterial*, void*), void *user) {
    if (o->mesh && o->mesh->material && o->mesh->material->vtable == &basic_material_vtable) fn((BasicMaterial *) o->mesh->material, user);
    if (o->model) {
        for (size_t i = 0; i < o->model->mesh_count; i++) {
            Material *m = o->model->meshes[i]->material;
            if (m && m->vtable == &basic_material_vtable) fn((BasicMaterial *) m, user);
        }
    }
}

static void obj_apply_visible(KsObj3D *o) {
    if (o->mesh) o->mesh->visible = o->visible;
    if (o->model) gltf_instance_set_visible(o->model, o->visible);
}

static KsValue o_id(KsScript *s, void *native, uint32_t id) { (void) s; (void) id; return ks_num(obj_of(native)->id); }
static KsValue o_get_visible(KsScript *s, void *native, uint32_t id) { (void) s; (void) id; return ks_bool(obj_of(native)->visible); }
static void o_set_visible(KsScript *s, void *native, uint32_t id, KsValue v) {
    (void) s; (void) id;
    KsObj3D *o = obj_of(native);
    o->visible = ksc_truthy(v);
    obj_apply_visible(o);
}
// v10.0.3 obj3D intensity / distance / color (deob-10.0.3.js:208565-208600): lights only (0 / "" otherwise); intensity
// 0..15 (the scene's own units: light_sync scales it), distance 0..1000 on point and spot lights (the others have none)
static KsValue o_get_intensity(KsScript *s, void *native, uint32_t id) {
    (void) s;
    (void) id;
    const KsObj3D *o = obj_of(native);
    return ks_num(o->kind == KSO_LIGHT ? o->intensity : 0);
}
static void o_set_intensity(KsScript *s, void *native, uint32_t id, KsValue v) {
    (void) s;
    (void) id;
    KsObj3D *o = obj_of(native);
    if (o->kind != KSO_LIGHT || v.tag != KS_NUM || !isfinite(v.as.num)) return;
    o->intensity = (float) ksc_clamp(v.as.num, 0, 15);
    light_sync(o);
}
static int light_has_distance(const KsObj3D *o) { return o->kind == KSO_LIGHT && (o->light == KSL_POINT || o->light == KSL_SPOT); }
static KsValue o_get_distance(KsScript *s, void *native, uint32_t id) {
    (void) s;
    (void) id;
    const KsObj3D *o = obj_of(native);
    return ks_num(light_has_distance(o) ? o->distance : 0);
}
static void o_set_distance(KsScript *s, void *native, uint32_t id, KsValue v) {
    (void) s;
    (void) id;
    KsObj3D *o = obj_of(native);
    if (!light_has_distance(o) || v.tag != KS_NUM || !isfinite(v.as.num)) return;
    o->distance = (float) ksc_clamp(v.as.num, 0, 1000);
    light_sync(o);
}
static KsValue o_get_color(KsScript *s, void *native, uint32_t id) {
    (void) id;
    const KsObj3D *o = obj_of(native);
    if (o->kind != KSO_LIGHT) return ks_str(s, "", 0);
    char hex[8]; // "#" + color.getHexString()
    const int r = (int) lroundf(fminf(fmaxf(o->light_color.x, 0.0f), 1.0f) * 255.0f), g = (int) lroundf(fminf(fmaxf(o->light_color.y, 0.0f), 1.0f) * 255.0f),
              b = (int) lroundf(fminf(fmaxf(o->light_color.z, 0.0f), 1.0f) * 255.0f);
    snprintf(hex, sizeof(hex), "#%02x%02x%02x", r, g, b);
    return ks_str(s, hex, 7);
}
static void o_set_color(KsScript *s, void *native, uint32_t id, KsValue v) {
    (void) id;
    KsObj3D *o = obj_of(native);
    if (o->kind != KSO_LIGHT || !ksc_is_hex(v)) return;
    const vec4 c = arg_color(s, v, "#ffffff");
    o->light_color = (vec3) {c.x, c.y, c.z};
    light_sync(o);
}
static KsValue o_get_emissive(KsScript *s, void *native, uint32_t id) { (void) native; (void) id; return ks_str(s, "", 0); }
static void set_emissive(BasicMaterial *m, void *user) { m->emissive = *(vec4 *) user; }
static void o_set_emissive(KsScript *s, void *native, uint32_t id, KsValue v) {
    (void) id;
    if (!ksc_is_hex(v)) return;
    vec4 c = arg_color(s, v, "#000");
    obj_material_each(obj_of(native), set_emissive, &c);
}
static KsValue o_get_opacity(KsScript *s, void *native, uint32_t id) {
    (void) s; (void) id;
    const KsObj3D *o = obj_of(native);
    return ks_num(o->mesh || o->model ? o->opacity : 1);
}
static void set_opacity(BasicMaterial *m, void *user) {
    const float op = *(float *) user;
    m->color.w = op;
    m->base.transparent = op != 1.0f || m->additive_blend;
}
static void o_set_opacity(KsScript *s, void *native, uint32_t id, KsValue v) {
    (void) s; (void) id;
    KsObj3D *o = obj_of(native);
    float op = (float) ksc_num_or(v, 1);
    o->opacity = op;
    obj_material_each(o, set_opacity, &op);
}
static KsValue o_get_texture(KsScript *s, void *native, uint32_t id) { (void) s; (void) id; return ks_num(obj_of(native)->asset); }
static void o_set_texture(KsScript *s, void *native, uint32_t id, KsValue v) {
    (void) id;
    KsObj3D *o = obj_of(native);
    if (o->kind == KSO_PLAYER_ASSET) { // a GLTF root has no material: data.asset only (deob-10.0.3.js:208637-208650)
        char buf[32];
        if (ksc_is_str(v)) {
            const char *t = ksc_str(s, v, buf, sizeof(buf));
            int digits = t[0] != 0;
            for (const char *c = t; *c; c++) digits &= *c >= '0' && *c <= '9';
            if (digits) o->asset = atoi(t); // /^\d+$/: parseInt
        } else if (ksc_is_num(v)) {
            o->asset = (int) v.as.num;
        }
        return;
    }
    if (o->is_model || !o->mesh || !ksc_is_num(v)) return;
    o->asset = (int) v.as.num;
    ((BasicMaterial *) o->mesh->material)->texture = asset_texture(o->asset);
}
static KsValue o_get_anim_speed(KsScript *s, void *native, uint32_t id) { (void) s; (void) id; return ks_num(obj_of(native)->anim_speed); }
static void o_set_anim_speed(KsScript *s, void *native, uint32_t id, KsValue v) {
    (void) s; (void) id;
    KsObj3D *o = obj_of(native);
    if (!o->is_model) return;
    o->anim_speed = ksc_is_num(v) && v.as.num ? (float) v.as.num : 1.0f;
#ifdef GLTF_HAVE_SCRIPT_API
    if (o->model) gltf_instance_set_time_scale(o->model, o->anim_speed);
#else
    if (o->model) o->model->time_scale = o->anim_speed;
#endif
}

// position / rotation / scale sub-objects: a class per field over the same native
#define VEC_CLASS(field)                                                                                     \
    static KsValue v_##field##_x(KsScript *s, void *n, uint32_t i) { (void) s; (void) i; return ks_num(obj_of(n)->xf.field.x); } \
    static KsValue v_##field##_y(KsScript *s, void *n, uint32_t i) { (void) s; (void) i; return ks_num(obj_of(n)->xf.field.y); } \
    static KsValue v_##field##_z(KsScript *s, void *n, uint32_t i) { (void) s; (void) i; return ks_num(obj_of(n)->xf.field.z); } \
    static void sv_##field##_x(KsScript *s, void *n, uint32_t i, KsValue v) { (void) s; (void) i; obj_of(n)->xf.field.x = (float) ksc_float(v); light_sync(obj_of(n)); } \
    static void sv_##field##_y(KsScript *s, void *n, uint32_t i, KsValue v) { (void) s; (void) i; obj_of(n)->xf.field.y = (float) ksc_float(v); light_sync(obj_of(n)); } \
    static void sv_##field##_z(KsScript *s, void *n, uint32_t i, KsValue v) { (void) s; (void) i; obj_of(n)->xf.field.z = (float) ksc_float(v); light_sync(obj_of(n)); } \
    static const KsHostProp field##_PROPS[] = {{"x", v_##field##_x, sv_##field##_x, NULL, 0}, {"y", v_##field##_y, sv_##field##_y, NULL, 0}, {"z", v_##field##_z, sv_##field##_z, NULL, 0}}; \
    static const KsHostClass field##_CLASS = {#field, field##_PROPS, 3};                                     \
    static KsValue o_##field(KsScript *s, void *native, uint32_t id) { return ks_host_object(s, &field##_CLASS, native, id); }
VEC_CLASS(position)
VEC_CLASS(rotation)
VEC_CLASS(scale)

static void set3(vec3 *out, const KsValue *a, const int n) {
    *out = (vec3) {(float) ksc_float(ksc_arg(a, n, 0)), (float) ksc_float(ksc_arg(a, n, 1)), (float) ksc_float(ksc_arg(a, n, 2))};
}

static KsValue o_move(KsScript *s, void *native, const KsValue *a, int n) {
    (void) s;
    set3(&obj_of(native)->xf.position, a, n);
    light_sync(obj_of(native));
    return ks_undef();
}

static KsValue o_rotate(KsScript *s, void *native, const KsValue *a, int n) {
    (void) s;
    set3(&obj_of(native)->xf.rotation, a, n);
    return ks_undef();
}

// rotateLocal(x, y, z): about the bounding-box centre on world axes, x then y then z. KN takes
// the object's position as the centre (the geometry is centred or base-centred).
static KsValue o_rotate_local(KsScript *s, void *native, const KsValue *a, int n) {
    (void) s;
    KsObj3D *o = obj_of(native);
    o->xf.rotation.x += (float) ksc_float(ksc_arg(a, n, 0));
    o->xf.rotation.y += (float) ksc_float(ksc_arg(a, n, 1));
    o->xf.rotation.z += (float) ksc_float(ksc_arg(a, n, 2));
    return ks_undef();
}

// lookAt(x, y, z): lights move their target; meshes turn toward the point (three.js lookAt:
// the object's +Z axis faces the target)
static KsValue o_look_at(KsScript *s, void *native, const KsValue *a, int n) {
    (void) s;
    KsObj3D *o = obj_of(native);
    vec3 t;
    set3(&t, a, n);
    if (o->kind == KSO_LIGHT) {
        o->target = t;
        light_sync(o);
        return ks_undef();
    }
    const vec3 p = obj_world_position(&o->xf);
    const float dx = t.x - p.x, dy = t.y - p.y, dz = t.z - p.z;
    o->xf.rotation_order = ROTATION_ORDER_YXZ;
    o->xf.rotation = (vec3) {-atan2f(dy, sqrtf(dx * dx + dz * dz)), atan2f(dx, dz), 0.0f};
    return ks_undef();
}

static KsValue o_play_anim(KsScript *s, void *native, const KsValue *a, int n) {
    KsObj3D *o = obj_of(native);
    if (!o->model || !ksc_is_str(ksc_arg(a, n, 0))) return ks_undef();
#ifdef GLTF_HAVE_SCRIPT_API
    char clip[64];
    gltf_instance_play_clip(o->model, ksc_str(s, ksc_arg(a, n, 0), clip, sizeof(clip)), (int) ksc_num_or(ksc_arg(a, n, 1), 1));
#else
    (void) s;
#endif
    return ks_undef();
}

static KsValue o_stop_anim(KsScript *s, void *native, const KsValue *a, int n) {
    (void) s; (void) a; (void) n;
#ifdef GLTF_HAVE_SCRIPT_API
    if (obj_of(native)->model) gltf_instance_stop_clips(obj_of(native)->model);
#else
    (void) native;
#endif
    return ks_undef();
}

static KsValue bone_value(KsScript *s, uint32_t owner, const char *name);

// getBone(name): a skinned-mesh bone of a model (RENDER.traverseBones)
static KsValue o_get_bone(KsScript *s, void *native, const KsValue *a, int n) {
    KsObj3D *o = obj_of(native);
    GltfInstance *model = obj_gltf(o); // traverseBones(the object, name): a player asset's are the class model's
    if (!ksc_is_str(ksc_arg(a, n, 0)) || !model) return ks_undef();
    char name[64];
    ksc_str(s, ksc_arg(a, n, 0), name, sizeof(name));
#ifdef GLTF_HAVE_SCRIPT_API
    if (!gltf_instance_bone(model, name)) return ks_undef();
    return bone_value(s, o->id, name);
#else
    return ks_undef();
#endif
}

static KsValue o_update_morph(KsScript *s, void *native, const KsValue *a, int n) {
    KsObj3D *o = obj_of(native);
    if (!o->model || !ksc_is_str(ksc_arg(a, n, 0))) return ks_undef();
#ifdef GLTF_HAVE_SCRIPT_API
    char name[64];
    double v = ksc_float(ksc_arg(a, n, 1));
    gltf_instance_set_morph(o->model, ksc_str(s, ksc_arg(a, n, 0), name, sizeof(name)), (float) ksc_clamp(v, 0, 1));
#else
    (void) s;
#endif
    return ks_undef();
}

// delete(): RENDER.scene.remove (the object keeps working, unseen)
static KsValue o_delete(KsScript *s, void *native, const KsValue *a, int n) {
    (void) s; (void) a; (void) n;
    KsObj3D *o = obj_of(native);
    if (o->kind == KSO_PLAYER_ASSET) o->asset_deleted = 1; // parent.remove(root): the class model is unseen
    obj_remove_from_scene(o);
    if (o->kind == KSO_LIGHT) {
        o->in_scene = 0;
        light_sync(o);
    }
    return ks_undef();
}

// attachTo(target, x, y, z, keepWorld, point): a pivot at (x, y, z) under a player (target.sid; point
// "hands" / "handL": that anchor of its model) or a scene object / bone / the camera (target.id).
// Re-resolved every frame (meshes are rebuilt).
static KsValue o_attach_to(KsScript *s, void *native, const KsValue *a, int n) {
    KsObj3D *o = obj_of(native);
    const KsValue target = ksc_arg(a, n, 0);
    if (target.tag != KS_OBJ && target.tag != KS_HOST) return ks_undef();
    const KsValue sid = ks_obj_get(s, target, "sid");
    const KsValue id = ks_obj_get(s, target, "id");
    const vec3 world = obj_world_position(&o->xf);
    o->pivot.position = (vec3) {(float) ksc_float(ksc_arg(a, n, 1)), (float) ksc_float(ksc_arg(a, n, 2)), (float) ksc_float(ksc_arg(a, n, 3))};
    if (sid.tag != KS_UNDEF && sid.tag != KS_NULL) {
        o->attach = KSA_PLAYER;
        o->attach_uid = (int) ks_to_number(sid);
        char point[8] = "";
        const KsValue p = ksc_arg(a, n, 5);
        if (ksc_is_str(p)) ksc_str(s, p, point, sizeof(point));
        o->attach_point = !strcmp(point, "hands") ? KSP_HANDS : !strcmp(point, "handL") ? KSP_HAND_L : KSP_ANCHOR;
    } else {
        const uint32_t tid = (uint32_t) ks_to_number(id);
        if (tid == KSC_CAMERA_OBJECT_ID) o->attach = KSA_CAMERA;
        else if (obj_find(tid)) o->attach = KSA_OBJECT;
        else {
            int bone = 0;
            for (int i = 0; i < g_sc.bone_count; i++) bone |= g_sc.bones[i].id == tid;
            if (!bone) return ks_undef();
            o->attach = KSA_BONE;
        }
        o->attach_id = tid;
    }
    o->xf.parent = &o->pivot;
    o->asset_deleted = 0; // a deleted player asset is added again, under the pivot
    if (ksc_truthy(ksc_arg(a, n, 4))) { // keepWorld: keep the world position (pivot resolved next frame)
        o->xf.position = (vec3) {world.x - o->pivot.position.x, world.y - o->pivot.position.y, world.z - o->pivot.position.z};
    }
    return ks_undef();
}

static KsValue o_detach(KsScript *s, void *native, const KsValue *a, int n) {
    (void) s;
    KsObj3D *o = obj_of(native);
    if (!o->attach) return ks_undef();
    const vec3 world = obj_world_position(&o->xf);
    o->attach = KSA_NONE;
    o->xf.parent = NULL;
    o->pivot.parent = NULL;
    o->asset_unparented = 1; // scene.add(root): a player asset leaves the player for the world
    if (ksc_truthy(ksc_arg(a, n, 0))) o->xf.position = world;
    return ks_undef();
}

static const KsHostProp OBJ_PROPS[] = {
    {"id", o_id, NULL, NULL, 0},
    {"visible", o_get_visible, o_set_visible, NULL, 0},
    {"intensity", o_get_intensity, o_set_intensity, NULL, 0},
    {"distance", o_get_distance, o_set_distance, NULL, 0},
    {"color", o_get_color, o_set_color, NULL, 0},
    {"emissive", o_get_emissive, o_set_emissive, NULL, 0},
    {"opacity", o_get_opacity, o_set_opacity, NULL, 0},
    {"texture", o_get_texture, o_set_texture, NULL, 0},
    {"animSpeed", o_get_anim_speed, o_set_anim_speed, NULL, 0},
    {"position", o_position, NULL, NULL, 1},
    {"rotation", o_rotation, NULL, NULL, 1},
    {"scale", o_scale, NULL, NULL, 1},
    {"move", NULL, NULL, o_move, 0},
    {"rotate", NULL, NULL, o_rotate, 0},
    {"rotateLocal", NULL, NULL, o_rotate_local, 0},
    {"lookAt", NULL, NULL, o_look_at, 0},
    {"playAnim", NULL, NULL, o_play_anim, 0},
    {"stopAnim", NULL, NULL, o_stop_anim, 0},
    {"getBone", NULL, NULL, o_get_bone, 0},
    {"updateMorph", NULL, NULL, o_update_morph, 0},
    {"delete", NULL, NULL, o_delete, 0},
    {"attachTo", NULL, NULL, o_attach_to, 0},
    {"detach", NULL, NULL, o_detach, 0},
};
static const KsHostClass OBJ_CLASS = {"obj3D", OBJ_PROPS, (int) (sizeof(OBJ_PROPS) / sizeof(OBJ_PROPS[0]))};

static KsValue obj_value(KsScript *s, KsObj3D *o) { return ks_host_object(s, &OBJ_CLASS, o, o->id); }

// ---------------------------------------------------------------------------
// Bones (3.2.5): the bone's node transform, looked up by name on every access

static KsBone *bone_of(void *native) { return native; }

static MeshTransform *bone_transform(const KsBone *b) {
#ifdef GLTF_HAVE_SCRIPT_API
    const KsObj3D *o = obj_find(b->owner);
    GltfInstance *model = o ? obj_gltf(o) : NULL;
    return model ? gltf_instance_bone(model, b->name) : NULL;
#else
    (void) b;
    return NULL;
#endif
}

static MeshTransform g_dummy_xf;
static MeshTransform *bone_xf(void *native) {
    MeshTransform *t = bone_transform(bone_of(native));
    return t ? t : &g_dummy_xf;
}

#define BONE_VEC(field)                                                                                    \
    static KsValue b_##field##_x(KsScript *s, void *n, uint32_t i) { (void) s; (void) i; return ks_num(bone_xf(n)->field.x); } \
    static KsValue b_##field##_y(KsScript *s, void *n, uint32_t i) { (void) s; (void) i; return ks_num(bone_xf(n)->field.y); } \
    static KsValue b_##field##_z(KsScript *s, void *n, uint32_t i) { (void) s; (void) i; return ks_num(bone_xf(n)->field.z); } \
    static void bs_##field##_x(KsScript *s, void *n, uint32_t i, KsValue v) { (void) s; (void) i; bone_xf(n)->field.x = (float) ksc_float(v); } \
    static void bs_##field##_y(KsScript *s, void *n, uint32_t i, KsValue v) { (void) s; (void) i; bone_xf(n)->field.y = (float) ksc_float(v); } \
    static void bs_##field##_z(KsScript *s, void *n, uint32_t i, KsValue v) { (void) s; (void) i; bone_xf(n)->field.z = (float) ksc_float(v); } \
    static const KsHostProp bone_##field##_PROPS[] = {{"x", b_##field##_x, bs_##field##_x, NULL, 0}, {"y", b_##field##_y, bs_##field##_y, NULL, 0}, {"z", b_##field##_z, bs_##field##_z, NULL, 0}}; \
    static const KsHostClass bone_##field##_CLASS = {"bone_" #field, bone_##field##_PROPS, 3};             \
    static KsValue b_##field(KsScript *s, void *native, uint32_t id) { return ks_host_object(s, &bone_##field##_CLASS, native, id); }
BONE_VEC(position)
BONE_VEC(rotation)
BONE_VEC(scale)

static KsValue b_id(KsScript *s, void *native, uint32_t id) { (void) s; (void) id; return ks_num(bone_of(native)->id); }
static KsValue b_move(KsScript *s, void *native, const KsValue *a, int n) {
    (void) s;
    set3(&bone_xf(native)->position, a, n);
    return ks_undef();
}

static const KsHostProp BONE_PROPS[] = {
    {"id", b_id, NULL, NULL, 0},
    {"move", NULL, NULL, b_move, 0},
    {"position", b_position, NULL, NULL, 1},
    {"rotation", b_rotation, NULL, NULL, 1},
    {"scale", b_scale, NULL, NULL, 1},
};
static const KsHostClass BONE_CLASS = {"bone", BONE_PROPS, (int) (sizeof(BONE_PROPS) / sizeof(BONE_PROPS[0]))};

static KsValue bone_value(KsScript *s, const uint32_t owner, const char *name) {
    for (int i = 0; i < g_sc.bone_count; i++) {
        KsBone *b = &g_sc.bones[i];
        if (b->owner == owner && !strcmp(b->name, name)) return ks_host_object(s, &BONE_CLASS, b, b->id);
    }
    if (g_sc.bone_count == g_sc.bone_cap) {
        // the KsBone array moves when it grows: wrappers point into it, so it never shrinks or moves
        if (g_sc.bone_cap) return ks_undef();
        g_sc.bone_cap = 256;
        g_sc.bones = calloc((size_t) g_sc.bone_cap, sizeof(KsBone));
        if (!g_sc.bones) {
            g_sc.bone_cap = 0;
            return ks_undef();
        }
    }
    KsBone *b = &g_sc.bones[g_sc.bone_count++];
    b->id = new_id();
    b->owner = owner;
    snprintf(b->name, sizeof(b->name), "%s", name);
    return ks_host_object(s, &BONE_CLASS, b, b->id);
}

// ---------------------------------------------------------------------------
// Lights

static void light_sync(KsObj3D *o) {
    if (!o || o->kind != KSO_LIGHT) return;
#ifdef BASIC_HAVE_LIGHT_LIST
    BasicLight light;
    memset(&light, 0, sizeof(light));
    light.type = o->light == KSL_POINT ? BASIC_LIGHT_POINT : o->light == KSL_SPOT ? BASIC_LIGHT_SPOT
               : o->light == KSL_DIR ? BASIC_LIGHT_DIRECTIONAL : BASIC_LIGHT_RECT;
    light.color = o->light_color;
    light.visible = o->visible && o->in_scene;
    light.intensity = o->intensity;
    light.distance = o->distance;
    light.decay = o->decay;
    light.angle = o->angle;
    light.penumbra = o->penumbra;
    light.position = obj_world_position(&o->xf);
    light.target = o->target;
    light.width = o->width;
    light.height = o->height;
    if (o->light_handle) basic_lights_update(o->light_handle, &light);
    else o->light_handle = basic_lights_add(&light);
    if (o->light_handle) return;
#endif
    static const char *const NAMES[] = {"", "point lights", "spot lights", "directional lights", "rect-area lights"};
    ksc_warn_light(NAMES[o->light]);
}

// ---------------------------------------------------------------------------
// Scene-wide calls

// posToScreen(x, y, z): {x, y, onScreen} in overlay units
static KsValue sc_pos_to_screen(KsScript *s, void *u, const KsValue *a, int n) {
    (void) u;
    Client *client = g_ksc.client;
    if (!client) return ks_undef();
    const float x = NUM(0, 0), y = NUM(1, 0), z = NUM(2, 0);
    const float *v = client->camera.world_inverse_matrix, *p = client->camera.projection_matrix;
    const float cx = v[0] * x + v[1] * y + v[2] * z + v[3];
    const float cy = v[4] * x + v[5] * y + v[6] * z + v[7];
    const float cz = v[8] * x + v[9] * y + v[10] * z + v[11];
    const float clip_x = p[0] * cx + p[1] * cy + p[2] * cz + p[3];
    const float clip_y = p[4] * cx + p[5] * cy + p[6] * cz + p[7];
    const float clip_z = p[8] * cx + p[9] * cy + p[10] * cz + p[11];
    const float clip_w = p[12] * cx + p[13] * cy + p[14] * cz + p[15];
    const float w = clip_w != 0.0f ? clip_w : 1e-6f;
    const float nx = clip_x / w, ny = clip_y / w, nz = clip_z / w;
    const int on_screen = clip_w > 0.0f && nx >= -1.0f && nx <= 1.0f && ny >= -1.0f && ny <= 1.0f && nz >= -1.0f && nz <= 1.0f;
    const KsOvResult size = ks_overlay_call(KS_OV_GET_SIZE, NULL, 0);
    const double ow = size.type == KS_OV_RET_SIZE ? size.width : 0, oh = size.type == KS_OV_RET_SIZE ? size.height : 0;
    const KsValue out = ks_obj_new(s);
    ks_obj_set(s, out, "x", ks_num(ow * ((nx + 1.0) / 2.0)));
    ks_obj_set(s, out, "y", ks_num(oh * (1.0 - (ny + 1.0) / 2.0)));
    ks_obj_set(s, out, "onScreen", ks_bool(on_screen));
    return out;
}

// clear(): every object leaves the render scene, the map included
static KsValue sc_clear(KsScript *s, void *u, const KsValue *a, int n) {
    (void) s; (void) u; (void) a; (void) n;
    Scene *scene = world_scene();
    if (!scene) return ks_undef();
    for (int i = 0; i < g_sc.count; i++) { // the viewmodel pass's ones are in the same three.js scene
        if (g_sc.objs[i]->fps_layer) obj_remove_from_scene(g_sc.objs[i]);
        g_sc.objs[i]->in_scene = 0;
    }
    scene->mesh_count = 0;
    return ks_undef();
}

static vec4 lighting_hex(KsScript *s, const KsValue v, const vec4 fallback) {
    vec4 c = fallback;
    char buf[16];
    if (ksc_is_hex(v)) ksc_parse_hex(ksc_str(s, v, buf, sizeof(buf)), &c);
    return c;
}

// setFog(colr, dist 10..15000): Fog(colr, 1, dist)
static KsValue sc_set_fog(KsScript *s, void *u, const KsValue *a, int n) {
    (void) u;
    g_render_lighting.fog_color = lighting_hex(s, ksc_arg(a, n, 0), g_render_lighting.fog_color);
    g_render_lighting.fog_near = 1.0f;
    g_render_lighting.fog_far = (float) ksc_clamp(ksc_num_or(ksc_arg(a, n, 1), g_render_lighting.fog_far), 10, 15000);
    g_render_lighting.fog_enabled = 1;
    return ks_undef();
}

static double round2(const double x) { return round(x * 100.0) / 100.0; }

// setAmbientLight(colr, intensity 0.1..3)
static KsValue sc_set_ambient(KsScript *s, void *u, const KsValue *a, int n) {
    (void) u;
    g_render_lighting.ambient_color = lighting_hex(s, ksc_arg(a, n, 0), g_render_lighting.ambient_color);
    g_render_lighting.ambient_intensity = (float) round2(ksc_clamp(ksc_num_or(ksc_arg(a, n, 1), g_render_lighting.ambient_intensity), 0.1, 3));
    return ks_undef();
}

// setSkyLight(colr, intensity 0.1..3, sunAngX = 90, sunAngY = 54, dist 10..15000)
static KsValue sc_set_sky_light(KsScript *s, void *u, const KsValue *a, int n) {
    (void) u;
    g_render_lighting.light_color = lighting_hex(s, ksc_arg(a, n, 0), g_render_lighting.light_color);
    g_render_lighting.light_intensity = (float) round2(ksc_clamp(ksc_num_or(ksc_arg(a, n, 1), g_render_lighting.light_intensity), 0.1, 3));
    const double ax = M_PI * (ksc_clamp(ksc_num_or(ksc_arg(a, n, 2), 90), 0, 360) / -180.0);
    const double ay = M_PI * (ksc_clamp(ksc_num_or(ksc_arg(a, n, 3), 54), 0, 360) / -180.0);
    vec3 d = {(float) cos(ax), (float) (sin(ax) * sin(ay)), (float) (sin(ax) * cos(ay))};
    const float len = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
    if (len > 0.0f) g_render_lighting.light_direction = (vec3) {d.x / len, d.y / len, d.z / len};
    return ks_undef();
}

// setSkyColor(colr = "#dce8ed"): the clear colour
static KsValue sc_set_sky_color(KsScript *s, void *u, const KsValue *a, int n) {
    (void) u;
    vec4 c = {0, 0, 0, 1};
    ksc_parse_hex("#dce8ed", &c);
    g_render_lighting.sky_color = lighting_hex(s, ksc_arg(a, n, 0), c);
    return ks_undef();
}

// setSkyDome(c0, c1, c2, data): the map sky dome with these stops (sky.c)
static KsValue sc_set_sky_dome(KsScript *s, void *u, const KsValue *a, int n) {
    (void) u;
    cJSON *raw = cJSON_CreateObject();
    if (!raw) return ks_undef();
    cJSON_AddTrueToObject(raw, "skyDome");
    static const char *const KEYS[] = {"skyDomeCol0", "skyDomeCol1", "skyDomeCol2"};
    for (int i = 0; i < 3; i++) {
        char buf[16];
        cJSON_AddStringToObject(raw, KEYS[i], ksc_is_hex(ksc_arg(a, n, i)) ? ksc_str(s, ksc_arg(a, n, i), buf, sizeof(buf)) : "#fff");
    }
    const KsValue data = ksc_arg(a, n, 3);
    if (ksc_truthy(ksc_opt(s, data, "texture"))) cJSON_AddTrueToObject(raw, "skyDomeTex");
    const KsValue mt = ksc_opt(s, data, "textureMoveSpeed");
    if (ksc_is_num(mt)) cJSON_AddNumberToObject(raw, "skyDomeMovT", mt.as.num);
    sky_load(raw);
    cJSON_Delete(raw);
    g_sc.sky_changed = 1;
    return ks_undef();
}

static KsValue sc_remove_sky_dome(KsScript *s, void *u, const KsValue *a, int n) {
    (void) s; (void) u; (void) a; (void) n;
    sky_load(NULL);
    g_sc.sky_changed = 1;
    return ks_undef();
}

static KsObj3D *obj_from_value(KsScript *s, const KsValue v) {
    (void) s;
    if (v.tag != KS_HOST) return NULL;
    const uint32_t id = (uint32_t) ks_to_number(ks_obj_get(s, v, "id"));
    return obj_find(id);
}

// movObj(o, dx, dy, dz) adds; rotObj / scaleObj set
static KsValue sc_mov_obj(KsScript *s, void *u, const KsValue *a, int n) {
    (void) u;
    KsObj3D *o = obj_from_value(s, ksc_arg(a, n, 0));
    if (!o) return ks_undef();
    o->xf.position.x += (float) ksc_float(ksc_arg(a, n, 1));
    o->xf.position.y += (float) ksc_float(ksc_arg(a, n, 2));
    o->xf.position.z += (float) ksc_float(ksc_arg(a, n, 3));
    light_sync(o);
    return ks_undef();
}

static KsValue sc_rot_obj(KsScript *s, void *u, const KsValue *a, int n) {
    (void) u;
    KsObj3D *o = obj_from_value(s, ksc_arg(a, n, 0));
    if (o) set3(&o->xf.rotation, a + 1, n - 1);
    return ks_undef();
}

static KsValue sc_scale_obj(KsScript *s, void *u, const KsValue *a, int n) {
    (void) u;
    KsObj3D *o = obj_from_value(s, ksc_arg(a, n, 0));
    if (o) set3(&o->xf.scale, a + 1, n - 1);
    return ks_undef();
}

static KsValue sc_add_particles_none(KsScript *s, void *u, const KsValue *args, int argc)
{
    (void) s; (void) u; (void) args; (void) argc;
    return ks_undef();
}

void ks_scene_api(KsApiImpl *api) {
    api[KS_API_SCENE_POSTOSCREEN].fn = sc_pos_to_screen;
    // SCENE.addParticles (v10 L182830): the particle emitter is not ported yet (the KS particle
    // layer is a gap, see the krunkscript gap list); the call is accepted and answers undefined
    api[KS_API_SCENE_ADDPARTICLES].fn = sc_add_particles_none;
    api[KS_API_SCENE_CLEAR].fn = sc_clear;
    api[KS_API_SCENE_SETFOG].fn = sc_set_fog;
    api[KS_API_SCENE_SETAMBIENTLIGHT].fn = sc_set_ambient;
    api[KS_API_SCENE_SETSKYLIGHT].fn = sc_set_sky_light;
    api[KS_API_SCENE_SETSKYCOLOR].fn = sc_set_sky_color;
    api[KS_API_SCENE_SETSKYDOME].fn = sc_set_sky_dome;
    api[KS_API_SCENE_REMOVESKYDOME].fn = sc_remove_sky_dome;
    api[KS_API_SCENE_ADDCUSTOM].fn = sc_add_custom;
    api[KS_API_SCENE_ADDCUBE].fn = sc_add_cube;
    api[KS_API_SCENE_ADDPLANE].fn = sc_add_plane;
    api[KS_API_SCENE_ADDSPHERE].fn = sc_add_sphere;
    api[KS_API_SCENE_ADDASSET].fn = sc_add_asset;
    api[KS_API_SCENE_ADDSPRITE].fn = sc_add_sprite;
    api[KS_API_SCENE_ADDPOINTLIGHT].fn = sc_add_point_light;
    api[KS_API_SCENE_ADDDIRLIGHT].fn = sc_add_dir_light;
    api[KS_API_SCENE_ADDSPOTLIGHT].fn = sc_add_spot_light;
    api[KS_API_SCENE_ADDRECTLIGHT].fn = sc_add_rect_light;
    api[KS_API_SCENE_ADDSIGN].fn = sc_add_sign;
    api[KS_API_SCENE_ADDLIGHTCONE].fn = sc_add_light_cone;
    api[KS_API_SCENE_ADDLIGHTBAR].fn = sc_add_light_bar;
    api[KS_API_SCENE_MOVOBJ].fn = sc_mov_obj;
    api[KS_API_SCENE_ROTOBJ].fn = sc_rot_obj;
    api[KS_API_SCENE_SCALEOBJ].fn = sc_scale_obj;
}

// ---------------------------------------------------------------------------
// Players' models (player.getAsset / playAnim / stopAnim)

// getAsset(): `meshRef && meshRef.children[0] && obj3D(meshRef.children[0])` (deob-10.0.3.js:207888-207892): the class
// model's GLTF scene root once it loaded (loadAnimMesh's holder has it as its only child; an OBJ's loadMesh has none,
// a sprite no meshRef), wrapped by obj3D (:208553-208890), one wrapper per root (`_protected ||=`), its data reset to {}
// on every call. On the root: position / rotation / scale / move / rotate / lookAt / visible / delete / attachTo /
// detach / getBone act; playAnim, stopAnim, updateMorph and animSpeed do nothing (the holder has the mixer), emissive
// and opacity have no material (opacity reads 1), texture keeps data.asset. Here a KSO_PLAYER_ASSET object: its xf sits
// between the holder and the GLTF root (identity at first), bound each frame (ks_scene_frame) while that model lives.
int ks_scene_player_asset(KsScript *s, const Player *p, KsValue *out) {
    *out = ks_undef();
    const PlayerMesh *pm = p ? p->mesh : NULL;
    const PlayerClassModel *m = pm ? pm->class_model : NULL;
    if (!player_class_model_replaces_body(m) || !m->gltf) return 0;
    KsObj3D *o = NULL;
    for (int i = 0; i < g_sc.count && !o; i++) {
        KsObj3D *c = g_sc.objs[i];
        if (c->kind == KSO_PLAYER_ASSET && c->asset_uid == p->uid && c->asset_serial == m->serial) o = c;
    }
    if (!o) {
        o = obj_new(KSO_PLAYER_ASSET);
        if (!o) return 0;
        o->asset_uid = p->uid;
        o->asset_serial = m->serial;
        o->xf.parent = (MeshTransform *) &m->holder;
    }
    o->asset = 0; // data = {}
    *out = obj_value(s, o);
    return 1;
}

void ks_scene_class_model_freed(const PlayerClassModel *m) {
    for (int i = 0; m && i < g_sc.count; i++) {
        KsObj3D *o = g_sc.objs[i];
        if (o->kind == KSO_PLAYER_ASSET && o->xf.parent == &m->holder) o->xf.parent = NULL; // the holder goes with it
    }
}

// playAnim(clip, reps): meshRef.playClip(clip, isNumber(reps) ? reps : 1) on the drawn GLTF class model, with no
// hasAnims test (deob-10.0.3.js:208232-208238); stopAnim(): meshRef.stopAllClips() (:208240-208244)
void ks_scene_player_play_anim(const Player *p, const char *clip, const int reps) {
    if (clip) player_class_model_script_clip(p, clip, reps);
}
void ks_scene_player_stop_anim(const Player *p) { player_class_model_script_clip(p, NULL, 0); }

MeshTransform *ks_scene_camera_transform(void) { return &g_sc.camera_xf; }

// ---------------------------------------------------------------------------
// Frame / lifecycle

static MeshTransform *attach_target(const KsObj3D *o) {
    switch (o->attach) {
        case KSA_CAMERA: return &g_sc.camera_xf;
        case KSA_OBJECT: {
            KsObj3D *t = obj_find(o->attach_id);
            return t && t != o ? &t->xf : NULL;
        }
        case KSA_BONE:
            for (int i = 0; i < g_sc.bone_count; i++) {
                if (g_sc.bones[i].id == o->attach_id) return bone_transform(&g_sc.bones[i]);
            }
            return NULL;
        case KSA_PLAYER: {
            Client *client = g_ksc.client;
            Player *p = client ? ks_game_player_by_uid(&client->game, (uint32_t) o->attach_uid) : NULL;
            const PlayerMesh *pm = p ? p->mesh : NULL;
            // getHands(player) || objInstances, getHandL(player) || objInstances
            MeshTransform *point = o->attach_point == KSP_HANDS ? player_pose_hands_anchor(p) : o->attach_point == KSP_HAND_L ? player_pose_hand_l_anchor(p) : NULL;
            return point ? point : pm ? pm->anchor : NULL;
        }
        default: return NULL;
    }
}

// syncHandL's layers.mask (deob-10.0.3.js:193540-193546): an object under a first-person "handL" anchor
// (attached to it, or to an object or a model bone that is) takes the arm's layer, so the viewmodel pass draws it
static int obj_first_person(const KsObj3D *o, const int depth) {
    if (!o->attach || depth > 8) return 0;
    switch (o->attach) {
        case KSA_PLAYER: {
            if (o->attach_point != KSP_HAND_L) return 0;
            Client *client = g_ksc.client;
            return player_pose_hand_l_first_person(client ? ks_game_player_by_uid(&client->game, (uint32_t) o->attach_uid) : NULL);
        }
        case KSA_OBJECT: {
            const KsObj3D *t = obj_find(o->attach_id);
            return t && t != o && obj_first_person(t, depth + 1);
        }
        case KSA_BONE:
            for (int i = 0; i < g_sc.bone_count; i++) {
                if (g_sc.bones[i].id != o->attach_id) continue;
                const KsObj3D *owner = obj_find(g_sc.bones[i].owner);
                return owner && owner != o && obj_first_person(owner, depth + 1);
            }
            return 0;
        default: return 0;
    }
}

void ks_scene_attached_layers(int *fps, int *world) {
    int f = 0, w = 0;
    for (int i = 0; i < g_sc.count; i++) {
        const KsObj3D *o = g_sc.objs[i];
        if (!o->attach || !o->in_scene || o->kind == KSO_LIGHT) continue;
        if (o->fps_layer) f++;
        else w++;
    }
    if (fps) *fps = f;
    if (world) *world = w;
}

static void obj_set_fps_layer(KsObj3D *o, const int fps) {
    if (o->fps_layer == fps) return;
    const int shown = o->in_scene;
    if (shown) obj_remove_from_scene(o);
    o->fps_layer = fps;
    if (shown) obj_add_to_scene(o);
}

void ks_scene_frame(Client *client, const float delta_ms) {
    // the camera as an attach target: position + rotation like the render camera
    g_sc.camera_xf.position = client->camera.position;
    g_sc.camera_xf.rotation = client->camera.rotation;
    g_sc.camera_xf.scale = (vec3) {1, 1, 1};
    g_sc.camera_xf.rotation_order = ROTATION_ORDER_EXTRINSIC;

    for (int i = 0; i < g_sc.count; i++) {
        KsObj3D *o = g_sc.objs[i];
        if (o->attach) {
            MeshTransform *t = attach_target(o);
            o->pivot.parent = t;
            o->xf.parent = &o->pivot;
            // an object attached under a target that is gone stays where its pivot says
        }
        if (o->kind != KSO_LIGHT) obj_set_fps_layer(o, obj_first_person(o, 0));
        if (o->kind == KSO_SPRITE && o->mesh) {
            // SpriteMaterial: always faces the camera
            o->mesh->transform.rotation_order = ROTATION_ORDER_YXZ;
            o->mesh->transform.rotation = (vec3) {client->camera.rotation.x - o->xf.rotation.x, client->camera.rotation.y - o->xf.rotation.y, 0.0f};
        }
        if (o->mesh && o->mesh->material && o->mesh->material->vtable == &basic_material_vtable) {
            BasicMaterial *m = (BasicMaterial *) o->mesh->material;
            o->tex_time += delta_ms;
            if (o->tex_move != 0.0f) {
                const float step = fmodf(o->tex_move * delta_ms, 1.0f);
                if (o->tex_move_axis) m->texture_offset.y = fmodf(m->texture_offset.y + step, 1.0f);
                else m->texture_offset.x = fmodf(m->texture_offset.x + step, 1.0f);
            }
            if (o->frames > 0 && o->frame_ms > 0) {
                m->texture_repeat.x = 1.0f / (float) o->frames;
                m->texture_offset.x = (float) ((int) (o->tex_time / o->frame_ms) % o->frames);
            }
        }
        if (o->model) gltf_instance_update(o->model, gltf_clock());
        if (o->kind == KSO_LIGHT && o->attach) light_sync(o);
        if (o->kind == KSO_PLAYER_ASSET) { // the class model's GLTF root under this object while that model lives
            PlayerClassModel *m = obj_borrowed(o);
            if (m) {
                m->gltf->root.parent = &o->xf;
                if (!o->attach && !o->asset_unparented) o->xf.parent = &m->holder;
                gltf_instance_set_visible(m->gltf, o->visible && !o->asset_deleted);
            } else if (!o->attach && !o->asset_unparented) {
                o->xf.parent = NULL;
            }
        }
    }
}

void ks_scene_unload(void) {
    for (int i = 0; i < g_sc.count; i++) obj_free(g_sc.objs[i]);
    g_sc.count = 0;
    // bone records are kept (wrappers of the freed script may not outlive it, but the array
    // must not move while any script runs); their owners are gone, so they resolve to nothing
    g_sc.bone_count = 0;
    if (g_sc.sky_changed && g_ksc.client && g_ksc.client->game.map) sky_load(g_ksc.client->game.map->raw_data);
    g_sc.sky_changed = 0;
}
