/*
 * services/brain/v10_skin_thumb.h - the thumbnail of every skin in /data/skins (Krunker v10.0.3's rule).
 *
 * The v10.0.3 store loop gives each skin its `thumbnail` from the skin's type, id, weapon, assetType, tex,
 * pat, mid, midT, prevExt and texExt (deob-10.0.3.js: path builder 1051172, store loop 4804884; the rule and
 * its offsets are in doc/v10/skin-thumbnails.md). The path is under /textures/ and the client prefixes the
 * asset host, so the table carries "/textures/previews/melee/melee_20_3.png" and the like. The official
 * table gives a thumbnail to all 8153 records (2026-10-30, v10.0.6); v10_skin_thumbs does the same for assets/skins.json.
 */
#ifndef KN_BRAIN_V10_SKIN_THUMB_H
#define KN_BRAIN_V10_SKIN_THUMB_H

#include <stddef.h>

#include "cJSON.h"

/* The rule's path of one store record into out (cap bytes, NUL-terminated). Returns 0 when r is not an
 * object or the path does not fit. */
int v10_skin_thumb_path(const cJSON *r, char *out, size_t cap);

/* Sets `thumbnail` on every record of the store array (replacing any value). Returns the number of records
 * given a thumbnail. */
int v10_skin_thumbs(cJSON *skins);

#endif
