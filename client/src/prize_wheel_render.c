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
//   the item    the stop puts the won item up in front of the hub (meshPrev, :219485-219574; prize_wheel_item_pose):
//               its store model as loadMesh / loadAnimMesh build it (store_item_model.c, the viewer's code: src / texSrc,
//               glowText / emissive, skin_fx's animations), a spray as createSpray's 9 x 7.2 plane (:232665-232680),
//               a dye as a class-9 preview Player wearing it (:219504-219516): a client-only render model (its own
//               private Game, player_switch_class's setClass loadout, no spawn or its hooks), at init(0, 0, -6) in
//               meshPrev, its primary's geometry hidden; bobbing at each step; updateSpinner's next spin takes it
//               down (:219413-219417). An Unobtainable's shake and explosion are prize_wheels.c's (at the stop).
#include <gl.h> // first: glad before any other GL header
#include <prize_wheel_render.h>
#include <prize_wheel_geom.h>
#include <prize_wheels.h>
#include <map_spinners.h>
#include <store_spins.h>
#include <canvas2d.h>
#include <map_post.h> // prefab_canvas_end: the canvas into a texture
#include <store_item_model.h> // the won item's model
#include <gltf.h>
#include <klog.h>
#include <render_settings.h>
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
    float size, dir;   // the object's sy and d
    // the won item: iíÍíìiï (the frame at the wheel's centre) and meshPrev in it, the model or the spray's plane in that
    MeshTransform frame, prev;
    int item_id;       // the item up, -1 none
    float org_y;       // meshPrev.orgYP
    StoreItemModel item;
    Mesh *spray;
    int spray_frames, spray_frame_t;
    Player *dye_player; // a won dye's preview Player (client-only)
} WheelMesh;

static struct {
    Scene *scene;
    Map *map;      // the map the planes are of (the dye previews' Game reads its config.model)
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
    g_pwr.map = (Map *) map;
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
        wm->size = o->scale.y;
        wm->dir = map_spinner_dir(o);
        wm->item_id = -1;
        scene_add_mesh(scene, mesh);
    }
    if (g_pwr.count) KN_LOG_VERBOSE2("[prize] %d prize wheel plane(s)\n", g_pwr.count);
}

static void item_down(WheelMesh *wm);

void prize_wheel_render_unload(void) {
    for (int i = 0; i < g_pwr.count; i++) {
        WheelMesh *wm = &g_pwr.wheels[i];
        item_down(wm);
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
    g_pwr.map = NULL;
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

// A private Game for the dye previews: players.setClass's tables (the ruleset) and the current map (generateMeshes
// reads its config.model; the previews go with the map), no mode, never ticked, so nothing a game player's spawn
// does (spawn points, mode / KrunkScript / trigger hooks) reaches it. viewer_scene.c gives its preview players one the
// same way.
static Game *preview_game(void) {
    static Game game;
    static int ready;
    if (!ready) {
        game_configure(&game, NULL, NULL, 0, NULL, 0, NULL, 0, NULL, 0);
        ready = 1;
    }
    game.map = g_pwr.map;
    return &game;
}

static void dye_player_free(Player *player) {
    if (!player) return;
    if (player->mesh) {
        if (g_pwr.scene) scene_remove_player_mesh(g_pwr.scene, player->mesh, player->loadout_size);
        player_meshes_fini(player);
    }
    free(player->loadout);
    free(player->ammo);
    free(player->reloads);
    free(player->input_queue);
    free(player);
}

// The won dye (:219504-219516): new Player(-1), sid -1, init(0, 0, -6, "preview") (z 0, x 0, y -6), skins [-1, -1],
// dyeIndex = itemID, setClass(game, 9, 2, true) (the Runner, the class lists bypassed), swapWeapon(0), generateMeshes,
// its objInstances in meshPrev, swapTime 0, weaponMeshes[0] shown with weaponGeos[0] hidden, updateMsh
static Player *dye_player_new(const int dye, MeshTransform *parent) {
    if (!g_pwr.map) return NULL;
    Player *player = player_init(preview_game());
    if (!player) return NULL;
    player->uid = -1; // player_update_meshes anchors uid -1 (Krunker's preview sid) at player->position
    player->skins = PLAYER_SKINS_NONE;
    player->cosmetics = PLAYER_COSMETICS_NONE;
    player->cosmetics.dye = dye;
    player->cosmetics_v7 = PLAYER_COSMETICS_V7_NONE;
    if (!player_switch_class(player, 9, 2)) { // setClass: the loadout only (no spawn point, no spawn hooks)
        dye_player_free(player);
        return NULL;
    }
    player->active = 1;
    player->position = player->last_position = (vec3) {0.0f, -6.0f, 0.0f};
    player->velocity = (vec3) {0};
    player->direction = (vec2) {0.0f, 0.0f};
    player_generate_meshes(player, 0); // third person
    if (!player->mesh) {
        dye_player_free(player);
        return NULL;
    }
    player_swap_weapon(player, 0, 1, 1, 1);
    player->swap_timer = 0.0f;
    player_update_meshes(player, 1);
    PlayerMesh *mesh = player->mesh;
    mesh->anchor->parent = parent; // meshPrev.add(objInstances)
    PlayerArms *arms = mesh->arms && player->loadout_size > 0 ? mesh->arms[0] : NULL;
    if (arms) { // weaponGeos[0].visible = false: the arms stay, the primary's model goes
        if (arms->weapon_right) arms->weapon_right->visible = 0;
        if (arms->weapon_models[0]) gltf_instance_set_visible(arms->weapon_models[0], 0);
    }
    scene_add_player_mesh(g_pwr.scene, mesh, player->loadout_size);
    return player;
}

// meshPrev removed (updateSpinner, :219413-219417; the map going)
static void item_down(WheelMesh *wm) {
    dye_player_free(wm->dye_player);
    wm->dye_player = NULL;
    if (wm->item.mesh && g_pwr.scene) scene_remove_mesh(g_pwr.scene, wm->item.mesh);
    if (wm->item.model && g_pwr.scene) scene_remove_gltf(g_pwr.scene, wm->item.model);
    store_item_model_fini(&wm->item);
    if (wm->spray) {
        if (g_pwr.scene) scene_remove_mesh(g_pwr.scene, wm->spray);
        mesh_fini(wm->spray); // the material with it; the quad and the texture are shared
        wm->spray = NULL;
    }
    wm->item_id = -1;
}

// render.createSpray(item) (:232665-232680): PlaneGeometry(1, 1) scaled (9, 7.2, 1), getMat("sprays/<id>", {transparent,
// alphaTest 0.2, frames, frameT, side 2}); its rotation.y = PI in meshPrev (:219518-219519)
static Mesh *spray_plane(const ViewerSkin *item, MeshTransform *parent, const float rot_y) {
    Geometry *quad = quad_geometry();
    BasicMaterial *material = quad ? basic_material_init() : NULL;
    if (!material) return NULL;
    char rel[64];
    snprintf(rel, sizeof(rel), "textures/sprays/%d.png", item->id > 0 ? item->id : 0);
    char *path = concat(assets_path(), rel);
    material->texture = path ? load_texture(path) : 0; // cached by path (ensure_asset_file on the web)
    free(path);
    material->base.transparent = 1;
    material->alpha_test = 0.2f;
    material->double_sided = 1;
    if (item->frames > 0) material->texture_repeat.x = 1.0f / (float) item->frames;
    Mesh *mesh = mesh_init(quad, (Material *) material);
    if (!mesh) {
        material_fini((Material *) material);
        return NULL;
    }
    mesh->transform.rotation.y = rot_y;
    mesh->transform.scale = (vec3) {9.0f, 7.2f, 1.0f};
    mesh->transform.parent = parent;
    return mesh;
}

// The stop's meshPrev (:219485-219574): the won item in front of the hub
static void item_up(WheelMesh *wm, const PrizeWheel *w) {
    item_down(wm);
    const ViewerSkin *item = viewer_skin(w->item_id);
    if (!item) return;
    // iîIïîïì = item.type ? item : store.getPreview(item) (:219489)
    const ViewerPreview *preview = viewer_preview(item);
    const float y_off = item->type ? item->y_off : preview->y_off, x_off = item->type ? item->x_off : preview->x_off;
    const float scl = item->type ? item->scl : preview->scl;
    PrizeWheelItemPose pose;
    prize_wheel_item_pose(wm->size, wm->dir, item->type, y_off, x_off, scl, item->spn_scl, &pose);
    wm->frame = (MeshTransform) {.position = w->position, .rotation = {0.0f, pose.frame_yaw, 0.0f}, .scale = {1.0f, 1.0f, 1.0f}};
    wm->prev = (MeshTransform) {
        .parent = &wm->frame,
        .position = {pose.position[0], pose.position[1], pose.position[2]},
        .rotation = {pose.rotation[0], pose.rotation[1], 0.0f}, // rotateX, then rotateY: Euler XYZ
        .scale = {pose.scale, pose.scale, pose.scale},
    };
    wm->org_y = pose.position[1];
    wm->item_id = w->item_id;
    if (item->type == PRIZE_WHEEL_ITEM_DYE) {
        wm->dye_player = dye_player_new(w->item_id, &wm->prev);
        if (!wm->dye_player) KN_LOG_VERBOSE2("[prize] wheel %d: dye %d has no preview player\n", wm->uid, w->item_id);
        return;
    }
    if (item->type == PRIZE_WHEEL_ITEM_SPRAY) {
        wm->spray = spray_plane(item, &wm->prev, pose.item_rot_y);
        wm->spray_frames = item->frames > 0 && item->frame_t > 0 ? item->frames : 0;
        wm->spray_frame_t = item->frame_t;
        if (wm->spray) scene_add_mesh(g_pwr.scene, wm->spray);
        return;
    }
    // loadMesh / loadAnimMesh({src, texSrc, glowText: glow || sameGlow, emissive: glow || sameGlow ? "#ffffff" : null,
    // ...}, 0, 0, 0, item_rot_y, item_scale, meshPrev) (:219527-219565)
    const int glow = (item->flags & (VS_GLOW | VS_SAME_GLOW)) != 0;
    if (!store_item_model_build(item, &wm->prev, (vec3) {0.0f, 0.0f, 0.0f}, pose.item_rot_y, pose.item_scale, glow, glow, 0, &wm->item)) {
        KN_LOG_VERBOSE2("[prize] wheel %d: item %d has no model\n", wm->uid, w->item_id);
        return;
    }
    if (wm->item.mesh) scene_add_mesh(g_pwr.scene, wm->item.mesh);
    if (wm->item.model) scene_add_gltf(g_pwr.scene, wm->item.model);
}

int prize_wheel_render_item(const int uid, vec3 *at) {
    for (int i = 0; i < g_pwr.count; i++) {
        const WheelMesh *wm = &g_pwr.wheels[i];
        if (wm->uid != uid) continue;
        if (wm->item_id < 0) return -1;
        if (at) {
            Mesh *mesh = wm->item.mesh ? wm->item.mesh : wm->spray;
            const PlayerMesh *dye = wm->dye_player ? wm->dye_player->mesh : NULL;
            if (!mesh && dye && dye->body && dye->body->mesh_count) mesh = dye->body->meshes[0]; // the dyed body
            if (mesh) {
                mesh_update_transform_matrix(mesh);
                *at = (vec3) {mesh->transform_matrix[3], mesh->transform_matrix[7], mesh->transform_matrix[11]}; // row-major (mesh.c)
            } else {
                *at = wm->frame.position;
            }
        }
        return wm->item_id;
    }
    return -1;
}

void prize_wheel_render_frame(Client *client) {
    if (!client || !g_pwr.count) return;
    for (int i = 0; i < g_pwr.count; i++) {
        WheelMesh *wm = &g_pwr.wheels[i];
        const PrizeWheel *w = prize_wheels_get(client, wm->uid);
        if (!w || w->object != wm->object) continue; // the spin state is still on another map
        // meshPrev: up at the stop, down at the next spin; its bob follows the steps
        if (w->item_shown && wm->item_id != w->item_id) item_up(wm, w);
        else if (!w->item_shown && wm->item_id >= 0) item_down(wm);
        if (wm->item_id >= 0) {
            wm->prev.position.y = prize_wheel_item_bob(wm->org_y, w->bob_anim);
            if (wm->item.model && client_render_settings()->object_anim) gltf_instance_update(wm->item.model, gltf_clock());
            if (wm->spray && wm->spray_frames && client_render_settings()->texture_anim) // frameMats: a frame every frameT ms
                ((BasicMaterial *) wm->spray->material)->texture_offset.x = (float) ((long long) (gltf_clock() * 1000.0 / wm->spray_frame_t) % wm->spray_frames);
        }
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
