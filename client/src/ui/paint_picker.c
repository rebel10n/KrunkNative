// The paint picker of an owned skin card (client/include/paint_picker.h): Krunker v10.0.7's paint popup
// (deob-10.0.7.js:108483-108814) on KrunkNative's brain. The popup is the item viewer (an iframe of
// /viewer.html?nobg&showcase=<item>, drawn natively over the iframe's box by viewer_embed.h) with the item's name and
// the paint controls over it. The swatches are every store paint, the owned ones with their count from the owned list
// (data slot 10), the others locked: picking one shows it on the item, Paint is for an owned one only. Paint applies the
// pick as v10's setPaint (:108616-108665): the socket frame
// ["ip", skinId, item, paint, colour, shader, uid], answered by ["ipr", {copy_id, item_id, paint_item_id, m,
// count} | null, uid] or, for a refusal, ["po", null, message] (services/brain/economy.c sh_item_paint; the same
// work as the HTTP route POST /items/<id>/paint, doc/v10/paint-copy.md). The list is the store's and the account's own
// data, so the popup has no load to fail; v10's empty state is for a store without paints. A paint that was applied
// sets the copy's paint on the owned list at once (account_paint_applied); the brain pushes the account again ("ua").
//
// A krunker.io account sends v10.0.7's own request to krunker.io with its Bearer (kio_store_write): items.setPaint
// POST /items/<skinId>/paint {item_id, paint_item_id} (deob-10.0.7.js:27093-27097, :108616-108665), skinId the card's
// instance (the inventory tab's row sid, the loadout copy's id). An error shows error.message || "Failed to apply
// paint" in the popup; a success takes one paint unit off the account's skins as v10 does (:108632-108648), gives the
// instance its paint as the inventory's caller does (:147688-147705), toasts, and the Market's inventory asks again
// (:134247-134250).
#include <paint_picker.h>
#include <backend.h>
#include <backend_ui.h>
#include <kio_store.h> // kio_store_write: a krunker.io account's setPaint
#include <ui_logic.h>
#include <v10_ui.h>
#include <i18n.h>
#include <js_int.h> // js_number_text: the preview's &wear=
#include <shared.h> // skin_copy_seed: the preview's &seed=
#include "../viewer/viewer_store.h"
#include <cJSON.h>
#include <stdint.h> // intptr_t: the paint of a krunker.io request
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define PAINT_TYPE 18        // a store paint (v10: type 18, deob-10.0.7.js:20164)
#define COPY_ID_BASE 1000000 // the brain's V10_COPY_ID_BASE: a copy's id; below it a stack's sid is its store index
#define PAINT_WAIT_MS 15000  // no answer by then: the popup says the paint failed (the market's wait is the same)

// One store paint (deob-10.0.7.js:108523-108549): its index, the account's count of it (owned entries cnt ?? 1, 0 = a
// locked swatch the popup still previews), its colour or shader and name.
typedef struct {
    int index;
    int count;
    int has_shader, shader;
    char color[16]; // paintColor "#rrggbb", "" for a shader paint
    char name[96];  // the store name, "Paint" when it has none (v10: name || "Paint")
} PaintRow;

static struct {
    int open, busy;
    int ind, sid;      // the card's item (store index) and its skinId (copy id, or the stack's store index)
    int selected;      // store index of the picked paint, -1 none
    int shader;        // the preview's paintShader (v10's shader state: initialShader, then a picked shader), -1 none
    int pending_paint; // the paint of the request in flight (its toast and owned-list update)
    int uid;           // the request in flight's uid (the brain echoes it in "ipr")
    int timer;         // the wait's deadline (html_timer), 0 none
    float scroll;      // the swatch row's scrollLeft, kept across the popup's re-renders
    char error[160];
    char initial_paint[16]; // the copy's m.paint (v10's initialPaint), "" none
    int has_wear;           // the copy's m.wear (v10's initialWear)
    double wear;
    int seed;               // iiiíìiî({id: skinId}, metadata): the copy's seed, 0 none
} S = {.selected = -1, .shader = -1, .pending_paint = -1};

static int g_listening; // the social socket's listener (on_social)
static int g_uids;      // the last uid sent

// ---------------------------------------------------------------- helpers

static int arg_int(const HtmlArg *args, const int argc, const int i, const int fallback) {
    if (i >= argc) return fallback;
    const HtmlArg *a = &args[i];
    if (a->kind == HTML_ARG_NUM || a->kind == HTML_ARG_BOOL) return (int) a->num;
    if (a->kind == HTML_ARG_STR && a->str) {
        char *end;
        const long v = strtol(a->str, &end, 10);
        return end != a->str ? (int) v : fallback;
    }
    return fallback;
}

static const cJSON *owned_list(void) {
    const AccountInfo *acc = active_account();
    return acc && acc->data ? cJSON_GetArrayItem(acc->data, 10) : NULL;
}

// The store item's JSON (skins.json), NULL when unknown.
static const cJSON *store_json(const int index) {
    return ui_loadout_skin_json(index);
}

static const char *json_str(const cJSON *j, const char *key) {
    const cJSON *v = j ? cJSON_GetObjectItemCaseSensitive(j, key) : NULL;
    return cJSON_IsString(v) && v->valuestring && v->valuestring[0] ? v->valuestring : NULL;
}

static int is_paint(const int index) {
    const ViewerSkin *s = viewer_skin(index);
    if (!s || s->type != PAINT_TYPE) return 0;
    const cJSON *j = store_json(index);
    const cJSON *shader = j ? cJSON_GetObjectItemCaseSensitive(j, "paintShader") : NULL;
    return json_str(j, "paintColor") != NULL || cJSON_IsNumber(shader);
}

static const char *item_name(const int index) {
    const char *n = json_str(store_json(index), "name");
    return n ? n : I18N("kn.paint.unnamed", "Unnamed Item");
}

// The owned copy with id `id` (a copy's entry carries its id; a stack's does not).
static const cJSON *owned_copy(const cJSON *owned, const int id) {
    const cJSON *e;
    cJSON_ArrayForEach(e, owned) {
        const cJSON *cid = cJSON_GetObjectItemCaseSensitive(e, "id");
        if (cJSON_IsNumber(cid) && cid->valueint == id) return e;
    }
    return NULL;
}

// Sorted as v10 sorts the paints (deob-10.0.7.js:108548): owned first, then by index.
static int row_cmp(const void *pa, const void *pb) {
    const PaintRow *a = pa, *b = pb;
    const int oa = a->count > 0, ob = b->count > 0;
    if (oa != ob) return ob - oa;
    return a->index - b->index;
}

// Every store paint with the account's count of it, sorted (v10's list, deob-10.0.7.js:108523-108549): the swatches
// are all of them, an unowned one locked and preview only. *n is their number; the caller frees the array.
static PaintRow *store_paints(int *n) {
    *n = 0;
    const cJSON *owned = owned_list();
    int total = 0;
    for (int i = 0; i < g_viewer_skin_count; i++) total += is_paint(i);
    PaintRow *all = total ? calloc((size_t) total, sizeof(*all)) : NULL;
    if (!all) return NULL;
    int k = 0;
    for (int i = 0; i < g_viewer_skin_count && k < total; i++) {
        if (!is_paint(i)) continue;
        PaintRow *r = &all[k++];
        r->index = i;
        const cJSON *j = store_json(i);
        const cJSON *shader = cJSON_GetObjectItemCaseSensitive(j, "paintShader");
        r->has_shader = cJSON_IsNumber(shader);
        r->shader = r->has_shader ? shader->valueint : 0;
        const char *colour = json_str(j, "paintColor");
        snprintf(r->color, sizeof(r->color), "%s", colour ? colour : "");
        const char *name = json_str(j, "name");
        snprintf(r->name, sizeof(r->name), "%s", name ? name : I18N("kn.paint.fallbackName", "Paint"));
        const cJSON *e;
        cJSON_ArrayForEach(e, owned) {
            const cJSON *ind = cJSON_GetObjectItemCaseSensitive(e, "ind"), *cnt = cJSON_GetObjectItemCaseSensitive(e, "cnt");
            if (cJSON_IsNumber(ind) && ind->valueint == i) r->count += cJSON_IsNumber(cnt) ? cnt->valueint : 1;
        }
    }
    qsort(all, (size_t) total, sizeof(*all), row_cmp);
    *n = total;
    return all;
}

static const PaintRow *find_row(const PaintRow *rows, const int n, const int index) {
    for (int i = 0; rows && i < n; i++) if (rows[i].index == index) return &rows[i];
    return NULL;
}

// The card's copy: its m as an object (krunker.io's m is a JSON string) for initialPaint / initialShader / initialWear
// (deob-10.0.7.js:134238-134245, :147679-147686). NULL for a stack (no copy yet) or no m; the caller cJSON_Deletes it.
static cJSON *copy_meta(void) {
    if (S.sid < COPY_ID_BASE && account_backend() != ACCOUNT_BACKEND_KRUNKER) return NULL; // (krunker.io: any instance id)
    const cJSON *e = owned_copy(owned_list(), S.sid);
    const cJSON *m = e ? cJSON_GetObjectItemCaseSensitive(e, "m") : NULL;
    if (cJSON_IsString(m) && m->valuestring) return cJSON_Parse(m->valuestring);
    return cJSON_IsObject(m) ? cJSON_Duplicate(m, 1) : NULL;
}

// A colour as a style value or a query value: only "#" and hex digits survive.
static void swatch_colour(char *out, const size_t n, const char *colour) {
    size_t k = 0;
    for (const char *p = colour; *p && k + 1 < n; p++) {
        const char c = *p;
        if (c == '#' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) out[k++] = c;
    }
    out[k] = '\0';
}

// ---------------------------------------------------------------- the popup

static void paint_render(void);

// The preview iframe's src (deob-10.0.7.js:108675): "/viewer.html?nobg&showcase=" + item.index, the paint, "&paintShader="
// + the shader state, "&wear=" + initialWear, "&seed=" + the copy's seed. v10 loads it with initialPaint and posts each
// picked colour to the iframe (updatePaint, :108596-108606); KrunkNative's viewer (viewer.c state_from_params: the native
// showcase over the iframe's box and the web build's viewer page) reads the paint from its query only, so the src carries
// the picked colour itself and the preview follows the pick.
static void preview_src(char *out, const size_t n, const PaintRow *sel) {
    int k = snprintf(out, n, "/viewer.html?nobg&showcase=%d", S.ind);
    char hex[16];
    if (sel && sel->color[0] && !sel->has_shader) {
        swatch_colour(hex, sizeof(hex), sel->color);
        k += snprintf(out + k, k < (int) n ? n - (size_t) k : 0, "&paint=%s", hex + (hex[0] == '#'));
    } else if (!sel && S.initial_paint[0]) {
        swatch_colour(hex, sizeof(hex), S.initial_paint);
        k += snprintf(out + k, k < (int) n ? n - (size_t) k : 0, "&paint=%s", hex + (hex[0] == '#'));
    }
    if (S.shader >= 0) k += snprintf(out + k, k < (int) n ? n - (size_t) k : 0, "&paintShader=%d", S.shader);
    if (S.has_wear) {
        char num[40];
        js_number_text(num, sizeof(num), S.wear);
        k += snprintf(out + k, k < (int) n ? n - (size_t) k : 0, "&wear=%s", num);
    }
    if (S.seed) snprintf(out + k, k < (int) n ? n - (size_t) k : 0, "&seed=%d", S.seed);
}

// The popup (deob-10.0.7.js:108483-108499 markup, :108677-108808 bindings) in v10's popup component with width 1200,
// translucent and slimBorder (:108677-108680, :12094-12110): the item viewer iframe fills the 800px content, the
// header and the controls lie over it (paint-overlay). Not drawn: the wear row, which only the preview-only picker of
// a wearable item has (previewOnly, :109454-109460); the paint button never opens that one.
static char *paint_html(void) {
    int n = 0;
    PaintRow *rows = store_paints(&n);
    const PaintRow *sel = find_row(rows, n, S.selected);
    const ViewerSkin *item = viewer_skin(S.ind);
    // item.rarity_meta: color || "#B0B0B0", animate (Unobtainable), dark (Contraband)
    const char *rcolor = item && item->rarity >= 0 && item->rarity < g_viewer_rarity_count && g_viewer_rarities[item->rarity].color
                             ? g_viewer_rarities[item->rarity].color : "#B0B0B0";
    const int animate = item && item->rarity == 6, dark = item && item->rarity == 5;
    char src[256], label[160];
    preview_src(src, sizeof(src), sel);
    StrBuf sb = {0};
    sb_append(&sb, "<div class=\"popup-container svelte-1wz7ymp\"><div class=\"popup svelte-1wz7ymp translucent slim-border\" style=\"width: 1200px;\">"
                   "<div class=\"paint-container io-popup-container svelte-10qrqvt\"><div class=\"paint-content io-popup-content svelte-10qrqvt\">");
    sb_appendf(&sb, "<iframe class=\"paint-iframe svelte-10qrqvt%s\" title=\"Paint preview viewer\" src=\"", animate ? " rainbow" : "");
    sb_append_esc(&sb, src);
    sb_append(&sb, "\" style=\"border: 3px solid ");
    sb_append_esc(&sb, rcolor);
    sb_appendf(&sb, ";\"></iframe> <div class=\"paint-overlay svelte-10qrqvt\"><div class=\"paint-header svelte-10qrqvt\"><div class=\"item-name svelte-10qrqvt%s%s\" style=\"color: ",
               animate ? " rainbow" : "", dark ? " dark" : "");
    sb_append_esc(&sb, rcolor);
    sb_append(&sb, ";\">");
    sb_append_esc(&sb, item_name(S.ind));
    sb_append(&sb, "</div> <div class=\"item-sub svelte-10qrqvt\">");
    sb_append_esc(&sb, I18N("kn.paint.sub", "Pick a paint to preview, then press Paint to apply."));
    sb_append(&sb, "</div></div> <div class=\"paint-controls svelte-10qrqvt\">");
    if (n == 0) { // the store has no paint at all (:108689-108691)
        sb_append(&sb, "<div class=\"paint-empty svelte-10qrqvt\">");
        sb_append_esc(&sb, I18N("kn.paint.empty", "You don’t own any paints yet. Earn paints from rewards or the market to start painting items."));
        sb_append(&sb, "</div>");
    } else {
        // the arrows start disabled and follow the row's scroll once it is laid out (slider_sync, :108593-108595)
        sb_append(&sb, "<div class=\"swatch-slider svelte-10qrqvt\"><button type=\"button\" class=\"slider-arrow left svelte-10qrqvt disabled\" "
                       "aria-label=\"Scroll paints left\" onclick=\"paintScroll(-1)\" disabled><span class=\"slider-arrow-icon svelte-10qrqvt\"></span></button> "
                       "<div class=\"swatch-row svelte-10qrqvt\" role=\"radiogroup\" aria-label=\"Available paints\" onwheel=\"return paintWheel()\">");
        for (int i = 0; i < n; i++) {
            const PaintRow *r = &rows[i];
            const int on = r->index == S.selected, locked = r->count <= 0;
            // aria-label: name + " - locked, preview only" or name + " - " + count + " owned" (:108724)
            sb_appendf(&sb, "<button type=\"button\" class=\"swatch svelte-10qrqvt%s%s\" role=\"radio\" aria-checked=\"%s\" aria-label=\"",
                       on ? " selected" : "", locked ? " locked" : "", on ? "true" : "false");
            sb_append_esc(&sb, r->name);
            if (locked) sb_append(&sb, " - locked, preview only");
            else sb_appendf(&sb, " - %d owned", r->count);
            sb_appendf(&sb, "\" onclick=\"paintPick(%d)\">", r->index);
            // the paint's thumbnail, else its colour as the fill's background (:108702-108711)
            const char *thumb = json_str(store_json(r->index), "thumbnail");
            if (thumb) {
                sb_append(&sb, "<img class=\"swatch-img svelte-10qrqvt\" alt=\"\" draggable=\"false\" src=\"");
                sb_append_esc(&sb, thumb);
                sb_append(&sb, "\">");
            } else if (r->color[0]) {
                char colour[24];
                swatch_colour(colour, sizeof(colour), r->color);
                sb_appendf(&sb, "<span class=\"swatch-fill svelte-10qrqvt\" style=\"background: %s;\"></span>", colour);
            } else {
                sb_append(&sb, "<span class=\"swatch-fill svelte-10qrqvt\"></span>");
            }
            if (locked) sb_append(&sb, " <span class=\"swatch-lock material-icons svelte-10qrqvt\">lock</span>");
            else sb_appendf(&sb, " <span class=\"swatch-count svelte-10qrqvt\">%d</span>", r->count);
            sb_append(&sb, " <span class=\"swatch-name svelte-10qrqvt\">");
            sb_append_esc(&sb, r->name);
            sb_append(&sb, "</span></button>");
        }
        sb_append(&sb, "</div> <button type=\"button\" class=\"slider-arrow right svelte-10qrqvt disabled\" aria-label=\"Scroll paints right\" "
                       "onclick=\"paintScroll(1)\" disabled><span class=\"slider-arrow-icon svelte-10qrqvt\"></span></button></div>");
    }
    if (S.error[0]) {
        sb_append(&sb, " <div class=\"paint-error svelte-10qrqvt\">");
        sb_append_esc(&sb, S.error);
        sb_append(&sb, "</div>");
    }
    // Cancel (disabled while painting) and Paint: "Painting…", "Locked (<name>)" for an unowned pick, "Paint (<name>)",
    // "Paint" with no pick; disabled while painting, with no pick or a locked one (:108766-108796)
    sb_appendf(&sb, " <div class=\"paint-actions svelte-10qrqvt\"><button class=\"btn btn-cancel svelte-10qrqvt\" onclick=\"knViewPop(0)\"%s>",
               S.busy ? " disabled" : "");
    sb_append_esc(&sb, I18N("generic.cancel", "Cancel"));
    sb_append(&sb, "</button> ");
    if (S.busy) snprintf(label, sizeof(label), "%s", I18N("kn.paint.applying", "Painting…"));
    else if (sel && sel->count <= 0) I18N_F(label, "kn.paint.locked", "Locked ({0})", sel->name);
    else if (sel) I18N_F(label, "kn.paint.apply", "Paint ({0})", sel->name);
    else snprintf(label, sizeof(label), "%s", I18N("kn.paint.fallbackName", "Paint"));
    sb_appendf(&sb, "<button class=\"btn btn-apply svelte-10qrqvt\" onclick=\"paintApply()\"%s>", S.busy || !sel || sel->count <= 0 ? " disabled" : "");
    sb_append_esc(&sb, label);
    sb_append(&sb, "</button></div></div></div></div></div></div></div>");
    free(rows);
    return sb_finish(&sb);
}

static HtmlElem swatch_row(void) {
    return html_dom_query(0, ".paint-controls .swatch-row");
}

static void arrow_state(const char *selector, const int enabled) {
    const HtmlElem b = html_dom_query(0, selector);
    if (!b) return;
    html_dom_set_class(b, "disabled", !enabled);
    if (enabled) html_dom_remove_attr(b, "disabled");
    else html_dom_set_attr(b, "disabled", "");
}

// updateArrows (deob-10.0.7.js:108593-108595): left once scrollLeft > 1, right while scrollLeft + clientWidth <
// scrollWidth - 1. Natively the row is laid out on demand (html_dom_get_scroll), where v10 waits a frame.
static void slider_sync(void) {
    HtmlScroll sc;
    const HtmlElem row = swatch_row();
    if (!row || !html_dom_get_scroll(row, &sc)) return;
    S.scroll = sc.left;
    arrow_state(".paint-controls .slider-arrow.left", sc.left > 1.0f);
    arrow_state(".paint-controls .slider-arrow.right", sc.left + sc.client_w < sc.width - 1.0f);
}

static void slider_scroll_by(const float dx) {
    const HtmlElem row = swatch_row();
    HtmlScroll sc;
    if (!row || !html_dom_get_scroll(row, &sc)) return;
    html_dom_set_scroll(row, sc.left + dx, sc.top);
    slider_sync();
}

// Re-renders the open popup (the top view) when it is still open; the swatch row keeps its scroll.
static void paint_render(void) {
    if (!S.open) return;
    const float keep = S.scroll;
    v10_view_set(paint_html());
    const HtmlElem row = swatch_row();
    if (row && keep > 0.0f) html_dom_set_scroll(row, keep, 0.0f);
    slider_sync();
}

static void paint_closed(const int accepted, void *user) {
    (void) accepted;
    (void) user;
    S.open = 0;
    S.error[0] = 0;
}

// ---------------------------------------------------------------- apply

static void applied_toast(int item, int paint);

// The brain's answer to the paint in flight: `data` is the "ipr" payload (NULL: refused, no text), `message` the
// "po" text (NULL: none). On success the paint's unit is taken, the copy gets its paint, and when the copy is new one
// unpainted unit of the item becomes it (from_stack); the popup closes with the toast. The brain pushes the account
// again after ("ua"); v10 takes one paint unit locally and the copy comes from the reload (deob-10.0.7.js:108623-108648).
static void finish_apply(const cJSON *data, const char *message) {
    S.busy = 0;
    if (S.timer) html_timer_cancel(S.timer);
    S.timer = 0;
    const cJSON *copy = data ? cJSON_GetObjectItemCaseSensitive(data, "copy_id") : NULL;
    if (!cJSON_IsNumber(copy)) {
        const char *fallback = I18N("kn.paint.failed", "Failed to apply paint");
        snprintf(S.error, sizeof(S.error), "%s", message && message[0] ? message : fallback);
        paint_render();
        return;
    }
    const int item = S.ind, paint = S.pending_paint;
    // v10.0.3 (deob-10.0.3.js:149074-149075): "Painted " + item.name + (paintName ? " with " + paintName : ""), where
    // paintName is the store paint's own name (no "Paint" fallback: that is only the Paint button's label, :122033)
    applied_toast(item, paint);
    account_paint_applied(item, paint, copy->valueint, S.sid < COPY_ID_BASE, cJSON_GetObjectItemCaseSensitive(data, "m"));
    ui_loadout_owned_changed();
    if (S.open) v10_view_pop(1); // accepted: v10's push resolves with the result and the popup closes (deob-10.0.7.js:108649-108654)
}

// The social socket: the answers to the paint in flight ("ipr" with its uid, "po" for a refusal of the socket's
// requests: the one request in flight is ours), and the socket's close (the request is lost).
static void on_social(void *user, const cJSON *frame) {
    (void) user;
    if (!S.busy) return;
    const char *label = cJSON_GetStringValue(cJSON_GetArrayItem(frame, 0));
    if (!label) return;
    if (!strcmp(label, "__close")) {
        finish_apply(NULL, NULL);
    } else if (!strcmp(label, "ipr")) {
        const cJSON *uid = cJSON_GetArrayItem(frame, 2);
        if (cJSON_IsNumber(uid) && uid->valueint == S.uid) finish_apply(cJSON_GetArrayItem(frame, 1), NULL);
    } else if (!strcmp(label, "po")) {
        finish_apply(NULL, cJSON_GetStringValue(cJSON_GetArrayItem(frame, 2)));
    }
}

static void paint_expired(void *user) {
    (void) user;
    S.timer = 0;
    if (S.busy) finish_apply(NULL, NULL);
}

// The toast of an applied paint: "Painted " + item.name + (paintName ? " with " + paintName : "") (deob-10.0.7.js
// :134247-134249, :147726-147728), paintName the store paint's own name.
static void applied_toast(const int item, const int paint) {
    const char *paint_name = json_str(store_json(paint), "name");
    char text[256];
    if (paint_name && paint_name[0]) I18N_F(text, "kn.paint.applied", "Painted {0} with {1}", item_name(item), paint_name);
    else snprintf(text, sizeof(text), "Painted %s", item_name(item));
    StrBuf sb = {0};
    sb_append_esc(&sb, text);
    char *toast = sb_finish(&sb);
    ui_notify(toast);
    free(toast);
}

// krunker.io's answer to items.setPaint (deob-10.0.7.js:108626-108660): an error's message (no code text: :108631),
// else "Failed to apply paint"; a success takes the paint unit and paints the instance (account_paint_applied), the
// toast, and the Market's inventory asks again (:134250). user: the paint index.
static void kio_paint_answer(void *user, const int status, const cJSON *data, const char *code, const char *message) {
    (void) status;
    (void) data;
    const int paint = (int) (intptr_t) user;
    S.busy = 0;
    if (message || code) {
        snprintf(S.error, sizeof(S.error), "%s", message && message[0] ? message : I18N("kn.paint.failed", "Failed to apply paint"));
        paint_render();
        return;
    }
    applied_toast(S.ind, paint);
    // the instance's new paint: {paintShader} for a shader paint, else {paint: colour} (:108651-108655, :147697-147699)
    cJSON *m = cJSON_CreateObject();
    const cJSON *j = store_json(paint);
    const cJSON *shader = j ? cJSON_GetObjectItemCaseSensitive(j, "paintShader") : NULL;
    if (cJSON_IsNumber(shader)) cJSON_AddNumberToObject(m, "paintShader", shader->valuedouble);
    else if (json_str(j, "paintColor")) cJSON_AddStringToObject(m, "paint", json_str(j, "paintColor"));
    account_paint_applied(S.ind, paint, S.sid, 0, m);
    cJSON_Delete(m);
    ui_loadout_owned_changed();
    v10_market_owned_changed();
    if (S.open) v10_view_pop(1);
}

static void apply(void) {
    if (!S.open || S.busy || S.selected < 0) return;
    int n = 0;
    PaintRow *rows = store_paints(&n);
    const PaintRow *row = find_row(rows, n, S.selected);
    if (!row || row->count <= 0) { // (:108618-108619)
        free(rows);
        snprintf(S.error, sizeof(S.error), "%s", I18N("kn.paint.none", "You don’t have any of that paint left."));
        paint_render();
        return;
    }
    if (account_backend() == ACCOUNT_BACKEND_KRUNKER) {
        // items.setPaint(skinId, {item_id: item.index, paint_item_id: paint.index}) (deob-10.0.7.js:108623-108626)
        char path[64], body[96];
        snprintf(path, sizeof(path), "items/%d/paint", S.sid);
        snprintf(body, sizeof(body), "{\"item_id\":%d,\"paint_item_id\":%d}", S.ind, row->index);
        const int paint = row->index;
        free(rows);
        S.error[0] = 0;
        S.busy = 1;
        paint_render();
        kio_store_write("POST", path, body, kio_paint_answer, (void *) (intptr_t) paint); // (a refusal answers at once)
        return;
    }
    // v10's items.setPaint(skinId, {item_id, paint_item_id}) (deob-10.0.7.js:108623-108626) as the socket frame
    cJSON *f = cJSON_CreateArray();
    cJSON_AddItemToArray(f, cJSON_CreateString("ip"));
    cJSON_AddItemToArray(f, cJSON_CreateNumber(S.sid));
    cJSON_AddItemToArray(f, cJSON_CreateNumber(S.ind));
    cJSON_AddItemToArray(f, cJSON_CreateNumber(row->index));
    cJSON_AddItemToArray(f, cJSON_CreateString(row->color));
    cJSON_AddItemToArray(f, cJSON_CreateNumber(row->has_shader ? row->shader : -1));
    S.uid = ++g_uids;
    cJSON_AddItemToArray(f, cJSON_CreateNumber(S.uid));
    S.pending_paint = row->index;
    free(rows);
    S.error[0] = 0;
    if (!g_listening) g_listening = backend_social_listen(on_social, NULL);
    backend_social_open();
    const int queued = g_listening && backend_social_send(f);
    cJSON_Delete(f);
    if (queued) {
        S.busy = 1;
        S.timer = html_timer_add(PAINT_WAIT_MS, paint_expired, NULL);
    } else {
        snprintf(S.error, sizeof(S.error), "%s", I18N("kn.paint.failed", "Failed to apply paint"));
    }
    paint_render();
}

// ---------------------------------------------------------------- open / handlers

int paint_picker_can_open(const int ind) {
    if (!active_account()) return 0; // (krunker.io too: its setPaint goes to krunker.io, apply)
    const cJSON *j = store_json(ind);
    const cJSON *t = j ? cJSON_GetObjectItemCaseSensitive(j, "paintTargets") : NULL; // true or a list of meshes
    return t && !cJSON_IsNull(t) && !cJSON_IsFalse(t);
}

// v10's initial pick (deob-10.0.7.js:108551-108571): the paint of the copy's initialShader, else of its initialPaint
// (colour, case-insensitive), else the first owned paint, else the first paint; -1 when the store has none.
static int initial_pick(const int initial_shader) {
    int n = 0;
    PaintRow *rows = store_paints(&n);
    int pick = -1;
    for (int i = 0; i < n && pick < 0 && initial_shader >= 0; i++)
        if (rows[i].has_shader && rows[i].shader == initial_shader) pick = rows[i].index;
    for (int i = 0; i < n && pick < 0 && S.initial_paint[0]; i++)
        if (rows[i].color[0] && !strcasecmp(rows[i].color, S.initial_paint)) pick = rows[i].index;
    for (int i = 0; i < n && pick < 0; i++)
        if (rows[i].count > 0) pick = rows[i].index;
    if (pick < 0 && n > 0) pick = rows[0].index;
    free(rows);
    return pick;
}

void paint_picker_open(const int ind, const int sid) {
    if (S.busy || !paint_picker_can_open(ind)) return;
    S.ind = ind;
    S.sid = sid;
    S.error[0] = 0;
    S.scroll = 0.0f;
    // the copy's m: initialPaint (m.paint || null), initialShader (a numeric m.paintShader), initialWear (a numeric
    // m.wear) as the inventory passes them (deob-10.0.7.js:147679-147686), and its seed for the preview (:108516)
    cJSON *m = copy_meta();
    const char *paint = json_str(m, "paint");
    snprintf(S.initial_paint, sizeof(S.initial_paint), "%s", paint ? paint : "");
    const cJSON *shader = cJSON_GetObjectItemCaseSensitive(m, "paintShader"), *wear = cJSON_GetObjectItemCaseSensitive(m, "wear");
    const cJSON *seed = cJSON_GetObjectItemCaseSensitive(m, "seed");
    S.shader = cJSON_IsNumber(shader) ? shader->valueint : -1;
    S.has_wear = cJSON_IsNumber(wear);
    S.wear = S.has_wear ? wear->valuedouble : 0.0;
    S.seed = m ? skin_copy_seed(S.sid, cJSON_IsNumber(seed), cJSON_IsNumber(seed) ? seed->valuedouble : 0.0) : 0;
    cJSON_Delete(m);
    S.selected = initial_pick(S.shader);
    S.open = 1;
    v10_view_push(paint_html(), paint_render, paint_closed, NULL);
    slider_sync();
}

int paint_picker_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    if (!name) return -1;
    if (!strcmp(name, "paintOpen")) {
        paint_picker_open(arg_int(args, argc, 0, -1), arg_int(args, argc, 1, 0));
        return 1;
    }
    if (!strcmp(name, "paintPick")) {
        // selectPaint (deob-10.0.7.js:108607-108611): the pick, and the preview's shader state becomes the pick's shader
        // (null for a colour paint) when it differs; a locked paint is picked too (preview only)
        int n = 0;
        PaintRow *rows = S.open && !S.busy ? store_paints(&n) : NULL;
        const PaintRow *row = find_row(rows, n, arg_int(args, argc, 0, -1));
        if (row) {
            S.selected = row->index;
            S.shader = row->has_shader ? row->shader : -1;
            paint_render();
        }
        free(rows);
        return 1;
    }
    if (!strcmp(name, "paintScroll")) {
        // scrollSwatches(dir): 0.8 * clientWidth per arrow (deob-10.0.7.js:108580-108589; natively without the smooth scroll)
        const HtmlElem row = S.open ? swatch_row() : 0;
        HtmlScroll sc;
        if (row && html_dom_get_scroll(row, &sc)) slider_scroll_by((float) arg_int(args, argc, 0, 0) * 0.8f * sc.client_w);
        return 1;
    }
    if (!strcmp(name, "paintWheel")) {
        // the row's wheel: preventDefault, scrollLeft += deltaY (:108590-108592); an inline onwheel's key is deltaY
        if (!S.open || !ev) return 1;
        slider_scroll_by((float) ev->key);
        return 0;
    }
    if (!strcmp(name, "paintApply")) {
        apply();
        return 1;
    }
    return -1;
}
