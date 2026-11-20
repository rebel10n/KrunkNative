// The replay viewer's hitbox outlines (hud_replay.h; Krunker v10.0.7 deob-10.0.7.js:229661-229689): while
// the toggle is on, an edges box around every active player that is not ours, 2 * (scale + hitboxPadW)
// wide and height + hitboxPadH tall from its feet (objInstances.position), yellow (0xffd000) for the
// suspect and red (0xff3838) for the others, depthTest false, drawn after the world. Krunker rebuilds
// the boxes every 50 ms; these are rebuilt every frame (the same boxes, no lag behind the models). Two
// LineMaterial meshes over one dynamic vertex buffer each, like nav_overlay.c's bot path.
#include <gl.h>
#include <client.h>
#include <demo_play.h>
#include <hud_replay.h>
#include <stdlib.h>
#include <string.h>

enum { RH_SUSPECT, RH_OTHERS, RH_LAYERS };
static const int k_hex[RH_LAYERS] = {0xffd000, 0xff3838};
#define RH_BOX_VERTS 24 // 12 edges

static struct {
    Mesh *meshes[RH_LAYERS];
    Geometry geometry[RH_LAYERS];
    GLuint vbo[RH_LAYERS];
    size_t vbo_floats[RH_LAYERS];
    GLuint ibo;
    size_t ibo_count;
    float *verts[RH_LAYERS];
    size_t n[RH_LAYERS], cap[RH_LAYERS];
} g_rh;

static int rh_push(const int layer, const float x, const float y, const float z) {
    if (g_rh.n[layer] + 3 > g_rh.cap[layer]) {
        const size_t cap = g_rh.cap[layer] ? g_rh.cap[layer] * 2 : RH_BOX_VERTS * 3 * 16;
        float *v = realloc(g_rh.verts[layer], cap * sizeof(float));
        if (!v) return 0;
        g_rh.verts[layer] = v;
        g_rh.cap[layer] = cap;
    }
    float *v = g_rh.verts[layer] + g_rh.n[layer];
    v[0] = x;
    v[1] = y;
    v[2] = z;
    g_rh.n[layer] += 3;
    return 1;
}

// EdgesGeometry(BoxGeometry(1, 1, 1)) scaled (w, h, w) at (x, y + h / 2, z): the 12 edges
static void rh_box(const int layer, const vec3 c, const float w, const float h) {
    const float x0 = c.x - w * 0.5f, x1 = c.x + w * 0.5f, y0 = c.y, y1 = c.y + h, z0 = c.z - w * 0.5f, z1 = c.z + w * 0.5f;
    const float p[8][3] = {{x0, y0, z0}, {x1, y0, z0}, {x1, y0, z1}, {x0, y0, z1}, {x0, y1, z0}, {x1, y1, z0}, {x1, y1, z1}, {x0, y1, z1}};
    static const int e[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6}, {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (int i = 0; i < 12; i++) {
        rh_push(layer, p[e[i][0]][0], p[e[i][0]][1], p[e[i][0]][2]);
        rh_push(layer, p[e[i][1]][0], p[e[i][1]][1], p[e[i][1]][2]);
    }
}

static int rh_indices(const size_t count) {
    if (g_rh.ibo && g_rh.ibo_count >= count) return 1;
    size_t n = g_rh.ibo_count ? g_rh.ibo_count : 256;
    while (n < count) n *= 2;
    unsigned int *indices = malloc(n * sizeof(unsigned int));
    if (!indices) return 0;
    for (size_t k = 0; k < n; k++) indices[k] = (unsigned int) k;
    if (!g_rh.ibo) glGenBuffers(1, &g_rh.ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_rh.ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr) (n * sizeof(unsigned int)), indices, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    free(indices);
    g_rh.ibo_count = n;
    return 1;
}

static int rh_upload(const int i) {
    Geometry *g = &g_rh.geometry[i];
    if (!g_rh.meshes[i]) {
        LineMaterial *material = line_material_init();
        if (!material) return 0;
        const int hex = k_hex[i];
        material->color = (vec4) {(float) ((hex >> 16) & 255) / 255.0f, (float) ((hex >> 8) & 255) / 255.0f, (float) (hex & 255) / 255.0f, 1.0f};
        material->depth_test = 0; // depthTest: false
        material->fog = 0;
        g->draw_mode = GL_LINES;
        glGenVertexArrays(1, &g->vao);
        glGenBuffers(1, &g_rh.vbo[i]);
        glBindVertexArray(g->vao);
        glBindBuffer(GL_ARRAY_BUFFER, g_rh.vbo[i]);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void *) 0);
        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        g_rh.meshes[i] = mesh_init(g, &material->base);
        if (!g_rh.meshes[i]) {
            free(material);
            return 0;
        }
    }
    const size_t count = g_rh.n[i] / 3;
    g->index_count = 0;
    if (!count || !rh_indices(count)) return count == 0;
    glBindBuffer(GL_ARRAY_BUFFER, g_rh.vbo[i]);
    if (g_rh.vbo_floats[i] < g_rh.n[i]) {
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr) (g_rh.cap[i] * sizeof(float)), NULL, GL_DYNAMIC_DRAW);
        g_rh.vbo_floats[i] = g_rh.cap[i];
    }
    glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr) (g_rh.n[i] * sizeof(float)), g_rh.verts[i]);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    g->ebo = g_rh.ibo;
    g->index_count = (int) count;
    return 1;
}

void replay_hitbox_render(Client *client) {
    if (!client || !demo_play_active() || !hud_replay_hitboxes()) return;
    const int suspect = demo_play_suspect_uid();
    g_rh.n[RH_SUSPECT] = g_rh.n[RH_OTHERS] = 0;
    for (size_t i = 0; i < client->game.player_count; i++) {
        const Player *p = client->game.players[i];
        if (!p || !p->active || p == client->me || p->is_you) continue;
        const float w = 2.0f * ((p->scale > 0.0f ? p->scale : game_constants.player_scale) + player_hitbox_pad_w(p));
        const float h = (p->height > 0.0f ? p->height : game_constants.player_height) + player_hitbox_pad_h(p);
        const PlayerMesh *pm = p->mesh; // objInstances.position
        const vec3 at = pm && pm->anchor ? pm->anchor->position : p->position;
        rh_box(p->uid == suspect ? RH_SUSPECT : RH_OTHERS, at, w, h);
    }
    for (int i = 0; i < RH_LAYERS; i++) {
        if (!rh_upload(i) || !g_rh.geometry[i].index_count) continue;
        Mesh *mesh = g_rh.meshes[i];
        const Scene layer = {1, &mesh};
        scene_render(&layer, &client->camera);
    }
}

void replay_hitbox_fini(void) {
    for (int i = 0; i < RH_LAYERS; i++) {
        if (g_rh.meshes[i]) mesh_fini(g_rh.meshes[i]);
        g_rh.meshes[i] = NULL;
        if (g_rh.geometry[i].vao) glDeleteVertexArrays(1, &g_rh.geometry[i].vao);
        if (g_rh.vbo[i]) glDeleteBuffers(1, &g_rh.vbo[i]);
        memset(&g_rh.geometry[i], 0, sizeof(g_rh.geometry[i]));
        g_rh.vbo[i] = 0;
        g_rh.vbo_floats[i] = 0;
        free(g_rh.verts[i]);
        g_rh.verts[i] = NULL;
        g_rh.n[i] = g_rh.cap[i] = 0;
    }
    if (g_rh.ibo) glDeleteBuffers(1, &g_rh.ibo);
    g_rh.ibo = 0;
    g_rh.ibo_count = 0;
}
