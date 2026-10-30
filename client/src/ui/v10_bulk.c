// Krunker v10.0.4's bulk market overlays (client/include/v10_bulk.h): the bulk quick-sell confirm, List Items and Buy
// Multiple, for a krunker.io account's official inventory and listings. One overlay at a time, pushed on the v10 view
// stack; the markup is the bundle's (svelte-1pxa8ry, svelte-1u5rc76, svelte-dk2h1b in ui/css/main.css).
//
// The inputs report on change (the field's value once it is committed), not on each key: the overlay's markup is
// redrawn after each change, and a redraw between keys would take the caret.
#include <v10_bulk.h>
#include <inv_source.h> // the active account's inventory: krunker.io's or Box Office's
#include <v10_ui.h>
#include <v10_wallet.h> // the inventory's notification list (the quick-sell results)
#include <kio_store.h>
#include <backend_ui.h>
#include <economy_client.h> // econ_rarity_sell: a listing's minimum price
#include <i18n.h>
#include <html_ui.h>
#include "../viewer/viewer_store.h"
#include <math.h>
#include <stdint.h> // intptr_t: an answer's overlay generation
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LIST_MAX_PRICE 10000000 // iííìîîì: "Price cannot exceed 10,000,000 KR" (deob-10.0.4.js:109011)
#define SELL_GROUPS_SHOWN 8     // deob-10.0.4.js:146976: the first 8 groups, then "and N more" (:146983-146987)

enum { OV_NONE, OV_SELL, OV_LIST, OV_BUY };

// one List Items row: an item's stack (stack_id, max) or its picked copies (copies), its quantity and price as typed
typedef struct {
    int item;
    int stack_id, max;
    int *copies;
    int n_copies;
    int qty;   // a stack's quantity (0..max); copies list them all
    int price; // 0: not typed
} ListRow;

static struct {
    int kind;
    int depth;  // v10_view_depth() with the overlay on top
    int busy;   // a run or request is in flight
    char error[200];
    KioInvDone after;
    void *user;
    // sell
    int action;
    MarketPicks picks;
    // list
    ListRow *rows;
    int n_rows;
    // buy
    int item;
    int loading;
    double *prices;
    int n_prices;
    int qty;
    int gen; // a buy-ladder answer for an older overlay is dropped
} B;

// ---------------------------------------------------------------- helpers

static const ViewerSkin *skin_of(const int item) { return viewer_skin(item); }

static const char *item_name(const int item) {
    const ViewerSkin *s = skin_of(item);
    return s && s->name && s->name[0] ? s->name : "Unnamed item";
}

static const char *rarity_color(const int item) {
    const ViewerSkin *s = skin_of(item);
    return s && s->rarity >= 0 && s->rarity < g_viewer_rarity_count ? g_viewer_rarities[s->rarity].color : "#B0B0B0";
}

static void fmt_int(char *out, const size_t n, const double v) { v10_fmt_int(out, n, v); }

// iììíííï(parseFloat(x.toFixed(2))): a Junk amount, two decimals at most (deob-10.0.4.js:146999)
static void fmt_junk(char *out, const size_t n, const double v) {
    snprintf(out, n, "%.2f", v);
    char *dot = strchr(out, '.');
    if (!dot) return;
    char *end = out + strlen(out) - 1;
    while (end > dot && *end == '0') *end-- = 0;
    if (end == dot) *end = 0;
}

static int arg_int(const HtmlArg *args, const int argc, const int i) {
    if (!args || i >= argc) return -1;
    if (args[i].kind == HTML_ARG_NUM || args[i].kind == HTML_ARG_BOOL) return (int) args[i].num;
    return args[i].str ? (int) floor(atof(args[i].str)) : -1; // Math.floor(Number(v)) || 0
}

static double funds(void) {
    const AccountInfo *a = active_account();
    return a ? a->funds : 0;
}

// a pick's units: a stack sells or lists its count less the units the Keep options keep (MarketPicks.keeps), a copy one
static int pick_units_kept(const int skin, const int item, const int keep) {
    int n = 0;
    const MarketGroup *g = inv_groups(&n);
    for (int i = 0; g && i < n; i++)
        if (g[i].item == item && g[i].stack_id == skin) {
            const int units = (g[i].stack_max > 0 ? g[i].stack_max : 1) - (keep > 0 ? keep : 0);
            return units > 0 ? units : 0;
        }
    return 1;
}

static int pick_units(const int skin, const int item) { return pick_units_kept(skin, item, 0); }

// iîìïìïî's value (deob-10.0.4.js:108926-108962) from the item's store entry
static MarketQsValue qs_value(const int item) { return market_qs_value(ui_loadout_skin_json(item)); }

static int takes(const MarketQsValue *v, const int action) {
    return action == 0 ? v->can_kr : action == 1 ? v->can_junk : v->can_destroy;
}

void v10_bulk_sums(const MarketPicks *picks, int *n_kr, int *n_junk, int *n_destroy, double *kr, double *junk) {
    int a = 0, b = 0, c = 0;
    double sk = 0, sj = 0;
    for (int i = 0; picks && i < picks->n; i++) {
        const MarketQsValue v = qs_value(picks->item_ids[i]);
        const int units = pick_units_kept(picks->skin_ids[i], picks->item_ids[i], picks->keeps ? picks->keeps[i] : 0);
        if (v.can_kr) a++, sk += v.kr * units;
        if (v.can_junk) b++, sj += v.junk * units;
        if (v.can_destroy) c++;
    }
    if (n_kr) *n_kr = a;
    if (n_junk) *n_junk = b;
    if (n_destroy) *n_destroy = c;
    if (kr) *kr = sk;
    if (junk) *junk = sj;
}

MarketPicks v10_bulk_filter(const MarketPicks *picks, const int action) {
    MarketPicks out = {0};
    if (!picks || picks->n <= 0) return out;
    out.skin_ids = malloc(sizeof(int) * (size_t) picks->n);
    out.item_ids = malloc(sizeof(int) * (size_t) picks->n);
    out.keeps = calloc((size_t) picks->n, sizeof(int));
    if (!out.skin_ids || !out.item_ids || !out.keeps) {
        market_picks_free(&out);
        return out;
    }
    out.cap = picks->n;
    for (int i = 0; i < picks->n; i++) {
        const MarketQsValue v = qs_value(picks->item_ids[i]);
        if (!takes(&v, action)) continue;
        out.skin_ids[out.n] = picks->skin_ids[i];
        out.item_ids[out.n] = picks->item_ids[i];
        out.keeps[out.n] = picks->keeps ? picks->keeps[i] : 0;
        out.n++;
    }
    return out;
}

// ---------------------------------------------------------------- state

static void rows_free(void) {
    for (int i = 0; i < B.n_rows; i++) free(B.rows[i].copies);
    free(B.rows);
    B.rows = NULL;
    B.n_rows = 0;
}

static void reset(void) {
    market_picks_free(&B.picks);
    rows_free();
    free(B.prices);
    const int gen = B.gen;
    memset(&B, 0, sizeof(B));
    B.gen = gen + 1;
}

static char *markup(void);

static void render(void) {
    if (B.kind == OV_NONE || v10_view_depth() != B.depth) return;
    v10_view_set(markup());
}

// the overlay left the stack (Cancel, the backdrop, Escape): a cancel reports to the opener
static void closed(const int accepted, void *user) {
    (void) user;
    const KioInvDone after = B.after;
    void *u = B.user;
    reset();
    // a run popped mid-way (Escape over the wallet) keeps going; its result is still a notification (g_run_action)
    if (!accepted && after) after(u, 0, NULL);
}

static void push(const int kind, KioInvDone after, void *user) {
    if (B.kind != OV_NONE) v10_view_pop(0);
    B.kind = kind;
    B.after = after;
    B.user = user;
    v10_view_push(markup(), render, closed, NULL);
    B.depth = v10_view_depth();
}

// the overlay's run ended well: it closes, then the opener hears (its own redraw comes after)
static void finish_ok(void) {
    const KioInvDone after = B.after;
    void *u = B.user;
    B.after = NULL;
    B.busy = 0;
    if (B.kind != OV_NONE && v10_view_depth() == B.depth) v10_view_pop(1);
    else reset();
    if (after) after(u, 1, NULL);
}

static void fail(const char *message) {
    B.busy = 0;
    snprintf(B.error, sizeof(B.error), "%s", message && message[0] ? message : I18N("generic.error", "Error"));
    render();
}

// ---------------------------------------------------------------- bulk quick-sell (svelte-1pxa8ry)

static void sell_markup(StrBuf *sb) {
    char title[96], num[48], line[160];
    const int count = B.picks.n;
    snprintf(num, sizeof(num), "%d", count);
    // deob-10.0.4.js:146954: the title by action
    if (B.action == 2) I18N_F(title, "wallet.bulk.title.destroy", "Destroy ({0})", num);
    else if (B.action == 1) I18N_F(title, "wallet.bulk.title.junk", "Sell for Junk ({0})", num);
    else I18N_F(title, "wallet.bulk.title.kr", "Sell for KR ({0})", num);
    sb_append(sb, "<div class=\"bqs-overlay svelte-1pxa8ry\" role=\"presentation\" onclick=\"knV10BulkCancel()\">"
                  "<div class=\"bqs-window svelte-1pxa8ry\" role=\"dialog\" aria-modal=\"true\" aria-label=\"");
    sb_append_esc(sb, title);
    sb_append(sb, "\" onclick=\"event.stopPropagation()\"><div class=\"bqs-title svelte-1pxa8ry\">");
    sb_append_esc(sb, title);
    sb_append(sb, "</div> <div class=\"bqs-groups svelte-1pxa8ry\">");
    // deob-10.0.4.js:146936-146950: one group per item (index:name), its pick count, most first
    int *items = calloc((size_t) (count ? count : 1), sizeof(int)), *counts = calloc((size_t) (count ? count : 1), sizeof(int));
    int groups = 0;
    for (int i = 0; items && counts && i < count; i++) {
        int g = 0;
        while (g < groups && items[g] != B.picks.item_ids[i]) g++;
        if (g == groups) items[groups++] = B.picks.item_ids[i];
        counts[g]++;
    }
    for (int shown = 0; shown < groups && shown < SELL_GROUPS_SHOWN; shown++) {
        int best = -1;
        for (int g = 0; g < groups; g++)
            if (counts[g] > 0 && (best < 0 || counts[g] > counts[best])) best = g;
        if (best < 0) break;
        sb_appendf(sb, "<div class=\"bqs-group svelte-1pxa8ry\" style=\"color:%s\"><span class=\"bqs-group-name svelte-1pxa8ry\">", rarity_color(items[best]));
        sb_append_esc(sb, item_name(items[best]));
        sb_appendf(sb, "</span> <span class=\"bqs-group-count svelte-1pxa8ry\">\xc3\x97%d</span></div>", counts[best]);
        counts[best] = -counts[best]; // drawn
    }
    if (groups > SELL_GROUPS_SHOWN) {
        snprintf(num, sizeof(num), "%d", groups - SELL_GROUPS_SHOWN);
        sb_append(sb, " <div class=\"bqs-more svelte-1pxa8ry\">");
        sb_append_esc(sb, I18N_F(line, "wallet.bulk.more", "and {0} more", num));
        sb_append(sb, "</div>");
    }
    free(items);
    free(counts);
    sb_append(sb, "</div> ");
    // deob-10.0.4.js:146989-147006: what the sale pays (not for Destroy)
    if (B.action != 2) {
        double kr = 0, junk = 0;
        v10_bulk_sums(&B.picks, NULL, NULL, NULL, &kr, &junk);
        sb_append(sb, "<div class=\"bqs-total svelte-1pxa8ry\">");
        sb_append_esc(sb, I18N("wallet.bulk.receive", "You will receive"));
        sb_append(sb, " ");
        if (B.action == 0) {
            fmt_int(num, sizeof(num), kr);
            sb_appendf(sb, "<span class=\"bqs-total-num svelte-1pxa8ry\">%s</span> KR", num);
        } else {
            fmt_junk(num, sizeof(num), junk);
            sb_appendf(sb, "<span class=\"bqs-total-num svelte-1pxa8ry\">%s</span> Junk", num);
        }
        sb_append(sb, "</div> ");
    }
    sb_append(sb, "<div class=\"bqs-warning svelte-1pxa8ry\">");
    sb_append_esc(sb, I18N("wallet.bulk.irreversible", "This can't be undone."));
    sb_append(sb, "</div> ");
    if (B.busy) { // deob-10.0.4.js:147011-147019
        int done = 0, total = 0;
        inv_bulk_progress(&done, &total);
        char a[16], b[16];
        snprintf(a, sizeof(a), "%d", done);
        snprintf(b, sizeof(b), "%d", B.picks.n);
        sb_appendf(sb, "<div class=\"bqs-progress svelte-1pxa8ry\"><div class=\"bqs-progress-bar svelte-1pxa8ry\" style=\"width:%.1f%%\"></div></div> "
                       "<div class=\"bqs-progress-text svelte-1pxa8ry\">",
                   total > 0 ? 100.0 * done / total : 0.0);
        sb_append_esc(sb, I18N_F(line, "wallet.bulk.progress", "Selling {0} / {1}", a, b));
        sb_append(sb, "</div> ");
    }
    if (B.error[0]) {
        sb_append(sb, "<div class=\"bqs-error svelte-1pxa8ry\">");
        sb_append_esc(sb, B.error);
        sb_append(sb, "</div> ");
    }
    const char *btn = B.action == 2 ? "bqs-btn-destroy" : B.action == 1 ? "bqs-btn-junk" : "bqs-btn-kr";
    sb_appendf(sb, "<div class=\"bqs-actions svelte-1pxa8ry\"><button type=\"button\" class=\"bqs-btn bqs-btn-cancel svelte-1pxa8ry\"%s onclick=\"knV10BulkCancel()\">",
               B.busy ? " disabled" : "");
    sb_append_esc(sb, I18N("wallet.bulk.cancel", "Cancel"));
    sb_appendf(sb, "</button> <button type=\"button\" class=\"bqs-btn %s svelte-1pxa8ry\" id=\"knV10BulkConfirm\"%s onclick=\"knV10BulkConfirm()\">", btn,
               B.busy ? " disabled" : "");
    sb_append_esc(sb, B.busy ? "\xe2\x80\xa6" : I18N("wallet.bulk.confirm", "Confirm"));
    sb_append(sb, "</button></div></div></div>");
}

static int g_run_action; // the sell run's action, for its result (the overlay may be gone by then)

// the run ended: the result in the inventory's notification list (deob-10.0.5.js:147362-147370: success
// "wallet.bulk.result.*", then error "wallet.bulk.result.skipped" when some were skipped), then the overlay
// closes. A failure is withAwait's error: the overlay's, or the io error popup once the overlay is gone.
static void sell_done(void *user, const int ok, const char *message) {
    (void) user;
    if (!ok) {
        if (B.kind == OV_SELL) fail(message);
        else v10_view_push(v10_io_error(I18N("kn.inv.quickSellFailedTitle", "Quick sell"), message ? message : I18N("generic.error", "Error")), NULL, NULL, NULL);
        return;
    }
    const KioBulkResult *r = inv_bulk_result();
    char a[24], b[48], text[160];
    snprintf(a, sizeof(a), "%d", r->sold);
    if (g_run_action == 2) I18N_F(text, "wallet.bulk.result.destroy", "Destroyed {0}", a);
    else if (g_run_action == 1) {
        fmt_junk(b, sizeof(b), r->junk_gained);
        I18N_F(text, "wallet.bulk.result.junk", "Sold {0} for {1} Junk", a, b);
    } else {
        fmt_int(b, sizeof(b), r->kr_gained);
        I18N_F(text, "wallet.bulk.result.kr", "Sold {0} for {1} KR", a, b);
    }
    v10_wallet_note(1, text);
    if (r->skipped) {
        snprintf(a, sizeof(a), "%d", r->skipped);
        v10_wallet_note(0, I18N_F(text, "wallet.bulk.result.skipped", "Skipped {0} (listed or no longer available)", a));
    }
    finish_ok();
}

static void sell_confirm(void) {
    if (B.busy || B.picks.n == 0) return;
    // the run owns a copy of the picks: the overlay keeps its own to draw while it goes
    MarketPicks run = {0};
    run.skin_ids = malloc(sizeof(int) * (size_t) B.picks.n);
    run.item_ids = malloc(sizeof(int) * (size_t) B.picks.n);
    if (!run.skin_ids || !run.item_ids) {
        market_picks_free(&run);
        fail(NULL);
        return;
    }
    memcpy(run.skin_ids, B.picks.skin_ids, sizeof(int) * (size_t) B.picks.n);
    memcpy(run.item_ids, B.picks.item_ids, sizeof(int) * (size_t) B.picks.n);
    run.keeps = calloc((size_t) B.picks.n, sizeof(int));
    if (run.keeps && B.picks.keeps) memcpy(run.keeps, B.picks.keeps, sizeof(int) * (size_t) B.picks.n);
    run.n = run.cap = B.picks.n;
    g_run_action = B.action;
    B.busy = 1;
    B.error[0] = 0;
    render();
    if (!inv_bulk_sell(B.action, run, sell_done, NULL)) B.busy = 0; // done already ran with the error
}

void v10_bulk_sell_open(const int action, MarketPicks picks, KioInvDone after, void *user) {
    if (picks.n == 0) {
        market_picks_free(&picks);
        return;
    }
    if (B.kind != OV_NONE) v10_view_pop(0);
    B.action = action >= 0 && action <= 2 ? action : 0;
    B.picks = picks;
    push(OV_SELL, after, user);
}

// ---------------------------------------------------------------- List Items (svelte-1u5rc76)

// iìïìïìî (deob-10.0.4.js:109006-109012): the price rule of one row, NULL when the price is fine
static const char *price_error(const int item, const int price, char *buf, const size_t n) {
    const ViewerSkin *s = skin_of(item);
    if (!s) return "Item not found";
    const int min = s->type == 10 ? 1 : econ_rarity_sell(s->rarity); // iíiììîí: the rarity's minimum (collectibles 1)
    if (price < min) {
        snprintf(buf, n, "Price must be at least %d KR for this rarity", min);
        return buf;
    }
    if (price > LIST_MAX_PRICE) return "Price cannot exceed 10,000,000 KR";
    return NULL;
}

static int row_units(const ListRow *r) { // IÎiìîîí: a stack's quantity clamped to 0..max, else its copies
    if (r->stack_id) return r->qty < 0 ? 0 : r->qty > r->max ? r->max : r->qty;
    return r->n_copies;
}

static int list_invalid(void) { // IÌiiîïì: a row with units and a price the rule refuses
    char buf[96];
    for (int i = 0; i < B.n_rows; i++)
        if (row_units(&B.rows[i]) > 0 && price_error(B.rows[i].item, B.rows[i].price, buf, sizeof(buf))) return 1;
    return 0;
}

static void list_totals(int *units, long *fee, double *value) {
    *units = 0;
    *fee = 0;
    *value = 0;
    for (int i = 0; i < B.n_rows; i++) {
        const int u = row_units(&B.rows[i]);
        *units += u;
        *fee += market_list_fee(B.rows[i].price, u); // u * ceil(0.1 * price) (deob-10.0.4.js:132071)
        *value += (double) u * B.rows[i].price;
    }
}

static void list_markup(StrBuf *sb) {
    char buf[128], num[32];
    sb_append(sb, "<div class=\"bl-overlay svelte-1u5rc76\" role=\"presentation\" onclick=\"knV10BulkCancel()\">"
                  "<div class=\"bl-window svelte-1u5rc76\" role=\"dialog\" aria-modal=\"true\" aria-label=\"");
    sb_append_esc(sb, I18N("market.listMulti.title", "List Items"));
    sb_append(sb, "\" onclick=\"event.stopPropagation()\"><div class=\"bl-title svelte-1u5rc76\">");
    sb_append_esc(sb, I18N("market.listMulti.title", "List Items"));
    sb_append(sb, "</div> <div class=\"bl-groups svelte-1u5rc76\">");
    for (int i = 0; i < B.n_rows; i++) {
        const ListRow *r = &B.rows[i];
        sb_append(sb, "<div class=\"bl-group svelte-1u5rc76\"><div class=\"bl-name svelte-1u5rc76\" style=\"color:");
        sb_append(sb, rarity_color(r->item));
        sb_append(sb, "\">");
        sb_append_esc(sb, item_name(r->item));
        sb_append(sb, "</div> ");
        if (r->stack_id) { // a stack: its quantity, "/ max" (deob-10.0.4.js:131998)
            sb_append(sb, "<label class=\"bl-field svelte-1u5rc76\"><span class=\"svelte-1u5rc76\">");
            sb_append_esc(sb, I18N("market.buyMulti.quantity", "Quantity"));
            sb_appendf(sb, "</span> <input type=\"number\" min=\"1\" max=\"%d\" class=\"svelte-1u5rc76\" value=\"%d\"%s onchange=\"knV10BulkQty(%d,this.value)\">"
                           " <span class=\"bl-dim svelte-1u5rc76\">/ %d</span></label> ",
                       r->max, row_units(r), B.busy ? " disabled" : "", i, r->max);
        } else {
            sb_appendf(sb, "<span class=\"bl-count svelte-1u5rc76\">\xc3\x97%d</span> ", r->n_copies);
        }
        const int bad = row_units(r) > 0 && r->price > 0 && price_error(r->item, r->price, buf, sizeof(buf));
        sb_append(sb, "<label class=\"bl-field svelte-1u5rc76\"><span class=\"svelte-1u5rc76\">");
        sb_append_esc(sb, I18N("market.listMulti.price", "Price"));
        sb_appendf(sb, "</span> <input type=\"number\" class=\"svelte-1u5rc76%s\"", bad ? " bl-bad" : "");
        if (r->price > 0) sb_appendf(sb, " value=\"%d\"", r->price);
        if (bad) {
            sb_append(sb, " title=\"");
            sb_append_esc(sb, price_error(r->item, r->price, buf, sizeof(buf)));
            sb_append(sb, "\"");
        }
        sb_appendf(sb, "%s onchange=\"knV10BulkPrice(%d,this.value)\"> <span class=\"bl-dim svelte-1u5rc76\">KR</span></label></div>",
                   B.busy ? " disabled" : "", i);
    }
    int units;
    long fee;
    double value;
    list_totals(&units, &fee, &value);
    sb_append(sb, "</div> <div class=\"bl-summary svelte-1u5rc76\"><div class=\"bl-row svelte-1u5rc76\"><span class=\"svelte-1u5rc76\">");
    sb_append_esc(sb, I18N("market.listMulti.units", "Listings"));
    sb_appendf(sb, "</span> <span class=\"svelte-1u5rc76\">%d</span></div> <div class=\"bl-row svelte-1u5rc76\"><span class=\"svelte-1u5rc76\">", units);
    sb_append_esc(sb, I18N("market.listMulti.fee", "Listing fee"));
    fmt_int(num, sizeof(num), (double) fee);
    sb_appendf(sb, "</span> <span class=\"bl-fee svelte-1u5rc76\">%s KR</span></div> <div class=\"bl-row svelte-1u5rc76\"><span class=\"svelte-1u5rc76\">", num);
    sb_append_esc(sb, I18N("market.listMulti.value", "Total price"));
    fmt_int(num, sizeof(num), value);
    sb_appendf(sb, "</span> <span class=\"svelte-1u5rc76\">%s KR</span></div></div> <div class=\"bl-note svelte-1u5rc76\">", num);
    sb_append_esc(sb, I18N("market.listMulti.note", "Each listing costs a 10% fee when it's listed. Your listing limit still applies."));
    sb_append(sb, "</div> ");
    if (B.busy) {
        int done = 0, total = 0;
        inv_bulk_progress(&done, &total);
        sb_appendf(sb, "<div class=\"bl-progress svelte-1u5rc76\"><div class=\"bl-progress-bar svelte-1u5rc76\" style=\"width:%.1f%%\"></div></div> ",
                   total > 0 ? 100.0 * done / total : 0.0);
    }
    if (B.error[0]) {
        sb_append(sb, "<div class=\"bl-error svelte-1u5rc76\">");
        sb_append_esc(sb, B.error);
        sb_append(sb, "</div> ");
    }
    const int disabled = B.busy || list_invalid() || units == 0; // deob-10.0.4.js:132179
    snprintf(num, sizeof(num), "%d", units);
    sb_appendf(sb, "<div class=\"bl-actions svelte-1u5rc76\"><button type=\"button\" class=\"bl-btn bl-btn-cancel svelte-1u5rc76\"%s onclick=\"knV10BulkCancel()\">",
               B.busy ? " disabled" : "");
    sb_append_esc(sb, I18N("wallet.bulk.cancel", "Cancel"));
    sb_appendf(sb, "</button> <button type=\"button\" class=\"bl-btn bl-btn-list svelte-1u5rc76\" id=\"knV10BulkConfirm\"%s onclick=\"knV10BulkConfirm()\">",
               disabled ? " disabled" : "");
    sb_append_esc(sb, B.busy ? "\xe2\x80\xa6" : I18N_F(buf, "market.listMulti.confirm", "List {0}", num));
    sb_append(sb, "</button></div></div></div>");
}

// the run ended: the exchange's notification list (deob-10.0.5.js:133978-133982: success "market.listMulti.done"
// when any was listed, then each error as an error entry)
static void list_done(void *user, const int ok, const char *message) {
    (void) user;
    if (!ok) {
        if (B.kind == OV_LIST) fail(message);
        else v10_market_note(0, message ? message : I18N("generic.error", "Error"));
        return;
    }
    const KioBulkResult *r = inv_bulk_result();
    char a[24], text[160];
    snprintf(a, sizeof(a), "%d", r->listed);
    if (r->listed > 0) v10_market_note(1, I18N_F(text, "market.listMulti.done", "Listed {0} items", a));
    if (r->error[0]) v10_market_note(0, r->error);
    finish_ok();
}

// deob-10.0.4.js:131917-131945: the fee against the balance, then one row per stack (its quantity) or per copy
static void list_confirm(void) {
    int units;
    long fee;
    double value;
    list_totals(&units, &fee, &value);
    if (B.busy || list_invalid() || units == 0) return;
    if ((double) fee > funds()) {
        fail(I18N("market.listMulti.noFee", "You don't have enough KR for the listing fees."));
        return;
    }
    int n = 0;
    for (int i = 0; i < B.n_rows; i++) n += B.rows[i].stack_id ? (row_units(&B.rows[i]) > 0) : B.rows[i].n_copies;
    MarketListRow *rows = calloc((size_t) (n ? n : 1), sizeof(*rows));
    if (!rows) {
        fail(NULL);
        return;
    }
    int k = 0;
    for (int i = 0; i < B.n_rows; i++) {
        const ListRow *r = &B.rows[i];
        if (r->stack_id) {
            if (row_units(r) > 0) rows[k++] = (MarketListRow) {r->item, r->stack_id, r->price, row_units(r)};
        } else {
            for (int c = 0; c < r->n_copies; c++) rows[k++] = (MarketListRow) {r->item, r->copies[c], r->price, 1};
        }
    }
    B.busy = 1;
    B.error[0] = 0;
    render();
    if (!inv_bulk_list_rows(rows, k, list_done, NULL)) B.busy = 0;
    free(rows);
}

// deob-10.0.4.js:132190-132213: the picks grouped by item; a picked stack lists its count (the group's stack wins over
// its copies, :131927), picked copies list each
void v10_bulk_list_open(MarketPicks picks, KioInvDone after, void *user) {
    if (picks.n == 0) {
        market_picks_free(&picks);
        return;
    }
    if (B.kind != OV_NONE) v10_view_pop(0);
    int ng = 0;
    const MarketGroup *groups = inv_groups(&ng);
    B.rows = calloc((size_t) picks.n, sizeof(ListRow));
    for (int i = 0; B.rows && i < picks.n; i++) {
        const int item = picks.item_ids[i], sid = picks.skin_ids[i];
        ListRow *r = NULL;
        for (int k = 0; k < B.n_rows; k++)
            if (B.rows[k].item == item) r = &B.rows[k];
        if (!r) {
            r = &B.rows[B.n_rows++];
            r->item = item;
        }
        int stack_max = 0;
        for (int g = 0; groups && g < ng; g++)
            if (groups[g].item == item && groups[g].stack_id == sid) stack_max = groups[g].stack_max > 0 ? groups[g].stack_max : 1;
        if (stack_max) {
            r->stack_id = sid;
            r->max = stack_max;
            r->qty = stack_max; // IÎîîiíí: stackMax ?? 1
        } else {
            int *grown = realloc(r->copies, sizeof(int) * (size_t) (r->n_copies + 1));
            if (!grown) continue;
            r->copies = grown;
            r->copies[r->n_copies++] = sid;
        }
    }
    market_picks_free(&picks);
    if (!B.rows) return;
    push(OV_LIST, after, user);
}

// ---------------------------------------------------------------- Buy Multiple (svelte-dk2h1b)

static int buy_count(void) { // IÍíïïiï: the quantity clamped to 0..the ladder's length
    const int q = B.qty;
    return q < 0 ? 0 : q > B.n_prices ? B.n_prices : q;
}

static double buy_total(void) { return market_ladder_total(B.prices, B.n_prices, buy_count()); }

static void buy_markup(StrBuf *sb) {
    char buf[128], a[32], b[32];
    sb_append(sb, "<div class=\"bm-overlay svelte-dk2h1b\" role=\"presentation\" onclick=\"knV10BulkCancel()\">"
                  "<div class=\"bm-window svelte-dk2h1b\" role=\"dialog\" aria-modal=\"true\" aria-label=\"");
    sb_append_esc(sb, I18N("market.buyMulti.title", "Buy Multiple"));
    sb_append(sb, "\" onclick=\"event.stopPropagation()\"><div class=\"bm-title svelte-dk2h1b\">");
    sb_append_esc(sb, I18N("market.buyMulti.title", "Buy Multiple"));
    sb_appendf(sb, "</div> <div class=\"bm-item svelte-dk2h1b\" style=\"color:%s\">", rarity_color(B.item));
    sb_append_esc(sb, item_name(B.item));
    sb_append(sb, "</div> ");
    const int count = buy_count();
    const double total = buy_total(), balance = funds();
    if (B.loading) {
        sb_append(sb, "<div class=\"bm-note svelte-dk2h1b\">");
        sb_append_esc(sb, I18N("market.buyMulti.loading", "Loading listings..."));
        sb_append(sb, "</div> ");
    } else if (B.n_prices == 0) {
        sb_append(sb, "<div class=\"bm-note svelte-dk2h1b\">");
        sb_append_esc(sb, I18N("market.buyMulti.none", "No listings you can buy right now."));
        sb_append(sb, "</div> ");
    } else {
        fmt_int(a, sizeof(a), B.n_prices);
        sb_append(sb, "<div class=\"bm-available svelte-dk2h1b\">");
        sb_append_esc(sb, I18N_F(buf, "market.buyMulti.available", "{0} available", a));
        sb_append(sb, "</div> <div class=\"bm-qty-row svelte-dk2h1b\"><label class=\"bm-label svelte-dk2h1b\" for=\"bm-qty\">");
        sb_append_esc(sb, I18N("market.buyMulti.quantity", "Quantity"));
        sb_appendf(sb, "</label> <input id=\"bm-qty\" class=\"bm-qty svelte-dk2h1b\" type=\"number\" min=\"1\" max=\"%d\" value=\"%d\"%s onchange=\"knV10BulkBuyQty(this.value)\"> ",
                   B.n_prices, count, B.busy ? " disabled" : "");
        static const int chips[] = {10, 50, 100}; // IÏíîiïî (deob-10.0.4.js:132808), those under the ladder's length
        for (size_t i = 0; i < sizeof(chips) / sizeof(chips[0]); i++) {
            if (chips[i] >= B.n_prices) continue;
            sb_appendf(sb, "<button type=\"button\" class=\"bm-chip svelte-dk2h1b\"%s onclick=\"knV10BulkBuyQty(%d)\">%d</button> ", B.busy ? " disabled" : "",
                       chips[i], chips[i]);
        }
        sb_appendf(sb, "<button type=\"button\" class=\"bm-chip svelte-dk2h1b\"%s onclick=\"knV10BulkBuyQty(%d)\">", B.busy ? " disabled" : "", B.n_prices);
        sb_append_esc(sb, I18N("market.buyMulti.max", "Max"));
        sb_appendf(sb, "</button></div> <input class=\"bm-range svelte-dk2h1b\" type=\"range\" min=\"1\" max=\"%d\" value=\"%d\"%s onchange=\"knV10BulkBuyQty(this.value)\"> ",
                   B.n_prices, count, B.busy ? " disabled" : "");
        sb_append(sb, "<div class=\"bm-summary svelte-dk2h1b\"><div class=\"bm-row svelte-dk2h1b\"><span class=\"svelte-dk2h1b\">");
        sb_append_esc(sb, I18N("market.buyMulti.total", "Total"));
        fmt_int(a, sizeof(a), total);
        sb_appendf(sb, "</span> <span class=\"bm-total svelte-dk2h1b%s\">%s KR</span></div> <div class=\"bm-row bm-sub svelte-dk2h1b\"><span class=\"svelte-dk2h1b\">",
                   total > balance ? " bm-short" : "", a);
        sb_append_esc(sb, I18N("market.buyMulti.highest", "Highest price paid"));
        fmt_int(a, sizeof(a), count > 0 ? B.prices[count - 1] : 0); // IÍíiiïí: the last unit's price
        sb_appendf(sb, "</span> <span class=\"svelte-dk2h1b\">%s KR</span></div> <div class=\"bm-row bm-sub svelte-dk2h1b\"><span class=\"svelte-dk2h1b\">", a);
        sb_append_esc(sb, I18N("market.buyMulti.balance", "Your balance"));
        fmt_int(a, sizeof(a), balance);
        sb_appendf(sb, "</span> <span class=\"svelte-dk2h1b\">%s KR</span></div></div> <div class=\"bm-note svelte-dk2h1b\">", a);
        sb_append_esc(sb, I18N("market.buyMulti.cap", "Buys the cheapest listings. If prices change before your order goes through, nothing is bought."));
        sb_append(sb, "</div> ");
    }
    if (B.error[0]) {
        sb_append(sb, "<div class=\"bm-error svelte-dk2h1b\">");
        sb_append_esc(sb, B.error);
        sb_append(sb, "</div> ");
    }
    const int short_kr = total > balance;
    const int disabled = B.busy || B.loading || count < 1 || short_kr; // IÎîìíîí: total <= funds
    snprintf(a, sizeof(a), "%d", count);
    fmt_int(b, sizeof(b), total);
    sb_appendf(sb, "<div class=\"bm-actions svelte-dk2h1b\"><button type=\"button\" class=\"bm-btn bm-btn-cancel svelte-dk2h1b\"%s onclick=\"knV10BulkCancel()\">",
               B.busy ? " disabled" : "");
    sb_append_esc(sb, I18N("wallet.bulk.cancel", "Cancel"));
    sb_appendf(sb, "</button> <button type=\"button\" class=\"bm-btn bm-btn-buy svelte-dk2h1b\" id=\"knV10BulkConfirm\"%s onclick=\"knV10BulkConfirm()\">",
               disabled ? " disabled" : "");
    if (B.busy) sb_append(sb, "\xe2\x80\xa6");
    else if (short_kr && count > 0) sb_append_esc(sb, I18N("market.buyMulti.short", "Not enough KR"));
    else sb_append_esc(sb, I18N_F(buf, "market.buyMulti.buy", "Buy {0} for {1} KR", a, b));
    sb_append(sb, "</button></div></div></div>");
}

// GET market/items/:id/buy-ladder (getBuyLadder, deob-10.0.4.js:132838-132846): data.prices, cheapest first
static void ladder_answer(void *user, const int status, const cJSON *data, const char *code, const char *message) {
    (void) status;
    (void) code;
    if ((int) (intptr_t) user != B.gen || B.kind != OV_BUY) return;
    B.loading = 0;
    if (message) {
        snprintf(B.error, sizeof(B.error), "%s", message);
        render();
        return;
    }
    free(B.prices);
    B.prices = NULL;
    B.n_prices = market_ladder_parse(data, &B.prices);
    if (B.n_prices < 0) B.n_prices = 0;
    if (B.qty > B.n_prices) B.qty = B.n_prices; // :132849-132850
    if (B.qty < 1 && B.n_prices) B.qty = 1;
    render();
}

static void ladder_load(void) {
    char path[96];
    snprintf(path, sizeof(path), "market/items/%d/buy-ladder", B.item);
    B.loading = 1;
    if (!kio_store_write("GET", path, NULL, ladder_answer, (void *) (intptr_t) B.gen)) {
        B.loading = 0;
        snprintf(B.error, sizeof(B.error), "%s", "Failed to load listings");
    }
}

// POST market/buy-multiple {item_id, quantity, max_total} (deob-10.0.4.js:132755-132785): data {funds, bought, spent};
// a changed price or too few listings reload the ladder (:132783)
static void buy_answer(void *user, const int status, const cJSON *data, const char *code, const char *message) {
    (void) status;
    if ((int) (intptr_t) user != B.gen || B.kind != OV_BUY) return;
    if (message || code) {
        B.busy = 0;
        snprintf(B.error, sizeof(B.error), "%s", message && message[0] ? message : "Failed to buy items");
        if (code && (!strcmp(code, "MARKET_PRICE_CHANGED") || !strcmp(code, "MARKET_NOT_ENOUGH_LISTINGS"))) ladder_load();
        render();
        return;
    }
    const cJSON *f = cJSON_GetObjectItemCaseSensitive(data, "funds");
    if (cJSON_IsNumber(f)) account_kio_set_balances(1, f->valuedouble, 0, 0);
    const cJSON *bought = cJSON_GetObjectItemCaseSensitive(data, "bought"), *spent = cJSON_GetObjectItemCaseSensitive(data, "spent");
    char a[24], b[32], text[160];
    snprintf(a, sizeof(a), "%d", cJSON_IsNumber(bought) ? bought->valueint : 0);
    fmt_int(b, sizeof(b), cJSON_IsNumber(spent) ? spent->valuedouble : 0);
    v10_market_note(1, I18N_F(text, "market.buyMulti.done", "Bought {0} for {1} KR", a, b)); // deob-10.0.5.js:133996
    finish_ok();
}

static void buy_confirm(void) {
    const int count = buy_count();
    const double total = buy_total();
    if (B.busy || B.loading || count < 1 || total > funds()) return;
    cJSON *body = market_buy_multiple_body(B.item, count, total);
    char *text = body ? cJSON_PrintUnformatted(body) : NULL;
    cJSON_Delete(body);
    B.busy = 1;
    B.error[0] = 0;
    if (!text || !kio_store_write("POST", "market/buy-multiple", text, buy_answer, (void *) (intptr_t) B.gen)) {
        B.busy = 0;
        snprintf(B.error, sizeof(B.error), "%s", "Failed to buy items");
    }
    free(text);
    render();
}

void v10_bulk_buy_open(const int item, KioInvDone after, void *user) {
    if (B.kind != OV_NONE) v10_view_pop(0);
    B.item = item;
    B.qty = 1; // IÌiïîíî: 1
    push(OV_BUY, after, user);
    ladder_load();
    render();
}

// ---------------------------------------------------------------- the view

static char *markup(void) {
    StrBuf sb = {0};
    if (B.kind == OV_SELL) sell_markup(&sb);
    else if (B.kind == OV_LIST) list_markup(&sb);
    else if (B.kind == OV_BUY) buy_markup(&sb);
    return sb_finish(&sb);
}

int v10_bulk_open(void) { return B.kind != OV_NONE; }

void v10_bulk_scenario(const cJSON *state) {
    if (!cJSON_IsObject(state)) return;
    const char *kind = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(state, "kind"));
    if (!kind) return;
    if (!strcmp(kind, "buy")) {
        const cJSON *item = cJSON_GetObjectItemCaseSensitive(state, "item");
        v10_bulk_buy_open(cJSON_IsNumber(item) ? item->valueint : 0, NULL, NULL);
        B.loading = 0; // the scenario's ladder instead of the request's
        B.gen++;
        free(B.prices);
        B.prices = NULL;
        cJSON *wrap = cJSON_CreateObject();
        cJSON_AddItemToObject(wrap, "prices", cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(state, "prices"), 1));
        B.n_prices = market_ladder_parse(wrap, &B.prices);
        cJSON_Delete(wrap);
        if (B.n_prices < 0) B.n_prices = 0;
        const cJSON *qty = cJSON_GetObjectItemCaseSensitive(state, "qty");
        B.qty = cJSON_IsNumber(qty) ? qty->valueint : 1;
        render();
        return;
    }
    MarketPicks picks = {0};
    const cJSON *e;
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(state, "items")) {
        const int item = cJSON_GetArrayItem(e, 0) ? cJSON_GetArrayItem(e, 0)->valueint : 0, sid = cJSON_GetArrayItem(e, 1) ? cJSON_GetArrayItem(e, 1)->valueint : 0;
        market_picks_toggle(&picks, NULL, sid, item, 0);
    }
    if (!strcmp(kind, "sell")) {
        const cJSON *action = cJSON_GetObjectItemCaseSensitive(state, "action");
        v10_bulk_sell_open(cJSON_IsNumber(action) ? action->valueint : 0, picks, NULL, NULL);
    } else if (!strcmp(kind, "list")) {
        v10_bulk_list_open(picks, NULL, NULL);
        const cJSON *price = cJSON_GetObjectItemCaseSensitive(state, "price");
        for (int i = 0; cJSON_IsNumber(price) && i < B.n_rows; i++) B.rows[i].price = price->valueint;
        render();
    } else {
        market_picks_free(&picks);
    }
}

void v10_bulk_tick(void) {
    static int last;
    if (B.kind == OV_NONE || !B.busy || B.kind == OV_BUY) return;
    int done = 0, total = 0;
    inv_bulk_progress(&done, &total);
    if (done != last) {
        last = done;
        render();
    }
}

int v10_bulk_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) ev;
    if (!name || strncmp(name, "knV10Bulk", 9) != 0) return -1;
    if (B.kind == OV_NONE) return 1;
    if (!strcmp(name, "knV10BulkCancel")) {
        if (!B.busy && v10_view_depth() == B.depth) v10_view_pop(0);
        return 1;
    }
    if (!strcmp(name, "knV10BulkConfirm")) {
        if (B.kind == OV_SELL) sell_confirm();
        else if (B.kind == OV_LIST) list_confirm();
        else if (B.kind == OV_BUY) buy_confirm();
        return 1;
    }
    if (!strcmp(name, "knV10BulkQty") || !strcmp(name, "knV10BulkPrice")) {
        const int i = arg_int(args, argc, 0), v = arg_int(args, argc, 1);
        if (B.kind != OV_LIST || B.busy || i < 0 || i >= B.n_rows) return 1;
        if (!strcmp(name, "knV10BulkQty")) B.rows[i].qty = v < 0 ? 0 : v;
        else B.rows[i].price = v < 0 ? 0 : v;
        B.error[0] = 0;
        render();
        return 1;
    }
    if (!strcmp(name, "knV10BulkBuyQty")) {
        const int v = arg_int(args, argc, 0);
        if (B.kind != OV_BUY || B.busy) return 1;
        B.qty = v < 0 ? 0 : v;
        render();
        return 1;
    }
    return 1;
}
