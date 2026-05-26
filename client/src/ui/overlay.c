#include <glad/glad.h>
#include <client.h>

static unsigned int ammo_icon;
static unsigned int timer_icon;

void overlay_render(Client *client, const float delta) {
    if (!client->me) return;

    const vec4 white = {1.0f, 1.0f, 1.0f, 1.0f};
    const vec4 background_color = {0.0f, 0.0f, 0.0f, 0.4f};

    { // xp bar
        const vec2 anchor = {20.0f * client->ui->scale, client->ui->height - 22.0f * client->ui->scale};

        ui_round_rect(client->ui, background_color, anchor.x, anchor.y, client->ui->width - 35.0f * client->ui->scale, 12.0f * client->ui->scale, 4.0f * client->ui->scale);
    }

    { // bottom left HUD
        const ClassConfig *class = &client->game.classes[client->me->class_index];
        const vec4 max_health_color = {1.0f, 1.0f, 1.0f, 0.8f};

        const vec2 anchor = {20.0f * client->ui->scale, client->ui->height - 35.0f * client->ui->scale};
        const vec2 class_icon_pos = {anchor.x, anchor.y - 103.0f * client->ui->scale};

        ui_round_rect(client->ui, background_color, class_icon_pos.x, class_icon_pos.y, 103.0f * client->ui->scale, 103.0f * client->ui->scale, 10.0f * client->ui->scale);

        const size_t class_icon_length = snprintf(NULL, 0, "textures/classes/icon_%d.png", class->icon_index);
        char *class_icon_path = malloc(class_icon_length + 1);

        if (class_icon_path) {
            snprintf(class_icon_path, class_icon_length + 1, "textures/classes/icon_%d.png", class->icon_index);

            char *full_class_icon_path = concat(assets_path(), class_icon_path);
            const unsigned int class_icon = load_texture(full_class_icon_path);

            if (class_icon) ui_draw_image_rounded(client->ui, class_icon, class_icon_pos.x, class_icon_pos.y, 103.0f * client->ui->scale, 103.0f * client->ui->scale, 10.0f * client->ui->scale);

            free(class_icon_path);
            free(full_class_icon_path);
        }

        const vec4 segment_color = hex_to_vec(0x9eeb56);

        for (int i = 0; i < class->health_segments; i++) {
            ui_round_rect(
                client->ui, background_color,
                anchor.x + (113.0f + 50.0f * (float) i) * client->ui->scale,
                anchor.y - 50.0f * client->ui->scale,
                40.0f * client->ui->scale,
                50.0f * client->ui->scale,
                5.0f * client->ui->scale
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
                    5.0f * client->ui->scale
                );

                ui_round_rect(
                    client->ui, background_color,
                    anchor.x + (113.0f + 50.0f * (float) i) * client->ui->scale,
                    anchor.y - 50.0f * client->ui->scale,
                    40.0f * client->ui->scale * progress,
                    50.0f * client->ui->scale * (1.0f - (float) j),
                    5.0f * client->ui->scale
                );
            }
        }

        const size_t health_length = snprintf(NULL, 0, "%.f", client->me->health);
        char *health_str = malloc(health_length + 1);

        if (health_str) snprintf(health_str, health_length + 1, "%.f", client->me->health);

        const size_t max_health_length = snprintf(NULL, 0, "| %d", class->health);
        char *max_health_str = malloc(max_health_length + 1);

        if (max_health_str) snprintf(max_health_str, max_health_length + 1, "| %d", class->health);

        const float health_size = 20.0f * client->ui->scale;
        const float health_str_width = ui_measure_text(client->ui, health_str, health_size);
        const float space_width = ui_measure_text(client->ui, " ", health_size);

        const float text_width = health_str_width + space_width + ui_measure_text(client->ui, max_health_str, health_size);

        ui_round_rect(client->ui, background_color, anchor.x + 113.0f * client->ui->scale, anchor.y - (59.0f + 41.0f) * client->ui->scale, text_width + 54.0f * client->ui->scale, 41.0f * client->ui->scale, 5.0f * client->ui->scale);
        ui_fill_text(client->ui, white, health_str, anchor.x + 125.0f * client->ui->scale, anchor.y - (59.0f + 8.0f) * client->ui->scale, health_size);
        ui_fill_text(client->ui, white, " ", anchor.x + 125.0f * client->ui->scale + health_str_width, anchor.y - (59.0f + 8.0f) * client->ui->scale, health_size);
        ui_fill_text(client->ui, max_health_color, max_health_str, anchor.x + 125.0f * client->ui->scale + health_str_width + space_width, anchor.y - (59.0f + 8.0f) * client->ui->scale, health_size);

        char *icon_path = concat(assets_path(), 0 ? "img/skull_0.png" : "img/hp_0.png"); // TODO: challenge mode
        const unsigned int icon = load_texture(icon_path);

        if (icon) ui_draw_image(client->ui, icon, anchor.x + 130.0f * client->ui->scale + text_width, anchor.y - (59.0f + 41.0f) * client->ui->scale + (41.0f - 28.0f) * 0.5f * client->ui->scale, 28.0f * client->ui->scale, 28.0f * client->ui->scale);

        free(health_str);
        free(max_health_str);
    }

    { // test crosshair
        const vec4 color = {1.0f, 1.0f, 0.0f, 1.0f};

        ui_fill_rect(client->ui, color, client->ui->width * 0.5f - 1.0f * client->ui->scale, client->ui->height * 0.5f - 5.0f * client->ui->scale, 2.0f * client->ui->scale, 10.0f * client->ui->scale);
        ui_fill_rect(client->ui, color, client->ui->width * 0.5f - 5.0f * client->ui->scale, client->ui->height * 0.5f - 1.0f * client->ui->scale, 10.0f * client->ui->scale, 2.0f * client->ui->scale);
    }

    { // bottom right HUD
        const vec4 max_ammo_color = {1.0f, 1.0f, 1.0f, 0.7f};
        const vec2 anchor = {client->ui->width - 20.0f * client->ui->scale, client->ui->height - 35.0f * client->ui->scale};

        if (!ammo_icon) {
            char *icon_path = concat(assets_path(), "textures/ammo_0.png");

            ammo_icon = load_texture(icon_path);
            free(icon_path);
        }

        if (ammo_icon) {
            const int ammo = client->me->ammo[client->me->loadout_index];
            char *ammo_str;

            if (client->me->ammo[client->me->loadout_index]) {
                const size_t ammo_str_length = snprintf(NULL, 0, "%d", ammo);
                ammo_str = malloc(ammo_str_length + 1);

                if (ammo_str) snprintf(ammo_str, ammo_str_length + 1, "%d", ammo);
            } else if (client->me->weapon->melee) {
                ammo_str = malloc(2);

                if (ammo_str) {
                    ammo_str[0] = '-';
                    ammo_str[1] = 0;
                }
            } else {
                ammo_str = malloc(2);

                if (ammo_str) {
                    ammo_str[0] = '0';
                    ammo_str[1] = 0;
                }
            }

            char *max_ammo_str;

            if (client->game.mode->config.ammo_limit || !client->me->weapon->ammo) {
                max_ammo_str = malloc(4);

                if (max_ammo_str) {
                    const char *max_ammo = "| -";
                    memcpy(max_ammo_str, max_ammo, 4);
                }
            } else {
                const size_t max_ammo_length = snprintf(NULL, 0, "| %d", client->me->weapon->ammo);
                max_ammo_str = malloc(max_ammo_length + 1);

                if (max_ammo_str) snprintf(max_ammo_str, max_ammo_length + 1, "| %d", client->me->weapon->ammo);
            }

            const float ammo_size = 35.0f * client->ui->scale;
            const float ammo_str_width = ui_measure_text(client->ui, ammo_str, ammo_size);
            const float space_width = ui_measure_text(client->ui, " ", ammo_size);

            const float text_width = ammo_str_width + space_width + ui_measure_text(client->ui, max_ammo_str, ammo_size);
            const vec2 ammo_holder_size = {text_width + 107.0f * client->ui->scale, (65.0f + 7.0f + 8.0f) * client->ui->scale};
            const float reload_animation = client->me->weapon->reload_time ? client->me->reload_timer / (client->me->weapon->reload_time * client->game.config.reload_speed) : 0.0f;

            ui_round_rect(client->ui, background_color, anchor.x - ammo_holder_size.x, anchor.y - ammo_holder_size.y, ammo_holder_size.x, ammo_holder_size.y, 10.0f * client->ui->scale);

            if (reload_animation) {
                const vec4 reload_color = {1.0f, 1.0f, 1.0f, 0.247f};
                ui_fill_rect_rclip(client->ui, reload_color, anchor.x - ammo_holder_size.x, anchor.y - ammo_holder_size.y, ammo_holder_size.x, ammo_holder_size.y, 10.0f * client->ui->scale, 1.0f - reload_animation);
            }

            ui_fill_text(client->ui, white, ammo_str, anchor.x - ammo_holder_size.x + 20.0f * client->ui->scale, anchor.y - ammo_holder_size.y * 0.5f + (35.0f - 14.0f) * client->ui->scale, ammo_size);
            ui_fill_text(client->ui, white, " ", anchor.x - ammo_holder_size.x + ammo_str_width + 20.0f * client->ui->scale, anchor.y - ammo_holder_size.y * 0.5f + (35.0f - 14.0f) * client->ui->scale, ammo_size);
            ui_fill_text(client->ui, max_ammo_color, max_ammo_str, anchor.x - ammo_holder_size.x + ammo_str_width + space_width + 20.0f * client->ui->scale, anchor.y - ammo_holder_size.y * 0.5f + (35.0f - 14.0f) * client->ui->scale, ammo_size);

            ui_draw_image(client->ui, ammo_icon, anchor.x - ammo_holder_size.x + text_width + 35.0f * client->ui->scale, anchor.y - (55.0f + 7.0f) * client->ui->scale, 55.0f * client->ui->scale, 55.0f * client->ui->scale);

            free(ammo_str);
            free(max_ammo_str);
        }

        for (size_t i = 0; i < client->me->loadout_size; i++) {
            const Weapon *weapon =  client->game.weapons[client->me->loadout[i]];
            if (!weapon) continue;

            char *icon_suffixed = concat(weapon->icon, ".png");
            char *icon_path = concat("textures/weapons/", icon_suffixed);

            char *full_icon_path = concat(assets_path(), icon_path);
            const unsigned int icon = load_texture(full_icon_path);

            free(icon_path);
            free(icon_suffixed);
            free(full_icon_path);

            if (!icon) continue;

            // TODO: draw weapon icon
            // ui_draw_image(client->ui, icon, 0, 0, 200.0f, 200.0f);
        }
    }

    { // top left HUD
        const vec2 anchor = {20.0f * client->ui->scale, 20.0f * client->ui->scale};

        if (!timer_icon) {
            char *icon_path = concat(assets_path(), "img/timer.png");

            timer_icon = load_texture(icon_path);
            free(icon_path);
        }

        if (timer_icon) {
            const char *timer = "04:00";
            const float timer_width = ui_measure_text(client->ui, timer, 32.0f * client->ui->scale);

            ui_round_rect(client->ui, background_color, anchor.x, anchor.y, timer_width + 88.0f * client->ui->scale, 76.0f * client->ui->scale, 10.0f * client->ui->scale);
            ui_draw_image(client->ui, timer_icon, anchor.x + 10.0f * client->ui->scale, anchor.y + (76.0f - 45.0f) * 0.5f * client->ui->scale, 45.0f * client->ui->scale, 45.0f * client->ui->scale);
            ui_fill_text(client->ui, white, timer, anchor.x + 68.0f * client->ui->scale, anchor.y + (76.0f - 18.0f) * client->ui->scale, 32.0f * client->ui->scale);
        }
    }
}
