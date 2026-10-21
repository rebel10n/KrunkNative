// texture_checker_test: a texture that does not load shows the shared missing-texture checker
// (texture_checker, loaders.c: render.js's missingTexture), not a blank or a flat colour.
//   - a map's sky whose user-asset texture the tree does not ship (sky.c sky_texture) gets the
//     checker: the dome is textured with it, not a plain gradient;
//   - a map texture that does not ship (textures/<src>.png, the path prefab.c takes through
//     load_texture) gets the same checker;
//   - the checker is one texture for every caller, and a texture that does load is not it.
// Headless GL (--headless); run from the repo root. Exits 77 (skipped) without GL.
#include <gl.h>
#include <GLFW/glfw3.h>
#include <client.h>
#include <headless.h>
#include <render_settings.h>
#include <sky.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures, g_checks;
#define CHECK(cond, ...) do { g_checks++; if (!(cond)) { g_failures++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

// ---- the renderer state the loaders read (main.c / ui_render_settings.c / scene.c), as gltf_test has it
geometry_cache_map g_geometry_cache;
texture_cache_map g_texture_cache;
glyph_cache_map g_glyph_cache;
FT_Face g_game_font;
Geometry *g_cube_geometry;
Geometry *g_plane_geometry;
Geometry *g_ramp_geometry;
unsigned int g_blank_texture;
unsigned int g_active_texture;
unsigned int g_active_shader;
RenderLighting g_render_lighting;
void camera_update_projection_matrix(Camera *camera, float aspect) { (void) camera; (void) aspect; }
void camera_update_world_inverse_matrix(Camera *camera) { (void) camera; }
void scene_add_mesh(Scene *scene, Mesh *mesh) { (void) scene; (void) mesh; }
void scene_remove_mesh(Scene *scene, Mesh *mesh) { (void) scene; (void) mesh; }
static ClientRenderSettings g_settings;
const ClientRenderSettings *client_render_settings(void) { return &g_settings; }
void scene_draw_state(int cull_back, int additive_blend, int depth_write) { (void) cull_back; (void) additive_blend; (void) depth_write; }
void scene_draw_cull_front(void) {}

// True when the asset file exists under assets_path() (the tree may not ship every texture).
static int asset_exists(const char *relative) {
    char *path = concat(assets_path(), relative);
    FILE *f = path ? fopen(path, "rb") : NULL;
    if (f) fclose(f);
    free(path);
    return f != NULL;
}

// The map's texture loader for textures/<src>.png (prefab.c prefab_load_texture_src).
static unsigned int map_texture(const char *relative) {
    char *path = concat(assets_path(), relative);
    const unsigned int texture = path ? load_texture(path) : 0;
    free(path);
    return texture;
}

// A fake fetch for the paths named "downloading_test": 0 = downloading, -1 = given up (asset_fetch.h).
static int g_fetch_state;
static int test_fetch_hook(const char *path, int enqueue) {
    (void) enqueue;
    return strstr(path, "downloading_test") ? g_fetch_state : 1;
}

// A map whose sky uses a user asset the tree does not ship (skyDomeTexA: user-assets/<id>/texture.png).
static void load_missing_sky(void) {
    cJSON *map = cJSON_Parse("{\"skyDome\":true,\"skyDomeTex\":true,\"skyDomeTexA\":987654}");
    CHECK(map != NULL, "the test map parses");
    if (map) sky_load(map);
    cJSON_Delete(map);
}

int main(int argc, char **argv) {
    headless_parse_args(&argc, argv);
    vt_init(&g_geometry_cache);
    vt_init(&g_texture_cache);
    vt_init(&g_glyph_cache);
    if (!headless_glfw_init()) {
        printf("texture_checker_test: no GL (glfwInit failed), skipped\n");
        return 77;
    }
    headless_window_hints();
    GLFWwindow *win = glfwCreateWindow(64, 64, "texture checker test", NULL, NULL);
    if (!win || !headless_make_context_current(win) || !kn_gl_load(headless_get_proc_address)) {
        printf("texture_checker_test: no GL context, skipped\n");
        return 77;
    }

    const unsigned int checker = texture_checker();
    CHECK(checker != 0, "the checker is a GL texture");
    CHECK(texture_checker() == checker, "the checker is one texture for every caller");

    // 1. a missing sky texture: the dome is textured with the checker
    load_missing_sky();
    const unsigned int sky_texture = sky_texture_id();
    CHECK(sky_texture == checker, "missing sky texture: the sky is the checker (got %u, checker %u)", sky_texture, checker);

    // 2. a missing map texture: the same checker
    const unsigned int missing_map = map_texture("textures/no_such_map_texture_checker_test.png");
    CHECK(missing_map == checker, "missing map texture: the checker (got %u, checker %u)", missing_map, checker);
    // and again: the failure is not cached under the path, so it still answers the checker
    CHECK(map_texture("textures/no_such_map_texture_checker_test.png") == checker, "missing map texture, asked again: the checker");

    // 3. a texture that loads is not the checker (the fallback must not replace real textures)
    if (asset_exists("textures/sky_6.png")) {
        const unsigned int real = map_texture("textures/sky_6.png");
        CHECK(real != 0 && real != checker, "textures/sky_6.png loads as its own texture (got %u)", real);
        cJSON *map = cJSON_Parse("{\"skyDome\":true,\"skyDomeTex\":true}");
        if (map) sky_load(map);
        cJSON_Delete(map);
        CHECK(sky_texture_id() != 0 && sky_texture_id() != checker, "the default sky texture loads (got %u)", sky_texture_id());
    } else {
        printf("texture_checker_test: textures/sky_6.png is not in the asset tree, the loaded-texture check is skipped\n");
    }

    // 4. a download in flight is a blank placeholder (three.js's empty texture) until the fetch answers:
    //    given up, it becomes the checker, in place (the same GL texture every material holds)
    asset_set_fetch_hook(test_fetch_hook);
    g_fetch_state = 0;
    const unsigned int placeholder = map_texture("textures/downloading_test.png");
    CHECK(placeholder != 0 && placeholder != checker, "a download in flight: its placeholder (got %u)", placeholder);
    CHECK(texture_placeholders_check() == 0, "a download still in flight stays a placeholder");
    g_fetch_state = -1;
    CHECK(texture_placeholders_check() == 1, "a download given up becomes the checker in place");
    CHECK(texture_placeholders_check() == 0, "a given-up download is upgraded once");
    CHECK(map_texture("textures/downloading_test.png") == placeholder, "the given-up texture keeps its GL texture");
    CHECK(glGetError() == GL_NO_ERROR, "the checker upload raises no GL error");
    asset_set_fetch_hook(NULL);

    if (g_failures) {
        printf("texture_checker_test: %d of %d checks failed\n", g_failures, g_checks);
        return 1;
    }
    printf("texture_checker_test: %d checks passed\n", g_checks);
    return 0;
}
