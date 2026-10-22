/* Unit test of the skin thumbnails (v10_skin_thumb.c): no database, no network.
 *
 * Part 1 pins the rule's path for one record of each branch. The records are Krunker v10.0.3's own
 * (data/skins.jspck, decoded; the same rows are in assets/skins.json), and each expected path is the
 * official record's thumbnail without its asset host, except the KrunkCup Jerseys row: the official
 * record ships ".png", the client's store loop gives ".webp" (prevExt), and only the .webp file is in
 * the asset tree (doc/v10/skin-thumbnails.md). The three video skins are checked after the still pass,
 * as /data/skins runs them.
 *
 * Part 2 runs the whole assets/skins.json table (KN_SKINS_JSON) through the still pass and the thumbnail
 * pass and checks that every record has a thumbnail equal to its rule path, and that the count of records
 * with one is the table's count, which is the official table's 8069 records. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "v10_skin_still.h"
#include "v10_skin_thumb.h"

static int g_fail;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)

/* the official table's size (data/skins.jspck, 10.0.3) */
#define OFFICIAL_SKINS 8069

struct pinned {
    const char *record;
    const char *want;
};

static const struct pinned PINNED[] = {
    {"{\"id\":0,\"name\":\"Arctic Hunt\",\"weapon\":1}", "/textures/previews/weapons/weapon_1_0.png"},
    {"{\"name\":\"Intervention XI\",\"weapon\":1,\"mid\":0,\"midT\":1}", "/textures/previews/weapons/weapon_1_m0_1.png"},
    {"{\"name\":\"Nova Pump\",\"weapon\":6,\"mid\":0,\"midT\":38}", "/textures/previews/weapons/weapon_6_m0_38.png"},
    {"{\"name\":\"x0n-voX\",\"weapon\":4,\"tex\":\"weapons/pat/0\",\"pat\":0}", "/textures/previews/weapons/weapon_4_c0.png"},
    {"{\"id\":7,\"name\":\"no weapon\"}", "/textures/previews/weapons/weapon_0_7.png"},
    {"{\"name\":\"mid, no midT\",\"weapon\":2,\"mid\":3}", "/textures/previews/weapons/weapon_2_m3.png"},
    {"{\"id\":5,\"name\":\"tex\",\"weapon\":3,\"tex\":\"abc\"}", "/textures/previews/weapons/weapon_3_abc.png"},
    {"{\"id\":1,\"name\":\"Top Hat\",\"type\":1}", "/textures/previews/cosmetics/1_1.png"},
    {"{\"id\":4,\"name\":\"Panda Body\",\"type\":2}", "/textures/previews/cosmetics/2_4.png"},
    {"{\"id\":0,\"name\":\"Hollow Fade\",\"type\":3,\"tex\":2}", "/textures/previews/melee/melee_0_2.png"},
    {"{\"id\":1,\"name\":\"Frostbite\",\"type\":3}", "/textures/previews/melee/melee_1.png"},
    {"{\"id\":39,\"name\":\"Terminal\",\"type\":4}", "/textures/sprays/39.png"},
    {"{\"id\":0,\"name\":\"Vantablack\",\"type\":5}", "/textures/previews/cosmetics/5_0.png"},
    {"{\"id\":12,\"name\":\"Ranked\",\"type\":12,\"tex\":\"weapons/pat/3\",\"pat\":3}", "/textures/previews/cosmetics/12_12_c3.png"},
    {"{\"id\":0,\"name\":\"Demonic Wings\",\"type\":14,\"assetType\":2}", "/textures/previews/cosmetics/2_0.png"},
    {"{\"id\":3,\"name\":\"Jack O' Lantern\",\"type\":15,\"assetType\":1}", "/textures/previews/cosmetics/1_3.png"},
    {"{\"id\":0,\"name\":\"Kanji\",\"type\":16,\"texExt\":\"webp\"}", "/textures/playercards/card_0.webp"},
    {"{\"id\":0,\"name\":\"Love\",\"type\":17,\"texExt\":\"webp\"}", "/textures/emoji/emoji_0.webp"},
    {"{\"id\":\"red\",\"name\":\"Red Paint\",\"type\":18,\"texExt\":\"webp\"}", "/textures/paints/paint_red.webp"},
    {"{\"id\":190,\"name\":\"KrunkCup Jerseys\",\"type\":2,\"tex\":40,\"prevExt\":\"webp\"}", "/textures/previews/cosmetics/2_190_40.webp"},
    /* the video skins (store indexes 4208, 4211, 7446), as the still pass leaves them */
    {"{\"id\":21,\"name\":\"Deep Space\",\"projection\":true,\"type\":5,\"video\":0}", "/textures/previews/cosmetics/5_21.png"},
    {"{\"id\":20,\"name\":\"Mysterious Orb\",\"projection\":true,\"type\":3,\"tex\":3,\"video\":0}", "/textures/previews/melee/melee_20_3.png"},
    {"{\"id\":43,\"name\":\"Glitch\",\"type\":5,\"video\":3}", "/textures/previews/cosmetics/5_43.png"},
};

static int str_is(const cJSON *r, const char *key, const char *want) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(r, key);
    return cJSON_IsString(v) && strcmp(v->valuestring, want) == 0;
}

static cJSON *load_skins(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    const long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buf = n > 0 ? malloc((size_t) n + 1) : NULL;
    cJSON *skins = NULL;
    if (buf && fread(buf, 1, (size_t) n, fp) == (size_t) n) {
        buf[n] = 0;
        skins = cJSON_Parse(buf);
    }
    free(buf);
    fclose(fp);
    return skins;
}

static void check_pinned(void) {
    const size_t n = sizeof PINNED / sizeof PINNED[0];
    size_t ok = 0;
    for (size_t i = 0; i < n; i++) {
        cJSON *r = cJSON_Parse(PINNED[i].record);
        CHECK(r != NULL);
        if (!r) continue;
        /* the video skins go through the still pass first, as /data/skins does */
        if (cJSON_GetObjectItemCaseSensitive(r, "video")) {
            cJSON *arr = cJSON_CreateArray();
            cJSON_AddItemReferenceToArray(arr, r);
            v10_skin_still(arr);
            cJSON_Delete(arr);
        }
        char path[512];
        const int got = v10_skin_thumb_path(r, path, sizeof path);
        if (got && strcmp(path, PINNED[i].want) == 0) ok++;
        else fprintf(stderr, "pinned %zu: got %s, want %s\n", i, got ? path : "(nothing)", PINNED[i].want);
        cJSON_Delete(r);
    }
    CHECK(ok == n);
    printf("v10_skin_thumb: %zu of %zu pinned records give their official thumbnail path\n", ok, n);
}

static void check_table(const char *file) {
    cJSON *skins = load_skins(file);
    CHECK(skins != NULL && cJSON_IsArray(skins));
    if (!skins || !cJSON_IsArray(skins)) return;
    const int size = cJSON_GetArraySize(skins);
    CHECK(size == OFFICIAL_SKINS);

    const int stills = v10_skin_still(skins); /* the three video skins, as /data/skins runs them */
    CHECK(stills == 3);
    const int given = v10_skin_thumbs(skins);
    CHECK(given == size);

    /* the expected path of each record, computed from the record before its thumbnail is set */
    int with = 0, equal = 0;
    cJSON *r = NULL;
    cJSON_ArrayForEach(r, skins) {
        char want[512];
        if (cJSON_GetObjectItemCaseSensitive(r, "thumbnail")) with++;
        if (v10_skin_thumb_path(r, want, sizeof want) && str_is(r, "thumbnail", want)) equal++;
    }
    CHECK(with == size);
    CHECK(equal == size);
    printf("v10_skin_thumb: %d of %d skin records carry a thumbnail; %d equal their rule path\n", with, size, equal);

    /* the thumbnail is a path under /textures/ and the three video skins keep the official still */
    cJSON_ArrayForEach(r, skins) {
        const cJSON *t = cJSON_GetObjectItemCaseSensitive(r, "thumbnail");
        if (t && cJSON_IsString(t)) CHECK(strncmp(t->valuestring, "/textures/", 10) == 0);
    }
    cJSON_ArrayForEach(r, skins) {
        const cJSON *ix = cJSON_GetObjectItemCaseSensitive(r, "index");
        if (!cJSON_IsNumber(ix)) continue;
        if (ix->valueint == 4208) CHECK(str_is(r, "thumbnail", "/textures/previews/cosmetics/5_21.png"));
        if (ix->valueint == 4211) CHECK(str_is(r, "thumbnail", "/textures/previews/melee/melee_20_3.png"));
        if (ix->valueint == 7446) CHECK(str_is(r, "thumbnail", "/textures/previews/cosmetics/5_43.png"));
    }

    /* a second pass changes nothing: the thumbnails are the rule's, set once */
    CHECK(v10_skin_thumbs(skins) == size);
    cJSON_Delete(skins);
}

int main(void) {
    check_pinned();
    check_table(KN_SKINS_JSON);
    if (g_fail) return 1;
    printf("v10_skin_thumb: ok\n");
    return 0;
}
