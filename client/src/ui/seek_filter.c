// The seek filter's pure core (client/include/seek_filter.h).
#include <seek_filter.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

int seek_filter_parse(const cJSON *v, int *out, const int max, int (*valid)(int)) {
    double vals[SEEK_FILTER_MAX];
    int count = 0, n = 0;
    const char *s = cJSON_GetStringValue(v);
    if (s) { // "a,b".split(',').map(Number): an empty or non-numeric entry is NaN, filtered out
        for (const char *p = s; *p && count < SEEK_FILTER_MAX;) {
            char *end = NULL;
            const double d = strtod(p, &end);
            if (end != p && (*end == ',' || !*end)) vals[count++] = d;
            p = strchr(p, ',');
            if (!p) break;
            p++;
        }
    } else if (cJSON_IsArray(v)) {
        const cJSON *e;
        cJSON_ArrayForEach(e, v) {
            if (count >= SEEK_FILTER_MAX) break;
            if (cJSON_IsNumber(e)) vals[count++] = e->valuedouble;
            else if (cJSON_GetStringValue(e) && e->valuestring[0]) {
                char *end = NULL;
                const double d = strtod(e->valuestring, &end);
                if (end != e->valuestring && !*end) vals[count++] = d;
            }
        }
    } else if (cJSON_IsNumber(v)) {
        vals[count++] = v->valuedouble;
    }
    for (int i = 0; i < count && n < max; i++) {
        if (vals[i] < 0 || vals[i] > 1e6) continue;
        const int id = (int) vals[i];
        if ((double) id != vals[i] || (valid && !valid(id))) continue;
        int dup = 0;
        for (int k = 0; k < n; k++) dup |= out[k] == id;
        if (!dup) out[n++] = id;
    }
    return n;
}

int seek_filter_flag(const cJSON *v) { return cJSON_IsTrue(v) || (cJSON_IsNumber(v) && v->valuedouble != 0); }

static int list_has(const int *list, const int count, const int v) {
    for (int i = 0; i < count; i++) {
        if (list[i] == v) return 1;
    }
    return 0;
}

// v10.0.2 rotationMaps (v10.js:167210): the maps of the public rotation, as official mapIndexes
// (krunker_maps.h). It adds 32-45 to v7's rotationMaps (g_rotation_maps, the game's own rotation).
static const int k_rotation_maps[] = {0, 1, 2, 3, 4, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 19, 21, 22, 23, 24, 25, 26, 28,
                                      32, 33, 34, 36, 37, 39, 40, 42, 44, 45};

size_t seek_filter_rotation_maps(const int **out) {
    if (out) *out = k_rotation_maps;
    return sizeof(k_rotation_maps) / sizeof(k_rotation_maps[0]);
}

int seek_filter_effective(const int *on, const int on_count, const int *defaults, const int default_count, int *out, const int max) {
    if (!out || max <= 0) return 0;
    const int *src = on_count > 0 ? on : defaults;
    const int count = on_count > 0 ? on_count : default_count;
    int n = 0;
    for (int i = 0; i < count && n < max; i++) {
        if (!list_has(out, n, src[i])) out[n++] = src[i];
    }
    return n;
}

int seek_filter_toggle(const int *on, const int on_count, const int *defaults, const int default_count, const int id, const int checked, int *out,
                       const int max) {
    int tmp[SEEK_FILTER_MAX];
    int n = seek_filter_effective(on, on_count, defaults, default_count, tmp, SEEK_FILTER_MAX);
    if (checked && !list_has(tmp, n, id) && n < SEEK_FILTER_MAX) tmp[n++] = id;
    if (!checked) {
        int k = 0;
        for (int i = 0; i < n; i++) {
            if (tmp[i] != id) tmp[k++] = tmp[i];
        }
        n = k;
    }
    for (int i = 1; i < n; i++) { // ascending, as the switches read
        for (int j = i; j > 0 && tmp[j - 1] > tmp[j]; j--) {
            const int t = tmp[j - 1];
            tmp[j - 1] = tmp[j];
            tmp[j] = t;
        }
    }
    int m = n < max ? n : max;
    for (int i = 0; i < m; i++) out[i] = tmp[i];
    return m;
}

int seek_filter_mode_ok(const SeekFilter *f, const int mode) {
    return f && list_has(f->modes, f->mode_count, mode);
}

int seek_filter_lobby_ok(const SeekFilter *f, const int mode, const char *map_name, const unsigned flags) {
    if (!f || (flags & (SEEK_LOBBY_CUSTOM | SEEK_LOBBY_PASSWORD))) return 0;
    // an official custom or dedicated lobby is hosted outside Krunker's public pool: only a mode the
    // player switched on (the rotation alone does not seek them)
    if ((flags & SEEK_LOBBY_HOSTED) && !(f->modes_chosen && list_has(f->modes, f->mode_count, mode))) return 0;
    if (!seek_filter_mode_ok(f, mode)) return 0;
    if (!f->map_count) return 1;
    for (int i = 0; i < f->map_count; i++) {
        if (map_name && f->map_names[i] && !strcasecmp(map_name, f->map_names[i])) return 1;
    }
    return 0;
}

static int append_list(char *out, const size_t n, int len, const char *key, const int *list, const int count) {
    if (!count || len < 0 || (size_t) len >= n) return len;
    len += snprintf(out + len, n - (size_t) len, ",\"%s\":[", key);
    for (int i = 0; i < count && (size_t) len < n; i++) len += snprintf(out + len, n - (size_t) len, "%s%d", i ? "," : "", list[i]);
    if ((size_t) len < n) len += snprintf(out + len, n - (size_t) len, "]");
    return len;
}

void seek_filter_dq_fields(const SeekFilter *f, char *out, const size_t n) {
    if (!out || !n) return;
    out[0] = 0;
    if (!f) return;
    // v10: dsma, then dsmo (v10.js:222051-222055)
    const int len = append_list(out, n, 0, "dsma", f->maps, f->map_count);
    append_list(out, n, len, "dsmo", f->modes, f->mode_count);
}
