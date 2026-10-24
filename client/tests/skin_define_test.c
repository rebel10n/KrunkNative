// skin_define_test: v10.0.3 "sdc" SKIN_DEFINITION_CHANGED on the client (client/src/viewer/viewer_skin_define.c):
// store.skins[skinId] = skin (deob-10.0.3.js:261712) and the store's entry setup (iîíììïî :116530) as the ViewerSkin
// viewer_skin() returns from then on: the generator's field mapping (tools/ui-import/gen_viewer_store.js), BACK / HEAD
// assetType, free items out of the wheels, a redefinition, an index past the table, the dev edits over it.
#include "../src/viewer/viewer_store.h"
#include <cosmetics.h>
#include <cJSON.h>
#include <stdio.h>
#include <string.h>

static int g_failures, g_checks;
#define CHECK(cond, ...) do { g_checks++; \
    if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_failures++; } \
} while (0)

static int define(const int index, const char *json) {
    cJSON *skin = cJSON_Parse(json);
    const int ok = viewer_skin_define(index, skin);
    cJSON_Delete(skin); // the definition keeps its own copy
    return ok;
}

int main(void) {
    const ViewerSkin *row = viewer_skin(812);
    CHECK(row && !strcmp(row->name, "Steelfang"), "the table's 812 first");

    // a melee skin replaced: name, rarity, tex as a number, glow, a creator, offsets, a colour string
    CHECK(define(812, "{\"name\":\"Steelfang Redux\",\"type\":3,\"id\":3,\"rarity\":5,\"seas\":9,\"tex\":2,\"glow\":true,"
                      "\"creator\":\"Someone\",\"xOff\":-1.25,\"mScl\":\"0.5\",\"emiss\":\"#ff8800\",\"paintTargets\":[\"blade\",7],"
                      "\"hasAnim\":true,\"fileExt\":\"glb\"}"), "sdc 812 applied");
    const ViewerSkin *s = viewer_skin(812);
    CHECK(s && !strcmp(s->name, "Steelfang Redux") && s->type == ITEM_MELEE && s->id == 3 && s->rarity == 5 && s->seas == 9,
          "812: name, type, id, rarity, season");
    CHECK(s && s->tex && !strcmp(s->tex, "2") && (s->flags & VS_TEX_NUM) && (s->flags & VS_GLOW) && (s->flags & VS_HAS_ANIM) &&
          !(s->flags & VS_PAT), "812: tex 2 (a number), glow, hasAnim");
    CHECK(s && s->creator && !strcmp(s->creator, "Someone") && s->x_off == -1.25f && s->m_scl == 0.5f && s->emiss == 0xff8800 &&
          s->color == -1 && s->pat == -1 && s->mid == -1, "812: creator, xOff, mScl (a numeric string), emiss, defaults");
    CHECK(s && s->paint_targets && !strcmp(s->paint_targets, "blade,7") && (s->flags2 & VS2_PAINTABLE) && s->anim_s == 1.0f &&
          s->file_ext && !strcmp(s->file_ext, "glb"), "812: paintTargets joined, animS || 1, fileExt");

    // redefined again: the old strings go, the new ones stand
    CHECK(define(812, "{\"name\":\"Steelfang II\",\"type\":3,\"rarity\":2,\"free\":true}"), "sdc 812 again");
    s = viewer_skin(812);
    CHECK(s && !strcmp(s->name, "Steelfang II") && s->rarity == 2 && !s->creator && !s->tex && !s->paint_targets && s->emiss == -1,
          "812 redefined: only the new fields");
    CHECK(s && (s->flags & VS_FREE) && (s->flags & VS_NO_SPIN) && (s->flags & VS_NOT_IN_WHEEL), "free: noSpin, out of the wheels (:116554)");

    // BACK / HEAD take assetType 2 / 1 (:116533-116540); a paint's string id
    CHECK(define(9001, "{\"name\":\"New Cape\",\"type\":14}"), "a new index past the table");
    s = viewer_skin(9001);
    CHECK(s && !strcmp(s->name, "New Cape") && s->asset_type == 2 && s->seas == 0, "9001: BACK -> assetType 2 (season 1)");
    CHECK(define(9002, "{\"name\":\"Hood\",\"type\":15}") && viewer_skin(9002)->asset_type == 1, "9002: HEAD -> assetType 1");
    CHECK(define(9003, "{\"name\":\"Red\",\"type\":18,\"id\":\"red\",\"paintColor\":\"#ff0000\"}") && viewer_skin(9003)->id == -1 &&
          !strcmp(viewer_skin(9003)->id_name, "red") && !strcmp(viewer_skin(9003)->paint_color, "#ff0000"), "9003: a paint's named id");

    // refused: not an object, out of range; the dev edits still win over a definition
    cJSON *num = cJSON_CreateNumber(3);
    const ViewerSkin *before = viewer_skin(813);
    CHECK(!viewer_skin_define(813, num) && !viewer_skin_define(-1, NULL) && viewer_skin(813) == before, "a number is no skin: 813 unchanged");
    cJSON_Delete(num);
    CHECK(!viewer_skin_define_slot(VIEWER_SKIN_INDEX_MAX + 1), "an index past VIEWER_SKIN_INDEX_MAX");
    ViewerSkin *edit = viewer_skin_edit(812);
    CHECK(edit && !strcmp(edit->name, "Steelfang II"), "a dev edit starts from the definition");
    if (edit) edit->rarity = 6;
    CHECK(viewer_skin(812)->rarity == 6, "the edit stands over the definition");
    viewer_skin_edits_clear();
    CHECK(viewer_skin(812)->rarity == 2, "edits cleared: the definition again");

    printf("skin_define_test: %d/%d passed\n", g_checks - g_failures, g_checks);
    return g_failures ? 1 : 0;
}
