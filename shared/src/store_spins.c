// Krunker v10.0.2 prize wheels and item pools. See shared/include/store_spins.h.
#include <store_spins.h>
#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void copy_str(char *dst, const size_t n, const cJSON *o, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    snprintf(dst, n, "%s", cJSON_IsString(v) ? v->valuestring : "");
}

static int get_int(const cJSON *o, const char *key, const int d) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(v) ? (int) v->valuedouble : cJSON_IsBool(v) ? cJSON_IsTrue(v) : d;
}

static int truthy(const cJSON *o, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsTrue(v) || (cJSON_IsNumber(v) && v->valuedouble != 0) || (cJSON_IsString(v) && v->valuestring[0]);
}

// days since 1970-01-01 of a civil date (H. Hinnant's days_from_civil)
static int64_t days_from_civil(int64_t y, const int m, const int d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

int64_t store_iso_ms(const char *iso) {
    int y, mo, d, h = 0, mi = 0, s = 0, ms = 0;
    if (!iso || sscanf(iso, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &s) < 3) return 0;
    const char *dot = strchr(iso, '.');
    if (dot) ms = atoi(dot + 1) % 1000;
    if (mo < 1 || mo > 12 || d < 1 || d > 31) return 0;
    return ((days_from_civil(y, mo, d) * 24 + h) * 60 + mi) * 60000LL + s * 1000LL + ms;
}

int store_wheel_parse(const cJSON *o, StoreWheel *w) {
    memset(w, 0, sizeof *w);
    w->ticket = -1;
    if (!cJSON_IsObject(o)) return -1;
    copy_str(w->name, sizeof w->name, o, "name");
    copy_str(w->language, sizeof w->language, o, "language");
    copy_str(w->head_col, sizeof w->head_col, o, "headCol");
    copy_str(w->button_col, sizeof w->button_col, o, "buttonColor");
    copy_str(w->price_t, sizeof w->price_t, o, "priceT");
    copy_str(w->promo_text, sizeof w->promo_text, o, "promoText");
    copy_str(w->promo_col, sizeof w->promo_col, o, "promoCol");
    copy_str(w->lim_type, sizeof w->lim_type, o, "limType");
    copy_str(w->lim_group, sizeof w->lim_group, o, "limGroup");
    copy_str(w->tag, sizeof w->tag, o, "tag");
    w->price = get_int(o, "price", 0);
    w->multi = get_int(o, "multi", 0);
    w->icons = get_int(o, "icons", 0);
    w->primary = truthy(o, "primary");
    w->free = truthy(o, "free");
    w->kr = truthy(o, "kr");
    w->min_lvl = get_int(o, "minLvl", 0);
    w->hide = truthy(o, "hide");
    w->locked = truthy(o, "locked");
    w->ticket = get_int(o, "ticketIndex", -1);
    w->ticket_only = truthy(o, "ticketOnly");
    w->big_rar = get_int(o, "bigRar", 0);
    w->season = get_int(o, "season", 0);
    w->items_unique = truthy(o, "itemsAreUnique");
    const cJSON *it;
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(o, "duplicateProtectedPools")) {
        if (cJSON_IsNumber(it) && it->valuedouble >= 0 && it->valuedouble < STORE_WHEEL_SLICES) w->dup_protected |= 1u << (int) it->valuedouble;
    }
    const cJSON *pools = cJSON_GetObjectItemCaseSensitive(o, "pools");
    const cJSON *weights = cJSON_GetObjectItemCaseSensitive(o, cJSON_IsArray(pools) && cJSON_GetArraySize(pools) > 0 ? "poolWeights" : "rarities");
    if (cJSON_IsArray(pools) && cJSON_GetArraySize(pools) > 0) {
        cJSON_ArrayForEach(it, pools) {
            if (w->pool_count >= STORE_WHEEL_SLICES) break;
            snprintf(w->pools[w->pool_count++], sizeof w->pools[0], "%s", cJSON_IsString(it) ? it->valuestring : "");
        }
    }
    cJSON_ArrayForEach(it, weights) {
        if (w->slices >= STORE_WHEEL_SLICES) break;
        w->weights[w->slices++] = cJSON_IsNumber(it) && it->valuedouble > 0 ? it->valuedouble : 0;
    }
    if (w->pool_count && w->slices > w->pool_count) w->slices = w->pool_count;
    w->start_ms = store_iso_ms(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, "startDate")));
    w->end_ms = store_iso_ms(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, "endDate")));
    w->display_ms = store_iso_ms(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, "displayDate")));
    return 0;
}

void store_spins_free(StoreSpins *s) {
    if (!s) return;
    for (int i = 0; i < s->pool_count; i++) free(s->pools[i].items);
    free(s->pools);
    free(s->wheels);
    memset(s, 0, sizeof *s);
}

int store_spins_parse(const char *json, StoreSpins *out) {
    memset(out, 0, sizeof *out);
    cJSON *root = json ? cJSON_Parse(json) : NULL;
    const cJSON *wheels = cJSON_GetObjectItemCaseSensitive(root, "wheels");
    const cJSON *pools = cJSON_GetObjectItemCaseSensitive(root, "itemPools");
    if (!cJSON_IsArray(wheels) || (pools && !cJSON_IsObject(pools))) {
        cJSON_Delete(root);
        return -1;
    }
    out->season = get_int(root, "season", 0);
    const int nw = cJSON_GetArraySize(wheels), np = cJSON_GetArraySize(pools);
    out->wheels = calloc((size_t) (nw > 0 ? nw : 1), sizeof *out->wheels);
    out->pools = calloc((size_t) (np > 0 ? np : 1), sizeof *out->pools);
    if (!out->wheels || !out->pools) {
        store_spins_free(out);
        cJSON_Delete(root);
        return -1;
    }
    const cJSON *it;
    cJSON_ArrayForEach(it, wheels) store_wheel_parse(it, &out->wheels[out->wheel_count++]);
    cJSON_ArrayForEach(it, pools) {
        StorePool *p = &out->pools[out->pool_count++];
        snprintf(p->name, sizeof p->name, "%s", it->string ? it->string : "");
        copy_str(p->label, sizeof p->label, it, "name");
        copy_str(p->color, sizeof p->color, it, "color");
        const cJSON *items = cJSON_GetObjectItemCaseSensitive(it, "items");
        const int n = cJSON_GetArraySize(items);
        p->items = n > 0 ? malloc((size_t) n * sizeof *p->items) : NULL;
        const cJSON *v;
        cJSON_ArrayForEach(v, items) {
            if (!p->items || !cJSON_IsNumber(v) || v->valuedouble < 0) continue;
            int dup = 0; // getSpnItems collects a Set
            for (int k = 0; k < p->count && !dup; k++) dup = p->items[k] == (int) v->valuedouble;
            if (!dup) p->items[p->count++] = (int) v->valuedouble;
        }
    }
    cJSON_Delete(root);
    return 0;
}

const StoreSpins *store_spins_builtin(void) {
    static StoreSpins spins;
    static int loaded;
    if (!loaded) {
        loaded = 1;
        if (store_spins_parse(g_store_spins_json, &spins) != 0) fprintf(stderr, "store: the built-in spin data does not parse\n");
    }
    return &spins;
}

const StorePool *store_spins_pool(const StoreSpins *s, const char *name) {
    for (int i = 0; s && name && i < s->pool_count; i++)
        if (!strcmp(s->pools[i].name, name)) return &s->pools[i];
    return NULL;
}

int store_spins_find(const StoreSpins *s, const char *name) {
    for (int i = 0; s && name && *name && i < s->wheel_count; i++)
        if (!strcmp(s->wheels[i].name, name)) return i;
    return -1;
}

int store_wheel_open(const StoreWheel *w, const int64_t now_ms) {
    return w && !w->locked && (!w->start_ms || now_ms >= w->start_ms) && (!w->end_ms || now_ms < w->end_ms);
}

double store_wheel_total(const StoreWheel *w) {
    double t = 0;
    for (int i = 0; i < w->slices; i++) t += w->weights[i];
    return t;
}

int store_wheel_slice(const StoreWheel *w, double roll) {
    for (int i = 0; i < w->slices; i++) {
        if (roll < w->weights[i]) return i;
        roll -= w->weights[i];
    }
    for (int i = w->slices - 1; i >= 0; i--)
        if (w->weights[i] > 0) return i;
    return 0;
}
