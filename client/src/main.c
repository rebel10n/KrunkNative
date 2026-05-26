#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <client.h>
#include <math.h>
#include <stdlib.h>
#include <pcg_basic.h>
#include <string.h>
#include <time.h>
#include <stb_image.h>

#ifdef WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif

geometry_cache_map g_geometry_cache;
texture_cache_map g_texture_cache;
glyph_cache_map g_glyph_cache;

FT_Library g_freetype;
FT_Face g_game_font;

Geometry *g_cube_geometry;
Geometry *g_plane_geometry;
Geometry *g_ramp_geometry;

unsigned int g_blank_texture;
unsigned int g_active_texture;
unsigned int g_active_shader;
int g_skip_map_meshes;
RenderLighting g_render_lighting = {
    .ambient_color = {0.592f, 0.627f, 0.659f, 1.0f},
    .light_color = {0.949f, 0.973f, 0.988f, 1.0f},
    .sky_color = {0.863f, 0.910f, 0.929f, 1.0f},
    .fog_color = {0.553f, 0.604f, 0.627f, 1.0f},
    .light_direction = {0.0f, 0.809f, -0.588f},
    .ambient_intensity = 1.0f,
    .light_intensity = 1.3f,
    .fog_near = 1.0f,
    .fog_far = 2000.0f,
    .enabled = 1,
    .fog_enabled = 1,
};

void client_load_map(Client*);
void client_unload_map(Client*);
void client_tick(Client*, float, float);
void client_tick_net(Client*);
void resize_viewport(GLFWwindow*, int, int);

static vec4 json_color_or(const cJSON *json, const vec4 fallback) {
    if (cJSON_IsNumber(json)) return hex_to_vec((int) cJSON_GetNumberValue(json));

    if (cJSON_IsString(json)) {
        vec4 parsed = fallback;
        if (!parse_hex_color(cJSON_GetStringValue(json), &parsed)) return parsed;
    }

    return fallback;
}

static float json_float_or(const cJSON *json, const float fallback) {
    return cJSON_IsNumber(json) ? (float) cJSON_GetNumberValue(json) : fallback;
}

static void client_apply_map_lighting(const Map *map) {
    if (!map || !map->raw_data) return;

    const cJSON *raw = map->raw_data;

    g_render_lighting.ambient_color = json_color_or(cJSON_GetObjectItem(raw, "ambient"), g_render_lighting.ambient_color);
    g_render_lighting.light_color = json_color_or(cJSON_GetObjectItem(raw, "light"), g_render_lighting.light_color);
    g_render_lighting.sky_color = json_color_or(cJSON_GetObjectItem(raw, "sky"), g_render_lighting.sky_color);
    g_render_lighting.fog_color = json_color_or(cJSON_GetObjectItem(raw, "fog"), g_render_lighting.fog_color);

    g_render_lighting.ambient_intensity = json_float_or(cJSON_GetObjectItem(raw, "ambientI"), 1.0f);
    g_render_lighting.light_intensity = json_float_or(cJSON_GetObjectItem(raw, "lightI"), 1.3f);
    g_render_lighting.fog_near = 1.0f;
    g_render_lighting.fog_far = json_float_or(cJSON_GetObjectItem(raw, "fogD"), 2000.0f);
    g_render_lighting.enabled = 1;
    g_render_lighting.fog_enabled = cJSON_GetObjectItem(raw, "fog") != NULL && g_render_lighting.fog_far > g_render_lighting.fog_near;

    const float sun_y = json_float_or(cJSON_GetObjectItem(raw, "sunAngY"), 54.0f);
    const float sun_x = json_float_or(cJSON_GetObjectItem(raw, "sunAngX"), 90.0f);
    const float u = (float) M_PI * (sun_y / -180.0f);
    const float v = (float) M_PI * (sun_x / -180.0f);

    g_render_lighting.light_direction.x = cosf(v);
    g_render_lighting.light_direction.y = sinf(v) * sinf(u);
    g_render_lighting.light_direction.z = sinf(v) * cosf(u);

    const float len = sqrtf(
        g_render_lighting.light_direction.x * g_render_lighting.light_direction.x +
        g_render_lighting.light_direction.y * g_render_lighting.light_direction.y +
        g_render_lighting.light_direction.z * g_render_lighting.light_direction.z
    );

    if (len > 0.0f) {
        g_render_lighting.light_direction.x /= len;
        g_render_lighting.light_direction.y /= len;
        g_render_lighting.light_direction.z /= len;
    }

    glClearColor(g_render_lighting.sky_color.x, g_render_lighting.sky_color.y, g_render_lighting.sky_color.z, g_render_lighting.sky_color.w);
}

int main() {
    char *rand_memory = malloc(1);
    pcg32_srandom(time(NULL), *(unsigned int *) &rand_memory);
    free(rand_memory);

    vt_init(&g_geometry_cache);
    vt_init(&g_texture_cache);
    vt_init(&g_glyph_cache);

    char *game_font_path = concat(assets_path(), "css/fonts/font2.ttf");

    if (FT_Init_FreeType(&g_freetype) || FT_New_Face(g_freetype, game_font_path, 0, &g_game_font)) {
        return -1;
    }

    free(game_font_path);

    if (!glfwInit()) return -1;

    static Client INSTANCE = {};

    pthread_mutex_init(&INSTANCE.net_lock, NULL);
    pthread_mutex_init(&INSTANCE.local_server_lock, NULL);
    INSTANCE.net_socket = -1;

    INSTANCE.camera.zoom = 1.0f;
    INSTANCE.camera.fov = M_PI / 2.0f;
    INSTANCE.camera.near = 0.1f;
    INSTANCE.camera.far = 10000.0f;

    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);

    INSTANCE.window = glfwCreateWindow(1280, 720, "KrunkNative", NULL, NULL);
    if (!INSTANCE.window) return -1;

    GLFWimage icon = {0};
    char *icon_path = concat(assets_path(), "img/icon.png");

    int icon_channels;
    icon.pixels = stbi_load(icon_path, &icon.width, &icon.height, &icon_channels, 4);

    if (icon.pixels) {
        glfwSetWindowIcon(INSTANCE.window, 1, &icon);
        stbi_image_free(icon.pixels);
    }

    free(icon_path);

    glfwSetFramebufferSizeCallback(INSTANCE.window, resize_viewport);
    glfwMakeContextCurrent(INSTANCE.window);

    if (glfwRawMouseMotionSupported()) {
        glfwSetInputMode(INSTANCE.window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
    }

    if (!gladLoadGLLoader((void *) glfwGetProcAddress)) return -1;

    glfwSwapInterval(0);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    INSTANCE.scene = scene_init();
    INSTANCE.fps_scene = scene_init();
    INSTANCE.ui = ui_init();

    if (!INSTANCE.scene || !INSTANCE.fps_scene || !INSTANCE.ui) return -1;

    game_configure(&INSTANCE.game, NULL, NULL, 0, NULL, 0, NULL, 0, NULL, 0);

    if (!local_server_start(&INSTANCE)) return -1;

    double last_tick = glfwGetTime();

    while (!glfwWindowShouldClose(INSTANCE.window)) {
        const float now = (float) glfwGetTime();
        const float delta = (float) (now - last_tick);

        last_tick = now;

        client_tick(&INSTANCE, now, delta);
        glfwSwapBuffers(INSTANCE.window);
        glfwPollEvents();
    }

    INSTANCE.net_should_quit = 1;
    local_server_stop(&INSTANCE);

    return 0;
}

void client_load_map(Client *client) {
    if (!client->game.map) return;

    client_apply_map_lighting(client->game.map);

    for (size_t i = 0; i < client->game.map->object_count; i++) {
        const Object *object = client->game.map->objects[i];
        if (object->mesh) scene_add_mesh(client->scene, object->mesh);
    }
}

void client_unload_map(Client *client) {
    if (!client->game.map) return;

    for (size_t i = 0; i < client->game.map->object_count; i++) {
        const Object *object = client->game.map->objects[i];
        if (object->mesh) scene_remove_mesh(client->scene, object->mesh);
    }
}

void client_clear_players(Client *client) {
    for (size_t i = 0; i < client->game.player_count; i++) {
        Player *player = client->game.players[i];
        if (!player) continue;

        if (player->mesh) {
            scene_remove_player_mesh(player->render_you ? client->fps_scene : client->scene, player->mesh, player->loadout_size);
            player_meshes_fini(player);
        }

        free(player->loadout);
        free(player->ammo);
        free(player->reloads);
        free(player->input_queue);
        free(player);
    }

    free(client->game.players);
    client->game.players = NULL;
    client->game.player_count = 0;
    client->me = NULL;
    client->in_game = 0;
    client->spawn_requested = 0;
    client->net_next_input_seq = 0;
    client->last_swap_key = 0;
}

Player *client_find_player(Client *client, const int uid) {
    for (size_t i = 0; i < client->game.player_count; i++) {
        Player *player = client->game.players[i];
        if (player && player->uid == uid) return player;
    }

    return NULL;
}

void client_add_player_mesh(Client *client, Player *player, const int render_you) {
    if (player->mesh) return;

    player_generate_meshes(player, render_you);
    player_swap_weapon(player, player->loadout_index, 1, 1, 1);
    player_update_meshes(player, 0);

    if (render_you) {
        scene_add_player_mesh(client->fps_scene, player->mesh, player->loadout_size);
    } else {
        scene_add_player_mesh(client->scene, player->mesh, player->loadout_size);
    }
}

void client_apply_spawn(Client *client, const NetSpawnPacket *packet) {
    Player *player = client_find_player(client, packet->uid);

    if (!player) {
        player = player_init(&client->game);
        if (!player) return;

        player->uid = packet->uid;
        game_players_add(&client->game, player);
        player_spawn(player);
    } else if (!player->loadout_size) {
        player_spawn(player);
    }

    player->active = packet->active;
    player->position.x = packet->position[0];
    player->position.y = packet->position[1];
    player->position.z = packet->position[2];
    player->direction.x = packet->direction[0];
    player->direction.y = packet->direction[1];

    if (packet->is_you) {
        client->me = player;
        player->is_you = 1;
        client_add_player_mesh(client, player, 1);
    } else {
        client_add_player_mesh(client, player, 0);
    }
}

void client_apply_state(Client *client, const NetStatePacket *packet) {
    Player *player = client_find_player(client, packet->uid);
    if (!player) return;

    if (player == client->me) {
        player->input_seq = packet->ack_seq;
        return;
    }

    net_packet_apply_player(player, packet);
}

void client_send_cycle_packet(Client *client) {
    pthread_mutex_lock(&client->net_lock);
    const int socket = client->net_socket;
    pthread_mutex_unlock(&client->net_lock);

    if (client->local_server_running) {
        local_server_queue_packet(client, NET_PACKET_CYCLE, NULL, 0);
    } else if (socket >= 0) {
        client_net_send_packet(socket, NET_PACKET_CYCLE, NULL, 0);
    } else if (client->game.is_local && client->game.map_count) {
        if (client->map_loaded) client_unload_map(client);
        client_clear_players(client);

        const int next_map = (client->game.current_map_index + 1) % (int) client->game.map_count;
        game_init(&client->game, next_map, client->game.current_mode_index, 1);
        client_load_map(client);
        client->map_loaded = client->game.ready;
    }
}

void client_tick_net(Client *client) {
    pthread_mutex_lock(&client->net_lock);

    const size_t packet_count = client->net_packet_count;
    NetPacket *packets = client->net_packets;

    client->net_packet_count = 0;
    client->net_packets = NULL;

    pthread_mutex_unlock(&client->net_lock);

    for (size_t i = 0; i < packet_count; i++) {
        const NetPacket *packet = &packets[i];

        switch (packet->type) {
            case NET_PACKET_INIT: {
                if (packet->length != sizeof(NetInitPacket)) break;

                NetInitPacket init;
                memcpy(&init, packet->payload, sizeof(init));

                if (client->map_loaded) client_unload_map(client);
                client_clear_players(client);

                game_init(&client->game, init.map_index, init.mode_index, 0);
                client_load_map(client);
                client->map_loaded = client->game.ready;
                break;
            }
            case NET_PACKET_SPAWN: {
                if (packet->length != sizeof(NetSpawnPacket) || !client->game.ready) break;

                NetSpawnPacket spawn;
                memcpy(&spawn, packet->payload, sizeof(spawn));
                client_apply_spawn(client, &spawn);
                break;
            }
            case NET_PACKET_STATE: {
                if (packet->length != sizeof(NetStatePacket) || !client->game.ready) break;

                NetStatePacket state;
                memcpy(&state, packet->payload, sizeof(state));
                client_apply_state(client, &state);
                break;
            }
            default:
                break;
        }
    }

    free(packets);
}

void resize_viewport(GLFWwindow *window, const int width, const int height) {
    if (glfwGetCurrentContext() != window) return;
    glViewport(0, 0, width, height);
}

void client_animate_object_texture(Object *object, const float now) {
    if (!object->mesh || !object->tex_anim) return;

    const Mesh *mesh = object->mesh;
    const TextureAnimation *tex_anim = object->tex_anim;

    if (mesh->material->vtable != &basic_material_vtable) return;

    BasicMaterial *material = (BasicMaterial *) mesh->material;

    if (tex_anim->frames) {
        const int frame = (int) (fmodf(now, (float) tex_anim->frames * tex_anim->frame_time) / tex_anim->frame_time);

        material->texture_repeat.x = 1.0f / (float) tex_anim->frames;
        material->texture_offset.x = (float) frame;
    }

    if (tex_anim->move) {
        const float period = 1.0f / tex_anim->move;
        const float progress = fmodf(now, period) / period;

        if (tex_anim->move_direction) material->texture_offset.y = progress;
        else material->texture_offset.x = progress;
    }
}

void client_tick_textures(Client *client, const float now) {
    if (!client->game.map) return;

    for (size_t i = 0; i < client->game.map->object_count; i++) {
        Object *object = client->game.map->objects[i];
        client_animate_object_texture(object, now);
    }
}

void client_enter_game(Client *client) {
    if (!client->game.ready || client->in_game) return;
    client->in_game = 1;

    if (client->game.is_local) {
        if (!client->me) {
            client->me = player_init(&client->game);

            if (!client->me) {
                client->in_game = 0;
                return;
            }

            game_players_add(&client->game, client->me);
        }

        client->me->is_you = 1;
        player_spawn(client->me);
        player_generate_meshes(client->me, 1);
        player_swap_weapon(client->me, 0, 1, 0, 0);
        scene_add_player_mesh(client->fps_scene, client->me->mesh, client->me->loadout_size);
    } else {
        const NetEntPacket packet = {0};

        pthread_mutex_lock(&client->net_lock);
        const int socket = client->net_socket;
        pthread_mutex_unlock(&client->net_lock);

        if (client->local_server_running) {
            local_server_queue_packet(client, NET_PACKET_ENT, &packet, sizeof(packet));
            client->spawn_requested = 1;
        } else if (socket >= 0) {
            client_net_send_packet(socket, NET_PACKET_ENT, &packet, sizeof(packet));
            client->spawn_requested = 1;
        } else {
            client->in_game = 0;
        }
    }
}

void client_tick(Client *client, const float now, const float delta) {
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    client_tick_net(client);

    if (!client->game.ready) return;

    client_tick_textures(client, now);
    scene_render(client->scene, &client->camera);

    glClear(GL_DEPTH_BUFFER_BIT);
    scene_render(client->fps_scene, &client->camera);

    ui_update(client->ui);

    if (!client->mouse_state.locked) {
        hud_render(client, now);
    } else {
        overlay_render(client, delta);
    }

    const int debug_key = glfwGetKey(client->window, GLFW_KEY_GRAVE_ACCENT);

    if (debug_key && !client->last_debug_key) {
        client_send_cycle_packet(client);
    }

    client->last_debug_key = debug_key;

    double x, y;
    glfwGetCursorPos(client->window, &x, &y);

    const int fullscreen_key = glfwGetKey(client->window, GLFW_KEY_F11);

    if (fullscreen_key && !client->last_fullscreen_key) {
        const int fullscreen = !!glfwGetWindowMonitor(client->window);

        if (fullscreen) {
            glfwSetWindowMonitor(client->window, NULL,
                client->windowed_rect.x, client->windowed_rect.y,
                client->windowed_rect.width, client->windowed_rect.height, 0
            );
        } else {
            glfwGetWindowPos(client->window, &client->windowed_rect.x, &client->windowed_rect.y);
            glfwGetWindowSize(client->window, &client->windowed_rect.width, &client->windowed_rect.height);

            GLFWmonitor *monitor = glfwGetPrimaryMonitor();
            const GLFWvidmode *mode = glfwGetVideoMode(monitor);

            glfwSetWindowMonitor(client->window, monitor, 0, 0, mode->width, mode->height, mode->refreshRate);
        }
    }

    client->last_fullscreen_key = fullscreen_key;
    client->mouse_state.locked = glfwGetInputMode(client->window, GLFW_CURSOR) == GLFW_CURSOR_DISABLED;

    if (client->mouse_state.locked && glfwGetKey(client->window, GLFW_KEY_ESCAPE)) {
        glfwSetInputMode(client->window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        client->mouse_state.locked = 0;
    } else if (!client->mouse_state.locked && glfwGetMouseButton(client->window, GLFW_MOUSE_BUTTON_LEFT)) {
        glfwSetInputMode(client->window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);

        client->mouse_state.locked = 1;
        client->mouse_state.last_pos.x = (float) x;
        client->mouse_state.last_pos.y = (float) y;
    }

    if (client->mouse_state.locked && (!client->me || !client->me->active)) {
        client_enter_game(client);
    }

    if (!client->me) {
        client->camera.position = client->game.map->camera_position;
        client->camera.rotation.y += delta * 0.1f;
        client->camera.rotation.y = fmodf(client->camera.rotation.y, 2.0f * (float) M_PI);
    }

    const int noclip_key = glfwGetKey(client->window, GLFW_KEY_N);

    if (client->me && client->me->active) {
        Input input = {0};
        vec2 mouse_delta = {0};

        mouse_delta.x = (float) x - client->mouse_state.last_pos.x;
        mouse_delta.y = (float) y - client->mouse_state.last_pos.y;

        input.move_dir = -1;
        input.delta = delta;

        input.x_dir = client->me->direction.x;
        input.y_dir = client->me->direction.y;

        if (client->mouse_state.locked) {
            if (noclip_key && !client->last_noclip_key) client->me->noclip ^= 1;

            input.x_dir -= mouse_delta.y * game_constants.mouse_sensitivity / client->camera.zoom;
            input.y_dir -= mouse_delta.x * game_constants.mouse_sensitivity / client->camera.zoom;

            input.jump = glfwGetKey(client->window, GLFW_KEY_SPACE);
            input.crouch = glfwGetKey(client->window, GLFW_KEY_LEFT_SHIFT);
            input.reload = glfwGetKey(client->window, GLFW_KEY_R);
            input.shoot = glfwGetMouseButton(client->window, GLFW_MOUSE_BUTTON_LEFT);
            input.scope = glfwGetMouseButton(client->window, GLFW_MOUSE_BUTTON_RIGHT);

            if (glfwGetKey(client->window, GLFW_KEY_E)) input.swap = 1;
            else if (glfwGetKey(client->window, GLFW_KEY_Q)) input.swap = 2;

            const int swap_key = input.swap;

            if (swap_key == client->last_swap_key) input.swap = 0;
            client->last_swap_key = swap_key;

            const int forward = glfwGetKey(client->window, GLFW_KEY_W);
            const int back = glfwGetKey(client->window, GLFW_KEY_S);
            const int left = glfwGetKey(client->window, GLFW_KEY_A);
            const int right = glfwGetKey(client->window, GLFW_KEY_D);

            if (forward ^ back) {
                input.move_dir += forward ? 1 : 5;
                if (left ^ right) input.move_dir += right ? (forward ? 1 : -1) : forward ? 7 : 1;
            } else if (left ^ right) {
                input.move_dir += right ? 3 : 7;
            }
        }

        input.seq = client->net_next_input_seq++;
        player_queue_input(client->me, &input);

        if (!client->game.is_local) {
            NetInputPacket packet;
            net_packet_from_input(&packet, &input);

            pthread_mutex_lock(&client->net_lock);
            const int socket = client->net_socket;
            pthread_mutex_unlock(&client->net_lock);

            if (client->local_server_running) {
                local_server_queue_packet(client, NET_PACKET_INPUT, &packet, sizeof(packet));
            } else if (socket >= 0) {
                client_net_send_packet(socket, NET_PACKET_INPUT, &packet, sizeof(packet));
            }
        }

        client->camera.position = client->me->position;
        client->camera.position.y += client->me->height - game_constants.camera_height;

        client->camera.rotation.x = client->me->direction.x + client->me->recoil_anim_y * game_constants.recoil_mlt;
        client->camera.rotation.y = client->me->direction.y;

        client->camera.zoom = 1.0f + (client->me->weapon->zoom - 1.0f) * client->me->aim_val * 1.0f; // TODO: adsFovMlt
    }

    client->mouse_state.last_pos.x = (float) x;
    client->mouse_state.last_pos.y = (float) y;
    client->last_noclip_key = noclip_key;

    game_tick(&client->game, now, delta);
}
