// Player sprays: Krunker v3.1.1 RENDER.addSpray / clearSprays (src/libs/render.js) and the
// "sp" handler addSpray (src/index.js). One decal per player sid (a new spray moves it): a
// THREE.PlaneGeometry(1, 1) scaled to config.sprayScale, rotation.y = xDr, rotation.x = yDr
// (degrees), with getMat("sprays/<id>", {depthWrite: false, opacity, transparent: true,
// frames, frameT, movT}): the default lit, fogged, front-side material, receiveShadow.
// The 3D sprays (is3D) of later versions do not exist in v3.1.1's tables.
#include <gl.h>
#include <sprays.h>
#include <render_settings.h>
#include "viewer/viewer_store.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define SPRAYS_MAX 128 // live decals (one per sprayer); Krunker's list is unbounded

typedef struct {
    int id;        // textures/sprays/<id>.png
    float opacity;
    int frames;    // horizontal sprite-sheet frames, 0 = none
    int frame_t;   // ms per frame
} SprayDef;

// src/data/sprays.js (Game.sprays): id, opacity, frames, frameT
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

// addSpray(sid, sprayIndex, ...): Game.sprays[i], else store.skins[i] of type 4, else Game.sprays[0]
static SprayDef spray_resolve(const int index, float *mov_t) {
    *mov_t = 0.0f;
    if (index >= 0 && index < SPRAYS_BASE_COUNT) return g_sprays[index];
    const ViewerSkin *skin = index >= 0 ? viewer_skin(index) : NULL;
    if (skin && skin->type == 4 && skin->id >= 0) {
        *mov_t = skin->mov_t;
        return (SprayDef) {skin->id, skin->opacity > 0.0f ? skin->opacity : 1.0f, skin->frames, skin->frame_t};
    }
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

static Spray *spray_slot(Client *client, const int uid) {
    for (int i = 0; i < g_spray.count; i++) {
        if (g_spray.sprays[i].uid == uid) return &g_spray.sprays[i];
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
    *spray = (Spray) {.uid = uid, .mesh = mesh, .material = material};
    return spray;
}

void sprays_add(Client *client, const NetSprayPacket *packet) {
    if (!client || !packet || !client->game.ready) return;
    if (!isfinite(packet->position[0]) || !isfinite(packet->position[1]) || !isfinite(packet->position[2])) return;
    Spray *spray = spray_slot(client, packet->uid);
    if (!spray) return;

    float mov_t;
    const SprayDef def = spray_resolve(packet->spray_index, &mov_t);
    BasicMaterial *material = spray->material;
    material->texture = spray_texture(def.id);
    material->color.w = def.opacity;
    material->texture_repeat.x = def.frames > 0 ? 1.0f / (float) def.frames : 1.0f;
    material->texture_offset = (vec2) {0.0f, 0.0f};
    spray->frames = def.frames > 0 && def.frame_t > 0 ? def.frames : 0;
    spray->frame_t = def.frame_t;
    spray->mov_t = mov_t;

    Mesh *mesh = spray->mesh;
    mesh->transform.position = (vec3) {packet->position[0], packet->position[1], packet->position[2]};
    mesh->transform.rotation = (vec3) {(float) packet->x_rot * (float) M_PI / 180.0f, (float) packet->y_rot * (float) M_PI / 180.0f, 0.0f};
    mesh->visible = 1;
    mesh_update_transform_matrix(mesh);
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
