#include <glad/glad.h>
#include <client.h>
#include <stdio.h>
#include <stdlib.h>

UI *ui_init() {
    UI *ui = calloc(1, sizeof(UI));
    if (!ui) return NULL;

    ui->material = quad_material_init();
    ui->text_material = text_material_init();

    glGenBuffers(1, &ui->vbo);

    glGenVertexArrays(1, &ui->vao);
    glBindVertexArray(ui->vao);

    const vertex vertices[] = {
        {-1.0f, -1.0f, 0.0f, 0.0f, 0.0f},
        {1.0f, -1.0f, 0.0f, 1.0f, 0.0f},
        {1.0f, 1.0f, 0.0f, 1.0f, 1.0f},
        {-1.0f, 1.0f, 0.0f, 0.0f, 1.0f},
    };

    glBindBuffer(GL_ARRAY_BUFFER, ui->vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), NULL); // position
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *) (3 * sizeof(float))); // tex_coord

    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);

    return ui;
}

void ui_update_size(UI *ui) {
    int viewport[4];
    glGetIntegerv(GL_VIEWPORT, viewport);

    ui->width = (float) viewport[2];
    ui->height = (float) viewport[3];
}

void ui_fill_rect_(UI *ui, const unsigned int shader, const float x, const float y, const float width, const float height) {
    const float offset_x = (x + width * 0.5f) / ui->width * 2.0f - 1.0f;
    const float offset_y = 1.0f - (y + height * 0.5f) / ui->height * 2.0f;

    const float scale_x = width / ui->width;
    const float scale_y = height / ui->height;

    const int offset = glGetUniformLocation(shader, "offset");
    const int scale = glGetUniformLocation(shader, "scale");

    glBindVertexArray(ui->vao);

    glUniform2f(offset, offset_x, offset_y);
    glUniform2f(scale, scale_x, scale_y);

    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
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

    ui_fill_rect_(ui, ui->material->base.program, x, y, width, height);
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

    ui_fill_rect_(ui, ui->material->base.program, x, y, width, height);
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

    ui_fill_rect_(ui, ui->material->base.program, x, y, width, height);
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

    ui_fill_rect_(ui, ui->material->base.program, x, y, width, height);
}

float ui_measure_text(UI *ui, const char *text, const float size) {
    float width = 0;

    while (*text != 0) {
        GlyphCacheEntry *glyph = load_glyph(*text);
        text++;

        if (!glyph) continue;

        width += glyph->advance * size / 100.0f;
    }

    return width;
}

float ui_fill_text(UI *ui, const vec4 color, const char *text, float x, const float y, const float size) {
    const float start_x = x;

    ui->text_material->color = color;
    glUseProgram(ui->text_material->base.program);
    glBindVertexArray(ui->vao);

    while (*text != 0) {
        const char c = *text;
        const GlyphCacheEntry *glyph = load_glyph(c);

        text++;

        if (!glyph) continue;

        if (c != ' ') {
            ui->text_material->texture = glyph->texture;
            material_update_uniforms((Material *) ui->text_material);

            float glyph_y = y + (100.0f - glyph->h_bearing.y) * size / 100.0f;

            if (ui->text_baseline == TEXT_BASELINE_MIDDLE) {
                glyph_y += glyph->h_bearing.y * size / 100.0f * 0.5f;
            } else if (ui->text_baseline == TEXT_BASELINE_TOP) {
                glyph_y += glyph->h_bearing.y * size / 100.0f;
            }

            ui_fill_rect_(ui, ui->text_material->base.program, x - glyph->h_bearing.x, glyph_y, glyph->size.x * size / 100.0f, glyph->size.y * size / 100.0f);
        }

        x += glyph->advance * size / 100.0f;
    }

    return x - start_x;
}

void ui_fini(UI *ui) {
    free(ui);
}