/*
 * services/brain/v10_paint.c - the v10.0.2 / v10.0.3 paints (the store's type 18 skins) and the paint metadata
 * of an owned copy (v10_paint.h). The table is the one clan_v10.c's banner colour reads.
 */
#include "v10_paint.h"

#include <stddef.h>

static const struct {
    int index;
    const char *color; /* NULL: shader */
    int shader;
} k_paints[] = {
    {7776, "#dc3c3c", -1}, {7777, "#50c864", -1}, {7778, "#0457d1", -1}, {7779, "#1a1a1a", -1}, {7780, "#f0f0f0", -1},
    {7781, "#f0d23c", -1}, {7795, NULL, 0},       {7796, NULL, 1},       {7831, "#A884E1", -1}, {7832, "#84E18C", -1},
    {7833, "#84D9E1", -1}, {7834, "#FF1FB8", -1}, {7835, "#c08552", -1}, {7836, "#ff9913", -1}, {7837, "#bf00ff", -1},
    {7904, NULL, 2},       {7905, NULL, 3},       {7906, NULL, 4},       {7907, NULL, 5},
};

int v10_paint_of(int64_t index, const char **color, int *shader) {
    for (size_t i = 0; i < sizeof k_paints / sizeof k_paints[0]; i++) {
        if (k_paints[i].index != index) continue;
        if (color) *color = k_paints[i].color;
        if (shader) *shader = k_paints[i].shader;
        return 1;
    }
    return 0;
}

cJSON *v10_paint_meta(const char *color, int shader) {
    if (color && color[0]) {
        cJSON *m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "paint", color);
        return m;
    }
    if (shader >= 0) {
        cJSON *m = cJSON_CreateObject();
        cJSON_AddNumberToObject(m, "paintShader", (double) shader);
        return m;
    }
    return NULL;
}
