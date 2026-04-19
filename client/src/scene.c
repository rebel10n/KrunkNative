#include <glad/glad.h>
#include <client.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

Scene *scene_init() {
    Scene *scene = calloc(1, sizeof(Scene));

    scene->camera.near = 0.1f;
    scene->camera.far = 1000.0f;
    scene->camera.fov = M_PI / 2.0f;

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

void scene_render(Scene *scene) {
    int viewport[4];
    glGetIntegerv(GL_VIEWPORT, viewport);

    camera_update_projection_matrix(&scene->camera, (float) viewport[2] / (float) viewport[3]);
    camera_update_world_inverse_matrix(&scene->camera);

    for (size_t i = 0; i < scene->mesh_count; i++) {
        Mesh *mesh = scene->meshes[i];
        mesh_update_transform_matrix(mesh);

        glBindVertexArray(mesh->vao);

        glBindBuffer(GL_ARRAY_BUFFER, mesh->vbo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh->ebo);

        glUseProgram(mesh->material->program);
        material_update_uniforms(mesh->material);

        const int transform = glGetUniformLocation(mesh->material->program, "transform");
        const int camera_world_inverse = glGetUniformLocation(mesh->material->program, "camera_world_inverse");
        const int camera_projection = glGetUniformLocation(mesh->material->program, "camera_projection");

        if (transform > -1) glUniformMatrix4fv(transform, 1, GL_TRUE, mesh->transform_matrix);
        if (camera_world_inverse > -1) glUniformMatrix4fv(camera_world_inverse, 1, GL_TRUE, scene->camera.world_inverse_matrix);
        if (camera_projection > -1) glUniformMatrix4fv(camera_projection, 1, GL_TRUE, scene->camera.projection_matrix);

        if (mesh->material->wireframe) {
            glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        } else {
            glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        }

        if (mesh->ebo && mesh->index_count) {
            glDrawElements(GL_TRIANGLES, mesh->index_count, GL_UNSIGNED_INT, NULL);
        } else {
            glDrawArrays(GL_TRIANGLES, 0, mesh->vertex_count);
        }
    }
}

void scene_fini(Scene *scene) {
    free(scene);
}