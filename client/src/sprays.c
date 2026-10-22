// Player sprays: Krunker RENDER.addSpray / clearSprays (src/libs/render.js) behind the "sp"
// handler addSpray (src/index.js; v7.1.4 game.addSpray), and v7.1.4's spray wheel
// (src/libs/game-overlay.js:45-77, :1941-2018). A decal is a THREE.PlaneGeometry(1, 1) scaled to
// config.sprayScale, rotation.y = xDr, rotation.x = yDr, rotation.z (v7) in degrees, with
// getMat("sprays/<id>", {depthWrite: false, opacity, transparent: true, frames, frameT, movT}): the
// default lit, fogged, front-side material, receiveShadow.
//
// v7.1.4 keeps several sprays per player: the decal id is "<sid>_<n>" (NET_PACKET_SPRAY_V7 slot),
// a spray into a used slot moves that decal. Older servers send NET_PACKET_SPRAY: one decal per
// sid, index 0..52 = v3.1.1's Game.sprays (store items 2482 + i in v7).
#include <gl.h>
#include "klog.h"
#include <sprays.h>
#include <social_v7.h>
#include <canvas2d.h>
#include <audio.h>
#include <headless.h>
#include <html_dom.h>
#include <ui_logic.h>
#include <render_settings.h>
#include "viewer/viewer_store.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define SPRAYS_MAX 128 // live decals; Krunker's list is unbounded

typedef struct {
    int id;        // textures/sprays/<id>.png
    float opacity;
    int frames;    // horizontal sprite-sheet frames, 0 = none
    int frame_t;   // ms per frame
} SprayDef;

// src/data/sprays.js (v3.1.1 Game.sprays): id, opacity, frames, frameT. Older servers' indices.
static const SprayDef g_sprays[SPRAYS_BASE_COUNT] = {
    {0, 0.6f, 0, 0},    {1, 0.6f, 0, 0},    {2, 0.6f, 0, 0},    {3, 0.6f, 0, 0},    {4, 0.6f, 0, 0},
    {5, 0.6f, 0, 0},    {6, 0.6f, 0, 0},    {7, 0.7f, 0, 0},    {8, 0.9f, 0, 0},    {9, 0.9f, 0, 0},
    {10, 0.6f, 0, 0},   {11, 1.0f, 0, 0},   {12, 1.0f, 0, 0},   {13, 0.6f, 0, 0},   {14, 0.6f, 0, 0},
    {15, 0.7f, 0, 0},   {16, 0.6f, 0, 0},   {17, 0.6f, 0, 0},   {18, 0.6f, 0, 0},   {19, 0.6f, 0, 0},
    {20, 0.85f, 0, 0},  {21, 0.85f, 0, 0},  {22, 0.9f, 0, 0},   {23, 0.9f, 0, 0},   {24, 0.85f, 0, 0},
    {25, 0.85f, 0, 0},  {26, 0.9f, 0, 0},   {27, 0.85f, 0, 0},  {28, 0.9f, 0, 0},   {29, 0.85f, 0, 0},
    {30, 0.9f, 0, 0},   {31, 0.9f, 0, 0},   {32, 0.9f, 0, 0},   {33, 0.95f, 0, 0},  {34, 0.85f, 0, 0},
    {35, 0.85f, 0, 0},  {36, 0.9f, 0, 0},   {37, 0.9f, 0, 0},   {38, 0.9f, 0, 0},   {54, 0.9f, 0, 0},
    {55, 0.9f, 0, 0},   {56, 0.9f, 0, 0},   {57, 0.9f, 0, 0},   {58, 0.8f, 0, 0},   {59, 0.9f, 0, 0},
    {63, 0.9f, 0, 0},   {64, 0.9f, 0, 0},   {65, 0.9f, 0, 0},   {66, 0.9f, 0, 0},   {79, 0.9f, 0, 0},
    {83, 0.9f, 0, 0},   {87, 0.9f, 0, 0},   {88, 0.9f, 4, 250},
};

typedef struct {
    int uid;
    int slot;      // v7 "<sid>_<slot>", -1 for an older server's one-per-player decal
    Mesh *mesh;
    BasicMaterial *material;
    int frames, frame_t;
    float mov_t;   // texture.offset.x per ms (movTextures)
} Spray;

static struct {
    Scene *scene;
    Geometry *geometry;
    unsigned int vbo;
    int count;
    Spray sprays[SPRAYS_MAX];
} g_spray;

static SprayDef skin_def(const ViewerSkin *skin, float *mov_t) {
    *mov_t = skin->mov_t;
    return (SprayDef) {skin->id >= 0 ? skin->id : 0, skin->opacity > 0.0f ? skin->opacity : 1.0f, skin->frames, skin->frame_t};
}

// v3 addSpray(sid, sprayIndex, ...): Game.sprays[i], else store.skins[i] of type 4, else Game.sprays[0]
static SprayDef spray_resolve(const int index, float *mov_t) {
    *mov_t = 0.0f;
    if (index >= 0 && index < SPRAYS_BASE_COUNT) return g_sprays[index];
    const ViewerSkin *skin = index >= 0 ? viewer_skin(index) : NULL;
    if (skin && skin->type == SPRAY_ITEM_TYPE && skin->id >= 0) return skin_def(skin, mov_t);
    return g_sprays[0];
}

// v7 game.addSpray: store.skins[sprayId] when it is a SPRAY, else store.skins[2482]
static SprayDef spray_resolve_v7(const int index, float *mov_t) {
    const ViewerSkin *skin = index >= 0 ? viewer_skin(index) : NULL;
    if (!skin || skin->type != SPRAY_ITEM_TYPE) skin = viewer_skin(SPRAY_DEFAULT_INDEX);
    if (skin && skin->type == SPRAY_ITEM_TYPE) return skin_def(skin, mov_t);
    *mov_t = 0.0f;
    return g_sprays[0];
}

// THREE.PlaneGeometry(1, 1): the XY unit quad facing +z (CCW front), UVs 0..1.
static Geometry *spray_geometry(void) {
    if (g_spray.geometry) return g_spray.geometry;

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
    glGenBuffers(1, &g_spray.vbo);

    glBindVertexArray(geometry->vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_spray.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, geometry->ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), NULL);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *) (3 * sizeof(float)));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);

    geometry->index_count = (int) (sizeof(indices) / sizeof(indices[0]));
    geometry->bound_radius = 0.7072f; // half diagonal of the unit quad (frustum culling)
    g_spray.geometry = geometry;
    return geometry;
}

static unsigned int spray_texture(const int id) {
    char path[64];
    snprintf(path, sizeof(path), "textures/sprays/%d.png", id);
    char *full = concat(assets_path(), path);
    if (!full) return 0;
    const unsigned int texture = load_texture(full); // cached by path (ensure_asset_file on the web)
    free(full);
    return texture;
}

static Spray *spray_slot(Client *client, const int uid, const int slot) {
    for (int i = 0; i < g_spray.count; i++) {
        if (g_spray.sprays[i].uid == uid && g_spray.sprays[i].slot == slot) return &g_spray.sprays[i];
    }
    if (g_spray.count >= SPRAYS_MAX || !client->scene) return NULL;

    Geometry *geometry = spray_geometry();
    BasicMaterial *material = geometry ? basic_material_init() : NULL;
    if (!material) return NULL;
    material->base.transparent = 1; // transparent: true (drawn after the opaque world, sorted)
    material->no_depth_write = 1;   // depthWrite: false
    material->double_sided = 0;     // FrontSide
    Mesh *mesh = mesh_init(geometry, (Material *) material);
    if (!mesh) {
        material_fini((Material *) material);
        return NULL;
    }
    mesh->transform.scale = (vec3) {GAME_SPRAY_SCALE, GAME_SPRAY_SCALE, 1.0f};
    scene_add_mesh(client->scene, mesh); // receiveShadow = true (Mesh default)
    g_spray.scene = client->scene;

    Spray *spray = &g_spray.sprays[g_spray.count++];
    *spray = (Spray) {.uid = uid, .slot = slot, .mesh = mesh, .material = material};
    return spray;
}

// Returns 1 when the decal was placed (or moved).
static int spray_place(Client *client, const int uid, const int slot, const SprayDef def, const float mov_t, const float *position,
                       const int y_rot, const int x_rot, const int z_rot) {
    if (!client || !client->game.ready) return 0;
    if (!isfinite(position[0]) || !isfinite(position[1]) || !isfinite(position[2])) return 0;
    Spray *spray = spray_slot(client, uid, slot);
    if (!spray) return 0;

    BasicMaterial *material = spray->material;
    material->texture = spray_texture(def.id);
    material->color.w = def.opacity;
    material->texture_repeat.x = def.frames > 0 ? 1.0f / (float) def.frames : 1.0f;
    material->texture_offset = (vec2) {0.0f, 0.0f};
    spray->frames = def.frames > 0 && def.frame_t > 0 ? def.frames : 0;
    spray->frame_t = def.frame_t;
    spray->mov_t = mov_t;

    Mesh *mesh = spray->mesh;
    const float deg = (float) M_PI / 180.0f;
    mesh->transform.position = (vec3) {position[0], position[1], position[2]};
    mesh->transform.rotation = (vec3) {(float) x_rot * deg, (float) y_rot * deg, (float) z_rot * deg};
    mesh->visible = 1;
    mesh_update_transform_matrix(mesh);
    return 1;
}

void sprays_add(Client *client, const NetSprayPacket *packet) {
    if (!packet) return;
    float mov_t;
    const SprayDef def = spray_resolve(packet->spray_index, &mov_t);
    if (spray_place(client, packet->uid, -1, def, mov_t, packet->position, packet->y_rot, packet->x_rot, 0))
        KN_LOG_VERBOSE2("[sprays] decal %d skin=%d texture=textures/sprays/%d.png\n", packet->uid, packet->spray_index, def.id);
}

void sprays_add_v7(Client *client, const NetSprayV7Packet *packet) {
    if (!packet) return;
    float mov_t;
    const SprayDef def = spray_resolve_v7(packet->skin, &mov_t);
    if (spray_place(client, packet->uid, packet->slot, def, mov_t, packet->position, packet->y_rot, packet->x_rot, packet->z_rot))
        KN_LOG_VERBOSE2("[sprays] decal %d_%u skin=%d texture=textures/sprays/%d.png\n", packet->uid, (unsigned) packet->slot, packet->skin, def.id);
}

void sprays_tick(Client *client) {
    (void) client;
    if (!g_spray.count || !client_render_settings()->texture_anim) return; // textureAnim
    const double now_ms = glfwGetTime() * 1000.0;
    for (int i = 0; i < g_spray.count; i++) {
        Spray *spray = &g_spray.sprays[i];
        if (spray->frames) { // frameMats: next frame every frameT ms
            spray->material->texture_offset.x = (float) ((long long) (now_ms / spray->frame_t) % spray->frames);
        } else if (spray->mov_t != 0.0f) { // movTextures: offset.x += movT * delta
            spray->material->texture_offset.x = (float) fmod(now_ms * spray->mov_t, 1.0);
        }
    }
}

void sprays_clear(Client *client) {
    (void) client;
    for (int i = 0; i < g_spray.count; i++) {
        Spray *spray = &g_spray.sprays[i];
        if (g_spray.scene) scene_remove_mesh(g_spray.scene, spray->mesh);
        mesh_fini(spray->mesh); // frees the material; the geometry is shared (kept), textures belong to load_texture's cache
    }
    g_spray.count = 0;
}

int sprays_count(void) {
    return g_spray.count;
}

// ---------------------------------------------------------------- spray wheel

// overlay.sprayList / sprayWheel / sprayWheelTimer / sprayWheelIndex. The list is built when the
// wheel opens (updateSprayWheel runs on menu changes in v7; the inventory cannot change in game).
static struct {
    int open;            // sprayWheel: the key is held
    float timer;         // sprayWheelTimer (s)
    int index;           // sprayWheelIndex
    int list[SPRAY_WHEEL_MAX + 1];
    int count;
    int frame[SPRAY_WHEEL_MAX + 1];  // sprite-sheet frame (cur) per entry
    float frame_ms[SPRAY_WHEEL_MAX + 1]; // dur
    Canvas2D *canvas;
    int drawn;           // the canvas holds a wheel (cleared once after it closes)
} g_wheel;

void sprays_wheel_open(void) {
    if (g_wheel.open) return;
    g_wheel.count = ui_loadout_spray_wheel(g_wheel.list, SPRAY_WHEEL_MAX + 1);
    // v7 starts at sprayWheelIndex 15, the equipped spray only when the list is full (and reads
    // past the list otherwise: getSprayIndex().index throws). KrunkNative: the equipped spray, the
    // last entry, so a quick tap sprays it.
    g_wheel.index = g_wheel.count > 0 ? g_wheel.count - 1 : 0;
    for (int i = 0; i <= SPRAY_WHEEL_MAX; i++) g_wheel.frame[i] = 0, g_wheel.frame_ms[i] = -1.0f;
    g_wheel.open = 1;
    g_wheel.timer = 0.0f;
}

void sprays_wheel_close(void) {
    g_wheel.open = 0;
    g_wheel.timer = 0.0f;
}

int sprays_wheel_is_open(void) {
    return g_wheel.open;
}

int sprays_wheel_opened(void) {
    return g_wheel.open && g_wheel.timer >= SPRAY_WHEEL_DELAY; // sprayWheelOpened
}

int sprays_wheel_shown(void) {
    return g_wheel.drawn;
}

// sprayWheelChange(dir): wraps over the list (v7 wraps at sprayWheelMax whatever the list holds).
void sprays_wheel_change(const int step) {
    if (g_wheel.count <= 0) return;
    int i = g_wheel.index + step;
    if (i >= g_wheel.count) i = 0;
    if (i < 0) i = g_wheel.count - 1;
    g_wheel.index = i;
    audio_play("hover_0", 0.2f, 0, 1.0f); // window.playTick()
}

int sprays_wheel_selected(void) {
    return g_wheel.count > 0 && g_wheel.index >= 0 && g_wheel.index < g_wheel.count ? g_wheel.list[g_wheel.index] : SPRAY_DEFAULT_INDEX;
}

// index.js scaleBase(): the overlay scale (uiScale fits 1700x900 * (2 - scaleUI)), 1 with uiScaling off.
static double wheel_ui_scale(const float vw, const float vh) {
    if (ui_setting_is_false("uiScaling")) return 1.0;
    double scale_ui;
    if (!ui_setting_number("scaleUI", &scale_ui) || scale_ui != scale_ui) scale_ui = 0.8;
    const double custom = 2.0 - scale_ui;
    const double sx = vw / (custom * 1700.0), sy = vh / (custom * 900.0);
    return sy < sx ? sy : sx;
}

static C2DImage *wheel_image(Canvas2D *c, const ViewerSkin *skin) {
    char rel[64];
    snprintf(rel, sizeof(rel), "textures/sprays/%d.png", skin && skin->id >= 0 ? skin->id : 0); // skin.thumbnail
    char *full = concat(assets_path(), rel);
    C2DImage *img = full ? c2d_image(c, full) : NULL;
    free(full);
    return img;
}

// One thumbnail, its current sprite-sheet frame when the spray is animated.
static void wheel_draw_skin(Canvas2D *c, C2DImage *img, const ViewerSkin *skin, const int frame, const double x, const double y, const double size) {
    if (!img || c2d_image_state(img) != 1) return;
    int w = 0, h = 0;
    c2d_image_size(img, &w, &h);
    if (skin && skin->frames > 0) {
        const double fw = (double) w / skin->frames;
        c2d_draw_image_sub(c, img, fw * frame, 0, fw, h, x, y, size, size);
    } else {
        c2d_draw_image(c, img, x, y, size, size);
    }
}

static void wheel_advance(const int i, const ViewerSkin *skin, const float delta_ms) {
    if (!skin || skin->frames <= 0) return;
    if (g_wheel.frame_ms[i] < 0.0f) g_wheel.frame_ms[i] = (float) skin->frame_t;
    g_wheel.frame_ms[i] -= delta_ms;
    if (g_wheel.frame_ms[i] <= 0.0f) {
        g_wheel.frame_ms[i] = (float) skin->frame_t;
        if (++g_wheel.frame[i] >= skin->frames) g_wheel.frame[i] = 0;
    }
}

// game-overlay.js render: the spray wheel, centred, while it is open and the player is alive (no
// death screen) with the menu and the end screen down (v9926: endUI / menuHolder / the death UI
// shown hide it). v7 also hides it under a sniper's scope image (v9929), which KrunkNative draws
// on the page, not on this canvas: the wheel shows over it.
void sprays_wheel_render(Client *client, const float delta_seconds) {
    const float delta_ms = delta_seconds * 1000.0f;
    // io_killPlayer: sprayWheelClose() for the local player; a key still held at the respawn
    // does not bring it back (it opens on a press)
    if (g_wheel.open && client && client->me && !client->me->active) sprays_wheel_close();
    if (g_wheel.open) g_wheel.timer += delta_seconds; // sprayWheelUpdate
    const int show = sprays_wheel_opened() && g_wheel.count > 0 && client && client->me && client->me->active && !html_ui_is_menu_open() &&
                     !hud_ui_end_screen_up();
    if (!show && !g_wheel.drawn) return;

    GLFWwindow *window = headless_current_window();
    int fb_w = 0, fb_h = 0;
    if (window) glfwGetFramebufferSize(window, &fb_w, &fb_h);
    if (fb_w <= 0 || fb_h <= 0) return;
    float vw = 0, vh = 0, dpr = 1;
    html_dom_viewport(&vw, &vh, &dpr);
    if (vw <= 0 || vh <= 0) vw = (float) fb_w, vh = (float) fb_h;
    if (!g_wheel.canvas) g_wheel.canvas = c2d_create();
    Canvas2D *c = g_wheel.canvas;
    if (!c) return;
    if (c2d_width(c) != (int) vw || c2d_height(c) != (int) vh) c2d_set_size(c, (int) vw, (int) vh, fb_w, fb_h);
    c2d_clear_rect(c, 0, 0, vw, vh);
    g_wheel.drawn = show;
    if (!show) return;

    const double scale = wheel_ui_scale(vw, vh);
    const double w = vw / scale, h = vh / scale;
    c2d_save(c);
    c2d_scale(c, scale, scale);
    c2d_translate(c, w / 2, h / 2);
    const double radius = h / 2 * 0.4;
    c2d_set_global_alpha(c, 0.7);
    c2d_begin_path(c);
    c2d_arc(c, 0, 0, radius, 0, M_PI * 2, 0);
    c2d_set_fill_color(c, 0x17 / 255.0f, 0x17 / 255.0f, 0x17 / 255.0f, 1); // #171717
    c2d_fill(c);
    const double ring = radius * 1.33, ring_w = radius / 1.9;
    c2d_set_global_alpha(c, 1);
    c2d_set_line_width(c, ring_w);
    c2d_set_stroke_color(c, 0xcf / 255.0f, 0xcf / 255.0f, 0xcf / 255.0f, 1); // #cfcfcf
    c2d_begin_path(c);
    c2d_arc(c, 0, 0, ring, 0, M_PI * 2, 0);
    c2d_stroke(c);
    const double step = M_PI * 2 / g_wheel.count;
    const double start = step * g_wheel.index;
    c2d_set_stroke_color(c, 0x55 / 255.0f, 0xac / 255.0f, 0xee / 255.0f, 1); // #55acee: the chosen segment
    c2d_begin_path(c);
    c2d_arc(c, 0, 0, ring, start, start + step, 0);
    c2d_stroke(c);

    const ViewerSkin *chosen = viewer_skin(g_wheel.list[g_wheel.index]);
    const double offset = 50.0;
    float rgba[4] = {0xdb / 255.0f, 0xdb / 255.0f, 0xdb / 255.0f, 1};
    // rarity == null ? "#dbdbdb" : rarities[rarity].color (the free sprays carry no rarity)
    if (chosen && !(chosen->flags & VS_FREE) && chosen->rarity >= 0 && chosen->rarity < g_viewer_rarity_count)
        c2d_parse_color(g_viewer_rarities[chosen->rarity].color, rgba);
    c2d_set_fill_color(c, rgba[0], rgba[1], rgba[2], rgba[3]);
    c2d_set_font(c, 40, "GameFont");
    const char *name = chosen && chosen->name ? chosen->name : "";
    c2d_fill_text(c, name, -c2d_measure_text(c, name) / 2, 40.0 / 2 + offset, 0, 0);
    c2d_set_fill_color(c, 0xa3 / 255.0f, 0xa3 / 255.0f, 0xa3 / 255.0f, 1);
    c2d_set_font(c, 25, "GameFont");
    c2d_fill_text(c, "Spray", -c2d_measure_text(c, "Spray") / 2, 25.0 / 2 + offset + 50, 0, 0);
    wheel_advance(g_wheel.index, chosen, delta_ms);
    wheel_draw_skin(c, wheel_image(c, chosen), chosen, g_wheel.frame[g_wheel.index], -50, -50 - offset * 1.5, 100);
    // img/scroll_2.png beside the ring: v7.1.4 ships no such file (the image never loads)

    double angle = -(step / 2);
    for (int i = 0; i < g_wheel.count; i++) {
        angle += step;
        const ViewerSkin *skin = viewer_skin(g_wheel.list[i]);
        if (i != g_wheel.index) wheel_advance(i, skin, delta_ms);
        wheel_draw_skin(c, wheel_image(c, skin), skin, g_wheel.frame[i], ring * cos(angle) - 40, ring * sin(angle) - 40, 80);
    }
    c2d_restore(c);
    c2d_composite(c);
}

void sprays_wheel_fini(void) {
    if (g_wheel.canvas) c2d_destroy(g_wheel.canvas);
    g_wheel.canvas = NULL;
    g_wheel.drawn = 0;
}
