#include <glad/glad.h>
#include <client.h>
#include <math.h>

void overlay_render(Client *client) {
    { // test crosshair
        const vec4 color = {1.0f, 1.0f, 0.0f, 1.0f};

        ui_fill_rect(client->ui, color, client->ui->width * 0.5f - 1.0f, client->ui->height * 0.5f - 5.0f, 2.0f, 10.0f);
        ui_fill_rect(client->ui, color, client->ui->width * 0.5f - 5.0f, client->ui->height * 0.5f - 1.0f, 10.0f, 2.0f);
    }

    { // debug metrics
        const vec4 color = {1.0f, 1.0f, 1.0f, 0.5f};

        ui_fill_text(client->ui, color, "DEBUG METRICS", 10.0f, 20.0f, 10.0f);

        const float speed = hypotf(client->me->velocity.x, client->me->velocity.z) * 1000.0f;
        const size_t speed_length = snprintf(NULL, 0, "speed = %.f", speed);
        char *speed_str = malloc(speed_length + 1);

        if (speed_str) {
            snprintf(speed_str, speed_length + 1, "speed = %.f", speed);
            ui_fill_text(client->ui, color, speed_str, 10.0f, 35.0f, 10.0f);
            
            free(speed_str);
        }
    }
}
