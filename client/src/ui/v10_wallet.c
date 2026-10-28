// Krunker v10.0.2's Wallet and Inventory modal (_52368, v10:145475-145480; the template _52366, the
// stylesheet svelte-l0gwpc in ui/css/svelte.css). The header's Wallet item (v10's _52516, the
// backpack in #signedInHeaderBar, v10_menu.c) pushes it: a 1600 x 860 dialog on the view stack
// (_5235(...).push()) with
//   - the popup header (svelte-4fhlo6: the backpack icon, "Inventory", and the Catalog button in
//     its children slot), which calls openItems() after clearing the stack (_52307: _5227())
//   - the currency cards (_52362, the currencies grid): Krunkies (KR, #fbc02d), Junk (JNK, the
//     plumbing icon, #cc51f8, two decimals) and Ranked Points (RP, emoji_events, #4caf50). Their
//     taps do nothing in v10 (_52382 is empty)
//   - the owned skins (_52374: the account's inventory, data slot 10, each row {ind, cnt} is one
//     tile here): the market card at 128 px (_36396), a tap opens the item page (_35204 in v10),
//     the Quick Sell button top right (.quick-sell-icon, on hover in v10: always drawn here), the
//     spin ticket's Redeem top left (.spin-ticket-icon: prizeWheel(0, skin), as v10_daily_spin.c)
//     and the "×N" stack badge (.is-stack, the stack shadow); 60 tiles per page, then "Load More"
//     (v10's IntersectionObserver sentinel, _52410 / _52438, is a button on the market's grids too)
//   - the empty state (_52365)
// Escape and a click on the backdrop close it (the view stack's own handlers); the modal leaves the
// stack when the account changes (v10's $account effect, the Svelte component unmounting).
//
// The Quick Sell and the item page are the market's own views (v10_market.c
// v10_market_wallet_sell / v10_market_wallet_item): pushed over the wallet, with the exchange closed.
//
// Differences from v10, because KrunkNative's account has less than v10's:
//   - the owned list is the account's stacks: one tile per {ind, cnt} row (no per-copy instance
//     ids, no stackable flag, so no grouped stacks, no "in-group" collapse, no paint or style icons:
//     KrunkNative has no paint or style editor, and no instance ids to paint)
//   - the filter bar (_45793: search, rarity / weapon / type / wear / season filters, sort, the
//     "Group by wear" and paintable options) is not built; the grid keeps the account's order
//   - the item tap opens the Catalog's item page, not v10's item popup (_35204)
//   - Quick Sell is offered on every owned tile: the market's view says "Destroy Item" where the
//     item has no KR value (its inventory row decides, as the market's own grid)
#include <v10_wallet.h>
#include <v10_ui.h>
#include <kio_inventory.h>
#include <kio_store.h>
#include <market_bulk.h>
#include <backend_ui.h>
#include <i18n.h>
#include <html_ui.h>
#include "../viewer/viewer_store.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#define WALLET_PAGE 60 // v10's _52410: the first 60 tiles, then 60 more per scroll

// the view-stack modal's state (one wallet at a time)
static struct {
    int open;
    int depth;        // v10_view_depth() with the wallet on top: it is the top view while the depth is this
    int shown;        // the tiles drawn (WALLET_PAGE, then more on "Load More")
    int account_id;   // the account the modal was opened for (a sign-in / out closes it)
    int backend;
    // the account's figures when the modal was last drawn: redrawn when one changes
    int skins, copies, id;
    double kr, junk, rp;
} W;

// the krunker.io side (kio_inventory.h): the select-mode picks, the bulk sell in flight, the load's error text
static MarketPicks W_picks;
static int W_select;
static int W_busy;
static int W_price;   // the bulk list's unit price, as typed (0: none)
static char W_inv_err[160];

// ---------------------------------------------------------------- the account

static const AccountInfo *account(void) { return active_account(); }

// the owned stacks: data slot 10's rows whose skin exists (v10's skins.map(...).filter(Boolean))
typedef struct {
    int index;
    int count;
    const ViewerSkin *skin;
} WalletStack;

static int copies_of(const cJSON *e) {
    const cJSON *cnt = cJSON_GetObjectItem(e, "cnt");
    return cJSON_IsNumber(cnt) ? cnt->valueint : 1;
}

static int stacks_of(const AccountInfo *a, WalletStack *out, const int cap) {
    const cJSON *skins = a && a->data ? cJSON_GetArrayItem(a->data, 10) : NULL, *e;
    int n = 0;
    cJSON_ArrayForEach(e, skins) {
        const cJSON *ind = cJSON_GetObjectItem(e, "ind");
        if (!cJSON_IsNumber(ind)) continue;
        const ViewerSkin *k = viewer_skin(ind->valueint);
        if (!k || n >= cap) continue;
        out[n++] = (WalletStack) {ind->valueint, copies_of(e), k};
    }
    return n;
}

static int stack_total(const AccountInfo *a) {
    const cJSON *skins = a && a->data ? cJSON_GetArrayItem(a->data, 10) : NULL, *e;
    int n = 0;
    cJSON_ArrayForEach(e, skins) n += copies_of(e);
    return n;
}

static int stack_count(const AccountInfo *a) {
    const cJSON *skins = a && a->data ? cJSON_GetArrayItem(a->data, 10) : NULL;
    return cJSON_IsArray(skins) ? cJSON_GetArraySize(skins) : 0;
}

// the figures the modal shows: a change redraws it (the tick)
static int changed(const AccountInfo *a) {
    if (!a) return 1;
    return a->id != W.id || (int) account_backend() != W.backend || stack_count(a) != W.skins || stack_total(a) != W.copies
        || a->funds != W.kr || a->junk != W.junk || a->ranked_points != W.rp;
}

static void remember(const AccountInfo *a) {
    W.id = a ? a->id : 0;
    W.backend = (int) account_backend();
    W.skins = stack_count(a);
    W.copies = stack_total(a);
    W.kr = a ? a->funds : 0;
    W.junk = a ? a->junk : 0;
    W.rp = a ? a->ranked_points : 0;
}

// ---------------------------------------------------------------- markup

// v10's number formats: Number.toLocaleString('en-US') (v10_fmt_int); the junk card keeps two decimals
static void junk_text(char *out, const size_t n, const double v) {
    char fixed[64];
    snprintf(fixed, sizeof(fixed), "%.2f", fabs(v));
    char *dot = strchr(fixed, '.');
    if (dot) {
        *dot = 0;
        char frac[3] = {dot[1], dot[2], 0};
        if (frac[1] == '0') frac[1] = 0;
        if (frac[0] == '0' && !frac[1]) frac[0] = 0;
        char whole[32], grouped[48];
        v10_fmt_int(whole, sizeof(whole), (double) atoll(fixed));
        snprintf(grouped, sizeof(grouped), "%s%s%s%s", v < 0 ? "-" : "", whole, frac[0] ? "." : "", frac);
        snprintf(out, n, "%s", grouped);
        return;
    }
    v10_fmt_int(out, n, v);
}

static void card(StrBuf *sb, const char *id, const char *name, const char *color, const char *icon_text, const char *icon_mat, const char *amount, const char *symbol) {
    sb_appendf(sb, "<div class=\"currency-card svelte-l0gwpc\" id=\"%s\" role=\"button\" tabindex=\"0\"><div class=\"currency-icon svelte-l0gwpc\" style=\"color: %s\">",
               id, color);
    if (icon_text) sb_appendf(sb, "<span class=\"currency-text svelte-l0gwpc\">%s</span>", icon_text);
    else sb_appendf(sb, "<span class=\"material-icons svelte-l0gwpc\">%s</span>", icon_mat);
    sb_append(sb, "</div> <div class=\"currency-info svelte-l0gwpc\"><div class=\"currency-name svelte-l0gwpc\">");
    sb_append_esc(sb, name);
    sb_appendf(sb, "</div> <div class=\"currency-amount svelte-l0gwpc\" style=\"color: %s\">%s %s</div></div></div>", color, amount, symbol);
}

static void currencies(StrBuf *sb, const AccountInfo *a) {
    char kr[48], junk[48], rp[48];
    v10_fmt_int(kr, sizeof(kr), floor(a ? a->funds : 0));
    junk_text(junk, sizeof(junk), a ? a->junk : 0);
    v10_fmt_int(rp, sizeof(rp), floor(a ? a->ranked_points : 0));
    sb_append(sb, "<div class=\"currencies-grid svelte-l0gwpc\">");
    card(sb, "knV10WalletKR", I18N("kn.wallet.krunkies", "Krunkies"), "#fbc02d", "KR", NULL, kr, "KR");
    card(sb, "knV10WalletJunk", I18N("kn.wallet.junk", "Junk"), "#cc51f8", NULL, "plumbing", junk, "JNK");
    card(sb, "knV10WalletRP", I18N("kn.wallet.rankedPoints", "Ranked Points"), "#4caf50", NULL, "emoji_events", rp, "RP");
    sb_append(sb, "</div>");
}

// one owned stack: the market card, then v10's overlay buttons (.wallet-item's children)
static void tile(StrBuf *sb, const WalletStack *s) {
    char onclick[64];
    snprintf(onclick, sizeof(onclick), "knV10WalletItem(%d)", s->index);
    sb_appendf(sb, "<div class=\"wallet-item svelte-l0gwpc%s\">", s->count > 1 ? " is-stack" : "");
    v10_market_card(sb, s->index, 128, 128, "top-center", onclick, NULL);
    if (s->skin->type == 13) { // a spin ticket: Redeem (v10's spin-ticket-icon, _52335)
        char label[160];
        snprintf(label, sizeof(label), "%s %s", I18N("kn.wallet.redeem", "Redeem"), s->skin->name ? s->skin->name : "");
        sb_appendf(sb, "<button type=\"button\" class=\"spin-ticket-icon svelte-l0gwpc\" aria-label=\"");
        sb_append_esc(sb, label);
        sb_appendf(sb, "\" onclick=\"playSelect(0.1),knV10WalletRedeem(%d)\"><i class=\"material-icons svelte-l0gwpc\">casino</i></button>", s->index);
    }
    {
        char label[160];
        snprintf(label, sizeof(label), "%s %s", I18N("kn.wallet.quickSell", "Quick sell"), s->skin->name ? s->skin->name : "");
        sb_append(sb, "<button type=\"button\" class=\"quick-sell-icon svelte-l0gwpc\" aria-label=\"");
        sb_append_esc(sb, label);
        sb_appendf(sb, "\" onclick=\"playSelect(0.1),knV10WalletSell(%d)\"><i class=\"material-icons svelte-l0gwpc\">sell</i></button>", s->index);
    }
    if (s->count > 1) sb_appendf(sb, "<div class=\"stack-badge svelte-l0gwpc\" aria-hidden=\"true\">\xc3\x97%d</div>", s->count);
    sb_append(sb, "</div>");
}

// ---------------------------------------------------------------- the krunker.io inventory (kio_inventory.h)

static int picked_row(const MarketGroup *g, const int copy) {
    const int skin = copy < 0 ? g->stack_id : g->instances[copy];
    for (int i = 0; i < W_picks.n; i++)
        if (W_picks.skin_ids[i] == skin && W_picks.item_ids[i] == g->item) return 1;
    return 0;
}

// one row of the official inventory: a stack (copy -1, with its count) or one copy. Its lock and, in select
// mode, its pick name the official ids (gi = the group, copy = the copy or -1 for the stack).
static void official_tile(StrBuf *sb, const MarketGroup *groups, const int gi, const int copy) {
    const MarketGroup *g = &groups[gi];
    const int stack = copy < 0;
    const int locked = kio_inventory_locked(g, copy);
    const int on = W_select && picked_row(g, copy);
    char onclick[64];
    snprintf(onclick, sizeof(onclick), "knV10WalletItem(%d)", g->item);
    sb_appendf(sb, "<div class=\"wallet-item svelte-l0gwpc%s%s\">", stack && g->stack_max > 1 ? " is-stack" : "", locked ? " is-locked" : "");
    v10_market_card(sb, g->item, 128, 128, "top-center", onclick, NULL);
    if (W_select)
        sb_appendf(sb, "<button type=\"button\" class=\"wallet-pick%s\" onclick=\"knV10WalletPick(%d,%d)\"><i class=\"material-icons\">%s</i></button>",
                   on ? " is-on" : "", gi, copy, on ? "check_box" : "check_box_outline_blank");
    sb_appendf(sb, "<button type=\"button\" class=\"wallet-lock\" onclick=\"knV10WalletLock(%d,%d)\" aria-label=\"", gi, copy);
    sb_append_esc(sb, locked ? I18N("kn.inv.unlock", "Unlock") : I18N("kn.inv.lock", "Lock"));
    sb_appendf(sb, "\"><i class=\"material-icons\">%s</i></button>", locked ? "lock" : "lock_open");
    if (stack && g->stack_max > 1) sb_appendf(sb, "<div class=\"stack-badge svelte-l0gwpc\" aria-hidden=\"true\">\xc3\x97%d</div>", g->stack_max);
    sb_append(sb, "</div>");
}

// the select bar: Select / Cancel, and while selecting the count and the bulk Quick Sell
static void official_bar(StrBuf *sb) {
    sb_append(sb, "<div class=\"wallet-bulkbar svelte-l0gwpc\">");
    sb_append(sb, "<button type=\"button\" class=\"wallet-select-btn svelte-l0gwpc\" onclick=\"knV10WalletSelect()\">");
    sb_append_esc(sb, W_select ? I18N("kn.inv.cancel", "Cancel") : I18N("kn.inv.select", "Select"));
    sb_append(sb, "</button>");
    if (W_select) {
        sb_appendf(sb, "<span class=\"wallet-count svelte-l0gwpc\">%d</span>", W_picks.n);
        sb_appendf(sb, "<button type=\"button\" class=\"wallet-select-btn svelte-l0gwpc\"%s onclick=\"knV10WalletBulkSell()\">", W_busy ? " disabled" : "");
        sb_append_esc(sb, I18N("kn.inv.quickSellSelected", "Quick sell selected"));
        sb_append(sb, "</button>");
        sb_appendf(sb, "<input type=\"number\" min=\"1\" class=\"wallet-price svelte-l0gwpc\" aria-label=\"");
        sb_append_esc(sb, I18N("kn.inv.price", "Price per item"));
        sb_appendf(sb, "\" value=\"%d\" onchange=\"knV10WalletPrice(this.value)\">", W_price > 0 ? W_price : 0);
        sb_appendf(sb, "<button type=\"button\" class=\"wallet-select-btn svelte-l0gwpc\"%s onclick=\"knV10WalletBulkList()\">", W_busy ? " disabled" : "");
        sb_append_esc(sb, I18N("kn.inv.listSelected", "List selected"));
        sb_append(sb, "</button>");
    }
    sb_append(sb, "</div>");
}

static void official_grid(StrBuf *sb) {
    int n = 0;
    const MarketGroup *groups = kio_inventory_groups(&n);
    official_bar(sb);
    sb_append(sb, "<div class=\"items-grid svelte-l0gwpc\" id=\"knV10WalletGrid\">");
    int drawn = 0;
    for (int gi = 0; groups && gi < n; gi++) {
        if (groups[gi].stack_id) {
            if (drawn < W.shown) official_tile(sb, groups, gi, -1);
            drawn++;
        }
        for (int c = 0; c < groups[gi].n_instances; c++) {
            if (drawn < W.shown) official_tile(sb, groups, gi, c);
            drawn++;
        }
    }
    if (drawn > W.shown) {
        sb_append(sb, "<div class=\"wallet-items-sentinel svelte-l0gwpc\"><button type=\"button\" class=\"load-more-btn svelte-zhgogl\" id=\"knV10WalletMore\" onclick=\"knV10WalletMore()\">");
        sb_append_esc(sb, I18N("kn.wallet.loadMore", "Load More"));
        sb_append(sb, "</button></div>");
    }
    if (!drawn) {
        sb_append(sb, "<div class=\"empty-state svelte-l0gwpc\" id=\"knV10WalletEmpty\"><p class=\"svelte-l0gwpc\">");
        if (kio_inventory_ready()) sb_append_esc(sb, I18N("kn.wallet.empty", "No items match your current filters."));
        else sb_append_esc(sb, W_inv_err[0] ? W_inv_err : kio_inventory_gate_text());
        sb_append(sb, "</p></div>");
    }
    sb_append(sb, "</div>");
}

static void grid(StrBuf *sb, const AccountInfo *a) {
    if (kio_store_active()) {
        official_grid(sb);
        return;
    }
    const int cap = stack_count(a) > 0 ? stack_count(a) : 1;
    WalletStack *list = calloc((size_t) cap, sizeof(*list));
    const int n = list ? stacks_of(a, list, cap) : 0;
    sb_append(sb, "<div class=\"items-grid svelte-l0gwpc\" id=\"knV10WalletGrid\">");
    for (int i = 0; i < n && i < W.shown; i++) tile(sb, &list[i]);
    if (n > W.shown) {
        sb_append(sb, "<div class=\"wallet-items-sentinel svelte-l0gwpc\"><button type=\"button\" class=\"load-more-btn svelte-zhgogl\" id=\"knV10WalletMore\" onclick=\"knV10WalletMore()\">");
        sb_append_esc(sb, I18N("kn.wallet.loadMore", "Load More"));
        sb_append(sb, "</button></div>");
    }
    if (!n) {
        sb_append(sb, "<div class=\"empty-state svelte-l0gwpc\" id=\"knV10WalletEmpty\"><p class=\"svelte-l0gwpc\">");
        sb_append_esc(sb, I18N("kn.wallet.empty", "No items match your current filters."));
        sb_append(sb, "</p></div>");
    }
    sb_append(sb, "</div>");
    free(list);
}

// the whole modal (.wallet-root) for the active account
static char *markup(void) {
    const AccountInfo *a = account();
    StrBuf sb = {0};
    sb_append(&sb, "<div class=\"wallet-root svelte-l0gwpc\" id=\"knV10WalletRoot\" role=\"dialog\" aria-label=\"");
    sb_append_esc(&sb, I18N("kn.wallet.dialog", "Wallet and Inventory"));
    sb_append(&sb, "\">");
    sb_append(&sb, "<div class=\"window-header svelte-4fhlo6\"><i class=\"material-icons window-header-icon svelte-4fhlo6\">backpack</i> <span class=\"window-header-title svelte-4fhlo6\">");
    sb_append_esc(&sb, I18N("kn.wallet.title", "Inventory"));
    sb_append(&sb, "</span> <button class=\"wallet-catalog-btn svelte-l0gwpc\" type=\"button\" id=\"knV10WalletCatalogBtn\" onclick=\"playSelect(0.1),knV10WalletCatalog()\">"
                   "<i class=\"material-icons svelte-l0gwpc\">inventory_2</i> <span class=\"svelte-l0gwpc\">");
    sb_append_esc(&sb, I18N("kn.wallet.catalog", "Catalog"));
    sb_append(&sb, "</span></button></div>");
    sb_append(&sb, "<div class=\"wallet-body svelte-l0gwpc\">");
    currencies(&sb, a);
    grid(&sb, a);
    sb_append(&sb, "</div></div>");
    return sb_finish(&sb);
}

// ---------------------------------------------------------------- the view

static void closed(int accepted, void *user);

// redraw the modal: the top view's markup when it is on top, else the page's copy in place (the
// Quick Sell or item page over it keeps its own markup)
static void refresh(void) {
    if (!W.open) return;
    char *html = markup();
    remember(account());
    if (v10_view_depth() == W.depth) {
        v10_view_set(html);
        return;
    }
    const HtmlElem old = html_dom_by_id("knV10WalletRoot");
    if (old) {
        html_dom_insert_html(old, HTML_INSERT_BEFORE_BEGIN, html);
        html_dom_remove(old);
    }
    free(html);
}

// the callbacks of the krunker.io writes and the load: the modal redraws when they end
static void inventory_loaded(void *user, const int ok, const char *message) {
    (void) user;
    W_inv_err[0] = 0;
    if (!ok && message) snprintf(W_inv_err, sizeof(W_inv_err), "%s", message);
    refresh();
}

static void lock_done(void *user, const int ok, const char *message) {
    (void) user;
    if (!ok) v10_view_push(v10_io_error(I18N("kn.inv.lockFailedTitle", "Lock"), message ? message : I18N("generic.error", "Error")), NULL, NULL, NULL);
    refresh();
}

static void bulk_done(void *user, const int ok, const char *message) {
    (void) user;
    W_busy = 0;
    if (!ok && message) v10_view_push(v10_io_error(I18N("kn.inv.quickSellFailedTitle", "Quick sell"), message), NULL, NULL, NULL);
    refresh();
}

static void open_modal(void) {
    const AccountInfo *a = account();
    if (!a) { // v10 shows the Wallet to an account only: a guest gets the login modal
        html_ui_dispatch("loginOrRegister", NULL, NULL, 0);
        return;
    }
    if (W.open) return;
    W.open = 1;
    W.shown = WALLET_PAGE;
    W.account_id = a->id;
    W.backend = (int) account_backend();
    remember(a);
    v10_view_push(markup(), refresh, closed, NULL);
    W.depth = v10_view_depth();
    if (kio_store_active() && !kio_inventory_ready() && !kio_inventory_loading()) kio_inventory_load(inventory_loaded, NULL);
}

// the modal left the stack (Escape, a backdrop tap, Catalog, an account change): its market views go too
static void closed(const int accepted, void *user) {
    (void) accepted;
    (void) user;
    W.open = 0;
    W.depth = 0;
    W_select = 0;
    market_picks_free(&W_picks);
    v10_market_wallet_release();
}

// ---------------------------------------------------------------- handlers

static int arg_int(const HtmlArg *args, const int argc, const int i) {
    if (!args || i >= argc) return -1;
    if (args[i].kind == HTML_ARG_NUM || args[i].kind == HTML_ARG_BOOL) return (int) args[i].num;
    return args[i].str ? atoi(args[i].str) : -1;
}

int v10_wallet_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) ev;
    if (!name) return -1;
    if (!strcmp(name, "knV10Wallet")) { // the header's Wallet item (_52516)
        open_modal();
        return 1;
    }
    if (strncmp(name, "knV10Wallet", 11) != 0) return -1;
    if (!W.open) return 1;
    if (!strcmp(name, "knV10WalletCatalog")) { // Catalog: _5227() then openItems() (_52307)
        v10_view_clear();
        html_ui_dispatch("openItems", NULL, NULL, 0);
        return 1;
    }
    if (!strcmp(name, "knV10WalletItem")) {
        v10_market_wallet_item(arg_int(args, argc, 0));
        return 1;
    }
    if (!strcmp(name, "knV10WalletSell")) {
        {
            const int skin = arg_int(args, argc, 0); // a stack tile sells its item (sid = index); a copy passes its own id
            v10_market_wallet_sell(skin, argc > 1 ? arg_int(args, argc, 1) : skin);
        }
        return 1;
    }
    if (!strcmp(name, "knV10WalletRedeem")) { // window.prizeWheel(0, skin) (v10_daily_spin.c's call)
        const int skin = arg_int(args, argc, 0);
        const HtmlArg pw[2] = {{.kind = HTML_ARG_NUM, .num = 0}, {.kind = HTML_ARG_NUM, .num = skin}};
        html_ui_dispatch("prizeWheel", NULL, pw, 2);
        return 1;
    }
    if (!strcmp(name, "knV10WalletSelect")) {
        W_select = !W_select;
        if (!W_select) market_picks_free(&W_picks);
        refresh();
        return 1;
    }
    if (!strcmp(name, "knV10WalletPick") || !strcmp(name, "knV10WalletLock")) {
        int n = 0;
        const MarketGroup *groups = kio_inventory_groups(&n);
        const int gi = arg_int(args, argc, 0), copy = arg_int(args, argc, 1);
        if (!groups || gi < 0 || gi >= n || copy >= groups[gi].n_instances) return 1;
        const MarketGroup *g = &groups[gi];
        if (!strcmp(name, "knV10WalletPick")) {
            const int skin = copy < 0 ? g->stack_id : g->instances[copy];
            market_picks_toggle(&W_picks, kio_inventory_locks(), skin, g->item, copy < 0);
            refresh();
            return 1;
        }
        kio_inventory_set_lock(g, copy, !kio_inventory_locked(g, copy), lock_done, NULL);
        return 1;
    }
    if (!strcmp(name, "knV10WalletBulkSell")) {
        if (W_busy) return 1;
        W_busy = 1;
        const MarketPicks picks = W_picks; // the run owns the picks from here
        memset(&W_picks, 0, sizeof(W_picks));
        W_select = 0;
        kio_inventory_bulk_sell(0, picks, bulk_done, NULL); // 0: Get KR, the single quick-sell's first mode
        refresh();
        return 1;
    }
    if (!strcmp(name, "knV10WalletPrice")) {
        W_price = arg_int(args, argc, 0);
        return 1;
    }
    if (!strcmp(name, "knV10WalletBulkList")) {
        if (W_busy) return 1;
        if (W_price < 1) {
            v10_view_push(v10_io_error(I18N("kn.inv.listFailedTitle", "List"), I18N("kn.inv.priceNeeded", "Enter a price per item first.")), NULL, NULL, NULL);
            return 1;
        }
        W_busy = 1;
        const MarketPicks picks = W_picks; // the run owns the picks from here
        memset(&W_picks, 0, sizeof(W_picks));
        W_select = 0;
        kio_inventory_bulk_list(W_price, picks, bulk_done, NULL);
        refresh();
        return 1;
    }
    if (!strcmp(name, "knV10WalletMore")) {
        W.shown += WALLET_PAGE;
        refresh();
        return 1;
    }
    return -1;
}

// Escape: the top view goes (a Quick Sell or item page over the wallet first, else the wallet)
int v10_wallet_key(const int key) {
    if (!W.open || !html_key_is_escape(key)) return 0;
    v10_view_pop(0);
    return 1;
}

// per menu frame: the figures moved (a sale, a purchase, funds) redraw the modal
void v10_wallet_tick(void) {
    if (!W.open) return;
    const AccountInfo *a = account();
    if (!a || a->id != W.account_id) {
        v10_view_clear(); // the account is gone: v10's $account effect
        return;
    }
    if (changed(a)) refresh();
}

void v10_wallet_account_changed(void) {
    kio_inventory_clear();
    market_picks_free(&W_picks);
    W_select = 0;
    W_busy = 0;
    W_inv_err[0] = 0;
    if (!W.open) return;
    const AccountInfo *a = account();
    if (!a || a->id != W.account_id || (int) account_backend() != W.backend) v10_view_clear();
}

// --render-scenario state.wallet: {open: true} opens the modal over the menu (the account's
// stand-in comes from state.account: its skins, funds, junk and rankedPoints)
void v10_wallet_scenario(const cJSON *state) {
    if (!cJSON_IsObject(state) || !cJSON_IsTrue(cJSON_GetObjectItem(state, "open"))) return;
    if (!account()) return;
    open_modal();
}
