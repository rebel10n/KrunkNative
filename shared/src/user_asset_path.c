// Krunker user-asset file rules (shared/include/user_asset_path.h).
#include <user_asset_path.h>

#include <string.h>

static const char *const k_files[] = {
    "texture.png", "thumb.png", "model.obj", "model.png", "model_e.png", "model_l.png", "model_n.png",
    "model_p.png", // a paint's mask (paint.mask.src "<asset>/model_p": the Turf Wars banners, deob-10.0.3.js:214990)
    "model.gltf", "model.glb", "sound.mp3", "video.mp4", "data.json", "mod.zip",
    "video.mp4.knv", // not Krunker's: KrunkNative's frames of video.mp4 (knv.h), made by the brain's mirror
};

static int digits(const char *s, const size_t n) {
    if (!n || n > 12) return 0;
    for (size_t i = 0; i < n; i++)
        if (s[i] < '0' || s[i] > '9') return 0;
    return 1;
}

// A map studio's custom skin's files under skins/<folder>/ (folder: its _id, or for a weapon's texture its skin index;
// letters, digits, '_' or '-'): the model (customModel src "skins/" + _id + "/model" + fileExt, deob-10.0.3.js:194709,
// :233093), the texture and its glow map (customTextures texSrc ".../texture" + "_e", :194710, :231924), and the shot /
// swap / inspect sounds ("skins/" + _id, ".../sound_swap", ".../sound_inspect", :196407, :198265, :195533, :225205)
static int custom_skin_file(const char *rel) {
    static const char *const files[] = {"model.obj", "model.gltf", "model.glb", "texture.png", "texture_e.png",
                                        "sound.mp3", "sound_swap.mp3", "sound_inspect.mp3"};
    if (strncmp(rel, "skins/", 6) != 0) return 0;
    const char *id = rel + 6, *slash = strchr(id, '/');
    if (!slash || slash == id || slash - id > 64) return 0;
    for (const char *c = id; c < slash; c++) {
        const int ok = (*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || *c == '_' || *c == '-';
        if (!ok) return 0;
    }
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++)
        if (!strcmp(slash + 1, files[i])) return 1;
    return 0;
}

int user_asset_rel_ok(const char *rel) {
    if (!rel) return 0;
    if (custom_skin_file(rel)) return 1;
    const char *slash = strchr(rel, '/');
    if (!slash) return 0;
    const size_t n = (size_t) (slash - rel);
    const int id = digits(rel, n) || (n > 2 && !strncmp(rel, "md", 2) && digits(rel + 2, n - 2)) ||
                   (n > 1 && rel[0] == 'm' && digits(rel + 1, n - 1));
    if (!id) return 0;
    for (size_t i = 0; i < sizeof(k_files) / sizeof(k_files[0]); i++)
        if (!strcmp(slash + 1, k_files[i])) return 1;
    return 0;
}

int user_asset_tree_rel_ok(const char *tree_rel) {
    const size_t n = sizeof(USER_ASSET_DIR) - 1;
    return tree_rel && !strncmp(tree_rel, USER_ASSET_DIR, n) && user_asset_rel_ok(tree_rel + n);
}

static int ends_with(const char *s, const char *suffix) {
    const size_t a = strlen(s), b = strlen(suffix);
    return a >= b && !strcmp(s + a - b, suffix);
}

long long user_asset_max_bytes(const char *rel) {
    if (rel && (ends_with(rel, ".zip") || ends_with(rel, ".mp4") || ends_with(rel, ".knv"))) return 64ll << 20;
    return 32ll << 20; // Krunker's upload limit is far below (models: a few MB)
}

int user_asset_content_ok(const char *rel, const unsigned char *data, const size_t len) {
    if (!rel || !data) return 0;
    // an HTML page (Cloudflare's block page, a captive portal) is never an asset
    size_t i = 0;
    while (i < len && i < 64 && (data[i] == ' ' || data[i] == '\t' || data[i] == '\r' || data[i] == '\n')) i++;
    static const char *const html[] = {"<!doctype", "<html", "<head", "<!--"};
    for (size_t h = 0; h < sizeof(html) / sizeof(html[0]); h++) {
        const size_t hn = strlen(html[h]);
        if (len - i < hn) continue;
        size_t k = 0;
        while (k < hn && (data[i + k] | 0x20) == (unsigned char) (html[h][k] | 0x20)) k++;
        if (k == hn) return 0;
    }
    if (ends_with(rel, ".png")) {
        if (len >= 8 && !memcmp(data, "\x89PNG\r\n\x1a\n", 8)) return 1;
        if (len >= 3 && !memcmp(data, "\xff\xd8\xff", 3)) return 1;  // a JPEG uploaded as the texture
        if (len >= 6 && (!memcmp(data, "GIF87a", 6) || !memcmp(data, "GIF89a", 6))) return 1;
        if (len >= 2 && !memcmp(data, "BM", 2)) return 1;
        if (len >= 12 && !memcmp(data, "RIFF", 4) && !memcmp(data + 8, "WEBP", 4)) return 1;
        return 0;
    }
    if (ends_with(rel, ".zip")) return len >= 4 && !memcmp(data, "PK", 2);
    if (ends_with(rel, ".knv")) return len >= 4 && !memcmp(data, "KNVF", 4);
    if (ends_with(rel, ".glb")) return len >= 4 && !memcmp(data, "glTF", 4);
    return len > 0;
}
