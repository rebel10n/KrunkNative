#include <glad/glad.h>
#include <client.h>
#include <stdio.h>
#include <stdlib.h>

UI *ui_init() {
    UI *ui = calloc(1, sizeof(UI));
    if (!ui) return NULL;

    ui->material = quad_material_init();

    glGenBuffers(1, &ui->vbo);
    glGenBuffers(1, &ui->ebo);

    glGenVertexArrays(1, &ui->vao);
    glBindVertexArray(ui->vao);

    glBindBuffer(GL_ARRAY_BUFFER, ui->vbo);
    glBufferData(GL_ARRAY_BUFFER, 100 * sizeof(vertex), NULL, GL_DYNAMIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ui->ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, 200 * sizeof(unsigned int), NULL, GL_DYNAMIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), NULL); // position
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *) (3 * sizeof(float))); // tex_coord

    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexArrayElementBuffer(ui->vao, ui->ebo);

    return ui;
}

void ui_update_size(UI *ui) {
    int viewport[4];
    glGetIntegerv(GL_VIEWPORT, viewport);

    ui->width = (float) viewport[2];
    ui->height = (float) viewport[3];
}

void ui_fill_rect_(UI *ui, const float x, const float y, const float width, const float height) {
    const float min_x = x / ui->width * 2.0f - 1.0f;
    const float max_x = (x + width) / ui->width * 2.0f - 1.0f;
    const float min_y = 1.0f - y / ui->height * 2.0f;
    const float max_y = 1.0f - (y + height) / ui->height * 2.0f;

    const vertex vertices[] = {
        {min_x, max_y, 0.0f, 0.0f, 0.0f},
        {max_x, max_y, 0.0f, 1.0f, 0.0f},
        {max_x, min_y, 0.0f, 1.0f, 1.0f},
        {min_x, min_y, 0.0f, 0.0f, 1.0f},
    };

    glBindVertexArray(ui->vao);

    glBindBuffer(GL_ARRAY_BUFFER, ui->vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(vertices), vertices);

    glDrawArrays(GL_QUADS, 0, 4);
}

void ui_fill_rect(UI *ui, const vec4 color, const float x, const float y, const float width, const float height) {
    ui->material->color = color;
    ui->material->texture = 0;

    ui->material->texture_viewport[0] = 0.0f;
    ui->material->texture_viewport[1] = 0.0f;
    ui->material->texture_viewport[2] = 1.0f;
    ui->material->texture_viewport[3] = 1.0f;

    ui->material->border_bottom_left_radius = 0.0f;
    ui->material->border_bottom_right_radius = 0.0f;
    ui->material->border_top_left_radius = 0.0f;
    ui->material->border_top_right_radius = 0.0f;

    glUseProgram(ui->material->base.program);
    material_update_uniforms((Material *) ui->material);

    ui_fill_rect_(ui, x, y, width, height);
}

void ui_round_rect(UI *ui, const vec4 color, const float x, const float y, const float width, const float height, const float radius) {
    ui->material->color = color;
    ui->material->texture = 0;

    ui->material->texture_viewport[0] = 0.0f;
    ui->material->texture_viewport[1] = 0.0f;
    ui->material->texture_viewport[2] = 1.0f;
    ui->material->texture_viewport[3] = 1.0f;

    const float radius_mlt = radius / (width > height ? height : width);

    ui->material->aspect = width / height;
    ui->material->border_bottom_left_radius = radius_mlt;
    ui->material->border_bottom_right_radius = radius_mlt;
    ui->material->border_top_left_radius = radius_mlt;
    ui->material->border_top_right_radius = radius_mlt;

    glUseProgram(ui->material->base.program);
    material_update_uniforms((Material *) ui->material);

    ui_fill_rect_(ui, x, y, width, height);
}

void ui_draw_image(UI *ui, const unsigned int texture_id, const float x, const float y, const float width, const float height) {
    ui->material->color = (vec4) {1.0f, 1.0f, 1.0f, 1.0f};
    ui->material->texture = texture_id;

    ui->material->texture_viewport[0] = 0.0f;
    ui->material->texture_viewport[1] = 0.0f;
    ui->material->texture_viewport[2] = 1.0f;
    ui->material->texture_viewport[3] = 1.0f;

    ui->material->border_bottom_left_radius = 0.0f;
    ui->material->border_bottom_right_radius = 0.0f;
    ui->material->border_top_left_radius = 0.0f;
    ui->material->border_top_right_radius = 0.0f;

    glUseProgram(ui->material->base.program);
    material_update_uniforms((Material *) ui->material);

    ui_fill_rect_(ui, x, y, width, height);
}

void ui_draw_image_rounded(UI *ui, const unsigned int texture_id, const float x, const float y, const float width, const float height, const float radius) {
    ui->material->color = (vec4) {1.0f, 1.0f, 1.0f, 1.0f};
    ui->material->texture = texture_id;

    ui->material->texture_viewport[0] = 0.0f;
    ui->material->texture_viewport[1] = 0.0f;
    ui->material->texture_viewport[2] = 1.0f;
    ui->material->texture_viewport[3] = 1.0f;

    const float radius_mlt = radius / (width > height ? height : width);

    ui->material->aspect = width / height;
    ui->material->border_bottom_left_radius = radius_mlt;
    ui->material->border_bottom_right_radius = radius_mlt;
    ui->material->border_top_left_radius = radius_mlt;
    ui->material->border_top_right_radius = radius_mlt;

    glUseProgram(ui->material->base.program);
    material_update_uniforms((Material *) ui->material);

    ui_fill_rect_(ui, x, y, width, height);
}

void ui_fini(UI *ui) {
    free(ui);
}