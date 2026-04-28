#include <glad/glad.h>
#include <client.h>
#include <stdlib.h>
#include <string.h>

Scene *scene_init() {
    Scene *scene = calloc(1, sizeof(Scene));
    return scene;
}

void scene_add_mesh(Scene *scene, Mesh *mesh) {
    if (!scene->meshes) {
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

    Mesh **meshes = realloc(scene->meshes, scene->mesh_count * sizeof(Mesh *));
    if (!meshes) return;

    scene->meshes = meshes;
}

const Scene *sort_scene;
const Camera *sort_camera;

int scene_depth_sort(const size_t *p_a_idx, const size_t *p_b_idx) {
    const Mesh *a = sort_scene->meshes[*p_a_idx];
    const Mesh *b = sort_scene->meshes[*p_b_idx];

    if (!a->visible) return 1;
    if (!b->visible) return -1;

    if (a->material->transparent || b->material->transparent) {
        if (!b->material->transparent) return 1;
        if (!a->material->transparent) return -1;

        const float a_depth = sort_camera->world_inverse_matrix[8] * a->position.x + sort_camera->world_inverse_matrix[9] * a->position.y + sort_camera->world_inverse_matrix[10] * a->position.z + sort_camera->world_inverse_matrix[11];
        const float b_depth = sort_camera->world_inverse_matrix[8] * b->position.x + sort_camera->world_inverse_matrix[9] * b->position.y + sort_camera->world_inverse_matrix[10] * b->position.z + sort_camera->world_inverse_matrix[11];

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
    for (size_t i = 0; i < scene->mesh_count; i++) indices[i] = i;

    sort_scene = scene;
    sort_camera = camera;

    qsort(indices, scene->mesh_count, sizeof(size_t), (void *) scene_depth_sort);

    for (size_t i = 0; i < scene->mesh_count; i++) {
        Mesh *mesh = scene->meshes[indices[i]];
        if (!mesh->visible) continue;

        if (mesh->material->transparent) {
            glEnable(GL_BLEND);
        } else {
            glDisable(GL_BLEND);
        }

        mesh_update_transform_matrix(mesh);

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