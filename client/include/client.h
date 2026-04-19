#pragma once
#include <shared.h>
#include <glfw/glfw3.h>
#include <cJSON.h>

typedef struct {
    float near;
    float far;
    float fov;

    vec3 position;
    vec3 rotation;

    float projection_matrix[16];
    float world_inverse_matrix[16];
} Camera;

void camera_update_projection_matrix(Camera*, float);
void camera_update_world_inverse_matrix(Camera*);

typedef struct {
    vec3 position;
    vec2 tex_coord;
} vertex;

typedef struct {
    void (*update_uniforms)(void*);
} MaterialVTable;

typedef struct {
    MaterialVTable *vtable;
    int wireframe;
    unsigned int program;
} Material;

unsigned int shader_compile(const char*, const char*);
void material_update_uniforms(Material*);
void material_fini(Material*);

typedef struct {
    Material base;

    vec4 color;
    unsigned int texture;
} FlatMaterial;

FlatMaterial *flat_material_init();

typedef struct {
    Material base;

    vec4 color;
    unsigned int texture;
} BasicMaterial;

BasicMaterial *basic_material_init();

typedef struct Mesh {
    vec3 position;
    vec3 rotation;
    vec3 scale;

    unsigned int vao;
    unsigned int vbo;
    unsigned int ebo;

    int vertex_count;
    int index_count;

    float transform_matrix[16];
    Material *material;
} Mesh;

Mesh *mesh_init(const vertex*, const unsigned int*, int, int, Material*);
void mesh_update_transform_matrix(Mesh*);
void mesh_fini(Mesh*); // CAUTION: does NOT free underlying Material!

typedef struct {
    Camera camera;

    size_t mesh_count;
    Mesh **meshes;
} Scene;

Scene *scene_init();
void scene_add_mesh(Scene*, Mesh*);
void scene_remove_mesh(Scene*, Mesh*);
void scene_render(Scene*);
void scene_fini(Scene*);

typedef struct {
    void (*render)(void*, int, int);
} UIVTable;

typedef struct {
    UIVTable *vtable;
    FlatMaterial *material;

    unsigned int vao;
    unsigned int vbo;
    unsigned int ebo;
} UI;

void ui_init(UI*);
void ui_draw_color(UI*, vec4);
void ui_draw_texture(UI*, unsigned int, vec4);
void ui_fill_rect(UI*, int, int, int, int);
void ui_render(UI*);
void ui_fini(UI*);

typedef struct {
    UI base;
} MainMenu;

MainMenu *main_menu_init();

typedef struct {
    GLFWwindow *glfw_window;
    int width;
    int height;

    UI *ui;
    Scene *scene;
} GameWindow;

GameWindow *game_window_init(int, int);
void game_window_render(GameWindow*);
void game_window_fini(GameWindow*);

const char *client_assets_path();
void client_tick(GameWindow*, double);

Mesh *map_mesh_init(Object*, const cJSON*);
