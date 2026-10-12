// gltf_test: the GLTF loader (client/src/gltf.c) on Krunker v7.1.4 animated store models, loaded
// through the pet path (client/src/pets.c pet_create, as a hasAnim pet) and the cosmetic path:
//   - wrist/wrist_0.gltf (Kronix Gold, wrist 4249) and back/back_103.gltf (Moonfeather, back 2837):
//     the model loads, has meshes and clips, and its clips move its nodes over time;
//   - pets.js follow AI: a pet moves toward its holder, slows down within 40 units, bobs, turns;
//   - a v7 OBJ pet (pets/pet_0, Ghostie) loads with its material options (frames, glow);
//   - the hue cycle (UTILS.hexFromHue) and the frames / pulse / rgb animations (skin_fx_tick).
//   gltf_test [--headless]      (from the repo root; KRUNKNATIVE_HEADLESS=1 works too)
#include <gl.h>
#include <GLFW/glfw3.h>
#include <client.h>
#include <cosmetics.h>
#include <gltf.h>
#include <headless.h>
#include <render_settings.h>
#include <pets.h>
#include <skin_fx.h>
#include "../../client/src/viewer/viewer_store.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures, g_checks;
#define CHECK(cond, ...) do { g_checks++; if (!(cond)) { g_failures++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

// ---- the renderer state basic.c / gltf.c / loaders.c read in the client (main.c / ui_render_settings.c / scene.c)
geometry_cache_map g_geometry_cache;
texture_cache_map g_texture_cache;
glyph_cache_map g_glyph_cache;
FT_Face g_game_font;
Geometry *g_cube_geometry;
Geometry *g_plane_geometry;
Geometry *g_ramp_geometry;
unsigned int g_blank_texture;
unsigned int g_active_texture;
RenderLighting g_render_lighting;
void scene_add_mesh(Scene *scene, Mesh *mesh) { (void) scene; (void) mesh; }
void scene_remove_mesh(Scene *scene, Mesh *mesh) { (void) scene; (void) mesh; }
static ClientRenderSettings g_settings;
const ClientRenderSettings *client_render_settings(void) { return &g_settings; }
void scene_draw_state(int cull_back, int additive_blend, int depth_write) { (void) cull_back; (void) additive_blend; (void) depth_write; }
void scene_draw_cull_front(void) {}

// the meshes' world matrices and the morph weights summed: changes when any node moves or morphs
static double model_pose(GltfInstance *model) {
    double sum = 0.0;
    for (size_t i = 0; i < model->mesh_count; i++) {
        mesh_update_transform_matrix(model->meshes[i]);
        for (int k = 0; k < 16; k++) sum += fabs((double) model->meshes[i]->transform_matrix[k]) * (double) (k + 1) * (double) (i + 1);
    }
    float weights[64];
    const int n = gltf_instance_morph_weights(model, weights, 64);
    for (int i = 0; i < n && i < 64; i++) sum += (double) weights[i] * 1000.0 * (double) (i + 1);
    return sum;
}

// a hasAnim model as a pet: a copy of a pet item pointing at another model dir (assetType)
static void check_gltf_pet(const int store_index, const char *what) {
    const ViewerSkin *src = viewer_skin(store_index);
    CHECK(src && (src->flags & VS_HAS_ANIM), "%s (%d) is a hasAnim item", what, store_index);
    if (!src) return;
    ViewerSkin item = *src;
    item.asset_type = src->asset_type ? src->asset_type : src->type; // its own model dir
    item.type = ITEM_PET;
    char model_key[96], texture_key[96];
    cosmetic_asset_keys(&item, model_key, sizeof(model_key), texture_key, sizeof(texture_key));

    PlayerPet *pet = pet_create(&item, (vec3) {0.0f, 10.0f, 0.0f}, 0);
    CHECK(pet && pet->model && !pet->mesh, "%s: loads as a GLTF pet (%s)", what, model_key);
    if (!pet || !pet->model) {
        pet_free(pet);
        return;
    }
    CHECK(pet->model->mesh_count > 0, "%s: %zu meshes", what, pet->model->mesh_count);
    CHECK(gltf_model_animated(pet->model->model), "%s: has animation clips", what);
    CHECK(fabsf(pet->model->root.scale.x - (item.scl_mlt > 0.0f ? item.scl_mlt : PET_DEFAULT_SCALE)) < 1e-5f, "%s: pet scale sclMlt || 0.8", what);
    CHECK(fabsf(pet->model->root.rotation.y - (float) M_PI * 0.5f) < 1e-5f, "%s: turned PI / 2 (not a preview)", what);

    gltf_instance_update(pet->model, 0.0);
    const double pose0 = model_pose(pet->model);
    gltf_instance_update(pet->model, 0.37);
    const double pose1 = model_pose(pet->model);
    gltf_instance_update(pet->model, 0.91);
    const double pose2 = model_pose(pet->model);
    CHECK(fabs(pose1 - pose0) > 1e-6 || fabs(pose2 - pose0) > 1e-6, "%s: the clips move the model (%.6f %.6f %.6f)", what, pose0, pose1, pose2);
    pet_free(pet);
}

static void check_follow(void) {
    const ViewerSkin *ghostie = viewer_skin(2964);
    PlayerPet *pet = pet_create(ghostie, (vec3) {0.0f, 0.0f, 0.0f}, 0);
    CHECK(pet && pet->mesh, "Ghostie (2964): an OBJ pet");
    if (!pet) return;
    BasicMaterial *material = (BasicMaterial *) pet->mesh->material;
    CHECK(fabsf(material->texture_repeat.x - 0.5f) < 1e-6f, "Ghostie: 2 frames, repeat.x 1/2 (%.3f)", (double) material->texture_repeat.x);
    CHECK(material->base.transparent && material->double_sided, "Ghostie: trans, side 2");
    CHECK(fabsf(pet->speed - PET_DEFAULT_SPEED) < 1e-6f, "default speed 0.12");

    // far target: full speed 0.12 per ms
    const vec3 far = {100.0f, 0.0f, 0.0f};
    pet_update(pet, far, 100.0f);
    CHECK(fabsf(pet->position.x - 12.0f) < 1e-3f && fabsf(pet->position.z) < 1e-3f, "follows at 0.12 / ms toward a far target (x %.3f)", (double) pet->position.x);
    // within 40 units: slowed by distance / 40
    pet->position = (vec3) {80.0f, 0.0f, 0.0f};
    pet_update(pet, far, 10.0f);
    CHECK(fabsf(pet->position.x - (80.0f + 1.2f * 0.5f)) < 1e-3f, "slows down within 40 units (x %.4f)", (double) pet->position.x);
    // climbs toward a target above
    pet->position = (vec3) {0.0f, 0.0f, 0.0f};
    pet_update(pet, (vec3) {0.0f, 50.0f, 0.0f}, 10.0f);
    CHECK(pet->position.y > 1.19f && fabsf(pet->position.x) < 1e-3f, "climbs (y %.3f)", (double) pet->position.y);
    // bob: root.y = y + sin(bobY) * 0.2
    CHECK(fabsf(pet->root.position.y - (pet->position.y + sinf(pet->bob_y) * PET_BOB_HEIGHT)) < 1e-5f && pet->bob_y > 0.0f, "bobs");
    // turns by at most 0.01 rad per ms
    pet->root.rotation.y = 0.0f;
    pet->position = (vec3) {0.0f, 0.0f, 0.0f};
    pet_update(pet, (vec3) {0.0f, 0.0f, 100.0f}, 10.0f);
    const float turned = fminf(pet->root.rotation.y, 2.0f * (float) M_PI - pet->root.rotation.y);
    CHECK(turned > 0.0f && turned <= 0.1f + 1e-5f, "turns at most 0.01 rad / ms (%.4f)", (double) turned);
    pet_free(pet);

    // a preview pet is not turned
    PlayerPet *preview = pet_create(ghostie, (vec3) {0}, 1);
    CHECK(preview && preview->mesh && preview->mesh->transform.rotation.y == 0.0f, "preview pet: rotation 0");
    pet_free(preview);
}

static void check_effects(void) {
    CHECK(skin_fx_hex_from_hue(0) == 0xFF0000 && skin_fx_hex_from_hue(120) == 0x00FF00 && skin_fx_hex_from_hue(240) == 0x0000FF,
          "hexFromHue 0 / 120 / 240 (%06X %06X %06X)", skin_fx_hex_from_hue(0), skin_fx_hex_from_hue(120), skin_fx_hex_from_hue(240));
    CHECK(skin_fx_hex_from_hue(60) == 0xFFFF00 && skin_fx_hex_from_hue(300) == 0xFF00FF, "hexFromHue 60 / 300");

    // frames: Ghostie (2 frames, frameT 400)
    PlayerPet *pet = pet_create(viewer_skin(2964), (vec3) {0}, 0);
    if (!pet || !pet->mesh) {
        CHECK(0, "Ghostie for the effects");
        return;
    }
    BasicMaterial *m = (BasicMaterial *) pet->mesh->material;
    skin_fx_tick(399.0f, 1, 0);
    CHECK(m->texture_offset.x == 0.0f, "frame 0 before frameT");
    skin_fx_tick(2.0f, 1, 0);
    CHECK(m->texture_offset.x == 1.0f, "frame 1 after frameT (%.1f)", (double) m->texture_offset.x);
    skin_fx_tick(400.0f, 0, 0);
    CHECK(m->texture_offset.x == 1.0f, "textureAnim off: frozen");
    skin_fx_tick(400.0f, 1, 0);
    CHECK(m->texture_offset.x == 0.0f, "wraps to frame 0");
    pet_free(pet);
}

int main(int argc, char **argv) {
    headless_parse_args(&argc, argv);
    vt_init(&g_geometry_cache);
    vt_init(&g_texture_cache);
    vt_init(&g_glyph_cache);
    g_settings.texture_anim = 1;
    g_settings.lighting = 1;
    if (!headless_glfw_init()) {
        fprintf(stderr, "gltf_test: glfwInit failed (use --headless / KRUNKNATIVE_HEADLESS=1)\n");
        return 1;
    }
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
#ifdef KRUNKNATIVE_GL_ANGLE
    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
    glfwWindowHint(GLFW_CONTEXT_CREATION_API, GLFW_EGL_CONTEXT_API);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#else
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
#ifdef __APPLE__
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
#else
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
#endif
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#endif
    headless_window_hints();
    GLFWwindow *window = glfwCreateWindow(64, 64, "gltf_test", NULL, NULL);
    if (!window || !headless_make_context_current(window) || !kn_gl_load(headless_get_proc_address)) {
        fprintf(stderr, "gltf_test: no GL context\n");
        return 1;
    }

    check_gltf_pet(4249, "Kronix Gold wrist_0.gltf");
    check_gltf_pet(2837, "Moonfeather back_103.gltf");
    check_follow();
    check_effects();

    glfwDestroyWindow(window);
    glfwTerminate();
    printf(g_failures ? "gltf_test: %d of %d checks failed\n" : "gltf_test: OK (%2$d checks)\n", g_failures, g_checks);
    return g_failures ? 1 : 0;
}
