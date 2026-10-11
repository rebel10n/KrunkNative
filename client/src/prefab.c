#include <gl.h>
#include <client.h>
#include <render_settings.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stb_image.h>
#include <pcg_basic.h>

// config.js ambientVal: brightness of the bottom vertices of map cubes (31.js generateCube amb)
#define PREFAB_AMBIENT_VAL 0.5f

typedef struct {
    const char *filename;
    const float scale;
    const unsigned char transparent:1;
    const int frames;
    const float frame_time;
} PrefabModel;

const PrefabModel prefab_models[] = {
    {NULL}, // cube (software-generated)
    {"crate_0", 6.0f},
    {"barrel_0", 4.0f},
    {NULL}, // ladder (software-generated)
    {NULL}, // plane (software-generated)
    {NULL}, // spawnpoint
    {NULL}, // camera position
    {"vehicle_0", 20.0f},
    {"stack_0", 6.0f},
    {NULL}, // ramp (software-generated)
    {NULL}, // score zone
    {NULL}, // billboard (software-generated)
    {NULL}, // death zone
    {NULL}, // particles
    {NULL}, // objective
    {"tree_0", 10.0f},
    {"cone_0", 4.0f},
    {"container_0", 7.0f},
    {"grass_0", 32.0f, 1, 4, 0.180f},
    {"containerr_0", 7.0f},
    {"acidbarrel_0", 4.0f},
    {"door_0", 5.0f},
    {"window_0", 6.0f, 1},
    {NULL}, // flag
    {NULL}, // gate
    {NULL}, // check point
    {NULL}, // weapon pickup (software-generated)
    {NULL}, // teleporter
    {"teddy_0", 6.0f},
    {NULL}, // trigger
    {NULL}, // sign (software-generated)
    {NULL}, // deposit box
    {NULL}, // light cone
    {NULL}, // spectate cam
    {NULL}, // sphere (software-generated)
    {NULL}, // placeholder
    {"cardb_0", 5.0f},
    {"pallet_0", 6.0f},
    {NULL}, // liquid
    {NULL}, // sound emitter
    {NULL}, // event
    {NULL}, // terminal
    {NULL}, // premium zone
    {NULL}, // verified zone
    {NULL}, // custom asset
    {NULL}, // bomb site
    {NULL}, // boost pad (software-generated)
    {NULL}, // team zone
    {NULL}, // cylinder (software-generated)
    {"police_0", 4.0f},
    {"cage_0", 6.0f},
    {"ebarrel_0", 4.0f},
    {NULL}, // showcase
    {NULL}, // point light
    {"ghost_0", 4.0f},
    {NULL}, // bot (unsupported - for now at least)
    {"pumpkin_0", 4.0f},
    {NULL}, // rune (software-generated)
    {"skeleton_0", 4.0f},
    {"knight_0", 4.0f},
};

typedef struct {
    const char *name;
    const char *src;
    const unsigned char transparent:1;
} PrefabTexture;

const PrefabTexture prefab_textures[] = {
    {"WALL", "wall_0"},
    {"DIRT", "dirt_0"},
    {"FLOOR", "floor_0"},
    {"GRID", "grid_0"},
    {"GREY", "grey_0"},
    {"DEFAULT", "default"},
    {"ROOF", "roof_0"},
    {"FLAG", "flag_0"},
    {"GRASS", "grass_1"},
    {"CHECK", "check_0"},
    {"LINES", "lines_0"},
    {"BRICK", "brick_0"},
    {"LINK", "link_0", 1},
    {"LIQUID", "liquid_0", 1},
    {"GRAIN", "grain_0"},
    {"FABRIC", "fabric_0"},
    {"TILE", "tile_0"},
};

static int prefab_texture_string_equals(const char *a, const char *b) {
    if (!a || !b) return 0;

    while (*a && *b) {
        const char ca = *a >= 'a' && *a <= 'z' ? *a - 32 : *a;
        const char cb = *b >= 'a' && *b <= 'z' ? *b - 32 : *b;
        if (ca != cb) return 0;

        a++;
        b++;
    }

    return *a == *b;
}

static int prefab_texture_id_from_string(const char *name) {
    if (!name) return 0;

    char *end = NULL;
    const long parsed_id = strtol(name, &end, 10);
    if (end && *end == '\0') return (int) parsed_id;

    for (size_t i = 0; i < sizeof(prefab_textures) / sizeof(prefab_textures[0]); i++) {
        if (
            prefab_texture_string_equals(name, prefab_textures[i].name) ||
            prefab_texture_string_equals(name, prefab_textures[i].src)
        ) {
            return (int) i;
        }
    }

    return 0;
}

static int prefab_face_mask(const cJSON *raw_faces) {
    if (!cJSON_IsString(raw_faces)) return 0x3f;

    const long bits = strtol(cJSON_GetStringValue(raw_faces), NULL, 16);
    int mask = 0;

    for (int i = 0; i < 6; i++) {
        if (bits & (1 << (5 - i))) mask |= 1 << i;
    }

    return mask;
}

static int prefab_hidden_in_game(const int prefab) {
    switch (prefab) {
        case PREFAB_SPAWN_POINT:
        case PREFAB_CAMERA_POSITION:
        case PREFAB_SCORE_ZONE:
        case PREFAB_DEATH_ZONE:
        case PREFAB_PARTICLES:
        case PREFAB_FLAG:
        case PREFAB_CHECK_POINT:
        case PREFAB_WEAPON_PICKUP:
        case PREFAB_TELEPORTER:
        case PREFAB_TRIGGER:
        case PREFAB_SPECTATE_CAM:
        case PREFAB_PLACEHOLDER:
        case PREFAB_SOUND_EMITTER:
        case PREFAB_EVENT:
        case PREFAB_PREMIUM_ZONE:
        case PREFAB_VERIFIED_ZONE:
        case PREFAB_CUSTOM_ASSET:
        case PREFAB_BOMB_SITE:
        case PREFAB_TEAM_ZONE:
        case PREFAB_SHOWCASE:
        case PREFAB_POINT_LIGHT:
            return 1;
        default:
            return 0;
    }
}

static void prefab_apply_lighting_controls(BasicMaterial *material, const Object *object) {
    material->ambient_enabled = object->ambient_enabled;
    material->fog_enabled = object->fog_enabled;
    // the lighting setting (0 Basic / 1 Lambert / 2 Phong) is applied per draw in basic.c
}

// 29.js noDetail prefabs and the mapDet-only builders (93.js liquids/light cones): skipped
// unless the mapDet setting is on.
static int prefab_needs_map_details(const int prefab) {
    switch (prefab) {
        case PREFAB_GRASS:
        case PREFAB_WINDOW:
        case PREFAB_TEDDY:
        case PREFAB_FLAG:
        case PREFAB_LIGHT_CONE:
        case PREFAB_POINT_LIGHT:
        case PREFAB_LIQUID:
            return 1;
        default:
            return 0;
    }
}

static float prefab_hue_to_rgb(const float p, const float q, float t) {
    if (t < 0.0f) t += 1.0f;
    if (t > 1.0f) t -= 1.0f;
    if (t < 1.0f / 6.0f) return p + (q - p) * 6.0f * t;
    if (t < 0.5f) return q;
    if (t < 2.0f / 3.0f) return p + (q - p) * 6.0f * (2.0f / 3.0f - t);
    return p;
}

// Krunker 31.js getColor(colr, mult): THREE.Color(colr).multiplyScalar(mult), then the
// HSL saturation is raised by 1.5 (clamped). Used for the vertex colours of generated map
// geometry (cubes, planes, ramps); models use the raw colour as their material colour.
static vec4 prefab_krunker_color(const vec4 color, const float mult) {
    const float r = color.x * mult;
    const float g = color.y * mult;
    const float b = color.z * mult;
    const float max = fmaxf(r, fmaxf(g, b));
    const float min = fminf(r, fminf(g, b));
    const float l = (min + max) * 0.5f;
    float h = 0.0f;
    float s = 0.0f;

    if (max != min) {
        const float delta = max - min;
        s = l <= 0.5f ? delta / (max + min) : delta / (2.0f - max - min);

        if (max == r) h = (g - b) / delta + (g < b ? 6.0f : 0.0f);
        else if (max == g) h = (b - r) / delta + 2.0f;
        else h = (r - g) / delta + 4.0f;

        h /= 6.0f;
    }

    s = fminf(1.0f, s * 1.5f);

    vec4 out = {l, l, l, color.w};

    if (s > 0.0f) {
        const float p = l <= 0.5f ? l * (1.0f + s) : l + s - l * s;
        const float q = 2.0f * l - p;

        out.x = prefab_hue_to_rgb(q, p, h + 1.0f / 3.0f);
        out.y = prefab_hue_to_rgb(q, p, h);
        out.z = prefab_hue_to_rgb(q, p, h - 1.0f / 3.0f);
    }

    return out;
}

static float prefab_json_float(const cJSON *json, const float fallback) {
    if (cJSON_IsNumber(json)) return (float) cJSON_GetNumberValue(json);
    if (cJSON_IsString(json)) return (float) atof(cJSON_GetStringValue(json));
    return fallback;
}

// 94.js: texOffX/texOffY = limitMM(parseFloat(tox) || 0, 0, 1).round(1), the three.js
// texture.offset of cubes, planes and ramps.
static vec2 prefab_texture_offset(const cJSON *raw_obj) {
    vec2 offset = {
        prefab_json_float(cJSON_GetObjectItem(raw_obj, "tox"), 0.0f),
        prefab_json_float(cJSON_GetObjectItem(raw_obj, "toy"), 0.0f),
    };

    offset.x = roundf(fminf(fmaxf(offset.x, 0.0f), 1.0f) * 10.0f) / 10.0f;
    offset.y = roundf(fminf(fmaxf(offset.y, 0.0f), 1.0f) * 10.0f) / 10.0f;
    return offset;
}

static int prefab_json_present(const cJSON *json) {
    return json && !cJSON_IsNull(json);
}

static Geometry *prefab_upload_geo(const vertex *vertices, const int vertex_count, const unsigned int *indices, const int index_count) {
    Geometry *geometry = calloc(1, sizeof(Geometry));
    if (!geometry) return NULL;

    unsigned int vbo;
    glGenVertexArrays(1, &geometry->vao);
    glGenBuffers(1, &geometry->ebo);
    glGenBuffers(1, &vbo);

    glBindVertexArray(geometry->vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, (long) vertex_count * (long) sizeof(vertex), vertices, GL_STATIC_DRAW);
    geometry_set_bounds(geometry, vertices, (size_t) vertex_count);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, geometry->ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (long) index_count * (long) sizeof(unsigned int), indices, GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), NULL);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *) (3 * sizeof(float)));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);

    geometry->index_count = index_count;
    return geometry;
}

// THREE.PlaneGeometry(1, 1): the XY unit square facing +Z, CCW from the front. Krunker
// planes, billboards, signs, liquids and ramps all start from it (55.js addPlane/addRamp).
static Geometry *prefab_plane_xy_geo() {
    static Geometry *cached;
    static const vertex vertices[] = {
        {-0.5f, -0.5f, 0.0f, 0.0f, 0.0f},
        {0.5f, 0.5f, 0.0f, 1.0f, 1.0f},
        {-0.5f, 0.5f, 0.0f, 0.0f, 1.0f},
        {0.5f, -0.5f, 0.0f, 1.0f, 0.0f},
    };
    static const unsigned int indices[] = {0, 1, 2, 0, 3, 1};

    if (!cached) cached = prefab_upload_geo(vertices, 4, indices, 6);
    return cached;
}

// --- SIGN canvas text (2.js createCanvasText, drawn with ctxText) --------------------------

static int prefab_utf8_next(const char **text) {
    const unsigned char *s = (const unsigned char *) *text;
    int codepoint = *s++;
    int extra = codepoint >= 0xf0 ? 3 : codepoint >= 0xe0 ? 2 : codepoint >= 0xc0 ? 1 : 0;

    if (extra) codepoint &= 0x3f >> extra;
    while (extra-- && (*s & 0xc0) == 0x80) codepoint = (codepoint << 6) | (*s++ & 0x3f);

    *text = (const char *) s;
    return codepoint;
}

// canvas measureText(text).width: the sum of the glyph advances at the current pixel size
static float prefab_text_width(const char *text, const size_t length) {
    const char *end = text + length;
    float width = 0.0f;

    while (text < end && *text) {
        const int codepoint = prefab_utf8_next(&text);
        if (!FT_Load_Char(g_game_font, codepoint, FT_LOAD_DEFAULT)) width += (float) g_game_font->glyph->advance.x / 64.0f;
    }

    return width;
}

static void prefab_text_draw(unsigned char *rgba, const int width, const int height, const char *text, const size_t length, float x, const float baseline, const vec4 color) {
    const char *end = text + length;

    while (text < end && *text) {
        const int codepoint = prefab_utf8_next(&text);
        if (FT_Load_Char(g_game_font, codepoint, FT_LOAD_RENDER)) continue;

        const FT_GlyphSlot glyph = g_game_font->glyph;
        const int left = (int) floorf(x) + glyph->bitmap_left;
        const int top = (int) floorf(baseline) - glyph->bitmap_top;

        for (int row = 0; row < (int) glyph->bitmap.rows; row++) {
            const int py = top + row;
            if (py < 0 || py >= height) continue;

            for (int col = 0; col < (int) glyph->bitmap.width; col++) {
                const int px = left + col;
                if (px < 0 || px >= width) continue;

                const float coverage = (float) glyph->bitmap.buffer[row * glyph->bitmap.pitch + col] / 255.0f;
                if (coverage <= 0.0f) continue;

                unsigned char *dst = rgba + ((size_t) py * (size_t) width + (size_t) px) * 4;
                const float dst_alpha = (float) dst[3] / 255.0f;
                const float out_alpha = coverage + dst_alpha * (1.0f - coverage);
                const float channels[3] = {color.x, color.y, color.z};

                for (int c = 0; c < 3; c++) {
                    const float dst_value = (float) dst[c] / 255.0f;
                    const float value = out_alpha > 0.0f ? (channels[c] * coverage + dst_value * dst_alpha * (1.0f - coverage)) / out_alpha : 0.0f;
                    dst[c] = (unsigned char) (fminf(fmaxf(value, 0.0f), 1.0f) * 255.0f + 0.5f);
                }

                dst[3] = (unsigned char) (out_alpha * 255.0f + 0.5f);
            }
        }

        x += (float) glyph->advance.x / 64.0f;
    }
}

// Draws one source line word-wrapped like 2.js getLines (maxWidth = canvas width - fontSize).
// Returns the baseline after the last drawn line.
static float prefab_sign_line(unsigned char *rgba, const int width, const int height, const char *line, const size_t length, const int align, const float font_size, const float anchor_x, float baseline, const float line_step, const vec4 color) {
    const char *end = line + length;
    const char *start = line;
    const char *cursor = memchr(line, ' ', length);
    if (!cursor) cursor = end;

    while (start <= end) {
        // grow the current line word by word while "line + ' ' + word" fits
        const char *line_end = cursor;

        while (line_end < end) {
            const char *next = memchr(line_end + 1, ' ', (size_t) (end - line_end - 1));
            if (!next) next = end;
            if (prefab_text_width(start, (size_t) (next - start)) >= (float) width - font_size) break;
            line_end = next;
        }

        const size_t line_length = (size_t) (line_end - start);
        const float line_width = prefab_text_width(start, line_length);
        const float x = align == 2 ? anchor_x - line_width : align == 1 ? anchor_x - line_width * 0.5f : anchor_x;

        prefab_text_draw(rgba, width, height, start, line_length, floorf(x), floorf(baseline), color);
        baseline += line_step;
        if (baseline > (float) height) return baseline;

        if (line_end >= end) break;
        start = line_end + 1;
        cursor = memchr(start, ' ', (size_t) (end - start));
        if (!cursor) cursor = end;
    }

    return baseline;
}

// Uploads a top-row-first RGBA canvas as a nearest-filtered, repeating texture (55.js
// loadTexture canvas path) and caches it in g_texture_cache under `key`.
static unsigned int prefab_upload_canvas(const unsigned char *rgba, const int width, const int height, const char *key) {
    // GL rows run bottom-up (three.js flipY)
    const size_t row = (size_t) width * 4;
    unsigned char *flipped = malloc(row * (size_t) height);

    if (flipped) {
        for (int y = 0; y < height; y++) memcpy(flipped + (size_t) y * row, rgba + (size_t) (height - 1 - y) * row, row);
    }

    unsigned int texture_id = 0;
    glGenTextures(1, &texture_id);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture_id);
    g_active_texture = texture_id;

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, flipped ? flipped : rgba);
    free(flipped);

    char *cache_key = malloc(strlen(key) + 1);
    if (cache_key) {
        strcpy(cache_key, key);
        vt_insert(&g_texture_cache, cache_key, texture_id);
    }

    return texture_id;
}

static unsigned int prefab_sign_texture(const Object *object, const cJSON *raw_obj) {
    const cJSON *raw_text = cJSON_GetObjectItem(raw_obj, "st");
    const char *text = cJSON_IsString(raw_text) ? cJSON_GetStringValue(raw_text) : "Hello World";
    int font_size = (int) prefab_json_float(cJSON_GetObjectItem(raw_obj, "sf"), 0.0f); // parseInt(sf) || 10
    if (font_size <= 0) font_size = 10;
    const int align = (int) prefab_json_float(cJSON_GetObjectItem(raw_obj, "sa"), 0.0f);
    const cJSON *raw_fg = cJSON_GetObjectItem(raw_obj, "sc");
    const cJSON *raw_bg = cJSON_GetObjectItem(raw_obj, "sb");
    const int no_background = prefab_json_present(cJSON_GetObjectItem(raw_obj, "bv")) && cJSON_GetNumberValue(cJSON_GetObjectItem(raw_obj, "bv")) != 0;

    vec4 fg = {0.0f, 0.0f, 0.0f, 1.0f};
    vec4 bg = {1.0f, 1.0f, 1.0f, 1.0f};
    if (cJSON_IsString(raw_fg)) parse_hex_color(cJSON_GetStringValue(raw_fg), &fg);
    if (cJSON_IsString(raw_bg)) parse_hex_color(cJSON_GetStringValue(raw_bg), &bg);

    // canvas: 1024 px along the longer side of the s[0] x s[2] sign
    const float sx = object->scale.x;
    const float sz = object->scale.z;
    const int width = sz > sx && sz > 0.0f ? (int) (sx / sz * 1024.0f) : 1024;
    const int height = sz < sx && sx > 0.0f ? (int) (sz / sx * 1024.0f) : 1024;
    if (width <= 0 || height <= 0 || !g_game_font) return 0;

    char key[512];
    snprintf(key, sizeof(key), "sign:%dx%d:%d:%d:%d:%.3f,%.3f,%.3f:%.3f,%.3f,%.3f:%s", width, height, font_size, align, no_background, fg.x, fg.y, fg.z, bg.x, bg.y, bg.z, text);

    texture_cache_map_itr cached = vt_get(&g_texture_cache, key);
    if (!vt_is_end(cached)) return cached.data->val;

    unsigned char *rgba = calloc((size_t) width * (size_t) height, 4);
    if (!rgba) return 0;

    if (!no_background) {
        for (size_t i = 0; i < (size_t) width * (size_t) height; i++) {
            rgba[i * 4] = (unsigned char) (bg.x * 255.0f + 0.5f);
            rgba[i * 4 + 1] = (unsigned char) (bg.y * 255.0f + 0.5f);
            rgba[i * 4 + 2] = (unsigned char) (bg.z * 255.0f + 0.5f);
            rgba[i * 4 + 3] = 255;
        }
    } else {
        // transparent canvas pixels are (0, 0, 0, 0); keep the text colour under the fringe
        for (size_t i = 0; i < (size_t) width * (size_t) height; i++) {
            rgba[i * 4] = (unsigned char) (fg.x * 255.0f + 0.5f);
            rgba[i * 4 + 1] = (unsigned char) (fg.y * 255.0f + 0.5f);
            rgba[i * 4 + 2] = (unsigned char) (fg.z * 255.0f + 0.5f);
        }
    }

    FT_Set_Pixel_Sizes(g_game_font, 0, (unsigned int) font_size);

    const float font = (float) font_size;
    const float m_width = prefab_text_width("M", 1);
    const float anchor_x = align == 2 ? (float) width - m_width * 0.5f : align == 1 ? (float) width * 0.5f : m_width * 0.5f;
    const float line_step = m_width * 1.2f;
    float baseline = font * 2.0f;

    // lines are split on a literal backslash-n; text is capped at signTextLimit - 1 characters
    const size_t text_length = strlen(text) < 1499 ? strlen(text) : 1499;
    const char *line = text;
    const char *text_end = text + text_length;

    while (line <= text_end && baseline <= (float) height) {
        const char *split = line;
        while (split + 1 < text_end && !(split[0] == '\\' && split[1] == 'n')) split++;
        if (split + 1 >= text_end) split = text_end;

        baseline = prefab_sign_line(rgba, width, height, line, (size_t) (split - line), align, font, anchor_x, baseline, line_step, fg);
        if (split >= text_end) break;
        line = split + 2;
    }

    const unsigned int texture_id = prefab_upload_canvas(rgba, width, height, key);
    free(rgba);
    return texture_id;
}

// "sad" billboards show 2.js createCanvasAD(text, textCol, bgCol) instead of their poster:
// a 2048x1024 canvas filled with bgCol and the text drawn by drawMultilineText at the
// largest font that fits. Offline there is no server billboard config, so the defaults
// ("Your Ad Here", #e3e3e3 on #000000) apply.
static unsigned int prefab_ad_texture() {
    static const char *text = "Your Ad Here";
    const int width = 2048;
    const int height = 1024;
    const vec4 fg = {227.0f / 255.0f, 227.0f / 255.0f, 227.0f / 255.0f, 1.0f};

    texture_cache_map_itr cached = vt_get(&g_texture_cache, "billboard-ad:Your Ad Here");
    if (!vt_is_end(cached)) return cached.data->val;
    if (!g_game_font) return 0;

    // word list (drawMultilineText splits on single spaces)
    const char *words[16];
    size_t word_lengths[16];
    int word_count = 0;

    for (const char *w = text; *w && word_count < 16;) {
        const char *space = strchr(w, ' ');
        const size_t length = space ? (size_t) (space - w) : strlen(w);
        words[word_count] = w;
        word_lengths[word_count++] = length;
        w += length + (space ? 1 : 0);
    }

    // lines[i] = [first word, word count] for the chosen size
    int lines[16][2];
    int line_count = 0;
    int best_size = 0;
    int size = 15;

    for (; size <= 2000; size++) {
        const float line_height = (float) size * 1.4f;
        float text_height = line_height;
        int try_lines[16][2];
        int try_count = 0;
        int first = 0;

        FT_Set_Pixel_Sizes(g_game_font, 0, (unsigned int) size);

        if (prefab_text_width(words[0], word_lengths[0]) > (float) width - 80.0f) {
            best_size = size - 1;
            break;
        }

        for (int i = 1; i < word_count; i++) {
            const size_t joined = (size_t) (words[i] + word_lengths[i] - words[first]);
            if (prefab_text_width(words[first], joined) >= (float) width - 80.0f) {
                try_lines[try_count][0] = first;
                try_lines[try_count++][1] = i - first;
                first = i;
                text_height += line_height;
            }
        }

        try_lines[try_count][0] = first;
        try_lines[try_count++][1] = word_count - first;

        if (text_height > (float) height - 80.0f) {
            best_size = size - 1;
            break;
        }

        memcpy(lines, try_lines, sizeof(try_lines));
        line_count = try_count;
        best_size = size;
    }

    unsigned char *rgba = calloc((size_t) width * (size_t) height, 4);
    if (!rgba) return 0;
    for (size_t i = 0; i < (size_t) width * (size_t) height; i++) rgba[i * 4 + 3] = 255; // #000000

    // layout uses the size that failed (C), the glyphs the last size that fit (B)
    const float line_height = (float) size * 1.4f;
    float x = (float) width * 0.5f;
    float y = (float) height * 0.5f;
    if (line_count > 1) y -= line_height * (float) (line_count - 1) * 0.5f;
    if (line_count) {
        x += 40.0f / (float) line_count;
        y += 80.0f / (float) line_count;
    }

    FT_Set_Pixel_Sizes(g_game_font, 0, (unsigned int) (best_size > 0 ? best_size : 15));

    for (int i = 0; i < line_count; i++) {
        const char *start = words[lines[i][0]];
        const int last = lines[i][0] + lines[i][1] - 1;
        const size_t length = (size_t) (words[last] + word_lengths[last] - start);
        const float line_width = prefab_text_width(start, length);

        prefab_text_draw(rgba, width, height, start, length, floorf(x - line_width * 0.5f), floorf(y + (float) size * 0.5f), fg);
        y += line_height;
    }

    const unsigned int texture_id = prefab_upload_canvas(rgba, width, height, "billboard-ad:Your Ad Here");
    free(rgba);
    return texture_id;
}

static Geometry *prefab_cube_geo(const int face_mask) {
    static Geometry *cached[64];
    static const vertex vertices[] = {
        // FRONT (Krunker face 4)
        {-0.5f, 0.0f, 0.5f, 0.0f, 0.0f},
        {0.5f, 1.0f, 0.5f, 1.0f, 1.0f},
        {-0.5f, 1.0f, 0.5f, 0.0f, 1.0f},
        {0.5f, 0.0f, 0.5f, 1.0f, 0.0f},

        // BACK (Krunker face 5)
        {0.5f, 0.0f, -0.5f, 0.0f, 0.0f},
        {-0.5f, 1.0f, -0.5f, 1.0f, 1.0f},
        {0.5f, 1.0f, -0.5f, 0.0f, 1.0f},
        {-0.5f, 0.0f, -0.5f, 1.0f, 0.0f},

        // LEFT (Krunker face 1)
        {-0.5f, 0.0f, -0.5f, 0.0f, 0.0f},
        {-0.5f, 1.0f, 0.5f, 1.0f, 1.0f},
        {-0.5f, 1.0f, -0.5f, 0.0f, 1.0f},
        {-0.5f, 0.0f, 0.5f, 1.0f, 0.0f},

        // RIGHT (Krunker face 0)
        {0.5f, 0.0f, 0.5f, 0.0f, 0.0f},
        {0.5f, 1.0f, -0.5f, 1.0f, 1.0f},
        {0.5f, 1.0f, 0.5f, 0.0f, 1.0f},
        {0.5f, 0.0f, -0.5f, 1.0f, 0.0f},

        // TOP (Krunker face 2)
        {-0.5f, 1.0f, 0.5f, 0.0f, 0.0f},
        {0.5f, 1.0f, -0.5f, 1.0f, 1.0f},
        {-0.5f, 1.0f, -0.5f, 0.0f, 1.0f},
        {0.5f, 1.0f, 0.5f, 1.0f, 0.0f},

        // BOTTOM (Krunker face 3)
        {-0.5f, 0.0f, -0.5f, 0.0f, 0.0f},
        {0.5f, 0.0f, 0.5f, 1.0f, 1.0f},
        {-0.5f, 0.0f, 0.5f, 0.0f, 1.0f},
        {0.5f, 0.0f, -0.5f, 1.0f, 0.0f},
    };
    static const unsigned int face_indices[][6] = {
        {0, 1, 2, 0, 3, 1},
        {4, 5, 6, 4, 7, 5},
        {8, 9, 10, 8, 11, 9},
        {12, 13, 14, 12, 15, 13},
        {16, 17, 18, 16, 19, 17},
        {20, 21, 22, 20, 23, 21},
    };
    static const int krunker_to_native[] = {3, 2, 4, 5, 0, 1};

    if (face_mask == 0x3f) return create_cube_geo();
    if (face_mask >= 0 && face_mask < 64 && cached[face_mask]) return cached[face_mask];

    unsigned int indices[36];
    size_t index_count = 0;

    for (int i = 0; i < 6; i++) {
        if (!(face_mask & (1 << i))) continue;

        const int native_face = krunker_to_native[i];
        memcpy(indices + index_count, face_indices[native_face], sizeof(face_indices[native_face]));
        index_count += 6;
    }

    Geometry *geometry = calloc(1, sizeof(Geometry));
    if (!geometry) return NULL;

    unsigned int vbo;
    glGenVertexArrays(1, &geometry->vao);
    glGenBuffers(1, &geometry->ebo);
    glGenBuffers(1, &vbo);

    glBindVertexArray(geometry->vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    geometry_set_bounds(geometry, vertices, sizeof(vertices) / sizeof(vertices[0]));

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, geometry->ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (int) (index_count * sizeof(unsigned int)), indices, GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), NULL);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *) (3 * sizeof(float)));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);

    geometry->index_count = (int) index_count;
    if (face_mask >= 0 && face_mask < 64) cached[face_mask] = geometry;
    return geometry;
}

static Geometry *prefab_cylinder_geo(const int segments, const int cone) {
    static Geometry *cached_cylinder;
    static Geometry *cached_cone;

    if (!cone && cached_cylinder) return cached_cylinder;
    if (cone && cached_cone) return cached_cone;

    const int side_vertices = segments * 4;
    const int cap_vertices = cone ? 0 : (segments + 1) * 2;
    const int vertex_count = side_vertices + cap_vertices;
    const int index_count = segments * 6 + (cone ? 0 : segments * 6);

    vertex *vertices = calloc(vertex_count, sizeof(vertex));
    unsigned int *indices = calloc(index_count, sizeof(unsigned int));

    if (!vertices || !indices) {
        free(vertices);
        free(indices);
        return NULL;
    }

    int v = 0;
    int idx = 0;

    for (int i = 0; i < segments; i++) {
        const float a0 = (float) i / (float) segments * 2.0f * (float) M_PI;
        const float a1 = (float) (i + 1) / (float) segments * 2.0f * (float) M_PI;
        // THREE.CylinderGeometry: x = sin(theta), z = cos(theta), u = theta / 2PI
        const float x0 = sinf(a0) * 0.5f;
        const float z0 = cosf(a0) * 0.5f;
        const float x1 = sinf(a1) * 0.5f;
        const float z1 = cosf(a1) * 0.5f;
        const float u0 = (float) i / (float) segments;
        const float u1 = (float) (i + 1) / (float) segments;
        const int base = v;

        vertices[v++] = (vertex) {x0, 0.0f, z0, u0, 0.0f};
        vertices[v++] = (vertex) {cone ? 0.0f : x0, 1.0f, cone ? 0.0f : z0, u0, 1.0f};
        vertices[v++] = (vertex) {cone ? 0.0f : x1, 1.0f, cone ? 0.0f : z1, u1, 1.0f};
        vertices[v++] = (vertex) {x1, 0.0f, z1, u1, 0.0f};

        indices[idx++] = base;
        indices[idx++] = base + 1;
        indices[idx++] = base + 2;
        indices[idx++] = base;
        indices[idx++] = base + 2;
        indices[idx++] = base + 3;
    }

    if (!cone) {
        const int bottom_center = v;
        vertices[v++] = (vertex) {0.0f, 0.0f, 0.0f, 0.5f, 0.5f};

        for (int i = 0; i < segments; i++) {
            const float a = (float) i / (float) segments * 2.0f * (float) M_PI;
            vertices[v++] = (vertex) {cosf(a) * 0.5f, 0.0f, sinf(a) * 0.5f, cosf(a) * 0.5f + 0.5f, sinf(a) * 0.5f + 0.5f};
        }

        for (int i = 0; i < segments; i++) {
            indices[idx++] = bottom_center;
            indices[idx++] = bottom_center + 1 + ((i + 1) % segments);
            indices[idx++] = bottom_center + 1 + i;
        }

        const int top_center = v;
        vertices[v++] = (vertex) {0.0f, 1.0f, 0.0f, 0.5f, 0.5f};

        for (int i = 0; i < segments; i++) {
            const float a = (float) i / (float) segments * 2.0f * (float) M_PI;
            vertices[v++] = (vertex) {cosf(a) * 0.5f, 1.0f, sinf(a) * 0.5f, cosf(a) * 0.5f + 0.5f, sinf(a) * 0.5f + 0.5f};
        }

        for (int i = 0; i < segments; i++) {
            indices[idx++] = top_center;
            indices[idx++] = top_center + 1 + i;
            indices[idx++] = top_center + 1 + ((i + 1) % segments);
        }
    }

    Geometry *geometry = calloc(1, sizeof(Geometry));
    if (!geometry) {
        free(vertices);
        free(indices);
        return NULL;
    }

    unsigned int vbo;
    glGenVertexArrays(1, &geometry->vao);
    glGenBuffers(1, &geometry->ebo);
    glGenBuffers(1, &vbo);

    glBindVertexArray(geometry->vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, vertex_count * sizeof(vertex), vertices, GL_STATIC_DRAW);
    geometry_set_bounds(geometry, vertices, (size_t) vertex_count);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, geometry->ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, index_count * sizeof(unsigned int), indices, GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), NULL);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *) (3 * sizeof(float)));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);

    geometry->index_count = index_count;
    if (cone) cached_cone = geometry;
    else cached_cylinder = geometry;

    free(vertices);
    free(indices);
    return geometry;
}

static Geometry *prefab_sphere_geo() {
    static Geometry *cached;
    if (cached) return cached;

    // THREE.SphereGeometry(1, 12, 8) (31.js generateSphere): 12 segments, 8 rings, v = 1 at the top
    const int rings = 8;
    const int segments = 12;
    const int vertex_count = (rings + 1) * (segments + 1);
    const int index_count = rings * segments * 6;

    vertex *vertices = calloc(vertex_count, sizeof(vertex));
    unsigned int *indices = calloc(index_count, sizeof(unsigned int));

    if (!vertices || !indices) {
        free(vertices);
        free(indices);
        return NULL;
    }

    int v = 0;
    for (int y = 0; y <= rings; y++) {
        const float vy = (float) y / (float) rings;
        const float phi = vy * (float) M_PI;

        for (int x = 0; x <= segments; x++) {
            const float vx = (float) x / (float) segments;
            const float theta = vx * 2.0f * (float) M_PI;
            vertices[v++] = (vertex) {
                -cosf(theta) * sinf(phi) * 0.5f,
                0.5f + cosf(phi) * 0.5f,
                sinf(theta) * sinf(phi) * 0.5f,
                vx,
                1.0f - vy,
            };
        }
    }

    int idx = 0;
    for (int y = 0; y < rings; y++) {
        for (int x = 0; x < segments; x++) {
            const int a = y * (segments + 1) + x;
            const int b = a + segments + 1;

            indices[idx++] = a;
            indices[idx++] = b;
            indices[idx++] = a + 1;
            indices[idx++] = a + 1;
            indices[idx++] = b;
            indices[idx++] = b + 1;
        }
    }

    Geometry *geometry = calloc(1, sizeof(Geometry));
    if (!geometry) {
        free(vertices);
        free(indices);
        return NULL;
    }

    unsigned int vbo;
    glGenVertexArrays(1, &geometry->vao);
    glGenBuffers(1, &geometry->ebo);
    glGenBuffers(1, &vbo);

    glBindVertexArray(geometry->vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, vertex_count * sizeof(vertex), vertices, GL_STATIC_DRAW);
    geometry_set_bounds(geometry, vertices, (size_t) vertex_count);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, geometry->ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, index_count * sizeof(unsigned int), indices, GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), NULL);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *) (3 * sizeof(float)));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);

    geometry->index_count = index_count;
    cached = geometry;

    free(vertices);
    free(indices);
    return geometry;
}

// 55.js loadMesh merges every instance of a model that is queued before its OBJ finishes
// loading (in practice the whole map) into one mesh that uses the FIRST instance's
// material: each model type renders with the colour/opacity of its first map object.
// Destructible instances (health / cdy) keep their own material.
typedef struct {
    int seen;
    vec4 color;
} PrefabModelMaterial;

static PrefabModelMaterial g_prefab_model_materials[sizeof(prefab_models) / sizeof(prefab_models[0])];
static const cJSON *g_prefab_map_objects; // the raw objects of the map being built

// ---- load-time prefetch (web): the textures and models prefab_init() is about to open,
// collected from the raw objects and downloaded as one parallel batch (asset_prefetch_wait).
// A hint only: whatever this misses is still fetched on demand, extras are just unused, and
// it never draws random numbers (billboards without "bb" are left to prefab_init()).
#ifdef __EMSCRIPTEN__
static int prefab_model_glows(int prefab);

typedef struct {
    char **paths;
    size_t count;
    size_t capacity;
} PrefabPrefetch;

static void prefab_prefetch_add(PrefabPrefetch *list, const char *format, const char *name) {
    char relative[128];
    snprintf(relative, sizeof(relative), format, name);

    char *path = concat(assets_path(), relative);
    if (!path) return;

    for (size_t i = 0; i < list->count; i++) {
        if (!strcmp(list->paths[i], path)) {
            free(path);
            return;
        }
    }

    if (list->count == list->capacity) {
        const size_t capacity = list->capacity ? list->capacity * 2 : 32;
        char **grown = realloc(list->paths, capacity * sizeof(char *));
        if (!grown) {
            free(path);
            return;
        }
        list->paths = grown;
        list->capacity = capacity;
    }

    list->paths[list->count++] = path;
}

static void prefab_prefetch_object(PrefabPrefetch *list, const cJSON *raw_obj, const ClientRenderSettings *settings) {
    const cJSON *raw_prefab = cJSON_GetObjectItem(raw_obj, "i");
    const int prefab = cJSON_IsNumber(raw_prefab) ? (int) cJSON_GetNumberValue(raw_prefab) : 0;
    if (prefab < 0 || prefab >= (int) (sizeof(prefab_models) / sizeof(prefab_models[0]))) return;
    if (prefab_hidden_in_game(prefab)) return;
    if (!settings->map_details && prefab_needs_map_details(prefab)) return;

    const char *model = prefab == PREFAB_BOOST_PAD ? "pad_0" : prefab_models[prefab].filename;
    if (model) {
        prefab_prefetch_add(list, "models/%s.obj", model);
        prefab_prefetch_add(list, "textures/%s.png", model);
        if (prefab_model_glows(prefab) && !settings->no_textures) prefab_prefetch_add(list, "textures/%s_e.png", model);
        return;
    }

    if (prefab == PREFAB_OBJECTIVE) {
        prefab_prefetch_add(list, "textures/%s.png", "objective_0");
        return;
    }
    if (settings->no_textures) return;

    if (prefab == PREFAB_LIGHT_CONE) {
        prefab_prefetch_add(list, "textures/%s.png", cJSON_GetObjectItem(raw_obj, "fc") == NULL ? "lightcone_0" : "lightcone_1");
        return;
    }
    if (prefab == PREFAB_BILLBOARD) {
        const cJSON *bb = cJSON_GetObjectItem(raw_obj, "bb");
        if (!prefab_json_present(cJSON_GetObjectItem(raw_obj, "sad")) && cJSON_IsNumber(bb) && cJSON_GetNumberValue(bb)) {
            char src[32];
            snprintf(src, sizeof(src), "pubs/b_%d", (int) cJSON_GetNumberValue(bb));
            prefab_prefetch_add(list, "textures/%s.png", src);
        }
        return;
    }
    if (prefab == PREFAB_SIGN) return;

    const cJSON *t = cJSON_GetObjectItem(raw_obj, "t");
    int tex_id = cJSON_IsString(t) ? prefab_texture_id_from_string(cJSON_GetStringValue(t)) : cJSON_IsNumber(t) ? (int) cJSON_GetNumberValue(t) : prefab == PREFAB_LADDER ? 2 : 0;
    if (tex_id < 0 || tex_id >= (int) (sizeof(prefab_textures) / sizeof(prefab_textures[0]))) tex_id = prefab == PREFAB_LADDER ? 2 : 0;
    if (strcmp(prefab_textures[tex_id].src, "default")) prefab_prefetch_add(list, "textures/%s.png", prefab_textures[tex_id].src);
}

static void prefab_prefetch_map_assets(const cJSON *objects) {
    const ClientRenderSettings *settings = client_render_settings();
    PrefabPrefetch list = {0};

    const cJSON *raw_obj;
    cJSON_ArrayForEach(raw_obj, objects) prefab_prefetch_object(&list, raw_obj, settings);
    // ambientShading planes (ambient.c) load right after the objects
    prefab_prefetch_add(&list, "textures/ambient_%s.png", "0");
    prefab_prefetch_add(&list, "textures/ambient_%s.png", "1");

    asset_prefetch_wait((const char *const *) list.paths, list.count);
    for (size_t i = 0; i < list.count; i++) free(list.paths[i]);
    free(list.paths);
}
#else
static void prefab_prefetch_map_assets(const cJSON *objects) {
    (void) objects; // native files are local: nothing to batch
}
#endif

void prefab_begin_map(const cJSON *objects) {
    memset(g_prefab_model_materials, 0, sizeof(g_prefab_model_materials));
    g_prefab_map_objects = objects;
    prefab_prefetch_map_assets(objects);
}

static int prefab_raw_is_cube(const cJSON *raw) {
    const cJSON *i = cJSON_GetObjectItem(raw, "i");
    if (!i) i = cJSON_GetObjectItem(raw, "id");
    if (!i || cJSON_IsNull(i)) return 1; // 93.js: i || id || 0
    if (cJSON_IsNumber(i)) return (int) cJSON_GetNumberValue(i) == PREFAB_CUBE;
    return cJSON_IsString(i) && prefab_texture_string_equals(cJSON_GetStringValue(i), "CUBE");
}

// 93.js: a cube whose base lies within another cube's height range and whose footprint is
// strictly inside that cube's footprint gets no bottom face (faces[3] = 0).
static int prefab_cube_rests_inside(const cJSON *self, const vec3 p, const vec3 s) {
    if (!cJSON_IsArray(g_prefab_map_objects)) return 0;

    const cJSON *other;
    cJSON_ArrayForEach(other, g_prefab_map_objects) {
        if (other == self || !prefab_raw_is_cube(other)) continue;

        const cJSON *op = cJSON_GetObjectItem(other, "p");
        const cJSON *os = cJSON_GetObjectItem(other, "s");
        if (cJSON_GetArraySize(op) < 3 || cJSON_GetArraySize(os) < 3) continue;

        const double ox = prefab_json_float(cJSON_GetArrayItem(op, 0), 0.0f), oy = prefab_json_float(cJSON_GetArrayItem(op, 1), 0.0f), oz = prefab_json_float(cJSON_GetArrayItem(op, 2), 0.0f);
        const double sx = prefab_json_float(cJSON_GetArrayItem(os, 0), 0.0f), sy = prefab_json_float(cJSON_GetArrayItem(os, 1), 0.0f), sz = prefab_json_float(cJSON_GetArrayItem(os, 2), 0.0f);

        if (p.y <= oy + sy && p.y >= oy &&
            p.x + s.x / 2.0 < ox + sx / 2.0 && p.x - s.x / 2.0 > ox - sx / 2.0 &&
            p.z + s.z / 2.0 < oz + sz / 2.0 && p.z - s.z / 2.0 > oz - sz / 2.0) {
            return 1;
        }
    }

    return 0;
}

// THREE.PlaneGeometry(1, 1, tiles_x, tiles_z): (tiles_x + 1) x (tiles_z + 1) vertices,
// row by row from the top (+y) like three.js, so a vertex's index is its gl_VertexID (the
// liquid waves in basic.vert depend on it). Cached per grid size.
static Geometry *prefab_plane_grid_geo(int tiles_x, int tiles_z) {
    static struct { int x, z; Geometry *geometry; } cache[32];
    static int cache_count;

    if (tiles_x < 1) tiles_x = 1;
    if (tiles_z < 1) tiles_z = 1;
    if (tiles_x > 256) tiles_x = 256;
    if (tiles_z > 256) tiles_z = 256;

    for (int i = 0; i < cache_count; i++) {
        if (cache[i].x == tiles_x && cache[i].z == tiles_z) return cache[i].geometry;
    }

    const int columns = tiles_x + 1;
    const int vertex_count = columns * (tiles_z + 1);
    const int index_count = tiles_x * tiles_z * 6;
    vertex *vertices = calloc((size_t) vertex_count, sizeof(vertex));
    unsigned int *indices = calloc((size_t) index_count, sizeof(unsigned int));
    Geometry *geometry = NULL;

    if (vertices && indices) {
        for (int iy = 0; iy <= tiles_z; iy++) {
            for (int ix = 0; ix <= tiles_x; ix++) {
                const float x = (float) ix / (float) tiles_x - 0.5f;
                const float y = 0.5f - (float) iy / (float) tiles_z;
                vertices[iy * columns + ix] = (vertex) {x, y, 0.0f, x + 0.5f, y + 0.5f};
            }
        }

        int k = 0;
        for (int iy = 0; iy < tiles_z; iy++) {
            for (int ix = 0; ix < tiles_x; ix++) {
                const unsigned int a = (unsigned int) (ix + columns * iy);
                const unsigned int b = (unsigned int) (ix + columns * (iy + 1));
                const unsigned int c = (unsigned int) (ix + 1 + columns * (iy + 1));
                const unsigned int d = (unsigned int) (ix + 1 + columns * iy);
                indices[k++] = a; indices[k++] = b; indices[k++] = d;
                indices[k++] = b; indices[k++] = c; indices[k++] = d;
            }
        }

        geometry = prefab_upload_geo(vertices, vertex_count, indices, index_count);
        if (geometry) geometry->bound_radius = 0.0f; // liquid waves displace it in basic.vert: never culled
    }

    free(vertices);
    free(indices);

    if (geometry && cache_count < 32) {
        cache[cache_count].x = tiles_x;
        cache[cache_count].z = tiles_z;
        cache[cache_count++].geometry = geometry;
    }

    return geometry;
}

// 29.js prefabs with emiss: true: loadMesh emissive 0xffffff with the "<src>_e" emissiveMap
static int prefab_model_glows(const int prefab) {
    return prefab == PREFAB_VEHICLE || prefab == PREFAB_ACID_BARREL || prefab == PREFAB_GHOST || prefab == PREFAB_PUMPKIN;
}

static Mesh *prefab_model_mesh(const char *name) {
    char model_path[128];
    char texture_path[128];

    snprintf(model_path, sizeof(model_path), "models/%s.obj", name);
    snprintf(texture_path, sizeof(texture_path), "textures/%s.png", name);

    char *full_model_path = concat(assets_path(), model_path);
    char *full_texture_path = concat(assets_path(), texture_path);

    const unsigned int texture_id = full_texture_path ? load_texture(full_texture_path) : 0;
    // Krunker keeps the model origin (94.js addMesh places it at p.y + s.y / 2): no re-basing
    Geometry *geometry = full_model_path ? load_obj_model(full_model_path, 0) : NULL;

    free(full_model_path);
    free(full_texture_path);

    if (!geometry) return NULL;

    BasicMaterial *material = basic_material_init();
    if (!material) return NULL;

    Mesh *mesh = mesh_init(geometry, (Material *) material);
    material->texture = texture_id;
    return mesh;
}

static unsigned int prefab_load_texture_src(const char *src) {
    char texture_path[128];
    snprintf(texture_path, sizeof(texture_path), "textures/%s.png", src);

    char *full_texture_path = concat(assets_path(), texture_path);
    if (!full_texture_path) return 0;

    const unsigned int texture_id = load_texture(full_texture_path);
    free(full_texture_path);
    return texture_id;
}

static vec4 prefab_json_color(const cJSON *raw_color, const cJSON *raw_color_idx, const vec4 *colors, const vec4 fallback) {
    // 93.js: a "ci"/"ei" palette index replaces "c"/"e"
    if (cJSON_IsNumber(raw_color_idx) && colors) return colors[(int) cJSON_GetNumberValue(raw_color_idx)];

    vec4 color = fallback;
    if (cJSON_IsString(raw_color)) parse_hex_color(cJSON_GetStringValue(raw_color), &color);
    else if (cJSON_IsNumber(raw_color)) color = hex_to_vec((int) cJSON_GetNumberValue(raw_color));
    return color;
}

Mesh *prefab_init(Object *object, const vec4 *colors, const cJSON *raw_obj) {
    if (object->prefab < 0 || object->prefab >= sizeof(prefab_models) / sizeof(prefab_models[0])) {
        return NULL;
    }
    if (prefab_hidden_in_game(object->prefab)) return NULL;

    // noTex / mapDet are restart settings in v3.1.1: read once when the map is built
    const ClientRenderSettings *render_settings = client_render_settings();
    const int no_textures = render_settings->no_textures;
    if (!render_settings->map_details && prefab_needs_map_details(object->prefab)) return NULL;
    // 55.js addCylinder/addCone build nothing with noTex
    if (no_textures && (object->prefab == PREFAB_CYLINDER || object->prefab == PREFAB_LIGHT_CONE)) return NULL;

    const PrefabModel prefab_model = prefab_models[object->prefab];

    const cJSON *raw_visibility = cJSON_GetObjectItem(raw_obj, "v");
    const cJSON *raw_rotation = cJSON_GetObjectItem(raw_obj, "r");
    const cJSON *raw_opacity = cJSON_GetObjectItem(raw_obj, "o");
    const cJSON *raw_model_size = cJSON_GetObjectItem(raw_obj, "ms");
    // "tsr" (texture stretch): no world-scaled UVs, the texture covers each face once
    const int stretch_texture = prefab_json_present(cJSON_GetObjectItem(raw_obj, "tsr"));

    const float opacity = prefab_json_float(raw_opacity, 1.0f);

    const vec4 white = {1.0f, 1.0f, 1.0f, 1.0f};
    const vec4 none = {0.0f, 0.0f, 0.0f, 0.0f};
    vec4 color = prefab_json_color(cJSON_GetObjectItem(raw_obj, "c"), cJSON_GetObjectItem(raw_obj, "ci"), colors, white);
    const vec4 emissive = prefab_json_color(cJSON_GetObjectItem(raw_obj, "e"), cJSON_GetObjectItem(raw_obj, "ei"), colors, none);
    color.w = opacity;

    const int visible = cJSON_GetNumberValue(raw_visibility) != 1;
    vec3 rotation = {0};

    if (cJSON_IsArray(raw_rotation)) {
        rotation.x = (float) cJSON_GetNumberValue(cJSON_GetArrayItem(raw_rotation, 0));
        rotation.y = (float) cJSON_GetNumberValue(cJSON_GetArrayItem(raw_rotation, 1));
        rotation.z = (float) cJSON_GetNumberValue(cJSON_GetArrayItem(raw_rotation, 2));

        if (isnan(rotation.x)) rotation.x = 0.0f;
        if (isnan(rotation.y)) rotation.y = 0.0f;
        if (isnan(rotation.z)) rotation.z = 0.0f;
    }

    if (prefab_model.frames) {
        TextureAnimation *tex_anim = object->tex_anim;

        if (!tex_anim) {
            tex_anim = calloc(1, sizeof(TextureAnimation));
            object->tex_anim = tex_anim;
        }

        if (tex_anim) {
            tex_anim->frames = prefab_model.frames;
            tex_anim->frame_time = prefab_model.frame_time;
        }
    }

    // Models (94.js addMesh -> 55.js loadMesh) and boost pads (94.js addPad: pad_0, scale 4)
    if (prefab_model.filename || object->prefab == PREFAB_BOOST_PAD) {
        const int pad = object->prefab == PREFAB_BOOST_PAD;
        Mesh *mesh = prefab_model_mesh(pad ? "pad_0" : prefab_model.filename);
        if (!mesh) return NULL;

        BasicMaterial *material = (BasicMaterial *) mesh->material;
        float model_scale = pad ? 4.0f : prefab_model.scale;
        if (no_textures) material->texture = 0; // 55.js add(): getMat("default")

        if (!pad && cJSON_IsNumber(raw_model_size) && cJSON_GetNumberValue(raw_model_size)) {
            model_scale = fminf(fmaxf((float) cJSON_GetNumberValue(raw_model_size), 0.1f), 100.0f);
        }

        const int destructible = prefab_json_float(cJSON_GetObjectItem(raw_obj, "h"), 0.0f) != 0.0f || prefab_json_float(cJSON_GetObjectItem(raw_obj, "hp"), 0.0f) != 0.0f || prefab_json_present(cJSON_GetObjectItem(raw_obj, "cdy"));
        PrefabModelMaterial *shared = &g_prefab_model_materials[object->prefab];

        if (!destructible) {
            if (!shared->seen) {
                shared->seen = 1;
                shared->color = color;
            }

            color = shared->color;
        }

        mesh->visible = visible;
        mesh->transform.position = object->position;
        if (!pad) mesh->transform.position.y += object->scale.y * 0.5f;
        mesh->transform.rotation = rotation;
        mesh->transform.scale = (vec3) {model_scale, model_scale, model_scale};

        material->base.transparent = prefab_model.transparent || color.w != 1.0f;
        material->color = color; // loadMesh color: colr (no getColor saturation for models)

        if (prefab_model_glows(object->prefab) && !no_textures) {
            char emissive_src[96];
            snprintf(emissive_src, sizeof(emissive_src), "%s_e", prefab_model.filename);
            material->emissive = white;
            material->emissive_texture = prefab_load_texture_src(emissive_src);
        }
        // models take no map emissive: only "emiss" prefabs glow, through their _e textures
        material->double_sided = pad || object->prefab == PREFAB_GRASS;
        material->alpha_test = object->prefab == PREFAB_GRASS ? 0.02f : 0.0f;
        prefab_apply_lighting_controls(material, object);

        return mesh;
    }

    const cJSON *t = cJSON_GetObjectItem(raw_obj, "t");
    int tex_id = cJSON_IsString(t) ? prefab_texture_id_from_string(cJSON_GetStringValue(t)) : cJSON_IsNumber(t) ? (int) cJSON_GetNumberValue(t) : object->prefab == PREFAB_LADDER ? 2 : 0;
    if (tex_id < 0 || tex_id >= (int) (sizeof(prefab_textures) / sizeof(prefab_textures[0]))) tex_id = object->prefab == PREFAB_LADDER ? 2 : 0;

    const char *texture_src = prefab_textures[tex_id].src;
    const int link_texture = !strcmp(texture_src, "link_0");
    unsigned int texture_id = 0;

    if (object->prefab == PREFAB_LIGHT_CONE) {
        texture_id = prefab_load_texture_src(cJSON_GetObjectItem(raw_obj, "fc") == NULL ? "lightcone_0" : "lightcone_1");
    } else if (object->prefab == PREFAB_BILLBOARD && prefab_json_present(cJSON_GetObjectItem(raw_obj, "sad"))) {
        texture_id = prefab_ad_texture();
    } else if (object->prefab == PREFAB_BILLBOARD) {
        const cJSON *bb = cJSON_GetObjectItem(raw_obj, "bb");
        int billboard_id = cJSON_IsNumber(bb) ? (int) cJSON_GetNumberValue(bb) : 0;

        if (!billboard_id) billboard_id = (int) (pcg32_boundedrand(game_constants.billboard_count) % game_constants.billboard_count) + 1;

        char src[32];
        snprintf(src, sizeof(src), "pubs/b_%d", billboard_id);
        texture_id = prefab_load_texture_src(src);
    } else if (object->prefab == PREFAB_SIGN) {
        texture_id = prefab_sign_texture(object, raw_obj);
    } else if (strcmp(texture_src, "default") && !no_textures) {
        texture_id = prefab_load_texture_src(texture_src);
    }

    // noTex: map materials use "default"; sign and "Your Ad Here" canvases are kept
    const int canvas_texture = object->prefab == PREFAB_SIGN || (object->prefab == PREFAB_BILLBOARD && prefab_json_present(cJSON_GetObjectItem(raw_obj, "sad")));
    if (no_textures && !canvas_texture) texture_id = 0;

    const cJSON *ts = cJSON_GetObjectItem(raw_obj, "ts");
    const cJSON *td = cJSON_GetObjectItem(raw_obj, "td");

    if (cJSON_IsNumber(ts)) {
        TextureAnimation *tex_anim = object->tex_anim;

        if (!tex_anim) {
            tex_anim = calloc(1, sizeof(TextureAnimation));
            object->tex_anim = tex_anim;
        }

        if (tex_anim) {
            tex_anim->move = (float) cJSON_GetNumberValue(ts) / 10.0f;
            // 55.js render: movD == 0 scrolls offset.x, anything else offset.y
            tex_anim->move_direction = cJSON_IsNumber(td) && cJSON_GetNumberValue(td) != 0;
        }
    }

    const float world_uv = game_constants.world_uv_scale;

    if (object->prefab == PREFAB_OBJECTIVE) {
        // 94.js addObjective: a cube without top and bottom (faces 1,1,0,0,1,1), objective_0
        // stretched, MeshBasicMaterial, transparent, depthWrite false, DoubleSide. It starts
        // hidden; objective modes show the active one (mesh->visible, mode logic).
        Geometry *geometry = prefab_cube_geo(0x33);
        if (!geometry) return NULL;

        BasicMaterial *material = basic_material_init();
        Mesh *mesh = mesh_init(geometry, (Material *) material);

        mesh->visible = 0;
        mesh->transform.position = object->position;
        mesh->transform.scale = object->scale;

        material->uv_mode = BASIC_UV_CUBE;
        material->texture = prefab_load_texture_src("objective_0"); // forceTex: kept with noTex
        material->base.transparent = 1;
        material->double_sided = 1;
        material->no_depth_write = 1;
        material->unlit = 1;
        return mesh;
    }

    if (object->prefab == PREFAB_CUBE) {
        int face_mask = prefab_face_mask(cJSON_GetObjectItem(raw_obj, "f"));
        if (prefab_cube_rests_inside(raw_obj, object->position, object->scale)) face_mask &= ~(1 << 3);

        Geometry *geometry = prefab_cube_geo(face_mask);
        if (!geometry) return NULL;

        BasicMaterial *material = basic_material_init();
        Mesh *mesh = mesh_init(geometry, (Material *) material);
        const vec3 size = object->scale;

        mesh->visible = visible;
        mesh->transform.position = object->position;
        mesh->transform.rotation = rotation;
        mesh->transform.scale = size;

        // 31.js generateCube: per-face UVs scaled by size / worldUV (scale 1, 0 with tsr);
        // floor_0 rotates its UVs on cubes wider than tall (flipp) / wider than deep (flippW)
        const int floor_texture = !strcmp(texture_src, "floor_0");
        material->uv_mode = BASIC_UV_CUBE;
        material->uv_size = stretch_texture ? (vec3) {1.0f, 1.0f, 1.0f} : (vec3) {size.x / world_uv, size.y / world_uv, size.z / world_uv};
        material->uv_flip_sides = floor_texture && (size.x > size.y || size.z > size.y);
        material->uv_flip_top = floor_texture && size.x > size.z;

        // vertex colours: getColor(colr) on top, getColor(colr, ambientVal) at the bottom
        material->color = prefab_krunker_color(color, 1.0f);
        material->use_color_gradient = object->ambient_enabled;
        material->color_bottom = object->ambient_enabled ? prefab_krunker_color(color, PREFAB_AMBIENT_VAL) : material->color;

        material->texture = texture_id;
        material->texture_base_offset = prefab_texture_offset(raw_obj);
        material->emissive = emissive;
        material->base.transparent = link_texture || opacity != 1.0f;
        material->alpha_test = link_texture ? 0.02f : 0.0f;
        material->double_sided = 0;
        prefab_apply_lighting_controls(material, object);

        return mesh;
    }

    if (object->prefab == PREFAB_PLANE || object->prefab == PREFAB_BILLBOARD || object->prefab == PREFAB_SIGN || object->prefab == PREFAB_LIQUID) {
        const int liquid = object->prefab == PREFAB_LIQUID;
        // 93.js addLiquid: tilesX = parseInt(tx) || 10, tilesZ = parseInt(tz) || 10
        const int tiles_x = liquid ? (int) prefab_json_float(cJSON_GetObjectItem(raw_obj, "tx"), 0.0f) : 1;
        const int tiles_z = liquid ? (int) prefab_json_float(cJSON_GetObjectItem(raw_obj, "tz"), 0.0f) : 1;
        // noise planes ("en") get their bump geometry in map_post.c once every object exists
        Geometry *geometry = liquid ? prefab_plane_grid_geo(tiles_x ? tiles_x : 10, tiles_z ? tiles_z : 10) : prefab_plane_xy_geo();
        if (!geometry) return NULL;

        BasicMaterial *material = basic_material_init();
        Mesh *mesh = mesh_init(geometry, (Material *) material);
        const int sign = object->prefab == PREFAB_SIGN;

        // 93.js passes (r[2], r[0] - PI/2, r[1]) (signs: -r[2]), 94.js adds PI/2 back and
        // 55.js addPlane does rotateY(a) rotateX(b - PI/2) rotateZ(c) = Euler YXZ, then
        // scale(s[0], s[2], 1) on the XY unit plane.
        mesh->visible = visible;
        mesh->transform.position = object->position;
        mesh->transform.rotation_order = ROTATION_ORDER_YXZ;
        mesh->transform.rotation = (vec3) {rotation.x - (float) M_PI / 2.0f, sign ? -rotation.z : rotation.z, rotation.y};
        mesh->transform.scale = (vec3) {object->scale.x, object->scale.z, 1.0f};

        // 31.js generatePlane w(): u * (s[0] / 2) / worldUV * scale, v * (s[2] / 2) / worldUV * scale
        // with scale 2 for planes (0 with tsr) and liquids, 0 (stretched) for billboards/signs
        const float tex_scale = object->prefab == PREFAB_LIQUID ? 2.0f : object->prefab == PREFAB_PLANE && !stretch_texture ? 2.0f : 0.0f;
        material->uv_mode = BASIC_UV_PLANE;
        material->uv_size = tex_scale ? (vec3) {object->scale.x * 0.5f / world_uv * tex_scale, object->scale.z * 0.5f / world_uv * tex_scale, 1.0f} : (vec3) {1.0f, 1.0f, 1.0f};

        material->texture = texture_id;
        material->texture_base_offset = sign ? (vec2) {0} : prefab_texture_offset(raw_obj);
        material->color = sign ? white : prefab_krunker_color(color, 1.0f);
        material->color.w = sign ? 1.0f : opacity;
        material->emissive = sign ? none : emissive;
        // 94.js addPlane/addCanvas/addLiquid: transparent, DoubleSide (signs alphaTest 0.02)
        material->base.transparent = 1;
        material->double_sided = 1;
        material->alpha_test = sign ? 0.02f : 0.0f;

        if (object->prefab == PREFAB_PLANE && prefab_json_float(cJSON_GetObjectItem(raw_obj, "en"), 0.0f) > 0.0f) {
            // noise plane bumps carry getColor(colr, 0.6) (map_post.c builds the geometry)
            material->use_color_gradient = 1;
            material->color_bottom = prefab_krunker_color(color, 0.6f);
            material->color_bottom.w = opacity;
        }

        if (liquid) {
            // 93.js: wHeight = parseFloat(wh) || 0.5, wSpeed = parseFloat(ws) || 10
            const float height = prefab_json_float(cJSON_GetObjectItem(raw_obj, "wh"), 0.0f);
            const float speed = prefab_json_float(cJSON_GetObjectItem(raw_obj, "ws"), 0.0f);
            material->wave_height = height ? height : 0.5f;
            material->wave_speed = speed ? speed : 10.0f;
        }

        prefab_apply_lighting_controls(material, object);

        return mesh;
    }

    if (object->prefab == PREFAB_RAMP) {
        Geometry *geometry = prefab_plane_xy_geo();
        if (!geometry) return NULL;

        BasicMaterial *material = basic_material_init();
        Mesh *mesh = mesh_init(geometry, (Material *) material);

        // 94.js/55.js addRamp: one double-sided plane, width across the slope, scaled to the
        // slope length, centred at half height; rotateY(-PI/2 - d*PI/2), rotateX(asin(h/l) - PI/2)
        const int odd = object->direction % 2;
        const float width = odd ? object->scale.x : object->scale.z;
        const float length = odd ? object->scale.z : object->scale.x;
        const float height = object->scale.y;
        const float slant = sqrtf(height * height + length * length);

        mesh->visible = visible;
        mesh->transform.position = object->position;
        mesh->transform.position.y += height * 0.5f;
        mesh->transform.rotation_order = ROTATION_ORDER_YXZ;
        mesh->transform.rotation = (vec3) {
            (slant > 0.0f ? asinf(height / slant) : 0.0f) - (float) M_PI / 2.0f,
            -(float) M_PI / 2.0f * (1.0f + (float) object->direction),
            0.0f,
        };
        mesh->transform.scale = (vec3) {width, slant, 1.0f};

        // generatePlane(length, width) with scale 1 (0 with tsr): u = width / worldUV, v = length / worldUV
        material->uv_mode = BASIC_UV_PLANE;
        material->uv_size = stretch_texture ? (vec3) {1.0f, 1.0f, 1.0f} : (vec3) {width / world_uv, length / world_uv, 1.0f};

        material->texture = texture_id;
        material->texture_base_offset = prefab_texture_offset(raw_obj);
        material->color = prefab_krunker_color(color, 1.0f);
        material->emissive = emissive;
        material->base.transparent = link_texture || opacity != 1.0f;
        material->alpha_test = link_texture ? 0.02f : 0.0f;
        material->double_sided = 1;
        // edge strips are skipped for "ab" ramps (noAmb = ab != null; the map nAmb does not apply)
        // (and with noTex); the ambientShading setting toggles them at draw time (basic.c)
        material->ramp_shade_width = prefab_json_present(cJSON_GetObjectItem(raw_obj, "ab")) || no_textures ? 0.0f : width;
        prefab_apply_lighting_controls(material, object);

        return mesh;
    }

    if (object->prefab == PREFAB_CYLINDER || object->prefab == PREFAB_SPHERE || object->prefab == PREFAB_LIGHT_CONE) {
        const int cone = object->prefab == PREFAB_LIGHT_CONE;
        // 31.js generateCylinder: 12 sides; generateCone (light cones): 10 sides, open ended
        Geometry *geometry = object->prefab == PREFAB_SPHERE ? prefab_sphere_geo() : prefab_cylinder_geo(cone ? 10 : 12, cone);
        if (!geometry) return NULL;

        BasicMaterial *material = basic_material_init();
        Mesh *mesh = mesh_init(geometry, (Material *) material);

        mesh->visible = visible;
        mesh->transform.position = object->position;
        mesh->transform.rotation = rotation;
        mesh->transform.scale = object->scale;

        if (object->prefab != PREFAB_SPHERE) {
            // 55.js addCylinder/addCone: radius min(s[0], s[2]) / 2 (unit radius geometry)
            const float diameter = fminf(object->scale.x, object->scale.z);
            mesh->transform.scale.x = mesh->transform.scale.z = diameter;
        }

        material->texture = texture_id;
        material->color = prefab_krunker_color(color, 1.0f);
        material->color.w = opacity;
        material->emissive = cone ? none : emissive;
        material->base.transparent = cone || opacity != 1.0f || link_texture;
        material->alpha_test = link_texture ? 0.02f : 0.0f;

        if (cone) {
            // 94.js addLightCone: MeshBasicMaterial, AdditiveBlending, depthWrite false, DoubleSide
            material->unlit = 1;
            material->additive_blend = 1;
            material->no_depth_write = 1;
        }

        prefab_apply_lighting_controls(material, object);
        return mesh;
    }

    if (object->prefab == PREFAB_LADDER) {
        Geometry *geometry = create_ladder_geo(object->scale.y);
        if (!geometry) return NULL;

        BasicMaterial *material = basic_material_init();
        Mesh *mesh = mesh_init(geometry, (Material *) material);

        mesh->visible = visible;
        mesh->transform.position = object->position;
        mesh->transform.rotation.y = (float) M_PI * 0.5f * (float) (object->direction - 1);

        // 94.js addLadder: rails are generateCube without a UV scale and the rungs are
        // unscaled planes, so the texture is stretched over every face (vertex UVs as-is)
        material->texture = texture_id;
        material->color = prefab_krunker_color(color, 1.0f);
        material->emissive = emissive;
        prefab_apply_lighting_controls(material, object);

        return mesh;
    }

    // return NULL;

    BasicMaterial *debug_material = basic_material_init();
    Mesh *debug_mesh = mesh_init(create_cube_geo(), (Material *) debug_material);

    // debug_material->base.wireframe = 1;
    debug_mesh->transform.position = object->position;
    debug_mesh->transform.scale = object->scale;

    return debug_mesh;
}
