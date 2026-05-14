#include <glad/glad.h>
#include <client.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

Scene *scene_init() {
    Scene *scene = calloc(1, sizeof(Scene));
    return scene;
}

void scene_add_player_mesh(Scene *scene, const PlayerMesh *player_mesh, const size_t loadout_size) {
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

            for (int ii = 0; ii < 2; ii++) {
                const PlayerArmMesh *arm = ii ? arms->left : arms->right;

                if (arm->extender) scene_add_mesh(scene, arm->extender);

                if (arm->upper) scene_add_mesh(scene, arm->upper);
                if (arm->joint) scene_add_mesh(scene, arm->joint);

                for (size_t iii = 0; iii < arm->lower->mesh_count; iii++) {
                    scene_add_mesh(scene, arm->lower->meshes[iii]);
                }
            }

            break;
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

            for (int ii = 0; ii < 2; ii++) {
                const PlayerArmMesh *arm = ii ? arms->left : arms->right;

                if (arm->extender) scene_remove_mesh(scene, arm->extender);

                if (arm->upper) scene_remove_mesh(scene, arm->upper);
                if (arm->joint) scene_remove_mesh(scene, arm->joint);

                for (size_t iii = 0; iii < arm->lower->mesh_count; iii++) {
                    scene_remove_mesh(scene, arm->lower->meshes[iii]);
                }
            }

            break;
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
    if (!scene->mesh_count) {
        scene->meshes = calloc(1, sizeof(Mesh *));
        if (!scene->meshes) return;

        scene->mesh_count = 1;
        scene->meshes[0] = mesh;

        return;
    }

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

void scene_render(const Scene *scene, Camera *camera) {
    int viewport[4];
    glGetIntegerv(GL_VIEWPORT, viewport);

    glEnable(GL_DEPTH_TEST);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    camera_update_projection_matrix(camera, (float) viewport[2] / (float) viewport[3]);
    camera_update_world_inverse_matrix(camera);

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

    for (size_t i = 0; i < scene->mesh_count; i++) {
        const Mesh *mesh = scene->meshes[indices[i]];

        if (!mesh->visible) continue;

        if (mesh->material->transparent) {
            glEnable(GL_BLEND);
        } else {
            glDisable(GL_BLEND);
        }

        glBindVertexArray(mesh->geometry->vao);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh->geometry->ebo);

        if (g_active_shader != mesh->material->program) {
            glUseProgram(mesh->material->program);
            g_active_shader = mesh->material->program;
        }

        material_update_uniforms(mesh->material);

        const int transform = glGetUniformLocation(mesh->material->program, "transform");
        const int camera_world_inverse = glGetUniformLocation(mesh->material->program, "camera_world_inverse");
        const int camera_projection = glGetUniformLocation(mesh->material->program, "camera_projection");

        if (transform > -1) glUniformMatrix4fv(transform, 1, GL_TRUE, mesh->transform_matrix);
        if (camera_world_inverse > -1) glUniformMatrix4fv(camera_world_inverse, 1, GL_TRUE, camera->world_inverse_matrix);
        if (camera_projection > -1) glUniformMatrix4fv(camera_projection, 1, GL_TRUE, camera->projection_matrix);

        if (mesh->material->wireframe) {
            glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        } else {
            glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        }

        glDrawElements(GL_TRIANGLES, mesh->geometry->index_count, GL_UNSIGNED_INT, NULL);
    }
}

void scene_fini(Scene *scene) {
    free(scene);
}