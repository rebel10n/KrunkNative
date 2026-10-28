// Item locks and select-mode picks for the bulk market actions (client/include/market_bulk.h). Pure state and
// request bodies: no DOM, no network. Ground truth: deob-10.0.4.js (lines cited in the header).
#include <market_bulk.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LIST_START 16

// one more id in a list of ids (no duplicates); *cap is the list's allocated size
static void list_push(int **list, int *n, int *cap, const int v) {
    for (int i = 0; i < *n; i++)
        if ((*list)[i] == v) return;
    if (*n == *cap) {
        const int next = *cap ? *cap * 2 : LIST_START;
        int *grown = realloc(*list, sizeof(int) * (size_t) next);
        if (!grown) return;
        *list = grown;
        *cap = next;
    }
    (*list)[(*n)++] = v;
}

static int list_has(const int *list, const int n, const int v) {
    for (int i = 0; i < n; i++)
        if (list[i] == v) return 1;
    return 0;
}

void market_locks_apply(MarketLocks *locks, const cJSON *rows) {
    market_locks_free(locks);
    int skin_cap = 0, item_cap = 0;
    if (!cJSON_IsArray(rows)) return;
    for (const cJSON *row = rows->child; row; row = row->next) {
        const cJSON *skin = cJSON_GetObjectItemCaseSensitive(row, "skin_id");
        const cJSON *item = cJSON_GetObjectItemCaseSensitive(row, "item_id");
        const cJSON *stack = cJSON_GetObjectItemCaseSensitive(row, "stack");
        // the bundle adds every row's skin_id to ids (deob-10.0.4.js:108851), and a stack's item_id to stacks
        if (cJSON_IsNumber(skin)) list_push(&locks->skin_ids, &locks->skin_n, &skin_cap, skin->valueint);
        if (cJSON_IsTrue(stack) && cJSON_IsNumber(item)) list_push(&locks->item_ids, &locks->item_n, &item_cap, item->valueint);
    }
}

int market_locks_has(const MarketLocks *locks, const int skin_id, const int item_id, const int stackable) {
    if (list_has(locks->skin_ids, locks->skin_n, skin_id)) return 1;
    return stackable && list_has(locks->item_ids, locks->item_n, item_id);
}

void market_locks_free(MarketLocks *locks) {
    free(locks->skin_ids);
    free(locks->item_ids);
    memset(locks, 0, sizeof(*locks));
}

cJSON *market_lock_body(const int skin_id, const int item_id, const int locked) {
    cJSON *body = cJSON_CreateObject();
    if (!body) return NULL;
    // deob-10.0.4.js:108862-108866: skin_id when the row has one, else item_id; then locked
    if (skin_id) cJSON_AddNumberToObject(body, "skin_id", skin_id);
    else cJSON_AddNumberToObject(body, "item_id", item_id);
    cJSON_AddBoolToObject(body, "locked", locked ? 1 : 0);
    return body;
}

// both pick arrays grow together: one shared capacity, so each stays as long as the other
static int picks_reserve(MarketPicks *picks) {
    if (picks->n < picks->cap) return 1;
    const int next = picks->cap ? picks->cap * 2 : LIST_START;
    int *skins = realloc(picks->skin_ids, sizeof(int) * (size_t) next);
    if (!skins) return 0;
    picks->skin_ids = skins;
    int *items = realloc(picks->item_ids, sizeof(int) * (size_t) next);
    if (!items) return 0;
    picks->item_ids = items;
    picks->cap = next;
    return 1;
}

int market_picks_toggle(MarketPicks *picks, const MarketLocks *locks, const int skin_id, const int item_id,
                        const int stackable) {
    for (int i = 0; i < picks->n; i++) {
        if (picks->skin_ids[i] == skin_id && picks->item_ids[i] == item_id) {
            const size_t rest = sizeof(int) * (size_t) (picks->n - i - 1);
            memmove(picks->skin_ids + i, picks->skin_ids + i + 1, rest);
            memmove(picks->item_ids + i, picks->item_ids + i + 1, rest);
            picks->n--;
            return 0;
        }
    }
    if (market_locks_has(locks, skin_id, item_id, stackable)) return 0; // a locked row is never picked
    if (!picks_reserve(picks)) return 0;
    picks->skin_ids[picks->n] = skin_id;
    picks->item_ids[picks->n] = item_id;
    picks->n++;
    return 1;
}

void market_picks_free(MarketPicks *picks) {
    free(picks->skin_ids);
    free(picks->item_ids);
    memset(picks, 0, sizeof(*picks));
}

cJSON *market_quick_sell_body(const int action, const MarketPicks *picks, const int from, const int count) {
    cJSON *body = cJSON_CreateObject();
    cJSON *items = cJSON_CreateArray();
    if (!body || !items) {
        cJSON_Delete(body);
        cJSON_Delete(items);
        return NULL;
    }
    cJSON_AddNumberToObject(body, "action", action);
    cJSON_AddItemToObject(body, "items", items);
    const int start = from < 0 ? 0 : from;
    int end = start + (count < MARKET_BULK_QUICK_SELL_CHUNK ? count : MARKET_BULK_QUICK_SELL_CHUNK);
    if (end > picks->n) end = picks->n;
    for (int i = start; i < end; i++) {
        cJSON *row = cJSON_CreateObject();
        if (!row) break;
        cJSON_AddNumberToObject(row, "skin_id", picks->skin_ids[i]);
        cJSON_AddNumberToObject(row, "item_id", picks->item_ids[i]);
        cJSON_AddItemToArray(items, row);
    }
    return body;
}

int market_list_fit(const MarketPicks *picks, const int *quantity, const int from) {
    int total = 0, n = 0;
    for (int i = from < 0 ? 0 : from; i < picks->n; i++) {
        if (total + quantity[i] > MARKET_BULK_LIST_CHUNK) break;
        total += quantity[i];
        n++;
    }
    return n;
}

cJSON *market_list_body(const MarketPicks *picks, const int *quantity, const int price, const int from, const int count) {
    cJSON *body = cJSON_CreateObject();
    cJSON *items = cJSON_CreateArray();
    if (!body || !items) {
        cJSON_Delete(body);
        cJSON_Delete(items);
        return NULL;
    }
    cJSON_AddItemToObject(body, "items", items);
    const int start = from < 0 ? 0 : from;
    int end = start + count;
    if (end > picks->n) end = picks->n;
    for (int i = start; i < end; i++) {
        cJSON *row = cJSON_CreateObject();
        if (!row) break;
        cJSON_AddNumberToObject(row, "item_id", picks->item_ids[i]);
        cJSON_AddNumberToObject(row, "skin_id", picks->skin_ids[i]);
        cJSON_AddNumberToObject(row, "price", price);
        cJSON_AddNumberToObject(row, "quantity", quantity[i]);
        cJSON_AddItemToArray(items, row);
    }
    return body;
}

cJSON *market_buy_multiple_body(const int item_id, const int quantity, const double max_total) {
    cJSON *body = cJSON_CreateObject();
    if (!body) return NULL;
    cJSON_AddNumberToObject(body, "item_id", item_id);
    cJSON_AddNumberToObject(body, "quantity", quantity);
    cJSON_AddNumberToObject(body, "max_total", max_total);
    return body;
}

const char *market_error_key(const char *code) {
    if (!code) return NULL;
    if (!strcmp(code, "MARKET_BUSY")) return "errors.exchange.busy"; // deob-10.0.4.js:104255
    return NULL;
}

// one group per item index: a stack, or the copies of an item (deob-10.0.4.js:132199-132209)
static MarketGroup *group_for(MarketGroup **groups, int *n, const int item) {
    for (int i = 0; i < *n; i++)
        if ((*groups)[i].item == item) return &(*groups)[i];
    MarketGroup *grown = realloc(*groups, sizeof(**groups) * (size_t) (*n + 1));
    if (!grown) return NULL;
    *groups = grown;
    MarketGroup *g = &grown[(*n)++];
    memset(g, 0, sizeof(*g));
    g->item = item;
    return g;
}

// The copy's paint key from its row's "m" (an object, or its JSON string), deob-10.0.4.js:131126 and :106577-106578.
static char *row_paint_key(const cJSON *row) {
    const cJSON *m = cJSON_GetObjectItemCaseSensitive(row, "m");
    cJSON *parsed = cJSON_IsString(m) && m->valuestring[0] ? cJSON_Parse(m->valuestring) : NULL;
    char *key = market_paint_key(parsed ? parsed : cJSON_IsObject(m) ? m : NULL);
    cJSON_Delete(parsed);
    return key;
}

char *market_paint_key(const cJSON *meta) {
    const cJSON *colour = meta ? cJSON_GetObjectItemCaseSensitive(meta, "paint") : NULL;
    const cJSON *shader = meta ? cJSON_GetObjectItemCaseSensitive(meta, "paintShader") : NULL;
    const int has_colour = cJSON_IsString(colour) && colour->valuestring[0];
    const int has_shader = cJSON_IsNumber(shader);
    if (!has_colour && !has_shader) return NULL;
    char *key = malloc(256);
    if (!key) return NULL;
    size_t n = 0;
    for (const char *s = has_colour ? colour->valuestring : ""; *s && n < 200; s++) { // the '#' dropped, case folded
        if (*s == '#') continue;
        key[n++] = (char) (*s >= 'A' && *s <= 'Z' ? *s - 'A' + 'a' : *s);
    }
    key[n] = 0;
    snprintf(key + n, 256 - n, "|s:%d", has_shader ? shader->valueint : -1);
    return key;
}

static void group_add_instance(MarketGroup *g, const int id, char *paint) {
    int *grown = realloc(g->instances, sizeof(int) * (size_t) (g->n_instances + 1));
    if (grown) g->instances = grown;
    char **paints = realloc(g->paints, sizeof(char *) * (size_t) (g->n_instances + 1));
    if (paints) g->paints = paints;
    if (!grown || !paints) {
        free(paint);
        return;
    }
    g->instances[g->n_instances] = id;
    g->paints[g->n_instances] = paint;
    g->n_instances++;
}

void market_group_clear(MarketGroup *g) {
    for (int i = 0; i < g->n_instances; i++) free(g->paints[i]);
    free(g->paints);
    free(g->instances);
    g->paints = NULL;
    g->instances = NULL;
    g->n_instances = 0;
}

int market_groups_parse(const cJSON *rows, MarketGroup **out, int *n) {
    *out = NULL;
    *n = 0;
    if (!cJSON_IsArray(rows)) return 0;
    for (const cJSON *row = rows->child; row; row = row->next) {
        const cJSON *i = cJSON_GetObjectItemCaseSensitive(row, "i");
        const cJSON *st = cJSON_GetObjectItemCaseSensitive(row, "st");
        const cJSON *sid = cJSON_GetObjectItemCaseSensitive(row, "sid");
        const cJSON *cnt = cJSON_GetObjectItemCaseSensitive(row, "cnt");
        if (!cJSON_IsNumber(i) || !cJSON_IsNumber(sid) || !sid->valueint) continue;
        MarketGroup *g = group_for(out, n, i->valueint);
        if (!g) {
            market_groups_free(*out, *n);
            *out = NULL;
            *n = 0;
            return -1;
        }
        if (cJSON_IsNumber(st) && st->valueint == 1) {
            g->stack_id = sid->valueint;
            g->stack_max = cJSON_IsNumber(cnt) ? cnt->valueint : 1;
        } else {
            group_add_instance(g, sid->valueint, row_paint_key(row));
        }
    }
    return 0;
}

void market_groups_free(MarketGroup *groups, const int n) {
    for (int i = 0; i < n; i++) market_group_clear(&groups[i]);
    free(groups);
}
