#pragma once
#include <shared.h>
#include <GLFW/glfw3.h>
#include <cJSON.h>
#include <freetype/freetype.h>
#include <pthread.h>

#ifdef _WIN32
// <windows.h> (minwindef.h) defines `near` and `far` as empty macros. In a translation unit
// that included it first (net.c, ...), Camera's near/far members vanished ("declaration does
// not declare anything"), so Camera and Client had a different layout there than everywhere
// else: the offline client crashed in client_net_queue_packet (pthread_mutex_lock on a
// garbage net_lock). A TU that includes <windows.h> after this header and then uses
// camera.near fails to compile instead of miscompiling.
#undef near
#undef far
#endif

#define NAME texture_cache_map
#define KEY_TY char*
#define VAL_TY unsigned int
#include <verstable.h>

typedef struct {
    unsigned int vao;
    unsigned int ebo;
    int index_count;
    // Local-space bounding sphere for scene_render()'s frustum culling (three.js
    // frustumCulled). bound_radius 0 = unknown: the geometry is always drawn.
    float bound_center[3];
    float bound_radius;
    // glDrawElements mode: 0 = GL_TRIANGLES; GL_LINES for line lists (LineMaterial: the
    // show_navmeshes overlay, nav_overlay.c). Lines are the GLES 3.0 way to draw wireframes.
    unsigned int draw_mode;
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

// Before drawing into a framebuffer that has `texture` attached: unbinds it (GL_TEXTURE_2D)
// from the renderer's texture units 0..GL_RENDER_TEXTURE_UNITS-1 (and from g_active_texture's
// unit-0 cache), so no sampler of the pass can read its own target. WebGL and ANGLE reject
// such a draw as a feedback loop even when the shader never samples that unit on its path
// (gl_feedback_check.h asserts it in debug builds). Keeps the active texture unit.
#define GL_RENDER_TEXTURE_UNITS 8
void gl_release_texture(unsigned int texture);

typedef struct {
    vec4 ambient_color;
    vec4 light_color;
    vec4 sky_color;
    vec4 fog_color;
    vec3 light_direction;
    float ambient_intensity;
    float light_intensity;
    float fog_near;
    float fog_far;
    unsigned char enabled:1;
    unsigned char fog_enabled:1;
    // map-G: Krunker v7.1.4 renderer output state (render.js init: toneMapping,
    // toneMappingExposure, outputEncoding, useLegacyLights = !correctLights), applied by
    // basic.frag after lighting and before fog like three.js r150. Zero = v3.1.1 output.
    unsigned char correct_lights:1;  // physical light units / falloff (three useLegacyLights false)
    int tone_mapping;                // 0 none, 1 Linear, 2 Reinhard, 3 Cineon, 4 ACESFilmic (config.toneTypes)
    float tone_mapping_exposure;     // renderer.toneMappingExposure (1)
    int output_encoding;             // 0/2 linear, 1 sRGB (config.encodingTypes)
} RenderLighting;

extern RenderLighting g_render_lighting;

typedef struct {
    float near;
    float far;
    float fov;
    float zoom;
    float ortho; // > 0: orthographic projection, half the view height in world units (editor renders); 0 = perspective
    float aspect_override; // map-G: > 0 replaces the viewport aspect (ocean mirror pass: the main camera's projection on a square target)

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

typedef struct Material Material;

// Each material's update_uniforms takes the Material base exactly and downcasts inside (every
// material struct starts with `Material base`): calling through a mismatched function type is UB
// (-fsanitize=function), so the vtable entries are never cast.
typedef struct {
    void (*update_uniforms)(Material*);
} MaterialVTable;

struct Material {
    const MaterialVTable *vtable;
    unsigned char wireframe:1;
    unsigned char transparent:1;
    unsigned int program;
};

unsigned int shader_compile(const char*, const char*);
void material_update_uniforms(Material*);
void material_fini(Material*);

// Cached uniform locations + last uploaded values (shader.c). glGetUniformLocation by
// name is a string decode + map lookup per call on the web (and a driver call natively),
// and most per-draw uniforms (lighting, fog, camera) repeat from draw to draw. A
// GlUniform is a call-site handle (static, GL_UNIFORM_INIT("name")); the value cache lives
// per (program, uniform name), so two call sites writing the same uniform stay coherent.
// Every write to a cached uniform must go through these helpers, with `program` bound.
typedef struct GlUniformEntry GlUniformEntry;
typedef struct {
    const char *name;
    unsigned int program;
    GlUniformEntry *entry;
} GlUniform;
#define GL_UNIFORM_INIT(n) {(n), 0, NULL} // not GL_UNIFORM: glad.h defines that enum (0x92E1)

int gl_uniform_location(GlUniform*, unsigned int program); // -1 when the program lacks it
void gl_uniform_1i(GlUniform*, unsigned int program, int);
void gl_uniform_1f(GlUniform*, unsigned int program, float);
void gl_uniform_2f(GlUniform*, unsigned int program, float, float);
void gl_uniform_3f(GlUniform*, unsigned int program, float, float, float);
void gl_uniform_4f(GlUniform*, unsigned int program, float, float, float, float);
void gl_uniform_mat3(GlUniform*, unsigned int program, int transpose, const float *m);
void gl_uniform_mat4(GlUniform*, unsigned int program, int transpose, const float *m);
void gl_uniform_forget_program(unsigned int program); // linked anew: locations and values are stale
unsigned int gl_uniform_epoch(void); // changes whenever a program's cached uniform values were forgotten

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

// Unlit single-colour material for debug lines and flat overlays (three.js LineBasicMaterial /
// MeshBasicMaterial): assets/shaders/line.vert/.frag, fogged like three.js (fog: true), no
// lighting, no texture. Draw lines with a Geometry whose draw_mode is GL_LINES. depth_test 0 is
// three.js depthTest: false (drawn over the scene; depth writes are off for every LineMaterial).
typedef struct {
    Material base;
    vec4 color;       // rgb + opacity (transparent materials blend)
    int fog;          // three.js material.fog
    int depth_test;
} LineMaterial;

LineMaterial *line_material_init(void); // NULL when the shader is unavailable
void line_material_fini_program(void);  // the shared program (renderer shutdown)

// How BasicMaterial texture coordinates are produced (basic.frag).
typedef enum {
    BASIC_UV_MESH,  // the geometry's vertex UVs as-is (models, ladders, cylinders, ...)
    BASIC_UV_CUBE,  // Krunker generateCube (31.js): per-face UVs from the unit-cube position
    BASIC_UV_PLANE, // Krunker generatePlane (31.js): UVs from the XY unit-plane position
} BasicUvMode;

typedef struct {
    Material base;

    int unlit;
    int ambient_enabled;
    int fog_enabled;
    int use_flat_normal;
    vec3 flat_normal;

    vec4 color;
    vec4 emissive;

    // Krunker map-object texturing (prefab.c). uv_size is the texture repeat count along
    // each axis (size / worldUV * texture scale): cube (x, y, z), plane (u, v, -). The
    // flips are 31.js flipp (side faces) / flippW (top/bottom) for floor_0 on wide cubes.
    BasicUvMode uv_mode;
    vec3 uv_size;
    int uv_flip_sides;
    int uv_flip_top;
    // Cube vertex-colour "ambient" gradient (31.js generateCube amb): side faces fade from
    // color at the top to color_bottom at the bottom, the bottom face is color_bottom.
    int use_color_gradient;
    vec4 color_bottom;
    // three.js vertexColors: color is multiplied by the geometry's per-vertex colour
    // (attribute 2, vec3; only ladder geometry has one, prefab.c)
    int use_vertex_color;
    int double_sided;   // 0: back faces culled (three.js FrontSide)
    float alpha_test;   // fragments with alpha below this are discarded (0: off)
    int additive_blend; // three.js AdditiveBlending (SRC_ALPHA, ONE), e.g. light cones
    int no_depth_write; // three.js depthWrite: false
    float ramp_shade_width; // ramp width for the ambient_1 edge strips (basic.frag), 0 = off
    int ambient_group;      // ambient.c AO planes: hidden while the ambientShading setting is off
    // Uploaded only when non-zero (basic.c): the emissive texture (three.js emissiveMap,
    // "<model>_e.png", bound to texture unit 2) of glowing models.
    unsigned int emissive_texture;
    // Krunker liquids (55.js render): local z = height * sin(vertex / 5 + liquidVal * speed / 4)
    // on a tilesX x tilesZ grid (basic.vert); 0 = flat. Gated by the objectAnim setting.
    float wave_height;
    float wave_speed;

    vec2 texture_repeat;
    vec2 texture_offset;      // in repeat units (frame index / scroll progress, main.c)
    vec2 texture_base_offset; // texture.offset from the map (tox/toy), added as-is
    unsigned int texture;

    // map-G: Krunker v7.1.4 map material inputs (prefab.c)
    float texture_rotation;   // three.js texture.rotation in radians ("tro" degrees), about uv (0, 0)
    int texture_srgb;         // "ten" 1: the texture is sRGBEncoding, decoded to linear when sampled
    int cull_front;           // three.js BackSide ("rs" 1): front faces culled (double_sided must be 0)
    int sprite;               // THREE.Sprite / "msp": the XY unit quad always faces the camera (basic.vert)
    unsigned int light_map;   // three.js lightMap (user asset "l" flag, "<asset>_l"), read with the mesh UVs
    float light_map_intensity; // lightMapIntensity (1.5)

    // WP-F: v7.1.4 store-item material effects (skin_fx.c)
    unsigned int env_map;     // envMap "<tex>_env" (EquirectangularReflectionMapping), texture unit 4, 0 = none
    float env_map_intensity;  // envMapIntensity (skin env || 3)
    int projection_uv;        // skin `projection`: screen-space texture coordinates (render.js vTexCoords)
} BasicMaterial;

extern const MaterialVTable basic_material_vtable;
BasicMaterial *basic_material_init();

// Texture unit of BasicMaterial.emissive_texture (unit 0: base map, 1: shadow map).
#define BASIC_EMISSIVE_TEXTURE_UNIT 2
#define BASIC_LIGHT_MAP_TEXTURE_UNIT 3 // BasicMaterial.light_map (map-G)
#define BASIC_ENV_MAP_TEXTURE_UNIT 4   // BasicMaterial.env_map (WP-F skin env)
#define BASIC_MAX_POINT_LIGHTS 16 // basic.frag MAX_POINT_LIGHTS

// v3.1.1 POINT_LIGHT objects (94.js addPointLight -> THREE.PointLight), lit only with the
// lighting setting at 2. Set per map by map_post.c; `color` is colour * intensity.
typedef struct {
    vec3 position;
    vec3 color;
    float distance;
    float decay;
} BasicPointLight;

void basic_material_set_point_lights(const BasicPointLight *lights, int count);

// map-G: the forward light list of basic.frag (Krunker v7.1.4 map lights, KrunkScript SCENE
// lights): three.js r150 PointLight / SpotLight / DirectionalLight / RectAreaLight. Which
// material types see which lights follows three.js: Lambert and Phong take point, spot and
// directional lights, only Standard (lighting 3) takes rect-area lights; the v7 lighting-setting
// gates of who may add a light (render.js addPointLight: lighting >= 2, ...) stay with the caller.
// Colours are the hex channels / 255 (no sRGB decode: three r150 legacy colour management).
// Rect-area lights only light diffuse (three's LTC form factor; no LTC specular, documented).
#define BASIC_HAVE_LIGHT_LIST 1
#define BASIC_MAX_LIGHTS 16 // basic.frag MAX_LIGHTS (point lights of basic_material_set_point_lights included)
typedef enum {
    BASIC_LIGHT_POINT,
    BASIC_LIGHT_SPOT,
    BASIC_LIGHT_DIRECTIONAL,
    BASIC_LIGHT_RECT,
} BasicLightType;

typedef struct {
    BasicLightType type;
    int visible;
    vec3 color;
    float intensity;
    float distance;   // point / spot cutoff (0 = infinite)
    float decay;      // point / spot
    float angle;      // spot cone half angle, radians
    float penumbra;   // spot 0..1
    vec3 position;
    vec3 target;      // spot / directional: aimed from position at target
    float width, height; // rect full size
    vec3 rotation;    // rect: rotateY(rotation.y) rotateX(rotation.x) rotateZ(rotation.z) (render.js addRectAreaLight)
} BasicLight;

int basic_lights_add(const BasicLight *light);              // handle > 0, 0 = list full
// map-G: a world-space clip plane for every BasicMaterial draw (the ocean mirror pass: three.js
// Water's oblique near plane), fragments with dot(n, p) + d < 0 are discarded; NULL = off.
void basic_material_set_clip_plane(const float plane[4]);
void basic_lights_update(int handle, const BasicLight *light);
// WP-F (the viewer page): while count >= 0, draws take exactly `lights` (copied, up to
// BASIC_MAX_LIGHTS) instead of the registered map lights; count < 0 goes back to the list.
void basic_lights_override(const BasicLight *lights, int count);
void basic_lights_remove(int handle);

typedef enum {
    ROTATION_ORDER_INTRINSIC,
    ROTATION_ORDER_EXTRINSIC,
    ROTATION_ORDER_YXZ, // three.js Euler "YXZ": Ry * Rx * Rz (Krunker planes and ramps)
} RotationOrder;

typedef struct MeshTransform_t {
    struct MeshTransform_t *parent;

    RotationOrder rotation_order;

    vec3 position;
    vec3 rotation;
    vec3 scale;
} MeshTransform;

// What mesh_update_transform_matrix() last computed transform_matrix from: one node of the
// transform chain, by address and value. A chain with every node unchanged keeps its matrix
// (map geometry never moves, so scene_render stops rebuilding thousands of matrices a frame).
#define MESH_XF_CACHE_DEPTH 8
typedef struct {
    const MeshTransform *node;
    const MeshTransform *parent;
    RotationOrder rotation_order;
    vec3 position, rotation, scale;
} MeshTransformSnapshot;

typedef struct {
    MeshTransform transform;

    int visible;
    Geometry *geometry;

    float transform_matrix[16];
    float _camera_space_matrix[16]; // temporary - used during scene depth sort
    // mesh_update_transform_matrix's cache: _xf_cache_depth nodes (0 = none: zeroed meshes recompute)
    int _xf_cache_depth;
    MeshTransformSnapshot _xf_cache[MESH_XF_CACHE_DEPTH];

    Material *material;

    // Krunker receiveShadow, inverted so calloc'd meshes (players, props) receive by
    // default; map objects set it from their prefab in shadow.c.
    unsigned char no_shadow_receive:1;
} Mesh;

typedef struct {
    MeshTransform transform;

    size_t mesh_count;
    Mesh **meshes;
} ColorCube;

// playerMesh.js genCube / render.js genColorCube: a width x height x length box of stacked colour
// segments (heights in units of `height`) stacking down from top = 0.5 + yOffset (genColorCube's
// offset); a segment of height 0 is skipped (v7 segment[5]: noHair, noBody, legLen). player_mesh.c
typedef struct {
    int color;
    float height;
    float scale;
} ColorCubeSegment;
ColorCube *generate_color_cube_at(float width, float height, float length, const ColorCubeSegment *segments, size_t segment_count, float top);
void color_cube_fini(ColorCube *cube); // its meshes (and their skin effects), the cube
vec4 krunker_get_color(int hex); // geos.js getColor(colr): the colour with its HSL saturation x1.5 (player_mesh.c)

typedef struct {
    MeshTransform anchor;

    Mesh *upper;
    Mesh *joint;

    ColorCube *lower;
} PlayerCrouchedLeg;

typedef struct {
    // first person
    Mesh *extender;

    // third person
    Mesh *upper;
    Mesh *joint;

    // shared
    ColorCube *lower;
    MeshTransform anchor; // genArm placement (child of pivot)
    MeshTransform pivot;  // Krunker's per-arm mesh transform (armMeshes[i]): first-person melee pose; identity otherwise
} PlayerArmMesh;

typedef struct {
    MeshTransform anchor;

    PlayerArmMesh *left;
    PlayerArmMesh *right;

    Mesh *weapon_left;
    Mesh *weapon_right;
    Mesh *attachment; // sight attachment (Weapon.has_attach), first-person / preview models only
    Mesh *hose;        // KrunkNative Piss: the hose (= weapon_right, player_hose_mesh), NULL for every other weapon
    float hose_length; // its length (WeaponStream.hose_length): the tip is (0, 0, -hose_length / 2) in it
    // v7 hasAnim weapon / melee skins: the GLTF remodel (gltf.h) of weapon_right [0] / weapon_left [1],
    // parented to that mesh, which is then an empty carrier (its pose and visibility drive the model)
    struct GltfInstance *weapon_models[2];

    int melee_pose;   // first-person melee: arms are posed per arm (pivots) and the melee mesh hangs off the right pivot
    vec3 melee_position; // first-person melee mesh hold (weaponGeos xP/yP/zP incl. the skin's sxOff/syOff/szOff)
    vec3 melee_rotation; // its rotation (yR/zR incl. the skin's flip and sxRot)
    // ac.muzzles[i] (local player): sprite positions in the anchor. v10 builds them at the barrel,
    // (0, muzOffY, -muzOff), with the weapon mesh; a muzzle flash init()s them again (the akimbo x).
    // vfx.c local_shot places them on the first shot (muzzle_placed) and starts the tracer there,
    // muzzleFlash on or off (v3.1.1 left them at the anchor origin until a flash).
    vec3 muzzle[2];
    int muzzle_placed;
    // v7.1.4 (WP-F): the weapon's charm (players.js:1262-1291): charms/charm_mount_0 at the weapon's
    // charm mount, the charm item inside it (swaying, player_update_meshes); NULL when none
    Mesh *charm_mount;
    Mesh *charm;
    // the wrist item (type 11, players.js:1082-1089 / 1127-1130): first-person / own model only,
    // on the left arm; wrist (OBJ) or wrist_model (hasAnim GLTF) inside wrist_holder
    MeshTransform wrist_holder;
    Mesh *wrist;
    struct GltfInstance *wrist_model;
    // What player_generate_meshes picked for this slot, model-set independent (the weapon, its store
    // skin as worn after fallbacks, model / texture keys or the procedural hose, the tint, the sight,
    // the charm, the wrist item): equal for the menu preview and the viewmodel of the same loadout
    // (the input script's `wsel` probe, server/tests/ui_transitions_e2e.py loadout-images)
    char selection[256];
} PlayerArms;

typedef struct {
    MeshTransform *anchor;
    MeshTransform *body_anchor;
    MeshTransform *upper_body_anchor;

    ColorCube *head;
    ColorCube *body;

    ColorCube *legs[2];
    PlayerCrouchedLeg *crouched_legs[2];

    PlayerArms **arms;

    // v7.1.4 cosmetic items on the third-person model (players.js generateMeshes, PLAYER_COSMETIC_*):
    // each item hangs off its genObj3D holder (hatMesh, headMesh, ..., shoeMeshes[0..1]); an OBJ
    // model in cosmetics[slot] or a hasAnim GLTF in cosmetic_models[slot], NULL when unused
    Mesh *cosmetics[8];
    struct GltfInstance *cosmetic_models[8];
    MeshTransform cosmetic_holders[8];
    unsigned char shoe_hidden[2]; // randomLeg: the shoe of the other leg is hidden
    float head_mlt;               // the headMlt the head / face / hat / head holders are scaled for
    int render_you;               // built as the local first-person model (the pet goes to the world scene)
    int kn_third_person;          // the local model built for the KrunkNative third-person camera (third_person.h)

    // v7.1.4 pet (pets.h): the follower of petIndex, in world space, toward pet_holder (petHolder:
    // (3, player.height, 0) on the anchor); NULL when none
    struct PlayerPet *pet;
    MeshTransform pet_holder;
    // Prop Hunt: the prop model a Prop-team player wears instead of its body (players.js :171319:
    // "<prefab name>_0" at y scale + CPY, scaled by the prefab's scale), NULL otherwise
    Mesh *prop;
    double pet_clock;             // gltf_clock() of the last pet / item-effect update, < 0 before the first
    float effect_timer;           // players.js itemEffects effectTimer (ms)
} PlayerMesh;

enum {
    PLAYER_COSMETIC_HAT, PLAYER_COSMETIC_BODY, PLAYER_COSMETIC_WAIST, PLAYER_COSMETIC_FACE,
    PLAYER_COSMETIC_HEAD, PLAYER_COSMETIC_BACK, PLAYER_COSMETIC_SHOE_R, PLAYER_COSMETIC_SHOE_L, PLAYER_COSMETIC_SLOTS
};

void player_generate_meshes(Player*, int);
// Prop Hunt (player_mesh.c): the player wears a prop (a prop mode, the Prop team, a valid propID).
int prop_hunt_player(const Player*);
void player_update_meshes(Player*, int);
void player_meshes_fini(Player*);
void player_mesh_debug_log(Player*, double now); // KRUNKNATIVE_PLAYER_LOG (player_mesh.c)
// KrunkNative Piss: world points of the held weapon's hose (the first-person one in the trigger hand,
// else the pelvis one): its tip (hoseTip, where the spray leaves), its back end and the trigger hand's
// centre; any may be NULL. 0 when the held weapon has no hose.
int player_hose_points(const Player*, vec3 *tip, vec3 *back, vec3 *hand);
// Nettspend Purple Drank (nettspend.h): the pour-up's bottle mouth and cup rim (world) while the
// local player reloads it (first or third person); 0 otherwise.
int player_pour_points(const Player*, vec3 *mouth, vec3 *rim);
// The held slot's hands (world, first or third person): the trigger hand's and the off hand's centres,
// and where the Drank's bottle is held (`bottle`, its grip; `bottle_visible` 0 when hidden), and
// whether the off arm is drawn (`off_visible`); 0 without arms. Any may be NULL.
int player_hands_points(const Player*, vec3 *trigger, vec3 *off, vec3 *bottle, int *bottle_visible, int *off_visible);

Mesh *mesh_init(Geometry*, Material*);
void mesh_update_transform_matrix(Mesh*);
// `local` in `node`'s frame, carried up `node`'s parent chain into `root`'s frame (`root` an ancestor of
// `node`, excluded; NULL: up to the world)
vec3 transform_point_to(const MeshTransform *node, vec3 local, const MeshTransform *root);
void mesh_fini(Mesh*); // CAUTION: does NOT free underlying vertex/element buffers in case of re-use!

typedef struct {
    size_t mesh_count;
    Mesh **meshes;
} Scene;

Scene *scene_init();
void scene_add_player_mesh(Scene*, const PlayerMesh*, size_t);
void scene_remove_player_mesh(Scene*, const PlayerMesh*, size_t);
void scene_add_mesh(Scene*, Mesh*);
void scene_remove_mesh(Scene*, Mesh*);
void scene_render(const Scene*, Camera*);
// Called by a material's update_uniforms() inside scene_render(): the draw's face culling
// (back faces, CCW front), additive blending and depth writes. Unset values keep the
// per-draw defaults (no culling, normal alpha blending, depth writes on); scene_render()
// only issues the GL calls for state that differs from the previous draw.
void scene_draw_state(int cull_back, int additive_blend, int depth_write);
// Called the same way: this draw ignores the depth buffer (three.js depthTest: false).
void scene_draw_no_depth_test(void);
void scene_set_alpha_target(int on); // class_preview.c: blended draws keep a premultiplied destination alpha (three.js alpha canvas)
void scene_draw_cull_front(void); // map-G: after scene_draw_state(1, ...): three.js BackSide (cull front faces)
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

// HTML HUD mirror (client/src/ui/hud_ui.c): the values last written to the page's
// #inGameUI, used to throttle (20 Hz) and to write only what changed.
typedef struct {
    int valid;               // the fields below describe what the page currently shows
    int was_ready;           // html_ui_is_ready() of the previous tick (fresh page -> rewrite)
    double next_emit_time;   // no HUD writes before this time (seconds, client clock)
    double fps_window_start; // last #ingameFPS update (hud_sample_fps, every 100 ms)
    int fps;                 // frames per second of the last completed window
    int health, max_health, health_segments;
    int ammo, ammo_max, ammo_limit, melee, reload_percent;
    int kills, deaths, score;
    int class_index, scoped;
    int aim_dot; // #aimDot reticle: fully aimed through a sight attachment (95.js toggleAim)
    const void *weapon;      // Weapon identity (name/icon follow from it)
    char timer_text[16];     // "MM:SS"
} HtmlHudState;

// Selected-class preview drawn into the HTML menu's preview box (client/src/ui/class_preview.c)
typedef struct {
    int has_rect;         // the page (or the test hook) reported a rectangle
    int visible;          // page says its preview box is visible
    float x, y, width, height; // CSS px from the viewport top-left
    float dpr;            // devicePixelRatio reported by the page

    Scene *scene;         // preview-only scene (never the world scene)
    Camera camera;
    Player *player;       // private third-person player, not in game.players
    int class_index;      // class the meshes were generated for, -1 when none
    const void *map;      // game.map the meshes were generated against
    int step_dir;         // Krunker stepDir for the in-place walk cycle
    double last_time;     // glfwGetTime() of the previous render

    // Render target the page's #classPreviewCanvas shows (html_dom_canvas_gl)
    unsigned int fbo, color_tex, depth_rb;
    int target_width, target_height; // allocated size (device px), 0 = none
    int shown;            // the canvas shows color_tex (a frame was rendered)
} ClassPreview;

// A server projectile mirrored on the client (Krunker "pr" / "pre").
typedef struct {
    int active;
    int owner_uid;
    int weapon_index;   // 12 = thrown knife
    vec3 position;
    vec3 velocity;      // units per ms
    float gravity;      // units per ms^2
    float range;        // distance left before it expires
} ClientProjectile;

// Connection status handed from the transport (native TCP thread, web
// callbacks, local server) to the main thread: client_menu_tick() consumes it
// under net_lock, updates net_connected and the page's connection state.
typedef enum {
    NET_STATUS_NONE = 0,
    NET_STATUS_CONNECTED,
    NET_STATUS_FAILED, // connect attempt failed or the session dropped; see net_status_error (final: see net_status_final)
    NET_STATUS_CONNECTING, // a switch closed the session on purpose, or a retry is under way: the page's loading spinner
} NetStatus;

// Retry budget of the transports (net.c, net_web.c, net_wt.c): this many failed connect attempts /
// unstable sessions in a row and the client gives up. A give-up is a final NET_STATUS_FAILED ("Disconnected:
// gave up after N attempts"), the page shows the disconnected screen with Retry (client_net_request_reconnect).
// Until then every failure is NET_STATUS_FAILED but not final: the page keeps its loading spinner.
#define NET_RETRY_BUDGET 5

typedef struct {
    GLFWwindow *window;
    Camera camera;
    Scene *scene;
    Scene *fps_scene;
    UI *ui;
    ClassPreview class_preview;

    struct {
        int locked;
        vec2 last_pos;
        int scroll_delta;
    } mouse_state;

    struct {
        int x;
        int y;
        int width;
        int height;
    } windowed_rect; // the rect windowed mode returns to (tracked while windowed and not maximized)
    int windowed_maximized; // windowed mode is maximized by the window manager (window_state.h)

    unsigned char last_debug_key:1;
    unsigned char last_swap_key:2;
    unsigned char in_game:1;
    unsigned char spawn_requested:1;
    unsigned char map_loaded:1;
    unsigned char net_connected:1;
    unsigned char net_thread_running:1;
    // Offline (client/src/net/offline.c): connected to the in-process sandbox server
    // (server/include/server_sandbox.h) over its loopback transport; `game` is a normal client game.
    unsigned char offline:1;

    pthread_t net_thread;
    pthread_mutex_t net_lock;
    int net_socket;
    int net_should_quit;
    int net_retry_now;             // page asked for an immediate reconnect; the net thread skips its retry sleep
    int net_status;                // NetStatus pending for the main thread (under net_lock)
    char net_status_error[160];    // message for NET_STATUS_FAILED (under net_lock)
    char net_disconnect_reason[96];// server ERROR text for the coming disconnect (under net_lock), "" none
    char net_disconnect_code[8];   // its Krunker error code ("000005" idle kick), under net_lock
    char net_status_code[8];       // error code for NET_STATUS_FAILED (under net_lock): "000005" kick, "000001" connect error, "000000" drop
    int net_hold_retry;            // the server closed with an ERROR: no automatic reconnect until `reconnect`
    // A session the transport lost without a server ERROR (client_net_set_drop, under net_lock): its
    // own account of why, for the error screen ("Disconnected: <reason>", v7's Code / Reason line).
    char net_drop_reason[112];     // "" = none yet for this session (the first explanation wins)
    char net_drop_tag[24];         // short cause: timeout, ice, gateway, offline, reset, closed, ...
    int net_drop_close_code;       // the WebSocket close code (web), 0 otherwise
    // NET_STATUS_FAILED details (under net_lock) for menu_poll_connection: a live session dropped
    // (not a kick / server stop / connect failure: servers_ui.c rejoins), v7's "Code:" / "Reason:".
    int net_status_dropped;
    int net_status_gave_up;        // the NET_STATUS_FAILED is the retry budget's give-up (client_net_give_up): the page's disconnected screen
    int net_close_intended;        // a server switch / quit closes the session on purpose: not a drop (no rejoin)
    int net_status_close_code;
    char net_status_close_reason[24];
    int net_session_connected;     // transport-private: a session was established (disconnect vs. failed connect)
    unsigned net_session_seq;      // under net_lock: bumped by every session that connects (a new game's INIT leaves the old game)
    char net_address[64];          // server address/port, for net_main() and error messages
    char net_ws_url[512];          // web: game WebSocket URL override (brain seek webUrl / /native?target=), "" = default (net_web.c)
    NetJoinPacket net_join;        // sent right after HELLO (net.c / net_web.c, under net_lock); all empty = guest (backend_ui.h)
    unsigned short net_port;
    int net_map_index;
    int net_mode_index;
    int net_next_input_seq;
    // Prediction / reconciliation (client/src/net/recon.c): CONTROLS.tmpInpts (sent, not yet
    // acked inputs, seq ascending) and the "q" batch of the current clientSendRate window.
    Input *net_pending_inputs;
    size_t net_pending_count, net_pending_capacity;
    NetInputPacket net_batch[NET_INPUT_BATCH_MAX];
    int net_batch_count;
    float net_batch_time;          // frame time (s) summed into the current "q" window (iiïïiïï)
    // official lobbies: this frame's input is in the window but its tick has not run yet, so the window
    // closes after it (recon.c client_recon_after_tick): its fire count before the tick
    int net_frame_open;
    unsigned int net_frame_fires;
    unsigned int net_recon_count;  // RECON packets applied (tests, KRUNKNATIVE_RECON_LOG)
    // How far reconciliation moved our predicted player (recon.c): every RECON snaps and replays; a
    // correction is one that moved it more than NET_RECON_CORRECTION_EPS (the "[krunker] sync" trace, krunker_net.c)
    unsigned int net_recon_corrections;
    float net_recon_error_peak;      // the largest since the last "[krunker] sync" trace line took it (krunker_net.c)

    // Live match state from the server (client/src/net/game_events.c). Read by the HUD.
    int net_ping_ms;               // smoothed RTT from PING_RESULT, -1 = unknown (Krunker "-")
    uint16_t net_ent_size;         // ENT size the server's INIT protocol accepts (net_ent_size_for): V7 from 1.2, V6 for 1.1, else V4
    uint8_t net_server_minor;      // WP-B: INIT minor (NET_PROTOCOL_MINOR_V7 servers take the v7 combat packets)
    int net_client_id;             // INIT client_id: this connection's id (v7 player.id, the comp lobby key)
    uint8_t net_server_patch;      // WP-C: INIT patch (net_peer_has_wpc: the server reads ENT V8)
    float net_input_accum;         // WP-C: frame time (s) since the last sampled input frame (index.js inputFrameDelta, netRate)
    int net_round_seconds;         // round clock (TIMER), -1 = no round clock received yet
    int net_end_seconds;           // end screen "Next Round in" seconds, -1 = no end screen
    int net_end_result;            // END result: 1 victory, 0 defeat, -1 draw
    int net_end_rank;              // END: the local player's place on the board (1 first), 0 not on it
    int net_damage_from_valid;     // the last HEALTH for `me` carried a dealer position
    float net_damage_from_x;       // dealer x/z of that hit (Krunker "h" packet args 3/4)
    float net_damage_from_z;
    double net_damage_from_time;   // glfwGetTime() of that hit
    double net_death_time;         // client_death_clock() of the local player's death, 0 = alive
    float net_respawn_delay;       // seconds after net_death_time before the menu returns
    int net_death_menu_pending;    // reopen the menu once the death delay ran out (Krunker cE())
    // Server projectiles in flight (PROJECTILE / PROJECTILE_END), dead-reckoned per frame by
    // client_game_events_tick() for rendering (vfx.c); slot = projectile id.
    ClientProjectile net_projectiles[GAME_MAX_PROJECTILES];
    double net_projectiles_time;   // glfwGetTime() of the last dead-reckoning step

    // Map resolution/streaming (MAP_INFO / MAP_REQUEST / MAP_DATA, net/game_events.c).
    NetMapInfoPacket net_map_info; // last MAP_INFO (valid while net_map_info_valid)
    int net_map_info_valid;
    int net_map_wait;              // downloading net_map_info's map; packets are held
    double net_map_wait_since;     // glfwGetTime() of the MAP_REQUEST (gives up after a while)
    int net_map_ready;             // download complete: client_map_take_ready() applies net_map_init
    NetInitPacket net_map_init;    // the INIT waiting for the download
    char *net_map_data;            // assembly buffer (net_map_info.size bytes)
    uint32_t net_map_received;     // bytes received
    NetPacket *net_map_held;       // packets after that INIT, replayed once the map is in
    size_t net_map_held_count;
    cJSON **net_map_cache;         // maps received from servers (owned, kept for later rounds)
    size_t net_map_cache_count;
    // the custom game's id (Krunker INIT arg 9 .id, v10 customMapData.id): GAME.COOKIES' key; "" none.
    // Set per round by client_apply_init from the map the INIT names (client_custom_game_id_of).
    char custom_game_id[48];
    struct { const cJSON *map; char id[48]; } *custom_games; // the official custom maps and their ids
    size_t custom_game_count, custom_game_cap;
    double last_frame_time;        // glfwGetTime() of the previous client_frame()

    size_t net_packet_count;
    NetPacket *net_packets;

    // HTML menu state (client/src/ui/menu_ui.c)
    int menu_class_index;
    char menu_name[32];
    double menu_lock_grace_until;
    int menu_connect_sent;         // last connection state shown by the page: -1 none, 0 disconnected, 1 connected
    char menu_connect_error[160];  // error text of the last connection error shown
    double menu_connect_error_time;// glfwGetTime() of that update (repeat throttle)
    int menu_play_pending;         // `play` arrived before the map was ready; entered once game.ready
    HtmlHudState hud_html; // HTML HUD state (client/src/ui/hud_ui.c)

    Game game;
    const cJSON *startup_map;
    // Host Game / Games-window Play: the map (one of game.maps) the next local_game_start
    // begins on instead of the startup / a random map; consumed by that start. NULL = none.
    const cJSON *offline_start_map;
    Player *me;
} Client;

void hud_render(Client*, float);
void overlay_render(Client*, float);

void client_enter_game(Client*);
void client_clear_players(Client*); // drops every player (and mesh) of the current session; resets in_game
void client_menu_bind(Client*);
void client_menu_tick(Client*);
void client_set_menu_open(Client*, int);
// The pointer-lock owner (client/src/ui/menu_ui.c over client/include/pointer_lock.h): decides and
// applies the lock from the menu / end screen / console / focus / KrunkScript state. Call it after
// any of them changes (client_set_menu_open does); main.c runs it once a frame too.
void client_pointer_update(Client*);
int  client_pointer_look_allowed(const Client*); // the mouse may turn the camera: controls on and really locked
// main.c: mouseFlick / flickClamp / rawMouse changed (ui_settings.c): the look filter and raw input follow
void client_mouse_settings_changed(void);
int  client_pointer_click(Client*);              // a press on the game view with the lock off: lock again (1 = taken)
void client_pointer_focus(Client*, int focused); // native window focus changes
int  client_pointer_play_wanted(const Client*);   // the player is in play (the window's focus left out): enter the game
void client_pointer_end_free(Client*);           // the lock went during the end title: free until a click
void client_pointer_refresh(Client*);            // the window changed monitor / size (F11): apply the lock again
void client_window_focus_lost(Client*); // native alt-tab: resetKeys + menu, as on pointer lock loss (main.c)
void client_window_focus_changed(Client*, int focused); // the window's keyboard focus (GLFW callback, input script blur / focus)
void client_hud_html_tick(Client*, float); // HTML HUD (client/src/ui/hud_ui.c)

// Class preview (client/src/ui/class_preview.c). Rect is in CSS px (menu_send_preview_rect in menu_ui.c).
void client_class_preview_set_rect(Client*, int, float, float, float, float, float);
void client_class_preview_render(Client*); // renders the canvas's frame; before html_ui_render()
void client_class_preview_fini(Client*);
void client_class_preview_turn(Client*, float dx); // v10 #charPreviewSlot drag: xDire += 0.008 * dx (CSS px)

void on_client_socket_connect(Client*);
void on_client_socket_disconnect(Client*);
void on_client_socket_error(Client*, int);
void on_client_socket_failed(Client*, const char*); // connect attempt failed with this message
// The transport's reason for the session it is about to report lost (before
// on_client_socket_disconnect; any thread). tag: a short cause for the error screen's "Reason:"
// (timeout, ice, gateway, offline, reset, closed), close_code: the WebSocket close code or 0. The
// first call per session wins (the most specific cause is reported first).
void client_net_set_drop(Client*, const char *tag, int close_code, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
void client_net_request_reconnect(Client*);        // page `reconnect`: retry now instead of after the backoff
// The retry budget is spent (any transport, any thread): hold the transport (only the page's reconnect
// tries again) and report a final NET_STATUS_FAILED "Disconnected: gave up after <attempts> attempts".
void client_net_give_up(Client*, int attempts);
// Krunker disconnect(reason, code): close the remote session for good (no auto-retry until
// client_net_request_reconnect); NET_STATUS_FAILED carries reason/code. No-op without a session.
#define CLIENT_HAVE_NET_DISCONNECT 1
void client_net_disconnect(Client*, const char *reason, const char *code);
// Console `disconnect`: close the session (or stop retrying) and leave the transport idle
// until client_net_switch_server; NET_STATUS_FAILED carries `reason` (net.c / net_web.c).
void client_net_stop(Client*, const char *reason);
void on_client_socket_event(Client*);

typedef struct {
    Client *client;
    const char *address;
} NetMainArgs;

void *net_main(void*); // native TCP thread entry (net.c)
void net_socket_shutdown(int socket); // net.c: wake the TCP thread with a closed session
// Native UDP transport (net/net_udp.c): net_main() tries it before TCP.
struct sockaddr_in;
int net_udp_client_session(Client*, const struct sockaddr_in *address); // 1 ran, 0 no UDP answer, -1 refused
int net_udp_client_mode(void);              // KRUNKNATIVE_NET_TRANSPORT: 0 auto, 1 udp, -1 tcp
int net_udp_client_is_virtual(int socket);
void net_udp_client_request_close(void);
void net_udp_client_info(char *out, size_t size); // netinfo: the knet session's counters ("" without one)
// Brain matchmaking (servers_ui.c): connect to client->net_address:net_port (web: net_ws_url)
// with client->net_join, replacing the current session (starts the transport when idle,
// leaves an offline game). net.c natively, net_web.c on the web.
void client_net_switch_server(Client*);
// KN_KRUNKER_OFFICIAL (krunker_net.h) takes the connection: our transport stops (an intended
// switch: no error screen, no reconnect) and an offline game is left. net.c / net_web.c.
void client_net_release(Client*);
void client_net_send_join(Client*, int socket); // NET_PACKET_JOIN right after HELLO (net_join.c)
#ifdef __EMSCRIPTEN__
int net_web_connect(Client*); // WebSocket transport (net_web.c)
void net_web_target_url(char *out, size_t size, const char *host, int port); // bridge URL for host:port
void net_web_disconnect(Client*);
void net_web_request_reconnect(Client*);
// knet over a browser datagram transport (net_wt.c), tried by net_web_connect before the
// WebSocket: WebRTC DataChannel (net_rtc.c) first, then WebTransport (net_wt.c).
typedef struct {
    const char *name;                            // "webrtc" / "webtransport" (logs, stats)
    int (*wanted)(void);                         // configured for this page and supported by the browser
    void (*open)(const char *target, int gen);   // async; reports through kn_dg_on_open/_datagram/_close/_fail(gen)
    void (*send)(const uint8_t *data, int length);
    void (*close)(void);
    void (*describe)(char *out, size_t size); // netinfo lines about the browser session (may be NULL)
} NetDgramBackend;
extern const NetDgramBackend net_rtc_backend;
int net_wt_connect(Client*);
int net_wt_active(void);
int net_wt_busy(void);
int net_wt_send(NetPacketType, const void*, uint16_t);
void net_wt_close(int reconnect);
const char *net_wt_transport_label(void); // "WebRTC" / "WebTransport" while a session is up, else NULL
void net_wt_info(char *out, size_t size); // netinfo lines: knet session, ICE path, per-transport retry state
#endif
// Transport notes (net_notes.c): printed at once and queued for the in-game console
// (ui_console_tick drains them on the main thread), so the net thread may call it natively.
enum { NET_NOTE_INFO, NET_NOTE_WARN, NET_NOTE_ERROR };
void client_net_note(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int client_net_note_next(int *level, char *out, size_t size); // 1 = one line taken (main thread)
const char *client_net_transport_name(const Client*);         // "WebRTC", "WebSocket", "UDP", "TCP", "offline", "-"
void client_net_info(const Client*, char *out, size_t size);  // the console's `netinfo`
void client_net_queue_packet(Client*, const NetPacket*);
// WP-E: v7 io-client packet tally (settings showNetStat: #ingressPacketCount, #egressPacketCount,
// #tickPacketCount). Every received / sent frame counts; a tick is one server player sync ("k":
// one burst of NET_PACKET_STATE). client_net_tally_take() returns the counts since the last take
// and resets them (resetPacketTally).
typedef struct {
    int ingress_packets, ingress_bytes;
    int egress_packets, egress_bytes;
    int ticks;
} ClientNetTally;
void client_net_tally_take(ClientNetTally *out);
void client_net_tally_egress(uint16_t length);
void client_net_log_packet(const char*, NetPacketType, const void*, uint16_t);
int client_net_send_packet(int, NetPacketType, const void*, uint16_t);

// Offline (client/src/net/offline.c): the in-process sandbox server (server_sandbox.h, the
// real server core) joined over its loopback transport like any server, stepped by the main
// loop. client->game is a normal predicted client game.
int local_game_start(Client*);   // start the sandbox and join it: offline_start_map, else the startup map, else a random rotation map; mode net_mode_index / KRUNKNATIVE_MODE
void local_game_stop(Client*);   // leave offline mode (the sandbox stops); the game stays loaded as a plain mirror
void client_offline_pump(Client*, float delta);  // main loop, before client_tick_net: step the sandbox, queue what it sent
void client_offline_send(Client*, NetPacketType, const void *payload, uint16_t length); // client_net_send offline
void client_tick_net(Client*);   // handle the queued packets (main.c)
// Host Game / console `map`: reload on `map_index` (-1 current) in Krunker mode `mode_id`,
// the only mode of the next rounds.
void local_game_host(Client*, int map_index, int mode_id);
// The editor's Test Map: play `map` (a Krunker map JSON, copied) offline now, starting the
// offline game (leaving a remote session) when needed. 0 on failure.
#define LOCAL_GAME_HAVE_HOST_CUSTOM 1
int local_game_host_custom(Client*, const cJSON *map);
// Host Game's config (createPrivateRoom: ui_host_config_collect's request, a config.hostTypes
// index): the offline game plays it from its next round on (a running sandbox) or from its start,
// until the offline game stops. NULL drops it.
#define LOCAL_GAME_HAVE_HOST_CONFIG 1
void local_game_host_config(Client*, const cJSON *request, int host_type);
// The offline sandbox's map: v7.1.4 config.blankMap (index.js: ?sandbox / ?test, and the
// editor's Test with no map stored loadSandbox it), the catalogue's community map "Test"
// (maps/custom/Test.json). One name for --sandbox, web ?sandbox and the console `sandbox`.
#define LOCAL_GAME_SANDBOX_MAP "Test"
// A map named on the command line, in ?map= or by a console command -> its catalogue index
// (-1 none): an exact catalogue key, a path under assets/maps ("custom/ESP_Parkour",
// "maps/kanji.json", "311_kanji"), an old path of a renamed map that is no file any more
// ("v7/kanji", "custom/old_burg": map_catalog_index_of_file), else map_catalog_resolve's names
// and old keys ("Kanji", "soul sanctum", "v3_burg" = 311_burg).
int local_game_map_lookup(const char *name);
// The catalogue map `catalog_index` in the client's map list (map_catalog_slot): the offline
// game reloads on it when it runs, else the next local_game_start starts on it
// (offline_start_map). Returns its slot, -1 when the map could not be loaded.
int local_game_pick_map(Client*, int catalog_index);

void client_frame(Client*); // one iteration of the main loop: tick, swap, poll events

// Match events (client/src/net/game_events.c). CLIENT_HAVE_NET_LIVE_FIELDS marks the
// net_ping_ms / net_round_seconds / Player.name / Player.score fields above.
#define CLIENT_HAVE_NET_LIVE_FIELDS 1
#define CLIENT_HAVE_CHAT_SEND 1
void client_chat_send(Client*, const char *text); // Enter-to-chat: sends one line (trimmed, max 70 chars)
int client_handle_game_packet(Client*, const NetPacket*); // 1 = handled (KILL/HIT/HEALTH/CHAT/...)
int client_handle_game_event(Client*, int type, const void *payload, uint16_t length); // same, typed payload
// WP-B v7 combat packets (client/src/net/combat_events.c): effects, streaks, Nuke, Juggernaut, perks.
int combat_events_handle(Client*, int type, const void *payload, uint16_t length); // 1 = handled
// map-logic WP-H zombies / airdrop packets (client/src/net/zombie_events.c)
int zombie_events_handle(Client*, int type, const void *payload, uint16_t length); // 1 = handled
void combat_send_streak_slots(Client*);      // the streakIndex1..3 loadout, right before every ENT
void client_send_pc_stat(Client*);           // WP-E (net/game_events.c): pcStatIndex, right before every ENT
void combat_call_in_streak(Client*, int slot); // window.callInStreak(slot): the streak keys
// The sight a player's weapons take (attach.js index): the local player's saved attachIndex,
// others' Player.attach_index (0 Reflex until the v7 SPAWN carries attachIndex, WP-C).
int player_attach_index(const Player*);
int client_saved_attach_index(void); // getSavedVal("attachIndex") || 0: our pick (ENT V8 ent[12])
// window.updateScope(scopeIndex, savedScope) (v7.1.4 aimRecticles.js): the #recticleImg source for
// the saved scopeIndex (getSavedVal, 0 Default) into buf; NULL when scopeIndex is -1 (no overlay).
// A custom (-2) image must be an http(s) URL and not .svg, else scope_0.
const char *combat_scope_overlay_src(char *buf, size_t n);
void client_net_send(Client*, NetPacketType, const void*, uint16_t); // the socket, or offline the sandbox connection
// Prediction / reconciliation (client/src/net/recon.c, Krunker tmpInpts / "q" / reconcilePlayer).
void client_recon_reset(Client*);                                 // CONTROLS.reset(): our spawn, a new session
float client_recon_sample(Client*, float frame_delta);            // WP-C netRate: this frame's input delta (s), -1 = no input frame
void client_recon_record(Client*, const Input*);                  // a predicted input: pending + the "q" batch
int  client_recon_sending(const Client*);                         // inputs may leave: alive, round not over (net/recon.c)
int  client_round_ended(const Client*);                           // END seen while playing, until the next INIT (net/recon.c)
void client_recon_on_death(Client*);                              // our KILL: the unsent "q" window is dropped
void client_recon_drop_pending(Client*);                          // Krunker "f": the pending inputs are dropped (never acked)
#define NET_RECON_CORRECTION_EPS 0.01f // a RECON that moved our player further is a correction (units)
void client_recon_after_tick(Client*);                            // the frame's shot knockback -> input.velObj
void client_recon_apply(Client*, const NetReconPacket*);          // "l": snap + replay
int  client_recon_delay_inbound(Client*, const NetPacket*);       // KRUNKNATIVE_NET_DELAY_MS: 1 = held
void client_recon_drain_delayed(Client*, void (*)(Client*, const NetPacket*));
void client_game_events_tick(Client*); // per frame: death delay -> menu, HUD bridge
void client_game_events_reset(Client*); // new session / INIT: forget round and death state
double client_death_clock(const Client*); // the death delay's clock (s): the HUD frame clock (GLFW in play)
void client_leave_game(Client*);        // the game is left (disconnect, switch, offline <-> online): its state goes
void client_apply_init(Client*, const NetInitPacket*);  // main.c: (re)load the map and drop the players
// main.c scene helpers for client->game (also driven by the offline game, net/local_game.c)
void client_load_map(Client*);
void client_unload_map(Client*);
void client_add_player_mesh(Client*, Player*, int render_you);
void client_drop_player_mesh(Client*, Player*);
void client_player_spawned(Client*, Player*, int is_you); // meshes + (is_you) in-game state after a spawn
void client_force_player_positions(Client*); // players.js forcePos(): every remote player's next STATE snaps
int client_map_prepare_init(Client*, NetInitPacket*);   // 1: apply now (map_index resolved), 0: map download started
int client_map_hold(Client*, const NetPacket*);         // 1: packet queued until the streamed map is in
int client_map_take_ready(Client*, NetInitPacket*, NetPacket**, size_t*); // 1: download done (INIT + held packets, caller frees)
int client_map_list_add(Client*, const cJSON*); // index of the map in game.maps, appending it (official games, streamed maps); -1 OOM
// The custom game id of a map the official link streamed (Krunker INIT arg 9 .id) and the id of a game.maps
// slot ("" none). Kept per map object, so two queued INITs never swap ids.
void client_set_custom_game_id(Client*, const cJSON *map, const char *id);
const char *client_custom_game_id_of(const Client*, int map_index);

void client_animate_object_texture(Object*, float);

unsigned int load_texture(char*);
GlyphCacheEntry *load_glyph(char);
Geometry *load_obj_model(char*, int);
// The file behind a cached texture / OBJ model changed (a resource pack came or went,
// mod_pack.c): read it again (through file_redirect) into the same GL texture / Geometry, so
// every material and mesh holding it follows. 0 when `path` is not cached or cannot be read.
int texture_cache_reload(const char *path);
int geometry_cache_reload(const char *path);
// F5 (game_reload.c): every cached texture / OBJ model read again the same way. Returns how
// many were reloaded.
int texture_cache_reload_all(void);
int geometry_cache_reload_all(void);
// Fills geometry->bound_center/bound_radius from its local vertices (loaders.c); geometry
// whose vertices move in the shader (liquid waves) must leave the radius at 0.
void geometry_set_bounds(Geometry*, const vertex *vertices, size_t vertex_count);

Geometry *create_cube_geo();
Geometry *create_plane_geo();
Geometry *create_ramp_geo();
Geometry *create_hose_geo(float radius, float length, int sides); // KrunkNative Piss: render.js genHose (cached)
// KrunkNative Piss: the hose (genHose) of a stream weapon in `color` (0xRRGGBB; the player's skin
// colour on players, as the branch passes skinCol), MeshLambert-like BasicMaterial; NULL none.
Mesh *player_hose_mesh(const WeaponStream *stream, int color);

Mesh *prefab_init(Object*, const vec4*, const cJSON*);
void prefab_begin_map(const cJSON *objects, const cJSON *colors); // per-map prefab state (shared model materials, the raw objects) before prefab_init()
vec4 prefab_raw_color(const cJSON *raw_obj, const vec4 *colors); // map-G: the c / ci colour of a raw object (white when absent)
