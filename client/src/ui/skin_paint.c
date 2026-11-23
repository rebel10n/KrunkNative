// The paint badge of an owned skin card (client/include/skin_paint.h). The markup and the rules are
// Krunker v10.0.3's (game.js: the helper the skin and item pickers call after each card's image).
#include <skin_paint.h>
#include <i18n.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// metadata.paint, a non-empty string (the page's truthy test); NULL otherwise
static const char *paint_text(const cJSON *meta) {
    const cJSON *p = meta ? cJSON_GetObjectItemCaseSensitive(meta, "paint") : NULL;
    return cJSON_IsString(p) && p->valuestring && p->valuestring[0] ? p->valuestring : NULL;
}

// metadata.paintShader when it is set (the page's `null == x` test: a missing or null value is no shader)
static const cJSON *paint_shader(const cJSON *meta) {
    const cJSON *s = meta ? cJSON_GetObjectItemCaseSensitive(meta, "paintShader") : NULL;
    return s && !cJSON_IsNull(s) ? s : NULL;
}

// v10.0.3's store paints of the six paint shaders (their paintShader in the store table; clan_ui.c k_paints)
static const char *const k_shader_paint_names[] = {"Spectrum Paint", "Synth Paint", "Gold Paint", "Silver Paint",
                                                   "Prism Paint", "Codefall Paint"};

const char *skin_paint_shader_name(int shader) {
    if (shader < 0 || shader >= (int) (sizeof(k_shader_paint_names) / sizeof(k_shader_paint_names[0]))) return NULL;
    return k_shader_paint_names[shader];
}

int skin_paint_shows(const cJSON *meta) {
    return paint_text(meta) != NULL || paint_shader(meta) != NULL;
}

const char *skin_paint_color(const cJSON *meta) {
    return paint_text(meta);
}

int skin_paint_same_color(const char *a, const char *b) {
    return a && b && !strcasecmp(a, b);
}

// Appends s to out (at is the length so far; out stays NUL-terminated within n). Returns the new length.
static size_t put(char *out, size_t n, size_t at, const char *s) {
    while (s && *s && at + 1 < n) out[at++] = *s++;
    out[at] = '\0';
    return at;
}

// Appends s with & < > " ' escaped (the page's esc()).
static size_t put_esc(char *out, size_t n, size_t at, const char *s) {
    for (; s && *s; s++) {
        const char *rep = NULL;
        switch (*s) {
            case '&': rep = "&amp;"; break;
            case '<': rep = "&lt;"; break;
            case '>': rep = "&gt;"; break;
            case '"': rep = "&quot;"; break;
            case '\'': rep = "&#39;"; break;
            default: break;
        }
        if (rep) {
            at = put(out, n, at, rep);
        } else if (at + 1 < n) {
            out[at++] = *s;
            out[at] = '\0';
        }
    }
    return at;
}

// The swatch colour: the page lowercases the paint and keeps only "#" and hex digits.
static size_t put_swatch_colour(char *out, size_t n, size_t at, const char *colour) {
    for (const char *p = colour; *p; p++) {
        const char c = (char) (*p >= 'A' && *p <= 'Z' ? *p + ('a' - 'A') : *p);
        if (c == '#' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) {
            if (at + 1 < n) {
                out[at++] = c;
                out[at] = '\0';
            }
        }
    }
    return at;
}

// The badge; place (NULL for the stylesheet's position) is the inline style of the v10 offset / size props.
static size_t badge(char *out, size_t n, const cJSON *meta, const SkinPaint *store, const char *place) {
    if (!out || n == 0) return 0;
    out[0] = '\0';
    if (!skin_paint_shows(meta)) return 0;

    const char *colour = paint_text(meta);
    const cJSON *shader = paint_shader(meta);
    const char *name = store && store->name && store->name[0] ? store->name : NULL;

    // the page lowercases the colour: the title's fallback and the swatch use that
    char lower[64] = "";
    if (colour) {
        size_t k = 0;
        for (const char *p = colour; *p && k + 1 < sizeof(lower); p++, k++)
            lower[k] = (char) (*p >= 'A' && *p <= 'Z' ? *p + ('a' - 'A') : *p);
        lower[k] = '\0';
    }

    // the title's name: the store paint's name, else the shader's store paint (deob:124589-124594), else the
    // shader number, else the colour
    char shader_name[64];
    if (!name && shader && cJSON_IsNumber(shader)) name = skin_paint_shader_name((int) shader->valuedouble);
    if (!name && shader) {
        if (cJSON_IsNumber(shader)) snprintf(shader_name, sizeof(shader_name), "Shader #%.15g", shader->valuedouble);
        else snprintf(shader_name, sizeof(shader_name), "Shader #%s", cJSON_IsString(shader) && shader->valuestring ? shader->valuestring : "");
        name = shader_name;
    }
    if (!name) name = lower;

    char title[192];
    I18N_F(title, "windows.skins.painted", "Painted: {0}", name);

    size_t at = 0;
    at = put(out, n, at, "<div class='skinPaintBadge' title='");
    at = put_esc(out, n, at, title);
    if (place) {
        at = put(out, n, at, "' style='");
        at = put(out, n, at, place);
    }
    at = put(out, n, at, "'>");
    const char *thumb = store ? store->thumb : NULL;
    if (thumb && thumb[0]) {
        at = put(out, n, at, "<img draggable='false' src='");
        at = put_esc(out, n, at, thumb);
        at = put(out, n, at, "' />");
    } else if (colour) {
        at = put(out, n, at, "<span class='skinPaintSwatch' style='background-color:");
        at = put_swatch_colour(out, n, at, lower);
        at = put(out, n, at, "'></span>");
    } else {
        at = put(out, n, at, "<i class='material-icons'>format_paint</i>");
    }
    return put(out, n, at, "</div>");
}

size_t skin_paint_badge(char *out, size_t n, const cJSON *meta, const SkinPaint *store) {
    return badge(out, n, meta, store, NULL);
}

size_t skin_paint_badge_at(char *out, size_t n, const cJSON *meta, const SkinPaint *store, const int offset, const int size) {
    // v10 sets top and left to the offset and --badge-size to the size (deob:124661-124663); the size is
    // written as the width and height here (the stylesheet's .skinPaintBadge sizes the inner parts in %)
    char place[96];
    snprintf(place, sizeof(place), "top:%dpx;left:%dpx;width:%dpx;height:%dpx", offset, offset, size, size);
    return badge(out, n, meta, store, place);
}

// ---------------------------------------------------------------- v10.0.7 copies: wear tiers, slot values

static const struct {
    double max;
    const char *key, *color;
} k_wear_tiers[] = {
    {0.1, "wear_flawless", "#41c300"}, {0.3, "wear_tested", "#a8d600"}, {0.6, "wear_trashed", "#faa332"}, {1.0, "wear_wrecked", "#f44336"},
};
#define WEAR_TIER_COUNT ((int) (sizeof(k_wear_tiers) / sizeof(k_wear_tiers[0])))

static int wear_tier(const double wear) {
    for (int i = 0; i < WEAR_TIER_COUNT; i++)
        if (wear < k_wear_tiers[i].max) return i;
    return WEAR_TIER_COUNT - 1;
}

const char *skin_wear_tier_key(const double wear) { return k_wear_tiers[wear_tier(wear)].key; }
const char *skin_wear_tier_color(const double wear) { return k_wear_tiers[wear_tier(wear)].color; }

int skin_slot_parse(const char *value, const int fallback, int *copy_id) {
    if (copy_id) *copy_id = 0;
    if (!value) return fallback;
    const char *hash = strchr(value, '#');
    if (hash && hash != value) {
        char *end_id, *end_ind;
        const long id = strtol(value, &end_id, 10);
        const long ind = strtol(hash + 1, &end_ind, 10);
        if (end_id == value || end_ind == hash + 1) return fallback; // parseInt: NaN
        if (copy_id && id > 0 && id <= 0x7fffffffL) *copy_id = (int) id;
        return (int) ind;
    }
    char *end;
    const long v = strtol(value, &end, 10);
    return end != value ? (int) v : fallback;
}
