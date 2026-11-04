// Box Office ray-tracing material classes (client/include/rt.h "material classes"): the table
// assets/materials/rt_materials.json, its lookup, and the per-BasicMaterial resolution cache.
//
// Krunker's content carries no material maps (one flat texture a model, flat colours on map cubes), so
// metalness / roughness / reflectivity come from a hand-authored table keyed by what the engine does know
// about a material: its paint shader (skin_fx.c), the weapon it is on, the model file it came from, the
// texture file it loads, or the player part it belongs to. The table applies only while ray tracing is on
// (RT_EFFECT_MATERIALS, rt_pass.c); rt_mode 0 never calls into here, so Krunker's frame is unchanged.
//
// Schema (tools/rt-materials/ authors the content):
//   { "classes": { name: { metalness, roughness, reflect, note } },
//     "textures": { "<base name or glob>": class }, "models": { "<assets-relative path glob>": class },
//     "weapons": { "<weapon id>": class }, "parts": { player_body|player_head|player_arms|player_legs|cosmetic: class },
//     "paint": { "<paintShader 0..5>": class }, "default": class | "" }
// Precedence: paint > weapons > models > textures > parts > default. Globs: `*` only.
#include <gl.h>
#include <rt.h>
#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RT_MAT_CLASSES_MAX 64
#define RT_MAT_RULES_MAX 1024
#define RT_MAT_PATTERN_MAX 96

typedef struct {
    char pattern[RT_MAT_PATTERN_MAX];
    int class_index;
} RtRule;

typedef struct {
    RtMaterialClass classes[RT_MAT_CLASSES_MAX];
    int class_count;
    RtRule textures[RT_MAT_RULES_MAX]; int texture_count;
    RtRule models[RT_MAT_RULES_MAX];   int model_count;
    int weapons[128];                  // weapon id -> class index + 1 (0 = none)
    int parts[6];                      // RT_PART_* -> class index + 1
    int paint[8];                      // paint shader -> class index + 1
    int default_class;                 // class index + 1
    int loaded;
    int tried;
    unsigned epoch;                    // bumps on every (re)load: cached resolutions are stale
} RtMaterialTable;

static RtMaterialTable g_mat;

// ---------------------------------------------------------------- table

static int class_index(const char *name) {
    if (!name || !*name) return -1;
    for (int i = 0; i < g_mat.class_count; i++) if (!strcmp(g_mat.classes[i].name, name)) return i;
    return -1;
}

static float json_num(const cJSON *object, const char *key, const float fallback) {
    const cJSON *v = cJSON_GetObjectItem(object, key);
    return cJSON_IsNumber(v) ? (float) v->valuedouble : fallback;
}

static float clampf(const float v, const float lo, const float hi) { return v < lo ? lo : v > hi ? hi : v; }

static void load_rules(const cJSON *object, RtRule *rules, int *count, const int max) {
    *count = 0;
    const cJSON *item;
    cJSON_ArrayForEach(item, object) {
        if (!cJSON_IsString(item) || !item->string || *count >= max) continue;
        const int c = class_index(item->valuestring);
        if (c < 0) continue; // "" or an unknown class: no rule
        snprintf(rules[*count].pattern, RT_MAT_PATTERN_MAX, "%s", item->string);
        rules[*count].class_index = c;
        (*count)++;
    }
}

static void load_indexed(const cJSON *object, int *slots, const int slot_count) {
    memset(slots, 0, (size_t) slot_count * sizeof(int));
    const cJSON *item;
    cJSON_ArrayForEach(item, object) {
        if (!cJSON_IsString(item) || !item->string) continue;
        const int c = class_index(item->valuestring);
        const int id = atoi(item->string);
        if (c < 0 || id < 0 || id >= slot_count) continue;
        slots[id] = c + 1;
    }
}

static int part_index(const char *name) {
    static const char *const NAMES[] = {"", "player_body", "player_head", "player_arms", "player_legs", "cosmetic"};
    for (int i = 1; i < 6; i++) if (!strcmp(NAMES[i], name)) return i;
    return 0;
}

int rt_materials_load(void) {
    g_mat.tried = 1;
    char *path = concat(assets_path(), "materials/rt_materials.json");
    if (!path) return 0;
    ensure_asset_file(path);
    unsigned char *text = NULL;
    size_t length = 0;
    const int missing = read_file(path, &text, &length);
    free(path);
    if (missing || !text) {
        fprintf(stderr, "[rt] materials: assets/materials/rt_materials.json not found (classes off)\n");
        return 0;
    }
    cJSON *root = cJSON_ParseWithLength((const char *) text, length);
    free(text);
    if (!root) {
        fprintf(stderr, "[rt] materials: rt_materials.json does not parse (classes off)\n");
        return 0;
    }
    RtMaterialTable fresh;
    memset(&fresh, 0, sizeof(fresh));
    fresh.tried = 1;
    fresh.epoch = g_mat.epoch + 1;
    const RtMaterialTable saved = g_mat;
    g_mat = fresh; // class_index() below reads g_mat while it fills

    const cJSON *classes = cJSON_GetObjectItem(root, "classes");
    const cJSON *item;
    cJSON_ArrayForEach(item, classes) {
        if (!cJSON_IsObject(item) || !item->string || g_mat.class_count >= RT_MAT_CLASSES_MAX) continue;
        RtMaterialClass *c = &g_mat.classes[g_mat.class_count++];
        snprintf(c->name, sizeof(c->name), "%s", item->string);
        c->metalness = clampf(json_num(item, "metalness", 0.0f), 0.0f, 1.0f);
        c->roughness = clampf(json_num(item, "roughness", 1.0f), 0.0f, 1.0f);
        c->reflect = clampf(json_num(item, "reflect", 0.0f), 0.0f, 4.0f);
        c->f0 = clampf(json_num(item, "f0", c->metalness >= 0.5f ? 0.45f : 0.0f), 0.0f, 1.0f);
        c->bump = clampf(json_num(item, "bump", 0.0f), 0.0f, 2.0f); // rt_normals.c relief strength (0 = none)
        const cJSON *detail = cJSON_GetObjectItem(item, "detail"); // Cinematic procedural detail (basic.frag krunker_rt_detail)
        const char *dv = cJSON_IsString(detail) ? detail->valuestring : "";
        c->detail = !strcmp(dv, "stone") ? RT_DETAIL_STONE : !strcmp(dv, "metal") ? RT_DETAIL_METAL : !strcmp(dv, "wood") ? RT_DETAIL_WOOD : RT_DETAIL_NONE;
    }
    load_rules(cJSON_GetObjectItem(root, "textures"), g_mat.textures, &g_mat.texture_count, RT_MAT_RULES_MAX);
    load_rules(cJSON_GetObjectItem(root, "models"), g_mat.models, &g_mat.model_count, RT_MAT_RULES_MAX);
    load_indexed(cJSON_GetObjectItem(root, "weapons"), g_mat.weapons, 128);
    load_indexed(cJSON_GetObjectItem(root, "paint"), g_mat.paint, 8);
    const cJSON *parts = cJSON_GetObjectItem(root, "parts");
    cJSON_ArrayForEach(item, parts) {
        if (!cJSON_IsString(item) || !item->string) continue;
        const int p = part_index(item->string), c = class_index(item->valuestring);
        if (p > 0 && c >= 0) g_mat.parts[p] = c + 1;
    }
    const cJSON *def = cJSON_GetObjectItem(root, "default");
    g_mat.default_class = cJSON_IsString(def) ? class_index(def->valuestring) + 1 : 0;
    cJSON_Delete(root);

    if (!g_mat.class_count) {
        fprintf(stderr, "[rt] materials: rt_materials.json has no classes (classes off)\n");
        g_mat = saved;
        g_mat.tried = 1;
        return 0;
    }
    g_mat.loaded = 1;
    int weapon_rules = 0, paint_rules = 0, part_rules = 0;
    for (int i = 0; i < 128; i++) weapon_rules += g_mat.weapons[i] != 0;
    for (int i = 0; i < 8; i++) paint_rules += g_mat.paint[i] != 0;
    for (int i = 0; i < 6; i++) part_rules += g_mat.parts[i] != 0;
    fprintf(stderr, "[rt] materials: %d classes, %d texture, %d model, %d weapon, %d part, %d paint rules, default %s\n",
            g_mat.class_count, g_mat.texture_count, g_mat.model_count, weapon_rules, part_rules, paint_rules,
            g_mat.default_class ? g_mat.classes[g_mat.default_class - 1].name : "none");
    return 1;
}

int rt_materials_loaded(void) {
    if (!g_mat.tried) rt_materials_load();
    return g_mat.loaded;
}

// ---------------------------------------------------------------- lookup

// `*` matches any run of characters (fnmatch without classes / `?`); case-sensitive
static int glob_match(const char *pattern, const char *text) {
    while (*pattern) {
        if (*pattern == '*') {
            while (*pattern == '*') pattern++;
            if (!*pattern) return 1;
            for (const char *t = text; ; t++) {
                if (glob_match(pattern, t)) return 1;
                if (!*t) return 0;
            }
        }
        if (*pattern != *text) return 0;
        pattern++;
        text++;
    }
    return !*text;
}

static int rules_match(const RtRule *rules, const int count, const char *text) {
    if (!text || !*text) return -1;
    for (int i = 0; i < count; i++) if (glob_match(rules[i].pattern, text)) return rules[i].class_index;
    return -1;
}

const RtMaterialClass *rt_materials_lookup(const RtMaterialKey *key) {
    if (!rt_materials_loaded() || !key) return NULL;
    int c = -1;
    if (key->paint >= 0 && key->paint < 8 && g_mat.paint[key->paint]) c = g_mat.paint[key->paint] - 1;
    if (c < 0 && key->weapon >= 0 && key->weapon < 128 && g_mat.weapons[key->weapon]) c = g_mat.weapons[key->weapon] - 1;
    if (c < 0) c = rules_match(g_mat.models, g_mat.model_count, key->model);
    if (c < 0) c = rules_match(g_mat.textures, g_mat.texture_count, key->texture);
    if (c < 0 && key->part > 0 && key->part < 6 && g_mat.parts[key->part]) c = g_mat.parts[key->part] - 1;
    if (c < 0 && g_mat.default_class) c = g_mat.default_class - 1;
    return c < 0 ? NULL : &g_mat.classes[c];
}

// ---------------------------------------------------------------- texture id -> base name

// The texture cache (loaders.c g_texture_cache) maps path -> GL id; the class rules want the base name.
// Reverse map built lazily, refreshed when the cache grew (loads add, nothing removes by id reuse before
// a map unload, after which rt_materials_resolve re-resolves anyway).
#define NAME rt_texname_map
#define KEY_TY unsigned int
#define VAL_TY char*
#include <verstable.h>

static rt_texname_map g_texnames;
static size_t g_texnames_cache_size;
static int g_texnames_init;

static void base_name(const char *path, char *out, const size_t n) {
    const char *slash = strrchr(path, '/');
#ifdef _WIN32
    const char *bslash = strrchr(path, '\\');
    if (bslash && (!slash || bslash > slash)) slash = bslash;
#endif
    const char *base = slash ? slash + 1 : path;
    snprintf(out, n, "%s", base);
    char *dot = strrchr(out, '.');
    if (dot && dot != out) *dot = '\0';
}

static const char *texture_name(const unsigned int texture) {
    if (!texture) return NULL;
    if (!g_texnames_init) { vt_init(&g_texnames); g_texnames_init = 1; }
    const size_t cache_size = vt_size(&g_texture_cache);
    if (cache_size != g_texnames_cache_size) {
        for (rt_texname_map_itr it = vt_first(&g_texnames); !vt_is_end(it); it = vt_next(it)) free(it.data->val);
        vt_cleanup(&g_texnames);
        vt_init(&g_texnames);
        for (texture_cache_map_itr it = vt_first(&g_texture_cache); !vt_is_end(it); it = vt_next(it)) {
            char base[RT_MAT_PATTERN_MAX];
            base_name(it.data->key, base, sizeof(base));
            vt_insert(&g_texnames, it.data->val, strdup(base));
        }
        g_texnames_cache_size = cache_size;
    }
    const rt_texname_map_itr found = vt_get(&g_texnames, texture);
    return vt_is_end(found) ? NULL : found.data->val;
}

// ---------------------------------------------------------------- per-material resolution

void rt_materials_hint_model(BasicMaterial *material, const char *model_path) {
    if (!material || !model_path) return;
    const char *rel = model_path;
    const char *assets = assets_path();
    const size_t assets_len = assets ? strlen(assets) : 0;
    if (assets_len && !strncmp(rel, assets, assets_len)) rel += assets_len; // "<assets>/models/x.obj" -> "models/x.obj"
    while (*rel == '/') rel++;
    snprintf(material->rt_hint_model, sizeof(material->rt_hint_model), "%s", rel);
    material->rt_resolved_paint = -2;
}

void rt_materials_hint_weapon(BasicMaterial *material, const int weapon_id) {
    if (!material) return;
    material->rt_hint_weapon = weapon_id >= 0 ? weapon_id + 1 : 0;
    material->rt_resolved_paint = -2;
}

void rt_materials_hint_part(BasicMaterial *material, const int part) {
    if (!material) return;
    material->rt_hint_part = part;
    material->rt_resolved_paint = -2;
}

void rt_materials_resolve(BasicMaterial *material) {
    if (!material) return;
    if (!rt_materials_loaded()) {
        material->rt_class = NULL;
        return;
    }
    const int paint = material->paint_enabled ? material->paint_shader : -1;
    if (material->rt_resolved_paint != -2 && material->rt_resolved_texture == material->texture &&
        material->rt_resolved_paint == paint && material->rt_resolved_epoch == g_mat.epoch)
        return; // cached
    material->rt_resolved_epoch = g_mat.epoch;
    material->rt_resolved_texture = material->texture;
    material->rt_resolved_paint = paint;
    material->rt_class = NULL;
    // a glTF material with its own metallicRoughness map is already physical: keep it (a part mask is ours)
    if (material->metal_rough_texture && !material->rt_mask) return;
    if (material->unlit || material->sprite) return;
    const RtMaterialKey key = {
        .texture = texture_name(material->texture),
        .model = material->rt_hint_model[0] ? material->rt_hint_model : NULL,
        .weapon = material->rt_hint_weapon - 1,
        .part = material->rt_hint_part,
        .paint = paint,
    };
    const RtMaterialClass *c = rt_materials_lookup(&key);
    static int log_resolutions = -1; // KRUNKNATIVE_RT_MATERIALS_LOG=1 (dev): print every resolution
    if (log_resolutions < 0) { const char *v = getenv("KRUNKNATIVE_RT_MATERIALS_LOG"); log_resolutions = v && *v && *v != '0'; }
    if (log_resolutions)
        fprintf(stderr, "[rt] material: texture %s model %s weapon %d part %d paint %d -> %s\n", key.texture ? key.texture : "-",
                key.model ? key.model : "-", key.weapon, key.part, key.paint, c ? c->name : "(none)");
    if (!c) return;
    material->rt_class = c;
    material->rt_metalness = c->metalness;
    material->rt_roughness = c->roughness;
    material->rt_reflect = c->reflect;
    material->rt_f0 = c->f0;
    material->rt_bump = c->bump;
}
