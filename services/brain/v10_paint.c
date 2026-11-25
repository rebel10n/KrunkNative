/*
 * services/brain/v10_paint.c - the v10.0.2 / v10.0.3 paints (the store's type 18 skins) and the paint metadata
 * of an owned copy (v10_paint.h). The table is the one clan_v10.c's banner colour reads.
 */
#include "v10_paint.h"

#include <stddef.h>

#include "db_procs.h"

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

/* A row's stats column (market_listings.stats, 0407; a copy's skin_stats rows as one object) as an object of the counts
 * of COPY_STATS at 1 or more, NULL for none: a stack's row has no stats, and an empty object ({}: sqlite's
 * json_group_object over no rows) does not make the row a copy. */
static cJSON *row_stats(const cJSON *row) {
    static const char *const names[COPY_STAT_COUNT] = COPY_STATS;
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(row, "stats");
    cJSON *parsed = cJSON_IsString(v) && v->valuestring[0] ? cJSON_Parse(v->valuestring) : NULL;
    const cJSON *o = parsed ? parsed : v;
    cJSON *out = NULL;
    for (int i = 0; cJSON_IsObject(o) && i < COPY_STAT_COUNT; i++) {
        const cJSON *n = cJSON_GetObjectItemCaseSensitive(o, names[i]);
        if (!cJSON_IsNumber(n) || n->valuedouble < 1) continue;
        if (!out && !(out = cJSON_CreateObject())) break;
        cJSON_AddNumberToObject(out, names[i], (double) (int64_t) n->valuedouble);
    }
    cJSON_Delete(parsed);
    return out;
}

static int row_has_stats(const cJSON *row) {
    cJSON *stats = row_stats(row);
    cJSON_Delete(stats);
    return stats != NULL;
}

cJSON *v10_copy_meta_row(const cJSON *row) {
    cJSON *m = v10_paint_meta(kn_json_str(row, "paint_color", ""), (int) kn_json_int(row, "paint_shader", -1));
    const cJSON *wear = cJSON_GetObjectItemCaseSensitive(row, "wear"), *serial = cJSON_GetObjectItemCaseSensitive(row, "serial");
    const int has_wear = cJSON_IsNumber(wear), has_serial = cJSON_IsNumber(serial) && serial->valuedouble > 0;
    const char *name = kn_json_str(row, "nametag", "");
    cJSON *stats = row_stats(row);
    if (!has_wear && !has_serial && !name[0] && !stats) return m;
    if (!m) m = cJSON_CreateObject();
    if (!m) {
        cJSON_Delete(stats);
        return NULL;
    }
    if (has_wear) cJSON_AddNumberToObject(m, "wear", wear->valuedouble);
    if (has_serial) cJSON_AddNumberToObject(m, "serial", serial->valuedouble);
    if (name[0]) cJSON_AddStringToObject(m, "name", name); /* m.name, the copy's name tag (0401) */
    /* m[stat]: v10's listing, sale and inventory cards read the copy's m (deob-10.0.7.js:131335), and the item view
     * draws each counter stat it holds (:106682, :106774), so a browse row without them showed no count */
    for (const cJSON *n = stats ? stats->child : NULL; n; n = n->next) cJSON_AddNumberToObject(m, n->string, n->valuedouble);
    cJSON_Delete(stats);
    return m;
}

int v10_copy_row(const cJSON *row) {
    const cJSON *serial = cJSON_GetObjectItemCaseSensitive(row, "serial");
    return kn_json_str(row, "paint_color", "")[0] != '\0' || kn_json_int(row, "paint_shader", -1) >= 0 ||
           cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(row, "wear")) || (cJSON_IsNumber(serial) && serial->valuedouble > 0) ||
           kn_json_str(row, "nametag", "")[0] != '\0' || row_has_stats(row);
}

void v10_copy_row_strip(cJSON *row) {
    cJSON_DeleteItemFromObjectCaseSensitive(row, "paint_color");
    cJSON_DeleteItemFromObjectCaseSensitive(row, "paint_shader");
    cJSON_DeleteItemFromObjectCaseSensitive(row, "wear");
    cJSON_DeleteItemFromObjectCaseSensitive(row, "serial");
    cJSON_DeleteItemFromObjectCaseSensitive(row, "nametag");
    cJSON_DeleteItemFromObjectCaseSensitive(row, "stats");
}
