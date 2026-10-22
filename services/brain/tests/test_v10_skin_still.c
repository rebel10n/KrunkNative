/* Unit test of the video-skin still (v10_skin_still.c): no database, no network.
 *
 * The three video skins are the records of Krunker v10.0.3's skins table (data/skins.jspck, decoded with
 * the bundle's own unpacker; the same records are in assets/skins.json): store index 4208 "Deep Space",
 * 4211 "Mysterious Orb" and 7446 "Glitch", with their official thumbnails. The plain skin and the user-asset
 * skin are controls that must come out unchanged. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "v10_skin_still.h"

static int g_fail;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)

static const char *FIXTURE =
    "["
    "{\"id\":1,\"name\":\"Wood\",\"tradable\":true,\"type\":2,\"tex\":4,\"index\":1},"
    "{\"id\":21,\"name\":\"Deep Space\",\"projection\":true,\"rarity\":6,\"seas\":5,\"tradable\":true,\"type\":5,"
    "\"video\":0,\"thumbnail\":\"https://assets.krunker.io/textures/previews/cosmetics/5_21.png\",\"index\":4208},"
    "{\"id\":20,\"keyW\":\"Knife\",\"name\":\"Mysterious Orb\",\"projection\":true,\"rarity\":6,\"tex\":3,\"tradable\":true,"
    "\"type\":3,\"video\":0,\"yOff\":-2,\"index\":4211},"
    "{\"creator\":\"Lednevir\",\"id\":43,\"name\":\"Glitch\",\"rarity\":6,\"seas\":9,\"tradable\":true,\"type\":5,"
    "\"video\":3,\"index\":7446,\"creators\":[\"lednevir\"]},"
    "{\"id\":99,\"name\":\"Custom\",\"video\":2,\"asset\":true,\"projection\":true,\"index\":9000}"
    "]";

static const cJSON *rec_at(const cJSON *arr, int store_index) {
    const cJSON *r = NULL;
    cJSON_ArrayForEach(r, arr) {
        const cJSON *ix = cJSON_GetObjectItemCaseSensitive(r, "index");
        if (cJSON_IsNumber(ix) && ix->valueint == store_index) return r;
    }
    return NULL;
}

static int str_is(const cJSON *r, const char *key, const char *want) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(r, key);
    return cJSON_IsString(v) && strcmp(v->valuestring, want) == 0;
}

static int has(const cJSON *r, const char *key) {
    return cJSON_GetObjectItemCaseSensitive(r, key) != NULL;
}

static char *dump(const cJSON *r) {
    return r ? cJSON_PrintUnformatted(r) : NULL;
}

int main(void) {
    cJSON *skins = cJSON_Parse(FIXTURE);
    CHECK(skins != NULL);
    if (!skins) return 1;
    cJSON *before = cJSON_Duplicate(skins, 1);
    CHECK(before != NULL);

    CHECK(v10_skin_still(skins) == 3); /* the three video skins, not the control records */

    const cJSON *deep = rec_at(skins, 4208);
    const cJSON *orb = rec_at(skins, 4211);
    const cJSON *glitch = rec_at(skins, 7446);
    CHECK(deep && orb && glitch);
    if (deep && orb && glitch) {
        /* the video is not sent, and neither is the projection that plays on it */
        CHECK(!has(deep, "video") && !has(deep, "projection"));
        CHECK(!has(orb, "video") && !has(orb, "projection"));
        CHECK(!has(glitch, "video"));
        /* the thumbnail is not this pass's: it keeps what the row has, and every skin's comes from
         * v10_skin_thumb (its test) */
        CHECK(str_is(deep, "thumbnail", "https://assets.krunker.io/textures/previews/cosmetics/5_21.png"));
        CHECK(!has(orb, "thumbnail") && !has(glitch, "thumbnail"));
        /* the skin's own texture stays: the still a video skin falls back to */
        const cJSON *tex = cJSON_GetObjectItemCaseSensitive(orb, "tex");
        CHECK(cJSON_IsNumber(tex) && tex->valueint == 3);
        /* the rest of the record is kept */
        CHECK(str_is(orb, "name", "Mysterious Orb") && str_is(orb, "keyW", "Knife"));
        const cJSON *yoff = cJSON_GetObjectItemCaseSensitive(orb, "yOff");
        CHECK(cJSON_IsNumber(yoff) && yoff->valueint == -2);
        const cJSON *creators = cJSON_GetObjectItemCaseSensitive(glitch, "creators");
        CHECK(cJSON_IsArray(creators) && cJSON_GetArraySize(creators) == 1);
    }

    /* the controls: a plain skin and a user-asset skin are byte-identical to before */
    char *a = dump(rec_at(before, 1)), *b = dump(rec_at(skins, 1));
    CHECK(a && b && strcmp(a, b) == 0);
    free(a);
    free(b);
    a = dump(rec_at(before, 9000));
    b = dump(rec_at(skins, 9000));
    CHECK(a && b && strcmp(a, b) == 0);
    free(a);
    free(b);

    /* a second pass finds nothing left to send as a video */
    CHECK(v10_skin_still(skins) == 0);

    cJSON_Delete(before);
    cJSON_Delete(skins);
    if (g_fail) return 1;
    printf("v10_skin_still: ok (3 video skins sent as stills; plain and asset skins unchanged)\n");
    return 0;
}
