// The Create Trade window's inventory rows (market_trade_list.h): filtered in one pass and sorted as v10 sorts
// them. The window keeps every match and draws the rows near its scroll position (a virtual list, ui_vgrid.h),
// so a search over a big inventory costs one pass plus a screenful of image cards (the markup and the images
// are what is expensive). The caller's limit caps the rows written to `out`.
#include <market_trade_list.h>
#include "../viewer/viewer_store.h" // ViewerSkin, VS2_PAINTABLE

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    TradeListRow row;
    int rarity, season; // the sort keys, read once (the comparator takes no lookup)
} KeptRow;

// jint(o, k, def) of v10_market.c: a number, or a non-empty string read as one
static int row_int(const cJSON *o, const char *k, const int def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsNumber(v)) return v->valueint;
    if (cJSON_IsString(v) && v->valuestring[0]) return atoi(v->valuestring);
    return def;
}

static const char *(*g_weapon_name)(int weapon_index);
static const cJSON *(*g_skin_json)(int skin_index);
static int g_hide_bound, g_dev;
static int (*g_copy_locked)(int sid);

void market_trade_list_set_weapon_name(const char *(*name)(int weapon_index)) { g_weapon_name = name; }
void market_trade_list_set_skin_json(const cJSON *(*json)(int skin_index)) { g_skin_json = json; }
void market_trade_list_set_hide_bound(const int on, const int is_dev) {
    g_hide_bound = on;
    g_dev = is_dev;
}
void market_trade_list_set_copy_locked(int (*locked)(int sid)) { g_copy_locked = locked; }

// iíiiiìi (deob-10.0.7.js:24045): either string contains the other, ignoring case
static int either_contains(const char *a, const char *b) {
    if (!a || !a[0] || !b || !b[0]) return 0;
    return market_text_contains(a, b) || market_text_contains(b, a);
}

static const char *json_str(const cJSON *o, const char *key) {
    const cJSON *v = o ? cJSON_GetObjectItemCaseSensitive(o, key) : NULL;
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static int json_truthy(const cJSON *o, const char *key) {
    const cJSON *v = o ? cJSON_GetObjectItemCaseSensitive(o, key) : NULL;
    if (!v || cJSON_IsNull(v) || cJSON_IsFalse(v)) return 0;
    if (cJSON_IsNumber(v)) return v->valuedouble != 0;
    if (cJSON_IsString(v)) return v->valuestring[0] != 0;
    return 1;
}

// iÏîïìïi (deob-10.0.7.js:111381): free / noSale / blackM / opensea / itemShop 1. A developer keeps the non-free ones.
static int bound_untradable(const cJSON *j) {
    if (!j) return 0;
    const int free = json_truthy(j, "free");
    int bound = free || json_truthy(j, "noSale") || json_truthy(j, "blackM") || json_truthy(j, "opensea");
    const cJSON *shop = cJSON_GetObjectItemCaseSensitive(j, "itemShop");
    if (cJSON_IsNumber(shop) && shop->valuedouble == 1) bound = 1;
    if (!bound) return 0;
    return free || !g_dev;
}

// the query against the item (deob-10.0.7.js:111394): its name, keyW, limT, weapon name, or "drops" on a drop
static int search_matches(const ViewerSkin *s, const int index, const char *search) {
    if (either_contains(s->name, search)) return 1;
    const cJSON *j = g_skin_json ? g_skin_json(index) : NULL;
    const char *key_w = json_str(j, "keyW"), *lim = json_str(j, "limT");
    if (key_w && key_w[0] && either_contains(key_w, search)) return 1;
    if (lim && lim[0] && either_contains(lim, search)) return 1;
    const char *weapon = s->weapon > 0 && g_weapon_name ? g_weapon_name(s->weapon - 1) : NULL;
    if (weapon && weapon[0] && either_contains(weapon, search)) return 1;
    return json_truthy(j, "drop") && either_contains("drops", search);
}

static int cmp_int(const void *pa, const void *pb) {
    const int a = *(const int *) pa, b = *(const int *) pb;
    return (a > b) - (a < b);
}

// rarity desc, season desc, index desc (v10's _9415 order of the trade inventory)
static int cmp_kept(const void *pa, const void *pb) {
    const KeptRow *a = pa, *b = pb;
    if (a->rarity != b->rarity) return b->rarity - a->rarity;
    if (a->season != b->season) return b->season - a->season;
    if (a->row.skin != b->row.skin) return b->row.skin - a->row.skin;
    return a->row.sid - b->row.sid; // one skin's rows: its stack (sid 0) first, then its copies by copy id
}

int market_paintable_match(const struct ViewerSkin *s, int only_paintable) {
    return !only_paintable || (s && (s->flags2 & VS2_PAINTABLE));
}

int market_text_contains(const char *hay, const char *needle) {
    if (!needle || !needle[0]) return 1;
    if (!hay) return 0;
    const size_t n = strlen(needle);
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < n && p[i] && tolower((unsigned char) p[i]) == tolower((unsigned char) needle[i])) i++;
        if (i == n) return 1;
    }
    return 0;
}

int market_trade_list_select(const struct ViewerSkin *(*skin)(int index), const cJSON *inv, const cJSON *other,
                             const char *search, int missing, int limit, TradeListRow *out, int *total) {
    *total = 0;
    if (!skin || !inv || !out || limit <= 0) return 0;

    // the other side's skin indices, sorted: "does the other player own it" is a bsearch per row
    int nowned = 0;
    int *owned = NULL;
    if (missing && other) {
        owned = calloc((size_t) (cJSON_GetArraySize(other) + 1), sizeof(*owned));
        const cJSON *o;
        cJSON_ArrayForEach(o, other) if (owned) owned[nowned++] = row_int(o, "i", -1);
        if (owned) qsort(owned, (size_t) nowned, sizeof(*owned), cmp_int);
    }

    const int n = cJSON_GetArraySize(inv);
    KeptRow *kept = calloc((size_t) (n + 1), sizeof(*kept));
    int nkept = 0;
    const cJSON *r;
    if (kept) cJSON_ArrayForEach(r, inv) {
        const int i = row_int(r, "i", -1), count = row_int(r, "c", 1);
        const ViewerSkin *s = i >= 0 ? skin(i) : NULL;
        if (!s || count <= 0) continue;
        const int sid = row_int(r, "sid", 0);
        if (sid > 0 && g_copy_locked && g_copy_locked(sid)) continue; // your locked copy is not offered
        if (g_hide_bound && bound_untradable(g_skin_json ? g_skin_json(i) : NULL)) continue;
        if (missing && owned && bsearch(&i, owned, (size_t) nowned, sizeof(*owned), cmp_int)) continue;
        if (search && search[0] && !search_matches(s, i, search)) continue;
        // a painted copy row carries its copy id as sid (a stack none); the row goes with it for its paint
        kept[nkept++] = (KeptRow) {{i, count, row_int(r, "nt", 0), row_int(r, "sid", 0), r}, s->rarity, s->seas ? s->seas : 1};
    }
    free(owned);

    *total = nkept;
    int written = 0;
    if (kept && nkept) {
        qsort(kept, (size_t) nkept, sizeof(*kept), cmp_kept);
        written = nkept < limit ? nkept : limit;
        for (int j = 0; j < written; j++) out[j] = kept[j].row;
    }
    free(kept);
    return written;
}
