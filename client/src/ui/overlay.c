#include <glad/glad.h>
#include <client.h>
#include <math.h>

void overlay_render(Client *client) {
    { // test crosshair
        const vec4 color = {1.0f, 1.0f, 0.0f, 1.0f};

        ui_fill_rect(client->ui, color, client->ui->width * 0.5f - 1.0f, client->ui->height * 0.5f - 5.0f, 2.0f, 10.0f);
        ui_fill_rect(client->ui, color, client->ui->width * 0.5f - 5.0f, client->ui->height * 0.5f - 1.0f, 10.0f, 2.0f);
    }
}
