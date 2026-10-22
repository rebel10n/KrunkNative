#ifndef KN_BRAIN_V10_PAINT_H
#define KN_BRAIN_V10_PAINT_H

/*
 * services/brain/v10_paint.h - the v10.0.3 paints (store skins of type 18) and the paint of an owned copy.
 *
 * A paint is a consumable: its store index names a colour (paintColor "#rrggbb") or a shader (paintShader n).
 * An owned copy painted with one carries it in its owned entry's metadata m: {paint: "#rrggbb"} or
 * {paintShader: n} (game.js 122030-122035, 123193-123199; doc/v10/paint-copy.md). The copy ids start at
 * V10_COPY_ID_BASE (the skin_copies table, migration 0352): a copy id is never a store index.
 */

#include <stdint.h>

#include "knsvc/json.h"

#define V10_COPY_ID_BASE 1000000

/* 1 when `index` is a paint: *color is its "#rrggbb" (NULL for a shader paint), *shader its shader id (-1 for a
 * colour paint). 0 otherwise. */
int v10_paint_of(int64_t index, const char **color, int *shader);

/* the owned entry's paint metadata: {paint: color} for a colour, {paintShader: n} for a shader; NULL for neither */
cJSON *v10_paint_meta(const char *color, int shader);

#endif
