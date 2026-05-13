#pragma once
#include <shared.h>
#include <GLFW/glfw3.h>
#include <cJSON.h>
#include <freetype/freetype.h>

#define NAME texture_cache_map
#define KEY_TY char*
#define VAL_TY unsigned int
#include <verstable.h>

typedef struct {
    unsigned int vao;
    unsigned int ebo;
    int index_count;
} Geometry;

#define NAME geometry_cache_map
#define KEY_TY char*
#define VAL_TY Geometry*
#include <verstable.h>

typedef struct {
    unsigned int texture;
    vec2 size;
    vec2 h_bearing;
    vec2 v_bearing;
    float advance;
} GlyphCacheEntry;

#define NAME glyph_cache_map
#define KEY_TY char
#define VAL_TY GlyphCacheEntry*
#include <verstable.h>

extern geometry_cache_map g_geometry_cache;
extern texture_cache_map g_texture_cache;

extern glyph_cache_map g_glyph_cache;
extern FT_Library g_freetype;
extern FT_Face g_game_font;

// low 4 bytes = VBO, high 4 bytes = EBO
extern Geometry *g_cube_geometry;
extern Geometry *g_plane_geometry;
extern Geometry *g_ramp_geometry;

extern unsigned int g_blank_texture;
extern unsigned int g_active_texture;
extern unsigned int g_active_shader;

typedef struct {
    float near;
    float far;
    float fov;
    float zoom;

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
    const MaterialVTable *vtable;
    unsigned char wireframe:1;
    unsigned char transparent:1;
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

    float r_clip;

    float texture_viewport[4];
    unsigned int texture;
} QuadMaterial;

QuadMaterial *quad_material_init();

typedef struct {
    Material base;
    vec4 color;

    unsigned int texture;
} TextMaterial;

TextMaterial *text_material_init();

typedef struct {
    Material base;

    int is_ramp;
    int is_ladder;
    int use_face_tex_scaling;
    vec3 face_scale;

    vec4 color;
    vec4 emissive;

    vec2 texture_repeat;
    vec2 texture_offset;
    unsigned int texture;
} BasicMaterial;

extern const MaterialVTable basic_material_vtable;
BasicMaterial *basic_material_init();

typedef enum {
    ROTATION_ORDER_INTRINSIC,
    ROTATION_ORDER_EXTRINSIC,
} RotationOrder;

typedef struct MeshTransform_t {
    struct MeshTransform_t *parent;

    RotationOrder rotation_order;

    vec3 position;
    vec3 rotation;
    vec3 scale;
} MeshTransform;

typedef struct {
    MeshTransform transform;

    int visible;
    Geometry *geometry;

    float transform_matrix[16];
    float _camera_space_matrix[16]; // temporary - used during scene depth sort

    Material *material;
} Mesh;

typedef struct {
    MeshTransform transform;

    size_t mesh_count;
    Mesh **meshes;
} ColorCube;

typedef struct {
    // third person
    Mesh *upper;
    Mesh *joint;

    // shared
    ColorCube *lower;
    MeshTransform anchor;
} PlayerArmMesh;

typedef struct {
    MeshTransform anchor;

    PlayerArmMesh *left;
    PlayerArmMesh *right;

    Mesh *weapon_left;
    Mesh *weapon_right;
} PlayerArms;

typedef struct {
    MeshTransform *anchor;
    MeshTransform *body_anchor;

    ColorCube *head;
    ColorCube *body;

    ColorCube *legs[2];
    void *legs_crouched[2];

    PlayerArms **arms;
} PlayerMesh;

void player_generate_meshes(Player*, int);

Mesh *mesh_init(Geometry*, Material*);
void mesh_update_transform_matrix(Mesh*);
void mesh_fini(Mesh*); // CAUTION: does NOT free underlying vertex/element buffers in case of re-use!

typedef struct {
    size_t mesh_count;
    Mesh **meshes;
} Scene;

Scene *scene_init();
void scene_add_player_mesh(Scene*, const PlayerMesh*, size_t);
void scene_remove_player_mesh(Scene*, const PlayerMesh*);
void scene_add_mesh(Scene*, Mesh*);
void scene_remove_mesh(Scene*, Mesh*);
void scene_render(const Scene*, Camera*);
void scene_fini(Scene*);

typedef struct {
    QuadMaterial *material;
    TextMaterial *text_material;

    unsigned int vao;
    unsigned int vbo;

    float width;
    float height;
    float scale;
} UI;

UI *ui_init();
void ui_update(UI*);
void ui_fill_rect(UI*, vec4, float, float, float, float);
void ui_round_rect(UI*, vec4, float, float, float, float, float);
void ui_fill_rect_rclip(UI*, vec4, float, float, float, float, float, float);
void ui_draw_image(UI*, unsigned int, float, float, float, float);
void ui_draw_image_rounded(UI*, unsigned int, float, float, float, float, float);
float ui_measure_text(UI*, const char*, float);
float ui_fill_text(UI*, vec4, const char*, float, float, float);
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

    struct {
        int x;
        int y;
        int width;
        int height;
    } windowed_rect;

    unsigned char last_noclip_key:1;
    unsigned char last_debug_key:1;
    unsigned char last_fullscreen_key:1;
    unsigned char last_swap_key:2;
    unsigned char in_game:1;

    Game game;
    Player *me;
} Client;

void hud_render(Client*, float);
void overlay_render(Client*, float);

const char *client_assets_path();
void client_animate_object_texture(Object*, float);

unsigned int load_texture(char*);
GlyphCacheEntry *load_glyph(char);
Geometry *load_obj_model(char*);

Geometry *create_cube_geo();
Geometry *create_plane_geo();
Geometry *create_ramp_geo();
Geometry *create_ladder_geo(float);

Mesh *prefab_init(Object*, const vec4*, const cJSON*);
