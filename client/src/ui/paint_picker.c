// The paint picker of an owned skin card (client/include/paint_picker.h): Krunker v10.0.3's paint popup
// (deob-10.0.3.js:121985-122180) on KrunkNative's brain. The popup lists the paints the account owns from its
// owned list (data slot 10) and applies the picked one as v10's setPaint (deob:122139-122142): the socket frame
// ["ip", skinId, item, paint, colour, shader, uid], answered by ["ipr", {copy_id, item_id, paint_item_id, m,
// count} | null, uid] or, for a refusal, ["po", null, message] (services/brain/economy.c sh_item_paint; the same
// work as the HTTP route POST /items/<id>/paint, doc/v10/paint-copy.md). The list is the account's own data, so the
// popup has no load to fail: an account without paints gets v10's empty state. A paint that was applied sets the
// copy's paint on the owned list at once (account_paint_applied); the brain pushes the account again ("ua") after.
#include <paint_picker.h>
#include <backend.h>
#include <backend_ui.h>
#include <ui_logic.h>
#include <v10_ui.h>
#include <i18n.h>
#include "../viewer/viewer_store.h"
#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define PAINT_TYPE 18        // a store paint (v10: type 18, deob:122029)
#define COPY_ID_BASE 1000000 // the brain's V10_COPY_ID_BASE: a copy's id; below it a stack's sid is its store index
#define PAINT_WAIT_MS 15000  // no answer by then: the popup says the paint failed (the market's wait is the same)

// One owned paint: the store paint of an index with the account's count of it (owned entries cnt ?? 1).
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
    int pending_paint; // the paint of the request in flight (its toast and owned-list update)
    int uid;           // the request in flight's uid (the brain echoes it in "ipr")
    int timer;         // the wait's deadline (html_timer), 0 none
    char error[160];
} S = {.selected = -1, .pending_paint = -1};

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

// Sorted as v10 sorts the paints (deob:122041): owned first, then by index.
static int row_cmp(const void *pa, const void *pb) {
    const PaintRow *a = pa, *b = pb;
    const int oa = a->count > 0, ob = b->count > 0;
    if (oa != ob) return ob - oa;
    return a->index - b->index;
}

// The owned paints (count > 0), sorted. *n is their number; the caller frees the array (NULL when none).
static PaintRow *owned_paints(int *n) {
    *n = 0;
    const cJSON *owned = owned_list();
    if (!owned) return NULL;
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
    int owned_n = 0;
    for (int i = 0; i < total; i++) owned_n += all[i].count > 0;
    PaintRow *out = owned_n ? calloc((size_t) owned_n, sizeof(*out)) : NULL;
    if (out) {
        for (int i = 0; i < total; i++) if (all[i].count > 0) out[(*n)++] = all[i];
    }
    free(all);
    return out;
}

static const PaintRow *find_row(const PaintRow *rows, const int n, const int index) {
    for (int i = 0; rows && i < n; i++) if (rows[i].index == index) return &rows[i];
    return NULL;
}

// The paint the card's copy wears (its m: a paint colour or a shader number), by store index; -1 none.
static int copy_paint(const PaintRow *rows, const int n) {
    if (S.sid < COPY_ID_BASE) return -1;
    const cJSON *e = owned_copy(owned_list(), S.sid);
    const cJSON *m = e ? cJSON_GetObjectItemCaseSensitive(e, "m") : NULL;
    if (!m) return -1;
    const cJSON *shader = cJSON_GetObjectItemCaseSensitive(m, "paintShader");
    const char *colour = json_str(m, "paint");
    for (int i = 0; rows && i < n; i++) {
        if (cJSON_IsNumber(shader) && rows[i].has_shader && rows[i].shader == shader->valueint) return rows[i].index;
        if (colour && rows[i].color[0] && !strcasecmp(rows[i].color, colour)) return rows[i].index;
    }
    return -1;
}

// A colour as the swatch's style: only "#" and hex digits survive.
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

static char *paint_html(void) {
    int n = 0;
    PaintRow *rows = owned_paints(&n);
    StrBuf sb = {0};
    char label[160];
    const char *item = item_name(S.ind);
    // v10's popup wrapper (popup-container > popup, as v10_io_error and the cancel popups): centred over the page
    sb_append(&sb, "<div class=\"popup-container svelte-1wz7ymp\"><div class=\"popup svelte-1wz7ymp\" style=\"width: 700px;\">"
                   "<div class=\"paint-container io-popup-container svelte-10qrqvt\"><div class=\"paint-content io-popup-content svelte-10qrqvt\">"
                   "<div class=\"paint-header svelte-10qrqvt\"><div class=\"item-name svelte-10qrqvt\">");
    sb_append_esc(&sb, item);
    sb_append(&sb, "</div><div class=\"item-sub svelte-10qrqvt\">");
    sb_append_esc(&sb, I18N("kn.paint.sub", "Pick a paint, then press Paint to apply it."));
    sb_append(&sb, "</div></div><div class=\"paint-controls svelte-10qrqvt\">");
    if (n == 0) {
        sb_append(&sb, "<div class=\"paint-empty svelte-10qrqvt\">");
        sb_append_esc(&sb, I18N("kn.paint.empty", "You don’t own any paints yet. Earn paints from rewards or the market to start painting items."));
        sb_append(&sb, "</div>");
    } else {
        sb_append(&sb, "<div class=\"swatch-slider svelte-10qrqvt\"><div class=\"swatch-row svelte-10qrqvt\" role=\"radiogroup\" aria-label=\"Available paints\">");
        for (int i = 0; i < n; i++) {
            const PaintRow *r = &rows[i];
            const int on = r->index == S.selected;
            sb_appendf(&sb, "<button type=\"button\" class=\"swatch%s svelte-10qrqvt\" role=\"radio\" aria-checked=\"%s\" onclick=\"paintPick(%d)\">",
                       on ? " selected" : "", on ? "true" : "false", r->index);
            if (r->color[0]) {
                char colour[24];
                swatch_colour(colour, sizeof(colour), r->color);
                sb_appendf(&sb, "<span class=\"swatch-fill svelte-10qrqvt\" style=\"background-color:%s\"></span>", colour);
            } else {
                sb_append(&sb, "<span class=\"swatch-fill svelte-10qrqvt\"><i class=\"material-icons\">format_paint</i></span>");
            }
            sb_appendf(&sb, "<span class=\"swatch-count svelte-10qrqvt\">x%d</span>", r->count);
            sb_append(&sb, "<span class=\"swatch-name svelte-10qrqvt\">");
            sb_append_esc(&sb, r->name);
            sb_append(&sb, "</span></button>");
        }
        sb_append(&sb, "</div></div>");
    }
    if (S.error[0]) {
        sb_append(&sb, "<div class=\"paint-error svelte-10qrqvt\">");
        sb_append_esc(&sb, S.error);
        sb_append(&sb, "</div>");
    }
    const PaintRow *sel = find_row(rows, n, S.selected);
    sb_append(&sb, "<div class=\"paint-actions svelte-10qrqvt\"><button class=\"btn btn-cancel svelte-10qrqvt\" onclick=\"knViewPop(0)\">");
    sb_append_esc(&sb, I18N("generic.cancel", "Cancel"));
    sb_append(&sb, "</button>");
    if (n > 0) {
        if (S.busy) snprintf(label, sizeof(label), "%s", I18N("kn.paint.applying", "Painting…"));
        else I18N_F(label, "kn.paint.apply", "Paint ({0})", sel ? sel->name : "");
        sb_appendf(&sb, "<button class=\"btn btn-apply svelte-10qrqvt\" onclick=\"paintApply()\"%s>",
                   S.busy || !sel ? " disabled" : "");
        sb_append_esc(&sb, label);
        sb_append(&sb, "</button>");
    }
    sb_append(&sb, "</div></div></div></div></div></div>");
    free(rows);
    return sb_finish(&sb);
}

// Re-renders the open popup (the top view) when it is still open.
static void paint_render(void) {
    if (!S.open) return;
    v10_view_set(paint_html());
}

static void paint_closed(const int accepted, void *user) {
    (void) accepted;
    (void) user;
    S.open = 0;
    S.error[0] = 0;
}

// ---------------------------------------------------------------- apply

// The brain's answer to the paint in flight: `data` is the "ipr" payload (NULL: refused, no text), `message` the
// "po" text (NULL: none). On success the paint's unit is taken, the copy gets its paint, and when the copy is new one
// unpainted unit of the item becomes it (from_stack); the popup closes with the toast. The brain pushes the account
// again after ("ua"); v10 takes one paint unit locally and the copy comes from the reload (deob:122152-122170).
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
    const char *paint_name = "";
    int n = 0;
    PaintRow *rows = owned_paints(&n);
    const PaintRow *row = find_row(rows, n, paint);
    if (row) paint_name = row->name;
    char text[256];
    I18N_F(text, "kn.paint.applied", "Painted {0} with {1}", item_name(item), paint_name);
    StrBuf sb = {0};
    sb_append_esc(&sb, text);
    char *toast = sb_finish(&sb);
    ui_notify(toast);
    free(toast);
    free(rows);
    account_paint_applied(item, paint, copy->valueint, S.sid < COPY_ID_BASE, cJSON_GetObjectItemCaseSensitive(data, "m"));
    ui_loadout_owned_changed();
    if (S.open) v10_view_pop(1); // accepted: v10's push resolves with the result and the popup closes (deob:122190)
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

static void apply(void) {
    if (!S.open || S.busy || S.selected < 0) return;
    int n = 0;
    PaintRow *rows = owned_paints(&n);
    const PaintRow *row = find_row(rows, n, S.selected);
    if (!row || row->count <= 0) {
        free(rows);
        snprintf(S.error, sizeof(S.error), "%s", I18N("kn.paint.none", "You don’t have any of that paint left."));
        paint_render();
        return;
    }
    // v10's items.setPaint(skinId, {item_id, paint_item_id}) (deob:122139-122142) as the socket frame
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
    if (account_backend() == ACCOUNT_BACKEND_KRUNKER || !active_account()) return 0;
    const cJSON *j = store_json(ind);
    const cJSON *t = j ? cJSON_GetObjectItemCaseSensitive(j, "paintTargets") : NULL; // true or a list of meshes
    return t && !cJSON_IsNull(t) && !cJSON_IsFalse(t);
}

// The copy's paint, else the first owned paint (v10's initialShader / initialPaint, then the first, deob:122054-122064).
static int initial_paint(void) {
    int n = 0;
    PaintRow *rows = owned_paints(&n);
    const int copy = copy_paint(rows, n);
    const int pick = copy >= 0 ? copy : (n > 0 ? rows[0].index : -1);
    free(rows);
    return pick;
}

void paint_picker_open(const int ind, const int sid) {
    if (S.busy || !paint_picker_can_open(ind)) return;
    S.ind = ind;
    S.sid = sid;
    S.error[0] = 0;
    S.selected = initial_paint();
    S.open = 1;
    v10_view_push(paint_html(), paint_render, paint_closed, NULL);
}

int paint_picker_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) ev;
    if (!name) return -1;
    if (!strcmp(name, "paintOpen")) {
        paint_picker_open(arg_int(args, argc, 0, -1), arg_int(args, argc, 1, 0));
        return 1;
    }
    if (!strcmp(name, "paintPick")) {
        if (S.open && !S.busy) {
            S.selected = arg_int(args, argc, 0, -1);
            S.error[0] = 0;
            paint_render();
        }
        return 1;
    }
    if (!strcmp(name, "paintApply")) {
        apply();
        return 1;
    }
    return -1;
}
