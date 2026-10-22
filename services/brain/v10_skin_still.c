/*
 * services/brain/v10_skin_still.c - a video skin in the store table, sent as its still.
 *
 * The rule is the client's own (v10.0.3 getMat and the noVideoSkins drop in settings.quality): a record
 * with `video` set (0 counts) and no truthy `asset` is a video skin, and the drop removes `video`,
 * `projection` and `texSrc`. Its still is the skin's own texture `tex`. The thumbnail of every skin,
 * the still's preview included, is v10_skin_thumb.c's.
 */
#include "v10_skin_still.h"

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

int v10_skin_still(cJSON *skins) {
    if (!cJSON_IsArray(skins)) return 0;
    int n = 0;
    cJSON *r = NULL;
    cJSON_ArrayForEach(r, skins) {
        if (!cJSON_IsObject(r) || !is_video_skin(r)) continue;
        cJSON_DeleteItemFromObjectCaseSensitive(r, "video");
        cJSON_DeleteItemFromObjectCaseSensitive(r, "projection");
        cJSON_DeleteItemFromObjectCaseSensitive(r, "texSrc");
        n++;
    }
    return n;
}
