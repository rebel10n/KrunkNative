#include <client.h>
#include <math.h>

void hud_render(Client *client, const float now) {
    float progress = fmodf(now, 1.6f) / 0.8f;
    if (progress > 1) progress = 2 - progress;

    ui_fill_rect(client->ui, (vec4) {0.0f, 0.0f, 0.0f, 0.5f}, 0.0f, 0.0f, client->ui->width, client->ui->height);

    const char *instructions = "CLICK TO PLAY";
    const float text_size = 32.0f - 2.0f * progress;
    const float opacity = 0.8f * (1.0f - 0.7f * progress);
    const float text_width = ui_measure_text(client->ui, instructions, text_size);

    ui_fill_text(client->ui, (vec4) {1.0f, 1.0f, 1.0f, opacity}, instructions, (client->ui->width - text_width) * 0.5f, client->ui->height * 0.5f, text_size);
}