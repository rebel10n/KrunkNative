#pragma once
// Krunker user-asset files (https://user-assets.krunker.io/<id>/<file>, the asset tree's
// user-assets/<id>/<file>): which paths the client fetches on demand and the brain mirrors
// (client/src/net/asset_fetch.c, services/brain/user_assets.c). Pure, no state.
//
// <id> is an asset id (digits: textures, models, sounds, videos, prefabs), "md" + digits (a map's
// resource pack, mod.zip) or "m" + digits (a map thumbnail). <file> is one of the names Krunker's
// loaders ask for (render.js loadMesh / loadTexture / applyCompanionTextures, the editor's cards,
// loadModPack): texture.png, thumb.png, model.obj / .png / _e.png / _l.png / _n.png / _p.png (a paint mask) / .gltf /
// .glb, sound.mp3, video.mp4, data.json, mod.zip; and video.mp4.knv, the video's frames in KrunkNative's
// sidecar format (knv.h), which only the brain's mirror makes (KrunkNative has no video decoder).
#include <stddef.h>

#define USER_ASSET_HOST_URL "https://user-assets.krunker.io/"
#define USER_ASSET_DIR "user-assets/"

// rel below the user-asset host ("9356/texture.png", "md43867/mod.zip"): 1 when it is one.
int user_asset_rel_ok(const char *rel);
// rel below the asset tree ("user-assets/9356/texture.png"): 1 when it is a user-asset file.
int user_asset_tree_rel_ok(const char *tree_rel);
// A user-asset file this size may be (bytes): mod packs and models are the large ones.
long long user_asset_max_bytes(const char *rel);
// 1 when `data` (the first bytes of a download, len of them) can be the file `rel` names: an
// image for .png (PNG, JPEG, GIF, BMP or WebP bytes: Krunker serves what was uploaded), a zip for
// .zip, a glTF binary for .glb; never an HTML page (a CDN's block / error page answered 200).
int user_asset_content_ok(const char *rel, const unsigned char *data, size_t len);
