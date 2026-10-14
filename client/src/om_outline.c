// KrunkNative Object Manager outline (client/include/om_outline.h). Per colour: the meshes' silhouette
// into the stencil (no colour, no depth test), then the same meshes flat in that colour, shifted by
// the ring width in eight directions on screen, where the stencil is not set: a ring of constant
// pixel width around any shape (models, cubes, players), no normals needed. GL state is set and put
// back here (scene_render sets its own every call).
#include <gl.h> // first: glad before any GL header
#include <om_outline.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define OUTLINE_MAX 256

static struct {
    Mesh *mesh;
    int rgb;
} g_queue[OUTLINE_MAX];
static int g_count;
static unsigned int g_program;
static int g_failed;

static GlUniform u_transform = GL_UNIFORM_INIT("transform");
static GlUniform u_camera_world_inverse = GL_UNIFORM_INIT("camera_world_inverse");
static GlUniform u_camera_projection = GL_UNIFORM_INIT("camera_projection");
static GlUniform u_offset = GL_UNIFORM_INIT("offset");
static GlUniform u_color = GL_UNIFORM_INIT("color");

void om_outline_add(Mesh *mesh, const int rgb) {
    if (!mesh || !mesh->geometry || g_count >= OUTLINE_MAX) return;
    for (int i = 0; i < g_count; i++) {
        if (g_queue[i].mesh == mesh) return;
    }
    g_queue[g_count].mesh = mesh;
    g_queue[g_count].rgb = rgb;
    g_count++;
}

void om_outline_add_player(const Player *player, const int rgb) {
    if (!player || !player->mesh) return;
    Scene parts = {0, NULL}; // the meshes scene_add_player_mesh would draw
    scene_add_player_mesh(&parts, (const PlayerMesh *) player->mesh, (size_t) player->loadout_size);
    for (size_t i = 0; i < parts.mesh_count; i++) om_outline_add(parts.meshes[i], rgb);
    free(parts.meshes);
}

int om_outline_add_object(const Object *object, const int rgb) {
    Mesh *mesh = object ? object->mesh : NULL;
    if (!mesh || !mesh->visible || !mesh->geometry || mesh->geometry->draw_mode == GL_LINES) return 0;
    om_outline_add(mesh, rgb);
    return 1;
}

static int ol_program(void) {
    if (g_program || g_failed) return g_program != 0;
    char *vert = concat(assets_path(), "shaders/outline.vert");
    char *frag = concat(assets_path(), "shaders/outline.frag");
    if (vert && frag) {
        ensure_asset_file(vert);
        ensure_asset_file(frag);
        g_program = shader_compile(vert, frag);
    }
    free(vert);
    free(frag);
    g_failed = !g_program;
    return g_program != 0;
}

static void ol_draw(const int rgb) {
    for (int i = 0; i < g_count; i++) {
        Mesh *mesh = g_queue[i].mesh;
        if (g_queue[i].rgb != rgb || !mesh->visible) continue;
        mesh_update_transform_matrix(mesh);
        glBindVertexArray(mesh->geometry->vao);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh->geometry->ebo);
        gl_uniform_mat4(&u_transform, g_program, GL_TRUE, mesh->transform_matrix);
        glDrawElements(GL_TRIANGLES, mesh->geometry->index_count, GL_UNSIGNED_INT, NULL);
    }
}

void om_outline_render(Camera *camera) {
    if (!g_count || !camera || !ol_program()) {
        g_count = 0;
        return;
    }
    int viewport[4];
    glGetIntegerv(GL_VIEWPORT, viewport);
    if (viewport[2] <= 0 || viewport[3] <= 0) {
        g_count = 0;
        return;
    }
    camera_update_projection_matrix(camera, (float) viewport[2] / (float) viewport[3]);
    camera_update_world_inverse_matrix(camera);
    const GLboolean blend = glIsEnabled(GL_BLEND), depth = glIsEnabled(GL_DEPTH_TEST), cull = glIsEnabled(GL_CULL_FACE);

    glUseProgram(g_program);
    g_active_shader = g_program;
    gl_uniform_mat4(&u_camera_world_inverse, g_program, GL_TRUE, camera->world_inverse_matrix);
    gl_uniform_mat4(&u_camera_projection, g_program, GL_TRUE, camera->projection_matrix);
    glDisable(GL_DEPTH_TEST); // seen through what stands in front
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
#ifndef KN_GLES
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL); // (a wireframe material may have left GL_LINE)
#endif
    glEnable(GL_STENCIL_TEST);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    // the ring width: 2 px at 720p, 3 at 1080p, ...
    const float px = fmaxf(2.0f, roundf((float) viewport[3] / 360.0f));
    static const float k_dirs[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {0.7071f, 0.7071f}, {-0.7071f, 0.7071f}, {0.7071f, -0.7071f}, {-0.7071f, -0.7071f}};
    int done[OUTLINE_MAX] = {0};
    for (int i = 0; i < g_count; i++) {
        if (done[i]) continue;
        const int rgb = g_queue[i].rgb;
        for (int k = i; k < g_count; k++) done[k] |= g_queue[k].rgb == rgb;
        // 1: the silhouette into the stencil
        glStencilMask(0xff);
        glClearStencil(0);
        glClear(GL_STENCIL_BUFFER_BIT);
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        glStencilFunc(GL_ALWAYS, 1, 0xff);
        glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
        gl_uniform_2f(&u_offset, g_program, 0.0f, 0.0f);
        ol_draw(rgb);
        // 2: the ring, outside it
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glStencilMask(0x00);
        glStencilFunc(GL_NOTEQUAL, 1, 0xff);
        glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
        gl_uniform_4f(&u_color, g_program, (float) ((rgb >> 16) & 255) / 255.0f, (float) ((rgb >> 8) & 255) / 255.0f, (float) (rgb & 255) / 255.0f, 1.0f);
        for (int d = 0; d < 8; d++) {
            gl_uniform_2f(&u_offset, g_program, k_dirs[d][0] * px * 2.0f / (float) viewport[2], k_dirs[d][1] * px * 2.0f / (float) viewport[3]);
            ol_draw(rgb);
        }
    }

    glStencilMask(0xff);
    glDisable(GL_STENCIL_TEST);
    glBindVertexArray(0);
    glDepthMask(GL_TRUE);
    if (depth) glEnable(GL_DEPTH_TEST);
    if (blend) glEnable(GL_BLEND);
    if (cull) glEnable(GL_CULL_FACE);
    g_count = 0;
}

void om_outline_fini(void) {
    g_count = 0;
    if (!g_program) return;
    if (g_active_shader == g_program) g_active_shader = 0;
    glDeleteProgram(g_program);
    gl_uniform_forget_program(g_program);
    g_program = 0;
    g_failed = 0;
}
