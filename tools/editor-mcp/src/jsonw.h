// JSON writer with JavaScript JSON.stringify output (kn_editor_mcp).
//
// Krunker maps are written by JSON.stringify, so a map loaded with cJSON and printed back here
// is byte-identical to the file: numbers use the shortest round-trip form in JavaScript's
// Number#toString layout (1e+21, 1e-7, 0.000001, -0 -> 0), strings escape exactly like
// JSON.stringify (\b \f \n \r \t, other controls as \u00xx, raw UTF-8 otherwise) and key order
// is the document's. NaN / Infinity print as null like JSON.stringify.
#pragma once
#include <cJSON.h>
#include <stddef.h>

typedef enum {
    JSONW_COMPACT = 0, // JSON.stringify(v)
    JSONW_PRETTY = 1,  // JSON.stringify(v, null, 2), but arrays of <= 8 scalars stay on one line
    JSONW_INDENT2 = 2, // exactly JSON.stringify(v, null, 2)
} JsonwStyle;

// malloc'd text, NULL on OOM.
char *jsonw_print(const cJSON *item, JsonwStyle style);
// Appends one number in JavaScript Number#toString form to out (>= 32 bytes).
void jsonw_number(double value, char *out, size_t size);
