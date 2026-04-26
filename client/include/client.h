#pragma once
#include <shared.h>
#include <GLFW/glfw3.h>
#include <cJSON.h>

#define NAME asset_cache_map
#define KEY_TY char*
#define VAL_TY unsigned long long
#include <verstable.h>

extern asset_cache_map g_model_cache; // low 4 bytes = VBO, high 4 bytes = EBO
extern asset_cache_map g_texture_cache; // low 4 bytes = texID, high 4 bytes = unused

// low 4 bytes = VBO, high 4 bytes = EBO
extern unsigned long long g_cube_model;
extern unsigned long long g_plane_model;
extern unsigned long long g_ramp_model;

extern unsigned int g_blank_texture;

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

    float aspect;
    float border_bottom_left_radius;
    float border_bottom_right_radius;
    float border_top_left_radius;
    float border_top_right_radius;

    float texture_viewport[4];
    unsigned int texture;
} QuadMaterial;

QuadMaterial *quad_material_init();

typedef struct {
    Material base;

    int is_ramp;
    int use_face_tex_scaling;
    vec3 face_scale;

    vec4 color;
    vec4 emissive;

    vec2 texture_repeat;
    vec2 texture_offset;
    unsigned int texture;
} BasicMaterial;

extern MaterialVTable basic_material_vtable;
BasicMaterial *basic_material_init();

typedef struct Mesh {
    vec3 position;
    vec3 rotation;
    vec3 scale;

    unsigned int vao;
    unsigned int vbo;
    unsigned int ebo;

    int visible;
    int vertex_count;
    int index_count;

    float transform_matrix[16];
    Material *material;
} Mesh;

Mesh *mesh_init(unsigned int, unsigned int, Material*);
void mesh_update_transform_matrix(Mesh*);
void mesh_fini(Mesh*); // CAUTION: does NOT free underlying Material!

typedef struct {
    size_t mesh_count;
    Mesh **meshes;
} Scene;

Scene *scene_init();
void scene_add_mesh(Scene*, Mesh*);
void scene_remove_mesh(Scene*, Mesh*);
void scene_render(const Scene*, Camera*);
void scene_fini(Scene*);

typedef struct {
    QuadMaterial *material;

    unsigned int vao;
    unsigned int vbo;
    unsigned int ebo;

    float width;
    float height;
} UI;

UI *ui_init();
void ui_update_size(UI*);
void ui_fill_rect(UI*, vec4, float, float, float, float);
void ui_round_rect(UI*, vec4, float, float, float, float, float);
void ui_draw_image(UI*, unsigned int, float, float, float, float);
void ui_draw_image_rounded(UI*, unsigned int, float, float, float, float, float);
void ui_fini(UI*);

typedef struct {
    GLFWwindow *window;
    Camera camera;
    Scene *scene;
    UI *ui;

    struct {
        int locked;
        vec2 last_pos;
    } mouse_state;

    int fullscreen;
    struct {
        int x, y, width, height;
    } windowed_rect;

    int last_noclip_key;
    int last_fullscreen_key;

    Game game;
    Player **me;
} Client;

void overlay_render(Client*);

const char *client_assets_path();
void client_animate_object_texture(Object*, float);

unsigned long long load_obj_model(char*);
unsigned long long load_texture(char*);

unsigned long long create_cube_model();
unsigned long long create_plane_model();
unsigned long long create_ramp_model();

Mesh *prefab_init(Object*, const vec4*, const cJSON*);
