#include <client.h>
#include <math.h>

unsigned int logo;

void hud_render(Client *client, const float now) {
    float progress = fmodf(now, 1.6f) / 0.8f;
    if (progress > 1) progress = 2 - progress;

    ui_fill_rect(client->ui, (vec4) {0.0f, 0.0f, 0.0f, 0.5f}, 0.0f, 0.0f, client->ui->width, client->ui->height);

    const char *instructions = "CLICK TO PLAY";
    const float text_size = 32.0f - 2.0f * progress;
    const float opacity = 0.8f * (1.0f - 0.7f * progress);
    const float text_width = ui_measure_text(client->ui, instructions, text_size);

    ui_fill_text(client->ui, (vec4) {1.0f, 1.0f, 1.0f, opacity}, instructions, (client->ui->width - text_width) * 0.5f, client->ui->height * 0.5f, text_size);

    const vec2 logo_size = {763.0f, 225.0f};

    if (!logo) {
        char *logo_path = concat(client_assets_path(), "img/logo_4.png");

        logo = load_texture(logo_path);
        free(logo_path);
    }

    if (logo) ui_draw_image(client->ui, logo, (client->ui->width - logo_size.x) * 0.5f, 15.0f, logo_size.x, logo_size.y);
}