/*
 * services/brain/v10_skin_thumb.c - the thumbnail of every skin in /data/skins (v10_skin_thumb.h).
 *
 * The rule is the v10.0.3 client's, written as JavaScript would evaluate it on a store record (the
 * values of assets/skins.json, which the store loop has already processed):
 *
 *   "/textures/" + P + "." + (prevExt || texExt || "png")
 *
 *   P = previews/ + cosmetics/<assetType||type>_<id>[_c<pat> | _<tex>]          type truthy, not 3
 *     = previews/ + <folder>[assetType||type||0] + tail                          otherwise
 *         tail (melee, type 3)  = <id>[_c<pat> | _<tex>]                         tex != null
 *         tail (weapon, others) = <weapon||0>_ + (mid != null ? m<mid>[_<midT>]
 *                                 : pat != null ? c<pat> : tex truthy ? <tex> : <id>)
 *   P = <folder>[type] + <id>                                                    type 4, 16, 17 or 18
 *
 * <folder> is the client's list of 19 item folders (weapons/weapon_, hats/hat_, ... paints/paint_). A
 * missing value reads as JavaScript's undefined ("undefined" in the text), null as "null". The store
 * loop sets texExt "webp" on types 16 and 17 before it computes the path, so they take ".webp" unless
 * prevExt says otherwise. Source and offsets: doc/v10/skin-thumbnails.md.
 */
#include "v10_skin_thumb.h"

#include <stdio.h>
#include <string.h>

/* itemTypes folders (deob-10.0.3.js 4794545) */
static const char *const FOLDERS[] = {
    "weapons/weapon_", "hats/hat_", "body/body_", "melee/melee_", "sprays/", "dyes/", "waist/waist_",
    "faces/face_", "shoes/shoe_", "pets/pet_", "collectibles/collect_", "wrist/wrist_", "charms/charm_",
    "tickets/ticket_", "back/back_", "head/head_", "playercards/card_", "emoji/emoji_", "paints/paint_",
};
#define FOLDER_COUNT ((int) (sizeof FOLDERS / sizeof FOLDERS[0]))

/* a value JavaScript reads as true: a true, a non-zero number, a non-empty string or an object */
static int truthy(const cJSON *v) {
    if (!v || cJSON_IsNull(v) || cJSON_IsFalse(v)) return 0;
    if (cJSON_IsNumber(v)) return v->valuedouble != 0;
    if (cJSON_IsString(v)) return v->valuestring && v->valuestring[0];
    return 1;
}

/* a key that is present and not null (JavaScript's `!= null`) */
static int present(const cJSON *v) {
    return v && !cJSON_IsNull(v);
}

static int is_num(const cJSON *v, double n) {
    return cJSON_IsNumber(v) && v->valuedouble == n;
}

/* the text of a path under construction; ok goes 0 when a piece does not fit */
struct pbuf {
    char *s;
    size_t cap, len;
    int ok;
};

static void pb_put(struct pbuf *b, const char *s) {
    const size_t n = strlen(s);
    if (!b->ok || b->len + n >= b->cap) {
        b->ok = 0;
        return;
    }
    memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = 0;
}

/* JavaScript's String() of a record value; a missing key is "undefined" */
static void pb_js(struct pbuf *b, const cJSON *v) {
    char num[64];
    if (!v) return pb_put(b, "undefined");
    if (cJSON_IsNull(v)) return pb_put(b, "null");
    if (cJSON_IsTrue(v)) return pb_put(b, "true");
    if (cJSON_IsFalse(v)) return pb_put(b, "false");
    if (cJSON_IsString(v)) return pb_put(b, v->valuestring ? v->valuestring : "");
    if (cJSON_IsNumber(v)) {
        if (v->valuedouble > -1e15 && v->valuedouble < 1e15 && v->valuedouble == (double) (long long) v->valuedouble) {
            snprintf(num, sizeof num, "%lld", (long long) v->valuedouble);
        }
        else snprintf(num, sizeof num, "%.15g", v->valuedouble);
        return pb_put(b, num);
    }
    pb_put(b, "[object Object]");
}

/* folder[idx] for a value used as an index: the folders are 0..18, anything else reads undefined */
static void pb_folder(struct pbuf *b, const cJSON *idx) {
    if (!cJSON_IsNumber(idx) || idx->valuedouble < 0 || idx->valuedouble >= FOLDER_COUNT || idx->valuedouble != (double) (int) idx->valuedouble) {
        return pb_put(b, "undefined");
    }
    pb_put(b, FOLDERS[(int) idx->valuedouble]);
}

static const cJSON *field(const cJSON *r, const char *key) {
    return cJSON_GetObjectItemCaseSensitive(r, key);
}

/* the thumbnail path of a record that is not a type 4, 16, 17 or 18 item (the "previews/" branch) */
static void pb_preview(struct pbuf *b, const cJSON *r) {
    const cJSON *type = field(r, "type"), *id = field(r, "id"), *weapon = field(r, "weapon");
    const cJSON *tex = field(r, "tex"), *pat = field(r, "pat"), *mid = field(r, "mid"), *midT = field(r, "midT");
    const cJSON *at = field(r, "assetType");
    pb_put(b, "previews/");
    if (truthy(type) && !is_num(type, 3)) {
        pb_put(b, "cosmetics/");
        pb_js(b, truthy(at) ? at : type);
        pb_put(b, "_");
        pb_js(b, id);
        if (present(pat)) {
            pb_put(b, "_c");
            pb_js(b, pat);
        } else if (truthy(tex)) {
            pb_put(b, "_");
            pb_js(b, tex);
        }
        return;
    }
    /* folder[assetType || type || 0] */
    const cJSON *idx = truthy(at) ? at : (truthy(type) ? type : NULL);
    if (idx) pb_folder(b, idx);
    else pb_put(b, FOLDERS[0]);
    if (truthy(type) && is_num(type, 3)) {
        pb_js(b, id);
        if (present(pat)) {
            pb_put(b, "_c");
            pb_js(b, pat);
        } else if (present(tex)) {
            pb_put(b, "_");
            pb_js(b, tex);
        }
        return;
    }
    if (truthy(weapon)) pb_js(b, weapon);
    else pb_put(b, "0");
    pb_put(b, "_");
    if (present(mid)) {
        pb_put(b, "m");
        pb_js(b, mid);
        if (present(midT)) {
            pb_put(b, "_");
            pb_js(b, midT);
        }
    } else if (present(pat)) {
        pb_put(b, "c");
        pb_js(b, pat);
    } else if (truthy(tex)) {
        pb_js(b, tex);
    } else {
        pb_js(b, id);
    }
}

int v10_skin_thumb_path(const cJSON *r, char *out, size_t cap) {
    if (!cJSON_IsObject(r) || !out || cap == 0) return 0;
    const cJSON *type = field(r, "type");
    const cJSON *prev = field(r, "prevExt"), *texExt = field(r, "texExt");
    struct pbuf b = {out, cap, 0, 1};
    out[0] = 0;
    pb_put(&b, "/textures/");
    if (!truthy(type) || !(is_num(type, 4) || is_num(type, 16) || is_num(type, 17) || is_num(type, 18))) {
        pb_preview(&b, r);
    } else {
        pb_folder(&b, type);
        pb_js(&b, field(r, "id"));
    }
    pb_put(&b, ".");
    if (truthy(prev)) pb_js(&b, prev);
    else if (is_num(type, 16) || is_num(type, 17)) pb_put(&b, "webp");
    else if (truthy(texExt)) pb_js(&b, texExt);
    else pb_put(&b, "png");
    return b.ok;
}

int v10_skin_thumbs(cJSON *skins) {
    if (!cJSON_IsArray(skins)) return 0;
    int n = 0;
    cJSON *r = NULL;
    cJSON_ArrayForEach(r, skins) {
        char path[512];
        if (!v10_skin_thumb_path(r, path, sizeof path)) continue;
        cJSON_DeleteItemFromObjectCaseSensitive(r, "thumbnail");
        cJSON_AddStringToObject(r, "thumbnail", path);
        n++;
    }
    return n;
}
