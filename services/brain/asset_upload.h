#pragma once
/* services/brain/asset_upload.c: the map editor's Publish Asset on a Box Office server (Krunker's
 * uploadA -> pubR -> POST <srv>/upload-user-asset, editor-10.0.7.js:20656-20806), stored where
 * GET /api/user-assets/<id>/<file> (user_assets.c) answers it; the mods' and the Skin Manager's files ride the same
 * tickets. */
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

struct Brain;
struct SocialSession;
typedef struct kn_http_req kn_http_req;

int asset_upload_init(struct Brain *b);
void asset_upload_shutdown(struct Brain *b);
/* ["uploadA", name, desc, category, private, data, sizes, mType, typeCode, uvc] (social_ws.c) -> ["pubR", {fileId, srv}]
 * or ["upMp", reason] */
void asset_upload_begin(struct SocialSession *s, const cJSON *frame);
/* The thumbnail upload of a map the editor's Publish Game (v10's uploadM) just published: a ticket for its
 * "thumb" (thumb bytes, 0 when no thumbnail was picked: the POST of the fileId alone finishes it), posted to
 * /upload-user-asset like an asset's files. 1 with the fileId (and *srv, uploads.public_host or NULL); 0 with *why. The
 * size limit (250 / 500 kB premium) is the caller's. */
int asset_upload_map_thumb_open(int64_t account, const char *username, int64_t map, const char *map_name, long long thumb,
                                char *file_id, size_t file_id_n, const char **srv, const char **why);
/* The stored file of a Box Office asset (rel "<id>/<file>", shared/include/user_asset_path.h): its path in out, 1 when
 * uploads are on (whether the file exists is the caller's question), 0 otherwise. */
int asset_upload_path(const char *rel, char *out, size_t cap);

/* The Skin Manager's files (skins.uploadAsset; skins.c): a ticket for one file of the account's custom skin <skin>
 * (asset_type model / texture / sound / thumbnail, the texture's or sound's category, the picked file's name for a
 * model's extension, its size). 1 with the ticket's fileId and the chunks' uploadUrl; 0 with a SKIN_UPLOAD_* code. */
int asset_upload_skin_open(int64_t account, const char *username, int64_t skin, const char *asset_type, const char *category,
                           const char *file_name, long long size, char *file_id, size_t file_id_n, char *url, size_t url_n,
                           const char **code);
/* v10.0.7's own FormData POST /skins/<skin>/upload of a signed-in account (v10_catalog.c), answered here */
void asset_upload_skin_form(kn_http_req *req, int64_t account, const char *username, int64_t skin);
/* a deleted skin's files go; a duplicate gets a copy of its original's */
void asset_upload_skin_purge(int64_t skin);
void asset_upload_skin_copy(int64_t from, int64_t to);
