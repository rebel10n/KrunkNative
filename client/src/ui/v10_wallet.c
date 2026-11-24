// Krunker v10.0.2's Wallet and Inventory modal (_52368, v10:145475-145480; the template _52366, the
// stylesheet in ui/css/svelte.css, named svelte-l0gwpc in 10.0.2 and svelte-1wcm5z8 since v10.0.7, deob-10.0.7.js:147378-147412,
// whose bulk-select / lock rules live in ui/css/main.css). The header's Wallet item (v10's _52516, the
// backpack in #signedInHeaderBar, v10_menu.c) pushes it: a 1600 x 860 dialog on the view stack
// (_5235(...).push()) with
//   - the popup header (svelte-4fhlo6: the backpack icon, "Inventory", and the Catalog button in
//     its children slot), which calls openItems() after clearing the stack (_52307: _5227())
//   - the currency cards (_52362, the currencies grid): Krunkies (KR, #fbc02d), Junk (JNK, the
//     plumbing icon, #cc51f8, two decimals) and Ranked Points (RP, emoji_events, #4caf50). Their
//     taps do nothing in v10 (_52382 is empty)
//   - the owned skins (the account's inventory): one tile per stack and one per copy (instance id),
//     the market card at 128 px. A tap opens viewItem. Not in select mode, a tile also has the lock
//     (.lock-toggle, top right under Quick Sell), then exactly one of Paint (paintTargets and an
//     instance), Style (styles and an instance, the paint button's else) or Redeem (a type-13 ticket
//     with spinWheel or spinWheelId and a count), and Quick Sell when the item can be sold or
//     destroyed and the unit is not locked (deob-10.0.7.js:169669-169765). The "×N" stack badge
//     (.is-stack) and 60 tiles per page, then "Load More" (v10's IntersectionObserver sentinel is a
//     button here, as on the market's grids)
//   - the empty state
// Escape and a click on the backdrop close it (the view stack's own handlers); the modal leaves the
// stack when the account changes (v10's $account effect, the Svelte component unmounting).
//
// Quick Sell is the market's own view (v10_market.c v10_market_wallet_sell): a stack sells by its
// store index, a copy by its instance id. Paint and Style open the same pickers as the market grid.
//
// Not built: the filter bar (search, rarity / weapon / type / wear / season, sort, "Group by wear")
// and the expanded-stack collapse (in-group / stack-collapse-fab). The grid keeps inventory order.
#include <v10_wallet.h>
#include <v10_ui.h>
#include <kio_inventory.h>
#include <kio_store.h>
#include <market_bulk.h>
#include <v10_bulk.h> // the bulk quick-sell confirm (deob-10.0.4.js:146908-147037)
#include <paint_picker.h> // a copy's Paint button (paintTargets && instanceId, deob-10.0.7.js:169713-169722)
#include <style_picker.h> // a copy's Style button, the paint button's else (styles && instanceId, :169725-169733)
#include <inv_source.h> // the active account's inventory: krunker.io's (kio_inventory) or Box Office's (bo_inventory)
#include <backend_ui.h>
#include <i18n.h>
#include <html_ui.h>
#include "../viewer/viewer_store.h"
#include <stdint.h> // intptr_t: a note's id as the timer's user
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

// the inventory's notification list (v10_wallet_note): v10's notifications store of the inventory, each removed
// 4 s (0xfa0) after it was added (deob-10.0.5.js:147225-147236)
#define WALLET_NOTE_MS 4000.0
#define WALLET_NOTE_MAX 8
static struct {
    int id, ok;
    char msg[200];
} W_notes[WALLET_NOTE_MAX];
static int W_nnotes, W_note_seq;

// the krunker.io side (kio_inventory.h): the select-mode picks, the bulk sell in flight, the load's error text
static MarketPicks W_picks;
static int W_select;
static int W_busy;
static char W_inv_err[160];

// ---------------------------------------------------------------- the account

static const AccountInfo *account(void) { return active_account(); }

static int copies_of(const cJSON *e) {
    const cJSON *cnt = cJSON_GetObjectItem(e, "cnt");
    return cJSON_IsNumber(cnt) ? cnt->valueint : 1;
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
    sb_appendf(sb, "<div class=\"currency-card svelte-1wcm5z8\" id=\"%s\" role=\"button\" tabindex=\"0\"><div class=\"currency-icon svelte-1wcm5z8\" style=\"color: %s\">",
               id, color);
    if (icon_text) sb_appendf(sb, "<span class=\"currency-text svelte-1wcm5z8\">%s</span>", icon_text);
    else sb_appendf(sb, "<span class=\"material-icons svelte-1wcm5z8\">%s</span>", icon_mat);
    sb_append(sb, "</div> <div class=\"currency-info svelte-1wcm5z8\"><div class=\"currency-name svelte-1wcm5z8\">");
    sb_append_esc(sb, name);
    sb_appendf(sb, "</div> <div class=\"currency-amount svelte-1wcm5z8\" style=\"color: %s\">%s %s</div></div></div>", color, amount, symbol);
}

static void currencies(StrBuf *sb, const AccountInfo *a) {
    char kr[48], junk[48], rp[48];
    v10_fmt_int(kr, sizeof(kr), floor(a ? a->funds : 0));
    junk_text(junk, sizeof(junk), a ? a->junk : 0);
    v10_fmt_int(rp, sizeof(rp), floor(a ? a->ranked_points : 0));
    sb_append(sb, "<div class=\"currencies-grid svelte-1wcm5z8\">");
    card(sb, "knV10WalletKR", I18N("kn.wallet.krunkies", "Krunkies"), "#fbc02d", "KR", NULL, kr, "KR");
    card(sb, "knV10WalletJunk", I18N("kn.wallet.junk", "Junk"), "#cc51f8", NULL, "plumbing", junk, "JNK");
    card(sb, "knV10WalletRP", I18N("kn.wallet.rankedPoints", "Ranked Points"), "#4caf50", NULL, "emoji_events", rp, "RP");
    sb_append(sb, "</div>");
}

// ---------------------------------------------------------------- the inventory (kio_inventory.h / bo_inventory.h)

// ---- the Keep options (iíiiììi, deob-10.0.4.js:108967-108975; localStorage "bulkSellKeep", :147278-147287, :147309-147313)

static MarketKeep W_keep;
static int W_keep_loaded, W_keep_open;

static MarketKeep *keep_opts(void) {
    if (!W_keep_loaded) {
        char buf[256];
        W_keep = market_keep_parse(html_storage_get("bulkSellKeep", buf, sizeof(buf)) >= 0 ? buf : NULL);
        W_keep_loaded = 1;
    }
    return &W_keep;
}

static void keep_save(void) {
    char *json = market_keep_json(keep_opts());
    if (json) html_storage_set("bulkSellKeep", json);
    free(json);
}

// IÌïîîîï (deob-10.0.4.js:147289-147306) for one row: a krunker.io stackable row is "stack" (the bundle bulk-sells
// copies only). A Box Office stack is N plain copies (bo_inventory.c), so it is pickable: its reasons are the item's
// (locked, equipped, rarity). Listed units are out of the inventory (the brain's escrow; krunker.io's rows), so none here.
static int row_reason(const MarketGroup *g, const int copy) {
    const MarketQsValue v = market_qs_value(ui_loadout_skin_json(g->item));
    const ViewerSkin *k = viewer_skin(g->item);
    return market_keep_reason(keep_opts(), copy < 0 && inv_official(), v.can_qs || v.can_destroy, 0, inv_locked(g, copy),
                              ui_loadout_wears(g->item), k ? k->rarity : 0, copy >= 0 && g->metas ? &g->metas[copy] : NULL);
}

// IÍïiïïí: a reason that keeps the unit for its own sake (not stack / unsellable): the per-item count's slots go to these first
static int kept_reason(const int r) { return r != MARKET_KEEP_NONE && r != MARKET_KEEP_STACK && r != MARKET_KEEP_UNSELLABLE; }

static int picked_row(const MarketGroup *g, const int copy) {
    const int skin = copy < 0 ? g->stack_id : g->instances[copy];
    for (int i = 0; i < W_picks.n; i++)
        if (W_picks.skin_ids[i] == skin && W_picks.item_ids[i] == g->item) return 1;
    return 0;
}

// spinWheel is truthy, or spinWheelId is present (0 counts): v10's Redeem gate (deob-10.0.7.js:169737-169738)
static int ticket_has_wheel(const int index) {
    const cJSON *j = ui_loadout_skin_json(index);
    const cJSON *wheel = cJSON_GetObjectItemCaseSensitive(j, "spinWheel");
    if (wheel && !cJSON_IsNull(wheel) && !cJSON_IsFalse(wheel) && !(cJSON_IsNumber(wheel) && wheel->valuedouble == 0.0) &&
        !(cJSON_IsString(wheel) && !wheel->valuestring[0]))
        return 1;
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(j, "spinWheelId");
    return id && !cJSON_IsNull(id);
}

static void aria_icon(StrBuf *sb, const char *cls, const char *label, const char *onclick, const char *icon) {
    sb_appendf(sb, "<button type=\"button\" class=\"%s svelte-1wcm5z8\" aria-label=\"", cls);
    sb_append_esc(sb, label);
    sb_appendf(sb, "\" onclick=\"%s\"><i class=\"material-icons svelte-1wcm5z8\">%s</i></button>", onclick, icon);
}

// Not in select mode (deob-10.0.7.js:169669-169765): the lock, then Paint or else Style or else Redeem, and Quick Sell
// when the unit can be sold or destroyed and is not locked. A stack has no instance, so no Paint or Style.
static void wallet_actions(StrBuf *sb, const MarketGroup *g, const int gi, const int copy, const int locked) {
    const ViewerSkin *k = viewer_skin(g->item);
    const int instance = copy >= 0 ? g->instances[copy] : 0;
    const int count = copy < 0 ? g->stack_max : 1;
    const char *name = k && k->name ? k->name : "";
    char label[160], click[96];
    sb_appendf(sb, "<button type=\"button\" class=\"lock-toggle svelte-1wcm5z8%s\" onclick=\"knV10WalletLock(%d,%d)\" aria-label=\"",
               locked ? " is-locked" : "", gi, copy);
    sb_append_esc(sb, locked ? I18N("kn.inv.unlock", "Unlock") : I18N("kn.inv.lock", "Lock"));
    sb_appendf(sb, "\"><i class=\"material-icons svelte-1wcm5z8\">%s</i></button>", locked ? "lock" : "lock_open");
    if (instance && paint_picker_can_open(g->item)) {
        snprintf(label, sizeof(label), "Paint %s", name);
        snprintf(click, sizeof(click), "playSelect(0.1),paintOpen(%d,%d)", g->item, instance);
        aria_icon(sb, "paint-icon", label, click, "format_paint");
    } else if (instance && style_picker_can_open(g->item)) {
        snprintf(label, sizeof(label), "Style %s", name);
        snprintf(click, sizeof(click), "playSelect(0.1),styleOpen(%d,%d)", g->item, instance);
        aria_icon(sb, "paint-icon", label, click, "checkroom"); // v10's style control reuses .paint-icon
    } else if (k && k->type == 13 && count > 0 && ticket_has_wheel(g->item)) {
        snprintf(label, sizeof(label), "%s %s", I18N("kn.wallet.redeem", "Redeem"), name);
        snprintf(click, sizeof(click), "playSelect(0.1),knV10WalletRedeem(%d)", g->item);
        aria_icon(sb, "spin-ticket-icon", label, click, "casino");
    }
    const MarketQsValue sell = market_qs_value(ui_loadout_skin_json(g->item));
    if (!locked && (sell.can_qs || sell.can_destroy)) {
        snprintf(label, sizeof(label), "%s %s", I18N("kn.wallet.quickSell", "Quick sell"), name);
        if (instance) snprintf(click, sizeof(click), "playSelect(0.1),knV10WalletSell(%d,%d)", g->item, instance);
        else snprintf(click, sizeof(click), "playSelect(0.1),knV10WalletSell(%d)", g->item);
        aria_icon(sb, "quick-sell-icon", label, click, "sell");
    }
}

// one row of the inventory: a stack (copy -1, with its count) or one copy. Its lock and, in select
// mode, its pick name the ids (gi = the group, copy = the copy or -1 for the stack).
static void official_tile(StrBuf *sb, const MarketGroup *groups, const int gi, const int copy) {
    const MarketGroup *g = &groups[gi];
    const int stack = copy < 0;
    const int locked = inv_locked(g, copy);
    const int on = W_select && picked_row(g, copy);
    const int reason = W_select ? row_reason(g, copy) : MARKET_KEEP_NONE;
    char onclick[64];
    snprintf(onclick, sizeof(onclick), "knV10WalletItem(%d)", g->item);
    sb_appendf(sb, "<div class=\"wallet-item svelte-1wcm5z8%s%s%s%s\"", stack && g->stack_max > 1 ? " is-stack" : "", locked ? " is-locked" : "",
               on ? " bulk-selected svelte-1wcm5z8" : "", reason != MARKET_KEEP_NONE ? " bulk-dim svelte-1wcm5z8" : "");
    if (market_keep_reason_name(reason)) { // its keep reason on hover (wallet.bulk.keep.reason.*, deob-10.0.4.js:147624)
        char key[64];
        snprintf(key, sizeof(key), "wallet.bulk.keep.reason.%s", market_keep_reason_name(reason));
        sb_append(sb, " title=\"");
        sb_append_esc(sb, i18n_t(key));
        sb_append(sb, "\"");
    }
    sb_append(sb, ">");
    v10_market_card(sb, g->item, 128, 128, "top-center", onclick, NULL);
    if (!W_select) wallet_actions(sb, g, gi, copy, locked);
    else if (reason == MARKET_KEEP_NONE || on)
        sb_appendf(sb, "<button type=\"button\" class=\"wallet-pick%s\" onclick=\"knV10WalletPick(%d,%d)\"><i class=\"material-icons\">%s</i></button>",
                   on ? " is-on" : "", gi, copy, on ? "check_box" : "check_box_outline_blank");
    if (stack && g->stack_max > 1) sb_appendf(sb, "<div class=\"stack-badge svelte-1wcm5z8\" aria-hidden=\"true\">\xc3\x97%d</div>", g->stack_max);
    sb_append(sb, "</div>");
}

// the select toggle (wallet.bulk.select / done, deob-10.0.4.js:147549) over the grid
static void official_toolbar(StrBuf *sb) {
    sb_append(sb, "<div class=\"wallet-bulkbar svelte-1wcm5z8\">");
    sb_appendf(sb, "<button type=\"button\" class=\"bulk-btn svelte-1wcm5z8%s\" onclick=\"knV10WalletSelect()\">", W_select ? " active" : "");
    sb_append_esc(sb, W_select ? I18N("wallet.bulk.done", "Done") : I18N("wallet.bulk.select", "Select"));
    sb_append(sb, "</button></div>");
}

// iíiîìïí (deob-10.0.4.js:24597-24606, the tiers :16481-16497): the wear's name, e.g. "Flawless"
static const char *wear_name(const double wear) {
    static const double max[] = {0.1, 0.3, 0.6, 1};
    static const char *const keys[] = {"wear_flawless", "wear_tested", "wear_trashed", "wear_wrecked"};
    for (int i = 0; i < 4; i++)
        if (wear < max[i]) return i18n_t(keys[i]);
    return i18n_t(keys[3]);
}

// the Keep options panel (iìiiiìi, deob-10.0.4.js:147151, wired :147810-147871): the per-item count, painted, equipped,
// the serial and wear sliders, the rarity and Reset
static void keep_panel(StrBuf *sb) {
    const MarketKeep *k = keep_opts();
    char val[64];
    sb_append(sb, "<div class=\"bulk-keep-panel svelte-1wcm5z8\"><label class=\"bulk-opt svelte-1wcm5z8\">");
    sb_appendf(sb, "<input type=\"checkbox\" class=\"svelte-1wcm5z8\"%s onchange=\"knV10WalletKeep('perItem',this.checked)\"> <span class=\"svelte-1wcm5z8\">",
               k->per_item ? " checked" : "");
    sb_append_esc(sb, I18N("wallet.bulk.keep.perItem", "Keep at least this many of each item (the best ones)"));
    sb_appendf(sb, "</span> <input type=\"number\" class=\"bulk-num svelte-1wcm5z8\" min=\"1\" max=\"99\" value=\"%d\"%s onchange=\"knV10WalletKeep('perItemCount',this.value)\"></label> ",
               k->per_item_count, k->per_item ? "" : " disabled");
    sb_appendf(sb, "<label class=\"bulk-opt svelte-1wcm5z8\"><input type=\"checkbox\" class=\"svelte-1wcm5z8\"%s onchange=\"knV10WalletKeep('painted',this.checked)\"> <span class=\"svelte-1wcm5z8\">",
               k->painted ? " checked" : "");
    sb_append_esc(sb, I18N("wallet.bulk.keep.painted", "Keep painted and customized items"));
    sb_appendf(sb, "</span></label> <label class=\"bulk-opt svelte-1wcm5z8\"><input type=\"checkbox\" class=\"svelte-1wcm5z8\"%s onchange=\"knV10WalletKeep('equipped',this.checked)\"> <span class=\"svelte-1wcm5z8\">",
               k->equipped ? " checked" : "");
    sb_append_esc(sb, I18N("wallet.bulk.keep.equipped", "Keep equipped items"));
    sb_append(sb, "</span></label> <label class=\"bulk-opt svelte-1wcm5z8\"><span class=\"svelte-1wcm5z8\">");
    sb_append_esc(sb, I18N("wallet.bulk.keep.serial", "Keep serials up to"));
    if (k->serial_max > 0) snprintf(val, sizeof(val), "#%d", k->serial_max); // :147834
    else snprintf(val, sizeof(val), "%s", I18N("wallet.bulk.keep.off", "Off"));
    sb_appendf(sb, "</span> <input type=\"range\" min=\"0\" max=\"1000\" step=\"10\" class=\"svelte-1wcm5z8\" value=\"%d\" onchange=\"knV10WalletKeep('serialMax',this.value)\"> "
                   "<span class=\"bulk-opt-val svelte-1wcm5z8\">",
               k->serial_max);
    sb_append_esc(sb, val);
    sb_append(sb, "</span></label> <label class=\"bulk-opt svelte-1wcm5z8\"><span class=\"svelte-1wcm5z8\">");
    sb_append_esc(sb, I18N("wallet.bulk.keep.wear", "Keep wear below"));
    if (k->wear_max > 0) snprintf(val, sizeof(val), "%.2f (%s)", k->wear_max, wear_name(k->wear_max - 0.0001)); // :147840-147842
    else snprintf(val, sizeof(val), "%s", I18N("wallet.bulk.keep.off", "Off"));
    sb_appendf(sb, "</span> <input type=\"range\" min=\"0\" max=\"1\" step=\"0.01\" class=\"svelte-1wcm5z8\" value=\"%.2f\" onchange=\"knV10WalletKeep('wearMax',this.value)\"> "
                   "<span class=\"bulk-opt-val svelte-1wcm5z8\">",
               k->wear_max);
    sb_append_esc(sb, val);
    sb_append(sb, "</span></label> <label class=\"bulk-opt svelte-1wcm5z8\"><span class=\"svelte-1wcm5z8\">");
    sb_append_esc(sb, I18N("wallet.bulk.keep.rarity", "Keep rarity"));
    sb_append(sb, "</span> <select class=\"bulk-select svelte-1wcm5z8\" onchange=\"knV10WalletKeep('minRarity',this.value)\"><option class=\"svelte-1wcm5z8\" value=\"-1\">");
    sb_append_esc(sb, I18N("wallet.bulk.keep.off", "Off"));
    sb_append(sb, "</option>");
    for (int r = 0; r < g_viewer_rarity_count; r++) { // :147855-147860: "<rarity> and up"
        char line[96];
        sb_appendf(sb, "<option class=\"svelte-1wcm5z8\" value=\"%d\"%s>", r, k->min_rarity == r ? " selected" : "");
        sb_append_esc(sb, I18N_F(line, "wallet.bulk.keep.rarityAndUp", "{0} and up", g_viewer_rarities[r].name ? g_viewer_rarities[r].name : ""));
        sb_append(sb, "</option>");
    }
    sb_append(sb, "</select></label> <button type=\"button\" class=\"bulk-btn svelte-1wcm5z8\" onclick=\"knV10WalletKeep('reset',0)\">");
    sb_append_esc(sb, I18N("wallet.bulk.keep.reset", "Reset"));
    sb_append(sb, "</button></div>");
}

// the bulk bar while selecting (iîíîìïi, deob-10.0.4.js:147160, wired :147877-147925): the count, what the KR and Junk
// picks pay, Select all / Clear, then Sell for KR (n), Sell for Junk (n) and Destroy (n) when any pick only destroys
static void official_bulkbar(StrBuf *sb) {
    if (!W_select) return;
    int n_kr = 0, n_junk = 0, n_destroy = 0;
    double kr = 0, junk = 0;
    v10_bulk_sums(&W_picks, &n_kr, &n_junk, &n_destroy, &kr, &junk);
    char num[48], line[96];
    snprintf(num, sizeof(num), "%d", W_picks.n);
    sb_append(sb, "<div class=\"bulk-bar svelte-1wcm5z8\">");
    if (W_keep_open) keep_panel(sb);
    sb_append(sb, "<div class=\"bulk-bar-actions svelte-1wcm5z8\"><span class=\"bulk-count svelte-1wcm5z8\">");
    sb_append_esc(sb, I18N_F(line, "wallet.bulk.selected", "{0} selected", num));
    sb_append(sb, "</span> ");
    if (n_kr) {
        v10_fmt_int(num, sizeof(num), kr);
        sb_appendf(sb, "<span class=\"bulk-sum svelte-1wcm5z8\">%s KR</span> ", num);
    }
    if (n_junk) {
        snprintf(num, sizeof(num), "%.2f", junk);
        sb_appendf(sb, "<span class=\"bulk-sum svelte-1wcm5z8\">%s JNK</span> ", num);
    }
    sb_appendf(sb, "<button type=\"button\" class=\"bulk-btn svelte-1wcm5z8\"%s onclick=\"knV10WalletSelectAll()\">", W_busy ? " disabled" : "");
    sb_append_esc(sb, I18N("wallet.bulk.selectAll", "Select all"));
    sb_appendf(sb, "</button> <button type=\"button\" class=\"bulk-btn svelte-1wcm5z8\"%s onclick=\"knV10WalletClear()\">", W_picks.n ? "" : " disabled");
    sb_append_esc(sb, I18N("wallet.bulk.clear", "Clear"));
    sb_appendf(sb, "</button> <button type=\"button\" class=\"bulk-btn svelte-1wcm5z8%s\" onclick=\"knV10WalletKeepToggle()\"><i class=\"material-icons svelte-1wcm5z8\">tune</i> ",
               W_keep_open ? " active" : "");
    sb_append_esc(sb, I18N("wallet.bulk.keep.title", "Keep options"));
    sb_append(sb, "</button> <span class=\"bulk-spacer svelte-1wcm5z8\"></span> ");
    snprintf(num, sizeof(num), "%d", n_kr);
    sb_appendf(sb, "<button type=\"button\" class=\"bulk-btn bulk-kr svelte-1wcm5z8\"%s onclick=\"knV10WalletBulkSell(0)\">", n_kr && !W_busy ? "" : " disabled");
    sb_append_esc(sb, I18N("wallet.bulk.sellKR", "Sell for KR"));
    sb_appendf(sb, " (%s)</button> ", num);
    snprintf(num, sizeof(num), "%d", n_junk);
    sb_appendf(sb, "<button type=\"button\" class=\"bulk-btn bulk-junk svelte-1wcm5z8\"%s onclick=\"knV10WalletBulkSell(1)\">", n_junk && !W_busy ? "" : " disabled");
    sb_append_esc(sb, I18N("wallet.bulk.sellJunk", "Sell for Junk"));
    sb_appendf(sb, " (%s)</button> ", num);
    if (n_destroy) {
        snprintf(num, sizeof(num), "%d", n_destroy);
        sb_appendf(sb, "<button type=\"button\" class=\"bulk-btn bulk-destroy svelte-1wcm5z8\"%s onclick=\"knV10WalletBulkSell(2)\">", W_busy ? " disabled" : "");
        sb_append_esc(sb, I18N("wallet.bulk.destroy", "Destroy"));
        sb_appendf(sb, " (%s)</button>", num);
    }
    sb_append(sb, "</div></div>");
}

static void official_grid(StrBuf *sb) {
    int n = 0;
    const MarketGroup *groups = inv_groups(&n);
    official_toolbar(sb);
    sb_append(sb, "<div class=\"items-grid svelte-1wcm5z8\" id=\"knV10WalletGrid\">");
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
        sb_append(sb, "<div class=\"wallet-items-sentinel svelte-1wcm5z8\"><button type=\"button\" class=\"load-more-btn svelte-zhgogl\" id=\"knV10WalletMore\" onclick=\"knV10WalletMore()\">");
        sb_append_esc(sb, I18N("kn.wallet.loadMore", "Load More"));
        sb_append(sb, "</button></div>");
    }
    if (!drawn) {
        sb_append(sb, "<div class=\"empty-state svelte-1wcm5z8\" id=\"knV10WalletEmpty\"><p class=\"svelte-1wcm5z8\">");
        if (inv_ready()) sb_append_esc(sb, I18N("kn.wallet.empty", "No items match your current filters."));
        else sb_append_esc(sb, W_inv_err[0] ? W_inv_err : inv_gate_text());
        sb_append(sb, "</p></div>");
    }
    sb_append(sb, "</div>");
    official_bulkbar(sb);
}

static void grid(StrBuf *sb, const AccountInfo *a) {
    (void) a; // both accounts: one tile per stack and one per copy (inv_groups)
    official_grid(sb);
}

// the whole modal (.wallet-root) for the active account
// svelte-ekpmow (deob-10.0.5.js:130812-130832): "notification-item <type>", check_circle / error, the message
static void notes_html(StrBuf *sb) {
    sb_append(sb, "<div class=\"notification-list svelte-ekpmow\" id=\"knV10WalletNotes\">");
    for (int i = 0; i < W_nnotes; i++) {
        sb_appendf(sb, "<div class=\"notification-item %s svelte-ekpmow\"><i class=\"material-icons notification-icon svelte-ekpmow\">%s</i> "
                       "<span class=\"notification-message svelte-ekpmow\">",
                   W_notes[i].ok ? "success" : "error", W_notes[i].ok ? "check_circle" : "error");
        sb_append_esc(sb, W_notes[i].msg);
        sb_append(sb, "</span></div>");
    }
    sb_append(sb, "</div>");
}

static void render_notes(void) {
    const HtmlElem old = html_dom_by_id("knV10WalletNotes");
    if (!old) return;
    StrBuf sb = {0};
    notes_html(&sb);
    char *html = sb_finish(&sb);
    html_dom_insert_html(old, HTML_INSERT_BEFORE_BEGIN, html);
    html_dom_remove(old);
    free(html);
}

static void note_expire(void *user) {
    const int id = (int) (intptr_t) user;
    for (int i = 0; i < W_nnotes; i++) {
        if (W_notes[i].id != id) continue;
        memmove(&W_notes[i], &W_notes[i + 1], sizeof(W_notes[0]) * (size_t) (W_nnotes - i - 1));
        W_nnotes--;
        break;
    }
    render_notes();
}

void v10_wallet_note(const int ok, const char *msg) {
    if (W_nnotes == WALLET_NOTE_MAX) { // v10's list is unbounded; the oldest goes here
        memmove(&W_notes[0], &W_notes[1], sizeof(W_notes[0]) * (size_t) (W_nnotes - 1));
        W_nnotes--;
    }
    W_notes[W_nnotes].id = ++W_note_seq;
    W_notes[W_nnotes].ok = ok != 0;
    snprintf(W_notes[W_nnotes].msg, sizeof(W_notes[0].msg), "%s", msg ? msg : "");
    html_timer_add(WALLET_NOTE_MS, note_expire, (void *) (intptr_t) W_notes[W_nnotes].id);
    W_nnotes++;
    render_notes();
}

static char *markup(void) {
    const AccountInfo *a = account();
    StrBuf sb = {0};
    sb_append(&sb, "<div class=\"wallet-root svelte-1wcm5z8\" id=\"knV10WalletRoot\" role=\"dialog\" aria-label=\"");
    sb_append_esc(&sb, I18N("kn.wallet.dialog", "Wallet and Inventory"));
    sb_append(&sb, "\">");
    sb_append(&sb, "<div class=\"window-header svelte-4fhlo6\"><i class=\"material-icons window-header-icon svelte-4fhlo6\">backpack</i> <span class=\"window-header-title svelte-4fhlo6\">");
    sb_append_esc(&sb, I18N("kn.wallet.title", "Inventory"));
    sb_append(&sb, "</span> <button class=\"wallet-catalog-btn svelte-1wcm5z8\" type=\"button\" id=\"knV10WalletCatalogBtn\" onclick=\"playSelect(0.1),knV10WalletCatalog()\">"
                   "<i class=\"material-icons svelte-1wcm5z8\">inventory_2</i> <span class=\"svelte-1wcm5z8\">");
    sb_append_esc(&sb, I18N("kn.wallet.catalog", "Catalog"));
    sb_append(&sb, "</span></button></div>");
    sb_append(&sb, "<div class=\"wallet-body svelte-1wcm5z8\">");
    currencies(&sb, a);
    grid(&sb, a);
    sb_append(&sb, "</div>");
    notes_html(&sb);
    sb_append(&sb, "</div>");
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

// the bulk overlay ended (v10_bulk.c): a sale clears the picks (deob-10.0.4.js:147363); a cancel keeps them
static void bulk_done(void *user, const int ok, const char *message) {
    (void) user;
    (void) message; // the overlay showed the error itself
    W_busy = 0;
    if (ok) {
        market_picks_free(&W_picks);
        W_select = 0;
    }
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
    if (!inv_ready() && !inv_loading()) inv_load(inventory_loaded, NULL); // the locks ("ilk" on Box Office)
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

// ---------------------------------------------------------------- the Keep options' effect on the picks

typedef struct {
    int copy, reason, serial, idx;
    double wear;
} KeepUnit;

// deob-10.0.4.js:147086-147097: the units with a reason first (they take the per-item slots), then the lowest wear,
// then the lowest serial (none: wear 1, serial Infinity)
static int cmp_keep_units(const void *pa, const void *pb) {
    const KeepUnit *a = pa, *b = pb;
    const int ka = kept_reason(a->reason) ? 0 : 1, kb = kept_reason(b->reason) ? 0 : 1;
    if (ka != kb) return ka - kb;
    if (a->wear != b->wear) return a->wear < b->wear ? -1 : 1;
    if (a->serial != b->serial) return a->serial < b->serial ? -1 : 1;
    return a->idx - b->idx;
}

// Select all (iìïïìïî, deob-10.0.4.js:147077-147101): per item, the best per-item count stay, the rest without a
// reason are picked. A Box Office stack's units are its group's plain copies: the picked stack keeps the units that
// fall in the kept slots (MarketPicks.keeps).
static void select_all(void) {
    market_picks_free(&W_picks);
    int n = 0;
    const MarketGroup *groups = inv_groups(&n);
    const MarketKeep *k = keep_opts();
    const int per = k->per_item ? (k->per_item_count > 0 ? k->per_item_count : 0) : 0;
    for (int gi = 0; groups && gi < n; gi++) {
        const MarketGroup *g = &groups[gi];
        int stack_units = g->stack_id ? (g->stack_max > 0 ? g->stack_max : 1) : 0;
        if (stack_units > 10000) stack_units = 10000;
        const int total = g->n_instances + stack_units;
        KeepUnit *u = total ? calloc((size_t) total, sizeof(*u)) : NULL;
        if (!u) continue;
        int m = 0;
        for (int c = 0; c < g->n_instances; c++) {
            const MarketCopyMeta *meta = g->metas ? &g->metas[c] : NULL;
            u[m] = (KeepUnit) {c, row_reason(g, c), meta && meta->serial > 0 ? meta->serial : 0x7fffffff, m, meta && meta->wear >= 0 ? meta->wear : 1};
            m++;
        }
        const int stack_reason = g->stack_id ? row_reason(g, -1) : MARKET_KEEP_NONE;
        for (int i = 0; i < stack_units; i++, m++) u[m] = (KeepUnit) {-1, stack_reason, 0x7fffffff, m, 1};
        qsort(u, (size_t) m, sizeof(*u), cmp_keep_units);
        int stack_sell = 0;
        for (int i = per; i < m; i++) {
            if (u[i].reason != MARKET_KEEP_NONE) continue;
            if (u[i].copy >= 0) market_picks_toggle(&W_picks, inv_locks(), g->instances[u[i].copy], g->item, 0);
            else stack_sell++;
        }
        if (stack_sell > 0 && market_picks_toggle(&W_picks, inv_locks(), g->stack_id, g->item, 1))
            market_picks_set_keep(&W_picks, W_picks.n - 1, stack_units - stack_sell);
        free(u);
    }
}

// deob-10.0.4.js:147317-147321: a changed option drops the picks it now keeps
static void prune_kept_picks(void) {
    int n = 0;
    const MarketGroup *groups = inv_groups(&n);
    for (int gi = 0; groups && gi < n; gi++) {
        const MarketGroup *g = &groups[gi];
        if (g->stack_id && picked_row(g, -1) && row_reason(g, -1) != MARKET_KEEP_NONE)
            market_picks_toggle(&W_picks, NULL, g->stack_id, g->item, 1);
        for (int c = 0; c < g->n_instances; c++)
            if (picked_row(g, c) && row_reason(g, c) != MARKET_KEEP_NONE) market_picks_toggle(&W_picks, NULL, g->instances[c], g->item, 0);
    }
}

static double arg_num(const HtmlArg *args, const int argc, const int i) {
    if (!args || i >= argc) return 0;
    if (args[i].kind == HTML_ARG_NUM || args[i].kind == HTML_ARG_BOOL) return args[i].num;
    if (!args[i].str) return 0;
    if (!strcmp(args[i].str, "true")) return 1;
    return atof(args[i].str);
}

// ---------------------------------------------------------------- handlers

static int arg_int(const HtmlArg *args, const int argc, const int i) {
    if (!args || i >= argc) return -1;
    if (args[i].kind == HTML_ARG_NUM || args[i].kind == HTML_ARG_BOOL) return (int) args[i].num;
    return args[i].str ? atoi(args[i].str) : -1;
}

int v10_wallet_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
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
        // v10's inventory tile opens the item popup (viewItem), not the Catalog's item page
        const HtmlArg item = {.kind = HTML_ARG_NUM, .num = arg_int(args, argc, 0)};
        html_ui_dispatch("viewItem", ev, &item, 1);
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
        else if (!inv_ready() && !inv_loading()) inv_load(inventory_loaded, NULL); // the locks (Box Office: "ilk")
        refresh();
        return 1;
    }
    if (!strcmp(name, "knV10WalletPick") || !strcmp(name, "knV10WalletLock")) {
        int n = 0;
        const MarketGroup *groups = inv_groups(&n);
        const int gi = arg_int(args, argc, 0), copy = arg_int(args, argc, 1);
        if (!groups || gi < 0 || gi >= n || copy >= groups[gi].n_instances) return 1;
        const MarketGroup *g = &groups[gi];
        if (!strcmp(name, "knV10WalletPick")) {
            const int skin = copy < 0 ? g->stack_id : g->instances[copy];
            if (!picked_row(g, copy) && row_reason(g, copy) != MARKET_KEEP_NONE) return 1; // a kept unit is not picked
            market_picks_toggle(&W_picks, inv_locks(), skin, g->item, copy < 0);
            refresh();
            return 1;
        }
        inv_set_lock(g, copy, !inv_locked(g, copy), lock_done, NULL);
        return 1;
    }
    if (!strcmp(name, "knV10WalletBulkSell")) { // IÌîiiiï(action) (deob-10.0.4.js:147352): the picks the action takes
        if (W_busy) return 1;
        const int action = arg_int(args, argc, 0);
        MarketPicks take = v10_bulk_filter(&W_picks, action >= 0 && action <= 2 ? action : 0);
        if (take.n == 0) {
            market_picks_free(&take);
            return 1;
        }
        W_busy = 1;
        v10_bulk_sell_open(action >= 0 && action <= 2 ? action : 0, take, bulk_done, NULL);
        return 1;
    }
    if (!strcmp(name, "knV10WalletSelectAll") || !strcmp(name, "knV10WalletClear")) {
        market_picks_free(&W_picks);
        if (!strcmp(name, "knV10WalletSelectAll")) select_all(); // iìïïìïî (deob-10.0.4.js:147896): with the Keep options
        refresh();
        return 1;
    }
    if (!strcmp(name, "knV10WalletKeepToggle")) { // the tune button (deob-10.0.4.js:147900-147903)
        W_keep_open = !W_keep_open;
        refresh();
        return 1;
    }
    if (!strcmp(name, "knV10WalletKeep")) { // a Keep option changed: saved, and the picks it now keeps dropped
        MarketKeep *k = keep_opts();
        const char *field = args && argc > 0 && args[0].str ? args[0].str : "";
        const double v = arg_num(args, argc, 1);
        if (!strcmp(field, "perItem")) k->per_item = v != 0;
        else if (!strcmp(field, "perItemCount")) k->per_item_count = v < 1 ? 1 : v > 99 ? 99 : (int) v;
        else if (!strcmp(field, "painted")) k->painted = v != 0;
        else if (!strcmp(field, "equipped")) k->equipped = v != 0;
        else if (!strcmp(field, "serialMax")) k->serial_max = v < 0 ? 0 : v > 1000 ? 1000 : (int) v;
        else if (!strcmp(field, "wearMax")) k->wear_max = v < 0 ? 0 : v > 1 ? 1 : v;
        else if (!strcmp(field, "minRarity")) k->min_rarity = (int) v;
        else if (!strcmp(field, "reset")) *k = (MarketKeep) MARKET_KEEP_DEFAULTS; // :147862-147864
        keep_save();
        prune_kept_picks();
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
    v10_bulk_tick(); // a bulk run's progress bar
    if (!W.open) return;
    const AccountInfo *a = account();
    if (!a || a->id != W.account_id) {
        v10_view_clear(); // the account is gone: v10's $account effect
        return;
    }
    if (changed(a)) refresh();
}

void v10_wallet_account_changed(void) {
    // the account itself changed, not only its data: the menu calls this for each (account_active_generation), and a
    // krunker.io answer's balances (a bulk run's funds, account_kio_set_balances) or a spent paint move the data of the
    // same account; clearing then dropped the run's own inventory reload (its done never ran: no "Listed" / "Sold" note)
    static unsigned seen;
    static int seen_any;
    const unsigned identity = account_active_identity();
    if (seen_any && identity == seen) return;
    seen = identity;
    seen_any = 1;
    kio_inventory_clear();
    bo_inventory_clear();
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
    // select: true opens select mode (the account's rows as tiles), keep: true its Keep options panel (no locks loaded)
    if (cJSON_IsTrue(cJSON_GetObjectItem(state, "select"))) {
        W_select = 1;
        W_keep_open = cJSON_IsTrue(cJSON_GetObjectItem(state, "keep"));
        if (cJSON_IsTrue(cJSON_GetObjectItem(state, "selectAll"))) select_all();
        refresh();
    }
}
