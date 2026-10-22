/*
 * services/brain/v10_skin_still.c - a video skin in the store table, sent as its still.
 *
 * The rule is the client's own (v10.0.3 getMat and the noVideoSkins drop in settings.quality): a record
 * with `video` set (0 counts) and no truthy `asset` is a video skin, and the drop removes `video`,
 * `projection` and `texSrc`. What is added is the still: the official thumbnail of each video skin
 * (data/skins.jspck, 10.0.3), named by the store's own preview paths:
 *   melee (type 3) with a texture:   /textures/previews/melee/melee_<id>_<tex>.png    (index 4211: melee_20_3)
 *   cosmetic (type 5):               /textures/previews/cosmetics/5_<id>.png          (index 4208: 5_21, 7446: 5_43)
 * A video skin of another type keeps no thumbnail, and the client shows its default preview.
 */
#include "v10_skin_still.h"

#include <stdio.h>
#include <string.h>

/* a JSON value the client reads as true: a true, a non-zero number, a non-empty string or a container */
static int truthy(const cJSON *v) {
    if (!v || cJSON_IsNull(v) || cJSON_IsFalse(v)) return 0;
    if (cJSON_IsNumber(v)) return v->valuedouble != 0;
    if (cJSON_IsString(v)) return v->valuestring && v->valuestring[0];
    return 1;
}

static int is_video_skin(const cJSON *r) {
    const cJSON *video = cJSON_GetObjectItemCaseSensitive(r, "video");
    return video && !cJSON_IsNull(video) && !truthy(cJSON_GetObjectItemCaseSensitive(r, "asset"));
}

/* the still's preview path, or 0 when the store's naming gives none for this record */
static int still_path(const cJSON *r, char *out, size_t n) {
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(r, "type");
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(r, "id");
    const cJSON *tex = cJSON_GetObjectItemCaseSensitive(r, "tex");
    if (!cJSON_IsNumber(type) || !cJSON_IsNumber(id)) return 0;
    if (type->valueint == 3 && cJSON_IsNumber(tex)) {
        snprintf(out, n, "/textures/previews/melee/melee_%d_%d.png", id->valueint, tex->valueint);
        return 1;
    }
    if (type->valueint == 5 && !tex) {
        snprintf(out, n, "/textures/previews/cosmetics/5_%d.png", id->valueint);
        return 1;
    }
    return 0;
}

int v10_skin_still(cJSON *skins) {
    if (!cJSON_IsArray(skins)) return 0;
    int n = 0;
    cJSON *r = NULL;
    cJSON_ArrayForEach(r, skins) {
        if (!cJSON_IsObject(r) || !is_video_skin(r)) continue;
        cJSON_DeleteItemFromObjectCaseSensitive(r, "video");
        cJSON_DeleteItemFromObjectCaseSensitive(r, "projection");
        cJSON_DeleteItemFromObjectCaseSensitive(r, "texSrc");
        char path[128];
        if (still_path(r, path, sizeof path)) {
            cJSON_DeleteItemFromObjectCaseSensitive(r, "thumbnail");
            cJSON_AddStringToObject(r, "thumbnail", path);
        }
        n++;
    }
    return n;
}
