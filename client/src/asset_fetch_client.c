// Native on-demand game content fetch: the client's side (client/include/asset_fetch.h). Starts
// the fetch for assets_path(), answers ensure_asset_file() / asset_prefetch() through the shared
// hook, and once per frame hands the files that arrived to what loaded them while they were
// missing: load_texture's placeholder is filled (texture_cache_reload: same GL texture), an OBJ
// placeholder is rebuilt in place (geometry_cache_reload: every mesh holding it draws the model),
// GLTF models and sounds are read again on their next use, the HTML engine probes its missing
// page images again (asset_arrival_bump). The same hooks as a map resource pack (mod_pack.c).
#ifndef __EMSCRIPTEN__
#include <asset_fetch.h>
#include <audio.h>
#include <client.h>
#include <gltf.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int env_flag(const char *name, const int fallback) {
    const char *v = getenv(name);
    if (!v || !*v) return fallback;
    return strcmp(v, "0") != 0 && strcmp(v, "off") != 0 && strcmp(v, "false") != 0;
}

static int fetch_hook(const char *path, const int enqueue) {
    return asset_fetch_request(path, enqueue);
}

void asset_fetch_client_init(void) {
    const char *mode = getenv("KRUNKNATIVE_ASSET_FETCH");
    if (mode && (!strcmp(mode, "0") || !strcmp(mode, "off"))) return;
    const char *dir = assets_path();
    if (!dir || !*dir) return;
    const AssetFetchConfig cfg = {
        .assets_dir = dir,
        .origins = getenv("KRUNKNATIVE_ASSET_ORIGINS"),
        .allow_checkout = env_flag("KRUNKNATIVE_ASSET_FETCH_CHECKOUT", 0),
        // only a launcher-managed content dir (its content manifest) unless forced
        .require_manifest = !env_flag("KRUNKNATIVE_ASSET_FETCH", 0),
    };
    char why[4096 + 128] = "";
    if (!asset_fetch_start(&cfg, why, sizeof(why))) {
        // a dev tree or a test fixture: quiet unless the fetch was asked for
        if (mode || cfg.origins) fprintf(stderr, "[assets] on-demand fetch off: %s\n", why);
        return;
    }
    AssetFetchStats st;
    asset_fetch_stats(&st);
    printf("[assets] on-demand fetch for %s (%s)\n", dir, st.has_manifest ? "content manifest" : "no manifest: any missing file");
    asset_set_fetch_hook(fetch_hook);
}

// sound/<name>.mp3 -> <name>; user-assets/<id>/sound.mp3 -> asset:<id> (audio.c clip names)
static int sound_name(const char *rel, char *out, const size_t n) {
    const size_t len = strlen(rel);
    if (!strncmp(rel, "sound/", 6) && len > 10 && !strcmp(rel + len - 4, ".mp3")) {
        snprintf(out, n, "%.*s", (int) (len - 10), rel + 6);
        return 1;
    }
    if (!strncmp(rel, "user-assets/", 12) && len > 22 && !strcmp(rel + len - 10, "/sound.mp3")) {
        snprintf(out, n, "asset:%.*s", (int) (len - 22), rel + 12);
        return 1;
    }
    return 0;
}

static void arrived(void *user, const char *path) {
    (void) user;
    texture_cache_reload(path);  // a placeholder (or an earlier copy) gets the file's pixels
    geometry_cache_reload(path); // a placeholder model is rebuilt in place
    gltf_model_forget(path);     // parsed again on its next use
    const char *base = assets_path();
    const size_t blen = base ? strlen(base) : 0;
    char name[256];
    if (base && !strncmp(path, base, blen) && sound_name(path + blen, name, sizeof(name))) audio_reload(name);
}

void asset_fetch_client_frame(void) {
    if (asset_fetch_running() && asset_fetch_poll(arrived, NULL) > 0) asset_arrival_bump();
}

void asset_fetch_client_shutdown(void) {
    if (!asset_fetch_running()) return;
    asset_set_fetch_hook(NULL);
    asset_fetch_stop();
}
#endif
