// Per-weapon metal / roughness masks for the ray-traced material pass (client/include/rt.h "masks",
// doc/raytracing.md). A weapon / melee model's texture is classified per texel into metal, painted
// metal, wood, polymer and plastic from its colour, and the result is bound as the material's
// metallicRoughness map (R unused, G = roughness, B = metalness; basic.frag multiplies it into
// metalness_factor / roughness_factor on BASIC_METAL_ROUGHNESS_TEXTURE_UNIT). So one gun gets a
// metal barrel and receiver and a wood / rubber grip and stock.
//
// Two sources, in order:
//   1. "textures/<model key>_mr.png": a hand-painted (or tools/rt-materials/weapon_masks.py generated)
//      mask next to the texture. An override: Krunker's texture directories are not shipped content
//      of this repository, so nothing relies on it.
//   2. Built here, at run time, from the base texture the weapon already loaded: the same HSV rules, the
//      same 5x5 majority filter and the same assets/materials/weapon_masks.json overrides as
//      weapon_masks.py (the Python tool is the reference and the sheet viewer; keep the three in step).
//      On the GPU by default: assets/shaders/rt_mask_classify.frag + rt_mask_filter.frag, two fullscreen
//      passes into a new texture, ~0.2 ms of CPU and no readback (the programs compile once, on the
//      first mask). The CPU classifier below (glReadPixels + the same rules, ~12 ms for 1024x1024) is
//      the fallback when the programs do not compile, and the reference (KRUNKNATIVE_RT_MASK_CPU=1).
//      Once per model key, lazily on the first mesh that needs it; the result is bit-identical to the
//      Python tool's full-resolution mask on both paths.
//
// The mask belongs to the MODEL (its UV layout), not to the texture drawn on it: a store skin that
// swaps the texture keeps the base model's mask, a remodel ("weapons/weapon_1_3") has its own.
// Nothing here runs with rtMode 0: no file is looked for and metal_rough_texture stays 0, so the
// default frame is byte-identical. A mesh built while ray tracing was off keeps no mask until it is
// rebuilt (respawn, class / skin change, map load).
//
// Dev knobs: KRUNKNATIVE_RT_NO_MASKS=1 (class factors alone), KRUNKNATIVE_RT_MASK_CPU=1 (the CPU classifier),
// KRUNKNATIVE_RT_MASK_DUMP=<dir> (every built mask also goes to <dir>/<base>_mr.png, top row first, to diff
// against the Python tool's: `weapon_masks.py --only <base> --max-size 0`).
#include <gl.h>
#include <rt.h>
#include <render_settings.h>
#include <cosmetics.h>   // cosmetic_asset_path: "textures/<key>.png" (user-asset keys stay as they are)
#include <png_write.h>   // the dump knob
#include <stb_image.h>
#include <cJSON.h>
#include <GLFW/glfw3.h>  // glfwGetTime: the build-time log line
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------- the classifier (= weapon_masks.py)

enum { CLS_METAL, CLS_PAINTED, CLS_WOOD, CLS_POLYMER, CLS_PLASTIC, CLS_COUNT };
static const char *const CLASS_NAMES[CLS_COUNT] = {"metal", "painted", "wood", "polymer", "plastic"};
// (G roughness, B metalness) per class. Metalness tops out at 200 (0.78): a metal texel keeps a fifth of
// its diffuse, so a gun whose pixels get no traced reflection reads as dark worn metal, not black.
static const unsigned char CLASS_VALUES[CLS_COUNT][2] = {{90, 200}, {140, 100}, {190, 0}, {110, 0}, {100, 0}};

typedef struct {
    int cls;
    float hue0, hue1;  // degrees; hue0 > hue1 wraps (plastic: 50..360 or 0..5); < 0 = no hue test
    float sat_min, sat_max, val_min, val_max; // < 0 = no test (sat_max / val_max are exclusive)
} MaskRule;

#define MASK_RULES_MAX 16
#define MASK_RECTS_MAX 16
typedef struct {
    int cls;
    float x0, y0, x1, y1; // UV, top-left origin like the image
} MaskRect;

typedef struct {
    MaskRule rules[MASK_RULES_MAX];
    int rule_count;
    MaskRect rects[MASK_RECTS_MAX];
    int rect_count;
    int default_cls;
    unsigned char values[CLS_COUNT][2];
} MaskSpec;

static const MaskRule DEFAULT_RULES[] = {
    {CLS_WOOD, 8.0f, 50.0f, 0.30f, -1.0f, 0.18f, -1.0f},
    {CLS_POLYMER, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, 0.05f},
    {CLS_PLASTIC, 50.0f, 5.0f, 0.55f, -1.0f, 0.18f, -1.0f},
    {CLS_PAINTED, -1.0f, -1.0f, 0.14f, 0.55f, 0.22f, -1.0f},
};

static void spec_defaults(MaskSpec *spec) {
    memset(spec, 0, sizeof(*spec));
    memcpy(spec->rules, DEFAULT_RULES, sizeof(DEFAULT_RULES));
    spec->rule_count = (int) (sizeof(DEFAULT_RULES) / sizeof(DEFAULT_RULES[0]));
    spec->default_cls = CLS_METAL;
    memcpy(spec->values, CLASS_VALUES, sizeof(CLASS_VALUES));
}

static int class_by_name(const char *name) {
    for (int i = 0; i < CLS_COUNT; i++) if (name && !strcmp(name, CLASS_NAMES[i])) return i;
    return -1;
}

// PIL's RGB -> HSV (libImaging Convert.c rgb2hsv): h and s as the bytes the Python tool read back.
static void rgb_to_hsv_pil(const unsigned char r, const unsigned char g, const unsigned char b, int *h8, int *s8, int *v8) {
    const int maxc = r > g ? (r > b ? r : b) : (g > b ? g : b);
    const int minc = r < g ? (r < b ? r : b) : (g < b ? g : b);
    *v8 = maxc;
    if (minc == maxc) {
        *h8 = 0;
        *s8 = 0;
        return;
    }
    const float cr = (float) (maxc - minc);
    *s8 = (int) (cr * 255.0f / (float) maxc);
    const float rc = (float) (maxc - r) / cr, gc = (float) (maxc - g) / cr, bc = (float) (maxc - b) / cr;
    float h;
    if (r == maxc) h = bc - gc;
    else if (g == maxc) h = 2.0f + rc - bc;
    else h = 4.0f + gc - rc;
    h = h / 6.0f + 1.0f;
    h -= floorf(h); // fmod(h, 1) of a positive value
    *h8 = (int) (h * 255.0f);
}

static int rule_matches(const MaskRule *rule, const float hue, const float sat, const float val) {
    if (rule->hue0 >= 0.0f) {
        const int in = rule->hue0 <= rule->hue1 ? (hue >= rule->hue0 && hue <= rule->hue1) : (hue >= rule->hue0 || hue <= rule->hue1);
        if (!in) return 0;
    }
    if (rule->sat_min >= 0.0f && sat < rule->sat_min) return 0;
    if (rule->sat_max >= 0.0f && sat >= rule->sat_max) return 0;
    if (rule->val_min >= 0.0f && val < rule->val_min) return 0;
    if (rule->val_max >= 0.0f && val >= rule->val_max) return 0;
    return 1;
}

// PIL ImageFilter.ModeFilter(5): the most frequent value of the 5x5 box; values seen once or twice are
// ignored, and when none is seen more than twice the pixel keeps its own (ties: the lowest class, like
// PIL's ascending histogram scan). The 2-px border is copied. Sliding window: per-column counts over the
// 5 rows, then a running 5-column sum, so each texel costs a few adds, not 25 reads.
static void mode_filter_5x5(const unsigned char *in, unsigned char *out, const int w, const int h) {
    memcpy(out, in, (size_t) w * (size_t) h);
    if (w < 5 || h < 5) return;
    unsigned short *col = calloc((size_t) w * CLS_COUNT, sizeof(*col));
    if (!col) return;
    for (int y = 0; y < 5; y++) {
        const unsigned char *row = in + (size_t) y * (size_t) w;
        for (int x = 0; x < w; x++) col[x * CLS_COUNT + row[x]]++;
    }
    for (int y = 2; y < h - 2; y++) {
        if (y > 2) { // rows y-2..y+2: drop y-3, add y+2
            const unsigned char *rem = in + (size_t) (y - 3) * (size_t) w, *add = in + (size_t) (y + 2) * (size_t) w;
            for (int x = 0; x < w; x++) {
                col[x * CLS_COUNT + rem[x]]--;
                col[x * CLS_COUNT + add[x]]++;
            }
        }
        int cnt[CLS_COUNT] = {0};
        for (int x = 0; x < 5; x++) for (int c = 0; c < CLS_COUNT; c++) cnt[c] += col[x * CLS_COUNT + c];
        unsigned char *orow = out + (size_t) y * (size_t) w;
        for (int x = 2; x < w - 2; x++) {
            if (x > 2) for (int c = 0; c < CLS_COUNT; c++) cnt[c] += col[(x + 2) * CLS_COUNT + c] - col[(x - 3) * CLS_COUNT + c];
            int best = -1, best_n = 2; // > 2 occurrences only
            for (int c = 0; c < CLS_COUNT; c++) if (cnt[c] > best_n) { best_n = cnt[c]; best = c; }
            if (best >= 0) orow[x] = (unsigned char) best;
        }
    }
    free(col);
}

// Classifies `rgba` (w x h, bottom row first: GL order) into `cls` (w * h bytes) with `spec`.
static int mask_classify(const unsigned char *rgba, const int w, const int h, const MaskSpec *spec, unsigned char *cls) {
    unsigned char *raw = malloc((size_t) w * (size_t) h);
    // Krunker's weapon textures hold a few hundred distinct colours (indexed PNGs): a direct-mapped memo of
    // rgb -> class (4096 slots, tag = the full rgb) skips the HSV + rule work for every repeat
    enum { MEMO_SLOTS = 4096 };
    unsigned int *memo_key = malloc(MEMO_SLOTS * sizeof(*memo_key));
    unsigned char *memo_cls = malloc(MEMO_SLOTS);
    if (!raw || !memo_key || !memo_cls) {
        free(raw);
        free(memo_key);
        free(memo_cls);
        return 0;
    }
    memset(memo_key, 0xff, MEMO_SLOTS * sizeof(*memo_key)); // 0xffffffff: no rgb (keys are 24-bit)
    for (int i = 0; i < w * h; i++) {
        const unsigned char *p = rgba + (size_t) i * 4;
        int c = spec->default_cls;
        if (p[3] >= 8) {
            const unsigned int rgb = (unsigned int) p[0] << 16 | (unsigned int) p[1] << 8 | p[2];
            const unsigned int slot = (rgb * 2654435761u) >> 20; // 12 bits
            if (memo_key[slot] == rgb) {
                c = memo_cls[slot];
            } else {
                int h8, s8, v8;
                rgb_to_hsv_pil(p[0], p[1], p[2], &h8, &s8, &v8);
                const float hue = (float) h8 * (360.0f / 255.0f), sat = (float) s8 / 255.0f, val = (float) v8 / 255.0f;
                for (int r = 0; r < spec->rule_count; r++) {
                    if (rule_matches(&spec->rules[r], hue, sat, val)) { c = spec->rules[r].cls; break; }
                }
                memo_key[slot] = rgb;
                memo_cls[slot] = (unsigned char) c;
            }
        }
        raw[i] = (unsigned char) c;
    }
    free(memo_key);
    free(memo_cls);
    mode_filter_5x5(raw, cls, w, h);
    free(raw);
    for (int r = 0; r < spec->rect_count; r++) {
        const MaskRect *rc = &spec->rects[r];
        const int x0 = (int) (rc->x0 * (float) w), x1 = (int) (rc->x1 * (float) w);
        // rows are bottom first (GL order, as read back): the rect's top-left-origin y range is mirrored
        const int top0 = (int) (rc->y0 * (float) h), top1 = (int) (rc->y1 * (float) h);
        const int y0 = h - top1, y1 = h - top0;
        for (int y = y0 < 0 ? 0 : y0; y < (y1 > h ? h : y1); y++)
            for (int x = x0 < 0 ? 0 : x0; x < (x1 > w ? w : x1); x++) cls[(size_t) y * (size_t) w + (size_t) x] = (unsigned char) rc->cls;
    }
    return 1;
}

// ---------------------------------------------------------------- overrides (assets/materials/weapon_masks.json)

static cJSON *g_overrides;      // the parsed file, NULL until the first mask / when missing
static int g_overrides_loaded;

static void overrides_load(void) {
    if (g_overrides_loaded) return;
    g_overrides_loaded = 1;
    char *path = concat(assets_path(), "materials/weapon_masks.json");
    if (!path) return;
    ensure_asset_file(path);
    unsigned char *text = NULL;
    size_t length = 0;
    const int missing = read_file(path, &text, &length);
    free(path);
    if (missing || !text) return;
    g_overrides = cJSON_ParseWithLength((const char *) text, length);
    free(text);
    if (!g_overrides) fprintf(stderr, "[rt] masks: assets/materials/weapon_masks.json does not parse (defaults only)\n");
}

// `*`-only glob, like the Python tool's fnmatch use
static int glob_match(const char *pat, const char *s) {
    if (!*pat) return !*s;
    if (*pat == '*') return glob_match(pat + 1, s) || (*s && glob_match(pat, s + 1));
    return *pat == *s && glob_match(pat + 1, s + 1);
}

static float json_or(const cJSON *obj, const char *key, const float fallback) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(v) ? (float) cJSON_GetNumberValue(v) : fallback;
}

static void spec_apply_entry(MaskSpec *spec, const cJSON *entry) {
    const cJSON *rules = cJSON_GetObjectItemCaseSensitive(entry, "rules");
    if (cJSON_IsArray(rules)) {
        spec->rule_count = 0;
        const cJSON *rule;
        cJSON_ArrayForEach(rule, rules) {
            if (spec->rule_count >= MASK_RULES_MAX) break;
            const int cls = class_by_name(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(rule, "class")));
            if (cls < 0) continue;
            MaskRule *r = &spec->rules[spec->rule_count++];
            *r = (MaskRule) {cls, -1.0f, -1.0f, json_or(rule, "sat_min", -1.0f), json_or(rule, "sat_max", -1.0f), json_or(rule, "val_min", -1.0f), json_or(rule, "val_max", -1.0f)};
            const cJSON *hue = cJSON_GetObjectItemCaseSensitive(rule, "hue");
            if (cJSON_IsArray(hue) && cJSON_GetArraySize(hue) == 2) {
                r->hue0 = (float) cJSON_GetNumberValue(cJSON_GetArrayItem(hue, 0));
                r->hue1 = (float) cJSON_GetNumberValue(cJSON_GetArrayItem(hue, 1));
            }
        }
    }
    const cJSON *rects = cJSON_GetObjectItemCaseSensitive(entry, "rects");
    if (cJSON_IsArray(rects)) {
        const cJSON *rect;
        cJSON_ArrayForEach(rect, rects) {
            if (spec->rect_count >= MASK_RECTS_MAX) break;
            const int cls = class_by_name(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(rect, "class")));
            const cJSON *uv = cJSON_GetObjectItemCaseSensitive(rect, "uv");
            if (cls < 0 || !cJSON_IsArray(uv) || cJSON_GetArraySize(uv) != 4) continue;
            MaskRect *rc = &spec->rects[spec->rect_count++];
            *rc = (MaskRect) {cls, (float) cJSON_GetNumberValue(cJSON_GetArrayItem(uv, 0)), (float) cJSON_GetNumberValue(cJSON_GetArrayItem(uv, 1)),
                              (float) cJSON_GetNumberValue(cJSON_GetArrayItem(uv, 2)), (float) cJSON_GetNumberValue(cJSON_GetArrayItem(uv, 3))};
        }
    }
    const int def = class_by_name(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(entry, "default")));
    if (def >= 0) spec->default_cls = def;
    const cJSON *values = cJSON_GetObjectItemCaseSensitive(entry, "values");
    if (cJSON_IsObject(values)) {
        for (int c = 0; c < CLS_COUNT; c++) {
            const cJSON *v = cJSON_GetObjectItemCaseSensitive(values, CLASS_NAMES[c]);
            if (cJSON_IsArray(v) && cJSON_GetArraySize(v) == 2) {
                spec->values[c][0] = (unsigned char) cJSON_GetNumberValue(cJSON_GetArrayItem(v, 0));
                spec->values[c][1] = (unsigned char) cJSON_GetNumberValue(cJSON_GetArrayItem(v, 1));
            }
        }
    }
}

// weapon_masks.py overrides_for(): "*", then the exact name, then every other glob that matches
static void spec_for(MaskSpec *spec, const char *base) {
    spec_defaults(spec);
    overrides_load();
    if (!g_overrides) return;
    const cJSON *entry = cJSON_GetObjectItemCaseSensitive(g_overrides, "*");
    if (cJSON_IsObject(entry)) spec_apply_entry(spec, entry);
    entry = cJSON_GetObjectItemCaseSensitive(g_overrides, base);
    if (cJSON_IsObject(entry)) spec_apply_entry(spec, entry);
    cJSON_ArrayForEach(entry, g_overrides) {
        const char *key = entry->string;
        if (!key || !cJSON_IsObject(entry) || !strcmp(key, "*") || !strcmp(key, base) || !strchr(key, '*')) continue;
        if (glob_match(key, base)) spec_apply_entry(spec, entry);
    }
}

// ---------------------------------------------------------------- building and caching

#define RT_MASK_CACHE_MAX 512 // model keys seen this session (a few dozen: the loadouts in play)
typedef struct {
    char *key;
    unsigned int texture; // 0 = no mask (no file, no texture to build from)
    int built;            // ours (mask_build), deleted in rt_masks_fini; a loaded _mr.png is load_texture's
} MaskEntry;

static MaskEntry g_cache[RT_MASK_CACHE_MAX];
static int g_cache_count;

static MaskEntry *cache_find(const char *key) {
    for (int i = 0; i < g_cache_count; i++) if (!strcmp(g_cache[i].key, key)) return &g_cache[i];
    return NULL;
}

static void cache_put(const char *key, const unsigned int texture, const int built) {
    if (g_cache_count >= RT_MASK_CACHE_MAX) return;
    g_cache[g_cache_count].key = strdup(key);
    if (!g_cache[g_cache_count].key) return;
    g_cache[g_cache_count].texture = texture;
    g_cache[g_cache_count].built = built;
    g_cache_count++;
}

static int masks_disabled(void) { // KRUNKNATIVE_RT_NO_MASKS=1: dev A/B (class factors alone)
    static int cached = -1;
    if (cached < 0) {
        const char *env = getenv("KRUNKNATIVE_RT_NO_MASKS");
        cached = env && *env && *env != '0';
    }
    return cached;
}

static int file_exists(const char *path) {
    ensure_asset_file(path); // the web fetches it first
    FILE *file = fopen(path, "rb");
    if (file) fclose(file);
    return file != NULL;
}

static const char *key_base(const char *model_key) { // "weapons/weapon_2" -> "weapon_2"
    const char *slash = strrchr(model_key, '/');
    return slash ? slash + 1 : model_key;
}

// Reads a GL_RGBA8 texture back (GL_RGBA / GL_UNSIGNED_BYTE, the one readback the GL subset allows)
// through a scratch framebuffer; bottom row first, as load_texture() uploaded it. NULL on failure.
static unsigned char *texture_read_back(const unsigned int texture, const int w, const int h) {
    unsigned char *rgba = malloc((size_t) w * (size_t) h * 4);
    if (!rgba) return NULL;
    int previous_fbo;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous_fbo);
    unsigned int fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    const int complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (complete) glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glBindFramebuffer(GL_FRAMEBUFFER, (unsigned int) previous_fbo);
    glDeleteFramebuffers(1, &fbo);
    if (!complete) {
        free(rgba);
        return NULL;
    }
    return rgba;
}

static void flip_rows(unsigned char *rgba, const int w, const int h) {
    for (int y = 0; y < h / 2; y++) {
        unsigned char *a = rgba + (size_t) y * (size_t) w * 4, *b = rgba + (size_t) (h - 1 - y) * (size_t) w * 4;
        for (int i = 0; i < w * 4; i++) { const unsigned char t = a[i]; a[i] = b[i]; b[i] = t; }
    }
}

// Builds the mask of `model_key` from "textures/<model_key>.png"; 0 when the texture is missing. The
// pixels come back from the GL texture load_texture() already holds (decoded once for the weapon itself;
// a PNG decode of a 1024x1024 texture is ~40-90 ms, the readback ~1 ms), the size from the PNG header.
static unsigned int mask_texture_new(const int w, const int h, const void *pixels) {
    unsigned int texture = 0;
    glGenTextures(1, &texture);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    g_active_texture = texture; // loaders.c's unit-0 binding cache
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    return texture;
}

// KRUNKNATIVE_RT_MASK_DUMP=<dir>: `bottom_first` (GL row order) written top row first, like the Python tool's
static void mask_dump(const char *model_key, unsigned char *bottom_first, const int w, const int h) {
    const char *dump = getenv("KRUNKNATIVE_RT_MASK_DUMP");
    if (!dump || !*dump || !bottom_first) return;
    char dump_path[1024];
    snprintf(dump_path, sizeof(dump_path), "%s/%s_mr.png", dump, key_base(model_key));
    flip_rows(bottom_first, w, h);
    png_write_rgba(dump_path, bottom_first, w, h);
    flip_rows(bottom_first, w, h);
}

// The UV rects in texel rows of the GL-order image (mirrored y: the rects' origin is the top-left)
static void spec_rect_texels(const MaskRect *rc, const int w, const int h, int out[4]) {
    const int x0 = (int) (rc->x0 * (float) w), x1 = (int) (rc->x1 * (float) w);
    const int top0 = (int) (rc->y0 * (float) h), top1 = (int) (rc->y1 * (float) h);
    out[0] = x0 < 0 ? 0 : x0;
    out[1] = h - top1 < 0 ? 0 : h - top1;
    out[2] = x1 > w ? w : x1;
    out[3] = h - top0 > h ? h : h - top0;
}

// ---- GPU path (the default): rt_mask_classify.frag + rt_mask_filter.frag, two fullscreen passes, no readback.

static struct {
    int tried;
    unsigned int classify, filter, vao, fbo;
} g_gpu;

static int gpu_ready(void) {
    if (g_gpu.tried) return g_gpu.classify && g_gpu.filter;
    g_gpu.tried = 1;
    const char *cpu = getenv("KRUNKNATIVE_RT_MASK_CPU"); // dev: the CPU classifier (the reference) instead
    if (cpu && *cpu && *cpu != '0') return 0;
    char *vert = concat(assets_path(), "shaders/scene_composite.vert");
    char *frag1 = concat(assets_path(), "shaders/rt_mask_classify.frag");
    char *frag2 = concat(assets_path(), "shaders/rt_mask_filter.frag");
    if (vert && frag1) g_gpu.classify = shader_compile(vert, frag1);
    if (vert && frag2) g_gpu.filter = shader_compile(vert, frag2);
    free(vert);
    free(frag1);
    free(frag2);
    if (!g_gpu.classify || !g_gpu.filter) {
        fprintf(stderr, "[rt] masks: rt_mask_*.frag failed to compile: the CPU classifier builds the masks\n");
        if (g_gpu.classify) glDeleteProgram(g_gpu.classify);
        if (g_gpu.filter) glDeleteProgram(g_gpu.filter);
        g_gpu.classify = g_gpu.filter = 0;
        return 0;
    }
    glGenVertexArrays(1, &g_gpu.vao);
    glGenFramebuffers(1, &g_gpu.fbo);
    return 1;
}

static int gpu_draw_into(const unsigned int target, const int w, const int h) {
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) return 0;
    glViewport(0, 0, w, h);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    return 1;
}

// Builds the mask on the GPU from `base` (w x h); 0 on failure. Restores framebuffers, viewport, program,
// VAO, unit 0 and the blend / depth / cull / scissor enables it found (it runs from mesh building, which
// can happen anywhere in the frame).
static unsigned int mask_build_gpu(const unsigned int base, const int w, const int h, const MaskSpec *spec) {
    int draw_fbo, read_fbo, viewport[4], vao;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fbo);
    glGetIntegerv(GL_VIEWPORT, viewport);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
    const GLboolean blend = glIsEnabled(GL_BLEND), depth = glIsEnabled(GL_DEPTH_TEST), cull = glIsEnabled(GL_CULL_FACE),
                    scissor = glIsEnabled(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);

    const unsigned int classes = mask_texture_new(w, h, NULL);
    const unsigned int mask = mask_texture_new(w, h, NULL);
    glBindFramebuffer(GL_FRAMEBUFFER, g_gpu.fbo);
    glBindVertexArray(g_gpu.vao);

    // pass 1: the HSV rules
    glUseProgram(g_gpu.classify);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, base);
    glUniform1i(glGetUniformLocation(g_gpu.classify, "src"), 0);
    glUniform1i(glGetUniformLocation(g_gpu.classify, "rule_count"), spec->rule_count);
    glUniform1i(glGetUniformLocation(g_gpu.classify, "default_cls"), spec->default_cls);
    int rule_cls[MASK_RULES_MAX] = {0};
    float rule_hs[MASK_RULES_MAX * 4] = {0}, rule_v[MASK_RULES_MAX * 2] = {0};
    for (int r = 0; r < spec->rule_count; r++) {
        const MaskRule *m = &spec->rules[r];
        rule_cls[r] = m->cls;
        rule_hs[r * 4] = m->hue0;
        rule_hs[r * 4 + 1] = m->hue1;
        rule_hs[r * 4 + 2] = m->sat_min;
        rule_hs[r * 4 + 3] = m->sat_max;
        rule_v[r * 2] = m->val_min;
        rule_v[r * 2 + 1] = m->val_max;
    }
    glUniform1iv(glGetUniformLocation(g_gpu.classify, "rule_cls"), MASK_RULES_MAX, rule_cls);
    glUniform4fv(glGetUniformLocation(g_gpu.classify, "rule_hs"), MASK_RULES_MAX, rule_hs);
    glUniform2fv(glGetUniformLocation(g_gpu.classify, "rule_v"), MASK_RULES_MAX, rule_v);
    int ok = gpu_draw_into(classes, w, h);

    // pass 2: the 5x5 majority filter, the rects, the values
    if (ok) {
        glUseProgram(g_gpu.filter);
        glBindTexture(GL_TEXTURE_2D, classes);
        glUniform1i(glGetUniformLocation(g_gpu.filter, "cls_tex"), 0);
        glUniform1i(glGetUniformLocation(g_gpu.filter, "rect_count"), spec->rect_count);
        int rect_cls[MASK_RECTS_MAX] = {0}, rect_px[MASK_RECTS_MAX * 4] = {0};
        for (int r = 0; r < spec->rect_count; r++) {
            rect_cls[r] = spec->rects[r].cls;
            spec_rect_texels(&spec->rects[r], w, h, &rect_px[r * 4]);
        }
        glUniform1iv(glGetUniformLocation(g_gpu.filter, "rect_cls"), MASK_RECTS_MAX, rect_cls);
        glUniform4iv(glGetUniformLocation(g_gpu.filter, "rect_px"), MASK_RECTS_MAX, rect_px);
        float values[CLS_COUNT * 2];
        for (int c = 0; c < CLS_COUNT; c++) {
            values[c * 2] = spec->values[c][0];
            values[c * 2 + 1] = spec->values[c][1];
        }
        glUniform2fv(glGetUniformLocation(g_gpu.filter, "values"), CLS_COUNT, values);
        ok = gpu_draw_into(mask, w, h);
    }
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);

    glBindTexture(GL_TEXTURE_2D, 0);
    g_active_texture = 0;
    glDeleteTextures(1, &classes);
    glUseProgram(0);
    g_active_shader = 0;
    glBindVertexArray((unsigned int) vao);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (unsigned int) draw_fbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (unsigned int) read_fbo);
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    if (blend) glEnable(GL_BLEND);
    if (depth) glEnable(GL_DEPTH_TEST);
    if (cull) glEnable(GL_CULL_FACE);
    if (scissor) glEnable(GL_SCISSOR_TEST);
    if (!ok) {
        glDeleteTextures(1, &mask);
        return 0;
    }
    return mask;
}

// ---- CPU path (the fallback and the reference): the texture read back, classified here.

static unsigned int mask_build_cpu(const char *model_key, const unsigned int base, const int w, const int h, const MaskSpec *spec) {
    // kept bottom row first (GL order) throughout: the classifier and the 5x5 filter do not care about the row
    // order, the UV rects are mirrored in mask_classify, and the upload needs this order. Only the dump flips.
    unsigned char *rgba = texture_read_back(base, w, h);
    if (!rgba) return 0;
    unsigned char *cls = malloc((size_t) w * (size_t) h);
    unsigned char *out = malloc((size_t) w * (size_t) h * 4);
    if (!cls || !out || !mask_classify(rgba, w, h, spec, cls)) {
        free(cls);
        free(out);
        free(rgba);
        return 0;
    }
    free(rgba);
    for (int i = 0; i < w * h; i++) {
        out[(size_t) i * 4] = 255;
        out[(size_t) i * 4 + 1] = spec->values[cls[i]][0];
        out[(size_t) i * 4 + 2] = spec->values[cls[i]][1];
        out[(size_t) i * 4 + 3] = 255;
    }
    free(cls);
    mask_dump(model_key, out, w, h);
    const unsigned int texture = mask_texture_new(w, h, out);
    free(out);
    return texture;
}

// Builds the mask of `model_key` from "textures/<model_key>.png" (the GL texture load_texture() already holds
// for the weapon itself; its size from the PNG header); 0 when the texture is missing.
static unsigned int mask_build(const char *model_key) {
    char *path = cosmetic_asset_path("textures", model_key, "png");
    if (!path) return 0;
    if (!file_exists(path)) {
        free(path);
        return 0;
    }
    int w = 0, h = 0, _;
    char swapped[1024];
    const char *real = file_redirect(path, swapped, sizeof(swapped));
    if (!stbi_info(real, &w, &h, &_) || w < 1 || h < 1) {
        free(path);
        return 0;
    }
    const unsigned int base = load_texture(path); // cached: the weapon's own texture, or decoded now
    free(path);
    if (!base) return 0;

    const double tc = glfwGetTime();
    const int gpu = gpu_ready(); // compiles the two programs on the first mask of the session
    const double t0 = glfwGetTime();
    if (t0 - tc > 0.001) fprintf(stderr, "[rt] masks: programs compiled in %.1f ms\n", (t0 - tc) * 1000.0);
    MaskSpec spec;
    spec_for(&spec, key_base(model_key));
    unsigned int texture = gpu ? mask_build_gpu(base, w, h, &spec) : 0;
    if (!texture) texture = mask_build_cpu(model_key, base, w, h, &spec);
    const double t1 = glfwGetTime();
    if (texture && gpu) { // the dump of a GPU mask (dev): read back outside the timed part
        unsigned char *pixels = texture_read_back(texture, w, h);
        mask_dump(model_key, pixels, w, h);
        free(pixels);
    }
    if (texture) fprintf(stderr, "[rt] mask %s: built %dx%d on the %s in %.2f ms\n", model_key, w, h, gpu ? "GPU" : "CPU", (t1 - t0) * 1000.0);
    return texture;
}

// Masks exist for weapon and melee models only: "weapons/<src>[_<mid>]", "melee/melee_<id>" (player_mesh.c,
// prefab_weapon_mesh). Map OBJ models and other loadMesh assets go through the same prefab_model_mesh and
// are left alone (their surfaces are the material table's business).
static int key_is_weapon(const char *model_key) {
    return !strncmp(model_key, "weapons/", 8) || !strncmp(model_key, "melee/", 6);
}

unsigned int rt_mask_texture(const char *model_key) {
    if (!model_key || !*model_key || !client_render_settings()->rt_mode || masks_disabled() || !key_is_weapon(model_key)) return 0;
    const MaskEntry *hit = cache_find(model_key);
    if (hit) return hit->texture;

    unsigned int texture = 0;
    int built = 0;
    // 1. a mask file next to the texture (hand-painted / the Python tool's): load_texture caches it by path
    char key[320];
    snprintf(key, sizeof(key), "%s_mr", model_key);
    char *path = cosmetic_asset_path("textures", key, "png");
    if (path && file_exists(path)) texture = load_texture(path);
    free(path);
    // 2. built from the base texture
    if (!texture) {
        texture = mask_build(model_key);
        built = texture != 0;
    }
    cache_put(model_key, texture, built);
    return texture;
}

void rt_mask_apply(BasicMaterial *material, const char *model_key) {
    if (!material) return;
    const unsigned int mask = rt_mask_texture(model_key);
    if (!mask) return;
    material->metal_rough_texture = mask;
    material->rt_mask = 1; // rt_materials.c: a part mask, not a glTF map: the class factors still resolve and scale it
    // basic.frag: metalness = metalness_factor * mask.b, roughness = roughness_factor * mask.g. A plain
    // weapon material has both factors at 0 (calloc), which would zero the mask: glTF's defaults (1)
    // let the mask speak alone; a class (rt_materials_resolve) or a glTF material that set them keeps them.
    if (material->metalness_factor == 0.0f && material->roughness_factor == 0.0f) {
        material->metalness_factor = 1.0f;
        material->roughness_factor = 1.0f;
    }
}

void rt_masks_fini(void) {
    for (int i = 0; i < g_cache_count; i++) {
        if (g_cache[i].built && g_cache[i].texture) glDeleteTextures(1, &g_cache[i].texture);
        free(g_cache[i].key);
    }
    g_cache_count = 0;
    if (g_gpu.classify) glDeleteProgram(g_gpu.classify);
    if (g_gpu.filter) glDeleteProgram(g_gpu.filter);
    if (g_gpu.vao) glDeleteVertexArrays(1, &g_gpu.vao);
    if (g_gpu.fbo) glDeleteFramebuffers(1, &g_gpu.fbo);
    memset(&g_gpu, 0, sizeof(g_gpu));
    if (g_overrides) cJSON_Delete(g_overrides);
    g_overrides = NULL;
    g_overrides_loaded = 0;
}
