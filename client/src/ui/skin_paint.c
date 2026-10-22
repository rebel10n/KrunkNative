// The paint badge of an owned skin card (client/include/skin_paint.h). The markup and the rules are
// Krunker v10.0.3's (game.js: the helper the skin and item pickers call after each card's image).
#include <skin_paint.h>
#include <i18n.h>

#include <stdio.h>
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

size_t skin_paint_badge(char *out, size_t n, const cJSON *meta, const SkinPaint *store) {
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

    // the title's name: the store paint's name, else the shader number, else the colour
    char shader_name[64];
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
