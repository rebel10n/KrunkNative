// The Create Trade window's inventory rows (market_trade_list.h): filtered in one pass, sorted as v10 sorts
// them, and capped by the caller. The window draws only the first page of them, so a search over a big
// inventory costs the same as one over a small one (the markup and the image cards are what is expensive).
#include <market_trade_list.h>
#include "../viewer/viewer_store.h" // ViewerSkin

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

static int cmp_int(const void *pa, const void *pb) {
    const int a = *(const int *) pa, b = *(const int *) pb;
    return (a > b) - (a < b);
}

// rarity desc, season desc, index desc (v10's _9415 order of the trade inventory)
static int cmp_kept(const void *pa, const void *pb) {
    const KeptRow *a = pa, *b = pb;
    if (a->rarity != b->rarity) return b->rarity - a->rarity;
    if (a->season != b->season) return b->season - a->season;
    return b->row.skin - a->row.skin;
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
        if (missing && owned && bsearch(&i, owned, (size_t) nowned, sizeof(*owned), cmp_int)) continue;
        if (search && search[0] && !market_text_contains(s->name, search) && !(s->name && market_text_contains(search, s->name))) continue;
        kept[nkept++] = (KeptRow) {{i, count, row_int(r, "nt", 0)}, s->rarity, s->seas ? s->seas : 1};
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
