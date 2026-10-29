// The official inventory of a krunker.io account (kio_inventory.h): the loads and the writes that take its ids.
// Requests go through kio_store_write (the account's Bearer token). A load or write answer for an older load
// (the account changed, the inventory was cleared) is dropped by its generation.
#include <kio_inventory.h>

#include <kio_store.h>
#include <backend_ui.h> // active_account, account_kio_set_balances: a run's balances
#include <i18n.h>
#include "../viewer/viewer_store.h"

#include <stdlib.h>
#include <string.h>

#define BULK_SELL_CHUNK MARKET_BULK_QUICK_SELL_CHUNK
#define BUSY_TEXT "Server is busy. Please try again in a few seconds."

static struct {
    int gen;          // bumped by kio_inventory_clear: older answers are dropped
    int loaded;       // the groups and locks below are the account's
    int loading;
    MarketGroup *groups;
    int n;
    MarketLocks locks;
} I;

static const char *error_text(const char *code, const char *message) {
    const char *key = market_error_key(code);
    if (key) return I18N(key, BUSY_TEXT);
    return message ? message : I18N("generic.error", "Error");
}

// the answer's data is freed after the callback: what is kept is copied here (market_groups_parse copies)
static void apply_locks(const cJSON *data) {
    market_locks_apply(&I.locks, data ? cJSON_GetObjectItemCaseSensitive(data, "locks") : NULL);
}

// ---------------------------------------------------------------- the load: GET market/inventory, then items/locks

// A row's i is the official item index (deob-10.0.4.js:132197 resolves it through itemsMap and skips a row it
// does not know; :132059 keys the groups by item.index, :132209 is the stack's skin). KN's catalogue index is the
// same space: tools/ui-import/gen_viewer_store.js asserts store.skins[i].index === i, and viewer_skin(i) is that
// row. So a group whose index KN has no skin for is dropped here, not drawn as a blank card.
static void drop_unknown_items(MarketGroup *groups, int *n) {
    int kept = 0;
    for (int i = 0; i < *n; i++) {
        if (!viewer_skin(groups[i].item)) {
            market_group_clear(&groups[i]);
            continue;
        }
        if (kept != i) groups[kept] = groups[i];
        kept++;
    }
    *n = kept;
}

typedef struct {
    int gen;
    KioInvDone done;
    void *user;
} LoadReq;

static void load_finish(const LoadReq *r, const int ok, const char *message) {
    if (r->gen != I.gen) return;
    I.loading = 0;
    I.loaded = ok;
    if (r->done) r->done(r->user, ok, message);
}

static void locks_loaded(void *user, const int status, const cJSON *data, const char *code, const char *message) {
    (void) status;
    LoadReq *r = user;
    if (r->gen != I.gen) {
        free(r);
        return;
    }
    if (message || code) {
        load_finish(r, 0, error_text(code, message));
    } else {
        apply_locks(data);
        load_finish(r, 1, NULL);
    }
    free(r);
}

static void inventory_loaded(void *user, const int status, const cJSON *data, const char *code, const char *message) {
    (void) status;
    LoadReq *r = user;
    if (r->gen != I.gen) {
        free(r);
        return;
    }
    if (message || code) {
        load_finish(r, 0, error_text(code, message));
        free(r);
        return;
    }
    market_groups_free(I.groups, I.n);
    I.groups = NULL;
    I.n = 0;
    market_groups_parse(data, &I.groups, &I.n); // (ok: the answer is an array of rows; a non-array has no groups)
    drop_unknown_items(I.groups, &I.n);
    if (!kio_store_write("GET", "items/locks", NULL, locks_loaded, r)) {
        load_finish(r, 0, I18N("generic.error", "Error"));
        free(r);
    }
}

void kio_inventory_load(KioInvDone done, void *user) {
    if (!kio_store_active()) {
        if (done) done(user, 0, I18N("kn.inv.notKrunker", "Your inventory is on Box Office, not krunker.io."));
        return;
    }
    LoadReq *r = calloc(1, sizeof(*r));
    if (!r) {
        if (done) done(user, 0, I18N("generic.error", "Error"));
        return;
    }
    r->gen = I.gen;
    r->done = done;
    r->user = user;
    I.loading = 1;
    if (!kio_store_write("GET", "market/inventory", NULL, inventory_loaded, r)) {
        I.loading = 0;
        free(r);
        if (done) done(user, 0, I18N("generic.error", "Error"));
    }
}

int kio_inventory_ready(void) { return I.loaded && kio_store_active(); }

int kio_inventory_copy_for_paint(const int item, const char *key) {
    if (!I.loaded || !key) return 0;
    for (int i = 0; i < I.n; i++) {
        if (I.groups[i].item != item) continue;
        int found = 0, sid = 0;
        for (int k = 0; k < I.groups[i].n_instances; k++) {
            if (I.groups[i].paints[k] && !strcmp(I.groups[i].paints[k], key)) {
                found++;
                sid = I.groups[i].instances[k];
            }
        }
        return found == 1 ? sid : 0;
    }
    return 0;
}
int kio_inventory_loading(void) { return I.loading; }

void kio_inventory_clear(void) {
    I.gen++;
    I.loaded = 0;
    I.loading = 0;
    market_groups_free(I.groups, I.n);
    I.groups = NULL;
    I.n = 0;
    market_locks_free(&I.locks);
}

const MarketGroup *kio_inventory_groups(int *n) {
    if (n) *n = I.loaded ? I.n : 0;
    return I.loaded ? I.groups : NULL;
}

const MarketLocks *kio_inventory_locks(void) { return &I.locks; }

int kio_inventory_locked(const MarketGroup *g, const int copy) {
    if (copy < 0) return market_locks_has(&I.locks, 0, g->item, 1);
    return market_locks_has(&I.locks, g->instances[copy], g->item, 0);
}

const char *kio_inventory_gate_text(void) {
    return I18N("kn.inv.notLoaded", "Your krunker.io inventory is still loading. Try again in a moment.");
}

// ---------------------------------------------------------------- the writes

// a write's answer: the callback, for a request of the current generation
typedef struct {
    int gen;
    KioInvDone done;
    void *user;
    int kind; // 0 lock, 1 sell, 2 list
    // lock: the body's ids, to set the local state on success
    int skin, item, locked;
    // bulk: the run
    MarketPicks picks;
    int *quantity;
    int price, action, from;
    int count;
    // list: the batches' cursor (market_list_batch): the rows, the row and the units of it already sent
    MarketListRow *rows;
    int nrows, row, used;
} Req;

static void req_free(Req *r) {
    market_picks_free(&r->picks);
    free(r->quantity);
    free(r->rows);
    free(r);
}

static int in_gen(const Req *r) { return r->gen == I.gen; }

// the last bulk run's figures (kio_inventory_bulk_result / _progress): what the answers said
static KioBulkResult g_result;
static int g_progress, g_total;

// a run starts: its units to send, and the balances its gains are counted from (deob-10.0.4.js:146845: funds, junk)
static void result_reset(const int total) {
    memset(&g_result, 0, sizeof(g_result));
    const AccountInfo *a = active_account();
    g_result.funds_before = a ? a->funds : 0;
    g_result.junk_before = a ? a->junk : 0;
    g_progress = 0;
    g_total = total;
}

static void bulk_next(Req *r);

// the run ends: a success reloads the official inventory first, so done sees the account's new rows
static void bulk_done(Req *r, const int ok, const char *message) {
    const KioInvDone done = r->done;
    void *user = r->user;
    const int current = in_gen(r);
    req_free(r);
    if (!ok || !current) {
        if (done) done(user, 0, ok ? NULL : message);
        return;
    }
    kio_inventory_load(done, user);
}

static void bulk_answer(void *user, const int status, const cJSON *data, const char *code, const char *message) {
    (void) status;
    Req *r = user;
    if (!in_gen(r)) {
        req_free(r);
        return;
    }
    if (message || code) {
        bulk_done(r, 0, error_text(code, message));
        return;
    }
    // the answer's figures (deob-10.0.4.js:146862-146866 for a sell: sold / skipped ids, funds and junk after it;
    // :131995-132019 for a list: listed, funds, results[].error), and the new balances on the account
    const cJSON *funds = cJSON_GetObjectItemCaseSensitive(data, "funds"), *junk = cJSON_GetObjectItemCaseSensitive(data, "junk");
    if (r->kind == 1) {
        const cJSON *sold = cJSON_GetObjectItemCaseSensitive(data, "sold"), *skipped = cJSON_GetObjectItemCaseSensitive(data, "skipped");
        g_result.sold += cJSON_IsArray(sold) ? cJSON_GetArraySize(sold) : 0;
        g_result.skipped += cJSON_IsArray(skipped) ? cJSON_GetArraySize(skipped) : 0;
        if (cJSON_IsNumber(funds)) g_result.kr_gained = funds->valuedouble - g_result.funds_before;
        if (cJSON_IsNumber(junk)) g_result.junk_gained = junk->valuedouble - g_result.junk_before;
        g_progress += r->count;
    } else {
        const cJSON *listed = cJSON_GetObjectItemCaseSensitive(data, "listed"), *results = cJSON_GetObjectItemCaseSensitive(data, "results"), *e;
        if (cJSON_IsNumber(listed)) g_result.listed += listed->valueint;
        int failed = 0;
        cJSON_ArrayForEach(e, results) {
            const char *err = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(e, "error"));
            if (!err || !err[0]) continue;
            failed++;
            if (!g_result.error[0]) {
                const char *text = error_text(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(e, "code")), err);
                snprintf(g_result.error, sizeof(g_result.error), "%s", text ? text : err);
            }
        }
        g_progress = g_progress + r->count;
        if (failed) { // a batch with failed rows ends the run, as the bundle's loop does (:132020-132022)
            account_kio_set_balances(cJSON_IsNumber(funds), cJSON_IsNumber(funds) ? funds->valuedouble : 0, 0, 0);
            bulk_done(r, 1, NULL);
            return;
        }
    }
    account_kio_set_balances(cJSON_IsNumber(funds), cJSON_IsNumber(funds) ? funds->valuedouble : 0, cJSON_IsNumber(junk),
                             cJSON_IsNumber(junk) ? junk->valuedouble : 0);
    r->from += r->count;
    bulk_next(r);
}

static void bulk_next(Req *r) {
    cJSON *body;
    const char *path;
    if (r->kind == 1) {
        const int n = r->picks.n;
        if (r->from >= n) {
            bulk_done(r, 1, NULL);
            return;
        }
        r->count = n - r->from < BULK_SELL_CHUNK ? n - r->from : BULK_SELL_CHUNK;
        body = market_quick_sell_body(r->action, &r->picks, r->from, r->count);
        path = "market/quick-sell/bulk";
    } else {
        // the list run's cursor lives in the batches (market_list_batch); NULL = every row is sent, or no memory
        int units = 0;
        body = market_list_batch(r->rows, r->nrows, &r->row, &r->used, &units);
        if (!body) {
            const int finished = r->row >= r->nrows;
            bulk_done(r, finished, finished ? NULL : I18N("generic.error", "Error"));
            return;
        }
        r->count = units; // the batch's units: the progress the answer adds
        path = "market/list/bulk";
    }
    char *text = body ? cJSON_PrintUnformatted(body) : NULL;
    cJSON_Delete(body);
    const int sent = text && kio_store_write("POST", path, text, bulk_answer, r);
    free(text);
    if (!sent) bulk_done(r, 0, I18N("generic.error", "Error"));
}

static int quantity_of(const MarketGroup *groups, const int n, const int skin, const int item) {
    for (int i = 0; i < n; i++) {
        if (groups[i].item != item) continue;
        if (groups[i].stack_id == skin) return groups[i].stack_max > 0 ? groups[i].stack_max : 1;
    }
    return 1; // a copy: one
}

static int start_bulk(const int kind, const int action, const int price, MarketPicks picks, KioInvDone done, void *user) {
    if (!kio_inventory_ready() || picks.n == 0) {
        market_picks_free(&picks);
        if (done) done(user, 0, picks.n == 0 ? I18N("kn.inv.noneSelected", "Select some items first.") : kio_inventory_gate_text());
        return 0;
    }
    Req *r = calloc(1, sizeof(*r));
    if (!r) {
        market_picks_free(&picks);
        if (done) done(user, 0, I18N("generic.error", "Error"));
        return 0;
    }
    r->gen = I.gen;
    r->done = done;
    r->user = user;
    r->kind = kind;
    r->action = action;
    r->price = price;
    r->picks = picks;
    result_reset(picks.n);
    r->quantity = calloc((size_t) picks.n, sizeof(int));
    for (int i = 0; r->quantity && i < picks.n; i++) r->quantity[i] = quantity_of(I.groups, I.n, picks.skin_ids[i], picks.item_ids[i]);
    if (!r->quantity) {
        req_free(r);
        if (done) done(user, 0, I18N("generic.error", "Error"));
        return 0;
    }
    if (kind == 2) { // one row per pick: a stack lists its quantity, a copy one (the List Items modal, deob-10.0.4.js:131924-131943)
        r->rows = calloc((size_t) picks.n, sizeof(MarketListRow));
        if (!r->rows) {
            req_free(r);
            if (done) done(user, 0, I18N("generic.error", "Error"));
            return 0;
        }
        r->nrows = picks.n;
        for (int i = 0; i < picks.n; i++) r->rows[i] = (MarketListRow) {picks.item_ids[i], picks.skin_ids[i], price, r->quantity[i]};
    }
    bulk_next(r);
    return 1;
}

int kio_inventory_bulk_sell(const int action, MarketPicks picks, KioInvDone done, void *user) {
    return start_bulk(1, action, 0, picks, done, user);
}

int kio_inventory_bulk_list(const int price, MarketPicks picks, KioInvDone done, void *user) {
    return start_bulk(2, 0, price, picks, done, user);
}

// The List Items modal's rows: each its own price and quantity (deob-10.0.4.js:131924-131943). Copied.
int kio_inventory_bulk_list_rows(const MarketListRow *rows, const int n, KioInvDone done, void *user) {
    int units = 0;
    for (int i = 0; i < n; i++) units += rows[i].quantity > 0 ? rows[i].quantity : 0;
    if (!kio_inventory_ready() || n <= 0 || units <= 0) {
        if (done) done(user, 0, n <= 0 || units <= 0 ? I18N("kn.inv.noneSelected", "Select some items first.") : kio_inventory_gate_text());
        return 0;
    }
    Req *r = calloc(1, sizeof(*r));
    MarketListRow *copy = r ? malloc(sizeof(MarketListRow) * (size_t) n) : NULL;
    if (!r || !copy) {
        free(r);
        free(copy);
        if (done) done(user, 0, I18N("generic.error", "Error"));
        return 0;
    }
    memcpy(copy, rows, sizeof(MarketListRow) * (size_t) n);
    r->gen = I.gen;
    r->done = done;
    r->user = user;
    r->kind = 2;
    r->rows = copy;
    r->nrows = n;
    result_reset(units);
    bulk_next(r);
    return 1;
}

const KioBulkResult *kio_inventory_bulk_result(void) { return &g_result; }

void kio_inventory_bulk_progress(int *done, int *total) {
    *done = g_progress < g_total ? g_progress : g_total;
    *total = g_total;
}

// a lock write's answer: the locks are read again, done runs once they are applied
static void locks_reread(void *user, const int status, const cJSON *data, const char *code, const char *message) {
    (void) status;
    Req *r = user;
    const KioInvDone done = r->done;
    void *u = r->user;
    const int current = in_gen(r);
    req_free(r);
    if (!current) return;
    if (message || code) {
        if (done) done(u, 0, error_text(code, message));
        return;
    }
    apply_locks(data);
    if (done) done(u, 1, NULL);
}

static void lock_answer(void *user, const int status, const cJSON *data, const char *code, const char *message) {
    (void) status;
    (void) data;
    Req *r = user;
    if (!in_gen(r) || message || code) {
        const KioInvDone done = r->done;
        void *u = r->user;
        const int current = in_gen(r);
        req_free(r);
        if (current && done) done(u, 0, error_text(code, message));
        return;
    }
    if (!kio_store_write("GET", "items/locks", NULL, locks_reread, r)) {
        const KioInvDone done = r->done;
        void *u = r->user;
        req_free(r);
        if (done) done(u, 0, I18N("generic.error", "Error"));
    }
}

int kio_inventory_set_lock(const MarketGroup *g, const int copy, const int locked, KioInvDone done, void *user) {
    if (!kio_inventory_ready()) {
        if (done) done(user, 0, kio_inventory_gate_text());
        return 0;
    }
    const int skin = copy >= 0 ? g->instances[copy] : 0;
    cJSON *body = market_lock_body(skin, copy >= 0 ? 0 : g->item, locked);
    char *text = body ? cJSON_PrintUnformatted(body) : NULL;
    cJSON_Delete(body);
    Req *r = calloc(1, sizeof(*r));
    if (!r || !text) {
        free(r);
        free(text);
        if (done) done(user, 0, I18N("generic.error", "Error"));
        return 0;
    }
    r->gen = I.gen;
    r->done = done;
    r->user = user;
    r->kind = 0;
    const int sent = kio_store_write("POST", "items/locks", text, lock_answer, r);
    free(text);
    if (!sent) {
        req_free(r);
        if (done) done(user, 0, I18N("generic.error", "Error"));
    }
    return sent;
}
