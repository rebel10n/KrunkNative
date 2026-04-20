#include <glad/glad.h>
#include <client.h>
#include <stdio.h>
#include <stdlib.h>

void ui_init(UI *ui) {
    ui->material = flat_material_init();

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
}

void ui_draw_color(UI *ui, const vec4 color) {
    ui->material->texture = 0;
    ui->material->color = color;

    glUseProgram(ui->material->base.program);
    material_update_uniforms((Material *) ui->material);
}

void ui_draw_texture(UI *ui, const unsigned int texture, const vec4 color) {
    ui->material->texture = texture;
    ui->material->color = color;

    glUseProgram(ui->material->base.program);
    material_update_uniforms((Material *) ui->material);
}

void ui_fill_rect(UI *ui, const int x, const int y, const int width, const int height) {
    int viewport[4];
    glGetIntegerv(GL_VIEWPORT, viewport);

    const float min_x = (float) x / (float) viewport[2] * 2.0f - 1.0f;
    const float max_x = (float) (x + width) / (float) viewport[2] * 2.0f - 1.0f;
    const float min_y = 1.0f - (float) y / (float) viewport[3] * 2.0f;
    const float max_y = 1.0f - (float) (y + height) / (float) viewport[3] * 2.0f;

    const vertex vertices[] = {
        {min_x, max_y, 0.0f, 0.0f, 0.0f},
        {max_x, min_y, 0.0f, 1.0f, 1.0f},
        {min_x, min_y, 0.0f, 0.0f, 1.0f},
        {max_x, max_y, 0.0f, 1.0f, 0.0f},
    };

    const unsigned int indices[] = {0, 1, 2, 0, 3, 1};

    glBindVertexArray(ui->vao);

    glBindBuffer(GL_ARRAY_BUFFER, ui->vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(vertices), vertices);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ui->ebo);
    glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, sizeof(indices), indices);

    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, NULL);
}

void ui_render(UI *ui) {
    int viewport[4];
    glGetIntegerv(GL_VIEWPORT, viewport);

    ui->vtable->render(ui, viewport[2], viewport[3]);
}

void ui_fini(UI *ui) {
    free(ui);
}