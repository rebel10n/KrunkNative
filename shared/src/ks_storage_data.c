// GAME.STORAGE data rules (shared/include/ks_storage_data.h).
#include <ks_storage_data.h>
#include "ks/ks_rt.h" // ks_num_format, ks_parse_float (shared/src/ks/ks_number.c)

#include <math.h>
#include <stdio.h>
#include <string.h>

void ks_storage_data_init(KsStorageData *d) {
    memset(d, 0, sizeof(*d));
}

size_t ks_storage_u16len(const char *utf8) {
    size_t units = 0;
    for (const unsigned char *p = (const unsigned char *) utf8; p && *p; p++)
        if ((*p & 0xC0) != 0x80) units += *p >= 0xF0 ? 2 : 1;
    return units;
}

// (x + "").replace(/[,'"]/g, "") into out; 0 when it is longer than max_units UTF-16 units
static int strip(const char *in, char *out, const size_t cap, const size_t max_units) {
    size_t o = 0, units = 0;
    for (const unsigned char *p = (const unsigned char *) (in ? in : ""); *p; p++) {
        if (*p == ',' || *p == '\'' || *p == '"') continue;
        if ((*p & 0xC0) != 0x80) units += *p >= 0xF0 ? 2 : 1;
        if (units > max_units || o + 1 >= cap) return 0;
        out[o++] = (char) *p;
    }
    out[o] = 0;
    return 1;
}

void ks_storage_data_add(KsStorageData *d, const char *key, const char *value) {
    if (!d || d->error) return; // storageUpdt returns at the first bad key / value
    d->keys++;
    KsStoragePair pair;
    if (!strip(key, pair.key, sizeof(pair.key), KS_STORAGE_KEY_MAX)) {
        d->error = "Key too long";
        return;
    }
    if (!strip(value, pair.value, sizeof(pair.value), KS_STORAGE_VALUE_MAX)) {
        d->error = "Value too long";
        return;
    }
    if (d->count < KS_STORAGE_MAX_KEYS) d->pairs[d->count++] = pair;
}

const char *ks_storage_data_error(const KsStorageData *d) {
    if (!d) return "Missing storage data";
    if (d->error) return d->error;
    if (d->keys > KS_STORAGE_MAX_KEYS) return "Too many keys: max 30";
    return NULL;
}

void ks_storage_format_number(const double x, char *out) {
    ks_num_format(x, out);
}

double ks_storage_parse_number(const char *text) {
    return text ? ks_parse_float(text, strlen(text)) : NAN;
}

int ks_storage_apply(const int type, const char *old, const char *value, char *out, const size_t n) {
    if (!out || !n) return 0;
    out[0] = 0;
    const double v = ks_storage_parse_number(value), o = ks_storage_parse_number(old);
    switch (type) {
        case KS_STORAGE_SET:
            snprintf(out, n, "%s", value ? value : "");
            return 1;
        case KS_STORAGE_SET_HIGHER:
        case KS_STORAGE_SET_LOWER:
            if (isnan(v)) return 0;
            if (old && !isnan(o) && (type == KS_STORAGE_SET_HIGHER ? !(v > o) : !(v < o))) return 0;
            snprintf(out, n, "%s", value);
            return 1;
        case KS_STORAGE_INCREASE:
        case KS_STORAGE_DECREASE: {
            if (isnan(v)) return 0;
            const double base = old && !isnan(o) ? o : 0.0;
            char text[40];
            ks_storage_format_number(type == KS_STORAGE_INCREASE ? base + v : base - v, text);
            snprintf(out, n, "%s", text);
            return 1;
        }
        case KS_STORAGE_DELETE:
            return old ? -1 : 0;
        default:
            return 0;
    }
}

const char *ks_storage_script_key(const char *stored) {
    if (!stored) return "";
    const size_t n = strlen(KS_STORAGE_SCRIPT_PREFIX);
    return !strncmp(stored, KS_STORAGE_SCRIPT_PREFIX, n) ? stored + n : stored;
}
