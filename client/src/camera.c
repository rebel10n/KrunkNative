#include <client.h>
#include <math.h>
#include <string.h>

void camera_update_projection_matrix(Camera *camera, const float aspect) {
    const float half_height = tanf(camera->fov / 2.0f) * camera->near;
    const float half_width = half_height * aspect;

    const float A = -(camera->far + camera->near) / (camera->far - camera->near);
    const float B = camera->near * (A - 1);

    const float matrix[] = {
        camera->near / half_width, 0.0f, 0.0f, 0.0f,
        0.0f, camera->near / half_height, 0.0f, 0.0f,
        0.0f, 0.0f, A, B,
        0.0f, 0.0f, -1.0f, 0.0f,
    };

    memcpy(camera->projection_matrix, matrix, sizeof(matrix));
}

void camera_update_world_inverse_matrix(Camera *camera) {
    const float rotate_x_matrix[] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, cosf(-camera->rotation.x), -sinf(-camera->rotation.x), 0.0f,
        0.0f, sinf(-camera->rotation.x), cosf(-camera->rotation.x), 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f,
    };

    const float rotate_y_matrix[] = {
        cosf(-camera->rotation.y), 0.0f, sinf(-camera->rotation.y), 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        -sinf(-camera->rotation.y), 0.0f, cosf(-camera->rotation.y), 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f,
    };

    const float rotate_z_matrix[] = {
        cosf(-camera->rotation.z), -sinf(-camera->rotation.z), 0.0f, 0.0f,
        sinf(-camera->rotation.z), cosf(-camera->rotation.z), 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f,
    };

    const float translate_matrix[] = {
        1.0f, 0.0f, 0.0f, -camera->position.x,
        0.0f, 1.0f, 0.0f, -camera->position.y,
        0.0f, 0.0f, 1.0f, -camera->position.z,
        0.0f, 0.0f, 0.0f, 1.0f,
    };

    float tmp[16];
    mat4x4(rotate_z_matrix, translate_matrix, tmp);

    float tmp1[16];
    mat4x4(rotate_y_matrix, tmp, tmp1);
    mat4x4(rotate_x_matrix, tmp1, camera->world_inverse_matrix);
}