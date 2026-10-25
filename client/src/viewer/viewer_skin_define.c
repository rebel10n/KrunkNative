// v10.0.3 "sdc" SKIN_DEFINITION_CHANGED (deob-10.0.3.js:261708-261714): the server replaces one store.skins entry at
// run time: store.skins[skinId] = skin, then the store's setup of an entry (iîíììïî :116530: index, seas || 1, BACK /
// HEAD assetType, creators, template, free). Here the JSON becomes the ViewerSkin viewer_skin() returns for that index
// (viewer_store.c viewer_skin_define_slot), field for field as tools/ui-import/gen_viewer_store.js builds the table
// rows. Not applied (documented in doc/v10/s2c-coverage.md): `template` (v10's template rows are folded into the
// generated table; a new definition naming one keeps its own fields only), a dye's colours (viewer_dye keeps the
// table's), and the lists built from the table (dropdowns, wheel pools) do not gain a new index.
#include "viewer_store.h"
#include <cosmetics.h>
#include <cJSON.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The JSON each defined index keeps (its strings are the ViewerSkin's) and the strings it had to build
typedef struct {
    int index;
    cJSON *json;
    char tex[32];          // a numeric tex as text
    char custom_id[32];    // a numeric _id as text
    char *paint_targets;   // paintTargets joined by ","
} Definition;

#define DEFINITIONS_MAX 256
static Definition g_defs[DEFINITIONS_MAX];
static int g_def_count;

static Definition *definition(const int index) {
    for (int i = 0; i < g_def_count; i++) {
        if (g_defs[i].index == index) return &g_defs[i];
    }
    if (g_def_count >= DEFINITIONS_MAX) return NULL;
    Definition *d = &g_defs[g_def_count++];
    memset(d, 0, sizeof(*d));
    d->index = index;
    return d;
}

static const cJSON *get(const cJSON *o, const char *key) { return cJSON_GetObjectItemCaseSensitive(o, key); }

// gen_viewer_store.js num(): a number, or a numeric string (v10's table has a few: "mScl": "0.0005")
static double num(const cJSON *v, const double def) {
    if (cJSON_IsNumber(v) && isfinite(v->valuedouble)) return v->valuedouble;
    if (cJSON_IsString(v) && v->valuestring[0]) {
        char *end = NULL;
        const double d = strtod(v->valuestring, &end);
        if (end && !*end && isfinite(d)) return d;
    }
    return def;
}

// JS truthiness ("false" as a string counts false, as the generator's truthy())
static int truthy(const cJSON *v) {
    if (!v || cJSON_IsNull(v) || cJSON_IsFalse(v)) return 0;
    if (cJSON_IsNumber(v)) return v->valuedouble != 0 && !isnan(v->valuedouble);
    if (cJSON_IsString(v)) return v->valuestring[0] && strcmp(v->valuestring, "false");
    return 1;
}

static int present(const cJSON *v) { return v && !cJSON_IsNull(v); } // != null

// gen_viewer_store.js col(): a number, or "#rrggbb" / "rrggbb"; -1 otherwise
static int col(const cJSON *v) {
    if (cJSON_IsNumber(v)) return (int) v->valuedouble;
    if (!cJSON_IsString(v)) return -1;
    const char *s = v->valuestring[0] == '#' ? v->valuestring + 1 : v->valuestring;
    if (strlen(s) != 6) return -1;
    char *end = NULL;
    const long c = strtol(s, &end, 16);
    return end && !*end ? (int) c : -1;
}

static const char *str(const cJSON *v) { return cJSON_IsString(v) && v->valuestring[0] ? v->valuestring : NULL; }

static float f(const cJSON *o, const char *key) { return (float) num(get(o, key), 0.0); }

static void fill(ViewerSkin *s, Definition *d, const cJSON *j) {
    memset(s, 0, sizeof(*s));
    s->name = cJSON_IsString(get(j, "name")) ? get(j, "name")->valuestring : "";
    s->type = (int) num(get(j, "type"), 0);
    s->weapon = (int) num(get(j, "weapon"), 0);
    const cJSON *id = get(j, "id");
    const int id_named = cJSON_IsString(id) && isnan(num(id, NAN));
    s->id = id_named ? -1 : (int) num(id, -1);
    if (id_named) s->id_name = id->valuestring; // paints: "red"
    s->rarity = (int) num(get(j, "rarity"), 0);
    s->seas = (int) num(get(j, "seas"), 0); // 0 = season 1 (`seas || 1`, :116532)
    const cJSON *tex = get(j, "tex");
    if (cJSON_IsString(tex)) s->tex = tex->valuestring;
    else if (cJSON_IsNumber(tex)) {
        snprintf(d->tex, sizeof(d->tex), "%.17g", tex->valuedouble);
        s->tex = d->tex;
    }
    s->pat = (int) num(get(j, "pat"), -1);
    s->mid = (int) num(get(j, "mid"), -1);
    s->mid_t = (int) num(get(j, "midT"), -1);
    const struct { const char *key; unsigned bit; } flags[] = {
        {"glow", VS_GLOW}, {"sameGlow", VS_SAME_GLOW}, {"noEmis", VS_NO_EMIS}, {"trans", VS_TRANS}, {"wire", VS_WIRE},
        {"blocked", VS_BLOCKED}, {"is3D", VS_IS_3D}, {"flip", VS_FLIP}, {"doubleSide", VS_DOUBLE_SIDE}, {"lowR", VS_LOW_R},
        {"noHead", VS_NO_HEAD}, {"noLegs", VS_NO_LEGS}, {"rgb", VS_RGB}, {"noSpin", VS_NO_SPIN}, {"free", VS_FREE},
        {"customModel", VS_CUSTOM_MODEL}, {"customTextures", VS_CUSTOM_TEXTURES}, {"flipAnim", VS_FLIP_ANIM},
        {"hasAnim", VS_HAS_ANIM},
    };
    for (size_t i = 0; i < sizeof(flags) / sizeof(flags[0]); i++)
        if (truthy(get(j, flags[i].key))) s->flags |= flags[i].bit;
    if (present(get(j, "pat"))) s->flags |= VS_PAT;
    if (cJSON_IsNumber(tex)) s->flags |= VS_TEX_NUM;
    // store.getSpnItems: limited / blackM / opensea / limT / limG never in a plain wheel's pool; a free item gets
    // limT "Free", noSale, noSpin (:116554-116558)
    if (truthy(get(j, "limited")) || truthy(get(j, "illicit")) || truthy(get(j, "limT")) || truthy(get(j, "blackM")) ||
        truthy(get(j, "opensea")) || truthy(get(j, "limG")) || truthy(get(j, "free")))
        s->flags |= VS_NOT_IN_WHEEL;
    if (truthy(get(j, "free"))) s->flags |= VS_NO_SPIN;
    s->creator = str(get(j, "creator"));
    // BACK -> assetType 2 (keyW "Back"), HEAD -> assetType 1 ("Head") (:116533-116540)
    s->asset_type = s->type == ITEM_BACK ? 2 : s->type == ITEM_HEAD ? 1 : (int) num(get(j, "assetType"), 0);
    s->tex_ext = str(get(j, "texExt"));
    s->prev_ext = str(get(j, "prevExt"));
    s->file_ext = str(get(j, "fileExt"));
    if (s->flags & VS_HAS_ANIM) s->anim_s = (float) num(get(j, "animS"), 1.0);
    if (truthy(get(j, "env")) || truthy(get(j, "sameEnv"))) s->env = (float) num(get(j, "env"), 3.0);
    s->x_off = f(j, "xOff"), s->y_off = f(j, "yOff"), s->z_off = f(j, "zOff");
    s->m_scl = f(j, "mScl"), s->scl = f(j, "scl"), s->spn_scl = f(j, "spnScl");
    s->x_rot = f(j, "xRot"), s->z_rota = f(j, "zRota"), s->scl_mlt = f(j, "sclMlt");
    s->sit_off = f(j, "sitOff"), s->sit_off_z = f(j, "sitOffZ");
    s->sx_off = f(j, "sxOff"), s->sy_off = f(j, "syOff"), s->sz_off = f(j, "szOff"), s->sx_rot = f(j, "sxRot");
    s->alpha = f(j, "alpha"), s->prev_rot = f(j, "prevRot"), s->tile_s = f(j, "tileS");
    s->opac = f(j, "opac"), s->opacity = f(j, "opacity");
    s->emiss = present(get(j, "emiss")) ? col(get(j, "emiss")) : -1;
    s->color = present(get(j, "color")) ? col(get(j, "color")) : -1;
    s->side = (int) num(get(j, "side"), 0);
    if (truthy(get(j, "frames"))) s->frames = (int) num(get(j, "frames"), 0), s->frame_t = (int) num(get(j, "frameT"), 0);
    if (truthy(get(j, "rAnim"))) {
        s->r_anim = f(j, "rAnim");
        const char *axis = str(get(j, "rAnimA"));
        s->r_anim_axis = (char) (axis ? (axis[0] | 0x20) : 'y'); // toLowerCase
    }
    s->mov_t = f(j, "movT");
    s->puls_t = cJSON_IsTrue(get(j, "pulsT")) ? 1.0f : f(j, "pulsT"); // one skin has pulsT: true
    const struct { const char *key; unsigned bit; } flags2[] = {
        {"noBody", VS2_NO_BODY}, {"noHair", VS2_NO_HAIR}, {"randomLeg", VS2_RANDOM_LEG}, {"noSleeve", VS2_NO_SLEEVE},
        {"noLeftSleeve", VS2_NO_LEFT_SLEEVE}, {"noRightHand", VS2_NO_RIGHT_HAND}, {"noCharm", VS2_NO_CHARM},
        {"projection", VS2_PROJECTION}, {"useSkin", VS2_USE_SKIN}, {"snd", VS2_SND}, {"killSnd", VS2_KILL_SND},
        {"stepSnd", VS2_STEP_SND}, {"jumpSnd", VS2_JUMP_SND}, {"reloadSnd", VS2_RELOAD_SND}, {"explode", VS2_EXPLODE},
        {"sameEnv", VS2_SAME_ENV}, {"forceAttach", VS2_FORCE_ATTACH}, {"tradable", VS2_TRADABLE},
        {"limitedSpin", VS2_LIMITED_SPIN}, {"premiumSpin", VS2_PREMIUM_SPIN}, {"paintTargets", VS2_PAINTABLE},
        {"noComp", VS2_NO_COMP}, {"noSnd", VS2_NO_SND}, {"inspSLoop", VS2_INSP_S_LOOP}, {"customSound", VS2_CUSTOM_SOUND},
        {"customSwapSound", VS2_CUSTOM_SWAP_SOUND}, {"customInspectSound", VS2_CUSTOM_INSPECT_SOUND},
    };
    for (size_t i = 0; i < sizeof(flags2) / sizeof(flags2[0]); i++)
        if (truthy(get(j, flags2[i].key))) s->flags2 |= flags2[i].bit;
    if (present(get(j, "shaderNum"))) s->flags2 |= VS2_SHADER, s->shader_num = (int) num(get(j, "shaderNum"), 0);
    if (present(get(j, "video"))) s->flags2 |= VS2_VIDEO, s->video = (int) num(get(j, "video"), 0);
    if (present(get(j, "particle"))) s->flags2 |= VS2_PARTICLE, s->particle = (int) num(get(j, "particle"), 0);
    if (present(get(j, "spraySnd"))) s->flags2 |= VS2_SPRAY_SND, s->spray_snd = (int) num(get(j, "spraySnd"), 0);
    s->desc = str(get(j, "desc"));
    s->paint_color = str(get(j, "paintColor"));
    const cJSON *mask = get(j, "paintMask");
    s->paint_mask = str(get(mask, "src"));
    const char *mask_ext = str(get(get(mask, "data"), "texExt"));
    if (s->paint_mask && mask_ext && strcmp(mask_ext, "png")) s->paint_mask_ext = mask_ext;
    const cJSON *targets = get(j, "paintTargets");
    if (cJSON_IsArray(targets)) { // paintTargets.map(String).join(",")
        size_t n = 1;
        const cJSON *t;
        cJSON_ArrayForEach(t, targets) n += (cJSON_IsString(t) ? strlen(t->valuestring) : 24) + 1;
        d->paint_targets = calloc(1, n);
        if (d->paint_targets) {
            size_t at = 0;
            cJSON_ArrayForEach(t, targets) {
                char part[32];
                const char *text = cJSON_IsString(t) ? t->valuestring : part;
                if (!cJSON_IsString(t)) snprintf(part, sizeof(part), "%.17g", num(t, 0));
                at += (size_t) snprintf(d->paint_targets + at, n - at, "%s%s", at ? "," : "", text);
            }
            s->paint_targets = d->paint_targets;
        }
    }
    s->title = (int) num(get(j, "title"), 0);
    s->set = str(get(j, "set"));
    if (s->set) s->set_min = (int) num(get(j, "setMin"), 0);
    s->leg_len = f(j, "legLen"), s->sit_off_x = f(j, "sitOffX"), s->sy_rot = f(j, "syRot"), s->sz_rot = f(j, "szRot");
    s->glow_int = f(j, "glowInt"), s->min_glo = f(j, "minGlo"), s->rgb_s = f(j, "rgbS"), s->rgb_t = f(j, "rgbT");
    s->r_anim_t = f(j, "rAnimT"), s->r_anim_m = f(j, "rAnimM"), s->mov_d = f(j, "movD"), s->mov_e = f(j, "movE");
    s->rot_t = f(j, "rotT"), s->spd = f(j, "spd");
    s->left_hold[0] = f(j, "leftHoldX"), s->left_hold[1] = f(j, "leftHoldY"), s->left_hold[2] = f(j, "leftHoldZ");
    s->right_hold[0] = f(j, "rightHoldX"), s->right_hold[1] = f(j, "rightHoldY"), s->right_hold[2] = f(j, "rightHoldZ");
    // v10.0.3 skin sounds: inspS / swapSnd / swapSndVol, and a custom skin's _id (its user-asset folder skins/<_id>/)
    s->insp_s = str(get(j, "inspS"));
    s->swap_snd = str(get(j, "swapSnd"));
    s->swap_snd_vol = f(j, "swapSndVol");
    const cJSON *custom_id = get(j, "_id");
    if (cJSON_IsString(custom_id) && custom_id->valuestring[0]) s->custom_id = custom_id->valuestring;
    else if (cJSON_IsNumber(custom_id)) {
        snprintf(d->custom_id, sizeof(d->custom_id), "%.17g", custom_id->valuedouble);
        s->custom_id = d->custom_id;
    }
}

int viewer_skin_define(const int index, const cJSON *skin) {
    if (!cJSON_IsObject(skin)) return 0;
    ViewerSkin *slot = viewer_skin_define_slot(index);
    Definition *d = slot ? definition(index) : NULL;
    if (!d) return 0;
    cJSON *json = cJSON_Duplicate(skin, 1);
    if (!json) return 0;
    cJSON *old_json = d->json;
    char *old_targets = d->paint_targets;
    d->json = json;
    d->paint_targets = NULL;
    fill(slot, d, json);
    cJSON_Delete(old_json); // the old strings went with the old definition
    free(old_targets);
    return 1;
}
