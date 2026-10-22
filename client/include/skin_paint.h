#ifndef KN_SKIN_PAINT_H
#define KN_SKIN_PAINT_H

#include <stddef.h>

#include <cJSON.h>

// The paint badge on an owned skin card (Krunker v10.0.3's skinPaintBadge; client/src/ui/skin_paint.c).
// An owned entry's metadata (acc.skins[i].m: paint, a "#rrggbb" colour, and paintShader, a number)
// paints the item; the badge says so. Rules, from game.js's helper that the skin lists call:
//   - no badge unless the metadata has a paint colour or a paint shader;
//   - the title is "Painted: <name>", the store paint's name (the store's paint of that colour);
//     with no store name, "Painted: Shader #<n>" for a shader and "Painted: <colour>" for a colour;
//   - the body is the store paint's thumbnail when it has one, else a swatch of the colour, else
//     the format_paint icon.

// The store's paint an item's metadata names (the caller looks it up: type 18, same colour).
typedef struct {
    const char *name;  // the store paint's name ("Red Paint"), NULL when none
    const char *thumb; // its thumbnail URL, NULL when none
} SkinPaint;

// 1 when the metadata paints the item (a paint colour or a paint shader); meta may be NULL.
int skin_paint_shows(const cJSON *meta);

// The metadata's paint colour ("#dc3c3c"), NULL when the item has none (a shader paint only).
const char *skin_paint_color(const cJSON *meta);

// 1 when two paint colours name the same colour (case-insensitive, as the page compares them).
int skin_paint_same_color(const char *a, const char *b);

// The badge's markup into out (NUL-terminated, n bytes); "" when skin_paint_shows is 0. store may be
// NULL (no store paint). Returns the length written.
size_t skin_paint_badge(char *out, size_t n, const cJSON *meta, const SkinPaint *store);

#endif
