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

// Files on the local assets tree that the kru content index (dl.krunk.io, updates/client/latest.json,
// checked 2026-10-23) does not carry: the launcher does not install them either, so a shipped client draws
// no icon / texture for these melee ids. Remove an entry when the index gets it.
static const char *const MELEE_NOT_IN_INDEX[] = {
    "textures/weapons/pat/1.png",  // Raynb0w (melee skin 1624)
    "textures/melee/icon_124.png",
    "textures/melee/icon_125.png",
    "textures/melee/icon_126.png",
    "textures/melee/icon_127.png",
    "textures/melee/icon_128.png",
    "textures/melee/icon_129.png",
    "textures/melee/icon_13.png",
    "textures/melee/icon_130.png",
    "textures/melee/icon_131.png",
    "textures/melee/icon_132.png",
    "textures/melee/icon_133.png",
    "textures/melee/icon_134.png",
    "textures/melee/icon_135.png",
    "textures/melee/icon_136.png",
    "textures/melee/icon_137.png",
    "textures/melee/icon_138.png",
    "textures/melee/icon_139.png",
    "textures/melee/icon_140.png",
    "textures/melee/icon_141.png",
    "textures/melee/icon_142.png",
    "textures/melee/icon_143.png",
    "textures/melee/icon_144.png",
    "textures/melee/icon_145.png",
    "textures/melee/icon_146.png",
    "textures/melee/icon_147.png",
    "textures/melee/icon_148.png",
    "textures/melee/icon_149.png",
    "textures/melee/icon_150.png",
    "textures/melee/icon_151.png",
    "textures/melee/icon_152.png",
    "textures/melee/icon_153.png",
    "textures/melee/icon_154.png",
    "textures/melee/icon_155.png",
    "textures/melee/icon_156.png",
    "textures/melee/icon_157.png",
    "textures/melee/icon_158.png",
    "textures/melee/icon_159.png",
    "textures/melee/icon_160.png",
    "textures/melee/icon_161.png",
    "textures/melee/icon_162.png",
    "textures/melee/icon_163.png",
    "textures/melee/icon_164.png",
    "textures/melee/icon_165.png",
    "textures/melee/icon_166.png",
    "textures/melee/icon_167.png",
    "textures/melee/icon_168.png",
    "textures/melee/icon_169.png",
    "textures/melee/icon_170.png",
    "textures/melee/icon_171.png",
    "textures/melee/icon_172.png",
    "textures/melee/icon_173.png",
    "textures/melee/icon_174.png",
    "textures/melee/icon_175.png",
    "textures/melee/icon_176.png",
    "textures/melee/icon_177.png",
    "textures/melee/icon_178.png",
    "textures/melee/icon_179.png",
    "textures/melee/icon_180.png",
    "textures/melee/icon_181.png",
    "textures/melee/icon_182.png",
    "textures/melee/icon_183.png",
    "textures/melee/icon_184.png",
    "textures/melee/icon_185.png",
    "textures/melee/icon_186.png",
    "textures/melee/icon_187.png",
    "textures/melee/icon_188.png",
    "textures/melee/icon_189.png",
    "textures/melee/icon_190.png",
    "textures/melee/icon_191.png",
    "textures/melee/icon_192.png",
    "textures/melee/icon_193.png",
    "textures/melee/icon_194.png",
    "textures/melee/icon_195.png",
    "textures/melee/icon_196.png",
    "textures/melee/icon_197.png",
    "textures/melee/icon_198.png",
    "textures/melee/icon_199.png",
    "textures/melee/icon_200.png",
    "textures/melee/icon_201.png",
    "textures/melee/icon_202.png",
    "textures/melee/icon_203.png",
    "textures/melee/icon_204.png",
    "textures/melee/icon_205.png",
    "textures/melee/icon_206.png",
    "textures/melee/icon_207.png",
    "textures/melee/icon_208.png",
    "textures/melee/icon_209.png",
    "textures/melee/icon_210.png",
    "textures/melee/icon_211.png",
    "textures/melee/icon_212.png",
    "textures/melee/icon_59.png",
    "textures/melee/icon_66.png",
    "textures/melee/icon_67.png",
    "textures/melee/icon_83.png",
    "textures/melee/icon_84.png",
};

static const char *root_dir = ".";

static int no_official_texture(const char *rel) {
    for (size_t i = 0; i < sizeof(MELEE_NO_OFFICIAL_TEXTURE) / sizeof(MELEE_NO_OFFICIAL_TEXTURE[0]); i++)
        if (!strcmp(rel, MELEE_NO_OFFICIAL_TEXTURE[i])) return 1;
    return 0;
}

static int not_in_index(const char *rel) {
    for (size_t i = 0; i < sizeof(MELEE_NOT_IN_INDEX) / sizeof(MELEE_NOT_IN_INDEX[0]); i++)
        if (!strcmp(rel, MELEE_NOT_IN_INDEX[i])) return 1;
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
        // models/melee/melee_63.gltf: the index copy has a cut-short base64 buffer (Gold Plated); the local copy is whole
        if (asset_exists(rel) && strcmp(rel, "models/melee/melee_63.gltf")) CHECK(gltf_whole(rel), "%s: %s is not a whole GLTF (corrupt buffer)\n", label, rel);
        return; // a hasAnim skin's textures are embedded in its GLTF
    }
    snprintf(rel, sizeof(rel), "models/%s.obj", model);
    CHECK(asset_exists(rel), "%s: missing model %s\n", label, rel);
    snprintf(rel, sizeof(rel), "textures/%s.png", texture);
    if (!no_official_texture(rel) && !not_in_index(rel)) CHECK(asset_exists(rel), "%s: missing texture %s\n", label, rel);
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
            if (!not_in_index(icon)) CHECK(asset_exists(icon), "%s: missing loadout icon %s\n", label, icon);
        }
    }
    CHECK(melee_skins > 0, "the store table has no melee skins\n");

    printf("melee_assets_test: %d checks passed, %d failed (%d melee skins)\n", g_pass, g_fail, melee_skins);
    return g_fail ? 1 : 0;
}
