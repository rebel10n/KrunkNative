#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <client.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <pcg_basic.h>
#include <time.h>

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

int main() {
    pcg32_srandom(time(NULL), 0x1337);

    vt_init(&g_model_cache);
    vt_init(&g_texture_cache);

    if (!glfwInit()) return -1;

    GameWindow *game_window = game_window_init(800, 800);
    if (!game_window) return -1;

    char *map_path = concat(client_assets_path(), "maps/kanji.json");
    FILE *map = fopen(map_path, "rb");

    free(map_path);
    fseek(map, 0, SEEK_END);

    const size_t map_size = ftell(map);
    char *map_data = malloc(map_size + 1);

    fseek(map, 0, SEEK_SET);
    fread(map_data, 1, map_size, map);
    fclose(map);

    map_data[map_size] = 0;

    const GameMap *map_obj = game_map_init(map_data);

    free(map_data);

    for (size_t i = 0; i < map_obj->object_count; i++) {
        const Object *object = map_obj->objects[i];
        if (object->mesh) scene_add_mesh(game_window->scene, object->mesh);
    }

    MainMenu *ui = main_menu_init();
    game_window->ui = (UI *) ui;

    double last_tick = glfwGetTime();

    while (!glfwWindowShouldClose(game_window->glfw_window)) {
        glfwPollEvents();

        const double now = glfwGetTime();
        const double delta = now - last_tick;

        client_tick(game_window, delta);
        last_tick = now;

        glfwSwapBuffers(game_window->glfw_window);
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

double last_mouse_x, last_mouse_y;

void client_tick(GameWindow *game_window, const double delta) {
    game_window_render(game_window);

    vec3 forward = {0};
    vec3 right = {0};
    vec3 up = {0};

    const float yaw[] = {
        cosf(game_window->scene->camera.rotation.y), 0.0f, -sinf(game_window->scene->camera.rotation.y), 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        sinf(game_window->scene->camera.rotation.y), 0.0f, cosf(game_window->scene->camera.rotation.y), 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f,
    };

    const float pitch[] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, cosf(game_window->scene->camera.rotation.x), -sinf(game_window->scene->camera.rotation.x), 0.0f,
        0.0f, sinf(game_window->scene->camera.rotation.x), cosf(game_window->scene->camera.rotation.x), 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f,
    };

    float out[16];
    mat4x4(yaw, pitch, out);

    forward.x = out[2];
    forward.y = out[6];
    forward.z = out[10];

    right.x = out[0];
    right.y = out[4];
    right.z = out[8];

    up.x = out[1];
    up.y = out[5];
    up.z = out[9];

    float vel_x = 0.0f;
    float vel_y = 0.0f;
    float vel_z = 0.0f;

    int locked = glfwGetInputMode(game_window->glfw_window, GLFW_CURSOR) == GLFW_CURSOR_DISABLED;

    if (glfwGetMouseButton(game_window->glfw_window, GLFW_MOUSE_BUTTON_LEFT) && !locked) {
        glfwSetInputMode(game_window->glfw_window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        glfwGetCursorPos(game_window->glfw_window, &last_mouse_x, &last_mouse_y);
    } else if (glfwGetKey(game_window->glfw_window, GLFW_KEY_ESCAPE) && locked) {
        glfwSetInputMode(game_window->glfw_window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        locked = false;
    }

    if (locked) {
        double mouse_x, mouse_y;
        glfwGetCursorPos(game_window->glfw_window, &mouse_x, &mouse_y);

        const double delta_x = mouse_x - last_mouse_x;
        const double delta_y = mouse_y - last_mouse_y;

        game_window->scene->camera.rotation.y += (float) delta_x * 0.001f;
        game_window->scene->camera.rotation.x -= (float) delta_y * 0.001f;

        if (game_window->scene->camera.rotation.x > M_PI / 2.0f) game_window->scene->camera.rotation.x = M_PI / 2.0f;
        else if (game_window->scene->camera.rotation.x < -M_PI / 2.0f) game_window->scene->camera.rotation.x = -(float) M_PI / 2.0f;

        last_mouse_x = mouse_x;
        last_mouse_y = mouse_y;
    }

    if (glfwGetKey(game_window->glfw_window, GLFW_KEY_W)) {
        vel_z = (float) delta * -50.0f;
    } else if (glfwGetKey(game_window->glfw_window, GLFW_KEY_S)) {
        vel_z = (float) delta * 50.0f;
    }

    if (glfwGetKey(game_window->glfw_window, GLFW_KEY_SPACE)) {
        vel_y = (float) delta * 50.0f;
    } else if (glfwGetKey(game_window->glfw_window, GLFW_KEY_LEFT_CONTROL)) {
        vel_y = (float) delta * -50.0f;
    }

    if (glfwGetKey(game_window->glfw_window, GLFW_KEY_A)) {
        vel_x = (float) delta * -50.0f;
    } else if (glfwGetKey(game_window->glfw_window, GLFW_KEY_D)) {
        vel_x = (float) delta * 50.0f;
    }

    game_window->scene->camera.position.x += vel_x * right.x + vel_y * up.x + vel_z * forward.x;
    game_window->scene->camera.position.y += vel_x * right.y + vel_y * up.y + vel_z * forward.y;
    game_window->scene->camera.position.z += vel_x * right.z + vel_y * up.z + vel_z * forward.z;
}