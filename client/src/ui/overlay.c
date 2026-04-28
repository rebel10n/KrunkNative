#include <glad/glad.h>
#include <client.h>
#include <math.h>

void overlay_render(Client *client) {
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);

    ui_update_size(client->ui);

    if (client->watermark) { // alpha build watermark
        const char *watermark = "ALPHA BUILD @REBEL10N";
        const float width = ui_measure_text(client->ui, watermark, 30.0f);

        const size_t grid_h = (size_t) ceilf(client->ui->height / 100.0f) + 1;
        const size_t grid_w = (size_t) ceilf(client->ui->width / (width + 70.0f));

        for (size_t i = 0; i < grid_h * grid_w; i++) {
            const size_t x = i % grid_w;
            const size_t y = i / grid_w;

            ui_fill_text(client->ui, (vec4) {1.0f, 1.0f, 1.0f, 0.03f}, watermark, -10.0f + (width + 70.0f) * (float) x, -10.0f + 100.0f * (float) y, 30.0f);
        }
    }

    const Player *me = client->game.players[0];
    if (!me || !me->active) return;

    {
        // bottom left HUD (icon, health)
        const vec4 background_color = {0.0f, 0.0f, 0.0f, 0.4f};
        const vec2 hud_icon_size = {103.0f, 103.0f};
        const vec2 hud_icon = {20.0f, client->ui->height - 20.0f - hud_icon_size.y};

        ui_round_rect(client->ui, background_color, hud_icon.x, hud_icon.y, hud_icon_size.x, hud_icon_size.y, 10.0f);

        char *class_icon_path = concat(client_assets_path(), "textures/classes/icon_0.png");
        const unsigned int class_icon = load_texture(class_icon_path);

        free(class_icon_path);

        if (class_icon) ui_draw_image_rounded(client->ui, class_icon, hud_icon.x, hud_icon.y, hud_icon_size.x, hud_icon_size.y, 10.0f);

        const float health = 100.0f;
        const float max_health = 100.0f;

        const vec2 health_seg_size = {40.0f, 50.0f};
        const vec2 health_seg_origin = {hud_icon.x + hud_icon_size.x + 10.0f, hud_icon.y + hud_icon_size.y - health_seg_size.y};

        const size_t segments = 5;

        for (size_t i = 0; i < segments; i++) {
            const vec2 health_seg = {health_seg_origin.x + (10.0f + health_seg_size.x) * (float) i, health_seg_origin.y};

            ui_round_rect(client->ui, background_color, health_seg.x, health_seg.y, health_seg_size.x, health_seg_size.y, 5.0f);

            const float health_per_segment = max_health / (float) segments;
            const float health_width = CLAMP((health - health_per_segment * i) / health_per_segment, 0.0f, 1.0f);

            const vec4 full_health_color = {0.67f, 0.85f, 0.36f, 1.0f};

            ui_round_rect(client->ui, full_health_color, health_seg.x, health_seg.y, health_seg_size.x * health_width, health_seg_size.y, 5.0f);
            ui_round_rect(client->ui, (vec4) {0.0f, 0.0f, 0.0f, 0.28f}, health_seg.x, health_seg.y, health_seg_size.x * health_width, health_seg_size.y, 5.0f);
            ui_round_rect(client->ui, full_health_color, health_seg.x, health_seg.y, health_seg_size.x * health_width, health_seg_size.y - 10.0f, 5.0f);
        }
    }

    { // test crosshair
        const vec4 color = {1.0f, 1.0f, 0.0f, 1.0f};

        ui_fill_rect(client->ui, color, client->ui->width * 0.5f - 1.0f, client->ui->height * 0.5f - 5.0f, 2.0f, 10.0f);
        ui_fill_rect(client->ui, color, client->ui->width * 0.5f - 5.0f, client->ui->height * 0.5f - 1.0f, 10.0f, 2.0f);
    }
}
