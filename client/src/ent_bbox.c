// Krunker v7.1.4 developer console `ent_bbox` (client/include/ent_bbox.h): the movement boxes of the players and
// map objects (client/src/ent_bbox_geom.c) as LineMaterial meshes, one per layer over one dynamic vertex buffer
// each, depthTest false, drawn after the world (like nav_overlay.c's bot path and replay_hitbox.c's outlines).
#include <gl.h>
#include <ent_bbox.h>
#include <ent_bbox_geom.h>
#include <console.h>
#include <third_person.h> // third_person_view: the own box only when the camera is outside it
#include <stdlib.h>
#include <string.h>

#define COL_INFO "#9ac8ff"
#define COL_ERROR "#ff6a6a"

static struct {
    int on;
    EntBboxGeom geom;
    Mesh *meshes[ENT_BBOX_LAYERS];
    Geometry geometry[ENT_BBOX_LAYERS];
    GLuint vbo[ENT_BBOX_LAYERS];
    size_t vbo_floats[ENT_BBOX_LAYERS];
    GLuint ibo;
    size_t ibo_count;
} g_eb;

static void eb_meshes_free(void) {
    for (int i = 0; i < ENT_BBOX_LAYERS; i++) {
        if (g_eb.meshes[i]) mesh_fini(g_eb.meshes[i]); // frees the LineMaterial
        g_eb.meshes[i] = NULL;
        if (g_eb.geometry[i].vao) glDeleteVertexArrays(1, &g_eb.geometry[i].vao);
        if (g_eb.vbo[i]) glDeleteBuffers(1, &g_eb.vbo[i]);
        memset(&g_eb.geometry[i], 0, sizeof(g_eb.geometry[i]));
        g_eb.vbo[i] = 0;
        g_eb.vbo_floats[i] = 0;
    }
    if (g_eb.ibo) glDeleteBuffers(1, &g_eb.ibo);
    g_eb.ibo = 0;
    g_eb.ibo_count = 0;
}

// 0 .. count-1 (GL_LINES over sequential vertices), grown as needed
static int eb_indices(const size_t count) {
    if (g_eb.ibo && g_eb.ibo_count >= count) return 1;
    size_t n = g_eb.ibo_count ? g_eb.ibo_count : 1024;
    while (n < count) n *= 2;
    unsigned int *indices = malloc(n * sizeof(unsigned int));
    if (!indices) return 0;
    for (size_t k = 0; k < n; k++) indices[k] = (unsigned int) k;
    if (!g_eb.ibo) glGenBuffers(1, &g_eb.ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_eb.ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr) (n * sizeof(unsigned int)), indices, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    free(indices);
    g_eb.ibo_count = n;
    return 1;
}

static int eb_upload(const int i) {
    const EntBboxVerts *v = &g_eb.geom.layers[i];
    Geometry *g = &g_eb.geometry[i];
    if (!g_eb.meshes[i]) {
        LineMaterial *material = line_material_init();
        if (!material) return 0;
        const int hex = ent_bbox_layer_hex[i];
        material->color = (vec4) {(float) ((hex >> 16) & 255) / 255.0f, (float) ((hex >> 8) & 255) / 255.0f, (float) (hex & 255) / 255.0f, 1.0f};
        material->depth_test = 0;
        material->fog = 0;
        g->draw_mode = GL_LINES;
        glGenVertexArrays(1, &g->vao);
        glGenBuffers(1, &g_eb.vbo[i]);
        glBindVertexArray(g->vao);
        glBindBuffer(GL_ARRAY_BUFFER, g_eb.vbo[i]);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void *) 0);
        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        g_eb.meshes[i] = mesh_init(g, &material->base);
        if (!g_eb.meshes[i]) {
            free(material);
            return 0;
        }
    }
    const size_t count = v->n / 3;
    g->index_count = 0;
    if (!count) return 1;
    if (!eb_indices(count)) return 0;
    glBindBuffer(GL_ARRAY_BUFFER, g_eb.vbo[i]);
    if (g_eb.vbo_floats[i] < v->n) { // grown: a new store (GLES 3.0: glBufferData, no persistent mapping)
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr) (v->cap * sizeof(float)), NULL, GL_DYNAMIC_DRAW);
        g_eb.vbo_floats[i] = v->cap;
    }
    glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr) (v->n * sizeof(float)), v->v);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    g->ebo = g_eb.ibo;
    g->index_count = (int) count;
    return 1;
}

static int eb_draw_self(const Client *client) { return third_person_view(client); }

void ent_bbox_command(Client *client, const int argc, char **argv) {
    int on = !g_eb.on;
    if (argc && (!strcmp(argv[0], "off") || !strcmp(argv[0], "0") || !strcmp(argv[0], "clear"))) on = 0;
    else if (argc && (!strcmp(argv[0], "on") || !strcmp(argv[0], "1"))) on = 1;
    if (!on) {
        g_eb.on = 0;
        ui_console_printf(COL_INFO, "ent_bbox: off");
        return;
    }
    if (!client || !client->game.ready || !client->game.map) {
        ui_console_printf(COL_ERROR, "ent_bbox: no game running (are you in a game?)");
        return;
    }
    g_eb.on = 1;
    ent_bbox_geom_build(&client->game, client->me, eb_draw_self(client), &g_eb.geom); // the counts for the answer
    ui_console_printf(COL_INFO, "ent_bbox: on (%d player boxes, %d object boxes)", g_eb.geom.players, g_eb.geom.objects);
}

int ent_bbox_active(void) { return g_eb.on; }

void ent_bbox_render(Client *client) {
    if (!g_eb.on || !client || !client->game.ready || !client->game.map) return;
    if (!ent_bbox_geom_build(&client->game, client->me, eb_draw_self(client), &g_eb.geom)) return;
    for (int i = 0; i < ENT_BBOX_LAYERS; i++) {
        if (!eb_upload(i) || !g_eb.geometry[i].index_count) continue;
        Mesh *mesh = g_eb.meshes[i];
        const Scene layer = {1, &mesh}; // one scene per layer: with depth testing off the order decides
        scene_render(&layer, &client->camera);
    }
}

void ent_bbox_fini(void) {
    eb_meshes_free();
    ent_bbox_geom_free(&g_eb.geom);
    g_eb.on = 0;
}
