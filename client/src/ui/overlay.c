#include <glad/glad.h>
#include <client.h>
#include <math.h>

void overlay_render(Client *client, const float delta) {
    { // bottom left HUD
        const ClassConfig *class = &client->game.classes[client->me->class_index];
        const vec4 background_color = {0.0f, 0.0f, 0.0f, 0.4f};

        const vec2 anchor = {20.0f * client->ui->scale, client->ui->height - 30.0f * client->ui->scale};
        const vec2 class_icon_pos = {anchor.x, anchor.y - 103.0f * client->ui->scale};

        ui_round_rect(client->ui, background_color, class_icon_pos.x, class_icon_pos.y, 103.0f * client->ui->scale, 103.0f * client->ui->scale, 10.0f);

        const vec4 segment_color = hex_to_vec(0x9eeb56);

        for (int i = 0; i < class->health_segments; i++) {
            ui_round_rect(
                client->ui, background_color,
                anchor.x + (113.0f + 50.0f * (float) i) * client->ui->scale,
                anchor.y - 50.0f * client->ui->scale,
                40.0f * client->ui->scale,
                50.0f * client->ui->scale,
                5.0f
            );

            const float health_per_segment = (float) client->me->max_health / (float) class->health_segments;
            const float progress = CLAMP((client->me->health - (float) i * health_per_segment) / health_per_segment, 0.0f, 1.0f);

            for (int j = 0; j < 2; j++) {
                ui_round_rect(
                    client->ui, segment_color,
                    anchor.x + (113.0f + 50.0f * (float) i) * client->ui->scale,
                    anchor.y - 50.0f * client->ui->scale,
                    40.0f * client->ui->scale * progress,
                    (50.0f - 10.0f * (float) j) * client->ui->scale,
                    5.0f
                );

                ui_round_rect(
                    client->ui, background_color,
                    anchor.x + (113.0f + 50.0f * (float) i) * client->ui->scale,
                    anchor.y - 50.0f * client->ui->scale,
                    40.0f * client->ui->scale * progress,
                    50.0f * client->ui->scale * (1.0f - (float) j),
                    5.0f
                );
            }
        }
    }

    { // test crosshair
        const vec4 color = {1.0f, 1.0f, 0.0f, 1.0f};

        ui_fill_rect(client->ui, color, client->ui->width * 0.5f - 1.0f * client->ui->scale, client->ui->height * 0.5f - 5.0f * client->ui->scale, 2.0f * client->ui->scale, 10.0f * client->ui->scale);
        ui_fill_rect(client->ui, color, client->ui->width * 0.5f - 5.0f * client->ui->scale, client->ui->height * 0.5f - 1.0f * client->ui->scale, 10.0f * client->ui->scale, 2.0f * client->ui->scale);
    }

    { // debug (bottom right HUD)
        const vec4 color = {0.0f, 0.0f, 0.0f, 1.0f};
        const vec2 anchor = {client->ui->width - 20.0f * client->ui->scale, client->ui->height - 30.0f * client->ui->scale};

        const float weapon_text_length = ui_measure_text(client->ui, client->me->weapon->name, 16.0f * client->ui->scale);
        ui_fill_text(client->ui, color, client->me->weapon->name, anchor.x - weapon_text_length, anchor.y - 16.0f * client->ui->scale, 16.0f * client->ui->scale);
    }
}
