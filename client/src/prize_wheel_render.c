// v10.0.3 map prize wheels drawn (client/include/prize_wheel_render.h). Line numbers: deob-10.0.3.js.
//   the mesh    render.addSpinner (:232681-232707): new Mesh(GEOS.generatePlane(size, size)) with a MeshBasicMaterial
//               (unlit, fogged, FrontSide, white vertex colours, transparent: MapManager.addSpinner sets it, :186563)
//               whose map is a CanvasTexture of the spinner's own canvas (alphaTest 0.1), at the object's bottom
//               centre + sy / 2, rotateY(PI / 2 - d * PI / 4), scaled sy x sy (prize_wheel_plane)
//   the canvas  min(max(sy / 7.5 * 265, 768), 4096) px square, painted by updateSpinners at each 50 ms step while
//               spinInfo.needsUpdate (prize_wheels.c counts the paints, prize_wheel_geom.c lays them out). Like the
//               browser canvas it is never cleared: each paint goes over the last.
//   the shadow  the pointer's shadowColor black / shadowBlur 20 stay set on the 2D context, so the hub, the label and,
//               from the second paint on, everything casts it. canvas2d.h has no shadows: each shape is first
//               stroked black at the levels of prize_wheel_shadow_levels (the blur's coverage outside the shape;
//               the inner half of each stroke lies under the shape's own fill).
// Not drawn: the won item's model in front of the hub (meshPrev; its place and bob are prize_wheels.c state).
#include <gl.h> // first: glad before any other GL header
#include <prize_wheel_render.h>
#include <prize_wheel_geom.h>
#include <prize_wheels.h>
#include <map_spinners.h>
#include <store_spins.h>
#include <canvas2d.h>
#include <map_post.h> // prefab_canvas_end: the canvas into a texture
#include <klog.h>
#include "viewer/viewer_store.h" // store.rarities: the slices' colours
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PRIZE_WHEEL_RENDER_MAX 64     // prize_wheels.c's PRIZE_WHEELS_MAX
#define PRIZE_WHEEL_ALPHA_TEST 0.1f   // material.alphaTest = 0.1 (:232706)
#define PRIZE_WHEEL_SHADOW_STEPS 6    // nested strokes per shadow (prize_wheel_shadow_levels)
#define PRIZE_WHEEL_RED "#F44336"     // the pointer and the label disc (:219625, :219645)
#define PRIZE_WHEEL_SETTLED_PAINTS 60 // identical paints (3 s of steps) after which the canvas no longer changes

typedef struct {
    int uid;
    int object;
    Mesh *mesh;
    BasicMaterial *material;
    Canvas2D *canvas;
    int px;
    int paints;        // canvas paints so far: the context's shadow is on from the first one's pointer on
    unsigned drawn;    // PrizeWheel.paints last painted
    int painted;       // at least one paint
    float last[5];     // the spin state of the last paint, and how many paints in a row repeated it
    int repeats;
} WheelMesh;

static struct {
    Scene *scene;
    int count;
    WheelMesh wheels[PRIZE_WHEEL_RENDER_MAX];
    Geometry *quad;
    unsigned int quad_vbo;
    int shadow_levels;
    double shadow_grow[PRIZE_WHEEL_SHADOW_STEPS], shadow_alpha[PRIZE_WHEEL_SHADOW_STEPS];
} g_pwr;

// THREE.PlaneGeometry(1, 1) (GEOS.generatePlane's unit plane): the XY quad facing +z, UVs 0..1 (v 1 = the canvas top)
static Geometry *quad_geometry(void) {
    if (g_pwr.quad) return g_pwr.quad;
    static const float vertices[] = {
        -0.5f, -0.5f, 0.0f, 0.0f, 0.0f,
        0.5f, -0.5f, 0.0f, 1.0f, 0.0f,
        0.5f, 0.5f, 0.0f, 1.0f, 1.0f,
        -0.5f, 0.5f, 0.0f, 0.0f, 1.0f,
    };
    static const unsigned int indices[] = {0, 1, 2, 0, 2, 3};
    Geometry *geometry = calloc(1, sizeof(Geometry));
    if (!geometry) return NULL;
    glGenVertexArrays(1, &geometry->vao);
    glGenBuffers(1, &geometry->ebo);
    glGenBuffers(1, &g_pwr.quad_vbo);
    glBindVertexArray(geometry->vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_pwr.quad_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, geometry->ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), NULL);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *) (3 * sizeof(float)));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);
    geometry->index_count = (int) (sizeof(indices) / sizeof(indices[0]));
    geometry->bound_radius = 0.7072f; // the unit quad's half diagonal (frustum culling)
    g_pwr.quad = geometry;
    return geometry;
}

void prize_wheel_render_load(Scene *scene, const Map *map) {
    prize_wheel_render_unload();
    if (!scene || !map) return;
    g_pwr.scene = scene;
    if (!g_pwr.shadow_levels)
        g_pwr.shadow_levels = prize_wheel_shadow_levels(PRIZE_WHEEL_SHADOW_BLUR, PRIZE_WHEEL_SHADOW_STEPS, g_pwr.shadow_grow, g_pwr.shadow_alpha);
    Geometry *quad = quad_geometry();
    for (int uid = 0, index; quad && (index = map_spinner_object(map, uid)) >= 0 && g_pwr.count < PRIZE_WHEEL_RENDER_MAX; uid++) {
        const Object *o = map->objects[index];
        BasicMaterial *material = basic_material_init();
        if (!material) break;
        material->unlit = 1;              // MeshBasicMaterial
        material->base.transparent = 1;   // IÏiíìíî.transparent = true (:186563)
        material->alpha_test = PRIZE_WHEEL_ALPHA_TEST;
        material->double_sided = 0;       // FrontSide
        Mesh *mesh = mesh_init(quad, (Material *) material);
        if (!mesh) {
            material_fini((Material *) material);
            break;
        }
        float centre[3], yaw;
        prize_wheel_plane((const float[3]) {o->position.x, o->position.y, o->position.z}, o->scale.y, map_spinner_dir(o), centre, &yaw);
        mesh->transform.position = (vec3) {centre[0], centre[1], centre[2]};
        mesh->transform.rotation = (vec3) {0.0f, yaw, 0.0f};
        mesh->transform.scale = (vec3) {o->scale.y, o->scale.y, 1.0f};
        mesh->no_shadow_receive = 1;      // a MeshBasicMaterial is not shadowed (shadows: false, :186566)
        mesh->visible = 0;                // nothing to show before the first paint (the canvas starts empty)
        mesh_update_transform_matrix(mesh);
        WheelMesh *wm = &g_pwr.wheels[g_pwr.count++];
        memset(wm, 0, sizeof(*wm));
        wm->uid = uid;
        wm->object = index;
        wm->mesh = mesh;
        wm->material = material;
        wm->px = prize_wheel_canvas_px(o->scale.y);
        scene_add_mesh(scene, mesh);
    }
    if (g_pwr.count) KN_LOG_VERBOSE2("[prize] %d prize wheel plane(s)\n", g_pwr.count);
}

void prize_wheel_render_unload(void) {
    for (int i = 0; i < g_pwr.count; i++) {
        WheelMesh *wm = &g_pwr.wheels[i];
        if (g_pwr.scene) scene_remove_mesh(g_pwr.scene, wm->mesh);
        if (wm->material->texture) {
            gl_release_texture(wm->material->texture);
            glDeleteTextures(1, &wm->material->texture);
            if (g_active_texture == wm->material->texture) g_active_texture = 0;
            wm->material->texture = 0;
        }
        mesh_fini(wm->mesh); // the material with it; the quad is shared (kept)
        if (wm->canvas) c2d_destroy(wm->canvas);
    }
    g_pwr.count = 0;
    g_pwr.scene = NULL;
}

int prize_wheel_render_count(void) { return g_pwr.count; }

unsigned int prize_wheel_render_texture(const int uid) {
    for (int i = 0; i < g_pwr.count; i++) {
        if (g_pwr.wheels[i].uid == uid) return g_pwr.wheels[i].painted ? g_pwr.wheels[i].material->texture : 0;
    }
    return 0;
}

static void set_fill(Canvas2D *c, const char *css) {
    float rgba[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    c2d_parse_color(css, rgba);
    c2d_set_fill_color(c, rgba[0], rgba[1], rgba[2], rgba[3]);
}

// The context's shadow under the current path (or `text`): black strokes, widest first, before the shape's own fill
static void shadow(Canvas2D *c, const int on, const char *text, const double x, const double y) {
    if (!on) return;
    c2d_save(c);
    c2d_set_line_join(c, C2D_JOIN_ROUND);
    for (int k = 0; k < g_pwr.shadow_levels; k++) {
        if (g_pwr.shadow_grow[k] < 0.25) continue; // the shape's own edge: nothing outside it (a 0 width would keep the last)
        c2d_set_stroke_color(c, 0.0f, 0.0f, 0.0f, (float) g_pwr.shadow_alpha[k]);
        c2d_set_line_width(c, g_pwr.shadow_grow[k] * 2.0);
        if (text) c2d_stroke_text(c, text, x, y, 0, 0.0);
        else c2d_stroke(c);
    }
    c2d_restore(c);
}

static void disc(Canvas2D *c, const double x, const double y, const double r, const char *color, const int shadowed) {
    if (!(r > 0.0)) return;
    c2d_begin_path(c);
    c2d_arc(c, x, y, r, 0.0, M_PI * 2.0, 0);
    c2d_close_path(c);
    shadow(c, shadowed, NULL, 0.0, 0.0);
    set_fill(c, color);
    c2d_fill(c);
}

// One updateSpinners paint (:219577-219668)
static void paint(WheelMesh *wm, const PrizeWheel *w) {
    const StoreSpins *spins = store_spins_builtin();
    const StoreWheel *sw = w->wheel < spins->wheel_count ? &spins->wheels[w->wheel] : NULL;
    if (!sw) return;
    if (!wm->canvas) {
        wm->canvas = c2d_create();
        if (!wm->canvas) return;
        c2d_set_size(wm->canvas, wm->px, wm->px, wm->px, wm->px);
    }
    PrizeWheelState st = {
        .canvas = wm->px,
        .pool = sw->pool_count > 0,
        // pools ? pools.length : store.rarities.length (:219587)
        .count = sw->pool_count > 0 ? sw->pool_count : g_viewer_rarity_count,
        .weight_count = sw->slices,
        .won_rarity = w->won_rarity,
        .spin_whl_mlt = w->spin_whl_mlt,
        .end_anim_val = w->end_anim_val,
        .end_anim_target = w->end_anim_target,
        .spin_rotation = w->spin_rotation,
        .item_shown = w->item_shown,
    };
    for (int i = 0; i < sw->slices && i < PRIZE_WHEEL_GEOM_SLICES; i++) st.weights[i] = sw->weights[i];
    PrizeWheelLayout l;
    prize_wheel_layout(&st, &l);

    Canvas2D *c = wm->canvas;
    const int all_shadowed = wm->paints > 0; // the first paint sets shadowBlur only at the pointer
    c2d_reset(c);
    disc(c, l.centre, l.centre, l.radius, "#fff", all_shadowed);
    c2d_save(c);
    c2d_translate(c, l.centre, l.centre);
    if (l.turn != 0.0) c2d_rotate(c, l.turn);
    for (int i = 0; i < l.count; i++) {
        if (!(l.slices[i].sweep > 0.0)) continue;
        const char *color = i < g_viewer_rarity_count ? g_viewer_rarities[i].color : "#fff"; // store.rarities[i].color
        c2d_begin_path(c);
        c2d_move_to(c, 0.0, 0.0);
        c2d_arc(c, 0.0, 0.0, l.slice_radius, l.slices[i].start, l.slices[i].start + l.slices[i].sweep, 0);
        c2d_line_to(c, 0.0, 0.0);
        shadow(c, all_shadowed, NULL, 0.0, 0.0);
        set_fill(c, color);
        c2d_fill(c);
    }
    c2d_restore(c);
    // the pointer: shadowColor black, shadowBlur 20 from here on (:219625-219637)
    c2d_save(c);
    c2d_translate(c, l.centre, l.centre);
    c2d_rotate(c, l.pointer_rotation);
    c2d_begin_path(c);
    c2d_move_to(c, l.pointer[0][0], l.pointer[0][1]);
    c2d_line_to(c, l.pointer[1][0], l.pointer[1][1]);
    c2d_line_to(c, l.pointer[2][0], l.pointer[2][1]);
    c2d_close_path(c);
    shadow(c, 1, NULL, 0.0, 0.0);
    set_fill(c, PRIZE_WHEEL_RED);
    c2d_fill(c);
    c2d_restore(c);
    disc(c, l.centre, l.centre, l.hub_radius, "#fff", 1);
    if (l.label) {
        disc(c, l.centre, l.centre, l.label_radius, PRIZE_WHEEL_RED, 1);
        c2d_save(c);
        c2d_translate(c, l.centre, l.centre);
        c2d_set_font(c, l.spin_font, "GameFont");
        c2d_set_text_baseline(c, C2D_BASELINE_BOTTOM);
        c2d_set_text_align(c, C2D_ALIGN_CENTER);
        shadow(c, 1, "SPIN", 0.0, l.spin_y);
        set_fill(c, "#fff");
        c2d_fill_text(c, "SPIN", 0.0, l.spin_y, 0, 0.0);
        c2d_restore(c);
        char price[32];
        snprintf(price, sizeof(price), "-%d KR", sw->price); // "-" + iìÏîìiì.price + " KR"
        c2d_save(c);
        c2d_translate(c, l.centre, l.centre);
        c2d_set_global_alpha(c, 0.7);
        c2d_set_font(c, l.price_font, "GameFont");
        c2d_set_text_baseline(c, C2D_BASELINE_TOP);
        c2d_set_text_align(c, C2D_ALIGN_CENTER);
        shadow(c, 1, price, 0.0, l.price_y);
        set_fill(c, "#fff");
        c2d_fill_text(c, price, 0.0, l.price_y, 0, 0.0);
        c2d_restore(c);
    }
    wm->paints++;

    // material.map.needsUpdate: the canvas into a new texture (a CanvasTexture: linear, mipmapped, clamped)
    const unsigned int texture = prefab_canvas_end(c, NULL);
    if (!texture) return;
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glGenerateMipmap(GL_TEXTURE_2D);
    const unsigned int old = wm->material->texture;
    wm->material->texture = texture;
    if (old) {
        gl_release_texture(old);
        glDeleteTextures(1, &old);
        if (g_active_texture == old) g_active_texture = 0;
    }
    g_active_texture = texture; // left bound on unit 0
    wm->painted = 1;
    wm->mesh->visible = 1;
}

void prize_wheel_render_frame(Client *client) {
    if (!client || !g_pwr.count) return;
    for (int i = 0; i < g_pwr.count; i++) {
        WheelMesh *wm = &g_pwr.wheels[i];
        const PrizeWheel *w = prize_wheels_get(client, wm->uid);
        if (!w || w->object != wm->object) continue; // the spin state is still on another map
        if (w->paints == wm->drawn && wm->painted) continue;
        wm->drawn = w->paints;
        if (!w->paints) continue;
        // a stopped pool wheel goes on painting the same wheel (its needsUpdate never clears, prize_wheels.c): the
        // paints over the last only darken the shadows' rims, which are as dark as they get after a few seconds
        const float now[5] = {w->spin_whl_mlt, w->spin_rotation, w->end_anim_val, w->end_anim_target, (float) w->item_shown};
        wm->repeats = wm->painted && !memcmp(now, wm->last, sizeof(now)) ? wm->repeats + 1 : 0;
        memcpy(wm->last, now, sizeof(now));
        if (wm->repeats > PRIZE_WHEEL_SETTLED_PAINTS) continue;
        paint(wm, w);
    }
}
