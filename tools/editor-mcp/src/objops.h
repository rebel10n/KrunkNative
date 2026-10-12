// Object helpers shared by the tool modules (tools_objects.c).
#pragma once
#include "ctx.h"
#include <cJSON.h>

// {prefab, p, s, r | rotation_deg, props, raw} -> a new map object (not added), NULL + err.
cJSON *objops_build(Doc *doc, const cJSON *spec, int flags, double snap, char *err, size_t errsz);
int objops_apply_props(Doc *doc, cJSON *obj, const cJSON *props, int flags, char *err, size_t errsz);
// A standalone copy: pooled size and palette colours resolved (for any document).
cJSON *objops_portable_copy(const Doc *src, const cJSON *obj);
// Folds the copy's c / e colours into dst's palette when dst has one.
void objops_adopt(Doc *dst, cJSON *obj);
// Collision bounds of the objects (ids); returns how many were found.
int objops_selection_bounds(Doc *doc, const int *ids, int n, double mn[3], double mx[3]);
int objops_check_ids(Doc *doc, const int *ids, int n, char *err, size_t errsz);
int objops_flags(const cJSON *args); // force / clamp -> SET_*
cJSON *objops_ids_json(const Edit *e, const int *ids, int n); // final ids after commit
