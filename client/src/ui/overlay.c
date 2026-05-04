#include <glad/glad.h>
#include <client.h>
#include <math.h>

void overlay_render(Client *client, const float delta) {
    { // test crosshair
        const vec4 color = {1.0f, 1.0f, 0.0f, 1.0f};

        ui_fill_rect(client->ui, color, client->ui->width * 0.5f - 1.0f, client->ui->height * 0.5f - 5.0f, 2.0f, 10.0f);
        ui_fill_rect(client->ui, color, client->ui->width * 0.5f - 5.0f, client->ui->height * 0.5f - 1.0f, 10.0f, 2.0f);
    }

    { // debug metrics
        const vec4 color = {1.0f, 1.0f, 1.0f, 0.5f};

        ui_fill_text(client->ui, color, "DEBUG METRICS", 10.0f, 20.0f, 10.0f);

        float metrics_offset = 38.0f;

        const size_t frame_rate_length = snprintf(NULL, 0, "frame rate = %.f", 1.0f / delta);
        char *frame_rate_str = malloc(frame_rate_length + 1);

        if (frame_rate_str) {
            snprintf(frame_rate_str, frame_rate_length + 1, "frame rate = %.f", 1.0f / delta);
            ui_fill_text(client->ui, color, frame_rate_str, 10.0f, metrics_offset, 10.0f);

            metrics_offset += 15.0f;
            free(frame_rate_str);
        }

        const size_t frame_time_length = snprintf(NULL, 0, "last frame time = %.2f ms", delta * 1000.0f);
        char *frame_time_str = malloc(frame_time_length + 1);

        if (frame_time_str) {
            snprintf(frame_time_str, frame_time_length + 1, "last frame time = %.2f ms", delta * 1000.0f);
            ui_fill_text(client->ui, color, frame_time_str, 10.0f, metrics_offset, 10.0f);

            metrics_offset += 15.0f;
            free(frame_time_str);
        }

        const float speed = hypotf(client->me->velocity.x, client->me->velocity.z) * 1000.0f;
        const size_t speed_length = snprintf(NULL, 0, "player speed = %.f", speed);
        char *speed_str = malloc(speed_length + 1);

        if (speed_str) {
            snprintf(speed_str, speed_length + 1, "player speed = %.f", speed);
            ui_fill_text(client->ui, color, speed_str, 10.0f, metrics_offset, 10.0f);

            metrics_offset += 15.0f;
            free(speed_str);
        }

        const size_t weapon_length = snprintf(NULL, 0, "player weapon = %s", client->me->weapon->name);
        char *weapon_str = malloc(weapon_length + 1);

        if (weapon_str) {
            snprintf(weapon_str, weapon_length + 1, "player weapon = %s", client->me->weapon->name);
            ui_fill_text(client->ui, color, weapon_str, 10.0f, metrics_offset, 10.0f);

            free(weapon_str);
        }
    }
}
