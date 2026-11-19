#pragma once
/* services/brain/asset_upload.c: the map editor's Publish Asset on a Box Office server (Krunker's
 * uploadA -> pubR -> POST <srv>/upload-user-asset, editor-10.0.7.js:20656-20806), stored where
 * GET /api/user-assets/<id>/<file> (user_assets.c) answers it. */
#include <stddef.h>

#include "cJSON.h"

struct Brain;
struct SocialSession;

int asset_upload_init(struct Brain *b);
void asset_upload_shutdown(struct Brain *b);
/* ["uploadA", name, desc, category, private, data, sizes, mType, typeCode, uvc] (social_ws.c) -> ["pubR", {fileId, srv}]
 * or ["upMp", reason] */
void asset_upload_begin(struct SocialSession *s, const cJSON *frame);
/* The stored file of a Box Office asset (rel "<id>/<file>", shared/include/user_asset_path.h): its path in out, 1 when
 * uploads are on (whether the file exists is the caller's question), 0 otherwise. */
int asset_upload_path(const char *rel, char *out, size_t cap);
