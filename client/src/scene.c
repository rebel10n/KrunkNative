#include <gl.h>
#include <client.h>
#include <gltf.h>
#include <shadow.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

Scene *scene_init() {
    Scene *scene = calloc(1, sizeof(Scene));
    return scene;
}

void scene_add_player_mesh(Scene *scene, const PlayerMesh *player_mesh, const size_t loadout_size) {
    for (int i = 0; i < PLAYER_COSMETIC_SLOTS; i++) {
        if (player_mesh->cosmetics[i]) scene_add_mesh(scene, player_mesh->cosmetics[i]);
        scene_add_gltf(scene, player_mesh->cosmetic_models[i]);
    }

    if (player_mesh->body) {
        for (size_t i = 0; i < player_mesh->body->mesh_count; i++) {
            scene_add_mesh(scene, player_mesh->body->meshes[i]);
        }
    }

    if (player_mesh->head) {
        for (size_t i = 0; i < player_mesh->head->mesh_count; i++) {
            scene_add_mesh(scene, player_mesh->head->meshes[i]);
        }
    }

    if (player_mesh->arms) {
        for (size_t i = 0; i < loadout_size; i++) {
            const PlayerArms *arms = player_mesh->arms[i];
            if (!arms) continue;

            if (arms->weapon_right) scene_add_mesh(scene, arms->weapon_right);
            if (arms->weapon_left) scene_add_mesh(scene, arms->weapon_left);
            for (int k = 0; k < 2; k++) scene_add_gltf(scene, arms->weapon_models[k]);
            if (arms->attachment) scene_add_mesh(scene, arms->attachment);
            if (arms->charm_mount) scene_add_mesh(scene, arms->charm_mount);
            if (arms->charm) scene_add_mesh(scene, arms->charm);
            if (arms->wrist) scene_add_mesh(scene, arms->wrist);
            scene_add_gltf(scene, arms->wrist_model);

            for (int j = 0; j < 2; j++) {
                const PlayerArmMesh *arm = j ? arms->left : arms->right;

                if (arm->extender) scene_add_mesh(scene, arm->extender);

                if (arm->upper) scene_add_mesh(scene, arm->upper);
                if (arm->joint) scene_add_mesh(scene, arm->joint);

                for (size_t k = 0; k < arm->lower->mesh_count; k++) {
                    scene_add_mesh(scene, arm->lower->meshes[k]);
                }
            }
        }
    }

    for (int i = 0; i < 2; i++) {
        ColorCube *leg = player_mesh->legs[i];
        if (!leg) continue;

        for (size_t j = 0; j < leg->mesh_count; j++) {
            scene_add_mesh(scene, leg->meshes[j]);
        }
    }

    for (int i = 0; i < 2; i++) {
        PlayerCrouchedLeg *leg = player_mesh->crouched_legs[i];
        if (!leg) continue;

        scene_add_mesh(scene, leg->upper);
        scene_add_mesh(scene, leg->joint);

        for (size_t j = 0; j < leg->lower->mesh_count; j++) {
            scene_add_mesh(scene, leg->lower->meshes[j]);
        }
    }
}

// NOTE: probably should optimize - scene_remove_mesh iterates over scene children until it finds the target mesh!
void scene_remove_player_mesh(Scene *scene, const PlayerMesh *player_mesh, const size_t loadout_size) {
    if (!player_mesh) return;

    for (int i = 0; i < PLAYER_COSMETIC_SLOTS; i++) {
        if (player_mesh->cosmetics[i]) scene_remove_mesh(scene, player_mesh->cosmetics[i]);
        scene_remove_gltf(scene, player_mesh->cosmetic_models[i]);
    }

    if (player_mesh->body) {
        for (size_t i = 0; i < player_mesh->body->mesh_count; i++) {
            scene_remove_mesh(scene, player_mesh->body->meshes[i]);
        }
    }

    if (player_mesh->head) {
        for (size_t i = 0; i < player_mesh->head->mesh_count; i++) {
            scene_remove_mesh(scene, player_mesh->head->meshes[i]);
        }
    }

    if (player_mesh->arms) {
        for (size_t i = 0; i < loadout_size; i++) {
            const PlayerArms *arms = player_mesh->arms[i];
            if (!arms) continue;

            if (arms->weapon_right) scene_remove_mesh(scene, arms->weapon_right);
            if (arms->weapon_left) scene_remove_mesh(scene, arms->weapon_left);
            for (int k = 0; k < 2; k++) scene_remove_gltf(scene, arms->weapon_models[k]);
            if (arms->attachment) scene_remove_mesh(scene, arms->attachment);
            if (arms->charm_mount) scene_remove_mesh(scene, arms->charm_mount);
            if (arms->charm) scene_remove_mesh(scene, arms->charm);
            if (arms->wrist) scene_remove_mesh(scene, arms->wrist);
            scene_remove_gltf(scene, arms->wrist_model);

            for (int j = 0; j < 2; j++) {
                const PlayerArmMesh *arm = j ? arms->left : arms->right;

                if (arm->extender) scene_remove_mesh(scene, arm->extender);

                if (arm->upper) scene_remove_mesh(scene, arm->upper);
                if (arm->joint) scene_remove_mesh(scene, arm->joint);

                for (size_t k = 0; k < arm->lower->mesh_count; k++) {
                    scene_remove_mesh(scene, arm->lower->meshes[k]);
                }
            }
        }
    }

    for (int i = 0; i < 2; i++) {
        ColorCube *leg = player_mesh->legs[i];
        if (!leg) continue;

        for (size_t j = 0; j < leg->mesh_count; j++) {
            scene_remove_mesh(scene, leg->meshes[j]);
        }
    }

    for (int i = 0; i < 2; i++) {
        PlayerCrouchedLeg *leg = player_mesh->crouched_legs[i];
        if (!leg) continue;

        scene_remove_mesh(scene, leg->upper);
        scene_remove_mesh(scene, leg->joint);

        for (size_t j = 0; j < leg->lower->mesh_count; j++) {
            scene_remove_mesh(scene, leg->lower->meshes[j]);
        }
    }
}

void scene_add_mesh(Scene *scene, Mesh *mesh) {
    Mesh **meshes = realloc(scene->meshes, (scene->mesh_count + 1) * sizeof(Mesh *));
    if (!meshes) return;

    scene->meshes = meshes;
    scene->meshes[scene->mesh_count] = mesh;
    scene->mesh_count++;
}

void scene_remove_mesh(Scene *scene, Mesh *mesh) {
    size_t index = -1;

    for (size_t i = 0; i < scene->mesh_count; i++) {
        if (scene->meshes[i] == mesh) {
            index = i;
            break;
        }
    }

    if (index == -1) return;

    memcpy(&scene->meshes[index], &scene->meshes[index + 1], (scene->mesh_count - index - 1) * sizeof(Mesh *));
    scene->mesh_count--;

    if (!scene->mesh_count) {
        free(scene->meshes);
        scene->meshes = NULL;
        return;
    }

    Mesh **meshes = realloc(scene->meshes, scene->mesh_count * sizeof(Mesh *));
    if (!meshes) return;

    scene->meshes = meshes;
}

const Scene *sort_scene;

int scene_depth_sort(const size_t *p_a_idx, const size_t *p_b_idx) {
    const Mesh *a = sort_scene->meshes[*p_a_idx];
    const Mesh *b = sort_scene->meshes[*p_b_idx];

    if (!a->visible) return 1;
    if (!b->visible) return -1;

    if (a->material->transparent || b->material->transparent) {
        if (!b->material->transparent) return 1;
        if (!a->material->transparent) return -1;

        const float a_depth = a->_camera_space_matrix[11];
        const float b_depth = b->_camera_space_matrix[11];

        return b_depth > a_depth ? -1 : 1;
    }

    return 0;
}

// Per-draw GL state of scene_render(), filtered against what the previous draw set.
// -1 = unknown (forces the call); only valid inside one scene_render() call.
typedef struct {
    int blend;
    int cull_back;
    int additive_blend;
    int depth_write;
    int cull_front; // map-G: glCullFace(GL_FRONT) while culling (three.js BackSide)
} SceneDrawState;

static SceneDrawState g_draw_pending;
static SceneDrawState g_draw_current;

void scene_draw_state(const int cull_back, const int additive_blend, const int depth_write) {
    g_draw_pending.cull_back = cull_back ? 1 : 0;
    g_draw_pending.additive_blend = additive_blend ? 1 : 0;
    g_draw_pending.depth_write = depth_write ? 1 : 0;
    g_draw_pending.cull_front = 0;
}

void scene_draw_cull_front(void) {
    g_draw_pending.cull_front = 1;
}

static void scene_apply_draw_state(void) {
    const SceneDrawState *want = &g_draw_pending;
    SceneDrawState *have = &g_draw_current;

    if (want->blend != have->blend) {
        if (want->blend) glEnable(GL_BLEND);
        else glDisable(GL_BLEND);
        have->blend = want->blend;
    }
    if (want->cull_back != have->cull_back) {
        if (want->cull_back) glEnable(GL_CULL_FACE); // glCullFace(GL_BACK) / glFrontFace(GL_CCW) set once per scene_render()
        else glDisable(GL_CULL_FACE);
        have->cull_back = want->cull_back;
    }
    if (want->additive_blend != have->additive_blend) {
        glBlendFunc(GL_SRC_ALPHA, want->additive_blend ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA);
        have->additive_blend = want->additive_blend;
    }
    if (want->depth_write != have->depth_write) {
        glDepthMask(want->depth_write ? GL_TRUE : GL_FALSE);
        have->depth_write = want->depth_write;
    }
    if (want->cull_front != have->cull_front) {
        glCullFace(want->cull_front ? GL_FRONT : GL_BACK);
        have->cull_front = want->cull_front;
    }
}

// Frustum planes (a, b, c, d; inside when a*x + b*y + c*z + d >= 0) of a row-major
// view-projection matrix (Gribb/Hartmann), normalised so distances are in world units.
static void scene_frustum_planes(const float *vp, float planes[6][4]) {
    for (int i = 0; i < 6; i++) {
        const int row = i / 2;
        const float sign = i % 2 ? -1.0f : 1.0f;
        float length = 0.0f;
        for (int j = 0; j < 4; j++) {
            planes[i][j] = vp[12 + j] + sign * vp[row * 4 + j];
            if (j < 3) length += planes[i][j] * planes[i][j];
        }
        length = sqrtf(length);
        if (length > 0.0f) {
            for (int j = 0; j < 4; j++) planes[i][j] /= length;
        }
    }
}

// three.js frustumCulled: 1 when the mesh's transformed bounding sphere lies entirely
// outside the view frustum (GL would clip every triangle). Geometry without bounds is
// always drawn.
static int scene_mesh_culled(const Mesh *mesh, const float planes[6][4]) {
    const Geometry *geometry = mesh->geometry;
    if (!geometry || geometry->bound_radius <= 0.0f) return 0;

    const float *m = mesh->transform_matrix; // row-major, translation in m[3], m[7], m[11]
    const float *c = geometry->bound_center;
    float center[3];
    float scale = 0.0f;
    for (int row = 0; row < 3; row++) {
        center[row] = m[row * 4] * c[0] + m[row * 4 + 1] * c[1] + m[row * 4 + 2] * c[2] + m[row * 4 + 3];
        const float column = m[row] * m[row] + m[4 + row] * m[4 + row] + m[8 + row] * m[8 + row];
        if (column > scale) scale = column;
    }
    const float radius = geometry->bound_radius * sqrtf(scale);

    for (int i = 0; i < 6; i++) {
        const float *p = planes[i];
        if (p[0] * center[0] + p[1] * center[1] + p[2] * center[2] + p[3] < -radius) return 1;
    }
    return 0;
}

static GlUniform u_transform = GL_UNIFORM_INIT("transform");
static GlUniform u_camera_world_inverse = GL_UNIFORM_INIT("camera_world_inverse");
static GlUniform u_camera_projection = GL_UNIFORM_INIT("camera_projection");
static GlUniform u_camera_position = GL_UNIFORM_INIT("camera_position");

void scene_render(const Scene *scene, Camera *camera) {
    int viewport[4];
    glGetIntegerv(GL_VIEWPORT, viewport);

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL); // three.js LessEqualDepth: coplanar layers (stacked liquids, decals) all draw
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glActiveTexture(GL_TEXTURE0); // materials bind their base map to unit 0
    glCullFace(GL_BACK);          // FrontSide materials (scene_draw_state cull_back)
    glFrontFace(GL_CCW);
    g_draw_current = (SceneDrawState) {-1, -1, -1, -1, 0}; // cull_front 0: glCullFace(GL_BACK) above

    camera_update_projection_matrix(camera, (float) viewport[2] / (float) viewport[3]);
    camera_update_world_inverse_matrix(camera);

    float view_projection[16];
    float frustum[6][4];
    mat4x4(camera->projection_matrix, camera->world_inverse_matrix, view_projection);
    scene_frustum_planes(view_projection, frustum);

    size_t indices[scene->mesh_count];

    for (size_t i = 0; i < scene->mesh_count; i++) {
        Mesh *mesh = scene->meshes[i];
        mesh_update_transform_matrix(mesh);

        if (mesh->material->transparent) {
            mat4x4(camera->world_inverse_matrix, mesh->transform_matrix, mesh->_camera_space_matrix);
        }

        indices[i] = i;
    }

    sort_scene = scene;
    qsort(indices, scene->mesh_count, sizeof(size_t), (void *) scene_depth_sort);

    int last_transparent = -1; // GL_BLEND as the last visible mesh leaves it, culled or not

    for (size_t i = 0; i < scene->mesh_count; i++) {
        const Mesh *mesh = scene->meshes[indices[i]];

        if (!mesh->visible) continue;
        last_transparent = mesh->material->transparent ? 1 : 0;
        if (scene_mesh_culled(mesh, frustum)) continue;

        glBindVertexArray(mesh->geometry->vao);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh->geometry->ebo);

        const unsigned int program = mesh->material->program;
        if (g_active_shader != program) {
            glUseProgram(program);
            g_active_shader = program;
        }

        // per-draw defaults; BasicMaterial overrides them (FrontSide culling, additive
        // blending, depthWrite: false) inside material_update_uniforms()
        g_draw_pending = (SceneDrawState) {mesh->material->transparent ? 1 : 0, 0, 0, 1, 0};
        material_update_uniforms(mesh->material);
        scene_apply_draw_state();
        shadow_apply_mesh_uniforms(mesh); // sun shadow receiver uniforms (shadow.c)

        gl_uniform_mat4(&u_transform, program, GL_TRUE, mesh->transform_matrix);
        gl_uniform_mat4(&u_camera_world_inverse, program, GL_TRUE, camera->world_inverse_matrix);
        gl_uniform_mat4(&u_camera_projection, program, GL_TRUE, camera->projection_matrix);
        gl_uniform_3f(&u_camera_position, program, camera->position.x, camera->position.y, camera->position.z); // Phong view vector

#ifndef KN_GLES
        // OpenGL ES (WebGL2, ANGLE) has no glPolygonMode; wireframe materials render filled there.
        if (mesh->material->wireframe) {
            glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        } else {
            glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        }
#endif

        glDrawElements(GL_TRIANGLES, mesh->geometry->index_count, GL_UNSIGNED_INT, NULL);
    }

    if (last_transparent >= 0 && last_transparent != g_draw_current.blend) {
        if (last_transparent) glEnable(GL_BLEND);
        else glDisable(GL_BLEND);
    }

    glDisable(GL_CULL_FACE);
    glCullFace(GL_BACK); // map-G: a BackSide material may have left GL_FRONT
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LESS);
}

void scene_fini(Scene *scene) {
    free(scene);
}