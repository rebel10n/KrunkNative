#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <client.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <pcg_basic.h>
#include <time.h>
#include <stb_image.h>

#ifdef WIN32
#include <direct.h>
#define getcwd _getcwd
#else
#include <unistd.h>
#endif

asset_cache_map g_model_cache;
asset_cache_map g_texture_cache;

unsigned long long g_cube_model;
unsigned long long g_plane_model;
unsigned long long g_ramp_model;

unsigned int g_blank_texture;

void client_tick(Client*, float, float);
void resize_viewport(GLFWwindow*, int, int);

int main() {
    char *rand_memory = malloc(1);
    pcg32_srandom(time(NULL), *(unsigned int *) &rand_memory);

    if (rand_memory) free(rand_memory);

    vt_init(&g_model_cache);
    vt_init(&g_texture_cache);

    if (!glfwInit()) return -1;

    static Client INSTANCE = {0};

    INSTANCE.me = &INSTANCE.game.players[0];

    INSTANCE.camera.fov = M_PI / 2.0f;
    INSTANCE.camera.near = 0.1f;
    INSTANCE.camera.far = 10000.0f;

    INSTANCE.window = glfwCreateWindow(1280, 720, "KrunkNative", NULL, NULL);
    if (!INSTANCE.window) return -1;

    GLFWimage icon = {0};
    char *icon_path = concat(client_assets_path(), "icon.png");

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

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    INSTANCE.scene = scene_init();
    if (!INSTANCE.scene) return -1;

    // TODO: proper UI
    INSTANCE.ui = (UI *) main_menu_init();

    // === LOAD MAP FOR DEBUG ===
    char *map_path = concat(client_assets_path(), "maps/citadel.json");

    size_t map_size;
    unsigned char *map_data;

    if (!read_file(map_path, &map_data, &map_size)) {
        Map *map = map_init((const char *) map_data);
        INSTANCE.game.map = map;

        if (map) for (size_t i = 0; i < map->object_count; i++) {
            if (!map->objects[i]->mesh) continue;
            scene_add_mesh(INSTANCE.scene, map->objects[i]->mesh);
        }

        free(map_data);
    }

    free(map_path);

    // === ADD LOCAL PLAYER ===
    Player *me = player_init(&INSTANCE.game);

    if (me) {
        player_spawn(me);
        game_players_add(&INSTANCE.game, me, 1);
    }

    // === END OF DEBUG SECTION ===

    double last_tick = glfwGetTime();

    while (!glfwWindowShouldClose(INSTANCE.window)) {
        glfwPollEvents();

        const float now = (float) glfwGetTime();
        const float delta = (float) (now - last_tick);

        last_tick = now;

        client_tick(&INSTANCE, now, delta);
        glfwSwapBuffers(INSTANCE.window);
    }

    return 0;
}

const char *client_assets_path() {
    char path[256];
    if (!getcwd(path, sizeof(path))) return NULL;

#ifdef WIN32
    const char slash = '\\';
    const char *cmake_debug_suffix = "\\cmake-build-debug";
    const char *cmake_release_suffix = "\\cmake-build-release";
#else
    const char slash = '/';
    const char *cmake_debug_suffix = "/cmake-build-debug";
    const char *cmake_release_suffix = "/cmake-build-release";
#endif

    const size_t path_length = strlen(path);
    const size_t debug_length = strlen(cmake_debug_suffix);
    const size_t release_length = strlen(cmake_release_suffix);

    int devel = 0;

    if (path_length >= debug_length && !strcmp(path + path_length - debug_length, cmake_debug_suffix)) devel = 1;
    if (path_length > debug_length && !strcmp(path + path_length - debug_length - 1, cmake_debug_suffix) && path[path_length-1] == slash) devel = 1;

    if (path_length >= release_length && !strcmp(path + path_length - release_length, cmake_release_suffix)) devel = 1;
    if (path_length > release_length && !strcmp(path + path_length - release_length - 1, cmake_release_suffix) && path[path_length-1] == slash) devel = 1;

    const char *devel_assets = "../assets/";
    const char *assets = "./assets/";

    return devel ? devel_assets : assets;
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
    if (client->game.map) {
        for (size_t i = 0; i < client->game.map->object_count; i++) {
            Object *object = client->game.map->objects[i];
            client_animate_object_texture(object, now);
        }
    }
}

void client_tick(Client *client, const float now, const float delta) {
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    client_tick_textures(client, now);
    scene_render(client->scene, &client->camera);
    ui_render(client->ui);

    Input input = {0};
    vec2 mouse_delta = {0};

    input.move_dir = -1;
    client->mouse_state.locked = glfwGetInputMode(client->window, GLFW_CURSOR) == GLFW_CURSOR_DISABLED;

    double x, y;
    glfwGetCursorPos(client->window, &x, &y);

    if (client->mouse_state.locked) {
        if (glfwGetKey(client->window, GLFW_KEY_ESCAPE)) {
            glfwSetInputMode(client->window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        }

        mouse_delta.x = (float) x - client->mouse_state.last_pos.x;
        mouse_delta.y = (float) y - client->mouse_state.last_pos.y;

        input.jump = glfwGetKey(client->window, GLFW_KEY_SPACE);
        input.crouch = glfwGetKey(client->window, GLFW_KEY_LEFT_SHIFT);

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

    client->mouse_state.last_pos.x = (float) x;
    client->mouse_state.last_pos.y = (float) y;

    if (!client->mouse_state.locked && glfwGetMouseButton(client->window, GLFW_MOUSE_BUTTON_LEFT)) {
        glfwSetInputMode(client->window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    }

    Player *me = client->game.players ? client->game.players[0] : NULL;

    if (me && me->active) {
        client->camera.position = me->position;
        client->camera.position.y += me->height - game_constants.camera_height;

        client->camera.rotation.x = me->direction.x;
        client->camera.rotation.y = me->direction.y;

        input.seq = ++me->input_seq;
        input.delta = delta;

        input.x_dir = me->direction.x - mouse_delta.y * game_constants.mouse_sensitivity * 0.5f;
        input.y_dir = me->direction.y - mouse_delta.x * game_constants.mouse_sensitivity * 0.5f;

        player_proc_input(me, &input, 0, 0);
    } else {
        client->camera.position = client->game.map->camera_position;
        client->camera.rotation.y = fmodf(client->camera.rotation.y + delta * 0.1f, M_PI * 2.0f);
    }
}