#include <glad/glad.h>
#include <glfw/glfw3.h>
#include <client.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif

int main() {
    if (!glfwInit()) return -1;

    GameWindow *game_window = game_window_init(800, 800);
    if (!game_window) return -1;

    char *map_path = concat(client_assets_path(), "maps/burg.json");
    FILE *map = fopen(map_path, "rb");

    free(map_path);
    fseek(map, 0, SEEK_END);

    const size_t map_size = ftell(map);
    char *map_data = malloc(map_size + 1);

    fseek(map, 0, SEEK_SET);
    fread(map_data, 1, map_size, map);
    fclose(map);

    map_data[map_size] = 0;

#ifdef KRUNKNATIVE_CLIENT
    printf("KRUNKNATIVE_CLIENT is indeed defined!\n");
#endif

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
    if (!_getcwd(path, sizeof(path))) return NULL;

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

void client_tick(GameWindow *game_window, const double delta) {
    game_window_render(game_window);

    // TODO: logic xD

    if (glfwGetKey(game_window->glfw_window, GLFW_KEY_W)) {
        game_window->scene->camera.position.z -= (float) delta * 50.0f;
    } else if (glfwGetKey(game_window->glfw_window, GLFW_KEY_S)) {
        game_window->scene->camera.position.z += (float) delta * 50.0f;
    }

    if (glfwGetKey(game_window->glfw_window, GLFW_KEY_SPACE)) {
        game_window->scene->camera.position.y += (float) delta * 50.0f;
    } else if (glfwGetKey(game_window->glfw_window, GLFW_KEY_LEFT_CONTROL)) {
        game_window->scene->camera.position.y -= (float) delta * 50.0f;
    }

    if (glfwGetKey(game_window->glfw_window, GLFW_KEY_A)) {
        game_window->scene->camera.position.x -= (float) delta * 50.0f;
    } else if (glfwGetKey(game_window->glfw_window, GLFW_KEY_D)) {
        game_window->scene->camera.position.x += (float) delta * 50.0f;
    }
}