#pragma once
// Map resource packs (client/src/mod_pack.c): Krunker v7.1.4's loadModPack for a map's "modURL"
// (src/index.js: at io init, when Game.map.maps[0].modURL is set and differs from the last one,
// loadModPack(modURL, true, true, ...)). The pack is a zip (zip.js there, client/src/zip_read.c
// here) whose entries replace the game's own files:
//   textures/<key>.png, img/<key>.png  RENDER.updateTexture(key): the texture textures/<key>.png
//   models/<key>.obj|.gltf|.glb|.fbx   RENDER.updateMesh(key) (not models/*event_0*)
//   sound/<name>.mp3                    SOUND.updateSound(name)
// KrunkNative extracts those entries (a temporary directory natively, MEMFS on the web), points
// the asset lookups at them (file_set_overlay: file_redirect, read_file, the texture / OBJ /
// GLTF loaders, the web's ensure_asset_file) and makes what is already loaded follow: cached
// textures and OBJ models are read again into the same GL objects, GLTF models are parsed again
// on their next use, sounds are decoded again.
//
// A player's mod (the menu's Mod Manager Load / loadUserMod, the Mod Loader's URL or file: client/src/ui/mods_ui.c)
// stays loaded until Reset Mods, as Krunker's loadedMods (deob-10.0.7.js:218795-218823).
//
// Documented differences from v7:
//   - A map's own modURL pack belongs to that map: it replaces the player's mod while the map is up, and the next
//     map without one brings the player's mod back (or the game's files when there is none).
//   - Only packs on the user-asset host load (user-assets.krunker.io/<path> and its old CDN
//     names: assets/user-assets/<path>, fetched through ensure_asset_file on the web). v7 also
//     loads Dropbox links; KrunkNative has no fetch outside its asset host, so those are skipped.
//   - textures/logo.png (the menu logo, v7 only with canChangeLogo), img/medals/*, scripts/
//     (settings.txt), css/ and css-img/ (page styles) are not applied.
//   - shaders/<name>.json (v10's post passes, client/include/mod_shader.h) go to hooks.shader in zip order once the
//     pack is active; they belong to the pack (mod_pack_clear resets them), where Krunker keeps appending passes
//     from every pack it loads until Reset Mods.
//   - The lookup is per file: a page image the HTML engine reads from the asset tree while a pack
//     is active (/assets/textures/..., e.g. store previews) takes the pack's copy too, where v7's
//     page keeps loading it from the asset host.
#include <stddef.h>

struct cJSON;

// After a map loaded (game_init): `map_json` is its raw JSON (Map.raw_data). Loads the map's
// pack when it differs from the active one; removes the active pack when the map has none (or
// one that cannot load).
void mod_pack_map_loaded(const struct cJSON *map_json);
// Loads a pack by URL (replacing the active one). 1 when it is active afterwards.
int mod_pack_load(const char *url);
// Removes the active pack: the game's own files come back (caches refreshed).
void mod_pack_clear(void);
// The player's mod (loadModPack from the Mod Manager / Mod Loader): loads it as mod_pack_load does and keeps it
// over map changes. 1 when it is active afterwards.
int mod_pack_load_user(const char *url);
// The Mod Loader's local .zip (loadMod: the picked file), kept as the player's mod. 1 when active afterwards.
int mod_pack_load_file(const char *path);
// windows[3].reset: forgets the player's mod and removes the active pack.
void mod_pack_reset(void);
const char *mod_pack_user_url(void); // the player's mod ("file:<path>" for a local zip), "" none
// Exit: deletes the extracted files (no cache refresh).
void mod_pack_shutdown(void);

const char *mod_pack_active_url(void); // "" when no pack is active
int mod_pack_file_count(void);         // overriding files of the active pack
// The file a pack URL is read from (assets_path() + "user-assets/<path>"), malloc'd; NULL when
// the URL is not on the user-asset host or not a safe path.
char *mod_pack_resolve_url(const char *url);

// How the loaded assets follow a pack coming or going: full asset paths (assets_path() + rel)
// for textures / OBJ / GLTF models, clip names for sounds. The client sets them at start-up
// (texture_cache_reload, geometry_cache_reload, gltf_model_forget, audio_reload); NULL = skip.
typedef struct {
    int (*texture)(const char *path);
    int (*geometry)(const char *path);
    void (*gltf)(const char *path);
    void (*sound)(const char *name);
    int (*shader)(const char *text, size_t len); // a shaders/ entry (mod_shader_insert_json): 1 when it added a pass
    void (*shaders_reset)(void);                 // the pack's passes go (mod_shader_reset)
} ModPackHooks;
void mod_pack_set_hooks(const ModPackHooks *hooks);
