#include <glad/glad.h>
#include <client.h>
#include <stdlib.h>
#include <stb_image.h>

void game_window_resize(GLFWwindow *window, const int width, const int height) {
    GameWindow *game_window = glfwGetWindowUserPointer(window);
    if (!game_window) return;

    game_window->width = width;
    game_window->height = height;
}

GameWindow *game_window_init(const int width, const int height) {
    GLFWwindow *glfw_window = glfwCreateWindow(width, height, "KrunkNative", NULL, NULL);
    if (!glfw_window) return NULL;

    GLFWimage icon;
    int icon_channels;
    char *icon_path = concat(client_assets_path(), "icon.png");

    icon.pixels = stbi_load(icon_path, &icon.width, &icon.height, &icon_channels, 4);
    free(icon_path);

    if (icon.pixels) glfwSetWindowIcon(glfw_window, 1, &icon);

    glfwMakeContextCurrent(glfw_window);
    if (!gladLoadGLLoader((void *) glfwGetProcAddress)) return NULL;

    // glEnable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    GameWindow *game_window = calloc(1, sizeof(GameWindow));

    glfwSetWindowUserPointer(glfw_window, game_window);
    glfwSetFramebufferSizeCallback(glfw_window, game_window_resize);

    game_window->glfw_window = glfw_window;
    game_window->width = width;
    game_window->height = height;

    game_window->scene = scene_init();

    return game_window;
}

void game_window_render(GameWindow *game_window) {
    glEnable(GL_DEPTH_TEST);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glViewport(0, 0, game_window->width, game_window->height);

    if (game_window->scene) {
        scene_render(game_window->scene);
    }

    if (game_window->ui) {
        glDisable(GL_DEPTH_TEST);
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

        ui_render(game_window->ui);
    }
}

void game_window_fini(GameWindow *game_window) {
    if (game_window->glfw_window) {
        glfwSetWindowShouldClose(game_window->glfw_window, 1);
    }

    free(game_window);
}