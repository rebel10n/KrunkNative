// Krunker v7.1.4 battle-royale zone visual (libs/Zone.js, client side): an open
// CylinderGeometry(1, 1, 1, 32, 32) centred on y 0, MeshBasicMaterial(white, vertexColors,
// DoubleSide, transparent, opacity 0.5, depthWrite false) with zone_0 repeated 20 x 20 and
// scrolling (offset.y += delta * 0.00003), the zoneCol0 / 1 / 2 gradient (top, middle, bottom)
// multiplied in. Scaled (circle x, zoneHeight, circle z) at (x, 0, z), hidden at size 0. The
// circle comes from shared zone_circle() (the 900 ms tween of the server's "zn").
// Difference (documented): v7 multiplies the gradient after fog; here it is a vertex colour,
// exact along the 32 rows but fogged with the rest of the colour.
#include <gl.h>
#include <terrain_mesh.h>
#include <terrain.h>
#include <world.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    Mesh *mesh;
    Scene *scene;
    unsigned int vbo, cbo;
    float offset;
} ZoneMesh;

static ZoneMesh g_zone;

void zone_mesh_load(Scene *scene, const Map *map) {
    zone_mesh_unload();
    if (!scene || !map || !map->zone.enabled) return;

    float cols[3][3];
    for (int k = 0; k < 3; k++) {
        uint8_t c[3];
        terrain_hex(map->zone.col[k], "#c542d9", c); // zoneColN || COLORS.zone.BR[N]
        for (int j = 0; j < 3; j++) cols[k][j] = c[j] / 255.0f;
    }

    const int rs = 32, hs = 32;
    const int nv = (rs + 1) * (hs + 1), ni = rs * hs * 6;
    vertex *v = calloc((size_t) nv, sizeof(vertex));
    float *c = calloc((size_t) nv * 3, sizeof(float));
    unsigned int *idx = calloc((size_t) ni, sizeof(unsigned int));
    Geometry *g = calloc(1, sizeof(Geometry));
    BasicMaterial *m = basic_material_init();
    if (!v || !c || !idx || !g || !m) goto fail;

    for (int y = 0; y <= hs; y++) {
        const float t = (float) y / (float) hs;  // 0 at the top
        const float uv_v = 1.0f - t;              // three.js CylinderGeometry uv.y
        // getGradient: mix(mix(end, middle, v / 0.5), mix(middle, first, (v - 0.5) / 0.5), step(0.5, v))
        float grad[3];
        for (int j = 0; j < 3; j++) {
            grad[j] = uv_v < 0.5f ? cols[2][j] + (cols[1][j] - cols[2][j]) * (uv_v / 0.5f)
                                  : cols[1][j] + (cols[0][j] - cols[1][j]) * ((uv_v - 0.5f) / 0.5f);
        }
        for (int x = 0; x <= rs; x++) {
            const float u = (float) x / (float) rs, th = u * 2.0f * (float) M_PI;
            const int k = y * (rs + 1) + x;
            v[k] = (vertex) {{sinf(th), 0.5f - t, cosf(th)}, {u, uv_v}};
            memcpy(&c[k * 3], grad, sizeof(grad));
        }
    }
    int n = 0;
    for (int x = 0; x < rs; x++) {
        for (int y = 0; y < hs; y++) {
            const unsigned int a = (unsigned int) (y * (rs + 1) + x), b = (unsigned int) ((y + 1) * (rs + 1) + x);
            const unsigned int tri[6] = {a, b, a + 1, b, b + 1, a + 1};
            memcpy(idx + n, tri, sizeof(tri));
            n += 6;
        }
    }

    glGenVertexArrays(1, &g->vao);
    glGenBuffers(1, &g->ebo);
    glGenBuffers(1, &g_zone.vbo);
    glGenBuffers(1, &g_zone.cbo);
    glBindVertexArray(g->vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_zone.vbo);
    glBufferData(GL_ARRAY_BUFFER, (long) nv * (long) sizeof(vertex), v, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(vertex), NULL);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(vertex), (void *) (3 * sizeof(float)));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glBindBuffer(GL_ARRAY_BUFFER, g_zone.cbo);
    glBufferData(GL_ARRAY_BUFFER, (long) nv * 3 * (long) sizeof(float), c, GL_STATIC_DRAW);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), NULL);
    glEnableVertexAttribArray(2);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g->ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (long) ni * (long) sizeof(unsigned int), idx, GL_STATIC_DRAW);
    glBindVertexArray(0);
    g->index_count = ni; // bound_radius 0: never frustum culled (it is scaled every frame)

    char *path = concat(assets_path(), "textures/zone_0.png");
    m->texture = path ? load_texture(path) : 0;
    free(path);
    m->texture_repeat = (vec2) {20.0f, 20.0f};
    m->color = (vec4) {1.0f, 1.0f, 1.0f, 0.5f};
    m->unlit = 1;
    m->use_vertex_color = 1;
    m->double_sided = 1;
    m->no_depth_write = 1;
    m->base.transparent = 1;
    g_zone.mesh = mesh_init(g, (Material *) m);
    g_zone.mesh->visible = 0;
    g_zone.mesh->no_shadow_receive = 1;
    g_zone.scene = scene;
    scene_add_mesh(scene, g_zone.mesh);
    free(v);
    free(c);
    free(idx);
    return;
fail:
    free(v);
    free(c);
    free(idx);
    free(g);
    free(m);
}

void zone_mesh_unload(void) {
    if (g_zone.mesh) {
        if (g_zone.scene) scene_remove_mesh(g_zone.scene, g_zone.mesh);
        Geometry *g = g_zone.mesh->geometry;
        glDeleteVertexArrays(1, &g->vao);
        glDeleteBuffers(1, &g->ebo);
        glDeleteBuffers(1, &g_zone.vbo);
        glDeleteBuffers(1, &g_zone.cbo);
        free(g);
        mesh_fini(g_zone.mesh);
    }
    memset(&g_zone, 0, sizeof(g_zone));
}

void zone_mesh_frame(const Game *game, const float delta_ms) {
    if (!g_zone.mesh) return;
    float x, z, radius, height;
    if (!game || !zone_circle(game, &x, &z, &radius, &height)) {
        g_zone.mesh->visible = 0;
        return;
    }
    g_zone.mesh->visible = 1;
    g_zone.mesh->transform.position = (vec3) {x, 0.0f, z};
    g_zone.mesh->transform.scale = (vec3) {radius, height, radius};
    // Zone.animate: map.offset.y += delta * 0.00003 (offset in repeat units: / 20)
    g_zone.offset = fmodf(g_zone.offset + delta_ms * 0.00003f, 1.0f);
    ((BasicMaterial *) g_zone.mesh->material)->texture_offset.y = g_zone.offset / 20.0f;
}
