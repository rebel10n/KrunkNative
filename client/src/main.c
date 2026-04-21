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

unsigned int g_blank_texture;

void client_tick(Client*, float, float);
void resize_viewport(GLFWwindow*, int, int);

int main() {
    pcg32_srandom(time(NULL), 0x1337);

    vt_init(&g_model_cache);
    vt_init(&g_texture_cache);

    if (!glfwInit()) return -1;

    static Client INSTANCE = {0};

    INSTANCE.camera.fov = M_PI / 2.0f;
    INSTANCE.camera.near = 0.1f;
    INSTANCE.camera.far = 10000.0f;

    INSTANCE.window = glfwCreateWindow(800, 800, "KrunkNative", NULL, NULL);
    if (!INSTANCE.window) return -1;

    glfwSetFramebufferSizeCallback(INSTANCE.window, resize_viewport);
    glfwMakeContextCurrent(INSTANCE.window);

    if (!gladLoadGLLoader((void *) glfwGetProcAddress)) return -1;

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    INSTANCE.scene = scene_init();
    if (!INSTANCE.scene) return -1;

    INSTANCE.ui = (UI *) main_menu_init();

    // === LOAD MAP FOR DEBUG ===
    char *map_path = concat(client_assets_path(), "maps/littletown.json");

    size_t map_size;
    unsigned char *map_data;

    if (!read_file(map_path, &map_data, &map_size)) {
        const GameMap *map = game_map_init((const char *) map_data);

        for (size_t i = 0; i < map->object_count; i++) {
            if (!map->objects[i]->mesh) continue;
            scene_add_mesh(INSTANCE.scene, map->objects[i]->mesh);
        }

        free(map_data);
    }

    free(map_path);
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

double last_mouse_x, last_mouse_y;

void client_tick(Client *client, const float now, const float delta) {
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    scene_render(client->scene, &client->camera);
    ui_render(client->ui);

    vec3 forward = {0};
    vec3 right = {0};
    vec3 up = {0};

    const float yaw[] = {
        cosf(client->camera.rotation.y), 0.0f, -sinf(client->camera.rotation.y), 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        sinf(client->camera.rotation.y), 0.0f, cosf(client->camera.rotation.y), 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f,
    };

    const float pitch[] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, cosf(client->camera.rotation.x), -sinf(client->camera.rotation.x), 0.0f,
        0.0f, sinf(client->camera.rotation.x), cosf(client->camera.rotation.x), 0.0f,
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

    int locked = glfwGetInputMode(client->window, GLFW_CURSOR) == GLFW_CURSOR_DISABLED;

    if (glfwGetMouseButton(client->window, GLFW_MOUSE_BUTTON_LEFT) && !locked) {
        glfwSetInputMode(client->window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        glfwGetCursorPos(client->window, &last_mouse_x, &last_mouse_y);
    } else if (glfwGetKey(client->window, GLFW_KEY_ESCAPE) && locked) {
        glfwSetInputMode(client->window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        locked = false;
    }

    if (locked) {
        double mouse_x, mouse_y;
        glfwGetCursorPos(client->window, &mouse_x, &mouse_y);

        const double delta_x = mouse_x - last_mouse_x;
        const double delta_y = mouse_y - last_mouse_y;

        client->camera.rotation.y += (float) delta_x * 0.001f;
        client->camera.rotation.x -= (float) delta_y * 0.001f;

        if (client->camera.rotation.x > M_PI / 2.0f) client->camera.rotation.x = M_PI / 2.0f;
        else if (client->camera.rotation.x < -M_PI / 2.0f) client->camera.rotation.x = -(float) M_PI / 2.0f;

        last_mouse_x = mouse_x;
        last_mouse_y = mouse_y;
    }

    if (glfwGetKey(client->window, GLFW_KEY_W)) {
        vel_z = (float) delta * -50.0f;
    } else if (glfwGetKey(client->window, GLFW_KEY_S)) {
        vel_z = (float) delta * 50.0f;
    }

    if (glfwGetKey(client->window, GLFW_KEY_SPACE)) {
        vel_y = (float) delta * 50.0f;
    } else if (glfwGetKey(client->window, GLFW_KEY_LEFT_CONTROL)) {
        vel_y = (float) delta * -50.0f;
    }

    if (glfwGetKey(client->window, GLFW_KEY_A)) {
        vel_x = (float) delta * -50.0f;
    } else if (glfwGetKey(client->window, GLFW_KEY_D)) {
        vel_x = (float) delta * 50.0f;
    }

    client->camera.position.x += vel_x * right.x + vel_y * up.x + vel_z * forward.x;
    client->camera.position.y += vel_x * right.y + vel_y * up.y + vel_z * forward.y;
    client->camera.position.z += vel_x * right.z + vel_y * up.z + vel_z * forward.z;
}