// gltf_test: the GLTF loader (client/src/gltf.c) on Krunker v7.1.4 animated store models, loaded
// through the pet path (client/src/pets.c pet_create, as a hasAnim pet) and the cosmetic path:
//   - wrist/wrist_0.gltf (Kronix Gold, wrist 4249) and back/back_103.gltf (Moonfeather, back 2837):
//     the model loads, has meshes and clips, and its clips move its nodes over time;
//   - pets.js follow AI: a pet moves toward its holder, slows down within 40 units, bobs, turns;
//   - a v7 OBJ pet (pets/pet_0, Ghostie) loads with its material options (frames, glow);
//   - the hue cycle (UTILS.hexFromHue) and the frames / pulse / rgb animations (skin_fx_tick);
//   - v7's EXT_meshopt_compression AI / map models (user-assets/<id>/model.glb, gltf_meshopt.c):
//     each loads with meshes, a sane size, and its clips (when it has any) move it;
//   - v10.0.3 video skins (skin_video.c): the Glitch / Deep Space dyes and the Mysterious Orb melee show
//     videos/video_<n>.mp4 (its .knv sidecar) and the frame changes with the clock; noVideoSkins gives
//     them their regular texture (none for a dye, melee_20 for the melee) and no projection;
//   - v10.0.3 material extras (tools/gltf_test/env_extras.gltf): env (the opt-in reflections) and noFog;
//   - v10.0.3 weapon altSkin remodels (viewer_weapon_alt_skin): the Nova Pump / Semi / Uzi / Alien and the Cupids
//     Blaster load their model and texture, and the upgraded ones play the altSkin video on the remodel; the shot
//     sound a remodel plays (weapon.sound_<mid>, noSnd, upgraded, blast) and getSkinForLoadout; a custom studio skin's
//     model / texture keys and files (user-assets/skins/<_id>/).
//   gltf_test [--headless]      (from the repo root; KRUNKNATIVE_HEADLESS=1 works too)
#include <gl.h>
#include <GLFW/glfw3.h>
#include <client.h>
#include <cosmetics.h>
#include <gltf.h>
#include <headless.h>
#include <render_settings.h>
#include <network_sounds.h>
#include <pets.h>
#include <ruleset.h>
#include <skin_fx.h>
#include <skin_video.h>
#include <user_asset_path.h>
#include "../../client/src/viewer/viewer_store.h"
#include <math.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif
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
    for (size_t n = 0; n < gltf_instance_node_count(model); n++) { // bones: skinned meshes keep their own matrix
        const MeshTransform *t = gltf_instance_node(model, n);
        const float v[9] = {t->position.x, t->position.y, t->position.z, t->rotation.x, t->rotation.y, t->rotation.z, t->scale.x, t->scale.y, t->scale.z};
        for (int k = 0; k < 9; k++) sum += fabs((double) v[k]) * (double) (k + 1) * (double) (n + 1) * 0.001;
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

// v10.0.3's model setup reads the material extras (GLTFLoader userData, deob-10.0.3.js:233662-233669):
// env (parseFloat, limitMM 0..4) opts the material into getEnvMap reflections, noFog turns its fog off.
// Textures are uploaded raw (NoColorSpace): no texture here, so no GLTFLoader sRGB tag either.
static void check_material_extras(void) {
    GltfModel *model = gltf_model_load("tools/gltf_test/env_extras.gltf");
    CHECK(model != NULL, "env_extras.gltf loads");
    GltfInstance *instance = model ? gltf_instance_create(model, 1.0f) : NULL;
    CHECK(instance && instance->mesh_count == 3, "env_extras.gltf: 3 meshes");
    int env_nofog = 0, env_clamped = 0, plain = 0;
    for (size_t i = 0; instance && i < instance->mesh_count; i++) {
        const BasicMaterial *m = (const BasicMaterial *) instance->meshes[i]->material;
        if (m->scene_env_intensity == 2.5f && !m->fog_enabled && m->metalness_factor == 1.0f) env_nofog++;
        else if (m->scene_env_intensity == 4.0f && m->fog_enabled) env_clamped++;
        else if (m->scene_env_intensity == 0.0f && m->fog_enabled) plain++;
        CHECK(!m->gltf_srgb_maps && !m->texture_srgb, "env_extras.gltf mesh %zu: no sRGB maps", i);
    }
    CHECK(env_nofog == 1, "extras {env: 2.5, noFog: true}: envMapIntensity 2.5, fog off");
    CHECK(env_clamped == 1, "extras {env: \"9\"}: parseFloat, limitMM(.., 0, 4) = 4");
    CHECK(plain == 1, "no extras: no reflections, fogged");
    if (instance) gltf_instance_fini(instance);
}

// gltfpack output (KHR_mesh_quantization + EXT_meshopt_compression): Mirror Image (50578, AI.js
// kind 3), the Laboratory (50329-50331) and Tortuga (50572-50577) map AIs, 48931 and 52615
static void check_meshopt_models(void) {
    static const int ids[] = {48931, 50329, 50330, 50331, 50572, 50573, 50575, 50576, 50577, 50578, 52615};
    int loaded = 0;
    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        char path[256];
        snprintf(path, sizeof(path), "%suser-assets/%d/model.glb", assets_path(), ids[i]);
        FILE *f = fopen(path, "rb");
        if (!f) {
            printf("skip: %s (make assets-import-live)\n", path);
            continue;
        }
        fclose(f);
        GltfModel *model = gltf_model_load(path);
        CHECK(model != NULL, "%s: loads (EXT_meshopt_compression decoded)", path);
        if (!model) continue;
        loaded++;
        GltfInstance *instance = gltf_instance_create(model, 1.0f);
        CHECK(instance && instance->mesh_count > 0, "%s: has meshes", path);
        if (!instance) continue;
        float radius = 0.0f;
        for (size_t m = 0; m < instance->mesh_count; m++) {
            const Geometry *g = instance->meshes[m]->geometry;
            if (g && g->bound_radius > radius) radius = g->bound_radius;
            CHECK(g && g->index_count > 0, "%s: mesh %zu has triangles", path, m);
        }
        // local space: KHR_mesh_quantization positions are integers the node scale brings down;
        // skinned / morphed primitives are re-uploaded per frame and have no static bounds (0)
        CHECK(radius == 0.0f || (radius > 0.01f && radius < 70000.0f), "%s: bounding radius %.3f", path, radius);
        if (gltf_model_animated(model)) {
            gltf_instance_update(instance, 0.0);
            const double pose0 = model_pose(instance);
            gltf_instance_update(instance, 0.37);
            const double pose1 = model_pose(instance);
            gltf_instance_update(instance, 0.91);
            const double pose2 = model_pose(instance);
            CHECK(fabs(pose1 - pose0) > 1e-6 || fabs(pose2 - pose0) > 1e-6, "%s: the clips move the model", path);
        }
        gltf_instance_fini(instance); // frees it
    }
    printf("meshopt models: %d loaded\n", loaded);
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

// A checksum of a 8x8 grid of the texture's texels (an FBO read-back: no glGetTexImage on GLES), 0 when unreadable.
static unsigned long video_texture_sum(const unsigned texture, const int w, const int h) {
    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    unsigned long sum = 0;
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
        for (int y = 0; y < 8; y++) {
            for (int x = 0; x < 8; x++) {
                unsigned char px[4] = {0};
                glReadPixels(x * w / 8 + w / 16, y * h / 8 + h / 16, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
                sum = sum * 31u + px[0] * 65536u + px[1] * 256u + px[2] + 1u;
            }
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    return sum;
}

// a single-sided material (a body part's or weapon's), so the projection's DoubleSide shows
static Mesh *video_mesh(void) {
    BasicMaterial *material = basic_material_init();
    material->double_sided = 0;
    return mesh_init(NULL, (Material *) material);
}

static void check_video_skins(void) {
    CHECK(skin_video_alt_skin(0) == 2 && skin_video_alt_skin(28) == 2 && skin_video_alt_skin(11) == -1 && skin_video_alt_skin(17) == -1 &&
          skin_video_alt_skin(29) == -1, "upgradeWeapons altSkin videos: 0 / 28 video 2, Cupids Blaster 11 and Bomb 17 none");
    FILE *f = fopen("assets/videos/video_3.mp4.knv", "rb");
    FILE *g = fopen("assets/videos/video_0.mp4.knv", "rb");
    if (f) fclose(f);
    if (g) fclose(g);
    if (!f || !g) {
        printf("gltf_test: video skins skipped (no assets/videos/video_{0,3}.mp4.knv: make video-frames)\n");
        return;
    }
    const ViewerSkin *glitch = viewer_skin(7446), *deep_space = viewer_skin(4208), *orb = viewer_skin(4211);
    CHECK(glitch && glitch->video == 3 && deep_space && deep_space->video == 0 && orb && orb->video == 0, "the video skins of the store table");
    if (!glitch || !deep_space || !orb) return;

    // playing (noVideoSkins off, its default)
    skin_video_set_disabled(0);
    Mesh *dye = video_mesh(), *proj = video_mesh(), *melee = video_mesh();
    skin_fx_apply_dye(dye, glitch);
    skin_fx_apply_dye(proj, deep_space);
    skin_fx_apply(melee, orb, "melee/melee_20_3", "melee/melee_20", 0, 0);
    const BasicMaterial *dm = (BasicMaterial *) dye->material, *pm = (BasicMaterial *) proj->material, *mm = (BasicMaterial *) melee->material;
    CHECK(dm->texture != 0 && !dm->projection_uv, "Glitch dye: a video texture, UV-mapped");
    CHECK(pm->texture != 0 && pm->projection_uv && pm->double_sided, "Deep Space dye: a video texture in screen space, DoubleSide");
    CHECK(mm->texture == pm->texture && mm->projection_uv && mm->double_sided, "Mysterious Orb: video_0 in screen space (the dye's LinearFilter texture)");
    int videos = 0, in_use = 0;
    unsigned uploaded0 = 0, uploaded1 = 0;
    skin_video_frame(100.0);
    skin_video_stats(&videos, &in_use, &uploaded0);
    CHECK(videos == 2 && in_use == 2 && uploaded0 >= 2, "two videos play (%d, %d in use, %u frames)", videos, in_use, uploaded0);
    const unsigned long glitch0 = video_texture_sum(dm->texture, 736, 336), space0 = video_texture_sum(pm->texture, 736, 336);
    skin_video_frame(100.5); // frame 15 of 30 fps
    skin_video_stats(NULL, NULL, &uploaded1);
    const unsigned long glitch1 = video_texture_sum(dm->texture, 736, 336), space1 = video_texture_sum(pm->texture, 736, 336);
    CHECK(uploaded1 == uploaded0 + 2, "0.5 s later: one new frame per video (%u -> %u)", uploaded0, uploaded1);
    CHECK(glitch0 && space0 && glitch0 != glitch1 && space0 != space1, "the texels change with the frame (%lx %lx, %lx %lx)", glitch0, glitch1, space0, space1);
    skin_video_frame(100.51); // same frame: nothing decoded
    unsigned uploaded2 = 0;
    skin_video_stats(NULL, NULL, &uploaded2);
    CHECK(uploaded2 == uploaded1, "the same frame is not decoded again");

    // the materials go: no video is decoded any more
    skin_fx_forget(dye);
    skin_fx_forget(proj);
    skin_fx_forget(melee);
    mesh_fini(dye);
    mesh_fini(proj);
    mesh_fini(melee);
    skin_video_stats(NULL, &in_use, NULL);
    skin_video_frame(101.0);
    unsigned uploaded3 = 0;
    skin_video_stats(NULL, NULL, &uploaded3);
    CHECK(in_use == 0 && uploaded3 == uploaded2, "no user: no decoding (%d in use, %u -> %u)", in_use, uploaded2, uploaded3);

    // noVideoSkins: video, projection and texSrc stripped (deob-10.0.3.js:231704-231710)
    skin_video_set_disabled(1);
    dye = video_mesh();
    proj = video_mesh();
    melee = video_mesh();
    skin_fx_apply_dye(dye, glitch);
    skin_fx_apply_dye(proj, deep_space);
    skin_fx_apply(melee, orb, "melee/melee_20_3", "melee/melee_20", 0, 0);
    dm = (BasicMaterial *) dye->material;
    pm = (BasicMaterial *) proj->material;
    mm = (BasicMaterial *) melee->material;
    CHECK(dm->texture == 0 && pm->texture == 0 && !pm->projection_uv && !pm->double_sided, "noVideoSkins: the dyes keep the body's texture, no projection");
    char *path = concat(assets_path(), "textures/melee/melee_20.png");
    const unsigned regular = path ? load_texture(path) : 0;
    free(path);
    CHECK(regular && mm->texture == regular && !mm->projection_uv, "noVideoSkins: the Mysterious Orb shows melee_20.png, no projection");
    skin_video_stats(NULL, &in_use, NULL);
    CHECK(in_use == 0, "noVideoSkins: no video in use");
    skin_fx_forget(dye);
    skin_fx_forget(proj);
    skin_fx_forget(melee);
    mesh_fini(dye);
    mesh_fini(proj);
    mesh_fini(melee);
    skin_video_set_disabled(0);
    skin_video_clear();
}

// v10.0.3 weapon altSkin remodels (viewer_weapon_alt_skin; weapons deob-10.0.3.js:112928-113568, upgradeWeapons
// :200322-201030): the table, and each remodel's model ("weapons/<src>_<mid>") and getReModel texture
// ("weapons/weapon_<weapon>_<midT || mid>") load from the content set; the upgraded ones play the altSkin video on it.
static void check_weapon_remodels(void) {
    static const struct {
        int id, upgraded, mid;
        const char *name, *model, *texture;
        float m_scl, y_off, z_off, charm_x;
        int no_snd, video;
    } want[] = {
        {5, 0, 0, "Nova Pump", "weapons/weapon_6_0", "weapons/weapon_6_38", 1.0f, -0.5f, 0.28f, -0.37f, 0, -1},
        {7, 0, 0, "Nova Semi", "weapons/weapon_8_0", "weapons/weapon_8_0", 0.0f, 0.0f, 0.0f, -0.3f, 0, -1},
        {9, 0, 0, "Nova Uzi", "weapons/weapon_10_0", "weapons/weapon_10_0", 0.0f, 0.0f, 0.0f, 0.0f, 0, -1},
        {11, 0, 5, "Nova Alien", "weapons/weapon_12_5", "weapons/weapon_12_5", 1.5f, -0.2f, 0.0f, -0.51f, 1, -1},
        {5, 1, 0, "Nova Pump", "weapons/weapon_6_0", "weapons/weapon_6_38", 1.0f, -0.5f, 0.28f, -0.37f, 0, 2},
        {7, 1, 0, "Nova Semi", "weapons/weapon_8_0", "weapons/weapon_8_0", 0.0f, 0.0f, 0.0f, -0.3f, 0, 2},
        {9, 1, 0, "Nova Uzi", "weapons/weapon_10_0", "weapons/weapon_10_0", 0.0f, 0.0f, 0.0f, 0.0f, 0, 2},
        {11, 1, 4, NULL, "weapons/weapon_12_4", "weapons/weapon_12_4", 0.0f, 0.0f, 0.0f, 0.0f, 0, -1}, // Cupids Blaster
    };
    int others = 0;
    for (int id = 0; id < 29; id++) {
        if (id == 5 || id == 7 || id == 9 || id == 11) continue;
        others += viewer_weapon_alt_skin(id, 0) != NULL || viewer_weapon_alt_skin(id, 1) != NULL;
    }
    CHECK(!others && !viewer_weapon_alt_skin(-1, 0) && !viewer_weapon_alt_skin(64, 1), "no other weapon swaps its model (%d)", others);
    skin_video_set_disabled(0);
    for (size_t i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        const ViewerAltSkin *alt = viewer_weapon_alt_skin(want[i].id, want[i].upgraded);
        const Weapon *weapon = rules_weapon(want[i].id);
        CHECK(alt && weapon && weapon->src, "weapon %d (upgraded %d): an altSkin remodel", want[i].id, want[i].upgraded);
        if (!alt || !weapon || !weapon->src) continue;
        const ViewerSkin *s = &alt->skin;
        char model[96], texture[96];
        snprintf(model, sizeof(model), "weapons/%s_%d", weapon->src, s->mid);
        snprintf(texture, sizeof(texture), "%s%d_%d", g_viewer_types[0], s->weapon, s->mid_t > 0 ? s->mid_t : s->mid); // getReModel
        CHECK(s->mid == want[i].mid && !strcmp(model, want[i].model) && !strcmp(texture, want[i].texture), "%s: %s / %s", want[i].model, model, texture);
        CHECK((want[i].name ? s->name && !strcmp(s->name, want[i].name) : !s->name) && s->type == 0 && s->weapon - 1 == want[i].id,
              "weapon %d: the item %s", want[i].id, s->name ? s->name : "(no name)");
        CHECK(s->m_scl == want[i].m_scl && s->y_off == want[i].y_off && s->z_off == want[i].z_off && s->x_off == 0.0f,
              "%s: mScl %g yOff %g zOff %g", want[i].model, s->m_scl, s->y_off, s->z_off);
        const int no_snd = (s->flags2 & VS2_NO_SND) != 0;
        CHECK(fabsf(alt->charm_m[0] - want[i].charm_x) < 1e-5f && no_snd == want[i].no_snd, "%s: charmMXOff %g, noSnd %d", want[i].model,
              alt->charm_m[0], no_snd);
        CHECK(!(s->flags2 & (VS2_VIDEO | VS2_PROJECTION)) && (!want[i].upgraded || skin_video_alt_skin(want[i].id) == want[i].video),
              "%s: the video is skin_video_alt_skin's (%d)", want[i].model, want[i].video);

        char rel[160];
        snprintf(rel, sizeof(rel), "models/%s.obj", model);
        char *path = concat(assets_path(), rel);
        Geometry *geometry = path ? load_obj_model(path, 0) : NULL;
        free(path);
        CHECK(geometry && geometry->index_count > 300, "%s.obj loads (%d indices)", model, geometry ? geometry->index_count : -1);
        snprintf(rel, sizeof(rel), "textures/%s.png", texture);
        path = concat(assets_path(), rel);
        const unsigned tex = path ? load_texture(path) : 0;
        free(path);
        CHECK(tex && tex != texture_checker(), "%s.png loads", texture);
        if (!geometry || want[i].video < 0) continue;

        // the upgraded remodel: its altSkin video in screen space (LinearFilter) over the remodel's own texture
        FILE *f = fopen("assets/videos/video_2.mp4.knv", "rb");
        if (!f) {
            printf("gltf_test: %s video skipped (no assets/videos/video_2.mp4.knv: make video-frames)\n", model);
            continue;
        }
        fclose(f);
        BasicMaterial *material = basic_material_init();
        material->double_sided = 0;
        material->texture = tex;
        Mesh *mesh = mesh_init(geometry, (Material *) material);
        const int playing = skin_fx_apply_video(mesh, skin_video_alt_skin(want[i].id), 1, 1);
        CHECK(playing && material->texture && material->texture != tex && material->projection_uv && material->double_sided,
              "%s upgraded: video_2 in screen space on the remodel", model);
        skin_fx_forget(mesh);
        mesh_fini(mesh);
    }
    skin_video_clear();
}

// shoot()'s shot sound (viewer_weapon_shot_sound, deob-10.0.3.js:196392-196405): weapon.sound + "_<mid>" for a remodel (a
// store skin, else the weapon's altSkin) unless noSnd, "_blast" for a full charge, the plain sound when upgraded; the
// names are networkSounds entries (deob:21141) and getSkinForLoadout finds the skin made for the weapon (deob:190800).
static void check_shot_sounds(void) {
    static const struct {
        const char *sound;
        int skin, weapon_id, upgraded, blast;
        const char *want;
        int suffixed, id; // network_sound_index(want), -1 = not in the table
    } cases[] = {
        {"weapon_6", -1, 5, 0, 0, "weapon_6_0", 1, 24},       // no skin: the Nova Pump altSkin
        {"weapon_8", -1, 7, 0, 0, "weapon_8_0", 1, 33},       // Nova Semi
        {"weapon_10", -1, 9, 0, 0, "weapon_10_0", 1, 34},     // Nova Uzi
        {"weapon_12", -1, 11, 0, 0, "weapon_12", 0, 17},      // Nova Alien: noSnd
        {"weapon_2", -1, 1, 0, 0, "weapon_2", 0, 7},          // no altSkin
        {"weapon_upgraded", -1, 5, 1, 0, "weapon_upgraded", 0, -2}, // upgraded: upgradeWeapons' own sound
        {"weapon_29", -1, 28, 0, 1, "weapon_29_blast", 0, -2},
        {"weapon_6", 887, 5, 0, 0, "weapon_6_0", 1, 24},      // Roadkill (mid 0)
        {"weapon_6", 4362, 5, 0, 0, "weapon_6", 0, 11},       // Gold Plated (mid 5, noSnd)
        {"weapon_1", 0, 0, 0, 0, "weapon_1", 0, 5},           // Arctic Hunt: a texture skin, no remodel, no altSkin
        {"weapon_6", 0, 5, 0, 0, "weapon_6", 0, 11},          // a skin of another weapon: {} (plain), not the altSkin
        {"weapon_6", 887, 5, 1, 0, "weapon_6", 0, 11},        // upgraded: no remodel sound
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char name[48];
        const int suffixed = viewer_weapon_shot_sound(cases[i].sound, viewer_skin(cases[i].skin), cases[i].weapon_id, cases[i].upgraded,
                                                      cases[i].blast, name, sizeof(name));
        const int id = network_sound_index(name);
        CHECK(!strcmp(name, cases[i].want) && suffixed == cases[i].suffixed && (cases[i].id == -2 ? id >= 0 : id == cases[i].id),
              "shot sound %zu: %s (%d, id %d), want %s", i, name, suffixed, id, cases[i].want);
    }
    const ViewerSkin *gold = viewer_skin(4362), *roadkill = viewer_skin(887);
    CHECK(gold && (gold->flags2 & VS2_NO_SND) && gold->mid == 5 && roadkill && !(roadkill->flags2 & VS2_NO_SND) && roadkill->mid == 0,
          "the store table's noSnd (Gold Plated 4362) and Roadkill 887");
    // every store remodel without noSnd names a networkSounds entry (v10.0.2's store: all but the two weapon-30 ones)
    int remodels = 0, named = 0;
    for (int i = 0; i < g_viewer_skin_count; i++) {
        const ViewerSkin *s = &g_viewer_skins[i];
        if (s->type != 0 || s->mid < 0 || (s->flags2 & VS2_NO_SND) || s->weapon < 1 || s->weapon > 29) continue;
        char sound[16], name[48];
        snprintf(sound, sizeof(sound), "weapon_%d", s->weapon);
        viewer_weapon_shot_sound(sound, s, s->weapon - 1, 0, 0, name, sizeof(name));
        remodels++;
        named += network_sound_index(name) >= 0;
    }
    CHECK(remodels > 200 && named == remodels, "store remodels' shot sounds in networkSounds: %d of %d", named, remodels);
    // getSkinForLoadout: the skin made for the slot's weapon, whichever of skins[0] / skins[1] it is
    CHECK(viewer_loadout_skin(0, 887, 5) == 887 && viewer_loadout_skin(887, 0, 0) == 0 && viewer_loadout_skin(887, 0, 5) == 887 &&
          viewer_loadout_skin(-1, -1, 5) == -1 && viewer_loadout_skin(0, -1, 5) == -1, "getSkinForLoadout");
}

// The other v10.0.3 skin sounds: getReloadSnd (deob-10.0.3.js:198751-198758), getKillSnd's item part (:198745-198749),
// the swap sound (:198263-198268), the inspect sound (:195531-195540), and the user-asset files of a custom skin's sounds.
static void check_skin_sounds(void) {
    char name[96];
    // reload: reload_<n>, a reloadSnd skin weapon.sound + "_<mid>_r_<n>" (by player.skins[loadoutIndex], no weapon test)
    viewer_skin_reload_sound("weapon_8", NULL, 1, name, sizeof(name));
    CHECK(!strcmp(name, "reload_1"), "reload: no skin (%s)", name);
    viewer_skin_reload_sound("weapon_8", viewer_skin(887), 2, name, sizeof(name));
    CHECK(!strcmp(name, "reload_2"), "reload: a skin without reloadSnd (%s)", name);
    const struct { int skin; const char *sound; const char *want1, *want2; } reloads[] = {
        {5684, "weapon_8", "weapon_8_5_r_1", "weapon_8_5_r_2"},     // Bondsman
        {7096, "weapon_8", "weapon_8_10_r_1", "weapon_8_10_r_2"},   // Ad Astra
        {7097, "weapon_5", "weapon_5_11_r_1", "weapon_5_11_r_2"},   // Monte Carlo
        {6908, "weapon_15", "weapon_15_7_r_1", "weapon_15_7_r_2"},  // SuQi Superfan Package
        {7094, "weapon_31", "weapon_31_1_r_1", "weapon_31_1_r_2"},  // MelodiQ
    };
    for (size_t i = 0; i < sizeof(reloads) / sizeof(reloads[0]); i++) {
        const ViewerSkin *s = viewer_skin(reloads[i].skin);
        char two[96];
        viewer_skin_reload_sound(reloads[i].sound, s, 1, name, sizeof(name));
        viewer_skin_reload_sound(reloads[i].sound, s, 2, two, sizeof(two));
        char path[160];
        snprintf(path, sizeof(path), "assets/sound/%s.mp3", name);
        FILE *f = fopen(path, "rb");
        if (f) fclose(f);
        CHECK(s && (s->flags2 & VS2_RELOAD_SND) && !strcmp(name, reloads[i].want1) && !strcmp(two, reloads[i].want2) && f,
              "reload: skin %d %s / %s (file %s)", reloads[i].skin, name, two, f ? "ok" : "missing");
    }
    // kill: a killSnd melee plays "melee_<id>" (the Combat Knife's sound is "melee"); a weapon skin never (no type)
    CHECK(viewer_skin_kill_sound("melee", viewer_skin(1402), name, sizeof(name)) && !strcmp(name, "melee_12") && network_sound_index(name) > 0,
          "kill: Heated Crowbar (%s)", name);
    CHECK(viewer_skin_kill_sound("melee", viewer_skin(4398), name, sizeof(name)) && !strcmp(name, "melee_65") && network_sound_index(name) > 0,
          "kill: Present Basher (%s)", name);
    CHECK(!viewer_skin_kill_sound("weapon_6", viewer_skin(887), name, sizeof(name)) && !viewer_skin_kill_sound("melee", NULL, name, sizeof(name)),
          "kill: a weapon skin, no skin: none");
    // swap: weapon.swapSnd (the Build Tool's paper_1) ?? skin.swapSnd, volume swapSndVol ?? 0.05
    float volume = 0.0f;
    CHECK(viewer_skin_swap_sound("paper_1", 0.0f, NULL, name, sizeof(name), &volume) && !strcmp(name, "paper_1") && volume == 0.05f,
          "swap: the Build Tool (%s %g)", name, volume);
    CHECK(viewer_skin_swap_sound(NULL, 0.0f, viewer_skin(7554), name, sizeof(name), &volume) && !strcmp(name, "melee_butterfly_equip") &&
          volume == 1.0f, "swap: Midnight (%s %g)", name, volume);
    CHECK(viewer_skin_swap_sound(NULL, 0.0f, viewer_skin(7893), name, sizeof(name), &volume) && !strcmp(name, "melee_dragonbfk_equip") &&
          volume == 0.05f, "swap: Wrym Crest, no swapSndVol (%s %g)", name, volume);
    CHECK(!viewer_skin_swap_sound(NULL, 0.0f, viewer_skin(887), name, sizeof(name), &volume), "swap: no swap sound");
    // inspect: inspS, inspSLoop
    int loop = 0;
    CHECK(viewer_skin_inspect_sound(viewer_skin(7094), name, sizeof(name), &loop) && !strcmp(name, "inspect_idol_music_loop") && loop,
          "inspect: MelodiQ loops (%s)", name);
    CHECK(viewer_skin_inspect_sound(viewer_skin(7097), name, sizeof(name), &loop) && !strcmp(name, "inspect_monte_carlo_spin") && !loop,
          "inspect: Monte Carlo (%s)", name);
    CHECK(!viewer_skin_inspect_sound(viewer_skin(887), name, sizeof(name), &loop), "inspect: none");
    // a custom skin's sound files are user assets (userAssetsUrl("skins/" + _id + ...), deob:225205)
    CHECK(user_asset_rel_ok("skins/64f0c1a2b3/sound.mp3") && user_asset_rel_ok("skins/64f0c1a2b3/sound_swap.mp3") &&
          user_asset_rel_ok("skins/a-b_c/sound_inspect.mp3") && !user_asset_rel_ok("skins/64f0/thumbnail.png") &&
          !user_asset_rel_ok("skins/../sound.mp3") && !user_asset_rel_ok("skins//sound.mp3") && user_asset_rel_ok("30431/sound.mp3"),
          "custom skin sounds as user assets");
}

static void make_dir(const char *path) {
#ifdef _WIN32
    _mkdir(path);
#else
    mkdir(path, 0755);
#endif
}

static void remove_dir(const char *path) {
#ifdef _WIN32
    _rmdir(path);
#else
    rmdir(path);
#endif
}

// A custom studio skin's model and texture (deob-10.0.3.js:193943-193944, :194610-194611, :194709-194710): user assets
// skins/<_id>/model and skins/<_id>/texture (a weapon's texture: skins/<skin index>/texture), loaded from the user-asset
// tree (cosmetic_asset_path); a fixture copy of a weapon model and texture stands in for the studio's files.
static void check_custom_skin_assets(void) {
    ViewerSkin hat = {.name = "Studio Hat", .type = 1, .id = -1, .pat = -1, .mid = -1, .mid_t = -1, .emiss = -1, .color = -1,
                      .flags = VS_CUSTOM_MODEL | VS_CUSTOM_TEXTURES, .custom_id = "kntestcustom"};
    char model[128], texture[160];
    cosmetic_asset_keys(&hat, model, sizeof(model), texture, sizeof(texture));
    CHECK(!strcmp(model, "user-assets/skins/kntestcustom/model") && !strcmp(texture, "user-assets/skins/kntestcustom/texture"),
          "custom hat keys (%s, %s)", model, texture);
    ViewerSkin knife = hat;
    knife.type = 3;
    knife.flags = VS_CUSTOM_TEXTURES;
    viewer_melee_keys(&knife, model, sizeof(model), texture, sizeof(texture));
    CHECK(!strcmp(model, "melee/melee_0") && !strcmp(texture, "user-assets/skins/kntestcustom/texture"),
          "custom melee: the knife model, the custom texture (%s, %s)", model, texture);
    hat.flags = VS_CUSTOM_MODEL;
    cosmetic_asset_keys(&hat, model, sizeof(model), texture, sizeof(texture));
    CHECK(!strcmp(texture, "skins/kntestcustom/model"), "custom model without customTextures: src as the texture, from textures/ (%s)", texture);
    char *ua = cosmetic_asset_path("models", "user-assets/skins/kntestcustom/model", "obj");
    char *regular = cosmetic_asset_path("models", "hats/hat_1", "obj");
    const char *base = assets_path();
    CHECK(ua && regular && !strcmp(ua + strlen(base), "user-assets/skins/kntestcustom/model.obj") && !strcmp(regular + strlen(base), "models/hats/hat_1.obj"),
          "cosmetic_asset_path (%s, %s)", ua ? ua : "-", regular ? regular : "-");
    CHECK(user_asset_rel_ok("skins/kntestcustom/model.obj") && user_asset_rel_ok("skins/10000009/texture.png") &&
          user_asset_rel_ok("skins/x/texture_e.png") && user_asset_rel_ok("skins/x/model.glb") && !user_asset_rel_ok("skins/x/model.fbx"),
          "custom skin models and textures are user assets");
    CHECK(viewer_skin(812) && (viewer_skin(812)->flags & VS_FLIP_ANIM) && !(viewer_skin(887)->flags & VS_FLIP_ANIM), "flipAnim: Steelfang 812");

    // the fixture: weapon_12_5.obj and weapon_12_5.png as the custom skin's model and texture
    char *dir = concat(assets_path(), "user-assets/skins/kntestcustom");
    char *src_model = concat(assets_path(), "models/weapons/weapon_12_5.obj"), *src_tex = concat(assets_path(), "textures/weapons/weapon_12_5.png");
    char *tex_path = cosmetic_asset_path("textures", "user-assets/skins/kntestcustom/texture", "png");
    unsigned char *data = NULL;
    size_t len = 0;
    int wrote = 0;
    if (dir && src_model && src_tex && ua && tex_path) {
        char *parent = concat(assets_path(), "user-assets/skins");
        if (parent) make_dir(parent);
        free(parent);
        make_dir(dir);
        FILE *f;
        if (!read_file(src_model, &data, &len) && (f = fopen(ua, "wb"))) wrote += fwrite(data, 1, len, f) == len, fclose(f);
        free(data);
        data = NULL;
        if (!read_file(src_tex, &data, &len) && (f = fopen(tex_path, "wb"))) wrote += fwrite(data, 1, len, f) == len, fclose(f);
        free(data);
    }
    Geometry *geometry = wrote == 2 ? load_obj_model(ua, 0) : NULL;
    const unsigned tex = wrote == 2 ? load_texture(tex_path) : 0;
    CHECK(wrote == 2 && geometry && geometry->index_count > 300 && tex && tex != texture_checker(), "the custom skin's model and texture load (%d)",
          geometry ? geometry->index_count : -1);
    if (ua) remove(ua);
    if (tex_path) remove(tex_path);
    if (dir) remove_dir(dir);
    char *skins_dir = concat(assets_path(), "user-assets/skins");
    if (skins_dir) remove_dir(skins_dir); // only when the fixture made it (a non-empty folder stays)
    free(skins_dir);
    free(dir), free(src_model), free(src_tex), free(tex_path), free(ua), free(regular);
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
    check_video_skins();
    check_weapon_remodels();
    check_shot_sounds();
    check_skin_sounds();
    check_custom_skin_assets();
    check_meshopt_models();
    check_material_extras();

    glfwDestroyWindow(window);
    glfwTerminate();
    printf(g_failures ? "gltf_test: %d of %d checks failed\n" : "gltf_test: OK (%2$d checks)\n", g_failures, g_checks);
    return g_failures ? 1 : 0;
}
