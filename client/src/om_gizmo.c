// KrunkNative Object Manager world overlay (client/include/om_gizmo.h): one dynamic GL_LINES mesh
// per colour (a LineMaterial has one colour), refilled every frame; the GL objects are made on the
// first lines and kept until om_gizmo_fini. Only GL calls of both GL 4.5 core and GLES 3.0.
#include <gl.h> // first: glad before any GL header
#include <om_gizmo.h>
#include <stdlib.h>
#include <string.h>

static const int k_hex[OM_GIZMO_COLORS] = {0xffffff, 0xffd23f, 0xff4d4d, 0x4dff70, 0x4d8bff, 0x46e6ff, 0xff8af2};

static struct {
    float *v;            // x y z per vertex, two per line
    size_t n, cap;       // floats
    unsigned int *index; // 0..vertices-1 (scene_render draws elements)
    size_t index_cap;
    Geometry geometry;
    unsigned int vbo;
    Mesh *mesh;
} g_gz[OM_GIZMO_COLORS];

void om_gizmo_clear(void) {
    for (int i = 0; i < OM_GIZMO_COLORS; i++) g_gz[i].n = 0;
}

static void gz_push(const int color, const vec3 p) {
    if (color < 0 || color >= OM_GIZMO_COLORS) return;
    if (g_gz[color].n + 3 > g_gz[color].cap) {
        const size_t cap = g_gz[color].cap ? g_gz[color].cap * 2 : 384;
        float *grown = realloc(g_gz[color].v, cap * sizeof(float));
        if (!grown) return;
        g_gz[color].v = grown;
        g_gz[color].cap = cap;
    }
    g_gz[color].v[g_gz[color].n++] = p.x;
    g_gz[color].v[g_gz[color].n++] = p.y;
    g_gz[color].v[g_gz[color].n++] = p.z;
}

void om_gizmo_line(const int color, const vec3 a, const vec3 b) {
    gz_push(color, a);
    gz_push(color, b);
}

void om_gizmo_box(const int color, const vec3 feet, const vec3 size) {
    const float x0 = feet.x - size.x * 0.5f, x1 = feet.x + size.x * 0.5f;
    const float y0 = feet.y, y1 = feet.y + size.y;
    const float z0 = feet.z - size.z * 0.5f, z1 = feet.z + size.z * 0.5f;
    const vec3 c[8] = {{x0, y0, z0}, {x1, y0, z0}, {x1, y0, z1}, {x0, y0, z1}, {x0, y1, z0}, {x1, y1, z0}, {x1, y1, z1}, {x0, y1, z1}};
    static const int e[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6}, {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (int i = 0; i < 12; i++) om_gizmo_line(color, c[e[i][0]], c[e[i][1]]);
}

static int gz_upload(const int i) {
    const size_t verts = g_gz[i].n / 3;
    if (!g_gz[i].mesh) {
        LineMaterial *material = line_material_init();
        if (!material) return 0;
        const int hex = k_hex[i];
        material->color = (vec4) {(float) ((hex >> 16) & 255) / 255.0f, (float) ((hex >> 8) & 255) / 255.0f, (float) (hex & 255) / 255.0f, 1.0f};
        material->depth_test = 0; // the handles stay visible through what they belong to
        Geometry *g = &g_gz[i].geometry;
        g->draw_mode = GL_LINES;
        glGenVertexArrays(1, &g->vao);
        glGenBuffers(1, &g_gz[i].vbo);
        glGenBuffers(1, &g->ebo);
        glBindVertexArray(g->vao);
        glBindBuffer(GL_ARRAY_BUFFER, g_gz[i].vbo);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void *) 0);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g->ebo);
        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        g_gz[i].mesh = mesh_init(g, &material->base);
        if (!g_gz[i].mesh) {
            free(material);
            return 0;
        }
    }
    if (verts > g_gz[i].index_cap) {
        unsigned int *grown = realloc(g_gz[i].index, verts * sizeof(unsigned int));
        if (!grown) return 0;
        for (size_t k = g_gz[i].index_cap; k < verts; k++) grown[k] = (unsigned int) k;
        g_gz[i].index = grown;
        g_gz[i].index_cap = verts;
    }
    Geometry *g = &g_gz[i].geometry;
    glBindVertexArray(g->vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_gz[i].vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr) (g_gz[i].n * sizeof(float)), g_gz[i].v, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g->ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr) (verts * sizeof(unsigned int)), g_gz[i].index, GL_DYNAMIC_DRAW);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    g->index_count = (int) verts;
    return 1;
}

void om_gizmo_render(Camera *camera) {
    for (int i = 0; i < OM_GIZMO_COLORS; i++) {
        if (g_gz[i].n < 6 || !gz_upload(i)) continue;
        Mesh *mesh = g_gz[i].mesh;
        const Scene layer = {1, &mesh};
        scene_render(&layer, camera);
    }
}

void om_gizmo_fini(void) {
    for (int i = 0; i < OM_GIZMO_COLORS; i++) {
        if (g_gz[i].mesh) mesh_fini(g_gz[i].mesh); // frees the LineMaterial
        if (g_gz[i].geometry.vao) glDeleteVertexArrays(1, &g_gz[i].geometry.vao);
        if (g_gz[i].geometry.ebo) glDeleteBuffers(1, &g_gz[i].geometry.ebo);
        if (g_gz[i].vbo) glDeleteBuffers(1, &g_gz[i].vbo);
        free(g_gz[i].v);
        free(g_gz[i].index);
        memset(&g_gz[i], 0, sizeof(g_gz[i]));
    }
}
