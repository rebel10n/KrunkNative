#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <client.h>
#include <math.h>
#include <stdlib.h>
#include <pcg_basic.h>
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

void client_load_map(Client*);
void client_unload_map(Client*);
void client_tick(Client*, float, float);
void resize_viewport(GLFWwindow*, int, int);

int main() {
    char *rand_memory = malloc(1);
    pcg32_srandom(time(NULL), *(unsigned int *) &rand_memory);

    if (rand_memory) free(rand_memory);

    vt_init(&g_geometry_cache);
    vt_init(&g_texture_cache);
    vt_init(&g_glyph_cache);

    char *game_font_path = concat(client_assets_path(), "css/fonts/font2.ttf");

    if (FT_Init_FreeType(&g_freetype) || FT_New_Face(g_freetype, game_font_path, 0, &g_game_font)) {
        return -1;
    }

    free(game_font_path);

    if (!glfwInit()) return -1;

    static Client INSTANCE = {0};

    INSTANCE.camera.fov = M_PI / 2.0f;
    INSTANCE.camera.near = 0.1f;
    INSTANCE.camera.far = 10000.0f;

    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);

    INSTANCE.window = glfwCreateWindow(1280, 720, "KrunkNative", NULL, NULL);
    if (!INSTANCE.window) return -1;

    GLFWimage icon = {0};
    char *icon_path = concat(client_assets_path(), "img/icon.png");

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
    INSTANCE.ui = ui_init();

    if (!INSTANCE.scene || !INSTANCE.ui) return -1;

    game_configure(&INSTANCE.game, NULL, NULL, 0, NULL, 0);
    game_init(&INSTANCE.game, -1, -1);
    client_load_map(&INSTANCE);

    double last_tick = glfwGetTime();

    while (!glfwWindowShouldClose(INSTANCE.window)) {
        const float now = (float) glfwGetTime();
        const float delta = (float) (now - last_tick);

        last_tick = now;

        client_tick(&INSTANCE, now, delta);
        glfwSwapBuffers(INSTANCE.window);
        glfwPollEvents();
    }

    return 0;
}

void client_load_map(Client *client) {
    if (!client->game.map) return;

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
    if (!client->game.map) return;

    for (size_t i = 0; i < client->game.map->object_count; i++) {
        Object *object = client->game.map->objects[i];
        client_animate_object_texture(object, now);
    }
}

void client_tick(Client *client, const float now, const float delta) {
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    if (!client->game.ready) return;

    client_tick_textures(client, now);
    scene_render(client->scene, &client->camera);

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
    }

    if (!client->me) {
        client->camera.position = client->game.map->camera_position;
        client->camera.rotation.y += delta * 0.1f;
        client->camera.rotation.y = fmodf(client->camera.rotation.y, 2.0f * (float) M_PI);
    }

    if (client->me && client->me->active) {
        if (client->mouse_state.locked) {
            Input input = {0};
            vec2 mouse_delta = {0};

            mouse_delta.x = (float) x - client->mouse_state.last_pos.x;
            mouse_delta.y = (float) y - client->mouse_state.last_pos.y;

            input.move_dir = -1;
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

            player_queue_input(client->me, &input);
        }

        client->camera.position = client->me->position;
        client->camera.position.y += client->me->height - game_constants.camera_height;

        client->camera.rotation.x = client->me->direction.x;
        client->camera.rotation.y = client->me->direction.y;
    }

    client->mouse_state.last_pos.x = (float) x;
    client->mouse_state.last_pos.y = (float) y;

    game_tick(&client->game, now, delta);
}