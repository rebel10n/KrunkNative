// melee_assets_test: every melee skin of the store table (viewer_store_table.c, the skins with type 3) names
// files the assets tree has, through the same keys the client loads (viewer_melee_keys, player_mesh.c):
//   - a hasAnim skin: models/<model>.<file_ext || gltf> exists and is a whole GLTF (cgltf parses it, and its
//     embedded base64 buffers are not cut short: a length % 4 == 1 buffer is the corrupt melee_63 of v7.1.4);
//   - any other melee skin: models/<model>.obj exists, and textures/<texture>.png exists unless the texture is
//     in MELEE_NO_OFFICIAL_TEXTURE (the official host 404s it: the skin draws the checker, as the client does);
//   - every melee id above 0 has its loadout icon textures/melee/icon_<id>.png;
//   - the default knife (melee_0) exists.
// A missing file fails the test. The allow list is the only exception, one line per texture with its reason.
//   melee_assets_test [repo-root]     (default "."; the root holds assets/)
#include "../src/viewer/viewer_store.h"

// cgltf: header-only here (gltf.c has its own copy of the parser)
#define CGLTF_IMPLEMENTATION
#include "../lib/cgltf/cgltf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail, g_pass;
#define CHECK(cond, ...) do { if (cond) g_pass++; else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// Textures the official assets do not have (404 on assets.krunker.io, checked 2026-10-22). Their skins
// draw the checker texture on the item's own model; the test reports nothing for them.
static const char *const MELEE_NO_OFFICIAL_TEXTURE[] = {
    "textures/melee/melee_10_2.png",  // Redux (melee 10, tex 2)
    "textures/melee/melee_20_2.png",  // Galaxys (melee 20, tex 2)
    "textures/melee/melee_20_3.png",  // Mysterious Orb (melee 20, tex 3)
    "textures/melee/melee_167.png",   // Genesis (Neon) Redux (melee 167)
};

static const char *root_dir = ".";

static int no_official_texture(const char *rel) {
    for (size_t i = 0; i < sizeof(MELEE_NO_OFFICIAL_TEXTURE) / sizeof(MELEE_NO_OFFICIAL_TEXTURE[0]); i++)
        if (!strcmp(rel, MELEE_NO_OFFICIAL_TEXTURE[i])) return 1;
    return 0;
}

// the asset file "assets/<rel>" exists and is readable
static int asset_exists(const char *rel) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/assets/%s", root_dir, rel);
    FILE *f = fopen(path, "rb");
    if (f) fclose(f);
    return f != NULL;
}

// a GLTF parses and every embedded base64 buffer is whole (length % 4 != 1)
static int gltf_whole(const char *rel) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/assets/%s", root_dir, rel);
    cgltf_options options = {0};
    cgltf_data *data = NULL;
    if (cgltf_parse_file(&options, path, &data) != cgltf_result_success) return 0;
    int ok = 1;
    for (size_t i = 0; i < data->buffers_count; i++) {
        const char *uri = data->buffers[i].uri;
        const char *comma = uri && !strncmp(uri, "data:", 5) ? strchr(uri, ',') : NULL;
        if (comma && strlen(comma + 1) % 4 == 1) ok = 0;
    }
    if (ok && cgltf_load_buffers(&options, data, path) != cgltf_result_success) ok = 0;
    cgltf_free(data);
    return ok;
}

// the model and texture of one melee skin (or the default knife, skin NULL) are present
static void check_melee(const ViewerSkin *skin, const char *label) {
    char model[64], texture[192], rel[256];
    viewer_melee_keys(skin, model, sizeof(model), texture, sizeof(texture));
    const int anim = skin && (skin->flags & VS_HAS_ANIM);
    if (anim) {
        snprintf(rel, sizeof(rel), "models/%s.%s", model, skin->file_ext ? skin->file_ext : "gltf");
        CHECK(asset_exists(rel), "%s: missing model %s\n", label, rel);
        if (asset_exists(rel)) CHECK(gltf_whole(rel), "%s: %s is not a whole GLTF (corrupt buffer)\n", label, rel);
        return; // a hasAnim skin's textures are embedded in its GLTF
    }
    snprintf(rel, sizeof(rel), "models/%s.obj", model);
    CHECK(asset_exists(rel), "%s: missing model %s\n", label, rel);
    // a graph shader skin (18, Ametwista) is its graph's material only and never loads its tex (getMat,
    // deob-10.0.5.js:204755-204758): assets.krunker.io has no melee_3_164.png either
    if (skin && (skin->flags2 & VS2_SHADER) && skin->shader_num == 18) return;
    snprintf(rel, sizeof(rel), "textures/%s.png", texture);
    if (!no_official_texture(rel)) CHECK(asset_exists(rel), "%s: missing texture %s\n", label, rel);
}

int main(int argc, char **argv) {
    if (argc > 1) root_dir = argv[1];

    check_melee(NULL, "melee_0 (default knife)");

    int melee_skins = 0;
    for (int i = 0; i < g_viewer_skin_count; i++) {
        const ViewerSkin *skin = viewer_skin(i);
        if (!skin || skin->type != 3) continue;
        melee_skins++;
        char label[128], icon[256];
        snprintf(label, sizeof(label), "melee skin %d \"%s\" (id %d)", i, skin->name ? skin->name : "?", skin->id);
        check_melee(skin, label);
        if (skin->id > 0) {
            snprintf(icon, sizeof(icon), "textures/melee/icon_%d.png", skin->id);
            CHECK(asset_exists(icon), "%s: missing loadout icon %s\n", label, icon);
        }
    }
    CHECK(melee_skins > 0, "the store table has no melee skins\n");

    printf("melee_assets_test: %d checks passed, %d failed (%d melee skins)\n", g_pass, g_fail, melee_skins);
    return g_fail ? 1 : 0;
}
